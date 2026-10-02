// Copyright (c) 2026-present The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <crypto/randomx_util.h>

#include <randomx.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

static_assert(RANDOMX_HASH_SIZE == RandomXContext::OUTPUT_SIZE);

namespace {

randomx_flags AddFlag(randomx_flags flags, randomx_flags flag)
{
    return static_cast<randomx_flags>(static_cast<unsigned>(flags) | static_cast<unsigned>(flag));
}

randomx_flags RemoveFlag(randomx_flags flags, randomx_flags flag)
{
    return static_cast<randomx_flags>(static_cast<unsigned>(flags) & ~static_cast<unsigned>(flag));
}

bool HasFlag(randomx_flags flags, randomx_flags flag)
{
    return (static_cast<unsigned>(flags) & static_cast<unsigned>(flag)) != 0;
}

constexpr bool IsAddressSanitizerBuild()
{
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

struct CacheDeleter {
    void operator()(randomx_cache* cache) const
    {
        if (cache != nullptr) randomx_release_cache(cache);
    }
};

struct DatasetDeleter {
    void operator()(randomx_dataset* dataset) const
    {
        if (dataset != nullptr) randomx_release_dataset(dataset);
    }
};

struct VMDeleter {
    void operator()(randomx_vm* vm) const
    {
        if (vm != nullptr) randomx_destroy_vm(vm);
    }
};

using CachePtr = std::unique_ptr<randomx_cache, CacheDeleter>;
using DatasetPtr = std::unique_ptr<randomx_dataset, DatasetDeleter>;
using VMPtr = std::unique_ptr<randomx_vm, VMDeleter>;

randomx_flags MakeBaseFlags(RandomXAlgorithm algorithm, RandomXMemoryMode memory_mode, const RandomXOptions& options)
{
    randomx_flags flags{randomx_get_flags()};
    const bool require_secure_jit{HasFlag(flags, RANDOMX_FLAG_SECURE)};
    flags = RemoveFlag(flags, RANDOMX_FLAG_V2);
    flags = RemoveFlag(flags, RANDOMX_FLAG_FULL_MEM);
    flags = RemoveFlag(flags, RANDOMX_FLAG_LARGE_PAGES);
    flags = RemoveFlag(flags, RANDOMX_FLAG_SECURE);

    // RandomX's generated machine code has crashed ASan while executing a
    // LIGHT VM. Use the consensus-equivalent interpreter only with ASan;
    // disabling JIT for TSan and MSan makes their real-hash smoke tests
    // prohibitively slow without improving coverage of generated code.
    if (!options.use_jit || IsAddressSanitizerBuild()) {
        flags = RemoveFlag(flags, RANDOMX_FLAG_JIT);
    }

#if defined(_MSC_VER) && defined(_DEBUG)
    // MSVC's debug STL changes randomx_cache's layout. Upstream documents
    // that its JIT is unsafe with that layout; the interpreter is
    // consensus-equivalent and keeps Debug builds usable.
    flags = RemoveFlag(flags, RANDOMX_FLAG_JIT);
#endif

    if (algorithm != RandomXAlgorithm::V2) {
        throw std::invalid_argument("Unknown RandomX algorithm variant");
    }
    flags = AddFlag(flags, RANDOMX_FLAG_V2);

    if (memory_mode == RandomXMemoryMode::FAST) {
        flags = AddFlag(flags, RANDOMX_FLAG_FULL_MEM);
    } else if (memory_mode != RandomXMemoryMode::LIGHT) {
        throw std::invalid_argument("Unknown RandomX memory mode");
    }

    if ((options.secure_jit || require_secure_jit) && HasFlag(flags, RANDOMX_FLAG_JIT)) {
        flags = AddFlag(flags, RANDOMX_FLAG_SECURE);
    }
    return flags;
}

} // namespace

struct RandomXContext::Impl {
    const RandomXMemoryMode m_memory_mode;
    const RandomXOptions m_options;
    const randomx_flags m_base_flags;
    const bool m_require_secure_jit{HasFlag(randomx_get_flags(), RANDOMX_FLAG_SECURE)};
    CachePtr m_cache;
    DatasetPtr m_dataset;
    bool m_dataset_large_pages{false};
    mutable std::mutex m_vm_mutex;
    mutable std::array<std::vector<VMPtr>, 2> m_vm_pools;

    Impl(RandomXAlgorithm algorithm,
         std::span<const std::byte> key,
         RandomXMemoryMode memory_mode,
         RandomXOptions options)
        : m_memory_mode{memory_mode},
          m_options{options},
          m_base_flags{MakeBaseFlags(algorithm, memory_mode, options)}
    {
        if (key.empty()) throw std::invalid_argument("RandomX key must not be empty");
        AllocateCache();
        randomx_init_cache(m_cache.get(), key.data(), key.size());

        if (m_memory_mode == RandomXMemoryMode::FAST) {
            AllocateDataset();
            InitializeDataset();
            // FAST VMs use only the initialized dataset.
            m_cache.reset();
        }
    }

    randomx_flags PreferredFlags() const
    {
        return m_options.try_large_pages ? AddFlag(m_base_flags, RANDOMX_FLAG_LARGE_PAGES) : m_base_flags;
    }

    randomx_flags PortableFlags() const
    {
        return RemoveFlag(RemoveFlag(m_base_flags, RANDOMX_FLAG_JIT), RANDOMX_FLAG_SECURE);
    }

    void AllocateCache()
    {
        m_cache.reset(randomx_alloc_cache(PreferredFlags()));
        if (!m_cache && m_options.try_large_pages) {
            m_cache.reset(randomx_alloc_cache(m_base_flags));
        }
        // Hardened runtimes (notably signed macOS applications) may prohibit
        // executable JIT pages. The interpreter is consensus-equivalent.
        if (!m_cache && HasFlag(m_base_flags, RANDOMX_FLAG_JIT)) {
            m_cache.reset(randomx_alloc_cache(PortableFlags()));
        }
        if (!m_cache) throw std::runtime_error("RandomX cache allocation failed");
    }

    void AllocateDataset()
    {
        m_dataset.reset(randomx_alloc_dataset(PreferredFlags()));
        m_dataset_large_pages = m_dataset != nullptr && m_options.try_large_pages;
        if (!m_dataset && m_options.try_large_pages) {
            m_dataset.reset(randomx_alloc_dataset(m_base_flags));
        }
        if (!m_dataset) throw std::runtime_error("RandomX dataset allocation failed");
    }

    void InitializeDataset()
    {
        const unsigned long item_count{randomx_dataset_item_count()};
        const unsigned requested_threads{m_options.dataset_init_threads != 0
                                             ? m_options.dataset_init_threads
                                             : std::max(1U, std::thread::hardware_concurrency())};
        const unsigned long thread_count{std::min<unsigned long>(requested_threads, item_count)};

        if (thread_count == 1) {
            randomx_init_dataset(m_dataset.get(), m_cache.get(), 0, item_count);
            return;
        }

        std::vector<std::thread> workers;
        workers.reserve(thread_count);
        try {
            for (unsigned long thread{0}; thread < thread_count; ++thread) {
                const unsigned long items_per_thread{item_count / thread_count};
                const unsigned long extra_items{item_count % thread_count};
                const unsigned long begin{items_per_thread * thread + std::min(thread, extra_items)};
                const unsigned long count{items_per_thread + (thread < extra_items ? 1UL : 0UL)};
                workers.emplace_back([this, begin, count] {
                    randomx_init_dataset(m_dataset.get(), m_cache.get(), begin, count);
                });
            }
        } catch (...) {
            for (auto& worker : workers) worker.join();
            throw;
        }
        for (auto& worker : workers) worker.join();
    }

    VMPtr CreateVM(bool secure_jit) const
    {
        const auto create = [this](randomx_flags flags) {
            return VMPtr{randomx_create_vm(flags,
                                           m_memory_mode == RandomXMemoryMode::LIGHT ? m_cache.get() : nullptr,
                                           m_memory_mode == RandomXMemoryMode::FAST ? m_dataset.get() : nullptr)};
        };

        randomx_flags flags{RemoveFlag(m_base_flags, RANDOMX_FLAG_SECURE)};
        if (secure_jit && HasFlag(flags, RANDOMX_FLAG_JIT)) {
            flags = AddFlag(flags, RANDOMX_FLAG_SECURE);
        }
        VMPtr vm{create(m_options.try_large_pages ? AddFlag(flags, RANDOMX_FLAG_LARGE_PAGES) : flags)};
        if (!vm && m_options.try_large_pages) vm = create(flags);
        if (!vm && HasFlag(m_base_flags, RANDOMX_FLAG_JIT)) vm = create(PortableFlags());
        if (!vm) throw std::runtime_error("RandomX virtual machine creation failed");
        return vm;
    }

    RandomXContext::Hash Calculate(std::span<const std::byte> input, bool secure_jit) const
    {
        // Some supported platforms prohibit writable/executable JIT pages.
        secure_jit = secure_jit || m_require_secure_jit;
        auto& pool{m_vm_pools[secure_jit ? 1 : 0]};
        VMPtr vm;
        {
            std::lock_guard lock{m_vm_mutex};
            if (!pool.empty()) {
                vm = std::move(pool.back());
                pool.pop_back();
            }
        }
        if (!vm) vm = CreateVM(secure_jit);

        RandomXContext::Hash result{};
        randomx_calculate_hash(vm.get(), input.data(), input.size(), result.data());

        {
            std::lock_guard lock{m_vm_mutex};
            pool.push_back(std::move(vm));
        }
        return result;
    }
};

RandomXContext::RandomXContext(RandomXAlgorithm algorithm,
                               std::span<const std::byte> key,
                               RandomXMemoryMode memory_mode,
                               RandomXOptions options)
    : m_impl{std::make_unique<Impl>(algorithm, key, memory_mode, options)}
{
}

RandomXContext::~RandomXContext() = default;
RandomXContext::RandomXContext(RandomXContext&&) noexcept = default;
RandomXContext& RandomXContext::operator=(RandomXContext&&) noexcept = default;

RandomXContext::Hash RandomXContext::Calculate(std::span<const std::byte> input) const
{
    return m_impl->Calculate(input, m_impl->m_options.secure_jit);
}

RandomXContext::Hash RandomXContext::Calculate(std::span<const std::byte> input, bool secure_jit) const
{
    return m_impl->Calculate(input, secure_jit);
}

RandomXMemoryMode RandomXContext::MemoryMode() const
{
    return m_impl->m_memory_mode;
}

bool RandomXContext::UsesLargePagesForDataset() const
{
    return m_impl->m_dataset_large_pages;
}
