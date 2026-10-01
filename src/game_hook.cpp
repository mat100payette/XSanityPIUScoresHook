#include "game_hook.h"

namespace piu {

void safe_path(const fs::path& path) {
    auto current = fs::absolute(path).lexically_normal();
    for (;;) {
        DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw Error("Setup cannot modify a symbolic link or junction: " + utf8(current.wstring()));
        }

        auto parent = current.parent_path();
        if (parent == current || parent.empty()) {
            break;
        }

        current = parent;
    }
}

std::string GameHook::fingerprint(std::string_view source) {
    if (source.size() > 65536) {
        throw Error("The game export layer exceeds the size limit.");
    }

    std::string canonical;
    canonical.reserve(source.size());
    for (size_t index = 0; index < source.size(); ++index) {
        if (source[index] == '\r' && index + 1 < source.size() && source[index + 1] == '\n') {
            continue;
        }

        canonical += source[index];
    }

    return sha256(canonical);
}

fs::path GameHook::validate(const fs::path& root) {
    if (root.empty() || root.wstring().find(L'"') != std::wstring::npos) {
        throw Error("Choose your XSanity folder.");
    }

    auto path = fs::absolute(root).lexically_normal();
    safe_path(path);
    game_executable(path);
    if (!fs::is_directory(path / L"Themes" / L"xsanity" / L"BGAnimations")) {
        throw Error("The selected folder does not contain the xsanity theme.");
    }

    auto fallback = path / L"Themes" / L"_fallback" / L"BGAnimations" / L"ScreenSystemLayer overlay";
    if (!fs::is_regular_file(fallback / L"default.lua") &&
        !fs::is_regular_file(fallback.wstring() + L".lua")) {
        throw Error(
            "XSanity's built-in system overlay is missing. Restore the game theme before installing.");
    }

    return path;
}

fs::path GameHook::layer(const fs::path& root) {
    return root / L"Themes" / L"xsanity" / L"BGAnimations" / LayerName;
}

fs::path GameHook::exports(const fs::path& root) {
    return root / L"Save" / L"PiuCompanion";
}

HookStatus GameHook::inspect(const fs::path& path, std::string_view installed_fingerprint) const {
    safe_path(path);
    if (!fs::exists(path)) {
        return {};
    }

    if (!fs::is_regular_file(path)) {
        return {HookState::Conflict, {}};
    }

    auto actual = fingerprint(read(path, 65536));
    if (actual == fingerprint_) {
        return {HookState::Current, std::move(actual)};
    }

    bool unchanged = actual == installed_fingerprint;
    return {unchanged ? HookState::Outdated : HookState::Conflict, std::move(actual)};
}

bool GameHook::owned(const fs::path& path, std::string_view installed_fingerprint) const {
    auto state = inspect(path, installed_fingerprint).state;
    return state == HookState::Current || state == HookState::Outdated;
}

bool GameHook::current(const fs::path& path) const {
    return inspect(path).state == HookState::Current;
}

HookStatus GameHook::preflight(const fs::path& root, std::string_view installed_fingerprint) const {
    validate(root);
    auto path = layer(root);
    safe_path(exports(root));
    auto base = path.parent_path();
    auto status = inspect(path, installed_fingerprint);
    if (fs::exists(base / L"ScreenSystemLayer overlay") ||
        fs::exists(base / L"ScreenSystemLayer overlay.redir") || status.state == HookState::Conflict) {
        throw Error(
            "XSanity already has a different or modified ScreenSystemLayer overlay. Setup will leave it "
            "untouched.");
    }

    return status;
}
} // namespace piu
