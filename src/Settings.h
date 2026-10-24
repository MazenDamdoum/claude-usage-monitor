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
    bool bar_quarters = true;  // usage bars cut into four, to read 25/50/75/100% at a glance
    bool percent_space = true;
    bool hide_percent = false;
    bool tooltip = true;
    ItemConfig items[kItemCount];
};

struct Settings {
    // General
    int poll_seconds = 60;          // interval while Claude Code is idle
    bool live = true;               // check as fast as allowed while Claude Code works
    std::wstring credentials_path;  // empty = Claude Code default location
    int warn_percent = 80;
    int critical_percent = 95;
    bool notify = true;
    bool show_tray_icon = true;
    bool phone_notifications = false;  // forward reset events to the push server (push/)
    bool accounts_enabled = true;      // show every claude-swap account
    bool auto_switch = true;           // switch accounts when the active one runs low
    int auto_switch_percent = 95;      // ...at this 5h or weekly usage
    std::wstring cswap_path;           // empty = find cswap.exe automatically
    std::wstring your_name;            // how account owners see you in their alerts ("Nayef started using...")

    // Taskbar window
    bool taskbar_visible = true;
    WindowLook taskbar;
    bool taskbar_transparent = true;
    bool taskbar_auto_colors = true;
    bool taskbar_right_align = true;
    bool taskbar_horizontal = false;
    bool taskbar_tokens = true;  // the live token-speed column (Display Settings > Token speed)
    bool taskbar_token_pile = false;  // count tokens up (a running total, like Claude Code) instead of per second
    bool taskbar_blossom = true;      // Claude's spinner beside the token speed
    bool taskbar_switch_glow = true;  // the account's name glows green once a switch to it is confirmed
    bool taskbar_active_first = true; // the account in use comes first (the accounts swap places)
    bool taskbar_dim_standby = true;  // accounts not in use are drawn a little dimmer
    bool taskbar_long_unit = false;   // "tokens/s" instead of "tok/s"
    bool taskbar_check_timer = true;  // the ring at the far right: time to the next usage check
    bool taskbar_fit = true;          // drop detail to fit the taskbar's space (off: always everything)
    bool taskbar_collapsed = false;   // only the blossom, at the far right (clicking it toggles this)
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
