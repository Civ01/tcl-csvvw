// Bottom status strip (SPECS §6.1): rows x columns, delimiter, encoding,
// header state, sort state and selection size, separated by vertical bars.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace csvvw::view {

class StatusBar {
public:
    bool create(HWND parent);
    void setColors(COLORREF bg, COLORREF fg, COLORREF sep);
    void setParts(std::vector<std::wstring> parts);
    void show(bool visible);
    bool visible() const { return visible_; }
    int height() const;  // 0 when hidden
    void move(int x, int y, int w);
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<std::wstring> parts_;
    COLORREF bg_ = RGB(240, 240, 240), fg_ = RGB(0, 0, 0), sep_ = RGB(160, 160, 160);
    bool visible_ = true;
    int lineHeight_ = 16;
};

}  // namespace csvvw::view
