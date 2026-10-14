// net.h — outbound TCP for relayed streams, and the private-address gate.
#pragma once
#include "util.h"

#include <string>

namespace net {

bool startup();

struct ConnectResult {
    SOCKET sock = INVALID_SOCKET;
    sockaddr_storage peer{};
    int peer_len = 0;
    std::string error;  // empty on success
};

// Resolve ``host`` and connect to the FIRST address it yields (one attempt,
// the same rule the Android SDK follows), within ``timeout_ms``.
ConnectResult tcp_connect(const std::string& host, int port, int timeout_ms, int idle_timeout_ms);

// True for an address that must never be reached through this machine:
// loopback, LAN, link-local, CGNAT, multicast, unspecified. Checked on the
// address actually connected to, so a public hostname that resolves into the
// user's own network is caught too.
bool is_private(const sockaddr* sa);

std::string addr_to_string(const sockaddr* sa);

bool send_all(SOCKET s, const char* data, size_t len);

}  // namespace net
