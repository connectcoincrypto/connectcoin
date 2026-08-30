// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <interfaces/mining.h>
#include <key.h>
#include <node/blockstorage.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <script/interpreter.h>
#include <sync.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/check.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using kernel::ChainstateRole;

namespace validation_block_tests {

static CKey DeterministicP2PKKey()
{
    std::array<unsigned char, 32> secret{};
    secret.back() = 1;
    CKey key;
    key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
    assert(key.IsValid());
    return key;
}

static CScript DeterministicP2PKScript()
{
    const XOnlyPubKey pubkey{DeterministicP2PKKey().GetPubKey()};
    return CScript{} << OP_1 << std::vector<unsigned char>{pubkey.begin(), pubkey.end()};
}

static void SignP2PKInput(CMutableTransaction& tx, const CTxOut& prevout)
{
    PrecomputedTransactionData txdata;
    txdata.Init(tx, {prevout}, /*force=*/true);
    ScriptExecutionData execdata;
    execdata.m_annex_init = true;
    execdata.m_annex_present = false;
    uint256 sighash;
    BOOST_REQUIRE(SignatureHashSchnorr(sighash, execdata, tx, /*in_pos=*/0, SIGHASH_DEFAULT,
                                       SigVersion::TAPROOT, txdata, MissingDataBehavior::ASSERT_FAIL));
    std::array<unsigned char, 64> signature;
    BOOST_REQUIRE(DeterministicP2PKKey().SignSchnorr(sighash, signature, /*merkle_root=*/nullptr, uint256{}));
    tx.vin[0].scriptWitness.stack.emplace_back(signature.begin(), signature.end());
}

struct MinerTestingSetup : public RegTestingSetup {
    std::shared_ptr<CBlock> Block(const uint256& prev_hash);
    std::shared_ptr<const CBlock> GoodBlock(const uint256& prev_hash);
    std::shared_ptr<const CBlock> BadBlock(const uint256& prev_hash);
    std::shared_ptr<CBlock> FinalizeBlock(std::shared_ptr<CBlock> pblock);
    void BuildChain(const uint256& root, int height, unsigned int invalid_rate, unsigned int branch_rate, unsigned int max_size, std::vector<std::shared_ptr<const CBlock>>& blocks);
};
} // namespace validation_block_tests

BOOST_FIXTURE_TEST_SUITE(validation_block_tests, MinerTestingSetup)

struct TestSubscriber final : public CValidationInterface {
    uint256 m_expected_tip;

    explicit TestSubscriber(uint256 tip) : m_expected_tip(tip) {}

    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload) override
    {
        BOOST_CHECK_EQUAL(m_expected_tip, pindexNew->GetBlockHash());
    }

    void BlockConnected(const ChainstateRole& role, const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        BOOST_CHECK_EQUAL(m_expected_tip, block->hashPrevBlock);
        BOOST_CHECK_EQUAL(m_expected_tip, pindex->pprev->GetBlockHash());

        m_expected_tip = block->GetHash();
    }

    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        BOOST_CHECK_EQUAL(m_expected_tip, block->GetHash());
        BOOST_CHECK_EQUAL(m_expected_tip, pindex->GetBlockHash());

        m_expected_tip = block->hashPrevBlock;
    }
};

std::shared_ptr<CBlock> MinerTestingSetup::Block(const uint256& prev_hash)
{
    static uint64_t time = Params().GenesisBlock().nTime;
    static uint32_t extra_nonce{0};

    auto mining{interfaces::MakeMining(m_node)};
    auto block_template{mining->createNewBlock({
        .coinbase_output_script = DeterministicP2PKScript(),
    }, /*cooldown=*/false)};
    BOOST_REQUIRE(block_template);
    auto pblock = std::make_shared<CBlock>(block_template->getBlock());
    pblock->hashPrevBlock = prev_hash;
    pblock->nTime = ++time;

    // Make the coinbase deterministic and spendable with the test P2PK key.
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptWitness.SetNull();
    // Always pad with OP_0 as dummy extraNonce (also avoids bad-cb-length error for block <=16)
    const int prev_height{WITH_LOCK(::cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(prev_hash)->nHeight)};
    // Keep coinbase txids unique across competing blocks at the same height.
    txCoinbase.vin[0].scriptSig = CScript{} << prev_height + 1 << OP_0 << ++extra_nonce;
    txCoinbase.nLockTime = static_cast<uint32_t>(prev_height);
    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));

    return pblock;
}

std::shared_ptr<CBlock> MinerTestingSetup::FinalizeBlock(std::shared_ptr<CBlock> pblock)
{
    const CBlockIndex* prev_block{WITH_LOCK(::cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(pblock->hashPrevBlock))};
    m_node.chainman->GenerateCoinbaseCommitment(*pblock, prev_block);

    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);

    while (!CheckProofOfWork(*pblock, prev_block, Params().GetConsensus())) {
        ++(pblock->nNonce);
    }

    // submit block header, so that miner can get the block height from the
    // global state and the node has the topology of the chain
    BlockValidationState ignored;
    BOOST_CHECK(Assert(m_node.chainman)->ProcessNewBlockHeaders({{*pblock}}, true, ignored));

    return pblock;
}

// construct a valid block
std::shared_ptr<const CBlock> MinerTestingSetup::GoodBlock(const uint256& prev_hash)
{
    return FinalizeBlock(Block(prev_hash));
}

// construct an invalid block (but with a valid header)
std::shared_ptr<const CBlock> MinerTestingSetup::BadBlock(const uint256& prev_hash)
{
    auto pblock = Block(prev_hash);

    CMutableTransaction coinbase_spend;
    coinbase_spend.vin.emplace_back(COutPoint(pblock->vtx[0]->GetHash(), 0), CScript(), 0);
    coinbase_spend.vout.push_back(pblock->vtx[0]->vout[0]);

    CTransactionRef tx = MakeTransactionRef(coinbase_spend);
    pblock->vtx.push_back(tx);

    auto ret = FinalizeBlock(pblock);
    return ret;
}

// NOLINTNEXTLINE(misc-no-recursion)
void MinerTestingSetup::BuildChain(const uint256& root, int height, const unsigned int invalid_rate, const unsigned int branch_rate, const unsigned int max_size, std::vector<std::shared_ptr<const CBlock>>& blocks)
{
    if (height <= 0 || blocks.size() >= max_size) return;

    bool gen_invalid = m_rng.randrange(100U) < invalid_rate;
    bool gen_fork = m_rng.randrange(100U) < branch_rate;

    const std::shared_ptr<const CBlock> pblock = gen_invalid ? BadBlock(root) : GoodBlock(root);
    blocks.push_back(pblock);
    if (!gen_invalid) {
        BuildChain(pblock->GetHash(), height - 1, invalid_rate, branch_rate, max_size, blocks);
    }

    if (gen_fork) {
        blocks.push_back(GoodBlock(root));
        BuildChain(blocks.back()->GetHash(), height - 1, invalid_rate, branch_rate, max_size, blocks);
    }
}

BOOST_AUTO_TEST_CASE(processnewblock_signals_ordering)
{
    // build a large-ish chain that's likely to have some forks
    std::vector<std::shared_ptr<const CBlock>> blocks;
    while (blocks.size() < 50) {
        blocks.clear();
        BuildChain(Params().GenesisBlock().GetHash(), 100, 15, 10, 500, blocks);
    }

    bool ignored;
    // Connect the genesis block and drain any outstanding events
    BOOST_CHECK(Assert(m_node.chainman)->ProcessNewBlock(std::make_shared<CBlock>(Params().GenesisBlock()), true, true, &ignored));
    m_node.validation_signals->SyncWithValidationInterfaceQueue();

    // subscribe to events (this subscriber will validate event ordering)
    const CBlockIndex* initial_tip = nullptr;
    {
        LOCK(cs_main);
        initial_tip = m_node.chainman->ActiveChain().Tip();
    }
    auto sub = std::make_shared<TestSubscriber>(initial_tip->GetBlockHash());
    m_node.validation_signals->RegisterSharedValidationInterface(sub);

    // create a bunch of threads that repeatedly process a block generated above at random
    // this will create parallelism and randomness inside validation - the ValidationInterface
    // will subscribe to events generated during block validation and assert on ordering invariance
    std::vector<std::thread> threads;
    threads.reserve(10);
    for (int i = 0; i < 10; i++) {
        threads.emplace_back([&]() {
            bool ignored;
            FastRandomContext insecure;
            // Direct Schnorr validation is materially more expensive than the
            // former OP_TRUE fixture. One hundred randomized submissions per
            // thread still exercises ordering under contention without making
            // this single unit test dominate cross-platform CI time.
            for (int i = 0; i < 100; i++) {
                const auto& block = blocks[insecure.randrange(blocks.size() - 1)];
                Assert(m_node.chainman)->ProcessNewBlock(block, true, true, &ignored);
            }

            // to make sure that eventually we process the full chain - do it here
            for (const auto& block : blocks) {
                if (block->vtx.size() == 1) {
                    bool processed = Assert(m_node.chainman)->ProcessNewBlock(block, true, true, &ignored);
                    assert(processed);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
    m_node.validation_signals->SyncWithValidationInterfaceQueue();

    m_node.validation_signals->UnregisterSharedValidationInterface(sub);

    LOCK(cs_main);
    BOOST_CHECK_EQUAL(sub->m_expected_tip, m_node.chainman->ActiveChain().Tip()->GetBlockHash());
}

/**
 * Test that mempool updates happen atomically with reorgs.
 *
 * This prevents RPC clients, among others, from retrieving immediately-out-of-date mempool data
 * during large reorgs.
 *
 * The test verifies this by creating a chain of `num_txs` blocks, matures their coinbases, and then
 * submits txns spending from their coinbase to the mempool. A fork chain is then processed,
 * invalidating the txns and evicting them from the mempool.
 *
 * We verify that the mempool updates atomically by polling it continuously
 * from another thread during the reorg and checking that its size only changes
 * once. The size changing exactly once indicates that the polling thread's
 * view of the mempool is either consistent with the chain state before reorg,
 * or consistent with the chain state after the reorg, and not just consistent
 * with some intermediate state during the reorg.
 */
BOOST_AUTO_TEST_CASE(mempool_locks_reorg)
{
    bool ignored;
    auto ProcessBlock = [&](std::shared_ptr<const CBlock> block) -> bool {
        return Assert(m_node.chainman)->ProcessNewBlock(block, /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/&ignored);
    };

    // Process all mined blocks
    BOOST_REQUIRE(ProcessBlock(std::make_shared<CBlock>(Params().GenesisBlock())));
    auto last_mined = GoodBlock(Params().GenesisBlock().GetHash());
    BOOST_REQUIRE(ProcessBlock(last_mined));

    // Run the test multiple times
    for (int test_runs = 3; test_runs > 0; --test_runs) {
        BOOST_CHECK_EQUAL(last_mined->GetHash(), WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip()->GetBlockHash()));

        // Later on split from here
        const uint256 split_hash{last_mined->hashPrevBlock};

        // Create a bunch of transactions to spend the miner rewards of the
        // most recent blocks
        std::vector<CTransactionRef> txs;
        for (int num_txs = 22; num_txs > 0; --num_txs) {
            CMutableTransaction mtx;
            mtx.vin.emplace_back(COutPoint{last_mined->vtx[0]->GetHash(), 0}, CScript{});
            mtx.vout.push_back(last_mined->vtx[0]->vout[0]);
            mtx.vout[0].nValue -= 1000;
            SignP2PKInput(mtx, last_mined->vtx[0]->vout[0]);
            txs.push_back(MakeTransactionRef(mtx));

            last_mined = GoodBlock(last_mined->GetHash());
            BOOST_REQUIRE(ProcessBlock(last_mined));
        }

        // Mature the inputs of the txs
        for (int j = COINBASE_MATURITY; j > 0; --j) {
            last_mined = GoodBlock(last_mined->GetHash());
            BOOST_REQUIRE(ProcessBlock(last_mined));
        }

        // Mine a reorg (and hold it back) before adding the txs to the mempool
        const uint256 tip_init{last_mined->GetHash()};

        std::vector<std::shared_ptr<const CBlock>> reorg;
        last_mined = GoodBlock(split_hash);
        reorg.push_back(last_mined);
        for (size_t j = COINBASE_MATURITY + txs.size() + 1; j > 0; --j) {
            last_mined = GoodBlock(last_mined->GetHash());
            reorg.push_back(last_mined);
        }

        // Add the txs to the tx pool
        {
            LOCK(cs_main);
            for (const auto& tx : txs) {
                const MempoolAcceptResult result = m_node.chainman->ProcessTransaction(tx);
                BOOST_REQUIRE(result.m_result_type == MempoolAcceptResult::ResultType::VALID);
            }
        }

        // Check that all txs are in the pool
        {
            BOOST_CHECK_EQUAL(m_node.mempool->size(), txs.size());
        }

        // Run a thread that simulates an RPC caller that is polling while
        // validation is doing a reorg
        std::thread rpc_thread{[&]() {
            // This thread is checking that the mempool either contains all of
            // the transactions invalidated by the reorg, or none of them, and
            // not some intermediate amount.
            while (true) {
                LOCK(m_node.mempool->cs);
                if (m_node.mempool->size() == 0) {
                    // We are done with the reorg
                    break;
                }
                // Internally, we might be in the middle of the reorg, but
                // externally the reorg to the most-proof-of-work chain should
                // be atomic. So the caller assumes that the returned mempool
                // is consistent. That is, it has all txs that were there
                // before the reorg.
                assert(m_node.mempool->size() == txs.size());
                continue;
            }
            LOCK(cs_main);
            // We are done with the reorg, so the tip must have changed
            assert(tip_init != m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }};

        // Submit the reorg in this thread to invalidate and remove the txs from the tx pool
        for (const auto& b : reorg) {
            ProcessBlock(b);
        }
        // Check that the reorg was eventually successful
        BOOST_CHECK_EQUAL(last_mined->GetHash(), WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip()->GetBlockHash()));

        // We can join the other thread, which returns when the reorg was successful
        rpc_thread.join();
    }
}

BOOST_AUTO_TEST_CASE(witness_commitment_index)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto mining{interfaces::MakeMining(m_node)};
    auto block_template{mining->createNewBlock({
        .coinbase_output_script = DeterministicP2PKScript(),
    }, /*cooldown=*/false)};
    BOOST_REQUIRE(block_template);
    CBlock pblock{block_template->getBlock()};

    std::vector<unsigned char> first(MINIMUM_WITNESS_COMMITMENT);
    std::copy(std::begin(WITNESS_COMMITMENT_HEADER), std::end(WITNESS_COMMITMENT_HEADER), first.begin());
    std::vector<unsigned char> second{first};
    second.back() = 1;

    CMutableTransaction txCoinbase(*pblock.vtx[0]);
    txCoinbase.vin[0].scriptSig = CScript{} << OP_0 << first << second;
    pblock.vtx[0] = MakeTransactionRef(std::move(txCoinbase));

    // Each commitment is a canonical 36-byte push. The last marker wins.
    BOOST_CHECK_EQUAL(GetWitnessCommitmentOffset(pblock), 39);

    CMutableTransaction invalid_coinbase(*pblock.vtx[0]);
    invalid_coinbase.vin[0].scriptSig[40] ^= 1;
    pblock.vtx[0] = MakeTransactionRef(std::move(invalid_coinbase));
    BOOST_CHECK_EQUAL(GetWitnessCommitmentOffset(pblock), 2);

    // A marker embedded in the payload of a larger push is not a commitment.
    std::vector<unsigned char> embedded(MINIMUM_WITNESS_COMMITMENT + 1);
    embedded[0] = MINIMUM_WITNESS_COMMITMENT;
    std::copy(std::begin(WITNESS_COMMITMENT_HEADER), std::end(WITNESS_COMMITMENT_HEADER), embedded.begin() + 1);
    CMutableTransaction embedded_coinbase(*pblock.vtx[0]);
    embedded_coinbase.vin[0].scriptSig = CScript{} << embedded;
    pblock.vtx[0] = MakeTransactionRef(std::move(embedded_coinbase));
    BOOST_CHECK_EQUAL(GetWitnessCommitmentOffset(pblock), NO_WITNESS_COMMITMENT);

    // The 36-byte payload must use its canonical direct-push opcode.
    CScript nonminimal{OP_PUSHDATA1};
    nonminimal.push_back(MINIMUM_WITNESS_COMMITMENT);
    nonminimal.insert(nonminimal.end(), first.begin(), first.end());
    CMutableTransaction nonminimal_coinbase(*pblock.vtx[0]);
    nonminimal_coinbase.vin[0].scriptSig = std::move(nonminimal);
    pblock.vtx[0] = MakeTransactionRef(std::move(nonminimal_coinbase));
    BOOST_CHECK_EQUAL(GetWitnessCommitmentOffset(pblock), NO_WITNESS_COMMITMENT);
}
BOOST_AUTO_TEST_SUITE_END()
