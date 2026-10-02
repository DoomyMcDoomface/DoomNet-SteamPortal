// Spec: "Lua host and the four shims". The `logger` module, and `print`.
//
// Read: Millennium src/lua_host/api/logger.cc on main. The player calls
// `logger:info(msg)` with a colon, so the message is argument 2. A plain
// `logger.info(msg)` is accepted too. Each line goes to logs\doomnet.log with the
// calling file and line in front, the same information Millennium's log panel
// showed.

#include "anchors.h"
#include "host_internal.h"
#include "log.h"
#include "text.h"

#include <string>

namespace doomnet {

void host_logger_anchor() {}

std::string lua_caller(lua_State* L, int level) {
    lua_Debug frame{};
    if (!lua_getstack(L, level, &frame) || !lua_getinfo(L, "Sl", &frame)) {
        return std::string();
    }
    std::string name = frame.source != nullptr ? frame.source : "";
    if (!name.empty() && (name[0] == '@' || name[0] == '=')) {
        name.erase(0, 1);
    } else {
        name = frame.short_src;
    }
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        name.erase(0, slash + 1);
    }
    if (name.empty() || frame.currentline <= 0) {
        return name;
    }
    return name + ":" + std::to_string(frame.currentline);
}

namespace {

int write_log(lua_State* L, const wchar_t* level) {
    const char* message = nullptr;
    size_t length = 0;
    if (lua_type(L, 2) == LUA_TSTRING || lua_type(L, 2) == LUA_TNUMBER) {
        message = lua_tolstring(L, 2, &length);
    } else if (lua_type(L, 1) == LUA_TSTRING || lua_type(L, 1) == LUA_TNUMBER) {
        message = lua_tolstring(L, 1, &length);
    } else {
        return luaL_error(L, "logger needs a message: use logger:info(\"text\")");
    }
    std::string line = lua_caller(L, 1);
    if (!line.empty()) {
        line += ": ";
    }
    line.append(message, length);
    const std::wstring wide = utf8_to_wide_lossy(line);
    log_event(level, L"lua", wide.c_str());
    return 0;
}

int Lua_info(lua_State* L) { return write_log(L, L"info"); }
int Lua_warn(lua_State* L) { return write_log(L, L"warn"); }
int Lua_error(lua_State* L) { return write_log(L, L"error"); }
int Lua_debug(lua_State* L) { return write_log(L, L"debug"); }

const luaL_Reg kLoggerFunctions[] = {
    DOOMNET_LUA_FN(info),
    DOOMNET_LUA_FN(warn),
    DOOMNET_LUA_FN(error),
    DOOMNET_LUA_FN(debug),
    {nullptr, nullptr},
};

int luaopen_host_logger(lua_State* L) {
    luaL_newlib(L, kLoggerFunctions);
    return 1;
}

// print() has no console to write to. Send it to the log instead of losing it.
// Joins the `count` strings on top of the stack into one log line. Raises nothing.
void log_printed(lua_State* L, int count) {
    std::string line = lua_caller(L, 1);
    line += line.empty() ? "print: " : ": print: ";
    for (int i = 0; i < count; ++i) {
        size_t length = 0;
        const char* piece = lua_tolstring(L, lua_gettop(L) - count + 1 + i, &length);
        if (i > 0) {
            line += '\t';
        }
        line.append(piece != nullptr ? piece : "", piece != nullptr ? length : 0);
    }
    const std::wstring wide = utf8_to_wide_lossy(line);
    log_event(L"info", L"lua", wide.c_str());
}

int Lua_print(lua_State* L) {
    // luaL_tolstring can run a script's __tostring, which can raise. So convert
    // everything first, with no C++ object alive, then build the line.
    const int count = lua_gettop(L);
    luaL_checkstack(L, count + 2, "too many arguments to print");
    for (int i = 1; i <= count; ++i) {
        luaL_tolstring(L, i, nullptr);
    }
    log_printed(L, count);
    return 0;
}

}  // namespace

void open_host_logger(lua_State* L) {
    luaL_requiref(L, "logger", luaopen_host_logger, 0);
    lua_pop(L, 1);
    lua_pushcfunction(L, guarded<&Lua_print>);
    lua_setglobal(L, "print");
}

}  // namespace doomnet
