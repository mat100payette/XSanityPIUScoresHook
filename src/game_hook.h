#pragma once
#include "platform.h"

namespace piu {
class GameHook {
    std::string source_;
public:
    explicit GameHook(std::string source = resource(HookResource)) : source_(std::move(source)) {}
    static fs::path validate(const fs::path& root);
    static fs::path layer(const fs::path& root);
    static fs::path exports(const fs::path& root);
    bool owned(const fs::path& path) const;
    void preflight(const fs::path& root) const;
    const std::string& source() const { return source_; }
};
void safe_path(const fs::path& path);
}
