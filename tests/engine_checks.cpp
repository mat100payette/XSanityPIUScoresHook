#include "support.h"
#include "engine.h"

namespace piu::test {
struct FakeApi : Api {
    std::vector<Chart> charts{{"chart-id", "Test Song", "Single", 10}};
    std::vector<Score> records;
    std::vector<Play> plays;
    int catalog_calls = 0, score_calls = 0, upload_error = -1, fail_scores_at = -1;
    bool offline = false, update_best = true;

    std::vector<Chart> catalog(const std::string&, const std::string&) override {
        ++catalog_calls;
        if (offline) {
            throw Error("offline");
        }

        return charts;
    }

    std::vector<Score> scores(const std::string&, const std::string&) override {
        ++score_calls;
        if (offline || score_calls == fail_scores_at) {
            throw Error("offline");
        }

        return records;
    }

    void upload(const std::string&, const std::string&, const Play& play) override {
        plays.push_back(play);
        if (upload_error == 0) {
            throw Error("lost response");
        }

        if (upload_error > 0) {
            throw HttpError(upload_error);
        }

        if (update_best) {
            records = {{play.chart_id, play.score, play.broken}};
        }
    }
};

void configure_fixture(const fs::path& state, const fs::path& game, bool sync = true, bool overlay = true) {
    Preferences config;
    config.game_root = game;
    config.sync = sync;
    config.overlay = overlay;
    config.protected_token = protect("fake-test-token");
    save_preferences(state, config);
}

struct FakeFeed {
    Object packet;
    double clock = 100;
    uint64_t observed = now();

    ExportFeed reader() {
        return [this](const fs::path&) -> std::optional<ExportFrame> {
            return ExportFrame{encode(packet), clock, 2, observed};
        };
    }

    void current(const Result& value) {
        auto chart = result_json(value);
        put(chart, L"playing", true);
        packet.Insert(L"current", chart);
    }

    void completed(const Result& value, double age = 0) {
        packet.Insert(L"result", result_json(value));
        packet.Insert(L"completed", Value::CreateNumberValue(clock - age));
    }
};

void engine_checks() {
    Fixture fixture("engine");
    auto game = fixture.game(), state = fixture.root / L"state";
    configure_fixture(state, game);
    FakeApi api;
    FakeFeed feed;
    api.records = {{"chart-id", 900000, false}};
    Engine engine(state, api, feed.reader());
    engine.load();
    engine.sync();
    auto value = result();
    value.plate = "TG";
    feed.current(value);
    engine.poll();
    auto overlay = parse(engine.state());
    check(overlay.Size() == 4 && overlay.HasKey(L"playing") && overlay.HasKey(L"song") &&
              overlay.HasKey(L"difficulty") && overlay.HasKey(L"pb"),
        "overlay exposes exactly the requested fields plus visibility");
    check(number(overlay, L"pb") == 900000 && str(overlay, L"song") == "Test Song!",
        "PB comes from the website");
    check(!flag(parse(engine.state(now() + 50000000)), L"playing"), "stale heartbeat hides overlay");
    auto written = now() - 10000000;
    engine.capture(value, written);
    check(number(parse(engine.state()), L"pb") == 900000, "captured result cannot become overlay PB");
    check(engine.store().pending.size() == 1 && engine.store().pending[0].played_at == iso_time(written),
        "actual export time is stored as completion time");
    check(load_store(state).pending[0].result.plate == "TG", "plate survives persistence for offline retry");
    auto legacy_result = result_from_json(parse(
        R"({"id":"old","title":"Song","type":"Single","difficulty":"S10","level":10,"score":900000,"eligible":true})"));
    check(legacy_result.plate.empty(), "pending results from previous versions have no invented plate");
    engine.capture(value, written);
    check(engine.store().pending.size() == 1, "same result is captured once");
    engine.sync();
    check(api.plays.size() == 1 && api.plays[0].chart_id == "chart-id" && api.plays[0].score == 950000 &&
              api.plays[0].played_at == iso_time(written) && api.plays[0].plate == "TG",
        "new PB uploads exact score, chart and completion time");
    check(engine.store().pending.empty() && engine.store().receipts.at(value.id) == "accepted",
        "confirmed result payload is removed");
    check(read(state / L"uploads.json").find("950000") == std::string::npos,
        "receipt retains no score payload");
    Engine restarted(state, api, feed.reader());
    restarted.load();
    restarted.capture(value, written);
    restarted.sync();
    check(api.plays.size() == 1, "receipt prevents duplicate after restart");
    restarted.capture(result("lower", 850000), now());
    restarted.sync();
    check(api.plays.size() == 1 && restarted.store().receipts.at("lower") == "covered",
        "lower score is covered without upload");
    restarted.capture(result("broken", 990000, true), now());
    restarted.sync();
    check(api.plays.size() == 1, "broken score cannot replace a clear");
    auto invalid = result("autoplay");
    invalid.eligible = false;
    restarted.capture(invalid, now());
    auto out_of_range = result("invalid", 1000001);
    restarted.capture(out_of_range, now());
    check(restarted.store().receipts.at("autoplay") == "skipped" &&
              restarted.store().receipts.at("invalid") == "skipped",
        "ineligible and out of range results are skipped");
    Score broken_best{"x", 990000, true};
    check(better(result("clear", 100), &broken_best), "clear takes priority over broken numeric score");
    auto duplicate_catalog = api.charts;
    duplicate_catalog.push_back(api.charts[0]);
    check(match(value.chart, duplicate_catalog).empty(), "ambiguous chart is not guessed");
    auto custom = value.chart;
    custom.difficulty = "Custom";
    check(match(custom, api.charts).empty(), "custom description does not match official chart");
    auto coop = value.chart;
    coop.type = "CoOp";
    check(match(coop, api.charts).empty(), "unsupported chart type does not match");
    auto unicode = value.chart;
    unicode.title = "Ｔｅｓｔ Ｓｏｎｇ";
    check(match(unicode, api.charts) == "chart-id", "Unicode compatibility normalization matches titles");
    auto changed_level = value.chart;
    changed_level.level = 11;
    check(match(changed_level, api.charts).empty(), "difficulty and level must agree");
    check(engine.state().find("fake-test-token") == std::string::npos &&
              read(state / L"settings.json").find("fake-test-token") == std::string::npos,
        "token is absent from overlay and plaintext settings");
    check(unprotect(protect("token-ü")) == "token-ü", "Windows user encryption round trips UTF-8");
    rejects(
        [&] {
            validate_token("a\nsecret");
        },
        "control characters rejected in token");
    rejects(
        [&] {
            validate_mix("XX");
        },
        "legacy score scale rejected");
    auto no_token_state = fixture.root / L"no-token";
    Preferences blank;
    blank.game_root = game;
    save_preferences(no_token_state, blank);
    Engine no_token(no_token_state, api, feed.reader());
    no_token.load();
    no_token.capture(value, now());
    check(no_token.store().pending.empty(), "no token means no score capture");
    auto read_only_state = fixture.root / L"overlay-only";
    configure_fixture(read_only_state, game, false, true);
    FakeApi read_only_api;
    Engine read_only(read_only_state, read_only_api, feed.reader());
    read_only.load();
    feed.completed(result("overlay-only"));
    read_only.poll();
    read_only.capture(value, now());
    read_only.sync();
    check(read_only.store().pending.empty() && read_only_api.plays.empty() &&
              !fs::exists(read_only_state / L"uploads.json"),
        "overlay-only never captures or uploads results");
    auto sync_state = fixture.root / L"sync-only";
    configure_fixture(sync_state, game, true, false);
    Engine sync_only(sync_state, api, feed.reader());
    sync_only.load();
    check(sync_only.status().find("Waiting for XSanity exports") != std::string::npos,
        "missing exporter cannot look like healthy syncing");
    sync_only.poll();
    check(!flag(parse(sync_only.state()), L"playing"), "sync-only has no overlay state");
    check(sync_only.status().find("Waiting for XSanity exports") == std::string::npos,
        "sync-only mode monitors the game heartbeat");
    check(sync_only.status(now() + 200000000).find("Waiting for XSanity exports") != std::string::npos,
        "stopped exporter becomes visible after heartbeat expires");
    feed.packet.Insert(L"current", parse(R"({"playing":false,"error":"test Lua failure"})"));
    sync_only.poll();
    sync_only.sync();
    check(sync_only.status() == "XSanity exporter: test Lua failure",
        "successful API refresh cannot conceal a Lua error");
    check(!flag(parse(sync_only.state()), L"playing"), "exporter diagnostics never become OBS content");
    feed.current(value);
    sync_only.poll();
    check(sync_only.status().find("test Lua failure") == std::string::npos,
        "healthy heartbeat clears recovered exporter error");
    auto cutoff_state = fixture.root / L"cutoff";
    configure_fixture(cutoff_state, game);
    auto cutoff = load_preferences(cutoff_state);
    cutoff.capture_after = now();
    save_preferences(cutoff_state, cutoff);
    Engine cutoff_engine(cutoff_state, api, feed.reader());
    cutoff_engine.load();
    cutoff_engine.capture(value, cutoff.capture_after - 1);
    check(cutoff_engine.store().pending.empty(), "enabling sync cannot import a stale exported result");
    auto paused_state = fixture.root / L"offline";
    configure_fixture(paused_state, game);
    FakeApi offline;
    offline.offline = true;
    Engine queued(paused_state, offline, feed.reader());
    queued.load();
    queued.capture(value, now());
    rejects(
        [&] {
            queued.sync();
        },
        "offline sync fails without POST");
    check(queued.store().pending[0].state == "queued" && offline.plays.empty(),
        "offline result stays durable and queued");
    rejects(
        [&] {
            queued.configure("fake-test-token", "Phoenix2");
        },
        "pending results prevent mix changes");
    queued.configure("replacement-test-token", "Phoenix");
    check(queued.store().pending.size() == 1 && queued.token() == "replacement-test-token",
        "expired account token can be replaced without losing pending results");
    offline.offline = false;
    queued.sync();
    check(offline.plays.size() == 1 && queued.store().pending.empty(),
        "offline queue resumes when website returns");
    for (int code : {0, 400, 401, 403, 429, 500}) {
        auto error_state = fixture.root / (L"http-" + std::to_wstring(code));
        configure_fixture(error_state, game);
        FakeApi failing;
        failing.upload_error = code;
        Engine uncertain(error_state, failing, feed.reader());
        uncertain.load();
        uncertain.capture(value, now());
        uncertain.sync();
        auto expected = code == 400                                 ? "rejected"
                        : code == 401 || code == 403 || code == 429 ? "queued"
                                                                    : "uncertain";
        check(
            uncertain.store().pending[0].state == expected, "POST failure classified without losing payload");
        if (code == 0 || code == 500 || code == 400) {
            uncertain.sync();
            check(failing.plays.size() == 1, "unconfirmed or rejected POST is not repeated blindly");
        }

        if (code == 0) {
            failing.records = {{"chart-id", value.score, false}};
            uncertain.sync();
            check(uncertain.store().pending.empty() && uncertain.store().receipts.at(value.id) == "covered",
                "lost response reconciles with authoritative website PB");
        }
    }

    auto crash_state = fixture.root / L"crash";
    configure_fixture(crash_state, game);
    Store interrupted;
    interrupted.pending.push_back({value, "Phoenix", iso_time(now()), "sending"});
    save_store(crash_state, interrupted);
    FakeApi crash_api;
    Engine crash(crash_state, crash_api, feed.reader());
    crash.load();
    crash.sync();
    check(crash.store().pending[0].state == "uncertain" && crash_api.plays.empty(),
        "in-flight crash recovers without duplicate submission");
    auto refresh_state = fixture.root / L"refresh";
    configure_fixture(refresh_state, game);
    FakeApi refresh;
    refresh.fail_scores_at = 2;
    Engine accepted(refresh_state, refresh, feed.reader());
    accepted.load();
    accepted.capture(value, now());
    rejects(
        [&] {
            accepted.sync();
        },
        "refresh can fail after accepted upload");
    check(accepted.store().pending.empty() && accepted.store().receipts.at(value.id) == "accepted",
        "failed GET does not undo accepted POST");
    auto authority_state = fixture.root / L"authority";
    configure_fixture(authority_state, game);
    FakeApi authority;
    authority.records = {{"chart-id", 800000, false}};
    authority.update_best = false;
    Engine authoritative(authority_state, authority, feed.reader());
    authoritative.load();
    authoritative.capture(value, now());
    authoritative.sync();
    authoritative.poll();
    check(number(parse(authoritative.state()), L"pb") == 800000,
        "PB remains website value even when POST confirmation precedes PB refresh");
}

void capture_window_checks() {
    Fixture fixture("capture-window");
    auto game = fixture.game();
    auto state = fixture.root / L"state";
    configure_fixture(state, game);
    auto config = load_preferences(state);
    config.capture_after = now() - 1200000000;
    save_preferences(state, config);
    FakeApi api;
    FakeFeed feed;
    feed.current(result());
    Engine engine(state, api, feed.reader());
    engine.load();
    for (double age : {-1.0, 60.001, 101.0}) {
        feed.completed(result("expired"), age);
        engine.poll();
        check(engine.store().pending.empty(), "future, expired, and negative-clock results are ignored");
    }

    feed.completed(result("boundary"), 60);
    engine.poll();
    check(engine.store().pending.size() == 1 &&
              engine.store().pending.front().played_at == iso_time(feed.observed - 600000000),
        "mailbox age is accepted at the boundary and preserves completion time");
    engine.poll();
    check(engine.store().pending.size() == 1, "repeated mailbox snapshot is queued only once");
    feed.observed = config.capture_after;
    feed.completed(result("before-enabled"));
    engine.poll();
    check(engine.store().pending.size() == 1, "mailbox capture respects the enabling cutoff");
}

} // namespace piu::test
