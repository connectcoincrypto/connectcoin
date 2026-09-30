// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/p2c.h>
#include <consensus/p2c_x509.h>
#include <consensus/p2c_x509_mutex.h>
#include <crypto/mbedtls_rsa_pss.h>
#include <crypto/mbedtls_x509_root_first.h>
#include <crypto/sha256.h>
#include <primitives/transaction.h>
#include <random.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <mbedtls/bignum.h>
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
#include <utility>
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

/** Replace only the public exponent in an RSA SubjectPublicKeyInfo. Keep the
 * original certificate signature deliberately invalid, without generating a
 * private key for unusual exponents or adding large binary test fixtures.
 */
Bytes WithRsaExponent(const Bytes& certificate, const Bytes& exponent, size_t expected_bits)
{
    std::span<const unsigned char> outer{certificate}, contents, tbs_contents;
    TakeDer(outer, &contents);
    BOOST_REQUIRE(outer.empty());
    TakeDer(contents, &tbs_contents);
    const Bytes signature_tail{contents.begin(), contents.end()};
    Bytes tbs;
    bool replaced{false};
    for (unsigned field{0}; !tbs_contents.empty(); ++field) {
        std::span<const unsigned char> field_contents;
        const auto encoded{TakeDer(tbs_contents, &field_contents)};
        Bytes replacement;
        if (field == 6) {
            const auto algorithm{TakeDer(field_contents)};
            std::span<const unsigned char> bit_string;
            TakeDer(field_contents, &bit_string);
            BOOST_REQUIRE(field_contents.empty());
            BOOST_REQUIRE(!bit_string.empty() && bit_string.front() == 0);
            auto key_der{bit_string.subspan(1)};
            std::span<const unsigned char> rsa_key;
            TakeDer(key_der, &rsa_key);
            BOOST_REQUIRE(key_der.empty());
            const auto modulus{TakeDer(rsa_key)};
            TakeDer(rsa_key); // Original public exponent.
            BOOST_REQUIRE(rsa_key.empty());
            const Bytes new_key{Der(0x30, Join({modulus, Der(0x02, exponent)}))};
            replacement = Der(0x30, Join({algorithm, Der(0x03, Join({Bytes{0}, new_key}))}));
            replaced = true;
        } else {
            replacement.assign(encoded.begin(), encoded.end());
        }
        tbs.insert(tbs.end(), replacement.begin(), replacement.end());
    }
    BOOST_REQUIRE(replaced);
    const Bytes result{Der(0x30, Join({Der(0x30, tbs), signature_tail}))};
    Certificate parsed;
    BOOST_REQUIRE_EQUAL(mbedtls_x509_crt_parse_der(&parsed.value, result.data(), result.size()), 0);
    BOOST_REQUIRE_EQUAL(mbedtls_pk_get_bitlen(&parsed.value.pk), 2048U);
    mbedtls_mpi actual;
    mbedtls_mpi_init(&actual);
    const int exported{mbedtls_rsa_export(mbedtls_pk_rsa(parsed.value.pk), nullptr, nullptr, nullptr, nullptr, &actual)};
    const size_t bits{mbedtls_mpi_bitlen(&actual)};
    mbedtls_mpi_free(&actual);
    BOOST_REQUIRE_EQUAL(exported, 0);
    BOOST_REQUIRE_EQUAL(bits, expected_bits);
    return result;
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

    Bytes MakeCertificate(bool ca, const char* subject = nullptr, const char* issuer = "CN=P2C RSA Test CA",
                          int max_pathlen = 1, unsigned int key_usage = 0)
    {
        mbedtls_x509write_cert writer;
        mbedtls_x509write_crt_init(&writer);
        mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
        unsigned char serial{static_cast<unsigned char>(ca ? 1 : 2)};
        BOOST_REQUIRE_EQUAL(mbedtls_x509write_crt_set_serial_raw(&writer, &serial, 1), 0);
        BOOST_REQUIRE_EQUAL(mbedtls_x509write_crt_set_subject_name(&writer, subject ? subject : ca ? "CN=P2C RSA Test CA" : "CN=localhost"), 0);
        BOOST_REQUIRE_EQUAL(mbedtls_x509write_crt_set_issuer_name(&writer, issuer), 0);
        BOOST_REQUIRE_EQUAL(mbedtls_x509write_crt_set_validity(&writer, "20200101000000", "20400101000000"), 0);
        mbedtls_x509write_crt_set_subject_key(&writer, &key.value);
        mbedtls_x509write_crt_set_issuer_key(&writer, &key.value);
        mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
        BOOST_REQUIRE_EQUAL(mbedtls_x509write_crt_set_basic_constraints(&writer, ca, ca ? max_pathlen : -1), 0);
        BOOST_REQUIRE_EQUAL(mbedtls_x509write_crt_set_key_usage(&writer,
            key_usage != 0 ? key_usage : ca ? MBEDTLS_X509_KU_KEY_CERT_SIGN : MBEDTLS_X509_KU_DIGITAL_SIGNATURE), 0);
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
                uint8_t mask = PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL,
                const std::vector<Bytes>& extra_certificates = {},
                const std::vector<Bytes>& extra_trust_roots = {})
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
        for (const auto& extra : extra_certificates) proof.certificate_chain.emplace_back(extra);
        proof.certificate_verify_scheme = scheme;
        proof.certificate_verify_signature = signature;
        Bytes roots_pem{Pem(trust_root ? *trust_root : root)};
        for (const auto& extra : extra_trust_roots) {
            const Bytes pem{Pem(extra)};
            roots_pem.insert(roots_pem.end(), pem.begin(), pem.end());
        }
        return VerifyP2CCertificateProofForTest(spent, proof, time, roots_pem, error);
    }
};

using SignatureEdge = std::pair<int, int>;

struct PathVerification {
    bool valid{false};
    std::vector<SignatureEdge> signatures;
};

/** Observer identities are the supplied-list index, or 100 + trust-list index.
 * Count actual signature checks, not callbacks after a path has already been
 * verified. No wall-clock threshold or production root participates here.
 */
struct SignatureTrace {
    const mbedtls_x509_crt* supplied;
    const mbedtls_x509_crt* roots;
    std::vector<SignatureEdge> signatures;

    int Index(const mbedtls_x509_crt* certificate) const
    {
        int index{0};
        for (const auto* current{supplied}; current != nullptr; current = current->next, ++index) {
            if (current == certificate) return index;
        }
        index = 100;
        for (const auto* current{roots}; current != nullptr; current = current->next, ++index) {
            if (current == certificate) return index;
        }
        return -1;
    }

    static void Observe(void* opaque, const mbedtls_x509_crt* child, const mbedtls_x509_crt* parent)
    {
        auto& trace{*static_cast<SignatureTrace*>(opaque)};
        trace.signatures.emplace_back(trace.Index(child), trace.Index(parent));
    }
};

PathVerification CompareRootFirstWithLegacy(const std::vector<Bytes>& supplied,
                                          const std::vector<Bytes>& trusted,
                                          const char* domain = "localhost",
                                          const mbedtls_x509_crt_profile* profile = &mbedtls_x509_crt_profile_default)
{
    Certificate chain, roots, legacy_chain, legacy_roots;
    for (const auto& certificate : supplied) {
        BOOST_REQUIRE_EQUAL(mbedtls_x509_crt_parse_der(&chain.value, certificate.data(), certificate.size()), 0);
        BOOST_REQUIRE_EQUAL(mbedtls_x509_crt_parse_der(&legacy_chain.value, certificate.data(), certificate.size()), 0);
    }
    for (const auto& certificate : trusted) {
        BOOST_REQUIRE_EQUAL(mbedtls_x509_crt_parse_der(&roots.value, certificate.data(), certificate.size()), 0);
        BOOST_REQUIRE_EQUAL(mbedtls_x509_crt_parse_der(&legacy_roots.value, certificate.data(), certificate.size()), 0);
    }
    SignatureTrace trace{&chain.value, &roots.value, {}};
    uint32_t flags{0}, legacy_flags{0};
    const int result{connectcoin_mbedtls_x509_crt_verify_root_first(
        &chain.value, trusted.empty() ? nullptr : &roots.value, profile, domain, &flags, SignatureTrace::Observe, &trace)};
    const int legacy_result{mbedtls_x509_crt_verify_with_profile(
        &legacy_chain.value, trusted.empty() ? nullptr : &legacy_roots.value, nullptr, profile, domain,
        &legacy_flags, nullptr, nullptr)};
    const bool valid{result == 0 && flags == 0};
    // Fail-fast may report fewer errors, but must not change acceptance.
    BOOST_CHECK_EQUAL(valid, legacy_result == 0 && legacy_flags == 0);
    return {valid, std::move(trace.signatures)};
}

void CheckSignatureOrder(const PathVerification& result, std::initializer_list<SignatureEdge> expected)
{
    BOOST_CHECK_EQUAL(result.signatures.size(), expected.size());
    BOOST_CHECK(result.signatures == std::vector<SignatureEdge>(expected));
}

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

BOOST_AUTO_TEST_CASE(root_first_path_signature_order_and_fail_fast)
{
    RsaFixture fixture;
    const Bytes original_leaf{fixture.MakeCertificate(false, "CN=localhost", "CN=P2C Order I1")};
    const Bytes original_i1{fixture.MakeCertificate(true, "CN=P2C Order I1", "CN=P2C Order I2")};
    const Bytes original_i2{fixture.MakeCertificate(true, "CN=P2C Order I2", "CN=P2C Order Root", 2)};
    const Bytes original_root{fixture.MakeCertificate(true, "CN=P2C Order Root", "CN=P2C Order Root", 3)};
    const Bytes unrelated_root{fixture.MakeCertificate(true, "CN=P2C Unrelated Root", "CN=P2C Unrelated Root", 3)};
    const Bytes cv_signature{fixture.PssSign(RsaFixture::SignedHash())};

    for (bool pss : {false, true}) {
        BOOST_TEST_CONTEXT("restricted PSS chain=" << pss) {
            const auto encode = [&](const Bytes& certificate) {
                return pss ? fixture.Rewrite(certificate, PssAlgorithm(PssParams()), true) : certificate;
            };
            const std::vector<Bytes> supplied{encode(original_leaf), encode(original_i1), encode(original_i2)};
            const Bytes root{encode(original_root)};
            const PathVerification valid{CompareRootFirstWithLegacy(supplied, {root})};
            BOOST_REQUIRE(valid.valid);
            CheckSignatureOrder(valid, {{2, 100}, {1, 2}, {0, 1}});

            for (size_t invalid_index{0}; invalid_index < supplied.size(); ++invalid_index) {
                BOOST_TEST_CONTEXT("invalid certificate index=" << invalid_index) {
                    auto invalid{supplied};
                    invalid[invalid_index].back() ^= 1; // Signature only; preserve valid DER and keys.
                    const PathVerification rejected{CompareRootFirstWithLegacy(invalid, {root})};
                    BOOST_CHECK(!rejected.valid);
                    if (invalid_index == 2) CheckSignatureOrder(rejected, {{2, 100}});
                    if (invalid_index == 1) CheckSignatureOrder(rejected, {{2, 100}, {1, 2}});
                    if (invalid_index == 0) CheckSignatureOrder(rejected, {{2, 100}, {1, 2}, {0, 1}});
                }
            }

            const PathVerification unanchored{CompareRootFirstWithLegacy(supplied, {unrelated_root})};
            BOOST_CHECK(!unanchored.valid);
            CheckSignatureOrder(unanchored, {});
            const PathVerification empty_roots{CompareRootFirstWithLegacy(supplied, {})};
            BOOST_CHECK(!empty_roots.valid);
            CheckSignatureOrder(empty_roots, {});

            auto with_root{supplied};
            with_root.push_back(root);
            const PathVerification root_included{CompareRootFirstWithLegacy(with_root, {root})};
            BOOST_CHECK(root_included.valid);
            CheckSignatureOrder(root_included, {{2, 100}, {1, 2}, {0, 1}});

            Bytes invalid_self_signature{root};
            invalid_self_signature.back() ^= 1;
            const PathVerification root_self_signature{CompareRootFirstWithLegacy(supplied, {invalid_self_signature})};
            BOOST_CHECK(root_self_signature.valid);
            CheckSignatureOrder(root_self_signature, {{2, 100}, {1, 2}, {0, 1}});

            // Exercise the production P2C path as well as the low-level adapter:
            // a valid chain still requires the separate TLS CertificateVerify.
            const uint16_t scheme{static_cast<uint16_t>(pss ? 0x0809 : 0x0804)};
            std::string error;
            BOOST_REQUIRE_MESSAGE(fixture.Verify(supplied[0], scheme, cv_signature, error,
                "localhost", 1800000000, &root, PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL,
                {supplied[1], supplied[2]}), error);
            Bytes invalid_cv{cv_signature};
            invalid_cv.back() ^= 1;
            BOOST_CHECK(!fixture.Verify(supplied[0], scheme, invalid_cv, error,
                "localhost", 1800000000, &root, PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL,
                {supplied[1], supplied[2]}));
            BOOST_CHECK_EQUAL(error, "invalid P2C TLS CertificateVerify signature");
        }
    }
}

BOOST_AUTO_TEST_CASE(root_first_preserves_trusted_and_untrusted_candidate_selection)
{
    RsaFixture fixture;
    const Bytes leaf{fixture.MakeCertificate(false, "CN=localhost", "CN=P2C Selection I1")};
    const Bytes i1{fixture.MakeCertificate(true, "CN=P2C Selection I1", "CN=P2C Selection Root")};
    const Bytes root{fixture.MakeCertificate(true, "CN=P2C Selection Root", "CN=P2C Selection Root", 3)};
    // Import a different public key without generating new RSA primes. Its
    // exponent remains well inside the consensus cap and all DER is valid.
    const Bytes wrong_root{WithRsaExponent(root, Bytes{3}, 2)};
    const PathVerification second_root{CompareRootFirstWithLegacy({leaf, i1}, {wrong_root, root})};
    BOOST_REQUIRE(second_root.valid);
    CheckSignatureOrder(second_root, {{1, 100}, {1, 101}, {0, 1}});
    const PathVerification only_wrong_root{CompareRootFirstWithLegacy({leaf, i1}, {wrong_root})};
    BOOST_CHECK(!only_wrong_root.valid);
    CheckSignatureOrder(only_wrong_root, {{1, 100}});

    // Re-sign the wrong-key intermediate with the real CA key. Its own chain
    // is valid, but it cannot verify the leaf. Legacy Mbed TLS selects the
    // first structurally suitable untrusted intermediate without backtracking.
    const Bytes wrong_i1{fixture.Rewrite(WithRsaExponent(i1, Bytes{3}, 2), std::nullopt)};
    const PathVerification wrong_intermediate_first{CompareRootFirstWithLegacy({leaf, wrong_i1, i1}, {root})};
    BOOST_CHECK(!wrong_intermediate_first.valid);
    CheckSignatureOrder(wrong_intermediate_first, {{1, 100}, {0, 1}});
    const PathVerification good_intermediate_first{CompareRootFirstWithLegacy({leaf, i1, wrong_i1}, {root})};
    BOOST_CHECK(good_intermediate_first.valid);
    CheckSignatureOrder(good_intermediate_first, {{1, 100}, {0, 1}});

    // A matching trusted issuer takes precedence over the supplied chain.
    const PathVerification directly_trusted_parent{CompareRootFirstWithLegacy({leaf, wrong_i1, i1}, {i1, root})};
    BOOST_CHECK(directly_trusted_parent.valid);
    CheckSignatureOrder(directly_trusted_parent, {{0, 100}});
}

BOOST_AUTO_TEST_CASE(root_first_preserves_path_policy_and_locally_trusted_leaf)
{
    RsaFixture fixture;
    const Bytes leaf{fixture.MakeCertificate(false, "CN=localhost", "CN=P2C Policy I1")};
    const Bytes i1{fixture.MakeCertificate(true, "CN=P2C Policy I1", "CN=P2C Policy I2")};
    const Bytes i2{fixture.MakeCertificate(true, "CN=P2C Policy I2", "CN=P2C Policy Root", 2)};
    const Bytes root{fixture.MakeCertificate(true, "CN=P2C Policy Root", "CN=P2C Policy Root", 3)};
    BOOST_REQUIRE(CompareRootFirstWithLegacy({leaf, i1, i2}, {root}).valid);
    BOOST_CHECK(!CompareRootFirstWithLegacy({leaf, i1, i2}, {root}, "other.example").valid);

    mbedtls_x509_crt_profile no_hashes{mbedtls_x509_crt_profile_default};
    no_hashes.allowed_mds = 0;
    BOOST_CHECK(!CompareRootFirstWithLegacy({leaf, i1, i2}, {root}, "localhost", &no_hashes).valid);
    mbedtls_x509_crt_profile stronger_rsa{mbedtls_x509_crt_profile_default};
    stronger_rsa.rsa_min_bitlen = 4096;
    BOOST_CHECK(!CompareRootFirstWithLegacy({leaf, i1, i2}, {root}, "localhost", &stronger_rsa).valid);

    const Bytes constrained_i2{fixture.MakeCertificate(true, "CN=P2C Policy I2", "CN=P2C Policy Root", 0)};
    BOOST_CHECK(!CompareRootFirstWithLegacy({leaf, i1, constrained_i2}, {root}).valid);
    const Bytes non_ca_i1{fixture.MakeCertificate(false, "CN=P2C Policy I1", "CN=P2C Policy I2")};
    BOOST_CHECK(!CompareRootFirstWithLegacy({leaf, non_ca_i1, i2}, {root}).valid);
    const Bytes no_cert_sign_i1{fixture.MakeCertificate(true, "CN=P2C Policy I1", "CN=P2C Policy I2",
        1, MBEDTLS_X509_KU_DIGITAL_SIGNATURE)};
    BOOST_CHECK(!CompareRootFirstWithLegacy({leaf, no_cert_sign_i1, i2}, {root}).valid);

    Bytes locally_trusted_leaf{fixture.MakeCertificate(false, "CN=localhost", "CN=localhost")};
    locally_trusted_leaf.back() ^= 1;
    const PathVerification direct{CompareRootFirstWithLegacy({locally_trusted_leaf}, {locally_trusted_leaf})};
    BOOST_CHECK(direct.valid);
    CheckSignatureOrder(direct, {});
    BOOST_CHECK(!CompareRootFirstWithLegacy({locally_trusted_leaf}, {locally_trusted_leaf}, "other.example").valid);
    BOOST_CHECK(!CompareRootFirstWithLegacy({locally_trusted_leaf}, {locally_trusted_leaf}, "localhost", &no_hashes).valid);
    BOOST_CHECK(!CompareRootFirstWithLegacy({locally_trusted_leaf}, {locally_trusted_leaf}, "localhost", &stronger_rsa).valid);
}

BOOST_AUTO_TEST_CASE(root_first_preserves_self_issued_path_length_accounting)
{
    RsaFixture fixture;
    const Bytes leaf{fixture.MakeCertificate(false, "CN=localhost", "CN=P2C Rollover")};
    const Bytes self_issued{fixture.MakeCertificate(true, "CN=P2C Rollover", "CN=P2C Rollover")};
    const Bytes parent{fixture.MakeCertificate(true, "CN=P2C Rollover", "CN=P2C Rollover Root")};
    const Bytes root{fixture.MakeCertificate(true, "CN=P2C Rollover Root", "CN=P2C Rollover Root", 1)};
    const PathVerification valid{CompareRootFirstWithLegacy({leaf, self_issued, parent}, {root})};
    BOOST_REQUIRE(valid.valid);
    CheckSignatureOrder(valid, {{2, 100}, {1, 2}, {0, 1}});

    // With two non-self-issued intermediates the same root pathLen=1 is too
    // short. Neither the order change nor early exit may relax this boundary.
    const Bytes ordinary_i1{fixture.MakeCertificate(true, "CN=P2C Rollover", "CN=P2C Rollover I2")};
    const Bytes ordinary_i2{fixture.MakeCertificate(true, "CN=P2C Rollover I2", "CN=P2C Rollover Root")};
    BOOST_CHECK(!CompareRootFirstWithLegacy({leaf, ordinary_i1, ordinary_i2}, {root}).valid);
}

BOOST_AUTO_TEST_CASE(root_first_preserves_certificate_chain_capacity_boundaries)
{
    RsaFixture fixture;
    const auto make_chain = [&](size_t count) {
        std::vector<Bytes> supplied;
        for (size_t index{0}; index < count; ++index) {
            const std::string subject{index == 0 ? "CN=localhost" : "CN=P2C Long " + util::ToString(index)};
            const std::string issuer{index + 1 == count ? "CN=P2C Long Root" : "CN=P2C Long " + util::ToString(index + 1)};
            supplied.push_back(fixture.MakeCertificate(index != 0, subject.c_str(), issuer.c_str(), static_cast<int>(count)));
        }
        return supplied;
    };
    const Bytes root{fixture.MakeCertificate(true, "CN=P2C Long Root", "CN=P2C Long Root",
        MBEDTLS_X509_MAX_INTERMEDIATE_CA + 2)};
    const auto supplied{make_chain(8)};
    const PathVerification valid{CompareRootFirstWithLegacy(supplied, {root})};
    BOOST_REQUIRE(valid.valid);
    CheckSignatureOrder(valid, {{7, 100}, {6, 7}, {5, 6}, {4, 5}, {3, 4}, {2, 3}, {1, 2}, {0, 1}});
    const Bytes cv_signature{fixture.PssSign(RsaFixture::SignedHash())};
    std::string error;
    BOOST_REQUIRE_MESSAGE(fixture.Verify(supplied.front(), 0x0804, cv_signature, error,
        "localhost", 1800000000, &root, PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL,
        std::vector<Bytes>(supplied.begin() + 1, supplied.end())), error);

    // These two are direct library tests, not P2C wire proofs: test the
    // adapter's deferred-edge storage at Mbed TLS's own capacity and one above.
    const auto at_limit{make_chain(MBEDTLS_X509_MAX_INTERMEDIATE_CA + 1)};
    const PathVerification maximum{CompareRootFirstWithLegacy(at_limit, {root})};
    BOOST_CHECK(maximum.valid);
    BOOST_CHECK_EQUAL(maximum.signatures.size(), at_limit.size());
    const auto above_limit{make_chain(MBEDTLS_X509_MAX_INTERMEDIATE_CA + 2)};
    BOOST_CHECK(!CompareRootFirstWithLegacy(above_limit, {root}).valid);
}

BOOST_AUTO_TEST_CASE(root_first_differential_reordered_and_repeated_candidates)
{
    RsaFixture fixture;
    const std::vector<Bytes> original{
        fixture.MakeCertificate(false, "CN=localhost", "CN=P2C Differential I1"),
        fixture.MakeCertificate(true, "CN=P2C Differential I1", "CN=P2C Differential I2"),
        fixture.MakeCertificate(true, "CN=P2C Differential I2", "CN=P2C Differential Root", 2),
        fixture.MakeCertificate(true, "CN=P2C Differential Root", "CN=P2C Differential Root", 3),
    };
    const Bytes wrong_root{WithRsaExponent(original[3], Bytes{3}, 2)};
    const std::array<std::vector<Bytes>, 4> root_sets{{
        {original[3]}, {wrong_root}, {wrong_root, original[3]}, {},
    }};
    const std::array<std::vector<size_t>, 8> layouts{{
        {0, 1, 2},
        {0, 2, 1},
        {0, 1, 1, 2},
        {0, 1, 2, 2},
        {0, 1, 2, 3},
        {0, 1, 2, 3, 3},
        {0, 2, 1, 2, 3},
        {0, 3, 2, 1, 3},
    }};

    size_t comparisons{0}, accepted{0};
    // All certificates and signatures are created once. Mutation flips only
    // signature bytes and keeps keys, names, profiles and DER structure intact.
    // In particular, roots are candidates, not an implicit ordered chain.
    for (unsigned invalid_mask{0}; invalid_mask < 8; ++invalid_mask) {
        auto certificates{original};
        for (unsigned index{0}; index < 3; ++index) {
            if (invalid_mask & (1U << index)) certificates[index].back() ^= 1;
        }
        for (size_t layout_index{0}; layout_index < layouts.size(); ++layout_index) {
            std::vector<Bytes> supplied;
            for (const size_t index : layouts[layout_index]) supplied.push_back(certificates[index]);
            for (size_t roots_index{0}; roots_index < root_sets.size(); ++roots_index) {
                BOOST_TEST_CONTEXT("invalid mask=" << invalid_mask << ", layout=" << layout_index << ", roots=" << roots_index) {
                    const PathVerification result{CompareRootFirstWithLegacy(supplied, root_sets[roots_index])};
                    ++comparisons;
                    if (result.valid) ++accepted;
                }
            }
        }
    }
    BOOST_CHECK_EQUAL(comparisons, 256U);
    BOOST_CHECK_GT(accepted, 0U);
    BOOST_CHECK_LT(accepted, comparisons);
}

BOOST_AUTO_TEST_CASE(rsa_public_exponent_limit_is_inclusive_for_rsae_and_pss_leaves)
{
    RsaFixture fixture;
    const Bytes signature{fixture.PssSign(RsaFixture::SignedHash())};
    // A DER INTEGER sign byte is not part of the mathematical exponent size.
    const Bytes maximum{0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}; // 2^64 - 1
    const Bytes excessive{1, 0, 0, 0, 0, 0, 0, 0, 1}; // 2^64 + 1
    for (bool pss : {false, true}) {
        BOOST_TEST_CONTEXT("restricted PSS SPKI=" << pss) {
            const Bytes leaf{pss ? fixture.Rewrite(fixture.leaf, PssAlgorithm(PssParams())) : fixture.leaf};
            const uint16_t scheme{static_cast<uint16_t>(pss ? 0x0809 : 0x0804)};
            std::string error;
            BOOST_REQUIRE_MESSAGE(fixture.Verify(leaf, scheme, signature, error), error);
            const Bytes boundary{WithRsaExponent(leaf, maximum, 64)};
            BOOST_CHECK(!fixture.Verify(boundary, scheme, signature, error));
            // The exponent is allowed, but changing SPKI invalidated the CA
            // signature. Reaching path validation proves the inclusive bound.
            BOOST_CHECK_EQUAL(error, "P2C certificate path or domain validation failed");
            const Bytes rejected{WithRsaExponent(leaf, excessive, 65)};
            BOOST_CHECK(!fixture.Verify(rejected, scheme, signature, error));
            BOOST_CHECK_EQUAL(error, "P2C certificate RSA public exponent exceeds 64 bits");
        }
    }
}

BOOST_AUTO_TEST_CASE(rsa_public_exponent_limit_covers_intermediates_and_unused_certificates)
{
    RsaFixture fixture;
    const Bytes signature{fixture.PssSign(RsaFixture::SignedHash())};
    const Bytes maximum{0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const Bytes excessive{1, 0, 0, 0, 0, 0, 0, 0, 1};
    const Bytes parent_root{fixture.MakeCertificate(true, "CN=P2C RSA Parent Root", "CN=P2C RSA Parent Root")};
    const Bytes intermediate{fixture.MakeCertificate(true, nullptr, "CN=P2C RSA Parent Root")};
    const Bytes unused{fixture.MakeCertificate(true, "CN=P2C Unlinked Certificate")};
    std::string error;
    BOOST_REQUIRE_MESSAGE(fixture.Verify(fixture.leaf, 0x0804, signature, error, "localhost", 1800000000,
        &parent_root, PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL, {intermediate}), error);
    for (bool pss : {false, true}) {
        BOOST_TEST_CONTEXT("restricted PSS SPKI=" << pss) {
            for (bool linked : {false, true}) {
                BOOST_TEST_CONTEXT("intermediate in path=" << linked) {
                    const Bytes original{linked ? intermediate : unused};
                    const Bytes certificate{pss ? fixture.Rewrite(original, PssAlgorithm(PssParams())) : original};
                    const Bytes* root{linked ? &parent_root : nullptr};
                    const Bytes boundary{WithRsaExponent(certificate, maximum, 64)};
                    const bool valid{fixture.Verify(fixture.leaf, 0x0804, signature, error, "localhost", 1800000000,
                        root, PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL, {boundary})};
                    if (linked) {
                        BOOST_CHECK(!valid);
                        BOOST_CHECK_EQUAL(error, "P2C certificate path or domain validation failed");
                    } else {
                        // The path deliberately ignores this certificate. It
                        // must not bypass the all-supplied-certificates rule.
                        BOOST_CHECK_MESSAGE(valid, error);
                    }
                    const Bytes rejected{WithRsaExponent(certificate, excessive, 65)};
                    BOOST_CHECK(!fixture.Verify(fixture.leaf, 0x0804, signature, error, "localhost", 1800000000,
                        root, PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL, {rejected}));
                    BOOST_CHECK_EQUAL(error, "P2C certificate RSA public exponent exceeds 64 bits");
                }
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(rsa_public_exponent_limit_covers_explicit_trust_roots)
{
    RsaFixture fixture;
    const Bytes signature{fixture.PssSign(RsaFixture::SignedHash())};
    const Bytes maximum{0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const Bytes excessive{1, 0, 0, 0, 0, 0, 0, 0, 1};
    for (bool pss : {false, true}) {
        BOOST_TEST_CONTEXT("restricted PSS SPKI=" << pss) {
            const Bytes root{pss ? fixture.Rewrite(fixture.root, PssAlgorithm(PssParams())) : fixture.root};
            const Bytes boundary{WithRsaExponent(root, maximum, 64)};
            std::string error;
            BOOST_CHECK(!fixture.Verify(fixture.leaf, 0x0804, signature, error, "localhost", 1800000000, &boundary));
            BOOST_CHECK_EQUAL(error, "P2C certificate path or domain validation failed");
            const Bytes rejected{WithRsaExponent(root, excessive, 65)};
            BOOST_CHECK(!fixture.Verify(fixture.leaf, 0x0804, signature, error, "localhost", 1800000000, &rejected));
            BOOST_CHECK_EQUAL(error, "P2C certificate RSA public exponent exceeds 64 bits");
        }
    }
}

BOOST_AUTO_TEST_CASE(rsa_public_exponent_limit_checks_unused_entries_in_a_trust_bundle)
{
    RsaFixture fixture;
    const Bytes signature{fixture.PssSign(RsaFixture::SignedHash())};
    const Bytes maximum{0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const Bytes excessive{1, 0, 0, 0, 0, 0, 0, 0, 1};
    const Bytes unused_root{fixture.MakeCertificate(true, "CN=P2C Unused Root", "CN=P2C Unused Root")};
    for (bool pss : {false, true}) {
        BOOST_TEST_CONTEXT("restricted PSS SPKI=" << pss) {
            const Bytes root{pss ? fixture.Rewrite(unused_root, PssAlgorithm(PssParams())) : unused_root};
            std::string error;
            const Bytes boundary{WithRsaExponent(root, maximum, 64)};
            BOOST_REQUIRE_MESSAGE(fixture.Verify(fixture.leaf, 0x0804, signature, error, "localhost", 1800000000,
                nullptr, PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL, {}, {boundary}), error);
            const Bytes rejected{WithRsaExponent(root, excessive, 65)};
            BOOST_CHECK(!fixture.Verify(fixture.leaf, 0x0804, signature, error, "localhost", 1800000000,
                nullptr, PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL, {}, {rejected}));
            BOOST_CHECK_EQUAL(error, "P2C certificate RSA public exponent exceeds 64 bits");
        }
    }
}

BOOST_AUTO_TEST_CASE(rsa_public_exponent_preflight_rejects_dense_values_before_path_validation)
{
    RsaFixture fixture;
    const Bytes signature{fixture.PssSign(RsaFixture::SignedHash())};
    Bytes dense(128, 0xff); // 2^1024 - 1, strictly below the fixture's 2048-bit modulus.
    dense.insert(dense.begin(), 0); // Positive DER INTEGER sign byte.
    for (bool pss : {false, true}) {
        BOOST_TEST_CONTEXT("restricted PSS SPKI=" << pss) {
            const Bytes leaf{pss ? fixture.Rewrite(fixture.leaf, PssAlgorithm(PssParams())) : fixture.leaf};
            const Bytes rejected{WithRsaExponent(leaf, dense, 1024)};
            std::string error;
            // The modified SPKI also makes the CA signature invalid. The
            // specific preflight error proves we do not reach expensive path
            // verification; do not use wall-clock thresholds in this test.
            BOOST_CHECK(!fixture.Verify(rejected, pss ? 0x0809 : 0x0804, signature, error));
            BOOST_CHECK_EQUAL(error, "P2C certificate RSA public exponent exceeds 64 bits");
        }
    }
    BOOST_CHECK(P2CRootStoreAvailable());
}

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
