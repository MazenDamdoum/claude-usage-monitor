// "How many prompts do I have left?" per account, at each effort level.
//
// Claude Code's own transcripts (<config dir>\projects\*\*.jsonl) record every reply's
// model, effort and token usage. From them we learn what one of your Opus prompts costs at
// low, medium and high effort (the median over the last 14 days). From successive usage
// readings we learn how many percent of an account's 5-hour and weekly limits those tokens
// use. Remaining percent / percent per prompt = prompts left.
//
// Tokens are weighed roughly by price ("units"): output 5, input 1, cache write 1.25 (5 min)
// or 2 (1 hour), cache read 0.1; Sonnet x0.6, Haiku x0.2. The weights only need the right
// proportions: the calibration supplies the scale. Usage from elsewhere (claude.ai, the
// phone) has no transcript, which makes the estimates err on the low side.
#pragma once
#include <windows.h>

#include <map>
#include <string>
#include <vector>

enum Effort { kEffortLow = 0, kEffortMedium = 1, kEffortHigh = 2, kEffortCount = 3 };

// Weighted token units of one reply. Exposed for tests.
double TokenUnits(const std::string& model, double input, double cache_write_5m, double cache_write_1h,
                  double cache_read, double output);

// What the transcripts say. Not thread-safe; PromptStats guards it.
class TranscriptStats {
public:
    struct FileContext {
        std::string prompt;        // promptId of the turn being written
        std::string last_message;  // a reply spans several lines with the same message id
    };
    void AddLine(FileContext& ctx, const std::string& line);
    double UnitsBetween(long long from, long long to) const;  // replies written in (from, to]
    // Tokens Claude writes per second (output, like Claude Code's own "↓ tokens"), all sessions
    // together, over the last `window` seconds up to `now` (unix seconds with fractions).
    // Claude Code logs a reply's tokens only once it is finished, so each reply is spread over the
    // time it took (from its "api-request" line), and a reply still being written counts at that
    // session's recent writing speed until its real count lands.
    double TokensPerSecond(double now, double window) const;
    // Tokens per second for each of the last `minutes` whole minutes, oldest first.
    std::vector<double> TokensPerSecondByMinute(long long now, int minutes) const;
    // Median units of an Opus prompt at each effort over the last 14 days. Levels with too few
    // prompts are filled from the others (low = half of medium, high = twice), and the order
    // low <= medium <= high is kept. All 0 when there are no Opus prompts at all.
    void MedianPromptUnits(double out[kEffortCount], long long now) const;
    void Prune(long long now);

private:
    struct Prompt {
        double units = 0;
        long long last = 0;
        int effort[kEffortCount + 1] = {};  // replies per effort (+ unknown)
        int opus = 0;
        int replies = 0;
    };
    struct Flight {
        double start = 0;  // when the reply being written was requested; 0 = none
        double speed = 0;  // this session's recent writing speed, tokens per second
    };
    void Spread(double tokens, double from, double to);

    std::map<long long, double> seconds_;  // unix second -> units
    std::map<long long, double> written_;  // unix second -> output tokens, spread over each reply's time
    std::map<const FileContext*, Flight> flights_;  // per session (transcript)
    double typical_speed_ = 0;  // across sessions, for one that hasn't replied yet
    std::map<std::string, Prompt> prompts_;
};

// Percent of a limit per unit, learnt per account. Exposed for tests.
class UsageCalibration {
public:
    // window: 0 = 5-hour, 1 = weekly.
    void Observe(const std::string& email, int window, double delta_percent, double units);
    // False while there is too little data for this account and for all accounts together.
    bool PercentPerUnit(const std::string& email, int window, double& out) const;
    void Load(const std::wstring& path);
    void Save(const std::wstring& path) const;

private:
    struct Sums {
        double percent = 0;
        double units = 0;
    };
    std::map<std::string, Sums> sums_[2];
};

// Percent of one window used by one prompt at each effort; 0 = unknown.
struct PromptCosts {
    double percent[2][kEffortCount] = {};  // [window][effort]
};

// Background scanner + calibration. Start once; everything else is thread-safe.
class PromptStats {
public:
    void Start(const std::wstring& projects_dir);
    // A fresh usage reading of the signed-in account (lowercase email). Paired with the
    // previous reading of the same account once the transcripts up to `at` have been read.
    void OnReading(const std::string& email, long long at, double five_pct, long long five_reset, double week_pct,
                   long long week_reset);
    PromptCosts CostsFor(const std::string& email);
    double TokenRate();  // tokens Claude is writing per second, live (last 3 s)
    // Like Claude Code's "↓ 1.6k tokens": what Claude has written since this stretch of work began
    // (all sessions together), piling up as it writes. 30 s without a token ends the stretch; the
    // next one starts a new pile (a new `pile` number).
    struct Pile {
        double rate = 0;      // TokenRate
        double count = 0;     // tokens written in this stretch so far
        bool working = false; // a stretch is under way
        long long pile = 0;   // which stretch
    };
    Pile Live();
    std::vector<double> TokenHistory(int minutes);  // per-minute tokens per second, oldest first
    std::vector<double> TokenSeries(int seconds);   // TokenRate at each of the last `seconds`, oldest first
    // Percent of each window (5-hour, weekly) used since `since` by the work in the transcripts,
    // for the signed-in account. Between usage checks this keeps its numbers live, so the app can
    // switch accounts before a limit is hit rather than up to two minutes after. A window whose
    // rate isn't known yet gets 0; false when neither is.
    bool PercentSince(const std::string& email, long long since, double out[2]);
    // Tokens left in the 5-hour window, as tokens Claude writes (the kind tok/s counts; reading costs
    // a fifth as much, so it takes from them too): what was left at the reading (`five_pct` used at
    // `since`), less what's been spent since, at this account's learnt rate. False while unknown.
    bool TokensLeft(const std::string& email, double five_pct, long long since, double& out);

    struct Reading {
        std::string email;
        long long at = 0;
        double pct[2] = {};
        long long reset[2] = {};
    };

private:
    struct FileState {
        unsigned long long offset = 0;
        TranscriptStats::FileContext ctx;
    };
    static DWORD WINAPI ThreadMain(LPVOID param);
    void Loop();
    void Scan();
    void ScanFile(const std::wstring& path, FileState& state);
    void Calibrate();

    std::wstring dir_;
    HANDLE wake_ = nullptr;
    SRWLOCK lock_ = SRWLOCK_INIT;
    // guarded by lock_
    TranscriptStats stats_;
    UsageCalibration calibration_;
    std::map<std::string, Reading> last_reading_;
    std::vector<Reading> pending_;
    long long scanned_through_ = 0;
    double pile_start_ = 0;   // the stretch under way began here; 0 = none
    double pile_anchor_ = 0;  // counted up to here into pile_base_ (keeps the sum short)
    double pile_base_ = 0;
    double last_flow_ = 0;
    long long pile_id_ = 0;
    // scanner thread only
    std::map<std::wstring, FileState> files_;
};

// Usage readings already logged in app.log (seeds the calibration on first run). Exposed for tests.
std::vector<PromptStats::Reading> ReadingsFromLog(const std::wstring& path);
