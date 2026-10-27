#pragma once

// Messages between the two DoomNet threads. Spec: "Two threads, two queues".
//
// The state thread is the only thread that calls into Lua. The browser thread
// never touches a lua_State. What crosses between them is plain data: strings,
// numbers, and a file handle that a parked coroutine handed off.
//
// One mutex and one condition variable protect both queues. The state thread
// sleeps on the condition variable only when no message is waiting. The browser
// thread sleeps in WaitForMultipleObjects and is woken by the bus event.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace doomnet {

// A value that can cross the thread boundary. `text` is the string bytes, or
// the JSON text of an object for Kind::Json.
struct WireValue {
    enum class Kind { Nothing, Nil, Boolean, Number, String, Json };
    Kind kind = Kind::Nothing;
    bool boolean = false;
    double number = 0;
    std::string text;
};

// ---- browser thread -> state thread --------------------------------------

// A page asked for a Lua global function by name. `args` are the positional
// arguments of callServerMethod, already parsed (boolean, number, string).
struct CallMsg {
    uint64_t call_id = 0;
    std::string method;
    std::vector<WireValue> args;
};

// A parked coroutine can continue.
//   Sleep      : nothing.
//   Frontend   : `value` is the typed return, or `failed` with `error`.
//   ReadRange  : `value.text` holds the bytes, or `failed` with `error`.
//   Exec       : `value.text` holds the output and `exit_code` the exit code,
//                or `failed` with `error`.
struct ResumeMsg {
    uint64_t co_id = 0;
    bool failed = false;
    std::string error;
    WireValue value;
    int64_t exit_code = 0;
};

struct StopStateMsg {};

using StateInbox = std::variant<CallMsg, ResumeMsg, StopStateMsg>;

// ---- state thread -> browser thread --------------------------------------

// A Call returned. `ok`: `json` is a JSON literal for the page to hand to
// resolve(). Not ok: `json` is the error text for reject().
// `urgent` replies are delivered before anything else, so a play call does not
// wait behind the duck-envelope poll.
struct ResolveMsg {
    uint64_t call_id = 0;
    bool ok = true;
    bool urgent = false;
    bool playback = false;
    std::string method;
    std::vector<uint64_t> also;
    std::string json;
};

inline bool playback_call(const std::string& method) {
    return method == "stage_track_for_playback" || method == "get_track_audio_chunk" || method == "release_staged_track"
        || method == "claim_audio_owner" || method == "release_audio_owner" || method == "transport_command"
        || method == "take_pending_transport";
}

// The page stops waiting for these after 8 seconds. They leave before every poll.
inline bool audio_load_call(const std::string& method) {
    return method == "stage_track_for_playback" || method == "get_track_audio_chunk" || method == "release_staged_track";
}

// The page fires these on a timer. One answer covers every call that arrived
// while that answer was still being read, so the timer cannot fill the queue.
inline bool shared_poll_call(const std::string& method) {
    return method == "get_duck_envelope" || method == "get_player_state" || method == "get_player_state_full";
}

// Replies the page cannot show the player without. They jump ahead of the
// duck poll so the game overlay loads the same folders, theme, and library
// as the Steam window.
inline bool page_jump_reply(const std::string& method) {
    return method == "stage_track_for_playback" || method == "get_track_audio_chunk" || method == "release_staged_track"
        || method == "get_settings" || method == "get_music_folders" || method == "get_library_info"
        || method == "get_playlists" || method == "set_ui_pointer" || method == "set_setting"
        || method == "get_player_state" || method == "get_player_state_full";
}

struct SleepMsg {
    uint64_t co_id = 0;
    uint32_t milliseconds = 0;
};

// Run a JS method on the page. `params_json` is a JSON array of the arguments.
struct FrontendMsg {
    uint64_t co_id = 0;
    std::string method;
    std::string params_json;
    std::string caller;  // "file.lua:123" for the log
};

// Read up to `length` bytes at `offset` from `file`. The handle belongs to a
// Lua file object whose coroutine is parked, so nothing else touches it until
// the Resume arrives.
struct ReadRangeMsg {
    uint64_t co_id = 0;
    void* file = nullptr;
    std::wstring path;  // for the log only
    uint64_t offset = 0;
    uint32_t length = 0;
};

// Start a process, capture stdout, wait for it to exit.
struct ExecMsg {
    uint64_t co_id = 0;
    std::wstring command_line;
};

struct StopBrowserMsg {};

using BrowserInbox = std::variant<ResolveMsg, SleepMsg, FrontendMsg, ReadRangeMsg, ExecMsg, StopBrowserMsg>;

class Bus {
public:
    Bus() : event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~Bus() {
        if (event_ != nullptr) {
            CloseHandle(event_);
        }
    }
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    bool valid() const { return event_ != nullptr; }

    void to_state(StateInbox message) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_queue_.push_back(std::move(message));
        }
        state_cv_.notify_one();
    }

    // A volume wheel posts faster than the player can apply. Fold the new
    // delta into the bump already waiting, so the wheel does not queue a
    // round trip per notch.
    bool merge_queued_volume(double delta) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (StateInbox& message : state_queue_) {
            auto* call = std::get_if<CallMsg>(&message);
            if (call == nullptr || call->method != "transport_command" || call->args.empty() || call->args[0].kind != WireValue::Kind::String) {
                continue;
            }
            std::string& text = call->args[0].text;
            const size_t action = text.find("\"volumeBump\"");
            const size_t value_key = text.find("\"value\":");
            if (action == std::string::npos || value_key == std::string::npos) {
                continue;
            }
            const char* start = text.c_str() + value_key + 8;
            char* end = nullptr;
            const double current = std::strtod(start, &end);
            if (end == start) {
                continue;
            }
            // Rolling up banks steps that the player never plays once the
            // level is already at the top. Adding the downward steps onto
            // that bank is the delay before the volume can fall.
            const bool reversed = (current > 0 && delta < 0) || (current < 0 && delta > 0);
            char number[32];
            std::snprintf(number, sizeof(number), "%.2f", reversed ? delta : current + delta);
            text.replace(value_key + 8, static_cast<size_t>(end - start), number);
            return true;
        }
        return false;
    }

    // True when a playback call is sitting in the queue. The state thread's
    // artwork hook uses this to yield so the play call can run.
    bool playback_queued() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const StateInbox& message : state_queue_) {
            if (const auto* call = std::get_if<CallMsg>(&message)) {
                if (is_playback_method(call->method)) {
                    return true;
                }
            }
        }
        return false;
    }

    // Non-blocking. False when the queue is empty.
    bool try_take_state(StateInbox& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_queue_.empty()) {
            return false;
        }
        out = take_state_locked();
        return true;
    }

    // Blocks until a message is queued. Playback calls come out before artwork
    // calls. Everything else keeps its queue order. State thread only.
    StateInbox wait_state() {
        std::unique_lock<std::mutex> lock(mutex_);
        state_cv_.wait(lock, [this] { return !state_queue_.empty(); });
        return take_state_locked();
    }

    void to_browser(BrowserInbox message) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            browser_queue_.push_back(std::move(message));
        }
        SetEvent(event_);
    }

    // Moves everything queued for the browser thread into `out`. Never blocks.
    void take_browser(std::vector<BrowserInbox>& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!browser_queue_.empty()) {
            out.push_back(std::move(browser_queue_.front()));
            browser_queue_.pop_front();
        }
    }

    HANDLE browser_event() const { return event_; }

    // Lowest set CPU for the Lua thread, the next one for artwork. Both are 0
    // when Windows only offers one CPU, and the caller should not pin then.
    static void cpu_masks(DWORD_PTR& lua_mask, DWORD_PTR& art_mask) {
        lua_mask = 0;
        art_mask = 0;
        DWORD_PTR process_mask = 0;
        DWORD_PTR system_mask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
            return;
        }
        for (unsigned bit = 0; bit < sizeof(DWORD_PTR) * 8; ++bit) {
            const DWORD_PTR mask = DWORD_PTR(1) << bit;
            if ((process_mask & mask) == 0) {
                continue;
            }
            if (lua_mask == 0) {
                lua_mask = mask;
            } else {
                art_mask = mask;
                return;
            }
        }
    }

    size_t state_depth() {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_queue_.size();
    }

private:
    static bool is_playback_method(const std::string& method) {
        return playback_call(method);
    }

    static bool is_artwork_method(const std::string& method) {
        return method == "request_idle_art" || method == "restore_published_art" || method == "start_queued_artwork_regenerate"
            || method == "regenerate_artwork" || method == "get_art_progress" || method == "queue_artwork_regenerate"
            || method == "rescan_library_start" || method == "scan_library_batch" || method == "get_track_art"
            || method == "get_first_track_art";
    }

    // Caller holds mutex_. Queue is not empty.
    StateInbox take_state_locked() {
        auto take_at = [this](size_t index) {
            StateInbox message = std::move(state_queue_[index]);
            state_queue_.erase(state_queue_.begin() + static_cast<std::deque<StateInbox>::difference_type>(index));
            return message;
        };
        for (size_t i = 0; i < state_queue_.size(); ++i) {
            if (std::holds_alternative<StopStateMsg>(state_queue_[i])) {
                return take_at(i);
            }
        }
        for (size_t i = 0; i < state_queue_.size(); ++i) {
            if (const auto* call = std::get_if<CallMsg>(&state_queue_[i])) {
                if (is_playback_method(call->method)) {
                    return take_at(i);
                }
            }
        }
        for (size_t i = 0; i < state_queue_.size(); ++i) {
            if (const auto* call = std::get_if<CallMsg>(&state_queue_[i])) {
                if (is_artwork_method(call->method)) {
                    continue;
                }
            }
            return take_at(i);
        }
        return take_at(0);
    }

    std::mutex mutex_;
    std::condition_variable state_cv_;
    std::deque<StateInbox> state_queue_;
    std::deque<BrowserInbox> browser_queue_;
    HANDLE event_ = nullptr;
};

inline uint64_t monotonic_ms() {
    return static_cast<uint64_t>(GetTickCount64());
}

}  // namespace doomnet
