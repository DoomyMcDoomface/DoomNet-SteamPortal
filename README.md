# DoomNet

Open `D:\Documents\Repos\DoomNet.code-workspace` so this repo, SteamTunes, and SteamSkin are one window. Agents only see the folders in the workspace that is open.

Windowless host that will stage Doomy's SteamTunes and Steam-Skin inside the Steam client. This tree is the skeleton. It compiles to a Windows-subsystem executable that returns immediately. It does not inject, embed Lua, or start Steam.

Behavior lives in the spec in this repo:

`own_steam_loader_a1dd8dbe.plan.md`

Fill the stubs in the spec's build order. Scaffolding stopped before Lua and before `steam_attach`.

## Read these when a stub does not say

Do not edit them while filling this repo, and do not copy them in as submodules.

- Player: `D:\Documents\Repos\SteamTunes`. If the spec and a file disagree, the file wins.
- Skin: `D:\Documents\Repos\SteamSkin`. Payload source. The installed theme at `C:\Program Files (x86)\Steam\millennium\themes\Steam-Skin` has hand-edited bootstrap query strings. Read both when they differ. Install the repo copy unless the installed file shows a behavior the repo lost.
- Millennium: `https://github.com/SteamClientHomebrew/Millennium` branch `main`.
- Steam on this PC: `HKCU\Software\Valve\Steam\SteamPath`, then `HKLM\SOFTWARE\WOW6432Node\Valve\Steam\InstallPath`, then `C:\Program Files (x86)\Steam`.

## What is already fixed

- One executable, `DoomNet`, subsystem WINDOWS. No console. A second process exits immediately.
- Each launch writes a sentence per step to `logs\doomnet.log` (`startup`, `instance`, `steam`, `config`), including Windows error text. `DOOMNET_ROOT`, when set to an absolute path, is the data root. Otherwise it is `%LOCALAPPDATA%\DoomNet`.
- `config.json` is loaded when present. A bad file is an error and is not half-applied. Files over 64 KiB are rejected.
- Steam is looked up and not started. `launch_hidden` refuses `steam.exe`.
- The setup window can be created hidden and driven by tests. Startup does not show it.
- Payloads copy into a chosen folder and refuse the Steam install. A shortcut can be retargeted in a caller-chosen folder while its icon path stays put.
- Every `src\*.cpp` file is linked. `wWinMain` calls one empty anchor in each. Removing a file from `CMakeLists.txt` fails the link. The anchors allocate nothing. Replace the function body when you fill that file. Do not hang `wWinMain`.
- `src\boot_script.js` is comments only. It is not compiled and not injected.
- `src\steam_attach.cpp` stays empty until the windowless exe and the Lua host work.
- Two threads, later: the state thread is the only one that enters Lua. The browser thread never calls `lua_*`. No thread pool. `utils.sleep` yields. A second instance must exit, not wait.
- The process that exists today must not stay resident. The skeleton test kills the run if an instance is still alive after two seconds.

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
