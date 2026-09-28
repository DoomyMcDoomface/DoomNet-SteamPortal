#include "payload.h"

#include "config.h"
#include "installer.h"
#include "log.h"
#include "paths.h"
#include "steam_discover.h"
#include "wide_io.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

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

}  // namespace doomnet
