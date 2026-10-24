// Detects confirmed limit resets from consecutive fresh usage fetches.
//
// A reset is confirmed only when fresh API data shows that the previous window
// is over: the fetch happened at/after the old reset time AND the API now
// reports a different, later window (or no running window). A timestamp that
// merely changes before the reset time never counts, and the first fetch after
// startup only records state.
#pragma once
#include "UsageClient.h"

namespace claude_usage {

enum ResetKind { kResetFiveHour = 0, kResetWeekly = 1, kResetKindCount = 2 };

struct ResetState {
    long long known_resets_at[kResetKindCount] = {};     // window end in the latest fresh data
    long long notified_resets_at[kResetKindCount] = {};  // last reset announced (dedupe)
};

struct LimitReset {
    ResetKind kind;
    long long reset_at;  // the moment the old window ended (unix seconds)
};

// Feed every fresh, successful fetch in order. `primed` must be false for the
// first fetch after startup. Writes up to two events to `out`; returns the count.
int DetectResets(ResetState& state, const FetchResult& data, long long fetched_at, bool primed,
                 LimitReset out[kResetKindCount]);

const char* ResetKindName(ResetKind kind);  // "five_hour_reset" / "weekly_reset"

}  // namespace claude_usage
