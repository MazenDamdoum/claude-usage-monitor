#include "UsagePoller.h"

#include <algorithm>

namespace claude_usage {
namespace {

const int kLiveWindowSeconds = 180;    // "live" if Claude Code wrote within this window
const int kSuccessesBeforeFaster = 10;  // clean checks before trying a shorter gap
const int kSpacingStep = 5;
const int kTooFastPenalty = 15;
const int kTooFastMemorySeconds = 3600;

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

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

void UsagePoller::Configure(int idle_seconds, const std::wstring& credentials_path, bool live) {
    AcquireSRWLockExclusive(&lock_);
    idle_seconds_ = idle_seconds;
    credentials_path_ = credentials_path;
    live_enabled_ = live;
    ReleaseSRWLockExclusive(&lock_);
}

void UsagePoller::Seed(const UsageSnapshot& snapshot, long long not_before, int spacing) {
    AcquireSRWLockExclusive(&lock_);
    shared_ = snapshot;
    shared_.next_attempt = not_before;
    ++shared_.version;
    ReleaseSRWLockExclusive(&lock_);
    not_before_ = not_before;
    spacing_ = spacing > 0 ? Clamp(spacing, kFloorSpacing, kMaxSpacing) : kStartSpacing;
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

// Claude Code appends to its session transcripts under <config dir>\projects
// while it works; a change notification there is a free "someone is busy" signal.
void UsagePoller::WatchActivity(const std::wstring& credentials_path) {
    std::wstring dir = credentials_path.substr(0, credentials_path.find_last_of(L"\\/") + 1) + L"projects";
    if (dir == watched_dir_ && change_ != INVALID_HANDLE_VALUE) return;
    if (change_ != INVALID_HANDLE_VALUE) FindCloseChangeNotification(change_);
    change_ = FindFirstChangeNotificationW(
        dir.c_str(), TRUE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_FILE_NAME);
    watched_dir_ = dir;
}

bool UsagePoller::IsLive(long long now) const {
    return last_activity_ > 0 && now - last_activity_ < kLiveWindowSeconds;
}

// Sleeps until `deadline`. With wake_early, a manual refresh returns at once and
// Claude Code becoming active pulls the deadline in to the earliest allowed check.
void UsagePoller::WaitUntil(long long deadline, bool wake_early) {
    for (;;) {
        long long now = NowUnix();
        if (now >= deadline) return;
        DWORD ms = static_cast<DWORD>(std::min<long long>(deadline - now, 3600)) * 1000;
        HANDLE handles[2] = {wake_, change_};
        DWORD count = change_ != INVALID_HANDLE_VALUE ? 2 : 1;
        DWORD r = WaitForMultipleObjects(count, handles, FALSE, ms);
        if (r == WAIT_OBJECT_0) {
            if (wake_early) return;
        } else if (r == WAIT_OBJECT_0 + 1) {
            bool was_live = IsLive(now);
            last_activity_ = now;
            FindNextChangeNotification(change_);
            AcquireSRWLockExclusive(&lock_);
            bool live_enabled = live_enabled_;
            if (live_enabled && wake_early && !was_live) {
                deadline = std::min(deadline, std::max(now, shared_.last_attempt + spacing_));
                shared_.next_attempt = deadline;
            }
            bool changed = shared_.live != live_enabled;
            shared_.live = live_enabled;
            if (changed) ++shared_.version;
            ReleaseSRWLockExclusive(&lock_);
        } else if (r != WAIT_TIMEOUT) {
            Sleep(1000);  // handle trouble: don't spin
        }
    }
}

void UsagePoller::Loop() {
    WaitUntil(not_before_, false);  // respect the previous run's schedule

    int failures = 0;
    // End-to-end tests (fake API only) don't wait the real minimum between checks.
    const int test_spacing = TestApiPort() ? _wtoi(_wgetenv(L"CUM_TEST_POLL_SECONDS") ? _wgetenv(L"CUM_TEST_POLL_SECONDS") : L"0") : 0;
    for (;;) {
        AcquireSRWLockShared(&lock_);
        int idle = idle_seconds_;
        bool live_enabled = live_enabled_;
        std::wstring path = credentials_path_.empty() ? DefaultCredentialsPath() : credentials_path_;
        long long last_attempt = shared_.last_attempt;
        ReleaseSRWLockShared(&lock_);
        WatchActivity(path);

        // Hard floor between requests, whatever woke us.
        if (test_spacing > 0) spacing_ = test_spacing;
        if (last_attempt > 0) WaitUntil(last_attempt + spacing_, false);

        long long started = NowUnix();
        long long gap = last_attempt > 0 ? started - last_attempt : 0;
        long long interval = spacing_;
        try {
            FetchResult result = FetchUsage(path);
            result.body.clear();
            long long now = NowUnix();
            bool live = live_enabled && IsLive(now);

            if (result.status == FetchStatus::Ok) {
                failures = 0;
                if (live && ++clean_successes_ >= kSuccessesBeforeFaster) {
                    int faster = spacing_ - kSpacingStep;
                    bool avoid = now < too_fast_until_ && faster <= too_fast_spacing_;
                    if (faster >= kFloorSpacing && !avoid) spacing_ = faster;
                    clean_successes_ = 0;
                }
                interval = live || test_spacing > 0 ? spacing_ : std::max(idle, spacing_);
            } else {
                clean_successes_ = 0;
                if (result.status == FetchStatus::RateLimited) {
                    too_fast_spacing_ = static_cast<int>(gap > 0 ? gap : spacing_);
                    too_fast_until_ = now + kTooFastMemorySeconds;
                    spacing_ = std::min(kMaxSpacing, std::max(spacing_, too_fast_spacing_) + kTooFastPenalty);
                }
                interval = BackoffSeconds(result, spacing_, failures);
            }
            if (test_spacing > 0) interval = test_spacing;

            AcquireSRWLockExclusive(&lock_);
            shared_.last_attempt = now;
            shared_.next_attempt = now + interval;
            shared_.spacing = spacing_;
            shared_.live = live;
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
            interval = 60;
        }
        WaitUntil(NowUnix() + interval, true);
    }
}

DWORD UsagePoller::BackoffSeconds(const FetchResult& r, int spacing, int& failures) {
    ++failures;
    int shift = failures > 5 ? 5 : failures - 1;
    long long delay;
    switch (r.status) {
    case FetchStatus::RateLimited:
        // A single 429 just means the gap was a bit short (it has already been widened);
        // only repeated ones back off hard: 2m, 4m, 8m ...
        delay = failures == 1 ? spacing : 120LL << (shift - 1);
        if (r.retry_after_seconds > delay) delay = r.retry_after_seconds;
        break;
    case FetchStatus::NetworkError:
    case FetchStatus::HttpError:
        delay = std::max<long long>(spacing, 15LL << shift);  // capped below
        if (delay > 300) delay = 300;
        break;
    default:  // login missing / expired / rejected: cheap to re-check the file
        delay = 60;
        break;
    }
    return static_cast<DWORD>(delay > 1800 ? 1800 : delay);
}

}  // namespace claude_usage
