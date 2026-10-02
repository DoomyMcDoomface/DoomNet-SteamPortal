#pragma once

// A child process with captured stdout, for utils.exec.
//
// Never goes through cmd.exe: the command line is handed to CreateProcessW, so a
// console window cannot flash and a shell built-in is not silently accepted.
// stdin and stderr are NUL. Only the stdout pipe and NUL are inherited
// (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), so a child cannot pick up a handle that
// belongs to Steam or to the CDP pipes.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>

namespace doomnet {

class ProcessJob {
public:
    ProcessJob() = default;
    ~ProcessJob();
    ProcessJob(const ProcessJob&) = delete;
    ProcessJob& operator=(const ProcessJob&) = delete;

    // `error` is a sentence naming the command and the Windows error text.
    bool start(const std::wstring& command_line, std::wstring& error);

    // Reads whatever stdout has ready without blocking. True once the process
    // has exited and its pipe is drained (output from a grandchild that outlives
    // the child is not waited for).
    bool pump();

    HANDLE process_handle() const { return process_; }
    const std::string& output() const { return output_; }
    int64_t exit_code() const { return exit_code_; }
    uint64_t started_ms() const { return started_ms_; }
    const std::wstring& command_line() const { return command_line_; }

private:
    void drain();
    void close_all();

    HANDLE process_ = nullptr;
    HANDLE read_ = nullptr;
    std::string output_;
    int64_t exit_code_ = 0;
    uint64_t started_ms_ = 0;
    bool exited_ = false;
    std::wstring command_line_;
};

// Runs to completion or until `timeout_ms`. False with `error` set on failure
// to start or on timeout (the child is left running on timeout).
bool run_process_blocking(const std::wstring& command_line, unsigned long timeout_ms, std::string& output, int64_t& exit_code, std::wstring& error);

}  // namespace doomnet
