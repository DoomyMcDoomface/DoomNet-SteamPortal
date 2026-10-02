#include "art_thumbs.h"
#include "cdp_pipe.h"
#include "host_bus.h"
#include "host_internal.h"
#include "config.h"
#include "installer.h"
#include "instance.h"
#include "launch.h"
#include "log.h"
#include "paths.h"
#include "page_bridge.h"
#include "payload.h"
#include "pipe_session.h"
#include "shortcut.h"
#include "skin_page.h"
#include "steam_attach.h"
#include "steam_discover.h"
#include "state_thread.h"
#include "toggle.h"
#include "wide_io.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failed = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL  %s\n", message);
        ++g_failed;
    }
}

int g_temp_dirs = 0;

std::wstring temp_dir() {
    wchar_t buf[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buf);
    wchar_t unique[80] = {};
    swprintf_s(unique, L"doomnet-foundation-%lu-%d", GetCurrentProcessId(), g_temp_dirs++);
    std::wstring path = buf;
    if (!path.empty() && path.back() != L'\\') {
        path.push_back(L'\\');
    }
    path += unique;
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

void test_paths() {
    SetEnvironmentVariableW(L"DOOMNET_ROOT", L"relative\\doomnet");
    std::wstring root;
    std::wstring error;
    check(!doomnet::data_root(root, error), "relative DOOMNET_ROOT accepted");
    check(error.find(L"relative\\doomnet") != std::wstring::npos && error.find(L"absolute path") != std::wstring::npos, "relative path error does not quote the value");
    SetEnvironmentVariableW(L"DOOMNET_ROOT", L"C:\\doomnet\\..\\Windows");
    check(!doomnet::data_root(root, error), "dotdot DOOMNET_ROOT accepted");
    check(error.find(L"..") != std::wstring::npos, "dotdot error does not mention ..");
    const std::wstring dir = temp_dir();
    SetEnvironmentVariableW(L"DOOMNET_ROOT", dir.c_str());
    check(doomnet::data_root(root, error) && root == dir, "absolute DOOMNET_ROOT rejected");
    const std::wstring winerr = doomnet::windows_error_text(ERROR_FILE_NOT_FOUND);
    check(winerr.find(L"Windows error 2") != std::wstring::npos && winerr.find(L"(") != std::wstring::npos, "windows error is only a number");
}

void test_config() {
    const std::wstring dir = temp_dir();
    const std::wstring path = dir + L"\\config.json";
    doomnet::Config config;
    config.steam_path = L"C:\\Program Files (x86)\\Steam";
    config.player = true;
    config.skin = true;
    config.player_color = doomnet::PlayerColor::Red;
    std::wstring error;
    check(doomnet::save_config_file(path, config, error), "save config");
    doomnet::Config loaded;
    check(doomnet::load_config_file(path, loaded, error) == doomnet::ConfigLoad::Ok, "load config");
    check(loaded.steam_path == config.steam_path && loaded.player && loaded.skin && loaded.player_color == doomnet::PlayerColor::Red, "round trip");

    doomnet::Config missing;
    check(doomnet::load_config_file(dir + L"\\nope.json", missing, error) == doomnet::ConfigLoad::Missing, "missing config");

    const char* bad[] = {
        "{\"playerColor\":\"blue\"}",
        "{\"steamPath\":\"C:\\\\Steam\\\\..\\\\Windows\",\"player\":true,\"skin\":false,\"playerColor\":\"steam\"}",
        "{\"player\":true,}",
        "{",
        "{\"unknown\":true}",
        "{\"player\":true,\"player\":false,\"skin\":false,\"playerColor\":\"steam\",\"steamPath\":\"C:\\\\Steam\"}",
    };
    for (const char* text : bad) {
        const std::wstring bad_path = dir + L"\\bad.json";
        HANDLE handle = CreateFileW(bad_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(handle != INVALID_HANDLE_VALUE, "open bad config");
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(handle, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
            CloseHandle(handle);
        }
        doomnet::Config ignored;
        ignored.player = true;
        const doomnet::ConfigLoad status = doomnet::load_config_file(bad_path, ignored, error);
        check(status == doomnet::ConfigLoad::Error, text);
        check(ignored.player, "failed load mutated the output");
        if (std::strstr(text, "blue") != nullptr) {
            check(error.find(L"playerColor") != std::wstring::npos && error.find(L"blue") != std::wstring::npos && error.find(L"Steam Blue") != std::wstring::npos, "color error does not name the bad value and the allowed ones");
        }
    }

    std::string huge(70 * 1024, ' ');
    const std::wstring huge_path = dir + L"\\huge.json";
    HANDLE huge_file = CreateFileW(huge_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (huge_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(huge_file, huge.data(), static_cast<DWORD>(huge.size()), &written, nullptr);
        CloseHandle(huge_file);
    }
    check(doomnet::load_config_file(huge_path, config, error) == doomnet::ConfigLoad::Error, "oversize config accepted");
}

void test_color() {
    check(!doomnet::color_question_needed(true, false), "player-only asked for color");
    check(!doomnet::color_question_needed(false, true), "skin-only asked for color");
    check(doomnet::color_question_needed(true, true), "both did not ask for color");
    check(wcscmp(doomnet::theme_for_new_settings(true, false, true, false), L"steam") == 0, "player-only theme");
    check(doomnet::theme_for_new_settings(false, true, true, false) == nullptr, "skin-only wrote a theme");
    check(wcscmp(doomnet::theme_for_new_settings(true, true, true, false), L"red") == 0, "match skin");
    check(wcscmp(doomnet::theme_for_new_settings(true, true, false, false), L"steam") == 0, "both defaulted away from steam blue");
    check(doomnet::theme_for_new_settings(true, true, true, true) == nullptr, "existing settings overwritten");
}

void test_log_threads() {
    const std::wstring dir = temp_dir();
    SetEnvironmentVariableW(L"DOOMNET_ROOT", dir.c_str());
    constexpr int kThreads = 8;
    constexpr int kEach = 200;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t]() {
            for (int i = 0; i < kEach; ++i) {
                wchar_t message[64] = {};
                swprintf_s(message, L"thread-%d line-%d\nhidden", t, i);
                if (!doomnet::log_line(L"info", message)) {
                    std::fprintf(stderr, "FAIL  log write\n");
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    const std::wstring log_path = dir + L"\\logs\\doomnet.log";
    HANDLE handle = CreateFileW(log_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(handle != INVALID_HANDLE_VALUE, "log file missing");
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(handle, &size);
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(handle);
    int lines = 0;
    size_t start = 0;
    bool torn = false;
    while (start < bytes.size()) {
        const size_t end = bytes.find('\n', start);
        if (end == std::string::npos) {
            torn = true;
            break;
        }
        const std::string line = bytes.substr(start, end - start);
        if (line.find("hidden") == std::string::npos || line.find('\r') != line.size() - 1) {
            torn = true;
        }
        if (line.find("thread-") == std::string::npos) {
            torn = true;
        }
        ++lines;
        start = end + 1;
    }
    check(!torn, "a log line was torn or a newline survived inside a message");
    check(lines == kThreads * kEach, "log line count");
}

void test_steam() {
    check(!doomnet::directory_has_steam_exe(temp_dir()), "empty directory looked like Steam");
    check(!doomnet::directory_has_steam_exe(L"C:\\Steam\\..\\Windows"), "dotdot steam path accepted");
    std::wstring found;
    check(doomnet::discover_steam_path(found), "Steam was not found in the registry or the default directory");
    check(doomnet::directory_has_steam_exe(found), "discovered path has no steam.exe");
}

void test_wide_io() {
    const std::wstring dir = temp_dir();
    const std::wstring path = dir + L"\\音楽-café.txt";
    const char bytes[] = "abc-unicode";
    std::wstring error;
    check(doomnet::write_new_file(path, bytes, sizeof(bytes) - 1, error), "unicode write");
    std::string read;
    check(doomnet::read_entire_file(path, read, error) && read == "abc-unicode", "unicode read");
    check(doomnet::wide_file_exists(path), "unicode exists");
    check(!doomnet::write_new_file(dir + L"\\..\\escape.txt", bytes, 3, error), "dotdot write accepted");
    check(error.find(L"..") != std::wstring::npos, "dotdot write error is vague");
}

void test_launch_and_shortcut() {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring folder = self;
    folder.resize(folder.find_last_of(L"\\/"));
    const std::wstring child = folder + L"\\DoomNetDummyChild.exe";
    const std::wstring dir = temp_dir();
    const std::wstring marker = dir + L"\\marker.txt";
    SetEnvironmentVariableW(L"DOOMNET_CHILD_MARKER", marker.c_str());
    DWORD code = 1;
    std::wstring error;
    check(doomnet::launch_hidden(child, 2000, code, error), "hidden child failed");
    std::string body;
    check(doomnet::read_entire_file(marker, body, error) && body == "hidden-ok", "hidden child marker");

    std::wstring steam;
    if (doomnet::discover_steam_path(steam)) {
        check(!doomnet::launch_hidden(steam + L"\\steam.exe", 2000, code, error), "steam.exe was allowed to start");
        check(error.find(L"steam.exe") != std::wstring::npos, "steam.exe refusal is vague");
    }

    const std::wstring icon = L"C:\\Windows\\System32\\notepad.exe";
    const std::wstring lnk = dir + L"\\Player.lnk";
    check(doomnet::create_shortcut(lnk, icon, icon, error), "create shortcut");
    check(doomnet::retarget_shortcut(lnk, child, icon, error), "retarget shortcut");
    std::wstring target;
    std::wstring icon_read;
    check(doomnet::read_shortcut(lnk, target, icon_read, error), "read shortcut");
    check(target == child, "shortcut target was not the dummy");
    check(icon_read == icon, "shortcut icon did not stay on notepad");
    check(!doomnet::retarget_shortcut(lnk, steam.empty() ? L"C:\\Steam\\steam.exe" : steam + L"\\steam.exe", icon, error), "shortcut to steam.exe was allowed");
}

void test_install_window_and_payload() {
    doomnet::InstallChoices choices;
    HWND window = doomnet::create_install_window(&choices);
    check(window != nullptr, "setup window was not created");
    if (!window) {
        return;
    }
    check(IsWindowVisible(window) == FALSE, "setup window was shown on screen");
    check(IsWindowEnabled(GetDlgItem(window, doomnet::kInstallIdMatch)) == FALSE, "color question was enabled before both pieces were selected");
    SendMessageW(GetDlgItem(window, doomnet::kInstallIdPlayer), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(doomnet::kInstallIdPlayer, BN_CLICKED), 0);
    check(IsWindowEnabled(GetDlgItem(window, doomnet::kInstallIdMatch)) == FALSE, "color question enabled for player only");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(doomnet::kInstallIdInstall, BN_CLICKED), 0);
    check(choices.accepted && choices.player && !choices.skin && !choices.match_skin, "player-only setup choices");
    check(IsWindow(window) == FALSE, "setup window stayed open");

    const std::wstring source = temp_dir();
    const char plugin[] = "{\"name\":\"SteamMusicPlayer\"}\n";
    const char backend[] = "backend-marker\n";
    const char skin[] = "{\"UseDefaultPatches\":true}\n";
    std::wstring error;
    CreateDirectoryW((source + L"\\backend").c_str(), nullptr);
    CreateDirectoryW((source + L"\\skin").c_str(), nullptr);
    check(doomnet::write_new_file(source + L"\\plugin.json", plugin, sizeof(plugin) - 1, error), "fixture plugin");
    check(doomnet::write_new_file(source + L"\\backend\\main.lua", backend, sizeof(backend) - 1, error), "fixture backend");
    check(doomnet::write_new_file(source + L"\\skin\\skin.json", skin, sizeof(skin) - 1, error), "fixture skin");

    const std::wstring root = temp_dir();
    doomnet::InstallRequest request;
    request.player = true;
    request.skin = true;
    request.match_skin = true;
    request.player_source = source;
    request.skin_source = source + L"\\skin";
    request.steam_path = L"C:\\Program Files (x86)\\Steam";
    const doomnet::InstallResult first = doomnet::apply_install(root, request);
    check(first.ok, "fixture install");
    std::string settings;
    check(doomnet::read_entire_file(root + L"\\player\\backend\\data\\settings.json", settings, error) && settings.find("red") != std::string::npos, "new settings color");
    std::string copied;
    check(doomnet::read_entire_file(root + L"\\player\\plugin.json", copied, error) && copied.find("SteamMusicPlayer") != std::string::npos, "copied plugin.json");
    check(doomnet::wide_file_exists(root + L"\\skin\\skin.json"), "copied skin.json");

    const std::string original = settings;
    request.match_skin = false;
    const doomnet::InstallResult second = doomnet::apply_install(root, request);
    check(second.ok, "second install");
    std::string after;
    check(doomnet::read_entire_file(root + L"\\player\\backend\\data\\settings.json", after, error) && after == original, "existing settings.json was overwritten");

    std::wstring steam;
    if (doomnet::discover_steam_path(steam)) {
        const doomnet::InstallResult refused = doomnet::apply_install(steam, request);
        check(!refused.ok, "install into the Steam directory was allowed");
        check(!doomnet::wide_file_exists(steam + L"\\doomnet-foundation-must-not-exist.txt"), "a sentinel appeared in the Steam directory");
    }

    const std::wstring player_repo = L"D:\\Documents\\Repos\\SteamTunes";
    const std::wstring skin_repo = L"D:\\Documents\\Repos\\SteamSkin";
    if (doomnet::wide_file_exists(player_repo + L"\\plugin.json") && doomnet::wide_file_exists(skin_repo + L"\\skin.json")) {
        const std::wstring real_root = temp_dir();
        doomnet::InstallRequest real;
        real.player = true;
        real.skin = false;
        real.player_source = player_repo;
        real.steam_path = steam;
        const doomnet::InstallResult copied_player = doomnet::apply_install(real_root, real);
        check(copied_player.ok && copied_player.files_copied > 10, "real player payload copy");
        std::string real_plugin;
        check(doomnet::read_entire_file(real_root + L"\\player\\plugin.json", real_plugin, error) && real_plugin.find("SteamMusicPlayer") != std::string::npos, "real plugin.json");
        check(doomnet::wide_file_exists(real_root + L"\\player\\backend\\main.lua"), "real main.lua");
        check(!doomnet::wide_file_exists(real_root + L"\\skin\\skin.json"), "skin was copied for a player-only install");
    }
}

void test_uninstall() {
    const std::wstring source = temp_dir();
    CreateDirectoryW((source + L"\\backend").c_str(), nullptr);
    CreateDirectoryW((source + L"\\skin").c_str(), nullptr);
    std::wstring error;
    const char plugin[] = "{\"name\":\"SteamMusicPlayer\"}\n";
    const char backend[] = "backend-marker\n";
    const char skin[] = "{\"name\":\"Steam-Skin\"}\n";
    check(doomnet::write_new_file(source + L"\\plugin.json", plugin, sizeof(plugin) - 1, error), "uninstall fixture plugin");
    check(doomnet::write_new_file(source + L"\\backend\\main.lua", backend, sizeof(backend) - 1, error), "uninstall fixture backend");
    check(doomnet::write_new_file(source + L"\\skin\\skin.json", skin, sizeof(skin) - 1, error), "uninstall fixture skin");

    const std::wstring root = temp_dir();
    doomnet::InstallRequest request;
    request.player = true;
    request.skin = false;
    request.player_source = source;
    request.skin_source = source + L"\\skin";
    request.steam_path = L"C:\\Program Files (x86)\\Steam";
    check(doomnet::apply_install(root, request).ok, "player-only install before uninstall");

    const std::wstring steamui = temp_dir() + L"\\steamui";
    CreateDirectoryW(steamui.c_str(), nullptr);
    CreateDirectoryW((steamui + L"\\steam-music-player").c_str(), nullptr);
    CreateDirectoryW((steamui + L"\\doomnet-skin").c_str(), nullptr);
    check(doomnet::write_new_file(steamui + L"\\library.js", "keep\n", 5, error), "steamui neighbor");
    check(doomnet::write_new_file(steamui + L"\\steam-music-player\\bootstrap.js", "player\n", 7, error), "steamui player");
    const std::wstring shortcut = temp_dir() + L"\\DoomNet.lnk";
    check(doomnet::create_shortcut(shortcut, source + L"\\plugin.json", L"C:\\Windows\\System32\\notepad.exe", error), "uninstall shortcut");

    doomnet::UninstallRequest uninstall;
    uninstall.root = root;
    uninstall.steamui = steamui;
    uninstall.shortcut = shortcut;
    const doomnet::UninstallResult removed = doomnet::apply_uninstall(uninstall);
    check(removed.ok && removed.removed_player && removed.skipped_skin, "player-only uninstall");
    check(GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES, "DoomNet folder remained");
    check(GetFileAttributesW((steamui + L"\\steam-music-player").c_str()) == INVALID_FILE_ATTRIBUTES, "player steamui files remained");
    check(GetFileAttributesW(shortcut.c_str()) == INVALID_FILE_ATTRIBUTES, "shortcut remained");
    check(doomnet::wide_file_exists(steamui + L"\\library.js"), "steamui neighbor was deleted");

    const doomnet::UninstallResult missing_shortcut = doomnet::apply_uninstall(uninstall);
    check(missing_shortcut.ok && missing_shortcut.skipped_player && missing_shortcut.skipped_skin, "second uninstall did not skip cleanly");

    doomnet::UninstallRequest source_repo;
    source_repo.root = L"D:\\Documents\\Repos\\SteamTunes";
    const doomnet::UninstallResult refused_source = doomnet::apply_uninstall(source_repo);
    check(!refused_source.ok, "SteamTunes source was eligible for delete");
    check(doomnet::wide_file_exists(L"D:\\Documents\\Repos\\SteamTunes\\plugin.json"), "SteamTunes plugin.json was deleted");

    std::wstring steam;
    if (doomnet::discover_steam_path(steam)) {
        doomnet::UninstallRequest steam_root;
        steam_root.root = steam;
        check(!doomnet::apply_uninstall(steam_root).ok, "Steam install was eligible for delete");
        check(doomnet::wide_file_exists(steam + L"\\steam.exe"), "steam.exe was deleted");
    }
}

void make_fake_steam(const std::wstring& dir, bool with_hooks) {
    std::wstring error;
    const char steam_bytes[] = "MZ fake steam";
    doomnet::write_new_file(dir + L"\\steam.exe", steam_bytes, sizeof(steam_bytes) - 1, error);
    // steamclient64.dll is a normal Steam DLL the toggle must never move.
    const char other[] = "not a hook";
    doomnet::write_new_file(dir + L"\\steamclient64.dll", other, sizeof(other) - 1, error);
    if (!with_hooks) {
        return;
    }
    // wsock32.dll carries Millennium's __get_shim_version marker so it reads as Millennium's.
    const char shim[] = "MZ...exports...__get_shim_version...forwards to ws2_32";
    doomnet::write_new_file(dir + L"\\wsock32.dll", shim, sizeof(shim) - 1, error);
    const char plain[] = "MZ retired hook";
    doomnet::write_new_file(dir + L"\\version.dll", plain, sizeof(plain) - 1, error);
    doomnet::write_new_file(dir + L"\\millennium-legacy.version.dll", plain, sizeof(plain) - 1, error);
    doomnet::write_new_file(dir + L"\\user32.dll", plain, sizeof(plain) - 1, error);
}

void test_toggle() {
    // A full off/on cycle against a fake Steam directory. Nothing here touches a
    // real Steam install; require_steam_closed is false so the running-Steam guard
    // does not gate the file logic.
    const std::wstring steam = temp_dir();
    make_fake_steam(steam, true);
    const std::wstring stash = steam + L"\\doomnet-stash";
    const std::wstring marker = stash + L"\\doomnet-stash.txt";

    check(doomnet::millennium_state(steam) == doomnet::MillenniumState::Active, "fresh fake Steam with hooks is not Active");
    check(doomnet::is_millennium_bootstrap(steam + L"\\wsock32.dll"), "Millennium wsock32.dll was not recognized by its shim marker");
    check(!doomnet::is_millennium_bootstrap(steam + L"\\user32.dll"), "user32.dll was wrongly taken for Millennium's bootstrap");
    check(!doomnet::is_millennium_bootstrap(steam + L"\\does-not-exist.dll"), "a missing file was taken for Millennium's bootstrap");

    doomnet::ToggleResult off = doomnet::millennium_off(steam, false);
    check(off.ok && off.changed, "millennium_off did not report success");
    check(off.moved.size() == 4, "millennium_off did not move all four hook files");
    check(off.state == doomnet::MillenniumState::Stashed, "state after off was not Stashed");
    for (const wchar_t* name : {L"wsock32.dll", L"version.dll", L"millennium-legacy.version.dll", L"user32.dll"}) {
        check(!doomnet::wide_file_exists(steam + L"\\" + name), "a hook file was left in the Steam root after off");
        check(doomnet::wide_file_exists(stash + L"\\" + name), "a hook file did not land in the stash after off");
    }
    check(doomnet::wide_file_exists(steam + L"\\steam.exe"), "off moved steam.exe");
    check(doomnet::wide_file_exists(steam + L"\\steamclient64.dll"), "off moved an unrelated Steam DLL");
    check(doomnet::wide_file_exists(marker), "off did not write the stash marker");

    check(doomnet::millennium_state(steam) == doomnet::MillenniumState::Stashed, "state read as not Stashed after off");
    doomnet::ToggleResult off_again = doomnet::millennium_off(steam, false);
    check(off_again.ok && !off_again.changed, "a second off moved files again");

    doomnet::ToggleResult on = doomnet::millennium_on(steam, false);
    check(on.ok && on.changed && on.moved.size() == 4, "millennium_on did not restore all four files");
    check(on.state == doomnet::MillenniumState::Active, "state after on was not Active");
    for (const wchar_t* name : {L"wsock32.dll", L"version.dll", L"millennium-legacy.version.dll", L"user32.dll"}) {
        check(doomnet::wide_file_exists(steam + L"\\" + name), "a hook file was not restored to the Steam root");
    }
    check(!doomnet::wide_file_exists(marker), "the stash marker was left behind after on");
    check(GetFileAttributesW(stash.c_str()) == INVALID_FILE_ATTRIBUTES, "the empty stash folder was left behind after on");

    doomnet::ToggleResult on_again = doomnet::millennium_on(steam, false);
    check(on_again.ok && !on_again.changed, "a second on restored files that were not stashed");

    // A directory whose only wsock32.dll is not Millennium's (no shim marker) is
    // not Active, so DoomNet's own future bootstrap is not mistaken for Millennium.
    const std::wstring doomnet_dir = temp_dir();
    make_fake_steam(doomnet_dir, false);
    std::wstring err;
    const char plain[] = "MZ doomnet bootstrap, forwards ws2_32, no shim marker";
    doomnet::write_new_file(doomnet_dir + L"\\wsock32.dll", plain, sizeof(plain) - 1, err);
    check(doomnet::millennium_state(doomnet_dir) == doomnet::MillenniumState::NotPresent, "DoomNet's own wsock32.dll was mistaken for Millennium");
    doomnet::ToggleResult off_doomnet = doomnet::millennium_off(doomnet_dir, false);
    check(off_doomnet.ok && !off_doomnet.changed, "off moved DoomNet's own wsock32.dll");
    check(doomnet::wide_file_exists(doomnet_dir + L"\\wsock32.dll"), "off deleted DoomNet's own wsock32.dll");

    // A clean install with just one Millennium file present moves exactly that one.
    const std::wstring one = temp_dir();
    make_fake_steam(one, false);
    const char shim[] = "MZ __get_shim_version";
    doomnet::write_new_file(one + L"\\wsock32.dll", shim, sizeof(shim) - 1, err);
    doomnet::ToggleResult off_one = doomnet::millennium_off(one, false);
    check(off_one.ok && off_one.changed && off_one.moved.size() == 1, "off did not move exactly the one present hook file");

    // Guards.
    check(!doomnet::millennium_off(L"", false).ok, "off accepted an empty Steam path");
    check(!doomnet::millennium_off(L"C:\\Steam\\..\\Windows", false).ok, "off accepted a path with ..");
    const std::wstring no_steam = temp_dir();
    check(!doomnet::millennium_off(no_steam, false).ok, "off accepted a folder with no steam.exe");
    check(doomnet::millennium_state(no_steam) == doomnet::MillenniumState::Unknown, "a folder with no steam.exe was not Unknown");

    // The running-Steam guard, only asserted when Steam is actually up right now.
    std::wstring which;
    if (doomnet::steam_is_running(which)) {
        const std::wstring guarded = temp_dir();
        make_fake_steam(guarded, true);
        doomnet::ToggleResult refused = doomnet::millennium_off(guarded, true);
        check(!refused.ok && !refused.changed, "off did not refuse while Steam was running");
        check(doomnet::wide_file_exists(guarded + L"\\wsock32.dll"), "off moved files while Steam was running");
    }
}

void test_place_bootstrap_and_pipe() {
    const std::wstring source_dir = temp_dir();
    std::wstring error;
    const char dll_bytes[] = "MZ DoomNetWsock32 bootstrap";
    doomnet::write_new_file(source_dir + L"\\wsock32.dll", dll_bytes, sizeof(dll_bytes) - 1, error);

    const std::wstring active = temp_dir();
    make_fake_steam(active, true);
    doomnet::PlaceBootstrapResult refused = doomnet::place_doomnet_bootstrap(active, source_dir + L"\\wsock32.dll", false);
    check(!refused.ok, "bootstrap was copied while Millennium was active");
    check(doomnet::is_millennium_bootstrap(active + L"\\wsock32.dll"), "Millennium's wsock32.dll was overwritten");

    const std::wstring clear = temp_dir();
    make_fake_steam(clear, false);
    doomnet::PlaceBootstrapResult placed = doomnet::place_doomnet_bootstrap(clear, source_dir + L"\\wsock32.dll", false);
    check(placed.ok && !placed.replaced, "bootstrap was not copied into a clean Steam folder");
    check(doomnet::wide_file_exists(clear + L"\\wsock32.dll"), "copied bootstrap is missing");
    std::string copied;
    doomnet::read_entire_file(clear + L"\\wsock32.dll", copied, error);
    check(copied.find("DoomNetWsock32") != std::string::npos, "copied bootstrap lost its marker");

    doomnet::PlaceBootstrapResult again = doomnet::place_doomnet_bootstrap(clear, source_dir + L"\\wsock32.dll", false);
    check(again.ok && again.replaced, "replacing DoomNet's own bootstrap was refused");

    const char stranger[] = "MZ some other dll";
    doomnet::write_new_file(clear + L"\\wsock32.dll", stranger, sizeof(stranger) - 1, error);
    doomnet::PlaceBootstrapResult unknown = doomnet::place_doomnet_bootstrap(clear, source_dir + L"\\wsock32.dll", false);
    check(!unknown.ok, "an unrecognized wsock32.dll was replaced");
    std::string still;
    doomnet::read_entire_file(clear + L"\\wsock32.dll", still, error);
    check(still.find("some other dll") != std::string::npos, "unrecognized wsock32.dll was overwritten");

    check(!doomnet::place_doomnet_bootstrap(L"", source_dir + L"\\wsock32.dll", false).ok, "empty Steam path was accepted");
    check(!doomnet::place_doomnet_bootstrap(clear + L"\\..\\nope", source_dir + L"\\wsock32.dll", false).ok, "a .. path was accepted");
    const std::wstring no_steam = temp_dir();
    check(!doomnet::place_doomnet_bootstrap(no_steam, source_dir + L"\\wsock32.dll", false).ok, "a folder with no steam.exe was accepted");

    const char not_ours[] = "MZ not the bootstrap";
    doomnet::write_new_file(source_dir + L"\\other.dll", not_ours, sizeof(not_ours) - 1, error);
    const std::wstring fresh = temp_dir();
    make_fake_steam(fresh, false);
    check(!doomnet::place_doomnet_bootstrap(fresh, source_dir + L"\\other.dll", false).ok, "a DLL without the DoomNet marker was copied");

    wchar_t self_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self_path, MAX_PATH);
    std::wstring self_dir = self_path;
    const size_t slash = self_dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        self_dir.resize(slash);
    }
    const std::wstring built = self_dir + L"\\wsock32.dll";
    check(doomnet::wide_file_exists(built), "the built wsock32.dll is not beside the test");
    std::string built_bytes;
    check(doomnet::read_entire_file(built, built_bytes, error) && built_bytes.find("DoomNetWsock32") != std::string::npos, "the built wsock32.dll does not contain DoomNetWsock32");
    check(!doomnet::is_millennium_bootstrap(built), "the built wsock32.dll was mistaken for Millennium");

    // NUL-delimited DevTools message over a named pipe, the same shape the
    // relay will carry once Steam is started.
    const std::wstring pipe_name = L"\\\\.\\pipe\\DoomNet.test." + std::to_wstring(GetCurrentProcessId());
    HANDLE server = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
    check(server != INVALID_HANDLE_VALUE, "test pipe was not created");
    std::thread peer([server] {
        ConnectNamedPipe(server, nullptr);
        std::string request;
        char byte = 0;
        DWORD read = 0;
        while (ReadFile(server, &byte, 1, &read, nullptr) && read == 1 && byte != '\0') {
            request.push_back(byte);
        }
        const char* reply = request.find("setDiscoverTargets") == std::string::npos ? "{\"id\":1,\"error\":{\"message\":\"unexpected\"}}\0" : "{\"id\":1,\"result\":{\"ok\":true}}\0";
        DWORD written = 0;
        WriteFile(server, reply, static_cast<DWORD>(strlen(reply) + 1), &written, nullptr);
    });
    doomnet::CdpPipe pipe;
    std::wstring pipe_error;
    const bool connected = pipe.connect_name(pipe_name, pipe_error);
    check(connected, "cdp pipe did not connect");
    std::string result;
    if (connected) {
        check(pipe.call("Target.setDiscoverTargets", "{\"discover\":true}", result, pipe_error), "discover command failed");
        check(result.find("\"ok\":true") != std::string::npos, "discover reply was not ok");
    }
    pipe.close();
    if (peer.joinable()) {
        peer.join();
    }
    if (server != INVALID_HANDLE_VALUE) {
        CloseHandle(server);
    }
}

void copy_beside(const std::wstring& from, const std::wstring& to) {
    CopyFileW(from.c_str(), to.c_str(), FALSE);
}

void test_launch_inject() {
    wchar_t self_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self_path, MAX_PATH);
    std::wstring bin = self_path;
    const size_t slash = bin.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        bin.resize(slash);
    }
    const std::wstring root = temp_dir();
    copy_beside(bin + L"\\wsock32.dll", root + L"\\wsock32.dll");
    copy_beside(bin + L"\\DoomNetFakeSteam.exe", root + L"\\steam.exe");
    copy_beside(bin + L"\\DoomNetFakeWebHelper.exe", root + L"\\steamwebhelper.exe");
    copy_beside(bin + L"\\steamui.dll", root + L"\\steamui.dll");

    wchar_t saved_root[32768] = {};
    const DWORD saved_n = GetEnvironmentVariableW(L"DOOMNET_ROOT", saved_root, 32768);
    wchar_t saved_script[32768] = {};
    const DWORD saved_script_n = GetEnvironmentVariableW(L"DOOMNET_SCRIPT_OUT", saved_script, 32768);
    SetEnvironmentVariableW(L"DOOMNET_ROOT", root.c_str());
    SetEnvironmentVariableW(L"DOOMNET_SCRIPT_OUT", (root + L"\\script.txt").c_str());

    doomnet::InjectResult injected = doomnet::launch_and_inject_boot(root + L"\\steam.exe", L"cdp", false);
    check(injected.ok, "fake client did not receive the boot script");
    if (!injected.ok) {
        std::fprintf(stderr, "      %ls\n", injected.detail.c_str());
    }
    if (injected.process != nullptr) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (Process32FirstW(snapshot, &entry)) {
                do {
                    if (entry.th32ParentProcessID == injected.pid) {
                        HANDLE child = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
                        if (child != nullptr) {
                            TerminateProcess(child, 0);
                            CloseHandle(child);
                        }
                    }
                } while (Process32NextW(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }
        TerminateProcess(static_cast<HANDLE>(injected.process), 0);
        WaitForSingleObject(static_cast<HANDLE>(injected.process), 5000);
        CloseHandle(static_cast<HANDLE>(injected.process));
    }
    std::wstring error;
    std::string script;
    check(doomnet::read_entire_file(root + L"\\script.txt", script, error) && script.find("callServerMethod") != std::string::npos, "the injected script was not the boot script");

    std::wstring installed;
    if (doomnet::discover_steam_path(installed)) {
        doomnet::InjectResult live = doomnet::launch_and_inject_boot(installed + L"\\steam.exe", L"", false);
        check(!live.ok, "the installed Steam client was eligible to start");
        if (live.process != nullptr) {
            TerminateProcess(static_cast<HANDLE>(live.process), 1);
            CloseHandle(static_cast<HANDLE>(live.process));
        }
    }

    if (saved_n > 0 && saved_n < 32768) {
        SetEnvironmentVariableW(L"DOOMNET_ROOT", saved_root);
    } else {
        SetEnvironmentVariableW(L"DOOMNET_ROOT", nullptr);
    }
    if (saved_script_n > 0 && saved_script_n < 32768) {
        SetEnvironmentVariableW(L"DOOMNET_SCRIPT_OUT", saved_script);
    } else {
        SetEnvironmentVariableW(L"DOOMNET_SCRIPT_OUT", nullptr);
    }
}

void test_instance() {
    check(doomnet::acquire_instance(), "first instance was rejected");
    check(doomnet::acquire_instance(), "owner lost the mutex on a second call in the same process");

    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring folder = self;
    const size_t slash = folder.find_last_of(L"\\/");
    folder.resize(slash);
    const std::wstring exe = folder + L"\\DoomNet.exe";
    const std::wstring root = temp_dir();
    SetEnvironmentVariableW(L"DOOMNET_ROOT", root.c_str());
    std::wstring command = L"\"" + exe + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    check(created == TRUE, "could not start DoomNet.exe for the duplicate-instance check");
    if (created) {
        const DWORD waited = WaitForSingleObject(process.hProcess, 2000);
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        check(waited == WAIT_OBJECT_0 && code == 0, "duplicate DoomNet.exe did not exit 0");
        const std::wstring log_path = root + L"\\logs\\doomnet.log";
        HANDLE handle = CreateFileW(log_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(handle != INVALID_HANDLE_VALUE, "duplicate process wrote no log");
        if (handle != INVALID_HANDLE_VALUE) {
            char bytes[4096] = {};
            DWORD read = 0;
            ReadFile(handle, bytes, sizeof(bytes) - 1, &read, nullptr);
            CloseHandle(handle);
            const std::string text(bytes, read);
            check(text.find("already held") != std::string::npos, "duplicate process did not explain that Local\\DoomNet is already held");
            check(text.find("exiting without starting Steam") != std::string::npos, "duplicate process did not say it would not start Steam");
        }
    }
}

void test_skin_page() {
    const std::wstring repo = L"D:\\Documents\\Repos\\SteamSkin\\skin.json";
    doomnet::SkinPreamble loaded = doomnet::load_skin_preamble(repo, false);
    check(loaded.ok && loaded.patches == 28, "Steam-Skin skin.json did not prepare 28 patches");
    check(loaded.javascript.find("music-player-inject.js") == std::string::npos, "music-player-inject.js was prepared for the page");
    check(loaded.javascript.find("friends.custom") == std::string::npos, "UseDefaultPatches added a friends stylesheet");
    check(loaded.javascript.find("https://steamloopback.host/doomnet-skin/libraryroot.custom.css") != std::string::npos, "library stylesheet URL is missing");
    check(loaded.javascript.find("\"player\":false") != std::string::npos, "skin-only preamble turned the player on");
    check(doomnet::skin_tags_ready(), "skin tags were not kept for store documents");
    const std::string store_tags = doomnet::skin_tags_for_document("https://store.steampowered.com/");
    check(store_tags.find("steam-store-chrome-bg") != std::string::npos, "the store document did not inline the store chrome colors");
    check(doomnet::skin_style_script("https://store.steampowered.com/").find("steam-store-chrome-bg") != std::string::npos, "the store style script is missing the chrome colors");
    check(store_tags.find("<style") != std::string::npos, "the store colors were not inlined into the document");
    check(store_tags.find("libraryroot.custom.css") == std::string::npos, "the store document got the library sheet");
    check(store_tags.find("music-player-inject.js") == std::string::npos, "the store document got music-player-inject.js");
    check(doomnet::skin_tags_for_document("https://checkout.steampowered.com/checkout/").empty(), "checkout received skin tags");
    const std::string html = "<html><head><title>Store</title></head><body></body></html>";
    const std::string skinned = doomnet::inject_skin_into_html(html, "<link rel=\"stylesheet\" href=\"https://steamloopback.host/doomnet-skin/webkit.css\">");
    check(skinned.find("<head><link rel=\"stylesheet\" href=\"https://steamloopback.host/doomnet-skin/webkit.css\">") != std::string::npos, "the skin link was not inserted after head");
    doomnet::SkinPreamble with_player = doomnet::load_skin_preamble(repo, true);
    check(with_player.ok && with_player.javascript.find("\"player\":true") != std::string::npos, "player preamble did not enable the player");
    check(!doomnet::load_skin_preamble(L"C:\\skin\\..\\skin.json", false).ok, "a .. skin.json path was accepted");

    std::wstring error;
    const std::wstring steam = temp_dir();
    const std::wstring skin = temp_dir();
    check(doomnet::write_new_file(steam + L"\\steam.exe", "x", 1, error), "fixture steam.exe");
    check(doomnet::write_new_file(skin + L"\\libraryroot.custom.css", "body{}\n", 7, error), "fixture css");
    check(doomnet::write_new_file(skin + L"\\notes.txt", "no\n", 3, error), "fixture note");
    const char json[] = "{\"UseDefaultPatches\":true,\"Patches\":[{\"MatchRegexString\":\"^Steam$\",\"TargetCss\":\"libraryroot.custom.css\"},{\"MatchRegexString\":\"^Steam$\",\"TargetJs\":\"music-player-inject.js\"}]}";
    check(doomnet::write_new_file(skin + L"\\skin.json", json, sizeof(json) - 1, error), "fixture skin.json");
    doomnet::SkinPreamble fixture = doomnet::load_skin_preamble(skin + L"\\skin.json", false);
    check(fixture.ok && fixture.patches == 1, "fixture skin.json did not keep the one css patch");
    check(fixture.javascript.find("music-player-inject.js") == std::string::npos, "fixture preamble named music-player-inject.js");
    doomnet::SkinFileCopy copied = doomnet::copy_skin_files(skin, steam);
    check(copied.ok && copied.copied == 1, "skin copy did not take the one css file");
    check(doomnet::wide_file_exists(steam + L"\\steamui\\doomnet-skin\\libraryroot.custom.css"), "libraryroot.custom.css was not copied");
    check(!doomnet::wide_file_exists(steam + L"\\steamui\\doomnet-skin\\notes.txt"), "a non-css file was copied into steamui");
    check(!doomnet::copy_skin_files(skin, steam + L"\\..\\Windows").ok, "skin copy accepted a .. path");
    check(!doomnet::copy_skin_files(skin, temp_dir()).ok, "skin copy accepted a folder with no steam.exe");

    const std::string boot = doomnet::boot_script_text();
    check(boot.find("Steam Games List") != std::string::npos, "boot script does not treat Steam Games List like Steam");
    check(boot.find("location.href") != std::string::npos, "boot script does not match the page URL");
    check(boot.find("music-player-inject.js") != std::string::npos, "boot script no longer skips music-player-inject.js");
    check(boot.find("g_PopupManager") != std::string::npos, "boot script does not look at Steam popups");
    check(boot.find("HAS_INJECTED_THEME") != std::string::npos, "boot script no longer patches each popup once");
    check(boot.find("__doomnetQuiet") != std::string::npos, "boot script no longer defers skin document observers");
    check(boot.find("__doomnetLibraryReady") != std::string::npos, "boot script no longer preloads the library grid");
    check(boot.find("stableSince") != std::string::npos, "boot script no longer waits for the library grid to finish its first pass");
    check(boot.find("wantsPlayer") != std::string::npos, "boot script no longer inserts the player on the Steam window");
    check(boot.find("doomnet-steamtunes-tab") != std::string::npos, "boot script does not add a SteamTunes tab");
    check(boot.find("row.appendChild(tab)") != std::string::npos, "the SteamTunes tab is not placed after the account name");
    check(boot.find("SetVisible") != std::string::npos, "opening SteamTunes does not hide the store page underneath");
    check(boot.find("#doomnet-steamtunes-host *{-webkit-app-region:no-drag") != std::string::npos, "Steam's window drag regions can still swallow clicks on the SteamTunes transport");
    check(boot.find("findNavLabel") != std::string::npos && boot.find("\"STORE\"") != std::string::npos && boot.find("\"LIBRARY\"") != std::string::npos && boot.find("\"COMMUNITY\"") != std::string::npos, "the SteamTunes tab is not matched by the Store, Library, and Community labels");
    check(boot.find("OverlayBrowser_Browser") != std::string::npos && boot.find("SP Overlay:") != std::string::npos, "the overlay no longer gets the compact player");
    check(boot.find("__doomnetPlayerOnly") != std::string::npos, "boot script no longer has a store-only player path");
    check(boot.find("window !== window.top && !window.__doomnetPlayerOnly") != std::string::npos, "a store frame still returns before the player binding");
    const std::string store_player = doomnet::store_player_script();
    check(store_player.find("smp-plugin-ui") != std::string::npos, "the store player script is missing the player UI");
    const std::string patched_ui = doomnet::patched_player_ui_text();
    check(patched_ui.find("get_album_art_b64") != std::string::npos, "the player UI was not given the cover file fallback");
    check(patched_ui.find("url.indexOf(\"data:\") !== 0 && !artUrlMatchesAlbum(album, url)") != std::string::npos, "a cover already read as bytes is not painted on every tile");
    check(patched_ui.find("incomingRev <= (UI.pointerBusyRev || 0)") != std::string::npos && patched_ui.find("UI.pointerRevFloor") != std::string::npos, "a late page reply can still overwrite the page that was just opened");
    check(patched_ui.find("albumArtist: group.artist") != std::string::npos, "Album and Genre covers are not looked up by the album artist the way Artist covers are");
    check(patched_ui.find("doomnetMountArtSize") != std::string::npos && patched_ui.find("count: 12, size: \"smd\"") != std::string::npos && patched_ui.find("count: 8, size: \"mdl\"") != std::string::npos && patched_ui.find("UI.doomnetArtCols || 14") != std::string::npos && doomnet::patched_player_css_text().find(".smp-art-size-sm{width:10px;height:10px;}") != std::string::npos && doomnet::patched_player_css_text().find(".smp-art-size-smd{width:12px;height:12px;}") != std::string::npos && doomnet::patched_player_css_text().find(".smp-art-size-md{width:14px;height:14px;}") != std::string::npos && doomnet::patched_player_css_text().find(".smp-art-size-mdl{width:16px;height:16px;}") != std::string::npos && doomnet::patched_player_css_text().find(".smp-art-size-lg{width:18px;height:18px;}") != std::string::npos, "the library artwork control is not five ascending squares");
    check(patched_ui.find("wantCols = 4") != std::string::npos && patched_ui.find("coverTrackId != null && !document.documentElement.classList.contains(\"smp-ctx-overlay\")") != std::string::npos, "the mini player still shows small covers and a Set Cover button");
    check(doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-search{flex:0 1 260px;") != std::string::npos, "the library search box still stretches across the header");
    check(doomnet::patched_player_ui_text().find("step: \"0.01\"") != std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-volume{width:96px;") != std::string::npos, "the time bar and volume slider still move in coarse steps");
    check(doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-nptab-backdrop{display:block;top:0;left:0;right:0;bottom:0;height:auto;") != std::string::npos, "the Now Playing artwork blur still ends on a fixed band");
    check(patched_ui.find("artistText.title") != std::string::npos, "the now-playing artist link still owns the whole row");
    check(doomnet::patched_player_css_text().find("button.smp-album-artist.smp-artist-link{display:inline-block;width:fit-content;") != std::string::npos, "album artist links still fill the card");
    check(patched_ui.find("UI.ensureBrowseCards(\"artist\")") != std::string::npos, "the library cards are not built when the snapshot loads");
    check(patched_ui.find("window.__doomnetTabHost") != std::string::npos && patched_ui.find("smp-tab-page") != std::string::npos, "the main player is not mounted into the SteamTunes tab");
    check(patched_ui.find("smp-launcher") != std::string::npos && patched_ui.find("mountOverlay") != std::string::npos, "the overlay player was removed while adding the SteamTunes tab");
    check(doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-panel.smp-tab-page{position:absolute !important;") != std::string::npos, "the SteamTunes tab panel can still sit as a floating bubble");
    check(doomnet::shared_cover_path("https://steamloopback.host/steam-music-player/art/albums/../secret.jpg").empty(), "cover lookup accepted a path that leaves the cache");
    check(doomnet::shared_cover_path("https://example.com/not-art.jpg").empty(), "cover lookup accepted an address outside the player art folder");
    const std::wstring shared_cover = doomnet::shared_cover_path("https://steamloopback.host/steam-music-player/art/albums/blood_lord_the_bloodstained_keep.jpg?r=3");
    if (!shared_cover.empty()) {
        check(shared_cover.find(L"blood_lord_the_bloodstained_keep.jpg") != std::wstring::npos && GetFileAttributesW(shared_cover.c_str()) != INVALID_FILE_ATTRIBUTES, "the shared cover path did not name the album file");
    }
    check(boot.find("requestIdleCallback(go") == std::string::npos, "boot script still runs skin scripts from an idle callback");
    check(boot.find("frameUrl") != std::string::npos, "boot script does not style a store frame");
    check(doomnet::steam_web_document("iframe", "https://store.steampowered.com/"), "a store iframe was not treated as a web document");
    check(doomnet::steam_store_home("https://store.steampowered.com/"), "the store front was not treated as the store page");
    check(doomnet::steam_store_home("https://store.steampowered.com/?snr=1"), "a store front query was not treated as the store page");
    check(!doomnet::steam_store_home("https://store.steampowered.com/news/app/1"), "a news page was treated as the store page");
    check(!doomnet::steam_store_home("https://steamcommunity.com/"), "a community page was treated as the store page");
    check(doomnet::store_player_script().find("return 'skip'") != std::string::npos, "the inlined player does not refuse news and community pages");
    check(!doomnet::steam_web_document("iframe", "https://checkout.steampowered.com/checkout/"), "checkout was treated as a skin target");
    check(!doomnet::steam_web_document("worker", "https://store.steampowered.com/"), "a worker was treated as a skin target");
    doomnet::set_boot_preamble(fixture.javascript);
    const std::string page = doomnet::boot_script_for_page();
    check(page.find("libraryroot.custom.css") != std::string::npos && page.find("callServerMethod") != std::string::npos, "page script did not carry the skin config and the binding");
    doomnet::set_boot_preamble("");
    check(patched_ui.find("doomnetThumbUrl") != std::string::npos && patched_ui.find("/steam-music-player/art/thumbs/") != std::string::npos, "the grids still load full-size covers instead of thumbnails");
    check(patched_ui.find("doomnetPageWarmDisabled") != std::string::npos && patched_ui.find("doomnetWarmThumbs: function () {\n\t\t\treturn;") != std::string::npos, "the page still prefetches every cover and can hang Steam's web helper");
    check(doomnet::patched_player_css_text().find("@media (min-width:1920px) and (min-height:1000px){") != std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-nptab-hero{flex-direction:row;") != std::string::npos, "the stacked Now Playing layout is used on screens smaller than 1080p");
    check(patched_ui.find("UI.doomnetMountNowPlayingFlanks(wrap)") != std::string::npos && patched_ui.find("App.settings.eqPreset = \"custom\"") != std::string::npos && patched_ui.find("engine.eqFilters") != std::string::npos, "the Now Playing EQ does not show the active preset or switch to Custom");
    check(patched_ui.find("doomnetChooseEqPreset") != std::string::npos && patched_ui.find("doomnetToggleEqMenu") != std::string::npos && patched_ui.find("doomnetCloseEqMenu") != std::string::npos && patched_ui.find("menu.hidden = true") != std::string::npos && doomnet::patched_player_css_text().find(".doomnet-eq-caret{width:0;height:0;border-left:4px solid transparent;border-right:4px solid transparent;border-top:5px solid var(--smp-accent,#66c0f4);}") != std::string::npos, "the Now Playing preset name does not open a list that closes after a choice");
    check(patched_ui.find("engine.masterGain.connect(node)") != std::string::npos && patched_ui.find("doomnet-spectrum") != std::string::npos && patched_ui.find("getByteTimeDomainData") != std::string::npos && patched_ui.find("doomnet-eq-meter") != std::string::npos && patched_ui.find("doomnet-lyrics") != std::string::npos && patched_ui.find("get_track_lyrics") != std::string::npos, "the Now Playing page does not draw a spectrum under the EQ or scrolling lyrics");
    check(patched_ui.find("UI.hexToRgba(accentHex, 0.9)") != std::string::npos && patched_ui.find("rgba(227, 93, 93") == std::string::npos, "the spectrum bars stay red when the appearance accent changes");
    check(patched_ui.find("doomnetLayoutFlanks") != std::string::npos && patched_ui.find("tall ? 100 : 200") != std::string::npos && patched_ui.find("fader - 88") == std::string::npos && patched_ui.find("doomnet-flanks-inline") != std::string::npos && doomnet::patched_player_css_text().find("height:var(--doomnet-fader,120px)") != std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .doomnet-flank-row:not(.doomnet-flanks-inline) .doomnet-eq-meter{position:absolute;left:0;right:0;top:100%;margin-top:12px;}") != std::string::npos, "the Now Playing EQ and meter stay a fixed size and hide on smaller screens");
    check(patched_ui.find("callServer(\"get_music_folders\").then(function (folders)") != std::string::npos && patched_ui.find("UI.applySettingsToEngine();") != std::string::npos, "the overlay waits on the full queue before it shows the saved settings");
    check(patched_ui.find("cacheReady.then(function (cached)") != std::string::npos, "the overlay stays on Loading library until the server answers");
    check(patched_ui.find("text: \"Add to playlist\"") != std::string::npos && patched_ui.find("callServer(\"playlist_add_track\"") != std::string::npos && patched_ui.find("return playlist && !playlist.sourcePath;") != std::string::npos && patched_ui.find("callServer(\"playlist_remove_track\"") != std::string::npos && patched_ui.find("mode === \"playlistTracks\"") != std::string::npos && patched_ui.find("document.body.appendChild(menu)") != std::string::npos && patched_ui.find("rect.top - menuRect.height") != std::string::npos && patched_ui.find("role: \"menuitem\"") != std::string::npos && patched_ui.find("placed.top") != std::string::npos, "a song cannot be added to a playlist you created, and the playlist list does not open as a menu on the button");
    check(patched_ui.find("doomnetMovePlaylistTrack") != std::string::npos && patched_ui.find("title: \"Move Up\"") != std::string::npos && patched_ui.find("title: \"Move Down\"") != std::string::npos && patched_ui.find("doomnetFinishPlaylistDrag") != std::string::npos && patched_ui.find("doomnetKeepPlaylistScroll") != std::string::npos, "a playlist you created cannot be reordered without jumping to the top");
    check(std::string(doomnet::player_compat_lua()).find("entry_path = entry_path or function(dir, entry)") != std::string::npos, "the library scan stops on the first folder, so albums saved without tags are never read");
    check(patched_ui.find("reader.readAsArrayBuffer(file)") != std::string::npos && patched_ui.find("callServer(\"begin_display_art\"") != std::string::npos && patched_ui.find("callServer(\"finish_display_art\"") != std::string::npos && patched_ui.find("App.coverPreview[pinnedAlbum]") != std::string::npos, "Set Cover still depends on a file path and the old picture can paint back over the one you chose");
    {
        const std::string library_scan =
            "\t\telse\n"
            "\t\t\tlocal sizeOk, size = pcall(fs.file_size, path)\n"
            "\t\t\tsize = sizeOk and size or -1\n"
            "\t\t\tif existing.size ~= size then\n"
            "\t\t\t\tscanState.pending[#scanState.pending + 1] = { path = path, size = size }\n"
            "\t\t\tend\n";
        const std::string patched = doomnet::patched_player_module("library", library_scan);
        check(patched.find("album == \"Unknown Album\"") != std::string::npos && patched.find("artist == \"Unknown Artist\"") != std::string::npos, "a file saved without an album or artist is not read again on the next scan");
        check(doomnet::patched_player_module("settings", library_scan) == library_scan, "a module other than the library scanner was rewritten");
        const std::string cover_names =
            "function library.album_art_safe(track)\r\n"
            "\tif not track then\r\n"
            "\t\treturn \"unknown\"\r\n"
            "\tend\r\n"
            "\tlocal artist = track.albumArtist\r\n"
            "\tif not artist or artist == \"\" then\r\n"
            "\t\tartist = track.artist or \"Unknown Artist\"\r\n"
            "\tend\r\n"
            "\tlocal album = track.album or \"Unknown Album\"\r\n"
            "\tlocal label = string.lower(tostring(artist)) .. \"\\t\" .. string.lower(tostring(album))\r\n"
            "\tlocal safe = label:gsub(\"[^%w]+\", \"_\"):gsub(\"^_+\", \"\"):gsub(\"_+$\", \"\")\r\n"
            "\tif safe == \"\" then\r\n"
            "\t\tsafe = \"unknown\"\r\n"
            "\tend\r\n"
            "\tif #safe > 80 then\r\n"
            "\t\tsafe = safe:sub(1, 80)\r\n"
            "\tend\r\n"
            "\treturn safe\r\n"
            "end\r\n"
            "\tlocal cache = art_cache_path(track.id)\r\n"
            "\tif cache and fs.exists(cache) and not artBacklog.cacheQueued[albumSafe] then\r\n"
            "_art_jpeg_worker.ps1\r\n"
            "\tif ok and content and content ~= \"\" then\r\n"
            "\t\tpcall(utils.write_file, dest, content)\r\n"
            "\tend\r\n";
        const std::string covers = doomnet::patched_player_module("library", cover_names);
        check(covers.find("not tostring(album):find(\"[A-Za-z0-9]\")") != std::string::npos && covers.find("if false and cache and fs.exists(cache)") != std::string::npos && covers.find(".musiclibrary") != std::string::npos && covers.find("67108864") != std::string::npos, "albums with no English letters share one cover, or one Apple Music picture can be painted onto unrelated albums");
        const std::string oversized =
            "hasArt = (tags.artData ~= nil and #tags.artData > 0) or folderArt ~= nil,\n"
            "local TAG_SWEEP_VERSION = 3\n";
        const std::string art_scan = doomnet::patched_player_module("library", oversized);
        check(art_scan.find("tags.artPending") != std::string::npos && art_scan.find("local TAG_SWEEP_VERSION = 5") != std::string::npos, "a picture frame larger than the first read stays hidden on the next scan");
        const std::string head_read = "local MAX_HEAD_READ_BYTES = 8 * 1024 * 1024\n";
        check(doomnet::patched_player_module("library", head_read).find("local MAX_HEAD_READ_BYTES = 15 * 1024 * 1024") != std::string::npos, "the cover read is still 8 MB");
        const std::string id3_break =
            "\t\tif frameSize <= 0 or pos + headerLen + frameSize - 1 > tagEnd then\r\n"
            "\t\t\tbreak\r\n"
            "\t\tend\r\n";
        check(doomnet::patched_player_module("id3", id3_break).find("tags.artPending = true") != std::string::npos, "the tag reader drops a picture frame that continues past the first read");
        check(doomnet::patched_player_module("settings", id3_break) == id3_break, "a module other than the tag reader was treated as the tag reader");
        const std::string playlist_readers =
            "function playlist_file.supported(path)\n"
            "\tlocal ext = extension_of(path)\n"
            "\treturn ext == \"m3u\" or ext == \"m3u8\" or ext == \"pls\"\n"
            "end\n"
            "\n"
            "-- { name = string, paths = { string, ... } } or nil when the file cannot be read.\n"
            "function playlist_file.read(path)\n"
            "\tlocal readOk, content = pcall(utils.read_file, path)\n"
            "\tif not readOk or type(content) ~= \"string\" or content == \"\" then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\tlocal ext = extension_of(path)\n"
            "\tlocal text = decode_text(content, ext)\n"
            "\tlocal title, paths\n"
            "\tif ext == \"pls\" then\n"
            "\t\ttitle, paths = parse_pls(text)\n"
            "\telse\n"
            "\t\ttitle, paths = parse_m3u(text)\n"
            "\tend\n"
            "\tif not title or title == \"\" then\n"
            "\t\ttitle = basename_no_ext(path)\n"
            "\tend\n"
            "\treturn { name = title, paths = paths or {} }\n"
            "end\n";
        const std::string playlist_mod = doomnet::patched_player_module("playlist_file", playlist_readers);
        check(playlist_mod.find("function playlist_file.read_all") != std::string::npos && playlist_mod.find("<key>Playlists</key>") != std::string::npos && playlist_mod.find("<location") != std::string::npos && playlist_mod.find("mpcpl") != std::string::npos && playlist_mod.find("aimppl") != std::string::npos && playlist_mod.find("parse_wpl") != std::string::npos, "playlist files other than M3U and PLS are ignored");
        const std::string playlist_sync =
            "\t\t\tlocal parsed = playlist_file.read(path)\n"
            "\t\t\tlocal trackIds = {}\n"
            "\t\t\tif parsed then\n"
            "\t\t\t\tlocal seenTrack = {}\n"
            "\t\t\t\tfor _, entry in ipairs(parsed.paths) do\n"
            "\t\t\t\t\tlocal resolved = resolve_entry(path, entry)\n"
            "\t\t\t\t\tlocal id = lookup(path_key(resolved))\n"
            "\t\t\t\t\tif id and not seenTrack[id] then\n"
            "\t\t\t\t\t\tseenTrack[id] = true\n"
            "\t\t\t\t\t\ttrackIds[#trackIds + 1] = id\n"
            "\t\t\t\t\tend\n"
            "\t\t\t\tend\n"
            "\t\t\tend\n"
            "\t\t\tlocal id = file_playlist_id(path)\n"
            "\t\t\tseen[id] = true\n"
            "\t\t\t-- Keep the playlist when the file names local tracks, even if none\n"
            "\t\t\t-- of them are in the library yet. A file that is only stream URLs\n"
            "\t\t\t-- is not something this player can queue.\n"
            "\t\t\tif parsed and #parsed.paths > 0 then\n"
            "\t\t\t\tlocal existing = state.byId[id]\n"
            "\t\t\t\tlocal name = parsed.name or \"\"\n"
            "\t\t\t\tif not existing or existing.sourcePath ~= path or existing.name ~= name or not same_ids(existing.trackIds or {}, trackIds) then\n"
            "\t\t\t\t\tstate.byId[id] = {\n"
            "\t\t\t\t\t\tid = id,\n"
            "\t\t\t\t\t\tname = name,\n"
            "\t\t\t\t\t\ttrackIds = trackIds,\n"
            "\t\t\t\t\t\tsourcePath = path,\n"
            "\t\t\t\t\t}\n"
            "\t\t\t\t\tchanged = changed + 1\n"
            "\t\t\t\tend\n"
            "\t\t\telseif state.byId[id] and state.byId[id].sourcePath then\n"
            "\t\t\t\tstate.byId[id] = nil\n"
            "\t\t\t\tchanged = changed + 1\n"
            "\t\t\tend\n";
        check(doomnet::patched_player_module("playlists", playlist_sync).find("playlist_file.read_all") != std::string::npos, "one iTunes library file cannot provide more than one playlist");
        const std::string playlist_walk = "\tif ext ~= \"m3u\" and ext ~= \"m3u8\" and ext ~= \"pls\" then\n\t\treturn\n\tend\n$plexts = @{'.m3u'=$true;'.m3u8'=$true;'.pls'=$true}\nelseif file_extension(path) == \"m3u\" or file_extension(path) == \"m3u8\" or file_extension(path) == \"pls\" then\nelseif ext == \"m3u\" or ext == \"m3u8\" or ext == \"pls\" then\n";
        const std::string walked = doomnet::patched_player_module("library", playlist_walk);
        check(walked.find(".xspf") != std::string::npos && walked.find(".plist") != std::string::npos && walked.find("note_playlist_file(path)") != std::string::npos, "the library scan does not look for XSPF, WPL, ASX, or iTunes playlist files");
    }
    check(patched_ui.find("!/[A-Za-z0-9]/.test(String(album))") != std::string::npos && patched_ui.find("new TextEncoder().encode(hashLabel)") != std::string::npos, "albums with no English letters still share one cover file");
    check(doomnet::shared_poll_call("get_duck_envelope") && doomnet::shared_poll_call("get_player_state") && doomnet::shared_poll_call("get_player_state_full") && !doomnet::shared_poll_call("stage_track_for_playback") && doomnet::audio_load_call("stage_track_for_playback"), "a play call waits behind the duck poll and the player-state poll");
    check(doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .doomnet-eq-slider::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:8px;height:8px;") != std::string::npos && doomnet::patched_player_css_text().find("background:var(--smp-accent,#66c0f4);") != std::string::npos && doomnet::patched_player_css_text().find("rotate(-90deg)") != std::string::npos, "the Now Playing EQ still uses the browser's blue slider");
    check(patched_ui.find("UI.doomnetPaintEqSlider(slider)") != std::string::npos && doomnet::patched_player_css_text().find(".smp-eq-fader-input::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:8px;height:8px;") != std::string::npos, "the settings equalizer still uses the browser's slider");
    check(doomnet::patched_player_css_text().find(".smp-settings-slider{flex:0 1 240px;width:240px;max-width:240px;") != std::string::npos && doomnet::patched_player_css_text().find(".smp-select{flex:0 1 360px;width:360px;max-width:min(360px,100%);}") != std::string::npos, "settings sliders and dropdowns still stretch across the page");
    check(doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-nptab-queue{width:min(740px,100%);max-width:none;margin:28px auto 0;}") != std::string::npos, "the Now Playing track list is not centered");
    check(doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-nptab-art-wrap{width:min(440px,100%);") != std::string::npos, "the Now Playing cover is still the small size");
    check(doomnet::patched_player_ui_text().find("line._smpMarqueeTries = 0") != std::string::npos, "a title still scrolls when the whole line is already visible");
    check(doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-seek{flex:0 1 360px;width:360px;max-width:360px;}") != std::string::npos, "the transport seek bar still stretches across the window");
}

bool write_test_bmp(const std::wstring& path, int width, int height) {
    const int row = (width * 3 + 3) & ~3;
    std::vector<unsigned char> file(54 + static_cast<size_t>(row) * height, 0);
    const unsigned size = static_cast<unsigned>(file.size());
    auto put32 = [&](size_t at, unsigned value) {
        for (int i = 0; i < 4; ++i) {
            file[at + i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFF);
        }
    };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<unsigned>(width));
    put32(22, static_cast<unsigned>(height));
    file[26] = 1;
    file[28] = 24;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned char* px = &file[54 + static_cast<size_t>(y) * row + static_cast<size_t>(x) * 3];
            px[0] = static_cast<unsigned char>(x);
            px[1] = static_cast<unsigned char>(y);
            px[2] = 128;
        }
    }
    std::wstring error;
    return doomnet::write_new_file(path, reinterpret_cast<const char*>(file.data()), file.size(), error);
}

void test_playback_queue() {
    doomnet::Bus bus;
    doomnet::CallMsg artwork;
    artwork.call_id = 1;
    artwork.method = "request_idle_art";
    doomnet::CallMsg play;
    play.call_id = 2;
    play.method = "stage_track_for_playback";
    bus.to_state(std::move(artwork));
    bus.to_state(std::move(play));
    doomnet::StateInbox first = bus.wait_state();
    const auto* call = std::get_if<doomnet::CallMsg>(&first);
    check(call != nullptr && call->call_id == 2, "a playback call waited behind artwork");
    doomnet::StateInbox second = bus.wait_state();
    const auto* later = std::get_if<doomnet::CallMsg>(&second);
    check(later != nullptr && later->call_id == 1, "the artwork call was dropped when playback ran first");
    doomnet::ResumeMsg resume;
    resume.co_id = 9;
    doomnet::CallMsg scan;
    scan.call_id = 3;
    scan.method = "rescan_library_start";
    bus.to_state(std::move(scan));
    bus.to_state(std::move(resume));
    doomnet::StateInbox third = bus.wait_state();
    check(std::holds_alternative<doomnet::ResumeMsg>(third), "a parked playback resume waited behind a library scan");
}

void test_art_thumbs() {
    check(doomnet::art_thumb_path(L"C:\\s\\art\\albums\\kiss_alive.png") == L"C:\\s\\art\\thumbs\\kiss_alive.jpg", "an album cover did not map to a jpg in the thumbs folder");
    check(doomnet::art_thumb_path(L"C:\\s\\art\\albums\\kiss_alive.custom").empty(), "a custom-cover marker was given a thumbnail");
    check(doomnet::art_thumb_path(L"C:\\s\\art\\kiss_alive.jpg").empty(), "a cover outside the albums folder was given a thumbnail");

    const std::wstring dir = temp_dir();
    const std::wstring big = dir + L"\\big.bmp";
    const std::wstring small = dir + L"\\small.bmp";
    check(write_test_bmp(big, 1000, 500), "could not write the large test image");
    check(write_test_bmp(small, 100, 50), "could not write the small test image");
    std::wstring error;
    unsigned w = 0;
    unsigned h = 0;
    check(doomnet::write_art_thumbnail(big, dir + L"\\big.jpg", doomnet::kArtThumbSize, error), "a large cover did not get a thumbnail");
    check(doomnet::art_image_size(dir + L"\\big.jpg", w, h) && w == doomnet::kArtThumbSize && h == doomnet::kArtThumbSize / 2, "a large cover thumbnail is not the thumbnail size with the same shape");
    check(doomnet::write_art_thumbnail(small, dir + L"\\small.jpg", doomnet::kArtThumbSize, error), "a small cover did not get a thumbnail");
    check(doomnet::art_image_size(dir + L"\\small.jpg", w, h) && w == 100 && h == 50, "a small cover was enlarged");
    check(!doomnet::write_art_thumbnail(dir + L"\\missing.jpg", dir + L"\\missing-thumb.jpg", doomnet::kArtThumbSize, error) && !error.empty(), "a missing cover did not report why it failed");
    check(GetFileAttributesW((dir + L"\\missing-thumb.jpg.tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "a failed thumbnail left a temporary file");
}

}  // namespace

int main() {
    test_paths();
    test_config();
    test_color();
    test_log_threads();
    test_steam();
    test_wide_io();
    test_launch_and_shortcut();
    test_install_window_and_payload();
    test_uninstall();
    test_toggle();
    test_place_bootstrap_and_pipe();
    test_launch_inject();
    test_skin_page();
    test_art_thumbs();
    test_playback_queue();
    test_instance();
    if (g_failed != 0) {
        std::fprintf(stderr, "----\n%d failure(s)\n", g_failed);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
