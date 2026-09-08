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

namespace node {
/** Apply an already validated block, or disconnect it using its undo data.
 * The caller must discard a partially updated catalog on failure/cancellation.
 * This is discovery metadata only, never an alternative to claim validation.
 */
inline bool ApplyP2CBountyBlock(std::map<COutPoint, CTxOut>& catalog, const CBlock& block,
                                const CBlockUndo* undo, const std::function<bool()>& cancelled)
{
    if (block.vtx.empty() || (undo && undo->vtxundo.size() + 1 != block.vtx.size())) return false;
    for (size_t step = 0; step < block.vtx.size(); ++step) {
        if (cancelled()) return false;
        const size_t i{undo ? block.vtx.size() - 1 - step : step};
        const auto& tx{*block.vtx[i]};
        if (!undo) {
            for (const auto& input : tx.vin) catalog.erase(input.prevout);
        }
        for (uint32_t n = 0; n < tx.vout.size(); ++n) {
            const COutPoint outpoint{tx.GetHash(), n};
            if (undo) catalog.erase(outpoint);
            else if (IsCanonicalP2COutput(tx.vout[n])) catalog.insert_or_assign(outpoint, tx.vout[n]);
        }
        if (undo && i > 0) {
            const auto& previous{undo->vtxundo[i - 1].vprevout};
            if (previous.size() != tx.vin.size()) return false;
            for (size_t n = 0; n < previous.size(); ++n) {
                if (IsCanonicalP2COutput(previous[n].out)) catalog.insert_or_assign(tx.vin[n].prevout, previous[n].out);
            }
        }
    }
    return true;
}
} // namespace node
#endif // CONNECTCOIN_NODE_P2C_BOUNTY_CATALOG_H
