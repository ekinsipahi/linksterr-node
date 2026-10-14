// linksterr-node — share your PC's idle internet connection and earn on Linksterr.
//
//   linksterr-node.exe --key <your node key> [--label you@example.com]
//   linksterr-node.exe --config node.ini
//
// One process can carry several accounts (repeat --key, or several [account]
// sections in the config file). Everything is a command-line switch, so a batch
// file or a scheduled task runs it unattended once the terms were accepted.
#include "net.h"
#include "tunnel.h"
#include "util.h"

#include <io.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr const char* VERSION = "1.1.0";
constexpr int TERMS_VERSION = 1;
constexpr const char* TERMS_URL = "https://linksterr.com/node-terms/";

std::mutex g_tunnels_mu;
std::vector<Tunnel*> g_tunnels;
std::atomic<bool> g_stop{false};

BOOL WINAPI on_console_event(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        if (!g_stop.exchange(true)) {
            fprintf(stderr, "\nstopping...\n");
            std::lock_guard<std::mutex> lock(g_tunnels_mu);
            for (Tunnel* t : g_tunnels) t->stop();
        }
        return TRUE;
    }
    return FALSE;
}

void usage() {
    printf(
        "linksterr-node %s — share your PC's idle connection, earn on Linksterr\n\n"
        "usage: linksterr-node.exe --key KEY [--label NAME] [options]\n"
        "       linksterr-node.exe --config node.ini [options]\n\n"
        "  --key KEY          node key from your Linksterr dashboard (repeat for more accounts)\n"
        "  --label NAME       a name for the last --key, shown in the log only\n"
        "  --config FILE      read settings and [account] sections from an INI file\n"
        "  --agree            record that you accept the terms (needed once; unattended use)\n"
        "                     (--accept-terms is the same)\n"
        "  --withdraw         delete your acceptance and this PC's device identities, then exit\n"
        "  --max-streams N    concurrent connections to carry (1-64, default 16)\n"
        "  --server URL       coordinator (default wss://node.ipsterr.com)\n"
        "  --data-dir DIR     where consent and device identities are kept (default %%LOCALAPPDATA%%\\LinksterrNode)\n"
        "  --verbose          debug logging\n"
        "  --version, --help\n\n"
        "The LINKSTERR_NODE_KEY environment variable is used when no --key is given.\n"
        "Terms and privacy policy: %s\n", VERSION, TERMS_URL);
}

struct Options {
    NodeConfig cfg;
    std::vector<Account> accounts;
    bool accept_terms = false;
    bool withdraw = false;
};

bool apply_setting(Options& o, const std::string& key, const std::string& value, std::string& err) {
    if (key == "server") o.cfg.server = value;
    else if (key == "max_streams") o.cfg.max_streams = atoi(value.c_str());
    else if (key == "allow_private") o.cfg.allow_private = (value == "1" || value == "true" || value == "yes");
    else if (key == "any_port") { if (value == "1" || value == "true" || value == "yes") { o.cfg.allowed_ports.clear(); o.cfg.follow_server_ports = false; } }
    else if (key == "pool") o.cfg.pool = value;
    else if (key == "api_key") o.cfg.api_key = value;
    else if (key == "log") util::g_level = value == "debug" ? util::Level::Debug : util::Level::Info;
    else { err = "unknown setting: " + key; return false; }
    return true;
}

bool load_config(Options& o, const std::string& path, std::string& err) {
    std::string text;
    if (!util::read_file(util::widen(path), text)) { err = "cannot read " + path; return false; }
    std::istringstream in(text);
    std::string line;
    bool in_account = false;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        line = util::trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line == "[account]") { o.accounts.push_back({}); in_account = true; continue; }
        if (line[0] == '[') { in_account = false; continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) { err = path + ":" + std::to_string(line_no) + ": expected key = value"; return false; }
        std::string key = util::trim(line.substr(0, eq)), value = util::trim(line.substr(eq + 1));
        if (in_account) {
            if (key == "key") o.accounts.back().key = value;
            else if (key == "label") o.accounts.back().label = value;
            else { err = path + ":" + std::to_string(line_no) + ": unknown account setting " + key; return false; }
        } else if (!apply_setting(o, key, value, err)) {
            err = path + ":" + std::to_string(line_no) + ": " + err;
            return false;
        }
    }
    return true;
}

bool parse_args(int argc, wchar_t** argv, Options& o, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        std::string a = util::narrow(argv[i]);
        auto next = [&](std::string& out) {
            if (i + 1 >= argc) { err = a + " needs a value"; return false; }
            out = util::narrow(argv[++i]);
            return true;
        };
        std::string v;
        if (a == "--help" || a == "-h" || a == "/?") { usage(); exit(0); }
        else if (a == "--version") { printf("linksterr-node %s\n", VERSION); exit(0); }
        else if (a == "--key") { if (!next(v)) return false; o.accounts.push_back({util::trim(v), ""}); }
        else if (a == "--label") {
            if (!next(v)) return false;
            if (o.accounts.empty()) { err = "--label must follow a --key"; return false; }
            o.accounts.back().label = v;
        }
        else if (a == "--config") { if (!next(v)) return false; if (!load_config(o, v, err)) return false; }
        else if (a == "--accept-terms" || a == "--agree") o.accept_terms = true;
        else if (a == "--withdraw") o.withdraw = true;
        else if (a == "--max-streams") { if (!next(v)) return false; o.cfg.max_streams = atoi(v.c_str()); }
        else if (a == "--server") { if (!next(v)) return false; o.cfg.server = v; }
        else if (a == "--verbose" || a == "-v") util::g_level = util::Level::Debug;
        else if (a == "--data-dir") { if (!next(v)) return false; o.cfg.data_dir = util::widen(v); CreateDirectoryW(o.cfg.data_dir.c_str(), nullptr); }
        // Testing against a local coordinator: LAN targets, any port, plain SDK pool.
        else if (a == "--allow-private") o.cfg.allow_private = true;
        else if (a == "--any-port") { o.cfg.allowed_ports.clear(); o.cfg.follow_server_ports = false; }
        else if (a == "--pool") { if (!next(v)) return false; o.cfg.pool = v == "sdk" ? "" : v; }
        else if (a == "--api-key") { if (!next(v)) return false; o.cfg.api_key = v; }
        else { err = "unknown option " + a + " (see --help)"; return false; }
    }
    return true;
}

std::wstring consent_path(const Options& o) { return o.cfg.data_dir + L"\\consent.txt"; }

bool consent_recorded(const Options& o) {
    std::string text;
    if (!util::read_file(consent_path(o), text)) return false;
    return text.find("terms_version=" + std::to_string(TERMS_VERSION)) != std::string::npos;
}

void record_consent(const Options& o) {
    util::write_file(consent_path(o), "accepted_at=" + util::now_iso8601() +
                                      "\nterms_version=" + std::to_string(TERMS_VERSION) +
                                      "\nterms_url=" + TERMS_URL + "\n");
}

void print_disclosure() {
    printf(
        "\nLinksterr Node shares your PC's idle internet connection.\n\n"
        "With your permission this PC will carry other people's web requests while it\n"
        "is idle, acting as an exit point of the Linksterr network (operated with\n"
        "Proxysterr). Websites reached that way see this connection's IP address.\n\n"
        "  - Only ordinary web traffic is relayed (ports 80, 443, 8080, 8443)\n"
        "  - Nothing on this PC is read, stored or sent; only the relayed byte count\n"
        "    and this connection's IP, country and network operator are recorded\n"
        "  - One internet connection earns once, however many PCs run this\n"
        "  - Stop at any time: Ctrl+C or close this window; --withdraw forgets this PC\n"
        "  - You earn for data actually relayed; see your Linksterr dashboard\n\n"
        "Full terms and privacy policy: %s\n\n", TERMS_URL);
}

bool obtain_consent(const Options& o) {
    if (consent_recorded(o)) return true;
    print_disclosure();
    if (o.accept_terms) {
        record_consent(o);
        printf("Terms accepted (recorded in %s).\n", util::narrow(consent_path(o)).c_str());
        return true;
    }
    if (!_isatty(_fileno(stdin))) {
        fprintf(stderr, "No terms acceptance on record. Run once interactively, or pass --agree "
                        "if you have read and accept the terms above.\n");
        return false;
    }
    printf("Type I AGREE to accept, or anything else to quit: ");
    fflush(stdout);
    char line[64] = {0};
    if (!fgets(line, sizeof line, stdin)) return false;
    if (util::trim(line) != "I AGREE") { printf("Not accepted. Nothing was started.\n"); return false; }
    record_consent(o);
    printf("Thank you. You can withdraw at any time with --withdraw.\n\n");
    return true;
}

void withdraw(const Options& o) {
    util::remove_file(consent_path(o));
    WIN32_FIND_DATAW fd;
    std::wstring pattern = o.cfg.data_dir + L"\\token-*.txt";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    int removed = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do { if (util::remove_file(o.cfg.data_dir + L"\\" + fd.cFileName)) ++removed; } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    printf("Consent withdrawn; %d device identit%s forgotten. This PC will not contribute again "
           "until you accept the terms anew.\n", removed, removed == 1 ? "y" : "ies");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Options o;
    o.cfg.data_dir = util::data_dir();
    std::string err;
    if (!parse_args(argc, argv, o, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }
    if (o.withdraw) { withdraw(o); return 0; }

    if (o.accounts.empty()) {
        wchar_t buf[256];
        DWORD n = GetEnvironmentVariableW(L"LINKSTERR_NODE_KEY", buf, 256);
        if (n > 0 && n < 256) o.accounts.push_back({util::trim(util::narrow(buf)), ""});
    }
    for (auto& a : o.accounts) {
        if (a.key.empty()) { fprintf(stderr, "error: an account has no key\n"); return 2; }
    }
    if (o.accounts.empty()) {
        fprintf(stderr, "error: no node key given. Get one from your Linksterr dashboard, then run\n"
                        "  linksterr-node.exe --key <KEY>\n");
        return 2;
    }
    if (o.cfg.pool.empty() && o.cfg.api_key.empty()) {
        fprintf(stderr, "error: SDK mode needs --api-key\n");
        return 2;
    }
    if (!obtain_consent(o)) return 4;
    if (!net::startup()) { fprintf(stderr, "error: Winsock failed to start\n"); return 1; }

    printf("linksterr-node %s\n  server    %s\n  accounts  %zu\n  streams   up to %d per account\n"
           "  data      %s\n  Press Ctrl+C to stop.\n\n",
           VERSION, o.cfg.server.c_str(), o.accounts.size(), std::clamp(o.cfg.max_streams, 1, 64),
           util::narrow(o.cfg.data_dir).c_str());
    fflush(stdout);

    std::vector<std::unique_ptr<Tunnel>> tunnels;
    int index = 0;
    for (const Account& a : o.accounts) tunnels.push_back(std::make_unique<Tunnel>(o.cfg, a, ++index));
    {
        std::lock_guard<std::mutex> lock(g_tunnels_mu);
        for (auto& t : tunnels) g_tunnels.push_back(t.get());
    }
    SetConsoleCtrlHandler(on_console_event, TRUE);

    std::vector<std::thread> threads;
    std::vector<Tunnel::Outcome> outcomes(tunnels.size(), Tunnel::Outcome::Stopped);
    for (size_t i = 0; i < tunnels.size(); ++i)
        threads.emplace_back([&, i] { outcomes[i] = tunnels[i]->run(); });
    for (auto& t : threads) t.join();
    {
        std::lock_guard<std::mutex> lock(g_tunnels_mu);
        g_tunnels.clear();
    }
    bool any_ok = false;
    for (auto outcome : outcomes) if (outcome != Tunnel::Outcome::Refused) any_ok = true;
    WSACleanup();
    if (!any_ok) {
        fprintf(stderr, "every account was refused — check the key(s) in your Linksterr dashboard\n");
        return 3;
    }
    return 0;
}
