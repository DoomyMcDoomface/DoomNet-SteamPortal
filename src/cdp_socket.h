#pragma once

// One WebSocket to a page's DevTools endpoint. The browser thread is the only
// caller after connect_ws returns. No Lua.

#include <memory>
#include <string>
#include <vector>

namespace doomnet {

class CdpSocket {
public:
    CdpSocket();
    ~CdpSocket();
    CdpSocket(const CdpSocket&) = delete;
    CdpSocket& operator=(const CdpSocket&) = delete;

    // `url` is ws://host:port/path. The handshake blocks this thread for at most
    // a few seconds. After it returns, reads are driven by `event_handle`.
    bool connect_ws(const std::string& url, std::wstring& error);

    bool send_text(const std::string& text, std::wstring& error);

    // Read whatever the socket has ready. Complete text messages are appended
    // to `messages`. False when the connection has failed.
    bool pump(std::vector<std::string>& messages, std::wstring& error);

    // Text frames already buffered (the handshake can include the first one).
    void take_pending(std::vector<std::string>& messages);

    void close();
    void* event_handle() const;
    bool is_open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace doomnet
