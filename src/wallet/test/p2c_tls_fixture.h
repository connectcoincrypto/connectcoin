// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_TEST_P2C_TLS_FIXTURE_H
#define CONNECTCOIN_WALLET_TEST_P2C_TLS_FIXTURE_H

#include <random.h>
#include <test/util/net.h>
#include <tinyformat.h>

#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <algorithm>
#include <cerrno>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wallet::test {

/** Public test CA for VerifyP2CCertificateProofForTest only. Never installed in
 * the production root store; this matches the consensus TLS test fixture.
 */
inline constexpr unsigned char P2C_TEST_ROOTS_PEM[]{R"pem(-----BEGIN CERTIFICATE-----
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

/** In-memory TLS 1.3 server for sequential capture/worker tests. The public test
 * certificate/key match the existing consensus fixture in p2c_tests.cpp. They
 * are deliberately NOT trusted by Core. No real DNS, sockets or HTTP are used.
 */
struct P2CTLSServer {
    mbedtls_ssl_config config{};
    mbedtls_x509_crt certificate{};
    mbedtls_pk_context key{};

    P2CTLSServer()
    {
        mbedtls_ssl_config_init(&config);
        mbedtls_x509_crt_init(&certificate);
        mbedtls_pk_init(&key);
    }
    ~P2CTLSServer()
    {
        mbedtls_ssl_config_free(&config);
        mbedtls_x509_crt_free(&certificate);
        mbedtls_pk_free(&key);
    }
    P2CTLSServer(const P2CTLSServer&) = delete;
    P2CTLSServer& operator=(const P2CTLSServer&) = delete;

    static int Random(void*, unsigned char* bytes, size_t size)
    {
        while (size != 0) {
            const auto chunk{std::min(size, size_t{32})};
            GetStrongRandBytes(std::span{bytes, chunk});
            bytes += chunk;
            size -= chunk;
        }
        return 0;
    }
    static void Check(int result)
    {
        if (result != 0) throw std::runtime_error(strprintf("P2C test TLS server setup failed: %d", result));
    }
    void Setup()
    {
        static constexpr char CERTIFICATE[]{R"pem(-----BEGIN CERTIFICATE-----
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
)pem"};
        static constexpr char PRIVATE_KEY[]{R"pem(-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIPEqEyB2AnCoPL/9U/YDHvdqXYbIogTywwyp6/UfDw6noAoGCCqGSM49
AwEHoUQDQgAEN8xW2XYJHlpyPsdZLf8gbu58+QaRdNCtFLX3aCJZYpJO5QDYIxH/
6i/SNF1dFr2KiMJrdw1VzYoqDvoByLTt/w==
-----END EC PRIVATE KEY-----
)pem"};
        Check(psa_crypto_init());
        Check(mbedtls_x509_crt_parse(&certificate, reinterpret_cast<const unsigned char*>(CERTIFICATE), sizeof(CERTIFICATE)));
        Check(mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char*>(PRIVATE_KEY), sizeof(PRIVATE_KEY), nullptr, 0, Random, nullptr));
        Check(mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT));
        mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_max_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_tls13_key_exchange_modes(&config, MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL);
        mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&config, Random, nullptr);
        Check(mbedtls_ssl_conf_own_cert(&config, &certificate, &key));
    }
};

/** Pump the server synchronously from the client's I/O callbacks. Handshake
 * crypto therefore runs under CaptureP2CTls's existing crypto mutex. Use one
 * connection at a time; setup/teardown also touch the library's PSA state.
 */
class P2CTLSSocket final : public ZeroSock {
    P2CTLSSocket& operator=(Sock&&) override { throw std::logic_error("Move of Sock into P2CTLSSocket not allowed"); }
    const std::shared_ptr<P2CTLSServer> m_config;
    // Reuse freed allocations to expose accidentally dangling domain strings.
    const std::vector<std::string> m_churn;
    mutable mbedtls_ssl_context m_server{};
    mutable std::vector<unsigned char> m_to_server, m_to_client;
    mutable size_t m_server_pos{0}, m_client_pos{0};
    const size_t m_chunk;

    static int ServerSend(void* ctx, const unsigned char* bytes, size_t size)
    {
        auto& self{*static_cast<P2CTLSSocket*>(ctx)};
        self.m_to_client.insert(self.m_to_client.end(), bytes, bytes + size);
        return static_cast<int>(size);
    }
    static int ServerReceive(void* ctx, unsigned char* bytes, size_t size)
    {
        auto& self{*static_cast<P2CTLSSocket*>(ctx)};
        const auto count{std::min(size, self.m_to_server.size() - self.m_server_pos)};
        if (count == 0) return MBEDTLS_ERR_SSL_WANT_READ;
        std::copy_n(self.m_to_server.begin() + self.m_server_pos, count, bytes);
        self.m_server_pos += count;
        return static_cast<int>(count);
    }

public:
    P2CTLSSocket(std::shared_ptr<P2CTLSServer> config, std::vector<std::string> churn, size_t chunk)
        : m_config{std::move(config)}, m_churn{std::move(churn)}, m_chunk{chunk}
    {
        mbedtls_ssl_init(&m_server);
        const int result{mbedtls_ssl_setup(&m_server, &m_config->config)};
        if (result != 0) {
            mbedtls_ssl_free(&m_server);
            P2CTLSServer::Check(result);
        }
        mbedtls_ssl_set_bio(&m_server, this, ServerSend, ServerReceive, nullptr);
    }
    ~P2CTLSSocket() override { mbedtls_ssl_free(&m_server); }

    ssize_t Send(const void* bytes, size_t size, int) const override
    {
        const auto* start{static_cast<const unsigned char*>(bytes)};
        const size_t count{std::min(size, m_chunk)};
        m_to_server.insert(m_to_server.end(), start, start + count);
        return static_cast<ssize_t>(count);
    }
    ssize_t Recv(void* bytes, size_t size, int) const override
    {
        if (m_client_pos == m_to_client.size()) {
            const int result{mbedtls_ssl_handshake(&m_server)};
            if (result != 0 && result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        }
        const auto count{std::min({size, m_chunk, m_to_client.size() - m_client_pos})};
        if (count == 0) {
            #ifdef WIN32
            WSASetLastError(WSAEWOULDBLOCK);
            #else
            errno = EAGAIN;
            #endif
            return -1;
        }
        std::copy_n(m_to_client.begin() + m_client_pos, count, static_cast<unsigned char*>(bytes));
        m_client_pos += count;
        return static_cast<ssize_t>(count);
    }
};
} // namespace wallet::test

#endif // CONNECTCOIN_WALLET_TEST_P2C_TLS_FIXTURE_H
