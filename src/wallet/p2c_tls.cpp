// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// Access is confined to this adapter for the pinned Mbed TLS 3.6.7. The RNG
// override identifies the ClientHello buffer by address, never by call order
// or size alone: ephemeral secret keys must ALWAYS get fresh randomness.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <wallet/p2c_tls.h>
#include <wallet/p2c_tls_lifecycle.h>
#include <wallet/p2c_tls_private.h>

#include <consensus/p2c.h>
#include <consensus/p2c_x509.h>
#include <netbase.h>
#include <primitives/transaction.h>
#include <random.h>
#include <sync.h>
#include <tinyformat.h>
#include <util/sock.h>
#include <util/time.h>
#include <util/translation.h>

#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <psa/crypto.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <span>

static_assert(MBEDTLS_VERSION_NUMBER == 0x03060700, "Review P2C transcript capture when upgrading Mbed TLS");

namespace wallet {

P2CRsaProbeLifecycle::Phase::~Phase()
{
    if (!m_lifecycle) return;
    const std::lock_guard lock{m_lifecycle->m_mutex};
    --m_lifecycle->m_active;
    if (m_lifecycle->m_active == 0) m_lifecycle->m_cv.notify_all();
}

P2CRsaProbeLifecycle::Phase P2CRsaProbeLifecycle::TryEnter()
{
    const std::lock_guard lock{m_mutex};
    if (m_stopping.load()) return Phase{nullptr};
    ++m_active;
    return Phase{this};
}

void P2CRsaProbeLifecycle::Stop()
{
    std::unique_lock lock{m_mutex};
    m_stopping.store(true);
    m_cv.wait(lock, [this] { return m_active == 0; });
}

namespace {
P2CRsaProbeLifecycle& ProbeLifecycle()
{
    // Late DNS completions need only this fixed gate, after all destructible
    // networking and RNG state is gone. It contains no keys or wallet state.
    static P2CRsaProbeLifecycle* const lifecycle{new P2CRsaProbeLifecycle};
    return *lifecycle;
}

// The pinned library's PSA keystore is not configured for multithreading.
// Serialize its short CPU operations, not network waits. Consensus X.509
// verification uses the independent classic crypto API, not PSA.
GlobalMutex g_p2c_tls_mutex;

struct Connection {
    mbedtls_ssl_context ssl{};
    mbedtls_ssl_config config{};
    const uint256& challenge;
    const Sock& socket;
    bool random_set{false};
    std::array<unsigned char, 16384 + 5> sent{};
    size_t sent_size{0};
    bool captured_client_hello{false};
    Connection(const uint256& value, const Sock& sock) : challenge(value), socket(sock)
    {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&config);
    }
    ~Connection()
    {
        LOCK(g_p2c_tls_mutex);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&config);
    }
    static int Random(void* context, unsigned char* output, size_t size)
    {
        auto& self{*static_cast<Connection*>(context)};
        if (output == connectcoin_p2c_client_random(&self.ssl) && size == 32) {
            std::copy(self.challenge.begin(), self.challenge.end(), output);
            self.random_set = true;
        } else {
            // Core's strong RNG accepts at most 32 bytes per call; TLS may
            // request a larger buffer for a supported crypto operation.
            while (size != 0) {
                const auto chunk{std::min(size, size_t{32})};
                GetStrongRandBytes(std::span<unsigned char>{output, chunk});
                output += chunk;
                size -= chunk;
            }
        }
        return 0;
    }
    static int Send(void* context, const unsigned char* data, size_t size)
    {
        auto& self{*static_cast<Connection*>(context)};
        const auto result{self.socket.Send(data, size, MSG_NOSIGNAL)};
        if (result > 0 && !self.captured_client_hello) {
            const auto count{static_cast<size_t>(result)};
            if (count > self.sent.size() - self.sent_size) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
            std::copy_n(data, count, self.sent.data() + self.sent_size);
            self.sent_size += count;
        }
        if (result >= 0) return static_cast<int>(result);
        return IOErrorIsPermanent(WSAGetLastError()) ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    static int Receive(void* context, unsigned char* data, size_t size)
    {
        auto& self{*static_cast<Connection*>(context)};
        const auto result{self.socket.Recv(data, size, 0)};
        if (result >= 0) return static_cast<int>(result);
        return IOErrorIsPermanent(WSAGetLastError()) ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : MBEDTLS_ERR_SSL_WANT_READ;
    }
};

std::string TlsError(int result)
{
    char error[256]{};
    mbedtls_strerror(result, error, sizeof(error));
    return std::string{"TLS handshake failed: "} + error;
}
} // namespace

util::Result<std::vector<CService>> ResolveP2CDomain(const std::string& domain)
{
    if (!IsCanonicalP2CDomain(domain)) return util::Error{Untranslated("Invalid P2C domain")};
    if (GetNameProxy() || GetProxy(NET_IPV4) || GetProxy(NET_IPV6)) {
        return util::Error{Untranslated("Automatic P2C currently requires direct HTTPS; a configured proxy will not be bypassed")};
    }
    std::vector<CService> endpoints;
    for (const auto& address : LookupHost(domain, 32, true)) {
        if (address.IsRoutable() && (address.IsIPv4() || address.IsIPv6()) && !address.IsInternal()) {
            endpoints.emplace_back(address, 443);
        }
    }
    if (endpoints.empty()) return util::Error{Untranslated("P2C domain resolved to no public IP addresses")};
    return endpoints;
}

util::Result<std::vector<unsigned char>> CaptureP2CTls(
    const CService& endpoint, const std::string& domain, const uint256& challenge,
    const std::function<bool()>& cancelled, const P2CTlsCaptureOptions& options)
{
    if (!IsValidP2CSignatureAlgorithmsMask(options.signature_algorithms_mask)) {
        return util::Error{Untranslated("Invalid P2C signature algorithms mask")};
    }
    if (cancelled() || std::chrono::steady_clock::now() >= options.deadline) {
        return util::Error{Untranslated("P2C cancelled or deadline expired")};
    }
    if (!IsCanonicalP2CDomain(domain) || !endpoint.IsRoutable() || (!endpoint.IsIPv4() && !endpoint.IsIPv6()) || endpoint.GetPort() != 443 ||
        GetNameProxy() || GetProxy(NET_IPV4) || GetProxy(NET_IPV6)) {
        return util::Error{Untranslated("P2C requires a public direct HTTPS endpoint on port 443")};
    }
    if (cancelled()) return util::Error{Untranslated("P2C cancelled")};
    const auto deadline{std::min(options.deadline, std::chrono::steady_clock::now() + std::chrono::seconds{10})};
    const auto connect_timeout{std::min(std::chrono::milliseconds{1000},
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()))};
    if (connect_timeout <= std::chrono::milliseconds::zero()) return util::Error{Untranslated("P2C TLS timeout")};
    auto socket{ConnectDirectly(endpoint, /*manual_connection=*/true, connect_timeout)};
    if (!socket || !socket->SetNonBlocking()) return util::Error{Untranslated("P2C TCP connection failed")};
    if (cancelled() || std::chrono::steady_clock::now() >= deadline) return util::Error{Untranslated("P2C cancelled or deadline expired")};
    static constexpr int suites[]{MBEDTLS_TLS1_3_AES_128_GCM_SHA256, MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256, 0};
    static constexpr uint16_t groups[]{29, 23, 0};
    std::vector<uint16_t> signatures;
    if (options.signature_algorithms_mask & PayToDomainOutput::SIGNATURE_ALGORITHM_ECDSA_P256_SHA256) signatures.push_back(0x0403);
    if (options.signature_algorithms_mask & PayToDomainOutput::SIGNATURE_ALGORITHM_RSA_PSS_RSAE_SHA256) signatures.push_back(0x0804);
    if (options.signature_algorithms_mask & PayToDomainOutput::SIGNATURE_ALGORITHM_RSA_PSS_PSS_SHA256) signatures.push_back(0x0809);
    signatures.push_back(0);
    Connection connection{challenge, *socket};
    {
        LOCK(g_p2c_tls_mutex);
        if (cancelled() || std::chrono::steady_clock::now() >= deadline) return util::Error{Untranslated("P2C cancelled or deadline expired")};
        if (psa_crypto_init() != PSA_SUCCESS) return util::Error{Untranslated("TLS crypto initialization failed")};
        const int result{mbedtls_ssl_config_defaults(&connection.config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)};
        if (result != 0) return util::Error{Untranslated(TlsError(result))};
        mbedtls_ssl_conf_min_tls_version(&connection.config, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_max_tls_version(&connection.config, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_tls13_key_exchange_modes(&connection.config, MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL);
        mbedtls_ssl_conf_ciphersuites(&connection.config, suites);
        mbedtls_ssl_conf_groups(&connection.config, groups);
        mbedtls_ssl_conf_sig_algs(&connection.config, signatures.data());
        // Capture first, then verify with Core's immutable roots and chain MTP.
        // The platform trust store and wall clock are NOT consensus authorities.
        mbedtls_ssl_conf_authmode(&connection.config, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&connection.config, Connection::Random, &connection);
        int setup{mbedtls_ssl_setup(&connection.ssl, &connection.config)};
        if (setup == 0) setup = mbedtls_ssl_set_hostname(&connection.ssl, domain.c_str());
        if (setup != 0) return util::Error{Untranslated(TlsError(setup))};
        mbedtls_ssl_set_bio(&connection.ssl, &connection, Connection::Send, Connection::Receive, nullptr);
    }
    // Keep CertificateVerify in the v2 proof even though its entire message is
    // excluded from connection work. The captured transcript must still be
    // authenticated by Core's certificate/signature verification before use.
    std::vector<unsigned char> proof{P2C_PROOF_VERSION};
    std::string captured_order;
    bool certificate_verified{false};
    while (!cancelled() && std::chrono::steady_clock::now() < deadline) {
        const int state{connection.ssl.state};
        if (state == MBEDTLS_SSL_CLIENT_HELLO && connection.captured_client_hello) {
            return util::Error{Untranslated("TLS HelloRetryRequest is not supported by P2C")};
        }
        int result;
        {
            LOCK(g_p2c_tls_mutex);
            if (cancelled() || std::chrono::steady_clock::now() >= deadline) return util::Error{Untranslated("P2C cancelled or deadline expired")};
            result = mbedtls_ssl_handshake_step(&connection.ssl);
        }
        // Use the transmitted record, including partial writes. Mbed TLS
        // clears its outgoing handshake header after flushing the record.
        if (!connection.captured_client_hello && connection.sent_size >= 5) {
            const size_t length{size_t{connection.sent[3]} * 256 + connection.sent[4]};
            if (connection.sent[0] != 22 || length < 4 || length > connection.sent.size() - 5) {
                return util::Error{Untranslated("Invalid outgoing TLS ClientHello record")};
            }
            if (connection.sent_size >= length + 5) {
                if (!connection.random_set || connection.sent[5] != 1) return util::Error{Untranslated("Unsupported ClientHello random source")};
                proof.insert(proof.end(), connection.sent.begin() + 5, connection.sent.begin() + 5 + length);
                connection.captured_client_hello = true;
            }
        }
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (!socket->Wait(std::chrono::milliseconds{100}, result == MBEDTLS_ERR_SSL_WANT_READ ? Sock::RecvEvent : Sock::SendEvent)) break;
            continue;
        }
        if (result != 0) return util::Error{Untranslated(TlsError(result))};
        const unsigned char* message{nullptr};
        size_t length{0};
        if (state == MBEDTLS_SSL_SERVER_HELLO || state == MBEDTLS_SSL_ENCRYPTED_EXTENSIONS ||
                   state == MBEDTLS_SSL_SERVER_CERTIFICATE || state == MBEDTLS_SSL_CERTIFICATE_VERIFY) {
            message = connection.ssl.in_msg;
            length = connection.ssl.in_hslen;
        }
        if (message) {
            if (length < 4 || length > MAX_P2C_PROOF_SIZE - proof.size()) return util::Error{Untranslated("TLS proof exceeds capture limit")};
            proof.insert(proof.end(), message, message + length);
            captured_order += strprintf(" %d:%u/%u", state, message[0], length);
        }
        if (state == MBEDTLS_SSL_CERTIFICATE_VERIFY) {
            P2CTlsProofView view;
            std::string error;
            if (!ParseP2CTlsProof(proof, domain, challenge, view, error)) return util::Error{Untranslated(error + "; capture" + captured_order)};
            if (!P2CSignatureSchemeAllowed(options.signature_algorithms_mask, view.certificate_verify_scheme)) {
                return util::Error{Untranslated("TLS server used a disallowed P2C signature scheme")};
            }
            certificate_verified = true;
            if (!options.complete_handshake) return proof;
        }
        if (connection.ssl.state == MBEDTLS_SSL_HANDSHAKE_OVER) {
            if (certificate_verified && !cancelled() && std::chrono::steady_clock::now() < deadline) return proof;
            break;
        }
    }
    return util::Error{Untranslated(cancelled() ? "P2C cancelled" : "P2C TLS timeout")};
}

bool ProbeP2CRsaForTest(const std::string& domain, uint32_t roots_version,
                       const std::function<bool()>& cancelled,
                       std::chrono::steady_clock::time_point deadline,
                       P2CRsaProbeLifecycle& lifecycle)
{
    const auto expired = [&] {
        return lifecycle.IsStopping() || cancelled() || std::chrono::steady_clock::now() >= deadline;
    };
    if (expired() || !IsCanonicalP2CDomain(domain) || !IsSupportedP2CRootCertificatesVersion(roots_version)) return false;
    DNSLookupFn dns_lookup;
    {
        const auto phase{lifecycle.TryEnter()};
        if (!phase || expired()) return false;
        if (GetNameProxy() || GetProxy(NET_IPV4) || GetProxy(NET_IPV6)) return false;
        dns_lookup = g_dns_lookup;
    }
    if (expired()) return false;
    // LookupHost with an explicit copied resolver uses only local values and
    // getaddrinfo. Shutdown never waits for this potentially blocking phase.
    const auto addresses{LookupHost(domain, 32, true, std::move(dns_lookup))};
    const auto phase{lifecycle.TryEnter()};
    if (!phase || expired()) return false;
    std::vector<CService> endpoints;
    for (const auto& address : addresses) {
        if (address.IsRoutable() && (address.IsIPv4() || address.IsIPv6()) && !address.IsInternal()) {
            endpoints.emplace_back(address, 443);
        }
    }
    if (endpoints.empty()) return false;
    // Retain the phase through all RNG/TLS/X509 calls and their destructors.
    // Stop() cancels this work and waits for it before process-state teardown.
    const auto challenge{GetRandHash()};
    const P2CTlsCaptureOptions options{
        .signature_algorithms_mask = PayToDomainOutput::SIGNATURE_ALGORITHMS_RSA,
        .deadline = deadline,
        .complete_handshake = true,
    };
    const auto proof{CaptureP2CTls(endpoints.front(), domain, challenge, expired, options)};
    if (expired() || !proof) return false;
    P2CTlsProofView view;
    std::string error;
    if (!ParseP2CTlsProof(*proof, domain, challenge, view, error)) return false;
    const CTxOut requirement{0, PayToDomainOutput{domain, uint256{}, roots_version,
        PayToDomainOutput::SIGNATURE_ALGORITHMS_RSA}};
    return VerifyP2CCertificateProof(requirement, view, GetTime(), error) && !expired();
}

bool ProbeP2CRsa(const std::string& domain, uint32_t roots_version,
                 const std::function<bool()>& cancelled,
                 std::chrono::steady_clock::time_point deadline)
{
    return ProbeP2CRsaForTest(domain, roots_version, cancelled, deadline, ProbeLifecycle());
}

void ShutdownP2CRsaProbes()
{
    ProbeLifecycle().Stop();
}
} // namespace wallet
