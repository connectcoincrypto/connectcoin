// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_SIGNET_H
#define CONNECTCOIN_SIGNET_H

#include <primitives/block.h>
#include <primitives/transaction.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

class CScript;
namespace Consensus {
struct Params;
} // namespace Consensus

/**
 * Extract signature and check whether a block has a valid solution
 */
bool CheckSignetBlockSolution(const CBlock& block, const Consensus::Params& consensusParams);

/**
 * Return whether a signet challenge is one of the deliberately supported
 * truthy scripts that needs no scriptSig or witness solution. ConnectCoin's
 * typed outputs cannot represent BIP325's arbitrary Script prevout.
 */
inline bool IsTrivialSignetChallenge(std::span<const uint8_t> challenge)
{
    if (challenge.size() == 1 && challenge[0] >= 0x51 && challenge[0] <= 0x60) {
        return true; // OP_1 through OP_16
    }
    if (challenge.size() < 2 || challenge.size() > 76 || std::size_t{challenge[0]} + 1 != challenge.size()) {
        return false;
    }
    const auto value{challenge.subspan(1)};
    for (std::size_t index{0}; index < value.size(); ++index) {
        if (value[index] != 0) {
            return !(index == value.size() - 1 && value[index] == 0x80);
        }
    }
    return false;
}

/**
 * Generate the signet tx corresponding to the given block
 *
 * The signet tx commits to everything in the block except:
 * 1. It hashes a modified merkle root with the signet signature removed.
 * 2. It skips the nonce.
 */
class SignetTxs {
    template<class T1, class T2>
    SignetTxs(const T1& to_spend, const T2& to_sign) : m_to_spend{to_spend}, m_to_sign{to_sign} { }

public:
    static std::optional<SignetTxs> Create(const CBlock& block, const CScript& challenge);

    const CTransaction m_to_spend;
    const CTransaction m_to_sign;
};

#endif // CONNECTCOIN_SIGNET_H
