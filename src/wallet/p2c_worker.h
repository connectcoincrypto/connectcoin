// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_WORKER_H
#define CONNECTCOIN_WALLET_P2C_WORKER_H

#include <util/result.h>

#include <memory>
#include <string>
#include <vector>

class UniValue;

namespace wallet {
class CWallet;
inline constexpr int DEFAULT_P2C_CLAIM_CONCURRENCY{1000};
inline constexpr int DEFAULT_P2C_BOUNTY_LOOKBACK{600};
/** Wallet-owned, opt-in worker. Never owns a shared_ptr back to its wallet.
 * Stop before unloading the wallet/chain. All HTTPS is outside wallet locks.
 * Rate is aggregate per wallet: 0 disables, -1 explicitly means unlimited.
 */
class P2CClaimWorker {
public:
    virtual ~P2CClaimWorker() = default;
    //! recent_blocks includes the tip; 0 discovers all confirmed bounty ages.
    virtual util::Result<void> Configure(int connections_per_second, int concurrency, std::vector<std::string> domains = {}, std::string reward_address = {}, int recent_blocks = DEFAULT_P2C_BOUNTY_LOOKBACK) = 0;
    virtual void Stop() = 0;
    virtual void Shutdown() = 0;
    virtual UniValue Status() const = 0;
};
std::unique_ptr<P2CClaimWorker> MakeP2CClaimWorker(CWallet& wallet);
} // namespace wallet
#endif // CONNECTCOIN_WALLET_P2C_WORKER_H
