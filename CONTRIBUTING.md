# Contributing

Keep changes focused and the companion small. Discuss larger behavior changes in an issue first.

## Local checks

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Test -Package
```

Install Visual Studio Build Tools with **Desktop development with C++**, the MSVC x64 tools, and Windows SDK. The build uses C++20. Warnings fail the build. Follow `.editorconfig`; behavioral checks in `tests/checks.cpp` use disposable game folders, a fake API, and fake installation registration. Installer checks cover native accessibility, mouse/keyboard input, display scaling, scrolling, and themed screen previews.

Open [XSanityPIUScoresHook.code-workspace](XSanityPIUScoresHook.code-workspace) in VS Code for editor settings and the **Build**, **Check**, and **Package** tasks.

- `dist/` contains the app and maintenance installer.
- `artifacts/` contains the versioned installer and its SHA-256 checksum.
- Omit `-Test -Package` for a build alone; use `-Configuration Debug` for `dist/Debug`.

`VERSION` supplies executable and package versions. Generated outputs stay outside Git; CI runs the same checks.

C++ uses `.clang-format` and Lua uses `.stylua.toml`: four spaces, expanded control flow, and a 110-column limit. Leave one blank line after a completed control-flow block before the next statement; the formatters preserve this spacing but do not insert it automatically. Both languages format on save with the recommended VS Code C/C++ and StyLua extensions.

Run `.\scripts\format.ps1` to format both languages, or add `-Check` to check them. Use `-Language Cpp` or `-Language Lua` for just one language. The script finds clang-format (15 or newer) and StyLua on PATH or in their VS Code extension locations; `-ClangFormat` and `-StyLua` accept explicit paths. Open a Lua file once after installing the StyLua extension so it downloads its formatter. Lua formatting also verifies that the parsed syntax is preserved.

The engine's mutex protects state shared by export polling, syncing, and the UI. Keep network requests outside that lock. Settings and pending-upload fields are persisted; preserve compatibility when changing them. Installer tests must never use real game folders, account tokens, Start menu shortcuts, or uninstall registration.

Exporter ownership and version matching are separate. `installation.json` records the installed exporter SHA-256 and game folder; normalize only CRLF/LF when hashing. Setup can replace an unchanged recorded exporter with the current bundled copy, even across skipped releases. When the user selects a moved or copied game folder, accept only an exporter matching that receipt or the bundled source, validate both locations again before writing, and commit the new folder with the fingerprint. Relocation clears stale exports and advances the capture cutoff even when the hook is already current. Test upgrades with synthetic exporters; do not keep historical hook copies or per-release fingerprints. Update the hook, capture cutoff, transient exports, and installation receipt in the same rollback transaction. Preserve pending uploads and account settings. The companion requires its bundled exporter at startup.

The overlay contains only current song, difficulty, and website PB. PIU Scores owns PBs; XSanity provides transient exports. Keep overlay-only mode read-only and syncing-only mode free of a local listener. Setup must preserve existing game files and refuse conflicting theme layers.

Describe what changed, why, and how it was tested in pull requests. Call out live game/API behavior that has not been verified.

## Installed files

| Location | Contents |
| --- | --- |
| `%LOCALAPPDATA%/Programs/XSanityPIUScoresHook/` | App, maintenance setup, and installation receipt |
| `%LOCALAPPDATA%/XSanityPIUScoresHook/` | Account settings and pending uploads |
| `<XSanity>/Themes/xsanity/BGAnimations/ScreenSystemLayer aux.lua` | Game exporter |
| `<XSanity>/Save/PiuCompanion/` | Transient game exports |

The [PIU Scores API documentation](https://piuscores.arroweclip.se/swagger/index.html) describes the score contract.

## Release

1. Commit and push the changes you want to release.
2. On GitHub, open **Actions → Release → Run workflow**.
3. Choose **main** and a version bump: **patch**, **minor**, or **major**.
4. Click **Run workflow**. When it succeeds, the published release link appears in the run summary.

The workflow bumps the latest `vX.Y.Z` tag, updates `VERSION`, builds and checks the installer, pushes the version commit and tag, and publishes the installer and SHA-256 checksum. The first release uses `VERSION` as-is. No manual version edit or publishing step is needed.

Release logic lives in `scripts/prepare-release.ps1` (version commit and tag) and `scripts/publish-release.ps1` (push and publication); the workflow calls these around `scripts/build.ps1`.

Check the installer, component combinations, and a live gameplay/upload session before distributing a tested release. Sign the executables and installer when a certificate is available; regenerate its checksum after signing.
