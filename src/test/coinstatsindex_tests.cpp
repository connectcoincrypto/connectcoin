// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <index/coinstatsindex.h>
#include <interfaces/chain.h>
#include <kernel/coinstats.h>
#include <key.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

BOOST_AUTO_TEST_SUITE(coinstatsindex_tests)

BOOST_FIXTURE_TEST_CASE(coinstatsindex_counts_spendable_mainnet_genesis, TestingSetup)
{
    CoinStatsIndex coin_stats_index{interfaces::MakeChain(m_node), 1_MiB, true};
    BOOST_REQUIRE(coin_stats_index.Init());
    coin_stats_index.Sync();

    const CBlockIndex* genesis_block_index;
    {
        LOCK(cs_main);
        genesis_block_index = Assert(m_node.chainman)->ActiveChain().Genesis();
    }

    const auto stats{coin_stats_index.LookUpStats(*Assert(genesis_block_index))};
    BOOST_REQUIRE(stats);
    BOOST_REQUIRE(stats->total_amount);
    BOOST_CHECK_EQUAL(*stats->total_amount, Params().GenesisBlock().vtx.front()->vout.front().nValue);
    BOOST_CHECK_EQUAL(stats->nTransactionOutputs, 1U);
    BOOST_CHECK_EQUAL(stats->total_unspendables_genesis_block, 0);

    coin_stats_index.Stop();
}

BOOST_FIXTURE_TEST_CASE(coinstatsindex_initial_sync, TestChain100Setup)
{
    CoinStatsIndex coin_stats_index{interfaces::MakeChain(m_node), 1_MiB, true};
    BOOST_REQUIRE(coin_stats_index.Init());

    const CBlockIndex* block_index;
    {
        LOCK(cs_main);
        block_index = m_node.chainman->ActiveChain().Tip();
    }

    // CoinStatsIndex should not be found before it is started.
    BOOST_CHECK(!coin_stats_index.LookUpStats(*block_index));

    // BlockUntilSyncedToCurrentChain should return false before CoinStatsIndex
    // is started.
    BOOST_CHECK(!coin_stats_index.BlockUntilSyncedToCurrentChain());

    coin_stats_index.Sync();

    // Check that CoinStatsIndex works for genesis block.
    const CBlockIndex* genesis_block_index;
    {
        LOCK(cs_main);
        genesis_block_index = m_node.chainman->ActiveChain().Genesis();
    }
    const auto genesis_stats{coin_stats_index.LookUpStats(*genesis_block_index)};
    BOOST_REQUIRE(genesis_stats);
    BOOST_REQUIRE(genesis_stats->total_amount);
    BOOST_CHECK_EQUAL(*genesis_stats->total_amount, Params().GenesisBlock().vtx.front()->vout.front().nValue);
    BOOST_CHECK_EQUAL(genesis_stats->nTransactionOutputs, 1U);
    BOOST_CHECK_EQUAL(genesis_stats->total_unspendables_genesis_block, 0);

    // Check that CoinStatsIndex updates with new blocks.
    BOOST_CHECK(coin_stats_index.LookUpStats(*block_index));

    const CScript script_pub_key{CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG};
    std::vector<CMutableTransaction> noTxns;
    CreateAndProcessBlock(noTxns, script_pub_key);

    // Let the CoinStatsIndex to catch up again.
    BOOST_CHECK(coin_stats_index.BlockUntilSyncedToCurrentChain());

    const CBlockIndex* new_block_index;
    {
        LOCK(cs_main);
        new_block_index = m_node.chainman->ActiveChain().Tip();
    }
    BOOST_CHECK(coin_stats_index.LookUpStats(*new_block_index));

    BOOST_CHECK(block_index != new_block_index);

    // Shutdown sequence (c.f. Shutdown() in init.cpp)
    coin_stats_index.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
