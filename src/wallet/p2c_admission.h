// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef CONNECTCOIN_WALLET_P2C_ADMISSION_H
#define CONNECTCOIN_WALLET_P2C_ADMISSION_H

#include <netaddress.h>
#include <wallet/p2c_claim.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>

namespace wallet {

/** Exact precomputed threshold for the existing successful-capture policy.
 * Saturated counters still stop regardless of target, as before. */
class P2CConnectionBudget {
    uint64_t m_first_excess{std::numeric_limits<uint64_t>::max()};
public:
    explicit P2CConnectionBudget(const uint256& target)
    {
        if (!IsP2CClaimConnectionLimitExceeded(target, m_first_excess)) return;
        uint64_t low{0}, high{m_first_excess};
        while (low < high) {
            const uint64_t middle{low + (high - low) / 2};
            if (IsP2CClaimConnectionLimitExceeded(target, middle)) high = middle;
            else low = middle + 1;
        }
        m_first_excess = low;
    }
    bool Exceeded(uint64_t successes) const { return successes >= m_first_excess; }
};

/** One deadline-owning head; other workers sleep without timer polling.
 * The worker's work mutex serializes every operation and ticket lifetime. */
class P2CFifoAdmission {
public:
    struct Ticket { std::condition_variable wake; };
private:
    std::deque<Ticket*> m_waiters;
public:
    void Enqueue(Ticket& ticket) { m_waiters.push_back(&ticket); }
    bool IsHead(const Ticket& ticket) const { return !m_waiters.empty() && m_waiters.front() == &ticket; }
    void NotifyHead() { if (!m_waiters.empty()) m_waiters.front()->wake.notify_one(); }
    void NotifyAll() { for (auto* ticket : m_waiters) ticket->wake.notify_one(); }
    void Remove(Ticket& ticket)
    {
        const auto found{std::find(m_waiters.begin(), m_waiters.end(), &ticket)};
        if (found == m_waiters.end()) return;
        const bool head{found == m_waiters.begin()};
        m_waiters.erase(found);
        if (head) NotifyHead();
    }
    bool Empty() const { return m_waiters.empty(); }
};

/** Cache only definite local route/family failures, never TLS/server refusals.
 * Entries expire rather than excluding an address permanently after a route
 * or adapter changes. Caller serializes access using monotonic seconds. */
class P2CLocalEndpointPolicy {
    static constexpr size_t MAX_ENDPOINTS{4096};
    std::map<CService, double> m_endpoints;
    std::map<int, double> m_families;
public:
    bool Available(const CService& endpoint, double now) const
    {
        const auto family{m_families.find(endpoint.GetSAFamily())};
        if (family != m_families.end() && family->second > now) return false;
        const auto address{m_endpoints.find(endpoint)};
        return address == m_endpoints.end() || address->second <= now;
    }
    void Reject(const CService& endpoint, bool family, double now)
    {
        std::erase_if(m_endpoints, [&](const auto& entry) { return entry.second <= now; });
        std::erase_if(m_families, [&](const auto& entry) { return entry.second <= now; });
        if (family) {
            if (!m_families.contains(endpoint.GetSAFamily()) && m_families.size() >= 2)
                m_families.erase(std::min_element(m_families.begin(), m_families.end(), [](const auto& a, const auto& b) { return a.second < b.second; }));
            m_families[endpoint.GetSAFamily()] = now + 60;
        } else {
            if (!m_endpoints.contains(endpoint) && m_endpoints.size() >= MAX_ENDPOINTS)
                m_endpoints.erase(std::min_element(m_endpoints.begin(), m_endpoints.end(), [](const auto& a, const auto& b) { return a.second < b.second; }));
            m_endpoints[endpoint] = now + 60;
        }
    }
    size_t EndpointCount() const { return m_endpoints.size(); }
};
} // namespace wallet
#endif
