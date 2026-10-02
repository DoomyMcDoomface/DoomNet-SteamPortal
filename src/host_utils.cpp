// Spec: "Lua host and the four shims". The `utils` module.
//
// Read: Millennium src/lua_host/api/utils.cc on main (last changed in commit
// e442e87ebd351aecd6f6ebe0e99ed9f7451af350) and src/include/millennium/encoding.h.
// The function names, argument order, and return shapes match. Where this
// differs, it is on purpose and named here:
//   sleep     parks the coroutine (Sleep) instead of blocking the thread.
//   exec      parks the coroutine (Exec) and never starts cmd.exe. Millennium runs
//             `cmd.exe /c <command>`; the player only passes a program and its
//             arguments (procexec.lua), which CreateProcessW runs directly.
//   hash      the same value as Millennium's std::hash<std::string> on MSVC
//             (64-bit FNV-1a), returned as a Lua double the way LuaJIT's
//             lua_pushinteger did, so tostring(utils.hash(path)) matches the
//             ids already saved in library_index.json.
//   getenv, read_file, write_file, append_file use UTF-8 and wide Windows calls.
//   Numbers are pushed as doubles, as they were under LuaJIT.

#include "anchors.h"
#include "host_internal.h"
#include "log.h"
#include "process_job.h"
#include "text.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace doomnet {

void host_utils_anchor() {}

namespace {

std::mt19937& generator() {
    static std::mt19937 gen{std::random_device{}()};
    return gen;
}

// ---- time / sleep ---------------------------------------------------------

int Lua_sleep(lua_State* L) {
    lua_Number requested = luaL_checknumber(L, 1);
    if (!(requested > 0)) {
        requested = 0;
    }
    if (requested > 24.0 * 3600.0 * 1000.0) {
        requested = 24.0 * 3600.0 * 1000.0;
    }
    const uint32_t milliseconds = static_cast<uint32_t>(requested);
    if (can_park(L)) {
        HostState& host = host_of(L);
        host.pending = YieldRequest{};
        host.pending.kind = YieldRequest::Kind::Sleep;
        host.pending.sleep_ms = milliseconds;
        return lua_yield(L, 0);
    }
    // Lua is inside a C boundary that cannot yield (a table.sort comparator, a
    // gsub callback, a require). Blocking is the only option. Say so.
    static std::atomic<int> reported{0};
    if (reported.fetch_add(1) < 5) {
        const std::string caller = lua_caller(L, 1);
        const std::wstring detail = L"utils.sleep(" + std::to_wstring(milliseconds) + L") was called from " + utf8_to_wide_lossy(caller) + L" where Lua cannot yield, so this call blocked the state thread for that long.";
        log_event(L"warn", L"lua", detail.c_str());
    }
    Sleep(milliseconds);
    return 0;
}

int Lua_time(lua_State* L) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    push_number(L, static_cast<double>(ms.count()) / 1000.0);
    return 1;
}

int Lua_time_ms(lua_State* L) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    push_number(L, static_cast<double>(ms.count()));
    return 1;
}

int Lua_time_micro(lua_State* L) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch());
    push_number(L, static_cast<double>(us.count()));
    return 1;
}

// ---- math -----------------------------------------------------------------

int Lua_round(lua_State* L) {
    push_number(L, std::round(luaL_checknumber(L, 1)));
    return 1;
}

int Lua_clamp(lua_State* L) {
    const double value = luaL_checknumber(L, 1);
    const double low = luaL_checknumber(L, 2);
    const double high = luaL_checknumber(L, 3);
    push_number(L, (low <= high) ? std::clamp(value, low, high) : value);  // std::clamp(lo>hi) is undefined
    return 1;
}

int Lua_sign(lua_State* L) {
    const double value = luaL_checknumber(L, 1);
    push_number(L, (value > 0) - (value < 0));
    return 1;
}

int Lua_lerp(lua_State* L) {
    const double a = luaL_checknumber(L, 1);
    const double b = luaL_checknumber(L, 2);
    const double t = luaL_checknumber(L, 3);
    push_number(L, a + t * (b - a));
    return 1;
}

// ---- random ---------------------------------------------------------------

int Lua_random_int(lua_State* L) {
    int low = static_cast<int>(luaL_checkinteger(L, 1));
    int high = static_cast<int>(luaL_checkinteger(L, 2));
    if (low > high) {
        std::swap(low, high);
    }
    std::uniform_int_distribution<int> distribution(low, high);
    push_number(L, distribution(generator()));
    return 1;
}

int Lua_random_float(lua_State* L) {
    const double low = luaL_optnumber(L, 1, 0.0);
    const double high = luaL_optnumber(L, 2, 1.0);
    if (!(low < high)) {
        push_number(L, low);
        return 1;
    }
    std::uniform_real_distribution<double> distribution(low, high);
    push_number(L, distribution(generator()));
    return 1;
}

int Lua_random_seed(lua_State* L) {
    generator().seed(static_cast<unsigned int>(luaL_checkinteger(L, 1)));
    return 0;
}

// ---- strings --------------------------------------------------------------

int Lua_split(lua_State* L) {
    size_t text_length = 0;
    size_t delimiter_length = 0;
    const char* text = luaL_checklstring(L, 1, &text_length);
    const char* delimiter = luaL_checklstring(L, 2, &delimiter_length);
    lua_newtable(L);
    lua_Integer index = 1;
    const std::string subject(text, text_length);
    const std::string separator(delimiter, delimiter_length);
    if (separator.empty()) {
        // Millennium's loop never ends on an empty delimiter. Return the text whole.
        lua_pushlstring(L, subject.data(), subject.size());
        lua_rawseti(L, -2, index);
        return 1;
    }
    size_t start = 0;
    for (;;) {
        const size_t found = subject.find(separator, start);
        if (found == std::string::npos) {
            break;
        }
        lua_pushlstring(L, subject.data() + start, found - start);
        lua_rawseti(L, -2, index++);
        start = found + separator.size();
    }
    lua_pushlstring(L, subject.data() + start, subject.size() - start);
    lua_rawseti(L, -2, index);
    return 1;
}

constexpr const char* kSpace = " \t\n\r\f\v";

int trim_with(lua_State* L, bool left, bool right) {
    size_t length = 0;
    const char* raw = luaL_checklstring(L, 1, &length);
    const std::string text(raw, length);
    const size_t first = left ? text.find_first_not_of(kSpace) : 0;
    if (first == std::string::npos) {
        lua_pushliteral(L, "");
        return 1;
    }
    size_t last = text.size();
    if (right) {
        const size_t found = text.find_last_not_of(kSpace);
        last = found == std::string::npos ? 0 : found + 1;
    }
    lua_pushlstring(L, text.data() + first, last > first ? last - first : 0);
    return 1;
}

int Lua_trim(lua_State* L) { return trim_with(L, true, true); }
int Lua_ltrim(lua_State* L) { return trim_with(L, true, false); }
int Lua_rtrim(lua_State* L) { return trim_with(L, false, true); }

int Lua_startswith(lua_State* L) {
    size_t text_length = 0;
    size_t prefix_length = 0;
    const char* text = luaL_checklstring(L, 1, &text_length);
    const char* prefix = luaL_checklstring(L, 2, &prefix_length);
    lua_pushboolean(L, prefix_length <= text_length && std::memcmp(text, prefix, prefix_length) == 0);
    return 1;
}

int Lua_endswith(lua_State* L) {
    size_t text_length = 0;
    size_t suffix_length = 0;
    const char* text = luaL_checklstring(L, 1, &text_length);
    const char* suffix = luaL_checklstring(L, 2, &suffix_length);
    lua_pushboolean(L, suffix_length <= text_length && std::memcmp(text + text_length - suffix_length, suffix, suffix_length) == 0);
    return 1;
}

int Lua_join(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    size_t delimiter_length = 0;
    const char* delimiter = luaL_checklstring(L, 2, &delimiter_length);
    const lua_Integer count = luaL_len(L, 1);
    std::string joined;
    for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(L, 1, i);
        size_t length = 0;
        const char* value = (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER) ? lua_tolstring(L, -1, &length) : nullptr;
        if (value != nullptr) {
            if (i > 1) {
                joined.append(delimiter, delimiter_length);
            }
            joined.append(value, length);
        }
        lua_pop(L, 1);
    }
    lua_pushlstring(L, joined.data(), joined.size());
    return 1;
}

// ---- tables ---------------------------------------------------------------

int Lua_contains(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checkany(L, 2);
    const lua_Integer count = luaL_len(L, 1);
    for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(L, 1, i);
        const bool equal = lua_compare(L, -1, 2, LUA_OPEQ) != 0;
        lua_pop(L, 1);
        if (equal) {
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

int Lua_slice(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const lua_Integer start = luaL_checkinteger(L, 2);
    const lua_Integer end = luaL_optinteger(L, 3, luaL_len(L, 1));
    lua_newtable(L);
    lua_Integer index = 1;
    for (lua_Integer i = start; i <= end; ++i) {
        lua_rawgeti(L, 1, i);
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

int Lua_keys(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_newtable(L);
    lua_Integer index = 1;
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        lua_pop(L, 1);
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, index++);
    }
    return 1;
}

int Lua_values(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_newtable(L);
    lua_Integer index = 1;
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        lua_rawseti(L, -3, index++);
    }
    return 1;
}

int Lua_merge(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_newtable(L);
    for (int source = 1; source <= 2; ++source) {
        lua_pushnil(L);
        while (lua_next(L, source) != 0) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_settable(L, 3);
        }
    }
    return 1;
}

// ---- environment ----------------------------------------------------------

int Lua_getenv(lua_State* L) {
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

int Lua_setenv(lua_State* L) {
    size_t name_length = 0;
    size_t value_length = 0;
    const char* raw_name = luaL_checklstring(L, 1, &name_length);
    const char* raw_value = luaL_checklstring(L, 2, &value_length);
    std::wstring name;
    std::wstring value;
    const bool ok = utf8_to_wide_strict(raw_name, name_length, name) && utf8_to_wide_strict(raw_value, value_length, value) && !name.empty() && SetEnvironmentVariableW(name.c_str(), value.c_str());
    lua_pushboolean(L, ok);
    return 1;
}

// All the C++ work for utils.exec. Returns -1 when the request is stored in
// host.pending and the caller must yield, else the number of values pushed. The
// function that yields (Lua_exec) holds no C++ objects: the yield longjmps out of
// its frame, and under optimisation that unwind freed memory it should not have.
int prepare_exec(lua_State* L, const char* raw, size_t length) {
    std::wstring command;
    if (!lua_path_to_wide(raw, length, command)) {
        lua_pushnil(L);
        lua_pushliteral(L, "Failed to execute command: the command line is empty or not valid text");
        return 2;
    }
    if (can_park(L)) {
        HostState& host = host_of(L);
        host.pending = YieldRequest{};
        host.pending.kind = YieldRequest::Kind::Exec;
        host.pending.command_line = std::move(command);
        return -1;
    }
    std::string output;
    int64_t exit_code = 0;
    std::wstring error;
    if (!run_process_blocking(command, 120000, output, exit_code, error)) {
        log_event(L"warn", L"lua", error.c_str());
        lua_pushnil(L);
        const std::string message = "Failed to execute command: " + wide_to_utf8(error);
        lua_pushlstring(L, message.data(), message.size());
        return 2;
    }
    lua_pushlstring(L, output.data(), output.size());
    push_number(L, static_cast<double>(exit_code));
    return 2;
}

int Lua_exec(lua_State* L) {
    size_t length = 0;
    const char* raw = luaL_checklstring(L, 1, &length);
    const int pushed = prepare_exec(L, raw, length);
    if (pushed < 0) {
        return lua_yield(L, 0);
    }
    return pushed;
}

// ---- encoding -------------------------------------------------------------

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const char* data, size_t length) {
    std::string out;
    out.reserve((length + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < length; i += 3) {
        const unsigned v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8) | static_cast<unsigned char>(data[i + 2]);
        out.push_back(kBase64[(v >> 18) & 0x3F]);
        out.push_back(kBase64[(v >> 12) & 0x3F]);
        out.push_back(kBase64[(v >> 6) & 0x3F]);
        out.push_back(kBase64[v & 0x3F]);
    }
    if (i < length) {
        unsigned v = static_cast<unsigned char>(data[i]) << 16;
        if (i + 1 < length) {
            v |= static_cast<unsigned char>(data[i + 1]) << 8;
        }
        out.push_back(kBase64[(v >> 18) & 0x3F]);
        out.push_back(kBase64[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < length ? kBase64[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

// Same rule as Millennium: stop at the first character outside the alphabet
// ('=' padding, whitespace, anything else). What was decoded so far is returned.
std::string base64_decode(const char* data, size_t length) {
    static const std::vector<int> table = [] {
        std::vector<int> t(256, -1);
        for (int i = 0; i < 64; ++i) {
            t[static_cast<unsigned char>(kBase64[i])] = i;
        }
        return t;
    }();
    std::string out;
    out.reserve(length / 4 * 3);
    unsigned value = 0;
    int bits = -8;
    for (size_t i = 0; i < length; ++i) {
        const int digit = table[static_cast<unsigned char>(data[i])];
        if (digit < 0) {
            break;
        }
        value = (value << 6) + static_cast<unsigned>(digit);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

int Lua_base64_encode(lua_State* L) {
    size_t length = 0;
    const char* data = luaL_checklstring(L, 1, &length);
    const std::string encoded = base64_encode(data, length);
    lua_pushlstring(L, encoded.data(), encoded.size());
    return 1;
}

int Lua_base64_decode(lua_State* L) {
    size_t length = 0;
    const char* data = luaL_checklstring(L, 1, &length);
    const std::string decoded = base64_decode(data, length);
    lua_pushlstring(L, decoded.data(), decoded.size());
    return 1;
}

int Lua_url_encode(lua_State* L) {
    size_t length = 0;
    const char* input = luaL_checklstring(L, 1, &length);
    std::string out;
    char piece[4];
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            std::snprintf(piece, sizeof(piece), "%%%02x", c);  // lower-case, like Millennium
            out.append(piece, 3);
        }
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int Lua_url_decode(lua_State* L) {
    size_t length = 0;
    const char* input = luaL_checklstring(L, 1, &length);
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        if (input[i] == '%' && i + 2 < length) {
            const int high = hex(input[i + 1]);
            const int low = hex(input[i + 2]);
            if (high != -1 && low != -1) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        out.push_back(input[i]);
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int Lua_hex_encode(lua_State* L) {
    size_t length = 0;
    const char* input = luaL_checklstring(L, 1, &length);
    std::string out;
    out.reserve(length * 2);
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0F]);
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int Lua_to_hex(lua_State* L) {
    const lua_Integer value = luaL_checkinteger(L, 1);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    lua_pushstring(L, buffer);
    return 1;
}

int Lua_from_hex(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    std::istringstream stream(text);
    long long value = 0;
    stream >> std::hex >> value;
    push_number(L, static_cast<double>(value));
    return 1;
}

int Lua_uuid(lua_State* L) {
    static std::mt19937_64 gen{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> distribution;
    uint64_t data1 = distribution(gen);
    uint64_t data2 = distribution(gen);
    data1 = (data1 & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    data2 = (data2 & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char text[40];
    std::snprintf(text, sizeof(text), "%08x-%04x-%04x-%04x-%012llx", static_cast<uint32_t>(data1 >> 32), static_cast<uint16_t>(data1 >> 16), static_cast<uint16_t>(data1),
                  static_cast<uint16_t>(data2 >> 48), static_cast<unsigned long long>(data2 & 0x0000FFFFFFFFFFFFULL));
    lua_pushstring(L, text);
    return 1;
}

// MSVC's std::hash<std::string> is 64-bit FNV-1a over the bytes. It is written
// out here so the value does not depend on a standard library release. The
// golden test (host tests) checks it against ids saved by Millennium.
uint64_t fnv1a_64(const char* data, size_t length) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < length; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

int Lua_hash(lua_State* L) {
    // Millennium: lua_pushstring-style C string, so the hash stops at a NUL.
    const char* text = luaL_checkstring(L, 1);
    const uint64_t hash = fnv1a_64(text, std::strlen(text));
    push_number(L, static_cast<double>(static_cast<int64_t>(hash)));
    return 1;
}

// ---- file i/o -------------------------------------------------------------

int Lua_read_file(lua_State* L) {
    size_t length = 0;
    const char* raw = luaL_checklstring(L, 1, &length);
    std::wstring path;
    std::string content;
    std::wstring error;
    if (!lua_path_to_wide(raw, length, path) || !read_whole_file(path, content, error)) {
        lua_pushnil(L);
        lua_pushliteral(L, "Failed to open file");
        return 2;
    }
    lua_pushlstring(L, content.data(), content.size());
    return 1;
}

int write_with(lua_State* L, bool append) {
    size_t path_length = 0;
    size_t content_length = 0;
    const char* raw = luaL_checklstring(L, 1, &path_length);
    const char* content = luaL_checklstring(L, 2, &content_length);
    std::wstring path;
    if (!lua_path_to_wide(raw, path_length, path)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, append ? "Failed to open file for appending" : "Failed to open file for writing");
        return 2;
    }
    const DWORD access = append ? (FILE_APPEND_DATA | SYNCHRONIZE) : GENERIC_WRITE;
    const DWORD disposition = append ? OPEN_ALWAYS : CREATE_ALWAYS;
    HANDLE handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, append ? "Failed to open file for appending" : "Failed to open file for writing");
        return 2;
    }
    bool ok = true;
    size_t done = 0;
    while (done < content_length) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(content_length - done, 1u << 20));
        DWORD wrote = 0;
        if (!WriteFile(handle, content + done, chunk, &wrote, nullptr) || wrote == 0) {
            ok = false;
            break;
        }
        done += wrote;
    }
    CloseHandle(handle);
    lua_pushboolean(L, ok);
    return 1;
}

int Lua_write_file(lua_State* L) { return write_with(L, false); }
int Lua_append_file(lua_State* L) { return write_with(L, true); }

int Lua_get_backend_path(lua_State* L) {
    HostState& host = host_of(L);
    lua_pushlstring(L, host.backend_dir_utf8.data(), host.backend_dir_utf8.size());
    return 1;
}

const luaL_Reg kUtilsFunctions[] = {
    DOOMNET_LUA_FN(sleep),
    DOOMNET_LUA_FN(time),
    DOOMNET_LUA_FN(time_ms),
    DOOMNET_LUA_FN(time_micro),
    DOOMNET_LUA_FN(round),
    DOOMNET_LUA_FN(clamp),
    DOOMNET_LUA_FN(sign),
    DOOMNET_LUA_FN(lerp),
    DOOMNET_LUA_FN(random_int),
    DOOMNET_LUA_FN(random_float),
    DOOMNET_LUA_FN(random_seed),
    DOOMNET_LUA_FN(split),
    DOOMNET_LUA_FN(trim),
    DOOMNET_LUA_FN(ltrim),
    DOOMNET_LUA_FN(rtrim),
    DOOMNET_LUA_FN(startswith),
    DOOMNET_LUA_FN(endswith),
    DOOMNET_LUA_FN(join),
    DOOMNET_LUA_FN(contains),
    DOOMNET_LUA_FN(slice),
    DOOMNET_LUA_FN(keys),
    DOOMNET_LUA_FN(values),
    DOOMNET_LUA_FN(merge),
    DOOMNET_LUA_FN(getenv),
    DOOMNET_LUA_FN(setenv),
    DOOMNET_LUA_FN(exec),
    DOOMNET_LUA_FN(base64_encode),
    DOOMNET_LUA_FN(base64_decode),
    DOOMNET_LUA_FN(url_encode),
    DOOMNET_LUA_FN(url_decode),
    DOOMNET_LUA_FN(hex_encode),
    DOOMNET_LUA_FN(to_hex),
    DOOMNET_LUA_FN(from_hex),
    DOOMNET_LUA_FN(uuid),
    DOOMNET_LUA_FN(hash),
    DOOMNET_LUA_FN(read_file),
    DOOMNET_LUA_FN(write_file),
    DOOMNET_LUA_FN(append_file),
    DOOMNET_LUA_FN(get_backend_path),
    {nullptr, nullptr},
};

int luaopen_host_utils(lua_State* L) {
    luaL_newlib(L, kUtilsFunctions);
    return 1;
}

}  // namespace

void open_host_utils(lua_State* L) {
    luaL_requiref(L, "utils", luaopen_host_utils, 0);
    lua_pop(L, 1);
}

}  // namespace doomnet
