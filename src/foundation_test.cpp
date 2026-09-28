#include "config.h"
#include "installer.h"
#include "instance.h"
#include "launch.h"
#include "log.h"
#include "paths.h"
#include "payload.h"
#include "shortcut.h"
#include "steam_discover.h"
#include "wide_io.h"

#include <windows.h>

#include <cstdio>
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
    test_instance();
    if (g_failed != 0) {
        std::fprintf(stderr, "----\n%d failure(s)\n", g_failed);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
