// Copyright (c) 2026 The ConnectCoin developers. MIT license.
#ifndef CONNECTCOIN_CRYPTO_MBEDTLS_THREADING_H
#define CONNECTCOIN_CRYPTO_MBEDTLS_THREADING_H
/** Install provider mutexes once before constructing provider objects.
 * Consensus verification must not acquire a new dependency on OS entropy.
 * No shutdown/free while worker-local TLS or certificate objects can exist.
 */
bool InitializeP2CCrypto() noexcept;
/** Initialize the PSA random/key subsystem for TLS, after provider mutexes. */
bool InitializeP2CTlsCrypto() noexcept;
#endif
