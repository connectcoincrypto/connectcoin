// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/validation.h>
#include <core_memusage.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <test/fuzz/fuzz.h>

FUZZ_TARGET(tx_out)
{
    CTxOut tx_out;
    try {
        SpanReader{buffer} >> tx_out;
    } catch (const std::ios_base::failure&) {
        return;
    }

    // Type 1 must imply a curve-valid stored public key, including after copies
    // and explicit replacement. Getter calls intentionally do not reparse it.
    if (const auto pubkey{tx_out.GetP2PKPubKey()}) {
        assert(pubkey->IsFullyValid());
        CTxOut changed{tx_out};
        changed.scriptPubKey.back() ^= 1;
        assert(!changed.GetP2PKPubKey());
        changed.scriptPubKey = tx_out.scriptPubKey;
        assert(changed.GetP2PKPubKey() == pubkey);
    } else {
        assert(tx_out.GetType() != TxOutputType::P2PK);
    }
    if (buffer.size() >= XOnlyPubKey::size()) {
        const XOnlyPubKey candidate{buffer.last(XOnlyPubKey::size())};
        CTxOut changed{tx_out};
        try {
            changed.SetP2PK(candidate);
            assert(candidate.IsFullyValid());
            assert(changed.GetP2PKPubKey() == candidate);
        } catch (const std::ios_base::failure&) {
            assert(!candidate.IsFullyValid());
            assert(changed == tx_out);
        }
    }

    const CFeeRate dust_relay_fee{DUST_RELAY_TX_FEE};
    (void)GetDustThreshold(tx_out, dust_relay_fee);
    (void)IsDust(tx_out, dust_relay_fee);
    (void)RecursiveDynamicUsage(tx_out);

    (void)tx_out.ToString();
    (void)tx_out.IsNull();
    tx_out.SetNull();
    assert(tx_out.IsNull());
}
