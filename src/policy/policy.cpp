// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// NOTE: This file is intended to be customised by the end user, and includes only local node policy logic

#include <policy/policy.h>

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/p2c.h>
#include <consensus/validation.h>
#include <policy/feerate.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/solver.h>
#include <serialize.h>
#include <span.h>
#include <tinyformat.h>

#include <algorithm>
#include <cstddef>
#include <vector>

CAmount GetDustThreshold(const CTxOut& txout, const CFeeRate& dustRelayFeeIn)
{
    // "Dust" is defined in terms of dustRelayFee,
    // which has units connects per kilobyte.
    // If you'd pay more in fees than the value of the output
    // to spend something, then we consider it dust.
    // A type-1 input is 58 vbytes. A P2C proof is witness data, so use the
    // maximum canonical proof size when estimating the cost of spending a
    // type-2 output. This is relay policy only; it is not a consensus rule.
    uint64_t nSize{GetSerializeSize(txout)};
    if (txout.GetType() == TxOutputType::PAY_TO_CONNECT) {
        const uint64_t witness_size{GetSizeOfCompactSize(1) + GetSizeOfCompactSize(MAX_P2C_PROOF_SIZE) + MAX_P2C_PROOF_SIZE};
        nSize += 41 + (witness_size + WITNESS_SCALE_FACTOR - 1) / WITNESS_SCALE_FACTOR;
    } else {
        nSize += 58;
    }

    return dustRelayFeeIn.GetFee(nSize);
}

bool IsDust(const CTxOut& txout, const CFeeRate& dustRelayFeeIn)
{
    return (txout.nValue < GetDustThreshold(txout, dustRelayFeeIn));
}

std::vector<uint32_t> GetDust(const CTransaction& tx, CFeeRate dust_relay_rate)
{
    std::vector<uint32_t> dust_outputs;
    for (uint32_t i{0}; i < tx.vout.size(); ++i) {
        if (IsDust(tx.vout[i], dust_relay_rate)) dust_outputs.push_back(i);
    }
    return dust_outputs;
}

bool IsStandard(const CScript& scriptPubKey, TxoutType& whichType)
{
    std::vector<std::vector<unsigned char> > vSolutions;
    whichType = Solver(scriptPubKey, vSolutions);

    if (whichType == TxoutType::NONSTANDARD) {
        return false;
    } else if (whichType == TxoutType::MULTISIG) {
        unsigned char m = vSolutions.front()[0];
        unsigned char n = vSolutions.back()[0];
        // Support up to x-of-3 multisig txns as standard
        if (n < 1 || n > 3)
            return false;
        if (m < 1 || m > n)
            return false;
    }

    return true;
}

bool IsStandardTx(const CTransaction& tx, const std::optional<unsigned>& max_datacarrier_bytes, bool permit_bare_multisig, const CFeeRate& dust_relay_fee, std::string& reason)
{
    (void)max_datacarrier_bytes;
    (void)permit_bare_multisig;
    if (tx.version > TX_MAX_STANDARD_VERSION || tx.version < TX_MIN_STANDARD_VERSION) {
        reason = "version";
        return false;
    }

    // Extremely large transactions with lots of inputs can cost the network
    // almost as much to process as they cost the sender in fees, because
    // computing signature hashes is O(ninputs*txsize). Limiting transactions
    // to MAX_STANDARD_TX_WEIGHT mitigates CPU exhaustion attacks.
    unsigned int sz = GetTransactionWeight(tx);
    if (sz > MAX_STANDARD_TX_WEIGHT) {
        reason = "tx-size";
        return false;
    }

    if (!tx.IsCoinBase()) {
        for (const CTxIn& txin : tx.vin) {
            if (!txin.scriptSig.empty()) {
                reason = "scriptsig-nonempty";
                return false;
            }
        }
    }

    for (const CTxOut& txout : tx.vout) {
        const bool valid_p2pk{txout.GetType() == TxOutputType::P2PK && txout.GetP2PKPubKey().has_value()};
        if (!valid_p2pk && !IsCanonicalP2COutput(txout)) {
            reason = "output-type";
            return false;
        }
    }

    // Only MAX_DUST_OUTPUTS_PER_TX dust is permitted(on otherwise valid ephemeral dust)
    if (GetDust(tx, dust_relay_fee).size() > MAX_DUST_OUTPUTS_PER_TX) {
        reason = "dust";
        return false;
    }

    return true;
}

TxValidationState ValidateInputsStandardness(const CTransaction& tx, const CCoinsViewCache& mapInputs)
{
    TxValidationState state;
    if (tx.IsCoinBase()) {
        return state; // Coinbases don't use vin normally
    }

    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        const CTxOut& prev = mapInputs.AccessCoin(tx.vin[i].prevout).out;
        const CTxIn& input{tx.vin[i]};
        const bool valid_p2pk{prev.GetType() == TxOutputType::P2PK && prev.GetP2PKPubKey().has_value() &&
                              input.scriptWitness.stack.size() == 1 && input.scriptWitness.stack.front().size() == 64};
        const bool valid_p2c{IsCanonicalP2COutput(prev) && input.scriptWitness.stack.size() == 1 &&
                             !input.scriptWitness.stack.front().empty() &&
                             input.scriptWitness.stack.front().size() <= MAX_P2C_PROOF_SIZE};
        if (!input.scriptSig.empty() || (!valid_p2pk && !valid_p2c)) {
            state.Invalid(TxValidationResult::TX_INPUTS_NOT_STANDARD, "bad-txns-nonstandard-inputs",
                          strprintf("input %u is not a canonical typed-output spend", i));
            return state;
        }
    }

    return state;
}

bool IsWitnessStandard(const CTransaction& tx, const CCoinsViewCache& mapInputs)
{
    if (tx.IsCoinBase())
        return true; // Coinbases are skipped

    for (unsigned int i = 0; i < tx.vin.size(); ++i) {
        const CTxOut& prev{mapInputs.AccessCoin(tx.vin[i].prevout).out};
        const auto& witness{tx.vin[i].scriptWitness.stack};
        const bool valid_p2pk{prev.GetType() == TxOutputType::P2PK && prev.GetP2PKPubKey().has_value() &&
                              witness.size() == 1 && witness.front().size() == 64};
        const bool valid_p2c{IsCanonicalP2COutput(prev) && witness.size() == 1 && !witness.front().empty() &&
                             witness.front().size() <= MAX_P2C_PROOF_SIZE};
        if (!valid_p2pk && !valid_p2c) return false;
    }
    return true;
}

bool SpendsNonAnchorWitnessProg(const CTransaction& tx, const CCoinsViewCache& prevouts)
{
    if (tx.IsCoinBase()) {
        return false;
    }

    for (const auto& txin: tx.vin) {
        const CTxOut& prev{prevouts.AccessCoin(txin.prevout).out};
        if (prev.GetType() == TxOutputType::P2PK || prev.GetType() == TxOutputType::PAY_TO_CONNECT) return true;
    }

    return false;
}

int64_t GetSigOpsAdjustedWeight(int64_t weight, int64_t sigop_cost, unsigned int bytes_per_sigop)
{
    return std::max(weight, sigop_cost * bytes_per_sigop);
}

int64_t GetVirtualTransactionSize(int64_t nWeight, int64_t nSigOpCost, unsigned int bytes_per_sigop)
{
    return (GetSigOpsAdjustedWeight(nWeight, nSigOpCost, bytes_per_sigop) + WITNESS_SCALE_FACTOR - 1) / WITNESS_SCALE_FACTOR;
}

int64_t GetVirtualTransactionSize(const CTransaction& tx, int64_t nSigOpCost, unsigned int bytes_per_sigop)
{
    return GetVirtualTransactionSize(GetTransactionWeight(tx), nSigOpCost, bytes_per_sigop);
}

int64_t GetVirtualTransactionInputSize(const CTxIn& txin, int64_t nSigOpCost, unsigned int bytes_per_sigop)
{
    return GetVirtualTransactionSize(GetTransactionInputWeight(txin), nSigOpCost, bytes_per_sigop);
}
