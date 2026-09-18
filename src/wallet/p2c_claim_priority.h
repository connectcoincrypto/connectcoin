// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_CLAIM_PRIORITY_H
#define CONNECTCOIN_WALLET_P2C_CLAIM_PRIORITY_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <utility>

namespace wallet {
/** Exact expected-return numerator (target + 1) * net_reward, most-significant
 * word first. All claims share the denominator 2^256, so array ordering gives
 * their expected-value ordering without division or floating-point rounding.
 * Non-positive/out-of-range payouts have zero priority.
 */
using P2CClaimPriority = std::array<uint32_t, 10>;
P2CClaimPriority GetP2CClaimPriority(const uint256& target, CAmount net_reward);

inline constexpr uint32_t P2C_CLAIM_FACTOR_SCALE{1'000'000};
inline constexpr uint32_t P2C_CLAIM_FACTOR_MAX{1'100'000};

/** Local ranking only: an inclusive, uniformly sampled multiplier of 1..1.1.
 * This must never change fees, profitability eligibility or connection budgets.
 */
uint32_t RandomP2CClaimFactor();

/** Exact (target + 1) * net_reward * factor, most-significant word first.
 * Eleven words retain the extra factor precision without overflow/rounding.
 */
using P2CClaimSelectionPriority = std::array<uint32_t, 11>;
inline P2CClaimSelectionPriority GetP2CClaimSelectionPriority(const P2CClaimPriority& priority, uint32_t factor)
{
    if (factor < P2C_CLAIM_FACTOR_SCALE || factor > P2C_CLAIM_FACTOR_MAX) {
        throw std::invalid_argument("Invalid P2C claim ranking factor");
    }
    P2CClaimSelectionPriority result{};
    uint64_t carry{0};
    for (size_t i = priority.size(); i > 0; --i) {
        const uint64_t product{uint64_t{priority[i - 1]} * factor + carry};
        result[i] = static_cast<uint32_t>(product);
        carry = product >> 32;
    }
    result[0] = static_cast<uint32_t>(carry);
    return result;
}

/** A factor survives schedule refreshes, retries and stop/start while tracked.
 * The owner retains catalog entries and any still-live/completed claim work.
 */
class P2CClaimFactorCache {
    std::map<COutPoint, uint32_t> m_factors;
    std::function<uint32_t()> m_generate;

public:
    explicit P2CClaimFactorCache(std::function<uint32_t()> generate = RandomP2CClaimFactor)
        : m_generate{std::move(generate)} {}

    uint32_t Get(const COutPoint& outpoint)
    {
        if (const auto found{m_factors.find(outpoint)}; found != m_factors.end()) return found->second;
        const auto factor{m_generate()};
        if (factor < P2C_CLAIM_FACTOR_SCALE || factor > P2C_CLAIM_FACTOR_MAX) {
            throw std::invalid_argument("Invalid P2C claim ranking factor");
        }
        m_factors.emplace(outpoint, factor);
        return factor;
    }

    template <typename Predicate> void Retain(Predicate keep)
    {
        std::erase_if(m_factors, [&](const auto& item) { return !keep(item.first); });
    }
    size_t Size() const { return m_factors.size(); }
};
} // namespace wallet

#endif // CONNECTCOIN_WALLET_P2C_CLAIM_PRIORITY_H
