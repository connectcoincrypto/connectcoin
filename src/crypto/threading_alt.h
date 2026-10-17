// Copyright (c) 2026 The ConnectCoin developers. MIT license.
#ifndef CONNECTCOIN_CRYPTO_THREADING_ALT_H
#define CONNECTCOIN_CRYPTO_THREADING_ALT_H
/* C-compatible storage. InitializeP2CCrypto installs owning mutex callbacks. */
typedef struct mbedtls_threading_mutex_t { void* mutex; } mbedtls_threading_mutex_t;
#endif // CONNECTCOIN_CRYPTO_THREADING_ALT_H
