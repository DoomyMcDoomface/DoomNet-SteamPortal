// Spec: "Lua host and the four shims". The `millennium` module.
//
// Read: Millennium src/lua_host/api/millennium.cc on main (commit
// ba4d360a766318c432678c4ed3a5b3dec16cc7dc). The module name stays
// `millennium` until the paired rename release of this repo and SteamTunes.
//
// What each function does here:
//   ready()                 records that the player finished loading.
//   version()               "doomnet".
//   steam_path()            the Steam install folder.
//   get_install_path()      DoomNet's data folder. Its "themes" subfolder exists
//                           so the player's theme-host write lands here, not in
//                           the Steam install.
//   add_browser_js/css      record the request and return a positive id. The
//                           player's real assets are loaded by the injected
//                           boot script, not by this call.
//   call_frontend_method    validates the parameters exactly like Millennium
//                           (boolean, string, number; a number is sent as text,
//                           because Millennium checked lua_isstring first), then
//                           parks the coroutine (Frontend). The typed return
//                           comes back through Resume.
//   is_plugin_enabled       false. cmp_version compares dotted numbers.

#include "anchors.h"
#include "host_internal.h"
#include "log.h"
#include "text.h"

#include <string>
#include <vector>

namespace doomnet {

void host_millennium_anchor() {}

namespace {

int Lua_ready(lua_State* L) {
    HostState& host = host_of(L);
    if (!host.ready_signalled) {
        host.ready_signalled = true;
        log_event(L"info", L"lua", L"The player called millennium.ready(): it finished loading.");
    }
    return 0;
}

int Lua_version(lua_State* L) {
    lua_pushliteral(L, "doomnet");
    return 1;
}

int Lua_steam_path(lua_State* L) {
    const HostState& host = host_of(L);
    lua_pushlstring(L, host.steam_path_utf8.data(), host.steam_path_utf8.size());
    return 1;
}

int Lua_get_install_path(lua_State* L) {
    const HostState& host = host_of(L);
    lua_pushlstring(L, host.install_path_utf8.data(), host.install_path_utf8.size());
    return 1;
}

int register_browser_module(lua_State* L, const wchar_t* kind) {
    HostState& host = host_of(L);
    size_t length = 0;
    const char* content = luaL_checklstring(L, 1, &length);
    const char* pattern = luaL_optstring(L, 2, ".*");
    host.browser_js.emplace_back(content, length);
    const std::wstring detail = std::wstring(L"The player asked to inject ") + kind + L" '" + utf8_to_wide_lossy(std::string(content, length)) + L"' into pages matching '" + utf8_to_wide_lossy(pattern) + L"'. It is recorded as module " +
                                std::to_wstring(host.browser_js.size()) + L"; the boot script loads the player itself.";
    log_event(L"info", L"lua", detail.c_str());
    push_count(L, host.browser_js.size());
    return 1;
}

int Lua_add_browser_js(lua_State* L) { return register_browser_module(L, L"script"); }
int Lua_add_browser_css(lua_State* L) { return register_browser_module(L, L"stylesheet"); }

int Lua_remove_browser_module(lua_State* L) {
    luaL_checkinteger(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

int Lua_is_plugin_enabled(lua_State* L) {
    luaL_checkstring(L, 1);
    lua_pushboolean(L, 0);
    return 1;
}

bool parse_version(const char* text, std::vector<long>& parts) {
    parts.clear();
    long current = 0;
    bool have_digit = false;
    for (const char* p = text;; ++p) {
        if (*p >= '0' && *p <= '9') {
            current = current * 10 + (*p - '0');
            have_digit = true;
        } else if (*p == '.' || *p == '\0') {
            if (!have_digit) {
                return false;
            }
            parts.push_back(current);
            current = 0;
            have_digit = false;
            if (*p == '\0') {
                return true;
            }
        } else {
            return false;
        }
    }
}

int Lua_cmp_version(lua_State* L) {
    const char* left = luaL_checkstring(L, 1);
    const char* right = luaL_checkstring(L, 2);
    std::vector<long> a;
    std::vector<long> b;
    if (!parse_version(left, a) || !parse_version(right, b)) {
        lua_pushinteger(L, -2);  // Millennium's "could not compare"
        return 1;
    }
    const size_t count = a.size() > b.size() ? a.size() : b.size();
    for (size_t i = 0; i < count; ++i) {
        const long x = i < a.size() ? a[i] : 0;
        const long y = i < b.size() ? b[i] : 0;
        if (x != y) {
            lua_pushinteger(L, x < y ? -1 : 1);
            return 1;
        }
    }
    lua_pushinteger(L, 0);
    return 1;
}

// Builds the page call from the (already validated) arguments. Returns -1 when
// the request is stored in host.pending and the caller must yield, or the number
// of values already pushed to return at once.
//
// C++ objects live only in this function. The one that calls lua_yield or
// lua_error below has none: yielding and raising both longjmp out of the frame,
// and under optimisation that unwind freed a std::string it should not have.
int prepare_frontend_call(lua_State* L, const char* name, size_t name_length) {
    std::string params_json = "[]";
    std::string caller;
    std::string method(name, name_length);
    {
        std::string error;
        if (lua_gettop(L) >= 2 && !lua_isnil(L, 2) && luaL_len(L, 2) > 0) {
            // Rebuild as a plain array so a number becomes text, as it did under Millennium.
            const lua_Integer count = luaL_len(L, 2);
            lua_newtable(L);
            for (lua_Integer i = 1; i <= count; ++i) {
                lua_rawgeti(L, 2, i);
                if (lua_type(L, -1) == LUA_TNUMBER) {
                    lua_tolstring(L, -1, nullptr);  // converts the stack slot to a string in place
                }
                lua_rawseti(L, -2, i);
            }
            const bool ok = encode_json_value(L, -1, params_json, error);
            lua_pop(L, 1);
            if (!ok) {
                lua_pushnil(L);
                const std::string message = "call_frontend_method could not encode its parameters: " + error;
                lua_pushlstring(L, message.data(), message.size());
                return 2;
            }
        }
        caller = lua_caller(L, 1);
    }

    if (!can_park(L)) {
        lua_pushnil(L);
        lua_pushliteral(L, "call_frontend_method was called where Lua cannot wait for the page (inside a callback of table.sort, string.gsub or require)");
        return 2;
    }
    HostState& host = host_of(L);
    host.pending = YieldRequest{};
    host.pending.kind = YieldRequest::Kind::Frontend;
    host.pending.method = std::move(method);
    host.pending.params_json = std::move(params_json);
    host.pending.caller = std::move(caller);
    return -1;
}

int Lua_call_frontend_method(lua_State* L) {
    size_t name_length = 0;
    const char* name = luaL_checklstring(L, 1, &name_length);

    // Validate first, build afterward, so no C++ object is alive when Lua raises.
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        if (!lua_istable(L, 2)) {
            return luaL_error(L, "params must be a table");
        }
        const lua_Integer count = luaL_len(L, 2);
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(L, 2, i);
            const int type = lua_type(L, -1);
            lua_pop(L, 1);
            if (type != LUA_TSTRING && type != LUA_TNUMBER && type != LUA_TBOOLEAN) {
                return luaL_error(L, "Millennium's IPC can only handle [boolean, string, number]");
            }
        }
    }

    const int pushed = prepare_frontend_call(L, name, name_length);
    if (pushed < 0) {
        return lua_yield(L, 0);
    }
    return pushed;
}

const luaL_Reg kMillenniumFunctions[] = {
    DOOMNET_LUA_FN(ready),
    DOOMNET_LUA_FN(version),
    DOOMNET_LUA_FN(steam_path),
    DOOMNET_LUA_FN(get_install_path),
    DOOMNET_LUA_FN(add_browser_js),
    DOOMNET_LUA_FN(add_browser_css),
    DOOMNET_LUA_FN(remove_browser_module),
    DOOMNET_LUA_FN(is_plugin_enabled),
    DOOMNET_LUA_FN(cmp_version),
    DOOMNET_LUA_FN(call_frontend_method),
    {nullptr, nullptr},
};

int luaopen_host_millennium(lua_State* L) {
    luaL_newlib(L, kMillenniumFunctions);
    return 1;
}

}  // namespace

void open_host_millennium(lua_State* L) {
    luaL_requiref(L, "millennium", luaopen_host_millennium, 0);
    lua_pop(L, 1);
}

}  // namespace doomnet
