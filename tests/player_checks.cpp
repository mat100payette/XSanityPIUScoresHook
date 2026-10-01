#include "support.h"
#include "account_view.h"
#include "preview.h"

namespace piu::test {
namespace {
struct PlayerApi : Api {
    std::vector<std::string> recipients;
    bool fail_alice = false;

    std::vector<Chart> catalog(const std::string&, const std::string&) override {
        return {{"chart-id", "Test Song", "Single", 10}};
    }

    std::vector<Score> scores(const std::string& token, const std::string&) override {
        return {{"chart-id", token == "alice-key" ? 980000 : 800000, false}};
    }

    void upload(const std::string& token, const std::string&, const Play&) override {
        if (token == "alice-key" && fail_alice) {
            throw HttpError(401);
        }

        recipients.push_back(token);
    }
};

Result play(const char* id, const char* profile, int side, int score = 950000) {
    auto value = result(id, score);
    value.profile = profile;
    value.side = side;
    return value;
}

void preview_accounts(HWND dialog, const fs::path& path) {
    if (!previews) {
        return;
    }

    ShowWindow(dialog, SW_SHOWNOACTIVATE);
    UpdateWindow(dialog);
    save_window_preview(dialog, path, true, true);
    ShowWindow(dialog, SW_HIDE);
}

void check_account_layout(HWND dialog) {
    RECT client{};
    GetClientRect(dialog, &client);
    bool contained = true;
    for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT bounds{};
        GetWindowRect(child, &bounds);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&bounds), 2);
        contained &= bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right &&
                     bounds.bottom <= client.bottom;
    }

    check(contained, "native dialog scaling keeps every account control inside the client area");
}
} // namespace

void player_checks() {
    Fixture fixture("players");
    auto state = fixture.root / L"state";
    Preferences config;
    config.game_root = fixture.game();
    config.accounts = {
        PlayerAccount{"Alice", protect("alice-key")}, PlayerAccount{"Bob", protect("bob-key")}};
    save_preferences(state, config);
    PlayerApi api;
    Object packet;
    packet.Insert(L"current", parse(R"({"playing":false})"));
    Engine engine(state, api, [&](const fs::path&) -> std::optional<ExportFrame> {
        return ExportFrame{encode(packet), 100, 2, now()};
    });
    engine.load();
    engine.capture(play("a", " ALICE ", 2), now());
    engine.capture(play("b", "Bob", 1), now());
    engine.sync();
    check(api.recipients == std::vector<std::string>{"bob-key"},
        "each PB comparison uses that account's website scores");
    check(engine.store().receipts.at("a") == "covered" && engine.store().receipts.at("b") == "accepted",
        "swapped sides preserve independent result receipts");
    engine.capture(play("guest", "", 1), now());
    engine.capture(play("unknown", "Other", 1), now());
    engine.capture(play("punctuation", "A-lice", 1), now());
    check(engine.store().pending.empty(), "blank, unknown and loosely similar names are never guessed");

    auto duplicate = play("duplicate", "Alice", 1);
    duplicate.ambiguous = true;
    engine.capture(duplicate, now());
    check(engine.store().pending.empty(), "ambiguous profile results do not queue");
    Array results;
    results.Append(result_json(play("same1", "Alice", 1)));
    results.Append(result_json(play("same2", "aLiCe", 2)));
    packet.Insert(L"results", results);
    put(packet, L"completed", 100);
    engine.poll();
    check(
        engine.store().pending.empty(), "case-insensitive duplicate names across sides cannot double-upload");

    engine.capture(play("queued1", "Alice", 2, 990000), now());
    engine.capture(play("queued2", "Bob", 1, 990000), now());
    api.fail_alice = true;
    engine.sync();
    check(engine.pending_count(0) == 1 && engine.pending_count(1) == 0 && api.recipients.back() == "bob-key",
        "one rejected credential does not block the other player's upload");
    check(engine.status().find("Account 1:") != std::string::npos,
        "the second account's success cannot hide the first account's upload error");
    rejects(
        [&] {
            engine.configure({AccountInput{"Alice", "new-key"}, AccountInput{"Bob", "bob-key"}}, "Phoenix");
        },
        "pending results cannot move to a replacement key");
    rejects(
        [&] {
            engine.configure({AccountInput{"Other", "alice-key"}, AccountInput{"Bob", "bob-key"}}, "Phoenix");
        },
        "pending results cannot move to a replacement profile");
    engine.capture(play("queued3", "Bob", 1), now());
    engine.discard_pending(0);
    check(engine.pending_count(0) == 0 && engine.pending_count(1) == 1 &&
              engine.store().receipts.at("queued1") == "discarded",
        "explicit discard affects only the selected account and prevents re-import");
    engine.capture(play("queued1", "Alice", 2, 990000), now());
    check(engine.pending_count(0) == 0, "discarded snapshot is not queued again");
    engine.discard_pending(1);
    rejects(
        [&] {
            engine.configure({AccountInput{"Alice", "a"}, AccountInput{" alice ", "b"}}, "Phoenix");
        },
        "settings reject ambiguous duplicate names");
    rejects(
        [&] {
            engine.configure({AccountInput{"Alice", "same"}, AccountInput{"Bob", "same"}}, "Phoenix");
        },
        "settings catch an accidentally repeated key");
    rejects(
        [&] {
            engine.configure({AccountInput{"Alice", ""}, AccountInput{}}, "Phoenix");
        },
        "an account needs a profile and key together");

    auto dialog = CreateDialogParamW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDD_ACCOUNT),
        nullptr,
        [](HWND, UINT, WPARAM, LPARAM) -> INT_PTR {
            return FALSE;
        },
        0);
    check(dialog != nullptr, "native account settings dialog loads");

    struct CloseDialog {
        HWND window;

        ~CloseDialog() {
            DestroyWindow(window);
        }
    } close{dialog};

    initialize_accounts(dialog, engine);
    check(control_text(dialog, IDC_PROFILE1) == L"Alice" && control_text(dialog, IDC_PROFILE2) == L"Bob" &&
              control_text(dialog, IDC_TOKEN) == L"alice-key" &&
              control_text(dialog, IDC_TOKEN2) == L"bob-key",
        "settings populate both saved name/key pairs in the correct controls");
    check((GetWindowLongPtrW(GetDlgItem(dialog, IDC_TOKEN), GWL_STYLE) & ES_PASSWORD) &&
              (GetWindowLongPtrW(GetDlgItem(dialog, IDC_TOKEN2), GWL_STYLE) & ES_PASSWORD),
        "both API key fields are masked");
    SetDlgItemTextW(dialog, IDC_PROFILE1, L" Alice ");
    SetDlgItemTextW(dialog, IDC_TOKEN, L"new-alice-key");
    save_accounts(dialog, engine);
    Engine saved(state, api);
    saved.load();
    check(saved.token() == "new-alice-key" && saved.token(1) == "bob-key" &&
              saved.config().accounts[0].profile == "Alice",
        "saving and reopening settings preserves the two distinct key associations");
    auto bytes = read(state / L"settings.json");
    check(bytes.find("new-alice-key") == std::string::npos && bytes.find("bob-key") == std::string::npos,
        "both account keys are encrypted on disk");
    check_account_layout(dialog);
    preview_accounts(dialog, executable().parent_path() / L"accounts.png");
    {
        // Compare native dialog-unit layout at 96 DPI with the current per-monitor scale above.
        auto previous_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);

        struct RestoreDpi {
            DPI_AWARENESS_CONTEXT previous;

            ~RestoreDpi() {
                SetThreadDpiAwarenessContext(previous);
            }
        } restore{previous_dpi};

        auto standard = CreateDialogParamW(
            GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDD_ACCOUNT),
            nullptr,
            [](HWND, UINT, WPARAM, LPARAM) -> INT_PTR {
                return FALSE;
            },
            0);
        check(standard != nullptr && GetDpiForWindow(standard) == 96,
            "account dialog supports 100 percent scaling");
        CloseDialog close_standard{standard};
        initialize_accounts(standard, saved);
        check_account_layout(standard);
        preview_accounts(standard, executable().parent_path() / L"accounts-96.png");
    }

    auto legacy = fixture.root / L"legacy";
    Object old;
    put(old, L"ProtectedToken", protect("old-key"));
    atomic_write(legacy / L"settings.json", encode(old));
    Engine upgraded(legacy, api);
    upgraded.load();
    check(upgraded.token() == "old-key" && upgraded.config().accounts[0].profile.empty(),
        "upgrade preserves the old key without inventing a profile association");
    check(upgraded.status().find("Add the XSanity profile") != std::string::npos,
        "a migrated account explains why new scores cannot sync yet");
    upgraded.capture(play("unassigned", "Alice", 1), now());
    check(upgraded.store().pending.empty(),
        "legacy account waits for an explicit profile before capturing new scores");
    Store old_queue;
    old_queue.pending.push_back({result("old-result"), "Phoenix", iso_time(now()), "queued"});
    save_store(legacy, old_queue);
    upgraded.load();
    upgraded.configure({AccountInput{"Alice", "old-key"}, AccountInput{}}, "Phoenix");
    check(upgraded.pending_count(0) == 1 && upgraded.token() == "old-key",
        "binding a migrated profile preserves the original key and queued scores");
    auto before = read(legacy / L"settings.json");
    upgraded.configure({AccountInput{"Alice", "old-key"}, AccountInput{}}, "Phoenix");
    check(read(legacy / L"settings.json") == before,
        "saving unchanged accounts preserves the capture cutoff and persisted settings");
}
} // namespace piu::test
