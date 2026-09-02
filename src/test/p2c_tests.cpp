// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/p2c.h>
#include <consensus/p2c_x509.h>
#include <crypto/sha256.h>
#include <key.h>
#include <primitives/transaction.h>
#include <rpc/rawtransaction_util.h>
#include <script/sigcache.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <univalue.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/test/unit_test.hpp>

namespace {

using Bytes = std::vector<unsigned char>;

void U16(Bytes& out, uint16_t value)
{
    out.push_back(static_cast<unsigned char>(value >> 8));
    out.push_back(static_cast<unsigned char>(value));
}

void U24(Bytes& out, uint32_t value)
{
    out.push_back(static_cast<unsigned char>(value >> 16));
    out.push_back(static_cast<unsigned char>(value >> 8));
    out.push_back(static_cast<unsigned char>(value));
}

void Extension(Bytes& out, uint16_t type, const Bytes& data)
{
    U16(out, type);
    U16(out, data.size());
    out.insert(out.end(), data.begin(), data.end());
}

Bytes Handshake(uint8_t type, const Bytes& body)
{
    Bytes result{type};
    U24(result, body.size());
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

void Append(Bytes& out, const Bytes& bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

Bytes StructuralProof(std::string_view domain, const uint256& challenge, bool invalid_key_share = false,
                      std::span<const unsigned char> certificate_der = {},
                      std::span<const unsigned char> certificate_signature = {})
{
    constexpr uint16_t SERVER_NAME{0};
    constexpr uint16_t SIGNATURE_ALGORITHMS{13};
    constexpr uint16_t SUPPORTED_VERSIONS{43};
    constexpr uint16_t KEY_SHARE{51};

    Bytes client;
    U16(client, 0x0303);
    client.insert(client.end(), challenge.begin(), challenge.end());
    client.push_back(0); // legacy_session_id
    U16(client, 2);
    U16(client, 0x1301);
    client.insert(client.end(), {1, 0}); // null compression

    Bytes client_extensions;
    Bytes sni;
    U16(sni, 1 + 2 + domain.size());
    sni.push_back(0);
    U16(sni, domain.size());
    sni.insert(sni.end(), domain.begin(), domain.end());
    Extension(client_extensions, SERVER_NAME, sni);
    Extension(client_extensions, SUPPORTED_VERSIONS, {2, 0x03, 0x04});
    Extension(client_extensions, SIGNATURE_ALGORITHMS, {0, 2, 0x04, 0x03});
    Bytes client_share;
    const uint16_t client_share_size{static_cast<uint16_t>(invalid_key_share ? 31 : 32)};
    U16(client_share, 2 + 2 + client_share_size);
    U16(client_share, 0x001d);
    U16(client_share, client_share_size);
    client_share.insert(client_share.end(), client_share_size, 1);
    Extension(client_extensions, KEY_SHARE, client_share);
    U16(client, client_extensions.size());
    Append(client, client_extensions);

    Bytes server;
    U16(server, 0x0303);
    server.insert(server.end(), 32, 3);
    server.push_back(0);
    U16(server, 0x1301);
    server.push_back(0);
    Bytes server_extensions;
    Extension(server_extensions, SUPPORTED_VERSIONS, {0x03, 0x04});
    Bytes server_share;
    U16(server_share, 0x001d);
    U16(server_share, 32);
    server_share.insert(server_share.end(), 32, 2);
    Extension(server_extensions, KEY_SHARE, server_share);
    U16(server, server_extensions.size());
    Append(server, server_extensions);

    Bytes encrypted_extensions{0, 0};
    Bytes certificate;
    certificate.push_back(0); // request context
    Bytes certificate_list;
    const Bytes dummy_certificate{0x30};
    if (certificate_der.empty()) certificate_der = dummy_certificate;
    U24(certificate_list, certificate_der.size());
    certificate_list.insert(certificate_list.end(), certificate_der.begin(), certificate_der.end());
    U16(certificate_list, 0);
    U24(certificate, certificate_list.size());
    Append(certificate, certificate_list);
    Bytes certificate_verify;
    U16(certificate_verify, 0x0403);
    const Bytes dummy_signature{0x30};
    if (certificate_signature.empty()) certificate_signature = dummy_signature;
    U16(certificate_verify, certificate_signature.size());
    certificate_verify.insert(certificate_verify.end(), certificate_signature.begin(), certificate_signature.end());

    Bytes proof{P2C_PROOF_VERSION};
    Append(proof, Handshake(1, client));
    Append(proof, Handshake(2, server));
    Append(proof, Handshake(8, encrypted_extensions));
    Append(proof, Handshake(11, certificate));
    Append(proof, Handshake(15, certificate_verify));
    return proof;
}

CTransaction ChallengeTransaction()
{
    CKey key;
    key.MakeNewKey(/*fCompressed=*/true);
    CMutableTransaction tx;
    tx.vin.emplace_back(Txid::FromUint256(uint256::ONE), 0);
    tx.vin.emplace_back(Txid::FromUint256(uint256::ONE), 1);
    tx.vout.emplace_back(1, XOnlyPubKey{key.GetPubKey()});
    return CTransaction{tx};
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(p2c_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(challenge_commits_txid_and_input_index_but_not_witness)
{
    const CTransaction tx{ChallengeTransaction()};
    BOOST_CHECK(P2CClaimChallenge(tx, 0) != P2CClaimChallenge(tx, 1));

    CMutableTransaction with_witness{tx};
    with_witness.vin[0].scriptWitness.stack.push_back({1, 2, 3});
    const CTransaction witnessed{with_witness};
    BOOST_CHECK(tx.GetHash() == witnessed.GetHash());
    BOOST_CHECK(P2CClaimChallenge(tx, 0) == P2CClaimChallenge(witnessed, 0));

    with_witness.vin[0].prevout.n++;
    BOOST_CHECK(P2CClaimChallenge(tx, 0) != P2CClaimChallenge(CTransaction{with_witness}, 0));
}

BOOST_AUTO_TEST_CASE(canonical_tls_profile_and_connection_work)
{
    const CTransaction tx{ChallengeTransaction()};
    const uint256 challenge{P2CClaimChallenge(tx, 0)};
    const Bytes proof{StructuralProof("example.com", challenge)};
    P2CTlsProofView parsed;
    std::string error;
    BOOST_REQUIRE(ParseP2CTlsProof(proof, "example.com", challenge, parsed, error));
    BOOST_CHECK_EQUAL(parsed.certificate_chain.size(), 1U);
    BOOST_CHECK(!parsed.connection_work_hash.IsNull());

    uint256 maximum;
    std::fill(maximum.begin(), maximum.end(), 0xff);
    BOOST_CHECK(P2CMeetsWorkTarget(parsed.connection_work_hash, maximum));
    BOOST_CHECK(!P2CMeetsWorkTarget(parsed.connection_work_hash, uint256{}));

    uint256 wrong_challenge{challenge};
    wrong_challenge.begin()[0] ^= 1;
    BOOST_CHECK(!ParseP2CTlsProof(proof, "example.com", wrong_challenge, parsed, error));
    BOOST_CHECK(!ParseP2CTlsProof(proof, "other.example", challenge, parsed, error));

    Bytes trailing{proof};
    trailing.push_back(0);
    BOOST_CHECK(!ParseP2CTlsProof(trailing, "example.com", challenge, parsed, error));

    const Bytes malformed_share{StructuralProof("example.com", challenge, /*invalid_key_share=*/true)};
    BOOST_CHECK(!ParseP2CTlsProof(malformed_share, "example.com", challenge, parsed, error));
}

BOOST_AUTO_TEST_CASE(immutable_root_store_v1_is_parseable)
{
    BOOST_CHECK(P2CRootStoreAvailable());
}

BOOST_AUTO_TEST_CASE(consensus_spend_path_rejects_an_untrusted_tls_proof)
{
    CKey destination_key;
    destination_key.MakeNewKey(/*fCompressed=*/true);
    uint256 target;
    std::fill(target.begin(), target.end(), 0xff);
    const CTxOut prevout{1000, PayToDomainOutput{
        .domain = "example.com",
        .connection_work_target = target,
        .root_certificates_version = P2C_ROOT_CERTIFICATES_VERSION_1,
    }};

    CMutableTransaction spending;
    spending.vin.emplace_back(Txid::FromUint256(uint256::ONE), 0);
    spending.vout.emplace_back(900, XOnlyPubKey{destination_key.GetPubKey()});
    const CTransaction unsigned_tx{spending};
    spending.vin[0].scriptWitness.stack = {
        StructuralProof("example.com", P2CClaimChallenge(unsigned_tx, 0)),
    };

    const CTransaction final_tx{spending};
    PrecomputedTransactionData txdata;
    txdata.Init(final_tx, {prevout}, /*force=*/true);
    SignatureCache signature_cache{1 << 20};
    CScriptCheck check{prevout, final_tx, signature_cache, /*nInIn=*/0, SCRIPT_VERIFY_NONE,
                       /*cacheIn=*/true, &txdata, /*p2c_validation_time=*/1800000000};
    const auto failure{check()};
    BOOST_REQUIRE(failure.has_value());
    BOOST_CHECK(failure->second.find("P2C proof contains an invalid DER certificate") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(rpc_constructs_only_the_single_domain_p2c_form)
{
    UniValue spec{UniValue::VOBJ};
    spec.pushKV("amount", "1.25");
    spec.pushKV("domain", "example.com");
    spec.pushKV("connection_work_target", uint256::ONE.GetHex());
    spec.pushKV("root_certificates_version", P2C_ROOT_CERTIFICATES_VERSION_1);
    UniValue entry{UniValue::VOBJ};
    entry.pushKV("p2c", spec);
    UniValue outputs{UniValue::VARR};
    outputs.push_back(entry);

    CMutableTransaction tx;
    AddOutputs(tx, outputs);
    BOOST_REQUIRE_EQUAL(tx.vout.size(), 1U);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(tx.vout[0].GetType()), 2U);
    const auto p2c{tx.vout[0].GetPayToDomain()};
    BOOST_REQUIRE(p2c);
    BOOST_CHECK_EQUAL(p2c->domain, "example.com");
    BOOST_CHECK_EQUAL(p2c->root_certificates_version, P2C_ROOT_CERTIFICATES_VERSION_1);

    UniValue obsolete_spec{spec};
    obsolete_spec.pushKV("mode", "domain");
    UniValue obsolete_entry{UniValue::VOBJ};
    obsolete_entry.pushKV("p2c", obsolete_spec);
    UniValue obsolete_outputs{UniValue::VARR};
    obsolete_outputs.push_back(obsolete_entry);
    CMutableTransaction rejected;
    BOOST_CHECK_THROW(AddOutputs(rejected, obsolete_outputs), UniValue);
}

BOOST_AUTO_TEST_CASE(domain_tls_proof_verifies_end_to_end)
{
    static constexpr unsigned char TEST_ROOTS_PEM[]{R"pem(-----BEGIN CERTIFICATE-----
MIICBzCCAYugAwIBAgIJAMFD4n5iQ8zoMAwGCCqGSM49BAMCBQAwPjELMAkGA1UE
BhMCTkwxETAPBgNVBAoMCFBvbGFyU1NMMRwwGgYDVQQDDBNQb2xhcnNzbCBUZXN0
IEVDIENBMB4XDTE5MDIxMDE0NDQwMFoXDTI5MDIxMDE0NDQwMFowPjELMAkGA1UE
BhMCTkwxETAPBgNVBAoMCFBvbGFyU1NMMRwwGgYDVQQDDBNQb2xhcnNzbCBUZXN0
IEVDIENBMHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEw9orNEE3WC+HVv78ibopQ0tO
4G7DDldTMzlY1FK0kZU5CyPfXxckYkj8GpUpziwth8KIUoCv1mqrId240xxuWLjK
6LJpjvNBrSnDtF91p0dv1RkpVWmaUzsgtGYWYDMeo1MwUTAPBgNVHRMBAf8EBTAD
AQH/MB0GA1UdDgQWBBSdbSAkSQE/K8t4tRm8fiTJ2/s2fDAfBgNVHSMEGDAWgBSd
bSAkSQE/K8t4tRm8fiTJ2/s2fDAMBggqhkjOPQQDAgUAA2gAMGUCMQDpNWfBIlzq
6xV2UwQD/1YGz9fQUM7AfNKzVa2PVBpf/QD1TAylTYTF4GI6qlb6EPYCMF/YVa29
N5yC1mFAir19jb9Pl9iiIkRm17dM4y6m5VIMepEPm/VlWAa8H5p1+BPbGw==
-----END CERTIFICATE-----
)pem"};
    static constexpr char CERTIFICATE_PEM[] = R"pem(-----BEGIN CERTIFICATE-----
MIICIDCCAaWgAwIBAgIBCTAKBggqhkjOPQQDAjA+MQswCQYDVQQGEwJOTDERMA8G
A1UECgwIUG9sYXJTU0wxHDAaBgNVBAMME1BvbGFyc3NsIFRlc3QgRUMgQ0EwHhcN
MjMwNTE3MDcxMDM2WhcNMzMwNTE0MDcxMDM2WjA0MQswCQYDVQQGEwJOTDERMA8G
A1UECgwIUG9sYXJTU0wxEjAQBgNVBAMMCWxvY2FsaG9zdDBZMBMGByqGSM49AgEG
CCqGSM49AwEHA0IABDfMVtl2CR5acj7HWS3/IG7ufPkGkXTQrRS192giWWKSTuUA
2CMR/+ov0jRdXRa9iojCa3cNVc2KKg76Aci07f+jgZ0wgZowCQYDVR0TBAIwADAd
BgNVHQ4EFgQUUGGlj9QH2deCAQzlZX+MY0anE74wbgYDVR0jBGcwZYAUnW0gJEkB
PyvLeLUZvH4kydv7NnyhQqRAMD4xCzAJBgNVBAYTAk5MMREwDwYDVQQKDAhQb2xh
clNTTDEcMBoGA1UEAwwTUG9sYXJzc2wgVGVzdCBFQyBDQYIJAMFD4n5iQ8zoMAoG
CCqGSM49BAMCA2kAMGYCMQDg6p7PPfr2+n7nGvya3pU4ust3k7Obk4/tZX+uHHRQ
qaccsyULeFNzkyRvWHFeT5sCMQCzDJX79Ii7hILYza/iXWJe/BjJEE8MteCRGXDN
06jC+BLgOH1KQV9ArqEh3AhOhEg=
-----END CERTIFICATE-----
)pem";
    static constexpr char PRIVATE_KEY_PEM[] = R"pem(-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIPEqEyB2AnCoPL/9U/YDHvdqXYbIogTywwyp6/UfDw6noAoGCCqGSM49
AwEHoUQDQgAEN8xW2XYJHlpyPsdZLf8gbu58+QaRdNCtFLX3aCJZYpJO5QDYIxH/
6i/SNF1dFr2KiMJrdw1VzYoqDvoByLTt/w==
-----END EC PRIVATE KEY-----
)pem";

    BOOST_REQUIRE(psa_crypto_init() == PSA_SUCCESS);
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context key;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&rng);
    mbedtls_x509_crt_init(&certificate);
    mbedtls_pk_init(&key);
    const unsigned char personalization[]{'p', '2', 'c', '-', 't', 'e', 's', 't'};
    BOOST_REQUIRE_EQUAL(mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &entropy,
                                             personalization, sizeof(personalization)), 0);
    BOOST_REQUIRE_EQUAL(mbedtls_x509_crt_parse(&certificate,
        reinterpret_cast<const unsigned char*>(CERTIFICATE_PEM), sizeof(CERTIFICATE_PEM)), 0);
    BOOST_REQUIRE_EQUAL(mbedtls_pk_parse_key(&key,
        reinterpret_cast<const unsigned char*>(PRIVATE_KEY_PEM), sizeof(PRIVATE_KEY_PEM),
        nullptr, 0, mbedtls_ctr_drbg_random, &rng), 0);

    CKey destination_key;
    destination_key.MakeNewKey(/*fCompressed=*/true);
    CMutableTransaction spending;
    spending.vin.emplace_back(Txid::FromUint256(uint256::ONE), 0);
    spending.vout.emplace_back(1, XOnlyPubKey{destination_key.GetPubKey()});
    const CTransaction unsigned_tx{spending};
    const uint256 challenge{P2CClaimChallenge(unsigned_tx, 0)};

    const std::span<const unsigned char> certificate_der{certificate.raw.p, certificate.raw.len};
    Bytes placeholder{StructuralProof("localhost", challenge, false, certificate_der)};
    P2CTlsProofView parsed;
    std::string error;
    BOOST_REQUIRE(ParseP2CTlsProof(placeholder, "localhost", challenge, parsed, error));

    std::array<unsigned char, 64> prefix;
    prefix.fill(0x20);
    static constexpr std::string_view CONTEXT{"TLS 1.3, server CertificateVerify"};
    uint256 signed_hash;
    CSHA256 digest;
    digest.Write(prefix.data(), prefix.size());
    digest.Write(reinterpret_cast<const unsigned char*>(CONTEXT.data()), CONTEXT.size());
    const unsigned char separator{0};
    digest.Write(&separator, 1);
    digest.Write(parsed.transcript_hash.begin(), parsed.transcript_hash.size());
    digest.Finalize(signed_hash.begin());

    Bytes signature(MBEDTLS_PK_SIGNATURE_MAX_SIZE);
    size_t signature_size{0};
    BOOST_REQUIRE_EQUAL(mbedtls_pk_sign(&key, MBEDTLS_MD_SHA256, signed_hash.begin(), signed_hash.size(),
                                       signature.data(), signature.size(), &signature_size,
                                       mbedtls_ctr_drbg_random, &rng), 0);
    signature.resize(signature_size);
    Bytes proof{StructuralProof("localhost", challenge, false, certificate_der, signature)};
    BOOST_REQUIRE(ParseP2CTlsProof(proof, "localhost", challenge, parsed, error));

    uint256 target;
    std::fill(target.begin(), target.end(), 0xff);
    const CTxOut prevout{1000, PayToDomainOutput{
        .domain = "localhost",
        .connection_work_target = target,
        .root_certificates_version = P2C_ROOT_CERTIFICATES_VERSION_1,
    }};
    BOOST_CHECK(P2CMeetsWorkTarget(parsed.connection_work_hash, target));
    const std::span<const unsigned char> test_roots{TEST_ROOTS_PEM, sizeof(TEST_ROOTS_PEM) - 1};
    BOOST_CHECK(VerifyP2CCertificateProofForTest(prevout, parsed, /*validation_time=*/1800000000,
                                                test_roots, error));
    BOOST_CHECK(!VerifyP2CCertificateProofForTest(prevout, parsed, /*validation_time=*/1600000000,
                                                 test_roots, error));
    BOOST_CHECK(!VerifyP2CCertificateProofForTest(prevout, parsed, /*validation_time=*/2100000000,
                                                 test_roots, error));

    proof.back() ^= 1;
    BOOST_REQUIRE(ParseP2CTlsProof(proof, "localhost", challenge, parsed, error));
    BOOST_CHECK(!VerifyP2CCertificateProofForTest(prevout, parsed, /*validation_time=*/1800000000,
                                                 test_roots, error));

    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&certificate);
    mbedtls_ctr_drbg_free(&rng);
    mbedtls_entropy_free(&entropy);
}

BOOST_AUTO_TEST_SUITE_END()
