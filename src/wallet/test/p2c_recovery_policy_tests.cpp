// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <util/string.h>
#include <wallet/p2c_recovery_policy.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

namespace wallet {
BOOST_AUTO_TEST_SUITE(p2c_recovery_policy_tests)

BOOST_AUTO_TEST_CASE(bootstrap_then_one_minute_per_policy)
{
    P2CRecoveryPolicy policy;
    const P2CRecoveryPolicy::Key key{"example.com", 6};
    for (uint64_t index = 0; index < P2CRecoveryPolicy::BOOTSTRAP_STARTS; ++index) {
        const double now{index * P2CRecoveryPolicy::BOOTSTRAP_INTERVAL};
        BOOST_CHECK_EQUAL(policy.Due(key, now), now);
        BOOST_REQUIRE(policy.Reserve(key, index, now));
        BOOST_CHECK(policy.ReservedFor(index));
        const double next{policy.Started(key, index, now)};
        BOOST_CHECK_EQUAL(next, now + (index + 1 < P2CRecoveryPolicy::BOOTSTRAP_STARTS ? 0.25 : 60));
        BOOST_CHECK(!policy.Reserved());
        BOOST_CHECK(!policy.Reserve(key, index + 1, now + 0.125));
    }
    BOOST_CHECK(!policy.Reserve(key, 40, 67.749));
    BOOST_REQUIRE(policy.Reserve(key, 40, 67.75));
    BOOST_CHECK_EQUAL(policy.Started(key, 40, 67.75), 127.75);
}

BOOST_AUTO_TEST_CASE(global_reservation_and_start_deadline_cross_domains_and_masks)
{
    P2CRecoveryPolicy policy;
    const P2CRecoveryPolicy::Key six{"example.com", 6}, seven{"example.com", 7}, other{"other.example", 6};
    BOOST_REQUIRE(policy.Reserve(six, 1, 0));
    BOOST_CHECK(!policy.Reserve(seven, 2, 0));
    BOOST_CHECK(!policy.Reserve(other, 3, 0));
    BOOST_CHECK(!policy.Release(999));
    BOOST_CHECK_THROW(policy.Started(seven, 1, 0), std::invalid_argument);
    BOOST_CHECK_THROW(policy.Started(six, 2, 0), std::invalid_argument);
    BOOST_CHECK(policy.ReservedFor(1));
    BOOST_CHECK_EQUAL(policy.Started(six, 1, 0), 0.25);
    BOOST_CHECK_EQUAL(policy.Due(seven, 0), 0.25);
    BOOST_CHECK_EQUAL(policy.Due(other, 0), 0.25);
    BOOST_CHECK(!policy.Reserve(seven, 2, 0.249));
    BOOST_REQUIRE(policy.Reserve(seven, 2, 0.25));
    BOOST_CHECK_EQUAL(policy.Started(seven, 2, 0.25), 0.5);
    BOOST_REQUIRE(policy.Reserve(other, 3, 0.5));
    BOOST_CHECK_EQUAL(policy.Started(other, 3, 0.5), 0.75);
}

BOOST_AUTO_TEST_CASE(pre_tcp_failure_consumes_no_bootstrap_start_or_deadline)
{
    P2CRecoveryPolicy policy;
    const P2CRecoveryPolicy::Key key{"example.com", 6};
    for (uint64_t token = 1; token <= 100; ++token) {
        BOOST_REQUIRE(policy.Reserve(key, token, 0));
        BOOST_CHECK(policy.Release(token));
        BOOST_CHECK_EQUAL(policy.Due(key, 0), 0);
    }
    BOOST_REQUIRE(policy.Reserve(key, 101, 0));
    BOOST_CHECK_EQUAL(policy.Started(key, 101, 0), 0.25);
}

BOOST_AUTO_TEST_CASE(bounded_eviction_retains_pending_owner_and_global_deadline)
{
    P2CRecoveryPolicy policy;
    const P2CRecoveryPolicy::Key first{"first.example", 6};
    BOOST_REQUIRE(policy.Reserve(first, 1, 0));
    for (size_t index = 0; index < 1000; ++index) {
        policy.Due({util::ToString(index) + ".example", 6}, 0);
        BOOST_CHECK_LE(policy.Size(), P2CRecoveryPolicy::MAX_POLICIES);
    }
    BOOST_CHECK(policy.ReservedFor(1));
    BOOST_CHECK_EQUAL(policy.Started(first, 1, 0), 0.25);
    for (size_t index = 1000; index < 2000; ++index) {
        const P2CRecoveryPolicy::Key key{util::ToString(index) + ".example", 6};
        BOOST_CHECK_EQUAL(policy.Due(key, 0), 0.25);
        BOOST_CHECK(!policy.Reserve(key, index, 0));
    }
    BOOST_CHECK_EQUAL(policy.Size(), P2CRecoveryPolicy::MAX_POLICIES);
    BOOST_CHECK_EQUAL(policy.Due(first, 0), 0.25);
}

BOOST_AUTO_TEST_CASE(retain_and_clear_preserve_live_reservation_and_global_gate)
{
    P2CRecoveryPolicy policy;
    const P2CRecoveryPolicy::Key first{"first.example", 6}, second{"second.example", 7};
    BOOST_REQUIRE(policy.Reserve(first, 1, 0));
    policy.Due(second, 0);
    policy.Retain({});
    BOOST_CHECK_EQUAL(policy.Size(), 1U);
    BOOST_CHECK(policy.ReservedFor(1));
    policy.Clear();
    BOOST_CHECK_EQUAL(policy.Size(), 0U);
    BOOST_CHECK(policy.ReservedFor(1));
    BOOST_CHECK(!policy.Reserve(second, 2, 0));
    BOOST_CHECK_EQUAL(policy.Started(first, 1, 0), 0.25);
    policy.Clear();
    BOOST_CHECK_EQUAL(policy.Due(second, 0), 0.25);
    BOOST_CHECK(!policy.Reserve(second, 2, 0));
    BOOST_REQUIRE(policy.Reserve(second, 2, 0.25));
    BOOST_CHECK_EQUAL(policy.Started(second, 2, 0.25), 0.5);
    policy.Retain({first});
    BOOST_CHECK_EQUAL(policy.Size(), 0U);
    BOOST_CHECK_EQUAL(policy.Due(first, 0.25), 0.5);
}

BOOST_AUTO_TEST_CASE(backwards_and_invalid_clock_do_not_erase_deadlines)
{
    P2CRecoveryPolicy policy;
    const P2CRecoveryPolicy::Key key{"example.com", 6};
    BOOST_REQUIRE(policy.Reserve(key, 1, 100));
    BOOST_CHECK_EQUAL(policy.Started(key, 1, 0), 100.25);
    BOOST_CHECK(!policy.Reserve(key, 2, 0));
    BOOST_CHECK_EQUAL(policy.Due(key, 0), 100.25);
    for (const double invalid : std::array<double, 3>{-1, INFINITY, NAN}) {
        BOOST_CHECK_THROW(policy.Due(key, invalid), std::invalid_argument);
        BOOST_CHECK_THROW(policy.Reserve(key, 2, invalid), std::invalid_argument);
    }
    BOOST_REQUIRE(policy.Reserve(key, 2, 100.25));
    BOOST_CHECK_THROW(policy.Started(key, 2, NAN), std::invalid_argument);
    BOOST_CHECK(policy.ReservedFor(2));
    BOOST_CHECK(policy.Release(2));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
