// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <crypto/randomx_util.h>
#include <primitives/block.h>
#include <span.h>
#include <streams.h>
#include <uint256.h>
#include <util/check.h>
#include <util/log.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

namespace {

struct RandomXCacheEntry {
    uint256 key;
    RandomXMemoryMode mode;
    uint64_t last_use;
    std::shared_future<std::shared_ptr<const RandomXContext>> context;
};

std::shared_ptr<const RandomXContext> MakeRandomXContext(const uint256& key, RandomXMemoryMode mode)
{
    try {
        return std::make_shared<const RandomXContext>(RandomXAlgorithm::V2, MakeByteSpan(key), mode);
    } catch (const std::exception& e) {
        if (mode != RandomXMemoryMode::FAST) throw;
        LogWarning("Unable to initialize the RandomX FAST dataset (%s); falling back to consensus-equivalent LIGHT mode.\n", e.what());
        return std::make_shared<const RandomXContext>(RandomXAlgorithm::V2, MakeByteSpan(key), RandomXMemoryMode::LIGHT);
    }
}

class RandomXContextCache
{
public:
    std::shared_ptr<const RandomXContext> Get(const uint256& key, RandomXMemoryMode mode)
    {
        std::shared_future<std::shared_ptr<const RandomXContext>> future;
        {
            std::lock_guard lock{m_mutex};
            CleanupRetired();
            auto it{Find(key, mode)};
            if (it == m_entries.end()) {
                it = Insert(key, mode);
            } else {
                it->last_use = ++m_clock;
            }
            future = it->context;
        }

        try {
            return future.get();
        } catch (...) {
            std::lock_guard lock{m_mutex};
            const auto it{Find(key, mode)};
            if (it != m_entries.end()) m_entries.erase(it);
            throw;
        }
    }

    void Prepare(const uint256& key, RandomXMemoryMode mode)
    {
        std::lock_guard lock{m_mutex};
        CleanupRetired();
        auto it{Find(key, mode)};
        if (it == m_entries.end()) {
            Insert(key, mode);
        } else {
            it->last_use = ++m_clock;
        }
    }

private:
    static constexpr size_t MAX_CONTEXTS{2};
    using EntryIterator = std::vector<RandomXCacheEntry>::iterator;

    std::mutex m_mutex;
    uint64_t m_clock{0};
    std::vector<RandomXCacheEntry> m_entries;
    std::vector<std::shared_future<std::shared_ptr<const RandomXContext>>> m_retired;

    EntryIterator Find(const uint256& key, RandomXMemoryMode mode)
    {
        return std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& entry) {
            return entry.key == key && entry.mode == mode;
        });
    }

    void CleanupRetired()
    {
        std::erase_if(m_retired, [](const auto& future) {
            return future.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
        });
    }

    EntryIterator Insert(const uint256& key, RandomXMemoryMode mode)
    {
        // Destroying the last shared_future returned by std::async may wait for
        // the task. Keep evicted builds alive until they are ready so callers
        // (notably UpdateTip while holding cs_main) never block on eviction.
        auto future{std::async(std::launch::async, [key, mode] {
            return MakeRandomXContext(key, mode);
        }).share()};
        m_entries.push_back(RandomXCacheEntry{key, mode, ++m_clock, std::move(future)});

        if (m_entries.size() > MAX_CONTEXTS) {
            const auto oldest{std::min_element(m_entries.begin(), m_entries.end(), [](const auto& a, const auto& b) {
                return a.last_use < b.last_use;
            })};
            if (oldest->context.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
                m_retired.push_back(std::move(oldest->context));
            }
            m_entries.erase(oldest);
        }
        return Find(key, mode);
    }
};

RandomXContextCache& GetRandomXContextCache()
{
    static RandomXContextCache cache;
    return cache;
}

RandomXMemoryMode GetRandomXMemoryMode(const Consensus::Params& params)
{
    return params.randomx_fast_mode ? RandomXMemoryMode::FAST : RandomXMemoryMode::LIGHT;
}

} // namespace

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight+1) % params.DifficultyAdjustmentInterval() != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then it MUST be a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be 14 days worth of blocks
    int nHeightFirst = pindexLast->nHeight - (params.DifficultyAdjustmentInterval()-1);
    assert(nHeightFirst >= 0);
    const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
    assert(pindexFirst);

    return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;

    // Special difficulty rule for Testnet4
    if (params.enforce_BIP94) {
        // Here we use the first block of the difficulty period. This way
        // the real difficulty is always preserved in the first block as
        // it is not allowed to use the min-difficulty exception.
        int nHeightFirst = pindexLast->nHeight - (params.DifficultyAdjustmentInterval()-1);
        const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
        bnNew.SetCompact(pindexFirst->nBits);
    } else {
        bnNew.SetCompact(pindexLast->nBits);
    }

    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

// Check that on difficulty adjustments, the new difficulty does not increase
// or decrease beyond the permitted limits.
bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t height, uint32_t old_nbits, uint32_t new_nbits)
{
    if (params.fPowAllowMinDifficultyBlocks) return true;

    if (height % params.DifficultyAdjustmentInterval() == 0) {
        int64_t smallest_timespan = params.nPowTargetTimespan/4;
        int64_t largest_timespan = params.nPowTargetTimespan*4;

        const arith_uint256 pow_limit = UintToArith256(params.powLimit);
        arith_uint256 observed_new_target;
        observed_new_target.SetCompact(new_nbits);

        // Calculate the largest difficulty value possible:
        arith_uint256 largest_difficulty_target;
        largest_difficulty_target.SetCompact(old_nbits);
        largest_difficulty_target *= largest_timespan;
        largest_difficulty_target /= params.nPowTargetTimespan;

        if (largest_difficulty_target > pow_limit) {
            largest_difficulty_target = pow_limit;
        }

        // Round and then compare this new calculated value to what is
        // observed.
        arith_uint256 maximum_new_target;
        maximum_new_target.SetCompact(largest_difficulty_target.GetCompact());
        if (maximum_new_target < observed_new_target) return false;

        // Calculate the smallest difficulty value possible:
        arith_uint256 smallest_difficulty_target;
        smallest_difficulty_target.SetCompact(old_nbits);
        smallest_difficulty_target *= smallest_timespan;
        smallest_difficulty_target /= params.nPowTargetTimespan;

        if (smallest_difficulty_target > pow_limit) {
            smallest_difficulty_target = pow_limit;
        }

        // Round and then compare this new calculated value to what is
        // observed.
        arith_uint256 minimum_new_target;
        minimum_new_target.SetCompact(smallest_difficulty_target.GetCompact());
        if (minimum_new_target > observed_new_target) return false;
    } else if (old_nbits != new_nbits) {
        return false;
    }
    return true;
}

// Bypasses the actual proof of work check during fuzz testing with a simplified validation checking whether
// the most significant bit of the last byte of the hash is set.
bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    if (EnableFuzzDeterminism()) return (hash.data()[31] & 0x80) == 0;
    return CheckProofOfWorkImpl(hash, nBits, params);
}

std::optional<arith_uint256> DeriveTarget(unsigned int nBits, const uint256 pow_limit)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(pow_limit))
        return {};

    return bnTarget;
}

bool CheckProofOfWorkImpl(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    auto bnTarget{DeriveTarget(nBits, params.powLimit)};
    if (!bnTarget) return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}

int RandomXSeedHeight(int block_height, const Consensus::Params& params)
{
    assert(block_height >= 0);
    if (params.randomx_epoch_blocks == 0) return 0;
    assert(params.randomx_epoch_lag > 0);
    assert(params.randomx_epoch_lag < params.randomx_epoch_blocks);
    if (block_height <= static_cast<int>(params.randomx_epoch_lag)) return 0;
    return ((block_height - params.randomx_epoch_lag) / params.randomx_epoch_blocks) * params.randomx_epoch_blocks;
}

uint256 GetRandomXKey(const CBlockIndex* pindexPrev, const Consensus::Params& params)
{
    const int block_height{pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1};
    const int seed_height{RandomXSeedHeight(block_height, params)};
    if (seed_height == 0) return params.randomx_bootstrap_key;

    assert(pindexPrev != nullptr);
    const CBlockIndex* seed_block{pindexPrev->GetAncestor(seed_height)};
    assert(seed_block != nullptr);
    return seed_block->GetBlockHash();
}

uint256 GetPoWHash(const CBlockHeader& header, const uint256& key, const Consensus::Params& params)
{
    DataStream stream;
    stream << header;
    assert(stream.size() == 80);

    const auto context{GetRandomXContextCache().Get(key, GetRandomXMemoryMode(params))};
    const auto hash{context->Calculate(MakeByteSpan(stream))};
    return uint256{MakeUCharSpan(hash)};
}

bool CheckProofOfWork(const CBlockHeader& header, const CBlockIndex* pindexPrev, const Consensus::Params& params)
{
    const int block_height{pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1};
    return CheckProofOfWork(header, GetRandomXKey(pindexPrev, params), block_height, params);
}

bool CheckProofOfWork(const CBlockHeader& header, const uint256& key, int block_height, const Consensus::Params& params)
{
    if (params.randomx_mock_pow) {
        // The hardcoded genesis was mined with RandomX. Test harnesses that
        // replace RandomX with header hashing must still be able to initialize
        // the chain from that exact consensus genesis.
        if (block_height == 0 && header.GetHash() == params.hashGenesisBlock) return true;
        return CheckProofOfWorkImpl(header.GetHash(), header.nBits, params);
    }
    if (EnableFuzzDeterminism()) return (header.GetHash().data()[31] & 0x80) == 0;
    return CheckProofOfWorkImpl(GetPoWHash(header, key, params), header.nBits, params);
}

void PrepareRandomXKey(const uint256& key, const Consensus::Params& params)
{
    if (params.randomx_mock_pow || EnableFuzzDeterminism()) return;
    GetRandomXContextCache().Prepare(key, GetRandomXMemoryMode(params));
}

void PrepareRandomXKeys(const CBlockIndex* tip, const Consensus::Params& params)
{
    // Always prepare the key needed by the block after the active tip. This is
    // the bootstrap key for a new chain and the current epoch key otherwise.
    PrepareRandomXKey(GetRandomXKey(tip, params), params);

    if (tip == nullptr || params.randomx_epoch_blocks == 0) return;

    // A key block becomes usable only after randomx_epoch_lag blocks. During
    // that window, prepare its dataset without allowing arbitrary headers or
    // side chains to create multi-gigabyte datasets.
    const int epoch_blocks{static_cast<int>(params.randomx_epoch_blocks)};
    const int key_height{tip->nHeight - tip->nHeight % epoch_blocks};
    if (key_height == 0 || tip->nHeight >= key_height + static_cast<int>(params.randomx_epoch_lag)) return;

    const CBlockIndex* key_block{tip->GetAncestor(key_height)};
    assert(key_block != nullptr);
    PrepareRandomXKey(key_block->GetBlockHash(), params);
}
