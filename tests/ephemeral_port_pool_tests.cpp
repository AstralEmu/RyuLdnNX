/**
 * @file ephemeral_port_pool_tests.cpp
 * @brief Unit tests for the real EphemeralPortPool
 * (bsd/ephemeral_port_pool.cpp)
 *
 * Built against the host Stratosphere stand-in in tests/stubs.
 */

#include <cstdio>
#include <cstdint>

#include "test_harness.hpp"
#include "bsd/ephemeral_port_pool.hpp"

using ams::mitm::bsd::EPHEMERAL_PORT_COUNT;
using ams::mitm::bsd::EPHEMERAL_PORT_MAX;
using ams::mitm::bsd::EPHEMERAL_PORT_MIN;
using ams::mitm::bsd::EphemeralPortPool;
using ryu_ldn::bsd::ProtocolType;

/**
 * @brief A new pool starts with the whole ephemeral range free for TCP and UDP.
 */
TEST(a_fresh_pool_has_every_port_available_for_both_protocols) {
    EphemeralPortPool pool;
    ASSERT_EQ(pool.GetAvailableCount(ProtocolType::Udp), EPHEMERAL_PORT_COUNT);
    ASSERT_EQ(pool.GetAvailableCount(ProtocolType::Tcp), EPHEMERAL_PORT_COUNT);
}

/**
 * @brief Allocation hands out EPHEMERAL_PORT_MIN, then the next port, and the
 *        free count drops accordingly (ports are not picked at random).
 */
TEST(allocation_is_round_robin_starting_at_the_range_start) {
    EphemeralPortPool pool;
    ASSERT_EQ(pool.AllocatePort(ProtocolType::Udp), EPHEMERAL_PORT_MIN);
    ASSERT_EQ(pool.AllocatePort(ProtocolType::Udp), EPHEMERAL_PORT_MIN + 1);
    ASSERT_TRUE(pool.IsPortAllocated(EPHEMERAL_PORT_MIN, ProtocolType::Udp));
    ASSERT_EQ(
        pool.GetAvailableCount(ProtocolType::Udp), EPHEMERAL_PORT_COUNT - 2);
}

/**
 * @brief A released port is free again but the next allocation continues
 *        after it, so a just-closed port is not recycled immediately.
 */
TEST(released_ports_are_not_reused_before_the_hint_wraps) {
    EphemeralPortPool pool;
    const uint16_t first = pool.AllocatePort(ProtocolType::Tcp);
    pool.ReleasePort(first, ProtocolType::Tcp);
    ASSERT_FALSE(pool.IsPortAllocated(first, ProtocolType::Tcp));
    ASSERT_EQ(pool.AllocatePort(ProtocolType::Tcp), first + 1);
}

/**
 * @brief The same port number can be allocated once for TCP and once for UDP.
 */
TEST(tcp_and_udp_pools_are_independent) {
    EphemeralPortPool pool;
    ASSERT_EQ(pool.AllocatePort(ProtocolType::Tcp), EPHEMERAL_PORT_MIN);
    ASSERT_EQ(pool.AllocatePort(ProtocolType::Udp), EPHEMERAL_PORT_MIN);
    ASSERT_TRUE(pool.IsPortAllocated(EPHEMERAL_PORT_MIN, ProtocolType::Tcp));
    ASSERT_TRUE(pool.IsPortAllocated(EPHEMERAL_PORT_MIN, ProtocolType::Udp));
}

/**
 * @brief Any protocol other than TCP (here Unspecified) is allocated from the
 *        UDP pool and never marks a TCP port.
 */
TEST(other_protocols_share_the_udp_pool) {
    EphemeralPortPool pool;
    ASSERT_EQ(pool.AllocatePort(ProtocolType::Unspecified), EPHEMERAL_PORT_MIN);
    ASSERT_TRUE(pool.IsPortAllocated(EPHEMERAL_PORT_MIN, ProtocolType::Udp));
    ASSERT_TRUE(
        pool.IsPortAllocated(EPHEMERAL_PORT_MIN, ProtocolType::Unspecified));
    ASSERT_FALSE(pool.IsPortAllocated(EPHEMERAL_PORT_MIN, ProtocolType::Tcp));
}

/**
 * @brief AllocateSpecificPort refuses a port already taken for the same
 *        protocol but accepts it for the other one.
 */
TEST(a_specific_port_can_be_taken_once_per_protocol) {
    EphemeralPortPool pool;
    const uint16_t port = EPHEMERAL_PORT_MIN + 100;
    ASSERT_TRUE(pool.AllocateSpecificPort(port, ProtocolType::Udp));
    ASSERT_FALSE(pool.AllocateSpecificPort(port, ProtocolType::Udp));
    ASSERT_TRUE(pool.AllocateSpecificPort(port, ProtocolType::Tcp));
}

/**
 * @brief Ports below the ephemeral range (e.g. 80) always succeed, are not
 *        recorded and leave the available count untouched.
 */
TEST(ports_outside_the_ephemeral_range_are_accepted_but_never_tracked) {
    EphemeralPortPool pool;
    ASSERT_TRUE(pool.AllocateSpecificPort(80, ProtocolType::Udp));
    ASSERT_TRUE(pool.AllocateSpecificPort(80, ProtocolType::Udp));
    ASSERT_FALSE(pool.IsPortAllocated(80, ProtocolType::Udp));
    pool.ReleasePort(80, ProtocolType::Udp);
    ASSERT_EQ(pool.GetAvailableCount(ProtocolType::Udp), EPHEMERAL_PORT_COUNT);
}

/**
 * @brief EPHEMERAL_PORT_MAX is inside the range: it can be taken and is then
 *        reported as allocated (off-by-one guard on the upper bound).
 */
TEST(the_last_port_of_the_range_is_valid) {
    EphemeralPortPool pool;
    ASSERT_TRUE(
        pool.AllocateSpecificPort(EPHEMERAL_PORT_MAX, ProtocolType::Tcp));
    ASSERT_TRUE(pool.IsPortAllocated(EPHEMERAL_PORT_MAX, ProtocolType::Tcp));
}

/**
 * @brief Once every port is used AllocatePort returns 0; releasing one port
 *        makes exactly that port available again.
 */
TEST(an_exhausted_pool_returns_zero_until_a_port_is_released) {
    EphemeralPortPool pool;
    for (size_t i = 0; i < EPHEMERAL_PORT_COUNT; ++i) {
        ASSERT_TRUE(pool.AllocatePort(ProtocolType::Udp) != 0);
    }
    ASSERT_EQ(
        pool.GetAvailableCount(ProtocolType::Udp), static_cast<size_t>(0));
    ASSERT_EQ(pool.AllocatePort(ProtocolType::Udp), static_cast<uint16_t>(0));

    pool.ReleasePort(EPHEMERAL_PORT_MIN + 5, ProtocolType::Udp);
    ASSERT_EQ(
        pool.AllocatePort(ProtocolType::Udp),
        static_cast<uint16_t>(EPHEMERAL_PORT_MIN + 5));
}

/**
 * @brief ReleaseAll frees both protocol pools and restarts allocation at
 *        EPHEMERAL_PORT_MIN.
 */
TEST(release_all_frees_both_pools_and_resets_the_hints) {
    EphemeralPortPool pool;
    pool.AllocatePort(ProtocolType::Udp);
    pool.AllocatePort(ProtocolType::Udp);
    pool.AllocatePort(ProtocolType::Tcp);
    pool.ReleaseAll();
    ASSERT_EQ(pool.GetAvailableCount(ProtocolType::Udp), EPHEMERAL_PORT_COUNT);
    ASSERT_EQ(pool.GetAvailableCount(ProtocolType::Tcp), EPHEMERAL_PORT_COUNT);
    ASSERT_EQ(pool.AllocatePort(ProtocolType::Udp), EPHEMERAL_PORT_MIN);
    ASSERT_EQ(pool.AllocatePort(ProtocolType::Tcp), EPHEMERAL_PORT_MIN);
}

/**
 * @brief Test entry point.
 * @return Process exit code from test_report(): 0 when every test passed.
 */
int main() {
    return test_report("EphemeralPortPool Tests");
}
