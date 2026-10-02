#pragma once

// Keeps a Steam debugging pipe open and answers window.Millennium.callServerMethod.
// The serve thread reads the pipe. The browser thread owns the resolve hook and
// never calls Lua. The state thread is the only thread that enters Lua.

#include "browser_thread.h"
#include "cdp_pipe.h"
#include "host_bus.h"

#include <memory>
#include <string>

namespace doomnet {

class SteamPage {
public:
    SteamPage();
    ~SteamPage();
    SteamPage(const SteamPage&) = delete;
    SteamPage& operator=(const SteamPage&) = delete;

    // Host thread. `pipe` is the open connection from a successful inject.
    // `session_id` already has the boot script and the doomnet binding.
    // Replaces `browser`'s resolve and frontend hooks.
    bool start(CdpPipe pipe, const std::string& session_id, BrowserThread& browser, Bus& bus, std::wstring& error);

    // Host thread. Stops the serve thread and closes the pipe.
    void stop();

    // Host thread. Drops the debugger off the library page. The boot script
    // keeps running in the page. An attached debugger makes the game grid slow.
    void release_debugger();

    // Host thread. Evaluates `expression` in the primary page session.
    // Do not await a Promise that itself calls back into the host; the serve
    // thread would be stuck inside this call and could not send the answer.
    bool evaluate(const std::string& expression, bool await_promise, std::string& result, std::wstring& error);

    // Same as evaluate, but uses the page whose document title is Steam once
    // that page has been installed. Until then it uses the primary session.
    bool evaluate_display(const std::string& expression, bool await_promise, std::string& result, std::wstring& error);

private:
    bool evaluate_impl(const std::string& expression, bool await_promise, bool prefer_display, std::string& result, std::wstring& error);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// How many store or community documents have had the skin written into them.
int patched_web_documents();

// The album JPEG the library already shows for this steamloopback art address.
// Empty when the address is not an art path, walks out of the cache, or the file is absent.
std::wstring shared_cover_path(const std::string& art_url);

}  // namespace doomnet
