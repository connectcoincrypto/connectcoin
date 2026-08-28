// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/amount.h>
#include <consensus/tx_check.h>
#include <key.h>
#include <primitives/transaction.h>
#include <psbt.h>
#include <script/interpreter.h>
#include <script/sigcache.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(typed_output_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(p2pk_wire_format)
{
    CKey key;
    key.MakeNewKey(/*fCompressedIn=*/true);
    const XOnlyPubKey pubkey{key.GetPubKey()};
    const CTxOut output{42 * COIN, pubkey};

    DataStream encoded;
    encoded << output;

    // int64 amount + uint8 type + 32-byte x-only public key.
    BOOST_REQUIRE_EQUAL(encoded.size(), 41U);
    BOOST_CHECK_EQUAL(std::to_integer<uint8_t>(encoded[8]), static_cast<uint8_t>(TxOutputType::P2PK));
    BOOST_CHECK_EQUAL(std::memcmp(pubkey.data(), encoded.data() + 9, pubkey.size()), 0);

    CTxOut decoded;
    encoded >> decoded;
    BOOST_CHECK(encoded.empty());
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(decoded.GetType()), static_cast<uint8_t>(TxOutputType::P2PK));
    BOOST_CHECK(decoded.GetP2PKPubKey() == pubkey);
    BOOST_CHECK_EQUAL(decoded.nValue, output.nValue);
}

BOOST_AUTO_TEST_CASE(invalid_and_unknown_wire_types)
{
    CTxOut invalid{42, CScript{} << OP_TRUE};
    BOOST_CHECK(invalid.GetType() == TxOutputType::INVALID);

    DataStream invalid_encoded;
    invalid_encoded << invalid;
    // Invalid type 0 is amount + type only. The compatibility Script is never
    // serialized and consensus rejects the output after parsing it.
    BOOST_REQUIRE_EQUAL(invalid_encoded.size(), 9U);
    BOOST_CHECK_EQUAL(std::to_integer<uint8_t>(invalid_encoded[8]), 0U);

    CTxOut decoded_invalid;
    invalid_encoded >> decoded_invalid;
    BOOST_CHECK(decoded_invalid.GetType() == TxOutputType::INVALID);
    BOOST_CHECK(decoded_invalid.scriptPubKey.empty());

    DataStream unknown_encoded;
    unknown_encoded << CAmount{42} << uint8_t{2};
    CTxOut decoded_unknown;
    BOOST_CHECK_THROW(unknown_encoded >> decoded_unknown, std::ios_base::failure);
}

BOOST_AUTO_TEST_CASE(mutable_script_view_cannot_change_consensus_payload)
{
    CKey key;
    key.MakeNewKey(/*fCompressedIn=*/true);
    CTxOut output{42, XOnlyPubKey{key.GetPubKey()}};

    output.scriptPubKey = CScript{} << OP_TRUE;
    BOOST_CHECK(!output.GetP2PKPubKey());
    DataStream encoded;
    BOOST_CHECK_THROW(encoded << output, std::ios_base::failure);
}

BOOST_AUTO_TEST_CASE(only_p2pk_is_valid)
{
    CKey key;
    key.MakeNewKey(/*fCompressedIn=*/true);

    CMutableTransaction valid;
    valid.vin.emplace_back();
    valid.vin[0].prevout.SetNull();
    valid.vin[0].scriptSig = CScript{} << OP_0 << OP_0;
    valid.vout.emplace_back(COIN, XOnlyPubKey{key.GetPubKey()});

    TxValidationState state;
    BOOST_CHECK(CheckTransaction(CTransaction{valid}, state));

    CMutableTransaction invalid{valid};
    invalid.vout[0].SetScriptPubKey(CScript{} << OP_TRUE);
    TxValidationState invalid_state;
    BOOST_CHECK(!CheckTransaction(CTransaction{invalid}, invalid_state));
    BOOST_CHECK_EQUAL(invalid_state.GetRejectReason(), "bad-txns-vout-type");
}

BOOST_AUTO_TEST_CASE(direct_schnorr_authorization)
{
    CKey spending_key;
    spending_key.MakeNewKey(/*fCompressedIn=*/true);
    CKey destination_key;
    destination_key.MakeNewKey(/*fCompressedIn=*/true);

    const CTxOut prevout{5 * COIN, XOnlyPubKey{spending_key.GetPubKey()}};
    CMutableTransaction spend;
    spend.vin.emplace_back(Txid::FromUint256(uint256::ONE), 0);
    spend.vout.emplace_back(5 * COIN - 1000, XOnlyPubKey{destination_key.GetPubKey()});

    PrecomputedTransactionData signing_data;
    signing_data.Init(spend, {prevout}, /*force=*/true);
    ScriptExecutionData execdata;
    execdata.m_annex_init = true;
    execdata.m_annex_present = false;
    uint256 sighash;
    BOOST_REQUIRE(SignatureHashSchnorr(sighash, execdata, spend, /*in_pos=*/0, SIGHASH_DEFAULT,
                                       SigVersion::TAPROOT, signing_data, MissingDataBehavior::ASSERT_FAIL));

    std::array<unsigned char, 64> signature;
    BOOST_REQUIRE(spending_key.SignSchnorr(sighash, signature, /*merkle_root=*/nullptr, uint256{}));
    spend.vin[0].scriptWitness.stack.emplace_back(signature.begin(), signature.end());

    const CTransaction final_tx{spend};
    PrecomputedTransactionData checking_data;
    checking_data.Init(final_tx, {prevout}, /*force=*/true);
    SignatureCache signature_cache{1 << 20};
    CScriptCheck check{prevout, final_tx, signature_cache, /*nInIn=*/0, SCRIPT_VERIFY_NONE,
                       /*cacheIn=*/true, &checking_data};
    BOOST_CHECK(!check().has_value());

    spend.vin[0].scriptWitness.stack[0][0] ^= 1;
    const CTransaction tampered_tx{spend};
    PrecomputedTransactionData tampered_data;
    tampered_data.Init(tampered_tx, {prevout}, /*force=*/true);
    CScriptCheck tampered_check{prevout, tampered_tx, signature_cache, /*nInIn=*/0, SCRIPT_VERIFY_NONE,
                                /*cacheIn=*/true, &tampered_data};
    BOOST_CHECK(tampered_check().has_value());
}

BOOST_AUTO_TEST_CASE(psbt_requires_canonical_typed_outputs_and_spends)
{
    CKey spending_key;
    spending_key.MakeNewKey(/*fCompressedIn=*/true);
    CKey destination_key;
    destination_key.MakeNewKey(/*fCompressedIn=*/true);

    const CTxOut prevout{5 * COIN, XOnlyPubKey{spending_key.GetPubKey()}};
    CMutableTransaction spend;
    spend.vin.emplace_back(Txid::FromUint256(uint256::ONE), 0);
    spend.vout.emplace_back(5 * COIN - 1000, XOnlyPubKey{destination_key.GetPubKey()});

    PrecomputedTransactionData signing_data;
    signing_data.Init(spend, {prevout}, /*force=*/true);
    ScriptExecutionData execdata;
    execdata.m_annex_init = true;
    execdata.m_annex_present = false;
    uint256 sighash;
    BOOST_REQUIRE(SignatureHashSchnorr(sighash, execdata, spend, /*in_pos=*/0, SIGHASH_DEFAULT,
                                       SigVersion::TAPROOT, signing_data, MissingDataBehavior::ASSERT_FAIL));
    std::array<unsigned char, 64> signature;
    BOOST_REQUIRE(spending_key.SignSchnorr(sighash, signature, /*merkle_root=*/nullptr, uint256{}));

    PartiallySignedTransaction canonical{spend};
    canonical.inputs[0].witness_utxo = prevout;
    canonical.inputs[0].final_script_witness.stack.emplace_back(signature.begin(), signature.end());
    const auto psbt_data{PrecomputePSBTData(canonical)};
    BOOST_REQUIRE(psbt_data.has_value());
    BOOST_CHECK(PSBTHasValidTypedOutputs(canonical));
    BOOST_CHECK(PSBTInputSignedAndVerified(canonical, 0, &*psbt_data));
    CMutableTransaction extracted;
    BOOST_CHECK(FinalizeAndExtractPSBT(canonical, extracted));

    auto signature_with_sighash{canonical};
    signature_with_sighash.inputs[0].final_script_witness.stack[0].push_back(SIGHASH_ALL);
    BOOST_CHECK(!PSBTInputSignedAndVerified(signature_with_sighash, 0, &*psbt_data));
    BOOST_CHECK(!FinalizePSBT(signature_with_sighash));

    auto with_annex{canonical};
    with_annex.inputs[0].final_script_witness.stack.push_back({0x50});
    BOOST_CHECK(!PSBTInputSignedAndVerified(with_annex, 0, &*psbt_data));
    BOOST_CHECK(!FinalizePSBT(with_annex));

    auto with_script_path{canonical};
    with_script_path.inputs[0].final_script_witness.stack.push_back({OP_TRUE});
    with_script_path.inputs[0].final_script_witness.stack.emplace_back(33, 0);
    BOOST_CHECK(!PSBTInputSignedAndVerified(with_script_path, 0, &*psbt_data));

    auto with_script_sig{canonical};
    with_script_sig.inputs[0].final_script_sig = CScript{} << OP_0;
    BOOST_CHECK(!PSBTInputSignedAndVerified(with_script_sig, 0, &*psbt_data));

    auto with_declared_sighash{canonical};
    with_declared_sighash.inputs[0].sighash_type = SIGHASH_ALL;
    BOOST_CHECK(!PSBTInputSignedAndVerified(with_declared_sighash, 0, &*psbt_data));

    auto with_legacy_output{canonical};
    with_legacy_output.outputs[0].script = CScript{} << OP_TRUE;
    BOOST_CHECK(!PSBTHasValidTypedOutputs(with_legacy_output));
    BOOST_CHECK(!FinalizePSBT(with_legacy_output));

    auto with_legacy_prevout{canonical};
    with_legacy_prevout.inputs[0].witness_utxo.SetScriptPubKey(CScript{} << OP_TRUE);
    BOOST_CHECK(!PSBTInputSignedAndVerified(with_legacy_prevout, 0, nullptr));
}

BOOST_AUTO_TEST_CASE(independent_sighash_golden_and_mutations)
{
    std::array<unsigned char, 32> secret_one{};
    secret_one.back() = 1;
    std::array<unsigned char, 32> secret_two{};
    secret_two.back() = 2;
    CKey key_one;
    CKey key_two;
    key_one.Set(secret_one.begin(), secret_one.end(), /*fCompressedIn=*/true);
    key_two.Set(secret_two.begin(), secret_two.end(), /*fCompressedIn=*/true);

    CTxOut prevout{5 * COIN, XOnlyPubKey{key_one.GetPubKey()}};
    CMutableTransaction tx;
    tx.version = 2;
    tx.vin.emplace_back(Txid::FromUint256(uint256::ONE), 3, CScript{}, 0xfffffffe);
    tx.vout.emplace_back(5 * COIN - 1000, XOnlyPubKey{key_two.GetPubKey()});
    tx.nLockTime = 7;

    const auto compute_sighash = [](const CMutableTransaction& transaction, const CTxOut& spent) {
        PrecomputedTransactionData txdata;
        txdata.Init(transaction, {spent}, /*force=*/true);
        ScriptExecutionData execdata;
        execdata.m_annex_init = true;
        execdata.m_annex_present = false;
        uint256 hash;
        const bool success{SignatureHashSchnorr(hash, execdata, transaction, /*in_pos=*/0, SIGHASH_DEFAULT,
                                                SigVersion::TAPROOT, txdata, MissingDataBehavior::ASSERT_FAIL)};
        BOOST_CHECK(success);
        return hash;
    };

    const uint256 golden{compute_sighash(tx, prevout)};
    // Independently generated by test_framework.script.TaprootSignatureHash.
    BOOST_CHECK_EQUAL(HexStr(golden), "5338332fa1e0d391fe52735cd25683e480560b5e60797b08ee692850538fa68f");

    CMutableTransaction mutated{tx};
    mutated.vin[0].prevout.n++;
    BOOST_CHECK(compute_sighash(mutated, prevout) != golden);
    mutated = tx;
    mutated.vin[0].nSequence--;
    BOOST_CHECK(compute_sighash(mutated, prevout) != golden);
    mutated = tx;
    mutated.vout[0].nValue--;
    BOOST_CHECK(compute_sighash(mutated, prevout) != golden);
    mutated = tx;
    mutated.nLockTime++;
    BOOST_CHECK(compute_sighash(mutated, prevout) != golden);

    CTxOut mutated_prevout{prevout};
    mutated_prevout.nValue--;
    BOOST_CHECK(compute_sighash(tx, mutated_prevout) != golden);
    mutated_prevout = prevout;
    mutated_prevout.SetP2PK(XOnlyPubKey{key_two.GetPubKey()});
    BOOST_CHECK(compute_sighash(tx, mutated_prevout) != golden);
}

BOOST_AUTO_TEST_SUITE_END()
