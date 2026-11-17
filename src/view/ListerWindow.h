// Plugin window handed to Total Commander: notice bar + grid + status bar.
// Owns the load pipeline, the context menus and the settings window.
#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>

#include "GridView.h"
#include "InfoBar.h"
#include "StatusBar.h"
#include "core/Config.h"

namespace csvvw::view {

class ListerWindow {
public:
    static ListerWindow* fromHwnd(HWND h);
    static ListerWindow* create(HWND parent, const std::wstring& file, int showFlags);

    // New file (ListLoad / ListLoadNext). Returns false if the plugin does not handle it.
    bool load(const std::wstring& file, int showFlags);
    int search(const std::wstring& text, int flags);
    int command(int cmd, int param);
    HWND hwnd() const { return hwnd_; }

private:
    ListerWindow() = default;
    bool createWindows(HWND parent);
    bool loadInternal(const std::wstring& file, int showFlags, bool sameFile);
    GridOptions gridOptions() const;
    void applyColors();
    void layout();
    void stateChanged();
    void updateStatus();
    void reportPercent();
    void showContextMenu(POINT screen, long headerCol);
    void runCommand(int id, long headerCol);
    void toggleOption(const char* key, bool& field);
    void setLastColumn(LastColumnMode mode);
    void copy(csv::CopyFormat fmt, bool withHeader);
    void openSettings();
    void saveZoom();

    static LRESULT CALLBACK containerProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    HWND lister_ = nullptr;
    bool quickView_ = false;
    bool dark_ = false;
    std::wstring file_;
    int showFlags_ = 0;
    Config cfg_;
    std::shared_ptr<csv::Table> table_;
    GridView grid_;
    InfoBar info_;
    StatusBar status_;

    // per-file overrides (reset when another file is loaded)
    std::optional<char> delimOverride_;
    std::optional<EncodingChoice> encOverride_;
    std::optional<bool> headerOverride_;
    bool oversized_ = false;

    int zoom_ = 100;       // runtime zoom (kept across ListLoadNext)
    bool zoomInit_ = false;
    int lastPercent_ = -1;
    bool settingsOpen_ = false;
};

}  // namespace csvvw::view
