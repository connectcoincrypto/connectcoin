// Copyright (c) 2026-present The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <crypto/randomx_cache.h>
#include <crypto/randomx_util.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
constexpr auto WAIT_TIMEOUT{10s};
// Outlive both factory-entry waits plus the operation-under-test wait. An
// expired factory must not release a blocked operation and make it appear safe.
constexpr auto GATE_TIMEOUT{4 * WAIT_TIMEOUT};

// Fake contexts test admission/concurrency without allocating multi-GiB datasets.
// The real RandomX FAST/LIGHT hash-equivalence vectors live in randomx_tests.
struct FakeContext {
    uint256 key;
    RandomXMemoryMode mode;
    uint256 Calculate() const { return key; }
};

using ContextPtr = std::shared_ptr<const FakeContext>;
using Cache = RandomXContextCache<FakeContext>;

struct Factory {
    std::mutex mutex;
    std::vector<std::pair<uint256, RandomXMemoryMode>> allocations;
    std::function<void(const uint256&, RandomXMemoryMode)> before_return;

    ContextPtr Create(const uint256& key, RandomXMemoryMode mode)
    {
        {
            std::lock_guard lock{mutex};
            allocations.emplace_back(key, mode);
        }
        if (before_return) before_return(key, mode);
        return std::make_shared<const FakeContext>(FakeContext{key, mode});
    }

    size_t Count(RandomXMemoryMode mode)
    {
        std::lock_guard lock{mutex};
        size_t count{0};
        for (const auto& allocation : allocations) count += allocation.second == mode;
        return count;
    }
};

// Bounded waits ensure a broken cache implementation cannot leave the test
// process stuck in a factory forever. Release is idempotent and exception-safe.
struct Gate {
    std::promise<void> entered;
    std::future<void> entered_future{entered.get_future()};
    std::promise<void> release;
    std::shared_future<void> release_future{release.get_future().share()};
    std::once_flag released;

    ~Gate() { Open(); }
    void Open() { std::call_once(released, [&] { release.set_value(); }); }
    void Wait()
    {
        entered.set_value();
        if (release_future.wait_for(GATE_TIMEOUT) != std::future_status::ready) {
            throw std::runtime_error("RandomX cache test factory was not released");
        }
    }
};

bool WaitForFast(Cache& cache, const uint256& key)
{
    const auto deadline{std::chrono::steady_clock::now() + WAIT_TIMEOUT};
    do {
        if (cache.Get(key, true)->mode == RandomXMemoryMode::FAST) return true;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

ContextPtr WaitForPrepared(Cache& cache, const uint256& key)
{
    const auto deadline{std::chrono::steady_clock::now() + WAIT_TIMEOUT};
    do {
        if (auto context{cache.PeekPrepared(key)}) return context;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return {};
}

} // namespace

BOOST_AUTO_TEST_SUITE(randomx_cache_tests)

BOOST_AUTO_TEST_CASE(dataset_status_observation_does_not_allocate_or_refresh_lru)
{
    Factory factory;
    Cache cache{[&](const uint256& key, RandomXMemoryMode mode) { return factory.Create(key, mode); }};
    const uint256 first{1}, second{2}, third{3};
    for (int i{0}; i < 20; ++i) BOOST_CHECK(!cache.PeekPrepared(first));
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::LIGHT), 0U);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::FAST), 0U);
    cache.Prepare(first, RandomXMemoryMode::FAST);
    const auto original{WaitForPrepared(cache, first)};
    BOOST_REQUIRE(original);
    cache.Prepare(second, RandomXMemoryMode::FAST);
    BOOST_REQUIRE(WaitForPrepared(cache, second));
    for (int i{0}; i < 20; ++i) BOOST_CHECK(cache.PeekPrepared(first) == original);
    cache.Prepare(third, RandomXMemoryMode::FAST);
    BOOST_REQUIRE(WaitForPrepared(cache, third));
    BOOST_CHECK(!cache.PeekPrepared(first));
    BOOST_CHECK(cache.PeekPrepared(second));
    BOOST_CHECK(original->key == first);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::LIGHT), 0U);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::FAST), 3U);
}

BOOST_AUTO_TEST_CASE(dataset_status_observation_never_waits_for_preparation)
{
    Gate gate;
    Factory factory;
    factory.before_return = [&](const uint256&, RandomXMemoryMode mode) {
        if (mode == RandomXMemoryMode::FAST) gate.Wait();
    };
    Cache cache{[&](const uint256& key, RandomXMemoryMode mode) { return factory.Create(key, mode); }};
    const uint256 key{1};
    cache.Prepare(key, RandomXMemoryMode::FAST);
    const bool entered{gate.entered_future.wait_for(WAIT_TIMEOUT) == std::future_status::ready};
    if (!entered) gate.Open();
    BOOST_REQUIRE(entered);
    auto lookup{std::async(std::launch::async, [&] { return cache.PeekPrepared(key); })};
    const bool ready{lookup.wait_for(WAIT_TIMEOUT) == std::future_status::ready};
    gate.Open();
    BOOST_REQUIRE(ready);
    BOOST_CHECK(!lookup.get());
    BOOST_REQUIRE(WaitForPrepared(cache, key));
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::LIGHT), 0U);
}

BOOST_AUTO_TEST_CASE(dataset_status_observation_preserves_effective_light_fallback)
{
    std::atomic<unsigned> allocations{0};
    Cache cache{[&](const uint256& key, RandomXMemoryMode) -> ContextPtr {
        ++allocations;
        return std::make_shared<const FakeContext>(FakeContext{key, RandomXMemoryMode::LIGHT});
    }};
    const uint256 key{1};
    cache.Prepare(key, RandomXMemoryMode::FAST);
    const auto context{WaitForPrepared(cache, key)};
    BOOST_REQUIRE(context);
    BOOST_CHECK(context->mode == RandomXMemoryMode::LIGHT);
    BOOST_CHECK(cache.PeekPrepared(key) == context);
    BOOST_CHECK(!cache.PeekPrepared(uint256{2}));
    BOOST_CHECK_EQUAL(allocations.load(), 1U);
}

BOOST_AUTO_TEST_CASE(cold_get_only_builds_and_reuses_light)
{
    Factory factory;
    Cache cache{[&](const uint256& key, RandomXMemoryMode mode) { return factory.Create(key, mode); }};
    const uint256 key{1};
    const auto first{cache.Get(key, true)};
    BOOST_CHECK(first->mode == RandomXMemoryMode::LIGHT);
    BOOST_CHECK(first->key == key);
    for (int i{0}; i < 20; ++i) {
        BOOST_CHECK(cache.Get(key, true) == first);
        BOOST_CHECK(cache.Get(key, false) == first);
    }
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::LIGHT), 1U);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::FAST), 0U);

    // LIGHT preparation is not a remote-key cache-warming allocation path.
    cache.Prepare(uint256{2}, RandomXMemoryMode::LIGHT);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::LIGHT), 1U);
    const auto other{cache.Get(uint256{2}, true)};
    BOOST_CHECK(other->mode == RandomXMemoryMode::LIGHT);
    BOOST_CHECK(other != first);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::FAST), 0U);
    // Eviction does not invalidate contexts leased by concurrent callers.
    BOOST_CHECK(first->Calculate() == key);
}

BOOST_AUTO_TEST_CASE(ready_fast_is_optional_and_hash_equivalent)
{
    Factory factory;
    Cache cache{[&](const uint256& key, RandomXMemoryMode mode) { return factory.Create(key, mode); }};
    const uint256 key{1};
    const auto light{cache.Get(key, false)};
    cache.Prepare(key, RandomXMemoryMode::FAST);
    BOOST_REQUIRE(WaitForFast(cache, key));
    const auto fast{cache.Get(key, true)};
    BOOST_CHECK(fast->mode == RandomXMemoryMode::FAST);
    BOOST_CHECK(fast != light);
    BOOST_CHECK(fast->Calculate() == light->Calculate());
    BOOST_CHECK(cache.Get(key, false) == light);
    cache.Prepare(key, RandomXMemoryMode::FAST);
    BOOST_CHECK(cache.Get(key, true) == fast);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::FAST), 1U);
}

BOOST_AUTO_TEST_CASE(remote_light_churn_preserves_prepared_fast)
{
    Factory factory;
    Cache cache{[&](const uint256& key, RandomXMemoryMode mode) { return factory.Create(key, mode); }};
    const uint256 first_key{1}, second_key{2};
    cache.Prepare(first_key, RandomXMemoryMode::FAST);
    BOOST_REQUIRE(WaitForFast(cache, first_key));
    const auto first{cache.Get(first_key, true)};
    cache.Prepare(second_key, RandomXMemoryMode::FAST);
    BOOST_REQUIRE(WaitForFast(cache, second_key));
    const auto second{cache.Get(second_key, true)};
    for (uint8_t i{3}; i < 30; ++i) {
        BOOST_CHECK(cache.Get(uint256{i}, true)->mode == RandomXMemoryMode::LIGHT);
    }
    BOOST_CHECK(cache.Get(first_key, true) == first);
    BOOST_CHECK(cache.Get(second_key, true) == second);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::FAST), 2U);
}

BOOST_AUTO_TEST_CASE(remote_fast_lookup_does_not_refresh_eviction_order)
{
    Factory factory;
    Cache cache{[&](const uint256& key, RandomXMemoryMode mode) { return factory.Create(key, mode); }};
    const uint256 first_key{1}, second_key{2}, third_key{3};
    cache.Prepare(first_key, RandomXMemoryMode::FAST);
    BOOST_REQUIRE(WaitForFast(cache, first_key));
    cache.Prepare(second_key, RandomXMemoryMode::FAST);
    BOOST_REQUIRE(WaitForFast(cache, second_key));
    const auto first{cache.Get(first_key, true)};
    const auto second{cache.Get(second_key, true)};
    for (int i{0}; i < 20; ++i) BOOST_CHECK(cache.Get(first_key, true) == first);
    cache.Prepare(third_key, RandomXMemoryMode::FAST);
    BOOST_REQUIRE(WaitForFast(cache, third_key));
    BOOST_CHECK(cache.Get(first_key, true)->mode == RandomXMemoryMode::LIGHT);
    BOOST_CHECK(cache.Get(second_key, true) == second);
    BOOST_CHECK(first->Calculate() == first_key);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::FAST), 3U);
}

BOOST_AUTO_TEST_CASE(explicit_preparation_refreshes_eviction_order)
{
    Factory factory;
    Cache cache{[&](const uint256& key, RandomXMemoryMode mode) { return factory.Create(key, mode); }};
    const uint256 first_key{1}, second_key{2}, third_key{3};
    cache.Prepare(first_key, RandomXMemoryMode::FAST);
    BOOST_REQUIRE(WaitForFast(cache, first_key));
    cache.Prepare(second_key, RandomXMemoryMode::FAST);
    BOOST_REQUIRE(WaitForFast(cache, second_key));
    const auto first{cache.Get(first_key, true)};
    cache.Prepare(first_key, RandomXMemoryMode::FAST);
    cache.Prepare(third_key, RandomXMemoryMode::FAST);
    BOOST_REQUIRE(WaitForFast(cache, third_key));
    BOOST_CHECK(cache.Get(first_key, true) == first);
    BOOST_CHECK(cache.Get(second_key, true)->mode == RandomXMemoryMode::LIGHT);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::FAST), 3U);
}

BOOST_AUTO_TEST_CASE(failed_fast_falls_back_without_automatic_rebuild)
{
    Factory factory;
    std::atomic<unsigned> attempts{0};
    std::promise<void> failed;
    auto failed_future{failed.get_future()};
    factory.before_return = [&](const uint256&, RandomXMemoryMode mode) {
        if (mode == RandomXMemoryMode::FAST && attempts.fetch_add(1) == 0) {
            failed.set_value();
            throw std::runtime_error("simulated FAST allocation failure");
        }
    };
    Cache cache{[&](const uint256& key, RandomXMemoryMode mode) { return factory.Create(key, mode); }};
    const uint256 key{1};
    cache.Prepare(key, RandomXMemoryMode::FAST);
    BOOST_REQUIRE(failed_future.wait_for(WAIT_TIMEOUT) == std::future_status::ready);
    const auto light{cache.Get(key, true)};
    BOOST_CHECK(light->mode == RandomXMemoryMode::LIGHT);
    for (int i{0}; i < 20; ++i) BOOST_CHECK(cache.Get(key, true) == light);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::FAST), 1U);

    // The promise above signals just before the exception reaches std::async.
    // Retry explicit preparation until that future has become ready/was removed.
    const auto deadline{std::chrono::steady_clock::now() + WAIT_TIMEOUT};
    bool ready{false};
    do {
        cache.Prepare(key, RandomXMemoryMode::FAST);
        if (cache.Get(key, true)->mode == RandomXMemoryMode::FAST) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    BOOST_CHECK(ready);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::FAST), 2U);
}

BOOST_AUTO_TEST_CASE(failed_light_initialization_can_be_retried)
{
    Factory factory;
    const uint256 first_key{1}, second_key{2};
    bool fail{true};
    factory.before_return = [&](const uint256& key, RandomXMemoryMode mode) {
        if (key == second_key && mode == RandomXMemoryMode::LIGHT && std::exchange(fail, false)) {
            throw std::runtime_error("simulated LIGHT allocation failure");
        }
    };
    Cache cache{[&](const uint256& key, RandomXMemoryMode mode) { return factory.Create(key, mode); }};
    const auto first{cache.Get(first_key, true)};
    BOOST_CHECK_THROW(cache.Get(second_key, true), std::runtime_error);
    const auto second{cache.Get(second_key, true)};
    BOOST_CHECK(second->key == second_key);
    BOOST_CHECK(second->mode == RandomXMemoryMode::LIGHT);
    BOOST_CHECK(first->Calculate() == first_key);
    BOOST_CHECK(cache.Get(second_key, true) == second);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::LIGHT), 3U);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::FAST), 0U);
}

BOOST_AUTO_TEST_CASE(pending_fast_does_not_block_light_lookup)
{
    Gate gate;
    Factory factory;
    factory.before_return = [&](const uint256&, RandomXMemoryMode mode) {
        if (mode == RandomXMemoryMode::FAST) gate.Wait();
    };
    Cache cache{[&](const uint256& key, RandomXMemoryMode mode) { return factory.Create(key, mode); }};
    const uint256 key{1};
    cache.Prepare(key, RandomXMemoryMode::FAST);
    const bool entered{gate.entered_future.wait_for(WAIT_TIMEOUT) == std::future_status::ready};
    if (!entered) gate.Open();
    BOOST_REQUIRE(entered);
    auto lookup{std::async(std::launch::async, [&] { return cache.Get(key, true); })};
    const bool ready{lookup.wait_for(WAIT_TIMEOUT) == std::future_status::ready};
    // Always release before assertions/future destruction, including regressions.
    gate.Open();
    BOOST_REQUIRE(ready);
    BOOST_CHECK(lookup.get()->mode == RandomXMemoryMode::LIGHT);
    BOOST_REQUIRE(WaitForFast(cache, key));
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::FAST), 1U);
}

BOOST_AUTO_TEST_CASE(two_pending_fast_builds_bound_preparation_and_allow_retry)
{
    Gate first_gate, second_gate;
    Factory factory;
    const uint256 first_key{1}, second_key{2}, third_key{3};
    factory.before_return = [&](const uint256& key, RandomXMemoryMode mode) {
        if (mode != RandomXMemoryMode::FAST) return;
        if (key == first_key) first_gate.Wait();
        if (key == second_key) second_gate.Wait();
    };
    Cache cache{[&](const uint256& key, RandomXMemoryMode mode) { return factory.Create(key, mode); }};
    cache.Prepare(first_key, RandomXMemoryMode::FAST);
    cache.Prepare(second_key, RandomXMemoryMode::FAST);
    const bool first_entered{first_gate.entered_future.wait_for(WAIT_TIMEOUT) == std::future_status::ready};
    const bool second_entered{second_gate.entered_future.wait_for(WAIT_TIMEOUT) == std::future_status::ready};
    if (!first_entered || !second_entered) {
        first_gate.Open();
        second_gate.Open();
    }
    BOOST_REQUIRE(first_entered && second_entered);
    auto prepare{std::async(std::launch::async, [&] {
        cache.Prepare(first_key, RandomXMemoryMode::FAST);
        cache.Prepare(third_key, RandomXMemoryMode::FAST);
    })};
    const bool ready{prepare.wait_for(WAIT_TIMEOUT) == std::future_status::ready};
    const auto builds_while_pending{factory.Count(RandomXMemoryMode::FAST)};
    first_gate.Open();
    second_gate.Open();
    BOOST_REQUIRE(ready);
    prepare.get();
    BOOST_CHECK_EQUAL(builds_while_pending, 2U);
    BOOST_REQUIRE(WaitForFast(cache, first_key));
    BOOST_REQUIRE(WaitForFast(cache, second_key));
    // A skipped request was not retained as an unbounded background queue.
    BOOST_CHECK(cache.Get(third_key, true)->mode == RandomXMemoryMode::LIGHT);
    cache.Prepare(third_key, RandomXMemoryMode::FAST);
    BOOST_REQUIRE(WaitForFast(cache, third_key));
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::FAST), 3U);
}

BOOST_AUTO_TEST_CASE(light_cache_initializations_are_serialized)
{
    Gate first_gate;
    Factory factory;
    const uint256 first_key{1}, second_key{2};
    std::atomic<unsigned> active{0}, max_active{0};
    std::promise<void> second_entered;
    auto second_entered_future{second_entered.get_future()};
    factory.before_return = [&](const uint256& key, RandomXMemoryMode mode) {
        if (mode != RandomXMemoryMode::LIGHT) return;
        const auto now{++active};
        auto previous{max_active.load()};
        while (previous < now && !max_active.compare_exchange_weak(previous, now)) {}
        if (key == first_key) first_gate.Wait();
        if (key == second_key) second_entered.set_value();
        --active;
    };
    Cache cache{[&](const uint256& key, RandomXMemoryMode mode) { return factory.Create(key, mode); }};
    auto first{std::async(std::launch::async, [&] { return cache.Get(first_key, true); })};
    const bool entered{first_gate.entered_future.wait_for(WAIT_TIMEOUT) == std::future_status::ready};
    if (!entered) first_gate.Open();
    BOOST_REQUIRE(entered);
    std::promise<void> second_requested;
    auto requested{second_requested.get_future()};
    auto second{std::async(std::launch::async, [&] {
        second_requested.set_value();
        return cache.Get(second_key, true);
    })};
    const bool request_started{requested.wait_for(WAIT_TIMEOUT) == std::future_status::ready};
    const bool parallel_factory{second_entered_future.wait_for(100ms) == std::future_status::ready};
    first_gate.Open();
    BOOST_REQUIRE(request_started);
    BOOST_CHECK(!parallel_factory);
    BOOST_REQUIRE(first.wait_for(WAIT_TIMEOUT) == std::future_status::ready);
    BOOST_REQUIRE(second.wait_for(WAIT_TIMEOUT) == std::future_status::ready);
    BOOST_CHECK(first.get()->key == first_key);
    BOOST_CHECK(second.get()->key == second_key);
    BOOST_CHECK_EQUAL(max_active.load(), 1U);
    BOOST_CHECK_EQUAL(factory.Count(RandomXMemoryMode::FAST), 0U);
}

BOOST_AUTO_TEST_SUITE_END()
