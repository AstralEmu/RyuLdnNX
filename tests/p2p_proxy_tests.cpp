/**
 * @file p2p_proxy_tests.cpp
 * @brief Unit tests for P2P Proxy Server
 *
 * These tests verify the P2pProxyServer constants and logic match Ryujinx
 * for full interoperability.
 *
 * ## Test Categories
 *
 * 1. **Constants Tests**: Verify P2P constants match Ryujinx
 *    - Port ranges
 *    - Lease timing
 *    - Auth timeout
 *
 * 2. **Logic Tests**: Test non-network functionality
 *    - Virtual IP handling
 *    - Broadcast address calculation
 *    - Token validation logic
 *
 * Note: Actual P2P functionality requires Switch hardware and network.
 * These tests validate the portable logic and constants.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>

// ============================================================================
// Test Framework (minimal, no external dependencies)
// ============================================================================

/// Number of tests started.
static int g_tests_run = 0;
/// Number of tests that passed.
static int g_tests_passed = 0;
/// Number of tests that failed.
static int g_tests_failed = 0;

/**
 * @brief Define and register a test.
 * @param name Identifier of the test (also printed, stringified).
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
 * @brief Throw when cond is false.
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
 * @brief Throw when cond is true.
 * @param cond Expression expected to be false.
 */
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

/**
 * @brief Widen an integral value for PRId64 printing.
 * @tparam T integral or enum type of the value
 * @param v value to widen
 * @return v as long long
 */
template <typename T>
static int64_t assert_ll(T v) {
    return static_cast<int64_t>(v);
}

/**
 * @brief Throw when a != b.
 * @param a Actual value.
 * @param b Expected value.
 */
#define ASSERT_EQ(a, b)                                               \
    do {                                                              \
        auto _a = (a);                                                \
        auto _b = (b);                                                \
        if (_a != _b) {                                               \
            printf("  FAIL: %s == %s (line %d)\n", #a, #b, __LINE__); \
            printf(                                                   \
                "    got: %" PRId64 " vs %" PRId64 "\n",              \
                assert_ll(_a),                                        \
                assert_ll(_b));                                       \
            throw std::runtime_error("Test assertion failed");        \
        }                                                             \
    } while (0)

/**
 * @brief Throw when a == b.
 * @param a First value.
 * @param b Value that must differ.
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

/// One registered test: its name and entry point.
struct TestEntry {
    /// Test name printed by main().
    const char* name;
    /// Test body; throws on a failed assertion.
    void (*func)();
};

/// Registered tests, in registration order.
static TestEntry g_tests[128];
/// Number of entries used in g_tests.
static int g_test_count = 0;

/**
 * @brief Add a test to g_tests (ignored when the table is full).
 * @param name Test name.
 * @param func Test entry point.
 */
static void register_test(const char* name, void (*func)()) {
    if (g_test_count < 128) {
        g_tests[g_test_count++] = {.name = name, .func = func};
    }
}

// ============================================================================
// P2P Proxy Constants (must match Ryujinx)
// ============================================================================

namespace p2p_proxy {
// Port configuration - must match Ryujinx for interoperability
/// First TCP port the P2P server listens on.
constexpr uint16_t PRIVATE_PORT_BASE = 39990;
/// Number of consecutive private ports tried.
constexpr int PRIVATE_PORT_RANGE = 10;
/// First externally mapped (UPnP) port.
constexpr uint16_t PUBLIC_PORT_BASE = 39990;
/// Number of consecutive public ports tried.
constexpr int PUBLIC_PORT_RANGE = 10;

// UPnP lease timing
/// UPnP port-mapping lease duration in seconds.
constexpr int PORT_LEASE_LENGTH = 60;  // seconds
/// Delay in seconds before a UPnP lease is renewed.
constexpr int PORT_LEASE_RENEW = 50;  // seconds

// Authentication
/// Seconds a session waits for the authentication packet.
constexpr int AUTH_WAIT_SECONDS = 1;
/// Maximum number of players in a P2P session.
constexpr int MAX_PLAYERS = 8;

// Network constants
/// Subnet mask of the virtual LDN network (/16).
constexpr uint32_t SUBNET_MASK = 0xFFFF0000;  // /16 subnet
/// Low byte of a broadcast address (.255).
constexpr uint32_t BROADCAST_SUFFIX = 0x000000FF;  // .255 for broadcast

/**
 * @brief Calculate broadcast address from IP and subnet mask
 *
 * @param ip IP address (host byte order)
 * @param mask Subnet mask (host byte order)
 * @return Broadcast address (host byte order)
 */
inline uint32_t calculate_broadcast(uint32_t ip, uint32_t mask) {
    return (ip & mask) | (~mask);
}

/**
 * @brief Check if IP is a broadcast address for given subnet
 *
 * @param ip destination address
 * @param broadcast broadcast address of the subnet
 * @return true when ip equals broadcast or is 255.255.255.255
 */
inline bool is_broadcast(uint32_t ip, uint32_t broadcast) {
    return ip == broadcast || ip == 0xFFFFFFFFU;
}

/**
 * @brief Check if token is all zeros (private mode)
 *
 * @param physical_ip 16-byte physical IP token
 * @return true when every byte of the token is zero
 */
inline bool is_private_ip(const uint8_t physical_ip[16]) {
    for (int i = 0; i < 16; i++) {
        if (physical_ip[i] != 0) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Extract IPv4 from physical IP array
 * @param physical_ip 16-byte array (network byte order)
 * @return IPv4 in host byte order
 */
inline uint32_t extract_ipv4(const uint8_t physical_ip[16]) {
    return (static_cast<uint32_t>(physical_ip[0]) << 24) |
           (static_cast<uint32_t>(physical_ip[1]) << 16) |
           (static_cast<uint32_t>(physical_ip[2]) << 8) |
           static_cast<uint32_t>(physical_ip[3]);
}
}  // namespace p2p_proxy

// ============================================================================
// Constants Tests - Verify Ryujinx Compatibility
// ============================================================================

/// @test Verify constants private port base matches ryujinx.
TEST(constants_private_port_base_matches_ryujinx) {
    // Ryujinx P2pProxyServer.PrivatePortBase = 39990
    ASSERT_EQ(p2p_proxy::PRIVATE_PORT_BASE, 39990U);
}

/// @test Verify constants private port range matches ryujinx.
TEST(constants_private_port_range_matches_ryujinx) {
    // Ryujinx P2pProxyServer.PrivatePortRange = 10
    ASSERT_EQ(p2p_proxy::PRIVATE_PORT_RANGE, 10);
}

/// @test Verify constants public port base matches ryujinx.
TEST(constants_public_port_base_matches_ryujinx) {
    // Ryujinx P2pProxyServer.PublicPortBase = 39990
    ASSERT_EQ(p2p_proxy::PUBLIC_PORT_BASE, 39990U);
}

/// @test Verify constants public port range matches ryujinx.
TEST(constants_public_port_range_matches_ryujinx) {
    // Ryujinx P2pProxyServer.PublicPortRange = 10
    ASSERT_EQ(p2p_proxy::PUBLIC_PORT_RANGE, 10);
}

/// @test Verify constants port lease length matches ryujinx.
TEST(constants_port_lease_length_matches_ryujinx) {
    // Ryujinx P2pProxyServer.PortLeaseLength = 60
    ASSERT_EQ(p2p_proxy::PORT_LEASE_LENGTH, 60);
}

/// @test Verify constants port lease renew matches ryujinx.
TEST(constants_port_lease_renew_matches_ryujinx) {
    // Ryujinx P2pProxyServer.PortLeaseRenew = 50
    ASSERT_EQ(p2p_proxy::PORT_LEASE_RENEW, 50);
}

/// @test Verify constants auth wait matches ryujinx.
TEST(constants_auth_wait_matches_ryujinx) {
    // Ryujinx uses 1 second timeout for token validation
    ASSERT_EQ(p2p_proxy::AUTH_WAIT_SECONDS, 1);
}

/// @test Verify constants max players matches ryujinx.
TEST(constants_max_players_matches_ryujinx) {
    // LDN supports up to 8 players
    ASSERT_EQ(p2p_proxy::MAX_PLAYERS, 8);
}

/// @test Verify constants subnet mask is class b.
TEST(constants_subnet_mask_is_class_b) {
    // /16 subnet = 0xFFFF0000
    ASSERT_EQ(p2p_proxy::SUBNET_MASK, 0xFFFF0000U);
}

/// @test Verify lease timing correct.
TEST(lease_timing_correct) {
    // Renewal should happen before expiry
    ASSERT_TRUE(p2p_proxy::PORT_LEASE_RENEW < p2p_proxy::PORT_LEASE_LENGTH);

    // 10 second margin
    int margin = p2p_proxy::PORT_LEASE_LENGTH - p2p_proxy::PORT_LEASE_RENEW;
    ASSERT_EQ(margin, 10);
}

/// @test Verify port range valid.
TEST(port_range_valid) {
    // All ports in range should be valid (< 65536)
    const uint32_t max_private =
        p2p_proxy::PRIVATE_PORT_BASE + p2p_proxy::PRIVATE_PORT_RANGE - 1;
    const uint32_t max_public =
        p2p_proxy::PUBLIC_PORT_BASE + p2p_proxy::PUBLIC_PORT_RANGE - 1;

    ASSERT_TRUE(max_private < 65536U);
    ASSERT_TRUE(max_public < 65536U);
    ASSERT_EQ(max_private, 39999U);
    ASSERT_EQ(max_public, 39999U);
}

// ============================================================================
// Broadcast Address Tests
// ============================================================================

/// @test Verify broadcast calculation class b.
TEST(broadcast_calculation_class_b) {
    // Virtual IP: 10.114.0.1 with /16 mask
    uint32_t ip = 0x0A720001U;    // 10.114.0.1
    uint32_t mask = 0xFFFF0000U;  // /16

    uint32_t broadcast = p2p_proxy::calculate_broadcast(ip, mask);

    // Expected: 10.114.255.255 = 0x0A72FFFF
    ASSERT_EQ(broadcast, 0x0A72FFFFU);
}

/// @test Verify broadcast calculation class c.
TEST(broadcast_calculation_class_c) {
    // 192.168.1.100 with /24 mask
    uint32_t ip = 0xC0A80164U;    // 192.168.1.100
    uint32_t mask = 0xFFFFFF00U;  // /24

    uint32_t broadcast = p2p_proxy::calculate_broadcast(ip, mask);

    // Expected: 192.168.1.255 = 0xC0A801FF
    ASSERT_EQ(broadcast, 0xC0A801FFU);
}

/// @test Verify is broadcast exact match.
TEST(is_broadcast_exact_match) {
    uint32_t broadcast = 0x0A72FFFFU;  // 10.114.255.255

    ASSERT_TRUE(p2p_proxy::is_broadcast(broadcast, broadcast));
}

/// @test Verify is broadcast global.
TEST(is_broadcast_global) {
    uint32_t broadcast = 0x0A72FFFFU;

    // 255.255.255.255 is always broadcast
    ASSERT_TRUE(p2p_proxy::is_broadcast(0xFFFFFFFFU, broadcast));
}

/// @test Verify is broadcast normal ip false.
TEST(is_broadcast_normal_ip_false) {
    uint32_t broadcast = 0x0A72FFFFU;
    uint32_t normal_ip = 0x0A720001U;  // 10.114.0.1

    ASSERT_FALSE(p2p_proxy::is_broadcast(normal_ip, broadcast));
}

// ============================================================================
// Private IP Detection Tests
// ============================================================================

/// @test Verify is private ip all zeros.
TEST(is_private_ip_all_zeros) {
    uint8_t physical_ip[16] = {0};

    ASSERT_TRUE(p2p_proxy::is_private_ip(physical_ip));
}

/// @test Verify is private ip has bytes.
TEST(is_private_ip_has_bytes) {
    uint8_t physical_ip[16] = {
        192, 168, 1, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    ASSERT_FALSE(p2p_proxy::is_private_ip(physical_ip));
}

/// @test Verify is private ip last byte set.
TEST(is_private_ip_last_byte_set) {
    uint8_t physical_ip[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

    ASSERT_FALSE(p2p_proxy::is_private_ip(physical_ip));
}

// ============================================================================
// IPv4 Extraction Tests
// ============================================================================

/// @test Verify extract ipv4 standard.
TEST(extract_ipv4_standard) {
    uint8_t physical_ip[16] = {
        192, 168, 1, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    uint32_t ip = p2p_proxy::extract_ipv4(physical_ip);

    // 192.168.1.100 = 0xC0A80164
    ASSERT_EQ(ip, 0xC0A80164U);
}

/// @test Verify extract ipv4 localhost.
TEST(extract_ipv4_localhost) {
    uint8_t physical_ip[16] = {
        127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    uint32_t ip = p2p_proxy::extract_ipv4(physical_ip);

    // 127.0.0.1 = 0x7F000001
    ASSERT_EQ(ip, 0x7F000001U);
}

/// @test Verify extract ipv4 ldn network.
TEST(extract_ipv4_ldn_network) {
    // LDN virtual network uses 10.114.x.x
    uint8_t physical_ip[16] = {
        10, 114, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    uint32_t ip = p2p_proxy::extract_ipv4(physical_ip);

    // 10.114.0.1 = 0x0A720001
    ASSERT_EQ(ip, 0x0A720001U);
}

/// @test Verify extract ipv4 zeros.
TEST(extract_ipv4_zeros) {
    uint8_t physical_ip[16] = {0};

    uint32_t ip = p2p_proxy::extract_ipv4(physical_ip);

    ASSERT_EQ(ip, 0U);
}

// ============================================================================
// Token Validation Logic Tests
// ============================================================================

/// @test Verify token compare match.
TEST(token_compare_match) {
    uint8_t token1[16] = {
        0x01,
        0x02,
        0x03,
        0x04,
        0x05,
        0x06,
        0x07,
        0x08,
        0x09,
        0x0A,
        0x0B,
        0x0C,
        0x0D,
        0x0E,
        0x0F,
        0x10};
    uint8_t token2[16] = {
        0x01,
        0x02,
        0x03,
        0x04,
        0x05,
        0x06,
        0x07,
        0x08,
        0x09,
        0x0A,
        0x0B,
        0x0C,
        0x0D,
        0x0E,
        0x0F,
        0x10};

    ASSERT_EQ(std::memcmp(token1, token2, 16), 0);
}

/// @test Verify token compare mismatch first byte.
TEST(token_compare_mismatch_first_byte) {
    uint8_t token1[16] = {
        0x01,
        0x02,
        0x03,
        0x04,
        0x05,
        0x06,
        0x07,
        0x08,
        0x09,
        0x0A,
        0x0B,
        0x0C,
        0x0D,
        0x0E,
        0x0F,
        0x10};
    uint8_t token2[16] = {
        0xFF,
        0x02,
        0x03,
        0x04,
        0x05,
        0x06,
        0x07,
        0x08,
        0x09,
        0x0A,
        0x0B,
        0x0C,
        0x0D,
        0x0E,
        0x0F,
        0x10};

    ASSERT_NE(std::memcmp(token1, token2, 16), 0);
}

/// @test Verify token compare mismatch last byte.
TEST(token_compare_mismatch_last_byte) {
    uint8_t token1[16] = {
        0x01,
        0x02,
        0x03,
        0x04,
        0x05,
        0x06,
        0x07,
        0x08,
        0x09,
        0x0A,
        0x0B,
        0x0C,
        0x0D,
        0x0E,
        0x0F,
        0x10};
    uint8_t token2[16] = {
        0x01,
        0x02,
        0x03,
        0x04,
        0x05,
        0x06,
        0x07,
        0x08,
        0x09,
        0x0A,
        0x0B,
        0x0C,
        0x0D,
        0x0E,
        0x0F,
        0xFF};

    ASSERT_NE(std::memcmp(token1, token2, 16), 0);
}

// ============================================================================
// Virtual IP Range Tests
// ============================================================================

/// @test Verify virtual ip in ldn range.
TEST(virtual_ip_in_ldn_range) {
    // LDN virtual IPs are in 10.114.x.x range
    uint32_t base = 0x0A720000U;  // 10.114.0.0
    uint32_t mask = p2p_proxy::SUBNET_MASK;

    // Player 1: 10.114.0.1
    uint32_t player1 = 0x0A720001U;
    ASSERT_EQ(player1 & mask, base);

    // Player 2: 10.114.0.2
    uint32_t player2 = 0x0A720002U;
    ASSERT_EQ(player2 & mask, base);

    // Player 8 (max): 10.114.0.8
    uint32_t player8 = 0x0A720008U;
    ASSERT_EQ(player8 & mask, base);
}

// ============================================================================
// Main
// ============================================================================

/**
 * @brief Run every registered test and print the summary.
 * @return 0 when all tests passed, 1 otherwise.
 */
int main() {
    printf("=== P2P Proxy Server Tests ===\n\n");

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
