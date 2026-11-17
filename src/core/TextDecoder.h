// Byte -> text decoding with BOM / UTF-8 detection and ANSI fallback.
//
// The CSV pipeline works on UTF-8 internally (SPECS §5.1): every input is
// converted to UTF-8 once at load, so the parser only ever scans bytes.
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace csvvw {

enum class Encoding { Utf8, Utf8Bom, Utf16LE, Utf16BE, Ansi };
// User choice: Auto, or force one decoding (menu / INI "encoding=").
enum class EncodingChoice { Auto, Utf8, Utf16LE, Utf16BE, Ansi };

const char* encodingName(Encoding e);
const char* encodingChoiceKey(EncodingChoice c);  // "auto", "utf8", ...
bool parseEncodingChoice(std::string_view s, EncodingChoice& out);

// Converts bytes in the "ANSI" (system) code page. The platform layer supplies
// MultiByteToWideChar(CP_ACP); the portable default maps bytes as Latin-1.
using AnsiConverter = std::function<std::u16string(std::string_view)>;

bool isValidUtf8(std::string_view bytes);

// --- UTF-16 result (used for the plugin INI) ---
struct DecodedText {
    std::u16string text;  // without BOM
    Encoding encoding = Encoding::Utf8;
};
DecodedText decodeText(std::string_view bytes, const AnsiConverter& ansi = {});

// --- UTF-8 result (CSV data) ---
struct DecodedUtf8 {
    std::string text;  // without BOM; UTF-8 (invalid input bytes kept as is when UTF-8 is forced)
    Encoding encoding = Encoding::Utf8;
};
// Consumes `bytes` (moved into the result when no conversion is needed).
DecodedUtf8 decodeToUtf8(std::string&& bytes, EncodingChoice choice, const AnsiConverter& ansi = {});

}  // namespace csvvw
