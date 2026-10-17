// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <coins.h>
#include <core_io.h>
#include <interfaces/chain.h>
#include <netbase.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>
#include <test/util/validation.h>
#include <univalue.h>
#include <validation.h>
#include <wallet/p2c_worker.h>
#include <wallet/coincontrol.h>
#include <wallet/p2c_claim.h>
#include <wallet/p2c_connection_rate.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace wallet {
namespace {
using namespace std::chrono_literals;

struct RestoreNetwork {
    const decltype(CreateSock) sockets{CreateSock};
    const DNSLookupFn dns{g_dns_lookup};
    ~RestoreNetwork() { CreateSock = sockets; g_dns_lookup = dns; }
};

template <typename Predicate>
bool WaitFor(Predicate predicate)
{
    const auto deadline{std::chrono::steady_clock::now() + 15s};
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

class CountConnect final : public ZeroSock {
    CountConnect& operator=(Sock&&) override { throw std::logic_error("Cannot move into CountConnect"); }
    std::atomic<unsigned>& m_count;
    std::atomic<bool>& m_destroyed;
    const std::function<void()> m_on_connect;
public:
    CountConnect(std::atomic<unsigned>& count, std::atomic<bool>& destroyed, std::function<void()> on_connect = {})
        : m_count{count}, m_destroyed{destroyed}, m_on_connect{std::move(on_connect)} {}
    ~CountConnect() override { m_destroyed = true; }
    int Connect(const sockaddr*, socklen_t) const override
    {
        ++m_count;
        if (m_on_connect) m_on_connect();
#ifdef WIN32
        WSASetLastError(WSAECONNREFUSED);
#else
        errno = ECONNREFUSED;
#endif
        return -1;
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(p2c_worker_admission_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(refresh_retires_candidate_before_tcp_without_spending_turn_or_rate)
{
    FakeSteadyClock clock;
    auto* chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    const COutPoint outpoint{Txid::FromUint256(uint256::ONE), 0};
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoint,
        Coin{CTxOut{COIN, PayToDomainOutput{"admission.example", uint256::FromHex(std::string(64, 'f')).value(), 1}}, 100, false}, false));
    RestoreNetwork restore;
    const auto address{LookupHost("8.8.8.8", false, restore.dns).value()};
    g_dns_lookup = [&](const std::string&, bool) { return std::vector<CNetAddr>{address}; };
    std::atomic<unsigned> allocations{0}, connects{0};
    std::atomic<bool> socket_destroyed{false};
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool released{false};
    auto worker{MakeP2CClaimWorker(*wallet)};
    struct ReleaseAndStop {
        std::mutex& mutex;
        std::condition_variable& gate;
        bool& released;
        P2CClaimWorker& worker;
        ~ReleaseAndStop() {
            { std::lock_guard lock{mutex}; released = true; }
            gate.notify_all();
            worker.Stop();
        }
    } release{gate_mutex, gate, released, *worker};
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        ++allocations;
        std::unique_lock lock{gate_mutex};
        gate.wait(lock, [&] { return released; });
        return std::make_unique<CountConnect>(connects, socket_destroyed);
    };
    BOOST_REQUIRE(worker->Configure(-1, 1));
    BOOST_REQUIRE(WaitFor([&] { return allocations.load() == 1; }));
    BOOST_CHECK_EQUAL(worker->Status()["attempts"].getInt<uint64_t>(), 0U);
    BOOST_CHECK_EQUAL(worker->Status()["domain_rounds"].getInt<uint64_t>(), 0U);
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().SpendCoin(outpoint));
    const auto before{worker->Status()["schedule_refreshes"].getInt<uint64_t>()};
    clock += 5s;
    BOOST_REQUIRE(WaitFor([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > before; }));
    { std::lock_guard lock{gate_mutex}; released = true; }
    gate.notify_all();
    BOOST_REQUIRE(WaitFor([&] { return socket_destroyed.load(); }));
    BOOST_CHECK_EQUAL(connects.load(), 0U);
    worker->Stop();
    BOOST_CHECK_EQUAL(connects.load(), 0U);
    BOOST_CHECK_EQUAL(worker->Status()["attempts"].getInt<uint64_t>(), 0U);
    BOOST_CHECK_EQUAL(worker->Status()["domain_rounds"].getInt<uint64_t>(), 0U);
}

BOOST_AUTO_TEST_CASE(unsupported_local_family_is_cached_without_tcp_attempt_or_efficiency_sample)
{
    FakeSteadyClock clock;
    auto* chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    for (uint32_t index = 0; index < 2; ++index) {
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(COutPoint{Txid::FromUint256(uint256::ONE), index},
            Coin{CTxOut{COIN, PayToDomainOutput{index ? "second.example" : "first.example", uint256::FromHex(std::string(64, 'f')).value(), 1}}, 100, false}, false));
    }
    RestoreNetwork restore;
    const auto address{LookupHost("2001:4860:4860::8888", false, restore.dns).value()};
    g_dns_lookup = [&](const std::string&, bool) { return std::vector<CNetAddr>{address}; };
    std::atomic<unsigned> allocations{0};
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        ++allocations;
#ifdef WIN32
        WSASetLastError(WSAEAFNOSUPPORT);
#else
        errno = EAFNOSUPPORT;
#endif
        return nullptr;
    };
    auto worker{MakeP2CClaimWorker(*wallet)};
    BOOST_REQUIRE(worker->Configure(-1, 4));
    BOOST_REQUIRE(WaitFor([&] { return allocations.load() == 1; }));
    auto before{worker->Status()["schedule_refreshes"].getInt<uint64_t>()};
    clock += 5s;
    BOOST_REQUIRE(WaitFor([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > before; }));
    std::this_thread::sleep_for(150ms);
    BOOST_CHECK_EQUAL(allocations.load(), 1U); // Cached across both domains.
    BOOST_CHECK_EQUAL(worker->Status()["attempts"].getInt<uint64_t>(), 0U);
    BOOST_CHECK_EQUAL(worker->Status()["domain_rounds"].getInt<uint64_t>(), 0U);
    before = worker->Status()["schedule_refreshes"].getInt<uint64_t>();
    clock += 60s;
    BOOST_REQUIRE(WaitFor([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > before; }));
    BOOST_REQUIRE(WaitFor([&] { return allocations.load() == 2; }));
    worker->Stop();
    BOOST_CHECK_EQUAL(worker->Status()["attempts"].getInt<uint64_t>(), 0U);
}

BOOST_AUTO_TEST_CASE(ordinary_assignment_cannot_bypass_new_recovery_classification)
{
    FakeSteadyClock clock;
    auto* chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    wallet->m_default_max_tx_fee = MAX_MONEY;
    const COutPoint outpoint{Txid::FromUint256(uint256::ONE), 0};
    const auto target{uint256::FromHex(std::string(6, '0') + "3" + std::string(57, 'f')).value()};
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoint,
        Coin{CTxOut{4 * COIN, PayToDomainOutput{"class.example", target, 1}}, 100, false}, false));
    CCoinControl control;
    control.m_feerate = CFeeRate{1000};
    const auto proposal{PrepareP2CClaim(*wallet, outpoint, control)};
    BOOST_REQUIRE(proposal);
    CMutableTransaction tx{*proposal->tx};
    // Barely profitable at the 5/s prior; one measured failure moves it below
    // the ordinary floor while the next selected socket is not yet admitted.
    tx.vout[0].nValue = 1000 * (CAmount{1} << 26) / 5 + COIN / 1000;
    UniValue saved{UniValue::VOBJ}, pending{UniValue::VARR};
    pending.push_back(EncodeHexTx(CTransaction{tx}));
    saved.pushKV("pending", std::move(pending));
    saved.pushKV("ready", "");
    saved.pushKV("proof", "");
    saved.pushKV("attempts", 0);
    saved.pushKV("submitted", 0);
    saved.pushKV("last_txid", "");
    BOOST_REQUIRE(wallet->GetDatabase().MakeBatch()->Write(std::string{"p2c_claim_worker_v1"}, saved.write()));
    RestoreNetwork restore;
    const auto address{LookupHost("8.8.8.8", false, restore.dns).value()};
    g_dns_lookup = [&](const std::string&, bool) { return std::vector<CNetAddr>{address}; };
    std::atomic<unsigned> allocations{0}, first_connects{0}, stale_connects{0}, later_connects{0};
    std::atomic<bool> first_destroyed{false}, stale_destroyed{false}, later_destroyed{false};
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool release_first{false}, release_stale{false};
    auto worker{MakeP2CClaimWorker(*wallet)};
    struct ReleaseAndStop {
        std::mutex& mutex;
        std::condition_variable& gate;
        bool& first;
        bool& stale;
        P2CClaimWorker& worker;
        ~ReleaseAndStop() {
            { std::lock_guard lock{mutex}; first = stale = true; }
            gate.notify_all();
            worker.Stop();
        }
    } release{gate_mutex, gate, release_first, release_stale, *worker};
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        const auto index{++allocations};
        if (index == 1) return std::make_unique<CountConnect>(first_connects, first_destroyed, [&] {
            std::unique_lock lock{gate_mutex};
            gate.wait(lock, [&] { return release_first; });
            lock.unlock();
            std::this_thread::sleep_for(50ms);
        });
        if (index == 2) {
            std::unique_lock lock{gate_mutex};
            gate.wait(lock, [&] { return release_stale; });
            return std::make_unique<CountConnect>(stale_connects, stale_destroyed);
        }
        return std::make_unique<CountConnect>(later_connects, later_destroyed);
    };
    BOOST_REQUIRE(worker->Configure(-1, 2));
    BOOST_REQUIRE(WaitFor([&] { return allocations.load() == 2 && first_connects.load() == 1; }));
    { std::lock_guard lock{gate_mutex}; release_first = true; }
    gate.notify_all();
    BOOST_REQUIRE(WaitFor([&] { return worker->Status()["state"].get_str() == "retrying connections"; }));
    const auto before{worker->Status()["schedule_refreshes"].getInt<uint64_t>()};
    clock += 5s;
    BOOST_REQUIRE(WaitFor([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > before; }));
    { std::lock_guard lock{gate_mutex}; release_stale = true; }
    gate.notify_all();
    BOOST_REQUIRE(WaitFor([&] { return stale_destroyed.load(); }));
    BOOST_CHECK_EQUAL(stale_connects.load(), 0U);
    // A fresh reservation may now start, so rejecting the stale selection
    // neither disables the domain nor permanently holds the FIFO head.
    BOOST_REQUIRE(WaitFor([&] { return later_connects.load() >= 1; }));
    worker->Stop();
}

BOOST_AUTO_TEST_CASE(catalog_pause_drops_overdue_credit_but_capacity_waits_preserve_it)
{
    FakeSteadyClock clock;
    auto& manager{static_cast<TestChainstateManager&>(*m_node.chainman)};
    auto* chain{WITH_LOCK(cs_main, return &manager.ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    WITH_LOCK(cs_main, manager.ActiveChainstate().CoinsTip().AddCoin(COutPoint{Txid::FromUint256(uint256::ONE), 0},
        Coin{CTxOut{COIN, PayToDomainOutput{"pause.example", uint256::FromHex(std::string(64, 'f')).value(), 1}}, 100, false}, false));
    RestoreNetwork restore;
    const auto address{LookupHost("8.8.8.8", false, restore.dns).value()};
    g_dns_lookup = [&](const std::string&, bool) { return std::vector<CNetAddr>{address}; };
    std::atomic<unsigned> connects{0};
    std::atomic<bool> destroyed{false}, catalog_blocked{false};
    std::mutex events_mutex;
    std::vector<std::chrono::steady_clock::time_point> starts;
    auto worker{MakeP2CClaimWorker(*wallet)};
    struct StopAndRestore {
        P2CClaimWorker& worker;
        TestChainstateManager& manager;
        ~StopAndRestore() {
            worker.Stop();
            if (manager.IsInitialBlockDownload()) manager.JumpOutOfIbd();
        }
    } stop_and_restore{*worker, manager};
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        return std::make_unique<CountConnect>(connects, destroyed, [&] {
            bool first;
            { std::lock_guard lock{events_mutex}; starts.push_back(std::chrono::steady_clock::now()); first = starts.size() == 1; }
            if (first) {
                // Trigger scanP2CBounties(false) with the existing chainstate
                // test helper, without adding a production injection seam.
                manager.ResetIbd();
                clock += 5s;
                catalog_blocked = true;
            }
        });
    };
    BOOST_REQUIRE(worker->Configure(2, 8));
    BOOST_REQUIRE(WaitFor([&] { return catalog_blocked.load(); }));
    std::this_thread::sleep_for(2s); // Deliberately exceeds the 500ms pacing interval.
    BOOST_CHECK_EQUAL(connects.load(), 1U);
    manager.JumpOutOfIbd();
    clock += 5s;
    BOOST_REQUIRE(WaitFor([&] { std::lock_guard lock{events_mutex}; return starts.size() >= 3; }));
    worker->Stop();
    {
        std::lock_guard lock{events_mutex};
        // Both resumed workers were free throughout the pause. An unre-based
        // deadline would let them spend stale credit almost back-to-back.
        BOOST_CHECK(starts[2] - starts[1] >= 350ms);
    }

    // The capacity-only prepared fallback deliberately does NOT mark idle.
    // Protect the corresponding limiter policy without a 257-thread fixture.
    P2CConnectionRateLimiter capacity;
    using Clock = P2CConnectionRateLimiter::Clock;
    const auto initial{Clock::time_point{} + 10s};
    capacity.RecordStart(2, initial);
    const auto resumed{initial + 2s};
    capacity.RecordStart(2, resumed);
    BOOST_CHECK(!capacity.BlockedUntil(resumed));
    P2CConnectionRateLimiter unavailable;
    unavailable.RecordStart(2, initial);
    unavailable.MarkIdle();
    unavailable.RecordStart(2, resumed);
    BOOST_REQUIRE(unavailable.BlockedUntil(resumed));
    BOOST_CHECK(*unavailable.BlockedUntil(resumed) == resumed + 500ms);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
