// Turns usage snapshots into display strings shared by every window.
#pragma once
#include <string>
#include <vector>

#include "Accounts.h"
#include "Settings.h"
#include "UsagePoller.h"

struct DisplayItem {
    std::wstring label;
    std::wstring value;
    std::wstring reset;      // countdown, empty when unknown
    float fraction = -1.0f;  // 0..1, < 0 when unknown
    int severity = 0;        // 0 normal, 1 warning, 2 critical
    // Multi-account (claude-swap) styling:
    bool header = false;     // main window: account title line (label = email, sublabel = alias)
    bool compact = false;    // main window: one-line row  label | bar | value | reset
    bool accent = false;     // label in the accent colour (active account)
    bool dim = false;        // standby account
    std::wstring sublabel;
    bool plain = false;      // no bar under it (the token-speed column)
    bool span = false;       // taskbar: one value centred across the whole column (the item after it is left empty)
    double reserve_from = -1;  // a friend freed their reserve: the bar is green from here (0-1) to its end
    double cap_at = -1;        // a cap on this limit: a bright tick across the bar here (0-1)
};

const claude_usage::UsageWindow& WindowFor(const claude_usage::FetchResult& data, int item);
long long RoundPercent(const claude_usage::UsageWindow& w);
int Severity(const claude_usage::UsageWindow& w, int warn_percent, int critical_percent);

// Visible items of `look`, in display order.
std::vector<DisplayItem> BuildItems(const claude_usage::UsageSnapshot& s, const WindowLook& look, int warn_percent,
                                    int critical_percent, long long now);

std::wstring BuildTooltip(const claude_usage::UsageSnapshot& s, long long now);
std::wstring PlanName(const claude_usage::UsageSnapshot& s);

// ---- multiple accounts (claude-swap) ----------------------------------------------

// claude-swap's accounts, with this app's live reading placed on the account whose email
// it was taken with (never on any other), and ctive set to the login Claude Code really
// uses when that is known. Active account first.
std::vector<AccountInfo> MergeAccounts(const AccountsSnapshot& accounts, const claude_usage::UsageSnapshot& live);
// True when the live login belongs to an account claude-swap doesn't have yet.
bool LiveLoginIsUnknown(const AccountsSnapshot& accounts, const claude_usage::UsageSnapshot& live);
// Taskbar: one identical column per account (name, 5h on top, 7d below, time left).
std::vector<DisplayItem> BuildAccountTaskbarItems(const std::vector<AccountInfo>& accounts, const WindowLook& look,
                                                  int warn_percent, int critical_percent, long long now);
// Main window: a header line per account, then one compact line per limit.
std::vector<DisplayItem> BuildAccountMainItems(const std::vector<AccountInfo>& accounts, const WindowLook& look,
                                               int warn_percent, int critical_percent, long long now);
