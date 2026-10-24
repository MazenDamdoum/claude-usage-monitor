#include "ResetDetector.h"

namespace claude_usage {

int DetectResets(ResetState& state, const FetchResult& data, long long fetched_at, bool primed,
                 LimitReset out[kResetKindCount]) {
    const UsageWindow* windows[kResetKindCount] = {&data.five_hour, &data.seven_day};
    int count = 0;
    for (int k = 0; k < kResetKindCount; ++k) {
        const UsageWindow& w = *windows[k];
        if (!w.present) continue;  // limit not reported: nothing to compare against
        long long previous = state.known_resets_at[k];
        long long current = w.resets_at;  // 0 when no window is running
        bool rolled_over = previous > 0 && fetched_at >= previous && (current == 0 || current > previous);
        if (rolled_over && primed && state.notified_resets_at[k] != previous) {
            out[count++] = LimitReset{static_cast<ResetKind>(k), previous};
            state.notified_resets_at[k] = previous;
        }
        state.known_resets_at[k] = current;
    }
    return count;
}

const char* ResetKindName(ResetKind kind) {
    return kind == kResetFiveHour ? "five_hour_reset" : "weekly_reset";
}

}  // namespace claude_usage
