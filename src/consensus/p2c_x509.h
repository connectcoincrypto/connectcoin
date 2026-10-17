// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_CONSENSUS_P2C_X509_H
#define CONNECTCOIN_CONSENSUS_P2C_X509_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

class CTxOut;
struct P2CTlsProofView;
struct mbedtls_x509_crt;

/**
 * Cheap preflight of every parsed certificate's RSA public exponent (at most
 * 64 bits), including RSA-PSS keys. Does not verify signatures or trust.
 * Also used by TLS capture before processing the server's CertificateVerify.
 */
bool CheckP2CCertificatePublicKeys(const mbedtls_x509_crt& chain, std::string& error);

/** Return whether the immutable version-1 trust store parsed and passed key limits. */
bool P2CRootStoreAvailable();

/** Capture preflight: parse one Certificate message and enforce public-key
 * limits. Reuses only immutable exact-DER data, never trust/time/CV decisions.
 */
bool ValidateP2CCertificateMessage(std::span<const unsigned char> message, std::string& error);

/** Per-thread public-cache instrumentation for regression tests. */
struct P2CX509CacheStats {
    uint64_t certificate_hits, certificate_misses;
    size_t certificate_entries, certificate_bytes, certificate_estimated_bytes;
    uint64_t signature_hits, signature_misses;
    size_t signature_entries, signature_bytes;
};
P2CX509CacheStats P2CX509CacheStatsForTest();
void ClearP2CX509CachesForTest();

/**
 * Check public-exponent limits before verifying the domain path against the
 * versioned root bundle (root-to-leaf, fail-fast), certificate validity at
 * validation_time, server-auth key usage, and the TLS 1.3 CertificateVerify
 * signature over the parsed transcript.
 */
bool VerifyP2CCertificateProof(const CTxOut& spent_output,
                               const P2CTlsProofView& proof,
                               int64_t validation_time,
                               std::string& error);

/** Unit-test entry point using an explicit PEM trust store instead of consensus roots. */
bool VerifyP2CCertificateProofForTest(const CTxOut& spent_output,
                                      const P2CTlsProofView& proof,
                                      int64_t validation_time,
                                      std::span<const unsigned char> trusted_roots_pem,
                                      std::string& error);

#endif // CONNECTCOIN_CONSENSUS_P2C_X509_H
