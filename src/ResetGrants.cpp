#include "ResetGrants.h"

#include "Accounts.h"
#include "AppFiles.h"
#include "mini_json.h"

namespace {

const DWORD kEveryMs = 30 * 60 * 1000;

// Reads each saved account's access token through claude-swap's own code and asks the usage
// endpoint for its grants. Prints one JSON line; accounts it can't read are left out.
const char kScript[] = R"PY(
import json, os, sys, urllib.request, datetime, subprocess
try: VER = subprocess.run(["claude", "--version"], capture_output=True, text=True, timeout=30, creationflags=0x08000000).stdout.split()[0]
except Exception: VER = "9.9.9"
from claude_swap.switcher import ClaudeAccountSwitcher
def ts(s):
    try: return int(datetime.datetime.fromisoformat(s).timestamp())
    except Exception: return 0
def ask(tok, path):
    r = urllib.request.Request("https://api.anthropic.com" + path, headers={"Authorization": "Bearer " + tok,
        "anthropic-beta": "oauth-2025-04-20", "User-Agent": "claude-cli/" + VER + " (external, cli)", "x-app": "cli"})
    return json.load(urllib.request.urlopen(r, timeout=20))
tokens = []
try:
    live = json.load(open(os.path.expanduser("~/.claude/.credentials.json")))["claudeAiOauth"]["accessToken"]
    tokens.append((ask(live, "/api/oauth/profile")["account"]["email"].lower(), live))
except Exception: pass
s = ClaudeAccountSwitcher()
seq = json.load(open(os.path.expanduser("~/.claude-swap-backup/sequence.json")))
for num, info in seq.get("accounts", {}).items():
    try: tokens.append((info["email"].lower(), json.loads(s._read_account_credentials(num, info["email"]))["claudeAiOauth"]["accessToken"]))
    except Exception: pass
out, seen = [], set()
for email, tok in tokens:
    if email in seen: continue
    try: ce = ask(tok, "/api/oauth/usage?cedar_ember=1&skip_spend=1").get("cedar_ember") or {}
    except Exception: continue
    seen.add(email)
    out.append({"email": email, "eligible": bool(ce.get("eligible")), "reason": ce.get("ineligible_reason") or "",
        "grants": [{"label": g.get("label") or "", "left": g.get("resets_left") or 0, "total": g.get("resets_total") or 0,
                    "ends_at": ts(g.get("ends_at") or ""), "usable": bool(g.get("usable_now"))} for g in ce.get("grants") or []]})
print(json.dumps({"accounts": out}))
)PY";

std::wstring FindPython() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring p = std::wstring(buf) + L"\Programs\Python\Python313\python.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return L"";
}

}  // namespace

void ResetGrants::Start() {
    HANDLE t = CreateThread(nullptr, 0, &ResetGrants::ThreadMain, this, 0, nullptr);
    if (t) CloseHandle(t);
}

std::string ResetGrants::Json() {
    AcquireSRWLockShared(&lock_);
    std::string copy = json_;
    ReleaseSRWLockShared(&lock_);
    return copy;
}

DWORD WINAPI ResetGrants::ThreadMain(LPVOID param) {
    static_cast<ResetGrants*>(param)->Loop();
    return 0;
}

void ResetGrants::Loop() {
    Sleep(20000);  // let the app and claude-swap settle first
    for (;;) {
        std::wstring py = FindPython();
        std::string out;
        int code = py.empty() ? -1 : RunHidden(py, L"-", out, 120000, false, kScript);
        mini_json::Value v;
        if (code == 0 && mini_json::Parser(out).Parse(v) && v.Get("accounts")) {
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            AcquireSRWLockExclusive(&lock_);
            json_ = out;
            ReleaseSRWLockExclusive(&lock_);
        } else {
            AppendAppLog(L"limit resets: read failed (exit " + std::to_wstring(code) + L")");
        }
        Sleep(kEveryMs);
    }
}
