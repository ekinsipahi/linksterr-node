// ws.h — a WebSocket client on WinHTTP: TLS, proxies and ping/pong come from
// Windows, so the exe ships with no third-party code at all.
#pragma once
#include "util.h"

#include <winhttp.h>

#include <atomic>
#include <mutex>
#include <string>

namespace ws {

struct Url {
    bool secure = true;
    std::wstring host;
    INTERNET_PORT port = 443;
    std::wstring path = L"/";
};

bool parse_url(const std::string& url, Url& out, std::string& err);

class WebSocket {
public:
    WebSocket() = default;
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    bool connect(const std::string& url, const std::wstring& user_agent, std::string& err);
    bool send_text(const std::string& text);
    bool send_binary(const void* data, size_t len);
    // 1 = a whole message is in ``out``; 0 = the peer closed; -1 = error.
    int receive(std::string& out, bool& is_text, std::string& err);
    // Safe from another thread: it also unblocks a pending receive.
    void close(USHORT code = 1000, const char* reason = "");
    bool open() const { return socket_ != nullptr && !closed_; }

private:
    void release();
    HINTERNET session_ = nullptr;
    HINTERNET connect_ = nullptr;
    HINTERNET socket_ = nullptr;
    std::mutex send_mu_;
    std::atomic<bool> closed_{false};
    std::string buffer_;
};

}  // namespace ws
