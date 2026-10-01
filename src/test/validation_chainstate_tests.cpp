// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <kernel/coinstats.h>
#include <key.h>
#include <node/blockstorage.h>
#include <node/kernel_notifications.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <script/solver.h>
#include <sync.h>
#include <test/util/chainstate.h>
#include <test/util/coins.h>
#include <test/util/common.h>
#include <test/util/mining.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

class CTxMemPool;

class RegtestTestingSetup : public TestingSetup
{
public:
    RegtestTestingSetup() : TestingSetup{ChainType::REGTEST} {}
};

BOOST_FIXTURE_TEST_SUITE(validation_chainstate_tests, ChainTestingSetup)

BOOST_AUTO_TEST_CASE(mainnet_launch_is_separate_from_retired_test_fixture)
{
    const auto params{CreateChainParams(m_args, ChainType::MAIN)};
    BOOST_REQUIRE(params->HasGenesisBlock());
    BOOST_CHECK(!params->GetConsensus().hashGenesisBlock.IsNull());
    // Historical storage vectors remain test-only and must never overwrite
    // the mainnet factory's independently committed launch genesis.
    BOOST_REQUIRE(Params().HasGenesisBlock());
    BOOST_CHECK_EQUAL(Params().GetConsensus().hashGenesisBlock,
                      uint256{"8b6373205ad2b6314f2937cebacfc143af9eb6183162c24fb19cdf382ff576c5"});
    BOOST_CHECK(params->GetConsensus().hashGenesisBlock != Params().GetConsensus().hashGenesisBlock);
    const auto datadir{m_path_root / "launched-mainnet"};
    fs::create_directories(datadir / "blocks");
    const ChainstateManager::Options options{
        .chainparams = *params,
        .datadir = datadir,
        .notifications = *m_node.notifications,
    };
    const node::BlockManager::Options block_options{
        .chainparams = *params,
        .blocks_dir = datadir / "blocks",
        .notifications = *m_node.notifications,
        .block_tree_db_params = DBParams{.path = datadir / "blocks" / "index", .cache_bytes = 0},
    };
    BOOST_CHECK_NO_THROW((void)std::make_unique<ChainstateManager>(m_interrupt, options, block_options));
}

BOOST_AUTO_TEST_CASE(mainnet_genesis_has_two_independent_allocations)
{
    const auto params{CreateChainParams(m_args, ChainType::MAIN)};
    BOOST_REQUIRE(params->HasGenesisBlock());
    BOOST_CHECK(params->GetConsensus().genesis_coinbase_spendable);
    const CBlock& genesis{params->GenesisBlock()};
    BOOST_REQUIRE_EQUAL(genesis.vtx.size(), 1U);
    BOOST_REQUIRE_EQUAL(genesis.vtx.front()->vout.size(), 2U);
    BOOST_CHECK_EQUAL(genesis.nBits, 0x1e333300U);
    const std::vector<std::string> pubkeys{
        "29a6b41260ed25e3019d18236192afecec9ea0b7312837bacf81a3c40d7b2034",
        "2c83a568529b55cc93e2d20754f079f6072d77ac88266d9b054cb3e0e92a7845",
    };
    CAmount total{0};
    for (size_t i{0}; i < pubkeys.size(); ++i) {
        const auto& output{genesis.vtx.front()->vout[i]};
        BOOST_CHECK_EQUAL(output.nValue, 5'000'000 * COIN);
        BOOST_CHECK(output.GetType() == TxOutputType::P2PK);
        BOOST_REQUIRE(output.GetP2PKPubKey());
        BOOST_CHECK_EQUAL(HexStr(*output.GetP2PKPubKey()), pubkeys[i]);
        BOOST_CHECK_EQUAL(output.scriptPubKey.size(), 34U);
        total += output.nValue;
    }
    BOOST_CHECK_EQUAL(total, 10'000'000 * COIN);
    BOOST_CHECK(genesis.vtx.front()->vout[0].scriptPubKey != genesis.vtx.front()->vout[1].scriptPubKey);
}

BOOST_AUTO_TEST_CASE(mainnet_genesis_allocations_require_coinbase_maturity)
{
    const auto params{CreateChainParams(m_args, ChainType::MAIN)};
    const auto& genesis_tx{*params->GenesisBlock().vtx.front()};
    CCoinsViewCache inputs{&CoinsViewEmpty::Get()};
    AddCoins(inputs, genesis_tx, /*nHeight=*/0, /*check=*/false);
    BOOST_REQUIRE_EQUAL(genesis_tx.vout.size(), 2U);
    for (uint32_t index{0}; index < genesis_tx.vout.size(); ++index) {
        CMutableTransaction spend;
        spend.vin.emplace_back(COutPoint{genesis_tx.GetHash(), index});
        spend.vout.push_back(genesis_tx.vout[index]);
        spend.vout.front().nValue -= 1;
        CAmount fee{0};
        TxValidationState premature;
        BOOST_CHECK(!Consensus::CheckTxInputs(CTransaction{spend}, premature, inputs, COINBASE_MATURITY - 1, fee));
        BOOST_CHECK(premature.GetResult() == TxValidationResult::TX_PREMATURE_SPEND);
        BOOST_CHECK_EQUAL(premature.GetRejectReason(), "bad-txns-premature-spend-of-coinbase");
        // This checks value and maturity, not authorization: signing either
        // allocation still requires its owner's private key.
        TxValidationState mature;
        BOOST_CHECK(Consensus::CheckTxInputs(CTransaction{spend}, mature, inputs, COINBASE_MATURITY, fee));
        BOOST_CHECK_EQUAL(fee, 1);
    }
}

BOOST_AUTO_TEST_CASE(launched_networks_have_spendable_genesis_coinbase)
{
    const auto public_params{CreateChainParams(m_args, ChainType::TESTNET4)};
    const auto historical_params{CreateChainParams(m_args, ChainType::TESTNET)};
    const auto regtest_params{CreateChainParams(m_args, ChainType::REGTEST)};
    const CScript& public_script{public_params->GenesisBlock().vtx.front()->vout.front().scriptPubKey};
    const CScript& historical_script{historical_params->GenesisBlock().vtx.front()->vout.front().scriptPubKey};
    const CScript& regtest_script{regtest_params->GenesisBlock().vtx.front()->vout.front().scriptPubKey};

    // A known private key is intentionally available for regtest. Public
    // networks must never reuse its output script.
    BOOST_CHECK(public_script != regtest_script);
    BOOST_CHECK(public_script != historical_script);
    BOOST_CHECK(historical_script != regtest_script);
    BOOST_REQUIRE(public_params->GenesisBlock().vtx.front()->vout.front().GetP2PKPubKey());
    BOOST_CHECK_EQUAL(HexStr(*public_params->GenesisBlock().vtx.front()->vout.front().GetP2PKPubKey()),
                      "2ef316afd6177619f68ecfc6521fc3fcbf7faa2b25273f6ddea7971fae0de144");
    BOOST_CHECK_EQUAL(public_script.size(), 34U);
    BOOST_CHECK_EQUAL(regtest_script.size(), 34U);

    for (const ChainType chain_type : {ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET, ChainType::REGTEST}) {
        const auto params{CreateChainParams(m_args, chain_type)};
        BOOST_CHECK(params->GetConsensus().genesis_coinbase_spendable);
        BOOST_REQUIRE_EQUAL(params->GenesisBlock().vtx.size(), 1U);
        BOOST_REQUIRE_EQUAL(params->GenesisBlock().vtx.front()->vout.size(), 1U);
        BOOST_CHECK_EQUAL(params->GenesisBlock().vtx.front()->vout.front().nValue, 10'000'000 * COIN);
        BOOST_CHECK(params->GenesisBlock().vtx.front()->vout.front().GetType() == TxOutputType::P2PK);
        BOOST_CHECK(params->GenesisBlock().vtx.front()->vout.front().GetP2PKPubKey().has_value());
        if (chain_type != ChainType::REGTEST) {
            BOOST_CHECK(params->GenesisBlock().vtx.front()->vout.front().scriptPubKey ==
                        (chain_type == ChainType::TESTNET4 ? public_script : historical_script));
        }
    }
}

BOOST_FIXTURE_TEST_CASE(mainnet_fixture_genesis_coinbase_is_spendable, TestingSetup)
{
    const CBlock& genesis{Params().GenesisBlock()};
    BOOST_REQUIRE_EQUAL(genesis.vtx.size(), 1U);
    BOOST_REQUIRE_EQUAL(genesis.vtx.front()->vout.size(), 1U);

    const COutPoint genesis_outpoint{genesis.vtx.front()->GetHash(), 0};
    LOCK(cs_main);
    const Coin& coin{Assert(m_node.chainman)->ActiveChainstate().CoinsTip().AccessCoin(genesis_outpoint)};
    BOOST_REQUIRE(!coin.IsSpent());
    BOOST_CHECK(coin.IsCoinBase());
    BOOST_CHECK_EQUAL(coin.nHeight, 0);
    BOOST_CHECK_EQUAL(coin.out.nValue, genesis.vtx.front()->vout.front().nValue);
    BOOST_CHECK(coin.out.scriptPubKey == genesis.vtx.front()->vout.front().scriptPubKey);
}

BOOST_FIXTURE_TEST_CASE(regtest_genesis_coinbase_can_be_spent, TestChain100Setup)
{
    const CBlock& genesis{Params().GenesisBlock()};
    const auto secret{ParseHex("bc4470438702a7aa1c7696ff857e0439657583f87e3d889abea285771604891d")};
    CKey genesis_key;
    genesis_key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/false);
    BOOST_REQUIRE(genesis_key.IsValid());
    BOOST_REQUIRE(XOnlyPubKey{genesis_key.GetPubKey()} == *genesis.vtx.front()->vout.front().GetP2PKPubKey());

    const COutPoint genesis_outpoint{genesis.vtx.front()->GetHash(), 0};
    const CMutableTransaction spend{CreateValidMempoolTransaction(
        genesis.vtx.front(),
        /*input_vout=*/0,
        /*input_height=*/0,
        genesis_key,
        genesis.vtx.front()->vout.front().scriptPubKey,
        /*output_amount=*/9'999'999 * COIN,
        /*submit=*/false)};
    const CBlock block{CreateAndProcessBlock({spend}, genesis.vtx.front()->vout.front().scriptPubKey)};

    Chainstate& chainstate{Assert(m_node.chainman)->ActiveChainstate()};
    const COutPoint spend_outpoint{spend.GetHash(), 0};
    {
        LOCK(cs_main);
        BOOST_REQUIRE_EQUAL(chainstate.m_chain.Tip()->GetBlockHash(), block.GetHash());
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(genesis_outpoint));
        BOOST_CHECK(chainstate.CoinsTip().HaveCoin(spend_outpoint));
    }

    // A reorg must restore the development-fund allocation from the undo data.
    // Reconnecting the same block must then consume it again without changing
    // the monetary state.
    BlockValidationState state;
    {
        LOCK2(Assert(m_node.chainman)->GetMutex(), chainstate.MempoolMutex());
        BOOST_REQUIRE(chainstate.DisconnectTip(state, nullptr));
        BOOST_CHECK(chainstate.CoinsTip().HaveCoin(genesis_outpoint));
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(spend_outpoint));
    }

    BOOST_REQUIRE(chainstate.ActivateBestChain(state, nullptr));
    {
        LOCK(cs_main);
        BOOST_REQUIRE_EQUAL(chainstate.m_chain.Tip()->GetBlockHash(), block.GetHash());
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(genesis_outpoint));
        BOOST_CHECK(chainstate.CoinsTip().HaveCoin(spend_outpoint));
    }
}

BOOST_FIXTURE_TEST_CASE(regtest_assumeutxo_commitments_match_chainstate, TestChain100Setup)
{
    mineBlocks(10);

    LOCK(cs_main);
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    chainstate.ForceFlushStateToDisk(/*wipe_cache=*/false);
    const auto stats{kernel::ComputeUTXOStats(
        kernel::CoinStatsHashType::HASH_SERIALIZED,
        chainstate.CoinsDB(),
        chainman.m_blockman)};
    BOOST_REQUIRE(stats);

    const auto assumeutxo{Params().AssumeutxoForHeight(110)};
    BOOST_REQUIRE(assumeutxo);
    BOOST_CHECK_EQUAL(stats->nHeight, assumeutxo->height);
    BOOST_CHECK_EQUAL(stats->hashBlock.ToString(), assumeutxo->blockhash.ToString());
    BOOST_CHECK_EQUAL(stats->hashSerialized.ToString(), assumeutxo->hash_serialized.ToString());
    BOOST_CHECK_EQUAL(stats->nTransactions, assumeutxo->m_chain_tx_count);
    BOOST_CHECK_EQUAL(stats->coins_count, 111U);
}

BOOST_FIXTURE_TEST_CASE(regtest_fuzz_assumeutxo_commitment_matches_chainstate, RegtestTestingSetup)
{
    const auto chain{CreateBlockChain(2 * COINBASE_MATURITY, Params())};
    for (auto& block : chain) {
        BOOST_REQUIRE(!ProcessBlock(m_node, block).IsNull());
    }

    LOCK(cs_main);
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    chainstate.ForceFlushStateToDisk(/*wipe_cache=*/false);
    const auto stats{kernel::ComputeUTXOStats(
        kernel::CoinStatsHashType::HASH_SERIALIZED,
        chainstate.CoinsDB(),
        chainman.m_blockman)};
    BOOST_REQUIRE(stats);

    const auto assumeutxo{Params().AssumeutxoForHeight(2 * COINBASE_MATURITY)};
    BOOST_REQUIRE(assumeutxo);
    BOOST_CHECK_EQUAL(stats->nHeight, assumeutxo->height);
    BOOST_CHECK_EQUAL(stats->hashBlock.ToString(), assumeutxo->blockhash.ToString());
    BOOST_CHECK_EQUAL(stats->hashSerialized.ToString(), assumeutxo->hash_serialized.ToString());
    BOOST_CHECK_EQUAL(stats->nTransactions, assumeutxo->m_chain_tx_count);
    BOOST_CHECK_EQUAL(stats->coins_count, chain.size() + 1); // Includes spendable genesis.
}

//! Test resizing coins-related Chainstate caches during runtime.
//!
BOOST_AUTO_TEST_CASE(validation_chainstate_resize_caches)
{
    ChainstateManager& manager = *Assert(m_node.chainman);
    CTxMemPool& mempool = *Assert(m_node.mempool);
    Chainstate& c1 = WITH_LOCK(cs_main, return manager.InitializeChainstate(&mempool));
    c1.InitCoinsDB(
        /*cache_size_bytes=*/8_MiB, /*in_memory=*/true, /*should_wipe=*/false);
    WITH_LOCK(::cs_main, c1.InitCoinsCache(8_MiB));
    BOOST_REQUIRE(manager.LoadGenesisBlock()); // Need at least one block loaded to be able to flush caches

    // Add a coin to the in-memory cache, upsize once, then downsize.
    {
        LOCK(::cs_main);
        const auto outpoint = AddTestCoin(m_rng, c1.CoinsTip());

        // Set a meaningless bestblock value in the coinsview cache - otherwise we won't
        // flush during ResizecoinsCaches() and will subsequently hit an assertion.
        c1.CoinsTip().SetBestBlock(m_rng.rand256());

        BOOST_CHECK(c1.CoinsTip().HaveCoinInCache(outpoint));

        c1.ResizeCoinsCaches(
            16_MiB, // upsizing the coinsview cache
            4_MiB // downsizing the coinsdb cache
        );

        // View should still have the coin cached, since we haven't destructed the cache on upsize.
        BOOST_CHECK(c1.CoinsTip().HaveCoinInCache(outpoint));

        c1.ResizeCoinsCaches(
            4_MiB, // downsizing the coinsview cache
            8_MiB // upsizing the coinsdb cache
        );

        // The view cache should be empty since we had to destruct to downsize.
        BOOST_CHECK(!c1.CoinsTip().HaveCoinInCache(outpoint));
    }
}

BOOST_FIXTURE_TEST_CASE(connect_tip_does_not_cache_inputs_on_failed_connect, TestChain100Setup)
{
    Chainstate& chainstate{Assert(m_node.chainman)->ActiveChainstate()};

    COutPoint outpoint;
    {
        LOCK(cs_main);
        outpoint = AddTestCoin(m_rng, chainstate.CoinsTip());
        chainstate.CoinsTip().Flush(/*reallocate_cache=*/false);
    }

    const XOnlyPubKey test_pubkey{coinbaseKey.GetPubKey()};
    const CScript test_script{CScript{} << OP_1 << std::vector<unsigned char>{test_pubkey.begin(), test_pubkey.end()}};
    CMutableTransaction tx;
    tx.vin.emplace_back(outpoint);
    tx.vout.emplace_back(MAX_MONEY, test_pubkey);

    const auto tip{WITH_LOCK(cs_main, return chainstate.m_chain.Tip()->GetBlockHash())};
    const CBlock block{CreateBlock({tx}, test_script)};
    BOOST_CHECK(Assert(m_node.chainman)->ProcessNewBlock(std::make_shared<CBlock>(block), true, true, nullptr));

    LOCK(cs_main);
    BOOST_CHECK_EQUAL(tip, chainstate.m_chain.Tip()->GetBlockHash()); // block rejected
    BOOST_CHECK(!chainstate.CoinsTip().HaveCoinInCache(outpoint));    // input not cached
}

//! Test UpdateTip behavior for both active and background chainstates.
//!
//! When run on the background chainstate, UpdateTip should do a subset
//! of what it does for the active chainstate.
BOOST_FIXTURE_TEST_CASE(chainstate_update_tip, TestChain100Setup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    const auto get_notify_tip{[&]() {
        LOCK(m_node.notifications->m_tip_block_mutex);
        BOOST_REQUIRE(m_node.notifications->TipBlock());
        return *m_node.notifications->TipBlock();
    }};
    uint256 curr_tip = get_notify_tip();

    // Mine 10 more blocks, putting at us height 110 where a valid assumeutxo value can
    // be found.
    mineBlocks(10);

    // After adding some blocks to the tip, best block should have changed.
    BOOST_CHECK(get_notify_tip() != curr_tip);

    // Grab block 1 from disk; we'll add it to the background chain later.
    std::shared_ptr<CBlock> pblockone = std::make_shared<CBlock>();
    {
        LOCK(::cs_main);
        chainman.m_blockman.ReadBlock(*pblockone, *chainman.ActiveChain()[1]);
    }

    BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(
        this, NoMalleation, /*reset_chainstate=*/ true));

    // Ensure our active chain is the snapshot chainstate.
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.CurrentChainstate().m_from_snapshot_blockhash));

    curr_tip = get_notify_tip();

    // Mine a new block on top of the activated snapshot chainstate.
    mineBlocks(1);  // Defined in TestChain100Setup.

    // After adding some blocks to the snapshot tip, best block should have changed.
    BOOST_CHECK(get_notify_tip() != curr_tip);

    curr_tip = get_notify_tip();

    Chainstate& background_cs{*Assert(WITH_LOCK(::cs_main, return chainman.HistoricalChainstate()))};

    // Append the first block to the background chain.
    BlockValidationState state;
    CBlockIndex* pindex = nullptr;
    const CChainParams& chainparams = Params();
    bool newblock = false;

    // TODO: much of this is inlined from ProcessNewBlock(); just reuse PNB()
    // once it is changed to support multiple chainstates.
    {
        LOCK(::cs_main);
        bool checked = CheckBlock(*pblockone, state, chainparams.GetConsensus());
        BOOST_CHECK(checked);
        bool accepted = chainman.AcceptBlock(
            pblockone, state, &pindex, true, nullptr, &newblock, true);
        BOOST_CHECK(accepted);
    }

    // UpdateTip is called here
    bool block_added = background_cs.ActivateBestChain(state, pblockone);

    // Ensure tip is as expected
    BOOST_CHECK_EQUAL(background_cs.m_chain.Tip()->GetBlockHash(), pblockone->GetHash());

    // get_notify_tip() should be unchanged after adding a block to the background
    // validation chain.
    BOOST_CHECK(block_added);
    BOOST_CHECK_EQUAL(curr_tip, get_notify_tip());
}

BOOST_AUTO_TEST_SUITE_END()
