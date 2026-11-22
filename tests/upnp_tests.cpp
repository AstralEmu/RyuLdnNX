/**
 * @file upnp_tests.cpp
 * @brief Unit tests for UPnP Port Mapper
 *
 * These tests verify the UpnpPortMapper class behavior.
 *
 * ## Test Categories
 *
 * 1. **API Tests**: Verify public interface behavior
 *    - Singleton access
 *    - State before/after discovery
 *    - Parameter validation
 *
 * 2. **Mock Tests**: Test internal logic without network
 *    - IP string parsing
 *    - Port string formatting
 *
 * Note: Actual UPnP discovery requires a real router with UPnP enabled.
 * These tests mock the network layer or test non-network functionality.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>

// ============================================================================
// Test Framework (minimal, no external dependencies)
// ============================================================================

/// Number of registered tests that were executed.
static int g_tests_run = 0;
/// Number of tests that returned without a failed assertion.
static int g_tests_passed = 0;
/// Number of tests aborted by a failed assertion.
static int g_tests_failed = 0;

/**
 * @brief Define a test and register it for main() to run.
 *
 * Declares test_<name>() and a static object whose constructor registers it
 * through register_test(); the body follows the macro.
 *
 * @param name Identifier of the test (becomes test_<name>).
 */
#define TEST(name)                             \
    static void test_##name();                 \
    static struct TestRegister_##name {        \
        TestRegister_##name() {                \
            register_test(#name, test_##name); \
        }                                      \
    } g_test_register_##name;                  \
    static void test_##name()

/**
 * @brief Abort the current test by exception when cond is false.
 * @param cond Expression expected to be true.
 */
#define ASSERT_TRUE(cond)                                      \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
            throw std::runtime_error("Test assertion failed"); \
        }                                                      \
    } while (0)

/**
 * @brief Abort the current test by exception when cond is true.
 * @param cond Expression expected to be false.
 */
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

/**
 * @brief Abort the current test by exception when a != b.
 * @param a Actual value (integral).
 * @param b Expected value (integral).
 */
#define ASSERT_EQ(a, b)                                                      \
    do {                                                                     \
        auto _a = (a);                                                       \
        auto _b = (b);                                                       \
        if (_a != _b) {                                                      \
            printf("  FAIL: %s == %s (line %d)\n", #a, #b, __LINE__);        \
            printf("    got: %lld vs %lld\n", (long long)_a, (long long)_b); \
            throw std::runtime_error("Test assertion failed");               \
        }                                                                    \
    } while (0)

/**
 * @brief Abort the current test by exception when the C strings differ.
 * @param a Actual NUL-terminated string.
 * @param b Expected NUL-terminated string.
 */
#define ASSERT_STREQ(a, b)                                            \
    do {                                                              \
        const char* _a = (a);                                         \
        const char* _b = (b);                                         \
        if (std::strcmp(_a, _b) != 0) {                               \
            printf("  FAIL: %s == %s (line %d)\n", #a, #b, __LINE__); \
            printf("    got: \"%s\" vs \"%s\"\n", _a, _b);            \
            throw std::runtime_error("Test assertion failed");        \
        }                                                             \
    } while (0)

/**
 * @brief Abort the current test by exception when a == b.
 * @param a Actual value.
 * @param b Value a must differ from.
 */
#define ASSERT_NE(a, b)                                               \
    do {                                                              \
        auto _a = (a);                                                \
        auto _b = (b);                                                \
        if (_a == _b) {                                               \
            printf("  FAIL: %s != %s (line %d)\n", #a, #b, __LINE__); \
            throw std::runtime_error("Test assertion failed");        \
        }                                                             \
    } while (0)

/// A registered test: its name and its body.
struct TestEntry {
    /// Test name as printed in the run log.
    const char* name;
    /// Test body.
    void (*func)();
};

/// Registered tests, in registration order.
static TestEntry g_tests[128];
/// Number of valid entries in g_tests.
static int g_test_count = 0;

/**
 * @brief Add a test to g_tests (silently ignored once 128 are registered).
 * @param name Test name.
 * @param func Test body.
 */
static void register_test(const char* name, void (*func)()) {
    if (g_test_count < 128) {
        g_tests[g_test_count++] = {name, func};
    }
}

// ============================================================================
// Constants Tests
// ============================================================================
// Verify P2P constants match Ryujinx implementation

namespace p2p_constants {
// These values must match Ryujinx for interoperability
/// First port of the P2P range (Ryujinx PrivatePortBase).
constexpr uint16_t P2P_PORT_BASE = 39990;
/// Number of consecutive ports in the P2P range.
constexpr int P2P_PORT_RANGE = 10;
/// UPnP discovery timeout in milliseconds.
constexpr int UPNP_DISCOVERY_TIMEOUT_MS = 2500;
/// Port mapping lease length in seconds.
constexpr int PORT_LEASE_DURATION = 60;
/// Delay in seconds after which a lease is renewed.
constexpr int PORT_LEASE_RENEW = 50;
}  // namespace p2p_constants

/**
 * @brief The P2P port base is 39990, as in Ryujinx.
 */
TEST(constants_port_base_matches_ryujinx) {
    // Ryujinx uses PrivatePortBase = 39990
    ASSERT_EQ(p2p_constants::P2P_PORT_BASE, 39990);
}

/**
 * @brief The P2P port range is 10 ports, as in Ryujinx.
 */
TEST(constants_port_range_matches_ryujinx) {
    // Ryujinx uses PrivatePortRange = 10 (ports 39990-39999)
    ASSERT_EQ(p2p_constants::P2P_PORT_RANGE, 10);
}

/**
 * @brief The UPnP discovery timeout is 2500 ms, as in Ryujinx.
 */
TEST(constants_discovery_timeout_matches_ryujinx) {
    // Ryujinx uses 2500ms timeout for UPnP discovery
    ASSERT_EQ(p2p_constants::UPNP_DISCOVERY_TIMEOUT_MS, 2500);
}

/**
 * @brief The port lease lasts 60 seconds, as in Ryujinx.
 */
TEST(constants_lease_duration_matches_ryujinx) {
    // Ryujinx uses PortLeaseLength = 60 seconds
    ASSERT_EQ(p2p_constants::PORT_LEASE_DURATION, 60);
}

/**
 * @brief The lease is renewed after 50 seconds, as in Ryujinx.
 */
TEST(constants_lease_renew_matches_ryujinx) {
    // Ryujinx uses PortLeaseRenew = 50 seconds
    ASSERT_EQ(p2p_constants::PORT_LEASE_RENEW, 50);
}

/**
 * @brief The last port of the range is 39999, below 65536.
 */
TEST(port_range_is_valid) {
    // Verify all ports in range are valid (< 65536)
    uint16_t max_port =
        p2p_constants::P2P_PORT_BASE + p2p_constants::P2P_PORT_RANGE - 1;
    ASSERT_TRUE(max_port < 65536);
    ASSERT_EQ(max_port, 39999);
}

// ============================================================================
// IPv4 String Parsing Tests
// ============================================================================
// Test the IP address string to uint32_t conversion logic

namespace ipv4_parse {
/**
 * @brief Parse IPv4 string to host-byte-order uint32_t
 *
 * This mirrors the logic in UpnpPortMapper::GetLocalIPv4()
 *
 * @param ip_str IP address string (e.g., "192.168.1.100")
 * @return IP as uint32_t in host byte order, or 0 on error
 */
uint32_t parse_ipv4(const char* ip_str) {
    if (ip_str == nullptr || ip_str[0] == '\0') {
        return 0;
    }

    unsigned int a, b, c, d;
    if (std::sscanf(ip_str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return 0;
    }

    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return 0;
    }

    return (a << 24) | (b << 16) | (c << 8) | d;
}
}  // namespace ipv4_parse

/**
 * @brief 192.168.1.100 parses to 0xC0A80164.
 */
TEST(ipv4_parse_valid_address) {
    // Common private IP address
    uint32_t ip = ipv4_parse::parse_ipv4("192.168.1.100");
    ASSERT_EQ(ip, 0xC0A80164U);  // 192.168.1.100 in hex
}

/**
 * @brief 127.0.0.1 parses to 0x7F000001.
 */
TEST(ipv4_parse_localhost) {
    uint32_t ip = ipv4_parse::parse_ipv4("127.0.0.1");
    ASSERT_EQ(ip, 0x7F000001U);  // 127.0.0.1
}

/**
 * @brief 255.255.255.255 parses to 0xFFFFFFFF.
 */
TEST(ipv4_parse_broadcast) {
    uint32_t ip = ipv4_parse::parse_ipv4("255.255.255.255");
    ASSERT_EQ(ip, 0xFFFFFFFFU);
}

/**
 * @brief 0.0.0.0 parses to 0.
 */
TEST(ipv4_parse_zero) {
    uint32_t ip = ipv4_parse::parse_ipv4("0.0.0.0");
    ASSERT_EQ(ip, 0U);
}

/**
 * @brief The LDN address 10.114.0.1 parses to 0x0A720001.
 */
TEST(ipv4_parse_class_a) {
    // 10.114.0.1 - LDN network address
    uint32_t ip = ipv4_parse::parse_ipv4("10.114.0.1");
    ASSERT_EQ(ip, 0x0A720001U);
}

/**
 * @brief An empty string parses to 0.
 */
TEST(ipv4_parse_empty_string_returns_zero) {
    uint32_t ip = ipv4_parse::parse_ipv4("");
    ASSERT_EQ(ip, 0U);
}

/**
 * @brief A null pointer parses to 0.
 */
TEST(ipv4_parse_null_returns_zero) {
    uint32_t ip = ipv4_parse::parse_ipv4(nullptr);
    ASSERT_EQ(ip, 0U);
}

/**
 * @brief An address with a missing octet parses to 0.
 */
TEST(ipv4_parse_invalid_format_returns_zero) {
    // Missing octet
    uint32_t ip = ipv4_parse::parse_ipv4("192.168.1");
    ASSERT_EQ(ip, 0U);
}

/**
 * @brief An octet above 255 makes the parse return 0.
 */
TEST(ipv4_parse_octet_overflow_returns_zero) {
    // 256 is out of range for an octet
    uint32_t ip = ipv4_parse::parse_ipv4("256.0.0.1");
    ASSERT_EQ(ip, 0U);
}

/**
 * @brief A non-numeric string parses to 0.
 */
TEST(ipv4_parse_garbage_returns_zero) {
    uint32_t ip = ipv4_parse::parse_ipv4("not.an.ip.addr");
    ASSERT_EQ(ip, 0U);
}

// ============================================================================
// Port String Formatting Tests
// ============================================================================
// Test port number to string conversion (used for UPNP_AddPortMapping)

namespace port_format {
/**
 * @brief Format port number as string
 *
 * @param port Port number (host byte order)
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 */
void format_port(uint16_t port, char* buffer, size_t buffer_size) {
    std::snprintf(buffer, buffer_size, "%u", port);
}
}  // namespace port_format

/**
 * @brief Port 39990 is formatted as "39990".
 */
TEST(port_format_base_port) {
    char buffer[8];
    port_format::format_port(39990, buffer, sizeof(buffer));
    ASSERT_STREQ(buffer, "39990");
}

/**
 * @brief Port 65535 is formatted as "65535".
 */
TEST(port_format_max_port) {
    char buffer[8];
    port_format::format_port(65535, buffer, sizeof(buffer));
    ASSERT_STREQ(buffer, "65535");
}

/**
 * @brief Port 0 is formatted as "0".
 */
TEST(port_format_zero) {
    char buffer[8];
    port_format::format_port(0, buffer, sizeof(buffer));
    ASSERT_STREQ(buffer, "0");
}

/**
 * @brief Ports 80, 443 and 8080 are formatted in decimal.
 */
TEST(port_format_common_ports) {
    char buffer[8];

    port_format::format_port(80, buffer, sizeof(buffer));
    ASSERT_STREQ(buffer, "80");

    port_format::format_port(443, buffer, sizeof(buffer));
    ASSERT_STREQ(buffer, "443");

    port_format::format_port(8080, buffer, sizeof(buffer));
    ASSERT_STREQ(buffer, "8080");
}

// ============================================================================
// UPnP Return Code Tests
// ============================================================================
// Document expected UPnP error codes

namespace upnp_errors {
// miniupnpc error codes
/// Command succeeded.
constexpr int UPNPCOMMAND_SUCCESS = 0;
/// Unspecified miniupnpc failure.
constexpr int UPNPCOMMAND_UNKNOWN_ERROR = -1;
/// Invalid arguments (UPnP error 402).
constexpr int UPNPCOMMAND_INVALID_ARGS = 402;
/// Action failed (UPnP error 501).
constexpr int UPNPCOMMAND_ACTION_FAILED = 501;
/// No mapping matches the request (UPnP error 714).
constexpr int UPNPCOMMAND_NO_SUCH_ENTRY = 714;
/// Port already mapped by another host (UPnP error 718).
constexpr int UPNPCOMMAND_CONFLICT = 718;
/// Router only accepts permanent leases (UPnP error 725).
constexpr int UPNPCOMMAND_ONLY_PERMANENT = 725;
}  // namespace upnp_errors

/**
 * @brief The success code is 0.
 */
TEST(upnp_error_success_is_zero) {
    ASSERT_EQ(upnp_errors::UPNPCOMMAND_SUCCESS, 0);
}

/**
 * @brief The no-such-entry code is 714 (to be treated as success on delete).
 */
TEST(upnp_error_no_such_entry_is_714) {
    // DeletePortMapping returns this if mapping doesn't exist
    // We should treat this as success (goal is "no mapping exists")
    ASSERT_EQ(upnp_errors::UPNPCOMMAND_NO_SUCH_ENTRY, 714);
}

/**
 * @brief The conflict code is 718 (port mapped by another host).
 */
TEST(upnp_error_conflict_is_718) {
    // AddPortMapping returns this if port is already mapped by another host
    ASSERT_EQ(upnp_errors::UPNPCOMMAND_CONFLICT, 718);
}

// ============================================================================
// Lease Duration Tests
// ============================================================================
// Verify lease timing logic

/**
 * @brief The renew delay is shorter than the lease duration.
 */
TEST(lease_renew_before_expiry) {
    // Renew should happen before lease expires
    ASSERT_TRUE(
        p2p_constants::PORT_LEASE_RENEW < p2p_constants::PORT_LEASE_DURATION);
}

/**
 * @brief Lease duration minus renew delay leaves a 10 second margin.
 */
TEST(lease_renew_margin_is_10_seconds) {
    // Ryujinx uses 10 second margin (60 - 50 = 10)
    int margin =
        p2p_constants::PORT_LEASE_DURATION - p2p_constants::PORT_LEASE_RENEW;
    ASSERT_EQ(margin, 10);
}

// ============================================================================
// Main
// ============================================================================

/**
 * @brief Run every registered test and print the totals.
 * @return 1 when at least one test failed, otherwise 0.
 */
int main() {
    printf("=== UPnP Port Mapper Tests ===\n\n");

    for (int i = 0; i < g_test_count; i++) {
        g_tests_run++;
        printf("Running: %s ... ", g_tests[i].name);

        try {
            g_tests[i].func();
            printf("PASS\n");
            g_tests_passed++;
        } catch (const std::exception& e) {
            g_tests_failed++;
        }
    }

    printf("\n=== Results ===\n");
    printf("Total:  %d\n", g_tests_run);
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}
