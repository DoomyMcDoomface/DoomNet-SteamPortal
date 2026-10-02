#pragma once

// The Millennium toggle. On this PC, Millennium and DoomNet cannot both own the
// Steam page. `off` moves the hook files Millennium placed beside steam.exe into
// a stash folder and records what it moved. `on` moves them back. DoomNet refuses
// to inject while Millennium's hooks are still in their original place.
//
// Only these names, and only from the Steam root, are ever moved:
//   wsock32.dll, version.dll, millennium-legacy.version.dll, user32.dll
// System32 is never touched. millennium\plugins, millennium\themes, and player
// data are never deleted.

#include <string>
#include <vector>

namespace doomnet {

enum class MillenniumState {
    Active,      // a Millennium hook file is beside steam.exe and nothing is stashed
    Stashed,     // DoomNet has moved the hook files into the stash folder
    NotPresent,  // no Millennium hook file and no stash
    Unknown,     // the Steam path could not be used
};

struct ToggleResult {
    bool ok = false;
    bool changed = false;                 // files were actually moved this call
    MillenniumState state = MillenniumState::Unknown;  // state after the call
    std::vector<std::wstring> moved;      // base names moved this call
    std::wstring detail;                  // one full sentence, also logged
};

// True when steam.exe or steamwebhelper.exe is running anywhere on this machine.
// `which` names the first process found. The toggle must not move files while
// Steam is running.
bool steam_is_running(std::wstring& which);

// What is beside steam.exe right now. Unknown when the path is unusable.
MillenniumState millennium_state(const std::wstring& steam_path);

// Millennium's active bootstrap wsock32.dll exports __get_shim_version. DoomNet's
// own wsock32.dll (a later step) does not, so this tells the two apart without
// loading the DLL. False when the file is absent or is not Millennium's.
bool is_millennium_bootstrap(const std::wstring& dll_path);

// off / on. When `require_steam_closed` is true (the real menu action) the call
// refuses while Steam is running. Tests pass false and operate on a fake Steam
// directory. Both refuse an empty path, a path with "..", a path with no
// steam.exe, and any path inside the Windows directory.
ToggleResult millennium_off(const std::wstring& steam_path, bool require_steam_closed);
ToggleResult millennium_on(const std::wstring& steam_path, bool require_steam_closed);

void toggle_anchor();

}  // namespace doomnet
