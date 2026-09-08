// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/p2c_claim.h>

#include <coins.h>
#include <consensus/consensus.h>
#include <crypto/common.h>
#include <interfaces/chain.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <script/solver.h>
#include <sync.h>
#include <util/strencodings.h>
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
P2CClaimPriority MultiplyP2CTarget(const uint256& target, uint64_t payout)
{
    P2CClaimPriority result{};
    constexpr uint64_t MASK{0xffffffff};
    // Eight target words times two payout words, accumulated into ten words.
    // Each multiply/add is at most (2^32-1)^2 + 2*(2^32-1) = 2^64-1.
    for (size_t i = 0; i < 8; ++i) {
        uint64_t carry{0};
        const uint64_t word{ReadLE32(target.begin() + 4 * i)};
        for (size_t j = 0; j < 2; ++j) {
            const size_t pos{9 - (i + j)};
            const uint64_t product{word * ((payout >> (32 * j)) & MASK) + result[pos] + carry};
            result[pos] = static_cast<uint32_t>(product & MASK);
            carry = product >> 32;
        }
        result[7 - i] = static_cast<uint32_t>(carry);
    }
    // T*N + N handles the inclusive target, including T=2^256-1, without
    // wrapping a 256-bit T+1. The full result fits in 320 bits.
    uint64_t carry{payout};
    for (auto it = result.rbegin(); it != result.rend(); ++it) {
        const uint64_t sum{*it + (carry & MASK)};
        *it = static_cast<uint32_t>(sum & MASK);
        carry = (carry >> 32) + (sum >> 32);
    }
    return result;
}
} // namespace

P2CClaimPriority GetP2CClaimPriority(const uint256& target, CAmount net_reward)
{
    if (net_reward <= 0 || !MoneyRange(net_reward)) return {};
    return MultiplyP2CTarget(target, static_cast<uint64_t>(net_reward));
}

bool IsP2CClaimAttemptLimitExceeded(const uint256& target, uint64_t attempts)
{
    // attempts * (target + 1) > 2^257. The full uint64 range fits in
    // 320 bits, including target=2^256-1; no rounded division or overflow.
    constexpr P2CClaimPriority twice_space{0, 2};
    return MultiplyP2CTarget(target, attempts) > twice_space;
}

util::Result<CAmount> CalculateP2CClaimFee(const CFeeRate& rate, size_t proof_size)
{
    if (proof_size == 0 || proof_size > MAX_P2C_PROOF_SIZE) {
        return util::Error{Untranslated("P2C proof_size must be between 1 and 65536 bytes")};
    }
    if (!SafeFeeRate(rate)) return util::Error{Untranslated("Fee rate is too high or invalid for safe P2C claim construction")};
    // The outpoint, amount and 32-byte payout key do not change the wire size.
    CMutableTransaction tx;
    tx.vin.emplace_back();
    static const XOnlyPubKey SIZE_KEY{ParseHex("79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798")};
    tx.vout.emplace_back(0, SIZE_KEY);
    tx.vin[0].scriptWitness.stack.emplace_back(proof_size, 0);
    const auto vsize{GetVirtualTransactionSize(CTransaction{tx})};
    const CAmount fee{rate.GetFee(static_cast<int32_t>(vsize))};
    if (!MoneyRange(fee)) return util::Error{Untranslated("P2C claim fee is out of range")};
    return fee;
}

bool IsP2CClaimPayout(CWallet& wallet, const CTxOut& output, const std::optional<CTxDestination>& destination)
{
    const auto key{output.GetP2PKPubKey()};
    if (!key) return false;
    if (destination) return key == CTxOut{0, GetScriptForDestination(*destination)}.GetP2PKPubKey();
    LOCK(wallet.cs_wallet);
    return wallet.IsMine(output);
}

util::Result<P2CClaimProposal> PrepareP2CClaim(CWallet& wallet, const COutPoint& bounty,
                                           const CCoinControl& control, size_t proof_size,
                                           const std::optional<CTxDestination>& payout_destination)
{
    LOCK(wallet.cs_wallet);
    if (!payout_destination && !HasLocalKeys(wallet)) return util::Error{Untranslated("P2C claiming requires a wallet with local private keys or an explicit reward address")};
    if (proof_size == 0 || proof_size > MAX_P2C_PROOF_SIZE) {
        return util::Error{Untranslated("P2C proof_size must be between 1 and 65536 bytes")};
    }
    auto coin{FindClaimBounty(wallet, bounty)};
    if (!coin) return util::Error{util::ErrorString(coin)};

    if (control.m_feerate && !SafeFeeRate(*control.m_feerate)) {
        return util::Error{Untranslated("Fee rate is too high or invalid for safe P2C claim construction")};
    }
    const auto estimate{GetMinimumFeeRate(wallet, control)};
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
    auto destination{payout_destination ? util::Result<CTxDestination>{*payout_destination} : reserved.GetReservedDestination(/*internal=*/false)};
    if (!destination) return util::Error{util::ErrorString(destination)};
    CMutableTransaction tx;
    tx.vin.emplace_back(bounty);
    tx.vout.emplace_back(coin->out.nValue, GetScriptForDestination(*destination));
    if (!IsP2CClaimPayout(wallet, tx.vout[0], payout_destination)) {
        return util::Error{Untranslated("P2C payout must be a wallet-owned P2PK destination or the explicitly specified reward address")};
    }
    // Includes witness marker/flags and CompactSize transitions. Never revise
    // the payout/fee after committing the ClientHello to this transaction.
    const auto fee{CalculateP2CClaimFee(rate, proof_size)};
    if (!fee) return util::Error{util::ErrorString(fee)};
    if (*fee > wallet.m_default_max_tx_fee || *fee >= coin->out.nValue) {
        return util::Error{Untranslated("P2C claim fee exceeds the wallet maximum or consumes the entire bounty")};
    }
    tx.vout[0].nValue -= *fee;
    if (IsDust(tx.vout[0], dust_rate)) return util::Error{Untranslated("P2C reward after fees is dust")};
    if (!payout_destination) reserved.KeepDestination();
    return P2CClaimProposal{MakeTransactionRef(std::move(tx)), coin->out, *destination, *fee, proof_size, validation_time};
}

util::Result<P2CClaimProposal> ResumeP2CClaim(CWallet& wallet, const CTransaction& proposal,
                                          const std::optional<CTxDestination>& destination)
{
    LOCK(wallet.cs_wallet);
    if (!destination && !HasLocalKeys(wallet)) return util::Error{Untranslated("P2C claiming requires a wallet with local private keys or an explicit reward address")};
    if (proposal.version != CTransaction::CURRENT_VERSION || proposal.nLockTime != 0 ||
        proposal.vin.size() != 1 || proposal.vout.size() != 1 ||
        !proposal.vin[0].scriptSig.empty() || proposal.HasWitness() ||
        proposal.vin[0].nSequence != CTxIn::SEQUENCE_FINAL) {
        return util::Error{Untranslated("Expected an unwitnessed P2C claim with one input and one output, version 2, final sequence and no locktime")};
    }
    if (!IsP2CClaimPayout(wallet, proposal.vout[0], destination)) {
        return util::Error{Untranslated("P2C payout must be a P2PK destination belonging to this wallet or match the explicitly specified reward address")};
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
                                            std::span<const unsigned char> proof,
                                            const std::optional<CTxDestination>& destination)
{
    LOCK(wallet.cs_wallet);
    auto prepared{ResumeP2CClaim(wallet, proposal, destination)};
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
