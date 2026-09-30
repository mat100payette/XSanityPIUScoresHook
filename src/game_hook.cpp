#include "game_hook.h"
#include <wincrypt.h>
#include <array>

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

    std::array<BYTE, 32> hash{};
    DWORD size = static_cast<DWORD>(hash.size());
    if (!CryptHashCertificate2(L"SHA256",
            0,
            nullptr,
            reinterpret_cast<const BYTE*>(canonical.data()),
            static_cast<DWORD>(canonical.size()),
            hash.data(),
            &size)) {
        fail("Identify installed exporter");
    }

    if (size != hash.size()) {
        throw Error("Unexpected exporter fingerprint size.");
    }

    constexpr char Hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(hash.size() * 2);
    for (BYTE byte : hash) {
        result += Hex[byte >> 4];
        result += Hex[byte & 15];
    }

    return result;
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

    return path;
}

fs::path GameHook::layer(const fs::path& root) {
    return root / L"Themes" / L"xsanity" / L"BGAnimations" / L"ScreenSystemLayer aux.lua";
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
    if (fs::exists(base / L"ScreenSystemLayer aux") || fs::exists(base / L"ScreenSystemLayer aux.redir") ||
        status.state == HookState::Conflict) {
        throw Error("XSanity already has a different or modified ScreenSystemLayer aux. Setup will leave it "
                    "untouched.");
    }

    return status;
}
} // namespace piu
