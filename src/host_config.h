#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace doomnet {

struct HostConfig {
    // Folder that holds main.lua (the player's backend directory).
    std::wstring backend_dir;
    // Steam install folder. millennium.steam_path() returns this. When empty the
    // host substitutes a folder under install_path, so the player's writes to
    // "<steam>\steamui" cannot land in the current directory.
    std::wstring steam_path;
    // DoomNet's own data folder. millennium.get_install_path() returns this, and
    // its "themes" subfolder is created so the player's theme-host write lands
    // here and not inside the Steam install.
    std::wstring install_path;
    // Lua methods (global function names) inside which file:read(n) of
    // kParkReadThreshold bytes or more parks the coroutine (a ReadRange).
    // Every other call, including scan_library_batch, reads on the state
    // thread with no yield so its tables are never seen half built.
    std::vector<std::string> parking_read_methods = {"get_track_audio_chunk"};
    // Lua source run once on the state, after main.lua has loaded and before
    // on_load. This is where the host replaces a player global (the spec's
    // stage_track_for_playback) without editing the player. Tests use it to
    // define probe functions. A failure is logged and does not stop startup.
    std::string post_load_lua;
};

constexpr uint32_t kParkReadThreshold = 64 * 1024;

// Reserved method names the browser side may post as a Call.
inline constexpr const char* kLifecycleFrontendLoaded = "__lifecycle.on_frontend_loaded";

}  // namespace doomnet
