#include "Accounts.h"

#include "AppFiles.h"
#include "UsageClient.h"
#include "mini_json.h"

using namespace claude_usage;

namespace {

const DWORD kListTimeoutMs = 90'000;  // cswap may refresh a login or two before answering
const int kRefreshSeconds = 60;
const int kIdleRefreshSeconds = 300;  // no accounts saved yet

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? buf : L"";
}

// <base>\Python3*\Scripts\cswap.exe
std::wstring FindInPythonDirs(const std::wstring& base) {
    if (base.empty()) return {};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"\\Python3*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    do {
        std::wstring candidate = base + L"\\" + fd.cFileName + L"\\Scripts\\cswap.exe";
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && FileExists(candidate)) found = candidate;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

void ReadWindow(const mini_json::Value* usage, const char* key, AccountWindow& w) {
    const mini_json::Value* win = usage ? usage->Get(key) : nullptr;
    if (!win || !win->IsObject()) return;
    const mini_json::Value* pct = win->Get("pct");
    if (!pct || !pct->IsNumber()) return;
    w.present = true;
    w.percent = pct->number;
    const mini_json::Value* r = win->Get("resetsAt");
    if (r && r->IsString()) w.resets_at = ParseIso8601(r->string);
}

}  // namespace

std::wstring AccountInfo::ShortName() const {
    if (!alias.empty()) return alias;
    std::wstring name = email.substr(0, email.find(L'@'));
    if (name.size() > 12) name = name.substr(0, 11) + L"…";
    return name.empty() ? L"#" + std::to_wstring(number) : name;
}

bool ParseCswapList(const std::string& json, std::vector<AccountInfo>& out, int* schema_version) {
    out.clear();
    mini_json::Value root;
    if (!mini_json::Parser(json).Parse(root) || !root.IsObject()) return false;
    const mini_json::Value* schema = root.Get("schemaVersion");
    if (schema_version && schema && schema->IsNumber()) *schema_version = static_cast<int>(schema->number);
    const mini_json::Value* accounts = root.Get("accounts");
    if (!accounts || accounts->type != mini_json::Value::Array) return false;
    for (const mini_json::Value& a : accounts->items) {
        if (!a.IsObject()) continue;
        AccountInfo info;
        const mini_json::Value* v;
        if ((v = a.Get("number")) && v->IsNumber()) info.number = static_cast<int>(v->number);
        if ((v = a.Get("email")) && v->IsString()) info.email = Utf8ToWide(v->string);
        if ((v = a.Get("alias")) && v->IsString()) info.alias = Utf8ToWide(v->string);
        if ((v = a.Get("organizationUuid")) && v->IsString()) info.org_uuid = Utf8ToWide(v->string);
        if ((v = a.Get("active")) && v->type == mini_json::Value::Bool) info.active = v->boolean;
        if ((v = a.Get("disabled")) && v->type == mini_json::Value::Bool) info.disabled = v->boolean;
        if ((v = a.Get("usageStatus")) && v->IsString()) info.status = Utf8ToWide(v->string);

        const mini_json::Value* usage = a.Get("usage");
        if (!usage || !usage->IsObject()) {
            usage = a.Get("lastGoodUsage");  // display last-known numbers, marked stale
            info.stale = usage && usage->IsObject();
        }
        if (usage && usage->IsObject()) {
            ReadWindow(usage, "fiveHour", info.five_hour);
            ReadWindow(usage, "sevenDay", info.seven_day);
            const mini_json::Value* scoped = usage->Get("scoped");
            if (scoped && scoped->type == mini_json::Value::Array) {
                for (const mini_json::Value& s : scoped->items) {
                    const mini_json::Value* name = s.Get("name");
                    const mini_json::Value* pct = s.Get("pct");
                    if (!name || !name->IsString() || !pct || !pct->IsNumber()) continue;
                    AccountWindow w;
                    w.present = true;
                    w.percent = pct->number;
                    const mini_json::Value* r = s.Get("resetsAt");
                    if (r && r->IsString()) w.resets_at = ParseIso8601(r->string);
                    info.scoped.emplace_back(Utf8ToWide(name->string), w);
                }
            }
        }
        if (info.number > 0) out.push_back(info);
    }
    return true;
}

static std::string IsoUtc(long long unix_seconds) {
    long long days = unix_seconds / 86400, secs = unix_seconds % 86400;
    // civil-from-days (Howard Hinnant)
    days += 719468;
    long long era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = static_cast<long long>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02lld:%02lld:%02lld+00:00", y, m, d, secs / 3600, secs / 60 % 60,
                  secs % 60);
    return buf;
}

static std::string JsonText(const std::wstring& s) {
    std::string out = "\"";
    for (unsigned char c : WideToUtf8(s)) {
        if (c == '"' || c == '\\') out += '\\';
        if (c >= 0x20) out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string BuildUsageShareJson(int schema_version, const AccountInfo& a, long long age_seconds) {
    auto window = [](const AccountWindow& w) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "{\"pct\":%.2f", w.percent);
        std::string out = buf;
        if (w.resets_at > 0) out += ",\"resetsAt\":\"" + IsoUtc(w.resets_at) + "\"";
        return out + "}";
    };
    std::string usage;
    if (a.five_hour.present) usage += "\"fiveHour\":" + window(a.five_hour);
    if (a.seven_day.present) usage += std::string(usage.empty() ? "" : ",") + "\"sevenDay\":" + window(a.seven_day);
    if (!a.scoped.empty()) {
        usage += std::string(usage.empty() ? "" : ",") + "\"scoped\":[";
        for (size_t i = 0; i < a.scoped.size(); ++i) {
            std::string w = window(a.scoped[i].second);
            usage += (i ? "," : "") + w.substr(0, w.size() - 1) + ",\"name\":" + JsonText(a.scoped[i].first) + "}";
        }
        usage += "]";
    }
    return "{\"schemaVersion\":" + std::to_string(schema_version) + ",\"accounts\":[{\"email\":" + JsonText(a.email) +
           ",\"organizationUuid\":" + JsonText(a.org_uuid) + ",\"usageStatus\":\"ok\",\"usageAgeSeconds\":" +
           std::to_string(age_seconds < 0 ? 0 : age_seconds) + ",\"usage\":{" + usage + "}}]}";
}

struct ShareJob {
    std::wstring exe;
    std::string document;
    int hold;
};

static DWORD WINAPI ShareMain(LPVOID param) {
    ShareJob* job = static_cast<ShareJob*>(param);
    std::string out;
    int code = RunHidden(job->exe, L"import-usage - --hold " + std::to_wstring(job->hold), out, 30000, true, job->document);
    if (code != 0) AppendAppLog(L"accounts: cswap import-usage failed (exit " + std::to_wstring(code) + L")");
    delete job;
    return 0;
}

void AccountsSource::ShareReading(const std::string& document, int hold_seconds) {
    AcquireSRWLockShared(&lock_);
    std::wstring exe = exe_;
    ReleaseSRWLockShared(&lock_);
    if (exe.empty()) return;
    HANDLE t = CreateThread(nullptr, 0, &ShareMain, new ShareJob{exe, document, hold_seconds}, 0, nullptr);
    if (t) CloseHandle(t);
}

std::wstring FindCswap(const std::wstring& override_path) {
    if (!override_path.empty()) return FileExists(override_path) ? override_path : L"";
    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"cswap.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    std::wstring profile = Env(L"USERPROFILE");
    for (const std::wstring& c : {profile + L"\\.local\\bin\\cswap.exe", profile + L"\\pipx\\venvs\\claude-swap\\Scripts\\cswap.exe"})
        if (FileExists(c)) return c;
    std::wstring p = FindInPythonDirs(Env(L"APPDATA") + L"\\Python");
    if (p.empty()) p = FindInPythonDirs(Env(L"LOCALAPPDATA") + L"\\Programs\\Python");
    return p;
}

int RunHidden(const std::wstring& exe, const std::wstring& args, std::string& out, DWORD timeout_ms,
              bool include_stderr, const std::string& input) {
    out.clear();
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return -1;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    HANDLE in_read = nullptr, in_write = nullptr;  // what the program reads as typed input
    if (!input.empty() && CreatePipe(&in_read, &in_write, &sa, 0)) {
        SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
        DWORD put = 0;
        WriteFile(in_write, input.data(), static_cast<DWORD>(input.size()), &put, nullptr);
        CloseHandle(in_write);
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read ? in_read : nul;
    si.hStdOutput = write;
    si.hStdError = include_stderr ? write : nul;  // cswap prints human-readable messages on stderr
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write);
    if (in_read) CloseHandle(in_read);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(read);
        return -1;
    }

    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(read, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            if (!ReadFile(read, buf, sizeof(buf), &got, nullptr) || got == 0) break;
            if (out.size() < (1u << 20)) out.append(buf, got);
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            DWORD got = 0;
            while (ReadFile(read, buf, sizeof(buf), &got, nullptr) && got > 0)
                if (out.size() < (1u << 20)) out.append(buf, got);
            break;
        }
        if (GetTickCount64() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            break;
        }
    }
    DWORD code = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read);
    return static_cast<int>(code);
}

// ---- AccountsSource -----------------------------------------------------------------

void AccountsSource::Start(HWND notify_hwnd, UINT notify_msg, const std::wstring& cswap_override) {
    AcquireSRWLockExclusive(&lock_);
    exe_ = FindCswap(cswap_override);
    ReleaseSRWLockExclusive(&lock_);
    if (thread_) {
        RefreshNow();
        return;
    }
    notify_hwnd_ = notify_hwnd;
    notify_msg_ = notify_msg;
    // cswap is Python: ask for UTF-8 JSON and no colour codes.
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"NO_COLOR", L"1");
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, &AccountsSource::ThreadMain, this, 0, nullptr);
}

void AccountsSource::RefreshNow() {
    if (wake_) SetEvent(wake_);
}

bool AccountsSource::Found() {
    AcquireSRWLockShared(&lock_);
    bool found = !exe_.empty();
    ReleaseSRWLockShared(&lock_);
    return found;
}

AccountsSnapshot AccountsSource::Snapshot() {
    AcquireSRWLockShared(&lock_);
    AccountsSnapshot copy = shared_;
    ReleaseSRWLockShared(&lock_);
    return copy;
}

DWORD WINAPI AccountsSource::ThreadMain(LPVOID param) {
    static_cast<AccountsSource*>(param)->Loop();
    return 0;
}

void AccountsSource::Loop() {
    for (;;) {
        AcquireSRWLockShared(&lock_);
        std::wstring exe = exe_;
        ReleaseSRWLockShared(&lock_);

        int wait = kIdleRefreshSeconds;
        bool any = false;
        if (!exe.empty()) {
            std::string out;
            int code = RunHidden(exe, L"list --json", out, kListTimeoutMs);
            std::vector<AccountInfo> accounts;
            int schema = 0;
            bool parsed = ParseCswapList(out, accounts, &schema);
            AcquireSRWLockExclusive(&lock_);
            any = !accounts.empty();
            if (parsed) {
                shared_.available = true;
                shared_.schema_version = schema;
                shared_.accounts = accounts;
                shared_.fetched_at = NowUnix();
                shared_.error.clear();
            } else {
                shared_.error = L"cswap list failed (exit " + std::to_wstring(code) + L")";
            }
            ++shared_.version;
            ReleaseSRWLockExclusive(&lock_);
            if (!parsed) AppendAppLog(L"accounts: " + shared_.error);
            if (notify_hwnd_) PostMessageW(notify_hwnd_, notify_msg_, 0, 0);
            wait = any ? kRefreshSeconds : kIdleRefreshSeconds;
        }
        WaitForSingleObject(wake_, static_cast<DWORD>(wait) * 1000);
    }
}

void AccountsSource::Switch(int number) { RunCommand(kSwitch, L"switch " + std::to_wstring(number) + L" --json"); }

void AccountsSource::Rename(int number, const std::wstring& name) {
    RunCommand(kRename, L"alias " + std::to_wstring(number) + L" " + name);
}

void AccountsSource::Remove(int number) { RunCommand(kRemove, L"remove " + std::to_wstring(number)); }

void AccountsSource::AddCurrent(const std::wstring& alias) {
    RunCommand(kAddCurrent, alias.empty() ? std::wstring(L"add") : L"add --alias " + alias);
}

bool AccountsSource::SwitchBest() {
    if (Busy()) return false;
    RunCommand(kSwitchBest, L"switch --strategy best --json");
    return true;
}

std::wstring AccountsSource::LastCommandOutput() {
    AcquireSRWLockShared(&lock_);
    std::wstring out = command_output_;
    ReleaseSRWLockShared(&lock_);
    return out;
}

void AccountsSource::RunCommand(Command kind, const std::wstring& args) {
    if (InterlockedCompareExchange(&busy_, 1, 0) != 0) return;  // one at a time
    command_ = kind;
    command_args_ = args;
    HANDLE t = CreateThread(nullptr, 0, &AccountsSource::CommandMain, this, 0, nullptr);
    if (t) CloseHandle(t);
    else InterlockedExchange(&busy_, 0);
}

DWORD WINAPI AccountsSource::CommandMain(LPVOID param) {
    auto* self = static_cast<AccountsSource*>(param);
    AcquireSRWLockShared(&self->lock_);
    std::wstring exe = self->exe_;
    ReleaseSRWLockShared(&self->lock_);
    std::string out;
    int code = -1;
    bool ok = false;
    // A switch rewrites Claude Code's settings file; while Claude Code has it open, Windows says
    // "access is denied" and claude-swap rolls back. Wait a moment and try again (up to 4 times).
    const int tries = (self->command_ == kSwitch || self->command_ == kSwitchBest) ? 4 : 1;
    for (int attempt = 1; attempt <= tries && !ok; ++attempt) {
        if (attempt > 1) Sleep(1500);
        code = exe.empty() ? -1 : RunHidden(exe, self->command_args_, out, kListTimeoutMs,
                                            self->command_ != kSwitchBest,  // JSON on stdout only
                                            self->command_ == kRemove ? "y\n" : "");  // "Are you sure?"
        ok = code == 0 && out.find("\"error\"") == std::string::npos;
        if (!ok && attempt < tries) AppendAppLog(L"accounts: cswap " + self->command_args_ + L" refused, trying again");
    }
    std::wstring text = Utf8ToWide(out);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
    AcquireSRWLockExclusive(&self->lock_);
    self->command_output_ = text;
    ReleaseSRWLockExclusive(&self->lock_);
    AppendAppLog(L"accounts: cswap " + self->command_args_ + (ok ? L" done" : L" failed (exit " + std::to_wstring(code) + L")"));
    Command kind = self->command_;
    InterlockedExchange(&self->busy_, 0);
    if (self->notify_hwnd_) PostMessageW(self->notify_hwnd_, kMsgCommandDone, ok ? 1 : 0, kind);
    self->RefreshNow();
    return 0;
}