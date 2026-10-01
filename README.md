# XSanity PIU Scores Hook

[![CI](https://github.com/mat100payette/XSanityPIUScoresHook/actions/workflows/ci.yml/badge.svg)](https://github.com/mat100payette/XSanityPIUScoresHook/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Automatically upload your XSanity personal bests to [PIU Scores](https://piuscores.arroweclip.se). Add an optional OBS overlay showing **song · difficulty · website PB**.

**Requires:** Windows 10/11 (x64) and XSanity with the **xsanity** theme. Keep the companion running while playing; no game logging settings are needed.

## Get started

1. **[Download the installer](https://github.com/mat100payette/XSanityPIUScoresHook/releases/latest)** — choose the `win-x64-setup.exe` asset.
2. Close XSanity, open the installer, and select your game folder containing **Program64** (or **Program32**) and **Themes**.
3. Choose **Automatic PB syncing**, **OBS overlay**, or both, then click **Install**. The overlay is off by default.
4. Enter your personal PIU Scores API token and select **Phoenix** or **Phoenix 2** to match your scores.
5. Open **Play XSanity** from the Start menu to launch the game and companion together.

The tray icon gives you **Account settings**, **Check for updates**, **Manage installation**, and **Exit**. Closing the game leaves the companion running. Run both normally as the same Windows user; do not launch the game as administrator.

## Set up the OBS overlay

With **OBS overlay** installed, add a **Browser Source** in OBS:

| Setting | Value |
| --- | --- |
| URL | `http://127.0.0.1:8765/overlay` |
| Width | `700` |
| Height | `100` |

The background is transparent, and the overlay hides outside gameplay. PB shows `—` if no website score or unique chart match is available.

## Change your setup

Close XSanity, then open **Manage installation** from the Start menu or tray. You can also reopen the installer.

| To… | Do this |
| --- | --- |
| Add or remove a component | Change its checkbox and click **Apply changes**. |
| Update | Choose **Check for updates** from the tray, then **Download and open setup**. Click **Apply changes** in setup. |
| Move your game folder | Exit the companion before moving it. Select the new folder in setup and click **Apply changes**. |
| Uninstall everything | Clear both checkboxes and click **Remove all**. |

If your version has no **Check for updates** menu item, download and run the [latest installer](https://github.com/mat100payette/XSanityPIUScoresHook/releases/latest) once. No uninstall is needed.

- Updates and moves preserve account settings and pending uploads while syncing stays enabled. The OBS URL stays the same.
- Removing syncing discards pending uploads. Enabling it again captures new plays only.
- Removing both components also deletes saved account data.

## What gets synced?

- **Supported play:** normal single-player Single/Double charts with Phoenix scoring.
- **Skipped play:** autoplay, slow/fast Rush, altered judgement or note counts, disqualified results, custom or unmatched charts, training, missions, multiplayer, and courses. Normal scroll speed and visual settings are allowed.
- **Plates:** included with new PBs, using the completed result’s judgements. Failed stages have no plate.
- **Personal bests:** compared against PIU Scores. A cleared stage beats a stage break; otherwise, the higher score wins. Existing game score history is not imported.
- **Offline play:** results wait for the connection to return. If the companion reports an unresolved upload, check it on PIU Scores.
- **Account changes:** finish pending uploads before switching accounts or mixes.

## Development

See [Contributing](CONTRIBUTING.md) for building, formatting, and publishing releases, or [Security](SECURITY.md) for account and data handling.

Checks cover simulated accounts, installer maintenance, and the Lua bridge across real processes. Gameplay capture and an authenticated PB upload have also been verified in XSanity with logging disabled.
