/**
 * @file upnp_port_mapper_tests.cpp
 * @brief Unit tests for the real UpnpPortMapper (p2p/upnp_port_mapper.cpp)
 *
 * The mapper is a Meyer's singleton, so its construction happens once per
 * process: the allocation-failure scenarios and the normal one each run in
 * their own process, selected by UPNP_SCENARIO (the Makefile target loops over
 * them); a test whose scenario is not the selected one returns immediately.
 * miniupnpc is replaced by the scripted stand-in in tests/stubs/miniupnpc.
 */

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include <stratosphere.hpp>
#include <miniupnpc/miniupnpc.h>

#include "test_harness.hpp"
#include "p2p/upnp_port_mapper.hpp"

using ams::mitm::p2p::UpnpPortMapper;

// ---------------------------------------------------------------------------
// Allocation failure injection: the sysmodule's own heap returns nullptr on
// exhaustion through the nothrow form, which is what the mapper calls.
// ---------------------------------------------------------------------------
namespace {
/// Number of nothrow operator new calls since the last
/// arm_allocation_failure().
int g_nothrow_calls = 0;
int g_nothrow_fail_at =
    0;  ///< 1-based index of the nothrow allocation to fail; 0 = never
}  // namespace

/**
 * @brief Test replacement of the nothrow operator new used by the mapper.
 * Counts calls and returns nullptr on the g_nothrow_fail_at-th one, mimicking
 * heap exhaustion of the sysmodule's expanded heap.
 * @param size Number of bytes requested.
 * @return the allocation, or nullptr for the armed failing call.
 */
void* operator new(
    std::size_t size, const std::nothrow_t& /*unused*/) noexcept {
    if (++g_nothrow_calls == g_nothrow_fail_at) {
        return nullptr;
    }
    // Through the replaceable throwing form, so the default operator delete
    // that frees the object pairs with the allocator that made it.
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

/**
 * @brief Check whether the process runs the given scenario.
 * @param name Scenario identifier compared to the UPNP_SCENARIO variable.
 * @return true when UPNP_SCENARIO is set and equals `name`.
 */
static bool scenario(const char* name) {
    const char* s = std::getenv("UPNP_SCENARIO");
    return s != nullptr && std::strcmp(s, name) == 0;
}

/**
 * @brief Make the nth nothrow allocation from now on fail.
 * @param nth 1-based index of the allocation that returns nullptr.
 */
static void arm_allocation_failure(int nth) {
    g_nothrow_calls = 0;
    g_nothrow_fail_at = nth;
}

/// @brief Stop injecting allocation failures.
static void disarm_allocation_failure() {
    g_nothrow_fail_at = 0;
}

/// @brief Reset the scripted miniupnpc stand-in to its defaults.
static void reset_fake() {
    g_fake_upnp = FakeUpnp{};
}

// ---------------------------------------------------------------------------

/**
 * @brief When the first allocation of the singleton's construction fails,
 * Discover() refuses to run (no network call) and the mapper reports itself
 * unavailable. Run only in the alloc_urls_fail scenario.
 */
TEST(a_failed_first_allocation_leaves_the_mapper_unusable) {
    if (!scenario("alloc_urls_fail")) {
        return;
    }
    arm_allocation_failure(1);
    UpnpPortMapper& m = UpnpPortMapper::GetInstance();
    disarm_allocation_failure();
    reset_fake();
    ASSERT_FALSE(m.Discover());
    ASSERT_EQ(g_fake_upnp.discover_calls, 0);
    ASSERT_FALSE(m.IsAvailable());
    ASSERT_FALSE(m.AddPortMapping(1, 1, "x"));
    m.Cleanup();
}

/**
 * @brief When the second allocation fails, the first is released and
 * Discover() is refused without any network call. Run only in the
 * alloc_data_fail scenario.
 */
TEST(a_failed_second_allocation_frees_the_first_and_disables_the_mapper) {
    if (!scenario("alloc_data_fail")) {
        return;
    }
    arm_allocation_failure(2);
    UpnpPortMapper& m = UpnpPortMapper::GetInstance();
    disarm_allocation_failure();
    reset_fake();
    ASSERT_FALSE(m.Discover());
    ASSERT_EQ(g_fake_upnp.discover_calls, 0);
    ASSERT_FALSE(m.IsAvailable());
    m.Cleanup();
}

/**
 * @brief Before Discover() succeeds, availability is false and every mapping
 * or address query fails (the local IPv4 reads 0).
 */
TEST(nothing_works_before_a_successful_discovery) {
    if (!scenario("ok")) {
        return;
    }
    reset_fake();
    UpnpPortMapper& m = UpnpPortMapper::GetInstance();
    char buf[16];
    ASSERT_FALSE(m.IsAvailable());
    ASSERT_FALSE(m.AddPortMapping(1, 1, "x"));
    ASSERT_FALSE(m.DeletePortMapping(1));
    ASSERT_FALSE(m.RefreshPortMapping(1, 1, "x"));
    ASSERT_FALSE(m.GetExternalIPAddress(buf, sizeof(buf)));
    ASSERT_FALSE(m.GetLocalIPAddress(buf, sizeof(buf)));
    ASSERT_EQ(m.GetLocalIPv4(), static_cast<uint32_t>(0));
}

/**
 * @brief Discover() fails when no UPnP device answers or no valid IGD is
 * found, and a retry first releases what the previous attempt left behind
 * (device list, URLs) so nothing leaks.
 */
TEST(discovery_fails_without_devices_or_gateway_and_retries_cleanly) {
    if (!scenario("ok")) {
        return;
    }
    UpnpPortMapper& m = UpnpPortMapper::GetInstance();

    reset_fake();
    g_fake_upnp.devices_found = false;
    ASSERT_FALSE(m.Discover());
    ASSERT_EQ(
        g_fake_upnp.last_delay, ams::mitm::p2p::UPNP_DISCOVERY_TIMEOUT_MS);
    ASSERT_EQ(g_fake_upnp.devlist_frees, 0);

    // Devices found but no IGD: the control URL the library filled in is kept
    // until the next attempt, which must release it first.
    reset_fake();
    g_fake_upnp.igd_result = 0;
    ASSERT_FALSE(m.Discover());
    ASSERT_EQ(g_fake_upnp.devlist_frees, 1);
    ASSERT_FALSE(m.IsAvailable());

    g_fake_upnp.igd_result = 3;
    ASSERT_FALSE(m.Discover());
    ASSERT_EQ(g_fake_upnp.urls_frees, 1);
    ASSERT_FALSE(m.IsAvailable());
}

/**
 * @brief Discover() succeeds on a connected or a disconnected IGD, and a
 * second call returns the cached result without a new network round trip.
 */
TEST(discovery_succeeds_on_any_gateway_state_and_is_cached) {
    if (!scenario("ok")) {
        return;
    }
    UpnpPortMapper& m = UpnpPortMapper::GetInstance();

    reset_fake();
    g_fake_upnp.igd_result = 2;
    ASSERT_TRUE(m.Discover());
    ASSERT_TRUE(m.IsAvailable());
    m.Cleanup();
    ASSERT_FALSE(m.IsAvailable());

    reset_fake();
    ASSERT_TRUE(m.Discover());
    ASSERT_EQ(g_fake_upnp.discover_calls, 1);
    ASSERT_TRUE(m.Discover());  // cached: no second network round trip
    ASSERT_EQ(g_fake_upnp.discover_calls, 1);
}

/**
 * @brief AddPortMapping forwards internal/external ports, description, TCP
 * protocol, lease and the LAN address to the gateway's WANIP service, and
 * reports gateway errors as failure.
 */
TEST(adding_a_mapping_sends_the_ports_the_lease_and_the_lan_address) {
    if (!scenario("ok")) {
        return;
    }
    UpnpPortMapper& m = UpnpPortMapper::GetInstance();
    ASSERT_TRUE(m.IsAvailable());

    ASSERT_TRUE(m.AddPortMapping(39990, 40001, "ryu_ldn_nx P2P", 120));
    ASSERT_TRUE(g_fake_upnp.add_int_port == "39990");
    ASSERT_TRUE(g_fake_upnp.add_ext_port == "40001");
    ASSERT_TRUE(g_fake_upnp.add_client == "192.168.1.100");
    ASSERT_TRUE(g_fake_upnp.add_desc == "ryu_ldn_nx P2P");
    ASSERT_TRUE(g_fake_upnp.add_proto == "TCP");
    ASSERT_TRUE(g_fake_upnp.add_lease == "120");
    ASSERT_TRUE(g_fake_upnp.add_remote_host_null);
    ASSERT_TRUE(g_fake_upnp.used_service_type == "urn:fake:WANIPConnection:1");

    g_fake_upnp.add_result = 718;
    ASSERT_FALSE(m.AddPortMapping(39990, 39990, "x"));
}

/**
 * @brief RefreshPortMapping re-adds the mapping with PORT_LEASE_DURATION
 * and reports gateway errors as failure.
 */
TEST(refreshing_re_adds_the_mapping_with_the_standard_lease) {
    if (!scenario("ok")) {
        return;
    }
    UpnpPortMapper& m = UpnpPortMapper::GetInstance();
    g_fake_upnp.add_result = 0;
    ASSERT_TRUE(m.RefreshPortMapping(39991, 39992, "renew"));
    ASSERT_TRUE(
        g_fake_upnp.add_lease ==
        std::to_string(ams::mitm::p2p::PORT_LEASE_DURATION));
    ASSERT_TRUE(g_fake_upnp.add_int_port == "39991");
    ASSERT_TRUE(g_fake_upnp.add_ext_port == "39992");
    g_fake_upnp.add_result = 501;
    ASSERT_FALSE(m.RefreshPortMapping(39991, 39992, "renew"));
}

/**
 * @brief DeletePortMapping succeeds on code 0 and on 714 (no such entry,
 * nothing to remove) but fails on other gateway errors.
 */
TEST(deleting_a_mapping_treats_a_missing_entry_as_success) {
    if (!scenario("ok")) {
        return;
    }
    UpnpPortMapper& m = UpnpPortMapper::GetInstance();
    g_fake_upnp.delete_result = 0;
    ASSERT_TRUE(m.DeletePortMapping(39993));
    ASSERT_TRUE(g_fake_upnp.del_ext_port == "39993");
    ASSERT_TRUE(g_fake_upnp.del_proto == "TCP");
    ASSERT_TRUE(g_fake_upnp.del_remote_host_null);
    g_fake_upnp.delete_result = 714;
    ASSERT_TRUE(m.DeletePortMapping(39993));
    g_fake_upnp.delete_result = 500;
    ASSERT_FALSE(m.DeletePortMapping(39993));
}

/**
 * @brief GetExternalIPAddress copies the address, truncating into a small
 * buffer while keeping the NUL terminator, and refuses a null or empty
 * buffer and gateway errors.
 */
TEST(the_external_address_is_copied_terminated_and_bad_arguments_refused) {
    if (!scenario("ok")) {
        return;
    }
    UpnpPortMapper& m = UpnpPortMapper::GetInstance();
    char buf[16];
    ASSERT_TRUE(m.GetExternalIPAddress(buf, sizeof(buf)));
    ASSERT_TRUE(std::strcmp(buf, "203.0.113.7") == 0);

    char small[4];
    ASSERT_TRUE(m.GetExternalIPAddress(small, sizeof(small)));
    ASSERT_TRUE(std::strcmp(small, "203") == 0);

    ASSERT_FALSE(m.GetExternalIPAddress(nullptr, 16));
    ASSERT_FALSE(m.GetExternalIPAddress(buf, 0));
    g_fake_upnp.external_ip_result = 501;
    ASSERT_FALSE(m.GetExternalIPAddress(buf, sizeof(buf)));
}

/**
 * @brief GetLocalIPAddress copies the LAN address, truncating into a small
 * buffer while keeping the NUL terminator, and refuses a null or empty
 * buffer.
 */
TEST(the_local_address_is_copied_terminated_and_bad_arguments_refused) {
    if (!scenario("ok")) {
        return;
    }
    UpnpPortMapper& m = UpnpPortMapper::GetInstance();
    char buf[32];
    ASSERT_TRUE(m.GetLocalIPAddress(buf, sizeof(buf)));
    ASSERT_TRUE(std::strcmp(buf, "192.168.1.100") == 0);

    char small[4];
    ASSERT_TRUE(m.GetLocalIPAddress(small, sizeof(small)));
    ASSERT_TRUE(std::strcmp(small, "192") == 0);

    ASSERT_FALSE(m.GetLocalIPAddress(nullptr, 16));
    ASSERT_FALSE(m.GetLocalIPAddress(buf, 0));
}

/**
 * @brief GetLocalIPv4 returns the LAN address in host byte order and 0 for
 * out-of-range octets, non-addresses, short or empty strings.
 */
TEST(the_local_address_is_parsed_to_host_order_and_malformed_ones_give_zero) {
    if (!scenario("ok")) {
        return;
    }
    UpnpPortMapper& m = UpnpPortMapper::GetInstance();
    ASSERT_EQ(m.GetLocalIPv4(), 0xC0A80164U);

    struct Case {
        const char* lan;
        uint32_t expected;
    };
    const Case cases[] = {
        {.lan = "10.114.0.1", .expected = 0x0A720001},
        {.lan = "255.255.255.255", .expected = 0xFFFFFFFF},
        {.lan = "256.1.1.1", .expected = 0},
        {.lan = "1.256.1.1", .expected = 0},
        {.lan = "1.1.256.1", .expected = 0},
        {.lan = "1.1.1.256", .expected = 0},
        {.lan = "not an address", .expected = 0},
        {.lan = "1.2.3", .expected = 0},
        {.lan = "", .expected = 0},
    };
    for (const Case& c : cases) {
        m.Cleanup();
        reset_fake();
        g_fake_upnp.lan_addr = c.lan;
        ASSERT_TRUE(m.Discover());
        ASSERT_EQ(m.GetLocalIPv4(), c.expected);
    }
}

/**
 * @brief If the IGD leaves the control URL empty, mapping, deletion and
 * external-address queries all fail, and Cleanup() makes the mapper
 * unavailable again.
 */
TEST(a_gateway_without_a_control_url_refuses_every_command) {
    if (!scenario("ok")) {
        return;
    }
    UpnpPortMapper& m = UpnpPortMapper::GetInstance();
    m.Cleanup();
    reset_fake();
    g_fake_upnp.igd_sets_control = false;
    ASSERT_TRUE(m.Discover());
    char buf[16];
    ASSERT_FALSE(m.AddPortMapping(1, 1, "x"));
    ASSERT_FALSE(m.DeletePortMapping(1));
    ASSERT_FALSE(m.GetExternalIPAddress(buf, sizeof(buf)));
    m.Cleanup();
    ASSERT_FALSE(m.IsAvailable());
}

/**
 * @brief Print the test totals (the tests ran during static initialization).
 * @return Non-zero when at least one assertion failed.
 */
int main() {
    return test_report("UpnpPortMapper Tests");
}
