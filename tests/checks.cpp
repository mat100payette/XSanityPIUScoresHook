#include "setup_view.h"
#include <oleacc.h>
#include <algorithm>
#include "engine.h"
#include "install.h"
#include "overlay.h"
#include "ui.h"
#include <wincodec.h>
#include <iostream>
#include <chrono>

using namespace piu;

namespace {
int passed = 0;

void check(bool condition, const char* label) {
    if (!condition) {
        throw Error(std::string("FAILED: ") + label);
    }

    ++passed;
}

template <typename F> void rejects(F&& action, const char* label) {
    bool rejected = false;
    try {
        action();
    } catch (...) {
        rejected = true;
    }

    check(rejected, label);
}

struct Fixture {
    fs::path root;

    explicit Fixture(const char* name, bool temporary = false) {
        root = temporary ? maintenance_directory() : executable().parent_path() / L"fixtures" / wide(name);
        safe_path(root);
        if (fs::exists(root)) {
            throw Error("A test fixture already exists. Inspect and remove build fixtures before retrying.");
        }

        fs::create_directories(root);
    }

    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    fs::path game(const wchar_t* name = L"game") {
        auto path = root / name;
        atomic_write(path / L"Program64" / L"XSanity.exe", "fake game");
        fs::create_directories(path / L"Themes" / L"xsanity" / L"BGAnimations");
        return path;
    }
};

Result result(std::string id = "event-1", int score = 950000, bool broken = false) {
    return {std::move(id), {"Test Song!", "Single", "S10", 10}, score, broken, true};
}

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

void write_current(const fs::path& game, const Result& value, bool playing = true) {
    auto data = result_json(value);
    put(data, L"playing", playing);
    atomic_write(GameHook::exports(game) / L"current.json", encode(data));
}

void engine_checks() {
    Fixture fixture("engine");
    auto game = fixture.game(), state = fixture.root / L"state";
    configure_fixture(state, game);
    FakeApi api;
    api.records = {{"chart-id", 900000, false}};
    Engine engine(state, api);
    engine.load();
    engine.sync();
    auto value = result();
    write_current(game, value);
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
    engine.capture(value, written);
    check(engine.store().pending.size() == 1, "same result is captured once");
    engine.sync();
    check(api.plays.size() == 1 && api.plays[0].chart_id == "chart-id" && api.plays[0].score == 950000 &&
              api.plays[0].played_at == iso_time(written),
        "new PB uploads exact score, chart and completion time");
    check(engine.store().pending.empty() && engine.store().receipts.at(value.id) == "accepted",
        "confirmed result payload is removed");
    check(read(state / L"uploads.json").find("950000") == std::string::npos,
        "receipt retains no score payload");
    Engine restarted(state, api);
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
    Engine no_token(no_token_state, api);
    no_token.load();
    no_token.capture(value, now());
    check(no_token.store().pending.empty(), "no token means no score capture");
    auto read_only_state = fixture.root / L"overlay-only";
    configure_fixture(read_only_state, game, false, true);
    FakeApi read_only_api;
    Engine read_only(read_only_state, read_only_api);
    read_only.load();
    atomic_write(GameHook::exports(game) / L"result.json", encode(result_json(result("overlay-only"))));
    read_only.poll();
    read_only.capture(value, now());
    read_only.sync();
    check(read_only.store().pending.empty() && read_only_api.plays.empty() &&
              !fs::exists(read_only_state / L"uploads.json"),
        "overlay-only never captures or uploads results");
    auto sync_state = fixture.root / L"sync-only";
    configure_fixture(sync_state, game, true, false);
    Engine sync_only(sync_state, api);
    sync_only.load();
    sync_only.poll();
    check(!flag(parse(sync_only.state()), L"playing"), "sync-only has no overlay state");
    auto cutoff_state = fixture.root / L"cutoff";
    configure_fixture(cutoff_state, game);
    auto cutoff = load_preferences(cutoff_state);
    cutoff.capture_after = now();
    save_preferences(cutoff_state, cutoff);
    Engine cutoff_engine(cutoff_state, api);
    cutoff_engine.load();
    cutoff_engine.capture(value, cutoff.capture_after - 1);
    check(cutoff_engine.store().pending.empty(), "enabling sync cannot import a stale exported result");
    atomic_write(game / L"Save" / L"Stats.xml", "sentinel history");
    engine.poll();
    check(read(game / L"Save" / L"Stats.xml") == "sentinel history", "history file remains untouched");
    auto paused_state = fixture.root / L"offline";
    configure_fixture(paused_state, game);
    FakeApi offline;
    offline.offline = true;
    Engine queued(paused_state, offline);
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
        Engine uncertain(error_state, failing);
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
    Engine crash(crash_state, crash_api);
    crash.load();
    crash.sync();
    check(crash.store().pending[0].state == "uncertain" && crash_api.plays.empty(),
        "in-flight crash recovers without duplicate submission");
    auto refresh_state = fixture.root / L"refresh";
    configure_fixture(refresh_state, game);
    FakeApi refresh;
    refresh.fail_scores_at = 2;
    Engine accepted(refresh_state, refresh);
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
    Engine authoritative(authority_state, authority);
    authoritative.load();
    authoritative.capture(value, now());
    authoritative.sync();
    authoritative.poll();
    check(number(parse(authoritative.state()), L"pb") == 800000,
        "PB remains website value even when POST confirmation precedes PB refresh");
}

void api_checks() {
    check(PiuScoresApi::allowed("https://piuscores.arroweclip.se/api/v2/charts?mix=Phoenix"),
        "official HTTPS API is allowed");
    for (auto url : {"http://piuscores.arroweclip.se/api/v2/charts",
             "https://evil.test/api/v2/charts",
             "https://piuscores.arroweclip.se.evil.test/api/v2/charts",
             "https://user@piuscores.arroweclip.se/api/v2/charts",
             "https://piuscores.arroweclip.se:444/api/v2/charts",
             "https://piuscores.arroweclip.se/api/v2/charts#fragment",
             "https://piuscores.arroweclip.se/account",
             "https://piuscores.arroweclip.se/api/v2/../account"}) {
        check(!PiuScoresApi::allowed(url), "unsafe API destination rejected before token transport");
    }

    int calls = 0;
    PiuScoresApi pages([&](const std::string&, const std::string&, const std::optional<Object>&) {
        ++calls;
        return parse(
            calls == 1
                ? R"({"data":[{"id":"a","songName":"Song","type":"Single","level":10}],"next":"https://piuscores.arroweclip.se/api/v2/charts?cursor=x"})"
                : R"({"data":[{"id":"b","songName":"Song","type":"Double","level":20}],"next":null})");
    });
    check(pages.catalog("test", "Phoenix").size() == 2 && calls == 2,
        "catalog cursor pages parsed with string chart IDs");
    calls = 0;
    PiuScoresApi malicious([&](const std::string&, const std::string&, const std::optional<Object>&) {
        ++calls;
        return parse(R"({"data":[],"next":"https://evil.test/api/v2/charts"})");
    });
    rejects(
        [&] {
            malicious.catalog("test", "Phoenix");
        },
        "foreign pagination URL rejected");
    check(calls == 1, "token never reaches foreign pagination transport");
    PiuScoresApi cycle([](const std::string& url, const std::string&, const std::optional<Object>&) {
        Object page;
        page.Insert(L"data", Array{});
        put(page, L"next", url);
        return page;
    });
    rejects(
        [&] {
            cycle.catalog("test", "Phoenix");
        },
        "pagination cycle rejected");
    PiuScoresApi legacy([](const std::string&, const std::string&, const std::optional<Object>&) {
        return parse(R"({"scoringModel":"legacy","data":[],"next":null})");
    });
    rejects(
        [&] {
            legacy.scores("test", "Phoenix");
        },
        "score envelope must use Phoenix model");
    PiuScoresApi upload([](const std::string& url, const std::string&, const std::optional<Object>& body) {
        check(url == "https://piuscores.arroweclip.se/api/v2/players/me/plays" && body &&
                  str(*body, L"source") == "xsanity-companion" && flag(*body, L"recordBrokenAsBest"),
            "POST uses documented route and source");
        auto play = body->GetNamedArray(L"plays").GetAt(0).GetObject();
        check(play.Size() == 4 && str(play, L"chartId") == "uuid" && number(play, L"score") == 999000,
            "no invented judgment or award fields uploaded");
        return parse(R"({"recorded":1})");
    });
    upload.upload("test", "Phoenix", {"uuid", 999000, false, iso_time(now())});
    PiuScoresApi unconfirmed([](const std::string&, const std::string&, const std::optional<Object>&) {
        return parse(R"({"recorded":0})");
    });
    rejects(
        [&] {
            unconfirmed.upload("test", "Phoenix", {});
        },
        "POST must confirm one recorded play");
}

struct FakeHost : InstallHost {
    bool running = false;
    int stops = 0, mutations = 0, fail_at = -1;
    Registration values;

    bool game_is_running() override {
        return running;
    }

    void stop_companion() override {
        ++stops;
    }

    Registration registration() override {
        return values;
    }

    void registration(const Registration& data) override {
        values = data;
    }

    void shortcut(const fs::path& path, const fs::path& target, const std::wstring& args) override {
        atomic_write(path, utf8(target.wstring() + L" " + args));
    }

    void checkpoint() override {
        if (++mutations == fail_at) {
            throw Error("Injected setup failure");
        }
    }
};

void installer_worker_checks() {
    Fixture fixture("installer-worker");
    auto game = fixture.game();
    InstallPaths paths{fixture.root / L"app", fixture.root / L"state", fixture.root / L"menu"};
    FakeHost host;
    Installer installer(paths, host, GameHook{}, "app", "setup", "license");

    // Match the real dialog: its UI thread owns the window mutex while a worker applies changes.
    Handle window_mutex(CreateMutexW(nullptr, TRUE, SetupWindowMutex));
    if (!window_mutex.get()) {
        fail("Create setup window test mutex");
    }

    bool created = GetLastError() != ERROR_ALREADY_EXISTS;
    DWORD acquired = created ? WAIT_OBJECT_0 : WaitForSingleObject(window_mutex.get(), 0);
    if (acquired == WAIT_FAILED) {
        fail("Acquire setup window test mutex");
    }

    // An open setup window may already own this mutex; either owner must allow the worker to run.
    bool owned = acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED;
    std::exception_ptr worker_error;
    std::thread worker([&] {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            try {
                installer.apply({game, true, false});
                check(installer.installed() && installer.selection().sync,
                    "setup worker installs while the window mutex is held");
                installer.apply({game, false, true});
                auto selection = installer.selection();
                check(!selection.sync && selection.overlay,
                    "setup worker changes components while the window mutex is held");
                installer.apply({game, false, false});
                check(!installer.installed() && !fs::exists(GameHook::layer(game)),
                    "setup worker removes all components while the window mutex is held");
            } catch (...) {
                worker_error = std::current_exception();
            }

            winrt::uninit_apartment();
        } catch (...) {
            worker_error = std::current_exception();
        }
    });
    worker.join();
    if (owned) {
        ReleaseMutex(window_mutex.get());
    }

    if (worker_error) {
        std::rethrow_exception(worker_error);
    }
}

void installer_checks() {
    Fixture fixture("installer");
    auto game = fixture.game();
    auto game2 = fixture.game(L"game2");
    GameHook hook;
    InstallPaths paths{fixture.root / L"app", fixture.root / L"state", fixture.root / L"menu"};
    FakeHost host;
    Installer installer(paths, host, hook, "native app", "native setup", "license");
    check(!installer.installed() && installer.selection().sync && !installer.selection().overlay,
        "fresh installer defaults to sync with overlay unchecked");
    auto layer = GameHook::layer(game);
    installer.apply({game, true, false});
    auto config = load_preferences(paths.state);
    check(installer.installed() && config.sync && !config.overlay && hook.owned(GameHook::layer(game)),
        "fresh sync-only installation creates shared layer");
    check(hook.current(layer) &&
              str(parse(read(paths.app / L"installation.json")), L"hookSha256") == hook.fingerprint(),
        "fresh setup installs the current exporter and records its fingerprint");
    check(host.values && fs::exists(paths.menu / L"Play XSanity.lnk") &&
              fs::exists(paths.menu / L"Manage installation.lnk"),
        "setup registers maintenance and simultaneous launch shortcuts");
    auto preferences = config;
    preferences.protected_token = protect("existing-token");
    save_preferences(paths.state, preferences);
    installer.apply({game, true, true});
    config = load_preferences(paths.state);
    check(config.sync && config.overlay && unprotect(config.protected_token) == "existing-token",
        "add optional overlay without losing account settings");
    host.running = true;
    installer.apply({game, true, false});
    check(!load_preferences(paths.state).overlay && hook.owned(GameHook::layer(game)),
        "remove overlay while game is open and keep syncing layer");
    Store queue;
    queue.pending.push_back({result(), "Phoenix", iso_time(now()), "queued"});
    save_store(paths.state, queue);
    installer.apply({game, false, true});
    check(!load_preferences(paths.state).sync && load_preferences(paths.state).overlay &&
              load_store(paths.state).pending.empty(),
        "remove syncing independently and erase pending score payloads");
    installer.apply({game, true, true});
    check(load_preferences(paths.state).capture_after >= config.capture_after,
        "reenabling sync establishes new export cutoff");
    rejects(
        [&] {
            installer.apply({game, false, false});
        },
        "full uninstall requires game closed to remove its layer");
    check(installer.installed(), "game-running preflight changes no installed files");
    rejects(
        [&] {
            installer.apply({game2, true, true});
        },
        "moving game connection requires game closed");
    host.running = false;
    installer.apply({game2, false, true});
    check(!fs::exists(GameHook::layer(game)) && hook.owned(GameHook::layer(game2)),
        "changing selected game moves only owned layer");
    atomic_write(paths.app / L"keep.txt", "foreign app file");
    atomic_write(paths.state / L"keep.txt", "foreign state file");
    atomic_write(GameHook::exports(game2) / L"keep.txt", "foreign game export");
    installer.apply({game2, false, false});
    check(!installer.installed() && !host.values && !fs::exists(paths.app / L"PiuCompanion.exe") &&
              !fs::exists(paths.state / L"settings.json") && !fs::exists(GameHook::layer(game2)),
        "full uninstall removes app, account data, registration and layer");
    check(read(paths.app / L"keep.txt") == "foreign app file" &&
              read(paths.state / L"keep.txt") == "foreign state file" &&
              read(GameHook::exports(game2) / L"keep.txt") == "foreign game export",
        "uninstall preserves unrelated files");
    installer.apply({game, false, true});
    check(!load_preferences(paths.state).sync && load_preferences(paths.state).overlay,
        "overlay can be installed alone after removal");
    auto settings_before = read(paths.state / L"settings.json"),
         marker_before = read(paths.app / L"installation.json");
    host.fail_at = host.mutations + 3;
    rejects(
        [&] {
            installer.apply({game, true, false});
        },
        "injected update failure triggers rollback");
    check(read(paths.state / L"settings.json") == settings_before &&
              read(paths.app / L"installation.json") == marker_before && hook.owned(GameHook::layer(game)),
        "failed update restores settings, marker and shared layer");
    host.fail_at = -1;
    host.fail_at = host.mutations + 4;
    rejects(
        [&] {
            installer.apply({game, false, false});
        },
        "uninstall registration failure triggers rollback");
    check(installer.installed() && host.values && hook.owned(GameHook::layer(game)) &&
              read(paths.state / L"settings.json") == settings_before,
        "failed uninstall restores registration and game layer");
    host.fail_at = -1;
    atomic_write(GameHook::layer(game), "customized layer");
    int before_stops = host.stops;
    rejects(
        [&] {
            installer.apply({game, false, false});
        },
        "modified hook cannot be removed");
    check(read(GameHook::layer(game)) == "customized layer" && host.stops == before_stops,
        "conflict preflight leaves modified layer and running app untouched");
    atomic_write(GameHook::layer(game), hook.source());
    installer.apply({game, false, false});
    auto clean = fixture.root / L"new";
    InstallPaths new_paths{clean / L"app", clean / L"state", clean / L"menu"};
    FakeHost new_host;
    Installer fresh(new_paths, new_host, hook, "app", "setup", "license");
    new_host.fail_at = 4;
    rejects(
        [&] {
            fresh.apply({game, true, false});
        },
        "failed fresh installation rolls back");
    check(!fs::exists(GameHook::layer(game)) && !fs::exists(new_paths.app / L"PiuCompanion.exe") &&
              !fs::exists(new_paths.state / L"settings.json") && !new_host.values,
        "fresh rollback removes newly created files and registration");
    auto conflict = fixture.game(L"conflict");
    atomic_write(GameHook::layer(conflict), "other theme layer");
    new_host.fail_at = -1;
    rejects(
        [&] {
            fresh.apply({conflict, true, false});
        },
        "fresh install refuses other layer");
    check(
        read(GameHook::layer(conflict)) == "other theme layer", "other theme source preserved byte for byte");
    auto defaults_folder = fixture.root / L"settings-defaults";
    atomic_write(
        defaults_folder / L"settings.json", R"({"Mix":"Phoenix2","ProtectedToken":"","GameRoot":""})");
    auto defaults = load_preferences(defaults_folder);
    check(defaults.mix == "Phoenix2" && defaults.sync && !defaults.overlay,
        "missing optional settings use component defaults");
    auto duplicate_layer = fixture.game(L"duplicate");
    fs::create_directory(GameHook::layer(duplicate_layer).parent_path() / L"ScreenSystemLayer aux");
    rejects(
        [&] {
            hook.preflight(duplicate_layer);
        },
        "directory theme layer conflict rejected");
    fresh.apply({game, false, true});
    fs::remove(new_paths.state / L"settings.json");
    check(fresh.selection().game_root == game && fresh.selection().overlay && !fresh.selection().sync,
        "maintenance reads installed components even if account settings are missing");
    atomic_write(new_paths.state / L"settings.json", "damaged settings");
    fresh.apply({game, false, false});
    check(!fresh.installed() && !fs::exists(GameHook::layer(game)),
        "full uninstall works with damaged account settings");
}

using FileSnapshot = std::map<fs::path, std::string>;

FileSnapshot snapshot_files(const fs::path& root) {
    FileSnapshot files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            files.emplace(entry.path(), read(entry.path()));
        }
    }

    return files;
}

bool same_registration(const Registration& left, const Registration& right) {
    if (left.has_value() != right.has_value()) {
        return false;
    }

    if (!left) {
        return true;
    }

    if (left->size() != right->size()) {
        return false;
    }

    for (const auto& [name, value] : *left) {
        auto found = right->find(name);
        if (found == right->end() || found->second.type != value.type || found->second.bytes != value.bytes) {
            return false;
        }
    }

    return true;
}

struct HookUpgradeFixture {
    Fixture files;
    fs::path game;
    InstallPaths paths;
    FakeHost host;

    explicit HookUpgradeFixture(const std::string& name)
        : files(name.c_str()), game(files.game()),
          paths{files.root / L"app", files.root / L"state", files.root / L"menu"} {
    }

    Installer installer(const GameHook& hook) {
        return Installer(paths, host, hook, "new app", "new setup", "license");
    }

    void move_game() {
        auto destination = files.root / L"moved game";
        fs::rename(game, destination);
        game = destination;
    }

    void seed() {
        auto preferences = load_preferences(paths.state);
        preferences.mix = "Phoenix2";
        preferences.protected_token = protect("upgrade-account-token");
        preferences.capture_after = 1;
        save_preferences(paths.state, preferences);

        Store queue;
        queue.pending.push_back({result("queued-before-upgrade"), "Phoenix2", iso_time(now()), "queued"});
        queue.receipts.emplace("previous-upload", "accepted");
        save_store(paths.state, queue);
        write_current(game, result("old-current"));
        atomic_write(GameHook::exports(game) / L"result.json", encode(result_json(result("old-result"))));
        atomic_write(GameHook::exports(game) / L"keep.txt", "unrelated export file");
    }
};

void hook_receipt_checks() {
    const GameHook first("return Def.ActorFrame {}\n-- test exporter release 1\n");
    const GameHook latest("return Def.ActorFrame {}\n-- test exporter release 9\n");
    HookUpgradeFixture marker_errors("hook-marker-errors");
    marker_errors.installer(first).apply({marker_errors.game, true, false});
    auto valid_marker = read(marker_errors.paths.app / L"installation.json");
    auto error_setup = marker_errors.installer(latest);
    auto refuses_marker = [&](const std::optional<std::string>& content, const char* label) {
        auto path = marker_errors.paths.app / L"installation.json";
        if (content) {
            atomic_write(path, *content);
        } else {
            fs::remove(path);
        }

        auto files = snapshot_files(marker_errors.files.root);
        auto stops = marker_errors.host.stops;
        rejects(
            [&] {
                error_setup.apply({marker_errors.game, true, false});
            },
            label);
        check(snapshot_files(marker_errors.files.root) == files && marker_errors.host.stops == stops,
            "invalid receipt refuses maintenance before changing files or stopping companion");
        atomic_write(path, valid_marker);
    };
    refuses_marker(std::nullopt, "missing marker cannot adopt an unknown installed app or previous exporter");
    refuses_marker("damaged JSON", "malformed installation receipt is refused");
    for (auto key : {L"product", L"gameRoot", L"sync", L"overlay", L"hookSha256"}) {
        auto invalid = parse(valid_marker);
        invalid.Insert(key, Value::CreateNullValue());
        refuses_marker(encode(invalid), "wrong typed required installation field is refused");
    }

    auto missing_hash = parse(valid_marker);
    missing_hash.Remove(L"hookSha256");
    refuses_marker(encode(missing_hash), "installation receipt requires an exporter fingerprint");
    for (double schema : {0.0, 2.0, 1.5}) {
        auto invalid = parse(valid_marker);
        invalid.Insert(L"schema", Value::CreateNumberValue(schema));
        refuses_marker(encode(invalid), "unsupported or fractional installation schema is refused");
    }

    auto wrong_hash = parse(valid_marker);
    put(wrong_hash, L"hookSha256", std::string(64, '0'));
    refuses_marker(encode(wrong_hash), "receipt cannot authorize a previous exporter whose hash differs");
    put(wrong_hash, L"hookSha256", "not-a-sha256");
    refuses_marker(encode(wrong_hash), "invalid fingerprint syntax is refused");
    auto empty_root = parse(valid_marker);
    put(empty_root, L"gameRoot", "");
    refuses_marker(encode(empty_root), "receipt cannot refer to an empty game folder");

    auto nul_root = parse(valid_marker);
    auto root_with_nul = utf8(marker_errors.game.wstring()) + std::string(1, '\0') + "ignored";
    put(nul_root, L"gameRoot", root_with_nul);
    refuses_marker(encode(nul_root), "embedded NUL cannot truncate receipt game folder validation");
    auto nul_hash = parse(valid_marker);
    put(nul_hash, L"hookSha256", first.fingerprint() + std::string(1, '\0') + "ignored");
    refuses_marker(encode(nul_hash), "embedded NUL cannot truncate receipt fingerprint validation");

    HookUpgradeFixture recovery("hook-repair");
    recovery.installer(first).apply({recovery.game, true, false});
    recovery.seed();
    auto recovery_queue = read(recovery.paths.state / L"uploads.json");
    fs::remove(GameHook::layer(recovery.game));
    recovery.installer(latest).apply({recovery.game, true, false});
    check(latest.current(GameHook::layer(recovery.game)) &&
              read(recovery.paths.state / L"uploads.json") == recovery_queue &&
              !fs::exists(GameHook::exports(recovery.game) / L"result.json"),
        "valid receipt repairs a missing exporter without losing pending uploads");
}

void hook_race_checks() {
    const GameHook first("return Def.ActorFrame {}\n-- test exporter release 1\n");
    const GameHook latest("return Def.ActorFrame {}\n-- test exporter release 9\n");

    struct EditingHost : FakeHost {
        fs::path target;

        void stop_companion() override {
            FakeHost::stop_companion();
            atomic_write(target, "external change while companion stops");
        }
    };

    HookUpgradeFixture race("hook-stop-race");
    race.installer(first).apply({race.game, true, false});
    race.seed();
    EditingHost editing;
    editing.values = race.host.values;
    editing.target = GameHook::layer(race.game);
    auto race_files = snapshot_files(race.files.root);
    race_files[editing.target] = "external change while companion stops";
    Installer race_setup(race.paths, editing, latest, "app", "setup", "license");
    rejects(
        [&] {
            race_setup.apply({race.game, true, false});
        },
        "exporter changes during companion shutdown are checked again before mutation");
    check(snapshot_files(race.files.root) == race_files &&
              same_registration(editing.values, race.host.values) && editing.stops == 1,
        "post-shutdown refusal preserves the external change and all installed state");

    struct StartingGameHost : FakeHost {
        void stop_companion() override {
            FakeHost::stop_companion();
            running = true;
        }
    };

    StartingGameHost starting;
    starting.values = race.host.values;
    atomic_write(editing.target, first.source());
    auto before_game_start = snapshot_files(race.files.root);
    Installer starting_setup(race.paths, starting, latest, "app", "setup", "license");
    rejects(
        [&] {
            starting_setup.apply({race.game, true, false});
        },
        "game starting during companion shutdown prevents exporter mutation");
    check(snapshot_files(race.files.root) == before_game_start &&
              same_registration(starting.values, race.host.values),
        "late game-start refusal preserves the old exporter and all installed state");

    HookUpgradeFixture concurrent("hook-concurrent");
    concurrent.installer(first).apply({concurrent.game, true, false});
    auto background_setup = concurrent.installer(latest);
    auto competing_setup = concurrent.installer(latest);
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Handle resume(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ready.get() || !resume.get()) {
        fail("Create concurrent setup test events");
    }

    std::exception_ptr background_error;
    auto stops = concurrent.host.stops;
    auto concurrent_files = snapshot_files(concurrent.files.root);
    std::thread background([&] {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            background_setup.apply({concurrent.game, true, false}, [&](int percent, std::wstring_view) {
                if (percent == 30) {
                    SetEvent(ready.get());
                    WaitForSingleObject(resume.get(), 10000);
                }
            });
            winrt::uninit_apartment();
        } catch (...) {
            background_error = std::current_exception();
        }
    });
    auto signaled = WaitForSingleObject(ready.get(), 10000) == WAIT_OBJECT_0;
    bool refused = false;
    if (signaled) {
        try {
            competing_setup.apply({concurrent.game, true, false});
        } catch (...) {
            refused = true;
        }
    }

    bool untouched = snapshot_files(concurrent.files.root) == concurrent_files;
    SetEvent(resume.get());
    background.join();
    if (background_error) {
        std::rethrow_exception(background_error);
    }

    check(signaled && refused && untouched && concurrent.host.stops == stops + 1,
        "overlapping setup refuses a second operation before companion stop or file mutation");
    check(latest.current(GameHook::layer(concurrent.game)),
        "serialized upgrade completes successfully after competing setup is refused");
}

void hook_relocation_checks() {
    const GameHook first("return Def.ActorFrame {}\n-- test exporter release 1\n");
    const GameHook latest("return Def.ActorFrame {}\n-- test exporter release 9\n");
    for (bool upgrade : {false, true}) {
        HookUpgradeFixture moved(upgrade ? "hook-move-upgrade" : "hook-move-current");
        moved.installer(first).apply({moved.game, true, true});
        moved.seed();
        auto old_root = moved.game;
        auto token = load_preferences(moved.paths.state).protected_token;
        auto queue = read(moved.paths.state / L"uploads.json");
        auto timestamp = fs::file_time_type::clock::now() - std::chrono::hours(48);
        fs::last_write_time(GameHook::layer(moved.game), timestamp);
        moved.move_game();
        const auto& target = upgrade ? latest : first;
        auto setup = moved.installer(target);

        moved.host.running = true;
        auto before = snapshot_files(moved.files.root);
        auto stops = moved.host.stops;
        rejects(
            [&] {
                setup.apply({moved.game, true, true});
            },
            "reconnecting a moved game requires the game closed even if the exporter is current");
        check(snapshot_files(moved.files.root) == before && moved.host.stops == stops,
            "running-game relocation leaves the moved folder and installed state untouched");

        moved.host.running = false;
        setup.apply({moved.game, true, true});
        auto marker = parse(read(moved.paths.app / L"installation.json"));
        auto config = load_preferences(moved.paths.state);
        check(!fs::exists(old_root) && target.current(GameHook::layer(moved.game)) &&
                  config.game_root == moved.game && str(marker, L"gameRoot") == utf8(moved.game.wstring()) &&
                  str(marker, L"hookSha256") == target.fingerprint(),
            "relocation adopts the recorded exporter and commits its new path and target version");
        check(config.sync && config.overlay && config.mix == "Phoenix2" && config.protected_token == token &&
                  read(moved.paths.state / L"uploads.json") == queue,
            "move and upgrade preserve component choices, account and pending uploads");
        check(config.capture_after > 1 && !fs::exists(GameHook::exports(moved.game) / L"current.json") &&
                  !fs::exists(GameHook::exports(moved.game) / L"result.json") &&
                  read(GameHook::exports(moved.game) / L"keep.txt") == "unrelated export file",
            "relocation clears stale exports and advances capture cutoff without removing unrelated files");
        if (!upgrade) {
            check(fs::last_write_time(GameHook::layer(moved.game)) == timestamp,
                "relocation keeps a current exporter without rewriting it");
        }

        moved.installer(latest).apply({moved.game, false, false});
        check(!fs::exists(GameHook::layer(moved.game)) && fs::exists(game_executable(moved.game)) &&
                  !fs::exists(old_root) && !setup.installed(),
            "uninstall follows the relocated receipt and preserves the game");
    }

    HookUpgradeFixture copied("hook-copy-upgrade");
    copied.installer(first).apply({copied.game, false, true});
    auto config = load_preferences(copied.paths.state);
    config.mix = "Phoenix2";
    config.protected_token = protect("copy-account-token");
    save_preferences(copied.paths.state, config);
    atomic_write(GameHook::exports(copied.game) / L"current.json", "stale chart");
    atomic_write(GameHook::exports(copied.game) / L"keep.txt", "unrelated file");
    auto destination = copied.files.root / L"copied game";
    fs::copy(copied.game, destination, fs::copy_options::recursive);
    auto copied_setup = copied.installer(latest);
    for (const auto& root : {copied.game, destination}) {
        atomic_write(GameHook::layer(root), first.source() + "-- owner edit\n");
        auto before = snapshot_files(copied.files.root);
        auto stops = copied.host.stops;
        rejects(
            [&] {
                copied_setup.apply({destination, false, true});
            },
            "relocation refuses an edited exporter in either the old or selected folder");
        check(snapshot_files(copied.files.root) == before && copied.host.stops == stops,
            "conflicting relocation preserves both folders and receipt before stopping the companion");
        atomic_write(GameHook::layer(root), first.source());
    }

    copied_setup.apply({destination, false, true});
    check(latest.current(GameHook::layer(destination)) && !fs::exists(GameHook::layer(copied.game)) &&
              fs::exists(game_executable(copied.game)) &&
              read(GameHook::exports(copied.game) / L"keep.txt") == "unrelated file" &&
              read(GameHook::exports(destination) / L"keep.txt") == "unrelated file" &&
              !fs::exists(GameHook::exports(copied.game) / L"current.json") &&
              !fs::exists(GameHook::exports(destination) / L"current.json"),
        "selecting a copied game transfers the exporter and clears stale charts while preserving both games");
    auto relocated = load_preferences(copied.paths.state);
    check(!relocated.sync && relocated.overlay && relocated.game_root == destination &&
              relocated.mix == config.mix && relocated.protected_token == config.protected_token &&
              !fs::exists(copied.paths.state / L"uploads.json"),
        "overlay-only relocation retains account settings without enabling syncing or creating a queue");

    HookUpgradeFixture race("hook-move-race");
    race.installer(first).apply({race.game, true, false});
    race.seed();
    race.move_game();
    auto layer = GameHook::layer(race.game);
    auto before = snapshot_files(race.files.root);
    before[layer] = "external change at relocation commit";
    auto registry = race.host.values;
    rejects(
        [&] {
            race.installer(latest).apply({race.game, true, false}, [&](int percent, std::wstring_view) {
                if (percent == 30) {
                    atomic_write(layer, "external change at relocation commit");
                }
            });
        },
        "relocation checks the destination again inside its transaction before replacing it");
    check(snapshot_files(race.files.root) == before && same_registration(race.host.values, registry),
        "relocation race preserves the external edit and original receipt, settings and exports");
}

void hook_rollback_checks(bool move) {
    const GameHook first("return Def.ActorFrame {}\n-- test exporter release 1\n");
    const GameHook latest("return Def.ActorFrame {}\n-- test exporter release 9\n");
    HookUpgradeFixture stages(move ? "hook-move-rollback-count" : "hook-rollback-count");
    stages.installer(first).apply({stages.game, true, true});
    stages.seed();
    if (move) {
        stages.move_game();
    }

    auto initial_mutations = stages.host.mutations;
    stages.installer(latest).apply({stages.game, true, true});
    auto mutation_count = stages.host.mutations - initial_mutations;
    check(
        mutation_count > 6, "upgrade exercises hook, exports, settings, receipt and application transaction");

    for (int stage = 1; stage <= mutation_count; ++stage) {
        HookUpgradeFixture rollback(
            (move ? "hook-move-rollback-" : "hook-rollback-") + std::to_string(stage));
        rollback.installer(first).apply({rollback.game, true, true});
        rollback.seed();
        if (move) {
            rollback.move_game();
        }

        auto before = snapshot_files(rollback.files.root);
        auto registry = rollback.host.values;
        rollback.host.fail_at = rollback.host.mutations + stage;
        rejects(
            [&] {
                rollback.installer(latest).apply({rollback.game, true, true});
            },
            "injected failure at each upgrade mutation triggers rollback");
        check(snapshot_files(rollback.files.root) == before &&
                  same_registration(rollback.host.values, registry),
            "every failed upgrade restores exporter, receipt, settings, exports, queue and registration");
        rollback.host.fail_at = -1;
        rollback.installer(latest).apply({rollback.game, true, true});
        auto marker = parse(read(rollback.paths.app / L"installation.json"));
        check(latest.current(GameHook::layer(rollback.game)) &&
                  str(marker, L"hookSha256") == latest.fingerprint() &&
                  str(marker, L"gameRoot") == utf8(rollback.game.wstring()),
            "rolled-back installation remains usable for a successful retry");
    }
}

void hook_upgrade_checks() {
    const GameHook first("return Def.ActorFrame {}\n-- test exporter release 1\n");
    const GameHook second("return Def.ActorFrame {}\n-- test exporter release 2\n");
    const GameHook latest("return Def.ActorFrame {}\n-- test exporter release 9\n");
    HookUpgradeFixture sequential("hook-sequential");
    auto first_setup = sequential.installer(first);
    auto second_setup = sequential.installer(second);
    auto latest_setup = sequential.installer(latest);
    first_setup.apply({sequential.game, true, true});
    sequential.seed();

    auto layer = GameHook::layer(sequential.game);
    auto marker_path = sequential.paths.app / L"installation.json";
    auto marker = parse(read(marker_path));
    check(number(marker, L"schema") == 1 && str(marker, L"hookSha256") == first.fingerprint() &&
              str(marker, L"gameRoot") == utf8(sequential.game.wstring()),
        "installation records exact exporter fingerprint and its game folder");
    check(!latest.owned(layer) && latest.owned(layer, first.fingerprint()) && !latest.current(layer),
        "receipt recognizes arbitrary previous managed exporter without adding historical hashes");

    auto queued = read(sequential.paths.state / L"uploads.json");
    auto token = load_preferences(sequential.paths.state).protected_token;
    sequential.host.running = true;
    auto before_stops = sequential.host.stops;
    auto before_files = snapshot_files(sequential.files.root);
    rejects(
        [&] {
            second_setup.apply({sequential.game, true, true});
        },
        "exporter update requires the game to be closed");
    check(sequential.host.stops == before_stops && snapshot_files(sequential.files.root) == before_files,
        "game-running upgrade changes no files and does not stop companion");

    sequential.host.running = false;
    second_setup.apply({sequential.game, true, true});
    check(second.current(layer) && str(parse(read(marker_path)), L"hookSha256") == second.fingerprint(),
        "sequential update replaces old exporter and advances its receipt");
    auto upgraded = load_preferences(sequential.paths.state);
    check(upgraded.capture_after > 1 && upgraded.mix == "Phoenix2" && upgraded.protected_token == token &&
              read(sequential.paths.state / L"uploads.json") == queued,
        "exporter replacement resets cutoff while preserving account, settings and pending uploads");
    check(!fs::exists(GameHook::exports(sequential.game) / L"current.json") &&
              !fs::exists(GameHook::exports(sequential.game) / L"result.json") &&
              read(GameHook::exports(sequential.game) / L"keep.txt") == "unrelated export file",
        "exporter replacement clears only transient result and current-chart payloads");

    sequential.seed();
    latest_setup.apply({sequential.game, true, true});
    check(latest.current(layer) && str(parse(read(marker_path)), L"hookSha256") == latest.fingerprint(),
        "skipping arbitrary exporter releases updates through the installed receipt");

    sequential.seed();
    auto cutoff = load_preferences(sequential.paths.state).capture_after;
    auto exports = GameHook::exports(sequential.game);
    auto current = read(exports / L"current.json"), completed = read(exports / L"result.json");
    auto timestamp = fs::file_time_type::clock::now() - std::chrono::hours(48);
    fs::last_write_time(layer, timestamp);
    sequential.host.running = true;
    latest_setup.apply({sequential.game, true, false});
    check(fs::last_write_time(layer) == timestamp &&
              load_preferences(sequential.paths.state).capture_after == cutoff &&
              read(exports / L"current.json") == current && read(exports / L"result.json") == completed,
        "same exporter is not rewritten and component maintenance keeps payloads and cutoff");

    auto root_with_separator = fs::path(sequential.game.wstring() + L"\\");
    latest_setup.apply({root_with_separator, true, false});
    check(latest.current(layer) && fs::last_write_time(layer) == timestamp &&
              load_preferences(sequential.paths.state).capture_after == cutoff &&
              read(exports / L"current.json") == current && read(exports / L"result.json") == completed,
        "same game folder with trailing separator keeps current hook and payloads while game is running");

    std::string crlf;
    for (char character : latest.source()) {
        crlf += character == '\n' ? "\r\n" : std::string(1, character);
    }

    atomic_write(layer, crlf);
    fs::last_write_time(layer, timestamp);
    latest_setup.apply({sequential.game, true, false});
    check(latest.current(layer) && fs::last_write_time(layer) == timestamp &&
              GameHook::fingerprint(crlf) == latest.fingerprint(),
        "Windows line endings match the recorded exporter without a rewrite");
    auto standalone_cr = std::string("return Def.ActorFrame {}\r-- test exporter release 9\n");
    atomic_write(layer, standalone_cr);
    check(!latest.owned(layer, latest.fingerprint()) &&
              GameHook::fingerprint(standalone_cr) != latest.fingerprint(),
        "standalone carriage returns cannot erase a meaningful exporter modification");

    sequential.host.running = false;
    atomic_write(layer, latest.source() + "-- modified by owner\n");
    auto modified_files = snapshot_files(sequential.files.root);
    before_stops = sequential.host.stops;
    for (bool keep : {true, false}) {
        rejects(
            [&] {
                latest_setup.apply({sequential.game, keep, false});
            },
            "modified managed exporter refuses both update and removal");
    }

    check(snapshot_files(sequential.files.root) == modified_files && sequential.host.stops == before_stops,
        "modified-exporter refusal preserves files and leaves companion running");

    atomic_write(layer, latest.source());
    auto stale = parse(read(marker_path));
    put(stale, L"hookSha256", first.fingerprint());
    atomic_write(marker_path, encode(stale));
    fs::last_write_time(layer, timestamp);
    latest_setup.apply({sequential.game, true, false});
    check(fs::last_write_time(layer) == timestamp &&
              str(parse(read(marker_path)), L"hookSha256") == latest.fingerprint(),
        "exact current exporter safely repairs a stale valid receipt without a rewrite");

    auto other_game = sequential.files.game(L"other-game");
    atomic_write(GameHook::layer(other_game), second.source());
    before_stops = sequential.host.stops;
    rejects(
        [&] {
            latest_setup.apply({other_game, true, false});
        },
        "relocation cannot authorize an exporter that differs from both the receipt and current source");
    check(read(GameHook::layer(other_game)) == second.source() && sequential.host.stops == before_stops,
        "unrecognized destination exporter leaves both connections untouched");
    fs::remove(GameHook::layer(other_game));
    latest_setup.apply({other_game, false, true});
    check(!fs::exists(layer) && latest.current(GameHook::layer(other_game)) &&
              !load_preferences(sequential.paths.state).sync &&
              load_preferences(sequential.paths.state).overlay &&
              str(parse(read(marker_path)), L"gameRoot") == utf8(other_game.wstring()),
        "moving to overlay-only records the new receipt folder and preserves independent components");
    latest_setup.apply({other_game, false, false});
    check(!latest_setup.installed() && !fs::exists(GameHook::layer(other_game)),
        "receipt-managed exporter supports full removal after moving components");

    HookUpgradeFixture removal("hook-old-removal");
    removal.installer(first).apply({removal.game, false, true});
    removal.installer(latest).apply({removal.game, false, false});
    check(!fs::exists(GameHook::layer(removal.game)) && !fs::exists(removal.paths.app / L"installation.json"),
        "latest setup removes an unchanged older managed exporter through its receipt");

    hook_receipt_checks();
    hook_race_checks();
    hook_relocation_checks();
    hook_rollback_checks(false);
    hook_rollback_checks(true);
}

std::string get(unsigned short port, const std::string& header) {
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) {
        throw Error("test socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
        closesocket(client);
        throw Error("test connect");
    }

    send(client, header.data(), static_cast<int>(header.size()), 0);
    std::string response;
    char buffer[1024];
    for (;;) {
        int count = recv(client, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            break;
        }

        response.append(buffer, static_cast<size_t>(count));
    }

    closesocket(client);
    return response;
}

void overlay_checks() {
    OverlayServer disabled([] {
        return "{}";
    });
    disabled.start(false, 0);
    check(disabled.port() == 0, "disabled overlay opens no listener");
    OverlayServer server([] {
        return R"({"playing":true,"song":"Song","difficulty":"S10","pb":950000})";
    });
    server.start(true, 0);
    auto port = server.port();
    auto header = " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n";
    check(get(port, "GET /state" + header).find("\"pb\":950000") != std::string::npos,
        "local listener serves authoritative state");
    check(get(port, "GET /overlay" + header).find("<small>PB</small>") != std::string::npos,
        "embedded overlay page is served");
    check(get(port, "POST /state" + header).starts_with("HTTP/1.1 403"), "OBS listener refuses writes");
    check(
        get(port, "GET /missing" + header).starts_with("HTTP/1.1 404"), "OBS listener rejects unknown route");
    check(get(port, "GET /state HTTP/1.1\r\nHost: evil.test\r\n\r\n").starts_with("HTTP/1.1 403"),
        "foreign Host cannot access local state");
    std::string path;
    check(OverlayServer::request_status(
              "GET /state HTTP/1.1\r\nHost: localhost:8765\r\nHost: evil.test\r\n\r\n", 8765, path) == 403,
        "duplicate Host rejected");
    server.stop();
    check(server.port() == 0, "overlay stops and releases its port");
    server.start(true, port);
    check(server.port() == port, "overlay can restart on released port");
}

bool icon_size(HICON icon, int width, int height) {
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) {
        fail("Inspect icon");
    }

    BITMAP bitmap{};
    bool valid = info.hbmColor && GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) &&
                 bitmap.bmWidth == width && bitmap.bmHeight == height;
    DeleteObject(info.hbmColor);
    DeleteObject(info.hbmMask);
    return valid;
}

void icon_checks() {
    for (int resource : {IDI_APP, IDI_SETUP}) {
        Icon icon;
        bool valid = true;
        for (int size : {16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 84, 96, 112, 128, 144, 192, 256}) {
            icon.load(resource, size, size);
            valid = valid && icon_size(icon.get(), size, size);
        }

        check(valid, "embedded app and setup icons load at tray, title and header sizes");
    }
}

struct Preview {
    SetupView view;
    HWND window = nullptr;
    SetupAppearance appearance;
    UINT dpi;
    bool installed = false;
    std::string error;

    explicit Preview(SetupAppearance style, UINT scale = 96) : appearance(style), dpi(scale) {
        window = CreateDialogParamW(
            GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDD_SETUP),
            nullptr,
            [](HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) -> INT_PTR {
                auto preview = reinterpret_cast<Preview*>(GetWindowLongPtrW(dialog, DWLP_USER));
                if (message == WM_INITDIALOG) {
                    preview = reinterpret_cast<Preview*>(lparam);
                    SetWindowLongPtrW(dialog, DWLP_USER, lparam);
                    try {
                        preview->view.initialize(dialog, preview->appearance, preview->dpi);
                        SetDlgItemTextW(dialog, IDC_ROOT, L"C:\\XSanity");
                        CheckDlgButton(dialog, IDC_SYNC, BST_CHECKED);
                        CheckDlgButton(dialog, IDC_LAUNCH, BST_CHECKED);
                        preview->view.refresh(false);
                    } catch (...) {
                        preview->error = exception_message();
                    }

                    return TRUE;
                }

                if (preview) {
                    if (auto result = preview->view.message(message, wparam, lparam)) {
                        return *result;
                    }

                    if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED &&
                        (LOWORD(wparam) == IDC_SYNC || LOWORD(wparam) == IDC_OVERLAY)) {
                        preview->view.refresh(preview->installed);
                        return TRUE;
                    }
                }

                return FALSE;
            },
            reinterpret_cast<LPARAM>(this));
        if (!window || !error.empty()) {
            throw Error(error.empty() ? "Could not create setup preview." : error);
        }
    }

    ~Preview() {
        if (window) {
            DestroyWindow(window);
        }
    }

    void save(const fs::path& path, bool client_only = false) {
        RECT rect{};
        if (client_only) {
            GetClientRect(window, &rect);
        } else {
            GetWindowRect(window, &rect);
        }

        int width = rect.right - rect.left, height = rect.bottom - rect.top;
        HDC screen = client_only ? GetDC(window) : GetWindowDC(window);
        HDC memory = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        auto old = SelectObject(memory, bitmap);
        if (client_only) {
            // Paint the client and children directly; WM_PRINT's legacy frame is not the DWM frame.
            SendMessageW(
                window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_ERASEBKGND);
            for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) {
                    continue;
                }

                RECT bounds{};
                GetWindowRect(child, &bounds);
                MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
                int saved = SaveDC(memory);
                SetWindowOrgEx(memory, -bounds.left, -bounds.top, nullptr);
                IntersectClipRect(memory, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top);
                SendMessageW(
                    child, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_ERASEBKGND);
                RestoreDC(memory, saved);
            }
        } else {
            SendMessageW(window,
                WM_PRINT,
                reinterpret_cast<WPARAM>(memory),
                PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        }

        auto factory =
            winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory, CLSCTX_INPROC_SERVER);
        winrt::com_ptr<IWICBitmap> image;
        winrt::check_hresult(
            factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, image.put()));
        winrt::com_ptr<IWICStream> stream;
        winrt::check_hresult(factory->CreateStream(stream.put()));
        winrt::check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
        winrt::com_ptr<IWICBitmapEncoder> encoder;
        winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
        winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
        winrt::check_hresult(frame->Initialize(nullptr));
        winrt::check_hresult(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)));
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR;
        winrt::check_hresult(frame->SetPixelFormat(&format));
        winrt::check_hresult(frame->WriteSource(image.get(), nullptr));
        winrt::check_hresult(frame->Commit());
        winrt::check_hresult(encoder->Commit());
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(window, screen);
    }
};

void view_removal_transition_checks() {
    struct RedrawAudit {
        bool paused = false;
        int changes = 0, intermediate = 0;

        static LRESULT CALLBACK observe(
            HWND control, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
            auto audit = reinterpret_cast<RedrawAudit*>(data);
            if (message == WM_SETREDRAW) {
                audit->paused = !wparam;
            } else if (message == WM_ENABLE || message == WM_SETTEXT) {
                ++audit->changes;
                if (!audit->paused) {
                    ++audit->intermediate;
                }
            }

            return DefSubclassProc(control, message, wparam, lparam);
        }
    };

    for (auto appearance : {SetupAppearance::Dark, SetupAppearance::Light, SetupAppearance::Contrast}) {
        for (bool installed : {false, true}) {
            Preview preview(appearance);
            preview.installed = installed;
            preview.view.refresh(installed);
            auto dialog = preview.window;
            constexpr std::array<int, 5> Controls{IDC_ROOT, IDC_BROWSE, IDC_APPLY, IDC_STATUS, IDC_DETAILS};
            std::array<RedrawAudit, Controls.size()> audits;
            for (size_t index = 0; index < Controls.size(); ++index) {
                check(SetWindowSubclass(GetDlgItem(dialog, Controls[index]),
                          RedrawAudit::observe,
                          3,
                          reinterpret_cast<DWORD_PTR>(&audits[index])) != FALSE,
                    "observe native redraw state during component transitions");
            }

            auto field = GetDlgItem(dialog, IDC_ROOT);
            auto dc = GetDC(field);
            for (bool keep : {false, true, false, true}) {
                SendDlgItemMessageW(dialog, IDC_SYNC, BM_CLICK, 0, 0);
                check(control_text(dialog, IDC_APPLY) ==
                              (keep ? (installed ? L"&Apply changes" : L"&Install") : L"&Remove all") &&
                          (IsWindowEnabled(GetDlgItem(dialog, IDC_APPLY)) != FALSE) == (keep || installed),
                    "real checkbox notifications update removal caption and action availability");
                check((IsWindowEnabled(field) != FALSE) == keep &&
                          (IsWindowEnabled(GetDlgItem(dialog, IDC_BROWSE)) != FALSE) == keep &&
                          (IsWindowEnabled(GetDlgItem(dialog, IDC_LAUNCH)) != FALSE) == keep,
                    "entering and leaving removal updates all folder and launch controls");

                auto brush = reinterpret_cast<HBRUSH>(SendMessageW(dialog,
                    keep ? WM_CTLCOLOREDIT : WM_CTLCOLORSTATIC,
                    reinterpret_cast<WPARAM>(dc),
                    reinterpret_cast<LPARAM>(field)));
                LOGBRUSH background{};
                GetObjectW(brush, sizeof(background), &background);
                check(GetBkColor(dc) == background.lbColor,
                    "enabled and disabled folder text uses the same background as its returned brush");
            }

            bool batched = true;
            int changes = 0;
            for (size_t index = 0; index < Controls.size(); ++index) {
                auto control = GetDlgItem(dialog, Controls[index]);
                batched &= !audits[index].paused && audits[index].intermediate == 0 &&
                           (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
                changes += audits[index].changes;
                RemoveWindowSubclass(control, RedrawAudit::observe, 3);
            }

            check(changes >= 12 && batched,
                "removal transitions suppress intermediate native redraws and restore every child");
            check((GetWindowLongPtrW(dialog, GWL_STYLE) & WS_VISIBLE) == 0 &&
                      (GetWindowLongPtrW(dialog, GWL_EXSTYLE) & WS_EX_COMPOSITED) != 0,
                "batched composited updates do not reveal a hidden installer preview");
            ReleaseDC(field, dc);
            if (!installed && appearance == SetupAppearance::Dark) {
                SendDlgItemMessageW(dialog, IDC_SYNC, BM_CLICK, 0, 0);
                preview.save(executable().parent_path() / L"setup-none-client.png", true);
                SendDlgItemMessageW(dialog, IDC_SYNC, BM_CLICK, 0, 0);
                preview.save(executable().parent_path() / L"setup-restored-client.png", true);
            }
        }
    }
}

void view_input_visibility_checks() {
    struct VisibilityAudit {
        HWND dialog;
        int observations = 0;
        bool visible = true;

        static LRESULT CALLBACK observe(
            HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
            auto audit = reinterpret_cast<VisibilityAudit*>(data);
            auto result = DefSubclassProc(window, message, wparam, lparam);
            if (message == WM_SETREDRAW || message == WM_ENABLE || message == WM_SETTEXT) {
                ++audit->observations;
                audit->visible &= IsWindowVisible(audit->dialog) != FALSE;
            }

            return result;
        }
    };

    for (auto appearance : {SetupAppearance::Dark, SetupAppearance::Light, SetupAppearance::Contrast}) {
        Preview preview(appearance);
        auto dialog = preview.window;
        // Exercise a visible window without moving the user's pointer or covering their desktop.
        SetWindowPos(dialog,
            nullptr,
            GetSystemMetrics(SM_XVIRTUALSCREEN) - 5000,
            GetSystemMetrics(SM_YVIRTUALSCREEN) - 5000,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(dialog, SW_SHOWNOACTIVATE);
        check(IsWindowVisible(dialog) != FALSE, "input regression uses a visible installer");

        VisibilityAudit audit{dialog};
        std::vector<HWND> windows{dialog};
        for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            windows.push_back(child);
        }

        for (auto window : windows) {
            if (!SetWindowSubclass(
                    window, VisibilityAudit::observe, 4, reinterpret_cast<DWORD_PTR>(&audit))) {
                fail("Observe setup input visibility");
            }
        }

        for (bool installed : {false, true}) {
            preview.installed = installed;
            // Include ordinary toggles and repeated transitions into and out of Remove all.
            for (int id : {IDC_OVERLAY, IDC_SYNC, IDC_OVERLAY, IDC_SYNC, IDC_SYNC, IDC_SYNC}) {
                SendDlgItemMessageW(dialog, id, BM_CLICK, 0, 0);
            }
        }

        for (auto window : windows) {
            RemoveWindowSubclass(window, VisibilityAudit::observe, 4);
        }

        check(audit.observations > 0 && audit.visible && IsWindowVisible(dialog),
            "installer stays visible to input throughout checkbox redraws, including Remove all");
    }
}

void view_repaint_checks() {
    struct Messages {
        int layouts = 0, enables = 0;

        static LRESULT CALLBACK observe(
            HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
            auto counts = reinterpret_cast<Messages*>(data);
            if (message == WM_WINDOWPOSCHANGING) {
                ++counts->layouts;
            } else if (message == WM_ENABLE) {
                ++counts->enables;
            }

            return DefSubclassProc(window, message, wparam, lparam);
        }
    };

    for (auto appearance : {SetupAppearance::Dark, SetupAppearance::Light, SetupAppearance::Contrast}) {
        Preview preview(appearance);
        auto dialog = preview.window;
        auto browse = GetDlgItem(dialog, IDC_BROWSE);
        Messages counts;
        SetWindowSubclass(dialog, Messages::observe, 2, reinterpret_cast<DWORD_PTR>(&counts));
        SetWindowSubclass(browse, Messages::observe, 2, reinterpret_cast<DWORD_PTR>(&counts));
        for (int id : {IDC_OVERLAY, IDC_SYNC, IDC_LAUNCH}) {
            SendDlgItemMessageW(dialog, id, BM_CLICK, 0, 0);
            preview.view.refresh(false);
            SendDlgItemMessageW(dialog, id, BM_CLICK, 0, 0);
            preview.view.refresh(false);
        }

        // Sync toggles disable Browse when no components remain; overlay/launch do not.
        check(counts.layouts == 0 && counts.enables == 2 && IsWindowEnabled(browse),
            "checkbox changes avoid relayout and only change Browse availability when necessary");
        check((GetWindowLongPtrW(dialog, GWL_STYLE) & WS_CLIPCHILDREN) != 0,
            "dialog background redraws exclude child controls");
        RemoveWindowSubclass(browse, Messages::observe, 2);
        RemoveWindowSubclass(dialog, Messages::observe, 2);

        RECT rect{};
        GetClientRect(browse, &rect);
        auto screen = GetDC(browse);
        auto memory = CreateCompatibleDC(screen);
        auto bitmap = CreateCompatibleBitmap(screen, rect.right, rect.bottom);
        auto old = SelectObject(memory, bitmap);
        auto sentinel = RGB(201, 17, 163);
        SetPixelV(memory, 12, 8, sentinel);
        NMCUSTOMDRAW draw{};
        draw.hdr = {browse, IDC_BROWSE, NM_CUSTOMDRAW};
        draw.hdc = memory;
        draw.rc = rect;
        draw.dwDrawStage = CDDS_PREERASE;
        SendMessageW(dialog, WM_NOTIFY, IDC_BROWSE, reinterpret_cast<LPARAM>(&draw));
        check(GetWindowLongPtrW(dialog, DWLP_MSGRESULT) == CDRF_SKIPDEFAULT &&
                  GetPixel(memory, 12, 8) == sentinel,
            "native button erase notifications cannot flash the system background");
        draw.dwDrawStage = CDDS_PREPAINT;
        SendMessageW(dialog, WM_NOTIFY, IDC_BROWSE, reinterpret_cast<LPARAM>(&draw));
        auto panel = appearance == SetupAppearance::Dark    ? RGB(24, 34, 47)
                     : appearance == SetupAppearance::Light ? RGB(255, 255, 255)
                                                            : GetSysColor(COLOR_WINDOW);
        check(
            GetWindowLongPtrW(dialog, DWLP_MSGRESULT) == CDRF_SKIPDEFAULT && GetPixel(memory, 12, 8) == panel,
            "native button state redraws use the styled palette in each appearance");

        auto apply = GetDlgItem(dialog, IDC_APPLY);
        RECT ready{}, finished{}, restored{};
        GetWindowRect(apply, &ready);
        preview.view.finish(true);
        GetWindowRect(apply, &finished);
        preview.view.refresh(true);
        GetWindowRect(apply, &restored);
        check(ready.left != finished.left && EqualRect(&ready, &restored),
            "editing after completion restores the action-row layout");
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(browse, screen);
    }
}

void view_checks() {
    view_input_visibility_checks();
    view_removal_transition_checks();
    view_repaint_checks();
    Preview preview(SetupAppearance::Dark);
    auto dialog = preview.window;
    check(IsDlgButtonChecked(dialog, IDC_SYNC) == BST_CHECKED &&
              IsDlgButtonChecked(dialog, IDC_OVERLAY) == BST_UNCHECKED,
        "polished setup preserves optional-overlay defaults");
    auto overlay = GetDlgItem(dialog, IDC_OVERLAY);
    SendMessageW(overlay, BM_CLICK, 0, 0);
    check(IsDlgButtonChecked(dialog, IDC_OVERLAY) == BST_CHECKED,
        "whole component card retains native checkbox behavior");
    winrt::com_ptr<IAccessible> accessible;
    winrt::check_hresult(AccessibleObjectFromWindow(
        overlay, static_cast<DWORD>(OBJID_CLIENT), IID_IAccessible, accessible.put_void()));
    VARIANT self{};
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    VARIANT role{}, state{};
    winrt::check_hresult(accessible->get_accRole(self, &role));
    winrt::check_hresult(accessible->get_accState(self, &state));
    check(role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_CHECKBUTTON && state.vt == VT_I4 &&
              (state.lVal & STATE_SYSTEM_CHECKED),
        "styled cards expose native accessible checkbox role and checked state");
    BSTR name = nullptr;
    winrt::check_hresult(accessible->get_accName(self, &name));
    bool named = name && std::wstring_view(name).find(L"OBS overlay") != std::wstring_view::npos;
    SysFreeString(name);
    check(named, "styled component keeps its accessible name");
    CheckDlgButton(dialog, IDC_OVERLAY, BST_UNCHECKED);
    SetFocus(overlay);
    SendMessageW(overlay, WM_KEYDOWN, VK_SPACE, 0);
    SendMessageW(overlay, WM_KEYUP, VK_SPACE, 0);
    check(
        IsDlgButtonChecked(dialog, IDC_OVERLAY) == BST_CHECKED, "space key toggles a focused component card");
    RECT hit{};
    GetClientRect(overlay, &hit);
    LPARAM point = MAKELPARAM(hit.right - 12, hit.bottom - 12);
    SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(overlay, WM_LBUTTONUP, 0, point);
    check(IsDlgButtonChecked(dialog, IDC_OVERLAY) == BST_UNCHECKED,
        "clicking the card outside its glyph and title toggles the component");
    preview.view.working(false, false);
    bool locked = true;
    for (int id : {IDC_SYNC, IDC_OVERLAY, IDC_ROOT, IDC_BROWSE, IDC_LAUNCH, IDC_APPLY, IDCANCEL}) {
        locked &= !IsWindowEnabled(GetDlgItem(dialog, id));
    }

    check(locked, "working setup disables actions that could interrupt a transaction");
    preview.view.progress(60, L"Installing the companion");
    check(control_text(dialog, IDC_STATUS) == L"Installing the companion",
        "progress displays the current installation phase");
    preview.view.finish(true);
    check(control_text(dialog, IDC_APPLY) == L"&Done" && IsWindowEnabled(GetDlgItem(dialog, IDC_APPLY)),
        "success gives one enabled finish action");
    CheckDlgButton(dialog, IDC_SYNC, BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_OVERLAY, BST_UNCHECKED);
    preview.view.refresh(true);
    check(control_text(dialog, IDC_APPLY) == L"&Remove all" &&
              IsWindowEnabled(GetDlgItem(dialog, IDC_APPLY)) &&
              !IsWindowEnabled(GetDlgItem(dialog, IDC_ROOT)),
        "maintenance supports full removal and disables irrelevant folder input");
    check(control_text(dialog, IDC_DETAILS).find(L"saved account data") != std::wstring::npos,
        "full-removal consequences are visible before applying");
    preview.view.refresh(false);
    check(
        !IsWindowEnabled(GetDlgItem(dialog, IDC_APPLY)), "fresh setup cannot remove an absent installation");
    CheckDlgButton(dialog, IDC_OVERLAY, BST_CHECKED);
    preview.view.refresh(true, true);
    check(control_text(dialog, IDC_DETAILS).find(L"pending uploads") != std::wstring::npos &&
              IsWindowEnabled(GetDlgItem(dialog, IDC_ROOT)),
        "overlay-only maintenance explains discarded pending uploads");
    preview.view.failure();
    preview.view.refresh(true);
    check(IsWindowEnabled(GetDlgItem(dialog, IDCANCEL)) &&
              control_text(dialog, IDC_STATUS) == L"Ready to apply changes",
        "editing after an error restores ready controls");

    for (UINT dpi : {96u, 144u, 192u}) {
        Preview scaled(SetupAppearance::Light, dpi);
        RECT client{}, card{}, folder{};
        GetClientRect(scaled.window, &client);
        GetWindowRect(GetDlgItem(scaled.window, IDC_SYNC), &card);
        GetWindowRect(GetDlgItem(scaled.window, IDC_ROOT), &folder);
        MapWindowPoints(nullptr, scaled.window, reinterpret_cast<POINT*>(&card), 2);
        MapWindowPoints(nullptr, scaled.window, reinterpret_cast<POINT*>(&folder), 2);
        check(card.left >= 0 && card.right <= client.right && folder.left >= 0 &&
                  folder.right <= client.right && folder.top >= card.bottom,
            "scaled layout keeps component cards and folder input aligned without horizontal clipping");
        scaled.view.message(WM_VSCROLL, SB_BOTTOM, 0);
        RECT action{};
        GetWindowRect(GetDlgItem(scaled.window, IDC_APPLY), &action);
        MapWindowPoints(nullptr, scaled.window, reinterpret_cast<POINT*>(&action), 2);
        scaled.save(executable().parent_path() / (L"setup-light-" + std::to_wstring(dpi) + L".png"));
        if (action.top < 0 || action.bottom > client.bottom) {
            std::cerr << "Layout DPI " << dpi << ", window DPI " << GetDpiForWindow(scaled.window)
                      << ", client " << client.bottom << ", action " << action.top << ".." << action.bottom
                      << '\n';
        }

        check(action.top >= 0 && action.bottom <= client.bottom,
            "short displays can scroll to the complete action row");
    }

    RECT frame{};
    GetWindowRect(dialog, &frame);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST), &monitor);
    auto tall_dpi = static_cast<UINT>(((monitor.rcWork.bottom - monitor.rcWork.top) / 600 + 2) * 96);
    Preview compact(SetupAppearance::Dark, tall_dpi);
    check((GetWindowLongPtrW(compact.window, GWL_STYLE) & WS_VSCROLL) != 0,
        "layout adds scrolling when content exceeds the display height");
    compact.view.message(WM_VSCROLL, SB_BOTTOM, 0);
    RECT client{}, action{};
    GetClientRect(compact.window, &client);
    GetWindowRect(GetDlgItem(compact.window, IDC_APPLY), &action);
    MapWindowPoints(nullptr, compact.window, reinterpret_cast<POINT*>(&action), 2);
    check(action.top >= 0 && action.bottom <= client.bottom,
        "scrolling reaches the action row on an actually constrained display");
    int previous_scroll = GetScrollPos(compact.window, SB_VERT);
    SendMessageW(GetDlgItem(compact.window, IDC_SYNC), WM_SETFOCUS, 0, 0);
    RECT focused{};
    GetWindowRect(GetDlgItem(compact.window, IDC_SYNC), &focused);
    MapWindowPoints(nullptr, compact.window, reinterpret_cast<POINT*>(&focused), 2);
    check(GetScrollPos(compact.window, SB_VERT) < previous_scroll && focused.top >= 0 &&
              focused.bottom <= client.bottom,
        "keyboard focus brings a scrolled-out component back into view");
    Preview transition(SetupAppearance::Dark);
    RECT suggested{};
    GetWindowRect(transition.window, &suggested);
    transition.view.message(WM_DPICHANGED, MAKELONG(144, 144), reinterpret_cast<LPARAM>(&suggested));
    LOGFONTW font{};
    GetObjectW(reinterpret_cast<HFONT>(SendDlgItemMessageW(transition.window, IDC_ROOT, WM_GETFONT, 0, 0)),
        sizeof(font),
        &font);
    check(font.lfHeight == -MulDiv(15, 144, 96) &&
              control_text(transition.window, IDC_ROOT) == L"C:\\XSanity" &&
              IsDlgButtonChecked(transition.window, IDC_SYNC) == BST_CHECKED,
        "DPI changes rebuild fonts and preserve folder and component selection");
    auto small_icon = reinterpret_cast<HICON>(SendMessageW(transition.window, WM_GETICON, ICON_SMALL, 0));
    auto large_icon = reinterpret_cast<HICON>(SendMessageW(transition.window, WM_GETICON, ICON_BIG, 0));
    check(
        icon_size(
            small_icon, GetSystemMetricsForDpi(SM_CXSMICON, 144), GetSystemMetricsForDpi(SM_CYSMICON, 144)) &&
            icon_size(
                large_icon, GetSystemMetricsForDpi(SM_CXICON, 144), GetSystemMetricsForDpi(SM_CYICON, 144)),
        "DPI changes reload both setup title-bar icons at the new scale");
    auto output = executable().parent_path();
    CheckDlgButton(dialog, IDC_SYNC, BST_CHECKED);
    CheckDlgButton(dialog, IDC_OVERLAY, BST_UNCHECKED);
    preview.view.refresh(false);
    preview.save(output / L"setup-dark.png");
    preview.save(output / L"setup-dark-client.png", true);
    CheckDlgButton(dialog, IDC_SYNC, BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_OVERLAY, BST_UNCHECKED);
    preview.view.refresh(true);
    preview.save(output / L"setup-remove.png");
    CheckDlgButton(dialog, IDC_SYNC, BST_CHECKED);
    CheckDlgButton(dialog, IDC_OVERLAY, BST_CHECKED);
    preview.view.refresh(true);
    preview.view.working(false, true);
    preview.view.progress(60, L"Installing the companion");
    preview.save(output / L"setup-progress.png");
    preview.view.finish(true);
    preview.save(output / L"setup-complete.png");
    preview.view.refresh(true);
    preview.view.failure();
    preview.save(output / L"setup-error.png");
    Preview contrast(SetupAppearance::Contrast);
    contrast.save(output / L"setup-contrast.png");
}

void progress_checks() {
    Fixture fixture("progress");
    auto game = fixture.game();
    FakeHost host;
    InstallPaths paths{fixture.root / L"app", fixture.root / L"state", fixture.root / L"menu"};
    Installer installer(paths, host, GameHook{}, "app", "setup", "license");
    std::vector<int> phases;
    std::vector<std::wstring> statuses;
    auto progress = [&](int percent, std::wstring_view status) {
        phases.push_back(percent);
        statuses.emplace_back(status);
    };
    installer.apply({game, true, false}, progress);
    check(phases.front() == 0 && phases.back() == 100 && std::is_sorted(phases.begin(), phases.end()) &&
              phases.size() > 3,
        "installation reports ordered real phases and completion");
    installer.apply({game, true, true}, [](int, std::wstring_view) {
        throw Error("UI observer failure");
    });
    check(installer.selection().overlay, "a failed progress observer cannot undo a successful installation");
    host.fail_at = host.mutations + 3;
    phases.clear();
    statuses.clear();
    rejects(
        [&] {
            installer.apply({game, false, true}, progress);
        },
        "failed maintenance still reports an operation error");
    check(std::find(statuses.begin(), statuses.end(), L"Restoring the previous installation") !=
                  statuses.end() &&
              std::find(phases.begin(), phases.end(), 100) == phases.end() && installer.selection().sync,
        "rollback reports restoration without announcing completion");
    host.fail_at = -1;
    phases.clear();
    installer.apply({game, false, false}, progress);
    check(!installer.installed() && phases.front() == 0 && phases.back() == 100,
        "full removal reports completion after all components are removed");
}

int ipc_companion(const std::wstring& prefix) {
    auto mutex_name = L"Local\\" + prefix + L".Mutex", event_name = L"Local\\" + prefix + L".Stop",
         ready_name = L"Local\\" + prefix + L".Ready";
    Handle mutex(CreateMutexW(nullptr, TRUE, mutex_name.c_str()));
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name.c_str()));
    if (!mutex.get() || !stop.get() || !ready.get()) {
        fail("Create isolated IPC fixture");
    }

    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = prefix.c_str();
    type.lpfnWndProc = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
        if (message == WM_CREATE) {
            SetWindowLongPtrW(window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(
                    static_cast<CREATESTRUCTW*>(reinterpret_cast<void*>(lparam))->lpCreateParams));
        }

        if (message == WM_TIMER &&
            WaitForSingleObject(reinterpret_cast<HANDLE>(GetWindowLongPtrW(window, GWLP_USERDATA)), 0) ==
                WAIT_OBJECT_0) {
            DestroyWindow(window);
        }

        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(window, message, wparam, lparam);
    };
    if (!RegisterClassW(&type)) {
        fail("Register isolated IPC window");
    }

    HWND window = CreateWindowExW(
        0, prefix.c_str(), L"Test companion", 0, 0, 0, 0, 0, nullptr, nullptr, type.hInstance, stop.get());
    if (!window) {
        fail("Create isolated IPC window");
    }

    SetTimer(window, 1, 20, nullptr);
    SetEvent(ready.get());
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    ReleaseMutex(mutex.get());
    Sleep(300);
    return 0; // Simulate an executable still mapped after mutex release.
}

void ipc_checks() {
    Fixture fixture("ipc");
    auto copy = fixture.root / L"isolated-companion.exe";
    fs::copy_file(executable(), copy);
    auto prefix = L"PiuCompanion.Checks." + std::to_wstring(GetCurrentProcessId());
    auto ready_name = L"Local\\" + prefix + L".Ready";
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
    std::wstring command = L"\"" + copy.wstring() + L"\" --ipc-companion " + prefix;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(copy.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        fail("Start isolated IPC fixture");
    }

    Handle child(process.hProcess);
    CloseHandle(process.hThread);
    check(WaitForSingleObject(ready.get(), 10000) == WAIT_OBJECT_0,
        "isolated companion IPC fixture becomes ready");
    WindowsInstallHost host(L"Local\\" + prefix + L".Mutex", L"Local\\" + prefix + L".Stop", prefix);
    host.stop_companion();
    check(WaitForSingleObject(child.get(), 0) == WAIT_OBJECT_0,
        "maintenance waits for full process exit after singleton release");
    atomic_write(copy, "replacement executable");
    check(read(copy) == "replacement executable",
        "companion executable can be replaced immediately after maintenance stop");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        if (argc == 2 && std::wstring_view(argv[1]) == L"--self-delete") {
            clean_maintenance_after_exit(executable().parent_path());
            return 0;
        }

        if (argc == 3 && std::wstring_view(argv[1]) == L"--ipc-companion") {
            return ipc_companion(argv[2]);
        }

        if (argc == 2 && std::wstring_view(argv[1]) == L"--ui") {
            view_checks();
            std::cout << passed << " installer UI checks passed.\n";
            return 0;
        }

        icon_checks();
        engine_checks();
        api_checks();
        installer_checks();
        installer_worker_checks();
        hook_upgrade_checks();
        overlay_checks();
        ipc_checks();
        Fixture self("self-delete", true);
        auto copy = self.root / L"maintenance.exe";
        fs::copy_file(executable(), copy);
        std::wstring command = L"\"" + copy.wstring() + L"\" --self-delete";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(copy.c_str(),
                command.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process)) {
            fail("Start self-cleanup test");
        }

        Handle child(process.hProcess);
        CloseHandle(process.hThread);
        auto waited = WaitForSingleObject(child.get(), 10000);
        DWORD code = 0;
        GetExitCodeProcess(child.get(), &code);
        if (code) {
            throw Error("Maintenance self-cleanup returned Windows error " + std::to_string(code));
        }

        auto deadline = GetTickCount64() + 15000;
        while (fs::exists(self.root) && GetTickCount64() < deadline) {
            Sleep(50);
        }

        check(waited == WAIT_OBJECT_0 && !fs::exists(self.root),
            "maintenance cleanup removes its copy, script and private folder after exit");
        rejects(
            [&] {
                clean_maintenance_after_exit(fs::temp_directory_path());
            },
            "cleanup refuses the general temp folder");
        check(!is_maintenance_directory(
                  L"\\\\server\\share\\PiuCompanionSetup-{00000000-0000-0000-0000-000000000000}"),
            "cleanup refuses a network working directory");
        Fixture foreign("foreign-cleanup", true);
        atomic_write(foreign.root / L"maintenance.exe", "fake maintenance copy");
        atomic_write(foreign.root / L"keep.txt", "unrelated");
        clean_maintenance_after_exit(foreign.root);
        deadline = GetTickCount64() + 10000;
        while (fs::exists(foreign.root / L"cleanup.cmd") && GetTickCount64() < deadline) {
            Sleep(50);
        }

        check(
            !fs::exists(foreign.root / L"maintenance.exe") && read(foreign.root / L"keep.txt") == "unrelated",
            "maintenance cleanup leaves unrelated files untouched");
        view_checks();
        progress_checks();
        std::cout << passed << " behavioral checks passed. Fake API and game folders only.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const winrt::hresult_error& error) {
        std::cerr << utf8(error.message().c_str()) << '\n';
        return 1;
    }
}
