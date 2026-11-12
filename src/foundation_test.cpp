#include "art_thumbs.h"
#include "cdp_pipe.h"
#include "host_bus.h"
#include "host_internal.h"
#include "catalog.h"
#include "config.h"
#include "installer.h"
#include "update_gate.h"
#include "instance.h"
#include "launch.h"
#include "log.h"
#include "paths.h"
#include "resident.h"
#include "page_bridge.h"
#include "payload.h"
#include "pipe_session.h"
#include "shortcut.h"
#include "skin_page.h"
#include "steam_attach.h"
#include "steam_discover.h"
#include "state_thread.h"
#include "stream.h"
#include "streamtunes_identity.h"
#include "json_wire.h"
#include "text.h"
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
    config.streamtunes_relay = L"wss://relay.example/streamtunes";
    config.updates = true;
    std::wstring error;
    check(doomnet::save_config_file(path, config, error), "save config");
    doomnet::Config loaded;
    check(doomnet::load_config_file(path, loaded, error) == doomnet::ConfigLoad::Ok, "load config");
    check(loaded.steam_path == config.steam_path && loaded.player && loaded.skin && loaded.player_color == doomnet::PlayerColor::Red &&
              loaded.streamtunes_relay == config.streamtunes_relay && loaded.updates,
          "round trip");

    doomnet::Config missing;
    check(doomnet::load_config_file(dir + L"\\nope.json", missing, error) == doomnet::ConfigLoad::Missing, "missing config");

    const char* bad[] = {
        "{\"playerColor\":\"blue\"}",
        "{\"steamPath\":\"C:\\\\Steam\\\\..\\\\Windows\",\"player\":true,\"skin\":false,\"playerColor\":\"steam\"}",
        "{\"player\":true,}",
        "{",
        "{\"unknown\":true}",
        "{\"streamTunesRelay\":\"http://relay.example/streamtunes\"}",
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

void test_streamtunes_identity() {
    const std::wstring steam = temp_dir();
    const std::wstring config_dir = steam + L"\\config";
    CreateDirectoryW(config_dir.c_str(), nullptr);
    const char vdf[] =
        "\"users\"\n{\n"
        "\t\"76561198000000001\"\n\t{\n"
        "\t\t\"AccountName\"\t\"private-login\"\n"
        "\t\t\"PersonaName\"\t\"Other Persona\"\n"
        "\t\t\"AutoLogin\"\t\"0\"\n\t}\n"
        "\t\"76561198000000002\"\n\t{\n"
        "\t\t\"AccountName\"\t\"also-private\"\n"
        "\t\t\"PersonaName\"\t\"Party Host\"\n"
        "\t\t\"AutoLogin\"\t\"1\"\n\t}\n"
        "}\n";
    std::wstring error;
    check(doomnet::write_new_file(config_dir + L"\\loginusers.vdf", vdf, sizeof(vdf) - 1, error), "write Steam persona fixture");
    std::string persona;
    check(doomnet::streamtunes_persona(steam, persona, error) && persona == "Party Host", "StreamTunes did not select the automatic Steam persona");
    const std::string script = doomnet::streamtunes_config_script(steam, L"wss://relay.example/streamtunes", error);
    check(script.find("Party Host") != std::string::npos && script.find("private-login") == std::string::npos &&
              script.find("wss://relay.example/streamtunes") != std::string::npos,
          "StreamTunes exposed the account login or omitted its guarded page config");
    check(doomnet::streamtunes_config_script(steam, L"", error).empty() && error.empty(), "an empty relay URL still enabled StreamTunes");
    check(doomnet::streamtunes_relay_for_launch(L"D:\\Documents\\Repos\\DoomNet-SteamPortal\\build\\Release\\DoomNet.exe", L"").empty(), "a source tree was given the test party address");
    const std::wstring tester_relay = doomnet::streamtunes_relay_for_launch(L"C:\\Users\\Tester\\AppData\\Local\\DoomNet\\DoomNet.exe", L"");
    check(tester_relay.rfind(L"wss://", 0) == 0 && tester_relay.find(L"/streamtunes") != std::wstring::npos, "a tester install was not given the party address");
    check(doomnet::streamtunes_relay_for_launch(L"C:\\Users\\Tester\\AppData\\Local\\DoomNet\\DoomNet.exe", L"wss://relay.example/streamtunes") == L"wss://relay.example/streamtunes", "a saved party address was replaced");
}

void test_update_gate() {
    check(doomnet::path_is_dev_source_tree(L"D:\\Documents\\Repos\\DoomNet-SteamPortal\\build\\Release\\DoomNet.exe"), "DoomNet source tree was treated as a tester install");
    check(doomnet::path_is_dev_source_tree(L"D:\\Documents\\Repos\\SteamTunes"), "SteamTunes source tree was treated as a tester install");
    check(doomnet::path_is_dev_source_tree(L"D:\\Documents\\Repos\\SteamSkin\\skin.json"), "SteamSkin source tree was treated as a tester install");
    check(!doomnet::path_is_dev_source_tree(L"C:\\Users\\Tester\\AppData\\Local\\DoomNet\\DoomNet.exe"), "a tester install path was treated as a source tree");
    check(!doomnet::updates_should_run(L"C:\\Users\\Tester\\AppData\\Local\\DoomNet\\DoomNet.exe", L"C:\\Users\\Tester\\AppData\\Local\\DoomNet", false), "updates ran while config kept them off");
    check(!doomnet::updates_should_run(L"D:\\Documents\\Repos\\DoomNet-SteamPortal\\build\\Release\\DoomNet.exe", L"C:\\Users\\Tester\\AppData\\Local\\DoomNet", true), "updates ran from the DoomNet source tree");
    const doomnet::UpdateCheckResult blocked = doomnet::check_production_updates(L"D:\\Documents\\Repos\\DoomNet-SteamPortal\\build\\Release\\DoomNet.exe", L"C:\\Users\\Tester\\AppData\\Local\\DoomNet", true);
    check(!blocked.restart_steam && blocked.message.find(L"nothing was downloaded") != std::wstring::npos, "a source tree was offered an update download");
    const std::wstring stale_exe = temp_dir() + L"\\newer-host.exe";
    const char stale_byte[] = "x";
    std::wstring stale_error;
    check(doomnet::write_new_file(stale_exe, stale_byte, 1, stale_error), "write a newer host file");
    HANDLE stale_file = CreateFileW(stale_exe.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(stale_file != INVALID_HANDLE_VALUE, "open the newer host file");
    if (stale_file != INVALID_HANDLE_VALUE) {
        FILETIME written{};
        GetSystemTimeAsFileTime(&written);
        ULARGE_INTEGER later{};
        later.LowPart = written.dwLowDateTime;
        later.HighPart = written.dwHighDateTime;
        later.QuadPart += 60ULL * 60ULL * 10000000ULL;
        written.dwLowDateTime = later.LowPart;
        written.dwHighDateTime = later.HighPart;
        check(SetFileTime(stale_file, nullptr, nullptr, &written) != 0, "set the newer host file time");
        CloseHandle(stale_file);
    }
    check(doomnet::host_file_is_newer_than_this_process(stale_exe), "a host file written after startup was treated as the copy already running");
    check(doomnet::command_is_reload_steam(L"--reload-steam \"C:\\Program Files (x86)\\Steam\\steam.exe\" 42"), "the clean restart command was not recognized");
    check(doomnet::clean_restart_should_launch(true, true), "a leftover Steam process canceled the reopen");
    check(!doomnet::command_is_reload_steam(L"--install"), "install was treated as the clean restart");
    const std::wstring steam_folder = L"C:\\Program Files (x86)\\Steam";
    const std::wstring host_exe = L"C:\\Users\\Tester\\AppData\\Local\\DoomNet\\DoomNet.exe";
    check(doomnet::update_restart_stops_image(steam_folder, steam_folder + L"\\steam.exe", host_exe, 10, 1), "steam.exe was left running");
    check(doomnet::update_restart_stops_image(steam_folder, steam_folder + L"\\bin\\cef\\steamwebhelper.exe", host_exe, 11, 1), "steamwebhelper.exe was left running");
    check(doomnet::update_restart_stops_image(steam_folder, host_exe, host_exe, 12, 1), "the old DoomNet.exe was left running");
    check(!doomnet::update_restart_stops_image(steam_folder, host_exe, host_exe, 1, 1), "the restart process was told to stop itself");
    check(!doomnet::update_restart_stops_image(steam_folder, steam_folder + L"\\steamapps\\common\\Game\\game.exe", host_exe, 13, 1), "a game was treated as a Steam background process");
    check(!doomnet::update_restart_stops_image(steam_folder, L"C:\\Windows\\System32\\steam.exe", host_exe, 14, 1), "a System32 process was allowed to be stopped");
    check(!doomnet::host_file_is_newer_than_this_process(L"C:\\Users\\Tester\\AppData\\Local\\DoomNet\\missing-host.exe"), "a missing host file was treated as a newer copy");
    check(doomnet::updates_should_run(L"C:\\Users\\Tester\\AppData\\Local\\DoomNet\\DoomNet.exe", L"C:\\Users\\Tester\\AppData\\Local\\DoomNet", true), "a tester install with updates on was blocked");
    doomnet::publish_update_notice(L"A new version is available. It's downloading now.");
    check(doomnet::current_update_notice() == L"A new version is available. It's downloading now.", "the page is not told that a new version is downloading");
    doomnet::publish_update_notice(L"The new version was downloaded. Quit Steam and open it again to use it.");
    check(doomnet::current_update_notice().find(L"downloaded") != std::wstring::npos, "the page is not told when the download finishes");
    doomnet::publish_update_notice(L"");
    check(doomnet::installed_host_leaves_when_steam_exits(L"C:\\Users\\Tester\\AppData\\Local\\DoomNet\\DoomNet.exe"), "an installed copy stays running after Steam exits, so the new program never loads");
    check(!doomnet::installed_host_leaves_when_steam_exits(L"D:\\Documents\\Repos\\DoomNet-SteamPortal\\build\\Release\\DoomNet.exe"), "a source tree leaves when Steam exits");
    const std::wstring payload_repo = temp_dir();
    const std::wstring payload_installed = temp_dir();
    std::wstring payload_error;
    const char payload_marker[] = "{\"name\":\"marker\"}";
    check(doomnet::write_new_file(payload_repo + L"\\skin.json", payload_marker, sizeof(payload_marker) - 1, payload_error), "payload repo marker");
    check(doomnet::write_new_file(payload_installed + L"\\skin.json", payload_marker, sizeof(payload_marker) - 1, payload_error), "payload installed marker");
    check(doomnet::launch_payload_dir_for(L"C:\\Users\\Tester\\AppData\\Local\\DoomNet\\DoomNet.exe", L"C:\\Users\\Tester\\AppData\\Local\\DoomNet", payload_repo, payload_installed, L"skin.json") == payload_installed, "an installed copy read the local repo instead of the installed skin");
    check(doomnet::launch_payload_dir_for(L"D:\\Documents\\Repos\\DoomNet-SteamPortal\\build\\Release\\DoomNet.exe", L"C:\\Users\\Tester\\AppData\\Local\\DoomNet", payload_repo, payload_installed, L"skin.json") == payload_repo, "a source tree did not read the local repo");

    const std::wstring dir = temp_dir();
    const std::wstring path = dir + L"\\versions.json";
    doomnet::InstalledVersions versions;
    versions.doomnet = L"0.1.0";
    versions.steamtunes = L"1.15.0";
    versions.steamskin = L"0.9.27";
    std::wstring error;
    check(doomnet::save_installed_versions(path, versions, error), "save versions.json");
    doomnet::InstalledVersions loaded;
    check(doomnet::load_installed_versions(path, loaded, error), "load versions.json");
    check(loaded.doomnet == versions.doomnet && loaded.steamtunes == versions.steamtunes && loaded.steamskin == versions.steamskin, "versions.json round trip");

    const char* manifest_text =
        "{\"doomnet\":{\"version\":\"0.1.0\",\"url\":\"https://example.com/doomnet\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},"
        "\"steamtunes\":{\"version\":\"1.15.0\",\"url\":\"https://example.com/player\",\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"},"
        "\"steamskin\":{\"version\":\"9.9.9\",\"url\":\"https://example.com/skin\",\"sha256\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"}}";
    doomnet::ReleaseManifest manifest;
    check(doomnet::parse_release_manifest(manifest_text, manifest, error), "production manifest parse");
    doomnet::InstalledVersions installed = versions;
    installed.steamskin.clear();
    const doomnet::UpdatePlan skipped = doomnet::updates_needed(manifest, installed);
    check(!skipped.doomnet && !skipped.steamtunes && !skipped.steamskin, "a skipped SteamSkin was treated as an update");
    installed.steamskin = L"0.9.27";
    installed.steamtunes = L"1.0.0";
    const doomnet::UpdatePlan needed = doomnet::updates_needed(manifest, installed);
    check(!needed.doomnet && needed.steamtunes && needed.steamskin, "installed pieces with a new version were not offered");
    check(!doomnet::parse_release_manifest("{\"doomnet\":{\"version\":\"1\",\"url\":\"http://example.com/x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}", manifest, error), "an http manifest was accepted");

    const std::string hello = "hello";
    const std::string hello_hash = doomnet::sha256_hex(hello.data(), hello.size());
    check(hello_hash.size() == 64, "sha256 length");
    const std::wstring payload_file = dir + L"\\payload.bin";
    const std::wstring installed_file = dir + L"\\installed.bin";
    const char keep[] = "keep";
    check(doomnet::write_new_file(payload_file, hello.data(), hello.size(), error), "payload write");
    check(doomnet::write_new_file(installed_file, keep, sizeof(keep) - 1, error), "installed write");
    check(!doomnet::payload_hash_matches(payload_file, std::string(64, '0'), error), "a mismatched hash was accepted");
    check(!doomnet::replace_file_if_hash_matches(payload_file, installed_file, std::string(64, '0'), error), "a mismatched hash replaced a file");
    std::string still;
    check(doomnet::read_entire_file(installed_file, still, error) && still == "keep", "a mismatched hash changed the installed file");
    check(doomnet::replace_file_if_hash_matches(payload_file, installed_file, hello_hash, error), "a matching hash did not replace the file");
    check(doomnet::read_entire_file(installed_file, still, error) && still == "hello", "the verified payload was not the file that landed");
    const std::wstring rejected_download = dir + L"\\not-downloaded.bin";
    check(!doomnet::download_https(L"http://example.com/payload.bin", rejected_download, 1024, error), "an http download was accepted");
    check(!doomnet::wide_file_exists(rejected_download), "an http download left a file");

    doomnet::note_production_update_gate(L"D:\\Documents\\Repos\\DoomNet-SteamPortal\\build\\Release\\DoomNet.exe", dir, true);

    auto append_u16 = [](std::string& out, unsigned value) {
        out.push_back(static_cast<char>(value & 0xFF));
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
    };
    auto append_u32 = [](std::string& out, unsigned value) {
        out.push_back(static_cast<char>(value & 0xFF));
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
        out.push_back(static_cast<char>((value >> 16) & 0xFF));
        out.push_back(static_cast<char>((value >> 24) & 0xFF));
    };
    auto store_zip = [&](const std::vector<std::pair<std::string, std::string>>& files) {
        std::string locals;
        std::string central;
        unsigned count = 0;
        for (const auto& file : files) {
            const unsigned offset = static_cast<unsigned>(locals.size());
            append_u32(locals, 0x04034b50u);
            append_u16(locals, 20);
            append_u16(locals, 0);
            append_u16(locals, 0);
            append_u16(locals, 0);
            append_u16(locals, 0);
            append_u32(locals, 0);
            append_u32(locals, static_cast<unsigned>(file.second.size()));
            append_u32(locals, static_cast<unsigned>(file.second.size()));
            append_u16(locals, static_cast<unsigned>(file.first.size()));
            append_u16(locals, 0);
            locals += file.first;
            locals += file.second;
            append_u32(central, 0x02014b50u);
            append_u16(central, 20);
            append_u16(central, 20);
            append_u16(central, 0);
            append_u16(central, 0);
            append_u16(central, 0);
            append_u16(central, 0);
            append_u32(central, 0);
            append_u32(central, static_cast<unsigned>(file.second.size()));
            append_u32(central, static_cast<unsigned>(file.second.size()));
            append_u16(central, static_cast<unsigned>(file.first.size()));
            append_u16(central, 0);
            append_u16(central, 0);
            append_u16(central, 0);
            append_u16(central, 0);
            append_u32(central, 0);
            append_u32(central, offset);
            central += file.first;
            ++count;
        }
        std::string zip = locals;
        const unsigned cd_off = static_cast<unsigned>(zip.size());
        zip += central;
        append_u32(zip, 0x06054b50u);
        append_u16(zip, 0);
        append_u16(zip, 0);
        append_u16(zip, count);
        append_u16(zip, count);
        append_u32(zip, static_cast<unsigned>(central.size()));
        append_u32(zip, cd_off);
        append_u16(zip, 0);
        return zip;
    };

    const std::wstring update_root = dir + L"\\updater";
    CreateDirectoryW(update_root.c_str(), nullptr);
    CreateDirectoryW((update_root + L"\\updates").c_str(), nullptr);
    CreateDirectoryW((update_root + L"\\player").c_str(), nullptr);
    CreateDirectoryW((update_root + L"\\player\\backend").c_str(), nullptr);
    CreateDirectoryW((update_root + L"\\player\\backend\\data").c_str(), nullptr);
    const char saved_settings[] = "{\"uiColorTheme\":\"red\"}\n";
    check(doomnet::write_new_file(update_root + L"\\player\\backend\\data\\settings.json", saved_settings, sizeof(saved_settings) - 1, error), "updater settings");
    const char old_main[] = "old";
    check(doomnet::write_new_file(update_root + L"\\player\\backend\\main.lua", old_main, sizeof(old_main) - 1, error), "updater old main");
    doomnet::InstalledVersions update_versions;
    update_versions.doomnet = L"0.1.0";
    update_versions.steamtunes = L"1.15.0";
    update_versions.steamskin = L"0.9.27";
    check(doomnet::save_installed_versions(update_root + L"\\versions.json", update_versions, error), "updater versions");
    const std::string player_zip = store_zip({{"plugin.json", "{}\n"}, {"backend/main.lua", "new-player"}, {"backend/data/settings.json", "overwrite"}});
    const std::string player_hash = doomnet::sha256_hex(player_zip.data(), player_zip.size());
    check(doomnet::write_new_file(update_root + L"\\updates\\steamtunes.bin", player_zip.data(), player_zip.size(), error), "updater player zip");
    const std::string player_pending = "steamtunes playerhash1 " + player_hash + "\n";
    check(player_pending.size() > 64, "player pending line");
    check(doomnet::write_new_file(update_root + L"\\updates\\pending.txt", player_pending.data(), player_pending.size(), error), "updater pending");
    check(doomnet::apply_pending_updates(update_root, L""), "the player update was not applied");
    std::string updated_main;
    std::string kept_settings;
    check(doomnet::read_entire_file(update_root + L"\\player\\backend\\main.lua", updated_main, error) && updated_main == "new-player", "the player update did not replace main.lua");
    check(doomnet::read_entire_file(update_root + L"\\player\\backend\\data\\settings.json", kept_settings, error) && kept_settings == saved_settings, "the player update replaced settings.json");
    check(!doomnet::wide_file_exists(update_root + L"\\player\\backend\\data\\settings.json") || kept_settings == saved_settings, "settings check");
    doomnet::InstalledVersions after_update;
    check(doomnet::load_installed_versions(update_root + L"\\versions.json", after_update, error) && after_update.steamtunes == L"playerhash1" && after_update.doomnet == L"0.1.0", "the applied player version was not recorded");

    const std::string hostile = store_zip({{"../escaped.txt", "no"}});
    const std::string hostile_hash = doomnet::sha256_hex(hostile.data(), hostile.size());
    check(doomnet::write_new_file(update_root + L"\\updates\\steamskin.bin", hostile.data(), hostile.size(), error), "hostile zip");
    const std::string hostile_pending = "steamskin hostilehash " + hostile_hash + "\n";
    check(doomnet::write_new_file(update_root + L"\\updates\\pending.txt", hostile_pending.data(), hostile_pending.size(), error), "hostile pending");
    check(!doomnet::apply_pending_updates(update_root, L""), "a zip that climbs out of the folder was applied");
    check(!doomnet::wide_file_exists(dir + L"\\escaped.txt"), "a zip wrote a file outside the update folder");
    check(!doomnet::wide_file_exists(update_root + L"\\updates\\steamskin.bin"), "a rejected update was kept");

    const char old_exe[] = "OLD";
    check(doomnet::write_new_file(update_root + L"\\DoomNet.exe", old_exe, sizeof(old_exe) - 1, error), "old program");
    const std::string program_zip = store_zip({{"DoomNet.exe", "NEWEXE"}, {"version.dll", "DLL"}});
    const std::string program_hash = doomnet::sha256_hex(program_zip.data(), program_zip.size());
    check(doomnet::write_new_file(update_root + L"\\updates\\doomnet.bin", program_zip.data(), program_zip.size(), error), "program zip");
    const std::string program_pending = "doomnet programhash " + program_hash + "\n";
    check(doomnet::write_new_file(update_root + L"\\updates\\pending.txt", program_pending.data(), program_pending.size(), error), "program pending");
    check(doomnet::apply_pending_updates(update_root, L""), "the program update was not applied");
    std::string new_exe;
    std::string previous_exe;
    std::string new_dll;
    check(doomnet::read_entire_file(update_root + L"\\DoomNet.exe", new_exe, error) && new_exe == "NEWEXE", "DoomNet.exe was not replaced");
    check(doomnet::read_entire_file(update_root + L"\\DoomNet.exe.previous", previous_exe, error) && previous_exe == "OLD", "the previous program was not kept");
    check(doomnet::read_entire_file(update_root + L"\\version.dll", new_dll, error) && new_dll == "DLL", "version.dll was not saved beside the program");

    const char keep_exe[] = "KEEP";
    check(doomnet::write_new_file(update_root + L"\\DoomNet.exe", keep_exe, sizeof(keep_exe) - 1, error), "keep program");
    check(doomnet::write_new_file(update_root + L"\\updates\\doomnet.bin", old_exe, sizeof(old_exe) - 1, error), "bad program payload");
    const std::string bad_pending = std::string("doomnet badhash ") + std::string(64, '0') + "\n";
    check(doomnet::write_new_file(update_root + L"\\updates\\pending.txt", bad_pending.data(), bad_pending.size(), error), "bad pending");
    check(!doomnet::apply_pending_updates(update_root, L""), "a mismatched update hash was applied");
    std::string kept_exe;
    check(doomnet::read_entire_file(update_root + L"\\DoomNet.exe", kept_exe, error) && kept_exe == "KEEP", "a mismatched hash replaced DoomNet.exe");

    const std::wstring skin_src = dir + L"\\srcskin";
    CreateDirectoryW(skin_src.c_str(), nullptr);
    const char skin_json[] = "{\"version\":\"9.9.9\"}\n";
    const char theme[] = "body{}\n";
    check(doomnet::write_new_file(skin_src + L"\\skin.json", skin_json, sizeof(skin_json) - 1, error), "fixture skin.json");
    check(doomnet::write_new_file(skin_src + L"\\theme.css", theme, sizeof(theme) - 1, error), "fixture theme");
    const std::wstring install_root = dir + L"\\install";
    CreateDirectoryW(install_root.c_str(), nullptr);
    const std::wstring settings = install_root + L"\\player\\backend\\data\\settings.json";
    CreateDirectoryW((install_root + L"\\player").c_str(), nullptr);
    CreateDirectoryW((install_root + L"\\player\\backend").c_str(), nullptr);
    CreateDirectoryW((install_root + L"\\player\\backend\\data").c_str(), nullptr);
    const char saved[] = "{\"uiColorTheme\":\"red\",\"keep\":\"yes\"}\n";
    check(doomnet::write_new_file(settings, saved, sizeof(saved) - 1, error), "existing settings");
    doomnet::Config before;
    before.player = true;
    before.skin = false;
    before.player_color = doomnet::PlayerColor::Red;
    before.updates = false;
    before.steam_path = dir + L"\\notsteam";
    check(doomnet::save_config_file(install_root + L"\\config.json", before, error), "existing config");
    doomnet::InstalledVersions prior;
    prior.doomnet = L"0.1.0";
    prior.steamtunes = L"1.15.0";
    check(doomnet::save_installed_versions(install_root + L"\\versions.json", prior, error), "existing versions");
    doomnet::StoreInstallRequest store;
    store.root = install_root;
    store.skin_source = skin_src;
    store.steam_path = before.steam_path;
    const doomnet::InstallResult stored = doomnet::install_store_skin(store);
    check(stored.ok, "store skin install");
    std::string settings_after;
    check(doomnet::read_entire_file(settings, settings_after, error) && settings_after == saved, "store install rewrote settings.json");
    doomnet::Config after;
    check(doomnet::load_config_file(install_root + L"\\config.json", after, error) == doomnet::ConfigLoad::Ok, "store config load");
    check(after.skin && after.player && !after.updates && after.player_color == doomnet::PlayerColor::Red, "store install changed player, color, or updates");
    doomnet::InstalledVersions after_versions;
    check(doomnet::load_installed_versions(install_root + L"\\versions.json", after_versions, error), "store versions");
    check(after_versions.steamskin == L"9.9.9" && after_versions.steamtunes == L"1.15.0", "store version record");
    const doomnet::InstallResult again = doomnet::install_store_skin(store);
    check(!again.ok && again.files_copied == 0, "a second SteamSkin install was accepted");
    check(doomnet::read_entire_file(settings, settings_after, error) && settings_after == saved, "a second SteamSkin install rewrote settings.json");
    const std::wstring steam_root = dir + L"\\steamdir";
    CreateDirectoryW(steam_root.c_str(), nullptr);
    check(doomnet::write_new_file(steam_root + L"\\steam.exe", "x", 1, error), "fake steam.exe");
    doomnet::StoreInstallRequest inside;
    inside.root = steam_root + L"\\data";
    inside.skin_source = skin_src;
    inside.steam_path = steam_root;
    check(!doomnet::install_store_skin(inside).ok, "store install inside Steam was accepted");

    doomnet::StartupMode mode = doomnet::StartupMode::Run;
    std::wstring rejected;
    check(doomnet::parse_startup_mode(L"", mode, rejected) && mode == doomnet::StartupMode::Once, "an empty command line stayed resident");
    check(!doomnet::command_is_install(L""), "an empty command line was treated as --install");
    check(doomnet::command_is_install(L"--install --skin --steam \"C:\\Steam\""), "--install was not recognized");
    check(doomnet::command_is_hook(L"--hook \"C:\\Steam\""), "--hook was not recognized");
    check(!doomnet::command_is_hook(L"--install"), "--install was treated as the hook copy");
    const std::wstring command = L"--install --skin --color red --steam \"" + (dir + L"\\notsteam") + L"\" --skin-source \"" + skin_src + L"\"";
    const std::wstring command_root = dir + L"\\command-install";
    const doomnet::InstallResult commanded = doomnet::run_command_line_install(command_root, command.c_str(), L"");
    check(commanded.ok, "--install did not copy the skin");
    check(doomnet::wide_file_exists(command_root + L"\\skin\\skin.json"), "--install did not write skin.json");
    std::string command_settings;
    check(!doomnet::read_entire_file(command_root + L"\\player\\backend\\data\\settings.json", command_settings, error), "--install wrote settings.json for a skin-only copy");

    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring folder = self;
    folder.resize(folder.find_last_of(L"\\/"));
    const std::wstring child = folder + L"\\DoomNetDummyChild.exe";
    const std::wstring steam_exe = dir + L"\\steam.exe";
    const std::wstring marker = dir + L"\\steam-marker.txt";
    check(CopyFileW(child.c_str(), steam_exe.c_str(), FALSE) != FALSE, "could not copy the steam.exe test stub");
    SetEnvironmentVariableW(L"DOOMNET_CHILD_MARKER", marker.c_str());
    check(doomnet::start_steam_visible(steam_exe, error), "visible steam start");
    bool marked = false;
    for (int i = 0; i < 50 && !marked; ++i) {
        marked = doomnet::wide_file_exists(marker);
        if (!marked) {
            Sleep(20);
        }
    }
    check(marked, "the visible steam.exe stub did not write its marker");
    DWORD exit_code = 0;
    check(!doomnet::launch_hidden(steam_exe, 1000, exit_code, error), "launch_hidden started steam.exe");
    check(error.find(L"steam.exe") != std::wstring::npos, "launch_hidden did not name steam.exe");
    check(!doomnet::start_steam_visible(dir + L"\\not-steam.exe", error), "a file that is not steam.exe was started");
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
    const char plugin[] = "{\"name\":\"SteamMusicPlayer\",\"version\":\"1.15.0\"}\n";
    const char backend[] = "backend-marker\n";
    const char skin[] = "{\"UseDefaultPatches\":true,\"version\":\"0.9.27\"}\n";
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
    doomnet::Config installed_config;
    check(doomnet::load_config_file(root + L"\\config.json", installed_config, error) == doomnet::ConfigLoad::Ok && installed_config.updates, "tester install did not turn updates on");
    doomnet::InstalledVersions installed_versions;
    check(doomnet::load_installed_versions(root + L"\\versions.json", installed_versions, error), "versions.json missing after install");
    check(installed_versions.doomnet == L"0.1.0" && installed_versions.steamtunes == L"1.15.0" && installed_versions.steamskin == L"0.9.27", "versions.json did not capture the installed app versions");

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
    const std::wstring built_version = self_dir + L"\\version.dll";
    check(doomnet::wide_file_exists(built_version), "the built version.dll is not beside the test");
    std::string version_bytes;
    check(doomnet::read_entire_file(built_version, version_bytes, error) && version_bytes.find("DoomNetWsock32") != std::string::npos && version_bytes.find("VerQueryValueW") != std::string::npos, "the built version.dll is not the startup hook");
    const std::wstring version_steam = temp_dir();
    CreateDirectoryW(version_steam.c_str(), nullptr);
    check(doomnet::write_new_file(version_steam + L"\\steam.exe", "x", 1, error), "version steam.exe");
    check(CopyFileW(built_version.c_str(), (version_steam + L"\\version.dll").c_str(), FALSE) != FALSE, "could not stage version.dll");
    check(doomnet::millennium_state(version_steam) == doomnet::MillenniumState::NotPresent, "DoomNet's version.dll was treated as Millennium");
    const std::wstring late_hook = version_steam + L"\\wsock32.dll";
    check(CopyFileW(built.c_str(), late_hook.c_str(), FALSE) != FALSE, "could not stage the late wsock32.dll");
    const doomnet::PlaceBootstrapResult version_placed = doomnet::place_doomnet_bootstrap(version_steam, built_version, false);
    check(version_placed.ok, "version.dll was not placed beside steam.exe");
    check(!doomnet::wide_file_exists(late_hook), "the late wsock32.dll was left beside steam.exe");
    check(doomnet::doomnet_hook_installed(version_steam), "version.dll was not accepted as the hook for a Steam the user already opened");
    const std::wstring bare_steam = temp_dir();
    make_fake_steam(bare_steam, false);
    check(!doomnet::doomnet_hook_installed(bare_steam), "a Steam folder with no DoomNet hook was accepted");
    const std::wstring missing_tree = temp_dir() + L"\\missing";
    const std::wstring installed_tree = temp_dir();
    check(doomnet::write_new_file(installed_tree + L"\\skin.json", "{}\n", 3, error), "installed skin marker");
    check(doomnet::first_existing_marker(missing_tree, installed_tree, L"skin.json") == installed_tree, "an installed skin was ignored when the source repo is not on the machine");
    check(doomnet::first_existing_marker(installed_tree, missing_tree, L"skin.json") == installed_tree, "the source repo did not win over an installed copy");

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

    std::wstring install_command = L"\"" + exe + L"\" --install --player --steam \"C:\\no-such-steam\"";
    std::vector<wchar_t> install_mutable(install_command.begin(), install_command.end());
    install_mutable.push_back(L'\0');
    PROCESS_INFORMATION install_process{};
    const BOOL install_created = CreateProcessW(exe.c_str(), install_mutable.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &install_process);
    check(install_created == TRUE, "could not start DoomNet.exe --install while the mutex is held");
    if (install_created) {
        const DWORD install_waited = WaitForSingleObject(install_process.hProcess, 2000);
        DWORD install_code = 0;
        GetExitCodeProcess(install_process.hProcess, &install_code);
        CloseHandle(install_process.hThread);
        CloseHandle(install_process.hProcess);
        check(install_waited == WAIT_OBJECT_0 && install_code == 1, "install while DoomNet is already running exited as success");
        std::string install_result;
        std::wstring install_error;
        check(doomnet::read_entire_file(root + L"\\install-result.txt", install_result, install_error) && install_result.find("Nothing was copied") != std::string::npos, "install while DoomNet is already running did not write that nothing was copied");
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
    std::string accent;
    std::string accent_dark;
    check(doomnet::skin_accent_for_theme("green", "", accent, accent_dark) && accent == "#3d9b4a" && accent_dark == "#24662e", "green did not use the music player's green");
    check(doomnet::skin_accent_for_theme("custom", "#112233", accent, accent_dark) && accent == "#112233" && accent_dark == "#0b1621", "a custom color was not darkened");
    const std::string recolored = doomnet::recolor_skin_css("a #c93939 b #8b2929 c #8b929a d #E07070", "#3d9b4a", "#24662e");
    check(recolored.find("#c93939") == std::string::npos && recolored.find("#8b2929") == std::string::npos && recolored.find("#e07070") == std::string::npos && recolored.find("#E07070") == std::string::npos && recolored.find("#3d9b4a") != std::string::npos && recolored.find("#24662e") != std::string::npos && recolored.find("#8b929a") != std::string::npos, "SteamSkin red was left in place or a grey color was changed");
    check(!doomnet::skin_accent_for_theme("purple", "", accent, accent_dark), "an unknown color was accepted");
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
    wchar_t previous_root[MAX_PATH] = {};
    const DWORD previous_root_length = GetEnvironmentVariableW(L"DOOMNET_ROOT", previous_root, MAX_PATH);
    const std::wstring color_root = temp_dir();
    SetEnvironmentVariableW(L"DOOMNET_ROOT", color_root.c_str());
    std::wstring color_error;
    check(doomnet::save_skin_color("green", "", color_error), "SteamSkin color was not saved");
    std::string color_json;
    check(doomnet::read_entire_file(color_root + L"\\skin-color.json", color_json, color_error) && color_json.find("\"green\"") != std::string::npos, "skin-color.json did not record green");
    check(doomnet::saved_skin_color_token() == "green", "the saved SteamSkin color was not green");
    check(!doomnet::wide_file_exists(color_root + L"\\player\\backend\\data\\settings.json"), "saving a SteamSkin color wrote the music player's settings");
    const std::wstring color_skin = temp_dir();
    const std::wstring color_steam = temp_dir();
    CreateDirectoryW(color_skin.c_str(), nullptr);
    CreateDirectoryW(color_steam.c_str(), nullptr);
    const char red_css[] = "a #c93939 b #8b929a\n";
    check(doomnet::write_new_file(color_steam + L"\\steam.exe", "x", 1, color_error), "color fixture steam.exe");
    check(doomnet::write_new_file(color_skin + L"\\libraryroot.custom.css", red_css, sizeof(red_css) - 1, color_error), "color fixture css");
    const doomnet::SkinFileCopy colored = doomnet::copy_skin_files(color_skin, color_steam);
    std::string colored_css;
    check(colored.ok && doomnet::read_entire_file(color_steam + L"\\steamui\\doomnet-skin\\libraryroot.custom.css", colored_css, color_error) && colored_css.find("#3d9b4a") != std::string::npos && colored_css.find("#c93939") == std::string::npos && colored_css.find("#8b929a") != std::string::npos, "the installed SteamSkin file stayed red");
    std::string source_css;
    check(doomnet::read_entire_file(color_skin + L"\\libraryroot.custom.css", source_css, color_error) && source_css.find("#c93939") != std::string::npos, "the SteamSkin source file was recolored");
    if (previous_root_length > 0 && previous_root_length < MAX_PATH) {
        SetEnvironmentVariableW(L"DOOMNET_ROOT", previous_root);
    } else {
        SetEnvironmentVariableW(L"DOOMNET_ROOT", nullptr);
    }

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
    check(boot.find("win.__doomnetStore = window.__doomnetStore") != std::string::npos, "the Steam window does not receive the Store record, so an installed skin is shown as not installed");
    check(boot.find("20261112-version-1.17.1-skin-portal") != std::string::npos && boot.find("20261027-version-1.15.0") == std::string::npos, "Steam still loads the player as version 1.15.0");
    check(boot.find("win.__doomnetPartyConfig = window.__doomnetPartyConfig") != std::string::npos && boot.find("win.DoomNetStreamTunes = window.DoomNetStreamTunes") != std::string::npos, "the Steam window does not receive StreamTunes, so the listening-party button never appears");
    check(boot.find("doomnet-steamtunes-tab") != std::string::npos, "boot script does not add a SteamTunes tab");
    check(boot.find("row.appendChild(tab)") != std::string::npos, "the SteamTunes tab is not placed after the account name");
    check(boot.find("SetVisible") != std::string::npos && boot.find("bounds.w > 8 && bounds.h > 8") != std::string::npos && boot.find("on || !steamTunesOpen(doc)") != std::string::npos && boot.find("function steamContentBox") != std::string::npos && boot.find("Leave the extra views collapsed") != std::string::npos && boot.find("area >= browser.__doomnetBestArea") != std::string::npos, "opening SteamTunes does not hide the store page underneath");
    check(boot.find("#doomnet-steamtunes-host *{-webkit-app-region:no-drag") != std::string::npos, "Steam's window drag regions can still swallow clicks on the SteamTunes transport");
    check(boot.find("findNavLabel") != std::string::npos && boot.find("\"STORE\"") != std::string::npos && boot.find("\"LIBRARY\"") != std::string::npos && boot.find("\"COMMUNITY\"") != std::string::npos, "the SteamTunes tab is not matched by the Store, Library, and Community labels");
    check(boot.find("function releaseOtherNav") != std::string::npos && boot.find("function restoreOtherNav") != std::string::npos && boot.find("html body #doomnet-steamtunes-tab.doomnet-tab-on,html body #doomnet-steamtunes-tab.doomnet-tab-on *{color:var(--steam-accent-highlight,#c93939) !important;}") != std::string::npos && boot.find("html body #doomnet-steamtunes-tab.doomnet-tab-on::after{content:\\\"\\\";position:absolute") != std::string::npos, "the SteamTunes tab stays grey when it is selected");
    check(boot.find("OverlayBrowser_Browser") != std::string::npos && boot.find("SP Overlay:") != std::string::npos, "the overlay no longer gets the compact player");
    check(boot.find("__doomnetPlayerOnly") != std::string::npos, "boot script no longer has a store-only player path");
    check(boot.find("window !== window.top && !window.__doomnetPlayerOnly") != std::string::npos, "a store frame still returns before the player binding");
    const std::string store_player = doomnet::store_player_script();
    check(store_player.find("smp-plugin-ui") != std::string::npos, "the store player script is missing the player UI");
    const std::string patched_ui = doomnet::patched_player_ui_text();
    const std::string patched_engine = doomnet::patched_player_audio_engine_text();
    check(patched_engine.find("this.doomnetStreamLimiter = null") != std::string::npos &&
              patched_engine.find("AudioEngine.prototype.doomnetAttachStream") != std::string::npos &&
              patched_engine.find("AudioEngine.prototype.doomnetDetachStream") != std::string::npos,
          "the player engine has no detachable StreamTunes output");
    check(patched_engine.find("this.dryGain.connect(this.masterGain);") != std::string::npos &&
              patched_engine.find("this.wetGain.connect(this.masterGain);") != std::string::npos,
          "the detached StreamTunes output changed the default masterGain path");
    const size_t stream_attach_at = patched_engine.find("AudioEngine.prototype.doomnetAttachStream");
    const size_t stream_node_at = patched_engine.find("ctx.createMediaStreamDestination");
    check(stream_attach_at != std::string::npos && stream_node_at > stream_attach_at,
          "the StreamTunes destination is created before a host session attaches it");
    check(patched_engine.find("this.dryGain.disconnect(limiter)") != std::string::npos &&
              patched_engine.find("this.wetGain.disconnect(limiter)") != std::string::npos &&
              patched_engine.find("this.doomnetDetachStream();\n\t\tthis.doomnetDetachRemoteStream();\n\t\tthis.beginCommand();") != std::string::npos,
          "ending ownership does not detach the StreamTunes output");
    check(patched_engine.find("AudioEngine.prototype.doomnetAttachRemoteStream") != std::string::npos &&
              patched_engine.find("ctx.createMediaStreamSource(stream)") != std::string::npos &&
              patched_engine.find("this.doomnetRemoteGain.gain.value = this.volumeToGain(this.volume)") != std::string::npos &&
              patched_engine.find("AudioEngine.prototype.doomnetDetachRemoteStream") != std::string::npos,
          "the listener has no detachable receive graph with personal volume");
    check(store_player.find("doomnetAttachStream") != std::string::npos, "the store player still receives the unpatched audio engine");
    check(patched_ui.find("doomnetMountPartySlot") != std::string::npos &&
              patched_ui.find("Start Listening Party") != std::string::npos &&
              patched_ui.find("End Party") != std::string::npos &&
              patched_ui.find("Configure Party") != std::string::npos &&
              patched_ui.find("window.__doomnetParty") != std::string::npos &&
              patched_ui.find("window.__doomnetTabHost") != std::string::npos &&
              patched_ui.find("window.__doomnetPartyConfig") != std::string::npos &&
              patched_ui.find("new Client({") != std::string::npos &&
              patched_ui.find("Listening Party needs a relay address before it can start.") != std::string::npos &&
              patched_ui.find("getElementById(\"doomnet-steamtunes-host\")") != std::string::npos &&
              patched_ui.find("!service || !window.__doomnetTabHost") == std::string::npos,
          "the DoomNet library row has no guarded listening-party controls");
    {
        std::string already_patched =
            "\t\tdoomnetMountPartySlot: function (row) {\n"
            "\t\t\tvar service = UI.doomnetPartyService();\n"
            "\t\t\tif (!row || !service || !window.__doomnetTabHost) {\n"
            "\t\t\t\treturn;\n"
            "\t\t\t}\n"
            "\t\t\tUI.doomnetBindParty();\n"
            "\t\t\tvar state = UI.doomnetPartySnapshot();\n"
            "\t\t\tif (!state.active) {\n"
            "\t\t\t\tbutton(\"Start Listening Party\", \"\", function () { UI.doomnetOpenParty(true); }, true);\n";
        doomnet::repair_party_buttons(already_patched);
        check(already_patched.find("!service || !window.__doomnetTabHost") == std::string::npos && already_patched.find("getElementById(\"doomnet-steamtunes-host\")") != std::string::npos && already_patched.find("!service) {") != std::string::npos, "an already patched player page still hides Start Listening Party");
    }
    {
        const std::wstring installed = L"C:\\Users\\Pablo\\AppData\\Local\\DoomNet\\player\\backend\\assets\\frontend\\steam-music-player.js";
        std::string page;
        std::wstring read_error;
        if (doomnet::read_entire_file(installed, page, read_error)) {
            doomnet::repair_party_buttons(page);
            check(page.find("!service || !window.__doomnetTabHost") == std::string::npos, "the installed player page still hides Start Listening Party");
            check(doomnet::write_new_file(L"C:\\Users\\Pablo\\AppData\\Local\\Temp\\doomnet-player-fixed.js", page.data(), page.size(), read_error), "the repaired player page was not written");
        }
    }
    check(patched_ui.find("button(\"Join Party\", \"\", function () { UI.doomnetOpenParty(false); }, true);") != std::string::npos &&
              patched_ui.find("placeholder: \"Enter a party code\"") != std::string::npos &&
              patched_ui.find("Type the code from the host") != std::string::npos,
          "a listener has no place to type a party code");
    check(patched_ui.find("var leavingPartyPage = UI.doomnetPartyPage;") != std::string::npos &&
              patched_ui.find("UI.searchQuery = value;\n\t\t\tUI.doomnetPartyPage = false;") != std::string::npos &&
              patched_ui.find("var partyOnScreen = UI.doomnetPartyPage || (listening && UI.libraryGroupBy === \"nowplaying\" && !UI.libraryDrill);") != std::string::npos,
          "a library tab or a search stays behind the party page");
    check(patched_ui.find("value = App.engine.doomnetAttachStream();") != std::string::npos &&
              patched_ui.find("App.engine.doomnetAttachRemoteStream(stream);") != std::string::npos &&
              patched_ui.find("App.engine.doomnetDetachRemoteStream();") != std::string::npos,
          "the party portal is not connected to the tested host and listener audio paths");
    check(patched_ui.find("text: \"End Session\"") != std::string::npos &&
              patched_ui.find("end.disabled = !(state.active && state.role === \"host\")") != std::string::npos &&
              patched_ui.find("state.count + \"/\" + state.capacity") != std::string::npos &&
              patched_ui.find("state.pingMs + \" ms to host\"") != std::string::npos &&
              patched_ui.find("smp-track-list doomnet-party-roster") != std::string::npos,
          "the Listening Party page does not harden End Session or show its room status");
    check(patched_ui.find("doomnetListenerParty") != std::string::npos &&
              patched_ui.find("return \"Listening Party\"") != std::string::npos &&
              patched_ui.find("text: \"Leave Session\"") != std::string::npos &&
              patched_ui.find("UI.doomnetRenderParty(pane)") != std::string::npos &&
              patched_ui.find("doomnetRestoreLocalPlayback") != std::string::npos &&
              patched_ui.find("engine.play()") != std::string::npos &&
              patched_ui.find("state.role === \"listener\"") != std::string::npos &&
              patched_ui.find("btn.disabled = enabled === false") != std::string::npos &&
              patched_ui.find("Leave the current party before starting one") != std::string::npos &&
              patched_ui.find("startHost && UI.doomnetListenerParty()") != std::string::npos,
          "a listener session does not replace Now Playing or restore the local player on Leave Session");
    check(patched_ui.find("var PLUGIN_VERSION = \"1.17.1\"") != std::string::npos && patched_ui.find("var PLUGIN_VERSION = \"1.15.0\"") == std::string::npos && patched_ui.find("Start Listening Party") != std::string::npos && patched_ui.find("tagCount") != std::string::npos, "the About version is still 1.17.0, or it does not include the listening party or the scan");
    {
        const std::string stock_sync =
            "local function sync_assets_to_steamui()\n"
            "\tlocal destDir = fs.join(steamui_dir(), INJECT_SUBDIR)\n";
        const std::string kept = doomnet::patched_player_module("main", stock_sync);
        const size_t stop_copy = kept.find("The original 1.15.0 files were not copied over it.");
        const size_t copy_again = kept.find("local destDir");
        const size_t guarded = kept.find("if true then");
        const size_t returned = kept.find("return");
        const size_t closed = kept.find("end");
        check(stop_copy != std::string::npos && guarded != std::string::npos && returned != std::string::npos && closed != std::string::npos && copy_again != std::string::npos && stop_copy < guarded && guarded < returned && returned < closed && closed < copy_again, "the player still copies version 1.15.0 over the 1.16.0 page");
    }
    check(doomnet::patched_player_bootstrap_text().find("var ASSET_REV = \"20261112-version-1.17.1-skin-portal\"") != std::string::npos && doomnet::patched_player_bootstrap_text().find("20261027-version-1.15.0") == std::string::npos, "the player files are still cached as version 1.15.0");
    check(patched_ui.find("settingsTabList.push({ key: \"store\", label: \"Store\" })") != std::string::npos &&
              patched_ui.find("UI.doomnetRenderStore(view)") != std::string::npos &&
              patched_ui.find("doomnet_install_app") != std::string::npos &&
              patched_ui.find("app.id === \"steamskin\"") != std::string::npos &&
              patched_ui.find("button.disabled = true") != std::string::npos &&
              patched_ui.find("SteamSkin is already installed") != std::string::npos &&
              patched_ui.find("Configure changes SteamSkin. The music player's own color stays as it is.") != std::string::npos &&
              patched_ui.find("doomnet-store-configure") != std::string::npos &&
              patched_ui.find("text: \"Configure\"") != std::string::npos &&
              patched_ui.find("doomnet_set_skin_color") != std::string::npos &&
              patched_ui.find("doomnet-skin-appearance") != std::string::npos &&
              patched_ui.find("The music player was left as it is.") != std::string::npos &&
              patched_ui.find("UI.commitSetting(\"uiColorTheme\", theme, false);") == std::string::npos &&
              patched_ui.find("Check for Updates") != std::string::npos &&
              patched_ui.find("doomnet_check_updates") != std::string::npos &&
              patched_ui.find("doomnet_update_notice") != std::string::npos &&
              patched_ui.find("doomnet-update-notice") != std::string::npos &&
              doomnet::patched_player_css_text().find(".doomnet-update-notice{") != std::string::npos,
          "Settings has no DoomNet Store tab");
    check(patched_ui.find("pick_music_folder") != std::string::npos && patched_ui.find("and the folders inside it") != std::string::npos,
          "Add Folder still asks the page for a path, so the chosen music folder is never saved");
    check(patched_ui.find("UI.scanNewOnly = true;") != std::string::npos && patched_ui.find("}, false, true);") != std::string::npos,
          "adding a folder still starts a full rescan of songs already in the library");
    check(patched_ui.find("Looking for new songs and playlist files") != std::string::npos && patched_ui.find("!batchInfo.newOnly") != std::string::npos && patched_ui.find("playlist files.") != std::string::npos,
          "Scan For New Tracks still looks only for new audio files");
    check(patched_ui.find("Scan For New Files") != std::string::npos && patched_ui.find("Scan For New Tracks") == std::string::npos,
          "the scan button is still named Scan For New Tracks");
    check(patched_ui.find("UI.artProbeGen = Date.now();") != std::string::npos && patched_ui.find("setTimeout(pollArt, 500)") != std::string::npos && patched_ui.find("setTimeout(pollArt, 2000)") == std::string::npos,
          "covers already on screen do not update while artwork is found");
    {
        const size_t restart_button = patched_ui.find("section.appendChild(restartBtn);");
        const size_t restart_help = patched_ui.find("use this instead of restarting Steam.\\n");
        const size_t redraw_button = patched_ui.find("section.appendChild(continueBtn);");
        const size_t redraw_help = patched_ui.find("If covers stop showing after you browse around");
        const size_t art_button = patched_ui.find("section.appendChild(artBtn);");
        const size_t art_help = patched_ui.find("This reads the picture stored inside songs already in the library.");
        const size_t art_status = patched_ui.find("section.appendChild(artStatus);");
        check(restart_button != std::string::npos && restart_help != std::string::npos && restart_button < restart_help && redraw_button != std::string::npos && redraw_help != std::string::npos && redraw_button < redraw_help && art_button != std::string::npos && art_help != std::string::npos && art_status != std::string::npos && art_button < art_help && art_help < art_status,
              "troubleshooting help still sits above its button");
    }
    check(doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-view-settings{padding-left:16px;") != std::string::npos && doomnet::patched_player_css_text().find(".smp-view-settings .smp-collapse-heading{display:flex;width:100%;}") != std::string::npos && doomnet::patched_player_css_text().find(".smp-view-settings .smp-settings-section>.smp-btn{display:block;width:fit-content;margin-top:28px;}") != std::string::npos,
          "settings still sit on the left edge, Restart App still sits beside the heading, or the troubleshooting buttons are still packed together");
    {
        std::string library_source;
        std::wstring library_error;
        const bool library_read = doomnet::read_entire_file(L"D:\\Documents\\Repos\\SteamTunes\\backend\\library.lua", library_source, library_error);
        check(library_read, "library.lua was not read");
        if (library_read) {
            const std::string patched = doomnet::patched_player_module("library", library_source);
            check(patched.find("entry.is_directory") != std::string::npos && patched.find("and every folder inside it") != std::string::npos && patched.find("not entry.is_symlink") != std::string::npos && patched.find("not entry.is_file") == std::string::npos,
                  "the scan still does not walk folders inside the music folder");
            check(patched.find("return library.regenerate_artwork()") != std::string::npos && patched.find("$probe = Read-Bytes $audioPath 10") != std::string::npos,
                  "Regenerate Artwork still clears the cache and reads more than the picture stored in each song");
            check(patched.find("STEAMUI") != std::string::npos && patched.find("SHOW") != std::string::npos && patched.find("sample = track") != std::string::npos && patched.find("64 * 1024 * 1024") != std::string::npos,
                  "Regenerate Artwork still does not put a found picture on the library view");
            check(patched.find("doomnetAddedFolder = path") != std::string::npos && patched.find("listJob.addedOnly") != std::string::npos && patched.find("Songs already in the library are skipped.") != std::string::npos,
                  "adding a folder still scans songs that are already in the library");
            check(patched.find("result.playlistsUpdated = updated") != std::string::npos && patched.find("if listed then") != std::string::npos,
                  "Scan For New Tracks still finishes without importing playlist files");
        }
        std::string main_source;
        std::wstring main_error;
        const bool main_read = doomnet::read_entire_file(L"D:\\Documents\\Repos\\SteamTunes\\backend\\main.lua", main_source, main_error);
        check(main_read, "main.lua was not read");
        if (main_read) {
            const std::string patched_main = doomnet::patched_player_module("main", main_source);
            check(patched_main.find("folder = result.folder") != std::string::npos, "a scan result still omits the music folder, so the page cannot name it");
            check(patched_main.find("doomnet_stream_hook") != std::string::npos && patched_main.find("url_encode_segment(tostring(track.id))") != std::string::npos,
                  "a play still copies the song into steamui");
        }
    }
    check(doomnet::patched_player_css_text().find(".doomnet-party-slot{display:flex;align-items:center;gap:4px;margin-left:auto;margin-right:8px;}") != std::string::npos &&
              doomnet::patched_player_css_text().find(".doomnet-party-slot + .smp-art-size{margin-left:0;}") != std::string::npos &&
              doomnet::patched_player_css_text().find(".doomnet-party-button{background:var(--smp-accent,#e35d5d)") != std::string::npos &&
              doomnet::patched_player_css_text().find(".doomnet-party-code{") != std::string::npos &&
              doomnet::patched_player_css_text().find("color:var(--smp-accent)") != std::string::npos,
          "the listening-party controls are not aligned with Now Playing or the code does not use the accent");
    check(patched_ui.find("get_album_art_b64") != std::string::npos, "the player UI was not given the cover file fallback");
    check(patched_ui.find("This reads the picture stored inside songs already in the library.") != std::string::npos &&
              patched_ui.find("Clear every cached cover") == std::string::npos &&
              patched_ui.find("Clearing...") == std::string::npos,
          "Regenerate Artwork still asks to clear every cover before it reads pictures");
    check(patched_ui.find("url.indexOf(\"data:\") !== 0 && !artUrlMatchesAlbum(album, url)") != std::string::npos, "a cover already read as bytes is not painted on every tile");
    check(patched_ui.find("incomingRev <= (UI.pointerBusyRev || 0)") != std::string::npos && patched_ui.find("UI.pointerRevFloor") != std::string::npos, "a late page reply can still overwrite the page that was just opened");
    check(patched_ui.find("albumArtist: group.artist") != std::string::npos, "Album and Genre covers are not looked up by the album artist the way Artist covers are");
    check(patched_ui.find("doomnetMountArtSize") != std::string::npos && patched_ui.find("count: 12, size: \"smd\"") != std::string::npos && patched_ui.find("count: 8, size: \"mdl\"") != std::string::npos && patched_ui.find("UI.doomnetArtCols || 14") != std::string::npos && doomnet::patched_player_css_text().find(".smp-art-size-sm{width:10px;height:10px;}") != std::string::npos && doomnet::patched_player_css_text().find(".smp-art-size-smd{width:12px;height:12px;}") != std::string::npos && doomnet::patched_player_css_text().find(".smp-art-size-md{width:14px;height:14px;}") != std::string::npos && doomnet::patched_player_css_text().find(".smp-art-size-mdl{width:16px;height:16px;}") != std::string::npos && doomnet::patched_player_css_text().find(".smp-art-size-lg{width:18px;height:18px;}") != std::string::npos, "the library artwork control is not five ascending squares");
    check(patched_ui.find("wantCols = 4") != std::string::npos && patched_ui.find("coverTrackId != null && !document.documentElement.classList.contains(\"smp-ctx-overlay\")") != std::string::npos, "the mini player still shows small covers and a Set Cover button");
    check(patched_ui.find("html.smp-ctx-overlay .smp-launcher,html.smp-ctx-overlay .smp-overlay-widget{bottom:4.5vh !important}") != std::string::npos && patched_ui.find("html.smp-ctx-overlay .smp-panel{bottom:calc(4.5vh + 64px) !important}") != std::string::npos && doomnet::patched_player_css_text().find("html.smp-ctx-overlay .smp-launcher,html.smp-ctx-overlay .smp-overlay-widget{bottom:4.5vh !important;}") != std::string::npos && doomnet::patched_player_css_text().find("html.smp-ctx-overlay .smp-panel{bottom:calc(4.5vh + 64px) !important;}") != std::string::npos, "the overlay mini player still sits on the bottom edge");
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
    check(page.find("DoomNetStreamTunes") != std::string::npos &&
              page.find("StreamTunesClient") != std::string::npos &&
              page.find("RTCPeerConnection") != std::string::npos,
          "the page script does not carry the tested StreamTunes WebRTC client");
    doomnet::set_boot_preamble("");
    check(patched_ui.find("doomnetThumbUrl") != std::string::npos && patched_ui.find("/steam-music-player/art/thumbs/") != std::string::npos, "the grids still load full-size covers instead of thumbnails");
    check(patched_ui.find("doomnetPageWarmDisabled") != std::string::npos && patched_ui.find("doomnetWarmThumbs: function () {\n\t\t\treturn;") != std::string::npos, "the page still prefetches every cover and can hang Steam's web helper");
    check(doomnet::patched_player_css_text().find("@media (min-width:1920px) and (min-height:1000px){") != std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-nptab-hero{flex-direction:column;align-items:center;justify-content:center;gap:22px;width:min(480px,92%);margin:0 auto;}") != std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-nptab-hero{flex-direction:row;") == std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-nptab-eyebrow,#doomnet-steamtunes-host .smp-nptab-title,#doomnet-steamtunes-host .smp-nptab-artist,#doomnet-steamtunes-host .smp-nptab-album,#doomnet-steamtunes-host .smp-nptab-modes{order:0;}") != std::string::npos, "a smaller window still puts the song title beside the cover");
    check(patched_ui.find("doomnetMountNpTransport") != std::string::npos && patched_ui.find("doomnet-np-cluster") != std::string::npos && patched_ui.find("UI.doomnetSyncNpTransport();\n\t\t\t\tif (!bar || UI.scrubbing)") != std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-panel:has(.smp-nptab) > .smp-nowplaying{display:none !important;}") != std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-nptab-modes{display:flex;align-items:center;justify-content:center;width:100%;gap:6px;}") != std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .doomnet-np-seekline .smp-seek{flex:0 1 220px;width:220px;min-width:0;max-width:220px;}") != std::string::npos, "Now Playing leaves the transport in the bottom bar");
    check(patched_ui.find("doomnetEnsureNpOrient") != std::string::npos && patched_ui.find("smp-np-orient") != std::string::npos && patched_ui.find("Place the song beside the cover") != std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host.doomnet-np-horizontal .smp-nptab-hero{flex-direction:row;") != std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host.doomnet-np-horizontal .doomnet-np-cluster,#doomnet-steamtunes-host.doomnet-np-horizontal .doomnet-np-seekline{display:none !important;}") != std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host.doomnet-np-horizontal .smp-panel:has(.smp-nptab) > .smp-nowplaying{display:flex !important;}") != std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-view-library:has(.smp-nptab) > .doomnet-np-orient{position:absolute;right:16px;bottom:16px;") != std::string::npos, "Now Playing cannot switch the cover back beside the song");
    check(patched_ui.find("parkedHorizontal") != std::string::npos && patched_ui.find("hostEl.classList.remove(\"doomnet-np-horizontal\")") != std::string::npos && patched_ui.find("var artBox = art ? art.getBoundingClientRect()") != std::string::npos, "the side-by-side song moves the equalizer and the lyrics");
    check(patched_ui.find("doomnetCenterNpBlock") != std::string::npos && patched_ui.find("doomnetWatchNpBlock") != std::string::npos && patched_ui.find("new ResizeObserver") != std::string::npos && patched_ui.find("doomnetSlideNpBlock") != std::string::npos && patched_ui.find("(eqBox.right + lyricBox.left) / 2") != std::string::npos && patched_ui.find("queue.style.transform = slide") != std::string::npos && patched_ui.find("horizontal ? 50 : 0") != std::string::npos && patched_ui.find("info.getBoundingClientRect().width + 100") == std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host.doomnet-np-horizontal .smp-nptab-info{width:max-content;flex:0 0 auto;max-width:460px;box-sizing:content-box;padding:0 50px;}") != std::string::npos, "the side-by-side cover, song, and list are shifted instead of centered");
    check(patched_ui.find("var edge = Math.max(16, Math.min(100, avail - 150));") != std::string::npos && patched_ui.find("eq.style.left = edge + \"px\";") != std::string::npos && patched_ui.find("var meterLeft = hostBox.width - edge - meterWidth;") != std::string::npos && patched_ui.find("var meterWidth = eqWidth;") != std::string::npos, "the equalizer and the lyrics are not placed 100 pixels from the sides");
    check(patched_ui.find("eq.querySelector(\".doomnet-eq-meter\")") != std::string::npos && patched_ui.find("underBox.bottom + 24 - contentEdge") != std::string::npos && patched_ui.find("wrap.style.paddingBottom = hang > 36 ? hang + \"px\" : \"\"") != std::string::npos, "a short playlist cuts off the equalizer");
    check(doomnet::patched_player_css_text().find(".smp-panel:has(.smp-view-library.active) > .smp-scan-banner{display:none !important;}") != std::string::npos, "the library scan bar shows outside Settings");
    check(patched_ui.find("UI.doomnetMountNowPlayingFlanks(wrap)") != std::string::npos && patched_ui.find("App.settings.eqPreset = \"custom\"") != std::string::npos && patched_ui.find("engine.eqFilters") != std::string::npos, "the Now Playing EQ does not show the active preset or switch to Custom");
    check(patched_ui.find("doomnetChooseEqPreset") != std::string::npos && patched_ui.find("doomnetToggleEqMenu") != std::string::npos && patched_ui.find("doomnetCloseEqMenu") != std::string::npos && patched_ui.find("menu.hidden = true") != std::string::npos && doomnet::patched_player_css_text().find(".doomnet-eq-caret{width:0;height:0;border-left:4px solid transparent;border-right:4px solid transparent;border-top:5px solid var(--smp-accent,#66c0f4);}") != std::string::npos, "the Now Playing preset name does not open a list that closes after a choice");
    check(patched_ui.find("engine.masterGain.connect(node)") != std::string::npos && patched_ui.find("doomnet-spectrum") != std::string::npos && patched_ui.find("getByteTimeDomainData") != std::string::npos && patched_ui.find("UI._doomnetMeterAt") != std::string::npos && patched_ui.find("nowMs - UI._doomnetMeterAt >= 250") != std::string::npos && patched_ui.find("doomnet-eq-meter") != std::string::npos && patched_ui.find("doomnet-lyrics") != std::string::npos && patched_ui.find("get_track_lyrics") != std::string::npos && patched_ui.find("body._doomnetLyricAnim") != std::string::npos && patched_ui.find("iterations: Infinity") != std::string::npos && patched_ui.find("UI._doomnetLyricPayload") != std::string::npos && patched_ui.find("doomnet-lyrics-label") != std::string::npos && patched_ui.find("preset.getBoundingClientRect().top - eq.getBoundingClientRect().top") != std::string::npos && doomnet::patched_player_css_text().find(".doomnet-lyrics-label{flex:0 0 auto;") != std::string::npos, "the Now Playing page does not draw a spectrum under the EQ or scrolling lyrics");
    check(patched_ui.find("getPropertyValue(\"--smp-accent\")") != std::string::npos && patched_ui.find("rgba(227, 93, 93") == std::string::npos && patched_ui.find("doomnetMountMixBars") != std::string::npos && patched_ui.find("doomnetRefreshMixBars") != std::string::npos && patched_ui.find("return v + \" LUFS\";") != std::string::npos && doomnet::patched_player_css_text().find(".doomnet-mix{width:100%;max-width:100%;min-width:0;margin-top:calc(10px + 2.4em);}") != std::string::npos && doomnet::patched_player_css_text().find(".doomnet-mix-label{display:block;margin:0 66px 2px 0;text-align:center;") != std::string::npos && doomnet::patched_player_css_text().find(".doomnet-mix-value{width:58px;flex:0 0 58px;") != std::string::npos && doomnet::patched_player_css_text().find(".doomnet-mix-slider{-webkit-appearance:none;appearance:none;flex:1 1 0;width:auto;min-width:0;") != std::string::npos && doomnet::patched_player_css_text().find(".doomnet-mix-slider::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:10px;height:10px;") != std::string::npos && doomnet::patched_player_css_text().find("background:var(--smp-accent,#66c0f4);box-shadow:0 0 0 2px rgba(0,0,0,0.45);cursor:pointer;}") != std::string::npos, "the spectrum bars stay red when the appearance accent changes");
    check(patched_ui.find("doomnetLayoutFlanks") != std::string::npos && patched_ui.find("tall ? 100 : 250") == std::string::npos && patched_ui.find("eqNudge") == std::string::npos && patched_ui.find("matchMedia(\"(min-width: 1920px)") == std::string::npos && patched_ui.find("fader - 88") == std::string::npos && patched_ui.find("doomnet-flanks-inline") != std::string::npos && doomnet::patched_player_css_text().find("height:var(--doomnet-fader,120px)") != std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .doomnet-flank-row:not(.doomnet-flanks-inline) .doomnet-eq-meter{position:absolute;left:0;right:0;top:100%;margin-top:12px;}") != std::string::npos, "the Now Playing EQ and meter stay a fixed size and hide on smaller screens");
    check(patched_ui.find("doomnet-flank-eye") != std::string::npos && patched_ui.find("smp-flanks-shown") != std::string::npos && patched_ui.find("doomnet-flanks-off") != std::string::npos && patched_ui.find("Hide equalizer and lyrics") != std::string::npos && doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .doomnet-flank-eye{position:absolute;top:8px;left:8px;") != std::string::npos && doomnet::patched_player_css_text().find(".smp-nptab.doomnet-flanks-off .doomnet-flank-row{display:none;}") != std::string::npos, "Now Playing has no eye to hide the equalizer and lyrics");
    check(patched_ui.find("callServer(\"get_music_folders\").then(function (folders)") != std::string::npos && patched_ui.find("UI.applySettingsToEngine();") != std::string::npos, "the overlay waits on the full queue before it shows the saved settings");
    check(patched_ui.find("cacheReady.then(function (cached)") != std::string::npos, "the overlay stays on Loading library until the server answers");
    check(patched_ui.find("text: \"Add to playlist\"") != std::string::npos && patched_ui.find("callServer(\"playlist_add_track\"") != std::string::npos && patched_ui.find("return playlist && !playlist.sourcePath;") != std::string::npos && patched_ui.find("callServer(\"playlist_remove_track\"") != std::string::npos && patched_ui.find("mode === \"playlistTracks\"") != std::string::npos && patched_ui.find("document.body.appendChild(menu)") != std::string::npos && patched_ui.find("rect.top - menuRect.height") != std::string::npos && patched_ui.find("role: \"menuitem\"") != std::string::npos && patched_ui.find("placed.top") != std::string::npos, "a song cannot be added to a playlist you created, and the playlist list does not open as a menu on the button");
    {
        const std::string album_button = "var addAlbumPlaylistBtn";
        size_t album_buttons = 0;
        for (size_t at = 0; (at = patched_ui.find(album_button, at)) != std::string::npos; at += album_button.size()) {
            album_buttons += 1;
        }
        check(album_buttons == 1, "Add to playlist is added again on the album page every time Steam starts");
        check(patched_ui.find("SCAN_MAX_RESUME_ATTEMPTS: 40") != std::string::npos && patched_ui.find("return resume(err, resumesLeft, pollBatch)") == std::string::npos && patched_ui.find("the scan stopped before the library was saved") != std::string::npos && patched_ui.find("return resume(err, resumesLeft, startOrPoll)") != std::string::npos, "a large library scan still gives up when one batch fails");
        std::string repeated = patched_ui.substr(patched_ui.find(album_button));
        const size_t block_end = repeated.find("actions.appendChild(addAlbumPlaylistBtn);\n");
        check(block_end != std::string::npos, "the album Add to playlist button was not found");
        if (block_end != std::string::npos) {
            const std::string block = repeated.substr(0, block_end + std::string("actions.appendChild(addAlbumPlaylistBtn);\n").size());
            std::string four = block + block + block + block;
            doomnet::keep_one_repeated_block(four, block);
            check(four == block, "four album Add to playlist buttons were not folded back to one");
        }
    }
    check(patched_ui.find("doomnetConfirmPlaylist") != std::string::npos && patched_ui.find("The playlist file stays where it is.") != std::string::npos && patched_ui.find("dismiss_playlist") != std::string::npos && patched_ui.find("text: playlist.sourcePath ? \"Remove\" : \"Delete\"") != std::string::npos, "a playlist read from a file has no confirmation before it is removed from the player");
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
        check(doomnet::patched_player_module("settings", "\tmediaKeysEnabled = true,\n").find("globalControl = false") != std::string::npos, "Global Control is not a saved setting");
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
        const std::string scan_count =
            "\t\tprocessed = processed,\n"
            "\t\tremaining = remaining,\n"
            "\t\ttotalFiles = scanState.totalFiles or (listJob.results and #listJob.results) or 0,\n"
            "\t\ttotalTracks = totalTracks,\n"
            "\t\tpendingCount = remaining,\n"
            "local function find_folder_art(trackPath, albumName)\n"
            "\tif not is_library_root(dir) then\n"
            "\t\tlocal okList, entries = pcall(fs.list, dir)\n"
            "\t\tif okList and type(entries) == \"table\" then\n"
            "\t\t\tlocal fallback = nil\n"
            "\t\t\tfor _, entry in ipairs(entries) do\n"
            "\t\t\t\tlocal name = tostring(entry):match(\"[^/\\\\]+$\") or tostring(entry)\n"
            "\t\t\t\tif lower:match(\"^albumart.*_large%.jpe?g$\") or lower:match(\"^albumart.*large%.jpe?g$\") then\n"
            "\t\t\t\t\treturn fs.join(dir, name)\n"
            "\t\t\t\tend\n"
            "\t\t\tif fallback then\n"
            "\t\t\t\treturn fallback\n"
            "\t\t\tend\n"
            "\t\tend\n"
            "\tend\n"
            "\treturn nil\n"
            "end\n";
        const std::string scan_progress = doomnet::patched_player_module("library", scan_count);
        check(scan_progress.find("pendingCount = #scanState.pending") != std::string::npos && scan_progress.find("processed = math.max(0, scanState.pendingIndex - 1)") != std::string::npos && scan_progress.find("folderArtByDir[dir]") != std::string::npos, "the scan count stays at 0 while more local files are found");
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
        check(doomnet::patched_player_module("playlists", playlist_sync).find("playlist_file.read_all") != std::string::npos && doomnet::patched_player_module("playlists", playlist_sync).find("playlists._hidden") != std::string::npos, "one iTunes library file cannot provide more than one playlist");
        const std::string playlist_delete =
            "function playlists.delete(id)\n"
            "\tlocal playlist = state.byId[id]\n"
            "\tif not playlist or playlist.sourcePath then\n"
            "\t\treturn false\n"
            "\tend\n"
            "\tstate.byId[id] = nil\n"
            "\tpersist()\n"
            "\treturn true\n"
            "end\n";
        check(doomnet::patched_player_module("playlists", playlist_delete).find("function playlists.dismiss") != std::string::npos && doomnet::patched_player_module("playlists", playlist_delete).find("playlist_hidden.json") != std::string::npos, "a playlist read from a file cannot be removed from the player");
        const std::string playlist_main =
            "function get_playlists()\n"
            "\treturn json.encode(playlists.get_all())\n"
            "end\n";
        check(doomnet::patched_player_module("main", playlist_main).find("function dismiss_playlist") != std::string::npos, "the page cannot ask the player to remove a playlist that came from a file");
        const std::string playlist_walk = "\tif ext ~= \"m3u\" and ext ~= \"m3u8\" and ext ~= \"pls\" then\n\t\treturn\n\tend\n$plexts = @{'.m3u'=$true;'.m3u8'=$true;'.pls'=$true}\nelseif file_extension(path) == \"m3u\" or file_extension(path) == \"m3u8\" or file_extension(path) == \"pls\" then\nelseif ext == \"m3u\" or ext == \"m3u8\" or ext == \"pls\" then\n";
        const std::string walked = doomnet::patched_player_module("library", playlist_walk);
        check(walked.find(".xspf") != std::string::npos && walked.find(".plist") != std::string::npos && walked.find("note_playlist_file(path)") != std::string::npos, "the library scan does not look for XSPF, WPL, ASX, or iTunes playlist files");
    }
    check(patched_ui.find("!/[A-Za-z0-9]/.test(String(album))") != std::string::npos && patched_ui.find("new TextEncoder().encode(hashLabel)") != std::string::npos, "albums with no English letters still share one cover file");
    check(doomnet::shared_poll_call("get_duck_envelope") && doomnet::shared_poll_call("get_player_state") && doomnet::shared_poll_call("get_player_state_full") && !doomnet::shared_poll_call("stage_track_for_playback") && doomnet::audio_load_call("stage_track_for_playback"), "a play call waits behind the duck poll and the player-state poll");
    check(doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .doomnet-eq-slider::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:8px;height:8px;") != std::string::npos && doomnet::patched_player_css_text().find("background:var(--smp-accent,#66c0f4);") != std::string::npos && doomnet::patched_player_css_text().find("rotate(-90deg)") != std::string::npos, "the Now Playing EQ still uses the browser's blue slider");
    check(patched_ui.find("UI.doomnetPaintEqSlider(slider)") != std::string::npos && doomnet::patched_player_css_text().find(".smp-eq-fader-input::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:8px;height:8px;") != std::string::npos, "the settings equalizer still uses the browser's slider");
    check(doomnet::patched_player_css_text().find(".smp-settings-slider{flex:0 1 240px;width:240px;max-width:240px;") != std::string::npos && doomnet::patched_player_css_text().find(".smp-select{flex:0 1 360px;width:360px;max-width:min(360px,100%);}") != std::string::npos && doomnet::patched_player_css_text().find(".smp-settings-readout{width:36ch;min-width:36ch;max-width:36ch;flex:0 0 36ch;") != std::string::npos && doomnet::patched_player_css_text().find("html.smp-ctx-overlay .smp-settings-slider{flex:1 1 auto;width:auto;min-width:0;max-width:none;}") != std::string::npos && doomnet::patched_player_css_text().find("html.smp-ctx-overlay .smp-settings-label,html.smp-ctx-overlay .smp-setting-block .smp-settings-label{flex:0 0 168px;width:168px;min-width:168px;}") != std::string::npos && doomnet::patched_player_css_text().find("html.smp-ctx-overlay .smp-select{flex:1 1 auto;width:auto;min-width:0;max-width:none;box-sizing:border-box;}") != std::string::npos, "settings sliders and dropdowns still stretch across the page");
    check(doomnet::patched_player_css_text().find(".smp-folder-path{flex:0 1 auto;min-width:0;}") != std::string::npos, "the Music folders Remove button still stretches to the far edge");
    check(doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-nptab-queue{width:min(740px,100%);max-width:none;margin:28px auto 0;}") != std::string::npos, "the Now Playing track list is not centered");
    check(doomnet::patched_player_css_text().find("#doomnet-steamtunes-host .smp-nptab-art-wrap{width:min(440px,100%);") != std::string::npos, "the Now Playing cover is still the small size");
    check(doomnet::patched_player_ui_text().find("line._smpMarqueeTries = 0") != std::string::npos, "a title still scrolls when the whole line is already visible");
    check(doomnet::patched_player_ui_text().find("command.action === \"prev\" && UI.playbackProgress().elapsed > 5") != std::string::npos && doomnet::patched_player_ui_text().find("command.action = \"seek\"") != std::string::npos, "Previous still skips to the prior song after the first five seconds");
    {
        std::string hook_source;
        std::wstring hook_error;
        check(doomnet::read_entire_file(L"D:\\Documents\\Repos\\DoomNet-SteamPortal\\src\\global_hotkeys.cpp", hook_source, hook_error) && hook_source.find("doomnet-key-") != std::string::npos && hook_source.find("kTransportQuietMs = 500") != std::string::npos && hook_source.find("flush_volume(false)") != std::string::npos && hook_source.find("merge_queued_volume") != std::string::npos, "a global key press is not given its own id, so the player can apply it twice");
        std::string bus_source;
        check(doomnet::read_entire_file(L"D:\\Documents\\Repos\\DoomNet-SteamPortal\\src\\host_bus.h", bus_source, hook_error) && bus_source.find("reversed ? delta : current + delta") != std::string::npos, "rolling the volume wheel the other way keeps the unapplied steps from the previous direction");
    }
    check(doomnet::patched_player_ui_text().find("\"globalControl\"") != std::string::npos && doomnet::patched_player_ui_text().find("Global Control") != std::string::npos && doomnet::patched_player_ui_text().find("case \"volumeBump\":") != std::string::npos && doomnet::patched_player_ui_text().find("followWheelVolume") != std::string::npos && doomnet::patched_player_ui_text().find("requestAnimationFrame(step)") != std::string::npos && doomnet::patched_player_ui_text().find("!mediaKeysAllowed() || settingFlag(App.settings.globalControl, false)") != std::string::npos && doomnet::patched_player_ui_text().find("UI.commitSetting(\"globalControl\", false, false)") != std::string::npos && doomnet::patched_player_css_text().find(".smp-feature-block.is-locked{opacity:0.45;}") != std::string::npos, "media keys still require Steam to be in front, and a volume key still changes the Windows volume");
    {
        const std::string settings_page = doomnet::patched_player_ui_text();
        const std::string call = "UI.appendGlobalControlBlock(section);";
        const size_t first_call = settings_page.find(call);
        check(first_call != std::string::npos && settings_page.find(call, first_call + call.size()) == std::string::npos, "Global Control is added more than once");
        std::string doubled = call + "\n" + call + "\n" + call + "\n";
        doomnet::keep_one_repeated_block(doubled, call + "\n");
        check(doubled == call + "\n", "a repeated settings block is not folded back to one");
        const std::string marker = "\t\tappendGlobalControlBlock: function (section) {\n";
        std::string separated =
            "doomnetRenderStore: function (view) {\n\treturn;\n},\n" +
            marker + "\t\treturn;\n\t},\n" +
            "doomnetRenderStore: function (view) {\n\ttext: \"Check for Updates\"\n},\n" +
            marker + "\t\treturn;\n\t},\n";
        doomnet::keep_one_repeated_block(separated, marker);
        const size_t kept_control = separated.find(marker);
        check(separated.find("Check for Updates") != std::string::npos && kept_control != std::string::npos && separated.find(marker, kept_control + marker.size()) == std::string::npos, "removing a repeated Global Control block also removes Check for Updates");
    }
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

void test_stream_file() {
    wchar_t saved_root[32768] = {};
    const DWORD saved_n = GetEnvironmentVariableW(L"DOOMNET_ROOT", saved_root, 32768);
    const std::wstring dir = temp_dir();
    SetEnvironmentVariableW(L"DOOMNET_ROOT", dir.c_str());
    check(doomnet::ensure_directory(dir + L"\\player"), "player data folder");
    check(doomnet::ensure_directory(dir + L"\\player\\backend"), "player backend folder");
    check(doomnet::ensure_directory(dir + L"\\player\\backend\\data"), "player data folder");
    std::wstring error;
    const std::string token = "abcdef0123456789";
    check(doomnet::write_new_file(dir + L"\\player\\backend\\data\\media_token", token.data(), token.size(), error), "the playback token was not written");
    const std::wstring song = dir + L"\\song.mp3";
    const std::string bytes = "sound";
    check(doomnet::write_new_file(song, bytes.data(), bytes.size(), error), "the dummy song was not written");
    const std::string index = std::string("[{\"id\":\"track1\",\"path\":") + doomnet::json_quote(doomnet::wide_to_utf8(song)) + ",\"size\":5}]";
    check(doomnet::write_new_file(dir + L"\\player\\backend\\data\\library_index.json", index.data(), index.size(), error), "library_index.json was not written");
    const std::string url = "https://steamloopback.host/steam-music-player/stream/" + token + "/track1";
    const doomnet::StreamReply whole = doomnet::serve_stream_url(url);
    check(whole.status == 200 && whole.body == bytes && whole.content_type == "audio/mpeg", "a play did not return the original file");
    const doomnet::StreamReply ranged = doomnet::serve_stream_url(url, "bytes=1-3");
    check(ranged.status == 206 && ranged.body == "oun" && ranged.content_range == "bytes 1-3/5", "a range request did not return that slice");
    const doomnet::StreamReply missing = doomnet::serve_stream_url("https://steamloopback.host/steam-music-player/stream/" + token + "/missing");
    check(missing.status == 404, "a missing track id was served");
    const doomnet::StreamReply wrong = doomnet::serve_stream_url("https://steamloopback.host/steam-music-player/stream/abcdef0123456780/track1");
    check(wrong.status == 404, "a wrong playback token was served");
    const doomnet::StreamReply climb = doomnet::serve_stream_url("https://steamloopback.host/steam-music-player/stream/" + token + "/%2e%2e/%2e%2e/steamui/x");
    check(climb.status == 404, "an address containing .. was served");
    const doomnet::StreamReply drive = doomnet::serve_stream_url("https://steamloopback.host/steam-music-player/stream/" + token + "/C:");
    check(drive.status == 404, "a drive letter in the address was served");
    check(GetFileAttributesW((dir + L"\\steamui").c_str()) == INVALID_FILE_ATTRIBUTES, "serving the song created a steamui folder");
    if (saved_n > 0 && saved_n < 32768) {
        SetEnvironmentVariableW(L"DOOMNET_ROOT", saved_root);
    } else {
        SetEnvironmentVariableW(L"DOOMNET_ROOT", nullptr);
    }
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
    test_streamtunes_identity();
    test_update_gate();
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
    test_stream_file();
    test_playback_queue();
    test_instance();
    if (g_failed != 0) {
        std::fprintf(stderr, "----\n%d failure(s)\n", g_failed);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
