#pragma once

#include <string>

namespace doomnet {

enum class PlayerColor { Steam, Red };

struct Config {
    std::wstring steam_path;
    bool player = false;
    bool skin = false;
    PlayerColor player_color = PlayerColor::Steam;
    std::wstring streamtunes_relay;
    bool updates = false;
};

enum class ConfigLoad { Missing, Ok, Error };

// Missing is success with `out` left as the defaults above.
// A file larger than 64 KiB, invalid UTF-8, an unknown key, a bad color,
// or a ".." path is Error and `out` is unchanged.
ConfigLoad load_config_file(const std::wstring& path, Config& out, std::wstring& error);
bool save_config_file(const std::wstring& path, const Config& config, std::wstring& error);

}
