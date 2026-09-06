// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_CLAIM_H
#define CONNECTCOIN_WALLET_P2C_CLAIM_H

#include <addresstype.h>
#include <consensus/p2c.h>
#include <primitives/transaction.h>
#include <util/result.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace wallet {
class CCoinControl;
class CWallet;

/** Fixed non-witness transaction. The fee comes only from the bounty, never
 * from the claimant's other coins. Adding a TLS proof must not change its txid.
 * The receiving destination is kept before this object is returned.
 */
struct P2CClaimProposal {
    CTransactionRef tx;
    CTxOut bounty;
    CTxDestination destination;
    CAmount fee;
    size_t proof_size;
    int64_t validation_time;
};

/** Prepare one confirmed bounty for a local-key wallet (which may be locked).
 * No TLS, mempool insertion, or transaction signing happens here.
 */
util::Result<P2CClaimProposal> PrepareP2CClaim(CWallet& wallet, const COutPoint& bounty,
                                           const CCoinControl& control, size_t proof_size = MAX_P2C_PROOF_SIZE);

/** Revalidate a saved unsigned proposal, including ownership, safe amounts,
 * bounty availability and current chain time. Does not reserve another key.
 */
util::Result<P2CClaimProposal> ResumeP2CClaim(CWallet& wallet, const CTransaction& proposal);

/** Attach the proof, require a wallet-owned P2PK payout, and run current node
 * acceptance checks, including certificate validation at the current tip MTP.
 * Does not store or broadcast. The caller can commit exactly this transaction.
 */
util::Result<CTransactionRef> CompleteP2CClaim(CWallet& wallet, const CTransaction& proposal,
                                            std::span<const unsigned char> proof);
} // namespace wallet

#endif // CONNECTCOIN_WALLET_P2C_CLAIM_H
