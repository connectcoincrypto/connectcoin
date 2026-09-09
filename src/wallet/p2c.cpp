// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/p2c.h>

#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/p2c.h>
#include <consensus/validation.h>
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
        !IsSupportedP2CRootCertificatesVersion(recipient.p2c->root_certificates_version) ||
        !IsValidP2CSignatureAlgorithmsMask(recipient.p2c->signature_algorithms_mask)) {
        return util::Error{Untranslated("Invalid P2C domain, root certificates version or signature algorithms mask")};
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
    batch->m_signature_algorithms_mask = recipient.p2c->signature_algorithms_mask;
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

util::Result<void> P2CTransactionBatch::PrepareSignatureAlgorithmsAlternative(uint8_t mask)
{
    LOCK(m_wallet->cs_wallet);
    if (m_commit_attempted || m_mask_selected || !m_alternative_transactions.empty() ||
        !IsValidP2CSignatureAlgorithmsMask(mask) || mask == m_signature_algorithms_mask) {
        return util::Error{Untranslated("Invalid or already finalized P2C signature mask alternative")};
    }
    if (m_wallet->IsLocked()) return util::Error{Untranslated("Wallet is locked")};
    // Construct locally first: an error must leave the original signed batch
    // and its reservation ownership unchanged.
    auto alternatives{m_transactions};
    for (auto& [created, count] : alternatives) {
        CMutableTransaction tx{*created.tx};
        size_t changed{0};
        for (auto& output : tx.vout) {
            if (auto p2c{output.GetPayToDomain()}) {
                if (p2c->signature_algorithms_mask != m_signature_algorithms_mask) {
                    return util::Error{Untranslated("Inconsistent P2C batch signature mask")};
                }
                p2c->signature_algorithms_mask = mask;
                output.SetPayToDomain(*p2c);
                ++changed;
            }
        }
        if (changed != count) return util::Error{Untranslated("Inconsistent P2C batch output count")};
        for (auto& input : tx.vin) {
            input.scriptSig.clear();
            input.scriptWitness.SetNull();
        }
        if (!m_wallet->SignTransaction(tx)) {
            return util::Error{Untranslated("Unable to sign the alternative P2C signature mask")};
        }
        const auto alternate{MakeTransactionRef(std::move(tx))};
        // Typed P2PK inputs have fixed-size signatures; changing this one-byte
        // field must not alter the displayed fee rate or transaction weight.
        if (GetTransactionWeight(*alternate) != GetTransactionWeight(*created.tx)) {
            return util::Error{Untranslated("Alternative P2C signature mask changed the transaction weight")};
        }
        created.tx = alternate;
    }
    m_alternative_transactions = std::move(alternatives);
    m_alternative_mask = mask;
    return {};
}

util::Result<void> P2CTransactionBatch::SelectSignatureAlgorithmsMask(uint8_t mask)
{
    if (m_commit_attempted || m_mask_selected) {
        return util::Error{Untranslated("P2C signature mask was already finalized")};
    }
    if (mask != m_signature_algorithms_mask) {
        if (mask != m_alternative_mask || m_alternative_transactions.empty()) {
            return util::Error{Untranslated("P2C signature mask was not prepared for approval")};
        }
        m_transactions.swap(m_alternative_transactions);
        m_signature_algorithms_mask = mask;
    }
    m_alternative_transactions.clear();
    m_alternative_mask = 0;
    m_mask_selected = true;
    return {};
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
