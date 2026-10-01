# Contributing

Keep changes focused and the companion small. Discuss larger behavior changes in an issue first.

## Local checks

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Test -Package
```

Install Visual Studio Build Tools with **Desktop development with C++**, the MSVC x64 tools, and Windows SDK. The build uses C++20. Warnings fail the build. Follow `.editorconfig`; tests are grouped by feature in `tests/*_checks.cpp`, with shared assertions and disposable fixtures in `tests/support.h`. They use fake APIs and installation registration. Installer checks cover rollback, native accessibility, mouse/keyboard input, display scaling, scrolling, and repaint behavior.

Lua exporter checks run as part of the test build using a local Lua 5.1.5 interpreter, built from the checksum-verified official source and cached under build/tools. The first test build needs internet access; the interpreter is only a development tool and is not packaged. The tests exercise actor initialization, current-chart heartbeats, result capture, eligibility, duplicate prevention, component isolation, and error reporting. A separate native integration test reads the shipped hook's mailbox from an isolated Lua process.

After building, run `build/Release/Checks.exe` for all native suites or add `--suite engine`, `--suite installer`, `--suite setup`, etc. Each suite prints its duration; failed assertions include the source file and line. For manual visual review, add `--previews` to save PNGs beside the test executable. Normal runs still test UI behavior without generating screenshots.

Open [XSanityPIUScoresHook.code-workspace](XSanityPIUScoresHook.code-workspace) in VS Code for editor settings and the **Build**, **Check**, and **Package** tasks.

- `dist/` contains the app and maintenance installer.
- `artifacts/` contains the versioned installer and its SHA-256 checksum.
- Omit `-Test -Package` for a build alone; use `-Configuration Debug` for `dist/Debug`.

`VERSION` is the fallback for local and CI builds. Release builds pass `-Version X.Y.Z` to set executable metadata, updater comparisons, and package filenames without editing tracked files. To reproduce a release, check out its tag and pass its version explicitly. Generated outputs stay outside Git; CI runs the same checks.

C++ uses `.clang-format` and Lua uses `.stylua.toml`: four spaces, expanded control flow, and a 110-column limit. Leave one blank line after a completed control-flow block before the next statement; the formatters preserve this spacing but do not insert it automatically. Both languages format on save with the recommended VS Code C/C++ and StyLua extensions.

Run `.\scripts\format.ps1` to format both languages, or add `-Check` to check them. Use `-Language Cpp` or `-Language Lua` for just one language. The script finds clang-format (15 or newer) and StyLua on PATH or in their VS Code extension locations; `-ClangFormat` and `-StyLua` accept explicit paths. Open a Lua file once after installing the StyLua extension so it downloads its formatter. Lua formatting also verifies that the parsed syntax is preserved.

The engine's mutex protects state shared by export polling, syncing, and the UI. Keep network requests outside that lock. Settings and pending-upload fields are persisted; preserve compatibility when changing them. Installer tests must never use real game folders, account tokens, Start menu shortcuts, or uninstall registration.

Exporter ownership and version matching are separate. `installation.json` records the installed exporter SHA-256, filename, and game folder; normalize only CRLF/LF when hashing. Setup can replace an unchanged recorded exporter with the current bundled copy, even across skipped releases. When the user selects a moved or copied game folder, accept only an exporter matching that receipt or the bundled source, validate both locations again before writing, and commit the new folder with the fingerprint. Relocation clears any obsolete file exports and advances the capture cutoff even when the hook is already current. Test upgrades with synthetic exporters; do not keep historical hook copies or per-release fingerprints. Update the hook, capture cutoff, transient exports, and installation receipt in the same rollback transaction. Preserve pending uploads and account settings. The companion requires its bundled exporter configured for the installed components at startup. See [Game bridge](docs/bridge.md) for the in-memory protocol and runtime compatibility limits.

The in-app updater requires public GitHub releases; it never requests a GitHub token or uses the PIU Scores token. Updates reuse the installer rather than maintaining a second installation path. Keep release parsing and integrity checks in update.cpp, cancellable GitHub transport in update_http.cpp, and the native dialog in update_ui.cpp. Tests use fake releases and launch callbacks; they must not run a downloaded installer. The optional build/Release/Checks.exe --update-network check downloads and verifies the public release, then deletes its temporary copy without executing it.

The overlay contains only current song, difficulty, and website PB. PIU Scores owns PBs; XSanity provides transient exports. Keep overlay-only mode read-only and syncing-only mode free of a local listener. Setup must preserve existing game files and refuse conflicting theme layers.

Describe what changed, why, and how it was tested in pull requests. Call out live game/API behavior that has not been verified.

## Installed files

| Location | Contents |
| --- | --- |
| `%LOCALAPPDATA%/Programs/XSanityPIUScoresHook/` | App, maintenance setup, and installation receipt |
| `%LOCALAPPDATA%/XSanityPIUScoresHook/` | Account settings and pending uploads |
| `<XSanity>/Themes/xsanity/BGAnimations/ScreenSystemLayer overlay.lua` | Game exporter |

The [PIU Scores API documentation](https://piuscores.arroweclip.se/swagger/index.html) describes the score contract.

## Release

1. Merge the changes you want to release into **main** through a pull request.
2. On GitHub, open **Actions → Release → Run workflow**.
3. Choose **main** and a version bump: **patch**, **minor**, or **major**.
4. Click **Run workflow**. When it succeeds, the published release link appears in the run summary.

The workflow calculates the next version from the latest `vX.Y.Z` tag and passes it into the build. The first release uses `VERSION` as-is. It tags the exact main commit that passed the build and publishes the installer and SHA-256 checksum. It never commits or pushes to main, so required pull requests remain enforced; no bypass or personal token is needed. Only the publish job has repository write permission.

Release logic lives in `scripts/prepare-release.ps1` (version and commit selection) and `scripts/publish-release.ps1` (tag and publication). Run `scripts/test-release.ps1` for isolated release checks with a local Git remote and fake GitHub responses.

If publishing fails after the build, use **Re-run failed jobs** to reuse the tested package. An existing tag must point to the same commit; draft uploads can be retried, and published releases are never overwritten. Rerunning the entire workflow at the same tagged commit keeps that version instead of creating another release.

Check the installer, component combinations, and a live gameplay/upload session before distributing a tested release. Sign the executables and installer when a certificate is available; regenerate its checksum after signing.
