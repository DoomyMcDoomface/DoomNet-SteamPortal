// Spec: What makes the inject succeed.
// The hook lives in wsock32.dll. This file decides whether that DLL may be
// copied beside steam.exe, and it can start a steam.exe that is not the
// discovered install. It refuses the installed client unless the caller
// explicitly allows a live run. launch_hidden is not used.

#include "steam_attach.h"

#include "anchors.h"
#include "log.h"
#include "page_bridge.h"
#include "skin_page.h"
#include "steam_discover.h"
#include "toggle.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace doomnet {

void steam_attach_anchor() {}

namespace {

const char kDoomNetMarker[] = "DoomNetWsock32";

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

bool file_contains(const std::wstring& path, const char* marker) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
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
    return bytes.find(marker) != std::string::npos;
}

bool is_doomnet_bootstrap(const std::wstring& path) {
    return file_contains(path, kDoomNetMarker);
}

}  // namespace

PlaceBootstrapResult place_doomnet_bootstrap(const std::wstring& steam_path, const std::wstring& dll_source, bool require_steam_closed) {
    PlaceBootstrapResult result;
    if (steam_path.empty() || dll_source.empty()) {
        result.detail = L"No Steam folder or no bootstrap DLL was given, so wsock32.dll was not copied.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    if (steam_path.find(L"..") != std::wstring::npos || dll_source.find(L"..") != std::wstring::npos) {
        result.detail = L"Refusing to copy the bootstrap because a path contains '..'.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    if (!directory_has_steam_exe(steam_path)) {
        result.detail = L"'" + steam_path + L"' does not contain steam.exe, so the bootstrap was not copied.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    wchar_t windows_dir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(windows_dir, MAX_PATH) != 0 && inside_directory(steam_path, windows_dir)) {
        result.detail = L"'" + steam_path + L"' is inside the Windows directory, so the bootstrap was not copied.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    const DWORD source_attrs = GetFileAttributesW(dll_source.c_str());
    if (source_attrs == INVALID_FILE_ATTRIBUTES || (source_attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        result.detail = L"The bootstrap DLL '" + dll_source + L"' is not a file, so nothing was copied.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    if (!is_doomnet_bootstrap(dll_source)) {
        result.detail = L"'" + dll_source + L"' is not DoomNet's bootstrap, so it was not copied beside steam.exe.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    if (require_steam_closed) {
        std::wstring which;
        if (steam_is_running(which)) {
            result.detail = L"Refusing to copy the bootstrap while '" + which + L"' is running. Quit Steam fully first.";
            log_event(L"warn", L"inject", result.detail.c_str());
            return result;
        }
    }

    const MillenniumState state = millennium_state(steam_path);
    if (state == MillenniumState::Active || state == MillenniumState::Unknown) {
        result.detail = L"Refusing to copy the bootstrap into '" + steam_path + L"' because Millennium's hooks are still beside steam.exe. Stash them first.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }

    const std::wstring destination = steam_path + L"\\wsock32.dll";
    const DWORD dest_attrs = GetFileAttributesW(destination.c_str());
    const bool dest_exists = dest_attrs != INVALID_FILE_ATTRIBUTES && (dest_attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    if (dest_exists && is_millennium_bootstrap(destination)) {
        result.detail = L"Refusing to replace '" + destination + L"' because it is Millennium's bootstrap. Stash it first.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    if (dest_exists && !is_doomnet_bootstrap(destination)) {
        result.detail = L"Refusing to replace '" + destination + L"' because it is not DoomNet's bootstrap and it is not Millennium's either.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }

    if (!CopyFileW(dll_source.c_str(), destination.c_str(), FALSE)) {
        result.detail = L"Could not copy '" + dll_source + L"' to '" + destination + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    result.ok = true;
    result.replaced = dest_exists;
    result.detail = dest_exists ? L"Replaced DoomNet's bootstrap at '" + destination + L"'."
                                : L"Copied DoomNet's bootstrap to '" + destination + L"'.";
    log_event(L"info", L"inject", result.detail.c_str());
    return result;
}

bool module_loaded(unsigned long pid, const wchar_t* file_name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, file_name) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

// The debugging relay starts as the broker process is created. A command written
// in that window is dropped, so injection waits until the process exists.
bool child_process_named(unsigned long parent, const wchar_t* file_name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID == parent && _wcsicmp(entry.szExeFile, file_name) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

void terminate_process_tree(void* process, unsigned long pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ParentProcessID == pid) {
                    HANDLE child = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
                    if (child != nullptr) {
                        TerminateProcess(child, 1);
                        CloseHandle(child);
                    }
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    if (process != nullptr) {
        TerminateProcess(static_cast<HANDLE>(process), 1);
    }
}

std::wstring parent_dir(const std::wstring& path) {
    const std::wstring full = full_path(path);
    const size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    return full.substr(0, slash);
}

InjectResult launch_and_inject_boot(const std::wstring& steam_exe, const std::wstring& arguments, bool allow_real_client) {
    InjectResult result;
    if (steam_exe.empty() || steam_exe.find(L"..") != std::wstring::npos) {
        result.detail = L"Refusing to start Steam because the exe path is empty or contains '..'.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    const std::wstring exe = full_path(steam_exe);
    const std::wstring folder = parent_dir(exe);
    if (!directory_has_steam_exe(folder)) {
        result.detail = L"'" + folder + L"' does not contain steam.exe, so Steam was not started.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    std::wstring installed;
    if (!allow_real_client && discover_steam_path(installed) && inside_directory(exe, installed)) {
        result.detail = L"Refusing to start the installed Steam client at '" + installed + L"'. Live proof is a separate step.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    const MillenniumState state = millennium_state(folder);
    if (state == MillenniumState::Active) {
        result.detail = L"Refusing to start '" + exe + L"' because Millennium's hooks are still beside steam.exe.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    const std::wstring bootstrap = folder + L"\\wsock32.dll";
    if (!is_doomnet_bootstrap(bootstrap)) {
        result.detail = L"'" + bootstrap + L"' is not DoomNet's bootstrap, so Steam was not started.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }

    std::wstring command = L"\"" + exe + L"\"";
    if (!arguments.empty()) {
        command += L" " + arguments;
    }
    std::vector<wchar_t> mutable_line(command.begin(), command.end());
    mutable_line.push_back(0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    DWORD creation_flags = 0;
    if (!allow_real_client) {
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        creation_flags = CREATE_NO_WINDOW;
    } else if (skin_tags_ready()) {
        // Stay hidden until the skin is in the documents. Steam paints its own
        // windows later; hold_client_windows puts those back behind the desktop.
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
    } else {
        // A hidden parent would otherwise hand Steam SW_HIDE, so the client
        // process starts and never puts a window on the desktop.
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_SHOWNORMAL;
    }
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exe.c_str(), mutable_line.data(), nullptr, nullptr, FALSE, creation_flags, nullptr, folder.c_str(), &startup, &info)) {
        result.detail = L"Could not start '" + exe + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    CloseHandle(info.hThread);
    result.pid = info.dwProcessId;
    result.process = info.hProcess;
    if (allow_real_client && skin_tags_ready()) {
        hold_client_windows(result.pid);
    }
    log_event(L"info", L"inject", (L"Started '" + exe + L"' (pid " + std::to_wstring(result.pid) + L"). Waiting for steamui.dll and steamwebhelper.exe.").c_str());

    const DWORD started = GetTickCount();
    bool saw_ui = false;
    bool saw_helper = false;
    while (GetTickCount() - started < 90000) {
        if (WaitForSingleObject(info.hProcess, 0) == WAIT_OBJECT_0) {
        result.detail = L"'" + exe + L"' exited before the boot script could be injected.";
        log_event(L"error", L"inject", result.detail.c_str());
        drop_client_windows();
        CloseHandle(info.hProcess);
            result.process = nullptr;
            return result;
        }
        if (!saw_ui && module_loaded(result.pid, L"steamui.dll")) {
            saw_ui = true;
            log_event(L"info", L"inject", (L"steamui.dll is loaded in pid " + std::to_wstring(result.pid) + L".").c_str());
        }
        if (!saw_helper && child_process_named(result.pid, L"steamwebhelper.exe")) {
            saw_helper = true;
            log_event(L"info", L"inject", (L"steamwebhelper.exe is running under pid " + std::to_wstring(result.pid) + L".").c_str());
        }
        if (saw_ui && saw_helper) {
            // CreateProcess has returned and the relay threads are running.
            Sleep(100);
            break;
        }
        Sleep(50);
    }
    if (!saw_ui || !saw_helper) {
        result.detail = L"pid " + std::to_wstring(result.pid) + (saw_ui ? L" loaded steamui.dll but steamwebhelper.exe did not start" : L" did not load steamui.dll") + L" within 90 seconds. The process was stopped.";
        log_event(L"error", L"inject", result.detail.c_str());
        drop_client_windows();
        terminate_process_tree(info.hProcess, result.pid);
        CloseHandle(info.hProcess);
        result.process = nullptr;
        return result;
    }

    CdpPipe pipe;
    std::wstring error;
    if (!pipe.connect_steam(result.pid, error)) {
        result.detail = error;
        drop_client_windows();
        terminate_process_tree(info.hProcess, result.pid);
        CloseHandle(info.hProcess);
        result.process = nullptr;
        return result;
    }
    if (!inject_boot_script(pipe, boot_script_for_page(), result.session_id, error)) {
        result.detail = error;
        pipe.close();
        drop_client_windows();
        terminate_process_tree(info.hProcess, result.pid);
        CloseHandle(info.hProcess);
        result.process = nullptr;
        return result;
    }
    result.pipe = std::move(pipe);
    result.ok = true;
    result.detail = L"The boot script was injected into the page target of pid " + std::to_wstring(result.pid) + L".";
    log_event(L"info", L"inject", result.detail.c_str());
    if (allow_real_client && !skin_tags_ready()) {
        const int shown = show_client_windows(result.pid);
        log_event(L"info", L"inject", (L"Brought " + std::to_wstring(shown) + L" Steam window(s) to the desktop for pid " + std::to_wstring(result.pid) + L".").c_str());
    }
    return result;
}

unsigned long find_steam_pid(const std::wstring& steam_exe) {
    const std::wstring wanted = full_path(steam_exe);
    if (wanted.empty()) {
        return 0;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    unsigned long found = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"steam.exe") != 0) {
                continue;
            }
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process == nullptr) {
                continue;
            }
            wchar_t image[MAX_PATH] = {};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, image, &size) && _wcsicmp(image, wanted.c_str()) == 0) {
                found = entry.th32ProcessID;
            }
            CloseHandle(process);
            if (found != 0) {
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

InjectResult inject_running_client(const std::wstring& steam_exe) {
    InjectResult result;
    if (steam_exe.empty() || steam_exe.find(L"..") != std::wstring::npos) {
        result.detail = L"Refusing to attach because the Steam exe path is empty or contains '..'.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    const std::wstring exe = full_path(steam_exe);
    const std::wstring folder = parent_dir(exe);
    const std::wstring bootstrap = folder + L"\\wsock32.dll";
    if (!is_doomnet_bootstrap(bootstrap)) {
        result.detail = L"'" + bootstrap + L"' is not DoomNet's bootstrap, so the running Steam was not injected.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    const MillenniumState state = millennium_state(folder);
    if (state == MillenniumState::Active) {
        result.detail = L"Refusing to attach to '" + exe + L"' because Millennium's hooks are still beside steam.exe.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }

    const DWORD started = GetTickCount();
    unsigned long pid = 0;
    bool saw_ui = false;
    bool saw_helper = false;
    while (GetTickCount() - started < 90000) {
        pid = find_steam_pid(exe);
        if (pid == 0) {
            Sleep(50);
            continue;
        }
        if (!saw_ui && module_loaded(pid, L"steamui.dll")) {
            saw_ui = true;
            log_event(L"info", L"inject", (L"steamui.dll is loaded in pid " + std::to_wstring(pid) + L".").c_str());
        }
        if (!saw_helper && child_process_named(pid, L"steamwebhelper.exe")) {
            saw_helper = true;
            log_event(L"info", L"inject", (L"steamwebhelper.exe is running under pid " + std::to_wstring(pid) + L".").c_str());
        }
        if (saw_ui && saw_helper) {
            Sleep(100);
            break;
        }
        Sleep(50);
    }
    if (pid == 0 || !saw_ui || !saw_helper) {
        result.detail = L"Steam at '" + exe + L"' did not have steamui.dll and steamwebhelper.exe within 90 seconds, so nothing was injected.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    result.pid = pid;
    CdpPipe pipe;
    std::wstring error;
    if (!pipe.connect_steam(pid, error)) {
        result.detail = error;
        return result;
    }
    if (!inject_boot_script(pipe, boot_script_for_page(), result.session_id, error)) {
        result.detail = error;
        pipe.close();
        return result;
    }
    result.pipe = std::move(pipe);
    result.ok = true;
    result.detail = L"The boot script was injected into the running Steam pid " + std::to_wstring(pid) + L".";
    log_event(L"info", L"inject", result.detail.c_str());
    return result;
}

namespace {

struct WindowSearch {
    unsigned long pids[64] = {};
    int count = 0;
    int shown = 0;
    bool hide = false;
};

BOOL CALLBACK show_one_window(HWND hwnd, LPARAM param) {
    auto* search = reinterpret_cast<WindowSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    bool mine = false;
    for (int i = 0; i < search->count; ++i) {
        if (search->pids[i] == pid) {
            mine = true;
            break;
        }
    }
    if (!mine || (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) != 0) {
        return TRUE;
    }
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) {
        return TRUE;
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width < 200 || height < 200) {
        return TRUE;
    }
    if (search->hide) {
        if (IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_HIDE);
            search->shown += 1;
        }
        return TRUE;
    }
    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 256);
    // The blank login surface has no title. Only the real client window is shown.
    if (wcsstr(title, L"Steam") == nullptr) {
        return TRUE;
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    search->shown += 1;
    return TRUE;
}

void collect_process_tree(unsigned long root, WindowSearch& search) {
    search.pids[search.count++] = root;
    bool grew = true;
    while (grew && search.count < 64) {
        grew = false;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return;
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                bool parent_known = false;
                bool already = false;
                for (int i = 0; i < search.count; ++i) {
                    if (search.pids[i] == entry.th32ParentProcessID) {
                        parent_known = true;
                    }
                    if (search.pids[i] == entry.th32ProcessID) {
                        already = true;
                    }
                }
                if (parent_known && !already && search.count < 64) {
                    search.pids[search.count++] = entry.th32ProcessID;
                    grew = true;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
}

}  // namespace

int show_client_windows(unsigned long pid) {
    WindowSearch search;
    collect_process_tree(pid, search);
    EnumWindows(show_one_window, reinterpret_cast<LPARAM>(&search));
    return search.shown;
}

namespace {

std::atomic<bool> g_hold_stop{true};
std::atomic<bool> g_hold_show{false};
std::thread g_hold_thread;
std::mutex g_hold_mu;

void hide_client_windows(unsigned long pid) {
    WindowSearch search;
    search.hide = true;
    collect_process_tree(pid, search);
    EnumWindows(show_one_window, reinterpret_cast<LPARAM>(&search));
}

}  // namespace

void hold_client_windows(unsigned long pid) {
    std::lock_guard<std::mutex> lock(g_hold_mu);
    if (g_hold_thread.joinable()) {
        return;
    }
    g_hold_show = false;
    g_hold_stop = false;
    g_hold_thread = std::thread([pid]() {
        const DWORD start = GetTickCount();
        while (!g_hold_stop.load()) {
            if (GetTickCount() - start > 120000) {
                g_hold_show = true;
                log_event(L"warn", L"inject", L"Stopped holding the Steam window after 120 seconds.");
                break;
            }
            hide_client_windows(pid);
            Sleep(10);
        }
        if (g_hold_show.load()) {
            const int shown = show_client_windows(pid);
            log_event(L"info", L"inject", (L"Brought " + std::to_wstring(shown) + L" Steam window(s) to the desktop after the skin was ready.").c_str());
        }
    });
        log_event(L"info", L"inject", (L"Holding Steam windows for pid " + std::to_wstring(pid) + L" until the library grid has finished its first pass.").c_str());
}

void finish_hold(bool show) {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(g_hold_mu);
        if (!g_hold_thread.joinable()) {
            return;
        }
        g_hold_show = show;
        g_hold_stop = true;
        worker = std::move(g_hold_thread);
    }
    if (worker.joinable()) {
        worker.join();
    }
}

void present_client_windows() {
    finish_hold(true);
}

void drop_client_windows() {
    finish_hold(false);
}

}  // namespace doomnet
