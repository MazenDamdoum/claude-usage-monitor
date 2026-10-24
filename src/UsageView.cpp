#include "UsageView.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

using namespace claude_usage;

const UsageWindow& WindowFor(const FetchResult& data, int item) {
    switch (item) {
    case kItemSession: return data.five_hour;
    case kItemWeekly: return data.seven_day;
    case kItemSonnet: return data.seven_day_sonnet;
    default: return data.seven_day_opus;
    }
}

long long RoundPercent(const UsageWindow& w) {
    return std::llround(w.utilization < 0 ? 0 : w.utilization);
}

int Severity(const UsageWindow& w, int warn_percent, int critical_percent) {
    if (!w.present) return 0;
    long long pct = RoundPercent(w);
    return pct >= critical_percent ? 2 : (pct >= warn_percent ? 1 : 0);
}

std::vector<DisplayItem> BuildItems(const UsageSnapshot& s, const WindowLook& look, int warn_percent,
                                    int critical_percent, long long now) {
    std::vector<DisplayItem> items;
    for (int i = 0; i < kItemCount; ++i) {
        if (!look.items[i].show) continue;
        const UsageWindow& w = WindowFor(s.data, i);
        DisplayItem item;
        item.label = look.items[i].label;
        item.value = L"--";
        if (s.has_data && w.present) {
            item.value = std::to_wstring(RoundPercent(w));
            if (!look.hide_percent) item.value += look.percent_space ? L" %" : L"%";
            if (w.resets_at > 0) item.reset = FormatDuration(w.resets_at - now);
            double f = w.utilization / 100.0;
            item.fraction = static_cast<float>(f < 0 ? 0 : (f > 1 ? 1 : f));
            item.severity = Severity(w, warn_percent, critical_percent);
        }
        items.push_back(item);
    }
    return items;
}

std::wstring PlanName(const UsageSnapshot& s) {
    std::wstring plan = Utf8ToWide(s.data.subscription_type);
    if (!plan.empty()) plan[0] = static_cast<wchar_t>(std::towupper(plan[0]));
    return plan;
}

std::wstring BuildTooltip(const UsageSnapshot& s, long long now) {
    std::wstring t = L"Claude usage";
    std::wstring plan = PlanName(s);
    if (!plan.empty()) t += L" (" + plan + L")";
    if (s.has_data) {
        for (int i = 0; i < kItemCount; ++i) {
            const UsageWindow& w = WindowFor(s.data, i);
            if (!w.present) continue;
            t += L"\n";
            t += kItemNames[i];
            t += L": " + std::to_wstring(RoundPercent(w)) + L"%";
            if (w.resets_at > 0) {
                t += L"  ·  resets in " + FormatDuration(w.resets_at - now);
                std::wstring at = FormatLocalTime(w.resets_at);
                if (!at.empty()) t += L" (" + at + L")";
            }
        }
        long long age = now - s.fetched_at;
        t += age < 60 ? L"\nUpdated " + std::to_wstring(age < 0 ? 0 : age) + L"s ago"
                      : L"\nUpdated " + FormatDuration(age) + L" ago";
        long long next = s.next_attempt - now;
        if (next > 0) t += L"  ·  next in " + (next < 60 ? std::to_wstring(next) + L"s" : FormatDuration(next));
        if (s.live) t += L"  ·  live";
    } else if (s.last_attempt == 0) {
        t += L"\nLoading...";
    }
    if (!s.last_error.empty()) t += L"\n⚠ " + s.last_error;
    return t;
}

// ---- multiple accounts (claude-swap) ------------------------------------------------

namespace {

AccountWindow FromLive(const UsageWindow& w) {
    AccountWindow a;
    a.present = w.present;
    a.percent = w.utilization;
    a.resets_at = w.resets_at;
    return a;
}

int AccountSeverity(const AccountWindow& w, int warn_percent, int critical_percent) {
    if (!w.present) return 0;
    long long pct = std::llround(w.percent < 0 ? 0 : w.percent);
    return pct >= critical_percent ? 2 : (pct >= warn_percent ? 1 : 0);
}

void FillValue(DisplayItem& item, const AccountWindow& w, const WindowLook& look, int warn, int crit, long long now) {
    item.value = L"--";
    if (!w.present) return;
    item.value = std::to_wstring(std::llround(w.percent < 0 ? 0 : w.percent));
    if (!look.hide_percent) item.value += look.percent_space ? L" %" : L"%";
    if (w.resets_at > 0) item.reset = FormatDuration(w.resets_at - now);
    double f = w.percent / 100.0;
    item.fraction = static_cast<float>(f < 0 ? 0 : (f > 1 ? 1 : f));
    item.severity = AccountSeverity(w, warn, crit);
}

std::wstring StatusText(const AccountInfo& a) {
    if (a.status == L"ok" || a.status.empty()) return a.stale ? L"last known" : L"";
    if (a.status == L"token_expired") return L"login refreshing";
    if (a.status == L"relogin_required") return L"needs /login";
    if (a.status == L"api_key") return L"API key";
    return a.stale ? L"last known" : L"unavailable";
}

}  // namespace

static std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

static std::wstring LiveEmail(const UsageSnapshot& live) {
    return live.has_data ? Lower(Utf8ToWide(live.data.account_email)) : L"";
}

bool LiveLoginIsUnknown(const AccountsSnapshot& snapshot, const UsageSnapshot& live) {
    std::wstring email = LiveEmail(live);
    if (!snapshot.available || email.empty()) return false;
    for (const AccountInfo& a : snapshot.accounts)
        if (Lower(a.email) == email) return false;
    return true;
}

std::vector<AccountInfo> MergeAccounts(const AccountsSnapshot& snapshot, const UsageSnapshot& live) {
    std::vector<AccountInfo> out = snapshot.accounts;
    const std::wstring email = LiveEmail(live);
    bool known = false;
    for (const AccountInfo& a : out) known = known || (!email.empty() && Lower(a.email) == email);
    for (AccountInfo& a : out) {
        const bool mine = known && Lower(a.email) == email;
        if (known) a.active = mine;  // the login Claude Code is really using
        if (!mine) continue;
        if (live.data.five_hour.present) a.five_hour = FromLive(live.data.five_hour);
        if (live.data.seven_day.present) a.seven_day = FromLive(live.data.seven_day);
        a.status = L"ok";
        a.stale = false;
    }
    // Each account keeps its place (claude-swap's slot order); the one in use is shown by colour.
    std::stable_sort(out.begin(), out.end(), [](const AccountInfo& x, const AccountInfo& y) {
        return x.number < y.number;
    });
    return out;
}

std::vector<DisplayItem> BuildAccountTaskbarItems(const std::vector<AccountInfo>& accounts, const WindowLook& look,
                                                  int warn, int crit, long long now) {
    std::vector<DisplayItem> items;
    for (const AccountInfo& a : accounts) {
        if (a.disabled) continue;
        // Every account looks the same: name, 5h on top, 7d below, each with the time
        // left until it resets. The one in use is listed first with its name in orange.
        DisplayItem top, bottom;
        top.label = a.ShortName();
        top.accent = a.active;  // the account in use: name in the accent (orange) colour
        FillValue(top, a.five_hour, look, warn, crit, now);
        FillValue(bottom, a.seven_day, look, warn, crit, now);
        items.push_back(top);
        items.push_back(bottom);
    }
    return items;
}

std::vector<DisplayItem> BuildAccountMainItems(const std::vector<AccountInfo>& accounts, const WindowLook& look,
                                               int warn, int crit, long long now) {
    std::vector<DisplayItem> items;
    for (const AccountInfo& a : accounts) {
        DisplayItem head;
        head.header = true;
        head.label = std::to_wstring(a.number) + L"  " + (a.email.empty() ? a.ShortName() : a.email);
        if (!a.alias.empty()) head.sublabel = L"[" + a.alias + L"]";
        head.accent = a.active;
        head.dim = !a.active;
        head.value = a.active ? L"\u25CF active" : (a.disabled ? L"disabled" : StatusText(a));
        items.push_back(head);

        auto row = [&](const std::wstring& label, const AccountWindow& w) {
            DisplayItem r;
            r.compact = true;
            r.label = label;
            r.dim = !a.active;
            FillValue(r, w, look, warn, crit, now);
            items.push_back(r);
        };
        if (a.five_hour.present) row(L"5h", a.five_hour);
        if (a.seven_day.present) row(L"7d", a.seven_day);
        for (const auto& s : a.scoped) row(s.first, s.second);
        if (!a.five_hour.present && !a.seven_day.present && a.scoped.empty()) row(L"usage", AccountWindow{});
    }
    return items;
}
