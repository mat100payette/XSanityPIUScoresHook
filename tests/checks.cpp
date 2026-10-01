#include "support.h"

namespace piu::test {
void engine_checks();
void capture_window_checks();
void player_checks();
void screenshot_checks();
void feedback_checks();
void api_checks();
void mailbox_checks();
void update_checks();
void installer_checks();
void installer_worker_checks();
void hook_upgrade_checks();
void overlay_checks();
void maintenance_checks();
void icon_checks();
void view_checks();
void progress_checks();
int ipc_companion(const std::wstring& prefix);
int update_network_check();
} // namespace piu::test

using namespace piu;
using namespace piu::test;

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

        if (argc == 2 && std::wstring_view(argv[1]) == L"--update-network") {
            return update_network_check();
        }

        std::wstring selected;
        for (int i = 1; i < argc; ++i) {
            std::wstring_view argument(argv[i]);
            if (argument == L"--previews") {
                previews = true;
            } else if (argument == L"--ui") {
                selected = L"setup";
            } else if (argument == L"--updates") {
                selected = L"updater";
            } else if (argument == L"--suite" && i + 1 < argc) {
                selected = argv[++i];
            } else {
                throw Error("Usage: Checks.exe [--suite NAME] [--previews]");
            }
        }

        struct Suite {
            const wchar_t* name;
            void (*run)();
        };

        const Suite suites[] = {{L"engine", engine_checks},
            {L"capture", capture_window_checks},
            {L"players", player_checks},
            {L"screenshots", screenshot_checks},
            {L"feedback", feedback_checks},
            {L"api", api_checks},
            {L"mailbox", mailbox_checks},
            {L"updater", update_checks},
            {L"installer", installer_checks},
            {L"installer-worker", installer_worker_checks},
            {L"hook-upgrade", hook_upgrade_checks},
            {L"overlay", overlay_checks},
            {L"maintenance", maintenance_checks},
            {L"icons", icon_checks},
            {L"setup", view_checks},
            {L"progress", progress_checks}};
        bool ran = false;
        for (const auto& suite : suites) {
            if (!selected.empty() && selected != suite.name) {
                continue;
            }

            ran = true;
            auto before = assertions.load();
            auto start = std::chrono::steady_clock::now();
            std::cout << "Running " << utf8(suite.name) << "..." << std::flush;
            suite.run();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            std::cout << " " << assertions.load() - before << " assertions, " << ms.count() << " ms\n";
        }

        if (!ran) {
            throw Error("Unknown test suite: " + utf8(selected));
        }

        std::cout << assertions.load() << " assertions passed. Isolated fixtures; no live game or account.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "\n" << error.what() << '\n';
        return 1;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "\n" << utf8(error.message().c_str()) << '\n';
        return 1;
    }
}
