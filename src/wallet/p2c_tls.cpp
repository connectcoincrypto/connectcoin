// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// Access is confined to this adapter for the pinned Mbed TLS 3.6.7. The RNG
// override identifies the ClientHello buffer by address, never by call order
// or size alone: ephemeral secret keys must ALWAYS get fresh randomness.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <wallet/p2c_tls.h>
#include <wallet/p2c_tls_private.h>

#include <consensus/p2c.h>
#include <netbase.h>
#include <primitives/transaction.h>
#include <random.h>
#include <sync.h>
#include <tinyformat.h>
#include <util/sock.h>
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
namespace {
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
            GetStrongRandBytes(std::span<unsigned char>{output, size});
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
    const std::function<bool()>& cancelled)
{
    if (!IsCanonicalP2CDomain(domain) || !endpoint.IsRoutable() || (!endpoint.IsIPv4() && !endpoint.IsIPv6()) || endpoint.GetPort() != 443 ||
        GetNameProxy() || GetProxy(NET_IPV4) || GetProxy(NET_IPV6)) {
        return util::Error{Untranslated("P2C requires a public direct HTTPS endpoint on port 443")};
    }
    if (cancelled()) return util::Error{Untranslated("P2C cancelled")};
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{10}};
    auto socket{ConnectDirectly(endpoint, /*manual_connection=*/true, std::chrono::milliseconds{1000})};
    if (!socket || !socket->SetNonBlocking()) return util::Error{Untranslated("P2C TCP connection failed")};
    Connection connection{challenge, *socket};
    static constexpr int suites[]{MBEDTLS_TLS1_3_AES_128_GCM_SHA256, MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256, 0};
    static constexpr uint16_t groups[]{29, 23, 0};
    static constexpr uint16_t signatures[]{0x0403, 0x0804, 0x0809, 0};
    {
        LOCK(g_p2c_tls_mutex);
        if (psa_crypto_init() != PSA_SUCCESS) return util::Error{Untranslated("TLS crypto initialization failed")};
        const int result{mbedtls_ssl_config_defaults(&connection.config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)};
        if (result != 0) return util::Error{Untranslated(TlsError(result))};
        mbedtls_ssl_conf_min_tls_version(&connection.config, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_max_tls_version(&connection.config, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_tls13_key_exchange_modes(&connection.config, MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL);
        mbedtls_ssl_conf_ciphersuites(&connection.config, suites);
        mbedtls_ssl_conf_groups(&connection.config, groups);
        mbedtls_ssl_conf_sig_algs(&connection.config, signatures);
        // Capture first, then verify with Core's immutable roots and chain MTP.
        // The platform trust store and wall clock are NOT consensus authorities.
        mbedtls_ssl_conf_authmode(&connection.config, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&connection.config, Connection::Random, &connection);
        int setup{mbedtls_ssl_setup(&connection.ssl, &connection.config)};
        if (setup == 0) setup = mbedtls_ssl_set_hostname(&connection.ssl, domain.c_str());
        if (setup != 0) return util::Error{Untranslated(TlsError(setup))};
        mbedtls_ssl_set_bio(&connection.ssl, &connection, Connection::Send, Connection::Receive, nullptr);
    }
    std::vector<unsigned char> proof{P2C_PROOF_VERSION};
    std::string captured_order;
    while (!cancelled() && std::chrono::steady_clock::now() < deadline) {
        const int state{connection.ssl.state};
        if (state == MBEDTLS_SSL_CLIENT_HELLO && connection.captured_client_hello) {
            return util::Error{Untranslated("TLS HelloRetryRequest is not supported by P2C")};
        }
        int result;
        {
            LOCK(g_p2c_tls_mutex);
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
            return proof;
        }
        if (connection.ssl.state == MBEDTLS_SSL_HANDSHAKE_OVER) break;
    }
    return util::Error{Untranslated(cancelled() ? "P2C cancelled" : "P2C TLS timeout")};
}
} // namespace wallet
