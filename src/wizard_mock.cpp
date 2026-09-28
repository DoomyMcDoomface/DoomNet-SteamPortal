// Opens the setup preview. DoomNetSetup.exe is a windowed program, so Windows
// Terminal does not open a PowerShell console beside the wizard.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace {

std::wstring exe_directory() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L"";
    }
    std::wstring full(path, length);
    const auto slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L"";
    }
    return full.substr(0, slash);
}

bool file_exists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring find_setup() {
    std::wstring dir = exe_directory();
    for (int depth = 0; depth < 5 && !dir.empty(); ++depth) {
        const std::wstring beside = dir + L"\\DoomNetSetup.exe";
        const std::wstring release = dir + L"\\build\\Release\\DoomNetSetup.exe";
        if (file_exists(beside)) {
            return beside;
        }
        if (file_exists(release)) {
            return release;
        }
        const auto slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            break;
        }
        dir.resize(slash);
    }
    return L"";
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const std::wstring setup = find_setup();
    if (setup.empty()) {
        MessageBoxW(nullptr, L"Could not find DoomNetSetup.exe.", L"DoomNet Setup", MB_OK | MB_ICONERROR);
        return 1;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(setup.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        MessageBoxW(nullptr, L"Could not open the setup window.", L"DoomNet Setup", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
