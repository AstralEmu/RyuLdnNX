/**
 * @file ldn_packet_dispatcher_tests.cpp
 * @brief Unit tests for LDN Packet Dispatcher
 *
 * Tests the packet dispatch system that routes incoming packets
 * to registered handlers based on PacketId.
 *
 * Test-First: These tests are written BEFORE the implementation.
 */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

// Include protocol types
#include "protocol/types.hpp"

// Include the dispatcher (will be created)
#include "ldn/ldn_packet_dispatcher.hpp"

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

using namespace ryu_ldn;
using namespace ryu_ldn::protocol;

/// Records which dispatcher callbacks ran and the last arguments they got.
static struct CallbackState {
    /// Callback state: initialize called.
    bool initialize_called{false};
    /// Callback state: connected called.
    bool connected_called{false};
    /// Callback state: sync network called.
    bool sync_network_called{false};
    /// Callback state: scan reply called.
    bool scan_reply_called{false};
    /// Callback state: scan reply end called.
    bool scan_reply_end_called{false};
    /// Callback state: disconnect called.
    bool disconnect_called{false};
    /// Callback state: ping called.
    bool ping_called{false};
    /// Callback state: network error called.
    bool network_error_called{false};
    /// Callback state: proxy config called.
    bool proxy_config_called{false};
    /// Callback state: proxy connect called.
    bool proxy_connect_called{false};
    /// Callback state: proxy connect reply called.
    bool proxy_connect_reply_called{false};
    /// Callback state: proxy data called.
    bool proxy_data_called{false};
    /// Callback state: proxy disconnect called.
    bool proxy_disconnect_called{false};
    /// Callback state: reject called.
    bool reject_called{false};
    /// Callback state: reject reply called.
    bool reject_reply_called{false};
    /// Callback state: set accept policy called.
    bool set_accept_policy_called{false};

    // Store received data for verification
    /// Callback state: last initialize.
    InitializeMessage last_initialize{};
    /// Callback state: last network info.
    NetworkInfo last_network_info{};
    /// Callback state: last ping.
    PingMessage last_ping{};
    /// Callback state: last error.
    NetworkErrorMessage last_error{};
    /// Callback state: last proxy config.
    ProxyConfig last_proxy_config{};
    /// Callback state: last proxy header.
    ProxyDataHeader last_proxy_header{};
    /// Callback state: last proxy data.
    std::vector<uint8_t> last_proxy_data;

    /// Clears every flag and recorded argument.
    void reset() {
        initialize_called = false;
        connected_called = false;
        sync_network_called = false;
        scan_reply_called = false;
        scan_reply_end_called = false;
        disconnect_called = false;
        ping_called = false;
        network_error_called = false;
        proxy_config_called = false;
        proxy_connect_called = false;
        proxy_connect_reply_called = false;
        proxy_data_called = false;
        proxy_disconnect_called = false;
        reject_called = false;
        reject_reply_called = false;
        set_accept_policy_called = false;
        last_proxy_data.clear();
    }
} g_callback_state;  ///< Shared instance written by the callbacks.

// ============================================================================
// Callback Functions
// ============================================================================

/**
 * @brief Test helper: on initialize.
 *
 * @param msg Initialize message, stored in the callback state
 */
static void on_initialize(
    const LdnHeader& /*header*/, const InitializeMessage& msg) {
    g_callback_state.initialize_called = true;
    g_callback_state.last_initialize = msg;
}

/**
 * @brief Test helper: on connected.
 *
 * @param info NetworkInfo of the Connected packet, stored in the callback state
 */
static void on_connected(const LdnHeader& /*header*/, const NetworkInfo& info) {
    g_callback_state.connected_called = true;
    g_callback_state.last_network_info = info;
}

/**
 * @brief Test helper: on sync network.
 *
 * @param info NetworkInfo of the SyncNetwork packet, stored in the callback
 * state
 */
static void on_sync_network(
    const LdnHeader& /*header*/, const NetworkInfo& info) {
    g_callback_state.sync_network_called = true;
    g_callback_state.last_network_info = info;
}

/**
 * @brief Test helper: on scan reply.
 *
 * @param info NetworkInfo of the ScanReply packet, stored in the callback state
 */
static void on_scan_reply(
    const LdnHeader& /*header*/, const NetworkInfo& info) {
    g_callback_state.scan_reply_called = true;
    g_callback_state.last_network_info = info;
}

/// Test helper: on scan reply end.
static void on_scan_reply_end(const LdnHeader& /*header*/) {
    g_callback_state.scan_reply_end_called = true;
}

/// Test helper: on disconnect.
static void on_disconnect(
    const LdnHeader& /*header*/, const DisconnectMessage& /*msg*/) {
    g_callback_state.disconnect_called = true;
}

/**
 * @brief Test helper: on ping.
 *
 * @param msg Ping message, stored in the callback state
 */
static void on_ping(const LdnHeader& /*header*/, const PingMessage& msg) {
    g_callback_state.ping_called = true;
    g_callback_state.last_ping = msg;
}

/**
 * @brief Test helper: on network error.
 *
 * @param msg NetworkError message, stored in the callback state
 */
static void on_network_error(
    const LdnHeader& /*header*/, const NetworkErrorMessage& msg) {
    g_callback_state.network_error_called = true;
    g_callback_state.last_error = msg;
}

/**
 * @brief Test helper: on proxy config.
 *
 * @param cfg ProxyConfig, stored in the callback state
 */
static void on_proxy_config(
    const LdnHeader& /*header*/, const ProxyConfig& cfg) {
    g_callback_state.proxy_config_called = true;
    g_callback_state.last_proxy_config = cfg;
}

/// Test helper: on proxy connect.
static void on_proxy_connect(
    const LdnHeader& /*header*/, const ProxyConnectRequest& /*req*/) {
    g_callback_state.proxy_connect_called = true;
}

/// Test helper: on proxy connect reply.
static void on_proxy_connect_reply(
    const LdnHeader& /*header*/, const ProxyConnectResponse& /*resp*/) {
    g_callback_state.proxy_connect_reply_called = true;
}

/**
 * @brief Test helper: records the proxy data callback and its payload.
 *
 * @param hdr proxy data header, stored in the callback state
 * @param data payload bytes, copied into the callback state
 * @param size payload size in bytes
 */
static void on_proxy_data(
    const LdnHeader& /*header*/,
    const ProxyDataHeader& hdr,
    const uint8_t* data,
    size_t size) {
    g_callback_state.proxy_data_called = true;
    g_callback_state.last_proxy_header = hdr;
    g_callback_state.last_proxy_data.assign(data, data + size);
}

/// Test helper: on proxy disconnect.
static void on_proxy_disconnect(
    const LdnHeader& /*header*/, const ProxyDisconnectMessage& /*msg*/) {
    g_callback_state.proxy_disconnect_called = true;
}

/// Test helper: on reject.
static void on_reject(
    const LdnHeader& /*header*/, const RejectRequest& /*req*/) {
    g_callback_state.reject_called = true;
}

/// Test helper: on reject reply.
static void on_reject_reply(const LdnHeader& /*header*/) {
    g_callback_state.reject_reply_called = true;
}

/// Test helper: on set accept policy.
static void on_set_accept_policy(
    const LdnHeader& /*header*/, const SetAcceptPolicyRequest& /*req*/) {
    g_callback_state.set_accept_policy_called = true;
}

// ============================================================================
// Tests - Basic Dispatcher Functionality
// ============================================================================

/** @brief Test case: dispatcher default construction. */
TEST(dispatcher_default_construction) {
    ldn::PacketDispatcher dispatcher;
    // Should construct without crashing
    ASSERT_TRUE(true);
}

/** @brief Test case: dispatcher register callback. */
TEST(dispatcher_register_callback) {
    ldn::PacketDispatcher dispatcher;
    dispatcher.set_initialize_handler(on_initialize);
    // Should not crash
    ASSERT_TRUE(true);
}

/** @brief Test case: dispatcher dispatch initialize. */
TEST(dispatcher_dispatch_initialize) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_initialize_handler(on_initialize);

    // Create a valid Initialize packet
    InitializeMessage msg{};
    msg.id.data[0] = 0x12;
    msg.id.data[1] = 0x34;
    msg.mac_address.data[0] = 0xAA;
    msg.mac_address.data[5] = 0xBB;

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::Initialize);
    header.data_size = sizeof(InitializeMessage);

    dispatcher.dispatch(
        header, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));

    ASSERT_TRUE(g_callback_state.initialize_called);
    ASSERT_EQ(g_callback_state.last_initialize.id.data[0], 0x12);
    ASSERT_EQ(g_callback_state.last_initialize.mac_address.data[0], 0xAA);
}

/** @brief Test case: dispatcher dispatch ping. */
TEST(dispatcher_dispatch_ping) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_ping_handler(on_ping);

    PingMessage msg{};
    msg.requester = 0;
    msg.id = 42;

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::Ping);
    header.data_size = sizeof(PingMessage);

    dispatcher.dispatch(
        header, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));

    ASSERT_TRUE(g_callback_state.ping_called);
    ASSERT_EQ(g_callback_state.last_ping.requester, 0);
    ASSERT_EQ(g_callback_state.last_ping.id, 42);
}

/** @brief Test case: dispatcher dispatch network error. */
TEST(dispatcher_dispatch_network_error) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_network_error_handler(on_network_error);

    NetworkErrorMessage msg{};
    msg.error_code = static_cast<uint32_t>(NetworkErrorCode::SessionFull);

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::NetworkError);
    header.data_size = sizeof(NetworkErrorMessage);

    dispatcher.dispatch(
        header, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));

    ASSERT_TRUE(g_callback_state.network_error_called);
    ASSERT_EQ(
        g_callback_state.last_error.error_code,
        static_cast<uint32_t>(NetworkErrorCode::SessionFull));
}

/** @brief Test case: dispatcher dispatch connected. */
TEST(dispatcher_dispatch_connected) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_connected_handler(on_connected);

    NetworkInfo info{};
    info.network_id.intent_id.local_communication_id = 0x0100000000001234ULL;
    info.ldn.node_count = 2;

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::Connected);
    header.data_size = sizeof(NetworkInfo);

    dispatcher.dispatch(
        header, reinterpret_cast<const uint8_t*>(&info), sizeof(info));

    ASSERT_TRUE(g_callback_state.connected_called);
    ASSERT_EQ(
        g_callback_state.last_network_info.network_id.intent_id
            .local_communication_id,
        0x0100000000001234ULL);
    ASSERT_EQ(g_callback_state.last_network_info.ldn.node_count, 2);
}

/** @brief Test case: dispatcher dispatch sync network. */
TEST(dispatcher_dispatch_sync_network) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_sync_network_handler(on_sync_network);

    NetworkInfo info{};
    info.ldn.node_count = 4;

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::SyncNetwork);
    header.data_size = sizeof(NetworkInfo);

    dispatcher.dispatch(
        header, reinterpret_cast<const uint8_t*>(&info), sizeof(info));

    ASSERT_TRUE(g_callback_state.sync_network_called);
    ASSERT_EQ(g_callback_state.last_network_info.ldn.node_count, 4);
}

/** @brief Test case: dispatcher dispatch scan reply. */
TEST(dispatcher_dispatch_scan_reply) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_scan_reply_handler(on_scan_reply);

    NetworkInfo info{};
    info.ldn.node_count_max = 8;

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::ScanReply);
    header.data_size = sizeof(NetworkInfo);

    dispatcher.dispatch(
        header, reinterpret_cast<const uint8_t*>(&info), sizeof(info));

    ASSERT_TRUE(g_callback_state.scan_reply_called);
    ASSERT_EQ(g_callback_state.last_network_info.ldn.node_count_max, 8);
}

/** @brief Test case: dispatcher dispatch scan reply end. */
TEST(dispatcher_dispatch_scan_reply_end) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_scan_reply_end_handler(on_scan_reply_end);

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::ScanReplyEnd);
    header.data_size = 0;

    dispatcher.dispatch(header, nullptr, 0);

    ASSERT_TRUE(g_callback_state.scan_reply_end_called);
}

/** @brief Test case: dispatcher dispatch disconnect. */
TEST(dispatcher_dispatch_disconnect) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_disconnect_handler(on_disconnect);

    DisconnectMessage msg{};
    msg.disconnect_ip = 0xC0A80101;

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::Disconnect);
    header.data_size = sizeof(DisconnectMessage);

    dispatcher.dispatch(
        header, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));

    ASSERT_TRUE(g_callback_state.disconnect_called);
}

// ============================================================================
// Tests - Proxy Packets
// ============================================================================

/** @brief Test case: dispatcher dispatch proxy config. */
TEST(dispatcher_dispatch_proxy_config) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_proxy_config_handler(on_proxy_config);

    ProxyConfig cfg{};
    cfg.proxy_ip = 0x0A720001;
    cfg.proxy_subnet_mask = 0xFFFF0000;

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::ProxyConfig);
    header.data_size = sizeof(ProxyConfig);

    dispatcher.dispatch(
        header, reinterpret_cast<const uint8_t*>(&cfg), sizeof(cfg));

    ASSERT_TRUE(g_callback_state.proxy_config_called);
    ASSERT_EQ(g_callback_state.last_proxy_config.proxy_ip, 0x0A720001U);
    ASSERT_EQ(
        g_callback_state.last_proxy_config.proxy_subnet_mask, 0xFFFF0000U);
}

/** @brief Test case: dispatcher dispatch proxy connect. */
TEST(dispatcher_dispatch_proxy_connect) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_proxy_connect_handler(on_proxy_connect);

    ProxyConnectRequest req{};
    req.info.source_ipv4 = 0xC0A80101;
    req.info.dest_ipv4 = 0xC0A80102;

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::ProxyConnect);
    header.data_size = sizeof(ProxyConnectRequest);

    dispatcher.dispatch(
        header, reinterpret_cast<const uint8_t*>(&req), sizeof(req));

    ASSERT_TRUE(g_callback_state.proxy_connect_called);
}

/** @brief Test case: dispatcher dispatch proxy connect reply. */
TEST(dispatcher_dispatch_proxy_connect_reply) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_proxy_connect_reply_handler(on_proxy_connect_reply);

    ProxyConnectResponse resp{};
    resp.info.source_ipv4 = 0xC0A80101;

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::ProxyConnectReply);
    header.data_size = sizeof(ProxyConnectResponse);

    dispatcher.dispatch(
        header, reinterpret_cast<const uint8_t*>(&resp), sizeof(resp));

    ASSERT_TRUE(g_callback_state.proxy_connect_reply_called);
}

/** @brief Test case: dispatcher dispatch proxy data. */
TEST(dispatcher_dispatch_proxy_data) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_proxy_data_handler(on_proxy_data);

    // Build packet: ProxyDataHeader + data
    uint8_t game_data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};

    ProxyDataHeader hdr{};
    hdr.info.source_ipv4 = 0xC0A80101;
    hdr.info.source_port = 12345;
    hdr.info.dest_ipv4 = 0xC0A80102;
    hdr.info.dest_port = 54321;
    hdr.info.protocol = ProtocolType::Udp;
    hdr.data_length = sizeof(game_data);

    // Combine header + data
    std::vector<uint8_t> packet;
    packet.resize(sizeof(ProxyDataHeader) + sizeof(game_data));
    memcpy(packet.data(), &hdr, sizeof(hdr));
    memcpy(packet.data() + sizeof(hdr), game_data, sizeof(game_data));

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::ProxyData);
    header.data_size = static_cast<int32_t>(packet.size());

    dispatcher.dispatch(header, packet.data(), packet.size());

    ASSERT_TRUE(g_callback_state.proxy_data_called);
    ASSERT_EQ(g_callback_state.last_proxy_header.info.source_ipv4, 0xC0A80101U);
    ASSERT_EQ(g_callback_state.last_proxy_header.info.dest_port, 54321);
    ASSERT_EQ(
        g_callback_state.last_proxy_header.data_length, sizeof(game_data));
    ASSERT_EQ(g_callback_state.last_proxy_data.size(), sizeof(game_data));
    ASSERT_EQ(g_callback_state.last_proxy_data[0], 0xDE);
    ASSERT_EQ(g_callback_state.last_proxy_data[3], 0xEF);
}

/** @brief Test case: dispatcher dispatch proxy disconnect. */
TEST(dispatcher_dispatch_proxy_disconnect) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_proxy_disconnect_handler(on_proxy_disconnect);

    ProxyDisconnectMessage msg{};
    msg.info.source_ipv4 = 0xC0A80101;
    msg.disconnect_reason = 1;

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::ProxyDisconnect);
    header.data_size = sizeof(ProxyDisconnectMessage);

    dispatcher.dispatch(
        header, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));

    ASSERT_TRUE(g_callback_state.proxy_disconnect_called);
}

// ============================================================================
// Tests - Control Packets
// ============================================================================

/** @brief Test case: dispatcher dispatch reject. */
TEST(dispatcher_dispatch_reject) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_reject_handler(on_reject);

    RejectRequest req{};
    req.node_id = 3;
    req.disconnect_reason = static_cast<uint32_t>(DisconnectReason::Rejected);

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::Reject);
    header.data_size = sizeof(RejectRequest);

    dispatcher.dispatch(
        header, reinterpret_cast<const uint8_t*>(&req), sizeof(req));

    ASSERT_TRUE(g_callback_state.reject_called);
}

/** @brief Test case: dispatcher dispatch reject reply. */
TEST(dispatcher_dispatch_reject_reply) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_reject_reply_handler(on_reject_reply);

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::RejectReply);
    header.data_size = 0;

    dispatcher.dispatch(header, nullptr, 0);

    ASSERT_TRUE(g_callback_state.reject_reply_called);
}

/** @brief Test case: dispatcher dispatch set accept policy. */
TEST(dispatcher_dispatch_set_accept_policy) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_accept_policy_handler(on_set_accept_policy);

    SetAcceptPolicyRequest req{};
    req.accept_policy = static_cast<uint32_t>(AcceptPolicy::RejectAll);

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::SetAcceptPolicy);
    header.data_size = sizeof(SetAcceptPolicyRequest);

    dispatcher.dispatch(
        header, reinterpret_cast<const uint8_t*>(&req), sizeof(req));

    ASSERT_TRUE(g_callback_state.set_accept_policy_called);
}

// ============================================================================
// Tests - Error Handling
// ============================================================================

/** @brief Test case: dispatcher no handler registered. */
TEST(dispatcher_no_handler_registered) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    // Don't register any handler

    PingMessage msg{};
    msg.id = 1;

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::Ping);
    header.data_size = sizeof(PingMessage);

    // Should not crash, just silently ignore
    dispatcher.dispatch(
        header, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));

    ASSERT_FALSE(g_callback_state.ping_called);
}

/** @brief Test case: dispatcher undersized packet. */
TEST(dispatcher_undersized_packet) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_ping_handler(on_ping);

    // Only 1 byte, but PingMessage needs 2
    uint8_t incomplete_data[1] = {0x00};

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::Ping);
    header.data_size = 1;  // Too small

    // Should not crash, should not call handler
    dispatcher.dispatch(header, incomplete_data, 1);

    ASSERT_FALSE(g_callback_state.ping_called);
}

/** @brief Test case: dispatcher unknown packet type. */
TEST(dispatcher_unknown_packet_type) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;

    uint8_t data[10] = {0};

    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = 99;  // Unknown type
    header.data_size = 10;

    // Should not crash, just ignore
    dispatcher.dispatch(header, data, 10);

    ASSERT_TRUE(true);  // If we get here, we didn't crash
}

// ============================================================================
// Tests - Multiple Handlers
// ============================================================================

/** @brief Test case: dispatcher multiple packet types. */
TEST(dispatcher_multiple_packet_types) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_ping_handler(on_ping);
    dispatcher.set_network_error_handler(on_network_error);
    dispatcher.set_connected_handler(on_connected);

    // Dispatch Ping
    {
        PingMessage msg{};
        msg.id = 1;
        LdnHeader header{};
        header.magic = PROTOCOL_MAGIC;
        header.version = PROTOCOL_VERSION;
        header.type = static_cast<uint8_t>(PacketId::Ping);
        header.data_size = sizeof(PingMessage);
        dispatcher.dispatch(
            header, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
    }

    // Dispatch NetworkError
    {
        NetworkErrorMessage msg{};
        msg.error_code = 100;
        LdnHeader header{};
        header.magic = PROTOCOL_MAGIC;
        header.version = PROTOCOL_VERSION;
        header.type = static_cast<uint8_t>(PacketId::NetworkError);
        header.data_size = sizeof(NetworkErrorMessage);
        dispatcher.dispatch(
            header, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
    }

    ASSERT_TRUE(g_callback_state.ping_called);
    ASSERT_TRUE(g_callback_state.network_error_called);
    ASSERT_FALSE(g_callback_state.connected_called);  // Not dispatched
}

/** @brief Test case: dispatcher clear handler. */
TEST(dispatcher_clear_handler) {
    g_callback_state.reset();

    ldn::PacketDispatcher dispatcher;
    dispatcher.set_ping_handler(on_ping);

    // First dispatch should work
    {
        PingMessage msg{};
        msg.id = 1;
        LdnHeader header{};
        header.magic = PROTOCOL_MAGIC;
        header.version = PROTOCOL_VERSION;
        header.type = static_cast<uint8_t>(PacketId::Ping);
        header.data_size = sizeof(PingMessage);
        dispatcher.dispatch(
            header, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
    }
    ASSERT_TRUE(g_callback_state.ping_called);

    // Clear handler
    g_callback_state.reset();
    dispatcher.set_ping_handler(nullptr);

    // Second dispatch should not call handler
    {
        PingMessage msg{};
        msg.id = 2;
        LdnHeader header{};
        header.magic = PROTOCOL_MAGIC;
        header.version = PROTOCOL_VERSION;
        header.type = static_cast<uint8_t>(PacketId::Ping);
        header.data_size = sizeof(PingMessage);
        dispatcher.dispatch(
            header, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
    }
    ASSERT_FALSE(g_callback_state.ping_called);
}

// ============================================================================
// Guard paths: no handler registered / undersized payload / bad ProxyData
// ============================================================================

namespace {

/// Test fixture value: typed hits.
int g_typed_hits = 0;

template <typename T>
/// Test helper: count typed.
void count_typed(const LdnHeader& /*unused*/, const T& /*unused*/) {
    ++g_typed_hits;
}

/**
 * @brief Test helper: make header.
 *
 * @param id packet id stored in the header
 * @param size payload size stored in the header
 * @return header with the protocol magic and version set
 */
LdnHeader make_header(PacketId id, size_t size) {
    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(id);
    header.data_size = static_cast<int32_t>(size);
    return header;
}

/**
 * @brief Checks the size guards of one typed packet handler.
 *
 * For packet `id` carrying payload type T: dispatching with no handler
 * registered is a no-op, an undersized payload never reaches the handler and
 * an exactly-sized payload reaches it exactly once.
 *
 * @param id Packet id under test.
 * @param setter Dispatcher member that installs the handler for `id`.
 * @return True when all three expectations hold.
 */
template <typename T>
bool check_typed_guards(
    PacketId id, void (ldn::PacketDispatcher::*setter)(ldn::PacketHandler<T>)) {
    std::vector<uint8_t> payload(sizeof(T), 0);
    ldn::PacketDispatcher dispatcher;

    g_typed_hits = 0;
    dispatcher.dispatch(
        make_header(id, payload.size()), payload.data(), payload.size());
    if (g_typed_hits != 0) {
        return false;
    }

    (dispatcher.*setter)(&count_typed<T>);

    dispatcher.dispatch(
        make_header(id, payload.size() - 1),
        payload.data(),
        payload.size() - 1);
    if (g_typed_hits != 0) {
        return false;
    }

    dispatcher.dispatch(make_header(id, 0), payload.data(), 0);
    if (g_typed_hits != 0) {
        return false;
    }

    dispatcher.dispatch(
        make_header(id, payload.size()), payload.data(), payload.size());
    return g_typed_hits == 1;
}

}  // namespace

/**
 * @brief Test case: typed packets ignore missing handler and undersized
 *   payload.
 */
TEST(typed_packets_ignore_missing_handler_and_undersized_payload) {
    ASSERT_TRUE(
        check_typed_guards<InitializeMessage>(
            PacketId::Initialize,
            &ldn::PacketDispatcher::set_initialize_handler));
    ASSERT_TRUE(
        check_typed_guards<NetworkInfo>(
            PacketId::Connected,
            &ldn::PacketDispatcher::set_connected_handler));
    ASSERT_TRUE(
        check_typed_guards<NetworkInfo>(
            PacketId::SyncNetwork,
            &ldn::PacketDispatcher::set_sync_network_handler));
    ASSERT_TRUE(
        check_typed_guards<NetworkInfo>(
            PacketId::ScanReply,
            &ldn::PacketDispatcher::set_scan_reply_handler));
    ASSERT_TRUE(
        check_typed_guards<DisconnectMessage>(
            PacketId::Disconnect,
            &ldn::PacketDispatcher::set_disconnect_handler));
    ASSERT_TRUE(
        check_typed_guards<PingMessage>(
            PacketId::Ping, &ldn::PacketDispatcher::set_ping_handler));
    ASSERT_TRUE(
        check_typed_guards<NetworkErrorMessage>(
            PacketId::NetworkError,
            &ldn::PacketDispatcher::set_network_error_handler));
    ASSERT_TRUE(
        check_typed_guards<ProxyConfig>(
            PacketId::ProxyConfig,
            &ldn::PacketDispatcher::set_proxy_config_handler));
    ASSERT_TRUE(
        check_typed_guards<ProxyConnectRequest>(
            PacketId::ProxyConnect,
            &ldn::PacketDispatcher::set_proxy_connect_handler));
    ASSERT_TRUE(
        check_typed_guards<ProxyConnectResponse>(
            PacketId::ProxyConnectReply,
            &ldn::PacketDispatcher::set_proxy_connect_reply_handler));
    ASSERT_TRUE(
        check_typed_guards<ProxyDisconnectMessage>(
            PacketId::ProxyDisconnect,
            &ldn::PacketDispatcher::set_proxy_disconnect_handler));
    ASSERT_TRUE(
        check_typed_guards<RejectRequest>(
            PacketId::Reject, &ldn::PacketDispatcher::set_reject_handler));
    ASSERT_TRUE(
        check_typed_guards<SetAcceptPolicyRequest>(
            PacketId::SetAcceptPolicy,
            &ldn::PacketDispatcher::set_accept_policy_handler));
}

/**
 * @brief Test case: empty packets ignore missing handler and reach registered
 *   one.
 */
TEST(empty_packets_ignore_missing_handler_and_reach_registered_one) {
    static int hits;
    hits = 0;
    ldn::PacketDispatcher dispatcher;

    dispatcher.dispatch(make_header(PacketId::ScanReplyEnd, 0), nullptr, 0);
    dispatcher.dispatch(make_header(PacketId::RejectReply, 0), nullptr, 0);
    ASSERT_EQ(hits, 0);

    dispatcher.set_scan_reply_end_handler([](const LdnHeader&) { ++hits; });
    dispatcher.set_reject_reply_handler([](const LdnHeader&) { hits += 10; });
    dispatcher.dispatch(make_header(PacketId::ScanReplyEnd, 0), nullptr, 0);
    ASSERT_EQ(hits, 1);
    dispatcher.dispatch(make_header(PacketId::RejectReply, 0), nullptr, 0);
    ASSERT_EQ(hits, 11);
}

/**
 * @brief Test case: proxy data rejects truncated header and overlong declared
 *   length.
 */
TEST(proxy_data_rejects_truncated_header_and_overlong_declared_length) {
    g_callback_state.reset();
    ldn::PacketDispatcher dispatcher;

    // No handler: nothing happens.
    std::vector<uint8_t> packet(sizeof(ProxyDataHeader) + 4, 0);
    dispatcher.dispatch(
        make_header(PacketId::ProxyData, packet.size()),
        packet.data(),
        packet.size());
    ASSERT_FALSE(g_callback_state.proxy_data_called);

    dispatcher.set_proxy_data_handler(on_proxy_data);

    // Payload shorter than the ProxyDataHeader itself.
    dispatcher.dispatch(
        make_header(PacketId::ProxyData, sizeof(ProxyDataHeader) - 1),
        packet.data(),
        sizeof(ProxyDataHeader) - 1);
    ASSERT_FALSE(g_callback_state.proxy_data_called);

    // Declared data_length larger than the bytes actually present.
    ProxyDataHeader hdr{};
    hdr.data_length = 5;
    memcpy(packet.data(), &hdr, sizeof(hdr));
    dispatcher.dispatch(
        make_header(PacketId::ProxyData, packet.size()),
        packet.data(),
        packet.size());
    ASSERT_FALSE(g_callback_state.proxy_data_called);

    // Declared data_length exactly equal to the bytes present is accepted.
    hdr.data_length = 4;
    memcpy(packet.data(), &hdr, sizeof(hdr));
    dispatcher.dispatch(
        make_header(PacketId::ProxyData, packet.size()),
        packet.data(),
        packet.size());
    ASSERT_TRUE(g_callback_state.proxy_data_called);
    ASSERT_EQ(g_callback_state.last_proxy_data.size(), 4U);
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
    printf("  LDN Packet Dispatcher Tests\n");
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
