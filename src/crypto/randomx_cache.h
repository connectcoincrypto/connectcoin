// Copyright (c) 2026-present The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_CRYPTO_RANDOMX_CACHE_H
#define CONNECTCOIN_CRYPTO_RANDOMX_CACHE_H

#include <crypto/randomx_util.h>
#include <uint256.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

/** Separate trusted FAST preparation from untrusted, on-demand LIGHT hashing.
 * Context is a template parameter so admission/concurrency tests need not
 * allocate multi-gigabyte datasets. Factories must be safe to call concurrently.
 */
template <typename Context>
class RandomXContextCache
{
public:
    using ContextPtr = std::shared_ptr<const Context>;
    using Factory = std::function<ContextPtr(const uint256&, RandomXMemoryMode)>;

    explicit RandomXContextCache(Factory factory) : m_factory{std::move(factory)} {}

    ContextPtr Get(const uint256& key, bool prefer_fast)
    {
        if (prefer_fast) {
            std::lock_guard lock{m_fast_mutex};
            const auto it{FindFast(key)};
            if (it != m_fast.end() && Ready(*it)) {
                try {
                    // A hash request must not affect trusted preparation order.
                    return it->context.get();
                } catch (...) {
                    // Do not retry expensive preparation on a hash request.
                    m_fast.erase(it);
                }
            }
        }

        // Never wait for a FAST build or promote a cold key to FAST. Keep one
        // LIGHT entry, independent of the current/next FAST datasets. Serialize
        // initialization rather than spawning tasks for peer-selected keys.
        std::lock_guard lock{m_light_mutex};
        if (!m_light || key != m_light_key) {
            m_light.reset();
            m_light = m_factory(key, RandomXMemoryMode::LIGHT);
            m_light_key = key;
        }
        // In-flight hashes may retain an evicted context until they finish.
        return m_light;
    }

    /** Only active-chain updates and explicit local mining may call Prepare.
     * LIGHT has no speculative preparation. FAST preparation never waits for
     * an existing build, and admits at most two outstanding builds/entries.
     */
    void Prepare(const uint256& key, RandomXMemoryMode mode)
    {
        if (mode != RandomXMemoryMode::FAST) return;
        std::lock_guard lock{m_fast_mutex};
        const auto existing{FindFast(key)};
        if (existing != m_fast.end()) {
            existing->last_prepare = ++m_clock;
            return;
        }
        if (m_fast.size() == MAX_FAST_CONTEXTS) {
            auto oldest{m_fast.end()};
            for (auto it{m_fast.begin()}; it != m_fast.end(); ++it) {
                if (Ready(*it) && (oldest == m_fast.end() || it->last_prepare < oldest->last_prepare)) oldest = it;
            }
            // Never evict an unfinished std::async future: its destructor can
            // wait, and retiring unlimited futures permits unbounded builds.
            // A later active-tip/miner update can retry this preparation.
            if (oldest == m_fast.end()) return;
            m_fast.erase(oldest);
        }
        auto future{std::async(std::launch::async, [factory = m_factory, key] {
            return factory(key, RandomXMemoryMode::FAST);
        }).share()};
        m_fast.push_back({key, ++m_clock, std::move(future)});
    }

private:
    struct Entry {
        uint256 key;
        uint64_t last_prepare;
        std::shared_future<ContextPtr> context;
    };
    static constexpr size_t MAX_FAST_CONTEXTS{2};

    const Factory m_factory;
    std::mutex m_fast_mutex;
    uint64_t m_clock{0};
    std::vector<Entry> m_fast;
    std::mutex m_light_mutex;
    uint256 m_light_key;
    ContextPtr m_light;

    auto FindFast(const uint256& key)
    {
        return std::find_if(m_fast.begin(), m_fast.end(), [&](const auto& entry) { return entry.key == key; });
    }

    static bool Ready(const Entry& entry)
    {
        return entry.context.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
    }
};

#endif // CONNECTCOIN_CRYPTO_RANDOMX_CACHE_H
