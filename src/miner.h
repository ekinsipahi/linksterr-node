// miner.h — optional, opt-in Monero (XMR) mining.
//
// This file does NOT contain a miner. It is a small supervisor that launches an
// EXTERNAL, owner-supplied mining program (the standard open-source RandomX
// miner, placed next to this exe as the configured binary) and keeps it running
// while the node runs. Mining is:
//   * off unless the user turns it on (--mining), so genuinely opt-in;
//   * disclosed in the consent screen and /node-terms/ in plain words ("your PC
//     mines Monero, you earn your share, turn it off any time");
//   * CPU-capped (--mining-cpu), and stopped when the node stops.
// Nothing is hidden and nothing runs unless the user asked for it.
#pragma once
#include "util.h"

#include <atomic>
#include <string>
#include <thread>

struct MiningConfig {
    bool enabled = false;                 // opt-in: the user passed --mining
    std::string pool = "pool.supportxmr.com:3333";
    std::string wallet;                   // our XMR payout address (from config/welcome)
    std::string worker;                   // = this node's account id, so earnings map back
    int cpu_percent = 50;                 // share of CPU the miner may use
    // The external miner executable. Default: xmrig.exe next to this program.
    // The owner ships a verified/signed build there; we never bundle or fetch it.
    std::wstring miner_path;
};

// Supervises the external miner process: starts it with the configured pool/
// wallet/worker/cpu, restarts it if it exits, and terminates it on stop().
class Miner {
public:
    explicit Miner(MiningConfig cfg) : cfg_(std::move(cfg)) {}
    ~Miner();
    void start();   // no-op unless cfg_.enabled and a miner binary is present
    void stop();

private:
    void supervise();
    MiningConfig cfg_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    void* process_ = nullptr;   // HANDLE of the child, under proc_mu_
};
