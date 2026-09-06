// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <coins.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <netbase.h>
#include <test/util/net.h>
#include <test/util/random.h>
#include <util/translation.h>
#include <validation.h>
#include <wallet/p2c_claim.h>
#include <wallet/p2c_tls.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace wallet {
namespace {
struct RestoreSocketFactory {
    const decltype(CreateSock) original{CreateSock};
    ~RestoreSocketFactory() { CreateSock = original; }
};

class ClientHelloSocket final : public ZeroSock {
    std::vector<unsigned char>& m_sent;
    const size_t m_chunk;
public:
    ClientHelloSocket(std::vector<unsigned char>& sent, size_t chunk) : m_sent(sent), m_chunk(chunk) {}
    ssize_t Send(const void* data, size_t size, int) const override
    {
        const auto length{std::min(size, m_chunk)};
        const auto* bytes{static_cast<const unsigned char*>(data)};
        m_sent.insert(m_sent.end(), bytes, bytes + length);
        return length;
    }
    ssize_t Recv(void*, size_t, int) const override { return 0; } // EOF, no real server/network.
};
}

BOOST_FIXTURE_TEST_SUITE(p2c_claim_tests, WalletTestingSetup)

BOOST_AUTO_TEST_CASE(tls_client_random_and_fresh_keyshare)
{
    RestoreSocketFactory restore;
    const auto endpoint{Lookup("8.8.8.8", 443, false)};
    BOOST_REQUIRE(endpoint);
    const uint256 challenge{uint256::ONE};
    std::vector<unsigned char> previous_keyshare;
    // Exercise successful partial sends as well as a single complete record.
    for (const size_t chunk : {size_t{7}, size_t{16384}}) {
        std::vector<unsigned char> sent;
        CreateSock = [&](int, int, int) { return std::make_unique<ClientHelloSocket>(sent, chunk); };
        auto result{CaptureP2CTls(*endpoint, "example.com", challenge, [] { return false; })};
        BOOST_REQUIRE(!result); // EOF cannot produce a valid proof.
        BOOST_REQUIRE(sent.size() > 44);
        BOOST_CHECK_EQUAL(sent[0], 22);
        BOOST_CHECK_EQUAL(sent[5], 1);
        BOOST_CHECK_EQUAL(size_t{sent[3]} * 256 + sent[4] + 5, sent.size());
        BOOST_CHECK_EQUAL_COLLECTIONS(sent.begin() + 11, sent.begin() + 43, challenge.begin(), challenge.end());
        // Parse the actual wire ClientHello and compare key_share extensions.
        // Fixing random MUST NOT fix ephemeral key generation as a side effect.
        size_t pos{43};
        const auto read8 = [&]() -> size_t { BOOST_REQUIRE(pos < sent.size()); return sent[pos++]; };
        const auto read16 = [&]() -> size_t { const auto high{read8()}; return high * 256 + read8(); };
        const auto skip = [&](size_t length) { BOOST_REQUIRE(length <= sent.size() - pos); pos += length; };
        skip(read8()); // legacy session id
        skip(read16()); // cipher suites
        skip(read8()); // compression
        const auto extensions_length{read16()};
        BOOST_REQUIRE_EQUAL(extensions_length, sent.size() - pos);
        std::vector<unsigned char> keyshare;
        while (pos < sent.size()) {
            const auto type{read16()};
            const auto length{read16()};
            BOOST_REQUIRE(length <= sent.size() - pos);
            if (type == 51) keyshare.assign(sent.begin() + pos, sent.begin() + pos + length);
            skip(length);
        }
        BOOST_REQUIRE(keyshare.size() >= 38);
        if (!previous_keyshare.empty()) BOOST_CHECK(keyshare != previous_keyshare);
        previous_keyshare = std::move(keyshare);
    }
}

BOOST_AUTO_TEST_CASE(tls_cancellation_and_private_endpoint_never_connect)
{
    RestoreSocketFactory restore;
    unsigned calls{0};
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> { ++calls; return nullptr; };
    const auto public_endpoint{Lookup("8.8.8.8", 443, false)};
    const auto private_endpoint{Lookup("127.0.0.1", 443, false)};
    BOOST_REQUIRE(public_endpoint && private_endpoint);
    BOOST_CHECK(!CaptureP2CTls(*public_endpoint, "example.com", uint256{}, [] { return true; }));
    BOOST_CHECK(!CaptureP2CTls(*private_endpoint, "example.com", uint256{}, [] { return false; }));
    BOOST_CHECK_EQUAL(calls, 0U);
}

BOOST_AUTO_TEST_CASE(resumed_proposal_revalidates_amounts_and_ownership)
{
    LOCK(m_wallet.cs_wallet);
    m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    const auto key{GenerateRandomKey()};
    BOOST_REQUIRE(CreateDescriptor(m_wallet, "rawtr(" + EncodeSecret(key) + ")", true));
    const COutPoint outpoint{Txid::FromUint256(uint256::ONE), 0};
    {
        LOCK(cs_main);
        auto& state{m_node.chainman->ActiveChainstate()};
        m_wallet.SetLastBlockProcessed(state.m_chain.Height(), state.m_chain.Tip()->GetBlockHash());
        state.CoinsTip().AddCoin(outpoint, Coin{CTxOut{COIN, PayToDomainOutput{"example.com", uint256{}, 1}}, 0, false}, false);
    }
    CMutableTransaction tx;
    tx.vin.emplace_back(outpoint);
    tx.vout.emplace_back(COIN - 1000, XOnlyPubKey{key.GetPubKey()});
    BOOST_REQUIRE(ResumeP2CClaim(m_wallet, CTransaction{tx}));
    for (const CAmount value : {std::numeric_limits<CAmount>::min(), CAmount{-1}, CAmount{0}, COIN + 1, std::numeric_limits<CAmount>::max()}) {
        tx.vout[0].nValue = value;
        auto result{ResumeP2CClaim(m_wallet, CTransaction{tx})};
        BOOST_REQUIRE(!result);
        BOOST_CHECK(util::ErrorString(result).original.find("payout amount") != std::string::npos);
    }
    tx.vout[0] = CTxOut{COIN - 1000, XOnlyPubKey{GenerateRandomKey().GetPubKey()}};
    BOOST_CHECK(!ResumeP2CClaim(m_wallet, CTransaction{tx}));
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().SpendCoin(outpoint));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
