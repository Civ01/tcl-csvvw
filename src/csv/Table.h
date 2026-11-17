// Loaded CSV table: decoded UTF-8 buffer + row index + column statistics
// (SPECS §5, §7). Immutable after load except for the header toggle.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Parser.h"
#include "ValueParse.h"
#include "core/TextDecoder.h"

namespace csvvw::csv {

constexpr size_t kMaxColumns = 16384;
constexpr size_t kLongestPerColumn = 8;

enum class HeaderMode { Auto, On, Off };
enum class HeaderDetect { Yes, No, Inconclusive };

const char* delimiterKey(char d);   // "," ";" "tab" "pipe" (0 -> "auto")
bool parseDelimiterKey(std::string_view s, char& out);  // auto -> 0
const char* delimiterLabel(char d);  // "," ";" "Tab" "|"

struct LoadOptions {
    EncodingChoice encoding = EncodingChoice::Auto;
    char delimiter = 0;  // 0 = auto-detect
    HeaderMode headerMode = HeaderMode::Auto;
    bool headerDefault = true;  // used when detection is inconclusive
    bool preferTab = false;     // .tsv / .tab: Tab wins delimiter ties
    int typeThreshold = 90;     // % of non-empty cells that make a column number/date (sorting, alignment)
    int headerThreshold = 75;   // same, for header detection only
};

struct ColumnStats {
    uint32_t nonEmpty = 0, numbers = 0, dates = 0;  // over data rows (row 0 excluded)
    uint32_t commaDecimalVotes = 0, commaThousandsVotes = 0;
    ValueKind row0Kind = ValueKind::Empty;
    // Up to kLongestPerColumn (byte length, file row) of the longest cells (row 0 excluded).
    std::vector<std::pair<uint32_t, uint32_t>> longest;
    uint32_t row0Length = 0;
    // Length distribution of non-empty cells in characters (row 0 excluded):
    // lenHist[n] for n < kLenBuckets-1, the last bucket counts longer cells.
    // lenExample[n] = first file row with that length (UINT32_MAX = none).
    static constexpr size_t kLenBuckets = 129;
    std::vector<uint32_t> lenHist, lenExample;
    uint32_t row0Chars = 0;  // 0 = empty
};

// Result of Table::typicalLength().
struct TypicalLength {
    bool empty = true;           // no non-empty cells
    bool beyondHistogram = false;  // the percentile falls into the "longer" bucket
    uint32_t chars = 0;          // length (characters) at the percentile
    std::vector<uint32_t> rows;  // example file rows with about that length (for measuring)
};

class Table {
public:
    // --- source ---
    std::string data;  // UTF-8, BOM removed
    Encoding encoding = Encoding::Utf8;
    char delimiter = ',';
    bool delimiterDetected = true;
    std::vector<uint32_t> rowStarts;  // file rows + sentinel
    size_t columns = 0;               // widest row (ragged rows are padded)
    bool columnsClamped = false;      // a row had more than kMaxColumns fields

    // --- per row (file rows): own line count of the cells (capped at 255) ---
    std::vector<uint8_t> rowLines;       // max over the row's cells
    std::vector<uint8_t> lastCellLines;  // of the last column's cell

    // --- columns ---
    std::vector<ColumnStats> stats;
    HeaderDetect headerDetect = HeaderDetect::Inconclusive;
    bool header = false;
    int typeThreshold = 90;  // percent, see LoadOptions

    size_t fileRows() const { return rowStarts.empty() ? 0 : rowStarts.size() - 1; }
    size_t headerRows() const { return header && fileRows() > 0 ? 1 : 0; }
    size_t dataRows() const { return fileRows() - headerRows(); }
    size_t fileRowOfData(size_t d) const { return d + headerRows(); }

    void fields(size_t fileRow, std::vector<Field>& out) const;
    // Unescaped cell value; "" for padded cells.
    std::string cell(size_t fileRow, size_t col) const;
    std::string_view rawRow(size_t fileRow) const;

    // Column type for sorting / alignment, over data rows.
    ValueKind columnType(size_t col) const;
    CommaMode commaMode(size_t col) const;

    // Cell length at the given percentile of the column's non-empty cells
    // (data rows; row 0 too when there is no header).
    TypicalLength typicalLength(size_t col, int percentile) const;

    // Spreadsheet-style column name: A, B, ..., Z, AA, ...
    static std::string columnLetters(size_t col);
    // Header caption (row 0 cell when header is on, else letters).
    std::string caption(size_t col) const;
};

// Typed (number/date) or Text for a column's statistics at `thresholdPct`.
ValueKind typedKind(uint32_t nonEmpty, uint32_t numbers, uint32_t dates, int thresholdPct);
HeaderDetect detectHeader(const Table& t, int thresholdPct = 75);
char sniffDelimiter(std::string_view data, bool preferTab);

// Decodes, sniffs, indexes and computes statistics. Never throws on bad
// input (std::bad_alloc aside).
std::unique_ptr<Table> loadTable(std::string&& bytes, const LoadOptions& opt, const AnsiConverter& ansi = {});

}  // namespace csvvw::csv
