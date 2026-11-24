/**
 * @file ldn_proxy_handler_tests.cpp
 * @brief Unit tests for LDN Proxy Handler
 *
 * Tests the P2P proxy management logic that handles virtual network
 * connections tunneled through the RyuLDN server.
 *
 * Test-First: These tests are written BEFORE the implementation.
 */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

// Include protocol types
#include "protocol/types.hpp"

// Include the proxy handler (will be created)
#include "ldn/ldn_proxy_handler.hpp"

// ============================================================================
// Test Framework (minimal)
// ============================================================================

/// Number of test cases that finished without a failed assertion.
static int g_tests_passed = 0;
/// Number of test cases that hit a failed assertion.
static int g_tests_failed = 0;

/// Defines a test function and registers it in the test table.
#define TEST(name)                            \
    static void test_##name();                \
    static struct test_##name##_register {    \
        test_##name##_register() {            \
            printf("  [TEST] %s... ", #name); \
            fflush(stdout);                   \
            test_##name();                    \
            printf("PASS\n");                 \
            g_tests_passed++;                 \
        }                                     \
    } test_##name##_instance;                 \
    static void test_##name()

/// Fails the current test when the expression is false.
#define ASSERT_TRUE(cond)                                         \
    do {                                                          \
        if (!(cond)) {                                            \
            printf(                                               \
                "FAIL\n    Assertion failed: %s\n    at %s:%d\n", \
                #cond,                                            \
                __FILE__,                                         \
                __LINE__);                                        \
            g_tests_failed++;                                     \
            return;                                               \
        }                                                         \
    } while (0)

/// Fails the current test when the expression is true.
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

/// Fails the current test when the two values differ.
#define ASSERT_EQ(a, b)                                         \
    do {                                                        \
        if ((a) != (b)) {                                       \
            printf(                                             \
                "FAIL\n    Expected: %s == %s\n    at %s:%d\n", \
                #a,                                             \
                #b,                                             \
                __FILE__,                                       \
                __LINE__);                                      \
            g_tests_failed++;                                   \
            return;                                             \
        }                                                       \
    } while (0)

// ============================================================================
// Test Helpers
// ============================================================================

namespace ldn = ryu_ldn::ldn;
using ryu_ldn::protocol::DisconnectReason;
using ryu_ldn::protocol::LdnHeader;
using ryu_ldn::protocol::PacketId;
using ryu_ldn::protocol::PROTOCOL_MAGIC;
using ryu_ldn::protocol::PROTOCOL_VERSION;
using ryu_ldn::protocol::ProtocolType;
using ryu_ldn::protocol::ProxyConfig;
using ryu_ldn::protocol::ProxyConnectRequest;
using ryu_ldn::protocol::ProxyConnectResponse;
using ryu_ldn::protocol::ProxyDataHeader;
using ryu_ldn::protocol::ProxyDisconnectMessage;
using ryu_ldn::protocol::ProxyInfo;

/**
 * @brief Create a test LdnHeader
 *
 * @param type packet id stored in the header
 * @param data_size payload size stored in the header
 * @return header with the protocol magic and version set
 */
static LdnHeader make_test_header(PacketId type, int32_t data_size) {
    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(type);
    header.data_size = data_size;
    return header;
}

/**
 * @brief Create a test ProxyInfo structure
 *
 * @param src_ip source IPv4 address, Ryujinx format
 * @param src_port source port
 * @param dest_ip destination IPv4 address, Ryujinx format
 * @param dest_port destination port
 * @param proto transport protocol
 * @return the filled ProxyInfo
 */
static ProxyInfo make_test_proxy_info(
    uint32_t src_ip,
    uint16_t src_port,
    uint32_t dest_ip,
    uint16_t dest_port,
    ProtocolType proto = ProtocolType::Udp) {
    ProxyInfo info{};
    info.source_ipv4 = src_ip;
    info.source_port = src_port;
    info.dest_ipv4 = dest_ip;
    info.dest_port = dest_port;
    info.protocol = proto;
    return info;
}

/// Records the proxy handler callbacks that ran and their last arguments.
static struct CallbackState {
    /// Callback state: config received.
    bool config_received = false;
    /// Callback state: proxy ip.
    uint32_t proxy_ip = 0;
    /// Callback state: proxy subnet mask.
    uint32_t proxy_subnet_mask = 0;

    /// Callback state: connect received.
    bool connect_received = false;
    /// Callback state: connect info.
    ProxyInfo connect_info{};

    /// Callback state: connect reply received.
    bool connect_reply_received = false;
    /// Callback state: connect reply info.
    ProxyInfo connect_reply_info{};

    /// Callback state: data received.
    bool data_received = false;
    /// Callback state: data info.
    ProxyInfo data_info{};
    /// Callback state: data payload.
    std::vector<uint8_t> data_payload;

    /// Callback state: disconnect received.
    bool disconnect_received = false;
    /// Callback state: disconnect info.
    ProxyInfo disconnect_info{};
    /// Callback state: disconnect reason.
    int32_t disconnect_reason = 0;

    /// Clears every flag and recorded argument.
    void reset() {
        config_received = false;
        proxy_ip = 0;
        proxy_subnet_mask = 0;
        connect_received = false;
        connect_info = {};
        connect_reply_received = false;
        connect_reply_info = {};
        data_received = false;
        data_info = {};
        data_payload.clear();
        disconnect_received = false;
        disconnect_info = {};
        disconnect_reason = 0;
    }
} g_callback_state;  ///< Shared instance written by the callbacks.

// Callback functions
/**
 * @brief Test helper: on proxy config.
 *
 * @param config ProxyConfig received, its ip and mask are recorded
 */
static void on_proxy_config(const ProxyConfig& config) {
    g_callback_state.config_received = true;
    g_callback_state.proxy_ip = config.proxy_ip;
    g_callback_state.proxy_subnet_mask = config.proxy_subnet_mask;
}

/**
 * @brief Test helper: on proxy connect.
 *
 * @param info ProxyInfo of the connect request, recorded
 */
static void on_proxy_connect(const ProxyInfo& info) {
    g_callback_state.connect_received = true;
    g_callback_state.connect_info = info;
}

/**
 * @brief Test helper: on proxy connect reply.
 *
 * @param info ProxyInfo of the connect reply, recorded
 */
static void on_proxy_connect_reply(const ProxyInfo& info) {
    g_callback_state.connect_reply_received = true;
    g_callback_state.connect_reply_info = info;
}

/**
 * @brief Test helper: on proxy data.
 *
 * @param info ProxyInfo of the data packet, recorded
 * @param data payload bytes, copied into the callback state
 * @param length payload size in bytes
 */
static void on_proxy_data(
    const ProxyInfo& info, const uint8_t* data, size_t length) {
    g_callback_state.data_received = true;
    g_callback_state.data_info = info;
    g_callback_state.data_payload.assign(data, data + length);
}

/**
 * @brief Test helper: on proxy disconnect.
 *
 * @param info ProxyInfo of the closed connection, recorded
 * @param reason disconnect reason, recorded
 */
static void on_proxy_disconnect(const ProxyInfo& info, int32_t reason) {
    g_callback_state.disconnect_received = true;
    g_callback_state.disconnect_info = info;
    g_callback_state.disconnect_reason = reason;
}

// ============================================================================
// Tests - Proxy Handler Construction
// ============================================================================

/** @brief Test case: proxy handler default construction. */
TEST(proxy_handler_default_construction) {
    ldn::LdnProxyHandler handler;
    ASSERT_FALSE(handler.is_configured());
}

/** @brief Test case: proxy handler initial state. */
TEST(proxy_handler_initial_state) {
    ldn::LdnProxyHandler handler;
    ASSERT_FALSE(handler.is_configured());
    ASSERT_EQ(handler.get_connection_count(), 0);
}

// ============================================================================
// Tests - ProxyConfig Handling
// ============================================================================

/** @brief Test case: proxy handler process config. */
TEST(proxy_handler_process_config) {
    g_callback_state.reset();

    ldn::LdnProxyHandler handler;
    handler.set_config_callback(on_proxy_config);

    ProxyConfig config{};
    config.proxy_ip = 0x0A720001;           // 10.114.0.1
    config.proxy_subnet_mask = 0xFFFF0000;  // 255.255.0.0

    LdnHeader header = make_test_header(PacketId::ProxyConfig, sizeof(config));
    handler.handle_proxy_config(header, config);

    ASSERT_TRUE(g_callback_state.config_received);
    ASSERT_EQ(g_callback_state.proxy_ip, 0x0A720001);
    ASSERT_EQ(g_callback_state.proxy_subnet_mask, 0xFFFF0000);
    ASSERT_TRUE(handler.is_configured());
}

/** @brief Test case: proxy handler config stores values. */
TEST(proxy_handler_config_stores_values) {
    ldn::LdnProxyHandler handler;

    ProxyConfig config{};
    config.proxy_ip = 0xC0A80001;           // 192.168.0.1
    config.proxy_subnet_mask = 0xFFFFFF00;  // 255.255.255.0

    LdnHeader header = make_test_header(PacketId::ProxyConfig, sizeof(config));
    handler.handle_proxy_config(header, config);

    ASSERT_EQ(handler.get_proxy_ip(), 0xC0A80001);
    ASSERT_EQ(handler.get_proxy_subnet_mask(), 0xFFFFFF00);
}

// ============================================================================
// Tests - ProxyConnect Handling
// ============================================================================

/** @brief Test case: proxy handler process connect. */
TEST(proxy_handler_process_connect) {
    g_callback_state.reset();

    ldn::LdnProxyHandler handler;
    handler.set_connect_callback(on_proxy_connect);

    ProxyConnectRequest req{};
    req.info = make_test_proxy_info(
        0x0A720001, 1234, 0x0A720002, 5678, ProtocolType::Tcp);

    LdnHeader header = make_test_header(PacketId::ProxyConnect, sizeof(req));
    handler.handle_proxy_connect(header, req);

    ASSERT_TRUE(g_callback_state.connect_received);
    ASSERT_EQ(g_callback_state.connect_info.source_ipv4, 0x0A720001);
    ASSERT_EQ(g_callback_state.connect_info.source_port, 1234);
    ASSERT_EQ(g_callback_state.connect_info.dest_ipv4, 0x0A720002);
    ASSERT_EQ(g_callback_state.connect_info.dest_port, 5678);
}

/** @brief Test case: proxy handler connect adds connection. */
TEST(proxy_handler_connect_adds_connection) {
    ldn::LdnProxyHandler handler;

    ASSERT_EQ(handler.get_connection_count(), 0);

    ProxyConnectRequest req{};
    req.info = make_test_proxy_info(0x0A720001, 1234, 0x0A720002, 5678);

    LdnHeader header = make_test_header(PacketId::ProxyConnect, sizeof(req));
    handler.handle_proxy_connect(header, req);

    ASSERT_EQ(handler.get_connection_count(), 1);
    ASSERT_TRUE(handler.has_connection(0x0A720001, 1234, 0x0A720002, 5678));
}

// ============================================================================
// Tests - ProxyConnectReply Handling
// ============================================================================

/** @brief Test case: proxy handler process connect reply. */
TEST(proxy_handler_process_connect_reply) {
    g_callback_state.reset();

    ldn::LdnProxyHandler handler;
    handler.set_connect_reply_callback(on_proxy_connect_reply);

    ProxyConnectResponse resp{};
    resp.info = make_test_proxy_info(0x0A720002, 5678, 0x0A720001, 1234);

    LdnHeader header =
        make_test_header(PacketId::ProxyConnectReply, sizeof(resp));
    handler.handle_proxy_connect_reply(header, resp);

    ASSERT_TRUE(g_callback_state.connect_reply_received);
    ASSERT_EQ(g_callback_state.connect_reply_info.source_ipv4, 0x0A720002);
    ASSERT_EQ(g_callback_state.connect_reply_info.dest_ipv4, 0x0A720001);
}

// ============================================================================
// Tests - ProxyData Handling
// ============================================================================

/** @brief Test case: proxy handler process data. */
TEST(proxy_handler_process_data) {
    g_callback_state.reset();

    ldn::LdnProxyHandler handler;
    handler.set_data_callback(on_proxy_data);

    ProxyDataHeader data_header{};
    data_header.info = make_test_proxy_info(0x0A720001, 1234, 0x0A720002, 5678);
    data_header.data_length = 4;

    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};

    LdnHeader header =
        make_test_header(PacketId::ProxyData, sizeof(data_header) + 4);
    handler.handle_proxy_data(header, data_header, payload, 4);

    ASSERT_TRUE(g_callback_state.data_received);
    ASSERT_EQ(g_callback_state.data_info.source_ipv4, 0x0A720001);
    ASSERT_EQ(g_callback_state.data_payload.size(), 4);
    ASSERT_EQ(g_callback_state.data_payload[0], 0xDE);
    ASSERT_EQ(g_callback_state.data_payload[3], 0xEF);
}

/** @brief Test case: proxy handler data without callback. */
TEST(proxy_handler_data_without_callback) {
    ldn::LdnProxyHandler handler;
    // No callback registered

    ProxyDataHeader data_header{};
    data_header.info = make_test_proxy_info(0x0A720001, 1234, 0x0A720002, 5678);
    data_header.data_length = 4;

    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};

    LdnHeader header =
        make_test_header(PacketId::ProxyData, sizeof(data_header) + 4);

    // Should not crash
    handler.handle_proxy_data(header, data_header, payload, 4);
}

/** @brief Test case: proxy handler data empty payload. */
TEST(proxy_handler_data_empty_payload) {
    g_callback_state.reset();

    ldn::LdnProxyHandler handler;
    handler.set_data_callback(on_proxy_data);

    ProxyDataHeader data_header{};
    data_header.info = make_test_proxy_info(0x0A720001, 1234, 0x0A720002, 5678);
    data_header.data_length = 0;

    LdnHeader header =
        make_test_header(PacketId::ProxyData, sizeof(data_header));
    handler.handle_proxy_data(header, data_header, nullptr, 0);

    ASSERT_TRUE(g_callback_state.data_received);
    ASSERT_EQ(g_callback_state.data_payload.size(), 0);
}

// ============================================================================
// Tests - ProxyDisconnect Handling
// ============================================================================

/** @brief Test case: proxy handler process disconnect. */
TEST(proxy_handler_process_disconnect) {
    g_callback_state.reset();

    ldn::LdnProxyHandler handler;
    handler.set_disconnect_callback(on_proxy_disconnect);

    // First establish a connection
    ProxyConnectRequest req{};
    req.info = make_test_proxy_info(0x0A720001, 1234, 0x0A720002, 5678);
    LdnHeader connect_header =
        make_test_header(PacketId::ProxyConnect, sizeof(req));
    handler.handle_proxy_connect(connect_header, req);

    ASSERT_EQ(handler.get_connection_count(), 1);

    // Then disconnect
    ProxyDisconnectMessage msg{};
    msg.info = make_test_proxy_info(0x0A720001, 1234, 0x0A720002, 5678);
    msg.disconnect_reason = static_cast<int32_t>(DisconnectReason::User);

    LdnHeader header = make_test_header(PacketId::ProxyDisconnect, sizeof(msg));
    handler.handle_proxy_disconnect(header, msg);

    ASSERT_TRUE(g_callback_state.disconnect_received);
    ASSERT_EQ(g_callback_state.disconnect_info.source_ipv4, 0x0A720001);
    ASSERT_EQ(
        g_callback_state.disconnect_reason,
        static_cast<int32_t>(DisconnectReason::User));
}

/** @brief Test case: proxy handler disconnect removes connection. */
TEST(proxy_handler_disconnect_removes_connection) {
    ldn::LdnProxyHandler handler;

    // Establish connection
    ProxyConnectRequest req{};
    req.info = make_test_proxy_info(0x0A720001, 1234, 0x0A720002, 5678);
    LdnHeader connect_header =
        make_test_header(PacketId::ProxyConnect, sizeof(req));
    handler.handle_proxy_connect(connect_header, req);

    ASSERT_EQ(handler.get_connection_count(), 1);

    // Disconnect
    ProxyDisconnectMessage msg{};
    msg.info = make_test_proxy_info(0x0A720001, 1234, 0x0A720002, 5678);
    msg.disconnect_reason = 0;

    LdnHeader header = make_test_header(PacketId::ProxyDisconnect, sizeof(msg));
    handler.handle_proxy_disconnect(header, msg);

    ASSERT_EQ(handler.get_connection_count(), 0);
    ASSERT_FALSE(handler.has_connection(0x0A720001, 1234, 0x0A720002, 5678));
}

// ============================================================================
// Tests - Multiple Connections
// ============================================================================

/** @brief Test case: proxy handler multiple connections. */
TEST(proxy_handler_multiple_connections) {
    ldn::LdnProxyHandler handler;

    // Add 3 connections
    for (uint16_t port = 1000; port < 1003; port++) {
        ProxyConnectRequest req{};
        req.info = make_test_proxy_info(0x0A720001, port, 0x0A720002, 5678);
        LdnHeader header =
            make_test_header(PacketId::ProxyConnect, sizeof(req));
        handler.handle_proxy_connect(header, req);
    }

    ASSERT_EQ(handler.get_connection_count(), 3);

    // Remove middle connection
    ProxyDisconnectMessage msg{};
    msg.info = make_test_proxy_info(0x0A720001, 1001, 0x0A720002, 5678);
    msg.disconnect_reason = 0;
    LdnHeader header = make_test_header(PacketId::ProxyDisconnect, sizeof(msg));
    handler.handle_proxy_disconnect(header, msg);

    ASSERT_EQ(handler.get_connection_count(), 2);
    ASSERT_TRUE(handler.has_connection(0x0A720001, 1000, 0x0A720002, 5678));
    ASSERT_FALSE(handler.has_connection(0x0A720001, 1001, 0x0A720002, 5678));
    ASSERT_TRUE(handler.has_connection(0x0A720001, 1002, 0x0A720002, 5678));
}

// ============================================================================
// Tests - Reset
// ============================================================================

/** @brief Test case: proxy handler reset. */
TEST(proxy_handler_reset) {
    ldn::LdnProxyHandler handler;

    // Configure
    ProxyConfig config{};
    config.proxy_ip = 0x0A720001;
    config.proxy_subnet_mask = 0xFFFF0000;
    LdnHeader config_header =
        make_test_header(PacketId::ProxyConfig, sizeof(config));
    handler.handle_proxy_config(config_header, config);

    // Add connections
    for (int i = 0; i < 3; i++) {
        ProxyConnectRequest req{};
        req.info = make_test_proxy_info(
            0x0A720001, static_cast<uint16_t>(1000 + i), 0x0A720002, 5678);
        LdnHeader header =
            make_test_header(PacketId::ProxyConnect, sizeof(req));
        handler.handle_proxy_connect(header, req);
    }

    ASSERT_TRUE(handler.is_configured());
    ASSERT_EQ(handler.get_connection_count(), 3);

    // Reset
    handler.reset();

    ASSERT_FALSE(handler.is_configured());
    ASSERT_EQ(handler.get_connection_count(), 0);
    ASSERT_EQ(handler.get_proxy_ip(), 0);
}

// ============================================================================
// Tests - Protocol Type Filtering
// ============================================================================

/** @brief Test case: proxy handler tcp and udp separate. */
TEST(proxy_handler_tcp_and_udp_separate) {
    ldn::LdnProxyHandler handler;

    // Add TCP connection
    ProxyConnectRequest tcp_req{};
    tcp_req.info = make_test_proxy_info(
        0x0A720001, 1234, 0x0A720002, 5678, ProtocolType::Tcp);
    LdnHeader tcp_header =
        make_test_header(PacketId::ProxyConnect, sizeof(tcp_req));
    handler.handle_proxy_connect(tcp_header, tcp_req);

    // Add UDP connection with same ports
    ProxyConnectRequest udp_req{};
    udp_req.info = make_test_proxy_info(
        0x0A720001, 1234, 0x0A720002, 5678, ProtocolType::Udp);
    LdnHeader udp_header =
        make_test_header(PacketId::ProxyConnect, sizeof(udp_req));
    handler.handle_proxy_connect(udp_header, udp_req);

    // Should have 2 separate connections
    ASSERT_EQ(handler.get_connection_count(), 2);
}

/// Test fixture value: saturation callbacks.
static int g_saturation_callbacks = 0;
/// Test helper: on saturation connect.
static void on_saturation_connect(const ProxyInfo& /*unused*/) {
    ++g_saturation_callbacks;
}

/**
 * @brief Test case: proxy connect table full drops new connection but still
 *   notifies.
 */
TEST(proxy_connect_table_full_drops_new_connection_but_still_notifies) {
    g_saturation_callbacks = 0;
    ldn::LdnProxyHandler handler;
    handler.set_connect_callback(on_saturation_connect);

    const size_t capacity = ldn::LdnProxyHandler::MAX_PROXY_CONNECTIONS;
    for (size_t i = 0; i < capacity; ++i) {
        ProxyConnectRequest req{};
        req.info = make_test_proxy_info(
            0x0A720001, static_cast<uint16_t>(1000 + i), 0x0A720002, 5678);
        handler.handle_proxy_connect(
            make_test_header(PacketId::ProxyConnect, sizeof(req)), req);
    }
    ASSERT_EQ(handler.get_connection_count(), capacity);

    // One more: the table is full, the entry is dropped, the count is
    // unchanged.
    ProxyConnectRequest overflow{};
    overflow.info = make_test_proxy_info(0x0A720001, 60000, 0x0A720002, 5678);
    handler.handle_proxy_connect(
        make_test_header(PacketId::ProxyConnect, sizeof(overflow)), overflow);

    ASSERT_EQ(handler.get_connection_count(), capacity);
    ASSERT_FALSE(handler.has_connection(0x0A720001, 60000, 0x0A720002, 5678));
    // The application is still notified of the connect attempt.
    ASSERT_EQ(g_saturation_callbacks, static_cast<int>(capacity) + 1);
}

// ============================================================================
// Main
// ============================================================================

/**
 * @brief Runs the whole suite; the exit code is non-zero if any test failed.
 *
 * @return 0 when every test passed, non-zero otherwise
 */
int main() {
    printf("\n========================================\n");
    printf("  LDN Proxy Handler Tests\n");
    printf("========================================\n\n");

    // Tests run automatically via static initializers

    printf("\n========================================\n");
    printf(
        "  Results: %d/%d passed\n",
        g_tests_passed,
        g_tests_passed + g_tests_failed);
    printf("========================================\n\n");

    return g_tests_failed > 0 ? 1 : 0;
}
