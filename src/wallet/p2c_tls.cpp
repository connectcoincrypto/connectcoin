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
#include <wallet/p2c_tls_test.h>

#include <consensus/p2c.h>
#include <consensus/p2c_x509.h>
#include <crypto/mbedtls_threading.h>
#include <netbase.h>
#include <primitives/transaction.h>
#include <random.h>
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
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

static_assert(MBEDTLS_VERSION_NUMBER == 0x03060700, "Review P2C transcript capture when upgrading Mbed TLS");

namespace wallet {

struct P2CCapturedProof::Storage {
    const std::vector<unsigned char> bytes;
    const std::string domain;
    const uint256 challenge;
    P2CTlsProofView view;
    Storage(std::vector<unsigned char> value, std::string name, const uint256& random)
        : bytes{std::move(value)}, domain{std::move(name)}, challenge{random} {}
};

P2CCapturedProof::P2CCapturedProof(std::shared_ptr<const Storage> storage) : m_storage{std::move(storage)} {}

const std::vector<unsigned char>& P2CCapturedProof::Bytes() const
{
    if (!m_storage) throw std::logic_error("Moved-from P2C captured proof");
    return m_storage->bytes;
}

const P2CTlsProofView& P2CCapturedProof::View() const
{
    if (!m_storage) throw std::logic_error("Moved-from P2C captured proof");
    return m_storage->view;
}

bool P2CCapturedProof::MatchesCaptureContext(const std::string& domain, const uint256& challenge) const
{
    return m_storage && m_storage->domain == domain && m_storage->challenge == challenge;
}

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

std::string TlsError(int result);

// Only allocations/configuration survive between captures on one worker. The
// public full-session reset destroys peer/session state, traffic keys and
// ephemeral secrets before the context becomes idle; it does NOT resume TLS.
struct WorkerTls {
    mbedtls_ssl_context ssl{};
    mbedtls_ssl_config config{};
    std::array<uint16_t, 4> signatures{};
    bool ready{false}, in_use{false}, fail_next_reset{false};
    size_t setups{0}, resets{0}, discarded{0};

    WorkerTls()
    {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&config);
        const int result{mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)};
        if (result != 0) {
            mbedtls_ssl_config_free(&config);
            throw std::runtime_error(TlsError(result));
        }
        static constexpr int suites[]{MBEDTLS_TLS1_3_AES_128_GCM_SHA256, MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256, 0};
        static constexpr uint16_t groups[]{29, 23, 0};
        mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_max_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_tls13_key_exchange_modes(&config, MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL);
        mbedtls_ssl_conf_ciphersuites(&config, suites);
        mbedtls_ssl_conf_groups(&config, groups);
        mbedtls_ssl_conf_sig_algs(&config, signatures.data());
        // Capture first; trust, time and signature verification use Core's
        // immutable roots and chain MTP, never the platform store/wall clock.
        mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_NONE);
    }
    ~WorkerTls()
    {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&config);
    }
    WorkerTls(const WorkerTls&) = delete;
    WorkerTls& operator=(const WorkerTls&) = delete;

    void Release() noexcept
    {
        mbedtls_ssl_set_bio(&ssl, nullptr, nullptr, nullptr, nullptr);
        mbedtls_ssl_conf_rng(&config, nullptr, nullptr);
        (void)mbedtls_ssl_set_hostname(&ssl, nullptr);
        int result{ready ? mbedtls_ssl_session_reset(&ssl) : MBEDTLS_ERR_SSL_INTERNAL_ERROR};
        if (std::exchange(fail_next_reset, false)) result = MBEDTLS_ERR_SSL_ALLOC_FAILED;
        if (result != 0) {
            // Never reuse a partially configured or incompletely reset context.
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_init(&ssl);
            ready = false;
            ++discarded;
        } else {
            ++resets;
        }
        signatures.fill(0);
        in_use = false;
    }
};

WorkerTls& ThreadTls()
{
    thread_local WorkerTls tls;
    return tls;
}

struct Connection {
    WorkerTls& tls;
    mbedtls_ssl_context& ssl;
    const uint256& challenge;
    const Sock& socket;
    const std::function<bool()>& cancelled;
    const std::chrono::steady_clock::time_point deadline;
    bool random_set{false};
    std::array<unsigned char, 16384 + 5> sent{};
    size_t sent_size{0};
    bool captured_client_hello{false};
    Connection(const uint256& value, const Sock& sock,
               const std::function<bool()>& cancel, std::chrono::steady_clock::time_point end)
        : tls{ThreadTls()}, ssl{tls.ssl}, challenge(value), socket(sock), cancelled{cancel}, deadline{end}
    {
        if (tls.in_use) throw std::logic_error("Nested P2C TLS capture on one worker");
        tls.in_use = true;
    }
    ~Connection()
    {
        tls.Release();
    }
    bool IOAllowed() const noexcept
    {
        // The provider can consume multiple ignored records or partial writes
        // inside one handshake_step. Its BIO must enforce the same limits as
        // the outer loop, without unwinding an arbitrary callback through C.
        try {
            return !cancelled() && std::chrono::steady_clock::now() < deadline;
        } catch (...) {
            return false;
        }
    }
    int Configure(const std::string& domain, uint8_t mask)
    {
        tls.signatures.fill(0);
        size_t count{0};
        if (mask & PayToDomainOutput::SIGNATURE_ALGORITHM_ECDSA_P256_SHA256) tls.signatures[count++] = 0x0403;
        if (mask & PayToDomainOutput::SIGNATURE_ALGORITHM_RSA_PSS_RSAE_SHA256) tls.signatures[count++] = 0x0804;
        if (mask & PayToDomainOutput::SIGNATURE_ALGORITHM_RSA_PSS_PSS_SHA256) tls.signatures[count++] = 0x0809;
        mbedtls_ssl_conf_rng(&tls.config, Random, this);
        int result{0};
        if (!tls.ready) {
            result = mbedtls_ssl_setup(&ssl, &tls.config);
            if (result == 0) {
                tls.ready = true;
                ++tls.setups;
            }
        }
        if (result == 0) result = mbedtls_ssl_set_hostname(&ssl, domain.c_str());
        if (result == 0) mbedtls_ssl_set_bio(&ssl, this, Send, Receive, nullptr);
        return result;
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
        if (!self.IOAllowed()) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        const auto result{self.socket.Send(data, size, MSG_NOSIGNAL)};
        const int error{result < 0 ? WSAGetLastError() : 0};
        if (!self.IOAllowed()) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        if (result > 0 && !self.captured_client_hello) {
            const auto count{static_cast<size_t>(result)};
            if (count > self.sent.size() - self.sent_size) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
            std::copy_n(data, count, self.sent.data() + self.sent_size);
            self.sent_size += count;
        }
        if (result >= 0) return static_cast<int>(result);
        return IOErrorIsPermanent(error) ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    static int Receive(void* context, unsigned char* data, size_t size)
    {
        auto& self{*static_cast<Connection*>(context)};
        if (!self.IOAllowed()) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        const auto result{self.socket.Recv(data, size, 0)};
        const int error{result < 0 ? WSAGetLastError() : 0};
        if (!self.IOAllowed()) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        if (result >= 0) return static_cast<int>(result);
        return IOErrorIsPermanent(error) ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : MBEDTLS_ERR_SSL_WANT_READ;
    }
};

std::string TlsError(int result)
{
    char error[256]{};
    mbedtls_strerror(result, error, sizeof(error));
    return std::string{"TLS handshake failed: "} + error;
}

bool UnsupportedSocketFamily(int error)
{
#ifdef WIN32
    return error == WSAEAFNOSUPPORT || error == WSAEPFNOSUPPORT ||
        error == WSAEPROTONOSUPPORT || error == WSAESOCKTNOSUPPORT;
#else
    return error == EAFNOSUPPORT || error == EPROTONOSUPPORT
#ifdef EPFNOSUPPORT
        || error == EPFNOSUPPORT
#endif
#ifdef ESOCKTNOSUPPORT
        || error == ESOCKTNOSUPPORT
#endif
        ;
#endif
}

bool UnavailableRoute(int error)
{
#ifdef WIN32
    return error == WSAENETUNREACH || error == WSAEHOSTUNREACH || error == WSAENETDOWN;
#else
    return error == ENETUNREACH || error == EHOSTUNREACH || error == ENETDOWN;
#endif
}

std::unique_ptr<Sock> ConnectP2CSocket(const CService& endpoint,
    const std::function<bool()>& cancelled, const P2CTlsCaptureOptions& options,
    std::chrono::steady_clock::time_point& tls_deadline)
{
    const auto failed = [&](int error, bool creating) {
        if (options.on_local_unavailable && !cancelled() &&
            std::chrono::steady_clock::now() < options.deadline &&
            ((creating && UnsupportedSocketFamily(error)) || (!creating && UnavailableRoute(error)))) {
            options.on_local_unavailable(creating);
        }
    };
    // CreateSock preserves Core's selectable/nonblocking/no-SIGPIPE/TCP_NODELAY
    // platform setup and its test seam. No proxy fallback or name lookup here.
#ifdef WIN32
    WSASetLastError(0);
#else
    errno = 0;
#endif
    int socket_type{SOCK_STREAM};
#if defined(__linux__) && defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    // Avoid two fcntl syscalls and close the descriptor-inheritance race.
    socket_type |= SOCK_NONBLOCK | SOCK_CLOEXEC;
#endif
    auto socket{CreateSock(endpoint.GetSAFamily(), socket_type, IPPROTO_TCP)};
    if (!socket) {
        const int error{WSAGetLastError()};
        failed(error, true);
        return {};
    }
    // CreateSock already guarantees nonblocking public IPv4/IPv6 sockets.
    if (cancelled() || std::chrono::steady_clock::now() >= options.deadline) return {};
    sockaddr_storage address{};
    socklen_t length{sizeof(address)};
    if (!endpoint.GetSockAddr(reinterpret_cast<sockaddr*>(&address), &length)) return {};
    if (cancelled() || std::chrono::steady_clock::now() >= options.deadline) return {};
    if (options.before_tcp_start && !options.before_tcp_start()) return {};
    if (cancelled() || std::chrono::steady_clock::now() >= options.deadline) return {};
    if (options.on_tcp_started && !options.on_tcp_started()) return {};
    const auto started{std::chrono::steady_clock::now()};
    // Pacing/admission must not consume the transport budget. Preserve the
    // caller's absolute cap, then start fresh relative TCP/TLS limits here.
    tls_deadline = std::min(options.deadline, started + std::chrono::seconds{10});
    const auto deadline{std::min(tls_deadline, started + std::chrono::seconds{1})};
    if (socket->Connect(reinterpret_cast<sockaddr*>(&address), length) != SOCKET_ERROR) return socket;
    const int error{WSAGetLastError()};
#ifdef WIN32
    if (error == WSAEISCONN) return socket;
#endif
    if (error != WSAEINPROGRESS && error != WSAEWOULDBLOCK && error != WSAEINVAL && error != WSAEINTR) {
        failed(error, false);
        return {};
    }
    // Wait for readiness, not a fixed delay. Short slices preserve stop and
    // deadline responsiveness even while the TCP connect is still pending.
    while (!cancelled()) {
        const auto now{std::chrono::steady_clock::now()};
        if (now >= deadline) return {};
        const auto timeout{std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
            std::chrono::milliseconds{1}, std::chrono::milliseconds{100})};
        Sock::Event occurred{0};
        if (!socket->Wait(timeout, Sock::RecvEvent | Sock::SendEvent, &occurred)) {
            if (WSAGetLastError() == WSAEINTR) continue;
            return {};
        }
        if (cancelled() || std::chrono::steady_clock::now() >= deadline) return {};
        if (occurred == 0) continue;
        int connected_error{0};
        socklen_t error_length{sizeof(connected_error)};
        if (socket->GetSockOpt(SOL_SOCKET, SO_ERROR, &connected_error, &error_length) == SOCKET_ERROR) return {};
        if (connected_error != 0) {
            failed(connected_error, false);
            return {};
        }
        return socket;
    }
    return {};
}
} // namespace

P2CTlsReuseSnapshot P2CTlsReuseSnapshotForTest()
{
    const auto& tls{ThreadTls()};
    const bool clean{!tls.in_use && tls.config.p_rng == nullptr && tls.config.f_rng == nullptr &&
        std::all_of(tls.signatures.begin(), tls.signatures.end(), [](uint16_t value) { return value == 0; }) &&
        connectcoin_p2c_tls_reset_clean(&tls.ssl) != 0};
    return {tls.setups, tls.resets, tls.discarded,
        reinterpret_cast<uintptr_t>(tls.ssl.in_buf), reinterpret_cast<uintptr_t>(tls.ssl.out_buf), clean};
}

void FailNextP2CTlsResetForTest()
{
    ThreadTls().fail_next_reset = true;
}

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

util::Result<P2CCapturedProof> CaptureP2CTlsParsed(
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
    auto deadline{options.deadline};
    auto socket{ConnectP2CSocket(endpoint, cancelled, options, deadline)};
    if (!socket) return util::Error{Untranslated("P2C TCP connection failed")};
    if (cancelled() || std::chrono::steady_clock::now() >= deadline) return util::Error{Untranslated("P2C cancelled or deadline expired")};
    if (!InitializeP2CTlsCrypto()) return util::Error{Untranslated("TLS crypto initialization failed")};
    Connection connection{challenge, *socket, cancelled, deadline};
    if (const int result{connection.Configure(domain, options.signature_algorithms_mask)}; result != 0) {
        return util::Error{Untranslated(TlsError(result))};
    }
    // Keep CertificateVerify in the v2 proof even though its entire message is
    // excluded from connection work. The captured transcript must still be
    // authenticated by Core's certificate/signature verification before use.
    std::vector<unsigned char> proof{P2C_PROOF_VERSION};
    std::optional<P2CCapturedProof> parsed_proof;
    std::string captured_order;
    bool certificate_captured{false};
    while (!cancelled() && std::chrono::steady_clock::now() < deadline) {
        const int state{connection.ssl.state};
        if (state == MBEDTLS_SSL_CLIENT_HELLO && connection.captured_client_hello) {
            return util::Error{Untranslated("TLS HelloRetryRequest is not supported by P2C")};
        }
        int result;
        // Abbreviated capture retains authenticated TLS record/reassembly and
        // Certificate envelope checks, but leaves DER and signature work to
        // the cached full proof verifier. Completed RSA probes retain upstream
        // Certificate, CertificateVerify and Finished authentication unchanged.
        if (!options.complete_handshake && state == MBEDTLS_SSL_SERVER_CERTIFICATE) {
            result = connectcoin_p2c_fetch_certificate(&connection.ssl);
        } else if (!options.complete_handshake && state == MBEDTLS_SSL_CERTIFICATE_VERIFY) {
            result = connectcoin_p2c_fetch_certificate_verify(&connection.ssl);
        } else {
            result = mbedtls_ssl_handshake_step(&connection.ssl);
        }
        if (cancelled() || std::chrono::steady_clock::now() >= deadline) return util::Error{Untranslated("P2C cancelled or deadline expired")};
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
            const auto remaining{std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())};
            const auto wait{std::clamp(remaining, std::chrono::milliseconds{1}, std::chrono::milliseconds{100})};
            if (!socket->Wait(wait, result == MBEDTLS_ERR_SSL_WANT_READ ? Sock::RecvEvent : Sock::SendEvent)) break;
            continue;
        }
        if (result != 0) return util::Error{Untranslated(TlsError(result))};
        if (state == MBEDTLS_SSL_SERVER_CERTIFICATE) {
            // Enforce key limits before any RSA signature operation, even on
            // unused extra chain entries. Ordinary captures prime the exact
            // owned chain cache; completed probes retain the TLS peer chain.
            std::string error;
            if (!options.complete_handshake) {
                if (!ValidateP2CCertificateMessage({connection.ssl.in_msg, connection.ssl.in_hslen}, error)) {
                    return util::Error{Untranslated(error)};
                }
            } else {
                const auto* session{connection.ssl.session_negotiate};
                if (!session || !session->peer_cert) return util::Error{Untranslated("TLS server certificate is unavailable")};
                if (!CheckP2CCertificatePublicKeys(*session->peer_cert, error)) return util::Error{Untranslated(error)};
            }
        }
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
            auto storage{std::make_shared<P2CCapturedProof::Storage>(std::exchange(proof, {}), domain, challenge)};
            std::string error;
            if (!ParseP2CTlsProof(storage->bytes, domain, challenge, storage->view, error)) return util::Error{Untranslated(error + "; capture" + captured_order)};
            if (!P2CSignatureSchemeAllowed(options.signature_algorithms_mask, storage->view.certificate_verify_scheme)) {
                return util::Error{Untranslated("TLS server used a disallowed P2C signature scheme")};
            }
            parsed_proof = P2CCapturedProof{std::move(storage)};
            certificate_captured = true;
            if (!options.complete_handshake) return std::move(*parsed_proof);
        }
        if (connection.ssl.state == MBEDTLS_SSL_HANDSHAKE_OVER) {
            if (certificate_captured && !cancelled() && std::chrono::steady_clock::now() < deadline) return std::move(*parsed_proof);
            break;
        }
    }
    return util::Error{Untranslated(cancelled() ? "P2C cancelled" : "P2C TLS timeout")};
}

util::Result<std::vector<unsigned char>> CaptureP2CTls(
    const CService& endpoint, const std::string& domain, const uint256& challenge,
    const std::function<bool()>& cancelled, const P2CTlsCaptureOptions& options)
{
    auto captured{CaptureP2CTlsParsed(endpoint, domain, challenge, cancelled, options)};
    if (!captured) return util::Error{util::ErrorString(captured)};
    return captured->Bytes();
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
    const auto proof{CaptureP2CTlsParsed(endpoints.front(), domain, challenge, expired, options)};
    if (expired() || !proof) return false;
    std::string error;
    const CTxOut requirement{0, PayToDomainOutput{domain, uint256{}, roots_version,
        PayToDomainOutput::SIGNATURE_ALGORITHMS_RSA}};
    return VerifyP2CCertificateProof(requirement, proof->View(), GetTime(), error) && !expired();
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
