// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <netbase.h>
#include <test/util/setup_common.h>
#include <wallet/p2c_domain_stats.h>
#include <wallet/p2c_endpoint_priority.h>
#include <wallet/p2c_performance_window.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(p2c_performance_window_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(minute_boundaries_idle_expiry_and_clock_regression)
{
    P2CMinutePerformanceWindow window;
    BOOST_CHECK(window.Record(true, 0.25, 59.999));
    BOOST_CHECK(window.Record(false, 0.75, 60));
    auto totals{window.Read(3599.999)};
    BOOST_CHECK_EQUAL(totals.attempts, 2U);
    BOOST_CHECK_EQUAL(totals.valid, 1U);
    BOOST_CHECK_EQUAL(totals.seconds, 1);
    totals = window.Read(3600);
    BOOST_CHECK_EQUAL(totals.attempts, 1U);
    BOOST_CHECK_EQUAL(totals.valid, 0U);
    BOOST_CHECK_EQUAL(totals.seconds, 0.75);
    BOOST_CHECK_EQUAL(window.Read(0).seconds, 0.75);
    BOOST_CHECK(window.Record(true, 0.125, 120));
    totals = window.Read(3660);
    BOOST_CHECK_EQUAL(totals.attempts, 1U);
    BOOST_CHECK_EQUAL(totals.valid, 1U);
    BOOST_CHECK_EQUAL(totals.seconds, 0.125);
    totals = window.Read(7200);
    BOOST_CHECK_EQUAL(totals.attempts, 0U);
    BOOST_CHECK_EQUAL(totals.seconds, 0);
    BOOST_CHECK(window.Record(true, 0, 7200));
    BOOST_CHECK(window.Record(false, 2, 10800));
    BOOST_CHECK_EQUAL(window.Read(10800).seconds, 2);
    BOOST_CHECK_EQUAL(window.Read(10800).valid, 0U);
    for (const double invalid : std::array<double, 3>{-1, INFINITY, NAN}) {
        BOOST_CHECK(!window.Record(true, invalid, 10800));
        BOOST_CHECK_EQUAL(window.Read(10800).attempts, 1U);
    }
    for (const double invalid : std::array<double, 4>{-1, INFINITY, NAN, 1e300}) {
        BOOST_CHECK_THROW(window.Read(invalid), std::invalid_argument);
    }
    BOOST_CHECK_THROW(window.ReadMinute(-1), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(bounded_rotations_preserve_small_retained_durations)
{
    P2CMinutePerformanceWindow ring;
    for (int64_t minute = 0; minute < 6000; ++minute) {
        BOOST_CHECK(ring.Record(minute % 2 == 0, 0.125, minute * 60.0));
        const auto count{static_cast<uint64_t>(std::min<int64_t>(minute + 1, 60))};
        const auto current{ring.Read(minute * 60.0)};
        BOOST_CHECK_EQUAL(current.attempts, count);
        BOOST_CHECK_LE(current.valid, count);
        BOOST_CHECK_EQUAL(current.seconds, count * 0.125);
    }
    BOOST_CHECK_EQUAL(ring.Read(6000 * 60.0).attempts, 59U);
    P2CMinutePerformanceWindow precision;
    precision.Record(true, 1e16, 0);
    precision.Record(true, 0.125, 60);
    BOOST_CHECK_EQUAL(precision.Read(3600).seconds, 0.125);
}

BOOST_AUTO_TEST_CASE(one_permanent_prior_and_finite_extremes)
{
    P2CMinutePerformanceWindow window;
    BOOST_CHECK_EQUAL(GetP2CSmoothedPerformanceRate(window.Read(0)), 5);
    BOOST_CHECK(window.Record(false, 1, 0));
    BOOST_CHECK_EQUAL(GetP2CSmoothedPerformanceRate(window.Read(0)), 2.5);
    BOOST_CHECK(window.Record(true, 0.25, 60));
    const auto totals{window.Read(60)};
    BOOST_CHECK_EQUAL(totals.attempts, 2U);
    BOOST_CHECK_EQUAL(totals.valid, 1U);
    BOOST_CHECK_EQUAL(totals.seconds, 1.25);
    BOOST_CHECK_EQUAL(GetP2CSmoothedPerformanceRate(totals), 6.0 / 2.25);
    BOOST_CHECK_EQUAL(GetP2CSmoothedPerformanceRate(window.Read(3600)), 6.0 / 1.25);
    BOOST_CHECK_EQUAL(GetP2CSmoothedPerformanceRate(window.Read(3660)), 5);
    BOOST_CHECK_EQUAL(GetP2CSmoothedPerformanceRate({1, 0, 0}), 5);
    BOOST_CHECK_EQUAL(GetP2CSmoothedPerformanceRate({1, 1, 0}), 6);
    BOOST_CHECK(std::isfinite(GetP2CSmoothedPerformanceRate({UINT64_MAX, UINT64_MAX, 0})));
    const auto extreme{GetP2CSmoothedPerformanceRate({1, 0, std::numeric_limits<double>::max()})};
    BOOST_CHECK(std::isfinite(extreme));
    BOOST_CHECK_GT(extreme, 0);
    BOOST_CHECK(window.Record(false, std::numeric_limits<double>::max(), 3660));
    BOOST_CHECK(!window.Record(true, std::numeric_limits<double>::max(), 3660));
    BOOST_CHECK_EQUAL(window.Read(3660).attempts, 1U);
    BOOST_CHECK_EQUAL(GetP2CSmoothedPerformanceRate(window.Read(7260)), 5);
}

BOOST_AUTO_TEST_CASE(shared_cache_bounds_expiry_and_in_flight_ownership)
{
    P2CPerformanceCache cache{2};
    const auto a{std::make_shared<P2CPerformanceScore>()};
    const auto b{std::make_shared<P2CPerformanceScore>()};
    const auto c{std::make_shared<P2CPerformanceScore>()};
    BOOST_CHECK_EQUAL(cache.Rate(a, 0), 5);
    BOOST_CHECK_EQUAL(cache.Size(), 0U);
    BOOST_CHECK(cache.Record(a, true, 0.5, 0));
    BOOST_CHECK_EQUAL(cache.Rate(a, 0), 4);
    BOOST_CHECK(cache.Record(b, false, 1, 1));
    BOOST_CHECK(cache.Record(a, true, 0.5, 2));
    BOOST_CHECK_EQUAL(cache.Rate(b, 2), 2.5); // Reads must not make an old sample recent.
    BOOST_CHECK(cache.Record(c, true, 0, 3));
    BOOST_CHECK_EQUAL(cache.Size(), 2U);
    BOOST_CHECK_EQUAL(cache.Rate(b, 3), 5);
    BOOST_CHECK_EQUAL(cache.Rate(a, 3), 3.5);
    BOOST_CHECK_EQUAL(cache.Rate(c, 3), 6);
    BOOST_CHECK_EQUAL(cache.Rate(a, 3600), 5);
    BOOST_CHECK_EQUAL(cache.Size(), 0U);
    BOOST_CHECK_EQUAL(cache.Rate(a, 0), 5);
    BOOST_CHECK(cache.Record(a, true, 0.25, 0));
    BOOST_CHECK_EQUAL(cache.Rate(a, 7199.999), 4.8);
    BOOST_CHECK_EQUAL(cache.Rate(a, 7200), 5);
    BOOST_CHECK(!cache.Record(b, true, NAN, 7200));
    BOOST_CHECK_EQUAL(cache.Size(), 0U);
    std::vector<std::shared_ptr<P2CPerformanceScore>> held;
    for (size_t i = 0; i < 10000; ++i) {
        held.push_back(std::make_shared<P2CPerformanceScore>());
        BOOST_CHECK(cache.Record(held.back(), true, 1, 7200));
        BOOST_CHECK_LE(cache.Size(), 2U);
    }
    BOOST_CHECK_EQUAL(cache.Rate(held.front(), 7200), 5);
    BOOST_CHECK_EQUAL(cache.Rate(held.back(), 7200), 3);
    BOOST_CHECK(cache.Record(held.front(), false, 1, 7200));
    BOOST_CHECK_EQUAL(cache.Size(), 2U);
    BOOST_CHECK_EQUAL(cache.Rate(held.back(), 10800), 5);
    BOOST_CHECK_EQUAL(cache.Size(), 0U);
    auto survivor{std::make_shared<P2CPerformanceScore>()};
    {
        P2CPerformanceCache temporary;
        BOOST_CHECK(temporary.Record(survivor, true, 1, 0));
        BOOST_CHECK_THROW(cache.Record(survivor, true, 1, 10800), std::invalid_argument);
    }
    BOOST_CHECK_EQUAL(cache.Rate(survivor, 10800), 5);
    BOOST_CHECK(cache.Record(survivor, true, 0.5, 10800));
    BOOST_CHECK_THROW(P2CPerformanceCache{0}, std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(domain_and_endpoint_histories_share_cap_but_not_samples)
{
    const auto cache{std::make_shared<P2CPerformanceCache>(2)};
    P2CDomainStats domain{cache};
    P2CEndpointPriority endpoints{cache};
    const auto ip{Lookup("8.8.8.8", 443, false).value()};
    endpoints.Refresh({ip});
    const auto six{endpoints.Select(6, 0)};
    const auto seven{endpoints.Select(7, 0)};
    BOOST_REQUIRE(six);
    BOOST_REQUIRE(seven);
    domain.Record(false, 1, 0);
    six->statistics->Record(true, 0.25, 1);
    BOOST_CHECK_EQUAL(domain.ConnectionRate(1), 2.5);
    BOOST_CHECK_EQUAL(six->statistics->ConnectionRate(1), 4.8);
    BOOST_CHECK_EQUAL(seven->statistics->ConnectionRate(1), 5);
    seven->statistics->Record(true, 0, 2);
    BOOST_CHECK_EQUAL(cache->Size(), 2U);
    BOOST_CHECK_EQUAL(domain.ConnectionRate(2), 5); // Oldest history, not only endpoint histories, is evicted.
    BOOST_CHECK_EQUAL(six->statistics->ConnectionRate(2), 4.8);
    endpoints.Refresh({ip});
    BOOST_CHECK(endpoints.Select(6, 2)->statistics == six->statistics);
    endpoints.Refresh({});
    six->statistics->Record(false, 1, 3); // Removed endpoint can finish, but cannot reinsert itself.
    endpoints.Refresh({ip});
    const auto replacement{endpoints.Select(6, 3)};
    BOOST_REQUIRE(replacement);
    BOOST_CHECK(replacement->statistics != six->statistics);
    BOOST_CHECK_EQUAL(replacement->statistics->ConnectionRate(3), 5);
    BOOST_CHECK_EQUAL(six->statistics->ConnectionRate(3600), 5);
    BOOST_CHECK_EQUAL(seven->statistics->ConnectionRate(3600), 5);
    BOOST_CHECK_EQUAL(cache->Size(), 0U);
}

BOOST_AUTO_TEST_CASE(endpoint_window_expiry_restores_balanced_exploration)
{
    P2CEndpointPriority priority;
    const auto first{Lookup("8.8.8.8", 443, false).value()};
    const auto second{Lookup("1.1.1.1", 443, false).value()};
    priority.Refresh({first, second});
    const auto failed{priority.Select(6, 0)};
    BOOST_REQUIRE(failed);
    BOOST_CHECK(failed->endpoint == first);
    for (size_t i = 0; i < 100; ++i) failed->statistics->Record(false, 2, 0);
    std::array<size_t, 2> counts{};
    for (size_t i = 0; i < 1000; ++i) ++counts[priority.Select(6, 1)->endpoint == first ? 0 : 1];
    BOOST_CHECK_GT(counts[1], 950U);
    BOOST_CHECK_GT(counts[0], 0U); // Keep the exploration floor while the bad history is live.
    counts = {};
    for (size_t i = 0; i < 1000; ++i) ++counts[priority.Select(6, 3600)->endpoint == first ? 0 : 1];
    BOOST_CHECK_GE(counts[0], 499U);
    BOOST_CHECK_LE(counts[0], 501U);
    BOOST_CHECK_EQUAL(failed->statistics->ConnectionRate(3600), 5);
}

BOOST_AUTO_TEST_CASE(local_availability_filters_without_training_or_spending_unavailable_credit)
{
    const auto cache{std::make_shared<P2CPerformanceCache>()};
    P2CEndpointPriority priority{cache};
    const auto first{Lookup("8.8.8.8", 443, false).value()};
    const auto second{Lookup("1.1.1.1", 443, false).value()};
    const auto third{Lookup("2001:4860:4860::8888", 443, false).value()};
    priority.Refresh({first, second, third});
    const auto initial{priority.Select(6, 0)};
    BOOST_REQUIRE(initial);
    BOOST_CHECK(initial->endpoint == first);
    initial->statistics->Record(true, 0.5, 0);
    const auto available = [&](const CService& endpoint) { return endpoint != first; };
    BOOST_CHECK(priority.HasAvailable(available));
    BOOST_CHECK(!priority.HasAvailable([](const CService&) { return false; }));
    for (size_t i = 0; i < 100; ++i) {
        BOOST_CHECK(!priority.Select(6, 0, [](const CService&) { return false; }));
    }
    std::array<size_t, 2> counts{};
    for (size_t i = 0; i < 1000; ++i) {
        const auto selected{priority.Select(6, 0, available)};
        BOOST_REQUIRE(selected);
        BOOST_CHECK(selected->endpoint != first);
        ++counts[selected->endpoint == second ? 0 : 1];
    }
    BOOST_CHECK_EQUAL(counts[0], 500U);
    BOOST_CHECK_EQUAL(counts[1], 500U);
    BOOST_CHECK_EQUAL(initial->statistics->ConnectionRate(0), 4);
    BOOST_CHECK_EQUAL(cache->Size(), 1U);
    const auto restored{priority.Select(6, 0, [&](const CService& endpoint) { return endpoint == first; })};
    BOOST_REQUIRE(restored);
    BOOST_CHECK(restored->statistics == initial->statistics);
    BOOST_CHECK_EQUAL(restored->statistics->ConnectionRate(0), 4);
    BOOST_CHECK(priority.HasAvailable());
    priority.Refresh({});
    BOOST_CHECK(!priority.HasAvailable());
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
