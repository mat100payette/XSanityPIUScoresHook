#pragma once
#include "platform.h"

namespace piu {
enum class HookState { Missing, Current, Outdated, Conflict };

struct HookStatus {
    HookState state = HookState::Missing;
    std::string fingerprint;
    bool operator==(const HookStatus&) const = default;
};

class GameHook {
    std::string source_;
    std::string fingerprint_;

public:
    explicit GameHook(std::string source = resource(HookResource))
        : source_(std::move(source)), fingerprint_(fingerprint(source_)) {
    }

    static fs::path validate(const fs::path& root);
    static fs::path layer(const fs::path& root);
    static fs::path exports(const fs::path& root);
    static std::string fingerprint(std::string_view source);

    HookStatus inspect(const fs::path& path, std::string_view installed_fingerprint = {}) const;
    HookStatus preflight(const fs::path& root, std::string_view installed_fingerprint = {}) const;
    bool owned(const fs::path& path, std::string_view installed_fingerprint = {}) const;
    bool current(const fs::path& path) const;

    const std::string& source() const {
        return source_;
    }

    const std::string& fingerprint() const {
        return fingerprint_;
    }
};

void safe_path(const fs::path& path);
} // namespace piu
