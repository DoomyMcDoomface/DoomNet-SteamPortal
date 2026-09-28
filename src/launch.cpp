#include "launch.h"

#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace doomnet {
namespace {

std::wstring file_name_of(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    for (wchar_t& ch : name) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return name;
}

}  // namespace

bool launch_hidden(const std::wstring& exe, DWORD timeout_ms, DWORD& exit_code, std::wstring& error) {
    exit_code = 1;
    error.clear();
    if (exe.empty() || exe.find(L"..") != std::wstring::npos) {
        error = L"Refusing to start '" + exe + L"' because the path is empty or contains '..'.";
        log_event(L"error", L"launch", error.c_str());
        return false;
    }
    if (file_name_of(exe) == L"steam.exe") {
        error = L"Refusing to start steam.exe. The inject path is not in place, so DoomNet will not open the Steam client.";
        log_event(L"error", L"launch", error.c_str());
        return false;
    }
    std::wstring command = L"\"" + exe + L"\"";
    std::wstring mutable_command = command;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) {
        error = L"Could not start '" + exe + L"' with no console. " + windows_error_text(GetLastError());
        log_event(L"error", L"launch", error.c_str());
        return false;
    }
    const DWORD waited = WaitForSingleObject(process.hProcess, timeout_ms);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        error = L"Waited " + std::to_wstring(timeout_ms) + L" ms for '" + exe + L"' and it was still running, so it was stopped. Steam was not launched.";
        log_event(L"error", L"launch", error.c_str());
        return false;
    }
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    const std::wstring detail = L"Started '" + exe + L"' with no console. It exited with code " + std::to_wstring(exit_code) + L".";
    log_event(exit_code == 0 ? L"info" : L"error", L"launch", detail.c_str());
    return exit_code == 0;
}

}  // namespace doomnet
