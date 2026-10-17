// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_RECOVERY_POLICY_H
#define CONNECTCOIN_WALLET_P2C_RECOVERY_POLICY_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace wallet {

/** Bounded sampling for positive-return policies excluded by the measured
 * economic floor. Only below-floor representatives use this gate: it adds no
 * delay to ordinary profitable work and changes no observed counters/ratings.
 * The caller selects the best bounty per exact (domain, mask), holds its worker
 * mutex, and still enforces the ordinary rate, concurrency and bounty budgets.
 */
class P2CRecoveryPolicy {
public:
    using Key = std::pair<std::string, uint8_t>;
    static constexpr size_t MAX_POLICIES{256};
    static constexpr uint32_t BOOTSTRAP_STARTS{32};
    static constexpr double BOOTSTRAP_INTERVAL{0.25};
    static constexpr double RECOVERY_INTERVAL{60};

private:
    struct State {
        uint32_t started{0};
        double due{0};
        std::list<Key>::iterator position;
    };
    struct Reservation {
        Key key;
        uint64_t token;
    };
    std::map<Key, State> m_probes;
    std::list<Key> m_recent;
    double m_next_global_start{0};
    double m_now{0};
    std::optional<Reservation> m_reservation;

    double Advance(double now)
    {
        if (!std::isfinite(now) || now < 0) throw std::invalid_argument("Invalid monotonic P2C recovery timestamp");
        m_now = std::max(m_now, now);
        return m_now;
    }

    State& Entry(const Key& key, double now)
    {
        if (const auto found{m_probes.find(key)}; found != m_probes.end()) return found->second;
        if (m_probes.size() == MAX_POLICIES) {
            auto oldest{m_recent.begin()};
            if (m_reservation && *oldest == m_reservation->key) ++oldest;
            m_probes.erase(*oldest);
            m_recent.erase(oldest);
        }
        // An evicted policy may bootstrap again, but the global four-start/s
        // deadline survives eviction/retention/clear and prevents churn bursts.
        const auto inserted{m_probes.emplace(key, State{0, now, {}}).first};
        try {
            m_recent.push_back(key);
        } catch (...) {
            m_probes.erase(inserted);
            throw;
        }
        inserted->second.position = std::prev(m_recent.end());
        return inserted->second;
    }

public:
    P2CRecoveryPolicy() = default;
    P2CRecoveryPolicy(const P2CRecoveryPolicy&) = delete;
    P2CRecoveryPolicy& operator=(const P2CRecoveryPolicy&) = delete;

    size_t Size() const { return m_probes.size(); }
    bool Reserved() const { return m_reservation.has_value(); }
    bool ReservedFor(uint64_t token) const { return m_reservation && m_reservation->token == token; }

    double Due(const Key& key, double now)
    {
        now = Advance(now);
        return std::max(Entry(key, now).due, m_next_global_start);
    }

    /** A single global pre-TCP reservation prevents delayed acknowledgements
     * or multiple capture threads from dispatching simultaneous recovery starts.
     */
    bool Reserve(const Key& key, uint64_t token, double now)
    {
        now = Advance(now);
        if (m_reservation || Due(key, now) > now) return false;
        m_reservation = Reservation{key, token};
        return true;
    }

    /** Pre-start failure/cancellation spends neither a sample nor a deadline. */
    bool Release(uint64_t token)
    {
        if (!ReservedFor(token)) return false;
        m_reservation.reset();
        return true;
    }

    /** Call only when the reserved operation actually reaches TCP admission,
     * not on assignment, DNS, preparation or an unsuccessful admission check.
     */
    double Started(const Key& key, uint64_t token, double now)
    {
        if (!ReservedFor(token) || m_reservation->key != key) throw std::invalid_argument("Invalid P2C recovery start reservation");
        now = Advance(now);
        auto& state{Entry(key, now)};
        state.started = std::min(BOOTSTRAP_STARTS, state.started + 1);
        state.due = now + (state.started < BOOTSTRAP_STARTS ? BOOTSTRAP_INTERVAL : RECOVERY_INTERVAL);
        m_next_global_start = std::max(m_next_global_start, now + BOOTSTRAP_INTERVAL);
        m_recent.splice(m_recent.end(), m_recent, state.position);
        Release(token);
        return std::max(state.due, m_next_global_start);
    }

    void Retain(const std::set<Key>& policies)
    {
        for (auto it = m_probes.begin(); it != m_probes.end();) {
            if (!policies.contains(it->first) && (!m_reservation || m_reservation->key != it->first)) {
                m_recent.erase(it->second.position);
                it = m_probes.erase(it);
            } else {
                ++it;
            }
        }
    }

    /** A context reset clears policy history, never an already promised global
     * deadline or a live admission. The owner releases/drains that separately.
     */
    void Clear()
    {
        m_probes.clear();
        m_recent.clear();
    }
};

} // namespace wallet

#endif // CONNECTCOIN_WALLET_P2C_RECOVERY_POLICY_H
