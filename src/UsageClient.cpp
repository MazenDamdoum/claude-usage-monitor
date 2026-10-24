#include "UsageClient.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>

#include "mini_json.h"
#include "version.h"

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace claude_usage {
namespace {

const wchar_t kApiHost[] = L"api.anthropic.com";
const wchar_t kUsagePath[] = L"/api/oauth/usage";
const size_t kMaxBodyBytes = 1 << 20;

template <typename S>
void Wipe(S& s) {
    if (!s.empty()) SecureZeroMemory(&s[0], s.size() * sizeof(s[0]));
    s.clear();
}

void WipeJson(mini_json::Value& v) {
    Wipe(v.string);
    for (auto& k : v.keys) Wipe(k);
    for (auto& item : v.items) WipeJson(item);
}

std::wstring GetEnv(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring value(n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, &value[0], n);
    value.resize(got);
    return value;
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > static_cast<LONGLONG>(kMaxBodyBytes)) {
        CloseHandle(h);
        return false;
    }
    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    BOOL ok = out.empty() || ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    out.resize(read);
    return ok != FALSE;
}

long long DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<long long>(doe) - 719468;
}

void ReadWindow(const mini_json::Value& root, const char* key, UsageWindow& w) {
    const mini_json::Value* obj = root.Get(key);
    if (!obj || !obj->IsObject()) return;
    const mini_json::Value* util = obj->Get("utilization");
    if (!util || !util->IsNumber()) return;
    w.present = true;
    w.utilization = util->number;
    const mini_json::Value* reset = obj->Get("resets_at");
    if (reset && reset->IsString()) w.resets_at = ParseIso8601(reset->string);
}

}  // namespace

// CUM_TEST_API=http://127.0.0.1:<port>: a fake usage API for end-to-end tests. 0 when unset.
int TestApiPort() {
    std::wstring v = GetEnv(L"CUM_TEST_API");
    const std::wstring prefix = L"http://127.0.0.1:";
    if (v.compare(0, prefix.size(), prefix) != 0) return 0;
    int port = _wtoi(v.c_str() + prefix.size());
    return port > 0 && port < 65536 ? port : 0;
}

namespace {

struct HttpResponse {
    int status = 0;
    int retry_after = 0;
    DWORD error = 0;
    std::string body;
};

HttpResponse HttpGet(const wchar_t* host, const wchar_t* path, std::wstring& headers) {
    HttpResponse res;
    const wchar_t* agent = L"claude-usage-monitor/" CUM_VERSION_W;
    HINTERNET session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)  // Windows < 8.1
        session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;

    // End-to-end tests point the app at a fake API on this PC (tests/app-e2e). Loopback only.
    int test_port = TestApiPort();
    if (session) {
        WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);
        connect = test_port ? WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(test_port), 0)
                            : WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
    if (connect) {
        request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, test_port ? 0 : WINHTTP_FLAG_SECURE);
    }
    bool ok = request && WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    Wipe(headers);

    if (ok) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        res.status = static_cast<int>(status);

        DWORD retry = 0;
        size = sizeof(retry);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RETRY_AFTER | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &retry, &size, WINHTTP_NO_HEADER_INDEX))
            res.retry_after = static_cast<int>(retry);

        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            size_t offset = res.body.size();
            if (offset + available > kMaxBodyBytes) break;
            res.body.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, &res.body[offset], available, &read)) {
                res.body.resize(offset);
                break;
            }
            res.body.resize(offset + read);
        }
    } else {
        res.error = GetLastError();
    }

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return res;
}

// Resolves an access token to its account email with GET /api/oauth/profile (the same
// identity check claude-swap uses). Cached by a fingerprint of the token, so it runs
// once per token rather than on every usage check; failures are retried after 10 min.
std::string AccountEmailFor(const std::string& token) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static unsigned long long cached_fp = 0;
    static std::string cached_email;
    static long long retry_at = 0;

    unsigned long long fp = 1469598103934665603ULL;  // FNV-1a
    for (unsigned char c : token) fp = (fp ^ c) * 1099511628211ULL;
    AcquireSRWLockShared(&lock);
    bool hit = fp == cached_fp && (!cached_email.empty() || NowUnix() < retry_at);
    std::string email = cached_email;
    ReleaseSRWLockShared(&lock);
    if (hit) return email;

    std::wstring headers = L"Authorization: Bearer " + Utf8ToWide(token) +
                           L"\r\nanthropic-beta: oauth-2025-04-20\r\nAccept: application/json\r\n";
    HttpResponse http = HttpGet(kApiHost, L"/api/oauth/profile", headers);  // wipes headers
    email.clear();
    mini_json::Value root;
    if (http.status == 200 && mini_json::Parser(http.body).Parse(root)) {
        const mini_json::Value* account = root.Get("account");
        const mini_json::Value* e = account ? account->Get("email") : nullptr;
        if (e && e->IsString()) {
            email = e->string;
            for (char& c : email) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
    }
    AcquireSRWLockExclusive(&lock);
    cached_fp = fp;
    cached_email = email;
    retry_at = NowUnix() + 600;
    ReleaseSRWLockExclusive(&lock);
    return email;
}

}  // namespace

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring DefaultCredentialsPath() {
    std::wstring dir = GetEnv(L"CLAUDE_CONFIG_DIR");
    if (dir.empty()) dir = GetEnv(L"USERPROFILE") + L"\\.claude";
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    return dir + L".credentials.json";
}

long long NowUnix() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return static_cast<long long>(t.QuadPart / 10000000ULL) - 11644473600LL;
}

long long ParseIso8601(const std::string& text) {
    int y, mo, d, h, mi, s;
    int consumed = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &s, &consumed) != 6)
        return 0;
    const char* p = text.c_str() + consumed;
    // Fractions round to the nearest second: Anthropic's resets come as 02:59:59.94..., which is 03:00.
    int round_up = 0;
    if (*p == '.') {
        ++p;
        if (*p >= '5' && *p <= '9') round_up = 1;
        for (; *p >= '0' && *p <= '9'; ++p) {}
    }
    long long offset = 0;
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        if (std::sscanf(p + 1, "%2d:%2d", &oh, &om) < 1) return 0;
        offset = (oh * 3600LL + om * 60LL) * (*p == '-' ? -1 : 1);
    }
    long long days = DaysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return days * 86400LL + h * 3600LL + mi * 60LL + s + round_up - offset;
}

std::wstring FormatDuration(long long seconds) {
    if (seconds <= 0) return L"now";
    if (seconds < 60) return L"<1m";
    long long minutes = seconds / 60;
    if (minutes < 60) return std::to_wstring(minutes) + L"m";
    long long hours = minutes / 60;
    if (hours < 24) {
        long long m = minutes % 60;
        return std::to_wstring(hours) + L"h" + (m ? L" " + std::to_wstring(m) + L"m" : L"");
    }
    long long h = hours % 24;
    return std::to_wstring(hours / 24) + L"d" + (h ? L" " + std::to_wstring(h) + L"h" : L"");
}

std::wstring FormatLocalTime(long long unix_seconds) {
    ULARGE_INTEGER t;
    t.QuadPart = static_cast<ULONGLONG>(unix_seconds + 11644473600LL) * 10000000ULL;
    FILETIME ft{t.LowPart, t.HighPart};
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return {};
    wchar_t day[32] = {}, time[32] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, L"ddd", day, 32, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, time, 32);
    return std::wstring(day) + L" " + time;
}

std::string LoginEmail(const std::wstring& credentials_path) {
    std::string raw;
    if (!ReadFileBytes(credentials_path, raw)) return {};
    mini_json::Value creds;
    bool parsed = mini_json::Parser(raw).Parse(creds);
    Wipe(raw);
    std::string email;
    const mini_json::Value* oauth = parsed ? creds.Get("claudeAiOauth") : nullptr;
    const mini_json::Value* token = oauth ? oauth->Get("accessToken") : nullptr;
    if (token && token->IsString() && !token->string.empty()) email = AccountEmailFor(token->string);
    WipeJson(creds);
    return email;
}

std::vector<UsageExtra> FindUsageExtras(const std::string& body) {
    static const char* const kKnown[] = {
        "five_hour", "seven_day", "seven_day_oauth_apps", "seven_day_opus", "seven_day_sonnet", "seven_day_cowork",
        "seven_day_omelette", "omelette_promotional", "extra_usage", "limits", "spend", "member_dashboard_available",
        "seven_day_breakdown", "weekly_scoped_shares", "iguana_necktie"};  // iguana_necktie: a dollar credit grant
    std::vector<UsageExtra> out;
    mini_json::Value root;
    if (!mini_json::Parser(body).Parse(root) || !root.IsObject()) return out;
    for (size_t i = 0; i < root.keys.size() && out.size() < 5; ++i) {
        const std::string& key = root.keys[i];
        const mini_json::Value& v = root.items[i];
        bool known = false;
        for (const char* k : kKnown) known = known || key == k;
        if (known || v.type == mini_json::Value::Null || v.type == mini_json::Value::Bool) continue;
        if ((v.type == mini_json::Value::Array || v.type == mini_json::Value::Object) && v.items.empty()) continue;
        UsageExtra e;
        e.key = key;
        // An end date, if it has one: on it, or on its first entry.
        const mini_json::Value* holder = v.type == mini_json::Value::Array ? &v.items[0] : &v;
        for (const char* name : {"expires_at", "ends_at", "valid_until", "expiration", "expiry", "resets_at"}) {
            const mini_json::Value* d = holder->Get(name);
            if (d && d->IsString() && (e.ends_at = ParseIso8601(d->string)) > 0) break;
        }
        out.push_back(e);
    }
    return out;
}

FetchResult FetchUsage(const std::wstring& credentials_path) {
    FetchResult r;

    std::string raw;
    if (!ReadFileBytes(credentials_path, raw)) {
        r.status = FetchStatus::NoCredentials;
        r.error = L"Claude Code login not found (" + credentials_path + L"). Run `claude` and /login.";
        return r;
    }
    mini_json::Value creds;
    bool parsed = mini_json::Parser(raw).Parse(creds);
    Wipe(raw);
    if (!parsed) {
        WipeJson(creds);
        r.status = FetchStatus::ParseError;
        r.error = L"Could not read the Claude Code credentials file.";
        return r;
    }

    const mini_json::Value* oauth = creds.Get("claudeAiOauth");
    const mini_json::Value* token = oauth ? oauth->Get("accessToken") : nullptr;
    if (!token || !token->IsString() || token->string.empty()) {
        WipeJson(creds);
        r.status = FetchStatus::NoCredentials;
        r.error = L"No Claude.ai subscription login found. Run `claude` and /login.";
        return r;
    }
    const mini_json::Value* sub = oauth->Get("subscriptionType");
    if (sub && sub->IsString()) r.subscription_type = sub->string;
    const mini_json::Value* expires = oauth->Get("expiresAt");  // epoch milliseconds
    if (expires && expires->IsNumber() && expires->number > 0 &&
        static_cast<long long>(expires->number / 1000.0) <= NowUnix()) {
        WipeJson(creds);
        r.status = FetchStatus::TokenExpired;
        r.error = L"Login token expired. Open Claude Code once and it will refresh.";
        return r;
    }

    // Whose login is this? Claude Code can be signed in to a different account than an
    // account switcher believes, so every reading carries the email it was taken with.
    r.account_email = AccountEmailFor(token->string);

    std::wstring headers = L"Authorization: Bearer " + Utf8ToWide(token->string) +
                           L"\r\nanthropic-beta: oauth-2025-04-20\r\nAccept: application/json\r\n";
    WipeJson(creds);
    HttpResponse http = HttpGet(kApiHost, kUsagePath, headers);  // wipes headers

    r.http_status = http.status;
    r.retry_after_seconds = http.retry_after;
    if (http.status == 0) {
        r.status = FetchStatus::NetworkError;
        r.error = L"Network error (WinHTTP " + std::to_wstring(http.error) + L").";
        return r;
    }
    if (http.status == 401 || http.status == 403) {
        r.status = FetchStatus::Unauthorized;
        r.error = L"Claude rejected the login (HTTP " + std::to_wstring(http.status) +
                  L"). Open Claude Code to refresh it.";
        return r;
    }
    if (http.status == 429) {
        r.status = FetchStatus::RateLimited;
        r.error = L"Rate limited by the usage endpoint; backing off.";
        return r;
    }
    if (http.status != 200) {
        r.status = FetchStatus::HttpError;
        r.error = L"Usage endpoint returned HTTP " + std::to_wstring(http.status) + L".";
        return r;
    }

    r.body = std::move(http.body);
    mini_json::Value root;
    if (!mini_json::Parser(r.body).Parse(root) || !root.IsObject()) {
        r.status = FetchStatus::ParseError;
        r.error = L"Unexpected response from the usage endpoint.";
        return r;
    }
    ReadWindow(root, "five_hour", r.five_hour);
    ReadWindow(root, "seven_day", r.seven_day);
    ReadWindow(root, "seven_day_sonnet", r.seven_day_sonnet);
    ReadWindow(root, "seven_day_opus", r.seven_day_opus);
    r.extras = FindUsageExtras(r.body);
    if (!r.five_hour.present && !r.seven_day.present) {
        r.status = FetchStatus::ParseError;
        r.error = L"Usage response had no five_hour / seven_day data.";
        return r;
    }
    r.status = FetchStatus::Ok;
    return r;
}

}  // namespace claude_usage
