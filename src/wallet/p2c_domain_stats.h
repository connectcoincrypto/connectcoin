// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_DOMAIN_STATS_H
#define CONNECTCOIN_WALLET_P2C_DOMAIN_STATS_H

#include <wallet/p2c_claim.h>
#include <wallet/p2c_claim_priority.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wallet {
/** Wallet-local TCP/TLS observations, in completion order. Not consensus data.
 * Success means capture through CertificateVerify, not a winning work hash.
 * The caller serializes access and excludes locally cancelled attempts.
 */
class P2CDomainStats {
    double m_success_average{0.1};
    double m_seconds_average{0.02};

public:
    void Record(bool success, double seconds)
    {
        // Keep NaN/negative durations out of the ordered scheduler, including
        // if a future caller supplies observations from outside steady_clock.
        if (!std::isfinite(seconds) || seconds < 0) return;
        m_success_average = 0.999 * m_success_average + 0.001 * success;
        m_seconds_average = 0.999 * m_seconds_average + 0.001 * seconds;
    }

    double ConnectionRate() const
    {
        // The positive initial values decay with observations; they are not
        // added back on each refresh. Clamp only an unrepresentable quotient
        // to double's positive finite range, with no policy rate floor or cap.
        return std::clamp(m_success_average / m_seconds_average,
                          std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max());
    }
};

/** Expected net return per second of TCP/TLS effort. Economic numerators fit
 * in 320 bits. Saturate overflow from extreme measured rates to finite double.
 * This measured domain score is approximate; bounty ordering remains exact.
 * The scheduler uses the exact economic key to break rounded score ties.
 */
inline double GetP2CDomainPriority(const P2CClaimPriority& economic_priority, double connection_rate)
{
    double numerator{0};
    for (const auto word : economic_priority) numerator = std::ldexp(numerator, 32) + word;
    return std::min(std::ldexp(numerator, -256) * connection_rate, std::numeric_limits<double>::max());
}

/** The domain ranking uses the same stable local factor as bounty ordering.
 * Normalize only here; the exact integer ranking key retains all precision.
 */
inline double GetP2CDomainSelectionPriority(const P2CClaimSelectionPriority& selection_priority, double connection_rate)
{
    double numerator{0};
    for (const auto word : selection_priority) numerator = std::ldexp(numerator, 32) + word;
    const double expected_return{std::ldexp(numerator, -256)};
    const double score{expected_return * connection_rate};
    if (std::isfinite(score)) return score / P2C_CLAIM_FACTOR_SCALE;
    // Preserve ordinary rounding, but avoid an overflowing intermediate when
    // removing the integer factor's scale first makes the result representable.
    return std::min((expected_return / P2C_CLAIM_FACTOR_SCALE) * connection_rate,
                    std::numeric_limits<double>::max());
}

/** Local automatic-search policy, in connects per second of TCP/TLS effort.
 * Equal to the floor remains eligible. This does not restrict manual claims,
 * change consensus or discard proofs whose work has already been completed.
 */
inline constexpr double MIN_P2C_EXPECTED_RETURN{1000};

inline bool IsP2CClaimWorthAttempting(const P2CClaimPriority& economic_priority, double connection_rate)
{
    if (!std::isfinite(connection_rate) || connection_rate <= 0) return false;
    const double expected_return{GetP2CDomainPriority(economic_priority, connection_rate)};
    return std::isfinite(expected_return) && expected_return >= MIN_P2C_EXPECTED_RETURN;
}
} // namespace wallet

#endif // CONNECTCOIN_WALLET_P2C_DOMAIN_STATS_H
