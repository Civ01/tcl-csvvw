// Display transforms and line layout for cell text (SPECS §6.2, §6.3.1).
//
// All functions work on UTF-16 (what the Windows text APIs take). Measuring is
// injected so the logic is testable without GDI.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace csvvw::csv {

constexpr char16_t kLineBreakGlyph = u'↵';  // ↵
constexpr char16_t kEllipsis = u'…';        // …

// A run of display text; `glyph` runs are drawn dimmed (↵).
struct DisplayLine {
    std::u16string text;
    std::vector<bool> glyphMask;  // same length as text; true for ↵ glyphs (empty = none)
    bool hasGlyph() const { return !glyphMask.empty(); }
};

bool hasLineBreak(std::u16string_view s);
size_t countLines(std::u16string_view s);  // 1 + number of line breaks (CRLF counts once)

// Single-line display: line breaks become ↵, tabs and NULs a space.
DisplayLine singleLine(std::u16string_view s);

// Multi-line display: one entry per own line; every line that ends with a
// break gets a trailing ↵ glyph.
std::vector<DisplayLine> ownLines(std::u16string_view s);

// Maps an offset in the original text to the offset in singleLine() output
// (CRLF collapses to one glyph).
size_t singleLineOffset(std::u16string_view s, size_t offset);

// Returns how many leading characters of `text` fit into `width` pixels.
using FitFn = std::function<size_t(std::u16string_view text, int width)>;

struct WrapResult {
    std::vector<std::u16string_view> lines;  // views into the input
    bool truncated = false;                  // text remained after maxLines
};

// Greedy word wrap: breaks after spaces and after '-', '/', ',', ';'; a word
// wider than the line is broken between characters. Leading spaces of a
// continuation line are skipped. At most maxLines lines (>= 1).
WrapResult wrapText(std::u16string_view text, int width, size_t maxLines, const FitFn& fit);

}  // namespace csvvw::csv
