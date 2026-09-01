// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/p2c.h>
#include <test/fuzz/fuzz.h>

#include <algorithm>
#include <cstdint>
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
        (void)P2CMeetsWorkTarget(parsed.connection_work_hash, challenge);
    }
}
