// Spec: Process model.
// Read: D:\Documents\Repos\SteamTunes (player), and
// own_steam_loader_a1dd8dbe.plan.md in this repo.
//
// Entry is wWinMain. Subsystem is WINDOWS, so this process has no console.
// Local\DoomNet is taken without waiting. A second launch exits 0.
// This process does not start Steam, retarget a shortcut, or inject.
// Every step writes one sentence to logs\doomnet.log.

#include "anchors.h"
#include "config.h"
#include "installer.h"
#include "instance.h"
#include "log.h"
#include "paths.h"
#include "steam_discover.h"

#include <windows.h>

#include <string>

namespace {

const wchar_t* yes_no(bool value) {
    return value ? L"on" : L"off";
}

const wchar_t* color_name(doomnet::PlayerColor color) {
    return color == doomnet::PlayerColor::Red ? L"red" : L"steam";
}

void log_config_result(const std::wstring& path, doomnet::ConfigLoad loaded, const doomnet::Config& config, const std::wstring& error) {
    if (loaded == doomnet::ConfigLoad::Missing) {
        const std::wstring detail = L"'" + path + L"' is not there yet. Using defaults: player off, skin off, playerColor steam. The installer has not written a config.";
        doomnet::log_event(L"info", L"config", detail.c_str());
        return;
    }
    if (loaded == doomnet::ConfigLoad::Error) {
        const std::wstring detail = error.empty() ? L"config.json was rejected and the in-memory defaults were left unchanged." : error;
        doomnet::log_event(L"error", L"config", detail.c_str());
        return;
    }
    const std::wstring detail = L"Loaded '" + path + L"'. player is " + yes_no(config.player) + L", skin is " + yes_no(config.skin) + L", playerColor is " + color_name(config.player_color) + L" (" + std::wstring(config.player_color == doomnet::PlayerColor::Red ? L"match Steam-Skin" : L"Steam Blue") + L"), steamPath is '" + config.steam_path + L"'.";
    doomnet::log_event(L"info", L"config", detail.c_str());
    if (doomnet::color_question_needed(config.player, config.skin)) {
        doomnet::log_event(L"info", L"config", L"Both player and skin are enabled, so the installer would ask whether the player starts on Steam Blue or matches Steam-Skin. This startup does not show that window and does not write settings.json.");
    } else if (!config.player) {
        doomnet::log_event(L"info", L"config", L"The player is off, so the installer would not ask for a player color and would not write uiColorTheme.");
    } else {
        doomnet::log_event(L"info", L"config", L"The player is on and the skin is off, so a new settings.json would use Steam Blue (uiColorTheme steam). An existing settings.json would be left alone.");
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    doomnet::log_anchor();
    doomnet::config_anchor();
    doomnet::steam_discover_anchor();
    doomnet::state_thread_anchor();
    doomnet::browser_thread_anchor();
    doomnet::steam_attach_anchor();
    doomnet::stream_anchor();
    doomnet::host_fs_anchor();
    doomnet::host_json_anchor();
    doomnet::host_logger_anchor();
    doomnet::host_utils_anchor();
    doomnet::host_millennium_anchor();
    doomnet::toggle_anchor();
    doomnet::installer_anchor();

    std::wstring root;
    std::wstring root_error;
    const bool rooted = doomnet::data_root(root, root_error);
    if (!rooted) {
        SetEnvironmentVariableW(L"DOOMNET_ROOT", nullptr);
        doomnet::log_event(L"error", L"startup", root_error.c_str());
        return 0;
    }
    const wchar_t* root_source = GetEnvironmentVariableW(L"DOOMNET_ROOT", nullptr, 0) > 0 ? L"DOOMNET_ROOT" : L"LOCALAPPDATA";
    const std::wstring root_detail = L"Data root is '" + root + L"' from " + root_source + L".";
    doomnet::log_event(L"info", L"startup", L"Process started. No console is attached. This build does not start Steam and does not inject.");
    doomnet::log_event(L"info", L"startup", root_detail.c_str());

    if (!doomnet::acquire_instance()) {
        doomnet::log_event(L"info", L"instance", L"Local\\DoomNet is already held by another DoomNet.exe. This process is exiting without starting Steam, writing config, or injecting.");
        return 0;
    }
    doomnet::log_event(L"info", L"instance", L"Acquired Local\\DoomNet. This process is the owner and will continue startup.");

    std::wstring steam;
    if (!doomnet::discover_steam_path(steam)) {
        doomnet::log_event(L"warn", L"startup", L"Steam discovery did not return a folder. Later steps that need steam.exe cannot run until an install is found.");
    }

    doomnet::Config config;
    std::wstring config_error;
    const std::wstring config_path = root + L"\\config.json";
    const doomnet::ConfigLoad loaded = doomnet::load_config_file(config_path, config, config_error);
    log_config_result(config_path, loaded, config, config_error);

    doomnet::log_event(L"info", L"startup", L"Startup checks finished. Steam was not started and nothing was injected.");
    return 0;
}
