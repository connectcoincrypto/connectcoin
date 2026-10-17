# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

if(NOT DEFINED MBEDTLS_SOURCE_DIR OR NOT COMMAND p2c_mbedtls_replace)
  message(FATAL_ERROR "Load the checked Mbed TLS replacement helper first")
endif()

# Mbed TLS 3.6.7 increments its self-test counters in ordinary EC arithmetic.
# Keep the timing-resistance self-tests enabled without sharing diagnostic
# state across independent TLS/X.509 workers. Use the supported compilers'
# C99-compatible TLS spellings; no arithmetic or public context layout changes.
p2c_mbedtls_replace(library/ecp.c
[=[#if defined(MBEDTLS_ECP_C)
static unsigned long add_count, dbl_count;
#endif /* MBEDTLS_ECP_C */
static unsigned long mul_count;]=]
[=[/* ConnectCoin: self-test state belongs to the calling thread. */
#if defined(_MSC_VER)
#define CONNECTCOIN_ECP_THREAD_LOCAL __declspec(thread)
#else
#define CONNECTCOIN_ECP_THREAD_LOCAL __thread
#endif
#if defined(MBEDTLS_ECP_C)
static CONNECTCOIN_ECP_THREAD_LOCAL unsigned long add_count, dbl_count;
#endif /* MBEDTLS_ECP_C */
static CONNECTCOIN_ECP_THREAD_LOCAL unsigned long mul_count;]=] 1)

# This insecure RNG is used exclusively by the ECP self-test. Keep concurrent
# self-tests independent too; production entropy and RNGs remain unchanged.
p2c_mbedtls_replace(library/ecp.c
  "    static uint32_t state = 42;"
  "    static CONNECTCOIN_ECP_THREAD_LOCAL uint32_t state = 42;" 1)
