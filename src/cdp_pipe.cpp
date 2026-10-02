#include "cdp_pipe.h"

#include "json_wire.h"
#include "log.h"
#include "text.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace doomnet {

namespace {

void* invalid_handle() {
    return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}

}  // namespace

CdpPipe::~CdpPipe() {
    close();
}

CdpPipe::CdpPipe(CdpPipe&& other) noexcept : pipe_(other.pipe_), next_id_(other.next_id_), inbox_(std::move(other.inbox_)), pending_(std::move(other.pending_)) {
    other.pipe_ = invalid_handle();
    other.next_id_ = 1;
}

CdpPipe& CdpPipe::operator=(CdpPipe&& other) noexcept {
    if (this != &other) {
        close();
        pipe_ = other.pipe_;
        next_id_ = other.next_id_;
        inbox_ = std::move(other.inbox_);
        pending_ = std::move(other.pending_);
        other.pipe_ = invalid_handle();
        other.next_id_ = 1;
    }
    return *this;
}

bool CdpPipe::connect_steam(unsigned long pid, std::wstring& error) {
    return connect_name(L"\\\\.\\pipe\\DoomNet.cdp." + std::to_wstring(pid), error);
}

bool CdpPipe::connect_name(const std::wstring& pipe_name, std::wstring& error) {
    close();
    for (int attempt = 0; attempt < 100; ++attempt) {
        HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            pipe_ = pipe;
            const std::wstring detail = L"Connected to '" + pipe_name + L"'.";
            log_event(L"info", L"cdp", detail.c_str());
            return true;
        }
        Sleep(20);
    }
    error = L"Could not connect to '" + pipe_name + L"'. " + windows_error_text(GetLastError());
    log_event(L"error", L"cdp", error.c_str());
    return false;
}

void CdpPipe::close() {
    if (pipe_ != nullptr && pipe_ != invalid_handle()) {
        CloseHandle(static_cast<HANDLE>(pipe_));
    }
    pipe_ = invalid_handle();
    inbox_.clear();
    pending_.clear();
}

bool CdpPipe::write_all(const std::string& bytes, std::wstring& error) {
    DWORD sent = 0;
    while (sent < bytes.size()) {
        DWORD wrote = 0;
        if (!WriteFile(static_cast<HANDLE>(pipe_), bytes.data() + sent, static_cast<DWORD>(bytes.size() - sent), &wrote, nullptr) || wrote == 0) {
            error = L"Could not write a debugging command. " + windows_error_text(GetLastError());
            return false;
        }
        sent += wrote;
    }
    return true;
}

bool CdpPipe::read_message(std::string& json, unsigned long timeout_ms, std::wstring& error) {
    json.clear();
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeout_ms || pending_.find('\0') != std::string::npos) {
        const size_t end = pending_.find('\0');
        if (end != std::string::npos) {
            json.assign(pending_, 0, end);
            pending_.erase(0, end + 1);
            return true;
        }
        if (GetTickCount() - start >= timeout_ms) {
            break;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(static_cast<HANDLE>(pipe_), nullptr, 0, nullptr, &available, nullptr)) {
            error = L"The debugging pipe closed while a reply was expected. " + windows_error_text(GetLastError());
            return false;
        }
        if (available == 0) {
            Sleep(1);
            continue;
        }
        const DWORD chunk = available > 65536 ? 65536 : available;
        const size_t offset = pending_.size();
        pending_.resize(offset + chunk);
        DWORD read = 0;
        if (!ReadFile(static_cast<HANDLE>(pipe_), pending_.data() + offset, chunk, &read, nullptr) || read == 0) {
            pending_.resize(offset);
            error = L"Could not read a debugging message. " + windows_error_text(GetLastError());
            return false;
        }
        pending_.resize(offset + read);
        if (pending_.size() > 8u * 1024u * 1024u) {
            pending_.clear();
            error = L"A debugging message was larger than 8 MB.";
            return false;
        }
    }
    error = L"A debugging reply did not arrive within " + std::to_wstring(timeout_ms) + L" ms.";
    return false;
}

bool CdpPipe::read_raw(std::string& json, unsigned long timeout_ms, std::wstring& error) {
    if (!inbox_.empty()) {
        json = std::move(inbox_.front());
        inbox_.pop_front();
        return true;
    }
    return read_message(json, timeout_ms, error);
}

bool CdpPipe::poll(std::string& json, std::wstring& error) {
    error.clear();
    if (!inbox_.empty()) {
        json = std::move(inbox_.front());
        inbox_.pop_front();
        return true;
    }
    if (!connected()) {
        error = L"The debugging pipe is closed.";
        return false;
    }
    DWORD available = 0;
    if (!PeekNamedPipe(static_cast<HANDLE>(pipe_), nullptr, 0, nullptr, &available, nullptr)) {
        error = L"The debugging pipe closed while a page message was expected. " + windows_error_text(GetLastError());
        return false;
    }
    if (available == 0 && pending_.find('\0') == std::string::npos) {
        return false;
    }
    return read_message(json, 2000, error);
}

bool CdpPipe::call(const std::string& method, const std::string& params_json, std::string& result_json, std::wstring& error, unsigned long timeout_ms) {
    return call_impl("", method, params_json, result_json, error, timeout_ms);
}

bool CdpPipe::call_in_session(const std::string& session_id, const std::string& method, const std::string& params_json, std::string& result_json, std::wstring& error, unsigned long timeout_ms) {
    return call_impl(session_id, method, params_json, result_json, error, timeout_ms);
}

bool CdpPipe::call_impl(const std::string& session_id, const std::string& method, const std::string& params_json, std::string& result_json, std::wstring& error, unsigned long timeout_ms) {
    result_json.clear();
    if (pipe_ == invalid_handle()) {
        error = L"There is no debugging pipe, so '" + utf8_to_wide_lossy(method) + L"' was not sent.";
        return false;
    }
    const unsigned long id = next_id_++;
    std::string message = "{\"id\":" + std::to_string(id) + ",\"method\":" + json_quote(method) + ",\"params\":" + params_json;
    if (!session_id.empty()) {
        message += ",\"sessionId\":" + json_quote(session_id);
    }
    message += "}";
    std::string framed = message;
    framed.push_back('\0');
    if (!write_all(framed, error)) {
        log_event(L"error", L"cdp", error.c_str());
        return false;
    }
    const DWORD start = GetTickCount();
    const DWORD budget = timeout_ms == 0 ? 1 : timeout_ms;
    while (GetTickCount() - start < budget) {
        const DWORD elapsed = GetTickCount() - start;
        const DWORD left = elapsed >= budget ? 1 : budget - elapsed;
        std::string incoming;
        if (!read_message(incoming, left, error)) {
            if (budget >= 1000) {
                log_event(L"error", L"cdp", error.c_str());
            }
            return false;
        }
        Json root;
        std::string parse_error;
        if (!json_parse(incoming, root, parse_error)) {
            inbox_.push_back(incoming);
            continue;
        }
        const Json* reply_id = root.find("id");
        if (reply_id == nullptr || reply_id->type != Json::Type::Number || static_cast<unsigned long>(reply_id->n) != id) {
            inbox_.push_back(std::move(incoming));
            continue;
        }
        if (const Json* failure = root.find("error")) {
            error = L"'" + utf8_to_wide_lossy(method) + L"' was rejected: " + utf8_to_wide_lossy(failure->key_string("message"));
            log_event(L"error", L"cdp", error.c_str());
            return false;
        }
        result_json = incoming;
        log_event(L"info", L"cdp", (L"'" + utf8_to_wide_lossy(method) + L"' completed.").c_str());
        return true;
    }
    error = L"'" + utf8_to_wide_lossy(method) + L"' did not complete within " + std::to_wstring(budget) + L" ms.";
    if (budget >= 1000) {
        log_event(L"error", L"cdp", error.c_str());
    }
    return false;
}

bool inject_boot_script(CdpPipe& pipe, const std::string& script, std::string& session_id, std::wstring& error) {
    session_id.clear();
    std::string reply;
    // Before any page is waited on. A store document that loads first is the
    // blue window; catching it here puts the skin in that response.
    enable_store_fetch(pipe);
    if (!pipe.call("Target.setDiscoverTargets", "{\"discover\":true}", reply, error)) {
        return false;
    }
    // Millennium only discovers targets and attaches to the pages it wants. Auto-attach
    // pulls Steam's shared script context into a debugger session and Steam's real
    // window never appears, so it is not used here.
    std::vector<std::string> attach_attempted;
    std::wstring observed;
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 45000 && session_id.empty()) {
        std::string incoming;
        const DWORD elapsed = GetTickCount() - start;
        if (!pipe.read_raw(incoming, elapsed >= 45000 ? 1 : 45000 - elapsed, error)) {
            log_event(L"error", L"cdp", error.c_str());
            break;
        }
        Json root;
        std::string parse_error;
        if (!json_parse(incoming, root, parse_error)) {
            continue;
        }
        const std::string method = root.key_string("method");
        if (method.empty()) {
            continue;
        }
        const Json* params = root.find("params");
        const Json* info = params != nullptr ? params->find("targetInfo") : nullptr;
        const std::string type = info != nullptr ? info->key_string("type") : std::string();
        const std::string url = info != nullptr ? info->key_string("url") : std::string();
        const std::string title = info != nullptr ? info->key_string("title") : std::string();
        const std::string target_id = info != nullptr ? info->key_string("targetId") : std::string();
        if (observed.size() < 700) {
            observed += utf8_to_wide_lossy(method);
            if (!type.empty()) {
                observed += L" type=" + utf8_to_wide_lossy(type);
            }
            if (!url.empty()) {
                std::wstring wide_url = utf8_to_wide_lossy(url);
                if (wide_url.size() > 80) {
                    wide_url.resize(80);
                }
                observed += L" url=" + wide_url;
            }
            observed += L"; ";
        }
        log_event(L"info", L"cdp", (L"DevTools event '" + utf8_to_wide_lossy(method) + L"'" + (type.empty() ? L"" : L" type '" + utf8_to_wide_lossy(type) + L"'") + L".").c_str());
        if (params != nullptr && method == "Target.attachedToTarget") {
            const std::string sid = params->key_string("sessionId");
            if (!sid.empty() && steam_shell_page(type, url, title)) {
                session_id = sid;
                break;
            }
            continue;
        }
        if (!steam_shell_page(type, url, title) || target_id.empty()) {
            continue;
        }
        if (method != "Target.targetCreated" && method != "Target.targetInfoChanged") {
            continue;
        }
        bool already = false;
        for (const std::string& seen : attach_attempted) {
            if (seen == target_id) {
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }
        attach_attempted.push_back(target_id);
        log_event(L"info", L"inject", (L"Attaching to page target '" + utf8_to_wide_lossy(target_id) + L"'.").c_str());
        std::wstring attach_error;
        const std::string attach_params = std::string("{\"targetId\":") + json_quote(target_id) + ",\"flatten\":true}";
        if (!pipe.call("Target.attachToTarget", attach_params, reply, attach_error)) {
            log_event(L"warn", L"inject", attach_error.c_str());
            continue;
        }
        Json attach_root;
        std::string attach_parse;
        if (!json_parse(reply, attach_root, attach_parse)) {
            continue;
        }
        const Json* attach_result = attach_root.find("result");
        if (attach_result != nullptr) {
            session_id = attach_result->key_string("sessionId");
        }
    }
    if (session_id.empty()) {
        std::wstring detail = L"No page target attached, so the boot script was not injected.";
        if (!observed.empty()) {
            detail += L" Events seen: " + observed;
        }
        if (!error.empty()) {
            detail += L" " + error;
        }
        error = detail;
        log_event(L"error", L"inject", error.c_str());
        return false;
    }
    log_event(L"info", L"inject", (L"A page target attached as session '" + utf8_to_wide_lossy(session_id) + L"'.").c_str());
    if (!pipe.call_in_session(session_id, "Runtime.enable", "{}", reply, error)) {
        return false;
    }
    // Attaching pauses the page until this runs. Leaving it paused is a black window.
    if (!pipe.call_in_session(session_id, "Runtime.runIfWaitingForDebugger", "{}", reply, error)) {
        log_event(L"warn", L"cdp", error.c_str());
        error.clear();
    }
    if (!pipe.call_in_session(session_id, "Page.enable", "{}", reply, error)) {
        return false;
    }
    // Store pages refuse a steamloopback stylesheet unless this is set.
    if (!pipe.call_in_session(session_id, "Page.setBypassCSP", "{\"enabled\":true}", reply, error)) {
        log_event(L"warn", L"cdp", error.c_str());
        error.clear();
    }
    if (!pipe.call_in_session(session_id, "Runtime.addBinding", "{\"name\":\"doomnet\"}", reply, error)) {
        return false;
    }
    const std::string add_script = std::string("{\"source\":") + json_quote(script) + "}";
    if (!pipe.call_in_session(session_id, "Page.addScriptToEvaluateOnNewDocument", add_script, reply, error)) {
        return false;
    }
    const std::string evaluate = std::string("{\"expression\":") + json_quote(script) + ",\"returnByValue\":true}";
    if (!pipe.call_in_session(session_id, "Runtime.evaluate", evaluate, reply, error)) {
        return false;
    }
    if (reply.find("exceptionDetails") != std::string::npos) {
        error = L"The boot script threw inside the page, so the binding was not installed.";
        log_event(L"error", L"inject", error.c_str());
        return false;
    }
    const std::string probe_expr = "typeof window.Millennium===\"object\"&&typeof window.Millennium.callServerMethod===\"function\"";
    const std::string probe = std::string("{\"expression\":") + json_quote(probe_expr) + ",\"returnByValue\":true}";
    if (!pipe.call_in_session(session_id, "Runtime.evaluate", probe, reply, error)) {
        return false;
    }
    Json probe_root;
    std::string probe_error;
    const Json* probe_result = nullptr;
    const Json* probe_value = nullptr;
    if (json_parse(reply, probe_root, probe_error)) {
        const Json* outer = probe_root.find("result");
        probe_result = outer != nullptr ? outer->find("result") : nullptr;
        probe_value = probe_result != nullptr ? probe_result->find("value") : nullptr;
    }
    if (probe_value == nullptr || probe_value->type != Json::Type::Bool || !probe_value->b) {
        error = L"The page does not have window.Millennium.callServerMethod after the boot script ran.";
        log_event(L"error", L"inject", error.c_str());
        return false;
    }
    log_event(L"info", L"inject", L"The boot script was sent to the attached page.");
    return true;
}

}  // namespace doomnet
