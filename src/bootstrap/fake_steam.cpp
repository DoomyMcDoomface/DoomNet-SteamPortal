// A stand-in for steam.exe. The test copies this to steam.exe beside wsock32.dll
// so the bootstrap loads and its image-name check passes. It waits until the
// hook is installed, then starts steamwebhelper.exe the way the client does.

#include <winsock.h>
#include <windows.h>

#include <string>

namespace {

std::wstring self_dir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring text = path;
    const size_t slash = text.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        text.resize(slash);
    }
    return text;
}

bool wait_ready() {
    const std::wstring name = L"Local\\DoomNetHookReady." + std::to_wstring(GetCurrentProcessId());
    for (int i = 0; i < 50; ++i) {
        HANDLE ready = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
        if (ready != nullptr) {
            const DWORD waited = WaitForSingleObject(ready, 2000);
            CloseHandle(ready);
            return waited == WAIT_OBJECT_0;
        }
        Sleep(20);
    }
    return false;
}

bool launch(const std::wstring& command, PROCESS_INFORMATION& info) {
    std::wstring mutable_line = command;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    return CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) == TRUE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(1, 1), &wsa);
    if (!wait_ready()) {
        return 2;
    }
    const wchar_t* mode = argc >= 2 ? argv[1] : L"broker";
    const std::wstring helper = self_dir() + L"\\steamwebhelper.exe";
    if (_wcsicmp(mode, L"cdp") == 0) {
        LoadLibraryW((self_dir() + L"\\steamui.dll").c_str());
        const std::wstring command = L"\"" + helper + L"\" --doomnet-cdp";
        PROCESS_INFORMATION info{};
        if (!launch(command, info)) {
            return 4;
        }
        WaitForSingleObject(info.hProcess, 20000);
        DWORD code = 1;
        GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        return code == STILL_ACTIVE ? 0 : static_cast<int>(code);
    }

    const std::wstring go_name = L"Local\\DoomNetFakeGo." + std::to_wstring(GetCurrentProcessId());
    HANDLE go = CreateEventW(nullptr, TRUE, FALSE, go_name.c_str());
    if (go == nullptr || WaitForSingleObject(go, 20000) != WAIT_OBJECT_0) {
        return 3;
    }
    CloseHandle(go);

    std::wstring command = L"\"" + helper + L"\"";
    if (_wcsicmp(mode, L"renderer") == 0) {
        command += L" --type=renderer";
    } else if (_wcsicmp(mode, L"flood") == 0) {
        command += L" --doomnet-flood";
    } else {
        command += L" --browser";
    }

    const int launches = _wcsicmp(mode, L"twice") == 0 ? 2 : 1;
    for (int i = 0; i < launches; ++i) {
        PROCESS_INFORMATION info{};
        if (!launch(command, info)) {
            return 4;
        }
        WaitForSingleObject(info.hProcess, 5000);
        DWORD code = 1;
        GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        if (code != 0) {
            return static_cast<int>(code);
        }
    }
    return 0;
}
