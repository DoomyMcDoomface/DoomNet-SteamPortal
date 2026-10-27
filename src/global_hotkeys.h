#pragma once

// Media keys while Steam is not the foreground window. The process main
// thread owns the hook. It posts transport_command and never calls Lua.

#include "host_bus.h"

#include <string>

namespace doomnet {

void global_keys_bind(Bus& bus, const std::wstring& settings_path);
void global_keys_poll();
bool global_keys_armed();
// Pumps the keyboard hook for this long. A plain sleep drops the key when Steam is not focused.
void global_keys_wait(unsigned long milliseconds);
void global_keys_stop();

}  // namespace doomnet
