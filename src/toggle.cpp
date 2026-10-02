// Spec: Millennium toggle on this PC.
// Steam must be fully quit first (no steam.exe, no steamwebhelper.exe).
// Stash only these names from the Steam root, and only if they are present:
//   wsock32.dll
//   version.dll
//   millennium-legacy.version.dll
//   user32.dll
// Do not move any other DLL. Do not touch System32.
// Do not delete millennium\plugins, millennium\themes, or player data.
// DoomNet refuses to inject while those hook files are back in place.
// No console.

#include "toggle.h"

#include "anchors.h"
#include "log.h"
#include "steam_discover.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <string>
#include <vector>

namespace doomnet {

void toggle_anchor() {}

namespace {

// The only files the toggle may move, lower-case for comparison. version.dll and
// user32.dll are the retired hook and its shim; wsock32.dll is the current
// bootstrap; millennium-legacy.version.dll is where the retired version.dll is
// parked. Nothing else in the Steam root is Millennium's to move.
const wchar_t* const kHookFiles[] = {
    L"wsock32.dll",
    L"version.dll",
    L"millennium-legacy.version.dll",
    L"user32.dll",
};

const wchar_t* const kStashDirName = L"doomnet-stash";
const wchar_t* const kMarkerName = L"doomnet-stash.txt";

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
    if (parent.empty()) {
        return false;
    }
    if (child == parent) {
        return true;
    }
    return child.size() > parent.size() && child.compare(0, parent.size(), parent) == 0 && (child[parent.size()] == L'\\' || child[parent.size()] == L'/');
}

bool file_present(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool is_allowed_hook_name(const std::wstring& name) {
    const std::wstring lowered = lower_copy(name);
    for (const wchar_t* allowed : kHookFiles) {
        if (lowered == allowed) {
            return true;
        }
    }
    return false;
}

std::wstring join(const std::wstring& dir, const std::wstring& name) {
    std::wstring out = dir;
    if (!out.empty() && out.back() != L'\\' && out.back() != L'/') {
        out.push_back(L'\\');
    }
    out += name;
    return out;
}

// A steam_path that names an existing Steam install, is absolute, has no "..",
// and is not inside the Windows directory. `error` is a full sentence.
bool validate_steam_dir(const std::wstring& steam_path, std::wstring& error) {
    if (steam_path.empty()) {
        error = L"No Steam folder was given, so the Millennium toggle did nothing.";
        return false;
    }
    if (steam_path.find(L"..") != std::wstring::npos) {
        error = L"The Steam folder '" + steam_path + L"' contains '..', so the Millennium toggle refused to touch it.";
        return false;
    }
    if (!directory_has_steam_exe(steam_path)) {
        error = L"'" + steam_path + L"' does not contain steam.exe, so the Millennium toggle refused to move files there.";
        return false;
    }
    wchar_t windows_dir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(windows_dir, MAX_PATH) != 0 && inside_directory(steam_path, windows_dir)) {
        error = L"'" + steam_path + L"' is inside the Windows directory, so the Millennium toggle refused to touch it. System32 is never modified.";
        return false;
    }
    return true;
}

std::vector<std::wstring> read_marker(const std::wstring& marker) {
    std::vector<std::wstring> names;
    HANDLE handle = CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return names;
    }
    std::string bytes;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        bytes.append(buffer, read);
        if (bytes.size() > 64 * 1024) {
            break;
        }
    }
    CloseHandle(handle);
    std::string line;
    auto flush = [&]() {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            const int chars = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0);
            std::wstring wide(chars, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), wide.data(), chars);
            names.push_back(wide);
        }
        line.clear();
    };
    for (const char c : bytes) {
        if (c == '\n') {
            flush();
        } else {
            line.push_back(c);
        }
    }
    flush();
    return names;
}

bool write_marker(const std::wstring& marker, const std::vector<std::wstring>& names, std::wstring& error) {
    std::string bytes;
    for (const std::wstring& name : names) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
        std::string narrow(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), narrow.data(), len, nullptr, nullptr);
        bytes += narrow;
        bytes.push_back('\n');
    }
    HANDLE handle = CreateFileW(marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = L"Could not write the stash marker '" + marker + L"'. " + windows_error_text(GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(handle);
    if (!ok) {
        error = L"Could not finish writing the stash marker '" + marker + L"'. " + windows_error_text(GetLastError());
    }
    return ok;
}

}  // namespace

bool steam_is_running(std::wstring& which) {
    which.clear();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const std::wstring name = lower_copy(entry.szExeFile);
            if (name == L"steam.exe" || name == L"steamwebhelper.exe") {
                which = entry.szExeFile;
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool is_millennium_bootstrap(const std::wstring& dll_path) {
    if (!file_present(dll_path)) {
        return false;
    }
    HANDLE handle = CreateFileW(dll_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    // Millennium's bootstrap exports __get_shim_version. DoomNet's own wsock32.dll
    // does not, so the presence of that symbol name in the file's bytes tells the
    // two apart without loading the DLL.
    std::string bytes;
    char buffer[65536];
    DWORD read = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        bytes.append(buffer, read);
        if (bytes.size() > 16u * 1024u * 1024u) {
            break;
        }
    }
    CloseHandle(handle);
    return bytes.find("__get_shim_version") != std::string::npos;
}

MillenniumState millennium_state(const std::wstring& steam_path) {
    std::wstring ignored;
    if (!validate_steam_dir(steam_path, ignored)) {
        return MillenniumState::Unknown;
    }
    const std::wstring marker = join(join(steam_path, kStashDirName), kMarkerName);
    if (file_present(marker)) {
        return MillenniumState::Stashed;
    }
    for (const wchar_t* name : kHookFiles) {
        const std::wstring candidate = join(steam_path, name);
        if (!file_present(candidate)) {
            continue;
        }
        // A plain wsock32.dll could be DoomNet's own bootstrap once that exists.
        // Only Millennium's counts as Active.
        if (lower_copy(name) == L"wsock32.dll" && !is_millennium_bootstrap(candidate)) {
            continue;
        }
        return MillenniumState::Active;
    }
    return MillenniumState::NotPresent;
}

ToggleResult millennium_off(const std::wstring& steam_path, bool require_steam_closed) {
    ToggleResult result;
    std::wstring error;
    if (!validate_steam_dir(steam_path, error)) {
        result.detail = error;
        log_event(L"error", L"toggle", result.detail.c_str());
        return result;
    }
    if (require_steam_closed) {
        std::wstring which;
        if (steam_is_running(which)) {
            result.detail = L"Refusing to stash Millennium while '" + which + L"' is running. Quit Steam fully first.";
            log_event(L"warn", L"toggle", result.detail.c_str());
            return result;
        }
    }

    const std::wstring stash = join(steam_path, kStashDirName);
    const std::wstring marker = join(stash, kMarkerName);
    if (file_present(marker)) {
        result.ok = true;
        result.state = MillenniumState::Stashed;
        result.detail = L"Millennium is already stashed at '" + stash + L"', so nothing was moved.";
        log_event(L"info", L"toggle", result.detail.c_str());
        return result;
    }

    std::vector<std::wstring> present;
    for (const wchar_t* name : kHookFiles) {
        const std::wstring candidate = join(steam_path, name);
        if (!file_present(candidate)) {
            continue;
        }
        // A wsock32.dll without Millennium's shim marker is DoomNet's own bootstrap.
        // Leave it alone; the toggle only moves Millennium's files.
        if (lower_copy(name) == L"wsock32.dll" && !is_millennium_bootstrap(candidate)) {
            continue;
        }
        present.emplace_back(name);
    }
    if (present.empty()) {
        result.ok = true;
        result.state = MillenniumState::NotPresent;
        result.detail = L"No Millennium hook file was beside steam.exe in '" + steam_path + L"', so nothing was stashed.";
        log_event(L"info", L"toggle", result.detail.c_str());
        return result;
    }

    if (GetFileAttributesW(stash.c_str()) == INVALID_FILE_ATTRIBUTES && !CreateDirectoryW(stash.c_str(), nullptr)) {
        result.detail = L"Could not create the stash folder '" + stash + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"toggle", result.detail.c_str());
        return result;
    }

    for (const std::wstring& name : present) {
        const std::wstring from = join(steam_path, name);
        const std::wstring to = join(stash, name);
        if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            const DWORD code = GetLastError();
            if (!result.moved.empty()) {
                write_marker(marker, result.moved, error);
            }
            result.detail = L"Stashed " + std::to_wstring(result.moved.size()) + L" file(s), then could not move '" + from + L"' to '" + to + L"'. " + windows_error_text(code);
            log_event(L"error", L"toggle", result.detail.c_str());
            return result;
        }
        result.moved.push_back(name);
        const std::wstring detail = L"Moved '" + from + L"' to '" + to + L"'.";
        log_event(L"info", L"toggle", detail.c_str());
    }

    if (!write_marker(marker, result.moved, error)) {
        result.detail = error;
        log_event(L"error", L"toggle", result.detail.c_str());
        return result;
    }

    result.ok = true;
    result.changed = true;
    result.state = MillenniumState::Stashed;
    result.detail = L"Stashed " + std::to_wstring(result.moved.size()) + L" Millennium hook file(s) into '" + stash + L"'. Steam will start clean; DoomNet may now place its bootstrap.";
    log_event(L"info", L"toggle", result.detail.c_str());
    return result;
}

ToggleResult millennium_on(const std::wstring& steam_path, bool require_steam_closed) {
    ToggleResult result;
    std::wstring error;
    if (!validate_steam_dir(steam_path, error)) {
        result.detail = error;
        log_event(L"error", L"toggle", result.detail.c_str());
        return result;
    }
    if (require_steam_closed) {
        std::wstring which;
        if (steam_is_running(which)) {
            result.detail = L"Refusing to restore Millennium while '" + which + L"' is running. Quit Steam fully first.";
            log_event(L"warn", L"toggle", result.detail.c_str());
            return result;
        }
    }

    const std::wstring stash = join(steam_path, kStashDirName);
    const std::wstring marker = join(stash, kMarkerName);
    if (!file_present(marker)) {
        result.ok = true;
        result.state = millennium_state(steam_path);
        result.detail = L"No stash marker was found at '" + marker + L"', so there was nothing for the Millennium toggle to restore.";
        log_event(L"info", L"toggle", result.detail.c_str());
        return result;
    }

    const std::vector<std::wstring> names = read_marker(marker);
    for (const std::wstring& name : names) {
        if (!is_allowed_hook_name(name) || name.find(L'\\') != std::wstring::npos || name.find(L'/') != std::wstring::npos) {
            result.detail = L"The stash marker '" + marker + L"' named '" + name + L"', which is not one of the four Millennium hook files. Nothing else was restored.";
            log_event(L"error", L"toggle", result.detail.c_str());
            return result;
        }
    }

    for (const std::wstring& name : names) {
        const std::wstring from = join(stash, name);
        const std::wstring to = join(steam_path, name);
        if (!file_present(from)) {
            const std::wstring detail = L"The stashed file '" + from + L"' was already gone, so it was skipped.";
            log_event(L"warn", L"toggle", detail.c_str());
            continue;
        }
        if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            result.detail = L"Restored " + std::to_wstring(result.moved.size()) + L" file(s), then could not move '" + from + L"' back to '" + to + L"'. " + windows_error_text(GetLastError());
            log_event(L"error", L"toggle", result.detail.c_str());
            return result;
        }
        result.moved.push_back(name);
        const std::wstring detail = L"Restored '" + to + L"' from the stash.";
        log_event(L"info", L"toggle", detail.c_str());
    }

    DeleteFileW(marker.c_str());
    RemoveDirectoryW(stash.c_str());  // only succeeds when the stash is empty

    result.ok = true;
    result.changed = !result.moved.empty();
    result.state = millennium_state(steam_path);
    result.detail = L"Restored " + std::to_wstring(result.moved.size()) + L" Millennium hook file(s) to '" + steam_path + L"'. Millennium owns the page again on the next Steam start.";
    log_event(L"info", L"toggle", result.detail.c_str());
    return result;
}

}  // namespace doomnet
