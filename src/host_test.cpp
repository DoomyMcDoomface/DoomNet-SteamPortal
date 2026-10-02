// Tests for the Lua host: the state thread, the browser thread, and the shims.
//
// Two fixtures:
//   synthetic  a tiny main.lua written here, to prove each shim and each
//              concurrency rule with a known input and a known output;
//   player     a copy of the real SteamTunes backend, to prove that main.lua
//              loads under this host and answers get_settings / get_library_info.
//
// Nothing here touches the real Steam folder. Every path is under a temp
// directory, and DOOMNET_ROOT sends the log there too.

#include "browser_thread.h"
#include "cdp_pipe.h"
#include "config.h"
#include "host_bus.h"
#include "host_config.h"
#include "log.h"
#include "page_bridge.h"
#include "pipe_session.h"
#include "resident.h"
#include "state_thread.h"
#include "text.h"

#include <windows.h>

#include <dbghelp.h>

#include <crtdbg.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fsn = std::filesystem;
using namespace doomnet;

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) {
        ++g_failed;
        std::printf("FAIL  %s\n", message.c_str());
        std::fflush(stdout);
    }
}

void section(const char* name) {
    std::printf("test  %s\n", name);
    std::fflush(stdout);
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::wstring g_root;

std::wstring make_temp_root() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    std::wstring path = std::wstring(temp) + L"doomnet-hosttest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

void write_text(const fsn::path& path, const std::string& text) {
    fsn::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_text(const fsn::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string log_text() {
    return read_text(fsn::path(g_root) / L"logs" / L"doomnet.log");
}

WireValue str(const std::string& text) {
    WireValue v;
    v.kind = WireValue::Kind::String;
    v.text = text;
    return v;
}

WireValue num(double value) {
    WireValue v;
    v.kind = WireValue::Kind::Number;
    v.number = value;
    return v;
}

// ---- rig -----------------------------------------------------------------

struct Rig {
    Bus bus;
    BrowserThread browser{bus};
    std::unique_ptr<StateThread> state;

    std::mutex mutex;
    std::condition_variable arrived;
    std::map<uint64_t, ResolveMsg> resolved;
    std::vector<uint64_t> resolve_order;
    std::atomic<uint64_t> next_call{1};
    std::atomic<uint64_t> reenter_co{0};

    explicit Rig(HostConfig config) {
        state = std::make_unique<StateThread>(bus, std::move(config));
        browser.on_resolve = [this](ResolveMsg&& message) {
            if (message.call_id == 900000) {
                // The fake page's own call finished: now the JS function it stands for returns.
                WireValue value;
                value.kind = WireValue::Kind::Boolean;
                value.boolean = true;
                browser.complete_frontend(reenter_co.load(), std::move(value));
            }
            std::lock_guard<std::mutex> lock(mutex);
            resolve_order.push_back(message.call_id);
            resolved[message.call_id] = std::move(message);
            arrived.notify_all();
        };
    }

    ~Rig() {
        browser.stop();
        state->stop();
    }

    bool start() {
        std::wstring error;
        if (!browser.start(error)) {
            check(false, "browser thread start: " + wide_to_utf8(error));
            return false;
        }
        if (!state->start(error)) {
            check(false, "state thread start: " + wide_to_utf8(error));
            return false;
        }
        return true;
    }

    uint64_t call(const std::string& method, std::vector<WireValue> args = {}) {
        CallMsg message;
        message.call_id = next_call++;
        message.method = method;
        message.args = std::move(args);
        const uint64_t id = message.call_id;
        bus.to_state(std::move(message));
        return id;
    }

    bool wait(uint64_t id, ResolveMsg& out, unsigned long timeout_ms = 15000) {
        std::unique_lock<std::mutex> lock(mutex);
        const bool got = arrived.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return resolved.count(id) != 0; });
        if (got) {
            out = resolved[id];
        }
        return got;
    }

    ResolveMsg call_wait(const std::string& method, std::vector<WireValue> args = {}, unsigned long timeout_ms = 15000) {
        const uint64_t id = call(method, std::move(args));
        ResolveMsg out;
        if (!wait(id, out, timeout_ms)) {
            check(false, "timed out waiting for " + method);
            out.ok = false;
            out.json = "timeout";
        }
        return out;
    }

    size_t order_of(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        for (size_t i = 0; i < resolve_order.size(); ++i) {
            if (resolve_order[i] == id) {
                return i;
            }
        }
        return static_cast<size_t>(-1);
    }
};

// A call's result is JSON. When the Lua function returned a string that itself
// holds JSON, the result is that string escaped once more. This undoes one layer.
std::string unescape_json_string(const std::string& json) {
    std::string out;
    size_t i = json.size() >= 2 && json.front() == '"' ? 1 : 0;
    const size_t end = json.size() >= 2 && json.front() == '"' ? json.size() - 1 : json.size();
    while (i < end) {
        const char c = json[i];
        if (c == '\\' && i + 1 < end) {
            const char n = json[i + 1];
            switch (n) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                default: out += n; break;
            }
            i += 2;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

// ---- synthetic backend ------------------------------------------------------

const char* const kSyntheticMain = R"LUA(
local millennium = require("millennium")
local utils = require("utils")
local fs = require("fs")
local json = require("json")
local logger = require("logger")

function t_ping() return "pong" end

function t_hash(s) return tostring(utils.hash(s)) end

function t_sleep(ms)
  local t0 = utils.time_ms()
  local ok, err = pcall(utils.sleep, ms)
  return json.encode({ ok = ok, err = err and tostring(err) or nil, waited = utils.time_ms() - t0 })
end

function t_join()
  local a, amsg = fs.join(nil, "x")
  local ok = pcall(fs.join, nil, "x")
  return json.encode({ nil_result = (a == nil), nil_msg = amsg, joined = fs.join("a", "b", "c.txt"), pcall_ok = ok })
end

function t_frontend(name)
  local v, err = millennium.call_frontend_method(name, { "a", 1, true })
  return json.encode({ v = v, vt = type(v), err = err })
end

function t_frontend_noargs(name)
  local v, err = millennium.call_frontend_method(name, {})
  return json.encode({ v = v, vt = type(v), err = err })
end

function t_frontend_bad()
  local ok, err = pcall(millennium.call_frontend_method, "x", { {} })
  return json.encode({ ok = ok, err = tostring(err) })
end

function t_read_chunk(path, off)
  local f, err = io.open(path, "rb")
  if not f then return json.encode({ ok = false, err = err }) end
  f:seek("set", off)
  local c = f:read(524288)
  f:close()
  return json.encode({ ok = true, n = c and #c or -1, first = c and c:byte(1) or -1, last = c and c:byte(#c) or -1 })
end

function t_read_tagstyle(path)
  local f = io.open(path, "rb")
  local head = f:read(10)
  local big = f:read(200000)
  f:close()
  return json.encode({ n = #head + #big })
end

function t_io_roundtrip(path)
  local f = assert(io.open(path, "wb"))
  f:write("line1\n", "line2\n", 42)
  f:close()
  local r = assert(io.open(path, "rb"))
  local l1 = r:read("l")
  local rest = r:read("a")
  local size = r:seek("end")
  r:close()
  local a = assert(io.open(path, "ab"))
  a:write("!")
  a:close()
  local missing, merr, mcode = io.open(path .. ".missing", "rb")
  local closed_ok, closed_err = pcall(function() return r:read(1) end)
  return json.encode({ l1 = l1, rest = rest, size = size, appended = utils.read_file(path), missing = (missing == nil), merr = merr, mcode = mcode, closed_ok = closed_ok, closed_err = tostring(closed_err), iotype = io.type(r) })
end

function t_error() error("boom") end
function t_error_table() error({ code = 7 }) end
function t_return_table() return { a = 1, b = { 2, 3 }, c = "x/y" } end
function t_number(x) return x + 1 end
function t_yield_bare() coroutine.yield() end
function t_json() return json.encode({ empty = {}, slash = "a/b", n = 1.5, big = 12345678901234 }) end
function t_numfmt() return tostring(3.0) .. "|" .. tostring(10 / 2) .. "|" .. tostring(2 ^ 53) .. "|" .. tostring(7) end
function t_sort_sleep()
  local t = { 3, 2, 1 }
  table.sort(t, function(a, b) utils.sleep(1); return a < b end)
  return json.encode(t)
end
function t_exec(cmd)
  local out, code = utils.exec(cmd)
  return json.encode({ out = out, code = code })
end
function t_exec_bad()
  local out, err = utils.exec("C:\\definitely\\not\\here.exe")
  return json.encode({ out = out, err = err })
end
function t_os()
  local rn = os.rename
  return json.encode({ execute = (os.execute == nil), exit = (os.exit == nil), getenv = os.getenv("PATH") ~= nil, unpack = (unpack ~= nil) })
end
function t_utils()
  return json.encode({
    b64 = utils.base64_encode("Man"), b64b = utils.base64_encode("Ma"), b64c = utils.base64_encode("M"), b64d = utils.base64_decode("TWFu"),
    split = utils.split("a,b,,c", ","), trim = utils.trim("  hi  "), hex = utils.hex_encode("AB"), to_hex = utils.to_hex(255),
    starts = utils.startswith("hello", "he"), ends = utils.endswith("hello", "lo"), url = utils.url_encode("a b/c"), urld = utils.url_decode("a%20b"),
    contains = utils.contains({ 1, 2, 3 }, 2), joined = utils.join({ "x", "y" }, "-"),
    uuid_len = #utils.uuid(), empty_split = #utils.split("abc", ""),
  })
end
function t_raise_paths(dir)
  -- Every path that raises a Lua error from C++ code, many times over: the
  -- unwind must leave the heap alone (this used to crash the optimised build).
  local bad = io.open(dir .. "\\badmod.lua", "wb")
  bad:write("this is not lua ((")
  bad:close()
  package.path = dir .. "\\?.lua;" .. package.path
  local seen = {}
  for i = 1, 300 do
    local ok1, e1 = pcall(fs.copy, dir .. "\\no_such_a", dir .. "\\no_such_b", true)
    local ok2, e2 = pcall(require, "no_such_module_xyz")
    local ok3, e3 = pcall(require, "badmod")
    local ok4 = pcall(print, setmetatable({}, { __tostring = function() error("tostring boom") end }))
    local ok5, e5 = pcall(os.getenv)
    local ok6, e6 = pcall(io.open, dir .. "\\x", "zz")
    local ok7 = pcall(millennium.call_frontend_method, "x", { {} })
    local ok8 = pcall(utils.sleep, "not a number")
    local f = io.open(dir .. "\\badmod.lua", "rb")
    local ok9 = pcall(f.read, f, "q")
    f:close()
    local ok10 = pcall(f.read, f, 1)
    if i == 1 then
      seen = { copy = tostring(e1), req = tostring(e2), bad = tostring(e3), print = ok4, list = tostring(e5), mode = tostring(e6), fe = ok7, sl = ok8, fmt = ok9, closed = ok10 }
    end
    local flags = { ok1, ok2, ok3, ok4, ok5, ok6, ok7, ok8, ok9, ok10 }
    for n = 1, 10 do
      if flags[n] then error("call number " .. n .. " should have raised but did not, at iteration " .. i) end
    end
  end
  return json.encode(seen)
end
function t_reenter_marker() return "reentered" end
function t_scan_like()
  -- 25 "files" of tag-style reads with no yield between them: nothing else may interleave.
  local marker = 0
  for i = 1, 25 do marker = marker + 1 end
  return tostring(marker)
end

return {
  on_load = function() millennium.ready() end,
  on_frontend_loaded = function() end,
}
)LUA";

HostConfig synthetic_config(const std::wstring& root) {
    HostConfig config;
    config.backend_dir = root + L"\\synthetic\\backend";
    config.steam_path = root + L"\\fake-steam";
    config.install_path = root;
    write_text(fsn::path(config.backend_dir) / L"main.lua", kSyntheticMain);
    return config;
}

void install_fake_page(Rig& rig) {
    rig.browser.on_frontend = [&rig](FrontendMsg&& message) {
        const uint64_t co = message.co_id;
        WireValue value;
        if (message.method == "typed.string") {
            value.kind = WireValue::Kind::String;
            value.text = "hello";
        } else if (message.method == "typed.bool") {
            value.kind = WireValue::Kind::Boolean;
            value.boolean = true;
        } else if (message.method == "typed.number") {
            value.kind = WireValue::Kind::Number;
            value.number = 42.5;
        } else if (message.method == "typed.null") {
            value.kind = WireValue::Kind::Nil;
        } else if (message.method == "typed.object") {
            value.kind = WireValue::Kind::Json;
            value.text = "{\"a\":1,\"b\":[true,false]}";
        } else if (message.method == "typed.params") {
            value.kind = WireValue::Kind::String;
            value.text = message.params_json;
        } else if (message.method == "typed.caller") {
            value.kind = WireValue::Kind::String;
            value.text = message.caller;
        } else if (message.method == "typed.fail") {
            rig.browser.fail_frontend(co, "no such method on the page");
            return;
        } else if (message.method == "Test.reenter") {
            // The JS function calls back into Lua before it returns. The state
            // thread must be free to answer, or this never completes.
            rig.reenter_co = co;
            CallMsg call;
            call.call_id = 900000;
            call.method = "t_reenter_marker";
            rig.bus.to_state(std::move(call));
            return;
        } else if (message.method == "Test.slow") {
            rig.browser.after(300, [&rig, co] {
                WireValue done;
                done.kind = WireValue::Kind::Boolean;
                done.boolean = true;
                rig.browser.complete_frontend(co, std::move(done));
            });
            return;
        } else {
            value.kind = WireValue::Kind::Nil;
        }
        rig.browser.complete_frontend(co, std::move(value));
    };
}

// ---- tests ---------------------------------------------------------------

void test_synthetic() {
    section("synthetic: host loads main.lua");
    HostConfig config = synthetic_config(g_root);
    Rig rig(config);
    install_fake_page(rig);
    if (!rig.start()) {
        return;
    }
    check(rig.state->wait_loaded(20000), "synthetic main.lua finished loading");
    check(rig.state->main_lua_loaded(), "main.lua loaded");
    check(rig.state->on_load_ok(), "on_load returned");
    check(rig.state->ready_signalled(), "millennium.ready() was called");

    section("synthetic: plain call and number handling");
    ResolveMsg r = rig.call_wait("t_ping");
    check(r.ok && r.json == "\"pong\"", "t_ping returns the JSON string \"pong\" (got " + r.json + ")");
    r = rig.call_wait("t_number", {num(41)});
    check(r.ok && r.json == "42", "t_number(41) is 42 (got " + r.json + ")");
    r = rig.call_wait("t_numfmt");
    check(r.ok && r.json == "\"3|5|9.007199254741e+15|7\"", "tostring of whole floats has no .0, like LuaJIT (got " + r.json + ")");

    section("synthetic: utils.hash golden ids from a real Millennium library");
    r = rig.call_wait("t_hash", {str("D:\\OneDrive\\Music\\07. Theatrical Exposure.mp3")});
    check(r.ok && r.json == "\"8.1536513882893e+18\"", "hash of '07. Theatrical Exposure.mp3' (got " + r.json + ")");
    r = rig.call_wait("t_hash", {str("D:\\OneDrive\\Music\\04 Courage.mp3")});
    check(r.ok && r.json == "\"-3.8526496468561e+18\"", "hash of '04 Courage.mp3' (got " + r.json + ")");
    r = rig.call_wait("t_hash", {str("")});
    check(r.ok && r.json == "\"-3.7507630343629e+18\"", "hash of the empty string is the FNV-1a offset basis (got " + r.json + ")");

    section("synthetic: fs.join(nil, ...) does not throw");
    r = rig.call_wait("t_join");
    check(r.ok && contains(r.json, "\\\"nil_result\\\":true") && contains(r.json, "\\\"pcall_ok\\\":true") && contains(r.json, "c.txt"), "fs.join(nil, x) returns nil plus a message and pcall succeeds (got " + r.json + ")");

    section("synthetic: json defaults match Millennium's module");
    r = rig.call_wait("t_json");
    check(r.ok && contains(r.json, "\\\"empty\\\":{}") && contains(r.json, R"(a\\\/b)") && contains(r.json, "\\\"n\\\":1.5") && contains(r.json, "\\\"big\\\":12345678901234"), "empty table is {}, '/' is escaped, numbers keep their digits (got " + r.json + ")");
    r = rig.call_wait("t_return_table");
    check(r.ok && contains(r.json, "\"b\":[2,3]") && contains(r.json, "\"c\":\"x\\/y\"") && contains(r.json, "\"a\":1"), "a returned table is encoded (got " + r.json + ")");

    section("synthetic: utils functions with known outputs");
    r = rig.call_wait("t_utils");
    check(r.ok, "t_utils ran");
    check(contains(r.json, "\\\"b64\\\":\\\"TWFu\\\"") && contains(r.json, "\\\"b64b\\\":\\\"TWE=\\\"") && contains(r.json, "\\\"b64c\\\":\\\"TQ==\\\"") && contains(r.json, "\\\"b64d\\\":\\\"Man\\\""), "base64 known vectors (got " + r.json + ")");
    check(contains(r.json, "\\\"hex\\\":\\\"4142\\\"") && contains(r.json, "\\\"to_hex\\\":\\\"0xff\\\"") && contains(r.json, "\\\"trim\\\":\\\"hi\\\""), "hex and trim");
    check(contains(r.json, "\\\"url\\\":\\\"a%20b%2fc\\\"") && contains(r.json, "\\\"urld\\\":\\\"a b\\\""), "url_encode is lower-case hex like Millennium (got " + r.json + ")");
    check(contains(r.json, "\\\"uuid_len\\\":36") && contains(r.json, "\\\"empty_split\\\":1") && contains(r.json, "\\\"joined\\\":\\\"x-y\\\""), "uuid length, empty delimiter, join");
    r = rig.call_wait("t_os");
    check(r.ok && contains(r.json, "\\\"execute\\\":true") && contains(r.json, "\\\"exit\\\":true") && contains(r.json, "\\\"getenv\\\":true") && contains(r.json, "\\\"unpack\\\":true"), "os.execute/os.exit removed, getenv works, unpack exists (got " + r.json + ")");

    section("synthetic: io opens non-ASCII paths and reads/writes/seeks");
    const fsn::path unicode_path = fsn::path(g_root) / L"m\u00fcsic \u65e5\u672c\u8a9e" / L"caf\u00e9 \u30c6\u30b9\u30c8.txt";
    fsn::create_directories(unicode_path.parent_path());
    r = rig.call_wait("t_io_roundtrip", {str(wide_to_utf8(unicode_path.wstring()))});
    check(r.ok, "io round trip ran on a Unicode path (got " + r.json + ")");
    check(contains(r.json, "\\\"l1\\\":\\\"line1\\\"") && contains(r.json, "\\\"rest\\\":\\\"line2\\\\n42\\\"") && contains(r.json, "\\\"size\\\":14"), "line, rest and size are right (got " + r.json + ")");
    check(contains(r.json, "\\\"appended\\\":\\\"line1\\\\nline2\\\\n42!\\\"") && contains(r.json, "\\\"missing\\\":true") && contains(r.json, "No such file or directory") && contains(r.json, "\\\"mcode\\\":2"), "append works and a missing file reports errno 2 (got " + r.json + ")");
    check(contains(r.json, "\\\"closed_ok\\\":false") && contains(r.json, "\\\"iotype\\\":\\\"closed file\\\""), "a closed file raises and io.type says so");
    check(fsn::exists(unicode_path), "the Unicode-named file exists on disk");

    section("synthetic: a Lua error fails that call only");
    r = rig.call_wait("t_error");
    check(!r.ok && contains(r.json, "boom"), "t_error rejects with the message (got " + r.json + ")");
    r = rig.call_wait("t_error_table");
    check(!r.ok, "an error value that is a table still rejects (got " + r.json + ")");
    r = rig.call_wait("t_yield_bare");
    check(!r.ok && contains(r.json, "coroutine.yield"), "a bare coroutine.yield ends that call with a clear message (got " + r.json + ")");
    r = rig.call_wait("no_such_function_here");
    check(!r.ok && contains(r.json, "no_such_function_here"), "an unknown function names it (got " + r.json + ")");
    r = rig.call_wait("bad name; drop");
    check(!r.ok, "an invalid function name is refused (got " + r.json + ")");
    r = rig.call_wait("t_ping");
    check(r.ok && r.json == "\"pong\"", "the VM is still up after all those failures");

    section("synthetic: every path that raises from C++ leaves the heap alone");
    {
        const fsn::path raise_dir = fsn::path(g_root) / L"raise";
        fsn::create_directories(raise_dir);
        r = rig.call_wait("t_raise_paths", {str(wide_to_utf8(raise_dir.wstring()))});
        check(r.ok, "300 rounds of raising calls finished (got " + r.json.substr(0, 300) + ")");
        check(contains(r.json, "No such file") || contains(r.json, "cannot") || contains(r.json, "copy"), "fs.copy(..., true) raised with a message (got " + r.json.substr(0, 400) + ")");
        check(contains(r.json, "no_such_module_xyz") && contains(r.json, "badmod"), "require reports the missing module and the bad one by name");
        check(contains(r.json, "bad argument"), "a missing argument is reported as a bad argument");
        r = rig.call_wait("t_ping");
        check(r.ok && r.json == "\"pong\"", "the VM still answers afterwards");
    }

    section("synthetic: sleep parks the coroutine (proof 1 and 5)");
    {
        const uint64_t sleeper = rig.call("t_sleep", {num(400)});
        std::vector<uint64_t> pings;
        for (int i = 0; i < 20; ++i) {
            pings.push_back(rig.call("t_ping"));
        }
        ResolveMsg sleep_result;
        check(rig.wait(sleeper, sleep_result), "the sleeping call finished");
        bool all_before = true;
        for (const uint64_t id : pings) {
            ResolveMsg p;
            check(rig.wait(id, p), "a ping finished");
            all_before = all_before && rig.order_of(id) < rig.order_of(sleeper);
        }
        check(all_before, "twenty calls made during a 400 ms sleep all finished before it did");
        check(sleep_result.ok && contains(sleep_result.json, "\\\"ok\\\":true"), "pcall(utils.sleep, ms) is true (got " + sleep_result.json + ")");
        const size_t at = sleep_result.json.find("waited");
        const double waited = at == std::string::npos ? -1 : std::atof(sleep_result.json.c_str() + at + 9);
        check(waited >= 380 && waited < 1500, "the sleep lasted about 400 ms (got " + std::to_string(waited) + ")");
    }
    r = rig.call_wait("t_sleep", {num(50)});
    check(r.ok && contains(r.json, "\\\"ok\\\":true"), "pcall(utils.sleep, 50) is true");

    section("synthetic: a sleep where Lua cannot yield still works (blocking fallback)");
    r = rig.call_wait("t_sort_sleep");
    check(r.ok && contains(r.json, "[1,2,3]"), "sleep inside a table.sort comparator sorts and returns (got " + r.json + ")");

    section("synthetic: call_frontend_method typed returns (proof 2 helpers)");
    r = rig.call_wait("t_frontend", {str("typed.string")});
    check(r.ok && contains(r.json, "\\\"v\\\":\\\"hello\\\"") && contains(r.json, "\\\"vt\\\":\\\"string\\\""), "string return (got " + r.json + ")");
    r = rig.call_wait("t_frontend", {str("typed.bool")});
    check(r.ok && contains(r.json, "\\\"v\\\":true"), "boolean return (got " + r.json + ")");
    r = rig.call_wait("t_frontend", {str("typed.number")});
    check(r.ok && contains(r.json, "\\\"v\\\":42.5"), "number return (got " + r.json + ")");
    r = rig.call_wait("t_frontend", {str("typed.null")});
    check(r.ok && contains(r.json, "\\\"vt\\\":\\\"nil\\\""), "null return is nil (got " + r.json + ")");
    r = rig.call_wait("t_frontend", {str("typed.object")});
    check(r.ok && contains(r.json, "\\\"a\\\":1") && contains(r.json, "[true,false]"), "object return becomes a table (got " + r.json + ")");
    r = rig.call_wait("t_frontend", {str("typed.fail")});
    check(r.ok && contains(r.json, "no such method on the page") && contains(r.json, "\\\"vt\\\":\\\"nil\\\""), "a failed page call returns nil plus the message (got " + r.json + ")");
    r = rig.call_wait("t_frontend", {str("typed.params")});
    check(r.ok && contains(unescape_json_string(r.json), R"("v":"[\"a\",\"1\",true]")"), "a number parameter is sent as text, like Millennium (got " + r.json + ")");
    r = rig.call_wait("t_frontend_noargs", {str("typed.params")});
    check(r.ok && contains(r.json, "\\\"v\\\":\\\"[]\\\""), "no parameters is the empty array (got " + r.json + ")");
    r = rig.call_wait("t_frontend", {str("typed.caller")});
    check(r.ok && contains(r.json, "main.lua:"), "the page call carries the Lua file and line (got " + r.json + ")");
    r = rig.call_wait("t_frontend_bad");
    check(r.ok && contains(r.json, "\\\"ok\\\":false") && contains(r.json, "boolean, string, number"), "a table parameter is refused with Millennium's message (got " + r.json + ")");

    section("synthetic: a JS function that calls back into Lua does not deadlock (proof 2)");
    r = rig.call_wait("t_frontend", {str("Test.reenter")}, 8000);
    check(r.ok && contains(r.json, "\\\"v\\\":true"), "call_frontend_method returned true after the page called back (got " + r.json + ")");
    ResolveMsg inner;
    check(rig.wait(900000, inner, 2000) && inner.ok && inner.json == "\"reentered\"", "the call the page made from inside also finished");

    section("synthetic: a slow page call does not hold up other calls");
    {
        const uint64_t slow = rig.call("t_frontend", {str("Test.slow")});
        const uint64_t quick = rig.call("t_ping");
        ResolveMsg a;
        ResolveMsg b;
        check(rig.wait(quick, a) && rig.wait(slow, b), "both finished");
        check(rig.order_of(quick) < rig.order_of(slow), "the ping did not wait for the slow page call");
    }

    section("synthetic: audio chunk read parks (ReadRange); tag-style reads do not");
    {
        const fsn::path big = fsn::path(g_root) / L"chunks" / L"track.bin";
        std::string content(2 * 1024 * 1024 + 123, '\0');
        for (size_t i = 0; i < content.size(); ++i) {
            content[i] = static_cast<char>((i * 31 + 7) & 0xFF);
        }
        write_text(big, content);
        const std::string path8 = wide_to_utf8(big.wstring());

        // The synthetic backend names its chunk reader t_read_chunk, which is not the
        // allow-listed method, so it reads on the state thread. Prove that first.
        const uint64_t before = rig.browser.reads_completed();
        r = rig.call_wait("t_read_chunk", {str(path8), num(1000)});
        check(r.ok && contains(r.json, "\\\"n\\\":524288"), "a 512 KiB read outside the allow list works (got " + r.json + ")");
        check(rig.browser.reads_completed() == before, "and it did not park (no ReadRange)");
        r = rig.call_wait("t_read_tagstyle", {str(path8)});
        check(r.ok && contains(r.json, "\\\"n\\\":200010"), "a tag-style read works (got " + r.json + ")");
        check(rig.browser.reads_completed() == before, "and it did not park either");
    }
    r = rig.call_wait("t_scan_like");
    check(r.ok && r.json == "\"25\"", "an uninterrupted loop returns");

    section("synthetic: utils.exec runs a program without a shell");
    {
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
        GetComputerNameW(name, &size);
        wchar_t windir[MAX_PATH]{};
        GetWindowsDirectoryW(windir, MAX_PATH);
        const std::string host_exe = wide_to_utf8(std::wstring(L"\"") + windir + L"\\System32\\hostname.exe\"");
        r = rig.call_wait("t_exec", {str(host_exe)});
        check(r.ok && contains(r.json, wide_to_utf8(name)) && contains(r.json, "\\\"code\\\":0"), "hostname.exe output and exit code 0 (got " + r.json + ")");
        r = rig.call_wait("t_exec", {str(wide_to_utf8(std::wstring(L"\"") + windir + L"\\System32\\whoami.exe\" /nosuchswitch"))});
        check(r.ok && !contains(r.json, "\\\"code\\\":0"), "a non-zero exit code is reported (got " + r.json + ")");
        r = rig.call_wait("t_exec_bad");
        check(r.ok && contains(r.json, "Failed to execute command") && contains(r.json, "here.exe"), "a missing program returns nil plus a message that names it (got " + r.json + ")");
    }

    section("synthetic: lifecycle call");
    r = rig.call_wait(kLifecycleFrontendLoaded);
    check(r.ok, "on_frontend_loaded ran (got " + r.json + ")");

    section("synthetic: a burst of calls runs in order");
    {
        std::vector<uint64_t> ids;
        for (int i = 0; i < 300; ++i) {
            ids.push_back(rig.call("t_number", {num(i)}));
        }
        bool in_order = true;
        bool values = true;
        size_t last = 0;
        for (size_t i = 0; i < ids.size(); ++i) {
            ResolveMsg m;
            if (!rig.wait(ids[i], m)) {
                values = false;
                break;
            }
            values = values && m.ok && m.json == std::to_string(i + 1);
            const size_t at = rig.order_of(ids[i]);
            in_order = in_order && (i == 0 || at > last);
            last = at;
        }
        check(values, "300 queued calls all returned the right value");
        check(in_order, "and finished in the order they were sent");
    }

    section("synthetic: stopping with a call parked returns promptly");
    {
        rig.call("t_sleep", {num(60000)});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto began = std::chrono::steady_clock::now();
        rig.browser.stop();
        rig.state->stop();
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
        check(took < 3000, "stop took " + std::to_string(took) + " ms with a 60 s sleep parked");
    }
}

void test_no_page_hook() {
    section("no page: call_frontend_method resumes with nil at once");
    HostConfig config = synthetic_config(g_root);
    Rig rig(config);
    if (!rig.start()) {
        return;
    }
    rig.state->wait_loaded(20000);
    const auto began = std::chrono::steady_clock::now();
    ResolveMsg r = rig.call_wait("t_frontend", {str("anything")}, 5000);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    check(r.ok && contains(r.json, "\\\"vt\\\":\\\"nil\\\""), "the call returned nil (got " + r.json + ")");
    check(took < 1500, "and did not wait (" + std::to_string(took) + " ms)");
}

void test_missing_main() {
    section("a backend with no main.lua fails loudly and still answers calls");
    HostConfig config;
    config.backend_dir = g_root + L"\\empty-backend";
    fsn::create_directories(config.backend_dir);
    config.steam_path = g_root + L"\\fake-steam";
    config.install_path = g_root;
    Rig rig(config);
    if (!rig.start()) {
        return;
    }
    check(rig.state->wait_loaded(10000), "loading finished (as a failure)");
    check(!rig.state->main_lua_loaded(), "main.lua is reported as not loaded");
    ResolveMsg r = rig.call_wait("get_settings");
    check(!r.ok && contains(r.json, "did not load"), "a call gets a clear rejection (got " + r.json + ")");
    check(contains(log_text(), "main.lua could not be read"), "the log says why");
}

// The player starts one wscript.exe supervisor under its backend\data folder.
// It must be gone before that folder is deleted, or Windows shows a "Can not
// find script file" dialog. Counts (and optionally kills) the wscript.exe
// processes whose command line names this test's own temp folder.
int reap_test_supervisors(bool kill) {
    std::wstring root = g_root;
    std::wstring escaped;
    for (const wchar_t c : root) {
        escaped += (c == L'\'') ? L"''" : std::wstring(1, c);
    }
    std::wstring script = L"$n=0; Get-CimInstance Win32_Process -Filter \"Name='wscript.exe'\" | Where-Object { $_.CommandLine -like '*" + escaped + L"*' } | ForEach-Object { $n++; ";
    if (kill) {
        script += L"Stop-Process -Id $_.ProcessId -Force; ";
    }
    script += L"}; exit $n";
    std::wstring command = L"powershell.exe -NoProfile -NonInteractive -Command \"" + script + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(0);
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
        return -1;
    }
    WaitForSingleObject(info.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return static_cast<int>(code);
}

void stop_player_supervisor(const fsn::path& jobs_dir) {
    write_text(jobs_dir / L"_supervisor_stop.flag", "1");
    int left = 1;
    for (int attempt = 0; attempt < 8 && left != 0; ++attempt) {
        left = reap_test_supervisors(false);
        if (left != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(750));
        }
    }
    if (left != 0) {
        std::printf("info  the player's supervisor did not exit after its stop flag; ending it\n");
        reap_test_supervisors(true);
    }
}

void copy_player(const fsn::path& source_backend, const fsn::path& dest_backend) {
    fsn::create_directories(dest_backend);
    for (const auto& entry : fsn::recursive_directory_iterator(source_backend)) {
        const fsn::path relative = fsn::relative(entry.path(), source_backend);
        if (!relative.empty() && *relative.begin() == L"data") {
            continue;  // never copy a developer's data into a test
        }
        if (entry.is_directory()) {
            fsn::create_directories(dest_backend / relative);
        } else if (entry.is_regular_file()) {
            fsn::copy_file(entry.path(), dest_backend / relative, fsn::copy_options::overwrite_existing);
        }
    }
}

void test_real_player() {
    section("player: the real SteamTunes backend loads under this host");
    wchar_t source_env[MAX_PATH]{};
    std::wstring source = L"D:\\Documents\\Repos\\SteamTunes";
    if (GetEnvironmentVariableW(L"DOOMNET_PLAYER_SOURCE", source_env, MAX_PATH) > 0) {
        source = source_env;
    }
    const fsn::path backend_source = fsn::path(source) / L"backend";
    if (!fsn::exists(backend_source / L"main.lua")) {
        check(false, "player source not found at " + wide_to_utf8(backend_source.wstring()) + " (set DOOMNET_PLAYER_SOURCE)");
        return;
    }
    const fsn::path player_root = fsn::path(g_root) / L"player";
    copy_player(backend_source, player_root / L"backend");
    HostConfig config;
    config.backend_dir = (player_root / L"backend").wstring();
    config.steam_path = g_root + L"\\fake-steam";
    config.install_path = g_root;
    config.post_load_lua = R"LUA(
      local json = require("json")
      function t_probe_settings()
        local raw = get_settings()
        local ok, decoded = pcall(json.decode, raw)
        local keys = {}
        if ok and type(decoded) == "table" then for k, _ in pairs(decoded) do keys[#keys + 1] = k end table.sort(keys) end
        return json.encode({ raw_type = type(raw), decoded = ok, keys = keys })
      end
    )LUA";

    Rig rig(config);
    install_fake_page(rig);
    if (!rig.start()) {
        return;
    }
    const bool loaded = rig.state->wait_loaded(120000);
    check(loaded, "main.lua and on_load finished within 120 s");
    check(rig.state->main_lua_loaded(), "main.lua loaded (see logs\\doomnet.log if not)");
    check(rig.state->on_load_ok(), "on_load completed without an error");
    check(rig.state->ready_signalled(), "the player called millennium.ready()");

    check(fsn::exists(fsn::path(g_root) / L"fake-steam" / L"steamui" / L"steam-music-player" / L"bootstrap.js"), "the player's assets were synced into the (fake) steamui folder");
    check(fsn::exists(fsn::path(g_root) / L"themes" / L"steam-music-player" / L"bootstrap.js"), "the theme-host write landed in DoomNet's own themes folder");
    check(!fsn::exists(fsn::path(g_root) / L"fake-steam" / L"millennium"), "nothing was written under <steam>\\millennium");

    ResolveMsg r = rig.call_wait("get_settings");
    check(r.ok && !r.json.empty() && r.json[0] == '"', "get_settings resolves to a JSON string (got " + r.json.substr(0, 120) + ")");
    std::printf("info  get_settings => %s\n", r.json.substr(0, 400).c_str());

    r = rig.call_wait("t_probe_settings");
    std::printf("info  settings probe => %s\n", r.json.substr(0, 600).c_str());
    check(r.ok && contains(r.json, "\\\"decoded\\\":true"), "the settings string decodes as JSON");

    r = rig.call_wait("get_library_info");
    check(r.ok && !r.json.empty() && r.json[0] == '"', "get_library_info resolves to a JSON string (got " + r.json.substr(0, 120) + ")");
    std::printf("info  get_library_info => %s\n", r.json.substr(0, 400).c_str());

    r = rig.call_wait("get_player_state");
    check(r.ok, "get_player_state resolves (got " + r.json.substr(0, 120) + ")");

    // Stop the player's process supervisor (wscript.exe) so the test leaves nothing running.
    rig.browser.stop();
    rig.state->stop();
    stop_player_supervisor(player_root / L"backend" / L"data" / L"proc_jobs");
    check(reap_test_supervisors(false) == 0, "no supervisor process is left running");
    const std::string log = log_text();
    check(!contains(log, "Lua panic"), "no Lua panic in the log");
    check(!contains(log, "caught a C++ exception"), "no C++ exception was caught by the state thread");
}

// A crash inside the host is the worst failure this test can find, and a bare
// exit code says nothing. Print the exception, the thread, and a symbolised
// stack (needs the PDB next to the exe) before the process ends.
LONG WINAPI crash_reporter(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const bool fatal = code == 0xC0000005 || code == 0xC0000374 || code == 0xC0000409 || code == 0xC00000FD || code == 0xC0000094;
    if (!fatal) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static std::atomic<bool> reported{false};
    if (reported.exchange(true)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    std::printf("CRASH exception 0x%08lX on thread %lu at %p\n", code, GetCurrentThreadId(), info->ExceptionRecord->ExceptionAddress);
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    for (int depth = 0; depth < 40; ++depth) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) {
            break;
        }
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256]{};
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        const bool has_symbol = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) != FALSE;
        const bool has_line = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_displacement, &line) != FALSE;
        std::printf("  #%02d %s%s%s:%lu\n", depth, has_symbol ? symbol->Name : "(no symbol)", has_line ? " at " : "", has_line ? line.FileName : "", has_line ? line.LineNumber : 0);
    }
    std::fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

std::wstring exe_beside_this_test(const wchar_t* name) {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring path = self;
    const size_t slash = path.find_last_of(L"\\/");
    path.resize(slash + 1);
    path += name;
    return path;
}

bool wait_for_text(const fsn::path& path, const std::string& needle, int timeout_ms) {
    const int steps = timeout_ms / 50;
    for (int i = 0; i < steps; ++i) {
        if (contains(read_text(path), needle)) {
            return true;
        }
        Sleep(50);
    }
    return contains(read_text(path), needle);
}

// Launches DoomNet.exe and kills it if it is still running when this returns.
struct ExeChild {
    PROCESS_INFORMATION info{};
    explicit ExeChild(const std::wstring& exe, const std::wstring& arguments, const std::wstring& root) {
        const std::wstring saved = g_root;
        SetEnvironmentVariableW(L"DOOMNET_ROOT", root.c_str());
        std::wstring command = L"\"" + exe + L"\" " + arguments;
        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(0);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
        SetEnvironmentVariableW(L"DOOMNET_ROOT", saved.c_str());
    }
    ~ExeChild() {
        if (info.hProcess == nullptr) {
            return;
        }
        if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
            TerminateProcess(info.hProcess, 1);
            WaitForSingleObject(info.hProcess, 3000);
        }
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }
    bool running() const { return info.hProcess != nullptr && WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT; }
    bool wait_exit(unsigned long timeout_ms, DWORD& code) const {
        if (info.hProcess == nullptr || WaitForSingleObject(info.hProcess, timeout_ms) != WAIT_OBJECT_0) {
            return false;
        }
        return GetExitCodeProcess(info.hProcess, &code) != FALSE;
    }
};

std::wstring edge_executable() {
    const wchar_t* keys[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe",
    };
    for (const wchar_t* key : keys) {
        wchar_t buffer[MAX_PATH]{};
        DWORD size = sizeof(buffer);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, nullptr, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS && buffer[0] != L'\0') {
            return buffer;
        }
    }
    const wchar_t* files[] = {
        L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
    };
    for (const wchar_t* file : files) {
        if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES) {
            return file;
        }
    }
    return {};
}

int debugging_port(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    char buffer[64]{};
    DWORD read = 0;
    ReadFile(handle, buffer, sizeof(buffer) - 1, &read, nullptr);
    CloseHandle(handle);
    return std::atoi(buffer);
}

struct EdgeSession {
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    std::wstring directory;

    ~EdgeSession() {
        if (process != nullptr) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 3000);
            CloseHandle(process);
        }
        if (job != nullptr) {
            CloseHandle(job);
        }
        if (!directory.empty()) {
            std::error_code ignored;
            fsn::remove_all(directory, ignored);
        }
    }

    bool launch(const std::wstring& exe, const std::wstring& dir, std::wstring& error) {
        directory = dir;
        fsn::create_directories(dir);
        job = CreateJobObjectW(nullptr, nullptr);
        if (job == nullptr) {
            error = L"Could not create a job for the test browser. " + windows_error_text(GetLastError());
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            error = L"Could not mark the test browser job to close with this test. " + windows_error_text(GetLastError());
            return false;
        }
        std::wstring command = L"\"" + exe + L"\" --headless=new --disable-gpu --no-first-run --no-default-browser-check --remote-allow-origins=* --remote-debugging-port=0 --user-data-dir=\"" + dir + L"\" about:blank";
        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(0);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &info)) {
            error = L"Could not start the test browser '" + exe + L"'. " + windows_error_text(GetLastError());
            return false;
        }
        if (!AssignProcessToJobObject(job, info.hProcess)) {
            error = L"Could not put the test browser in a job. " + windows_error_text(GetLastError());
            TerminateProcess(info.hProcess, 1);
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
            return false;
        }
        ResumeThread(info.hThread);
        CloseHandle(info.hThread);
        process = info.hProcess;
        return true;
    }
};

bool poll_page(PageBridge& bridge, const std::string& expression, const std::string& expected, std::string& last) {
    for (int i = 0; i < 80; ++i) {
        std::wstring error;
        if (bridge.evaluate(expression, false, last, error) && last == expected) {
            return true;
        }
        Sleep(50);
    }
    return false;
}

void test_page_bridge() {
    section("page: the boot script is a binding, not a loopback fetch");
    const std::string boot = boot_script_text();
    check(boot.find("callServerMethod") != std::string::npos, "the embedded boot script defines callServerMethod");
    check(boot.find("window !== window.top") != std::string::npos, "the embedded boot script skips iframes");
    check(boot.find("127.0.0.1") == std::string::npos, "the embedded boot script does not mention 127.0.0.1");
    check(boot.find("fetch(") == std::string::npos, "the embedded boot script does not fetch");

    section("page: callServerMethod and call_frontend_method on a local browser");
    const std::wstring edge = edge_executable();
    if (edge.empty()) {
        check(false, "Microsoft Edge was not found, so the page bridge could not be proven");
        return;
    }
    EdgeSession browser;
    std::wstring launch_error;
    const std::wstring profile = g_root + L"\\edge-profile";
    if (!browser.launch(edge, profile, launch_error)) {
        check(false, "test browser did not start: " + wide_to_utf8(launch_error));
        return;
    }
    int port = 0;
    for (int i = 0; i < 200 && port <= 0; ++i) {
        if (WaitForSingleObject(browser.process, 0) == WAIT_OBJECT_0) {
            break;
        }
        port = debugging_port(profile + L"\\DevToolsActivePort");
        if (port <= 0) {
            Sleep(100);
        }
    }
    check(port > 0, "the test browser did not publish a DevTools port");
    if (port <= 0) {
        return;
    }
    std::string websocket;
    std::wstring discover_error;
    bool discovered = false;
    for (int i = 0; i < 20 && !discovered; ++i) {
        discovered = discover_cdp_page(port, websocket, discover_error);
        if (!discovered) {
            Sleep(200);
        }
    }
    check(discovered, "no page target: " + wide_to_utf8(discover_error));
    if (!discovered) {
        return;
    }

    Rig rig(synthetic_config(g_root));
    PageBridge bridge(rig.bus);
    std::mutex seen_mu;
    std::vector<ResolveMsg> seen;
    auto rig_resolve = std::move(rig.browser.on_resolve);
    rig.browser.on_resolve = [&](ResolveMsg&& message) {
        {
            std::lock_guard<std::mutex> lock(seen_mu);
            seen.push_back(message);
        }
        ResolveMsg copy = message;
        if (rig_resolve) {
            rig_resolve(std::move(copy));
        }
        bridge.settle(std::move(message));
    };
    rig.browser.on_frontend = [&](FrontendMsg&& message) { bridge.call_page(std::move(message)); };
    if (!rig.start()) {
        return;
    }
    check(rig.state->wait_loaded(20000), "synthetic main.lua loaded beside the page");
    std::wstring open_error;
    check(bridge.open(rig.browser, websocket, open_error), "boot script installed: " + wide_to_utf8(open_error));
    if (!bridge.connected()) {
        bridge.close();
        return;
    }

    std::string value;
    std::wstring eval_error;
    const char* fixtures = "window.Test={str:function(){return 'hello';},reenter:function(){return window.Millennium.callServerMethod('SteamMusicPlayer','t_reenter_marker',[]).then(function(){return true;});}}; 'fixtures'";
    check(bridge.evaluate(fixtures, false, value, eval_error) && value == "fixtures", "test methods were defined on the page (" + value + ") " + wide_to_utf8(eval_error));

    check(bridge.evaluate("window.__out='wait'; window.Millennium.callServerMethod('SteamMusicPlayer','t_ping',[]).then(function(v){window.__out='ok:'+v;},function(e){window.__out='bad:'+(e&&e.message?e.message:e);}); 'started'", false, value, eval_error) && value == "started", "the page started a callServerMethod");
    std::string last;
    check(poll_page(bridge, "window.__out", "ok:pong", last), "callServerMethod('t_ping') resolved to pong (got " + last + ")");

    check(bridge.evaluate("window.__out='wait'; window.Millennium.callServerMethod('Other','t_ping',[]).then(function(v){window.__out='ok:'+v;},function(e){window.__out='bad:'+(e&&e.message?e.message:e);}); 'started'", false, value, eval_error), "a call with another plugin name started");
    check(poll_page(bridge, "window.__out", "ok:pong", last), "the plugin name is not part of dispatch (got " + last + ")");

    check(bridge.evaluate("window.__out='wait'; window.Millennium.callServerMethod('SteamMusicPlayer','t_ping',[{}]).then(function(){window.__out='ok';},function(e){window.__out='bad:'+(e&&e.message?e.message:e);}); 'started'", false, value, eval_error), "a bad argument call started");
    check(poll_page(bridge, "window.__out", "bad:Millennium's IPC can only handle [boolean, string, number]", last), "an object argument is rejected before Lua (got " + last + ")");

    ResolveMsg frontend = rig.call_wait("t_frontend", {str("Test.str")});
    check(frontend.ok && contains(frontend.json, "hello") && contains(frontend.json, "string"), "call_frontend_method got the page's string (got " + frontend.json + ")");
    frontend = rig.call_wait("t_frontend", {str("Test.missing")});
    check(frontend.ok && contains(frontend.json, "no such method on the page"), "a missing page method returns nil plus the message (got " + frontend.json + ")");
    frontend = rig.call_wait("t_frontend", {str("Test.reenter")}, 15000);
    check(frontend.ok && contains(frontend.json, "true"), "a page method that calls back into Lua returned true (got " + frontend.json + ")");
    bool reentered = false;
    {
        std::lock_guard<std::mutex> lock(seen_mu);
        for (const ResolveMsg& message : seen) {
            if (message.ok && message.json.find("reentered") != std::string::npos) {
                reentered = true;
            }
        }
    }
    check(reentered, "the call the page made from inside call_frontend_method finished");

    check(bridge.evaluate("(function(){var body=document.body||document.documentElement.appendChild(document.createElement('body')); var frame=document.createElement('iframe'); body.appendChild(frame); var child=frame.contentWindow; if(!child) return 'no-child'; if(child.Millennium) return 'child-has-millennium'; if(child.__doomnetTop===false) return 'child-bailed'; return 'child-absent';})()", false, value, eval_error), "iframe probe ran");
    check(value == "child-bailed" || value == "child-absent", "an iframe did not receive window.Millennium (got " + value + ")");
    check(bridge.evaluate("document.documentElement.innerHTML.indexOf('steamloopback')===-1?'clean':'injected'", false, value, eval_error) && value == "clean", "the proof page did not get a player script tag (got " + value + ")");
    check(contains(log_text(), "The page binding 'doomnet' is installed and the boot script ran."), "the log says the boot script ran");

    bridge.close();
}

void test_resident() {
    section("resident: argument parsing");
    StartupMode mode = StartupMode::Run;
    std::wstring rejected;
    check(parse_startup_mode(nullptr, mode, rejected) && mode == StartupMode::Once, "no command line is the once path");
    check(parse_startup_mode(L"   ", mode, rejected) && mode == StartupMode::Once, "blank command line is the once path");
    check(parse_startup_mode(L"  --run  ", mode, rejected) && mode == StartupMode::Run, "--run");
    check(parse_startup_mode(L"--watch", mode, rejected) && mode == StartupMode::Watch, "--watch");
    check(parse_startup_mode(L"--attach", mode, rejected) && mode == StartupMode::Attach, "--attach");
    check(!parse_startup_mode(L"--run --watch", mode, rejected), "two flags are refused");
    check(!parse_startup_mode(L"--inject", mode, rejected) && rejected == L"--inject", "an unknown flag is named");

    const std::wstring exe = exe_beside_this_test(L"DoomNet.exe");
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        check(false, "DoomNet.exe is not beside the test binary");
        return;
    }

    section("resident: --run starts the Lua host and leaves on Local\\DoomNetStop");
    {
        const std::wstring root = make_temp_root();
        const fsn::path backend = fsn::path(root) / L"player" / L"backend";
        write_text(backend / L"main.lua", "local millennium = require('millennium')\nreturn { on_load = function() millennium.ready() end }\n");
        Config config;
        config.player = true;
        config.steam_path = root + L"\\fake-steam";
        std::wstring error;
        check(save_config_file(root + L"\\config.json", config, error), "saved a player-on config for --run");
        ExeChild child(exe, L"--run", root);
        const fsn::path log = fsn::path(root) / L"logs" / L"doomnet.log";
        check(child.info.hProcess != nullptr, "--run process started");
        check(wait_for_text(log, "Running the host", 15000), "the log says the host is running");
        check(wait_for_text(log, "The player called millennium.ready()", 15000), "--run reached millennium.ready()");
        check(child.running(), "--run is still running after the player loaded");
        check(!contains(read_text(log), "Refusing to start steam.exe") && !contains(read_text(log), "CreateProcess"), "--run did not try to start a process");
        HANDLE stop = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\DoomNetStop");
        check(stop != nullptr, "Local\\DoomNetStop exists while --run is up");
        if (stop != nullptr) {
            SetEvent(stop);
            CloseHandle(stop);
        }
        DWORD code = 1;
        check(child.wait_exit(5000, code) && code == 0, "--run exited 0 after Local\\DoomNetStop");
        const std::string text = read_text(log);
        check(contains(text, "Local\\DoomNetStop was signaled") && contains(text, "The host has stopped."), "the log names the stop and the exit");
        std::error_code ignored;
        fsn::remove_all(root, ignored);
    }

    section("resident: --watch with the player off stays up until stop.flag");
    {
        const std::wstring root = make_temp_root();
        Config config;
        config.player = false;
        std::wstring error;
        check(save_config_file(root + L"\\config.json", config, error), "saved a player-off config for --watch");
        ExeChild child(exe, L"--watch", root);
        const fsn::path log = fsn::path(root) / L"logs" / L"doomnet.log";
        check(wait_for_text(log, "Watching for a Steam launch", 5000), "--watch logged that it is watching");
        check(child.running(), "--watch stayed running");
        const std::string text = read_text(log);
        check(contains(text, "The player is off in config.json, so the Lua host was not started."), "the Lua host stayed down");
        check(contains(text, "does not start steam.exe"), "the watcher says it will not start steam.exe");
        write_text(fsn::path(root) / L"stop.flag", "1");
        DWORD code = 1;
        check(child.wait_exit(5000, code) && code == 0, "--watch exited 0 after stop.flag");
        check(contains(read_text(log), "stop.flag") && contains(read_text(log), "The host has stopped."), "the log names stop.flag");
        std::error_code ignored;
        fsn::remove_all(root, ignored);
    }

    section("resident: an unknown argument exits and does not stay running");
    {
        const std::wstring root = make_temp_root();
        ExeChild child(exe, L"--inject", root);
        DWORD code = 1;
        check(child.wait_exit(2000, code) && code == 0, "--inject exited 0 within 2 seconds");
        const std::string text = read_text(fsn::path(root) / L"logs" / L"doomnet.log");
        check(contains(text, "'--inject' is not --run, --watch, or --attach"), "the refusal names the argument");
        check(!contains(text, "Running the host") && !contains(text, "Watching for a Steam launch"), "it did not become resident");
        std::error_code ignored;
        fsn::remove_all(root, ignored);
    }
}

void test_pipe_session() {
    section("pipe: a page call is answered on the debugging pipe");
    const std::wstring pipe_name = L"\\\\.\\pipe\\DoomNetPipeTest." + std::to_wstring(GetCurrentProcessId());
    HANDLE listening = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE settled = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> server_stop{false};
    std::thread server([&] {
        HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 1 << 20, 1 << 20, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            SetEvent(listening);
            return;
        }
        SetEvent(listening);
        ConnectNamedPipe(pipe, nullptr);
        std::string pending;
        bool announced = false;
        bool sent_call = false;
        while (!server_stop.load()) {
            char buffer[4096];
            DWORD read = 0;
            if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
                break;
            }
            pending.append(buffer, read);
            for (;;) {
                const size_t end = pending.find('\0');
                if (end == std::string::npos) {
                    break;
                }
                const std::string message = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (message.find("__doomnetSettle") != std::string::npos && message.find("pipe-ok") != std::string::npos) {
                    SetEvent(settled);
                }
                int id = 1;
                const char* id_at = strstr(message.c_str(), "\"id\":");
                if (id_at != nullptr) {
                    id = std::atoi(id_at + 5);
                }
                std::string framed;
                if (!announced && message.find("setDiscoverTargets") != std::string::npos) {
                    framed = "{\"method\":\"Target.targetCreated\",\"params\":{\"targetInfo\":{\"targetId\":\"T1\",\"type\":\"page\",\"title\":\"Steam\",\"url\":\"https://steamloopback.host/\"}}}";
                    framed.push_back('\0');
                    DWORD written = 0;
                    WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()), &written, nullptr);
                    announced = true;
                }
                char reply[256] = {};
                if (message.find("typeof window.Millennium") != std::string::npos) {
                    std::snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":{\"result\":{\"type\":\"boolean\",\"value\":true}}}", id);
                } else if (message.find("Target.attachToTarget") != std::string::npos) {
                    std::snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":{\"sessionId\":\"page-1\"}}", id);
                } else {
                    std::snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":{}}", id);
                }
                framed = reply;
                framed.push_back('\0');
                DWORD written = 0;
                WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()), &written, nullptr);
                if (!sent_call && message.find("typeof window.Millennium") != std::string::npos) {
                    const char* call = "{\"method\":\"Runtime.bindingCalled\",\"params\":{\"name\":\"doomnet\",\"payload\":\"{\\\"id\\\":1000000,\\\"plugin\\\":\\\"SteamMusicPlayer\\\",\\\"method\\\":\\\"doomnet_probe\\\",\\\"args\\\":[]}\"},\"sessionId\":\"page-1\"}";
                    std::string event = call;
                    event.push_back('\0');
                    WriteFile(pipe, event.data(), static_cast<DWORD>(event.size()), &written, nullptr);
                    sent_call = true;
                }
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    });
    WaitForSingleObject(listening, 5000);

    HostConfig config;
    config.backend_dir = g_root + L"\\pipe-backend";
    config.steam_path = g_root + L"\\fake-steam";
    config.install_path = g_root;
    config.post_load_lua = "function doomnet_probe()\n  return 'pipe-ok'\nend\n";
    write_text(fsn::path(config.backend_dir) / L"main.lua", "return {}\n");
    Rig rig(config);
    bool answered = false;
    if (rig.start() && rig.state->wait_loaded(20000)) {
        CdpPipe pipe;
        std::wstring error;
        std::string session;
        if (pipe.connect_name(pipe_name, error) && inject_boot_script(pipe, boot_script_text(), session, error) && session == "page-1") {
            SteamPage page;
            if (page.start(std::move(pipe), session, rig.browser, rig.bus, error)) {
                answered = WaitForSingleObject(settled, 10000) == WAIT_OBJECT_0;
                page.stop();
            } else {
                check(false, wide_to_utf8(error));
            }
        } else {
            check(false, "pipe inject failed, session='" + session + "' " + wide_to_utf8(error));
        }
    }
    check(answered, "the page call doomnet_probe settled to pipe-ok");
    server_stop = true;
    // Unblock the server if it is still waiting on a read.
    HANDLE poke = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (poke != INVALID_HANDLE_VALUE) {
        CloseHandle(poke);
    }
    if (server.joinable()) {
        server.join();
    }
    CloseHandle(listening);
    CloseHandle(settled);
}

}  // namespace

int main() {
    AddVectoredExceptionHandler(1, crash_reporter);
#ifdef _DEBUG
    // Debug builds check the whole heap on every allocation and free, so an
    // overrun is caught at the next heap call instead of much later.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_CHECK_ALWAYS_DF);
#endif
    g_root = make_temp_root();
    SetEnvironmentVariableW(L"DOOMNET_ROOT", g_root.c_str());
    std::printf("root  %s\n", wide_to_utf8(g_root).c_str());

    test_synthetic();
    test_no_page_hook();
    test_missing_main();
    test_real_player();
    test_page_bridge();
    test_pipe_session();
    test_resident();

    reap_test_supervisors(true);  // whatever happened above, leave nothing pointing at the temp folder
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed != 0) {
        std::printf("log   %s\\logs\\doomnet.log\n", wide_to_utf8(g_root).c_str());
        return 1;
    }
    std::error_code ignored;
    fsn::remove_all(g_root, ignored);
    std::printf("ok\n");
    return 0;
}
