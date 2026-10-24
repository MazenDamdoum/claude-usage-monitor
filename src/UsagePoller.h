// Background thread that polls the usage endpoint and publishes snapshots.
//
// Pacing: the endpoint rate-limits bursts, so requests are always at least
// `spacing` seconds apart. The spacing is learned: it starts at kStartSpacing,
// creeps down after a run of clean successes, and jumps up (and is remembered
// as too fast for an hour) on HTTP 429. While Claude Code is writing to its
// session files ("live"), checks run at the learned spacing; when idle they
// fall back to the configured interval.
#pragma once
#include <windows.h>

#include <string>

#include "UsageClient.h"

namespace claude_usage {

struct UsageSnapshot {
    bool has_data = false;     // at least one successful fetch
    FetchResult data;          // last successful fetch
    long long fetched_at = 0;  // unix seconds of last success
    FetchStatus last_status = FetchStatus::Ok;
    std::wstring last_error;   // error of the most recent attempt, empty if it succeeded
    long long last_attempt = 0;
    long long next_attempt = 0;  // unix seconds of the next scheduled check
    int spacing = 0;             // current learned minimum gap between checks
    bool live = false;           // Claude Code was active recently
    unsigned version = 0;
};

class UsagePoller {
public:
    // Anthropic's usage endpoint allows about 28-30 requests per account in any rolling hour
    // (claude-swap's measurements); going over blocks the account for up to an hour. One check
    // every 140 s is ~26 an hour: as fresh as the budget allows, with room for wake-ups.
    // claude-swap is handed each reading (and holds its own checks of this account), so this app
    // is the only one spending it.
    static const int kFloorSpacing = 140;   // never ask more often than this
    static const int kStartSpacing = 140;
    static const int kMaxSpacing = 300;

    UsagePoller();

    // Posts notify_msg to notify_hwnd after every attempt.
    void Start(HWND notify_hwnd, UINT notify_msg);
    // idle_seconds: interval while Claude Code is idle. live: speed up while it's active.
    void Configure(int idle_seconds, const std::wstring& credentials_path, bool live);
    // Restores state saved by a previous run (call before Start). The first
    // check waits until `not_before` (unix seconds) so restarts don't hammer the
    // endpoint into rate limiting.
    void Seed(const UsageSnapshot& snapshot, long long not_before, int spacing);
    void RefreshNow();
    UsageSnapshot Snapshot();

private:
    static DWORD WINAPI ThreadMain(LPVOID param);
    void Loop();
    void WatchActivity(const std::wstring& credentials_path);
    void WaitUntil(long long deadline, bool wake_early);
    bool IsLive(long long now) const;
    static DWORD BackoffSeconds(const FetchResult& r, int spacing, int& failures);

    SRWLOCK lock_;
    UsageSnapshot shared_;          // guarded by lock_
    int idle_seconds_ = 60;         // guarded by lock_
    bool live_enabled_ = true;      // guarded by lock_
    std::wstring credentials_path_; // guarded by lock_
    long long not_before_ = 0;

    // Worker thread only.
    int spacing_ = kStartSpacing;
    int clean_successes_ = 0;
    int too_fast_spacing_ = 0;      // a spacing that got HTTP 429 ...
    long long too_fast_until_ = 0;  // ... and when to stop avoiding it
    long long last_activity_ = 0;
    std::wstring watched_dir_;
    HANDLE change_ = INVALID_HANDLE_VALUE;

    HANDLE wake_ = nullptr;
    HANDLE thread_ = nullptr;
    HWND notify_hwnd_ = nullptr;
    UINT notify_msg_ = 0;
};

}  // namespace claude_usage
