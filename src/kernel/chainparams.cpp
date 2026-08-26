// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/chainparams.h>

#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <crypto/hex_base.h>
#include <hash.h>
#include <kernel/messagestartchars.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/log.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

using namespace util::hex_literals;

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.version = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

/** Public-network development-fund key. The corresponding private key is not part of the source tree. */
static const CScript& PublicGenesisOutputScript()
{
    static const CScript script = CScript() << "02da12a44d69673e42ba95ac1d2bd4e5c76c3709a1765edbc5f52b8e5e643b0609"_hex << OP_CHECKSIG;
    return script;
}

/** Regtest-only key with a published private key, so tests can spend its genesis output. */
static const CScript& RegTestGenesisOutputScript()
{
    static const CScript script = CScript() << "04738a50e0af6185956d5e0c393859830eb70ea92351b19facbd47259cd7a10c278e0dc93464060c9d873d7a2b0fc106888d87539c83d75de94c3e43d8fbabbefb"_hex << OP_CHECKSIG;
    return script;
}

/** Build a reproducible ConnectCoin genesis block for the selected network. */
static CBlock CreateConnectCoinGenesisBlock(const char* pszTimestamp, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward, const CScript& genesisOutputScript)
{
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

void CChainParams::ApplyDeploymentOptions(const DeploymentOptions& opts)
{
    for (const auto& [dep, height] : opts.activation_heights) {
        switch (dep) {
        case Consensus::BuriedDeployment::DEPLOYMENT_SEGWIT:
            consensus.SegwitHeight = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_HEIGHTINCB:
            consensus.BIP34Height = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_DERSIG:
            consensus.BIP66Height = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_CLTV:
            consensus.BIP65Height = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_CSV:
            consensus.CSVHeight = int{height};
            break;
        }
    }

    for (const auto& [deployment_pos, version_bits_params] : opts.version_bits_parameters) {
        consensus.vDeployments[deployment_pos].nStartTime = version_bits_params.start_time;
        consensus.vDeployments[deployment_pos].nTimeout = version_bits_params.timeout;
        consensus.vDeployments[deployment_pos].min_activation_height = version_bits_params.min_activation_height;
    }
}

/**
 * Main network on which people trade goods and services.
 */
class CMainParams : public CChainParams {
public:
    CMainParams(const MainNetOptions& opts) {
        m_chain_type = ChainType::MAIN;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.genesis_coinbase_spendable = true;
        consensus.nSubsidyHalvingInterval = 450000;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"000001ffff000000000000000000000000000000000000000000000000000000"};
        consensus.nPowTargetTimespan = 24 * 60 * 60; // one day
        consensus.nPowTargetSpacing = 60;
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.enforce_BIP94 = false;
        consensus.fPowNoRetargeting = false;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1815; // 90%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        ApplyDeploymentOptions(opts.dep_opts);

        // Non-zero so IBD can distinguish a chain that has not accumulated
        // even the minimum work represented by the genesis chain.
        consensus.nMinimumChainWork = uint256{1};
        consensus.defaultAssumeValid = uint256{};

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        // First four bytes of SHA256("ConnectCoin main network").
        pchMessageStart = {0xd9, 0x51, 0xa5, 0xe2};
        nDefaultPort = 48173;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        genesis = CreateConnectCoinGenesisBlock("teste testado", 1787596781, 314125, 0x1e01ffff, 1, 10'000'000 * COIN, PublicGenesisOutputScript());
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"0000004b461aae33a4be0ee95ae8461155f2c48130bc8dd521adb71ec0d3e9a2"});
        assert(genesis.hashMerkleRoot == uint256{"16c0a19492ab1767d72cea5f47a6720fa85bc0f21716c8affdf541758d34611b"});

        // Note that of those which support the service bits prefix, most only support a subset of
        // possible options.
        // This is fine at runtime as we'll fall back to using them as an addrfetch if they don't support the
        // service bits we want, but we should get them updated to support all service bits wanted by any
        // release ASAP to avoid it where possible.
        // ConnectCoin DNS and fixed seeds will be added when public seed nodes exist.
        vSeeds.clear();

        // ConnectCoin-specific address, private-key, and BIP32 encodings.
        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 28);  // C...
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 87); // c...
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1, 80);  // C... (compressed WIF)
        base58Prefixes[EXT_PUBLIC_KEY] = {0xA7, 0xC7, 0x3F, 0xD9};           // ccpub...
        base58Prefixes[EXT_SECRET_KEY] = {0xA7, 0xC7, 0x3B, 0x9F};           // ccprv...

        bech32_hrp = "cc";

        vFixedSeeds.clear();

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data.clear();

        chainTxData = ChainTxData{
            // No ConnectCoin mainnet chain statistics are available yet.
            .nTime    = 0,
            .tx_count = 0,
            .dTxRate  = 0,
        };

        // Conservative development values. Regenerate with explicit ConnectCoin
        // inputs after a nontrivial minimum-chain-work policy is established.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 641,
            .redownload_buffer_size = 15218, // 15218/641 = ~23.7 commitments
        };
    }
};

/**
 * Testnet (v3): public test network which is reset from time to time.
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams(const TestNetOptions& opts) {
        m_chain_type = ChainType::TESTNET;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.genesis_coinbase_spendable = true;
        consensus.nSubsidyHalvingInterval = 450000;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"000001ffff000000000000000000000000000000000000000000000000000000"};
        consensus.nPowTargetTimespan = 24 * 60 * 60; // one day
        consensus.nPowTargetSpacing = 60;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = false;
        consensus.fPowNoRetargeting = false;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1512; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        ApplyDeploymentOptions(opts.dep_opts);

        consensus.nMinimumChainWork = uint256{1};
        consensus.defaultAssumeValid = uint256{};

        // First four bytes of SHA256("ConnectCoin testnet3 network").
        pchMessageStart = {0x03, 0x84, 0x8e, 0x59};
        nDefaultPort = 48176;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        genesis = CreateConnectCoinGenesisBlock("teste testado | ConnectCoin testnet3", 1787596782, 4355844, 0x1e01ffff, 1, 10'000'000 * COIN, PublicGenesisOutputScript());
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(genesis.hashMerkleRoot == uint256{"f69148784ca217158b5fa2dc172e6f66499535189304b27806d92cb194fb6b0f"});
        assert(consensus.hashGenesisBlock == uint256{"000001c906bb16924aaa92ab23ba23616d7916ecfdc3a256c060dbe9ead948f3"});

        vFixedSeeds.clear();
        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 65);   // T...
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 127);  // t...
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1, 178);  // T... (compressed WIF)
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x31, 0x3A, 0x97};            // tcub...
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x31, 0x33, 0x16};            // tcpr...

        bech32_hrp = "tcc";

        vFixedSeeds.clear();

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data.clear();

        chainTxData = ChainTxData{
            // No ConnectCoin testnet3 chain statistics are available yet.
            .nTime    = 0,
            .tx_count = 0,
            .dTxRate  = 0,
        };

        // Conservative development values. Regenerate with explicit ConnectCoin
        // inputs after a nontrivial minimum-chain-work policy is established.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 673,
            .redownload_buffer_size = 14460, // 14460/673 = ~21.5 commitments
        };
    }
};

/**
 * Testnet (v4): public test network which is reset from time to time.
 */
class CTestNet4Params : public CChainParams {
public:
    CTestNet4Params(const TestNetOptions& opts) {
        m_chain_type = ChainType::TESTNET4;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.genesis_coinbase_spendable = true;
        consensus.nSubsidyHalvingInterval = 450000;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"000001ffff000000000000000000000000000000000000000000000000000000"};
        consensus.nPowTargetTimespan = 24 * 60 * 60; // one day
        consensus.nPowTargetSpacing = 60;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = true;
        consensus.fPowNoRetargeting = false;

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1512; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        ApplyDeploymentOptions(opts.dep_opts);

        consensus.nMinimumChainWork = uint256{1};
        consensus.defaultAssumeValid = uint256{};

        // First four bytes of SHA256("ConnectCoin testnet4 network").
        pchMessageStart = {0xbb, 0x51, 0xf5, 0xe7};
        nDefaultPort = 48179;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        genesis = CreateConnectCoinGenesisBlock("teste testado | ConnectCoin testnet4", 1787596783, 20414323, 0x1e01ffff, 1, 10'000'000 * COIN, PublicGenesisOutputScript());
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"000001218f2321c9ccd0f18fe8603865e9a97ea644d8ab62c434cc39928377f1"});
        assert(genesis.hashMerkleRoot == uint256{"39760bb1e81f6d6b56cbf85d55f404289b842c5f30b7c652f0d2b0ac53fcd998"});

        vFixedSeeds.clear();
        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 65);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 127);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1, 178);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x31, 0x3A, 0x97};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x31, 0x33, 0x16};

        bech32_hrp = "tcc";

        vFixedSeeds.clear();

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data.clear();

        chainTxData = ChainTxData{
            // No ConnectCoin testnet4 chain statistics are available yet.
            .nTime    = 0,
            .tx_count = 0,
            .dTxRate  = 0,
        };

        // Conservative development values. Regenerate with explicit ConnectCoin
        // inputs after a nontrivial minimum-chain-work policy is established.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 606,
            .redownload_buffer_size = 16092, // 16092/606 = ~26.6 commitments
        };
    }
};

/**
 * Signet: test network with an additional consensus parameter (see BIP325).
 */
class SigNetParams : public CChainParams {
public:
    explicit SigNetParams(const SigNetOptions& options)
    {
        std::vector<uint8_t> bin;
        vFixedSeeds.clear();
        vSeeds.clear();

        if (!options.challenge) {
            // ConnectCoin's development signet is permissionless (OP_TRUE).
            // This also derives a message start distinct from Bitcoin Signet.
            bin = "51"_hex_v_u8;
        } else {
            bin = *options.challenge;
            LogInfo("Signet with challenge %s", HexStr(bin));
        }

        if (options.seeds) {
            vSeeds = *options.seeds;
        }

        m_chain_type = ChainType::SIGNET;
        consensus.signet_blocks = true;
        consensus.signet_challenge.assign(bin.begin(), bin.end());
        consensus.genesis_coinbase_spendable = true;
        consensus.nSubsidyHalvingInterval = 450000;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.nPowTargetTimespan = 24 * 60 * 60; // one day
        consensus.nPowTargetSpacing = 60;
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.enforce_BIP94 = false;
        consensus.fPowNoRetargeting = false;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"00000377ae000000000000000000000000000000000000000000000000000000"};
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1815; // 90%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        ApplyDeploymentOptions(options.dep_opts);

        consensus.nMinimumChainWork = uint256{1};
        consensus.defaultAssumeValid = uint256{};
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;
        chainTxData = ChainTxData{0, 0, 0};

        // message start is defined as the first 4 bytes of the sha256d of the block script
        HashWriter h{};
        h << consensus.signet_challenge;
        uint256 hash = h.GetHash();
        std::copy_n(hash.begin(), 4, pchMessageStart.begin());

        nDefaultPort = 48182;
        nPruneAfterHeight = 1000;

        genesis = CreateConnectCoinGenesisBlock("teste testado | ConnectCoin signet", 1787596784, 12166496, 0x1e01ffff, 1, 10'000'000 * COIN, PublicGenesisOutputScript());
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"000000850a5c90845788b6f2688e27976da0ef181258378d39764f85baa64780"});
        assert(genesis.hashMerkleRoot == uint256{"039d04fd0d0875ccefe1c4a615780b99f27f645e9b848ce059287e87ff5f0ca9"});

        m_assumeutxo_data.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 65);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 127);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1, 178);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x31, 0x3A, 0x97};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x31, 0x33, 0x16};

        bech32_hrp = "tcc";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        // Conservative development values. Regenerate with explicit ConnectCoin
        // inputs after a nontrivial minimum-chain-work policy is established.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 620,
            .redownload_buffer_size = 15724, // 15724/620 = ~25.4 commitments
        };
    }
};

/**
 * Regression test: intended for private networks only. Has minimal difficulty to ensure that
 * blocks can be found instantly.
 */
class CRegTestParams : public CChainParams
{
public:
    explicit CRegTestParams(const RegTestOptions& opts)
    {
        m_chain_type = ChainType::REGTEST;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.genesis_coinbase_spendable = true;
        consensus.nSubsidyHalvingInterval = 150;
        consensus.BIP34Height = 1; // Always active unless overridden
        consensus.BIP34Hash = uint256();
        consensus.BIP65Height = 1;  // Always active unless overridden
        consensus.BIP66Height = 1;  // Always active unless overridden
        consensus.CSVHeight = 1;    // Always active unless overridden
        consensus.SegwitHeight = 0; // Always active unless overridden
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetTimespan = 24 * 60 * 60; // one day
        consensus.nPowTargetSpacing = 10 * 60;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = opts.enforce_bip94;
        consensus.fPowNoRetargeting = true;

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 108; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 144; // Faster than normal for regtest (144 instead of 2016)

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        // First four bytes of SHA256("ConnectCoin regtest network").
        pchMessageStart = {0xa5, 0x4f, 0xc7, 0xd5};
        nDefaultPort = 48185;
        nPruneAfterHeight = opts.fastprune ? 100 : 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        ApplyDeploymentOptions(opts.dep_opts);

        genesis = CreateConnectCoinGenesisBlock("teste testado | ConnectCoin regtest", 1296688602, 0, 0x207fffff, 1, 10'000'000 * COIN, RegTestGenesisOutputScript());
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"197dd70fa5df793d1b9e4684f3c9608afcdae4b86f935c04e8187a48def347f6"});
        assert(genesis.hashMerkleRoot == uint256{"1859f50c30e835050a33fe2773c3b33c54160923d10afa0a9eba901f28fb969b"});

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();
        vSeeds.emplace_back("dummySeed.invalid.");

        fDefaultConsistencyChecks = true;
        m_is_mockable_chain = true;

        m_assumeutxo_data = {
            {
                // Regenerated for the spendable ConnectCoin regtest genesis.
                .height = 110,
                .hash_serialized = AssumeutxoHash{uint256{"cc524f6f2a786c23ada8f047c6cf72b99da1430a62949dfacdcdad9fc94e4721"}},
                .m_chain_tx_count = 111,
                .blockhash = uint256{"734c0ba8aa3d36e9b9e357b6752592b07154f0cb2830782e0415ccf3b461ce9d"},
            },
            {
                // For use by fuzz target src/test/fuzz/utxo_snapshot.cpp.
                .height = 200,
                .hash_serialized = AssumeutxoHash{uint256{"625485c6e2e46dbda2e6c4cb6b4c3ea7b665e15d8b708de7c279063496221000"}},
                .m_chain_tx_count = 201,
                .blockhash = uint256{"52f716326ae1bd1be2643d1b4bbe48ae2549d3148f8519f4af062465f6012c74"},
            },
            {
                // For use by feature_assumeutxo.py and tool_bitcoin_chainstate.py.
                .height = 299,
                .hash_serialized = AssumeutxoHash{uint256{"f08765d5aac8a793b35d644120aadc280317e0c1118f3f3ee7eaf8c958e854aa"}},
                .m_chain_tx_count = 334,
                .blockhash = uint256{"3543b4c9d2b4f69bd34634920756b94c2c1783ea43a7aea030270f800349f25f"},
            },
        };

        chainTxData = ChainTxData{
            .nTime = 0,
            .tx_count = 0,
            .dTxRate = 0.001, // Set a non-zero rate to make it testable
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 65);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 127);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1, 178);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x31, 0x3A, 0x97};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x31, 0x33, 0x16};

        bech32_hrp = "ccrt";

        // Development-only parameters used by the regtest fixtures.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 275,
            .redownload_buffer_size = 7017, // 7017/275 = ~25.5 commitments
        };
    }
};

std::unique_ptr<const CChainParams> CChainParams::SigNet(const SigNetOptions& options)
{
    return std::make_unique<const SigNetParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::RegTest(const RegTestOptions& options)
{
    return std::make_unique<const CRegTestParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::Main(const MainNetOptions& options)
{
    return std::make_unique<const CMainParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::TestNet(const TestNetOptions& options)
{
    return std::make_unique<const CTestNetParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::TestNet4(const TestNetOptions& options)
{
    return std::make_unique<const CTestNet4Params>(options);
}

std::vector<int> CChainParams::GetAvailableSnapshotHeights() const
{
    std::vector<int> heights;
    heights.reserve(m_assumeutxo_data.size());

    for (const auto& data : m_assumeutxo_data) {
        heights.emplace_back(data.height);
    }
    return heights;
}

std::optional<ChainType> GetNetworkForMagic(const MessageStartChars& message)
{
    const auto mainnet_msg = CChainParams::Main()->MessageStart();
    const auto testnet_msg = CChainParams::TestNet()->MessageStart();
    const auto testnet4_msg = CChainParams::TestNet4()->MessageStart();
    const auto regtest_msg = CChainParams::RegTest()->MessageStart();
    const auto signet_msg = CChainParams::SigNet()->MessageStart();

    if (std::ranges::equal(message, mainnet_msg)) {
        return ChainType::MAIN;
    } else if (std::ranges::equal(message, testnet_msg)) {
        return ChainType::TESTNET;
    } else if (std::ranges::equal(message, testnet4_msg)) {
        return ChainType::TESTNET4;
    } else if (std::ranges::equal(message, regtest_msg)) {
        return ChainType::REGTEST;
    } else if (std::ranges::equal(message, signet_msg)) {
        return ChainType::SIGNET;
    }
    return std::nullopt;
}
