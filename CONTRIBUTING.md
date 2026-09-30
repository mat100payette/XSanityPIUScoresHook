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

1. Set `VERSION` to the version you want to release, then commit and push your changes.
2. On GitHub, open **Actions → Release → Run workflow**, choose **main**, and click **Run workflow**.
3. The workflow builds and checks the installer, creates the matching tag if needed, and prepares a draft release with the installer and SHA-256 checksum.
4. Open **Releases**, review the draft, and publish when ready.

Check the installer, component combinations, and a live gameplay/upload session before distributing a tested release. Sign the executables and installer when a certificate is available; regenerate its checksum after signing.

The workflow uses `VERSION` from the selected commit. It does not bump versions or commit changes. For another release, update `VERSION` first. An existing tag must point to that same commit. Pushing a matching version tag also triggers the workflow.
