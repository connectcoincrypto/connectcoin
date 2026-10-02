// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_ENDPOINT_PRIORITY_H
#define CONNECTCOIN_WALLET_P2C_ENDPOINT_PRIORITY_H

#include <netaddress.h>
#include <wallet/p2c_domain_stats.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace wallet {

/** Wallet-local IP selection within one domain's active DNS set. Not consensus.
 * The exact signature mask has its own validated-proof EMA and smooth weighted
 * round-robin credits. This only chooses an address: it adds no start-rate or
 * concurrency limit, cooldown, timeout, or domain eligibility policy.
 * The caller serializes Refresh, Select, and observations of selected statistics.
 */
class P2CEndpointPriority {
public:
    static constexpr size_t MAX_ENDPOINTS{32};
    static constexpr double EXPLORATION{0.01};

    struct Selection {
        CService endpoint;
        std::shared_ptr<P2CDomainStats> statistics;
    };

private:
    static constexpr size_t MASK_COUNT{PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL};
    struct MaskState {
        std::shared_ptr<P2CDomainStats> statistics;
        double credit{0};
    };
    struct EndpointState {
        CService endpoint;
        std::array<MaskState, MASK_COUNT> masks;
    };
    std::vector<EndpointState> m_endpoints;
    std::array<size_t, MASK_COUNT> m_next{};

public:
    /** Retain the same statistics objects and credits for unchanged IPs, even
     * when a validation finishes after refresh. Removed addresses are forgotten;
     * an outstanding Selection keeps only its old statistics alive and cannot
     * reinsert them if that IP/domain is later added again.
     */
    void Refresh(const std::vector<CService>& endpoints)
    {
        std::vector<EndpointState> next;
        next.reserve(std::min(endpoints.size(), MAX_ENDPOINTS));
        for (const auto& endpoint : endpoints) {
            if (std::any_of(next.begin(), next.end(), [&](const auto& item) { return item.endpoint == endpoint; })) continue;
            const auto previous{std::find_if(m_endpoints.begin(), m_endpoints.end(),
                                           [&](const auto& item) { return item.endpoint == endpoint; })};
            next.push_back(previous == m_endpoints.end() ? EndpointState{endpoint, {}} : *previous);
            if (next.size() == MAX_ENDPOINTS) break;
        }
        m_endpoints = std::move(next);
    }

    bool Empty() const { return m_endpoints.empty(); }
    size_t Size() const { return m_endpoints.size(); }

    std::optional<Selection> Select(uint8_t signature_algorithms_mask)
    {
        if (Empty() || signature_algorithms_mask == 0 || signature_algorithms_mask > MASK_COUNT) return std::nullopt;
        const size_t mask{static_cast<size_t>(signature_algorithms_mask - 1)};
        const size_t count{m_endpoints.size()};
        std::array<double, MAX_ENDPOINTS> rates{};
        double maximum{0};
        for (size_t index = 0; index < count; ++index) {
            auto& state{m_endpoints[index].masks[mask]};
            if (!state.statistics) state.statistics = std::make_shared<P2CDomainStats>();
            rates[index] = state.statistics->ConnectionRate();
            maximum = std::max(maximum, rates[index]);
        }
        // Dividing by the maximum before summing avoids overflow even if every
        // measured rate is near double's largest representable finite value.
        double total{0};
        for (size_t index = 0; index < count; ++index) {
            rates[index] /= maximum;
            total += rates[index];
        }
        for (size_t index = 0; index < count; ++index) {
            m_endpoints[index].masks[mask].credit +=
                EXPLORATION / count + (1.0 - EXPLORATION) * rates[index] / total;
        }
        const size_t cursor{m_next[mask] % count};
        size_t selected{cursor};
        for (size_t offset = 1; offset < count; ++offset) {
            const size_t candidate{(cursor + offset) % count};
            if (m_endpoints[candidate].masks[mask].credit > m_endpoints[selected].masks[mask].credit) selected = candidate;
        }
        auto& state{m_endpoints[selected].masks[mask]};
        state.credit -= 1.0;
        m_next[mask] = (selected + 1) % count;
        return Selection{m_endpoints[selected].endpoint, state.statistics};
    }
};

} // namespace wallet

#endif // CONNECTCOIN_WALLET_P2C_ENDPOINT_PRIORITY_H
