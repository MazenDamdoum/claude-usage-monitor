#include "PromptStats.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "AppFiles.h"
#include "UsageClient.h"
#include "mini_json.h"

using namespace claude_usage;

namespace {

const long long kPromptHistory = 14 * 86400;  // prompts the medians are taken over
const long long kUnitHistory = 2 * 86400;     // per-minute units kept for calibration
const long long kMaxGap = 30 * 60;            // readings further apart aren't paired
const double kDecay[2] = {0.99, 0.998};       // calibration memory: 5-hour, weekly
const double kMinPercent[2] = {3.0, 1.0};     // percent observed before an estimate is shown
const int kMinPrompts = 3;
const double kMaxStep = 10.0;                 // percent between two readings that counts as a sample

double Num(const mini_json::Value* v) { return v && v->IsNumber() ? v->number : 0.0; }

std::string Str(const mini_json::Value* v) { return v && v->IsString() ? v->string : std::string(); }

const double kMaxReply = 600;      // seconds; a request open longer than this was abandoned
const double kDefaultSpeed = 60;   // tokens per second before any reply has been timed
// A reply still being written is counted at this share of its session's usual speed. Erring low
// matters: the shown pile never runs backwards, so an estimate that overshot would hold it still
// until the real count caught up; one that undershot just catches up with a jump when it lands.
const double kEstimateShare = 0.7;

// A line's top-level "timestamp", in unix seconds with milliseconds; 0 if there is none.
double TimeOf(const std::string& line) {
    size_t at = line.find("\"timestamp\":\"");
    if (at == std::string::npos) return 0;
    at += 13;
    size_t end = line.find('"', at);
    if (end == std::string::npos || end - at > 40) return 0;
    std::string text = line.substr(at, end - at);
    long long whole = ParseIso8601(text);
    if (whole <= 0) return 0;
    double fraction = 0, scale = 0.1;
    size_t dot = text.find('.');
    for (size_t i = dot == std::string::npos ? text.size() : dot + 1; i < text.size() && isdigit(static_cast<unsigned char>(text[i])); ++i, scale /= 10)
        fraction += (text[i] - '0') * scale;
    return whole + fraction;
}

double NowPrecise() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return t.QuadPart / 1e7 - 11644473600.0;
}

}  // namespace

double TokenUnits(const std::string& model, double input, double cache_write_5m, double cache_write_1h,
                  double cache_read, double output) {
    double weight = 1.0;  // Opus and anything newer we don't know
    if (model.find("sonnet") != std::string::npos) weight = 0.6;
    else if (model.find("haiku") != std::string::npos) weight = 0.2;
    else if (model.empty() || model[0] == '<') weight = 0.0;  // "<synthetic>": no API call
    return weight * (input + cache_write_5m * 1.25 + cache_write_1h * 2.0 + cache_read * 0.1 + output * 5.0);
}

// ---- TranscriptStats -------------------------------------------------------------

void TranscriptStats::AddLine(FileContext& ctx, const std::string& line) {
    // Only top-level keys appear unescaped, so a plain search can't be fooled by content.
    if (line.find("\"type\":\"assistant\"") == std::string::npos) {
        if (line.find("\"type\":\"api-request\"") != std::string::npos) {  // Claude starts writing a reply
            double t = TimeOf(line);
            if (t > 0) flights_[&ctx].start = t;
            return;
        }
        if (line.find("\"type\":\"user\"") != std::string::npos) {  // a reply still open was interrupted
            auto f = flights_.find(&ctx);
            if (f != flights_.end()) f->second.start = 0;
        }
        size_t at = line.find("\"promptId\":\"");
        if (at != std::string::npos && line.find("\"type\":\"user\"") != std::string::npos) {
            at += 12;
            size_t end = line.find('"', at);
            if (end != std::string::npos && end - at <= 100) ctx.prompt = line.substr(at, end - at);
        }
        return;
    }
    mini_json::Value root;
    if (!mini_json::Parser(line).Parse(root) || Str(root.Get("type")) != "assistant") return;
    const mini_json::Value* message = root.Get("message");
    const mini_json::Value* usage = message ? message->Get("usage") : nullptr;
    if (!usage || !usage->IsObject()) return;
    std::string id = Str(message->Get("id"));
    if (!id.empty() && id == ctx.last_message) return;  // the same reply, another content block
    ctx.last_message = id;
    long long at = ParseIso8601(Str(root.Get("timestamp")));
    if (at <= 0) return;

    std::string model = Str(message->Get("model"));
    double write_total = Num(usage->Get("cache_creation_input_tokens"));
    double write_1h = 0;
    if (const mini_json::Value* cc = usage->Get("cache_creation")) write_1h = Num(cc->Get("ephemeral_1h_input_tokens"));
    double units = TokenUnits(model, Num(usage->Get("input_tokens")), std::max(0.0, write_total - write_1h), write_1h,
                              Num(usage->Get("cache_read_input_tokens")), Num(usage->Get("output_tokens")));
    Flight& flight = flights_[&ctx];
    double end = std::max(TimeOf(line), static_cast<double>(at));
    double start = flight.start > 0 && flight.start <= end && end - flight.start < kMaxReply ? flight.start : end;
    flight.start = 0;
    if (units <= 0) return;
    seconds_[at] += units;
    double output = Num(usage->Get("output_tokens"));
    Spread(output, start, end);
    if (end - start >= 1 && output >= 20) {  // learn how fast this session writes
        double speed = output / (end - start);
        flight.speed = flight.speed > 0 ? 0.6 * flight.speed + 0.4 * speed : speed;
        typical_speed_ = typical_speed_ > 0 ? 0.8 * typical_speed_ + 0.2 * speed : speed;
    }

    if (ctx.prompt.empty()) return;
    Prompt& p = prompts_[ctx.prompt];
    p.units += units;
    p.last = std::max(p.last, at);
    std::string effort = Str(root.Get("effort"));
    int e = effort == "low" ? kEffortLow : effort == "medium" ? kEffortMedium : effort == "high" ? kEffortHigh : kEffortCount;
    p.effort[e] += 1;
    if (model.find("opus") != std::string::npos) p.opus += 1;
    p.replies += 1;
}

double TranscriptStats::UnitsBetween(long long from, long long to) const {
    double total = 0;
    for (auto it = seconds_.upper_bound(from); it != seconds_.end() && it->first <= to; ++it) total += it->second;
    return total;
}

void TranscriptStats::Spread(double tokens, double from, double to) {
    if (tokens <= 0) return;
    if (to - from < 0.05) {
        written_[static_cast<long long>(std::floor(to))] += tokens;
        return;
    }
    for (long long s = static_cast<long long>(std::floor(from)); s <= static_cast<long long>(std::floor(to)); ++s) {
        double lo = std::max(from, static_cast<double>(s)), hi = std::min(to, s + 1.0);
        if (hi > lo) written_[s] += tokens * (hi - lo) / (to - from);
    }
}

std::vector<double> TranscriptStats::TokensPerSecondByMinute(long long now, int minutes) const {
    std::vector<double> out(minutes, 0.0);
    for (auto it = written_.upper_bound(now - minutes * 60LL); it != written_.end() && it->first <= now; ++it) {
        long long slot = (it->first - (now - minutes * 60LL)) / 60;
        if (slot >= 0 && slot < minutes) out[static_cast<size_t>(slot)] += it->second / 60.0;
    }
    return out;
}

double TranscriptStats::TokensPerSecond(double now, double window) const {
    const double from = now - window;
    double total = 0;
    for (auto it = written_.lower_bound(static_cast<long long>(std::floor(from))); it != written_.end() && it->first <= now; ++it) {
        double lo = std::max(from, static_cast<double>(it->first)), hi = std::min(now, it->first + 1.0);
        if (hi > lo) total += it->second * (hi - lo);
    }
    for (const auto& entry : flights_) {  // replies still being written: their session's usual speed
        const Flight& f = entry.second;
        if (f.start <= 0 || now - f.start > kMaxReply) continue;
        double speed = kEstimateShare * (f.speed > 0 ? f.speed : typical_speed_ > 0 ? typical_speed_ : kDefaultSpeed);
        double lo = std::max(from, f.start);
        if (now > lo) total += speed * (now - lo);
    }
    return total / window;
}

void TranscriptStats::MedianPromptUnits(double out[kEffortCount], long long now) const {
    std::vector<double> costs[kEffortCount];
    for (const auto& kv : prompts_) {
        const Prompt& p = kv.second;
        if (p.last < now - kPromptHistory || p.opus * 2 <= p.replies) continue;  // mostly Opus prompts only
        int best = kEffortCount;
        for (int e = 0; e < kEffortCount; ++e)
            if (p.effort[e] > 0 && (best == kEffortCount || p.effort[e] > p.effort[best])) best = e;
        if (best != kEffortCount) costs[best].push_back(p.units);
    }
    const double ratio[kEffortCount] = {0.5, 1.0, 2.0};
    double base = 0;
    int known = 0;
    for (int e = 0; e < kEffortCount; ++e) {
        out[e] = 0;
        std::vector<double>& c = costs[e];
        if (static_cast<int>(c.size()) < kMinPrompts) continue;
        std::nth_element(c.begin(), c.begin() + c.size() / 2, c.end());
        out[e] = c[c.size() / 2];
        base += out[e] / ratio[e];
        ++known;
    }
    if (!known) return;
    base /= known;
    for (int e = 0; e < kEffortCount; ++e)
        if (out[e] <= 0) out[e] = base * ratio[e];
    out[kEffortLow] = std::min(out[kEffortLow], out[kEffortMedium]);
    out[kEffortHigh] = std::max(out[kEffortHigh], out[kEffortMedium]);
}

void TranscriptStats::Prune(long long now) {
    seconds_.erase(seconds_.begin(), seconds_.lower_bound(now - kUnitHistory));
    written_.erase(written_.begin(), written_.lower_bound(now - 20 * 60));  // the phone's 15-minute chart
    for (auto it = prompts_.begin(); it != prompts_.end();)
        it = it->second.last < now - kPromptHistory ? prompts_.erase(it) : std::next(it);
}

// ---- UsageCalibration --------------------------------------------------------------

void UsageCalibration::Observe(const std::string& email, int window, double delta_percent, double units) {
    if (window < 0 || window > 1 || delta_percent < 0 || units < 0 || (delta_percent == 0 && units == 0)) return;
    Sums& s = sums_[window][email];
    s.percent = s.percent * kDecay[window] + delta_percent;
    s.units = s.units * kDecay[window] + units;
}

bool UsageCalibration::PercentPerUnit(const std::string& email, int window, double& out) const {
    if (window < 0 || window > 1) return false;
    auto it = sums_[window].find(email);
    if (it != sums_[window].end() && it->second.percent >= kMinPercent[window] && it->second.units > 0) {
        out = it->second.percent / it->second.units;
        return true;
    }
    Sums all;  // no data for this account yet: what the other accounts show
    for (const auto& kv : sums_[window]) {
        all.percent += kv.second.percent;
        all.units += kv.second.units;
    }
    if (all.percent < kMinPercent[window] || all.units <= 0) return false;
    out = all.percent / all.units;
    return true;
}

void UsageCalibration::Load(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return;
    char email[256];
    int window = 0;
    double percent = 0, units = 0;
    while (std::fscanf(f, "%255s %d %lf %lf", email, &window, &percent, &units) == 4) {
        if (window >= 0 && window <= 1 && percent >= 0 && units >= 0) sums_[window][email] = Sums{percent, units};
    }
    std::fclose(f);
}

void UsageCalibration::Save(const std::wstring& path) const {
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return;
    for (int w = 0; w < 2; ++w)
        for (const auto& kv : sums_[w]) std::fprintf(f, "%s %d %.6f %.3f\n", kv.first.c_str(), w, kv.second.percent, kv.second.units);
    std::fclose(f);
}

// ---- PromptStats ------------------------------------------------------------------

// First run: learn from the readings already in app.log ("2026-10-15 14:25:13  ok  5h=4%
// 7d=41%  name@example.com ..."), so estimates show up without waiting for new usage. The log
// has no reset times; a drop in percent marks a reset, which pairing skips anyway.
std::vector<PromptStats::Reading> ReadingsFromLog(const std::wstring& path) {
    std::vector<PromptStats::Reading> out;
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return out;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        SYSTEMTIME local = {}, utc = {};
        int y, mo, d, h, mi, s, five, week;
        char email[256];
        if (std::sscanf(line, "%d-%d-%d %d:%d:%d  ok  5h=%d%%  7d=%d%%  %255s", &y, &mo, &d, &h, &mi, &s, &five, &week,
                        email) != 9 ||
            !std::strchr(email, '@'))
            continue;
        local.wYear = static_cast<WORD>(y);
        local.wMonth = static_cast<WORD>(mo);
        local.wDay = static_cast<WORD>(d);
        local.wHour = static_cast<WORD>(h);
        local.wMinute = static_cast<WORD>(mi);
        local.wSecond = static_cast<WORD>(s);
        FILETIME ft;
        if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &ft)) continue;
        ULARGE_INTEGER t;
        t.LowPart = ft.dwLowDateTime;
        t.HighPart = ft.dwHighDateTime;
        PromptStats::Reading r;
        r.email = email;
        r.at = static_cast<long long>(t.QuadPart / 10000000ULL) - 11644473600LL;
        r.pct[0] = five;
        r.pct[1] = week;
        r.reset[0] = r.reset[1] = 1;  // unknown, treated as one window
        out.push_back(r);
    }
    std::fclose(f);
    return out;
}

void PromptStats::Start(const std::wstring& projects_dir) {
    dir_ = projects_dir;
    const std::wstring saved = AppDataFile(L"calibration.txt");
    if (GetFileAttributesW(saved.c_str()) == INVALID_FILE_ATTRIBUTES) {
        for (const Reading& r : ReadingsFromLog(AppDataFile(L"app.log")))
            if (r.at > NowUnix() - kUnitHistory + kMaxGap) pending_.push_back(r);
    }
    calibration_.Load(saved);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE thread = CreateThread(nullptr, 0, &PromptStats::ThreadMain, this, 0, nullptr);
    if (thread) {
        SetThreadPriority(thread, THREAD_PRIORITY_LOWEST);
        CloseHandle(thread);
    }
}

void PromptStats::OnReading(const std::string& email, long long at, double five_pct, long long five_reset,
                            double week_pct, long long week_reset) {
    if (email.empty() || at <= 0) return;
    Reading r;
    r.email = email;
    r.at = at;
    r.pct[0] = five_pct;
    r.pct[1] = week_pct;
    r.reset[0] = five_reset;
    r.reset[1] = week_reset;
    AcquireSRWLockExclusive(&lock_);
    if (pending_.size() < 100) pending_.push_back(r);
    ReleaseSRWLockExclusive(&lock_);
    if (wake_) SetEvent(wake_);
}

PromptCosts PromptStats::CostsFor(const std::string& email) {
    PromptCosts out;
    double units[kEffortCount];
    AcquireSRWLockShared(&lock_);
    stats_.MedianPromptUnits(units, NowUnix());
    for (int w = 0; w < 2; ++w) {
        double k = 0;
        if (!calibration_.PercentPerUnit(email, w, k)) continue;
        for (int e = 0; e < kEffortCount; ++e) out.percent[w][e] = units[e] * k;
    }
    ReleaseSRWLockShared(&lock_);
    return out;
}

PromptStats::Pile PromptStats::Live() {
    const double kQuiet = 30;   // seconds without a token that end a stretch of work
    const double kSettle = 120; // tokens older than this are folded into the base (replies landing
                                // later than that would re-spread over time already counted)
    Pile out;
    AcquireSRWLockExclusive(&lock_);
    const double now = NowPrecise();
    out.rate = stats_.TokensPerSecond(now, 3);
    if (out.rate >= 0.5) {
        if (!pile_start_) {
            pile_start_ = pile_anchor_ = now - 3;  // the rate's own window: those tokens count too
            pile_base_ = 0;
            ++pile_id_;
        }
        last_flow_ = now;
    } else if (pile_start_ && now - last_flow_ > kQuiet) {
        pile_start_ = 0;
    }
    if (pile_start_) {
        if (now - pile_anchor_ > 2 * kSettle) {
            double to = now - kSettle;
            pile_base_ += stats_.TokensPerSecond(to, to - pile_anchor_) * (to - pile_anchor_);
            pile_anchor_ = to;
        }
        out.count = pile_base_ + stats_.TokensPerSecond(now, now - pile_anchor_) * (now - pile_anchor_);
        out.working = true;
    }
    out.pile = pile_id_;
    ReleaseSRWLockExclusive(&lock_);
    return out;
}

double PromptStats::TokenRate() {
    AcquireSRWLockShared(&lock_);
    double rate = stats_.TokensPerSecond(NowPrecise(), 3);
    ReleaseSRWLockShared(&lock_);
    return rate;
}

std::vector<double> PromptStats::TokenSeries(int seconds) {
    std::vector<double> out(seconds, 0.0);
    AcquireSRWLockShared(&lock_);
    const double now = NowPrecise();
    for (int i = 0; i < seconds; ++i) out[i] = stats_.TokensPerSecond(now - (seconds - 1 - i), 3);
    ReleaseSRWLockShared(&lock_);
    return out;
}

std::vector<double> PromptStats::TokenHistory(int minutes) {
    AcquireSRWLockShared(&lock_);
    std::vector<double> out = stats_.TokensPerSecondByMinute(NowUnix(), minutes);
    ReleaseSRWLockShared(&lock_);
    return out;
}

bool PromptStats::PercentSince(const std::string& email, long long since, double out[2]) {
    AcquireSRWLockShared(&lock_);
    // From the reading's own second: a reply written just after it in that second is new work.
    double units = stats_.UnitsBetween(since - 1, NowUnix() + 60);
    bool known = false;
    for (int w = 0; w < 2; ++w) {
        double k = 0;
        out[w] = calibration_.PercentPerUnit(email, w, k) ? units * k : 0;
        known = known || k > 0;
    }
    ReleaseSRWLockShared(&lock_);
    return known;
}

bool PromptStats::TokensLeft(const std::string& email, double five_pct, long long since, double& out) {
    AcquireSRWLockShared(&lock_);
    double k = 0;
    bool known = calibration_.PercentPerUnit(email, 0, k) && k > 0;
    double units = known ? stats_.UnitsBetween(since - 1, NowUnix() + 60) : 0;
    ReleaseSRWLockShared(&lock_);
    if (!known) return false;
    const double left_percent = std::max(0.0, 100.0 - five_pct - units * k);
    out = left_percent / k / 5.0;  // units -> written tokens (a written token is 5 units)
    return true;
}

DWORD WINAPI PromptStats::ThreadMain(LPVOID param) {
    static_cast<PromptStats*>(param)->Loop();
    return 0;
}

void PromptStats::Loop() {
    // Rescan as soon as Claude Code writes to a transcript (and every 20 s regardless).
    HANDLE change = FindFirstChangeNotificationW(dir_.c_str(), TRUE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
    for (;;) {
        Scan();
        Calibrate();
        HANDLE handles[2] = {wake_, change};
        DWORD r = WaitForMultipleObjects(change != INVALID_HANDLE_VALUE ? 2 : 1, handles, FALSE, 20000);
        if (r == WAIT_OBJECT_0 + 1) {
            FindNextChangeNotification(change);
            Sleep(50);  // let a burst of writes settle (short: the token speed is shown live)
        }
    }
}

void PromptStats::Scan() {
    long long started = NowUnix();
    std::vector<std::wstring> paths;
    WIN32_FIND_DATAW dir_data;
    HANDLE dirs = FindFirstFileW((dir_ + L"\\*").c_str(), &dir_data);
    if (dirs != INVALID_HANDLE_VALUE) {
        do {
            if (!(dir_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || dir_data.cFileName[0] == L'.') continue;
            std::wstring sub = dir_ + L"\\" + dir_data.cFileName;
            WIN32_FIND_DATAW file_data;
            HANDLE files = FindFirstFileW((sub + L"\\*.jsonl").c_str(), &file_data);
            if (files == INVALID_HANDLE_VALUE) continue;
            do {
                std::wstring path = sub + L"\\" + file_data.cFileName;
                if (!files_.count(path)) {
                    // First sight of a file nobody has written to for two weeks: skip its past.
                    ULARGE_INTEGER t;
                    t.LowPart = file_data.ftLastWriteTime.dwLowDateTime;
                    t.HighPart = file_data.ftLastWriteTime.dwHighDateTime;
                    long long written = static_cast<long long>(t.QuadPart / 10000000ULL) - 11644473600LL;
                    FileState& fresh = files_[path];
                    if (written < started - kPromptHistory)
                        fresh.offset = (static_cast<unsigned long long>(file_data.nFileSizeHigh) << 32) | file_data.nFileSizeLow;
                }
                paths.push_back(path);
            } while (FindNextFileW(files, &file_data));
            FindClose(files);
        } while (FindNextFileW(dirs, &dir_data));
        FindClose(dirs);
    }
    for (const std::wstring& path : paths) ScanFile(path, files_[path]);
    AcquireSRWLockExclusive(&lock_);
    stats_.Prune(started);
    scanned_through_ = started - 2;  // a line written as the scan began may have been missed
    ReleaseSRWLockExclusive(&lock_);
}

void PromptStats::ScanFile(const std::wstring& path, FileState& state) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return;
    }
    unsigned long long end = static_cast<unsigned long long>(size.QuadPart);
    if (end < state.offset) state = FileState();  // rewritten: start over
    if (end == state.offset) {
        CloseHandle(h);
        return;
    }
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(state.offset);
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);

    std::string pending;
    std::vector<char> buf(1 << 20);
    unsigned long long consumed = state.offset;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) || got == 0) break;
        pending.append(buf.data(), got);
        size_t start = 0;
        AcquireSRWLockExclusive(&lock_);
        for (size_t nl; (nl = pending.find('\n', start)) != std::string::npos; start = nl + 1)
            stats_.AddLine(state.ctx, pending.substr(start, nl - start));
        ReleaseSRWLockExclusive(&lock_);
        consumed += start;
        pending.erase(0, start);
        if (pending.size() > (64u << 20)) {  // a runaway line: skip it
            consumed += pending.size();
            pending.clear();
        }
    }
    CloseHandle(h);
    state.offset = consumed;  // an unfinished last line is read again next time
}

void PromptStats::Calibrate() {
    AcquireSRWLockExclusive(&lock_);
    std::sort(pending_.begin(), pending_.end(), [](const Reading& a, const Reading& b) { return a.at < b.at; });
    bool changed = false;
    size_t done = 0;
    for (; done < pending_.size() && pending_[done].at <= scanned_through_; ++done) {
        const Reading& r = pending_[done];
        auto prev = last_reading_.find(r.email);
        if (prev != last_reading_.end() && r.at > prev->second.at && r.at - prev->second.at <= kMaxGap) {
            const Reading& p = prev->second;
            double units = stats_.UnitsBetween(p.at, r.at);
            for (int w = 0; w < 2; ++w) {
                // Same window, and usage didn't drop (a drop means it reset in between).
                if (r.reset[w] <= 0 || p.reset[w] <= 0 || std::llabs(r.reset[w] - p.reset[w]) > 600 || r.pct[w] < p.pct[w])
                    continue;
                // A jump no amount of Claude Code work explains (a new window that kept its reset
                // time, a burst elsewhere) would skew the rate for hours: leave it out.
                if (r.pct[w] - p.pct[w] > kMaxStep) continue;
                calibration_.Observe(r.email, w, r.pct[w] - p.pct[w], units);
                changed = true;
                if (TestApiPort())  // end-to-end tests: show every sample
                    AppendAppLog(L"calibration: " + Utf8ToWide(r.email) + L" w" + std::to_wstring(w) + L" +" +
                                 std::to_wstring(r.pct[w] - p.pct[w]) + L"% " + std::to_wstring(units) + L" units (" +
                                 std::to_wstring(p.at) + L"," + std::to_wstring(r.at) + L"]");
            }
        }
        last_reading_[r.email] = r;
    }
    pending_.erase(pending_.begin(), pending_.begin() + done);
    if (changed) calibration_.Save(AppDataFile(L"calibration.txt"));
    ReleaseSRWLockExclusive(&lock_);
}
