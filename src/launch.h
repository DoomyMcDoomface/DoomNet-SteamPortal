#pragma once

#include <windows.h>

#include <string>

namespace doomnet {

// Starts exe with CREATE_NO_WINDOW and waits. steam.exe is refused so this
// scaffolding cannot launch the real client by mistake.
bool launch_hidden(const std::wstring& exe, DWORD timeout_ms, DWORD& exit_code, std::wstring& error);

}
