#include "mailbox.h"
#include "game_hook.h"
#include <cmath>
#include <limits>

using namespace piu;

namespace {
int passed = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        throw Error(std::string("Mailbox: ") + message);
    }

    ++passed;
}

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
    DWORD written = 0;
    WriteFile(commands.get(), "quit\n", 5, &written, nullptr);
    check(WaitForSingleObject(child.get(), 5000) == WAIT_OBJECT_0, "child exits normally");
    check(!reader.poll(folder), "disconnects when the game exits");
}
} // namespace

int mailbox_checks() {
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
    check(decode_mailbox(maximum, maximum)->payload.size() == 6090, "accepts the exact capacity");
    auto hook = GameHook{};
    auto overlay = hook.configured(false, true);
    check(overlay.source().find("local capture_results = false") != std::string::npos &&
              overlay.fingerprint() != hook.fingerprint(),
        "installer disables capture in overlay-only source");
    check(hook.configured(true, false).source().find("local publish_chart = false") != std::string::npos,
        "installer omits current-chart publishing in sync-only source");
    live_checks();
    return passed;
}
