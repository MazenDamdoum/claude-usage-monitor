// settings.ini: next to the exe if one exists there (portable), otherwise
// %APPDATA%\ClaudeUsageMonitor\settings.ini. Edited through the Options dialog.
#pragma once
#include <windows.h>

#include <string>

enum ItemId { kItemSession, kItemWeekly, kItemSonnet, kItemOpus, kItemCount };

extern const wchar_t* const kItemNames[kItemCount];  // "Session (5h)", ...

struct ItemConfig {
    bool show = false;
    std::wstring label;
};

// Look shared by the taskbar window and the main (floating) window.
struct WindowLook {
    std::wstring font_name = L"Segoe UI";
    int font_size = 9;
    bool font_bold = false;
    COLORREF label_color = RGB(255, 255, 255);
    COLORREF value_color = RGB(255, 255, 255);
    COLORREF bar_color = RGB(217, 119, 87);
    COLORREF background_color = RGB(0, 0, 0);
    bool show_reset = true;
    bool show_bars = false;
    bool percent_space = true;
    bool hide_percent = false;
    bool tooltip = true;
    ItemConfig items[kItemCount];
};

struct Settings {
    // General
    int poll_seconds = 60;
    std::wstring credentials_path;  // empty = Claude Code default location
    int warn_percent = 80;
    int critical_percent = 95;
    bool notify = true;
    bool show_tray_icon = true;

    // Taskbar window
    bool taskbar_visible = true;
    WindowLook taskbar;
    bool taskbar_transparent = true;
    bool taskbar_auto_colors = true;
    bool taskbar_right_align = true;
    bool taskbar_horizontal = false;
    int taskbar_offset_x = 0;

    // Main window
    bool main_visible = false;
    WindowLook main;
    int main_opacity = 94;  // percent
    bool main_topmost = true;
    bool main_click_through = false;
    bool main_locked = false;
    bool main_offscreen = false;
    bool main_has_position = false;
    int main_x = 0;
    int main_y = 0;

    static Settings Defaults();
    static WindowLook DefaultTaskbarLook();
    static WindowLook DefaultMainLook();
    static std::wstring Path();
    static Settings Load();
    void Save() const;
};
