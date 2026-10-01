#include "support.h"
#include "mailbox.h"
#include "engine.h"
#include "game_hook.h"
#include <cmath>
#include <limits>

using namespace piu;

namespace piu::test {
namespace {
MailboxBlock block(std::string_view text) {
    MailboxBlock data{};
    for (auto& slot : data) {
        slot.type = 3;
    }

    std::array<double, 9> header{204081632653,
        918273645546,
        672345891234,
        135792468013,
        1,
        1024,
        2,
        static_cast<double>(text.size()),
        100};
    for (size_t i = 0; i < header.size(); ++i) {
        data[i].value = header[i];
    }

    for (size_t i = 0; i < text.size(); ++i) {
        data[9 + i / 6].value += static_cast<double>(static_cast<unsigned char>(text[i])) *
                                 std::pow(256.0, static_cast<double>(i % 6));
    }

    return data;
}

void live_checks() {
    auto repo = executable().parent_path().parent_path().parent_path();
    auto folder = fs::temp_directory_path() / (L"piu-mailbox-test-" + std::to_wstring(GetCurrentProcessId()));
    if (fs::exists(folder)) {
        throw Error("Mailbox test directory already exists.");
    }

    struct Cleanup {
        fs::path path;

        ~Cleanup() {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{folder};

    auto program = folder / L"Program64";
    fs::create_directories(program);
    auto game = program / L"XSanity.exe";
    fs::copy_file(repo / L"build/tools/lua-5.1.5/lua.exe", game);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE input_read = nullptr, input_write = nullptr;
    if (!CreatePipe(&input_read, &input_write, &security, 0)) {
        fail("Create mailbox test pipe");
    }

    Handle input(input_read), commands(input_write);
    SetHandleInformation(commands.get(), HANDLE_FLAG_INHERIT, 0);
    Handle output(CreateFileW(
        L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.get();
    startup.hStdOutput = startup.hStdError = output.get();
    auto args = L"\"" + game.wstring() + L"\" \"" + (repo / L"tests/mailbox_host.lua").wstring() + L"\" \"" +
                (repo / L"hook/ScreenSystemLayer overlay.lua").wstring() + L"\"";
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(game.c_str(),
            args.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            program.c_str(),
            &startup,
            &process)) {
        fail("Start Lua mailbox test");
    }

    Handle child(process.hProcess), thread(process.hThread);

    struct Stop {
        HANDLE input, process;

        ~Stop() {
            DWORD written = 0;
            WriteFile(input, "quit\n", 5, &written, nullptr);
            if (WaitForSingleObject(process, 5000) == WAIT_TIMEOUT) {
                // This handle belongs only to the isolated child created above.
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 5000);
            }
        }
    } stop{commands.get(), child.get()};

    MailboxReader reader, foreign;
    std::optional<ExportFrame> received;
    auto deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline && !received) {
        DWORD written = 0;
        WriteFile(commands.get(), "tick\n", 5, &written, nullptr);
        Sleep(25);
        received = reader.poll(folder);
    }

    check(received.has_value(), "discovers a mailbox allocated by real Lua with no game offsets");
    check(!flag(parse(received->payload).GetNamedObject(L"current"), L"playing"),
        "decodes the shipped hook's actual heartbeat after full Lua garbage collections");
    check(!foreign.poll(folder / L"wrong-game"), "ignores a process outside the selected game folder");
    check(!reader.poll(folder), "unchanged memory cannot refresh a heartbeat");
    // Real hook -> real Lua process memory -> real mailbox reader -> engine -> API transport.
    Fixture account_files("two-players");
    auto state = account_files.root / L"state";
    Preferences config;
    config.game_root = folder;
    config.accounts = {
        PlayerAccount{"Alice", protect("alice-key")}, PlayerAccount{"Bob", protect("bob-key")}};
    save_preferences(state, config);
    std::vector<std::pair<std::string, Object>> uploads;
    bool alice_offline = true;
    PiuScoresApi api([&](const std::string& url,
                         const std::string& token,
                         const std::optional<Object>& body) {
        check(token == "alice-key" || token == "bob-key", "transport receives only the matched account key");
        if (url.find("/charts?") != std::string::npos) {
            return parse(
                R"({"data":[{"id":"s14","songName":"Digitalis","type":"Single","level":14},{"id":"s16","songName":"Digitalis","type":"Single","level":16}],"next":null})");
        }

        if (!body) {
            if (token == "alice-key" && alice_offline) {
                throw Error("Account temporarily unavailable");
            }

            return parse(R"({"scoringModel":"phoenix","data":[],"next":null})");
        }

        uploads.emplace_back(token, body->GetNamedArray(L"plays").GetAt(0).GetObject());
        return parse(R"({"recorded":1})");
    });
    Engine engine(state, api);
    engine.load();
    auto command = [&](std::string_view value) {
        DWORD count = 0;
        check(WriteFile(commands.get(), value.data(), static_cast<DWORD>(value.size()), &count, nullptr) &&
                  count == value.size(),
            "drive isolated game process");
    };
    auto wait_for = [&](const auto& ready) {
        auto until = GetTickCount64() + 10000;
        while (GetTickCount64() < until) {
            command("tick\n");
            Sleep(30);
            engine.poll();
            if (ready()) {
                return;
            }
        }

        throw Error("Two-player mailbox integration timed out");
    };
    command("play\n");
    wait_for([&] {
        return engine.player_status().find("P2: Bob") != std::string::npos;
    });
    command("finish\n");
    wait_for([&] {
        return engine.store().pending.size() == 2;
    });
    rejects(
        [&] {
            engine.sync();
        },
        "one account outage is reported after the other account is serviced");
    check(uploads.size() == 1 && uploads[0].first == "bob-key" &&
              str(uploads[0].second, L"chartId") == "s16" && number(uploads[0].second, L"score") == 980002 &&
              str(uploads[0].second, L"award") == "TG",
        "P2 posts its own chart, score and plate while P1 remains offline");
    Engine restarted(state, api);
    restarted.load();
    check(restarted.pending_count(0) == 1 && restarted.pending_count(1) == 0,
        "pending account survives restart");
    alice_offline = false;
    restarted.sync();
    check(uploads.size() == 2 && uploads[1].first == "alice-key" &&
              str(uploads[1].second, L"chartId") == "s14" && number(uploads[1].second, L"score") == 950001,
        "restarted queue uses Alice's original account");
    // Continue with the restarted engine so old snapshots cannot be submitted again.
    command("swap\n");
    auto until = GetTickCount64() + 10000;
    while (restarted.player_status().find("P1: Bob") == std::string::npos && GetTickCount64() < until) {
        command("tick\n");
        Sleep(30);
        restarted.poll();
    }

    check(restarted.player_status().find("P1: Bob — Account 2 matched") != std::string::npos,
        "saved profiles follow the players when sides swap");
    command("finish\n");
    until = GetTickCount64() + 10000;
    while (restarted.store().pending.size() != 2 && GetTickCount64() < until) {
        command("tick\n");
        Sleep(30);
        restarted.poll();
    }

    check(restarted.store().pending.size() == 2, "both swapped results reach the real mailbox reader");
    restarted.sync();
    check(uploads.size() == 4 && uploads[2].first == "alice-key" &&
              number(uploads[2].second, L"score") == 950101 && uploads[3].first == "bob-key" &&
              number(uploads[3].second, L"score") == 980102,
        "side swapping never swaps API keys or result payloads");
    restarted.poll();
    restarted.sync();
    check(uploads.size() == 4, "repeated snapshots do not duplicate either player's upload");
    auto prior_receipts = restarted.store().receipts.size();
    command("invalid\n");
    command("finish\n");
    until = GetTickCount64() + 10000;
    while (restarted.store().receipts.size() == prior_receipts && GetTickCount64() < until) {
        command("tick\n");
        Sleep(30);
        restarted.poll();
    }

    check(restarted.pending_count(0) == 0 && restarted.pending_count(1) == 1 &&
              restarted.store().receipts.size() == prior_receipts + 1,
        "Alice's improper judgement modifier is skipped while Bob's valid result queues");
    restarted.sync();
    check(uploads.size() == 5 && uploads.back().first == "bob-key",
        "only the eligible player's result reaches the upload transport");
    DWORD written = 0;
    WriteFile(commands.get(), "quit\n", 5, &written, nullptr);
    check(WaitForSingleObject(child.get(), 5000) == WAIT_OBJECT_0, "child exits normally");
    check(!reader.poll(folder), "disconnects when the game exits");
}
} // namespace

void mailbox_checks() {
    std::string text = R"({"current":{"playing":false,"title":"éあ\\\""}})";
    auto valid = block(text);
    auto decoded = decode_mailbox(valid, valid);
    check(decoded && decoded->payload == text, "preserves exact UTF-8 and JSON escapes");
    auto changed = valid;
    changed[6].value = 3;
    check(!decode_mailbox(changed, changed), "rejects a writer in progress");
    changed[6].value = 4;
    check(!decode_mailbox(valid, changed), "rejects a sequence changed during reading");
    changed = valid;
    changed[9].value += 1;
    check(!decode_mailbox(valid, changed), "rejects a changed payload even with an unchanged header");
    for (auto length : {-1.0, 6091.0, 2.5, std::numeric_limits<double>::quiet_NaN()}) {
        changed = valid;
        changed[7].value = length;
        check(!decode_mailbox(changed, changed), "rejects an invalid payload length");
    }

    changed = valid;
    changed[9].type = 4;
    check(!decode_mailbox(changed, changed), "never follows a Lua string or other pointer");
    changed = valid;
    changed[9].value = 281474976710656.0;
    check(!decode_mailbox(changed, changed), "rejects oversized numeric chunks");
    changed = valid;
    changed[4].value = 2;
    check(!decode_mailbox(changed, changed), "rejects unsupported protocol versions");
    changed = valid;
    changed[8].value = std::numeric_limits<double>::infinity();
    check(!decode_mailbox(changed, changed), "rejects an invalid game clock");
    auto maximum = block(std::string(6090, 'x'));
    auto full = decode_mailbox(maximum, maximum);
    check(full && full->payload.size() == 6090, "accepts the exact capacity");
    auto hook = GameHook{};
    auto overlay = hook.configured(false, true);
    check(overlay.source().find("local capture_results = false") != std::string::npos &&
              overlay.fingerprint() != hook.fingerprint(),
        "installer disables capture in overlay-only source");
    check(hook.configured(true, false).source().find("local publish_chart = false") != std::string::npos,
        "installer omits current-chart publishing in sync-only source");
    live_checks();
}

} // namespace piu::test
