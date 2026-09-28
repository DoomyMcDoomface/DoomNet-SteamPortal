// Spec: Installer.
// Steam path order: HKCU\Software\Valve\Steam\SteamPath, then
// HKLM\SOFTWARE\WOW6432Node\Valve\Steam\InstallPath, then
// C:\Program Files (x86)\Steam if steam.exe is there.
// A candidate is accepted only when steam.exe exists in that directory.
// Read-only. This file does not start Steam.

#include "steam_discover.h"

#include "anchors.h"
#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace doomnet {
namespace {

bool read_reg_string(HKEY root, const wchar_t* subkey, const wchar_t* value, std::wstring& out) {
    out.clear();
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    const LSTATUS size_status = RegQueryValueExW(key, value, nullptr, &type, nullptr, &bytes);
    if (size_status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t) || bytes > 32768 || (bytes % sizeof(wchar_t)) != 0) {
        RegCloseKey(key);
        return false;
    }
    std::wstring raw(bytes / sizeof(wchar_t), L'\0');
    DWORD got = bytes;
    const LSTATUS read_status = RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<LPBYTE>(raw.data()), &got);
    RegCloseKey(key);
    if (read_status != ERROR_SUCCESS || raw.empty()) {
        return false;
    }
    if (raw.back() == L'\0') {
        raw.pop_back();
    }
    if (type == REG_EXPAND_SZ) {
        wchar_t expanded[32768] = {};
        const DWORD n = ExpandEnvironmentStringsW(raw.c_str(), expanded, 32768);
        if (n == 0 || n > 32768) {
            return false;
        }
        raw.assign(expanded);
    }
    for (wchar_t& ch : raw) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    while (!raw.empty() && (raw.back() == L'\\' || raw.back() == L' ')) {
        raw.pop_back();
    }
    out = raw;
    return !out.empty();
}

}  // namespace

bool directory_has_steam_exe(const std::wstring& directory) {
    if (directory.empty() || directory.find(L"..") != std::wstring::npos) {
        return false;
    }
    std::wstring path = directory;
    if (path.back() != L'\\') {
        path.push_back(L'\\');
    }
    path += L"steam.exe";
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool discover_steam_path(std::wstring& out) {
    out.clear();
    const struct Place {
        HKEY root;
        const wchar_t* subkey;
        const wchar_t* value;
        const wchar_t* label;
    } places[] = {
        {HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", L"HKCU\\Software\\Valve\\Steam SteamPath"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", L"HKLM\\SOFTWARE\\WOW6432Node\\Valve\\Steam InstallPath"},
    };
    for (const Place& place : places) {
        std::wstring candidate;
        HKEY key = nullptr;
        const LSTATUS opened = RegOpenKeyExW(place.root, place.subkey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
        if (opened != ERROR_SUCCESS) {
            const std::wstring detail = std::wstring(L"Checked ") + place.label + L". The key could not be opened. " + windows_error_text(static_cast<unsigned long>(opened)) + L" Trying the next location.";
            log_event(L"info", L"steam", detail.c_str());
            continue;
        }
        RegCloseKey(key);
        if (!read_reg_string(place.root, place.subkey, place.value, candidate)) {
            const std::wstring detail = std::wstring(L"Checked ") + place.label + L". The value is missing, empty, or not a string. Trying the next location.";
            log_event(L"info", L"steam", detail.c_str());
            continue;
        }
        if (!directory_has_steam_exe(candidate)) {
            const std::wstring detail = std::wstring(L"Checked ") + place.label + L". It names '" + candidate + L"', and steam.exe is not in that folder, so this candidate was rejected. Trying the next location.";
            log_event(L"info", L"steam", detail.c_str());
            continue;
        }
        const std::wstring detail = std::wstring(L"Checked ") + place.label + L". It names '" + candidate + L"', and steam.exe is in that folder. This is the Steam install DoomNet will use.";
        log_event(L"info", L"steam", detail.c_str());
        out = candidate;
        return true;
    }
    const std::wstring fallback = L"C:\\Program Files (x86)\\Steam";
    if (directory_has_steam_exe(fallback)) {
        log_event(L"info", L"steam", L"The registry candidates were not usable. The default folder 'C:\\Program Files (x86)\\Steam' contains steam.exe, so that is the Steam install DoomNet will use.");
        out = fallback;
        return true;
    }
    log_event(L"error", L"steam", L"No Steam install was found. Checked HKCU\\Software\\Valve\\Steam SteamPath, HKLM\\SOFTWARE\\WOW6432Node\\Valve\\Steam InstallPath, and C:\\Program Files (x86)\\Steam. DoomNet will not start Steam.");
    return false;
}

void steam_discover_anchor() {}

}  // namespace doomnet
