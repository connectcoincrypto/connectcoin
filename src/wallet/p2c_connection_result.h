// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_CONNECTION_RESULT_H
#define CONNECTCOIN_WALLET_P2C_CONNECTION_RESULT_H

#include <consensus/p2c.h>
#include <primitives/transaction.h>
#include <util/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wallet {

/** Validate and observe one completed TCP/TLS attempt against a P2C output.
 * A success requires both parsing and certificate/signature verification; work
 * target probability is applied separately by the scheduler. Incomplete local
 * cancellations supply no observation. Completed captures are evaluated even
 * if local cancellation races with their completion.
 * The caller supplies the verifier and serializes the observe callback, so
 * certificate verification never needs to hold the scheduler's lock.
 */
template <typename VerifyCertificate, typename Observe>
bool ObserveP2CTlsCapture(const util::Result<std::vector<unsigned char>>& captured,
                         const CTxOut& spent_output, const uint256& challenge,
                         int64_t validation_time, bool cancelled,
                         P2CTlsProofView& view, std::string& error,
                         const VerifyCertificate& verify_certificate, const Observe& observe)
{
    if (!captured) {
        if (!cancelled) observe(false);
        return false;
    }
    const bool valid{ParseP2CTlsProof(*captured, spent_output.GetPayToDomain()->domain, challenge, view, error) &&
        verify_certificate(spent_output, view, validation_time, error)};
    observe(valid);
    return valid;
}

} // namespace wallet

#endif // CONNECTCOIN_WALLET_P2C_CONNECTION_RESULT_H
