// Floating "main window", like TrafficMonitor's: a small draggable card.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "Settings.h"
#include "UsageView.h"

class MainWindow {
public:
    static const UINT kMsgContextMenu = WM_APP + 30;  // posted to owner
    static const UINT kMsgDoubleClick = WM_APP + 31;  // posted to owner
    static const UINT kMsgMoved = WM_APP + 32;        // posted to owner; lParam = MAKELPARAM(x, y)

    void Init(HINSTANCE instance, HWND owner);
    bool IsOpen() const { return hwnd_ != nullptr; }
    void Open(const Settings& settings);
    void Close();
    void Update(const std::vector<DisplayItem>& items, const Settings& settings);
    void SetTooltip(const std::wstring& text, bool enabled);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void ApplyWindowStyle(const Settings& settings);
    void ClampToScreen(RECT& r) const;

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND tooltip_ = nullptr;
    std::wstring tooltip_text_;
    bool tooltip_enabled_ = true;
    bool locked_ = false;
    bool allow_offscreen_ = false;

    HFONT font_ = nullptr;
    HFONT small_font_ = nullptr;
    std::wstring font_key_;
    std::wstring render_key_;
};
