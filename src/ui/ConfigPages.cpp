// Pages of the settings window: General, View, Colors (SPECS §12).
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <set>

#include "ConfigDialog.h"
#include "DarkMode.h"
#include "core/OptionSchema.h"
#include "core/TextUtil.h"
#include "plugin/WinUtil.h"
#include "resource.h"

namespace csvvw::ui {

namespace {

// ------------------------------------------------------------------ helpers
std::wstring getText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}
std::string getTextUtf8(HWND h) { return win::narrow(getText(h)); }
void setText(HWND h, std::string_view utf8) { SetWindowTextW(h, win::widen(utf8).c_str()); }

void comboAdd(HWND combo, const std::wstring& text) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
}
void comboSelect(HWND combo, const std::wstring& text) {
    LRESULT i =
        SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(text.c_str()));
    if (i == CB_ERR) {
        LONG style = GetWindowLongW(combo, GWL_STYLE);
        if ((style & 0x3) == CBS_DROPDOWN) {
            SetWindowTextW(combo, text.c_str());
            return;
        }
        i = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
}
std::wstring comboText(HWND combo) {
    LRESULT i = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (i == CB_ERR) return getText(combo);
    LRESULT n = SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(s.data()));
    s.resize(static_cast<size_t>(n));
    return s;
}
void fillChoices(HWND combo, const char* key) {
    for (const char* c : findGeneralOption(key)->choices) comboAdd(combo, win::widen(c));
}
bool checked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void setChecked(HWND h, bool v) { SendMessageW(h, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0); }

std::vector<std::wstring> allFonts() {
    std::set<std::wstring> names;
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(
        dc, &lf,
        [](const LOGFONTW* f, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
            if (f->lfFaceName[0] != L'@') reinterpret_cast<std::set<std::wstring>*>(p)->insert(f->lfFaceName);
            return 1;
        },
        reinterpret_cast<LPARAM>(&names), 0);
    ReleaseDC(nullptr, dc);
    return {names.begin(), names.end()};
}

// Validates `text` for option `key`; returns an error message or empty.
std::wstring checkOption(const char* key, const std::string& text) {
    Config probe;
    if (setGeneralValue(probe, key, text)) return {};
    const OptionDef* o = findGeneralOption(key);
    std::wstring label = win::widen(o ? o->label : key);
    if (o && o->type == OptionType::Int)
        return label + L" must be a number between " + std::to_wstring(o->minValue) + L" and " +
               std::to_wstring(o->maxValue) + L".";
    if (o && o->type == OptionType::Char) return label + L" must be a single character.";
    if (o && o->type == OptionType::Size)
        return label + L" must be a size such as 100M, 512K or 1000000 (between " +
               win::widen(formatSizeSpec(static_cast<uint64_t>(o->minValue))) + L" and " +
               win::widen(formatSizeSpec(static_cast<uint64_t>(o->maxValue))) + L").";
    return label + L": invalid value.";
}

// ------------------------------------------------------------------ General
class GeneralPage final : public ConfigPage {
public:
    explicit GeneralPage(const ConfigContext& ctx) : ctx_(ctx) {}
    const wchar_t* title() const override { return L"General"; }
    int dialogId() const override { return IDD_PAGE_GENERAL; }

    void init() override {
        fillChoices(item(IDC_GEN_DELIM), "delimiter");
        fillChoices(item(IDC_GEN_ENCODING), "encoding");
        fillChoices(item(IDC_GEN_HEADER_MODE), "header_mode");
    }

    void load(const Config& c) override {
        loading_ = true;
        setText(item(IDC_GEN_EXTS), getGeneralValue(c, "extensions"));
        setText(item(IDC_GEN_MAXSIZE), getGeneralValue(c, "max_file_size"));
        setText(item(IDC_GEN_MAXCELL), getGeneralValue(c, "max_cell_chars"));
        comboSelect(item(IDC_GEN_DELIM), win::widen(getGeneralValue(c, "delimiter")));
        comboSelect(item(IDC_GEN_ENCODING), win::widen(getGeneralValue(c, "encoding")));
        comboSelect(item(IDC_GEN_HEADER_MODE), win::widen(getGeneralValue(c, "header_mode")));
        setChecked(item(IDC_GEN_HEADER_DEFAULT), c.headerDefault);
        setText(item(IDC_GEN_TYPE_THRESHOLD), std::to_string(c.typeThreshold));
        setText(item(IDC_GEN_HEADER_THRESHOLD), std::to_string(c.headerTypeThreshold));
        setChecked(item(IDC_GEN_DEBUGLOG), c.debugLog);
        loading_ = false;
        updateDetect();
    }

    HWND validate(std::wstring& err) override {
        struct F { int id; const char* key; };
        for (F f : {F{IDC_GEN_EXTS, "extensions"}, F{IDC_GEN_MAXSIZE, "max_file_size"},
                    F{IDC_GEN_MAXCELL, "max_cell_chars"}, F{IDC_GEN_TYPE_THRESHOLD, "type_threshold"},
                    F{IDC_GEN_HEADER_THRESHOLD, "header_type_threshold"}}) {
            err = checkOption(f.key, getTextUtf8(item(f.id)));
            if (!err.empty()) return item(f.id);
        }
        return nullptr;
    }

    void store(Config& c) override {
        setGeneralValue(c, "extensions", getTextUtf8(item(IDC_GEN_EXTS)));
        setGeneralValue(c, "max_file_size", getTextUtf8(item(IDC_GEN_MAXSIZE)));
        setGeneralValue(c, "max_cell_chars", getTextUtf8(item(IDC_GEN_MAXCELL)));
        setGeneralValue(c, "delimiter", win::narrow(comboText(item(IDC_GEN_DELIM))));
        setGeneralValue(c, "encoding", win::narrow(comboText(item(IDC_GEN_ENCODING))));
        setGeneralValue(c, "header_mode", win::narrow(comboText(item(IDC_GEN_HEADER_MODE))));
        c.headerDefault = checked(item(IDC_GEN_HEADER_DEFAULT));
        setGeneralValue(c, "type_threshold", getTextUtf8(item(IDC_GEN_TYPE_THRESHOLD)));
        setGeneralValue(c, "header_type_threshold", getTextUtf8(item(IDC_GEN_HEADER_THRESHOLD)));
        c.debugLog = checked(item(IDC_GEN_DEBUGLOG));
    }

    INT_PTR handle(UINT msg, WPARAM wp, LPARAM) override {
        if (msg != WM_COMMAND) return FALSE;
        if (LOWORD(wp) == IDC_GEN_EXTS && HIWORD(wp) == EN_CHANGE && !loading_) {
            updateDetect();
            return TRUE;
        }
        if (LOWORD(wp) == IDC_GEN_COPY_DETECT) {
            win::copyToClipboard(hwnd_, getText(item(IDC_GEN_DETECT)));
            return TRUE;
        }
        return FALSE;
    }

private:
    void updateDetect() {
        Config probe;
        if (setGeneralValue(probe, "extensions", getTextUtf8(item(IDC_GEN_EXTS))))
            setText(item(IDC_GEN_DETECT), probe.buildDetectString(1024));
        else
            setText(item(IDC_GEN_DETECT), "");
    }
    ConfigContext ctx_;
    bool loading_ = false;
};

// ------------------------------------------------------------------ View
class ViewPage final : public ConfigPage {
public:
    explicit ViewPage(const ConfigContext& ctx) : ctx_(ctx) {}
    const wchar_t* title() const override { return L"View"; }
    int dialogId() const override { return IDD_PAGE_VIEW; }

    void init() override {
        for (const auto& f : allFonts()) comboAdd(item(IDC_VIEW_FONT), f);
        fillChoices(item(IDC_VIEW_LASTCOL), "last_column");
        fillChoices(item(IDC_VIEW_AUTOSIZE), "auto_size_columns");
        auto range = [&](int spin, const char* key) {
            auto* o = findGeneralOption(key);
            SendMessageW(item(spin), UDM_SETRANGE32, static_cast<WPARAM>(o->minValue), static_cast<LPARAM>(o->maxValue));
        };
        range(IDC_VIEW_FONTSIZE_SPIN, "font_size");
        range(IDC_VIEW_ZOOM_SPIN, "zoom");
        range(IDC_VIEW_MAXLINES_SPIN, "max_cell_lines");
        SendMessageW(item(IDC_VIEW_ZOOM_SPIN), UDM_SETACCEL, 0, 0);
        UDACCEL acc{0, 10};
        SendMessageW(item(IDC_VIEW_ZOOM_SPIN), UDM_SETACCEL, 1, reinterpret_cast<LPARAM>(&acc));
    }

    void load(const Config& c) override {
        comboSelect(item(IDC_VIEW_FONT), win::widen(c.fontFace));
        SendMessageW(item(IDC_VIEW_FONTSIZE_SPIN), UDM_SETPOS32, 0, c.fontSize);
        SendMessageW(item(IDC_VIEW_ZOOM_SPIN), UDM_SETPOS32, 0, c.zoom);
        SendMessageW(item(IDC_VIEW_MAXLINES_SPIN), UDM_SETPOS32, 0, c.maxCellLines);
        setChecked(item(IDC_VIEW_REMEMBER_ZOOM), c.rememberZoom);
        comboSelect(item(IDC_VIEW_AUTOSIZE), win::widen(getGeneralValue(c, "auto_size_columns")));
        setText(item(IDC_VIEW_PERCENTILE), std::to_string(c.autoSizePercentile));
        setText(item(IDC_VIEW_MAXAUTO), std::to_string(c.maxAutoWidth));
        setText(item(IDC_VIEW_MINW), std::to_string(c.minColumnWidth));
        setText(item(IDC_VIEW_DEFW), std::to_string(c.defaultColumnWidth));
        setText(item(IDC_VIEW_MAXFIT), std::to_string(c.maxFitWidth));
        comboSelect(item(IDC_VIEW_LASTCOL), win::widen(getGeneralValue(c, "last_column")));
        setChecked(item(IDC_VIEW_MULTILINE), c.multilineCells);
        setChecked(item(IDC_VIEW_ROWNUM), c.showRowNumbers);
        setChecked(item(IDC_VIEW_FREEZE), c.freezeFirstColumn);
        setChecked(item(IDC_VIEW_STATUS), c.showStatusBar);
        setChecked(item(IDC_VIEW_ZEBRA), c.zebraRows);
        setChecked(item(IDC_VIEW_ALIGN), c.alignNumbersRight);
        setText(item(IDC_VIEW_LBFALLBACK), getGeneralValue(c, "linebreak_fallback"));
    }

    HWND validate(std::wstring& err) override {
        struct F { int id; const char* key; };
        for (F f : {F{IDC_VIEW_FONT, "font_face"}, F{IDC_VIEW_FONTSIZE, "font_size"}, F{IDC_VIEW_ZOOM, "zoom"},
                    F{IDC_VIEW_MAXAUTO, "max_auto_width"}, F{IDC_VIEW_MINW, "min_column_width"},
                    F{IDC_VIEW_DEFW, "default_column_width"}, F{IDC_VIEW_MAXFIT, "max_fit_width"},
                    F{IDC_VIEW_MAXLINES, "max_cell_lines"}, F{IDC_VIEW_PERCENTILE, "auto_size_percentile"}, F{IDC_VIEW_LBFALLBACK, "linebreak_fallback"}}) {
            err = checkOption(f.key, getTextUtf8(item(f.id)));
            if (!err.empty()) return item(f.id);
        }
        return nullptr;
    }

    void store(Config& c) override {
        setGeneralValue(c, "font_face", win::narrow(comboText(item(IDC_VIEW_FONT))));
        setGeneralValue(c, "font_size", getTextUtf8(item(IDC_VIEW_FONTSIZE)));
        setGeneralValue(c, "zoom", getTextUtf8(item(IDC_VIEW_ZOOM)));
        setGeneralValue(c, "max_auto_width", getTextUtf8(item(IDC_VIEW_MAXAUTO)));
        setGeneralValue(c, "min_column_width", getTextUtf8(item(IDC_VIEW_MINW)));
        setGeneralValue(c, "default_column_width", getTextUtf8(item(IDC_VIEW_DEFW)));
        setGeneralValue(c, "max_fit_width", getTextUtf8(item(IDC_VIEW_MAXFIT)));
        setGeneralValue(c, "max_cell_lines", getTextUtf8(item(IDC_VIEW_MAXLINES)));
        setGeneralValue(c, "last_column", win::narrow(comboText(item(IDC_VIEW_LASTCOL))));
        c.rememberZoom = checked(item(IDC_VIEW_REMEMBER_ZOOM));
        setGeneralValue(c, "auto_size_columns", win::narrow(comboText(item(IDC_VIEW_AUTOSIZE))));
        setGeneralValue(c, "auto_size_percentile", getTextUtf8(item(IDC_VIEW_PERCENTILE)));
        c.multilineCells = checked(item(IDC_VIEW_MULTILINE));
        c.showRowNumbers = checked(item(IDC_VIEW_ROWNUM));
        c.freezeFirstColumn = checked(item(IDC_VIEW_FREEZE));
        c.showStatusBar = checked(item(IDC_VIEW_STATUS));
        c.zebraRows = checked(item(IDC_VIEW_ZEBRA));
        c.alignNumbersRight = checked(item(IDC_VIEW_ALIGN));
        setGeneralValue(c, "linebreak_fallback", getTextUtf8(item(IDC_VIEW_LBFALLBACK)));
    }

private:
    ConfigContext ctx_;
};

// ------------------------------------------------------------------ Colors
class ColorsPage final : public ConfigPage {
public:
    explicit ColorsPage(const ConfigContext& ctx) : ctx_(ctx) {}
    const wchar_t* title() const override { return L"Colors"; }
    int dialogId() const override { return IDD_PAGE_COLORS; }

    void init() override {
        fillChoices(item(IDC_COL_THEME), "theme");
        HWND list = item(IDC_COL_LIST);
        for (size_t i = 0; i < colorKeys().size(); ++i)
            SendMessageW(list, LB_ADDSTRING, 0, static_cast<LPARAM>(i));
        HDC dc = GetDC(list);
        TEXTMETRICW tm{};
        HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0)));
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        ReleaseDC(list, dc);
        SendMessageW(list, LB_SETITEMHEIGHT, 0, tm.tmHeight + 6);
        SendMessageW(list, LB_SETCURSEL, 0, 0);
        editDark_ = ctx_.dark;
    }

    void load(const Config& c) override {
        light_ = c.colorsLight;
        dark_ = c.colorsDark;
        comboSelect(item(IDC_COL_THEME), win::widen(getGeneralValue(c, "theme")));
        setChecked(item(IDC_COL_LIGHT), !editDark_);
        setChecked(item(IDC_COL_DARK), editDark_);
        refresh();
    }

    void store(Config& c) override {
        setGeneralValue(c, "theme", win::narrow(comboText(item(IDC_COL_THEME))));
        c.colorsLight = light_;
        c.colorsDark = dark_;
    }

    INT_PTR handle(UINT msg, WPARAM wp, LPARAM lp) override {
        switch (msg) {
            case WM_COMMAND:
                switch (LOWORD(wp)) {
                    case IDC_COL_LIGHT:
                    case IDC_COL_DARK:
                        editDark_ = checked(item(IDC_COL_DARK));
                        refresh();
                        return TRUE;
                    case IDC_COL_LIST:
                        if (HIWORD(wp) == LBN_SELCHANGE) refresh();
                        if (HIWORD(wp) == LBN_DBLCLK) change();
                        return TRUE;
                    case IDC_COL_CHANGE: change(); return TRUE;
                    case IDC_COL_RESET:
                        if (auto* k = selectedKey()) map().erase(k->key);
                        refresh();
                        return TRUE;
                    case IDC_COL_RESET_ALL:
                        map().clear();
                        refresh();
                        return TRUE;
                }
                break;
            case WM_DRAWITEM: {
                auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
                if (di->CtlID != IDC_COL_LIST || di->itemID == static_cast<UINT>(-1)) break;
                drawItem(*di);
                SetWindowLongPtrW(hwnd_, DWLP_MSGRESULT, TRUE);
                return TRUE;
            }
        }
        return FALSE;
    }

private:
    std::map<std::string, std::string>& map() { return editDark_ ? dark_ : light_; }
    const ColorKey* selectedKey() {
        LRESULT i = SendMessageW(item(IDC_COL_LIST), LB_GETCURSEL, 0, 0);
        if (i < 0 || static_cast<size_t>(i) >= colorKeys().size()) return nullptr;
        return &colorKeys()[static_cast<size_t>(i)];
    }
    uint32_t valueOf(const ColorKey& k, bool* custom = nullptr) {
        Palette base = builtinPalette(editDark_);
        uint32_t v = base.*(k.member);
        auto it = map().find(k.key);
        uint32_t o;
        bool isCustom = it != map().end() && parseColor(it->second, o);
        if (isCustom) v = o;
        if (custom) *custom = isCustom;
        return v;
    }
    void refresh() {
        InvalidateRect(item(IDC_COL_LIST), nullptr, TRUE);
        const ColorKey* k = selectedKey();
        if (!k) return;
        bool custom = false;
        uint32_t v = valueOf(*k, &custom);
        std::wstring s = win::widen(k->key) + L"\n" + win::widen(formatColor(v)) +
                         (custom ? L" (custom)" : L" (built-in)");
        SetWindowTextW(item(IDC_COL_STATE), s.c_str());
        EnableWindow(item(IDC_COL_RESET), custom);
    }
    void change() {
        const ColorKey* k = selectedKey();
        if (!k) return;
        static COLORREF custom[16] = {};
        CHOOSECOLORW cc{sizeof cc};
        cc.hwndOwner = hwnd_;
        cc.rgbResult = win::toColorRef(valueOf(*k));
        cc.lpCustColors = custom;
        cc.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
        if (!ChooseColorW(&cc)) return;
        uint32_t rgb = (static_cast<uint32_t>(GetRValue(cc.rgbResult)) << 16) |
                       (static_cast<uint32_t>(GetGValue(cc.rgbResult)) << 8) | GetBValue(cc.rgbResult);
        map()[k->key] = formatColor(rgb);
        refresh();
    }
    void drawItem(const DRAWITEMSTRUCT& di) {
        const auto& keys = colorKeys();
        if (di.itemID >= keys.size()) return;
        const ColorKey& k = keys[di.itemID];
        bool sel = (di.itemState & ODS_SELECTED) != 0;
        COLORREF bg, fg;
        if (dark_mode()) {
            bg = sel ? dark::palette().selected : dark::palette().control;
            fg = dark::palette().text;
        } else {
            bg = GetSysColor(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW);
            fg = GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
        }
        SetDCBrushColor(di.hDC, bg);
        FillRect(di.hDC, &di.rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        bool custom = false;
        uint32_t v = valueOf(k, &custom);
        int h = di.rcItem.bottom - di.rcItem.top;
        RECT sw{di.rcItem.left + 3, di.rcItem.top + 3, di.rcItem.left + 3 + 2 * (h - 6), di.rcItem.bottom - 3};
        SetDCBrushColor(di.hDC, win::toColorRef(v));
        FillRect(di.hDC, &sw, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        FrameRect(di.hDC, &sw, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
        RECT tr = di.rcItem;
        tr.left = sw.right + 6;
        SetBkMode(di.hDC, TRANSPARENT);
        SetTextColor(di.hDC, fg);
        std::wstring label = win::widen(k.label) + (custom ? L"  •" : L"");
        DrawTextW(di.hDC, label.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (di.itemState & ODS_FOCUS) DrawFocusRect(di.hDC, &di.rcItem);
    }
    bool dark_mode() const { return dark_ui_; }

    ConfigContext ctx_;
    std::map<std::string, std::string> light_, dark_;
    bool editDark_ = false;
    bool dark_ui_ = ctx_.dark;
};

}  // namespace

std::unique_ptr<ConfigPage> createGeneralPage(const ConfigContext& ctx) { return std::make_unique<GeneralPage>(ctx); }
std::unique_ptr<ConfigPage> createViewPage(const ConfigContext& ctx) { return std::make_unique<ViewPage>(ctx); }
std::unique_ptr<ConfigPage> createColorsPage(const ConfigContext& ctx) { return std::make_unique<ColorsPage>(ctx); }

}  // namespace csvvw::ui
