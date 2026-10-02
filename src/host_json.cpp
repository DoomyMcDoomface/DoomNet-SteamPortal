// Spec: "Lua host and the four shims". The `json` module.
//
// This is Millennium's own cjson fork, built from third_party/millennium-json
// (see DOOMNET-PATCHES.md there), so json.encode / json.decode produce the
// same bytes the player's saved files were written with. This file only opens
// the module and lets the host encode and decode from C++.

#include "anchors.h"
#include "host_internal.h"

namespace doomnet {

void host_json_anchor() {}

HostState& host_of(lua_State* L) {
    return **static_cast<HostState**>(lua_getextraspace(L));
}

void set_host_of(lua_State* L, HostState* state) {
    *static_cast<HostState**>(lua_getextraspace(L)) = state;
}

}  // namespace doomnet

extern "C" int luaopen_cjson(lua_State* L);

namespace doomnet {

void open_host_json(lua_State* L) {
    HostState& host = host_of(L);
    luaL_requiref(L, "json", luaopen_cjson, 0);
    lua_getfield(L, -1, "encode");
    host.json_encode_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, -1, "decode");
    host.json_decode_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
}

bool encode_json_value(lua_State* L, int index, std::string& out, std::string& error) {
    HostState& host = host_of(L);
    index = lua_absindex(L, index);
    lua_rawgeti(L, LUA_REGISTRYINDEX, host.json_encode_ref);
    lua_pushvalue(L, index);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char* message = lua_tostring(L, -1);
        error = message != nullptr ? message : "json.encode failed with a value that is not text";
        lua_pop(L, 1);
        return false;
    }
    size_t length = 0;
    const char* text = lua_tolstring(L, -1, &length);
    out.assign(text != nullptr ? text : "", text != nullptr ? length : 0);
    lua_pop(L, 1);
    return true;
}

bool decode_json_value(lua_State* L, const std::string& text, std::string& error) {
    HostState& host = host_of(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, host.json_decode_ref);
    lua_pushlstring(L, text.data(), text.size());
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char* message = lua_tostring(L, -1);
        error = message != nullptr ? message : "json.decode failed with a value that is not text";
        lua_pop(L, 1);
        return false;
    }
    return true;
}

}  // namespace doomnet
