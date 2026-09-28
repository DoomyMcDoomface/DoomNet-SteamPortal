#include "instance.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace doomnet {
namespace {
HANDLE g_instance = nullptr;
}

bool acquire_instance() {
    if (g_instance) {
        return true;
    }
    g_instance = CreateMutexW(nullptr, TRUE, L"Local\\DoomNet");
    if (!g_instance) {
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_instance);
        g_instance = nullptr;
        return false;
    }
    return true;
}

}  // namespace doomnet
