// Writes DOOMNET_CHILD_MARKER and exits. Windows subsystem, so it has no console.
// launch_hidden starts this, never steam.exe, while the inject path is absent.

#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t marker[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"DOOMNET_CHILD_MARKER", marker, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return 2;
    }
    HANDLE handle = CreateFileW(marker, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return 3;
    }
    const char text[] = "hidden-ok";
    DWORD written = 0;
    const BOOL ok = WriteFile(handle, text, sizeof(text) - 1, &written, nullptr);
    CloseHandle(handle);
    return ok && written == sizeof(text) - 1 ? 0 : 4;
}
