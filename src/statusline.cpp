// Claude Code's status line command. Claude Code runs it after each reply with a JSON summary on
// stdin; since 2.1.80 that carries rate_limits: the 5-hour and weekly usage exactly as Anthropic's
// own answers report them, with no usage check spent. This saves it for the taskbar app
// (%APPDATA%\ClaudeUsageMonitor\live-usage.json, read once a second) and prints a short line for
// Claude Code's status bar: "5h 34% · wk 21%".
#include <windows.h>

#include <cstdio>
#include <ctime>
#include <string>

#include "mini_json.h"

namespace {

struct Window {
    bool present = false;
    double percent = 0;
    long long resets_at = 0;
};

Window Read(const mini_json::Value* limits, const char* key) {
    Window w;
    const mini_json::Value* v = limits ? limits->Get(key) : nullptr;
    const mini_json::Value* used = v ? v->Get("used_percentage") : nullptr;
    if (!used || !used->IsNumber()) return w;
    w.present = true;
    w.percent = used->number;
    const mini_json::Value* at = v->Get("resets_at");
    if (at && at->IsNumber()) w.resets_at = static_cast<long long>(at->number);
    return w;
}

std::string Json(const Window& w) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "{\"percent\":%.2f,\"resets_at\":%lld}", w.percent, w.resets_at);
    return buf;
}

// Written whole, then moved into place: the app never reads half a file.
void Save(const Window& five, const Window& week) {
    wchar_t appdata[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return;
    std::wstring dir = std::wstring(appdata) + L"\\ClaudeUsageMonitor";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\live-usage.json", temp = path + L".tmp";
    std::string body = "{\"at\":" + std::to_string(static_cast<long long>(std::time(nullptr))) +
                       ",\"five_hour\":" + (five.present ? Json(five) : "null") +
                       ",\"seven_day\":" + (week.present ? Json(week) : "null") + "}";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    bool ok = WriteFile(f, body.data(), static_cast<DWORD>(body.size()), &written, nullptr) && written == body.size();
    CloseHandle(f);
    if (ok) MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(temp.c_str());
}

}  // namespace

int main() {
    std::string input;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) input.append(buf, n);
    static const char kBom[] = {'\xEF', '\xBB', '\xBF'};  // a byte-order mark, if any
    if (input.size() >= 3 && input.compare(0, 3, kBom, 3) == 0) input.erase(0, 3);
    mini_json::Value root;
    if (!mini_json::Parser(input).Parse(root)) return 0;
    const mini_json::Value* limits = root.Get("rate_limits");
    Window five = Read(limits, "five_hour"), week = Read(limits, "seven_day");
    if (!five.present && !week.present) return 0;  // not in this payload (before the first reply)
    Save(five, week);
    std::string line;
    char part[48];
    if (five.present) {
        std::snprintf(part, sizeof(part), "5h %.0f%%", five.percent);
        line += part;
    }
    if (week.present) {
        std::snprintf(part, sizeof(part), "%swk %.0f%%", line.empty() ? "" : " \xC2\xB7 ", week.percent);
        line += part;
    }
    std::fwrite(line.data(), 1, line.size(), stdout);
    return 0;
}
