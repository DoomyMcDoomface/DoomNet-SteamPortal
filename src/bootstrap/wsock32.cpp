// DoomNet's wsock32.dll. Steam loads this because it sits beside steam.exe and
// the client imports wsock32. Every real socket export is forwarded to ws2_32
// or MSWSOCK (see export.wsock32.def). The only extra behavior, and only when
// the process image is steam.exe, is a hook on CreateProcessInternalW:
// the top-level steamwebhelper launch gets --remote-debugging-pipe and a pair
// of inherited handles. Bytes from those pipes are relayed to
// \\.\pipe\DoomNet.cdp.<steam pid>. If DoomNet is not connected, the bytes are
// drained and discarded so the helper cannot block on a full pipe.

#include "MinHook.h"

// Forwards every wsock32 export at the real system DLL. Kept as pragmas because
// this linker's .def forward syntax does not accept the same list.
#include "export_forwards.inc"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Kept in the binary so steam_attach can tell this DLL from Millennium's and
// from any other wsock32.dll. A wide log line would be stored as UTF-16.
extern "C" const char kDoomNetBootstrapMarker[] = "DoomNetWsock32";
#pragma comment(linker, "/INCLUDE:kDoomNetBootstrapMarker")

namespace {

using CreateProcessInternalW_t = BOOL(WINAPI*)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION, PHANDLE);

CreateProcessInternalW_t g_original = nullptr;
void* g_hook_target = nullptr;
thread_local int g_create_depth = 0;

std::atomic<bool> g_process_stop{false};
std::atomic<bool> g_relay_stop{false};
std::atomic<bool> g_connected{false};
std::atomic<bool> g_logged_discard{false};

HANDLE g_pipe = INVALID_HANDLE_VALUE;
HANDLE g_disconnect = nullptr;
HANDLE g_listening = nullptr;
HANDLE g_parent_read = INVALID_HANDLE_VALUE;
HANDLE g_parent_write = INVALID_HANDLE_VALUE;

std::thread g_accept;
std::thread g_relay;
std::mutex g_mu;
std::string g_pending;
constexpr size_t kPendingCap = 256 * 1024;

std::wstring windows_error_text(unsigned long code) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring words;
    if (length != 0 && buffer != nullptr) {
        words.assign(buffer, length);
        while (!words.empty() && (words.back() == L'\r' || words.back() == L'\n' || words.back() == L' ' || words.back() == L'.')) {
            words.pop_back();
        }
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    wchar_t number[32] = {};
    swprintf_s(number, L"Windows error %lu", code);
    if (words.empty()) {
        return number;
    }
    return std::wstring(number) + L" (" + words + L")";
}

bool contains_dotdot(const std::wstring& path) {
    return path.find(L"..") != std::wstring::npos;
}

bool is_absolute_path(const std::wstring& path) {
    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        return true;
    }
    return path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
}

std::wstring data_directory() {
    wchar_t buffer[32768] = {};
    const DWORD n = GetEnvironmentVariableW(L"DOOMNET_ROOT", buffer, 32768);
    if (n > 0 && n < 32768) {
        std::wstring override_path(buffer, n);
        if (is_absolute_path(override_path) && !contains_dotdot(override_path)) {
            return override_path;
        }
    }
    const DWORD local_n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, 32768);
    if (local_n == 0 || local_n >= 32768) {
        return {};
    }
    std::wstring local(buffer, local_n);
    if (!local.empty() && (local.back() == L'\\' || local.back() == L'/')) {
        local.pop_back();
    }
    return local + L"\\DoomNet";
}

void ensure_dir(const std::wstring& path) {
    CreateDirectoryW(path.c_str(), nullptr);
}

void hook_log(const wchar_t* level, const std::wstring& sentence) {
    const std::wstring root = data_directory();
    if (root.empty()) {
        return;
    }
    ensure_dir(root);
    const std::wstring logs = root + L"\\logs";
    ensure_dir(logs);

    std::wstring text = sentence;
    for (wchar_t& ch : text) {
        if (ch == L'\r' || ch == L'\n') {
            ch = L' ';
        }
    }
    if (text.size() > 3500) {
        text.resize(3500);
        text += L" [truncated]";
    }

    SYSTEMTIME time{};
    GetSystemTime(&time);
    wchar_t stamp[40] = {};
    swprintf_s(stamp, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    const std::wstring line = std::wstring(stamp) + L" " + level + L" pid=" + std::to_wstring(GetCurrentProcessId()) + L" hook: " + text + L"\r\n";

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return;
    }
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), bytes, nullptr, nullptr);

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\DoomNetHookLog");
    if (mutex == nullptr) {
        return;
    }
    if (WaitForSingleObject(mutex, 1000) == WAIT_OBJECT_0) {
        const std::wstring file = logs + L"\\doomnet-hook.log";
        HANDLE handle = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            CloseHandle(handle);
        }
        ReleaseMutex(mutex);
    }
    CloseHandle(mutex);
}

bool contains_i(const wchar_t* haystack, const wchar_t* needle) {
    if (haystack == nullptr || needle == nullptr || needle[0] == L'\0') {
        return false;
    }
    const size_t n = wcslen(needle);
    for (const wchar_t* p = haystack; *p != L'\0'; ++p) {
        if (_wcsnicmp(p, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

std::wstring file_name_of_self() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    const wchar_t* name = path;
    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') {
            name = p + 1;
        }
    }
    return name;
}

void flush_pending_locked() {
    if (g_pending.empty() || g_pipe == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    if (!WriteFile(g_pipe, g_pending.data(), static_cast<DWORD>(g_pending.size()), &written, nullptr)) {
        hook_log(L"warn", L"Could not flush buffered helper bytes to the DoomNet pipe. " + windows_error_text(GetLastError()));
    }
    g_pending.clear();
}

void ensure_accept();
void start_installed_host();

void accept_loop() {
    while (!g_process_stop.load()) {
        SetEvent(g_listening);
        const BOOL connected = ConnectNamedPipe(g_pipe, nullptr);
        const DWORD error = GetLastError();
        if (g_process_stop.load()) {
            break;
        }
        if (!connected && error != ERROR_PIPE_CONNECTED) {
            hook_log(L"warn", L"The DoomNet pipe did not accept a client. " + windows_error_text(error));
            Sleep(50);
            continue;
        }
        ResetEvent(g_listening);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_connected = true;
            flush_pending_locked();
        }
        hook_log(L"info", L"DoomNet connected to \\\\.\\pipe\\DoomNet.cdp." + std::to_wstring(GetCurrentProcessId()) + L".");
        WaitForSingleObject(g_disconnect, INFINITE);
        ResetEvent(g_disconnect);
        g_connected = false;
        DisconnectNamedPipe(g_pipe);
        hook_log(L"info", L"DoomNet disconnected from the debugging pipe. Further helper bytes are drained until it connects again.");
        if (g_process_stop.load()) {
            break;
        }
    }
}

// One thread moves bytes both ways. Two threads cannot block in ReadFile and
// WriteFile on the same non-overlapped named pipe: the reply would wait behind
// the read, and the read would wait for the next command.
void relay_loop() {
    char buffer[16384];
    hook_log(L"info", L"The debugging relay is moving bytes both ways.");
    while (!g_relay_stop.load()) {
        bool moved = false;

        if (g_connected.load() && g_pipe != INVALID_HANDLE_VALUE) {
            DWORD available = 0;
            if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) {
                    g_connected = false;
                }
            } else if (available > 0) {
                const DWORD chunk = available > sizeof(buffer) ? static_cast<DWORD>(sizeof(buffer)) : available;
                DWORD read = 0;
                if (!ReadFile(g_pipe, buffer, chunk, &read, nullptr) || read == 0) {
                    hook_log(L"warn", L"Could not read a DoomNet command from the debugging pipe. " + windows_error_text(GetLastError()));
                    g_connected = false;
                } else {
                    DWORD sent = 0;
                    bool failed = g_parent_write == INVALID_HANDLE_VALUE;
                    while (!failed && sent < read) {
                        DWORD wrote = 0;
                        if (!WriteFile(g_parent_write, buffer + sent, read - sent, &wrote, nullptr) || wrote == 0) {
                            failed = true;
                            break;
                        }
                        sent += wrote;
                    }
                    if (failed) {
                        hook_log(L"warn", L"Could not forward a DoomNet command to steamwebhelper. " + windows_error_text(GetLastError()));
                        break;
                    }
                    moved = true;
                }
            }
        }

        DWORD helper_available = 0;
        if (g_parent_read == INVALID_HANDLE_VALUE) {
            // No helper is attached yet.
        } else if (!PeekNamedPipe(g_parent_read, nullptr, 0, nullptr, &helper_available, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                hook_log(L"info", L"The helper closed its debugging pipe.");
                break;
            }
        } else if (helper_available > 0) {
            const DWORD chunk = helper_available > sizeof(buffer) ? static_cast<DWORD>(sizeof(buffer)) : helper_available;
            DWORD read = 0;
            if (!ReadFile(g_parent_read, buffer, chunk, &read, nullptr) || read == 0) {
                hook_log(L"info", L"The helper closed its debugging pipe.");
                break;
            }
            bool wrote_host = false;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                if (g_connected.load() && g_pipe != INVALID_HANDLE_VALUE) {
                    DWORD sent = 0;
                    wrote_host = true;
                    while (sent < read) {
                        DWORD wrote = 0;
                        if (!WriteFile(g_pipe, buffer + sent, read - sent, &wrote, nullptr) || wrote == 0) {
                            wrote_host = false;
                            break;
                        }
                        sent += wrote;
                    }
                } else if (g_pending.size() < kPendingCap) {
                    const size_t room = kPendingCap - g_pending.size();
                    g_pending.append(buffer, read > room ? room : read);
                } else if (!g_logged_discard.exchange(true)) {
                    hook_log(L"info", L"DoomNet is not connected and the helper has already sent 256 KB, so further debugging bytes are discarded until it connects.");
                }
            }
            if (g_connected.load() && !wrote_host) {
                hook_log(L"warn", L"Could not relay helper bytes to DoomNet. " + windows_error_text(GetLastError()));
                g_connected = false;
            }
            moved = true;
        }

        if (!moved) {
            Sleep(5);
        }
    }
}

void stop_relay() {
    g_relay_stop = true;
    if (g_relay.joinable()) {
        CancelSynchronousIo(g_relay.native_handle());
        g_relay.join();
    }
    g_relay_stop = false;
    if (g_parent_read != INVALID_HANDLE_VALUE) {
        CloseHandle(g_parent_read);
        g_parent_read = INVALID_HANDLE_VALUE;
    }
    if (g_parent_write != INVALID_HANDLE_VALUE) {
        CloseHandle(g_parent_write);
        g_parent_write = INVALID_HANDLE_VALUE;
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        // Bytes from the previous helper are not part of the new one. The DoomNet
        // client stays connected; only the helper pipes are replaced.
        g_pending.clear();
        g_logged_discard = false;
    }
}

void start_relay(HANDLE parent_read, HANDLE parent_write) {
    g_parent_read = parent_read;
    g_parent_write = parent_write;
    g_relay_stop = false;
    g_relay = std::thread(relay_loop);
}

bool contains_switch(const std::wstring& line, const wchar_t* name) {
    return contains_i(line.c_str(), name);
}

BOOL WINAPI hooked_create_process(HANDLE token, LPCWSTR application, LPWSTR command_line, LPSECURITY_ATTRIBUTES process_attr, LPSECURITY_ATTRIBUTES thread_attr, BOOL inherit_handles, DWORD flags, LPVOID environment, LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process_info, PHANDLE new_token) {
    if (g_original == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    // CreateProcessW below re-enters this hook. The inner call must run the
    // real function so Windows, not the trampoline, applies handle inheritance.
    if (g_create_depth > 0) {
        return g_original(token, application, command_line, process_attr, thread_attr, inherit_handles, flags, environment, directory, startup, process_info, new_token);
    }
    // Steam sometimes passes the exe only as lpApplicationName. Either place is enough.
    const bool named_in_app = application != nullptr && contains_i(application, L"steamwebhelper");
    const bool named_in_line = command_line != nullptr && contains_i(command_line, L"steamwebhelper");
    if (!named_in_app && !named_in_line) {
        return g_original(token, application, command_line, process_attr, thread_attr, inherit_handles, flags, environment, directory, startup, process_info, new_token);
    }
    if (command_line != nullptr && contains_i(command_line, L"--type=")) {
        hook_log(L"info", L"A steamwebhelper launch includes --type=, so it is a subprocess and was left unchanged.");
        return g_original(token, application, command_line, process_attr, thread_attr, inherit_handles, flags, environment, directory, startup, process_info, new_token);
    }

    ensure_accept();
    stop_relay();

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE child_read = INVALID_HANDLE_VALUE;
    HANDLE parent_write = INVALID_HANDLE_VALUE;
    HANDLE parent_read = INVALID_HANDLE_VALUE;
    HANDLE child_write = INVALID_HANDLE_VALUE;
    const bool pipes_ok = CreatePipe(&child_read, &parent_write, &inheritable, 0) && CreatePipe(&parent_read, &child_write, &inheritable, 0);
    if (!pipes_ok) {
        hook_log(L"error", L"Could not create the debugging pipes for steamwebhelper. The launch was left unchanged. " + windows_error_text(GetLastError()));
        if (child_read != INVALID_HANDLE_VALUE) {
            CloseHandle(child_read);
        }
        if (parent_write != INVALID_HANDLE_VALUE) {
            CloseHandle(parent_write);
        }
        return g_original(token, application, command_line, process_attr, thread_attr, inherit_handles, flags, environment, directory, startup, process_info, new_token);
    }
    SetHandleInformation(parent_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parent_read, HANDLE_FLAG_INHERIT, 0);

    static thread_local std::wstring rewritten;
    if (command_line != nullptr) {
        rewritten = command_line;
    } else {
        rewritten = L"\"" + std::wstring(application) + L"\"";
    }
    if (!contains_switch(rewritten, L"--remote-debugging-pipe")) {
        rewritten += L" --remote-debugging-pipe";
    }
    if (!contains_switch(rewritten, L"--remote-debugging-io-pipes=")) {
        rewritten += L" --remote-debugging-io-pipes=" + std::to_wstring(reinterpret_cast<uintptr_t>(child_read)) + L"," + std::to_wstring(reinterpret_cast<uintptr_t>(child_write));
    }

    hook_log(L"info", L"Rewrote the steamwebhelper broker launch. Child read handle " + std::to_wstring(reinterpret_cast<uintptr_t>(child_read)) + L", child write handle " + std::to_wstring(reinterpret_cast<uintptr_t>(child_write)) + L".");

    // Call the real CreateProcessW with the hook disabled. The trampoline does
    // not reliably pass the inherit flag, so the helper would not receive the pipes.
    HANDLE inherited[2] = {child_read, child_write};
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<char> attribute_storage(attribute_bytes);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    const bool listed = InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes) &&
                        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);

    STARTUPINFOEXW extended{};
    extended.StartupInfo.cb = sizeof(extended);
    extended.lpAttributeList = attributes;
    g_create_depth++;
    if (g_hook_target != nullptr) {
        MH_DisableHook(g_hook_target);
    }
    BOOL created = FALSE;
    if (listed) {
        created = CreateProcessW(nullptr, rewritten.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, reinterpret_cast<LPSTARTUPINFOW>(&extended), process_info);
    } else {
        hook_log(L"warn", L"Could not restrict inherited handles for steamwebhelper, so the launch inherited every inheritable handle. " + windows_error_text(GetLastError()));
        STARTUPINFOW plain{};
        plain.cb = sizeof(plain);
        created = CreateProcessW(nullptr, rewritten.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &plain, process_info);
    }
    if (g_hook_target != nullptr) {
        MH_EnableHook(g_hook_target);
    }
    g_create_depth--;
    if (listed) {
        DeleteProcThreadAttributeList(attributes);
    }

    CloseHandle(child_read);
    CloseHandle(child_write);
    if (!created) {
        hook_log(L"error", L"steamwebhelper did not start after the command line was rewritten. " + windows_error_text(GetLastError()));
        CloseHandle(parent_read);
        CloseHandle(parent_write);
        return FALSE;
    }

    const DWORD pid = process_info != nullptr ? process_info->dwProcessId : 0;
    hook_log(L"info", L"steamwebhelper started (pid " + std::to_wstring(pid) + L"). Debugging bytes are relayed on \\\\.\\pipe\\DoomNet.cdp." + std::to_wstring(GetCurrentProcessId()) + L".");
    start_relay(parent_read, parent_write);
    start_installed_host();
    return TRUE;
}

bool install_hook() {
    const MH_STATUS ready = MH_Initialize();
    if (ready != MH_OK && ready != MH_ERROR_ALREADY_INITIALIZED) {
        hook_log(L"error", L"The hook library did not start, so steamwebhelper launches will not be rewritten.");
        return false;
    }
    void* target = nullptr;
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase != nullptr) {
        target = reinterpret_cast<void*>(GetProcAddress(kernelbase, "CreateProcessInternalW"));
    }
    if (target == nullptr) {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (kernel32 != nullptr) {
            target = reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateProcessInternalW"));
        }
    }
    if (target == nullptr) {
        hook_log(L"error", L"CreateProcessInternalW was not found in kernelbase.dll or kernel32.dll, so steamwebhelper launches will not be rewritten.");
        return false;
    }
    g_hook_target = target;
    const MH_STATUS created = MH_CreateHook(target, reinterpret_cast<void*>(&hooked_create_process), reinterpret_cast<void**>(&g_original));
    if (created != MH_OK) {
        hook_log(L"error", L"The CreateProcessInternalW hook could not be created, so steamwebhelper launches will not be rewritten.");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        hook_log(L"error", L"The CreateProcessInternalW hook could not be enabled, so steamwebhelper launches will not be rewritten.");
        return false;
    }
    hook_log(L"info", L"The CreateProcessInternalW hook is installed. A top-level steamwebhelper launch will get debugging pipes.");
    return true;
}

void start_installed_host() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (GetEnvironmentVariableW(L"DOOMNET_ROOT", nullptr, 0) > 0) {
            hook_log(L"info", L"DOOMNET_ROOT is set, so this steam.exe did not start the installed host.");
            return;
        }
        wchar_t image[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, image, MAX_PATH) == 0) {
            return;
        }
        std::wstring folder = image;
        const size_t slash = folder.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            return;
        }
        folder.resize(slash);
        const DWORD ui = GetFileAttributesW((folder + L"\\steamui").c_str());
        if (ui == INVALID_FILE_ATTRIBUTES || (ui & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            hook_log(L"info", L"'" + folder + L"' has no steamui folder, so the host was not started.");
            return;
        }
        HANDLE held = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\DoomNet");
        if (held != nullptr) {
            CloseHandle(held);
            hook_log(L"info", L"Local\\DoomNet is already held, so this Steam launch did not start another host.");
            return;
        }
        wchar_t local_app[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app, MAX_PATH) == 0) {
            hook_log(L"error", L"LOCALAPPDATA is not set, so the host was not started.");
            return;
        }
        const std::wstring exe = std::wstring(local_app) + L"\\DoomNet\\DoomNet.exe";
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            hook_log(L"error", L"'" + exe + L"' is not there, so this Steam launch has the bootstrap but not the host.");
            return;
        }
        std::wstring command = L"\"" + exe + L"\" --attach";
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(0);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            hook_log(L"error", L"Could not start '" + exe + L"' so this Steam launch would load the player and the skin. " + windows_error_text(GetLastError()));
            return;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        hook_log(L"info", L"Started '" + exe + L"' with --attach so this Steam launch loads the player and the skin.");
    });
}

bool create_pipe_server() {
    const std::wstring name = L"\\\\.\\pipe\\DoomNet.cdp." + std::to_wstring(GetCurrentProcessId());
    g_pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 1u << 20, 1u << 20, 0, nullptr);
    if (g_pipe == INVALID_HANDLE_VALUE) {
        hook_log(L"error", L"Could not create " + name + L". " + windows_error_text(GetLastError()));
        return false;
    }
    g_disconnect = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_listening = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_disconnect == nullptr || g_listening == nullptr) {
        hook_log(L"error", L"Could not create the pipe events. " + windows_error_text(GetLastError()));
        return false;
    }
    // The accept thread starts on the first broker launch, not here. Starting a
    // thread from DllMain deadlocks the loader lock, so steam.exe never reaches main.
    hook_log(L"info", L"Created " + name + L". It will accept DoomNet when steamwebhelper is launched.");
    return true;
}

void ensure_accept() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (g_pipe == INVALID_HANDLE_VALUE) {
            return;
        }
        g_accept = std::thread(accept_loop);
    });
}

void bootstrap() {
    const std::wstring name = file_name_of_self();
    if (_wcsicmp(name.c_str(), L"steam.exe") != 0) {
        hook_log(L"info", L"This process is '" + name + L"', not steam.exe, so the bootstrap did not install a hook.");
        return;
    }
    hook_log(L"info", L"Loaded into steam.exe. Socket exports are forwarded. The hook is installed next.");
    if (!install_hook()) {
        return;
    }
    create_pipe_server();
    const std::wstring ready_name = L"Local\\DoomNetHookReady." + std::to_wstring(GetCurrentProcessId());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
    if (ready != nullptr) {
        SetEvent(ready);
        // The event stays alive for the process. Closing it would drop the name
        // before the test opens it. Leak of one event per Steam launch is fine.
    }
}

void shutdown_bootstrap() {
    g_process_stop = true;
    g_relay_stop = true;
    if (g_relay.joinable()) {
        CancelSynchronousIo(g_relay.native_handle());
        g_relay.join();
    }
    if (g_disconnect != nullptr) {
        SetEvent(g_disconnect);
    }
    if (g_accept.joinable()) {
        CancelSynchronousIo(g_accept.native_handle());
    }
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    if (g_accept.joinable()) {
        if (WaitForSingleObject(g_accept.native_handle(), 1000) == WAIT_OBJECT_0) {
            g_accept.join();
        } else {
            g_accept.detach();
        }
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Skip thread attach notifications so the pipe thread started below does
        // not re-enter the loader lock while this call still holds it.
        DisableThreadLibraryCalls(module);
        bootstrap();
    } else if (reason == DLL_PROCESS_DETACH) {
        shutdown_bootstrap();
    }
    return TRUE;
}
