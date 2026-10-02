// Copyright (c) 2026-present The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <crypto/hex_base.h>
#include <crypto/randomx_util.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cfenv>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <span>
#include <string_view>

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#endif

BOOST_AUTO_TEST_SUITE(randomx_tests)

namespace {

bool UseReducedRandomXCoverage()
{
    return std::getenv("TEST_RANDOMX_MOCK_POW") != nullptr;
}

template <typename Operation>
void CheckFloatingPointPreserved(Operation operation)
{
    struct RestoreEnvironment {
        std::fenv_t environment{};
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
        unsigned mxcsr{_mm_getcsr()};
#endif
        RestoreEnvironment() { std::fegetenv(&environment); }
        ~RestoreEnvironment()
        {
            std::fesetenv(&environment);
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
            _mm_setcsr(mxcsr);
#endif
        }
    } restore;
    BOOST_REQUIRE_EQUAL(std::fesetround(FE_DOWNWARD), 0);
    std::feraiseexcept(FE_INEXACT);
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
    _mm_setcsr(_mm_getcsr() | _MM_FLUSH_ZERO_ON);
    const auto before_mxcsr{_mm_getcsr()};
#endif
    const int before_round{std::fegetround()};
    const int before_exceptions{std::fetestexcept(FE_ALL_EXCEPT)};
    operation();
    const int after_round{std::fegetround()};
    const int after_exceptions{std::fetestexcept(FE_ALL_EXCEPT)};
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
    const auto after_mxcsr{_mm_getcsr()};
    BOOST_CHECK_EQUAL(after_mxcsr, before_mxcsr);
#endif
    BOOST_CHECK_EQUAL(after_round, before_round);
    BOOST_CHECK_EQUAL(after_exceptions, before_exceptions);
}

} // namespace

BOOST_AUTO_TEST_CASE(v2_reference_vector_light)
{
    constexpr std::string_view key{"test key 000"};
    constexpr std::string_view input{"This is a test"};
    const RandomXOptions options{
        .try_large_pages = false,
        .use_jit = true,
        .secure_jit = true,
        .dataset_init_threads = 1,
    };
    const RandomXContext context{
        RandomXAlgorithm::V2,
        std::as_bytes(std::span{key}),
        RandomXMemoryMode::LIGHT,
        options,
    };

    const auto hash{context.Calculate(std::as_bytes(std::span{input}))};
    BOOST_CHECK(context.MemoryMode() == RandomXMemoryMode::LIGHT);
    BOOST_CHECK(!context.UsesLargePagesForDataset());
    BOOST_CHECK_EQUAL(HexStr(hash), "22ec6b861b3eb23686b2efbad69513c967ecfce80983df66c9c5b4fbfb4cdb6f");
    RandomXContext::Hash pooled{};
    // Cover both the first mining-policy lease and its return/reuse without
    // retaining an unused VM or adding another large cache to the test.
    for (int i{0}; i < 2; ++i) {
        CheckFloatingPointPreserved([&] { pooled = context.Calculate(std::as_bytes(std::span{input}), false); });
        BOOST_CHECK(pooled == hash);
    }
}

BOOST_AUTO_TEST_CASE(v2_reference_vector_interpreter)
{
    if (UseReducedRandomXCoverage()) {
        BOOST_TEST_MESSAGE("Skipping the redundant interpreter vector in the reduced RandomX CI profile");
        return;
    }

    constexpr std::string_view key{"test key 000"};
    constexpr std::string_view input{"This is a test"};
    const RandomXOptions options{
        .try_large_pages = false,
        .use_jit = false,
        .secure_jit = true,
        .dataset_init_threads = 1,
    };
    const RandomXContext context{
        RandomXAlgorithm::V2,
        std::as_bytes(std::span{key}),
        RandomXMemoryMode::LIGHT,
        options,
    };

    const auto hash{context.Calculate(std::as_bytes(std::span{input}))};
    BOOST_CHECK_EQUAL(HexStr(hash), "22ec6b861b3eb23686b2efbad69513c967ecfce80983df66c9c5b4fbfb4cdb6f");
}

BOOST_AUTO_TEST_CASE(v2_secure_and_nonsecure_jit_share_context)
{
    constexpr std::string_view key{"test key 000"};
    constexpr std::string_view input{"This is a test"};
    const auto bytes{std::as_bytes(std::span{input})};
    const RandomXOptions options{
        .try_large_pages = false,
        .secure_jit = false,
        .dataset_init_threads = 1,
    };
    // Validation keeps the secure default; miners opt out where permitted.
    // Mandatory backend protection (e.g. Apple Silicon) still overrides false.
    BOOST_CHECK(RandomXOptions{}.secure_jit);
    BOOST_CHECK(options.use_jit);
    BOOST_CHECK(!options.secure_jit);
    const RandomXContext context{RandomXAlgorithm::V2, std::as_bytes(std::span{key}), RandomXMemoryMode::LIGHT, options};
    const auto expected{context.Calculate(bytes)};
    BOOST_CHECK_EQUAL(HexStr(expected), "22ec6b861b3eb23686b2efbad69513c967ecfce80983df66c9c5b4fbfb4cdb6f");
    // Explicit opt-in/out policies remain hash-equivalent and may run together.
    for (int i{0}; i < 2; ++i) {
        auto nonsecure{std::async(std::launch::async, [&] { return context.Calculate(bytes, false); })};
        auto secure{std::async(std::launch::async, [&] { return context.Calculate(bytes, true); })};
        BOOST_CHECK(nonsecure.get() == expected);
        BOOST_CHECK(secure.get() == expected);
        BOOST_CHECK(context.Calculate(bytes) == expected);
    }
}

#ifdef ENABLE_RANDOMX_FAST_TEST
BOOST_AUTO_TEST_CASE(v2_fast_large_pages_preference_preserves_hash)
{
    constexpr std::string_view key{"test key 000"};
    constexpr std::string_view input{"This is a test"};
    const RandomXContext fast{
        RandomXAlgorithm::V2, std::as_bytes(std::span{key}), RandomXMemoryMode::FAST,
        RandomXOptions{.try_large_pages = true, .secure_jit = false},
    };
    BOOST_CHECK(fast.MemoryMode() == RandomXMemoryMode::FAST);
    const bool large_pages{fast.UsesLargePagesForDataset()};
    BOOST_TEST_MESSAGE("FAST dataset large-page allocation succeeded: " << large_pages);
    // Both the successful preferred allocation and normal-page fallback must
    // remain usable and hash-equivalent on machines without huge-page rights.
    const auto hash{fast.Calculate(std::as_bytes(std::span{input}), false)};
    BOOST_CHECK_EQUAL(HexStr(hash), "22ec6b861b3eb23686b2efbad69513c967ecfce80983df66c9c5b4fbfb4cdb6f");
    BOOST_CHECK_EQUAL(fast.UsesLargePagesForDataset(), large_pages);
}

BOOST_AUTO_TEST_CASE(v2_reference_vector_fast_matches_light)
{
    constexpr std::string_view key{"test key 000"};
    constexpr std::string_view input{"This is a test"};
    const RandomXOptions options{
        .try_large_pages = false,
        .secure_jit = false,
        .dataset_init_threads = 0,
    };
    BOOST_CHECK(!options.secure_jit);

    RandomXContext::Hash light_hash;
    {
        const RandomXContext light{RandomXAlgorithm::V2, std::as_bytes(std::span{key}), RandomXMemoryMode::LIGHT, options};
        light_hash = light.Calculate(std::as_bytes(std::span{input}));
    }
    const RandomXContext fast{RandomXAlgorithm::V2, std::as_bytes(std::span{key}), RandomXMemoryMode::FAST, options};
    BOOST_CHECK(fast.MemoryMode() == RandomXMemoryMode::FAST);
    BOOST_CHECK(!fast.UsesLargePagesForDataset());
    const auto fast_hash{fast.Calculate(std::as_bytes(std::span{input}))};
    const auto nonsecure_hash{fast.Calculate(std::as_bytes(std::span{input}), false)};

    BOOST_CHECK(fast_hash == light_hash);
    BOOST_CHECK(nonsecure_hash == fast_hash);
    BOOST_CHECK(fast.Calculate(std::as_bytes(std::span{input}), true) == fast_hash);
    BOOST_CHECK_EQUAL(HexStr(fast_hash), "22ec6b861b3eb23686b2efbad69513c967ecfce80983df66c9c5b4fbfb4cdb6f");
}
#endif

BOOST_AUTO_TEST_CASE(v2_context_reuse)
{
    if (UseReducedRandomXCoverage()) {
        BOOST_TEST_MESSAGE("Skipping redundant context reuse in the reduced RandomX CI profile");
        return;
    }

    constexpr std::array<std::byte, 4> key{std::byte{'k'}, std::byte{'e'}, std::byte{'y'}, std::byte{'2'}};
    constexpr std::array<std::byte, 5> input{std::byte{'b'}, std::byte{'l'}, std::byte{'o'}, std::byte{'c'}, std::byte{'k'}};
    const RandomXOptions options{
        .try_large_pages = false,
        .use_jit = true,
        .secure_jit = true,
        .dataset_init_threads = 1,
    };
    const RandomXContext context{RandomXAlgorithm::V2, key, RandomXMemoryMode::LIGHT, options};

    const auto first{context.Calculate(input)};
    BOOST_CHECK(first == context.Calculate(input));
}

BOOST_AUTO_TEST_SUITE_END()
