// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <coins.h>
#include <consensus/consensus.h>
#include <node/mining_types.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/mining.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <validation.h>

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

using node::BlockCreateOptions;

static void AssembleBlock(benchmark::Bench& bench)
{
    const auto test_setup = MakeNoLogFileContext<const TestingSetup>();

    BlockCreateOptions options{
        .coinbase_output_script = DeterministicP2PKScript(),
    };

    // Collect some loose transactions that spend the coinbases of our mined blocks
    constexpr size_t NUM_BLOCKS{200};
    std::array<CTransactionRef, NUM_BLOCKS - COINBASE_MATURITY + 1> txs;
    for (size_t b{0}; b < NUM_BLOCKS; ++b) {
        CMutableTransaction tx;
        const COutPoint coinbase_outpoint{MineBlock(test_setup->m_node, options)};
        tx.vin.emplace_back(coinbase_outpoint);
        tx.vout.emplace_back(1337, DeterministicP2PKScript());
        CTxOut spent_output;
        {
            LOCK(::cs_main);
            spent_output = Assert(test_setup->m_node.chainman)
                               ->ActiveChainstate()
                               .CoinsTip()
                               .AccessCoin(coinbase_outpoint)
                               .out;
        }
        SignDeterministicP2PKInputs(tx, {spent_output});
        if (NUM_BLOCKS - b >= COINBASE_MATURITY)
            txs.at(b) = MakeTransactionRef(tx);
    }
    {
        LOCK(::cs_main);

        for (const auto& txr : txs) {
            const MempoolAcceptResult res = test_setup->m_node.chainman->ProcessTransaction(txr);
            assert(res.m_result_type == MempoolAcceptResult::ResultType::VALID);
        }
    }

    bench.run([&] {
        PrepareBlock(test_setup->m_node, options);
    });
}
static void BlockAssemblerAddPackageTxns(benchmark::Bench& bench)
{
    FastRandomContext det_rand{true};
    auto testing_setup{MakeNoLogFileContext<TestChain100Setup>()};
    testing_setup->PopulateMempool(det_rand, /*num_transactions=*/1000, /*submit=*/true);

    bench.run([&] {
        PrepareBlock(testing_setup->m_node, {
            .coinbase_output_script = DeterministicP2PKScript(),
            .test_block_validity = false
        });
    });
}

BENCHMARK(AssembleBlock);
BENCHMARK(BlockAssemblerAddPackageTxns);
