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

BOOST_FIXTURE_TEST_CASE(worker_rotates_without_idle_delay_and_stops_during_retry, TestChain100Setup)
{
    using namespace std::chrono_literals;
    FakeSteadyClock clock;
    BOOST_REQUIRE(!m_node.chainman->IsInitialBlockDownload());
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    const COutPoint first{Txid::FromUint256(uint256::ONE), 0};
    const COutPoint second{Txid::FromUint256(uint256::ONE), 1};
    {
        LOCK(cs_main);
        auto& coins{m_node.chainman->ActiveChainstate().CoinsTip()};
        for (const auto& outpoint : {first, second}) {
            coins.AddCoin(outpoint, Coin{CTxOut{COIN, PayToDomainOutput{"example.com", uint256{}, 1}}, 100, false}, false);
        }
    }
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(wallet->LockCoin(first, /*persist=*/false));
        BOOST_REQUIRE(wallet->LockCoin(second, /*persist=*/false));
    }

    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    std::atomic<unsigned> lookups{0};
    std::atomic<unsigned> sockets{0};
    std::atomic<unsigned> public_lookups{0};
    // Exercise the real scheduling loop without DNS or HTTPS traffic.
    g_dns_lookup = [&](const std::string&, bool) {
        ++lookups;
        return std::vector<CNetAddr>{};
    };
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> { ++sockets; return nullptr; };
    auto worker{MakeP2CClaimWorker(*wallet)};
    const auto wait_until = [](const auto& predicate) {
        const auto deadline{std::chrono::steady_clock::now() + 30s};
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(10ms);
        }
        return true;
    };

    BOOST_REQUIRE(worker->Configure(-1, 1));
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["state"].get_str() == "waiting for eligible bounties"; }));
    BOOST_CHECK_EQUAL(lookups.load(), 0U);
    {
        LOCK(wallet->cs_wallet);
        wallet->UnlockCoin(first);
        wallet->UnlockCoin(second);
    }
    // Three domain-level DNS attempts prove that subsequent passes start
    // without the former unconditional 60-second sleep. The initial idle wait
    // must also rediscover coins made eligible without restarting the worker.
    BOOST_REQUIRE(wait_until([&] {
        return lookups.load() >= 3 && worker->Status()["state"].get_str() == "retrying domain resolution";
    }));
    const auto before_stop{std::chrono::steady_clock::now()};
    worker->Stop();
    BOOST_CHECK(std::chrono::steady_clock::now() - before_stop < 5s);
    BOOST_CHECK_EQUAL(worker->Status()["state"].get_str(), "disabled");
    BOOST_CHECK_EQUAL(worker->Status()["connections_per_second"].getInt<int>(), 0);
    BOOST_CHECK_EQUAL(sockets.load(), 0U);

    // A resolved public address still uses the failing socket factory. More
    // than 64 attempts in one domain round demonstrate there is no attempt cap.
    // Real one-second connection backoffs must remain, but no minute-long idle.
    const auto public_address{LookupHost("8.8.8.8", false, restore_dns.original)};
    BOOST_REQUIRE(public_address);
    g_dns_lookup = [&, address = *public_address](const std::string&, bool) { ++public_lookups; return std::vector<CNetAddr>{address}; };
    const auto before_connections{std::chrono::steady_clock::now()};
    BOOST_REQUIRE(worker->Configure(-1, 16));
    BOOST_REQUIRE(wait_until([&] { return sockets.load() > 64; }));
    BOOST_CHECK(std::chrono::steady_clock::now() - before_connections >= 2s);
    BOOST_CHECK(worker->Status()["state"].get_str() != "waiting for bounties");
    BOOST_CHECK_EQUAL(public_lookups.load(), 1U);
    // A sole domain resumes immediately after the internal scheduling quantum.
    // The quantum is not a per-domain rate quota or a cooldown period.
    const auto previous_sockets{sockets.load()};
    clock += 30s;
    BOOST_REQUIRE(wait_until([&] { return public_lookups.load() >= 2 && sockets.load() > previous_sockets; }));
    BOOST_CHECK(worker->Status()["state"].get_str() != "waiting for bounties");
    worker->Stop();
    BOOST_CHECK_EQUAL(worker->Status()["connections_per_second"].getInt<int>(), 0);

    // Keep simulated connects in flight until all 65 have started. This tests
    // actual parallelism, not just accepting a larger configuration value, and
    // catches a hidden 32-attempt round cap. No OS/network sockets are opened.
    struct PendingConnections {
        std::mutex mutex;
        std::condition_variable wake;
        unsigned active{0};
        bool released{false};
        void Release()
        {
            { std::lock_guard lock{mutex}; released = true; }
            wake.notify_all();
        }
    };
    const auto pending{std::make_shared<PendingConnections>()};
    struct ReleaseSockets {
        std::shared_ptr<PendingConnections> pending;
        ~ReleaseSockets() { pending->Release(); }
    } release_sockets{pending}; // Also unblock threads if an assertion throws.
    CreateSock = [pending](int, int, int) -> std::unique_ptr<Sock> {
        std::unique_lock lock{pending->mutex};
        ++pending->active;
        pending->wake.wait(lock, [&] { return pending->released; });
        --pending->active;
        return nullptr;
    };
    BOOST_REQUIRE(worker->Configure(-1, 65));
    const bool reached_65{wait_until([&] {
        std::lock_guard lock{pending->mutex};
        return pending->active == 65;
    })};
    pending->Release();
    worker->Stop();
    BOOST_CHECK(reached_65);
    BOOST_CHECK_EQUAL(worker->Status()["concurrency"].getInt<int>(), 65);
    {
        LOCK(cs_main);
        auto& coins{m_node.chainman->ActiveChainstate().CoinsTip()};
        coins.SpendCoin(first);
        coins.SpendCoin(second);
    }
    BOOST_REQUIRE(worker->Configure(-1, 1));
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["state"].get_str() == "waiting for bounties"; }));
    BOOST_CHECK(worker->Status()["domain"].get_str().empty());
    worker->Stop();

    // Cancellation is reset only after the previous thread was joined. Each
    // restart must reach the idle wait, and Stop must wake that five-second
    // wait rather than leaving a detached worker behind. Shutdown is terminal.
    for (int restart{0}; restart < 3; ++restart) {
        BOOST_REQUIRE(worker->Configure(1, 4));
        BOOST_REQUIRE(wait_until([&] { return worker->Status()["state"].get_str() == "waiting for bounties"; }));
        const auto before_idle_stop{std::chrono::steady_clock::now()};
        worker->Stop();
        BOOST_CHECK(std::chrono::steady_clock::now() - before_idle_stop < 3s);
        BOOST_CHECK_EQUAL(worker->Status()["state"].get_str(), "disabled");
        BOOST_CHECK_EQUAL(worker->Status()["connections_per_second"].getInt<int>(), 0);
    }
    worker->Shutdown();
    BOOST_CHECK(!worker->Configure(-1, 4));
    worker->Stop(); // Idempotent after shutdown.
}

BOOST_FIXTURE_TEST_CASE(worker_shares_domain_rounds_and_rotates_large_bounty_groups, TestChain100Setup)
{
    using namespace std::chrono_literals;
    FakeSteadyClock clock;
    BOOST_REQUIRE(!m_node.chainman->IsInitialBlockDownload());
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    const uint256 target{uint256::FromHex("003" + std::string(61, 'f')).value()}; // Ten leading zero bits.
    {
        LOCK(cs_main);
        auto& coins{m_node.chainman->ActiveChainstate().CoinsTip()};
        // More outputs than the proposal cache must not crowd out other domains.
        for (uint32_t i = 0; i < 262; ++i) {
            const std::string domain{i < 260 ? "alpha.example" : i == 260 ? "beta.example" : "gamma.example"};
            // The adversarial domain has many bounties with enormous rewards,
            // yet its server below never returns a usable proof. Neither its
            // high expected payout nor its output count may starve beta/gamma.
            const CAmount reward{i < 260 ? 100'000 * COIN + i * (COIN / 1000) : COIN};
            coins.AddCoin(COutPoint{Txid::FromUint256(uint256::ONE), i},
                          Coin{CTxOut{reward, PayToDomainOutput{domain, target, 1}}, 100, false}, false);
        }
    }
    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    const auto public_address{LookupHost("8.8.8.8", false, restore_dns.original)};
    BOOST_REQUIRE(public_address);
    struct Round {
        std::string domain;
        std::vector<std::shared_ptr<std::vector<unsigned char>>> hellos;
        std::shared_ptr<std::atomic<unsigned>> captures{std::make_shared<std::atomic<unsigned>>(0)};
    };
    std::mutex rounds_mutex;
    std::vector<Round> rounds;
    std::map<std::vector<unsigned char>, COutPoint> claim_outpoints;
    g_dns_lookup = [&, address = *public_address](const std::string& domain, bool) {
        // Proposals were persisted before resolving. Remember their challenges
        // before a later bounded window evicts them from the live wallet cache.
        std::string saved;
        if (!wallet->GetDatabase().MakeBatch()->Read(std::string{"p2c_claim_worker_v1"}, saved)) return std::vector<CNetAddr>{};
        UniValue state;
        if (!state.read(saved)) return std::vector<CNetAddr>{};
        for (const auto& pending : state["pending"].getValues()) {
            CMutableTransaction tx;
            if (!DecodeHexTx(tx, pending.get_str())) return std::vector<CNetAddr>{};
            const auto challenge{P2CClaimChallenge(CTransaction{tx}, 0)};
            claim_outpoints.emplace(std::vector<unsigned char>{challenge.begin(), challenge.end()}, tx.vin[0].prevout);
        }
        std::lock_guard lock{rounds_mutex};
        rounds.push_back({domain, {}});
        return std::vector<CNetAddr>{address};
    };
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        auto sent{std::make_shared<std::vector<unsigned char>>()};
        std::shared_ptr<std::atomic<unsigned>> captures;
        {
            std::lock_guard lock{rounds_mutex};
            rounds.back().hellos.push_back(sent);
            captures = rounds.back().captures;
        }
        return std::make_unique<ClientHelloSocket>(*sent, 16384, [captures] { ++*captures; });
    };
    auto worker{MakeP2CClaimWorker(*wallet)};
    BOOST_REQUIRE(worker->Configure(-1, 32));
    bool completed{false};
    for (size_t i = 0; i < 6; ++i) {
        const auto deadline{std::chrono::steady_clock::now() + 30s};
        completed = false;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock{rounds_mutex};
                completed = rounds.size() == i + 1 && rounds[i].captures->load() >= 64;
                if (rounds.size() > i + 1) break; // Rotated before the deadline.
            }
            if (completed) break;
            std::this_thread::sleep_for(10ms);
        }
        if (!completed) break;
        clock += 30s; // Exercise internal rotation without minutes of CI sleep.
    }
    worker->Stop(); // Join before inspecting each socket's captured bytes.
    BOOST_REQUIRE(completed);
    // Fair turns still visit every domain, while the economic winner receives
    // extra turns. Those extras must NOT advance the fair-domain cursor.
    const std::vector<std::string> expected{"alpha.example", "alpha.example", "beta.example",
                                            "alpha.example", "gamma.example", "alpha.example"};
    std::set<std::vector<unsigned char>> first_alpha;
    uint32_t first_alpha_min{0};
    for (size_t i = 0; i < 6; ++i) {
        const auto& round{rounds[i]};
        BOOST_CHECK_EQUAL(round.domain, expected[i]);
        BOOST_CHECK(round.captures->load() >= 64U); // More than 32 and concurrency.
        std::set<std::vector<unsigned char>> challenges;
        for (const auto& hello : round.hellos) {
            if (hello->size() < 43) continue; // Cancelled at the round deadline.
            challenges.emplace(hello->begin() + 11, hello->begin() + 43);
        }
        if (i < 2) BOOST_CHECK(challenges.size() >= 64U);
        if (round.domain != "alpha.example") BOOST_CHECK_EQUAL(challenges.size(), 1U);
        if (round.domain == "alpha.example") {
            std::set<uint32_t> indices;
            for (const auto& challenge : challenges) {
                const auto found{claim_outpoints.find(challenge)};
                BOOST_REQUIRE(found != claim_outpoints.end());
                indices.insert(found->second.n);
            }
            // Highest net rewards must be selected across the entire domain,
            // including outputs beyond the first 256 in outpoint order.
            BOOST_REQUIRE(!indices.empty());
            if (i == 0) {
                BOOST_CHECK_EQUAL(*indices.rbegin(), 259U);
                first_alpha_min = *indices.begin();
            } else if (i == 1) {
                BOOST_CHECK(*indices.rbegin() < first_alpha_min);
            }
        }
        if (i == 0) first_alpha = challenges;
        if (i == 1) {
            // The second domain turn must reach later outputs, not restart at
            // its first window. Each ClientHello commits to a different fixed claim.
            for (const auto& challenge : challenges) BOOST_CHECK(!first_alpha.contains(challenge));
        }
    }
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
    struct Round { std::string domain; unsigned attempts{0}; };
    std::mutex rounds_mutex;
    std::vector<Round> rounds;
    g_dns_lookup = [&](const std::string& domain, bool) {
        std::lock_guard lock{rounds_mutex};
        // Discovery can exceed an entire quantum. It must not consume the
        // first search window before even one connection starts (including
        // the extra scan needed to select an economic-priority domain).
        clock += 31s;
        rounds.push_back({domain});
        return std::vector<CNetAddr>{*address};
    };
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        std::lock_guard lock{rounds_mutex};
        ++rounds.back().attempts;
        return nullptr; // Entirely offline; advance the scheduling clock below.
    };
    auto worker{MakeP2CClaimWorker(*wallet)};
    BOOST_REQUIRE(worker->Configure(-1, 1));
    // Scores are 5, 8 and 12. Gamma wins extras despite the lowest gross reward.
    // Locking gamma after turn six must redirect the next extra to beta, whose
    // retained payout beats alpha even though alpha has the larger gross reward.
    const std::vector<std::string> expected{"alpha.example", "gamma.example", "beta.example", "gamma.example",
                                            "gamma.example", "gamma.example", "alpha.example", "beta.example"};
    bool completed{false};
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto deadline{std::chrono::steady_clock::now() + 30s};
        completed = false;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock{rounds_mutex};
                completed = rounds.size() == i + 1 && rounds[i].attempts > 0;
                if (rounds.size() > i + 1) break;
            }
            if (completed) break;
            std::this_thread::sleep_for(10ms);
        }
        if (!completed) break;
        if (i == 5) WITH_LOCK(wallet->cs_wallet, BOOST_REQUIRE(wallet->LockCoin(outpoints[2], /*persist=*/false)));
        if (i + 1 < expected.size()) {
            std::lock_guard lock{rounds_mutex};
            clock += 30s;
        }
    }
    worker->Stop();
    BOOST_REQUIRE(completed);
    BOOST_REQUIRE_EQUAL(rounds.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) BOOST_CHECK_EQUAL(rounds[i].domain, expected[i]);
    BOOST_CHECK_EQUAL(worker->Status()["domain_rounds"].getInt<uint64_t>(), expected.size());
}

BOOST_FIXTURE_TEST_CASE(worker_exhausts_all_domain_windows_before_rotating, TestChain100Setup)
{
    using namespace std::chrono_literals;
    FakeSteadyClock clock; // The 30-second domain deadline never expires here.
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    {
        LOCK(cs_main);
        for (uint32_t i = 0; i < 261; ++i) {
            m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(COutPoint{Txid::FromUint256(uint256::ONE), i},
                Coin{CTxOut{COIN, PayToDomainOutput{i < 260 ? "alpha.example" : "beta.example", uint256{}, 1}}, 100, false}, false);
        }
    }
    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    const auto address{LookupHost("8.8.8.8", false, restore_dns.original)};
    BOOST_REQUIRE(address);
    std::mutex pending_mutex;
    std::vector<COutPoint> pending;
    std::vector<std::string> windows;
    unsigned retired{0}, retired_before_beta{0};
    g_dns_lookup = [&](const std::string& domain, bool) {
        std::string encoded;
        if (!wallet->GetDatabase().MakeBatch()->Read(std::string{"p2c_claim_worker_v1"}, encoded)) return std::vector<CNetAddr>{};
        UniValue saved;
        if (!saved.read(encoded)) return std::vector<CNetAddr>{};
        std::lock_guard lock{pending_mutex};
        pending.clear();
        windows.push_back(domain);
        if (domain == "beta.example") retired_before_beta = retired;
        for (const auto& proposal : saved["pending"].getValues()) {
            CMutableTransaction tx;
            if (!DecodeHexTx(tx, proposal.get_str())) return std::vector<CNetAddr>{};
            pending.push_back(tx.vin[0].prevout);
        }
        return std::vector<CNetAddr>{*address};
    };
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        std::lock_guard lock{pending_mutex};
        if (!pending.empty()) {
            WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().SpendCoin(pending.back()));
            pending.pop_back();
            ++retired;
        }
        return nullptr; // Simulate competing spends, never real connections.
    };
    auto worker{MakeP2CClaimWorker(*wallet)};
    BOOST_REQUIRE(worker->Configure(-1, 32));
    const auto deadline{std::chrono::steady_clock::now() + 30s};
    bool exhausted{false};
    while (std::chrono::steady_clock::now() < deadline) {
        { std::lock_guard lock{pending_mutex}; exhausted = retired == 261; }
        if (exhausted) break;
        std::this_thread::sleep_for(10ms);
    }
    worker->Stop();
    BOOST_CHECK(exhausted);
    BOOST_CHECK_EQUAL(retired_before_beta, 260U);
    BOOST_REQUIRE(windows.size() >= 3);
    BOOST_CHECK_EQUAL(windows.front(), "alpha.example");
    BOOST_CHECK_EQUAL(windows[1], "alpha.example"); // Refill within the SAME round.
    BOOST_CHECK_EQUAL(windows.back(), "beta.example");
    BOOST_CHECK_EQUAL(worker->Status()["domain_rounds"].getInt<uint64_t>(), 2U);
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
    for (const std::string domain : {"google.com", "lifetime-regression-long-domain.example"}) {
        CreateSock = [config, size = domain.size()](int, int, int) -> std::unique_ptr<Sock> {
            std::vector<std::string> churn;
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
        uint256 maximum;
        std::fill(maximum.begin(), maximum.end(), 0xff);
        const CTxOut output{COIN, PayToDomainOutput{domain, maximum, 1}};
        const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), 0};
        {
            LOCK(cs_main);
            m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoint, Coin{output, 100, false}, false);
        }
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
        BOOST_REQUIRE(worker->Configure(1, 1, {domain}));
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
