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
#include <signet.h>
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
#include <stdexcept>
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
    txNew.vout[0].SetScriptPubKey(genesisOutputScript);

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

/** Historical testnet3/signet development-fund key. No private key is in the source tree. */
static const CScript& PublicGenesisOutputScript()
{
    static const CScript script = CScript() << OP_1 << "da12a44d69673e42ba95ac1d2bd4e5c76c3709a1765edbc5f52b8e5e643b0609"_hex;
    return script;
}

/** Testnet4 beta fund key, backed by a locally generated wallet; public material only. */
static const CScript& TestNet4GenesisOutputScript()
{
    static const CScript script = CScript() << OP_1 << "2ef316afd6177619f68ecfc6521fc3fcbf7faa2b25273f6ddea7971fae0de144"_hex;
    return script;
}

/** Regtest-only key with a published private key, so tests can spend its genesis output. */
static const CScript& RegTestGenesisOutputScript()
{
    static const CScript script = CScript() << OP_1 << "738a50e0af6185956d5e0c393859830eb70ea92351b19facbd47259cd7a10c27"_hex;
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
        consensus.nSubsidyHalvingInterval = 3'000'000;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"0000ffff00000000000000000000000000000000000000000000000000000000"};
        consensus.randomx_bootstrap_key = uint256{"d91b262aecaac2c4868b2cbe1563538f107c33fbee8c5d373bdaa8e551567fe5"};
        consensus.randomx_epoch_blocks = 12'288;
        consensus.randomx_epoch_lag = 64;
        consensus.randomx_fast_mode = opts.randomx_fast;
        consensus.nPowTargetTimespan = 24 * 60 * 60; // one day
        consensus.nPowTargetSpacing = 10;
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
        m_wallet_database_id = {0xd9, 0x51, 0xa5, 0xe2};
        nDefaultPort = 48173;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        // Mainnet has not been launched. Do not embed a provisional genesis:
        // nodes must refuse to initialize this chain until its launch genesis
        // is defined. The retired development block lives only in test_util.
        assert(!HasGenesisBlock());
        assert(consensus.hashGenesisBlock.IsNull());

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
        consensus.nSubsidyHalvingInterval = 3'000'000;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"0000ffff00000000000000000000000000000000000000000000000000000000"};
        consensus.randomx_bootstrap_key = uint256{"50baa475cea61a44ccd0bda1646a957e11d4e973746b47384a299e2003f8732a"};
        consensus.randomx_epoch_blocks = 12'288;
        consensus.randomx_epoch_lag = 64;
        consensus.randomx_fast_mode = opts.randomx_fast;
        consensus.nPowTargetTimespan = 24 * 60 * 60; // one day
        consensus.nPowTargetSpacing = 10;
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

        // First four bytes of SHA256("ConnectCoin testnet3 P2C mask v1 network").
        pchMessageStart = {0xc7, 0x29, 0x1f, 0xf5};
        // Preserve the pre-reset wallet identity, not the wire protocol magic.
        m_wallet_database_id = {0x03, 0x84, 0x8e, 0x59};
        nDefaultPort = 48176;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        genesis = CreateConnectCoinGenesisBlock("ConnectCoin testnet3 | P2C mask v1 | 2026-09-09", 1788912000, 66621, 0x1f00ffff, 1, 10'000'000 * COIN, PublicGenesisOutputScript());
        consensus.hashGenesisBlock = genesis->GetHash();
        assert(genesis->hashMerkleRoot == uint256{"2e2e1a9fdad577fc8832a2e582f0149de3ab2032764a978c7e469b18f1c4099e"});
        assert(consensus.hashGenesisBlock == uint256{"1025889d725c5d64c3ee38ab07d2de279ab57036a2482186c65806d6c0291787"});

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
        consensus.nSubsidyHalvingInterval = 3'000'000;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"0000ffff00000000000000000000000000000000000000000000000000000000"};
        consensus.randomx_bootstrap_key = uint256{"5f594825538aa326f645f024473b140479f43dd072714ba353341d69178616ab"};
        consensus.randomx_epoch_blocks = 12'288;
        consensus.randomx_epoch_lag = 64;
        consensus.randomx_fast_mode = opts.randomx_fast;
        consensus.nPowTargetTimespan = 24 * 60 * 60; // one day
        consensus.nPowTargetSpacing = 10;
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

        // First four bytes of SHA256("ConnectCoin testnet4 P2C mask v1 network").
        pchMessageStart = {0x77, 0xd6, 0x6c, 0xbc};
        m_wallet_database_id = {0xbb, 0x51, 0xf5, 0xe7};
        nDefaultPort = 48179;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        genesis = CreateConnectCoinGenesisBlock("ConnectCoin testnet4 | P2C mask v1 | 2026-09-09", 1788912001, 913, 0x1f00ffff, 1, 10'000'000 * COIN, TestNet4GenesisOutputScript());
        consensus.hashGenesisBlock = genesis->GetHash();
        assert(consensus.hashGenesisBlock == uint256{"710dc5910cbef40216bd82ccfb66af2273b2b1d336b034c5794966904cb603bf"});
        assert(genesis->hashMerkleRoot == uint256{"e09a12d2aca740a06be984897fa268d4f03317c2363748d4ab69768ed92ca555"});

        vFixedSeeds.clear();
        // ConnectCoin public testnet4 bootstrap (base hostnames, including DDNS).
        vSeeds.emplace_back("connectcoin1.com");
        vSeeds.emplace_back("connectcoin2.com");
        vSeeds.emplace_back("connectcoin3.com");
        vSeeds.emplace_back("dememzea.tplinkdns.com");

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

        if (!IsTrivialSignetChallenge(bin)) {
            throw std::runtime_error("-signetchallenge must be a trivial truthy script that needs no scriptSig or witness; arbitrary BIP325 Script challenges are incompatible with ConnectCoin typed outputs.");
        }

        if (options.seeds) {
            vSeeds = *options.seeds;
        }

        m_chain_type = ChainType::SIGNET;
        consensus.signet_blocks = true;
        consensus.signet_challenge.assign(bin.begin(), bin.end());
        consensus.genesis_coinbase_spendable = true;
        consensus.nSubsidyHalvingInterval = 3'000'000;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.nPowTargetTimespan = 24 * 60 * 60; // one day
        consensus.nPowTargetSpacing = 10;
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.enforce_BIP94 = false;
        consensus.fPowNoRetargeting = false;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"0000ffff00000000000000000000000000000000000000000000000000000000"};
        consensus.randomx_bootstrap_key = uint256{"34b9e892605c061b7c1846232f615e1a3118e4bf859afe1a5a4a0cb89300f5db"};
        consensus.randomx_epoch_blocks = 12'288;
        consensus.randomx_epoch_lag = 64;
        consensus.randomx_fast_mode = options.randomx_fast;
        consensus.randomx_mock_pow = options.randomx_mock_pow;
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

        // Wallet identity keeps the original SHA256d(serialized challenge),
        // independent of network resets but still isolated by custom challenge.
        HashWriter wallet_id_hash{};
        wallet_id_hash << consensus.signet_challenge;
        const uint256 wallet_id = wallet_id_hash.GetHash();
        std::copy_n(wallet_id.begin(), 4, m_wallet_database_id.begin());

        // Domain-separate the reset network while preserving custom-challenge isolation.
        // First four bytes of SHA256d of the serialized string and challenge vector.
        HashWriter h{};
        h << std::string{"ConnectCoin signet P2C mask v1 network"};
        h << consensus.signet_challenge;
        uint256 hash = h.GetHash();
        std::copy_n(hash.begin(), 4, pchMessageStart.begin());

        nDefaultPort = 48182;
        nPruneAfterHeight = 1000;

        genesis = CreateConnectCoinGenesisBlock("ConnectCoin signet | P2C mask v1 | 2026-09-09", 1788912002, 2069, 0x1f00ffff, 1, 10'000'000 * COIN, PublicGenesisOutputScript());
        consensus.hashGenesisBlock = genesis->GetHash();
        assert(consensus.hashGenesisBlock == uint256{"a694dccdc04a316a4f4fe496f311aff981392f25ea18e4b7f77d9f449b9089fc"});
        assert(genesis->hashMerkleRoot == uint256{"e9b9c33924f02701bc84751dc4701a1eef9b5c865fdc030b34843ed45db74dbf"});

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
        consensus.randomx_bootstrap_key = uint256{"a31f20a588a3c9bc5657fba84b9460591a8c661a72d046b3356ae83600da3dc8"};
        consensus.randomx_epoch_blocks = 0;
        consensus.randomx_epoch_lag = 0;
        consensus.randomx_fast_mode = opts.randomx_fast;
        consensus.randomx_mock_pow = opts.randomx_mock_pow;
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

        // First four bytes of SHA256("ConnectCoin regtest P2C mask v1 network").
        pchMessageStart = {0x3a, 0xf8, 0x3b, 0xe3};
        m_wallet_database_id = {0xa5, 0x4f, 0xc7, 0xd5};
        nDefaultPort = 48185;
        nPruneAfterHeight = opts.fastprune ? 100 : 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        ApplyDeploymentOptions(opts.dep_opts);

        // Keep the historical regtest clock for tests; the new coinbase and nonce reset its identity.
        genesis = CreateConnectCoinGenesisBlock("ConnectCoin regtest | P2C mask v1 | 2026-09-09", 1296688602, 20, 0x207fffff, 1, 10'000'000 * COIN, RegTestGenesisOutputScript());
        consensus.hashGenesisBlock = genesis->GetHash();
        assert(consensus.hashGenesisBlock == uint256{"53c5145452f6957a2674ab904726afc2d7643c4a4fb9c2beab193ea983e500f0"});
        assert(genesis->hashMerkleRoot == uint256{"9fee3081f6d76758b63691d412ce44408c471b02d0e5ae443aa5adf3a7345c9e"});

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();
        vSeeds.emplace_back("dummySeed.invalid.");

        fDefaultConsistencyChecks = true;
        m_is_mockable_chain = true;

        m_assumeutxo_data = {
            {
                // Deterministic snapshot used by the chainstate manager unit tests.
                .height = 110,
                .hash_serialized = AssumeutxoHash{uint256{"4f999da88f2d55aaf4d79d92565f2c03ec5c50dbf04ad48fbc4c809ec9075ea7"}},
                .m_chain_tx_count = 111,
                .blockhash = opts.randomx_mock_pow
                    ? uint256{"74aa335863dad6125b1eed3dffb2312aaab36057d2be88a393c93b15b4eae1c0"}
                    : uint256{"cc663f573619d2c6ecb84ce51d3cda51efe9fc70fc241f153ca2e2dd8e695455"},
            },
            {
                // Deterministic regtest commitment used by the utxo_snapshot fuzz targets.
                .height = 200,
                .hash_serialized = AssumeutxoHash{uint256{"9bf1e57fe8fbaea995248beee3f679ad7fb9dc47cbdca2388b285689aef1ea93"}},
                .m_chain_tx_count = 201,
                .blockhash = opts.randomx_mock_pow
                    ? uint256{"686ec6f0097a0c42be14d672b2c6b336c93224c183d59872aeff473f7f99bc6a"}
                    : uint256{"03085c6cd5ab716a533e4dbd23d01792960047a71fa273b6be3b8f7f4f787d13"},
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
