#pragma once
#include "game_hook.h"
#include "maintenance.h"
#include "model.h"
#include <chrono>
#include <iostream>
#include <source_location>

namespace piu::test {
inline std::atomic<int> assertions = 0;
inline bool previews = false;

class Failure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void check(
    bool condition, const char* label, std::source_location location = std::source_location::current()) {
    if (!condition) {
        throw Failure(
            std::string(location.file_name()) + ":" + std::to_string(location.line()) + ": " + label);
    }

    ++assertions;
}

template <typename F>
void rejects(F&& action, const char* label, std::source_location location = std::source_location::current()) {
    bool rejected = false;
    try {
        action();
    } catch (const Failure&) {
        throw; // A failed assertion inside a fake is never the expected rejection.
    } catch (const std::exception&) {
        rejected = true;
    } catch (const winrt::hresult_error&) {
        rejected = true;
    }

    check(rejected, label, location);
}

// Only used for processes started by these tests; cleanup also runs after a failed check.
struct ChildProcessGuard {
    HANDLE process;

    ~ChildProcessGuard() {
        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 5000);
        }
    }
};

struct Fixture {
    fs::path root;

    explicit Fixture(const char* name, bool temporary = false) {
        auto unique = maintenance_directory();
        auto id = unique.filename().wstring();
        id = id.substr(id.find(L'{') + 1, 8);
        root = temporary ? unique : executable().parent_path() / L"fixtures" / (wide(name) + L"-" + id);
        safe_path(root);
        if (fs::exists(root)) {
            throw Error("A test fixture already exists. Inspect and remove build fixtures before retrying.");
        }

        fs::create_directories(root);
    }

    ~Fixture() {
        if (std::uncaught_exceptions()) {
            std::cerr << "Failure in fixture: " << root.filename().string() << "\n";
        }

        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    fs::path game(const wchar_t* name = L"game") {
        auto path = root / name;
        atomic_write(path / L"Program64" / L"XSanity.exe", "fake game");
        fs::create_directories(path / L"Themes" / L"xsanity" / L"BGAnimations");
        atomic_write(
            path / L"Themes" / L"_fallback" / L"BGAnimations" / L"ScreenSystemLayer overlay" / L"default.lua",
            "return Def.ActorFrame {} -- built-in overlay");
        return path;
    }
};

inline Result result(std::string id = "event-1", int score = 950000, bool broken = false) {
    return {std::move(id), {"Test Song!", "Single", "S10", 10}, score, broken, true};
}

} // namespace piu::test
