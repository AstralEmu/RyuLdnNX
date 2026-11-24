/**
 * @file tcp_client_tests.cpp
 * @brief Unit tests for ryu_ldn::network::TcpClient
 *
 * These tests verify the TcpClient class functionality including:
 * - Construction and state management
 * - Connection handling (without actual server)
 * - Send operations (encoding verification)
 * - Result code mapping
 * - Move semantics
 *
 * ## Test Strategy
 *
 * Since TcpClient depends on actual network connectivity for full testing,
 * these unit tests focus on:
 * 1. State management (connected/disconnected)
 * 2. Error handling (proper error codes returned)
 * 3. API contracts (correct behavior when disconnected)
 * 4. Result code string conversion
 *
 * Integration tests with a mock server would be needed for complete coverage.
 */

#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "network/tcp_client.hpp"

using ryu_ldn::network::ClientResult;
using ryu_ldn::network::socket_exit;
using ryu_ldn::network::socket_init;
using ryu_ldn::network::socket_is_initialized;
using ryu_ldn::network::SocketResult;
using ryu_ldn::network::TcpClient;
using ryu_ldn::protocol::AddressEntry;
using ryu_ldn::protocol::AddressList;
using ryu_ldn::protocol::ConnectPrivateRequest;
using ryu_ldn::protocol::ConnectRequest;
using ryu_ldn::protocol::CreateAccessPointPrivateRequest;
using ryu_ldn::protocol::CreateAccessPointRequest;
using ryu_ldn::protocol::DisconnectMessage;
using ryu_ldn::protocol::EncodeResult;
using ryu_ldn::protocol::get_packet_size;
using ryu_ldn::protocol::InitializeMessage;
using ryu_ldn::protocol::LdnHeader;
using ryu_ldn::protocol::PacketId;
using ryu_ldn::protocol::PassphraseMessage;
using ryu_ldn::protocol::PingMessage;
using ryu_ldn::protocol::PROTOCOL_MAGIC;
using ryu_ldn::protocol::PROTOCOL_VERSION;
using ryu_ldn::protocol::ProtocolType;
using ryu_ldn::protocol::ProxyDataHeader;
using ryu_ldn::protocol::RejectRequest;
using ryu_ldn::protocol::ScanFilterFull;
using ryu_ldn::protocol::SecurityParameter;
using ryu_ldn::protocol::SetAcceptPolicyRequest;

/**
 * @brief Observe whether a TcpClient left behind by a move is disconnected.
 *
 * Contract: TcpClient's move constructor and move assignment move the socket
 * out of the source and reset its buffers, so the source reads as disconnected;
 * only that empty state may be observed. The source is named through std::cref
 * before the move, so the observation targets the same object without reading
 * the moved-from variable itself.
 *
 * @param client Client whose move already happened.
 * @return true when the source reads as disconnected.
 */
bool moved_from_is_connected(const TcpClient& client) {
    return client.is_connected();
}

/**
 * @brief Build a ClientResult holding a raw value.
 *
 * ClientResult's underlying type is int, so any int is a valid value of the
 * enum; the value given here names no enumerator on purpose, to exercise the
 * "Unknown" fallback of client_result_to_string().
 *
 * @param raw Underlying value to reinterpret.
 * @return The enum value holding raw.
 */
ClientResult unknown_client_result(int raw) {
    ClientResult result{};
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

// =============================================================================
// Test Framework
// =============================================================================

/// Number of tests started by the TEST registrars.
static int g_tests_run = 0;
/// Number of tests that finished without a failed assertion.
static int g_tests_passed = 0;
/// Number of tests aborted by a failed assertion (non-zero fails the run).
static int g_tests_failed = 0;

/**
 * @brief Define and auto-run a test.
 * Declares test_<name>() and a static registrar whose constructor runs the
 * body during static initialization, counting it as passed or, when an
 * ASSERT throws a message, as failed.
 * @param name Identifier of the test (also printed, stringified).
 */
#define TEST(name)                               \
    static void test_##name();                   \
    static struct TestRegister_##name {          \
        TestRegister_##name() {                  \
            printf("  [TEST] " #name "... ");    \
            fflush(stdout);                      \
            g_tests_run++;                       \
            try {                                \
                test_##name();                   \
                printf("PASS\n");                \
                g_tests_passed++;                \
            } catch (const std::exception& ex) { \
                printf("FAIL: %s\n", ex.what()); \
                g_tests_failed++;                \
            }                                    \
        }                                        \
    } g_test_register_##name;                    \
    static void test_##name()

/**
 * @brief Fail the current test (throw) when cond is false.
 * @param cond Expression expected to be true.
 */
#define ASSERT(cond)                                              \
    do {                                                          \
        if (!(cond)) {                                            \
            throw std::runtime_error("Assertion failed: " #cond); \
        }                                                         \
    } while (0)

/**
 * @brief Fail the current test (throw) when a != b.
 * @param a First value.
 * @param b Expected value.
 */
#define ASSERT_EQ(a, b)                                                  \
    do {                                                                 \
        if ((a) != (b)) {                                                \
            throw std::runtime_error("Assertion failed: " #a " != " #b); \
        }                                                                \
    } while (0)

/**
 * @brief Fail the current test (throw) when a == b.
 * @param a First value.
 * @param b Value that must differ.
 */
#define ASSERT_NE(a, b)                                                  \
    do {                                                                 \
        if ((a) == (b)) {                                                \
            throw std::runtime_error("Assertion failed: " #a " == " #b); \
        }                                                                \
    } while (0)

/**
 * @brief Alias of ASSERT.
 * @param cond Expression expected to be true.
 */
#define ASSERT_TRUE(cond) ASSERT(cond)
/**
 * @brief Fail the current test when cond is true.
 * @param cond Expression expected to be false.
 */
#define ASSERT_FALSE(cond) ASSERT(!(cond))

// =============================================================================
// Tests: Construction and State
// =============================================================================

/**
 * @test Default constructed client is disconnected
 */
TEST(default_disconnected) {
    socket_init();

    TcpClient client;
    ASSERT_FALSE(client.is_connected());
}

/**
 * @test Multiple clients can be created
 */
TEST(multiple_clients) {
    socket_init();

    TcpClient client1;
    TcpClient client2;
    TcpClient client3;

    ASSERT_FALSE(client1.is_connected());
    ASSERT_FALSE(client2.is_connected());
    ASSERT_FALSE(client3.is_connected());
}

/**
 * @test Client destructor doesn't crash when disconnected
 */
TEST(destructor_safe_disconnected) {
    socket_init();

    {
        TcpClient client;
        // Destructor called here
    }
    // If we get here, test passed
    ASSERT_TRUE(true);
}

/**
 * @test Move constructor transfers state
 */
TEST(move_constructor) {
    socket_init();

    const auto client1 = std::make_unique<TcpClient>();
    const TcpClient& client1_source = *client1;
    TcpClient client2(std::move(*client1));

    ASSERT_FALSE(moved_from_is_connected(client1_source));
    ASSERT_FALSE(client2.is_connected());
}

/**
 * @test Move assignment transfers state
 */
TEST(move_assignment) {
    socket_init();

    const auto client1 = std::make_unique<TcpClient>();
    const TcpClient& client1_source = *client1;
    TcpClient client2;

    client2 = std::move(*client1);

    ASSERT_FALSE(moved_from_is_connected(client1_source));
    ASSERT_FALSE(client2.is_connected());
}

// =============================================================================
// Tests: Connection (Without Server)
// =============================================================================

/**
 * @test Connect to non-existent server fails
 */
TEST(connect_fails_no_server) {
    socket_init();

    TcpClient client;

    // Try to connect to a port that's almost certainly not listening
    ClientResult result = client.connect("127.0.0.1", 59999, 500);

    ASSERT_NE(result, ClientResult::Success);
    ASSERT_FALSE(client.is_connected());
}

/**
 * @test Connect with invalid host returns error
 */
TEST(connect_invalid_host) {
    socket_init();

    TcpClient client;
    ClientResult result =
        client.connect("invalid.host.that.does.not.exist.local", 30456, 500);

    ASSERT_NE(result, ClientResult::Success);
    ASSERT_FALSE(client.is_connected());
}

/**
 * @test Connect with null host returns error
 */
TEST(connect_null_host) {
    socket_init();

    TcpClient client;
    ClientResult result = client.connect(nullptr, 30456, 500);

    ASSERT_NE(result, ClientResult::Success);
    ASSERT_FALSE(client.is_connected());
}

/**
 * @test Connect with empty host returns error
 */
TEST(connect_empty_host) {
    socket_init();

    TcpClient client;
    ClientResult result = client.connect("", 30456, 500);

    ASSERT_NE(result, ClientResult::Success);
    ASSERT_FALSE(client.is_connected());
}

/**
 * @test Disconnect on non-connected client is safe
 */
TEST(disconnect_not_connected) {
    socket_init();

    TcpClient client;
    ASSERT_FALSE(client.is_connected());

    client.disconnect();  // Should not crash
    client.disconnect();  // Multiple disconnects should be safe

    ASSERT_FALSE(client.is_connected());
}

// =============================================================================
// Tests: Send Operations (Disconnected)
// =============================================================================

/**
 * @test send_initialize fails when disconnected
 */
TEST(send_initialize_not_connected) {
    socket_init();

    TcpClient client;

    InitializeMessage msg{};
    ClientResult result = client.send_initialize(msg);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_passphrase fails when disconnected
 */
TEST(send_passphrase_not_connected) {
    socket_init();

    TcpClient client;

    PassphraseMessage msg{};
    ClientResult result = client.send_passphrase(msg);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_ping fails when disconnected
 */
TEST(send_ping_not_connected) {
    socket_init();

    TcpClient client;

    PingMessage msg{};
    msg.requester = 1;
    msg.id = 42;
    ClientResult result = client.send_ping(msg);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_disconnect fails when disconnected
 */
TEST(send_disconnect_not_connected) {
    socket_init();

    TcpClient client;

    DisconnectMessage msg{};
    msg.disconnect_ip = 0xC0A80101;  // 192.168.1.1
    ClientResult result = client.send_disconnect(msg);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_packet fails when disconnected
 */
TEST(send_packet_not_connected) {
    socket_init();

    TcpClient client;

    uint8_t data[] = {0x01, 0x02, 0x03};
    ClientResult result =
        client.send_packet(PacketId::Ping, data, sizeof(data));

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_create_access_point fails when disconnected
 */
TEST(send_create_access_point_not_connected) {
    socket_init();

    TcpClient client;

    CreateAccessPointRequest request{};
    ClientResult result = client.send_create_access_point(request);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_connect fails when disconnected
 */
TEST(send_connect_not_connected) {
    socket_init();

    TcpClient client;

    ConnectRequest request{};
    ClientResult result = client.send_connect(request);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_scan fails when disconnected
 */
TEST(send_scan_not_connected) {
    socket_init();

    TcpClient client;

    ScanFilterFull filter{};
    ClientResult result = client.send_scan(filter);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_proxy_data fails when disconnected
 */
TEST(send_proxy_data_not_connected) {
    socket_init();

    TcpClient client;

    ProxyDataHeader header{};
    header.info.source_ipv4 = 0xC0A80101;  // 192.168.1.1
    header.info.source_port = 12345;
    header.info.dest_ipv4 = 0xC0A80102;  // 192.168.1.2
    header.info.dest_port = 54321;
    header.info.protocol = ProtocolType::Udp;
    header.data_length = 3;
    uint8_t data[] = {0xAA, 0xBB, 0xCC};

    ClientResult result = client.send_proxy_data(header, data, sizeof(data));

    ASSERT_EQ(result, ClientResult::NotConnected);
}

// =============================================================================
// Tests: Receive Operations (Disconnected)
// =============================================================================

/**
 * @test receive_packet fails when disconnected
 */
TEST(receive_packet_not_connected) {
    socket_init();

    TcpClient client;

    PacketId type;
    uint8_t payload[256];
    size_t payload_size;

    ClientResult result =
        client.receive_packet(type, payload, sizeof(payload), payload_size, 0);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test has_packet_available returns false when disconnected
 */
TEST(has_packet_not_connected) {
    socket_init();

    TcpClient client;

    ASSERT_FALSE(client.has_packet_available());
}

// =============================================================================
// Tests: Configuration (Disconnected)
// =============================================================================

/**
 * @test set_nodelay fails when disconnected
 */
TEST(set_nodelay_not_connected) {
    socket_init();

    TcpClient client;

    ClientResult result = client.set_nodelay(true);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

// =============================================================================
// Tests: Result String Conversion
// =============================================================================

/**
 * @test client_result_to_string covers all values
 */
TEST(result_to_string_success) {
    ASSERT(
        strcmp(client_result_to_string(ClientResult::Success), "Success") == 0);
}

/// @brief client_result_to_string maps NotConnected to "NotConnected".
TEST(result_to_string_not_connected) {
    ASSERT(
        strcmp(
            client_result_to_string(ClientResult::NotConnected),
            "NotConnected") == 0);
}

/// @brief client_result_to_string maps AlreadyConnected to "AlreadyConnected".
TEST(result_to_string_already_connected) {
    ASSERT(
        strcmp(
            client_result_to_string(ClientResult::AlreadyConnected),
            "AlreadyConnected") == 0);
}

/// @brief client_result_to_string maps ConnectionFailed to "ConnectionFailed".
TEST(result_to_string_connection_failed) {
    ASSERT(
        strcmp(
            client_result_to_string(ClientResult::ConnectionFailed),
            "ConnectionFailed") == 0);
}

/// @brief client_result_to_string maps ConnectionLost to "ConnectionLost".
TEST(result_to_string_connection_lost) {
    ASSERT(
        strcmp(
            client_result_to_string(ClientResult::ConnectionLost),
            "ConnectionLost") == 0);
}

/// @brief client_result_to_string maps Timeout to "Timeout".
TEST(result_to_string_timeout) {
    ASSERT(
        strcmp(client_result_to_string(ClientResult::Timeout), "Timeout") == 0);
}

/// @brief client_result_to_string maps InvalidPacket to "InvalidPacket".
TEST(result_to_string_invalid_packet) {
    ASSERT(
        strcmp(
            client_result_to_string(ClientResult::InvalidPacket),
            "InvalidPacket") == 0);
}

/// @brief client_result_to_string maps ProtocolError to "ProtocolError".
TEST(result_to_string_protocol_error) {
    ASSERT(
        strcmp(
            client_result_to_string(ClientResult::ProtocolError),
            "ProtocolError") == 0);
}

/// @brief client_result_to_string maps BufferTooSmall to "BufferTooSmall".
TEST(result_to_string_buffer_too_small) {
    ASSERT(
        strcmp(
            client_result_to_string(ClientResult::BufferTooSmall),
            "BufferTooSmall") == 0);
}

/// @brief client_result_to_string maps EncodingError to "EncodingError".
TEST(result_to_string_encoding_error) {
    ASSERT(
        strcmp(
            client_result_to_string(ClientResult::EncodingError),
            "EncodingError") == 0);
}

/// @brief client_result_to_string maps NotInitialized to "NotInitialized".
TEST(result_to_string_not_initialized) {
    ASSERT(
        strcmp(
            client_result_to_string(ClientResult::NotInitialized),
            "NotInitialized") == 0);
}

/// @brief client_result_to_string maps InternalError to "InternalError".
TEST(result_to_string_internal_error) {
    ASSERT(
        strcmp(
            client_result_to_string(ClientResult::InternalError),
            "InternalError") == 0);
}

/// @brief An out-of-range ClientResult value maps to "Unknown".
TEST(result_to_string_unknown) {
    auto unknown = unknown_client_result(9999);
    ASSERT(strcmp(client_result_to_string(unknown), "Unknown") == 0);
}

// =============================================================================
// Tests: Operations Before Socket Init
// =============================================================================

/**
 * @test Connect fails if socket subsystem not initialized
 */
TEST(connect_before_socket_init) {
    socket_exit();  // Ensure not initialized
    ASSERT_FALSE(socket_is_initialized());

    TcpClient client;
    ClientResult result = client.connect("127.0.0.1", 30456, 500);

    ASSERT_EQ(result, ClientResult::NotInitialized);

    // Restore for other tests
    socket_init();
}

// =============================================================================
// Tests: State After Failed Connect
// =============================================================================

/**
 * @test Client state is consistent after failed connect
 */
TEST(state_after_failed_connect) {
    socket_init();

    TcpClient client;

    // First failed connect
    ClientResult result1 = client.connect("127.0.0.1", 59998, 100);
    ASSERT_NE(result1, ClientResult::Success);
    ASSERT_FALSE(client.is_connected());

    // Second failed connect should also work
    ClientResult result2 = client.connect("127.0.0.1", 59997, 100);
    ASSERT_NE(result2, ClientResult::Success);
    ASSERT_FALSE(client.is_connected());

    // Disconnect should be safe
    client.disconnect();
    ASSERT_FALSE(client.is_connected());
}

/**
 * @test Can retry send after disconnect
 */
TEST(retry_after_failed_connect) {
    socket_init();

    TcpClient client;

    // Failed connect
    client.connect("127.0.0.1", 59996, 100);

    // Send should fail
    PingMessage msg{};
    ClientResult result = client.send_ping(msg);
    ASSERT_EQ(result, ClientResult::NotConnected);

    // Another failed connect
    client.connect("127.0.0.1", 59995, 100);

    // Send should still fail
    result = client.send_ping(msg);
    ASSERT_EQ(result, ClientResult::NotConnected);
}

// =============================================================================
// Tests: Edge Cases
// =============================================================================

/**
 * @test Multiple destructor calls (via multiple moves)
 */
TEST(multiple_moves_safe) {
    socket_init();

    const auto client1 = std::make_unique<TcpClient>();
    const TcpClient& client1_source = *client1;
    const auto client2 = std::make_unique<TcpClient>(std::move(*client1));
    const TcpClient& client2_source = *client2;
    const auto client3 = std::make_unique<TcpClient>(std::move(*client2));
    const TcpClient& client3_source = *client3;
    TcpClient client4;
    client4 = std::move(*client3);

    ASSERT_FALSE(moved_from_is_connected(client1_source));
    ASSERT_FALSE(moved_from_is_connected(client2_source));
    ASSERT_FALSE(moved_from_is_connected(client3_source));
    ASSERT_FALSE(client4.is_connected());
}

/**
 * @test Sending empty proxy data
 */
TEST(send_empty_proxy_data) {
    socket_init();

    TcpClient client;

    ProxyDataHeader header{};
    ClientResult result = client.send_proxy_data(header, nullptr, 0);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

// =============================================================================
// Tests: Private Room Operations (Story 7.7)
// =============================================================================

/**
 * @test send_create_access_point_private fails when disconnected
 */
TEST(send_create_access_point_private_not_connected) {
    socket_init();

    TcpClient client;

    CreateAccessPointPrivateRequest request{};
    request.security_config.security_mode = 2;
    request.security_parameter.data[0] = 0xAA;
    request.network_config.node_count_max = 8;

    ClientResult result = client.send_create_access_point_private(request);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_create_access_point_private with advertise data fails when
 * disconnected
 */
TEST(send_create_access_point_private_with_advertise_not_connected) {
    socket_init();

    TcpClient client;

    CreateAccessPointPrivateRequest request{};
    request.security_config.security_mode = 2;

    uint8_t advertise_data[] = {0x01, 0x02, 0x03, 0x04};
    ClientResult result = client.send_create_access_point_private(
        request, advertise_data, sizeof(advertise_data));

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_connect_private fails when disconnected
 */
TEST(send_connect_private_not_connected) {
    socket_init();

    TcpClient client;

    ConnectPrivateRequest request{};
    request.security_config.security_mode = 2;
    request.security_parameter.data[0] = 0xBB;
    request.local_communication_version = 1;
    request.network_config.node_count_max = 4;

    ClientResult result = client.send_connect_private(request);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test CreateAccessPointPrivateRequest size is correct
 */
TEST(create_access_point_private_request_size) {
    ASSERT_EQ(sizeof(CreateAccessPointPrivateRequest), 0x13CU);  // 316 bytes
}

/**
 * @test ConnectPrivateRequest size is correct
 */
TEST(connect_private_request_size) {
    ASSERT_EQ(sizeof(ConnectPrivateRequest), 0xBCU);  // 188 bytes
}

/**
 * @test SecurityParameter size is correct
 */
TEST(security_parameter_size) {
    ASSERT_EQ(sizeof(SecurityParameter), 0x20U);  // 32 bytes
}

/**
 * @test AddressList size is correct
 */
TEST(address_list_size) {
    ASSERT_EQ(sizeof(AddressList), 0x60U);  // 96 bytes
}

/**
 * @test AddressEntry size is correct
 */
TEST(address_entry_size) {
    ASSERT_EQ(sizeof(AddressEntry), 0x0CU);  // 12 bytes
}

// =============================================================================
// Tests: Additional Not-Connected Path Coverage
// =============================================================================

/**
 * @test send_raw fails when not connected
 */
TEST(send_raw_not_connected) {
    socket_init();
    TcpClient client;

    uint8_t data[] = {0x01, 0x02, 0x03};
    ClientResult result = client.send_raw(data, sizeof(data));

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_raw fails with null data when not connected
 */
TEST(send_raw_null_not_connected) {
    socket_init();
    TcpClient client;

    ClientResult result = client.send_raw(nullptr, 10);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_passphrase (char*) fails when not connected
 */
TEST(send_passphrase_cstr_not_connected) {
    socket_init();
    TcpClient client;

    ClientResult result = client.send_passphrase("AA-BB-CC");

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_passphrase (char*) with null fails when not connected
 */
TEST(send_passphrase_cstr_null_not_connected) {
    socket_init();
    TcpClient client;

    ClientResult result =
        client.send_passphrase(static_cast<const char*>(nullptr));

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_set_accept_policy fails when not connected
 */
TEST(send_set_accept_policy_not_connected) {
    socket_init();
    TcpClient client;

    SetAcceptPolicyRequest request{};
    request.accept_policy = static_cast<uint8_t>(1);

    ClientResult result = client.send_set_accept_policy(request);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_set_advertise_data fails when not connected
 */
TEST(send_set_advertise_data_not_connected) {
    socket_init();
    TcpClient client;

    uint8_t data[] = {0x01, 0x02, 0x03};
    ClientResult result = client.send_set_advertise_data(data, sizeof(data));

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_set_advertise_data null data fails when not connected
 */
TEST(send_set_advertise_data_null_not_connected) {
    socket_init();
    TcpClient client;

    ClientResult result = client.send_set_advertise_data(nullptr, 0);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test send_reject fails when not connected
 */
TEST(send_reject_not_connected) {
    socket_init();
    TcpClient client;

    RejectRequest request{};
    request.node_id = 1;
    request.disconnect_reason = 1;

    ClientResult result = client.send_reject(request);

    ASSERT_EQ(result, ClientResult::NotConnected);
}

/**
 * @test has_packet_available returns false when not connected
 */
TEST(has_packet_available_not_connected) {
    socket_init();
    TcpClient client;

    ASSERT_FALSE(client.has_packet_available());
}

/// @brief TcpClient::initialize() succeeds (it initializes the socket layer).
TEST(tcp_client_initialize) {
    TcpClient client;
    bool result = client.initialize();
    ASSERT_EQ(result, true);
    socket_exit();
}

/// @brief TcpClient::initialize() is idempotent: a second call also succeeds.
TEST(tcp_client_initialize_already_init) {
    TcpClient client;
    bool result = client.initialize();
    ASSERT_EQ(result, true);
    // Second init should also succeed (idempotent)
    result = client.initialize();
    ASSERT_EQ(result, true);
    socket_exit();
}

/// @brief A disconnected client reports no packet available.
TEST(tcp_client_has_packet_when_disconnected) {
    TcpClient client;
    ASSERT_TRUE(!client.has_packet_available());
}

/// @brief set_nodelay() on a disconnected client returns NotConnected.
TEST(tcp_client_set_nodelay_not_connected) {
    TcpClient client;
    ClientResult r = client.set_nodelay(true);
    ASSERT_EQ(r, ClientResult::NotConnected);
}

/**
 * @brief Second check of the ClientResult names (ConnectionFailed,
 * ProtocolError, BufferTooSmall, EncodingError, NotInitialized,
 * InternalError) through ASSERT_TRUE.
 */
TEST(client_result_to_string_additional) {
    ASSERT_TRUE(
        strcmp(
            client_result_to_string(ClientResult::ConnectionFailed),
            "ConnectionFailed") == 0);
    ASSERT_TRUE(
        strcmp(
            client_result_to_string(ClientResult::ProtocolError),
            "ProtocolError") == 0);
    ASSERT_TRUE(
        strcmp(
            client_result_to_string(ClientResult::BufferTooSmall),
            "BufferTooSmall") == 0);
    ASSERT_TRUE(
        strcmp(
            client_result_to_string(ClientResult::EncodingError),
            "EncodingError") == 0);
    ASSERT_TRUE(
        strcmp(
            client_result_to_string(ClientResult::NotInitialized),
            "NotInitialized") == 0);
    ASSERT_TRUE(
        strcmp(
            client_result_to_string(ClientResult::InternalError),
            "InternalError") == 0);
}

/**
 * @brief send_create_access_point_private() with default arguments on a
 * disconnected client returns NotConnected.
 */
TEST(tcp_client_send_create_access_point_private_not_connected) {
    TcpClient client;
    CreateAccessPointPrivateRequest req{};
    ClientResult r = client.send_create_access_point_private(req);
    ASSERT_EQ(r, ClientResult::NotConnected);
}

/**
 * @brief send_connect_private() on a disconnected client returns
 * NotConnected.
 */
TEST(tcp_client_send_connect_private_not_connected) {
    TcpClient client;
    ConnectPrivateRequest req{};
    ClientResult r = client.send_connect_private(req);
    ASSERT_EQ(r, ClientResult::NotConnected);
}

// =============================================================================
// Tests: Connected behaviour against a real loopback peer
// =============================================================================

/**
 * Loopback TCP peer standing in for the Ryujinx LDN server: it listens on an
 * OS-assigned port and, once the client connected, accepts the connection so
 * the test can read what the client wrote and write what it must receive.
 */
class Peer {
public:
    Peer() {
        m_listen = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_listen < 0) {
            throw std::runtime_error("socket failed");
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
            0) {
            throw std::runtime_error("bind failed");
        }
        ::listen(m_listen, 1);
        socklen_t len = sizeof(addr);
        ::getsockname(m_listen, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
    }

    ~Peer() {
        close_connection();
        if (m_listen >= 0) {
            ::close(m_listen);
        }
    }

    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    /**
     * @brief Get the port the peer listens on.
     * @return Listening port in host byte order.
     */
    [[nodiscard]] uint16_t port() const {
        return m_port;
    }

    /**
     * @brief Accept the pending client connection (the client already
     * connected).
     *
     * @return true when a client was accepted (receive timeout set to 2 s),
     * false when accept() failed
     */
    bool accept_client() {
        m_conn = ::accept(m_listen, nullptr, nullptr);
        if (m_conn < 0) {
            return false;
        }
        timeval tv{.tv_sec = 2, .tv_usec = 0};
        ::setsockopt(m_conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        return true;
    }

    /// @brief Read exactly `size` bytes; returns what was read (shorter on
    /// EOF/timeout).
    /// @param size Number of bytes to read (2 s receive timeout per recv).
    /// @return The bytes read, possibly fewer than `size`.
    [[nodiscard]] std::vector<uint8_t> read_bytes(size_t size) const {
        std::vector<uint8_t> out(size);
        size_t got = 0;
        while (got < size) {
            ssize_t n = ::recv(m_conn, out.data() + got, size - got, 0);
            if (n <= 0) {
                break;
            }
            got += static_cast<size_t>(n);
        }
        out.resize(got);
        return out;
    }

    /**
     * @brief Read one whole LDN packet (header, then data_size payload bytes).
     *
     * @return header plus payload bytes; shorter than a header when the read
     * timed out
     */
    [[nodiscard]] std::vector<uint8_t> read_packet() const {
        std::vector<uint8_t> pkt = read_bytes(sizeof(LdnHeader));
        if (pkt.size() != sizeof(LdnHeader)) {
            return pkt;
        }
        LdnHeader h{};
        std::memcpy(&h, pkt.data(), sizeof(h));
        std::vector<uint8_t> body =
            read_bytes(static_cast<size_t>(h.data_size));
        pkt.insert(pkt.end(), body.begin(), body.end());
        return pkt;
    }

    /**
     * @brief Send bytes to the connected client (as the server would).
     * @param data Bytes to send.
     * @param size Number of bytes in `data`.
     * @return true when all `size` bytes were written.
     */
    bool write_bytes(const void* data, size_t size) const {
        return ::send(m_conn, data, size, MSG_NOSIGNAL) ==
               static_cast<ssize_t>(size);
    }

    /// @brief Close the accepted connection (FIN); safe to call repeatedly.
    void close_connection() {
        if (m_conn >= 0) {
            ::close(m_conn);
            m_conn = -1;
        }
    }

    /// Abort the connection with a RST instead of a FIN (SO_LINGER, 0 s).
    void reset_connection() {
        if (m_conn >= 0) {
            linger lg{.l_onoff = 1, .l_linger = 0};
            ::setsockopt(m_conn, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        }
        close_connection();
    }

private:
    /// Listening socket on the loopback interface, or -1.
    int m_listen = -1;
    /// Accepted connection to the client under test, or -1.
    int m_conn = -1;
    /// Port the OS assigned to the listening socket (host order).
    uint16_t m_port = 0;
};

/// Connect `client` to `peer` and accept on the peer side; asserts both.
#define CONNECT_PAIR(client, peer)                              \
    do {                                                        \
        socket_init();                                          \
        ASSERT_EQ(                                              \
            (client).connect("127.0.0.1", (peer).port(), 2000), \
            ClientResult::Success);                             \
        ASSERT_TRUE((peer).accept_client());                    \
    } while (0)

/// Same as CONNECT_PAIR for helpers returning bool instead of asserting.
#define CONNECT_PAIR_BOOL(client, peer)                           \
    do {                                                          \
        socket_init();                                            \
        if ((client).connect("127.0.0.1", (peer).port(), 2000) != \
            ClientResult::Success)                                \
            return false;                                         \
        if (!(peer).accept_client())                              \
            return false;                                         \
    } while (0)

/**
 * @brief Build one wire packet (header + payload) the way the server would.
 *
 * @param type packet id
 * @param payload payload bytes, may be null when size is 0
 * @param size payload size in bytes
 * @return encoded header plus payload, throws a C string when encoding fails
 */
static std::vector<uint8_t> make_packet(
    PacketId type, const void* payload, size_t size) {
    std::vector<uint8_t> buf(sizeof(LdnHeader) + size);
    size_t written = 0;
    EncodeResult r = encode_raw(
        buf.data(),
        buf.size(),
        type,
        static_cast<const uint8_t*>(payload),
        size,
        written);
    if (r != EncodeResult::Success || written != buf.size()) {
        throw std::runtime_error("make_packet: encode_raw failed");
    }
    return buf;
}

/// Assert a received wire packet has the expected type and payload size.
#define ASSERT_WIRE(pkt, expect_type, expect_payload)                   \
    do {                                                                \
        ASSERT_EQ((pkt).size(), sizeof(LdnHeader) + (expect_payload));  \
        LdnHeader hdr_{};                                               \
        std::memcpy(&hdr_, (pkt).data(), sizeof(hdr_));                 \
        ASSERT_EQ(hdr_.magic, PROTOCOL_MAGIC);                          \
        ASSERT_EQ(hdr_.type, static_cast<uint8_t>(expect_type));        \
        ASSERT_EQ(hdr_.version, PROTOCOL_VERSION);                      \
        ASSERT_EQ(static_cast<size_t>(hdr_.data_size), expect_payload); \
    } while (0)

/**
 * @brief A connected client can toggle TCP_NODELAY, answers a second
 * connect() with AlreadyConnected, and disconnects cleanly.
 */
TEST(connected_connect_enables_nodelay_and_rejects_second_connect) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    ASSERT_TRUE(client.is_connected());
    ASSERT_EQ(client.set_nodelay(true), ClientResult::Success);
    ASSERT_EQ(client.set_nodelay(false), ClientResult::Success);
    ASSERT_EQ(
        client.connect("127.0.0.1", peer.port(), 2000),
        ClientResult::AlreadyConnected);
    ASSERT_TRUE(client.is_connected());
    client.disconnect();
    ASSERT_FALSE(client.is_connected());
}

/**
 * @brief Move-assigning a connected client moves the live connection: the
 * target can send on it and the source reads as disconnected.
 */
TEST(connected_move_assignment_transfers_the_connection) {
    Peer peer;
    const auto source = std::make_unique<TcpClient>();
    CONNECT_PAIR(*source, peer);
    const TcpClient& source_observer = *source;
    TcpClient target;
    target = std::move(*source);
    ASSERT_TRUE(target.is_connected());
    ASSERT_FALSE(moved_from_is_connected(source_observer));
    ASSERT_EQ(target.send_raw("ab", 2), ClientResult::Success);
    ASSERT_EQ(peer.read_bytes(2).size(), static_cast<size_t>(2));
}

/**
 * @brief send_packet() puts an LdnHeader followed by the payload on the
 * wire, with the right packet type and size.
 */
TEST(connected_send_packet_writes_header_and_payload) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    const uint8_t payload[5] = {1, 2, 3, 4, 5};
    ASSERT_EQ(
        client.send_packet(PacketId::Ping, payload, sizeof(payload)),
        ClientResult::Success);
    auto pkt = peer.read_packet();
    ASSERT_WIRE(pkt, PacketId::Ping, sizeof(payload));
    ASSERT_EQ(
        std::memcmp(pkt.data() + sizeof(LdnHeader), payload, sizeof(payload)),
        0);
}

/**
 * @brief A payload that does not fit the fixed send buffer is refused with
 * EncodingError and nothing is sent.
 */
TEST(connected_send_packet_larger_than_buffer_is_an_encoding_error) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    std::vector<uint8_t> big(4096, 0x5A);
    ASSERT_EQ(
        client.send_packet(PacketId::Ping, big.data(), big.size()),
        ClientResult::EncodingError);
}

/**
 * @brief send_raw() refuses a null pointer or zero length with
 * EncodingError and otherwise forwards the bytes untouched.
 */
TEST(connected_send_raw_forwards_bytes_unchanged_and_validates_input) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    ASSERT_EQ(client.send_raw(nullptr, 4), ClientResult::EncodingError);
    ASSERT_EQ(client.send_raw("x", 0), ClientResult::EncodingError);
    const uint8_t raw[3] = {9, 8, 7};
    ASSERT_EQ(client.send_raw(raw, sizeof(raw)), ClientResult::Success);
    auto got = peer.read_bytes(sizeof(raw));
    ASSERT_EQ(got.size(), sizeof(raw));
    ASSERT_EQ(std::memcmp(got.data(), raw, sizeof(raw)), 0);
}

/**
 * @brief Every fixed-size send_*() helper emits a packet with its own
 * PacketId and the sizeof of its message.
 */
TEST(connected_fixed_size_messages_carry_their_packet_id_and_size) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);

    InitializeMessage init{};
    ASSERT_EQ(client.send_initialize(init), ClientResult::Success);
    auto p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::Initialize, sizeof(InitializeMessage));

    PassphraseMessage pass{};
    ASSERT_EQ(client.send_passphrase(pass), ClientResult::Success);
    p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::Passphrase, sizeof(PassphraseMessage));

    PingMessage ping{};
    ASSERT_EQ(client.send_ping(ping), ClientResult::Success);
    p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::Ping, sizeof(PingMessage));

    DisconnectMessage disc{};
    ASSERT_EQ(client.send_disconnect(disc), ClientResult::Success);
    p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::Disconnect, sizeof(DisconnectMessage));

    ConnectRequest conn{};
    ASSERT_EQ(client.send_connect(conn), ClientResult::Success);
    p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::Connect, sizeof(ConnectRequest));

    ConnectPrivateRequest connp{};
    ASSERT_EQ(client.send_connect_private(connp), ClientResult::Success);
    p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::ConnectPrivate, sizeof(ConnectPrivateRequest));

    ScanFilterFull scan{};
    ASSERT_EQ(client.send_scan(scan), ClientResult::Success);
    p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::Scan, sizeof(ScanFilterFull));

    SetAcceptPolicyRequest pol{};
    ASSERT_EQ(client.send_set_accept_policy(pol), ClientResult::Success);
    p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::SetAcceptPolicy, sizeof(SetAcceptPolicyRequest));

    RejectRequest rej{};
    ASSERT_EQ(client.send_reject(rej), ClientResult::Success);
    p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::Reject, sizeof(RejectRequest));
}

/// A payload whose every byte is distinct and non-zero, so any reordering,
/// truncation or padding change on the wire shows up as a byte difference.
/// @tparam T Trivially copyable message type to fill.
/// @param seed Value that shifts the byte pattern so messages differ.
/// @return A T whose byte i equals seed + 31 * i + 1 (mod 256).
template <typename T>
static T patterned(uint8_t seed) {
    T value{};
    auto* bytes = reinterpret_cast<uint8_t*>(&value);
    for (size_t i = 0; i < sizeof(T); ++i) {
        bytes[i] = static_cast<uint8_t>(seed + (31 * i) + 1);
    }
    return value;
}

/// Send `msg` through the real TcpClient and require the bytes on the wire to
/// equal what protocol::encode() (the C#-server-verified layout) produces.
/// @tparam T Message type.
/// @tparam SendFn Callable `ClientResult(TcpClient&, const T&)`.
/// @param id Packet id passed to the reference encoder.
/// @param msg Message to send and to encode as reference.
/// @param send Calls the TcpClient send_*() helper under test.
/// @return true when the send succeeded and the peer read exactly the
/// reference bytes.
template <typename T, typename SendFn>
static bool wire_matches_reference(PacketId id, const T& msg, SendFn send) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR_BOOL(client, peer);
    if (send(client, msg) != ClientResult::Success) {
        return false;
    }

    std::vector<uint8_t> expected(get_packet_size<T>());
    size_t written = 0;
    if (encode(expected.data(), expected.size(), id, msg, written) !=
        EncodeResult::Success) {
        return false;
    }

    auto got = peer.read_packet();
    return written == expected.size() && got == expected;
}

/**
 * @brief The bytes each fixed-size send_*() helper puts on the wire equal
 * the output of protocol::encode(), whose layout is verified against the
 * C# server, using payloads with all-distinct bytes.
 */
TEST(connected_fixed_size_messages_are_byte_identical_to_reference_encoder) {
    ASSERT_TRUE(wire_matches_reference(
        PacketId::Initialize,
        patterned<InitializeMessage>(1),
        [](TcpClient& c, const InitializeMessage& m) {
            return c.send_initialize(m);
        }));
    ASSERT_TRUE(wire_matches_reference(
        PacketId::Passphrase,
        patterned<PassphraseMessage>(2),
        [](TcpClient& c, const PassphraseMessage& m) {
            return c.send_passphrase(m);
        }));
    ASSERT_TRUE(wire_matches_reference(
        PacketId::Ping,
        patterned<PingMessage>(3),
        [](TcpClient& c, const PingMessage& m) { return c.send_ping(m); }));
    ASSERT_TRUE(wire_matches_reference(
        PacketId::Disconnect,
        patterned<DisconnectMessage>(4),
        [](TcpClient& c, const DisconnectMessage& m) {
            return c.send_disconnect(m);
        }));
    ASSERT_TRUE(wire_matches_reference(
        PacketId::Connect,
        patterned<ConnectRequest>(5),
        [](TcpClient& c, const ConnectRequest& m) {
            return c.send_connect(m);
        }));
    ASSERT_TRUE(wire_matches_reference(
        PacketId::ConnectPrivate,
        patterned<ConnectPrivateRequest>(6),
        [](TcpClient& c, const ConnectPrivateRequest& m) {
            return c.send_connect_private(m);
        }));
    ASSERT_TRUE(wire_matches_reference(
        PacketId::Scan,
        patterned<ScanFilterFull>(7),
        [](TcpClient& c, const ScanFilterFull& m) { return c.send_scan(m); }));
    ASSERT_TRUE(wire_matches_reference(
        PacketId::SetAcceptPolicy,
        patterned<SetAcceptPolicyRequest>(8),
        [](TcpClient& c, const SetAcceptPolicyRequest& m) {
            return c.send_set_accept_policy(m);
        }));
    ASSERT_TRUE(wire_matches_reference(
        PacketId::Reject,
        patterned<RejectRequest>(9),
        [](TcpClient& c, const RejectRequest& m) { return c.send_reject(m); }));
}

/**
 * @brief send_passphrase(const char*) copies the string into the fixed
 * message, accepts nullptr (empty passphrase) and truncates to 127 characters
 * keeping the NUL terminator.
 */
TEST(connected_send_passphrase_string_copies_and_truncates) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);

    ASSERT_EQ(client.send_passphrase("hello"), ClientResult::Success);
    auto p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::Passphrase, sizeof(PassphraseMessage));
    ASSERT_EQ(std::memcmp(p.data() + sizeof(LdnHeader), "hello", 5), 0);
    ASSERT_EQ(p[sizeof(LdnHeader) + 5], static_cast<uint8_t>(0));

    ASSERT_EQ(
        client.send_passphrase(static_cast<const char*>(nullptr)),
        ClientResult::Success);
    p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::Passphrase, sizeof(PassphraseMessage));
    ASSERT_EQ(p[sizeof(LdnHeader)], static_cast<uint8_t>(0));

    std::string long_text(300, 'x');
    ASSERT_EQ(client.send_passphrase(long_text.c_str()), ClientResult::Success);
    p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::Passphrase, sizeof(PassphraseMessage));
    // Truncated to 127 characters, the 128th byte stays the terminator.
    ASSERT_EQ(p[sizeof(LdnHeader) + 126], static_cast<uint8_t>('x'));
    ASSERT_EQ(p[sizeof(LdnHeader) + 127], static_cast<uint8_t>(0));
}

/**
 * @brief send_create_access_point() appends the advertise data after the
 * request, and sends the request alone when there is none.
 */
TEST(connected_send_create_access_point_appends_advertise_data) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    CreateAccessPointRequest req{};
    const uint8_t adv[40] = {0xAA, 0xBB};

    ASSERT_EQ(
        client.send_create_access_point(req, adv, sizeof(adv)),
        ClientResult::Success);
    auto p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::CreateAccessPoint, sizeof(req) + sizeof(adv));
    ASSERT_EQ(p[sizeof(LdnHeader) + sizeof(req)], static_cast<uint8_t>(0xAA));

    // No advertise data: request only.
    ASSERT_EQ(
        client.send_create_access_point(req, nullptr, 0),
        ClientResult::Success);
    p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::CreateAccessPoint, sizeof(req));
}

/**
 * @brief Advertise data too large for the packet buffer is refused with
 * BufferTooSmall.
 */
TEST(connected_send_create_access_point_rejects_oversized_advertise_data) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    CreateAccessPointRequest req{};
    std::vector<uint8_t> adv(4096, 1);
    ASSERT_EQ(
        client.send_create_access_point(req, adv.data(), adv.size()),
        ClientResult::BufferTooSmall);
}

/**
 * @brief The private variant also appends advertise data, sends the bare
 * request without it, and refuses oversized data with BufferTooSmall.
 */
TEST(connected_send_create_access_point_private_appends_advertise_data) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    CreateAccessPointPrivateRequest req{};
    const uint8_t adv[16] = {0xCC};

    ASSERT_EQ(
        client.send_create_access_point_private(req, adv, sizeof(adv)),
        ClientResult::Success);
    auto p = peer.read_packet();
    ASSERT_WIRE(
        p, PacketId::CreateAccessPointPrivate, sizeof(req) + sizeof(adv));
    ASSERT_EQ(p[sizeof(LdnHeader) + sizeof(req)], static_cast<uint8_t>(0xCC));

    ASSERT_EQ(
        client.send_create_access_point_private(req, nullptr, 0),
        ClientResult::Success);
    p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::CreateAccessPointPrivate, sizeof(req));

    std::vector<uint8_t> big(4096, 1);
    ASSERT_EQ(
        client.send_create_access_point_private(req, big.data(), big.size()),
        ClientResult::BufferTooSmall);
}

/**
 * @brief send_proxy_data() sends the ProxyDataHeader followed by the
 * payload, and returns EncodingError when the payload is too large.
 */
TEST(connected_send_proxy_data_encodes_header_and_payload) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    ProxyDataHeader header{};
    header.info.source_port = 1234;
    const uint8_t data[6] = {1, 2, 3, 4, 5, 6};
    header.data_length = sizeof(data);

    ASSERT_EQ(
        client.send_proxy_data(header, data, sizeof(data)),
        ClientResult::Success);
    auto p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::ProxyData, sizeof(ProxyDataHeader) + sizeof(data));

    std::vector<uint8_t> big(4096, 1);
    ASSERT_EQ(
        client.send_proxy_data(header, big.data(), big.size()),
        ClientResult::EncodingError);
}

/**
 * @brief send_set_advertise_data() accepts empty and small data, and clamps
 * larger data to the 384-byte protocol maximum instead of failing.
 */
TEST(connected_send_set_advertise_data_clamps_to_protocol_maximum) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);

    const uint8_t small[4] = {1, 2, 3, 4};
    ASSERT_EQ(
        client.send_set_advertise_data(small, sizeof(small)),
        ClientResult::Success);
    auto p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::SetAdvertiseData, sizeof(small));

    ASSERT_EQ(
        client.send_set_advertise_data(nullptr, 0), ClientResult::Success);
    p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::SetAdvertiseData, static_cast<size_t>(0));

    std::vector<uint8_t> big(1000, 7);
    ASSERT_EQ(
        client.send_set_advertise_data(big.data(), big.size()),
        ClientResult::Success);
    p = peer.read_packet();
    ASSERT_WIRE(p, PacketId::SetAdvertiseData, static_cast<size_t>(384));
}

/**
 * @brief After the peer closes, sends eventually fail (the kernel first
 * buffers, then the RST surfaces) and keep failing afterwards.
 */
TEST(connected_send_fails_once_the_peer_is_gone) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    peer.close_connection();

    // The first writes after a FIN may still be accepted by the kernel; the RST
    // that follows must surface as a non-success result within a few sends.
    std::vector<uint8_t> chunk(1400, 0x11);
    ClientResult last = ClientResult::Success;
    for (int i = 0; i < 200 && last == ClientResult::Success; i++) {
        last = client.send_raw(chunk.data(), chunk.size());
    }
    ASSERT_NE(last, ClientResult::Success);

    // The failure is sticky: a later send on the same dead connection fails
    // too.
    uint8_t one = 1;
    ASSERT_NE(
        client.send_packet(PacketId::Ping, &one, 1), ClientResult::Success);
}

/**
 * @brief receive_packet() returns the packet type and payload, and the
 * packet is consumed so no further packet is available.
 */
TEST(connected_receive_packet_returns_payload_and_consumes_it) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    const uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    auto pkt = make_packet(PacketId::Ping, payload, sizeof(payload));
    ASSERT_TRUE(peer.write_bytes(pkt.data(), pkt.size()));

    PacketId type{};
    uint8_t out[16] = {};
    size_t out_size = 0;
    ASSERT_EQ(
        client.receive_packet(type, out, sizeof(out), out_size, 2000),
        ClientResult::Success);
    ASSERT_EQ(type, PacketId::Ping);
    ASSERT_EQ(out_size, sizeof(payload));
    ASSERT_EQ(std::memcmp(out, payload, sizeof(payload)), 0);
    ASSERT_FALSE(client.has_packet_available());
}

/// @brief A packet with an empty payload yields Success with payload size 0.
TEST(connected_receive_packet_without_payload) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    auto pkt = make_packet(PacketId::ScanReplyEnd, nullptr, 0);
    ASSERT_TRUE(peer.write_bytes(pkt.data(), pkt.size()));

    PacketId type{};
    uint8_t out[4];
    size_t out_size = 99;
    ASSERT_EQ(
        client.receive_packet(type, out, sizeof(out), out_size, 2000),
        ClientResult::Success);
    ASSERT_EQ(type, PacketId::ScanReplyEnd);
    ASSERT_EQ(out_size, static_cast<size_t>(0));
}

/**
 * @brief A packet delivered in two TCP writes (header cut short) is
 * reassembled and returned whole.
 */
TEST(connected_receive_packet_reassembles_a_split_packet) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto pkt = make_packet(PacketId::Ping, payload, sizeof(payload));
    // Header alone first: the client must wait for the rest.
    ASSERT_TRUE(peer.write_bytes(pkt.data(), 5));
    ASSERT_TRUE(peer.write_bytes(pkt.data() + 5, pkt.size() - 5));

    PacketId type{};
    uint8_t out[16] = {};
    size_t out_size = 0;
    ASSERT_EQ(
        client.receive_packet(type, out, sizeof(out), out_size, 2000),
        ClientResult::Success);
    ASSERT_EQ(out_size, sizeof(payload));
    ASSERT_EQ(std::memcmp(out, payload, sizeof(payload)), 0);
}

/**
 * @brief With a too-small output buffer receive_packet() returns
 * BufferTooSmall, reports the needed size and keeps the packet so a retry
 * with a larger buffer succeeds.
 */
TEST(connected_receive_packet_keeps_the_packet_when_the_buffer_is_too_small) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto pkt = make_packet(PacketId::Ping, payload, sizeof(payload));
    ASSERT_TRUE(peer.write_bytes(pkt.data(), pkt.size()));

    PacketId type{};
    uint8_t small[2];
    size_t out_size = 0;
    ASSERT_EQ(
        client.receive_packet(type, small, sizeof(small), out_size, 2000),
        ClientResult::BufferTooSmall);
    ASSERT_EQ(out_size, sizeof(payload));
    ASSERT_TRUE(client.has_packet_available());

    uint8_t big[16] = {};
    ASSERT_EQ(
        client.receive_packet(type, big, sizeof(big), out_size, 2000),
        ClientResult::Success);
    ASSERT_EQ(std::memcmp(big, payload, sizeof(payload)), 0);
}

/**
 * @brief Without incoming data receive_packet() returns Timeout, both with a
 * finite timeout and with a zero (poll) timeout.
 */
TEST(connected_receive_packet_times_out_without_data) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    PacketId type{};
    uint8_t out[4];
    size_t out_size = 0;
    ASSERT_EQ(
        client.receive_packet(type, out, sizeof(out), out_size, 50),
        ClientResult::Timeout);
    ASSERT_EQ(
        client.receive_packet(type, out, sizeof(out), out_size, 0),
        ClientResult::Timeout);
}

/// @brief An orderly close by the peer is reported as ConnectionLost.
TEST(connected_receive_packet_reports_connection_lost_on_peer_close) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    peer.close_connection();
    PacketId type{};
    uint8_t out[4];
    size_t out_size = 0;
    ASSERT_EQ(
        client.receive_packet(type, out, sizeof(out), out_size, 2000),
        ClientResult::ConnectionLost);
}

/**
 * @brief A header with an invalid magic is never treated as complete: the
 * client keeps waiting and times out instead of returning garbage.
 */
TEST(connected_receive_packet_rejects_a_corrupt_header) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    uint8_t garbage[sizeof(LdnHeader)];
    std::memset(garbage, 0xEE, sizeof(garbage));
    ASSERT_TRUE(peer.write_bytes(garbage, sizeof(garbage)));
    PacketId type{};
    uint8_t out[4];
    size_t out_size = 0;
    // A header with an invalid magic is never "complete": the client keeps
    // waiting, then reports a timeout rather than handing back garbage.
    ASSERT_EQ(
        client.receive_packet(type, out, sizeof(out), out_size, 100),
        ClientResult::Timeout);
}

/**
 * @brief A valid header announcing more payload than the stream buffer can
 * hold, followed by filler, ends in InvalidPacket instead of overflowing.
 */
TEST(connected_receive_packet_overflowing_the_stream_buffer_is_invalid) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);

    // A valid header announcing a payload larger than the 8 KB stream buffer,
    // followed by more bytes than the buffer can ever hold.
    LdnHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint8_t>(PacketId::Ping);
    header.data_size = 60000;
    ASSERT_TRUE(peer.write_bytes(&header, sizeof(header)));
    std::vector<uint8_t> filler(4096, 0x33);

    PacketId type{};
    uint8_t out[4];
    size_t out_size = 0;
    ClientResult result = ClientResult::Success;
    for (int i = 0; i < 8 && result != ClientResult::InvalidPacket; i++) {
        ASSERT_TRUE(peer.write_bytes(filler.data(), filler.size()));
        result = client.receive_packet(type, out, sizeof(out), out_size, 200);
    }
    ASSERT_EQ(result, ClientResult::InvalidPacket);
}

/// @brief A connection reset (RST) by the peer is reported as ConnectionLost.
TEST(connected_receive_packet_maps_a_connection_reset) {
    Peer peer;
    TcpClient client;
    CONNECT_PAIR(client, peer);
    peer.reset_connection();
    PacketId type{};
    uint8_t out[4];
    size_t out_size = 0;
    ASSERT_EQ(
        client.receive_packet(type, out, sizeof(out), out_size, 2000),
        ClientResult::ConnectionLost);
}

/**
 * @brief When the listener's accept queue is full and never drained, the
 * next connect() ends in Timeout rather than hanging.
 */
TEST(connect_times_out_when_the_server_never_accepts) {
    socket_init();
    // Listener with a one-slot accept queue that nobody drains: once it is
    // full the kernel drops further SYNs, so the next connect cannot finish.
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(::listen(fd, 0), 0);
    socklen_t len = sizeof(addr);
    ASSERT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    const uint16_t port = ntohs(addr.sin_port);

    TcpClient fillers[3];
    ClientResult last = ClientResult::Success;
    for (auto& filler : fillers) {
        last = filler.connect("127.0.0.1", port, 300);
        if (last != ClientResult::Success) {
            break;
        }
    }
    ::close(fd);
    ASSERT_EQ(last, ClientResult::Timeout);
}

/**
 * @brief TcpClient::socket_to_client_result() maps every SocketResult to the
 * intended ClientResult (WouldBlock counts as Success).
 */
TEST(socket_result_mapping_covers_every_socket_outcome) {
    struct Row {
        SocketResult in;
        ClientResult out;
    };
    const Row rows[] = {
        {.in = SocketResult::Success, .out = ClientResult::Success},
        {.in = SocketResult::WouldBlock, .out = ClientResult::Success},
        {.in = SocketResult::Timeout, .out = ClientResult::Timeout},
        {.in = SocketResult::NotConnected, .out = ClientResult::NotConnected},
        {.in = SocketResult::AlreadyConnected,
         .out = ClientResult::AlreadyConnected},
        {.in = SocketResult::NotInitialized,
         .out = ClientResult::NotInitialized},
        {.in = SocketResult::Closed, .out = ClientResult::ConnectionLost},
        {.in = SocketResult::ConnectionReset,
         .out = ClientResult::ConnectionLost},
        {.in = SocketResult::ConnectionRefused,
         .out = ClientResult::ConnectionFailed},
        {.in = SocketResult::HostUnreachable,
         .out = ClientResult::ConnectionFailed},
        {.in = SocketResult::NetworkDown,
         .out = ClientResult::ConnectionFailed},
        {.in = SocketResult::InvalidAddress,
         .out = ClientResult::ConnectionFailed},
        {.in = SocketResult::SocketError, .out = ClientResult::InternalError},
    };
    for (const Row& row : rows) {
        ASSERT_EQ(TcpClient::socket_to_client_result(row.in), row.out);
    }
}

// =============================================================================
// Main
// =============================================================================

/**
 * @brief Print the banner and the totals (the tests already ran during
 * static initialization) and close the socket layer.
 * @return 1 when at least one test failed, 0 otherwise.
 */
int main() {
    printf("\n");
    printf("========================================\n");
    printf("  TcpClient Tests - ryu_ldn_nx\n");
    printf("========================================\n\n");

    // Tests run automatically via static initialization

    printf("\n========================================\n");
    printf("  Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) {
        printf(" (%d FAILED)", g_tests_failed);
    }
    printf("\n========================================\n\n");

    // Cleanup
    socket_exit();

    return g_tests_failed > 0 ? 1 : 0;
}
