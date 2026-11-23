/**
 * @file p2p_proxy_client_real_tests.cpp
 * @brief Unit tests for the real P2pProxyClient (p2p/p2p_proxy_client.cpp)
 *
 * The client talks to a real TCP listener on the loopback interface, driven by
 * the test (the "host"). Failures that the kernel cannot be asked to produce
 * on demand (socket exhaustion, a connect that stays pending, select timing
 * out, a send that fails) are injected through the linker's --wrap of the
 * libc entry points the client calls; with no fault armed every wrapper passes
 * straight through to libc. Threads and condition variables come from the host
 * Stratosphere stand-in.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <stratosphere.hpp>

#include "test_harness.hpp"
#include "p2p/p2p_proxy_client.hpp"

namespace proto = ryu_ldn::protocol;
using ams::mitm::p2p::P2pProxyClient;

// ---------------------------------------------------------------------------
// Fault injection through --wrap (pass-through while every mode is 0)
// ---------------------------------------------------------------------------
namespace {
/// Fault mode of socket(): 0 pass through, 1 fail with EMFILE.
std::atomic<int> g_socket_mode{0};  // 1: EMFILE
/// Fault mode of setsockopt(TCP_NODELAY): 0 pass through, 1 fail.
std::atomic<int> g_nodelay_mode{0};  // 1: TCP_NODELAY fails
/// Fault mode of fcntl(F_GETFL): 0 pass through, 1 fail.
std::atomic<int> g_getfl_mode{0};  // 1: F_GETFL fails
/// Fault mode of connect(): 0 pass through, 1 ECONNREFUSED, 2 always
/// EINPROGRESS.
std::atomic<int> g_connect_mode{
    0};  // 1: ECONNREFUSED at once, 2: always EINPROGRESS
/// Fault mode of select(): 0 pass through, 1 times out, 2 error.
std::atomic<int> g_select_mode{0};  // 1: times out (0), 2: error (-1)
/// Fault mode of getsockopt(SO_ERROR): 0 pass through, 1 fails, 2 reports
/// ECONNREFUSED.
std::atomic<int> g_soerror_mode{
    0};  // 1: getsockopt fails, 2: SO_ERROR = ECONNREFUSED
/// Fault mode of send(): 0 pass through, 1 fails, 2 sends one byte less.
std::atomic<int> g_send_mode{0};  // 1: fails, 2: sends one byte less
/// Fault mode of recv(): 0 pass through, 1 ECONNRESET, 2 EINTR.
std::atomic<int> g_recv_mode{0};  // 1: ECONNRESET, 2: EINTR
}  // namespace

extern "C" {
/**
 * @brief Real libc socket(), resolved by the linker's --wrap.
 * @return The new descriptor or -1
 */
int __real_socket(int, int, int);
/**
 * @brief Real libc setsockopt(), resolved by the linker's --wrap.
 * @return 0 or -1
 */
int __real_setsockopt(int, int, int, const void*, socklen_t);
/**
 * @brief Real libc fcntl(), resolved by the linker's --wrap.
 * @return The fcntl result
 */
int __real_fcntl(int, int, ...);
/**
 * @brief Real libc connect(), resolved by the linker's --wrap.
 * @return 0 or -1
 */
int __real_connect(int, const struct sockaddr*, socklen_t);
/**
 * @brief Real libc select(), resolved by the linker's --wrap.
 * @return Ready count, 0 on timeout, -1 on error
 */
int __real_select(int, fd_set*, fd_set*, fd_set*, struct timeval*);
/**
 * @brief Real libc getsockopt(), resolved by the linker's --wrap.
 * @return 0 or -1
 */
int __real_getsockopt(int, int, int, void*, socklen_t*);
/**
 * @brief Real libc send(), resolved by the linker's --wrap.
 * @return Bytes sent or -1
 */
ssize_t __real_send(int, const void*, size_t, int);
/**
 * @brief Real libc recv(), resolved by the linker's --wrap.
 * @return Bytes received or -1
 */
ssize_t __real_recv(int, void*, size_t, int);

/**
 * @brief socket() wrapper: fails with EMFILE when armed.
 * @param domain Address family.
 * @param type Socket type.
 * @param protocol Protocol.
 * @return The descriptor, or -1.
 */
int __wrap_socket(int domain, int type, int protocol) {
    if (g_socket_mode == 1) {
        errno = EMFILE;
        return -1;
    }
    return __real_socket(domain, type, protocol);
}

/**
 * @brief setsockopt() wrapper: fails TCP_NODELAY when armed.
 * @param fd Socket descriptor.
 * @param level Option level.
 * @param name Option name.
 * @param value Option value.
 * @param len Size of value.
 * @return 0 or -1.
 */
int __wrap_setsockopt(
    int fd, int level, int name, const void* value, socklen_t len) {
    if (g_nodelay_mode == 1 && level == IPPROTO_TCP && name == TCP_NODELAY) {
        errno = ENOPROTOOPT;
        return -1;
    }
    return __real_setsockopt(fd, level, name, value, len);
}

/**
 * @brief fcntl() wrapper: fails F_GETFL when armed.
 * @param fd Descriptor.
 * @param cmd fcntl command.
 * @return The fcntl result.
 */
int __wrap_fcntl(int fd, int cmd, ...) {
    if (cmd == F_GETFL) {
        if (g_getfl_mode == 1) {
            errno = EBADF;
            return -1;
        }
        return __real_fcntl(fd, cmd);
    }
    va_list ap;
    va_start(ap, cmd);
    const std::intptr_t arg = va_arg(ap, std::intptr_t);
    va_end(ap);
    return __real_fcntl(fd, cmd, arg);
}

/**
 * @brief connect() wrapper: refuses or stays pending when armed.
 * @param fd Socket descriptor.
 * @param addr Destination address.
 * @param len Size of addr.
 * @return 0 or -1 (errno set).
 */
int __wrap_connect(int fd, const struct sockaddr* addr, socklen_t len) {
    if (g_connect_mode == 1) {
        errno = ECONNREFUSED;
        return -1;
    }
    const int r = __real_connect(fd, addr, len);
    if (g_connect_mode == 2 && r == 0) {
        errno = EINPROGRESS;
        return -1;
    }
    return r;
}

/**
 * @brief select() wrapper: times out or fails when armed.
 * @param n Highest descriptor plus one.
 * @param r Read set.
 * @param w Write set.
 * @param e Exception set.
 * @param t Timeout.
 * @return Ready count, 0 on timeout, -1 on error.
 */
int __wrap_select(int n, fd_set* r, fd_set* w, fd_set* e, struct timeval* t) {
    if (g_select_mode == 1) {
        return 0;
    }
    if (g_select_mode == 2) {
        errno = EBADF;
        return -1;
    }
    return __real_select(n, r, w, e, t);
}

/**
 * @brief getsockopt() wrapper: fails or fakes SO_ERROR when armed.
 * @param fd Socket descriptor.
 * @param level Option level.
 * @param name Option name.
 * @param[out] value Receives the option value.
 * @param[in,out] len Size of value.
 * @return 0 or -1.
 */
int __wrap_getsockopt(
    int fd, int level, int name, void* value, socklen_t* len) {
    if (level == SOL_SOCKET && name == SO_ERROR) {
        if (g_soerror_mode == 1) {
            errno = EBADF;
            return -1;
        }
        if (g_soerror_mode == 2) {
            *static_cast<int*>(value) = ECONNREFUSED;
            return 0;
        }
    }
    return __real_getsockopt(fd, level, name, value, len);
}

/**
 * @brief send() wrapper: fails or truncates by one byte when armed.
 * @param fd Socket descriptor.
 * @param data Bytes to send.
 * @param size Number of bytes.
 * @param flags send() flags.
 * @return Bytes sent or -1.
 */
ssize_t __wrap_send(int fd, const void* data, size_t size, int flags) {
    if (g_send_mode == 1) {
        errno = EPIPE;
        return -1;
    }
    if (g_send_mode == 2) {
        return __real_send(fd, data, size - 1, flags);
    }
    return __real_send(fd, data, size, flags);
}

/**
 * @brief recv() wrapper: fails with ECONNRESET or EINTR when armed.
 * @param fd Socket descriptor.
 * @param[out] buf Receives the bytes.
 * @param size Capacity of buf.
 * @param flags recv() flags.
 * @return Bytes received or -1.
 */
ssize_t __wrap_recv(int fd, void* buf, size_t size, int flags) {
    if (g_recv_mode == 1) {
        errno = ECONNRESET;
        return -1;
    }
    if (g_recv_mode == 2) {
        errno = EINTR;
        return -1;
    }
    return __real_recv(fd, buf, size, flags);
}
}  // extern "C"

namespace {

// ---------------------------------------------------------------------------
// The "host": a loopback listener plus the accepted connection
// ---------------------------------------------------------------------------
/// Loopback listener and its accepted connection, playing the P2P host.
struct Host {
    /// Listening socket, or -1.
    int listen_fd = -1;
    /// Accepted connection to the client, or -1.
    int peer_fd = -1;
    /// Port the listener is bound to (host byte order).
    uint16_t port = 0;

    Host() {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const int bound =
            ::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ASSERT_TRUE(bound == 0);
        ::listen(listen_fd, 4);
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
    }

    ~Host() {
        close_peer();
        ::close(listen_fd);
    }

    /**
     * @brief Accept the client connection (3 s timeout).
     * @return true when a peer was accepted.
     */
    bool accept_peer() {
        pollfd p{.fd = listen_fd, .events = POLLIN, .revents = 0};
        if (::poll(&p, 1, 3000) <= 0) {
            return false;
        }
        peer_fd = ::accept(listen_fd, nullptr, nullptr);
        return peer_fd >= 0;
    }

    /// @brief Close the accepted connection, if any.
    void close_peer() {
        if (peer_fd >= 0) {
            ::close(peer_fd);
            peer_fd = -1;
        }
    }

    /**
     * @brief Send bytes to the client.
     * @param bytes Data to write.
     */
    void write_all(const std::vector<uint8_t>& bytes) const {
        ::write(peer_fd, bytes.data(), bytes.size());
    }

    /// Read exactly `n` bytes (what the client sent), or fewer on timeout.
    /**
     * @brief Read exactly n bytes (what the client sent), or fewer on timeout.
     * @param n Number of bytes expected.
     * @return The bytes read.
     */
    [[nodiscard]] std::vector<uint8_t> read_n(size_t n) const {
        std::vector<uint8_t> out;
        while (out.size() < n) {
            pollfd p{.fd = peer_fd, .events = POLLIN, .revents = 0};
            if (::poll(&p, 1, 3000) <= 0) {
                break;
            }
            uint8_t buf[512];
            const ssize_t got =
                ::read(peer_fd, buf, std::min(sizeof(buf), n - out.size()));
            if (got <= 0) {
                break;
            }
            out.insert(out.end(), buf, buf + got);
        }
        return out;
    }
};

// ---------------------------------------------------------------------------
// What the client forwarded through its packet callback
// ---------------------------------------------------------------------------
/// One packet forwarded through the client's packet callback.
struct Recorded {
    /// Packet type reported by the callback.
    proto::PacketId type;
    /// Payload bytes reported by the callback.
    std::vector<uint8_t> bytes;
};

/// Guards g_rec.
std::mutex g_rec_mutex;
/// Signalled whenever a packet is appended to g_rec.
std::condition_variable g_rec_cv;
/// Packets forwarded so far, in arrival order.
std::vector<Recorded> g_rec;

/**
 * @brief Packet callback handed to the client; records the packet.
 * @param type Packet type.
 * @param data Payload.
 * @param size Payload size in bytes.
 */
void on_packet(proto::PacketId type, const void* data, size_t size) {
    std::scoped_lock lock(g_rec_mutex);
    const auto* p = static_cast<const uint8_t*>(data);
    g_rec.push_back({type, std::vector<uint8_t>(p, p + size)});
    g_rec_cv.notify_all();
}

/**
 * @brief Wait until at least count packets were recorded.
 * @param count Number of packets to wait for.
 * @param ms Timeout in milliseconds.
 * @return true when count was reached in time.
 */
bool wait_recorded(size_t count, int ms = 3000) {
    std::unique_lock<std::mutex> lock(g_rec_mutex);
    return g_rec_cv.wait_for(lock, std::chrono::milliseconds(ms), [&] {
        return g_rec.size() >= count;
    });
}

/**
 * @brief Count the recorded packets.
 * @return Number of entries in g_rec.
 */
size_t recorded_count() {
    std::scoped_lock lock(g_rec_mutex);
    return g_rec.size();
}

template <class Pred>
/**
 * @brief Poll a predicate until it holds or the timeout expires.
 * @tparam Pred Callable returning bool.
 * @param pred Condition to wait for.
 * @param ms Timeout in milliseconds.
 * @return true when pred held in time.
 */
bool wait_until(Pred pred, int ms = 3000) {
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

/// Let the receive thread consume what was just written (used to prove that
/// something was NOT forwarded: there is no event to wait for in that case).
void settle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
}

/// @brief Disarm every fault mode and clear the recorded packets.
void reset_world() {
    g_socket_mode = g_nodelay_mode = g_getfl_mode = g_connect_mode = 0;
    g_select_mode = g_soerror_mode = g_send_mode = g_recv_mode = 0;
    ams::os::g_host_create_thread_result = 0;
    std::scoped_lock lock(g_rec_mutex);
    g_rec.clear();
}

/**
 * @brief Encode a framed packet.
 * @param id Packet type.
 * @param payload Payload bytes.
 * @param size Payload size.
 * @return Header plus payload.
 */
std::vector<uint8_t> packet(
    proto::PacketId id, const void* payload, size_t size) {
    std::vector<uint8_t> out(sizeof(proto::LdnHeader) + size);
    size_t written = 0;
    proto::encode_raw(
        out.data(),
        out.size(),
        id,
        static_cast<const uint8_t*>(payload),
        size,
        written);
    out.resize(written);
    return out;
}

/**
 * @brief Encode a framed packet with a raw wire type byte.
 *
 * The type field is a byte on the wire, so handling an unknown type needs a
 * value that names no PacketId enumerator; writing the byte directly keeps
 * the test out of the enum's valid range.
 *
 * @param wire_type Type byte written to the header.
 * @param payload Payload bytes.
 * @param size Payload size.
 * @return Header plus payload.
 */
std::vector<uint8_t> packet(
    uint8_t wire_type, const void* payload, size_t size) {
    std::vector<uint8_t> out =
        packet(proto::PacketId::Initialize, payload, size);
    out[offsetof(proto::LdnHeader, type)] = wire_type;
    return out;
}

/**
 * @brief Build a constant-filled buffer.
 * @param size Number of bytes.
 * @param fill Byte value.
 * @return The buffer.
 */
std::vector<uint8_t> blob(size_t size, uint8_t fill) {
    return std::vector<uint8_t>(size, fill);
}

/**
 * @brief Append one buffer to another.
 * @param[in,out] to Destination.
 * @param from Bytes to append.
 */
void append(std::vector<uint8_t>& to, const std::vector<uint8_t>& from) {
    to.insert(to.end(), from.begin(), from.end());
}

/**
 * @brief Connects `client` to `host` and accepts the peer on the host side.
 *
 * @param client client under test, connected to 127.0.0.1
 * @param host local test host that accepts the connection
 * @return true when Connect() succeeded and the host accepted the peer
 */
bool connect_pair(P2pProxyClient& client, Host& host) {
    return client.Connect("127.0.0.1", host.port) && host.accept_peer();
}

/**
 * @brief Build a ProxyConfig.
 * @param ip Virtual IP assigned to the client.
 * @param mask Subnet mask.
 * @return The config.
 */
proto::ProxyConfig make_config(uint32_t ip, uint32_t mask) {
    proto::ProxyConfig c{};
    c.proxy_ip = ip;
    c.proxy_subnet_mask = mask;
    return c;
}

}  // namespace

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------

/// @test Verify connect refuses a null or malformed address.
TEST(connect_refuses_a_null_or_malformed_address) {
    reset_world();
    P2pProxyClient client;
    ASSERT_FALSE(client.Connect(static_cast<const char*>(nullptr), 1234));
    ASSERT_FALSE(client.Connect("not-an-address", 1234));
    ASSERT_FALSE(client.Connect("256.1.1.1", 1234));
    const uint8_t ip[4] = {127, 0, 0, 1};
    ASSERT_FALSE(client.Connect(static_cast<const uint8_t*>(nullptr), 4, 1234));
    ASSERT_FALSE(client.Connect(ip, 3, 1234));
    ASSERT_FALSE(client.IsConnected());
}

/// @test Verify connect by string and by bytes reaches the listener.
TEST(connect_by_string_and_by_bytes_reaches_the_listener) {
    reset_world();
    Host host;
    P2pProxyClient by_string;
    ASSERT_TRUE(by_string.Connect("127.0.0.1", host.port));
    ASSERT_TRUE(host.accept_peer());
    ASSERT_TRUE(by_string.IsConnected());
    by_string.Disconnect();
    ASSERT_FALSE(by_string.IsConnected());
    host.close_peer();

    P2pProxyClient by_bytes;
    const uint8_t ip[4] = {127, 0, 0, 1};
    ASSERT_TRUE(by_bytes.Connect(ip, 4, host.port));
    ASSERT_TRUE(host.accept_peer());
    ASSERT_TRUE(by_bytes.IsConnected());
}

/// @test Verify connecting twice keeps the first connection.
TEST(connecting_twice_keeps_the_first_connection) {
    reset_world();
    Host host;
    P2pProxyClient client;
    ASSERT_TRUE(connect_pair(client, host));
    ASSERT_TRUE(client.Connect("127.0.0.1", host.port));
    ASSERT_TRUE(client.IsConnected());
}

/// @test Verify disconnect is a no op when nothing is connected.
TEST(disconnect_is_a_no_op_when_nothing_is_connected) {
    reset_world();
    P2pProxyClient client;
    client.Disconnect();
    client.Disconnect();
    ASSERT_FALSE(client.IsConnected());
    ASSERT_FALSE(client.IsReady());
}

/// @test Verify a failing socket creation fails the connect and a retry works.
TEST(a_failing_socket_creation_fails_the_connect_and_a_retry_works) {
    reset_world();
    Host host;
    P2pProxyClient client;
    g_socket_mode = 1;
    ASSERT_FALSE(client.Connect("127.0.0.1", host.port));
    ASSERT_FALSE(client.IsConnected());
    g_socket_mode = 0;
    ASSERT_TRUE(connect_pair(client, host));
}

/// @test Verify connect still succeeds when nodelay or the flags cannot be set.
TEST(connect_still_succeeds_when_nodelay_or_the_flags_cannot_be_set) {
    reset_world();
    Host host;
    P2pProxyClient client;
    g_nodelay_mode = 1;
    ASSERT_TRUE(connect_pair(client, host));
    client.Disconnect();
    host.close_peer();

    g_nodelay_mode = 0;
    g_getfl_mode = 1;
    ASSERT_TRUE(connect_pair(client, host));
}

/// @test Verify a refused connect fails at once.
TEST(a_refused_connect_fails_at_once) {
    reset_world();
    Host host;
    P2pProxyClient client;
    g_connect_mode = 1;
    ASSERT_FALSE(client.Connect("127.0.0.1", host.port));
    ASSERT_FALSE(client.IsConnected());
}

/// @test Verify a pending connect that completes is accepted.
TEST(a_pending_connect_that_completes_is_accepted) {
    reset_world();
    Host host;
    P2pProxyClient client;
    g_connect_mode = 2;
    ASSERT_TRUE(connect_pair(client, host));
    ASSERT_TRUE(client.IsConnected());
}

/// @test Verify a pending connect fails on select timeout select error or
/// socket error.
TEST(a_pending_connect_fails_on_select_timeout_select_error_or_socket_error) {
    reset_world();
    Host host;
    P2pProxyClient client;
    g_connect_mode = 2;

    g_select_mode = 1;
    ASSERT_FALSE(client.Connect("127.0.0.1", host.port));
    g_select_mode = 2;
    ASSERT_FALSE(client.Connect("127.0.0.1", host.port));
    g_select_mode = 0;

    g_soerror_mode = 1;
    ASSERT_FALSE(client.Connect("127.0.0.1", host.port));
    g_soerror_mode = 2;
    ASSERT_FALSE(client.Connect("127.0.0.1", host.port));
    ASSERT_FALSE(client.IsConnected());
}

/// @test Verify a failing receive thread creation fails the connect.
TEST(a_failing_receive_thread_creation_fails_the_connect) {
    reset_world();
    Host host;
    P2pProxyClient client;
    ams::os::g_host_create_thread_result = 0x1234;
    ASSERT_FALSE(client.Connect("127.0.0.1", host.port));
    ASSERT_FALSE(client.IsConnected());
    ams::os::g_host_create_thread_result = 0;
    ASSERT_TRUE(connect_pair(client, host));
}

// ---------------------------------------------------------------------------
// Authentication and readiness
// ---------------------------------------------------------------------------

/// @test Verify authentication needs a connection and sends the external proxy
/// packet.
TEST(authentication_needs_a_connection_and_sends_the_external_proxy_packet) {
    reset_world();
    Host host;
    P2pProxyClient client;
    proto::ExternalProxyConfig config{};
    config.proxy_port = 0x1234;
    std::memset(config.token, 0xAB, sizeof(config.token));

    ASSERT_FALSE(client.PerformAuth(config));
    ASSERT_TRUE(connect_pair(client, host));
    ASSERT_TRUE(client.PerformAuth(config));

    const size_t total = sizeof(proto::LdnHeader) + sizeof(config);
    const auto got = host.read_n(total);
    ASSERT_EQ(got.size(), total);
    proto::LdnHeader header{};
    std::memcpy(&header, got.data(), sizeof(header));
    ASSERT_EQ(header.magic, proto::PROTOCOL_MAGIC);
    ASSERT_EQ(
        header.type, static_cast<uint8_t>(proto::PacketId::ExternalProxy));
    ASSERT_EQ(header.data_size, static_cast<int32_t>(sizeof(config)));
    ASSERT_EQ(
        std::memcmp(got.data() + sizeof(header), &config, sizeof(config)), 0);
}

/// @test Verify authentication reports a failed or short send.
TEST(authentication_reports_a_failed_or_short_send) {
    reset_world();
    Host host;
    P2pProxyClient client;
    proto::ExternalProxyConfig config{};
    ASSERT_TRUE(connect_pair(client, host));
    g_send_mode = 1;
    ASSERT_FALSE(client.PerformAuth(config));
    g_send_mode = 2;
    ASSERT_FALSE(client.PerformAuth(config));
}

/// @test Verify ensure ready fails when not connected and times out without a
/// config.
TEST(ensure_ready_fails_when_not_connected_and_times_out_without_a_config) {
    reset_world();
    Host host;
    P2pProxyClient client;
    ASSERT_FALSE(client.EnsureProxyReady(50));
    ASSERT_TRUE(connect_pair(client, host));
    ASSERT_FALSE(client.EnsureProxyReady(60));
    ASSERT_FALSE(client.IsReady());
}

/// @test Verify ensure ready waits for the proxy config the host sends.
TEST(ensure_ready_waits_for_the_proxy_config_the_host_sends) {
    reset_world();
    Host host;
    P2pProxyClient client(on_packet);
    ASSERT_TRUE(connect_pair(client, host));

    const auto config = make_config(0x0A720002, 0xFFFF0000);
    std::thread sender([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        host.write_all(
            packet(proto::PacketId::ProxyConfig, &config, sizeof(config)));
    });
    ASSERT_TRUE(client.EnsureProxyReady(3000));
    sender.join();

    ASSERT_TRUE(client.IsReady());
    ASSERT_TRUE(client.EnsureProxyReady(1));
    ASSERT_EQ(client.GetVirtualIp(), 0x0A720002U);
    ASSERT_EQ(client.GetProxyConfig().proxy_subnet_mask, 0xFFFF0000U);
    ASSERT_EQ(recorded_count(), 0U);
}

/// @test Verify disconnecting wakes a thread waiting for readiness.
TEST(disconnecting_wakes_a_thread_waiting_for_readiness) {
    reset_world();
    Host host;
    P2pProxyClient client;
    ASSERT_TRUE(connect_pair(client, host));
    std::atomic<int> result{-1};
    std::thread waiter([&] { result = client.EnsureProxyReady(5000) ? 1 : 0; });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    client.Disconnect();
    waiter.join();
    ASSERT_EQ(result.load(), 0);
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

/// @test Verify sending without a connection fails.
TEST(sending_without_a_connection_fails) {
    reset_world();
    P2pProxyClient client;
    const uint8_t byte = 1;
    ASSERT_FALSE(client.Send(&byte, 1));
    proto::ProxyDataHeader header{};
    ASSERT_FALSE(client.SendProxyData(header, &byte, 1));
    ASSERT_FALSE(client.SendProxyConnect(proto::ProxyConnectRequest{}));
    ASSERT_FALSE(client.SendProxyConnectReply(proto::ProxyConnectResponse{}));
    ASSERT_FALSE(client.SendProxyDisconnect(proto::ProxyDisconnectMessage{}));
}

/// @test Verify every proxy message is encoded with its packet type.
TEST(every_proxy_message_is_encoded_with_its_packet_type) {
    reset_world();
    Host host;
    P2pProxyClient client;
    ASSERT_TRUE(connect_pair(client, host));

    proto::ProxyDataHeader data_header{};
    data_header.data_length = 3;
    const uint8_t payload[3] = {7, 8, 9};
    ASSERT_TRUE(client.SendProxyData(data_header, payload, sizeof(payload)));
    ASSERT_TRUE(client.SendProxyConnect(proto::ProxyConnectRequest{}));
    ASSERT_TRUE(client.SendProxyConnectReply(proto::ProxyConnectResponse{}));
    proto::ProxyDisconnectMessage bye{};
    bye.disconnect_reason = 5;
    ASSERT_TRUE(client.SendProxyDisconnect(bye));

    const size_t h = sizeof(proto::LdnHeader);
    const size_t total = (h + sizeof(data_header) + 3) +
                         (h + sizeof(proto::ProxyConnectRequest)) +
                         (h + sizeof(proto::ProxyConnectResponse)) +
                         (h + sizeof(proto::ProxyDisconnectMessage));
    const auto got = host.read_n(total);
    ASSERT_EQ(got.size(), total);

    size_t at = 0;
    auto next_header = [&](proto::PacketId expected, size_t expected_payload) {
        proto::LdnHeader header{};
        std::memcpy(&header, got.data() + at, sizeof(header));
        const bool ok =
            header.type == static_cast<uint8_t>(expected) &&
            static_cast<size_t>(header.data_size) == expected_payload;
        at += sizeof(header) + expected_payload;
        return ok;
    };
    ASSERT_TRUE(
        next_header(proto::PacketId::ProxyData, sizeof(data_header) + 3));
    ASSERT_EQ(std::memcmp(got.data() + h + sizeof(data_header), payload, 3), 0);
    ASSERT_TRUE(next_header(
        proto::PacketId::ProxyConnect, sizeof(proto::ProxyConnectRequest)));
    ASSERT_TRUE(next_header(
        proto::PacketId::ProxyConnectReply,
        sizeof(proto::ProxyConnectResponse)));
    ASSERT_TRUE(next_header(
        proto::PacketId::ProxyDisconnect,
        sizeof(proto::ProxyDisconnectMessage)));
}

/// @test Verify a failed send is reported.
TEST(a_failed_send_is_reported) {
    reset_world();
    Host host;
    P2pProxyClient client;
    ASSERT_TRUE(connect_pair(client, host));
    const uint8_t bytes[4] = {1, 2, 3, 4};
    g_send_mode = 1;
    ASSERT_FALSE(client.Send(bytes, sizeof(bytes)));
    g_send_mode = 2;
    ASSERT_FALSE(client.Send(bytes, sizeof(bytes)));
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

/// @test Verify every packet type is dispatched in order from one stream.
TEST(every_packet_type_is_dispatched_in_order_from_one_stream) {
    reset_world();
    Host host;
    P2pProxyClient client(on_packet);
    ASSERT_TRUE(connect_pair(client, host));

    const auto config = make_config(0x0A720003, 0xFFFFFF00);
    auto data = blob(sizeof(proto::ProxyDataHeader) + 5, 0x11);
    for (size_t i = 0; i < 5; ++i) {
        data[sizeof(proto::ProxyDataHeader) + i] =
            static_cast<uint8_t>(0xA0 + i);
    }
    const auto connect = blob(sizeof(proto::ProxyConnectRequest), 0x22);
    const auto reply = blob(sizeof(proto::ProxyConnectResponse), 0x33);
    const auto bye = blob(sizeof(proto::ProxyDisconnectMessage), 0x44);

    std::vector<uint8_t> stream;
    append(
        stream, packet(proto::PacketId::ProxyConfig, &config, sizeof(config)));
    append(
        stream, packet(proto::PacketId::ProxyData, data.data(), data.size()));
    append(
        stream,
        packet(proto::PacketId::ProxyConnect, connect.data(), connect.size()));
    append(
        stream,
        packet(proto::PacketId::ProxyConnectReply, reply.data(), reply.size()));
    append(
        stream,
        packet(proto::PacketId::ProxyDisconnect, bye.data(), bye.size()));
    host.write_all(stream);

    ASSERT_TRUE(wait_recorded(4));
    ASSERT_TRUE(client.EnsureProxyReady(1000));
    ASSERT_EQ(client.GetVirtualIp(), 0x0A720003U);

    std::scoped_lock lock(g_rec_mutex);
    ASSERT_EQ(g_rec.size(), 4U);
    ASSERT_TRUE(g_rec[0].type == proto::PacketId::ProxyData);
    ASSERT_EQ(g_rec[0].bytes, data);
    ASSERT_TRUE(g_rec[1].type == proto::PacketId::ProxyConnect);
    ASSERT_EQ(g_rec[1].bytes, connect);
    ASSERT_TRUE(g_rec[2].type == proto::PacketId::ProxyConnectReply);
    ASSERT_EQ(g_rec[2].bytes, reply);
    ASSERT_TRUE(g_rec[3].type == proto::PacketId::ProxyDisconnect);
    ASSERT_EQ(g_rec[3].bytes, bye);
}

/// @test Verify a client without a callback still handles the stream.
TEST(a_client_without_a_callback_still_handles_the_stream) {
    reset_world();
    Host host;
    P2pProxyClient client;
    ASSERT_TRUE(connect_pair(client, host));

    const auto config = make_config(0x0A720004, 0xFFFF0000);
    const auto data = blob(sizeof(proto::ProxyDataHeader) + 2, 1);
    const auto connect = blob(sizeof(proto::ProxyConnectRequest), 2);
    const auto reply = blob(sizeof(proto::ProxyConnectResponse), 3);
    const auto bye = blob(sizeof(proto::ProxyDisconnectMessage), 4);
    std::vector<uint8_t> stream;
    append(
        stream, packet(proto::PacketId::ProxyData, data.data(), data.size()));
    append(
        stream,
        packet(proto::PacketId::ProxyConnect, connect.data(), connect.size()));
    append(
        stream,
        packet(proto::PacketId::ProxyConnectReply, reply.data(), reply.size()));
    append(
        stream,
        packet(proto::PacketId::ProxyDisconnect, bye.data(), bye.size()));
    append(
        stream, packet(proto::PacketId::ProxyConfig, &config, sizeof(config)));
    host.write_all(stream);

    ASSERT_TRUE(client.EnsureProxyReady(3000));
    ASSERT_EQ(client.GetVirtualIp(), 0x0A720004U);
}

/// @test Verify a payload smaller than its struct is ignored but parsing goes
/// on.
TEST(a_payload_smaller_than_its_struct_is_ignored_but_parsing_goes_on) {
    reset_world();
    Host host;
    P2pProxyClient client(on_packet);
    ASSERT_TRUE(connect_pair(client, host));

    const auto tiny = blob(1, 0xEE);
    const auto short_data = blob(sizeof(proto::ProxyDataHeader) - 1, 0xEE);
    std::vector<uint8_t> stream;
    append(
        stream, packet(proto::PacketId::ProxyConfig, tiny.data(), tiny.size()));
    append(
        stream,
        packet(
            proto::PacketId::ProxyData, short_data.data(), short_data.size()));
    append(
        stream,
        packet(proto::PacketId::ProxyConnect, tiny.data(), tiny.size()));
    append(
        stream,
        packet(proto::PacketId::ProxyConnectReply, tiny.data(), tiny.size()));
    append(
        stream,
        packet(proto::PacketId::ProxyDisconnect, tiny.data(), tiny.size()));
    const auto valid = blob(sizeof(proto::ProxyConnectRequest), 0x55);
    append(
        stream,
        packet(proto::PacketId::ProxyConnect, valid.data(), valid.size()));
    host.write_all(stream);

    ASSERT_TRUE(wait_recorded(1));
    settle();
    ASSERT_EQ(recorded_count(), 1U);
    ASSERT_FALSE(client.IsReady());
}

/// @test Verify an unknown packet type is skipped.
TEST(an_unknown_packet_type_is_skipped) {
    reset_world();
    Host host;
    P2pProxyClient client(on_packet);
    ASSERT_TRUE(connect_pair(client, host));

    const auto junk = blob(4, 0x99);
    const auto valid = blob(sizeof(proto::ProxyConnectRequest), 0x66);
    std::vector<uint8_t> stream;
    append(stream, packet(uint8_t{0x7F}, junk.data(), junk.size()));
    append(
        stream,
        packet(proto::PacketId::ProxyConnect, valid.data(), valid.size()));
    host.write_all(stream);

    ASSERT_TRUE(wait_recorded(1));
    ASSERT_TRUE(g_rec[0].type == proto::PacketId::ProxyConnect);
}

/// @test Verify a truncated header a bad magic or a truncated packet stop the
/// parsing.
TEST(a_truncated_header_a_bad_magic_or_a_truncated_packet_stop_the_parsing) {
    reset_world();
    Host host;
    P2pProxyClient client(on_packet);
    ASSERT_TRUE(connect_pair(client, host));
    const auto valid = blob(sizeof(proto::ProxyConnectRequest), 0x77);
    const auto good =
        packet(proto::PacketId::ProxyConnect, valid.data(), valid.size());

    // Fewer bytes than a header.
    host.write_all(std::vector<uint8_t>(good.begin(), good.begin() + 5));
    settle();
    ASSERT_EQ(recorded_count(), 0U);

    // A header whose magic is wrong, followed by a good packet in the same
    // read: the parser gives up on the whole read.
    auto bad_magic = good;
    bad_magic[0] ^= 0xFF;
    std::vector<uint8_t> stream = bad_magic;
    append(stream, good);
    host.write_all(stream);
    settle();
    ASSERT_EQ(recorded_count(), 0U);

    // A header announcing more payload than was received.
    host.write_all(std::vector<uint8_t>(good.begin(), good.end() - 4));
    settle();
    ASSERT_EQ(recorded_count(), 0U);

    // The connection is still usable afterwards.
    host.write_all(good);
    ASSERT_TRUE(wait_recorded(1));
}

// ---------------------------------------------------------------------------
// The receive thread ending
// ---------------------------------------------------------------------------

/// @test Verify the host closing the connection marks the client disconnected.
TEST(the_host_closing_the_connection_marks_the_client_disconnected) {
    reset_world();
    Host host;
    P2pProxyClient client;
    ASSERT_TRUE(connect_pair(client, host));
    host.close_peer();
    ASSERT_TRUE(wait_until([&] { return !client.IsConnected(); }));
    ASSERT_FALSE(client.IsReady());

    // The socket is still open on our side: Disconnect releases it, and the
    // client can connect again.
    client.Disconnect();
    ASSERT_TRUE(connect_pair(client, host));
    ASSERT_TRUE(client.IsConnected());
}

/// @test Verify a receive error ends the thread and an interruption does too.
TEST(a_receive_error_ends_the_thread_and_an_interruption_does_too) {
    reset_world();
    Host host;
    P2pProxyClient client;

    g_recv_mode = 1;
    ASSERT_TRUE(connect_pair(client, host));
    ASSERT_TRUE(wait_until([&] { return !client.IsConnected(); }));
    client.Disconnect();
    host.close_peer();

    g_recv_mode = 2;
    ASSERT_TRUE(connect_pair(client, host));
    ASSERT_TRUE(wait_until([&] { return !client.IsConnected(); }));
    g_recv_mode = 0;
    client.Disconnect();
}

/// @test Verify destroying a connected client closes its connection.
TEST(destroying_a_connected_client_closes_its_connection) {
    reset_world();
    Host host;
    auto client = std::make_unique<P2pProxyClient>(on_packet);
    ASSERT_TRUE(connect_pair(*client, host));
    client.reset();  // destroying the client closes its connection
    const auto got = host.read_n(1);
    ASSERT_EQ(got.size(), 0U);
}

/**
 * @brief Run the registered tests.
 * @return The harness exit status.
 */
int main() {
    return test_report("P2pProxyClient Tests");
}
