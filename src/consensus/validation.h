// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_CONSENSUS_VALIDATION_H
#define CONNECTCOIN_CONSENSUS_VALIDATION_H

#include <consensus/consensus.h>
#include <prevector.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <vector>


/** Index marker for when no witness commitment is present in a coinbase transaction. */
inline constexpr int NO_WITNESS_COMMITMENT{-1};

/** Witness commitment marker and payload size in coinbase input metadata. */
inline constexpr uint8_t WITNESS_COMMITMENT_HEADER[4]{0xaa, 0x21, 0xa9, 0xed};
inline constexpr size_t MINIMUM_WITNESS_COMMITMENT{36};

/** A "reason" why a transaction was invalid, suitable for determining whether the
  * provider of the transaction should be banned/ignored/disconnected/etc.
  */
enum class TxValidationResult {
    TX_RESULT_UNSET = 0,     //!< initial value. Tx has not yet been rejected
    TX_CONSENSUS,            //!< invalid by consensus rules
    TX_INPUTS_NOT_STANDARD,   //!< inputs (covered by txid) failed policy rules
    TX_NOT_STANDARD,          //!< otherwise didn't meet our local policy rules
    TX_MISSING_INPUTS,        //!< transaction was missing some of its inputs
    TX_PREMATURE_SPEND,       //!< transaction spends a coinbase too early, or violates locktime/sequence locks
    /**
     * Transaction might have a witness prior to SegWit
     * activation, or witness may have been malleated (which includes
     * non-standard witnesses).
     */
    TX_WITNESS_MUTATED,
    /**
     * Transaction is missing a witness.
     */
    TX_WITNESS_STRIPPED,
    /**
     * Tx already in mempool or conflicts with a tx in the chain
     * (if it conflicts with another tx in mempool, we use MEMPOOL_POLICY as it failed to reach the RBF threshold)
     * Currently this is only used if the transaction already exists in the mempool or on chain.
     */
    TX_CONFLICT,
    TX_MEMPOOL_POLICY,        //!< violated mempool's fee/size/descendant/RBF/etc limits
    TX_NO_MEMPOOL,            //!< this node does not have a mempool so can't validate the transaction
    TX_RECONSIDERABLE,        //!< fails some policy, but might be acceptable if submitted in a (different) package
    TX_UNKNOWN,               //!< transaction was not validated because package failed
};

/** A "reason" why a block was invalid, suitable for determining whether the
  * provider of the block should be banned/ignored/disconnected/etc.
  * These are much more granular than the rejection codes, which may be more
  * useful for some other use-cases.
  */
enum class BlockValidationResult {
    BLOCK_RESULT_UNSET = 0,  //!< initial value. Block has not yet been rejected
    BLOCK_CONSENSUS,         //!< invalid by consensus rules (excluding any below reasons)
    BLOCK_CACHED_INVALID,    //!< this block was cached as being invalid and we didn't store the reason why
    BLOCK_INVALID_HEADER,    //!< invalid proof of work or time too old
    BLOCK_MUTATED,           //!< the block's data didn't match the data committed to by the PoW
    BLOCK_MISSING_PREV,      //!< We don't have the previous block the checked one is built on
    BLOCK_INVALID_PREV,      //!< A block this one builds on is invalid
    BLOCK_TIME_FUTURE,       //!< block timestamp was > 2 hours in the future (or our clock is bad)
    BLOCK_HEADER_LOW_WORK    //!< the block header may be on a too-little-work chain
};



/** Template for capturing information about block/transaction validation. This is instantiated
 *  by TxValidationState and BlockValidationState for validation information on transactions
 *  and blocks respectively. */
template <typename Result>
class ValidationState
{
private:
    enum class ModeState {
        M_VALID,   //!< everything ok
        M_INVALID, //!< network rule violation (DoS value may be set)
        M_ERROR,   //!< run-time error
    } m_mode{ModeState::M_VALID};
    Result m_result{};
    std::string m_reject_reason;
    std::string m_debug_message;

public:
    bool Invalid(Result result,
                 const std::string& reject_reason = "",
                 const std::string& debug_message = "")
    {
        m_result = result;
        m_reject_reason = reject_reason;
        m_debug_message = debug_message;
        if (m_mode != ModeState::M_ERROR) m_mode = ModeState::M_INVALID;
        return false;
    }
    bool Error(const std::string& reject_reason)
    {
        if (m_mode == ModeState::M_VALID)
            m_reject_reason = reject_reason;
        m_mode = ModeState::M_ERROR;
        return false;
    }
    bool IsValid() const { return m_mode == ModeState::M_VALID; }
    bool IsInvalid() const { return m_mode == ModeState::M_INVALID; }
    bool IsError() const { return m_mode == ModeState::M_ERROR; }
    Result GetResult() const { return m_result; }
    std::string GetRejectReason() const { return m_reject_reason; }
    std::string GetDebugMessage() const { return m_debug_message; }
    std::string ToString() const
    {
        if (IsValid()) {
            return "Valid";
        }

        if (!m_debug_message.empty()) {
            return m_reject_reason + ", " + m_debug_message;
        }

        return m_reject_reason;
    }
};

class TxValidationState : public ValidationState<TxValidationResult> {};
class BlockValidationState : public ValidationState<BlockValidationResult> {};

// These implement the weight = (stripped_size * 4) + witness_size formula,
// using only serialization with and without witness data. As witness_size
// is equal to total_size - stripped_size, this formula is identical to:
// weight = (stripped_size * 3) + total_size.
static inline int32_t GetTransactionWeight(const CTransaction& tx)
{
    return ::GetSerializeSize(TX_NO_WITNESS(tx)) * (WITNESS_SCALE_FACTOR - 1) + ::GetSerializeSize(TX_WITH_WITNESS(tx));
}
static inline int64_t GetBlockWeight(const CBlock& block)
{
    return ::GetSerializeSize(TX_NO_WITNESS(block)) * (WITNESS_SCALE_FACTOR - 1) + ::GetSerializeSize(TX_WITH_WITNESS(block));
}
static inline int64_t GetTransactionInputWeight(const CTxIn& txin)
{
    // scriptWitness size is added here because witnesses and txins are split up in segwit serialization.
    return ::GetSerializeSize(TX_NO_WITNESS(txin)) * (WITNESS_SCALE_FACTOR - 1) + ::GetSerializeSize(TX_WITH_WITNESS(txin)) + ::GetSerializeSize(txin.scriptWitness.stack);
}

/** Return the byte offset of the witness commitment marker in coinbase input
 * metadata, or -1 when none is present. The last marker wins, mirroring the
 * historical witness-output behavior.
 */
inline int GetWitnessCommitmentOffset(const CBlock& block)
{
    int commitpos = NO_WITNESS_COMMITMENT;
    if (!block.vtx.empty() && !block.vtx[0]->vin.empty()) {
        const CScript& metadata{block.vtx[0]->vin[0].scriptSig};
        CScript::const_iterator pc{metadata.begin()};
        opcodetype opcode;
        std::vector<unsigned char> pushdata;
        while (pc != metadata.end()) {
            const CScript::const_iterator opcode_pos{pc};
            if (!metadata.GetOp(pc, opcode, pushdata)) {
                break;
            }
            if (opcode == static_cast<opcodetype>(MINIMUM_WITNESS_COMMITMENT) &&
                pushdata.size() == MINIMUM_WITNESS_COMMITMENT &&
                std::equal(std::begin(WITNESS_COMMITMENT_HEADER), std::end(WITNESS_COMMITMENT_HEADER), pushdata.begin())) {
                commitpos = static_cast<int>(opcode_pos - metadata.begin()) + 1;
            }
        }
    }
    return commitpos;
}

#endif // CONNECTCOIN_CONSENSUS_VALIDATION_H
