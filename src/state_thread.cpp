// Spec: Threads.
// The only thread that may call lua_resume, lua_pcall, or lua_getglobal.
// One lua_State. Coroutines share it. Only one coroutine is resumed at a time.
// Inbound queue messages: Call, Resume, Stop.
// A call runs until it returns or yields. A yield is not a condvar wait on
// this thread. utils.sleep, call_frontend_method, and the audio-chunk read
// park the coroutine so the next Call can run.
// Do not close the lua_State because one call failed. Do not abort a call
// at 30 seconds. Do not start a thread pool or a thread per page.
// scan_library_batch stays one uninterrupted stretch (no yield inside
// read_tags_safe). Embed Lua 5.4, not LuaJIT.

#include "anchors.h"

namespace doomnet {
void state_thread_anchor() {}
}
