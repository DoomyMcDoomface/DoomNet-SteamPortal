// Spec: Lua host. No Lua headers in this scaffold.
// Host module "fs", later. Names the player already calls:
//   join       nil-safe. Joining onto nil must not throw.
//   exists, extension, remove, create_directories, list, is_directory
// io.open, fs.exists, and fs.file_size must be wide (CreateFileW).
// Lua's ANSI io.open is why non-ASCII tracks spawn PowerShell.
// Do not add a DoomNet-only function to the player. Replace io inside
// this host so Millennium's copy of the player is unchanged.

#include "anchors.h"

namespace doomnet {
void host_fs_anchor() {}
}
