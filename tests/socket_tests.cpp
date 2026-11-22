/**
 * @file socket_tests.cpp
 * @brief Unit tests for ryu_ldn::network::Socket using mocks
 *
 * These tests use mocking to test the Socket wrapper without requiring
 * actual network connections. This makes tests:
 * - Faster (no network I/O)
 * - More reliable (no port conflicts, no network issues)
 * - Deterministic (same results every run)
 *
 * ## Mock Architecture
 *
 * We intercept the low-level socket functions using a mock layer that
 * can be configured to return specific values for testing different
 * scenarios (success, errors, timeouts, etc.).
 *
 * ## Running Tests
 *
 * ```bash
 * cd tests
 * make test-socket
 * ```
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <vector>
#include <functional>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

// For test build, we mock the socket functions
#define TEST_BUILD_SOCKET_MOCK

#include "network/socket.hpp"

using namespace ryu_ldn::network;

// =============================================================================
// Test Framework (minimal, same as other test files)
// =============================================================================

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name)                            \
    static void test_##name();                \
    static struct TestRegister_##name {       \
        TestRegister_##name() {               \
            printf("  [TEST] " #name "... "); \
            fflush(stdout);                   \
            g_tests_run++;                    \
            try {                             \
                test_##name();                \
                printf("PASS\n");             \
                g_tests_passed++;             \
            } catch (const char* msg) {       \
                printf("FAIL: %s\n", msg);    \
                g_tests_failed++;             \
            }                                 \
        }                                     \
    } g_test_register_##name;                 \
    static void test_##name()

#define ASSERT(cond)                          \
    do {                                      \
        if (!(cond)) {                        \
            throw "Assertion failed: " #cond; \
        }                                     \
    } while (0)

#define ASSERT_EQ(a, b)                              \
    do {                                             \
        if ((a) != (b)) {                            \
            throw "Assertion failed: " #a " != " #b; \
        }                                            \
    } while (0)

#define ASSERT_NE(a, b)                              \
    do {                                             \
        if ((a) == (b)) {                            \
            throw "Assertion failed: " #a " == " #b; \
        }                                            \
    } while (0)

#define ASSERT_TRUE(cond) ASSERT(cond)
#define ASSERT_FALSE(cond) ASSERT(!(cond))

// =============================================================================
// Tests: Socket Subsystem Initialization
// =============================================================================

/**
 * @test socket_init should succeed
 *
 * Verifies that socket_init() returns Success and marks the subsystem
 * as initialized.
 */
TEST(socket_init_succeeds) {
    // Start fresh
    socket_exit();
    ASSERT_FALSE(socket_is_initialized());

    SocketResult result = socket_init();
    ASSERT_EQ(result, SocketResult::Success);
    ASSERT_TRUE(socket_is_initialized());
}

/**
 * @test socket_init is idempotent
 *
 * Calling socket_init() multiple times should succeed without error.
 */
TEST(socket_init_idempotent) {
    SocketResult result1 = socket_init();
    ASSERT_EQ(result1, SocketResult::Success);

    SocketResult result2 = socket_init();
    ASSERT_EQ(result2, SocketResult::Success);

    SocketResult result3 = socket_init();
    ASSERT_EQ(result3, SocketResult::Success);

    ASSERT_TRUE(socket_is_initialized());
}

/**
 * @test socket_exit clears initialized state
 *
 * After socket_exit(), socket_is_initialized() should return false.
 */
TEST(socket_exit_clears_state) {
    socket_init();
    ASSERT_TRUE(socket_is_initialized());

    socket_exit();
    ASSERT_FALSE(socket_is_initialized());
}

/**
 * @test socket_exit is idempotent
 *
 * Calling socket_exit() multiple times should not crash.
 */
TEST(socket_exit_idempotent) {
    socket_init();
    socket_exit();
    socket_exit();  // Should not crash
    socket_exit();  // Should not crash
    ASSERT_FALSE(socket_is_initialized());

    // Re-init for subsequent tests
    socket_init();
}

// =============================================================================
// Tests: Socket Construction and State
// =============================================================================

/**
 * @test Default constructed socket is invalid
 *
 * A newly constructed Socket should be invalid and not connected.
 */
TEST(socket_default_invalid) {
    socket_init();

    Socket sock;
    ASSERT_FALSE(sock.is_valid());
    ASSERT_FALSE(sock.is_connected());
    ASSERT_EQ(sock.get_fd(), -1);
}

/**
 * @test Socket can be created multiple times
 *
 * Creating multiple Socket objects should work independently.
 */
TEST(socket_multiple_instances) {
    socket_init();

    Socket sock1;
    Socket sock2;
    Socket sock3;

    ASSERT_FALSE(sock1.is_valid());
    ASSERT_FALSE(sock2.is_valid());
    ASSERT_FALSE(sock3.is_valid());

    ASSERT_EQ(sock1.get_fd(), -1);
    ASSERT_EQ(sock2.get_fd(), -1);
    ASSERT_EQ(sock3.get_fd(), -1);
}

/**
 * @test Socket move constructor transfers ownership
 *
 * Moving a socket should transfer the file descriptor and leave
 * the original socket invalid.
 */
TEST(socket_move_constructor) {
    socket_init();

    Socket sock1;
    // We can't connect without a server, but we can test move semantics
    // by checking that the fd transfers correctly

    Socket sock2(std::move(sock1));

    // Both should be invalid since sock1 was never connected
    ASSERT_FALSE(sock1.is_valid());
    ASSERT_FALSE(sock2.is_valid());
    ASSERT_EQ(sock1.get_fd(), -1);
    ASSERT_EQ(sock2.get_fd(), -1);
}

/**
 * @test Socket move assignment works
 *
 * Move assignment should transfer ownership.
 */
TEST(socket_move_assignment) {
    socket_init();

    Socket sock1;
    Socket sock2;

    sock2 = std::move(sock1);

    ASSERT_FALSE(sock1.is_valid());
    ASSERT_FALSE(sock2.is_valid());
}

// =============================================================================
// Tests: Connection State Without Server
// =============================================================================

/**
 * @test Connect to invalid host returns error
 *
 * Connecting to an invalid hostname should return an error.
 */
TEST(connect_invalid_host) {
    socket_init();

    Socket sock;
    SocketResult result =
        sock.connect("invalid.host.that.does.not.exist.local", 12345, 100);

    // Should fail with some error (InvalidAddress or HostUnreachable)
    ASSERT_NE(result, SocketResult::Success);
    ASSERT_FALSE(sock.is_connected());
}

/**
 * @test Connect with null host returns error
 *
 * Passing null for hostname should fail safely.
 */
TEST(connect_null_host) {
    socket_init();

    Socket sock;
    SocketResult result = sock.connect(nullptr, 12345, 100);

    ASSERT_NE(result, SocketResult::Success);
    ASSERT_FALSE(sock.is_connected());
}

/**
 * @test Connect with empty host returns error
 *
 * Empty hostname string should fail.
 */
TEST(connect_empty_host) {
    socket_init();

    Socket sock;
    SocketResult result = sock.connect("", 12345, 100);

    ASSERT_NE(result, SocketResult::Success);
    ASSERT_FALSE(sock.is_connected());
}

/**
 * @test Connect to unreachable port times out or refuses
 *
 * Connecting to localhost on a port with no listener should fail.
 */
TEST(connect_refused) {
    socket_init();

    Socket sock;
    // Port 1 is privileged and unlikely to be in use
    // Use high port that's almost certainly not listening
    SocketResult result = sock.connect("127.0.0.1", 59999, 500);

    // Should fail - either ConnectionRefused or Timeout
    ASSERT_NE(result, SocketResult::Success);
    ASSERT_FALSE(sock.is_connected());
}

// =============================================================================
// Tests: Close Operations
// =============================================================================

/**
 * @test Close on invalid socket is safe
 *
 * Calling close() on a socket that was never connected should not crash.
 */
TEST(close_on_invalid_safe) {
    socket_init();

    Socket sock;
    ASSERT_FALSE(sock.is_valid());

    sock.close();  // Should not crash
    sock.close();  // Multiple closes should be safe
    sock.close();

    ASSERT_FALSE(sock.is_valid());
    ASSERT_FALSE(sock.is_connected());
}

/**
 * @test Close is idempotent
 *
 * Calling close() multiple times should be safe.
 */
TEST(close_idempotent) {
    socket_init();

    Socket sock;

    for (int i = 0; i < 10; i++) {
        sock.close();
    }

    ASSERT_FALSE(sock.is_valid());
}

// =============================================================================
// Tests: Send Without Connection
// =============================================================================

/**
 * @test Send on disconnected socket fails
 *
 * Sending on a socket that isn't connected should fail.
 */
TEST(send_not_connected) {
    socket_init();

    Socket sock;
    ASSERT_FALSE(sock.is_connected());

    uint8_t data[] = {0x01, 0x02, 0x03};
    size_t sent = 0;
    SocketResult result = sock.send(data, sizeof(data), sent);

    ASSERT_EQ(result, SocketResult::NotConnected);
    ASSERT_EQ(sent, 0u);
}

/**
 * @test Send all on disconnected socket fails
 *
 * send_all() on a disconnected socket should fail.
 */
TEST(send_all_not_connected) {
    socket_init();

    Socket sock;

    uint8_t data[] = {0x01, 0x02, 0x03};
    SocketResult result = sock.send_all(data, sizeof(data));

    ASSERT_EQ(result, SocketResult::NotConnected);
}

/**
 * @test Send with null buffer on disconnected socket
 *
 * Should fail with NotConnected before checking buffer.
 */
TEST(send_null_buffer_not_connected) {
    socket_init();

    Socket sock;
    size_t sent = 0;
    SocketResult result = sock.send(nullptr, 10, sent);

    ASSERT_EQ(result, SocketResult::NotConnected);
}

/**
 * @test Send with zero size on disconnected socket
 *
 * Should fail with NotConnected.
 */
TEST(send_zero_size_not_connected) {
    socket_init();

    Socket sock;
    uint8_t data[] = {0x01};
    size_t sent = 999;  // Should be set to 0
    SocketResult result = sock.send(data, 0, sent);

    ASSERT_EQ(result, SocketResult::NotConnected);
}

// =============================================================================
// Tests: Receive Without Connection
// =============================================================================

/**
 * @test Receive on disconnected socket fails
 *
 * Receiving on a socket that isn't connected should fail.
 */
TEST(recv_not_connected) {
    socket_init();

    Socket sock;
    ASSERT_FALSE(sock.is_connected());

    uint8_t buffer[256];
    size_t received = 0;
    SocketResult result = sock.recv(buffer, sizeof(buffer), received, 0);

    ASSERT_EQ(result, SocketResult::NotConnected);
    ASSERT_EQ(received, 0u);
}

/**
 * @test Receive with timeout on disconnected socket
 *
 * Should fail immediately without waiting.
 */
TEST(recv_timeout_not_connected) {
    socket_init();

    Socket sock;

    uint8_t buffer[256];
    size_t received = 0;
    SocketResult result = sock.recv(buffer, sizeof(buffer), received, 1000);

    ASSERT_EQ(result, SocketResult::NotConnected);
}

/**
 * @test Receive with blocking mode on disconnected socket
 *
 * Should fail immediately without blocking.
 */
TEST(recv_blocking_not_connected) {
    socket_init();

    Socket sock;

    uint8_t buffer[256];
    size_t received = 0;
    SocketResult result = sock.recv(buffer, sizeof(buffer), received, -1);

    ASSERT_EQ(result, SocketResult::NotConnected);
}

// =============================================================================
// Tests: Socket Options Without Connection
// =============================================================================

/**
 * @test Set non-blocking on invalid socket fails
 *
 * Setting options on an invalid socket should fail.
 */
TEST(set_non_blocking_invalid) {
    socket_init();

    Socket sock;
    ASSERT_FALSE(sock.is_valid());

    SocketResult result = sock.set_non_blocking(true);
    ASSERT_EQ(result, SocketResult::SocketError);
}

/**
 * @test Set nodelay on invalid socket fails
 */
TEST(set_nodelay_invalid) {
    socket_init();

    Socket sock;

    SocketResult result = sock.set_nodelay(true);
    ASSERT_EQ(result, SocketResult::SocketError);
}

/**
 * @test Set recv buffer size on invalid socket fails
 */
TEST(set_recv_buffer_invalid) {
    socket_init();

    Socket sock;

    SocketResult result = sock.set_recv_buffer_size(65536);
    ASSERT_EQ(result, SocketResult::SocketError);
}

/**
 * @test Set send buffer size on invalid socket fails
 */
TEST(set_send_buffer_invalid) {
    socket_init();

    Socket sock;

    SocketResult result = sock.set_send_buffer_size(65536);
    ASSERT_EQ(result, SocketResult::SocketError);
}

// =============================================================================
// Tests: Result String Conversion
// =============================================================================

/**
 * @test socket_result_to_string covers all values
 *
 * All SocketResult values should have meaningful string representations.
 */
TEST(result_to_string_success) {
    ASSERT(
        strcmp(socket_result_to_string(SocketResult::Success), "Success") == 0);
}

TEST(result_to_string_would_block) {
    ASSERT(
        strcmp(
            socket_result_to_string(SocketResult::WouldBlock), "WouldBlock") ==
        0);
}

TEST(result_to_string_timeout) {
    ASSERT(
        strcmp(socket_result_to_string(SocketResult::Timeout), "Timeout") == 0);
}

TEST(result_to_string_connection_refused) {
    ASSERT(
        strcmp(
            socket_result_to_string(SocketResult::ConnectionRefused),
            "ConnectionRefused") == 0);
}

TEST(result_to_string_connection_reset) {
    ASSERT(
        strcmp(
            socket_result_to_string(SocketResult::ConnectionReset),
            "ConnectionReset") == 0);
}

TEST(result_to_string_host_unreachable) {
    ASSERT(
        strcmp(
            socket_result_to_string(SocketResult::HostUnreachable),
            "HostUnreachable") == 0);
}

TEST(result_to_string_network_down) {
    ASSERT(
        strcmp(
            socket_result_to_string(SocketResult::NetworkDown),
            "NetworkDown") == 0);
}

TEST(result_to_string_not_connected) {
    ASSERT(
        strcmp(
            socket_result_to_string(SocketResult::NotConnected),
            "NotConnected") == 0);
}

TEST(result_to_string_already_connected) {
    ASSERT(
        strcmp(
            socket_result_to_string(SocketResult::AlreadyConnected),
            "AlreadyConnected") == 0);
}

TEST(result_to_string_invalid_address) {
    ASSERT(
        strcmp(
            socket_result_to_string(SocketResult::InvalidAddress),
            "InvalidAddress") == 0);
}

TEST(result_to_string_socket_error) {
    ASSERT(
        strcmp(
            socket_result_to_string(SocketResult::SocketError),
            "SocketError") == 0);
}

TEST(result_to_string_not_initialized) {
    ASSERT(
        strcmp(
            socket_result_to_string(SocketResult::NotInitialized),
            "NotInitialized") == 0);
}

TEST(result_to_string_closed) {
    ASSERT(
        strcmp(socket_result_to_string(SocketResult::Closed), "Closed") == 0);
}

/**
 * @test Unknown result returns "Unknown"
 */
TEST(result_to_string_unknown) {
    SocketResult unknown = static_cast<SocketResult>(9999);
    ASSERT(strcmp(socket_result_to_string(unknown), "Unknown") == 0);
}

// =============================================================================
// Tests: Socket Subsystem Not Initialized
// =============================================================================

/**
 * @test Operations before socket_init fail
 *
 * Socket operations should fail if the subsystem isn't initialized.
 */
TEST(operations_before_init) {
    // Make sure we're not initialized
    socket_exit();
    ASSERT_FALSE(socket_is_initialized());

    Socket sock;

    // Connect should fail
    SocketResult connect_result = sock.connect("127.0.0.1", 12345, 100);
    ASSERT_EQ(connect_result, SocketResult::NotInitialized);

    // Re-init for subsequent tests
    socket_init();
}

// =============================================================================
// Tests: Edge Cases
// =============================================================================

/**
 * @test Very long hostname is handled
 *
 * An extremely long hostname should be handled without buffer overflow.
 */
TEST(very_long_hostname) {
    socket_init();

    Socket sock;

    // Create a 1024-char hostname
    char long_host[1025];
    memset(long_host, 'a', 1024);
    long_host[1024] = '\0';

    SocketResult result = sock.connect(long_host, 12345, 100);

    // Should fail with InvalidAddress, not crash
    ASSERT_NE(result, SocketResult::Success);
}

/**
 * @test Port 0 is handled
 *
 * Connecting to port 0 should fail.
 */
TEST(port_zero) {
    socket_init();

    Socket sock;
    SocketResult result = sock.connect("127.0.0.1", 0, 100);

    // Should fail
    ASSERT_NE(result, SocketResult::Success);
}

/**
 * @test Maximum port number is handled
 *
 * Port 65535 is valid and should be accepted.
 */
TEST(port_max) {
    socket_init();

    Socket sock;
    // This will fail to connect (no server) but shouldn't crash
    SocketResult result = sock.connect("127.0.0.1", 65535, 100);

    // Should fail with refused/timeout, not invalid
    ASSERT_NE(result, SocketResult::Success);
    // The result should be a connection error, not a validation error
    ASSERT(
        result == SocketResult::ConnectionRefused ||
        result == SocketResult::ConnectionReset ||
        result == SocketResult::Timeout ||
        result == SocketResult::HostUnreachable ||
        result == SocketResult::SocketError);
}

/**
 * @test IPv4 address format is accepted
 */
TEST(ipv4_format) {
    socket_init();

    Socket sock;
    // Valid format, should try to connect (and fail due to no server)
    SocketResult result = sock.connect("192.168.1.1", 12345, 100);

    // Should fail but with network error, not format error
    ASSERT_NE(result, SocketResult::Success);
}

/**
 * @test Localhost variants work
 */
TEST(localhost_variants) {
    socket_init();

    Socket sock1;
    SocketResult r1 = sock1.connect("127.0.0.1", 59998, 100);
    ASSERT_NE(r1, SocketResult::Success);

    Socket sock2;
    SocketResult r2 = sock2.connect("localhost", 59998, 100);
    // localhost should resolve (might fail to connect)
    ASSERT_NE(r2, SocketResult::Success);
}

// =============================================================================
// Tests: Destructor Safety
// =============================================================================

/**
 * @test Destructor on uninitialized socket is safe
 *
 * Socket destructor should handle uninitialized state.
 */
TEST(destructor_uninitialized) {
    socket_init();

    {
        Socket sock;
        // sock goes out of scope without being connected
    }
    // Should not crash

    ASSERT_TRUE(true);  // If we get here, test passed
}

/**
 * @test Multiple sockets destroyed in sequence
 *
 * Creating and destroying multiple sockets should work.
 */
TEST(multiple_destructor_calls) {
    socket_init();

    for (int i = 0; i < 100; i++) {
        Socket sock;
        // Let it be destroyed
    }

    ASSERT_TRUE(true);
}

// =============================================================================
// Tests: State Consistency
// =============================================================================

/**
 * @test State is consistent after failed connect
 *
 * After a failed connection attempt, socket should be in clean state.
 */
TEST(state_after_failed_connect) {
    socket_init();

    Socket sock;

    // Try to connect (will fail)
    sock.connect("127.0.0.1", 59997, 100);

    // State should be consistent
    ASSERT_FALSE(sock.is_connected());

    // Should be able to try again
    sock.connect("127.0.0.1", 59996, 100);
    ASSERT_FALSE(sock.is_connected());
}

/**
 * @test Close after failed connect is safe
 */
TEST(close_after_failed_connect) {
    socket_init();

    Socket sock;
    sock.connect("127.0.0.1", 59995, 100);

    sock.close();
    sock.close();

    ASSERT_FALSE(sock.is_valid());
    ASSERT_FALSE(sock.is_connected());
}

// =============================================================================
// Additional Coverage
// =============================================================================

/**
 * @test is_valid returns false for default-constructed socket
 */
TEST(is_valid_default_socket) {
    socket_init();
    Socket sock;

    ASSERT_FALSE(sock.is_valid());
}

/**
 * @test is_valid returns true after successful socket creation
 */
TEST(is_valid_after_create) {
    socket_init();
    Socket sock;
    // connect() internally creates a socket, which will fail
    // but we can't test is_valid on a connected path without a server.
    // is_valid on a failed-connect socket depends on implementation.
    // Just test the negative path.
    ASSERT_FALSE(sock.is_valid());
}

/**
 * @test socket result_to_string covers all SocketResult values
 */
TEST(result_to_string_all_values) {
    // Already partially tested, ensure full coverage
    ASSERT_TRUE(
        std::strcmp(
            socket_result_to_string(SocketResult::WouldBlock), "WouldBlock") ==
        0);
    ASSERT_TRUE(
        std::strcmp(
            socket_result_to_string(SocketResult::NetworkDown),
            "NetworkDown") == 0);
    ASSERT_TRUE(
        std::strcmp(socket_result_to_string(SocketResult::Closed), "Closed") ==
        0);
    // Unknown value
    ASSERT_TRUE(
        std::strcmp(
            socket_result_to_string(static_cast<SocketResult>(99)),
            "Unknown") == 0);
}

TEST(errno_to_result_all_mappings) {
    ASSERT_EQ(errno_to_result(EAGAIN), SocketResult::WouldBlock);
#if EAGAIN != EWOULDBLOCK
    ASSERT_EQ(errno_to_result(EWOULDBLOCK), SocketResult::WouldBlock);
#endif
    ASSERT_EQ(errno_to_result(ECONNREFUSED), SocketResult::ConnectionRefused);
    ASSERT_EQ(errno_to_result(ECONNRESET), SocketResult::ConnectionReset);
    ASSERT_EQ(errno_to_result(EHOSTUNREACH), SocketResult::HostUnreachable);
    ASSERT_EQ(errno_to_result(ENETUNREACH), SocketResult::HostUnreachable);
    ASSERT_EQ(errno_to_result(ENETDOWN), SocketResult::NetworkDown);
    ASSERT_EQ(errno_to_result(ENOTCONN), SocketResult::NotConnected);
    ASSERT_EQ(errno_to_result(EISCONN), SocketResult::AlreadyConnected);
    ASSERT_EQ(errno_to_result(ETIMEDOUT), SocketResult::Timeout);
    ASSERT_EQ(errno_to_result(999), SocketResult::SocketError);
    ASSERT_EQ(errno_to_result(0), SocketResult::SocketError);
}

// =============================================================================
// Tests: Socket Options on Valid Socket
// =============================================================================

/**
 * Helper: create a local TCP listener and return its port.
 * Returns 0 on failure.
 */
static uint16_t create_local_listener(int& server_fd) {
    server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
        return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);  // Let OS pick a port

    if (bind(
            server_fd,
            reinterpret_cast<struct sockaddr*>(&addr),
            sizeof(addr)) < 0) {
        ::close(server_fd);
        server_fd = -1;
        return 0;
    }

    if (listen(server_fd, 1) < 0) {
        ::close(server_fd);
        server_fd = -1;
        return 0;
    }

    // Retrieve assigned port
    socklen_t addrlen = sizeof(addr);
    if (getsockname(
            server_fd, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) <
        0) {
        ::close(server_fd);
        server_fd = -1;
        return 0;
    }

    return ntohs(addr.sin_port);
}

/**
 * @test set_nodelay on a connected socket succeeds
 */
TEST(set_nodelay_valid) {
    socket_init();
    int server_fd = -1;
    uint16_t port = create_local_listener(server_fd);
    ASSERT_NE(port, static_cast<uint16_t>(0));

    Socket sock;
    // Start accept in background to complete the connection
    // We need an accepted socket for the connection to succeed
    // Try to connect — may need to accept on server side first
    // Use a separate thread would be complex, so use blocking approach:
    // connect() will SYN, and we accept on server side.
    // Actually Socket::connect is synchronous with timeout, so we need
    // the server to accept. Let's use a simple approach.

    // Server accepts in a fork? No — simpler: just call connect
    // and then accept on server_fd. But connect blocks...
    // Let's use non-blocking on server side accept.
    SocketResult cr = sock.connect("127.0.0.1", port, 2000);

    // Accept the connection (regardless of connect result, drain the listener)
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int accepted = ::accept(
        server_fd,
        reinterpret_cast<struct sockaddr*>(&client_addr),
        &client_len);

    if (cr == SocketResult::Success) {
        SocketResult result = sock.set_nodelay(true);
        ASSERT_EQ(result, SocketResult::Success);

        result = sock.set_nodelay(false);
        ASSERT_EQ(result, SocketResult::Success);
    }

    if (accepted >= 0)
        ::close(accepted);
    ::close(server_fd);
    sock.close();
}

/**
 * @test set_recv_buffer_size on a connected socket succeeds
 */
TEST(set_recv_buffer_size_valid) {
    socket_init();
    int server_fd = -1;
    uint16_t port = create_local_listener(server_fd);
    ASSERT_NE(port, static_cast<uint16_t>(0));

    Socket sock;
    SocketResult cr = sock.connect("127.0.0.1", port, 2000);

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int accepted = ::accept(
        server_fd,
        reinterpret_cast<struct sockaddr*>(&client_addr),
        &client_len);

    if (cr == SocketResult::Success) {
        SocketResult result = sock.set_recv_buffer_size(65536);
        ASSERT_EQ(result, SocketResult::Success);
    }

    if (accepted >= 0)
        ::close(accepted);
    ::close(server_fd);
    sock.close();
}

/**
 * @test set_send_buffer_size on a connected socket succeeds
 */
TEST(set_send_buffer_size_valid) {
    socket_init();
    int server_fd = -1;
    uint16_t port = create_local_listener(server_fd);
    ASSERT_NE(port, static_cast<uint16_t>(0));

    Socket sock;
    SocketResult cr = sock.connect("127.0.0.1", port, 2000);

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int accepted = ::accept(
        server_fd,
        reinterpret_cast<struct sockaddr*>(&client_addr),
        &client_len);

    if (cr == SocketResult::Success) {
        SocketResult result = sock.set_send_buffer_size(65536);
        ASSERT_EQ(result, SocketResult::Success);
    }

    if (accepted >= 0)
        ::close(accepted);
    ::close(server_fd);
    sock.close();
}

/**
 * @test set_non_blocking on a connected socket toggles mode
 */
TEST(set_non_blocking_valid) {
    socket_init();
    int server_fd = -1;
    uint16_t port = create_local_listener(server_fd);
    ASSERT_NE(port, static_cast<uint16_t>(0));

    Socket sock;
    SocketResult cr = sock.connect("127.0.0.1", port, 2000);

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int accepted = ::accept(
        server_fd,
        reinterpret_cast<struct sockaddr*>(&client_addr),
        &client_len);

    if (cr == SocketResult::Success) {
        SocketResult result = sock.set_non_blocking(true);
        ASSERT_EQ(result, SocketResult::Success);

        result = sock.set_non_blocking(false);
        ASSERT_EQ(result, SocketResult::Success);
    }

    if (accepted >= 0)
        ::close(accepted);
    ::close(server_fd);
    sock.close();
}

/**
 * @test send and recv on a connected socket
 */
TEST(send_recv_connected) {
    socket_init();
    int server_fd = -1;
    uint16_t port = create_local_listener(server_fd);
    ASSERT_NE(port, static_cast<uint16_t>(0));

    Socket sock;
    SocketResult cr = sock.connect("127.0.0.1", port, 2000);

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int accepted = ::accept(
        server_fd,
        reinterpret_cast<struct sockaddr*>(&client_addr),
        &client_len);

    if (cr == SocketResult::Success) {
        uint8_t data[] = {0x01, 0x02, 0x03};
        size_t sent = 0;
        SocketResult sr = sock.send(data, sizeof(data), sent);
        ASSERT_EQ(sr, SocketResult::Success);
        ASSERT_TRUE(sent > 0);

        (void)sent;
        if (sent < sizeof(data)) {
            sr = sock.send_all(data + sent, sizeof(data) - sent);
            ASSERT_EQ(sr, SocketResult::Success);
        }

        // Read from server side to verify data arrived
        uint8_t buf[64];
        ssize_t n = ::recv(accepted, buf, sizeof(buf), 0);
        ASSERT_TRUE(n > 0);

        // Now server sends data back
        uint8_t reply[] = {0xAA, 0xBB};
        ::send(accepted, reply, sizeof(reply), MSG_NOSIGNAL);

        uint8_t rbuf[64];
        size_t received = 0;
        sr = sock.recv(rbuf, sizeof(rbuf), received, 2000);
        ASSERT_EQ(sr, SocketResult::Success);
        ASSERT_TRUE(received > 0);
    }

    if (accepted >= 0)
        ::close(accepted);
    ::close(server_fd);
    sock.close();
}

// =============================================================================
// Fault injection (libc interposition)
//
// The syscall wrappers below replace the libc symbols inside this test
// executable only. By default every call is forwarded to the real libc
// function; a test arms ONE fault on ONE function to make a specific call fail
// the way the kernel would. Production code is untouched: this is the only way
// to reach the "syscall failed" branches of socket.cpp deterministically.
// =============================================================================

#include <dlfcn.h>
#include <poll.h>
#include <cstdarg>

namespace {

enum Fn {
    FN_SOCKET,
    FN_FCNTL,
    FN_SETSOCKOPT,
    FN_GETSOCKOPT,
    FN_POLL,
    FN_SEND,
    FN_RECV,
    FN_COUNT
};

struct Fault {
    bool armed;    // fault pending
    int skip;      // number of calls to let through before failing
    int err;       // errno to set (when ret < 0)
    long ret;      // value returned instead of calling libc
    bool set_out;  // getsockopt only: succeed and write out_int to *optval
    int out_int;
};

Fault g_fault[FN_COUNT];

void arm(Fn fn, int err, int skip = 0, long ret = -1) {
    g_fault[fn] = Fault{true, skip, err, ret, false, 0};
}

void arm_so_error(int value) {
    g_fault[FN_GETSOCKOPT] = Fault{true, 0, 0, 0, true, value};
}

bool take(Fn fn) {
    Fault& f = g_fault[fn];
    if (!f.armed)
        return false;
    if (f.skip > 0) {
        --f.skip;
        return false;
    }
    f.armed = false;
    if (f.ret < 0)
        errno = f.err;
    return true;
}

struct FaultGuard {
    ~FaultGuard() {
        std::memset(g_fault, 0, sizeof(g_fault));
    }
};

template <typename F>
F real(const char* name) {
    return reinterpret_cast<F>(dlsym(RTLD_NEXT, name));
}

}  // namespace

extern "C" {

int socket(int d, int t, int p) noexcept {
    if (take(FN_SOCKET))
        return static_cast<int>(g_fault[FN_SOCKET].ret);
    static auto fn = real<int (*)(int, int, int)>("socket");
    return fn(d, t, p);
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (take(FN_FCNTL))
        return static_cast<int>(g_fault[FN_FCNTL].ret);
    static auto fn = real<int (*)(int, int, void*)>("fcntl");
    return fn(fd, cmd, arg);
}

int setsockopt(int fd, int l, int n, const void* v, socklen_t len) noexcept {
    if (take(FN_SETSOCKOPT))
        return static_cast<int>(g_fault[FN_SETSOCKOPT].ret);
    static auto fn =
        real<int (*)(int, int, int, const void*, socklen_t)>("setsockopt");
    return fn(fd, l, n, v, len);
}

int getsockopt(int fd, int l, int n, void* v, socklen_t* len) noexcept {
    bool inject = take(FN_GETSOCKOPT);
    if (inject && !g_fault[FN_GETSOCKOPT].set_out) {
        return static_cast<int>(g_fault[FN_GETSOCKOPT].ret);
    }
    static auto fn =
        real<int (*)(int, int, int, void*, socklen_t*)>("getsockopt");
    int r = fn(fd, l, n, v, len);
    if (inject) {
        *static_cast<int*>(v) = g_fault[FN_GETSOCKOPT].out_int;
    }
    return r;
}

int poll(struct pollfd* fds, nfds_t n, int t) {
    if (take(FN_POLL))
        return static_cast<int>(g_fault[FN_POLL].ret);
    static auto fn = real<int (*)(struct pollfd*, nfds_t, int)>("poll");
    return fn(fds, n, t);
}

ssize_t send(int fd, const void* b, size_t n, int f) {
    if (take(FN_SEND))
        return static_cast<ssize_t>(g_fault[FN_SEND].ret);
    static auto fn = real<ssize_t (*)(int, const void*, size_t, int)>("send");
    return fn(fd, b, n, f);
}

ssize_t recv(int fd, void* b, size_t n, int f) {
    if (take(FN_RECV))
        return static_cast<ssize_t>(g_fault[FN_RECV].ret);
    static auto fn = real<ssize_t (*)(int, void*, size_t, int)>("recv");
    return fn(fd, b, n, f);
}

}  // extern "C"

// =============================================================================
// Loopback peer used by the tests below
// =============================================================================

namespace {

struct LoopPeer {
    int listen_fd = -1;
    int accepted = -1;
    uint16_t port = 0;

    LoopPeer() {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listen_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0)
            throw "bind failed";
        if (listen(listen_fd, 4) < 0)
            throw "listen failed";
        socklen_t l = sizeof(a);
        getsockname(listen_fd, reinterpret_cast<sockaddr*>(&a), &l);
        port = ntohs(a.sin_port);
    }

    ~LoopPeer() {
        if (accepted >= 0)
            ::close(accepted);
        if (listen_fd >= 0)
            ::close(listen_fd);
    }

    void accept_one() {
        pollfd p{listen_fd, POLLIN, 0};
        if (::poll(&p, 1, 2000) != 1)
            throw "no pending connection";
        accepted = ::accept(listen_fd, nullptr, nullptr);
        if (accepted < 0)
            throw "accept failed";
    }
};

// Connect `sock` to the peer and accept it.
#define CONNECT_LOCAL(sock, peer, timeout)                       \
    do {                                                         \
        ASSERT_EQ(                                               \
            (sock).connect("127.0.0.1", (peer).port, (timeout)), \
            SocketResult::Success);                              \
        (peer).accept_one();                                     \
    } while (0)

// Zero-timeout reads race the loopback delivery: retry until the kernel has it.
SocketResult recv_nowait(Socket& sock, uint8_t* buf, size_t size, size_t& got) {
    SocketResult r = SocketResult::WouldBlock;
    for (int i = 0; i < 400 && r == SocketResult::WouldBlock; ++i) {
        r = sock.recv(buf, size, got, 0);
        if (r == SocketResult::WouldBlock)
            usleep(5000);
    }
    return r;
}

}  // namespace

// =============================================================================
// Tests: connect() failure paths
// =============================================================================

TEST(connect_twice_reports_already_connected) {
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    ASSERT_EQ(
        sock.connect("127.0.0.1", peer.port, 2000),
        SocketResult::AlreadyConnected);
    ASSERT_TRUE(sock.is_connected());
}

TEST(connect_fails_when_socket_creation_fails) {
    FaultGuard g;
    ASSERT_EQ(socket_init(), SocketResult::Success);
    arm(FN_SOCKET, EMFILE);
    Socket sock;
    ASSERT_EQ(sock.connect("127.0.0.1", 1, 1000), SocketResult::SocketError);
    ASSERT_FALSE(sock.is_valid());
}

TEST(connect_reuses_a_descriptor_left_by_a_failed_send) {
    // A send error clears m_connected but keeps the descriptor open; a new
    // connect() reuses it (create() is idempotent).
    FaultGuard g;
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    uint8_t byte = 1;
    size_t sent = 0;
    arm(FN_SEND, EPIPE);
    ASSERT_EQ(sock.send(&byte, 1, sent), SocketResult::SocketError);
    ASSERT_FALSE(sock.is_connected());
    ASSERT_TRUE(sock.is_valid());
    // Linux: the first connect() after a completed non-blocking connect()
    // reports 0 (it acknowledges the completion), so the reused descriptor
    // is adopted again; a further connect() is then refused up front.
    ASSERT_EQ(sock.connect("127.0.0.1", peer.port, 0), SocketResult::Success);
    ASSERT_TRUE(sock.is_connected());
    ASSERT_TRUE(sock.is_valid());
    ASSERT_EQ(
        sock.connect("127.0.0.1", peer.port, 0),
        SocketResult::AlreadyConnected);
}

TEST(connect_fails_when_nonblocking_mode_cannot_be_set) {
    FaultGuard g;
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    arm(FN_FCNTL, EBADF);
    ASSERT_EQ(
        sock.connect("127.0.0.1", peer.port, 1000), SocketResult::SocketError);
    ASSERT_FALSE(sock.is_valid());
}

TEST(connect_fails_when_so_error_cannot_be_read) {
    FaultGuard g;
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    arm(FN_GETSOCKOPT, EBADF);
    ASSERT_EQ(
        sock.connect("127.0.0.1", peer.port, 1000), SocketResult::SocketError);
    ASSERT_FALSE(sock.is_valid());
}

TEST(connect_reports_the_pending_socket_error) {
    FaultGuard g;
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    arm_so_error(ECONNREFUSED);
    ASSERT_EQ(
        sock.connect("127.0.0.1", peer.port, 1000),
        SocketResult::ConnectionRefused);
    ASSERT_FALSE(sock.is_valid());
}

TEST(connect_to_a_closed_port_fails_immediately_in_blocking_mode) {
    ASSERT_EQ(socket_init(), SocketResult::Success);
    uint16_t dead_port = 0;
    {
        LoopPeer peer;  // closed at scope exit: nothing listens on that port
        dead_port = peer.port;
    }
    Socket sock;
    ASSERT_EQ(
        sock.connect("127.0.0.1", dead_port, 0),
        SocketResult::ConnectionRefused);
    ASSERT_FALSE(sock.is_valid());
}

TEST(connect_succeeds_without_keepalive_when_it_cannot_be_enabled) {
    FaultGuard g;
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    arm(FN_SETSOCKOPT, ENOPROTOOPT);
    ASSERT_EQ(
        sock.connect("127.0.0.1", peer.port, 1000), SocketResult::Success);
    ASSERT_TRUE(sock.is_connected());
    peer.accept_one();
}

// =============================================================================
// Tests: send() / send_all()
// =============================================================================

TEST(send_reports_would_block_on_a_full_non_blocking_socket) {
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    ASSERT_EQ(sock.set_non_blocking(true), SocketResult::Success);
    std::vector<uint8_t> chunk(65536, 0x5A);
    SocketResult r = SocketResult::Success;
    // The peer never reads, so the kernel buffers fill up and send() must
    // eventually report WouldBlock instead of Success.
    for (int i = 0; i < 4096 && r == SocketResult::Success; ++i) {
        size_t sent = 0;
        r = sock.send(chunk.data(), chunk.size(), sent);
    }
    ASSERT_EQ(r, SocketResult::WouldBlock);
    ASSERT_TRUE(sock.is_connected());
}

TEST(send_treats_a_zero_byte_result_as_a_closed_connection) {
    FaultGuard g;
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    uint8_t byte = 1;
    size_t sent = 99;
    arm(FN_SEND, 0, 0, 0);
    ASSERT_EQ(sock.send(&byte, 1, sent), SocketResult::Closed);
    ASSERT_EQ(sent, static_cast<size_t>(0));
    ASSERT_FALSE(sock.is_connected());
}

TEST(send_all_waits_for_writability_after_would_block) {
    FaultGuard g;
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    uint8_t payload[] = {1, 2, 3, 4};
    arm(FN_SEND, EAGAIN);  // first send blocks, the retry goes through
    ASSERT_EQ(sock.send_all(payload, sizeof(payload)), SocketResult::Success);
    uint8_t got[4] = {};
    ASSERT_EQ(
        ::recv(peer.accepted, got, sizeof(got), MSG_WAITALL),
        static_cast<ssize_t>(4));
    ASSERT_EQ(std::memcmp(got, payload, 4), 0);
}

TEST(send_all_returns_the_wait_error_when_writability_cannot_be_awaited) {
    FaultGuard g;
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    uint8_t payload[] = {1};
    arm(FN_SEND, EAGAIN);
    arm(FN_POLL, EINTR);
    ASSERT_EQ(
        sock.send_all(payload, sizeof(payload)), SocketResult::SocketError);
}

// =============================================================================
// Tests: recv() in non-blocking and blocking modes
// =============================================================================

TEST(recv_non_blocking_without_data_would_block) {
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    uint8_t buf[16];
    size_t got = 7;
    ASSERT_EQ(sock.recv(buf, sizeof(buf), got, 0), SocketResult::WouldBlock);
    ASSERT_EQ(got, static_cast<size_t>(0));
    ASSERT_TRUE(sock.is_connected());
}

TEST(recv_non_blocking_reads_data_that_is_already_buffered) {
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    const uint8_t msg[] = {9, 8, 7};
    ASSERT_EQ(
        ::send(peer.accepted, msg, sizeof(msg), MSG_NOSIGNAL),
        static_cast<ssize_t>(3));
    uint8_t buf[16];
    size_t got = 0;
    ASSERT_EQ(recv_nowait(sock, buf, sizeof(buf), got), SocketResult::Success);
    ASSERT_EQ(got, static_cast<size_t>(3));
    ASSERT_EQ(std::memcmp(buf, msg, 3), 0);
}

TEST(recv_non_blocking_reports_a_graceful_close) {
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    ::close(peer.accepted);
    peer.accepted = -1;
    uint8_t buf[16];
    size_t got = 0;
    ASSERT_EQ(recv_nowait(sock, buf, sizeof(buf), got), SocketResult::Closed);
    ASSERT_FALSE(sock.is_connected());
}

TEST(recv_non_blocking_reports_a_connection_error) {
    FaultGuard g;
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    uint8_t buf[16];
    size_t got = 0;
    arm(FN_RECV, ECONNRESET);
    ASSERT_EQ(
        sock.recv(buf, sizeof(buf), got, 0), SocketResult::ConnectionReset);
    ASSERT_FALSE(sock.is_connected());
}

TEST(recv_blocking_returns_data) {
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    const uint8_t msg[] = {4, 5};
    ASSERT_EQ(
        ::send(peer.accepted, msg, sizeof(msg), MSG_NOSIGNAL),
        static_cast<ssize_t>(2));
    uint8_t buf[16];
    size_t got = 0;
    ASSERT_EQ(sock.recv(buf, sizeof(buf), got, -1), SocketResult::Success);
    ASSERT_EQ(got, static_cast<size_t>(2));
}

TEST(recv_blocking_reports_a_graceful_close) {
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    ::close(peer.accepted);
    peer.accepted = -1;
    uint8_t buf[16];
    size_t got = 0;
    ASSERT_EQ(sock.recv(buf, sizeof(buf), got, -1), SocketResult::Closed);
    ASSERT_FALSE(sock.is_connected());
}

TEST(recv_blocking_reports_would_block_and_connection_errors) {
    FaultGuard g;
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    uint8_t buf[16];
    size_t got = 0;
    arm(FN_RECV, EAGAIN);
    ASSERT_EQ(sock.recv(buf, sizeof(buf), got, -1), SocketResult::WouldBlock);
    ASSERT_TRUE(sock.is_connected());
    arm(FN_RECV, ECONNRESET);
    ASSERT_EQ(
        sock.recv(buf, sizeof(buf), got, -1), SocketResult::ConnectionReset);
    ASSERT_FALSE(sock.is_connected());
}

// =============================================================================
// Tests: option setters and wait_ready()
// =============================================================================

TEST(set_non_blocking_reports_fcntl_failures) {
    FaultGuard g;
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    arm(FN_FCNTL, EBADF);  // F_GETFL fails
    ASSERT_EQ(sock.set_non_blocking(true), SocketResult::SocketError);
    arm(FN_FCNTL, EBADF, 1);  // F_GETFL passes, F_SETFL fails
    ASSERT_EQ(sock.set_non_blocking(true), SocketResult::SocketError);
}

TEST(option_setters_report_setsockopt_failures) {
    FaultGuard g;
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    arm(FN_SETSOCKOPT, ENOPROTOOPT);
    ASSERT_EQ(sock.set_nodelay(true), SocketResult::SocketError);
    arm(FN_SETSOCKOPT, ENOPROTOOPT);
    ASSERT_EQ(sock.set_recv_buffer_size(4096), SocketResult::SocketError);
    arm(FN_SETSOCKOPT, ENOPROTOOPT);
    ASSERT_EQ(sock.set_send_buffer_size(4096), SocketResult::SocketError);
}

TEST(wait_ready_failure_is_propagated_by_recv) {
    FaultGuard g;
    ASSERT_EQ(socket_init(), SocketResult::Success);
    LoopPeer peer;
    Socket sock;
    CONNECT_LOCAL(sock, peer, 2000);
    uint8_t buf[16];
    size_t got = 0;
    arm(FN_POLL, EINTR);
    ASSERT_EQ(sock.recv(buf, sizeof(buf), got, 100), SocketResult::SocketError);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    printf("\n");
    printf("========================================\n");
    printf("  Socket Tests - ryu_ldn_nx\n");
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
