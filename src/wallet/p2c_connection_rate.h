// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_CONNECTION_RATE_H
#define CONNECTCOIN_WALLET_P2C_CONNECTION_RATE_H

#include <algorithm>
#include <cassert>
#include <chrono>
#include <optional>

namespace wallet {

/** Aggregate wallet-local start-rate pacing; the caller serializes all access.
 * Advance the shared deadline for every selected connection, then retain at
 * most one second of accumulated lateness. True idle time banks no credit;
 * timer delays and occupied workers preserve the bounded catch-up cadence.
 * No spinning or OS timer changes are needed. Reconfiguration resets this
 * object after joining workers. This is not a separate rolling-window quota.
 */
class P2CConnectionRateLimiter {
public:
    using Clock = std::chrono::steady_clock;

private:
    std::optional<Clock::time_point> m_next;
    bool m_idle{true};

public:
    std::optional<Clock::time_point> BlockedUntil(Clock::time_point now) const
    {
        if (m_next && now < *m_next) return m_next;
        return std::nullopt;
    }

    /** No assignable connection work remains. Rebase only when work resumes,
     * preserving a still-future deadline and never banking the idle interval.
     * Rate waits and occupied connection/proposal capacity are not idle.
     */
    void MarkIdle() { m_idle = true; }

    /** Charge one selected connection, not DNS resolution or an idle scan.
     * Only positive rates call this; disabled/unlimited modes bypass pacing.
     */
    void RecordStart(int rate, Clock::time_point now)
    {
        assert(rate > 0);
        const auto interval{std::chrono::ceil<Clock::duration>(std::chrono::nanoseconds{(1'000'000'000LL + rate - 1) / rate})};
        if (!m_next) m_next = now;
        else if (m_idle) *m_next = std::max(*m_next, now);
        *m_next = std::max(*m_next + interval, now - std::chrono::seconds{1});
        m_idle = false;
    }
};

} // namespace wallet

#endif // CONNECTCOIN_WALLET_P2C_CONNECTION_RATE_H
