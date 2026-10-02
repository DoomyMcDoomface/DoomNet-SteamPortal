# DoomNet

Windowless host that runs Doomy's SteamTunes and Steam-Skin inside the Steam client.

- Player: SteamTunes **1.14.1**. That number is `plugin.json` and the About line.
- Skin: Steam-Skin **0.9.27** (`skin.json`).

Open `D:\Documents\Repos\DoomNet.code-workspace` so this repo, SteamTunes, and SteamSkin are one window. Agents only see the folders in the workspace that is open.

Behavior lives in the spec in this repo:

`own_steam_loader_a1dd8dbe.plan.md`

## Read these when a stub does not say

Do not edit them while filling this repo, and do not copy them in as submodules.

- Player: `D:\Documents\Repos\SteamTunes`. If the spec and a file disagree, the file wins.
- Skin: `D:\Documents\Repos\SteamSkin`. Payload source. The installed theme at `C:\Program Files (x86)\Steam\millennium\themes\Steam-Skin` has hand-edited bootstrap query strings. Read both when they differ. Install the repo copy unless the installed file shows a behavior the repo lost.
- Millennium: `https://github.com/SteamClientHomebrew/Millennium` branch `main`.
- Steam on this PC: `HKCU\Software\Valve\Steam\SteamPath`, then `HKLM\SOFTWARE\WOW6432Node\Valve\Steam\InstallPath`, then `C:\Program Files (x86)\Steam`.

## What stays true

- One executable, `DoomNet`, subsystem WINDOWS. No console. A second process exits immediately. With no arguments it does not stay running. `--run` and `--watch` stay running and do not start `steam.exe`.
- Each launch writes a sentence per step to `logs\doomnet.log`, including Windows error text. `DOOMNET_ROOT`, when set to an absolute path, is the data root. Otherwise it is `%LOCALAPPDATA%\DoomNet`.
- `config.json` is loaded when present. A bad file is an error and is not half-applied. Files over 64 KiB are rejected.
- `launch_hidden` refuses `steam.exe`. Payloads refuse a destination inside the Steam install.
- The state thread is the only one that enters Lua. The browser thread never calls `lua_*`. No thread pool. A second instance exits and does not wait.

## Build

From this directory, with the CMake that ships in Visual Studio Build Tools:

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --build build --config Debug
```

## Check

```
powershell -NoProfile -File tests\Check-Contracts.ps1
powershell -NoProfile -File tests\Check-Skeleton.ps1
```

`Check-Contracts.ps1` does not build and does not start Steam. It reads SteamTunes and SteamSkin and fails if a fact this host must match has moved, or if `steam_attach.cpp` or `boot_script.js` grow a body before those gates are updated on purpose. `DoomNetFoundationTests.exe` checks config, the log, Steam discovery, and the color rules. Rules for the next model are in `AGENTS.md`.
