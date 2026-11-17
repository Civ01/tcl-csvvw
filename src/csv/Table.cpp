#include "Table.h"

#include <algorithm>
#include <map>

#include "core/TextUtil.h"

namespace csvvw::csv {

const char* delimiterKey(char d) {
    switch (d) {
        case 0: return "auto";
        case ',': return ",";
        case ';': return ";";
        case '\t': return "tab";
        case '|': return "pipe";
    }
    return "auto";
}

bool parseDelimiterKey(std::string_view s, char& out) {
    s = trim(s);
    if (iequals(s, "auto") || s.empty()) { out = 0; return true; }
    if (s == ",") { out = ','; return true; }
    if (s == ";") { out = ';'; return true; }
    if (iequals(s, "tab") || s == "\\t") { out = '\t'; return true; }
    if (iequals(s, "pipe") || s == "|") { out = '|'; return true; }
    return false;
}

const char* delimiterLabel(char d) {
    switch (d) {
        case ',': return ",";
        case ';': return ";";
        case '\t': return "Tab";
        case '|': return "|";
    }
    return "?";
}

// ------------------------------------------------------------------ Table

void Table::fields(size_t fileRow, std::vector<Field>& out) const {
    splitRow(data, rowStarts[fileRow], rowStarts[fileRow + 1], delimiter, out, kMaxColumns);
}

std::string Table::cell(size_t fileRow, size_t col) const {
    if (fileRow >= fileRows()) return {};
    std::vector<Field> f;
    fields(fileRow, f);
    if (col >= f.size()) return {};
    return fieldValue(data, f[col]);
}

std::string_view Table::rawRow(size_t fileRow) const {
    return std::string_view(data).substr(rowStarts[fileRow], rowStarts[fileRow + 1] - rowStarts[fileRow]);
}

ValueKind Table::columnType(size_t col) const {
    if (col >= stats.size()) return ValueKind::Text;
    const auto& s = stats[col];
    uint32_t nonEmpty = s.nonEmpty, numbers = s.numbers, dates = s.dates;
    if (!header && fileRows() > 0) {
        if (s.row0Kind != ValueKind::Empty) ++nonEmpty;
        if (s.row0Kind == ValueKind::Number) ++numbers;
        if (s.row0Kind == ValueKind::Date) ++dates;
    }
    return typedKind(nonEmpty, numbers, dates, typeThreshold);
}

ValueKind typedKind(uint32_t nonEmpty, uint32_t numbers, uint32_t dates, int pct) {
    if (nonEmpty == 0) return ValueKind::Text;
    const uint64_t need = static_cast<uint64_t>(nonEmpty) * static_cast<uint64_t>(pct);
    if (numbers * 100ull >= need) return ValueKind::Number;
    if (dates * 100ull >= need) return ValueKind::Date;
    return ValueKind::Text;
}

CommaMode Table::commaMode(size_t col) const {
    if (col >= stats.size()) return CommaMode::Thousands;
    return stats[col].commaDecimalVotes > stats[col].commaThousandsVotes ? CommaMode::Decimal : CommaMode::Thousands;
}

std::string Table::columnLetters(size_t col) {
    std::string s;
    size_t n = col + 1;
    while (n > 0) {
        size_t r = (n - 1) % 26;
        s.insert(s.begin(), static_cast<char>('A' + r));
        n = (n - 1) / 26;
    }
    return s;
}

std::string Table::caption(size_t col) const {
    if (header && fileRows() > 0) return cell(0, col);
    return columnLetters(col);
}

// ------------------------------------------------------------------ sniffing

char sniffDelimiter(std::string_view data, bool preferTab) {
    constexpr size_t kSample = 64 * 1024;
    constexpr size_t kMaxLines = 200;
    std::string_view sample = data.substr(0, kSample);
    if (data.size() > kSample) {  // whole lines only
        size_t cut = sample.find_last_of("\r\n");
        if (cut != std::string_view::npos && cut > 0) sample = sample.substr(0, cut + 1);
    }
    const char order[] = {',', ';', '\t', '|'};
    char best = ',';
    double bestScore = 0;
    std::vector<Field> f;
    for (char d : preferTab ? std::array<char, 4>{'\t', ',', ';', '|'}
                            : std::array<char, 4>{order[0], order[1], order[2], order[3]}) {
        auto starts = scanRows(sample, d);
        size_t rows = std::min(starts.size() - 1, kMaxLines);
        if (rows == 0) continue;
        std::map<size_t, size_t> hist;
        for (size_t r = 0; r < rows; ++r) {
            splitRow(sample, starts[r], starts[r + 1], d, f);
            if (f.size() == 1 && f[0].length == 0) continue;  // blank line
            ++hist[f.size()];
        }
        size_t total = 0, modeCount = 0, mode = 1;
        for (auto& [cnt, n] : hist) {
            total += n;
            if (n > modeCount || (n == modeCount && cnt > mode)) {
                modeCount = n;
                mode = cnt;
            }
        }
        if (total == 0 || mode <= 1) continue;
        double score = static_cast<double>(modeCount) / static_cast<double>(total);
        // Slightly favour more columns among equally consistent candidates.
        score += std::min<double>(static_cast<double>(mode), 1000.0) * 1e-6;
        if (score > bestScore + 1e-9) {
            bestScore = score;
            best = d;
        }
    }
    return best;
}

HeaderDetect detectHeader(const Table& t, int pct) {
    if (t.fileRows() < 2 || t.stats.empty()) return HeaderDetect::Inconclusive;
    bool anyRow0 = false, allRow0Text = true, typedMismatch = false;
    for (const auto& s : t.stats) {
        if (s.row0Kind != ValueKind::Empty) anyRow0 = true;
        if (s.row0Kind == ValueKind::Number || s.row0Kind == ValueKind::Date) allRow0Text = false;
        if (s.nonEmpty == 0) continue;
        ValueKind typed = typedKind(s.nonEmpty, s.numbers, s.dates, pct);
        if (typed == ValueKind::Text) continue;
        if (s.row0Kind == typed) return HeaderDetect::No;  // row 0 looks like data
        if (s.row0Kind != ValueKind::Empty) typedMismatch = true;
    }
    if (anyRow0 && allRow0Text && typedMismatch) return HeaderDetect::Yes;
    return HeaderDetect::Inconclusive;
}

// ------------------------------------------------------------------ loading

namespace {

uint8_t ownLineCount(std::string_view raw) {
    if (raw.empty() || raw[0] != '"') return 1;  // only quoted fields contain breaks
    unsigned n = 1;
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\r') {
            ++n;
            if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
        } else if (c == '\n') {
            ++n;
        }
    }
    return static_cast<uint8_t>(std::min(n, 255u));
}

void noteLongest(ColumnStats& s, uint32_t len, uint32_t row) {
    if (len == 0) return;
    if (s.longest.size() < kLongestPerColumn) {
        s.longest.emplace_back(len, row);
        return;
    }
    auto it = std::min_element(s.longest.begin(), s.longest.end());
    if (len > it->first) *it = {len, row};
}

// Fast reject for obvious text so the classification pass stays cheap.
bool mayBeNumberOrDate(std::string_view v) {
    for (char c : v) {
        if (c == ' ' || c == '\t') continue;
        return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == ',' ||
               static_cast<unsigned char>(c) == 0xC2;
    }
    return false;
}

}  // namespace

std::unique_ptr<Table> loadTable(std::string&& bytes, const LoadOptions& opt, const AnsiConverter& ansi) {
    auto t = std::make_unique<Table>();
    auto dec = decodeToUtf8(std::move(bytes), opt.encoding, ansi);
    t->data = std::move(dec.text);
    t->encoding = dec.encoding;
    if (t->data.size() > 0xFFFFFFF0u) t->data.resize(0xFFFFFFF0u);  // offsets are 32-bit

    t->delimiterDetected = opt.delimiter == 0;
    t->delimiter = opt.delimiter ? opt.delimiter : sniffDelimiter(t->data, opt.preferTab);
    t->rowStarts = scanRows(t->data, t->delimiter);
    const size_t rows = t->fileRows();

    t->rowLines.assign(rows, 1);
    t->lastCellLines.assign(rows, 1);
    std::vector<Field> f;
    std::vector<uint8_t> rowLastLines(rows, 1);
    std::vector<uint32_t> fieldCount(rows, 0);
    const std::string_view data(t->data);
    std::string unq;
    for (size_t r = 0; r < rows; ++r) {
        splitRow(data, t->rowStarts[r], t->rowStarts[r + 1], t->delimiter, f, kMaxColumns + 1);
        if (f.size() > kMaxColumns) {  // last column absorbs the rest of the row
            t->columnsClamped = true;
            splitRow(data, t->rowStarts[r], t->rowStarts[r + 1], t->delimiter, f, kMaxColumns);
        }
        fieldCount[r] = static_cast<uint32_t>(f.size());
        if (f.size() > t->columns) {
            t->columns = f.size();
            t->stats.resize(t->columns);
        }
        uint8_t maxLines = 1;
        for (size_t c = 0; c < f.size(); ++c) {
            std::string_view raw = rawField(data, f[c]);
            uint8_t lines = ownLineCount(raw);
            maxLines = std::max(maxLines, lines);
            std::string_view val = raw;
            if (!raw.empty() && raw[0] == '"') {
                unq = unescape(raw);
                val = unq;
            }
            ColumnStats& s = t->stats[c];
            ValueKind kind = ValueKind::Empty;
            CommaHint hint = CommaHint::None;
            if (!isBlank(val)) kind = mayBeNumberOrDate(val) ? classify(val, CommaMode::Thousands, nullptr, &hint)
                                                              : ValueKind::Text;
            if (r == 0) {
                s.row0Kind = kind;
                s.row0Length = f[c].length;
                continue;
            }
            if (kind != ValueKind::Empty) ++s.nonEmpty;
            if (kind == ValueKind::Number) ++s.numbers;
            if (kind == ValueKind::Date) ++s.dates;
            if (hint == CommaHint::Decimal) ++s.commaDecimalVotes;
            if (hint == CommaHint::Thousands) ++s.commaThousandsVotes;
            noteLongest(s, f[c].length, static_cast<uint32_t>(r));
        }
        t->rowLines[r] = maxLines;
        rowLastLines[r] = f.empty() ? 1 : ownLineCount(rawField(data, f.back()));
    }
    // The last column's cell is the row's last field only for full-width rows.
    for (size_t r = 0; r < rows; ++r)
        t->lastCellLines[r] = fieldCount[r] == t->columns ? rowLastLines[r] : 1;

    t->typeThreshold = std::clamp(opt.typeThreshold, 1, 100);
    t->headerDetect = detectHeader(*t, std::clamp(opt.headerThreshold, 1, 100));
    switch (opt.headerMode) {
        case HeaderMode::On: t->header = true; break;
        case HeaderMode::Off: t->header = false; break;
        case HeaderMode::Auto:
            t->header = t->headerDetect == HeaderDetect::Yes  ? true
                        : t->headerDetect == HeaderDetect::No ? false
                                                              : opt.headerDefault;
            break;
    }
    if (rows < 2) t->header = rows == 1 && t->header && opt.headerMode == HeaderMode::On;
    return t;
}

}  // namespace csvvw::csv
