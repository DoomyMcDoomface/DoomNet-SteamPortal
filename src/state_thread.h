#pragma once

// The state thread: the only thread that calls into Lua. Spec: "Two threads,
// two queues".
//
// One lua_State. Every call from a page runs as its own coroutine on that
// state, so a call that sleeps, waits for the page, reads a file range, or
// waits for a process parks and lets the next message run. Lua coroutines are
// not threads: one runs at a time, and from one Lua line to the next with no
// yield between them nothing else touches the tables.
//
// No thread pool. The state is never closed because one call failed.

#include "host_bus.h"
#include "host_config.h"

#include <memory>
#include <string>

namespace doomnet {

const char* player_compat_lua();

class StateThread {
public:
    StateThread(Bus& bus, HostConfig config);
    ~StateThread();
    StateThread(const StateThread&) = delete;
    StateThread& operator=(const StateThread&) = delete;

    // Creates the thread. It creates the Lua state, loads main.lua, and runs
    // on_load. Returns false (with a sentence) only if the thread could not start.
    bool start(std::wstring& error);

    // Waits until main.lua has been loaded and on_load has returned or failed.
    bool wait_loaded(unsigned long timeout_ms);

    bool main_lua_loaded() const;
    bool on_load_ok() const;
    bool ready_signalled() const;
    uint64_t calls_completed() const;

    // Posts Stop and joins. Stop the browser thread first: closing the state
    // closes Lua file objects, and a ReadRange must not be reading one.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace doomnet
