#include "ListerWindow.h"

#include <windowsx.h>

#include <algorithm>

#include "core/IniDocument.h"
#include "core/OptionSchema.h"
#include "core/Settings.h"
#include "core/TextUtil.h"
#include "plugin/DebugLog.h"
#include "plugin/WinUtil.h"
#include "plugin/listplug.h"
#include "ui/ConfigDialog.h"
#include "ui/DarkMode.h"
#include "ui/resource.h"

namespace csvvw::view {

namespace {
const wchar_t kContainerClass[] = L"TclCsvVw_Lister";
const wchar_t kPropSelf[] = L"TclCsvVw_Self";

bool registerContainerClass(WNDPROC proc) {
    static ATOM atom = 0;
    if (atom) return true;
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = win::g_instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kContainerClass;
    atom = RegisterClassExW(&wc);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

std::wstring classNameOf(HWND h) {
    wchar_t buf[128] = {};
    GetClassNameW(h, buf, 127);
    return buf;
}

std::wstring withThousands(size_t n) {
    std::wstring s = std::to_wstring(static_cast<unsigned long long>(n));
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), L",");
    return s;
}

// Windows sort key: locale-aware, case-insensitive, digits as numbers.
std::string windowsSortKey(std::u16string_view s) {
    if (s.empty()) return {};
    const wchar_t* src = reinterpret_cast<const wchar_t*>(s.data());
    int len = static_cast<int>(std::min<size_t>(s.size(), 4096));
    DWORD flags = LCMAP_SORTKEY | LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS;
    int n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, src, len, nullptr, 0, nullptr, nullptr, 0);
    if (n <= 0) {
        flags = LCMAP_SORTKEY | NORM_IGNORECASE;
        n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, src, len, nullptr, 0, nullptr, nullptr, 0);
        if (n <= 0) return csv::defaultSortKey(s);
    }
    std::string key(static_cast<size_t>(n), '\0');
    LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, src, len, reinterpret_cast<LPWSTR>(key.data()), n, nullptr, nullptr,
                  0);
    if (!key.empty() && key.back() == '\0') key.pop_back();
    return key;
}

bool isWordCharW(wchar_t c) { return c == L'_' || IsCharAlphaNumericW(c); }

// Case-insensitive search with the user's locale (FindNLSStringEx).
csv::Matcher windowsMatcher(const std::wstring& needle, bool matchCase, bool wholeWords) {
    if (matchCase) return csv::defaultMatcher(std::u16string(needle.begin(), needle.end()), true, wholeWords);
    // Fast path: an ASCII needle in an ASCII cell needs no linguistic search.
    bool asciiNeedle = std::all_of(needle.begin(), needle.end(), [](wchar_t c) { return c < 0x80; });
    csv::Matcher ascii = asciiNeedle ? csv::defaultMatcher(std::u16string(needle.begin(), needle.end()), false, wholeWords)
                                     : csv::Matcher();
    return [needle, wholeWords, ascii](std::u16string_view hay, size_t from, bool backward, size_t& pos, size_t& len) {
        if (ascii && std::all_of(hay.begin(), hay.end(), [](char16_t c) { return c < 0x80; }))
            return ascii(hay, from, backward, pos, len);
        const wchar_t* h = reinterpret_cast<const wchar_t*>(hay.data());
        auto boundaryOk = [&](size_t p, size_t l) {
            if (!wholeWords) return true;
            if (p > 0 && isWordCharW(h[p - 1]) && isWordCharW(needle.front())) return false;
            if (p + l < hay.size() && isWordCharW(h[p + l]) && isWordCharW(needle.back())) return false;
            return true;
        };
        if (!backward) {
            size_t start = from;
            while (start < hay.size()) {
                int found = 0;
                int idx = FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE, h + start,
                                          static_cast<int>(hay.size() - start), needle.c_str(),
                                          static_cast<int>(needle.size()), &found, nullptr, nullptr, 0);
                if (idx < 0) return false;
                size_t p = start + static_cast<size_t>(idx);
                size_t l = static_cast<size_t>(std::max(found, 1));
                if (boundaryOk(p, l)) {
                    pos = p;
                    len = l;
                    return true;
                }
                start = p + 1;
            }
            return false;
        }
        // Last match starting before `from`: search [0, end) from the end; a
        // match that starts too late or fails the word test shrinks the range
        // so that it can no longer be found.
        size_t end = std::min(hay.size(), from + needle.size() - 1);
        while (end > 0) {
            int found = 0;
            int idx = FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMEND | LINGUISTIC_IGNORECASE, h,
                                      static_cast<int>(end), needle.c_str(), static_cast<int>(needle.size()), &found,
                                      nullptr, nullptr, 0);
            if (idx < 0) return false;
            size_t p = static_cast<size_t>(idx);
            size_t l = static_cast<size_t>(std::max(found, 1));
            if (p < from && boundaryOk(p, l)) {
                pos = p;
                len = l;
                return true;
            }
            size_t next = p + l - 1;
            if (next >= end) next = end - 1;
            end = next;
        }
        return false;
    };
}
}  // namespace

// ====================================================================== creation

ListerWindow* ListerWindow::fromHwnd(HWND h) {
    if (!h) return nullptr;
    return static_cast<ListerWindow*>(GetPropW(h, kPropSelf));
}

ListerWindow* ListerWindow::create(HWND parent, const std::wstring& file, int showFlags) {
    {
        IniDocument doc;
        dbg::configure(IniDocument::loadFile(win::iniPath(), doc, win::ansiConverter()) &&
                       Config::fromIni(doc).debugLog);
    }
    if (!registerContainerClass(&ListerWindow::containerProc)) {
        dbg::log("register container class failed: %lu", GetLastError());
        return nullptr;
    }
    auto* w = new ListerWindow();
    if (!w->createWindows(parent)) {
        dbg::log("createWindows failed: %lu", GetLastError());
        if (w->hwnd_) DestroyWindow(w->hwnd_);
        else delete w;
        return nullptr;
    }
    if (!w->load(file, showFlags)) {
        DestroyWindow(w->hwnd_);  // deletes w in WM_NCDESTROY
        return nullptr;
    }
    ShowWindow(w->hwnd_, SW_SHOW);
    if (!w->quickView_) SetFocus(w->grid_.hwnd());
    return w;
}

bool ListerWindow::createWindows(HWND parent) {
    lister_ = parent;
    quickView_ = classNameOf(parent) != L"TLister";
    RECT rc{};
    GetClientRect(parent, &rc);
    hwnd_ = CreateWindowExW(0, kContainerClass, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, rc.right, rc.bottom, parent,
                            nullptr, win::g_instance, nullptr);
    if (!hwnd_) return false;
    SetPropW(hwnd_, kPropSelf, this);
    if (!info_.create(hwnd_)) {
        dbg::log("info bar creation failed");
        return false;
    }
    info_.onClosed = [this] { layout(); };
    if (!grid_.create(hwnd_)) {
        dbg::log("grid creation failed: %lu", GetLastError());
        return false;
    }
    if (!status_.create(hwnd_)) {
        dbg::log("status bar creation failed: %lu", GetLastError());
        return false;
    }
    grid_.setSortKeyFn(&windowsSortKey);
    grid_.setMatcherFactory(&windowsMatcher);
    grid_.onStateChanged = [this] { stateChanged(); };
    grid_.onContextMenu = [this](POINT pt, long col) { showContextMenu(pt, col); };
    grid_.onForwardKey = [this](UINT msg, WPARAM wp, LPARAM lp) {
        // Lister subclasses the plugin window (our container) to catch its hotkeys.
        PostMessageW(hwnd_, msg, wp, lp);
    };
    grid_.onShortcut = [this](int cmd) { runCommand(cmd, -1); };
    return true;
}

// ====================================================================== loading

bool ListerWindow::load(const std::wstring& file, int showFlags) {
    bool same = !file_.empty() && file == file_;
    if (!same) {
        delimOverride_.reset();
        encOverride_.reset();
        headerOverride_.reset();
    }
    return loadInternal(file, showFlags, same);
}

GridOptions ListerWindow::gridOptions() const {
    GridOptions o;
    o.fontFace = win::widen(cfg_.fontFace);
    o.fontSize = cfg_.fontSize;
    o.zoom = zoom_;
    o.autoSize = cfg_.autoSizeColumns;
    o.maxAutoWidth = cfg_.maxAutoWidth;
    o.minColumnWidth = cfg_.minColumnWidth;
    o.defaultColumnWidth = cfg_.defaultColumnWidth;
    o.maxFitWidth = cfg_.maxFitWidth;
    o.lastColumn = cfg_.lastColumn;
    o.multiline = cfg_.multilineCells;
    o.maxCellLines = cfg_.maxCellLines;
    o.rowNumbers = cfg_.showRowNumbers;
    o.freezeFirst = cfg_.freezeFirstColumn;
    o.zebra = cfg_.zebraRows;
    o.alignNumbersRight = cfg_.alignNumbersRight;
    o.maxCellChars = static_cast<size_t>(cfg_.maxCellChars);
    bool dark = cfg_.useDark(dark_);
    o.colors = cfg_.palette(dark);
    o.dark = dark;
    return o;
}

bool ListerWindow::loadInternal(const std::wstring& file, int showFlags, bool sameFile) {
    namespace fs = std::filesystem;
    const auto& ansi = win::ansiConverter();
    IniDocument doc;
    Config cfg = IniDocument::loadFile(win::iniPath(), doc, ansi) ? Config::fromIni(doc) : Config::defaults();
    dbg::configure(cfg.debugLog);
    dbg::log("load %s flags=0x%x quickView=%d same=%d", win::narrow(file).c_str(), showFlags, quickView_ ? 1 : 0,
             sameFile ? 1 : 0);
    const bool forced = (showFlags & lcp_forceshow) != 0;
    if (!cfg.handlesExtension(fs::path(file)) && !forced) return false;

    uint64_t size = 0;
    if (!win::fileSize(file, size)) return false;
    std::shared_ptr<csv::Table> table;
    bool oversized = size > cfg.maxFileSize;
    if (!oversized) {
        std::string bytes;
        if (!win::readFile(file, bytes, size)) return false;
        csv::LoadOptions lo;
        lo.encoding = encOverride_.value_or(cfg.encoding);
        lo.delimiter = delimOverride_.value_or(cfg.delimiter);
        lo.headerMode = headerOverride_ ? (*headerOverride_ ? csv::HeaderMode::On : csv::HeaderMode::Off)
                                        : cfg.headerMode;
        lo.headerDefault = cfg.headerDefault;
        auto ext = toLowerAscii(pathToUtf8(fs::path(file).extension()));
        lo.preferTab = ext == ".tsv" || ext == ".tab";
        DWORD t0 = GetTickCount();
        table = std::shared_ptr<csv::Table>(csv::loadTable(std::move(bytes), lo, ansi));
        dbg::log("parsed %llu rows x %llu cols in %lu ms", static_cast<unsigned long long>(table->fileRows()),
                 static_cast<unsigned long long>(table->columns), static_cast<unsigned long>(GetTickCount() - t0));
    }

    // --- commit ---
    cfg_ = std::move(cfg);
    file_ = file;
    showFlags_ = showFlags;
    dark_ = (showFlags & lcp_darkmode) != 0;
    oversized_ = oversized;
    table_ = table;
    if (!zoomInit_) {
        zoom_ = cfg_.rememberZoom ? cfg_.zoom : 100;
        zoomInit_ = true;
    }
    std::wstring notice;
    if (oversized) {
        notice = L"File is " + win::widen(formatSize(size)) + L"; tcl-csvvw shows files up to " +
                 win::widen(formatSize(cfg_.maxFileSize)) +
                 L" (max_file_size). Press 1-3 to switch to another viewer.";
    } else if (table_->columnsClamped) {
        notice = L"Rows with more than " + std::to_wstring(csv::kMaxColumns) +
                 L" fields: the remaining fields are shown in the last column.";
    }
    info_.setText(notice);
    status_.show(cfg_.showStatusBar);
    applyColors();
    layout();
    grid_.setTable(table_, gridOptions());
    ui::dark::allowForWindow(hwnd_, cfg_.useDark(dark_));
    ui::dark::allowForWindow(grid_.hwnd(), cfg_.useDark(dark_));
    lastPercent_ = -1;
    stateChanged();
    return true;
}

void ListerWindow::applyColors() {
    Palette p = cfg_.palette(cfg_.useDark(dark_));
    info_.setColors(win::toColorRef(p.infoBackground), win::toColorRef(p.infoText));
    status_.setColors(win::toColorRef(p.headerBackground), win::toColorRef(p.headerText),
                      win::toColorRef(p.gridLines));
}

void ListerWindow::layout() {
    if (!hwnd_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right, h = rc.bottom;
    int ih = info_.height();
    int sh = status_.height();
    info_.move(0, 0, w);
    MoveWindow(grid_.hwnd(), 0, ih, w, std::max(0, h - ih - sh), TRUE);
    status_.move(0, std::max(0, h - sh), w);
}

// ====================================================================== status

void ListerWindow::stateChanged() {
    int z = grid_.zoom();
    if (z != zoom_) zoom_ = z;
    updateStatus();
    reportPercent();
}

void ListerWindow::updateStatus() {
    if (!status_.visible()) return;
    std::vector<std::wstring> parts;
    if (!table_) {
        parts.push_back(oversized_ ? L"File too large" : L"");
        status_.setParts(std::move(parts));
        return;
    }
    const auto& t = *table_;
    parts.push_back(withThousands(t.dataRows()) + L" rows × " + withThousands(t.columns) + L" cols");
    parts.push_back(L"Delimiter: " + win::widen(csv::delimiterLabel(t.delimiter)) +
                    (t.delimiterDetected ? L" (auto)" : L""));
    parts.push_back(win::widen(encodingName(t.encoding)) + (encOverride_ ? L" (forced)" : L""));
    std::wstring hdr = t.header ? L"Header: yes" : L"Header: no";
    if (headerOverride_) hdr += L" (menu)";
    else if (cfg_.headerMode != csv::HeaderMode::Auto) hdr += L" (forced)";
    else hdr += t.headerDetect == csv::HeaderDetect::Inconclusive ? L" (default)" : L" (auto)";
    parts.push_back(hdr);
    if (grid_.sortDir() != csv::SortDir::None && grid_.sortColumn() < t.columns) {
        auto cap = win::widen(t.caption(grid_.sortColumn()));
        if (cap.size() > 40) cap = cap.substr(0, 40) + L"…";
        parts.push_back(L"Sorted: " + cap + (grid_.sortDir() == csv::SortDir::Asc ? L" ▲" : L" ▼"));
    }
    size_t sr = grid_.selectedRows(), sc = grid_.selectedCols();
    if (sr * sc > 1) parts.push_back(L"Sel: " + withThousands(sr) + L" × " + withThousands(sc));
    if (zoom_ != 100) parts.push_back(L"Zoom " + std::to_wstring(zoom_) + L"%");
    status_.setParts(std::move(parts));
}

void ListerWindow::reportPercent() {
    int pct = grid_.percent();
    if (pct == lastPercent_) return;
    lastPercent_ = pct;
    if (lister_) PostMessageW(lister_, WM_COMMAND, MAKEWPARAM(pct, itm_percent), reinterpret_cast<LPARAM>(hwnd_));
}

// ====================================================================== TC commands

int ListerWindow::search(const std::wstring& text, int flags) {
    return grid_.search(text, flags) ? LISTPLUGIN_OK : LISTPLUGIN_ERROR;
}

int ListerWindow::command(int cmd, int param) {
    switch (cmd) {
        case lc_copy:
            copy(csv::CopyFormat::Tsv, false);
            return LISTPLUGIN_OK;
        case lc_selectall:
            grid_.selectAll();
            return LISTPLUGIN_OK;
        case lc_setpercent:
            grid_.setPercent(param);
            return LISTPLUGIN_OK;
        case lc_newparams: {
            bool dark = (param & lcp_darkmode) != 0;
            showFlags_ = param | (showFlags_ & lcp_forceshow);
            if (dark != dark_) {
                dark_ = dark;
                applyColors();
                grid_.setOptions(gridOptions());
                ui::dark::allowForWindow(hwnd_, cfg_.useDark(dark_));
                ui::dark::allowForWindow(grid_.hwnd(), cfg_.useDark(dark_));
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            return LISTPLUGIN_OK;
        }
    }
    return LISTPLUGIN_ERROR;
}

void ListerWindow::copy(csv::CopyFormat fmt, bool withHeader) {
    auto text = grid_.copyText(fmt, withHeader);
    if (!text.empty()) win::copyToClipboard(hwnd_, text);
}

// ====================================================================== menus

void ListerWindow::showContextMenu(POINT pt, long headerCol) {
    const bool hasTable = table_ != nullptr;
    const bool cells = grid_.hasCells();
    auto flag = [](bool on) { return static_cast<UINT>(on ? MF_CHECKED : MF_UNCHECKED); };
    auto enabled = [](bool on) { return static_cast<UINT>(on ? MF_ENABLED : MF_GRAYED); };
    HMENU m = CreatePopupMenu();
    if (headerCol >= 0 && hasTable) {
        size_t col = static_cast<size_t>(headerCol);
        auto dir = grid_.sortColumn() == col ? grid_.sortDir() : csv::SortDir::None;
        AppendMenuW(m, MF_STRING | (dir == csv::SortDir::Asc ? MF_CHECKED : 0), IDM_SORT_ASC, L"Sort &ascending");
        AppendMenuW(m, MF_STRING | (dir == csv::SortDir::Desc ? MF_CHECKED : 0), IDM_SORT_DESC, L"Sort &descending");
        AppendMenuW(m, MF_STRING | enabled(grid_.sortDir() != csv::SortDir::None), IDM_SORT_NONE, L"&Original order");
        HMENU as = CreatePopupMenu();
        csv::SortAs cur = grid_.sortAs(col);
        const wchar_t* typeName = L"text";
        switch (table_->columnType(col)) {
            case csv::ValueKind::Number: typeName = L"number"; break;
            case csv::ValueKind::Date: typeName = L"date"; break;
            default: break;
        }
        std::wstring autoLabel = std::wstring(L"&Auto (") + typeName + L")";
        AppendMenuW(as, MF_STRING | (cur == csv::SortAs::Auto ? MF_CHECKED : 0), IDM_SORTAS_AUTO, autoLabel.c_str());
        AppendMenuW(as, MF_STRING | (cur == csv::SortAs::Text ? MF_CHECKED : 0), IDM_SORTAS_TEXT, L"&Text");
        AppendMenuW(as, MF_STRING | (cur == csv::SortAs::Number ? MF_CHECKED : 0), IDM_SORTAS_NUMBER, L"&Number");
        AppendMenuW(as, MF_STRING | (cur == csv::SortAs::Date ? MF_CHECKED : 0), IDM_SORTAS_DATE, L"Da&te");
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(as), L"Sort a&s");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_FIT_COLUMN, L"&Fit column");
        AppendMenuW(m, MF_STRING, IDM_FIT_ALL, L"Fit a&ll columns");
        AppendMenuW(m, MF_STRING | enabled(cells), IDM_SELECT_COLUMN, L"Se&lect column");
    } else {
        AppendMenuW(m, MF_STRING | enabled(cells), IDM_COPY, L"&Copy\tCtrl+C");
        AppendMenuW(m, MF_STRING | enabled(cells), IDM_COPY_CSV, L"Copy as CS&V\tCtrl+Shift+C");
        AppendMenuW(m, MF_STRING | enabled(cells && hasTable && table_->header), IDM_COPY_HEADER,
                    L"Copy &with header");
        AppendMenuW(m, MF_STRING | enabled(cells), IDM_SELECTALL, L"Select &all\tCtrl+A");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        HMENU dm = CreatePopupMenu();
        const char delims[] = {0, ',', ';', '\t', '|'};
        const wchar_t* dlabels[] = {L"&Auto", L"&Comma ( , )", L"&Semicolon ( ; )", L"&Tab", L"&Pipe ( | )"};
        char curDelim = delimOverride_.value_or(cfg_.delimiter);
        for (int i = 0; i < 5; ++i)
            AppendMenuW(dm, MF_STRING | flag(curDelim == delims[i]) | MFT_RADIOCHECK, IDM_DELIM_AUTO + i, dlabels[i]);
        AppendMenuW(m, MF_POPUP | enabled(hasTable), reinterpret_cast<UINT_PTR>(dm), L"&Delimiter");
        HMENU em = CreatePopupMenu();
        const EncodingChoice encs[] = {EncodingChoice::Auto, EncodingChoice::Utf8, EncodingChoice::Utf16LE,
                                       EncodingChoice::Utf16BE, EncodingChoice::Ansi};
        const wchar_t* elabels[] = {L"&Auto", L"UTF-&8", L"UTF-16 &LE", L"UTF-16 &BE", L"A&NSI"};
        EncodingChoice curEnc = encOverride_.value_or(cfg_.encoding);
        for (int i = 0; i < 5; ++i)
            AppendMenuW(em, MF_STRING | flag(curEnc == encs[i]) | MFT_RADIOCHECK, IDM_ENC_AUTO + i, elabels[i]);
        AppendMenuW(m, MF_POPUP | enabled(hasTable), reinterpret_cast<UINT_PTR>(em), L"&Encoding");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | enabled(hasTable) | flag(hasTable && table_->header), IDM_HEADER,
                    L"First row is &header");
        AppendMenuW(m, MF_STRING | flag(cfg_.multilineCells), IDM_MULTILINE, L"Show line &breaks in cells");
        AppendMenuW(m, MF_STRING | flag(cfg_.autoSizeColumns), IDM_AUTOSIZE, L"A&uto-size columns");
        HMENU lm = CreatePopupMenu();
        AppendMenuW(lm, MF_STRING | flag(cfg_.lastColumn == LastColumnMode::Fit), IDM_LAST_FIT,
                    L"&Fully-fit last column");
        AppendMenuW(lm, MF_STRING | flag(cfg_.lastColumn == LastColumnMode::Wrap), IDM_LAST_WRAP,
                    L"&Wrap text in last column");
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(lm), L"&Last column");
        AppendMenuW(m, MF_STRING | flag(cfg_.showRowNumbers), IDM_ROWNUM, L"&Row numbers");
        AppendMenuW(m, MF_STRING | flag(cfg_.freezeFirstColumn), IDM_FREEZE, L"&Freeze first column");
        AppendMenuW(m, MF_STRING | flag(cfg_.showStatusBar), IDM_STATUSBAR, L"S&tatus bar");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | enabled(grid_.sortDir() != csv::SortDir::None), IDM_RESTORE_ORDER,
                    L"Restore &original order");
        AppendMenuW(m, MF_STRING | enabled(zoom_ != 100), IDM_RESET_ZOOM, L"Reset &zoom\tCtrl+0");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"&Settings...");
    }
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);  // also destroys submenus
    if (cmd) runCommand(cmd, headerCol);
}

void ListerWindow::toggleOption(const char* key, bool& field) {
    field = !field;
    Settings::saveGeneralValue(win::iniPath(), key, field ? "true" : "false", win::ansiConverter());
}

void ListerWindow::setLastColumn(LastColumnMode mode) {
    cfg_.lastColumn = cfg_.lastColumn == mode ? LastColumnMode::Normal : mode;
    Settings::saveGeneralValue(win::iniPath(), "last_column", getGeneralValue(cfg_, "last_column"),
                               win::ansiConverter());
    grid_.setOptions(gridOptions());
}

void ListerWindow::runCommand(int id, long headerCol) {
    const size_t col = headerCol >= 0 ? static_cast<size_t>(headerCol) : 0;
    if (id >= IDM_DELIM_AUTO && id < IDM_DELIM_AUTO + 5) {
        const char delims[] = {0, ',', ';', '\t', '|'};
        delimOverride_ = delims[id - IDM_DELIM_AUTO];
        loadInternal(file_, showFlags_, true);
        return;
    }
    if (id >= IDM_ENC_AUTO && id < IDM_ENC_AUTO + 5) {
        const EncodingChoice encs[] = {EncodingChoice::Auto, EncodingChoice::Utf8, EncodingChoice::Utf16LE,
                                       EncodingChoice::Utf16BE, EncodingChoice::Ansi};
        encOverride_ = encs[id - IDM_ENC_AUTO];
        loadInternal(file_, showFlags_, true);
        return;
    }
    switch (id) {
        case IDM_COPY: copy(csv::CopyFormat::Tsv, false); break;
        case IDM_COPY_CSV: copy(csv::CopyFormat::Csv, false); break;
        case IDM_COPY_HEADER: copy(csv::CopyFormat::Tsv, true); break;
        case IDM_SELECTALL: grid_.selectAll(); break;
        case IDM_HEADER:
            if (table_ && table_->fileRows() > 0) {
                table_->header = !table_->header;
                headerOverride_ = table_->header;
                cfg_.headerDefault = table_->header;
                Settings::saveGeneralValue(win::iniPath(), "header_default", table_->header ? "true" : "false",
                                           win::ansiConverter());
                grid_.headerChanged();
            }
            break;
        case IDM_MULTILINE:
            toggleOption("multiline_cells", cfg_.multilineCells);
            grid_.setOptions(gridOptions());
            break;
        case IDM_AUTOSIZE:
            toggleOption("auto_size_columns", cfg_.autoSizeColumns);
            grid_.setOptions(gridOptions());
            break;
        case IDM_LAST_FIT: setLastColumn(LastColumnMode::Fit); break;
        case IDM_LAST_WRAP: setLastColumn(LastColumnMode::Wrap); break;
        case IDM_ROWNUM:
            toggleOption("show_row_numbers", cfg_.showRowNumbers);
            grid_.setOptions(gridOptions());
            break;
        case IDM_FREEZE:
            toggleOption("freeze_first_column", cfg_.freezeFirstColumn);
            grid_.setOptions(gridOptions());
            break;
        case IDM_STATUSBAR:
            toggleOption("show_status_bar", cfg_.showStatusBar);
            status_.show(cfg_.showStatusBar);
            layout();
            updateStatus();
            break;
        case IDM_RESTORE_ORDER: grid_.sortBy(0, csv::SortDir::None); break;
        case IDM_RESET_ZOOM: grid_.setZoom(100); break;
        case IDM_SETTINGS: openSettings(); break;
        case IDM_SORT_ASC: grid_.sortBy(col, csv::SortDir::Asc); break;
        case IDM_SORT_DESC: grid_.sortBy(col, csv::SortDir::Desc); break;
        case IDM_SORT_NONE: grid_.sortBy(col, csv::SortDir::None); break;
        case IDM_SORTAS_AUTO: grid_.setSortAs(col, csv::SortAs::Auto); break;
        case IDM_SORTAS_TEXT: grid_.setSortAs(col, csv::SortAs::Text); break;
        case IDM_SORTAS_NUMBER: grid_.setSortAs(col, csv::SortAs::Number); break;
        case IDM_SORTAS_DATE: grid_.setSortAs(col, csv::SortAs::Date); break;
        case IDM_FIT_COLUMN: grid_.fitColumn(col); break;
        case IDM_FIT_ALL: grid_.fitAllColumns(); break;
        case IDM_SELECT_COLUMN: grid_.selectColumns(col, col); break;
    }
    updateStatus();
}

void ListerWindow::saveZoom() {
    if (!cfg_.rememberZoom || zoom_ == cfg_.zoom) return;
    if (Settings::saveGeneralValue(win::iniPath(), "zoom", std::to_string(zoom_), win::ansiConverter()))
        cfg_.zoom = zoom_;
}

void ListerWindow::openSettings() {
    if (settingsOpen_) return;
    settingsOpen_ = true;
    saveZoom();
    Settings s;
    s.load(win::iniPath(), win::ansiConverter());
    ui::ConfigDialog dlg(s, ui::ConfigContext{win::pluginDir(), cfg_.useDark(dark_)});
    HWND self = hwnd_;
    dlg.onApplied = [this, self, &s] {
        if (!IsWindow(self)) return;
        zoom_ = s.config().zoom;
        loadInternal(file_, showFlags_, true);
    };
    dlg.show(hwnd_);
    settingsOpen_ = false;
    if (IsWindow(self)) SetFocus(grid_.hwnd());
}

// ====================================================================== window procedure

LRESULT CALLBACK ListerWindow::containerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = fromHwnd(hwnd);
    if (self) {
        try {
            switch (msg) {
                case WM_SIZE: self->layout(); return 0;
                case WM_SETFOCUS: SetFocus(self->grid_.hwnd()); return 0;
                case WM_COMMAND:
                    if (lp == 0 && HIWORD(wp) == 0 && LOWORD(wp) >= IDM_COPY) {  // menu ids / tests
                        self->runCommand(LOWORD(wp), -1);
                        return 0;
                    }
                    break;
                case WM_ERASEBKGND: return 1;
                case WM_DESTROY:
                    self->saveZoom();
                    break;
                case WM_NCDESTROY: {
                    RemovePropW(hwnd, kPropSelf);
                    self->hwnd_ = nullptr;
                    delete self;
                    return DefWindowProcW(hwnd, msg, wp, lp);
                }
            }
        } catch (...) {
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace csvvw::view
