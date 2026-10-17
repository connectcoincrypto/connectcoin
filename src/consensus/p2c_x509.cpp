// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/p2c_x509.h>

#include <consensus/p2c.h>
#include <consensus/p2c_roots_v1.pem.h>
#include <crypto/mbedtls_rsa_pss.h>
#include <crypto/mbedtls_threading.h>
#include <crypto/mbedtls_x509_root_first.h>
#include <crypto/sha256.h>
#include <mbedtls/asn1.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(MBEDTLS_HAVE_TIME) || defined(MBEDTLS_HAVE_TIME_DATE)
#error "P2C consensus validation must not depend on the node's local clock"
#endif

namespace {

constexpr uint16_t TLS_ECDSA_SECP256R1_SHA256{0x0403};
constexpr uint16_t TLS_RSA_PSS_RSAE_SHA256{0x0804};
constexpr uint16_t TLS_RSA_PSS_PSS_SHA256{0x0809};
constexpr std::string_view TLS13_SERVER_CERTIFICATE_VERIFY_CONTEXT{"TLS 1.3, server CertificateVerify"};

class CertificateChain
{
public:
    mbedtls_x509_crt value;

    CertificateChain() { mbedtls_x509_crt_init(&value); }
    ~CertificateChain() { mbedtls_x509_crt_free(&value); }
    CertificateChain(const CertificateChain&) = delete;
    CertificateChain& operator=(const CertificateChain&) = delete;
};

bool SetError(std::string& error, std::string message)
{
    error = std::move(message);
    return false;
}

const mbedtls_x509_crt* RootStoreV1()
{
    struct RootStore {
        CertificateChain chain;
        bool valid{false};

        RootStore()
        {
            std::vector<unsigned char> pem;
            pem.reserve(consensus::p2c::data::p2c_roots_v1.size() + 1);
            for (const std::byte value : consensus::p2c::data::p2c_roots_v1) {
                pem.push_back(std::to_integer<unsigned char>(value));
            }
            pem.push_back(0);
            std::string error;
            valid = mbedtls_x509_crt_parse(&chain.value, pem.data(), pem.size()) == 0 &&
                    CheckP2CCertificatePublicKeys(chain.value, error);
        }
    };
    if (!InitializeP2CCrypto()) return nullptr;
    // Public RSA/EC keys lazily acquire arithmetic state. Own the parsed roots
    // per worker instead of serializing every proof on a process-wide mutex.
    // Thread-local lifetime also covers late detached GUI probes safely.
    thread_local const RootStore roots;
    return roots.valid ? &roots.chain.value : nullptr;
}

bool ParseCertificateChain(const P2CTlsProofView& proof, CertificateChain& chain, std::string& error)
{
    if (proof.certificate_chain.empty()) return SetError(error, "P2C proof contains no leaf certificate");
    for (const auto certificate : proof.certificate_chain) {
        if (mbedtls_x509_crt_parse_der(&chain.value, certificate.data(), certificate.size()) != 0) {
            return SetError(error, "P2C proof contains an invalid DER certificate");
        }
    }
    return true;
}

bool IsLeapYear(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(int year, int month)
{
    constexpr std::array<int, 12> DAYS{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    return DAYS[month - 1] + (month == 2 && IsLeapYear(year) ? 1 : 0);
}

/** Days since 1970-01-01, valid for the Gregorian calendar. */
constexpr int64_t DaysFromCivil(int year, int month, int day)
{
    const int64_t adjusted_year{static_cast<int64_t>(year) - (month <= 2 ? 1 : 0)};
    const int64_t era{(adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400};
    const int64_t year_of_era{adjusted_year - era * 400};
    const int64_t adjusted_month{month > 2 ? month - 3 : month + 9};
    const int64_t day_of_year{(153 * adjusted_month + 2) / 5 + day - 1};
    const int64_t day_of_era{year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year};
    return era * 146097 + day_of_era - 719468;
}

static_assert(DaysFromCivil(1970, 1, 1) == 0);
static_assert(DaysFromCivil(2000, 3, 1) - DaysFromCivil(2000, 2, 28) == 2);
static_assert(DaysFromCivil(2100, 3, 1) - DaysFromCivil(2100, 2, 28) == 1);

bool X509TimeToUnix(const mbedtls_x509_time& time, int64_t& result)
{
    if (time.year < 1970 || time.mon < 1 || time.mon > 12 || time.day < 1 ||
        time.day > DaysInMonth(time.year, time.mon) || time.hour < 0 || time.hour > 23 ||
        time.min < 0 || time.min > 59 || time.sec < 0 || time.sec > 59) return false;
    const int64_t days{DaysFromCivil(time.year, time.mon, time.day)};
    if (days > (std::numeric_limits<int64_t>::max() - 86399) / 86400) return false;
    result = days * 86400 + int64_t{time.hour} * 3600 + int64_t{time.min} * 60 + time.sec;
    return true;
}

bool CheckLeafUsage(const mbedtls_x509_crt& leaf, std::string& error)
{
    if (mbedtls_x509_crt_check_key_usage(&leaf, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0 ||
        mbedtls_x509_crt_check_extended_key_usage(
            &leaf, MBEDTLS_OID_SERVER_AUTH, MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH)) != 0) {
        return SetError(error, "P2C leaf certificate is not authorized for TLS server signatures");
    }
    return true;
}

struct CachedCertificateChain {
    struct Validity { int64_t from{0}, to{0}; };
    const std::unique_ptr<CertificateChain> parsed;
    std::vector<Validity> times;
    bool dates_valid{true}, leaf_usage_valid{false};

    explicit CachedCertificateChain(std::unique_ptr<CertificateChain> chain) : parsed{std::move(chain)}
    {
        std::string ignored;
        leaf_usage_valid = CheckLeafUsage(parsed->value, ignored);
        for (const auto* certificate = &parsed->value; certificate; certificate = certificate->next) {
            Validity bounds;
            const bool from_ok = X509TimeToUnix(certificate->valid_from, bounds.from);
            const bool to_ok = X509TimeToUnix(certificate->valid_to, bounds.to);
            dates_valid = dates_valid && from_ok && to_ok;
            times.push_back(bounds);
        }
    }
};

struct CertificateCache {
    struct Entry {
        std::shared_ptr<const CachedCertificateChain> parsed;
        size_t bytes{0}, estimated_bytes{0};
    };
    static constexpr size_t MAX_ENTRIES{4}, MAX_BYTES{65536}, MAX_ESTIMATED_BYTES{262144};
    static_assert(MBEDTLS_ECP_WINDOW_SIZE <= 7 && MBEDTLS_ECP_MAX_BITS <= 521,
        "Review certificate cache allocation reserve when EC limits change");
    std::array<Entry, MAX_ENTRIES> entries;
    size_t count{0}, bytes{0}, estimated_bytes{0};
    uint64_t hits{0}, misses{0};

    std::shared_ptr<const CachedCertificateChain> Find(const P2CTlsProofView& proof)
    {
        for (size_t index = 0; index < count; ++index) {
            const auto* certificate = &entries[index].parsed->parsed->value;
            bool match{true};
            for (const auto der : proof.certificate_chain) {
                if (!certificate || !std::ranges::equal(der, std::span{certificate->raw.p, certificate->raw.len})) {
                    match = false;
                    break;
                }
                certificate = certificate->next;
            }
            // Exact ordered DER, including every supplied (even unused) entry.
            // The parsed chain owns its own copies; input addresses are no key.
            if (!match || certificate) continue;
            Entry hit{std::move(entries[index])};
            for (size_t next = index + 1; next < count; ++next) entries[next - 1] = std::move(entries[next]);
            entries[count - 1] = std::move(hit);
            ++hits;
            return entries[count - 1].parsed;
        }
        ++misses;
        return {};
    }

    void Remember(const std::shared_ptr<const CachedCertificateChain>& parsed)
    {
        size_t der_bytes{0}, structures{0};
        const auto nodes = [](const auto* node) {
            size_t size{0};
            for (; node; node = node->next) size += sizeof(*node) + 64;
            return size;
        };
        for (const auto* cert = &parsed->parsed->value; cert; cert = cert->next) {
            der_bytes += cert->raw.len;
            structures += sizeof(*cert) + 64 + nodes(&cert->issuer) + nodes(&cert->subject) +
                nodes(&cert->subject_alt_names) + nodes(&cert->ext_key_usage) +
                nodes(&cert->certificate_policies) + nodes(&cert->authority_key_id.authorityCertIssuer);
            // Conservative retention charge, not allocator RSS. Covers future
            // RSA/EC public-key arithmetic caches for the pinned provider.
            structures += 32768 + 32 * mbedtls_pk_get_len(&cert->pk);
        }
        Entry candidate{parsed, der_bytes, der_bytes + structures + sizeof(Entry) +
            sizeof(CachedCertificateChain) + sizeof(CertificateChain) + 128 +
            parsed->times.capacity() * sizeof(CachedCertificateChain::Validity)};
        if (candidate.bytes > MAX_BYTES || candidate.estimated_bytes > MAX_ESTIMATED_BYTES) return;
        // Parsing, public-key checks and allocations complete before eviction.
        while (count && (count == MAX_ENTRIES || bytes + candidate.bytes > MAX_BYTES ||
                        estimated_bytes + candidate.estimated_bytes > MAX_ESTIMATED_BYTES)) {
            bytes -= entries[0].bytes;
            estimated_bytes -= entries[0].estimated_bytes;
            for (size_t next = 1; next < count; ++next) entries[next - 1] = std::move(entries[next]);
            entries[--count] = Entry{};
        }
        bytes += candidate.bytes;
        estimated_bytes += candidate.estimated_bytes;
        entries[count++] = std::move(candidate);
    }
};

CertificateCache& Certificates()
{
    thread_local CertificateCache cache;
    return cache;
}

std::shared_ptr<const CachedCertificateChain> CachedChain(const P2CTlsProofView& proof, std::string& error)
{
    if (!InitializeP2CCrypto()) {
        SetError(error, "TLS crypto initialization failed");
        return {};
    }
    auto& cache{Certificates()};
    if (auto found = cache.Find(proof)) return found;
    auto chain{std::make_unique<CertificateChain>()};
    if (!ParseCertificateChain(proof, *chain, error) || !CheckP2CCertificatePublicKeys(chain->value, error)) return {};
    auto parsed{std::make_shared<const CachedCertificateChain>(std::move(chain))};
    cache.Remember(parsed);
    return parsed;
}

class SignatureCache {
    struct Entry { std::vector<unsigned char> child, parent; };
    static constexpr size_t MAX_ENTRIES{8}, MAX_BYTES{65536};
    std::array<Entry, MAX_ENTRIES> m_entries;
    size_t m_count{0}, m_bytes{0};
public:
    uint64_t hits{0}, misses{0};
    size_t Count() const { return m_count; }
    size_t Bytes() const { return m_bytes; }
    bool Contains(const mbedtls_x509_crt& child, const mbedtls_x509_crt& parent) noexcept
    {
        const auto end{m_entries.begin() + m_count};
        const bool found{std::any_of(m_entries.begin(), end, [&](const Entry& entry) {
            return std::ranges::equal(entry.child, std::span{child.raw.p, child.raw.len}) &&
                   std::ranges::equal(entry.parent, std::span{parent.raw.p, parent.raw.len});
        })};
        if (found) ++hits; else ++misses;
        return found;
    }
    void Remember(const mbedtls_x509_crt& child, const mbedtls_x509_crt& parent) noexcept
    {
        if (child.raw.len == 0 || parent.raw.len == 0 || child.raw.len > MAX_P2C_CERTIFICATE_SIZE ||
            parent.raw.len > MAX_P2C_CERTIFICATE_SIZE || child.raw.len + parent.raw.len > MAX_BYTES) return;
        try {
            Entry candidate{{child.raw.p, child.raw.p + child.raw.len}, {parent.raw.p, parent.raw.p + parent.raw.len}};
            const size_t bytes{candidate.child.capacity() + candidate.parent.capacity()};
            if (bytes > MAX_BYTES) return;
            while (m_count && (m_count == MAX_ENTRIES || m_bytes + bytes > MAX_BYTES)) {
                m_bytes -= m_entries[0].child.capacity() + m_entries[0].parent.capacity();
                for (size_t next = 1; next < m_count; ++next) m_entries[next - 1] = std::move(m_entries[next]);
                m_entries[--m_count] = Entry{};
            }
            m_entries[m_count++] = std::move(candidate);
            m_bytes += bytes;
        } catch (...) { /* Memoization failure never changes verification. */ }
    }
};

SignatureCache& Signatures()
{
    thread_local SignatureCache cache;
    return cache;
}
int SignatureLookup(void*, const mbedtls_x509_crt* child, const mbedtls_x509_crt* parent) noexcept
{
    try { return Signatures().Contains(*child, *parent) ? 1 : 0; }
    catch (...) { return 0; }
}
void SignatureRemember(void*, const mbedtls_x509_crt* child, const mbedtls_x509_crt* parent) noexcept
{
    try { Signatures().Remember(*child, *parent); }
    catch (...) { /* Exceptions never cross the provider callback boundary. */ }
}

bool CheckSuppliedCertificateTimes(const CachedCertificateChain& chain, int64_t validation_time, std::string& error)
{
    if (validation_time <= 0) return SetError(error, "P2C certificate validation time is unavailable");
    if (!chain.dates_valid) return SetError(error, "P2C certificate is not valid at block median time");
    for (const auto& bounds : chain.times) {
        if (validation_time < bounds.from || validation_time > bounds.to)
            return SetError(error, "P2C certificate is not valid at block median time");
    }
    return true;
}

bool VerifyDomainPath(mbedtls_x509_crt& chain, std::string_view domain,
                      const mbedtls_x509_crt& roots, std::string& error)
{
    const std::string domain_string{domain};
    uint32_t flags{0};
    const connectcoin_mbedtls_x509_signature_cache cache{nullptr, SignatureLookup, SignatureRemember};
    const int result{connectcoin_mbedtls_x509_crt_verify_root_first_cached(
        &chain, const_cast<mbedtls_x509_crt*>(&roots), &mbedtls_x509_crt_profile_default,
        domain_string.c_str(), &flags, nullptr, nullptr, &cache)};
    if (result != 0 || flags != 0) return SetError(error, "P2C certificate path or domain validation failed");
    return true;
}

bool VerifyCertificateVerify(const mbedtls_x509_crt& leaf, const P2CTlsProofView& proof, std::string& error)
{
    std::array<unsigned char, 64> prefix;
    prefix.fill(0x20);
    uint256 signed_hash;
    CSHA256 digest;
    digest.Write(prefix.data(), prefix.size());
    digest.Write(reinterpret_cast<const unsigned char*>(TLS13_SERVER_CERTIFICATE_VERIFY_CONTEXT.data()),
                 TLS13_SERVER_CERTIFICATE_VERIFY_CONTEXT.size());
    const unsigned char separator{0};
    digest.Write(&separator, 1);
    digest.Write(proof.transcript_hash.begin(), proof.transcript_hash.size());
    digest.Finalize(signed_hash.begin());

    int result{-1};
    if (proof.certificate_verify_scheme == TLS_ECDSA_SECP256R1_SHA256) {
        mbedtls_ecp_keypair* ec{mbedtls_pk_ec(leaf.pk)};
        if (!ec || mbedtls_ecp_keypair_get_group_id(ec) != MBEDTLS_ECP_DP_SECP256R1) {
            return SetError(error, "CertificateVerify scheme does not match leaf EC key");
        }
        result = mbedtls_pk_verify_ext(MBEDTLS_PK_ECDSA, /*options=*/nullptr,
                                       const_cast<mbedtls_pk_context*>(&leaf.pk), MBEDTLS_MD_SHA256,
                                       signed_hash.begin(), signed_hash.size(),
                                       proof.certificate_verify_signature.data(), proof.certificate_verify_signature.size());
    } else if (proof.certificate_verify_scheme == TLS_RSA_PSS_RSAE_SHA256 ||
               proof.certificate_verify_scheme == TLS_RSA_PSS_PSS_SHA256) {
        if (!connectcoin_mbedtls_rsa_tls_scheme_matches(&leaf.pk, proof.certificate_verify_scheme) ||
            mbedtls_pk_get_bitlen(&leaf.pk) < 2048) {
            return SetError(error, "CertificateVerify scheme does not match leaf RSA key");
        }
        const mbedtls_pk_rsassa_pss_options options{
            .mgf1_hash_id = MBEDTLS_MD_SHA256,
            .expected_salt_len = 32,
        };
        result = mbedtls_pk_verify_ext(MBEDTLS_PK_RSASSA_PSS, &options,
                                       const_cast<mbedtls_pk_context*>(&leaf.pk), MBEDTLS_MD_SHA256,
                                       signed_hash.begin(), signed_hash.size(),
                                       proof.certificate_verify_signature.data(), proof.certificate_verify_signature.size());
    }
    if (result != 0) return SetError(error, "invalid P2C TLS CertificateVerify signature");
    return true;
}

bool VerifyP2CCertificateProofImpl(const CTxOut& spent_output,
                                   const P2CTlsProofView& proof,
                                   int64_t validation_time,
                                   const mbedtls_x509_crt& roots,
                                   std::string& error)
{
    error.clear();
    const auto chain{CachedChain(proof, error)};
    if (!chain || !CheckSuppliedCertificateTimes(*chain, validation_time, error)) return false;
    if (!chain->leaf_usage_valid) return SetError(error, "P2C leaf certificate is not authorized for TLS server signatures");

    const auto output{spent_output.GetPayToDomain()};
    if (!output) return SetError(error, "spent output is not a canonical PAY_TO_CONNECT output");
    if (!P2CSignatureSchemeAllowed(output->signature_algorithms_mask, proof.certificate_verify_scheme)) {
        return SetError(error, "P2C CertificateVerify scheme is not allowed by the output");
    }
    if (!VerifyDomainPath(chain->parsed->value, output->domain, roots, error)) return false;
    return VerifyCertificateVerify(chain->parsed->value, proof, error);
}

} // namespace

bool CheckP2CCertificatePublicKeys(const mbedtls_x509_crt& chain, std::string& error)
{
    if (!InitializeP2CCrypto()) return SetError(error, "TLS crypto initialization failed");
    // Check ALL supplied certificates, including unused path candidates, before
    // Mbed TLS can perform any expensive signature verification. The pinned
    // adapter represents both rsaEncryption and restricted RSA-PSS keys as RSA;
    // pk_can_do(RSA) would incorrectly skip restricted RSA-PSS public keys.
    for (const mbedtls_x509_crt* certificate{&chain}; certificate != nullptr; certificate = certificate->next) {
        if (mbedtls_pk_get_type(&certificate->pk) != MBEDTLS_PK_RSA) continue;
        const mbedtls_rsa_context* rsa{mbedtls_pk_rsa(certificate->pk)};
        if (!rsa) return SetError(error, "P2C certificate RSA public key is unavailable");
        mbedtls_mpi exponent;
        mbedtls_mpi_init(&exponent);
        const int result{mbedtls_rsa_export(rsa, nullptr, nullptr, nullptr, nullptr, &exponent)};
        const size_t bits{mbedtls_mpi_bitlen(&exponent)};
        mbedtls_mpi_free(&exponent);
        if (result != 0) return SetError(error, "failed to read P2C certificate RSA public exponent");
        if (bits > MAX_P2C_RSA_PUBLIC_EXPONENT_BITS) {
            return SetError(error, "P2C certificate RSA public exponent exceeds 64 bits");
        }
    }
    return true;
}

bool P2CRootStoreAvailable()
{
    return RootStoreV1() != nullptr;
}

bool ValidateP2CCertificateMessage(std::span<const unsigned char> message, std::string& error)
{
    P2CTlsProofView parsed;
    return ParseP2CCertificateMessage(message, parsed, error) && bool(CachedChain(parsed, error));
}

P2CX509CacheStats P2CX509CacheStatsForTest()
{
    const auto& certificates{Certificates()};
    const auto& signatures{Signatures()};
    return {certificates.hits, certificates.misses, certificates.count, certificates.bytes, certificates.estimated_bytes,
            signatures.hits, signatures.misses, signatures.Count(), signatures.Bytes()};
}

void ClearP2CX509CachesForTest()
{
    Certificates() = CertificateCache{};
    Signatures() = SignatureCache{};
}

bool VerifyP2CCertificateProof(const CTxOut& spent_output,
                               const P2CTlsProofView& proof,
                               int64_t validation_time,
                               std::string& error)
{
    const auto output{spent_output.GetPayToDomain()};
    if (!output || !IsSupportedP2CRootCertificatesVersion(output->root_certificates_version)) {
        return SetError(error, "spent output is not a canonical PAY_TO_CONNECT output");
    }
    const mbedtls_x509_crt* roots{RootStoreV1()};
    if (!roots) return SetError(error, "failed to initialize immutable P2C root store");
    return VerifyP2CCertificateProofImpl(spent_output, proof, validation_time, *roots, error);
}

bool VerifyP2CCertificateProofForTest(const CTxOut& spent_output,
                                      const P2CTlsProofView& proof,
                                      int64_t validation_time,
                                      std::span<const unsigned char> trusted_roots_pem,
                                      std::string& error)
{
    if (!InitializeP2CCrypto()) return SetError(error, "TLS crypto initialization failed");
    CertificateChain roots;
    std::vector<unsigned char> nul_terminated{trusted_roots_pem.begin(), trusted_roots_pem.end()};
    nul_terminated.push_back(0);
    if (mbedtls_x509_crt_parse(&roots.value, nul_terminated.data(), nul_terminated.size()) != 0) {
        return SetError(error, "invalid P2C test root store");
    }
    if (!CheckP2CCertificatePublicKeys(roots.value, error)) return false;
    return VerifyP2CCertificateProofImpl(spent_output, proof, validation_time, roots.value, error);
}
