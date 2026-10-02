# Local changes to Lua 5.4.7

Source: https://www.lua.org/ftp/lua-5.4.7.tar.gz, `src/` only. `lua.c`, `luac.c`
and the Makefile are not included. Everything else is unmodified except as
listed here. Keep this list current, and re-apply it when Lua is upgraded.

## 1. `lobject.c` `tostringbuff`: no ".0" suffix on whole floats

Guarded by the compile definition `DOOMNET_LUAJIT_NUMBER_FORMAT`, which
`CMakeLists.txt` sets for the `doomnet_lua` target only.

Why: the player (SteamTunes) runs on Millennium's LuaJIT today, where every
number is a double and `tostring(3.0)` is `"3"`. Lua 5.4 has an integer
subtype and prints `"3.0"`. That matters in real data:

- `library.lua` `make_track_id` returns `tostring(utils.hash(path))`. In the
  live Millennium library the stored ids look like `"8.1536513882893e+18"`.
  Lua 5.4 would print the same double the same way (it contains an `e`), but
  a whole float such as `3.0` would print `3.0` and split any saved id,
  playlist key, or cache key that goes through `tostring` or `..`.
- `utils.hash` therefore pushes a double (see `src/host_utils.cpp`), the same
  value LuaJIT's `lua_pushinteger` produced.

What is not changed: `math.type`, integer division, bitwise operators, and
integer printing. Integers still print as integers. Values of 1e14 or more that
are stored as Lua integers print in full where LuaJIT would print an exponent;
the player does not depend on that.

## 2. `luaconf.h` `LUA_API`: C linkage when built as C++

Lua is compiled as C++ (`CMakeLists.txt` marks the sources `LANGUAGE CXX`) so
that `lua_error` and `lua_yield` throw C++ exceptions instead of calling
`longjmp`. `LUA_API` (and so `LUALIB_API` and `LUAMOD_API`) becomes
`extern "C"` under `__cplusplus`, so the host still includes the headers inside
`extern "C"` and the library and the host agree on names.

Why not plain C: with `longjmp`, a Lua error or a yield unwinds through the
host's optimised C++ frames. On x64 MSVC that unwind failed the stack-cookie
check (`_report_gsfailure` inside `RtlUnwind`) or freed a `std::string` twice,
and the process died with 0xC0000005 or 0xC0000374, in Release only. The tests
`t_raise_paths` and the real-player run in `src/host_test.cpp` reproduce it.

What this requires elsewhere:

- every target builds with `/EHs`, not `/EHsc`, so a call into an `extern "C"`
  Lua function is allowed to throw;
- `guarded<>` in `src/host_internal.h` rethrows `lua_longjmp*` (Lua's own
  exception) and only turns real C++ exceptions into Lua errors.
