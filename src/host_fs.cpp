// Spec: "Lua host and the four shims". The `fs` module, path helpers, and the
// searcher that lets `require` find the player's modules.
//
// Read: Millennium src/lua_host/api/filesystem.cc on main (commit
// c05144b98f7e05242d03b251184f60bc472d1023). The function list, the argument
// order, and the return shapes below match it. What differs, on purpose:
//   - paths are UTF-8 in Lua and wide on the way to Windows;
//   - list/list_recursive never throw for one bad file name, and report an
//     error as `nil, message` like the other functions;
//   - fs.join(nil, ...) does not throw (player_state.lua depends on that): it
//     returns `nil, message` when any argument is not a string or a number.

#include "anchors.h"
#include "host_internal.h"
#include "log.h"
#include "text.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>

namespace doomnet {

void host_fs_anchor() {}

namespace fsn = std::filesystem;

// ---- shared path helpers -------------------------------------------------

bool try_wide_path(lua_State* L, int index, std::wstring& out) {
    if (lua_type(L, index) != LUA_TSTRING) {
        return false;
    }
    size_t length = 0;
    const char* bytes = lua_tolstring(L, index, &length);
    return lua_path_to_wide(bytes, length, out);
}

std::wstring check_wide_path(lua_State* L, int index, const char* what) {
    size_t length = 0;
    const char* bytes = luaL_checklstring(L, index, &length);
    std::wstring wide;
    if (!lua_path_to_wide(bytes, length, wide)) {
        // Build the message, push it, and let the caller's frame unwind first.
        lua_pushfstring(L, "%s must be a usable file path; it was empty, held a NUL byte, or was not valid text", what);
        std::wstring().swap(wide);  // release it now: lua_error longjmps past this frame's destructor
        lua_error(L);
    }
    return wide;
}

bool read_whole_file(const std::wstring& path, std::string& out, std::wstring& error) {
    out.clear();
    error.clear();
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = L"Could not open '" + path + L"' for reading. " + windows_error_text(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0) {
        error = L"Could not read the size of '" + path + L"'. " + windows_error_text(GetLastError());
        CloseHandle(handle);
        return false;
    }
    if (static_cast<unsigned long long>(size.QuadPart) > (1ull << 31)) {
        error = L"Refusing to read '" + path + L"' into one Lua string because it is larger than 2 GiB.";
        CloseHandle(handle);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - done, 16u * 1024u * 1024u));
        DWORD got = 0;
        if (!ReadFile(handle, out.data() + done, want, &got, nullptr)) {
            error = L"Could not read '" + path + L"' after " + std::to_wstring(done) + L" bytes. " + windows_error_text(GetLastError());
            CloseHandle(handle);
            out.clear();
            return false;
        }
        if (got == 0) {
            break;  // the file shrank while we read it
        }
        done += got;
    }
    CloseHandle(handle);
    out.resize(done);
    return true;
}

namespace {

std::string to_utf8(const fsn::path& path) {
    return wide_to_utf8(path.native());
}

void push_path(lua_State* L, const fsn::path& path) {
    const std::string text = to_utf8(path);
    lua_pushlstring(L, text.data(), text.size());
}

// Leaves `nil, message` on the stack.
int push_error_pair(lua_State* L, const std::error_code& code) {
    lua_pushnil(L);
    const std::string message = code.message();
    lua_pushlstring(L, message.data(), message.size());
    return 2;
}

// Reads a required string argument as a path. Returns false, with `nil, message`
// left on the stack via the caller, when the bytes are not a usable path.
bool arg_path(lua_State* L, int index, fsn::path& out) {
    size_t length = 0;
    const char* bytes = luaL_checklstring(L, index, &length);
    std::wstring wide;
    if (!lua_path_to_wide(bytes, length, wide)) {
        return false;
    }
    out = fsn::path(wide);
    return true;
}

int bad_path_pair(lua_State* L, int index) {
    lua_pushnil(L);
    lua_pushfstring(L, "argument %d is not a usable file path (empty, a NUL byte, or not valid text)", index);
    return 2;
}

// Boolean queries answer false for anything that is not a path. Millennium
// raised for a non-string, which only turned a missing value into a stack trace.
int boolean_query(lua_State* L, bool (*check)(const fsn::path&, std::error_code&)) {
    fsn::path path;
    std::wstring wide;
    if (!try_wide_path(L, 1, wide)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    path = fsn::path(wide);
    std::error_code ec;
    const bool result = check(path, ec);
    lua_pushboolean(L, result && !ec);
    return 1;
}

int Lua_exists(lua_State* L) {
    return boolean_query(L, [](const fsn::path& p, std::error_code& ec) { return fsn::exists(p, ec); });
}
int Lua_is_directory(lua_State* L) {
    return boolean_query(L, [](const fsn::path& p, std::error_code& ec) { return fsn::is_directory(p, ec); });
}
int Lua_is_file(lua_State* L) {
    return boolean_query(L, [](const fsn::path& p, std::error_code& ec) { return fsn::is_regular_file(p, ec); });
}
int Lua_is_symlink(lua_State* L) {
    return boolean_query(L, [](const fsn::path& p, std::error_code& ec) { return fsn::is_symlink(p, ec); });
}
int Lua_is_empty(lua_State* L) {
    return boolean_query(L, [](const fsn::path& p, std::error_code& ec) { return fsn::is_empty(p, ec); });
}

// Matches Millennium: `bool [, message]`.
int bool_with_message(lua_State* L, bool result, const std::error_code& ec) {
    lua_pushboolean(L, result);
    if (ec) {
        const std::string message = ec.message();
        lua_pushlstring(L, message.data(), message.size());
        return 2;
    }
    return 1;
}

int Lua_create_directory(lua_State* L) {
    fsn::path path;
    if (!arg_path(L, 1, path)) {
        return bad_path_pair(L, 1);
    }
    std::error_code ec;
    const bool made = fsn::create_directory(path, ec);
    return bool_with_message(L, made, ec);
}

int Lua_create_directories(lua_State* L) {
    fsn::path path;
    if (!arg_path(L, 1, path)) {
        return bad_path_pair(L, 1);
    }
    std::error_code ec;
    const bool made = fsn::create_directories(path, ec);
    return bool_with_message(L, made, ec);
}

int Lua_remove(lua_State* L) {
    fsn::path path;
    if (!arg_path(L, 1, path)) {
        return bad_path_pair(L, 1);
    }
    std::error_code ec;
    const bool removed = fsn::remove(path, ec);
    return bool_with_message(L, removed, ec);
}

int Lua_remove_all(lua_State* L) {
    fsn::path path;
    if (!arg_path(L, 1, path)) {
        return bad_path_pair(L, 1);
    }
    std::error_code ec;
    const uintmax_t count = fsn::remove_all(path, ec);
    push_count(L, count);
    if (ec) {
        const std::string message = ec.message();
        lua_pushlstring(L, message.data(), message.size());
        return 2;
    }
    return 1;
}

int Lua_list(lua_State* L) {
    fsn::path path;
    if (!arg_path(L, 1, path)) {
        return bad_path_pair(L, 1);
    }
    std::error_code ec;
    fsn::directory_iterator it(path, fsn::directory_options::skip_permission_denied, ec);
    if (ec) {
        return push_error_pair(L, ec);
    }
    const fsn::directory_iterator end;
    lua_newtable(L);
    lua_Integer index = 1;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const fsn::directory_entry& entry = *it;
        std::error_code entry_ec;
        lua_newtable(L);
        push_path(L, entry.path().filename());
        lua_setfield(L, -2, "name");
        push_path(L, entry.path());
        lua_setfield(L, -2, "path");
        lua_pushboolean(L, entry.is_directory(entry_ec));
        lua_setfield(L, -2, "is_directory");
        const bool is_file = entry.is_regular_file(entry_ec);
        lua_pushboolean(L, is_file);
        lua_setfield(L, -2, "is_file");
        lua_pushboolean(L, entry.is_symlink(entry_ec));
        lua_setfield(L, -2, "is_symlink");
        if (is_file) {
            std::error_code size_ec;
            const auto size = entry.file_size(size_ec);
            if (!size_ec) {
                push_count(L, size);
                lua_setfield(L, -2, "size");
            }
        }
        lua_rawseti(L, -2, index++);
    }
    if (ec) {
        // A listing that stopped part way is returned with the reason, so the
        // caller keeps what was read. Millennium threw and returned nothing.
        const std::string message = "directory listing stopped early: " + ec.message();
        lua_pushlstring(L, message.data(), message.size());
        return 2;
    }
    return 1;
}

int Lua_list_recursive(lua_State* L) {
    fsn::path path;
    if (!arg_path(L, 1, path)) {
        return bad_path_pair(L, 1);
    }
    std::error_code ec;
    fsn::recursive_directory_iterator it(path, fsn::directory_options::skip_permission_denied, ec);
    if (ec) {
        return push_error_pair(L, ec);
    }
    const fsn::recursive_directory_iterator end;
    lua_newtable(L);
    lua_Integer index = 1;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const fsn::directory_entry& entry = *it;
        std::error_code entry_ec;
        lua_newtable(L);
        push_path(L, entry.path().filename());
        lua_setfield(L, -2, "name");
        push_path(L, entry.path());
        lua_setfield(L, -2, "path");
        lua_pushboolean(L, entry.is_directory(entry_ec));
        lua_setfield(L, -2, "is_directory");
        lua_pushboolean(L, entry.is_regular_file(entry_ec));
        lua_setfield(L, -2, "is_file");
        push_number(L, static_cast<double>(it.depth()));
        lua_setfield(L, -2, "depth");
        lua_rawseti(L, -2, index++);
    }
    if (ec) {
        const std::string message = "directory listing stopped early: " + ec.message();
        lua_pushlstring(L, message.data(), message.size());
        return 2;
    }
    return 1;
}

// copy / copy_recursive / rename share the (from, to, throw_error) shape.
// Returns -1 when an error message is on the stack and the caller must raise it.
// The raising is done by the caller, which holds no C++ objects, because lua_error
// longjmps and the unwind of live strings/paths in this frame is not safe.
int copy_like_impl(lua_State* L, int mode) {
    fsn::path from;
    fsn::path to;
    const bool from_ok = arg_path(L, 1, from);
    const bool to_ok = arg_path(L, 2, to);
    const bool throw_error = lua_toboolean(L, 3) != 0;
    std::error_code ec;
    if (!from_ok || !to_ok) {
        ec = std::make_error_code(std::errc::invalid_argument);
    } else if (mode == 0) {
        fsn::copy(from, to, fsn::copy_options::overwrite_existing, ec);
    } else if (mode == 1) {
        fsn::copy(from, to, fsn::copy_options::overwrite_existing | fsn::copy_options::recursive, ec);
    } else {
        fsn::rename(from, to, ec);
    }
    if (ec) {
        if (throw_error) {
            const std::string message = ec.message();
            lua_pushlstring(L, message.data(), message.size());
            return -1;
        }
        return push_error_pair(L, ec);
    }
    lua_pushboolean(L, 1);
    return 1;
}

int copy_like(lua_State* L, int mode) {
    const int pushed = copy_like_impl(L, mode);
    if (pushed < 0) {
        return lua_error(L);
    }
    return pushed;
}

int Lua_copy(lua_State* L) { return copy_like(L, 0); }
int Lua_copy_recursive(lua_State* L) { return copy_like(L, 1); }
int Lua_rename(lua_State* L) { return copy_like(L, 2); }

int Lua_file_size(lua_State* L) {
    fsn::path path;
    if (!arg_path(L, 1, path)) {
        return bad_path_pair(L, 1);
    }
    std::error_code ec;
    const auto size = fsn::file_size(path, ec);
    if (ec) {
        return push_error_pair(L, ec);
    }
    push_count(L, size);
    return 1;
}

int Lua_last_write_time(lua_State* L) {
    fsn::path path;
    if (!arg_path(L, 1, path)) {
        return bad_path_pair(L, 1);
    }
    std::error_code ec;
    const auto ftime = fsn::last_write_time(path, ec);
    if (ec) {
        return push_error_pair(L, ec);
    }
    const auto system_time = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
    push_count(L, static_cast<unsigned long long>(std::chrono::system_clock::to_time_t(system_time)));
    return 1;
}

int Lua_current_path(lua_State* L) {
    std::error_code ec;
    const auto path = fsn::current_path(ec);
    if (ec) {
        return push_error_pair(L, ec);
    }
    push_path(L, path);
    return 1;
}

int Lua_set_current_path(lua_State* L) {
    fsn::path path;
    if (!arg_path(L, 1, path)) {
        return bad_path_pair(L, 1);
    }
    std::error_code ec;
    fsn::current_path(path, ec);
    return bool_with_message(L, !ec, ec);
}

int path_transform(lua_State* L, int mode) {
    fsn::path path;
    if (!arg_path(L, 1, path)) {
        return bad_path_pair(L, 1);
    }
    std::error_code ec;
    fsn::path result;
    if (mode == 0) {
        result = fsn::absolute(path, ec);
    } else if (mode == 1) {
        result = fsn::canonical(path, ec);
    } else {
        std::wstring base_wide;
        if (try_wide_path(L, 2, base_wide)) {
            result = fsn::relative(path, fsn::path(base_wide), ec);
        } else {
            result = fsn::relative(path, ec);
        }
    }
    if (ec) {
        return push_error_pair(L, ec);
    }
    push_path(L, result);
    return 1;
}

int Lua_absolute(lua_State* L) { return path_transform(L, 0); }
int Lua_canonical(lua_State* L) { return path_transform(L, 1); }
int Lua_relative(lua_State* L) { return path_transform(L, 2); }

// Pure path text operations. An unusable path gives "".
int path_part(lua_State* L, int mode) {
    fsn::path path;
    fsn::path result;
    if (arg_path(L, 1, path)) {
        switch (mode) {
            case 0: result = path.filename(); break;
            case 1: result = path.extension(); break;
            case 2: result = path.stem(); break;
            default: result = path.parent_path(); break;
        }
    }
    push_path(L, result);
    return 1;
}

int Lua_filename(lua_State* L) { return path_part(L, 0); }
int Lua_extension(lua_State* L) { return path_part(L, 1); }
int Lua_stem(lua_State* L) { return path_part(L, 2); }
int Lua_parent_path(lua_State* L) { return path_part(L, 3); }

int Lua_join(lua_State* L) {
    const int count = lua_gettop(L);
    fsn::path result;
    for (int i = 1; i <= count; ++i) {
        const int type = lua_type(L, i);
        if (type != LUA_TSTRING && type != LUA_TNUMBER) {
            // Not a throw. player_state.lua once died on fs.join(nil, ...).
            lua_pushnil(L);
            lua_pushfstring(L, "fs.join argument %d is %s, not a string, so no path was built", i, luaL_typename(L, i));
            return 2;
        }
        size_t length = 0;
        const char* bytes = lua_tolstring(L, i, &length);
        std::wstring wide;
        if (length == 0) {
            wide.clear();  // Millennium: path / "" adds a trailing separator. Same here.
        } else if (!lua_path_to_wide(bytes, length, wide)) {
            lua_pushnil(L);
            lua_pushfstring(L, "fs.join argument %d is not a usable path piece", i);
            return 2;
        }
        result /= fsn::path(wide);
    }
    push_path(L, result);
    return 1;
}

int Lua_space_info(lua_State* L) {
    fsn::path path;
    if (!arg_path(L, 1, path)) {
        return bad_path_pair(L, 1);
    }
    std::error_code ec;
    const auto space = fsn::space(path, ec);
    if (ec) {
        return push_error_pair(L, ec);
    }
    lua_newtable(L);
    push_count(L, space.capacity);
    lua_setfield(L, -2, "capacity");
    push_count(L, space.free);
    lua_setfield(L, -2, "free");
    push_count(L, space.available);
    lua_setfield(L, -2, "available");
    return 1;
}

const luaL_Reg kFsFunctions[] = {
    DOOMNET_LUA_FN(exists),
    DOOMNET_LUA_FN(is_directory),
    DOOMNET_LUA_FN(is_file),
    DOOMNET_LUA_FN(is_symlink),
    DOOMNET_LUA_FN(is_empty),
    DOOMNET_LUA_FN(create_directory),
    DOOMNET_LUA_FN(create_directories),
    DOOMNET_LUA_FN(remove),
    DOOMNET_LUA_FN(remove_all),
    {"list", guarded<&Lua_list>},
    {"list_recursive", guarded<&Lua_list_recursive>},
    DOOMNET_LUA_FN(copy),
    DOOMNET_LUA_FN(copy_recursive),
    DOOMNET_LUA_FN(rename),
    DOOMNET_LUA_FN(file_size),
    DOOMNET_LUA_FN(last_write_time),
    DOOMNET_LUA_FN(current_path),
    DOOMNET_LUA_FN(set_current_path),
    DOOMNET_LUA_FN(absolute),
    DOOMNET_LUA_FN(canonical),
    DOOMNET_LUA_FN(relative),
    DOOMNET_LUA_FN(filename),
    DOOMNET_LUA_FN(extension),
    DOOMNET_LUA_FN(stem),
    DOOMNET_LUA_FN(parent_path),
    DOOMNET_LUA_FN(join),
    DOOMNET_LUA_FN(space_info),
    {nullptr, nullptr},
};

int luaopen_host_fs(lua_State* L) {
    luaL_newlib(L, kFsFunctions);
    return 1;
}

// ---- require searcher ----------------------------------------------------

// Replacement for package.searchers[2]. Looks through package.path, opens the
// file by wide path, and loads it with a chunk name that carries the path.
// Returns -1 when an error value is on the stack and the caller must raise it (see copy_like_impl).
int host_lua_searcher_impl(lua_State* L) {
    size_t name_length = 0;
    const char* raw_name = luaL_checklstring(L, 1, &name_length);
    std::string module_name(raw_name, name_length);
    for (char& c : module_name) {
        if (c == '.') {
            c = '/';
        }
    }

    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    const char* path_list = lua_tostring(L, -1);
    std::string templates = path_list != nullptr ? path_list : "";
    lua_pop(L, 2);

    std::string tried;
    size_t start = 0;
    while (start <= templates.size()) {
        size_t end = templates.find(';', start);
        if (end == std::string::npos) {
            end = templates.size();
        }
        std::string candidate = templates.substr(start, end - start);
        start = end + 1;
        if (candidate.empty()) {
            continue;
        }
        size_t at = 0;
        while ((at = candidate.find('?', at)) != std::string::npos) {
            candidate.replace(at, 1, module_name);
            at += module_name.size();
        }
        std::wstring wide;
        if (!lua_path_to_wide(candidate.data(), candidate.size(), wide)) {
            tried += "\n\tno usable path '" + candidate + "'";
            continue;
        }
        const DWORD attrs = GetFileAttributesW(wide.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            tried += "\n\tno file '" + candidate + "'";
            continue;
        }
        std::string source;
        std::wstring read_error;
        if (!read_whole_file(wide, source, read_error)) {
            const std::string message = wide_to_utf8(read_error);
            lua_pushlstring(L, message.data(), message.size());
            return -1;
        }
        source = patched_player_module(module_name, std::move(source));
        const std::string chunk_name = "@" + candidate;
        if (luaL_loadbufferx(L, source.data(), source.size(), chunk_name.c_str(), "t") != LUA_OK) {
            const char* load_error = lua_tostring(L, -1);
            lua_pushfstring(L, "error loading module '%s' from file '%s':\n\t%s", raw_name, candidate.c_str(), load_error != nullptr ? load_error : "?");
            return -1;
        }
        lua_pushlstring(L, candidate.data(), candidate.size());
        return 2;
    }
    lua_pushlstring(L, tried.data(), tried.size());
    return 1;
}

int host_lua_searcher(lua_State* L) {
    const int pushed = host_lua_searcher_impl(L);
    if (pushed < 0) {
        return lua_error(L);
    }
    return pushed;
}

}  // namespace

std::string patched_player_module(const std::string& module, std::string source) {
    if (module != "library" && module != "id3" && module != "playlist_file" && module != "playlists" && module != "main") {
        return source;
    }
    // The player ships with Windows line endings. The replacements below
    // match the linefeed form.
    {
        std::string normalized;
        normalized.reserve(source.size());
        for (char ch : source) {
            if (ch != '\r') {
                normalized.push_back(ch);
            }
        }
        source.swap(normalized);
    }
    if (module == "id3") {
        const std::string old_break =
            "\t\tif frameSize <= 0 or pos + headerLen + frameSize - 1 > tagEnd then\n"
            "\t\t\tbreak\n"
            "\t\tend\n";
        const std::string new_break =
            "\t\tif frameSize <= 0 or pos + headerLen + frameSize - 1 > tagEnd then\n"
            "\t\t\tif frameId == \"APIC\" or frameId == \"PIC\" then\n"
            "\t\t\t\ttags.artPending = true\n"
            "\t\t\tend\n"
            "\t\t\tbreak\n"
            "\t\tend\n";
        const size_t break_at = source.find(old_break);
        if (break_at == std::string::npos) {
            if (source.find("frameSize <= 0") != std::string::npos) {
                log_event(L"warn", L"lua", L"The tag reader did not keep a picture frame that continues past the first read, so that album stays without a cover.");
            }
        } else {
            source.replace(break_at, old_break.size(), new_break);
        }
        return source;
    }
    if (module == "playlist_file") {
        const std::string old_readers =
            "function playlist_file.supported(path)\n"
            "\tlocal ext = extension_of(path)\n"
            "\treturn ext == \"m3u\" or ext == \"m3u8\" or ext == \"pls\"\n"
            "end\n"
            "\n"
            "-- { name = string, paths = { string, ... } } or nil when the file cannot be read.\n"
            "function playlist_file.read(path)\n"
            "\tlocal readOk, content = pcall(utils.read_file, path)\n"
            "\tif not readOk or type(content) ~= \"string\" or content == \"\" then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\tlocal ext = extension_of(path)\n"
            "\tlocal text = decode_text(content, ext)\n"
            "\tlocal title, paths\n"
            "\tif ext == \"pls\" then\n"
            "\t\ttitle, paths = parse_pls(text)\n"
            "\telse\n"
            "\t\ttitle, paths = parse_m3u(text)\n"
            "\tend\n"
            "\tif not title or title == \"\" then\n"
            "\t\ttitle = basename_no_ext(path)\n"
            "\tend\n"
            "\treturn { name = title, paths = paths or {} }\n"
            "end\n";
        const std::string new_readers =
            "local PLAYLIST_EXT = {\n"
            "\tm3u = true, m3u8 = true, pls = true, xspf = true, jspf = true,\n"
            "\twpl = true, zpl = true, asx = true, wax = true, wvx = true,\n"
            "\tb4s = true, mpcpl = true, aimppl = true, aimppl4 = true,\n"
            "\txml = true, plist = true,\n"
            "}\n"
            "\n"
            "local function xml_unescape(value)\n"
            "\tvalue = value:gsub(\"&lt;\", \"<\"):gsub(\"&gt;\", \">\"):gsub(\"&quot;\", '\"'):gsub(\"&apos;\", \"'\")\n"
            "\tvalue = value:gsub(\"&#x(%x+);\", function(hex)\n"
            "\t\treturn utf8_encode(tonumber(hex, 16) or 0)\n"
            "\tend)\n"
            "\tvalue = value:gsub(\"&#(%d+);\", function(num)\n"
            "\t\treturn utf8_encode(tonumber(num) or 0)\n"
            "\tend)\n"
            "\treturn (value:gsub(\"&amp;\", \"&\"))\n"
            "end\n"
            "\n"
            "local function xml_attr(text, name)\n"
            "\tlocal out = {}\n"
            "\tlocal i = 1\n"
            "\twhile i <= #text do\n"
            "\t\tlocal startAt, endAt, word = text:find(\"([%w_:%-]+)%s*=%s*\", i)\n"
            "\t\tif not startAt then\n"
            "\t\t\tbreak\n"
            "\t\tend\n"
            "\t\tlocal quote = text:sub(endAt + 1, endAt + 1)\n"
            "\t\tif word:lower() == name and (quote == '\"' or quote == \"'\") then\n"
            "\t\t\tlocal closeAt = text:find(quote, endAt + 2, true)\n"
            "\t\t\tif not closeAt then\n"
            "\t\t\t\tbreak\n"
            "\t\t\tend\n"
            "\t\t\tout[#out + 1] = xml_unescape(text:sub(endAt + 2, closeAt - 1))\n"
            "\t\t\ti = closeAt + 1\n"
            "\t\telse\n"
            "\t\t\ti = endAt + 1\n"
            "\t\tend\n"
            "\tend\n"
            "\treturn out\n"
            "end\n"
            "\n"
            "local function collect_paths(values)\n"
            "\tlocal paths = {}\n"
            "\tfor _, value in ipairs(values) do\n"
            "\t\tlocal path = entry_to_path(value)\n"
            "\t\tif path then\n"
            "\t\t\tpaths[#paths + 1] = path\n"
            "\t\tend\n"
            "\tend\n"
            "\treturn paths\n"
            "end\n"
            "\n"
            "local function single(name, paths)\n"
            "\treturn { { name = name, paths = paths or {} } }\n"
            "end\n"
            "\n"
            "local function parse_xspf(text)\n"
            "\tif not text:find(\"<location\", 1, true) and not text:find(\"<trackList\", 1, true) then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\tlocal title = text:match(\"<title[^>]*>(.-)</title>\")\n"
            "\tif title then\n"
            "\t\ttitle = xml_unescape(title)\n"
            "\tend\n"
            "\tlocal values = {}\n"
            "\tfor loc in text:gmatch(\"<location[^>]*>(.-)</location>\") do\n"
            "\t\tvalues[#values + 1] = xml_unescape(loc)\n"
            "\tend\n"
            "\treturn single(title, collect_paths(values))\n"
            "end\n"
            "\n"
            "local function parse_jspf(text)\n"
            "\tif not text:find(\"\\\"location\\\"\", 1, true) then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\tlocal title = text:match('\"title\"%s*:%s*\"(.-)\"')\n"
            "\tlocal values = {}\n"
            "\tfor loc in text:gmatch('\"location\"%s*:%s*\"(.-)\"') do\n"
            "\t\tvalues[#values + 1] = loc:gsub(\"\\\\/\", \"/\")\n"
            "\tend\n"
            "\treturn single(title, collect_paths(values))\n"
            "end\n"
            "\n"
            "local function parse_asx(text)\n"
            "\tif not text:lower():find(\"<asx\", 1, true) and not text:lower():find(\"<ref\", 1, true) then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\tlocal title = text:match(\"<title[^>]*>(.-)</title>\")\n"
            "\tif title then\n"
            "\t\ttitle = xml_unescape(title)\n"
            "\tend\n"
            "\treturn single(title, collect_paths(xml_attr(text, \"href\")))\n"
            "end\n"
            "\n"
            "local function parse_wpl(text)\n"
            "\tlocal lower = text:lower()\n"
            "\tif not lower:find(\"<smil\", 1, true) and not lower:find(\"<media\", 1, true) and not lower:find(\"<?wpl\", 1, true) then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\tlocal title = text:match(\"<title[^>]*>(.-)</title>\")\n"
            "\tif title then\n"
            "\t\ttitle = xml_unescape(title)\n"
            "\tend\n"
            "\treturn single(title, collect_paths(xml_attr(text, \"src\")))\n"
            "end\n"
            "\n"
            "local function parse_b4s(text)\n"
            "\tif not text:find(\"Playstring\", 1, true) and not text:lower():find(\"<playlist\", 1, true) then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\tlocal title = text:match(\"<title[^>]*>(.-)</title>\")\n"
            "\tif title then\n"
            "\t\ttitle = xml_unescape(title)\n"
            "\tend\n"
            "\treturn single(title, collect_paths(xml_attr(text, \"playstring\")))\n"
            "end\n"
            "\n"
            "local function parse_mpcpl(text)\n"
            "\tif not text:find(\"filename,\", 1, true) and not text:find(\"MPCPLAYLIST\", 1, true) then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\tlocal paths = {}\n"
            "\tfor _, line in ipairs(read_lines(text)) do\n"
            "\t\tlocal value = line:match(\"^%d+,[Ff][Ii][Ll][Ee][Nn][Aa][Mm][Ee],(.*)$\")\n"
            "\t\tlocal path = value and entry_to_path(value)\n"
            "\t\tif path then\n"
            "\t\t\tpaths[#paths + 1] = path\n"
            "\t\tend\n"
            "\tend\n"
            "\treturn single(nil, paths)\n"
            "end\n"
            "\n"
            "local function parse_aimp(text)\n"
            "\tif not text:find(\"AIMPPL\", 1, true) and not text:find(\"[Ff]ile[Nn]ame=\") then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\tlocal title = text:match(\"[Nn]ame=([^\\r\\n]+)\") or text:match(\"[Pp]laylist[Nn]ame=([^\\r\\n]+)\")\n"
            "\tlocal paths = {}\n"
            "\tfor _, line in ipairs(read_lines(text)) do\n"
            "\t\tlocal trimmed = line:gsub(\"^%s+\", \"\"):gsub(\"%s+$\", \"\")\n"
            "\t\tif trimmed ~= \"\" and trimmed:sub(1, 1) ~= \"#\" then\n"
            "\t\t\tlocal value = trimmed:match(\"^[Ff]ile[Nn]ame=(.*)$\") or trimmed:match(\"^[Pp]ath=(.*)$\")\n"
            "\t\t\tif not value and not trimmed:find(\"^[%w_]+%s*=\") then\n"
            "\t\t\t\tvalue = trimmed\n"
            "\t\t\tend\n"
            "\t\t\tlocal path = value and entry_to_path(value)\n"
            "\t\t\tif path then\n"
            "\t\t\t\tpaths[#paths + 1] = path\n"
            "\t\t\tend\n"
            "\t\tend\n"
            "\tend\n"
            "\treturn single(title, paths)\n"
            "end\n"
            "\n"
            "local function matching_close(text, openTag, closeTag, from)\n"
            "\tlocal depth = 1\n"
            "\tlocal j = from\n"
            "\twhile depth > 0 do\n"
            "\t\tlocal openAt = text:find(openTag, j, true)\n"
            "\t\tlocal closeAt = text:find(closeTag, j, true)\n"
            "\t\tif not closeAt then\n"
            "\t\t\treturn nil\n"
            "\t\tend\n"
            "\t\tif openAt and openAt < closeAt then\n"
            "\t\t\tdepth = depth + 1\n"
            "\t\t\tj = openAt + #openTag\n"
            "\t\telse\n"
            "\t\t\tdepth = depth - 1\n"
            "\t\t\tif depth == 0 then\n"
            "\t\t\t\treturn closeAt\n"
            "\t\t\tend\n"
            "\t\t\tj = closeAt + #closeTag\n"
            "\t\tend\n"
            "\tend\n"
            "\treturn nil\n"
            "end\n"
            "\n"
            "local function top_level_dicts(section)\n"
            "\tlocal out = {}\n"
            "\tlocal i = 1\n"
            "\twhile i <= #section do\n"
            "\t\tlocal startAt = section:find(\"<dict>\", i, true)\n"
            "\t\tif not startAt then\n"
            "\t\t\tbreak\n"
            "\t\tend\n"
            "\t\tlocal closeAt = matching_close(section, \"<dict>\", \"</dict>\", startAt + 6)\n"
            "\t\tif not closeAt then\n"
            "\t\t\tbreak\n"
            "\t\tend\n"
            "\t\tout[#out + 1] = section:sub(startAt + 6, closeAt - 1)\n"
            "\t\ti = closeAt + 7\n"
            "\tend\n"
            "\treturn out\n"
            "end\n"
            "\n"
            "local function plist_true(body, key)\n"
            "\treturn body:find(\"<key>\" .. key .. \"</key>%s*<true%s*/>\") ~= nil\n"
            "\t\tor body:find(\"<key>\" .. key .. \"</key>%s*<true>%s*</true>\") ~= nil\n"
            "end\n"
            "\n"
            "local function parse_itunes(text)\n"
            "\tlocal listsAt = text:find(\"<key>Playlists</key>\", 1, true)\n"
            "\tif not listsAt then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\tlocal tracks = {}\n"
            "\tlocal tracksAt = text:find(\"<key>Tracks</key>\", 1, true)\n"
            "\tif tracksAt and tracksAt < listsAt then\n"
            "\t\tlocal body = text:sub(tracksAt, listsAt - 1)\n"
            "\t\tfor id, dict in body:gmatch(\"<key>(%d+)</key>%s*<dict>(.-)</dict>\") do\n"
            "\t\t\tlocal loc = dict:match(\"<key>Location</key>%s*<string>(.-)</string>\")\n"
            "\t\t\tif loc then\n"
            "\t\t\t\ttracks[id] = xml_unescape(loc)\n"
            "\t\t\tend\n"
            "\t\tend\n"
            "\tend\n"
            "\tlocal arrayAt = text:find(\"<array>\", listsAt, true)\n"
            "\tif not arrayAt then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\tlocal arrayEnd = matching_close(text, \"<array>\", \"</array>\", arrayAt + 7)\n"
            "\tif not arrayEnd then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\tlocal found = {}\n"
            "\tfor _, body in ipairs(top_level_dicts(text:sub(arrayAt + 7, arrayEnd - 1))) do\n"
            "\t\tif not plist_true(body, \"Master\") and not plist_true(body, \"Folder\") then\n"
            "\t\t\tlocal name = body:match(\"<key>Name</key>%s*<string>(.-)</string>\")\n"
            "\t\t\tif name then\n"
            "\t\t\t\tname = xml_unescape(name)\n"
            "\t\t\tend\n"
            "\t\t\tlocal key = body:match(\"<key>Playlist Persistent ID</key>%s*<string>(.-)</string>\") or name\n"
            "\t\t\tlocal paths = {}\n"
            "\t\t\tfor id in body:gmatch(\"<key>Track ID</key>%s*<integer>(%d+)</integer>\") do\n"
            "\t\t\t\tlocal loc = tracks[id]\n"
            "\t\t\t\tlocal path = loc and entry_to_path(loc)\n"
            "\t\t\t\tif path then\n"
            "\t\t\t\t\tpaths[#paths + 1] = path\n"
            "\t\t\tend\n"
            "\t\t\tend\n"
            "\t\t\tif #paths == 0 then\n"
            "\t\t\t\tfor loc in body:gmatch(\"<key>Location</key>%s*<string>(.-)</string>\") do\n"
            "\t\t\t\t\tlocal path = entry_to_path(xml_unescape(loc))\n"
            "\t\t\t\t\tif path then\n"
            "\t\t\t\t\t\tpaths[#paths + 1] = path\n"
            "\t\t\t\t\tend\n"
            "\t\t\t\tend\n"
            "\t\t\tend\n"
            "\t\t\tif #paths > 0 then\n"
            "\t\t\t\tfound[#found + 1] = { name = name, paths = paths, key = key }\n"
            "\t\t\tend\n"
            "\t\tend\n"
            "\tend\n"
            "\treturn found\n"
            "end\n"
            "\n"
            "function playlist_file.supported(path)\n"
            "\treturn PLAYLIST_EXT[extension_of(path)] == true\n"
            "end\n"
            "\n"
            "function playlist_file.read_all(path)\n"
            "\tlocal readOk, content = pcall(utils.read_file, path)\n"
            "\tif not readOk or type(content) ~= \"string\" or content == \"\" then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\tlocal ext = extension_of(path)\n"
            "\tlocal text = decode_text(content, ext)\n"
            "\tlocal lists\n"
            "\tif ext == \"pls\" then\n"
            "\t\tlocal title, paths = parse_pls(text)\n"
            "\t\tlists = single(title, paths)\n"
            "\telseif ext == \"m3u\" or ext == \"m3u8\" then\n"
            "\t\tlocal title, paths = parse_m3u(text)\n"
            "\t\tlists = single(title, paths)\n"
            "\telseif ext == \"jspf\" then\n"
            "\t\tlists = parse_jspf(text)\n"
            "\telseif ext == \"xspf\" then\n"
            "\t\tlists = parse_xspf(text)\n"
            "\telseif ext == \"mpcpl\" then\n"
            "\t\tlists = parse_mpcpl(text)\n"
            "\telseif ext == \"aimppl\" or ext == \"aimppl4\" then\n"
            "\t\tlists = parse_aimp(text)\n"
            "\telseif ext == \"wpl\" or ext == \"zpl\" then\n"
            "\t\tlists = parse_wpl(text)\n"
            "\telseif ext == \"asx\" or ext == \"wax\" or ext == \"wvx\" then\n"
            "\t\tlists = parse_asx(text)\n"
            "\telseif ext == \"b4s\" then\n"
            "\t\tlists = parse_b4s(text)\n"
            "\telse\n"
            "\t\tlists = parse_itunes(text) or parse_xspf(text) or parse_asx(text) or parse_wpl(text) or parse_b4s(text)\n"
            "\tend\n"
            "\tif not lists then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\tfor _, item in ipairs(lists) do\n"
            "\t\tif not item.name or item.name == \"\" then\n"
            "\t\t\titem.name = basename_no_ext(path)\n"
            "\t\tend\n"
            "\tend\n"
            "\treturn lists\n"
            "end\n"
            "\n"
            "-- { name = string, paths = { string, ... } } or nil when the file cannot be read.\n"
            "function playlist_file.read(path)\n"
            "\tlocal lists = playlist_file.read_all(path)\n"
            "\tif not lists or not lists[1] then\n"
            "\t\treturn nil\n"
            "\tend\n"
            "\treturn lists[1]\n"
            "end\n";
        const size_t readers_at = source.find(old_readers);
        if (readers_at == std::string::npos) {
            if (source.find("playlist_file.supported") != std::string::npos) {
                log_event(L"warn", L"lua", L"The playlist reader still only accepts M3U and PLS, so iTunes and the other standard playlist files stay hidden.");
            }
        } else {
            source.replace(readers_at, old_readers.size(), new_readers);
        }
        return source;
    }
    if (module == "playlists") {
        const std::string old_sync =
            "\t\t\tlocal parsed = playlist_file.read(path)\n"
            "\t\t\tlocal trackIds = {}\n"
            "\t\t\tif parsed then\n"
            "\t\t\t\tlocal seenTrack = {}\n"
            "\t\t\t\tfor _, entry in ipairs(parsed.paths) do\n"
            "\t\t\t\t\tlocal resolved = resolve_entry(path, entry)\n"
            "\t\t\t\t\tlocal id = lookup(path_key(resolved))\n"
            "\t\t\t\t\tif id and not seenTrack[id] then\n"
            "\t\t\t\t\t\tseenTrack[id] = true\n"
            "\t\t\t\t\t\ttrackIds[#trackIds + 1] = id\n"
            "\t\t\t\t\tend\n"
            "\t\t\t\tend\n"
            "\t\t\tend\n"
            "\t\t\tlocal id = file_playlist_id(path)\n"
            "\t\t\tseen[id] = true\n"
            "\t\t\t-- Keep the playlist when the file names local tracks, even if none\n"
            "\t\t\t-- of them are in the library yet. A file that is only stream URLs\n"
            "\t\t\t-- is not something this player can queue.\n"
            "\t\t\tif parsed and #parsed.paths > 0 then\n"
            "\t\t\t\tlocal existing = state.byId[id]\n"
            "\t\t\t\tlocal name = parsed.name or \"\"\n"
            "\t\t\t\tif not existing or existing.sourcePath ~= path or existing.name ~= name or not same_ids(existing.trackIds or {}, trackIds) then\n"
            "\t\t\t\t\tstate.byId[id] = {\n"
            "\t\t\t\t\t\tid = id,\n"
            "\t\t\t\t\t\tname = name,\n"
            "\t\t\t\t\t\ttrackIds = trackIds,\n"
            "\t\t\t\t\t\tsourcePath = path,\n"
            "\t\t\t\t\t}\n"
            "\t\t\t\t\tchanged = changed + 1\n"
            "\t\t\t\tend\n"
            "\t\t\telseif state.byId[id] and state.byId[id].sourcePath then\n"
            "\t\t\t\tstate.byId[id] = nil\n"
            "\t\t\t\tchanged = changed + 1\n"
            "\t\t\tend\n";
        const std::string new_sync =
            "\t\t\tlocal lists = playlist_file.read_all and playlist_file.read_all(path) or nil\n"
            "\t\t\tif lists == nil and playlist_file.read then\n"
            "\t\t\t\tlocal parsed = playlist_file.read(path)\n"
            "\t\t\t\tif parsed then\n"
            "\t\t\t\t\tlists = { parsed }\n"
            "\t\t\t\tend\n"
            "\t\t\tend\n"
            "\t\t\tif lists then\n"
            "\t\t\t\tlocal seenHere = {}\n"
            "\t\t\t\tfor _, parsed in ipairs(lists) do\n"
            "\t\t\t\t\tif parsed and parsed.paths and #parsed.paths > 0 then\n"
            "\t\t\t\t\t\tlocal trackIds = {}\n"
            "\t\t\t\t\t\tlocal seenTrack = {}\n"
            "\t\t\t\t\t\tfor _, entry in ipairs(parsed.paths) do\n"
            "\t\t\t\t\t\t\tlocal resolved = resolve_entry(path, entry)\n"
            "\t\t\t\t\t\t\tlocal id = lookup(path_key(resolved))\n"
            "\t\t\t\t\t\t\tif id and not seenTrack[id] then\n"
            "\t\t\t\t\t\t\t\tseenTrack[id] = true\n"
            "\t\t\t\t\t\t\t\ttrackIds[#trackIds + 1] = id\n"
            "\t\t\t\t\t\t\tend\n"
            "\t\t\t\t\t\tend\n"
            "\t\t\t\t\t\tlocal idKey = path\n"
            "\t\t\t\t\t\tif parsed.key and parsed.key ~= \"\" then\n"
            "\t\t\t\t\t\t\tidKey = path .. \"\\0\" .. parsed.key\n"
            "\t\t\t\t\t\tend\n"
            "\t\t\t\t\t\tlocal id = file_playlist_id(idKey)\n"
            "\t\t\t\t\t\tseen[id] = true\n"
            "\t\t\t\t\t\tseenHere[id] = true\n"
            "\t\t\t\t\t\tlocal existing = state.byId[id]\n"
            "\t\t\t\t\t\tlocal name = parsed.name or \"\"\n"
            "\t\t\t\t\t\tif not existing or existing.sourcePath ~= path or existing.name ~= name or not same_ids(existing.trackIds or {}, trackIds) then\n"
            "\t\t\t\t\t\t\tstate.byId[id] = {\n"
            "\t\t\t\t\t\t\t\tid = id,\n"
            "\t\t\t\t\t\t\t\tname = name,\n"
            "\t\t\t\t\t\t\t\ttrackIds = trackIds,\n"
            "\t\t\t\t\t\t\t\tsourcePath = path,\n"
            "\t\t\t\t\t\t\t}\n"
            "\t\t\t\t\t\t\tchanged = changed + 1\n"
            "\t\t\t\t\t\tend\n"
            "\t\t\t\t\tend\n"
            "\t\t\t\tend\n"
            "\t\t\t\tlocal sourceKey = path_key(path)\n"
            "\t\t\t\tfor id, playlist in pairs(state.byId) do\n"
            "\t\t\t\t\tif playlist.sourcePath and path_key(playlist.sourcePath) == sourceKey and not seenHere[id] then\n"
            "\t\t\t\t\t\tstate.byId[id] = nil\n"
            "\t\t\t\t\t\tchanged = changed + 1\n"
            "\t\t\t\tend\n"
            "\t\t\t\tend\n"
            "\t\t\tend\n";
        const size_t sync_at = source.find(old_sync);
        if (sync_at == std::string::npos) {
            if (source.find("playlist_file.read(path)") != std::string::npos) {
                log_event(L"warn", L"lua", L"A playlist file with several lists inside it, such as an iTunes library, still imports only one list.");
            }
        } else {
            source.replace(sync_at, old_sync.size(), new_sync);
        }
        return source;
    }
    if (module == "main") {
        const std::string old_lists =
            "function get_playlists()\n"
            "\treturn json.encode(playlists.get_all())\n"
            "end\n";
        const std::string new_lists =
            "function get_playlists()\n"
            "\treturn json.encode(playlists.get_all())\n"
            "end\n"
            "\n"
            "function get_track_lyrics(id)\n"
            "\tlocal ok, payload = pcall(library.track_lyrics, id)\n"
            "\tif not ok or type(payload) ~= \"table\" then\n"
            "\t\treturn json.encode({ text = \"\" })\n"
            "\tend\n"
            "\treturn json.encode(payload)\n"
            "end\n";
        const size_t lists_at = source.find(old_lists);
        if (lists_at == std::string::npos) {
            if (source.find("function get_playlists") != std::string::npos) {
                log_event(L"warn", L"lua", L"Lyrics cannot be requested, so the Now Playing page has nothing to scroll.");
            }
        } else {
            source.replace(lists_at, old_lists.size(), new_lists);
        }
        return source;
    }
    const std::string old_branch =
        "\t\telse\n"
        "\t\t\tlocal sizeOk, size = pcall(fs.file_size, path)\n"
        "\t\t\tsize = sizeOk and size or -1\n"
        "\t\t\tif existing.size ~= size then\n"
        "\t\t\t\tscanState.pending[#scanState.pending + 1] = { path = path, size = size }\n"
        "\t\t\tend\n";
    const std::string new_branch =
        "\t\telse\n"
        "\t\t\tlocal sizeOk, size = pcall(fs.file_size, path)\n"
        "\t\t\tsize = sizeOk and size or -1\n"
        "\t\t\tlocal album = tostring(existing.album or \"\")\n"
        "\t\t\tlocal artist = tostring(existing.artist or \"\")\n"
        "\t\t\tlocal unread = existing.size == nil or tonumber(existing.size) == nil or tonumber(existing.size) < 0\n"
        "\t\t\t\tor album == \"\" or album == \"Unknown Album\"\n"
        "\t\t\t\tor artist == \"\" or artist == \"Unknown Artist\"\n"
        "\t\t\tif unread or existing.size ~= size then\n"
        "\t\t\t\tscanState.pending[#scanState.pending + 1] = { path = path, size = size }\n"
        "\t\t\tend\n";
    const size_t at = source.find(old_branch);
    if (at == std::string::npos) {
        if (source.find("scanState.pending") != std::string::npos) {
            log_event(L"warn", L"lua", L"The library scan did not contain the size check, so a file saved without tags would stay hidden.");
        }
    } else {
        source.replace(at, old_branch.size(), new_branch);
    }
    const std::string old_album_safe =
        "function library.album_art_safe(track)\n"
        "\tif not track then\n"
        "\t\treturn \"unknown\"\n"
        "\tend\n"
        "\tlocal artist = track.albumArtist\n"
        "\tif not artist or artist == \"\" then\n"
        "\t\tartist = track.artist or \"Unknown Artist\"\n"
        "\tend\n"
        "\tlocal album = track.album or \"Unknown Album\"\n"
        "\tlocal label = string.lower(tostring(artist)) .. \"\\t\" .. string.lower(tostring(album))\n"
        "\tlocal safe = label:gsub(\"[^%w]+\", \"_\"):gsub(\"^_+\", \"\"):gsub(\"_+$\", \"\")\n"
        "\tif safe == \"\" then\n"
        "\t\tsafe = \"unknown\"\n"
        "\tend\n"
        "\tif #safe > 80 then\n"
        "\t\tsafe = safe:sub(1, 80)\n"
        "\tend\n"
        "\treturn safe\n"
        "end\n";
    const std::string new_album_safe =
        "function library.album_art_safe(track)\n"
        "\tif not track then\n"
        "\t\treturn \"unknown\"\n"
        "\tend\n"
        "\tlocal artist = track.albumArtist\n"
        "\tif not artist or artist == \"\" then\n"
        "\t\tartist = track.artist or \"Unknown Artist\"\n"
        "\tend\n"
        "\tlocal album = track.album or \"Unknown Album\"\n"
        "\tlocal label = string.lower(tostring(artist)) .. \"\\t\" .. string.lower(tostring(album))\n"
        "\tlocal safe = label:gsub(\"[^%w]+\", \"_\"):gsub(\"^_+\", \"\"):gsub(\"_+$\", \"\")\n"
        "\tif safe == \"\" then\n"
        "\t\tsafe = \"unknown\"\n"
        "\tend\n"
        "\tif #safe > 80 then\n"
        "\t\tsafe = safe:sub(1, 80)\n"
        "\tend\n"
        "\tif not tostring(album):find(\"[A-Za-z0-9]\") then\n"
        "\t\tlocal hashLabel = tostring(artist):gsub(\"[A-Z]\", function(c)\n"
        "\t\t\treturn string.char(string.byte(c) + 32)\n"
        "\t\tend) .. \"\\t\" .. tostring(album):gsub(\"[A-Z]\", function(c)\n"
        "\t\t\treturn string.char(string.byte(c) + 32)\n"
        "\t\tend)\n"
        "\t\tlocal h = 2166136261\n"
        "\t\tfor i = 1, #hashLabel do\n"
        "\t\t\tlocal b = string.byte(hashLabel, i)\n"
        "\t\t\th = (h + b) % 4294967296\n"
        "\t\t\th = (h * 16777619) % 4294967296\n"
        "\t\tend\n"
        "\t\tlocal hex = string.format(\"%08x\", h)\n"
        "\t\tif #safe > 71 then\n"
        "\t\t\tsafe = safe:sub(1, 71)\n"
        "\t\tend\n"
        "\t\tsafe = safe .. \"_\" .. hex\n"
        "\tend\n"
        "\treturn safe\n"
        "end\n";
    if (source.find("function library.album_art_safe") != std::string::npos) {
        const size_t album_at = source.find(old_album_safe);
        if (album_at == std::string::npos) {
            log_event(L"warn", L"lua", L"The album cover name did not match, so albums with no English letters still share one picture.");
        } else {
            source.replace(album_at, old_album_safe.size(), new_album_safe);
        }
    }
    const std::string old_lyrics =
        "function library.get_track_art(id)\n"
        "\treturn nil\n"
        "end\n";
    const std::string new_lyrics =
        "local lyricCache = {}\n"
        "local lyricCacheOrder = {}\n"
        "\n"
        "local function lyric_utf8(cp)\n"
        "\tif cp <= 0 or cp > 0x10FFFF or (cp >= 0xD800 and cp <= 0xDFFF) then\n"
        "\t\treturn \"\"\n"
        "\tend\n"
        "\tlocal ok, ch = pcall(utf8.char, cp)\n"
        "\tif ok then\n"
        "\t\treturn ch\n"
        "\tend\n"
        "\treturn \"\"\n"
        "end\n"
        "\n"
        "local function lyric_utf16(body, startAt, big)\n"
        "\tlocal out = {}\n"
        "\tlocal i = startAt\n"
        "\twhile i + 1 <= #body do\n"
        "\t\tlocal hi, lo\n"
        "\t\tif big then\n"
        "\t\t\thi, lo = body:byte(i), body:byte(i + 1)\n"
        "\t\telse\n"
        "\t\t\tlo, hi = body:byte(i), body:byte(i + 1)\n"
        "\t\tend\n"
        "\t\tlocal cp = hi * 256 + lo\n"
        "\t\ti = i + 2\n"
        "\t\tif cp >= 0xD800 and cp <= 0xDBFF and i + 1 <= #body then\n"
        "\t\t\tlocal hi2, lo2\n"
        "\t\t\tif big then\n"
        "\t\t\t\thi2, lo2 = body:byte(i), body:byte(i + 1)\n"
        "\t\t\telse\n"
        "\t\t\t\tlo2, hi2 = body:byte(i), body:byte(i + 1)\n"
        "\t\t\tend\n"
        "\t\t\tlocal low = hi2 * 256 + lo2\n"
        "\t\t\tif low >= 0xDC00 and low <= 0xDFFF then\n"
        "\t\t\t\tcp = 0x10000 + (cp - 0xD800) * 0x400 + (low - 0xDC00)\n"
        "\t\t\t\ti = i + 2\n"
        "\t\t\tend\n"
        "\t\tend\n"
        "\t\tif cp ~= 0 and cp ~= 0xFEFF then\n"
        "\t\t\tout[#out + 1] = lyric_utf8(cp)\n"
        "\t\tend\n"
        "\tend\n"
        "\treturn table.concat(out)\n"
        "end\n"
        "\n"
        "local function lyric_bytes(body, enc)\n"
        "\tif enc == 1 or enc == 2 then\n"
        "\t\tlocal startAt = 1\n"
        "\t\tlocal big = enc == 2\n"
        "\t\tif enc == 1 and #body >= 2 then\n"
        "\t\t\tlocal b1, b2 = body:byte(1), body:byte(2)\n"
        "\t\t\tif b1 == 0xFF and b2 == 0xFE then\n"
        "\t\t\t\tbig = false\n"
        "\t\t\t\tstartAt = 3\n"
        "\t\t\telseif b1 == 0xFE and b2 == 0xFF then\n"
        "\t\t\t\tbig = true\n"
        "\t\t\t\tstartAt = 3\n"
        "\t\t\tend\n"
        "\t\tend\n"
        "\t\treturn lyric_utf16(body, startAt, big)\n"
        "\tend\n"
        "\treturn body:gsub(\"%z\", \"\")\n"
        "end\n"
        "\n"
        "local function skip_descriptor(payload, enc, from)\n"
        "\tlocal i = from\n"
        "\tif enc == 1 or enc == 2 then\n"
        "\t\twhile i + 1 <= #payload do\n"
        "\t\t\tif payload:byte(i) == 0 and payload:byte(i + 1) == 0 then\n"
        "\t\t\t\treturn i + 2\n"
        "\t\t\tend\n"
        "\t\t\ti = i + 2\n"
        "\t\tend\n"
        "\t\treturn #payload + 1\n"
        "\tend\n"
        "\twhile i <= #payload and payload:byte(i) ~= 0 do\n"
        "\t\ti = i + 1\n"
        "\tend\n"
        "\treturn i + 1\n"
        "end\n"
        "\n"
        "local function uslt_text(payload)\n"
        "\tif #payload < 5 then\n"
        "\t\treturn \"\"\n"
        "\tend\n"
        "\tlocal enc = payload:byte(1)\n"
        "\tlocal textAt = skip_descriptor(payload, enc, 5)\n"
        "\treturn lyric_bytes(payload:sub(textAt), enc)\n"
        "end\n"
        "\n"
        "local function sylt_text(payload)\n"
        "\tif #payload < 7 then\n"
        "\t\treturn \"\"\n"
        "\tend\n"
        "\tlocal enc = payload:byte(1)\n"
        "\tlocal format = payload:byte(5)\n"
        "\tlocal textAt = skip_descriptor(payload, enc, 7)\n"
        "\tlocal lines = {}\n"
        "\tlocal i = textAt\n"
        "\twhile i + 4 <= #payload do\n"
        "\t\tlocal chunk\n"
        "\t\tif enc == 1 or enc == 2 then\n"
        "\t\t\tlocal startAt = i\n"
        "\t\t\twhile i + 1 <= #payload and not (payload:byte(i) == 0 and payload:byte(i + 1) == 0) do\n"
        "\t\t\t\ti = i + 2\n"
        "\t\t\tend\n"
        "\t\t\tchunk = lyric_bytes(payload:sub(startAt, i - 1), enc)\n"
        "\t\t\ti = i + 2\n"
        "\t\telse\n"
        "\t\t\tlocal startAt = i\n"
        "\t\t\twhile i <= #payload and payload:byte(i) ~= 0 do\n"
        "\t\t\t\ti = i + 1\n"
        "\t\t\tend\n"
        "\t\t\tchunk = payload:sub(startAt, i - 1)\n"
        "\t\t\ti = i + 1\n"
        "\t\tend\n"
        "\t\tif i + 3 > #payload then\n"
        "\t\t\tbreak\n"
        "\t\tend\n"
        "\t\tlocal stamp = payload:byte(i) * 16777216 + payload:byte(i + 1) * 65536 + payload:byte(i + 2) * 256 + payload:byte(i + 3)\n"
        "\t\ti = i + 4\n"
        "\t\tlocal seconds = format == 2 and (stamp / 1000) or stamp\n"
        "\t\tlocal mins = math.floor(seconds / 60)\n"
        "\t\tlocal secs = seconds - mins * 60\n"
        "\t\tif chunk and chunk ~= \"\" then\n"
        "\t\t\tlines[#lines + 1] = string.format(\"[%02d:%05.2f]%s\", mins, secs, chunk)\n"
        "\t\tend\n"
        "\tend\n"
        "\treturn table.concat(lines, \"\\n\")\n"
        "end\n"
        "\n"
        "local function read_id3_lyrics(path)\n"
        "\tlocal file = native_open(path, \"rb\")\n"
        "\tif not file then\n"
        "\t\treturn \"\"\n"
        "\tend\n"
        "\tlocal header = file:read(10) or \"\"\n"
        "\tif header:sub(1, 3) ~= \"ID3\" or #header < 10 then\n"
        "\t\tfile:close()\n"
        "\t\treturn \"\"\n"
        "\tend\n"
        "\tlocal ver = header:byte(4)\n"
        "\tlocal tagSize = 0\n"
        "\tlocal s\n"
        "\tfor s = 7, 10 do\n"
        "\t\ttagSize = tagSize * 128 + (header:byte(s) % 128)\n"
        "\tend\n"
        "\tlocal limit = math.min(10 + tagSize, 15 * 1024 * 1024)\n"
        "\tlocal pos = 10\n"
        "\tlocal found = \"\"\n"
        "\twhile pos + 6 < limit do\n"
        "\t\tfile:seek(\"set\", pos)\n"
        "\t\tlocal frameHead = file:read(ver == 2 and 6 or 10) or \"\"\n"
        "\t\tif #frameHead < (ver == 2 and 6 or 10) then\n"
        "\t\t\tbreak\n"
        "\t\tend\n"
        "\t\tlocal id, frameSize, headerLen\n"
        "\t\tif ver == 2 then\n"
        "\t\t\tid = frameHead:sub(1, 3)\n"
        "\t\t\theaderLen = 6\n"
        "\t\t\tframeSize = frameHead:byte(4) * 65536 + frameHead:byte(5) * 256 + frameHead:byte(6)\n"
        "\t\telseif ver >= 4 then\n"
        "\t\t\tid = frameHead:sub(1, 4)\n"
        "\t\t\theaderLen = 10\n"
        "\t\t\tframeSize = 0\n"
        "\t\t\tfor s = 5, 8 do\n"
        "\t\t\t\tframeSize = frameSize * 128 + (frameHead:byte(s) % 128)\n"
        "\t\t\tend\n"
        "\t\telse\n"
        "\t\t\tid = frameHead:sub(1, 4)\n"
        "\t\t\theaderLen = 10\n"
        "\t\t\tframeSize = frameHead:byte(5) * 16777216 + frameHead:byte(6) * 65536 + frameHead:byte(7) * 256 + frameHead:byte(8)\n"
        "\t\tend\n"
        "\t\tif id:sub(1, 1) == \"\\0\" or frameSize <= 0 or pos + headerLen + frameSize > limit + 16 then\n"
        "\t\t\tbreak\n"
        "\t\tend\n"
            "\t\tif id == \"USLT\" or id == \"ULT\" or id == \"SYLT\" or id == \"SLT\" then\n"
            "\t\t\tlocal payload = file:read(math.min(frameSize, 512 * 1024)) or \"\"\n"
            "\t\t\tif id == \"SYLT\" or id == \"SLT\" then\n"
            "\t\t\t\tlocal timed = sylt_text(payload)\n"
            "\t\t\t\tif timed ~= \"\" then\n"
            "\t\t\t\t\tfound = timed\n"
            "\t\t\t\tend\n"
            "\t\t\telse\n"
            "\t\t\t\tlocal text = uslt_text(payload)\n"
            "\t\t\t\tif text ~= \"\" then\n"
            "\t\t\t\t\tfound = text\n"
            "\t\t\t\tend\n"
            "\t\t\tend\n"
            "\t\tend\n"
            "\t\tif found ~= \"\" then\n"
            "\t\t\tbreak\n"
            "\t\tend\n"
            "\t\tpos = pos + headerLen + frameSize\n"
        "\tend\n"
        "\tfile:close()\n"
        "\treturn found\n"
        "end\n"
        "\n"
        "function library.track_lyrics(id)\n"
        "\tlocal key = tostring(id or \"\")\n"
        "\tif lyricCache[key] then\n"
        "\t\treturn lyricCache[key]\n"
        "\tend\n"
        "\tlocal track = library.get_track(id)\n"
        "\tlocal text = \"\"\n"
        "\tif track and track.path and track.path ~= \"\" then\n"
        "\t\tlocal lrcPath = track.path:gsub(\"%.[^%.\\\\/]+$\", \"\") .. \".lrc\"\n"
        "\t\tlocal lrc = native_open(lrcPath, \"rb\")\n"
        "\t\tif lrc then\n"
        "\t\t\ttext = lrc:read(\"*a\") or \"\"\n"
        "\t\t\tlrc:close()\n"
        "\t\tend\n"
        "\t\tif text == \"\" then\n"
        "\t\t\ttext = read_id3_lyrics(track.path)\n"
        "\t\tend\n"
        "\tend\n"
        "\tlocal payload = { text = text or \"\" }\n"
        "\tlyricCache[key] = payload\n"
        "\tlyricCacheOrder[#lyricCacheOrder + 1] = key\n"
        "\tif #lyricCacheOrder > 32 then\n"
        "\t\tlyricCache[lyricCacheOrder[1]] = nil\n"
        "\t\ttable.remove(lyricCacheOrder, 1)\n"
        "\tend\n"
        "\treturn payload\n"
        "end\n"
        "\n"
        "function library.get_track_art(id)\n"
        "\treturn nil\n"
        "end\n";
    const size_t lyrics_at = source.find(old_lyrics);
    if (lyrics_at == std::string::npos) {
        if (source.find("function library.get_track_art") != std::string::npos) {
            log_event(L"warn", L"lua", L"Lyrics are not read from the song file, so the Now Playing page has nothing to scroll.");
        }
    } else {
        source.replace(lyrics_at, old_lyrics.size(), new_lyrics);
    }
    const std::string old_has_art = "hasArt = (tags.artData ~= nil and #tags.artData > 0) or folderArt ~= nil,";
    const std::string new_has_art = "hasArt = (tags.artData ~= nil and #tags.artData > 0) or tags.artPending or folderArt ~= nil,";
    const size_t has_art_at = source.find(old_has_art);
    if (has_art_at == std::string::npos) {
        if (source.find("folderArt ~= nil") != std::string::npos) {
            log_event(L"warn", L"lua", L"A picture frame larger than the first read is still ignored, so that album stays without a cover.");
        }
    } else {
        source.replace(has_art_at, old_has_art.size(), new_has_art);
    }
    const std::string old_head = "local MAX_HEAD_READ_BYTES = 8 * 1024 * 1024";
    const std::string new_head = "local MAX_HEAD_READ_BYTES = 15 * 1024 * 1024";
    const size_t head_at = source.find(old_head);
    if (head_at == std::string::npos) {
        if (source.find("MAX_HEAD_READ_BYTES") != std::string::npos) {
            log_event(L"warn", L"lua", L"The cover read was not raised to 15 MB, so a picture just over 8 MB still stays out of the first read.");
        }
    } else {
        source.replace(head_at, old_head.size(), new_head);
    }
    const std::string old_playlist_note =
        "\tif key ~= \"\" then\n"
        "\t\tdiscoveredPlaylists[key] = path\n"
        "\tend\n"
        "end\n";
    const std::string new_playlist_note =
        "\tif key ~= \"\" then\n"
        "\t\tdiscoveredPlaylists[key] = path\n"
        "\tend\n"
        "\treturn true\n"
        "end\n";
    const size_t note_at = source.find(old_playlist_note);
    if (note_at == std::string::npos) {
        if (source.find("discoveredPlaylists[key]") != std::string::npos) {
            log_event(L"warn", L"lua", L"The library walk cannot tell a playlist file from a folder, so XSPF and iTunes files may be skipped.");
        }
    } else {
        source.replace(note_at, old_playlist_note.size(), new_playlist_note);
    }
    const std::string old_playlist_exts = "\tif ext ~= \"m3u\" and ext ~= \"m3u8\" and ext ~= \"pls\" then\n";
    const std::string new_playlist_exts =
        "\tif ext ~= \"m3u\" and ext ~= \"m3u8\" and ext ~= \"pls\" and ext ~= \"xspf\" and ext ~= \"jspf\" and ext ~= \"wpl\" and ext ~= \"zpl\" and ext ~= \"asx\" and ext ~= \"wax\" and ext ~= \"wvx\" and ext ~= \"b4s\" and ext ~= \"mpcpl\" and ext ~= \"aimppl\" and ext ~= \"aimppl4\" and ext ~= \"xml\" and ext ~= \"plist\" then\n";
    const size_t playlist_ext_at = source.find(old_playlist_exts);
    if (playlist_ext_at == std::string::npos) {
        if (source.find("note_playlist_file") != std::string::npos) {
            log_event(L"warn", L"lua", L"The library walk still ignores every playlist file except M3U and PLS.");
        }
    } else {
        source.replace(playlist_ext_at, old_playlist_exts.size(), new_playlist_exts);
    }
    const std::string old_walk_ext = "elseif file_extension(path) == \"m3u\" or file_extension(path) == \"m3u8\" or file_extension(path) == \"pls\" then\n";
    const std::string new_walk_ext = "elseif note_playlist_file(path) then\n";
    const std::string old_batch_ext = "elseif ext == \"m3u\" or ext == \"m3u8\" or ext == \"pls\" then\n";
    const std::string new_batch_ext = "elseif note_playlist_file(path) then\n";
    if (source.find(old_walk_ext) == std::string::npos || source.find(old_batch_ext) == std::string::npos) {
        if (source.find("note_playlist_file(path)") != std::string::npos) {
            log_event(L"warn", L"lua", L"One library walk still ignores playlist files other than M3U and PLS.");
        }
    } else {
        source.replace(source.find(old_walk_ext), old_walk_ext.size(), new_walk_ext);
        const size_t batch_at = source.find(old_batch_ext);
        if (batch_at != std::string::npos) {
            source.replace(batch_at, old_batch_ext.size(), new_batch_ext);
        }
    }
    const std::string old_plexts = "$plexts = @{'.m3u'=$true;'.m3u8'=$true;'.pls'=$true}";
    const std::string new_plexts = "$plexts = @{'.m3u'=$true;'.m3u8'=$true;'.pls'=$true;'.xspf'=$true;'.jspf'=$true;'.wpl'=$true;'.zpl'=$true;'.asx'=$true;'.wax'=$true;'.wvx'=$true;'.b4s'=$true;'.mpcpl'=$true;'.aimppl'=$true;'.aimppl4'=$true;'.xml'=$true;'.plist'=$true}";
    const size_t plext_at = source.find(old_plexts);
    if (plext_at == std::string::npos) {
        if (source.find("$plexts") != std::string::npos) {
            log_event(L"warn", L"lua", L"The folder scan still does not record XSPF, WPL, ASX, or iTunes playlist files.");
        }
    } else {
        source.replace(plext_at, old_plexts.size(), new_plexts);
    }
    const std::string old_sweep = "local TAG_SWEEP_VERSION = 3";
    const std::string new_sweep = "local TAG_SWEEP_VERSION = 5";
    const size_t sweep_at = source.find(old_sweep);
    if (sweep_at == std::string::npos) {
        if (source.find("TAG_SWEEP_VERSION") != std::string::npos) {
            log_event(L"warn", L"lua", L"The tag sweep was not moved forward, so a library saved before this cover check would not be read again.");
        }
    } else {
        source.replace(sweep_at, old_sweep.size(), new_sweep);
    }
    const std::string old_cache =
        "\tlocal cache = art_cache_path(track.id)\n"
        "\tif cache and fs.exists(cache) and not artBacklog.cacheQueued[albumSafe] then\n";
    const std::string new_cache =
        "\tlocal cache = art_cache_path(track.id)\n"
        "\t-- A picture saved on an earlier pass is not reused. One image from an\n"
        "\t-- Apple Music artwork cache was written onto hundreds of albums.\n"
        "\tif false and cache and fs.exists(cache) and not artBacklog.cacheQueued[albumSafe] then\n";
    const size_t cache_at = source.find(old_cache);
    if (source.find("art_cache_path(track.id)") != std::string::npos) {
        if (cache_at == std::string::npos) {
            log_event(L"warn", L"lua", L"The saved cover cache is still used, so one Apple Music picture can be painted onto unrelated albums.");
        } else {
            source.replace(cache_at, old_cache.size(), new_cache);
        }
    }
    const std::string old_worker =
        "\tif ok and content and content ~= \"\" then\n"
        "\t\tpcall(utils.write_file, dest, content)\n"
        "\tend\n";
    const std::string new_worker =
        "\tif ok and content and content ~= \"\" then\n"
        "\t\tcontent = content:gsub(\"%$side = Find%-Sidecar %$audioPath %$roots\", \"if ($audioPath -match '(?i)\\\\.musiclibrary\\\\\\\\|\\\\\\\\DoomNet Art\\\\\\\\') { return $null }\\n\\t$side = Find-Sidecar $audioPath $roots\\n\\tif ($side -match '(?i)\\\\.musiclibrary\\\\\\\\|\\\\\\\\DoomNet Art\\\\\\\\') { $side = $null }\", 1)\n"
        "\t\tcontent = content:gsub(\"%$head = Read%-Bytes %$audioPath 16777216\", \"$head = Read-Bytes $audioPath 15728640\\n\\tif ($head -and $head.Length -ge 10 -and [Text.Encoding]::ASCII.GetString($head, 0, 3) -eq 'ID3') {\\n\\t\\t$declared = 10 + (([int]$head[6] -band 0x7f) -shl 21) + (([int]$head[7] -band 0x7f) -shl 14) + (([int]$head[8] -band 0x7f) -shl 7) + ([int]$head[9] -band 0x7f)\\n\\t\\tif ($declared -gt $head.Length -and $declared -le 67108864) {\\n\\t\\t\\t$more = Read-Bytes $audioPath $declared\\n\\t\\t\\tif ($more -and $more.Length -gt $head.Length) { $head = $more }\\n\\t\\t}\\n\\t}\", 1)\n"
        "\t\tpcall(utils.write_file, dest, content)\n"
        "\tend\n";
    const size_t worker_at = source.find(old_worker);
    if (source.find("_art_jpeg_worker.ps1") != std::string::npos || source.find("_art_worker.ps1") != std::string::npos) {
        if (worker_at == std::string::npos) {
            log_event(L"warn", L"lua", L"The artwork worker was not updated, so a picture inside an Apple Music library can still be used as an album cover.");
        } else {
            size_t from = 0;
            while (from < source.size()) {
                const size_t found = source.find(old_worker, from);
                if (found == std::string::npos) {
                    break;
                }
                source.replace(found, old_worker.size(), new_worker);
                from = found + new_worker.size();
            }
        }
    }
    return source;
}

void open_host_fs(lua_State* L) {
    luaL_requiref(L, "fs", luaopen_host_fs, 0);
    lua_pop(L, 1);
}

void install_host_searcher(lua_State* L) {
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "searchers");
    // Keep [1] (package.preload). Replace [2]. Remove the C searchers so
    // require never calls LoadLibrary on a name from Lua.
    lua_pushcfunction(L, host_lua_searcher);
    lua_rawseti(L, -2, 2);
    lua_pushnil(L);
    lua_rawseti(L, -2, 3);
    lua_pushnil(L);
    lua_rawseti(L, -2, 4);
    lua_pop(L, 2);
}

}  // namespace doomnet
