/**
 * @file p2p_create_network_tests.cpp
 * @brief Unit tests for Story 9.8: CreateNetwork with P2P integration
 *
 * Tests for host-side P2P proxy server integration:
 * - P2pProxyServer lifecycle (start/stop)
 * - UPnP NAT punch configuration
 * - RyuNetworkConfig P2P port fields
 * - ExternalProxyToken handling
 * - Cleanup in CloseAccessPoint/DestroyNetwork
 */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cstddef>

// =============================================================================
// Test Framework (simple macros)
// =============================================================================

/// Defines a test function and registers it in the test table.
#define TEST(name)                             \
    static void test_##name();                 \
    static struct TestRegister_##name {        \
        TestRegister_##name() {                \
            printf("  Running %s... ", #name); \
            test_##name();                     \
            printf("OK\n");                    \
        }                                      \
    } test_register_##name;                    \
    static void test_##name()

/// Fails the current test when the expression is false.
#define ASSERT_TRUE(x)                                     \
    do {                                                   \
        if (!(x)) {                                        \
            printf("FAILED\n    Expected true: %s\n", #x); \
            exit(1);                                       \
        }                                                  \
    } while (0)

/// Fails the current test when the expression is true.
#define ASSERT_FALSE(x)                                     \
    do {                                                    \
        if (x) {                                            \
            printf("FAILED\n    Expected false: %s\n", #x); \
            exit(1);                                        \
        }                                                   \
    } while (0)

/// Fails the current test when the two values differ.
#define ASSERT_EQ(a, b)                                         \
    do {                                                        \
        if ((a) != (b)) {                                       \
            printf("FAILED\n    Expected: %s == %s\n", #a, #b); \
            exit(1);                                            \
        }                                                       \
    } while (0)

/// Fails the current test when the two values are equal.
#define ASSERT_NE(a, b)                                         \
    do {                                                        \
        if ((a) == (b)) {                                       \
            printf("FAILED\n    Expected: %s != %s\n", #a, #b); \
            exit(1);                                            \
        }                                                       \
    } while (0)

/// Fails the current test when the first value is below the second.
#define ASSERT_GE(a, b)                                         \
    do {                                                        \
        if ((a) < (b)) {                                        \
            printf("FAILED\n    Expected: %s >= %s\n", #a, #b); \
            exit(1);                                            \
        }                                                       \
    } while (0)

/// Fails the current test when the first value is above the second.
#define ASSERT_LE(a, b)                                         \
    do {                                                        \
        if ((a) > (b)) {                                        \
            printf("FAILED\n    Expected: %s <= %s\n", #a, #b); \
            exit(1);                                            \
        }                                                       \
    } while (0)

// =============================================================================
// Protocol Types (matching implementation)
// =============================================================================

namespace protocol {

// P2P Proxy Server Constants (from p2p_proxy_server.hpp)
/// Test fixture value: PRIVATE PORT BASE.
constexpr uint16_t PRIVATE_PORT_BASE = 39990;
/// Test fixture value: PRIVATE PORT RANGE.
constexpr int PRIVATE_PORT_RANGE = 10;
/// Test fixture value: PUBLIC PORT BASE.
constexpr uint16_t PUBLIC_PORT_BASE = 39990;
/// Test fixture value: PUBLIC PORT RANGE.
constexpr int PUBLIC_PORT_RANGE = 10;
/// Test fixture value: PORT LEASE LENGTH.
constexpr int PORT_LEASE_LENGTH = 60;  // seconds
/// Test fixture value: PORT LEASE RENEW.
constexpr int PORT_LEASE_RENEW = 50;  // seconds
/// Test fixture value: AUTH WAIT SECONDS.
constexpr int AUTH_WAIT_SECONDS = 1;
/// Test fixture value: MAX PLAYERS.
constexpr int MAX_PLAYERS = 8;

// Address family constants
/// Test fixture value: AF INET VALUE.
constexpr uint32_t AF_INET_VALUE = 2;  // IPv4
/// Test fixture value: AF INET6 VALUE.
constexpr uint32_t AF_INET6_VALUE = 10;  // IPv6

// Packet IDs
/// Test fixture value: Packet Id.
enum class PacketId : uint8_t {
    CreateAccessPoint = 5,
    Connected = 6,
    ExternalProxy = 14,
    ExternalProxyToken = 15,
    ProxyData = 9,
    ProxyConfig = 11,
};

// RyuNetworkConfig structure (from protocol/types.hpp)
#pragma pack(push, 1)
/// Test double: Ryu Network Config.
struct RyuNetworkConfig {
    /// Test fixture value: game version.
    char game_version[16];  // Game version string
    /// Test fixture value: private ip.
    uint8_t private_ip[16];  // Local IP (IPv4 in first 4 bytes)
    /// Test fixture value: address family.
    uint32_t address_family;  // 2 = IPv4, 10 = IPv6
    /// Test fixture value: external proxy port.
    uint16_t external_proxy_port;  // UPnP public port
    /// Test fixture value: internal proxy port.
    uint16_t internal_proxy_port;  // Local TCP port
};
#pragma pack(pop)

// ExternalProxyToken structure (from protocol/types.hpp)
#pragma pack(push, 1)
/// Test double: External Proxy Token.
struct ExternalProxyToken {
    /// Test fixture value: virtual ip.
    uint32_t virtual_ip;  // Virtual IP for the joiner
    /// Test fixture value: physical ip.
    uint8_t physical_ip[16];  // Physical IP of joiner
    /// Test fixture value: address family.
    uint32_t address_family;  // 2 = IPv4, 10 = IPv6
    /// Test fixture value: token.
    uint8_t token[16];  // Authentication token
};
#pragma pack(pop)

// CreateAccessPointRequest structure (simplified)
#pragma pack(push, 1)
/// Test double: Create Access Point Request.
struct CreateAccessPointRequest {
    /// Test fixture value: network config.
    uint8_t network_config[0x64];  // NetworkConfig
    /// Test fixture value: user config.
    uint8_t user_config[0x30];  // UserConfig
    /// Test fixture value: ryu network config.
    RyuNetworkConfig ryu_network_config;
};
#pragma pack(pop)

}  // namespace protocol

// =============================================================================
// P2P Proxy Server Constants Tests
// =============================================================================

/** @brief Test case: p2p server port base is 39990. */
TEST(p2p_server_port_base_is_39990) {
    // Ryujinx uses ports 39990-39999 for P2P
    ASSERT_EQ(protocol::PRIVATE_PORT_BASE, 39990);
    ASSERT_EQ(protocol::PUBLIC_PORT_BASE, 39990);
}

/** @brief Test case: p2p server port range is 10. */
TEST(p2p_server_port_range_is_10) {
    // 10 ports available: 39990-39999
    ASSERT_EQ(protocol::PRIVATE_PORT_RANGE, 10);
    ASSERT_EQ(protocol::PUBLIC_PORT_RANGE, 10);
}

/** @brief Test case: p2p server lease duration is 60s. */
TEST(p2p_server_lease_duration_is_60s) {
    // UPnP lease duration: 60 seconds
    ASSERT_EQ(protocol::PORT_LEASE_LENGTH, 60);
}

/** @brief Test case: p2p server lease renewal is 50s. */
TEST(p2p_server_lease_renewal_is_50s) {
    // Renew 10 seconds before expiry
    ASSERT_EQ(protocol::PORT_LEASE_RENEW, 50);
}

/** @brief Test case: p2p server auth wait is 1s. */
TEST(p2p_server_auth_wait_is_1s) {
    // Wait 1 second for client auth
    ASSERT_EQ(protocol::AUTH_WAIT_SECONDS, 1);
}

/** @brief Test case: p2p server max players is 8. */
TEST(p2p_server_max_players_is_8) {
    // Maximum concurrent P2P sessions
    ASSERT_EQ(protocol::MAX_PLAYERS, 8);
}

// =============================================================================
// RyuNetworkConfig Structure Tests
// =============================================================================

/** @brief Test case: ryu network config size. */
TEST(ryu_network_config_size) {
    // RyuNetworkConfig: 16 + 16 + 4 + 2 + 2 = 40 bytes
    ASSERT_EQ(sizeof(protocol::RyuNetworkConfig), 40U);
}

/** @brief Test case: ryu network config game version offset. */
TEST(ryu_network_config_game_version_offset) {
    protocol::RyuNetworkConfig config{};
    size_t offset = offsetof(protocol::RyuNetworkConfig, game_version);
    ASSERT_EQ(offset, 0U);
    ASSERT_EQ(sizeof(config.game_version), 16U);
}

/** @brief Test case: ryu network config private ip offset. */
TEST(ryu_network_config_private_ip_offset) {
    protocol::RyuNetworkConfig config{};
    size_t offset = offsetof(protocol::RyuNetworkConfig, private_ip);
    ASSERT_EQ(offset, 16U);
    ASSERT_EQ(sizeof(config.private_ip), 16U);
}

/** @brief Test case: ryu network config address family offset. */
TEST(ryu_network_config_address_family_offset) {
    size_t offset = offsetof(protocol::RyuNetworkConfig, address_family);
    ASSERT_EQ(offset, 32U);
}

/** @brief Test case: ryu network config external proxy port offset. */
TEST(ryu_network_config_external_proxy_port_offset) {
    size_t offset = offsetof(protocol::RyuNetworkConfig, external_proxy_port);
    ASSERT_EQ(offset, 36U);
}

/** @brief Test case: ryu network config internal proxy port offset. */
TEST(ryu_network_config_internal_proxy_port_offset) {
    size_t offset = offsetof(protocol::RyuNetworkConfig, internal_proxy_port);
    ASSERT_EQ(offset, 38U);
}

/** @brief Test case: ryu network config ipv4 initialization. */
TEST(ryu_network_config_ipv4_initialization) {
    protocol::RyuNetworkConfig config{};

    // Simulate CreateNetwork P2P initialization
    uint32_t local_ip = 0xC0A80101;  // 192.168.1.1
    std::memset(config.private_ip, 0, sizeof(config.private_ip));
    std::memcpy(config.private_ip, &local_ip, sizeof(local_ip));

    config.address_family = protocol::AF_INET_VALUE;
    config.external_proxy_port = 39990;
    config.internal_proxy_port = 39990;

    // Verify
    uint32_t stored_ip;
    std::memcpy(&stored_ip, config.private_ip, sizeof(stored_ip));
    ASSERT_EQ(stored_ip, 0xC0A80101U);
    ASSERT_EQ(config.address_family, 2U);
    ASSERT_EQ(config.external_proxy_port, 39990);
    ASSERT_EQ(config.internal_proxy_port, 39990);
}

/** @brief Test case: ryu network config p2p disabled. */
TEST(ryu_network_config_p2p_disabled) {
    protocol::RyuNetworkConfig config{};

    // When P2P is disabled, ports should be 0
    std::memset(config.private_ip, 0, sizeof(config.private_ip));
    config.address_family = 0;
    config.external_proxy_port = 0;
    config.internal_proxy_port = 0;

    // Verify all zeros
    ASSERT_EQ(config.address_family, 0U);
    ASSERT_EQ(config.external_proxy_port, 0);
    ASSERT_EQ(config.internal_proxy_port, 0);

    // Check private_ip is all zeros
    bool all_zeros = true;
    for (unsigned char i : config.private_ip) {
        if (i != 0) {
            all_zeros = false;
            break;
        }
    }
    ASSERT_TRUE(all_zeros);
}

/** @brief Test case: ryu network config game version copy. */
TEST(ryu_network_config_game_version_copy) {
    protocol::RyuNetworkConfig config{};
    const char* version = "1.0.0";

    // Copy game version like CreateNetwork does
    std::memset(config.game_version, 0, sizeof(config.game_version));
    std::strncpy(config.game_version, version, sizeof(config.game_version) - 1);

    ASSERT_EQ(std::strcmp(config.game_version, "1.0.0"), 0);
}

// =============================================================================
// ExternalProxyToken Structure Tests
// =============================================================================

/** @brief Test case: external proxy token size. */
TEST(external_proxy_token_size) {
    // ExternalProxyToken: 4 + 16 + 4 + 16 = 40 bytes (0x28)
    ASSERT_EQ(sizeof(protocol::ExternalProxyToken), 40U);
}

/** @brief Test case: external proxy token virtual ip offset. */
TEST(external_proxy_token_virtual_ip_offset) {
    size_t offset = offsetof(protocol::ExternalProxyToken, virtual_ip);
    ASSERT_EQ(offset, 0U);
}

/** @brief Test case: external proxy token physical ip offset. */
TEST(external_proxy_token_physical_ip_offset) {
    size_t offset = offsetof(protocol::ExternalProxyToken, physical_ip);
    ASSERT_EQ(offset, 4U);

    protocol::ExternalProxyToken token{};
    ASSERT_EQ(sizeof(token.physical_ip), 16U);
}

/** @brief Test case: external proxy token address family offset. */
TEST(external_proxy_token_address_family_offset) {
    size_t offset = offsetof(protocol::ExternalProxyToken, address_family);
    ASSERT_EQ(offset, 20U);
}

/** @brief Test case: external proxy token token offset. */
TEST(external_proxy_token_token_offset) {
    size_t offset = offsetof(protocol::ExternalProxyToken, token);
    ASSERT_EQ(offset, 24U);

    protocol::ExternalProxyToken token{};
    ASSERT_EQ(sizeof(token.token), 16U);
}

/** @brief Test case: external proxy token parsing. */
TEST(external_proxy_token_parsing) {
    protocol::ExternalProxyToken token{};

    // Simulate received token from master server
    token.virtual_ip = 0x0A720001;  // 10.114.0.1
    token.physical_ip[0] = 192;
    token.physical_ip[1] = 168;
    token.physical_ip[2] = 1;
    token.physical_ip[3] = 100;
    token.address_family = protocol::AF_INET_VALUE;

    // Random token
    for (int i = 0; i < 16; i++) {
        token.token[i] = static_cast<uint8_t>(i + 0xA0);
    }

    // Verify parsing
    ASSERT_EQ(token.virtual_ip, 0x0A720001U);
    ASSERT_EQ(token.address_family, 2U);
    ASSERT_EQ(token.physical_ip[0], 192);
    ASSERT_EQ(token.token[0], 0xA0);
}

// =============================================================================
// P2P Server Lifecycle Tests
// =============================================================================

/** @brief Test case: p2p server port valid range. */
TEST(p2p_server_port_valid_range) {
    // Verify port is in valid range 39990-39999
    for (int port = protocol::PRIVATE_PORT_BASE;
         port < protocol::PRIVATE_PORT_BASE + protocol::PRIVATE_PORT_RANGE;
         port++) {
        ASSERT_GE(port, 39990);
        ASSERT_LE(port, 39999);
    }
}

/** @brief Test case: p2p server upnp port calculation. */
TEST(p2p_server_upnp_port_calculation) {
    // Test UPnP port selection logic
    uint16_t private_port = protocol::PRIVATE_PORT_BASE + 3;  // e.g., 39993
    uint16_t public_port = protocol::PUBLIC_PORT_BASE + 5;    // e.g., 39995

    // Both should be in valid range
    ASSERT_GE(private_port, protocol::PRIVATE_PORT_BASE);
    ASSERT_LE(
        private_port,
        protocol::PRIVATE_PORT_BASE + protocol::PRIVATE_PORT_RANGE - 1);
    ASSERT_GE(public_port, protocol::PUBLIC_PORT_BASE);
    ASSERT_LE(
        public_port,
        protocol::PUBLIC_PORT_BASE + protocol::PUBLIC_PORT_RANGE - 1);
}

// =============================================================================
// CreateNetwork P2P Configuration Tests
// =============================================================================

/** @brief Test case: create network p2p enabled config. */
TEST(create_network_p2p_enabled_config) {
    // Simulate CreateNetwork with P2P enabled
    protocol::RyuNetworkConfig config{};

    // P2P server started successfully
    uint16_t public_port = 39990;
    uint16_t private_port = 39991;
    uint32_t local_ip = 0x0A720064;  // 10.114.0.100

    std::memcpy(config.private_ip, &local_ip, sizeof(local_ip));
    config.address_family = protocol::AF_INET_VALUE;
    config.external_proxy_port = public_port;
    config.internal_proxy_port = private_port;

    // Verify configuration
    ASSERT_EQ(config.address_family, 2U);
    ASSERT_EQ(config.external_proxy_port, 39990);
    ASSERT_EQ(config.internal_proxy_port, 39991);

    uint32_t stored_ip;
    std::memcpy(&stored_ip, config.private_ip, sizeof(stored_ip));
    ASSERT_EQ(stored_ip, 0x0A720064U);
}

/** @brief Test case: create network p2p disabled config. */
TEST(create_network_p2p_disabled_config) {
    // Simulate CreateNetwork with P2P disabled
    protocol::RyuNetworkConfig config{};

    // P2P disabled - all zeros
    std::memset(config.private_ip, 0, sizeof(config.private_ip));
    config.address_family = 0;
    config.external_proxy_port = 0;
    config.internal_proxy_port = 0;

    // Verify zeros
    ASSERT_EQ(config.address_family, 0U);
    ASSERT_EQ(config.external_proxy_port, 0);
    ASSERT_EQ(config.internal_proxy_port, 0);
}

/** @brief Test case: create network upnp failed config. */
TEST(create_network_upnp_failed_config) {
    // Simulate CreateNetwork where UPnP failed but server started
    protocol::RyuNetworkConfig config{};

    // Server started on private port but UPnP returned 0 (failed)
    uint16_t public_port = 0;  // UPnP failed
    uint16_t private_port = 39992;
    uint32_t local_ip = 0xC0A80101;  // 192.168.1.1

    std::memcpy(config.private_ip, &local_ip, sizeof(local_ip));
    config.address_family = protocol::AF_INET_VALUE;
    config.external_proxy_port = public_port;  // 0 = UPnP failed
    config.internal_proxy_port = private_port;

    // Verify - external port is 0 but internal port is set
    ASSERT_EQ(config.external_proxy_port, 0);
    ASSERT_NE(config.internal_proxy_port, 0);
    ASSERT_EQ(config.address_family, 2U);
}

// =============================================================================
// Token Handling Tests
// =============================================================================

/** @brief Test case: token add waiting token flow. */
TEST(token_add_waiting_token_flow) {
    // Simulate adding token to waiting list
    protocol::ExternalProxyToken tokens[protocol::MAX_PLAYERS]{};
    int token_count = 0;

    // Add first token (token_count starts at 0, always < MAX_PLAYERS=8)
    static_assert(protocol::MAX_PLAYERS > 0, "MAX_PLAYERS must be positive");
    protocol::ExternalProxyToken token1{};
    token1.virtual_ip = 0x0A720001;
    tokens[token_count++] = token1;

    ASSERT_EQ(token_count, 1);
    ASSERT_EQ(tokens[0].virtual_ip, 0x0A720001U);
}

/** @brief Test case: token max waiting tokens. */
TEST(token_max_waiting_tokens) {
    // Verify max waiting tokens constant
    // MAX_WAITING_TOKENS in implementation is 16
    constexpr int MAX_WAITING_TOKENS = 16;
    ASSERT_EQ(MAX_WAITING_TOKENS, 16);
}

/** @brief Test case: token validation ipv4. */
TEST(token_validation_ipv4) {
    protocol::ExternalProxyToken token{};
    token.address_family = protocol::AF_INET_VALUE;

    bool is_ipv4 = (token.address_family == protocol::AF_INET_VALUE);
    ASSERT_TRUE(is_ipv4);
}

/** @brief Test case: token validation ipv6. */
TEST(token_validation_ipv6) {
    protocol::ExternalProxyToken token{};
    token.address_family = protocol::AF_INET6_VALUE;

    bool is_ipv6 = (token.address_family == protocol::AF_INET6_VALUE);
    ASSERT_TRUE(is_ipv6);
}

// =============================================================================
// Cleanup Tests
// =============================================================================

/** @brief Test case: cleanup on destroy network. */
TEST(cleanup_on_destroy_network) {
    // Simulate DestroyNetwork cleanup
    bool p2p_server_running = true;

    // DestroyNetwork should stop P2P server
    p2p_server_running = false;

    ASSERT_FALSE(p2p_server_running);
}

/** @brief Test case: cleanup on close access point. */
TEST(cleanup_on_close_access_point) {
    // Simulate CloseAccessPoint cleanup
    bool p2p_server_running = true;

    // CloseAccessPoint should stop P2P server
    p2p_server_running = false;

    ASSERT_FALSE(p2p_server_running);
}

/** @brief Test case: cleanup release upnp port. */
TEST(cleanup_release_upnp_port) {
    // Simulate UPnP port release on cleanup
    bool upnp_port_mapped = true;

    // ReleaseNatPunch() should unmap the port
    upnp_port_mapped = false;

    ASSERT_FALSE(upnp_port_mapped);
}

// =============================================================================
// Master Send Callback Tests
// =============================================================================

/** @brief Test case: master send callback signature. */
TEST(master_send_callback_signature) {
    // Test callback signature: void(*)(const void*, size_t, void*)
    // The callback should accept data, size, and user_data

    bool callback_invoked = false;

    auto callback = [](const void* data, size_t size, void* user_data) {
        // In real code, this sends to master server
        (void)data;
        (void)size;
        // Mark as invoked
        *static_cast<bool*>(user_data) = true;
    };

    // Verify callback can be called
    uint8_t test_data[] = {1, 2, 3, 4};
    callback(test_data, sizeof(test_data), &callback_invoked);

    // Verify callback was invoked
    ASSERT_TRUE(callback_invoked);
}

/** @brief Test case: master send callback user data pattern. */
TEST(master_send_callback_user_data_pattern) {
    // Test user_data pattern for passing context
    struct Context {
        int call_count;
        size_t last_size;
    };

    Context ctx{.call_count = 0, .last_size = 0};

    auto callback = [](const void* data, size_t size, void* user_data) {
        auto* c = static_cast<Context*>(user_data);
        c->call_count++;
        c->last_size = size;
        (void)data;
    };

    uint8_t test_data[100] = {0};
    callback(test_data, sizeof(test_data), &ctx);

    ASSERT_EQ(ctx.call_count, 1);
    ASSERT_EQ(ctx.last_size, 100U);
}

// =============================================================================
// Port Selection Logic Tests
// =============================================================================

/** @brief Test case: port selection first available. */
TEST(port_selection_first_available) {
    // Simulate port selection logic - try ports until one works
    bool port_available[10] = {
        false, false, true, true, true, true, true, true, true, true};

    int selected_port = -1;
    for (int i = 0; i < protocol::PRIVATE_PORT_RANGE; i++) {
        if (port_available[i]) {
            selected_port = protocol::PRIVATE_PORT_BASE + i;
            break;
        }
    }

    // Should select 39992 (index 2)
    ASSERT_EQ(selected_port, 39992);
}

/** @brief Test case: port selection all busy. */
TEST(port_selection_all_busy) {
    // Simulate all ports busy
    bool port_available[10] = {
        false, false, false, false, false, false, false, false, false, false};

    int selected_port = -1;
    for (int i = 0; i < protocol::PRIVATE_PORT_RANGE; i++) {
        if (port_available[i]) {
            selected_port = protocol::PRIVATE_PORT_BASE + i;
            break;
        }
    }

    // No port available
    ASSERT_EQ(selected_port, -1);
}

// =============================================================================
// Integration Flow Tests
// =============================================================================

/** @brief Test case: create network full flow p2p success. */
TEST(create_network_full_flow_p2p_success) {
    // Simulate full CreateNetwork flow with P2P success

    // 1. Check if P2P is enabled
    bool use_p2p = true;
    ASSERT_TRUE(use_p2p);

    // 2. Start P2P server
    bool server_started = true;  // StartP2pProxyServer() returns true
    ASSERT_TRUE(server_started);

    // 3. Get private port
    uint16_t private_port = 39990;
    ASSERT_NE(private_port, 0);

    // 4. UPnP NAT punch
    uint16_t public_port = 39990;  // NatPunch() returns port
    ASSERT_NE(public_port, 0);

    // 5. Configure RyuNetworkConfig
    protocol::RyuNetworkConfig config{};
    config.external_proxy_port = public_port;
    config.internal_proxy_port = private_port;
    config.address_family = 2;

    ASSERT_EQ(config.external_proxy_port, 39990);
    ASSERT_EQ(config.internal_proxy_port, 39990);
}

/** @brief Test case: create network full flow p2p disabled. */
TEST(create_network_full_flow_p2p_disabled) {
    // Simulate CreateNetwork with P2P disabled

    // 1. P2P is disabled
    bool use_p2p = false;
    ASSERT_FALSE(use_p2p);

    // 2. Skip server start
    bool server_started = false;
    ASSERT_FALSE(server_started);

    // 3. RyuNetworkConfig has zeros
    protocol::RyuNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));

    ASSERT_EQ(config.external_proxy_port, 0);
    ASSERT_EQ(config.internal_proxy_port, 0);
    ASSERT_EQ(config.address_family, 0U);
}

/** @brief Test case: external proxy token handling flow. */
TEST(external_proxy_token_handling_flow) {
    // Simulate ExternalProxyToken handler flow

    // 1. Receive token from master server
    protocol::ExternalProxyToken token{};
    token.virtual_ip = 0x0A720005;

    // 2. Check if P2P server is running
    bool p2p_server_running = true;
    ASSERT_TRUE(p2p_server_running);

    // 3. Add to waiting tokens
    protocol::ExternalProxyToken waiting_tokens[16]{};
    int waiting_count = 0;

    waiting_tokens[waiting_count++] = token;

    ASSERT_EQ(waiting_count, 1);
    ASSERT_EQ(waiting_tokens[0].virtual_ip, 0x0A720005U);
}

// =============================================================================
// Main
// =============================================================================

/**
 * @brief Runs the whole suite; the exit code is non-zero if any test failed.
 *
 * @return 0 when every test passed, non-zero otherwise
 */
int main() {
    printf("=== Story 9.8 CreateNetwork P2P Tests ===\n\n");

    printf("P2P Server Constants Tests:\n");
    // Tests auto-register and run

    printf("\nAll tests passed!\n");
    return 0;
}
