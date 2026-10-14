// The readout embedded in the Windows taskbar, the same way TrafficMonitor
// does it: a layered child window of Shell_TrayWnd placed just left of the
// notification area (and left of TrafficMonitor, if it is there).
#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "Settings.h"
#include "UsageView.h"

class TaskbarWidget {
public:
    static const UINT kMsgContextMenu = WM_APP + 20;  // posted to owner
    static const UINT kMsgDoubleClick = WM_APP + 21;  // posted to owner

    void Init(HINSTANCE instance, HWND owner);
    void Destroy();
    // Call about once a second: re-attaches if Explorer restarted, follows the
    // tray as it grows/shrinks, and repaints when anything visible changed.
    void Update(const std::vector<DisplayItem>& items, const Settings& settings);
    void SetTooltip(const std::wstring& text, bool enabled);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool Attach();
    int RightEdge(int dpi) const;

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND taskbar_ = nullptr;
    HWND tooltip_ = nullptr;
    std::wstring tooltip_text_;
    bool tooltip_enabled_ = true;

    HFONT font_ = nullptr;
    std::wstring font_key_;
    std::wstring render_key_;
    RECT placed_ = {};
};
