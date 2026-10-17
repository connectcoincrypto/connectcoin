// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/p2c.h>
#include <consensus/p2c_x509.h>
#include <crypto/mbedtls_threading.h>
#include <netbase.h>
#include <primitives/transaction.h>
#include <test/util/setup_common.h>
#include <wallet/p2c_tls.h>
#include <wallet/test/p2c_tls_fixture.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace wallet {
namespace {
using Bytes = std::vector<unsigned char>;

struct RestoreFactory {
    const decltype(CreateSock) original{CreateSock};
    ~RestoreFactory() { CreateSock = original; }
};

struct CapturedFixture {
    RestoreFactory restore;
    std::shared_ptr<test::P2CTLSServer> server;
    Bytes bytes;
    P2CTlsProofView proof;
    CTxOut output{1000, PayToDomainOutput{"localhost", uint256{}, 1, 1}};
    std::string error;

    CapturedFixture()
    {
        BOOST_REQUIRE(InitializeP2CCrypto());
        server = std::make_shared<test::P2CTLSServer>();
        server->Setup();
        CreateSock = [config = server](int, int, int) -> std::unique_ptr<Sock> {
            return std::make_unique<test::P2CTLSSocket>(config, std::vector<std::string>{}, 127);
        };
        const auto endpoint{Lookup("8.8.8.8", 443, false)};
        BOOST_REQUIRE(endpoint);
        auto captured{CaptureP2CTls(*endpoint, "localhost", uint256::ONE, [] { return false; })};
        BOOST_REQUIRE(captured);
        bytes = std::move(*captured);
        BOOST_REQUIRE(ParseP2CTlsProof(bytes, "localhost", uint256::ONE, proof, error));
        ClearP2CX509CachesForTest();
    }
    static std::span<const unsigned char> Roots()
    {
        return {test::P2C_TEST_ROOTS_PEM, sizeof(test::P2C_TEST_ROOTS_PEM) - 1};
    }
    bool Verify(int64_t time = 1800000000)
    {
        return VerifyP2CCertificateProofForTest(output, proof, time, Roots(), error);
    }
};

void AppendU24(Bytes& output, size_t number)
{
    output.push_back(static_cast<unsigned char>(number >> 16));
    output.push_back(static_cast<unsigned char>(number >> 8));
    output.push_back(static_cast<unsigned char>(number));
}

Bytes CertificateMessage(std::span<const unsigned char> der, size_t copies = 1)
{
    Bytes list;
    for (size_t i = 0; i < copies; ++i) {
        AppendU24(list, der.size());
        list.insert(list.end(), der.begin(), der.end());
        list.insert(list.end(), {0, 0});
    }
    Bytes message{11};
    AppendU24(message, list.size() + 4);
    message.push_back(0);
    AppendU24(message, list.size());
    message.insert(message.end(), list.begin(), list.end());
    return message;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(p2c_x509_cache_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(reuses_der_and_static_signatures_but_not_authentication)
{
    CapturedFixture fixture;
    BOOST_REQUIRE(ValidateP2CCertificateMessage(fixture.proof.certificate, fixture.error));
    auto stats{P2CX509CacheStatsForTest()};
    BOOST_CHECK_EQUAL(stats.certificate_misses, 1U);
    BOOST_CHECK_EQUAL(stats.certificate_entries, 1U);
    BOOST_REQUIRE_MESSAGE(fixture.Verify(), fixture.error);
    const auto first{P2CX509CacheStatsForTest()};
    BOOST_CHECK_EQUAL(first.certificate_hits, 1U);
    BOOST_CHECK_GT(first.signature_misses, 0U);
    BOOST_REQUIRE(fixture.Verify());
    BOOST_CHECK_GT(P2CX509CacheStatsForTest().signature_hits, first.signature_hits);

    BOOST_CHECK(!fixture.Verify(1600000000));
    BOOST_CHECK_EQUAL(fixture.error, "P2C certificate is not valid at block median time");
    BOOST_CHECK(!fixture.Verify(2100000000));
    BOOST_CHECK(!fixture.Verify(0));
    BOOST_CHECK_EQUAL(fixture.error, "P2C certificate validation time is unavailable");

    const auto original{*fixture.output.GetPayToDomain()};
    auto changed{original};
    changed.domain = "other.example";
    fixture.output = CTxOut{1000, changed};
    BOOST_CHECK(!fixture.Verify());
    BOOST_CHECK_EQUAL(fixture.error, "P2C certificate path or domain validation failed");
    changed = original;
    changed.signature_algorithms_mask = 6;
    fixture.output = CTxOut{1000, changed};
    BOOST_CHECK(!fixture.Verify());
    BOOST_CHECK_EQUAL(fixture.error, "P2C CertificateVerify scheme is not allowed by the output");
    fixture.output = CTxOut{1000, original};

    // A previously verified edge cannot turn this test CA into a consensus CA.
    BOOST_CHECK(!VerifyP2CCertificateProof(fixture.output, fixture.proof, 1800000000, fixture.error));
    fixture.proof.transcript_hash.begin()[0] ^= 1;
    BOOST_CHECK(!fixture.Verify());
    BOOST_CHECK_EQUAL(fixture.error, "invalid P2C TLS CertificateVerify signature");
    fixture.proof.transcript_hash.begin()[0] ^= 1;
    BOOST_REQUIRE(fixture.Verify());
    fixture.bytes.back() ^= 1;
    BOOST_CHECK(!fixture.Verify());
    BOOST_CHECK_EQUAL(fixture.error, "invalid P2C TLS CertificateVerify signature");
}

BOOST_AUTO_TEST_CASE(owned_der_keys_resist_alias_mutation_and_invalid_signature_poisoning)
{
    CapturedFixture fixture;
    BOOST_REQUIRE(fixture.Verify());
    const auto good{P2CX509CacheStatsForTest()};
    Bytes changed{fixture.proof.certificate_chain.front().begin(), fixture.proof.certificate_chain.front().end()};
    // Alter the certificate's own signature while keeping its DER structure.
    changed.back() ^= 1;
    auto parsed{fixture.proof};
    parsed.certificate_chain.front() = changed;
    BOOST_CHECK(!VerifyP2CCertificateProofForTest(fixture.output, parsed, 1800000000, CapturedFixture::Roots(), fixture.error));
    BOOST_CHECK_EQUAL(fixture.error, "P2C certificate path or domain validation failed");
    BOOST_CHECK_GT(P2CX509CacheStatsForTest().signature_misses, good.signature_misses);
    const auto invalid{P2CX509CacheStatsForTest()};
    BOOST_CHECK(!VerifyP2CCertificateProofForTest(fixture.output, parsed, 1800000000, CapturedFixture::Roots(), fixture.error));
    BOOST_CHECK_GT(P2CX509CacheStatsForTest().signature_misses, invalid.signature_misses);
    BOOST_CHECK_EQUAL(P2CX509CacheStatsForTest().signature_entries, good.signature_entries);
    // The original cached DER is owned, not an alias of the changed input.
    changed.assign(changed.size(), 0);
    BOOST_REQUIRE(fixture.Verify());
    const auto before{P2CX509CacheStatsForTest()};
    BOOST_CHECK(!ValidateP2CCertificateMessage(CertificateMessage(changed), fixture.error));
    BOOST_CHECK_EQUAL(P2CX509CacheStatsForTest().certificate_entries, before.certificate_entries);
}

BOOST_AUTO_TEST_CASE(parsed_chain_lru_and_memory_budgets_are_bounded)
{
    CapturedFixture fixture;
    const auto original{fixture.proof.certificate_chain.front()};
    const auto& certificate{fixture.server->certificate};
    const size_t serial_offset{static_cast<size_t>(certificate.serial.p - certificate.raw.p)};
    BOOST_REQUIRE_LT(serial_offset, original.size());
    for (unsigned i = 0; i < 12; ++i) {
        Bytes der{original.begin(), original.end()};
        der[serial_offset] = static_cast<unsigned char>(10 + i);
        BOOST_REQUIRE(ValidateP2CCertificateMessage(CertificateMessage(der), fixture.error));
        const auto stats{P2CX509CacheStatsForTest()};
        BOOST_CHECK_LE(stats.certificate_entries, 4U);
        BOOST_CHECK_LE(stats.certificate_bytes, 65536U);
        BOOST_CHECK_LE(stats.certificate_estimated_bytes, 262144U);
    }
    BOOST_CHECK_EQUAL(P2CX509CacheStatsForTest().certificate_entries, 4U);
    ClearP2CX509CachesForTest();
    // Oversized retention cost does not change validation or retain the chain.
    BOOST_REQUIRE(ValidateP2CCertificateMessage(CertificateMessage(original, MAX_P2C_CERTIFICATES), fixture.error));
    BOOST_CHECK_EQUAL(P2CX509CacheStatsForTest().certificate_entries, 0U);
    BOOST_CHECK_EQUAL(P2CX509CacheStatsForTest().certificate_bytes, 0U);
    BOOST_CHECK(!ValidateP2CCertificateMessage(CertificateMessage(original, MAX_P2C_CERTIFICATES + 1), fixture.error));
    Bytes trailing{CertificateMessage(original)};
    trailing.push_back(0);
    BOOST_CHECK(!ValidateP2CCertificateMessage(trailing, fixture.error));
}

BOOST_AUTO_TEST_CASE(verification_caches_and_roots_are_worker_local)
{
    CapturedFixture fixture;
    std::atomic<bool> ok{true};
    std::vector<std::thread> workers;
    workers.reserve(4);
    for (unsigned thread = 0; thread < 4; ++thread) workers.emplace_back([&] {
        ClearP2CX509CachesForTest();
        std::string error;
        if (!P2CRootStoreAvailable()) ok = false;
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            if (!VerifyP2CCertificateProofForTest(fixture.output, fixture.proof, 1800000000, CapturedFixture::Roots(), error)) ok = false;
        }
        const auto stats{P2CX509CacheStatsForTest()};
        if (stats.certificate_misses != 1 || stats.certificate_hits != 7 || stats.signature_hits == 0) ok = false;
    });
    for (auto& worker : workers) worker.join();
    BOOST_CHECK(ok.load());
    BOOST_CHECK_EQUAL(P2CX509CacheStatsForTest().certificate_entries, 0U);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
