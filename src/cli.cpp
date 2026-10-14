// claude-usage.exe: command-line probe that shares the plugin's fetch code.
// Handy for checking your login / the endpoint without TrafficMonitor.
//
//   claude-usage                 print usage once
//   claude-usage --watch [secs]  keep printing (default every 60 s)
//   claude-usage --raw           also print the raw JSON response
//   claude-usage --credentials <path>

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "UsageClient.h"

using namespace claude_usage;

static void PrintWindow(const char* title, const UsageWindow& w, long long now) {
    if (!w.present) return;
    std::printf("  %-14s %3lld%%", title, static_cast<long long>(std::lround(w.utilization)));
    if (w.resets_at > 0)
        std::printf("   resets in %-8s (%s)", WideToUtf8(FormatDuration(w.resets_at - now)).c_str(),
                    WideToUtf8(FormatLocalTime(w.resets_at)).c_str());
    std::printf("\n");
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    bool raw = false, watch = false;
    int interval = 60;
    std::wstring credentials = DefaultCredentialsPath();

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--raw")) {
            raw = true;
        } else if (!std::strcmp(argv[i], "--watch")) {
            watch = true;
            if (i + 1 < argc && std::atoi(argv[i + 1]) > 0) interval = std::atoi(argv[++i]);
            if (interval < 30) interval = 30;
        } else if (!std::strcmp(argv[i], "--credentials") && i + 1 < argc) {
            credentials = Utf8ToWide(argv[++i]);
        } else {
            std::printf("usage: claude-usage [--watch [secs]] [--raw] [--credentials <path>]\n");
            return 2;
        }
    }

    for (;;) {
        FetchResult r = FetchUsage(credentials);
        long long now = NowUnix();
        if (r.status == FetchStatus::Ok) {
            std::printf("Claude usage%s%s%s  (HTTP %d)\n", r.subscription_type.empty() ? "" : " (",
                        r.subscription_type.c_str(), r.subscription_type.empty() ? "" : ")", r.http_status);
            PrintWindow("Session (5h)", r.five_hour, now);
            PrintWindow("Weekly (7d)", r.seven_day, now);
            PrintWindow("Weekly Sonnet", r.seven_day_sonnet, now);
            PrintWindow("Weekly Opus", r.seven_day_opus, now);
        } else {
            std::printf("error: %s\n", WideToUtf8(r.error).c_str());
        }
        if (raw && !r.body.empty()) std::printf("%s\n", r.body.c_str());
        std::fflush(stdout);
        if (!watch) return r.status == FetchStatus::Ok ? 0 : 1;
        Sleep(static_cast<DWORD>(interval) * 1000);
    }
}
