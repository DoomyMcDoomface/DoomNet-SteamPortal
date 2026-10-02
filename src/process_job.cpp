#include "process_job.h"

#include "host_bus.h"
#include "log.h"

#include <vector>

namespace doomnet {

namespace {

// First token of a command line, for messages. Never the whole line, which can
// hold a path with a user name in it more than the reader needs.
std::wstring first_token(const std::wstring& line) {
    if (line.empty()) {
        return line;
    }
    if (line[0] == L'"') {
        const size_t close = line.find(L'"', 1);
        return line.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1);
    }
    const size_t space = line.find(L' ');
    return line.substr(0, space);
}

}  // namespace

ProcessJob::~ProcessJob() {
    close_all();
}

void ProcessJob::close_all() {
    if (process_ != nullptr) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (read_ != nullptr) {
        CloseHandle(read_);
        read_ = nullptr;
    }
}

bool ProcessJob::start(const std::wstring& command_line, std::wstring& error) {
    error.clear();
    command_line_ = command_line;
    const std::wstring program = first_token(command_line);

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inheritable, 1u << 20)) {
        error = L"Could not create the output pipe for '" + program + L"'. " + windows_error_text(GetLastError());
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        error = L"Could not open NUL for the input of '" + program + L"'. " + windows_error_text(GetLastError());
        CloseHandle(read_end);
        CloseHandle(write_end);
        return false;
    }

    HANDLE inherited[2] = {write_end, nul};
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<char> attribute_storage(attribute_bytes);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
        error = L"Could not restrict the handles passed to '" + program + L"'. " + windows_error_text(GetLastError());
        CloseHandle(read_end);
        CloseHandle(write_end);
        CloseHandle(nul);
        return false;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nul;
    startup.StartupInfo.hStdOutput = write_end;
    startup.StartupInfo.hStdError = nul;
    startup.lpAttributeList = attributes;

    std::vector<wchar_t> mutable_line(command_line.begin(), command_line.end());
    mutable_line.push_back(L'\0');

    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &info);
    const DWORD create_error = created ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(write_end);
    CloseHandle(nul);
    if (!created) {
        CloseHandle(read_end);
        error = L"Could not start '" + program + L"' (no shell is used, so a built-in such as 'dir' will not run). " + windows_error_text(create_error);
        return false;
    }
    CloseHandle(info.hThread);
    process_ = info.hProcess;
    read_ = read_end;
    started_ms_ = monotonic_ms();
    exited_ = false;
    output_.clear();
    return true;
}

void ProcessJob::drain() {
    if (read_ == nullptr) {
        return;
    }
    char buffer[16384];
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(read_, nullptr, 0, nullptr, &available, nullptr)) {
            CloseHandle(read_);  // broken pipe: every writer has closed
            read_ = nullptr;
            return;
        }
        if (available == 0) {
            return;
        }
        DWORD got = 0;
        if (!ReadFile(read_, buffer, static_cast<DWORD>(sizeof(buffer)), &got, nullptr) || got == 0) {
            CloseHandle(read_);
            read_ = nullptr;
            return;
        }
        output_.append(buffer, got);
    }
}

bool ProcessJob::pump() {
    if (process_ == nullptr) {
        return true;
    }
    drain();
    if (!exited_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
        exited_ = true;
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        exit_code_ = static_cast<int64_t>(code);
        drain();  // the last bytes are often written just before exit
    }
    return exited_;
}

bool run_process_blocking(const std::wstring& command_line, unsigned long timeout_ms, std::string& output, int64_t& exit_code, std::wstring& error) {
    ProcessJob job;
    if (!job.start(command_line, error)) {
        return false;
    }
    const uint64_t deadline = monotonic_ms() + timeout_ms;
    while (!job.pump()) {
        if (monotonic_ms() >= deadline) {
            error = L"'" + first_token(command_line) + L"' did not exit within " + std::to_wstring(timeout_ms) + L" ms. It was left running.";
            return false;
        }
        WaitForSingleObject(job.process_handle(), 25);
    }
    output = job.output();
    exit_code = job.exit_code();
    return true;
}

}  // namespace doomnet
