#include "GridView.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "core/TextUtil.h"
#include "plugin/DebugLog.h"
#include "plugin/WinUtil.h"
#include "plugin/listplug.h"
#include "ui/resource.h"

namespace csvvw::view {

namespace {

const wchar_t kGridClass[] = L"TclCsvVw_Grid";
constexpr UINT_PTR kAutoScrollTimer = 1;
constexpr size_t kExactWrapRows = 200000;
constexpr size_t kCacheRows = 4096;

bool registerGridClass() {
    static ATOM atom = 0;
    if (atom) return true;
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = win::g_instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kGridClass;
    atom = RegisterClassExW(&wc);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

UINT dpiOf(HWND h) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (fn && h) {
        UINT d = fn(h);
        if (d) return d;
    }
    HDC dc = GetDC(nullptr);
    int d = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    return d > 0 ? static_cast<UINT>(d) : 96;
}

inline const wchar_t* W(const std::u16string& s) { return reinterpret_cast<const wchar_t*>(s.c_str()); }
inline const wchar_t* W(std::u16string_view s) { return reinterpret_cast<const wchar_t*>(s.data()); }

void fill(HDC dc, const RECT& rc, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
    RECT rc{l, t, r, b};
    fill(dc, rc, c);
}

COLORREF cr(uint32_t c) { return win::toColorRef(c); }

}  // namespace

// ====================================================================== lifetime

GridView::GridView() = default;

GridView::~GridView() {
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    destroyFonts();
    if (measureDc_) DeleteDC(measureDc_);
    if (backDc_) {
        SelectObject(backDc_, backOld_);
        DeleteObject(backBmp_);
        DeleteDC(backDc_);
    }
}

bool GridView::create(HWND parent) {
    if (!registerGridClass()) return false;
    hwnd_ = CreateWindowExW(0, kGridClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 0, 0,
                            10, 10, parent, nullptr, win::g_instance, nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&GridView::proc));
    measureDc_ = CreateCompatibleDC(nullptr);

    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr,
                               win::g_instance, nullptr);
    if (tooltip_) {
        TOOLINFOW ti{};
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = hwnd_;
        ti.uId = 1;
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 600);
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    }
    return true;
}

// ====================================================================== configuration

void GridView::setTable(std::shared_ptr<csv::Table> table, const GridOptions& opt) {
    table_ = std::move(table);
    opt_ = opt;
    cache_.clear();
    sortCol_ = SIZE_MAX;
    sortDir_ = csv::SortDir::None;
    sortAs_.clear();
    hit_.reset();
    hitCurRow_ = hitCurCol_ = -2;
    order_.resize(table_ ? table_->dataRows() : 0);
    for (size_t i = 0; i < order_.size(); ++i) order_[i] = static_cast<uint32_t>(i);
    const size_t cols = table_ ? table_->columns : 0;
    baseW_.assign(cols, opt_.defaultColumnWidth);
    manualW_.assign(cols, false);
    const size_t rows = table_ ? table_->fileRows() : 0;
    wrapLines_.assign(rows, 0);
    wrapEstimated_.assign(rows, false);
    scrollX_ = 0;
    scrollY_ = 0;
    selKind_ = SelKind::Cells;
    anchorRow_ = anchorCol_ = curRow_ = curCol_ = 0;
    updateSelectionRect();
    refreshColumnInfo();
    createFonts();
    computeMetrics();
    computeBaseWidths(false);
    relayout(false);
    if (tooltip_) SetWindowTheme(tooltip_, opt_.dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    SetWindowTheme(hwnd_, opt_.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
    notify();
}

void GridView::setOptions(const GridOptions& opt) {
    GridOptions old = opt_;
    opt_ = opt;
    const bool fontChanged = old.fontFace != opt.fontFace || old.fontSize != opt.fontSize || old.zoom != opt.zoom;
    const bool measureChanged = fontChanged || old.autoSize != opt.autoSize || old.multiline != opt.multiline ||
                                old.maxAutoWidth != opt.maxAutoWidth || old.minColumnWidth != opt.minColumnWidth ||
                                old.defaultColumnWidth != opt.defaultColumnWidth ||
                                old.maxCellChars != opt.maxCellChars;
    if (old.maxCellChars != opt.maxCellChars) cache_.clear();
    if (fontChanged || old.lastColumn != opt.lastColumn || old.maxCellLines != opt.maxCellLines ||
        old.multiline != opt.multiline || old.maxFitWidth != opt.maxFitWidth) {
        std::fill(wrapLines_.begin(), wrapLines_.end(), 0);
    }
    if (fontChanged) {
        createFonts();
    }
    computeMetrics();
    if (measureChanged) computeBaseWidths(old.fontFace == opt.fontFace && old.fontSize == opt.fontSize);
    if (old.dark != opt.dark) {
        if (tooltip_) SetWindowTheme(tooltip_, opt_.dark ? L"DarkMode_Explorer" : nullptr, nullptr);
        SetWindowTheme(hwnd_, opt_.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }
    relayout(true);
    InvalidateRect(hwnd_, nullptr, FALSE);
    notify();
}

void GridView::headerChanged() {
    if (!table_) return;
    cache_.clear();
    hit_.reset();
    order_.resize(table_->dataRows());
    for (size_t i = 0; i < order_.size(); ++i) order_[i] = static_cast<uint32_t>(i);
    refreshColumnInfo();
    if (sortDir_ != csv::SortDir::None) applySort();
    computeMetrics();
    computeBaseWidths(true);
    anchorRow_ = curRow_ = std::min<long>(curRow_, static_cast<long>(order_.size()) - 1);
    if (curRow_ < 0) anchorRow_ = curRow_ = 0;
    selKind_ = SelKind::Cells;
    updateSelectionRect();
    relayout(true);
    InvalidateRect(hwnd_, nullptr, FALSE);
    notify();
}

void GridView::refreshColumnInfo() {
    const size_t cols = table_ ? table_->columns : 0;
    captions_.assign(cols, {});
    numericCol_.assign(cols, false);
    for (size_t c = 0; c < cols; ++c) {
        auto cap = utf8ToUtf16Capped(table_->caption(c), 1024);
        captions_[c] = csv::singleLine(cap).text;
        numericCol_[c] = table_->columnType(c) == csv::ValueKind::Number;
    }
}

void GridView::setZoom(int zoom) {
    zoom = std::clamp(zoom, 50, 400);
    if (zoom == opt_.zoom) return;
    GridOptions o = opt_;
    o.zoom = zoom;
    setOptions(o);
}

// ====================================================================== geometry

double GridView::scale() const { return (opt_.zoom / 100.0) * (dpi_ / 96.0); }

int GridView::px(int base) const { return std::max(1, static_cast<int>(std::lround(base * scale()))); }

void GridView::destroyFonts() {
    if (font_) DeleteObject(font_);
    if (bold_) DeleteObject(bold_);
    font_ = bold_ = nullptr;
}

void GridView::createFonts() {
    destroyFonts();
    dpi_ = static_cast<int>(dpiOf(hwnd_));
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(opt_.fontSize * opt_.zoom, dpi_, 72 * 100);
    if (lf.lfHeight == 0) lf.lfHeight = -1;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, opt_.fontFace.c_str(), LF_FACESIZE);
    font_ = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_BOLD;
    bold_ = CreateFontIndirectW(&lf);
}

void GridView::computeMetrics() {
    HGDIOBJ old = SelectObject(measureDc_, font_);
    TEXTMETRICW tm{};
    GetTextMetricsW(measureDc_, &tm);
    lineH_ = std::max<int>(1, tm.tmHeight);
    maxCharW_ = std::max<int>(1, tm.tmMaxCharWidth);
    avgCharW_ = std::max<int>(1, tm.tmAveCharWidth);
    SIZE sz{};
    const wchar_t ell = csv::kEllipsis;
    GetTextExtentPoint32W(measureDc_, &ell, 1, &sz);
    ellipsisW_ = sz.cx;
    SIZE zero{};
    GetTextExtentPoint32W(measureDc_, L"0", 1, &zero);
    WORD glyph = 0;
    const wchar_t lb = csv::kLineBreakGlyph;
    breakGlyph_ = (GetGlyphIndicesW(measureDc_, &lb, 1, &glyph, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR ||
                   glyph == 0xFFFF)
                      ? u'\u00B6'
                      : csv::kLineBreakGlyph;
    SelectObject(measureDc_, bold_);
    TEXTMETRICW tb{};
    GetTextMetricsW(measureDc_, &tb);
    SelectObject(measureDc_, old);
    padX_ = std::max(2, static_cast<int>(std::lround(4 * scale())));
    padY_ = std::max(1, static_cast<int>(std::lround(2 * scale())));
    rowH_ = lineH_ + 2 * padY_ + 1;  // +1 grid line
    headerH_ = std::max<int>(lineH_, tb.tmHeight) + 2 * padY_ + 2;
    gutterW_ = 0;
    if (opt_.rowNumbers && table_) {
        size_t n = std::max<size_t>(table_->dataRows(), 1);
        int digits = 0;
        while (n) {
            ++digits;
            n /= 10;
        }
        gutterW_ = std::max(digits, 2) * zero.cx + 2 * padX_ + 1;
    }
}

int GridView::measureText(HFONT f, std::u16string_view s) {
    if (s.empty()) return 0;
    HGDIOBJ old = SelectObject(measureDc_, f);
    SIZE sz{};
    GetTextExtentPoint32W(measureDc_, W(s), static_cast<int>(std::min<size_t>(s.size(), 32767)), &sz);
    SelectObject(measureDc_, old);
    return sz.cx;
}

int GridView::measureFitPx(size_t col, bool withHeader) {
    int best = 0;
    if (withHeader && col < captions_.size()) {
        // sort arrow space
        best = measureText(bold_, captions_[col]) + measureText(bold_, u"▲") + padX_;
    }
    auto measureCell = [&](size_t fileRow) {
        CellText ct = cellTextUncached(fileRow, col);
        if (ct.text.empty()) return;
        int w = 0;
        if (opt_.multiline) {
            for (auto& l : csv::ownLines(ct.text)) w = std::max(w, measureText(font_, l.text));
        } else {
            w = measureText(font_, csv::singleLine(ct.text).text);
        }
        if (ct.capped) w += ellipsisW_;
        best = std::max(best, w);
    };
    if (table_ && col < table_->stats.size()) {
        for (auto& [len, row] : table_->stats[col].longest) measureCell(row);
        if (!table_->header && table_->fileRows() > 0) measureCell(0);
    }
    return best + 2 * padX_ + 1;
}

void GridView::computeBaseWidths(bool keepManual) {
    if (!table_) return;
    const double s = scale();
    for (size_t c = 0; c < table_->columns; ++c) {
        if (keepManual && manualW_[c]) continue;
        manualW_[c] = false;
        if (!opt_.autoSize) {
            baseW_[c] = std::max(opt_.minColumnWidth, opt_.defaultColumnWidth);
            continue;
        }
        int fit = static_cast<int>(std::ceil(measureFitPx(c, true) / s));
        baseW_[c] = std::clamp(fit, opt_.minColumnWidth, std::max(opt_.minColumnWidth, opt_.maxAutoWidth));
    }
}

void GridView::fitColumn(size_t col) {
    if (!table_ || col >= table_->columns) return;
    int fit = static_cast<int>(std::ceil(measureFitPx(col, true) / scale()));
    baseW_[col] = std::clamp(fit, opt_.minColumnWidth, std::max(opt_.minColumnWidth, opt_.maxFitWidth));
    manualW_[col] = true;
    relayout(true);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GridView::fitAllColumns() {
    if (!table_) return;
    computeBaseWidths(false);
    relayout(true);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GridView::layoutColumns() {
    const size_t cols = table_ ? table_->columns : 0;
    colX_.assign(cols + 1, 0);
    for (size_t c = 0; c < cols; ++c) colX_[c + 1] = colX_[c] + px(baseW_[c]);
    lastColPx_ = cols ? colWidth(cols - 1) : 0;
    if (cols == 0 || manualW_[cols - 1]) return;
    const size_t last = cols - 1;
    if (opt_.lastColumn == LastColumnMode::Fit) {
        int w = std::min(measureFitPx(last, true), px(opt_.maxFitWidth));
        w = std::max(w, px(opt_.minColumnWidth));
        colX_[cols] = colX_[last] + w;
    } else if (opt_.lastColumn == LastColumnMode::Wrap) {
        int normal = colWidth(last);
        int content = std::min(measureFitPx(last, true), px(opt_.maxFitWidth));
        int avail = clientW_ - gutterW_ - colX_[last];
        int w = std::max(normal, std::min(content, avail));
        colX_[cols] = colX_[last] + w;
    }
    lastColPx_ = colWidth(last);
}

bool GridView::variableRows() const {
    return table_ && (opt_.multiline || opt_.lastColumn == LastColumnMode::Wrap);
}

int GridView::wrapWidthPx() const { return std::max(1, lastColPx_ - 2 * padX_ - 1); }

int GridView::wrapLinesOf(size_t fileRow, bool exact) {
    if (!table_ || table_->columns == 0) return 1;
    if (table_->lastCellLines[fileRow] > 1) return 1;  // own breaks: never soft-wrapped
    CellText ct = cellTextUncached(fileRow, table_->columns - 1);
    if (ct.text.empty()) return 1;
    const int width = wrapWidthPx();
    if (static_cast<int64_t>(ct.text.size()) * maxCharW_ <= width) return 1;
    const int maxLines = std::max(1, opt_.maxCellLines);
    if (!exact) {
        int64_t est = (static_cast<int64_t>(ct.text.size()) * avgCharW_ + width - 1) / width;
        return static_cast<int>(std::clamp<int64_t>(est, 1, maxLines));
    }
    auto disp = csv::singleLine(ct.text);
    HGDIOBJ old = SelectObject(measureDc_, font_);
    auto fit = [this](std::u16string_view s, int w) -> size_t {
        int n = 0;
        SIZE sz{};
        int len = static_cast<int>(std::min<size_t>(s.size(), 32767));
        GetTextExtentExPointW(measureDc_, W(s), len, w, &n, nullptr, &sz);
        return static_cast<size_t>(std::max(n, 0));
    };
    auto r = csv::wrapText(disp.text, width, static_cast<size_t>(maxLines), fit);
    SelectObject(measureDc_, old);
    return static_cast<int>(std::max<size_t>(1, r.lines.size()));
}

int GridView::rowLinesOf(size_t displayRow) {
    size_t fr = fileRowOfDisplay(displayRow);
    int lines = 1;
    const int maxLines = std::max(1, opt_.maxCellLines);
    if (opt_.multiline) lines = std::min<int>(maxLines, table_->rowLines[fr]);
    if (opt_.lastColumn == LastColumnMode::Wrap) {
        if (wrapLines_[fr] == 0) {
            bool exact = wrapExact_;
            wrapLines_[fr] = static_cast<uint8_t>(std::min(255, wrapLinesOf(fr, exact)));
            wrapEstimated_[fr] = !exact;
        }
        lines = std::max<int>(lines, wrapLines_[fr]);
    }
    return std::max(1, lines);
}

void GridView::layoutRows() {
    if (!variableRows()) {
        rowY_.clear();
        return;
    }
    const size_t n = order_.size();
    wrapExact_ = table_->fileRows() <= kExactWrapRows;
    rowY_.assign(n + 1, 0);
    for (size_t r = 0; r < n; ++r) rowY_[r + 1] = rowY_[r] + rowLinesOf(r) * lineH_ + 2 * padY_ + 1;
}

int64_t GridView::rowTop(size_t r) const {
    return rowY_.empty() ? static_cast<int64_t>(r) * rowH_ : rowY_[std::min(r, rowY_.size() - 1)];
}

int GridView::rowHeight(size_t r) const {
    if (rowY_.empty()) return rowH_;
    if (r + 1 >= rowY_.size()) return rowH_;
    return static_cast<int>(rowY_[r + 1] - rowY_[r]);
}

int64_t GridView::totalHeight() const {
    return rowY_.empty() ? static_cast<int64_t>(order_.size()) * rowH_ : rowY_.back();
}

size_t GridView::rowAtY(int64_t y) const {
    const size_t n = order_.size();
    if (n == 0) return 0;
    if (y <= 0) return 0;
    if (rowY_.empty()) return std::min<size_t>(static_cast<size_t>(y / rowH_), n - 1);
    auto it = std::upper_bound(rowY_.begin(), rowY_.end(), y);
    size_t r = static_cast<size_t>(it - rowY_.begin());
    return r == 0 ? 0 : std::min(r - 1, n - 1);
}

int GridView::bodyHeight() const { return std::max(0, clientH_ - headerH_); }

int GridView::scrollAreaLeft() const {
    int x = gutterW_;
    if (opt_.freezeFirst && table_ && table_->columns > 1) x += colWidth(0);
    return x;
}

int GridView::colScreenX(size_t col) const {
    if (opt_.freezeFirst && col == 0 && table_ && table_->columns > 1) return gutterW_;
    return gutterW_ + colX_[col] - scrollX_;
}

int64_t GridView::maxScrollY() const {
    const int64_t body = bodyHeight();
    if (rowY_.empty()) {
        int64_t visible = std::max<int64_t>(1, body / rowH_);
        int64_t maxTop = static_cast<int64_t>(order_.size()) - visible;
        return std::max<int64_t>(0, maxTop) * rowH_;
    }
    return std::max<int64_t>(0, totalHeight() - body);
}

int GridView::maxScrollX() const {
    int content = colX_.empty() ? 0 : colX_.back();
    return std::max(0, content - (clientW_ - gutterW_));
}

void GridView::clampScroll() {
    scrollY_ = std::clamp<int64_t>(scrollY_, 0, maxScrollY());
    if (rowY_.empty() && rowH_ > 0) scrollY_ = scrollY_ / rowH_ * rowH_;
    scrollX_ = std::clamp(scrollX_, 0, maxScrollX());
}

void GridView::updateScrollBars() {
    SCROLLINFO si{sizeof si};
    si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
    if (rowY_.empty()) {  // whole rows
        int64_t visible = std::max<int64_t>(1, bodyHeight() / rowH_);
        si.nMin = 0;
        si.nMax = static_cast<int>(std::min<int64_t>(INT_MAX - 1, std::max<int64_t>(0, order_.size() - 1)));
        si.nPage = static_cast<UINT>(std::min<int64_t>(visible, INT_MAX));
        si.nPos = static_cast<int>(scrollY_ / rowH_);
        if (order_.size() <= static_cast<size_t>(visible)) si.nMax = 0, si.nPage = 1 + si.nMax + 1;
    } else {
        int64_t total = totalHeight();
        si.nMin = 0;
        si.nMax = static_cast<int>(std::min<int64_t>(INT_MAX - 1, std::max<int64_t>(0, total - 1)));
        si.nPage = static_cast<UINT>(bodyHeight());
        si.nPos = static_cast<int>(std::min<int64_t>(scrollY_, INT_MAX));
    }
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    SCROLLINFO sh{sizeof sh};
    sh.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
    int frozen = scrollAreaLeft() - gutterW_;
    int content = (colX_.empty() ? 0 : colX_.back()) - frozen;
    sh.nMin = 0;
    sh.nMax = std::max(0, content - 1);
    sh.nPage = static_cast<UINT>(std::max(0, clientW_ - gutterW_ - frozen));
    sh.nPos = scrollX_;
    SetScrollInfo(hwnd_, SB_HORZ, &sh, TRUE);
}

void GridView::relayout(bool keepTopRow) {
    size_t topRow = keepTopRow ? rowAtY(scrollY_) : 0;
    layoutColumns();
    layoutRows();
    if (keepTopRow) scrollY_ = rowTop(topRow);
    clampScroll();
    updateScrollBars();
}

// ====================================================================== cell text

size_t GridView::fileRowOfDisplay(size_t displayRow) const {
    size_t d = displayRow < order_.size() ? order_[displayRow] : displayRow;
    return table_->fileRowOfData(d);
}

GridView::CellText GridView::cellTextUncached(size_t fileRow, size_t col) const {
    CellText ct;
    if (!table_ || fileRow >= table_->fileRows()) return ct;
    std::vector<csv::Field> f;
    table_->fields(fileRow, f);
    if (col >= f.size()) return ct;
    ct.text = utf8ToUtf16Capped(csv::fieldValue(table_->data, f[col]), opt_.maxCellChars, &ct.capped);
    return ct;
}

const GridView::RowCache& GridView::rowCells(size_t fileRow) const {
    auto it = cache_.find(fileRow);
    if (it != cache_.end()) {
        it->second.stamp = ++cacheStamp_;
        return it->second;
    }
    if (cache_.size() >= kCacheRows) cache_.clear();
    RowCache rc;
    rc.stamp = ++cacheStamp_;
    std::vector<csv::Field> f;
    table_->fields(fileRow, f);
    rc.cells.resize(f.size());
    for (size_t c = 0; c < f.size(); ++c)
        rc.cells[c].text =
            utf8ToUtf16Capped(csv::fieldValue(table_->data, f[c]), opt_.maxCellChars, &rc.cells[c].capped);
    return cache_.emplace(fileRow, std::move(rc)).first->second;
}

const GridView::CellText& GridView::cellText(size_t fileRow, size_t col) const {
    static const CellText empty;
    const RowCache& rc = rowCells(fileRow);
    return col < rc.cells.size() ? rc.cells[col] : empty;
}

// ====================================================================== painting

void GridView::drawLine(HDC dc, const RECT& rc, int y, const csv::DisplayLine& line, bool forceEllipsis,
                        bool rightAlign, COLORREF textColor, std::optional<std::pair<size_t, size_t>> highlight) {
    const int avail = rc.right - rc.left - 2 * padX_ - 1;
    if (avail <= 0) return;
    if (line.hasGlyph() && breakGlyph_ != csv::kLineBreakGlyph) {
        csv::DisplayLine sub = line;
        for (size_t k = 0; k < sub.text.size(); ++k)
            if (sub.glyphMask[k]) sub.text[k] = breakGlyph_;
        drawLineImpl(dc, rc, y, sub, forceEllipsis, rightAlign, textColor, highlight);
        return;
    }
    drawLineImpl(dc, rc, y, line, forceEllipsis, rightAlign, textColor, highlight);
}

void GridView::drawLineImpl(HDC dc, const RECT& rc, int y, const csv::DisplayLine& line, bool forceEllipsis,
                            bool rightAlign, COLORREF textColor, std::optional<std::pair<size_t, size_t>> highlight) {
    const int avail = rc.right - rc.left - 2 * padX_ - 1;
    size_t n = line.text.size();
    const size_t limit = static_cast<size_t>(avail) + 8;  // glyphs are at least ~1px wide
    if (n > limit) {
        n = limit;
        forceEllipsis = true;
    }
    if (n == 0 && !forceEllipsis) return;
    std::vector<int> dx(std::max<size_t>(n, 1));
    SIZE sz{};
    // lpnFit must be NULL: with a fit pointer Windows fills dx only for the
    // characters that fit into nMaxExtent (none for 0), leaving zero advances.
    if (n) GetTextExtentExPointW(dc, W(line.text), static_cast<int>(n), 0, nullptr, dx.data(), &sz);
    int total = n ? dx[n - 1] : 0;
    size_t drawN = n;
    bool ell = forceEllipsis || total > avail;
    if (ell) {
        int limitW = avail - ellipsisW_;
        drawN = static_cast<size_t>(std::upper_bound(dx.begin(), dx.begin() + static_cast<ptrdiff_t>(n), limitW) -
                                    dx.begin());
        // never split a surrogate pair
        if (drawN > 0 && drawN < n && line.text[drawN - 1] >= 0xD800 && line.text[drawN - 1] <= 0xDBFF) --drawN;
    }
    int drawnW = (drawN ? dx[drawN - 1] : 0) + (ell ? ellipsisW_ : 0);
    int x0 = rightAlign ? rc.right - padX_ - 1 - drawnW : rc.left + padX_;
    RECT clip = rc;
    clip.right -= 1;
    if (highlight && drawN) {
        size_t a = std::min(highlight->first, drawN), b = std::min(highlight->second, drawN);
        if (b > a) {
            int xa = x0 + (a ? dx[a - 1] : 0), xb = x0 + dx[b - 1];
            fill(dc, std::max(xa, static_cast<int>(clip.left)), y, std::min(xb, static_cast<int>(clip.right)),
                 std::min(y + lineH_, static_cast<int>(clip.bottom)), cr(opt_.colors.searchHitBackground));
        }
    }
    const COLORREF glyphColor = cr(opt_.colors.glyph);
    std::vector<int> adv;
    size_t i = 0;
    while (i < drawN) {
        bool g = line.hasGlyph() && line.glyphMask[i];
        size_t j = i + 1;
        while (j < drawN && (line.hasGlyph() && line.glyphMask[j]) == g) ++j;
        SetTextColor(dc, g ? glyphColor : textColor);
        int x = x0 + (i ? dx[i - 1] : 0);
        // Explicit advances: glyphs land exactly where they were measured.
        adv.resize(j - i);
        for (size_t k = i; k < j; ++k) adv[k - i] = dx[k] - (k ? dx[k - 1] : 0);
        ExtTextOutW(dc, x, y, ETO_CLIPPED, &clip, W(line.text) + i, static_cast<UINT>(j - i), adv.data());
        i = j;
    }
    if (ell) {
        SetTextColor(dc, glyphColor);
        const wchar_t e = csv::kEllipsis;
        ExtTextOutW(dc, x0 + (drawN ? dx[drawN - 1] : 0), y, ETO_CLIPPED, &clip, &e, 1, nullptr);
    }
}

void GridView::paintCell(HDC dc, const RECT& rc, size_t displayRow, size_t col, bool selected) {
    const Palette& p = opt_.colors;
    if (selected) fill(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, cr(p.selectionBackground));
    const size_t fr = fileRowOfDisplay(displayRow);
    const CellText& ct = cellText(fr, col);
    if (ct.text.empty()) return;
    const COLORREF tc = cr(selected ? p.selectionText : p.text);
    const bool right = opt_.alignNumbersRight && col < numericCol_.size() && numericCol_[col];
    std::optional<std::pair<size_t, size_t>> hl;
    const bool isHit = hit_ && hit_->row == static_cast<long>(displayRow) && hit_->col == col;
    const bool isLast = col + 1 == table_->columns;
    const int y0 = rc.top + padY_;
    const int availLines = std::max<int>(1, static_cast<int>(rc.bottom - rc.top - 2 * padY_ - 1) / std::max(1, lineH_));
    const bool ownMulti = opt_.multiline && csv::hasLineBreak(ct.text);
    const bool wrapped = !ownMulti && isLast && opt_.lastColumn == LastColumnMode::Wrap &&
                         table_->lastCellLines[fr] <= 1 && availLines > 1;

    if (ownMulti) {
        // Split at own breaks, remembering original offsets for the search highlight.
        size_t start = 0, lineNo = 0;
        const std::u16string& s = ct.text;
        size_t i = 0;
        while (lineNo < static_cast<size_t>(availLines)) {
            size_t e = start;
            while (e < s.size() && s[e] != u'\r' && s[e] != u'\n') ++e;
            bool brk = e < s.size();
            size_t next = e;
            if (brk) next += (s[e] == u'\r' && e + 1 < s.size() && s[e + 1] == u'\n') ? 2 : 1;
            csv::DisplayLine dl = csv::singleLine(std::u16string_view(s).substr(start, e - start));
            if (brk) {
                if (dl.glyphMask.empty()) dl.glyphMask.assign(dl.text.size(), false);
                dl.text.push_back(csv::kLineBreakGlyph);
                dl.glyphMask.push_back(true);
            }
            bool lastVisible = lineNo + 1 == static_cast<size_t>(availLines);
            bool more = (lastVisible && brk) || (!brk && ct.capped);
            if (isHit) {
                size_t a = hit_->pos, b = hit_->pos + hit_->len;
                if (b > start && a < e) hl = std::make_pair(std::max(a, start) - start, std::min(b, e) - start);
                else hl.reset();
            }
            drawLine(dc, rc, y0 + static_cast<int>(lineNo) * lineH_, dl, more, right, tc, hl);
            ++lineNo;
            if (!brk) break;
            start = next;
            ++i;
        }
        return;
    }
    csv::DisplayLine dl = csv::singleLine(ct.text);
    if (wrapped) {
        HGDIOBJ old = SelectObject(measureDc_, font_);
        auto fitFn = [this](std::u16string_view v, int w) -> size_t {
            int n = 0;
            SIZE sz{};
            GetTextExtentExPointW(measureDc_, W(v), static_cast<int>(std::min<size_t>(v.size(), 32767)), w, &n,
                                  nullptr, &sz);
            return static_cast<size_t>(std::max(n, 0));
        };
        auto wr = csv::wrapText(dl.text, wrapWidthPx(), static_cast<size_t>(availLines), fitFn);
        SelectObject(measureDc_, old);
        for (size_t k = 0; k < wr.lines.size(); ++k) {
            size_t off = static_cast<size_t>(wr.lines[k].data() - dl.text.data());
            csv::DisplayLine part;
            part.text = std::u16string(wr.lines[k]);
            bool more = k + 1 == wr.lines.size() && (wr.truncated || ct.capped);
            if (isHit) {
                size_t a = hit_->pos, b = hit_->pos + hit_->len, e = off + part.text.size();
                if (b > off && a < e) hl = std::make_pair(std::max(a, off) - off, std::min(b, e) - off);
                else hl.reset();
            }
            drawLine(dc, rc, y0 + static_cast<int>(k) * lineH_, part, more, false, tc, hl);
        }
        return;
    }
    if (isHit) hl = std::make_pair(csv::singleLineOffset(ct.text, hit_->pos),
                                   csv::singleLineOffset(ct.text, hit_->pos + hit_->len));
    drawLine(dc, rc, y0, dl, ct.capped, right, tc, hl);
}

void GridView::paintRows(HDC dc, size_t r0, size_t r1, size_t c0, size_t c1, int clipLeft, int clipRight) {
    if (clipRight <= clipLeft) return;
    const Palette& p = opt_.colors;
    int saved = SaveDC(dc);
    IntersectClipRect(dc, clipLeft, headerH_, clipRight, clientH_);
    const COLORREF grid = cr(p.gridLines);
    bool needRebuild = false;
    for (size_t r = r0; r <= r1 && r < order_.size(); ++r) {
        int y = headerH_ + static_cast<int>(rowTop(r) - scrollY_);
        int h = rowHeight(r);
        if (opt_.zebra && (r & 1)) fill(dc, clipLeft, y, clipRight, y + h, cr(p.altRowBackground));
        if (opt_.lastColumn == LastColumnMode::Wrap && !wrapExact_) {
            size_t fr = fileRowOfDisplay(r);
            if (wrapEstimated_[fr]) {
                int exact = wrapLinesOf(fr, true);
                wrapEstimated_[fr] = false;
                if (exact != wrapLines_[fr]) {
                    wrapLines_[fr] = static_cast<uint8_t>(std::min(255, exact));
                    needRebuild = true;
                }
            }
        }
        for (size_t c = c0; c <= c1 && c < colX_.size() - 1; ++c) {
            RECT rc{colScreenX(c), y, colScreenX(c) + colWidth(c), y + h};
            if (rc.right < clipLeft || rc.left > clipRight) continue;
            paintCell(dc, rc, r, c, inSelection(static_cast<long>(r), static_cast<long>(c)));
            fill(dc, rc.right - 1, rc.top, rc.right, rc.bottom, grid);
        }
        fill(dc, clipLeft, y + h - 1, clipRight, y + h, grid);
    }
    RestoreDC(dc, saved);
    if (needRebuild) {
        size_t top = rowAtY(scrollY_);
        layoutRows();
        scrollY_ = rowTop(top);
        clampScroll();
        updateScrollBars();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void GridView::paintGutter(HDC dc, size_t r0, size_t r1) {
    if (gutterW_ <= 0) return;
    const Palette& p = opt_.colors;
    fill(dc, 0, headerH_, gutterW_, clientH_, cr(p.gutterBackground));
    int saved = SaveDC(dc);
    IntersectClipRect(dc, 0, headerH_, gutterW_, clientH_);
    SelectObject(dc, font_);
    for (size_t r = r0; r <= r1 && r < order_.size(); ++r) {
        int y = headerH_ + static_cast<int>(rowTop(r) - scrollY_);
        int h = rowHeight(r);
        bool sel = static_cast<long>(r) >= selR0_ && static_cast<long>(r) <= selR1_;
        if (sel) fill(dc, 0, y, gutterW_ - 1, y + h - 1, cr(p.selectionBackground));
        std::wstring num = std::to_wstring(static_cast<unsigned long long>(order_[r]) + 1);
        SetTextColor(dc, cr(sel ? p.selectionText : p.gutterText));
        RECT rc{0, y + padY_, gutterW_ - padX_, y + h};
        DrawTextW(dc, num.c_str(), static_cast<int>(num.size()), &rc, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        fill(dc, 0, y + h - 1, gutterW_, y + h, cr(p.gridLines));
    }
    RestoreDC(dc, saved);
    fill(dc, gutterW_ - 1, headerH_, gutterW_, clientH_, cr(p.gridLines));
}

void GridView::paintHeader(HDC dc) {
    const Palette& p = opt_.colors;
    fill(dc, 0, 0, clientW_, headerH_, cr(p.headerBackground));
    const size_t cols = table_ ? table_->columns : 0;
    SelectObject(dc, bold_);
    auto drawCol = [&](size_t c, int clipL, int clipR) {
        RECT rc{colScreenX(c), 0, colScreenX(c) + colWidth(c), headerH_};
        if (rc.right <= clipL || rc.left >= clipR) return;
        int saved = SaveDC(dc);
        IntersectClipRect(dc, std::max<int>(clipL, rc.left), 0, std::min<int>(clipR, rc.right), headerH_);
        bool sel = selKind_ == SelKind::Cols ? (static_cast<long>(c) >= selC0_ && static_cast<long>(c) <= selC1_)
                                             : (static_cast<long>(c) >= selC0_ && static_cast<long>(c) <= selC1_ &&
                                                hasCells());
        if (sel) fill(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, cr(p.selectionBackground));
        RECT textRc = rc;
        if (c == sortCol_ && sortDir_ != csv::SortDir::None) {
            const wchar_t* arrow = sortDir_ == csv::SortDir::Asc ? L"▲" : L"▼";
            SIZE as{};
            GetTextExtentPoint32W(dc, arrow, 1, &as);
            SetTextColor(dc, cr(p.glyph));
            int ax = rc.right - padX_ - 1 - as.cx;
            ExtTextOutW(dc, ax, padY_ + 1, ETO_CLIPPED, &rc, arrow, 1, nullptr);
            textRc.right = ax;
        }
        csv::DisplayLine dl;
        dl.text = c < captions_.size() ? captions_[c] : std::u16string();
        std::optional<std::pair<size_t, size_t>> hl;
        if (hit_ && hit_->row == -1 && hit_->col == c) hl = std::make_pair(hit_->pos, hit_->pos + hit_->len);
        drawLine(dc, textRc, padY_ + 1, dl, false, false, cr(sel ? p.selectionText : p.headerText), hl);
        fill(dc, rc.right - 1, 0, rc.right, headerH_, cr(p.gridLines));
        RestoreDC(dc, saved);
    };
    if (cols) {
        const bool frozen = opt_.freezeFirst && cols > 1;
        const int left = scrollAreaLeft();
        size_t first = frozen ? 1 : 0;
        for (size_t c = first; c < cols; ++c) {
            int x = colScreenX(c);
            if (x >= clientW_) break;
            if (x + colWidth(c) <= left) continue;
            drawCol(c, left, clientW_);
        }
        if (frozen) {
            drawCol(0, gutterW_, left);
            fill(dc, left - 1, 0, left, clientH_, cr(p.frozenDivider));
        }
    }
    fill(dc, 0, headerH_ - 1, clientW_, headerH_, cr(p.gridLines));
    if (gutterW_) fill(dc, gutterW_ - 1, 0, gutterW_, headerH_, cr(p.gridLines));
}

void GridView::paint(HDC dc, const RECT&) {
    const Palette& p = opt_.colors;
    fill(dc, 0, 0, clientW_, clientH_, cr(p.background));
    if (!table_) return;
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, font_);
    const size_t cols = table_->columns;
    size_t r0 = 0, r1 = 0;
    const bool anyRows = !order_.empty() && cols > 0;
    if (anyRows) {
        r0 = rowAtY(scrollY_);
        r1 = r0;
        while (r1 + 1 < order_.size() && rowTop(r1 + 1) - scrollY_ < bodyHeight()) ++r1;
        const bool frozen = opt_.freezeFirst && cols > 1;
        const int left = scrollAreaLeft();
        size_t c0 = frozen ? 1 : 0, c1 = c0;
        while (c0 + 1 < cols && colScreenX(c0) + colWidth(c0) <= left) ++c0;
        c1 = c0;
        while (c1 + 1 < cols && colScreenX(c1 + 1) < clientW_) ++c1;
        SelectObject(dc, font_);
        paintRows(dc, r0, r1, c0, c1, left, clientW_);
        if (frozen) {
            paintRows(dc, r0, r1, 0, 0, gutterW_, left);
        }
        paintGutter(dc, r0, r1);
        // Current cell border.
        if (curRow_ >= 0 && curRow_ < static_cast<long>(order_.size()) && curCol_ >= 0 &&
            curCol_ < static_cast<long>(cols)) {
            int y = headerH_ + static_cast<int>(rowTop(static_cast<size_t>(curRow_)) - scrollY_);
            int x = colScreenX(static_cast<size_t>(curCol_));
            RECT rc{x - 1, y - 1, x + colWidth(static_cast<size_t>(curCol_)), y + rowHeight(static_cast<size_t>(curRow_))};
            int saved = SaveDC(dc);
            int clipL = (frozen && curCol_ == 0) ? gutterW_ : left;
            int clipR = (frozen && curCol_ == 0) ? left : clientW_;
            IntersectClipRect(dc, clipL, headerH_, clipR, clientH_);
            HPEN pen = CreatePen(PS_SOLID, std::max(1, static_cast<int>(std::lround(scale() * 1.5))),
                                 cr(p.currentCellBorder));
            HGDIOBJ op = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, rc.left + 1, rc.top + 1, rc.right, rc.bottom);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(pen);
            RestoreDC(dc, saved);
        }
    } else if (gutterW_) {
        fill(dc, 0, headerH_, gutterW_, clientH_, cr(p.gutterBackground));
    }
    paintHeader(dc);
}

// ====================================================================== selection

void GridView::updateSelectionRect() {
    const long rows = static_cast<long>(order_.size());
    const long cols = table_ ? static_cast<long>(table_->columns) : 0;
    switch (selKind_) {
        case SelKind::Cells:
            selR0_ = std::min(anchorRow_, curRow_);
            selR1_ = std::max(anchorRow_, curRow_);
            selC0_ = std::min(anchorCol_, curCol_);
            selC1_ = std::max(anchorCol_, curCol_);
            break;
        case SelKind::Rows:
            selR0_ = std::min(anchorRow_, curRow_);
            selR1_ = std::max(anchorRow_, curRow_);
            selC0_ = 0;
            selC1_ = cols - 1;
            break;
        case SelKind::Cols:
            selR0_ = 0;
            selR1_ = rows - 1;
            selC0_ = std::min(anchorCol_, curCol_);
            selC1_ = std::max(anchorCol_, curCol_);
            break;
    }
}

bool GridView::inSelection(long row, long col) const {
    return row >= selR0_ && row <= selR1_ && col >= selC0_ && col <= selC1_;
}

bool GridView::hasCells() const { return table_ && !order_.empty() && table_->columns > 0; }

csv::CellRange GridView::selection() const {
    csv::CellRange r;
    r.r0 = static_cast<size_t>(std::max<long>(0, selR0_));
    r.r1 = static_cast<size_t>(std::max<long>(0, selR1_));
    r.c0 = static_cast<size_t>(std::max<long>(0, selC0_));
    r.c1 = static_cast<size_t>(std::max<long>(0, selC1_));
    return r;
}

size_t GridView::selectedRows() const { return hasCells() ? static_cast<size_t>(selR1_ - selR0_ + 1) : 0; }
size_t GridView::selectedCols() const { return hasCells() ? static_cast<size_t>(selC1_ - selC0_ + 1) : 0; }

void GridView::selectAll() {
    if (!hasCells()) return;
    selKind_ = SelKind::Rows;
    anchorRow_ = 0;
    selR0_ = 0;
    selR1_ = static_cast<long>(order_.size()) - 1;
    selC0_ = 0;
    selC1_ = static_cast<long>(table_->columns) - 1;
    // keep the current cell; anchor/cur define future extension from row 0
    InvalidateRect(hwnd_, nullptr, FALSE);
    notify();
}

void GridView::selectColumns(size_t c0, size_t c1) {
    if (!hasCells()) return;
    long last = static_cast<long>(table_->columns) - 1;
    selKind_ = SelKind::Cols;
    anchorCol_ = std::clamp<long>(static_cast<long>(c0), 0, last);
    curCol_ = std::clamp<long>(static_cast<long>(c1), 0, last);
    updateSelectionRect();
    ensureVisible(-1, curCol_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    notify();
}

std::wstring GridView::copyText(csv::CopyFormat fmt, bool withHeader) const {
    if (!hasCells()) {
        if (withHeader && table_ && table_->headerRows()) {
            auto s = csv::copyRange(*table_, order_, csv::CellRange{0, 0, 0, table_->columns - 1}, fmt, true);
            return win::widen(s);
        }
        return {};
    }
    auto s = csv::copyRange(*table_, order_, selection(), fmt, withHeader);
    return win::widen(s);
}

void GridView::setCurrent(long row, long col) {
    const long rows = static_cast<long>(order_.size());
    const long cols = table_ ? static_cast<long>(table_->columns) : 0;
    curRow_ = std::clamp<long>(row, 0, std::max<long>(0, rows - 1));
    curCol_ = std::clamp<long>(col, 0, std::max<long>(0, cols - 1));
}

void GridView::moveCurrent(long row, long col, bool extend) {
    if (!hasCells()) return;
    setCurrent(row, col);
    if (!extend) {
        anchorRow_ = curRow_;
        anchorCol_ = curCol_;
        selKind_ = SelKind::Cells;
    }
    updateSelectionRect();
    ensureVisible(curRow_, curCol_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    notify();
}

void GridView::ensureVisible(long row, long col) {
    if (!table_) return;
    if (row >= 0 && row < static_cast<long>(order_.size())) {
        int64_t top = rowTop(static_cast<size_t>(row));
        int64_t h = rowHeight(static_cast<size_t>(row));
        int64_t body = bodyHeight();
        if (top < scrollY_) scrollY_ = top;
        else if (top + h > scrollY_ + body) {
            if (rowY_.empty()) {
                int64_t visible = std::max<int64_t>(1, body / rowH_);
                scrollY_ = (static_cast<int64_t>(row) - visible + 1) * rowH_;
            } else {
                scrollY_ = top + h - body;
                if (h > body) scrollY_ = top;
            }
        }
    }
    if (col >= 0 && col < static_cast<long>(table_->columns)) {
        bool frozenCol = opt_.freezeFirst && col == 0 && table_->columns > 1;
        if (!frozenCol) {
            int left = scrollAreaLeft();
            int x = colScreenX(static_cast<size_t>(col));
            int w = colWidth(static_cast<size_t>(col));
            if (x < left) scrollX_ -= left - x;
            else if (x + w > clientW_) scrollX_ += std::min(x + w - clientW_, x - left);
        }
    }
    clampScroll();
    updateScrollBars();
}

// ====================================================================== sorting

csv::SortAs GridView::sortAs(size_t col) const {
    auto it = sortAs_.find(col);
    return it == sortAs_.end() ? csv::SortAs::Auto : it->second;
}

void GridView::setSortAs(size_t col, csv::SortAs as) {
    sortAs_[col] = as;
    if (col == sortCol_ && sortDir_ != csv::SortDir::None) {
        applySort();
        InvalidateRect(hwnd_, nullptr, FALSE);
        notify();
    }
}

void GridView::sortBy(size_t col, csv::SortDir dir) {
    if (!table_ || col >= table_->columns) return;
    sortCol_ = dir == csv::SortDir::None ? SIZE_MAX : col;
    sortDir_ = dir;
    applySort();
    InvalidateRect(hwnd_, nullptr, FALSE);
    notify();
}

void GridView::applySort() {
    if (!table_) return;
    uint32_t curData = curRow_ >= 0 && curRow_ < static_cast<long>(order_.size()) ? order_[static_cast<size_t>(curRow_)] : 0;
    HCURSOR oldCur = nullptr;
    if (order_.size() > 50000) oldCur = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    if (sortDir_ == csv::SortDir::None || sortCol_ == SIZE_MAX)
        order_ = csv::sortRows(*table_, 0, csv::SortDir::None, csv::SortAs::Auto, sortKey_);
    else
        order_ = csv::sortRows(*table_, sortCol_, sortDir_, sortAs(sortCol_), sortKey_);
    if (oldCur) SetCursor(oldCur);
    hit_.reset();
    long newRow = 0;
    for (size_t i = 0; i < order_.size(); ++i)
        if (order_[i] == curData) {
            newRow = static_cast<long>(i);
            break;
        }
    curRow_ = anchorRow_ = newRow;
    anchorCol_ = curCol_;
    selKind_ = SelKind::Cells;
    updateSelectionRect();
    relayout(true);
    ensureVisible(curRow_, curCol_);
}

// ====================================================================== search

bool GridView::search(const std::wstring& text, int flags) {
    if (!table_ || text.empty() || table_->columns == 0) return false;
    const bool backward = (flags & lcs_backwards) != 0;
    const bool mc = (flags & lcs_matchcase) != 0;
    const bool ww = (flags & lcs_wholewords) != 0;
    csv::Matcher m = matcherFactory_ ? matcherFactory_(text, mc, ww)
                                     : csv::defaultMatcher(std::u16string(text.begin(), text.end()), mc, ww);
    csv::SearchStart st;
    const long lastRow = static_cast<long>(order_.size()) - 1;
    if (flags & lcs_findfirst) {
        if (backward) st = {lastRow, table_->columns - 1, SIZE_MAX, true};
        else st = {table_->headerRows() ? -1L : 0L, 0, 0, true};
    } else if (hit_ && hitCurRow_ == curRow_ && hitCurCol_ == curCol_) {
        st = {hit_->row, hit_->col, backward ? hit_->pos : hit_->pos + 1, true};
    } else {
        st = {curRow_, static_cast<size_t>(curCol_), 0, false};
    }
    auto h = csv::searchTable(*table_, order_, st, backward, m);
    if (!h) return false;
    hit_ = h;
    if (h->row >= 0) {
        anchorRow_ = curRow_ = h->row;
        anchorCol_ = curCol_ = static_cast<long>(h->col);
        selKind_ = SelKind::Cells;
        updateSelectionRect();
        // center if off-screen
        int64_t top = rowTop(static_cast<size_t>(h->row));
        if (top < scrollY_ || top + rowHeight(static_cast<size_t>(h->row)) > scrollY_ + bodyHeight()) {
            scrollY_ = top - (bodyHeight() - rowHeight(static_cast<size_t>(h->row))) / 2;
            clampScroll();
        }
        ensureVisible(h->row, static_cast<long>(h->col));
    } else {
        anchorCol_ = curCol_ = static_cast<long>(h->col);
        if (selKind_ == SelKind::Cells) anchorRow_ = curRow_;
        updateSelectionRect();
        ensureVisible(-1, static_cast<long>(h->col));
    }
    hitCurRow_ = curRow_;
    hitCurCol_ = curCol_;
    InvalidateRect(hwnd_, nullptr, FALSE);
    notify();
    return true;
}

// ====================================================================== scrolling

int GridView::percent() const {
    int64_t m = maxScrollY();
    if (m <= 0) return 0;
    return static_cast<int>(std::clamp<int64_t>(scrollY_ * 100 / m, 0, 100));
}

void GridView::setPercent(int pct) { scrollToY(maxScrollY() * std::clamp(pct, 0, 100) / 100); }

void GridView::scrollToY(int64_t y) {
    int64_t old = scrollY_;
    scrollY_ = y;
    clampScroll();
    if (scrollY_ != old) {
        updateScrollBars();
        InvalidateRect(hwnd_, nullptr, FALSE);
        notify();
    }
}

void GridView::scrollToX(int x) {
    int old = scrollX_;
    scrollX_ = x;
    clampScroll();
    if (scrollX_ != old) {
        updateScrollBars();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void GridView::scrollBy(int64_t dy, int dx) {
    if (dy) scrollToY(scrollY_ + dy);
    if (dx) scrollToX(scrollX_ + dx);
}

void GridView::wheel(int delta, bool horizontal) {
    int& acc = horizontal ? wheelAccumH_ : wheelAccumV_;
    acc += delta;
    int notches = acc / WHEEL_DELTA;
    if (notches == 0) return;
    acc -= notches * WHEEL_DELTA;
    if (horizontal) {
        UINT chars = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &chars, 0);
        scrollBy(0, notches * static_cast<int>(chars) * avgCharW_ * 2);
        return;
    }
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    int64_t step = lines == WHEEL_PAGESCROLL ? std::max(rowH_, bodyHeight() - rowH_) : static_cast<int64_t>(lines) * rowH_;
    if (!rowY_.empty() && lines != WHEEL_PAGESCROLL) step = static_cast<int64_t>(lines) * (lineH_ + 2 * padY_ + 1);
    scrollBy(-notches * step, 0);
}

void GridView::notify() {
    if (onStateChanged) onStateChanged();
}

// ====================================================================== mouse

GridView::HitInfo GridView::hitTest(int x, int y) const {
    HitInfo h;
    if (!table_) return h;
    const size_t cols = table_->columns;
    auto colAt = [&](int px) -> long {
        if (px < gutterW_ || cols == 0) return -1;
        if (opt_.freezeFirst && cols > 1 && px < scrollAreaLeft()) return 0;
        int cx = px - gutterW_ + scrollX_;
        auto it = std::upper_bound(colX_.begin(), colX_.end(), cx);
        long c = static_cast<long>(it - colX_.begin()) - 1;
        return c >= 0 && c < static_cast<long>(cols) ? c : -1;
    };
    if (y < headerH_) {
        if (x < gutterW_) {
            h.what = Hit::Corner;
            return h;
        }
        // divider: near a column's right edge
        const int grip = std::max(3, static_cast<int>(std::lround(3 * scale())));
        for (size_t c = 0; c < cols; ++c) {
            int r = colScreenX(c) + colWidth(c);
            bool frozenCol = opt_.freezeFirst && cols > 1 && c == 0;
            if (!frozenCol && (r < scrollAreaLeft() || r > clientW_ + grip)) continue;
            if (std::abs(x - r) <= grip) {
                h.what = Hit::Divider;
                h.col = static_cast<long>(c);
                return h;
            }
        }
        h.col = colAt(x);
        h.what = h.col >= 0 ? Hit::Header : Hit::None;
        return h;
    }
    if (order_.empty()) return h;
    int64_t cy = static_cast<int64_t>(y - headerH_) + scrollY_;
    if (cy >= totalHeight()) {
        h.row = static_cast<long>(order_.size()) - 1;
    } else {
        h.row = static_cast<long>(rowAtY(cy));
    }
    if (x < gutterW_) {
        h.what = Hit::Gutter;
        return h;
    }
    h.col = colAt(x);
    h.what = h.col >= 0 ? Hit::Cell : Hit::None;
    return h;
}

void GridView::onMouseDown(int x, int y, WPARAM keys, bool dbl) {
    SetFocus(hwnd_);
    if (!table_) return;
    HitInfo h = hitTest(x, y);
    const bool shift = (keys & MK_SHIFT) != 0, ctrl = (keys & MK_CONTROL) != 0;
    pressPt_ = {x, y};
    switch (h.what) {
        case Hit::Divider:
            if (dbl) {
                fitColumn(static_cast<size_t>(h.col));
                return;
            }
            drag_ = Drag::Resize;
            dragCol_ = h.col;
            dragStartX_ = x;
            dragStartW_ = colWidth(static_cast<size_t>(h.col));
            break;
        case Hit::Header:
            if (ctrl) {
                if (shift && selKind_ == SelKind::Cols) {
                    curCol_ = h.col;
                } else {
                    anchorCol_ = curCol_ = h.col;
                }
                selKind_ = SelKind::Cols;
                updateSelectionRect();
                drag_ = Drag::Cols;
                InvalidateRect(hwnd_, nullptr, FALSE);
                notify();
            } else {
                drag_ = Drag::HeaderPress;
                dragCol_ = h.col;
            }
            break;
        case Hit::Corner:
            selectAll();
            return;
        case Hit::Gutter:
            if (!hasCells()) return;
            if (shift && selKind_ == SelKind::Rows) {
                curRow_ = h.row;
            } else {
                anchorRow_ = curRow_ = h.row;
            }
            selKind_ = SelKind::Rows;
            updateSelectionRect();
            drag_ = Drag::Rows;
            ensureVisible(curRow_, -1);
            InvalidateRect(hwnd_, nullptr, FALSE);
            notify();
            break;
        case Hit::Cell:
            if (!hasCells()) return;
            moveCurrent(h.row, h.col, shift);
            drag_ = Drag::Cells;
            break;
        case Hit::None:
            return;
    }
    SetCapture(hwnd_);
}

void GridView::dragTo(int x, int y) {
    if (!hasCells()) return;
    int cx = std::clamp(x, gutterW_, std::max(gutterW_, clientW_ - 1));
    int cyy = std::clamp(y, headerH_, std::max(headerH_, clientH_ - 1));
    HitInfo h = hitTest(drag_ == Drag::Rows ? gutterW_ : cx, drag_ == Drag::Cols ? headerH_ + 1 : cyy);
    if (drag_ == Drag::Cols) {
        HitInfo hc = hitTest(cx, 1);
        if (hc.col < 0 && hc.what == Hit::Divider) hc.col = hc.col;  // divider: keep column
        long col = hc.col >= 0 ? hc.col : curCol_;
        if (col != curCol_) {
            curCol_ = col;
            updateSelectionRect();
            ensureVisible(-1, curCol_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            notify();
        }
        return;
    }
    if (drag_ == Drag::Rows) {
        if (h.row >= 0 && h.row != curRow_) {
            curRow_ = h.row;
            updateSelectionRect();
            ensureVisible(curRow_, -1);
            InvalidateRect(hwnd_, nullptr, FALSE);
            notify();
        }
        return;
    }
    long row = h.row >= 0 ? h.row : curRow_;
    long col = h.col >= 0 ? h.col : curCol_;
    if (row != curRow_ || col != curCol_) moveCurrent(row, col, true);
}

void GridView::onMouseMove(int x, int y, WPARAM) {
    lastMouse_ = {x, y};
    switch (drag_) {
        case Drag::Resize: {
            int w = std::max(px(opt_.minColumnWidth) / 2 + 4, dragStartW_ + (x - dragStartX_));
            baseW_[static_cast<size_t>(dragCol_)] = std::max(4, static_cast<int>(std::lround(w / scale())));
            manualW_[static_cast<size_t>(dragCol_)] = true;
            if (opt_.lastColumn == LastColumnMode::Wrap) std::fill(wrapLines_.begin(), wrapLines_.end(), 0);
            relayout(true);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        case Drag::HeaderPress: {
            int th = GetSystemMetrics(SM_CXDRAG);
            HitInfo h = hitTest(x, 1);
            if (std::abs(x - pressPt_.x) > th && h.col >= 0 && h.col != dragCol_ && hasCells()) {
                anchorCol_ = dragCol_;
                curCol_ = h.col;
                selKind_ = SelKind::Cols;
                drag_ = Drag::Cols;
                updateSelectionRect();
                InvalidateRect(hwnd_, nullptr, FALSE);
                notify();
            }
            return;
        }
        case Drag::Cells:
        case Drag::Rows:
        case Drag::Cols: {
            bool outside = x < scrollAreaLeft() || x >= clientW_ || y < headerH_ || y >= clientH_;
            if (outside) SetTimer(hwnd_, kAutoScrollTimer, 50, nullptr);
            else KillTimer(hwnd_, kAutoScrollTimer);
            dragTo(x, y);
            return;
        }
        case Drag::None:
            updateTooltip(x, y);
            return;
    }
}

void GridView::onMouseUp(int x, int y) {
    Drag d = drag_;
    drag_ = Drag::None;
    KillTimer(hwnd_, kAutoScrollTimer);
    if (GetCapture() == hwnd_) ReleaseCapture();
    if (d == Drag::HeaderPress && dragCol_ >= 0) {
        HitInfo h = hitTest(x, y);
        if (h.what == Hit::Header && h.col == dragCol_) {
            size_t col = static_cast<size_t>(dragCol_);
            csv::SortDir next = csv::SortDir::Asc;
            if (col == sortCol_) {
                next = sortDir_ == csv::SortDir::Asc    ? csv::SortDir::Desc
                       : sortDir_ == csv::SortDir::Desc ? csv::SortDir::None
                                                        : csv::SortDir::Asc;
            }
            sortBy(col, next);
        }
    }
    if (d == Drag::Resize && opt_.lastColumn == LastColumnMode::Wrap) {
        relayout(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void GridView::updateTooltip(int x, int y) {
    if (!tooltip_ || !table_) return;
    HitInfo h = hitTest(x, y);
    long row = h.what == Hit::Cell ? h.row : LONG_MIN;
    long col = h.what == Hit::Cell ? h.col : -1;
    if (h.what == Hit::Header) {
        row = -1;
        col = h.col;
    }
    if (row == hoverRow_ && col == hoverCol_) return;
    hoverRow_ = row;
    hoverCol_ = col;
    SendMessageW(tooltip_, TTM_POP, 0, 0);
    TOOLINFOW ti{};
    ti.cbSize = sizeof ti;
    ti.hwnd = hwnd_;
    ti.uId = 1;
    if (col >= 0 && (row >= 0 || row == -1)) {
        int top = row == -1 ? 0 : headerH_ + static_cast<int>(rowTop(static_cast<size_t>(row)) - scrollY_);
        int hgt = row == -1 ? headerH_ : rowHeight(static_cast<size_t>(row));
        int left = colScreenX(static_cast<size_t>(col));
        ti.rect = RECT{left, top, left + colWidth(static_cast<size_t>(col)), top + hgt};
    } else {
        ti.rect = RECT{0, 0, 0, 0};
    }
    SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
}

// ====================================================================== keyboard

void GridView::onKey(WPARAM vk, LPARAM, bool& handled) {
    handled = true;
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    const bool alt = GetKeyState(VK_MENU) < 0;
    if (alt) {
        handled = false;
        return;
    }
    const long rows = static_cast<long>(order_.size());
    const long cols = table_ ? static_cast<long>(table_->columns) : 0;
    auto pageRows = [&]() -> long {
        if (rowY_.empty()) return std::max<long>(1, bodyHeight() / rowH_ - 1);
        long top = static_cast<long>(rowAtY(scrollY_));
        long bottom = static_cast<long>(rowAtY(scrollY_ + bodyHeight() - 1));
        return std::max<long>(1, bottom - top);
    };
    if (ctrl) {
        switch (vk) {
            case 'A': selectAll(); return;
            case 'C':
            case VK_INSERT:
                if (onShortcut) onShortcut(shift && vk == 'C' ? IDM_COPY_CSV : IDM_COPY);
                return;
            case '0':
            case VK_NUMPAD0: setZoom(100); return;
            case VK_OEM_PLUS:
            case VK_ADD: setZoom(opt_.zoom + 10); return;
            case VK_OEM_MINUS:
            case VK_SUBTRACT: setZoom(opt_.zoom - 10); return;
        }
    }
    if (vk == VK_APPS || (shift && vk == VK_F10)) {
        if (onContextMenu) {
            POINT pt{colScreenX(static_cast<size_t>(std::max<long>(0, curCol_))) + 8,
                     headerH_ + static_cast<int>(rowTop(static_cast<size_t>(std::max<long>(0, curRow_))) - scrollY_) + 8};
            ClientToScreen(hwnd_, &pt);
            onContextMenu(pt, -1);
        }
        return;
    }
    if (!hasCells()) {
        switch (vk) {
            case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_PRIOR: case VK_NEXT: case VK_HOME:
            case VK_END: return;
        }
        handled = false;
        return;
    }
    const bool rowMode = selKind_ == SelKind::Rows && shift;
    (void)rowMode;
    switch (vk) {
        case VK_UP: moveCurrent(ctrl ? 0 : curRow_ - 1, curCol_, shift); return;
        case VK_DOWN: moveCurrent(ctrl ? rows - 1 : curRow_ + 1, curCol_, shift); return;
        case VK_LEFT: moveCurrent(curRow_, ctrl ? 0 : curCol_ - 1, shift); return;
        case VK_RIGHT: moveCurrent(curRow_, ctrl ? cols - 1 : curCol_ + 1, shift); return;
        case VK_HOME: moveCurrent(ctrl ? 0 : curRow_, 0, shift); return;
        case VK_END: moveCurrent(ctrl ? rows - 1 : curRow_, cols - 1, shift); return;
        case VK_PRIOR: moveCurrent(curRow_ - pageRows(), curCol_, shift); return;
        case VK_NEXT: moveCurrent(curRow_ + pageRows(), curCol_, shift); return;
        case VK_SHIFT: case VK_CONTROL: case VK_MENU: return;
    }
    handled = false;
}

// ====================================================================== window procedure

LRESULT CALLBACK GridView::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<GridView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    try {
        return self->handle(msg, wp, lp);
    } catch (...) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

LRESULT GridView::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            if (clientW_ > 0 && clientH_ > 0) {
                if (!backDc_ || backW_ < clientW_ || backH_ < clientH_) {
                    if (backDc_) {
                        SelectObject(backDc_, backOld_);
                        DeleteObject(backBmp_);
                        DeleteDC(backDc_);
                    }
                    backW_ = std::max(clientW_, backW_);
                    backH_ = std::max(clientH_, backH_);
                    backDc_ = CreateCompatibleDC(dc);
                    backBmp_ = CreateCompatibleBitmap(dc, backW_, backH_);
                    backOld_ = SelectObject(backDc_, backBmp_);
                }
                paint(backDc_, ps.rcPaint);
                BitBlt(dc, 0, 0, clientW_, clientH_, backDc_, 0, 0, SRCCOPY);
            }
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PRINTCLIENT: {  // render into a caller-supplied DC (tests, PrintWindow)
            RECT rc{0, 0, clientW_, clientH_};
            paint(reinterpret_cast<HDC>(wp), rc);
            return 0;
        }
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            bool widthChanged = w != clientW_;
            clientW_ = w;
            clientH_ = h;
            if (widthChanged && opt_.lastColumn == LastColumnMode::Wrap) std::fill(wrapLines_.begin(), wrapLines_.end(), 0);
            if (table_) relayout(true);
            InvalidateRect(hwnd_, nullptr, FALSE);
            notify();
            return 0;
        }
        case WM_DPICHANGED_AFTERPARENT:
            if (table_) {
                createFonts();
                computeMetrics();
                relayout(true);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO si{sizeof si};
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd_, SB_VERT, &si);
            const int64_t unit = rowY_.empty() ? rowH_ : 1;
            const int64_t line = rowY_.empty() ? rowH_ : (lineH_ + 2 * padY_ + 1);
            const int64_t page = std::max<int64_t>(line, rowY_.empty() ? (bodyHeight() / rowH_ - 1) * rowH_ : bodyHeight() - line);
            switch (LOWORD(wp)) {
                case SB_LINEUP: scrollBy(-line, 0); break;
                case SB_LINEDOWN: scrollBy(line, 0); break;
                case SB_PAGEUP: scrollBy(-page, 0); break;
                case SB_PAGEDOWN: scrollBy(page, 0); break;
                case SB_TOP: scrollToY(0); break;
                case SB_BOTTOM: scrollToY(maxScrollY()); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: scrollToY(static_cast<int64_t>(si.nTrackPos) * unit); break;
            }
            return 0;
        }
        case WM_HSCROLL: {
            SCROLLINFO si{sizeof si};
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd_, SB_HORZ, &si);
            const int line = avgCharW_ * 4;
            const int page = std::max(line, clientW_ - scrollAreaLeft() - line);
            switch (LOWORD(wp)) {
                case SB_LINELEFT: scrollBy(0, -line); break;
                case SB_LINERIGHT: scrollBy(0, line); break;
                case SB_PAGELEFT: scrollBy(0, -page); break;
                case SB_PAGERIGHT: scrollBy(0, page); break;
                case SB_LEFT: scrollToX(0); break;
                case SB_RIGHT: scrollToX(maxScrollX()); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: scrollToX(si.nTrackPos); break;
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
                wheelAccumV_ += GET_WHEEL_DELTA_WPARAM(wp);
                int notches = wheelAccumV_ / WHEEL_DELTA;
                if (notches) {
                    wheelAccumV_ -= notches * WHEEL_DELTA;
                    setZoom(opt_.zoom + notches * 10);
                }
                return 0;
            }
            wheel(GET_WHEEL_DELTA_WPARAM(wp), (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0);
            return 0;
        case WM_MOUSEHWHEEL:
            wheel(GET_WHEEL_DELTA_WPARAM(wp), true);
            return TRUE;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            onMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp, msg == WM_LBUTTONDBLCLK);
            return 0;
        case WM_MOUSEMOVE:
            onMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp);
            return 0;
        case WM_LBUTTONUP:
            onMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lp) != hwnd_) {
                drag_ = Drag::None;
                KillTimer(hwnd_, kAutoScrollTimer);
            }
            return 0;
        case WM_TIMER:
            if (wp == kAutoScrollTimer && drag_ != Drag::None) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd_, &pt);
                int64_t dy = 0;
                int dx = 0;
                if (pt.y < headerH_) dy = -rowH_;
                else if (pt.y >= clientH_) dy = rowH_;
                if (pt.x < scrollAreaLeft()) dx = -avgCharW_ * 3;
                else if (pt.x >= clientW_) dx = avgCharW_ * 3;
                if (drag_ == Drag::Cols) dy = 0;
                if (drag_ == Drag::Rows) dx = 0;
                scrollBy(dy, dx);
                dragTo(pt.x, pt.y);
                return 0;
            }
            break;
        case WM_RBUTTONDOWN: {
            SetFocus(hwnd_);
            HitInfo h = hitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (h.what == Hit::Cell && !inSelection(h.row, h.col)) moveCurrent(h.row, h.col, false);
            if (h.what == Hit::Gutter && !(h.row >= selR0_ && h.row <= selR1_ && selKind_ == SelKind::Rows)) {
                anchorRow_ = curRow_ = h.row;
                selKind_ = SelKind::Rows;
                updateSelectionRect();
                InvalidateRect(hwnd_, nullptr, FALSE);
                notify();
            }
            return 0;
        }
        case WM_CONTEXTMENU: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            long headerCol = -1;
            if (pt.x == -1 && pt.y == -1) {
                bool handled = false;
                onKey(VK_APPS, 0, handled);
                return 0;
            }
            POINT cp = pt;
            ScreenToClient(hwnd_, &cp);
            HitInfo h = hitTest(cp.x, cp.y);
            if (h.what == Hit::Header || h.what == Hit::Divider) headerCol = h.col;
            if (onContextMenu) onContextMenu(pt, headerCol);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd_, &pt);
                if (drag_ == Drag::Resize || hitTest(pt.x, pt.y).what == Hit::Divider) {
                    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN: {
            bool handled = false;
            onKey(wp, lp, handled);
            if (!handled) {
                if (wp < 256) forwarded_[wp] = true;
                if (onForwardKey) onForwardKey(msg, wp, lp);
            }
            return 0;
        }
        case WM_KEYUP:
            if (wp < 256 && forwarded_[wp]) {
                forwarded_[wp] = false;
                if (onForwardKey) onForwardKey(msg, wp, lp);
            }
            return 0;
        case WM_CHAR:
            return 0;  // the posted WM_KEYDOWN is translated again by Lister's message loop
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->hwndFrom == tooltip_ && nm->code == TTN_GETDISPINFOW) {
                auto* di = reinterpret_cast<NMTTDISPINFOW*>(lp);
                tipText_.clear();
                if (table_ && hoverCol_ >= 0) {
                    std::u16string text;
                    bool capped = false, show = false;
                    int avail = colWidth(static_cast<size_t>(hoverCol_)) - 2 * padX_ - 1;
                    if (hoverRow_ == -1) {
                        text = captions_[static_cast<size_t>(hoverCol_)];
                        show = measureText(bold_, text) > avail - measureText(bold_, u"▲");
                    } else if (hoverRow_ >= 0 && hoverRow_ < static_cast<long>(order_.size())) {
                        const CellText& ct = cellText(fileRowOfDisplay(static_cast<size_t>(hoverRow_)),
                                                      static_cast<size_t>(hoverCol_));
                        text = ct.text;
                        capped = ct.capped;
                        bool wrappedFits = opt_.lastColumn == LastColumnMode::Wrap &&
                                           hoverCol_ + 1 == static_cast<long>(table_->columns) &&
                                           rowHeight(static_cast<size_t>(hoverRow_)) > rowH_;
                        show = capped || csv::hasLineBreak(text) ||
                               (!wrappedFits && measureText(font_, csv::singleLine(text).text) > avail);
                    }
                    if (show && !text.empty()) {
                        constexpr size_t kTipMax = 4000;
                        if (text.size() > kTipMax) {
                            text.resize(kTipMax);
                            capped = true;
                        }
                        // normalise breaks to CRLF for the tooltip, tabs to spaces
                        for (size_t i = 0; i < text.size(); ++i) {
                            char16_t c = text[i];
                            if (c == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') {
                                tipText_ += L"\r\n";
                                ++i;
                            } else if (c == u'\r' || c == u'\n') {
                                tipText_ += L"\r\n";
                            } else if (c == u'\t') {
                                tipText_ += L' ';
                            } else {
                                tipText_ += static_cast<wchar_t>(c);
                            }
                        }
                        if (capped) tipText_ += L"…";
                    }
                }
                di->lpszText = tipText_.empty() ? const_cast<wchar_t*>(L"") : tipText_.data();
                return 0;
            }
            break;
        }
        case WM_DESTROY:
            KillTimer(hwnd_, kAutoScrollTimer);
            if (tooltip_) {
                DestroyWindow(tooltip_);
                tooltip_ = nullptr;
            }
            break;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            hwnd_ = nullptr;
            return DefWindowProcW(nullptr, msg, wp, lp);
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace csvvw::view
