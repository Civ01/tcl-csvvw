// Windows helpers shared by the plugin layer.
#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

#include "core/TextDecoder.h"

namespace csvvw::win {

extern HINSTANCE g_instance;

std::filesystem::path modulePath();
std::filesystem::path pluginDir();
std::filesystem::path iniPath();  // <module stem>.ini next to the DLL

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view w);
std::wstring ansiToWide(const char* s);
inline std::wstring toW(std::u16string_view s) { return std::wstring(s.begin(), s.end()); }
inline std::u16string toU16(std::wstring_view s) { return std::u16string(s.begin(), s.end()); }

const AnsiConverter& ansiConverter();  // MultiByteToWideChar(CP_ACP)

// File size without reading; false if the file cannot be opened.
bool fileSize(const std::wstring& path, uint64_t& size);
// Reads a whole file (up to 2 GB); returns false on failure.
bool readFile(const std::wstring& path, std::string& out, uint64_t& size);

void copyToClipboard(HWND owner, const std::wstring& text);

inline COLORREF toColorRef(uint32_t c) {
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

}  // namespace csvvw::win
