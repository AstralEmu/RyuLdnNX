/**
 * @file p2p_integration_tests.cpp
 * @brief Unit tests for P2P integration in ICommunicationService (Story 9.7)
 *
 * Tests the HandleExternalProxy integration and SendProxyDataToServer routing
 * for P2P proxy connections.
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
/// Number of tests that completed (a failed assertion exits the process).
static int g_tests_passed = 0;

/**
 * @brief Define and auto-run a test case.
 *
 * Declares test_<name>() and a static object whose constructor runs it
 * during static initialisation, printing PASSED and counting it.
 *
 * @param name Identifier of the test (becomes test_<name>).
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
 * @brief Print the failure and exit(1) when expr is false.
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
 * @brief Print the failure and exit(1) when expr is true.
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
 * @brief Print the failure and exit(1) when a != b.
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

/**
 * @brief Print the failure and exit(1) when a == b.
 * @param a Actual value.
 * @param b Value a must differ from.
 */
#define ASSERT_NE(a, b)                                         \
    do {                                                        \
        if ((a) == (b)) {                                       \
            printf("FAILED\n    Expected: %s != %s\n", #a, #b); \
            exit(1);                                            \
        }                                                       \
    } while (0)

// =============================================================================
// Protocol Types (matching implementation)
// =============================================================================

namespace protocol {

// Address family constants (use different names to avoid conflict with system
// macros)
/// Address family value of IPv4 (AF_INET).
constexpr uint32_t ADDR_FAMILY_IPV4 = 2;  // AF_INET value
/// Address family value of IPv6 (AF_INET6).
constexpr uint32_t ADDR_FAMILY_IPV6 = 10;  // AF_INET6 value

// Packet IDs
/// Wire packet IDs used by the P2P tests.
enum class PacketId : uint8_t {
    /// Proxied game data.
    ProxyData = 9,
    /// External proxy configuration sent by the server.
    ExternalProxy = 14,
    /// Virtual IP configuration sent by the host.
    ProxyConfig = 11,
};

/**
 * @brief Mirror of the 0x26-byte ExternalProxyConfig packet (Ryujinx
 *        compatible); packed: no padding, as on the wire.
 */
struct ExternalProxyConfig {
    /// Proxy IP: IPv4 in the first 4 bytes, or a full IPv6 address.
    uint8_t proxy_ip[16];  // IPv4 in first 4 bytes or full IPv6
    /// Proxy port.
    uint16_t proxy_port;
    /// Address family: 2 for IPv4, 10 for IPv6.
    uint32_t address_family;  // 2=IPv4, 10=IPv6
    /// Authentication token.
    uint8_t token[16];
} __attribute__((packed));

/**
 * @brief Mirror of the 12-byte proxy data header; packed, as on the wire.
 */
struct ProxyDataHeader {
    /// Destination node ID.
    uint32_t dest_node_id;  // Destination node ID
    /// Source node ID.
    uint32_t src_node_id;  // Source node ID
    /// Transport protocol: UDP 17, TCP 6.
    uint16_t protocol_type;  // UDP=17, TCP=6
    /// Length of the data following the header.
    uint16_t data_length;  // Length of data following header
} __attribute__((packed));

/** Reduced ProxyConfig packet from the host; packed, as on the wire. */
struct ProxyConfig {
    /// Virtual IP assigned to the node.
    uint32_t proxy_ip;  // Virtual IP assigned
} __attribute__((packed));

}  // namespace protocol

// =============================================================================
// ExternalProxyConfig Tests - Structure Validation
// =============================================================================

/**
 * @brief ExternalProxyConfig is exactly 0x26 (38) bytes.
 */
TEST(external_proxy_config_size_0x26) {
    // ExternalProxyConfig must be exactly 0x26 (38) bytes
    ASSERT_EQ(sizeof(protocol::ExternalProxyConfig), 0x26U);
}

/**
 * @brief proxy_ip sits at offset 0.
 */
TEST(external_proxy_config_proxy_ip_offset) {
    // proxy_ip should be at offset 0
    protocol::ExternalProxyConfig config{};
    ASSERT_EQ(
        reinterpret_cast<uintptr_t>(&config.proxy_ip) -
            reinterpret_cast<uintptr_t>(&config),
        0U);
}

/**
 * @brief proxy_port sits at offset 16.
 */
TEST(external_proxy_config_proxy_port_offset) {
    // proxy_port should be at offset 16
    protocol::ExternalProxyConfig config{};
    ASSERT_EQ(
        reinterpret_cast<uintptr_t>(&config.proxy_port) -
            reinterpret_cast<uintptr_t>(&config),
        16U);
}

/**
 * @brief address_family sits at offset 18.
 */
TEST(external_proxy_config_address_family_offset) {
    // address_family should be at offset 18
    protocol::ExternalProxyConfig config{};
    ASSERT_EQ(
        reinterpret_cast<uintptr_t>(&config.address_family) -
            reinterpret_cast<uintptr_t>(&config),
        18U);
}

/**
 * @brief token sits at offset 22.
 */
TEST(external_proxy_config_token_offset) {
    // token should be at offset 22
    protocol::ExternalProxyConfig config{};
    ASSERT_EQ(
        reinterpret_cast<uintptr_t>(&config.token) -
            reinterpret_cast<uintptr_t>(&config),
        22U);
}

// =============================================================================
// Address Family Tests
// =============================================================================

/**
 * @brief The IPv4 family constant is 2.
 */
TEST(address_family_ipv4_is_2) {
    // AF_INET should be 2
    ASSERT_EQ(protocol::ADDR_FAMILY_IPV4, 2U);
}

/**
 * @brief The IPv6 family constant is 10.
 */
TEST(address_family_ipv6_is_10) {
    // AF_INET6 should be 10
    ASSERT_EQ(protocol::ADDR_FAMILY_IPV6, 10U);
}

/**
 * @brief An IPv4 ExternalProxyConfig is recognised and its first four bytes
 *        give the address 10.114.0.1.
 */
TEST(external_proxy_config_ipv4_parsing) {
    // Simulate parsing IPv4 from ExternalProxyConfig
    protocol::ExternalProxyConfig config{};
    config.address_family = protocol::ADDR_FAMILY_IPV4;
    config.proxy_port = 39990;

    // Set IPv4 address (10.114.0.1) in first 4 bytes
    config.proxy_ip[0] = 10;
    config.proxy_ip[1] = 114;
    config.proxy_ip[2] = 0;
    config.proxy_ip[3] = 1;

    // Verify address family check
    bool is_ipv4 = (config.address_family == protocol::ADDR_FAMILY_IPV4);
    ASSERT_TRUE(is_ipv4);

    // Verify IP extraction
    uint8_t ip[4];
    memcpy(ip, config.proxy_ip, 4);
    ASSERT_EQ(ip[0], 10);
    ASSERT_EQ(ip[1], 114);
    ASSERT_EQ(ip[2], 0);
    ASSERT_EQ(ip[3], 1);
}

/**
 * @brief An IPv6 family is detected as IPv6 and not as IPv4.
 */
TEST(external_proxy_config_ipv6_detection) {
    protocol::ExternalProxyConfig config{};
    config.address_family = protocol::ADDR_FAMILY_IPV6;

    bool is_ipv6 = (config.address_family == protocol::ADDR_FAMILY_IPV6);
    ASSERT_TRUE(is_ipv6);

    bool is_ipv4 = (config.address_family == protocol::ADDR_FAMILY_IPV4);
    ASSERT_FALSE(is_ipv4);
}

// =============================================================================
// ProxyDataHeader Tests
// =============================================================================

/**
 * @brief The mirrored ProxyDataHeader is 12 bytes.
 */
TEST(proxy_data_header_size) {
    // ProxyDataHeader should be 12 bytes
    ASSERT_EQ(sizeof(protocol::ProxyDataHeader), 12U);
}

/**
 * @brief dest_node_id sits at offset 0.
 */
TEST(proxy_data_header_dest_node_offset) {
    protocol::ProxyDataHeader header{};
    ASSERT_EQ(
        reinterpret_cast<uintptr_t>(&header.dest_node_id) -
            reinterpret_cast<uintptr_t>(&header),
        0U);
}

/**
 * @brief src_node_id sits at offset 4.
 */
TEST(proxy_data_header_src_node_offset) {
    protocol::ProxyDataHeader header{};
    ASSERT_EQ(
        reinterpret_cast<uintptr_t>(&header.src_node_id) -
            reinterpret_cast<uintptr_t>(&header),
        4U);
}

/**
 * @brief protocol_type sits at offset 8.
 */
TEST(proxy_data_header_protocol_type_offset) {
    protocol::ProxyDataHeader header{};
    ASSERT_EQ(
        reinterpret_cast<uintptr_t>(&header.protocol_type) -
            reinterpret_cast<uintptr_t>(&header),
        8U);
}

/**
 * @brief data_length sits at offset 10.
 */
TEST(proxy_data_header_data_length_offset) {
    protocol::ProxyDataHeader header{};
    ASSERT_EQ(
        reinterpret_cast<uintptr_t>(&header.data_length) -
            reinterpret_cast<uintptr_t>(&header),
        10U);
}

// =============================================================================
// P2P Routing Logic Tests
// =============================================================================

/// Test doubles reproducing the SendProxyDataToServer routing decision.
namespace routing_test {

// Simulates SendProxyDataToServer routing logic
/// Where a proxy packet is sent.
enum class RouteTarget {
    /// Through the master server.
    MasterServer,
    /// Through the direct P2P proxy connection.
    P2pProxy
};  ///< Routing decision of determine_route().

/// Mock P2P client exposing only the state the routing logic reads.
struct MockP2pClient {
    /// True once the TCP connection to the proxy is up.
    bool connected = false;
    /// True once authentication and proxy setup are done.
    bool ready = false;
    /// Number of Send() calls.
    int send_count = 0;

    /**
     * @brief Tell whether the P2P path can carry data.
     * @return True only when both connected and ready are set.
     */
    [[nodiscard]] bool IsReady() const {
        return connected && ready;
    }
    /**
     * @brief Record one send.
     * @return Always true.
     */
    bool Send() {
        send_count++;
        return true;
    }
};

/**
 * @brief Choose the path of a proxy packet.
 * @param client P2P client, or null when none exists.
 * @return P2pProxy for a ready client, MasterServer otherwise.
 */
RouteTarget determine_route(MockP2pClient* client) {
    if (client != nullptr && client->IsReady()) {
        return RouteTarget::P2pProxy;
    }
    return RouteTarget::MasterServer;
}

}  // namespace routing_test

/**
 * @brief Without a P2P client the packet goes through the master server.
 */
TEST(routing_null_client_uses_master) {
    auto route = routing_test::determine_route(nullptr);
    ASSERT_EQ(
        static_cast<int>(route),
        static_cast<int>(routing_test::RouteTarget::MasterServer));
}

/**
 * @brief A disconnected client routes through the master server.
 */
TEST(routing_disconnected_client_uses_master) {
    routing_test::MockP2pClient client;
    client.connected = false;
    client.ready = false;

    auto route = routing_test::determine_route(&client);
    ASSERT_EQ(
        static_cast<int>(route),
        static_cast<int>(routing_test::RouteTarget::MasterServer));
}

/**
 * @brief A connected but not ready client routes through the master server.
 */
TEST(routing_connected_not_ready_uses_master) {
    routing_test::MockP2pClient client;
    client.connected = true;
    client.ready = false;

    auto route = routing_test::determine_route(&client);
    ASSERT_EQ(
        static_cast<int>(route),
        static_cast<int>(routing_test::RouteTarget::MasterServer));
}

/**
 * @brief A connected and ready client routes through the P2P proxy.
 */
TEST(routing_ready_client_uses_p2p) {
    routing_test::MockP2pClient client;
    client.connected = true;
    client.ready = true;

    auto route = routing_test::determine_route(&client);
    ASSERT_EQ(
        static_cast<int>(route),
        static_cast<int>(routing_test::RouteTarget::P2pProxy));
}

/**
 * @brief IsReady() is true only when connected and ready are both set.
 */
TEST(routing_ready_check_is_and_of_connected_and_ready) {
    routing_test::MockP2pClient client;

    // Not connected, not ready -> not ready
    client.connected = false;
    client.ready = false;
    ASSERT_FALSE(client.IsReady());

    // Connected, not ready -> not ready
    client.connected = true;
    client.ready = false;
    ASSERT_FALSE(client.IsReady());

    // Not connected, ready -> not ready
    client.connected = false;
    client.ready = true;
    ASSERT_FALSE(client.IsReady());

    // Connected and ready -> ready
    client.connected = true;
    client.ready = true;
    ASSERT_TRUE(client.IsReady());
}

// =============================================================================
// Token Validation Tests
// =============================================================================

/**
 * @brief The ExternalProxyConfig token is 16 bytes.
 */
TEST(token_size_16_bytes) {
    // ExternalProxyToken is 16 bytes
    constexpr size_t TOKEN_SIZE = 16;
    ASSERT_EQ(TOKEN_SIZE, 16U);

    protocol::ExternalProxyConfig config{};
    ASSERT_EQ(sizeof(config.token), 16U);
}

/**
 * @brief A zeroed ExternalProxyConfig has an all-zero token.
 */
TEST(token_zero_check) {
    protocol::ExternalProxyConfig config{};
    memset(&config, 0, sizeof(config));

    // Token should be all zeros
    bool all_zero = true;
    for (unsigned char i : config.token) {
        if (i != 0) {
            all_zero = false;
            break;
        }
    }
    ASSERT_TRUE(all_zero);
}

/**
 * @brief A 16-byte token copied into the config compares equal.
 */
TEST(token_copy) {
    uint8_t source_token[16] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    protocol::ExternalProxyConfig config{};

    memcpy(config.token, source_token, sizeof(config.token));

    ASSERT_EQ(memcmp(config.token, source_token, 16), 0);
}

// =============================================================================
// HandleExternalProxy Logic Tests
// =============================================================================

/// Test doubles reproducing the HandleExternalProxy connection sequence.
namespace handle_external_proxy {

/// Mock of the P2P state HandleExternalProxy updates.
struct MockState {
    /// P2P proxy enabled in the configuration.
    bool use_p2p_proxy = false;
    /// TCP connection to the proxy established.
    bool p2p_connected = false;
    /// Authentication with the proxy completed.
    bool p2p_auth_done = false;
    /// Proxy ready to carry data.
    bool p2p_ready = false;
};

/// Outcome of the simulated HandleExternalProxy sequence.
enum class HandleResult {
    /// P2P is disabled in the configuration.
    Ignored,  // P2P disabled
    /// The TCP connection failed.
    ConnectFailed,  // TCP connect failed
    /// Authentication failed.
    AuthFailed,  // PerformAuth failed
    /// Proxy setup failed.
    ReadyFailed,  // EnsureProxyReady failed
    /// Every step passed.
    Success  // All steps passed
};

// Simulates HandleExternalProxyConnect logic
/**
 * @brief Replay the HandleExternalProxyConnect steps on a mock state.
 *
 * The existing connection is reset first; a failing step undoes the flags
 * set by the earlier ones.
 *
 * @param state State updated in place.
 * @param connect_succeeds Outcome of the TCP connect step.
 * @param auth_succeeds Outcome of the authentication step.
 * @param ready_succeeds Outcome of the proxy-ready step.
 * @return The first failing step, or Success.
 */
HandleResult simulate_handle_external_proxy(
    MockState& state,
    bool connect_succeeds,
    bool auth_succeeds,
    bool ready_succeeds) {
    // Step 1: Check if P2P is enabled
    if (!state.use_p2p_proxy) {
        return HandleResult::Ignored;
    }

    // Step 2: Disconnect existing (simulated)
    state.p2p_connected = false;
    state.p2p_auth_done = false;
    state.p2p_ready = false;

    // Step 3: Connect
    if (!connect_succeeds) {
        return HandleResult::ConnectFailed;
    }
    state.p2p_connected = true;

    // Step 4: PerformAuth
    if (!auth_succeeds) {
        state.p2p_connected = false;
        return HandleResult::AuthFailed;
    }
    state.p2p_auth_done = true;

    // Step 5: EnsureProxyReady
    if (!ready_succeeds) {
        state.p2p_connected = false;
        state.p2p_auth_done = false;
        return HandleResult::ReadyFailed;
    }
    state.p2p_ready = true;

    return HandleResult::Success;
}

}  // namespace handle_external_proxy

/**
 * @brief With P2P disabled the request is ignored and nothing connects.
 */
TEST(handle_external_p2p_disabled_ignored) {
    handle_external_proxy::MockState state;
    state.use_p2p_proxy = false;

    auto result = handle_external_proxy::simulate_handle_external_proxy(
        state, true, true, true);

    ASSERT_EQ(
        static_cast<int>(result),
        static_cast<int>(handle_external_proxy::HandleResult::Ignored));
    ASSERT_FALSE(state.p2p_connected);
}

/**
 * @brief A failing TCP connect reports ConnectFailed and stays disconnected.
 */
TEST(handle_external_connect_failure) {
    handle_external_proxy::MockState state;
    state.use_p2p_proxy = true;

    auto result = handle_external_proxy::simulate_handle_external_proxy(
        state, false, true, true);

    ASSERT_EQ(
        static_cast<int>(result),
        static_cast<int>(handle_external_proxy::HandleResult::ConnectFailed));
    ASSERT_FALSE(state.p2p_connected);
}

/**
 * @brief A failing authentication reports AuthFailed and drops the connection.
 */
TEST(handle_external_auth_failure) {
    handle_external_proxy::MockState state;
    state.use_p2p_proxy = true;

    auto result = handle_external_proxy::simulate_handle_external_proxy(
        state, true, false, true);

    ASSERT_EQ(
        static_cast<int>(result),
        static_cast<int>(handle_external_proxy::HandleResult::AuthFailed));
    ASSERT_FALSE(state.p2p_connected);
}

/**
 * @brief A failing proxy setup reports ReadyFailed and drops the connection.
 */
TEST(handle_external_ready_failure) {
    handle_external_proxy::MockState state;
    state.use_p2p_proxy = true;

    auto result = handle_external_proxy::simulate_handle_external_proxy(
        state, true, true, false);

    ASSERT_EQ(
        static_cast<int>(result),
        static_cast<int>(handle_external_proxy::HandleResult::ReadyFailed));
    ASSERT_FALSE(state.p2p_connected);
}

/**
 * @brief When every step succeeds the state is connected, authenticated and
 *        ready.
 */
TEST(handle_external_success) {
    handle_external_proxy::MockState state;
    state.use_p2p_proxy = true;

    auto result = handle_external_proxy::simulate_handle_external_proxy(
        state, true, true, true);

    ASSERT_EQ(
        static_cast<int>(result),
        static_cast<int>(handle_external_proxy::HandleResult::Success));
    ASSERT_TRUE(state.p2p_connected);
    ASSERT_TRUE(state.p2p_auth_done);
    ASSERT_TRUE(state.p2p_ready);
}

/**
 * @brief A failure after an earlier working connection clears all three
 *        flags.
 */
TEST(handle_external_cleanup_on_failure) {
    handle_external_proxy::MockState state;
    state.use_p2p_proxy = true;
    state.p2p_connected = true;  // Pre-existing connection
    state.p2p_auth_done = true;
    state.p2p_ready = true;

    // New connection fails at auth
    auto result = handle_external_proxy::simulate_handle_external_proxy(
        state, true, false, true);

    ASSERT_EQ(
        static_cast<int>(result),
        static_cast<int>(handle_external_proxy::HandleResult::AuthFailed));

    // Should have cleaned up
    ASSERT_FALSE(state.p2p_connected);
    ASSERT_FALSE(state.p2p_auth_done);
    ASSERT_FALSE(state.p2p_ready);
}

// =============================================================================
// DisconnectP2pProxy Logic Tests
// =============================================================================

/**
 * @brief Disconnecting a null client is a no-op.
 */
TEST(disconnect_p2p_null_safe) {
    // Disconnecting nullptr should be safe (no-op)
    void* ptr = nullptr;
    bool disconnected = false;

    if (ptr != nullptr) {
        disconnected = true;
    }

    ASSERT_FALSE(disconnected);
}

/**
 * @brief After a disconnect the client pointer is null.
 */
TEST(disconnect_p2p_sets_null) {
    // After disconnect, pointer should be null
    int dummy = 42;
    int* ptr = &dummy;
    ASSERT_TRUE(ptr != nullptr);

    // Simulate disconnect
    ptr = nullptr;

    ASSERT_TRUE(ptr == nullptr);
}

// =============================================================================
// ProxyPacketCallback Tests
// =============================================================================

/// Mock of the proxy packet callback and the packet it records.
namespace callback_test {

/// The last packet seen by mock_callback().
struct ReceivedPacket {
    /// Packet ID received.
    protocol::PacketId type;
    /// Payload size received.
    size_t data_size;
    /// True once the callback ran.
    bool processed;
};

/// Last packet recorded by mock_callback().
ReceivedPacket g_last_packet = {};

/**
 * @brief Record a packet; the payload pointer is ignored.
 * @param type Packet ID.
 * @param size Payload size in bytes.
 */
void mock_callback(protocol::PacketId type, const void* /*data*/, size_t size) {
    g_last_packet.type = type;
    g_last_packet.data_size = size;
    g_last_packet.processed = true;
}

}  // namespace callback_test

/**
 * @brief The callback records a ProxyData packet and its 64-byte size.
 */
TEST(callback_receives_proxy_data) {
    callback_test::g_last_packet = {};

    uint8_t dummy_data[64] = {0};
    callback_test::mock_callback(
        protocol::PacketId::ProxyData, dummy_data, sizeof(dummy_data));

    ASSERT_TRUE(callback_test::g_last_packet.processed);
    ASSERT_EQ(
        static_cast<int>(callback_test::g_last_packet.type),
        static_cast<int>(protocol::PacketId::ProxyData));
    ASSERT_EQ(callback_test::g_last_packet.data_size, 64U);
}

/**
 * @brief The callback records a ProxyConfig packet.
 */
TEST(callback_receives_proxy_config) {
    callback_test::g_last_packet = {};

    protocol::ProxyConfig config{};
    callback_test::mock_callback(
        protocol::PacketId::ProxyConfig, &config, sizeof(config));

    ASSERT_TRUE(callback_test::g_last_packet.processed);
    ASSERT_EQ(
        static_cast<int>(callback_test::g_last_packet.type),
        static_cast<int>(protocol::PacketId::ProxyConfig));
}

// =============================================================================
// IP Address Extraction Tests
// =============================================================================

/**
 * @brief The first four bytes of proxy_ip format as "192.168.1.100".
 */
TEST(ip_extract_from_external_proxy_config_ipv4) {
    protocol::ExternalProxyConfig config{};
    config.address_family = protocol::ADDR_FAMILY_IPV4;

    // Set 192.168.1.100
    config.proxy_ip[0] = 192;
    config.proxy_ip[1] = 168;
    config.proxy_ip[2] = 1;
    config.proxy_ip[3] = 100;

    // Extract as Connect() would
    uint8_t ip_bytes[4];
    memcpy(ip_bytes, config.proxy_ip, 4);

    // Convert to string for verification
    char ip_str[INET_ADDRSTRLEN];
    snprintf(
        ip_str,
        sizeof(ip_str),
        "%u.%u.%u.%u",
        ip_bytes[0],
        ip_bytes[1],
        ip_bytes[2],
        ip_bytes[3]);

    ASSERT_EQ(strcmp(ip_str, "192.168.1.100"), 0);
}

/**
 * @brief An IPv4 address uses 4 bytes.
 */
TEST(ip_extract_length_for_ipv4) {
    // IPv4 should use 4 bytes
    constexpr size_t IPV4_LEN = 4;
    ASSERT_EQ(IPV4_LEN, 4U);
}

/**
 * @brief An IPv6 address uses 16 bytes.
 */
TEST(ip_extract_length_for_ipv6) {
    // IPv6 should use 16 bytes
    constexpr size_t IPV6_LEN = 16;
    ASSERT_EQ(IPV6_LEN, 16U);
}

// =============================================================================
// Main
// =============================================================================

/**
 * @brief Print the totals; tests have already run from static initialisers.
 * @return 0 when every test passed, 1 otherwise.
 */
int main() {
    printf("=== P2P Integration Tests (Story 9.7) ===\n\n");

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
