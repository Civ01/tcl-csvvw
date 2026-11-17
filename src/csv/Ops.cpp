#include "Ops.h"

#include <algorithm>
#include <climits>
#include <cstring>

#include "core/TextUtil.h"

namespace csvvw::csv {

// ------------------------------------------------------------------ sorting

const char* sortAsName(SortAs s) {
    switch (s) {
        case SortAs::Auto: return "auto";
        case SortAs::Text: return "text";
        case SortAs::Number: return "number";
        case SortAs::Date: return "date";
    }
    return "auto";
}

std::string defaultSortKey(std::u16string_view s) {
    std::string k;
    k.reserve(s.size() * 3);
    size_t i = 0;
    while (i < s.size()) {
        char16_t c = s[i];
        if (c >= u'0' && c <= u'9') {
            size_t j = i;
            while (j < s.size() && s[j] >= u'0' && s[j] <= u'9') ++j;
            size_t b = i;
            while (b + 1 < j && s[b] == u'0') ++b;  // strip leading zeros
            size_t len = std::min<size_t>(j - b, 250);
            k.push_back('\x01');
            k.push_back(static_cast<char>(len));
            for (size_t q = b; q < b + len; ++q) k.push_back(static_cast<char>(s[q]));
            i = j;
            continue;
        }
        if (c >= u'A' && c <= u'Z') c = static_cast<char16_t>(c - u'A' + u'a');
        k.push_back('\x02');
        k.push_back(static_cast<char>(c >> 8));
        k.push_back(static_cast<char>(c & 0xFF));
        ++i;
    }
    return k;
}

ValueKind effectiveSortType(const Table& t, size_t col, SortAs as) {
    switch (as) {
        case SortAs::Text: return ValueKind::Text;
        case SortAs::Number: return ValueKind::Number;
        case SortAs::Date: return ValueKind::Date;
        case SortAs::Auto: break;
    }
    return t.columnType(col);
}

std::vector<uint32_t> sortRows(const Table& t, size_t col, SortDir dir, SortAs as, const SortKeyFn& keyFn) {
    const size_t n = t.dataRows();
    std::vector<uint32_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = static_cast<uint32_t>(i);
    if (dir == SortDir::None || n < 2) return order;

    const ValueKind type = effectiveSortType(t, col, as);
    const CommaMode comma = t.commaMode(col);
    struct Key {
        uint8_t group;  // 0 parsed / text, 1 unparsed text (typed sort), 2 empty
        double num;
        uint32_t text;  // index into texts
    };
    std::vector<Key> keys(n);
    std::vector<std::string> texts;
    texts.reserve(n);
    std::vector<Field> f;
    const std::string_view data(t.data);
    const SortKeyFn& kf = keyFn ? keyFn : SortKeyFn(defaultSortKey);
    for (size_t d = 0; d < n; ++d) {
        size_t row = t.fileRowOfData(d);
        t.fields(row, f);
        std::string val = col < f.size() ? fieldValue(data, f[col]) : std::string();
        Key& k = keys[d];
        k.num = 0;
        k.text = 0;
        if (isBlank(val)) {
            k.group = 2;
            continue;
        }
        if (type == ValueKind::Number || type == ValueKind::Date) {
            double v = 0;
            bool ok = type == ValueKind::Number ? parseNumber(val, comma, v) : parseDate(val, v);
            if (ok) {
                k.group = 0;
                k.num = v;
                continue;
            }
            k.group = 1;
        } else {
            k.group = 0;
        }
        k.text = static_cast<uint32_t>(texts.size());
        texts.push_back(kf(utf8ToUtf16(val)));
    }
    const bool desc = dir == SortDir::Desc;
    const bool numeric = type == ValueKind::Number || type == ValueKind::Date;
    auto cmpText = [&](uint32_t a, uint32_t b) {
        const std::string& x = texts[a];
        const std::string& y = texts[b];
        size_t m = std::min(x.size(), y.size());
        int c = m ? std::memcmp(x.data(), y.data(), m) : 0;
        if (c != 0) return c;
        return x.size() < y.size() ? -1 : x.size() > y.size() ? 1 : 0;
    };
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const Key& x = keys[a];
        const Key& y = keys[b];
        if (x.group != y.group) return x.group < y.group;
        if (x.group == 2) return false;
        int c;
        if (x.group == 0 && numeric) c = x.num < y.num ? -1 : x.num > y.num ? 1 : 0;
        else c = cmpText(x.text, y.text);
        return desc ? c > 0 : c < 0;
    });
    return order;
}

// ------------------------------------------------------------------ search

namespace {

bool isWordChar(char16_t c) {
    return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || c == u'_' ||
           (c >= 0xC0 && c != 0xD7 && c != 0xF7 && !(c >= 0x2000 && c <= 0x206F));
}

char16_t foldAscii(char16_t c) { return (c >= u'A' && c <= u'Z') ? static_cast<char16_t>(c - u'A' + u'a') : c; }

}  // namespace

Matcher defaultMatcher(std::u16string needle, bool matchCase, bool wholeWords) {
    if (!matchCase)
        for (auto& c : needle) c = foldAscii(c);
    return [needle, matchCase, wholeWords](std::u16string_view hay, size_t from, bool backward, size_t& pos,
                                           size_t& len) {
        const size_t m = needle.size();
        if (m == 0 || hay.size() < m) return false;
        auto at = [&](size_t i) {
            for (size_t k = 0; k < m; ++k) {
                char16_t c = hay[i + k];
                if (!matchCase) c = foldAscii(c);
                if (c != needle[k]) return false;
            }
            if (wholeWords) {
                if (i > 0 && isWordChar(hay[i - 1]) && isWordChar(needle.front())) return false;
                if (i + m < hay.size() && isWordChar(hay[i + m]) && isWordChar(needle.back())) return false;
            }
            return true;
        };
        const size_t last = hay.size() - m;
        if (!backward) {
            for (size_t i = from; i <= last; ++i)
                if (at(i)) {
                    pos = i;
                    len = m;
                    return true;
                }
        } else {
            size_t i = std::min(from == 0 ? 0 : from - 1, last);
            if (from == 0) return false;
            for (;; --i) {
                if (at(i)) {
                    pos = i;
                    len = m;
                    return true;
                }
                if (i == 0) break;
            }
        }
        return false;
    };
}

std::optional<SearchHit> searchTable(const Table& t, const std::vector<uint32_t>& order, const SearchStart& st,
                                     bool backward, const Matcher& match) {
    const size_t cols = t.columns;
    if (cols == 0 || !match) return std::nullopt;
    const long firstRow = t.headerRows() ? -1 : 0;
    const long lastRow = static_cast<long>(t.dataRows()) - 1;
    if (lastRow < firstRow) return std::nullopt;
    auto fileRowOf = [&](long r) -> size_t {
        if (r < 0) return 0;
        size_t d = static_cast<size_t>(r) < order.size() ? order[static_cast<size_t>(r)] : static_cast<size_t>(r);
        return t.fileRowOfData(d);
    };
    long row = std::clamp(st.row, firstRow, lastRow);
    long col = static_cast<long>(std::min(st.col, cols - 1));
    bool first = st.includeCell;
    if (!first) {  // move one cell
        if (!backward) {
            if (++col >= static_cast<long>(cols)) { col = 0; ++row; }
        } else {
            if (--col < 0) { col = static_cast<long>(cols) - 1; --row; }
        }
    }
    std::vector<Field> f;
    const std::string_view data(t.data);
    long cachedRow = LONG_MIN;
    while (row >= firstRow && row <= lastRow) {
        if (row != cachedRow) {
            t.fields(fileRowOf(row), f);
            cachedRow = row;
        }
        if (col < static_cast<long>(f.size())) {
            std::u16string hay = utf8ToUtf16(fieldValue(data, f[static_cast<size_t>(col)]));
            size_t from = backward ? hay.size() : 0;
            if (first) from = backward ? std::min(st.offset, hay.size()) : st.offset;
            size_t pos = 0, len = 0;
            if ((backward || from <= hay.size()) && match(hay, from, backward, pos, len))
                return SearchHit{row, static_cast<size_t>(col), pos, len};
        }
        first = false;
        if (!backward) {
            if (++col >= static_cast<long>(cols)) { col = 0; ++row; }
        } else {
            if (--col < 0) { col = static_cast<long>(cols) - 1; --row; }
        }
    }
    return std::nullopt;
}

// ------------------------------------------------------------------ clipboard

std::string quoteIfNeeded(std::string_view v, char delimiter) {
    bool need = false;
    for (char c : v)
        if (c == delimiter || c == '"' || c == '\r' || c == '\n') {
            need = true;
            break;
        }
    if (!need) return std::string(v);
    std::string out;
    out.reserve(v.size() + 2);
    out.push_back('"');
    for (char c : v) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string copyRange(const Table& t, const std::vector<uint32_t>& order, const CellRange& r, CopyFormat fmt,
                      bool withHeader) {
    const char sep = fmt == CopyFormat::Tsv ? '\t' : t.delimiter;
    withHeader = withHeader && t.headerRows();
    const size_t rows = t.dataRows();
    if (rows == 0 && !withHeader) return {};
    size_t r0 = std::min(r.r0, rows ? rows - 1 : 0), r1 = std::min(r.r1, rows ? rows - 1 : 0);
    size_t c0 = std::min(r.c0, t.columns ? t.columns - 1 : 0), c1 = std::min(r.c1, t.columns ? t.columns - 1 : 0);
    if (r0 > r1) std::swap(r0, r1);
    if (c0 > c1) std::swap(c0, c1);
    std::vector<Field> f;
    const std::string_view data(t.data);
    if (fmt == CopyFormat::Tsv && !withHeader && r0 == r1 && c0 == c1 && rows) {
        size_t d = r0 < order.size() ? order[r0] : r0;
        return t.cell(t.fileRowOfData(d), c0);
    }
    std::string out;
    auto emitRow = [&](size_t fileRow) {
        t.fields(fileRow, f);
        for (size_t c = c0; c <= c1; ++c) {
            if (c > c0) out.push_back(sep);
            if (c < f.size()) out += quoteIfNeeded(fieldValue(data, f[c]), sep);
        }
    };
    bool firstLine = true;
    if (withHeader) {
        emitRow(0);
        firstLine = false;
    }
    if (rows)
        for (size_t dr = r0; dr <= r1; ++dr) {
            if (!firstLine) out += "\r\n";
            firstLine = false;
            size_t d = dr < order.size() ? order[dr] : dr;
            emitRow(t.fileRowOfData(d));
        }
    return out;
}

}  // namespace csvvw::csv
