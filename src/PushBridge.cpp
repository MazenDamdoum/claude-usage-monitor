#include "PushBridge.h"

#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>

#include "AppFiles.h"
#include "mini_json.h"

using namespace claude_usage;

namespace {

const int kMaxEventAttempts = 6;  // 15 s, 30 s, 1 m, 2 m, 4 m between tries

std::wstring BridgeFilePath() {
    wchar_t custom[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"CUM_PUSH_BRIDGE_FILE", custom, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return custom;
    return AppDataFile(L"push\\bridge.json");
}

bool IsHex(const std::string& s) {
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

std::string WindowJson(const UsageWindow& w) {
    if (!w.present) return "null";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "{\"utilization_x100\":%lld,\"resets_at\":%lld}",
                  static_cast<long long>(std::llround(w.utilization * 100)), w.resets_at);
    return buf;
}

}  // namespace

std::string BuildUsageJson(const UsageSnapshot& s) {
    std::string plan;
    for (char c : s.data.subscription_type)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            plan += c;
    char head[96];
    std::snprintf(head, sizeof(head), "{\"fetched_at\":%lld,", s.fetched_at);
    return std::string(head) + "\"plan\":\"" + plan + "\"," + "\"five_hour\":" + WindowJson(s.data.five_hour) +
           ",\"seven_day\":" + WindowJson(s.data.seven_day) +
           ",\"seven_day_sonnet\":" + WindowJson(s.data.seven_day_sonnet) +
           ",\"seven_day_opus\":" + WindowJson(s.data.seven_day_opus) + "}";
}

static std::string JsonString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '\"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
        else out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string BuildNoticeJson(const std::string& id, const std::wstring& title, const std::wstring& body) {
    return "{\"id\":" + JsonString(id) + ",\"title\":" + JsonString(WideToUtf8(title)) +
           ",\"body\":" + JsonString(WideToUtf8(body)) + "}";
}

static std::string AccountWindowJson(const AccountWindow& w) {
    if (!w.present) return "null";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "{\"utilization_x100\":%lld,\"resets_at\":%lld}",
                  static_cast<long long>(std::llround(std::min(1000.0, std::max(0.0, w.percent)) * 100)),
                  w.resets_at > 0 ? w.resets_at : 0LL);
    return buf;
}

// {"five_hour":{"low":120,"medium":260,"high":700},"seven_day":null}: percent x100 per prompt.
static std::string PromptCostJson(const PromptCosts& c) {
    static const char* const kWindows[2] = {"five_hour", "seven_day"};
    static const char* const kEfforts[kEffortCount] = {"low", "medium", "high"};
    std::string out = "{";
    for (int w = 0; w < 2; ++w) {
        out += std::string(w ? "," : "") + "\"" + kWindows[w] + "\":";
        if (c.percent[w][kEffortMedium] <= 0) {
            out += "null";
            continue;
        }
        out += "{";
        for (int e = 0; e < kEffortCount; ++e) {
            long long x100 = std::llround(std::min(100.0, c.percent[w][e]) * 100);
            out += std::string(e ? "," : "") + "\"" + kEfforts[e] + "\":" + std::to_string(std::max(1LL, x100));
        }
        out += "}";
    }
    return out + "}";
}

std::string BuildAccountsJson(const std::wstring& user_name, const std::vector<AccountInfo>& accounts,
                              const std::vector<PromptCosts>& costs, const std::string& tokens_json,
                              long long checked_at, long long next_check_at, const std::string& extras_json,
                              const std::string& resets_json) {
    std::string out = "{\"user_name\":" + JsonString(WideToUtf8(user_name)) + ",\"accounts\":[";
    int n = 0;
    for (size_t i = 0; i < accounts.size(); ++i) {
        const AccountInfo& a = accounts[i];
        if (a.email.find(L'@') == std::wstring::npos || n == 10) continue;
        std::wstring email = a.email;
        for (wchar_t& c : email) c = static_cast<wchar_t>(std::towlower(c));
        if (n++) out += ",";
        out += "{\"email\":" + JsonString(WideToUtf8(email)) + ",\"name\":" + JsonString(WideToUtf8(a.ShortName())) +
               ",\"active\":" + (a.active ? "true" : "false") + ",\"five_hour\":" + AccountWindowJson(a.five_hour) +
               ",\"seven_day\":" + AccountWindowJson(a.seven_day) +
               (i < costs.size() ? ",\"prompt_cost\":" + PromptCostJson(costs[i]) : std::string()) + "}";
    }
    out += "]";
    if (!tokens_json.empty()) out += ",\"tokens\":" + tokens_json;
    if (next_check_at > 0) {
        out += ",\"usage_check\":{\"at\":" + std::to_string(checked_at) + ",\"next\":" + std::to_string(next_check_at) + "}";
    }
    if (!extras_json.empty()) out += ",\"extras\":" + extras_json;
    if (!resets_json.empty()) out += ",\"resets\":" + resets_json;  // ResetGrants
    return out + "}";
}

bool ParsePolicies(const std::string& body, std::vector<OwnerPolicy>& out) {
    mini_json::Value root;
    if (!mini_json::Parser(body).Parse(root)) return false;
    const mini_json::Value* list = root.Get("policies");
    if (!list || list->type != mini_json::Value::Array) return false;
    std::vector<OwnerPolicy> result;
    for (const mini_json::Value& p : list->items) {
        const mini_json::Value* email = p.Get("email");
        if (!email || !email->IsString() || email->string.empty()) continue;
        OwnerPolicy policy;
        policy.email = email->string;
        for (char& c : policy.email) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto cap = [&](const char* key) {
            const mini_json::Value* v = p.Get(key);
            return v && v->IsNumber() && v->number >= 1 && v->number <= 100 ? static_cast<int>(v->number) : 0;
        };
        policy.cap_five_hour = cap("cap_five_hour");
        policy.cap_weekly = cap("cap_weekly");
        policy.owner_cap_five_hour = p.Get("owner_cap_five_hour") ? cap("owner_cap_five_hour") : policy.cap_five_hour;
        policy.owner_cap_weekly = p.Get("owner_cap_weekly") ? cap("owner_cap_weekly") : policy.cap_weekly;
        policy.reserve_from = cap("reserve_from");
        const mini_json::Value* autoCap = p.Get("auto_five_hour");
        policy.auto_five_hour = autoCap && autoCap->type == mini_json::Value::Bool && autoCap->boolean;
        const mini_json::Value* ownerAuto = p.Get("owner_auto_five_hour");
        policy.owner_auto_five_hour = ownerAuto && ownerAuto->type == mini_json::Value::Bool && ownerAuto->boolean;
        const mini_json::Value* noAuto = p.Get("no_auto");
        policy.no_auto = noAuto && noAuto->type == mini_json::Value::Bool && noAuto->boolean;
        const mini_json::Value* allow = p.Get("allow");
        policy.allow = !(allow && allow->type == mini_json::Value::Bool && !allow->boolean);
        result.push_back(policy);
    }
    // The cloud lists them in whatever order the accounts came; compare them in a fixed one.
    std::sort(result.begin(), result.end(), [](const OwnerPolicy& a, const OwnerPolicy& b) { return a.email < b.email; });
    out.swap(result);
    return true;
}

std::string BuildEventJson(const LimitReset& e) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "{\"id\":\"%s:%lld\",\"kind\":\"%s\",\"reset_at\":%lld}", ResetKindName(e.kind),
                  e.reset_at, ResetKindName(e.kind), e.reset_at);
    return buf;
}

bool ReadPushBridgeConfig(int& port, std::string& token) {
    HANDLE h = CreateFileW(BridgeFilePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD read = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    std::string text(buf, read);
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.erase(0, 3);  // UTF-8 BOM

    mini_json::Value root;
    if (!mini_json::Parser(text).Parse(root)) return false;
    const mini_json::Value* p = root.Get("port");
    const mini_json::Value* t = root.Get("token");
    if (!p || !p->IsNumber() || !t || !t->IsString()) return false;
    if (p->number < 1 || p->number > 65535 || t->string.size() < 32 || t->string.size() > 128 || !IsHex(t->string))
        return false;
    port = static_cast<int>(p->number);
    token = t->string;
    return true;
}

int PostToPushServer(int port, const std::string& token, const char* path, const std::string& body) {
    HINTERNET session = WinHttpOpen(L"claude-usage-monitor-push-bridge", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return 0;
    WinHttpSetTimeouts(session, 1000, 2000, 3000, 3000);
    int status = 0;
    HINTERNET connect = WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", Utf8ToWide(path).c_str(), nullptr,
                                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                                : nullptr;
    if (request) {
        std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + Utf8ToWide(token) + L"\r\n";
        if (WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                               const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                               static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD code = 0, size = sizeof(code);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
            status = static_cast<int>(code);
        }
        SecureZeroMemory(&headers[0], headers.size() * sizeof(wchar_t));
        WinHttpCloseHandle(request);
    }
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return status;
}

bool ReadCloudConfig(std::wstring& url, std::string& token) {
    wchar_t custom[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"CUM_CLOUD_CONFIG_FILE", custom, MAX_PATH);
    std::wstring path = (n > 0 && n < MAX_PATH) ? std::wstring(custom) : AppDataFile(L"cloud.json");
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD read = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    std::string text(buf, read);
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.erase(0, 3);  // UTF-8 BOM

    mini_json::Value root;
    if (!mini_json::Parser(text).Parse(root)) return false;
    const mini_json::Value* u = root.Get("url");
    const mini_json::Value* t = root.Get("token");
    if (!u || !u->IsString() || !t || !t->IsString()) return false;
    const std::string& s = u->string;
    bool https = s.rfind("https://", 0) == 0;
    bool local = s.rfind("http://127.0.0.1:", 0) == 0;  // wrangler dev
    if ((!https && !local) || s.size() > 512 || t->string.size() < 32 || t->string.size() > 200) return false;
    for (char c : t->string)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    url = Utf8ToWide(s);
    while (!url.empty() && url.back() == L'/') url.pop_back();
    token = t->string;
    return true;
}

static thread_local DWORD g_post_error = 0;  // WinHTTP error of the last failed PostJsonToUrl

int PostJsonToUrl(const std::wstring& url, const std::string& token, const std::string& body, std::string* response) {
    g_post_error = 0;
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[1024] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) return 0;
    bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET session = WinHttpOpen(L"claude-usage-monitor-push-bridge", 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY */,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(L"claude-usage-monitor-push-bridge", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return 0;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
    int status = 0;
    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    if (request) {
        std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + Utf8ToWide(token) + L"\r\n";
        if (WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                               const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                               static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD code = 0, size = sizeof(code);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
            status = static_cast<int>(code);
            if (response) {
                response->clear();
                char chunk[4096];
                DWORD got = 0;
                while (response->size() < 65536 && WinHttpReadData(request, chunk, sizeof(chunk), &got) && got > 0)
                    response->append(chunk, got);
            }
        }
        else {
            g_post_error = GetLastError();
        }
        SecureZeroMemory(&headers[0], headers.size() * sizeof(wchar_t));
        WinHttpCloseHandle(request);
    } else {
        g_post_error = GetLastError();
    }
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return status;
}

// ---- PushBridge ------------------------------------------------------------------

void PushBridge::Init() {
    InitializeCriticalSection(&lock_);
    LoadState();
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, &PushBridge::ThreadMain, this, 0, nullptr);
}

void PushBridge::SetEnabled(bool enabled) {
    LONG was = InterlockedExchange(&enabled_, enabled ? 1 : 0);
    if (was && !enabled) {
        EnterCriticalSection(&lock_);
        queue_.clear();
        LeaveCriticalSection(&lock_);
    }
}

void PushBridge::OnUsage(const UsageSnapshot& s) {
    // Only fresh, successful data counts; cached or failed polls are ignored.
    if (!s.has_data || s.last_status != FetchStatus::Ok || s.fetched_at <= last_fetched_at_) return;
    last_fetched_at_ = s.fetched_at;

    ResetState before = state_;
    LimitReset events[kResetKindCount];
    int n = DetectResets(state_, s.data, s.fetched_at, primed_, events);
    primed_ = true;
    for (int k = 0; k < kResetKindCount; ++k) {
        if (before.known_resets_at[k] != state_.known_resets_at[k] ||
            before.notified_resets_at[k] != state_.notified_resets_at[k]) {
            SaveState();
            break;
        }
    }

    bool enabled = InterlockedCompareExchange(&enabled_, 0, 0) != 0;
    for (int i = 0; i < n; ++i) {
        AppendAppLog(std::wstring(L"push: ") + Utf8ToWide(ResetKindName(events[i].kind)) + L" confirmed" +
                     (enabled ? L", notifying phone" : L" (phone notifications off)"));
    }
    if (!enabled) return;

    // Send to every configured destination: the local push server and/or the cloud relay.
    int port = 0;
    std::wstring url;
    std::string token;
    bool targets[2] = {ReadPushBridgeConfig(port, token), ReadCloudConfig(url, token)};
    SecureZeroMemory(&token[0], token.size());
    for (int t = 0; t < 2; ++t) {
        if (!targets[t]) continue;
        const char* prefix = t == kCloud ? "/ingest/" : "/internal/";
        for (int i = 0; i < n; ++i) {
            Job job;
            job.target = t;
            job.path = std::string(prefix) + "event";
            job.body = BuildEventJson(events[i]);
            job.is_event = true;
            Enqueue(job);
        }
        Job job;
        job.target = t;
        job.path = std::string(prefix) + "usage";
        job.body = BuildUsageJson(s);
        Enqueue(job);
    }
}

void PushBridge::Notify(const std::string& id, const std::wstring& title, const std::wstring& body) {
    if (InterlockedCompareExchange(&enabled_, 0, 0) == 0) return;
    int port = 0;
    std::wstring url;
    std::string token;
    bool targets[2] = {ReadPushBridgeConfig(port, token), ReadCloudConfig(url, token)};
    SecureZeroMemory(&token[0], token.size());
    for (int t = 0; t < 2; ++t) {
        if (!targets[t]) continue;
        Job job;
        job.target = t;
        job.path = t == kCloud ? "/ingest/notice" : "/internal/notice";
        job.body = BuildNoticeJson(id, title, body);
        job.is_event = true;  // retried like reset events
        Enqueue(job);
    }
}

void PushBridge::OnAccounts(const std::wstring& user_name, const std::vector<AccountInfo>& accounts,
                            const std::vector<PromptCosts>& costs, const std::string& tokens_json,
                            long long checked_at, long long next_check_at, const std::string& extras_json,
                            const std::string& resets_json) {
    if (InterlockedCompareExchange(&enabled_, 0, 0) == 0 || accounts.empty()) return;
    std::string body = BuildAccountsJson(user_name, accounts, costs, tokens_json, checked_at, next_check_at, extras_json, resets_json);
    long long now = NowUnix();
    long long since = now - last_accounts_sent_;
    if (since < 15 || (body == last_accounts_json_ && since < 60)) return;
    std::wstring url;
    std::string token;
    bool cloud = ReadCloudConfig(url, token);
    SecureZeroMemory(&token[0], token.size());
    if (!cloud) return;
    last_accounts_json_ = body;
    last_accounts_sent_ = now;
    Job job;
    job.target = kCloud;
    job.path = "/ingest/accounts";
    job.body = body;
    Enqueue(job);
}

void PushBridge::UploadSoon() { last_accounts_sent_ = 0; }

bool PushBridge::TokensDue(bool flowing) {
    if (InterlockedCompareExchange(&enabled_, 0, 0) == 0 || (!flowing && !tokens_flowing_)) return false;
    ULONGLONG now = GetTickCount64();
    if (now - last_tokens_sent_ < 2000) return false;
    last_tokens_sent_ = now;
    tokens_flowing_ = flowing;
    return true;
}

void PushBridge::SendTokens(const std::string& tokens_json) {
    std::wstring url;
    std::string token;
    bool cloud = ReadCloudConfig(url, token);
    SecureZeroMemory(&token[0], token.size());
    if (!cloud) return;
    Job job;
    job.target = kCloud;
    job.path = "/ingest/tokens";
    job.body = tokens_json;
    Enqueue(job);
}

void PushBridge::StartPhoneCommands(HWND hwnd, UINT msg) {
    commands_hwnd_ = hwnd;
    commands_msg_ = msg;
    HANDLE t = CreateThread(nullptr, 0, &PushBridge::CommandsMain, this, 0, nullptr);
    if (t) CloseHandle(t);
}

DWORD WINAPI PushBridge::CommandsMain(LPVOID param) {
    auto* self = static_cast<PushBridge*>(param);
    const DWORD every = TestApiPort() ? 2000 : 10000;  // end-to-end tests don't wait as long
    for (;;) {
        Sleep(every);
        if (InterlockedCompareExchange(&self->enabled_, 0, 0) == 0) continue;
        std::wstring url;
        std::string token, reply;
        if (!ReadCloudConfig(url, token)) continue;
        int status = PostJsonToUrl(url + L"/ingest/commands", token, "{}", &reply);
        SecureZeroMemory(&token[0], token.size());
        mini_json::Value root;
        if (status != 200 || !mini_json::Parser(reply).Parse(root)) continue;
        const mini_json::Value* refresh = root.Get("refresh");
        if (refresh && refresh->type == mini_json::Value::Bool && refresh->boolean)
            PostMessageW(self->commands_hwnd_, self->commands_msg_, 1, 0);  // wParam 1: refresh
        const mini_json::Value* to = root.Get("switch_to");
        if (to && to->IsString() && !to->string.empty())
            PostMessageW(self->commands_hwnd_, self->commands_msg_, 0,
                         reinterpret_cast<LPARAM>(new std::wstring(Utf8ToWide(to->string))));
    }
}

bool PushBridge::TakePolicies(std::vector<OwnerPolicy>& out) {
    EnterCriticalSection(&lock_);
    bool changed = policies_changed_;
    if (changed) out = policies_;
    policies_changed_ = false;
    LeaveCriticalSection(&lock_);
    return changed;
}

void PushBridge::Enqueue(Job job) {
    EnterCriticalSection(&lock_);
    if (!job.is_event) {  // only the newest update of each kind per destination matters
        for (auto it = queue_.begin(); it != queue_.end();)
            it = (it->is_event || it->target != job.target || it->path != job.path) ? it + 1 : queue_.erase(it);
    }
    queue_.push_back(std::move(job));
    LeaveCriticalSection(&lock_);
    SetEvent(wake_);
}

DWORD WINAPI PushBridge::ThreadMain(LPVOID param) {
    static_cast<PushBridge*>(param)->Loop();
    return 0;
}

void PushBridge::Loop() {
    bool reachable[2] = {true, true};  // per destination; log only when this changes
    for (;;) {
        Job job;
        bool have = false;
        long long wait = -1;
        EnterCriticalSection(&lock_);
        long long now = NowUnix();
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->not_before <= now) {
                job = *it;
                queue_.erase(it);
                have = true;
                break;
            }
            long long w = it->not_before - now;
            if (wait < 0 || w < wait) wait = w;
        }
        LeaveCriticalSection(&lock_);
        if (!have) {
            WaitForSingleObject(wake_, wait < 0 ? INFINITE : static_cast<DWORD>(wait > 3600 ? 3600 : wait) * 1000);
            continue;
        }
        if (InterlockedCompareExchange(&enabled_, 0, 0) == 0) continue;

        int port = 0, status = 0;
        std::wstring url;
        std::string token, reply;
        const bool cloud = job.target == kCloud;
        const bool accounts = job.path == "/ingest/accounts";
        bool configured = cloud ? ReadCloudConfig(url, token) : ReadPushBridgeConfig(port, token);
        if (configured) {
            status = cloud ? PostJsonToUrl(url + Utf8ToWide(job.path), token, job.body, accounts ? &reply : nullptr)
                           : PostToPushServer(port, token, job.path.c_str(), job.body);
        }
        SecureZeroMemory(&token[0], token.size());
        bool ok = status >= 200 && status < 300;
        std::vector<OwnerPolicy> policies;
        if (ok && accounts && ParsePolicies(reply, policies)) {
            EnterCriticalSection(&lock_);
            if (!(policies == policies_)) {
                policies_ = policies;
                policies_changed_ = true;
            }
            LeaveCriticalSection(&lock_);
        }

        if (ok != reachable[job.target]) {
            const wchar_t* name = cloud ? L"cloud relay" : L"push server";
            std::wstring line;
            if (ok)
                line = std::wstring(L"push: connected to the ") + name;
            else if (!configured)
                line = std::wstring(L"push: ") + name + L" not set up";
            else if (status == 0)
                line = cloud ? L"push: cloud relay not reachable (WinHTTP " + std::to_wstring(g_post_error) + L")"
                             : std::wstring(L"push: push server not running (start it with push\\start.ps1)");
            else
                line = std::wstring(L"push: ") + name + L" error (HTTP " + std::to_wstring(status) + L")";
            AppendAppLog(line);
            reachable[job.target] = ok;
        }
        if (!job.is_event) continue;  // usage updates are best effort
        if (ok) {
            AppendAppLog(L"push: delivered " + Utf8ToWide(job.body));
        } else if (status == 400 || status == 401 || status == 403) {
            AppendAppLog(L"push: server rejected event (HTTP " + std::to_wstring(status) + L")");
        } else if (++job.attempts < kMaxEventAttempts) {
            job.not_before = NowUnix() + (15LL << (job.attempts - 1));
            Enqueue(job);
        } else {
            AppendAppLog(L"push: gave up delivering " + Utf8ToWide(job.body));
        }
    }
}

void PushBridge::LoadState() {
    std::wstring path = AppDataFile(L"push-state.ini");
    auto get = [&](const wchar_t* key) {
        wchar_t buf[32] = {};
        GetPrivateProfileStringW(L"reset", key, L"0", buf, 32, path.c_str());
        return std::wcstoll(buf, nullptr, 10);
    };
    state_.known_resets_at[kResetFiveHour] = get(L"known_five_hour");
    state_.known_resets_at[kResetWeekly] = get(L"known_weekly");
    state_.notified_resets_at[kResetFiveHour] = get(L"notified_five_hour");
    state_.notified_resets_at[kResetWeekly] = get(L"notified_weekly");
}

void PushBridge::SaveState() {
    std::wstring path = AppDataFile(L"push-state.ini");
    auto put = [&](const wchar_t* key, long long v) {
        WritePrivateProfileStringW(L"reset", key, std::to_wstring(v).c_str(), path.c_str());
    };
    put(L"known_five_hour", state_.known_resets_at[kResetFiveHour]);
    put(L"known_weekly", state_.known_resets_at[kResetWeekly]);
    put(L"notified_five_hour", state_.notified_resets_at[kResetFiveHour]);
    put(L"notified_weekly", state_.notified_resets_at[kResetWeekly]);
}
