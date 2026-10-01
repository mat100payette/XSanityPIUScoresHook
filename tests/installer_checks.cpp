#include "support.h"
#include "install.h"
#include <algorithm>

namespace piu::test {
void rename_fixture(const fs::path& source, const fs::path& destination) {
    auto deadline = GetTickCount64() + 1500;
    std::error_code error;
    do {
        fs::rename(source, destination, error);
        if (!error) {
            return;
        }

        if (error.value() != ERROR_ACCESS_DENIED && error.value() != ERROR_SHARING_VIOLATION) {
            break;
        }

        Sleep(25);
    } while (GetTickCount64() < deadline);
    throw fs::filesystem_error("Move isolated fixture", source, destination, error);
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
    auto owned = [&](const fs::path& path) {
        return hook.owned(path, str(parse(read(paths.app / L"installation.json")), L"hookSha256"));
    };
    installer.apply({game, true, false});
    auto config = load_preferences(paths.state);
    check(installer.installed() && config.sync && !config.overlay && owned(GameHook::layer(game)),
        "fresh sync-only installation creates shared layer");
    check(hook.configured(true, false).current(layer) &&
              str(parse(read(paths.app / L"installation.json")), L"hookSha256") ==
                  hook.configured(true, false).fingerprint(),
        "fresh setup installs the current exporter and records its fingerprint");
    check(host.values && fs::exists(paths.menu / L"Play XSanity.lnk") &&
              fs::exists(paths.menu / L"Manage installation.lnk"),
        "setup registers maintenance and simultaneous launch shortcuts");
    auto preferences = config;
    preferences.accounts = {
        PlayerAccount{"Alice", protect("existing-token")}, PlayerAccount{"Bob", protect("second-token")}};
    save_preferences(paths.state, preferences);
    installer.apply({game, true, true});
    config = load_preferences(paths.state);
    check(config.sync && config.overlay && config.accounts[0].profile == "Alice" &&
              config.accounts[1].profile == "Bob" &&
              unprotect(config.accounts[0].protected_token) == "existing-token" &&
              unprotect(config.accounts[1].protected_token) == "second-token",
        "component maintenance preserves both encrypted profile/key associations");
    host.running = true;
    rejects(
        [&] {
            installer.apply({game, true, false});
        },
        "component changes require the game closed before changing capture behavior");
    host.running = false;
    installer.apply({game, true, false});
    check(!load_preferences(paths.state).overlay && owned(GameHook::layer(game)),
        "remove overlay and keep the syncing layer");
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
    host.running = true;
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
    check(!fs::exists(GameHook::layer(game)) && owned(GameHook::layer(game2)),
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
              read(paths.app / L"installation.json") == marker_before && owned(GameHook::layer(game)),
        "failed update restores settings, marker and shared layer");
    host.fail_at = -1;
    host.fail_at = host.mutations + 4;
    rejects(
        [&] {
            installer.apply({game, false, false});
        },
        "uninstall registration failure triggers rollback");
    check(installer.installed() && host.values && owned(GameHook::layer(game)) &&
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
    atomic_write(GameHook::layer(game), hook.configured(false, true).source());
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
    fs::create_directory(GameHook::layer(duplicate_layer).parent_path() / L"ScreenSystemLayer overlay");
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
        rename_fixture(game, destination);
        game = destination;
    }

    void seed() {
        auto preferences = load_preferences(paths.state);
        preferences.mix = "Phoenix2";
        preferences.accounts[0].protected_token = protect("upgrade-account-token");
        preferences.capture_after = 1;
        save_preferences(paths.state, preferences);

        Store queue;
        queue.pending.push_back({result("queued-before-upgrade"), "Phoenix2", iso_time(now()), "queued"});
        queue.receipts.emplace("previous-upload", "accepted");
        save_store(paths.state, queue);
        atomic_write(GameHook::exports(game) / L"current.json", encode(result_json(result("old-current"))));
        atomic_write(GameHook::exports(game) / L"result.json", encode(result_json(result("old-result"))));
        atomic_write(GameHook::exports(game) / L"status.txt", "PIUCOMPANION 1\nold-result\taccepted\n");
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

void hook_entrypoint_checks() {
    const GameHook first("return Def.ActorFrame {} -- synthetic installed exporter\n");
    const auto latest = GameHook{}.configured(true, false);
    for (int location : {0, 1, 2}) {
        HookUpgradeFixture fixture(("hook-entrypoint-" + std::to_string(location)).c_str());
        fixture.installer(first).apply({fixture.game, true, false});
        fixture.seed();
        fs::remove(GameHook::exports(fixture.game) / L"keep.txt");
        auto old_root = fixture.game;
        auto marker_path = fixture.paths.app / L"installation.json";
        auto auxiliary = GameHook::layer(fixture.game).parent_path() / L"ScreenSystemLayer aux.lua";
        rename_fixture(GameHook::layer(fixture.game), auxiliary);
        auto marker = parse(read(marker_path));
        marker.Remove(L"hookFile");
        atomic_write(marker_path, encode(marker));
        if (location == 1) {
            fixture.move_game();
        } else if (location == 2) {
            auto copied = fixture.files.root / L"copied game";
            fs::copy(fixture.game, copied, fs::copy_options::recursive);
            fixture.game = copied;
        }

        auxiliary = GameHook::layer(fixture.game).parent_path() / L"ScreenSystemLayer aux.lua";
        auto fallback = fixture.game / L"Themes" / L"_fallback" / L"BGAnimations" /
                        L"ScreenSystemLayer overlay" / L"default.lua";
        auto builtin = read(fallback);
        auto queue = read(fixture.paths.state / L"uploads.json");
        auto token = load_preferences(fixture.paths.state).accounts[0].protected_token;
        auto setup = fixture.installer(latest);

        atomic_write(auxiliary, first.source() + "-- owner edit\n");
        auto modified_files = snapshot_files(fixture.files.root);
        rejects(
            [&] {
                setup.apply({fixture.game, true, false});
            },
            "entrypoint upgrade refuses a modified auxiliary actor, including moved and copied games");
        check(snapshot_files(fixture.files.root) == modified_files,
            "entrypoint conflict leaves every game and installation file untouched");
        atomic_write(auxiliary, first.source());

        auto before = snapshot_files(fixture.files.root);
        fixture.host.fail_at = fixture.host.mutations + 2;
        rejects(
            [&] {
                setup.apply({fixture.game, true, false});
            },
            "entrypoint upgrade rolls back if replacing its actor fails");
        check(snapshot_files(fixture.files.root) == before,
            "failed entrypoint upgrade restores the recorded actor and receipt");
        fixture.host.fail_at = -1;

        setup.apply({fixture.game, true, false});
        auto updated = parse(read(marker_path));
        check(!fs::exists(GameHook::exports(fixture.game)),
            "memory bridge removes the empty obsolete export directory");
        check(latest.current(GameHook::layer(fixture.game)) && !fs::exists(auxiliary) &&
                  !fs::exists(GameHook::layer(old_root).parent_path() / L"ScreenSystemLayer aux.lua") &&
                  str(updated, L"hookFile") == utf8(GameHook::LayerName),
            "entrypoint upgrade installs the loaded overlay and removes only receipt-owned auxiliary actors");
        check(read(fallback) == builtin && read(fixture.paths.state / L"uploads.json") == queue &&
                  load_preferences(fixture.paths.state).accounts[0].protected_token == token,
            "entrypoint upgrade preserves the built-in overlay, account, and pending uploads");
        setup.apply({fixture.game, false, false});
        check(!fs::exists(GameHook::layer(fixture.game)) && read(fallback) == builtin,
            "removing the companion restores the inherited system overlay without editing it");
    }

    HookUpgradeFixture invalid("hook-entrypoint-invalid");
    invalid.installer(first).apply({invalid.game, true, false});
    auto marker_path = invalid.paths.app / L"installation.json";
    auto marker = parse(read(marker_path));
    put(marker, L"hookFile", "../unrelated.lua");
    atomic_write(marker_path, encode(marker));
    auto before = snapshot_files(invalid.files.root);
    rejects(
        [&] {
            invalid.installer(latest).apply({invalid.game, true, false});
        },
        "receipt filenames cannot redirect setup to an unrelated file");
    check(snapshot_files(invalid.files.root) == before,
        "invalid receipt filename is rejected before any mutation");
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
    std::jthread background([&] {
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
        auto token = load_preferences(moved.paths.state).accounts[0].protected_token;
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
        check(config.sync && config.overlay && config.mix == "Phoenix2" &&
                  config.accounts[0].protected_token == token &&
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
    config.accounts[0].protected_token = protect("copy-account-token");
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
              relocated.mix == config.mix &&
              relocated.accounts[0].protected_token == config.accounts[0].protected_token &&
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
    auto token = load_preferences(sequential.paths.state).accounts[0].protected_token;
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
    check(upgraded.capture_after > 1 && upgraded.mix == "Phoenix2" &&
              upgraded.accounts[0].protected_token == token &&
              read(sequential.paths.state / L"uploads.json") == queued,
        "exporter replacement resets cutoff while preserving account, settings and pending uploads");
    check(!fs::exists(GameHook::exports(sequential.game) / L"current.json") &&
              !fs::exists(GameHook::exports(sequential.game) / L"result.json") &&
              !fs::exists(GameHook::exports(sequential.game) / L"status.txt") &&
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

    hook_entrypoint_checks();
    hook_receipt_checks();
    hook_race_checks();
    hook_relocation_checks();
    hook_rollback_checks(false);
    hook_rollback_checks(true);
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

} // namespace piu::test
