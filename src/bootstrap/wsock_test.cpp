// Proves the wsock32 bootstrap without Steam. A copied steam.exe loads the DLL,
// launches a copied steamwebhelper.exe, and the test talks through the relay pipe.

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failed = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failed;
        std::printf("FAIL  %s\n", message.c_str());
    }
}

std::wstring exe_dir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring text = path;
    const size_t slash = text.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        text.resize(slash);
    }
    return text;
}

std::wstring temp_root() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    std::wstring path = std::wstring(temp) + L"doomnet-wsock-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

bool copy_as(const std::wstring& from, const std::wstring& to) {
    return CopyFileW(from.c_str(), to.c_str(), FALSE) == TRUE;
}

std::string read_file(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

struct Child {
    PROCESS_INFORMATION info{};
    ~Child() {
        if (info.hProcess != nullptr) {
            if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(info.hProcess, 1);
                WaitForSingleObject(info.hProcess, 2000);
            }
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
        }
    }
};

bool start_steam(const std::wstring& steam_exe, const std::wstring& arg, const std::wstring& root, Child& child) {
    std::wstring command = L"\"" + steam_exe + L"\"";
    if (!arg.empty()) {
        command += L" " + arg;
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    // The child inherits DOOMNET_ROOT so the hook log lands in this test's folder.
    // Build a private environment block so the parent test keeps its own root.
    std::wstring env;
    wchar_t* existing = GetEnvironmentStringsW();
    if (existing != nullptr) {
        for (wchar_t* p = existing; *p != L'\0'; p += wcslen(p) + 1) {
            if (_wcsnicmp(p, L"DOOMNET_ROOT=", 13) == 0) {
                continue;
            }
            env.append(p);
            env.push_back(L'\0');
        }
        FreeEnvironmentStringsW(existing);
    }
    env += L"DOOMNET_ROOT=" + root;
    env.push_back(L'\0');
    env += L"DOOMNET_CMDLINE_OUT=" + root + L"\\cmdline.txt";
    env.push_back(L'\0');
    env += L"DOOMNET_REPLY_OUT=" + root + L"\\reply.txt";
    env.push_back(L'\0');
    env.push_back(L'\0');

    return CreateProcessW(steam_exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.data(), nullptr, &startup, &child.info) == TRUE;
}

HANDLE open_go(DWORD pid) {
    const std::wstring name = L"Local\\DoomNetFakeGo." + std::to_wstring(pid);
    for (int i = 0; i < 100; ++i) {
        HANDLE go = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
        if (go != nullptr) {
            return go;
        }
        Sleep(20);
    }
    return nullptr;
}

HANDLE open_pipe(DWORD pid) {
    const std::wstring name = L"\\\\.\\pipe\\DoomNet.cdp." + std::to_wstring(pid);
    for (int i = 0; i < 100; ++i) {
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            return pipe;
        }
        Sleep(20);
    }
    return INVALID_HANDLE_VALUE;
}

bool read_exact(HANDLE pipe, char* out, DWORD count) {
    DWORD got_total = 0;
    while (got_total < count) {
        DWORD got = 0;
        if (!ReadFile(pipe, out + got_total, count - got_total, &got, nullptr) || got == 0) {
            return false;
        }
        got_total += got;
    }
    return true;
}

void test_direct_inherit(const std::wstring& bin) {
    std::printf("test  anonymous pipe inheritance without the hook\n");
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE child_read = nullptr;
    HANDLE parent_write = nullptr;
    HANDLE parent_read = nullptr;
    HANDLE child_write = nullptr;
    check(CreatePipe(&child_read, &parent_write, &inheritable, 0) && CreatePipe(&parent_read, &child_write, &inheritable, 0), "CreatePipe");
    SetHandleInformation(parent_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parent_read, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = L"\"" + bin + L"\\DoomNetFakeWebHelper.exe\" --browser --remote-debugging-io-pipes=" + std::to_wstring(reinterpret_cast<uintptr_t>(child_read)) + L"," + std::to_wstring(reinterpret_cast<uintptr_t>(child_write));
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    check(created == TRUE, "direct CreateProcess");
    DWORD available = 0;
    for (int i = 0; i < 20 && available == 0; ++i) {
        PeekNamedPipe(parent_read, nullptr, 0, nullptr, &available, nullptr);
        Sleep(50);
    }
    check(available >= 17, "direct inherit delivered " + std::to_string(available) + " bytes");
    if (info.hProcess != nullptr) {
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }
    CloseHandle(child_read);
    CloseHandle(child_write);
    CloseHandle(parent_read);
    CloseHandle(parent_write);
}

void test_broker(const std::wstring& bin) {
    std::printf("test  broker launch is rewritten and the relay carries bytes\n");
    const std::wstring root = temp_root();
    check(copy_as(bin + L"\\wsock32.dll", root + L"\\wsock32.dll"), "copy wsock32.dll");
    check(copy_as(bin + L"\\DoomNetFakeSteam.exe", root + L"\\steam.exe"), "copy steam.exe");
    check(copy_as(bin + L"\\DoomNetFakeWebHelper.exe", root + L"\\steamwebhelper.exe"), "copy steamwebhelper.exe");

    Child steam;
    check(start_steam(root + L"\\steam.exe", L"broker", root, steam), "start fake steam.exe");
    if (steam.info.hProcess == nullptr) {
        return;
    }
    HANDLE go = open_go(steam.info.dwProcessId);
    check(go != nullptr, "fake steam published its go event");
    if (go != nullptr) {
        SetEvent(go);
        CloseHandle(go);
    }
    // The accept thread starts inside the launch hook, so connect after the go signal.
    HANDLE pipe = open_pipe(steam.info.dwProcessId);
    check(pipe != INVALID_HANDLE_VALUE, "connected to \\\\.\\pipe\\DoomNet.cdp.<pid>");
    char hello[17] = {};
    const bool got_hello = pipe != INVALID_HANDLE_VALUE && read_exact(pipe, hello, 17);
    check(got_hello && std::string(hello, 17) == "hello-from-helper", std::string("greeting was '") + hello + "'");
    const char ping[] = "ping-from-host";
    DWORD written = 0;
    check(pipe != INVALID_HANDLE_VALUE && WriteFile(pipe, ping, sizeof(ping) - 1, &written, nullptr) && written == sizeof(ping) - 1, "wrote the host ping");
    check(WaitForSingleObject(steam.info.hProcess, 6000) == WAIT_OBJECT_0, "fake steam exited");
    const std::string cmdline = read_file(root + L"\\cmdline.txt");
    check(cmdline.find("--remote-debugging-pipe") != std::string::npos, "command line has --remote-debugging-pipe");
    check(cmdline.find("--remote-debugging-io-pipes=") != std::string::npos, "command line has the pipe handles");
    check(cmdline.find("--browser") != std::string::npos, "the original argument survived");
    const std::string reply = read_file(root + L"\\reply.txt");
    check(reply.find("ping-from-host") != std::string::npos, "the helper received the host ping");
    const std::string log = read_file(root + L"\\logs\\doomnet-hook.log");
    check(log.find("hook: ") != std::string::npos && log.find("CreateProcessInternalW hook is installed") != std::string::npos, "hook log says the hook is installed");
    if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
    }
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_renderer_unchanged(const std::wstring& bin) {
    std::printf("test  a --type= subprocess is not rewritten\n");
    const std::wstring root = temp_root();
    copy_as(bin + L"\\wsock32.dll", root + L"\\wsock32.dll");
    copy_as(bin + L"\\DoomNetFakeSteam.exe", root + L"\\steam.exe");
    copy_as(bin + L"\\DoomNetFakeWebHelper.exe", root + L"\\steamwebhelper.exe");
    Child steam;
    check(start_steam(root + L"\\steam.exe", L"renderer", root, steam), "start fake steam for renderer");
    HANDLE go = steam.info.hProcess != nullptr ? open_go(steam.info.dwProcessId) : nullptr;
    if (go != nullptr) {
        SetEvent(go);
        CloseHandle(go);
    }
    check(steam.info.hProcess != nullptr && WaitForSingleObject(steam.info.hProcess, 15000) == WAIT_OBJECT_0, "renderer launch finished");
    const std::string cmdline = read_file(root + L"\\cmdline.txt");
    check(cmdline.find("--type=renderer") != std::string::npos, "subprocess kept --type=renderer");
    check(cmdline.find("--remote-debugging-pipe") == std::string::npos, "subprocess did not get debugging pipes");
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_flood_without_host(const std::wstring& bin) {
    std::printf("test  helper output is drained when DoomNet is not connected\n");
    const std::wstring root = temp_root();
    copy_as(bin + L"\\wsock32.dll", root + L"\\wsock32.dll");
    copy_as(bin + L"\\DoomNetFakeSteam.exe", root + L"\\steam.exe");
    copy_as(bin + L"\\DoomNetFakeWebHelper.exe", root + L"\\steamwebhelper.exe");
    Child steam;
    check(start_steam(root + L"\\steam.exe", L"flood", root, steam), "start fake steam for flood");
    HANDLE go = steam.info.hProcess != nullptr ? open_go(steam.info.dwProcessId) : nullptr;
    if (go != nullptr) {
        SetEvent(go);
        CloseHandle(go);
    }
    const DWORD waited = steam.info.hProcess != nullptr ? WaitForSingleObject(steam.info.hProcess, 15000) : WAIT_TIMEOUT;
    check(waited == WAIT_OBJECT_0, "flood finished without a connected host (a stuck pipe would still be running)");
    DWORD code = 1;
    if (steam.info.hProcess != nullptr) {
        GetExitCodeProcess(steam.info.hProcess, &code);
    }
    check(code == 0, "flood helper exited 0");
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_not_steam(const std::wstring& bin) {
    std::printf("test  the DLL does nothing in a process that is not steam.exe\n");
    const std::wstring root = temp_root();
    copy_as(bin + L"\\wsock32.dll", root + L"\\wsock32.dll");
    copy_as(bin + L"\\DoomNetFakeSteam.exe", root + L"\\DoomNetFakeSteam.exe");
    copy_as(bin + L"\\DoomNetFakeWebHelper.exe", root + L"\\steamwebhelper.exe");
    Child other;
    check(start_steam(root + L"\\DoomNetFakeSteam.exe", L"broker", root, other), "start a process that is not named steam.exe");
    // It still waits for the hook-ready event, which the DLL does not create
    // when the image is not steam.exe, and then exits 2.
    const DWORD waited = other.info.hProcess != nullptr ? WaitForSingleObject(other.info.hProcess, 5000) : WAIT_TIMEOUT;
    check(waited == WAIT_OBJECT_0, "non-steam process exited");
    DWORD code = 0;
    if (other.info.hProcess != nullptr) {
        GetExitCodeProcess(other.info.hProcess, &code);
    }
    check(code == 2, "non-steam process did not skip the hook (exit " + std::to_string(code) + ")");
    const std::string log = read_file(root + L"\\logs\\doomnet-hook.log");
    check(log.find("not steam.exe") != std::string::npos, "hook log names the process that was skipped");
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

}  // namespace

int main() {
    const std::wstring bin = exe_dir();
    test_direct_inherit(bin);
    test_broker(bin);
    test_renderer_unchanged(bin);
    test_flood_without_host(bin);
    test_not_steam(bin);
    std::printf("%s\n", g_failed == 0 ? "ok" : "failed");
    return g_failed == 0 ? 0 : 1;
}
