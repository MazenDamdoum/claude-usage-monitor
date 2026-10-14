#include "UsagePoller.h"

namespace claude_usage {

UsagePoller::UsagePoller() {
    InitializeSRWLock(&lock_);
}

void UsagePoller::Start(HWND notify_hwnd, UINT notify_msg) {
    if (thread_) return;
    notify_hwnd_ = notify_hwnd;
    notify_msg_ = notify_msg;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, &UsagePoller::ThreadMain, this, 0, nullptr);
}

void UsagePoller::Configure(int poll_seconds, const std::wstring& credentials_path) {
    AcquireSRWLockExclusive(&lock_);
    poll_seconds_ = poll_seconds;
    credentials_path_ = credentials_path;
    ReleaseSRWLockExclusive(&lock_);
}

void UsagePoller::Seed(const UsageSnapshot& snapshot, long long not_before) {
    AcquireSRWLockExclusive(&lock_);
    shared_ = snapshot;
    shared_.next_attempt = not_before;
    ++shared_.version;
    not_before_ = not_before;
    ReleaseSRWLockExclusive(&lock_);
}

void UsagePoller::RefreshNow() {
    if (wake_) SetEvent(wake_);
}

UsageSnapshot UsagePoller::Snapshot() {
    AcquireSRWLockShared(&lock_);
    UsageSnapshot copy = shared_;
    ReleaseSRWLockShared(&lock_);
    return copy;
}

DWORD WINAPI UsagePoller::ThreadMain(LPVOID param) {
    static_cast<UsagePoller*>(param)->Loop();
    return 0;
}

void UsagePoller::Loop() {
    long long wait = not_before_ - NowUnix();
    if (wait > 0) WaitForSingleObject(wake_, static_cast<DWORD>(wait > 3600 ? 3600 : wait) * 1000);

    int failures = 0;
    for (;;) {
        // The endpoint rate-limits bursts, so never ask more than once a minute,
        // even for manual refreshes or a limit rolling over.
        AcquireSRWLockShared(&lock_);
        long long gap = shared_.last_attempt + kMinSpacingSeconds - NowUnix();
        ReleaseSRWLockShared(&lock_);
        if (gap > 0) Sleep(static_cast<DWORD>(gap > kMinSpacingSeconds ? kMinSpacingSeconds : gap) * 1000);

        DWORD wait_ms = 60 * 1000;
        try {
            AcquireSRWLockShared(&lock_);
            int poll = poll_seconds_;
            std::wstring path = credentials_path_.empty() ? DefaultCredentialsPath() : credentials_path_;
            ReleaseSRWLockShared(&lock_);

            FetchResult result = FetchUsage(path);
            wait_ms = NextDelaySeconds(result, poll, failures) * 1000;
            result.body.clear();

            long long now = NowUnix();
            AcquireSRWLockExclusive(&lock_);
            shared_.last_attempt = now;
            shared_.next_attempt = now + wait_ms / 1000;
            shared_.last_status = result.status;
            if (result.status == FetchStatus::Ok) {
                shared_.data = std::move(result);
                shared_.has_data = true;
                shared_.fetched_at = now;
                shared_.last_error.clear();
            } else {
                shared_.last_error = result.error;
            }
            ++shared_.version;
            ReleaseSRWLockExclusive(&lock_);

            if (notify_hwnd_) PostMessageW(notify_hwnd_, notify_msg_, 0, 0);
        } catch (...) {
        }
        WaitForSingleObject(wake_, wait_ms);
    }
}

DWORD UsagePoller::NextDelaySeconds(const FetchResult& r, int poll, int& failures) {
    if (r.status == FetchStatus::Ok) {
        failures = 0;
        return static_cast<DWORD>(poll);
    }
    ++failures;
    int shift = failures > 5 ? 5 : failures - 1;
    long long delay;
    switch (r.status) {
    case FetchStatus::RateLimited:
        delay = 120LL << shift;  // 2m, 4m, 8m ...
        if (r.retry_after_seconds > delay) delay = r.retry_after_seconds;
        break;
    case FetchStatus::NetworkError:
    case FetchStatus::HttpError:
        delay = 15LL << shift;  // 15s, 30s, 1m ... capped at poll
        if (delay > poll) delay = poll;
        break;
    default:  // login missing / expired / rejected: cheap to re-check the file
        delay = 60;
        break;
    }
    return static_cast<DWORD>(delay > 1800 ? 1800 : delay);
}

}  // namespace claude_usage
