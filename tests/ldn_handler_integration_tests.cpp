/**
 * @file ldn_handler_integration_tests.cpp
 * @brief Integration tests for LDN packet handlers
 *
 * Tests the complete flow of packet handling by integrating:
 * - PacketDispatcher (routing)
 * - LdnSessionHandler (session state)
 * - LdnProxyHandler (P2P connections)
 *
 * These tests validate end-to-end scenarios like:
 * - Client joining a session
 * - Host creating and managing a session
 * - P2P data exchange through proxy
 * - Error handling and recovery
 */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

// Include protocol types
#include "protocol/types.hpp"

// Include all handlers
#include "ldn/ldn_packet_dispatcher.hpp"
#include "ldn/ldn_session_handler.hpp"
#include "ldn/ldn_proxy_handler.hpp"

// ============================================================================
// Test Framework (minimal)
// ============================================================================

/// Number of tests that ran to completion (incremented by TEST()).
static int g_tests_passed = 0;
/// Number of failed assertions (each one aborts its test and is counted here).
static int g_tests_failed = 0;

/**
 * @brief Define and auto-run a test case.
 *
 * Declares test_<name>(), and a static object whose constructor runs it during
 * static initialisation, printing PASS and counting it; a failed assertion
 * prints FAIL first. main() therefore only prints the totals.
 *
 * @param name Identifier of the test (becomes test_<name>).
 */
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

/**
 * @brief Fail the current test and return from it when cond is false.
 * @param cond Expression expected to be true.
 */
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

/**
 * @brief Fail the current test and return from it when cond is true.
 * @param cond Expression expected to be false.
 */
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

/**
 * @brief Fail the current test and return from it when a != b.
 * @param a Actual value.
 * @param b Expected value.
 */
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

/**
 * @brief Create a test NetworkInfo with specified parameters
 *
 * @param node_count number of nodes in the session
 * @param max_nodes maximum number of nodes of the session
 * @param game_id local communication id of the game
 * @return the filled NetworkInfo
 */
static NetworkInfo make_test_network_info(
    uint8_t node_count,
    uint8_t max_nodes,
    uint64_t game_id = 0x0100000000001234ULL) {
    NetworkInfo info{};
    info.network_id.intent_id.local_communication_id =
        static_cast<int64_t>(game_id);
    info.network_id.intent_id.scene_id = 1;
    info.ldn.node_count_max = max_nodes;
    info.ldn.node_count = node_count;

    for (uint8_t i = 0; i < node_count && i < MAX_NODES; i++) {
        info.ldn.nodes[i].node_id = i;
        info.ldn.nodes[i].is_connected = 1;
        info.ldn.nodes[i].ipv4_address = 0x0A720000 + i + 1;
        snprintf(
            info.ldn.nodes[i].user_name,
            sizeof(info.ldn.nodes[i].user_name),
            "Player%d",
            i + 1);
    }

    return info;
}

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

// ============================================================================
// Tests - Client Flow (Initialize -> Scan -> Connect -> Data)
// ============================================================================

/**
 * @brief An Initialize response moves a fresh session from None to Initialized
 *        and leaves the proxy unconfigured.
 */
TEST(integration_client_initialize_flow) {
    ldn::LdnSessionHandler session;
    ldn::LdnProxyHandler proxy;

    // Initial state
    ASSERT_EQ(session.get_state(), ldn::LdnSessionState::None);
    ASSERT_FALSE(proxy.is_configured());

    // Server sends Initialize response
    InitializeMessage init{};
    init.id.data[0] = 0x12;
    init.mac_address.data[0] = 0xAA;
    LdnHeader header = make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(header, init);

    // Should be initialized
    ASSERT_EQ(session.get_state(), ldn::LdnSessionState::Initialized);
}

/**
 * @brief Scan replies and ScanReplyEnd do not change the Initialized state; a
 *        Connected packet then puts the client in Station mode (not host,
 *        node count taken from the network info).
 */
TEST(integration_client_scan_and_connect) {
    ldn::LdnSessionHandler session;

    // Initialize first
    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);
    ASSERT_EQ(session.get_state(), ldn::LdnSessionState::Initialized);

    // Scan results arrive
    NetworkInfo scan1 = make_test_network_info(2, 8, 0x0100000000001111ULL);
    NetworkInfo scan2 = make_test_network_info(1, 4, 0x0100000000002222ULL);
    LdnHeader scan_header =
        make_test_header(PacketId::ScanReply, sizeof(NetworkInfo));
    session.handle_scan_reply(scan_header, scan1);
    session.handle_scan_reply(scan_header, scan2);

    LdnHeader scan_end_header = make_test_header(PacketId::ScanReplyEnd, 0);
    session.handle_scan_reply_end(scan_end_header);

    // Still in Initialized (scanning doesn't change state)
    ASSERT_EQ(session.get_state(), ldn::LdnSessionState::Initialized);

    // Connect to first network
    NetworkInfo connected = make_test_network_info(3, 8, 0x0100000000001111ULL);
    session.set_local_node_id(2);  // We're assigned node 2
    LdnHeader connected_header =
        make_test_header(PacketId::Connected, sizeof(connected));
    session.handle_connected(connected_header, connected);

    // Now in Station mode
    ASSERT_EQ(session.get_state(), ldn::LdnSessionState::Station);
    ASSERT_TRUE(session.is_in_session());
    ASSERT_FALSE(session.is_host());
    ASSERT_EQ(session.get_node_count(), 3);
}

/**
 * @brief A ProxyConfig packet configures the proxy handler with the virtual
 *        IP assigned by the server.
 */
TEST(integration_client_receives_proxy_config) {
    ldn::LdnSessionHandler session;
    ldn::LdnProxyHandler proxy;

    // Initialize and connect
    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);

    NetworkInfo connected = make_test_network_info(2, 8);
    session.set_local_node_id(1);
    LdnHeader connected_header =
        make_test_header(PacketId::Connected, sizeof(connected));
    session.handle_connected(connected_header, connected);

    ASSERT_FALSE(proxy.is_configured());

    // Server sends proxy config
    ProxyConfig config{};
    config.proxy_ip = 0x0A720001;
    config.proxy_subnet_mask = 0xFFFF0000;
    LdnHeader config_header =
        make_test_header(PacketId::ProxyConfig, sizeof(config));
    proxy.handle_proxy_config(config_header, config);

    ASSERT_TRUE(proxy.is_configured());
    ASSERT_EQ(proxy.get_proxy_ip(), 0x0A720001);
}

/**
 * @brief A peer ProxyConnect creates one proxy connection and ProxyData on it
 *        is accepted without dropping the connection.
 */
TEST(integration_client_p2p_data_exchange) {
    ldn::LdnSessionHandler session;
    ldn::LdnProxyHandler proxy;

    // Full setup
    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);

    NetworkInfo connected = make_test_network_info(2, 8);
    session.set_local_node_id(1);
    LdnHeader connected_header =
        make_test_header(PacketId::Connected, sizeof(connected));
    session.handle_connected(connected_header, connected);

    ProxyConfig config{};
    config.proxy_ip = 0x0A720002;
    config.proxy_subnet_mask = 0xFFFF0000;
    LdnHeader config_header =
        make_test_header(PacketId::ProxyConfig, sizeof(config));
    proxy.handle_proxy_config(config_header, config);

    // Peer connects to us
    ProxyConnectRequest connect_req{};
    connect_req.info.source_ipv4 = 0x0A720001;
    connect_req.info.source_port = 1234;
    connect_req.info.dest_ipv4 = 0x0A720002;
    connect_req.info.dest_port = 5678;
    connect_req.info.protocol = ProtocolType::Udp;
    LdnHeader connect_header =
        make_test_header(PacketId::ProxyConnect, sizeof(connect_req));
    proxy.handle_proxy_connect(connect_header, connect_req);

    ASSERT_EQ(proxy.get_connection_count(), 1);

    // Data arrives through proxy
    ProxyDataHeader data_header{};
    data_header.info = connect_req.info;
    data_header.data_length = 4;

    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    LdnHeader pkt_header =
        make_test_header(PacketId::ProxyData, sizeof(ProxyDataHeader) + 4);
    proxy.handle_proxy_data(pkt_header, data_header, payload, 4);

    // Connection still active
    ASSERT_EQ(proxy.get_connection_count(), 1);
}

// ============================================================================
// Tests - Host Flow (Initialize -> CreateAP -> Accept -> Data)
// ============================================================================

/**
 * @brief A SyncNetwork for node 0 turns an initialized session into an
 *        AccessPoint (host, in session, one node).
 */
TEST(integration_host_create_access_point) {
    ldn::LdnSessionHandler session;

    // Initialize
    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);

    // Set as host (node 0)
    session.set_local_node_id(0);

    // Server confirms AP creation via SyncNetwork
    NetworkInfo ap_info = make_test_network_info(1, 8);
    LdnHeader sync_header =
        make_test_header(PacketId::SyncNetwork, sizeof(ap_info));
    session.handle_sync_network(sync_header, ap_info);

    ASSERT_EQ(session.get_state(), ldn::LdnSessionState::AccessPoint);
    ASSERT_TRUE(session.is_host());
    ASSERT_TRUE(session.is_in_session());
    ASSERT_EQ(session.get_node_count(), 1);
}

/**
 * @brief A second SyncNetwork with two nodes raises the node count while the
 *        session stays host.
 */
TEST(integration_host_player_joins) {
    ldn::LdnSessionHandler session;

    // Initialize and create AP
    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);
    session.set_local_node_id(0);

    NetworkInfo ap_info = make_test_network_info(1, 8);
    LdnHeader sync_header =
        make_test_header(PacketId::SyncNetwork, sizeof(ap_info));
    session.handle_sync_network(sync_header, ap_info);

    ASSERT_EQ(session.get_node_count(), 1);

    // Player joins - server sends updated SyncNetwork
    NetworkInfo updated = make_test_network_info(2, 8);
    session.handle_sync_network(sync_header, updated);

    ASSERT_EQ(session.get_node_count(), 2);
    ASSERT_TRUE(session.is_host());  // Still host
}

/**
 * @brief SetAcceptPolicy changes the host policy from the default AcceptAll to
 *        RejectAll.
 */
TEST(integration_host_set_accept_policy) {
    ldn::LdnSessionHandler session;

    // Setup host
    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);
    session.set_local_node_id(0);

    NetworkInfo ap_info = make_test_network_info(1, 8);
    LdnHeader sync_header =
        make_test_header(PacketId::SyncNetwork, sizeof(ap_info));
    session.handle_sync_network(sync_header, ap_info);

    ASSERT_EQ(session.get_accept_policy(), AcceptPolicy::AcceptAll);

    // Change accept policy
    SetAcceptPolicyRequest policy_req{};
    policy_req.accept_policy = static_cast<uint8_t>(AcceptPolicy::RejectAll);
    LdnHeader policy_header =
        make_test_header(PacketId::SetAcceptPolicy, sizeof(policy_req));
    session.handle_set_accept_policy(policy_header, policy_req);

    ASSERT_EQ(session.get_accept_policy(), AcceptPolicy::RejectAll);
}

/**
 * @brief Rejecting a joined node keeps the host in session; the following
 *        SyncNetwork reduces the node count.
 */
TEST(integration_host_reject_player) {
    ldn::LdnSessionHandler session;

    // Setup host with 2 players
    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);
    session.set_local_node_id(0);

    NetworkInfo ap_info = make_test_network_info(2, 8);
    LdnHeader sync_header =
        make_test_header(PacketId::SyncNetwork, sizeof(ap_info));
    session.handle_sync_network(sync_header, ap_info);

    ASSERT_EQ(session.get_node_count(), 2);

    // Reject player 1
    RejectRequest reject{};
    reject.node_id = 1;
    reject.disconnect_reason =
        static_cast<uint32_t>(DisconnectReason::Rejected);
    LdnHeader reject_header =
        make_test_header(PacketId::Reject, sizeof(reject));
    session.handle_reject(reject_header, reject);

    // Host should still be in session (wasn't rejected)
    ASSERT_TRUE(session.is_in_session());

    // Server sends updated network info
    NetworkInfo after_reject = make_test_network_info(1, 8);
    session.handle_sync_network(sync_header, after_reject);

    ASSERT_EQ(session.get_node_count(), 1);
}

// ============================================================================
// Tests - Error Handling
// ============================================================================

/**
 * @brief A NetworkError packet does not change the session state by itself.
 */
TEST(integration_network_error_handling) {
    ldn::LdnSessionHandler session;

    // Initialize
    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);

    // Try to join but get error
    NetworkErrorMessage error{};
    error.error_code = static_cast<uint32_t>(NetworkErrorCode::SessionFull);
    LdnHeader error_header =
        make_test_header(PacketId::NetworkError, sizeof(error));
    session.handle_network_error(error_header, error);

    // Still initialized (error doesn't change state automatically)
    ASSERT_EQ(session.get_state(), ldn::LdnSessionState::Initialized);
}

/**
 * @brief A Reject targeting the local node id makes a connected client leave
 *        the session and fall back to Initialized.
 */
TEST(integration_client_gets_rejected) {
    ldn::LdnSessionHandler session;

    // Client connects
    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);
    session.set_local_node_id(2);

    NetworkInfo connected = make_test_network_info(3, 8);
    LdnHeader connected_header =
        make_test_header(PacketId::Connected, sizeof(connected));
    session.handle_connected(connected_header, connected);

    ASSERT_TRUE(session.is_in_session());

    // Get rejected
    RejectRequest reject{};
    reject.node_id = 2;  // Our node ID
    reject.disconnect_reason =
        static_cast<uint32_t>(DisconnectReason::SystemRequest);
    LdnHeader reject_header =
        make_test_header(PacketId::Reject, sizeof(reject));
    session.handle_reject(reject_header, reject);

    // Should leave session
    ASSERT_FALSE(session.is_in_session());
    ASSERT_EQ(session.get_state(), ldn::LdnSessionState::Initialized);
}

/**
 * @brief A Disconnect about another player keeps the client in session; the
 *        next SyncNetwork updates the node count.
 */
TEST(integration_disconnect_notification) {
    ldn::LdnSessionHandler session;

    // Client connects
    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);
    session.set_local_node_id(1);

    NetworkInfo connected = make_test_network_info(3, 8);
    LdnHeader connected_header =
        make_test_header(PacketId::Connected, sizeof(connected));
    session.handle_connected(connected_header, connected);

    // Another player disconnects
    DisconnectMessage disconnect{};
    disconnect.disconnect_ip = 0x0A720003;  // Player 3's IP
    LdnHeader disconnect_header =
        make_test_header(PacketId::Disconnect, sizeof(disconnect));
    session.handle_disconnect(disconnect_header, disconnect);

    // We're still in session
    ASSERT_TRUE(session.is_in_session());

    // Server sends updated network
    NetworkInfo after_disconnect = make_test_network_info(2, 8);
    LdnHeader sync_header =
        make_test_header(PacketId::SyncNetwork, sizeof(after_disconnect));
    session.handle_sync_network(sync_header, after_disconnect);

    ASSERT_EQ(session.get_node_count(), 2);
}

// ============================================================================
// Tests - Proxy Connection Lifecycle
// ============================================================================

/**
 * @brief ProxyConnect adds a proxy connection and the matching
 *        ProxyDisconnect removes it again.
 */
TEST(integration_proxy_full_lifecycle) {
    ldn::LdnSessionHandler session;
    ldn::LdnProxyHandler proxy;

    // Setup connected client with proxy
    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);
    session.set_local_node_id(1);

    NetworkInfo connected = make_test_network_info(2, 8);
    LdnHeader connected_header =
        make_test_header(PacketId::Connected, sizeof(connected));
    session.handle_connected(connected_header, connected);

    ProxyConfig config{};
    config.proxy_ip = 0x0A720002;
    config.proxy_subnet_mask = 0xFFFF0000;
    LdnHeader config_header =
        make_test_header(PacketId::ProxyConfig, sizeof(config));
    proxy.handle_proxy_config(config_header, config);

    // Connect
    ProxyConnectRequest connect{};
    connect.info.source_ipv4 = 0x0A720001;
    connect.info.source_port = 1234;
    connect.info.dest_ipv4 = 0x0A720002;
    connect.info.dest_port = 5678;
    connect.info.protocol = ProtocolType::Udp;
    LdnHeader connect_header =
        make_test_header(PacketId::ProxyConnect, sizeof(connect));
    proxy.handle_proxy_connect(connect_header, connect);

    ASSERT_EQ(proxy.get_connection_count(), 1);

    // Disconnect
    ProxyDisconnectMessage disconnect{};
    disconnect.info = connect.info;
    disconnect.disconnect_reason = static_cast<int32_t>(DisconnectReason::User);
    LdnHeader disconnect_header =
        make_test_header(PacketId::ProxyDisconnect, sizeof(disconnect));
    proxy.handle_proxy_disconnect(disconnect_header, disconnect);

    ASSERT_EQ(proxy.get_connection_count(), 0);
}

/**
 * @brief Three clients connect to the host through the proxy, then one
 *        disconnects: the connection count goes 3 -> 2.
 */
TEST(integration_multiple_proxy_connections) {
    ldn::LdnSessionHandler session;
    ldn::LdnProxyHandler proxy;

    // Setup
    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);
    session.set_local_node_id(0);

    NetworkInfo ap_info = make_test_network_info(4, 8);
    LdnHeader sync_header =
        make_test_header(PacketId::SyncNetwork, sizeof(ap_info));
    session.handle_sync_network(sync_header, ap_info);

    ProxyConfig config{};
    config.proxy_ip = 0x0A720001;
    config.proxy_subnet_mask = 0xFFFF0000;
    LdnHeader config_header =
        make_test_header(PacketId::ProxyConfig, sizeof(config));
    proxy.handle_proxy_config(config_header, config);

    // 3 players connect to host
    for (int i = 1; i <= 3; i++) {
        ProxyConnectRequest connect{};
        connect.info.source_ipv4 = static_cast<uint32_t>(0x0A720000 + i + 1);
        connect.info.source_port = static_cast<uint16_t>(1000 + i);
        connect.info.dest_ipv4 = 0x0A720001;
        connect.info.dest_port = 5678;
        connect.info.protocol = ProtocolType::Udp;
        LdnHeader connect_header =
            make_test_header(PacketId::ProxyConnect, sizeof(connect));
        proxy.handle_proxy_connect(connect_header, connect);
    }

    ASSERT_EQ(proxy.get_connection_count(), 3);

    // One player leaves
    ProxyDisconnectMessage disconnect{};
    disconnect.info.source_ipv4 = 0x0A720003;
    disconnect.info.source_port = 1002;
    disconnect.info.dest_ipv4 = 0x0A720001;
    disconnect.info.dest_port = 5678;
    disconnect.info.protocol = ProtocolType::Udp;
    disconnect.disconnect_reason = 0;
    LdnHeader disconnect_header =
        make_test_header(PacketId::ProxyDisconnect, sizeof(disconnect));
    proxy.handle_proxy_disconnect(disconnect_header, disconnect);

    ASSERT_EQ(proxy.get_connection_count(), 2);
}

// ============================================================================
// Tests - Reset and Reconnect
// ============================================================================

/**
 * @brief reset() on the session and proxy handlers drops the session state,
 *        the proxy configuration and every proxy connection.
 */
TEST(integration_session_reset_clears_state) {
    ldn::LdnSessionHandler session;
    ldn::LdnProxyHandler proxy;

    // Full setup
    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);
    session.set_local_node_id(1);

    NetworkInfo connected = make_test_network_info(3, 8);
    LdnHeader connected_header =
        make_test_header(PacketId::Connected, sizeof(connected));
    session.handle_connected(connected_header, connected);

    ProxyConfig config{};
    config.proxy_ip = 0x0A720002;
    config.proxy_subnet_mask = 0xFFFF0000;
    LdnHeader config_header =
        make_test_header(PacketId::ProxyConfig, sizeof(config));
    proxy.handle_proxy_config(config_header, config);

    ProxyConnectRequest connect{};
    connect.info.source_ipv4 = 0x0A720001;
    connect.info.source_port = 1234;
    connect.info.dest_ipv4 = 0x0A720002;
    connect.info.dest_port = 5678;
    connect.info.protocol = ProtocolType::Udp;
    LdnHeader connect_header =
        make_test_header(PacketId::ProxyConnect, sizeof(connect));
    proxy.handle_proxy_connect(connect_header, connect);

    // Verify state
    ASSERT_TRUE(session.is_in_session());
    ASSERT_TRUE(proxy.is_configured());
    ASSERT_EQ(proxy.get_connection_count(), 1);

    // Reset everything
    session.reset();
    proxy.reset();

    // All cleared
    ASSERT_EQ(session.get_state(), ldn::LdnSessionState::None);
    ASSERT_FALSE(session.is_in_session());
    ASSERT_FALSE(proxy.is_configured());
    ASSERT_EQ(proxy.get_connection_count(), 0);
}

/**
 * @brief After leave_session() a session can be re-established as host with a
 *        new SyncNetwork.
 */
TEST(integration_reconnect_after_disconnect) {
    ldn::LdnSessionHandler session;

    // First connection
    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);
    session.set_local_node_id(1);

    NetworkInfo connected = make_test_network_info(2, 8);
    LdnHeader connected_header =
        make_test_header(PacketId::Connected, sizeof(connected));
    session.handle_connected(connected_header, connected);

    ASSERT_TRUE(session.is_in_session());

    // Leave session
    session.leave_session();
    ASSERT_FALSE(session.is_in_session());
    ASSERT_EQ(session.get_state(), ldn::LdnSessionState::Initialized);

    // Reconnect to different session
    session.set_local_node_id(0);  // Now we're host
    NetworkInfo new_session =
        make_test_network_info(1, 4, 0x0100000000009999ULL);
    LdnHeader sync_header =
        make_test_header(PacketId::SyncNetwork, sizeof(new_session));
    session.handle_sync_network(sync_header, new_session);

    ASSERT_TRUE(session.is_in_session());
    ASSERT_TRUE(session.is_host());
    ASSERT_EQ(session.get_node_count(), 1);
}

// ============================================================================
// Tests - Ping Handling
// ============================================================================

/**
 * @brief A server Ping (requester 0) requires an echo and its id is stored.
 */
TEST(integration_ping_echo_required) {
    ldn::LdnSessionHandler session;

    InitializeMessage init{};
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    session.handle_initialize(init_header, init);

    // Server pings us (requester=0)
    PingMessage ping{};
    ping.requester = 0;  // Server requesting echo
    ping.id = 42;
    LdnHeader ping_header = make_test_header(PacketId::Ping, sizeof(ping));
    bool needs_echo = session.handle_ping(ping_header, ping);

    ASSERT_TRUE(needs_echo);
    ASSERT_EQ(session.get_last_ping_id(), 42);
}

// ============================================================================
// Tests - Dispatcher Integration
// ============================================================================

// Global handlers for dispatcher tests (since we can't use lambdas with
// capture)
/// Session handler the on_* trampolines forward to (set by the dispatcher
/// test).
static ldn::LdnSessionHandler* g_session = nullptr;
/// Proxy handler the on_* trampolines forward to (set by the dispatcher test).
static ldn::LdnProxyHandler* g_proxy = nullptr;

/**
 * @brief Dispatcher callback forwarding Initialize to g_session (if set).
 * @param h Packet header.
 * @param m Decoded Initialize message.
 */
static void on_initialize(const LdnHeader& h, const InitializeMessage& m) {
    if (g_session != nullptr) {
        g_session->handle_initialize(h, m);
    }
}

/**
 * @brief Dispatcher callback forwarding Connected to g_session (if set).
 * @param h Packet header.
 * @param i Network information of the joined session.
 */
static void on_connected(const LdnHeader& h, const NetworkInfo& i) {
    if (g_session != nullptr) {
        g_session->handle_connected(h, i);
    }
}

/**
 * @brief Dispatcher callback forwarding SyncNetwork to g_session (if set).
 * @param h Packet header.
 * @param i Updated network information.
 */
static void on_sync_network(const LdnHeader& h, const NetworkInfo& i) {
    if (g_session != nullptr) {
        g_session->handle_sync_network(h, i);
    }
}

/**
 * @brief Dispatcher callback forwarding ProxyConfig to g_proxy (if set).
 * @param h Packet header.
 * @param c Proxy configuration (virtual IP and subnet mask).
 */
static void on_proxy_config(const LdnHeader& h, const ProxyConfig& c) {
    if (g_proxy != nullptr) {
        g_proxy->handle_proxy_config(h, c);
    }
}

/**
 * @brief Dispatcher callback forwarding ProxyConnect to g_proxy (if set).
 * @param h Packet header.
 * @param r Connect request of the peer.
 */
static void on_proxy_connect(const LdnHeader& h, const ProxyConnectRequest& r) {
    if (g_proxy != nullptr) {
        g_proxy->handle_proxy_connect(h, r);
    }
}

/**
 * @brief PacketDispatcher routes Initialize and SyncNetwork to the session
 *        handler and ProxyConfig to the proxy handler.
 */
TEST(integration_dispatcher_routes_to_handlers) {
    ldn::PacketDispatcher dispatcher;
    ldn::LdnSessionHandler session;
    ldn::LdnProxyHandler proxy;

    g_session = &session;
    g_proxy = &proxy;

    // Wire up handlers
    dispatcher.set_initialize_handler(on_initialize);
    dispatcher.set_connected_handler(on_connected);
    dispatcher.set_sync_network_handler(on_sync_network);
    dispatcher.set_proxy_config_handler(on_proxy_config);
    dispatcher.set_proxy_connect_handler(on_proxy_connect);

    // Create and dispatch Initialize packet
    InitializeMessage init{};
    init.id.data[0] = 0x42;
    LdnHeader init_header =
        make_test_header(PacketId::Initialize, sizeof(init));
    dispatcher.dispatch(
        init_header, reinterpret_cast<uint8_t*>(&init), sizeof(init));

    ASSERT_EQ(session.get_state(), ldn::LdnSessionState::Initialized);

    // Create and dispatch SyncNetwork packet (as host)
    session.set_local_node_id(0);
    NetworkInfo ap_info = make_test_network_info(1, 8);
    LdnHeader sync_header =
        make_test_header(PacketId::SyncNetwork, sizeof(ap_info));
    dispatcher.dispatch(
        sync_header, reinterpret_cast<uint8_t*>(&ap_info), sizeof(ap_info));

    ASSERT_TRUE(session.is_host());
    ASSERT_TRUE(session.is_in_session());

    // Create and dispatch ProxyConfig packet
    ProxyConfig config{};
    config.proxy_ip = 0x0A720001;
    config.proxy_subnet_mask = 0xFFFF0000;
    LdnHeader config_header =
        make_test_header(PacketId::ProxyConfig, sizeof(config));
    dispatcher.dispatch(
        config_header, reinterpret_cast<uint8_t*>(&config), sizeof(config));

    ASSERT_TRUE(proxy.is_configured());

    g_session = nullptr;
    g_proxy = nullptr;
}

// ============================================================================
// Main
// ============================================================================

/**
 * @brief Test entry point: the tests already ran during static initialisation,
 *        so this only prints the totals.
 * @return 1 when any assertion failed, otherwise 0.
 */
int main() {
    printf("\n========================================\n");
    printf("  LDN Handler Integration Tests\n");
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
