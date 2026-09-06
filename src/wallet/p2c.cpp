// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/p2c.h>

#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/p2c.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <serialize.h>
#include <sync.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <exception>
#include <limits>

namespace wallet {
P2CTransactionBatch::P2CTransactionBatch(std::shared_ptr<CWallet> wallet) : m_wallet(std::move(wallet)) {}

P2CTransactionBatch::~P2CTransactionBatch()
{
    LOCK(m_wallet->cs_wallet);
    for (const auto& outpoint : m_locked) m_wallet->UnlockCoin(outpoint, this);
}

util::Result<std::unique_ptr<P2CTransactionBatch>> P2CTransactionBatch::Prepare(
    std::shared_ptr<CWallet> wallet, const CRecipient& recipient, int64_t output_count, CCoinControl coin_control)
{
    LOCK(wallet->cs_wallet);
    if (!recipient.p2c || !IsCanonicalP2CDomain(recipient.p2c->domain) ||
        !IsSupportedP2CRootCertificatesVersion(recipient.p2c->root_certificates_version)) {
        return util::Error{Untranslated("Invalid P2C domain or root certificates version")};
    }
    if (recipient.nAmount <= 0 || !MoneyRange(recipient.nAmount) || output_count < 1 ||
        output_count > MAX_P2C_OUTPUT_COUNT || output_count > MAX_MONEY / recipient.nAmount) {
        return util::Error{Untranslated("Invalid P2C amount or output count")};
    }
    if (wallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) || wallet->IsWalletFlagSet(WALLET_FLAG_EXTERNAL_SIGNER)) {
        return util::Error{Untranslated("P2C creation requires a wallet with local private keys")};
    }
    if (wallet->IsLocked()) return util::Error{Untranslated("Wallet is locked")};

    // Fee evaluation takes an int32_t vsize and requires its result to fit in
    // int64_t. Compare ratios first: evaluating an extreme rate for a large
    // batch and only then checking MoneyRange is already too late. This is a
    // local arithmetic safeguard, not a consensus fee limit.
    const CFeeRate max_safe_rate{MAX_MONEY, std::numeric_limits<int32_t>::max()};
    const auto safe_rate = [&max_safe_rate](const CFeeRate& rate) {
        return rate >= CFeeRate{0} && rate <= max_safe_rate;
    };
    if (!safe_rate(GetMinimumFeeRate(*wallet, coin_control).fee_rate) ||
        !safe_rate(GetDiscardRate(*wallet)) || !safe_rate(wallet->m_consolidate_feerate)) {
        return util::Error{Untranslated("Fee rate is too high or invalid for safe P2C transaction construction")};
    }

    auto batch = std::unique_ptr<P2CTransactionBatch>(new P2CTransactionBatch(wallet));
    const uint64_t output_weight{WITNESS_SCALE_FACTOR * ::GetSerializeSize(CTxOut{recipient.nAmount, *recipient.p2c})};
    const int64_t max_outputs_per_tx{static_cast<int64_t>((MAX_STANDARD_TX_WEIGHT - MIN_TRANSACTION_WEIGHT) / output_weight)};
    if (max_outputs_per_tx < 1) return util::Error{Untranslated("P2C output cannot fit in a standard transaction")};
    if (output_count > max_outputs_per_tx) coin_control.m_min_depth = 1;

    const CAmount total_amount{recipient.nAmount * output_count};
    if (total_amount > AvailableCoins(*wallet, &coin_control).GetTotalAmount()) {
        return util::Error{Untranslated("Insufficient confirmed funds for the P2C transaction batch")};
    }
    int64_t remaining{output_count};
    while (remaining > 0) {
        const size_t candidate_max{static_cast<size_t>(std::min(remaining, max_outputs_per_tx))};
        std::optional<CreatedTransactionResult> selected;
        size_t selected_count{0};
        std::string last_error;
        const auto try_create = [&](size_t count) {
            return CreateTransaction(*wallet, std::vector<CRecipient>(count, recipient), std::nullopt, coin_control, /*sign=*/true);
        };
        auto candidate{try_create(candidate_max)};
        if (candidate) {
            selected = *candidate;
            selected_count = candidate_max;
        } else {
            last_error = util::ErrorString(candidate).original;
            size_t low{1};
            size_t high{candidate_max - 1};
            while (low <= high) {
                const size_t middle{low + (high - low) / 2};
                auto attempt{try_create(middle)};
                if (attempt) {
                    selected = *attempt;
                    selected_count = middle;
                    low = middle + 1;
                } else {
                    last_error = util::ErrorString(attempt).original;
                    high = middle - 1;
                }
            }
        }
        if (!selected) {
            return util::Error{Untranslated((batch->m_transactions.empty() ? "" : "P2C batch requires enough distinct confirmed inputs to fund every split transaction: ") + last_error)};
        }
        if (batch->m_transactions.empty() && selected_count < static_cast<size_t>(remaining) && coin_control.m_min_depth < 1) {
            // Full transaction overhead required a split: restart with confirmed
            // inputs so transactions remain independent of mempool cluster limits.
            coin_control.m_min_depth = 1;
            if (total_amount > AvailableCoins(*wallet, &coin_control).GetTotalAmount()) {
                return util::Error{Untranslated("Insufficient confirmed funds for the P2C transaction batch")};
            }
            continue;
        }
        if (selected->fee < 0 || selected->fee > MAX_MONEY - batch->m_fee) {
            return util::Error{Untranslated("Total P2C fees exceed the maximum money range")};
        }
        for (const CTxIn& input : selected->tx->vin) {
            // Never acquire or later release an existing user's lock, including
            // inputs explicitly selected through coin control.
            if (wallet->IsLockedCoin(input.prevout)) return util::Error{Untranslated("P2C input is already reserved")};
            batch->m_locked.push_back(input.prevout);
            if (!wallet->LockCoin(input.prevout, /*persist=*/false, batch.get())) {
                batch->m_locked.pop_back();
                return util::Error{Untranslated("Unable to reserve an input for the P2C transaction batch")};
            }
        }
        batch->m_fee += selected->fee;
        batch->m_transactions.emplace_back(*selected, selected_count);
        remaining -= static_cast<int64_t>(selected_count);
    }
    return batch;
}

util::Result<void> P2CTransactionBatch::Commit(const std::optional<std::string>& comment, const std::optional<std::string>& comment_to)
{
    LOCK(m_wallet->cs_wallet);
    if (m_commit_attempted) return util::Error{Untranslated("P2C batch was already submitted")};
    for (const auto& outpoint : m_locked) {
        if (m_wallet->IsSpent(outpoint)) return util::Error{Untranslated("A P2C input was spent while awaiting confirmation. Prepare a new batch.")};
        if (!m_wallet->OwnsCoinLock(outpoint, this)) return util::Error{Untranslated("A P2C input reservation changed while awaiting confirmation. Prepare a new batch.")};
    }
    m_commit_attempted = true;
    // Retain cs_wallet across releasing reservations and committing the batch.
    for (const auto& outpoint : m_locked) m_wallet->UnlockCoin(outpoint, this);
    m_locked.clear();
    try {
        for (const auto& [created, count] : m_transactions) {
            m_wallet->CommitTransaction(created.tx, std::nullopt, comment, comment_to);
            ++m_committed_count;
        }
    } catch (const std::exception& e) {
        return util::Error{Untranslated(std::string{"P2C submission failed; check wallet history before retrying: "} + e.what())};
    }
    return {};
}
} // namespace wallet
