// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <coins.h>
#include <consensus/validation.h>
#include <interfaces/chain.h>
#include <key.h>
#include <node/blockstorage.h>
#include <node/p2c_bounty_catalog.h>
#include <test/util/common.h>
#include <test/util/script.h>
#include <test/util/setup_common.h>
#include <script/solver.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <future>
#include <map>
#include <utility>

using interfaces::FoundBlock;

BOOST_AUTO_TEST_SUITE(interfaces_tests)

BOOST_FIXTURE_TEST_CASE(findBlock, TestChain100Setup)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();

    uint256 hash;
    BOOST_CHECK(chain->findBlock(active[10]->GetBlockHash(), FoundBlock().hash(hash)));
    BOOST_CHECK_EQUAL(hash, active[10]->GetBlockHash());

    int height = -1;
    BOOST_CHECK(chain->findBlock(active[20]->GetBlockHash(), FoundBlock().height(height)));
    BOOST_CHECK_EQUAL(height, active[20]->nHeight);

    CBlock data;
    BOOST_CHECK(chain->findBlock(active[30]->GetBlockHash(), FoundBlock().data(data)));
    BOOST_CHECK_EQUAL(data.GetHash(), active[30]->GetBlockHash());

    int64_t time = -1;
    BOOST_CHECK(chain->findBlock(active[40]->GetBlockHash(), FoundBlock().time(time)));
    BOOST_CHECK_EQUAL(time, active[40]->GetBlockTime());

    int64_t max_time = -1;
    BOOST_CHECK(chain->findBlock(active[50]->GetBlockHash(), FoundBlock().maxTime(max_time)));
    BOOST_CHECK_EQUAL(max_time, active[50]->GetBlockTimeMax());

    int64_t mtp_time = -1;
    BOOST_CHECK(chain->findBlock(active[60]->GetBlockHash(), FoundBlock().mtpTime(mtp_time)));
    BOOST_CHECK_EQUAL(mtp_time, active[60]->GetMedianTimePast());

    bool cur_active{false}, next_active{false};
    uint256 next_hash;
    BOOST_CHECK_EQUAL(active.Height(), 100);
    BOOST_CHECK(chain->findBlock(active[99]->GetBlockHash(), FoundBlock().inActiveChain(cur_active).nextBlock(FoundBlock().inActiveChain(next_active).hash(next_hash))));
    BOOST_CHECK(cur_active);
    BOOST_CHECK(next_active);
    BOOST_CHECK_EQUAL(next_hash, active[100]->GetBlockHash());
    cur_active = next_active = false;
    BOOST_CHECK(chain->findBlock(active[100]->GetBlockHash(), FoundBlock().inActiveChain(cur_active).nextBlock(FoundBlock().inActiveChain(next_active))));
    BOOST_CHECK(cur_active);
    BOOST_CHECK(!next_active);

    BOOST_CHECK(!chain->findBlock({}, FoundBlock()));
}

BOOST_FIXTURE_TEST_CASE(findFirstBlockWithTimeAndHeight, TestChain100Setup)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();
    uint256 hash;
    int height;
    BOOST_CHECK(chain->findFirstBlockWithTimeAndHeight(/* min_time= */ 0, /* min_height= */ 5, FoundBlock().hash(hash).height(height)));
    BOOST_CHECK_EQUAL(hash, active[5]->GetBlockHash());
    BOOST_CHECK_EQUAL(height, 5);
    BOOST_CHECK(!chain->findFirstBlockWithTimeAndHeight(/* min_time= */ active.Tip()->GetBlockTimeMax() + 1, /* min_height= */ 0));
}

BOOST_FIXTURE_TEST_CASE(findAncestorByHeight, TestChain100Setup)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();
    uint256 hash;
    BOOST_CHECK(chain->findAncestorByHeight(active[20]->GetBlockHash(), 10, FoundBlock().hash(hash)));
    BOOST_CHECK_EQUAL(hash, active[10]->GetBlockHash());
    BOOST_CHECK(!chain->findAncestorByHeight(active[10]->GetBlockHash(), 20));
}

BOOST_FIXTURE_TEST_CASE(findAncestorByHash, TestChain100Setup)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();
    int height = -1;
    BOOST_CHECK(chain->findAncestorByHash(active[20]->GetBlockHash(), active[10]->GetBlockHash(), FoundBlock().height(height)));
    BOOST_CHECK_EQUAL(height, 10);
    BOOST_CHECK(!chain->findAncestorByHash(active[10]->GetBlockHash(), active[20]->GetBlockHash()));
}

BOOST_FIXTURE_TEST_CASE(findCommonAncestor, TestChain100Setup)
{
    auto& chain = m_node.chain;
    const CChain& active{*WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return &Assert(m_node.chainman)->ActiveChain())};
    auto* orig_tip = active.Tip();
    for (int i = 0; i < 10; ++i) {
        BlockValidationState state;
        m_node.chainman->ActiveChainstate().InvalidateBlock(state, active.Tip());
    }
    BOOST_CHECK_EQUAL(active.Height(), orig_tip->nHeight - 10);
    coinbaseKey.MakeNewKey(true);
    for (int i = 0; i < 20; ++i) {
        CreateAndProcessBlock({}, GetScriptForP2PKOutput(coinbaseKey));
    }
    BOOST_CHECK_EQUAL(active.Height(), orig_tip->nHeight + 10);
    uint256 fork_hash;
    int fork_height;
    int orig_height;
    BOOST_CHECK(chain->findCommonAncestor(orig_tip->GetBlockHash(), active.Tip()->GetBlockHash(), FoundBlock().height(fork_height).hash(fork_hash), FoundBlock().height(orig_height)));
    BOOST_CHECK_EQUAL(orig_height, orig_tip->nHeight);
    BOOST_CHECK_EQUAL(fork_height, orig_tip->nHeight - 10);
    BOOST_CHECK_EQUAL(fork_hash, active[fork_height]->GetBlockHash());

    uint256 active_hash, orig_hash;
    BOOST_CHECK(!chain->findCommonAncestor(active.Tip()->GetBlockHash(), {}, {}, FoundBlock().hash(active_hash), {}));
    BOOST_CHECK(!chain->findCommonAncestor({}, orig_tip->GetBlockHash(), {}, {}, FoundBlock().hash(orig_hash)));
    BOOST_CHECK_EQUAL(active_hash, active.Tip()->GetBlockHash());
    BOOST_CHECK_EQUAL(orig_hash, orig_tip->GetBlockHash());
}

BOOST_FIXTURE_TEST_CASE(hasBlocks, TestChain100Setup)
{
    LOCK(::cs_main);
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();

    // Test ranges
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, 90));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, {}));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 0, 90));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));
    active[5]->nStatus &= ~BLOCK_HAVE_DATA;
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, 90));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, 90));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));
    active[95]->nStatus &= ~BLOCK_HAVE_DATA;
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, 90));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 10, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, 90));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));
    active[50]->nStatus &= ~BLOCK_HAVE_DATA;
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 10, 90));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 10, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, 90));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));

    // Test edge cases
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 6, 49));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 5, 49));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 6, 50));
}

BOOST_FIXTURE_TEST_CASE(p2c_catalog_updates_new_blocks_and_reorgs, TestChain100Setup)
{
    auto& chain{*m_node.chain};
    const auto read = [&] {
        std::map<COutPoint, CTxOut> result;
        BOOST_REQUIRE(chain.scanP2CBounties([&](const auto& outpoint, const auto& output) {
            result.emplace(outpoint, output);
            return true;
        }, [] { return false; }));
        return result;
    };
    BOOST_CHECK(read().empty()); // Initialize before the funding block.
    const CTxOut bounty{COIN, PayToDomainOutput{"example.com", uint256{}, 1}};
    const auto [funding, fee]{CreateValidTransaction({m_coinbase_txns[0]}, {{m_coinbase_txns[0]->GetHash(), 0}},
        1, {coinbaseKey}, {bounty}, std::nullopt, std::nullopt)};
    const auto block{CreateAndProcessBlock({funding}, GetScriptForP2PKOutput(coinbaseKey))};
    const COutPoint funded{funding.GetHash(), 0};
    BOOST_REQUIRE(read().contains(funded));
    // A test-only mutation at the same tip MUST NOT trigger a whole-UTXO scan.
    const COutPoint sentinel{Txid::FromUint256(uint256::ONE), 0};
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(sentinel, Coin{bounty, 100, false}, false));
    BOOST_CHECK_EQUAL(read().size(), 1U);
    CreateAndProcessBlock({}, GetScriptForP2PKOutput(coinbaseKey));
    BOOST_CHECK_EQUAL(read().size(), 1U);
    // Stop a visitor early without truncating the shared catalog.
    unsigned visits{0};
    BOOST_REQUIRE(chain.scanP2CBounties([&](const auto&, const auto&) { ++visits; return false; }, [] { return false; }));
    BOOST_CHECK_EQUAL(visits, 1U);
    BOOST_CHECK(!chain.scanP2CBounties([](const auto&, const auto&) { return true; }, [] { return true; }));
    BOOST_CHECK_EQUAL(read().size(), 1U);
    BlockValidationState state;
    auto* index{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(block.GetHash()))};
    BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, index));
    BOOST_CHECK(read().empty());
    CreateAndProcessBlock({}, GetScriptForP2PKOutput(coinbaseKey));
    BOOST_CHECK(read().empty());
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().SpendCoin(sentinel));
}

BOOST_FIXTURE_TEST_CASE(p2c_catalog_recovers_after_pruning, TestChain100Setup)
{
    auto& chain{*m_node.chain};
    auto& chainman{*m_node.chainman};
    auto& blockman{chainman.m_blockman};
    // Start the catalog before funding, then let pruning overtake its tip.
    BOOST_REQUIRE(chain.scanP2CBounties([](const auto&, const auto&) { return true; }, [] { return false; }));
    const CTxOut bounty{COIN, PayToDomainOutput{"example.com", uint256{}, 1}};
    const auto [funding, fee]{CreateValidTransaction({m_coinbase_txns[0]}, {{m_coinbase_txns[0]->GetHash(), 0}},
        1, {coinbaseKey}, {bounty}, std::nullopt, std::nullopt)};
    const auto block{CreateAndProcessBlock({funding}, GetScriptForP2PKOutput(coinbaseKey))};
    const auto* index{WITH_LOCK(cs_main, return blockman.LookupBlockIndex(block.GetHash()))};
    const int file{WITH_LOCK(cs_main, return index->GetBlockPos().nFile)};
    WITH_LOCK(cs_main, blockman.GetBlockFileInfo(file)->nSize = node::MAX_BLOCKFILE_SIZE);
    CreateAndProcessBlock({}, GetScriptForP2PKOutput(coinbaseKey));
    BOOST_REQUIRE_NE(file, WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->GetBlockPos().nFile));
    WITH_LOCK(cs_main, blockman.PruneOneBlockFile(file));
    blockman.m_have_pruned = true;
    blockman.UnlinkPrunedFiles({file}); // Only this fixture's temporary block files.
    CBlock missing;
    BOOST_CHECK(!blockman.ReadBlock(missing, *index));
    const auto check = [&] {
        std::map<COutPoint, CTxOut> found;
        BOOST_REQUIRE(chain.scanP2CBounties([&](const auto& outpoint, const auto& output) {
            found.emplace(outpoint, output);
            return true;
        }, [] { return false; }));
        BOOST_REQUIRE_EQUAL(found.size(), 1U);
        BOOST_CHECK(found.contains({funding.GetHash(), 0}));
    };
    check(); // Missing incremental history must fall back to current UTXOs.
    CreateAndProcessBlock({}, GetScriptForP2PKOutput(coinbaseKey));
    check(); // Subsequent updates must continue from the rebuilt tip.
}

BOOST_FIXTURE_TEST_CASE(p2c_catalog_concurrent_readers_release_before_visiting, TestChain100Setup)
{
    const CTxOut bounty{COIN, PayToDomainOutput{"example.com", uint256{}, 1}};
    const auto [funding, fee]{CreateValidTransaction({m_coinbase_txns[0]}, {{m_coinbase_txns[0]->GetHash(), 0}},
        1, {coinbaseKey}, {bounty}, std::nullopt, std::nullopt)};
    CreateAndProcessBlock({funding}, GetScriptForP2PKOutput(coinbaseKey));
    std::promise<void> start, release, entered_first, entered_second;
    const auto starting{start.get_future().share()}, released{release.get_future().share()};
    auto first_entered{entered_first.get_future()}, second_entered{entered_second.get_future()};
    const auto read = [&](std::promise<void>& entered) {
        starting.wait();
        std::map<COutPoint, CTxOut> found;
        const bool ok{m_node.chain->scanP2CBounties([&](const auto& outpoint, const auto& output) {
            found.emplace(outpoint, output);
            entered.set_value();
            released.wait();
            return false;
        }, [] { return false; })};
        return std::pair{ok, found};
    };
    // Two wallet-like readers race the first initialization of one catalog.
    auto first{std::async(std::launch::async, [&] { return read(entered_first); })};
    auto second{std::async(std::launch::async, [&] { return read(entered_second); })};
    start.set_value();
    const bool both_visiting{first_entered.wait_for(std::chrono::seconds{10}) == std::future_status::ready &&
                             second_entered.wait_for(std::chrono::seconds{10}) == std::future_status::ready};
    // Always release and join before assertions, including on a lock regression.
    release.set_value();
    const auto [first_ok, first_found]{first.get()};
    const auto [second_ok, second_found]{second.get()};
    BOOST_CHECK(both_visiting);
    BOOST_CHECK(first_ok && second_ok);
    BOOST_CHECK_EQUAL(first_found.size(), 1U);
    BOOST_CHECK_EQUAL(second_found.size(), 1U);
    BOOST_CHECK(first_found.contains({funding.GetHash(), 0}));
    BOOST_CHECK(second_found.contains({funding.GetHash(), 0}));
}

BOOST_FIXTURE_TEST_CASE(p2c_catalog_undo_restores_spends_in_reverse_order, BasicTestingSetup)
{
    const CTxOut bounty{COIN, PayToDomainOutput{"example.com", uint256{}, 1}};
    const COutPoint original{Txid::FromUint256(uint256::ONE), 0};
    CMutableTransaction coinbase, parent, child;
    coinbase.vin.emplace_back();
    coinbase.vout.push_back(bounty);
    parent.vin.emplace_back(original);
    parent.vout.push_back(bounty);
    child.vin.emplace_back(parent.GetHash(), 0);
    CKey key;
    key.MakeNewKey(true);
    child.vout.emplace_back(COIN, GetScriptForP2PKOutput(key));
    CBlock block;
    block.vtx = {MakeTransactionRef(coinbase), MakeTransactionRef(parent), MakeTransactionRef(child)};
    CBlockUndo undo;
    undo.vtxundo.resize(2);
    for (auto& tx : undo.vtxundo) tx.vprevout.emplace_back(bounty, 1, false);
    std::map<COutPoint, CTxOut> catalog{{original, bounty}};
    BOOST_REQUIRE(node::ApplyP2CBountyBlock(catalog, block, nullptr, [] { return false; }));
    BOOST_CHECK_EQUAL(catalog.size(), 1U);
    BOOST_CHECK(catalog.contains({coinbase.GetHash(), 0}));
    BOOST_REQUIRE(node::ApplyP2CBountyBlock(catalog, block, &undo, [] { return false; }));
    BOOST_CHECK_EQUAL(catalog.size(), 1U);
    BOOST_CHECK(catalog.contains(original));
    // Cancellation and malformed undo cannot report a complete update.
    BOOST_CHECK(!node::ApplyP2CBountyBlock(catalog, block, nullptr, [] { return true; }));
    undo.vtxundo[0].vprevout.clear();
    BOOST_CHECK(!node::ApplyP2CBountyBlock(catalog, block, &undo, [] { return false; }));
}

BOOST_AUTO_TEST_SUITE_END()
