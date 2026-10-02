#pragma once

// Shared declarations for the Lua host. Only the state thread includes this
// (state_thread.cpp and the host_*.cpp shims). Nothing here is visible to the
// browser thread.

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

// Lua is built as C++ (see CMakeLists.txt), so lua_error and lua_yield throw a
// pointer of this type (ldo.c). It is only named here so `guarded` can let it pass.
struct lua_longjmp;

#include "host_bus.h"
#include "host_config.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace doomnet {

// What a C function that is about to yield wants done next.
struct YieldRequest {
    enum class Kind { None, Sleep, Frontend, ReadRange, Exec, Preempt };
    Kind kind = Kind::None;
    uint32_t sleep_ms = 0;
    std::string method;
    std::string params_json;
    std::string caller;
    void* file = nullptr;
    std::wstring path;
    uint64_t offset = 0;
    uint32_t length = 0;
    std::wstring command_line;
};

struct Coroutine {
    enum class Kind { Load, OnLoad, Call, Lifecycle };
    enum class Wait { None, Sleep, Frontend, ReadRange, Exec, Preempt };
    uint64_t id = 0;
    Kind kind = Kind::Call;
    Wait waiting = Wait::None;
    lua_State* thread = nullptr;
    int registry_ref = LUA_NOREF;
    uint64_t call_id = 0;  // page Promise id, 0 for internal routines
    std::string method;
    bool parking_reads = false;
    uint64_t started_ms = 0;
    uint64_t longest_stretch_ms = 0;
};

// Owned by the state thread and reachable from every lua_State through its
// extra space.
struct HostState {
    HostConfig config;
    Bus* bus = nullptr;
    Coroutine* running = nullptr;
    YieldRequest pending;
    std::atomic<bool> ready_signalled{false};  // written here, read by tests and the owner
    std::vector<std::string> browser_js;       // module paths passed to add_browser_js
    std::string backend_dir_utf8;
    std::string steam_path_utf8;
    std::string install_path_utf8;
    int json_encode_ref = LUA_NOREF;
    int json_decode_ref = LUA_NOREF;
};

HostState& host_of(lua_State* L);
void set_host_of(lua_State* L, HostState* state);

// Lua numbers coming out of the shims are doubles, like LuaJIT.
inline void push_number(lua_State* L, double value) {
    lua_pushnumber(L, static_cast<lua_Number>(value));
}
inline void push_count(lua_State* L, unsigned long long value) {
    lua_pushnumber(L, static_cast<lua_Number>(value));
}

// True when the running coroutine can be parked (it is a Call routine and
// Lua is not inside a C boundary that cannot yield).
bool can_park(lua_State* L);

// "file.lua:123" of the Lua function `level` frames up, or "".
std::string lua_caller(lua_State* L, int level);

// Converts the Lua string at `index` into a wide path. Raises a Lua error with
// a sentence that names the argument when it is not a usable path.
std::wstring check_wide_path(lua_State* L, int index, const char* what);

// Non-raising: false if the value at `index` is not a string or not a path.
bool try_wide_path(lua_State* L, int index, std::wstring& out);

// Reads a whole file with no size cap. `error` is a sentence with the path and
// the Windows error text.
bool read_whole_file(const std::wstring& path, std::string& out, std::wstring& error);

// Shim registration. Each opens its module table and stores it in
// package.preload (or replaces a global) on the given state.
void open_host_fs(lua_State* L);
void open_host_io(lua_State* L);
void open_host_os(lua_State* L);
void open_host_json(lua_State* L);
void open_host_logger(lua_State* L);
void open_host_utils(lua_State* L);
void open_host_millennium(lua_State* L);
void install_host_searcher(lua_State* L);

// library.lua skips a file it has already measured. A track saved with no
// album or artist would then stay hidden on every later scan. Those files
// are read again until the tags are there.
std::string patched_player_module(const std::string& module, std::string source);

// Called by the shims to run Lua-visible C++ under a guard. C++ exceptions must
// never cross into Lua's C frames.
template <int (*Fn)(lua_State*)>
int guarded(lua_State* L) {
    try {
        return Fn(L);
    } catch (lua_longjmp*) {
        // Lua itself is unwinding (lua_error or lua_yield inside Fn). Not ours to handle.
        throw;
    } catch (const std::exception& error) {
        lua_pushstring(L, error.what());
    } catch (...) {
        lua_pushstring(L, "unknown C++ exception inside the DoomNet host");
    }
    return lua_error(L);
}

#define DOOMNET_LUA_FN(name) {#name, ::doomnet::guarded<&Lua_##name>}

// Encodes the value at `index` with the json module. False with `error` set on failure.
bool encode_json_value(lua_State* L, int index, std::string& out, std::string& error);
// Decodes JSON text and pushes the result. False with `error` set on failure (nothing pushed).
bool decode_json_value(lua_State* L, const std::string& text, std::string& error);

}  // namespace doomnet
