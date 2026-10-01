# XSanity PIU Scores Hook

[![CI](https://github.com/mat100payette/XSanityPIUScoresHook/actions/workflows/ci.yml/badge.svg)](https://github.com/mat100payette/XSanityPIUScoresHook/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Automatically upload your XSanity personal bests to [PIU Scores](https://piuscores.arroweclip.se). Add an optional OBS overlay showing **song · difficulty · website PB**.

**Requires:** Windows 10/11 (x64) and XSanity with the **xsanity** theme. Keep the companion running while playing; no game logging settings are needed.

## Get started

1. **[Download the installer](https://github.com/mat100payette/XSanityPIUScoresHook/releases/latest)** — choose the `win-x64-setup.exe` asset.
2. Close XSanity, open the installer, and select your game folder containing **Program64** (or **Program32**) and **Themes**.
3. Choose **Automatic PB syncing**, **OBS overlay**, or both, then click **Install**. The overlay is off by default.
4. Enter your **XSanity profile name** and **PIU Scores API key**. Optionally add a second player, then select **Phoenix** or **Phoenix 2**.
5. Open **Play XSanity** from the Start menu to launch the game and companion together.

The tray icon gives you **Account settings**, **Check for updates**, **Manage installation**, and **Exit**. Closing the game leaves the companion running. Run both normally as the same Windows user; do not launch the game as administrator.

## Play with two people

In **Account settings**, pair each player's XSanity profile name with their own API key. The name is the one selected **in XSanity**, not their PIU Scores username. Names ignore capitalization and leading/trailing spaces; otherwise they must match exactly. Use distinct named profiles, not guest profiles.

The dialog shows the detected **P1/P2 → account** mapping. Players can swap sides without editing their keys. An unknown or duplicate profile does not upload; the other correctly matched player can still sync. Each player's chart difficulty, score, plate, and eligibility are checked independently. Co-op is excluded.

When upgrading from a single-account version, your existing key is retained as **Account 1**. Add its XSanity profile name before new scores can sync. Existing pending uploads retain their original account.

Profile/key changes are blocked while that account has pending uploads. Let them finish, or explicitly **Discard pending...** for that account if you no longer want them. Discarded scores are not recovered from game history.

## Result notifications

With syncing enabled, each player's results screen shows a small PIU Scores status card: submitted, website PB already up to date, saved for retry, or a specific reason it could not sync. Cards appear beneath each usercard, follow the current play, and fade out after five seconds. A new outcome can briefly show again; leaving the results screen hides them immediately.

**PB submitted** means PIU Scores confirmed the upload. **Upload unconfirmed** means its response was lost; check the website before taking action. **Sync status unavailable** means the game has no feedback from the companion and cannot confirm whether the score was saved.

## Set up the OBS overlay

With **OBS overlay** installed, add a **Browser Source** in OBS:

| Setting | Value |
| --- | --- |
| URL | `http://127.0.0.1:8765/overlay` |
| Width | `700` |
| Height | `100` |

The overlay remains single-player, uses Account 1, and hides during two-player gameplay. The background is transparent, and it hides outside gameplay. PB shows `—` if no website score or unique chart match is available.

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

- **Supported play:** single-player Single/Double charts, or two players on independent Single charts, with Phoenix scoring.
- **Skipped play:** autoplay, slow/fast Rush, altered judgement or note counts, disqualified results, custom or unmatched charts, training, missions, co-op, and courses. Normal scroll speed and visual settings are allowed.
- **Plates:** included with new PBs, using the completed result’s judgements. Failed stages have no plate.
- **Personal bests:** compared against PIU Scores. A cleared stage beats a stage break; otherwise, the higher score wins. Existing game score history is not imported.
- **Offline play:** results wait for the connection to return. If the companion reports an unresolved upload, check it on PIU Scores.
- **Account changes:** finish pending uploads before switching accounts or mixes.

## Development

See [Contributing](CONTRIBUTING.md) for building, formatting, and publishing releases, or [Security](SECURITY.md) for account and data handling.

Checks cover simulated accounts, installer maintenance, and the Lua bridge across real processes. Single-player gameplay and an authenticated PB upload were previously verified in XSanity with logging disabled. Two-player routing is covered through the real Lua mailbox and a simulated API; live two-player gameplay verification is still pending.
