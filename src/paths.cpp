#include "paths.h"
#include "anchors.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace doomnet {
namespace {

bool contains_dotdot(const std::wstring& path) {
    if (path == L".." || path.rfind(L"..\\", 0) == 0 || path.rfind(L"../", 0) == 0) {
        return true;
    }
    if (path.find(L"\\..\\") != std::wstring::npos || path.find(L"/../") != std::wstring::npos || path.find(L"\\../") != std::wstring::npos || path.find(L"/..\\") != std::wstring::npos) {
        return true;
    }
    return path.size() >= 3 && (path.compare(path.size() - 3, 3, L"\\..") == 0 || path.compare(path.size() - 3, 3, L"/..") == 0);
}

bool is_absolute_path(const std::wstring& path) {
    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        return true;
    }
    return path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
}

bool read_env(const wchar_t* name, std::wstring& out) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return false;
    }
    out.assign(needed, L'\0');
    const DWORD wrote = GetEnvironmentVariableW(name, out.data(), needed);
    if (wrote == 0 || wrote >= needed) {
        out.clear();
        return false;
    }
    out.resize(wrote);
    return true;
}

}  // namespace

bool ensure_directory(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    if (CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool data_root(std::wstring& out, std::wstring& error) {
    out.clear();
    error.clear();
    std::wstring override_path;
    if (read_env(L"DOOMNET_ROOT", override_path)) {
        if (!is_absolute_path(override_path) || contains_dotdot(override_path)) {
            error = L"DOOMNET_ROOT is '" + override_path + L"'. It must be an absolute path, such as C:\\Games\\DoomNet, and it must not contain '..'. It was not used, so nothing was written there.";
            return false;
        }
        out = override_path;
        return true;
    }
    std::wstring local;
    if (!read_env(L"LOCALAPPDATA", local) || local.empty()) {
        error = L"LOCALAPPDATA is not set, so DoomNet cannot choose a data folder. Nothing was written.";
        return false;
    }
    if (local.back() == L'\\' || local.back() == L'/') {
        local.pop_back();
    }
    out = local + L"\\DoomNet";
    return true;
}

}  // namespace doomnet
