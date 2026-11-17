// Small portable string helpers.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace csvvw {

std::string toLowerAscii(std::string_view s);
std::string_view trim(std::string_view s);
std::vector<std::string> splitList(std::string_view s, char sep);  // trimmed, empties dropped
bool iequals(std::string_view a, std::string_view b);

// UTF conversions (invalid sequences become U+FFFD).
std::u16string utf8ToUtf16(std::string_view s);
std::string utf16ToUtf8(std::u16string_view s);

// Path from a UTF-8 string (portable replacement for deprecated u8path).
inline std::filesystem::path utf8Path(std::string_view s) {
    std::u8string u(s.begin(), s.end());
    return std::filesystem::path(u);
}
inline std::string pathToUtf8(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// UTF-8 -> UTF-16 of at most `maxUnits` code units (never splits a surrogate
// pair). `truncated` is set when input remained.
std::u16string utf8ToUtf16Capped(std::string_view s, size_t maxUnits, bool* truncated = nullptr);

// Sizes: "100M", "32K", "1G", "12345" (K = 1024). Returns false if invalid.
bool parseSizeSpec(std::string_view s, uint64_t& out);
std::string formatSizeSpec(uint64_t v);  // shortest exact form: 104857600 -> "100M"
std::string formatSize(uint64_t bytes);  // human readable: "250 MB", "3.4 KB"

// Colors "#RRGGBB" <-> 0xRRGGBB.
bool parseColor(std::string_view s, uint32_t& out);
std::string formatColor(uint32_t rgb);

}  // namespace csvvw
