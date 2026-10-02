#include <winsock2.h>
#include <ws2tcpip.h>

#include "cdp_socket.h"

#include "log.h"
#include "text.h"

#include <cstdlib>

#include <bcrypt.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace doomnet {

namespace {

std::atomic<int> g_wsa{0};

bool wsa_up(std::wstring& error) {
    if (g_wsa.fetch_add(1) > 0) {
        return true;
    }
    WSADATA data{};
    const int started = WSAStartup(MAKEWORD(2, 2), &data);
    if (started != 0) {
        g_wsa.fetch_sub(1);
        error = L"Winsock could not start. " + windows_error_text(static_cast<unsigned long>(started));
        return false;
    }
    return true;
}

void wsa_down() {
    if (g_wsa.fetch_sub(1) == 1) {
        WSACleanup();
    }
}

std::string base64(const unsigned char* data, size_t length) {
    constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((length + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < length; i += 3) {
        const unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
    }
    if (i < length) {
        unsigned v = data[i] << 16;
        if (i + 1 < length) {
            v |= data[i + 1] << 8;
        }
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < length ? kTable[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

bool sha1(const std::string& text, unsigned char out[20], std::wstring& error) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) {
        error = L"SHA1 could not be opened. " + windows_error_text(GetLastError());
        return false;
    }
    DWORD object_length = 0;
    DWORD written = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length), &written, 0);
    std::vector<unsigned char> object(object_length);
    BCRYPT_HASH_HANDLE hash = nullptr;
    const NTSTATUS created = BCryptCreateHash(alg, &hash, object.data(), object_length, nullptr, 0, 0);
    if (created != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        error = L"SHA1 could not be created. " + windows_error_text(GetLastError());
        return false;
    }
    BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())), static_cast<ULONG>(text.size()), 0);
    const NTSTATUS finished = BCryptFinishHash(hash, out, 20, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (finished != 0) {
        error = L"SHA1 could not be finished. " + windows_error_text(GetLastError());
        return false;
    }
    return true;
}

bool random_bytes(unsigned char* out, ULONG count, std::wstring& error) {
    if (BCryptGenRandom(nullptr, out, count, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        error = L"A random WebSocket key could not be made. " + windows_error_text(GetLastError());
        return false;
    }
    return true;
}

bool parse_ws_url(const std::string& url, std::string& host, int& port, std::string& path, std::wstring& error) {
    constexpr const char* kPrefix = "ws://";
    if (url.compare(0, 5, kPrefix) != 0) {
        error = L"The page address '" + utf8_to_wide_lossy(url) + L"' is not a ws:// DevTools address. An encrypted wss:// address is not used for the local proof.";
        return false;
    }
    const std::string rest = url.substr(5);
    const size_t slash = rest.find('/');
    const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (authority.empty()) {
        error = L"The page address '" + utf8_to_wide_lossy(url) + L"' has no host.";
        return false;
    }
    if (authority.front() == '[') {
        const size_t end = authority.find(']');
        if (end == std::string::npos) {
            error = L"The page address '" + utf8_to_wide_lossy(url) + L"' has a broken IPv6 host.";
            return false;
        }
        host = authority.substr(1, end - 1);
        if (end + 1 < authority.size() && authority[end + 1] == ':') {
            port = std::atoi(authority.c_str() + end + 2);
        } else {
            port = 80;
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            host = authority;
            port = 80;
        } else {
            host = authority.substr(0, colon);
            port = std::atoi(authority.c_str() + colon + 1);
        }
    }
    if (host.empty() || port <= 0 || port > 65535) {
        error = L"The page address '" + utf8_to_wide_lossy(url) + L"' has no usable host or port.";
        return false;
    }
    return true;
}

struct FrameBuffer {
    std::string raw;
    std::string fragment;
    bool failed = false;
    std::wstring error;
    std::vector<std::string> texts;
    std::vector<std::string> pongs;

    void ingest(const char* data, size_t count) {
        raw.append(data, count);
        while (!failed) {
            if (raw.size() < 2) {
                return;
            }
            const auto* bytes = reinterpret_cast<const unsigned char*>(raw.data());
            const bool fin = (bytes[0] & 0x80) != 0;
            const int opcode = bytes[0] & 0x0F;
            const bool masked = (bytes[1] & 0x80) != 0;
            uint64_t length = bytes[1] & 0x7F;
            size_t header = 2;
            if (length == 126) {
                if (raw.size() < 4) {
                    return;
                }
                length = (static_cast<uint64_t>(bytes[2]) << 8) | bytes[3];
                header = 4;
            } else if (length == 127) {
                if (raw.size() < 10) {
                    return;
                }
                length = 0;
                for (int i = 0; i < 8; ++i) {
                    length = (length << 8) | bytes[2 + i];
                }
                header = 10;
            }
            if (masked) {
                header += 4;
            }
            if (length > 8u * 1024u * 1024u) {
                failed = true;
                error = L"A page message was larger than 8 MB, so the connection was closed.";
                return;
            }
            if (raw.size() < header + length) {
                return;
            }
            std::string payload = raw.substr(header, static_cast<size_t>(length));
            if (masked) {
                const char* key = raw.data() + header - 4;
                for (size_t i = 0; i < payload.size(); ++i) {
                    payload[i] = static_cast<char>(payload[i] ^ key[i % 4]);
                }
            }
            raw.erase(0, header + static_cast<size_t>(length));
            if (opcode == 0x8) {
                failed = true;
                error = L"The page closed the DevTools connection.";
                return;
            }
            if (opcode == 0x9) {
                pongs.push_back(std::move(payload));
                continue;
            }
            if (opcode == 0x1) {
                fragment.clear();
            }
            if (opcode == 0x1 || opcode == 0x0) {
                fragment += payload;
                if (fin) {
                    texts.push_back(std::move(fragment));
                    fragment.clear();
                }
            }
        }
    }
};

}  // namespace

struct CdpSocket::Impl {
    SOCKET socket = INVALID_SOCKET;
    HANDLE event = nullptr;
    bool open = false;
    bool wsa = false;
    FrameBuffer frames;
    std::string outgoing;

    ~Impl() { shutdown(); }

    void shutdown() {
        if (socket != INVALID_SOCKET) {
            closesocket(socket);
            socket = INVALID_SOCKET;
        }
        if (event != nullptr) {
            WSACloseEvent(event);
            event = nullptr;
        }
        open = false;
        if (wsa) {
            wsa_down();
            wsa = false;
        }
    }

    bool flush(std::wstring& error) {
        while (!outgoing.empty()) {
            const int sent = ::send(socket, outgoing.data(), static_cast<int>(outgoing.size()), 0);
            if (sent == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    return true;
                }
                error = L"Could not write to the page connection. " + windows_error_text(WSAGetLastError());
                return false;
            }
            if (sent <= 0) {
                error = L"The page connection stopped accepting bytes.";
                return false;
            }
            outgoing.erase(0, static_cast<size_t>(sent));
        }
        return true;
    }
};

CdpSocket::CdpSocket() : impl_(std::make_unique<Impl>()) {}
CdpSocket::~CdpSocket() = default;

bool CdpSocket::connect_ws(const std::string& url, std::wstring& error) {
    error.clear();
    close();
    impl_ = std::make_unique<Impl>();
    if (!wsa_up(error)) {
        return false;
    }
    impl_->wsa = true;

    std::string host;
    int port = 0;
    std::string path;
    if (!parse_ws_url(url, host, port, path, error)) {
        close();
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    const std::string port_text = std::to_string(port);
    const int looked = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result);
    if (looked != 0 || result == nullptr) {
        error = L"Could not resolve the page host '" + utf8_to_wide_lossy(host) + L"'. " + windows_error_text(static_cast<unsigned long>(looked));
        close();
        return false;
    }

    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == INVALID_SOCKET) {
            continue;
        }
        const DWORD timeout = 5000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        if (connect(sock, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
            break;
        }
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(result);
    if (sock == INVALID_SOCKET) {
        error = L"Could not connect to the page at '" + utf8_to_wide_lossy(host) + L":" + std::to_wstring(port) + L"'. " + windows_error_text(WSAGetLastError());
        close();
        return false;
    }

    unsigned char key_raw[16]{};
    if (!random_bytes(key_raw, sizeof(key_raw), error)) {
        closesocket(sock);
        close();
        return false;
    }
    const std::string key = base64(key_raw, sizeof(key_raw));
    const std::string host_header = host.find(':') == std::string::npos ? host + ":" + port_text : "[" + host + "]:" + port_text;
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host_header + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\nOrigin: http://" + host_header + "\r\n\r\n";
    size_t sent_total = 0;
    while (sent_total < request.size()) {
        const int sent = ::send(sock, request.data() + sent_total, static_cast<int>(request.size() - sent_total), 0);
        if (sent <= 0) {
            error = L"Could not send the page handshake to '" + utf8_to_wide_lossy(url) + L"'. " + windows_error_text(WSAGetLastError());
            closesocket(sock);
            close();
            return false;
        }
        sent_total += static_cast<size_t>(sent);
    }

    std::string response;
    char buffer[4096];
    while (response.find("\r\n\r\n") == std::string::npos) {
        const int got = recv(sock, buffer, sizeof(buffer), 0);
        if (got <= 0) {
            error = L"The page handshake at '" + utf8_to_wide_lossy(url) + L"' ended before a reply. " + windows_error_text(WSAGetLastError());
            closesocket(sock);
            close();
            return false;
        }
        response.append(buffer, static_cast<size_t>(got));
        if (response.size() > 65536) {
            error = L"The page handshake reply from '" + utf8_to_wide_lossy(url) + L"' was larger than 64 KB.";
            closesocket(sock);
            close();
            return false;
        }
    }
    const size_t header_end = response.find("\r\n\r\n");
    const std::string headers = response.substr(0, header_end);
    if (headers.find("101") == std::string::npos) {
        error = L"The page at '" + utf8_to_wide_lossy(url) + L"' did not accept the DevTools connection.";
        closesocket(sock);
        close();
        return false;
    }
    unsigned char digest[20]{};
    if (!sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", digest, error)) {
        closesocket(sock);
        close();
        return false;
    }
    const std::string accept = base64(digest, sizeof(digest));
    if (headers.find(accept) == std::string::npos) {
        error = L"The page at '" + utf8_to_wide_lossy(url) + L"' answered the handshake with the wrong accept value.";
        closesocket(sock);
        close();
        return false;
    }

    HANDLE event = WSACreateEvent();
    if (event == nullptr || WSAEventSelect(sock, event, FD_READ | FD_WRITE | FD_CLOSE) != 0) {
        error = L"Could not wait on the page connection. " + windows_error_text(WSAGetLastError());
        if (event != nullptr) {
            WSACloseEvent(event);
        }
        closesocket(sock);
        close();
        return false;
    }

    impl_->socket = sock;
    impl_->event = event;
    impl_->open = true;
    if (header_end + 4 < response.size()) {
        impl_->frames.ingest(response.data() + header_end + 4, response.size() - (header_end + 4));
    }
    log_event(L"info", L"cdp", (L"Connected to the page socket at '" + utf8_to_wide_lossy(url) + L"'.").c_str());
    return true;
}

bool CdpSocket::send_text(const std::string& text, std::wstring& error) {
    if (!impl_->open) {
        error = L"The page connection is not open.";
        return false;
    }
    unsigned char mask[4]{};
    if (!random_bytes(mask, sizeof(mask), error)) {
        return false;
    }
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    const size_t length = text.size();
    if (length < 126) {
        frame.push_back(static_cast<char>(0x80 | length));
    } else if (length <= 0xFFFF) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((length >> 8) & 0xFF));
        frame.push_back(static_cast<char>(length & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((static_cast<uint64_t>(length) >> shift) & 0xFF));
        }
    }
    frame.append(reinterpret_cast<char*>(mask), 4);
    for (size_t i = 0; i < text.size(); ++i) {
        frame.push_back(static_cast<char>(text[i] ^ mask[i % 4]));
    }
    impl_->outgoing += frame;
    if (!impl_->flush(error)) {
        impl_->open = false;
        return false;
    }
    return true;
}

bool CdpSocket::pump(std::vector<std::string>& messages, std::wstring& error) {
    if (!impl_->open) {
        error = L"The page connection is not open.";
        return false;
    }
    WSANETWORKEVENTS network{};
    if (WSAEnumNetworkEvents(impl_->socket, impl_->event, &network) != 0) {
        error = L"Could not read the page connection's state. " + windows_error_text(WSAGetLastError());
        impl_->open = false;
        return false;
    }
    if ((network.lNetworkEvents & FD_WRITE) != 0) {
        if (!impl_->flush(error)) {
            impl_->open = false;
            return false;
        }
    }
    if ((network.lNetworkEvents & FD_READ) != 0) {
        char buffer[65536];
        for (;;) {
            const int got = recv(impl_->socket, buffer, sizeof(buffer), 0);
            if (got == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    break;
                }
                error = L"Could not read from the page connection. " + windows_error_text(WSAGetLastError());
                impl_->open = false;
                return false;
            }
            if (got == 0) {
                error = L"The page closed the DevTools connection.";
                impl_->open = false;
                return false;
            }
            impl_->frames.ingest(buffer, static_cast<size_t>(got));
            if (impl_->frames.failed) {
                error = impl_->frames.error;
                impl_->open = false;
                return false;
            }
        }
    }
    if ((network.lNetworkEvents & FD_CLOSE) != 0 && impl_->open) {
        char buffer[4096];
        const int got = recv(impl_->socket, buffer, sizeof(buffer), 0);
        if (got > 0) {
            impl_->frames.ingest(buffer, static_cast<size_t>(got));
        }
        error = L"The page closed the DevTools connection.";
        impl_->open = false;
    }
    for (const std::string& ping : impl_->frames.pongs) {
        if (ping.size() > 125) {
            continue;
        }
        unsigned char mask[4]{};
        std::wstring ignored;
        random_bytes(mask, sizeof(mask), ignored);
        std::string frame;
        frame.push_back(static_cast<char>(0x8A));
        frame.push_back(static_cast<char>(0x80 | ping.size()));
        frame.append(reinterpret_cast<char*>(mask), 4);
        for (size_t i = 0; i < ping.size(); ++i) {
            frame.push_back(static_cast<char>(ping[i] ^ mask[i % 4]));
        }
        impl_->outgoing += frame;
    }
    impl_->frames.pongs.clear();
    if (!impl_->flush(error) && impl_->open) {
        impl_->open = false;
        return false;
    }
    messages.insert(messages.end(), impl_->frames.texts.begin(), impl_->frames.texts.end());
    impl_->frames.texts.clear();
    if (impl_->frames.failed) {
        error = impl_->frames.error;
        impl_->open = false;
        return false;
    }
    return impl_->open || !messages.empty();
}

void CdpSocket::take_pending(std::vector<std::string>& messages) {
    if (!impl_) {
        return;
    }
    messages.insert(messages.end(), impl_->frames.texts.begin(), impl_->frames.texts.end());
    impl_->frames.texts.clear();
}

void CdpSocket::close() {
    if (impl_) {
        impl_->shutdown();
    }
}

void* CdpSocket::event_handle() const {
    return impl_ ? impl_->event : nullptr;
}

bool CdpSocket::is_open() const {
    return impl_ && impl_->open;
}

}  // namespace doomnet
