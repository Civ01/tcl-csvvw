// Owner-drawn, virtual table control (SPECS §6, §8, §9, §11).
//
// Only the visible rows/columns are drawn. The control owns the per-file view
// state (display order, column widths, selection, scroll position) and talks
// to its owner (ListerWindow) through callbacks.
#pragma once

#include <windows.h>

#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Config.h"
#include "csv/Ops.h"
#include "csv/Table.h"
#include "csv/TextLayout.h"

namespace csvvw::view {

struct GridOptions {
    std::wstring fontFace = L"Segoe UI";
    int fontSize = 10;
    int zoom = 100;
    bool autoSize = true;
    int maxAutoWidth = 400, minColumnWidth = 40, defaultColumnWidth = 100, maxFitWidth = 4000;
    LastColumnMode lastColumn = LastColumnMode::Normal;
    bool multiline = false;
    int maxCellLines = 8;
    bool rowNumbers = true, freezeFirst = false, zebra = true, alignNumbersRight = true;
    size_t maxCellChars = 32768;
    Palette colors = builtinPalette(false);
    bool dark = false;
};

class GridView {
public:
    GridView();
    ~GridView();
    GridView(const GridView&) = delete;
    GridView& operator=(const GridView&) = delete;

    bool create(HWND parent);
    HWND hwnd() const { return hwnd_; }

    // New file: resets order, widths, selection and scroll position.
    void setTable(std::shared_ptr<csv::Table> table, const GridOptions& opt);
    // Same file, new options (keeps order, selection, manual widths where possible).
    void setOptions(const GridOptions& opt);
    const GridOptions& options() const { return opt_; }
    const csv::Table* table() const { return table_.get(); }
    // The table's header flag changed: rebuild order and widths, keep scroll position.
    void headerChanged();

    void setZoom(int zoom);
    int zoom() const { return opt_.zoom; }

    // --- sorting ---
    void sortBy(size_t col, csv::SortDir dir);
    void setSortAs(size_t col, csv::SortAs as);  // re-sorts if col is the sort column
    csv::SortAs sortAs(size_t col) const;
    size_t sortColumn() const { return sortCol_; }
    csv::SortDir sortDir() const { return sortDir_; }
    void setSortKeyFn(csv::SortKeyFn fn) { sortKey_ = std::move(fn); }

    // --- columns ---
    void fitColumn(size_t col);  // ignores max_auto_width (max_fit_width still applies)
    void fitAllColumns();

    // --- selection / clipboard ---
    void selectAll();
    void selectColumns(size_t c0, size_t c1);
    bool hasCells() const;
    csv::CellRange selection() const;
    size_t selectedRows() const;
    size_t selectedCols() const;
    std::wstring copyText(csv::CopyFormat fmt, bool withHeader) const;

    // --- search (TC flags lcs_*) ---
    void setMatcherFactory(std::function<csv::Matcher(const std::wstring&, bool matchCase, bool wholeWords)> f) {
        matcherFactory_ = std::move(f);
    }
    bool search(const std::wstring& text, int flags);

    // --- scrolling ---
    int percent() const;
    void setPercent(int pct);

    // --- callbacks to the owner ---
    std::function<void()> onStateChanged;                      // selection / scroll / sort / zoom
    std::function<void(POINT screen, long headerCol)> onContextMenu;  // headerCol -1 = not on a header
    std::function<void(UINT, WPARAM, LPARAM)> onForwardKey;    // keys for Lister
    std::function<void(int cmd)> onShortcut;                   // IDM_* triggered by keyboard

private:
    // geometry -------------------------------------------------------------
    double scale() const;
    int px(int base) const;
    void createFonts();
    void destroyFonts();
    void computeMetrics();
    void computeBaseWidths(bool keepManual);
    int measureFitPx(size_t col, bool withHeader);
    int measureText(HFONT f, std::u16string_view s);
    void layoutColumns();
    void layoutRows();
    bool variableRows() const;
    int rowLinesOf(size_t displayRow);
    int wrapLinesOf(size_t fileRow, bool exact);
    int wrapWidthPx() const;  // text width available for wrapping in the last column
    int64_t rowTop(size_t displayRow) const;
    int rowHeight(size_t displayRow) const;
    int64_t totalHeight() const;
    size_t rowAtY(int64_t contentY) const;  // content Y -> display row (clamped)
    int bodyTop() const { return headerH_; }
    int bodyHeight() const;
    int scrollAreaLeft() const;  // x where scrolled columns start (after gutter + frozen column)
    int colScreenX(size_t col) const;
    int colWidth(size_t col) const { return colX_[col + 1] - colX_[col]; }
    int64_t maxScrollY() const;
    int maxScrollX() const;
    void clampScroll();
    void updateScrollBars();
    void relayout(bool keepTopRow = true);

    // cell text ------------------------------------------------------------
    struct CellText {
        std::u16string text;  // capped at max_cell_chars
        bool capped = false;
    };
    struct RowCache {
        std::vector<CellText> cells;
        uint64_t stamp = 0;
    };
    const RowCache& rowCells(size_t fileRow) const;
    CellText cellTextUncached(size_t fileRow, size_t col) const;
    const CellText& cellText(size_t fileRow, size_t col) const;
    size_t fileRowOfDisplay(size_t displayRow) const;

    // painting -------------------------------------------------------------
    void paint(HDC dc, const RECT& clip);
    void paintHeader(HDC dc);
    void paintGutter(HDC dc, size_t r0, size_t r1);
    void paintRows(HDC dc, size_t r0, size_t r1, size_t c0, size_t c1, int clipLeft, int clipRight);
    void paintCell(HDC dc, const RECT& rc, size_t displayRow, size_t col, bool selected);
    void drawLine(HDC dc, const RECT& rc, int y, const csv::DisplayLine& line, bool forceEllipsis, bool rightAlign,
                  COLORREF textColor, std::optional<std::pair<size_t, size_t>> highlight);
    void drawLineImpl(HDC dc, const RECT& rc, int y, const csv::DisplayLine& line, bool forceEllipsis,
                      bool rightAlign, COLORREF textColor, std::optional<std::pair<size_t, size_t>> highlight);

    // interaction ----------------------------------------------------------
    enum class Hit { None, Corner, Header, Divider, Gutter, Cell };
    struct HitInfo {
        Hit what = Hit::None;
        long row = -1;  // display row
        long col = -1;
    };
    HitInfo hitTest(int x, int y) const;
    void onMouseDown(int x, int y, WPARAM keys, bool dbl);
    void onMouseMove(int x, int y, WPARAM keys);
    void onMouseUp(int x, int y);
    void dragTo(int x, int y);
    void onKey(WPARAM vk, LPARAM lp, bool& handled);
    void moveCurrent(long row, long col, bool extend);
    void setCurrent(long row, long col);
    void ensureVisible(long row, long col);
    void scrollBy(int64_t dy, int dx);
    void scrollToY(int64_t y);
    void scrollToX(int x);
    void wheel(int delta, bool horizontal);
    void notify();
    void updateTooltip(int x, int y);
    void applySort();

    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    HWND tooltip_ = nullptr;
    std::shared_ptr<csv::Table> table_;
    GridOptions opt_;

    // order and sort
    std::vector<uint32_t> order_;  // display row -> data row
    size_t sortCol_ = SIZE_MAX;
    csv::SortDir sortDir_ = csv::SortDir::None;
    std::unordered_map<size_t, csv::SortAs> sortAs_;
    csv::SortKeyFn sortKey_;
    std::function<csv::Matcher(const std::wstring&, bool, bool)> matcherFactory_;

    // fonts / metrics
    HFONT font_ = nullptr, bold_ = nullptr;
    int dpi_ = 96;
    int lineH_ = 16, rowH_ = 20, padX_ = 4, padY_ = 2, headerH_ = 20, gutterW_ = 0;
    int maxCharW_ = 10, avgCharW_ = 7, ellipsisW_ = 8;
    char16_t breakGlyph_ = csv::kLineBreakGlyph;  // ¶ when the font has no ↵
    HDC measureDc_ = nullptr;

    // columns: base widths (100% zoom, 96 DPI); colX_ in pixels, content coordinates
    std::vector<int> baseW_;
    std::vector<bool> manualW_;
    std::vector<int> colX_;
    int lastColPx_ = 0;  // width of the last column (fit / wrap modes)

    // rows (variable heights only)
    std::vector<int64_t> rowY_;         // display order prefix sums (size rows+1)
    std::vector<uint8_t> wrapLines_;    // per file row; 0 = unknown
    std::vector<bool> wrapEstimated_;   // per file row
    bool wrapExact_ = true;

    // scroll (pixels in content coordinates)
    int64_t scrollY_ = 0;
    int scrollX_ = 0;
    int clientW_ = 0, clientH_ = 0;
    int wheelAccumV_ = 0, wheelAccumH_ = 0;

    // selection (display coordinates)
    enum class SelKind { Cells, Rows, Cols };
    SelKind selKind_ = SelKind::Cells;
    long anchorRow_ = 0, anchorCol_ = 0, curRow_ = 0, curCol_ = 0;
    long selR0_ = 0, selR1_ = 0, selC0_ = 0, selC1_ = 0;  // inclusive
    void updateSelectionRect();
    bool inSelection(long row, long col) const;

    // search hit (and the current cell at the time of the hit)
    std::optional<csv::SearchHit> hit_;
    long hitCurRow_ = -2, hitCurCol_ = -2;

    // per-column cached info
    std::vector<std::u16string> captions_;
    std::vector<bool> numericCol_;
    void refreshColumnInfo();
    bool forwarded_[256] = {};

    // mouse
    enum class Drag { None, Cells, Rows, Cols, Resize, HeaderPress };
    Drag drag_ = Drag::None;
    long dragCol_ = -1;   // resize: column; header press: pressed column
    int dragStartX_ = 0, dragStartW_ = 0;
    POINT pressPt_{};
    POINT lastMouse_{};
    long hoverRow_ = LONG_MIN, hoverCol_ = -1;
    std::wstring tipText_;

    // cache
    mutable std::unordered_map<size_t, RowCache> cache_;
    mutable uint64_t cacheStamp_ = 0;

    // back buffer
    HDC backDc_ = nullptr;
    HBITMAP backBmp_ = nullptr;
    HGDIOBJ backOld_ = nullptr;
    int backW_ = 0, backH_ = 0;
};

}  // namespace csvvw::view
