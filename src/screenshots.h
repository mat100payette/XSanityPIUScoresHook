#pragma once
#include "model.h"
#include <set>

namespace piu {
using ScreenCapture = std::function<std::string(const fs::path&)>;
std::string capture_game_png(const fs::path& game_root);
std::string capture_window_png(HWND window);
void validate_screenshot_folder(
    const fs::path& folder, const fs::path& game_root, const fs::path& staging = {});

// One provisional image per result screen; only website-confirmed PBs are kept.
class Screenshots {
    struct Shot {
        std::vector<Result> candidates;
        fs::path destination;
        std::string stamp;
        std::string keeper;
        bool captured = false;
    };

    fs::path staging_;
    ScreenCapture capture_;
    mutable std::mutex gate_;
    std::map<std::string, Shot> shots_;
    std::string error_;
    void persist(const std::string& key, const Shot& shot);
    void erase(const std::string& key);
    void finish(const std::string& key);
    void failed() noexcept;

public:
    explicit Screenshots(const fs::path& state, ScreenCapture capture = capture_game_png)
        : staging_(state / L"pending-shots"), capture_(std::move(capture)) {
    }

    void load(const Store& store, bool enabled) noexcept;
    void prepare(
        const std::vector<Result>& results, const ScreenshotOptions& options, uint64_t written) noexcept;
    void update(const fs::path& root,
        const std::vector<Result>& results,
        bool evaluation,
        double age,
        const std::function<bool()>& still_current = {}) noexcept;
    void decide(const std::string& event, bool pb) noexcept;
    void configure(const ScreenshotOptions& options) noexcept;
    void clear() noexcept;
    std::string error() const;
};
} // namespace piu
