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
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <stratosphere.hpp>

#include "test_harness.hpp"
#include "p2p/p2p_proxy_client.hpp"

using ams::mitm::p2p::P2pProxyClient;
namespace proto = ryu_ldn::protocol;

// ---------------------------------------------------------------------------
// Fault injection through --wrap (pass-through while every mode is 0)
// ---------------------------------------------------------------------------
namespace {
std::atomic<int> g_socket_mode{0};   // 1: EMFILE
std::atomic<int> g_nodelay_mode{0};  // 1: TCP_NODELAY fails
std::atomic<int> g_getfl_mode{0};    // 1: F_GETFL fails
std::atomic<int> g_connect_mode{
    0};  // 1: ECONNREFUSED at once, 2: always EINPROGRESS
std::atomic<int> g_select_mode{0};  // 1: times out (0), 2: error (-1)
std::atomic<int> g_soerror_mode{
    0};  // 1: getsockopt fails, 2: SO_ERROR = ECONNREFUSED
std::atomic<int> g_send_mode{0};  // 1: fails, 2: sends one byte less
std::atomic<int> g_recv_mode{0};  // 1: ECONNRESET, 2: EINTR
}  // namespace

extern "C" {
int __real_socket(int, int, int);
int __real_setsockopt(int, int, int, const void*, socklen_t);
int __real_fcntl(int, int, ...);
int __real_connect(int, const struct sockaddr*, socklen_t);
int __real_select(int, fd_set*, fd_set*, fd_set*, struct timeval*);
int __real_getsockopt(int, int, int, void*, socklen_t*);
ssize_t __real_send(int, const void*, size_t, int);
ssize_t __real_recv(int, void*, size_t, int);

int __wrap_socket(int domain, int type, int protocol) {
    if (g_socket_mode == 1) {
        errno = EMFILE;
        return -1;
    }
    return __real_socket(domain, type, protocol);
}

int __wrap_setsockopt(
    int fd, int level, int name, const void* value, socklen_t len) {
    if (g_nodelay_mode == 1 && level == IPPROTO_TCP && name == TCP_NODELAY) {
        errno = ENOPROTOOPT;
        return -1;
    }
    return __real_setsockopt(fd, level, name, value, len);
}

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
    const long arg = va_arg(ap, long);
    va_end(ap);
    return __real_fcntl(fd, cmd, arg);
}

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
struct Host {
    int listen_fd = -1;
    int peer_fd = -1;
    uint16_t port = 0;

    Host() {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listen_fd, 4);
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
    }

    ~Host() {
        close_peer();
        ::close(listen_fd);
    }

    bool accept_peer() {
        pollfd p{listen_fd, POLLIN, 0};
        if (::poll(&p, 1, 3000) <= 0) {
            return false;
        }
        peer_fd = ::accept(listen_fd, nullptr, nullptr);
        return peer_fd >= 0;
    }

    void close_peer() {
        if (peer_fd >= 0) {
            ::close(peer_fd);
            peer_fd = -1;
        }
    }

    void write_all(const std::vector<uint8_t>& bytes) const {
        ::write(peer_fd, bytes.data(), bytes.size());
    }

    /// Read exactly `n` bytes (what the client sent), or fewer on timeout.
    std::vector<uint8_t> read_n(size_t n) const {
        std::vector<uint8_t> out;
        while (out.size() < n) {
            pollfd p{peer_fd, POLLIN, 0};
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
struct Recorded {
    proto::PacketId type;
    std::vector<uint8_t> bytes;
};

std::mutex g_rec_mutex;
std::condition_variable g_rec_cv;
std::vector<Recorded> g_rec;

void on_packet(proto::PacketId type, const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(g_rec_mutex);
    const auto* p = static_cast<const uint8_t*>(data);
    g_rec.push_back({type, std::vector<uint8_t>(p, p + size)});
    g_rec_cv.notify_all();
}

bool wait_recorded(size_t count, int ms = 3000) {
    std::unique_lock<std::mutex> lock(g_rec_mutex);
    return g_rec_cv.wait_for(lock, std::chrono::milliseconds(ms), [&] {
        return g_rec.size() >= count;
    });
}

size_t recorded_count() {
    std::lock_guard<std::mutex> lock(g_rec_mutex);
    return g_rec.size();
}

template <class Pred>
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

void reset_world() {
    g_socket_mode = g_nodelay_mode = g_getfl_mode = g_connect_mode = 0;
    g_select_mode = g_soerror_mode = g_send_mode = g_recv_mode = 0;
    ams::os::g_host_create_thread_result = 0;
    std::lock_guard<std::mutex> lock(g_rec_mutex);
    g_rec.clear();
}

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

std::vector<uint8_t> blob(size_t size, uint8_t fill) {
    return std::vector<uint8_t>(size, fill);
}

void append(std::vector<uint8_t>& to, const std::vector<uint8_t>& from) {
    to.insert(to.end(), from.begin(), from.end());
}

/// Client connected to `host` with its peer accepted; null on failure.
bool connect_pair(P2pProxyClient& client, Host& host) {
    return client.Connect("127.0.0.1", host.port) && host.accept_peer();
}

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

TEST(connecting_twice_keeps_the_first_connection) {
    reset_world();
    Host host;
    P2pProxyClient client;
    ASSERT_TRUE(connect_pair(client, host));
    ASSERT_TRUE(client.Connect("127.0.0.1", host.port));
    ASSERT_TRUE(client.IsConnected());
}

TEST(disconnect_is_a_no_op_when_nothing_is_connected) {
    reset_world();
    P2pProxyClient client;
    client.Disconnect();
    client.Disconnect();
    ASSERT_FALSE(client.IsConnected());
    ASSERT_FALSE(client.IsReady());
}

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

TEST(a_refused_connect_fails_at_once) {
    reset_world();
    Host host;
    P2pProxyClient client;
    g_connect_mode = 1;
    ASSERT_FALSE(client.Connect("127.0.0.1", host.port));
    ASSERT_FALSE(client.IsConnected());
}

TEST(a_pending_connect_that_completes_is_accepted) {
    reset_world();
    Host host;
    P2pProxyClient client;
    g_connect_mode = 2;
    ASSERT_TRUE(connect_pair(client, host));
    ASSERT_TRUE(client.IsConnected());
}

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

TEST(ensure_ready_fails_when_not_connected_and_times_out_without_a_config) {
    reset_world();
    Host host;
    P2pProxyClient client;
    ASSERT_FALSE(client.EnsureProxyReady(50));
    ASSERT_TRUE(connect_pair(client, host));
    ASSERT_FALSE(client.EnsureProxyReady(60));
    ASSERT_FALSE(client.IsReady());
}

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
    ASSERT_EQ(client.GetVirtualIp(), 0x0A720002u);
    ASSERT_EQ(client.GetProxyConfig().proxy_subnet_mask, 0xFFFF0000u);
    ASSERT_EQ(recorded_count(), 0u);
}

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

TEST(every_packet_type_is_dispatched_in_order_from_one_stream) {
    reset_world();
    Host host;
    P2pProxyClient client(on_packet);
    ASSERT_TRUE(connect_pair(client, host));

    const auto config = make_config(0x0A720003, 0xFFFFFF00);
    auto data = blob(sizeof(proto::ProxyDataHeader) + 5, 0x11);
    for (size_t i = 0; i < 5; ++i) {
        data[sizeof(proto::ProxyDataHeader) + i] = uint8_t(0xA0 + i);
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
    ASSERT_EQ(client.GetVirtualIp(), 0x0A720003u);

    std::lock_guard<std::mutex> lock(g_rec_mutex);
    ASSERT_EQ(g_rec.size(), 4u);
    ASSERT_TRUE(g_rec[0].type == proto::PacketId::ProxyData);
    ASSERT_EQ(g_rec[0].bytes, data);
    ASSERT_TRUE(g_rec[1].type == proto::PacketId::ProxyConnect);
    ASSERT_EQ(g_rec[1].bytes, connect);
    ASSERT_TRUE(g_rec[2].type == proto::PacketId::ProxyConnectReply);
    ASSERT_EQ(g_rec[2].bytes, reply);
    ASSERT_TRUE(g_rec[3].type == proto::PacketId::ProxyDisconnect);
    ASSERT_EQ(g_rec[3].bytes, bye);
}

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
    ASSERT_EQ(client.GetVirtualIp(), 0x0A720004u);
}

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
    ASSERT_EQ(recorded_count(), 1u);
    ASSERT_FALSE(client.IsReady());
}

TEST(an_unknown_packet_type_is_skipped) {
    reset_world();
    Host host;
    P2pProxyClient client(on_packet);
    ASSERT_TRUE(connect_pair(client, host));

    const auto junk = blob(4, 0x99);
    const auto valid = blob(sizeof(proto::ProxyConnectRequest), 0x66);
    std::vector<uint8_t> stream;
    append(
        stream,
        packet(static_cast<proto::PacketId>(0x7F), junk.data(), junk.size()));
    append(
        stream,
        packet(proto::PacketId::ProxyConnect, valid.data(), valid.size()));
    host.write_all(stream);

    ASSERT_TRUE(wait_recorded(1));
    ASSERT_TRUE(g_rec[0].type == proto::PacketId::ProxyConnect);
}

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
    ASSERT_EQ(recorded_count(), 0u);

    // A header whose magic is wrong, followed by a good packet in the same
    // read: the parser gives up on the whole read.
    auto bad_magic = good;
    bad_magic[0] ^= 0xFF;
    std::vector<uint8_t> stream = bad_magic;
    append(stream, good);
    host.write_all(stream);
    settle();
    ASSERT_EQ(recorded_count(), 0u);

    // A header announcing more payload than was received.
    host.write_all(std::vector<uint8_t>(good.begin(), good.end() - 4));
    settle();
    ASSERT_EQ(recorded_count(), 0u);

    // The connection is still usable afterwards.
    host.write_all(good);
    ASSERT_TRUE(wait_recorded(1));
}

// ---------------------------------------------------------------------------
// The receive thread ending
// ---------------------------------------------------------------------------

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

TEST(destroying_a_connected_client_closes_its_connection) {
    reset_world();
    Host host;
    auto* client = new P2pProxyClient(on_packet);
    ASSERT_TRUE(connect_pair(*client, host));
    delete client;
    const auto got = host.read_n(1);
    ASSERT_EQ(got.size(), 0u);
}

int main() {
    return test_report("P2pProxyClient Tests");
}
