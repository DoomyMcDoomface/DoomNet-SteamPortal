// Spec: "Two threads, two queues" and "What the browser thread does".
//
// One loop: wait on the bus event, the process handles, and any registered
// source, with a timeout equal to the next timer. Nothing here calls lua_*.

#include "anchors.h"
#include "browser_thread.h"
#include "log.h"
#include "process_job.h"
#include "text.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace doomnet {

void browser_thread_anchor() {}

namespace {

constexpr size_t kMaxSources = 32;
constexpr size_t kMaxRunningProcesses = 16;
constexpr uint64_t kProcessTimeoutMs = 120000;
constexpr unsigned long kProcessPollMs = 20;

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

std::string short_command(const std::wstring& line) {
    std::wstring head = line;
    if (!head.empty() && head[0] == L'"') {
        const size_t close = head.find(L'"', 1);
        head = head.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1);
    } else {
        head = head.substr(0, head.find(L' '));
    }
    const size_t slash = head.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        head.erase(0, slash + 1);
    }
    return wide_to_utf8(head);
}

}  // namespace

struct BrowserThread::Impl {
    Bus& bus;
    BrowserThread& owner;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> processes{0};

    std::mutex posted_mutex;
    std::vector<std::function<void()>> posted;

    std::multimap<uint64_t, std::function<void()>> timers;

    struct Source {
        HANDLE handle;
        std::function<void()> signalled;
    };
    std::vector<Source> sources;

    struct ExecEntry {
        uint64_t co_id = 0;
        std::unique_ptr<ProcessJob> job;
    };
    std::vector<ExecEntry> running_processes;
    std::deque<ExecMsg> waiting_processes;

    bool stop_requested = false;
    bool reported_no_page = false;

    Impl(Bus& b, BrowserThread& o) : bus(b), owner(o) {}

    void resume(ResumeMsg message) { bus.to_state(std::move(message)); }

    // ---- message handling -------------------------------------------------

    void on_sleep(const SleepMsg& message) {
        const uint64_t co_id = message.co_id;
        add_timer(message.milliseconds, [this, co_id] {
            ResumeMsg out;
            out.co_id = co_id;
            resume(std::move(out));
        });
    }

    void on_read_range(ReadRangeMsg& message) {
        ResumeMsg out;
        out.co_id = message.co_id;
        std::string bytes(message.length, '\0');
        size_t got = 0;
        DWORD error = 0;
        if (message.file == nullptr || !read_at(static_cast<HANDLE>(message.file), message.offset, bytes.data(), bytes.size(), got, error)) {
            out.failed = true;
            out.error = "Reading " + std::to_string(message.length) + " bytes at offset " + std::to_string(message.offset) + " of '" + wide_to_utf8(message.path) + "' failed: " + wide_to_utf8(windows_error_text(error));
            log_event(L"error", L"browser", utf8_to_wide_lossy(out.error).c_str());
        } else {
            bytes.resize(got);
            out.value.kind = WireValue::Kind::String;
            out.value.text = std::move(bytes);
        }
        ++reads;
        resume(std::move(out));
    }

    void start_process(ExecMsg message) {
        auto job = std::make_unique<ProcessJob>();
        std::wstring error;
        if (!job->start(message.command_line, error)) {
            log_event(L"warn", L"browser", error.c_str());
            ResumeMsg out;
            out.co_id = message.co_id;
            out.failed = true;
            out.error = "Failed to execute command: " + wide_to_utf8(error);
            resume(std::move(out));
            return;
        }
        ExecEntry entry;
        entry.co_id = message.co_id;
        entry.job = std::move(job);
        running_processes.push_back(std::move(entry));
    }

    void on_exec(ExecMsg& message) {
        if (running_processes.size() >= kMaxRunningProcesses) {
            waiting_processes.push_back(std::move(message));
            return;
        }
        start_process(std::move(message));
    }

    void on_frontend(FrontendMsg& message) {
        if (owner.on_frontend) {
            owner.on_frontend(std::move(message));
            return;
        }
        if (!reported_no_page) {
            reported_no_page = true;
            log_event(L"info", L"browser", L"The player called a page method but no page is connected. The call resumes with nil, as the spec says. This line is written once.");
        }
        ResumeMsg out;
        out.co_id = message.co_id;
        out.value.kind = WireValue::Kind::Nil;
        resume(std::move(out));
    }

    void on_resolve(ResolveMsg& message) {
        if (owner.on_resolve) {
            owner.on_resolve(std::move(message));
        }
    }

    void handle(BrowserInbox& item) {
        if (auto* m = std::get_if<ResolveMsg>(&item)) {
            on_resolve(*m);
        } else if (auto* m2 = std::get_if<SleepMsg>(&item)) {
            on_sleep(*m2);
        } else if (auto* m3 = std::get_if<FrontendMsg>(&item)) {
            on_frontend(*m3);
        } else if (auto* m4 = std::get_if<ReadRangeMsg>(&item)) {
            on_read_range(*m4);
        } else if (auto* m5 = std::get_if<ExecMsg>(&item)) {
            on_exec(*m5);
        } else if (std::holds_alternative<StopBrowserMsg>(item)) {
            stop_requested = true;
        }
    }

    // ---- timers, processes, sources ---------------------------------------

    void add_timer(uint32_t milliseconds, std::function<void()> work) {
        timers.emplace(monotonic_ms() + milliseconds, std::move(work));
    }

    void fire_timers() {
        const uint64_t now = monotonic_ms();
        while (!timers.empty() && timers.begin()->first <= now) {
            std::function<void()> work = std::move(timers.begin()->second);
            timers.erase(timers.begin());
            if (work) {
                work();
            }
        }
    }

    void pump_processes() {
        for (size_t i = 0; i < running_processes.size();) {
            ExecEntry& entry = running_processes[i];
            const bool done = entry.job->pump();
            const bool timed_out = !done && monotonic_ms() - entry.job->started_ms() > kProcessTimeoutMs;
            if (!done && !timed_out) {
                ++i;
                continue;
            }
            ResumeMsg out;
            out.co_id = entry.co_id;
            if (done) {
                out.value.kind = WireValue::Kind::String;
                out.value.text = entry.job->output();
                out.exit_code = entry.job->exit_code();
            } else {
                out.failed = true;
                out.error = "Failed to execute command: '" + short_command(entry.job->command_line()) + "' did not exit within " + std::to_string(kProcessTimeoutMs / 1000) + " seconds. It was left running.";
                log_event(L"warn", L"browser", utf8_to_wide_lossy(out.error).c_str());
            }
            ++processes;
            running_processes.erase(running_processes.begin() + static_cast<std::ptrdiff_t>(i));
            resume(std::move(out));
        }
        while (!waiting_processes.empty() && running_processes.size() < kMaxRunningProcesses) {
            ExecMsg next = std::move(waiting_processes.front());
            waiting_processes.pop_front();
            start_process(std::move(next));
        }
    }

    void run_posted() {
        std::vector<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(posted_mutex);
            batch.swap(posted);
        }
        for (auto& work : batch) {
            if (work) {
                work();
            }
        }
    }

    void loop() {
        log_event(L"info", L"browser", L"The browser thread started its event loop.");
        while (!stop_requested) {
            std::vector<HANDLE> handles;
            handles.push_back(bus.browser_event());
            const size_t first_source = handles.size();
            for (const Source& source : sources) {
                handles.push_back(source.handle);
            }
            const size_t first_process = handles.size();
            for (const ExecEntry& entry : running_processes) {
                if (handles.size() >= MAXIMUM_WAIT_OBJECTS) {
                    break;
                }
                handles.push_back(entry.job->process_handle());
            }

            DWORD timeout = INFINITE;
            if (!timers.empty()) {
                const uint64_t now = monotonic_ms();
                const uint64_t due = timers.begin()->first;
                timeout = due > now ? static_cast<DWORD>(std::min<uint64_t>(due - now, 0x7fffffff)) : 0;
            }
            if (!running_processes.empty()) {
                timeout = std::min<DWORD>(timeout, kProcessPollMs);  // stdout is drained by polling
            }

            const DWORD woke = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeout);
            if (woke == WAIT_FAILED) {
                log_event(L"error", L"browser", (L"The browser thread's wait failed, so its event loop is ending. " + windows_error_text(GetLastError())).c_str());
                break;
            }

            // Sources first, so a page message is seen before the timers it might cancel.
            if (woke >= WAIT_OBJECT_0 + first_source && woke < WAIT_OBJECT_0 + first_process) {
                const size_t index = woke - WAIT_OBJECT_0 - first_source;
                if (index < sources.size()) {
                    std::function<void()> work = sources[index].signalled;
                    if (work) {
                        work();
                    }
                }
            }

            std::vector<BrowserInbox> inbox;
            bus.take_browser(inbox);
            for (BrowserInbox& item : inbox) {
                handle(item);
                if (stop_requested) {
                    break;
                }
            }
            run_posted();
            fire_timers();
            pump_processes();
        }
        log_event(L"info", L"browser", L"The browser thread left its event loop.");
    }
};

BrowserThread::BrowserThread(Bus& bus) : impl_(std::make_unique<Impl>(bus, *this)) {}

BrowserThread::~BrowserThread() {
    stop();
}

bool BrowserThread::start(std::wstring& error) {
    error.clear();
    if (impl_->running.exchange(true)) {
        error = L"The browser thread was already started.";
        return false;
    }
    if (!impl_->bus.valid()) {
        impl_->running = false;
        error = L"The browser thread could not start because the message bus has no event. " + windows_error_text(GetLastError());
        log_event(L"error", L"browser", error.c_str());
        return false;
    }
    try {
        impl_->thread = std::thread([this] { impl_->loop(); });
    } catch (const std::exception& ex) {
        impl_->running = false;
        error = L"The browser thread could not be created: " + utf8_to_wide_lossy(ex.what());
        log_event(L"error", L"browser", error.c_str());
        return false;
    }
    return true;
}

void BrowserThread::stop() {
    if (!impl_ || !impl_->running.exchange(false)) {
        return;
    }
    impl_->bus.to_browser(StopBrowserMsg{});
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

void BrowserThread::post(std::function<void()> work) {
    {
        std::lock_guard<std::mutex> lock(impl_->posted_mutex);
        impl_->posted.push_back(std::move(work));
    }
    SetEvent(impl_->bus.browser_event());
}

void BrowserThread::after(uint32_t milliseconds, std::function<void()> work) {
    impl_->add_timer(milliseconds, std::move(work));
}

void BrowserThread::complete_frontend(uint64_t co_id, WireValue value) {
    ResumeMsg out;
    out.co_id = co_id;
    out.value = std::move(value);
    impl_->resume(std::move(out));
}

void BrowserThread::fail_frontend(uint64_t co_id, const std::string& error) {
    ResumeMsg out;
    out.co_id = co_id;
    out.failed = true;
    out.error = error;
    impl_->resume(std::move(out));
}

bool BrowserThread::add_source(void* handle, std::function<void()> signalled) {
    if (impl_->sources.size() >= kMaxSources || handle == nullptr) {
        return false;
    }
    impl_->sources.push_back({static_cast<HANDLE>(handle), std::move(signalled)});
    return true;
}

void BrowserThread::remove_source(void* handle) {
    auto& list = impl_->sources;
    list.erase(std::remove_if(list.begin(), list.end(), [handle](const Impl::Source& s) { return s.handle == handle; }), list.end());
}

uint64_t BrowserThread::reads_completed() const { return impl_->reads.load(); }
uint64_t BrowserThread::processes_completed() const { return impl_->processes.load(); }

}  // namespace doomnet
