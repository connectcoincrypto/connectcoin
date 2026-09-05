// Copyright (c) 2014-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <core_io.h>
#include <hash.h>
#include <kernel/mempool_options.h>
#include <net.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <signet.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <validation.h>

#include <algorithm>
#include <array>
#include <string>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(validation_tests, BasicTestingSetup)

static CTxOut DeterministicP2PKOutput(CAmount amount)
{
    constexpr std::array<unsigned char, 32> pubkey{
        0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac,
        0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b, 0x07,
        0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9,
        0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98,
    };
    return CTxOut{amount, XOnlyPubKey{pubkey}};
}

static void TestBlockSubsidyHalvings(const Consensus::Params& consensusParams)
{
    int maxHalvings = 64;
    CAmount nInitialSubsidy = 15 * COIN;

    CAmount nPreviousSubsidy = nInitialSubsidy * 2; // for height == 0
    BOOST_CHECK_EQUAL(nPreviousSubsidy, nInitialSubsidy * 2);
    for (int nHalvings = 0; nHalvings < maxHalvings; nHalvings++) {
        int nHeight = nHalvings * consensusParams.nSubsidyHalvingInterval;
        CAmount nSubsidy = GetBlockSubsidy(nHeight, consensusParams);
        BOOST_CHECK(nSubsidy <= nInitialSubsidy);
        BOOST_CHECK_EQUAL(nSubsidy, nPreviousSubsidy / 2);
        nPreviousSubsidy = nSubsidy;
    }
    BOOST_CHECK_EQUAL(GetBlockSubsidy(maxHalvings * consensusParams.nSubsidyHalvingInterval, consensusParams), 0);
}

static void TestBlockSubsidyHalvings(int nSubsidyHalvingInterval)
{
    Consensus::Params consensusParams;
    consensusParams.nSubsidyHalvingInterval = nSubsidyHalvingInterval;
    TestBlockSubsidyHalvings(consensusParams);
}

BOOST_AUTO_TEST_CASE(block_subsidy_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    TestBlockSubsidyHalvings(chainParams->GetConsensus()); // As in main
    TestBlockSubsidyHalvings(150); // As in regtest
    TestBlockSubsidyHalvings(1000); // Just another interval
}

BOOST_AUTO_TEST_CASE(subsidy_limit_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    CAmount nSum = 0;
    for (int nHeight = 0; nHeight < 14000000; nHeight += 1000) {
        CAmount nSubsidy = GetBlockSubsidy(nHeight, chainParams->GetConsensus());
        BOOST_CHECK(nSubsidy <= 15 * COIN);
        nSum += nSubsidy * 1000;
        BOOST_CHECK(MoneyRange(nSum));
    }
    BOOST_CHECK_EQUAL(nSum, CAmount{862'500'000'000'000'000});
}

BOOST_AUTO_TEST_CASE(block_subsidy_weight_penalty_test)
{
    const auto chain_params{CreateChainParams(*m_node.args, ChainType::MAIN)};
    const auto& consensus{chain_params->GetConsensus()};
    const CAmount base_subsidy{15 * COIN};

    BOOST_CHECK_EQUAL(GetBlockSubsidyForWeight(1, 0, consensus), base_subsidy);
    BOOST_CHECK_EQUAL(GetBlockSubsidyForWeight(1, MAX_BLOCK_WEIGHT / 2, consensus), base_subsidy * 95 / 100);
    BOOST_CHECK_EQUAL(GetBlockSubsidyForWeight(1, MAX_BLOCK_WEIGHT, consensus), base_subsidy * 90 / 100);
    BOOST_CHECK_EQUAL(GetBlockSubsidyForWeight(1, MAX_BLOCK_WEIGHT + 1ULL, consensus), base_subsidy * 90 / 100);

    // The dynamic relay floor follows the subsidy while keeping one
    // connect/vB above its marginal weight cost, as block assembly uses a
    // strict profitability comparison.
    constexpr int32_t sample_vsize{1000};
    constexpr uint64_t sample_weight{sample_vsize * WITNESS_SCALE_FACTOR};
    const CAmount initial_economic_fee{GetBlockSubsidyPenalty(base_subsidy, sample_weight) + ECONOMIC_RELAY_FEE_MARGIN};
    BOOST_CHECK_EQUAL(initial_economic_fee, 1'201'000);
    BOOST_CHECK_EQUAL(GetEconomicRelayFeeRate(1, consensus).GetFeePerK(), initial_economic_fee);
    BOOST_CHECK_EQUAL(CFeeRate{DEFAULT_MIN_RELAY_TX_FEE}.GetFeePerK(), initial_economic_fee);
    BOOST_CHECK_EQUAL(GetEconomicRelayFeeRate(consensus.nSubsidyHalvingInterval, consensus).GetFeePerK(), 601'000);
    BOOST_CHECK_EQUAL(GetEconomicRelayFeeRate(37 * consensus.nSubsidyHalvingInterval, consensus).GetFeePerK(), ECONOMIC_RELAY_FEE_MARGIN);

    bilingual_str error;
    CTxMemPool dynamic_pool{kernel::MemPoolOptions{}, error};
    BOOST_REQUIRE(error.empty());
    dynamic_pool.UpdateMinRelayFee(GetEconomicRelayFeeRate(consensus.nSubsidyHalvingInterval, consensus));
    BOOST_CHECK_EQUAL(dynamic_pool.GetMinRelayFee().GetFeePerK(), 601'000);

    kernel::MemPoolOptions explicit_options{};
    explicit_options.min_relay_feerate = CFeeRate{42'000};
    explicit_options.min_relay_feerate_is_explicit = true;
    CTxMemPool explicit_pool{explicit_options, error};
    BOOST_REQUIRE(error.empty());
    explicit_pool.UpdateMinRelayFee(GetEconomicRelayFeeRate(consensus.nSubsidyHalvingInterval, consensus));
    BOOST_CHECK_EQUAL(explicit_pool.GetMinRelayFee().GetFeePerK(), 42'000);

    // This specifically exercises a value for which the naive intermediate
    // MAX_MONEY * MAX_BLOCK_WEIGHT cannot fit in CAmount.
    BOOST_CHECK_EQUAL(GetBlockSubsidyPenalty(MAX_MONEY, MAX_BLOCK_WEIGHT), MAX_MONEY / 10);

    // Integer division rounds the withheld amount down. At the very end of
    // the subsidy schedule, a fraction of one connect cannot be withheld.
    BOOST_CHECK_EQUAL(GetBlockSubsidy(37 * consensus.nSubsidyHalvingInterval, consensus), 1);
    BOOST_CHECK_EQUAL(GetBlockSubsidyForWeight(37 * consensus.nSubsidyHalvingInterval, MAX_BLOCK_WEIGHT, consensus), 1);
}

static CAmount MaximumSupply(const CChainParams& chain_params)
{
    const auto& consensus = chain_params.GetConsensus();
    CAmount total{chain_params.GenesisBlock().vtx.front()->vout.front().nValue};

    for (int halving = 0; halving < 64; ++halving) {
        const int era_start = std::max(1, halving * consensus.nSubsidyHalvingInterval);
        const int era_end = (halving + 1) * consensus.nSubsidyHalvingInterval;
        total += GetBlockSubsidy(era_start, consensus) * (era_end - era_start);
        BOOST_CHECK(MoneyRange(total));
    }
    return total;
}

BOOST_AUTO_TEST_CASE(maximum_network_supply_test)
{
    for (const ChainType chain_type : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET}) {
        const auto chain_params = CreateChainParams(*m_node.args, chain_type);
        const auto& consensus = chain_params->GetConsensus();
        const CAmount total{MaximumSupply(*chain_params)};

        // Height 0 is the 10 million CC genesis output, not a regular 15 CC
        // subsidy. The remaining blocks and integer rounding leave 15.0045 CC
        // below MAX_MONEY on every public network.
        BOOST_CHECK_EQUAL(chain_params->GenesisBlock().vtx.front()->GetValueOut(), 10'000'000 * COIN);
        BOOST_CHECK_EQUAL(GetBlockSubsidy(1, consensus), 15 * COIN);
        BOOST_CHECK_EQUAL(GetBlockSubsidy(37 * consensus.nSubsidyHalvingInterval, consensus), 1);
        BOOST_CHECK_EQUAL(GetBlockSubsidy(38 * consensus.nSubsidyHalvingInterval, consensus), 0);
        BOOST_CHECK_EQUAL(total, CAmount{999'999'849'955'000'000});
        BOOST_CHECK_EQUAL(MAX_MONEY - total, CAmount{150'045'000'000});
    }

    // Regtest deliberately halves every 150 blocks to make subsidy transitions
    // practical to test. Its theoretical supply is therefore much smaller.
    const auto regtest_params = CreateChainParams(*m_node.args, ChainType::REGTEST);
    const auto& regtest_consensus = regtest_params->GetConsensus();
    BOOST_CHECK_EQUAL(regtest_params->GenesisBlock().vtx.front()->GetValueOut(), 10'000'000 * COIN);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(1, regtest_consensus), 15 * COIN);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(37 * regtest_consensus.nSubsidyHalvingInterval, regtest_consensus), 1);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(38 * regtest_consensus.nSubsidyHalvingInterval, regtest_consensus), 0);
    BOOST_CHECK_EQUAL(MaximumSupply(*regtest_params), CAmount{100'044'849'999'997'750});
}

BOOST_AUTO_TEST_CASE(signet_parse_tests)
{
    ArgsManager signet_argsman;
    signet_argsman.ForceSetArg("-signetchallenge", "51"); // set challenge to OP_TRUE
    const auto signet_params = CreateChainParams(signet_argsman, ChainType::SIGNET);
    CBlock block;
    BOOST_CHECK(signet_params->GetConsensus().signet_challenge == std::vector<uint8_t>{OP_TRUE});
    CScript challenge{OP_TRUE};

    // empty block is invalid
    BOOST_CHECK(!SignetTxs::Create(block, challenge));
    BOOST_CHECK(!CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // no witness commitment
    CMutableTransaction cb;
    cb.vin.resize(1);
    cb.vout.push_back(DeterministicP2PKOutput(0));
    block.vtx.push_back(MakeTransactionRef(cb));
    block.vtx.push_back(MakeTransactionRef(cb)); // Add dummy tx to exercise merkle root code
    BOOST_CHECK(!SignetTxs::Create(block, challenge));
    BOOST_CHECK(!CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // no header is treated valid
    std::vector<uint8_t> witness_commitment_section_141{0xaa, 0x21, 0xa9, 0xed};
    for (int i = 0; i < 32; ++i) {
        witness_commitment_section_141.push_back(0xff);
    }
    cb.vin.at(0).scriptSig = CScript{} << witness_commitment_section_141;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(SignetTxs::Create(block, challenge));
    BOOST_CHECK(CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // no data after header, valid
    std::vector<uint8_t> witness_commitment_section_325{0xec, 0xc7, 0xda, 0xa2};
    cb.vin.at(0).scriptSig = CScript{} << witness_commitment_section_141 << witness_commitment_section_325;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(SignetTxs::Create(block, challenge));
    BOOST_CHECK(CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // Premature end of data, invalid
    witness_commitment_section_325.push_back(0x01);
    witness_commitment_section_325.push_back(0x51);
    cb.vin.at(0).scriptSig = CScript{} << witness_commitment_section_141 << witness_commitment_section_325;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(!SignetTxs::Create(block, challenge));
    BOOST_CHECK(!CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // has data, valid
    witness_commitment_section_325.push_back(0x00);
    cb.vin.at(0).scriptSig = CScript{} << witness_commitment_section_141 << witness_commitment_section_325;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(SignetTxs::Create(block, challenge));
    BOOST_CHECK(CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // Extraneous data, invalid
    witness_commitment_section_325.push_back(0x00);
    cb.vin.at(0).scriptSig = CScript{} << witness_commitment_section_141 << witness_commitment_section_325;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(!SignetTxs::Create(block, challenge));
    BOOST_CHECK(!CheckSignetBlockSolution(block, signet_params->GetConsensus()));
}

//! Test retrieval of valid assumeutxo values.
BOOST_AUTO_TEST_CASE(test_assumeutxo)
{
    const auto params = CreateChainParams(*m_node.args, ChainType::REGTEST);

    const auto heights{params->GetAvailableSnapshotHeights()};
    BOOST_REQUIRE_EQUAL(heights.size(), 2U);
    BOOST_CHECK_EQUAL(heights[0], 110);
    BOOST_CHECK_EQUAL(heights[1], 200);

    const auto out110{params->AssumeutxoForHeight(110)};
    BOOST_REQUIRE(out110);
    BOOST_CHECK_EQUAL(out110->hash_serialized.ToString(), "e45ed18a928f9fa879bda51e00f492bfa7ba6b138f9fdf71f2bb29123af83507");
    BOOST_CHECK_EQUAL(out110->m_chain_tx_count, 111U);
    const uint256 expected_blockhash110{params->GetConsensus().randomx_mock_pow ? uint256{"307561b922859b22c518aaefd7c802d2571706a4e0ca1d0f506f24591607bef5"} : uint256{"fd2d86f0bfb22dba48758d291f89eb8f64bedaf13932350f3da84bbbf32fc260"}};
    BOOST_CHECK(out110->blockhash == expected_blockhash110);
    const auto out110_by_hash{params->AssumeutxoForBlockhash(expected_blockhash110)};
    BOOST_REQUIRE(out110_by_hash);
    BOOST_CHECK_EQUAL(out110_by_hash->height, 110);

    const auto out200{params->AssumeutxoForHeight(200)};
    BOOST_REQUIRE(out200);
    BOOST_CHECK_EQUAL(out200->hash_serialized.ToString(), "3fd7b22c0fa827f8119a9ae5a203481c3bcffd5d7dd74d9594ad87756b8434de");
    BOOST_CHECK_EQUAL(out200->m_chain_tx_count, 201U);

    const uint256 expected_blockhash200{params->GetConsensus().randomx_mock_pow ? uint256{"290319d9129f74fd13351f4df418434d4319579d66afb03f98f52a48981a11ff"} : uint256{"799750eee344c6bb269406c29e150138f7c56f3908c195edb25532083327f90b"}};
    BOOST_CHECK(out200->blockhash == expected_blockhash200);
    const auto out200_by_hash{params->AssumeutxoForBlockhash(expected_blockhash200)};
    BOOST_REQUIRE(out200_by_hash);
    BOOST_CHECK_EQUAL(out200_by_hash->height, 200);
    BOOST_CHECK(!params->AssumeutxoForBlockhash(uint256::ONE));
}

BOOST_AUTO_TEST_CASE(block_malleation)
{
    // Test utilities that calls `IsBlockMutated` and then clears the validity
    // cache flags on `CBlock`.
    auto is_mutated = [](CBlock& block, bool check_witness_root) {
        bool mutated{IsBlockMutated(block, check_witness_root)};
        block.fChecked = false;
        block.m_checked_witness_commitment = false;
        block.m_checked_merkle_root = false;
        return mutated;
    };
    auto is_not_mutated = [&is_mutated](CBlock& block, bool check_witness_root) {
        return !is_mutated(block, check_witness_root);
    };

    // Test utilities to create coinbase transactions and insert witness
    // commitments.
    //
    // Note: this will not include the witness stack by default to avoid
    // triggering the "no witnesses allowed for blocks that don't commit to
    // witnesses" rule when testing other malleation vectors.
    auto create_coinbase_tx = [](bool include_witness = false) {
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        if (include_witness) {
            coinbase.vin[0].scriptWitness.stack.resize(1);
            coinbase.vin[0].scriptWitness.stack[0] = std::vector<unsigned char>(32, 0x00);
        }

        coinbase.vout.push_back(DeterministicP2PKOutput(0));
        std::vector<unsigned char> commitment(MINIMUM_WITNESS_COMMITMENT);
        std::copy(std::begin(WITNESS_COMMITMENT_HEADER), std::end(WITNESS_COMMITMENT_HEADER), commitment.begin());
        coinbase.vin[0].scriptSig << commitment;

        auto tx = MakeTransactionRef(coinbase);
        assert(tx->IsCoinBase());
        return tx;
    };
    auto insert_witness_commitment = [](CBlock& block, uint256 commitment) {
        assert(!block.vtx.empty() && block.vtx[0]->IsCoinBase() && !block.vtx[0]->vin.empty());

        CMutableTransaction mtx{*block.vtx[0]};
        CHash256().Write(commitment).Write(std::vector<unsigned char>(32, 0x00)).Finalize(commitment);
        const int offset{GetWitnessCommitmentOffset(block)};
        assert(offset != NO_WITNESS_COMMITMENT);
        memcpy(&mtx.vin[0].scriptSig[offset + std::size(WITNESS_COMMITMENT_HEADER)], commitment.begin(), 32);
        block.vtx[0] = MakeTransactionRef(mtx);
    };

    {
        CBlock block;

        // Empty block is expected to have merkle root of 0x0.
        BOOST_CHECK(block.vtx.empty());
        block.hashMerkleRoot = uint256{1};
        BOOST_CHECK(is_mutated(block, /*check_witness_root=*/false));
        block.hashMerkleRoot = uint256{};
        BOOST_CHECK(is_not_mutated(block, /*check_witness_root=*/false));

        // Block with a single coinbase tx is mutated if the merkle root is not
        // equal to the coinbase tx's hash.
        block.vtx.push_back(create_coinbase_tx());
        BOOST_CHECK(block.vtx[0]->GetHash().ToUint256() != block.hashMerkleRoot);
        BOOST_CHECK(is_mutated(block, /*check_witness_root=*/false));
        block.hashMerkleRoot = block.vtx[0]->GetHash().ToUint256();
        BOOST_CHECK(is_not_mutated(block, /*check_witness_root=*/false));

        // Block with two transactions is mutated if the merkle root does not
        // match the double sha256 of the concatenation of the two transaction
        // hashes.
        block.vtx.push_back(MakeTransactionRef(CMutableTransaction{}));
        BOOST_CHECK(is_mutated(block, /*check_witness_root=*/false));
        HashWriter hasher;
        hasher.write(block.vtx[0]->GetHash());
        hasher.write(block.vtx[1]->GetHash());
        block.hashMerkleRoot = hasher.GetHash();
        BOOST_CHECK(is_not_mutated(block, /*check_witness_root=*/false));

        // Block with two transactions is mutated if any node is duplicate.
        {
            block.vtx[1] = block.vtx[0];
            HashWriter hasher;
            hasher.write(block.vtx[0]->GetHash());
            hasher.write(block.vtx[1]->GetHash());
            block.hashMerkleRoot = hasher.GetHash();
            BOOST_CHECK(is_mutated(block, /*check_witness_root=*/false));
        }

        // A typed P2PK output alone is 41 bytes, so a structurally valid
        // coinbase can no longer have the historical ambiguous 64-byte size.
        BOOST_CHECK(GetSerializeSize(TX_NO_WITNESS(*create_coinbase_tx())) > 64);
    }

    {
        // The historical 64-byte Bitcoin transaction used to exercise merkle
        // tree ambiguity contains Script-form outputs. It must not decode as a
        // typed ConnectCoin transaction.
        CMutableTransaction legacy_collision;
        BOOST_CHECK(!DecodeHexTx(legacy_collision, "cdaf22d00002c6a7f848f8ae4d30054e61dcf3303d6fe01d282163341f06feecc10032b3160fcab87bdfe3ecfb769206ef2d991b92f8a268e423a6ef4d485f06", /*try_no_witness=*/true, /*try_witness=*/false));
    }

    {
        CBlock block;
        block.vtx.push_back(create_coinbase_tx(/*include_witness=*/true));
        {
            CMutableTransaction mtx;
            mtx.vin.resize(1);
            mtx.vin[0].scriptWitness.stack.resize(1);
            mtx.vin[0].scriptWitness.stack[0] = {0};
            block.vtx.push_back(MakeTransactionRef(mtx));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        // Block with witnesses is considered mutated if the witness commitment
        // is not validated.
        BOOST_CHECK(is_mutated(block, /*check_witness_root=*/false));
        // Block with invalid witness commitment is considered mutated.
        BOOST_CHECK(is_mutated(block, /*check_witness_root=*/true));

        // Block with valid commitment is not mutated
        {
            auto commitment{BlockWitnessMerkleRoot(block)};
            insert_witness_commitment(block, commitment);
            block.hashMerkleRoot = BlockMerkleRoot(block);
        }
        BOOST_CHECK(is_not_mutated(block, /*check_witness_root=*/true));

        // Malleating witnesses should be caught by `IsBlockMutated`.
        {
            CMutableTransaction mtx{*block.vtx[1]};
            assert(!mtx.vin[0].scriptWitness.stack[0].empty());
            ++mtx.vin[0].scriptWitness.stack[0][0];
            block.vtx[1] = MakeTransactionRef(mtx);
        }
        // Without also updating the witness commitment, the merkle root should
        // not change when changing one of the witnesses.
        BOOST_CHECK(block.hashMerkleRoot == BlockMerkleRoot(block));
        BOOST_CHECK(is_mutated(block, /*check_witness_root=*/true));
        {
            auto commitment{BlockWitnessMerkleRoot(block)};
            insert_witness_commitment(block, commitment);
            block.hashMerkleRoot = BlockMerkleRoot(block);
        }
        BOOST_CHECK(is_not_mutated(block, /*check_witness_root=*/true));

        // Test malleating the coinbase witness reserved value
        {
            CMutableTransaction mtx{*block.vtx[0]};
            mtx.vin[0].scriptWitness.stack.resize(0);
            block.vtx[0] = MakeTransactionRef(mtx);
            block.hashMerkleRoot = BlockMerkleRoot(block);
        }
        BOOST_CHECK(is_mutated(block, /*check_witness_root=*/true));
    }
}

BOOST_AUTO_TEST_SUITE_END()
