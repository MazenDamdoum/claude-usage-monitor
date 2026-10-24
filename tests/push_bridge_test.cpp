// Tests for reset detection and the push bridge.
//
//   push_bridge_test.exe                         run the unit tests
//   push_bridge_test.exe --post usage|event      send one sample payload to the push
//                                                server named by bridge.json (or
//                                                CUM_PUSH_BRIDGE_FILE); prints the status

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../src/Accounts.h"
#include "../src/PushBridge.h"
#include "../src/ResetDetector.h"
#include "../src/UsageView.h"

using namespace claude_usage;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                    \
    } while (0)

static FetchResult Data(long long five_reset, long long week_reset, double five_pct = 50, double week_pct = 20) {
    FetchResult r;
    r.status = FetchStatus::Ok;
    r.five_hour = UsageWindow{true, five_pct, five_reset};
    r.seven_day = UsageWindow{true, week_pct, week_reset};
    return r;
}

static void TestResetDetection() {
    std::printf("reset detection\n");
    const long long R5 = 1'000'000, R7 = 5'000'000;
    LimitReset ev[kResetKindCount];
    ResetState st;

    // First fetch after startup only records state.
    CHECK(DetectResets(st, Data(R5, R7), R5 - 3600, false, ev) == 0);
    CHECK(st.known_resets_at[kResetFiveHour] == R5);

    // Same window, before the reset: nothing.
    CHECK(DetectResets(st, Data(R5, R7), R5 - 60, true, ev) == 0);

    // Timestamp changes BEFORE the reset time: not a reset.
    CHECK(DetectResets(st, Data(R5 + 30, R7), R5 - 30, true, ev) == 0);
    CHECK(st.known_resets_at[kResetFiveHour] == R5 + 30);
    st.known_resets_at[kResetFiveHour] = R5;

    // Reset time passed but the API still reports the old window: not confirmed yet.
    CHECK(DetectResets(st, Data(R5, R7), R5 + 10, true, ev) == 0);

    // Fresh data shows a new window after the reset time: exactly one 5-hour event.
    int n = DetectResets(st, Data(R5 + 18000, R7, 3), R5 + 40, true, ev);
    CHECK(n == 1);
    CHECK(n == 1 && ev[0].kind == kResetFiveHour && ev[0].reset_at == R5);

    // Polling again with the same data: no duplicate.
    CHECK(DetectResets(st, Data(R5 + 18000, R7, 4), R5 + 100, true, ev) == 0);

    // Restart (state reloaded from disk, first fetch unprimed): no duplicate.
    ResetState reloaded = st;
    CHECK(DetectResets(reloaded, Data(R5 + 18000, R7, 5), R5 + 200, false, ev) == 0);

    // Weekly reset on its own, reported with no running window afterwards (resets_at 0).
    n = DetectResets(st, Data(R5 + 18000, 0, 6, 0), R7 + 5, true, ev);
    CHECK(n == 1 && ev[0].kind == kResetWeekly && ev[0].reset_at == R7);

    // Both at once.
    ResetState both;
    DetectResets(both, Data(R5, R7), R5 - 10, false, ev);
    n = DetectResets(both, Data(R5 + 18000, R7 + 604800), R7 + 1, true, ev);
    CHECK(n == 2);

    // A reset that happened while the app was off never notifies on startup.
    ResetState offline;
    offline.known_resets_at[kResetFiveHour] = R5;
    CHECK(DetectResets(offline, Data(R5 + 18000, R7), R5 + 7200, false, ev) == 0);

    // Already announced (persisted notified_resets_at): no repeat even if state lags.
    ResetState announced;
    announced.known_resets_at[kResetFiveHour] = R5;
    announced.notified_resets_at[kResetFiveHour] = R5;
    CHECK(DetectResets(announced, Data(R5 + 18000, R7), R5 + 60, true, ev) == 0);

    // Limit missing from the response: ignored, state kept.
    ResetState missing;
    missing.known_resets_at[kResetFiveHour] = R5;
    FetchResult partial = Data(0, R7);
    partial.five_hour.present = false;
    CHECK(DetectResets(missing, partial, R5 + 60, true, ev) == 0);
    CHECK(missing.known_resets_at[kResetFiveHour] == R5);
}

static void TestJson() {
    std::printf("json payloads\n");
    UsageSnapshot s;
    s.has_data = true;
    s.fetched_at = 1234;
    s.data = Data(2000, 9000, 66.5, 12.25);
    s.data.subscription_type = "max\"<script>";
    std::string usage = BuildUsageJson(s);
    CHECK(usage.find("\"fetched_at\":1234") != std::string::npos);
    CHECK(usage.find("\"plan\":\"maxscript\"") != std::string::npos);  // unsafe characters stripped
    CHECK(usage.find("\"five_hour\":{\"utilization_x100\":6650,\"resets_at\":2000}") != std::string::npos);
    CHECK(usage.find("\"seven_day_sonnet\":null") != std::string::npos);

    std::string notice = BuildNoticeJson("switch:1", L"Claude account switched", L"bob used up its \"5-hour\" limit. Now using alice.");
    CHECK(notice == "{\"id\":\"switch:1\",\"title\":\"Claude account switched\",\"body\":\"bob used up its \\\"5-hour\\\" limit. Now using alice.\"}");

    std::string event = BuildEventJson(LimitReset{kResetWeekly, 777});
    CHECK(event == "{\"id\":\"weekly_reset:777\",\"kind\":\"weekly_reset\",\"reset_at\":777}");
}

static void TestCswapParse() {
    std::printf("cswap list parsing\n");
    const char* json = R"({"schemaVersion":1,"activeAccountNumber":2,"accounts":[
      {"number":2,"email":"john.doe@gmail.com","alias":"Personal","active":true,"usageStatus":"ok",
       "usage":{"fiveHour":{"pct":76.0,"resetsAt":"2026-06-22T23:29:59Z"},
                "scoped":[{"name":"Fable","pct":59.0,"resetsAt":"2026-06-27T00:00:00Z"}]}},
      {"number":3,"email":"john.doe@company.com","alias":"Work","active":false,"usageStatus":"unavailable",
       "usage":null,"lastGoodUsage":{"fiveHour":{"pct":96.0},"sevenDay":{"pct":40.0,"resetsAt":"2026-06-26T21:00:00Z"}}},
      {"number":4,"email":"averyveryverylongname@x.com","active":false,"usageStatus":"relogin_required","usage":null}
    ]})";
    std::vector<AccountInfo> a;
    CHECK(ParseCswapList(json, a));
    CHECK(a.size() == 3);
    if (a.size() != 3) return;
    CHECK(a[0].number == 2 && a[0].active && a[0].alias == L"Personal" && a[0].ShortName() == L"Personal");
    CHECK(a[0].five_hour.present && a[0].five_hour.percent == 76.0 && a[0].five_hour.resets_at == 1782170999);
    CHECK(!a[0].seven_day.present);
    CHECK(a[0].scoped.size() == 1 && a[0].scoped[0].first == L"Fable" && a[0].scoped[0].second.percent == 59.0);
    CHECK(a[1].stale && a[1].five_hour.percent == 96.0 && a[1].seven_day.percent == 40.0);
    CHECK(a[2].status == L"relogin_required" && !a[2].five_hour.present);
    CHECK(a[2].ShortName() == L"averyveryve\u2026");
    CHECK(ParseCswapList("{\"schemaVersion\":1,\"activeAccountNumber\":null,\"accounts\":[]}", a) && a.empty());
    CHECK(!ParseCswapList("not json", a));
}

static void TestAccountsIndependent() {
    std::printf("accounts stay independent\n");
    AccountsSnapshot snap;
    snap.available = true;
    AccountInfo alice, bob;
    alice.number = 1; alice.email = L"alicesmith@example.com"; alice.alias = L"alice"; alice.active = true;  // cswap's view
    alice.five_hour = AccountWindow{true, 56, 1000}; alice.seven_day = AccountWindow{true, 13, 2000};
    bob.number = 2; bob.email = L"Bobtest7@example.com"; bob.alias = L"bob";
    bob.five_hour = AccountWindow{true, 90, 3000}; bob.seven_day = AccountWindow{true, 35, 4000};
    snap.accounts = {alice, bob};

    // Claude Code is really signed in to bob (e.g. right after /login).
    UsageSnapshot live;
    live.has_data = true;
    live.data = Data(5000, 6000, 100, 40);
    live.data.account_email = "bobtest7@example.com";
    std::vector<AccountInfo> m = MergeAccounts(snap, live);
    CHECK(m.size() == 2);
    if (m.size() != 2) return;
    // Accounts keep their places (slot order); the real login is marked, not moved.
    CHECK(m[0].alias == L"alice" && !m[0].active);
    CHECK(m[0].five_hour.percent == 56 && m[0].seven_day.percent == 13);  // alice keeps its own numbers
    CHECK(m[1].alias == L"bob" && m[1].active);                     // the real login
    CHECK(m[1].five_hour.percent == 100 && m[1].seven_day.percent == 40);
    CHECK(!LiveLoginIsUnknown(snap, live));

    // Unknown login identity: nothing is overwritten.
    live.data.account_email.clear();
    m = MergeAccounts(snap, live);
    CHECK(m[0].alias == L"alice" && m[0].five_hour.percent == 56 && m[1].five_hour.percent == 90);

    // A login claude-swap doesn't have yet is reported (so it can be saved), never merged.
    live.data.account_email = "dave@example.com";
    m = MergeAccounts(snap, live);
    CHECK(LiveLoginIsUnknown(snap, live));
    CHECK(m[0].five_hour.percent == 56 && m[1].five_hour.percent == 90);
}

static void TestOwners() {
    std::printf("account owners\n");
    AccountInfo alice;
    alice.email = L"Alicesmyth@Example.com";
    alice.alias = L"alice \"h\"";
    alice.active = true;
    alice.five_hour = AccountWindow{true, 52.345, 1800000000};
    AccountInfo nokey;  // no email: never sent
    std::string json = BuildAccountsJson(L"Bob", {alice, nokey});
    CHECK(json ==
          "{\"user_name\":\"Bob\",\"accounts\":[{\"email\":\"alicesmyth@example.com\",\"name\":\"alice \\\"h\\\"\","
          "\"active\":true,\"five_hour\":{\"utilization_x100\":5235,\"resets_at\":1800000000},\"seven_day\":null}]}");

    std::vector<OwnerPolicy> p;
    CHECK(ParsePolicies("{\"policies\":[{\"email\":\"A@b.com\",\"cap_five_hour\":70,\"cap_weekly\":null,\"allow\":true},"
                        "{\"email\":\"c@d.com\",\"cap_five_hour\":null,\"cap_weekly\":500,\"allow\":false},{\"nope\":1}]}",
                        p));
    CHECK(p.size() == 2);
    CHECK(p.size() == 2 && p[0].email == "a@b.com" && p[0].cap_five_hour == 70 && p[0].cap_weekly == 0 && p[0].allow);
    CHECK(p.size() == 2 && p[1].cap_weekly == 0 && !p[1].allow);
    std::vector<OwnerPolicy> q;
    CHECK(ParsePolicies("{\"policies\":[{\"email\":\"c@d.com\",\"allow\":false},{\"email\":\"a@b.com\",\"cap_five_hour\":70}]}", q));
    CHECK(q == p);  // same rules in another order: no change
    // A freed reserve: the cap is lifted and the green starts at the old cap; it counts as news.
    std::vector<OwnerPolicy> freed;
    CHECK(ParsePolicies("{\"policies\":[{\"email\":\"a@b.com\",\"cap_five_hour\":null,\"reserve_from\":60}]}", freed));
    CHECK(freed.size() == 1 && freed[0].cap_five_hour == 0 && freed[0].reserve_from == 60);
    std::vector<OwnerPolicy> autoCap;
    CHECK(ParsePolicies("{\"policies\":[{\"email\":\"a@b.com\",\"cap_five_hour\":25,\"auto_five_hour\":true}]}", autoCap));
    CHECK(autoCap.size() == 1 && autoCap[0].auto_five_hour && autoCap[0].cap_five_hour == 25);
    CHECK(ParsePolicies("{\"policies\":[{\"email\":\"a@b.com\",\"owner_cap_five_hour\":40,\"owner_auto_five_hour\":true}]}", autoCap));
    CHECK(autoCap.size() == 1 && autoCap[0].owner_auto_five_hour && !autoCap[0].auto_five_hour);
    CHECK(ParsePolicies("{\"policies\":[{\"email\":\"a@b.com\",\"no_auto\":true}]}", autoCap));
    CHECK(autoCap.size() == 1 && autoCap[0].no_auto);
    CHECK(ParsePolicies("{\"policies\":[]}", p) && p.empty());
    CHECK(!ParsePolicies("{\"ok\":true}", p));
    CHECK(!ParsePolicies("<html>", p));
}

static std::string Reply(const char* id, const char* at, const char* effort, const char* model, int output) {
    char buf[600];
    std::snprintf(buf, sizeof(buf),
                  "{\"parentUuid\":\"x\",\"type\":\"assistant\",\"effort\":\"%s\",\"timestamp\":\"%s\","
                  "\"message\":{\"id\":\"%s\",\"model\":\"%s\",\"content\":[{\"type\":\"text\",\"text\":\"\\\"type\\\":\\\"user\\\"\"}],"
                  "\"usage\":{\"input_tokens\":100,\"cache_creation_input_tokens\":1000,\"cache_read_input_tokens\":10000,"
                  "\"output_tokens\":%d,\"cache_creation\":{\"ephemeral_1h_input_tokens\":1000}}}}",
                  effort, at, id, model, output);
    return buf;
}

static std::string Prompt(const char* id) {
    return std::string("{\"type\":\"user\",\"promptId\":\"") + id + "\",\"message\":{\"content\":\"hi\"}}";
}

static void TestPrompts() {
    std::printf("prompts left\n");
    // 100 input + 1000 x 2 (1-hour cache write) + 10000 x 0.1 + output x 5
    CHECK(TokenUnits("claude-opus-5-5", 100, 0, 1000, 10000, 200) == 4100);
    CHECK(TokenUnits("claude-haiku-5-5", 100, 0, 1000, 10000, 200) == 820);
    CHECK(TokenUnits("<synthetic>", 100, 0, 0, 0, 0) == 0);

    TranscriptStats stats;
    TranscriptStats::FileContext ctx;
    const long long base = ParseIso8601("2026-10-15T10:00:00Z");
    CHECK(ParseIso8601("2026-10-15T09:59:59.943648+00:00") == base);  // Anthropic's reset times: on the hour
    CHECK(ParseIso8601("2026-10-15T10:00:00.2Z") == base);
    // Something new in the usage response (a codenamed field filling in) is found; known ones aren't.
    std::vector<UsageExtra> extras = FindUsageExtras(
        "{\"five_hour\":{\"utilization\":1},\"iguana_necktie\":{\"limit_dollars\":100},\"amber_ladder\":null,"
        "\"harbor_lantern\":{\"count\":1,\"expires_at\":\"2026-10-15T10:00:00Z\"},\"limits\":[]}");
    CHECK(extras.size() == 1 && extras[0].key == "harbor_lantern" && extras[0].ends_at == base);
    double med[kEffortCount];
    stats.MedianPromptUnits(med, base);
    CHECK(med[0] == 0 && med[1] == 0 && med[2] == 0);
    // Three medium Opus prompts of 4100 units each; p1 later grows by a 5100-unit reply.
    const char* ids[] = {"p1", "p2", "p3"};
    for (int i = 0; i < 3; ++i) {
        stats.AddLine(ctx, Prompt(ids[i]));
        std::string reply = Reply((std::string("m") + ids[i]).c_str(), "2026-10-15T10:01:30.500Z", "medium", "claude-opus-5-5", 200);
        stats.AddLine(ctx, reply);
        stats.AddLine(ctx, reply);  // the same reply's next content block: counted once
    }
    stats.AddLine(ctx, "{\"type\":\"user\",\"promptId\":\"p1\",\"message\":{\"content\":[{\"type\":\"tool_result\"}]}}");
    stats.AddLine(ctx, Reply("m4", "2026-10-15T10:03:00Z", "medium", "claude-opus-5-5", 400));  // p1 grows to 9200
    stats.AddLine(ctx, "not json");
    stats.MedianPromptUnits(med, base + 600);
    CHECK(med[kEffortMedium] == 4100);
    CHECK(med[kEffortLow] == 2050 && med[kEffortHigh] == 8200);  // filled in from medium
    CHECK(stats.UnitsBetween(base, base + 120) == 3 * 4100);
    CHECK(stats.UnitsBetween(base + 120, base + 240) == 5100);
    CHECK(stats.UnitsBetween(base + 240, base + 600) == 0);
    stats.Prune(base + 30 * 86400);
    stats.MedianPromptUnits(med, base + 30 * 86400);
    CHECK(med[kEffortMedium] == 0);

    // Live token speed: a reply in flight counts at the default speed, then its real 1000 output
    // tokens land spread over the 10 s it took; the session's speed (100/s) is learnt for the next.
    TranscriptStats live;
    TranscriptStats::FileContext lctx;
    const double t0 = static_cast<double>(base);
    CHECK(live.TokensPerSecond(t0 + 5, 3) == 0);
    live.AddLine(lctx, "{\"type\":\"api-request\",\"timestamp\":\"2026-10-15T10:00:00.000Z\"}");
    CHECK(std::fabs(live.TokensPerSecond(t0 + 5, 3) - 42) < 0.01);  // estimated (on the low side) while being written
    live.AddLine(lctx, Reply("r1", "2026-10-15T10:00:10.000Z", "medium", "claude-opus-5-5", 1000));
    CHECK(std::fabs(live.TokensPerSecond(t0 + 10, 3) - 100) < 0.01);  // the real count, spread
    CHECK(std::fabs(live.TokensPerSecond(t0 + 11.5, 3) - 50) < 0.01);  // and fading once done
    CHECK(live.TokensPerSecond(t0 + 14, 3) == 0);
    live.AddLine(lctx, "{\"type\":\"api-request\",\"timestamp\":\"2026-10-15T10:00:20.000Z\"}");
    CHECK(std::fabs(live.TokensPerSecond(t0 + 25, 3) - 70) < 0.01);  // from this session's own speed
    live.AddLine(lctx, "{\"type\":\"user\",\"message\":{\"content\":\"[Request interrupted by user]\"}}");
    CHECK(live.TokensPerSecond(t0 + 25, 3) == 0);  // interrupted: no longer counted

    UsageCalibration cal;
    double k = 0;
    CHECK(!cal.PercentPerUnit("a@x.com", 0, k));
    cal.Observe("a@x.com", 0, 2, 100000);
    CHECK(!cal.PercentPerUnit("a@x.com", 0, k));  // not enough yet
    cal.Observe("a@x.com", 0, 2, 100000);
    CHECK(cal.PercentPerUnit("a@x.com", 0, k) && k > 1.9e-5 && k < 2.1e-5);
    CHECK(cal.PercentPerUnit("new@x.com", 0, k));  // borrows the other accounts' rate
    CHECK(!cal.PercentPerUnit("a@x.com", 1, k));
    cal.Observe("a@x.com", 0, -5, 100);  // a reset in between: ignored
    CHECK(cal.PercentPerUnit("a@x.com", 0, k) && k > 1.9e-5);

    PromptCosts c;
    c.percent[0][0] = 1.234;
    c.percent[0][1] = 2.5;
    c.percent[0][2] = 250;
    AccountInfo a;
    a.email = L"a@x.com";
    std::string json = BuildAccountsJson(L"N", {a}, {c});
    CHECK(json.find("\"prompt_cost\":{\"five_hour\":{\"low\":123,\"medium\":250,\"high\":10000},\"seven_day\":null}") !=
          std::string::npos);
    CHECK(BuildAccountsJson(L"N", {a}, {}, "", 100, 240).find(",\"usage_check\":{\"at\":100,\"next\":240}}") != std::string::npos);
}

static void TestTypedInput() {
    std::printf("typed input (claude-swap's \"Are you sure?\")\n");
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::string out;
    int code = RunHidden(std::wstring(sys) + L"\\findstr.exe", L"\"^\"", out, 5000, false, "y\n");
    CHECK(code == 0);
    CHECK(out.rfind("y", 0) == 0);
    RunHidden(std::wstring(sys) + L"\\findstr.exe", L"\"^\"", out, 5000);
    CHECK(out.empty());  // nothing typed: reads nothing
}

static void TestUsageShare() {
    std::printf("reading handed to claude-swap\n");
    AccountInfo a;
    a.email = L"alice@example.com";
    a.org_uuid = L"org-1";
    a.five_hour = AccountWindow{true, 52.5, 1792123200};  // 2026-10-16T04:00:00Z
    a.seven_day = AccountWindow{true, 35, 0};
    a.scoped.emplace_back(L"Fable", AccountWindow{true, 59, 1792123200});
    std::string json = BuildUsageShareJson(1, a, 12);
    CHECK(json ==
          "{\"schemaVersion\":1,\"accounts\":[{\"email\":\"alice@example.com\",\"organizationUuid\":\"org-1\","
          "\"usageStatus\":\"ok\",\"usageAgeSeconds\":12,\"usage\":{\"fiveHour\":{\"pct\":52.50,"
          "\"resetsAt\":\"2026-10-16T04:00:00+00:00\"},\"sevenDay\":{\"pct\":35.00},\"scoped\":[{\"pct\":59.00,"
          "\"resetsAt\":\"2026-10-16T04:00:00+00:00\",\"name\":\"Fable\"}]}}]}");
    std::vector<AccountInfo> parsed;
    int schema = 0;
    CHECK(ParseCswapList("{\"schemaVersion\":3,\"accounts\":[{\"number\":1,\"email\":\"x@y.z\",\"organizationUuid\":\"o\"}]}", parsed, &schema));
    CHECK(schema == 3 && parsed.size() == 1 && parsed[0].org_uuid == L"o");
}

static void TestServerOffline() {
    std::printf("server offline\n");
    // Find a port nobody listens on by asking Windows for one and closing it again.
    ULONGLONG start = GetTickCount64();
    int status = PostToPushServer(1, std::string(64, 'a'), "/internal/usage", "{}");
    ULONGLONG ms = GetTickCount64() - start;
    CHECK(status == 0);
    CHECK(ms < 5000);
    std::printf("  unreachable server answered in %llu ms\n", ms);
}

static int Post(const char* what, bool cloud, long long five_reset) {
    int port = 0;
    std::wstring url;
    std::string token;
    if (cloud ? !ReadCloudConfig(url, token) : !ReadPushBridgeConfig(port, token)) {
        std::printf("no valid %s config\n", cloud ? "cloud" : "bridge");
        return 2;
    }
    std::string body;
    const char* path;
    if (!std::strcmp(what, "event")) {
        path = cloud ? "/ingest/event" : "/internal/event";
        body = BuildEventJson(LimitReset{kResetFiveHour, NowUnix() - 5});
    } else {
        path = cloud ? "/ingest/usage" : "/internal/usage";
        UsageSnapshot s;
        s.has_data = true;
        s.fetched_at = NowUnix();
        s.data = Data(five_reset > 0 ? five_reset : NowUnix() + 3600, NowUnix() + 86400, 42, 17);
        s.data.subscription_type = "pro";
        body = BuildUsageJson(s);
    }
    int status = cloud ? PostJsonToUrl(url + Utf8ToWide(path), token, body) : PostToPushServer(port, token, path, body);
    std::printf("%d\n", status);
    return status >= 200 && status < 300 ? 0 : 1;
}

// --prompts <projects dir>: what the estimator learns from real transcripts (medians only).
static int ShowPrompts(const char* dir) {
    ULONGLONG start = GetTickCount64();
    TranscriptStats stats;
    std::string root(dir);
    WIN32_FIND_DATAA d;
    HANDLE dirs = FindFirstFileA((root + "\\*").c_str(), &d);
    size_t bytes = 0;
    while (dirs != INVALID_HANDLE_VALUE) {
        if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && d.cFileName[0] != '.') {
            std::string sub = root + "\\" + d.cFileName;
            WIN32_FIND_DATAA f;
            HANDLE files = FindFirstFileA((sub + "\\*.jsonl").c_str(), &f);
            while (files != INVALID_HANDLE_VALUE) {
                FILE* in = std::fopen((sub + "\\" + f.cFileName).c_str(), "rb");
                TranscriptStats::FileContext ctx;
                std::string line;
                for (int c; in && (c = std::fgetc(in)) != EOF;) {
                    ++bytes;
                    if (c != '\n') { line += static_cast<char>(c); continue; }
                    stats.AddLine(ctx, line);
                    line.clear();
                }
                if (in) std::fclose(in);
                if (!FindNextFileA(files, &f)) { FindClose(files); break; }
            }
        }
        if (!FindNextFileA(dirs, &d)) { FindClose(dirs); break; }
    }
    double med[kEffortCount];
    stats.MedianPromptUnits(med, NowUnix());
    std::printf("%zu MB in %llu ms; median units low %.0f, medium %.0f, high %.0f; last hour %.0f units\n", bytes >> 20,
                GetTickCount64() - start, med[0], med[1], med[2], stats.UnitsBetween(NowUnix() - 3600, NowUnix()));

    // The calibration the app would seed from app.log, and the resulting estimate.
    wchar_t appdata[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    std::vector<PromptStats::Reading> log = ReadingsFromLog(std::wstring(appdata) + L"\\ClaudeUsageMonitor\\app.log");
    UsageCalibration cal;
    std::map<std::string, PromptStats::Reading> last;
    int pairs = 0;
    for (const PromptStats::Reading& r : log) {
        auto p = last.find(r.email);
        if (p != last.end() && r.at - p->second.at > 0 && r.at - p->second.at <= 1800) {
            double units = stats.UnitsBetween(p->second.at, r.at);
            for (int w = 0; w < 2; ++w)
                if (r.pct[w] >= p->second.pct[w]) cal.Observe(r.email, w, r.pct[w] - p->second.pct[w], units);
            ++pairs;
        }
        last[r.email] = r;
    }
    std::printf("%zu logged readings, %d pairs\n", log.size(), pairs);
    for (const auto& kv : last) {
        for (int w = 0; w < 2; ++w) {
            double k = 0;
            if (!cal.PercentPerUnit(kv.first, w, k)) {
                std::printf("  %s %s: learning\n", kv.first.substr(0, 4).c_str(), w ? "7d" : "5h");
                continue;
            }
            std::printf("  %s %s: %.0f%% used; one prompt = %.2f%% / %.2f%% / %.2f%% -> left %.0f / %.0f / %.0f\n",
                        kv.first.substr(0, 4).c_str(), w ? "7d" : "5h", kv.second.pct[w], med[0] * k, med[1] * k,
                        med[2] * k, (100 - kv.second.pct[w]) / (med[0] * k), (100 - kv.second.pct[w]) / (med[1] * k),
                        (100 - kv.second.pct[w]) / (med[2] * k));
        }
    }
    return 0;
}

// --live <projects dir> <seconds>: print the live token pile twice a second (to watch it work).
static int ShowLive(const char* dir, int seconds) {
    static PromptStats stats;
    stats.Start(Utf8ToWide(dir));
    for (int i = 0; i < seconds * 2; ++i) {
        Sleep(500);
        PromptStats::Pile p = stats.Live();
        std::printf("%5.1fs  rate %6.1f  count %8.1f  working %d  pile %lld\n", i / 2.0 + 0.5, p.rate, p.count, p.working,
                    p.pile);
        std::fflush(stdout);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--prompts")) return ShowPrompts(argv[2]);
    if (argc == 4 && !std::strcmp(argv[1], "--live")) return ShowLive(argv[2], std::atoi(argv[3]));
    if (argc == 3 && !std::strcmp(argv[1], "--post")) return Post(argv[2], false, 0);
    // --cloud usage|event [five_hour_reset_in_seconds]
    if (argc >= 3 && !std::strcmp(argv[1], "--cloud"))
        return Post(argv[2], true, argc > 3 ? NowUnix() + std::atoll(argv[3]) : 0);
    TestResetDetection();
    TestJson();
    TestCswapParse();
    TestAccountsIndependent();
    TestOwners();
    TestPrompts();
    TestTypedInput();
    TestUsageShare();
    TestServerOffline();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
