// Turns usage snapshots into display strings shared by every window.
#pragma once
#include <string>
#include <vector>

#include "Settings.h"
#include "UsagePoller.h"

struct DisplayItem {
    std::wstring label;
    std::wstring value;
    std::wstring reset;      // countdown, empty when unknown
    float fraction = -1.0f;  // 0..1, < 0 when unknown
    int severity = 0;        // 0 normal, 1 warning, 2 critical
};

const claude_usage::UsageWindow& WindowFor(const claude_usage::FetchResult& data, int item);
long long RoundPercent(const claude_usage::UsageWindow& w);
int Severity(const claude_usage::UsageWindow& w, int warn_percent, int critical_percent);

// Visible items of `look`, in display order.
std::vector<DisplayItem> BuildItems(const claude_usage::UsageSnapshot& s, const WindowLook& look, int warn_percent,
                                    int critical_percent, long long now);

std::wstring BuildTooltip(const claude_usage::UsageSnapshot& s, long long now);
std::wstring PlanName(const claude_usage::UsageSnapshot& s);
