// Spec: What makes the inject succeed.
// The only file a Steam update should force a change in.
// This scaffold leaves it empty on purpose. Do not put a hook, a port
// constant, an export forward, or a debugger client here until the
// windowless exe and the Lua host are already proven.
// When filled: code inside steam.exe records the port and returns.
// File copies, Lua, and page scripts stay in DoomNet.exe.
// Refuse to run while Millennium's wsock32.dll is still beside steam.exe.

#include "anchors.h"

namespace doomnet {
void steam_attach_anchor() {}
}
