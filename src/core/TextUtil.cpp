#include "TextUtil.h"

#include <cstdio>

namespace csvvw {

std::string toLowerAscii(std::string_view s) {
    std::string r(s);
    for (auto& c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

std::string_view trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> splitList(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t next = s.find(sep, pos);
        if (next == std::string_view::npos) next = s.size();
        auto item = trim(s.substr(pos, next - pos));
        if (!item.empty()) out.emplace_back(item);
        pos = next + 1;
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

std::u16string utf8ToUtf16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0, n = s.size();
    auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    while (i < n) {
        unsigned char c = byte(i);
        uint32_t cp;
        size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { out.push_back(0xFFFD); ++i; continue; }
        if (i + len > n) { out.push_back(0xFFFD); ++i; continue; }
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            unsigned char cc = byte(i + k);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok || (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += len;
    }
    return out;
}

std::string utf16ToUtf8(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::u16string utf8ToUtf16Capped(std::string_view s, size_t maxUnits, bool* truncated) {
    // Find the byte offset after which ~maxUnits units would be produced.
    size_t units = 0, i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        size_t u = len == 4 ? 2 : 1;
        if (units + u > maxUnits) break;
        units += u;
        i += len;
    }
    if (i > n) i = n;
    if (truncated) *truncated = i < n;
    return utf8ToUtf16(s.substr(0, i));
}

bool parseSizeSpec(std::string_view s, uint64_t& out) {
    s = trim(s);
    if (s.empty()) return false;
    uint64_t mul = 1;
    char last = s.back();
    if (last == 'b' || last == 'B') {
        s.remove_suffix(1);
        if (s.empty()) return false;
        last = s.back();
    }
    switch (last) {
        case 'k': case 'K': mul = 1024; break;
        case 'm': case 'M': mul = 1024ull * 1024; break;
        case 'g': case 'G': mul = 1024ull * 1024 * 1024; break;
        default: break;
    }
    if (mul != 1) s.remove_suffix(1);
    s = trim(s);
    if (s.empty() || s.size() > 12) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    out = v * mul;
    return true;
}

std::string formatSizeSpec(uint64_t v) {
    static const struct { uint64_t mul; const char* suffix; } units[] = {
        {1024ull * 1024 * 1024, "G"}, {1024ull * 1024, "M"}, {1024, "K"}};
    if (v != 0)
        for (auto& u : units)
            if (v % u.mul == 0) return std::to_string(v / u.mul) + u.suffix;
    return std::to_string(v);
}

std::string formatSize(uint64_t b) {
    const char* units[] = {"bytes", "KB", "MB", "GB"};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024 && u < 3) {
        v /= 1024;
        ++u;
    }
    char buf[64];
    if (u == 0) std::snprintf(buf, sizeof buf, "%llu bytes", static_cast<unsigned long long>(b));
    else if (v < 10) std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    else std::snprintf(buf, sizeof buf, "%.0f %s", v, units[u]);
    return buf;
}

bool parseColor(std::string_view s, uint32_t& out) {
    s = trim(s);
    if (!s.empty() && s[0] == '#') s.remove_prefix(1);
    if (s.size() != 6) return false;
    uint32_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | static_cast<uint32_t>(d);
    }
    out = v;
    return true;
}

std::string formatColor(uint32_t rgb) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%06X", rgb & 0xFFFFFFu);
    return buf;
}

}  // namespace csvvw
