// Copyright (c) 2026-present The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <crypto/randomx_util.h>
#include <crypto/hex_base.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

BOOST_AUTO_TEST_SUITE(randomx_tests)

BOOST_AUTO_TEST_CASE(v2_reference_vector_light)
{
    constexpr std::string_view key{"test key 000"};
    constexpr std::string_view input{"This is a test"};
    const RandomXOptions options{
        .try_large_pages = false,
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

#ifdef ENABLE_RANDOMX_FAST_TEST
BOOST_AUTO_TEST_CASE(v2_reference_vector_fast_matches_light)
{
    constexpr std::string_view key{"test key 000"};
    constexpr std::string_view input{"This is a test"};
    const RandomXOptions options{
        .try_large_pages = false,
        .secure_jit = true,
        .dataset_init_threads = 0,
    };

    RandomXContext::Hash light_hash;
    {
        const RandomXContext light{RandomXAlgorithm::V2, std::as_bytes(std::span{key}), RandomXMemoryMode::LIGHT, options};
        light_hash = light.Calculate(std::as_bytes(std::span{input}));
    }
    const RandomXContext fast{RandomXAlgorithm::V2, std::as_bytes(std::span{key}), RandomXMemoryMode::FAST, options};
    const auto fast_hash{fast.Calculate(std::as_bytes(std::span{input}))};

    BOOST_CHECK(fast_hash == light_hash);
    BOOST_CHECK_EQUAL(HexStr(fast_hash), "22ec6b861b3eb23686b2efbad69513c967ecfce80983df66c9c5b4fbfb4cdb6f");
}
#endif

BOOST_AUTO_TEST_CASE(v2_context_reuse)
{
    constexpr std::array<std::byte, 4> key{std::byte{'k'}, std::byte{'e'}, std::byte{'y'}, std::byte{'2'}};
    constexpr std::array<std::byte, 5> input{std::byte{'b'}, std::byte{'l'}, std::byte{'o'}, std::byte{'c'}, std::byte{'k'}};
    const RandomXOptions options{
        .try_large_pages = false,
        .secure_jit = true,
        .dataset_init_threads = 1,
    };
    const RandomXContext context{RandomXAlgorithm::V2, key, RandomXMemoryMode::LIGHT, options};

    const auto first{context.Calculate(input)};
    BOOST_CHECK(first == context.Calculate(input));
}

BOOST_AUTO_TEST_SUITE_END()
