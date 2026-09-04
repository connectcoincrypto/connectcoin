// Copyright (c) 2015-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <test/util/common.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <cstdlib>

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

/* Test calculation of next difficulty target with no constraints applying */
BOOST_AUTO_TEST_CASE(get_next_work)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& consensus = chainParams->GetConsensus();
    CBlockIndex pindexLast;
    pindexLast.nHeight = consensus.DifficultyAdjustmentInterval() - 1;
    pindexLast.nTime = 1262152739;
    pindexLast.nBits = 0x1d00ffff;
    int64_t nLastRetargetTime = pindexLast.nTime - 12 * 60 * 60;

    // Here (and below): expected_nbits is calculated in
    // CalculateNextWorkRequired(); redoing the calculation here would be just
    // reimplementing the same code that is written in pow.cpp. Rather than
    // copy that code, we just hardcode the expected result.
    unsigned int expected_nbits = 0x1c7fff80U;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, consensus), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(consensus, pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
}

/* Test the constraint on the upper bound for next work */
BOOST_AUTO_TEST_CASE(get_next_work_pow_limit)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& consensus = chainParams->GetConsensus();
    CBlockIndex pindexLast;
    pindexLast.nHeight = consensus.DifficultyAdjustmentInterval() - 1;
    pindexLast.nTime = 1233061996;
    pindexLast.nBits = UintToArith256(consensus.powLimit).GetCompact();
    int64_t nLastRetargetTime = pindexLast.nTime - 10 * consensus.nPowTargetTimespan;
    unsigned int expected_nbits = pindexLast.nBits;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, consensus), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(consensus, pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
}

/* Test the constraint on the lower bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_lower_limit_actual)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& consensus = chainParams->GetConsensus();
    CBlockIndex pindexLast;
    pindexLast.nHeight = consensus.DifficultyAdjustmentInterval() - 1;
    pindexLast.nTime = 1279297671;
    pindexLast.nBits = 0x1c05a3f4;
    int64_t nLastRetargetTime = pindexLast.nTime - 1;
    unsigned int expected_nbits = 0x1c0168fdU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, consensus), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(consensus, pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
    // Test that reducing nbits further would not be a PermittedDifficultyTransition.
    unsigned int invalid_nbits = expected_nbits-1;
    BOOST_CHECK(!PermittedDifficultyTransition(consensus, pindexLast.nHeight+1, pindexLast.nBits, invalid_nbits));
}

/* Test the constraint on the upper bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_upper_limit_actual)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& consensus = chainParams->GetConsensus();
    CBlockIndex pindexLast;
    pindexLast.nHeight = consensus.DifficultyAdjustmentInterval() - 1;
    pindexLast.nTime = 1269211443;
    pindexLast.nBits = 0x1c387f6f;
    int64_t nLastRetargetTime = pindexLast.nTime - 10 * consensus.nPowTargetTimespan;
    unsigned int expected_nbits = 0x1d00e1fdU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, consensus), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(consensus, pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
    // Test that increasing nbits further would not be a PermittedDifficultyTransition.
    unsigned int invalid_nbits = expected_nbits+1;
    BOOST_CHECK(!PermittedDifficultyTransition(consensus, pindexLast.nHeight+1, pindexLast.nBits, invalid_nbits));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_negative_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    nBits = UintToArith256(consensus.powLimit).GetCompact(true);
    hash = uint256{1};
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_overflow_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits{~0x00800000U};
    hash = uint256{1};
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_too_easy_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 nBits_arith = UintToArith256(consensus.powLimit);
    nBits_arith *= 2;
    nBits = nBits_arith.GetCompact();
    hash = uint256{1};
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_biger_hash_than_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith = UintToArith256(consensus.powLimit);
    nBits = hash_arith.GetCompact();
    hash_arith *= 2; // hash > nBits
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_zero_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith{0};
    nBits = hash_arith.GetCompact();
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(randomx_epoch_schedule)
{
    const auto main_params{CreateChainParams(*m_node.args, ChainType::MAIN)};
    const auto& main_consensus{main_params->GetConsensus()};
    BOOST_CHECK_EQUAL(RandomXSeedHeight(0, main_consensus), 0);
    BOOST_CHECK_EQUAL(RandomXSeedHeight(64, main_consensus), 0);
    BOOST_CHECK_EQUAL(RandomXSeedHeight(12'288, main_consensus), 0);
    BOOST_CHECK_EQUAL(RandomXSeedHeight(12'351, main_consensus), 0);
    BOOST_CHECK_EQUAL(RandomXSeedHeight(12'352, main_consensus), 12'288);
    BOOST_CHECK_EQUAL(RandomXSeedHeight(24'639, main_consensus), 12'288);
    BOOST_CHECK_EQUAL(RandomXSeedHeight(24'640, main_consensus), 24'576);

    const auto regtest_params{CreateChainParams(*m_node.args, ChainType::REGTEST)};
    BOOST_CHECK_EQUAL(RandomXSeedHeight(1000000, regtest_params->GetConsensus()), 0);

    // Exercise contextual key selection across two deliberately short epochs.
    // This catches off-by-one differences between the numeric schedule and
    // the ancestry lookup used by validation.
    auto short_epoch_consensus{regtest_params->GetConsensus()};
    short_epoch_consensus.randomx_epoch_blocks = 4;
    short_epoch_consensus.randomx_epoch_lag = 2;
    std::vector<CBlockIndex> blocks(10);
    std::vector<uint256> hashes(10);
    for (int height{0}; height < static_cast<int>(blocks.size()); ++height) {
        blocks[height].nHeight = height;
        blocks[height].pprev = height == 0 ? nullptr : &blocks[height - 1];
        hashes[height].data()[0] = static_cast<uint8_t>(height + 1);
        blocks[height].phashBlock = &hashes[height];
        blocks[height].BuildSkip();
    }
    BOOST_CHECK(GetRandomXKey(nullptr, short_epoch_consensus) == short_epoch_consensus.randomx_bootstrap_key);
    BOOST_CHECK(GetRandomXKey(&blocks[4], short_epoch_consensus) == short_epoch_consensus.randomx_bootstrap_key); // candidate 5
    BOOST_CHECK(GetRandomXKey(&blocks[5], short_epoch_consensus) == hashes[4]); // candidate 6
    BOOST_CHECK(GetRandomXKey(&blocks[8], short_epoch_consensus) == hashes[4]); // candidate 9
    BOOST_CHECK(GetRandomXKey(&blocks[9], short_epoch_consensus) == hashes[8]); // candidate 10
}

BOOST_AUTO_TEST_CASE(randomx_mock_pow)
{
    const auto regtest_params{CreateChainParams(ArgsManager{}, ChainType::REGTEST)};
    const auto& genesis{regtest_params->GenesisBlock()};
    auto consensus{regtest_params->GetConsensus()};
    BOOST_CHECK_EQUAL(consensus.randomx_mock_pow, std::getenv("TEST_RANDOMX_MOCK_POW") != nullptr);

    ArgsManager mock_args;
    mock_args.ForceSetArg("-test", "randomx_mock_pow");
    BOOST_CHECK(CreateChainParams(mock_args, ChainType::REGTEST)->GetConsensus().randomx_mock_pow);
    BOOST_CHECK(CreateChainParams(mock_args, ChainType::SIGNET)->GetConsensus().randomx_mock_pow);
    BOOST_CHECK(!CreateChainParams(mock_args, ChainType::MAIN)->GetConsensus().randomx_mock_pow);

    consensus.randomx_mock_pow = true;

    // The test-only hash substitution must preserve the exact hardcoded
    // RandomX genesis while using header hashing for every later block.
    BOOST_CHECK(CheckProofOfWork(genesis, uint256{}, 0, consensus));
    CBlockHeader next{genesis};
    next.hashPrevBlock = genesis.GetHash();
    BOOST_CHECK_EQUAL(
        CheckProofOfWork(next, uint256{}, 1, consensus),
        CheckProofOfWork(next.GetHash(), next.nBits, consensus));
}

BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    std::vector<CBlockIndex> blocks(10000);
    for (int i = 0; i < 10000; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1269211443 + i * chainParams->GetConsensus().nPowTargetSpacing;
        blocks[i].nBits = 0x207fffff; /* target 0x7fffff000... */
        blocks[i].nChainWork = i ? blocks[i - 1].nChainWork + GetBlockProof(blocks[i - 1]) : arith_uint256(0);
    }

    for (int j = 0; j < 1000; j++) {
        CBlockIndex *p1 = &blocks[m_rng.randrange(10000)];
        CBlockIndex *p2 = &blocks[m_rng.randrange(10000)];
        CBlockIndex *p3 = &blocks[m_rng.randrange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }
}

void sanity_check_chainparams(const ArgsManager& args, ChainType chain_type)
{
    const auto chainParams = CreateChainParams(args, chain_type);
    const auto consensus = chainParams->GetConsensus();

    // hash genesis is correct
    BOOST_CHECK_EQUAL(consensus.hashGenesisBlock, chainParams->GenesisBlock().GetHash());

    // ConnectCoin must not ship Bitcoin Core's generated peer snapshots. This
    // can be relaxed only when project-owned fixed seeds have been reviewed.
    BOOST_CHECK(chainParams->FixedSeeds().empty());

    // target timespan is an even multiple of spacing
    BOOST_CHECK_EQUAL(consensus.nPowTargetTimespan % consensus.nPowTargetSpacing, 0);
    if (!consensus.fPowNoRetargeting) {
        BOOST_CHECK_EQUAL(consensus.nPowTargetTimespan, 24 * 60 * 60);
        BOOST_CHECK_EQUAL(consensus.DifficultyAdjustmentInterval(), 8'640);
    }

    // genesis nBits is positive, doesn't overflow and is lower than powLimit
    arith_uint256 pow_compact;
    bool neg, over;
    pow_compact.SetCompact(chainParams->GenesisBlock().nBits, &neg, &over);
    BOOST_CHECK(!neg);
    BOOST_CHECK(pow_compact != 0);
    BOOST_CHECK(!over);
    BOOST_CHECK(UintToArith256(consensus.powLimit) >= pow_compact);

    // Every hardcoded genesis must satisfy the RandomX v2 proof of work. Slow
    // sanitizer and 32-bit jobs opt into a reduced profile which already runs
    // one real RandomX reference vector; normal jobs continue checking every
    // network genesis. LIGHT and FAST are consensus-identical.
    if (std::getenv("TEST_RANDOMX_MOCK_POW") == nullptr) {
        auto light_consensus{consensus};
        light_consensus.randomx_fast_mode = false;
        BOOST_CHECK(CheckProofOfWork(chainParams->GenesisBlock(), nullptr, light_consensus));
    }

    // Retargeting can make the target at most four times easier. The
    // overflow-safe multiply/divide in pow.cpp only requires that final value
    // to fit, independently of the absolute target timespan.
    if (!consensus.fPowNoRetargeting) {
        arith_uint256 targ_max{UintToArith256(uint256{"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"})};
        targ_max /= 4;
        BOOST_CHECK(UintToArith256(consensus.powLimit) < targ_max);
    }
}

BOOST_AUTO_TEST_CASE(ChainParams_MAIN_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::MAIN);
}

BOOST_AUTO_TEST_CASE(ChainParams_REGTEST_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::REGTEST);
}

BOOST_AUTO_TEST_CASE(ChainParams_TESTNET_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::TESTNET);
}

BOOST_AUTO_TEST_CASE(ChainParams_TESTNET4_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::TESTNET4);
}

BOOST_AUTO_TEST_CASE(ChainParams_SIGNET_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::SIGNET);
}

BOOST_AUTO_TEST_SUITE_END()
