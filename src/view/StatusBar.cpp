#include "StatusBar.h"

#include "plugin/WinUtil.h"

namespace csvvw::view {

namespace {
const wchar_t kClass[] = L"TclCsvVw_Status";
}

bool StatusBar::create(HWND parent) {
    static ATOM atom = 0;
    if (!atom) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = &StatusBar::proc;
        wc.hInstance = win::g_instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        atom = RegisterClassExW(&wc);
        if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    hwnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, nullptr, win::g_instance,
                            this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    font_ = CreateFontIndirectW(&ncm.lfStatusFont);
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    lineHeight_ = tm.tmHeight;
    return true;
}

void StatusBar::setColors(COLORREF bg, COLORREF fg, COLORREF sep) {
    bg_ = bg;
    fg_ = fg;
    sep_ = sep;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void StatusBar::setParts(std::vector<std::wstring> parts) {
    if (parts == parts_) return;
    parts_ = std::move(parts);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void StatusBar::show(bool visible) {
    visible_ = visible;
    if (hwnd_) ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
}

int StatusBar::height() const { return visible_ ? lineHeight_ + 6 : 0; }

void StatusBar::move(int x, int y, int w) {
    if (hwnd_) MoveWindow(hwnd_, x, y, w, height(), TRUE);
}

LRESULT CALLBACK StatusBar::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<StatusBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ oldBmp = SelectObject(mem, bmp);
            if (self) {
                SetDCBrushColor(mem, self->bg_);
                FillRect(mem, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                RECT top{0, 0, rc.right, 1};
                SetDCBrushColor(mem, self->sep_);
                FillRect(mem, &top, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                HGDIOBJ oldFont = SelectObject(mem, self->font_);
                SetBkMode(mem, TRANSPARENT);
                int x = 6;
                const int y = 3;
                for (size_t i = 0; i < self->parts_.size(); ++i) {
                    const auto& p = self->parts_[i];
                    if (i) {
                        RECT sep{x, 3, x + 1, rc.bottom - 2};
                        SetDCBrushColor(mem, self->sep_);
                        FillRect(mem, &sep, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                        x += 8;
                    }
                    SIZE sz{};
                    GetTextExtentPoint32W(mem, p.c_str(), static_cast<int>(p.size()), &sz);
                    SetTextColor(mem, self->fg_);
                    ExtTextOutW(mem, x, y, ETO_CLIPPED, &rc, p.c_str(), static_cast<UINT>(p.size()), nullptr);
                    x += sz.cx + 8;
                }
                SelectObject(mem, oldFont);
            }
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            if (self && self->font_) {
                DeleteObject(self->font_);
                self->font_ = nullptr;
            }
            if (self) self->hwnd_ = nullptr;
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace csvvw::view
