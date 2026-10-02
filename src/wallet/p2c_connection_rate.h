// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_CONNECTION_RATE_H
#define CONNECTCOIN_WALLET_P2C_CONNECTION_RATE_H

#include <cassert>
#include <chrono>
#include <optional>

namespace wallet {

/** Aggregate wallet-local start-rate pacing; the caller serializes all access.
 * Carry short timer delays forward instead of resetting the schedule on every
 * wakeup. Discard missed slots after a stall longer than 100 ms: idle time must
 * not accumulate an unbounded burst of connections. No spinning or OS timer
 * changes are needed. Reconfiguration resets this object after joining workers.
 */
class P2CConnectionRateLimiter {
public:
    using Clock = std::chrono::steady_clock;

private:
    std::optional<Clock::time_point> m_next;

public:
    std::optional<Clock::time_point> BlockedUntil(Clock::time_point now) const
    {
        if (m_next && now < *m_next) return m_next;
        return std::nullopt;
    }

    /** Charge one selected connection, not DNS resolution or an idle scan.
     * Only positive rates call this; disabled/unlimited modes bypass pacing.
     */
    void RecordStart(int rate, Clock::time_point now)
    {
        assert(rate > 0);
        const auto interval{std::chrono::ceil<Clock::duration>(std::chrono::nanoseconds{(1'000'000'000LL + rate - 1) / rate})};
        if (!m_next || now - *m_next > std::chrono::milliseconds{100}) {
            m_next = now + interval;
        } else {
            *m_next += interval;
        }
    }
};

} // namespace wallet

#endif // CONNECTCOIN_WALLET_P2C_CONNECTION_RATE_H
