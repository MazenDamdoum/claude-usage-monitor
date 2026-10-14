// Claude Usage Monitor: live Claude plan usage in the Windows taskbar.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "Canvas.h"
#include "MainWindow.h"
#include "OptionsDialog.h"
#include "Settings.h"
#include "TaskbarWidget.h"
#include "UsageClient.h"
#include "UsageIcon.h"
#include "UsagePoller.h"
#include "UsageView.h"
#include "resource.h"
#include "version.h"

using namespace claude_usage;

namespace {

const wchar_t kAppClass[] = L"ClaudeUsageMonitor.App";
const wchar_t kAppName[] = L"Claude Usage Monitor";
const UINT kMsgUsage = WM_APP + 1;
const UINT kMsgTray = WM_APP + 2;
const UINT kMsgRemote = WM_APP + 3;  // wParam = Command, from a second launch
const UINT_PTR kTickTimer = 1;
const UINT kTrayId = 1;

enum Command {
    kCmdRefresh = 100,
    kCmdDisplay,
    kCmdToggleMain,
    kCmdToggleTaskbar,
    kCmdOptions,
    kCmdExit,
};

const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunValue[] = L"ClaudeUsageMonitor";

std::wstring ExePath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::wstring StartupCommand() { return L"\"" + ExePath() + L"\""; }

std::wstring ReadStartupEntry() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

void SetStartup(bool enabled) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enabled) {
        std::wstring cmd = StartupCommand();
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

ULONGLONG LastWriteTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

// cache.ini next to settings.ini: last good numbers plus the next allowed check,
// so a restart shows data instantly and doesn't trigger an extra request.
std::wstring DataPath(const wchar_t* file) {
    std::wstring path = Settings::Path();
    return path.substr(0, path.find_last_of(L"\\/") + 1) + file;
}

const wchar_t* const kCacheKeys[kItemCount] = {L"five_hour", L"seven_day", L"seven_day_sonnet", L"seven_day_opus"};

void SaveCache(const UsageSnapshot& s) {
    std::wstring path = DataPath(L"cache.ini");
    auto put = [&](const wchar_t* section, const wchar_t* key, long long v) {
        WritePrivateProfileStringW(section, key, std::to_wstring(v).c_str(), path.c_str());
    };
    put(L"cache", L"next_attempt", s.next_attempt);
    if (!s.has_data) return;
    put(L"cache", L"fetched_at", s.fetched_at);
    WritePrivateProfileStringW(L"cache", L"plan", Utf8ToWide(s.data.subscription_type).c_str(), path.c_str());
    for (int i = 0; i < kItemCount; ++i) {
        const UsageWindow& w = WindowFor(s.data, i);
        put(kCacheKeys[i], L"present", w.present);
        put(kCacheKeys[i], L"percent_x100", std::llround(w.utilization * 100));
        put(kCacheKeys[i], L"resets_at", w.resets_at);
    }
}

bool LoadCache(UsageSnapshot& s) {
    std::wstring path = DataPath(L"cache.ini");
    auto get = [&](const wchar_t* section, const wchar_t* key) {
        wchar_t buf[32] = {};
        GetPrivateProfileStringW(section, key, L"0", buf, 32, path.c_str());
        return std::wcstoll(buf, nullptr, 10);
    };
    s.next_attempt = get(L"cache", L"next_attempt");
    s.fetched_at = get(L"cache", L"fetched_at");
    s.has_data = s.fetched_at > 0;
    if (s.has_data) {
        wchar_t plan[64] = {};
        GetPrivateProfileStringW(L"cache", L"plan", L"", plan, 64, path.c_str());
        s.data.subscription_type = WideToUtf8(plan);
        UsageWindow* windows[kItemCount] = {&s.data.five_hour, &s.data.seven_day, &s.data.seven_day_sonnet,
                                            &s.data.seven_day_opus};
        for (int i = 0; i < kItemCount; ++i) {
            windows[i]->present = get(kCacheKeys[i], L"present") != 0;
            windows[i]->utilization = get(kCacheKeys[i], L"percent_x100") / 100.0;
            windows[i]->resets_at = get(kCacheKeys[i], L"resets_at");
        }
    }
    return s.has_data || s.next_attempt > 0;
}

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}

    int Run() {
        settings_ = Settings::Load();
        settings_stamp_ = LastWriteTime(Settings::Path());
        taskbar_created_msg_ = RegisterWindowMessageW(L"TaskbarCreated");

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &App::WndProc;
        wc.hInstance = instance_;
        wc.lpszClassName = kAppClass;
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kAppClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                instance_, this);
        if (!hwnd_) return 1;
        ChangeWindowMessageFilterEx(hwnd_, taskbar_created_msg_, MSGFLT_ALLOW, nullptr);

        poller_.Configure(settings_.poll_seconds, settings_.credentials_path);
        UsageSnapshot cached;
        if (LoadCache(cached)) {
            long long not_before = std::max(cached.next_attempt, cached.fetched_at + settings_.poll_seconds);
            poller_.Seed(cached, not_before);
        }
        poller_.Start(hwnd_, kMsgUsage);
        widget_.Init(instance_, hwnd_);
        main_window_.Init(instance_, hwnd_);
        SyncWindows();

        // Keep the autostart entry pointing at this exe if it was moved.
        std::wstring entry = ReadStartupEntry();
        if (!entry.empty() && entry != StartupCommand()) SetStartup(true);

        SetTimer(hwnd_, kTickTimer, 1000, nullptr);
        Tick();

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        RemoveTray();
        widget_.Destroy();
        main_window_.Close();
        return 0;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) return self->Handle(hwnd, msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == taskbar_created_msg_ && msg != 0) {  // Explorer restarted
            tray_added_ = false;
            Tick();
            return 0;
        }
        switch (msg) {
        case kMsgUsage:
            LogAttempt();
            SaveCache(poller_.Snapshot());
            Tick();
            return 0;
        case WM_TIMER:
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            Tick();
            return 0;
        case kMsgRemote:
            OnCommand(static_cast<int>(wp));
            return 0;
        case kMsgTray:
            switch (LOWORD(lp)) {
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                ShowMenu();
                break;
            case WM_LBUTTONDBLCLK:
                OnCommand(kCmdOptions);
                break;
            default:
                break;
            }
            return 0;
        case TaskbarWidget::kMsgContextMenu:
        case MainWindow::kMsgContextMenu:
            ShowMenu();
            return 0;
        case TaskbarWidget::kMsgDoubleClick:
        case MainWindow::kMsgDoubleClick:
            OnCommand(kCmdOptions);
            return 0;
        case MainWindow::kMsgMoved:
            settings_.main_has_position = true;
            settings_.main_x = static_cast<short>(LOWORD(lp));
            settings_.main_y = static_cast<short>(HIWORD(lp));
            SaveSettings();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTickTimer);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    // One line per usage check in app.log next to settings.ini (kept under 64 KB).
    void LogAttempt() {
        UsageSnapshot s = poller_.Snapshot();
        std::wstring path = DataPath(L"app.log");
        WIN32_FILE_ATTRIBUTE_DATA info;
        bool exists = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) != FALSE;
        if (exists && info.nFileSizeLow > 64 * 1024) DeleteFileW(path.c_str());

        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[32];
        swprintf(stamp, 32, L"%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::wstring line = std::wstring(stamp) + L"  ";
        if (s.last_error.empty()) {
            line += L"ok  5h=" + std::to_wstring(RoundPercent(s.data.five_hour)) +
                    L"%  7d=" + std::to_wstring(RoundPercent(s.data.seven_day)) + L"%";
        } else {
            line += L"error " + std::to_wstring(static_cast<int>(s.last_status)) + L"  " + s.last_error;
        }
        std::string utf8 = WideToUtf8(line) + "\r\n";
        HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        CloseHandle(h);
    }

    // ---- settings ------------------------------------------------------------

    void SaveSettings() {
        settings_.Save();
        settings_stamp_ = LastWriteTime(Settings::Path());
    }

    void ReloadSettingsIfChanged() {
        ULONGLONG stamp = LastWriteTime(Settings::Path());
        if (stamp == settings_stamp_) return;
        settings_stamp_ = stamp;
        settings_ = Settings::Load();
        poller_.Configure(settings_.poll_seconds, settings_.credentials_path);
        SyncWindows();
    }

    void ApplySettings(Settings s, bool start_with_windows) {
        // The main window may have been dragged while a dialog was open.
        s.main_has_position = settings_.main_has_position;
        s.main_x = settings_.main_x;
        s.main_y = settings_.main_y;
        // Never leave the app with no way to reach its menu.
        if (s.main_click_through || (!s.taskbar_visible && !s.main_visible)) s.show_tray_icon = true;
        if (start_with_windows != !ReadStartupEntry().empty()) SetStartup(start_with_windows);
        settings_ = s;
        SaveSettings();
        poller_.Configure(settings_.poll_seconds, settings_.credentials_path);
        SyncWindows();
        Tick();
    }

    void SyncWindows() {
        if (!settings_.taskbar_visible) widget_.Destroy();
        if (settings_.main_visible && !main_window_.IsOpen()) main_window_.Open(settings_);
        if (!settings_.main_visible && main_window_.IsOpen()) main_window_.Close();
    }

    // ---- periodic update -----------------------------------------------------

    void Tick() {
        ReloadSettingsIfChanged();
        UsageSnapshot s = poller_.Snapshot();
        long long now = NowUnix();
        const FetchResult& d = s.data;

        // A limit window just rolled over: fetch fresh numbers instead of waiting a full poll.
        for (const UsageWindow* w : {&d.five_hour, &d.seven_day}) {
            if (s.has_data && w->resets_at > 0 && now >= w->resets_at + 5 && s.fetched_at < w->resets_at &&
                refreshed_for_ != w->resets_at) {
                refreshed_for_ = w->resets_at;
                poller_.RefreshNow();
            }
        }

        std::wstring tooltip = BuildTooltip(s, now);
        if (settings_.taskbar_visible) {
            widget_.Update(BuildItems(s, settings_.taskbar, settings_.warn_percent, settings_.critical_percent, now),
                           settings_);
            widget_.SetTooltip(tooltip, settings_.taskbar.tooltip);
        }
        if (main_window_.IsOpen()) {
            main_window_.Update(BuildItems(s, settings_.main, settings_.warn_percent, settings_.critical_percent, now),
                                settings_);
            main_window_.SetTooltip(tooltip, settings_.main.tooltip);
        }
        UpdateTray(s);
        CheckAlerts(s, now);
    }

    // ---- notification area ---------------------------------------------------

    void UpdateTray(const UsageSnapshot& s) {
        if (!settings_.show_tray_icon) {
            RemoveTray();
            return;
        }
        const UsageWindow& session = s.data.five_hour;
        bool ok = s.has_data && session.present;
        int pct = ok ? static_cast<int>(RoundPercent(session)) : -1;
        int sev = ok ? Severity(session, settings_.warn_percent, settings_.critical_percent) : 0;
        std::wstring tip;
        if (s.has_data) {
            const UsageWindow& week = s.data.seven_day;
            tip = L"Claude  ·  5h " + (session.present ? std::to_wstring(RoundPercent(session)) + L"%" : L"--") +
                  L"  ·  7d " + (week.present ? std::to_wstring(RoundPercent(week)) + L"%" : L"--");
        } else {
            tip = s.last_error.empty() ? L"Claude usage: loading..." : L"Claude usage: " + s.last_error;
        }
        if (tray_added_ && pct == tray_pct_ && sev == tray_sev_ && tip == tray_tip_) return;

        int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(hwnd_));
        HICON icon = CreateUsageIcon(size > 0 ? size : 16, pct, sev);
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        nid.uCallbackMessage = kMsgTray;
        nid.hIcon = icon;
        lstrcpynW(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip));
        BOOL ok_tray = Shell_NotifyIconW(tray_added_ ? NIM_MODIFY : NIM_ADD, &nid);
        if (!ok_tray && tray_added_) ok_tray = Shell_NotifyIconW(NIM_ADD, &nid);
        tray_added_ = ok_tray != FALSE;
        if (tray_icon_) DestroyIcon(tray_icon_);
        tray_icon_ = icon;
        tray_pct_ = pct;
        tray_sev_ = sev;
        tray_tip_ = tip;
    }

    void RemoveTray() {
        if (!tray_added_) return;
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        tray_added_ = false;
    }

    // Balloon when the session or weekly limit climbs to the warning/critical level.
    void CheckAlerts(const UsageSnapshot& s, long long now) {
        if (!s.has_data) return;
        const int ids[2] = {kItemSession, kItemWeekly};
        for (int k = 0; k < 2; ++k) {
            const UsageWindow& w = WindowFor(s.data, ids[k]);
            if (!w.present) continue;
            int level = Severity(w, settings_.warn_percent, settings_.critical_percent);
            if (w.resets_at != alert_reset_[k]) {  // new window (or first data): start fresh
                alert_reset_[k] = w.resets_at;
                alert_level_[k] = alerts_primed_ ? 0 : level;
            }
            if (level > alert_level_[k] && settings_.notify && tray_added_) {
                std::wstring title = std::wstring(L"Claude ") + (k == 0 ? L"session" : L"weekly") + L" usage at " +
                                     std::to_wstring(RoundPercent(w)) + L"%";
                std::wstring text = w.resets_at > 0 ? L"Resets in " + FormatDuration(w.resets_at - now) + L" (" +
                                                          FormatLocalTime(w.resets_at) + L")."
                                                    : L"";
                NOTIFYICONDATAW nid = {};
                nid.cbSize = sizeof(nid);
                nid.hWnd = hwnd_;
                nid.uID = kTrayId;
                nid.uFlags = NIF_INFO;
                nid.dwInfoFlags = level == 2 ? NIIF_WARNING : NIIF_INFO;
                lstrcpynW(nid.szInfoTitle, title.c_str(), ARRAYSIZE(nid.szInfoTitle));
                lstrcpynW(nid.szInfo, text.c_str(), ARRAYSIZE(nid.szInfo));
                Shell_NotifyIconW(NIM_MODIFY, &nid);
            }
            alert_level_[k] = level;
        }
        alerts_primed_ = true;
    }

    // ---- menu ----------------------------------------------------------------

    void ShowMenu() {
        HMENU menu = CreatePopupMenu();
        MENUINFO mi = {};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIM_STYLE;
        mi.dwStyle = MNS_CHECKORBMP;
        SetMenuInfo(menu, &mi);

        int icon_size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(hwnd_));
        COLORREF icon_color = GetSysColor(COLOR_MENUTEXT);
        std::vector<HBITMAP> bitmaps;
        auto add = [&](UINT id, const wchar_t* text, wchar_t icon, bool checked) {
            MENUITEMINFOW item = {};
            item.cbSize = sizeof(item);
            item.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | (icon ? MIIM_BITMAP : 0);
            item.wID = id;
            item.dwTypeData = const_cast<wchar_t*>(text);
            item.fState = checked ? MFS_CHECKED : MFS_UNCHECKED;
            if (icon) {
                item.hbmpItem = CreateGlyphBitmap(icon, icon_size, icon_color);
                bitmaps.push_back(item.hbmpItem);
            }
            InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &item);
        };
        auto separator = [&] { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };

        add(kCmdRefresh, L"Refresh Now", glyph::kRefresh, false);
        separator();
        add(kCmdDisplay, L"Display Settings...", glyph::kDisplay, false);
        add(kCmdToggleMain, L"Show Main Window", 0, settings_.main_visible);
        add(kCmdToggleTaskbar, settings_.taskbar_visible ? L"Close Taskbar Window" : L"Show Taskbar Window",
            settings_.taskbar_visible ? glyph::kClose : glyph::kTaskbar, false);
        separator();
        add(kCmdOptions, L"Options...", glyph::kSettings, false);
        add(kCmdExit, L"Exit", glyph::kExit, false);

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(menu);
        for (HBITMAP b : bitmaps)
            if (b) DeleteObject(b);
        if (cmd) OnCommand(cmd);
    }

    void OnCommand(int cmd) {
        bool startup = !ReadStartupEntry().empty();
        switch (cmd) {
        case kCmdRefresh:
            poller_.RefreshNow();
            break;
        case kCmdDisplay: {
            Settings s = settings_;
            bool taskbar = s.taskbar_visible || !s.main_visible;
            WindowLook& look = taskbar ? s.taskbar : s.main;
            WindowLook def = taskbar ? Settings::DefaultTaskbarLook() : Settings::DefaultMainLook();
            if (options::EditDisplayItems(instance_, nullptr,
                                          taskbar ? L"Taskbar Window Display Items" : L"Main Window Display Items",
                                          look.items, def))
                ApplySettings(s, startup);
            break;
        }
        case kCmdToggleMain: {
            Settings s = settings_;
            s.main_visible = !s.main_visible;
            ApplySettings(s, startup);
            break;
        }
        case kCmdToggleTaskbar: {
            Settings s = settings_;
            s.taskbar_visible = !s.taskbar_visible;
            ApplySettings(s, startup);
            break;
        }
        case kCmdOptions:
            options::Show(instance_, nullptr, settings_, startup, options::kPageTaskbar,
                          [this](const Settings& s, bool start) { ApplySettings(s, start); });
            break;
        case kCmdExit:
            DestroyWindow(hwnd_);
            break;
        default:
            break;
        }
    }

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    UINT taskbar_created_msg_ = 0;
    Settings settings_;
    ULONGLONG settings_stamp_ = 0;
    UsagePoller poller_;
    TaskbarWidget widget_;
    MainWindow main_window_;
    long long refreshed_for_ = 0;

    bool tray_added_ = false;
    HICON tray_icon_ = nullptr;
    int tray_pct_ = -2;
    int tray_sev_ = -1;
    std::wstring tray_tip_;

    bool alerts_primed_ = false;
    long long alert_reset_[2] = {};
    int alert_level_[2] = {};
};

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\ClaudeUsageMonitor.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: launching again opens Options.
        HWND running = FindWindowW(kAppClass, nullptr);
        if (running) {
            DWORD pid = 0;
            GetWindowThreadProcessId(running, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(running, kMsgRemote, kCmdOptions, 0);
        }
        return 0;
    }

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_WIN95_CLASSES | ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&icc);

    // Never freed: the poller thread may still be running when we exit.
    App* app = new App(instance);
    ExitProcess(static_cast<UINT>(app->Run()));
}
