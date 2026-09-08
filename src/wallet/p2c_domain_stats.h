// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_DOMAIN_STATS_H
#define CONNECTCOIN_WALLET_P2C_DOMAIN_STATS_H

#include <wallet/p2c_claim.h>

#include <cmath>
#include <cstddef>
#include <deque>
#include <utility>

namespace wallet {
/** Wallet-local TCP/TLS observations, in completion order. Not consensus data.
 * Success means capture through CertificateVerify, not a winning work hash.
 * The caller serializes access and excludes locally cancelled attempts.
 */
class P2CDomainStats {
    std::deque<std::pair<bool, double>> m_attempts;

public:
    static constexpr size_t WINDOW{100};

    void Record(bool success, double seconds)
    {
        // Keep NaN/negative durations out of the ordered scheduler, including
        // if a future caller supplies observations from outside steady_clock.
        if (!std::isfinite(seconds) || seconds < 0) return;
        m_attempts.emplace_back(success, seconds);
        if (m_attempts.size() > WINDOW) m_attempts.pop_front();
    }

    size_t Attempts() const { return m_attempts.size(); }

    std::pair<size_t, double> Totals() const
    {
        size_t successes{0};
        double seconds{0};
        // Sum only at priority refresh, not on the connection hot path. A
        // bounded recomputation avoids accumulated add/subtract roundoff when
        // old slow attempts leave a window of much faster new attempts.
        for (const auto& [success, elapsed] : m_attempts) {
            successes += success;
            seconds += elapsed;
        }
        return {successes, seconds};
    }

    double ConnectionRate() const
    {
        const auto [successes, seconds]{Totals()};
        return (0.1 + successes) / (0.02 + seconds);
    }
};

/** Expected net return per second of TCP/TLS effort. Economic numerators fit
 * in 320 bits and the smoothed rate is at most 5005, comfortably within double.
 * This measured domain score is approximate; bounty ordering remains exact.
 * The scheduler uses the exact economic key to break rounded score ties.
 */
inline double GetP2CDomainPriority(const P2CClaimPriority& economic_priority, double connection_rate)
{
    double numerator{0};
    for (const auto word : economic_priority) numerator = std::ldexp(numerator, 32) + word;
    return std::ldexp(numerator, -256) * connection_rate;
}
} // namespace wallet

#endif // CONNECTCOIN_WALLET_P2C_DOMAIN_STATS_H
