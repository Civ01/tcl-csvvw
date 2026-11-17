#include "Parser.h"

namespace csvvw::csv {

std::vector<uint32_t> scanRows(std::string_view d, char delim) {
    std::vector<uint32_t> starts;
    const size_t n = d.size();
    starts.reserve(n / 32 + 2);
    size_t rowStart = 0;
    bool atFieldStart = true;
    size_t i = 0;
    while (i < n) {
        char c = d[i];
        if (c == '"' && atFieldStart) {
            // Quoted field: skip to the closing quote ("" is an escaped quote).
            ++i;
            for (;;) {
                if (i >= n) break;
                if (d[i] == '"') {
                    if (i + 1 < n && d[i + 1] == '"') {
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                ++i;
            }
            atFieldStart = false;
            continue;
        }
        if (c == delim) {
            atFieldStart = true;
            ++i;
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < n && d[i + 1] == '\n') ++i;
            ++i;
            starts.push_back(static_cast<uint32_t>(rowStart));
            rowStart = i;
            atFieldStart = true;
            continue;
        }
        atFieldStart = false;
        ++i;
    }
    if (rowStart < n) starts.push_back(static_cast<uint32_t>(rowStart));
    starts.push_back(static_cast<uint32_t>(n));
    return starts;
}

void splitRow(std::string_view d, uint32_t begin, uint32_t end, char delim, std::vector<Field>& out,
              size_t maxFields) {
    out.clear();
    size_t i = begin;
    size_t fieldStart = begin;
    bool atFieldStart = true;
    while (i < end) {
        char c = d[i];
        if (c == '"' && atFieldStart) {
            ++i;
            while (i < end) {
                if (d[i] == '"') {
                    if (i + 1 < end && d[i + 1] == '"') {
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                ++i;
            }
            atFieldStart = false;
            continue;
        }
        if (c == delim) {
            if (maxFields == 0 || out.size() + 1 < maxFields) {
                out.push_back({static_cast<uint32_t>(fieldStart), static_cast<uint32_t>(i - fieldStart)});
                fieldStart = i + 1;
                atFieldStart = true;
            } else {
                atFieldStart = false;  // last allowed field swallows the rest
            }
            ++i;
            continue;
        }
        if (c == '\n' || c == '\r') break;
        atFieldStart = false;
        ++i;
    }
    out.push_back({static_cast<uint32_t>(fieldStart), static_cast<uint32_t>(i - fieldStart)});
}

std::string unescape(std::string_view q) {
    std::string out;
    out.reserve(q.size());
    size_t i = q.empty() || q[0] != '"' ? 0 : 1;
    bool inQuotes = i == 1;
    while (i < q.size()) {
        char c = q[i];
        if (inQuotes && c == '"') {
            if (i + 1 < q.size() && q[i + 1] == '"') {
                out.push_back('"');
                i += 2;
                continue;
            }
            inQuotes = false;
            ++i;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

std::string fieldValue(std::string_view d, const Field& f) {
    auto raw = rawField(d, f);
    if (raw.empty() || raw[0] != '"') return std::string(raw);
    return unescape(raw);
}

}  // namespace csvvw::csv
