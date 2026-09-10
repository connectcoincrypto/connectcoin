// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_WORKER_THREADS_H
#define CONNECTCOIN_WALLET_P2C_WORKER_THREADS_H

#include <cstddef>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace wallet {

/** Grow an externally owned/joined connection pool. Resource exhaustion keeps
 * existing workers and prevents repeated creation attempts in this session.
 * A new coordinator/reconfiguration creates a fresh growth controller.
 */
class P2CThreadPoolGrowth
{
    bool m_limited{false};

public:
    template <typename CanStart, typename CreateThread>
    std::optional<std::string> Grow(std::vector<std::thread>& connections, size_t requested,
                                    const CanStart& can_start, const CreateThread& create_thread)
    {
        if (m_limited) return std::nullopt;
        while (connections.size() < requested && can_start()) {
            // Allocate the vector slot before starting a thread: a failed
            // allocation must never destroy an unowned joinable temporary.
            connections.emplace_back();
            try {
                connections.back() = create_thread();
            } catch (const std::system_error& error) {
                connections.pop_back();
                if (connections.empty()) throw;
                m_limited = true;
                return std::string{error.what()};
            } catch (...) {
                connections.pop_back();
                throw;
            }
        }
        return std::nullopt;
    }
};

} // namespace wallet

#endif // CONNECTCOIN_WALLET_P2C_WORKER_THREADS_H
