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

struct StoreInstallRequest {
    std::wstring root;
    std::wstring skin_source;
    std::wstring steam_path;
};

// Copies SteamSkin into root\skin, turns config.skin on, and records the
// skin.json version. settings.json is not opened. An empty skin_source uses
// payload\skin beside this exe, then D:\Documents\Repos\SteamSkin.
InstallResult install_store_skin(const StoreInstallRequest& request);

// The page calls this from the state thread. The resident loop performs the copy.
void queue_store_install(const StoreInstallRequest& request);
bool take_store_install(StoreInstallRequest& out);

// True when the first token is --install. The no-argument path is false.
bool command_is_install(const wchar_t* command_line);

// True when the first token is --hook. This only copies version.dll beside steam.exe.
bool command_is_hook(const wchar_t* command_line);

// `--hook "<steam folder>"`. Does not copy the player or the skin and does not start Steam.
InstallResult run_hook_command(const wchar_t* command_line);

// --install --player --steam "path" [--skin] [--color steam|red]
// [--player-source path] [--skin-source path]. Does not start Steam.
InstallResult run_command_line_install(const std::wstring& root, const wchar_t* command_line, const std::wstring& discovered_steam);

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
