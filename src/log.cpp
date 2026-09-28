// Spec: Threads.
// Append-only log under the data root, logs\doomnet.log. One named mutex
// covers the write so two processes cannot tear a line. The Lua logger,
// later, is the state thread only. No console.

#include "log.h"

#include "anchors.h"
#include "paths.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <string>

namespace doomnet {
namespace {

constexpr DWORD kLogWaitMs = 1000;
constexpr size_t kMaxMessage = 4096;

std::wstring sanitize(const wchar_t* text) {
    std::wstring out;
    if (!text) {
        return out;
    }
    bool truncated = false;
    for (size_t i = 0; text[i] != L'\0'; ++i) {
        if (out.size() >= kMaxMessage) {
            truncated = true;
            break;
        }
        const wchar_t ch = text[i];
        if (ch == L'\r' || ch == L'\n') {
            out.push_back(L' ');
        } else {
            out.push_back(ch);
        }
    }
    if (truncated) {
        out += L" [truncated]";
    }
    return out;
}

bool utf8_from_wide(const std::wstring& in, std::string& out) {
    if (in.empty()) {
        out.clear();
        return true;
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, in.c_str(), static_cast<int>(in.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return false;
    }
    out.assign(static_cast<size_t>(bytes), '\0');
    const int wrote = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, in.c_str(), static_cast<int>(in.size()), out.data(), bytes, nullptr, nullptr);
    return wrote == bytes;
}

std::wstring stamp() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    wchar_t buf[40] = {};
    swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return buf;
}

}  // namespace

bool log_line(const wchar_t* level, const wchar_t* message) {
    if (!level || level[0] == L'\0') {
        return false;
    }
    std::wstring root;
    std::wstring root_error;
    if (!data_root(root, root_error)) {
        return false;
    }
    const std::wstring line = stamp() + L" " + sanitize(level) + L" pid=" + std::to_wstring(GetCurrentProcessId()) + L" " + sanitize(message) + L"\r\n";
    std::string bytes;
    if (!utf8_from_wide(line, bytes) || bytes.empty()) {
        return false;
    }

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\DoomNetLog");
    if (!mutex) {
        return false;
    }
    const DWORD waited = WaitForSingleObject(mutex, kLogWaitMs);
    if (waited != WAIT_OBJECT_0) {
        CloseHandle(mutex);
        return false;
    }

    bool wrote = false;
    if (ensure_directory(root)) {
        const std::wstring logs = root + L"\\logs";
        if (ensure_directory(logs)) {
            const std::wstring file = logs + L"\\doomnet.log";
            HANDLE handle = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                wrote = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
                CloseHandle(handle);
            }
        }
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return wrote;
}

bool log_event(const wchar_t* level, const wchar_t* component, const wchar_t* detail) {
    if (!component || component[0] == L'\0') {
        return log_line(level, detail);
    }
    const std::wstring message = std::wstring(component) + L": " + (detail ? detail : L"");
    return log_line(level, message.c_str());
}

std::wstring windows_error_text(unsigned long code) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring words;
    if (length != 0 && buffer) {
        words.assign(buffer, length);
        while (!words.empty() && (words.back() == L'\r' || words.back() == L'\n' || words.back() == L' ' || words.back() == L'.')) {
            words.pop_back();
        }
    }
    if (buffer) {
        LocalFree(buffer);
    }
    wchar_t number[32] = {};
    swprintf_s(number, L"Windows error %lu", code);
    if (words.empty()) {
        return number;
    }
    return std::wstring(number) + L" (" + words + L")";
}

void log_anchor() {}

}  // namespace doomnet
