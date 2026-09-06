// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_WORKER_H
#define CONNECTCOIN_WALLET_P2C_WORKER_H

#include <util/result.h>
#include <univalue.h>

#include <memory>
#include <string>
#include <vector>

namespace wallet {
class CWallet;
/** Wallet-owned, opt-in worker. Never owns a shared_ptr back to its wallet.
 * Stop before unloading the wallet/chain. All HTTPS is outside wallet locks.
 * Rate is aggregate per wallet: 0 disables, -1 explicitly means unlimited.
 */
class P2CClaimWorker {
public:
    virtual ~P2CClaimWorker() = default;
    virtual util::Result<void> Configure(int connections_per_second, int concurrency, std::vector<std::string> domains = {}) = 0;
    virtual void Stop() = 0;
    virtual void Shutdown() = 0;
    virtual UniValue Status() const = 0;
};
std::unique_ptr<P2CClaimWorker> MakeP2CClaimWorker(CWallet& wallet);
} // namespace wallet
#endif // CONNECTCOIN_WALLET_P2C_WORKER_H
