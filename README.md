# XSanity PIU Scores Hook

[![CI](https://github.com/mat100payette/XSanityPIUScoresHook/actions/workflows/ci.yml/badge.svg)](https://github.com/mat100payette/XSanityPIUScoresHook/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

A small Windows companion that uploads new XSanity personal bests to [PIU Scores](https://piuscores.arroweclip.se). An **optional OBS overlay** shows only **current song · difficulty · your website PB**.

**Windows 10/11, x64**, with XSanity's **xsanity theme**.

## Install and play

1. Close XSanity and open **XSanityPIUScoresHook-v0.2.0-win-x64-setup.exe**.
2. Choose your XSanity folder and components. **Automatic PB syncing** is checked; **OBS overlay** is unchecked. Either can be installed alone.
3. Click **Install**. In the companion's account screen, enter your personal PIU Scores API token and select **Phoenix** or **Phoenix 2** to match your scores.
4. Use **Play XSanity** from the Start menu to launch the game and companion together.

Click the companion's tray icon for **Account settings**, **Manage installation**, or **Exit**. Closing the game leaves the companion running. Each player uses their own account and game folder; share the installer, not your saved settings.

## Change or remove components

Reopen the downloaded installer, choose **Manage installation** from the Start menu or tray, or open the app's uninstall entry in Windows Settings. Setup shows your installed choices.

| Selection | What runs |
| --- | --- |
| Automatic PB syncing | Upload eligible improved scores; no OBS listener |
| OBS overlay | Read website PBs; no result capture or score uploads |
| Both | Sync scores and show the overlay |
| Neither | Remove the app, game connection, shortcuts, registration and saved account data |

Change the boxes and click **Apply changes**. Clearing both changes the button to **Remove all**. Removing syncing discards pending uploads; enabling it again captures new plays only. The remaining component keeps its account settings.

Both components use the same small game exporter. Setup keeps it while either is installed and removes it with the last component. Close XSanity when adding, moving, or removing that game connection. Changes that keep the existing connection can be applied while the game is open.

Setup accepts an identical existing exporter and refuses to overwrite or remove a different or modified theme layer. It restores changed files and registration if an operation fails, and preserves unrelated files. Windows blocks replacing a running setup executable, so installed maintenance runs from a private temporary copy that is cleaned after exit.

## OBS overlay

With **OBS overlay** installed, add an OBS **Browser Source**:

```text
URL:    http://127.0.0.1:8765/overlay
Width:  700
Height: 100
```

The transparent overlay hides outside gameplay. PB shows `—` when there is no website score or unique chart match. Its server listens only on your computer.

## Score syncing

PIU Scores owns your PBs. XSanity supplies only the current chart and completed result; the companion never reads game score history. A passed stage takes priority over a broken stage, then the higher score wins. Website PBs refresh after an upload and once a minute.

Supports normal single-player **Single/Double** charts with **Phoenix scoring**. Skips autoplay, changed music rates, nonstandard judgement, disqualified results, ambiguous matches, and custom chart variants. Training, missions, multiplayer, and courses are unsupported.

Offline results wait in a temporary queue. Accepted, covered, and skipped score payloads are removed; event receipts prevent duplicates across restarts. If a POST response is lost, the companion checks website PBs and avoids blind retries. An unresolved upload needs checking on PIU Scores. Keep the same account while results are pending; you can replace an expired token, but finish pending uploads before changing mix.

The [PIU Scores API documentation](https://piuscores.arroweclip.se/swagger/index.html) describes the score contract. Tokens are encrypted for your Windows user and never exposed to OBS.

## Files and development

```text
%LOCALAPPDATA%/Programs/XSanityPIUScoresHook/    Installed app and maintenance setup
%LOCALAPPDATA%/XSanityPIUScoresHook/             Account settings and temporary queue
<XSanity>/Themes/xsanity/BGAnimations/ScreenSystemLayer aux.lua
<XSanity>/Save/PiuCompanion/                    Transient game exports
```

Install **Visual Studio Build Tools → Desktop development with C++**, including MSVC x64 tools and Windows SDK. From PowerShell in the repository:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Test -Package
```

This builds the app and installer, runs behavioral checks, and produces:

```text
dist/PiuCompanion.exe
dist/PiuCompanionSetup.exe
artifacts/XSanityPIUScoresHook-v0.2.0-win-x64-setup.exe
artifacts/XSanityPIUScoresHook-v0.2.0-win-x64-setup.exe.sha256
```

Share the versioned installer. `VERSION` supplies executable and package versions. Omit switches for a build alone; use `-Configuration Debug` for `dist/Debug`. VS Code tasks provide **Build**, **Check**, and **Package**.

```text
src/          Companion, API client, syncing, Win32 UI and overlay
installer/    Setup, component changes, rollback and removal
hook/         XSanity's transient export layer
scripts/      One build/check/package command
tests/        Fake API/game/installation checks and installer preview
```

Generated outputs stay outside Git. CI runs the same checks. To prepare a **draft release** on GitHub, open **Actions → Release → Run workflow**. It uses `VERSION`, creates the matching tag if needed, and attaches the installer and checksum. See [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md), and [LICENSE](LICENSE).

**Verification:** automated checks use fake game folders, account data, and installation registration. Live gameplay capture and authenticated uploads still need a real session. These local builds are unsigned.
