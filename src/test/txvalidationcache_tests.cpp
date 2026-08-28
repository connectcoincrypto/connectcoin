// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <coins.h>
#include <consensus/validation.h>
#include <key.h>
#include <key_io.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <vector>

#include <boost/test/unit_test.hpp>

bool CheckInputScripts(const CTransaction& tx, TxValidationState& state,
                       const CCoinsViewCache& inputs, script_verify_flags flags, bool cacheSigStore,
                       bool cacheFullScriptStore, PrecomputedTransactionData& txdata,
                       ValidationCache& validation_cache,
                       std::vector<CScriptCheck>* pvChecks) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

BOOST_AUTO_TEST_SUITE(txvalidationcache_tests)

BOOST_FIXTURE_TEST_CASE(typed_execution_cache_hit_and_miss, TestChain100Setup)
{
    CKey destination_key;
    destination_key.MakeNewKey(/*fCompressedIn=*/true);
    const CTransactionRef& funding{m_coinbase_txns[0]};
    const COutPoint funding_out{funding->GetHash(), 0};
    const CTxOut output{funding->vout[0].nValue - 1000, XOnlyPubKey{destination_key.GetPubKey()}};
    const CTransaction tx{CreateValidMempoolTransaction(
        /*input_transactions=*/{funding}, /*inputs=*/{funding_out}, /*input_height=*/1,
        /*input_signing_keys=*/{coinbaseKey}, /*outputs=*/{output}, /*submit=*/false)};

    LOCK(cs_main);
    CCoinsViewCache& coins{m_node.chainman->ActiveChainstate().CoinsTip()};
    ValidationCache& cache{m_node.chainman->m_validation_cache};
    constexpr script_verify_flags flags{SCRIPT_VERIFY_NONE};

    TxValidationState state;
    PrecomputedTransactionData first_data;
    BOOST_REQUIRE(CheckInputScripts(tx, state, coins, flags, /*cacheSigStore=*/true,
                                    /*cacheFullScriptStore=*/true, first_data, cache, nullptr));

    // The same wtxid and flags must hit the whole-transaction cache, so no
    // deferred signature check is produced.
    PrecomputedTransactionData hit_data;
    std::vector<CScriptCheck> checks;
    BOOST_REQUIRE(CheckInputScripts(tx, state, coins, flags, /*cacheSigStore=*/true,
                                    /*cacheFullScriptStore=*/true, hit_data, cache, &checks));
    BOOST_CHECK(checks.empty());

    // Script flags remain part of the cache key even though type-1 validation
    // itself is independent of the retired Script flag matrix.
    PrecomputedTransactionData other_flags_data;
    std::vector<CScriptCheck> other_flags_checks;
    BOOST_REQUIRE(CheckInputScripts(tx, state, coins, SCRIPT_VERIFY_P2SH, /*cacheSigStore=*/true,
                                    /*cacheFullScriptStore=*/true, other_flags_data, cache, &other_flags_checks));
    BOOST_REQUIRE_EQUAL(other_flags_checks.size(), 1U);
    BOOST_CHECK(!other_flags_checks.front()().has_value());

    // The execution-cache key commits to the witness. A mutated Schnorr
    // signature cannot reuse the valid transaction's entry.
    CMutableTransaction tampered{tx};
    tampered.vin[0].scriptWitness.stack[0][0] ^= 1;
    const CTransaction tampered_tx{tampered};
    PrecomputedTransactionData tampered_data;
    std::vector<CScriptCheck> tampered_checks;
    BOOST_REQUIRE(CheckInputScripts(tampered_tx, state, coins, flags, /*cacheSigStore=*/true,
                                    /*cacheFullScriptStore=*/true, tampered_data, cache, &tampered_checks));
    BOOST_REQUIRE_EQUAL(tampered_checks.size(), 1U);
    BOOST_CHECK(tampered_checks.front()().has_value());
}

BOOST_FIXTURE_TEST_CASE(mempool_cache_is_consumed_when_block_connects, TestChain100Setup)
{
    CKey destination_key;
    destination_key.MakeNewKey(/*fCompressedIn=*/true);
    const CTransactionRef& funding{m_coinbase_txns[0]};
    const COutPoint funding_out{funding->GetHash(), 0};
    const CTxOut output{funding->vout[0].nValue - 1000, XOnlyPubKey{destination_key.GetPubKey()}};
    const CMutableTransaction spend{CreateValidMempoolTransaction(
        /*input_transactions=*/{funding}, /*inputs=*/{funding_out}, /*input_height=*/1,
        /*input_signing_keys=*/{coinbaseKey}, /*outputs=*/{output}, /*submit=*/true)};
    const CTransaction tx{spend};

    script_verify_flags flags;
    {
        LOCK(cs_main);
        flags = GetBlockScriptFlags(*m_node.chainman->ActiveChain().Tip(), *m_node.chainman);
        TxValidationState state;
        PrecomputedTransactionData hit_data;
        std::vector<CScriptCheck> checks;
        BOOST_REQUIRE(CheckInputScripts(tx, state, m_node.chainman->ActiveChainstate().CoinsTip(), flags,
                                        /*cacheSigStore=*/true, /*cacheFullScriptStore=*/true,
                                        hit_data, m_node.chainman->m_validation_cache, &checks));
        BOOST_CHECK(checks.empty());
    }

    const CScript coinbase_destination{GetScriptForDestination(WitnessV1Taproot{XOnlyPubKey{coinbaseKey.GetPubKey()}})};
    const CBlock block{CreateAndProcessBlock({spend}, coinbase_destination)};
    {
        LOCK(cs_main);
        BOOST_REQUIRE(m_node.chainman->ActiveChain().Tip()->GetBlockHash() == block.GetHash());

        // ConnectBlock consulted the mempool-populated cache and accepted the
        // transaction, then removed it from the mempool as confirmed.
        BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);
    }
}

BOOST_AUTO_TEST_SUITE_END()
