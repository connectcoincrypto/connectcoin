// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef CONNECTCOIN_WALLET_P2C_TLS_PRIVATE_H
#define CONNECTCOIN_WALLET_P2C_TLS_PRIVATE_H
#include <mbedtls/ssl.h>
#ifdef __cplusplus
extern "C" {
#endif
unsigned char* connectcoin_p2c_client_random(mbedtls_ssl_context* ssl);
/* Abbreviated captures only. Retain the pinned Certificate envelope/extension
 * checks, but defer DER parsing and all trust to the owned cached verifier.
 * Never use this path to complete a handshake or to authenticate Finished. */
int connectcoin_p2c_fetch_certificate(mbedtls_ssl_context* ssl);
/* Fetch/decrypt only; full proof verification MUST authenticate the signature. */
int connectcoin_p2c_fetch_certificate_verify(mbedtls_ssl_context* ssl);
/* Test instrumentation inspects cleanliness only, never returns key material. */
int connectcoin_p2c_tls_reset_clean(const mbedtls_ssl_context* ssl);
int connectcoin_p2c_test_certificate(const unsigned char* message, size_t length,
                                   int abbreviated, uint32_t sent_extensions,
                                   uint32_t* received_extensions, int* peer_present);
#ifdef __cplusplus
}
#endif
#endif // CONNECTCOIN_WALLET_P2C_TLS_PRIVATE_H
