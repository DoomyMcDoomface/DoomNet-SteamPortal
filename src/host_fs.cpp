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

bool windows_says_directory(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool entry_is_directory(const fsn::directory_entry& entry) {
    std::error_code ec;
    if (entry.is_directory(ec)) {
        return true;
    }
    // The directory listing uses this flag to decide whether to keep going.
    return windows_says_directory(entry.path().native());
}

// A junction or a symbolic link. std::filesystem does not report a Windows
// junction as a symlink, and following one back into the music folder makes
// the scan count the same files without end.
bool path_is_folder_link(const fsn::path& path) {
    WIN32_FIND_DATAW data{};
    const HANDLE found = FindFirstFileW(path.c_str(), &data);
    if (found == INVALID_HANDLE_VALUE) {
        return false;
    }
    FindClose(found);
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        return false;
    }
    return data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || data.dwReserved0 == IO_REPARSE_TAG_SYMLINK;
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
    std::wstring wide;
    if (!try_wide_path(L, 1, wide)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    std::error_code ec;
    if (fsn::is_directory(fsn::path(wide), ec) || windows_says_directory(wide)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}
int Lua_is_file(lua_State* L) {
    return boolean_query(L, [](const fsn::path& p, std::error_code& ec) { return fsn::is_regular_file(p, ec); });
}
int Lua_is_symlink(lua_State* L) {
    return boolean_query(L, [](const fsn::path& p, std::error_code& ec) { return fsn::is_symlink(p, ec) || path_is_folder_link(p); });
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
    std::wstring wide;
    if (!try_wide_path(L, 1, wide)) {
        return bad_path_pair(L, 1);
    }
    // FindFirstFile keeps going after one bad name. Stopping at that name
    // hid every song listed after it in the same folder.
    std::wstring pattern = wide;
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
        pattern.push_back(L'\\');
    }
    pattern.push_back(L'*');
    WIN32_FIND_DATAW data{};
    const HANDLE found = FindFirstFileW(pattern.c_str(), &data);
    if (found == INVALID_HANDLE_VALUE) {
        return push_error_pair(L, std::error_code(GetLastError(), std::system_category()));
    }
    lua_newtable(L);
    lua_Integer index = 1;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        const bool is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        // A shortcut back into a folder already listed would count the same
        // songs forever. A normal folder is always listed.
        const bool link = reparse && (data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || data.dwReserved0 == IO_REPARSE_TAG_SYMLINK);
        std::wstring full = wide;
        if (!full.empty() && full.back() != L'\\' && full.back() != L'/') {
            full.push_back(L'\\');
        }
        full += data.cFileName;
        lua_newtable(L);
        const std::string name = wide_to_utf8(data.cFileName);
        lua_pushlstring(L, name.data(), name.size());
        lua_setfield(L, -2, "name");
        const std::string path_text = wide_to_utf8(full);
        lua_pushlstring(L, path_text.data(), path_text.size());
        lua_setfield(L, -2, "path");
        lua_pushboolean(L, is_dir);
        lua_setfield(L, -2, "is_directory");
        lua_pushboolean(L, !is_dir);
        lua_setfield(L, -2, "is_file");
        lua_pushboolean(L, link);
        lua_setfield(L, -2, "is_symlink");
        if (!is_dir) {
            const unsigned long long size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            push_count(L, size);
            lua_setfield(L, -2, "size");
        }
        lua_rawseti(L, -2, index++);
    } while (FindNextFileW(found, &data));
    FindClose(found);
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
        lua_pushboolean(L, entry_is_directory(entry));
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
    {"file_mtime", guarded<&Lua_last_write_time>},
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
    if (module == "settings") {
        std::string normalized;
        normalized.reserve(source.size());
        for (char ch : source) {
            if (ch != '\r') {
                normalized.push_back(ch);
            }
        }
        const std::string old_keys = "\tmediaKeysEnabled = true,\n";
        const std::string new_keys = "\tmediaKeysEnabled = true,\n\tglobalControl = false,\n";
        const size_t keys_at = normalized.find(old_keys);
        if (keys_at == std::string::npos) {
            if (normalized.find("mediaKeysEnabled") != std::string::npos) {
                log_event(L"warn", L"lua", L"The player settings did not list mediaKeysEnabled in the usual place, so Global Control cannot be saved.");
            }
            return source;
        }
        normalized.replace(keys_at, old_keys.size(), new_keys);
        return normalized;
    }
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
            "\t\t\tif playlists._load_hidden then\n"
            "\t\t\t\tplaylists._load_hidden()\n"
            "\t\t\tend\n"
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
            "\t\t\t\t\t\tif playlists._hidden and playlists._hidden[id] then\n"
            "\t\t\t\t\t\t\tif state.byId[id] then\n"
            "\t\t\t\t\t\t\t\tstate.byId[id] = nil\n"
            "\t\t\t\t\t\t\t\tchanged = changed + 1\n"
            "\t\t\t\t\t\t\tend\n"
            "\t\t\t\t\t\telse\n"
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
        const std::string old_delete =
            "function playlists.delete(id)\n"
            "\tlocal playlist = state.byId[id]\n"
            "\tif not playlist or playlist.sourcePath then\n"
            "\t\treturn false\n"
            "\tend\n"
            "\tstate.byId[id] = nil\n"
            "\tpersist()\n"
            "\treturn true\n"
            "end\n";
        const std::string new_delete =
            "function playlists._load_hidden()\n"
            "\tif playlists._hiddenLoaded or not state.dataDir then\n"
            "\t\tplaylists._hidden = playlists._hidden or {}\n"
            "\t\treturn\n"
            "\tend\n"
            "\tplaylists._hiddenLoaded = true\n"
            "\tplaylists._hidden = {}\n"
            "\tlocal hiddenFile = fs.join(state.dataDir, \"playlist_hidden.json\")\n"
            "\tif not fs.exists(hiddenFile) then\n"
            "\t\treturn\n"
            "\tend\n"
            "\tlocal okH, contentH = pcall(utils.read_file, hiddenFile)\n"
            "\tif not okH or type(contentH) ~= \"string\" or contentH == \"\" then\n"
            "\t\treturn\n"
            "\tend\n"
            "\tlocal okD, decodedH = pcall(json.decode, contentH)\n"
            "\tif not okD or type(decodedH) ~= \"table\" then\n"
            "\t\treturn\n"
            "\tend\n"
            "\tfor _, hid in ipairs(decodedH) do\n"
            "\t\tif type(hid) == \"string\" and hid ~= \"\" then\n"
            "\t\t\tplaylists._hidden[hid] = true\n"
            "\t\tend\n"
            "\tend\n"
            "end\n"
            "\n"
            "local function persist_hidden()\n"
            "\tlocal list = {}\n"
            "\tfor hid in pairs(playlists._hidden or {}) do\n"
            "\t\tlist[#list + 1] = hid\n"
            "\tend\n"
            "\tutils.write_file(fs.join(state.dataDir, \"playlist_hidden.json\"), json.encode(list))\n"
            "end\n"
            "\n"
            "function playlists.dismiss(id)\n"
            "\tplaylists._load_hidden()\n"
            "\tlocal key = tostring(id or \"\")\n"
            "\tlocal playlist = state.byId[key]\n"
            "\tif not playlist then\n"
            "\t\tfor storedId, item in pairs(state.byId) do\n"
            "\t\t\tif tostring(storedId) == key then\n"
            "\t\t\t\tplaylist = item\n"
            "\t\t\t\tbreak\n"
            "\t\t\tend\n"
            "\t\tend\n"
            "\tend\n"
            "\tif not playlist or not playlist.sourcePath then\n"
            "\t\treturn false\n"
            "\tend\n"
            "\tplaylists._hidden[tostring(playlist.id)] = true\n"
            "\tstate.byId[playlist.id] = nil\n"
            "\tpersist_hidden()\n"
            "\tpersist()\n"
            "\treturn true\n"
            "end\n"
            "\n"
            "function playlists.delete(id)\n"
            "\tlocal playlist = state.byId[id]\n"
            "\tif not playlist or playlist.sourcePath then\n"
            "\t\treturn false\n"
            "\tend\n"
            "\tstate.byId[id] = nil\n"
            "\tpersist()\n"
            "\treturn true\n"
            "end\n";
        const size_t delete_at = source.find(old_delete);
        if (delete_at == std::string::npos) {
            if (source.find("function playlists.delete") != std::string::npos) {
                log_event(L"warn", L"lua", L"A playlist read from a file cannot be removed from the player, so it comes back on the next scan.");
            }
        } else {
            source.replace(delete_at, old_delete.size(), new_delete);
        }
        return source;
    }
    if (module == "main") {
        const std::string old_sync =
            "local function sync_assets_to_steamui()\n"
            "\tlocal destDir = fs.join(steamui_dir(), INJECT_SUBDIR)\n";
        const std::string new_sync =
            "local function sync_assets_to_steamui()\n"
            "\tlogger:info(\"[SteamMusicPlayer] Left the DoomNet 1.16.0 player in steamui. The original 1.15.0 files were not copied over it.\")\n"
            "\tif true then\n"
            "\t\treturn\n"
            "\tend\n"
            "\tlocal destDir = fs.join(steamui_dir(), INJECT_SUBDIR)\n";
        const size_t sync_at = source.find(old_sync);
        if (sync_at == std::string::npos) {
            if (source.find("sync_assets_to_steamui") != std::string::npos) {
                log_event(L"warn", L"lua", L"The player can still copy version 1.15.0 over the page after DoomNet writes 1.16.0.");
            }
        } else {
            source.replace(sync_at, old_sync.size(), new_sync);
        }
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
            "end\n"
            "\n"
            "function dismiss_playlist(id)\n"
            "\tlocal ok, removed = pcall(playlists.dismiss, id)\n"
            "\tif not ok or not removed then\n"
            "\t\tlogger:warn(\"[SteamMusicPlayer] could not remove playlist \" .. tostring(id) .. \" from the player.\")\n"
            "\t\treturn json.encode({ ok = false })\n"
            "\tend\n"
            "\tlogger:info(\"[SteamMusicPlayer] removed playlist \" .. tostring(id) .. \" from the player. The playlist file was not deleted.\")\n"
            "\treturn json.encode({ ok = true })\n"
            "end\n";
        const size_t lists_at = source.find(old_lists);
        if (lists_at == std::string::npos) {
            if (source.find("function get_playlists") != std::string::npos) {
                log_event(L"warn", L"lua", L"Lyrics cannot be requested, so the Now Playing page has nothing to scroll.");
            }
        } else {
            source.replace(lists_at, old_lists.size(), new_lists);
        }
        const std::string old_rescan_json =
            "\t\tplaylistsUpdated = result.playlistsUpdated or 0,\n"
            "\t})\n"
            "end\n"
            "\n"
            "function scan_library_batch(batchSize)\n";
        const std::string new_rescan_json =
            "\t\tplaylistsUpdated = result.playlistsUpdated or 0,\n"
            "\t\tfolder = result.folder or \"\",\n"
            "\t\tdirsDone = result.dirsDone or 0,\n"
            "\t})\n"
            "end\n"
            "\n"
            "function scan_library_batch(batchSize)\n";
        const size_t rescan_json_at = source.find(old_rescan_json);
        if (rescan_json_at == std::string::npos) {
            log_event(L"warn", L"lua", L"The scan start result does not name the music folder, so the page cannot show which folder is being read.");
        } else {
            source.replace(rescan_json_at, old_rescan_json.size(), new_rescan_json);
        }
        const std::string old_batch_json =
            "\t\tplaylistsUpdated = result.playlistsUpdated or 0,\n"
            "\t\tlastPath = result.lastPath, -- TEMPORARY: crash-diagnosis aid\n";
        const std::string new_batch_json =
            "\t\tplaylistsUpdated = result.playlistsUpdated or 0,\n"
            "\t\tnewOnly = result.newOnly == true,\n"
            "\t\tfolder = result.folder or \"\",\n"
            "\t\tdirsDone = result.dirsDone or 0,\n"
            "\t\tlastPath = result.lastPath, -- TEMPORARY: crash-diagnosis aid\n";
        const size_t batch_json_at = source.find(old_batch_json);
        if (batch_json_at == std::string::npos) {
            log_event(L"warn", L"lua", L"A scan batch does not name the music folder, so the page cannot show which folder is being read.");
        } else {
            source.replace(batch_json_at, old_batch_json.size(), new_batch_json);
        }
        const std::string old_stage =
            "\tif not track or not track.path then\n"
            "\t\tlibrary.play_log(\"stage unknown id=\" .. tostring(trackId))\n"
            "\t\treturn json.encode({ ok = false, error = \"unknown track\" })\n"
            "\tend\n"
            "\tif not mediaState.prepared then\n";
        const std::string new_stage =
            "\tif not track or not track.path then\n"
            "\t\tlibrary.play_log(\"stage unknown id=\" .. tostring(trackId))\n"
            "\t\treturn json.encode({ ok = false, error = \"unknown track\" })\n"
            "\tend\n"
            "\tif type(doomnet_stream_hook) == \"function\" and doomnet_stream_hook() then\n"
            "\t\tlocal sizeOk, actual = pcall(fs.file_size, track.path)\n"
            "\t\tlocal token = media_token()\n"
            "\t\tif not sizeOk or type(actual) ~= \"number\" or actual < 0 or not token then\n"
            "\t\t\tlibrary.play_log(\"stage direct failed id=\" .. tostring(track.id))\n"
            "\t\t\treturn json.encode({ ok = false, error = \"the original file could not be read\" })\n"
            "\t\tend\n"
            "\t\tlibrary.play_log(\"stage direct id=\" .. tostring(track.id) .. \" bytes=\" .. tostring(actual))\n"
            "\t\treturn json.encode({\n"
            "\t\t\tok = true,\n"
            "\t\t\turl = stage_url_base() .. url_encode_segment(tostring(track.id)),\n"
            "\t\t\tsize = actual,\n"
            "\t\t})\n"
            "\tend\n"
            "\tif not mediaState.prepared then\n";
        const size_t stage_at = source.find(old_stage);
        if (stage_at == std::string::npos) {
            log_event(L"warn", L"lua", L"stage_track_for_playback was not patched, so a play will still copy the song into steamui.");
        } else {
            source.replace(stage_at, old_stage.size(), new_stage);
        }
        const std::string old_release =
            "function release_staged_track(trackId)\n"
            "\tlocal track = library.get_track(trackId)\n";
        const std::string new_release =
            "function release_staged_track(trackId)\n"
            "\tif type(doomnet_stream_hook) == \"function\" and doomnet_stream_hook() then\n"
            "\t\treturn json.encode({ ok = true })\n"
            "\tend\n"
            "\tlocal track = library.get_track(trackId)\n";
        const size_t release_at = source.find(old_release);
        if (release_at == std::string::npos) {
            log_event(L"warn", L"lua", L"release_staged_track was not patched, so stopping a song may still delete a copied file.");
        } else {
            source.replace(release_at, old_release.size(), new_release);
        }
        const std::string old_refresh_rpc =
            "function regenerate_artwork()\n"
            "\treturn json.encode(library.regenerate_artwork())\n"
            "end\n";
        const std::string new_refresh_rpc =
            "function regenerate_artwork()\n"
            "\treturn json.encode(library.regenerate_artwork())\n"
            "end\n"
            "\n"
            "function refresh_metadata_start()\n"
            "\tlocal ok, result = pcall(library.refresh_metadata_start, steamui_dir())\n"
            "\tif not ok or type(result) ~= \"table\" then\n"
            "\t\treturn json.encode({ ok = false, running = false, done = true, total = 0, doneCount = 0, updated = 0, missing = 0, unchanged = 0 })\n"
            "\tend\n"
            "\treturn json.encode(result)\n"
            "end\n"
            "\n"
            "function refresh_metadata_batch(batchSize)\n"
            "\tlocal ok, result = pcall(library.refresh_metadata_batch, batchSize)\n"
            "\tif not ok or type(result) ~= \"table\" then\n"
            "\t\treturn json.encode({ ok = false, running = false, done = true, total = 0, doneCount = 0, updated = 0, missing = 0, unchanged = 0 })\n"
            "\tend\n"
            "\treturn json.encode(result)\n"
            "end\n";
        const size_t refresh_rpc_at = source.find(old_refresh_rpc);
        if (refresh_rpc_at == std::string::npos) {
            log_event(L"warn", L"lua", L"Refresh Metadata has no call on the player, so changed tags stay as they were at the last scan.");
        } else {
            source.replace(refresh_rpc_at, old_refresh_rpc.size(), new_refresh_rpc);
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
    const std::string old_list_finish =
        "local function listing_can_finish(age)\n"
        "\tif fs.exists(scan_done_path()) then\n";
    const std::string new_list_finish =
        "local function listing_can_finish(age)\n"
        "\tif walkJob.active then\n"
        "\t\treturn false\n"
        "\tend\n"
        "\tif fs.exists(scan_done_path()) then\n";
    const std::string old_new_only_finish =
        "\t\tlistJob.active = false\n"
        "\t\tresult = listing_status()\n"
        "\t\tresult.listing = false\n"
        "\t\tresult.totalFiles = #unique\n"
        "\telse\n";
    const std::string new_new_only_finish =
        "\t\tlocal updated = sync_discovered_playlists()\n"
        "\t\tscanState.totalFiles = #unique\n"
        "\t\tlistJob.active = false\n"
        "\t\tresult = listing_status()\n"
        "\t\tresult.listing = false\n"
        "\t\tresult.totalFiles = #unique\n"
        "\t\tresult.playlistsUpdated = updated\n"
        "\t\tresult.totalTracks = count_tracks()\n"
        "\telse\n";
    if (source.find("if listJob.newOnly then") != std::string::npos) {
        const size_t new_only_at = source.find(old_new_only_finish);
        if (new_only_at == std::string::npos) {
            log_event(L"warn", L"lua", L"Scan For New Tracks still finishes without importing playlist files.");
        } else {
            source.replace(new_only_at, old_new_only_finish.size(), new_new_only_finish);
        }
    }
    const std::string old_batch_poll =
        "\tif listJob.active then\n"
        "\t\tlocal stillListing = poll_list_job()\n"
        "\t\tlisting = stillListing == true\n"
        "\tend\n";
    const std::string new_batch_poll =
        "\tlocal listed = nil\n"
        "\tif listJob.active then\n"
        "\t\tlocal stillListing, listedResult = poll_list_job()\n"
        "\t\tlisting = stillListing == true\n"
        "\t\tlisted = listedResult\n"
        "\tend\n";
    if (source.find("function library.scan_batch") != std::string::npos) {
        const size_t batch_poll_at = source.find(old_batch_poll);
        if (batch_poll_at == std::string::npos) {
            log_event(L"warn", L"lua", L"A finished search for new songs does not return the playlist files it found.");
        } else {
            source.replace(batch_poll_at, old_batch_poll.size(), new_batch_poll);
        }
    }
    const std::string old_idle_done =
        "\t\treturn {\n"
        "\t\t\tlisting = false,\n"
        "\t\t\tdone = true,\n"
        "\t\t\tprocessed = 0,\n"
        "\t\t\tremaining = 0,\n"
        "\t\t\ttotalFiles = 0,\n"
        "\t\t\ttotalTracks = totalTracks,\n"
        "\t\t\tadded = scanState.addedThisScan or 0,\n"
        "\t\t\tplaylistsUpdated = sync_discovered_playlists(),\n"
        "\t\t}\n";
    const std::string new_idle_done =
        "\t\tif listed then\n"
        "\t\t\tlisted.done = true\n"
        "\t\t\tlisted.listing = false\n"
        "\t\t\tlisted.processed = listed.processed or 0\n"
        "\t\t\tlisted.remaining = 0\n"
        "\t\t\tlisted.totalTracks = totalTracks\n"
        "\t\t\tif listed.playlistsUpdated == nil then\n"
        "\t\t\t\tlisted.playlistsUpdated = sync_discovered_playlists()\n"
        "\t\t\tend\n"
        "\t\t\treturn listed\n"
        "\t\tend\n"
        "\t\treturn {\n"
        "\t\t\tlisting = false,\n"
        "\t\t\tdone = true,\n"
        "\t\t\tprocessed = 0,\n"
        "\t\t\tremaining = 0,\n"
        "\t\t\ttotalFiles = 0,\n"
        "\t\t\ttotalTracks = totalTracks,\n"
        "\t\t\tadded = scanState.addedThisScan or 0,\n"
        "\t\t\tplaylistsUpdated = sync_discovered_playlists(),\n"
        "\t\t}\n";
    if (source.find("function library.scan_batch") != std::string::npos) {
        const size_t idle_done_at = source.find(old_idle_done);
        if (idle_done_at == std::string::npos) {
            log_event(L"warn", L"lua", L"Scan For New Tracks still reports that nothing was found before playlist files are imported.");
        } else {
            source.replace(idle_done_at, old_idle_done.size(), new_idle_done);
        }
    }
    const size_t list_finish_at = source.find(old_list_finish);
    if (list_finish_at == std::string::npos) {
        if (source.find("function listing_can_finish") != std::string::npos && source.find("if walkJob.active then") == std::string::npos) {
            log_event(L"warn", L"lua", L"A slow folder listing is treated as finished after 8 seconds, so a large library can be saved with files still unread.");
        }
    } else {
        source.replace(list_finish_at, old_list_finish.size(), new_list_finish);
    }
    const std::string old_read_tags = "\t\tlocal tags = read_tags_safe(path)\n";
    const std::string new_read_tags =
        "\t\tlocal tagOk, tags = pcall(read_tags_safe, path)\n"
        "\t\tif not tagOk or type(tags) ~= \"table\" then\n"
        "\t\t\tlogger:warn(\"[SteamMusicPlayer] skipped unreadable file \" .. tostring(path))\n"
        "\t\t\ttags = {}\n"
        "\t\tend\n";
    const size_t read_tags_at = source.find(old_read_tags);
    if (read_tags_at == std::string::npos) {
        if (source.find("read_tags_safe(path)") != std::string::npos) {
            log_event(L"warn", L"lua", L"One unreadable song can still stop the scan, so the files after it are left unread.");
        }
    } else {
        source.replace(read_tags_at, old_read_tags.size(), new_read_tags);
    }
    const std::string old_scan_count =
        "\t\tprocessed = processed,\n"
        "\t\tremaining = remaining,\n"
        "\t\ttotalFiles = scanState.totalFiles or (listJob.results and #listJob.results) or 0,\n"
        "\t\ttotalTracks = totalTracks,\n"
        "\t\tpendingCount = remaining,\n";
    const std::string new_scan_count =
        "\t\tprocessed = math.max(0, scanState.pendingIndex - 1),\n"
        "\t\tremaining = remaining,\n"
        "\t\ttotalFiles = scanState.totalFiles or (listJob.results and #listJob.results) or 0,\n"
        "\t\ttotalTracks = totalTracks,\n"
        "\t\tpendingCount = #scanState.pending,\n";
    const size_t scan_count_at = source.find(old_scan_count);
    if (scan_count_at == std::string::npos) {
        if (source.find("pendingCount = remaining") != std::string::npos) {
            log_event(L"warn", L"lua", L"The scan still reports files left instead of files read, so the count stays at 0 while more files are found.");
        }
    } else {
        source.replace(scan_count_at, old_scan_count.size(), new_scan_count);
    }
    const std::string old_folder_art =
        "local function find_folder_art(trackPath, albumName)\n";
    const std::string new_folder_art =
        "local folderArtByDir = {}\n"
        "local function find_folder_art(trackPath, albumName)\n";
    const size_t folder_art_at = source.find(old_folder_art);
    if (folder_art_at == std::string::npos) {
        if (source.find("function find_folder_art") != std::string::npos) {
            log_event(L"warn", L"lua", L"Each song still lists its whole folder, so a large folder keeps the scan sitting on 0.");
        }
    } else {
        source.replace(folder_art_at, old_folder_art.size(), new_folder_art);
    }
    const std::string old_folder_list =
        "\tif not is_library_root(dir) then\n"
        "\t\tlocal okList, entries = pcall(fs.list, dir)\n"
        "\t\tif okList and type(entries) == \"table\" then\n"
        "\t\t\tlocal fallback = nil\n"
        "\t\t\tfor _, entry in ipairs(entries) do\n"
        "\t\t\t\tlocal name = tostring(entry):match(\"[^/\\\\]+$\") or tostring(entry)\n";
    const std::string new_folder_list =
        "\tif not is_library_root(dir) then\n"
        "\t\tlocal cached = folderArtByDir[dir]\n"
        "\t\tif cached ~= nil then\n"
        "\t\t\treturn cached ~= false and cached or nil\n"
        "\t\tend\n"
        "\t\tlocal okList, entries = pcall(fs.list, dir)\n"
        "\t\tif okList and type(entries) == \"table\" then\n"
        "\t\t\tlocal fallback = nil\n"
        "\t\t\tfor _, entry in ipairs(entries) do\n"
        "\t\t\t\tlocal raw = type(entry) == \"table\" and (entry.name or entry.path) or entry\n"
        "\t\t\t\tlocal name = tostring(raw):match(\"[^/\\\\]+$\") or tostring(raw)\n";
    const size_t folder_list_at = source.find(old_folder_list);
    if (folder_list_at == std::string::npos) {
        if (source.find("AlbumArt_") != std::string::npos || source.find("okList, entries") != std::string::npos) {
            log_event(L"warn", L"lua", L"Each song still lists its whole folder, so a large folder keeps the scan sitting on 0.");
        }
    } else {
        source.replace(folder_list_at, old_folder_list.size(), new_folder_list);
    }
    const std::string old_folder_hit =
        "\t\t\t\tif lower:match(\"^albumart.*_large%.jpe?g$\") or lower:match(\"^albumart.*large%.jpe?g$\") then\n"
        "\t\t\t\t\treturn fs.join(dir, name)\n"
        "\t\t\t\tend\n";
    const std::string new_folder_hit =
        "\t\t\t\tif lower:match(\"^albumart.*_large%.jpe?g$\") or lower:match(\"^albumart.*large%.jpe?g$\") then\n"
        "\t\t\t\t\tlocal found = fs.join(dir, name)\n"
        "\t\t\t\t\tfolderArtByDir[dir] = found\n"
        "\t\t\t\t\treturn found\n"
        "\t\t\t\tend\n";
    const size_t folder_hit_at = source.find(old_folder_hit);
    if (folder_hit_at != std::string::npos) {
        source.replace(folder_hit_at, old_folder_hit.size(), new_folder_hit);
    }
    const std::string old_folder_fallback =
        "\t\t\tif fallback then\n"
        "\t\t\t\treturn fallback\n"
        "\t\t\tend\n"
        "\t\tend\n"
        "\tend\n"
        "\treturn nil\n"
        "end\n";
    const std::string new_folder_fallback =
        "\t\t\tif fallback then\n"
        "\t\t\t\tfolderArtByDir[dir] = fallback\n"
        "\t\t\t\treturn fallback\n"
        "\t\t\tend\n"
        "\t\tend\n"
        "\t\tfolderArtByDir[dir] = false\n"
        "\tend\n"
        "\treturn nil\n"
        "end\n";
    const size_t folder_fallback_at = source.find(old_folder_fallback);
    if (folder_fallback_at != std::string::npos) {
        source.replace(folder_fallback_at, old_folder_fallback.size(), new_folder_fallback);
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
    const std::string old_queue =
        "function library.queue_artwork_regenerate(steamUiDir)\n"
        "\tlocal jobsDir = art_jobs_dir()\n";
    const std::string new_queue =
        "function library.queue_artwork_regenerate(steamUiDir)\n"
        "\tif true then\n"
        "\t\tdoomnetArtSteamUi = steamUiDir\n"
        "\t\treturn library.regenerate_artwork()\n"
        "\tend\n"
        "\tlocal jobsDir = art_jobs_dir()\n";
    const size_t queue_at = source.find(old_queue);
    if (source.find("function library.queue_artwork_regenerate") != std::string::npos) {
        if (queue_at == std::string::npos) {
            log_event(L"warn", L"lua", L"Regenerate Artwork still clears the cache and waits for a song instead of reading pictures already in the library.");
        } else {
            source.replace(queue_at, old_queue.size(), new_queue);
        }
    }
    const std::string old_group = "\t\t\t\tgroup = { paths = {}, outs = {} }\n";
    const std::string new_group = "\t\t\t\tgroup = { paths = {}, outs = {}, sample = track }\n";
    const std::string old_art_paths =
        "\t\t\tif track.hasArt then\n"
        "\t\t\t\ttable.insert(group.paths, 1, track.path)\n"
        "\t\t\t\tif #group.paths > 3 then\n"
        "\t\t\t\t\tgroup.paths[#group.paths] = nil\n"
        "\t\t\t\tend\n"
        "\t\t\telseif #group.paths < 3 then\n"
        "\t\t\t\tgroup.paths[#group.paths + 1] = track.path\n"
        "\t\t\tend\n";
    const std::string new_art_paths =
        "\t\t\tgroup.seenPath = group.seenPath or {}\n"
        "\t\t\tif not group.seenPath[track.path] then\n"
        "\t\t\t\tgroup.seenPath[track.path] = true\n"
        "\t\t\t\tif track.hasArt then\n"
        "\t\t\t\t\ttable.insert(group.paths, 1, track.path)\n"
        "\t\t\t\telse\n"
        "\t\t\t\t\tgroup.paths[#group.paths + 1] = track.path\n"
        "\t\t\t\tend\n"
        "\t\t\t\t-- Every song in the album is checked until a picture is found.\n"
        "\t\t\tend\n";
    const std::string old_jobs_line = "\tlines[#lines + 1] = \"JOBS\"\n";
    const std::string new_jobs_line =
        "\tif doomnetArtSteamUi and doomnetArtSteamUi ~= \"\" then\n"
        "\t\tlines[#lines + 1] = \"STEAMUI\"\n"
        "\t\tlines[#lines + 1] = doomnetArtSteamUi\n"
        "\tend\n"
        "\tlines[#lines + 1] = \"JOBS\"\n";
    const std::string old_sep = "\t\tlines[#lines + 1] = \"===\"\n";
    const std::string new_sep =
        "\t\tif group.sample then\n"
        "\t\t\tlines[#lines + 1] = \"SHOW\"\n"
        "\t\t\tlines[#lines + 1] = library.album_art_safe(group.sample)\n"
        "\t\tend\n"
        "\t\tlines[#lines + 1] = \"===\"\n";
    const std::string old_display = "\tif size < 32 or size > 48 * 1024 * 1024 then\n";
    const std::string new_display = "\tif size < 32 or size > 512 * 1024 * 1024 then\n";
    if (source.find("function library.regenerate_artwork") != std::string::npos) {
        const size_t group_at = source.find(old_group);
        const size_t jobs_line_at = source.find(old_jobs_line);
        const size_t sep_at = source.find(old_sep);
        if (group_at == std::string::npos || jobs_line_at == std::string::npos || sep_at == std::string::npos) {
            log_event(L"warn", L"lua", L"Regenerate Artwork still does not tell the worker which album cover to put on screen.");
        } else {
            source.replace(sep_at, old_sep.size(), new_sep);
            source.replace(source.find(old_jobs_line), old_jobs_line.size(), new_jobs_line);
            source.replace(source.find(old_group), old_group.size(), new_group);
            const size_t art_paths_at = source.find(old_art_paths);
            if (art_paths_at == std::string::npos) {
                log_event(L"warn", L"lua", L"Regenerate Artwork still skips songs after the first few, so a picture stored in a later file is missed.");
            } else {
                source.replace(art_paths_at, old_art_paths.size(), new_art_paths);
            }
        }
    }
    if (source.find("file_is_displayable") != std::string::npos) {
        const size_t display_at = source.find(old_display);
        if (display_at == std::string::npos) {
            log_event(L"warn", L"lua", L"A cover larger than 48 MB is still thrown away before the library can show it.");
        } else {
            source.replace(display_at, old_display.size(), new_display);
        }
    }
    const std::string old_worker =
        "\tif ok and content and content ~= \"\" then\n"
        "\t\tpcall(utils.write_file, dest, content)\n"
        "\tend\n";
    const std::string new_worker =
        "\tif ok and content and content ~= \"\" then\n"
        "\t\tcontent = content:gsub(\"%$side = Find%-Sidecar %$audioPath %$roots\", \"if ($audioPath -match '(?i)\\\\.musiclibrary\\\\\\\\|\\\\\\\\DoomNet Art\\\\\\\\') { return $null }\\n\\t$side = Find-Sidecar $audioPath $roots\\n\\tif ($side -match '(?i)\\\\.musiclibrary\\\\\\\\|\\\\\\\\DoomNet Art\\\\\\\\') { $side = $null }\", 1)\n"
        "\t\tcontent = content:gsub(\"%$head = Read%-Bytes %$audioPath 16777216\", \"$head = Read-EmbeddedHead $audioPath\", 1)\n"
        "\t\tcontent = content:gsub(\"function Extract%-Art%(%[string%]%$audioPath, %[string%[%]]%$roots%) {\", [=[function Read-EmbeddedHead([string]$audioPath) {\n"
        "\t$cap = 2147483647\n"
        "\t$probe = Read-Bytes $audioPath 10\n"
        "\t$path = $audioPath\n"
        "\tif (-not $probe -or $probe.Length -lt 8) { return $probe }\n"
        "\t$ascii = [Text.Encoding]::ASCII\n"
        "\t$kind = $ascii.GetString($probe, 0, [Math]::Min(4, $probe.Length))\n"
        "\tif ($kind.StartsWith('ID3') -and $probe.Length -ge 10) {\n"
        "\t\t$declared = 10 + (([int]$probe[6] -band 0x7f) -shl 21) + (([int]$probe[7] -band 0x7f) -shl 14) + (([int]$probe[8] -band 0x7f) -shl 7) + ([int]$probe[9] -band 0x7f)\n"
        "\t\tif ($declared -gt $cap) { $declared = $cap }\n"
        "\t\tif ($declared -gt 10) { return Read-Bytes $path $declared }\n"
        "\t\treturn $probe\n"
        "\t}\n"
        "\t$fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')\n"
        "\ttry {\n"
        "\t\tif ($kind.StartsWith('fLaC')) {\n"
        "\t\t\t$ms = New-Object IO.MemoryStream\n"
        "\t\t\t$four = New-Object byte[] 4\n"
        "\t\t\t[void]$fs.Read($four, 0, 4)\n"
        "\t\t\t$ms.Write($four, 0, 4)\n"
        "\t\t\twhile ($ms.Length -lt $cap) {\n"
        "\t\t\t\t$hdr = New-Object byte[] 4\n"
        "\t\t\t\t$got = $fs.Read($hdr, 0, 4)\n"
        "\t\t\t\tif ($got -lt 4) { break }\n"
        "\t\t\t\t$ms.Write($hdr, 0, 4)\n"
        "\t\t\t\t$last = ($hdr[0] -band 0x80) -ne 0\n"
        "\t\t\t\t$len = (([int]$hdr[1]) -shl 16) -bor (([int]$hdr[2]) -shl 8) -bor [int]$hdr[3]\n"
        "\t\t\t\tif ($len -lt 0) { break }\n"
        "\t\t\t\t$room = $cap - [int]$ms.Length\n"
        "\t\t\t\tif ($len -gt $room) { $len = $room }\n"
        "\t\t\t\t$left = $len\n"
        "\t\t\t\t$chunk = New-Object byte[] 1048576\n"
        "\t\t\t\twhile ($left -gt 0) {\n"
        "\t\t\t\t\t$n = [Math]::Min($chunk.Length, $left)\n"
        "\t\t\t\t\t$got = $fs.Read($chunk, 0, $n)\n"
        "\t\t\t\t\tif ($got -le 0) { break }\n"
        "\t\t\t\t\t$ms.Write($chunk, 0, $got)\n"
        "\t\t\t\t\t$left -= $got\n"
        "\t\t\t\t}\n"
        "\t\t\t\tif ($last) { break }\n"
        "\t\t\t}\n"
        "\t\t\treturn $ms.ToArray()\n"
        "\t\t}\n"
        "\t\t$ms = New-Object IO.MemoryStream\n"
        "\t\t$fs.Position = 0\n"
        "\t\twhile (($fs.Position + 8) -le $fs.Length -and $ms.Length -lt $cap) {\n"
        "\t\t\t$pos = $fs.Position\n"
        "\t\t\t$hdr = New-Object byte[] 8\n"
        "\t\t\t$got = $fs.Read($hdr, 0, 8)\n"
        "\t\t\t\tif ($got -lt 8) { break }\n"
        "\t\t\t$size = ([int]$hdr[0] -shl 24) -bor ([int]$hdr[1] -shl 16) -bor ([int]$hdr[2] -shl 8) -bor [int]$hdr[3]\n"
        "\t\t\t$type = $ascii.GetString($hdr, 4, 4)\n"
        "\t\t\tif ($size -lt 8) { break }\n"
        "\t\t\tif ($type -eq 'mdat' -or $type -eq 'free' -or $type -eq 'skip') {\n"
        "\t\t\t\t$fs.Position = [Math]::Min($fs.Length, [int64]$pos + $size)\n"
        "\t\t\t\tcontinue\n"
        "\t\t\t}\n"
        "\t\t\t$take = [Math]::Min([int64]$size, [int64]($cap - $ms.Length))\n"
        "\t\t\t$fs.Position = $pos\n"
        "\t\t\t$left = [int]$take\n"
        "\t\t\t$chunk = New-Object byte[] 1048576\n"
        "\t\t\twhile ($left -gt 0) {\n"
        "\t\t\t\t$n = [Math]::Min($chunk.Length, $left)\n"
        "\t\t\t\t$got = $fs.Read($chunk, 0, $n)\n"
        "\t\t\t\tif ($got -le 0) { break }\n"
        "\t\t\t\t$ms.Write($chunk, 0, $got)\n"
        "\t\t\t\t$left -= $got\n"
        "\t\t\t}\n"
        "\t\t\t$next = [Math]::Min($fs.Length, [int64]$pos + $size)\n"
        "\t\t\tif ($fs.Position -lt $next) { $fs.Position = $next }\n"
        "\t\t}\n"
        "\t\tif ($ms.Length -ge 8) { return $ms.ToArray() }\n"
        "\t} finally { $fs.Close() }\n"
        "\treturn $probe\n"
        "}\n"
        "function Extract-Art([string]$audioPath, [string[]]$roots) {]=], 1)\n"
        "\t\tcontent = content:gsub(\"if %(%$line %-eq 'JOBS'%) { %$mode = 'jobs'; continue }\", \"if ($line -eq 'STEAMUI') { $mode = 'steamui'; continue }\\n\\tif ($line -eq 'SHOW') { $mode = 'show'; continue }\\n\\tif ($line -eq 'JOBS') { $mode = 'jobs'; continue }\", 1)\n"
        "\t\tcontent = content:gsub(\"if %(%$line %-like '%*%.json'%) {\", \"if ($mode -eq 'steamui') { $script:DoomNetSteamUi = $line; continue }\\n\\tif ($mode -eq 'show') { if ($current) { $current.show = $line }; continue }\\n\\tif ($line -like '*.json') {\", 1)\n"
        "\t\tcontent = content:gsub(\"if %(Write%-ArtJson %$out %$img%) { %$written%+%+ }\", \"if ($out -like '*.json') { if (Write-ArtJson $out $img) { $written++ } }\", 1)\n"
        "\t\tcontent = content:gsub(\"Write%-ProgressFile %$progressPath %(%$n %+ 1%) %$total %$written %$true\", \"if ($img -and $job.show -and -not (Test-Path -LiteralPath (Join-Path (Split-Path $JobDir -Parent) ('art_jpeg\\albums\\' + $job.show + '.custom')))) {\\n\\t\\t\\t\\t$ext = 'jpg'\\n\\t\\t\\t\\tif ($img.Length -ge 8 -and $img[0] -eq 0x89) { $ext = 'png' }\\n\\t\\t\\t\\t$shown = @()\\n\\t\\t\\t\\tif ($script:DoomNetSteamUi) { $shown += (Join-Path $script:DoomNetSteamUi ('steam-music-player\\art\\albums\\' + $job.show + '.' + $ext)) }\\n\\t\\t\\t\\t$shown += (Join-Path (Split-Path $JobDir -Parent) ('art_jpeg\\albums\\' + $job.show + '.' + $ext))\\n\\t\\t\\t\\tforeach ($destImg in $shown) {\\n\\t\\t\\t\\t\\t$dirImg = Split-Path -Parent $destImg\\n\\t\\t\\t\\t\\tif ($dirImg -and -not (Test-Path -LiteralPath $dirImg)) { New-Item -ItemType Directory -Path $dirImg -Force | Out-Null }\\n\\t\\t\\t\\t\\t[IO.File]::WriteAllBytes($destImg, $img)\\n\\t\\t\\t\\t}\\n\\t\\t\\t}\\n\\t\\t\\tWrite-ProgressFile $progressPath ($n + 1) $total $written $true\", 1)\n"
        "\t\tcontent = content:gsub(\"%$tail = Read%-Tail %$audioPath 8388608\", \"$tail = Read-Tail $audioPath 2097152\", 1)\n"
        "\t\tcontent = content:gsub(\"Extract%-Id3 %$head\", \"Read-AnyPicture $audioPath; if (-not (Test-RealCover $img)) { $img = Extract-Id3 $head }\", 1)\n"
        "\t\tcontent = content:gsub(\"function Be32\", \"function Test-RealCover([byte[]]$img) {\\n\\tif (-not $img -or $img.Length -lt 32) { return $false }\\n\\tif ($img[0] -eq 0xFF -and $img[1] -eq 0xD8 -and $img[2] -eq 0xFF) { return $true }\\n\\tif ($img.Length -ge 8 -and $img[0] -eq 0x89 -and $img[1] -eq 0x50 -and $img[2] -eq 0x4E -and $img[3] -eq 0x47) { return $true }\\n\\tif ($img.Length -ge 12 -and $img[0] -eq 0x52 -and $img[1] -eq 0x49 -and $img[2] -eq 0x46 -and $img[3] -eq 0x46 -and $img[8] -eq 0x57 -and $img[9] -eq 0x45 -and $img[10] -eq 0x42 -and $img[11] -eq 0x50) { return $true }\\n\\treturn $false\\n}\\nfunction Read-AnyPicture([string]$path) {\\n"
        "try {\\n"
        "\\t$fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')\\n"
        "\\tif ($fs.Length -lt 16) { return $null }\\n"
        "\\t$head = New-Object byte[] 10\\n"
        "\\t[void]$fs.Read($head, 0, 10)\\n"
        "\\tif ([Text.Encoding]::ASCII.GetString($head, 0, 3) -ne 'ID3') { return $null }\\n"
        "\\t$ver = [int]$head[3]\\n"
        "\\t$tagSize = (([int]$head[6] -band 0x7F) -shl 21) -bor (([int]$head[7] -band 0x7F) -shl 14) -bor (([int]$head[8] -band 0x7F) -shl 7) -bor ([int]$head[9] -band 0x7F)\\n"
        "\\t$end = [Math]::Min([int64](10 + $tagSize), $fs.Length)\\n"
        "\\t$pos = [int64]10\\n"
        "\\t$best = $null\\n"
        "\\t$ascii = [Text.Encoding]::ASCII\\n"
        "\\twhile ($pos + 10 -le $end -and $ver -ge 3) {\\n"
        "\\t\\t$fs.Position = $pos\\n"
        "\\t\\t$hdr = New-Object byte[] 10\\n"
        "\\t\\tif ($fs.Read($hdr, 0, 10) -lt 10) { break }\\n"
        "\\t\\t$id = $ascii.GetString($hdr, 0, 4)\\n"
        "\\t\\tif ($id -notmatch '^[A-Z0-9]{4}$') { break }\\n"
        "\\t\\t$frameSize = if ($ver -ge 4) { (([int]$hdr[4] -band 0x7F) -shl 21) -bor (([int]$hdr[5] -band 0x7F) -shl 14) -bor (([int]$hdr[6] -band 0x7F) -shl 7) -bor ([int]$hdr[7] -band 0x7F) } else { ([int]$hdr[4] -shl 24) -bor ([int]$hdr[5] -shl 16) -bor ([int]$hdr[6] -shl 8) -bor [int]$hdr[7] }\\n"
        "\\t\\tif ($frameSize -le 0) { break }\\n"
        "\\t\\t$payloadAt = $pos + 10\\n"
        "\\t\\tif ($id -eq 'APIC' -and $frameSize -ge 8 -and ($payloadAt + $frameSize) -le $fs.Length) {\\n"
        "\\t\\t\\t$fs.Position = $payloadAt\\n"
        "\\t\\t\\t$payload = New-Object byte[] $frameSize\\n"
        "\\t\\t\\t[void]$fs.Read($payload, 0, $frameSize)\\n"
        "\\t\\t\\t$start = 0\\n"
        "\\t\\t\\t$limit = [Math]::Min($frameSize - 3, 65536)\\n"
        "\\t\\t\\tfor ($n = 0; $n -lt $limit; $n++) {\\n"
        "\\t\\t\\t\\tif ($payload[$n] -eq 0xFF -and $payload[$n+1] -eq 0xD8 -and $payload[$n+2] -eq 0xFF) { $start = $n; break }\\n"
        "\\t\\t\\t\\tif ($payload[$n] -eq 0x89 -and $payload[$n+1] -eq 0x50 -and $payload[$n+2] -eq 0x4E -and $payload[$n+3] -eq 0x47) { $start = $n; break }\\n"
        "\\t\\t\\t}\\n"
        "\\t\\t\\t$len = $frameSize - $start\\n"
        "\\t\\t\\tif ($len -ge 32) {\\n"
        "\\t\\t\\t\\t$img = New-Object byte[] $len\\n"
        "\\t\\t\\t\\t[Array]::Copy($payload, $start, $img, 0, $len)\\n"
        "\\t\\t\\t\\tif (Test-RealCover $img) { if (-not $best -or $img.Length -gt $best.Length) { $best = $img } }\\n"
        "\\t\\t\\t}\\n"
        "\\t\\t}\\n"
        "\\t\\t$pos = $payloadAt + $frameSize\\n"
        "\\t}\\n"
        "\\treturn $best\\n"
        "} catch { return $null } finally { if ($fs) { $fs.Close() } }\\n"
        "}\\n"
        "function Be32\", 1)\n"
        "\t\tcontent = content:gsub(\"return %$img\", \"if (Test-RealCover $img) { return $img }\", 20)\n"
        "\t\tcontent = content:gsub(\"%$size %-ge 24 %-and %$size %-le 8MB\", \"$size -ge 24\", 1)\n"
        "\t\tcontent = content:gsub(\"%$img = %$null%s+foreach %(%$src in %$job%.paths%) {%s+%$img = Extract%-Art %$src %$rootArr%s+if %(%$img%) { break }%s+}\", \"$img = $null\\n\\t\\t$bestCover = $null\\n\\t\\tforeach ($src in $job.paths) {\\n\\t\\t\\t$img = Extract-Art $src $rootArr\\n\\t\\t\\tif (-not (Test-RealCover $img)) { $img = $null }\\n\\t\\t\\tif ($img -and ((-not $bestCover) -or $img.Length -gt $bestCover.Length)) { $bestCover = $img }\\n\\t\\t\\tif ($bestCover -and $bestCover.Length -ge 200000) { break }\\n\\t\\t}\\n\\t\\t$img = $bestCover\", 1)\n"
        "\t\tcontent = content:gsub(\"if %(%$hits%.Count %-gt 0%) { return %$hits%[0]%.FullName }\", \"$bestImg = $hits | Sort-Object Length -Descending | Select-Object -First 1\\n\\t\\tif ($bestImg) { return $bestImg.FullName }\", 1)\n"
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
    const std::string old_gate = "\t\tif fs.exists(folder) and fs.is_directory(folder) then\n";
    const std::string new_gate = "\t\tif fs.exists(folder) and (fs.is_directory(folder) or not fs.is_file(folder)) then\n";
    const std::string old_list_gate =
        "\tfor _, folder in ipairs(state.folders) do\n"
        "\t\tif fs.exists(folder) and fs.is_directory(folder) then\n"
        "\t\t\tlistJob.folders[#listJob.folders + 1] = folder\n"
        "\t\tend\n"
        "\tend\n"
        "\tif #listJob.folders == 0 then\n";
    const std::string new_list_gate =
        "\tlistJob.addedOnly = false\n"
        "\tlocal onlyAdded = listJob.newOnly and doomnetAddedFolder\n"
        "\tfor _, folder in ipairs(state.folders) do\n"
        "\t\tif onlyAdded and folder ~= doomnetAddedFolder then\n"
        "\t\telseif fs.exists(folder) and (fs.is_directory(folder) or not fs.is_file(folder)) then\n"
        "\t\t\tlistJob.folders[#listJob.folders + 1] = folder\n"
        "\t\telse\n"
        "\t\t\tlogger:warn(\"[SteamMusicPlayer] music folder was not scanned because it is missing or not a folder: \" .. tostring(folder))\n"
        "\t\tend\n"
        "\tend\n"
        "\tif onlyAdded then\n"
        "\t\tlistJob.addedOnly = true\n"
        "\t\tdoomnetAddedFolder = nil\n"
        "\t\tlogger:info(\"[SteamMusicPlayer] only the folder just added is being scanned. Songs already in the library are skipped.\")\n"
        "\tend\n"
        "\tif #listJob.folders > 0 then\n"
        "\t\tlistJob.currentFolder = listJob.folders[1]\n"
        "\t\tlogger:info(\"[SteamMusicPlayer] scanning '\" .. tostring(listJob.folders[1]) .. \"' and every folder inside it. \" .. tostring(#listJob.folders) .. \" music folder(s) are configured.\")\n"
        "\tend\n"
        "\tif #listJob.folders == 0 then\n";
    const std::string old_add_folder =
        "\tstate.folders[#state.folders + 1] = path\n"
        "\tutils.write_file(folders_path(), json.encode(state.folders))\n"
        "\treturn true\n"
        "end\n";
    const std::string new_add_folder =
        "\tstate.folders[#state.folders + 1] = path\n"
        "\tutils.write_file(folders_path(), json.encode(state.folders))\n"
        "\tdoomnetAddedFolder = path\n"
        "\treturn true\n"
        "end\n";
    if (source.find("function library.add_folder(path)") != std::string::npos) {
        const size_t add_folder_at = source.find(old_add_folder);
        if (add_folder_at == std::string::npos) {
            log_event(L"warn", L"lua", L"Adding a folder still scans every saved song again.");
        } else {
            source.replace(add_folder_at, old_add_folder.size(), new_add_folder);
        }
    }
    const std::string old_lua_walk =
        "local function start_lua_walk()\n"
        "\treset_lua_walk()\n"
        "\twalkJob.active = true\n"
        "\tfor _, folder in ipairs(state.folders) do\n";
    const std::string new_lua_walk =
        "local function start_lua_walk()\n"
        "\treset_lua_walk()\n"
        "\twalkJob.active = true\n"
        "\tlocal folders = state.folders\n"
        "\tif listJob.addedOnly and listJob.folders and #listJob.folders > 0 then\n"
        "\t\tfolders = listJob.folders\n"
        "\tend\n"
        "\tfor _, folder in ipairs(folders) do\n";
    if (source.find("local function start_lua_walk()") != std::string::npos) {
        const size_t lua_walk_at = source.find(old_lua_walk);
        if (lua_walk_at == std::string::npos) {
            log_event(L"warn", L"lua", L"A new folder still walks every music folder already in the library.");
        } else {
            source.replace(lua_walk_at, old_lua_walk.size(), new_lua_walk);
        }
    }
    const size_t list_gate_at = source.find(old_list_gate);
    if (list_gate_at == std::string::npos) {
        log_event(L"warn", L"lua", L"The scan does not record which music folder it opened, so a saved folder can be skipped with no line in the log.");
    } else {
        source.replace(list_gate_at, old_list_gate.size(), new_list_gate);
    }
    size_t gate_from = 0;
    while (gate_from < source.size()) {
        const size_t found = source.find(old_gate, gate_from);
        if (found == std::string::npos) {
            break;
        }
        source.replace(found, old_gate.size(), new_gate);
        gate_from = found + new_gate.size();
    }
    const std::string old_write_files =
        "\t\t\"    foreach ($p in [System.IO.Directory]::EnumerateFiles($dir)) {\",\n"
        "\t\t\"      $ext = [System.IO.Path]::GetExtension($p).ToLowerInvariant()\",\n"
        "\t\t\"      if ($exts.ContainsKey($ext)) { $sw.WriteLine($p) }\",\n"
        "\t\t\"      elseif ($plexts.ContainsKey($ext)) { $pl.WriteLine($p) }\",\n"
        "\t\t\"    }\",\n";
    const std::string new_write_files =
        "\t\t\"    foreach ($p in [System.IO.Directory]::EnumerateFiles($dir)) {\",\n"
        "\t\t\"      try {\",\n"
        "\t\t\"        $ext = [System.IO.Path]::GetExtension($p).ToLowerInvariant()\",\n"
        "\t\t\"        if ($exts.ContainsKey($ext)) { $sw.WriteLine($p) }\",\n"
        "\t\t\"        elseif ($plexts.ContainsKey($ext)) { $pl.WriteLine($p) }\",\n"
        "\t\t\"      } catch {}\",\n"
        "\t\t\"    }\",\n";
    const size_t write_files_at = source.find(old_write_files);
    if (write_files_at == std::string::npos) {
        if (source.find("function WriteFiles") != std::string::npos) {
            log_event(L"warn", L"lua", L"One unreadable file still stops the folder listing, so songs after it are left out.");
        }
    } else {
        source.replace(write_files_at, old_write_files.size(), new_write_files);
    }
    const std::string old_new_cache =
        "\tfor _, folder in ipairs(listJob.folders) do\n"
        "\t\tcollect_root_audio(folder, listJob.results)\n"
        "\t\tread_scan_result_file(folder, listJob.results, scan_new_path())\n"
        "\tend\n";
    const std::string new_new_cache =
        "\tif newOnly then\n"
        "\t\tprocexec.delete_file(scan_new_path())\n"
        "\t\tprocexec.delete_file(scan_new_path() .. \".started\")\n"
        "\tend\n"
        "\tfor _, folder in ipairs(listJob.folders) do\n"
        "\t\tcollect_root_audio(folder, listJob.results)\n"
        "\t\tif not newOnly then\n"
        "\t\t\tread_scan_result_file(folder, listJob.results, scan_new_path())\n"
        "\t\tend\n"
        "\tend\n";
    const size_t new_cache_at = source.find(old_new_cache);
    if (new_cache_at == std::string::npos) {
        if (source.find("scan_new_path()") != std::string::npos) {
            log_event(L"warn", L"lua", L"Scan For New Files still trusts the previous listing, so a song added since then is skipped.");
        }
    } else {
        source.replace(new_cache_at, old_new_cache.size(), new_new_cache);
    }
    const std::string old_walk_ps = "function Walk([string]$dir) {";
    const std::string new_walk_ps =
        "function Walk([string]$dir) { "
        "if (-not $script:DoomNetSeenDir) { $script:DoomNetSeenDir = @{} }; "
        "$key = $dir.TrimEnd([char]92).ToLowerInvariant(); "
        "if ($script:DoomNetSeenDir.ContainsKey($key)) { return }; "
        "$script:DoomNetSeenDir[$key] = $true; "
        "try { foreach ($t in @((Get-Item -LiteralPath $dir -Force).Target)) { "
        "if ($t) { $full = $t; if (-not [IO.Path]::IsPathRooted($full)) { $full = Join-Path (Split-Path -Parent $dir) $t }; "
        "$tk = ([string]$full).TrimEnd([char]92).ToLowerInvariant(); "
        "if ($script:DoomNetSeenDir.ContainsKey($tk)) { return }; "
        "$script:DoomNetSeenDir[$tk] = $true } } } catch {}; ";
    const size_t walk_ps_at = source.find(old_walk_ps);
    if (walk_ps_at == std::string::npos) {
        if (source.find("function Walk([string]$dir)") != std::string::npos) {
            log_event(L"warn", L"lua", L"The folder listing still follows a link back into the music folder, so the same songs can be counted without end.");
        }
    } else {
        source.replace(walk_ps_at, old_walk_ps.size(), new_walk_ps);
    }
    const std::string old_list_read = "read_scan_result_file(listJob.currentFolder, listJob.results, listJob.outPath)";
    const std::string new_list_read =
        "do local doomnetSeen = {} "
        "for _, doomnetRoot in ipairs(listJob.folders or {}) do "
        "read_scan_result_file(doomnetRoot, listJob.results, listJob.outPath) "
        "doomnetSeen[path_key(doomnetRoot)] = true "
        "end "
        "if listJob.currentFolder and not doomnetSeen[path_key(listJob.currentFolder)] then "
        "read_scan_result_file(listJob.currentFolder, listJob.results, listJob.outPath) "
        "end end";
    if (source.find(old_list_read) == std::string::npos) {
        log_event(L"warn", L"lua", L"The finished folder listing is still matched against the last folder opened, so songs in the other folders are left out.");
    } else {
        size_t list_read_from = 0;
        while (list_read_from < source.size()) {
            const size_t found = source.find(old_list_read, list_read_from);
            if (found == std::string::npos) {
                break;
            }
            source.replace(found, old_list_read.size(), new_list_read);
            list_read_from = found + new_list_read.size();
        }
    }
    const std::string old_descend = "\t\t\t\t\t\t\tif (not dirOk) or isDir then\n";
    const std::string new_descend = "\t\t\t\t\t\t\tif dirOk and isDir and not (type(entry) == \"table\" and entry.is_symlink) then\n";
    if (source.find(old_descend) == std::string::npos) {
        log_event(L"warn", L"lua", L"The library walk still gives up on a child when the directory check returns false, so folders inside the music folder are skipped.");
    } else {
        size_t descend_from = 0;
        while (descend_from < source.size()) {
            const size_t found = source.find(old_descend, descend_from);
            if (found == std::string::npos) {
                break;
            }
            source.replace(found, old_descend.size(), new_descend);
            descend_from = found + new_descend.size();
        }
    }
    const std::string old_root_audio =
        "\t\t\t\t\t\tif SUPPORTED_EXTENSIONS[file_extension(path)] then\n"
        "\t\t\t\t\t\t\tresults[#results + 1] = path\n";
    const std::string new_root_audio =
        "\t\t\t\t\t\tif type(entry) == \"table\" and entry.is_directory and not entry.is_symlink then\n"
        "\t\t\t\t\t\t\tstack[#stack + 1] = path\n"
        "\t\t\t\t\t\telseif SUPPORTED_EXTENSIONS[file_extension(path)] then\n"
        "\t\t\t\t\t\t\tresults[#results + 1] = path\n";
    const std::string old_batch_audio =
        "\t\t\t\t\t\tif SUPPORTED_EXTENSIONS[ext] then\n"
        "\t\t\t\t\t\t\tresults[#results + 1] = path\n";
    const std::string new_batch_audio =
        "\t\t\t\t\t\tif type(entry) == \"table\" and entry.is_directory and not entry.is_symlink then\n"
        "\t\t\t\t\t\t\twalkJob.stack[#walkJob.stack + 1] = path\n"
        "\t\t\t\t\t\telseif SUPPORTED_EXTENSIONS[ext] then\n"
        "\t\t\t\t\t\t\tresults[#results + 1] = path\n";
    if (source.find(old_root_audio) == std::string::npos || source.find(old_batch_audio) == std::string::npos) {
        log_event(L"warn", L"lua", L"The library walk still does not enter a listed folder before it looks at the file name, so a folder of songs is reported as empty.");
    } else {
        source.replace(source.find(old_root_audio), old_root_audio.size(), new_root_audio);
        const size_t batch_audio_at = source.find(old_batch_audio);
        if (batch_audio_at != std::string::npos) {
            source.replace(batch_audio_at, old_batch_audio.size(), new_batch_audio);
        }
    }
    const std::string old_walk_dir = "\t\tlocal dir = table.remove(walkJob.stack)\n\t\tif dir and not walkJob.seen[dir] then\n";
    const std::string new_walk_dir = "\t\tlocal dir = table.remove(walkJob.stack)\n\t\tif dir and not walkJob.seen[dir] then\n\t\t\tlistJob.currentFolder = dir\n";
    const size_t walk_dir_at = source.find(old_walk_dir);
    if (walk_dir_at == std::string::npos) {
        log_event(L"warn", L"lua", L"The scan does not remember the folder it is listing, so the page cannot name it.");
    } else {
        source.replace(walk_dir_at, old_walk_dir.size(), new_walk_dir);
    }
    const std::string old_status_folder =
        "\t\tnewOnly = listJob.newOnly == true,\n"
        "\t\tadded = scanState.addedThisScan or 0,\n"
        "\t}\n";
    const std::string new_status_folder =
        "\t\tnewOnly = listJob.newOnly == true,\n"
        "\t\tadded = scanState.addedThisScan or 0,\n"
        "\t\tfolder = listJob.currentFolder or (listJob.folders and listJob.folders[1]) or (state.folders and state.folders[1]) or \"\",\n"
        "\t\tdirsDone = walkJob.dirsDone or 0,\n"
        "\t}\n";
    const size_t status_folder_at = source.find(old_status_folder);
    if (status_folder_at == std::string::npos) {
        log_event(L"warn", L"lua", L"The scan status does not include the music folder, so the page can only show a generic progress line.");
    } else {
        source.replace(status_folder_at, old_status_folder.size(), new_status_folder);
    }
    const std::string old_listing_folder =
        "\t\t\t\tdirsDone = walkJob.dirsDone,\n"
        "\t\t\t\tpendingCount = 0,\n"
        "\t\t\t\tadded = scanState.addedThisScan or 0,\n";
    const std::string new_listing_folder =
        "\t\t\t\tdirsDone = walkJob.dirsDone,\n"
        "\t\t\t\tpendingCount = 0,\n"
        "\t\t\t\tadded = scanState.addedThisScan or 0,\n"
        "\t\t\t\tfolder = listJob.currentFolder or (listJob.folders and listJob.folders[1]) or (state.folders and state.folders[1]) or \"\",\n";
    const size_t listing_folder_at = source.find(old_listing_folder);
    if (listing_folder_at == std::string::npos) {
        log_event(L"warn", L"lua", L"A listing batch does not name the music folder, so the page cannot show which folder is being read.");
    } else {
        source.replace(listing_folder_at, old_listing_folder.size(), new_listing_folder);
    }
    const std::string old_done_folder = "\t\tadded = scanState.addedThisScan or 0,\n\t\tlastPath = lastPath, -- TEMPORARY: crash-diagnosis aid\n";
    const std::string new_done_folder =
        "\t\tadded = scanState.addedThisScan or 0,\n"
        "\t\tfolder = listJob.currentFolder or (listJob.folders and listJob.folders[1]) or (state.folders and state.folders[1]) or \"\",\n"
        "\t\tdirsDone = walkJob.dirsDone or 0,\n"
        "\t\tlastPath = lastPath, -- TEMPORARY: crash-diagnosis aid\n";
    const size_t done_folder_at = source.find(old_done_folder);
    if (done_folder_at == std::string::npos) {
        log_event(L"warn", L"lua", L"A finished scan batch does not name the music folder, so the page cannot show which folder was read.");
    } else {
        source.replace(done_folder_at, old_done_folder.size(), new_done_folder);
    }
    const std::string old_track_lookup = "function library.get_track(id)\n";
    const std::string new_track_lookup =
        "function library.id_for_path(path)\n"
        "\tif type(path) ~= \"string\" or path == \"\" then\n"
        "\t\treturn nil\n"
        "\tend\n"
        "\tlocal direct = state.tracksByPath[path]\n"
        "\tif direct and direct.id ~= nil then\n"
        "\t\treturn direct.id\n"
        "\tend\n"
        "\tlocal key = path_key(path)\n"
        "\tfor stored, track in pairs(state.tracksByPath) do\n"
        "\t\tif path_key(stored) == key and track and track.id ~= nil then\n"
        "\t\t\treturn track.id\n"
        "\t\tend\n"
        "\tend\n"
        "\treturn nil\n"
        "end\n"
        "\n"
        "function library.get_track(id)\n";
    const size_t track_lookup_at = source.find(old_track_lookup);
    if (track_lookup_at == std::string::npos) {
        log_event(L"warn", L"lua", L"The library has no path lookup, so a playlist file cannot be matched to songs already in the library.");
    } else {
        source.replace(track_lookup_at, old_track_lookup.size(), new_track_lookup);
    }
    const std::string refresh_metadata =
        "\n"
        "local refreshMeta = {\n"
        "\tactive = false,\n"
        "\tpaths = {},\n"
        "\tindex = 1,\n"
        "\tupdated = 0,\n"
        "\tunchanged = 0,\n"
        "\tmissing = 0,\n"
        "\tartCopied = 0,\n"
        "\tunpersisted = 0,\n"
        "\tsteamUiDir = \"\",\n"
        "}\n"
        "\n"
        "local function refresh_cover_present(dir, safe)\n"
        "\tif not dir or dir == \"\" or not safe or safe == \"\" then\n"
        "\t\treturn false\n"
        "\tend\n"
        "\treturn fs.exists(fs.join(dir, safe .. \".jpg\")) or fs.exists(fs.join(dir, safe .. \".png\")) or fs.exists(fs.join(dir, safe .. \".custom\"))\n"
        "end\n"
        "\n"
        "local function refresh_copy_named(dir, oldSafe, newSafe, suffix)\n"
        "\tlocal src = fs.join(dir, oldSafe .. suffix)\n"
        "\tlocal dst = fs.join(dir, newSafe .. suffix)\n"
        "\tif not fs.exists(src) or fs.exists(dst) then\n"
        "\t\treturn false\n"
        "\tend\n"
        "\tpcall(fs.create_directories, dir)\n"
        "\treturn copy_bytes(src, dst) == true\n"
        "end\n"
        "\n"
        "local function refresh_follow_cover(oldSafe, newSafe, steamUiDir)\n"
        "\tif not oldSafe or not newSafe or oldSafe == \"\" or newSafe == \"\" or oldSafe == newSafe then\n"
        "\t\treturn false\n"
        "\tend\n"
        "\tlocal dirs = { fs.join(art_durable_dir(), \"albums\") }\n"
        "\tif type(steamUiDir) == \"string\" and steamUiDir ~= \"\" then\n"
        "\t\tdirs[#dirs + 1] = fs.join(art_public_dir(steamUiDir), \"albums\")\n"
        "\tend\n"
        "\tlocal dirIndex\n"
        "\tfor dirIndex = 1, #dirs do\n"
        "\t\tif refresh_cover_present(dirs[dirIndex], newSafe) then\n"
        "\t\t\treturn false\n"
        "\t\tend\n"
        "\tend\n"
        "\tlocal copied = false\n"
        "\tlocal suffixes = { \".jpg\", \".png\", \".custom\" }\n"
        "\tfor dirIndex = 1, #dirs do\n"
        "\t\tlocal suffixIndex\n"
        "\t\tfor suffixIndex = 1, #suffixes do\n"
        "\t\t\tif refresh_copy_named(dirs[dirIndex], oldSafe, newSafe, suffixes[suffixIndex]) then\n"
        "\t\t\t\tcopied = true\n"
        "\t\t\tend\n"
        "\t\tend\n"
        "\tend\n"
        "\treturn copied\n"
        "end\n"
        "\n"
        "local function refresh_tags_usable(tags)\n"
        "\tif type(tags) ~= \"table\" then\n"
        "\t\treturn false\n"
        "\tend\n"
        "\treturn tags.title or tags.artist or tags.album or tags.albumArtist or tags.genre or tags.year or tags.track or tags.disc\n"
        "end\n"
        "\n"
        "local function refresh_status()\n"
        "\tlocal total = #refreshMeta.paths\n"
        "\tlocal doneCount = refreshMeta.index - 1\n"
        "\tif doneCount < 0 then\n"
        "\t\tdoneCount = 0\n"
        "\tend\n"
        "\tlocal remaining = total - doneCount\n"
        "\tif remaining < 0 then\n"
        "\t\tremaining = 0\n"
        "\tend\n"
        "\treturn {\n"
        "\t\tok = true,\n"
        "\t\trunning = refreshMeta.active == true,\n"
        "\t\tdone = refreshMeta.active ~= true,\n"
        "\t\ttotal = total,\n"
        "\t\tremaining = remaining,\n"
        "\t\tdoneCount = doneCount,\n"
        "\t\tupdated = refreshMeta.updated,\n"
        "\t\tunchanged = refreshMeta.unchanged,\n"
        "\t\tmissing = refreshMeta.missing,\n"
        "\t\tartCopied = refreshMeta.artCopied,\n"
        "\t}\n"
        "end\n"
        "\n"
        "function library.refresh_metadata_start(steamUiDir)\n"
        "\tif refreshMeta.active then\n"
        "\t\treturn refresh_status()\n"
        "\tend\n"
        "\tlocal paths = {}\n"
        "\tfor path, track in pairs(state.tracksByPath) do\n"
        "\t\tif type(path) == \"string\" and path ~= \"\" and track and track.id ~= nil then\n"
        "\t\t\tpaths[#paths + 1] = path\n"
        "\t\tend\n"
        "\tend\n"
        "\trefreshMeta.active = true\n"
        "\trefreshMeta.paths = paths\n"
        "\trefreshMeta.index = 1\n"
        "\trefreshMeta.updated = 0\n"
        "\trefreshMeta.unchanged = 0\n"
        "\trefreshMeta.missing = 0\n"
        "\trefreshMeta.artCopied = 0\n"
        "\trefreshMeta.unpersisted = 0\n"
        "\trefreshMeta.steamUiDir = type(steamUiDir) == \"string\" and steamUiDir or \"\"\n"
        "\treturn refresh_status()\n"
        "end\n"
        "\n"
        "function library.refresh_metadata_batch(batchSize)\n"
        "\tif not refreshMeta.active then\n"
        "\t\treturn refresh_status()\n"
        "\tend\n"
        "\tlocal limit = tonumber(batchSize) or 25\n"
        "\tif limit < 1 then\n"
        "\t\tlimit = 1\n"
        "\tend\n"
        "\tif limit > 25 then\n"
        "\t\tlimit = 25\n"
        "\tend\n"
        "\tlocal processed = 0\n"
        "\twhile processed < limit and refreshMeta.index <= #refreshMeta.paths do\n"
        "\t\tlocal path = refreshMeta.paths[refreshMeta.index]\n"
        "\t\trefreshMeta.index = refreshMeta.index + 1\n"
        "\t\tprocessed = processed + 1\n"
        "\t\tlocal track = state.tracksByPath[path]\n"
        "\t\tif not track or track.id == nil then\n"
        "\t\t\trefreshMeta.missing = refreshMeta.missing + 1\n"
        "\t\telseif not fs.exists(path) then\n"
        "\t\t\t-- A missing file stays in the library. Playlists keep this id.\n"
        "\t\t\trefreshMeta.missing = refreshMeta.missing + 1\n"
        "\t\telse\n"
        "\t\t\tlocal timeOk, mtime = pcall(fs.file_mtime, path)\n"
        "\t\t\tmtime = timeOk and tonumber(mtime) or nil\n"
        "\t\t\tif mtime and tonumber(track.tagMtime) == mtime then\n"
        "\t\t\t\trefreshMeta.unchanged = refreshMeta.unchanged + 1\n"
        "\t\t\telse\n"
        "\t\t\t\tlocal tags = read_tags_safe(path)\n"
        "\t\t\t\tif not refresh_tags_usable(tags) then\n"
        "\t\t\t\t\trefreshMeta.missing = refreshMeta.missing + 1\n"
        "\t\t\t\telse\n"
        "\t\t\t\t\tlocal oldSafe = library.album_art_safe(track)\n"
        "\t\t\t\t\tlocal nextTitle = sanitize_utf8(tags.title or fallback_title(path))\n"
        "\t\t\t\t\tlocal nextArtist = sanitize_utf8(tags.artist or \"Unknown Artist\")\n"
        "\t\t\t\t\tlocal nextAlbumArtist = sanitize_utf8(tags.albumArtist or \"\")\n"
        "\t\t\t\t\tlocal nextAlbum = sanitize_utf8(tags.album or \"Unknown Album\")\n"
        "\t\t\t\t\tlocal nextGenre = sanitize_utf8(tags.genre or \"Unknown Genre\")\n"
        "\t\t\t\t\tlocal nextTrack = parse_media_index(tags.track)\n"
        "\t\t\t\t\tlocal nextDisc = parse_media_index(tags.disc)\n"
        "\t\t\t\t\tlocal nextYear = tags.year\n"
        "\t\t\t\t\tlocal textChanged = track.title ~= nextTitle or track.artist ~= nextArtist or track.albumArtist ~= nextAlbumArtist or track.album ~= nextAlbum or track.genre ~= nextGenre or track.track ~= nextTrack or track.disc ~= nextDisc or track.year ~= nextYear\n"
        "\t\t\t\t\ttrack.title = nextTitle\n"
        "\t\t\t\t\ttrack.artist = nextArtist\n"
        "\t\t\t\t\ttrack.albumArtist = nextAlbumArtist\n"
        "\t\t\t\t\ttrack.album = nextAlbum\n"
        "\t\t\t\t\ttrack.genre = nextGenre\n"
        "\t\t\t\t\ttrack.track = nextTrack\n"
        "\t\t\t\t\ttrack.disc = nextDisc\n"
        "\t\t\t\t\ttrack.year = nextYear\n"
        "\t\t\t\t\t-- The id stays the one already stored. A new path hash is not used.\n"
        "\t\t\t\t\tif mtime then\n"
        "\t\t\t\t\t\ttrack.tagMtime = mtime\n"
        "\t\t\t\t\tend\n"
        "\t\t\t\t\tlocal sizeOk, size = pcall(fs.file_size, path)\n"
        "\t\t\t\t\tif sizeOk and tonumber(size) then\n"
        "\t\t\t\t\t\ttrack.size = tonumber(size)\n"
        "\t\t\t\t\tend\n"
        "\t\t\t\t\tlocal newSafe = library.album_art_safe(track)\n"
        "\t\t\t\t\tif oldSafe ~= newSafe and refresh_follow_cover(oldSafe, newSafe, refreshMeta.steamUiDir) then\n"
        "\t\t\t\t\t\trefreshMeta.artCopied = refreshMeta.artCopied + 1\n"
        "\t\t\t\t\tend\n"
        "\t\t\t\t\tif textChanged then\n"
        "\t\t\t\t\t\trefreshMeta.updated = refreshMeta.updated + 1\n"
        "\t\t\t\t\telse\n"
        "\t\t\t\t\t\trefreshMeta.unchanged = refreshMeta.unchanged + 1\n"
        "\t\t\t\t\tend\n"
        "\t\t\t\t\trefreshMeta.unpersisted = refreshMeta.unpersisted + 1\n"
        "\t\t\t\tend\n"
        "\t\t\tend\n"
        "\t\tend\n"
        "\tend\n"
        "\tlocal finished = refreshMeta.index > #refreshMeta.paths\n"
        "\tif (finished or refreshMeta.unpersisted >= 2000) and refreshMeta.unpersisted > 0 then\n"
        "\t\tpersist_index()\n"
        "\t\trefreshMeta.unpersisted = 0\n"
        "\tend\n"
        "\tif finished then\n"
        "\t\trefreshMeta.active = false\n"
        "\t\tif refreshMeta.updated > 0 then\n"
        "\t\t\tpersist_compact_snapshot()\n"
        "\t\tend\n"
        "\tend\n"
        "\treturn refresh_status()\n"
        "end\n";
    const std::string library_end = "\nreturn library";
    const size_t library_end_at = source.rfind(library_end);
    const bool library_ends_here = library_end_at != std::string::npos &&
        (library_end_at + library_end.size() == source.size() ||
         (library_end_at + library_end.size() + 1 == source.size() && source.back() == '\n'));
    if (!library_ends_here) {
        if (source.find("function library.regenerate_artwork") != std::string::npos) {
            log_event(L"warn", L"lua", L"Refresh Metadata was not added, so a tag edit still needs a full library scan.");
        }
    } else if (source.find("function library.refresh_metadata_batch") == std::string::npos) {
        source.insert(library_end_at, refresh_metadata);
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
