// Spec: Lua host, Threads. No Lua headers in this scaffold.
// Host module "utils":
//   get_backend_path   the player backend directory
//   read_file          binary-safe
//   write_file         binary-safe
//   hash               must stay the algorithm already stored in track ids
//                      and playlist file ids, or a copied library splits
//   uuid
//   base64_encode      state thread, after a yielded read. Chunk size
//                      stays 512 KiB. Do not raise it.
//   sleep(ms)          yields. pcall(utils.sleep, ms) must succeed, or
//                      procexec.lua and library.lua busy-spin on os.clock.
//   getenv
//   exec               CREATE_NO_WINDOW (0x08000000) and SW_HIDE.
//                      Not _popen. Not cmd.exe. Not powershell -NoExit.
// Art, loudness, and Discord stay child processes.

#include "anchors.h"

namespace doomnet {
void host_utils_anchor() {}
}
