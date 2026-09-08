// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_NODE_P2C_BOUNTY_CATALOG_H
#define CONNECTCOIN_NODE_P2C_BOUNTY_CATALOG_H

#include <coins.h>
#include <consensus/p2c.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <undo.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace node {
/** Confirmed discovery metadata, indexed by both outpoint and creation height.
 * Only the caller's catalog mutex may guard this class. Never consult it as
 * an alternative to current UTXO/consensus validation of a claim.
 */
class P2CBountyCatalog {
    std::map<COutPoint, Coin> m_coins;
    std::map<uint32_t, std::set<COutPoint>> m_by_height;

public:
    void Clear()
    {
        m_coins.clear();
        m_by_height.clear();
    }

    void Erase(const COutPoint& outpoint)
    {
        const auto coin{m_coins.find(outpoint)};
        if (coin == m_coins.end()) return;
        const auto group{m_by_height.find(coin->second.nHeight)};
        if (group != m_by_height.end()) {
            group->second.erase(outpoint);
            if (group->second.empty()) m_by_height.erase(group);
        }
        m_coins.erase(coin);
    }

    void Insert(const COutPoint& outpoint, Coin coin)
    {
        Erase(outpoint);
        const uint32_t height{coin.nHeight};
        m_coins.emplace(outpoint, std::move(coin));
        try {
            m_by_height[height].insert(outpoint);
        } catch (...) {
            Erase(outpoint); // Do not leave the two indexes inconsistent.
            throw;
        }
    }

    std::optional<std::map<COutPoint, Coin>> Snapshot(uint32_t min_height, const std::function<bool()>& cancelled) const
    {
        if (cancelled()) return std::nullopt;
        std::map<COutPoint, Coin> snapshot;
        // O(log(heights) + recent entries * log(all entries)), not a walk
        // over every ancient unclaimed bounty on each wallet refresh.
        for (auto group{m_by_height.lower_bound(min_height)}; group != m_by_height.end(); ++group) {
            for (const auto& outpoint : group->second) {
                if (cancelled()) return std::nullopt;
                snapshot.emplace(outpoint, m_coins.at(outpoint));
            }
        }
        return snapshot;
    }
};

/** Apply an already validated block, or disconnect it using its undo data.
 * The caller must discard a partially updated catalog on failure/cancellation.
 * This is discovery metadata only, never an alternative to claim validation.
 */
inline bool ApplyP2CBountyBlock(P2CBountyCatalog& catalog, const CBlock& block, int height,
                                const CBlockUndo* undo, const std::function<bool()>& cancelled)
{
    if (block.vtx.empty() || (undo && undo->vtxundo.size() + 1 != block.vtx.size())) return false;
    for (size_t step = 0; step < block.vtx.size(); ++step) {
        if (cancelled()) return false;
        const size_t i{undo ? block.vtx.size() - 1 - step : step};
        const auto& tx{*block.vtx[i]};
        if (!undo) {
            for (const auto& input : tx.vin) catalog.Erase(input.prevout);
        }
        for (uint32_t n = 0; n < tx.vout.size(); ++n) {
            const COutPoint outpoint{tx.GetHash(), n};
            if (undo) catalog.Erase(outpoint);
            else if (IsCanonicalP2COutput(tx.vout[n])) catalog.Insert(outpoint, Coin{tx.vout[n], height, tx.IsCoinBase()});
        }
        if (undo && i > 0) {
            const auto& previous{undo->vtxundo[i - 1].vprevout};
            if (previous.size() != tx.vin.size()) return false;
            for (size_t n = 0; n < previous.size(); ++n) {
                if (IsCanonicalP2COutput(previous[n].out)) catalog.Insert(tx.vin[n].prevout, previous[n]);
            }
        }
    }
    return true;
}
} // namespace node
#endif // CONNECTCOIN_NODE_P2C_BOUNTY_CATALOG_H
