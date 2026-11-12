// Spec: "Two threads, two queues" and "The state thread loop".
//
// This is the only file that calls lua_resume. Everything a coroutine wants
// from the outside world (a timer, the page, a byte range, a process) leaves as
// a message to the browser thread, and the coroutine stays parked until a
// Resume message arrives. A Lua error fails that one call; the state stays open.

#include "anchors.h"
#include "catalog.h"
#include "folder_pick.h"
#include "host_internal.h"
#include "payload.h"
#include "log.h"
#include "paths.h"
#include "skin_page.h"
#include "state_thread.h"
#include "steam_discover.h"
#include "stream.h"
#include "text.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace doomnet {

void state_thread_anchor() {}

// Lua 5.1/LuaJIT globals the player's era of code may still name. Lua 5.4
// dropped them; defining them costs nothing and hides no behavior.
// library.lua calls entry_path while listing a folder, before its own local
// function of that name exists. Without this global the scan stops on the
// first folder and files saved without tags are never read again.
constexpr const char* kCompatSource =
    "unpack = unpack or table.unpack\n"
    "loadstring = loadstring or load\n"
    "table.getn = table.getn or function(t) return #t end\n"
    "math.pow = math.pow or function(a, b) return a ^ b end\n"
    "math.atan2 = math.atan2 or math.atan\n"
    "entry_path = entry_path or function(dir, entry)\n"
    "  local name = entry\n"
    "  if type(entry) == \"table\" then\n"
    "    name = entry.path or entry.name or entry[1]\n"
    "  end\n"
    "  if type(name) ~= \"string\" or name == \"\" then\n"
    "    return nil\n"
    "  end\n"
    "  local ok, procexec = pcall(require, \"procexec\")\n"
    "  if not ok or not procexec or not procexec.child_path then\n"
    "    return nil\n"
    "  end\n"
    "  return procexec.child_path(dir, name)\n"
    "end\n";

const char* player_compat_lua() {
    return kCompatSource;
}

namespace {

constexpr uint64_t kSlowStretchMs = 1000;

void note(const wchar_t* level, const std::string& utf8_sentence) {
    const std::wstring wide = utf8_to_wide_lossy(utf8_sentence);
    log_event(level, L"lua", wide.c_str());
}

void note_wide(const wchar_t* level, const std::wstring& sentence) {
    log_event(level, L"lua", sentence.c_str());
}

int lua_panic(lua_State* L) {
    const char* message = lua_tostring(L, -1);
    note(L"error", std::string("Lua panic outside any protected call: ") + (message != nullptr ? message : "(no message)") + ". The host cannot continue this state.");
    return 0;
}

bool artwork_method(const std::string& method) {
    return method == "request_idle_art" || method == "restore_published_art" || method == "start_queued_artwork_regenerate"
        || method == "regenerate_artwork" || method == "get_art_progress" || method == "queue_artwork_regenerate"
        || method == "rescan_library_start" || method == "scan_library_batch" || method == "get_track_art"
        || method == "get_first_track_art";
}

void art_preempt_hook(lua_State* L, lua_Debug*) {
    HostState& host = host_of(L);
    if (host.bus == nullptr || host.running == nullptr || lua_isyieldable(L) == 0 || host.pending.kind != YieldRequest::Kind::None) {
        return;
    }
    if (!host.bus->playback_queued()) {
        return;
    }
    host.pending.kind = YieldRequest::Kind::Preempt;
    lua_yield(L, 0);
}

bool method_name_ok(const std::string& name) {
    if (name.empty() || name.size() > 120) {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

int lua_stream_hook(lua_State* L) {
    lua_pushboolean(L, stream_hook_up() ? 1 : 0);
    return 1;
}

int lua_begin_folder_pick(lua_State* L) {
    lua_pushboolean(L, begin_folder_pick() ? 1 : 0);
    return 1;
}

int lua_folder_pick_result(lua_State* L) {
    std::wstring path;
    std::wstring error;
    const std::string status = folder_pick_take(path, error);
    lua_pushlstring(L, status.data(), status.size());
    if (status == "path") {
        const std::string utf8 = wide_to_utf8(path);
        lua_pushlstring(L, utf8.data(), utf8.size());
    } else if (status == "failed") {
        const std::string utf8 = wide_to_utf8(error);
        lua_pushlstring(L, utf8.data(), utf8.size());
    } else {
        lua_pushliteral(L, "");
    }
    return 2;
}

// Add Folder calls this. It parks between polls so the state thread is not
// stuck inside the Windows folder chooser.
constexpr const char* kFolderPickSource =
    "function pick_music_folder()\n"
    "  if not doomnet_begin_folder_pick() then\n"
    "    return \"\"\n"
    "  end\n"
    "  local utils = require(\"utils\")\n"
    "  while true do\n"
    "    local status, value = doomnet_folder_pick_result()\n"
    "    if status ~= \"wait\" then\n"
    "      if status == \"path\" then\n"
    "        return value or \"\"\n"
    "      end\n"
    "      return \"\"\n"
    "    end\n"
    "    utils.sleep(200)\n"
    "  end\n"
    "end\n";

int lua_begin_update_check(lua_State* L) {
    const HostState& host = host_of(L);
    lua_pushboolean(L, begin_store_update_check(host.config.install_path, host.config.steam_path) ? 1 : 0);
    return 1;
}

int lua_update_notice(lua_State* L) {
    const std::string utf8 = wide_to_utf8(current_update_notice());
    lua_pushlstring(L, utf8.data(), utf8.size());
    return 1;
}

int lua_update_check_result(lua_State* L) {
    std::wstring message;
    const std::string status = take_store_update_check(message);
    lua_pushlstring(L, status.data(), status.size());
    const std::string utf8 = wide_to_utf8(message);
    lua_pushlstring(L, utf8.data(), utf8.size());
    return 2;
}

// The Store button calls this. It parks between polls so the download stays off the state thread.
constexpr const char* kUpdateCheckSource =
    "function doomnet_check_updates()\n"
    "  if not doomnet_begin_update_check() then\n"
    "    return \"An update check is already running.\"\n"
    "  end\n"
    "  local utils = require(\"utils\")\n"
    "  while true do\n"
    "    local status, value = doomnet_update_check_result()\n"
    "    if status ~= \"wait\" then\n"
    "      return value or \"\"\n"
    "    end\n"
    "    utils.sleep(200)\n"
    "  end\n"
    "end\n";

int lua_doomnet_install_app(lua_State* L) {
    size_t length = 0;
    const char* id = luaL_checklstring(L, 1, &length);
    if (id == nullptr || std::string(id, length) != "steamskin") {
        lua_pushstring(L, "Only SteamSkin can be installed from the Store.");
        return 1;
    }
    const HostState& host = host_of(L);
    StoreInstallRequest request;
    request.root = host.config.install_path;
    request.steam_path = host.config.steam_path;
    queue_store_install(request);
    lua_pushstring(L, "SteamSkin is installing. Steam will restart when the copy finishes so the skin can load. settings.json is left as it is.");
    return 1;
}

int lua_doomnet_skin_color(lua_State* L) {
    const std::string token = saved_skin_color_token();
    lua_pushlstring(L, token.data(), token.size());
    return 1;
}

int lua_doomnet_set_skin_color(lua_State* L) {
    size_t theme_length = 0;
    const char* theme = luaL_checklstring(L, 1, &theme_length);
    size_t custom_length = 0;
    const char* custom = luaL_optlstring(L, 2, "", &custom_length);
    std::wstring error;
    if (!save_skin_color(std::string(theme, theme_length), std::string(custom, custom_length), error)) {
        const std::string text = wide_to_utf8(error);
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }
    const HostState& host = host_of(L);
    std::wstring steam = host.config.steam_path;
    if (steam.empty()) {
        discover_steam_path(steam);
    }
    std::wstring root;
    std::wstring root_error;
    data_root(root, root_error);
    const std::wstring skin_dir = launch_payload_dir(L"D:\\Documents\\Repos\\SteamSkin", root.empty() ? std::wstring() : root + L"\\skin", L"skin.json");
    const SkinFileCopy copied = copy_skin_files(skin_dir, steam);
    if (!copied.ok) {
        const std::string text = wide_to_utf8(copied.detail);
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }
    lua_pushstring(L, "SteamSkin will use this color the next time Steam opens. The music player was left as it is.");
    return 1;
}

}  // namespace

bool can_park(lua_State* L) {
    HostState& host = host_of(L);
    return host.running != nullptr && lua_isyieldable(L) != 0;
}

struct StateThread::Impl {
    Bus& bus;
    HostState host;
    std::thread thread;
    HANDLE loaded_event = nullptr;
    std::atomic<bool> main_loaded{false};
    std::atomic<bool> on_load_done_ok{false};
    std::atomic<bool> ready{false};
    std::atomic<uint64_t> completed{0};
    bool started = false;

    lua_State* L = nullptr;
    std::unordered_map<uint64_t, std::unique_ptr<Coroutine>> coroutines;
    uint64_t next_co_id = 1;
    int lifecycle_ref = LUA_NOREF;

    Impl(Bus& b, HostConfig config) : bus(b) {
        host.config = std::move(config);
        host.bus = &b;
        loaded_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~Impl() {
        if (loaded_event != nullptr) {
            CloseHandle(loaded_event);
        }
    }

    // ---- setup -----------------------------------------------------------

    bool init_lua(std::string& error) {
        L = luaL_newstate();
        if (L == nullptr) {
            error = "luaL_newstate returned null: out of memory while creating the Lua state.";
            return false;
        }
        lua_atpanic(L, lua_panic);
        set_host_of(L, &host);

        static const luaL_Reg libs[] = {
            {LUA_GNAME, luaopen_base},       {LUA_LOADLIBNAME, luaopen_package}, {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
            {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},    {LUA_UTF8LIBNAME, luaopen_utf8},    {LUA_DBLIBNAME, luaopen_debug},
            {nullptr, nullptr},
        };
        for (const luaL_Reg* lib = libs; lib->func != nullptr; ++lib) {
            luaL_requiref(L, lib->name, lib->func, 1);
            lua_pop(L, 1);
        }
        // dofile and loadfile call fopen with an ANSI path. Nothing in the player uses them.
        lua_pushnil(L);
        lua_setglobal(L, "dofile");
        lua_pushnil(L);
        lua_setglobal(L, "loadfile");

        open_host_os(L);
        open_host_io(L);
        open_host_json(L);
        open_host_fs(L);
        open_host_logger(L);
        open_host_utils(L);
        open_host_millennium(L);
        install_host_searcher(L);
        lua_pushcfunction(L, lua_doomnet_install_app);
        lua_setglobal(L, "doomnet_install_app");
        lua_pushcfunction(L, lua_doomnet_skin_color);
        lua_setglobal(L, "doomnet_skin_color");
        lua_pushcfunction(L, lua_doomnet_set_skin_color);
        lua_setglobal(L, "doomnet_set_skin_color");
        lua_pushcfunction(L, lua_begin_update_check);
        lua_setglobal(L, "doomnet_begin_update_check");
        lua_pushcfunction(L, lua_update_check_result);
        lua_setglobal(L, "doomnet_update_check_result");
        lua_pushcfunction(L, lua_update_notice);
        lua_setglobal(L, "doomnet_update_notice");
        lua_pushcfunction(L, lua_begin_folder_pick);
        lua_setglobal(L, "doomnet_begin_folder_pick");
        lua_pushcfunction(L, lua_folder_pick_result);
        lua_setglobal(L, "doomnet_folder_pick_result");
        lua_pushcfunction(L, lua_stream_hook);
        lua_setglobal(L, "doomnet_stream_hook");

        // require finds the player's modules next to main.lua, and nowhere else.
        std::string search = host.backend_dir_utf8 + "\\?.lua";
        lua_getglobal(L, "package");
        lua_pushlstring(L, search.data(), search.size());
        lua_setfield(L, -2, "path");
        lua_pushliteral(L, "");
        lua_setfield(L, -2, "cpath");
        lua_pop(L, 1);

        lua_pushlstring(L, host.backend_dir_utf8.data(), host.backend_dir_utf8.size());
        lua_setglobal(L, "MILLENNIUM_PLUGIN_SECRET_BACKEND_ABSOLUTE");

        if (luaL_dostring(L, kCompatSource) != LUA_OK) {
            const char* message = lua_tostring(L, -1);
            error = std::string("The Lua 5.1 compatibility globals failed to load: ") + (message != nullptr ? message : "?");
            lua_pop(L, 1);
            return false;
        }
        if (luaL_dostring(L, kFolderPickSource) != LUA_OK) {
            const char* message = lua_tostring(L, -1);
            error = std::string("The music folder chooser failed to load: ") + (message != nullptr ? message : "?");
            lua_pop(L, 1);
            return false;
        }
        if (luaL_dostring(L, kUpdateCheckSource) != LUA_OK) {
            const char* message = lua_tostring(L, -1);
            error = std::string("The update check failed to load: ") + (message != nullptr ? message : "?");
            lua_pop(L, 1);
            return false;
        }
        return true;
    }

    Coroutine& new_coroutine(Coroutine::Kind kind, const std::string& method) {
        auto entry = std::make_unique<Coroutine>();
        entry->id = next_co_id++;
        entry->kind = kind;
        entry->method = method;
        entry->thread = lua_newthread(L);
        entry->registry_ref = luaL_ref(L, LUA_REGISTRYINDEX);  // pops the thread and anchors it
        entry->started_ms = monotonic_ms();
        Coroutine& ref = *entry;
        coroutines.emplace(ref.id, std::move(entry));
        return ref;
    }

    void release(Coroutine& c) {
        const uint64_t id = c.id;
        luaL_unref(L, LUA_REGISTRYINDEX, c.registry_ref);
        coroutines.erase(id);  // destroys c
    }

    static std::string describe(const Coroutine& c) {
        switch (c.kind) {
            case Coroutine::Kind::Load:
                return "main.lua";
            case Coroutine::Kind::OnLoad:
                return "on_load";
            case Coroutine::Kind::Lifecycle:
                return "lifecycle callback '" + c.method + "'";
            default:
                return "'" + c.method + "' (page call " + std::to_string(c.call_id) + ")";
        }
    }

    void resolve_error(uint64_t call_id, const std::string& message, const std::string& method = std::string()) {
        ResolveMsg out;
        out.call_id = call_id;
        out.ok = false;
        out.method = method;
        out.playback = audio_load_call(method);
        out.urgent = page_jump_reply(method);
        out.json = message;
        bus.to_browser(std::move(out));
    }

    // ---- loading ---------------------------------------------------------

    void signal_loaded() { SetEvent(loaded_event); }

    bool start_main() {
        const std::wstring path = host.config.backend_dir + L"\\main.lua";
        std::string source;
        std::wstring read_error;
        if (!read_whole_file(path, source, read_error)) {
            note_wide(L"error", L"main.lua could not be read, so the player did not start. " + read_error);
            return false;
        }
        source = patched_player_module("main", std::move(source));
        const std::string chunk_name = "@" + wide_to_utf8(path);
        if (luaL_loadbufferx(L, source.data(), source.size(), chunk_name.c_str(), "t") != LUA_OK) {
            const char* message = lua_tostring(L, -1);
            note(L"error", std::string("main.lua did not compile, so the player did not start: ") + (message != nullptr ? message : "?"));
            lua_pop(L, 1);
            return false;
        }
        Coroutine& c = new_coroutine(Coroutine::Kind::Load, "main.lua");
        lua_xmove(L, c.thread, 1);
        resume(c, 0);
        return true;
    }

    void run_post_load() {
        const std::string& source = host.config.post_load_lua;
        if (source.empty()) {
            return;
        }
        if (luaL_loadbufferx(L, source.data(), source.size(), "=doomnet post-load", "t") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
            const char* message = lua_tostring(L, -1);
            note(L"error", std::string("The host's post-load Lua failed and was skipped: ") + (message != nullptr ? message : "?"));
            lua_pop(L, 1);
            return;
        }
        note(L"info", "The host's post-load Lua ran.");
    }

    void start_on_load() {
        if (lifecycle_ref == LUA_NOREF) {
            note(L"warn", "main.lua returned no table, so there is no on_load to run. The player's global functions are still callable.");
            on_load_done_ok = true;
            signal_loaded();
            return;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, lifecycle_ref);
        lua_getfield(L, -1, "on_load");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            note(L"warn", "main.lua's returned table has no on_load function. Continuing without it.");
            on_load_done_ok = true;
            signal_loaded();
            return;
        }
        Coroutine& c = new_coroutine(Coroutine::Kind::OnLoad, "on_load");
        lua_xmove(L, c.thread, 1);
        lua_pop(L, 1);  // the lifecycle table
        note(L"info", "Running on_load.");
        resume(c, 0);
    }

    void start_lifecycle(const std::string& name, uint64_t call_id) {
        std::string field = name;
        const size_t dot = field.find('.');
        if (dot != std::string::npos) {
            field.erase(0, dot + 1);
        }
        if (lifecycle_ref == LUA_NOREF) {
            resolve_error(call_id, "The player has no lifecycle table (main.lua returned none).");
            return;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, lifecycle_ref);
        lua_getfield(L, -1, field.c_str());
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            ResolveMsg out;
            out.call_id = call_id;
            out.ok = true;
            out.json = "null";
            bus.to_browser(std::move(out));  // nothing to run is not an error
            return;
        }
        Coroutine& c = new_coroutine(Coroutine::Kind::Lifecycle, field);
        c.call_id = call_id;
        lua_xmove(L, c.thread, 1);
        lua_pop(L, 1);
        resume(c, 0);
    }

    // ---- messages --------------------------------------------------------

    void on_call(CallMsg& message) {
        if (!main_loaded.load()) {
            resolve_error(message.call_id, "The player's Lua code did not load, so '" + message.method + "' cannot run. See doomnet.log for the reason.");
            return;
        }
        if (message.method == kLifecycleFrontendLoaded) {
            start_lifecycle(message.method, message.call_id);
            return;
        }
        if (!method_name_ok(message.method)) {
            resolve_error(message.call_id, "'" + message.method.substr(0, 60) + "' is not a valid Lua function name for a page call.");
            return;
        }
        Coroutine& c = new_coroutine(Coroutine::Kind::Call, message.method);
        c.call_id = message.call_id;
        if (artwork_method(message.method)) {
            lua_sethook(c.thread, art_preempt_hook, LUA_MASKCOUNT, 8000);
        }
        c.parking_reads = std::find(host.config.parking_read_methods.begin(), host.config.parking_read_methods.end(), message.method) != host.config.parking_read_methods.end();
        lua_getglobal(c.thread, message.method.c_str());
        if (!lua_isfunction(c.thread, -1)) {
            const uint64_t call_id = c.call_id;
            const std::string missing = "The player has no Lua function named '" + message.method + "'. callServerMethod resolves a global function by name.";
            release(c);
            note(L"error", missing);
            resolve_error(call_id, missing);
            return;
        }
        int pushed = 0;
        lua_checkstack(c.thread, static_cast<int>(message.args.size()) + 4);
        for (const WireValue& arg : message.args) {
            switch (arg.kind) {
                case WireValue::Kind::Boolean:
                    lua_pushboolean(c.thread, arg.boolean ? 1 : 0);
                    break;
                case WireValue::Kind::Number:
                    lua_pushnumber(c.thread, static_cast<lua_Number>(arg.number));
                    break;
                case WireValue::Kind::String:
                    lua_pushlstring(c.thread, arg.text.data(), arg.text.size());
                    break;
                default:
                    lua_pushnil(c.thread);
                    break;
            }
            ++pushed;
        }
        resume(c, pushed);
    }

    // Pushes what a parked coroutine is waiting for. Returns how many values.
    int push_resume_values(Coroutine& c, ResumeMsg& message) {
        lua_State* t = c.thread;
        lua_checkstack(t, 6);
        switch (c.waiting) {
            case Coroutine::Wait::Sleep:
                return 0;
            case Coroutine::Wait::Frontend: {
                if (message.failed) {
                    lua_pushnil(t);
                    lua_pushlstring(t, message.error.data(), message.error.size());
                    return 2;
                }
                switch (message.value.kind) {
                    case WireValue::Kind::Boolean:
                        lua_pushboolean(t, message.value.boolean ? 1 : 0);
                        return 1;
                    case WireValue::Kind::Number:
                        lua_pushnumber(t, static_cast<lua_Number>(message.value.number));
                        return 1;
                    case WireValue::Kind::String:
                        lua_pushlstring(t, message.value.text.data(), message.value.text.size());
                        return 1;
                    case WireValue::Kind::Json: {
                        std::string error;
                        if (decode_json_value(t, message.value.text, error)) {
                            return 1;
                        }
                        lua_pushnil(t);
                        const std::string text = "The page returned an object that json.decode rejected: " + error;
                        lua_pushlstring(t, text.data(), text.size());
                        return 2;
                    }
                    default:
                        lua_pushnil(t);
                        return 1;
                }
            }
            case Coroutine::Wait::ReadRange:
                if (message.failed) {
                    lua_pushnil(t);
                    lua_pushlstring(t, message.error.data(), message.error.size());
                    return 2;
                }
                lua_pushlstring(t, message.value.text.data(), message.value.text.size());
                return 1;
            case Coroutine::Wait::Exec:
                if (message.failed) {
                    lua_pushnil(t);
                    lua_pushlstring(t, message.error.data(), message.error.size());
                    return 2;
                }
                lua_pushlstring(t, message.value.text.data(), message.value.text.size());
                lua_pushnumber(t, static_cast<lua_Number>(message.exit_code));
                return 2;
            default:
                return 0;
        }
    }

    void on_resume(ResumeMsg& message) {
        const auto found = coroutines.find(message.co_id);
        if (found == coroutines.end()) {
            note(L"warn", "A resume arrived for coroutine " + std::to_string(message.co_id) + ", which is no longer parked. It was ignored.");
            return;
        }
        Coroutine& c = *found->second;
        if (c.waiting == Coroutine::Wait::None) {
            note(L"warn", "A resume arrived for " + describe(c) + ", which was not waiting for anything. It was ignored.");
            return;
        }
        const int count = push_resume_values(c, message);
        c.waiting = Coroutine::Wait::None;
        resume(c, count);
    }

    // ---- running a coroutine --------------------------------------------

    void resume(Coroutine& c, int argument_count) {
        host.running = &c;
        const uint64_t began = monotonic_ms();
        int result_count = 0;
        const int status = lua_resume(c.thread, L, argument_count, &result_count);
        host.running = nullptr;
        const uint64_t took = monotonic_ms() - began;
        c.longest_stretch_ms = std::max(c.longest_stretch_ms, took);
        if (took >= kSlowStretchMs) {
            note(L"warn", describe(c) + " ran for " + std::to_string(took) + " ms without waiting on anything. Every other call was held for that long.");
        }
        if (status == LUA_YIELD) {
            park(c, result_count);
        } else if (status == LUA_OK) {
            finish_ok(c, result_count);
        } else {
            finish_error(c);
        }
    }

    void park(Coroutine& c, int yielded_values) {
        lua_pop(c.thread, yielded_values);
        YieldRequest request = std::move(host.pending);
        host.pending = YieldRequest{};
        switch (request.kind) {
            case YieldRequest::Kind::Sleep: {
                c.waiting = Coroutine::Wait::Sleep;
                SleepMsg out;
                out.co_id = c.id;
                out.milliseconds = request.sleep_ms;
                bus.to_browser(std::move(out));
                return;
            }
            case YieldRequest::Kind::Frontend: {
                c.waiting = Coroutine::Wait::Frontend;
                FrontendMsg out;
                out.co_id = c.id;
                out.method = std::move(request.method);
                out.params_json = std::move(request.params_json);
                out.caller = std::move(request.caller);
                bus.to_browser(std::move(out));
                return;
            }
            case YieldRequest::Kind::ReadRange: {
                c.waiting = Coroutine::Wait::ReadRange;
                ReadRangeMsg out;
                out.co_id = c.id;
                out.file = request.file;
                out.path = std::move(request.path);
                out.offset = request.offset;
                out.length = request.length;
                bus.to_browser(std::move(out));
                return;
            }
            case YieldRequest::Kind::Exec: {
                c.waiting = Coroutine::Wait::Exec;
                ExecMsg out;
                out.co_id = c.id;
                out.command_line = std::move(request.command_line);
                bus.to_browser(std::move(out));
                return;
            }
            case YieldRequest::Kind::Preempt:
                c.waiting = Coroutine::Wait::Preempt;
                return;
            default:
                break;
        }
        // A bare coroutine.yield() from Lua code. Nothing will ever resume it.
        const std::string message = describe(c) + " called coroutine.yield() directly. Only the host's utils.sleep, utils.exec, call_frontend_method and large file reads may yield, so this call was ended.";
        note(L"error", message);
        fail(c, message);
    }

    // Ends a coroutine with an error. Settles the page call if there is one.
    void fail(Coroutine& c, const std::string& message) {
        const Coroutine::Kind kind = c.kind;
        const uint64_t call_id = c.call_id;
        const std::string method = c.method;
        release(c);
        after_end(kind, call_id, false, message, method);
    }

    void after_end(Coroutine::Kind kind, uint64_t call_id, bool ok, const std::string& error_message, const std::string& method = std::string()) {
        switch (kind) {
            case Coroutine::Kind::Load:
                if (!ok) {
                    signal_loaded();
                }
                break;
            case Coroutine::Kind::OnLoad:
                on_load_done_ok = ok;
                signal_loaded();
                break;
            default:
                if (call_id != 0 && !ok) {
                    resolve_error(call_id, error_message, method);
                }
                break;
        }
        ++completed;
    }

    void finish_ok(Coroutine& c, int result_count) {
        const Coroutine::Kind kind = c.kind;
        const uint64_t call_id = c.call_id;
        const std::string label = describe(c);
        const uint64_t elapsed = monotonic_ms() - c.started_ms;

        if (kind == Coroutine::Kind::Load) {
            bool have_table = false;
            if (result_count >= 1) {
                const int first = lua_gettop(c.thread) - result_count + 1;
                if (lua_istable(c.thread, first)) {
                    lua_pushvalue(c.thread, first);
                    lua_xmove(c.thread, L, 1);
                    lifecycle_ref = luaL_ref(L, LUA_REGISTRYINDEX);
                    have_table = true;
                }
            }
            release(c);
            main_loaded = true;
            note(L"info", have_table ? "main.lua loaded and returned its lifecycle table." : "main.lua loaded but did not return a table.");
            run_post_load();
            start_on_load();
            return;
        }

        if (kind == Coroutine::Kind::Call || kind == Coroutine::Kind::Lifecycle) {
            std::string json = "null";
            std::string encode_error;
            bool encoded = true;
            if (result_count >= 1) {
                const int first = lua_gettop(c.thread) - result_count + 1;
                lua_pushvalue(c.thread, first);
                lua_xmove(c.thread, L, 1);
                encoded = encode_json_value(L, -1, json, encode_error);
                lua_pop(L, 1);
            }
            const std::string method = c.method;
            release(c);
            if (call_id != 0) {
                ResolveMsg out;
                out.call_id = call_id;
                out.method = method;
                out.playback = audio_load_call(method);
                out.urgent = page_jump_reply(method);
                if (encoded) {
                    out.ok = true;
                    out.json = std::move(json);
                } else {
                    out.ok = false;
                    out.json = "The Lua function returned a value that cannot be sent to the page: " + encode_error;
                    note(L"error", label + " returned a value json.encode rejected: " + encode_error);
                }
                bus.to_browser(std::move(out));
            }
            ++completed;
            return;
        }

        release(c);  // OnLoad
        note(L"info", "on_load returned after " + std::to_string(elapsed) + " ms.");
        after_end(kind, call_id, true, std::string());
    }

    void finish_error(Coroutine& c) {
        const Coroutine::Kind kind = c.kind;
        const uint64_t call_id = c.call_id;
        const std::string label = describe(c);

        const char* raw = lua_tostring(c.thread, -1);
        std::string message = raw != nullptr ? raw : std::string("(the error value is a ") + luaL_typename(c.thread, -1) + ", not text)";
        std::string trace = message;
        luaL_traceback(L, c.thread, message.c_str(), 0);
        if (const char* text = lua_tostring(L, -1)) {
            trace = text;
        }
        lua_pop(L, 1);

        const std::string method = c.method;
        release(c);
        note(L"error", label + " failed with a Lua error: " + trace);
        after_end(kind, call_id, false, message, method);
    }

    // ---- main loop -------------------------------------------------------

    void run() {
        std::string setup_error;
        bool lua_ok = false;
        try {
            lua_ok = init_lua(setup_error);
        } catch (const std::exception& error) {
            setup_error = std::string("C++ exception while creating the Lua state: ") + error.what();
        }
        if (!lua_ok) {
            note(L"error", "The Lua host could not start. " + setup_error);
            signal_loaded();
        } else {
            try {
                if (!start_main()) {
                    signal_loaded();
                }
            } catch (const std::exception& error) {
                note(L"error", std::string("C++ exception while loading main.lua: ") + error.what());
                signal_loaded();
            }
        }

        DWORD_PTR lua_mask = 0;
        DWORD_PTR art_mask = 0;
        Bus::cpu_masks(lua_mask, art_mask);
        if (lua_mask != 0 && art_mask != 0) {
            if (SetThreadAffinityMask(GetCurrentThread(), lua_mask) == 0) {
                note_wide(L"warn", L"Could not pin the Lua thread to its CPU. Playback still runs before artwork. " + windows_error_text(GetLastError()));
            }
        }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        note(L"info", "Playback calls run before artwork calls. An artwork call yields when a song is waiting to play.");

        for (;;) {
            StateInbox message;
            if (!bus.try_take_state(message)) {
                Coroutine* preempted = nullptr;
                for (auto& entry : coroutines) {
                    if (entry.second->waiting == Coroutine::Wait::Preempt) {
                        preempted = entry.second.get();
                        break;
                    }
                }
                if (preempted != nullptr && !bus.playback_queued()) {
                    preempted->waiting = Coroutine::Wait::None;
                    resume(*preempted, 0);
                    continue;
                }
                message = bus.wait_state();
            }
            if (std::holds_alternative<StopStateMsg>(message)) {
                break;
            }
            try {
                if (auto* call = std::get_if<CallMsg>(&message)) {
                    if (L == nullptr) {
                        resolve_error(call->call_id, "The Lua host is not running: " + setup_error);
                    } else {
                        on_call(*call);
                    }
                } else if (auto* resume_message = std::get_if<ResumeMsg>(&message)) {
                    if (L != nullptr) {
                        on_resume(*resume_message);
                    }
                }
            } catch (const std::exception& error) {
                host.running = nullptr;
                note(L"error", std::string("The state thread caught a C++ exception and kept running: ") + error.what());
            }
        }

        if (L != nullptr) {
            note(L"info", "Stopping the Lua host: " + std::to_string(coroutines.size()) + " parked coroutine(s) are dropped.");
            coroutines.clear();
            lua_close(L);
            L = nullptr;
        }
    }
};

StateThread::StateThread(Bus& bus, HostConfig config) : impl_(std::make_unique<Impl>(bus, std::move(config))) {}

StateThread::~StateThread() {
    stop();
}

bool StateThread::start(std::wstring& error) {
    error.clear();
    if (impl_->started) {
        error = L"The state thread was already started.";
        return false;
    }
    if (impl_->loaded_event == nullptr || !impl_->bus.valid()) {
        error = L"The state thread could not be created because a Windows event could not be created. " + windows_error_text(GetLastError());
        log_event(L"error", L"lua", error.c_str());
        return false;
    }
    HostConfig& config = impl_->host.config;
    if (config.install_path.empty()) {
        error = L"The state thread needs an install_path (DoomNet's data folder).";
        log_event(L"error", L"lua", error.c_str());
        return false;
    }
    // The player writes bootstrap.js into <install>\themes\steam-music-player. With this
    // folder present that write stays inside DoomNet's data, not in the Steam install.
    ensure_directory(config.install_path);
    ensure_directory(config.install_path + L"\\themes");
    if (config.steam_path.empty()) {
        config.steam_path = config.install_path + L"\\no-steam-found";
        log_event(L"warn", L"lua", (L"No Steam folder was given. millennium.steam_path() will return '" + config.steam_path + L"' so the player's writes under \"<steam>\\steamui\" stay in DoomNet's data.").c_str());
    }
    impl_->host.backend_dir_utf8 = wide_to_utf8(config.backend_dir);
    impl_->host.steam_path_utf8 = wide_to_utf8(config.steam_path);
    impl_->host.install_path_utf8 = wide_to_utf8(config.install_path);
    try {
        impl_->thread = std::thread([this] { impl_->run(); });
    } catch (const std::exception& ex) {
        error = L"The state thread could not be created: " + utf8_to_wide_lossy(ex.what());
        log_event(L"error", L"lua", error.c_str());
        return false;
    }
    impl_->started = true;
    log_event(L"info", L"lua", (L"The state thread started for the player at '" + config.backend_dir + L"'.").c_str());
    return true;
}

bool StateThread::wait_loaded(unsigned long timeout_ms) {
    return WaitForSingleObject(impl_->loaded_event, timeout_ms) == WAIT_OBJECT_0;
}

bool StateThread::main_lua_loaded() const { return impl_->main_loaded.load(); }
bool StateThread::on_load_ok() const { return impl_->on_load_done_ok.load(); }
bool StateThread::ready_signalled() const { return impl_->host.ready_signalled; }
uint64_t StateThread::calls_completed() const { return impl_->completed.load(); }

void StateThread::stop() {
    if (!impl_ || !impl_->started) {
        return;
    }
    impl_->started = false;
    impl_->bus.to_state(StopStateMsg{});
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

}  // namespace doomnet
