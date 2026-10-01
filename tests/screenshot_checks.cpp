#include "support.h"
#include "engine.h"
#include "preview.h"

namespace piu::test {
namespace {
struct ScreenshotApi : Api {
    std::vector<Score> records;
    std::vector<Play> posts;
    bool offline = false;

    std::vector<Chart> catalog(const std::string&, const std::string&) override {
        return {{"chart", "Test Song!", "Single", 10}};
    }

    std::vector<Score> scores(const std::string&, const std::string&) override {
        if (offline) {
            throw Error("offline");
        }
        return records;
    }

    void upload(const std::string&, const std::string&, const Play& play) override {
        posts.push_back(play);
    }
};

size_t files(const fs::path& folder, const fs::path& extension) {
    size_t count = 0;
    if (fs::exists(folder)) {
        for (const auto& entry : fs::directory_iterator(folder)) {
            count += entry.path().extension() == extension;
        }
    }
    return count;
}

void native_capture_check(const fs::path& folder) {
    if (!previews) {
        return;
    }
    auto window = CreateWindowExW(0,
        L"STATIC",
        L"PIU screenshot test",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        40,
        40,
        400,
        260,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    check(window != nullptr, "native capture fixture opens");

    struct CloseWindow {
        HWND value;

        ~CloseWindow() {
            DestroyWindow(value);
        }
    } close{window};

    UpdateWindow(window);
    auto png = capture_window_png(window);
    check(png.starts_with("\x89PNG\r\n\x1a\n"), "Windows Graphics Capture returns an encoded PNG");
    auto path = folder / L"native-capture.png";
    atomic_write(path, png);
    auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory, CLSCTX_INPROC_SERVER);
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::check_hresult(factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.put()));
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::check_hresult(decoder->GetFrame(0, frame.put()));
    UINT width = 0, height = 0;
    winrt::check_hresult(frame->GetSize(&width, &height));
    RECT client{};
    GetClientRect(window, &client);
    check(width == static_cast<UINT>(client.right) && height == static_cast<UINT>(client.bottom),
        "native capture crops to the game client, excluding desktop and title bar");
    atomic_write(executable().parent_path() / L"native-capture.png", png);
}
} // namespace

void screenshot_checks() {
    Fixture fixture("screenshots");
    auto state = fixture.root / L"state";
    auto game = fixture.game();
    auto config = load_preferences(state);
    check(!config.screenshots.enabled && !config.accounts[0].include_failed &&
              !config.accounts[1].include_failed && config.screenshots.folder == state / L"Screenshots",
        "new and upgraded users default to both options off");
    config.game_root = game;
    config.accounts[0] = {"Test Player", protect("test-key")};
    config.accounts[1] = {"Friend", protect("friend-key"), true};
    save_preferences(state, config);
    ScreenshotApi api;
    Engine engine(state, api);
    engine.load();
    auto failed = result("failed-default", 700000, true);
    engine.capture(failed, now());
    engine.sync();
    check(api.posts.empty() && engine.store().receipts.at(failed.id) == "failed_disabled",
        "failed score is skipped by default even with no website PB");
    auto friend_fail = failed;
    friend_fail.id = "friend-failed";
    friend_fail.profile = "Friend";
    friend_fail.side = 2;
    engine.capture(friend_fail, now());
    engine.sync();
    check(api.posts.size() == 1 && api.posts[0].broken, "only the opted-in player's failed PB uploads");
    auto invalid = friend_fail;
    invalid.id = "friend-autoplay";
    invalid.eligible = false;
    invalid.skip_reason = "autoplay";
    engine.capture(invalid, now());
    engine.sync();
    check(api.posts.size() == 1, "failed-PB opt-in cannot bypass eligibility checks");
    api.records = {{"chart", 600000, true}};
    friend_fail.id = "failed-improvement";
    engine.capture(friend_fail, now());
    engine.sync();
    check(api.posts.size() == 2, "higher failed PB improves a failed website best");
    api.records = {{"chart", 500000, false}};
    friend_fail.id = "failed-after-clear";
    engine.capture(friend_fail, now());
    engine.sync();
    check(api.posts.size() == 2, "failed PB never replaces a clear, even with a higher number");

    auto output = fixture.root / L"Images é";
    ScreenshotOptions options{true, output};
    int captures = 0;
    auto capture = [&](const fs::path&) {
        ++captures;
        return std::string("test-png");
    };
    auto first = result("screen-P1");
    auto second = result("screen-P2");
    second.side = 2;
    second.profile = "Friend";
    Screenshots shots(state, capture);
    shots.prepare({first, second}, options, now());
    shots.update(game, {first, second}, true, 1);
    check(captures == 0, "capture waits for the result animation");
    shots.update(game, {first, second}, true, 3);
    shots.update(game, {first, second}, true, 4);
    check(captures == 1 && files(output, L".png") == 0, "one provisional image covers both players");
    shots.decide(first.id, false);
    shots.decide(second.id, true);
    check(files(output, L".png") == 1 && files(state / L"pending-shots", L".json") == 0,
        "either player's PB keeps exactly one image and removes its staging metadata");
    first.id = "not-pb";
    shots.prepare({first}, options, now());
    shots.update(game, {first}, true, 3);
    shots.decide(first.id, false);
    check(files(output, L".png") == 1 && files(state / L"pending-shots", L".png") == 0,
        "a non-PB discards its provisional image");
    first.id = "fast-upload";
    shots.prepare({first}, options, now());
    shots.decide(first.id, true);
    shots.update(game, {first}, true, 3);
    check(files(output, L".png") == 2, "PB confirmation before capture still saves the later results frame");
    first.id = "offline";
    shots.prepare({first}, options, now());
    shots.update(game, {first}, true, 3);
    Store queue;
    queue.pending.push_back({first, "Phoenix", iso_time(now())});
    Screenshots restarted(state, capture);
    restarted.load(queue, true);
    restarted.decide(first.id, true);
    check(files(output, L".png") == 3,
        "provisional images survive restart while website comparison is offline");
    first.id = "left-results";
    shots.prepare({first}, options, now());
    auto before = captures;
    shots.update(game, {first}, false, 4);
    check(captures == before && !shots.error().empty(),
        "leaving results prevents capturing another game screen");
    rejects(
        [&] {
            validate_screenshot_folder(game / L"Screenshots", game);
        },
        "output cannot be inside the game");
    rejects(
        [&] {
            validate_screenshot_folder(L"relative", game);
        },
        "relative output paths are rejected");

    // Exercise the engine's real ordering: capture candidate, compare PB, then capture image.
    auto live_state = fixture.root / L"engine";
    config.screenshots = {true, fixture.root / L"Engine shots"};
    save_preferences(live_state, config);
    api.records.clear();
    api.posts.clear();
    auto event = result("engine-screen");
    Object packet;
    packet.Insert(L"current", Object{});
    Array results;
    results.Append(result_json(event));
    packet.Insert(L"results", results);
    packet.Insert(L"completed", Value::CreateNumberValue(100));
    put(packet, L"screen", "ScreenEvaluation");
    double clock = 100;
    auto stamp = now();
    Engine integrated(
        live_state,
        api,
        [&](const fs::path&) -> std::optional<ExportFrame> {
            return ExportFrame{
                encode(packet), clock, 2, stamp + static_cast<uint64_t>((clock - 100) * 10000000)};
        },
        capture);
    integrated.load();
    integrated.poll();
    integrated.sync();
    clock = 104;
    integrated.poll();
    integrated.poll();
    check(api.posts.size() == 1 && files(config.screenshots.folder, L".png") == 1,
        "actual engine flow saves one screenshot even when upload finishes before capture");

    auto error_state = fixture.root / L"capture-error";
    save_preferences(error_state, config);
    Engine broken_capture(
        error_state,
        api,
        [&](const fs::path&) -> std::optional<ExportFrame> {
            return ExportFrame{encode(packet), 104, 2, stamp + 40000000};
        },
        [](const fs::path&) -> std::string {
            throw Error("capture failed");
        });
    broken_capture.load();
    broken_capture.poll();
    broken_capture.sync();
    check(api.posts.size() == 2 && !broken_capture.screenshot_status().empty(),
        "native capture failure is visible but cannot prevent a valid upload");
    first.id = "transition-during-capture";
    shots.prepare({first}, options, now());
    shots.update(game, {first}, true, 3, [] {
        return false;
    });
    check(!shots.error().empty() && files(state / L"pending-shots", L".png") == 0,
        "a screen transition during native capture discards the image");
    first.id = "changed-folder";
    shots.prepare({first}, options, now());
    shots.update(game, {first}, true, 3);
    auto moved_options = ScreenshotOptions{true, fixture.root / L"New output"};
    shots.configure(moved_options);
    shots.decide(first.id, true);
    check(files(moved_options.folder, L".png") == 1, "changing destination also redirects pending images");
    first.id = "disabled";
    shots.prepare({first}, options, now());
    shots.update(game, {first}, true, 3);
    shots.configure({false, output});
    check(files(state / L"pending-shots", L".png") == 0 && files(output, L".png") == 3,
        "disabling screenshots clears provisional files and preserves saved images");

    friend_fail.id = "disabled-while-queued";
    api.records.clear();
    engine.capture(friend_fail, now());
    engine.configure(
        {AccountInput{"Test Player", "test-key"}, AccountInput{"Friend", "friend-key", false}}, "Phoenix");
    auto posts_before = api.posts.size();
    engine.sync();
    check(api.posts.size() == posts_before && engine.store().receipts.at(friend_fail.id) == "failed_disabled",
        "turning off failed PBs also skips unsent failed results already queued");
    native_capture_check(fixture.root);
}
} // namespace piu::test
