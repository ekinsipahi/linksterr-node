#include "tunnel.h"

#include "json.h"
#include "net.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

namespace {

constexpr uint8_t OP_OPEN = 1, OP_DATA = 2, OP_CLOSE = 3, OP_OPEN_OK = 4, OP_OPEN_ERR = 5;
constexpr size_t HEADER = 5;
constexpr int CHUNK = 16384;
constexpr int CONNECT_TIMEOUT_MS = 15000;
constexpr int IDLE_TIMEOUT_MS = 120000;
constexpr int PROTOCOL_VERSION = 1;

uint32_t read_be32(const char* p) {
    const unsigned char* u = reinterpret_cast<const unsigned char*>(p);
    return ((uint32_t)u[0] << 24) | ((uint32_t)u[1] << 16) | ((uint32_t)u[2] << 8) | u[3];
}

void write_be32(char* p, uint32_t v) {
    p[0] = (char)(v >> 24); p[1] = (char)(v >> 16); p[2] = (char)(v >> 8); p[3] = (char)v;
}

}  // namespace

Tunnel::Tunnel(const NodeConfig& cfg, Account account, int index)
    : cfg_(cfg), account_(std::move(account)) {
    std::string who = account_.label.empty()
        ? (account_.key.size() > 10 ? account_.key.substr(0, 10) + "…" : account_.key)
        : account_.label;
    tag_ = "node" + std::to_string(index) + ":" + who;
    effective_max_streams_ = std::clamp(cfg_.max_streams, 1, 64);
    std::string stored;
    if (util::read_file(token_path(), stored)) token_ = util::trim(stored);
}

std::wstring Tunnel::token_path() const {
    return cfg_.data_dir + L"\\token-" + util::widen(util::hex64(util::fnv1a64(account_.key))) + L".txt";
}

// -- lifecycle ---------------------------------------------------------------
Tunnel::Outcome Tunnel::run() {
    double backoff = 1.0;
    std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    while (running_) {
        int retry_after = 0;
        welcomed_ = false;
        SessionEnd end = session(retry_after);
        if (!running_ || end == SessionEnd::Stopped) break;
        if (end == SessionEnd::Refused) return Outcome::Refused;
        if (welcomed_) backoff = 1.0;
        if (end == SessionEnd::RetryAfter) {
            LOGI(tag_.c_str(), "retrying in %d s", retry_after);
            wait_or_stop(retry_after * 1000);
            continue;
        }
        // Jittered exponential backoff: 1–1.5 s, 2–3 s, … capped at 60–90 s.
        double jitter = backoff * 0.5 * std::uniform_real_distribution<double>(0.0, 1.0)(rng);
        int wait_ms = (int)((backoff + jitter) * 1000.0);
        LOGI(tag_.c_str(), "reconnecting in %.1f s", wait_ms / 1000.0);
        wait_or_stop(wait_ms);
        backoff = std::min(backoff * 2.0, 60.0);
    }
    return Outcome::Stopped;
}

void Tunnel::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (ws_.open()) {
        ws_.send_text("{\"op\":\"bye\"}");
        ws_.close(1000, "stopped");
    }
}

void Tunnel::wait_or_stop(int ms) {
    std::unique_lock<std::mutex> lock(cv_mu_);
    cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return !running_.load(); });
}

// -- one connection ------------------------------------------------------------
Tunnel::SessionEnd Tunnel::session(int& retry_after_s) {
    std::string err;
    if (!ws_.connect(cfg_.server, util::widen("LinksterrNode/" + cfg_.version), err)) {
        LOGW(tag_.c_str(), "cannot reach %s: %s", cfg_.server.c_str(), err.c_str());
        return running_ ? SessionEnd::Dropped : SessionEnd::Stopped;
    }
    SessionEnd end = handshake(retry_after_s);
    if (end != SessionEnd::Dropped || !welcomed_) {
        ws_.close(1000, end == SessionEnd::Refused ? "refused" : "bye");
        return end;
    }

    session_alive_ = true;
    keepalive_ = std::thread(&Tunnel::keepalive_loop, this);
    std::string msg;
    bool is_text = false;
    end = SessionEnd::Dropped;
    while (running_) {
        int rc = ws_.receive(msg, is_text, err);
        if (rc == 1) {
            if (is_text) {
                handle_text(msg, end, retry_after_s);
                if (end != SessionEnd::Dropped) break;
            } else {
                handle_binary(msg);
            }
            continue;
        }
        if (rc == 0) LOGI(tag_.c_str(), "coordinator closed the connection");
        else if (running_) LOGW(tag_.c_str(), "connection lost: %s", err.c_str());
        break;
    }
    session_alive_ = false;
    cv_.notify_all();
    if (keepalive_.joinable()) keepalive_.join();
    close_all_streams();
    wait_workers(5000);
    ws_.close(1000, "bye");
    return running_ ? end : SessionEnd::Stopped;
}

Tunnel::SessionEnd Tunnel::handshake(int& retry_after_s) {
    json::Writer hello;
    hello.str("op", "hello").num("v", PROTOCOL_VERSION).str("apiKey", cfg_.api_key)
         .str("token", token_).str("platform", "windows").str("model", util::os_description())
         .str("appId", cfg_.app_id).str("sdk", cfg_.version)
         .num("maxStreams", std::clamp(cfg_.max_streams, 1, 64))
         // Only ever true: main() refuses to start a tunnel before the user
         // accepted the disclosure, and --withdraw deletes that acceptance.
         .boolean("consent", true);
    if (!cfg_.pool.empty()) hello.str("pool", cfg_.pool).str("account", account_.key);
    if (!ws_.send_text(hello.done())) return SessionEnd::Dropped;

    std::string msg, err;
    bool is_text = false;
    int rc = ws_.receive(msg, is_text, err);
    if (rc != 1 || !is_text) {
        if (rc < 0 && running_) LOGW(tag_.c_str(), "no answer to hello: %s", err.c_str());
        return SessionEnd::Dropped;
    }
    json::Value v;
    if (!json::parse(msg, v) || v.type != json::Value::Object) return SessionEnd::Dropped;
    std::string op = v.get_string("op");
    if (op == "error") {
        SessionEnd end = SessionEnd::Refused;
        handle_text(msg, end, retry_after_s);
        return end;
    }
    if (op != "welcome") {
        LOGW(tag_.c_str(), "unexpected first frame: %s", op.c_str());
        return SessionEnd::Dropped;
    }
    std::string token = v.get_string("token");
    if (!token.empty() && token != token_) {
        token_ = token;
        if (!util::write_file(token_path(), token_))
            LOGW(tag_.c_str(), "could not save the device token; this PC will look new next time");
    }
    device_id_ = v.get_string("deviceId", device_id_);
    country_ = v.get_string("country", "ZZ");
    if (const json::Value* config = v.get("config")) {
        int theirs = (int)config->get_number("maxStreams", cfg_.max_streams);
        effective_max_streams_ = std::clamp(std::min(theirs, cfg_.max_streams), 1, 64);
        heartbeat_s_ = std::clamp((int)config->get_number("heartbeatS", 30), 10, 60);
        stats_s_ = std::clamp((int)config->get_number("statsIntervalS", 60), 15, 600);
        // The coordinator's own port policy, so this PC enforces exactly what
        // the service enforces (an empty list there means any port).
        if (const json::Value* ports = config->get("allowedPorts"); ports && ports->type == json::Value::Array) {
            std::set<int> allowed;
            for (const json::Value& port : ports->arr)
                if (port.type == json::Value::Number && port.num >= 1 && port.num <= 65535) allowed.insert((int)port.num);
            if (cfg_.follow_server_ports) cfg_.allowed_ports = allowed;
        }
    }
    std::string label = v.get_string("label");
    LOGI(tag_.c_str(), "connected: device %s, exit country %s%s%s", device_id_.c_str(),
         country_.c_str(), v.get_bool("mobile") ? " (mobile)" : "",
         label.empty() ? "" : (", account " + label).c_str());
    if (v.has("sellable") && !v.get_bool("sellable", true)) {
        LOGW(tag_.c_str(), "this connection cannot earn right now: %s",
             v.get_string("quality_reason", "address not usable").c_str());
    }
    welcomed_ = true;
    return SessionEnd::Dropped;  // "no decision yet" — the caller runs the relay loop
}

// -- control frames ---------------------------------------------------------------
void Tunnel::handle_text(const std::string& text, SessionEnd& end, int& retry_after_s) {
    json::Value v;
    if (!json::parse(text, v) || v.type != json::Value::Object) return;
    std::string op = v.get_string("op");
    if (op != "error") return;  // nothing else is defined coordinator → device
    std::string code = v.get_string("code");
    std::string message = v.get_string("message", code);
    int retry = (int)v.get_number("retryAfterS", 0);
    if (retry > 0) {
        // Not a verdict on this account, just "not now": the pool has no slot
        // for this country at the moment (shown in red, as the owner asked —
        // it is the one message a user must not miss), or another PC on this
        // internet connection already holds the slot (yellow). Either way we
        // keep retrying on our own; the user does nothing.
        bool no_slot = code == "country_full" || v.get_string("level") == "warn";
        util::log_painted(util::Level::Warn, no_slot ? util::Paint::Red : util::Paint::Yellow,
                          tag_.c_str(), "%s", message.c_str());
        retry_after_s = std::clamp(retry, 5, 3600);
        end = SessionEnd::RetryAfter;
        return;
    }
    // bad_key / no_consent / bad_version / unknown account: looping against it
    // only burns the user's connection. Stop and tell them why.
    LOGE(tag_.c_str(), "refused by the coordinator (%s): %s", code.c_str(), message.c_str());
    end = SessionEnd::Refused;
}

// -- data frames ------------------------------------------------------------------
void Tunnel::handle_binary(const std::string& frame) {
    if (frame.size() < HEADER) return;
    uint8_t op = (uint8_t)frame[0];
    uint32_t sid = read_be32(frame.data() + 1);
    const char* payload = frame.data() + HEADER;
    size_t len = frame.size() - HEADER;
    switch (op) {
        case OP_OPEN:
            open_stream(sid, std::string(payload, len));
            break;
        case OP_DATA: {
            // Written on this thread, in order, so bytes never reorder within a
            // stream. A slow destination stalls the others meanwhile — the
            // accepted cost of one socket for everything, bounded by max streams.
            std::lock_guard<std::mutex> lock(streams_mu_);
            auto it = streams_.find(sid);
            if (it == streams_.end()) return;  // a late frame for a stream already gone
            bytes_up_ += len;
            if (!net::send_all(it->second, payload, len)) {
                closesocket(it->second);
                streams_.erase(it);
                send_frame(OP_CLOSE, sid);
            }
            break;
        }
        case OP_CLOSE:
            close_stream(sid);
            break;
        default:
            break;
    }
}

void Tunnel::open_stream(uint32_t sid, const std::string& target) {
    size_t sep = target.rfind(':');
    if (sep == std::string::npos) { send_frame(OP_OPEN_ERR, sid, "bad target", 10); return; }
    std::string host = util::trim(target.substr(0, sep));
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    int port = atoi(target.c_str() + sep + 1);
    if (host.empty() || port < 1 || port > 65535) { send_frame(OP_OPEN_ERR, sid, "bad target", 10); return; }
    if (!cfg_.allowed_ports.empty() && !cfg_.allowed_ports.count(port)) {
        send_frame(OP_OPEN_ERR, sid, "port not allowed", 16);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(streams_mu_);
        if ((int)streams_.size() + workers_ >= effective_max_streams_ + 4) {
            send_frame(OP_OPEN_ERR, sid, "device at stream limit", 22);
            return;
        }
    }
    ++workers_;
    std::thread(&Tunnel::stream_worker, this, sid, host, port).detach();
}

void Tunnel::stream_worker(uint32_t sid, std::string host, int port) {
    struct Done {
        Tunnel* t;
        ~Done() { --t->workers_; t->cv_.notify_all(); }
    } done{this};

    net::ConnectResult r = net::tcp_connect(host, port, CONNECT_TIMEOUT_MS, IDLE_TIMEOUT_MS);
    if (r.sock == INVALID_SOCKET) {
        LOGD(tag_.c_str(), "stream %u: %s:%d %s", sid, host.c_str(), port, r.error.c_str());
        send_frame(OP_OPEN_ERR, sid, r.error.data(), std::min<size_t>(r.error.size(), 100));
        return;
    }
    // Checked after connecting, on the address actually reached: a public
    // hostname resolving into this home's LAN is the case worth catching.
    if (!cfg_.allow_private && net::is_private(reinterpret_cast<sockaddr*>(&r.peer))) {
        closesocket(r.sock);
        send_frame(OP_OPEN_ERR, sid, "private address refused", 23);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(streams_mu_);
        if (!session_alive_ || (int)streams_.size() >= effective_max_streams_) {
            closesocket(r.sock);
            send_frame(OP_OPEN_ERR, sid, "device at stream limit", 22);
            return;
        }
        streams_[sid] = r.sock;
    }
    ++served_;
    // Never a byte of DATA before OPEN_OK: the coordinator tells the customer
    // "connected" only once this arrives.
    if (!send_frame(OP_OPEN_OK, sid)) { close_stream(sid); return; }

    std::string buf(HEADER + CHUNK, '\0');
    buf[0] = (char)OP_DATA;
    write_be32(&buf[1], sid);
    while (session_alive_) {
        int n = recv(r.sock, &buf[HEADER], CHUNK, 0);
        if (n <= 0) break;  // EOF, reset, or 120 s idle (SO_RCVTIMEO)
        bytes_down_ += (uint64_t)n;
        if (!ws_.send_binary(buf.data(), HEADER + (size_t)n)) break;
    }
    close_stream(sid);
    send_frame(OP_CLOSE, sid);
}

void Tunnel::close_stream(uint32_t sid) {
    std::lock_guard<std::mutex> lock(streams_mu_);
    auto it = streams_.find(sid);
    if (it == streams_.end()) return;
    closesocket(it->second);  // unblocks the worker's recv, which then exits
    streams_.erase(it);
}

void Tunnel::close_all_streams() {
    std::lock_guard<std::mutex> lock(streams_mu_);
    for (auto& kv : streams_) closesocket(kv.second);
    streams_.clear();
}

void Tunnel::wait_workers(int timeout_ms) {
    std::unique_lock<std::mutex> lock(cv_mu_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return workers_.load() == 0; });
}

bool Tunnel::send_frame(uint8_t op, uint32_t sid, const char* payload, size_t len) {
    std::string frame(HEADER + len, '\0');
    frame[0] = (char)op;
    write_be32(&frame[1], sid);
    if (len) memcpy(&frame[HEADER], payload, len);
    return ws_.send_binary(frame.data(), frame.size());
}

// -- keep-alive + stats -------------------------------------------------------------
void Tunnel::keepalive_loop() {
    using clock = std::chrono::steady_clock;
    auto last_ping = clock::now(), last_stats = clock::now();
    while (session_alive_ && running_) {
        {
            std::unique_lock<std::mutex> lock(cv_mu_);
            cv_.wait_for(lock, std::chrono::seconds(1),
                         [&] { return !session_alive_.load() || !running_.load(); });
        }
        if (!session_alive_ || !running_) break;
        auto now = clock::now();
        if (now - last_ping >= std::chrono::seconds(heartbeat_s_)) {
            // WinHTTP already answers the coordinator's WebSocket pings; this
            // application-level frame additionally refreshes "last seen" there.
            ws_.send_text("{\"op\":\"ping\"}");
            last_ping = now;
        }
        if (now - last_stats >= std::chrono::seconds(stats_s_)) {
            size_t open;
            { std::lock_guard<std::mutex> lock(streams_mu_); open = streams_.size(); }
            json::Writer stats;
            stats.str("op", "stats").num("up", (long long)bytes_up_.load())
                 .num("down", (long long)bytes_down_.load()).num("streams", (long long)open);
            ws_.send_text(stats.done());  // diagnostics only; payouts are measured server-side
            log_summary();
            last_stats = now;
        }
    }
}

void Tunnel::log_summary() {
    size_t open;
    { std::lock_guard<std::mutex> lock(streams_mu_); open = streams_.size(); }
    LOGI(tag_.c_str(), "relayed %s this session (%llu connections, %zu open)",
         util::human_bytes(bytes_up_ + bytes_down_).c_str(), (unsigned long long)served_.load(), open);
}
