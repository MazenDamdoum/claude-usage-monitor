// Claude Usage Monitor: live Claude plan usage in the Windows taskbar.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include "Accounts.h"
#include "AppFiles.h"
#include "Canvas.h"
#include "MainWindow.h"
#include "OptionsDialog.h"
#include "PromptStats.h"
#include "PushBridge.h"
#include "ResetGrants.h"
#include "Settings.h"
#include "TaskbarWidget.h"
#include "UsageClient.h"
#include "UsageIcon.h"
#include "UsagePoller.h"
#include "UsageView.h"
#include "resource.h"
#include "mini_json.h"
#include "version.h"

using namespace claude_usage;

namespace {

const wchar_t kAppClass[] = L"ClaudeUsageMonitor.App";
const wchar_t kAppName[] = L"Claude Usage Monitor";
// Window title: kAppName plus CUM_INSTANCE, so a test copy and the real app never find each other.
std::wstring g_window_title = kAppName;
const UINT kMsgUsage = WM_APP + 1;
const UINT kMsgAccounts = WM_APP + 4;  // AccountsSource has a new snapshot
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
    kCmdExit,               // hide to the tray (keeps running: switching, alerts, phone)
    kCmdQuit,               // really close
    kCmdAccountsRefresh,
    kCmdAccountAdd,
    kCmdLimitHit,           // Claude Code stopped on a usage limit (StopFailure hook: --limit-hit)
    kCmdAccountBase = 200,  // + account number: switch to it
    kCmdRenameBase = 300,   // + account number: rename it
    kCmdRemoveBase = 400,   // + account number: remove it
    kCmdLinkBase = 500,     // + account number: phone link for its owner; + 0: for your own phone
};
const UINT kMsgPhoneLink = WM_APP + 50;  // lParam = new std::wstring: the link, or "!" + a problem
const UINT kMsgPhoneSwitch = WM_APP + 51;  // lParam = new std::wstring: the account the phone picked
const UINT kMsgSwitchConfirmed = WM_APP + 52;  // lParam = new std::wstring: whose login Claude Code now has

// After claude-swap swaps the login: read Claude Code's credentials file and ask whose login it is
// (an identity lookup, not a usage check), until it's the account switched to (up to 20 s).
struct VerifyJob {
    HWND hwnd;
    std::wstring credentials;
    std::wstring email;
};
DWORD WINAPI VerifySwitchThread(LPVOID param) {
    std::unique_ptr<VerifyJob> job(static_cast<VerifyJob*>(param));
    std::string want = WideToUtf8(job->email);
    for (char& c : want) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (int i = 0; i < 20; ++i) {
        if (LoginEmail(job->credentials) == want) {
            PostMessageW(job->hwnd, kMsgSwitchConfirmed, 0, reinterpret_cast<LPARAM>(new std::wstring(job->email)));
            return 0;
        }
        Sleep(1000);
    }
    AppendAppLog(L"accounts: couldn't confirm the switch to " + job->email + L" yet; the next usage check will");
    return 0;
}

// Asks the cloud relay for a one-time phone link (background thread: it's a network call).
struct LinkRequest {
    HWND hwnd;
    std::string body;
};
DWORD WINAPI PhoneLinkThread(LPVOID param) {
    LinkRequest* job = static_cast<LinkRequest*>(param);
    auto* result = new std::wstring;
    std::wstring url;
    std::string token;
    if (!ReadCloudConfig(url, token)) {
        *result = L"!Phone links need the cloud relay. Set it up once with cloud\\setup.ps1.";
    } else {
        std::string reply;
        int status = PostJsonToUrl(url + L"/ingest/pair", token, job->body, &reply);
        SecureZeroMemory(&token[0], token.size());
        mini_json::Value root;
        const mini_json::Value* code = status == 200 && mini_json::Parser(reply).Parse(root) ? root.Get("code") : nullptr;
        if (code && code->IsString())
            *result = url + L"/join/" + Utf8ToWide(code->string);
        else if (status == 404)
            *result = L"!That account hasn't reached the cloud yet. Wait a minute and try again.";
        else
            *result = L"!Couldn't reach the cloud relay" + (status ? L" (HTTP " + std::to_wstring(status) + L")." : std::wstring(L". Are you online?"));
    }
    PostMessageW(job->hwnd, kMsgPhoneLink, 0, reinterpret_cast<LPARAM>(result));
    delete job;
    return 0;
}

bool SameEmail(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && a.size() == b.size() && CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool CopyToClipboard(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    bool ok = false;
    if (mem) {
        std::memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

// A name claude-swap accepts as an alias: letters, digits, . - _ and not purely numeric.
bool ValidAccountName(const std::wstring& s) {
    if (s.empty() || s.size() > 24) return false;
    bool non_digit = false;
    for (wchar_t c : s) {
        bool alnum = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
        if (!alnum && c != L'.' && c != L'-' && c != L'_') return false;
        if (!(c >= L'0' && c <= L'9')) non_digit = true;
    }
    return non_digit;
}

struct NameDialogArgs {
    std::wstring title;
    std::wstring prompt;
    std::wstring value;
};

INT_PTR CALLBACK NameDialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* args = reinterpret_cast<NameDialogArgs*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        args = reinterpret_cast<NameDialogArgs*>(lp);
        SetWindowTextW(dlg, args->title.c_str());
        SetDlgItemTextW(dlg, IDC_RENAME_TEXT, args->prompt.c_str());
        SetDlgItemTextW(dlg, IDC_RENAME_EDIT, args->value.c_str());
        SendDlgItemMessageW(dlg, IDC_RENAME_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, IDC_RENAME_EDIT));
        SetForegroundWindow(dlg);
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[64] = {};
            GetDlgItemTextW(dlg, IDC_RENAME_EDIT, buf, 64);
            std::wstring name = buf;
            while (!name.empty() && name.back() == L' ') name.pop_back();
            while (!name.empty() && name.front() == L' ') name.erase(0, 1);
            if (!ValidAccountName(name)) {
                MessageBoxW(dlg, L"Use letters, numbers, . - or _ (up to 24, not only numbers).", args->title.c_str(),
                            MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            args->value = name;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

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
    if (s.spacing > 0) put(L"cache", L"spacing", s.spacing);
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
    s.spacing = static_cast<int>(get(L"cache", L"spacing"));
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
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kAppClass, g_window_title.c_str(), WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                instance_, this);
        if (!hwnd_) return 1;
        ChangeWindowMessageFilterEx(hwnd_, taskbar_created_msg_, MSGFLT_ALLOW, nullptr);

        poller_.Configure(settings_.poll_seconds, settings_.credentials_path, settings_.live);
        UsageSnapshot cached;
        if (LoadCache(cached)) {
            int spacing = cached.spacing > 0 ? cached.spacing : UsagePoller::kStartSpacing;
            long long not_before = std::max(cached.next_attempt, cached.fetched_at + spacing);
            poller_.Seed(cached, not_before, cached.spacing);
        }
        poller_.Start(hwnd_, kMsgUsage);
        push_.Init();
        push_.StartPhoneCommands(hwnd_, kMsgPhoneSwitch);
        {
            std::wstring creds = settings_.credentials_path.empty() ? DefaultCredentialsPath() : settings_.credentials_path;
            prompts_.Start(creds.substr(0, creds.find_last_of(L"\\/") + 1) + L"projects");
        }
        if (settings_.accounts_enabled) {
            accounts_.Start(hwnd_, kMsgAccounts, settings_.cswap_path);
            reset_grants_.Start();
        }
        push_.SetEnabled(settings_.phone_notifications);
        SyncWidgets();
        main_window_.Init(instance_, hwnd_);
        SyncWindows();

        // Keep the autostart entry pointing at this exe if it was moved (the old one is gone).
        // Another copy that still exists (a test build, a second install) keeps it.
        std::wstring entry = ReadStartupEntry();
        if (!entry.empty() && entry != StartupCommand()) {
            std::wstring old = entry;
            old.erase(std::remove(old.begin(), old.end(), L'"'), old.end());
            if (GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES) SetStartup(true);
        }

        SetTimer(hwnd_, kTickTimer, 1000, nullptr);
        Tick();

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        RemoveTray();
        for (auto& w : widgets_) w->Destroy();
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
            push_.OnUsage(poller_.Snapshot());
            {
                UsageSnapshot s = poller_.Snapshot();
                if (s.has_data && s.last_status == FetchStatus::Ok && s.fetched_at > last_reading_at_) {
                    last_reading_at_ = s.fetched_at;
                    push_.UploadSoon();  // phones get every fresh reading straight away
                    ShareWithClaudeSwap(s);
                    const UsageWindow& f = s.data.five_hour;
                    const UsageWindow& w = s.data.seven_day;
                    prompts_.OnReading(s.data.account_email, s.fetched_at, f.present ? f.utilization : 0,
                                       f.present ? f.resets_at : 0, w.present ? w.utilization : 0,
                                       w.present ? w.resets_at : 0);
                }
            }
            Tick();
            return 0;
        case WM_TIMER:
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            Tick();
            return 0;
        case kMsgAccounts: {
            // An account switch made anywhere (this menu, cswap in a terminal, cswap auto):
            // the live reading now belongs to the old account until the next check.
            AccountsSnapshot a = accounts_.Snapshot();
            int active = -1;
            for (const AccountInfo& acc : a.accounts)
                if (acc.active) active = acc.number;
            if (active != last_active_number_) {
                if (last_active_number_ != -1) {
                    active_changed_at_ = NowUnix();
                    poller_.RefreshNow();
                }
                last_active_number_ = active;
            }
            Tick();
            return 0;
        }
        case AccountsSource::kMsgCommandDone: {
            const auto kind = static_cast<AccountsSource::Command>(lp);
            if (kind == AccountsSource::kSwitchBest || (kind == AccountsSource::kSwitch && auto_switching_)) {
                OnAutoSwitchDone(wp != 0);
                return 0;
            }
            if (wp && kind == AccountsSource::kRemove) {
                NotifyEverywhere("", L"Account removed", removing_name_ + L" is no longer on this PC.", false);
                accounts_.RefreshNow();
                return 0;
            }
            if (wp && kind == AccountsSource::kAddCurrent) {
                NotifyEverywhere("", L"Claude account added",
                                 L"Your new login now shows on the taskbar. Right-click > Rename to name it.", false);
                return 0;
            }
            if (wp && kind == AccountsSource::kSwitch) {
                active_changed_at_ = NowUnix();
                VerifySwitch(manual_to_email_);
                poller_.RefreshNow();
                push_.UploadSoon();  // phones see the switch now, not after the usual spacing
                Tick();
                NotifyEverywhere("switch:" + std::to_string(NowUnix()), L"Switched to " + manual_to_name_,
                                 manual_from_phone_ ? L"From your phone. Claude keeps going." : L"You picked it. Claude keeps going.");
                manual_from_phone_ = false;
            } else if (!wp && kind == AccountsSource::kSwitch) {
                manual_choice_ = 0;
                NotifyEverywhere("switch-failed:" + std::to_string(NowUnix()), L"Couldn't switch to " + manual_to_name_,
                                 L"Claude Code kept its settings file busy. Try again in a moment.");
            } else if (!wp) {
                std::wstring detail = accounts_.LastCommandOutput();
                if (detail.size() > 600) detail = detail.substr(detail.size() - 600);
                const wchar_t* what = kind == AccountsSource::kSwitch   ? L"Couldn't switch accounts."
                                      : kind == AccountsSource::kRename ? L"Couldn't rename the account."
                                      : kind == AccountsSource::kRemove ? L"Couldn't remove the account. Close any Claude Code session running on it and try again."
                                                                        : L"Couldn't add the account.";
                MessageBoxW(nullptr, (std::wstring(what) + (detail.empty() ? L"" : L"\n\n" + detail)).c_str(), kAppName,
                            MB_OK | MB_ICONWARNING);
            }
            return 0;
        }
        case kMsgRemote:
            OnCommand(static_cast<int>(wp));
            return 0;
        case kMsgSwitchConfirmed: {  // Claude Code's login is now this account: show it
            std::unique_ptr<std::wstring> email(reinterpret_cast<std::wstring*>(lp));
            AppendAppLog(L"accounts: switch to " + *email + L" confirmed");
            switched_to_email_ = *email;
            switched_at_ = NowUnix();
            push_.UploadSoon();
            Tick();
            return 0;
        }
        case kMsgPhoneSwitch: {  // "switch to alice" or the refresh button, tapped on a phone
            if (wp == 1) {  // check now (within the rate limit) and send the result straight up
                poller_.RefreshNow();
                accounts_.RefreshNow();
                push_.UploadSoon();
                return 0;
            }
            std::unique_ptr<std::wstring> email(reinterpret_cast<std::wstring*>(lp));
            for (const AccountInfo& a : accounts_.Snapshot().accounts)
                if (SameEmail(a.email, *email)) SwitchByHand(a.number, true);
            return 0;
        }
        case kMsgPhoneLink: {
            std::unique_ptr<std::wstring> result(reinterpret_cast<std::wstring*>(lp));
            if (!result->empty() && (*result)[0] == L'!') {
                MessageBoxW(nullptr, result->c_str() + 1, kAppName, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
                return 0;
            }
            bool copied = !TestApiPort() && CopyToClipboard(hwnd, *result);  // tests leave your clipboard alone
            std::wstring who = link_for_.empty() ? L"your iPhone" : link_for_ + L"'s owner";
            std::wstring text =
                L"Link for " + who + (copied ? L" (copied):\n\n" : L":\n\n") + *result + L"\n\n" +
                (link_for_.empty() ? L"" : L"Send it to them. ") +
                L"On the iPhone: open it in Safari, tap Share > Add to Home Screen, open Claude Usage from the "
                L"Home Screen and tap Enable Notifications.\n\n" +
                (link_for_.empty() ? L"This phone sees every account and controls them all."
                                   : L"Their phone sees only " + link_for_ + L"'s usage, and they can cap it or ask you to stop.") +
                L" The link works once, within 24 hours.";
            MessageBoxW(nullptr, text.c_str(), kAppName, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            return 0;
        }
        case kMsgTray:
            switch (LOWORD(lp)) {
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                ShowMenu();
                break;
            case WM_LBUTTONDBLCLK:
                // Hidden to the tray: double-click brings the readout back.
                OnCommand(settings_.taskbar_visible ? kCmdOptions : kCmdToggleTaskbar);
                break;
            default:
                break;
            }
            return 0;
        case TaskbarWidget::kMsgColumnClick:
            if (wp < taskbar_accounts_.size()) {
                // Auto-switch off for it, or its owner turned off your access: never switched to by
                // a click (easy to hit by accident); its name flashes red instead.
                if (wp < taskbar_blocked_.size() && taskbar_blocked_[wp]) {
                    for (auto& w : widgets_) w->Flash(static_cast<int>(wp), RGB(255, 69, 58));
                } else {
                    SwitchByHand(taskbar_accounts_[wp]);
                }
            }
            return 0;
        case TaskbarWidget::kMsgCheckClick:  // the ring: check usage now (still within Anthropic's limit)
            poller_.RefreshNow();
            return 0;
        case TaskbarWidget::kMsgBlossomClick:  // collapse to the blossom, or bring everything back
            settings_.taskbar_collapsed = !settings_.taskbar_collapsed;
            SaveSettings();
            Tick();
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
                    L"%  7d=" + std::to_wstring(RoundPercent(s.data.seven_day)) + L"%  " +
                    (s.data.account_email.empty() ? std::wstring(L"(account unknown)") : Utf8ToWide(s.data.account_email));
        } else {
            line += L"error " + std::to_wstring(static_cast<int>(s.last_status)) + L"  " + s.last_error;
        }
        line += L"  [gap " + std::to_wstring(s.spacing) + L"s, next in " +
                std::to_wstring(s.next_attempt - s.last_attempt) + L"s, " + (s.live ? L"live]" : L"idle]");
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
        poller_.Configure(settings_.poll_seconds, settings_.credentials_path, settings_.live);
        push_.SetEnabled(settings_.phone_notifications);
        if (settings_.accounts_enabled) accounts_.Start(hwnd_, kMsgAccounts, settings_.cswap_path);
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
        poller_.Configure(settings_.poll_seconds, settings_.credentials_path, settings_.live);
        push_.SetEnabled(settings_.phone_notifications);
        if (settings_.accounts_enabled) accounts_.Start(hwnd_, kMsgAccounts, settings_.cswap_path);
        SyncWindows();
        Tick();
    }

    // A readout on every monitor's taskbar: one more when a monitor (with its taskbar) is added, one
    // fewer when it goes. Each gets the live token speed and the next usage check.
    void SyncWidgets() {
        size_t want = static_cast<size_t>(std::max(1, TaskbarWidget::TaskbarCount()));
        while (widgets_.size() > want) {
            widgets_.back()->Destroy();
            widgets_.pop_back();
        }
        while (widgets_.size() < want) {
            auto w = std::make_unique<TaskbarWidget>();
            w->Init(instance_, hwnd_);
            w->SetTarget(static_cast<int>(widgets_.size()));
            w->SetTokenSource([this] {
                TaskbarWidget::TokenFlow flow;
                if (!settings_.taskbar_tokens) return flow;
                PromptStats::Pile p = prompts_.Live();
                flow.rate = p.rate;
                flow.count = p.count;
                flow.working = p.working;
                flow.pile = p.pile;
                flow.count_up = settings_.taskbar_token_pile;
                flow.left = tokens_left_;
                return flow;
            });
            w->SetCheckSource([this] {
                UsageSnapshot s = poller_.Snapshot();
                return TaskbarWidget::CheckTimes{s.last_attempt, s.next_attempt};
            });
            widgets_.push_back(std::move(w));
        }
    }

    void SyncWindows() {
        if (!settings_.taskbar_visible) for (auto& w : widgets_) w->Destroy();
        if (settings_.main_visible && !main_window_.IsOpen()) main_window_.Open(settings_);
        if (!settings_.main_visible && main_window_.IsOpen()) main_window_.Close();
    }

    // What new turned up in the account in use's usage response (FindUsageExtras), for the phones:
    // {"email":..., "items":[{"key":..., "ends_at":...}]}; an empty list clears it.
    static std::string ExtrasJson(const UsageSnapshot& s) {
        if (!s.has_data || s.data.account_email.empty()) return "";
        std::string out = "{\"email\":\"" + s.data.account_email + "\",\"items\":[";
        for (size_t i = 0; i < s.data.extras.size(); ++i) {
            const std::string& k = s.data.extras[i].key;
            bool plain = !k.empty() && k.size() < 64;
            for (char c : k) plain = plain && (isalnum(static_cast<unsigned char>(c)) || c == '_');
            if (!plain) continue;
            if (out.back() != '[') out += ",";
            out += "{\"key\":\"" + k + "\",\"ends_at\":" + std::to_string(s.data.extras[i].ends_at) + "}";
        }
        return out + "]}";
    }

    // ---- periodic update -----------------------------------------------------

    // Claude Code's own reading after each reply: its status line gets the exact 5-hour and weekly
    // usage from Anthropic's answers, and claude-usage-statusline.exe saves it (live-usage.json).
    // Used for the account in use when it's newer than the last usage check, taken since that
    // account came into use, and the check's login is that account (not mid-switch).
    UsageSnapshot WithLiveUsage(UsageSnapshot s) {
        const long long now = NowUnix();
        std::string active;
        if (settings_.accounts_enabled) {
            AccountsSnapshot acc = accounts_.Snapshot();
            for (const AccountInfo& a : acc.accounts) {
                if (!a.active) continue;
                active = WideToUtf8(a.email);
                for (char& c : active) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            }
        }
        if (active != live_active_) {
            live_active_ = active;
            live_active_since_ = now;
        }
        if (!s.has_data || (!active.empty() && active != s.data.account_email)) return s;
        std::wstring path = AppDataFile(L"live-usage.json");
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return s;
        const unsigned long long stamp = (static_cast<unsigned long long>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                                         info.ftLastWriteTime.dwLowDateTime;
        if (stamp != live_stamp_) {  // read again only when it changed
            live_stamp_ = stamp;
            live_ = LiveUsage();
            HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                char buf[1024];
                DWORD got = 0;
                ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
                CloseHandle(f);
                mini_json::Value root;
                if (mini_json::Parser(std::string(buf, got)).Parse(root)) {
                    const mini_json::Value* at = root.Get("at");
                    live_.at = at && at->IsNumber() ? static_cast<long long>(at->number) : 0;
                    auto window = [&](const char* key, UsageWindow& w) {
                        const mini_json::Value* v = root.Get(key);
                        const mini_json::Value* p = v ? v->Get("percent") : nullptr;
                        const mini_json::Value* r = v ? v->Get("resets_at") : nullptr;
                        if (!p || !p->IsNumber()) return;
                        w.present = true;
                        w.utilization = p->number;
                        w.resets_at = r && r->IsNumber() ? static_cast<long long>(r->number) : 0;
                    };
                    window("five_hour", live_.five_hour);
                    window("seven_day", live_.seven_day);
                }
            }
        }
        if (live_.at <= s.fetched_at || live_.at < live_active_since_ || now - live_.at > 3600 || live_.at > now + 60) return s;
        for (auto [from, to] : {std::pair<const UsageWindow*, UsageWindow*>{&live_.five_hour, &s.data.five_hour},
                                {&live_.seven_day, &s.data.seven_day}}) {
            if (!from->present) continue;
            to->present = true;
            to->utilization = from->utilization;
            if (from->resets_at > 0) to->resets_at = from->resets_at;
        }
        s.fetched_at = live_.at;
        return s;
    }

    void Tick() {
        ReloadSettingsIfChanged();
        UsageSnapshot s = WithLiveUsage(poller_.Snapshot());
        {  // tokens left in the account in use's 5-hour window (after tok/s on the taskbar)
            double left = -1;
            const UsageWindow& f = s.data.five_hour;
            // Whose: the reading's login, else the account claude-swap has in use (a reading from the
            // cache at startup doesn't carry its login).
            const std::string email = !s.data.account_email.empty() ? s.data.account_email : live_active_;
            if (s.has_data && !email.empty() && f.present) {
                const long long t = NowUnix();
                const bool rolled = f.resets_at > 0 && t >= f.resets_at;  // a new window: from 0%
                double out = 0;
                if (prompts_.TokensLeft(email, rolled ? 0 : f.utilization, rolled ? f.resets_at : s.fetched_at, out))
                    left = out;
            }
            tokens_left_ = left;
        }
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
        std::vector<DisplayItem> taskbar_items, main_items;
        AccountsSnapshot acc = settings_.accounts_enabled ? accounts_.Snapshot() : AccountsSnapshot{};
        // Signed in to an account claude-swap doesn't have yet (e.g. after /login): save it.
        if (LiveLoginIsUnknown(acc, s) && now >= auto_add_next_ && !accounts_.Busy()) {
            auto_add_next_ = now + 120;
            AppendAppLog(L"accounts: new login " + Utf8ToWide(s.data.account_email) + L", saving it to claude-swap");
            accounts_.AddCurrent(L"");
        }
        if (acc.available && !acc.accounts.empty()) {
            // claude-swap accounts (see MergeAccounts for how the live reading is placed): the active one uses this app's live reading (once it's
            // from the current account), the others claude-swap's.
            std::vector<AccountInfo> merged = Merged(acc, s, now);
            if (now - prompt_costs_at_ >= 15 || prompt_costs_.size() != merged.size()) {
                prompt_costs_at_ = now;
                prompt_costs_.clear();
                for (const AccountInfo& a : merged) {
                    std::string email = WideToUtf8(a.email);
                    for (char& c : email) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                    prompt_costs_.push_back(prompts_.CostsFor(email));
                }
            }
            push_.OnAccounts(settings_.your_name, merged, prompt_costs_, TokensJson(), s.fetched_at, s.next_attempt, ExtrasJson(s),
                            reset_grants_.Json());
            if (push_.TokensDue(prompts_.TokenRate() >= 0.5)) push_.SendTokens(TokensJson());  // the phone's live graph
            ApplyOwnerPolicies(merged);
            if (!s.data.account_email.empty()) {
                // Between usage checks, add what Claude Code has spent since the last one, so a
                // switch lands before the limit (between two requests) and the work carries on.
                // (Not while a switch is landing: that reading belongs to the previous account.)
                std::vector<AccountInfo> judged = merged;
                double since[2] = {};
                for (AccountInfo& a : judged) {
                    if (!switched_to_email_.empty()) break;
                    if (!a.active || !prompts_.PercentSince(s.data.account_email, s.fetched_at, since)) continue;
                    if (a.five_hour.present) a.five_hour.percent += since[0];
                    if (a.seven_day.present) a.seven_day.percent += since[1];
                }
                MaybeAutoSwitch(judged, now);
            }
            // The account in use comes first, so the accounts swap places when you switch (once the
            // switch is confirmed: that is when "active" moves). The rest keep claude-swap's order.
            if (settings_.taskbar_active_first)
                std::stable_partition(merged.begin(), merged.end(), [](const AccountInfo& a) { return a.active; });
            taskbar_items = BuildAccountTaskbarItems(merged, settings_.taskbar, settings_.warn_percent,
                                                     settings_.critical_percent, now);
            if (!settings_.taskbar_dim_standby)
                for (DisplayItem& it : taskbar_items) it.dim = false;
            // A friend who freed their reserve: their 5-hour bar is green from the cap to the end.
            size_t row = 0;  // each account is two rows, 5-hour on top (as BuildAccountTaskbarItems lays them)
            for (const AccountInfo& a : merged) {
                if (a.disabled) continue;
                const OwnerPolicy* p = PolicyFor(a);
                if (p && p->reserve_from > 0 && row < taskbar_items.size()) taskbar_items[row].reserve_from = p->reserve_from / 100.0;
                // The caps in force (the lower of the owner's and yours): a tick on each bar. A freed
                // reserve keeps its tick where the cap was.
                const int five = p ? (p->reserve_from ? p->reserve_from : p->cap_five_hour) : 0;
                if (five > 0 && row < taskbar_items.size()) taskbar_items[row].cap_at = five / 100.0;
                if (p && p->cap_weekly > 0 && row + 1 < taskbar_items.size()) taskbar_items[row + 1].cap_at = p->cap_weekly / 100.0;
                row += 2;
            }
            AddTokenColumn(taskbar_items);
            main_items = BuildAccountMainItems(merged, settings_.main, settings_.warn_percent,
                                               settings_.critical_percent, now);
            // Minimal hover: each column says whose it is and what a click does.
            tooltip = L"Claude usage";
            std::vector<std::wstring> tips;
            std::vector<bool> clickable;
            taskbar_accounts_.clear();
            taskbar_blocked_.clear();
            if (settings_.taskbar_tokens) {  // the token-speed column comes first
                taskbar_accounts_.push_back(0);
                taskbar_blocked_.push_back(false);
                tips.push_back(L"");  // no hover text: it covered the readout
                clickable.push_back(false);
            }
            for (const AccountInfo& a : merged) {
                if (a.disabled) continue;  // same columns as BuildAccountTaskbarItems
                taskbar_accounts_.push_back(a.number);
                std::wstring block = OwnerBlock(a, true);
                const OwnerPolicy* pol = PolicyFor(a);
                const bool no_auto = pol && pol->no_auto;  // "No auto" on the phone
                tips.push_back(a.ShortName() + (a.active ? L"  \u00b7  in use"
                                                : no_auto ? L"  \u00b7  No auto: switch from the menu"
                                                : block.empty() ? L"  \u00b7  click to switch" : L"")
                               + (block.empty() ? L"" : L"  \u00b7  " + block));
                clickable.push_back(!a.active);
                taskbar_blocked_.push_back(OwnerStopped(a) || no_auto);
            }
            for (auto& w : widgets_) w->SetColumns(tips, clickable);
            // The account shown in use changed (only ever on proof: the identity check after a
            // switch, a usage reading, or claude-swap noticing a switch made elsewhere): flash it.
            int active_now = -1;
            for (const AccountInfo& a : merged)
                if (a.active) active_now = a.number;
            if (active_now != -1) {
                if (shown_active_ != -1 && active_now != shown_active_) {
                    for (size_t i = 0; i < taskbar_accounts_.size(); ++i)
                        if (taskbar_accounts_[i] == active_now && settings_.taskbar_switch_glow)
                            for (auto& w : widgets_) w->Flash(static_cast<int>(i));
                }
                shown_active_ = active_now;
            }
        } else {
            for (auto& w : widgets_) w->SetColumns({}, {});
            taskbar_accounts_.clear();
            taskbar_blocked_.clear();
            taskbar_items = BuildItems(s, settings_.taskbar, settings_.warn_percent, settings_.critical_percent, now);
            AddTokenColumn(taskbar_items);
            main_items = BuildItems(s, settings_.main, settings_.warn_percent, settings_.critical_percent, now);
        }
        if (settings_.taskbar_visible) {
            SyncWidgets();
            for (auto& w : widgets_) w->Update(taskbar_items, settings_);
            for (auto& w : widgets_) w->SetTooltip(tooltip, settings_.taskbar.tooltip);
        }
        if (main_window_.IsOpen()) {
            main_window_.Update(main_items, settings_);
            main_window_.SetTooltip(tooltip, settings_.main.tooltip);
        }
        UpdateTray(s);
        CheckAlerts(s, now);
    }

    // ---- notification area ---------------------------------------------------

    void UpdateTray(const UsageSnapshot& s) {
        // With the readout hidden, the tray icon is the way back, so it always shows then.
        if (!settings_.show_tray_icon && settings_.taskbar_visible) {
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

    // Give claude-swap this reading of the signed-in account, and have it hold its own checks of
    // that account for 5 minutes (renewed with every reading), so the two apps don't both spend
    // the account's usage-endpoint budget and get blocked.
    void ShareWithClaudeSwap(const UsageSnapshot& s) {
        if (!settings_.accounts_enabled || s.data.account_email.empty()) return;
        AccountsSnapshot acc = accounts_.Snapshot();
        if (acc.schema_version <= 0) return;
        for (const AccountInfo& a : acc.accounts) {
            if (!SameEmail(a.email, Utf8ToWide(s.data.account_email))) continue;
            AccountInfo reading = a;
            reading.five_hour = AccountWindow{s.data.five_hour.present, s.data.five_hour.utilization, s.data.five_hour.resets_at};
            reading.seven_day = AccountWindow{s.data.seven_day.present, s.data.seven_day.utilization, s.data.seven_day.resets_at};
            accounts_.ShareReading(BuildUsageShareJson(acc.schema_version, reading, NowUnix() - s.fetched_at), 300);
        }
    }

    // The token speed, like TrafficMonitor's network speed: one number, tokens per second across
    // every Claude Code session, from their transcripts on this PC (not Anthropic's usage checks),
    // averaged over the last 10 s and redrawn every second.
    void AddTokenColumn(std::vector<DisplayItem>& items) {
        if (!settings_.taskbar_tokens) return;
        DisplayItem speed, empty;  // the widget draws the number itself, live (SetTokenSource)
        speed.value = L"tok/s";
        speed.plain = speed.span = true;
        empty.plain = true;
        items.insert(items.begin(), {speed, empty});
    }

    // The live tokens for the phone: the pile (as on the taskbar), the speed now, each second of the
    // last 5 minutes (as it stands now: a reply still being written is corrected once it lands),
    // and each minute of the last 15.
    std::string TokensJson() {
        auto list = [](const std::vector<double>& values) {
            std::string out = "[";
            for (size_t i = 0; i < values.size(); ++i) out += (i ? "," : "") + std::to_string(std::llround(values[i]));
            return out + "]";
        };
        PromptStats::Pile pile = prompts_.Live();
        return "{\"rate\":" + std::to_string(std::llround(pile.rate)) + ",\"count\":" +
               std::to_string(std::llround(pile.count)) + ",\"working\":" + (pile.working ? "true" : "false") +
               ",\"pile\":" + std::to_string(pile.pile) +
               ",\"mode\":" + (settings_.taskbar_token_pile ? "\"pile\"" : "\"speed\"") +
               (tokens_left_ >= 0 ? ",\"left\":" + std::to_string(std::llround(tokens_left_)) : std::string()) +
               ",\"series\":" + list(prompts_.TokenSeries(300)) + ",\"history\":" + list(prompts_.TokenHistory(15)) + "}";
    }

    // ---- switching ---------------------------------------------------------------

    void VerifySwitch(const std::wstring& email) {
        std::wstring creds = settings_.credentials_path.empty() ? DefaultCredentialsPath() : settings_.credentials_path;
        HANDLE t = CreateThread(nullptr, 0, &VerifySwitchThread, new VerifyJob{hwnd_, creds, email}, 0, nullptr);
        if (t) CloseHandle(t);
    }

    // Every account with the usage numbers the taskbar shows. Right after a switch the app made,
    // the new account is shown in use at once; the next usage check (rate-limited to every
    // minute or two) then confirms it. The taskbar, the menu and clicks all use this one view.
    std::vector<AccountInfo> Merged(const AccountsSnapshot& acc, const UsageSnapshot& s, long long now) {
        std::vector<AccountInfo> merged = MergeAccounts(acc, s);
        // A window whose reset time has passed starts again empty: show 0% right away instead of
        // the old number until claude-swap's next check (the taskbar, the phone and the alerts
        // all read from here).
        for (AccountInfo& a : merged) {
            for (AccountWindow* w : {&a.five_hour, &a.seven_day}) {
                if (w->present && w->resets_at > 0 && w->resets_at <= now) {
                    w->percent = 0;
                    w->resets_at = 0;
                }
            }
        }
        if (switched_to_email_.empty()) return merged;
        if (SameEmail(Utf8ToWide(s.data.account_email), switched_to_email_) || now - switched_at_ > 300) {
            switched_to_email_.clear();
            return merged;
        }
        for (AccountInfo& a : merged) a.active = SameEmail(a.email, switched_to_email_);
        return merged;
    }

    // A switch the user asked for (taskbar click or menu). It always wins: auto-switch
    // won't move away from this account until its usage drops below the threshold again.
    void SwitchByHand(int number, bool from_phone = false) {
        for (const AccountInfo& a : Merged(accounts_.Snapshot(), poller_.Snapshot(), NowUnix())) {
            if (a.number != number || a.active) continue;
            // Your pick wins over caps (they steer the automatic switching); only an owner's
            // "stop using my account" refuses it.
            if (OwnerStopped(a)) {
                NotifyEverywhere(from_phone ? "blocked:" + std::to_string(NowUnix()) : "", L"Can't switch to " + a.ShortName(),
                                 OwnerBlock(a, false) + L".", from_phone);
                return;
            }
            manual_from_phone_ = from_phone;
            manual_choice_ = number;
            manual_choice_at_ = NowUnix();
            manual_to_name_ = a.ShortName();
            manual_to_email_ = a.email;
            AppendAppLog(L"accounts: switching to " + a.ShortName() + (from_phone ? L" (from the phone)" : L" (by hand)"));
            accounts_.Switch(number);
        }
    }

    // ---- automatic account switching ----------------------------------------

    // The active account just reached the threshold: ask claude-swap to move to the
    // account with the most quota left. Claude Code picks the new login up on its own,
    // so running terminals carry on.
    //
    // An account's owner (cloud/, from their phone) can cap it or ask you to stop: that moves
    // you off it whatever the auto-switch setting or a by-hand choice says.
    void MaybeAutoSwitch(const std::vector<AccountInfo>& accounts, long long now) {
        if (accounts.size() < 2 || now < auto_next_check_ || accounts_.Busy()) return;
        const AccountInfo* active = nullptr;
        for (const AccountInfo& a : accounts)
            if (a.active) active = &a;
        if (!active) return;
        const std::wstring owner_block = OwnerBlock(*active, false);
        if (OwnerStopped(*active)) {
            manual_choice_ = 0;  // the owner asked you to stop: that beats your pick
        } else if (owner_block.empty() && !settings_.auto_switch) {
            limit_hit_ = false;
            return;
        }
        const double th = settings_.auto_switch_percent;
        // A window whose reset time has passed is empty again, even before the numbers catch up
        // (for every account alike, or the app could bounce between two of them).
        auto pct_of = [now](const AccountWindow& w) {
            return !w.present || (w.resets_at > 0 && w.resets_at <= now) ? 0.0 : w.percent;
        };
        bool five = pct_of(active->five_hour) >= th;
        bool week = pct_of(active->seven_day) >= th;
        const bool limit_hit = limit_hit_;  // Claude Code said so: no estimate needed
        limit_hit_ = false;
        if (limit_hit) {
            manual_choice_ = 0;
            if (!week) five = true;
        }
        if (manual_choice_ != 0) {
            if (active->number == manual_choice_) {
                // Room again and under every cap: back to normal. Otherwise stay: you chose it.
                if (!five && !week && owner_block.empty()) manual_choice_ = 0;
                return;
            }
            if (now - manual_choice_at_ < 300) return;   // switch still landing
            manual_choice_ = 0;
        }
        if (!five && !week && owner_block.empty()) return;

        // The account with the most room left that its owner is happy for you to use.
        const AccountInfo* soonest = nullptr;
        const AccountInfo* best = nullptr;
        auto used = [&](const AccountInfo& a) { return std::max(pct_of(a.five_hour), pct_of(a.seven_day)); };
        for (const AccountInfo& a : accounts) {
            if (a.active || a.disabled || a.status == L"relogin_required" || a.status == L"api_key") continue;
            if (!OwnerBlock(a, false).empty()) continue;
            if (const OwnerPolicy* p = PolicyFor(a); p && p->no_auto) continue;  // you switch to it by hand only
            bool room = pct_of(a.five_hour) < th && pct_of(a.seven_day) < th;
            if (room && (!best || used(a) < used(*best))) best = &a;
            if (a.five_hour.resets_at > now && (!soonest || a.five_hour.resets_at < soonest->five_hour.resets_at)) soonest = &a;
        }
        long long pct = std::llround(five ? pct_of(active->five_hour) : pct_of(active->seven_day));
        auto_from_name_ = active->ShortName();
        auto_reason_ = !owner_block.empty() ? owner_block
                       : limit_hit         ? auto_from_name_ + L" hit its " + (five ? L"5h" : L"weekly") + L" limit"
                                           : auto_from_name_ + L" hit " + std::to_wstring(std::min(100LL, pct)) + L"% (" +
                                                 (five ? L"5h" : L"weekly") + L")";
        auto_next_check_ = now + 300;  // never flip-flop: at most one attempt every 5 minutes

        if (!best) {
            // Look again the moment the next account resets, not five minutes later.
            if (soonest) auto_next_check_ = std::min(auto_next_check_, soonest->five_hour.resets_at + 15);
            // Once per account and window (95% then 96% is the same news).
            long long when = five ? active->five_hour.resets_at : active->seven_day.resets_at;
            std::wstring key = auto_from_name_ + (owner_block.empty() ? (five ? L"|5h|" : L"|wk|") : L"|owner|") +
                               std::to_wstring(owner_block.empty() ? when : 0);
            if (key != exhausted_notified_for_) {
                exhausted_notified_for_ = key;
                std::wstring body = auto_reason_ + L". No other account has room.";
                if (soonest)
                    body += L" " + soonest->ShortName() + L" resets in " + FormatDuration(soonest->five_hour.resets_at - now) + L".";
                unsigned long long fp = 1469598103934665603ULL;  // the cloud takes ids of [\w:.-] only
                for (wchar_t c : key) fp = (fp ^ static_cast<unsigned long long>(c)) * 1099511628211ULL;
                NotifyEverywhere("exhausted:" + std::to_string(fp), L"No Claude account has room", body);
            }
            return;
        }
        auto_to_name_ = best->ShortName();
        auto_to_email_ = best->email;
        auto_switching_ = true;
        AppendAppLog(L"auto-switch: " + auto_reason_ + L", switching to " + auto_to_name_);
        accounts_.Switch(best->number);
    }

    void OnAutoSwitchDone(bool ok) {
        auto_switching_ = false;
        if (!ok) {
            AppendAppLog(L"auto-switch: cswap failed: " + accounts_.LastCommandOutput());
            return;
        }
        active_changed_at_ = NowUnix();
        VerifySwitch(auto_to_email_);
        poller_.RefreshNow();
        accounts_.RefreshNow();
        push_.UploadSoon();
        Tick();
        std::wstring body = auto_reason_ + L". Claude keeps going.";
        AppendAppLog(L"auto-switch: switched to " + auto_to_name_ + L": " + body);
        NotifyEverywhere("switch:" + std::to_string(NowUnix()), L"Switched to " + auto_to_name_, body);
    }

    // ---- account owners --------------------------------------------------------

    const OwnerPolicy* PolicyFor(const AccountInfo& a) const {
        std::string email = WideToUtf8(a.email);
        for (char& c : email) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        for (const OwnerPolicy& p : policies_)
            if (p.email == email) return &p;
        return nullptr;
    }

    bool OwnerStopped(const AccountInfo& a) const {
        const OwnerPolicy* p = PolicyFor(a);
        return p && !p->allow;
    }

    // Why the account's owner doesn't want it used right now; empty when it's fine.
    std::wstring OwnerBlock(const AccountInfo& a, bool brief) const {
        const OwnerPolicy* p = PolicyFor(a);
        if (!p) return L"";
        if (!p->allow) return brief ? L"owner asked you to stop" : a.ShortName() + L" asked you to stop";
        const int caps[2] = {p->cap_five_hour, p->cap_weekly};
        const AccountWindow* windows[2] = {&a.five_hour, &a.seven_day};
        for (int k = 0; k < 2; ++k) {
            if (caps[k] <= 0 || !windows[k]->present || windows[k]->percent < caps[k]) continue;
            std::wstring cap = std::to_wstring(caps[k]) + L"%";
            return brief ? cap + L" cap reached"
                         : a.ShortName() + L" hit the " + cap + (k == 0 ? L" 5h" : L" weekly") + L" cap";
        }
        return L"";
    }

    // New rules from an owner's phone: say so on the PC (their phone already told yours)
    // and act on them now rather than after the auto-switch cooldown.
    void ApplyOwnerPolicies(const std::vector<AccountInfo>& accounts) {
        std::vector<OwnerPolicy> next;
        if (!push_.TakePolicies(next)) return;
        std::vector<OwnerPolicy> before = policies_;
        policies_ = next;
        auto_next_check_ = 0;
        if (!policies_known_) {  // the first answer after start-up is not news
            policies_known_ = true;
            return;
        }
        for (const OwnerPolicy& p : next) {
            OwnerPolicy old;
            for (const OwnerPolicy& b : before)
                if (b.email == p.email) old = b;
            OwnerPolicy same = p;
            same.no_auto = old.no_auto;  // your own auto-switch choice: never news
            if (same == old) continue;
            // Only the automatic 5-hour cap moved (or was switched on or off): not news.
            OwnerPolicy quiet = p;
            quiet.cap_five_hour = old.cap_five_hour;
            quiet.auto_five_hour = old.auto_five_hour;
            quiet.owner_auto_five_hour = old.owner_auto_five_hour;
            quiet.no_auto = old.no_auto;
            if (p.owner_auto_five_hour || old.owner_auto_five_hour) quiet.owner_cap_five_hour = old.owner_cap_five_hour;
            bool automatic = p.auto_five_hour || old.auto_five_hour || p.owner_auto_five_hour || old.owner_auto_five_hour;
            if (automatic && quiet == old) continue;
            std::wstring name = Utf8ToWide(p.email.substr(0, p.email.find('@')));
            for (const AccountInfo& a : accounts)
                if (PolicyFor(a) && PolicyFor(a)->email == p.email) name = a.ShortName();
            auto caps = [](int five, int week) {
                return (five ? L" 5h " + std::to_wstring(five) + L"%" : std::wstring()) + (five && week ? L"," : L"") +
                       (week ? L" weekly " + std::to_wstring(week) + L"%" : std::wstring());
            };
            std::wstring title = name + L" updated access", text;
            if (old.reserve_from != p.reserve_from) {  // their phone has told yours already
                title = p.reserve_from ? name + L" freed their reserve" : name + L" took their reserve back";
                text = p.reserve_from ? L"Use the rest of the 5 hours until the reset (the green on the bar)."
                                      : L"Back to the " + std::to_wstring(p.reserve_from ? p.reserve_from : old.reserve_from) + L"% cap.";
            } else if (old.allow && !p.allow)
                text = L"Asked you to stop using it.";
            else if (!old.allow && p.allow)
                text = L"You can use it again.";
            else if (old.owner_cap_five_hour != p.owner_cap_five_hour || old.owner_cap_weekly != p.owner_cap_weekly)
                text = !p.owner_cap_five_hour && !p.owner_cap_weekly
                           ? std::wstring(L"Caps removed.")
                           : L"Capped at" + caps(p.owner_cap_five_hour, p.owner_cap_weekly) + L".";
            else {  // your own cap on the account changed
                title = L"Your cap on " + name;
                text = !p.cap_five_hour && !p.cap_weekly ? std::wstring(L"Removed.")
                                                         : L"Set at" + caps(p.cap_five_hour, p.cap_weekly) + L".";
            }
            AppendAppLog(L"owners: " + title + L": " + text);
            NotifyEverywhere("", title, text, false);
        }
    }

    // PC (notification-area balloon / Windows notification) + iPhone / Apple Watch.
    void NotifyEverywhere(const std::string& id, const std::wstring& title, const std::wstring& body,
                          bool phone = true) {
        AppendAppLog(L"notify: " + title + L" | " + body + (phone ? L" | + phone" : L""));
        if (tray_added_ && settings_.notify) {  // Options > Notifications: Windows pop-ups
            NOTIFYICONDATAW nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd_;
            nid.uID = kTrayId;
            nid.uFlags = NIF_INFO;
            nid.dwInfoFlags = NIIF_INFO;
            lstrcpynW(nid.szInfoTitle, title.c_str(), ARRAYSIZE(nid.szInfoTitle));
            lstrcpynW(nid.szInfo, body.c_str(), ARRAYSIZE(nid.szInfo));
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
        if (phone) push_.Notify(id, title, body);
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

        // Accounts right at the top: click one to switch (the ✓ is the login Claude Code
        // really uses), Rename underneath. Uses the same merged numbers as the taskbar.
        AccountsSnapshot acc = settings_.accounts_enabled ? accounts_.Snapshot() : AccountsSnapshot{};
        if (acc.available && !acc.accounts.empty()) {
            std::vector<AccountInfo> accounts = Merged(acc, poller_.Snapshot(), NowUnix());
            HMENU rename = CreatePopupMenu();
            auto pct = [](const AccountWindow& w) {
                return w.present ? std::to_wstring(std::llround(w.percent)) + L"%" : std::wstring(L"--");
            };
            for (const AccountInfo& a : accounts) {
                std::wstring text = a.ShortName() + L"\t5h " + pct(a.five_hour) + L"   7d " + pct(a.seven_day);
                AppendMenuW(menu, MF_STRING | (a.active ? MF_CHECKED : 0) | (a.disabled ? MF_GRAYED : 0),
                            kCmdAccountBase + a.number, text.c_str());
                AppendMenuW(rename, MF_STRING, kCmdRenameBase + a.number,
                            (a.ShortName() + L"\t" + a.email).c_str());
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(rename), L"Rename");

            // A one-time link for a phone: yours sees every account, an owner's only theirs.
            std::wstring cloud_url;
            std::string cloud_token;
            if (ReadCloudConfig(cloud_url, cloud_token)) {
                HMENU links = CreatePopupMenu();
                AppendMenuW(links, MF_STRING, kCmdLinkBase, L"For my iPhone (every account)");
                AppendMenuW(links, MF_SEPARATOR, 0, nullptr);
                for (const AccountInfo& a : accounts)
                    AppendMenuW(links, MF_STRING, kCmdLinkBase + a.number, (L"For " + a.ShortName() + L"'s owner").c_str());
                AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(links), L"Phone link");
            }
            SecureZeroMemory(&cloud_token[0], cloud_token.size());

            HMENU remove = CreatePopupMenu();
            for (const AccountInfo& a : accounts)
                AppendMenuW(remove, MF_STRING | (a.active ? MF_GRAYED : 0), kCmdRemoveBase + a.number,
                            (a.ShortName() + (a.active ? L"\tin use" : L"")).c_str());
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(remove), L"Remove");
            separator();
        }
        add(kCmdRefresh, L"Refresh Now", glyph::kRefresh, false);
        separator();
        add(kCmdDisplay, L"Display Settings...", glyph::kDisplay, false);
        add(kCmdToggleMain, L"Show Main Window", 0, settings_.main_visible);
        add(kCmdToggleTaskbar, settings_.taskbar_visible ? L"Close Taskbar Window" : L"Show Taskbar Window",
            settings_.taskbar_visible ? glyph::kClose : glyph::kTaskbar, false);
        separator();
        add(kCmdOptions, L"Options...", glyph::kSettings, false);
        if (settings_.taskbar_visible) add(kCmdExit, L"Exit", glyph::kExit, false);
        add(kCmdQuit, L"Quit Completely", settings_.taskbar_visible ? 0 : glyph::kExit, false);

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
            accounts_.RefreshNow();
            break;
        case kCmdLimitHit:
            AppendAppLog(L"auto-switch: Claude Code stopped on a usage limit");
            limit_hit_ = true;
            auto_next_check_ = 0;
            poller_.RefreshNow();
            Tick();
            break;
        case kCmdAccountsRefresh:
            accounts_.RefreshNow();
            break;
        case kCmdAccountAdd: {
            NameDialogArgs args{L"Add Account",
                                L"Saves the account Claude Code is signed in to now. Name it:", L""};
            if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_RENAME), nullptr, &NameDialogProc,
                                reinterpret_cast<LPARAM>(&args)) == IDOK)
                accounts_.AddCurrent(args.value);
            break;
        }
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
        case kCmdExit: {  // to the tray: everything keeps working, the readout comes back from there
            Settings s = settings_;
            s.taskbar_visible = false;
            s.main_visible = false;
            ApplySettings(s, startup);
            Tick();
            NotifyEverywhere("", L"Still running in the tray",
                             L"Switching and phone alerts carry on. Double-click the tray icon to bring it back.", false);
            break;
        }
        case kCmdQuit:
            DestroyWindow(hwnd_);
            break;
        default:
            if (cmd >= kCmdLinkBase && cmd < kCmdLinkBase + 100) {
                int number = cmd - kCmdLinkBase;
                link_for_.clear();
                std::string body = "{}";
                for (const AccountInfo& a : accounts_.Snapshot().accounts) {
                    if (a.number != number) continue;
                    link_for_ = a.ShortName();
                    std::string email = WideToUtf8(a.email);
                    for (char& c : email) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                    body = "{\"owner\":\"" + email + "\"}";
                }
                if (number != 0 && link_for_.empty()) return;
                HANDLE t = CreateThread(nullptr, 0, &PhoneLinkThread, new LinkRequest{hwnd_, body}, 0, nullptr);
                if (t) CloseHandle(t);
            } else if (cmd > kCmdRemoveBase && cmd < kCmdRemoveBase + 100) {
                int number = cmd - kCmdRemoveBase;
                for (const AccountInfo& a : accounts_.Snapshot().accounts) {
                    if (a.number != number || a.active) continue;
                    std::wstring text = L"Remove " + a.ShortName() + L" (" + a.email + L") from this PC?\n\n" +
                                        L"Its saved login is deleted, so the app can't switch to it any more, and its "
                                        L"owner's phone stops getting updates. To add it back, sign in to it again in "
                                        L"Claude Code.";
                    if (MessageBoxW(nullptr, text.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND) == IDYES) {
                        removing_name_ = a.ShortName();
                        accounts_.Remove(number);
                    }
                }
            } else if (cmd > kCmdRenameBase && cmd < kCmdRenameBase + 100) {
                int number = cmd - kCmdRenameBase;
                for (const AccountInfo& a : accounts_.Snapshot().accounts) {
                    if (a.number != number) continue;
                    NameDialogArgs args{L"Rename Account",
                                        L"New taskbar name for " + (a.email.empty() ? a.ShortName() : a.email) + L":",
                                        a.alias};
                    if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_RENAME), nullptr, &NameDialogProc,
                                        reinterpret_cast<LPARAM>(&args)) == IDOK)
                        accounts_.Rename(number, args.value);
                }
            } else if (cmd > kCmdAccountBase && cmd < kCmdAccountBase + 100) {
                int number = cmd - kCmdAccountBase;
                SwitchByHand(number);
            }
            break;
        }
    }

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    UINT taskbar_created_msg_ = 0;
    Settings settings_;
    ULONGLONG settings_stamp_ = 0;
    UsagePoller poller_;
    PushBridge push_;
    ResetGrants reset_grants_;
    AccountsSource accounts_;
    int last_active_number_ = -1;
    long long active_changed_at_ = 0;  // live data older than this belongs to the previous account
    long long auto_next_check_ = 0;    // auto-switch cooldown
    long long auto_add_next_ = 0;      // auto-add cooldown
    std::vector<int> taskbar_accounts_;  // account number of each taskbar column
    std::vector<bool> taskbar_blocked_;  // each column: auto-switch off or access off (a click won't switch)
    int manual_choice_ = 0;              // account the user picked by hand: auto-switch leaves it alone
    long long manual_choice_at_ = 0;
    std::wstring auto_from_name_, auto_reason_, auto_to_name_;
    bool auto_switching_ = false;         // the running `cswap switch` was an automatic one
    bool limit_hit_ = false;              // Claude Code reported hitting a usage limit
    std::wstring link_for_;               // whose phone link is being made ("" = your own)
    std::wstring manual_to_name_, manual_to_email_, auto_to_email_;
    std::wstring switched_to_email_;      // a switch the app made, until a usage check confirms it
    bool manual_from_phone_ = false;      // the switch running now was asked from the phone
    long long switched_at_ = 0;
    int shown_active_ = -1;               // the account the taskbar last showed in use
    std::wstring removing_name_;
    std::wstring exhausted_notified_for_;
    std::vector<OwnerPolicy> policies_;  // account owners' rules, from the cloud relay
    PromptStats prompts_;                // prompts left per effort, from Claude Code's transcripts
    std::vector<PromptCosts> prompt_costs_;
    long long prompt_costs_at_ = 0;
    long long last_reading_at_ = 0;
    bool policies_known_ = false;
    double tokens_left_ = -1;  // see Tick; the taskbar shows it after tok/s
    // Claude Code's status line reading (WithLiveUsage).
    struct LiveUsage {
        long long at = 0;
        UsageWindow five_hour, seven_day;
    };
    LiveUsage live_;
    unsigned long long live_stamp_ = 0;
    std::string live_active_;
    long long live_active_since_ = 0;
    // One readout per taskbar: the main monitor's and every other monitor's (SyncWidgets).
    std::vector<std::unique_ptr<TaskbarWidget>> widgets_;
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
    // CUM_INSTANCE lets an end-to-end test run its own copy next to the real one.
    wchar_t instance_name[64] = {};
    GetEnvironmentVariableW(L"CUM_INSTANCE", instance_name, 64);
    g_window_title = std::wstring(kAppName) + instance_name;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, (std::wstring(L"Local\\ClaudeUsageMonitor.SingleInstance") + instance_name).c_str());
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: launching again opens Options; `--limit-hit` (Claude Code's
        // StopFailure hook) asks it to switch accounts now.
        HWND running = FindWindowW(kAppClass, g_window_title.c_str());
        bool limit_hit = std::strstr(GetCommandLineA(), "--limit-hit") != nullptr;
        if (running) {
            DWORD pid = 0;
            GetWindowThreadProcessId(running, &pid);
            if (!limit_hit) AllowSetForegroundWindow(pid);
            PostMessageW(running, kMsgRemote, limit_hit ? kCmdLimitHit : kCmdOptions, 0);
        }
        return 0;
    }

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_WIN95_CLASSES | ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&icc);

    // Never freed: the poller thread may still be running when we exit.
    App* app = new App(instance);
    ExitProcess(static_cast<UINT>(app->Run()));
}
