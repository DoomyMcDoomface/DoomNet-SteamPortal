#pragma once

// The page bridge. A Steam page (and the local proof page) calls
// window.Millennium.callServerMethod. That call hits the `doomnet` binding,
// which becomes a Call on the state thread. call_frontend_method comes back
// the other way as a JS call on the same page.
//
// This file does not attach to Steam. steam_attach is what later points it
// at steamwebhelper. The page never fetches a loopback address.

#include "browser_thread.h"
#include "host_bus.h"

#include <memory>
#include <string>

namespace doomnet {

class PageBridge {
public:
    explicit PageBridge(Bus& bus);
    ~PageBridge();
    PageBridge(const PageBridge&) = delete;
    PageBridge& operator=(const PageBridge&) = delete;

    // Browser thread, from the resolve hook. Page calls are settled. Other calls
    // (a test posting straight to the bus) are ignored.
    void settle(ResolveMsg message);

    // Browser thread, from the frontend hook.
    void call_page(FrontendMsg message);

    // Test or host thread. `browser` must already be running. Installs the boot
    // script on the page at `websocket_url` (ws://host:port/devtools/page/...).
    bool open(BrowserThread& browser, const std::string& websocket_url, std::wstring& error);

    // Test or host thread. The returned text is the value when the page result
    // is a string, "true"/"false" for a boolean, or the number's digits.
    bool evaluate(const std::string& expression, bool await_promise, std::string& result, std::wstring& error);

    // Test or host thread. Safe to call twice.
    void close();

    bool connected() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// Host thread. GET http://127.0.0.1:<port>/json/list and return the first page
// target's ws:// address. The page itself does not make this request.
bool discover_cdp_page(int port, std::string& websocket_url, std::wstring& error);

// The boot script embedded from src/boot_script.js.
const char* boot_script_text();

// JavaScript run in the page before the boot script. Empty unless a host set
// window.__doomnetConfig. The local proof page leaves this empty.
void set_boot_preamble(const std::string& javascript);
std::string boot_script_for_page();

}  // namespace doomnet
