#include "page_bridge.h"

#include "cdp_socket.h"
#include "json_wire.h"
#include "log.h"
#include "text.h"

#include <winhttp.h>

#include "boot_script.inc"
#include "streamtunes_client.inc"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace doomnet {

namespace {

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::wstring error;
    std::string text;

    void finish(bool succeeded, std::wstring why, std::string value = {}) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (done) {
                return;
            }
            ok = succeeded;
            error = std::move(why);
            text = std::move(value);
            done = true;
        }
        cv.notify_all();
    }

    bool wait_for(unsigned long timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return done; });
    }
};

std::wstring page_exception(const Json& details) {
    std::string text = details.key_string("text");
    if (const Json* exception = details.find("exception")) {
        const std::string description = exception->key_string("description");
        if (!description.empty()) {
            text = description;
        }
    }
    if (text.empty()) {
        text = "the page raised an exception";
    }
    if (text.size() > 500) {
        text.resize(500);
    }
    return L"The page raised: " + utf8_to_wide_lossy(text);
}

bool remote_value(const Json& result, Json& value, std::wstring& error) {
    if (const Json* details = result.find("exceptionDetails")) {
        error = page_exception(*details);
        return false;
    }
    const Json* remote = result.find("result");
    if (remote == nullptr) {
        error = L"The page reply had no result.";
        return false;
    }
    if (const Json* inner = remote->find("value")) {
        value = *inner;
        return true;
    }
    if (remote->key_string("type") == "undefined") {
        value = Json{};
        value.type = Json::Type::Null;
        return true;
    }
    error = L"The page reply had no value.";
    return false;
}

std::string value_text(const Json& value) {
    switch (value.type) {
        case Json::Type::String:
            return value.s;
        case Json::Type::Bool:
            return value.b ? "true" : "false";
        case Json::Type::Number:
            return std::to_string(value.n);
        case Json::Type::Null:
            return "null";
        default:
            return {};
    }
}

std::string http_get(const std::wstring& host, int port, const std::wstring& path, std::wstring& error) {
    error.clear();
    HINTERNET session = WinHttpOpen(L"DoomNet", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        error = L"Could not open an HTTP session for the browser's page list. " + windows_error_text(GetLastError());
        return {};
    }
    WinHttpSetTimeouts(session, 2000, 2000, 2000, 2000);
    HINTERNET connect = WinHttpConnect(session, host.c_str(), static_cast<INTERNET_PORT>(port), 0);
    if (connect == nullptr) {
        error = L"Could not reach the browser's page list at " + host + L":" + std::to_wstring(port) + L". " + windows_error_text(GetLastError());
        WinHttpCloseHandle(session);
        return {};
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (request == nullptr || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr)) {
        error = L"The browser's page list at " + host + L":" + std::to_wstring(port) + L" did not answer. " + windows_error_text(GetLastError());
        if (request != nullptr) {
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return {};
    }
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            error = L"The browser's page list could not be read. " + windows_error_text(GetLastError());
            body.clear();
            break;
        }
        if (available == 0) {
            break;
        }
        const size_t at = body.size();
        body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, body.data() + at, available, &read)) {
            error = L"The browser's page list could not be read. " + windows_error_text(GetLastError());
            body.clear();
            break;
        }
        body.resize(at + read);
        if (body.size() > 4u * 1024u * 1024u) {
            error = L"The browser's page list was larger than 4 MB.";
            body.clear();
            break;
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return body;
}

}  // namespace

const char* boot_script_text() {
    return kBootScript;
}

namespace {
std::string g_boot_preamble;
}

void set_boot_preamble(const std::string& javascript) {
    g_boot_preamble = javascript;
}

std::string boot_script_for_page() {
    const char* boot = boot_script_text();
    const std::string body = std::string(kStreamTunesClientScript) + "\n" + (boot != nullptr ? boot : "");
    if (g_boot_preamble.empty()) {
        return body;
    }
    return g_boot_preamble + "\n" + body;
}

struct FrontCall {
    uint64_t co_id = 0;
    uint64_t cdp_id = 0;
    uint64_t started_ms = 0;
    std::string method;
};

struct PageBridge::Impl : std::enable_shared_from_this<PageBridge::Impl> {
    Bus& bus;
    BrowserThread* browser = nullptr;
    CdpSocket socket;
    bool source_added = false;
    bool dead_logged = false;
    bool closing = false;
    std::atomic<bool> connected{false};
    uint64_t next_cdp = 1;
    std::unordered_set<uint64_t> page_calls;
    std::unordered_map<uint64_t, std::function<void(const Json*, const std::wstring&)>> callbacks;
    std::unordered_map<uint64_t, FrontCall> fronts;
    std::shared_ptr<Gate> opening;

    explicit Impl(Bus& message_bus) : bus(message_bus) {}

    uint64_t send(const std::string& method, const std::string& params, std::function<void(const Json*, const std::wstring&)> done) {
        const uint64_t id = next_cdp++;
        const std::string text = "{\"id\":" + std::to_string(id) + ",\"method\":" + json_quote(method) + ",\"params\":" + params + "}";
        std::wstring error;
        if (!socket.send_text(text, error)) {
            if (done) {
                done(nullptr, error);
            }
            return 0;
        }
        callbacks.emplace(id, std::move(done));
        return id;
    }

    void arm_sweep() {
        if (closing || browser == nullptr || !socket.is_open()) {
            return;
        }
        const std::shared_ptr<Impl> self = shared_from_this();
        browser->after(1000, [self] { self->sweep(); });
    }

    void sweep() {
        if (closing || browser == nullptr) {
            return;
        }
        const uint64_t now = monotonic_ms();
        std::vector<FrontCall> late;
        for (auto it = fronts.begin(); it != fronts.end();) {
            if (now - it->second.started_ms >= 15000) {
                late.push_back(it->second);
                callbacks.erase(it->first);
                it = fronts.erase(it);
            } else {
                ++it;
            }
        }
        for (const FrontCall& call : late) {
            const std::wstring detail = L"The page did not answer '" + utf8_to_wide_lossy(call.method) + L"' within 15 seconds.";
            log_event(L"warn", L"cdp", detail.c_str());
            browser->fail_frontend(call.co_id, "The page did not answer '" + call.method + "' within 15 seconds.");
        }
        if (!closing && socket.is_open()) {
            arm_sweep();
        }
    }

    void fail_fronts(const std::wstring& why) {
        if (browser == nullptr) {
            fronts.clear();
            return;
        }
        const std::string text = wide_to_utf8(why);
        std::vector<FrontCall> left;
        for (auto& entry : fronts) {
            left.push_back(entry.second);
        }
        fronts.clear();
        callbacks.clear();
        for (const FrontCall& call : left) {
            browser->fail_frontend(call.co_id, text);
        }
    }

    void on_dead(const std::wstring& why) {
        if (!dead_logged) {
            dead_logged = true;
            log_event(L"warn", L"cdp", why.c_str());
        }
        fail_fronts(why);
        if (opening) {
            opening->finish(false, why);
            opening.reset();
        }
    }

    void handle_message(const std::string& text) {
        Json root;
        std::string parse_error;
        if (!json_parse(text, root, parse_error)) {
            log_event(L"warn", L"cdp", (L"A page message was not JSON (" + utf8_to_wide_lossy(parse_error) + L"). It was ignored.").c_str());
            return;
        }
        if (const Json* id = root.find("id")) {
            if (id->type != Json::Type::Number) {
                return;
            }
            const auto found = callbacks.find(static_cast<uint64_t>(id->n));
            if (found == callbacks.end()) {
                return;
            }
            auto done = std::move(found->second);
            callbacks.erase(found);
            fronts.erase(static_cast<uint64_t>(id->n));
            if (const Json* failure = root.find("error")) {
                const std::string message = failure->key_string("message");
                done(nullptr, utf8_to_wide_lossy(message.empty() ? "the page rejected a DevTools command" : message));
            } else if (const Json* result = root.find("result")) {
                done(result, L"");
            } else {
                done(nullptr, L"The page reply had no result.");
            }
            return;
        }
        if (root.key_string("method") == "Runtime.bindingCalled") {
            on_binding(root.find("params"));
        }
    }

    void on_binding(const Json* params) {
        if (params == nullptr) {
            return;
        }
        const std::string payload = params->key_string("payload");
        Json body;
        std::string parse_error;
        if (!json_parse(payload, body, parse_error)) {
            log_event(L"warn", L"cdp", (L"A page call was not JSON (" + utf8_to_wide_lossy(parse_error) + L"). It was ignored.").c_str());
            return;
        }
        const Json* id_value = body.find("id");
        if (id_value == nullptr || id_value->type != Json::Type::Number) {
            log_event(L"warn", L"cdp", L"A page call had no numeric id. It was ignored.");
            return;
        }
        const uint64_t id = static_cast<uint64_t>(id_value->n);
        const std::string method = body.key_string("method");
        const std::string plugin = body.key_string("plugin");
        const Json* args = body.find("args");
        if (args == nullptr || args->type != Json::Type::Array) {
            reject_page(id, "argsArray must be an array");
            return;
        }
        std::vector<WireValue> values;
        for (const Json& arg : args->a) {
            WireValue value;
            if (arg.type == Json::Type::String) {
                value.kind = WireValue::Kind::String;
                value.text = arg.s;
            } else if (arg.type == Json::Type::Number) {
                value.kind = WireValue::Kind::Number;
                value.number = arg.n;
            } else if (arg.type == Json::Type::Bool) {
                value.kind = WireValue::Kind::Boolean;
                value.boolean = arg.b;
            } else {
                reject_page(id, "Millennium's IPC can only handle [boolean, string, number]");
                return;
            }
            values.push_back(std::move(value));
        }
        page_calls.insert(id);
        const std::wstring detail = L"The page called '" + utf8_to_wide_lossy(method) + L"' (plugin '" + utf8_to_wide_lossy(plugin) + L"', call " + std::to_wstring(id) + L").";
        log_event(L"info", L"cdp", detail.c_str());
        CallMsg call;
        call.call_id = id;
        call.method = method;
        call.args = std::move(values);
        bus.to_state(std::move(call));
    }

    void reject_page(uint64_t id, const std::string& message) {
        page_calls.insert(id);
        ResolveMsg failed;
        failed.call_id = id;
        failed.ok = false;
        failed.json = message;
        settle(std::move(failed));
    }

    void settle(ResolveMsg message) {
        if (page_calls.erase(message.call_id) == 0) {
            return;
        }
        if (browser == nullptr || !socket.is_open()) {
            return;
        }
        const std::string expr = message.ok ? "window.__doomnetSettle(" + std::to_string(message.call_id) + ",true," + message.json + ")"
                                            : "window.__doomnetSettle(" + std::to_string(message.call_id) + ",false," + json_quote(message.json) + ")";
        send("Runtime.evaluate", "{\"expression\":" + json_quote(expr) + ",\"returnByValue\":true}", [](const Json*, const std::wstring& error) {
            if (!error.empty()) {
                log_event(L"warn", L"cdp", (L"Could not settle a page call. " + error).c_str());
            }
        });
    }

    void call_page(FrontendMsg message) {
        if (browser == nullptr || !socket.is_open()) {
            if (browser != nullptr) {
                browser->fail_frontend(message.co_id, "No page is connected.");
            }
            return;
        }
        const std::string params = message.params_json.empty() ? "[]" : message.params_json;
        const std::string expr = "window.__doomnetCall(" + json_quote(message.method) + "," + params + ")";
        const std::wstring detail = L"Calling '" + utf8_to_wide_lossy(message.method) + L"' on the page for coroutine " + std::to_wstring(message.co_id) + L".";
        log_event(L"info", L"cdp", detail.c_str());
        const uint64_t co_id = message.co_id;
        const std::string method = message.method;
        const std::shared_ptr<Impl> self = shared_from_this();
        const uint64_t cdp_id = send("Runtime.evaluate", "{\"expression\":" + json_quote(expr) + ",\"awaitPromise\":true,\"returnByValue\":true}", [self, co_id, method](const Json* result, const std::wstring& error) {
            if (self->browser == nullptr) {
                return;
            }
            if (!error.empty() || result == nullptr) {
                self->browser->fail_frontend(co_id, wide_to_utf8(error.empty() ? std::wstring(L"The page reply had no result.") : error));
                return;
            }
            Json value;
            std::wstring read_error;
            if (!remote_value(*result, value, read_error)) {
                self->browser->fail_frontend(co_id, wide_to_utf8(read_error));
                return;
            }
            const std::string kind = value.key_string("k");
            if (kind == "fail") {
                const std::string why = value.key_string("e");
                self->browser->fail_frontend(co_id, why.empty() ? "no such method on the page" : why);
                return;
            }
            WireValue packed;
            if (kind == "bool") {
                const Json* bit = value.find("v");
                packed.kind = WireValue::Kind::Boolean;
                packed.boolean = bit != nullptr && bit->type == Json::Type::Bool && bit->b;
            } else if (kind == "num") {
                const Json* number = value.find("v");
                packed.kind = WireValue::Kind::Number;
                packed.number = number != nullptr && number->type == Json::Type::Number ? number->n : 0;
            } else if (kind == "str") {
                packed.kind = WireValue::Kind::String;
                packed.text = value.key_string("v");
            } else if (kind == "json") {
                packed.kind = WireValue::Kind::Json;
                packed.text = value.key_string("v");
            } else {
                packed.kind = WireValue::Kind::Nil;
            }
            self->browser->complete_frontend(co_id, std::move(packed));
        });
        if (cdp_id != 0) {
            FrontCall front;
            front.co_id = co_id;
            front.cdp_id = cdp_id;
            front.started_ms = monotonic_ms();
            front.method = method;
            fronts.emplace(cdp_id, std::move(front));
        }
    }

    void on_socket() {
        std::vector<std::string> messages;
        std::wstring error;
        socket.pump(messages, error);
        for (const std::string& text : messages) {
            handle_message(text);
        }
        if (!socket.is_open()) {
            on_dead(error.empty() ? std::wstring(L"The page connection closed.") : error);
        }
    }

    void confirm(const std::shared_ptr<Gate>& gate) {
        const char* expr = "(typeof window.Millennium!==\"undefined\"&&typeof window.Millennium.callServerMethod===\"function\"&&typeof window.__doomnetCall===\"function\")?\"ready\":\"missing\"";
        const std::shared_ptr<Impl> self = shared_from_this();
        send("Runtime.evaluate", std::string("{\"expression\":") + json_quote(expr) + ",\"returnByValue\":true}", [self, gate](const Json* result, const std::wstring& error) {
            if (!error.empty() || result == nullptr) {
                gate->finish(false, error.empty() ? std::wstring(L"The boot script did not report a result.") : error);
                return;
            }
            Json value;
            std::wstring read_error;
            if (!remote_value(*result, value, read_error)) {
                gate->finish(false, read_error);
                return;
            }
            if (value_text(value) != "ready") {
                gate->finish(false, L"The boot script did not install window.Millennium.callServerMethod.");
                return;
            }
            self->connected = true;
            self->opening.reset();
            log_event(L"info", L"cdp", L"The page binding 'doomnet' is installed and the boot script ran.");
            self->arm_sweep();
            gate->finish(true, L"");
        });
    }

    void install(const std::shared_ptr<Gate>& gate) {
        opening = gate;
        using Step = std::pair<std::string, std::string>;
        auto steps = std::make_shared<std::vector<Step>>();
        steps->push_back({"Runtime.enable", "{}"});
        steps->push_back({"Page.enable", "{}"});
        steps->push_back({"Runtime.addBinding", "{\"name\":\"doomnet\"}"});
        // The raw boot script has no skin config. A later copy then sees the
        // binding and used to return before the player and the skin were added.
        const std::string page_script = boot_script_for_page();
        steps->push_back({"Page.addScriptToEvaluateOnNewDocument", std::string("{\"source\":") + json_quote(page_script) + "}"});
        steps->push_back({"Runtime.evaluate", std::string("{\"expression\":") + json_quote(page_script) + ",\"returnByValue\":true}"});
        auto index = std::make_shared<size_t>(0);
        auto next = std::make_shared<std::function<void()>>();
        const std::shared_ptr<Impl> self = shared_from_this();
        *next = [self, gate, steps, index, next]() {
            if (self->closing) {
                gate->finish(false, L"The page connection was closed.");
                return;
            }
            if (*index >= steps->size()) {
                self->confirm(gate);
                return;
            }
            const std::string method = (*steps)[*index].first;
            const std::string params = (*steps)[*index].second;
            ++*index;
            self->send(method, params, [self, gate, next, method](const Json* result, const std::wstring& error) {
                if (!error.empty()) {
                    gate->finish(false, utf8_to_wide_lossy(method) + L" failed. " + error);
                    return;
                }
                if (result != nullptr && result->find("exceptionDetails") != nullptr) {
                    gate->finish(false, utf8_to_wide_lossy(method) + L" failed. " + page_exception(*result->find("exceptionDetails")));
                    return;
                }
                (*next)();
            });
        };
        (*next)();
    }
};

PageBridge::PageBridge(Bus& bus) : impl_(std::make_shared<Impl>(bus)) {}

PageBridge::~PageBridge() {
    close();
}

void PageBridge::settle(ResolveMsg message) {
    impl_->settle(std::move(message));
}

void PageBridge::call_page(FrontendMsg message) {
    impl_->call_page(std::move(message));
}

bool PageBridge::connected() const {
    return impl_ && impl_->connected.load();
}

bool PageBridge::open(BrowserThread& browser, const std::string& websocket_url, std::wstring& error) {
    error.clear();
    impl_->browser = &browser;
    impl_->closing = false;
    impl_->dead_logged = false;
    auto gate = std::make_shared<Gate>();
    const std::shared_ptr<Impl> self = impl_;
    browser.post([self, websocket_url, gate] {
        std::wstring connect_error;
        if (!self->socket.connect_ws(websocket_url, connect_error)) {
            gate->finish(false, connect_error);
            return;
        }
        std::vector<std::string> early;
        self->socket.take_pending(early);
        for (const std::string& text : early) {
            self->handle_message(text);
        }
        if (self->browser == nullptr || !self->browser->add_source(self->socket.event_handle(), [self] { self->on_socket(); })) {
            self->socket.close();
            gate->finish(false, L"The browser thread could not watch the page connection.");
            return;
        }
        self->source_added = true;
        self->install(gate);
    });
    if (!gate->wait_for(20000)) {
        error = L"The page did not finish installing the boot script within 20 seconds.";
        return false;
    }
    error = gate->error;
    return gate->ok;
}

bool PageBridge::evaluate(const std::string& expression, bool await_promise, std::string& result, std::wstring& error) {
    result.clear();
    error.clear();
    if (!impl_ || impl_->browser == nullptr) {
        error = L"No page is connected.";
        return false;
    }
    auto gate = std::make_shared<Gate>();
    const std::shared_ptr<Impl> self = impl_;
    impl_->browser->post([self, expression, await_promise, gate] {
        if (self->closing || !self->socket.is_open()) {
            gate->finish(false, L"No page is connected.");
            return;
        }
        const std::string params = std::string("{\"expression\":") + json_quote(expression) + ",\"awaitPromise\":" + (await_promise ? "true" : "false") + ",\"returnByValue\":true}";
        self->send("Runtime.evaluate", params, [gate](const Json* reply, const std::wstring& failed) {
            if (!failed.empty() || reply == nullptr) {
                gate->finish(false, failed.empty() ? std::wstring(L"The page reply had no result.") : failed);
                return;
            }
            Json value;
            std::wstring read_error;
            if (!remote_value(*reply, value, read_error)) {
                gate->finish(false, read_error);
                return;
            }
            gate->finish(true, L"", value_text(value));
        });
    });
    if (!gate->wait_for(20000)) {
        error = L"The page did not answer within 20 seconds.";
        return false;
    }
    result = gate->text;
    error = gate->error;
    return gate->ok;
}

void PageBridge::close() {
    if (!impl_ || impl_->browser == nullptr) {
        return;
    }
    auto gate = std::make_shared<Gate>();
    const std::shared_ptr<Impl> self = impl_;
    impl_->browser->post([self, gate] {
        self->closing = true;
        self->connected = false;
        if (self->source_added && self->browser != nullptr && self->socket.event_handle() != nullptr) {
            self->browser->remove_source(self->socket.event_handle());
            self->source_added = false;
        }
        self->socket.close();
        self->fail_fronts(L"The page connection was closed.");
        if (self->opening) {
            self->opening->finish(false, L"The page connection was closed.");
            self->opening.reset();
        }
        self->browser = nullptr;
        gate->finish(true, L"");
    });
    gate->wait_for(5000);
    impl_->browser = nullptr;
}

bool discover_cdp_page(int port, std::string& websocket_url, std::wstring& error) {
    websocket_url.clear();
    const std::string body = http_get(L"127.0.0.1", port, L"/json/list", error);
    if (body.empty()) {
        if (error.empty()) {
            error = L"The browser's page list at 127.0.0.1:" + std::to_wstring(port) + L" was empty.";
        }
        return false;
    }
    Json root;
    std::string parse_error;
    if (!json_parse(body, root, parse_error) || root.type != Json::Type::Array) {
        error = L"The browser's page list at 127.0.0.1:" + std::to_wstring(port) + L" was not a JSON array (" + utf8_to_wide_lossy(parse_error) + L").";
        return false;
    }
    for (const Json& target : root.a) {
        if (target.key_string("type") != "page") {
            continue;
        }
        const std::string address = target.key_string("webSocketDebuggerUrl");
        if (!address.empty()) {
            websocket_url = address;
            break;
        }
    }
    if (websocket_url.empty()) {
        error = L"The browser at 127.0.0.1:" + std::to_wstring(port) + L" listed " + std::to_wstring(root.a.size()) + L" targets and none was a page with a DevTools address.";
        return false;
    }
    const std::wstring detail = L"The browser at 127.0.0.1:" + std::to_wstring(port) + L" listed " + std::to_wstring(root.a.size()) + L" targets. Using the page socket '" + utf8_to_wide_lossy(websocket_url) + L"'.";
    log_event(L"info", L"cdp", detail.c_str());
    return true;
}

}  // namespace doomnet
