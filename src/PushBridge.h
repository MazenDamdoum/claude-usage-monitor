// Optional phone notifications (see push/README.md and cloud/README.md).
//
// Forwards usage updates and confirmed reset events to the local push server
// (push/, localhost-only endpoint) and/or the cloud relay (cloud/, HTTPS),
// whichever is configured (bridge.json / cloud.json next to settings.ini).
// Everything runs on a background thread: the UI never waits, and when the
// server isn't running a send fails fast and is dropped. Reset events are
// retried for a few minutes and carry an id the server de-duplicates on.
#pragma once
#include <windows.h>

#include <deque>
#include <string>
#include <vector>

#include "Accounts.h"
#include "PromptStats.h"
#include "ResetDetector.h"
#include "UsagePoller.h"

// A friend's rules for their account, set from their phone (cloud/ only).
struct OwnerPolicy {
    std::string email;      // lowercase
    int cap_five_hour = 0;  // stop at this percent used; 0 = no cap
    int cap_weekly = 0;
    bool allow = true;      // false = they asked you to stop using it
    // The caps above are the lower of the owner's and your own; these are the owner's alone.
    int owner_cap_five_hour = 0;
    int owner_cap_weekly = 0;
    // The owner freed what the 5-hour cap kept for them, until the reset: the cap is lifted, and
    // the 5-hour bar shows green from this percent to 100%. 0 = not freed.
    int reserve_from = 0;
    // The user's 5-hour cap on it follows the suggested one by itself: its changes aren't news.
    bool auto_five_hour = false;
    bool owner_auto_five_hour = false;  // the owner's 5-hour cap does the same
    bool no_auto = false;               // the user: never switch to it by itself (by hand only)
    bool operator==(const OwnerPolicy& o) const {
        return email == o.email && cap_five_hour == o.cap_five_hour && cap_weekly == o.cap_weekly && allow == o.allow &&
               owner_cap_five_hour == o.owner_cap_five_hour && owner_cap_weekly == o.owner_cap_weekly &&
               reserve_from == o.reserve_from && auto_five_hour == o.auto_five_hour &&
               owner_auto_five_hour == o.owner_auto_five_hour && no_auto == o.no_auto;
    }
};

class PushBridge {
public:
    void Init();  // loads the persisted reset state and starts the sender thread
    void SetEnabled(bool enabled);
    void OnUsage(const claude_usage::UsageSnapshot& s);  // call after every poll
    // A one-off phone notification (e.g. an automatic account switch). id de-duplicates.
    void Notify(const std::string& id, const std::wstring& title, const std::wstring& body);
    // Every account's usage, for the friends who own them (cloud relay only). Call every tick;
    // sends at most every 15 s when something changed, and at least once a minute (that is
    // also how the owners' rules come back).
    // `costs` lines up with `accounts` (may be empty).
    // `tokens_json`: the live token speed for the phone ({"rate":..,"history":[..]}), or "".
    // checked_at / next_check_at: the last and next usage check with Anthropic (the phone's countdown).
    void OnAccounts(const std::wstring& user_name, const std::vector<AccountInfo>& accounts,
                    const std::vector<PromptCosts>& costs, const std::string& tokens_json = std::string(),
                    long long checked_at = 0, long long next_check_at = 0, const std::string& extras_json = std::string(),
                    const std::string& resets_json = std::string());
    // The owners' latest rules. True when they differ from what the last call returned.
    bool TakePolicies(std::vector<OwnerPolicy>& out);
    // Every few seconds, ask the cloud whether a phone wants something: posts `msg` to `hwnd` with
    // lParam = new std::wstring (an account's email) to switch, or wParam = 1 to refresh.
    void StartPhoneCommands(HWND hwnd, UINT msg);
    // Send the accounts on the next tick instead of waiting for the usual spacing (UI thread).
    void UploadSoon();
    // The live token speed for the user's phone goes out every 2 s while tokens flow, and once more
    // when they stop (between times the accounts upload carries it). True when one is due now.
    bool TokensDue(bool flowing);
    void SendTokens(const std::string& tokens_json);  // after TokensDue said so

private:
    static const int kLocal = 0;
    static const int kCloud = 1;

    struct Job {
        int target = kLocal;
        std::string path;
        std::string body;
        bool is_event = false;
        int attempts = 0;
        long long not_before = 0;
    };

    static DWORD WINAPI ThreadMain(LPVOID param);
    static DWORD WINAPI CommandsMain(LPVOID param);
    HWND commands_hwnd_ = nullptr;
    UINT commands_msg_ = 0;
    void Loop();
    void Enqueue(Job job);
    void LoadState();
    void SaveState();

    CRITICAL_SECTION lock_;
    std::deque<Job> queue_;  // guarded by lock_
    std::vector<OwnerPolicy> policies_;  // guarded by lock_
    bool policies_changed_ = false;      // guarded by lock_
    HANDLE wake_ = nullptr;
    HANDLE thread_ = nullptr;
    volatile LONG enabled_ = 0;

    // UI thread only.
    claude_usage::ResetState state_;
    bool primed_ = false;
    long long last_fetched_at_ = 0;
    std::string last_accounts_json_;
    ULONGLONG last_tokens_sent_ = 0;
    bool tokens_flowing_ = false;
    long long last_accounts_sent_ = 0;
};

// Exposed for tests.
std::string BuildUsageJson(const claude_usage::UsageSnapshot& s);
std::string BuildEventJson(const claude_usage::LimitReset& e);
std::string BuildNoticeJson(const std::string& id, const std::wstring& title, const std::wstring& body);
std::string BuildAccountsJson(const std::wstring& user_name, const std::vector<AccountInfo>& accounts,
                              const std::vector<PromptCosts>& costs = {}, const std::string& tokens_json = std::string(),
                              long long checked_at = 0, long long next_check_at = 0,
                              const std::string& extras_json = std::string(),
                              const std::string& resets_json = std::string());
// Reads {"policies":[{email, cap_five_hour, cap_weekly, allow}]} from the cloud's reply.
bool ParsePolicies(const std::string& body, std::vector<OwnerPolicy>& out);
// POSTs JSON to http://127.0.0.1:<port><path>; returns the HTTP status, 0 if unreachable.
int PostToPushServer(int port, const std::string& token, const char* path, const std::string& body);
// Reads {"port":..,"token":".."} written by the push server; false if missing/invalid.
bool ReadPushBridgeConfig(int& port, std::string& token);
// Reads {"url":"https://..","token":".."} written by cloud\\setup.ps1; false if missing/invalid.
bool ReadCloudConfig(std::wstring& url, std::string& token);
// POSTs JSON to an https:// URL (or http://127.0.0.1 for local testing); returns HTTP status or 0.
// The reply (up to 64 KB) goes to *response when given.
int PostJsonToUrl(const std::wstring& url, const std::string& token, const std::string& body,
                  std::string* response = nullptr);
