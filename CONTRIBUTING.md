# Contributing

Keep changes focused and the companion small. Discuss larger behavior changes in an issue first.

## Local checks

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Test -Package
```

Install Visual Studio Build Tools with **Desktop development with C++**, the MSVC x64 tools, and Windows SDK. The build uses C++20. Warnings fail the build. Follow `.editorconfig`; behavioral checks in `tests/checks.cpp` use disposable game folders, a fake API, and fake installation registration. Installer checks cover native accessibility, mouse/keyboard input, display scaling, scrolling, and themed screen previews.

The engine's mutex protects state shared by export polling, syncing, and the UI. Keep network requests outside that lock. Settings and pending-upload fields are persisted; preserve compatibility when changing them. Installer tests must never use real game folders, account tokens, Start menu shortcuts, or uninstall registration.

The overlay contains only current song, difficulty, and website PB. PIU Scores owns PBs; XSanity provides transient exports. Keep overlay-only mode read-only and syncing-only mode free of a local listener. Setup must preserve existing game files and refuse conflicting theme layers.

Describe what changed, why, and how it was tested in pull requests. Call out live game/API behavior that has not been verified.

## Release

1. Commit and push the changes you want to release.
2. On GitHub, open **Actions → Release → Run workflow**.
3. Choose **main** and a version bump: **patch**, **minor**, or **major**.
4. Click **Run workflow**. When it succeeds, the published release link appears in the run summary.

The workflow bumps the latest `vX.Y.Z` tag, updates `VERSION`, builds and checks the installer, pushes the version commit and tag, and publishes the installer and SHA-256 checksum. The first release uses `VERSION` as-is. No manual version edit or publishing step is needed.

Release logic lives in `scripts/prepare-release.ps1` (version commit and tag) and `scripts/publish-release.ps1` (push and publication); the workflow calls these around `scripts/build.ps1`.

Check the installer, component combinations, and a live gameplay/upload session before distributing a tested release. Sign the executables and installer when a certificate is available; regenerate its checksum after signing.
