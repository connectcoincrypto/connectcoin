// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>
#include <policy/fees/block_policy_estimator.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <set>

BOOST_AUTO_TEST_SUITE(feerounder_tests)

BOOST_AUTO_TEST_CASE(FeeRounder)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    FeeFilterRounder fee_rounder{CFeeRate{1000}, rng};

    // check that 1000 rounds to 974 or 1071
    std::set<CAmount> results;
    while (results.size() < 2) {
        results.emplace(fee_rounder.round(1000));
    }
    BOOST_CHECK_EQUAL(*results.begin(), 974);
    BOOST_CHECK_EQUAL(*++results.begin(), 1071);

    // check that negative amounts rounds to 0
    BOOST_CHECK_EQUAL(fee_rounder.round(-0), 0);
    BOOST_CHECK_EQUAL(fee_rounder.round(-1), 0);

    // check that MAX_MONEY rounds to 9170997
    BOOST_CHECK_EQUAL(fee_rounder.round(MAX_MONEY), 9170997);
}

BOOST_AUTO_TEST_CASE(PublicRelayFloor)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    FeeFilterRounder fee_rounder{CFeeRate{100}, rng};
    for (const CAmount floor : std::array<CAmount, 6>{10, 1000, 76'000, 301'000, 601'000, 1'201'000}) {
        for (int i{0}; i < 64; ++i) {
            BOOST_CHECK_EQUAL(fee_rounder.round(0, floor), floor);
            BOOST_CHECK_EQUAL(fee_rounder.round(floor, floor), floor);
            BOOST_CHECK_EQUAL(fee_rounder.round(floor - 1, floor), floor);
            BOOST_CHECK_EQUAL(fee_rounder.round(floor * 99 / 100, floor), floor);
            BOOST_CHECK_GE(fee_rounder.round(floor * 2, floor), floor);
        }
    }
    // The large IBD sentinel still uses the quantized maximum.
    BOOST_CHECK_EQUAL(fee_rounder.round(MAX_MONEY, 1'201'000), fee_rounder.round(MAX_MONEY));
}

BOOST_AUTO_TEST_SUITE_END()
