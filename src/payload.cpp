#include "payload.h"

#include "config.h"
#include "installer.h"
#include "update_gate.h"
#include "text.h"
#include "log.h"
#include "paths.h"
#include "steam_attach.h"
#include "steam_discover.h"
#include "wide_io.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

namespace doomnet {
namespace {

std::wstring lower_copy(std::wstring text) {
    for (wchar_t& ch : text) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return text;
}

std::wstring full_path(const std::wstring& path) {
    wchar_t buffer[32768] = {};
    const DWORD wrote = GetFullPathNameW(path.c_str(), 32768, buffer, nullptr);
    if (wrote == 0 || wrote >= 32768) {
        return path;
    }
    return buffer;
}

bool inside_directory(const std::wstring& path, const std::wstring& root) {
    std::wstring child = lower_copy(full_path(path));
    std::wstring parent = lower_copy(full_path(root));
    if (!parent.empty() && (parent.back() == L'\\' || parent.back() == L'/')) {
        parent.pop_back();
    }
    if (child == parent) {
        return true;
    }
    return child.size() > parent.size() && child.compare(0, parent.size(), parent) == 0 && (child[parent.size()] == L'\\' || child[parent.size()] == L'/');
}

bool copy_tree(const std::wstring& from, const std::wstring& to, int depth, int& files, std::wstring& error) {
    if (depth > 32) {
        error = L"Stopped copying '" + from + L"' because the folder is nested more than 32 levels.";
        log_event(L"error", L"payload", error.c_str());
        return false;
    }
    if (from.find(L"..") != std::wstring::npos || to.find(L"..") != std::wstring::npos) {
        error = L"Refusing to copy '" + from + L"' to '" + to + L"' because a path contains '..'.";
        log_event(L"error", L"payload", error.c_str());
        return false;
    }
    if (!ensure_directory(to)) {
        error = L"Could not create folder '" + to + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"payload", error.c_str());
        return false;
    }
    const std::wstring pattern = from + L"\\*";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        error = L"Could not list '" + from + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"payload", error.c_str());
        return false;
    }
    bool ok = true;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L".." || name == L".git") {
            continue;
        }
        const std::wstring source = from + L"\\" + name;
        const std::wstring dest = to + L"\\" + name;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            const std::wstring detail = L"Skipped reparse point '" + source + L"' so the copy cannot walk out of the source folder.";
            log_event(L"warn", L"payload", detail.c_str());
            continue;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (!copy_tree(source, dest, depth + 1, files, error)) {
                ok = false;
                break;
            }
            continue;
        }
        if (!CopyFileW(source.c_str(), dest.c_str(), FALSE)) {
            error = L"Could not copy '" + source + L"' to '" + dest + L"'. " + windows_error_text(GetLastError());
            log_event(L"error", L"payload", error.c_str());
            ok = false;
            break;
        }
        ++files;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return ok;
}

bool copy_one_file(const std::wstring& source, const std::wstring& dest, int& files, std::wstring& error) {
    const size_t slash = dest.find_last_of(L"\\/");
    if (slash != std::wstring::npos && !ensure_directory(dest.substr(0, slash))) {
        error = L"Could not create the folder for '" + dest + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"payload", error.c_str());
        return false;
    }
    if (!CopyFileW(source.c_str(), dest.c_str(), FALSE)) {
        error = L"Could not copy '" + source + L"' to '" + dest + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"payload", error.c_str());
        return false;
    }
    ++files;
    return true;
}

bool directory_exists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool is_drive_root(const std::wstring& path) {
    const std::wstring full = full_path(path);
    return full.size() == 3 && full[1] == L':' && (full[2] == L'\\' || full[2] == L'/');
}

bool delete_tree(const std::wstring& path, int depth, std::wstring& error) {
    if (depth > 32) {
        error = L"Stopped removing '" + path + L"' because the folder is nested more than 32 levels.";
        log_event(L"error", L"uninstall", error.c_str());
        return false;
    }
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        const std::wstring detail = L"Skipped reparse point '" + path + L"' so uninstall cannot follow a link out of the DoomNet folder.";
        log_event(L"warn", L"uninstall", detail.c_str());
        return true;
    }
    const std::wstring pattern = path + L"\\*";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring child = path + L"\\" + name;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (!delete_tree(child, depth + 1, error)) {
                    FindClose(find);
                    return false;
                }
            } else if (!DeleteFileW(child.c_str())) {
                error = L"Could not delete '" + child + L"'. " + windows_error_text(GetLastError());
                log_event(L"error", L"uninstall", error.c_str());
                FindClose(find);
                return false;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    if (!RemoveDirectoryW(path.c_str())) {
        error = L"Could not remove folder '" + path + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"uninstall", error.c_str());
        return false;
    }
    return true;
}

}  // namespace

InstallResult apply_install(const std::wstring& root, const InstallRequest& request) {
    InstallResult result;
    if (root.empty() || root.find(L"..") != std::wstring::npos) {
        result.detail = L"Refusing install into '" + root + L"' because the folder is empty or contains '..'.";
        log_event(L"error", L"payload", result.detail.c_str());
        return result;
    }
    std::wstring steam;
    if (!request.steam_path.empty()) {
        steam = request.steam_path;
    } else {
        discover_steam_path(steam);
    }
    if (!steam.empty() && inside_directory(root, steam)) {
        result.detail = L"Refusing to install into '" + root + L"' because that folder is inside the Steam install '" + steam + L"'.";
        log_event(L"error", L"payload", result.detail.c_str());
        return result;
    }
    if (!request.player && !request.skin) {
        result.detail = L"Nothing was installed. Choose the player, the skin, or both.";
        log_event(L"error", L"payload", result.detail.c_str());
        return result;
    }
    if (!ensure_directory(root)) {
        result.detail = L"Could not create '" + root + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"payload", result.detail.c_str());
        return result;
    }

    std::wstring error;
    if (request.player) {
        const std::wstring player_dest = root + L"\\player";
        const std::wstring plugin = request.player_source + L"\\plugin.json";
        const std::wstring backend = request.player_source + L"\\backend";
        log_event(L"info", L"payload", (L"Copying the player from '" + request.player_source + L"' to '" + player_dest + L"'.").c_str());
        if (!copy_one_file(plugin, player_dest + L"\\plugin.json", result.files_copied, error) || !copy_tree(backend, player_dest + L"\\backend", 0, result.files_copied, error)) {
            result.detail = error;
            return result;
        }
    }
    if (request.skin) {
        const std::wstring skin_dest = root + L"\\skin";
        log_event(L"info", L"payload", (L"Copying the skin from '" + request.skin_source + L"' to '" + skin_dest + L"'.").c_str());
        if (!copy_tree(request.skin_source, skin_dest, 0, result.files_copied, error)) {
            result.detail = error;
            return result;
        }
    }

    Config config;
    config.steam_path = steam;
    config.player = request.player;
    config.skin = request.skin;
    config.player_color = request.match_skin && request.player && request.skin ? PlayerColor::Red : PlayerColor::Steam;
    config.updates = true;
    if (!save_config_file(root + L"\\config.json", config, error)) {
        result.detail = error;
        log_event(L"error", L"payload", error.c_str());
        return result;
    }

    if (request.player) {
        const std::wstring settings_path = root + L"\\player\\backend\\data\\settings.json";
        const bool exists = wide_file_exists(settings_path);
        const wchar_t* theme = theme_for_new_settings(request.player, request.skin, request.match_skin, exists);
        if (theme == nullptr) {
            log_event(L"info", L"payload", (L"Left '" + settings_path + L"' unchanged. An existing color is not overwritten, and skin-only does not write player settings.").c_str());
        } else {
            if (!ensure_directory(root + L"\\player\\backend\\data")) {
                result.detail = L"Could not create the player data folder under '" + root + L"'. " + windows_error_text(GetLastError());
                log_event(L"error", L"payload", result.detail.c_str());
                return result;
            }
            const std::string body = std::string("{\"uiColorTheme\":\"") + (theme[0] == L'r' ? "red" : "steam") + "\"}\n";
            if (!write_new_file(settings_path, body.data(), body.size(), error)) {
                result.detail = error;
                return result;
            }
            const std::wstring detail = std::wstring(L"Wrote uiColorTheme ") + theme + L" into a new settings.json at '" + settings_path + L"'. An existing settings.json is never replaced.";
            log_event(L"info", L"payload", detail.c_str());
        }
    }

    InstalledVersions versions;
    versions.doomnet = L"0.1.0";
    if (request.player) {
        std::string plugin;
        std::wstring plugin_error;
        if (read_entire_file(request.player_source + L"\\plugin.json", plugin, plugin_error)) {
            const size_t at = plugin.find("\"version\"");
            const size_t quote = at == std::string::npos ? std::string::npos : plugin.find('\"', plugin.find(':', at));
            const size_t end = quote == std::string::npos ? std::string::npos : plugin.find('\"', quote + 1);
            if (quote != std::string::npos && end != std::string::npos && end > quote + 1 && end - quote - 1 <= 64) {
                versions.steamtunes = utf8_to_wide_lossy(plugin.substr(quote + 1, end - quote - 1));
            }
        }
    }
    if (request.skin) {
        std::string skin;
        std::wstring skin_error;
        if (read_entire_file(request.skin_source + L"\\skin.json", skin, skin_error)) {
            const size_t at = skin.find("\"version\"");
            const size_t quote = at == std::string::npos ? std::string::npos : skin.find('\"', skin.find(':', at));
            const size_t end = quote == std::string::npos ? std::string::npos : skin.find('\"', quote + 1);
            if (quote != std::string::npos && end != std::string::npos && end > quote + 1 && end - quote - 1 <= 64) {
                versions.steamskin = utf8_to_wide_lossy(skin.substr(quote + 1, end - quote - 1));
            }
        }
    }
    if (!save_installed_versions(root + L"\\versions.json", versions, error)) {
        result.detail = error;
        log_event(L"error", L"installer", error.c_str());
        return result;
    }
    log_event(L"info", L"installer", (L"Wrote versions.json for DoomNet " + versions.doomnet + L", SteamTunes " + (versions.steamtunes.empty() ? L"(skipped)" : versions.steamtunes) + L", SteamSkin " + (versions.steamskin.empty() ? L"(skipped)" : versions.steamskin) + L".").c_str());

    if (!directory_exists(root)) {
        result.detail = L"'" + root + L"' is not there after the copy. Nothing was installed.";
        log_event(L"error", L"payload", result.detail.c_str());
        return result;
    }
    if (request.player && !wide_file_exists(root + L"\\player\\backend\\main.lua")) {
        result.detail = L"The copy reported " + std::to_wstring(result.files_copied) + L" files, but '" + root + L"\\player\\backend\\main.lua' is not there. Nothing was installed.";
        log_event(L"error", L"payload", result.detail.c_str());
        return result;
    }
    if (request.skin && !wide_file_exists(root + L"\\skin\\skin.json")) {
        result.detail = L"The copy reported " + std::to_wstring(result.files_copied) + L" files, but '" + root + L"\\skin\\skin.json' is not there. Nothing was installed.";
        log_event(L"error", L"payload", result.detail.c_str());
        return result;
    }
    result.ok = true;
    result.detail = L"Installed " + std::to_wstring(result.files_copied) + L" files into '" + root + L"'.";
    log_event(L"info", L"payload", result.detail.c_str());
    return result;
}

UninstallResult apply_uninstall(const UninstallRequest& request) {
    UninstallResult result;
    const std::wstring& root = request.root;
    if (root.empty() || root.find(L"..") != std::wstring::npos || is_drive_root(root)) {
        result.detail = L"Refusing to uninstall '" + root + L"'. The path is empty, contains '..', or is a whole drive.";
        log_event(L"error", L"uninstall", result.detail.c_str());
        return result;
    }
    if (directory_has_steam_exe(root) || wide_file_exists(root + L"\\steam.exe")) {
        result.detail = L"Refusing to uninstall '" + root + L"' because steam.exe is in that folder. The Steam client was not touched.";
        log_event(L"error", L"uninstall", result.detail.c_str());
        return result;
    }
    if (wide_file_exists(root + L"\\plugin.json") && directory_exists(root + L"\\backend") && !wide_file_exists(root + L"\\config.json")) {
        result.detail = L"Refusing to uninstall '" + root + L"' because it is the SteamTunes source folder, not a DoomNet install. No source files were deleted.";
        log_event(L"error", L"uninstall", result.detail.c_str());
        return result;
    }
    if (wide_file_exists(root + L"\\skin.json") && !wide_file_exists(root + L"\\config.json")) {
        result.detail = L"Refusing to uninstall '" + root + L"' because it is the Steam-Skin source folder, not a DoomNet install. No source files were deleted.";
        log_event(L"error", L"uninstall", result.detail.c_str());
        return result;
    }

    log_event(L"info", L"uninstall", (L"Starting uninstall of '" + root + L"'.").c_str());
    Config config;
    std::wstring config_error;
    const ConfigLoad loaded = load_config_file(root + L"\\config.json", config, config_error);
    const bool player_selected = loaded == ConfigLoad::Ok && config.player;
    const bool skin_selected = loaded == ConfigLoad::Ok && config.skin;
    const std::wstring player_dir = root + L"\\player";
    const std::wstring skin_dir = root + L"\\skin";
    std::wstring error;

    if (!player_selected && !directory_exists(player_dir)) {
        result.skipped_player = true;
        log_event(L"info", L"uninstall", L"Doomy's SteamTunes was not installed. The player folder is absent, so it was skipped.");
    } else if (!directory_exists(player_dir)) {
        result.skipped_player = true;
        log_event(L"info", L"uninstall", (L"Config said the player was installed, but '" + player_dir + L"' is already gone. Skipped.").c_str());
    } else if (!delete_tree(player_dir, 0, error)) {
        result.detail = error;
        return result;
    } else {
        result.removed_player = true;
        log_event(L"info", L"uninstall", (L"Removed Doomy's SteamTunes from '" + player_dir + L"', including its settings and library data. The SteamTunes source folder was not touched.").c_str());
    }

    if (!skin_selected && !directory_exists(skin_dir)) {
        result.skipped_skin = true;
        log_event(L"info", L"uninstall", L"Steam-Skin was not selected and is not on disk. It was skipped.");
    } else if (!directory_exists(skin_dir)) {
        result.skipped_skin = true;
        log_event(L"info", L"uninstall", (L"Config said Steam-Skin was installed, but '" + skin_dir + L"' is already gone. Skipped.").c_str());
    } else if (!delete_tree(skin_dir, 0, error)) {
        result.detail = error;
        return result;
    } else {
        result.removed_skin = true;
        log_event(L"info", L"uninstall", (L"Removed Steam-Skin from '" + skin_dir + L"'. The Steam-Skin source folder was not touched.").c_str());
    }

    if (!request.steamui.empty()) {
        const std::wstring player_ui = request.steamui + L"\\steam-music-player";
        const std::wstring skin_ui = request.steamui + L"\\doomnet-skin";
        if (directory_exists(player_ui)) {
            if (!delete_tree(player_ui, 0, error)) {
                result.detail = error;
                return result;
            }
            log_event(L"info", L"uninstall", (L"Removed the player files DoomNet had placed in '" + player_ui + L"'. The rest of steamui was left in place.").c_str());
        } else {
            log_event(L"info", L"uninstall", (L"No player files were in '" + player_ui + L"'. Skipped.").c_str());
        }
        if (directory_exists(skin_ui)) {
            if (!delete_tree(skin_ui, 0, error)) {
                result.detail = error;
                return result;
            }
            log_event(L"info", L"uninstall", (L"Removed the skin files DoomNet had placed in '" + skin_ui + L"'. The rest of steamui was left in place.").c_str());
        } else {
            log_event(L"info", L"uninstall", (L"No skin files were in '" + skin_ui + L"'. Skipped.").c_str());
        }
    }

    if (!request.shortcut.empty()) {
        if (!wide_file_exists(request.shortcut)) {
            log_event(L"info", L"uninstall", (L"Shortcut '" + request.shortcut + L"' is not on disk. Skipped.").c_str());
        } else if (!DeleteFileW(request.shortcut.c_str())) {
            result.detail = L"Could not delete shortcut '" + request.shortcut + L"'. " + windows_error_text(GetLastError());
            log_event(L"error", L"uninstall", result.detail.c_str());
            return result;
        } else {
            log_event(L"info", L"uninstall", (L"Deleted shortcut '" + request.shortcut + L"'.").c_str());
        }
    }

    if (directory_exists(root)) {
        log_event(L"info", L"uninstall", (L"Removing the DoomNet data folder '" + root + L"'.").c_str());
        if (!delete_tree(root, 0, error)) {
            result.detail = error;
            return result;
        }
    }
    result.ok = true;
    result.detail = L"Uninstall finished for '" + root + L"'. DoomNet, and any player or skin it had installed there, are gone.";
    return result;
}

namespace {

std::mutex g_store_mu;
StoreInstallRequest g_store;
bool g_store_pending = false;

std::wstring module_directory() {
    wchar_t exe[MAX_PATH] = {};
    const DWORD wrote = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wrote == 0 || wrote >= MAX_PATH) {
        return L"";
    }
    std::wstring path = exe;
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}

std::wstring default_tree(const wchar_t* payload_name, const wchar_t* marker, const wchar_t* repo) {
    const std::wstring beside = module_directory() + L"\\payload\\" + payload_name;
    if (wide_file_exists(beside + L"\\" + marker)) {
        return beside;
    }
    if (wide_file_exists(std::wstring(repo) + L"\\" + marker)) {
        return repo;
    }
    return L"";
}

std::wstring version_in_file(const std::wstring& path) {
    std::string text;
    std::wstring error;
    if (!read_entire_file(path, text, error)) {
        return L"";
    }
    const size_t at = text.find("\"version\"");
    const size_t quote = at == std::string::npos ? std::string::npos : text.find('"', text.find(':', at));
    const size_t end = quote == std::string::npos ? std::string::npos : text.find('"', quote + 1);
    if (quote == std::string::npos || end == std::string::npos || end <= quote + 1 || end - quote - 1 > 64) {
        return L"";
    }
    return utf8_to_wide_lossy(text.substr(quote + 1, end - quote - 1));
}

std::vector<std::wstring> split_command(const wchar_t* command_line) {
    std::vector<std::wstring> tokens;
    if (command_line == nullptr) {
        return tokens;
    }
    const wchar_t* cursor = command_line;
    while (*cursor != L'\0') {
        while (*cursor == L' ' || *cursor == L'\t') {
            ++cursor;
        }
        if (*cursor == L'\0') {
            break;
        }
        std::wstring token;
        if (*cursor == L'"') {
            ++cursor;
            while (*cursor != L'\0' && *cursor != L'"') {
                token.push_back(*cursor++);
            }
            if (*cursor == L'"') {
                ++cursor;
            }
        } else {
            while (*cursor != L'\0' && *cursor != L' ' && *cursor != L'\t') {
                token.push_back(*cursor++);
            }
        }
        tokens.push_back(token);
    }
    return tokens;
}

}  // namespace

void queue_store_install(const StoreInstallRequest& request) {
    std::lock_guard<std::mutex> lock(g_store_mu);
    g_store = request;
    g_store_pending = true;
    log_event(L"info", L"installer", (L"SteamSkin install was requested for '" + request.root + L"'. The copy runs off the Lua thread, and settings.json is not changed.").c_str());
}

bool take_store_install(StoreInstallRequest& out) {
    std::lock_guard<std::mutex> lock(g_store_mu);
    if (!g_store_pending) {
        return false;
    }
    out = g_store;
    g_store = StoreInstallRequest{};
    g_store_pending = false;
    return true;
}

InstallResult install_store_skin(const StoreInstallRequest& request) {
    InstallResult result;
    if (request.root.empty() || request.root.find(L"..") != std::wstring::npos || path_is_dev_source_tree(request.root)) {
        result.detail = L"Refusing to install SteamSkin into '" + request.root + L"' because the folder is empty, contains '..', or is a source tree.";
        log_event(L"error", L"installer", result.detail.c_str());
        return result;
    }
    std::wstring steam = request.steam_path;
    if (!steam.empty() && inside_directory(request.root, steam)) {
        result.detail = L"Refusing to install SteamSkin into '" + request.root + L"' because that folder is inside the Steam install '" + steam + L"'.";
        log_event(L"error", L"installer", result.detail.c_str());
        return result;
    }
    Config existing;
    std::wstring existing_error;
    if (load_config_file(request.root + L"\\config.json", existing, existing_error) == ConfigLoad::Ok && existing.skin) {
        result.detail = L"SteamSkin is already installed in '" + request.root + L"'. It was not copied again, and settings.json was not changed.";
        log_event(L"info", L"installer", result.detail.c_str());
        return result;
    }
    const std::wstring source = request.skin_source.empty() ? default_tree(L"skin", L"skin.json", L"D:\\Documents\\Repos\\SteamSkin") : request.skin_source;
    if (source.empty() || source.find(L"..") != std::wstring::npos || !wide_file_exists(source + L"\\skin.json")) {
        result.detail = L"SteamSkin was not installed because '" + source + L"' has no skin.json.";
        log_event(L"error", L"installer", result.detail.c_str());
        return result;
    }
    const std::wstring dest = request.root + L"\\skin";
    if (lower_copy(full_path(source)) == lower_copy(full_path(dest))) {
        result.detail = L"Refusing to copy SteamSkin from '" + source + L"' onto itself.";
        log_event(L"error", L"installer", result.detail.c_str());
        return result;
    }
    const std::wstring settings_path = request.root + L"\\player\\backend\\data\\settings.json";
    std::string settings_before;
    std::wstring settings_error;
    const bool had_settings = read_entire_file(settings_path, settings_before, settings_error);
    log_event(L"info", L"installer", (L"Copying SteamSkin from '" + source + L"' to '" + dest + L"'.").c_str());
    std::wstring error;
    if (!copy_tree(source, dest, 0, result.files_copied, error)) {
        result.detail = error;
        return result;
    }
    if (had_settings) {
        std::string settings_after;
        if (!read_entire_file(settings_path, settings_after, error) || settings_after != settings_before) {
            result.detail = L"SteamSkin was copied, but '" + settings_path + L"' changed. The player's saved color must stay as it is.";
            log_event(L"error", L"installer", result.detail.c_str());
            return result;
        }
    }

    Config config;
    const ConfigLoad loaded = load_config_file(request.root + L"\\config.json", config, error);
    if (loaded == ConfigLoad::Error) {
        result.detail = error.empty() ? L"SteamSkin was copied, but config.json could not be read, so skin was not turned on." : error;
        log_event(L"error", L"installer", result.detail.c_str());
        return result;
    }
    if (loaded == ConfigLoad::Missing) {
        config = Config{};
        config.steam_path = steam;
    } else if (config.steam_path.empty()) {
        config.steam_path = steam;
    }
    config.skin = true;
    if (!save_config_file(request.root + L"\\config.json", config, error)) {
        result.detail = error;
        log_event(L"error", L"installer", error.c_str());
        return result;
    }

    InstalledVersions versions;
    if (wide_file_exists(request.root + L"\\versions.json") && !load_installed_versions(request.root + L"\\versions.json", versions, error)) {
        result.detail = error;
        log_event(L"error", L"installer", error.c_str());
        return result;
    }
    const std::wstring previous = versions.steamskin;
    versions.steamskin = version_in_file(source + L"\\skin.json");
    if (!save_installed_versions(request.root + L"\\versions.json", versions, error)) {
        result.detail = error;
        log_event(L"error", L"installer", error.c_str());
        return result;
    }
    result.ok = true;
    result.detail = L"Installed SteamSkin " + (versions.steamskin.empty() ? L"(no version)" : versions.steamskin) + L" (was " + (previous.empty() ? L"not installed" : previous) + L") into '" + dest + L"'. settings.json was not changed.";
    log_event(L"info", L"installer", result.detail.c_str());
    return result;
}

bool command_is_install(const wchar_t* command_line) {
    const std::vector<std::wstring> tokens = split_command(command_line);
    return !tokens.empty() && tokens[0] == L"--install";
}

bool command_is_hook(const wchar_t* command_line) {
    const std::vector<std::wstring> tokens = split_command(command_line);
    return !tokens.empty() && tokens[0] == L"--hook";
}

InstallResult run_hook_command(const wchar_t* command_line) {
    InstallResult result;
    const std::vector<std::wstring> tokens = split_command(command_line);
    if (tokens.size() < 2 || tokens[0] != L"--hook") {
        result.detail = L"The hook command needs the Steam folder. version.dll was not copied.";
        log_event(L"error", L"installer", result.detail.c_str());
        return result;
    }
    const std::wstring dll = module_directory() + L"\\version.dll";
    log_event(L"info", L"installer", (L"Copying '" + dll + L"' beside steam.exe in '" + tokens[1] + L"'.").c_str());
    const PlaceBootstrapResult placed = place_doomnet_bootstrap(tokens[1], dll, true);
    result.ok = placed.ok;
    result.detail = placed.detail.empty() ? L"version.dll was not copied beside steam.exe." : placed.detail;
    if (!result.ok) {
        log_event(L"error", L"installer", result.detail.c_str());
    }
    return result;
}

InstallResult run_command_line_install(const std::wstring& root, const wchar_t* command_line, const std::wstring& discovered_steam) {
    InstallResult result;
    const std::vector<std::wstring> tokens = split_command(command_line);
    if (tokens.empty() || tokens[0] != L"--install") {
        result.detail = L"The install command is missing --install, so nothing was copied.";
        log_event(L"error", L"installer", result.detail.c_str());
        return result;
    }
    InstallRequest request;
    request.steam_path = discovered_steam;
    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::wstring& token = tokens[i];
        auto need_value = [&](const wchar_t* flag) -> const std::wstring* {
            if (i + 1 >= tokens.size()) {
                result.detail = std::wstring(L"Install flag ") + flag + L" needs a value. Nothing was copied.";
                return nullptr;
            }
            return &tokens[++i];
        };
        if (token == L"--player") {
            request.player = true;
        } else if (token == L"--skin") {
            request.skin = true;
        } else if (token == L"--steam") {
            const std::wstring* value = need_value(L"--steam");
            if (value == nullptr) {
                log_event(L"error", L"installer", result.detail.c_str());
                return result;
            }
            request.steam_path = *value;
        } else if (token == L"--color") {
            const std::wstring* value = need_value(L"--color");
            if (value == nullptr) {
                log_event(L"error", L"installer", result.detail.c_str());
                return result;
            }
            if (*value == L"red") {
                request.match_skin = true;
            } else if (*value != L"steam") {
                result.detail = L"Install color '" + *value + L"' is not steam or red. Nothing was copied.";
                log_event(L"error", L"installer", result.detail.c_str());
                return result;
            }
        } else if (token == L"--player-source") {
            const std::wstring* value = need_value(L"--player-source");
            if (value == nullptr) {
                log_event(L"error", L"installer", result.detail.c_str());
                return result;
            }
            request.player_source = *value;
        } else if (token == L"--skin-source") {
            const std::wstring* value = need_value(L"--skin-source");
            if (value == nullptr) {
                log_event(L"error", L"installer", result.detail.c_str());
                return result;
            }
            request.skin_source = *value;
        } else {
            result.detail = L"Install flag '" + token + L"' is not recognized. Nothing was copied.";
            log_event(L"error", L"installer", result.detail.c_str());
            return result;
        }
    }
    if (request.player && request.player_source.empty()) {
        request.player_source = default_tree(L"player", L"plugin.json", L"D:\\Documents\\Repos\\SteamTunes");
    }
    if (request.skin && request.skin_source.empty()) {
        request.skin_source = default_tree(L"skin", L"skin.json", L"D:\\Documents\\Repos\\SteamSkin");
    }
    log_event(L"info", L"installer", (L"Running --install into '" + root + L"'. Steam is not started by this command.").c_str());
    result = apply_install(root, request);
    if (!result.ok) {
        return result;
    }
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) > 0) {
        std::wstring self_path = self;
        const size_t slash = self_path.find_last_of(L"\\/");
        const std::wstring name = slash == std::wstring::npos ? self_path : self_path.substr(slash + 1);
        if (_wcsicmp(name.c_str(), L"DoomNet.exe") == 0) {
            const std::wstring host_dest = root + L"\\DoomNet.exe";
            if (_wcsicmp(full_path(self_path).c_str(), full_path(host_dest).c_str()) == 0) {
                log_event(L"info", L"installer", (L"DoomNet.exe is already at '" + host_dest + L"', so Steam can start it.").c_str());
            } else if (!CopyFileW(self_path.c_str(), host_dest.c_str(), FALSE)) {
                result.ok = false;
                result.detail = L"Could not copy DoomNet.exe to '" + host_dest + L"'. " + windows_error_text(GetLastError());
                log_event(L"error", L"installer", result.detail.c_str());
                return result;
            } else {
                log_event(L"info", L"installer", (L"Copied DoomNet.exe to '" + host_dest + L"' so Steam can start it.").c_str());
            }
        }
    }
    if (!request.steam_path.empty() && directory_has_steam_exe(request.steam_path)) {
        const std::wstring dll = module_directory() + L"\\version.dll";
        if (!wide_file_exists(dll)) {
            result.ok = false;
            result.detail = L"'" + dll + L"' is not beside DoomNet.exe, so Steam would open without the skin or the player.";
            log_event(L"error", L"installer", result.detail.c_str());
            return result;
        }
        const PlaceBootstrapResult placed = place_doomnet_bootstrap(request.steam_path, dll, true);
        if (!placed.ok) {
            result.ok = false;
            result.detail = placed.detail.empty() ? L"DoomNet's bootstrap was not copied beside steam.exe." : placed.detail;
            return result;
        }
    }
    result.detail += L" Open Steam from its normal shortcut. DoomNet starts with it. Steam was not started by this install.";
    log_event(L"info", L"installer", result.detail.c_str());
    return result;
}

}  // namespace doomnet
