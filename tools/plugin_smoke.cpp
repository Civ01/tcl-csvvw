// End-to-end smoke test for tcl-csvvw.wlx64.
//
// Loads the plugin DLL like Total Commander does (host window of class
// "TLister"), opens the sample files and drives the real window: detect
// string, load / load-next, copy, select all, search, header-click sorting,
// context-menu toggles, every display mode (painted), the oversized notice and
// a load-time measurement on a generated file.
//
//   plugin_smoke <path-to-tcl-csvvw.wlx64> <samples-dir>
//
// The plugin directory must contain tcl-csvvw.ini; the test rewrites it for
// some cases and restores it at the end.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "plugin/listplug.h"
#include "ui/resource.h"

namespace fs = std::filesystem;

namespace {

template <class F>
F proc(HMODULE m, const char* name) {
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

using LoadW_t = HWND(__stdcall*)(HWND, WCHAR*, int);
using LoadNextW_t = int(__stdcall*)(HWND, HWND, WCHAR*, int);
using Close_t = void(__stdcall*)(HWND);
using Detect_t = void(__stdcall*)(char*, int);
using SearchW_t = int(__stdcall*)(HWND, WCHAR*, int);
using Command_t = int(__stdcall*)(HWND, int, int);

struct Api {
    LoadW_t load = nullptr;
    LoadNextW_t loadNext = nullptr;
    Close_t close = nullptr;
    Detect_t detect = nullptr;
    SearchW_t search = nullptr;
    Command_t command = nullptr;
} api;

int g_failures = 0, g_checks = 0;
int g_lastPercent = -1;
HWND g_host = nullptr;
fs::path g_pluginDir, g_samples, g_ini;
std::string g_iniBackup;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

void section(const char* name) { std::printf("[%s]\n", name); }

void pump(int ms) {
    DWORD end = GetTickCount() + static_cast<DWORD>(ms);
    MSG msg;
    do {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (ms > 0) Sleep(2);
    } while (GetTickCount() < end);
}

LRESULT CALLBACK hostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && HIWORD(wp) == itm_percent) {
        g_lastPercent = LOWORD(wp);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

std::wstring sample(const char* name) { return (g_samples / name).wstring(); }

HWND loadFile(const std::wstring& path, int flags = 0) {
    std::wstring p = path;
    HWND w = api.load(g_host, p.data(), flags);
    if (w) {
        RECT rc;
        GetClientRect(g_host, &rc);
        MoveWindow(w, 0, 0, rc.right, rc.bottom, TRUE);
        pump(20);
    }
    return w;
}

HWND grid(HWND plugin) { return FindWindowExW(plugin, nullptr, L"TclCsvVw_Grid", nullptr); }

void paint(HWND plugin) {
    HWND g = grid(plugin);
    InvalidateRect(g, nullptr, FALSE);
    UpdateWindow(g);
    pump(5);
}

std::wstring clipboard() {
    std::wstring s;
    if (!OpenClipboard(g_host)) return s;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            s = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return s;
}

void clearClipboard() {
    if (OpenClipboard(g_host)) {
        EmptyClipboard();
        CloseClipboard();
    }
}

// Optional screenshots of the host window: set CSVVW_SMOKE_SHOTS=<dir>.
void shot(const char* name) {
    wchar_t dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"CSVVW_SMOKE_SHOTS", dir, MAX_PATH)) return;
    pump(30);
    RECT rc;
    GetClientRect(g_host, &rc);
    int w = rc.right, h = rc.bottom;
    HDC src = GetDC(g_host);
    HDC mem = CreateCompatibleDC(src);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(src, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(g_host, mem, PW_CLIENTONLY);
    BitBlt(mem, 0, 0, w, h, src, 0, 0, SRCCOPY);
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(w * h * 4);
    bi.bmiHeader.biHeight = h;  // bottom-up file
    std::ofstream f(fs::path(dir) / (std::string(name) + ".bmp"), std::ios::binary);
    f.write(reinterpret_cast<const char*>(&fh), sizeof fh);
    f.write(reinterpret_cast<const char*>(&bi.bmiHeader), sizeof bi.bmiHeader);
    for (int y = h - 1; y >= 0; --y) f.write(static_cast<const char*>(bits) + static_cast<size_t>(y) * w * 4, w * 4);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(g_host, src);
}

// Renders the grid via WM_PRINTCLIENT and counts pixel columns that contain
// dark ("ink") pixels. Catches text drawn with wrong glyph advances (all
// characters stacked on one spot), independent of the screen.
int inkColumns(HWND plugin) {
    HWND g = grid(plugin);
    RECT rc;
    GetClientRect(g, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return 0;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    SendMessageW(g, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT);
    GdiFlush();
    int count = 0;
    const auto* px = static_cast<const uint32_t*>(bits);
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
            uint32_t c = px[static_cast<size_t>(y) * w + x];
            int r = (c >> 16) & 0xFF, gg = (c >> 8) & 0xFF, b = c & 0xFF;
            if ((r * 30 + gg * 59 + b * 11) / 100 < 90) {
                ++count;
                break;
            }
        }
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return count;
}

size_t countLines(const std::wstring& s) {
    if (s.empty()) return 0;
    size_t n = 1;
    for (size_t i = 0; i + 1 < s.size(); ++i)
        if (s[i] == L'\r' && s[i + 1] == L'\n') ++n;
    return n;
}

std::wstring firstLine(const std::wstring& s) { return s.substr(0, s.find(L"\r\n")); }

std::string readIni() {
    std::ifstream f(g_ini, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

void restoreIni() {
    std::ofstream f(g_ini, std::ios::binary | std::ios::trunc);
    f << g_iniBackup;
}

void menu(HWND plugin, int id) {
    SendMessageW(plugin, WM_COMMAND, MAKEWPARAM(id, 0), 0);
    pump(5);
}

// Header click at the middle of the first column's header.
void clickHeader(HWND plugin, int x) {
    HWND g = grid(plugin);
    SendMessageW(g, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, 5));
    SendMessageW(g, WM_LBUTTONUP, 0, MAKELPARAM(x, 5));
    pump(5);
}

std::wstring copySelection(HWND plugin, bool all) {
    clearClipboard();
    if (all) api.command(plugin, lc_selectall, 0);
    api.command(plugin, lc_copy, 0);
    return clipboard();
}

// --------------------------------------------------------------------- tests

void testDetect() {
    section("detect string");
    char buf[512] = {};
    api.detect(buf, sizeof buf);
    check(std::string(buf) == "EXT=\"CSV\" | EXT=\"TSV\" | EXT=\"TAB\"", std::string("detect string: ") + buf);
    char small[12] = {};
    api.detect(small, sizeof small);
    check(std::strlen(small) < sizeof small, "detect string truncated safely");
}

void testRefuse() {
    section("unmapped extension");
    auto tmp = fs::temp_directory_path() / "csvvw_smoke.json";
    std::ofstream(tmp) << "{}";
    check(loadFile(tmp.wstring()) == nullptr, "json file refused");
    HWND w = loadFile(tmp.wstring(), lcp_forceshow);
    check(w != nullptr, "forceshow shows any file");
    if (w) api.close(w);
    fs::remove(tmp);
}

void testUeCvars() {
    section("UE_cvars.csv");
    HWND w = loadFile(sample("UE_cvars.csv"));
    check(w != nullptr, "loaded");
    if (!w) return;
    check(grid(w) != nullptr, "grid child exists");
    paint(w);
    shot("ue_cvars");
    menu(w, IDM_AUTOSIZE_TYPICAL);
    paint(w);
    shot("ue_typical");
    menu(w, IDM_AUTOSIZE);
    int ink = inkColumns(w);
    std::printf("  ink columns: %d\n", ink);
    check(ink > 250, "text is drawn with proper glyph advances (ink columns " + std::to_string(ink) + ")");

    std::wstring all = copySelection(w, true);
    check(countLines(all) == 7034, "select all + copy: 7034 data rows, got " + std::to_string(countLines(all)));
    check(firstLine(all).rfind(L"a.AnimNode.AimOffsetLookAt.Debug\t0\tConstructor", 0) == 0, "first row as TSV");

    // search: TC's F7 dialog
    std::wstring needle = L"r.nanite.visualizeoverview";
    check(api.search(w, needle.data(), lcs_findfirst) == LISTPLUGIN_OK, "search finds (case-insensitive)");
    clearClipboard();
    api.command(w, lc_copy, 0);
    check(clipboard() == L"r.Nanite.VisualizeOverview", "found cell is selected (copy = single cell raw)");
    std::wstring miss = L"zzz-not-there";
    check(api.search(w, miss.data(), lcs_findfirst) == LISTPLUGIN_ERROR, "search miss");
    std::wstring mc = L"R.NANITE.VISUALIZEOVERVIEW";
    check(api.search(w, mc.data(), lcs_findfirst | lcs_matchcase) == LISTPLUGIN_ERROR, "match case");
    paint(w);
    shot("ue_search_hit");
    std::wstring ctor = L"Constructor";
    check(api.search(w, ctor.data(), lcs_findfirst | lcs_backwards) == LISTPLUGIN_OK, "backwards search");

    // percent
    g_lastPercent = -1;
    api.command(w, lc_setpercent, 50);
    pump(20);
    check(g_lastPercent >= 45 && g_lastPercent <= 55, "lc_setpercent 50 -> reported " + std::to_string(g_lastPercent));
    paint(w);

    // header click sorting (NAME column): asc, desc, original
    clickHeader(w, 60);
    std::wstring asc = copySelection(w, true);
    clickHeader(w, 60);
    std::wstring desc = copySelection(w, true);
    clickHeader(w, 60);
    std::wstring orig = copySelection(w, true);
    clickHeader(w, 60);
    clickHeader(w, 60);  // desc again for the picture
    paint(w);
    shot("ue_sorted_desc");
    clickHeader(w, 60);
    check(firstLine(desc) != firstLine(asc), "descending differs from ascending");
    check(orig == all, "third click restores the original order");
    std::printf("  asc first: %ls\n  desc first: %ls\n", firstLine(asc).substr(0, 60).c_str(),
                firstLine(desc).substr(0, 60).c_str());
    check(CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, firstLine(asc).c_str(), -1,
                          firstLine(desc).c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN,
          "asc first row sorts before desc first row (locale, case-insensitive)");
    paint(w);
    api.close(w);
}

void testModes() {
    section("display modes");
    HWND w = loadFile(sample("multiline.csv"));
    check(w != nullptr, "multiline.csv loaded");
    if (!w) return;
    paint(w);
    shot("multiline_default");
    const char* names[] = {"m1_multiline", "m2_wrap", "m3_freeze", "m4_norownum", "m5_fit", "m6_nostatus", "m7_noautosize"};
    int k = 0;
    for (int id : {IDM_MULTILINE, IDM_LAST_WRAP, IDM_FREEZE, IDM_ROWNUM, IDM_LAST_FIT, IDM_STATUSBAR, IDM_AUTOSIZE}) {
        menu(w, id);
        paint(w);
        shot(names[k++]);
    }
    std::string ini = readIni();
    check(ini.find("multiline_cells=true") != std::string::npos, "multiline toggle saved");
    check(ini.find("last_column=fit") != std::string::npos, "last column fit saved (wrap replaced)");
    check(ini.find("freeze_first_column=true") != std::string::npos, "freeze saved");
    // undo toggles
    for (int id : {IDM_MULTILINE, IDM_LAST_FIT, IDM_FREEZE, IDM_ROWNUM, IDM_STATUSBAR, IDM_AUTOSIZE}) menu(w, id);
    check(readIni().find("auto_size_columns=largest") != std::string::npos, "auto-size back to largest");
    menu(w, IDM_AUTOSIZE_TYPICAL);
    paint(w);
    shot("m8_typical");
    check(readIni().find("auto_size_columns=typical") != std::string::npos, "auto-size typical saved");
    menu(w, IDM_AUTOSIZE);  // largest (exclusive)
    check(readIni().find("auto_size_columns=largest") != std::string::npos, "auto-size largest replaces typical");
    ini = readIni();
    check(ini.find("last_column=normal") != std::string::npos, "second click turns last-column mode off");

    // header toggle
    std::wstring before = copySelection(w, true);
    menu(w, IDM_HEADER);
    std::wstring after = copySelection(w, true);
    check(countLines(after) == countLines(before) + 1 || after.size() > before.size(), "header off adds row 0 to data");
    check(readIni().find("header_default=false") != std::string::npos, "header toggle saves header_default");
    menu(w, IDM_HEADER);

    // zoom (Ctrl+wheel) and every paint path at another zoom level
    HWND g = grid(w);
    SendMessageW(g, WM_KEYDOWN, VK_CONTROL, 0);
    for (int i = 0; i < 3; ++i) SendMessageW(g, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA), 0);
    paint(w);
    shot("zoom130");
    menu(w, IDM_RESET_ZOOM);

    // copy variants
    clearClipboard();
    menu(w, IDM_SELECTALL);
    menu(w, IDM_COPY_CSV);
    std::wstring csvText = clipboard();
    check(csvText.find(L"\"first line\r\nsecond line\"") != std::wstring::npos, "copy as CSV keeps quoting");
    clearClipboard();
    menu(w, IDM_COPY_HEADER);
    check(firstLine(clipboard()) == L"id\tname\tnotes", "copy with header");

    // keyboard navigation (no crash, selection moves)
    for (WPARAM vk : {VK_DOWN, VK_DOWN, VK_RIGHT, VK_END, VK_HOME, VK_NEXT, VK_PRIOR}) {
        SendMessageW(g, WM_KEYDOWN, vk, 0);
        SendMessageW(g, WM_KEYUP, vk, 0);
    }
    paint(w);

    // load next: another file in the same window
    std::wstring next = sample("tabs.tsv");
    check(api.loadNext(g_host, w, next.data(), 0) == LISTPLUGIN_OK, "ListLoadNext");
    std::wstring tsv = copySelection(w, true);
    check(firstLine(tsv) == L"x\t1.5\t2024-01-01", "tsv after load-next");
    paint(w);
    api.close(w);
}

void testEncodings() {
    section("encodings and delimiters");
    for (const char* name : {"utf16le_bom.csv", "utf16be_bom.csv", "cp1251.csv"}) {
        HWND w = loadFile(sample(name));
        check(w != nullptr, std::string("loaded ") + name);
        if (!w) continue;
        std::wstring t = copySelection(w, true);
        check(countLines(t) == 3, std::string(name) + ": 3 data rows");
        paint(w);
        shot(name);
        if (std::strcmp(name, "cp1251.csv") != 0)
            check(t.find(L"Москва") != std::wstring::npos, std::string(name) + ": Cyrillic");
        // override the delimiter via the menu: comma -> one column
        menu(w, IDM_DELIM_AUTO + 1);
        std::wstring one = copySelection(w, true);
        check(one.find(L'\t') == std::wstring::npos, std::string(name) + ": forced comma = single column");
        menu(w, IDM_DELIM_AUTO);
        paint(w);
        api.close(w);
    }
}

void testOversized() {
    section("oversized file");
    std::string ini = g_iniBackup;
    auto pos = ini.find("max_file_size=100M");
    if (pos != std::string::npos) ini.replace(pos, 18, "max_file_size=1K");
    {
        std::ofstream f(g_ini, std::ios::binary | std::ios::trunc);
        f << ini;
    }
    HWND w = loadFile(sample("UE_cvars.csv"));
    check(w != nullptr, "oversized file claimed with a notice");
    if (w) {
        paint(w);
        shot("oversized");
        check(copySelection(w, true).empty(), "nothing to copy");
        std::wstring s = L"x";
        check(api.search(w, s.data(), lcs_findfirst) == LISTPLUGIN_ERROR, "no search");
        api.close(w);
    }
    restoreIni();
}

void testLarge() {
    section("large file timing");
    auto tmp = fs::temp_directory_path() / "csvvw_smoke_big.csv";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << "id,name,value,date,notes\r\n";
        std::string row;
        for (long long i = 0; i < 300000; ++i) {  // 64-bit: i * 7919 must not overflow
            row = std::to_string(i) + ",name_" + std::to_string((i * 7919) % 100000) + "," +
                  std::to_string((i * 31) % 1000) + "." + std::to_string(i % 100) + ",2024-0" +
                  std::to_string(1 + i % 9) + "-1" + std::to_string(i % 9) + ",\"note, " + std::to_string(i) + "\"\r\n";
            f << row;
        }
    }
    auto size = fs::file_size(tmp);
    DWORD t0 = GetTickCount();
    HWND w = loadFile(tmp.wstring());
    DWORD t1 = GetTickCount();
    check(w != nullptr, "large file loaded");
    if (w) {
        paint(w);
        DWORD t2 = GetTickCount();
        clickHeader(w, 60);  // sort by id (number)
        DWORD t3 = GetTickCount();
        std::wstring s = L"no-such-text";
        api.search(w, s.data(), lcs_findfirst);
        DWORD t4 = GetTickCount();
        std::printf("  %.1f MB: open %lu ms, sort %lu ms, search miss %lu ms\n", size / 1048576.0,
                    static_cast<unsigned long>(t1 - t0), static_cast<unsigned long>(t3 - t2),
                    static_cast<unsigned long>(t4 - t3));
        // wrap mode on a big file (estimated heights) must paint and scroll
        menu(w, IDM_LAST_WRAP);
        api.command(w, lc_setpercent, 70);
        paint(w);
        shot("big_wrap");
        menu(w, IDM_LAST_WRAP);
        api.close(w);
    }
    fs::remove(tmp);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: plugin_smoke <plugin.wlx64> <samples-dir>\n");
        return 2;
    }
    fs::path dll = argv[1];
    g_samples = argv[2];
    g_pluginDir = dll.parent_path();
    g_ini = g_pluginDir / (dll.stem().wstring() + L".ini");
    {
        std::ifstream f(g_ini, std::ios::binary);
        g_iniBackup.assign(std::istreambuf_iterator<char>(f), {});
    }
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) {
        std::printf("cannot load %ls (error %lu)\n", dll.c_str(), GetLastError());
        return 2;
    }
    api.load = proc<LoadW_t>(m, "ListLoadW");
    api.loadNext = proc<LoadNextW_t>(m, "ListLoadNextW");
    api.close = proc<Close_t>(m, "ListCloseWindow");
    api.detect = proc<Detect_t>(m, "ListGetDetectString");
    api.search = proc<SearchW_t>(m, "ListSearchTextW");
    api.command = proc<Command_t>(m, "ListSendCommand");
    if (!api.load || !api.loadNext || !api.close || !api.detect || !api.search || !api.command) {
        std::printf("missing exports\n");
        return 2;
    }
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = hostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TLister";
    RegisterClassExW(&wc);
    g_host = CreateWindowExW(0, L"TLister", L"smoke", WS_OVERLAPPEDWINDOW, 0, 0, 1000, 700, nullptr, nullptr,
                             wc.hInstance, nullptr);
    ShowWindow(g_host, SW_SHOWNOACTIVATE);

    testDetect();
    testRefuse();
    testUeCvars();
    testModes();
    testEncodings();
    testOversized();
    testLarge();

    restoreIni();
    DestroyWindow(g_host);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
