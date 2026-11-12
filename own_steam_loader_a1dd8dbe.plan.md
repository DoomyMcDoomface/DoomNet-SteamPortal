---
name: DoomNet loader
overview: DoomNet is built in D:\Documents\Repos\DoomNet-SteamPortal. The windowless foundation, payload copy, uninstall guards, and the accepted setup-window preview are already in that repo. Lua, injection, playback proof, and wiring the setup window to a real install are not. This SteamTunes repo stays the player.
todos:
  - id: installer
    content: DoomNet installer finds the Steam folder and offers the player and the skin as separate choices. If the player is installed with the skin, it asks whether the player starts on Steam Blue or matches the red-and-black skin. It does not install or remove Millennium.
    status: pending
  - id: millennium-toggle
    content: Add a quiet DoomNet control to turn Millennium off and back on by stashing its Steam hook files, without deleting plugins, themes, or saved player data, and without opening a console.
    status: pending
  - id: silent-start
    content: DoomNet and every helper it starts are windowless. The Steam shortcut opens Steam directly, with no command prompt before the client. The accepted setup window is the only exception, and only while installing.
    status: pending
  - id: shim-host
    content: DoomNet embeds Lua on a state thread and shims millennium, fs, json, and logger. A second thread owns the browser connection, sleeps, and file waits. Lua yields across those waits so transport still runs.
    status: pending
  - id: bridge
    content: Map Millennium.callServerMethod and call_frontend_method onto that process without changing the player UI's call shape.
    status: pending
  - id: inject-skin-and-player
    content: Inject only the chosen pieces. Player means bootstrap.js plus the Lua backend. Skin means skin.json patches, without using the skin to load the player.
    status: pending
  - id: remove-millennium
    content: After DoomNet is proven, delete the Millennium publish scripts. A later paired rename can drop the millennium module name from the player and from DoomNet together. Do not rename during the host build.
    status: pending
  - id: prove-playback
    content: With Millennium toggled off, confirm player-only, skin-only, and both. A normal play fetches the original file by track id with no steamui copy. Toggle Millennium back on afterward for comparison.
    status: pending
  - id: wire-setup-window
    content: The accepted setup window runs DoomNet.exe --install. Use tools/wizard.xaml and tools/DoomNetSetup.cs. Do not design a replacement. Install copies into the DoomNet data folder and does not start Steam.
    status: pending
isProject: false
---

# DoomNet implementation spec

DoomNet, Doomy's SteamTunes, and Steam-Skin are three products. They do not share source, and none of them calls a new API on the others.

This repository is only Doomy's SteamTunes. It stays a Millennium Lua plugin: [plugin.json](plugin.json), [backend/](backend/), and the page scripts. DoomNet source is not added here. There is no `doomnet/` directory, no DoomNet modules under `backend/`, and no `require` of DoomNet from Lua or JS. The player must still load under Millennium on this PC with these files unchanged. Host behavior (wide file opens, yielding sleeps, the stream URL) is implemented in DoomNet by replacing host functions and, after `main.lua` loads, replacing the global `stage_track_for_playback` when the stream hook is up. Do not add `utils.read_range` or any other DoomNet-only function to [backend/main.lua](backend/main.lua). Millennium's `utils` module will not have it, and the comparison install would break.

Steam-Skin stays its own theme. It is not moved into this repo. Choosing a folder for DoomNet inside SteamTunes would not pull the skin along, and it should not. The skin is a folder with `skin.json` and its patch files. DoomNet copies that folder only when the user asked for the skin. The player does not read `skin.json`. The skin does not load the player. `music-player-inject.js` is skipped so a skin-only install does not bring the player in, and a player-only install does not apply skin CSS.

DoomNet is a separate program with its own version. Its source root is `D:\Documents\Repos\DoomNet-SteamPortal`. That tree is no longer empty. The low-level host foundation and the accepted setup window are already there. Lua, injection, and a real install from that window are not. Nothing is added under `D:\Documents\Repos\SteamTunes` except the later packaging cleanup, after playback is proven. The player payload is read from `D:\Documents\Repos\SteamTunes` (`backend\` plus `plugin.json`) and copied at install time. The skin payload is the Steam-Skin directory the installer was pointed at, not a folder inside either repo. Install copies land here:

- Player payload: this repo's `backend\` plus `plugin.json`. Installed to `%LOCALAPPDATA%\DoomNet\player\`. Player data stays inside that copy.
- Skin payload: the Steam-Skin directory the installer was pointed at. Installed to `%LOCALAPPDATA%\DoomNet\skin\`. Absent when the user did not install the skin.

## Where to read when the plan is silent

Writes for the host go in `D:\Documents\Repos\DoomNet-SteamPortal`. The trees below are context. Read them when this document does not specify a behavior, a filename, a patch, or a path. Do not invent a stand-in. Do not copy those trees into the DoomNet repo as submodules. Do not edit them while building the host.

- Player: `D:\Documents\Repos\SteamTunes`. Lua, page scripts, `plugin.json`, and the IPC names. This plan cites that tree. If a citation and the file disagree, the file wins.
- Skin: `D:\Documents\Repos\SteamSkin`. `skin.json` and the patch files are the payload. The copy installed for Millennium is `C:\Program Files (x86)\Steam\millennium\themes\Steam-Skin`. That installed copy has been hand-edited (bootstrap query strings in `music-player-inject.js` and `webkit.js`). When the repo and the installed theme disagree, read both, and take the repo as what DoomNet installs unless the installed file shows a behavior the repo lost.
- Millennium: `https://github.com/SteamClientHomebrew/Millennium` branch `main`. Use it for how the current Windows bootstrap, the Lua host API, and theme patch matching actually work. Pinned files already named here are the bootstrap README, `src/bootstrap/win32/main.c`, `src/include/millennium/steam_hooks.h`, `src/lua_host/api/millennium.cc`, and commit `143e26d`. Read further files in that repo when the hook, the module table, or a theme field is not fully specified above.
- Steam on this PC: resolve the install with `HKCU\Software\Valve\Steam\SteamPath`, then `HKLM\SOFTWARE\WOW6432Node\Valve\Steam\InstallPath`, then `C:\Program Files (x86)\Steam` if `steam.exe` is there. List that directory for hook DLL names, `steamwebhelper.exe`'s real folder, `steamui`, and the live `millennium` tree before hardcoding a path. Reading is allowed. Writing into that tree happens only at the test and install steps this plan already describes, with Millennium's hooks stashed first.

## What has already been built

This is the state of `D:\Documents\Repos\DoomNet-SteamPortal` as of 2026-09-23, after the foundation work and the accepted setup preview. Read this before writing host code. If this section and a source file disagree, the file wins, then update this section in the same change. Do not rebuild a piece that is already implemented and tested. Do not treat a comment-only file as finished just because it is on disk.

SteamTunes and SteamSkin were not edited for this work. The real Steam shortcut was not retargeted. `steam.exe` was not started. Nothing was injected. `DoomNet.exe` does not show a window and does not call `apply_install`. With no arguments it does not stay running. `--run` and `--watch` stay running and, when the player is installed, start the Lua host. They do not start `steam.exe`.

Open `D:\Documents\Repos\DoomNet.code-workspace` when one window should see DoomNet, SteamTunes, and SteamSkin together. `README.md` and `RULES.md` repeat the rules. `RULES.md` is the short gate list. This section is the inventory.

### How it builds

C++20, Visual Studio 17 2022, x64. CMake is not on `PATH`. The Build Tools copy that configured this tree is `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`. From the DoomNet repo:

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`CMakeLists.txt` defines `doomnet_apply`: `UNICODE`, `WIN32_LEAN_AND_MEAN`, `NOMINMAX`, MSVC `/W4 /WX /permissive- /utf-8 /guard:cf`, link `/GUARD:CF /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA`, libraries `advapi32 ole32 uuid`. There is no `find_package`. Lua is not fetched. `src\boot_script.js` is not a compiled source. `Check-Contracts.ps1` fails if that changes.

Targets:

- `DoomNet.exe`, subsystem WINDOWS (`add_executable(DoomNet WIN32 ...)`). Entry is `wWinMain` in `src\main.cpp`. It links every file in `DOOMNET_CORE` and returns on the no-argument path. There is no image size cap. The host embeds Lua 5.4 and a JSON library (`third_party\`), and stability and clarity come before a small file. The resident host, the Lua state, and anything that starts Steam run only behind `--run` or `--watch`. The no-argument path is the one `tests\Check-Skeleton.ps1` measures.
- `DoomNetFoundationTests.exe`, console test binary, same core.
- `DoomNetDummyChild.exe`, a WINDOWS-subsystem child used only to prove hidden launch. It writes `hidden-ok` to the path in `DOOMNET_CHILD_MARKER` and exits.
- `DoomNetWizardMock.exe`, a WINDOWS-subsystem launcher. It does not draw the wizard. It `CreateProcess`es `DoomNetSetup.exe`.

`DoomNetSetup.exe` is not a CMake target. It is `tools\DoomNetSetup.cs` compiled with `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /target:winexe` against the .NET Framework 4 WPF references (`PresentationFramework`, `PresentationCore`, `WindowsBase`, `System.Xaml`, `System.Windows.Forms`). The output is `build\Release\DoomNetSetup.exe` with `wizard.xaml` copied beside it. The program loads that xaml from its own directory. Subsystem is 2, so Windows Terminal does not open. Do not put this UI back inside `powershell.exe`.

### What `DoomNet.exe` does on startup

`wWinMain` calls every `*_anchor()` in `src\anchors.h`, then:

1. Resolves the data root. `data_root` does not fall through: a relative path or a path containing `..` is an error and is not used as the root. `wWinMain` then clears `DOOMNET_ROOT` so that rejection can be logged under `%LOCALAPPDATA%\DoomNet`, and exits 0.
2. Logs `Process started. No console is attached. This build does not start Steam and does not inject.`
3. `acquire_instance()`. Failure logs that `Local\DoomNet` is already held and exits 0 without writing config.
4. `discover_steam_path`. A miss is a warning. Steam is not started.
5. `load_config_file` on `<root>\config.json`. Missing, ok, and error each get a full sentence. Both-selected, player-only, and player-off each add a sentence about what an installer would do. Startup does not show a window and does not write `settings.json`.
6. Logs `Startup checks finished. Steam was not started and nothing was injected.` With no arguments, or with an argument that is not `--run` or `--watch`, it returns 0. An unknown argument is logged by name and does not stay running.

The no-argument process must not stay resident. `tests\Check-Skeleton.ps1` launches with no arguments and fails if a `DoomNet` process is still alive two seconds after the launches. Do not hang that path on a wizard, a dialog, a Lua state, or a Steam wait.

`--run` and `--watch` are the only resident paths that do not start Steam. Both keep the `Local\DoomNet` mutex until `Local\DoomNetStop` is signaled or `<root>\stop.flag` exists. A `stop.flag` that is already there at startup is logged and the process leaves without starting the host. The Lua host starts only when `config.json` has `player` true and `<root>\player\backend\main.lua` exists. Otherwise a sentence says why it was not started, and the process still waits (so `--watch` keeps the mutex). Neither path calls `launch_hidden` and neither starts `steam.exe`. `--watch` is the later shortcut watcher; this build only holds the process and logs that it will not start Steam. `--attach` is different: DoomNet's bootstrap, once it is inside `steam.exe` and the top-level `steamwebhelper` starts, launches `%LOCALAPPDATA%\DoomNet\DoomNet.exe --attach` when `Local\DoomNet` is free and `DOOMNET_ROOT` is unset. The tester install places that bootstrap as `version.dll`, because `steam.exe` imports `VERSION.dll` at process start and only delay-loads `WSOCK32.dll`. The same hook still builds as `wsock32.dll` for the fake-steam tests. `--attach` accepts either file. The skin is read from `<root>\skin` when `D:\Documents\Repos\SteamSkin` is not on the machine, and the player files are read from `<root>\player\backend\assets\frontend` when the SteamTunes repo is not there. Copying those files into the DoomNet data folder does not by itself put them inside Steam. That process injects into the Steam the user just opened and stays up, so the next launch is covered without starting a second host. It does not start `steam.exe` itself.

### Logging, the single instance, and the data root

`src\log.cpp` appends UTF-8 CRLF lines to `<data root>\logs\doomnet.log`. The shape `Check-Skeleton.ps1` requires is `YYYY-MM-DDTHH:MM:SS.mmmZ info|warn|error ...`. `log_event` writes `timestamp level pid=N component: sentence`. Newlines inside a sentence become spaces. A line stops at 4096 characters and then ` [truncated]`. The write mutex is `Local\DoomNetLog`, waited at most one second. `windows_error_text` is `Windows error N (system message)`. Components already used: `startup`, `instance`, `steam`, `config`, `payload`, `launch`, `shortcut`, `installer`, `wideio`, `uninstall`, and, from the Lua host, `lua` (a script's own `logger`/`print` output and host steps) and `browser` (timers, page calls, child processes). Later phases add `cdp`, `relay`, `hook`, `stream`, `inject`, `toggle`. Keep that shape. Do not log a bare number.

`src\instance.cpp` creates mutex `Local\DoomNet` with initial ownership. A second call in the same process, while this process already holds it, returns true. Another process gets `ERROR_ALREADY_EXISTS`, closes the extra handle, and returns false immediately. It does not wait.

`src\paths.cpp`: `DOOMNET_ROOT` is the data root only when it is absolute and contains no `..`. Otherwise the error names the rejected value and `data_root` does not switch to AppData. An unset override uses `%LOCALAPPDATA%\DoomNet`. `ensure_directory` creates one directory when its parent already exists.

### `config.json`

`src\config.cpp` accepts `steamPath` (string), `player` (bool), `skin` (bool), `playerColor` (`steam` or `red`), optional `streamTunesRelay`, and optional `updates`. Save order includes those keys. The file is capped at 64 KiB. Unknown keys, a repeated key, a color other than `steam` or `red`, a `steamPath` containing `..`, invalid UTF-8, and a Windows failure on open or write are errors. The error string is a full sentence. A failed load does not change the caller's `Config`. Missing file is `ConfigLoad::Missing` and leaves the defaults: player off, skin off, color steam, empty path.

### Steam discovery

`src\steam_discover.cpp` tries, in order, `HKCU\Software\Valve\Steam` value `SteamPath`, then `HKLM\SOFTWARE\WOW6432Node\Valve\Steam` value `InstallPath`, then `C:\Program Files (x86)\Steam`. A candidate is kept only when `steam.exe` is in that directory. `..` is rejected. Each probe is one `steam:` log line, including the rejection and the reason. Failure does not start Steam.

The registry value on this PC is lowercase (`c:/program files (x86)/steam`). Discovery returns that string. The setup window is the place that turns it into real on-disk casing. Do not "fix" discovery by title-casing the registry text. Walk the directory entries, the way `FormatWindowsPath` in `tools\DoomNetSetup.cs` does, and use the names Windows returns.

### Wide files, hidden launch, and shortcuts

`src\wide_io.cpp` is `CreateFileW`. `wide_file_exists`, `write_new_file`, and `read_entire_file` reject `..`. Reads refuse files over 1 MiB. The installer uses it. The Lua `io` and `fs` modules are `src\host_io.cpp` and `src\host_fs.cpp`, and they open files themselves. The no-argument process does not create a Lua state.

`src\launch.cpp` `launch_hidden` uses `CREATE_NO_WINDOW` and `STARTF_USESHOWWINDOW` / `SW_HIDE`. It refuses an empty path, a path containing `..`, and any target whose file name is `steam.exe` (case-insensitive). The refusal text is `Refusing to start steam.exe`, which `Check-Contracts.ps1` requires. On timeout it terminates the child. The guard stays until `steam_attach` exists and is tested. Do not point this function at the real client to "see what happens."

`src\shortcut.cpp` creates, retargets, and reads an `IShellLink` in a caller-chosen folder. It does not search the desktop. A new target whose file name is `steam.exe` is refused. The icon path is an argument and is not switched to `steam.exe`. COM is `CoInitializeEx`; uninitialize only when that returned `S_OK`.

### Install and uninstall logic

This is library code. `wWinMain` does not call it. The accepted setup window does not call it yet.

`apply_install` in `src\payload.cpp` copies into a caller-supplied root:

- Player, when selected: `<player_source>\plugin.json` and `<player_source>\backend\` into `<root>\player\`. The foundation test has copied the real `D:\Documents\Repos\SteamTunes` tree this way, player only, and checked `plugin.json` and `backend\main.lua`.
- Skin, when selected: the skin source tree into `<root>\skin\`.
- Then `<root>\config.json` with the four keys. `playerColor` is `red` only when player, skin, and `match_skin` are all true. Otherwise `steam`.
- `uiColorTheme` is written only when the player was selected and `<root>\player\backend\data\settings.json` does not exist. The body is one key, `{"uiColorTheme":"red"}` or `{"uiColorTheme":"steam"}`. `theme_for_new_settings` in `src\installer.cpp` is the rule: null when settings already exist or the player is off; `steam` for player-only; `red` only when both pieces are selected and the user chose Match Steam-Skin. A second install must not overwrite `settings.json`. The test covers that.

Refusals, all logged under `payload` with a full sentence: empty root, `..` in the root, destination inside the discovered or requested Steam directory, and neither piece selected. Copy refuses a path containing `..`.

`apply_uninstall` removes a DoomNet install and skips a piece that was never there:

- Refuses an empty path, `..`, a drive root, a folder that contains `steam.exe`, the SteamTunes source ( `plugin.json` plus `backend\` and no `config.json` ), and the Steam-Skin source ( `skin.json` and no `config.json` ). The tests call it on `D:\Documents\Repos\SteamTunes` and on the real Steam directory and require `plugin.json` and `steam.exe` to still exist.
- Reads `config.json`. A player or skin folder that config did not select and that is absent is skipped. A selected folder that is already gone is skipped.
- If `steamui` is set, deletes only `<steamui>\steam-music-player` and `<steamui>\doomnet-skin`. Other files in `steamui` stay.
- Deletes the shortcut path when it is set and present. A missing shortcut is skipped.
- Deletes the DoomNet root last. Do not log after that delete. A log line would create `<root>\logs` again. The success sentence is returned in `UninstallResult.detail` and is not written once the folder is gone.
- `delete_tree` treats a missing path as already gone, skips reparse points so a link cannot walk out of the folder, and stops at 32 levels.

`color_question_needed` is true only when both pieces are selected. `create_install_window` is a separate hidden Win32 dialog (`DoomNetInstall`, created at -32000,-32000, then `SW_HIDE`) with checkbox and radio ids `kInstallIdPlayer` through `kInstallIdCancel`. Tests drive it with `SendMessage`. It does not copy files. It is not the window users see. Do not skin it, do not show it at startup, and do not replace `wizard.xaml` with it.

### The accepted setup window

The visual shell is done and was accepted on screen. The contract for pages, colors, bubbles, path casing, and the slim title bar, sidebar, fields, and buttons is the Installer section below. The code that matches it:

- `tools\wizard.xaml` is the layout.
- `tools\DoomNetSetup.cs` is the behavior: page order Welcome, Choose, Color, Finish; Color is skipped unless both pieces are selected; Next on Choose is disabled until one piece is selected; Browse uses `FolderBrowserDialog`; the Steam path is prefilled from `HKCU\Software\Valve\Steam\SteamPath`, then `HKLM\SOFTWARE\WOW6432Node\Valve\Steam\InstallPath`, then `C:\Program Files (x86)\Steam`, then rewritten with `FormatWindowsPath` so the field shows real Windows casing (`C:\Program Files (x86)\Steam` on this PC).
- `src\wizard_mock.cpp` only finds and starts `DoomNetSetup.exe`. It does not draw.
- Finish's Install button copies the payload into the user's DoomNet folder and runs `DoomNet.exe --hook` for the Steam folder on screen. Closing the window without Install copies nothing. `DoomNet.exe` itself does not open this window, and a later Steam launch does not show it.

### Files that are still only a comment and an empty anchor

These compile. Their only code is `void <name>_anchor() {}` plus a comment that names the spec behavior. Filling one means adding the real code beside that empty function. `tests\Check-Contracts.ps1` searches each mapped cpp for the exact text `void <name>_anchor() {}`. Deleting the empty function fails the test even after the file has a real implementation. The implemented files keep both.

The Lua host is implemented: `src\state_thread.cpp`, `src\browser_thread.cpp`, `src\host_fs.cpp`, `src\host_io.cpp`, `src\host_json.cpp`, `src\host_logger.cpp`, `src\host_utils.cpp`, `src\host_millennium.cpp`, plus `src\resident.cpp` for `--run` and `--watch`. `DoomNetHostTests` loads the real SteamTunes backend and checks `get_settings` and `get_library_info`. The page bridge is `src\page_bridge.cpp`: `window.Millennium.callServerMethod` goes through the `doomnet` binding, and `call_frontend_method` runs on that page. With no page connected, `call_frontend_method` still resumes with nil. Do not invent `utils.hash`, `utils.base64_encode`, `utils.exec`, or `fs.join`. `fs.join(nil, ...)` must not throw.

Still scaffolds:

- `src\stream.cpp` is implemented. It serves `https://steamloopback.host/steam-music-player/stream/<token>/<id>` from the original file. A play does not copy that file into `steamui` once the Steam page can intercept the address. The empty `stream_anchor()` stays.
- `src\steam_attach.cpp`. Places DoomNet's `wsock32.dll`, refuses while Millennium's copy is still beside `steam.exe`, and starts a `steam.exe`. It waits until `steamui.dll` is loaded and `steamwebhelper.exe` is running, then `CdpPipe` sends `Target.setDiscoverTargets` and attaches with `Target.attachToTarget` only to a page whose URL is `steamloopback.host` or `steampowered.com`, or whose title is exactly `Steam`. It does not send `Target.setAutoAttach`. The shared script context is a normal `steamloopback.host` page, and the boot script uses its `g_PopupManager` to reach the desktop popup. `GetPopups()` returns a map iterator, not an array. `SteamPage` keeps the pipe open and answers `doomnet` calls. Fake tests prove the probe round trip. A live run on this PC answered `doomnet_probe` with `live-ok`, and Steam's log recorded `SP Desktop_uid0-'Steam'` unhidden at 1274x720 with a visible window titled Steam. A later skin-only run copied Steam-Skin into `steamui\doomnet-skin`, skipped every `music-player-inject.js` patch, and the popup document's `--steam-neutral-bg-start` was `rgb(28, 28, 28)`. That run left Millennium stashed in `doomnet-stash` and DoomNet's `wsock32.dll` beside `steam.exe`. The player was not loaded. The anchor stays empty. The hook stays in the DLL. This file must not contain `WriteProcessMemory`, `CreateRemoteThread`, `VirtualAlloc`, `MinHook`, or `addScriptToEvaluateOnNewDocument`.

`src\toggle.cpp` is implemented: `millennium_off` stashes the four hook files (`wsock32.dll`, `version.dll`, `millennium-legacy.version.dll`, `user32.dll`) into `<steam>\doomnet-stash` with a marker; `millennium_on` restores them; `millennium_state` reports Active, Stashed, or NotPresent; `is_millennium_bootstrap` tells Millennium's `wsock32.dll` from DoomNet's by the `__get_shim_version` export. It refuses an empty path, a `..` path, a folder with no `steam.exe`, and a folder inside Windows, and (for the real menu action) refuses while Steam is running. `DoomNetFoundationTests` runs a full off/on cycle against a fake Steam directory. System32 is never touched.
- `src\boot_script.js`. The real page binding. It defines `window.Millennium.callServerMethod`, skips iframes, and does not fetch a loopback address. When `window.__doomnetConfig` is set, it inserts matching skin CSS and JS into the current document and into Steam popups from `g_PopupManager`. It never inserts `music-player-inject.js`. It is not a compiled source. `launch_and_inject_boot` sends it once a page target attaches, and `SteamPage` leaves the pipe open so a later call is a Lua function and the result is `__doomnetSettle`. A fake pipe and the installed Steam client both proved that round trip with a probe function. The host test also proves the script on a local Edge page.

### Tests that already pass and what they lock

`tests\Check-Contracts.ps1` does not build and does not start Steam. It reads the sibling folders `..\SteamTunes` and `..\SteamSkin` (same parent as the DoomNet repo). It fails if any of these move: plugin name `SteamMusicPlayer`, version `1.15.0`, `backendType` lua; `CHUNK_SIZE = 512 * 1024`; `require("millennium")`, `add_browser_js`, `call_frontend_method`, `ready()`; bootstrap `ASSET_REV` `20261027-version-1.15.0`, `mix-image.js`, `__steamMusicPlayerBootstrap`, `window !== window.top`; both JS files call `Millennium.callServerMethod(PLUGIN_NAME, method, argsArray || [])`; `__SMP_RECEIVERS__`; `AUDIO_CHUNK_TIMEOUT_MS = 8000`; `byteLength !== info.size`; `fs.join(nil, ...)`; skin.json `UseDefaultPatches` true and `TargetJs` `music-player-inject.js`. It also checks this spec still names `D:\Documents\Repos\DoomNet-SteamPortal` and `steam_attach`, and that CMake still lists the wide-I/O, launch, shortcut, payload, and dummy-child sources.

`tests\Check-Skeleton.ps1` sets `DOOMNET_ROOT` to a fresh temp directory and runs the Release `DoomNet.exe`: 3 warmup launches, 100 sequential, 32 parallel, 30 working-set samples. Every run must exit 0 within 2 seconds. The image must be subsystem 2 with `DYNAMICBASE`, `NXCOMPAT`, `HIGH_ENTROPY_VA`, and `GUARD_CF`. No `DoomNet` process may remain. Peak working set must be at or under 32 MB on this no-argument path, where no Lua state is created. The image size is printed and not capped. The log must be one record per line. Counts of `Process started`, `Acquired Local\DoomNet` plus `already held`, and `Startup checks finished` must match the launches. Do not weaken this test to make a resident process or a console subsystem pass. A Release run during the foundation work was about 76 KB, p50 about 46 ms, max 58 ms, peak working set about 9.7 MB. Re-run the test after a change. Do not treat those timings as a new requirement beyond the limits above.

`DoomNetFoundationTests` covers path rejection, config round-trip and the rejection wording (a bad color must mention `playerColor`), the color rules, 8 threads writing 200 log lines each without a torn line, Steam discovery on this PC, a Unicode path `音楽-café.txt`, hidden launch of the dummy child, refusal to launch `steam.exe`, a shortcut whose icon stays `notepad.exe`, the hidden install dialog, a fixture install of player plus skin with red settings and a second install that does not overwrite them, refusal to install into the real Steam directory, a copy of the real SteamTunes player with skin off, uninstall of a player-only install skipping the skin, a second uninstall skipping both, and refusal to uninstall the SteamTunes repo and the real Steam directory. The last test holds `Local\DoomNet` until the test process exits, then starts `DoomNet.exe` and expects the log to say the mutex is already held. Temp directories include the process id and a counter so two tests do not share one log.

Run `Check-Contracts.ps1` and `Check-Skeleton.ps1` before and after a host change. `Check-Contracts.ps1` does not know about `wizard.xaml`. Breaking the wizard will not fail that script.

### What this plan still requires, and in what order

The original todos are not done, except that the foundation they sit on is. Do the rest in this order. Do not skip ahead to injection.

1. Done. Lua 5.4 on the state thread, the shims, and `--run` / `--watch`. `DoomNetHostTests` loads the real backend. No injection. The browser thread does not enter Lua.
2. Done for a local page. `Millennium.callServerMethod` and `call_frontend_method` use the boot script and `src\page_bridge.cpp`. Do not rename `millennium` to `host` yet. `steam_attach` is what points this bridge at Steam.
3. Done for the boot script on the real client. `wsock32.dll` forwards socket exports and, only inside steam.exe, hooks `CreateProcessInternalW` so the top-level steamwebhelper gets `--remote-debugging-pipe`. One relay thread moves bytes both ways on `\\.\pipe\DoomNet.cdp.<pid>`. `launch_and_inject_boot` discovers targets and attaches to a real Steam page. It does not send `Target.setAutoAttach`. `SteamPage` answers `doomnet` calls. `DoomNetHostTests` settles `doomnet_probe` to `pipe-ok`. On this PC the same probe returned `live-ok`, and Steam's log recorded `SP Desktop_uid0-'Steam'` unhidden at 1274x720. `--run` still does not start Steam. The toggle is done (`src\toggle.cpp`).
4. Skin-only is done on the main window. `src\skin_page.cpp` reads `skin.json`, skips `music-player-inject.js`, does not apply `UseDefaultPatches`, and copies the css and js into `<steam>\steamui\doomnet-skin`. The boot script inserts the matching tags. On this PC the Steam popup loaded `libraryroot.custom.css` and `--steam-neutral-bg-start` was `rgb(28, 28, 28)`. The player bootstrap is inserted on the Steam window. Store and community documents get the same player inlined, because those pages block a steamloopback script address and are often a frame. The live host loads the SteamTunes backend. A track played on this PC. The player data folder's `art` and `art_jpeg` directories are links to `DoomNet Art` inside the first music folder. That folder is filled by moving the Millennium plugin cache there, so removing Millennium does not remove the covers. A cover the page cannot fetch is read from that cache. Store and community pages use the same cover address the library uses. The host answers that image request from the DoomNet Art cache, and a cover already read as bytes is painted on every tile of that page. A click updates that page immediately. A player-state reply that left before the click is ignored, and the new page and playback are pushed to every open Steam page instead of waiting for the next poll. Artist links are only as wide as their text. The client nav gets a SteamTunes tab after Community, matched by the Store, Library, and Community labels. That tab fills the area under the bar with the player. Selecting it takes the accent from the tab that had it and paints the SteamTunes label and underline. Store, Library, and Community hide it and show Steam's own page. While the player is open, a collapsed browser rectangle is not saved. Closing that tab restores only the main page under the nav and leaves the other browser views collapsed, so stacked views do not steal some of the clicks. Store, news, app, and community documents stay skinned and do not get a floating player. The overlay still gets the compact player. That bar and its button sit on the shortcut row, and the open panel starts above that row. SteamTunes publishes album covers at their embedded size, so `src\art_thumbs.cpp` runs one background-priority thread, outside Lua, that writes a JPEG of at most 400px for each cover in `steamui\steam-music-player\art\albums` into `art\thumbs`, and walks the folder again every minute. The host's patched player UI loads the thumbnail on grid tiles and falls back to the full cover. The page loads a thumbnail when a tile is drawn. It does not prefetch the whole library, because that hung steamwebhelper. The Lua thread runs playback calls before artwork calls and an artwork call yields when a song is waiting to load. The reply for a play call is sent to the page before queued poll replies. A short play reply goes out before a large audio chunk, and a duplicate page call is handled once. The play reply goes to the Steam window first. A slow overlay does not hold that reply past the page's 8 second wait. A library page the user has already left is not applied when its reply arrives late. Stale poll replies are dropped. Settings, music folders, and the library summary jump ahead of those polls, and their answers go to every page that asked, so the game overlay shows the same folders, color, and track names as the Steam window. The artwork size control is only on the Artist, Album, and Genre grids in the main window. It offers five sizes, 14, 12, 10, 8, and 6 covers across. The game overlay uses four covers across and does not offer Set Cover. It paints the saved library as soon as that copy is on disk, and does not wait for the server before leaving Loading library. Opening Settings draws that page immediately. A play call is answered before the duck poll and the player-state poll. Extra polls share the answer already in flight, so they do not fill the queue the page is waiting on. Thumbnail files are built on a background thread, pinned to another CPU when the machine has one. On the SteamTunes tab the Now Playing page keeps the cover beside the track text and the queue under that same column, and the transport groups the seek bar with the controls instead of stretching it across the window. Previous restarts the current song after the first five seconds and skips to the prior song before that. Settings, Misc Features, has Global Control. Turning Media Keys off clears Global Control and greys that box until Media Keys is on again. A settings readout keeps a fixed width, so a longer loudness description does not resize the slider beside it. When that box and Media Keys are on, the process main thread takes play, pause, stop, next, previous, and the volume keys even when Steam is not in front, and Windows and other players do not receive them. That thread posts `transport_command` and does not call Lua. The Music folders Remove button stays beside its path instead of stretching to the far edge. The cover stays centered, with the title under it. Shuffle and repeat sit beside previous, play, and next. The progress bar is centered under that transport. The bottom transport stays hidden while that tab is stacked. An accent arrow in the lower right of Now Playing switches that stack for the cover beside the song. Beside the song, the bottom transport returns and only shuffle and repeat stay with the title. The equalizer is 100 pixels from the left edge of Now Playing and the lyrics are 100 pixels from the right edge, at every window size, and neither moves when the cover and the song change places. The lyrics column is the same width as the equalizer. Beside the cover, the song and the list sit 50 pixels to the right of the midpoint, so the space before the lyrics is 50 pixels smaller. The stacked cover stays centered. Their width and height scale with the window. When the window is too narrow for that, the margin shrinks to 16 pixels, and below that both sit in a row under the cover. The cover, the song, and the list stay the same design. Stacked, they sit centered between the equalizer and the lyrics. A longer or shorter title changes that block's width and the block centers itself again. Beside the cover, the song block keeps fifty pixels of padding on each side. The equalizer is the active 10-band EQ, with the meter under it. The library scan bar is shown only on the Settings tab. An eye in the top left of Now Playing hides or shows the equalizer and the lyrics, and it uses the accent color. The sliders keep the height they had beside the cover. The spectrum and loudness meter sit under that EQ at the same width and extend below the cover. A short playlist still scrolls far enough to show the rest of that block. The spectrum bars use the accent already on the page. Under that meter, with two lines of space, Target Loudness, Ducking Strength, and Space Amount move the same settings as the sliders in Settings and use that accent. Those three names are centered on the bar. Each bar is the same length as the spectrum, and its number sits beside the bar in the same column as the dB readout. The loudness readout is the number and LUFS only. The space right of the cover is scrolling lyrics, faded at the top and bottom of that same frame, read from the song file or a matching LRC file. The lines stay inside that frame. A Lyrics label sits above that frame, in the same style as the EQ preset name, and the two labels share the same vertical position. When the lines are longer than the frame they wait, crawl to the bottom, pause, crawl back, and repeat, the same way a long song title does. Leaving Now Playing and opening it again shows those lyrics again. The host patches main.lua before that file runs, so the page's get_track_lyrics call reaches that reader. The EQ shows the selected preset. Clicking that name opens the preset list, and choosing one closes the list. Moving a band copies that preset into the custom settings. The settings equalizer uses those same vertical bands. Settings sliders stay 240 pixels wide and dropdowns stay 360 pixels wide, instead of stretching across the SteamTunes tab. In the overlay, labels share one column and the dropdowns, sliders, and readouts fill the rest of the row. When the cover and the song text leave open space on both sides, the panels sit in that space, lined up with the cover. The lyric frame follows the cover. The EQ sliders use that same height, and the spectrum continues below them. The EQ sits 100 pixels left of the cover on a 1080p window and 200 pixels left on a smaller window. The meter sits the same distance to the right. On the 768 by 1366 screen that space is not there, so the panels sit under the cover and scale down. The spectrum is drawn on the page from one analyser tapped after the EQ, and it stops when Now Playing is not showing. Its bars use the appearance accent. The spectrum bars redraw on every frame. The loudness number is the average of that quarter-second, so it changes four times a second. A song is added to a playlist you created from the track row, and that playlist opens so the song can be removed. The playlist list opens as a menu on the button that was clicked. A library scan reads M3U, PLS, XSPF, WPL, ZPL, ASX, Winamp B4S, Media Player Classic, AIMP, and iTunes XML playlists. One iTunes library file can supply every playlist inside it. A playlist file is not changed. A playlist read from a file can be removed from the player after a confirmation. The file stays on disk, and a later scan does not put that playlist back. Songs in a playlist you created can be moved up and down or dragged to a new place, and Play follows that order. Moving a song keeps the list where it was. The library scan names each file in a folder, so it does not stop on the first folder. On every scan, a track saved without a size, an album, or an artist is read again. Set Cover saves the image the page read, and a later art load does not put the previous picture back. An album cover is read from that album's music file. The first tag read for a cover is 15 MB. A picture frame larger than that still counts as art, and the artwork pass reads the rest of that tag. A picture saved on an earlier pass is not reused, and a file inside an Apple Music library or DoomNet Art is not used as that cover. The page asks for the cover file already saved when the album title still has English letters or digits. A title with no English letters gets its own cover file, so Russian and Japanese albums by one artist do not share a picture. The Now Playing tab and its backdrop keep the full cover. A normal play fetches the original file by track id and does not copy it into `steamui`. The overlay player is already on the shortcut row. A player-only run on a live Steam client is still a manual check.
5. After that live check, remove the Millennium publish path from SteamTunes, then the paired rename of `millennium` to `host` and `window.Millennium` to `window.Host` in SteamTunes and DoomNet together.
6. `tools\DoomNetSetup.cs` already copies the payload and runs `--hook`. Do not design a second wizard.

### StreamTunes foundation

The listening-party output is a separate DoomNet-only branch in the patched `audio-engine.js`. The unchanged SteamTunes source and Millennium player keep the original dry/wet-to-`masterGain` path. DoomNet adds only null StreamTunes fields to the engine constructor. `doomnetAttachStream` lazily creates a dedicated limiter and `MediaStreamAudioDestinationNode`, connects the already-ducked dry and wet buses to it before `masterGain`, and returns that media stream. `doomnetDetachStream` disconnects only that branch and stops its track. `dispose` calls detach before closing the context. With no host session, there is no stream node and the default graph is unchanged. `resident.cpp` publishes this patched engine beside the patched UI and CSS, and store pages receive the same patched engine inline.

The listener path is separate too. `doomnetAttachRemoteStream` accepts the remote media stream and lazily creates its source, a listener-only gain, and a limiter. It connects that branch directly to the context destination because the host's ducking, width, EQ, and reverb are already in the stream. The existing volume slider also updates the listener-only gain. `doomnetDetachRemoteStream` disconnects those receive nodes. Attaching a host output drops a listener output and attaching a listener output drops a host output, so one client cannot play both roles.

The listening-party UI is also a DoomNet patch. It is absent under Millennium. On the SteamTunes tab the library row always shows Start Listening Party. A click without a relay address says one is needed and does not open a session. The Steam window receives `__doomnetPartyConfig` and `DoomNetStreamTunes` from the page that loaded the boot script. The object supplies `snapshot`, `subscribe`, `start`, `end`, `leave`, and `join`. The library subtab row puts Start Listening Party, or End Party plus Configure Party, immediately to the left of the artwork size control. That control stays at the right edge. The party button uses the accent color. A host's Now Playing tab stays the normal player. A listener in a session still sees Start Listening Party, and that button stays disabled until Leave Session. While that listener is in the session, the Now Playing tab is labeled Listening Party and its page is the party page. Leave Session drops the receive graph, resumes the local queue when one is loaded, and returns that tab to Now Playing. The Configure Party page shows `n/5`, an accent-colored copyable code, a host roster, and the listener's own ping. End Session is present but disabled unless the snapshot says this page is the active host. The native service is added after this guarded page contract; until then the object is absent and no unfinished controls are visible. Settings on the main tab host adds a Store tab after Troubleshooting. It lists SteamSkin from `window.__doomnetStore`, shows the installed version, and does not offer a second install. Install does not write `settings.json`. The tester catalog download is not part of this tab yet.

`services\streamtunes-relay` is signaling only. It stores live room membership, WebRTC descriptions, ICE candidates, listener ping, and listener bandwidth; it never receives audio. A cryptographically random code is four characters, a hyphen, and four more characters. A room is one host plus at most four listeners. Only peers already in the same room can relay a signal to each other. The host roster receives persona names and listener stats; listeners receive only the room count. A clean host end sends `host-ended`; loss of the host socket sends `host-disconnected`; a listener leaving updates the roster without ending the room. The WebSocket wrapper accepts text JSON of at most 32 KiB at `/streamtunes`, disables compression, and drops dead sockets on a 30 second heartbeat. TLS terminates in front of that process in production. The first live audio proof is this PC hosting and one remote listener. There is no second machine on the LAN, so that session uses the public `wss://` relay and TURN. The relay may receive a validated array of at most eight `stun:`, `stuns:`, `turn:`, or `turns:` entries through `STREAMTUNES_ICE_SERVERS_JSON`; it sends that configuration only when a peer creates or joins a room, and never writes credentials to its logs or health response. An explicit empty array is the direct-only test configuration. Production TURN credentials should be short-lived and rotated. On this PC the audio helper is `services\streamtunes-relay\turn-helper.js`. It listens on UDP 3478 and relay ports 50000 through 50031, tells peers this house's public address, and keeps its password in `%LOCALAPPDATA%\DoomNet\party-helper.json`. That password is not logged. The room process sends the helper when a party is created or joined. The router has to forward those UDP ports to this PC before a listener outside the house can use it.

`src\streamtunes_client.js` is embedded into the executable beside the boot script. It defines the tested `DoomNetStreamTunes.StreamTunesClient` factory but does not create a party by itself. The client queues signaling until its WebSocket is open, creates one `RTCPeerConnection` per listener, sends the one host media stream to each peer, attaches the listener's first remote audio stream through the receive graph callback, exchanges descriptions and ICE only through the room relay, and reports the listener's selected candidate-pair RTT and inbound audio rate once a second. A failed or disconnected peer gets about 15 seconds to return to `connected` before anything tears down. After that window, a host closes only that peer and leaves the room up for the remaining listeners; a listener ends with Connection Lost, sends `leave`, and closes its relay socket so host capacity recovers. `end`, `leave`, and other failures also close the relay socket after the matching room message. Its state and notices match the guarded UI contract. The guarded UI creates `window.__doomnetParty` from the embedded client only when `window.__doomnetPartyConfig` already has a relay URL and the logged-in Steam persona.

`config.json` has an optional `streamTunesRelay`. Empty disables the feature on a source tree. A tester install with an empty value uses `wss://leu-stadium-bosnia-logic.trycloudflare.com/streamtunes`, the secure tunnel to the relay on this PC at port 8787. That address works only while this PC and that tunnel are running. A saved address is left as written. A non-empty production value must start with `wss://`; only loopback `ws://127.0.0.1` and `ws://localhost` are accepted for tests. Optional `updates` is a bool that defaults to false. Production update checks run only when that flag is true and neither the running executable nor the data root sits under `D:\Documents\Repos\DoomNet-SteamPortal`, `D:\Documents\Repos\SteamTunes`, or `D:\Documents\Repos\SteamSkin`. The resident host checks at startup and about once an hour. When a new version is available, the SteamTunes page says so and says that it is downloading. When the download finishes, the page says to quit Steam and open it again. That check does not restart Steam by itself. The Store's Check for Updates button runs that same check, shows the same notice, and restarts Steam when a payload is replaced. An installed copy reads the player and skin under its data folder, which that check replaces from GitHub, and does not read the SteamTunes or SteamSkin repos. A library scan counts files whose tags were already read. The count used to stay at 0 while more local files were still being found, because it subtracted the growing remainder from the first total. Each song also lists its folder once for a cover, then reuses that result. A folder link, including a link back into the same library, is not followed, so those songs are not counted again. Global Control is added to the settings page once. The patched player page is written into Steam and is not written back over the player copy in the data folder, so a later launch does not add that switch again. When Steam exits, that copy leaves so the next Steam launch starts the program on disk. A source tree still prefers the repos and stays up across a Steam exit. A source tree, or `updates` set to false, logs that production updates are off and does not read a manifest. The manifest is `<root>\production-manifest.json` when the published URL cannot be read, otherwise `https://github.com/DoomyMcDoomface/DoomNet-SteamPortal/releases/download/production/production-manifest.json`. It has `doomnet`, `steamtunes`, and `steamskin`, each with `version`, an `https` `url`, and a 64 character `sha256`. The Check for Updates button reads that production release, not a local folder. Each piece's version is the first 12 characters of its zip hash, so a new file is a new version without a hardcoded number. Each URL is a stored zip: `doomnet.zip` contains `DoomNet.exe` and `version.dll`, `steamtunes.zip` contains `plugin.json` and `backend` without `backend\data`, and `steamskin.zip` contains the skin. A piece is downloaded only when its installed version is non-empty and different. A skipped SteamSkin is not pulled in. The download is https only, the hash is checked before any replace, and a mismatch deletes the download and leaves the installed files alone. A verified zip replaces that piece immediately. `DoomNet.exe` in the data folder is replaced even while this copy is running and loads the next time Steam starts. `version.dll` beside `steam.exe` is replaced only when Steam is not running. `player\backend\data` and every `settings.json` are not replaced. A copy that fails because a file is in use stays in `<root>\updates` and is applied the next time DoomNet starts, before the player loads. A push of `main` or `production` runs `tools\Publish-Remote.ps1` before the remote is updated. That script builds this tree, writes `dist\DoomNet.zip` for a new tester, writes the three updater zips and the manifest, and uploads them onto the `production` release, replacing the previous files. The new-tester file is named `DoomNet-setup.zip` on that release, because `doomnet.zip` is the smaller update. If that upload fails, the push stops. `tools\Publish-Production.ps1` is the upload step. The GitHub workflow cannot see the private player and skin repos, so it does not upload a zip. Installed app versions live in `<root>\versions.json` with string keys `doomnet`, `steamtunes`, and `steamskin`. The Settings Store lists SteamSkin from `window.__doomnetStore`. The Steam window that shows the player gets that same object, because that window is not the page that loaded the boot script. SteamSkin is the only Store install. When it is already installed, Install stays on the row and is disabled. Install calls `doomnet_install_app`, which queues a copy of `payload\skin` or `D:\Documents\Repos\SteamSkin` into `<root>\skin` on the resident loop, sets `skin` true, records the skin version, and does not change `settings.json`. After that copy the resident asks that Steam folder's `steam.exe` to quit, waits until `steam.exe` and `steamwebhelper.exe` from that folder are gone, and starts `steam.exe` in a visible window. `launch_hidden` still refuses `steam.exe`. `DoomNet.exe --install` is the setup command and also does not start Steam. When this program is `DoomNet.exe`, that command copies it to `<root>\DoomNet.exe` and copies `version.dll` from beside the program to `<steam>\version.dll` only when `steam.exe` is there, Steam is fully quit, and Millennium's bootstrap is not already beside it. Steam imports `version.dll` as it starts, before `steamwebhelper.exe`. `wsock32.dll` is delay-loaded, so a hook only in that file misses the helper and the skin and player never appear. A DoomNet `version.dll` is recognized by the `DoomNetWsock32` marker and is not treated as Millennium. An older DoomNet `wsock32.dll` beside that Steam is removed. `DoomNetSetup.exe` requests administrator so that copy into `Program Files` can succeed. The result sentence is also written to `<root>\install-result.txt`. Opening Steam from its normal shortcut then starts `<root>\DoomNet.exe --attach`. A failed install exits with code 1. DoomNet reads `config\loginusers.vdf` under the discovered Steam folder and selects the `PersonaName` whose block has `AutoLogin` or `MostRecent` set to `1`; when exactly one persona exists it is the fallback. `AccountName` is never placed in the page. Only a valid relay plus an unambiguous persona adds `window.__doomnetPartyConfig` to the page preamble. The guarded UI then creates the embedded client, supplies the host stream on Start, routes a received stream into the listener graph, and detaches both branches on end, leave, disconnect, or error.

## What the products do not share

There is no DoomNet HTTP API, no shared library, and no plugin bus. The player already speaks `Millennium.callServerMethod` and `millennium.call_frontend_method`. The skin already speaks `skin.json` `Patches`. DoomNet is the host that those two existing contracts run on. A player release does not require a DoomNet rebuild unless one of those contracts changes. A skin release does not require a player release.

The player repo is not edited while DoomNet is being built. Phasing Millennium out of this repo happens after DoomNet has launched Steam and playback has been confirmed for player-only, skin-only, and both. Until that proof, this tree stays a Millennium plugin so the comparison install on this PC keeps working. Do not delete publish scripts, rewrite `require("millennium")`, or rename `Millennium.callServerMethod` as part of building the host.

What that later cleanup removes, first, is the shipping path to Millennium's organization: [scripts/open-plugin-database-pr.sh](scripts/open-plugin-database-pr.sh), [.github/workflows/publish-production.yml](.github/workflows/publish-production.yml), [scripts/should-ship.sh](scripts/should-ship.sh), [scripts/deploy.ps1](scripts/deploy.ps1), [scripts/prepare-millennium-frontend.js](scripts/prepare-millennium-frontend.js), the `.millennium` packager stub, and the README and CHANGELOG wording that tell people to install through the Plugin Database. The canceled pull request stays canceled. Comments that only mention Millennium can go in the same pass. None of those run during playback.

The call shape stays for that first cleanup. `require("millennium")`, `millennium.steam_path`, `millennium.ready`, `millennium.add_browser_js`, `millennium.call_frontend_method`, and `window.Millennium.callServerMethod` in both [backend/assets/frontend/steam-music-player.js](backend/assets/frontend/steam-music-player.js) and [backend/assets/frontend/audio-engine.js](backend/assets/frontend/audio-engine.js) are how the page and Lua talk. Deleting them with no replacement stops the player. DoomNet is implementing those names so the files on disk can stay as they are while the host is proven.

Once that proof is in and the comparison install is no longer needed, the names can change without a loss of function, as one paired release. The player and DoomNet switch together. The player requires a neutral module, `host`, not `millennium` and not `doomnet`. The page looks up `window.Host.callServerMethod` with the same positional arguments. DoomNet's shim registers `host` and installs `window.Host`. Method names on the player stay `SteamMusicPlayer.*`. User-visible strings that mention a Millennium crash dialog change in that same release. After it, toggling Millennium back on will not run this player. That is the point of waiting until DoomNet is the host you trust. Do not land the rename while the proof is still open, and do not land it only in this repo.

DoomNet stages the two payloads independently. A user can install either one, or both. DoomNet injects only the chosen piece. It does not install Millennium and it does not talk to Millennium.

On this PC, Millennium stays installed so the current skin and player can be compared with DoomNet. Testing uses a quiet off switch. That switch is not an uninstall. The Plugin Database pull request is already canceled. Do not reopen it.

Opening Steam looks the same as opening Steam. DoomNet stays in the background. No command prompt, PowerShell window, or other console appears before the client.

## What exists today

Steam's client UI and the Shift+Tab overlay are separate Chromium pages. Copying files into `steamui` does not run them. Steam does serve that folder at `https://steamloopback.host/...`, which is why the player can load its JS from there once something has inserted a script tag.

Millennium currently does three jobs:

- Starts [backend/main.lua](backend/main.lua) inside its Lua runtime when Steam loads the plugin. `on_load` copies assets, registers injection, then calls `millennium.ready()` within about 10 seconds.
- `millennium.add_browser_js` inserts the bootstrap into webkit pages. The shell and the overlay are not covered by that call. Those windows get the player because the Steam-Skin theme's `music-player-inject.js` is matched by window name in `skin.json` and that file appends `bootstrap.js`.
- `window.Millennium.callServerMethod(pluginName, methodName, argsArray)` calls a global Lua function. `millennium.call_frontend_method("SteamMusicPlayer.<method>", { arg })` runs that method on the page.

The visible client window is often an `about:blank` popup opened by another Steam context. [backend/assets/frontend/steam-music-player.js](backend/assets/frontend/steam-music-player.js) borrows `window.Millennium` from `opener`, `parent`, or `top` when the current page has no bridge. DoomNet must still install the bridge on the page that owns it, or every IPC call fails and the panel stays empty.

[backend/assets/frontend/bootstrap.js](backend/assets/frontend/bootstrap.js) is the only script that should be inserted for the player. It refuses to run inside an iframe (`window !== window.top`) and returns immediately if `window.__steamMusicPlayerBootstrap` is already set. Store and Community wait for `requestIdleCallback` (or 800 ms) after load. Every other window loads on `DOMContentLoaded`. Scripts are appended in series, because a failed script element does not reload if `src` is changed. Element ids are `smp-plugin-css`, `smp-plugin-mix-image`, `smp-plugin-engine`, `smp-plugin-ui`. The load order, each with `?v=` plus `ASSET_REV`, from `https://steamloopback.host/steam-music-player/` and then `/steam-music-player/`:

- `steam-music-player.css`
- `mix-image.js`
- `audio-engine.js`
- `steam-music-player.js`

`ASSET_FILES` in [backend/main.lua](backend/main.lua) copies bootstrap, the css, `audio-engine.js`, and `steam-music-player.js`. It does not copy `mix-image.js`. The missing file 404s, the element is removed, and the chain still continues, so the player comes up without the mix image. DoomNet's copy includes `mix-image.js`. It does not copy `mix-image-test.js`.

`ASSET_REV` in the SteamTunes source is `20261027-version-1.15.0`. CEF caches these URLs. DoomNet does not edit that source file. The player copies those 1.15.0 files into steamui on startup. DoomNet stops that copy with a return inside `if true then`, because Lua rejects a return that is followed by another statement and then the whole player fails to start. The music folder list and the library index stay on disk; the page only looks empty while the player is not running. After that, DoomNet writes its own page back: version 1.16.0, the scan, and the listening-party button. The bootstrap cache name is `20261112-version-1.16.0-scan-playlists`, so Steam loads that page instead of a cached page. Adding a folder scans only that folder and skips songs already in the library. Rescan Library still reads the whole library. Scan For New Tracks skips songs already saved and also imports playlist files that are not in the player yet. Regenerate Artwork does not clear the cache and does not scan the library again. It reads the picture stored inside songs already in the library: an MP3 contributes only its ID3 tag, and the rest of the song is not read. A listener who is not in a party presses Join Party, types the host code, and presses Join Party again. Start Listening Party still makes this PC the host. While a party is open, Artist, Album, Genre, Playlists, Now Playing, and search show those pages, and Configure Party opens the party page again. The party keeps running. A roster update does not pull the library back to the party page. A failed or empty scan batch starts the scan again from the saved library instead of treating it as finished. One unreadable file is skipped. The batch size stays 25 and the pause stays 300 ms, because larger batches crashed the player on a library of about 40,000 files. Album Add to playlist is inserted once. The search line stays in the page, so a later start must not insert it again, and extra copies already in the page are folded back to one. A repeated Global Control block is removed as that one function only. The cleanup must not delete the Store's Check for Updates button that sits between two copies.

Role detection uses `document.title`: `Steam` and `SteamBrowser_Find` are the client; `OverlayTab<N>_Find` and `SP Overlay:` are the in-game overlay. The client owns `AudioContext`. Overlay pages are remotes. They share state through `set_ui_pointer` / `get_player_state` and `transport_command`.

The normal audio path is `stage_track_for_playback`, then `fetch` of the returned URL, then `decodeAudioData`. `get_track_audio_chunk` is the fallback when that fetch fails. `utils.base64_encode` is still required for the fallback.

Hidden work is already designed in [backend/procexec.lua](backend/procexec.lua). Millennium's `utils.exec` is `_popen`, which always starts `cmd.exe /c` and flashes a console even when PowerShell is asked to hide. procexec works around that with one `wscript.exe` supervisor and `WScript.Shell.Run(..., 0)`. DoomNet's `utils.exec` must not use `_popen` or `cmd.exe`. It starts `wscript.exe` with `CREATE_NO_WINDOW` so the first hop never flashes either.

## Process model

One program, `DoomNet.exe`, Windows subsystem (`wWinMain`), no console. Single instance mutex `Local\DoomNet`.

Layout under `%LOCALAPPDATA%\DoomNet\`:

- `DoomNet.exe`
- `player\` — copy of this repo's `backend\` plus `plugin.json`
- `skin\` — copy of Steam-Skin, present only if the skin was installed
- `config.json` — `{ "steamPath", "player": true/false, "skin": true/false, "playerColor": "steam"|"red" }`
- `logs\doomnet.log`
- Player data stays at `player\backend\data\` because [backend/main.lua](backend/main.lua) sets `dataDir` to `fs.join(backendDir, "data")`. Do not point that at the Millennium plugin folder. The two installs must not share one `settings.json` while both exist for comparison.

If the user wants the same library for a test, the installer may copy `millennium\plugins\SteamMusicPlayer\backend\data` into DoomNet's data folder once, only when DoomNet's data folder is empty. It never deletes the Millennium copy.

Startup sequence, all with no window:

1. If Steam is not running, `CreateProcess` on `steam.exe` with the inherited environment and no extra console. Do not pass a debug-port flag that opens a console.
2. If Steam is already running, attach. Do not launch a second Steam.
3. If `config.json` has `player`, start the Lua host and copy player assets into `<steam>\steamui\steam-music-player\`.
4. When a Steam browser is created or found, insert only the enabled payloads.
5. If DoomNet is already running and the shortcut is clicked again, the second process signals the first and exits. It does not open a window and it does not start a second Steam.

The desktop and Start menu Steam shortcuts are retargeted to `DoomNet.exe`. The icon stays Steam's icon. `steam://` launches that would skip DoomNet are covered by a windowless watcher already running from the shortcut install (same exe, `DoomNet.exe --watch`), so a launch from a game or a protocol still gets the inject. The watcher is the same mutex owner, not a second visible process.

Children (`powershell.exe`, `wscript.exe`, the Discord bridge, art workers, the scan script) use `CREATE_NO_WINDOW` (`0x08000000`) and `STARTF_USESHOWWINDOW` with `SW_HIDE`. Do not call `cmd.exe`. Do not use `powershell -NoExit`. Logs append to `logs\doomnet.log`.

## Threads

Millennium's plugin occupies one Lua state until the call returns. A coroutine yield exists in their host, and their C++ process has a pool, but a plugin call does not share that state with the next call. [backend/main.lua](backend/main.lua) says a large audio read "starved the only backend thread." [backend/library.lua](backend/library.lua) sleeps on that thread: 100 ms inside `wait_for_scan_out` (up to 5 s, from `start_list_job`), 250 ms inside `library.rescan`, and 50 ms inside `wait_ms` (up to 4 s while a cover job finishes). [backend/procexec.lua](backend/procexec.lua) sleeps the same way, and if `utils.sleep` throws it busy-spins on `os.clock()`. The frontend splits scans into batches of 25 with a 300 ms gap because Millennium's Lua process crashed under faster RPC, and because one scan used to exceed an RPC timeout of about 30 s. Those limits are Millennium's. DoomNet does not copy them.

### Two operating-system threads, plus coroutines

`DoomNet.exe` has two threads. Lua coroutines are not a third thread. They are how the state thread puts one call down and runs another without a second `lua_State`.

- The state thread is the only thread that calls into Lua. It owns the library index, the queue, settings, and playlists, because those tables live in that state. Lua is not thread-safe. A lock around `lua_State` that the browser thread also takes is the wrong design. The browser thread never calls `lua_resume`, `lua_pcall`, or `lua_getglobal`.
- The browser thread owns the page-list socket, timers, and byte reads that a coroutine explicitly handed off. It never touches a Lua string except to copy bytes into a message the state thread will turn into a Lua value.

The hook inside `steam.exe` is neither thread. It records the port and returns. The process main thread pumps a low-level keyboard hook while Global Control is on, including the `--attach` process Steam starts. A plain sleep on that thread drops the key as soon as Steam is not focused. Play, pause, stop, next, and previous count once per press. Further signals for that key inside half a second are the same press. Every command the hook posts carries a unique `id`, because the player applies a command with an id once and the Steam window received each id-less command twice. Volume notches are sent from the hook itself and folded into one waiting bump, so a wheel does not queue a round trip per notch. Reversing the wheel drops that unapplied bump, so rolling down after the top does not cancel steps that never played. The slider and the gain catch the latest wheel position on each screen frame, so a fast roll moves farther in that frame instead of jumping and then lagging behind. That hook posts `transport_command` and never calls Lua. It is not another operating-system thread. Playback is neither thread. The Web Audio graph stays in `steamwebhelper`, in [backend/assets/frontend/audio-engine.js](backend/assets/frontend/audio-engine.js). Do not add an audio thread in DoomNet.

Do not start a pool, a thread per page, or a thread per `callServerMethod`. Two threads, one queue in each direction.

### What a second thread does not intercept

`scan_library_batch` reads tags with `io.open` inside `read_tags_safe` / `native_open`. That is ordinary Lua file I/O on whichever thread is running the coroutine. The browser thread does not see it. A batch of 25 files stays one uninterrupted stretch on the state thread, on purpose: `scan_batch` writes `state.tracksById` as it goes, and another call must not observe a half-built track. Do not replace `io.open` globally. Do not yield in the middle of `read_tags_safe`.

The waits that freeze transport today are not those tag reads. They are `utils.sleep`, `millennium.call_frontend_method` if it blocks on the page, and the 512 KiB read plus base64 inside `get_track_audio_chunk`. Those are the three yield points.

### Queues

One mutex and one condition variable protect both queues. The state thread waits on the condition variable only when no coroutine is runnable. It never waits on the browser thread by joining it.

Messages to the state thread:

- `Call`. A page invoked `callServerMethod(plugin, method, args)`. Fields: call id, method name, positional args already decoded (string, number, or boolean).
- `Resume`. A parked coroutine can continue. Fields: coroutine id, and the value to return into Lua (for a sleep, nothing; for a frontend call, the typed result; for a read, the bytes or an error).
- `Stop`.

Messages to the browser thread:

- `Resolve`. A coroutine returned. Fields: call id, the Lua return string, or an error. The browser thread settles the page's Promise. It does not enter Lua to format that string. The state thread already encoded it.
- `Sleep`. Parked coroutine id and milliseconds.
- `Frontend`. Parked coroutine id, method name, and the one JSON string argument.
- `ReadRange`. Parked coroutine id, path, offset, length. Only `get_track_audio_chunk` sends this.
- `Stop`.

### State-thread loop

Create one `lua_State` on this thread before any call. Load `backend/main.lua` here. `on_load` is the first coroutine.

Each `Call` gets its own coroutine (`lua_newthread` on that state). Coroutines share globals. Only one is resumed at a time.

```
while running:
  if a Call or Resume is queued:
    lua_resume that coroutine
    if it returned:
      enqueue Resolve
    if it yielded:
      leave it parked under the yield reason
      enqueue Sleep, Frontend, or ReadRange
  else:
    wait until the browser thread enqueues something
```

`lua_resume` returning a yield is not an error. A Lua error is a failed `Resolve` with the error string, then that coroutine is discarded. Do not close the `lua_State` because one call failed. Do not restart the VM because the page gave up. Millennium's process died under a burst of calls. DoomNet queues the burst and runs it one coroutine at a time.

From Lua's point of view a yielded call still looks synchronous. `utils.sleep(100)` returns after 100 ms inside that coroutine. Other coroutines may have run in between. Code may not assume it is the only Lua alive across a sleep or a `call_frontend_method`. It may assume that from one Lua line to the next, with no yield in between, nothing else mutates the tables. `scan_batch` has no yield between files, so a batch is still atomic.

### Browser-thread loop

Read the page socket and the timer list. Never block this thread on Lua.

- An inbound `callServerMethod` becomes a `Call`. If that call's Promise was already rejected by the page, still let the coroutine finish, then drop the late `Resolve`. [backend/assets/frontend/audio-engine.js](backend/assets/frontend/audio-engine.js) rejects `get_track_audio_chunk` after 8 s (`AUDIO_CHUNK_TIMEOUT_MS`). That timer is the page giving up. It is not a signal to cancel Lua. Cancelling mid-call is how a library index gets half-written.
- A `Sleep` becomes a timer. On fire, enqueue `Resume`.
- A `Frontend` runs the JS method on the bridge window through the binding, then enqueues `Resume` with the typed result. If no page has the method yet, resume with `nil` immediately. Do not wait. `on_load` calls `broadcast_state` before any window exists.
- A `ReadRange` reads the file and enqueues `Resume` with the bytes. On failure, resume with `nil` plus an error the state thread turns into the existing JSON `{ ok = false, error = ... }`. Do not base64 on this thread. The state thread calls `utils.base64_encode` after resume, then `json.encode`. Leave `CHUNK_SIZE` at 512 KiB. A larger chunk is what made every track fail: the base64 string never arrived.

### The three yield points, in code

`utils.sleep(ms)` yields `Sleep` and resumes with success. `pcall(utils.sleep, ms)` in `pause_ms`, `wait_ms`, and `wait_for_scan_out` must succeed. If it fails, those functions fall through to `while os.clock() < untilClock`, which burns the state thread and brings the stall back. Do not implement sleep as "block this thread for ms." That also prevents other coroutines from running, and it fails the deadlock test below.

`millennium.call_frontend_method` yields `Frontend`. The Lua caller still gets the typed return when the page returns. While it is parked, a `callServerMethod` from that page runs as another coroutine. This is the deadlock this design exists to avoid: `transport_command` calls `onRemoteCommand`, the page calls back into Lua before returning, and a single blocked thread never answers. Implement the yield. A condvar wait on the state thread is not a yield.

`get_track_audio_chunk` in [backend/main.lua](backend/main.lua) `io.open`s and `file:read`s 512 KiB on the Lua thread, then base64-encodes. DoomNet's `io` library, not a new Lua call, yields `ReadRange` from that read and returns the bytes. Leave [backend/main.lua](backend/main.lua) as it is so the Millennium install keeps using its own `io`. Keep `library.ensure_playable` where it is. It already returns `{ staging = true }` instead of waiting on the copy. `mark_play_hot` stays around the call. Tag reads in `read_tags_safe` stay on the state thread and do not yield. They can pull up to 8 MiB of an ID3 header and must finish before the track row is published. They do go through the same wide `io.open`.

`utils.exec` stays `CREATE_NO_WINDOW`. Waiting for that process to exit is a park on the browser thread, same shape as `Sleep`, not a blocked state thread. Art, loudness, and Discord stay child processes. The browser thread waits for their done-files only when Lua is inside a loop that calls `utils.sleep`. Do not bring those workers in-process.

### What stays one-at-a-time

These run to completion once resumed, with no yield in the middle, so they cannot race each other:

- `set_setting` and the settings broadcast that follows it, except the broadcast itself yields at `call_frontend_method`. The file write before that call does not.
- One `scan_library_batch`. The frontend keeps `SCAN_BATCH_SIZE` at 25 and `SCAN_BATCH_DELAY_MS` at 300. Do not raise them in this work. The host must survive a faster burst anyway, but the page pacing stays until a large library is scanned on DoomNet without a restart.
- `transport_command`'s update of `pendingTransport` and `player_state`. The frontend call after that update yields. Another call may run before `onRemoteCommand` returns. That is safe: the pending command is already stored.

There is no 30 s RPC abort. A slow `scan_library_batch` returns when the 25 files are done, even if that is longer than Millennium allowed. The page's own timeout still applies to audio chunks only.

### Proof before calling the host done

1. Start a scan that hits `wait_for_scan_out` (up to 5 s of 100 ms sleeps). During those sleeps, `get_settings` from the page returns. It must not sit until the 5 s ends.
2. From a test page, `call_frontend_method` a function that immediately `callServerMethod`s `get_settings`, then returns `true`. Both finish. A state thread blocked on the page hangs here.
3. Fire several `callServerMethod`s while one `scan_library_batch` is inside `read_tags_safe`. They wait, then run in order, and `tracksById` has no torn row. They must not run on a second `lua_State`.
4. Let `get_track_audio_chunk` exceed 8 s on a stalled disk. The page rejects. The coroutine still finishes, the VM is still up, and the next `transport_command` works.
5. `pcall(utils.sleep, 50)` is true, and a CPU sample during `library.rescan`'s 250 ms pauses shows the state thread idle, not spinning in `os.clock()`.

Both threads append to `logs\doomnet.log` under the same mutex. The Lua `logger` module runs only on the state thread. The browser thread writes its own lines without calling Lua.

## Contracts to match

Read these before changing a call shape. The page and the Lua files already agree on them. DoomNet implements the host side.

Public Millennium sources, current `main`, not the 2025 tree:

- [bootstrap README](https://github.com/SteamClientHomebrew/Millennium/blob/main/src/bootstrap/README.md) names the Windows shim `wsock32.dll`.
- [src/bootstrap/win32/main.c](https://github.com/SteamClientHomebrew/Millennium/blob/main/src/bootstrap/win32/main.c) loads `millennium\lib\millennium.dll` only when the process image is `steam.exe`.
- [src/include/millennium/steam_hooks.h](https://github.com/SteamClientHomebrew/Millennium/blob/main/src/include/millennium/steam_hooks.h) defines `DEFAULT_DEVTOOLS_PORT` as `8080` and the DLL-notification types.
- [src/lua_host/api/millennium.cc](https://github.com/SteamClientHomebrew/Millennium/blob/main/src/lua_host/api/millennium.cc) is the Lua API this plugin calls.
- Commit `143e26d` (2025-12-30) retires `version.dll`.

Page identity. [plugin.json](plugin.json): `name` is `SteamMusicPlayer`, `common_name` is `Doomy's SteamTunes`, `version` is `1.15.0`, `useBackend` true, `backendType` `lua`. The JS constant `PLUGIN_NAME` is the same string. `callServerMethod` is `(pluginName, methodName, argsArray)`. `argsArray` is a positional array. An object fails Millennium's bridge before Lua runs. The JS `callServer` in [backend/assets/frontend/steam-music-player.js](backend/assets/frontend/steam-music-player.js) JSON-parses a string return and leaves anything else as-is. Booleans from JS may arrive in Lua as the strings `"true"` and `"false"`.

Bridge lookup. `bridgeWindow()` tries `window`, then `opener`, `parent`, and `top`, and keeps the first whose `Millennium.callServerMethod` is a function. Cross-origin access throws and is skipped. The visible client is often `about:blank` and borrows the opener. `publishReceiverOn` writes `SteamMusicPlayer` and `__SMP_RECEIVERS__` onto that host. `call_frontend_method("SteamMusicPlayer.onRemoteCommand", ...)` must run in the window that holds `__SMP_RECEIVERS__`, which then walks the list. `Receiver.onRemoteCommand` returns false when `App.engine` is missing and true after `Engine.applyCommand`. `unlockAudio` returns true only when that page has an engine.

Library files, under `player\backend\data\` once DoomNet owns the data directory:

- `library_index.json` is a JSON array of track objects. Playback fields are `id`, `path`, `size`, `extension`. This is the file the stream hook reads. `persist_index` rewrites the whole array.
- `library_snapshot.json` is the UI payload. `v` is `2` (`SNAPSHOT_VERSION`). `t` is an array of display rows: id, title, then integer indexes into `artists`, `albums`, `genres`, `albumArtists`, then track, disc, year, `hasArt` as 0 or 1, then an index into `exts`. There is no filesystem path in `t`. A hook that opens music from this file will not find one.
- `media_token` is the hex token `stage_url_base` embeds. `media_token()` reads `data\media_token`, strips non-hex, and accepts it only when at least 16 hex characters remain. Otherwise it writes 8 times `%04x`. If that fails it disables direct playback rather than using a guessable name.
- `library.get_track` looks up `tracksById[id]`, then `tracksById[tostring(id)]`, then scans keys with `tostring`. A hash that JSON turned into scientific notation still has to resolve. The URL segment uses `tostring(track.id)` so it matches that lookup.

`stage_track_for_playback` returns one JSON string. The page treats `url` as opaque. It does not parse the filename.

- Ready: `{ "ok": true, "url": "<stage_url_base><encoded name>", "size": <bytes> }`. `stage_url_base` is `https://steamloopback.host/steam-music-player/stream/<token>/`. Today's copied-file name comes from `staged_name_for`: sanitized id, underscore, path length, optional extension. Sanitizing can collide, which is why the path length is mixed in. The hook URL skips that and uses `url_encode_segment(tostring(track.id))` as the last segment. `size` is the real file size. The page rejects the buffer when `byteLength !== size`.
- Still copying: `{ "ok": false, "staging": true, "retryAfterMs": 400 }`. `STAGE_POLL_INTERVAL_MS` is 400. The page polls until `STAGE_POLL_MAX_MS` (12000) and then uses chunks.
- Failed: `{ "ok": false, "error": "<text>" }`.

`fetchTrackViaHttp` calls `fetch(info.url, { credentials: "omit", cache: "no-store" })` and aborts at `AUDIO_CHUNK_TIMEOUT_MS` (8000). It reads `response.arrayBuffer()`. A body shorter than `info.size` is treated as a broken fetch and the chunk path runs. A 206 that contains only a range fails that check. When the request has no `Range` header, respond 200 with the entire file and a `Content-Length` equal to `size`. Answer `Range` with 206 only when the request sent `Range`. The current page does not send it. `release_staged_track` runs 150 ms after the bytes are in memory, and once more 400 ms later if `ok` is false. On the hook path there is no file to delete. Return `{ "ok": true }` so that retry does not run.

`get_host_escape_probe` returns `probeUrl` set to `stage_url_base` plus `%2e%2e/%2e%2e/%2e%2e/%2e%2e/steamui/steam-music-player/steam-music-player.css`. The page fetches it once and only logs the result. The hook answers 404. That probe must not open the stylesheet by walking `..`.

`get_track_audio_chunk` returns `{ ok, base64, isLast, totalSize, nextOffset }` or `{ ok: false, staging: true, retryAfterMs: 400 }` or `{ ok: false, error }`. `CHUNK_SIZE` is `512 * 1024`. `get_track_art` and `get_first_track_art` return `{ ok: false }` on purpose.

Frontend methods invoked as `SteamMusicPlayer.<name>`: `onStateUpdate`, `onSettingsUpdate`, `applyLiveMix`, `getMixProbe`, `unlockAudio`, `onRemoteCommand`. Arguments are one JSON string, except where the existing call already passes a single encoded value.

## Playback path

The slow part of starting a track is not the audio graph. [backend/assets/frontend/audio-engine.js](backend/assets/frontend/audio-engine.js) already decodes into an `AudioBuffer` and runs gapless and crossfade from memory. What it waits on first is `stage_track_for_playback` in [backend/main.lua](backend/main.lua): a full copy of the file into `<steam>\steamui\steam-music-player\stream\<token>\`, then a `fetch` of that copy, then `release`. A 40 MB FLAC is read twice and written once before the first sample. Unicode names often miss Lua's `io.open` and pay an extra PowerShell copy before that. The page polls every 400 ms until the copy exists, and gives up at 12 s, then falls back to 512 KiB base64 chunks.

That copy exists because `https://steamloopback.host/` only serves `steamui`, and a directory link from there was measured to resolve `..` against the real disk. DoomNet does not add those links back. It also does not put a filesystem path in a URL.

DoomNet answers that same prefix on the browser thread, for the main client window. The last segment is `tostring(track.id)`, not `staged_name_for`. The handler reads `library_index.json` (or an id-to-path map the state thread wrote from it), opens `path` for that id, and writes the body. It compares ids with `tostring`, the same way `library.get_track` does. A missing id, `..`, a drive letter, or a path segment is 404. The handler never joins the URL onto a directory. The token in the URL must equal the hex in `data\media_token`.

The main client is the page same-origin with `steamloopback.host`, and it is the page that owns `AudioContext`. Store, Community, and the overlay stay on the small JSON calls they already use.

The page keeps requesting `steamloopback.host`, so there is no fetch to `127.0.0.1` and no new certificate. Steam's static server 404s this URL until a copy exists under `steamui`. The hook runs first. Two tracks may be fetched at once (the current one and a preload). Both are full-file 200 responses. There is no copy worker in front of them.

`stage_track_for_playback` returns `{ ok: true, url, size }` immediately when the hook is up and the id resolves. `size` is the source file's size, because the page compares it to `arrayBuffer.byteLength`. Leave the other JSON shapes in place so a hook that did not attach still returns `staging: true` and the copy fallback. That fallback is not the path a normal launch takes.

While the hook is up, skip `ensure_playable`'s copy and `start_async_copy` on the play path. A file that is still an online-only placeholder downloads as part of the stream, not into `steamui` and then again into the page.

### Wide paths

Lua `io.open` on Windows is the ANSI code page. Japanese, Cyrillic, and many accented names fail it, and [backend/library.lua](backend/library.lua) then spawns PowerShell to copy the whole file before tags or playback can see it. DoomNet's `io.open`, `fs.exists`, `fs.file_size`, and `utils.read_range` use wide Win32 paths (`CreateFileW` and friends). After that, `native_readable` succeeds for those files and the play path does not start a copy script. Keep the PowerShell fallback in the Lua file for a host that is not DoomNet. Under DoomNet it should not run on a file the wide open can read.

Tag reads stay on the state thread inside `scan_library_batch`, but they go through the same wide open. A scan of a non-ASCII library stops spawning one process per file.

### What stays a file the page fetches

These already avoid the binding, and the hook must not pull them back onto `callServerMethod`:

- The library list is `library_snapshot.json` under `steamui\steam-music-player\`, fetched by the page with a `rev` query. The state thread encodes it. The browser thread writes the bytes. `get_library` stays the fallback when that file is missing.
- Cover images are JPEGs under `steamui\steam-music-player\art\`. `get_track_art` stays `{ ok = false }`. One art JSON document over the binding was enough to wedge Play.
- `bootstrap.js`, the stylesheet, and the two player scripts stay on `steamloopback.host` with `ASSET_REV`. Insert each once, in the main world, and skip iframes. A store iframe that loads the player is what made that tab slow.

`call_frontend_method` runs on the bridge window only. Evaluating a full state JSON in every overlay and store target multiplies a push by the number of open pages and does not move audio.

The page still decodes the whole track into an `AudioBuffer` before it schedules sound. The injector's job is to make that fetch a single read of the original file. Changing the decoder to stream from a `Range` body is later work in `audio-engine.js`, after this path is the one a normal play takes.

## Installer

A windowed setup program, shown once. Later Steam launches do not show it. DoomNet itself stays windowless. This setup window is the only window a distributed install opens, and it opens with no PowerShell window, Windows Terminal, or other console.

The look is already accepted. Do not design another wizard, and do not use the off-screen test dialog as the window people see. The files to keep are:

- Layout: `D:\Documents\Repos\DoomNet-SteamPortal\tools\wizard.xaml`
- Window behavior: `D:\Documents\Repos\DoomNet-SteamPortal\tools\DoomNetSetup.cs`
- Preview build: `build\Release\DoomNetSetup.exe`, a Windows-subsystem program. `src\wizard_mock.cpp` only starts that program.

What that window already does, and what distribution must keep:

- Borderless window on the player red theme: background `#0e0e0f`, elevated `#17181a`, panel `#121214`, border `#2a2b2e`, accent `#b33232`, text `#d9dadd`, dim `#8f98a0`. Title "DoomNet" is centered. The title bar, sidebar, fields, and buttons are the slim sizes already in `wizard.xaml`.
- Sidebar steps, in order: Welcome, Choose, Color, Finish.
- Welcome: an editable Steam folder field plus Browse, which opens a folder picker. Prefill from the discovery order below, then show the path with real on-disk casing. Do not show the lowercase registry string.
- Choose: tick bubbles, not cards. A red bubble with a white tick is selected. A white bubble is not. The choices are Doomy's SteamTunes and Steam-Skin. Next stays off until at least one is selected. The skin does not force the player on.
- Color: shown only when both are selected. Otherwise skip it and draw the Color step as skipped. Steam Blue starts selected. Match Steam-Skin is the other bubble. Same tick bubbles as Choose.
- Finish: a summary of the player, the skin, and the color. The button is Install. It starts `DoomNet.exe --install` and does not start Steam. The Steam folder must contain `steam.exe`.

The setup window runs as the user who opened it. It copies `payload\player` and `payload\skin` into that user's `%LOCALAPPDATA%\DoomNet`, copies `DoomNet.exe` there, and writes `config.json` before it asks Windows for permission. It does not say the copy succeeded unless `player\backend\main.lua` or `skin\skin.json` is on disk for each selected piece. It then runs `DoomNet.exe --hook "<steam folder>"` elevated, which copies `version.dll` beside `steam.exe`, and it checks that the copied file is the same size as the one in the setup folder. Closing the window without Install copies nothing. `--hook` does not take `Local\DoomNet`, so a running host does not block that copy. A second `DoomNet.exe --install` still does not install while `Local\DoomNet` is already held: it exits 1 and writes the reason to `<root>\install-result.txt`. A normal second launch, with no `--install`, still exits 0.

1. Find Steam: `HKCU\Software\Valve\Steam\SteamPath`, then `HKLM\SOFTWARE\WOW6432Node\Valve\Steam\InstallPath`, then `C:\Program Files (x86)\Steam` if `steam.exe` is there. The Welcome field is that folder, and it must contain `steam.exe` before install proceeds.
2. Ask which pieces to install: player, skin, or both. DoomNet itself is always installed.
3. If the player and the skin are both selected, ask the starting player color:
   - Steam Blue, labeled as the default. Writes `uiColorTheme` = `steam` (`#66c0f4` accent, grey-blue chrome `#171a21` / `#1b2838`).
   - Match Steam-Skin. Writes `uiColorTheme` = `red` (`#b33232` accent, near-black chrome). That is the red-and-black player palette, not a CSS file from the skin.
4. Player-only does not ask. It writes `steam`. Skin-only does not ask and does not write player settings.
5. Write that color only when DoomNet's `settings.json` does not already exist. An existing saved color is left alone. Appearance in the player can change it later.
6. Do not install Millennium. Do not write `millennium\plugins` or `millennium\themes`. Do not delete them on this PC.

Finish already runs that install for the choices on screen. `DoomNet.exe` does not open the window when `config.json` is missing. A later launch of Steam does not show it.

## Millennium toggle on this PC

Required only on this machine, for comparison. Steam must be fully quit first (no `steam.exe`, no `steamwebhelper.exe`). The control is a menu or a tray action on the installer, not a console command, and it shows no terminal.

`off` moves the hook files Millennium placed beside `steam.exe` into a stash folder next to them and writes a marker so `on` knows what to restore. It does not delete `millennium\plugins`, `millennium\themes`, or player data. List the Steam directory before moving anything. The public source names these files, and only these, in the Steam root: `wsock32.dll` (the current bootstrap), `version.dll` (the hook they retired), `millennium-legacy.version.dll` (where that retired hook is moved), and a legacy `user32.dll` shim. Do not move any other DLL. Do not touch `System32`. `on` moves the stashed files back.

DoomNet refuses to inject while those hook files are in their original place, so Millennium and DoomNet cannot both own the page. The toggle does not talk to Millennium's update servers.

## How Steam is actually put together

The browsers are not in `steam.exe`. `steam.exe` is the parent. It loads `steamui.dll` when the UI is really starting. A launch that is only verifying files or updating can start `steam.exe` and exit before any page exists. Wait until `steamui.dll` is loaded in that process before treating startup as success. Do not inject during the update pass.

`steam.exe` then starts one or more `steamwebhelper.exe` processes. Those are Chromium. You will see several with command lines containing `--type=gpu-process`, `--type=renderer`, `--type=utility`, and one broker that has no renderer type. Only the broker has the page list. Renderer processes have no DevTools `/json` list. Attaching to a renderer and finding no pages is not a failed inject. Find the broker.

The overlay is not a window inside `steam.exe`. It is another page, created later, when Shift+Tab opens or a game starts. Its title becomes `OverlayTab<N>_Find` or starts with `SP Overlay:`. A script installed only on the first page target will never see it. The client shell is often an `about:blank` popup whose opener is a shared context. The player already borrows `window.Millennium` from that opener. Both the opener and the popup need the bridge, or the panel mounts and every call fails.

`https://steamloopback.host/<path>` is Steam itself reading `<steam>\steamui\<path>`. It is not a server DoomNet should write. Copy files into `steamui` before any page is told to load them. Store and Community are real `https://store.steampowered.com` and `https://steamcommunity.com` documents inside the same helper. A root-relative `/steam-music-player/bootstrap.js` on those pages resolves to the website, not to steamui. Bootstrap already tries `steamloopback.host` first. Keep that order.

Steam used to turn on a debugger if the empty file `<steam>\.cef-enable-remote-debugging` existed, on port 8080. Current Millennium deletes that file and instead changes the command line of `steamwebhelper` in memory, before the process starts, and picks its own port. The public header still defines the default port as `8080`, and that value is used only when Steam was started with `-dev`. Otherwise the port is chosen from the ephemeral range. Do not depend on the file. Do not assume 8080. If Millennium's hooks are still in place it will choose a random port and delete the file, which is why DoomNet must refuse to run until those hooks are stashed.

## What Millennium's public source actually does

Read these before writing `steam_attach`. They are the current tree on `SteamClientHomebrew/Millennium` `main`, not the 2025 notes.

The Windows bootstrap is `wsock32.dll` in the Steam directory, documented in `src/bootstrap/README.md`. It is a separate project from `millennium.dll`. Linux uses a `libXtst.so.6` proxy. macOS uses an app wrapper. `src/bootstrap/win32/main.c` does only this: if the process image is not `steam.exe`, return. Otherwise `AddDllDirectory` on `<steam>\millennium\lib` and `LoadLibraryExW` `<steam>\millennium\lib\millennium.dll` with `LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`. If that load fails, a message box is shown and Steam keeps running. The bootstrap does not search `PATH`.

`version.dll` is the retired hook. Commit `143e26d` (2025-12-30, "Move old version.dll in favor of new hook method") builds a stub that forwards `version.dll` exports to `System32` and moves an existing `<steam>\version.dll` to `<steam>\millennium-legacy.version.dll` so the old hook stops loading. Do not implement DoomNet as a `version.dll` beside `steamwebhelper.exe`. That method is the one they left. A proxy in the wrong directory never loads, and Steam looks normal with no skin and no player.

Once `millennium.dll` is in `steam.exe`, `initialize_steam_hooks` (`src/include/millennium/steam_hooks.h`) registers DLL load notifications and hooks `CreateSimpleProcess` in `tier0_s.dll` before `steamwebhelper` is created. Startup waits until that hook is set up. It does not wait until a page exists. `steamui.dll` loading is the signal that this launch is a real UI start rather than a verify or update pass that exits.

The Lua plugin is not inside `steam.exe`. `src/lua_host/api/millennium.cc` is a child process. `call_frontend_method`, `add_browser_js`, and `ready` are RPC calls to the parent. DoomNet may keep Lua in its own process. It still has to honor that contract:

- `call_frontend_method(methodName, params)` requires `params` to be an array of boolean, string, or number. A nested table is an error. This player always passes one already-encoded JSON string. On failure the Lua function returns two values, `nil` and an error string. On success it returns one value whose type matches the page (`string`, `boolean`, `number`, a table for an object, or `nil` for null/undefined). [backend/main.lua](backend/main.lua) wraps the call in `pcall` and does not use the page's return for `onRemoteCommand`. The page method should still run on the bridge window, because that is what fans out to `__SMP_RECEIVERS__`.
- `add_browser_js` in this player is called with a theme-relative path, `steam-music-player/bootstrap-20261027-version-1.15.0.js`, and the regex `.*`. Millennium serves that path from `https://millennium.host/v1/themes/...`, which is why `sync_bootstrap_to_theme_host` copies the file into a themes folder. A steamui path 404s on that origin, and the Store tab then has no player. DoomNet does not implement `millennium.host`. The shim returns `1` and does not insert that URL. `steam_attach` inserts the `steamloopback.host` bootstrap into webkit, the shell, and the overlay. Inserting both loads the player twice. The installed Millennium build returns `-1` even when registration worked. Do not retry on `-1`.
- `get_install_path()` is Millennium's install directory. `theme_host_dir` in [backend/main.lua](backend/main.lua) uses it only when `<install>/themes` exists. Otherwise it writes into `<steam>\millennium\themes`. Create no `themes` folder under DoomNet and do not let that fallback run during a DoomNet session, or the comparison copy of Millennium gets a second bootstrap. Point the shim's install path at `%LOCALAPPDATA%\DoomNet` and make the theme-host write a no-op when the host is DoomNet.
- `ready()` in the parent has a 10 second deadline. Missing it is treated as a plugin crash. [backend/main.lua](backend/main.lua) calls it even after a partial `on_load` failure. DoomNet records the call and does not kill the process.
- Hooks apply only to documents created after they are registered. `on_load` registers before the library scan for that reason and still misses a page that finished loading first. DoomNet installs the boot script when the helper starts, which is earlier than `on_load`. The files in `steamui` must already be there, or the first navigation caches a 404.

Millennium's own Lua is LuaJIT. A search of this backend found no `//`, `goto`, `bit32`, or `utf8`, so embed Lua 5.4. Do not switch to LuaJIT to match them.

Each plugin later got its own CDP session so one plugin cannot consume another's events. DoomNet has one backend. Still treat the helper connection as replaceable: `steamwebhelper` restarts, the session dies, and the port connection has to be opened again.

## What makes the inject succeed

DoomNet.exe is already running when `steam.exe` starts. The hook that rewrites the helper command line has to be inside `steam.exe` before `CreateSimpleProcess` runs. Attaching after `steamwebhelper` is already up misses that command line and misses documents that already exist. The Steam shortcut launches DoomNet. DoomNet places its bootstrap only while Millennium's `wsock32.dll` is stashed, starts `steam.exe`, and the bootstrap loads DoomNet's library by absolute path, and only when the process image is `steam.exe`. If that load fails, log it, leave Steam usable, and do not trap the user out of the client. That is the failure behavior in Millennium's bootstrap.

The bootstrap has to keep Steam's real `wsock32` exports working. A missing export is why a bad file in that spot crashes the client or leaves it unable to open a connection. Do not put that file down while Millennium's copy is still there.

The attach lives in `steam_attach` only. Everything after the page list exists is the same for a Steam update. A Steam update that changes `CreateSimpleProcess` should break only `steam_attach`.

Once the helper's page list is up:

- Connect to the port DoomNet itself chose and recorded. Retry. The helper restarts on crashes and on some UI reloads, and the port connection dies with it.
- Enumerate targets whose type is `page`. Ignore `iframe`, `worker`, `service_worker`, and `browser`.
- Subscribe to new targets for the life of the process. The overlay target does not exist at startup.
- On each page, and on every new document in that page, run the boot script in the page's main world. Do not use an isolated world. An isolated world cannot set `window.Millennium` where [backend/assets/frontend/steam-music-player.js](backend/assets/frontend/steam-music-player.js) looks for it, and the panel stays empty with no error on screen.
- Also run the boot script once against pages that already exist, or the first window, which loaded before the subscription, never gets it.
- The boot script defines `window.Millennium.callServerMethod` and then inserts the player and skin tags. It does not `fetch` `http://127.0.0.1`. Steam's pages are `https://steamloopback.host` and `https://store.steampowered.com`. A loopback fetch is blocked or dropped, and the failure looks exactly like a dead backend. The call has to be a binding installed in that page, which returns a Promise.
- Re-check `document.title` after load. Overlay titles are assigned after the document is created. Match skin patches again when the title changes.
- Skip iframes. Bootstrap already exits when `window !== window.top`. Injecting the full player into every store iframe is what made the Store tab crawl.

Do not patch Steam's `chunk~.js` files. Millennium does that to edit Steam's own UI. DoomNet only needs to add tags. Patching chunks breaks on every Steam update and is the usual source of a client that opens to a white screen.

Copy `steamui\steam-music-player\` and, if the skin is on, `steamui\doomnet-skin\` before the boot script runs. If the copy happens late, the script tag 404s, CEF caches the failure, and a retry with the same URL never runs. Bump `ASSET_REV` when the JS or CSS changes.

`UseDefaultPatches` in `skin.json` is Millennium's extra CSS. Do not apply it.

## Injection

`on_browser(title, url)` runs from the boot script, in the page, after the binding exists. Steam updates should only force a change in `steam_attach`, not in the player. Do not ship a visible debugger window or a console. The user starts Steam through DoomNet on purpose.

`on_browser` decides from `config.json`:

- Player enabled: execute a one-line insert of `bootstrap.js` from `https://steamloopback.host/steam-music-player/bootstrap.js?v=ASSET_REV`, and install `window.Millennium` in that page before the bootstrap runs. Skip iframes. Targets include title `Steam`, `OverlayBrowser_Browser`, `SP Overlay:`, and `OverlayTab` plus digits plus `_Find`, plus `store.steampowered.com` and `steamcommunity.com` top frames. The same guard inside bootstrap prevents a double insert.
- Skin enabled: read `skin\skin.json` `Patches`. For each entry whose `MatchRegexString` matches the title, the URL, or a class name, insert that `TargetCss` as a `<link>` or that `TargetJs` as a `<script>`, loaded from `https://steamloopback.host/doomnet-skin/<file>`. The visible library is a popup. Its document is reached from `g_PopupManager.GetPopups()`, which returns a map iterator. Match `^Steam$` against `m_strTitle`. The store and community views are separate documents. Their responses are not held, because copying a store document froze the library tab. Store CSS is inserted as a style tag on that page, with content security policy bypassed. That store session stays attached, and the same style is applied again on every main-frame navigation, so leaving the store and coming back does not restore the blue chrome. Each popup is patched once, when it is created, from its title and `body_class`, then marked `HAS_INJECTED_THEME`. The live document class list is not read and class mutations are not observed. Skin scripts are modules inserted at that moment. Observers those scripts create are deferred until the page is idle, so the first library click is not blocked by a document-wide query. Before the window is shown, the boot script opens Library, waits until capsule images have started to decode, then returns to Store. That first build happens while the window is hidden. After the skin is in place the library debugger stays attached. Detaching it dropped the answers to player clicks. The library grid is built while the window is hidden. After a store navigation the live document is checked, and the skin and player are put back when that navigation dropped them. Checkout is not patched. Documents are not reloaded. Copy the skin files into `<steam>\steamui\doomnet-skin\` before the boot script runs so steamloopback can serve them. Skip any patch whose `TargetJs` is `music-player-inject.js`. The player is not loaded by the skin. A combined install must not insert the player twice.
- Skin files in use today: `libraryroot.custom.css`, `topbar-late.css`, `game-page.css`, `library-list.css`, `header-family.css`, `account-menu.css`, `dialogs.css`, `webkit.css`, `client-neutralize.js`, `game-page.js`, `webkit.js`, `webkit-neutralize.js`. Match strings include `^Steam$`, `2dcc5aaf7`, `Menu$`, `Settings$`, `Properties$`, `.ModalDialogPopup`, `.FullModalOverlay`, `https://.*\.steampowered\.com`, `https://steamcommunity\.com`, and the wishlist/community regex already in `skin.json`.

`UseDefaultPatches` in `skin.json` is Millennium's own extra CSS. DoomNet does not apply Millennium default patches.

Install `window.Millennium` as a page script that DoomNet injects, not as a real Millennium binary:

```javascript
window.Millennium = {
  callServerMethod: function (pluginName, methodName, argsArray) {
    // binding installed in this page, not fetch to 127.0.0.1
    // returns a Promise of the Lua return value
  }
};
```

The existing JS checks `typeof candidate.Millennium.callServerMethod === "function"` and then `.then` on the result. The shim must return a Promise. Arguments are a positional array, never a dictionary. Lua receives each element as its own argument. Return values are usually JSON strings. The JS already `JSON.parse`s strings. Keep that. Plugin name is `SteamMusicPlayer` and can be ignored as long as the method name is dispatched.

`millennium.call_frontend_method(name, args)` from Lua runs `SteamMusicPlayer.<method>` in every injected page that has published a receiver. Methods that exist today:

- `onStateUpdate(stateJson)`
- `onSettingsUpdate(settingsJson)`
- `applyLiveMix(settings)`
- `getMixProbe()`
- `unlockAudio()`
- `onRemoteCommand(commandJson)` — returns true if that page had an engine and applied the command

Several pages register at once via `__SMP_RECEIVERS__` on the bridge window. A push must hit the bridge window, not only the overlay, or the fan-out never runs. Pages that borrow the opener's bridge still receive the call if the opener's `SteamMusicPlayer` fan-out is invoked.

## Lua host

Embed Lua 5.4. A search of `backend\` found no `//`, `goto`, `bit32`, or `utf8`. Millennium's host is LuaJIT. These scripts do not need it.

Load `backend/main.lua` with `backend\` as `package.path`. These modules are already in the repo and stay: `utils`, `library`, `player_state`, `playlists`, `playlist_file`, `settings`, `discord`, `loudness`, `ducking`, `procexec`, `id3`, `flac`, `m4a`.

DoomNet supplies the four host modules Millennium used to supply.

`fs`: `join` (nil-safe: joining onto nil must not throw; player_state.lua depends on that), `exists`, `extension`, `remove`, `create_directories`, `list`, `is_directory`. `fs.is_directory` is true when Windows reports the directory attribute, including a folder the C++ status check misses. Add Folder calls `pick_music_folder`, which opens the Windows folder chooser and saves that path. The library walk stacks every listed child marked as a directory, and every child that is not a file, so a scan reads the chosen folder, the folders inside it, and the folders inside those. Audio files that sit directly in the chosen folder are included. The scan status names that folder.

`json`: `encode` and `decode`. Encode must accept the tables the backend builds, including arrays. Decode must return a table for objects and arrays.

`logger`: `info`, `warn`, `error`, writing to `logs\doomnet.log` with a timestamp. No console.

`utils`: `get_backend_path` returns the `backend` directory. `read_file` and `write_file` are binary-safe. `hash` is stable for path keys (the current player uses it for track ids and playlist file ids; a different algorithm splits the library from saved playlists, so preserve the existing hash if the data copy is used). `uuid`. `base64_encode`. `sleep(ms)`. `getenv`. `exec` starts a process with no console and returns its output when the caller expects to wait. Prefer going through procexec for long-running workers. Do not implement `exec` as `_popen`.

`millennium` shim:

- `steam_path()` returns the Steam directory from `config.json`.
- `get_install_path()` returns `%LOCALAPPDATA%\DoomNet`. Do not create a `themes` directory there. `theme_host_dir` in [backend/main.lua](backend/main.lua) falls through to `<steam>\millennium\themes` when `<install>/themes` is missing, so the shim must make `sync_bootstrap_to_theme_host` a no-op under DoomNet. Otherwise the comparison copy of Millennium receives a second bootstrap.
- `version()` returns `"doomnet"`.
- `add_browser_js` returns `1` and does not insert a `millennium.host` script. The real insert is `steam_attach`. Returning `-1` is what the installed Millennium build does on success. Do not retry.
- `call_frontend_method(name, argsTable)` runs the JS method on the bridge window. Args are a one-element list whose element is already a JSON string. Reject a nested table the way Millennium does. Failure returns `nil` plus an error string. Success returns one value.
- `ready()` records that `on_load` finished. There is no 10-second kill. Still let `on_load` call it.

Global Lua functions, not `local`, because dispatch is by name. Each returns a string unless noted. The JS sends the argument list in this order:

- `report_frontend_error(context, message, title, url)`
- `report_frontend_event(context, message, title, url)`
- `report_frontend_boot(context, phase, title, url)`
- `get_settings()`
- `set_setting(key, value)`
- `get_duck_envelope()`
- `get_duck_status()`
- `get_library()`
- `get_library_info()`
- `rescan_library()`
- `rescan_library_start(forceAll, newOnly)` — JS booleans may arrive as the strings `"true"` / `"false"`. [backend/settings.lua](backend/settings.lua) already documents that for settings. The scan entry points must accept the same.
- `scan_library_batch(batchSize)`
- `get_track_meta(trackId)`
- `get_track_art(trackId)`
- `get_first_track_art(idList)`
- `request_idle_art(idList)`
- `restore_published_art()`
- `set_display_art(trackId, sourcePath)`
- `begin_display_art(trackId)`
- `append_display_art(trackId, chunk)`
- `finish_display_art(trackId)`
- `queue_artwork_regenerate()`
- `start_queued_artwork_regenerate()`
- `regenerate_artwork()`
- `get_art_progress()`
- `get_track_loudness(trackId)`
- `get_all_loudness()`
- `set_track_loudness(trackId, lufs, peak)`
- `get_loudness_stats()`
- `get_track_audio_chunk(trackId, offset)` — must return base64 via `utils.base64_encode`
- `stage_track_for_playback(trackId)`
- `release_staged_track(trackId)`
- `get_host_escape_probe()`
- `get_music_folders()`
- `add_music_folder(path)`
- `remove_music_folder(path)`
- `get_playlists()`
- `create_playlist(name)`
- `delete_playlist(id)`
- `rename_playlist(id, name)`
- `playlist_add_track(id, trackId)`
- `playlist_remove_track(id, trackId)`
- `get_player_state()`
- `get_player_state_full()`
- `set_ui_chrome(panelOpen, currentTab)`
- `set_ui_pointer(pointerJson)`
- `set_player_state(stateJson)`
- `claim_audio_owner(clientId, priority, nowMs)`
- `nudge_live_mix(settingsJson)`
- `release_audio_owner(clientId)`
- `take_pending_transport()`
- `transport_command(commandJson)` — also pushes `SteamMusicPlayer.onRemoteCommand`
- `restart_plugin_runtime()` — recopies assets into steamui, clears staging, does not wipe `data\`

`on_load` also calls `ducking.init(dataDir, backendDir, millennium.steam_path())` and may start the ducking helper and the Discord bridge when those settings are on. Both must stay windowless.

Asset copy: the files in `ASSET_FILES` are `bootstrap.js`, `steam-music-player.css`, `audio-engine.js`, `steam-music-player.js`. Destination is `<steam>\steamui\steam-music-player\`. Stop writing `bootstrap-20261027-version-1.15.0.js` into a Millennium themes directory. That write exists only because Millennium served browser JS from `millennium.host`. DoomNet inserts the steamloopback URL directly.

After the proof below, and only then, remove the Millennium shipping path from this repo: [scripts/open-plugin-database-pr.sh](scripts/open-plugin-database-pr.sh), [.github/workflows/publish-production.yml](.github/workflows/publish-production.yml), [scripts/should-ship.sh](scripts/should-ship.sh), [scripts/deploy.ps1](scripts/deploy.ps1), [scripts/prepare-millennium-frontend.js](scripts/prepare-millennium-frontend.js), and the README install path that points at the Plugin Database. Leave the player version check if it is still useful locally. Leave every `millennium.*` call and `window.Millennium` lookup until a second, paired release renames them to `host` and `window.Host` in this repo and in DoomNet together. Do not contact Millennium. Do not do either cleanup while the host is still being written.

## Failures that look like a working install

- Steam opens with no skin and no player. The bootstrap is not beside `steam.exe`, it was loaded after `steamwebhelper` started, or Millennium's `wsock32.dll` is still in place and DoomNet correctly refused to inject.
- Steam opens and immediately closes, or it cannot reach the network. The bootstrap in the Steam directory did not forward `wsock32` exports, or the helper command line was damaged. A `version.dll` left over from the retired hook does the same thing if its exports are incomplete.
- The panel is on screen and stays empty, or Settings never fills. `window.Millennium` is missing on the opener, or the script ran in an isolated world, or the binding was a loopback fetch that the page blocked.
- The client has the player and the overlay does not. Only the first page target was hooked. New targets were not subscribed.
- Store is slow or the player appears in iframes. The boot script did not bail out when `window !== window.top`.
- A file change does not show up. `ASSET_REV` was not changed, or a failed script URL was cached.
- A black command window flashes over a game. A child was started with `_popen` or `cmd.exe /c` instead of `CREATE_NO_WINDOW`.
- Player-only still looks skinned, or skin-only still shows the player. `music-player-inject.js` was still applied, or Millennium is still injecting the old copies.

## Build order

1. Windowless exe, single instance, log file, Steam discovery, shortcut retarget, Steam starts with no console.
2. Lua host and the four shims. `get_settings` and `get_library_info` return the same JSON the JS already parses. No injection yet. A tiny test page is enough to prove `callServerMethod`.
3. `steam_attach` inserts bootstrap plus the Millennium shim into the main Steam window only. Library list and playback work. Millennium toggle is off for this run.
4. Same insert for one Shift+Tab overlay. Transport buttons on the overlay move the client audio. Switching games does not restart playback.
5. Skin patches from `skin.json`, with `music-player-inject.js` skipped. Confirm skin-only (no player UI), player-only (no skin CSS), and both.
6. Color question writes `uiColorTheme` only on a new `settings.json`.
7. Millennium `on` restores the hooks. DoomNet stays refused while those files are back, so the comparison session is Millennium alone.

## Done when

- Shortcut opens Steam. No console appears before or during launch. A second click does not open a second Steam or a second DoomNet.
- Player-only: empty library shows "No Music Detected" and the Settings button. After a folder is added, tracks play in the client and in one overlay. About reads the `PLUGIN_VERSION` in the JS file.
- Skin-only: Steam-Skin CSS and JS apply. The music panel is absent.
- Both: skin applies, player applies once, and a new settings file uses the color chosen in the installer.
- `millennium off` and `millennium on` move hook files without deleting plugins, themes, or data, and show no console. DoomNet will not inject while the hooks are active.
- PowerShell scan, art workers, and procexec do not flash a window over a game.
