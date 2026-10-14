#include "UsageView.h"

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
        t += age < 60 ? std::wstring(L"\nUpdated just now") : L"\nUpdated " + FormatDuration(age) + L" ago";
    } else if (s.last_attempt == 0) {
        t += L"\nLoading...";
    }
    if (!s.last_error.empty()) t += L"\n⚠ " + s.last_error;
    return t;
}
