// Copyright (c) 2026 The ConnectCoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_NODE_CPU_MINER_H
#define CONNECTCOIN_NODE_CPU_MINER_H

#include <node/cpu_miner_types.h>
#include <script/script.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace node {
struct NodeContext;

/** Node-wide opt-in CPU miner. Never owns or accesses a wallet/private key.
 * Stop is asynchronous (the current hash/dataset initialization must finish).
 * Destruction joins all workers before dependencies may be destroyed. */
class CpuMiner
{
public:
    explicit CpuMiner(NodeContext& node);
    ~CpuMiner();
    void Start(const std::string& address, int threads);
    void Stop();
    CpuMiningStatus GetStatus() const;

private:
    void Run(CScript payout, int threads);
    NodeContext& m_node;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    CpuMiningStatus m_status;
    std::atomic<bool> m_stop{true};
    std::atomic<uint64_t> m_hashes{0};
    std::thread m_thread;
};
} // namespace node

#endif // CONNECTCOIN_NODE_CPU_MINER_H
