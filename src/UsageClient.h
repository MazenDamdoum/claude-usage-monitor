// Reads the Claude Code OAuth login from disk and queries the same usage
// endpoint that Claude Code's /usage command uses.
#pragma once
#include <string>

namespace claude_usage {

struct UsageWindow {
    bool present = false;
    double utilization = 0.0;  // percent of the limit used, 0-100
    long long resets_at = 0;   // unix seconds (UTC), 0 if unknown
};

enum class FetchStatus {
    Ok,
    NoCredentials,
    TokenExpired,
    Unauthorized,
    RateLimited,
    NetworkError,
    HttpError,
    ParseError,
};

struct FetchResult {
    FetchStatus status = FetchStatus::NetworkError;
    int http_status = 0;
    int retry_after_seconds = 0;
    std::wstring error;             // human readable, empty on success
    std::string body;               // raw response body (never contains the token)
    std::string subscription_type;  // "pro", "max", ... from the credentials file
    UsageWindow five_hour;
    UsageWindow seven_day;
    UsageWindow seven_day_sonnet;
    UsageWindow seven_day_opus;
};

// %CLAUDE_CONFIG_DIR%\.credentials.json, else %USERPROFILE%\.claude\.credentials.json
std::wstring DefaultCredentialsPath();

// Blocking; call from a worker thread.
FetchResult FetchUsage(const std::wstring& credentials_path);

long long NowUnix();
long long ParseIso8601(const std::string& text);  // returns 0 on failure

// "now", "<1m", "42m", "2h 59m", "1d 4h"
std::wstring FormatDuration(long long seconds);
// "Wed 17:00" in the user's local time zone
std::wstring FormatLocalTime(long long unix_seconds);

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

}  // namespace claude_usage
