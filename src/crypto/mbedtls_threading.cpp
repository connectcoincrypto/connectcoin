// Copyright (c) 2026 The ConnectCoin developers. MIT license.
#include <crypto/mbedtls_threading.h>
#include <consensus/p2c_x509_mutex.h>
#include <mbedtls/threading.h>
#include <psa/crypto.h>
#include <new>

namespace {
using Mutex = consensus::p2c::RootKeyCacheMutex;
void Init(mbedtls_threading_mutex_t* mutex)
{
    if (!mutex) return;
    mutex->mutex = nullptr;
    // A native mutex constructor may throw independently of allocation.
    // Never propagate an exception through the provider's C callbacks.
    try { mutex->mutex = new (std::nothrow) Mutex; } catch (...) {}
}
void Free(mbedtls_threading_mutex_t* mutex)
{
    if (mutex) { delete static_cast<Mutex*>(mutex->mutex); mutex->mutex = nullptr; }
}
int Lock(mbedtls_threading_mutex_t* mutex)
{
    if (!mutex || !mutex->mutex) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    try { static_cast<Mutex*>(mutex->mutex)->lock(); return 0; }
    catch (...) { return MBEDTLS_ERR_THREADING_MUTEX_ERROR; }
}
int Unlock(mbedtls_threading_mutex_t* mutex)
{
    if (!mutex || !mutex->mutex) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    try { static_cast<Mutex*>(mutex->mutex)->unlock(); return 0; }
    catch (...) { return MBEDTLS_ERR_THREADING_MUTEX_ERROR; }
}
} // namespace

bool InitializeP2CCrypto() noexcept
{
    // Function-local initialization is serialized by the C++ runtime. Failure
    // stays fail-closed; reinstalling callbacks could race existing contexts.
    static const bool initialized = [] {
        mbedtls_threading_set_alt(Init, Free, Lock, Unlock);
        return true;
    }();
    return initialized;
}

bool InitializeP2CTlsCrypto() noexcept
{
    if (!InitializeP2CCrypto()) return false;
    static const bool initialized = [] {
#if defined(MBEDTLS_NO_PLATFORM_ENTROPY)
        return false;
#else
        return psa_crypto_init() == PSA_SUCCESS;
#endif
    }();
    return initialized;
}
