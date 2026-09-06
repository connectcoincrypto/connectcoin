// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/p2c_claim.h>

#include <coins.h>
#include <consensus/consensus.h>
#include <interfaces/chain.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <script/solver.h>
#include <sync.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace wallet {
namespace {
util::Result<Coin> FindClaimBounty(CWallet& wallet, const COutPoint& outpoint)
    EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    if (outpoint.IsNull()) return util::Error{Untranslated("Invalid P2C bounty outpoint")};
    if (wallet.IsLockedCoin(outpoint)) return util::Error{Untranslated("P2C bounty is locked in this wallet")};
    std::map<COutPoint, Coin> coins{{outpoint, Coin{}}};
    wallet.chain().findCoins(coins);
    auto& coin{coins.at(outpoint)};
    if (coin.IsSpent() || wallet.chain().isSpentByMempool(outpoint)) {
        return util::Error{Untranslated("P2C bounty is unavailable or already spent")};
    }
    if (!IsCanonicalP2COutput(coin.out)) return util::Error{Untranslated("Outpoint is not a supported P2C bounty")};
    // Start with confirmed bounties: no descendant limits, CPFP costs, or work
    // wasted on a funding transaction that can simply be replaced.
    const int height{wallet.GetLastBlockHeight()};
    if (height < 0 || coin.nHeight > static_cast<uint32_t>(height)) {
        return util::Error{Untranslated("P2C bounty must be confirmed before claiming")};
    }
    if (coin.IsCoinBase() && int64_t{height} + 1 - coin.nHeight < COINBASE_MATURITY) {
        return util::Error{Untranslated("P2C coinbase bounty is not mature")};
    }
    if (coin.out.nValue <= 0 || !MoneyRange(coin.out.nValue)) {
        return util::Error{Untranslated("Invalid P2C bounty amount")};
    }
    return std::move(coin);
}

bool HasLocalKeys(const CWallet& wallet)
{
    return !wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) &&
           !wallet.IsWalletFlagSet(WALLET_FLAG_EXTERNAL_SIGNER);
}

bool SafeFeeRate(const CFeeRate& rate)
{
    return rate >= CFeeRate{0} && rate <= CFeeRate{MAX_MONEY, std::numeric_limits<int32_t>::max()};
}
} // namespace

util::Result<P2CClaimProposal> PrepareP2CClaim(CWallet& wallet, const COutPoint& bounty,
                                           const CCoinControl& control, size_t proof_size)
{
    LOCK(wallet.cs_wallet);
    if (!HasLocalKeys(wallet)) return util::Error{Untranslated("P2C claiming requires a wallet with local private keys")};
    if (proof_size == 0 || proof_size > MAX_P2C_PROOF_SIZE) {
        return util::Error{Untranslated("P2C proof_size must be between 1 and 65536 bytes")};
    }
    auto coin{FindClaimBounty(wallet, bounty)};
    if (!coin) return util::Error{util::ErrorString(coin)};

    if (control.m_feerate && !SafeFeeRate(*control.m_feerate)) {
        return util::Error{Untranslated("Fee rate is too high or invalid for safe P2C claim construction")};
    }
    const auto estimate{GetMinimumFeeRate(wallet, control)};
    if (estimate.fee_reason == FeeReason::FALLBACK && !wallet.m_allow_fallback_fee) {
        return util::Error{Untranslated("Fee estimation failed. Set an explicit fee_rate or enable fallback fees")};
    }
    const CFeeRate rate{std::max({estimate.fee_rate, GetRequiredFeeRate(wallet), wallet.chain().mempoolMinFee()})};
    const CFeeRate dust_rate{wallet.chain().relayDustFee()};
    if (!SafeFeeRate(estimate.fee_rate) || !SafeFeeRate(rate) || !SafeFeeRate(dust_rate)) {
        return util::Error{Untranslated("Fee rate is too high or invalid for safe P2C claim construction")};
    }

    int64_t validation_time{0};
    bool active{false};
    if (!wallet.chain().findBlock(wallet.GetLastBlockHash(), interfaces::FoundBlock().mtpTime(validation_time).inActiveChain(active)) ||
        !active || validation_time <= 0) {
        return util::Error{Untranslated("Wallet must be synchronized to an active chain before claiming")};
    }

    ReserveDestination reserved{&wallet, OutputType::BECH32M};
    auto destination{reserved.GetReservedDestination(/*internal=*/false)};
    if (!destination) return util::Error{util::ErrorString(destination)};
    CMutableTransaction tx;
    tx.vin.emplace_back(bounty);
    tx.vout.emplace_back(coin->out.nValue, GetScriptForDestination(*destination));
    if (!tx.vout[0].GetP2PKPubKey() || !wallet.IsMine(tx.vout[0])) {
        return util::Error{Untranslated("P2C payout must be a P2PK destination belonging to this wallet")};
    }
    // Size the entire witnessed transaction, including marker/flags and the
    // CompactSize transition at 65536 bytes. Never revise outputs after TLS.
    tx.vin[0].scriptWitness.stack.emplace_back(proof_size, 0);
    const auto vsize{GetVirtualTransactionSize(CTransaction{tx})};
    const CAmount fee{rate.GetFee(static_cast<int32_t>(vsize))};
    if (!MoneyRange(fee) || fee > wallet.m_default_max_tx_fee || fee >= coin->out.nValue) {
        return util::Error{Untranslated("P2C claim fee exceeds the wallet maximum or consumes the entire bounty")};
    }
    tx.vout[0].nValue -= fee;
    if (IsDust(tx.vout[0], dust_rate)) return util::Error{Untranslated("P2C reward after fees is dust")};
    tx.vin[0].scriptWitness.stack.clear();
    reserved.KeepDestination();
    return P2CClaimProposal{MakeTransactionRef(std::move(tx)), coin->out, *destination, fee, proof_size, validation_time};
}

util::Result<P2CClaimProposal> ResumeP2CClaim(CWallet& wallet, const CTransaction& proposal)
{
    LOCK(wallet.cs_wallet);
    if (!HasLocalKeys(wallet)) return util::Error{Untranslated("P2C claiming requires a wallet with local private keys")};
    if (proposal.version != CTransaction::CURRENT_VERSION || proposal.nLockTime != 0 ||
        proposal.vin.size() != 1 || proposal.vout.size() != 1 ||
        !proposal.vin[0].scriptSig.empty() || proposal.HasWitness() ||
        proposal.vin[0].nSequence != CTxIn::SEQUENCE_FINAL) {
        return util::Error{Untranslated("Expected an unwitnessed P2C claim with one input and one output, version 2, final sequence and no locktime")};
    }
    if (!proposal.vout[0].GetP2PKPubKey() || !wallet.IsMine(proposal.vout[0])) {
        return util::Error{Untranslated("P2C payout must be a P2PK destination belonging to this wallet")};
    }
    auto coin{FindClaimBounty(wallet, proposal.vin[0].prevout)};
    if (!coin) return util::Error{util::ErrorString(coin)};
    const CAmount payout{proposal.vout[0].nValue};
    if (payout <= 0 || !MoneyRange(payout) || payout > coin->out.nValue) {
        return util::Error{Untranslated("Invalid P2C payout amount")};
    }
    if (coin->out.nValue - payout > wallet.m_default_max_tx_fee) {
        return util::Error{Untranslated("P2C claim fee exceeds the wallet maximum")};
    }
    int64_t validation_time{0};
    bool active{false};
    if (!wallet.chain().findBlock(wallet.GetLastBlockHash(), interfaces::FoundBlock().mtpTime(validation_time).inActiveChain(active)) ||
        !active || validation_time <= 0) {
        return util::Error{Untranslated("Wallet must be synchronized to an active chain before claiming")};
    }
    return P2CClaimProposal{MakeTransactionRef(proposal), coin->out, CNoDestination{}, coin->out.nValue - payout, MAX_P2C_PROOF_SIZE, validation_time};
}

util::Result<CTransactionRef> CompleteP2CClaim(CWallet& wallet, const CTransaction& proposal,
                                            std::span<const unsigned char> proof)
{
    LOCK(wallet.cs_wallet);
    auto prepared{ResumeP2CClaim(wallet, proposal)};
    if (!prepared) return util::Error{util::ErrorString(prepared)};
    if (proof.empty() || proof.size() > MAX_P2C_PROOF_SIZE) {
        return util::Error{Untranslated("P2C proof must be between 1 and 65536 bytes")};
    }
    const auto domain{prepared->bounty.GetPayToDomain()};
    P2CTlsProofView parsed;
    std::string error;
    if (!ParseP2CTlsProof(proof, domain->domain, P2CClaimChallenge(proposal, 0), parsed, error)) {
        return util::Error{Untranslated("Invalid P2C proof: " + error)};
    }
    if (!P2CMeetsWorkTarget(parsed.connection_work_hash, domain->connection_work_target)) {
        return util::Error{Untranslated("P2C connection work hash exceeds target")};
    }
    CMutableTransaction tx{proposal};
    tx.vin[0].scriptWitness.stack.emplace_back(proof.begin(), proof.end());
    auto completed{MakeTransactionRef(std::move(tx))};
    // Includes X.509, CertificateVerify, current MTP, fee/weight/standardness,
    // and conflicts. Do not mistake structural parsing for full validation.
    if (auto accepted{wallet.chain().checkTransaction(completed)}; !accepted) {
        return util::Error{Untranslated("P2C claim rejected: " + util::ErrorString(accepted).original)};
    }
    return completed;
}
} // namespace wallet
