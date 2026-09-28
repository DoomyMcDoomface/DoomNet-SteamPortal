// Spec: Lua host, Contracts to match. No Lua headers in this scaffold.
// Host module "millennium" until the paired rename to "host".
// The player source is not edited to say DoomNet.
//   steam_path()           from config.json
//   get_install_path()     %LOCALAPPDATA%\DoomNet
//                          Do not create a themes directory there.
//                          theme_host_dir would otherwise write into
//                          <steam>\millennium\themes. Make that write a no-op.
//   version()              "doomnet"
//   add_browser_js         return 1. Do not insert a millennium.host URL.
//                          Do not retry on -1. steam_attach inserts the
//                          steamloopback bootstrap.
//   call_frontend_method   one JSON string argument. Nested table is an
//                          error (nil, message). Yield while the page runs.
//                          Bridge window only (the one with __SMP_RECEIVERS__).
//   ready()                record on_load finished. Do not kill the process
//                          at 10 seconds.
// callServerMethod(plugin, method, argsArray). Plugin name SteamMusicPlayer.
// Args are positional. Dispatch is lua_getglobal. Globals stay global.

#include "anchors.h"

namespace doomnet {
void host_millennium_anchor() {}
}
