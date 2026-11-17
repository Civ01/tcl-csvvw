#include "TextLayout.h"

namespace csvvw::csv {

bool hasLineBreak(std::u16string_view s) {
    for (char16_t c : s)
        if (c == u'\n' || c == u'\r') return true;
    return false;
}

size_t countLines(std::u16string_view s) {
    size_t n = 1;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == u'\r') {
            ++n;
            if (i + 1 < s.size() && s[i + 1] == u'\n') ++i;
        } else if (s[i] == u'\n') {
            ++n;
        }
    }
    return n;
}

static void appendChar(DisplayLine& d, char16_t c, bool glyph) {
    if (glyph && d.glyphMask.empty()) d.glyphMask.assign(d.text.size(), false);
    d.text.push_back(c);
    if (!d.glyphMask.empty()) d.glyphMask.push_back(glyph);
}

DisplayLine singleLine(std::u16string_view s) {
    DisplayLine d;
    d.text.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char16_t c = s[i];
        if (c == u'\r' || c == u'\n') {
            if (c == u'\r' && i + 1 < s.size() && s[i + 1] == u'\n') ++i;
            appendChar(d, kLineBreakGlyph, true);
        } else if (c == u'\t' || c == 0) {
            appendChar(d, u' ', false);
        } else {
            appendChar(d, c, false);
        }
    }
    return d;
}

std::vector<DisplayLine> ownLines(std::u16string_view s) {
    std::vector<DisplayLine> out(1);
    for (size_t i = 0; i < s.size(); ++i) {
        char16_t c = s[i];
        if (c == u'\r' || c == u'\n') {
            if (c == u'\r' && i + 1 < s.size() && s[i + 1] == u'\n') ++i;
            appendChar(out.back(), kLineBreakGlyph, true);
            out.emplace_back();
        } else if (c == u'\t' || c == 0) {
            appendChar(out.back(), u' ', false);
        } else {
            appendChar(out.back(), c, false);
        }
    }
    return out;
}

size_t singleLineOffset(std::u16string_view s, size_t offset) {
    size_t out = 0;
    for (size_t i = 0; i < s.size() && i < offset; ++i) {
        if (s[i] == u'\r' && i + 1 < s.size() && s[i + 1] == u'\n') {
            if (i + 1 < offset) ++i;  // CRLF -> one glyph
        }
        ++out;
    }
    return out;
}

static bool breakAfter(char16_t c) {
    return c == u' ' || c == u'-' || c == u'/' || c == u',' || c == u';' || c == u'\t';
}

WrapResult wrapText(std::u16string_view text, int width, size_t maxLines, const FitFn& fit) {
    WrapResult r;
    if (maxLines == 0) maxLines = 1;
    size_t pos = 0;
    if (text.empty()) {
        r.lines.push_back(text);
        return r;
    }
    while (pos < text.size()) {
        if (r.lines.size() == maxLines) {
            r.truncated = true;
            break;
        }
        std::u16string_view rest = text.substr(pos);
        size_t n = fit(rest, width);
        if (n >= rest.size()) {
            r.lines.push_back(rest);
            pos = text.size();
            break;
        }
        if (n == 0) n = 1;  // always make progress
        // Back up to the last break opportunity inside the fitting prefix.
        size_t cut = n;
        // If the char right after the fitting prefix is a space, the prefix is a clean word end.
        if (rest[n] != u' ') {
            size_t k = n;
            while (k > 0 && !breakAfter(rest[k - 1])) --k;
            if (k > 0) cut = k;
        }
        // Don't split a surrogate pair.
        if (cut < rest.size() && cut > 0 && rest[cut - 1] >= 0xD800 && rest[cut - 1] <= 0xDBFF) {
            cut = cut > 1 ? cut - 1 : cut + 1;
        }
        std::u16string_view line = rest.substr(0, cut);
        while (!line.empty() && line.back() == u' ') line.remove_suffix(1);
        r.lines.push_back(line);
        pos += cut;
        while (pos < text.size() && text[pos] == u' ') ++pos;
    }
    if (r.lines.empty()) r.lines.push_back(text.substr(0, 0));
    return r;
}

}  // namespace csvvw::csv
