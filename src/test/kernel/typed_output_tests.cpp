// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <kernel/connectcoinkernel.h>
#include <kernel/connectcoinkernel_wrapper.h>

#include <key.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <streams.h>

// Boost.Test's SIGSTKSZ alternate stack can be smaller than Linux requires on musl.
#define BOOST_TEST_DISABLE_ALT_STACK
#define BOOST_TEST_MODULE ConnectCoin Typed Output Kernel Test Suite
#include <boost/test/included/unit_test.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace {

std::vector<std::byte> SerializeTransaction(const CMutableTransaction& tx)
{
    DataStream stream;
    stream << TX_WITH_WITNESS(CTransaction{tx});
    return {stream.begin(), stream.end()};
}

std::vector<std::byte> ScriptBytes(const CScript& script)
{
    const auto bytes{std::as_bytes(std::span{script})};
    return {bytes.begin(), bytes.end()};
}

} // namespace

BOOST_AUTO_TEST_CASE(kernel_type1_verification_matches_consensus)
{
    ECC_Context ecc_context;
    CKey spending_key;
    spending_key.MakeNewKey(/*fCompressed=*/true);
    CKey destination_key;
    destination_key.MakeNewKey(/*fCompressed=*/true);

    constexpr CAmount amount{50000};
    const CTxOut spent_output{amount, XOnlyPubKey{spending_key.GetPubKey()}};
    CMutableTransaction spend;
    spend.vin.emplace_back(Txid::FromUint256(uint256::ONE), 0);
    spend.vout.emplace_back(amount - 1000, XOnlyPubKey{destination_key.GetPubKey()});

    PrecomputedTransactionData signing_data;
    signing_data.Init(spend, {spent_output}, /*force=*/true);
    ScriptExecutionData execdata;
    execdata.m_annex_init = true;
    execdata.m_annex_present = false;
    uint256 sighash;
    BOOST_REQUIRE(SignatureHashSchnorr(sighash, execdata, spend, /*in_pos=*/0, SIGHASH_DEFAULT,
                                       SigVersion::TAPROOT, signing_data, MissingDataBehavior::ASSERT_FAIL));
    std::array<unsigned char, 64> signature;
    BOOST_REQUIRE(spending_key.SignSchnorr(sighash, signature, /*merkle_root=*/nullptr, uint256{}));
    spend.vin[0].scriptWitness.stack.emplace_back(signature.begin(), signature.end());

    cck::Transaction kernel_tx{SerializeTransaction(spend)};
    cck::ScriptPubkey kernel_script{ScriptBytes(spent_output.scriptPubKey)};
    std::vector<cck::TransactionOutput> kernel_spent_outputs;
    kernel_spent_outputs.emplace_back(kernel_script, amount);
    cck::PrecomputedTransactionData kernel_txdata{kernel_tx, kernel_spent_outputs};
    auto status{cck::ScriptVerifyStatus::OK};

    BOOST_CHECK(kernel_script.Verify(amount, kernel_tx, &kernel_txdata, 0,
                                     cck::ScriptVerificationFlags::ALL, status));
    BOOST_CHECK(status == cck::ScriptVerifyStatus::OK);
    BOOST_CHECK(!kernel_script.Verify(amount, kernel_tx, nullptr, 0,
                                      cck::ScriptVerificationFlags::ALL, status));
    BOOST_CHECK(status == cck::ScriptVerifyStatus::ERROR_SPENT_OUTPUTS_REQUIRED);
    BOOST_CHECK(!kernel_script.Verify(amount - 1, kernel_tx, &kernel_txdata, 0,
                                      cck::ScriptVerificationFlags::ALL, status));

    cck::ScriptPubkey p2pkh_script{ScriptBytes(CScript{} << OP_DUP << OP_HASH160 <<
                                               std::vector<unsigned char>(20) << OP_EQUALVERIFY << OP_CHECKSIG)};
    BOOST_CHECK(!p2pkh_script.Verify(amount, kernel_tx, &kernel_txdata, 0,
                                     cck::ScriptVerificationFlags::ALL, status));
    cck_TransactionOutput* invalid_output{cck_transaction_output_create(p2pkh_script.get(), amount)};
    BOOST_CHECK(invalid_output == nullptr);

    CMutableTransaction sighash_byte{spend};
    sighash_byte.vin[0].scriptWitness.stack.front().push_back(SIGHASH_ALL);
    cck::Transaction sighash_byte_tx{SerializeTransaction(sighash_byte)};
    cck::PrecomputedTransactionData sighash_byte_data{sighash_byte_tx, kernel_spent_outputs};
    BOOST_CHECK(!kernel_script.Verify(amount, sighash_byte_tx, &sighash_byte_data, 0,
                                      cck::ScriptVerificationFlags::ALL, status));

    CMutableTransaction annex{spend};
    annex.vin[0].scriptWitness.stack.emplace_back(1, 0x50);
    cck::Transaction annex_tx{SerializeTransaction(annex)};
    cck::PrecomputedTransactionData annex_data{annex_tx, kernel_spent_outputs};
    BOOST_CHECK(!kernel_script.Verify(amount, annex_tx, &annex_data, 0,
                                      cck::ScriptVerificationFlags::ALL, status));

    CMutableTransaction script_path{spend};
    script_path.vin[0].scriptWitness.stack.emplace_back(1, OP_TRUE);
    script_path.vin[0].scriptWitness.stack.emplace_back(33, 0xc0);
    cck::Transaction script_path_tx{SerializeTransaction(script_path)};
    cck::PrecomputedTransactionData script_path_data{script_path_tx, kernel_spent_outputs};
    BOOST_CHECK(!kernel_script.Verify(amount, script_path_tx, &script_path_data, 0,
                                      cck::ScriptVerificationFlags::ALL, status));

    CMutableTransaction script_sig{spend};
    script_sig.vin[0].scriptSig = CScript{} << OP_0;
    cck::Transaction script_sig_tx{SerializeTransaction(script_sig)};
    cck::PrecomputedTransactionData script_sig_data{script_sig_tx, kernel_spent_outputs};
    BOOST_CHECK(!kernel_script.Verify(amount, script_sig_tx, &script_sig_data, 0,
                                      cck::ScriptVerificationFlags::ALL, status));

    CMutableTransaction invalid_signature{spend};
    invalid_signature.vin[0].scriptWitness.stack.front().assign(64, 0);
    cck::Transaction invalid_signature_tx{SerializeTransaction(invalid_signature)};
    cck::PrecomputedTransactionData invalid_signature_data{invalid_signature_tx, kernel_spent_outputs};
    BOOST_CHECK(!kernel_script.Verify(amount, invalid_signature_tx, &invalid_signature_data, 0,
                                      cck::ScriptVerificationFlags::ALL, status));
}

BOOST_AUTO_TEST_CASE(kernel_signet_rejects_nontrivial_script_challenges)
{
    constexpr uint8_t trivial_challenge{OP_TRUE};
    cck_ChainParameters* trivial{cck_chain_parameters_create_signet(&trivial_challenge, 1)};
    BOOST_REQUIRE(trivial != nullptr);
    cck_chain_parameters_destroy(trivial);

    std::array<uint8_t, 34> p2tr_challenge{};
    p2tr_challenge[0] = OP_1;
    p2tr_challenge[1] = 32;
    BOOST_CHECK(cck_chain_parameters_create_signet(p2tr_challenge.data(), p2tr_challenge.size()) == nullptr);
}
