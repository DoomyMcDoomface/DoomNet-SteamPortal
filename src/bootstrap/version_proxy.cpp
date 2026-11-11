// Forwards version.dll calls to System32. This file is not named version.dll
// in the source tree. The built DLL is, and it is loaded by steam.exe at
// process start because VERSION.dll is a normal import. The hook in
// wsock32.cpp is linked into that DLL so it is installed before steamwebhelper.

#include <windows.h>

#include <cstdio>

namespace {

HMODULE system_version() {
    static HMODULE module = nullptr;
    if (module != nullptr) {
        return module;
    }
    wchar_t dir[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return nullptr;
    }
    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\version.dll", dir);
    module = LoadLibraryW(path);
    return module;
}

template <typename Fn>
Fn proc(const char* name) {
    const HMODULE module = system_version();
    if (module == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<Fn>(GetProcAddress(module, name));
}

}  // namespace

extern "C" {

DWORD WINAPI DoomNet_GetFileVersionInfoSizeA(LPCSTR filename, LPDWORD handle) {
    using Fn = DWORD(WINAPI*)(LPCSTR, LPDWORD);
    const Fn fn = proc<Fn>("GetFileVersionInfoSizeA");
    return fn == nullptr ? 0 : fn(filename, handle);
}

DWORD WINAPI DoomNet_GetFileVersionInfoSizeW(LPCWSTR filename, LPDWORD handle) {
    using Fn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    const Fn fn = proc<Fn>("GetFileVersionInfoSizeW");
    return fn == nullptr ? 0 : fn(filename, handle);
}

BOOL WINAPI DoomNet_GetFileVersionInfoA(LPCSTR filename, DWORD reserved, DWORD length, LPVOID data) {
    using Fn = BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID);
    const Fn fn = proc<Fn>("GetFileVersionInfoA");
    return fn == nullptr ? FALSE : fn(filename, reserved, length, data);
}

BOOL WINAPI DoomNet_GetFileVersionInfoW(LPCWSTR filename, DWORD reserved, DWORD length, LPVOID data) {
    using Fn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    const Fn fn = proc<Fn>("GetFileVersionInfoW");
    return fn == nullptr ? FALSE : fn(filename, reserved, length, data);
}

DWORD WINAPI DoomNet_GetFileVersionInfoSizeExA(DWORD flags, LPCSTR filename, LPDWORD handle) {
    using Fn = DWORD(WINAPI*)(DWORD, LPCSTR, LPDWORD);
    const Fn fn = proc<Fn>("GetFileVersionInfoSizeExA");
    return fn == nullptr ? 0 : fn(flags, filename, handle);
}

DWORD WINAPI DoomNet_GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR filename, LPDWORD handle) {
    using Fn = DWORD(WINAPI*)(DWORD, LPCWSTR, LPDWORD);
    const Fn fn = proc<Fn>("GetFileVersionInfoSizeExW");
    return fn == nullptr ? 0 : fn(flags, filename, handle);
}

BOOL WINAPI DoomNet_GetFileVersionInfoExA(DWORD flags, LPCSTR filename, DWORD reserved, DWORD length, LPVOID data) {
    using Fn = BOOL(WINAPI*)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
    const Fn fn = proc<Fn>("GetFileVersionInfoExA");
    return fn == nullptr ? FALSE : fn(flags, filename, reserved, length, data);
}

BOOL WINAPI DoomNet_GetFileVersionInfoExW(DWORD flags, LPCWSTR filename, DWORD reserved, DWORD length, LPVOID data) {
    using Fn = BOOL(WINAPI*)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
    const Fn fn = proc<Fn>("GetFileVersionInfoExW");
    return fn == nullptr ? FALSE : fn(flags, filename, reserved, length, data);
}

BOOL WINAPI DoomNet_GetFileVersionInfoByHandle(DWORD flags, HANDLE file, LPVOID data, DWORD length) {
    using Fn = BOOL(WINAPI*)(DWORD, HANDLE, LPVOID, DWORD);
    const Fn fn = proc<Fn>("GetFileVersionInfoByHandle");
    return fn == nullptr ? FALSE : fn(flags, file, data, length);
}

BOOL WINAPI DoomNet_VerQueryValueA(LPCVOID block, LPCSTR sub, LPVOID* buffer, PUINT length) {
    using Fn = BOOL(WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT);
    const Fn fn = proc<Fn>("VerQueryValueA");
    return fn == nullptr ? FALSE : fn(block, sub, buffer, length);
}

BOOL WINAPI DoomNet_VerQueryValueW(LPCVOID block, LPCWSTR sub, LPVOID* buffer, PUINT length) {
    using Fn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    const Fn fn = proc<Fn>("VerQueryValueW");
    return fn == nullptr ? FALSE : fn(block, sub, buffer, length);
}

DWORD WINAPI DoomNet_VerFindFileA(DWORD flags, LPCSTR file, LPCSTR win_dir, LPCSTR app_dir, LPSTR cur_dir, PUINT cur_len, LPSTR dest_dir, PUINT dest_len) {
    using Fn = DWORD(WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
    const Fn fn = proc<Fn>("VerFindFileA");
    return fn == nullptr ? 0 : fn(flags, file, win_dir, app_dir, cur_dir, cur_len, dest_dir, dest_len);
}

DWORD WINAPI DoomNet_VerFindFileW(DWORD flags, LPCWSTR file, LPCWSTR win_dir, LPCWSTR app_dir, LPWSTR cur_dir, PUINT cur_len, LPWSTR dest_dir, PUINT dest_len) {
    using Fn = DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
    const Fn fn = proc<Fn>("VerFindFileW");
    return fn == nullptr ? 0 : fn(flags, file, win_dir, app_dir, cur_dir, cur_len, dest_dir, dest_len);
}

DWORD WINAPI DoomNet_VerInstallFileA(DWORD flags, LPCSTR src_name, LPCSTR dest_name, LPCSTR src_dir, LPCSTR dest_dir, LPCSTR cur_dir, LPSTR tmp_file, PUINT tmp_len) {
    using Fn = DWORD(WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
    const Fn fn = proc<Fn>("VerInstallFileA");
    return fn == nullptr ? 0 : fn(flags, src_name, dest_name, src_dir, dest_dir, cur_dir, tmp_file, tmp_len);
}

DWORD WINAPI DoomNet_VerInstallFileW(DWORD flags, LPCWSTR src_name, LPCWSTR dest_name, LPCWSTR src_dir, LPCWSTR dest_dir, LPCWSTR cur_dir, LPWSTR tmp_file, PUINT tmp_len) {
    using Fn = DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
    const Fn fn = proc<Fn>("VerInstallFileW");
    return fn == nullptr ? 0 : fn(flags, src_name, dest_name, src_dir, dest_dir, cur_dir, tmp_file, tmp_len);
}

DWORD WINAPI DoomNet_VerLanguageNameA(DWORD lang, LPSTR buffer, DWORD chars) {
    using Fn = DWORD(WINAPI*)(DWORD, LPSTR, DWORD);
    const Fn fn = proc<Fn>("VerLanguageNameA");
    return fn == nullptr ? 0 : fn(lang, buffer, chars);
}

DWORD WINAPI DoomNet_VerLanguageNameW(DWORD lang, LPWSTR buffer, DWORD chars) {
    using Fn = DWORD(WINAPI*)(DWORD, LPWSTR, DWORD);
    const Fn fn = proc<Fn>("VerLanguageNameW");
    return fn == nullptr ? 0 : fn(lang, buffer, chars);
}

}  // extern "C"
