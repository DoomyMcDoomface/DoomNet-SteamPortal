// A stand-in for steamwebhelper.exe. Records the command line it was given and,
// when the bootstrap attached debugging pipes, speaks a few bytes through them.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::wstring env_or_empty(const wchar_t* name) {
    wchar_t buffer[32768] = {};
    const DWORD n = GetEnvironmentVariableW(name, buffer, 32768);
    if (n == 0 || n >= 32768) {
        return {};
    }
    return std::wstring(buffer, n);
}

bool parse_pipes(HANDLE& read_handle, HANDLE& write_handle) {
    const wchar_t* line = GetCommandLineW();
    const wchar_t* marker = L"--remote-debugging-io-pipes=";
    const wchar_t* at = wcsstr(line, marker);
    if (at == nullptr) {
        return false;
    }
    at += wcslen(marker);
    unsigned long long read_value = 0;
    unsigned long long write_value = 0;
    if (swscanf_s(at, L"%llu,%llu", &read_value, &write_value) != 2) {
        return false;
    }
    read_handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(read_value));
    write_handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(write_value));
    return true;
}

}  // namespace

int wmain() {
    const std::wstring command = GetCommandLineW();
    const std::wstring cmdline_path = env_or_empty(L"DOOMNET_CMDLINE_OUT");
    if (!cmdline_path.empty()) {
        // Append, so a second launch in the same test is visible too.
        HANDLE handle = CreateFileW(cmdline_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, command.c_str(), static_cast<int>(command.size()), nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<size_t>(bytes > 0 ? bytes : 0), '\0');
            if (bytes > 0) {
                WideCharToMultiByte(CP_UTF8, 0, command.c_str(), static_cast<int>(command.size()), utf8.data(), bytes, nullptr, nullptr);
            }
            utf8.append("\r\n");
            DWORD written = 0;
            WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            CloseHandle(handle);
        }
    }

    const bool flood = wcsstr(command.c_str(), L"--doomnet-flood") != nullptr;
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!parse_pipes(read_handle, write_handle)) {
        return flood ? 3 : (wcsstr(command.c_str(), L"--type=") != nullptr ? 0 : 3);
    }
    if (flood) {
        char block[65536];
        memset(block, 'x', sizeof(block));
        size_t left = 2u * 1024u * 1024u;
        while (left > 0) {
            const DWORD chunk = static_cast<DWORD>(left > sizeof(block) ? sizeof(block) : left);
            DWORD written = 0;
            if (!WriteFile(write_handle, block, chunk, &written, nullptr) || written != chunk) {
                return 4;
            }
            left -= chunk;
        }
        return 0;
    }

    if (wcsstr(command.c_str(), L"--doomnet-cdp") != nullptr) {
        std::string pending;
        bool announced = false;
        while (pending.size() < 8u * 1024u * 1024u) {
            char buffer[4096];
            DWORD read = 0;
            if (!ReadFile(read_handle, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
                return 7;
            }
            pending.append(buffer, read);
            for (;;) {
                const size_t end = pending.find('\0');
                if (end == std::string::npos) {
                    break;
                }
                const std::string message = pending.substr(0, end);
                pending.erase(0, end + 1);
                int id = 1;
                const char* id_at = strstr(message.c_str(), "\"id\":");
                if (id_at != nullptr) {
                    id = atoi(id_at + 5);
                }
                char reply[256] = {};
                if (message.find("typeof window.Millennium") != std::string::npos) {
                    sprintf_s(reply, "{\"id\":%d,\"result\":{\"result\":{\"type\":\"boolean\",\"value\":true}}}", id);
                } else if (message.find("Target.attachToTarget") != std::string::npos) {
                    sprintf_s(reply, "{\"id\":%d,\"result\":{\"sessionId\":\"page-1\"}}", id);
                } else {
                    sprintf_s(reply, "{\"id\":%d,\"result\":{}}", id);
                }
                std::string framed = reply;
                framed.push_back('\0');
                DWORD written = 0;
                if (!announced && message.find("setDiscoverTargets") != std::string::npos) {
                    const char* event = "{\"method\":\"Target.targetCreated\",\"params\":{\"targetInfo\":{\"targetId\":\"T1\",\"type\":\"page\",\"title\":\"Steam\",\"url\":\"https://steamloopback.host/\"}}}";
                    std::string event_frame = event;
                    event_frame.push_back('\0');
                    if (!WriteFile(write_handle, event_frame.data(), static_cast<DWORD>(event_frame.size()), &written, nullptr)) {
                        return 5;
                    }
                    announced = true;
                }
                if (!WriteFile(write_handle, framed.data(), static_cast<DWORD>(framed.size()), &written, nullptr)) {
                    return 5;
                }
                if (message.find("Runtime.evaluate") != std::string::npos && message.find("BOOTSTRAP_URL") != std::string::npos) {
                    const std::wstring script_path = env_or_empty(L"DOOMNET_SCRIPT_OUT");
                    if (!script_path.empty()) {
                        HANDLE out = CreateFileW(script_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (out != INVALID_HANDLE_VALUE) {
                            DWORD ignored = 0;
                            WriteFile(out, message.data(), static_cast<DWORD>(message.size()), &ignored, nullptr);
                            CloseHandle(out);
                        }
                    }
                }
            }
        }
        return 8;
    }

    const char hello[] = "hello-from-helper";
    DWORD written = 0;
    const BOOL wrote = WriteFile(write_handle, hello, sizeof(hello) - 1, &written, nullptr);
    const std::wstring status_path = env_or_empty(L"DOOMNET_CMDLINE_OUT");
    if (!status_path.empty()) {
        const std::wstring status = status_path.substr(0, status_path.find_last_of(L"\\/")) + L"\\status.txt";
        char line[128] = {};
        const DWORD file_type = GetFileType(write_handle);
        sprintf_s(line, "write ok=%d written=%lu err=%lu type=%lu read=%llu write=%llu\r\n", wrote ? 1 : 0, written, GetLastError(), file_type, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(read_handle)), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(write_handle)));
        HANDLE status_file = CreateFileW(status.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (status_file != INVALID_HANDLE_VALUE) {
            DWORD ignored = 0;
            WriteFile(status_file, line, static_cast<DWORD>(strlen(line)), &ignored, nullptr);
            CloseHandle(status_file);
        }
    }
    if (!wrote) {
        return 5;
    }
    char incoming[64] = {};
    DWORD read = 0;
    if (!ReadFile(read_handle, incoming, sizeof(incoming) - 1, &read, nullptr)) {
        return 6;
    }
    const std::wstring reply_path = env_or_empty(L"DOOMNET_REPLY_OUT");
    if (!reply_path.empty()) {
        HANDLE handle = CreateFileW(reply_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD ignored = 0;
            WriteFile(handle, incoming, read, &ignored, nullptr);
            const char nl[] = "\r\n";
            WriteFile(handle, nl, 2, &ignored, nullptr);
            CloseHandle(handle);
        }
    }
    return 0;
}
