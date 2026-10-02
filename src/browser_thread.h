#pragma once

// The browser thread. Spec: "Two threads, two queues".
//
// Owns everything that waits: timers, the byte reads a parked coroutine handed
// off, child processes, and (in a later step) the page connection. It never
// calls into Lua and never touches a Lua string. What it sends to the state
// thread is a ResumeMsg or a CallMsg made of plain data.
//
// The page transport is a pair of hooks so this file does not depend on CDP:
//   on_resolve   a Call finished; settle the page's Promise.
//   on_frontend  run a JS method on the page; call `complete_frontend` when it returns.
// With no hooks set, a Resolve is logged and a Frontend call resumes with nil at
// once ("no page has the method yet"), which is what the spec asks for.

#include "host_bus.h"

#include <functional>
#include <memory>
#include <string>

namespace doomnet {

class BrowserThread {
public:
    explicit BrowserThread(Bus& bus);
    ~BrowserThread();
    BrowserThread(const BrowserThread&) = delete;
    BrowserThread& operator=(const BrowserThread&) = delete;

    // Set before start(). Called on the browser thread.
    std::function<void(ResolveMsg&&)> on_resolve;
    std::function<void(FrontendMsg&&)> on_frontend;

    bool start(std::wstring& error);
    void stop();

    // Thread-safe. Runs `work` on the browser thread.
    void post(std::function<void()> work);

    // Browser thread only (call from a hook or from `post`).
    void after(uint32_t milliseconds, std::function<void()> work);
    void complete_frontend(uint64_t co_id, WireValue value);
    void fail_frontend(uint64_t co_id, const std::string& error);

    // Browser thread only. `signalled` runs when `handle` becomes signalled.
    // At most 32 sources.
    bool add_source(void* handle, std::function<void()> signalled);
    void remove_source(void* handle);

    uint64_t reads_completed() const;
    uint64_t processes_completed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace doomnet
