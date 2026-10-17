// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/p2c.h>
#include <consensus/p2c_x509.h>
#include <crypto/mbedtls_threading.h>
#include <netbase.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <wallet/p2c_tls.h>
#include <wallet/p2c_connection_result.h>
#include <wallet/p2c_tls_private.h>
#include <wallet/p2c_tls_test.h>
#include <wallet/test/p2c_tls_fixture.h>

#include <mbedtls/ecp.h>
#include <boost/test/unit_test.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace wallet {
namespace {
struct RestoreSockets {
    const decltype(CreateSock) previous{CreateSock};
    ~RestoreSockets() { CreateSock = previous; }
};

void SetSocketError(int error)
{
#ifdef WIN32
    WSASetLastError(error);
#else
    errno = error;
#endif
}

struct TLSOptimizationSetup : BasicTestingSetup {
    TLSOptimizationSetup() { BOOST_REQUIRE(InitializeP2CTlsCrypto()); }
};

auto CaptureFixture(const P2CTlsCaptureOptions& options = {})
{
    // Address policy remains production-strict; all socket I/O is mocked.
    const auto endpoint{Lookup("8.8.8.8", 443, false)};
    if (!endpoint) throw std::runtime_error("Cannot parse fixture endpoint");
    return CaptureP2CTls(*endpoint, "localhost", uint256::ONE, [] { return false; }, options);
}

class ConnectFailureSocket final : public ZeroSock {
    ConnectFailureSocket& operator=(Sock&&) override { throw std::logic_error("Cannot assign ConnectFailureSocket"); }
    const int m_error;
    const bool m_pending;
public:
    ConnectFailureSocket(int error, bool pending) : m_error{error}, m_pending{pending} {}
    int Connect(const sockaddr*, socklen_t) const override
    {
        SetSocketError(m_pending ? WSAEINPROGRESS : m_error);
        return SOCKET_ERROR;
    }
    int GetSockOpt(int, int, void* value, socklen_t* length) const override
    {
        if (*length < static_cast<socklen_t>(sizeof(m_error))) return SOCKET_ERROR;
        std::memcpy(value, &m_error, sizeof(m_error));
        *length = sizeof(m_error);
        return 0;
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(p2c_tls_optimization_tests, TLSOptimizationSetup)

BOOST_AUTO_TEST_CASE(reuse_cleans_secrets_after_abbreviated_and_complete_handshakes)
{
    RestoreSockets restore;
    const auto config{std::make_shared<test::P2CTLSServer>()};
    config->Setup();
    CreateSock = [config](int, int, int) -> std::unique_ptr<Sock> {
        return std::make_unique<test::P2CTLSSocket>(config, std::vector<std::string>{}, 17);
    };
    for (int iteration = 0; iteration < 6; ++iteration) {
        const auto before{P2CTlsReuseSnapshotForTest()};
        BOOST_CHECK(before.clean);
        const auto proof{CaptureFixture({.signature_algorithms_mask = static_cast<uint8_t>(iteration % 2 ? 1 : 7),
            .complete_handshake = iteration % 3 == 0})};
        BOOST_REQUIRE_MESSAGE(proof, util::ErrorString(proof).original);
        const auto after{P2CTlsReuseSnapshotForTest()};
        BOOST_CHECK(after.clean);
        BOOST_CHECK_EQUAL(after.resets, before.resets + 1);
        BOOST_CHECK_EQUAL(after.discarded, before.discarded);
        BOOST_CHECK_NE(after.input_buffer, 0U);
        BOOST_CHECK_NE(after.output_buffer, 0U);
        if (before.input_buffer) {
            BOOST_CHECK_EQUAL(after.setups, before.setups);
            BOOST_CHECK_EQUAL(after.input_buffer, before.input_buffer);
            BOOST_CHECK_EQUAL(after.output_buffer, before.output_buffer);
        }
        P2CTlsProofView view;
        std::string error;
        BOOST_REQUIRE(ParseP2CTlsProof(*proof, "localhost", uint256::ONE, view, error));
        const CTxOut output{1, PayToDomainOutput{"localhost", view.connection_work_hash, 1}};
        BOOST_CHECK_MESSAGE(VerifyP2CCertificateProofForTest(output, view, 1800000000,
            {test::P2C_TEST_ROOTS_PEM, sizeof(test::P2C_TEST_ROOTS_PEM) - 1}, error), error);
        BOOST_CHECK(!VerifyP2CCertificateProof(output, view, 1800000000, error));
    }
}

BOOST_AUTO_TEST_CASE(reset_failure_discards_context_before_reuse)
{
    RestoreSockets restore;
    const auto config{std::make_shared<test::P2CTLSServer>()};
    config->Setup();
    CreateSock = [config](int, int, int) -> std::unique_ptr<Sock> {
        return std::make_unique<test::P2CTLSSocket>(config, std::vector<std::string>{}, 53);
    };
    const auto before{P2CTlsReuseSnapshotForTest()};
    FailNextP2CTlsResetForTest();
    BOOST_REQUIRE(CaptureFixture());
    const auto dropped{P2CTlsReuseSnapshotForTest()};
    BOOST_CHECK(dropped.clean);
    BOOST_CHECK_EQUAL(dropped.discarded, before.discarded + 1);
    BOOST_CHECK_EQUAL(dropped.input_buffer, 0U);
    BOOST_CHECK_EQUAL(dropped.output_buffer, 0U);
    BOOST_REQUIRE(CaptureFixture());
    const auto repaired{P2CTlsReuseSnapshotForTest()};
    BOOST_CHECK(repaired.clean);
    BOOST_CHECK_EQUAL(repaired.setups, dropped.setups + 1);
}

BOOST_AUTO_TEST_CASE(owned_capture_keeps_spans_and_original_context_alive)
{
    RestoreSockets restore;
    const auto config{std::make_shared<test::P2CTLSServer>()};
    config->Setup();
    CreateSock = [config](int, int, int) -> std::unique_ptr<Sock> {
        return std::make_unique<test::P2CTLSSocket>(config, std::vector<std::string>{}, 31);
    };
    const auto endpoint{Lookup("8.8.8.8", 443, false)};
    BOOST_REQUIRE(endpoint);
    auto owned{[&] {
        auto captured{CaptureP2CTlsParsed(*endpoint, "localhost", uint256::ONE, [] { return false; })};
        BOOST_REQUIRE(captured);
        auto copy{*captured};
        auto moved{std::move(copy)};
        // These accessors explicitly reject moved-from owners.
        // NOLINTBEGIN(bugprone-use-after-move)
        BOOST_CHECK_THROW(copy.Bytes(), std::logic_error);
        BOOST_CHECK_THROW(copy.View(), std::logic_error);
        BOOST_CHECK(!copy.MatchesCaptureContext("localhost", uint256::ONE));
        // NOLINTEND(bugprone-use-after-move)
        return moved;
    }()};
    BOOST_CHECK(owned.MatchesCaptureContext("localhost", uint256::ONE));
    BOOST_CHECK(!owned.MatchesCaptureContext("other.example", uint256::ONE));
    BOOST_CHECK(!owned.MatchesCaptureContext("localhost", uint256{}));
    const auto bytes{std::vector<unsigned char>{owned.Bytes()}};
    const auto& view{owned.View()};
    for (const auto message : {view.client_hello, view.server_hello, view.encrypted_extensions,
             view.certificate, view.certificate_verify_signature}) {
        BOOST_CHECK(message.data() >= owned.Bytes().data());
        BOOST_CHECK(message.data() + message.size() <= owned.Bytes().data() + owned.Bytes().size());
    }
    const CTxOut output{1, PayToDomainOutput{"localhost", view.connection_work_hash, 1}};
    std::string error;
    BOOST_CHECK_MESSAGE(VerifyP2CCertificateProofForTest(output, view, 1800000000,
        {test::P2C_TEST_ROOTS_PEM, sizeof(test::P2C_TEST_ROOTS_PEM) - 1}, error), error);
    bool cross_thread{false};
    std::thread consumer{[captured = owned, output, &cross_thread] {
        std::string thread_error;
        cross_thread = VerifyP2CCertificateProofForTest(output, captured.View(), 1800000000,
            {test::P2C_TEST_ROOTS_PEM, sizeof(test::P2C_TEST_ROOTS_PEM) - 1}, thread_error);
    }};
    consumer.join();
    BOOST_CHECK(cross_thread);
    BOOST_CHECK(owned.Bytes() == bytes);

    // Only parsing is reused: every current domain/challenge, mask, chain
    // time and verifier policy must still apply to the same captured bytes.
    const util::Result<P2CCapturedProof> result{owned};
    bool policy_allows{true};
    const auto observe = [&](const CTxOut& spent, const uint256& challenge, int64_t time,
                             bool cancelled, bool expected, bool should_verify) {
        unsigned verifies{0}, observations{0};
        P2CTlsProofView observed;
        std::string observed_error;
        const bool valid{ObserveP2CTlsCapture(result, spent, challenge, time, cancelled, observed, observed_error,
            [&](const CTxOut& current, const P2CTlsProofView& parsed, int64_t current_time, std::string& failure) {
                ++verifies;
                return policy_allows && VerifyP2CCertificateProofForTest(current, parsed, current_time,
                    {test::P2C_TEST_ROOTS_PEM, sizeof(test::P2C_TEST_ROOTS_PEM) - 1}, failure);
            }, [&](bool accepted) { ++observations; BOOST_CHECK_EQUAL(accepted, expected); })};
        BOOST_CHECK_EQUAL(valid, expected);
        BOOST_CHECK_EQUAL(observations, 1U);
        BOOST_CHECK_EQUAL(verifies, should_verify ? 1U : 0U);
    };
    observe(output, uint256::ONE, 1800000000, false, true, true);
    observe(output, uint256::ONE, 1800000000, true, true, true);
    observe(output, uint256{}, 1800000000, false, false, false);
    observe(CTxOut{1, PayToDomainOutput{"other.example", view.connection_work_hash, 1}},
        uint256::ONE, 1800000000, false, false, false);
    observe(CTxOut{1, PayToDomainOutput{"localhost", view.connection_work_hash, 1, 6}},
        uint256::ONE, 1800000000, false, false, true);
    observe(output, uint256::ONE, 2100000000, false, false, true);
    policy_allows = false;
    observe(output, uint256::ONE, 1800000000, false, false, true);
}

BOOST_AUTO_TEST_CASE(capture_defers_cv_authentication_but_probe_and_verifier_do_not)
{
    RestoreSockets restore;
    const auto config{std::make_shared<test::P2CTLSServer>()};
    config->Setup();
    // Sign the handshake with a DIFFERENT EC key from the certificate. Record
    // encryption still works, but CertificateVerify must fail authentication.
    mbedtls_pk_free(&config->key);
    BOOST_REQUIRE_EQUAL(mbedtls_pk_setup(&config->key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)), 0);
    BOOST_REQUIRE_EQUAL(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(config->key),
        test::P2CTLSServer::Random, nullptr), 0);
    CreateSock = [config](int, int, int) -> std::unique_ptr<Sock> {
        return std::make_unique<test::P2CTLSSocket>(config, std::vector<std::string>{}, 127);
    };
    const auto captured{CaptureFixture()};
    BOOST_REQUIRE_MESSAGE(captured, util::ErrorString(captured).original);
    P2CTlsProofView view;
    std::string error;
    BOOST_REQUIRE(ParseP2CTlsProof(*captured, "localhost", uint256::ONE, view, error));
    const CTxOut output{1, PayToDomainOutput{"localhost", view.connection_work_hash, 1}};
    BOOST_CHECK(!VerifyP2CCertificateProofForTest(output, view, 1800000000,
        {test::P2C_TEST_ROOTS_PEM, sizeof(test::P2C_TEST_ROOTS_PEM) - 1}, error));
    BOOST_CHECK(!CaptureFixture({.complete_handshake = true}));
    BOOST_CHECK(P2CTlsReuseSnapshotForTest().clean);
}

BOOST_AUTO_TEST_CASE(certificate_envelope_matches_pinned_upstream_admission)
{
    RestoreSockets restore;
    const auto config{std::make_shared<test::P2CTLSServer>()};
    config->Setup();
    CreateSock = [config](int, int, int) -> std::unique_ptr<Sock> {
        return std::make_unique<test::P2CTLSSocket>(config, std::vector<std::string>{}, 47);
    };
    const auto proof{CaptureFixture()};
    BOOST_REQUIRE(proof);
    P2CTlsProofView view;
    std::string error;
    BOOST_REQUIRE(ParseP2CTlsProof(*proof, "localhost", uint256::ONE, view, error));
    const std::vector<unsigned char> original(view.certificate.begin(), view.certificate.end());
    std::vector<std::vector<unsigned char>> fixtures{original};
    // Every suffix truncation of a valid record, plus mismatched request
    // context, trailing data, empty chain and an oversized certificate vector.
    for (size_t size = 4; size < original.size(); size += 17) {
        fixtures.emplace_back(original.begin(), original.begin() + size);
    }
    auto malformed{original}; malformed[4] = 1; fixtures.push_back(malformed);
    malformed = original; malformed.push_back(0); fixtures.push_back(malformed);
    malformed = original; malformed[5] = 1; fixtures.push_back(malformed);
    fixtures.push_back({11, 0, 0, 4, 0, 0, 0, 0});
    // Duplicate/unsolicited/unknown certificate-entry extensions must retain
    // the upstream result; the abbreviated path must never parse a peer key.
    for (const std::vector<unsigned char>& extension : {
        std::vector<unsigned char>{0, 5, 0, 0},
        std::vector<unsigned char>{0, 18, 0, 0},
        std::vector<unsigned char>{0xff, 0xfd, 0, 0},
        std::vector<unsigned char>{0, 5, 0, 0, 0, 5, 0, 0}}) {
        malformed = original;
        BOOST_REQUIRE_EQUAL(malformed.back(), 0U);
        BOOST_REQUIRE_EQUAL(malformed[malformed.size() - 2], 0U);
        malformed.back() = static_cast<unsigned char>(extension.size());
        malformed.insert(malformed.end(), extension.begin(), extension.end());
        const size_t body{malformed.size() - 4}, list{malformed.size() - 8};
        malformed[1] = static_cast<unsigned char>(body >> 16);
        malformed[2] = static_cast<unsigned char>(body >> 8);
        malformed[3] = static_cast<unsigned char>(body);
        malformed[5] = static_cast<unsigned char>(list >> 16);
        malformed[6] = static_cast<unsigned char>(list >> 8);
        malformed[7] = static_cast<unsigned char>(list);
        fixtures.push_back(malformed);
    }
    for (const auto& message : fixtures) {
        uint32_t original_extensions{0}, capture_extensions{0};
        int original_peer{0}, capture_peer{0};
        const int upstream{connectcoin_p2c_test_certificate(message.data(), message.size(), 0, 0,
            &original_extensions, &original_peer)};
        const int capture{connectcoin_p2c_test_certificate(message.data(), message.size(), 1, 0,
            &capture_extensions, &capture_peer)};
        BOOST_CHECK_EQUAL(capture, upstream);
        BOOST_CHECK_EQUAL(capture_extensions, original_extensions);
        BOOST_CHECK_EQUAL(capture_peer, 0);
        if (capture == 0) {
            BOOST_CHECK_MESSAGE(ValidateP2CCertificateMessage(message, error), error);
            BOOST_CHECK_EQUAL(original_peer, 1);
        }
    }
}

BOOST_AUTO_TEST_CASE(local_endpoint_failures_have_exact_scope)
{
    RestoreSockets restore;
#ifdef WIN32
    const std::array family{WSAEAFNOSUPPORT, WSAEPFNOSUPPORT, WSAEPROTONOSUPPORT, WSAESOCKTNOSUPPORT};
    const std::array route{WSAENETUNREACH, WSAEHOSTUNREACH, WSAENETDOWN};
    const std::array ordinary{WSAECONNREFUSED, WSAECONNRESET, WSAETIMEDOUT, WSAEMFILE,
        WSAENOBUFS, WSAEACCES, WSAEADDRINUSE, WSAEADDRNOTAVAIL};
#else
    const std::array family{EAFNOSUPPORT, EPROTONOSUPPORT};
    const std::array route{ENETUNREACH, EHOSTUNREACH, ENETDOWN};
    const std::array ordinary{ECONNREFUSED, ECONNRESET, ETIMEDOUT, EMFILE,
        ENOBUFS, EACCES, EADDRINUSE, EADDRNOTAVAIL};
#endif
    const auto check = [&](int code, bool creating, bool pending, int expected) {
        int notified{-1};
        CreateSock = [=](int, int, int) -> std::unique_ptr<Sock> {
            if (creating) { SetSocketError(code); return {}; }
            return std::make_unique<ConnectFailureSocket>(code, pending);
        };
        BOOST_CHECK(!CaptureFixture({.on_local_unavailable = [&](bool is_family) {
            BOOST_CHECK_EQUAL(notified, -1); notified = is_family ? 1 : 0;
        }}));
        BOOST_CHECK_EQUAL(notified, expected);
    };
    for (const int error : family) {
        check(error, true, false, 1);
        check(error, false, false, -1);
    }
    for (const int error : route) {
        check(error, true, false, -1);
        check(error, false, false, 0);
        check(error, false, true, 0);
    }
    for (const int error : ordinary) {
        check(error, true, false, -1);
        check(error, false, false, -1);
        check(error, false, true, -1);
    }
}

BOOST_AUTO_TEST_CASE(tcp_readiness_wait_obeys_cancellation_and_remaining_deadline)
{
    RestoreSockets restore;
    bool stop{false};
    unsigned waits{0};
    class PendingSocket final : public ZeroSock {
        PendingSocket& operator=(Sock&&) override { throw std::logic_error("Cannot assign PendingSocket"); }
        bool& m_stop;
        unsigned& m_waits;
    public:
        PendingSocket(bool& stop, unsigned& waits) : m_stop{stop}, m_waits{waits} {}
        int Connect(const sockaddr*, socklen_t) const override { SetSocketError(WSAEINPROGRESS); return SOCKET_ERROR; }
        bool Wait(std::chrono::milliseconds timeout, Event, Event* occurred) const override
        {
            BOOST_CHECK_LE(timeout.count(), 25);
            BOOST_CHECK_GE(timeout.count(), 1);
            ++m_waits;
            m_stop = true;
            if (occurred) *occurred = 0;
            return true;
        }
    };
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> { return std::make_unique<PendingSocket>(stop, waits); };
    const auto endpoint{Lookup("8.8.8.8", 443, false)};
    BOOST_REQUIRE(endpoint);
    const auto captured{CaptureP2CTls(*endpoint, "localhost", uint256::ONE, [&] { return stop; },
        {.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{25},
         .on_local_unavailable = [](bool) { BOOST_FAIL("Cancellation marked a local endpoint unavailable"); }})};
    BOOST_CHECK(!captured);
    BOOST_CHECK_EQUAL(waits, 1U);
}

BOOST_AUTO_TEST_CASE(tls_bio_cancellation_interrupts_internal_record_loop)
{
    RestoreSockets restore;
    bool stop{false};
    size_t received{0};
    class CompatibilityCcsSocket final : public ZeroSock {
        CompatibilityCcsSocket& operator=(Sock&&) override { throw std::logic_error("Cannot assign CompatibilityCcsSocket"); }
        bool& m_stop;
        size_t& m_received;
    public:
        CompatibilityCcsSocket(bool& stop, size_t& received) : m_stop{stop}, m_received{received} {}
        ssize_t Recv(void* buffer, size_t length, int) const override
        {
            // Mbed TLS ignores these internally without returning control to
            // Capture's outer loop. EOF bounds the old behavior deterministically.
            static constexpr std::array<unsigned char, 6> ccs{0x14, 0x03, 0x03, 0x00, 0x01, 0x01};
            if (m_received == ccs.size() * 4096) return 0;
            const size_t offset{m_received % ccs.size()};
            const size_t count{std::min(length, ccs.size() - offset)};
            std::memcpy(buffer, ccs.data() + offset, count);
            m_received += count;
            if (m_received == ccs.size() * 32) m_stop = true;
            return static_cast<ssize_t>(count);
        }
    };
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        return std::make_unique<CompatibilityCcsSocket>(stop, received);
    };
    const auto endpoint{Lookup("8.8.8.8", 443, false)};
    BOOST_REQUIRE(endpoint);
    BOOST_CHECK(!CaptureP2CTls(*endpoint, "localhost", uint256::ONE, [&] { return stop; }));
    BOOST_CHECK(stop);
    BOOST_CHECK_EQUAL(received, 6U * 32U);
    BOOST_CHECK(P2CTlsReuseSnapshotForTest().clean);
}

BOOST_AUTO_TEST_CASE(tls_bio_deadline_interrupts_internal_record_loop)
{
    RestoreSockets restore;
    size_t received{0};
    bool waited{false};
    auto deadline{std::chrono::steady_clock::time_point::max()};
    class DeadlineCcsSocket final : public ZeroSock {
        DeadlineCcsSocket& operator=(Sock&&) override { throw std::logic_error("Cannot assign DeadlineCcsSocket"); }
        size_t& m_received;
        bool& m_waited;
        const std::chrono::steady_clock::time_point& m_deadline;
    public:
        DeadlineCcsSocket(size_t& received, bool& waited, const std::chrono::steady_clock::time_point& deadline)
            : m_received{received}, m_waited{waited}, m_deadline{deadline} {}
        ssize_t Recv(void* buffer, size_t length, int) const override
        {
            static constexpr std::array<unsigned char, 6> ccs{0x14, 0x03, 0x03, 0x00, 0x01, 0x01};
            if (m_received == ccs.size() * 4096) return 0;
            if (m_received == ccs.size() * 32 && !m_waited) {
                // A single bounded pause crosses the known absolute cap while
                // already inside the provider's ignored-record loop.
                std::this_thread::sleep_until(m_deadline + std::chrono::milliseconds{1});
                m_waited = true;
            }
            const size_t offset{m_received % ccs.size()};
            const size_t count{std::min(length, ccs.size() - offset)};
            std::memcpy(buffer, ccs.data() + offset, count);
            m_received += count;
            return static_cast<ssize_t>(count);
        }
    };
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        return std::make_unique<DeadlineCcsSocket>(received, waited, deadline);
    };
    P2CTlsCaptureOptions options;
    options.on_tcp_started = [&] {
        // Set after socket/preparation so those costs cannot consume the test
        // window. No assertion depends on an elapsed wall-clock measurement.
        options.deadline = deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
        return true;
    };
    BOOST_CHECK(!CaptureFixture(options));
    BOOST_CHECK(waited);
    // The syscall that crosses the deadline may return its final header;
    // it must not cause the rest of the ready stream to be consumed.
    BOOST_CHECK_LE(received, 6U * 32U + 5U);
    BOOST_CHECK(P2CTlsReuseSnapshotForTest().clean);
}

BOOST_AUTO_TEST_CASE(tls_bio_cancellation_stops_partial_client_hello_writes)
{
    RestoreSockets restore;
    bool stop{false};
    size_t sends{0};
    class PartialWriteSocket final : public ZeroSock {
        PartialWriteSocket& operator=(Sock&&) override { throw std::logic_error("Cannot assign PartialWriteSocket"); }
        bool& m_stop;
        size_t& m_sends;
    public:
        PartialWriteSocket(bool& stop, size_t& sends) : m_stop{stop}, m_sends{sends} {}
        ssize_t Send(const void*, size_t length, int) const override
        {
            if (length == 0) return 0;
            ++m_sends;
            m_stop = true;
            return 1;
        }
    };
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        return std::make_unique<PartialWriteSocket>(stop, sends);
    };
    const auto endpoint{Lookup("8.8.8.8", 443, false)};
    BOOST_REQUIRE(endpoint);
    BOOST_CHECK(!CaptureP2CTls(*endpoint, "localhost", uint256::ONE, [&] { return stop; }));
    BOOST_CHECK(stop);
    BOOST_CHECK_EQUAL(sends, 1U);
    BOOST_CHECK(P2CTlsReuseSnapshotForTest().clean);
}

BOOST_AUTO_TEST_CASE(tls_bio_does_not_unwind_cancellation_callback_through_provider)
{
    RestoreSockets restore;
    bool throw_once{false};
    size_t callback_errors{0};
    class ThrowingCancellationSocket final : public ZeroSock {
        ThrowingCancellationSocket& operator=(Sock&&) override { throw std::logic_error("Cannot assign ThrowingCancellationSocket"); }
        bool& m_throw_once;
    public:
        explicit ThrowingCancellationSocket(bool& throw_once) : m_throw_once{throw_once} {}
        ssize_t Send(const void*, size_t length, int) const override
        {
            m_throw_once = true;
            return static_cast<ssize_t>(length);
        }
    };
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        return std::make_unique<ThrowingCancellationSocket>(throw_once);
    };
    const auto endpoint{Lookup("8.8.8.8", 443, false)};
    BOOST_REQUIRE(endpoint);
    const auto cancellation = [&] {
        if (std::exchange(throw_once, false)) {
            ++callback_errors;
            throw std::runtime_error("Cancellation callback fixture");
        }
        return false;
    };
    BOOST_CHECK_NO_THROW(BOOST_CHECK(!CaptureP2CTls(*endpoint, "localhost", uint256::ONE, cancellation)));
    BOOST_CHECK_EQUAL(callback_errors, 1U);
    BOOST_CHECK(P2CTlsReuseSnapshotForTest().clean);
}

BOOST_AUTO_TEST_CASE(tcp_admission_runs_once_and_can_abort_before_connect)
{
    RestoreSockets restore;
    unsigned creates{0}, starts{0}, callbacks{0};
    bool admit{false};
    class AdmissionSocket final : public ZeroSock {
        AdmissionSocket& operator=(Sock&&) override { throw std::logic_error("Cannot assign AdmissionSocket"); }
        unsigned& m_starts;
    public:
        explicit AdmissionSocket(unsigned& starts) : m_starts{starts} {}
        int Connect(const sockaddr*, socklen_t) const override
        {
            ++m_starts;
            SetSocketError(WSAEINVAL); // Pending; the mock then reports ready.
            return SOCKET_ERROR;
        }
        ssize_t Recv(void*, size_t, int) const override { return 0; }
    };
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        ++creates;
        return std::make_unique<AdmissionSocket>(starts);
    };
    const P2CTlsCaptureOptions options{.before_tcp_start = [&] { ++callbacks; return admit; }};
    BOOST_CHECK(!CaptureFixture(options));
    BOOST_CHECK_EQUAL(creates, 1U);
    BOOST_CHECK_EQUAL(callbacks, 1U);
    BOOST_CHECK_EQUAL(starts, 0U);
    admit = true;
    BOOST_CHECK(!CaptureFixture(options)); // EOF, no server.
    BOOST_CHECK_EQUAL(creates, 2U);
    BOOST_CHECK_EQUAL(callbacks, 2U);
    BOOST_CHECK_EQUAL(starts, 1U);
    BOOST_CHECK(P2CTlsReuseSnapshotForTest().clean);
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> { ++creates; return {}; };
    BOOST_CHECK(!CaptureFixture(options));
    BOOST_CHECK_EQUAL(callbacks, 2U); // Creating a socket is not a TCP attempt.
}

BOOST_AUTO_TEST_CASE(tcp_admission_rechecks_cancel_and_absolute_deadline)
{
    RestoreSockets restore;
    unsigned starts{0}, accounted{0};
    bool stop{false};
    class CountSocket final : public ZeroSock {
        CountSocket& operator=(Sock&&) override { throw std::logic_error("Cannot assign CountSocket"); }
        unsigned& m_starts;
    public:
        explicit CountSocket(unsigned& starts) : m_starts{starts} {}
        int Connect(const sockaddr*, socklen_t) const override { ++m_starts; return 0; }
    };
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> { return std::make_unique<CountSocket>(starts); };
    const auto endpoint{Lookup("8.8.8.8", 443, false)};
    BOOST_REQUIRE(endpoint);
    P2CTlsCaptureOptions options;
    options.before_tcp_start = [&] { stop = true; return true; };
    options.on_tcp_started = [&] { ++accounted; return true; };
    BOOST_CHECK(!CaptureP2CTls(*endpoint, "localhost", uint256::ONE, [&] { return stop; }, options));
    BOOST_CHECK_EQUAL(starts, 0U);
    BOOST_CHECK_EQUAL(accounted, 0U);
    stop = false;
    options.before_tcp_start = [&] {
        options.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds{1};
        return true;
    };
    BOOST_CHECK(!CaptureP2CTls(*endpoint, "localhost", uint256::ONE, [&] { return stop; }, options));
    BOOST_CHECK_EQUAL(starts, 0U);
    BOOST_CHECK_EQUAL(accounted, 0U);
    options.deadline = std::chrono::steady_clock::time_point::max();
    options.before_tcp_start = [] { return true; };
    options.on_tcp_started = [] { return false; }; // A newly stale candidate.
    BOOST_CHECK(!CaptureP2CTls(*endpoint, "localhost", uint256::ONE, [&] { return stop; }, options));
    BOOST_CHECK_EQUAL(starts, 0U);
}

BOOST_AUTO_TEST_CASE(concurrent_workers_have_independent_reusable_tls_contexts)
{
    RestoreSockets restore;
    CreateSock = [](int, int, int) -> std::unique_ptr<Sock> {
        const auto config{std::make_shared<test::P2CTLSServer>()};
        config->Setup();
        return std::make_unique<test::P2CTLSSocket>(config, std::vector<std::string>{}, 71);
    };
    std::array<std::thread, 4> workers;
    std::array<uintptr_t, 4> buffers{};
    std::atomic<bool> good{true};
    std::barrier together{4};
    for (size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::thread{[&, index] {
            try {
                for (int iteration = 0; iteration < 3; ++iteration) {
                    if (!CaptureFixture({.complete_handshake = iteration == 1})) good = false;
                    const auto state{P2CTlsReuseSnapshotForTest()};
                    if (!state.clean || state.setups != 1 || state.resets != static_cast<size_t>(iteration + 1)) good = false;
                    buffers[index] = state.input_buffer;
                }
            } catch (...) { good = false; }
            together.arrive_and_wait(); // Keep all worker allocations alive.
        }};
    }
    for (auto& worker : workers) worker.join();
    BOOST_CHECK(good);
    for (size_t i = 0; i < buffers.size(); ++i) {
        BOOST_CHECK_NE(buffers[i], 0U);
        for (size_t j = i + 1; j < buffers.size(); ++j) BOOST_CHECK_NE(buffers[i], buffers[j]);
    }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
