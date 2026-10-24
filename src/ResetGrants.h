// Limit resets ("reset grants"): Anthropic sometimes gives an account a one-off reset of its
// 5-hour and weekly limits, with an end date. They only show in the usage response when it's
// asked for (?cedar_ember=1) by Claude Code itself, so a hidden Python run reads them for
// every claude-swap account with its saved login (read only: no refresh, nothing written),
// about every 30 minutes. Shown on the phones only; claiming one is left to Claude Code.
#pragma once
#include <windows.h>

#include <string>

class ResetGrants {
public:
    void Start();
    // {"accounts":[{"email":..., "eligible":bool, "reason":..., "grants":[{"label":..., "left":n,
    // "total":n, "ends_at":unix, "usable":bool}]}]}, or "" before the first good read.
    std::string Json();

private:
    static DWORD WINAPI ThreadMain(LPVOID param);
    void Loop();
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::string json_;  // guarded by lock_
};
