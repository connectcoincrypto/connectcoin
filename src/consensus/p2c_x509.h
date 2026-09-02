// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_CONSENSUS_P2C_X509_H
#define CONNECTCOIN_CONSENSUS_P2C_X509_H

#include <cstdint>
#include <span>
#include <string>

class CTxOut;
struct P2CTlsProofView;

/** Return whether the immutable version-1 trust store parsed successfully. */
bool P2CRootStoreAvailable();

/**
 * Verify the domain path against the versioned root bundle, certificate
 * validity at validation_time, server-auth key usage, and the TLS 1.3
 * CertificateVerify signature over the parsed transcript.
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
