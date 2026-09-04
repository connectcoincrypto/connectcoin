// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! @file node/mining_types.h is used externally by mining IPC clients, so it should
//! only declare simple data definitions.
//!
//! Avoid declaring functions or classes with methods here unless they are
//! header-only or provided by the util library.

#ifndef CONNECTCOIN_NODE_MINING_TYPES_H
#define CONNECTCOIN_NODE_MINING_TYPES_H

#include <consensus/amount.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>
#include <util/time.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace node {

/**
 * Block template creation options. These override node defaults, but can't
 * exceed node limits (e.g. block_reserved_weight can't exceed max block weight).
 */
struct BlockCreateOptions {
    /**
     * Set false to omit mempool transactions
     */
    bool use_mempool{true};
    /**
     * Minimum fee rate for transactions to be included. Providing a value
     * overrides the -blockmintxfee startup setting.
     */
    std::optional<CFeeRate> block_min_fee_rate{};
    /**
     * Whether to log the fee rate of each transaction when it is added to the
     * block template. Providing a value overrides the -printpriority startup
     * setting.
     */
    std::optional<bool> print_modified_fee{};
    /**
     * The default reserved weight for the fixed-size block header,
     * transaction count and coinbase transaction. Minimum: 2000 weight units
     * (MINIMUM_BLOCK_RESERVED_WEIGHT).
     *
     * Providing a value overrides the `-blockreservedweight` startup setting.
     * Cap'n Proto IPC clients currently cannot leave this field unset, so they
     * always provide a value.
     */
    std::optional<uint64_t> block_reserved_weight{};
    /**
     * Maximum block weight, defaults to -maxblockweight
     *
     * Must not be lower than block_reserved_weight. Setting this equal to
     * block_reserved_weight leaves no room for non-coinbase transactions.
     */
    std::optional<uint64_t> block_max_weight{};
    /**
     * The maximum additional sigops which the pool will add in coinbase
     * transaction outputs.
     */
    size_t coinbase_output_max_additional_sigops{DEFAULT_COINBASE_OUTPUT_MAX_ADDITIONAL_SIGOPS};
    /**
     * Compatibility script view of the type-1 coinbase payout key. The
     * default is the x-only public key for private key 1 and is intended only
     * as a deterministic test fallback.
     *
     * Should only be used for tests, when the default doesn't suffice.
     *
     * Note that higher level code like the getblocktemplate RPC may omit the
     * coinbase transaction entirely. It's instead constructed by pool software
     * using fields like coinbasevalue, coinbaseaux and default_witness_commitment.
     * This software typically also controls the payout outputs, even for solo
     * mining.
     *
     * The size and sigops are not checked against
     * coinbase_max_additional_weight and coinbase_output_max_additional_sigops.
     */
    CScript coinbase_output_script{CScript{} << OP_1 << std::vector<unsigned char>{
        0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac,
        0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b, 0x07,
        0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9,
        0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98,
    }};
    /**
     * Whether to call TestBlockValidity() at the end of CreateNewBlock().
     * Should only be disabled for tests / benchmarks.
     */
    bool test_block_validity{true};
};

struct BlockWaitOptions {
    /**
     * How long to wait before returning nullptr instead of a new template.
     * Default is to wait forever.
     */
    MillisecondsDouble timeout{MillisecondsDouble::max()};

    /**
     * The wait method will not return a new template unless it has fees at
     * least fee_threshold connects higher than the current template, or unless
     * the chain tip changes and the previous template is no longer valid.
     *
     * A caller may not be interested in templates with higher fees, and
     * determining whether fee_threshold is reached is also expensive. So as
     * an optimization, when fee_threshold is set to MAX_MONEY (default), the
     * implementation is able to be much more efficient, skipping expensive
     * checks and only returning new templates when the chain tip changes.
     */
    CAmount fee_threshold{MAX_MONEY};
};

struct BlockCheckOptions {
    /**
     * Set false to omit the merkle root check
     */
    bool check_merkle_root{true};

    /**
     * Set false to omit the proof-of-work check
     */
    bool check_pow{true};
};

/**
 * Template containing all coinbase transaction fields that are set by our
 * miner code. Clients are expected to add their own outputs and typically
 * also expand the scriptSig.
 */
struct CoinbaseTx {
    /* nVersion */
    uint32_t version;
    /* nSequence for the only coinbase transaction input */
    uint32_t sequence;
    /**
     * Prefix which needs to be placed at the beginning of the scriptSig.
     * Clients may append extra data to this as long as the overall scriptSig
     * size is 100 bytes or less, to avoid the block being rejected with
     * "bad-cb-length" error. The prefix includes the BIP34 height and
     * ConnectCoin's required witness commitment metadata. Clients must
     * preserve it byte-for-byte and append their extraNonce after it.
     *
     * The remaining space available to clients is 100 minus prefix.size().
     */
    CScript script_sig_prefix;
    /**
     * The first (and only) witness stack element of the coinbase input.
     *
     * Omitted for block templates without witness data.
     *
     * This is currently the BIP 141 witness reserved value, and can be chosen
     * arbitrarily by the node, but future soft forks may constrain it.
     */
    std::optional<uint256> witness;
    /**
     * Weight-adjusted block subsidy plus fees, minus any non-zero required_outputs.
     *
     * Currently there are no non-zero required_outputs, so block_reward_remaining
     * is the entire block reward. See also required_outputs.
     */
    CAmount block_reward_remaining;
    /*
     * To be included as the last outputs in the coinbase transaction. This is
     * currently empty because ConnectCoin's witness commitment is part of the
     * script_sig_prefix, but future soft forks or custom mining patches could
     * add required typed outputs.
     *
     * The dummy output that spends the full reward is excluded.
     */
    std::vector<CTxOut> required_outputs;
    uint32_t lock_time;
};

} // namespace node

#endif // CONNECTCOIN_NODE_MINING_TYPES_H
