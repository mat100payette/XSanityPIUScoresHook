#include "game_hook.h"

namespace piu {
void safe_path(const fs::path& path) {
    auto current = fs::absolute(path).lexically_normal();
    for (;;) {
        DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw Error("Setup cannot modify a symbolic link or junction: " + utf8(current.wstring()));
        auto parent = current.parent_path(); if (parent == current || parent.empty()) break; current = parent;
    }
}
fs::path GameHook::validate(const fs::path& root) {
    if (root.empty() || root.wstring().find(L'"') != std::wstring::npos) throw Error("Choose your XSanity folder.");
    auto path = fs::absolute(root).lexically_normal(); safe_path(path); game_executable(path);
    if (!fs::is_directory(path / L"Themes" / L"xsanity" / L"BGAnimations")) throw Error("The selected folder does not contain the xsanity theme.");
    return path;
}
fs::path GameHook::layer(const fs::path& root) { return root / L"Themes" / L"xsanity" / L"BGAnimations" / L"ScreenSystemLayer aux.lua"; }
fs::path GameHook::exports(const fs::path& root) { return root / L"Save" / L"PiuCompanion"; }
bool GameHook::owned(const fs::path& path) const {
    if (!fs::is_regular_file(path)) return false;
    auto canonical = [](const std::string& text) { std::string result; for (char c : text) if (c != '\r') result += c; return result; };
    return canonical(read(path, 65536)) == canonical(source_);
}
void GameHook::preflight(const fs::path& root) const {
    validate(root); auto path = layer(root); safe_path(path); safe_path(exports(root));
    auto base = path.parent_path();
    if (fs::exists(base / L"ScreenSystemLayer aux") || fs::exists(base / L"ScreenSystemLayer aux.redir") ||
        (fs::exists(path) && !owned(path))) throw Error("XSanity already has a different ScreenSystemLayer aux. Setup will leave it untouched.");
}
}
