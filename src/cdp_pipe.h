#pragma once

// NUL-delimited DevTools messages over \\.\pipe\DoomNet.cdp.<pid>.
// This is the Chrome pipe protocol, not a WebSocket. The page never sees it.

#include <cstdint>
#include <deque>
#include <string>

namespace doomnet {

class CdpPipe {
public:
    CdpPipe() = default;
    ~CdpPipe();
    CdpPipe(const CdpPipe&) = delete;
    CdpPipe& operator=(const CdpPipe&) = delete;
    CdpPipe(CdpPipe&& other) noexcept;
    CdpPipe& operator=(CdpPipe&& other) noexcept;

    // Connects to \\.\pipe\DoomNet.cdp.<pid>, retrying for a few seconds.
    bool connect_steam(unsigned long pid, std::wstring& error);
    // Connects to a full pipe path. Tests use this.
    bool connect_name(const std::wstring& pipe_name, std::wstring& error);

    // Sends one command and returns the matching reply. `params_json` is a JSON object.
    bool call(const std::string& method, const std::string& params_json, std::string& result_json, std::wstring& error, unsigned long timeout_ms = 15000);
    // Same, tagged with a flattened DevTools session id.
    bool call_in_session(const std::string& session_id, const std::string& method, const std::string& params_json, std::string& result_json, std::wstring& error, unsigned long timeout_ms = 15000);
    // The next NUL-terminated JSON message, reply or event. Queued events come first.
    bool read_raw(std::string& json, unsigned long timeout_ms, std::wstring& error);
    // The next message already read, or the next one waiting on the pipe.
    // False with an empty `error` means nothing is waiting.
    bool poll(std::string& json, std::wstring& error);

    void close();
    bool connected() const { return pipe_ != nullptr && pipe_ != reinterpret_cast<void*>(static_cast<intptr_t>(-1)); }

private:
    bool call_impl(const std::string& session_id, const std::string& method, const std::string& params_json, std::string& result_json, std::wstring& error, unsigned long timeout_ms);
    void* pipe_ = reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    unsigned long next_id_ = 1;
    // Events that arrived while a command was in flight. read_raw returns these first.
    std::deque<std::string> inbox_;
    // Bytes already taken off the pipe that belong to the next message.
    std::string pending_;

    bool read_message(std::string& json, unsigned long timeout_ms, std::wstring& error);
    bool write_all(const std::string& bytes, std::wstring& error);
};

// A page Steam is still building (about:blank login) is not injected. Injecting
// there freezes startup and the real window never appears.
inline bool steam_ui_page(const std::string& type, const std::string& url, const std::string& title) {
    if (type != "page") {
        return false;
    }
    // The real desktop is an about:blank popup whose title is Steam. The login
    // window is also about:blank, and its title is not Steam, so it stays out.
    if (title == "Steam" || title == "Steam Games List") {
        return true;
    }
    if (url.find("about:blank") != std::string::npos) {
        return false;
    }
    if (url.find("steamloopback.host") != std::string::npos || url.find("steampowered.com") != std::string::npos) {
        return true;
    }
    return false;
}

// The store and community views are their own documents, often an iframe inside
// SteamBrowser. The shell cannot style them. Checkout stays untouched.
inline bool steam_web_document(const std::string& type, const std::string& url) {
    if (type == "browser" || type == "worker" || type == "service_worker" || type == "shared_worker") {
        return false;
    }
    if (url.find("checkout.steampowered.com") != std::string::npos || url.find("api.steampowered.com") != std::string::npos) {
        return false;
    }
    return url.find("steampowered.com") != std::string::npos || url.find("steamcommunity.com") != std::string::npos;
}

// The music player is only inlined on the store front. News, app pages, and
// community documents stay skinned without a second player.
inline bool steam_store_home(const std::string& url) {
    const std::string host = "store.steampowered.com";
    const size_t at = url.find(host);
    if (at == std::string::npos) {
        return false;
    }
    const size_t path_at = at + host.size();
    if (path_at >= url.size()) {
        return true;
    }
    if (url[path_at] != '/') {
        return false;
    }
    const std::string path = url.substr(path_at);
    return path == "/" || path.rfind("/?", 0) == 0 || path.rfind("/#", 0) == 0;
}

// The shell gets the boot script. A store or community document does not, or the
// library page stays under the debugger and the store skin is a one-shot.
inline bool steam_shell_page(const std::string& type, const std::string& url, const std::string& title) {
    return steam_ui_page(type, url, title) && !steam_web_document(type, url);
}

bool inject_boot_script(CdpPipe& pipe, const std::string& script, std::string& session_id, std::wstring& error);

// Starts watching store and community documents. No-op until skin tags exist.
bool enable_store_fetch(CdpPipe& pipe);

}  // namespace doomnet
