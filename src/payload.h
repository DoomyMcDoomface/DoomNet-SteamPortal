#pragma once

#include <string>

namespace doomnet {

struct InstallRequest {
    bool player = false;
    bool skin = false;
    bool match_skin = false;
    std::wstring player_source;
    std::wstring skin_source;
    std::wstring steam_path;
};

struct InstallResult {
    bool ok = false;
    int files_copied = 0;
    std::wstring detail;
};

// Copies the chosen payloads into root and writes config.json.
// Refuses any destination inside the Steam install. Does not start Steam.
// Writes uiColorTheme only when player\backend\data\settings.json is absent.
InstallResult apply_install(const std::wstring& root, const InstallRequest& request);

struct UninstallRequest {
    std::wstring root;
    std::wstring steamui;
    std::wstring shortcut;
};

struct UninstallResult {
    bool ok = false;
    bool removed_player = false;
    bool removed_skin = false;
    bool skipped_player = false;
    bool skipped_skin = false;
    std::wstring detail;
};

// Removes the DoomNet folder and only the steamui subfolders DoomNet created.
// A piece that was never installed is skipped. Source repos and the Steam client are refused.
UninstallResult apply_uninstall(const UninstallRequest& request);

}
