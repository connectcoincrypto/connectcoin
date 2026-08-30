// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_POW_H
#define CONNECTCOIN_POW_H

#include <consensus/params.h>

#include <cstdint>

class CBlockHeader;
class CBlockIndex;
class uint256;
class arith_uint256;

/**
 * Convert nBits value to target.
 *
 * @param[in] nBits     compact representation of the target
 * @param[in] pow_limit PoW limit (consensus parameter)
 *
 * @return              the proof-of-work target or nullopt if the nBits value
 *                      is invalid (due to overflow or exceeding pow_limit)
 */
std::optional<arith_uint256> DeriveTarget(unsigned int nBits, uint256 pow_limit);

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params&);
unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params&);

/** Check whether a block hash satisfies the proof-of-work requirement specified by nBits */
bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params&);
bool CheckProofOfWorkImpl(uint256 hash, unsigned int nBits, const Consensus::Params&);

/** Return the key-block height used by RandomX for a candidate block height. */
int RandomXSeedHeight(int block_height, const Consensus::Params& params);

/** Derive the RandomX key for the block following pindexPrev. */
uint256 GetRandomXKey(const CBlockIndex* pindexPrev, const Consensus::Params& params);

/** Calculate the RandomX v2 proof-of-work hash of an 80-byte block header. */
uint256 GetPoWHash(const CBlockHeader& header, const uint256& key, const Consensus::Params& params);

/** Check RandomX proof of work using chain context or an already-derived key. */
bool CheckProofOfWork(const CBlockHeader& header, const CBlockIndex* pindexPrev, const Consensus::Params& params);
bool CheckProofOfWork(const CBlockHeader& header, const uint256& key, int block_height, const Consensus::Params& params);

/** Begin initializing a key-specific dataset without waiting for completion. */
void PrepareRandomXKey(const uint256& key, const Consensus::Params& params);

/** Prepare the current and, during the lag window, next active-chain key. */
void PrepareRandomXKeys(const CBlockIndex* tip, const Consensus::Params& params);

/**
 * Return false if the proof-of-work requirement specified by new_nbits at a
 * given height is not possible, given the proof-of-work on the prior block as
 * specified by old_nbits.
 *
 * This function only checks that the new value is within a factor of 4 of the
 * old value for blocks at the difficulty adjustment interval, and otherwise
 * requires the values to be the same.
 *
 * Always returns true on networks where min difficulty blocks are allowed,
 * such as regtest/testnet.
 */
bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t height, uint32_t old_nbits, uint32_t new_nbits);

#endif // CONNECTCOIN_POW_H
