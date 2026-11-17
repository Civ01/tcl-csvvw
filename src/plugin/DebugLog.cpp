#include "DebugLog.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "WinUtil.h"

namespace csvvw::dbg {

namespace {
bool g_on = false;
std::mutex g_mutex;
FILE* g_file = nullptr;

FILE* openLog() {
    if (g_file) return g_file;
    auto p = win::pluginDir() / L"tcl-csvvw.log";
    g_file = _wfopen(p.c_str(), L"ab");
    if (!g_file) {
        wchar_t tmp[MAX_PATH + 1] = {};
        if (GetTempPathW(MAX_PATH, tmp)) g_file = _wfopen((std::wstring(tmp) + L"tcl-csvvw.log").c_str(), L"ab");
    }
    return g_file;
}
}  // namespace

void configure(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_on = enabled;
    if (!enabled && g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

bool on() { return g_on; }

void log(const char* fmt, ...) {
    if (!g_on) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    FILE* f = openLog();
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(f, "%02u:%02u:%02u.%03u [%lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fflush(f);
}

}  // namespace csvvw::dbg
