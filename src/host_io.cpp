// Spec: "Lua host and the four shims". Lua's `io` and the file-related parts of
// `os`, opened by wide path so a Japanese or OneDrive path opens the first time.
//
// The stock `io` library calls the C runtime's fopen, which reads a Lua string
// as the ANSI code page. The player has PowerShell workarounds for that
// (library.lua open_file_safe). Here a Lua path is UTF-8, decoded to wide, and
// opened with CreateFileW.
//
// One extra behavior, and it is the only one that is not a plain file:
// inside an allow-listed call (HostConfig::parking_read_methods, by default
// get_track_audio_chunk) file:read(n) with n >= 64 KiB parks the coroutine and
// asks the browser thread to read the bytes (ReadRange). The state thread keeps
// running other calls meanwhile. In every other call, including
// scan_library_batch, reads happen on the state thread with no yield, so the
// tables it is building are never seen half done.

#include "host_internal.h"
#include "log.h"
#include "text.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace doomnet {
namespace {

const char* const kFileMeta = "DoomNet.FILE*";
constexpr size_t kWindowBytes = 64 * 1024;
constexpr uint32_t kMaxParkedRead = 16u * 1024u * 1024u;

struct HostFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool readable = false;
    bool writable = false;
    bool append = false;
    bool closed = false;
    uint64_t pos = 0;
    std::string window;  // read-ahead bytes starting at file offset window_off
    uint64_t window_off = 0;
    std::wstring path;
};

struct IoError {
    int errnum;
    std::string text;
};

IoError map_windows_error(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
            return {2, "No such file or directory"};
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return {13, "Permission denied"};
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return {17, "File exists"};
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return {28, "No space left on device"};
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_FILENAME_EXCED_RANGE:
            return {22, "Invalid argument"};
        default:
            return {5, wide_to_utf8(windows_error_text(code))};
    }
}

// Long paths: CreateFileW stops at 260 characters unless the path carries the
// \\?\ prefix. Music libraries do reach that depth.
std::wstring with_long_path_prefix(const std::wstring& path) {
    if (path.size() < 248 || path.rfind(L"\\\\?\\", 0) == 0) {
        return path;
    }
    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        std::wstring fixed = path;
        std::replace(fixed.begin(), fixed.end(), L'/', L'\\');
        return L"\\\\?\\" + fixed;
    }
    if (path.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return path;
}

struct OpenMode {
    bool read = false;
    bool write = false;
    bool append = false;
    bool truncate = false;
    bool create = false;
};

bool parse_mode(const char* mode, OpenMode& out) {
    if (mode == nullptr || mode[0] == '\0') {
        return false;
    }
    const char kind = mode[0];
    bool plus = false;
    for (const char* p = mode + 1; *p != '\0'; ++p) {
        if (*p == '+') {
            plus = true;
        } else if (*p != 'b') {
            return false;
        }
    }
    out = OpenMode{};
    switch (kind) {
        case 'r':
            out.read = true;
            out.write = plus;
            break;
        case 'w':
            out.write = true;
            out.read = plus;
            out.truncate = true;
            out.create = true;
            break;
        case 'a':
            out.write = true;
            out.append = true;
            out.read = plus;
            out.create = true;
            break;
        default:
            return false;
    }
    return true;
}

bool scan_call(lua_State* L) {
    const HostState& host = host_of(L);
    if (host.running == nullptr) {
        return false;
    }
    const std::string& method = host.running->method;
    return method == "scan_library_batch" || method == "rescan_library_start";
}

// A cloud placeholder has no local bytes. Opening it during a scan waits on the
// download, and one such file keeps the progress count at 0.
bool cloud_placeholder(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(with_long_path_prefix(path).c_str());
    constexpr DWORD recall = FILE_ATTRIBUTE_OFFLINE | 0x00040000u | 0x00400000u;
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & recall) != 0;
}

HANDLE open_handle(const std::wstring& path, const OpenMode& mode, DWORD& error, bool no_recall) {
    DWORD access = 0;
    if (mode.read) {
        access |= GENERIC_READ;
    }
    if (mode.write) {
        access |= mode.append ? (FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE) : GENERIC_WRITE;
    }
    DWORD disposition = OPEN_EXISTING;
    if (mode.truncate) {
        disposition = CREATE_ALWAYS;
    } else if (mode.create) {
        disposition = OPEN_ALWAYS;
    }
    const std::wstring target = with_long_path_prefix(path);
    const DWORD flags = FILE_ATTRIBUTE_NORMAL | (no_recall ? FILE_FLAG_OPEN_NO_RECALL : 0);
    HANDLE handle = CreateFileW(target.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = GetLastError();
    }
    return handle;
}

bool read_at(HANDLE handle, uint64_t offset, char* destination, size_t count, size_t& got, DWORD& error) {
    got = 0;
    while (got < count) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(count - got, 1u << 20));
        OVERLAPPED position{};
        const uint64_t at = offset + got;
        position.Offset = static_cast<DWORD>(at & 0xffffffffu);
        position.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD read = 0;
        if (!ReadFile(handle, destination + got, chunk, &read, &position)) {
            const DWORD code = GetLastError();
            if (code == ERROR_HANDLE_EOF) {
                break;
            }
            error = code;
            return false;
        }
        if (read == 0) {
            break;
        }
        got += read;
    }
    return true;
}

bool file_size_of(HANDLE handle, uint64_t& size, DWORD& error) {
    LARGE_INTEGER value{};
    if (!GetFileSizeEx(handle, &value)) {
        error = GetLastError();
        return false;
    }
    size = static_cast<uint64_t>(value.QuadPart);
    return true;
}

bool in_window(const HostFile& file) {
    return file.pos >= file.window_off && file.pos < file.window_off + file.window.size();
}

bool fill_window(HostFile& file, DWORD& error) {
    file.window.assign(kWindowBytes, '\0');
    size_t got = 0;
    if (!read_at(file.handle, file.pos, file.window.data(), kWindowBytes, got, error)) {
        file.window.clear();
        return false;
    }
    file.window.resize(got);
    file.window_off = file.pos;
    return true;
}

// Reads up to `count` bytes at the file position. Fewer bytes means end of file.
bool read_bytes(HostFile& file, uint64_t count, std::string& out, DWORD& error) {
    out.clear();
    uint64_t size = 0;
    if (!file_size_of(file.handle, size, error)) {
        return false;
    }
    if (file.pos >= size) {
        return true;
    }
    count = std::min<uint64_t>(count, size - file.pos);
    out.reserve(static_cast<size_t>(std::min<uint64_t>(count, 64u * 1024u * 1024u)));
    while (out.size() < count) {
        const size_t remaining = static_cast<size_t>(count - out.size());
        if (in_window(file)) {
            const size_t available = static_cast<size_t>(file.window_off + file.window.size() - file.pos);
            const size_t take = std::min(available, remaining);
            out.append(file.window, static_cast<size_t>(file.pos - file.window_off), take);
            file.pos += take;
            continue;
        }
        if (remaining >= kWindowBytes) {
            const size_t before = out.size();
            out.resize(before + remaining);
            size_t got = 0;
            if (!read_at(file.handle, file.pos, out.data() + before, remaining, got, error)) {
                out.resize(before);
                return false;
            }
            out.resize(before + got);
            file.pos += got;
            break;
        }
        if (!fill_window(file, error)) {
            return false;
        }
        if (file.window.empty()) {
            break;
        }
    }
    return true;
}

bool read_line(HostFile& file, bool keep_newline, std::string& out, bool& got_any, DWORD& error) {
    out.clear();
    got_any = false;
    for (;;) {
        if (!in_window(file)) {
            if (!fill_window(file, error)) {
                return false;
            }
            if (file.window.empty()) {
                return true;
            }
        }
        const size_t from = static_cast<size_t>(file.pos - file.window_off);
        const size_t newline = file.window.find('\n', from);
        got_any = true;
        if (newline == std::string::npos) {
            out.append(file.window, from, std::string::npos);
            file.pos = file.window_off + file.window.size();
            continue;
        }
        out.append(file.window, from, newline - from + (keep_newline ? 1 : 0));
        file.pos = file.window_off + newline + 1;
        return true;
    }
}

bool write_bytes(HostFile& file, const char* data, size_t length, DWORD& error) {
    file.window.clear();
    size_t done = 0;
    while (done < length) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(length - done, 1u << 20));
        DWORD wrote = 0;
        BOOL ok;
        if (file.append) {
            ok = WriteFile(file.handle, data + done, chunk, &wrote, nullptr);
        } else {
            OVERLAPPED position{};
            position.Offset = static_cast<DWORD>((file.pos + done) & 0xffffffffu);
            position.OffsetHigh = static_cast<DWORD>((file.pos + done) >> 32);
            ok = WriteFile(file.handle, data + done, chunk, &wrote, &position);
        }
        if (!ok) {
            error = GetLastError();
            file.pos += done;
            return false;
        }
        done += wrote;
    }
    if (file.append) {
        uint64_t size = 0;
        DWORD ignored = 0;
        if (file_size_of(file.handle, size, ignored)) {
            file.pos = size;
        }
    } else {
        file.pos += length;
    }
    return true;
}

// ---- Lua bindings --------------------------------------------------------

HostFile** file_slot(lua_State* L, int index) {
    return static_cast<HostFile**>(luaL_checkudata(L, index, kFileMeta));
}

HostFile* check_open_file(lua_State* L, int index) {
    HostFile* file = *file_slot(L, index);
    if (file == nullptr || file->closed) {
        luaL_error(L, "attempt to use a closed file");
    }
    return file;
}

int push_failure(lua_State* L, DWORD code, const char* prefix) {
    const IoError mapped = map_windows_error(code);
    lua_pushnil(L);
    if (prefix != nullptr) {
        lua_pushfstring(L, "%s: %s", prefix, mapped.text.c_str());
    } else {
        lua_pushlstring(L, mapped.text.data(), mapped.text.size());
    }
    lua_pushinteger(L, mapped.errnum);
    return 3;
}

int Lua_io_open(lua_State* L) {
    size_t length = 0;
    const char* raw_path = luaL_checklstring(L, 1, &length);
    const char* mode_text = luaL_optstring(L, 2, "r");
    OpenMode mode;
    if (!parse_mode(mode_text, mode)) {
        return luaL_argerror(L, 2, "invalid mode");
    }
    std::wstring path;
    if (!lua_path_to_wide(raw_path, length, path)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s: Invalid argument", raw_path);
        lua_pushinteger(L, 22);
        return 3;
    }
    DWORD error = 0;
    const bool no_recall = mode.read && !mode.write && scan_call(L);
    if (no_recall && cloud_placeholder(path)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            log_event(L"info", L"scan", (L"A music file at '" + path + L"' is still in the cloud, so the scan is not downloading it. The count moves on and the tags stay empty until the file is on this PC.").c_str());
        }
        lua_pushnil(L);
        lua_pushfstring(L, "%s: The file is still in the cloud", raw_path);
        lua_pushinteger(L, 0);
        return 3;
    }
    HANDLE handle = open_handle(path, mode, error, no_recall);
    if (handle == INVALID_HANDLE_VALUE) {
        return push_failure(L, error, raw_path);
    }
    HostFile* file = new HostFile();
    file->handle = handle;
    file->readable = mode.read;
    file->writable = mode.write;
    file->append = mode.append;
    file->path = path;
    if (mode.append) {
        uint64_t size = 0;
        DWORD ignored = 0;
        if (file_size_of(handle, size, ignored)) {
            file->pos = size;
        }
    }
    HostFile** slot = static_cast<HostFile**>(lua_newuserdatauv(L, sizeof(HostFile*), 0));
    *slot = file;
    luaL_setmetatable(L, kFileMeta);
    return 1;
}

int Lua_io_type(lua_State* L) {
    luaL_checkany(L, 1);
    void* raw = luaL_testudata(L, 1, kFileMeta);
    if (raw == nullptr) {
        lua_pushnil(L);
    } else {
        HostFile* file = *static_cast<HostFile**>(raw);
        lua_pushstring(L, (file == nullptr || file->closed) ? "closed file" : "file");
    }
    return 1;
}

int close_file(lua_State* L) {
    HostFile** slot = file_slot(L, 1);
    HostFile* file = *slot;
    if (file == nullptr || file->closed) {
        return luaL_error(L, "attempt to use a closed file");
    }
    file->closed = true;
    if (file->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(file->handle);
        file->handle = INVALID_HANDLE_VALUE;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int Lua_io_close(lua_State* L) {
    if (lua_isnone(L, 1)) {
        return luaL_error(L, "io.close needs a file; the host has no default output file");
    }
    return close_file(L);
}

int file_gc(lua_State* L) {
    HostFile** slot = static_cast<HostFile**>(luaL_checkudata(L, 1, kFileMeta));
    HostFile* file = *slot;
    if (file != nullptr) {
        if (!file->closed && file->handle != INVALID_HANDLE_VALUE) {
            CloseHandle(file->handle);
        }
        delete file;
        *slot = nullptr;
    }
    return 0;
}

int file_tostring(lua_State* L) {
    HostFile* file = *file_slot(L, 1);
    if (file == nullptr || file->closed) {
        lua_pushliteral(L, "file (closed)");
    } else {
        lua_pushfstring(L, "file (%p)", static_cast<void*>(file));
    }
    return 1;
}

// Pushes one result for a read format. Returns false when the read hit end of
// file (the caller pushes nil in its place). Raises a Lua error for an unknown
// format. I/O errors are reported through `error`.
bool read_one_format(lua_State* L, HostFile* file, int format_index, DWORD& error, bool& failed) {
    failed = false;
    if (lua_type(L, format_index) == LUA_TNUMBER) {
        const lua_Integer wanted = luaL_checkinteger(L, format_index);
        if (wanted < 0) {
            luaL_argerror(L, format_index, "count must not be negative");
        }
        std::string bytes;
        if (wanted == 0) {
            uint64_t size = 0;
            if (!file_size_of(file->handle, size, error)) {
                failed = true;
                return false;
            }
            if (file->pos >= size) {
                return false;
            }
            lua_pushliteral(L, "");
            return true;
        }
        if (!read_bytes(*file, static_cast<uint64_t>(wanted), bytes, error)) {
            failed = true;
            return false;
        }
        if (bytes.empty()) {
            return false;
        }
        lua_pushlstring(L, bytes.data(), bytes.size());
        return true;
    }
    const char* format = luaL_checkstring(L, format_index);
    if (*format == '*') {
        ++format;
    }
    switch (*format) {
        case 'a': {
            std::string all;
            std::string piece;
            for (;;) {
                if (!read_bytes(*file, 4u * 1024u * 1024u, piece, error)) {
                    failed = true;
                    return false;
                }
                if (piece.empty()) {
                    break;
                }
                all += piece;
            }
            lua_pushlstring(L, all.data(), all.size());
            return true;
        }
        case 'l':
        case 'L': {
            std::string line;
            bool got_any = false;
            if (!read_line(*file, *format == 'L', line, got_any, error)) {
                failed = true;
                return false;
            }
            if (!got_any) {
                return false;
            }
            lua_pushlstring(L, line.data(), line.size());
            return true;
        }
        default:
            luaL_argerror(L, format_index, "invalid format (the host supports a count, \"a\", \"l\" and \"L\")");
            return false;
    }
}

int read_formats(lua_State* L, HostFile* file, int first) {
    int given = lua_gettop(L) - first + 1;
    if (given <= 0) {
        // file:read() with no format means one line.
        lua_pushliteral(L, "l");
        first = lua_gettop(L);
        given = 1;
    }
    int results = 0;
    DWORD error = 0;
    bool failed = false;
    bool ok = true;
    for (int i = 0; i < given && ok; ++i) {
        ok = read_one_format(L, file, first + i, error, failed);
        if (failed) {
            return push_failure(L, error, nullptr);
        }
        if (!ok) {
            lua_pushnil(L);
        }
        ++results;
    }
    return results;
}

int read_after_park(lua_State* L, int, lua_KContext) {
    // Stack: file, count, bytes-already-buffered, then what the browser thread
    // sent: (bytes) or (nil, message).
    HostFile* file = check_open_file(L, 1);
    if (lua_isnil(L, 4)) {
        lua_pushnil(L);
        lua_pushvalue(L, 5);
        lua_pushinteger(L, 5);
        return 3;
    }
    size_t buffered = 0;
    lua_tolstring(L, 3, &buffered);
    size_t arrived = 0;
    lua_tolstring(L, 4, &arrived);
    file->pos += arrived;
    if (buffered + arrived == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushvalue(L, 3);
    lua_pushvalue(L, 4);
    lua_concat(L, 2);
    return 1;
}

int Lua_file_read(lua_State* L) {
    HostFile* file = check_open_file(L, 1);
    if (!file->readable) {
        lua_pushnil(L);
        lua_pushliteral(L, "Bad file descriptor");
        lua_pushinteger(L, 9);
        return 3;
    }
    if (lua_gettop(L) == 2 && lua_type(L, 2) == LUA_TNUMBER && can_park(L)) {
        HostState& host = host_of(L);
        const lua_Integer wanted = lua_tointeger(L, 2);
        if (host.running != nullptr && host.running->parking_reads && wanted >= static_cast<lua_Integer>(kParkReadThreshold) && wanted <= static_cast<lua_Integer>(kMaxParkedRead)) {
            uint64_t remaining = static_cast<uint64_t>(wanted);
            {
                // Scoped: nothing with a destructor may be alive when lua_yieldk
                // unwinds with longjmp.
                std::string buffered;
                if (in_window(*file)) {
                    const size_t available = static_cast<size_t>(file->window_off + file->window.size() - file->pos);
                    const size_t take = std::min<size_t>(available, static_cast<size_t>(wanted));
                    buffered.assign(file->window, static_cast<size_t>(file->pos - file->window_off), take);
                    file->pos += take;
                }
                file->window.clear();
                remaining -= buffered.size();
                lua_pushlstring(L, buffered.data(), buffered.size());
            }
            if (remaining == 0) {
                return 1;
            }
            host.pending = YieldRequest{};
            host.pending.kind = YieldRequest::Kind::ReadRange;
            host.pending.file = file->handle;
            host.pending.path = file->path;
            host.pending.offset = file->pos;
            host.pending.length = static_cast<uint32_t>(remaining);
            return lua_yieldk(L, 0, 0, read_after_park);
        }
    }
    return read_formats(L, file, 2);
}

int Lua_file_write(lua_State* L) {
    HostFile* file = check_open_file(L, 1);
    if (!file->writable) {
        lua_pushnil(L);
        lua_pushliteral(L, "Bad file descriptor");
        lua_pushinteger(L, 9);
        return 3;
    }
    const int count = lua_gettop(L);
    for (int i = 2; i <= count; ++i) {
        size_t length = 0;
        const char* data = nullptr;
        if (lua_type(L, i) == LUA_TNUMBER) {
            data = lua_tolstring(L, i, &length);
        } else {
            data = luaL_checklstring(L, i, &length);
        }
        DWORD error = 0;
        if (!write_bytes(*file, data, length, error)) {
            return push_failure(L, error, nullptr);
        }
    }
    lua_settop(L, 1);
    return 1;
}

int Lua_file_seek(lua_State* L) {
    HostFile* file = check_open_file(L, 1);
    static const char* const kWhence[] = {"set", "cur", "end", nullptr};
    const int whence = luaL_checkoption(L, 2, "cur", kWhence);
    const lua_Integer offset = luaL_optinteger(L, 3, 0);
    int64_t base = 0;
    if (whence == 1) {
        base = static_cast<int64_t>(file->pos);
    } else if (whence == 2) {
        uint64_t size = 0;
        DWORD error = 0;
        if (!file_size_of(file->handle, size, error)) {
            return push_failure(L, error, nullptr);
        }
        base = static_cast<int64_t>(size);
    }
    const int64_t target = base + static_cast<int64_t>(offset);
    if (target < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "Invalid argument");
        lua_pushinteger(L, 22);
        return 3;
    }
    file->pos = static_cast<uint64_t>(target);
    push_count(L, file->pos);
    return 1;
}

int Lua_file_close(lua_State* L) {
    return close_file(L);
}

int Lua_file_flush(lua_State* L) {
    check_open_file(L, 1);
    lua_settop(L, 1);
    return 1;  // writes go straight to the OS; there is no user-space buffer
}

int Lua_file_setvbuf(lua_State* L) {
    check_open_file(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

// Returns -1 when an error message is on the stack and the caller must raise it
// (lua_error longjmps, so the raising frame must hold no C++ objects).
int lines_step_impl(lua_State* L, HostFile* file) {
    DWORD error = 0;
    std::string line;
    bool got_any = false;
    if (!read_line(*file, false, line, got_any, error)) {
        const IoError mapped = map_windows_error(error);
        lua_pushlstring(L, mapped.text.data(), mapped.text.size());
        return -1;
    }
    if (!got_any) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, line.data(), line.size());
    return 1;
}

int lines_step(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    HostFile* file = check_open_file(L, -1);
    lua_pop(L, 1);
    const int pushed = lines_step_impl(L, file);
    if (pushed < 0) {
        return lua_error(L);
    }
    return pushed;
}

int Lua_file_lines(lua_State* L) {
    check_open_file(L, 1);
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, lines_step, 1);
    return 1;
}

const luaL_Reg kFileMethods[] = {
    {"read", guarded<&Lua_file_read>},
    {"write", guarded<&Lua_file_write>},
    {"seek", guarded<&Lua_file_seek>},
    {"close", guarded<&Lua_file_close>},
    {"flush", guarded<&Lua_file_flush>},
    {"setvbuf", guarded<&Lua_file_setvbuf>},
    {"lines", guarded<&Lua_file_lines>},
    {nullptr, nullptr},
};

int luaopen_host_io(lua_State* L) {
    if (luaL_newmetatable(L, kFileMeta) != 0) {
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        luaL_setfuncs(L, kFileMethods, 0);
        lua_pushcfunction(L, file_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, close_file);
        lua_setfield(L, -2, "__close");
        lua_pushcfunction(L, file_tostring);
        lua_setfield(L, -2, "__tostring");
        lua_pushstring(L, kFileMeta);
        lua_setfield(L, -2, "__name");
    }
    lua_pop(L, 1);
    const luaL_Reg functions[] = {
        {"open", guarded<&Lua_io_open>},
        {"type", guarded<&Lua_io_type>},
        {"close", guarded<&Lua_io_close>},
        {nullptr, nullptr},
    };
    luaL_newlib(L, functions);
    return 1;
}

// ---- os ------------------------------------------------------------------

int Lua_os_remove(lua_State* L) {
    size_t length = 0;
    const char* raw = luaL_checklstring(L, 1, &length);
    std::wstring path;
    if (!lua_path_to_wide(raw, length, path)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s: Invalid argument", raw);
        lua_pushinteger(L, 22);
        return 3;
    }
    const std::wstring target = with_long_path_prefix(path);
    const DWORD attrs = GetFileAttributesW(target.c_str());
    BOOL ok;
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        ok = RemoveDirectoryW(target.c_str());
    } else {
        ok = DeleteFileW(target.c_str());
    }
    if (!ok) {
        return push_failure(L, GetLastError(), raw);
    }
    lua_pushboolean(L, 1);
    return 1;
}

int Lua_os_rename(lua_State* L) {
    size_t from_length = 0;
    size_t to_length = 0;
    const char* raw_from = luaL_checklstring(L, 1, &from_length);
    const char* raw_to = luaL_checklstring(L, 2, &to_length);
    std::wstring from;
    std::wstring to;
    if (!lua_path_to_wide(raw_from, from_length, from) || !lua_path_to_wide(raw_to, to_length, to)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s: Invalid argument", raw_from);
        lua_pushinteger(L, 22);
        return 3;
    }
    // C rename() refuses to replace an existing file on Windows. Replacing is
    // what every caller in the player wants for its write-then-rename saves.
    if (!MoveFileExW(with_long_path_prefix(from).c_str(), with_long_path_prefix(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        return push_failure(L, GetLastError(), raw_from);
    }
    lua_pushboolean(L, 1);
    return 1;
}

int Lua_os_getenv(lua_State* L) {
    size_t length = 0;
    const char* raw = luaL_checklstring(L, 1, &length);
    std::wstring name;
    if (!utf8_to_wide_strict(raw, length, name) || name.empty()) {
        lua_pushnil(L);
        return 1;
    }
    const DWORD needed = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (needed == 0) {
        lua_pushnil(L);
        return 1;
    }
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name.c_str(), value.data(), needed);
    value.resize(written);
    const std::string utf8 = wide_to_utf8(value);
    lua_pushlstring(L, utf8.data(), utf8.size());
    return 1;
}

}  // namespace

void open_host_io(lua_State* L) {
    luaL_requiref(L, "io", luaopen_host_io, 1);
    lua_pop(L, 1);
}

void open_host_os(lua_State* L) {
    luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 1);
    // Keep os.clock, os.date, os.time, os.difftime. Replace what touches the
    // filesystem or the environment with wide versions, and remove what would
    // start a shell or end the process.
    lua_pushcfunction(L, guarded<&Lua_os_remove>);
    lua_setfield(L, -2, "remove");
    lua_pushcfunction(L, guarded<&Lua_os_rename>);
    lua_setfield(L, -2, "rename");
    lua_pushcfunction(L, guarded<&Lua_os_getenv>);
    lua_setfield(L, -2, "getenv");
    for (const char* name : {"execute", "exit", "tmpname", "setlocale"}) {
        lua_pushnil(L);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
}

}  // namespace doomnet
