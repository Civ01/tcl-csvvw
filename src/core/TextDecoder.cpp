#include "TextDecoder.h"

#include "TextUtil.h"

namespace csvvw {

const char* encodingName(Encoding e) {
    switch (e) {
        case Encoding::Utf8: return "UTF-8";
        case Encoding::Utf8Bom: return "UTF-8 (BOM)";
        case Encoding::Utf16LE: return "UTF-16 LE";
        case Encoding::Utf16BE: return "UTF-16 BE";
        case Encoding::Ansi: return "ANSI";
    }
    return "?";
}

bool isValidUtf8(std::string_view s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        size_t len;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

static std::u16string decodeUtf16(std::string_view b, bool bigEndian) {
    std::u16string out;
    out.resize(b.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        auto lo = static_cast<unsigned char>(b[2 * i + (bigEndian ? 1 : 0)]);
        auto hi = static_cast<unsigned char>(b[2 * i + (bigEndian ? 0 : 1)]);
        out[i] = static_cast<char16_t>(lo | (hi << 8));
    }
    return out;
}

DecodedText decodeText(std::string_view bytes, const AnsiConverter& ansi) {
    DecodedText r;
    auto has = [&](std::initializer_list<unsigned char> bom) {
        if (bytes.size() < bom.size()) return false;
        size_t i = 0;
        for (auto c : bom)
            if (static_cast<unsigned char>(bytes[i++]) != c) return false;
        return true;
    };
    if (has({0xEF, 0xBB, 0xBF})) {
        r.encoding = Encoding::Utf8Bom;
        r.text = utf8ToUtf16(bytes.substr(3));
    } else if (has({0xFF, 0xFE})) {
        r.encoding = Encoding::Utf16LE;
        r.text = decodeUtf16(bytes.substr(2), false);
    } else if (has({0xFE, 0xFF})) {
        r.encoding = Encoding::Utf16BE;
        r.text = decodeUtf16(bytes.substr(2), true);
    } else if (isValidUtf8(bytes)) {
        r.encoding = Encoding::Utf8;
        r.text = utf8ToUtf16(bytes);
    } else {
        r.encoding = Encoding::Ansi;
        if (ansi) {
            r.text = ansi(bytes);
        } else {
            r.text.reserve(bytes.size());
            for (char c : bytes) r.text.push_back(static_cast<unsigned char>(c));
        }
    }
    return r;
}

static void appendUtf8(std::string& out, uint32_t cp) {
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

static std::string utf16BytesToUtf8(std::string_view b, bool bigEndian) {
    std::string out;
    out.reserve(b.size() / 2 + b.size() / 8);
    size_t n = b.size() / 2;
    auto unit = [&](size_t i) -> uint32_t {
        auto lo = static_cast<unsigned char>(b[2 * i + (bigEndian ? 1 : 0)]);
        auto hi = static_cast<unsigned char>(b[2 * i + (bigEndian ? 0 : 1)]);
        return lo | (hi << 8);
    };
    for (size_t i = 0; i < n; ++i) {
        uint32_t cp = unit(i);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n) {
            uint32_t lo = unit(i + 1);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        appendUtf8(out, cp);
    }
    return out;
}

static bool hasPrefix(std::string_view b, std::initializer_list<unsigned char> bom) {
    if (b.size() < bom.size()) return false;
    size_t i = 0;
    for (auto c : bom)
        if (static_cast<unsigned char>(b[i++]) != c) return false;
    return true;
}

DecodedUtf8 decodeToUtf8(std::string&& bytes, EncodingChoice choice, const AnsiConverter& ansi) {
    DecodedUtf8 r;
    const bool bom8 = hasPrefix(bytes, {0xEF, 0xBB, 0xBF});
    const bool bomLE = hasPrefix(bytes, {0xFF, 0xFE});
    const bool bomBE = hasPrefix(bytes, {0xFE, 0xFF});
    if (choice == EncodingChoice::Auto) {
        if (bom8) choice = EncodingChoice::Utf8;
        else if (bomLE) choice = EncodingChoice::Utf16LE;
        else if (bomBE) choice = EncodingChoice::Utf16BE;
        else if (isValidUtf8(bytes)) choice = EncodingChoice::Utf8;
        else choice = EncodingChoice::Ansi;
    }
    switch (choice) {
        case EncodingChoice::Utf8:
            r.encoding = bom8 ? Encoding::Utf8Bom : Encoding::Utf8;
            if (bom8) bytes.erase(0, 3);
            r.text = std::move(bytes);
            break;
        case EncodingChoice::Utf16LE:
        case EncodingChoice::Utf16BE: {
            bool be = choice == EncodingChoice::Utf16BE;
            r.encoding = be ? Encoding::Utf16BE : Encoding::Utf16LE;
            std::string_view v(bytes);
            if ((be && bomBE) || (!be && bomLE)) v.remove_prefix(2);
            r.text = utf16BytesToUtf8(v, be);
            break;
        }
        default: {
            r.encoding = Encoding::Ansi;
            if (ansi) {
                r.text = utf16ToUtf8(ansi(bytes));
            } else {  // portable fallback: Latin-1
                r.text.reserve(bytes.size() + bytes.size() / 4);
                for (char c : bytes) appendUtf8(r.text, static_cast<unsigned char>(c));
            }
            break;
        }
    }
    return r;
}

const char* encodingChoiceKey(EncodingChoice c) {
    switch (c) {
        case EncodingChoice::Auto: return "auto";
        case EncodingChoice::Utf8: return "utf8";
        case EncodingChoice::Utf16LE: return "utf16le";
        case EncodingChoice::Utf16BE: return "utf16be";
        case EncodingChoice::Ansi: return "ansi";
    }
    return "auto";
}

bool parseEncodingChoice(std::string_view s, EncodingChoice& out) {
    for (auto c : {EncodingChoice::Auto, EncodingChoice::Utf8, EncodingChoice::Utf16LE, EncodingChoice::Utf16BE,
                   EncodingChoice::Ansi})
        if (iequals(s, encodingChoiceKey(c))) {
            out = c;
            return true;
        }
    if (iequals(s, "utf-8")) { out = EncodingChoice::Utf8; return true; }
    return false;
}

}  // namespace csvvw
