# Rules for filling this skeleton

The spec in this repo is `own_steam_loader_a1dd8dbe.plan.md`. If that spec and a file in SteamTunes or SteamSkin disagree, the file wins. Update the spec copy in the same change. Do not guess.

`tests\Check-Contracts.ps1` reads those files and fails when a load-bearing fact moves. Run it before and after every change. `tests\Check-Skeleton.ps1` must stay green. A change that needs the skeleton test loosened is not done.

## Do not

- Edit `D:\Documents\Repos\SteamTunes` or `D:\Documents\Repos\SteamSkin` while building this host. The player must still load under Millennium.
- Add a DoomNet-only function to the player. `io.open`, `fs`, and `utils.sleep` are replaced inside this host.
- Put a hook, a debugger client, or an export forward in `src\steam_attach.cpp` until the windowless exe and the Lua host already pass their tests. That file is the only one a Steam update should force open.
- Invent `utils.hash`, `utils.base64_encode`, `utils.exec`, or `fs.join`. Read Millennium `src/lua_host/api/utils.cc` and `src/lua_host/api/filesystem.cc` on `main`, then add a known input and output to a test before changing them. `fs.join(nil, ...)` must not throw. `player_state.lua` depends on that.
- Start a thread pool, call `lua_*` off the state thread, or block the state thread on a sleep or a page round-trip.
- Fetch `http://127.0.0.1` from a Steam page. The page binding is not HTTP.
- Raise `CHUNK_SIZE` above `512 * 1024`.
- Apply `music-player-inject.js` or `UseDefaultPatches`.
- Write `uiColorTheme` when `settings.json` already exists.
- Delete or stash any DLL in the Steam directory other than `wsock32.dll`, `version.dll`, `millennium-legacy.version.dll`, and a legacy `user32.dll`. Never touch `System32`.

- `launch_hidden` must not be pointed at `steam.exe`. The guard stays until `steam_attach` is in place and tested.
- Shortcut edits in tests use a temporary `.lnk` only. Do not open the real Steam shortcut on the desktop.
- Payload copies must refuse a destination inside the Steam install. Uninstall must refuse the Steam client and the SteamTunes and Steam-Skin source folders. A piece that was never installed is skipped.

## Logging

Every step writes one sentence, on success and on failure. The line names the component (`startup`, `instance`, `steam`, `config`, `payload`, `launch`, `shortcut`, `installer`, `wideio`, `lua`, `browser`; later `cdp`, `relay`, `hook`, `stream`, `inject`, `toggle`), the path or value involved, and `windows_error_text` when Windows failed. Do not log a bare number or "error 404". The last line of a run should be enough to see which step stopped. New code keeps that shape.

## Order

1. Windowless process, single instance (`Local\DoomNet`, no wait), log line under `Local\DoomNetLog`, Steam discovery, and `config.json` load. `DOOMNET_ROOT` is the test override for the data directory. `tests\Check-Skeleton.ps1` and `DoomNetFoundationTests` are the gate. The no-argument path does not create a Lua state and does not stay running. `--run` and `--watch` are the only resident paths; they may start the Lua host and they must not start `steam.exe` until `steam_attach` is in place and tested.
2. Lua host and the four shims, proven with `get_settings` and `get_library_info` against the JSON the page already parses. No injection.
3. Only then fill `steam_attach`.
4. Main window, then one overlay, then skin-only / player-only / both.
5. Millennium publish scripts and the `millennium` module name stay until that proof is done. The rename to `host` / `window.Host` is a later paired release of this repo and SteamTunes together.
