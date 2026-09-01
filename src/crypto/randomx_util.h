// Copyright (c) 2026-present The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_CRYPTO_RANDOMX_UTIL_H
#define CONNECTCOIN_CRYPTO_RANDOMX_UTIL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

/** RandomX consensus algorithm variant.
 *
 * ConnectCoin consensus uses V2. Keeping the selection explicit prevents an
 * installed library update from silently changing the proof-of-work function.
 */
enum class RandomXAlgorithm : uint8_t {
    V2,
};

/** RandomX memory mode.
 *
 * LIGHT and FAST produce identical hashes. FAST initializes the full shared
 * dataset and is the default for both mining and block verification.
 */
enum class RandomXMemoryMode : uint8_t {
    LIGHT,
    FAST,
};

struct RandomXOptions {
    /** Try large pages for each allocation and fall back if unavailable. */
    bool try_large_pages{true};

    /** Use the JIT backend when supported. The interpreter is hash-equivalent. */
    bool use_jit{true};

    /** Enforce W^X for JIT pages when JIT is available. */
    bool secure_jit{true};

    /** Dataset initialization workers. Zero selects hardware concurrency. */
    unsigned dataset_init_threads{0};
};

/** A key-specific RandomX cache/dataset with a thread-safe pool of VMs.
 *
 * One context is intended to be shared by all hashing threads for an epoch.
 * Each concurrent Calculate() call leases a distinct VM, so the roughly 2 GiB
 * FAST dataset is never duplicated per mining or verification thread.
 */
class RandomXContext final
{
public:
    static constexpr size_t OUTPUT_SIZE{32};
    using Hash = std::array<std::byte, OUTPUT_SIZE>;

    RandomXContext(RandomXAlgorithm algorithm,
                   std::span<const std::byte> key,
                   RandomXMemoryMode memory_mode = RandomXMemoryMode::FAST,
                   RandomXOptions options = {});
    ~RandomXContext();

    RandomXContext(const RandomXContext&) = delete;
    RandomXContext& operator=(const RandomXContext&) = delete;
    RandomXContext(RandomXContext&&) noexcept;
    RandomXContext& operator=(RandomXContext&&) noexcept;

    /** Calculate a 256-bit RandomX hash. Safe to call concurrently. */
    [[nodiscard]] Hash Calculate(std::span<const std::byte> input) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // CONNECTCOIN_CRYPTO_RANDOMX_UTIL_H
