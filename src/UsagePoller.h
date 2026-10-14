// Background thread that polls the usage endpoint and publishes snapshots.
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
    unsigned version = 0;
};

class UsagePoller {
public:
    static const int kMinSpacingSeconds = 60;

    UsagePoller();

    // Posts notify_msg to notify_hwnd after every attempt.
    void Start(HWND notify_hwnd, UINT notify_msg);
    void Configure(int poll_seconds, const std::wstring& credentials_path);
    // Restores a snapshot saved by a previous run (call before Start). The first
    // check waits until `not_before` (unix seconds) so restarts don't hammer the
    // endpoint into rate limiting.
    void Seed(const UsageSnapshot& snapshot, long long not_before);
    void RefreshNow();
    UsageSnapshot Snapshot();

private:
    static DWORD WINAPI ThreadMain(LPVOID param);
    void Loop();
    static DWORD NextDelaySeconds(const FetchResult& r, int poll, int& failures);

    SRWLOCK lock_;
    UsageSnapshot shared_;          // guarded by lock_
    int poll_seconds_ = 60;         // guarded by lock_
    std::wstring credentials_path_; // guarded by lock_
    long long not_before_ = 0;
    HANDLE wake_ = nullptr;
    HANDLE thread_ = nullptr;
    HWND notify_hwnd_ = nullptr;
    UINT notify_msg_ = 0;
};

}  // namespace claude_usage
