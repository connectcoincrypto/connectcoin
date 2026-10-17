// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <arith_uint256.h>
#include <netbase.h>
#include <test/util/setup_common.h>
#include <wallet/p2c_admission.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(p2c_admission_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(precomputed_budget_matches_exact_integer_policy)
{
    constexpr auto MAXIMUM{std::numeric_limits<uint64_t>::max()};
    const auto maximum_target{UintToArith256(uint256::FromHex(std::string(64, 'f')).value())};
    for (unsigned shift = 0; shift <= 256; ++shift) {
        const auto target{ArithToUint256(maximum_target >> shift)};
        const P2CConnectionBudget budget{target};
        const auto expected = [&](uint64_t successes) {
            return successes == MAXIMUM || IsP2CClaimConnectionLimitExceeded(target, successes);
        };
        for (const uint64_t count : std::array<uint64_t, 9>{0, 1, 2, 3, 4, 5, 1024, MAXIMUM - 1, MAXIMUM})
            BOOST_CHECK_EQUAL(budget.Exceeded(count), expected(count));
        if (shift < 62) {
            const uint64_t threshold{uint64_t{1} << (shift + 1)};
            for (const uint64_t count : {threshold - 1, threshold, threshold + 1})
                BOOST_CHECK_EQUAL(budget.Exceeded(count), expected(count));
        }
    }
}

BOOST_AUTO_TEST_CASE(fifo_head_survives_cancellation_and_duplicate_release)
{
    P2CFifoAdmission queue;
    std::array<P2CFifoAdmission::Ticket, 128> tickets;
    for (auto& ticket : tickets) queue.Enqueue(ticket);
    BOOST_CHECK(queue.IsHead(tickets[0]));
    queue.Remove(tickets[64]);
    queue.Remove(tickets[64]);
    BOOST_CHECK(queue.IsHead(tickets[0]));
    queue.NotifyHead();
    queue.NotifyAll();
    for (size_t index = 0; index < tickets.size(); ++index) {
        if (index == 64) continue;
        BOOST_CHECK(queue.IsHead(tickets[index]));
        for (size_t later = index + 1; later < tickets.size(); ++later) BOOST_CHECK(!queue.IsHead(tickets[later]));
        queue.Remove(tickets[index]);
    }
    BOOST_CHECK(queue.Empty());
    queue.Remove(tickets[0]);
    queue.NotifyHead();
    queue.NotifyAll();
    queue.Enqueue(tickets[0]);
    BOOST_CHECK(queue.IsHead(tickets[0]));
    queue.Remove(tickets[0]);
    BOOST_CHECK(queue.Empty());
}

BOOST_AUTO_TEST_CASE(local_route_and_family_failures_expire_without_disabling_other_addresses)
{
    const auto first{Lookup("8.8.8.8", 443, false).value()};
    const auto second{Lookup("1.1.1.1", 443, false).value()};
    const auto ipv6{Lookup("2001:4860:4860::8888", 443, false).value()};
    const auto other_ipv6{Lookup("2606:4700:4700::1111", 443, false).value()};
    P2CLocalEndpointPolicy policy;
    BOOST_CHECK(policy.Available(first, 0));
    policy.Reject(first, false, 10);
    BOOST_CHECK(!policy.Available(first, 69.999));
    BOOST_CHECK(policy.Available(second, 10));
    BOOST_CHECK(policy.Available(first, 70));
    policy.Reject(ipv6, true, 20);
    BOOST_CHECK(!policy.Available(ipv6, 79.999));
    BOOST_CHECK(!policy.Available(other_ipv6, 79.999));
    BOOST_CHECK(policy.Available(first, 70));
    BOOST_CHECK(policy.Available(other_ipv6, 80));
    policy.Reject(second, false, 81);
    BOOST_CHECK_EQUAL(policy.EndpointCount(), 1U);
}

BOOST_AUTO_TEST_CASE(local_route_cache_has_a_fixed_memory_bound)
{
    P2CLocalEndpointPolicy policy;
    const auto first{Lookup("8.1.0.1", 443, false).value()};
    for (unsigned index = 0; index < 5000; ++index) {
        const auto address{Lookup("8.1." + std::to_string(index / 250) + "." + std::to_string(index % 250 + 1), 443, false).value()};
        policy.Reject(address, false, index / 1000.0);
        BOOST_CHECK_LE(policy.EndpointCount(), 4096U);
    }
    BOOST_CHECK_EQUAL(policy.EndpointCount(), 4096U);
    BOOST_CHECK(policy.Available(first, 5)); // Oldest entry evicted, never permanent.
    const auto last{Lookup("8.1.19.250", 443, false).value()};
    BOOST_CHECK(!policy.Available(last, 5));
    BOOST_CHECK(policy.Available(last, 65));
    policy.Reject(first, false, 65);
    BOOST_CHECK_EQUAL(policy.EndpointCount(), 1U);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
