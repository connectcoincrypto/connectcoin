// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_H
#define CONNECTCOIN_WALLET_P2C_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <util/result.h>
#include <wallet/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wallet {
class CCoinControl;
class CWallet;
struct CRecipient;

inline constexpr int64_t MAX_P2C_OUTPUT_COUNT{1000};

/** Signed transactions awaiting approval. Owns temporary input reservations;
 * destroying the batch (including cancellation) releases only its reservations.
 * The shared wallet reference keeps reservations safe across wallet unloading.
 */
class P2CTransactionBatch
{
public:
    static util::Result<std::unique_ptr<P2CTransactionBatch>> Prepare(
        std::shared_ptr<CWallet> wallet, const CRecipient& recipient, int64_t output_count, CCoinControl coin_control);
    ~P2CTransactionBatch();
    P2CTransactionBatch(const P2CTransactionBatch&) = delete;
    P2CTransactionBatch& operator=(const P2CTransactionBatch&) = delete;

    const auto& GetTransactions() const { return m_transactions; }
    CAmount GetFee() const { return m_fee; }
    size_t GetCommittedCount() const { return m_committed_count; }
    /** Commits exactly the reviewed transactions, never regenerating their fees.
     * A storage error can leave a partially committed batch; callers must report
     * this and must not automatically retry the original request.
     */
    util::Result<void> Commit(const std::optional<std::string>& comment = {}, const std::optional<std::string>& comment_to = {});

private:
    explicit P2CTransactionBatch(std::shared_ptr<CWallet> wallet);
    std::shared_ptr<CWallet> m_wallet;
    std::vector<std::pair<CreatedTransactionResult, size_t>> m_transactions;
    std::vector<COutPoint> m_locked;
    CAmount m_fee{0};
    size_t m_committed_count{0};
    bool m_commit_attempted{false};
};
} // namespace wallet

#endif // CONNECTCOIN_WALLET_P2C_H
