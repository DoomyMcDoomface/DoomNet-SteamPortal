#include "pipe_session.h"

#include "json_wire.h"
#include "log.h"
#include "page_bridge.h"
#include "skin_page.h"
#include "steam_discover.h"
#include "text.h"

#include <windows.h>

#include <cstring>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace doomnet {

namespace {

struct EvalWait {
    std::mutex mu;
    std::string expression;
    bool await_promise = false;
    bool prefer_display = false;
    std::string result;
    std::wstring error;
    bool ok = false;
    bool abandoned = false;
    HANDLE done = nullptr;
    ~EvalWait() {
        if (done != nullptr) {
            CloseHandle(done);
        }
    }
};

std::string value_text(const Json& value) {
    switch (value.type) {
        case Json::Type::String:
            return value.s;
        case Json::Type::Bool:
            return value.b ? "true" : "false";
        case Json::Type::Number:
            return std::to_string(value.n);
        case Json::Type::Null:
            return "null";
        default:
            return {};
    }
}

bool remote_text(const std::string& reply, std::string& text, std::wstring& error) {
    Json root;
    std::string parse_error;
    if (!json_parse(reply, root, parse_error)) {
        error = L"The page reply was not JSON.";
        return false;
    }
    if (const Json* failure = root.find("error")) {
        const std::string message = failure->key_string("message");
        error = utf8_to_wide_lossy(message.empty() ? "the page rejected a DevTools command" : message);
        return false;
    }
    const Json* result = root.find("result");
    if (result == nullptr) {
        error = L"The page reply had no result.";
        return false;
    }
    if (const Json* details = result->find("exceptionDetails")) {
        const std::string message = details->key_string("text");
        error = utf8_to_wide_lossy(message.empty() ? "the page script threw" : message);
        return false;
    }
    const Json* remote = result->find("result");
    if (remote == nullptr) {
        error = L"The page reply had no result value.";
        return false;
    }
    if (const Json* inner = remote->find("value")) {
        text = value_text(*inner);
        return true;
    }
    if (remote->key_string("type") == "undefined") {
        text.clear();
        return true;
    }
    error = L"The page reply had no value.";
    return false;
}

std::string art_relative(const std::string& url) {
    const std::string marker = "steam-music-player/art/";
    const size_t at = url.find(marker);
    if (at == std::string::npos) {
        return {};
    }
    std::string rel = url.substr(at + marker.size());
    const size_t cut = rel.find_first_of("?#");
    if (cut != std::string::npos) {
        rel.resize(cut);
    }
    std::string decoded;
    decoded.reserve(rel.size());
    for (size_t i = 0; i < rel.size(); ++i) {
        if (rel[i] == '%' && i + 2 < rel.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') {
                    return c - '0';
                }
                if (c >= 'a' && c <= 'f') {
                    return c - 'a' + 10;
                }
                if (c >= 'A' && c <= 'F') {
                    return c - 'A' + 10;
                }
                return -1;
            };
            const int hi = hex(rel[i + 1]);
            const int lo = hex(rel[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(rel[i]);
    }
    if (decoded.empty() || decoded.find("..") != std::string::npos || decoded.find('\\') != std::string::npos || decoded.find(':') != std::string::npos) {
        return {};
    }
    return decoded;
}

std::wstring data_art_root() {
    wchar_t local_app[MAX_PATH] = {};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app, MAX_PATH);
    if (count == 0 || count >= MAX_PATH) {
        return {};
    }
    return std::wstring(local_app) + L"\\DoomNet\\player\\backend\\data";
}

std::wstring steam_art_root() {
    std::wstring steam;
    if (!discover_steam_path(steam)) {
        return {};
    }
    return steam + L"\\steamui\\steam-music-player\\art";
}

bool read_cover_bytes(const std::wstring& path, std::string& out) {
    out.clear();
    if (path.empty() || path.find(L"..") != std::wstring::npos) {
        return false;
    }
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    constexpr long long kMaxCoverBytes = 6ll * 1024 * 1024;
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 32 || size.QuadPart > kMaxCoverBytes) {
        CloseHandle(handle);
        return false;
    }
    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(handle, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(handle);
    if (!ok || read != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

std::string cover_base64(const std::string& bytes) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned value = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) | static_cast<unsigned char>(bytes[i + 2]);
        out.push_back(kAlphabet[(value >> 18) & 0x3F]);
        out.push_back(kAlphabet[(value >> 12) & 0x3F]);
        out.push_back(kAlphabet[(value >> 6) & 0x3F]);
        out.push_back(kAlphabet[value & 0x3F]);
    }
    if (i < bytes.size()) {
        unsigned value = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) {
            value |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        }
        out.push_back(kAlphabet[(value >> 18) & 0x3F]);
        out.push_back(kAlphabet[(value >> 12) & 0x3F]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(value >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

}  // namespace

std::wstring shared_cover_path(const std::string& art_url) {
    const std::string rel = art_relative(art_url);
    if (rel.empty()) {
        return {};
    }
    const std::wstring wide = utf8_to_wide_lossy(rel);
    const std::wstring data = data_art_root();
    const std::wstring published = steam_art_root();
    std::vector<std::wstring> candidates;
    if (rel.rfind("albums/", 0) == 0) {
        const std::wstring name = wide.substr(7);
        if (name.empty() || name.find(L'/') != std::wstring::npos || name.find(L'\\') != std::wstring::npos) {
            return {};
        }
        if (!data.empty()) {
            candidates.push_back(data + L"\\art_jpeg\\albums\\" + name);
        }
        if (!published.empty()) {
            candidates.push_back(published + L"\\albums\\" + name);
        }
    } else {
        if (wide.find(L'/') != std::wstring::npos || wide.find(L'\\') != std::wstring::npos) {
            return {};
        }
        if (!published.empty()) {
            candidates.push_back(published + L"\\" + wide);
        }
        if (!data.empty()) {
            candidates.push_back(data + L"\\art_jpeg\\" + wide);
        }
    }
    for (const std::wstring& path : candidates) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return path;
        }
    }
    return {};
}

std::atomic<int> g_patched_web_documents{0};

int patched_web_documents() {
    return g_patched_web_documents.load();
}

bool enable_store_fetch(CdpPipe&) {
    if (!skin_tags_ready()) {
        return false;
    }
    // Do not pause store responses. Copying the document body held the
    // browser, and the library tab waited on that hold.
    return true;
}

bool json_base64_field(const std::string& json, std::string& encoded) {
    const std::string key = "\"base64\"";
    size_t at = json.find(key);
    if (at == std::string::npos) {
        return false;
    }
    at = json.find('"', at + key.size());
    if (at == std::string::npos) {
        return false;
    }
    encoded.clear();
    for (++at; at < json.size() && json[at] != '"'; ++at) {
        if (json[at] == '\\' && at + 1 < json.size()) {
            encoded.push_back(json[++at]);
            continue;
        }
        encoded.push_back(json[at]);
    }
    return encoded.size() >= 32;
}

// `body` is the base64 image the store page is asking for. Album files are the
// same JPEGs the library loads. A per-track cache file stores that base64 already.
bool cover_transfer(const std::string& url, std::string& body, std::string& mime, std::wstring& source) {
    body.clear();
    mime = "image/jpeg";
    source = shared_cover_path(url);
    if (!source.empty()) {
        std::string bytes;
        if (!read_cover_bytes(source, bytes)) {
            source.clear();
            return false;
        }
        if (source.size() >= 4 && source.compare(source.size() - 4, 4, L".png") == 0) {
            mime = "image/png";
        }
        body = cover_base64(bytes);
        return !body.empty();
    }
    const std::string rel = art_relative(url);
    if (rel.empty() || rel.rfind("albums/", 0) == 0 || rel.find('/') != std::string::npos) {
        return false;
    }
    std::string id = rel;
    if (id.size() > 4 && (id.compare(id.size() - 4, 4, ".jpg") == 0 || id.compare(id.size() - 4, 4, ".png") == 0)) {
        if (id.compare(id.size() - 4, 4, ".png") == 0) {
            mime = "image/png";
        }
        id.resize(id.size() - 4);
    }
    const std::wstring data = data_art_root();
    if (data.empty() || id.empty()) {
        return false;
    }
    source = data + L"\\art\\" + utf8_to_wide_lossy(id) + L".json";
    std::string json;
    if (!read_cover_bytes(source, json) || !json_base64_field(json, body)) {
        source.clear();
        body.clear();
        return false;
    }
    if (json.find("image/png") != std::string::npos || json.find("image\\/png") != std::string::npos) {
        mime = "image/png";
    }
    return true;
}

struct SteamPage::Impl {
    CdpPipe pipe;
    Bus* bus = nullptr;
    BrowserThread* browser = nullptr;
    std::string primary_session;
    std::string display_session;
    std::vector<std::string> sessions;
    std::vector<std::string> attached_targets;
    std::unordered_map<uint64_t, std::vector<std::string>> call_sessions;
    struct SharedPoll {
        uint64_t leader = 0;
        std::vector<uint64_t> waiters;
        bool noted = false;
    };
    std::unordered_map<std::string, SharedPoll> shared_polls;

    void remember_call_session(uint64_t id, const std::string& session_id) {
        const std::string chosen = session_id.empty() ? primary_session : session_id;
        std::vector<std::string>& list = call_sessions[id];
        for (const std::string& seen : list) {
            if (seen == chosen) {
                return;
            }
        }
        list.push_back(chosen);
    }
    std::mutex mutex;
    std::vector<ResolveMsg> settles;
    DWORD settle_quiet_until = 0;
    std::vector<FrontendMsg> fronts;
    std::vector<std::shared_ptr<EvalWait>> evals;
    HANDLE wake = nullptr;
    std::atomic<bool> stop{false};
    std::thread thread;

    ~Impl() { shutdown(); }

    void shutdown() {
        stop = true;
        if (wake != nullptr) {
            SetEvent(wake);
        }
        if (thread.joinable()) {
            thread.join();
        }
        pipe.close();
        if (wake != nullptr) {
            CloseHandle(wake);
            wake = nullptr;
        }
    }

    bool known_session(const std::string& session) const {
        for (const std::string& item : sessions) {
            if (item == session) {
                return true;
            }
        }
        return false;
    }

    void note_session(const std::string& session) {
        if (!session.empty() && !known_session(session)) {
            sessions.push_back(session);
        }
    }

    void enqueue_settle(ResolveMsg message) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (ResolveMsg& existing : settles) {
                if (existing.call_id == message.call_id) {
                    existing = std::move(message);
                    SetEvent(wake);
                    return;
                }
            }
            settles.push_back(std::move(message));
        }
        SetEvent(wake);
    }

    // A play reply leaves on its own turn. Player-state and duck replies stay
    // queued, a few at a time, so a song is not stuck behind a poll the page
    // already replaced. The page stops waiting for the staged file after 8 seconds.
    std::vector<ResolveMsg> take_settles() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<ResolveMsg> batch;
        std::vector<ResolveMsg> play;
        std::vector<ResolveMsg> large;
        std::vector<ResolveMsg> jump;
        std::vector<ResolveMsg> rest;
        for (ResolveMsg& message : settles) {
            if (message.playback && message.json.size() > 65536) {
                large.push_back(std::move(message));
            } else if (message.playback) {
                play.push_back(std::move(message));
            } else if (message.urgent) {
                jump.push_back(std::move(message));
            } else {
                rest.push_back(std::move(message));
            }
        }
        if (rest.size() > 8) {
            const size_t stale = rest.size() - 8;
            log_event(L"info", L"cdp", (L"Dropped " + std::to_wstring(stale) + L" stale page replies so the overlay can load the library and settings.").c_str());
            rest.erase(rest.begin(), rest.begin() + static_cast<std::vector<ResolveMsg>::difference_type>(stale));
        }
        if (large.size() > 1) {
            size_t newest = 0;
            for (size_t i = 1; i < large.size(); ++i) {
                if (large[i].call_id > large[newest].call_id) {
                    newest = i;
                }
            }
            log_event(L"info", L"cdp", (L"Dropped " + std::to_wstring(large.size() - 1) + L" old audio replies so a play call can reach the page.").c_str());
            ResolveMsg keep = std::move(large[newest]);
            large.clear();
            large.push_back(std::move(keep));
        }
        for (ResolveMsg& message : play) {
            batch.push_back(std::move(message));
        }
        const bool hold_large = !batch.empty() && !large.empty();
        if (!hold_large && !large.empty()) {
            batch.push_back(std::move(large.front()));
            large.clear();
        }
        const bool playback_now = !batch.empty();
        const bool room = batch.empty() || batch.back().json.size() <= 65536;
        if (!playback_now && room) {
            const size_t jumps = jump.size() < 4 ? jump.size() : 4;
            for (size_t i = 0; i < jumps; ++i) {
                batch.push_back(std::move(jump[i]));
            }
            if (jumps > 0) {
                jump.erase(jump.begin(), jump.begin() + static_cast<std::vector<ResolveMsg>::difference_type>(jumps));
            }
            const size_t ordinary = rest.size() < 2 ? rest.size() : 2;
            for (size_t i = 0; i < ordinary; ++i) {
                batch.push_back(std::move(rest[i]));
            }
            if (ordinary > 0) {
                rest.erase(rest.begin(), rest.begin() + static_cast<std::vector<ResolveMsg>::difference_type>(ordinary));
            }
        }
        settles.clear();
        for (ResolveMsg& message : large) {
            settles.push_back(std::move(message));
        }
        for (ResolveMsg& message : jump) {
            settles.push_back(std::move(message));
        }
        for (ResolveMsg& message : rest) {
            settles.push_back(std::move(message));
        }
        return batch;
    }

    void enqueue_front(FrontendMsg message) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            fronts.push_back(std::move(message));
        }
        SetEvent(wake);
    }

    void enqueue_eval(std::shared_ptr<EvalWait> wait) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            evals.push_back(wait);
        }
        SetEvent(wake);
    }

    bool write_call(const std::string& session, const std::string& method, const std::string& params, std::string& reply, std::wstring& error, unsigned long timeout_ms = 15000) {
        if (!pipe.call_in_session(session, method, params, reply, error, timeout_ms)) {
            return false;
        }
        drain_queued();
        return true;
    }

    void drain_queued() {
        for (int i = 0; i < 32; ++i) {
            std::string incoming;
            std::wstring error;
            if (!pipe.poll(incoming, error)) {
                if (!error.empty()) {
                    log_event(L"warn", L"cdp", error.c_str());
                }
                return;
            }
            handle_message(incoming);
        }
        flush_pending();
    }

    void install_session(const std::string& session) {
        std::string reply;
        std::wstring error;
        const std::string script = boot_script_for_page();
        if (!write_call(session, "Runtime.enable", "{}", reply, error)) {
            log_event(L"warn", L"inject", (L"Could not prepare page session '" + utf8_to_wide_lossy(session) + L"'. " + error).c_str());
            return;
        }
        if (!write_call(session, "Runtime.runIfWaitingForDebugger", "{}", reply, error)) {
            log_event(L"warn", L"cdp", (L"Could not resume session '" + utf8_to_wide_lossy(session) + L"'. " + error).c_str());
        }
        if (!write_call(session, "Page.enable", "{}", reply, error)) {
            log_event(L"warn", L"inject", (L"Could not prepare page session '" + utf8_to_wide_lossy(session) + L"'. " + error).c_str());
            return;
        }
        if (!write_call(session, "Page.setBypassCSP", "{\"enabled\":true}", reply, error)) {
            log_event(L"warn", L"cdp", (L"Could not bypass CSP for session '" + utf8_to_wide_lossy(session) + L"'. " + error).c_str());
        }
        if (!write_call(session, "Runtime.addBinding", "{\"name\":\"doomnet\"}", reply, error)) {
            log_event(L"warn", L"inject", (L"Could not prepare page session '" + utf8_to_wide_lossy(session) + L"'. " + error).c_str());
            return;
        }
        const std::string add_script = std::string("{\"source\":") + json_quote(script) + "}";
        const std::string evaluate = std::string("{\"expression\":") + json_quote(script) + ",\"returnByValue\":true}";
        if (!write_call(session, "Page.addScriptToEvaluateOnNewDocument", add_script, reply, error) || !write_call(session, "Runtime.evaluate", evaluate, reply, error)) {
            log_event(L"warn", L"inject", (L"Could not install the boot script in session '" + utf8_to_wide_lossy(session) + L"'. " + error).c_str());
            return;
        }
        note_session(session);
        log_event(L"info", L"inject", (L"The boot script is installed in session '" + utf8_to_wide_lossy(session) + L"'.").c_str());
        std::string title;
        std::wstring title_error;
        if (write_call(session, "Runtime.evaluate", "{\"expression\":\"(document.title||'')+' '+String(location.href||'')\",\"returnByValue\":true}", reply, title_error) && remote_text(reply, title, title_error)) {
            if (title.size() > 180) {
                title.resize(180);
            }
            log_event(L"info", L"inject", (L"Page session '" + utf8_to_wide_lossy(session) + L"' is '" + utf8_to_wide_lossy(title) + L"'.").c_str());
            const bool main_window = title.rfind("Steam https://", 0) == 0 || title.rfind("Steam http://", 0) == 0 || title.rfind("Steam Games List ", 0) == 0;
            if (main_window) {
                display_session = session;
                log_event(L"info", L"inject", (L"Skin checks will use session '" + utf8_to_wide_lossy(session) + L"' because its title is Steam.").c_str());
            }
        }
    }

    std::vector<std::string> resumed;

    void resume_session(const std::string& session) {
        if (session.empty()) {
            return;
        }
        for (const std::string& seen : resumed) {
            if (seen == session) {
                return;
            }
        }
        resumed.push_back(session);
        std::string reply;
        std::wstring error;
        pipe.call_in_session(session, "Runtime.enable", "{}", reply, error);
        pipe.call_in_session(session, "Runtime.runIfWaitingForDebugger", "{}", reply, error);
        log_event(L"info", L"cdp", (L"Resumed DevTools session '" + utf8_to_wide_lossy(session) + L"'.").c_str());
    }
    std::vector<std::string> pending_targets;
    std::vector<std::string> pending_sessions;

    void queue_target(const std::string& target_id) {
        if (target_id.empty()) {
            return;
        }
        for (const std::string& seen : attached_targets) {
            if (seen == target_id) {
                return;
            }
        }
        for (const std::string& queued : pending_targets) {
            if (queued == target_id) {
                return;
            }
        }
        pending_targets.push_back(target_id);
    }

    void queue_session(const std::string& session) {
        if (session.empty() || known_session(session)) {
            return;
        }
        for (const std::string& queued : pending_sessions) {
            if (queued == session) {
                return;
            }
        }
        pending_sessions.push_back(session);
    }

    void attach_target(const std::string& target_id) {
        log_event(L"info", L"inject", (L"Attaching to page target '" + utf8_to_wide_lossy(target_id) + L"'.").c_str());
        std::string reply;
        std::wstring error;
        const std::string params = std::string("{\"targetId\":") + json_quote(target_id) + ",\"flatten\":true}";
        if (!pipe.call("Target.attachToTarget", params, reply, error)) {
            log_event(L"warn", L"inject", error.c_str());
            return;
        }
        Json root;
        std::string parse_error;
        if (!json_parse(reply, root, parse_error)) {
            return;
        }
        const Json* result = root.find("result");
        if (result != nullptr) {
            queue_session(result->key_string("sessionId"));
        }
    }

    bool installing = false;

    void flush_pending() {
        if (installing) {
            return;
        }
        installing = true;
        while (!pending_targets.empty() || !pending_sessions.empty()) {
            if (!pending_sessions.empty()) {
                const std::string session = pending_sessions.back();
                pending_sessions.pop_back();
                install_session(session);
                continue;
            }
            const std::string target_id = pending_targets.back();
            pending_targets.pop_back();
            attached_targets.push_back(target_id);
            attach_target(target_id);
        }
        installing = false;
    }

    void on_binding(const Json& root) {
        const Json* params = root.find("params");
        if (params == nullptr || params->key_string("name") != "doomnet") {
            return;
        }
        const std::string session = root.key_string("sessionId");
        Json body;
        std::string parse_error;
        if (!json_parse(params->key_string("payload"), body, parse_error)) {
            log_event(L"warn", L"cdp", (L"A page call was not JSON (" + utf8_to_wide_lossy(parse_error) + L"). It was ignored.").c_str());
            return;
        }
        const Json* id_value = body.find("id");
        if (id_value == nullptr || id_value->type != Json::Type::Number) {
            log_event(L"warn", L"cdp", L"A page call had no numeric id. It was ignored.");
            return;
        }
        const uint64_t id = static_cast<uint64_t>(id_value->n);
        const std::string method = body.key_string("method");
        const std::string plugin = body.key_string("plugin");
        if (call_sessions.find(id) != call_sessions.end()) {
            remember_call_session(id, session);
            if (method == "stage_track_for_playback" || method == "get_track_audio_chunk" || method == "release_staged_track") {
                log_event(L"info", L"cdp", (L"A duplicate '" + utf8_to_wide_lossy(method) + L"' page call " + std::to_wstring(id) + L" will be answered on every page that asked.").c_str());
            }
            return;
        }
        const Json* args = body.find("args");
        if (args == nullptr || args->type != Json::Type::Array) {
            ResolveMsg failed;
            failed.call_id = id;
            failed.ok = false;
            failed.json = "argsArray must be an array";
            remember_call_session(id, session);
            enqueue_settle(std::move(failed));
            return;
        }
        std::vector<WireValue> values;
        for (const Json& arg : args->a) {
            WireValue value;
            if (arg.type == Json::Type::String) {
                value.kind = WireValue::Kind::String;
                value.text = arg.s;
            } else if (arg.type == Json::Type::Number) {
                value.kind = WireValue::Kind::Number;
                value.number = arg.n;
            } else if (arg.type == Json::Type::Bool) {
                value.kind = WireValue::Kind::Boolean;
                value.boolean = arg.b;
            } else {
                ResolveMsg failed;
                failed.call_id = id;
                failed.ok = false;
                failed.json = "Millennium's IPC can only handle [boolean, string, number]";
                remember_call_session(id, session);
                enqueue_settle(std::move(failed));
                return;
            }
            values.push_back(std::move(value));
        }
        if (shared_poll_call(method)) {
            bool joined = false;
            bool note = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                SharedPoll& slot = shared_polls[method];
                if (slot.leader != 0) {
                    slot.waiters.push_back(id);
                    joined = true;
                    if (!slot.noted) {
                        slot.noted = true;
                        note = true;
                    }
                } else {
                    slot.leader = id;
                    slot.noted = false;
                }
            }
            if (joined) {
                remember_call_session(id, session);
                if (note) {
                    log_event(L"info", L"cdp", (L"Sharing the in-flight '" + utf8_to_wide_lossy(method) + L"' answer with later page calls so a play call is not stuck behind the poll.").c_str());
                }
                return;
            }
        }
        remember_call_session(id, session);
        const std::wstring detail = L"The page called '" + utf8_to_wide_lossy(method) + L"' (plugin '" + utf8_to_wide_lossy(plugin) + L"', call " + std::to_wstring(id) + L").";
        log_event(L"info", L"cdp", detail.c_str());
        CallMsg call;
        call.call_id = id;
        call.method = method;
        call.args = std::move(values);
        bus->to_state(std::move(call));
    }

    bool fetch_enabled = false;
    std::vector<std::string> cover_sessions;

    void enable_fetch() {
        if (fetch_enabled) {
            return;
        }
        if (!enable_store_fetch(pipe)) {
            return;
        }
        fetch_enabled = true;
    }

    void arm_cover_fetch(const std::string& session) {
        if (session.empty()) {
            return;
        }
        for (const std::string& seen : cover_sessions) {
            if (seen == session) {
                return;
            }
        }
        std::string reply;
        std::wstring error;
        const char* params = "{\"patterns\":[{\"urlPattern\":\"*steamloopback.host/steam-music-player/art/*\",\"requestStage\":\"Request\"}]}";
        if (!pipe.call_in_session(session, "Fetch.enable", params, reply, error)) {
            log_event(L"warn", L"inject", (L"Could not serve shared covers in store session '" + utf8_to_wide_lossy(session) + L"'. " + error).c_str());
            return;
        }
        cover_sessions.push_back(session);
        log_event(L"info", L"inject", (L"Store session '" + utf8_to_wide_lossy(session) + L"' will serve the same covers the library loads.").c_str());
    }

    void continue_request(const std::string& session, const std::string& request_id) {
        std::string reply;
        std::wstring error;
        const std::string params = std::string("{\"requestId\":") + json_quote(request_id) + "}";
        if (!write_call(session, "Fetch.continueRequest", params, reply, error)) {
            log_event(L"warn", L"cdp", (L"Could not release request '" + utf8_to_wide_lossy(request_id) + L"'. " + error).c_str());
        }
    }

    void fulfill_cover(const std::string& session, const std::string& request_id, const std::string& url) {
        std::string body;
        std::string mime;
        std::wstring source;
        if (!cover_transfer(url, body, mime, source)) {
            std::wstring shown = utf8_to_wide_lossy(url);
            if (shown.size() > 140) {
                shown.resize(140);
            }
            log_event(L"warn", L"inject", (L"The store asked for cover '" + shown + L"' and the shared art cache did not have it.").c_str());
            continue_request(session, request_id);
            return;
        }
        const std::string params = std::string("{\"requestId\":") + json_quote(request_id) + ",\"responseCode\":200,\"responseHeaders\":[{\"name\":\"Content-Type\",\"value\":" + json_quote(mime) + "},{\"name\":\"Access-Control-Allow-Origin\",\"value\":\"*\"}],\"body\":" + json_quote(body) + "}";
        std::string reply;
        std::wstring error;
        if (!write_call(session, "Fetch.fulfillRequest", params, reply, error)) {
            log_event(L"warn", L"inject", (L"Could not give the store cover '" + source + L"'. " + error).c_str());
            continue_request(session, request_id);
            return;
        }
        log_event(L"info", L"inject", (L"Served cover '" + source + L"' to the store page.").c_str());
    }

    void on_document(const Json& root) {
        const Json* params = root.find("params");
        const std::string request_id = params != nullptr ? params->key_string("requestId") : std::string();
        if (request_id.empty()) {
            return;
        }
        const std::string session = root.key_string("sessionId");
        const Json* request = params->find("request");
        const std::string url = request != nullptr ? request->key_string("url") : std::string();
        if (url.find("steam-music-player/art/") != std::string::npos) {
            fulfill_cover(session, request_id, url);
            return;
        }
        // Anything else must be released at once. Holding a store document froze the library tab.
        continue_request(session, request_id);
    }

    void handle_message(const std::string& incoming) {
        Json root;
        std::string parse_error;
        if (!json_parse(incoming, root, parse_error)) {
            return;
        }
        const std::string method = root.key_string("method");
        if (method == "Runtime.bindingCalled") {
            on_binding(root);
            return;
        }
        if (method == "Fetch.requestPaused") {
            on_document(root);
            return;
        }
        if (method == "Page.frameNavigated") {
            on_store_navigation(root);
            return;
        }
        if (method == "Target.targetDestroyed") {
            const Json* gone = root.find("params");
            forget_store_target(gone != nullptr ? gone->key_string("targetId") : std::string());
            return;
        }
        if (method != "Target.targetCreated" && method != "Target.targetInfoChanged" && method != "Target.attachedToTarget") {
            return;
        }
        const Json* params = root.find("params");
        const Json* info = params != nullptr ? params->find("targetInfo") : nullptr;
        if (method == "Target.attachedToTarget") {
            const std::string session = params != nullptr ? params->key_string("sessionId") : std::string();
            const std::string type = info != nullptr ? info->key_string("type") : std::string();
            const std::string url = info != nullptr ? info->key_string("url") : std::string();
            const std::string title = info != nullptr ? info->key_string("title") : std::string();
            if (steam_shell_page(type, url, title)) {
                queue_session(session);
            }
            return;
        }
        if (info != nullptr && steam_shell_page(info->key_string("type"), info->key_string("url"), info->key_string("title"))) {
            queue_target(info->key_string("targetId"));
        }
    }

    void take_shared_waiters(ResolveMsg& message) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!message.method.empty()) {
            const auto it = shared_polls.find(message.method);
            if (it != shared_polls.end() && it->second.leader == message.call_id) {
                message.also = std::move(it->second.waiters);
                shared_polls.erase(it);
            }
            return;
        }
        for (auto it = shared_polls.begin(); it != shared_polls.end();) {
            if (it->second.leader == message.call_id) {
                message.also = std::move(it->second.waiters);
                it = shared_polls.erase(it);
            } else {
                ++it;
            }
        }
    }

    void send_settle(ResolveMsg message) {
        take_shared_waiters(message);
        std::vector<uint64_t> ids;
        ids.push_back(message.call_id);
        for (uint64_t extra : message.also) {
            ids.push_back(extra);
        }
        struct Group {
            std::string session;
            std::vector<uint64_t> ids;
        };
        std::vector<Group> groups;
        const auto add_id = [&](uint64_t id, const std::string& session) {
            for (Group& group : groups) {
                if (group.session == session) {
                    group.ids.push_back(id);
                    return;
                }
            }
            groups.push_back(Group{session, {id}});
        };
        for (uint64_t id : ids) {
            std::vector<std::string> pages;
            const auto found = call_sessions.find(id);
            if (found != call_sessions.end()) {
                pages = found->second;
                call_sessions.erase(found);
            }
            if (pages.empty()) {
                pages.push_back(primary_session);
            }
            // Poll replies belong to the page that asked first. Copying every duck
            // answer onto the overlay doubles the queue, and the overlay's own
            // settings call never gets a turn.
            if (!message.urgent && pages.size() > 1) {
                pages.resize(1);
            }
            // A large audio body goes only to the Steam window. Copying it onto
            // the overlay keeps the next play reply waiting past the page's 8 second limit.
            if (message.json.size() > 65536) {
                std::vector<std::string> one;
                bool steam = false;
                for (const std::string& page : pages) {
                    if (page == primary_session) {
                        steam = true;
                        break;
                    }
                }
                one.push_back(steam ? primary_session : pages.back());
                pages.swap(one);
            } else if (pages.size() > 1) {
                std::vector<std::string> ordered;
                for (const std::string& page : pages) {
                    if (page == primary_session) {
                        ordered.insert(ordered.begin(), page);
                    } else {
                        ordered.push_back(page);
                    }
                }
                pages.swap(ordered);
            }
            for (const std::string& page : pages) {
                add_id(id, page);
            }
        }
        for (size_t i = 0; i < groups.size(); ++i) {
            if (groups[i].session == primary_session && i != 0) {
                std::swap(groups[0], groups[i]);
                break;
            }
        }
        const std::string value = message.ok ? message.json : json_quote(message.json);
        const char* ok_text = message.ok ? "true" : "false";
        bool delivered = false;
        std::wstring error;
        size_t page_count = 0;
        for (const Group& group : groups) {
            std::string expr = "(function(){var v=" + value + ";";
            for (uint64_t id : group.ids) {
                expr += "window.__doomnetSettle(" + std::to_string(id) + "," + ok_text + ",v);";
            }
            expr += "})()";
            const std::string params = std::string("{\"expression\":") + json_quote(expr) + ",\"returnByValue\":true}";
            std::string reply;
            std::wstring one_error;
            unsigned long timeout = 400;
            if (group.session == primary_session) {
                if (message.playback) {
                    timeout = message.json.size() > 65536 ? 3000 : 8000;
                } else {
                    timeout = message.json.size() > 65536 ? 3000 : 1500;
                }
            }
            if (write_call(group.session, "Runtime.evaluate", params, reply, one_error, timeout)) {
                delivered = true;
                page_count += 1;
            } else {
                error = one_error;
            }
        }
        if (!delivered) {
            settle_quiet_until = GetTickCount() + 3000;
            log_event(L"warn", L"cdp", (L"Could not settle page call " + std::to_wstring(message.call_id) + L". " + error + L" Page replies pause for 3 seconds so Steam's web helper can recover.").c_str());
            return;
        }
        const std::wstring extra = message.also.empty() ? std::wstring() : L" and " + std::to_wstring(message.also.size()) + L" later poll call(s)";
        log_event(L"info", L"cdp", (L"Settled page call " + std::to_wstring(message.call_id) + extra + (message.ok ? L"." : L" with an error.") + L" on " + std::to_wstring(page_count) + L" page(s).").c_str());
    }

    void mirror_player_call(const std::string& expr) {
        std::vector<std::string> seen;
        seen.push_back(primary_session);
        for (const StoreWatch& watch : store_watches) {
            if (!watch.session_id.empty()) {
                seen.push_back(watch.session_id);
            }
        }
        const std::string mirror = std::string("{\"expression\":") + json_quote(expr) + ",\"returnByValue\":true}";
        for (const std::string& session : sessions) {
            if (session.empty()) {
                continue;
            }
            bool already = false;
            for (const std::string& prior : seen) {
                if (prior == session) {
                    already = true;
                    break;
                }
            }
            if (already) {
                continue;
            }
            seen.push_back(session);
            std::string mirror_reply;
            std::wstring mirror_error;
            write_call(session, "Runtime.evaluate", mirror, mirror_reply, mirror_error, 400);
        }
    }

    void send_front(FrontendMsg message) {
        const std::string params_json = message.params_json.empty() ? "[]" : message.params_json;
        const std::string expr = "window.__doomnetCall(" + json_quote(message.method) + "," + params_json + ")";
        std::string reply;
        std::wstring error;
        const std::string params = std::string("{\"expression\":") + json_quote(expr) + ",\"awaitPromise\":true,\"returnByValue\":true}";
        mirror_player_call(expr);
        for (const StoreWatch& watch : store_watches) {
            if (watch.session_id.empty() || watch.session_id == primary_session) {
                continue;
            }
            const std::string mirror = std::string("{\"expression\":") + json_quote(expr) + ",\"returnByValue\":true}";
            std::string mirror_reply;
            std::wstring mirror_error;
            write_call(watch.session_id, "Runtime.evaluate", mirror, mirror_reply, mirror_error);
        }
        BrowserThread* target = browser;
        const uint64_t co_id = message.co_id;
        const std::string method = message.method;
        if (!write_call(primary_session, "Runtime.evaluate", params, reply, error)) {
            target->post([target, co_id, error] {
                target->fail_frontend(co_id, wide_to_utf8(error.empty() ? std::wstring(L"The page did not answer.") : error));
            });
            return;
        }
        Json value;
        std::string parse_error;
        if (!json_parse(reply, value, parse_error)) {
            target->post([target, co_id] {
                target->fail_frontend(co_id, "The page reply was not JSON.");
            });
            return;
        }
        const Json* result = value.find("result");
        const Json* remote = result != nullptr ? result->find("result") : nullptr;
        const Json* inner = remote != nullptr ? remote->find("value") : nullptr;
        WireValue packed;
        if (result != nullptr && result->find("exceptionDetails") != nullptr) {
            target->post([target, co_id] {
                target->fail_frontend(co_id, "the page script threw");
            });
            return;
        }
        if (inner == nullptr || inner->type != Json::Type::Object) {
            packed.kind = inner != nullptr && inner->type == Json::Type::String ? WireValue::Kind::String : WireValue::Kind::Nil;
            if (packed.kind == WireValue::Kind::String) {
                packed.text = inner->s;
            }
        } else {
            const std::string kind = inner->key_string("k");
            if (kind == "fail") {
                const std::string why = inner->key_string("e");
                target->post([target, co_id, why] {
                    target->fail_frontend(co_id, why.empty() ? "no such method on the page" : why);
                });
                return;
            }
            if (kind == "bool") {
                const Json* bit = inner->find("v");
                packed.kind = WireValue::Kind::Boolean;
                packed.boolean = bit != nullptr && bit->type == Json::Type::Bool && bit->b;
            } else if (kind == "num") {
                const Json* number = inner->find("v");
                packed.kind = WireValue::Kind::Number;
                packed.number = number != nullptr && number->type == Json::Type::Number ? number->n : 0;
            } else if (kind == "str") {
                packed.kind = WireValue::Kind::String;
                packed.text = inner->key_string("v");
            } else if (kind == "json") {
                packed.kind = WireValue::Kind::Json;
                packed.text = inner->key_string("v");
            } else {
                packed.kind = WireValue::Kind::Nil;
            }
        }
        log_event(L"info", L"cdp", (L"The page answered '" + utf8_to_wide_lossy(method) + L"'.").c_str());
        target->post([target, co_id, packed = std::move(packed)]() mutable { target->complete_frontend(co_id, std::move(packed)); });
    }

    void send_eval(std::shared_ptr<EvalWait> wait) {
        {
            std::lock_guard<std::mutex> lock(wait->mu);
            if (wait->abandoned) {
                return;
            }
        }
        const std::string params = std::string("{\"expression\":") + json_quote(wait->expression) + (wait->await_promise ? ",\"awaitPromise\":true" : "") + ",\"returnByValue\":true}";
        std::string reply;
        std::wstring call_error;
        std::string text;
        std::string session = primary_session;
        if (wait->prefer_display && !display_session.empty()) {
            session = display_session;
        }
        bool ok = write_call(session, "Runtime.evaluate", params, reply, call_error);
        if (ok && !remote_text(reply, text, call_error)) {
            ok = false;
        }
        std::lock_guard<std::mutex> lock(wait->mu);
        if (wait->abandoned) {
            return;
        }
        wait->ok = ok;
        wait->result = std::move(text);
        wait->error = std::move(call_error);
        SetEvent(wait->done);
    }

    struct StoreWatch {
        std::string target_id;
        std::string session_id;
        std::string url;
        bool armed = false;
    };

    std::vector<StoreWatch> store_watches;
    DWORD last_web_scan = 0;
    bool styling = false;
    std::atomic<bool> release_main{false};
    bool main_released = false;

    StoreWatch* find_store_target(const std::string& target_id) {
        for (StoreWatch& watch : store_watches) {
            if (watch.target_id == target_id) {
                return &watch;
            }
        }
        return nullptr;
    }

    StoreWatch* find_store_session(const std::string& session_id) {
        for (StoreWatch& watch : store_watches) {
            if (watch.session_id == session_id) {
                return &watch;
            }
        }
        return nullptr;
    }

    void forget_store_target(const std::string& target_id) {
        if (target_id.empty()) {
            return;
        }
        for (size_t i = 0; i < store_watches.size(); ++i) {
            if (store_watches[i].target_id == target_id) {
                store_watches.erase(store_watches.begin() + static_cast<long long>(i));
                log_event(L"info", L"inject", (L"The store target '" + utf8_to_wide_lossy(target_id) + L"' closed. The next one will be styled again.").c_str());
                return;
            }
        }
    }

    // addScriptToEvaluateOnNewDocument did not survive the store's own
    // reloads. Ask the live document, and put back whatever it lost.
    std::string store_document_state(const std::string& session) {
        const char* probe = "(function(){var css=!!document.getElementById('doomnet-css-0');var player=!!window.__steamMusicPlayerLoaded;return (css?'css':'nocss')+'|'+(player?'player':'noplayer');})()";
        const std::string expression = std::string("{\"expression\":") + json_quote(probe) + ",\"returnByValue\":true}";
        std::string reply;
        std::wstring error;
        if (!pipe.call_in_session(session, "Runtime.evaluate", expression, reply, error)) {
            return "nocss|noplayer";
        }
        std::string state;
        std::wstring state_error;
        if (!remote_text(reply, state, state_error)) {
            return "nocss|noplayer";
        }
        return state;
    }

    void remove_web_player(const std::string& session, const std::string& url) {
        const char* probe =
            "(function(){var panel=document.querySelector('.smp-panel');if(!panel&&!window.__steamMusicPlayerLoaded)return 'absent';"
            "if(panel&&panel.parentNode)panel.parentNode.removeChild(panel);"
            "['smp-plugin-ui','smp-plugin-css','smp-plugin-engine','smp-plugin-mix-image'].forEach(function(id){var n=document.getElementById(id);if(n&&n.parentNode)n.parentNode.removeChild(n);});"
            "try{window.__steamMusicPlayerLoaded=false;}catch(e){}"
            "return 'removed';})()";
        const std::string expression = std::string("{\"expression\":") + json_quote(probe) + ",\"returnByValue\":true}";
        std::string reply;
        std::wstring error;
        if (!pipe.call_in_session(session, "Runtime.evaluate", expression, reply, error)) {
            return;
        }
        std::string state;
        std::wstring state_error;
        if (!remote_text(reply, state, state_error) || state != "removed") {
            return;
        }
        std::wstring wide_url = utf8_to_wide_lossy(url);
        if (wide_url.size() > 140) {
            wide_url.resize(140);
        }
        log_event(L"info", L"inject", (L"Removed the music player from '" + wide_url + L"' because that page is not the SteamTunes tab or the overlay.").c_str());
    }

    bool style_store_session(const std::string& session, const std::string& url, bool arm, bool navigated) {
        if (styling || session.empty()) {
            return false;
        }
        arm_cover_fetch(session);
        remove_web_player(session, url);
        const std::string script = skin_style_script(url);
        if (script.empty()) {
            return false;
        }
        if (navigated) {
            StoreWatch* watch = find_store_session(session);
            const std::string state = store_document_state(session);
            const bool has_css = state.find("nocss") == std::string::npos && state.find("css") != std::string::npos;
            if (has_css) {
                if (watch != nullptr) {
                    watch->url = url;
                }
                std::wstring wide_url = utf8_to_wide_lossy(url);
                if (wide_url.size() > 140) {
                    wide_url.resize(140);
                }
                log_event(L"info", L"inject", (L"The store document navigated to '" + wide_url + L"' and still has its skin.").c_str());
                return true;
            }
        }
        styling = true;
        std::string reply;
        std::wstring error;
        pipe.call_in_session(session, "Page.setBypassCSP", "{\"enabled\":true}", reply, error);
        bool armed_now = false;
        if (arm) {
            const std::string source = std::string("{\"source\":") + json_quote(script) + ",\"runImmediately\":true}";
            armed_now = pipe.call_in_session(session, "Page.addScriptToEvaluateOnNewDocument", source, reply, error);
        }
        const std::string expression = std::string("{\"expression\":") + json_quote(script) + ",\"returnByValue\":true}";
        const bool ok = pipe.call_in_session(session, "Runtime.evaluate", expression, reply, error);
        styling = false;
        std::wstring wide_url = utf8_to_wide_lossy(url);
        if (wide_url.size() > 140) {
            wide_url.resize(140);
        }
        if (!ok) {
            log_event(L"warn", L"inject", (L"Could not style '" + wide_url + L"'. " + error).c_str());
            return false;
        }
        if (auto* watch = find_store_session(session)) {
            watch->url = url;
            if (armed_now) {
                watch->armed = true;
            }
        }
        g_patched_web_documents.fetch_add(1);
        if (navigated) {
            log_event(L"info", L"inject", (L"Restyled '" + wide_url + L"' after the store document navigated.").c_str());
        } else {
            log_event(L"info", L"inject", (L"Styled '" + wide_url + L"' and kept the session so the next document stays skinned.").c_str());
        }
        remove_web_player(session, url);
        return true;
    }

    void on_store_navigation(const Json& root) {
        const std::string session = root.key_string("sessionId");
        StoreWatch* watch = find_store_session(session);
        if (watch == nullptr) {
            return;
        }
        const Json* params = root.find("params");
        const Json* frame = params != nullptr ? params->find("frame") : nullptr;
        if (frame == nullptr) {
            return;
        }
        const std::string parent = frame->key_string("parentId");
        if (!parent.empty()) {
            return;
        }
        const std::string url = frame->key_string("url");
        if (!steam_web_document("page", url)) {
            return;
        }
        style_store_session(session, url, !watch->armed, true);
    }

    void request_release() {
        release_main.store(true);
        if (wake != nullptr) {
            SetEvent(wake);
        }
    }

    void paint_web_targets() {
        const DWORD now = GetTickCount();
        if (now - last_web_scan < 1000 || !skin_tags_ready() || styling) {
            return;
        }
        last_web_scan = now;
        std::string reply;
        std::wstring error;
        if (!pipe.call("Target.getTargets", "{}", reply, error)) {
            return;
        }
        Json root;
        std::string parse_error;
        if (!json_parse(reply, root, parse_error)) {
            return;
        }
        const Json* result = root.find("result");
        const Json* infos = result != nullptr ? result->find("targetInfos") : nullptr;
        if (infos == nullptr || infos->type != Json::Type::Array) {
            return;
        }
        for (const Json& info : infos->a) {
            const std::string type = info.key_string("type");
            const std::string url = info.key_string("url");
            const std::string target_id = info.key_string("targetId");
            if (target_id.empty() || !steam_web_document(type, url)) {
                continue;
            }
            StoreWatch* watch = find_store_target(target_id);
            if (watch != nullptr && watch->url == url && watch->armed) {
                continue;
            }
            std::string session = watch != nullptr ? watch->session_id : std::string();
            if (session.empty()) {
                const std::string attach = std::string("{\"targetId\":") + json_quote(target_id) + ",\"flatten\":true}";
                if (!pipe.call("Target.attachToTarget", attach, reply, error)) {
                    log_event(L"warn", L"inject", (L"Could not attach the store target '" + utf8_to_wide_lossy(target_id) + L"'. " + error).c_str());
                    continue;
                }
                Json attach_root;
                if (!json_parse(reply, attach_root, parse_error)) {
                    continue;
                }
                const Json* attach_result = attach_root.find("result");
                session = attach_result != nullptr ? attach_result->key_string("sessionId") : std::string();
                if (session.empty()) {
                    continue;
                }
                pipe.call_in_session(session, "Runtime.runIfWaitingForDebugger", "{}", reply, error);
                pipe.call_in_session(session, "Page.enable", "{}", reply, error);
                if (watch == nullptr) {
                    store_watches.push_back(StoreWatch{target_id, session, std::string(), false});
                    watch = find_store_target(target_id);
                } else {
                    watch->session_id = session;
                }
            }
            if (watch == nullptr) {
                continue;
            }
            style_store_session(session, url, !watch->armed, false);
        }
    }

    void run() {
        log_event(L"info", L"cdp", (L"Answering page calls on session '" + utf8_to_wide_lossy(primary_session) + L"'.").c_str());
        enable_fetch();
        while (!stop.load()) {
            std::vector<ResolveMsg> local_settles = take_settles();
            std::vector<FrontendMsg> local_fronts;
            std::vector<std::shared_ptr<EvalWait>> local_evals;
            {
                std::lock_guard<std::mutex> lock(mutex);
                local_fronts.swap(fronts);
                local_evals.swap(evals);
            }
            for (ResolveMsg& message : local_settles) {
                send_settle(std::move(message));
            }
            for (FrontendMsg& message : local_fronts) {
                send_front(std::move(message));
            }
            for (std::shared_ptr<EvalWait>& wait : local_evals) {
                send_eval(wait);
            }
            drain_queued();
            if (release_main.load() && !main_released) {
                std::string reply;
                std::wstring error;
                const std::string params = std::string("{\"sessionId\":") + json_quote(primary_session) + "}";
                if (pipe.call("Target.detachFromTarget", params, reply, error)) {
                    log_event(L"info", L"cdp", L"Detached the debugger from the library page so the game grid is not instrumented.");
                } else {
                    log_event(L"warn", L"cdp", (L"Could not detach the library debugger. " + error).c_str());
                }
                main_released = true;
            }
            paint_web_targets();
            if (stop.load()) {
                break;
            }
            // A play reply is sent on the next pass. Everything else waits, so a
            // full poll queue cannot keep this thread inside Runtime.evaluate
            // until the Steam page stops answering and crashes.
            bool play_waiting = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const ResolveMsg& message : settles) {
                    if (message.urgent) {
                        play_waiting = true;
                        break;
                    }
                }
            }
            if (!play_waiting) {
                const DWORD now = GetTickCount();
                const DWORD quiet = settle_quiet_until;
                if (quiet != 0 && now < quiet) {
                    WaitForSingleObject(wake, quiet - now);
                } else {
                    WaitForSingleObject(wake, 25);
                }
            }
        }
        log_event(L"info", L"cdp", L"Stopped answering page calls.");
    }
};

SteamPage::SteamPage() : impl_(std::make_unique<Impl>()) {}

SteamPage::~SteamPage() {
    stop();
}

bool SteamPage::start(CdpPipe pipe, const std::string& session_id, BrowserThread& browser, Bus& bus, std::wstring& error) {
    if (!pipe.connected() || session_id.empty()) {
        error = L"There is no page session, so page calls cannot be answered.";
        log_event(L"error", L"cdp", error.c_str());
        return false;
    }
    stop();
    impl_->pipe = std::move(pipe);
    impl_->bus = &bus;
    impl_->browser = &browser;
    impl_->primary_session = session_id;
    impl_->sessions.clear();
    impl_->sessions.push_back(session_id);
    impl_->stop = false;
    impl_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl_->wake == nullptr) {
        error = L"Could not create the page-call event. " + windows_error_text(GetLastError());
        log_event(L"error", L"cdp", error.c_str());
        return false;
    }
    browser.on_resolve = [impl = impl_.get()](ResolveMsg&& message) { impl->enqueue_settle(std::move(message)); };
    browser.on_frontend = [impl = impl_.get()](FrontendMsg&& message) { impl->enqueue_front(std::move(message)); };
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    error.clear();
    return true;
}

void SteamPage::stop() {
    if (impl_) {
        impl_->shutdown();
    }
}

void SteamPage::release_debugger() {
    if (impl_) {
        impl_->request_release();
    }
}

bool SteamPage::evaluate_display(const std::string& expression, bool await_promise, std::string& result, std::wstring& error) {
    return evaluate_impl(expression, await_promise, true, result, error);
}

bool SteamPage::evaluate(const std::string& expression, bool await_promise, std::string& result, std::wstring& error) {
    return evaluate_impl(expression, await_promise, false, result, error);
}

bool SteamPage::evaluate_impl(const std::string& expression, bool await_promise, bool prefer_display, std::string& result, std::wstring& error) {
    result.clear();
    if (!impl_ || !impl_->thread.joinable()) {
        error = L"The page session is not answering calls.";
        return false;
    }
    auto wait = std::make_shared<EvalWait>();
    wait->expression = expression;
    wait->await_promise = await_promise;
    wait->prefer_display = prefer_display;
    wait->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (wait->done == nullptr) {
        error = L"Could not wait for the page. " + windows_error_text(GetLastError());
        return false;
    }
    impl_->enqueue_eval(wait);
    const DWORD woke = WaitForSingleObject(wait->done, 20000);
    if (woke != WAIT_OBJECT_0) {
        std::lock_guard<std::mutex> lock(wait->mu);
        wait->abandoned = true;
        error = L"The page did not answer within 20 seconds.";
        return false;
    }
    std::lock_guard<std::mutex> lock(wait->mu);
    result = wait->result;
    error = wait->error;
    return wait->ok;
}

}  // namespace doomnet
