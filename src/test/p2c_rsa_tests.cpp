// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/p2c.h>
#include <consensus/p2c_x509.h>
#include <consensus/p2c_x509_mutex.h>
#include <crypto/mbedtls_rsa_pss.h>
#include <crypto/sha256.h>
#include <primitives/transaction.h>
#include <random.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using Bytes = std::vector<unsigned char>;

int Random(void*, unsigned char* out, size_t len)
{
    while (len != 0) {
        const size_t count{std::min(len, size_t{32})};
        GetStrongRandBytes(std::span{out, count});
        out += count;
        len -= count;
    }
    return 0;
}

struct Key {
    mbedtls_pk_context value;
    Key() { mbedtls_pk_init(&value); }
    ~Key() { mbedtls_pk_free(&value); }
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
};

struct Certificate {
    mbedtls_x509_crt value;
    Certificate() { mbedtls_x509_crt_init(&value); }
    ~Certificate() { mbedtls_x509_crt_free(&value); }
    Certificate(const Certificate&) = delete;
    Certificate& operator=(const Certificate&) = delete;
};

Bytes Join(std::initializer_list<std::span<const unsigned char>> parts)
{
    Bytes result;
    for (const auto part : parts) result.insert(result.end(), part.begin(), part.end());
    return result;
}

Bytes Der(unsigned char tag, std::span<const unsigned char> contents)
{
    Bytes result{tag};
    if (contents.size() < 128) {
        result.push_back(static_cast<unsigned char>(contents.size()));
    } else if (contents.size() < 256) {
        result.insert(result.end(), {0x81, static_cast<unsigned char>(contents.size())});
    } else {
        BOOST_REQUIRE_LT(contents.size(), 65536U);
        result.insert(result.end(), {0x82, static_cast<unsigned char>(contents.size() >> 8), static_cast<unsigned char>(contents.size())});
    }
    result.insert(result.end(), contents.begin(), contents.end());
    return result;
}

std::span<const unsigned char> TakeDer(std::span<const unsigned char>& input,
                                     std::span<const unsigned char>* contents = nullptr)
{
    const auto original{input};
    BOOST_REQUIRE_GE(input.size(), 2U);
    size_t header{2};
    size_t length{input[1]};
    if (length & 0x80) {
        const size_t count{length & 0x7f};
        BOOST_REQUIRE(count == 1 || count == 2);
        BOOST_REQUIRE_GE(input.size(), 2 + count);
        length = 0;
        for (size_t i{0}; i < count; ++i) length = (length << 8) | input[header++];
    }
    BOOST_REQUIRE_LE(header + length, input.size());
    if (contents) *contents = input.subspan(header, length);
    input = input.subspan(header + length);
    return original.first(header + length);
}

Bytes Oid(const char* bytes, size_t len)
{
    return Der(0x06, {reinterpret_cast<const unsigned char*>(bytes), len});
}

Bytes HashAlgorithm(unsigned char sha_suffix)
{
    // NIST hash OID: 2.16.840.1.101.3.4.2.{1=SHA256,2=SHA384}.
    return Der(0x30, Join({Bytes{0x06, 9, 0x60, 0x86, 0x48, 1, 0x65, 3, 4, 2, sha_suffix}, Bytes{0x05, 0}}));
}

Bytes PssParams(unsigned char hash = 1, unsigned char mgf_hash = 1, unsigned char min_salt = 32)
{
    const Bytes mgf{Der(0x30, Join({Oid(MBEDTLS_OID_MGF1, MBEDTLS_OID_SIZE(MBEDTLS_OID_MGF1)), HashAlgorithm(mgf_hash)}))};
    return Der(0x30, Join({Der(0xa0, HashAlgorithm(hash)), Der(0xa1, mgf), Der(0xa2, Bytes{0x02, 1, min_salt})}));
}

Bytes PssAlgorithm(const Bytes& params)
{
    return Der(0x30, Join({Oid(MBEDTLS_OID_RSASSA_PSS, MBEDTLS_OID_SIZE(MBEDTLS_OID_RSASSA_PSS)), params}));
}

/** Use a deliberately public, test-only RSA-2048 key. Generating fresh primes
 * for every fixture dominated sanitizer runtime, but these tests exercise
 * certificate parsing, TLS and signature verification, not RSA key generation.
 * Certificates and signatures are still created and checked at runtime.
 * No production roots or stored wallet keys participate in these fixtures.
 */
struct RsaFixture {
    Key key;
    Bytes root;
    Bytes leaf;

    RsaFixture()
    {
        // Generated solely for this fixture with:
        // openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048
        // This private key is PUBLIC TEST DATA. Never use it outside tests.
        static constexpr unsigned char KEY[]{R"pem(-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQC6/43cbDm0ABiD
s2KWDhAuI4gIlJ9Zt1H7zAovmYAZpumeoZrxzxtHg0OnNGpDWDW/qnERgITwyE+G
KZqJR3sXIunpS6NgUf13HfAqmk8yLRMpnwh+DWFq/Q6EilAsztfeJjz3rcK1DIJp
8Zvph3kLN8Z9ukAHMthVmNF50Sf813I57c1Yt2bKYh+dDqggaprovpQHaOrVqedz
x5IZysbcnpi20XYoC4cYVIvlP+szmJlgswRzk5wOFe5Qm8JXltWnjqRK/Oy1qARW
rzrOfrc5u3GZeFZDPYS0r2ki6ZsyJiNKnT8JPKVEVecqc6VgA0l8z3TyhNSewGpV
BEKDovtnAgMBAAECggEANhyiW/ETZ5OJhH7h3eM26msMv9LmI8uJFVCPeAO2znV+
8BD6qdORJMoGxzlDMLazYwG602I51gVZAc1DM0t0gpbvUju5jLNdId2PdHyPw0jI
3UfwaK2NjaypyU/O8JBwZg/xn4hwKfzzNh4czGCP9d+PeC1vvsWHVYmxwEr2g9MD
nHyhyPiPs3hCzfrOaffETIPgvZJ/DKg7ItSYqmmiK/a9LceViLSb/7fLdfL/9ESk
cxmHQtHEJZTkkFA12wH4I8K6URDRE/RDgphJI5jYtpeIPZSROc6mZSh4sW06aD6N
jHNtWr/xTbmWDRS0mMyvGMRH5d5GE2eZSaG6MlUsOQKBgQDnrk1aoHOk65PapdB3
LRp9nZsdgi+hUfgn2yEGX8RmHm7zm9Pc1/8dzoGvQRy+YM8u3fqQrh82et9BXq7w
nf3XVE7AAcDdjKwHFNlbscbQrbC/GHU63lqSkXCFBcbNJuFwlw/vJArHR0x+qBy6
cwU5JkRZd8p61PNhfEKSUUHvLwKBgQDOoIv5LlW+BdFuYkmqrgww6sfsZnhWwvQC
tkcMP+csjglmjtkiVFUbXIyojWYmtiXFyzCPhTaIIKeSWywevXQ0+qowFdDC38Zw
iiwNWPbUCnZk0PSLV5ryWzNPntdcmX5M+TvSYwcPt1RzoUaKhfOVVHdZduDgmy/U
mJiZlBzpSQKBgASgoaDmxYiMwAZE+5X1y6qopDmBqSvitD8vjEhRT13uy66H9UJa
+hiBUGvMtCNFUb4Q5vlO0QbIi38Fwh7CORi88Vm6bzy9m44Ep5bCRUNTxMz8UxMa
79ovl3zAscjVNvmFuua+5Iw4a1m4R+Kde4Q5tHHJB71OVZIj5jx/7P43AoGAcL0s
YkMjyVCHWsEKDLR2NmKDvrqSQlSQqsIltctQKQE+o9ShKJf277zpijXMXKbZqTga
QNSgUlnu1G4mfodEVnvGTAI7K3jJXzIkowu9cShcPNm99CFSi5WzQ2gZfY7KWNlM
CJi7i5mt3IFMadx4cSvrCsdQH3zM9iRkbrdfpvECgYA7W1441Ps8HWby9n0SRFpV
QVdTwGHpgagi/3QpuS9QMAoPwgCYUW/eFGrFo7M5G/0TNVQlEpA+Bz+CnZMkyzQ6
Snwm9dDUNZiK2/gtbNjnUNVt+qMzgfsPkxNeZYQW49uwoG7Cc9Qrm8XgiRLvhcFe
lNgW44LomPeKsFSCCcqrVg==
-----END PRIVATE KEY-----
)pem"};
        BOOST_REQUIRE_EQUAL(psa_crypto_init(), PSA_SUCCESS);
        BOOST_REQUIRE_EQUAL(mbedtls_pk_parse_key(&key.value, KEY, sizeof(KEY), nullptr, 0, Random, nullptr), 0);
        BOOST_REQUIRE_EQUAL(mbedtls_pk_get_bitlen(&key.value), 2048U);
        BOOST_REQUIRE_EQUAL(mbedtls_rsa_check_privkey(mbedtls_pk_rsa(key.value)), 0);
        root = MakeCertificate(true);
        leaf = MakeCertificate(false);
    }

    Bytes MakeCertificate(bool ca)
    {
        mbedtls_x509write_cert writer;
        mbedtls_x509write_crt_init(&writer);
        mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
        unsigned char serial{static_cast<unsigned char>(ca ? 1 : 2)};
        BOOST_REQUIRE_EQUAL(mbedtls_x509write_crt_set_serial_raw(&writer, &serial, 1), 0);
        BOOST_REQUIRE_EQUAL(mbedtls_x509write_crt_set_subject_name(&writer, ca ? "CN=P2C RSA Test CA" : "CN=localhost"), 0);
        BOOST_REQUIRE_EQUAL(mbedtls_x509write_crt_set_issuer_name(&writer, "CN=P2C RSA Test CA"), 0);
        BOOST_REQUIRE_EQUAL(mbedtls_x509write_crt_set_validity(&writer, "20200101000000", "20400101000000"), 0);
        mbedtls_x509write_crt_set_subject_key(&writer, &key.value);
        mbedtls_x509write_crt_set_issuer_key(&writer, &key.value);
        mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
        BOOST_REQUIRE_EQUAL(mbedtls_x509write_crt_set_basic_constraints(&writer, ca, ca ? 1 : -1), 0);
        BOOST_REQUIRE_EQUAL(mbedtls_x509write_crt_set_key_usage(&writer,
            ca ? MBEDTLS_X509_KU_KEY_CERT_SIGN : MBEDTLS_X509_KU_DIGITAL_SIGNATURE), 0);
        Bytes buffer(4096);
        const int len{mbedtls_x509write_crt_der(&writer, buffer.data(), buffer.size(), Random, nullptr)};
        mbedtls_x509write_crt_free(&writer);
        BOOST_REQUIRE_GT(len, 0);
        return {buffer.end() - len, buffer.end()};
    }

    Bytes PssSign(const uint256& hash, int salt_len = 32, mbedtls_md_type_t encoding_hash = MBEDTLS_MD_SHA256)
    {
        auto* rsa{mbedtls_pk_rsa(key.value)};
        BOOST_REQUIRE_EQUAL(mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, encoding_hash), 0);
        Bytes signature(mbedtls_pk_get_len(&key.value));
        const int result{mbedtls_rsa_rsassa_pss_sign_ext(rsa, Random, nullptr, MBEDTLS_MD_SHA256,
            hash.size(), hash.begin(), salt_len, signature.data())};
        BOOST_REQUIRE_EQUAL(mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE), 0);
        BOOST_REQUIRE_EQUAL(result, 0);
        return signature;
    }

    Bytes Rewrite(const Bytes& original, const std::optional<Bytes>& spki_algorithm, bool pss_signature = false)
    {
        std::span<const unsigned char> outer{original}, contents, tbs_contents;
        TakeDer(outer, &contents);
        BOOST_REQUIRE(outer.empty());
        TakeDer(contents, &tbs_contents);
        const auto original_sig_alg{TakeDer(contents)};
        Bytes tbs;
        for (unsigned field{0}; !tbs_contents.empty(); ++field) {
            std::span<const unsigned char> field_contents;
            const auto encoded{TakeDer(tbs_contents, &field_contents)};
            Bytes replacement;
            if (field == 2 && pss_signature) {
                replacement = PssAlgorithm(PssParams());
            } else if (field == 6 && spki_algorithm) {
                TakeDer(field_contents); // Original SPKI AlgorithmIdentifier.
                replacement = Der(0x30, Join({*spki_algorithm, field_contents}));
            } else {
                replacement.assign(encoded.begin(), encoded.end());
            }
            tbs.insert(tbs.end(), replacement.begin(), replacement.end());
        }
        const Bytes encoded_tbs{Der(0x30, tbs)};
        uint256 hash;
        CSHA256().Write(encoded_tbs.data(), encoded_tbs.size()).Finalize(hash.begin());
        Bytes signature;
        if (pss_signature) {
            signature = PssSign(hash);
        } else {
            signature.resize(mbedtls_pk_get_len(&key.value));
            size_t len{0};
            BOOST_REQUIRE_EQUAL(mbedtls_pk_sign(&key.value, MBEDTLS_MD_SHA256, hash.begin(), hash.size(),
                signature.data(), signature.size(), &len, Random, nullptr), 0);
            signature.resize(len);
        }
        const Bytes sig_alg{pss_signature ? PssAlgorithm(PssParams()) : Bytes{original_sig_alg.begin(), original_sig_alg.end()}};
        return Der(0x30, Join({encoded_tbs, sig_alg, Der(0x03, Join({Bytes{0}, signature}))}));
    }

    static Bytes Pem(const Bytes& der)
    {
        const std::string pem{"-----BEGIN CERTIFICATE-----\n" + EncodeBase64(der) + "\n-----END CERTIFICATE-----\n"};
        return {pem.begin(), pem.end()};
    }

    static uint256 SignedHash(const uint256& transcript = uint256{})
    {
        std::array<unsigned char, 64> prefix;
        prefix.fill(0x20);
        constexpr std::string_view context{"TLS 1.3, server CertificateVerify"};
        constexpr unsigned char separator{0};
        uint256 hash;
        CSHA256().Write(prefix.data(), prefix.size())
            .Write(reinterpret_cast<const unsigned char*>(context.data()), context.size())
            .Write(&separator, 1).Write(transcript.begin(), transcript.size()).Finalize(hash.begin());
        return hash;
    }

    bool Verify(const Bytes& certificate, uint16_t scheme, const Bytes& signature,
                std::string& error, std::string domain = "localhost", int64_t time = 1800000000,
                const Bytes* trust_root = nullptr,
                uint8_t mask = PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL)
    {
        uint256 target;
        std::fill(target.begin(), target.end(), 0xff);
        const CTxOut spent{1000, PayToDomainOutput{
            .domain = std::move(domain), .connection_work_target = target,
            .root_certificates_version = P2C_ROOT_CERTIFICATES_VERSION_1,
            .signature_algorithms_mask = mask,
        }};
        P2CTlsProofView proof;
        proof.certificate_chain = {certificate};
        proof.certificate_verify_scheme = scheme;
        proof.certificate_verify_signature = signature;
        return VerifyP2CCertificateProofForTest(spent, proof, time, Pem(trust_root ? *trust_root : root), error);
    }
};

struct TlsPeer {
    mbedtls_ssl_config config;
    mbedtls_ssl_context ssl;
    std::deque<unsigned char> incoming;
    TlsPeer* other{nullptr};
    Bytes sent;
    TlsPeer() { mbedtls_ssl_config_init(&config); mbedtls_ssl_init(&ssl); }
    ~TlsPeer() { mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&config); }
    TlsPeer(const TlsPeer&) = delete;
    TlsPeer& operator=(const TlsPeer&) = delete;
    static int Send(void* opaque, const unsigned char* bytes, size_t len)
    {
        auto& peer{*static_cast<TlsPeer*>(opaque)};
        peer.sent.insert(peer.sent.end(), bytes, bytes + len);
        peer.other->incoming.insert(peer.other->incoming.end(), bytes, bytes + len);
        return static_cast<int>(len);
    }
    static int Receive(void* opaque, unsigned char* bytes, size_t len)
    {
        auto& peer{*static_cast<TlsPeer*>(opaque)};
        const size_t count{std::min(len, peer.incoming.size())};
        if (count == 0) return MBEDTLS_ERR_SSL_WANT_READ;
        for (size_t i{0}; i < count; ++i) { bytes[i] = peer.incoming.front(); peer.incoming.pop_front(); }
        return static_cast<int>(count);
    }
    void Setup(int endpoint, const uint16_t* schemes, Certificate* cert = nullptr, Key* key = nullptr)
    {
        BOOST_REQUIRE_EQUAL(mbedtls_ssl_config_defaults(&config, endpoint, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT), 0);
        mbedtls_ssl_conf_rng(&config, Random, nullptr);
        mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_max_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_tls13_key_exchange_modes(&config, MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL);
        static constexpr int suites[]{MBEDTLS_TLS1_3_AES_128_GCM_SHA256, 0};
        mbedtls_ssl_conf_ciphersuites(&config, suites);
        mbedtls_ssl_conf_sig_algs(&config, schemes);
        if (cert) BOOST_REQUIRE_EQUAL(mbedtls_ssl_conf_own_cert(&config, &cert->value, &key->value), 0);
        BOOST_REQUIRE_EQUAL(mbedtls_ssl_setup(&ssl, &config), 0);
        mbedtls_ssl_set_bio(&ssl, this, Send, Receive, nullptr);
    }
};

bool Handshake(RsaFixture& fixture, const Bytes& certificate, uint16_t scheme, Bytes& client_flight)
{
    Certificate cert;
    BOOST_REQUIRE_EQUAL(mbedtls_x509_crt_parse_der(&cert.value, certificate.data(), certificate.size()), 0);
    const uint16_t schemes[]{scheme, 0};
    TlsPeer client, server;
    client.other = &server;
    server.other = &client;
    client.Setup(MBEDTLS_SSL_IS_CLIENT, schemes);
    server.Setup(MBEDTLS_SSL_IS_SERVER, schemes, &cert, &fixture.key);
    int client_result{MBEDTLS_ERR_SSL_WANT_READ}, server_result{MBEDTLS_ERR_SSL_WANT_READ};
    for (unsigned i{0}; i < 100; ++i) {
        if (client_result != 0) client_result = mbedtls_ssl_handshake(&client.ssl);
        if (client_flight.empty()) client_flight = client.sent;
        if (client_result != 0 && client_result != MBEDTLS_ERR_SSL_WANT_READ && client_result != MBEDTLS_ERR_SSL_WANT_WRITE) return false;
        if (server_result != 0) server_result = mbedtls_ssl_handshake(&server.ssl);
        if (server_result != 0 && server_result != MBEDTLS_ERR_SSL_WANT_READ && server_result != MBEDTLS_ERR_SSL_WANT_WRITE) return false;
        if (client_result == 0 && server_result == 0) return true;
    }
    return false;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(p2c_rsa_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(rsae_and_true_pss_require_their_original_spki_scheme)
{
    RsaFixture fixture;
    const Bytes signature{fixture.PssSign(RsaFixture::SignedHash())};
    std::string error;
    BOOST_REQUIRE_MESSAGE(fixture.Verify(fixture.leaf, 0x0804, signature, error), error);
    BOOST_CHECK(!fixture.Verify(fixture.leaf, 0x0809, signature, error));
    BOOST_CHECK_EQUAL(error, "CertificateVerify scheme does not match leaf RSA key");
    BOOST_CHECK(!fixture.Verify(fixture.leaf, 0x0804, signature, error, "localhost", 1800000000, nullptr,
        PayToDomainOutput::SIGNATURE_ALGORITHM_RSA_PSS_PSS_SHA256));
    BOOST_CHECK_EQUAL(error, "P2C CertificateVerify scheme is not allowed by the output");
    for (const auto& params : {Bytes{}, PssParams(), PssParams(1, 1, 20)}) {
        const Bytes certificate{fixture.Rewrite(fixture.leaf, PssAlgorithm(params))};
        Certificate parsed;
        BOOST_REQUIRE_EQUAL(mbedtls_x509_crt_parse_der(&parsed.value, certificate.data(), certificate.size()), 0);
        BOOST_CHECK(connectcoin_mbedtls_pss_key(&parsed.value.pk));
        Bytes exported(4096);
        BOOST_CHECK_EQUAL(mbedtls_pk_write_pubkey_der(&parsed.value.pk, exported.data(), exported.size()), MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE);
        BOOST_CHECK_EQUAL(mbedtls_pk_write_key_der(&parsed.value.pk, exported.data(), exported.size()), MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE);
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        BOOST_CHECK_EQUAL(mbedtls_pk_get_psa_attributes(&parsed.value.pk, PSA_KEY_USAGE_VERIFY_HASH, &attributes), MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE);
        mbedtls_svc_key_id_t psa_key = MBEDTLS_SVC_KEY_ID_INIT;
        BOOST_CHECK_EQUAL(mbedtls_pk_import_into_psa(&parsed.value.pk, &attributes, &psa_key), MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE);
        BOOST_CHECK(mbedtls_svc_key_id_is_null(psa_key));
        psa_reset_key_attributes(&attributes);
        BOOST_REQUIRE_MESSAGE(fixture.Verify(certificate, 0x0809, signature, error), error);
        BOOST_CHECK(!fixture.Verify(certificate, 0x0809, signature, error, "localhost", 1800000000, nullptr,
            PayToDomainOutput::SIGNATURE_ALGORITHM_RSA_PSS_RSAE_SHA256));
        BOOST_CHECK_EQUAL(error, "P2C CertificateVerify scheme is not allowed by the output");
        // Identical modulus, PSS signature, and transcript: only the TLS
        // scheme changes. RSA arithmetic capability cannot authorize RSAE.
        BOOST_CHECK(!fixture.Verify(certificate, 0x0804, signature, error));
        BOOST_CHECK_EQUAL(error, "CertificateVerify scheme does not match leaf RSA key");
    }
}

BOOST_AUTO_TEST_CASE(pss_key_constraints_and_parameter_encoding_are_enforced)
{
    RsaFixture fixture;
    const Bytes signature{fixture.PssSign(RsaFixture::SignedHash())};
    std::string error;
    for (const auto& params : {PssParams(2, 1), PssParams(1, 2), PssParams(1, 1, 33), Bytes{0x30, 0}}) {
        const Bytes certificate{fixture.Rewrite(fixture.leaf, PssAlgorithm(params))};
        BOOST_CHECK(!fixture.Verify(certificate, 0x0809, signature, error));
        BOOST_CHECK_EQUAL(error, "CertificateVerify scheme does not match leaf RSA key");
    }
    // NULL, duplicate fields, invalid trailer, negative salt, truncated field,
    // and non-MGF1 algorithm must never become unconstrained RSA keys.
    const std::vector<Bytes> malformed{
        {0x05, 0},
        Der(0x30, Join({Der(0xa0, HashAlgorithm(1)), Der(0xa0, HashAlgorithm(1))})),
        Der(0x30, Bytes{0xa3, 3, 2, 1, 2}),
        Der(0x30, Bytes{0xa2, 3, 2, 1, 0xff}),
        Der(0x30, Bytes{0xa0, 5, 0x30}),
        Der(0x30, Der(0xa1, HashAlgorithm(1))),
    };
    for (const auto& params : malformed) {
        const Bytes certificate{fixture.Rewrite(fixture.leaf, PssAlgorithm(params))};
        BOOST_CHECK(!fixture.Verify(certificate, 0x0809, signature, error));
        BOOST_CHECK_EQUAL(error, "P2C proof contains an invalid DER certificate");
    }
}

BOOST_AUTO_TEST_CASE(pss_signature_salt_hash_domain_time_and_chain_checks)
{
    RsaFixture fixture;
    const Bytes pss_leaf{fixture.Rewrite(fixture.leaf, PssAlgorithm(PssParams()))};
    std::string error;
    for (int salt : {0, 20, 31, 33, 48}) {
        const Bytes signature{fixture.PssSign(RsaFixture::SignedHash(), salt)};
        BOOST_CHECK(!fixture.Verify(fixture.leaf, 0x0804, signature, error));
        BOOST_CHECK_EQUAL(error, "invalid P2C TLS CertificateVerify signature");
        BOOST_CHECK(!fixture.Verify(pss_leaf, 0x0809, signature, error));
    }
    const Bytes signature{fixture.PssSign(RsaFixture::SignedHash())};
    BOOST_REQUIRE_MESSAGE(fixture.Verify(pss_leaf, 0x0809, signature, error), error);
    const Bytes wrong_hash_signature{fixture.PssSign(RsaFixture::SignedHash(), 32, MBEDTLS_MD_SHA384)};
    BOOST_CHECK(!fixture.Verify(pss_leaf, 0x0809, wrong_hash_signature, error));
    BOOST_CHECK(!fixture.Verify(pss_leaf, 0x0809, signature, error, "other.example"));
    BOOST_CHECK(!fixture.Verify(pss_leaf, 0x0809, signature, error, "localhost", 1500000000));
    BOOST_CHECK(!fixture.Verify(pss_leaf, 0x0809, signature, error, "localhost", 2300000000));
    Bytes invalid_certificate{pss_leaf};
    invalid_certificate.back() ^= 1;
    BOOST_CHECK(!fixture.Verify(invalid_certificate, 0x0809, signature, error));
    BOOST_CHECK_EQUAL(error, "P2C certificate path or domain validation failed");

    // A PSS CA is also restricted: a PKCS#1-v1.5 child signature is forbidden,
    // while a real SHA256/PSS child signature validates under the same key.
    const Bytes pss_root{fixture.Rewrite(fixture.root, PssAlgorithm(PssParams()))};
    BOOST_CHECK(!fixture.Verify(pss_leaf, 0x0809, signature, error, "localhost", 1800000000, &pss_root));
    const Bytes pss_signed_leaf{fixture.Rewrite(pss_leaf, std::nullopt, true)};
    BOOST_REQUIRE_MESSAGE(fixture.Verify(pss_signed_leaf, 0x0809, signature, error, "localhost", 1800000000, &pss_root), error);
    const Bytes incompatible_root{fixture.Rewrite(fixture.root, PssAlgorithm(PssParams(1, 1, 33)))};
    BOOST_CHECK(!fixture.Verify(pss_signed_leaf, 0x0809, signature, error, "localhost", 1800000000, &incompatible_root));
}

BOOST_AUTO_TEST_CASE(tls13_advertises_and_completes_real_pss_handshakes)
{
    RsaFixture fixture;
    const Bytes pss_leaf{fixture.Rewrite(fixture.leaf, PssAlgorithm(PssParams()))};
    Bytes client_flight;
    BOOST_REQUIRE(Handshake(fixture, pss_leaf, 0x0809, client_flight));
    const Bytes extension{0, 13, 0, 4, 0, 2, 8, 9};
    BOOST_CHECK(std::search(client_flight.begin(), client_flight.end(), extension.begin(), extension.end()) != client_flight.end());
    client_flight.clear();
    BOOST_CHECK(Handshake(fixture, fixture.leaf, 0x0804, client_flight));
    client_flight.clear();
    BOOST_CHECK(!Handshake(fixture, fixture.leaf, 0x0809, client_flight));
    client_flight.clear();
    BOOST_CHECK(!Handshake(fixture, pss_leaf, 0x0804, client_flight));
}

BOOST_AUTO_TEST_CASE(portable_root_cache_mutex_serializes_concurrent_updates)
{
    consensus::p2c::RootKeyCacheSpinMutex mutex;
    unsigned count{0};
    std::barrier start{4};
    std::vector<std::thread> threads;
    for (int i{0}; i < 4; ++i) {
        threads.emplace_back([&] {
            start.arrive_and_wait();
            for (int attempt{0}; attempt < 500; ++attempt) {
                const std::lock_guard lock{mutex};
                ++count;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    BOOST_CHECK_EQUAL(count, 2000U);
}

BOOST_AUTO_TEST_CASE(concurrent_certificate_validation_keeps_results_isolated)
{
    RsaFixture fixture;
    const Bytes certificate{fixture.Rewrite(fixture.leaf, PssAlgorithm(PssParams()))};
    const Bytes signature{fixture.PssSign(RsaFixture::SignedHash())};
    std::atomic<bool> valid{true};
    std::barrier start{4};
    std::vector<std::thread> threads;
    for (int i{0}; i < 4; ++i) {
        threads.emplace_back([&] {
            start.arrive_and_wait();
            for (int attempt{0}; attempt < 8; ++attempt) {
                std::string error;
                if (!P2CRootStoreAvailable() || !fixture.Verify(certificate, 0x0809, signature, error) || !error.empty()) valid = false;
                if (fixture.Verify(certificate, 0x0804, signature, error) ||
                    error != "CertificateVerify scheme does not match leaf RSA key") valid = false;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    BOOST_CHECK(valid.load());
}

BOOST_AUTO_TEST_SUITE_END()
