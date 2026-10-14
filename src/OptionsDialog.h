// TrafficMonitor-style "Option Settings" property sheet and the
// "Display Items" dialog.
#pragma once
#include <windows.h>

#include <functional>

#include "Settings.h"

namespace options {

enum Page { kPageMain, kPageTaskbar, kPageGeneral };

using ApplyFn = std::function<void(const Settings& settings, bool start_with_windows)>;

// Modal; `apply` runs on OK and on Apply. If the sheet is already open it is
// brought to the front instead.
void Show(HINSTANCE instance, HWND owner, const Settings& current, bool start_with_windows, Page start_page,
          const ApplyFn& apply);

// Modal; returns true if the user pressed OK (items updated in place).
bool EditDisplayItems(HINSTANCE instance, HWND owner, const wchar_t* title, ItemConfig items[kItemCount],
                      const WindowLook& defaults);

}  // namespace options
