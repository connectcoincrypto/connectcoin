// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <coins.h>
#include <consensus/p2c_x509.h>
#include <core_io.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <netbase.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <test/util/net.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>
#include <univalue.h>
#include <util/translation.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/p2c_claim.h>
#include <wallet/p2c_domain_stats.h>
#include <wallet/p2c_tls.h>
#include <wallet/p2c_worker.h>
#include <wallet/test/p2c_tls_fixture.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace wallet {
namespace {
struct RestoreSocketFactory {
    const decltype(CreateSock) original{CreateSock};
    ~RestoreSocketFactory() { CreateSock = original; }
};

struct RestoreDNSLookup {
    const DNSLookupFn original{g_dns_lookup};
    ~RestoreDNSLookup() { g_dns_lookup = original; }
};

class ClientHelloSocket final : public ZeroSock {
    ClientHelloSocket& operator=(Sock&&) override { throw std::logic_error("Move of Sock into ClientHelloSocket not allowed"); }
    std::vector<unsigned char>& m_sent;
    const size_t m_chunk;
    const std::function<void()> m_eof;
public:
    ClientHelloSocket(std::vector<unsigned char>& sent, size_t chunk, std::function<void()> eof = {})
        : m_sent(sent), m_chunk(chunk), m_eof(std::move(eof)) {}
    ssize_t Send(const void* data, size_t size, int) const override
    {
        const auto length{std::min(size, m_chunk)};
        const auto* bytes{static_cast<const unsigned char*>(data)};
        m_sent.insert(m_sent.end(), bytes, bytes + length);
        return length;
    }
    ssize_t Recv(void*, size_t, int) const override
    {
        if (m_eof) m_eof();
        return 0; // EOF, no real server/network.
    }
};
}

BOOST_FIXTURE_TEST_SUITE(p2c_claim_tests, WalletTestingSetup)

BOOST_AUTO_TEST_CASE(domain_connection_statistics_use_last_100_attempts)
{
    P2CDomainStats stats;
    BOOST_CHECK_EQUAL(stats.Attempts(), 0U);
    BOOST_CHECK_EQUAL(stats.ConnectionRate(), 5.0);
    stats.Record(true, 0.2);
    stats.Record(false, 0.8);
    BOOST_CHECK_EQUAL(stats.Totals().first, 1U);
    BOOST_CHECK_EQUAL(stats.Totals().second, 1.0);
    BOOST_CHECK_CLOSE(stats.ConnectionRate(), 1.1 / 1.02, 1e-10);
    for (size_t i = 0; i < 98; ++i) stats.Record(false, 0.0);
    BOOST_CHECK_EQUAL(stats.Attempts(), 100U);
    BOOST_CHECK_EQUAL(stats.Totals().first, 1U);
    stats.Record(false, 0.0); // Evict the success, not the oldest failure.
    BOOST_CHECK_EQUAL(stats.Totals().first, 0U);
    BOOST_CHECK_CLOSE(stats.Totals().second, 0.8, 1e-10);
    stats.Record(true, 0.1); // Evict the slow failure too.
    BOOST_CHECK_EQUAL(stats.Attempts(), 100U);
    BOOST_CHECK_EQUAL(stats.Totals().first, 1U);
    BOOST_CHECK_CLOSE(stats.ConnectionRate(), 1.1 / 0.12, 1e-10);
    for (const double invalid : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        stats.Record(true, invalid);
    }
    BOOST_CHECK_EQUAL(stats.Attempts(), 100U);
    BOOST_CHECK_EQUAL(stats.Totals().first, 1U);

    // Compare repeated rollover to an independent last-100 reference. No
    // subtractive cancellation after a very old, very slow attempt expires.
    stats.Record(false, 1e12);
    std::vector<std::pair<bool, double>> reference;
    for (size_t i = 0; i < 1000; ++i) {
        const bool success{i % 3 == 0};
        const double seconds{1e-6 * (i % 7 + 1)};
        reference.emplace_back(success, seconds);
        stats.Record(success, seconds);
        if (reference.size() < 100) continue;
        size_t successes{0};
        double total{0};
        for (auto it = reference.end() - 100; it != reference.end(); ++it) {
            successes += it->first;
            total += it->second;
        }
        BOOST_CHECK_EQUAL(stats.Attempts(), 100U);
        BOOST_CHECK_EQUAL(stats.Totals().first, successes);
        BOOST_CHECK_EQUAL(stats.Totals().second, total);
        BOOST_CHECK_EQUAL(stats.ConnectionRate(), (0.1 + successes) / (0.02 + total));
    }
}

BOOST_AUTO_TEST_CASE(domain_priority_rewards_success_and_low_latency)
{
    P2CDomainStats fresh, fast, slow, unreliable;
    for (size_t i = 0; i < 100; ++i) {
        fast.Record(true, 0.1);
        slow.Record(true, 0.5);
        unreliable.Record(i % 5 == 0, 0.1);
    }
    BOOST_CHECK(fast.ConnectionRate() > fresh.ConnectionRate());
    BOOST_CHECK(fresh.ConnectionRate() > slow.ConnectionRate());
    BOOST_CHECK(fast.ConnectionRate() > unreliable.ConnectionRate());
    const auto maximum{uint256::FromHex(std::string(64, 'f')).value()};
    const auto economic{GetP2CClaimPriority(maximum, COIN)};
    const auto richer{GetP2CClaimPriority(maximum, 2 * COIN)};
    BOOST_CHECK_EQUAL(GetP2CDomainPriority(economic, fresh.ConnectionRate()), 5.0 * COIN);
    BOOST_CHECK(GetP2CDomainPriority(economic, fast.ConnectionRate()) > GetP2CDomainPriority(richer, slow.ConnectionRate()));
    BOOST_CHECK(GetP2CDomainPriority(richer, fast.ConnectionRate()) > GetP2CDomainPriority(economic, fast.ConnectionRate()));
    BOOST_CHECK(GetP2CDomainPriority(economic, fast.ConnectionRate()) > GetP2CDomainPriority(economic, unreliable.ConnectionRate()));
    BOOST_CHECK(GetP2CDomainPriority(GetP2CClaimPriority(uint256{}, 1), slow.ConnectionRate()) > 0);
    BOOST_CHECK_EQUAL(GetP2CDomainPriority(GetP2CClaimPriority(maximum, 0), fast.ConnectionRate()), 0);
    for (size_t i = 0; i < 100; ++i) fast.Record(true, 0.0);
    BOOST_CHECK_EQUAL(fast.ConnectionRate(), 5005.0);
    BOOST_CHECK(std::isfinite(GetP2CDomainPriority(GetP2CClaimPriority(maximum, MAX_MONEY), fast.ConnectionRate())));
}

BOOST_AUTO_TEST_CASE(expected_return_priority_is_exact)
{
    const uint256 maximum{uint256::FromHex(std::string(64, 'f')).value()};
    const auto zero{GetP2CClaimPriority(uint256{}, 0)};
    for (const CAmount payout : {CAmount{1}, COIN, MAX_MONEY}) {
        auto expected{zero};
        expected[8] = static_cast<uint32_t>(static_cast<uint64_t>(payout) >> 32);
        expected[9] = static_cast<uint32_t>(static_cast<uint64_t>(payout) & 0xffffffff);
        BOOST_CHECK(GetP2CClaimPriority(uint256{}, payout) == expected);
        expected[0] = expected[8];
        expected[1] = expected[9];
        expected[8] = expected[9] = 0;
        BOOST_CHECK(GetP2CClaimPriority(maximum, payout) == expected);
    }
    for (const auto payout : {std::numeric_limits<CAmount>::min(), CAmount{-1}, CAmount{0}, MAX_MONEY + 1, std::numeric_limits<CAmount>::max()}) {
        BOOST_CHECK(GetP2CClaimPriority(maximum, payout) == zero);
    }
    const uint256 half{uint256::FromHex("7" + std::string(63, 'f')).value()};
    BOOST_CHECK(GetP2CClaimPriority(half, 2) == GetP2CClaimPriority(maximum, 1));
    BOOST_CHECK(GetP2CClaimPriority(maximum, MAX_MONEY) > GetP2CClaimPriority(maximum, MAX_MONEY - 1));
    // Smaller gross bounty can win on difficulty; fees can reverse gross ordering.
    BOOST_CHECK(GetP2CClaimPriority(maximum, 3 * COIN) > GetP2CClaimPriority(half, 5 * COIN));
    BOOST_CHECK(GetP2CClaimPriority(half, 9 * COIN - COIN) > GetP2CClaimPriority(half, 10 * COIN - 5 * COIN));

    // Independent bytewise shift/add oracle (no multiplication or 32-bit limbs).
    for (int trial = 0; trial < 256; ++trial) {
        const auto target{m_rng.rand256()};
        const CAmount payout{m_rng.randrange(MAX_MONEY) + 1};
        std::array<unsigned char, 40> factor{}, expected{};
        std::copy(target.begin(), target.end(), factor.begin());
        unsigned carry{1};
        for (auto& byte : factor) {
            const unsigned sum{byte + carry};
            byte = static_cast<unsigned char>(sum & 255);
            carry = sum >> 8;
        }
        for (unsigned bit = 0; bit < 63; ++bit) {
            if ((static_cast<uint64_t>(payout) >> bit) & 1) {
                carry = 0;
                for (size_t i = 0; i < expected.size(); ++i) {
                    const unsigned sum{expected[i] + factor[i] + carry};
                    expected[i] = static_cast<unsigned char>(sum & 255);
                    carry = sum >> 8;
                }
                BOOST_CHECK_EQUAL(carry, 0U);
            }
            carry = 0;
            for (auto& byte : factor) {
                const unsigned shifted{2U * byte + carry};
                byte = static_cast<unsigned char>(shifted & 255);
                carry = shifted >> 8;
            }
        }
        const auto actual{GetP2CClaimPriority(target, payout)};
        for (size_t i = 0; i < expected.size(); ++i) {
            BOOST_CHECK_EQUAL((actual[9 - i / 4] >> (8 * (i % 4))) & 255, expected[i]);
        }
    }
}

BOOST_AUTO_TEST_CASE(claim_fee_quote_matches_wire_transaction)
{
    const CFeeRate rate{1234567};
    for (const size_t proof_size : {size_t{1}, size_t{252}, size_t{253}, size_t{65535}, size_t{65536}}) {
        CMutableTransaction tx;
        tx.vin.emplace_back(COutPoint{Txid::FromUint256(m_rng.rand256()), 99});
        tx.vout.emplace_back(COIN, XOnlyPubKey{GenerateRandomKey().GetPubKey()});
        tx.vin[0].scriptWitness.stack.emplace_back(proof_size, 0);
        const auto quote{CalculateP2CClaimFee(rate, proof_size)};
        BOOST_REQUIRE(quote);
        BOOST_CHECK_EQUAL(*quote, rate.GetFee(static_cast<int32_t>(GetVirtualTransactionSize(CTransaction{tx}))));
    }
    BOOST_CHECK(!CalculateP2CClaimFee(rate, 0));
    BOOST_CHECK(!CalculateP2CClaimFee(rate, MAX_P2C_PROOF_SIZE + 1));
    BOOST_CHECK(!CalculateP2CClaimFee(CFeeRate{-1}));
    BOOST_CHECK(!CalculateP2CClaimFee(CFeeRate{std::numeric_limits<CAmount>::max()}));
}

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

BOOST_FIXTURE_TEST_CASE(worker_retries_dns_reuses_endpoints_and_stops, TestChain100Setup)
{
    using namespace std::chrono_literals;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    const COutPoint outpoint{Txid::FromUint256(uint256::ONE), 0};
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoint,
        Coin{CTxOut{COIN, PayToDomainOutput{"example.com", uint256{}, 1}}, 100, false}, false));
    WITH_LOCK(wallet->cs_wallet, BOOST_REQUIRE(wallet->LockCoin(outpoint, false)));
    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    std::atomic<unsigned> lookups{0}, sockets{0};
    g_dns_lookup = [&](const std::string&, bool) { ++lookups; return std::vector<CNetAddr>{}; };
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> { ++sockets; return nullptr; };
    const auto wait_until = [](const auto& predicate) {
        const auto deadline{std::chrono::steady_clock::now() + 30s};
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(10ms);
        }
        return true;
    };
    auto worker{MakeP2CClaimWorker(*wallet)};
    BOOST_REQUIRE(worker->Configure(-1, 1));
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["state"].get_str() == "waiting for eligible bounties"; }));
    BOOST_CHECK_EQUAL(lookups.load(), 0U);
    WITH_LOCK(wallet->cs_wallet, wallet->UnlockCoin(outpoint));
    BOOST_REQUIRE(wait_until([&] { return lookups >= 2; }));
    const auto before_stop{std::chrono::steady_clock::now()};
    worker->Stop();
    BOOST_CHECK(std::chrono::steady_clock::now() - before_stop < 3s);
    BOOST_CHECK_EQUAL(sockets.load(), 0U);

    const auto address{LookupHost("8.8.8.8", false, restore_dns.original)};
    BOOST_REQUIRE(address);
    lookups = 0;
    g_dns_lookup = [&, address = *address](const std::string&, bool) { ++lookups; return std::vector<CNetAddr>{address}; };
    BOOST_REQUIRE(worker->Configure(-1, 16));
    BOOST_REQUIRE(wait_until([&] { return sockets > 64; }));
    worker->Stop();
    BOOST_CHECK_EQUAL(lookups.load(), 1U); // Not once per connection/refresh.
    BOOST_CHECK_EQUAL(worker->Status()["concurrency"].getInt<int>(), 16);

    // More than 64 connections may actually run at once; no real sockets.
    struct Pending {
        std::mutex mutex;
        std::condition_variable wake;
        unsigned active{0};
        bool released{false};
        void Release() { { std::lock_guard lock{mutex}; released = true; } wake.notify_all(); }
    };
    const auto pending{std::make_shared<Pending>()};
    struct Release {
        std::shared_ptr<Pending> pending;
        ~Release() { pending->Release(); }
    } release{pending};
    CreateSock = [pending](int, int, int) -> std::unique_ptr<Sock> {
        std::unique_lock lock{pending->mutex};
        ++pending->active;
        pending->wake.wait(lock, [&] { return pending->released; });
        --pending->active;
        return nullptr;
    };
    BOOST_REQUIRE(worker->Configure(-1, 65));
    const bool reached{wait_until([&] { std::lock_guard lock{pending->mutex}; return pending->active == 65; })};
    pending->Release();
    worker->Stop();
    BOOST_CHECK(reached);
    for (int i = 0; i < 3; ++i) {
        BOOST_REQUIRE(worker->Configure(1, std::numeric_limits<int>::max(), {"unfunded.example"}));
        BOOST_REQUIRE(wait_until([&] { return worker->Status()["state"].get_str() == "waiting for bounties"; }));
        const auto before{std::chrono::steady_clock::now()};
        worker->Stop();
        BOOST_CHECK(std::chrono::steady_clock::now() - before < 3s);
        BOOST_CHECK_EQUAL(worker->Status()["connections_per_second"].getInt<int>(), 0);
    }
    worker->Shutdown();
    BOOST_CHECK(!worker->Configure(-1, 4));
}

BOOST_FIXTURE_TEST_CASE(worker_rotates_each_connection_and_reaches_large_domain_tail, TestChain100Setup)
{
    using namespace std::chrono_literals;
    FakeSteadyClock clock; // Never advance: rotation cannot depend on a timer.
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    const std::vector<std::string> names{"alpha.example", "beta.example", "gamma.example"};
    for (uint32_t i = 0; i < 262; ++i) {
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(
            COutPoint{Txid::FromUint256(uint256::ONE), i},
            Coin{CTxOut{i < 260 ? 100'000 * COIN + i * (COIN / 1000) : COIN,
                PayToDomainOutput{names[i < 260 ? 0 : i - 259], uint256{}, 1}}, 100, false}, false));
    }
    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    const auto address{LookupHost("8.8.8.8", false, restore_dns.original)};
    BOOST_REQUIRE(address);
    std::atomic<unsigned> lookups{0};
    g_dns_lookup = [&](const std::string&, bool) { ++lookups; return std::vector<CNetAddr>{*address}; };
    std::mutex events_mutex;
    std::vector<std::pair<std::string, uint32_t>> events;
    std::set<uint32_t> alpha_outputs;
    bool unknown_challenge{false};
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        auto sent{std::make_shared<std::vector<unsigned char>>()};
        return std::make_unique<ClientHelloSocket>(*sent, 16384, [&, sent] {
            if (sent->size() < 43) return;
            std::string name;
            for (const auto& candidate : names) {
                if (std::search(sent->begin(), sent->end(), candidate.begin(), candidate.end()) != sent->end()) name = candidate;
            }
            std::string encoded;
            UniValue saved;
            uint32_t index{9999};
            if (wallet->GetDatabase().MakeBatch()->Read(std::string{"p2c_claim_worker_v1"}, encoded) && saved.read(encoded)) {
                for (const auto& item : saved["pending"].getValues()) {
                    CMutableTransaction tx;
                    if (!DecodeHexTx(tx, item.get_str())) continue;
                    const auto challenge{P2CClaimChallenge(CTransaction{tx}, 0)};
                    if (std::equal(challenge.begin(), challenge.end(), sent->begin() + 11)) index = tx.vin[0].prevout.n;
                }
            }
            std::lock_guard lock{events_mutex};
            if (name.empty() || index == 9999) unknown_challenge = true;
            events.emplace_back(name, index);
            if (name == names[0]) alpha_outputs.insert(index);
        });
    };
    auto worker{MakeP2CClaimWorker(*wallet)};
    BOOST_REQUIRE(worker->Configure(-1, 32));
    const auto deadline{std::chrono::steady_clock::now() + 45s};
    bool reached{false};
    while (std::chrono::steady_clock::now() < deadline) {
        { std::lock_guard lock{events_mutex}; reached = alpha_outputs.size() == 260; }
        if (reached) break;
        std::this_thread::sleep_for(10ms);
    }
    worker->Stop();
    BOOST_REQUIRE(reached);
    BOOST_CHECK(!unknown_challenge);
    BOOST_CHECK_EQUAL(lookups.load(), 3U);
    BOOST_REQUIRE(events.size() > 300);
    std::map<std::string, unsigned> counts;
    for (size_t i = 0; i < std::min<size_t>(events.size(), 64); ++i) ++counts[events[i].first];
    BOOST_CHECK(counts[names[1]] > 0); // No 30-second monopolization.
    BOOST_CHECK(counts[names[2]] > 0);
    counts.clear();
    for (const auto& [name, index] : events) ++counts[name];
    BOOST_CHECK(counts[names[0]] > counts[names[1]] * 2); // Reward still matters.
    BOOST_CHECK(counts[names[0]] > counts[names[2]] * 2);
    // Bounties beyond the 256-entry proposal cache were reached.
    BOOST_CHECK(alpha_outputs.contains(0));
    BOOST_CHECK(alpha_outputs.contains(259));
}

BOOST_FIXTURE_TEST_CASE(worker_prioritizes_difficulty_and_saved_net_payout, TestChain100Setup)
{
    using namespace std::chrono_literals;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    wallet->m_default_max_tx_fee = MAX_MONEY;
    const std::array<CAmount, 3> rewards{10 * COIN, 9 * COIN, 4 * COIN};
    const std::array<CAmount, 3> payouts{5 * COIN, 8 * COIN, 3 * COIN};
    std::array<std::vector<unsigned char>, 3> challenges;
    UniValue saved{UniValue::VOBJ}, pending{UniValue::VARR};
    CCoinControl control;
    control.m_feerate = CFeeRate{1000};
    for (uint32_t i = 0; i < rewards.size(); ++i) {
        const COutPoint outpoint{Txid::FromUint256(uint256::ONE), i};
        const auto target{i == 2 ? uint256::FromHex(std::string(63, '0') + "3").value() : uint256{}};
        {
            LOCK(cs_main);
            m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoint,
                Coin{CTxOut{rewards[i], PayToDomainOutput{"example.com", target, 1}}, 100, false}, false);
        }
        const auto prepared{PrepareP2CClaim(*wallet, outpoint, control)};
        BOOST_REQUIRE(prepared);
        CMutableTransaction tx{*prepared->tx};
        tx.vout[0].nValue = payouts[i]; // Retained fees deliberately differ.
        pending.push_back(EncodeHexTx(CTransaction{tx}));
        const auto challenge{P2CClaimChallenge(CTransaction{tx}, 0)};
        challenges[i].assign(challenge.begin(), challenge.end());
    }
    saved.pushKV("pending", pending);
    saved.pushKV("ready", "");
    saved.pushKV("proof", "");
    saved.pushKV("attempts", 0);
    saved.pushKV("submitted", 0);
    saved.pushKV("last_txid", "");
    BOOST_REQUIRE(wallet->GetDatabase().MakeBatch()->Write(std::string{"p2c_claim_worker_v1"}, saved.write()));

    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    const auto public_address{LookupHost("8.8.8.8", false, restore_dns.original)};
    BOOST_REQUIRE(public_address);
    g_dns_lookup = [address = *public_address](const std::string&, bool) { return std::vector<CNetAddr>{address}; };
    std::mutex hellos_mutex;
    std::vector<std::shared_ptr<std::vector<unsigned char>>> hellos;
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        auto sent{std::make_shared<std::vector<unsigned char>>()};
        { std::lock_guard lock{hellos_mutex}; hellos.push_back(sent); }
        return std::make_unique<ClientHelloSocket>(*sent, 16384);
    };
    auto worker{MakeP2CClaimWorker(*wallet)};
    BOOST_REQUIRE(worker->Configure(-1, 1));
    const auto deadline{std::chrono::steady_clock::now() + 30s};
    while (std::chrono::steady_clock::now() < deadline) {
        { std::lock_guard lock{hellos_mutex}; if (hellos.size() >= 4) break; }
        std::this_thread::sleep_for(10ms);
    }
    worker->Stop(); // The first three attempts have completed before #4 starts.
    BOOST_REQUIRE(hellos.size() >= 4);
    const std::array<size_t, 3> order{2, 1, 0}; // Expected numerators: 12, 8, 5 coins.
    for (size_t i = 0; i < order.size(); ++i) {
        BOOST_REQUIRE(hellos[i]->size() >= 43);
        BOOST_CHECK_EQUAL_COLLECTIONS(hellos[i]->begin() + 11, hellos[i]->begin() + 43,
                                      challenges[order[i]].begin(), challenges[order[i]].end());
    }
}

BOOST_FIXTURE_TEST_CASE(worker_extra_domain_turns_use_net_return_and_recheck_locks, TestChain100Setup)
{
    using namespace std::chrono_literals;
    FakeSteadyClock clock;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    wallet->m_default_max_tx_fee = MAX_MONEY;
    const std::array<std::string, 4> domains{"alpha.example", "beta.example", "gamma.example", "zeta.example"};
    const std::array<CAmount, 4> rewards{10 * COIN, 9 * COIN, 4 * COIN, 100 * COIN};
    const std::array<CAmount, 4> payouts{5 * COIN, 8 * COIN, 3 * COIN, 99 * COIN};
    const std::array<uint8_t, 4> targets{0, 0, 3, 15};
    std::vector<COutPoint> outpoints;
    UniValue saved{UniValue::VOBJ}, pending{UniValue::VARR};
    CCoinControl control;
    control.m_feerate = CFeeRate{1000};
    for (uint32_t i = 0; i < domains.size(); ++i) {
        outpoints.emplace_back(Txid::FromUint256(uint256::ONE), i);
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoints.back(),
            Coin{CTxOut{rewards[i], PayToDomainOutput{domains[i], uint256{targets[i]}, 1}}, 100, false}, false));
        const auto prepared{PrepareP2CClaim(*wallet, outpoints.back(), control)};
        BOOST_REQUIRE(prepared);
        CMutableTransaction tx{*prepared->tx};
        tx.vout[0].nValue = payouts[i]; // Rank by the retained fee, not gross reward.
        pending.push_back(EncodeHexTx(CTransaction{tx}));
    }
    saved.pushKV("pending", std::move(pending));
    saved.pushKV("ready", "");
    saved.pushKV("proof", "");
    saved.pushKV("attempts", 0);
    saved.pushKV("submitted", 0);
    saved.pushKV("last_txid", "");
    BOOST_REQUIRE(wallet->GetDatabase().MakeBatch()->Write(std::string{"p2c_claim_worker_v1"}, saved.write()));
    WITH_LOCK(wallet->cs_wallet, BOOST_REQUIRE(wallet->LockCoin(outpoints[3], /*persist=*/false))); // Highest score, but unavailable.

    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    const auto address{LookupHost("8.8.8.8", false, restore_dns.original)};
    BOOST_REQUIRE(address);
    std::mutex events_mutex;
    std::condition_variable release;
    std::vector<std::string> events;
    bool finish{false};
    struct ReleaseAll {
        std::mutex& mutex;
        std::condition_variable& wake;
        bool& finish;
        ~ReleaseAll() { { std::lock_guard lock{mutex}; finish = true; } wake.notify_all(); }
    };
    g_dns_lookup = [&](const std::string&, bool) { return std::vector<CNetAddr>{*address}; };
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        auto sent{std::make_shared<std::vector<unsigned char>>()};
        return std::make_unique<ClientHelloSocket>(*sent, 16384, [&, sent] {
            std::unique_lock lock{events_mutex};
            for (const auto& name : domains) {
                if (std::search(sent->begin(), sent->end(), name.begin(), name.end()) != sent->end()) events.push_back(name);
            }
            if (events.size() == 6 || events.size() == 8) release.wait(lock, [&] { return finish; });
        });
    };
    auto worker{MakeP2CClaimWorker(*wallet)};
    ReleaseAll release_all{events_mutex, release, finish};
    BOOST_REQUIRE(worker->Configure(-1, 1));
    const auto wait_until = [](const auto& predicate) {
        const auto deadline{std::chrono::steady_clock::now() + 20s};
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(10ms);
        }
        return true;
    };
    const bool first_six{wait_until([&] { std::lock_guard lock{events_mutex}; return events.size() == 6; })};
    if (first_six) {
        // Recheck locks while the sixth connection is still in flight.
        WITH_LOCK(wallet->cs_wallet, BOOST_REQUIRE(wallet->LockCoin(outpoints[2], false)));
        const auto previous{worker->Status()["schedule_refreshes"].getInt<uint64_t>()};
        clock += 5s;
        BOOST_CHECK(wait_until([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > previous; }));
        { std::lock_guard lock{events_mutex}; finish = true; }
        release.notify_all();
    } else {
        { std::lock_guard lock{events_mutex}; finish = true; }
        release.notify_all();
    }
    const bool all{wait_until([&] { std::lock_guard lock{events_mutex}; return events.size() >= 8; })};
    worker->Stop();
    BOOST_REQUIRE(first_six && all);
    const std::vector<std::string> expected{"alpha.example", "gamma.example", "beta.example", "gamma.example",
                                            "gamma.example", "gamma.example", "alpha.example"};
    BOOST_REQUIRE(events.size() >= expected.size());
    BOOST_CHECK_EQUAL_COLLECTIONS(events.begin(), events.begin() + expected.size(), expected.begin(), expected.end());
    // After the refresh the extra turn also uses measured TCP/TLS performance.
    // Neither the locked domain nor the removed one may receive that turn.
    BOOST_CHECK(events[7] == "alpha.example" || events[7] == "beta.example");
}

BOOST_FIXTURE_TEST_CASE(worker_weights_domain_priority_by_complete_tls_per_second, TestChain100Setup)
{
    using namespace std::chrono_literals;
    FakeSteadyClock clock;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    wallet->m_default_max_tx_fee = MAX_MONEY; // Deliberately retained large fees below.
    const std::array<std::string, 2> domains{"alpha.example", "beta.example"};
    const auto maximum{uint256::FromHex(std::string(64, 'f')).value()};
    CCoinControl control;
    control.m_feerate = CFeeRate{1000};
    UniValue saved{UniValue::VOBJ}, pending{UniValue::VARR};
    for (uint32_t i = 0; i < domains.size(); ++i) {
        const COutPoint outpoint{Txid::FromUint256(uint256::ONE), i};
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoint,
            Coin{CTxOut{4 * COIN, PayToDomainOutput{domains[i], maximum, 1}}, 100, false}, false));
        const auto prepared{PrepareP2CClaim(*wallet, outpoint, control)};
        BOOST_REQUIRE(prepared);
        CMutableTransaction tx{*prepared->tx};
        tx.vout[0].nValue = (2 - i) * COIN; // Alpha has exactly twice the net EV.
        pending.push_back(EncodeHexTx(CTransaction{tx}));
    }
    saved.pushKV("pending", std::move(pending));
    saved.pushKV("ready", "");
    saved.pushKV("proof", "");
    saved.pushKV("attempts", 0);
    saved.pushKV("submitted", 0);
    saved.pushKV("last_txid", "");
    BOOST_REQUIRE(wallet->GetDatabase().MakeBatch()->Write(std::string{"p2c_claim_worker_v1"}, saved.write()));
    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    const auto address{LookupHost("8.8.8.8", false, restore_dns.original)};
    BOOST_REQUIRE(address);
    g_dns_lookup = [&](const std::string&, bool) { return std::vector<CNetAddr>{*address}; };
    const auto config{std::make_shared<test::P2CTLSServer>()};
    config->Setup();
    std::mutex events_mutex;
    std::condition_variable release;
    std::vector<std::string> events;
    bool finish{false};
    auto worker{MakeP2CClaimWorker(*wallet)};
    struct ReleaseAll {
        std::mutex& mutex;
        std::condition_variable& wake;
        bool& finish;
        ~ReleaseAll() { { std::lock_guard lock{mutex}; finish = true; } wake.notify_all(); }
    } release_all{events_mutex, release, finish};
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        // A single connection worker makes the last assignment unambiguous.
        const auto domain{worker->Status()["domain"].get_str()};
        {
            std::unique_lock lock{events_mutex};
            events.push_back(domain);
            if (events.size() == 4) release.wait(lock, [&] { return finish; });
        }
        if (domain == domains[0]) {
            std::this_thread::sleep_for(1s);
            return nullptr; // Slow TCP failure; never reached CertificateVerify.
        }
        // Beta is slower per attempt, but delivers CertificateVerify. If its
        // completed capture is mistakenly counted as a failure, its quality
        // score cannot beat Alpha's: this tests success, not latency alone.
        std::this_thread::sleep_for(2s);
        return std::make_unique<test::P2CTLSSocket>(config, std::vector<std::string>{}, 127);
    };
    const auto wait_until = [](const auto& predicate) {
        const auto deadline{std::chrono::steady_clock::now() + 30s};
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(10ms);
        }
        return true;
    };
    BOOST_REQUIRE(worker->Configure(-1, 1));
    BOOST_REQUIRE_MESSAGE(wait_until([&] { std::lock_guard lock{events_mutex}; return events.size() == 4; }), worker->Status().write());
    // No quality refresh before this point: initial extra turns favor Alpha.
    // Beta completed TLS, though its test certificate is not trusted by Core.
    // Capturing it must improve priority WITHOUT weakening claim verification.
    BOOST_CHECK_EQUAL(worker->Status()["state"].get_str(), "searching");
    BOOST_CHECK_EQUAL(worker->Status()["submitted"].getInt<uint64_t>(), 0U);
    const auto previous{worker->Status()["schedule_refreshes"].getInt<uint64_t>()};
    clock += 5s;
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > previous; }));
    { std::lock_guard lock{events_mutex}; finish = true; }
    release.notify_all();
    BOOST_REQUIRE(wait_until([&] { std::lock_guard lock{events_mutex}; return events.size() >= 6; }));
    worker->Stop();
    const std::vector<std::string> expected{domains[0], domains[0], domains[1], domains[0], domains[0], domains[1]};
    BOOST_CHECK_EQUAL_COLLECTIONS(events.begin(), events.begin() + expected.size(), expected.begin(), expected.end());
    BOOST_CHECK_EQUAL(worker->Status()["submitted"].getInt<uint64_t>(), 0U);

    // Reconfiguration and an entirely ineligible catalog must not discard the
    // learned history. With reset priors Alpha would win the next extra turn.
    for (uint32_t i = 0; i < domains.size(); ++i) {
        const COutPoint outpoint{Txid::FromUint256(uint256::ONE), i};
        WITH_LOCK(wallet->cs_wallet, BOOST_REQUIRE(wallet->LockCoin(outpoint, false)));
    }
    BOOST_REQUIRE(worker->Configure(-1, 1));
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > 0; }));
    { std::lock_guard lock{events_mutex}; BOOST_CHECK_EQUAL(events.size(), 6U); }
    for (uint32_t i = 0; i < domains.size(); ++i) {
        const COutPoint outpoint{Txid::FromUint256(uint256::ONE), i};
        WITH_LOCK(wallet->cs_wallet, wallet->UnlockCoin(outpoint));
    }
    clock += 5s;
    BOOST_REQUIRE_MESSAGE(wait_until([&] { std::lock_guard lock{events_mutex}; return events.size() >= 8; }), worker->Status().write());
    worker->Stop();
    BOOST_CHECK_EQUAL(events[6], domains[0]);
    BOOST_CHECK_EQUAL(events[7], domains[1]);
}

BOOST_FIXTURE_TEST_CASE(worker_batches_spent_checks_and_recovers_eligibility, TestChain100Setup)
{
    using namespace std::chrono_literals;
    FakeSteadyClock clock;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    const COutPoint outpoint{Txid::FromUint256(uint256::ONE), 0};
    const CTxOut bounty{COIN, PayToDomainOutput{"example.com", uint256{}, 1}};
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoint, Coin{bounty, 100, false}, false));
    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    const auto address{LookupHost("8.8.8.8", false, restore_dns.original)};
    BOOST_REQUIRE(address);
    g_dns_lookup = [&](const std::string&, bool) { return std::vector<CNetAddr>{*address}; };
    std::atomic<unsigned> attempts{0};
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> { ++attempts; return nullptr; };
    const auto wait_until = [](const auto& predicate) {
        const auto deadline{std::chrono::steady_clock::now() + 10s};
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    };
    auto worker{MakeP2CClaimWorker(*wallet)};
    BOOST_REQUIRE(worker->Configure(-1, 1));
    BOOST_REQUIRE(wait_until([&] { return attempts >= 1; }));
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().SpendCoin(outpoint));
    // The scheduling clock is frozen: no per-connection UTXO lookup is allowed.
    BOOST_REQUIRE(wait_until([&] { return attempts >= 2; }));
    auto previous{worker->Status()["schedule_refreshes"].getInt<uint64_t>()};
    clock += 5s;
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > previous; }));
    const auto stopped_at{attempts.load()};
    std::this_thread::sleep_for(1200ms);
    BOOST_CHECK_EQUAL(attempts.load(), stopped_at);
    // Restore the same coin to model removal of a temporary competing spend.
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoint, Coin{bounty, 100, false}, false));
    previous = worker->Status()["schedule_refreshes"].getInt<uint64_t>();
    clock += 5s;
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > previous; }));
    BOOST_REQUIRE(wait_until([&] { return attempts > stopped_at; }));
    worker->Stop();
}

BOOST_FIXTURE_TEST_CASE(worker_retains_multiple_completed_proofs_on_restart, TestChain100Setup)
{
    using namespace std::chrono_literals;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    UniValue saved{UniValue::VOBJ}, pending{UniValue::VARR}, completed{UniValue::VARR};
    std::vector<CTransactionRef> transactions;
    CCoinControl control;
    control.m_feerate = CFeeRate{1000};
    for (uint32_t i = 0; i < 3; ++i) {
        const COutPoint outpoint{Txid::FromUint256(uint256::ONE), i};
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoint,
            Coin{CTxOut{COIN, PayToDomainOutput{"example.com", uint256{}, 1}}, 100, false}, false));
        const auto prepared{PrepareP2CClaim(*wallet, outpoint, control)};
        BOOST_REQUIRE(prepared);
        transactions.push_back(prepared->tx);
        const auto encoded{EncodeHexTx(*prepared->tx)};
        pending.push_back(encoded);
        if (i == 0) {
            saved.pushKV("ready", encoded); // Legacy single-proof slot.
            saved.pushKV("proof", "00");
        } else {
            UniValue item{UniValue::VOBJ};
            item.pushKV("tx", encoded);
            item.pushKV("proof", "00"); // Deliberately invalid: must never submit.
            completed.push_back(std::move(item));
        }
        if (i < 2) {
            WITH_LOCK(wallet->cs_wallet, BOOST_REQUIRE(wallet->AddToWallet(prepared->tx, TxStateInactive{})));
        }
    }
    saved.pushKV("pending", std::move(pending));
    saved.pushKV("completed", std::move(completed));
    saved.pushKV("attempts", 0);
    saved.pushKV("submitted", 0);
    saved.pushKV("last_txid", "");
    BOOST_REQUIRE(wallet->GetDatabase().MakeBatch()->Write(std::string{"p2c_claim_worker_v1"}, saved.write()));
    RestoreDNSLookup restore_dns;
    unsigned lookups{0};
    g_dns_lookup = [&](const std::string&, bool) { ++lookups; return std::vector<CNetAddr>{}; };
    for (int restart = 0; restart < 2; ++restart) {
        auto worker{MakeP2CClaimWorker(*wallet)};
        BOOST_REQUIRE(worker->Configure(1, 1));
        const auto deadline{std::chrono::steady_clock::now() + 10s};
        while (worker->Status()["state"].get_str() != "stopped with error" && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        worker->Stop();
        BOOST_CHECK(worker->Status()["last_error"].get_str().find("Completed P2C proof retained") != std::string::npos);
        std::string encoded;
        BOOST_REQUIRE(wallet->GetDatabase().MakeBatch()->Read(std::string{"p2c_claim_worker_v1"}, encoded));
        UniValue retained;
        BOOST_REQUIRE(retained.read(encoded));
        BOOST_CHECK_EQUAL(retained["completed"].size(), 1U);
        BOOST_CHECK_EQUAL(retained["completed"][0]["tx"].get_str(), EncodeHexTx(*transactions.back()));
        BOOST_CHECK_EQUAL(retained["completed"][0]["proof"].get_str(), "00");
        BOOST_CHECK_EQUAL(retained["submitted"].getInt<uint64_t>(), 0U);
    }
    BOOST_CHECK_EQUAL(lookups, 0U);
}

BOOST_FIXTURE_TEST_CASE(worker_preserves_bounty_through_complete_tls_capture, TestChain100Setup)
{
    using namespace std::chrono_literals;
    FakeSteadyClock clock;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    const auto endpoint{Lookup("8.8.8.8", 443, false)};
    BOOST_REQUIRE(endpoint);
    const auto config{std::make_shared<test::P2CTLSServer>()};
    config->Setup();
    g_dns_lookup = [address = static_cast<const CNetAddr&>(*endpoint)](const std::string&, bool) {
        return std::vector<CNetAddr>{address};
    };
    auto worker{MakeP2CClaimWorker(*wallet)};
    std::map<std::string, COutPoint> bounties;
    const auto maximum{uint256::FromHex(std::string(64, 'f')).value()};
    for (const std::string name : {"google.com", "lifetime-regression-long-domain.example"}) {
        const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), 0};
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoint,
            Coin{CTxOut{COIN, PayToDomainOutput{name, maximum, 1}}, 100, false}, false));
        bounties.emplace(name, outpoint);
    }
    for (const auto& [domain, outpoint] : bounties) {
        std::atomic<bool> hold{false}, entered{false};
        struct StopBeforeLocals {
            P2CClaimWorker& worker;
            ~StopBeforeLocals() { worker.Stop(); }
        } stop_before_locals{*worker};
        CreateSock = [&, config, size = domain.size()](int, int, int) -> std::unique_ptr<Sock> {
            if (hold) {
                entered = true;
                const auto deadline{std::chrono::steady_clock::now() + 5s};
                while (hold && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
            }
            std::vector<std::string> churn;
            churn.reserve(256);
            for (int i = 0; i < 256; ++i) churn.emplace_back(size, '!');
            return std::make_unique<test::P2CTLSSocket>(config, std::move(churn), 127);
        };
        // First prove this fake transport completes encrypted TLS capture,
        // including CertificateVerify, instead of returning an EOF/timeout.
        const auto captured{CaptureP2CTls(*endpoint, domain, uint256::ONE, [] { return false; })};
        BOOST_REQUIRE_MESSAGE(captured, util::ErrorString(captured).original);
        P2CTlsProofView view;
        std::string error;
        BOOST_REQUIRE(ParseP2CTlsProof(*captured, domain, uint256::ONE, view, error));
        BOOST_CHECK(!view.certificate_verify_signature.empty());
        const CTxOut output{COIN, PayToDomainOutput{domain, maximum, 1}};
        CCoinControl control;
        control.m_feerate = CFeeRate{1000};
        const auto proposal{PrepareP2CClaim(*wallet, outpoint, control)};
        BOOST_REQUIRE(proposal);
        // Keep production trust rules intact: the fixture's certificate must
        // fail Core's certificate verification, NOT domain/transcript parsing.
        BOOST_REQUIRE(!VerifyP2CCertificateProof(output, view, proposal->validation_time, error));
        const std::string expected_error{error};
        BOOST_REQUIRE(!expected_error.empty());
        BOOST_CHECK(expected_error != "invalid expected P2C domain");
        hold = true;
        BOOST_REQUIRE(worker->Configure(1, 1, {domain}));
        const auto entered_deadline{std::chrono::steady_clock::now() + 5s};
        while (!entered && std::chrono::steady_clock::now() < entered_deadline) std::this_thread::sleep_for(5ms);
        const bool was_entered{entered.load()};
        const auto previous{worker->Status()["schedule_refreshes"].getInt<uint64_t>()};
        clock += 5s;
        const auto refresh_deadline{std::chrono::steady_clock::now() + 3s};
        while (worker->Status()["schedule_refreshes"].getInt<uint64_t>() == previous && std::chrono::steady_clock::now() < refresh_deadline) {
            std::this_thread::sleep_for(5ms);
        }
        hold = false;
        BOOST_CHECK(was_entered);
        BOOST_CHECK(worker->Status()["schedule_refreshes"].getInt<uint64_t>() > previous);
        const auto deadline{std::chrono::steady_clock::now() + 20s};
        UniValue status;
        do {
            status = worker->Status();
            if (!status["last_error"].get_str().empty()) break;
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);
        worker->Stop();
        BOOST_CHECK_EQUAL(status["state"].get_str(), "certificate rejected");
        BOOST_CHECK_EQUAL(status["last_error"].get_str(), expected_error);
        BOOST_CHECK_EQUAL(status["submitted"].getInt<uint64_t>(), 0U);
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().SpendCoin(outpoint));
    }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
