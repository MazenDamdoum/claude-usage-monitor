// Multiple Claude accounts via claude-swap (cswap).
//
// claude-swap keeps every saved account's login fresh and paces its own usage
// checks, so we read its view with `cswap list --json` (run hidden, never a
// console window) instead of polling stored logins ourselves: that would
// rotate their refresh tokens behind claude-swap's back and double the
// requests against Anthropic's rate limit. The active account is still polled
// live by UsagePoller; App merges the two.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

struct AccountWindow {
    bool present = false;
    double percent = 0.0;
    long long resets_at = 0;  // unix seconds, 0 if unknown
};

struct AccountInfo {
    int number = 0;
    std::wstring email;
    std::wstring alias;
    std::wstring org_uuid;  // with the email, how claude-swap tells accounts apart
    bool active = false;
    bool disabled = false;
    std::wstring status;  // "ok", "token_expired", "relogin_required", ...
    bool stale = false;   // showing last-good numbers
    AccountWindow five_hour;
    AccountWindow seven_day;
    std::vector<std::pair<std::wstring, AccountWindow>> scoped;  // per-model weekly, e.g. Fable

    std::wstring ShortName() const;  // alias, else the part of the email before '@'
};

struct AccountsSnapshot {
    bool available = false;  // cswap found and answered
    std::vector<AccountInfo> accounts;
    long long fetched_at = 0;
    std::wstring error;
    unsigned version = 0;
    int schema_version = 0;  // of `cswap list --json`, which `import-usage` must be given back
};

// Parses `cswap list --json`. Exposed for tests.
bool ParseCswapList(const std::string& json, std::vector<AccountInfo>& out, int* schema_version = nullptr);

// A reading of `account` this app just took, as `cswap import-usage` takes it (the shape of
// `cswap list --json`), so claude-swap doesn't spend the same account's request budget again.
std::string BuildUsageShareJson(int schema_version, const AccountInfo& account, long long age_seconds);

class AccountsSource {
public:
    // Posted to notify_hwnd when a background command finishes: wParam = 1 on success,
    // lParam = the Command it was.
    static const UINT kMsgCommandDone = WM_APP + 40;
    enum Command { kSwitch = 0, kRename = 1, kAddCurrent = 2, kSwitchBest = 3, kRemove = 4 };

    void Start(HWND notify_hwnd, UINT notify_msg, const std::wstring& cswap_override);
    void RefreshNow();
    AccountsSnapshot Snapshot();
    // Background cswap commands; each posts kMsgCommandDone when finished.
    void Switch(int number);
    void Rename(int number, const std::wstring& name);  // name already validated
    void AddCurrent(const std::wstring& alias);          // save the account Claude Code is signed in to
    void Remove(int number);                             // forget a saved account (not the one in use)
    // `cswap switch --strategy best`: to the account with the most quota left (if better).
    bool SwitchBest();
    bool Busy() { return InterlockedCompareExchange(&busy_, 0, 0) != 0; }
    // Hand claude-swap a fresh reading and hold its own checks of that account for `hold_seconds`
    // (`cswap import-usage - --hold N`, in the background).
    void ShareReading(const std::string& document, int hold_seconds);
    std::wstring LastCommandOutput();
    bool Found();

private:
    static DWORD WINAPI ThreadMain(LPVOID param);
    static DWORD WINAPI CommandMain(LPVOID param);
    void RunCommand(Command kind, const std::wstring& args);
    void Loop();

    SRWLOCK lock_ = SRWLOCK_INIT;
    AccountsSnapshot shared_;  // guarded by lock_
    std::wstring exe_;         // guarded by lock_
    HANDLE wake_ = nullptr;
    HANDLE thread_ = nullptr;
    HWND notify_hwnd_ = nullptr;
    UINT notify_msg_ = 0;
    volatile LONG busy_ = 0;
    Command command_ = kSwitch;
    std::wstring command_args_;
    std::wstring command_output_;  // guarded by lock_
};

// Finds cswap.exe: explicit path, PATH, then the usual pip / uv / pipx locations.
std::wstring FindCswap(const std::wstring& override_path);
// Runs a console program with no window; captures stdout (and stderr when asked).
// Returns the exit code, or -1.
// `input` is typed into the program (e.g. "y\n" for a confirmation prompt).
int RunHidden(const std::wstring& exe, const std::wstring& args, std::string& out, DWORD timeout_ms,
              bool include_stderr = false, const std::string& input = std::string());
