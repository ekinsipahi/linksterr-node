#include "miner.h"

#include <chrono>
#include <mutex>

namespace {
std::mutex g_proc_mu;
}

Miner::~Miner() { stop(); }

void Miner::start() {
    if (!cfg_.enabled) return;
    if (cfg_.wallet.empty()) {
        LOGW("miner", "mining is on but no payout wallet is configured yet — not starting");
        return;
    }
    if (!util::file_exists(cfg_.miner_path)) {
        LOGW("miner", "mining is on but the miner program is not installed at %s — "
             "place the miner there (see the dashboard); not starting",
             util::narrow(cfg_.miner_path).c_str());
        return;
    }
    running_ = true;
    thread_ = std::thread(&Miner::supervise, this);
    LOGI("miner", "optional Monero mining ON (cpu %d%%, worker %s) — you earn your share; "
         "stop it by closing the program", cfg_.cpu_percent, cfg_.worker.c_str());
}

void Miner::supervise() {
    // Build the command line for the external RandomX miner (xmrig-compatible
    // flags). We only pass where to connect and how hard to work; the miner is
    // the owner's signed binary.
    int threads_pct = cfg_.cpu_percent < 1 ? 1 : (cfg_.cpu_percent > 100 ? 100 : cfg_.cpu_percent);
    std::string args = "\"" + util::narrow(cfg_.miner_path) + "\""
        " -o " + cfg_.pool +
        " -u " + cfg_.wallet +
        " -p " + (cfg_.worker.empty() ? std::string("linksterr-node") : cfg_.worker) +
        " -a rx/0 --max-cpu-usage=" + std::to_string(threads_pct) +
        " --donate-level=1 --no-color";
    std::wstring wargs = util::widen(args);

    while (running_) {
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        std::wstring mutable_args = wargs;  // CreateProcessW may modify the buffer
        BOOL ok = CreateProcessW(cfg_.miner_path.c_str(), mutable_args.data(), nullptr, nullptr,
                                 FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                 nullptr, nullptr, &si, &pi);
        if (!ok) {
            LOGW("miner", "could not start the miner: %s", util::win_error(GetLastError()).c_str());
            break;
        }
        {
            std::lock_guard<std::mutex> lock(g_proc_mu);
            process_ = pi.hProcess;
        }
        CloseHandle(pi.hThread);
        // Wait for it to exit (or for stop() to terminate it).
        WaitForSingleObject(pi.hProcess, INFINITE);
        {
            std::lock_guard<std::mutex> lock(g_proc_mu);
            if (process_) { CloseHandle(static_cast<HANDLE>(process_)); process_ = nullptr; }
        }
        if (!running_) break;
        LOGW("miner", "miner exited; restarting in 10s");
        for (int i = 0; i < 100 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Miner::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(g_proc_mu);
        if (process_) { TerminateProcess(static_cast<HANDLE>(process_), 0); }
    }
    if (thread_.joinable()) thread_.join();
}
