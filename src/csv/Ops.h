// Table operations: sorting, searching and clipboard serialisation
// (SPECS §8.4, §9, §11). Locale-aware pieces are injected as callbacks so the
// logic stays portable and testable.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Table.h"

namespace csvvw::csv {

// ------------------------------------------------------------------ sorting
enum class SortDir { None, Asc, Desc };
enum class SortAs { Auto, Text, Number, Date };

const char* sortAsName(SortAs s);

// Binary sort key of a text value; keys are compared with memcmp order.
// Windows: LCMapStringEx(LCMAP_SORTKEY | LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS).
using SortKeyFn = std::function<std::string(std::u16string_view)>;
// Portable fallback: ASCII case-insensitive with natural digit order.
std::string defaultSortKey(std::u16string_view s);

// Effective type used when sorting `col` as `as`.
ValueKind effectiveSortType(const Table& t, size_t col, SortAs as);

// Returns the display order of data rows (indices 0..dataRows-1). Stable;
// empty cells always last; in Number/Date mode values that do not parse sort
// after the parsed ones, compared as text. SortDir::None = original order.
std::vector<uint32_t> sortRows(const Table& t, size_t col, SortDir dir, SortAs as, const SortKeyFn& key);

// ------------------------------------------------------------------ search
// Finds `needle` in `hay`: forward = first match starting at >= from,
// backward = last match starting before `from` (from may be hay.size()).
using Matcher = std::function<bool(std::u16string_view hay, size_t from, bool backward, size_t& pos, size_t& len)>;
// Portable matcher: ASCII case folding; whole words = not adjacent to letters/digits/_.
Matcher defaultMatcher(std::u16string needle, bool matchCase, bool wholeWords);

struct SearchHit {
    long row = 0;     // display row; -1 = header row
    size_t col = 0;
    size_t pos = 0;   // match in the full (uncapped) cell text, UTF-16 units
    size_t len = 0;
};

struct SearchStart {
    long row = 0;      // display row of the starting cell (-1 = header)
    size_t col = 0;
    size_t offset = 0;  // forward: search this cell from offset; backward: before offset
    bool includeCell = true;  // false: start with the next (forward) / previous (backward) cell
};

// Scans cells in display order (header row first when present), row by row,
// left to right. No wrap-around.
std::optional<SearchHit> searchTable(const Table& t, const std::vector<uint32_t>& order, const SearchStart& start,
                                     bool backward, const Matcher& match);

// ------------------------------------------------------------------ clipboard
struct CellRange {
    size_t r0 = 0, r1 = 0;  // display rows, inclusive
    size_t c0 = 0, c1 = 0;  // columns, inclusive
};

enum class CopyFormat { Tsv, Csv };

// TSV: cells joined with Tab, rows with CRLF; cells containing Tab, CR, LF or
// '"' are quoted Excel-style. A single cell (TSV, no header) is copied raw.
// CSV: the table's delimiter with RFC 4180 quoting where needed.
std::string copyRange(const Table& t, const std::vector<uint32_t>& order, const CellRange& r, CopyFormat fmt,
                      bool withHeader);

std::string quoteIfNeeded(std::string_view v, char delimiter);

}  // namespace csvvw::csv
