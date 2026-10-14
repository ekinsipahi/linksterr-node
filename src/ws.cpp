#include "ws.h"

#pragma comment(lib, "winhttp.lib")

namespace ws {

bool parse_url(const std::string& url, Url& out, std::string& err) {
    std::string rest;
    if (url.rfind("wss://", 0) == 0) { out.secure = true; rest = url.substr(6); }
    else if (url.rfind("ws://", 0) == 0) { out.secure = false; rest = url.substr(5); }
    else { err = "server URL must start with ws:// or wss://"; return false; }
    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    std::string host = hostport;
    out.port = out.secure ? 443 : 80;
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
        host = hostport.substr(0, colon);
        int p = atoi(hostport.c_str() + colon + 1);
        if (p <= 0 || p > 65535) { err = "bad port in server URL"; return false; }
        out.port = (INTERNET_PORT)p;
    }
    if (host.empty()) { err = "server URL has no host"; return false; }
    out.host = util::widen(host);
    out.path = util::widen(path);
    return true;
}

WebSocket::~WebSocket() { release(); }

void WebSocket::release() {
    if (socket_) { WinHttpCloseHandle(socket_); socket_ = nullptr; }
    if (connect_) { WinHttpCloseHandle(connect_); connect_ = nullptr; }
    if (session_) { WinHttpCloseHandle(session_); session_ = nullptr; }
}

bool WebSocket::connect(const std::string& url, const std::wstring& user_agent, std::string& err) {
    release();
    closed_ = false;
    Url u;
    if (!parse_url(url, u, err)) return false;

    // Automatic proxy discovery so a machine behind a corporate/system proxy
    // still reaches the coordinator; home PCs just connect directly.
    session_ = WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) session_ = WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) { err = "WinHttpOpen: " + util::win_error(GetLastError()); return false; }
    WinHttpSetTimeouts(session_, 15000, 15000, 15000, 0);  // resolve/connect/send; receive unbounded

    connect_ = WinHttpConnect(session_, u.host.c_str(), u.port, 0);
    if (!connect_) { err = "WinHttpConnect: " + util::win_error(GetLastError()); release(); return false; }

    HINTERNET request = WinHttpOpenRequest(connect_, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) { err = "WinHttpOpenRequest: " + util::win_error(GetLastError()); release(); return false; }
    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        err = "WebSocket upgrade option: " + util::win_error(GetLastError());
        WinHttpCloseHandle(request);
        release();
        return false;
    }
    // Client-side keep-alive pings every 20 s (the coordinator drops a device
    // silent for 90 s; the minimum WinHTTP allows is 15 s).
    DWORD keepalive_ms = 20000;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keepalive_ms, sizeof keepalive_ms))
        WinHttpSetOption(session_, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keepalive_ms, sizeof keepalive_ms);

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        err = "connect: " + util::win_error(GetLastError());
        WinHttpCloseHandle(request);
        release();
        return false;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 101) {
        err = "server answered HTTP " + std::to_string(status) + " instead of a WebSocket upgrade";
        WinHttpCloseHandle(request);
        release();
        return false;
    }
    socket_ = WinHttpWebSocketCompleteUpgrade(request, 0);
    WinHttpCloseHandle(request);
    if (!socket_) { err = "WebSocket upgrade: " + util::win_error(GetLastError()); release(); return false; }
    return true;
}

bool WebSocket::send_text(const std::string& text) {
    std::lock_guard<std::mutex> lock(send_mu_);
    if (!socket_ || closed_) return false;
    DWORD rc = WinHttpWebSocketSend(socket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                    (PVOID)text.data(), (DWORD)text.size());
    return rc == NO_ERROR;
}

bool WebSocket::send_binary(const void* data, size_t len) {
    std::lock_guard<std::mutex> lock(send_mu_);
    if (!socket_ || closed_) return false;
    DWORD rc = WinHttpWebSocketSend(socket_, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                    (PVOID)data, (DWORD)len);
    return rc == NO_ERROR;
}

int WebSocket::receive(std::string& out, bool& is_text, std::string& err) {
    out.clear();
    if (!socket_) { err = "not connected"; return -1; }
    if (buffer_.size() < 65536) buffer_.resize(65536);
    while (true) {
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD rc = WinHttpWebSocketReceive(socket_, buffer_.data(), (DWORD)buffer_.size(), &got, &type);
        if (rc != NO_ERROR) {
            if (closed_) return 0;
            err = util::win_error(rc);
            return -1;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return 0;
        out.append(buffer_.data(), got);
        if (out.size() > (1u << 20)) { err = "message larger than 1 MiB"; return -1; }
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) { is_text = true; return 1; }
        if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) { is_text = false; return 1; }
        // *_FRAGMENT_BUFFER_TYPE: more of the same message follows.
    }
}

void WebSocket::close(USHORT code, const char* reason) {
    if (!socket_ || closed_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(send_mu_);
        WinHttpWebSocketClose(socket_, code, (PVOID)reason, (DWORD)strlen(reason));
    }
    // Closing the handle makes a receive blocked on another thread return.
    HINTERNET s = socket_;
    socket_ = nullptr;
    WinHttpCloseHandle(s);
}

}  // namespace ws
