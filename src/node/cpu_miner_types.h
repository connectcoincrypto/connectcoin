// Copyright (c) 2026 The ConnectCoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_NODE_CPU_MINER_TYPES_H
#define CONNECTCOIN_NODE_CPU_MINER_TYPES_H

#include <cstdint>
#include <string>

namespace node {
inline constexpr int MAX_CPU_MINING_THREADS{1024};

struct CpuMiningStatus {
    bool running{false};
    bool stopping{false};
    int threads{1};
    int max_threads{MAX_CPU_MINING_THREADS};
    int logical_cpus{1};
    uint64_t hashes{0};
    uint64_t blocks{0};
    double hashes_per_second{0};
    std::string address;
    std::string state{"stopped"};
    std::string error;
};
} // namespace node

#endif // CONNECTCOIN_NODE_CPU_MINER_TYPES_H
