// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <coins.h>
#include <consensus/p2c_x509.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <hash.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <netbase.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <test/util/net.h>
#include <test/util/random.h>
#include <test/util/script.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>
#include <univalue.h>
#include <util/translation.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/p2c_claim.h>
#include <wallet/p2c_domain_stats.h>
#include <wallet/p2c_tls.h>
#include <wallet/p2c_tls_lifecycle.h>
#include <wallet/p2c_worker.h>
#include <wallet/p2c_worker_threads.h>
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
#include <span>
#include <stdexcept>
#include <system_error>
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

BOOST_AUTO_TEST_CASE(worker_thread_pool_keeps_partial_capacity)
{
    P2CThreadPoolGrowth growth;
    std::vector<std::thread> connections;
    std::mutex mutex;
    std::condition_variable wake;
    bool released{false};
    unsigned started{0};
    unsigned finished{0};
    struct ReleaseAndJoin {
        std::vector<std::thread>& connections;
        std::mutex& mutex;
        std::condition_variable& wake;
        bool& released;
        ~ReleaseAndJoin()
        {
            { std::lock_guard lock{mutex}; released = true; }
            wake.notify_all();
            for (auto& thread : connections) if (thread.joinable()) thread.join();
        }
    } release_and_join{connections, mutex, wake, released};
    unsigned creations{0};
    const auto create = [&]() -> std::thread {
        if (++creations == 3) throw std::system_error{std::make_error_code(std::errc::resource_unavailable_try_again)};
        return std::thread{[&] {
            std::unique_lock lock{mutex};
            ++started;
            wake.notify_all();
            wake.wait(lock, [&] { return released; });
            ++finished;
        }};
    };
    const auto limited{growth.Grow(connections, 1000, [] { return true; }, create)};
    BOOST_REQUIRE(limited);
    BOOST_CHECK(!limited->empty());
    BOOST_CHECK_EQUAL(connections.size(), 2U);
    BOOST_CHECK_EQUAL(creations, 3U);
    {
        std::unique_lock lock{mutex};
        BOOST_REQUIRE(wake.wait_for(lock, std::chrono::seconds{5}, [&] { return started == 2; }));
        BOOST_CHECK_EQUAL(finished, 0U); // Partial workers continue, not cancelled.
    }
    for (int i = 0; i < 10; ++i) {
        BOOST_CHECK(!growth.Grow(connections, 1000, [] { return true; }, create));
    }
    BOOST_CHECK_EQUAL(creations, 3U); // No retry loop after a resource failure.
    BOOST_CHECK(connections[0].joinable());
    BOOST_CHECK(connections[1].joinable());
}

BOOST_AUTO_TEST_CASE(worker_thread_pool_first_creation_failure_propagates)
{
    P2CThreadPoolGrowth growth;
    std::vector<std::thread> connections;
    unsigned creations{0};
    BOOST_CHECK_THROW(growth.Grow(connections, 1000, [] { return true; }, [&]() -> std::thread {
        ++creations;
        throw std::system_error{std::make_error_code(std::errc::resource_unavailable_try_again)};
    }), std::system_error);
    BOOST_CHECK_EQUAL(creations, 1U);
    BOOST_CHECK(connections.empty());
}

BOOST_AUTO_TEST_CASE(worker_thread_pool_does_not_swallow_unexpected_errors)
{
    P2CThreadPoolGrowth growth;
    std::vector<std::thread> connections;
    BOOST_CHECK_THROW(growth.Grow(connections, 1000, [] { return true; }, []() -> std::thread {
        throw std::runtime_error{"unexpected factory failure"};
    }), std::runtime_error);
    BOOST_CHECK(connections.empty());
    unsigned creations{0};
    // A system_error outside the actual thread-creation callback is not an
    // OS capacity failure and must propagate without attempting creation.
    BOOST_CHECK_THROW(growth.Grow(connections, 1000, []() -> bool {
        throw std::system_error{std::make_error_code(std::errc::io_error)};
    }, [&] { ++creations; return std::thread{}; }), std::system_error);
    BOOST_CHECK_EQUAL(creations, 0U);
    BOOST_CHECK(connections.empty());
}

BOOST_AUTO_TEST_CASE(worker_thread_pool_stops_growth_on_cancellation)
{
    P2CThreadPoolGrowth growth;
    std::vector<std::thread> connections;
    bool stopped{true};
    unsigned creations{0};
    const auto create = [&] {
        ++creations;
        stopped = true;
        return std::thread{}; // No OS resources needed to test stop boundaries.
    };
    BOOST_CHECK(!growth.Grow(connections, 1000, [&] { return !stopped; }, create));
    BOOST_CHECK_EQUAL(creations, 0U);
    stopped = false;
    BOOST_CHECK(!growth.Grow(connections, 1000, [&] { return !stopped; }, create));
    BOOST_CHECK_EQUAL(creations, 1U);
    BOOST_CHECK_EQUAL(connections.size(), 1U);
    BOOST_CHECK(!growth.Grow(connections, 1000, [&] { return !stopped; }, create));
    BOOST_CHECK_EQUAL(creations, 1U);
}

BOOST_AUTO_TEST_CASE(tls_capture_v2_authenticates_but_does_not_hash_certificate_verify)
{
    RestoreSocketFactory restore_sockets;
    const auto endpoint{Lookup("8.8.8.8", 443, false)};
    BOOST_REQUIRE(endpoint);
    const auto config{std::make_shared<test::P2CTLSServer>()};
    config->Setup();
    CreateSock = [config](int, int, int) -> std::unique_ptr<Sock> {
        return std::make_unique<test::P2CTLSSocket>(config, std::vector<std::string>{}, 127);
    };
    // All I/O goes to the in-memory TLS fixture, never to this IP address.
    const uint256 challenge{uint256::ONE};
    const auto captured{CaptureP2CTls(*endpoint, "localhost", challenge, [] { return false; })};
    BOOST_REQUIRE_MESSAGE(captured, util::ErrorString(captured).original);
    BOOST_REQUIRE(!captured->empty());
    BOOST_CHECK_EQUAL(captured->front(), 2U);
    P2CTlsProofView parsed;
    std::string error;
    BOOST_REQUIRE(ParseP2CTlsProof(*captured, "localhost", challenge, parsed, error));
    BOOST_REQUIRE(!parsed.certificate_verify_signature.empty());

    // Independently assemble the v2 candidate from the four signed messages:
    // no proof-version byte, CertificateVerify header, scheme, length, or signature.
    auto work{TaggedHash("ConnectCoin/P2C/work/v2")};
    for (const auto message : {parsed.client_hello, parsed.server_hello,
                               parsed.encrypted_extensions, parsed.certificate}) {
        work.write(std::as_bytes(message));
    }
    BOOST_CHECK(parsed.connection_work_hash == work.GetSHA256());
    const CTxOut output{COIN, PayToDomainOutput{"localhost", parsed.connection_work_hash, 1}};
    const std::span<const unsigned char> roots{test::P2C_TEST_ROOTS_PEM, sizeof(test::P2C_TEST_ROOTS_PEM) - 1};
    BOOST_REQUIRE_MESSAGE(VerifyP2CCertificateProofForTest(output, parsed, 1800000000, roots, error), error);
    BOOST_CHECK(P2CMeetsWorkTarget(parsed.connection_work_hash, parsed.connection_work_hash));

    // Excluding the signature from work must never make signature verification
    // optional: a changed signature keeps the candidate but fails authentication.
    auto invalid_signature{*captured};
    invalid_signature.back() ^= 1;
    P2CTlsProofView modified;
    BOOST_REQUIRE(ParseP2CTlsProof(invalid_signature, "localhost", challenge, modified, error));
    BOOST_CHECK(modified.connection_work_hash == parsed.connection_work_hash);
    BOOST_CHECK(modified.transcript_hash == parsed.transcript_hash);
    BOOST_CHECK(!VerifyP2CCertificateProofForTest(output, modified, 1800000000, roots, error));

    auto legacy{*captured};
    legacy.front() = 1;
    BOOST_CHECK(!ParseP2CTlsProof(legacy, "localhost", challenge, modified, error));
    BOOST_CHECK(error.find("unsupported P2C proof version") != std::string::npos);
}

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

BOOST_AUTO_TEST_CASE(expected_return_floor_uses_net_payout_and_domain_efficiency)
{
    const auto maximum{uint256::FromHex(std::string(64, 'f')).value()};
    const auto half{uint256::FromHex("7" + std::string(63, 'f')).value()};
    BOOST_CHECK(!IsP2CClaimWorthAttempting(GetP2CClaimPriority(maximum, 199), 5.0));
    BOOST_CHECK(IsP2CClaimWorthAttempting(GetP2CClaimPriority(maximum, 200), 5.0));
    BOOST_CHECK(IsP2CClaimWorthAttempting(GetP2CClaimPriority(half, 400), 5.0));
    BOOST_CHECK(!IsP2CClaimWorthAttempting(GetP2CClaimPriority(maximum, 200), std::nextafter(5.0, 0.0)));
    BOOST_CHECK(IsP2CClaimWorthAttempting(GetP2CClaimPriority(maximum, 200), std::nextafter(5.0, 6.0)));
    BOOST_CHECK(!IsP2CClaimWorthAttempting(GetP2CClaimPriority(maximum, 1000 - 801), 5.0));
    BOOST_CHECK(!IsP2CClaimWorthAttempting(GetP2CClaimPriority(maximum, 0), 5005.0));
    BOOST_CHECK(IsP2CClaimWorthAttempting(GetP2CClaimPriority(maximum, MAX_MONEY), 5005.0));
    for (const double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        BOOST_CHECK(!IsP2CClaimWorthAttempting(GetP2CClaimPriority(maximum, MAX_MONEY), invalid));
    }
    // Even the largest target with 64 leading zero bits and the largest payout
    // at the maximum smoothed capture rate is below the floor (about 271/s).
    for (const auto& target : {uint256{}, uint256::FromHex(std::string(16, '0') + std::string(48, 'f')).value()}) {
        BOOST_CHECK_EQUAL(target.GetUint64(3), 0U);
        BOOST_CHECK(!IsP2CClaimWorthAttempting(GetP2CClaimPriority(target, MAX_MONEY), 5005.0));
    }
    // A nonzero high word does not automatically make a bounty economical.
    const auto tiny{uint256::FromHex("0000000000000001" + std::string(48, 'f')).value()};
    BOOST_CHECK_NE(tiny.GetUint64(3), 0U);
    BOOST_CHECK(!IsP2CClaimWorthAttempting(GetP2CClaimPriority(tiny, MAX_MONEY), 5005.0));
}

BOOST_AUTO_TEST_CASE(successful_connection_limit_is_strict_and_overflow_safe)
{
    for (unsigned bits = 0; bits <= 62; ++bits) {
        // target = 2^(256-bits)-1, p = 2^-bits, 2*E = 2^(bits+1).
        uint256 target;
        for (unsigned bit = 0; bit < 256 - bits; ++bit) target.begin()[bit / 8] |= 1U << (bit % 8);
        const uint64_t twice_expected{uint64_t{1} << (bits + 1)};
        BOOST_CHECK(!IsP2CClaimConnectionLimitExceeded(target, twice_expected - 1));
        BOOST_CHECK(!IsP2CClaimConnectionLimitExceeded(target, twice_expected));
        BOOST_CHECK(IsP2CClaimConnectionLimitExceeded(target, twice_expected + 1));
    }
    const uint64_t maximum{std::numeric_limits<uint64_t>::max()};
    BOOST_CHECK(!IsP2CClaimConnectionLimitExceeded(uint256{}, maximum));
    BOOST_CHECK(!IsP2CClaimConnectionLimitExceeded(uint256::FromHex(std::string(16, '0') + std::string(48, 'f')).value(), maximum));
    const auto above_half{uint256::FromHex("8" + std::string(63, '0')).value()};
    BOOST_CHECK(!IsP2CClaimConnectionLimitExceeded(above_half, 3));
    BOOST_CHECK(IsP2CClaimConnectionLimitExceeded(above_half, 4));
    const auto below_half{uint256::FromHex("7" + std::string(62, 'f') + "e").value()};
    BOOST_CHECK(!IsP2CClaimConnectionLimitExceeded(below_half, 4));
    BOOST_CHECK(IsP2CClaimConnectionLimitExceeded(below_half, 5));
    // Independent bytewise shift/add oracle, including full-width counters.
    for (int trial = 0; trial < 256; ++trial) {
        const auto target{m_rng.rand256()};
        const uint64_t count{m_rng.rand64()};
        std::array<unsigned char, 40> factor{}, product{}, threshold{};
        std::copy(target.begin(), target.end(), factor.begin());
        unsigned carry{1};
        for (auto& byte : factor) {
            const unsigned sum{byte + carry};
            byte = static_cast<unsigned char>(sum & 255);
            carry = sum >> 8;
        }
        for (unsigned bit = 0; bit < 64; ++bit) {
            if ((count >> bit) & 1) {
                carry = 0;
                for (size_t i = 0; i < product.size(); ++i) {
                    const unsigned sum{product[i] + factor[i] + carry};
                    product[i] = static_cast<unsigned char>(sum & 255);
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
        threshold[32] = 2;
        const bool exceeded{std::lexicographical_compare(threshold.rbegin(), threshold.rend(), product.rbegin(), product.rend())};
        BOOST_CHECK_EQUAL(IsP2CClaimConnectionLimitExceeded(target, count), exceeded);
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

BOOST_AUTO_TEST_CASE(tls_complete_handshake_obeys_mask_and_preserves_proof)
{
    RestoreSocketFactory restore_sockets;
    const auto endpoint{Lookup("8.8.8.8", 443, false)};
    BOOST_REQUIRE(endpoint);
    const auto config{std::make_shared<test::P2CTLSServer>()};
    config->Setup();
    CreateSock = [config](int, int, int) -> std::unique_ptr<Sock> {
        return std::make_unique<test::P2CTLSSocket>(config, std::vector<std::string>{}, 23);
    };
    const auto complete{CaptureP2CTls(*endpoint, "localhost", uint256::ONE, [] { return false; },
                                    {.signature_algorithms_mask = 1, .complete_handshake = true})};
    BOOST_REQUIRE_MESSAGE(complete, util::ErrorString(complete).original);
    P2CTlsProofView proof;
    std::string error;
    // Finished is verified by TLS but must not leak into the proof format.
    BOOST_REQUIRE(ParseP2CTlsProof(*complete, "localhost", uint256::ONE, proof, error));
    BOOST_CHECK_EQUAL(proof.certificate_verify_scheme, 0x0403);
    BOOST_CHECK(!CaptureP2CTls(*endpoint, "localhost", uint256::ONE, [] { return false; },
                             {.signature_algorithms_mask = 6, .complete_handshake = true}));
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
    BOOST_CHECK(!CaptureP2CTls(*public_endpoint, "example.com", uint256{}, [] { return false; },
                             {.signature_algorithms_mask = 0}));
    BOOST_CHECK(!CaptureP2CTls(*public_endpoint, "example.com", uint256{}, [] { return false; },
                             {.deadline = std::chrono::steady_clock::now() - std::chrono::seconds{1}}));
    BOOST_CHECK_EQUAL(calls, 0U);
}

BOOST_AUTO_TEST_CASE(tls_client_hello_advertises_only_output_mask)
{
    RestoreSocketFactory restore;
    const auto endpoint{Lookup("8.8.8.8", 443, false)};
    BOOST_REQUIRE(endpoint);
    for (uint8_t mask = 1; mask <= 7; ++mask) {
        std::vector<unsigned char> sent;
        CreateSock = [&](int, int, int) { return std::make_unique<ClientHelloSocket>(sent, 13); };
        BOOST_CHECK(!CaptureP2CTls(*endpoint, "example.com", uint256::ONE, [] { return false; },
                                  {.signature_algorithms_mask = mask}));
        BOOST_REQUIRE(sent.size() > 44);
        size_t pos{43};
        const auto read8 = [&]() -> size_t { BOOST_REQUIRE(pos < sent.size()); return sent[pos++]; };
        const auto read16 = [&]() -> size_t { const auto high{read8()}; return high * 256 + read8(); };
        const auto skip = [&](size_t size) { BOOST_REQUIRE(size <= sent.size() - pos); pos += size; };
        skip(read8());
        skip(read16());
        skip(read8());
        const auto extensions_length{read16()};
        BOOST_REQUIRE_EQUAL(extensions_length, sent.size() - pos);
        std::vector<uint16_t> advertised;
        while (pos < sent.size()) {
            const auto type{read16()};
            const auto length{read16()};
            BOOST_REQUIRE(length <= sent.size() - pos);
            if (type == 13) {
                BOOST_REQUIRE(length >= 2);
                const auto list_size{read16()};
                BOOST_REQUIRE_EQUAL(list_size, length - 2);
                BOOST_REQUIRE_EQUAL(list_size % 2, 0U);
                for (size_t i = 0; i < list_size; i += 2) advertised.push_back(read16());
            } else {
                skip(length);
            }
        }
        std::vector<uint16_t> expected;
        if (mask & 1) expected.push_back(0x0403);
        if (mask & 2) expected.push_back(0x0804);
        if (mask & 4) expected.push_back(0x0809);
        BOOST_CHECK_EQUAL_COLLECTIONS(advertised.begin(), advertised.end(), expected.begin(), expected.end());
    }
}

BOOST_AUTO_TEST_CASE(rsa_probe_cancellation_after_dns_never_connects)
{
    RestoreSocketFactory restore_socket;
    RestoreDNSLookup restore_dns;
    unsigned sockets{0};
    unsigned lookups{0};
    bool cancelled{false};
    const auto address{LookupHost("8.8.8.8", false)};
    BOOST_REQUIRE(address);
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> { ++sockets; return nullptr; };
    g_dns_lookup = [&](const std::string&, bool) {
        ++lookups;
        cancelled = true;
        return std::vector<CNetAddr>{*address};
    };
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{3}};
    BOOST_CHECK(!ProbeP2CRsa("example.com", 1, [&] { return cancelled; }, deadline));
    BOOST_CHECK_EQUAL(lookups, 1U);
    BOOST_CHECK_EQUAL(sockets, 0U);
    BOOST_CHECK(!ProbeP2CRsa("example.com", 1, [] { return false; }, std::chrono::steady_clock::now()));
    BOOST_CHECK_EQUAL(lookups, 1U);
}

BOOST_AUTO_TEST_CASE(rsa_probe_shutdown_drains_an_active_moved_phase)
{
    P2CRsaProbeLifecycle lifecycle;
    std::atomic<bool> stopped{false};
    std::thread stopper;
    {
        auto original{lifecycle.TryEnter()};
        BOOST_REQUIRE(original);
        auto phase{std::move(original)};
        // Phase's move constructor explicitly empties the source; verify that
        // contract so its destructor cannot release the same phase twice.
        // NOLINTNEXTLINE(bugprone-use-after-move)
        BOOST_CHECK(!original);
        BOOST_CHECK(phase);
        stopper = std::thread([&] {
            lifecycle.Stop();
            stopped.store(true);
        });
        // Observing the stop flag establishes that Stop has entered its
        // critical section. It cannot return while the moved phase is alive.
        while (!lifecycle.IsStopping()) std::this_thread::yield();
        BOOST_CHECK(!stopped.load());
        BOOST_CHECK(!lifecycle.TryEnter());
    }
    stopper.join();
    BOOST_CHECK(stopped.load());
    BOOST_CHECK(!lifecycle.TryEnter());
    lifecycle.Stop();
}

BOOST_AUTO_TEST_CASE(rsa_probe_shutdown_does_not_wait_for_dns_or_allow_late_work)
{
    RestoreSocketFactory restore_socket;
    RestoreDNSLookup restore_dns;
    P2CRsaProbeLifecycle lifecycle;
    std::mutex mutex;
    std::condition_variable cv;
    bool dns_entered{false};
    bool release_dns{false};
    bool result{true};
    unsigned sockets{0};
    unsigned late_dns_calls{0};
    const auto address{LookupHost("8.8.8.8", false)};
    BOOST_REQUIRE(address);
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> { ++sockets; return nullptr; };
    g_dns_lookup = [&](const std::string&, bool) {
        std::unique_lock lock{mutex};
        dns_entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_dns; });
        return std::vector<CNetAddr>{*address};
    };
    const auto deadline{std::chrono::steady_clock::time_point::max()};
    std::thread probe([&] {
        result = ProbeP2CRsaForTest("example.com", 1, [] { return false; }, deadline, lifecycle);
    });
    {
        std::unique_lock lock{mutex};
        cv.wait(lock, [&] { return dns_entered; });
    }
    // This returns while the resolver is still blocked. No sleeps or network
    // are needed to establish that shutdown does not join the DNS phase.
    lifecycle.Stop();
    BOOST_CHECK(lifecycle.IsStopping());
    BOOST_CHECK(!lifecycle.TryEnter());
    g_dns_lookup = [&](const std::string&, bool) {
        ++late_dns_calls;
        return std::vector<CNetAddr>{};
    };
    {
        const std::lock_guard lock{mutex};
        release_dns = true;
    }
    cv.notify_all();
    probe.join();
    BOOST_CHECK(!result);
    BOOST_CHECK_EQUAL(sockets, 0U);
    BOOST_CHECK(!ProbeP2CRsaForTest("example.com", 1, [] { return false; }, deadline, lifecycle));
    BOOST_CHECK_EQUAL(late_dns_calls, 0U);
    lifecycle.Stop(); // Repeated application shutdown is harmless.
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
    const CTxDestination external{WitnessV1Taproot{*tx.vout[0].GetP2PKPubKey()}};
    BOOST_REQUIRE(ResumeP2CClaim(m_wallet, CTransaction{tx}, external));
    BOOST_CHECK(!ResumeP2CClaim(m_wallet, CTransaction{tx}, WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}}));
    BOOST_CHECK(!IsP2CClaimPayout(m_wallet, tx.vout[0], CNoDestination{}));
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
        Coin{CTxOut{COIN, PayToDomainOutput{"example.com", uint256::FromHex("0000" + std::string(60, 'f')).value(), 1}}, 100, false}, false));
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
    BOOST_CHECK_EQUAL(worker->Status()["concurrency"].getInt<int>(), 100);
    BOOST_CHECK_EQUAL(worker->Status()["connections_per_second"].getInt<int>(), 0);
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

BOOST_FIXTURE_TEST_CASE(worker_configurable_bounty_lookback, TestChain100Setup)
{
    using namespace std::chrono_literals;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    const COutPoint old{Txid::FromUint256(uint256::ONE), 0};
    const COutPoint latest{Txid::FromUint256(uint256::ONE), 1};
    const auto target{uint256::FromHex(std::string(64, 'f')).value()};
    {
        LOCK(cs_main);
        auto& coins{m_node.chainman->ActiveChainstate().CoinsTip()};
        coins.AddCoin(old, Coin{CTxOut{COIN, PayToDomainOutput{"old.example", target, 1}}, 0, false}, false);
        coins.AddCoin(latest, Coin{CTxOut{COIN, PayToDomainOutput{"tip.example", target, 1}}, 100, false}, false);
    }
    // Locked outputs remain visible to discovery but cannot initiate DNS/TLS.
    WITH_LOCK(wallet->cs_wallet, BOOST_REQUIRE(wallet->LockCoin(old, false)));
    WITH_LOCK(wallet->cs_wallet, BOOST_REQUIRE(wallet->LockCoin(latest, false)));
    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    std::atomic<unsigned> network_calls{0};
    g_dns_lookup = [&](const std::string&, bool) { ++network_calls; return std::vector<CNetAddr>{}; };
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> { ++network_calls; return nullptr; };
    auto worker{MakeP2CClaimWorker(*wallet)};
    const auto wait_for_state = [&](const std::string& expected) {
        const auto deadline{std::chrono::steady_clock::now() + 15s};
        while (worker->Status()["state"].get_str() != expected) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    };
    BOOST_CHECK_EQUAL(DEFAULT_P2C_BOUNTY_LOOKBACK, 600);
    BOOST_CHECK_EQUAL(worker->Status()["recent_blocks"].getInt<int>(), 600);
    BOOST_REQUIRE(worker->Configure(-1, 1, {"old.example"}));
    BOOST_REQUIRE(wait_for_state("waiting for eligible bounties"));
    for (const int lookback : {1, 100, 101, 0, std::numeric_limits<int>::max(), 100, 0}) {
        BOOST_REQUIRE(worker->Configure(-1, 1, {"old.example"}, {}, lookback));
        BOOST_CHECK_EQUAL(worker->Status()["recent_blocks"].getInt<int>(), lookback);
        BOOST_REQUIRE(wait_for_state(lookback > 0 && lookback < 101 ? "waiting for bounties" : "waiting for eligible bounties"));
    }
    BOOST_REQUIRE(worker->Configure(-1, 1, {"tip.example"}, {}, 1));
    BOOST_REQUIRE(wait_for_state("waiting for eligible bounties"));
    for (const int invalid : {-1, std::numeric_limits<int>::min()}) {
        BOOST_CHECK(!worker->Configure(0, 2, {}, {}, invalid));
        // Invalid configuration is atomic: leave the running worker unchanged.
        BOOST_CHECK_EQUAL(worker->Status()["recent_blocks"].getInt<int>(), 1);
        BOOST_CHECK_EQUAL(worker->Status()["connections_per_second"].getInt<int>(), -1);
        BOOST_CHECK_EQUAL(worker->Status()["concurrency"].getInt<int>(), 1);
    }
    worker->Stop();
    BOOST_CHECK_EQUAL(worker->Status()["recent_blocks"].getInt<int>(), 1);
    BOOST_REQUIRE(worker->Configure(0, 1));
    BOOST_CHECK_EQUAL(worker->Status()["recent_blocks"].getInt<int>(), 600);
    BOOST_CHECK_EQUAL(network_calls.load(), 0U);
    BOOST_CHECK_EQUAL(worker->Status()["attempts"].getInt<uint64_t>(), 0U);
    worker = MakeP2CClaimWorker(*wallet);
    BOOST_CHECK_EQUAL(worker->Status()["recent_blocks"].getInt<int>(), 600);
}

BOOST_FIXTURE_TEST_CASE(worker_age_window_does_not_cancel_inflight_tls, TestChain100Setup)
{
    using namespace std::chrono_literals;
    FakeSteadyClock clock;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    const COutPoint outpoint{Txid::FromUint256(uint256::ONE), 0};
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoint,
        Coin{CTxOut{COIN, PayToDomainOutput{"aging.example", uint256::FromHex(std::string(64, 'f')).value(), 1}}, 100, false}, false));
    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    const auto address{LookupHost("8.8.8.8", false, restore_dns.original)};
    BOOST_REQUIRE(address);
    g_dns_lookup = [&](const std::string&, bool) { return std::vector<CNetAddr>{*address}; };
    const auto config{std::make_shared<test::P2CTLSServer>()};
    config->Setup();
    std::atomic<unsigned> sockets{0};
    std::mutex pending_mutex;
    std::condition_variable release;
    bool released{false};
    auto worker{MakeP2CClaimWorker(*wallet)};
    struct ReleaseAndStop {
        std::mutex& mutex;
        std::condition_variable& wake;
        bool& released;
        P2CClaimWorker& worker;
        ~ReleaseAndStop() {
            { std::lock_guard lock{mutex}; released = true; }
            wake.notify_all();
            worker.Stop();
        }
    } release_and_stop{pending_mutex, release, released, *worker};
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        ++sockets;
        std::unique_lock lock{pending_mutex};
        release.wait(lock, [&] { return released; });
        return std::make_unique<test::P2CTLSSocket>(config, std::vector<std::string>{}, 127);
    };
    const auto wait_until = [](const auto& predicate) {
        const auto deadline{std::chrono::steady_clock::now() + 15s};
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    };
    // A one-block window tests aging without mining hundreds of extra blocks.
    BOOST_REQUIRE(worker->Configure(-1, 1, {}, {}, 1));
    BOOST_REQUIRE(wait_until([&] { return sockets == 1; }));
    // The catalog advances using real block deltas, without resetting the
    // assigned challenge. Height 100 is now just outside the one-block window.
    CreateAndProcessBlock({}, GetScriptForP2PKOutput(coinbaseKey));
    const auto previous{worker->Status()["schedule_refreshes"].getInt<uint64_t>()};
    clock += 5s;
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > previous; }));
    BOOST_CHECK_EQUAL(worker->Status()["state"].get_str(), "waiting for bounties");
    { std::lock_guard lock{pending_mutex}; released = true; }
    release.notify_all();
    BOOST_REQUIRE(wait_until([&] { return !worker->Status()["last_error"].get_str().empty(); }));
    // Completing capture and reaching certificate validation proves the age
    // refresh did NOT cancel TLS. The fixture root is intentionally untrusted.
    BOOST_CHECK_EQUAL(worker->Status()["state"].get_str(), "certificate rejected");
    worker->Stop();
    BOOST_CHECK_EQUAL(sockets.load(), 1U);
}

BOOST_FIXTURE_TEST_CASE(worker_success_budget_ignores_failures_and_is_per_bounty_memory_only, TestChain100Setup)
{
    using namespace std::chrono_literals;
    FakeSteadyClock clock;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    const auto half{uint256::FromHex("7" + std::string(63, 'f')).value()};
    for (uint32_t i = 0; i < 2; ++i) {
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(
            COutPoint{Txid::FromUint256(uint256::ONE), i},
            Coin{CTxOut{COIN, PayToDomainOutput{"budget.example", half, 1}}, 100, false}, false));
    }
    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    const auto address{LookupHost("8.8.8.8", false, restore_dns.original)};
    BOOST_REQUIRE(address);
    g_dns_lookup = [&](const std::string&, bool) { return std::vector<CNetAddr>{*address}; };
    const auto config{std::make_shared<test::P2CTLSServer>()};
    config->Setup();
    std::atomic<unsigned> sockets{0};
    std::vector<unsigned char> sent;
    auto worker{MakeP2CClaimWorker(*wallet)};
    struct StopBeforeLocals {
        std::unique_ptr<P2CClaimWorker>& worker;
        ~StopBeforeLocals() { if (worker) worker->Stop(); }
    } stop_before_locals{worker};
    static constexpr unsigned FAILED_CONNECTIONS{12};
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        const auto attempt{++sockets};
        if (attempt <= FAILED_CONNECTIONS) {
            if (attempt % 2 == 0) return nullptr; // TCP/socket failure.
            sent.clear();
            return std::make_unique<ClientHelloSocket>(sent, 127); // TLS EOF before CertificateVerify.
        }
        return std::make_unique<test::P2CTLSSocket>(config, std::vector<std::string>{}, 127);
    };
    const auto wait_until = [](const auto& predicate) {
        const auto deadline{std::chrono::steady_clock::now() + 30s};
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    };
    // p=1/2: four completed captures equal 2*E, the fifth exceeds it.
    // More than five failed attempts per bounty must not exhaust that budget.
    // The TLS fixture is deliberately untrusted by Core: captures count even
    // when subsequent certificate/claim validation cannot submit the reward.
    BOOST_REQUIRE(worker->Configure(-1, 1));
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["state"].get_str() == "waiting for eligible bounties"; }));
    BOOST_CHECK_EQUAL(sockets.load(), FAILED_CONNECTIONS + 10U);
    BOOST_CHECK_EQUAL(worker->Status()["attempts"].getInt<uint64_t>(), FAILED_CONNECTIONS + 10U);
    BOOST_CHECK_EQUAL(worker->Status()["submitted"].getInt<uint64_t>(), 0U);
    const auto previous{worker->Status()["schedule_refreshes"].getInt<uint64_t>()};
    clock += 5s;
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > previous; }));
    worker->Stop();
    // Neither reconfiguration nor replacing unsigned payout challenges resets
    // the in-memory counter for an outpoint.
    const auto destination{EncodeDestination(WitnessV1Taproot{XOnlyPubKey{GenerateRandomKey().GetPubKey()}})};
    BOOST_REQUIRE(worker->Configure(-1, 1, {}, destination));
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > 0; }));
    worker->Stop();
    BOOST_CHECK_EQUAL(sockets.load(), FAILED_CONNECTIONS + 10U);
    BOOST_REQUIRE(worker->Configure(-1, 1, {"unfunded.example"}));
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > 0; }));
    worker->Stop();
    BOOST_REQUIRE(worker->Configure(-1, 1));
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > 0; }));
    worker->Stop();
    BOOST_CHECK_EQUAL(sockets.load(), FAILED_CONNECTIONS + 10U); // Domain-filter changes do not reset budgets.

    // Once fully outside discovery and no longer in flight, counters are
    // collected. Use a one-block lookback to exercise the same cleanup/reorg
    // boundary without 600 RandomX blocks; the default is covered separately.
    CreateAndProcessBlock({}, GetScriptForP2PKOutput(coinbaseKey));
    BOOST_REQUIRE(worker->Configure(-1, 1, {}, {}, 1));
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["state"].get_str() == "waiting for bounties"; }));
    worker->Stop();
    BOOST_CHECK_EQUAL(sockets.load(), FAILED_CONNECTIONS + 10U);
    BlockValidationState state;
    auto* tip{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, tip));
    BOOST_REQUIRE(worker->Configure(-1, 1, {}, {}, 1));
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["state"].get_str() == "waiting for eligible bounties"; }));
    worker->Stop();
    BOOST_CHECK_EQUAL(sockets.load(), FAILED_CONNECTIONS + 20U);
    // A newly loaded worker has no persisted per-bounty budget. The aggregate
    // status counter remains durable, but does not restrict this new session.
    worker.reset();
    worker = MakeP2CClaimWorker(*wallet);
    BOOST_REQUIRE(worker->Configure(-1, 1));
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["state"].get_str() == "waiting for eligible bounties"; }));
    worker->Stop();
    BOOST_CHECK_EQUAL(sockets.load(), FAILED_CONNECTIONS + 30U);
}

BOOST_FIXTURE_TEST_CASE(worker_pending_and_failed_connections_do_not_reserve_success_budget, TestChain100Setup)
{
    using namespace std::chrono_literals;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    const auto half{uint256::FromHex("7" + std::string(63, 'f')).value()};
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(
        COutPoint{Txid::FromUint256(uint256::ONE), 0},
        Coin{CTxOut{COIN, PayToDomainOutput{"pending.example", half, 1}}, 100, false}, false));
    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    const auto address{LookupHost("8.8.8.8", false, restore_dns.original)};
    BOOST_REQUIRE(address);
    g_dns_lookup = [&](const std::string&, bool) { return std::vector<CNetAddr>{*address}; };
    std::atomic<unsigned> sockets{0};
    std::mutex pending_mutex;
    std::condition_variable release;
    bool released{false};
    auto worker{MakeP2CClaimWorker(*wallet)};
    struct ReleaseAndStop {
        std::mutex& mutex;
        std::condition_variable& wake;
        bool& released;
        P2CClaimWorker& worker;
        ~ReleaseAndStop() {
            { std::lock_guard lock{mutex}; released = true; }
            wake.notify_all();
            worker.Stop();
        }
    } release_and_stop{pending_mutex, release, released, *worker};
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        ++sockets;
        std::unique_lock lock{pending_mutex};
        release.wait(lock, [&] { return released; });
        return nullptr;
    };
    const auto wait_until = [](const auto& predicate) {
        const auto deadline{std::chrono::steady_clock::now() + 15s};
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    };
    // Five successes would exhaust p=1/2, but none of these eight in-flight
    // connections has completed yet. Concurrency is not a success reservation.
    BOOST_REQUIRE(worker->Configure(-1, 8));
    BOOST_REQUIRE(wait_until([&] { return sockets >= 8; }));
    BOOST_CHECK_EQUAL(sockets.load(), 8U);
    { std::lock_guard lock{pending_mutex}; released = true; }
    release.notify_all();
    // The whole first batch fails; another batch must still be allowed.
    BOOST_REQUIRE(wait_until([&] { return sockets >= 16; }));
    worker->Stop();
    BOOST_CHECK_GE(worker->Status()["attempts"].getInt<uint64_t>(), 16U);
    BOOST_CHECK_EQUAL(worker->Status()["submitted"].getInt<uint64_t>(), 0U);
}

BOOST_FIXTURE_TEST_CASE(worker_filters_each_bounty_before_network_and_rechecks_efficiency, TestChain100Setup)
{
    using namespace std::chrono_literals;
    FakeSteadyClock clock;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    wallet->m_default_max_tx_fee = MAX_MONEY;
    // Probability 2^-26: 1 CC net at the 5/s prior is below 1000 connects/s,
    // whereas 2 CC net is above it. Gross rewards alone would admit both.
    const auto target{uint256::FromHex(std::string(6, '0') + "3" + std::string(57, 'f')).value()};
    const std::array<uint256, 4> targets{uint256{},
        uint256::FromHex(std::string(16, '0') + std::string(48, 'f')).value(), target, target};
    const std::array<CAmount, 4> payouts{3 * COIN, 3 * COIN, COIN, 2 * COIN};
    std::vector<COutPoint> outpoints;
    UniValue saved{UniValue::VOBJ}, pending{UniValue::VARR};
    CCoinControl control;
    control.m_feerate = CFeeRate{1000};
    uint256 eligible_challenge;
    for (uint32_t i = 0; i < targets.size(); ++i) {
        outpoints.emplace_back(Txid::FromUint256(uint256::ONE), i);
        const std::string name{i == 0 ? "facebook.com" : "mixed.example"};
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoints.back(),
            Coin{CTxOut{4 * COIN, PayToDomainOutput{name, targets[i], 1}}, 100, false}, false));
        // Manual preparation remains allowed; the floor is only search policy.
        const auto prepared{PrepareP2CClaim(*wallet, outpoints.back(), control)};
        BOOST_REQUIRE(prepared);
        CMutableTransaction tx{*prepared->tx};
        tx.vout[0].nValue = payouts[i];
        pending.push_back(EncodeHexTx(CTransaction{tx}));
        if (i == 3) eligible_challenge = P2CClaimChallenge(CTransaction{tx}, 0);
    }
    saved.pushKV("pending", std::move(pending));
    saved.pushKV("ready", "");
    saved.pushKV("proof", "");
    saved.pushKV("attempts", 0);
    saved.pushKV("submitted", 0);
    saved.pushKV("last_txid", "");
    BOOST_REQUIRE(wallet->GetDatabase().MakeBatch()->Write(std::string{"p2c_claim_worker_v1"}, saved.write()));
    WITH_LOCK(wallet->cs_wallet, BOOST_REQUIRE(wallet->LockCoin(outpoints[3], false)));
    const COutPoint fresh{Txid::FromUint256(uint256::ONE), 4};
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(fresh,
        Coin{CTxOut{COIN, PayToDomainOutput{"mixed.example", uint256::FromHex(std::string(64, 'f')).value(), 1}}, 100, false}, false));
    WITH_LOCK(wallet->cs_wallet, BOOST_REQUIRE(wallet->LockCoin(fresh, false)));

    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    const auto address{LookupHost("8.8.8.8", false, restore_dns.original)};
    BOOST_REQUIRE(address);
    std::atomic<unsigned> lookups{0};
    std::atomic<bool> wrong_domain{false};
    g_dns_lookup = [&](const std::string& name, bool) {
        ++lookups;
        if (name != "mixed.example") wrong_domain = true;
        return std::vector<CNetAddr>{*address};
    };
    std::mutex sent_mutex;
    std::vector<std::vector<unsigned char>> hellos;
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        std::this_thread::sleep_for(100ms); // A measured, unsuccessful attempt.
        auto sent{std::make_shared<std::vector<unsigned char>>()};
        return std::make_unique<ClientHelloSocket>(*sent, 16384, [&, sent] {
            std::lock_guard lock{sent_mutex};
            hellos.push_back(*sent);
        });
    };
    const auto wait_until = [](const auto& predicate) {
        const auto deadline{std::chrono::steady_clock::now() + 15s};
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    };
    auto worker{MakeP2CClaimWorker(*wallet)};
    const auto refresh = [&] {
        const auto previous{worker->Status()["schedule_refreshes"].getInt<uint64_t>()};
        clock += 5s;
        return wait_until([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > previous; });
    };
    BOOST_REQUIRE(worker->Configure(-1, 1));
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["state"].get_str() == "waiting for eligible bounties"; }));
    BOOST_CHECK_EQUAL(lookups.load(), 0U);
    BOOST_CHECK_EQUAL(worker->Status()["attempts"].getInt<uint64_t>(), 0U);

    WITH_LOCK(wallet->cs_wallet, wallet->UnlockCoin(outpoints[3]));
    BOOST_REQUIRE(refresh());
    BOOST_REQUIRE(wait_until([&] {
        std::lock_guard lock{sent_mutex};
        return hellos.size() >= 2 && worker->Status()["state"].get_str() == "retrying connections";
    }));
    {
        std::lock_guard lock{sent_mutex};
        for (const auto& hello : hellos) {
            BOOST_REQUIRE(hello.size() >= 43);
            BOOST_CHECK_EQUAL_COLLECTIONS(hello.begin() + 11, hello.begin() + 43,
                                          eligible_challenge.begin(), eligible_challenge.end());
        }
    }
    // After the observed failures the formerly eligible bounty is below the
    // floor too; fair rotation may not keep probing it.
    BOOST_REQUIRE(refresh());
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["state"].get_str() == "waiting for eligible bounties"; }));
    const auto stopped_at{worker->Status()["attempts"].getInt<uint64_t>()};
    std::this_thread::sleep_for(1200ms);
    BOOST_CHECK_EQUAL(worker->Status()["attempts"].getInt<uint64_t>(), stopped_at);
    // A more profitable bounty becoming eligible revives the same domain.
    WITH_LOCK(wallet->cs_wallet, BOOST_REQUIRE(wallet->UnlockCoin(fresh)));
    BOOST_REQUIRE(refresh());
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["attempts"].getInt<uint64_t>() > stopped_at; }));
    worker->Stop();
    BOOST_CHECK(!wrong_domain);
}

BOOST_FIXTURE_TEST_CASE(worker_refresh_completion_arms_next_deadline, TestChain100Setup)
{
    using namespace std::chrono_literals;
    FakeSteadyClock clock;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    auto worker{MakeP2CClaimWorker(*wallet)};
    BOOST_REQUIRE(worker->Configure(-1, 1));
    // A completed refresh is a synchronization boundary: immediately advancing
    // five seconds must trigger the next refresh, even with no eligible work.
    // Do not keep advancing the clock in the wait loop or extend its timeout;
    // either would hide a deadline armed after completion became observable.
    for (uint64_t expected{1}; expected <= 21; ++expected) {
        const auto deadline{std::chrono::steady_clock::now() + 15s};
        while (worker->Status()["schedule_refreshes"].getInt<uint64_t>() < expected &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        const auto status{worker->Status()};
        BOOST_REQUIRE_EQUAL(status["schedule_refreshes"].getInt<uint64_t>(), expected);
        BOOST_CHECK_EQUAL(status["attempts"].getInt<uint64_t>(), 0U);
        clock += 5s;
    }
    worker->Stop();
}

BOOST_FIXTURE_TEST_CASE(worker_keeps_connection_history_separate_for_exact_signature_masks, TestChain100Setup)
{
    using namespace std::chrono_literals;
    FakeSteadyClock clock;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    wallet->m_default_max_tx_fee = MAX_MONEY;
    // At the untouched 5/s prior, both are just above the existing floor.
    // A failed 100 ms attempt drops only that mask below it.
    const auto target{uint256::FromHex(std::string(6, '0') + "3" + std::string(57, 'f')).value()};
    const std::array<uint8_t, 2> masks{PayToDomainOutput::SIGNATURE_ALGORITHMS_RSA,
                                      PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL};
    std::array<COutPoint, 2> outpoints;
    std::array<uint256, 2> challenges;
    CCoinControl control;
    control.m_feerate = CFeeRate{1000};
    UniValue saved{UniValue::VOBJ}, pending{UniValue::VARR};
    for (uint32_t i{0}; i < masks.size(); ++i) {
        outpoints[i] = COutPoint{Txid::FromUint256(uint256::ONE), i};
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoints[i],
            Coin{CTxOut{4 * COIN, PayToDomainOutput{"mixed.example", target, 1, masks[i]}}, 100, false}, false));
        const auto prepared{PrepareP2CClaim(*wallet, outpoints[i], control)};
        BOOST_REQUIRE(prepared);
        CMutableTransaction tx{*prepared->tx};
        tx.vout[0].nValue = 2 * COIN;
        challenges[i] = P2CClaimChallenge(CTransaction{tx}, 0);
        pending.push_back(EncodeHexTx(CTransaction{tx}));
    }
    saved.pushKV("pending", std::move(pending));
    saved.pushKV("ready", "");
    saved.pushKV("proof", "");
    saved.pushKV("attempts", 0);
    saved.pushKV("submitted", 0);
    saved.pushKV("last_txid", "");
    BOOST_REQUIRE(wallet->GetDatabase().MakeBatch()->Write(std::string{"p2c_claim_worker_v1"}, saved.write()));
    WITH_LOCK(wallet->cs_wallet, BOOST_REQUIRE(wallet->LockCoin(outpoints[1], false)));

    RestoreDNSLookup restore_dns;
    RestoreSocketFactory restore_sockets;
    const auto address{LookupHost("8.8.8.8", false, restore_dns.original)};
    BOOST_REQUIRE(address);
    g_dns_lookup = [&](const std::string&, bool) { return std::vector<CNetAddr>{*address}; };
    std::mutex events_mutex;
    std::vector<uint256> events;
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        std::this_thread::sleep_for(100ms);
        auto sent{std::make_shared<std::vector<unsigned char>>()};
        return std::make_unique<ClientHelloSocket>(*sent, 16384, [&, sent] {
            if (sent->size() < 43) return;
            uint256 challenge;
            std::copy_n(sent->begin() + 11, challenge.size(), challenge.begin());
            std::lock_guard lock{events_mutex};
            events.push_back(challenge);
        });
    };
    const auto wait_until = [](const auto& predicate) {
        const auto deadline{std::chrono::steady_clock::now() + 15s};
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    };
    auto worker{MakeP2CClaimWorker(*wallet)};
    const auto refresh = [&] {
        const auto previous{worker->Status()["schedule_refreshes"].getInt<uint64_t>()};
        clock += 5s;
        return wait_until([&] { return worker->Status()["schedule_refreshes"].getInt<uint64_t>() > previous; });
    };
    BOOST_REQUIRE(worker->Configure(-1, 1));
    BOOST_REQUIRE(wait_until([&] {
        std::lock_guard lock{events_mutex};
        return events.size() >= 2 && worker->Status()["state"].get_str() == "retrying connections";
    }));
    BOOST_REQUIRE(refresh());
    BOOST_REQUIRE(wait_until([&] { return worker->Status()["state"].get_str() == "waiting for eligible bounties"; }));
    {
        std::lock_guard lock{events_mutex};
        for (const auto& challenge : events) BOOST_CHECK(challenge == challenges[0]);
    }
    // The RSA-only failures must not poison the untried ALL mask at this same
    // domain. A shared per-domain history would leave both below the floor.
    WITH_LOCK(wallet->cs_wallet, BOOST_REQUIRE(wallet->UnlockCoin(outpoints[1])));
    BOOST_REQUIRE(refresh());
    BOOST_REQUIRE_MESSAGE(wait_until([&] {
        std::lock_guard lock{events_mutex};
        return std::find(events.begin(), events.end(), challenges[1]) != events.end();
    }), worker->Status().write());
    worker->Stop();
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
                PayToDomainOutput{names[i < 260 ? 0 : i - 259], uint256::FromHex("0000" + std::string(60, 'f')).value(), 1}}, 100, false}, false));
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
        // Keep the original 4:1 probability ratio, above the automatic floor.
        const auto target{uint256::FromHex(std::string(1, i == 2 ? '3' : '0') + std::string(63, 'f')).value()};
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
    const std::array<size_t, 3> order{2, 1, 0}; // Net EV ratio: 12 : 8 : 5.
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
    const auto sixteenth{uint256::FromHex("0" + std::string(63, 'f')).value()};
    const auto quarter{uint256::FromHex("3" + std::string(63, 'f')).value()};
    const auto maximum{uint256::FromHex(std::string(64, 'f')).value()};
    const std::array<uint256, 4> targets{sixteenth, sixteenth, quarter, maximum};
    std::vector<COutPoint> outpoints;
    UniValue saved{UniValue::VOBJ}, pending{UniValue::VARR};
    CCoinControl control;
    control.m_feerate = CFeeRate{1000};
    for (uint32_t i = 0; i < domains.size(); ++i) {
        outpoints.emplace_back(Txid::FromUint256(uint256::ONE), i);
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoints.back(),
            Coin{CTxOut{rewards[i], PayToDomainOutput{domains[i], targets[i], 1}}, 100, false}, false));
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
    const auto maximum{uint256::FromHex("0" + std::string(63, 'f')).value()};
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
    const CTxOut bounty{COIN, PayToDomainOutput{"example.com", uint256::FromHex(std::string(64, 'f')).value(), 1}};
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

BOOST_FIXTURE_TEST_CASE(worker_changes_unfinished_reward_destination, TestChain100Setup)
{
    using namespace std::chrono_literals;
    auto* active_chain{WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain())};
    auto wallet{CreateSyncedWallet(*m_node.chain, *active_chain, coinbaseKey)};
    wallet->SetBroadcastTransactions(true);
    const COutPoint outpoint{Txid::FromUint256(uint256::ONE), 0};
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoint,
        Coin{CTxOut{COIN, PayToDomainOutput{"example.com", uint256::FromHex(std::string(64, 'f')).value(), 1}}, 100, false}, false));
    RestoreDNSLookup restore_dns;
    std::atomic<unsigned> lookups{0};
    g_dns_lookup = [&](const std::string&, bool) { ++lookups; return std::vector<CNetAddr>{}; };
    const CTxDestination first{WitnessV1Taproot{XOnlyPubKey{GenerateRandomKey().GetPubKey()}}};
    const CTxDestination second{WitnessV1Taproot{XOnlyPubKey{GenerateRandomKey().GetPubKey()}}};
    auto worker{MakeP2CClaimWorker(*wallet)};
    CTransactionRef previous;
    for (const auto& address : {EncodeDestination(first), EncodeDestination(second), std::string{}}) {
        lookups = 0;
        BOOST_REQUIRE(worker->Configure(1, 1, {}, address));
        const auto deadline{std::chrono::steady_clock::now() + 10s};
        while (lookups == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
        worker->Stop();
        BOOST_REQUIRE(lookups > 0);
        std::string encoded;
        BOOST_REQUIRE(wallet->GetDatabase().MakeBatch()->Read(std::string{"p2c_claim_worker_v1"}, encoded));
        UniValue saved;
        BOOST_REQUIRE(saved.read(encoded));
        BOOST_REQUIRE_EQUAL(saved["pending"].size(), 1U);
        CMutableTransaction tx;
        BOOST_REQUIRE(DecodeHexTx(tx, saved["pending"][0].get_str()));
        BOOST_CHECK_EQUAL(worker->Status()["reward_address"].get_str(), address);
        if (address.empty()) {
            BOOST_CHECK(IsP2CClaimPayout(*wallet, tx.vout[0]));
        } else {
            BOOST_CHECK(IsP2CClaimPayout(*wallet, tx.vout[0], DecodeDestination(address)));
            BOOST_CHECK(!IsP2CClaimPayout(*wallet, tx.vout[0]));
        }
        if (previous) BOOST_CHECK(previous->GetHash() != CTransaction{tx}.GetHash());
        previous = MakeTransactionRef(tx);
    }
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
    const CTxDestination external{WitnessV1Taproot{XOnlyPubKey{GenerateRandomKey().GetPubKey()}}};
    for (uint32_t i = 0; i < 3; ++i) {
        const COutPoint outpoint{Txid::FromUint256(uint256::ONE), i};
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoint,
            Coin{CTxOut{COIN, PayToDomainOutput{"example.com", uint256{}, 1}}, 100, false}, false));
        const auto prepared{PrepareP2CClaim(*wallet, outpoint, control, MAX_P2C_PROOF_SIZE,
            i == 0 ? std::nullopt : std::optional<CTxDestination>{external})};
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
            item.pushKV("address", EncodeDestination(external));
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
        BOOST_CHECK_EQUAL(retained["completed"][0]["address"].get_str(), EncodeDestination(external));
        BOOST_CHECK(worker->Status()["last_error"].get_str().find("Invalid P2C proof:") != std::string::npos);
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
