// Spec: Millennium toggle on this PC.
// Steam must be fully quit first (no steam.exe, no steamwebhelper.exe).
// Stash only these names from the Steam root, and only if they are present:
//   wsock32.dll
//   version.dll
//   millennium-legacy.version.dll
//   user32.dll
// Do not move any other DLL. Do not touch System32.
// Do not delete millennium\plugins, millennium\themes, or player data.
// DoomNet refuses to inject while those hook files are back in place.
// No console.

#include "anchors.h"

namespace doomnet {
void toggle_anchor() {}
}
