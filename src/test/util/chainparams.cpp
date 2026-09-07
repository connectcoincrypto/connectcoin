// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <test/util/chainparams.h>

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <crypto/hex_base.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <cassert>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace util::hex_literals;

class MainNetTestParams : public CChainParams
{
public:
    explicit MainNetTestParams(const CChainParams& params) : CChainParams{params}
    {
        assert(GetChainType() == ChainType::MAIN);
        assert(!HasGenesisBlock());
        // Quarantine the retired development genesis here to preserve existing
        // consensus/storage vectors. This is NOT a mainnet launch commitment.
        constexpr std::string_view timestamp{"teste testado"};
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.vin.resize(1);
        coinbase.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4)
            << std::vector<unsigned char>{timestamp.begin(), timestamp.end()};
        coinbase.vout.emplace_back(10'000'000 * COIN, CScript() << OP_1
            << "da12a44d69673e42ba95ac1d2bd4e5c76c3709a1765edbc5f52b8e5e643b0609"_hex);
        CBlock block;
        block.nVersion = 1;
        block.nTime = 1787596781;
        block.nBits = 0x1f00ffff;
        block.nNonce = 37316;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        consensus.hashGenesisBlock = block.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"8b6373205ad2b6314f2937cebacfc143af9eb6183162c24fb19cdf382ff576c5"});
        genesis = std::move(block);
    }
};
} // namespace

std::unique_ptr<const CChainParams> CreateChainParamsForTest(const ArgsManager& args, ChainType chain)
{
    auto params{CreateChainParams(args, chain)};
    // Store the completed value as the base type: CChainParams has no virtual
    // destructor, so the owning pointer must not delete a derived object.
    if (chain == ChainType::MAIN) return std::make_unique<const CChainParams>(MainNetTestParams{*params});
    return params;
}
