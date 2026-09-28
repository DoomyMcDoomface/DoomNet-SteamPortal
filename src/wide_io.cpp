#include "wide_io.h"

#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace doomnet {
namespace {

constexpr size_t kMaxReadBytes = 1024 * 1024;

bool unsafe_path(const std::wstring& path) {
    return path.empty() || path.find(L"..") != std::wstring::npos;
}

}  // namespace

bool wide_file_exists(const std::wstring& path) {
    if (unsafe_path(path)) {
        return false;
    }
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool write_new_file(const std::wstring& path, const void* data, size_t size, std::wstring& error) {
    error.clear();
    if (unsafe_path(path)) {
        error = L"Refusing to write '" + path + L"' because the path is empty or contains '..'.";
        log_event(L"error", L"wideio", error.c_str());
        return false;
    }
    if (size > 0 && data == nullptr) {
        error = L"Refusing to write '" + path + L"' because the buffer is empty.";
        log_event(L"error", L"wideio", error.c_str());
        return false;
    }
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = L"Could not create '" + path + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"wideio", error.c_str());
        return false;
    }
    DWORD written = 0;
    const BOOL ok = size == 0 || WriteFile(handle, data, static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(handle);
    if (!ok || written != size) {
        error = L"The write to '" + path + L"' stopped after " + std::to_wstring(written) + L" of " + std::to_wstring(size) + L" bytes.";
        log_event(L"error", L"wideio", error.c_str());
        return false;
    }
    const std::wstring detail = L"Wrote " + std::to_wstring(size) + L" bytes to '" + path + L"'.";
    log_event(L"info", L"wideio", detail.c_str());
    return true;
}

bool read_entire_file(const std::wstring& path, std::string& out, std::wstring& error) {
    out.clear();
    error.clear();
    if (unsafe_path(path)) {
        error = L"Refusing to read '" + path + L"' because the path is empty or contains '..'.";
        log_event(L"error", L"wideio", error.c_str());
        return false;
    }
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = L"Could not open '" + path + L"' for reading. " + windows_error_text(GetLastError());
        log_event(L"error", L"wideio", error.c_str());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > kMaxReadBytes) {
        CloseHandle(handle);
        error = L"Refusing to read '" + path + L"' because it is larger than 1 MiB or the size could not be read.";
        log_event(L"error", L"wideio", error.c_str());
        return false;
    }
    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = size.QuadPart == 0 || ReadFile(handle, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(handle);
    if (!ok || read != out.size()) {
        out.clear();
        error = L"Could not read all of '" + path + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"wideio", error.c_str());
        return false;
    }
    const std::wstring detail = L"Read " + std::to_wstring(out.size()) + L" bytes from '" + path + L"'.";
    log_event(L"info", L"wideio", detail.c_str());
    return true;
}

}  // namespace doomnet
