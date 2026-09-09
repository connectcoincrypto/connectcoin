// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/p2c.h>
#include <hash.h>
#include <test/fuzz/fuzz.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

FUZZ_TARGET(p2c_tls_proof)
{
    uint256 challenge;
    const size_t challenge_size{std::min(buffer.size(), size_t{challenge.size()})};
    std::copy_n(buffer.begin(), challenge_size, challenge.begin());
    const std::span<const uint8_t> proof{buffer.subspan(challenge_size)};
    P2CTlsProofView parsed;
    std::string error;
    if (ParseP2CTlsProof(proof, "example.com", challenge, parsed, error)) {
        HashWriter work{TaggedHash("ConnectCoin/P2C/work/v2")};
        for (const auto message : {parsed.client_hello, parsed.server_hello, parsed.encrypted_extensions, parsed.certificate}) {
            work.write(std::as_bytes(message));
        }
        assert(parsed.connection_work_hash == work.GetSHA256());

        // Signature bytes must still be structurally present, but changing them
        // cannot produce another work candidate (cryptographic validity is
        // checked separately by the certificate verifier).
        std::vector<unsigned char> changed_signature{proof.begin(), proof.end()};
        changed_signature.back() ^= 1;
        P2CTlsProofView changed;
        assert(ParseP2CTlsProof(changed_signature, "example.com", challenge, changed, error));
        assert(changed.transcript_hash == parsed.transcript_hash);
        assert(changed.connection_work_hash == parsed.connection_work_hash);
        assert(P2CMeetsWorkTarget(changed.connection_work_hash, challenge) == P2CMeetsWorkTarget(parsed.connection_work_hash, challenge));
    }
}
