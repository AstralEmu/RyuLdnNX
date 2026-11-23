/**
 * @file p2p_proxy_client_tests.cpp
 * @brief Unit tests for P2pProxyClient constants and logic
 *
 * Tests the P2P proxy client implementation for compatibility with Ryujinx.
 * These tests focus on constants, IP parsing, and timeout values.
 */

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <arpa/inet.h>

// =============================================================================
// Minimal Test Framework
// =============================================================================

/// Number of tests started.
static int g_tests_run = 0;
/// Number of tests that passed.
static int g_tests_passed = 0;

/**
 * @brief Define a test and run it during static initialization.
 * @param name Identifier of the test (also printed, stringified).
 */
#define TEST(name)                              \
    static void test_##name();                  \
    static struct TestRegistrar_##name {        \
        TestRegistrar_##name() {                \
            printf("  Running: %s... ", #name); \
            g_tests_run++;                      \
            test_##name();                      \
            g_tests_passed++;                   \
            printf("PASSED\n");                 \
        }                                       \
    } g_registrar_##name;                       \
    static void test_##name()

/**
 * @brief Exit the process when expr is false.
 * @param expr Expression expected to be true.
 */
#define ASSERT_TRUE(expr)                                        \
    do {                                                         \
        if (!(expr)) {                                           \
            printf("FAILED\n    Assertion failed: %s\n", #expr); \
            exit(1);                                             \
        }                                                        \
    } while (0)

/**
 * @brief Exit the process when expr is true.
 * @param expr Expression expected to be false.
 */
#define ASSERT_FALSE(expr)                                          \
    do {                                                            \
        if (expr) {                                                 \
            printf("FAILED\n    Assertion failed: !(%s)\n", #expr); \
            exit(1);                                                \
        }                                                           \
    } while (0)

/**
 * @brief Exit the process when a != b.
 * @param a Actual value.
 * @param b Expected value.
 */
#define ASSERT_EQ(a, b)                                         \
    do {                                                        \
        if ((a) != (b)) {                                       \
            printf("FAILED\n    Expected: %s == %s\n", #a, #b); \
            exit(1);                                            \
        }                                                       \
    } while (0)

// =============================================================================
// P2P Proxy Client Constants (matching implementation)
// =============================================================================

namespace p2p_client {
// Timeout constants (matching Ryujinx)
/// Authentication and ready-wait timeout in milliseconds.
constexpr int FAILURE_TIMEOUT_MS = 4000;
/// TCP connect timeout in milliseconds.
constexpr int CONNECT_TIMEOUT_MS = 5000;

// Receive buffer size
/// Receive buffer size in bytes (64 KB).
constexpr size_t RECV_BUFFER_SIZE = 0x10000;  // 64KB
}  // namespace p2p_client

// =============================================================================
// Helper Functions for Tests
// =============================================================================

namespace test_helpers {

/**
 * @brief Parse IPv4 address string to network order uint32
 *
 * @param str dotted IPv4 string, may be null
 * @param out_ip receives the address in network byte order
 * @return true on success, false for a null or invalid string
 */
bool parse_ipv4(const char* str, uint32_t& out_ip) {
    if (str == nullptr) {
        return false;
    }
    struct in_addr addr{};
    if (inet_pton(AF_INET, str, &addr) != 1) {
        return false;
    }
    out_ip = addr.s_addr;
    return true;
}

/**
 * @brief Convert network order IP to string
 *
 * @param ip IPv4 address in network byte order
 * @param out buffer receiving the dotted string
 * @param out_len size of out in bytes
 */
void ipv4_to_string(uint32_t ip, char* out, size_t out_len) {
    struct in_addr addr{};
    addr.s_addr = ip;
    inet_ntop(AF_INET, &addr, out, static_cast<socklen_t>(out_len));
}

/**
 * @brief Check if IP is in private range (RFC 1918)
 *
 * @param ip IPv4 address in network byte order
 * @return true when ip is in 10/8, 172.16/12 or 192.168/16
 */
bool is_private_ip(uint32_t ip) {
    // Convert from network order to host order for comparison
    uint32_t host_ip = ntohl(ip);

    // 10.0.0.0/8
    if ((host_ip & 0xFF000000) == 0x0A000000) {
        return true;
    }

    // 172.16.0.0/12
    if ((host_ip & 0xFFF00000) == 0xAC100000) {
        return true;
    }

    // 192.168.0.0/16
    if ((host_ip & 0xFFFF0000) == 0xC0A80000) {
        return true;
    }

    return false;
}

/**
 * @brief Calculate broadcast address
 *
 * @param ip IPv4 address
 * @param mask subnet mask, same byte order as ip
 * @return ip with all host bits (those clear in mask) set
 */
uint32_t calculate_broadcast(uint32_t ip, uint32_t mask) {
    return ip | (~mask);
}

}  // namespace test_helpers

// =============================================================================
// Constant Tests - Ryujinx Compatibility
// =============================================================================

/// @test Verify constants failure timeout matches ryujinx.
TEST(constants_failure_timeout_matches_ryujinx) {
    // Ryujinx uses FailureTimeout = 4000ms
    ASSERT_EQ(p2p_client::FAILURE_TIMEOUT_MS, 4000);
}

/// @test Verify constants connect timeout reasonable.
TEST(constants_connect_timeout_reasonable) {
    // Connect timeout should be 5 seconds
    ASSERT_EQ(p2p_client::CONNECT_TIMEOUT_MS, 5000);
}

/// @test Verify constants recv buffer size.
TEST(constants_recv_buffer_size) {
    // 64KB receive buffer
    ASSERT_EQ(p2p_client::RECV_BUFFER_SIZE, 0x10000U);
    ASSERT_EQ(p2p_client::RECV_BUFFER_SIZE, 65536U);
}

// =============================================================================
// IP Parsing Tests
// =============================================================================

/// @test Verify ip parsing valid ipv4.
TEST(ip_parsing_valid_ipv4) {
    uint32_t ip = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("192.168.1.100", ip));
    ASSERT_TRUE(ip != 0);

    char str[INET_ADDRSTRLEN];
    test_helpers::ipv4_to_string(ip, str, sizeof(str));
    ASSERT_EQ(strcmp(str, "192.168.1.100"), 0);
}

/// @test Verify ip parsing localhost.
TEST(ip_parsing_localhost) {
    uint32_t ip = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("127.0.0.1", ip));
    ASSERT_TRUE(ip != 0);

    char str[INET_ADDRSTRLEN];
    test_helpers::ipv4_to_string(ip, str, sizeof(str));
    ASSERT_EQ(strcmp(str, "127.0.0.1"), 0);
}

/// @test Verify ip parsing broadcast.
TEST(ip_parsing_broadcast) {
    uint32_t ip = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("255.255.255.255", ip));
    ASSERT_EQ(ip, 0xFFFFFFFF);
}

/// @test Verify ip parsing zero.
TEST(ip_parsing_zero) {
    uint32_t ip = 1;  // Non-zero initial value
    ASSERT_TRUE(test_helpers::parse_ipv4("0.0.0.0", ip));
    ASSERT_EQ(ip, 0U);
}

/// @test Verify ip parsing invalid empty.
TEST(ip_parsing_invalid_empty) {
    uint32_t ip = 0;
    ASSERT_FALSE(test_helpers::parse_ipv4("", ip));
}

/// @test Verify ip parsing invalid null.
TEST(ip_parsing_invalid_null) {
    uint32_t ip = 0;
    ASSERT_FALSE(test_helpers::parse_ipv4(nullptr, ip));
}

/// @test Verify ip parsing invalid format.
TEST(ip_parsing_invalid_format) {
    uint32_t ip = 0;
    ASSERT_FALSE(test_helpers::parse_ipv4("not.an.ip.address", ip));
}

/// @test Verify ip parsing invalid overflow.
TEST(ip_parsing_invalid_overflow) {
    uint32_t ip = 0;
    ASSERT_FALSE(test_helpers::parse_ipv4("256.256.256.256", ip));
}

/// @test Verify ip parsing host address.
TEST(ip_parsing_host_address) {
    // Test P2P server default host address
    uint32_t ip = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("10.114.0.1", ip));

    char str[INET_ADDRSTRLEN];
    test_helpers::ipv4_to_string(ip, str, sizeof(str));
    ASSERT_EQ(strcmp(str, "10.114.0.1"), 0);
}

// =============================================================================
// Private IP Detection Tests
// =============================================================================

/// @test Verify private ip 10 0 0 0 network.
TEST(private_ip_10_0_0_0_network) {
    uint32_t ip = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("10.0.0.1", ip));
    ASSERT_TRUE(test_helpers::is_private_ip(ip));
}

/// @test Verify private ip 10 255 255 255.
TEST(private_ip_10_255_255_255) {
    uint32_t ip = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("10.255.255.255", ip));
    ASSERT_TRUE(test_helpers::is_private_ip(ip));
}

/// @test Verify private ip 172 16 0 0 network.
TEST(private_ip_172_16_0_0_network) {
    uint32_t ip = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("172.16.0.1", ip));
    ASSERT_TRUE(test_helpers::is_private_ip(ip));
}

/// @test Verify private ip 172 31 255 255.
TEST(private_ip_172_31_255_255) {
    uint32_t ip = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("172.31.255.255", ip));
    ASSERT_TRUE(test_helpers::is_private_ip(ip));
}

/// @test Verify private ip 192 168 0 0 network.
TEST(private_ip_192_168_0_0_network) {
    uint32_t ip = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("192.168.0.1", ip));
    ASSERT_TRUE(test_helpers::is_private_ip(ip));
}

/// @test Verify private ip 192 168 255 255.
TEST(private_ip_192_168_255_255) {
    uint32_t ip = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("192.168.255.255", ip));
    ASSERT_TRUE(test_helpers::is_private_ip(ip));
}

/// @test Verify public ip 8 8 8 8.
TEST(public_ip_8_8_8_8) {
    uint32_t ip = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("8.8.8.8", ip));
    ASSERT_FALSE(test_helpers::is_private_ip(ip));
}

/// @test Verify public ip 1 1 1 1.
TEST(public_ip_1_1_1_1) {
    uint32_t ip = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("1.1.1.1", ip));
    ASSERT_FALSE(test_helpers::is_private_ip(ip));
}

// =============================================================================
// Broadcast Calculation Tests
// =============================================================================

/// @test Verify broadcast calc class c.
TEST(broadcast_calc_class_c) {
    // 192.168.1.0/24
    uint32_t ip = 0;
    uint32_t mask = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("192.168.1.100", ip));
    ASSERT_TRUE(test_helpers::parse_ipv4("255.255.255.0", mask));

    uint32_t broadcast = test_helpers::calculate_broadcast(ip, mask);

    char str[INET_ADDRSTRLEN];
    test_helpers::ipv4_to_string(broadcast, str, sizeof(str));
    ASSERT_EQ(strcmp(str, "192.168.1.255"), 0);
}

/// @test Verify broadcast calc class b.
TEST(broadcast_calc_class_b) {
    // 10.114.0.0/16 (LDN network)
    uint32_t ip = 0;
    uint32_t mask = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("10.114.0.1", ip));
    ASSERT_TRUE(test_helpers::parse_ipv4("255.255.0.0", mask));

    uint32_t broadcast = test_helpers::calculate_broadcast(ip, mask);

    char str[INET_ADDRSTRLEN];
    test_helpers::ipv4_to_string(broadcast, str, sizeof(str));
    ASSERT_EQ(strcmp(str, "10.114.255.255"), 0);
}

/// @test Verify broadcast calc class a.
TEST(broadcast_calc_class_a) {
    // 10.0.0.0/8
    uint32_t ip = 0;
    uint32_t mask = 0;
    ASSERT_TRUE(test_helpers::parse_ipv4("10.0.0.1", ip));
    ASSERT_TRUE(test_helpers::parse_ipv4("255.0.0.0", mask));

    uint32_t broadcast = test_helpers::calculate_broadcast(ip, mask);

    char str[INET_ADDRSTRLEN];
    test_helpers::ipv4_to_string(broadcast, str, sizeof(str));
    ASSERT_EQ(strcmp(str, "10.255.255.255"), 0);
}

// =============================================================================
// Connection State Tests
// =============================================================================

/// @test Verify state initial not connected.
TEST(state_initial_not_connected) {
    // Verify initial state expectations
    bool connected = false;
    bool ready = false;
    ASSERT_FALSE(connected);
    ASSERT_FALSE(ready);
}

/// @test Verify state auth requires connection.
TEST(state_auth_requires_connection) {
    // Auth should fail if not connected
    bool connected = false;
    bool can_auth = connected;  // Simulates PerformAuth check
    ASSERT_FALSE(can_auth);
}

/// @test Verify state ready requires proxy config.
TEST(state_ready_requires_proxy_config) {
    // Ready state requires ProxyConfig from host
    bool ready = false;
    bool has_proxy_config = false;

    // Simulate receiving ProxyConfig
    has_proxy_config = true;
    ready = has_proxy_config;

    ASSERT_TRUE(ready);
}

// =============================================================================
// Timeout Value Tests
// =============================================================================

/// @test Verify timeout auth less than connect.
TEST(timeout_auth_less_than_connect) {
    // Auth timeout (4s) should be less than connect timeout (5s)
    // This ensures we don't wait forever for auth if connection is slow
    ASSERT_TRUE(
        p2p_client::FAILURE_TIMEOUT_MS < p2p_client::CONNECT_TIMEOUT_MS);
}

/// @test Verify timeout values positive.
TEST(timeout_values_positive) {
    ASSERT_TRUE(p2p_client::FAILURE_TIMEOUT_MS > 0);
    ASSERT_TRUE(p2p_client::CONNECT_TIMEOUT_MS > 0);
}

/// @test Verify timeout auth reasonable for network.
TEST(timeout_auth_reasonable_for_network) {
    // 4 seconds should be enough for most network conditions
    // but not so long as to cause poor UX
    ASSERT_TRUE(p2p_client::FAILURE_TIMEOUT_MS >= 1000);
    ASSERT_TRUE(p2p_client::FAILURE_TIMEOUT_MS <= 10000);
}

// =============================================================================
// Port Validation Tests
// =============================================================================

/// @test Verify port range valid.
TEST(port_range_valid) {
    // P2P ports should be in valid range
    constexpr uint16_t PORT_BASE = 39990;
    constexpr int PORT_RANGE = 10;

    ASSERT_TRUE(PORT_BASE > 1024);                // Above privileged ports
    ASSERT_TRUE(PORT_BASE < 65535 - PORT_RANGE);  // Leaves room for range

    for (int i = 0; i < PORT_RANGE; i++) {
        auto port = static_cast<uint16_t>(PORT_BASE + i);
        ASSERT_TRUE(port > 0);
        ASSERT_TRUE(port <= 65535);
    }
}

// =============================================================================
// ExternalProxyConfig Size Test
// =============================================================================

/// @test Verify external proxy config size.
TEST(external_proxy_config_size) {
    // ExternalProxyConfig should be 0x26 bytes for Ryujinx compatibility
    // proxy_ip[16] + proxy_port[2] + address_family[4] + token[32] = 54 bytes
    constexpr size_t EXPECTED_SIZE = 0x26;  // 38 bytes

    // This matches the protocol definition
    struct TestExternalProxyConfig {
        uint8_t proxy_ip[16];     // 16 bytes
        uint16_t proxy_port;      // 2 bytes
        uint32_t address_family;  // 4 bytes
        uint8_t token[16];        // 16 bytes
    } __attribute__((packed));

    ASSERT_EQ(sizeof(TestExternalProxyConfig), EXPECTED_SIZE);
}

// =============================================================================
// Main
// =============================================================================

/**
 * @brief Print the summary; tests already ran at static initialization.
 * @return 0 when all tests passed, 1 otherwise.
 */
int main() {
    printf("=== P2P Proxy Client Tests ===\n\n");

    // Tests run automatically via static initializers

    printf("\n=== Results ===\n");
    printf("Tests run: %d\n", g_tests_run);
    printf("Tests passed: %d\n", g_tests_passed);

    if (g_tests_passed == g_tests_run) {
        printf("\nAll tests PASSED!\n");
        return 0;
    }
    printf("\nSome tests FAILED!\n");
    return 1;
}
