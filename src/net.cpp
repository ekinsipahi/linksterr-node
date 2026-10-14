#include "net.h"

#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace net {

bool startup() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

static bool v4_private(uint32_t a) {  // host byte order
    uint8_t b0 = (uint8_t)(a >> 24), b1 = (uint8_t)(a >> 16);
    if (b0 == 0) return true;                                 // 0.0.0.0/8 (unspecified)
    if (b0 == 10) return true;                                // 10/8
    if (b0 == 127) return true;                               // loopback
    if (b0 == 100 && (b1 & 0xC0) == 64) return true;          // 100.64/10 CGNAT
    if (b0 == 169 && b1 == 254) return true;                  // link-local
    if (b0 == 172 && (b1 & 0xF0) == 16) return true;          // 172.16/12
    if (b0 == 192 && b1 == 168) return true;                  // 192.168/16
    if (b0 >= 224) return true;                               // multicast + reserved + broadcast
    return false;
}

bool is_private(const sockaddr* sa) {
    if (!sa) return true;
    if (sa->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
        return v4_private(ntohl(in->sin_addr.s_addr));
    }
    if (sa->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
        const unsigned char* b = in6->sin6_addr.s6_addr;
        static const unsigned char zero[16] = {0};
        if (memcmp(b, zero, 16) == 0) return true;                       // ::
        if (memcmp(b, zero, 15) == 0 && b[15] == 1) return true;         // ::1
        if (memcmp(b, zero, 10) == 0 && b[10] == 0xFF && b[11] == 0xFF)  // ::ffff:a.b.c.d
            return v4_private(((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) | ((uint32_t)b[14] << 8) | b[15]);
        if ((b[0] & 0xFE) == 0xFC) return true;                          // fc00::/7 unique local
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return true;          // fe80::/10 link-local
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0xC0) return true;          // fec0::/10 site-local
        if (b[0] == 0xFF) return true;                                   // multicast
        return false;
    }
    return true;  // anything we cannot classify is refused
}

std::string addr_to_string(const sockaddr* sa) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (sa->sa_family == AF_INET)
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, buf, sizeof buf);
    else if (sa->sa_family == AF_INET6)
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, buf, sizeof buf);
    return buf;
}

ConnectResult tcp_connect(const std::string& host, int port, int timeout_ms, int idle_timeout_ms) {
    ConnectResult r;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    std::string port_text = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &res);
    if (rc != 0 || !res) {
        r.error = "resolve failed";
        return r;
    }
    // First address only: one connect, like the reference client. A resolver
    // that puts a dead address first costs one failed stream, which the
    // coordinator retries on another device.
    addrinfo* ai = res;
    SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        r.error = "socket failed";
        return r;
    }
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    rc = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        r.error = "connect failed " + std::to_string(WSAGetLastError());
        closesocket(s);
        freeaddrinfo(res);
        return r;
    }
    fd_set wset, eset;
    FD_ZERO(&wset);
    FD_ZERO(&eset);
    FD_SET(s, &wset);
    FD_SET(s, &eset);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    rc = select(0, nullptr, &wset, &eset, &tv);
    if (rc <= 0 || FD_ISSET(s, &eset)) {
        int err = 0, len = sizeof err;
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
        r.error = rc == 0 ? "connect timeout" : "connect failed " + std::to_string(err);
        closesocket(s);
        freeaddrinfo(res);
        return r;
    }
    nonblocking = 0;
    ioctlsocket(s, FIONBIO, &nonblocking);
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof nodelay);
    DWORD idle = (DWORD)idle_timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&idle, sizeof idle);

    r.peer_len = (int)ai->ai_addrlen;
    memcpy(&r.peer, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(res);
    r.sock = s;
    return r;
}

bool send_all(SOCKET s, const char* data, size_t len) {
    while (len > 0) {
        int n = send(s, data, (int)(len > 0x7fffffff ? 0x7fffffff : len), 0);
        if (n <= 0) return false;
        data += n;
        len -= (size_t)n;
    }
    return true;
}

}  // namespace net
