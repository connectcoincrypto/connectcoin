// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_TLS_LIFECYCLE_H
#define CONNECTCOIN_WALLET_P2C_TLS_LIFECYCLE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace wallet {

/** Gate only probe phases that access process state. Blocking DNS runs with a
 * copied resolver and owns no phase. Production keeps one coordinator for the
 * process lifetime; tests instantiate their own without stopping other tests.
 */
class P2CRsaProbeLifecycle
{
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_stopping{false};
    size_t m_active{0};

public:
    class Phase
    {
        friend class P2CRsaProbeLifecycle;
        P2CRsaProbeLifecycle* m_lifecycle;
        explicit Phase(P2CRsaProbeLifecycle* lifecycle) : m_lifecycle{lifecycle} {}

    public:
        Phase(const Phase&) = delete;
        Phase& operator=(const Phase&) = delete;
        Phase(Phase&& other) noexcept : m_lifecycle{std::exchange(other.m_lifecycle, nullptr)} {}
        ~Phase();
        explicit operator bool() const { return m_lifecycle != nullptr; }
    };

    [[nodiscard]] Phase TryEnter();
    bool IsStopping() const { return m_stopping.load(); }
    /** Permanently reject new phases and wait for existing phases to finish. */
    void Stop();
};

/** Test entry point with an independent shutdown lifecycle. */
bool ProbeP2CRsaForTest(const std::string& domain, uint32_t roots_version,
                       const std::function<bool()>& cancelled,
                       std::chrono::steady_clock::time_point deadline,
                       P2CRsaProbeLifecycle& lifecycle);
} // namespace wallet

#endif // CONNECTCOIN_WALLET_P2C_TLS_LIFECYCLE_H
