// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_CONSENSUS_AMOUNT_H
#define CONNECTCOIN_CONSENSUS_AMOUNT_H

#include <cstdint>

/** Amount in connects (can be negative). */
typedef int64_t CAmount;

/** The amount of connects in one CC. */
inline constexpr CAmount COIN{10'000'000'000};

/** No amount larger than this (in connects) is valid.
 *
 * Note that this constant is *not* the total money supply. It is an upper-bound
 * sanity check for any monetary amount. As this sanity check is used by consensus-critical
 * validation code, the exact value of the MAX_MONEY constant is consensus
 * critical; in unusual circumstances like a(nother) overflow bug that allowed
 * for the creation of coins out of thin air modification could lead to a fork.
 * */
inline constexpr CAmount MAX_MONEY{100'000'000 * COIN};
inline bool MoneyRange(const CAmount& nValue) { return (nValue >= 0 && nValue <= MAX_MONEY); }

#endif // CONNECTCOIN_CONSENSUS_AMOUNT_H
