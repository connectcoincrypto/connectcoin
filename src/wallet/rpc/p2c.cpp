// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/p2c.h>
#include <core_io.h>
#include <key_io.h>
#include <rpc/util.h>
#include <sync.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/p2c_claim.h>
#include <wallet/p2c_worker.h>
#include <wallet/rpc/util.h>
#include <wallet/wallet.h>

#include <univalue.h>

#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>

namespace wallet {
namespace {
std::optional<CTxDestination> ClaimRewardDestination(const UniValue& value)
{
    if (value.isNull() || value.get_str().empty()) return std::nullopt;
    auto destination{DecodeDestination(value.get_str())};
    if (!IsValidDestination(destination) || !CTxOut{0, GetScriptForDestination(destination)}.GetP2PKPubKey()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Expected a type-1 P2PK reward address for this network");
    }
    return destination;
}

RPCResult ClaimWorkerResult()
{
    return RPCResult{RPCResult::Type::OBJ, "", "", {
        {RPCResult::Type::NUM, "connections_per_second", "Aggregate per-wallet rate; 0 disables, -1 is unlimited."},
        {RPCResult::Type::NUM, "concurrency", "Maximum simultaneous TLS handshakes."},
        {RPCResult::Type::STR, "reward_address", "Explicit destination for new searches; empty means this wallet. Completed proofs retain their original destination."},
        {RPCResult::Type::NUM, "domain_rounds", "Connection assignments since the last configuration (legacy field name)."},
        {RPCResult::Type::NUM, "schedule_refreshes", "Completed bounty/priority refreshes since the last configuration."},
        {RPCResult::Type::STR, "state", "Current worker state."},
        {RPCResult::Type::STR, "domain", "Most recently processed domain."},
        {RPCResult::Type::NUM, "attempts", "Started connection attempts, saved between worker restarts."},
        {RPCResult::Type::NUM, "submitted", "Claims stored and submitted by this worker, not necessarily confirmed."},
        {RPCResult::Type::STR, "last_error", "Last diagnostic, if any."},
        {RPCResult::Type::STR, "last_txid", "Last submitted claim transaction id, or empty."},
    }};
}
}

RPCMethod setp2cclaiming()
{
    return RPCMethod{
        "setp2cclaiming",
        "Enable or stop automatic wallet P2C claiming. Discovers confirmed bounties, generates TLS proofs and submits rewards automatically.\n"
        "Makes direct HTTPS connections to public IP addresses on port 443; configured proxies are never bypassed.\n"
        "Rate 0 disables HTTPS, -1 explicitly selects unlimited rate. Limits are per wallet, not across all loaded wallets.\n"
        "Wallets start disabled after reload; saved proposals and completed proofs resume only after explicit enabling.\n",
        {
            {"connections_per_second", RPCArg::Type::NUM, RPCArg::Optional::NO, "0 to disable, -1 for unlimited, otherwise a positive aggregate rate."},
            {"concurrency", RPCArg::Type::NUM, RPCArg::Default{DEFAULT_P2C_CLAIM_CONCURRENCY}, "Maximum simultaneous handshakes; any positive 32-bit integer. Actual capacity depends on system resources."},
            {"domains", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Optional allowlist. Empty means all canonical public domains with bounties.", {
                {"domain", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Canonical lower-case ASCII domain."},
            }},
            {"address", RPCArg::Type::STR, RPCArg::DefaultHint{"this wallet"}, "Optional reward address. Changing it restarts unfinished searches; completed proofs keep their original destination."},
        },
        ClaimWorkerResult(),
        RPCExamples{HelpExampleCli("setp2cclaiming", "1 4") + HelpExampleCli("setp2cclaiming", "0")},
        [](const RPCMethod&, const JSONRPCRequest& request) -> UniValue {
            const auto wallet{GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            std::vector<std::string> domains;
            if (!request.params[2].isNull()) {
                if (request.params[2].size() > 256) throw JSONRPCError(RPC_INVALID_PARAMETER, "At most 256 domain filters");
                for (const auto& domain : request.params[2].getValues()) domains.push_back(domain.get_str());
            }
            const int rate{request.params[0].getInt<int>()};
            const int concurrency{request.params[1].isNull() ? DEFAULT_P2C_CLAIM_CONCURRENCY : request.params[1].getInt<int>()};
            auto& worker{wallet->GetP2CClaimWorker()};
            if (auto result{worker.Configure(rate, concurrency, std::move(domains), request.params[3].isNull() ? "" : request.params[3].get_str())}; !result) {
                throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(result).original);
            }
            return worker.Status();
        },
    };
}

RPCMethod getp2cclaimstatus()
{
    return RPCMethod{
        "getp2cclaimstatus", "Return automatic P2C claim progress without starting HTTPS.\n", {},
        ClaimWorkerResult(), RPCExamples{HelpExampleCli("getp2cclaimstatus", "")},
        [](const RPCMethod&, const JSONRPCRequest& request) -> UniValue {
            const auto wallet{GetWalletForJSONRPCRequest(request)};
            return wallet ? wallet->GetP2CClaimWorker().Status() : UniValue{UniValue::VNULL};
        },
    };
}

RPCMethod preparep2cclaim()
{
    return RPCMethod{
        "preparep2cclaim",
        "Prepare a claim of one confirmed P2C bounty, paying this wallet by default or the specified address. Fees are deducted only from the bounty.\n"
        "The returned transaction and its challenge are fixed before proof generation; changing outputs or fees requires a new proof.\n"
        "By default this reserves a wallet receiving address, but it neither spends coins nor makes HTTPS connections. A locked wallet can use its existing keypool.\n",
        {
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Transaction containing the bounty."},
            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "Output index of the bounty."},
            {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"wallet fee estimation"}, "Fee rate in " + CURRENCY_ATOM + "/vB (not CC/kvB)."},
            {"proof_size", RPCArg::Type::NUM, RPCArg::Default{MAX_P2C_PROOF_SIZE}, "Proof bytes to budget for fees (1-65536). Defaults to the full consensus limit. Unused budget is still paid as a fee; it cannot be refunded without changing the challenge."},
            {"address", RPCArg::Type::STR, RPCArg::DefaultHint{"this wallet"}, "Optional type-1 P2PK payout. Specify the same address in submitp2cclaim to authorize an external payout."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR_HEX, "hex", "Unwitnessed claim transaction. Keep this unchanged during proof generation."},
            {RPCResult::Type::STR_HEX, "txid", "Claim transaction id, not the funding transaction id."},
            {RPCResult::Type::NUM, "input_index", "Always zero for this single-bounty claim."},
            {RPCResult::Type::STR_HEX, "clienthello_random", "Exact challenge bytes; do not reverse them."},
            {RPCResult::Type::STR, "address", "Reward address (new wallet address by default)."},
            {RPCResult::Type::STR, "domain", "Domain from the on-chain bounty."},
            {RPCResult::Type::STR_HEX, "connection_work_target", "Largest accepted work hash."},
            {RPCResult::Type::NUM, "root_certificates_version", "Immutable trusted-root bundle."},
            {RPCResult::Type::NUM, "signature_algorithms_mask", "Allowed CertificateVerify schemes: 1=ECDSA, 2=RSA-PSS-RSAE, 4=RSA-PSS-PSS."},
            {RPCResult::Type::NUM_TIME, "validation_time", "Chain median time past when prepared; checked again on submission."},
            {RPCResult::Type::NUM, "proof_size", "Proof bytes budgeted for fees."},
            {RPCResult::Type::STR_AMOUNT, "bounty_amount", "Gross bounty in CC."},
            {RPCResult::Type::STR_AMOUNT, "fee", "Fixed fee in CC."},
            {RPCResult::Type::STR_AMOUNT, "receive_amount", "Net payout in CC."},
        }},
        RPCExamples{HelpExampleCli("preparep2cclaim", "\"txid\" 0")},
        [](const RPCMethod&, const JSONRPCRequest& request) -> UniValue {
            const auto wallet{GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            const auto txid{Txid::FromUint256(ParseHashV(request.params[0], "txid"))};
            const auto vout{request.params[1].getInt<int64_t>()};
            if (vout < 0 || vout > std::numeric_limits<uint32_t>::max()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "vout must be a uint32 output index");
            }
            const int64_t proof_size{request.params[3].isNull() ? static_cast<int64_t>(MAX_P2C_PROOF_SIZE) : request.params[3].getInt<int64_t>()};
            if (proof_size < 1 || proof_size > static_cast<int64_t>(MAX_P2C_PROOF_SIZE)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "proof_size must be between 1 and 65536 bytes");
            }
            CCoinControl control;
            if (!request.params[2].isNull()) control.m_feerate = CFeeRate{AmountFromValue(request.params[2], /*decimals=*/3)};
            wallet->BlockUntilSyncedToCurrentChain();
            const auto prepared{PrepareP2CClaim(*wallet, COutPoint{txid, static_cast<uint32_t>(vout)}, control, proof_size, ClaimRewardDestination(request.params[4]))};
            if (!prepared) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(prepared).original);
            const auto bounty{prepared->bounty.GetPayToDomain()};
            const uint256 challenge{P2CClaimChallenge(*prepared->tx, 0)};
            UniValue result{UniValue::VOBJ};
            result.pushKV("hex", EncodeHexTx(*prepared->tx));
            result.pushKV("txid", prepared->tx->GetHash().GetHex());
            result.pushKV("input_index", 0);
            result.pushKV("clienthello_random", HexStr(challenge));
            result.pushKV("address", EncodeDestination(prepared->destination));
            result.pushKV("domain", bounty->domain);
            result.pushKV("connection_work_target", bounty->connection_work_target.GetHex());
            result.pushKV("root_certificates_version", bounty->root_certificates_version);
            result.pushKV("signature_algorithms_mask", bounty->signature_algorithms_mask);
            result.pushKV("validation_time", prepared->validation_time);
            result.pushKV("proof_size", static_cast<uint64_t>(prepared->proof_size));
            result.pushKV("bounty_amount", ValueFromAmount(prepared->bounty.nValue));
            result.pushKV("fee", ValueFromAmount(prepared->fee));
            result.pushKV("receive_amount", ValueFromAmount(prepared->tx->vout[0].nValue));
            return result;
        },
    };
}

RPCMethod submitp2cclaim()
{
    return RPCMethod{
        "submitp2cclaim",
        "Validate a prepared P2C claim and its complete binary TLS proof, then store it in this wallet and submit using walletbroadcast.\n"
        "Accepts only a single P2C input and a single P2PK payout owned by this wallet or matching the explicit address. It cannot spend ordinary wallet coins.\n"
        "No HTTPS connections are made. No private-key signature or wallet unlock is needed.\n",
        {
            {"hex", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Unwitnessed transaction from preparep2cclaim."},
            {"proof", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Complete version-2 binary TLS proof encoded as hex, not the JSON envelope. CertificateVerify remains mandatory but is excluded from connection work."},
            {"address", RPCArg::Type::STR, RPCArg::DefaultHint{"this wallet"}, "Explicitly authorize this exact reward address. Required for payouts not owned by this wallet."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR_HEX, "txid", "Claim transaction id, unchanged by proof attachment."},
            {RPCResult::Type::STR_HEX, "wtxid", "Witness transaction id."},
            {RPCResult::Type::BOOL, "stored", "Transaction has been stored in the wallet."},
            {RPCResult::Type::BOOL, "in_mempool", "Whether the local mempool contains the claim when checked. False is not a confirmation or acceptance guarantee; check wallet history, especially with walletbroadcast disabled or a concurrent chain/conflict update."},
        }},
        RPCExamples{HelpExampleCli("submitp2cclaim", "\"hex\" \"proof\"")},
        [](const RPCMethod&, const JSONRPCRequest& request) -> UniValue {
            const auto wallet{GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            const auto& hex{request.params[0].get_str()};
            const auto& proof_hex{request.params[1].get_str()};
            if (hex.size() > MAX_P2C_PROOF_SIZE * 2) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "P2C claim proposal is too large");
            }
            if (proof_hex.empty() || proof_hex.size() > MAX_P2C_PROOF_SIZE * 2 || !IsHex(proof_hex)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "proof must encode 1 through 65536 bytes as hex");
            }
            CMutableTransaction tx;
            if (!DecodeHexTx(tx, hex)) throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
            const auto proof{ParseHex(proof_hex)};
            wallet->BlockUntilSyncedToCurrentChain();
            LOCK(wallet->cs_wallet);
            const auto completed{CompleteP2CClaim(*wallet, CTransaction{tx}, proof, ClaimRewardDestination(request.params[2]))};
            if (!completed) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(completed).original);
            try {
                wallet->CommitTransaction(*completed);
            } catch (const std::exception& e) {
                throw JSONRPCError(RPC_WALLET_ERROR, "P2C claim storage/submission failed; check wallet history before retrying: " + std::string{e.what()});
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("txid", (*completed)->GetHash().GetHex());
            result.pushKV("wtxid", (*completed)->GetWitnessHash().GetHex());
            result.pushKV("stored", true);
            result.pushKV("in_mempool", wallet->chain().isInMempool((*completed)->GetHash()));
            return result;
        },
    };
}
} // namespace wallet
