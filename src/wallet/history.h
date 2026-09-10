// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_HISTORY_H
#define CONNECTCOIN_WALLET_HISTORY_H

#include <util/fs.h>

#include <cstddef>

struct bilingual_str;

namespace wallet {
class WalletDatabase;

/** Offline, explicitly destructive test-network recovery. The database must
 * already be exclusively opened. Verify its identity, create and verify a new
 * backup, then atomically erase TX, WTX_VARIANT and LOCKED_UTXO records only.
 * All other records, including the old chain locator, remain byte-for-byte
 * unchanged. Never load or convert historical transaction serialization.
 * A failed operation retains its backup directory for manual inspection.
 */
bool ResetTransactionHistory(WalletDatabase& database, const fs::path& backup_dir,
                             size_t& removed_records, bilingual_str& error);
} // namespace wallet

#endif // CONNECTCOIN_WALLET_HISTORY_H
