#include "ValueParse.h"

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <string>

namespace csvvw::csv {

namespace {

bool isDigitC(char c) { return c >= '0' && c <= '9'; }

std::string_view trimSpaces(std::string_view s) {
    auto isSp = [](std::string_view v, size_t i, bool fromEnd) -> size_t {
        // returns byte length of a leading/trailing space (ASCII space/tab or NBSP U+00A0)
        if (!fromEnd) {
            if (i < v.size() && (v[i] == ' ' || v[i] == '\t')) return 1;
            if (i + 1 < v.size() && static_cast<unsigned char>(v[i]) == 0xC2 &&
                static_cast<unsigned char>(v[i + 1]) == 0xA0)
                return 2;
            return 0;
        }
        if (!v.empty() && (v.back() == ' ' || v.back() == '\t')) return 1;
        if (v.size() >= 2 && static_cast<unsigned char>(v[v.size() - 2]) == 0xC2 &&
            static_cast<unsigned char>(v.back()) == 0xA0)
            return 2;
        return 0;
    };
    for (size_t k; (k = isSp(s, 0, false)) != 0;) s.remove_prefix(k);
    for (size_t k; (k = isSp(s, 0, true)) != 0;) s.remove_suffix(k);
    return s;
}

}  // namespace

bool isBlank(std::string_view s) { return trimSpaces(s).empty(); }

bool parseNumber(std::string_view s, CommaMode mode, double& out, CommaHint* hint) {
    if (hint) *hint = CommaHint::None;
    s = trimSpaces(s);
    if (s.empty() || s.size() > 64) return false;
    if (s.back() == '%') {
        s.remove_suffix(1);
        s = trimSpaces(s);
        if (s.empty()) return false;
    }
    size_t i = 0;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') {
        neg = s[i] == '-';
        ++i;
    }
    // Collect the mantissa: digits and separators. Decide on the decimal
    // separator afterwards.
    std::string digits;   // normalized mantissa with '.' as decimal point
    size_t dots = 0, commas = 0, lastDot = 0, lastComma = 0;
    size_t mStart = i;
    std::string_view rest;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (isDigitC(c)) continue;
        if (c == '.') { ++dots; lastDot = i; continue; }
        if (c == ',') { ++commas; lastComma = i; continue; }
        if (c == '\'' || c == ' ') continue;
        if (static_cast<unsigned char>(c) == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) {
            ++i;
            continue;
        }
        break;
    }
    std::string_view mant = s.substr(mStart, i - mStart);
    rest = s.substr(i);
    if (mant.empty()) return false;
    if (!isDigitC(mant.front()) && mant.front() != '.' && mant.front() != ',') return false;
    if (!isDigitC(mant.back())) return false;  // "12." / "1," / "1 " are not numbers

    // Pick the decimal separator.
    char decimal = 0;  // 0 = integer
    if (dots && commas) {
        decimal = lastDot > lastComma ? '.' : ',';
        if (hint) *hint = decimal == ',' ? CommaHint::Decimal : CommaHint::Thousands;
    } else if (dots) {
        decimal = dots == 1 ? '.' : 0;  // "1.234.567" = thousands
        if (dots > 1 && commas == 0) {
            // only valid as thousands groups
        }
    } else if (commas) {
        if (commas > 1) {
            decimal = 0;
            if (hint) *hint = CommaHint::Thousands;
        } else {
            size_t after = mant.size() - (lastComma - mStart) - 1;
            if (after != 3) {
                decimal = ',';
                if (hint) *hint = CommaHint::Decimal;
            } else {
                decimal = mode == CommaMode::Decimal ? ',' : 0;
            }
        }
    }
    // Validate grouping: every separator that is not the decimal must be a
    // thousands separator between digit groups of 3 (first group 1-3).
    size_t groupLen = 0;
    bool seenDecimal = false, seenGroupSep = false, firstGroup = true;
    for (size_t k = 0; k < mant.size(); ++k) {
        char c = mant[k];
        if (isDigitC(c)) {
            digits.push_back(c);
            ++groupLen;
            continue;
        }
        if (static_cast<unsigned char>(c) == 0xC2) continue;  // NBSP lead byte; tail handled below
        bool isNbspTail = static_cast<unsigned char>(c) == 0xA0;
        if (!seenDecimal && c == decimal) {
            if (seenGroupSep && groupLen != 3) return false;
            seenDecimal = true;
            digits.push_back('.');
            groupLen = 0;
            continue;
        }
        if (seenDecimal) return false;  // separators after the decimal point
        // thousands separator
        if (firstGroup) {
            if (groupLen < 1 || groupLen > 3) return false;
            firstGroup = false;
        } else if (groupLen != 3) {
            return false;
        }
        (void)isNbspTail;
        seenGroupSep = true;
        groupLen = 0;
    }
    if (!seenDecimal && seenGroupSep && groupLen != 3) return false;
    if (digits.empty() || digits == ".") return false;
    // Exponent.
    if (!rest.empty()) {
        if (rest[0] != 'e' && rest[0] != 'E') return false;
        size_t k = 1;
        std::string exp = "e";
        if (k < rest.size() && (rest[k] == '+' || rest[k] == '-')) exp.push_back(rest[k++]);
        if (k >= rest.size()) return false;
        for (; k < rest.size(); ++k) {
            if (!isDigitC(rest[k])) return false;
            exp.push_back(rest[k]);
        }
        if (exp.size() > 6) return false;
        digits += exp;
    }
    char* endp = nullptr;
    double v = std::strtod(digits.c_str(), &endp);
    if (!endp || *endp != '\0' || !std::isfinite(v)) return false;
    out = neg ? -v : v;
    return true;
}

namespace {

int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe);
}

bool readInt(std::string_view s, size_t& i, size_t ndigits, int& out) {
    if (i + ndigits > s.size()) return false;
    int v = 0;
    for (size_t k = 0; k < ndigits; ++k) {
        if (!isDigitC(s[i + k])) return false;
        v = v * 10 + (s[i + k] - '0');
    }
    i += ndigits;
    out = v;
    return true;
}

bool validDay(int y, int m, int d) {
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12 || d < 1) return false;
    int lim = dim[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) lim = 29;
    return d <= lim;
}

// Optional time "hh:mm[:ss[.fff]]" starting at i. Returns seconds or -1.
double parseTime(std::string_view s, size_t& i) {
    int hh, mm, ss = 0;
    if (!readInt(s, i, 2, hh)) return -1;
    if (i >= s.size() || s[i] != ':') return -1;
    ++i;
    if (!readInt(s, i, 2, mm)) return -1;
    double frac = 0;
    if (i < s.size() && s[i] == ':') {
        ++i;
        if (!readInt(s, i, 2, ss)) return -1;
        if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
            ++i;
            double scale = 0.1;
            size_t start = i;
            while (i < s.size() && isDigitC(s[i])) {
                frac += (s[i] - '0') * scale;
                scale /= 10;
                ++i;
            }
            if (i == start) return -1;
        }
    }
    if (hh > 23 || mm > 59 || ss > 60) return -1;
    return hh * 3600.0 + mm * 60.0 + ss + frac;
}

}  // namespace

bool parseDate(std::string_view s, double& out) {
    s = trimSpaces(s);
    if (s.size() < 10 || s.size() > 40) return false;
    int y = 0, m = 0, d = 0;
    size_t i = 0;
    if (s[4] == '-') {  // ISO
        if (!readInt(s, i, 4, y) || s[i++] != '-' || !readInt(s, i, 2, m) || i >= s.size() || s[i++] != '-' ||
            !readInt(s, i, 2, d))
            return false;
    } else if (s[2] == '.') {  // DD.MM.YYYY
        if (!readInt(s, i, 2, d) || s[i++] != '.' || !readInt(s, i, 2, m) || i >= s.size() || s[i++] != '.' ||
            !readInt(s, i, 4, y))
            return false;
    } else {
        return false;
    }
    if (!validDay(y, m, d)) return false;
    double secs = 0;
    if (i < s.size()) {
        if (s[i] != 'T' && s[i] != ' ') return false;
        ++i;
        secs = parseTime(s, i);
        if (secs < 0) return false;
        if (i < s.size() && (s[i] == 'Z' || s[i] == 'z')) ++i;
        if (i != s.size()) return false;
    }
    out = static_cast<double>(daysFromCivil(y, static_cast<unsigned>(m), static_cast<unsigned>(d))) * 86400.0 + secs;
    return true;
}

ValueKind classify(std::string_view s, CommaMode mode, double* key, CommaHint* hint) {
    if (hint) *hint = CommaHint::None;
    if (isBlank(s)) return ValueKind::Empty;
    double v = 0;
    if (parseNumber(s, mode, v, hint)) {
        if (key) *key = v;
        return ValueKind::Number;
    }
    if (parseDate(s, v)) {
        if (key) *key = v;
        return ValueKind::Date;
    }
    return ValueKind::Text;
}

}  // namespace csvvw::csv
