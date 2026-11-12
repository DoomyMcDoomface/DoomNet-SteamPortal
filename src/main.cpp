// Spec: Process model.
// Read: D:\Documents\Repos\SteamTunes (player), and
// own_steam_loader_a1dd8dbe.plan.md in this repo.
//
// Entry is wWinMain. Subsystem is WINDOWS, so this process has no console.
// Local\DoomNet is taken without waiting. A second launch exits 0.
// With no arguments this process does not stay running, start Steam, or inject.
// `--run` and `--watch` stay until Local\DoomNetStop or <root>\stop.flag, and
// they start the Lua host when the player is installed. They still do not
// start steam.exe. `--attach` is what the bootstrap starts when the user
// opens Steam, and that path injects into the client that is already running.
// Every step writes one sentence to logs\doomnet.log.

#include "anchors.h"
#include "catalog.h"
#include "config.h"
#include "installer.h"
#include "instance.h"
#include "log.h"
#include "paths.h"
#include "payload.h"
#include "resident.h"
#include "steam_discover.h"
#include "text.h"
#include "wide_io.h"

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

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command_line, int) {
    doomnet::StartupMode mode = doomnet::StartupMode::Once;
    std::wstring rejected_argument;
    const bool known_argument = doomnet::parse_startup_mode(command_line, mode, rejected_argument);
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
    if (doomnet::command_is_reload_steam(command_line)) {
        return doomnet::run_reload_steam_command(command_line);
    }
    if (doomnet::command_is_hook(command_line)) {
        const doomnet::InstallResult hooked = doomnet::run_hook_command(command_line);
        return hooked.ok ? 0 : 1;
    }
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
        if (doomnet::command_is_install(command_line)) {
            const wchar_t* sentence = L"Install did not run because DoomNet is already running. It has no window. End DoomNet.exe in Task Manager, then run setup again. Nothing was copied.";
            doomnet::log_event(L"error", L"installer", sentence);
            const std::string result_text = doomnet::wide_to_utf8(sentence);
            std::wstring result_error;
            doomnet::write_new_file(root + L"\\install-result.txt", result_text.data(), result_text.size(), result_error);
            return 1;
        }
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
    if (doomnet::command_is_install(command_line)) {
        const doomnet::InstallResult installed = doomnet::run_command_line_install(root, command_line, steam);
        if (!installed.ok && !installed.detail.empty()) {
            doomnet::log_event(L"error", L"installer", installed.detail.c_str());
        }
        const std::string result_text = doomnet::wide_to_utf8(installed.detail);
        std::wstring result_error;
        doomnet::write_new_file(root + L"\\install-result.txt", result_text.data(), result_text.size(), result_error);
        return installed.ok ? 0 : 1;
    }
    if (!known_argument) {
        const std::wstring detail = L"The argument '" + rejected_argument + L"' is not --run, --watch, or --attach, so this process is exiting without staying running and without starting the Lua host.";
        doomnet::log_event(L"error", L"startup", detail.c_str());
        return 0;
    }
    if (mode == doomnet::StartupMode::Once) {
        return 0;
    }
    doomnet::run_resident(mode, root, steam, config);
    return 0;
}
