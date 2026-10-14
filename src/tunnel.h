// tunnel.h — one account's connection to the coordinator: the hello/welcome
// handshake, the stream relay, keep-alive, stats, and reconnects.
//
// Wire contract (byte-for-byte the one the Android SDK speaks):
//   text frames   JSON control: hello → welcome | error; stats; bye
//   binary frames [1B op][4B stream id, big-endian][payload]
//                 OPEN=1 (payload "host:port") DATA=2 CLOSE=3 OPEN_OK=4 OPEN_ERR=5
// The coordinator opens every stream; this side never initiates one.
#pragma once
#include "util.h"
#include "ws.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

struct NodeConfig {
    std::string server = "wss://node.ipsterr.com";
    int max_streams = 16;
    // Ports the coordinator may ask this PC to reach; empty = any. Mirrors the
    // coordinator's own default so the consent text ("ordinary web traffic")
    // is enforced here too, not only trusted.
    std::set<int> allowed_ports = {80, 443, 8080, 8443};
    bool allow_private = false;      // local testing only: targets on this LAN
    bool follow_server_ports = true; // adopt the coordinator's allowedPorts from the welcome
    std::string pool = "linksterr";  // "" = plain SDK mode (needs api_key)
    std::string api_key;             // SDK mode only
    std::string app_id = "com.linksterr.node";
    std::string version = "1.1.0";
    std::wstring data_dir;
};

struct Account {
    std::string key;    // issued by Linksterr — the only credential
    std::string label;  // shown locally only (e.g. which e-mail it belongs to)
};

class Tunnel {
public:
    enum class Outcome { Stopped, Refused };

    Tunnel(const NodeConfig& cfg, Account account, int index);
    Outcome run();   // blocks; reconnects until stop() or a terminal refusal
    void stop();     // safe from any thread (the Ctrl+C handler calls it)
    const std::string& tag() const { return tag_; }

private:
    enum class SessionEnd { Dropped, RetryAfter, Refused, Stopped };

    SessionEnd session(int& retry_after_s);
    SessionEnd handshake(int& retry_after_s);
    void handle_text(const std::string& text, SessionEnd& end, int& retry_after_s);
    void handle_binary(const std::string& frame);
    void open_stream(uint32_t sid, const std::string& target);
    void stream_worker(uint32_t sid, std::string host, int port);
    void close_stream(uint32_t sid);
    void close_all_streams();
    void wait_workers(int timeout_ms);
    bool send_frame(uint8_t op, uint32_t sid, const char* payload = nullptr, size_t len = 0);
    void keepalive_loop();
    void wait_or_stop(int ms);
    std::wstring token_path() const;
    void log_summary();

    NodeConfig cfg_;
    Account account_;
    std::string tag_;
    ws::WebSocket ws_;

    std::atomic<bool> running_{true};
    std::atomic<bool> session_alive_{false};
    std::atomic<bool> welcomed_{false};
    std::mutex cv_mu_;
    std::condition_variable cv_;

    std::mutex streams_mu_;
    std::map<uint32_t, SOCKET> streams_;
    std::atomic<int> workers_{0};
    int effective_max_streams_ = 16;

    std::string token_;
    std::string device_id_;
    std::string country_;
    int heartbeat_s_ = 30;
    int stats_s_ = 60;

    std::atomic<uint64_t> bytes_up_{0};    // coordinator → destination
    std::atomic<uint64_t> bytes_down_{0};  // destination → coordinator
    std::atomic<uint64_t> served_{0};
    std::thread keepalive_;
};
