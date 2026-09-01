// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>
#include <key.h>
#include <script/solver.h>
#include <test/util/script.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/spend.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>

#include <algorithm>
#include <boost/test/unit_test.hpp>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(spend_tests, WalletTestingSetup)

static CTxDestination P2PKDestination(const CKey& key)
{
    return WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}};
}

BOOST_AUTO_TEST_CASE(max_signed_input_size_uses_external_outpoint)
{
    const CKey key{GenerateRandomKey()};
    FillableSigningProvider provider;
    BOOST_REQUIRE(provider.AddKey(key));

    const CTxOut txout{COIN, XOnlyPubKey{key.GetPubKey()}};
    const COutPoint outpoint{Txid{}, 0};
    CCoinControl coin_control;
    coin_control.Select(outpoint).SetTxOut(txout);

    const int low_r{CalculateMaximumSignedInputSize(txout, COutPoint{}, &provider, /*can_grind_r=*/true, &coin_control)};
    const int high_r{CalculateMaximumSignedInputSize(txout, outpoint, &provider, /*can_grind_r=*/true, &coin_control)};
    BOOST_CHECK_EQUAL(low_r, 58);
    BOOST_CHECK_EQUAL(high_r, low_r);
}

BOOST_AUTO_TEST_CASE(rejects_amounts_above_money_range)
{
    const CTxDestination destination{P2PKDestination(GenerateRandomKey())};
    CCoinControl coin_control;

    // Each recipient is individually valid, but their sum is not.
    auto result{CreateTransaction(m_wallet,
                                  {{destination, MAX_MONEY, /*subtract_fee=*/false},
                                   {destination, COIN, /*subtract_fee=*/false}},
                                  /*change_pos=*/std::nullopt, coin_control)};
    BOOST_REQUIRE(!result);
    BOOST_CHECK_EQUAL(util::ErrorString(result).original, "Transaction too large");

    // The recipient amount fits exactly, but adding a non-zero fee does not.
    coin_control.m_feerate.emplace(COIN);
    coin_control.fOverrideFeeRate = true;
    coin_control.destChange = destination;
    auto result_with_fee{CreateTransaction(m_wallet,
                                           {{destination, MAX_MONEY, /*subtract_fee=*/false}},
                                           /*change_pos=*/std::nullopt, coin_control)};
    BOOST_REQUIRE(!result_with_fee);
    BOOST_CHECK_EQUAL(util::ErrorString(result_with_fee).original, "Transaction too large");
}

BOOST_AUTO_TEST_CASE(rejects_invalid_typed_destination)
{
    const CRecipient recipient{WitnessV1Taproot{XOnlyPubKey{}}, COIN, /*subtract_fee=*/false};
    CCoinControl coin_control;

    const auto result{CreateTransaction(m_wallet, {recipient}, /*change_pos=*/std::nullopt, coin_control)};
    BOOST_REQUIRE(!result);
    BOOST_CHECK_EQUAL(util::ErrorString(result).original,
                      "ConnectCoin transactions require valid type-1 destinations or type-2 PAY_TO_CONNECT outputs");
}

BOOST_FIXTURE_TEST_CASE(SubtractFee, TestChain100Setup)
{
    CreateAndProcessBlock({}, GetScriptForP2PKOutput(coinbaseKey));
    auto wallet = CreateSyncedWallet(*m_node.chain, WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()), coinbaseKey);

    const CKey recipient_key{GenerateRandomKey()};
    const XOnlyPubKey recipient_pubkey{recipient_key.GetPubKey()};
    const CRecipient recipient{P2PKDestination(recipient_key), 100 * COIN, /*subtract_fee=*/true};
    CCoinControl coin_control;
    coin_control.m_feerate.emplace(10000);
    coin_control.fOverrideFeeRate = true;

    const auto result{CreateTransaction(*wallet, {recipient}, /*change_pos=*/std::nullopt, coin_control)};
    BOOST_REQUIRE(result);
    BOOST_CHECK_GT(result->fee, 0);

    const auto recipient_output{std::find_if(result->tx->vout.begin(), result->tx->vout.end(),
        [&](const CTxOut& out) { return out.GetP2PKPubKey() == recipient_pubkey; })};
    BOOST_REQUIRE(recipient_output != result->tx->vout.end());
    BOOST_CHECK_EQUAL(recipient_output->nValue, recipient.nAmount - result->fee);
    for (const CTxOut& output : result->tx->vout) {
        BOOST_CHECK(output.GetType() == TxOutputType::P2PK);
    }
}

BOOST_FIXTURE_TEST_CASE(wallet_duplicated_preset_inputs_test, TestChain100Setup)
{
    // Verify that the wallet's Coin Selection process does not include pre-selected inputs twice in a transaction.

    // Add 4 spendable UTXOs, 100 CC each, to the wallet (total balance 400 CC).
    for (int i = 0; i < 4; i++) CreateAndProcessBlock({}, GetScriptForP2PKOutput(coinbaseKey));
    auto wallet = CreateSyncedWallet(*m_node.chain, WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()), coinbaseKey);

    LOCK(wallet->cs_wallet);
    auto available_coins = AvailableCoins(*wallet);
    std::vector<COutput> coins = available_coins.All();
    // Preselect the first 3 UTXOs (300 CC total).
    std::set<COutPoint> preset_inputs = {coins[0].outpoint, coins[1].outpoint, coins[2].outpoint};

    // Try to create a tx that spends more than what preset inputs + wallet selected inputs are covering for.
    // The wallet can cover up to 400 CC, and the tx target is 499 CC.
    std::vector<CRecipient> recipients{{*Assert(wallet->GetNewDestination(OutputType::BECH32M, "dummy")),
                                           /*nAmount=*/499 * COIN, /*fSubtractFeeFromAmount=*/true}};
    CCoinControl coin_control;
    coin_control.m_allow_other_inputs = true;
    for (const auto& outpoint : preset_inputs) {
        coin_control.Select(outpoint);
    }

    // Attempt to send 499 CC from a wallet that only has 400 CC. The wallet should exclude
    // the preset inputs from the pool of available coins, realize that there is not enough
    // money to fund the 499 CC payment, and fail with "Insufficient funds".
    //
    // Even with SFFO, the wallet can only afford to send 400 CC.
    // If the wallet does not properly exclude preset inputs from the pool of available coins
    // prior to coin selection, it may create a transaction that does not fund the full payment
    // amount or, through SFFO, incorrectly reduce the recipient's amount by the difference
    // between the original target and the wrongly counted inputs (in this case 99 CC)
    // so that the recipient's amount is no longer equal to the user's selected target of 499 CC.

    // First case, use 'subtract_fee_from_outputs=true'
    BOOST_CHECK(!CreateTransaction(*wallet, recipients, /*change_pos=*/std::nullopt, coin_control));

    // Second case, don't use 'subtract_fee_from_outputs'.
    recipients[0].fSubtractFeeFromAmount = false;
    BOOST_CHECK(!CreateTransaction(*wallet, recipients, /*change_pos=*/std::nullopt, coin_control));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
