/**
 * @file p2p_proxy_server_real_tests.cpp
 * @brief Unit tests for the real P2pProxyServer (p2p/p2p_proxy_server.cpp)
 *
 * The server listens on a real loopback socket and the tests play the joiners
 * with plain TCP clients speaking the wire protocol. The UPnP side runs on the
 * miniupnpc stand-in (g_fake_upnp). Failures the kernel cannot be asked to
 * produce on demand (a busy port range, a failing accept, a send that stops
 * short, an allocation that returns null like the sysmodule heap does) are
 * injected through the linker's --wrap of the libc entry points the server
 * calls and through a replacement of the global operator new; with no fault
 * armed every wrapper passes straight through to libc.
 *
 * To keep the tests independent of the machine the listen socket is always
 * bound to the loopback interface on an ephemeral port (the requested port is
 * only recorded), and the 250 ms accept poll is shortened.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include <stratosphere.hpp>
#include <miniupnpc/miniupnpc.h>

#include "test_harness.hpp"
#include "p2p/p2p_proxy_server.hpp"

using ams::mitm::p2p::P2pProxyServer;
using ams::mitm::p2p::P2pProxySession;
namespace proto = ryu_ldn::protocol;

// ---------------------------------------------------------------------------
// Fault injection (pass-through while every knob is 0)
// ---------------------------------------------------------------------------
namespace {
std::atomic<int> g_socket_fail{0};  // next socket() calls that fail
std::atomic<int> g_bind_fail{0};    // next bind() calls that fail (EADDRINUSE)
std::atomic<bool> g_bind_fail_all{false};
std::atomic<int> g_listen_fail{0};
std::atomic<int> g_accept_fail{0};  // next accept() calls that fail
std::atomic<int> g_poll_fail{0};    // next accept polls that fail
std::atomic<int> g_poll_errno{EBADF};
std::atomic<bool> g_poll_error_event{
    false};  // next accept poll reports POLLERR
std::atomic<bool> g_nodelay_fail{false};
std::atomic<int> g_send_mode{0};   // 1: fails, 2: one byte short
std::atomic<int> g_recv_eintr{0};  // next recv() calls interrupted
std::atomic<bool> g_new_fail{
    false};  // next P2pProxySession allocation returns null
std::atomic<void*> g_last_session{
    nullptr};  // last P2pProxySession the accept loop allocated
std::atomic<int> g_recv_delay_ms{0};  // every recv() answers this late
std::atomic<uint16_t> g_bound_port{0};
std::atomic<int> g_bind_calls{0};
std::atomic<uint16_t> g_last_requested_port{0};

// Addresses released by operator delete since track_reset()
constexpr int TRACK_MAX = 512;
std::atomic<void*> g_freed[TRACK_MAX];
std::atomic<int> g_freed_count{0};
std::atomic<bool> g_track{false};
}  // namespace

extern "C" {
int __real_socket(int, int, int);
int __real_bind(int, const struct sockaddr*, socklen_t);
int __real_listen(int, int);
int __real_accept(int, struct sockaddr*, socklen_t*);
int __real_poll(struct pollfd*, nfds_t, int);
int __real_setsockopt(int, int, int, const void*, socklen_t);
ssize_t __real_send(int, const void*, size_t, int);
ssize_t __real_recv(int, void*, size_t, int);

namespace {
bool take(std::atomic<int>& counter) {
    int n = counter.load();
    while (n > 0) {
        if (counter.compare_exchange_weak(n, n - 1)) {
            return true;
        }
    }
    return false;
}
}  // namespace

int __wrap_socket(int domain, int type, int protocol) {
    if (take(g_socket_fail)) {
        errno = EMFILE;
        return -1;
    }
    return __real_socket(domain, type, protocol);
}

int __wrap_bind(int fd, const struct sockaddr* addr, socklen_t) {
    ++g_bind_calls;
    sockaddr_in requested{};
    std::memcpy(&requested, addr, sizeof(requested));
    g_last_requested_port = ntohs(requested.sin_port);
    if (g_bind_fail_all) {
        errno = EACCES;
        return -1;
    }
    if (take(g_bind_fail)) {
        errno = EADDRINUSE;
        return -1;
    }
    sockaddr_in loopback{};
    loopback.sin_family = AF_INET;
    loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    loopback.sin_port = 0;
    const int r = __real_bind(
        fd, reinterpret_cast<sockaddr*>(&loopback), sizeof(loopback));
    if (r == 0) {
        socklen_t len = sizeof(loopback);
        getsockname(fd, reinterpret_cast<sockaddr*>(&loopback), &len);
        g_bound_port = ntohs(loopback.sin_port);
    }
    return r;
}

int __wrap_listen(int fd, int backlog) {
    if (take(g_listen_fail)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return __real_listen(fd, backlog);
}

int __wrap_accept(int fd, struct sockaddr* addr, socklen_t* len) {
    if (take(g_accept_fail)) {
        errno = ECONNABORTED;
        return -1;
    }
    return __real_accept(fd, addr, len);
}

int __wrap_poll(struct pollfd* fds, nfds_t n, int timeout) {
    // Only the accept loop polls with exactly 250 ms; the tests' own waits
    // use other timeouts and must stay untouched.
    if (timeout == 250) {
        if (take(g_poll_fail)) {
            errno = g_poll_errno;
            return -1;
        }
        if (g_poll_error_event.exchange(false)) {
            fds[0].revents = POLLERR;
            return 1;
        }
        timeout = 15;
    }
    return __real_poll(fds, n, timeout);
}

int __wrap_setsockopt(
    int fd, int level, int name, const void* value, socklen_t len) {
    if (g_nodelay_fail && level == IPPROTO_TCP && name == TCP_NODELAY) {
        errno = ENOPROTOOPT;
        return -1;
    }
    return __real_setsockopt(fd, level, name, value, len);
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
    if (take(g_recv_eintr)) {
        errno = EINTR;
        return -1;
    }
    const ssize_t r = __real_recv(fd, buf, size, flags);
    const int delay = g_recv_delay_ms.load();
    if (delay > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    return r;
}
}  // extern "C"

// The sysmodule's heap hands out nullptr under pressure instead of throwing
// (see main.cpp); the host allocator is replaced the same way for the one
// object the accept loop allocates (a nothrow new), so that path can be
// exercised. The throwing form stays the standard one.
void* operator new(std::size_t size) {
    void* p = std::malloc(size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (size == sizeof(P2pProxySession) && g_new_fail.exchange(false)) {
        return nullptr;
    }
    void* p = std::malloc(size);
    if (size == sizeof(P2pProxySession)) {
        g_last_session = p;
    }
    return p;
}

void operator delete(void* p) noexcept {
    if (g_track && p != nullptr) {
        const int i = g_freed_count++;
        if (i < TRACK_MAX) {
            g_freed[i] = p;
        }
    }
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    operator delete(p);
}

namespace {

void track_reset() {
    g_freed_count = 0;
    g_track = true;
}

bool was_freed(const void* p) {
    const int n = std::min<int>(g_freed_count, TRACK_MAX);
    for (int i = 0; i < n; ++i) {
        if (g_freed[i] == p) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// What the server forwarded to the master and to the host game
// ---------------------------------------------------------------------------
std::mutex g_rec_mutex;
std::condition_variable g_rec_cv;
std::vector<std::vector<uint8_t>> g_master;

struct HostDelivery {
    uint32_t src_ip;
    uint16_t src_port;
    uint32_t dst_ip;
    uint16_t dst_port;
    proto::ProtocolType protocol;
    std::vector<uint8_t> data;
    void* user;
};
std::vector<HostDelivery> g_host;
int g_user_marker = 0;

void master_cb(const void* data, size_t size, void*) {
    std::lock_guard<std::mutex> lock(g_rec_mutex);
    const auto* p = static_cast<const uint8_t*>(data);
    g_master.emplace_back(p, p + size);
    g_rec_cv.notify_all();
}

void host_cb(
    uint32_t sip,
    uint16_t sport,
    uint32_t dip,
    uint16_t dport,
    proto::ProtocolType protocol,
    const uint8_t* data,
    size_t len,
    void* user) {
    std::lock_guard<std::mutex> lock(g_rec_mutex);
    g_host.push_back(
        {sip,
         sport,
         dip,
         dport,
         protocol,
         std::vector<uint8_t>(data, data + len),
         user});
    g_rec_cv.notify_all();
}

bool wait_master(size_t count, int ms = 3000) {
    std::unique_lock<std::mutex> lock(g_rec_mutex);
    return g_rec_cv.wait_for(lock, std::chrono::milliseconds(ms), [&] {
        return g_master.size() >= count;
    });
}

bool wait_host(size_t count, int ms = 3000) {
    std::unique_lock<std::mutex> lock(g_rec_mutex);
    return g_rec_cv.wait_for(lock, std::chrono::milliseconds(ms), [&] {
        return g_host.size() >= count;
    });
}

size_t host_count() {
    std::lock_guard<std::mutex> lock(g_rec_mutex);
    return g_host.size();
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

/// Let the server threads consume what was just sent (used to prove that
/// something was NOT delivered: there is no event to wait for in that case).
void settle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
}

void reset_world() {
    g_socket_fail = 0;
    g_bind_fail = 0;
    g_bind_fail_all = false;
    g_listen_fail = 0;
    g_accept_fail = 0;
    g_poll_fail = 0;
    g_poll_errno = EBADF;
    g_poll_error_event = false;
    g_nodelay_fail = false;
    g_send_mode = 0;
    g_recv_eintr = 0;
    g_new_fail = false;
    g_recv_delay_ms = 0;
    g_bind_calls = 0;
    g_track = false;
    ams::os::g_host_create_thread_result = 0;
    ams::os::g_host_wait_divisor = 1;
    g_fake_upnp = FakeUpnp{};
    std::lock_guard<std::mutex> lock(g_rec_mutex);
    g_master.clear();
    g_host.clear();
}

namespace {
// The tests run during static initialisation (TEST registers and runs): make
// stdout line-buffered first so a hung or killed run still names its last test.
[[maybe_unused]] const int g_line_buffered =
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
}  // namespace

// ---------------------------------------------------------------------------
// Wire helpers
// ---------------------------------------------------------------------------
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

void append(std::vector<uint8_t>& to, const std::vector<uint8_t>& from) {
    to.insert(to.end(), from.begin(), from.end());
}

proto::ProxyInfo info(
    uint32_t src,
    uint16_t sport,
    uint32_t dst,
    uint16_t dport,
    proto::ProtocolType protocol = proto::ProtocolType::Udp) {
    proto::ProxyInfo i{};
    i.source_ipv4 = src;
    i.source_port = sport;
    i.dest_ipv4 = dst;
    i.dest_port = dport;
    i.protocol = protocol;
    return i;
}

std::vector<uint8_t> proxy_data(
    const proto::ProxyInfo& i, const std::vector<uint8_t>& body) {
    proto::ProxyDataHeader h{};
    h.info = i;
    h.data_length = static_cast<uint32_t>(body.size());
    std::vector<uint8_t> payload(sizeof(h) + body.size());
    std::memcpy(payload.data(), &h, sizeof(h));
    if (!body.empty()) {
        std::memcpy(payload.data() + sizeof(h), body.data(), body.size());
    }
    return packet(proto::PacketId::ProxyData, payload.data(), payload.size());
}

template <class T>
std::vector<uint8_t> typed(proto::PacketId id, const T& value) {
    return packet(id, &value, sizeof(value));
}

proto::ExternalProxyToken make_token(uint32_t vip, uint8_t fill) {
    proto::ExternalProxyToken t{};
    t.virtual_ip = vip;
    std::memset(t.token, fill, sizeof(t.token));
    t.address_family = 2;
    return t;
}

std::vector<uint8_t> auth_packet(uint8_t fill) {
    proto::ExternalProxyConfig c{};
    c.address_family = 2;
    std::memset(c.token, fill, sizeof(c.token));
    return typed(proto::PacketId::ExternalProxy, c);
}

// ---------------------------------------------------------------------------
// A TCP peer of the server
// ---------------------------------------------------------------------------
int dial() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(g_bound_port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void send_all(int fd, const std::vector<uint8_t>& bytes) {
    size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
        if (n <= 0) {
            return;
        }
        off += static_cast<size_t>(n);
    }
}

bool read_exact(int fd, void* out, size_t n, int ms = 3000) {
    auto* p = static_cast<uint8_t*>(out);
    size_t got = 0;
    while (got < n) {
        pollfd pf{fd, POLLIN, 0};
        if (::poll(&pf, 1, ms) <= 0) {
            return false;
        }
        const ssize_t r = ::read(fd, p + got, n - got);
        if (r <= 0) {
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

struct Received {
    proto::PacketId type;
    std::vector<uint8_t> payload;
};

bool read_packet(int fd, Received& out, int ms = 3000) {
    proto::LdnHeader h{};
    if (!read_exact(fd, &h, sizeof(h), ms)) {
        return false;
    }
    out.type = static_cast<proto::PacketId>(h.type);
    out.payload.assign(static_cast<size_t>(h.data_size), 0);
    return out.payload.empty() ||
           read_exact(fd, out.payload.data(), out.payload.size(), ms);
}

/// True when the server closed its end (EOF or reset) within `ms`.
bool closed_by_server(int fd, int ms = 3000) {
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        pollfd pf{fd, POLLIN, 0};
        if (::poll(&pf, 1, 50) > 0) {
            uint8_t b[256];
            const ssize_t r = ::read(fd, b, sizeof(b));
            if (r == 0 || (r < 0 && errno != EINTR && errno != EAGAIN)) {
                return true;
            }
        }
    }
    return false;
}

bool nothing_pending(int fd) {
    pollfd pf{fd, POLLIN, 0};
    return ::poll(&pf, 1, 0) == 0;
}

/// Payload of a received ProxyData: its ProxyDataHeader and the body.
bool parse_proxy_data(
    const Received& r, proto::ProxyDataHeader& h, std::vector<uint8_t>& body) {
    if (r.type != proto::PacketId::ProxyData || r.payload.size() < sizeof(h)) {
        return false;
    }
    std::memcpy(&h, r.payload.data(), sizeof(h));
    body.assign(r.payload.begin() + sizeof(h), r.payload.end());
    return true;
}

bool start_server(P2pProxyServer& server, uint16_t port = 0) {
    g_bound_port = 0;
    return server.Start(port);
}

/// Registers a token, dials the server, authenticates and returns the
/// connected fd once the ProxyConfig answer arrived (-1 on any failure).
int join(P2pProxyServer& server, uint32_t vip, uint8_t fill) {
    server.AddWaitingToken(make_token(vip, fill));
    const int fd = dial();
    if (fd < 0) {
        return -1;
    }
    send_all(fd, auth_packet(fill));
    Received r;
    if (!read_packet(fd, r) || r.type != proto::PacketId::ProxyConfig) {
        ::close(fd);
        return -1;
    }
    return fd;
}

constexpr uint32_t HOST_VIP = 0x0A720001;
constexpr uint32_t VIP_A = 0x0A720002;
constexpr uint32_t VIP_B = 0x0A720003;
constexpr uint32_t BROADCAST = 0x0A72FFFF;

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST(start_listens_and_stop_closes) {
    reset_world();
    P2pProxyServer server;
    ASSERT_FALSE(server.IsRunning());
    ASSERT_TRUE(start_server(server));
    ASSERT_TRUE(server.IsRunning());
    ASSERT_EQ(g_last_requested_port.load(), P2pProxyServer::PRIVATE_PORT_BASE);
    ASSERT_EQ(server.GetPrivatePort(), P2pProxyServer::PRIVATE_PORT_BASE);
    ASSERT_EQ(server.GetPublicPort(), 0);
    const int fd = dial();
    ASSERT_TRUE(fd >= 0);
    ::close(fd);
    const int binds_before = g_bind_calls.load();
    ASSERT_TRUE(
        start_server(server));  // already running: reported, nothing rebound
    ASSERT_EQ(g_bind_calls.load(), binds_before);
    server.Stop();
    ASSERT_FALSE(server.IsRunning());
    server.Stop();
    ASSERT_FALSE(server.IsRunning());
    ASSERT_TRUE(dial() < 0);
}

TEST(start_skips_busy_ports_of_the_range) {
    reset_world();
    P2pProxyServer server;
    g_bind_fail = 3;
    ASSERT_TRUE(start_server(server));
    ASSERT_EQ(g_bind_calls.load(), 4);
    ASSERT_EQ(server.GetPrivatePort(), P2pProxyServer::PRIVATE_PORT_BASE + 3);
}

TEST(start_with_an_explicit_port_tries_only_that_port) {
    reset_world();
    P2pProxyServer server;
    g_bind_fail_all = true;
    ASSERT_FALSE(start_server(server, 40123));
    ASSERT_EQ(g_bind_calls.load(), 1);
    ASSERT_EQ(g_last_requested_port.load(), 40123);
    ASSERT_FALSE(server.IsRunning());
    g_bind_fail_all = false;
    ASSERT_TRUE(start_server(server, 40123));
    ASSERT_EQ(server.GetPrivatePort(), 40123);
}

TEST(start_fails_when_no_port_of_the_range_binds) {
    reset_world();
    P2pProxyServer server;
    g_bind_fail_all = true;
    ASSERT_FALSE(start_server(server));
    ASSERT_EQ(g_bind_calls.load(), P2pProxyServer::PRIVATE_PORT_RANGE);
    ASSERT_FALSE(server.IsRunning());
    g_bind_fail_all = false;
    ASSERT_TRUE(
        start_server(server));  // the failed attempt left nothing behind
}

TEST(start_fails_when_the_socket_or_the_listen_fails) {
    reset_world();
    P2pProxyServer server;
    g_socket_fail = 1;
    ASSERT_FALSE(start_server(server));
    ASSERT_FALSE(server.IsRunning());
    g_listen_fail = 1;
    ASSERT_FALSE(start_server(server));
    ASSERT_FALSE(server.IsRunning());
    ASSERT_TRUE(start_server(server));
}

TEST(destroying_a_running_server_stops_it) {
    reset_world();
    {
        P2pProxyServer server;
        ASSERT_TRUE(start_server(server));
        ASSERT_TRUE(dial() >= 0);
    }
    ASSERT_TRUE(dial() < 0);
}

// ---------------------------------------------------------------------------
// UPnP
// ---------------------------------------------------------------------------

TEST(nat_punch_without_an_igd_returns_zero) {
    reset_world();
    P2pProxyServer server;
    g_fake_upnp.devices_found = false;
    ASSERT_EQ(server.NatPunch(), 0);
    ASSERT_EQ(server.GetPublicPort(), 0);
    server.ReleaseNatPunch();
}

TEST(nat_punch_gives_up_when_no_public_port_can_be_mapped) {
    reset_world();
    P2pProxyServer server;
    g_fake_upnp.add_result = -1;
    ASSERT_EQ(server.NatPunch(), 0);
    ASSERT_EQ(g_fake_upnp.add_calls, P2pProxyServer::PUBLIC_PORT_RANGE);
    ASSERT_EQ(server.GetPublicPort(), 0);
}

TEST(nat_punch_maps_a_port_and_release_deletes_it) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    ASSERT_EQ(server.NatPunch(), P2pProxyServer::PUBLIC_PORT_BASE);
    ASSERT_EQ(server.GetPublicPort(), P2pProxyServer::PUBLIC_PORT_BASE);
    ASSERT_EQ(
        g_fake_upnp.add_int_port, std::to_string(server.GetPrivatePort()));
    ASSERT_EQ(
        g_fake_upnp.add_lease,
        std::to_string(P2pProxyServer::PORT_LEASE_LENGTH));
    server.ReleaseNatPunch();
    ASSERT_EQ(server.GetPublicPort(), 0);
    ASSERT_EQ(
        g_fake_upnp.del_ext_port,
        std::to_string(P2pProxyServer::PUBLIC_PORT_BASE));
}

TEST(nat_punch_skips_the_ports_the_router_refuses) {
    reset_world();
    P2pProxyServer server;
    g_fake_upnp.add_result = 718;
    ASSERT_EQ(server.NatPunch(), 0);
    g_fake_upnp.add_result = 0;
    g_fake_upnp.external_ip_result =
        -1;  // the mapping works, the external address is unknown
    ASSERT_EQ(server.NatPunch(), P2pProxyServer::PUBLIC_PORT_BASE);
    server.ReleaseNatPunch();
}

TEST(a_failing_release_still_clears_the_public_port) {
    reset_world();
    P2pProxyServer server;
    ASSERT_EQ(server.NatPunch(), P2pProxyServer::PUBLIC_PORT_BASE);
    g_fake_upnp.delete_result = -1;
    server.ReleaseNatPunch();
    ASSERT_EQ(server.GetPublicPort(), 0);
    server.ReleaseNatPunch();  // nothing left to release
}

TEST(the_lease_thread_renews_the_mapping_and_stops_with_the_release) {
    reset_world();
    // 50 s of 250 ms naps, compressed 2000 times: ~100 ms per renewal period
    ams::os::g_host_wait_divisor = 2000;
    P2pProxyServer server;
    ASSERT_EQ(server.NatPunch(), P2pProxyServer::PUBLIC_PORT_BASE);
    ASSERT_EQ(
        server.NatPunch(),
        P2pProxyServer::PUBLIC_PORT_BASE);  // second punch: lease thread
                                            // already runs
    const int after_punch = g_fake_upnp.add_calls;
    ASSERT_TRUE(
        wait_until([&] { return g_fake_upnp.add_calls > after_punch; }));
    g_fake_upnp.add_result = -1;  // a failing renewal is reported, not fatal
    const int after_ok = g_fake_upnp.add_calls;
    ASSERT_TRUE(
        wait_until([&] { return g_fake_upnp.add_calls > after_ok + 1; }));
    ams::os::g_host_wait_divisor = 1;
    server.ReleaseNatPunch();
    const int stopped = g_fake_upnp.add_calls;
    settle();
    ASSERT_EQ(g_fake_upnp.add_calls, stopped);
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

TEST(a_joiner_with_a_waiting_token_gets_its_proxy_config) {
    reset_world();
    P2pProxyServer server(master_cb);
    ASSERT_TRUE(start_server(server));
    server.AddWaitingToken(make_token(VIP_A, 0x11));
    const int fd = dial();
    ASSERT_TRUE(fd >= 0);
    send_all(fd, auth_packet(0x11));
    Received r;
    ASSERT_TRUE(read_packet(fd, r));
    ASSERT_TRUE(r.type == proto::PacketId::ProxyConfig);
    ASSERT_EQ(r.payload.size(), sizeof(proto::ProxyConfig));
    proto::ProxyConfig cfg{};
    std::memcpy(&cfg, r.payload.data(), sizeof(cfg));
    ASSERT_EQ(cfg.proxy_ip, VIP_A);
    ASSERT_EQ(cfg.proxy_subnet_mask, 0xFFFF0000u);
    ::close(fd);
}

TEST(the_token_is_consumed_by_its_first_use) {
    reset_world();
    ams::os::g_host_wait_divisor = 10;
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    const int first = join(server, VIP_A, 0x21);
    ASSERT_TRUE(first >= 0);
    const int again = dial();
    send_all(again, auth_packet(0x21));
    ASSERT_TRUE(closed_by_server(again));
    ::close(again);
    ::close(first);
}

TEST(a_token_for_a_private_address_matches_any_physical_ip) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    const int fd = join(server, VIP_A, 0x31);  // physical ip all zeros
    ASSERT_TRUE(fd >= 0);
    ::close(fd);
}

TEST(a_token_naming_the_real_physical_ip_is_accepted) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    auto token = make_token(VIP_A, 0x41);
    token.physical_ip[0] = 127;
    token.physical_ip[3] = 1;  // 127.0.0.1, the address the server sees
    server.AddWaitingToken(token);
    const int fd = dial();
    send_all(fd, auth_packet(0x41));
    Received r;
    ASSERT_TRUE(read_packet(fd, r));
    ASSERT_TRUE(r.type == proto::PacketId::ProxyConfig);
    ::close(fd);
}

TEST(a_matching_token_is_accepted_even_when_the_physical_ip_differs) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    auto token = make_token(VIP_A, 0x51);
    token.physical_ip[0] = 203;
    token.physical_ip[3] = 9;
    server.AddWaitingToken(token);
    auto other_family = make_token(VIP_B, 0x52);
    other_family.address_family = 23;
    other_family.physical_ip[0] = 1;
    server.AddWaitingToken(other_family);
    const int a = dial();
    send_all(a, auth_packet(0x51));
    Received r;
    ASSERT_TRUE(read_packet(a, r));
    ASSERT_TRUE(r.type == proto::PacketId::ProxyConfig);
    const int b = dial();
    send_all(b, auth_packet(0x52));
    ASSERT_TRUE(read_packet(b, r));
    ASSERT_TRUE(r.type == proto::PacketId::ProxyConfig);
    ::close(a);
    ::close(b);
}

TEST(an_unknown_token_is_disconnected_after_the_wait) {
    reset_world();
    ams::os::g_host_wait_divisor = 10;
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    server.AddWaitingToken(make_token(VIP_A, 0x61));
    const int fd = dial();
    send_all(fd, auth_packet(0x99));
    ASSERT_TRUE(closed_by_server(fd));
    ::close(fd);
    // the legitimate token is still usable
    const int ok = dial();
    send_all(ok, auth_packet(0x61));
    Received r;
    ASSERT_TRUE(read_packet(ok, r));
    ASSERT_TRUE(r.type == proto::PacketId::ProxyConfig);
    ::close(ok);
}

TEST(a_token_arriving_during_the_wait_authenticates_the_joiner) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    const int fd = dial();
    send_all(fd, auth_packet(0x71));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    server.AddWaitingToken(make_token(VIP_A, 0x71));
    Received r;
    ASSERT_TRUE(read_packet(fd, r));
    ASSERT_TRUE(r.type == proto::PacketId::ProxyConfig);
    ::close(fd);
}

TEST(the_waiting_queue_drops_its_oldest_token_when_full) {
    reset_world();
    ams::os::g_host_wait_divisor = 10;
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    for (int i = 0; i < 17; ++i) {
        server.AddWaitingToken(
            make_token(0x0A720100 + i, static_cast<uint8_t>(0x80 + i)));
    }
    const int oldest = dial();
    send_all(oldest, auth_packet(0x80));
    ASSERT_TRUE(closed_by_server(oldest));
    ::close(oldest);
    const int newest = dial();
    send_all(newest, auth_packet(0x90));
    Received r;
    ASSERT_TRUE(read_packet(newest, r));
    ASSERT_TRUE(r.type == proto::PacketId::ProxyConfig);
    ::close(newest);
}

TEST(the_ninth_connection_is_refused_once_eight_players_are_in) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    std::vector<int> joiners;
    for (int i = 0; i < P2pProxyServer::MAX_PLAYERS; ++i) {
        const int fd =
            join(server, 0x0A720010 + i, static_cast<uint8_t>(0xA0 + i));
        ASSERT_TRUE(fd >= 0);
        joiners.push_back(fd);
    }
    const int ninth = dial();
    ASSERT_TRUE(ninth >= 0);
    ASSERT_TRUE(closed_by_server(ninth));
    ::close(ninth);
    for (const int fd : joiners) {
        ::close(fd);
    }
}

TEST(the_session_stack_pool_runs_out_after_eight_unauthenticated_peers) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    std::vector<int> peers;
    for (int i = 0; i < P2pProxyServer::MAX_PLAYERS + 1; ++i) {
        peers.push_back(dial());
        ASSERT_TRUE(peers.back() >= 0);
    }
    settle();
    for (const int fd : peers) {
        ::close(fd);
    }
    settle();
}

// ---------------------------------------------------------------------------
// Routing between joiners
// ---------------------------------------------------------------------------

TEST(unicast_proxy_data_reaches_only_its_destination) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    const int a = join(server, VIP_A, 0x01);
    const int b = join(server, VIP_B, 0x02);
    ASSERT_TRUE(a >= 0 && b >= 0);
    send_all(a, proxy_data(info(VIP_A, 1000, VIP_B, 2000), {1, 2, 3, 4}));
    Received r;
    ASSERT_TRUE(read_packet(b, r));
    proto::ProxyDataHeader h{};
    std::vector<uint8_t> body;
    ASSERT_TRUE(parse_proxy_data(r, h, body));
    ASSERT_EQ(h.info.source_ipv4, VIP_A);
    ASSERT_EQ(h.info.dest_ipv4, VIP_B);
    ASSERT_EQ(h.info.source_port, 1000);
    ASSERT_EQ(h.info.dest_port, 2000);
    ASSERT_EQ(body.size(), 4u);
    ASSERT_EQ(body[3], 4);
    settle();
    ASSERT_TRUE(nothing_pending(a));
    ::close(a);
    ::close(b);
}

TEST(a_zero_source_is_filled_with_the_senders_virtual_ip) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    const int a = join(server, VIP_A, 0x01);
    const int b = join(server, VIP_B, 0x02);
    send_all(a, proxy_data(info(0, 1000, VIP_B, 2000), {9}));
    Received r;
    ASSERT_TRUE(read_packet(b, r));
    proto::ProxyDataHeader h{};
    std::vector<uint8_t> body;
    ASSERT_TRUE(parse_proxy_data(r, h, body));
    ASSERT_EQ(h.info.source_ipv4, VIP_A);
    ::close(a);
    ::close(b);
}

TEST(a_spoofed_source_is_dropped) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    const int a = join(server, VIP_A, 0x01);
    const int b = join(server, VIP_B, 0x02);
    send_all(a, proxy_data(info(VIP_B, 1000, VIP_B, 2000), {9}));
    settle();
    ASSERT_TRUE(nothing_pending(b));
    ::close(a);
    ::close(b);
}

TEST(broadcast_reaches_every_authenticated_joiner_including_the_sender) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    const int a = join(server, VIP_A, 0x01);
    const int b = join(server, VIP_B, 0x02);
    send_all(a, proxy_data(info(VIP_A, 1000, BROADCAST, 2000), {7}));
    Received r;
    ASSERT_TRUE(read_packet(a, r));
    ASSERT_TRUE(read_packet(b, r));
    ASSERT_TRUE(r.type == proto::PacketId::ProxyData);
    ::close(a);
    ::close(b);
}

TEST(the_legacy_broadcast_address_is_translated) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    const int a = join(server, VIP_A, 0x01);
    const int b = join(server, VIP_B, 0x02);
    send_all(a, proxy_data(info(VIP_A, 1000, 0xc0a800ff, 2000), {7}));
    Received r;
    ASSERT_TRUE(read_packet(b, r));
    proto::ProxyDataHeader h{};
    std::vector<uint8_t> body;
    ASSERT_TRUE(parse_proxy_data(r, h, body));
    ASSERT_EQ(h.info.dest_ipv4, BROADCAST);
    ::close(a);
    ::close(b);
}

TEST(proxy_connect_reply_and_disconnect_are_routed_like_data) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    const int a = join(server, VIP_A, 0x01);
    const int b = join(server, VIP_B, 0x02);
    Received r;

    proto::ProxyConnectRequest req{
        info(VIP_A, 1000, VIP_B, 2000, proto::ProtocolType::Tcp)};
    send_all(a, typed(proto::PacketId::ProxyConnect, req));
    ASSERT_TRUE(read_packet(b, r));
    ASSERT_TRUE(r.type == proto::PacketId::ProxyConnect);
    ASSERT_EQ(r.payload.size(), sizeof(req));

    proto::ProxyConnectResponse reply{
        info(VIP_B, 2000, VIP_A, 1000, proto::ProtocolType::Tcp)};
    send_all(b, typed(proto::PacketId::ProxyConnectReply, reply));
    ASSERT_TRUE(read_packet(a, r));
    ASSERT_TRUE(r.type == proto::PacketId::ProxyConnectReply);

    proto::ProxyDisconnectMessage bye{};
    bye.info = info(VIP_A, 1000, VIP_B, 2000, proto::ProtocolType::Tcp);
    bye.disconnect_reason = 7;
    send_all(a, typed(proto::PacketId::ProxyDisconnect, bye));
    ASSERT_TRUE(read_packet(b, r));
    ASSERT_TRUE(r.type == proto::PacketId::ProxyDisconnect);
    proto::ProxyDisconnectMessage got{};
    std::memcpy(&got, r.payload.data(), sizeof(got));
    ASSERT_EQ(got.disconnect_reason, 7);
    ::close(a);
    ::close(b);
}

namespace {
/// The three control messages carry a ProxyInfo first and share the routing
/// rules of the data path: build each one around a given ProxyInfo.
std::vector<uint8_t> control_message(
    proto::PacketId id, const proto::ProxyInfo& i) {
    if (id == proto::PacketId::ProxyConnect) {
        return typed(id, proto::ProxyConnectRequest{i});
    }
    if (id == proto::PacketId::ProxyConnectReply) {
        return typed(id, proto::ProxyConnectResponse{i});
    }
    proto::ProxyDisconnectMessage bye{};
    bye.info = i;
    return typed(id, bye);
}
}  // namespace

TEST(control_messages_follow_the_source_fix_spoof_and_broadcast_rules_of_data) {
    for (const auto id :
         {proto::PacketId::ProxyConnect,
          proto::PacketId::ProxyConnectReply,
          proto::PacketId::ProxyDisconnect}) {
        reset_world();
        P2pProxyServer server;
        ASSERT_TRUE(start_server(server));
        const int a = join(server, VIP_A, 0x01);
        const int b = join(server, VIP_B, 0x02);
        Received r;
        proto::ProxyInfo got{};

        // A zero source is replaced by the sender's virtual address.
        send_all(a, control_message(id, info(0, 1000, VIP_B, 2000)));
        ASSERT_TRUE(read_packet(b, r));
        ASSERT_TRUE(r.type == id);
        std::memcpy(&got, r.payload.data(), sizeof(got));
        ASSERT_EQ(got.source_ipv4, VIP_A);

        // A source that is not the sender's address is dropped.
        send_all(a, control_message(id, info(VIP_B, 1000, VIP_B, 2000)));
        settle();
        ASSERT_TRUE(nothing_pending(b));

        // The legacy broadcast address is translated and fanned out.
        send_all(a, control_message(id, info(VIP_A, 1000, 0xc0a800ff, 2000)));
        ASSERT_TRUE(read_packet(b, r));
        ASSERT_TRUE(r.type == id);
        std::memcpy(&got, r.payload.data(), sizeof(got));
        ASSERT_EQ(got.dest_ipv4, BROADCAST);
        ::close(a);
        ::close(b);
    }
}

TEST(an_unauthenticated_peer_cannot_route_anything) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    const int b = join(server, VIP_B, 0x02);
    const int intruder = dial();
    std::vector<uint8_t> burst;
    append(burst, proxy_data(info(VIP_A, 1, VIP_B, 2), {1}));
    proto::ProxyConnectRequest req{info(VIP_A, 1, VIP_B, 2)};
    append(burst, typed(proto::PacketId::ProxyConnect, req));
    proto::ProxyConnectResponse reply{info(VIP_A, 1, VIP_B, 2)};
    append(burst, typed(proto::PacketId::ProxyConnectReply, reply));
    proto::ProxyDisconnectMessage bye{};
    append(burst, typed(proto::PacketId::ProxyDisconnect, bye));
    send_all(intruder, burst);
    settle();
    ASSERT_TRUE(nothing_pending(b));
    ::close(intruder);
    ::close(b);
}

// ---------------------------------------------------------------------------
// The host's own game
// ---------------------------------------------------------------------------

TEST(proxy_data_for_the_host_is_handed_to_the_in_process_callback) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    server.SetHostVirtualIp(HOST_VIP);
    server.SetHostDataCallback(host_cb, &g_user_marker);
    const int a = join(server, VIP_A, 0x01);
    const int b = join(server, VIP_B, 0x02);

    send_all(a, proxy_data(info(VIP_A, 1000, HOST_VIP, 3000), {5, 6}));
    ASSERT_TRUE(wait_host(1));
    {
        std::lock_guard<std::mutex> lock(g_rec_mutex);
        ASSERT_EQ(g_host[0].src_ip, VIP_A);
        ASSERT_EQ(g_host[0].dst_ip, HOST_VIP);
        ASSERT_EQ(g_host[0].dst_port, 3000);
        ASSERT_EQ(g_host[0].data.size(), 2u);
        ASSERT_TRUE(g_host[0].user == &g_user_marker);
    }
    settle();
    ASSERT_TRUE(
        nothing_pending(b));  // not a broadcast: the other joiner sees nothing

    send_all(a, proxy_data(info(VIP_A, 1000, 0xc0a800ff, 3000), {1}));
    ASSERT_TRUE(wait_host(2));  // a broadcast is also for the host
    Received r;
    ASSERT_TRUE(read_packet(b, r));

    send_all(a, proxy_data(info(VIP_A, 1000, VIP_B, 3000), {1}));
    ASSERT_TRUE(read_packet(b, r));
    settle();
    ASSERT_EQ(
        host_count(), 2u);  // a unicast to another joiner is not for the host

    server.SetHostDataCallback(nullptr, nullptr);
    send_all(a, proxy_data(info(VIP_A, 1000, HOST_VIP, 3000), {1}));
    settle();
    ASSERT_EQ(host_count(), 2u);
    ::close(a);
    ::close(b);
}

TEST(a_spoofed_source_never_reaches_the_host) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    server.SetHostVirtualIp(HOST_VIP);
    server.SetHostDataCallback(host_cb, nullptr);
    const int a = join(server, VIP_A, 0x01);
    // Source claims to be another joiner: toward the host's address, toward the
    // broadcast, and toward the legacy broadcast that is translated later.
    send_all(a, proxy_data(info(VIP_B, 1000, HOST_VIP, 3000), {1}));
    send_all(a, proxy_data(info(VIP_B, 1000, BROADCAST, 3000), {1}));
    send_all(a, proxy_data(info(VIP_B, 1000, 0xc0a800ff, 3000), {1}));
    settle();
    ASSERT_EQ(host_count(), 0u);
    ::close(a);
}

TEST(without_a_host_virtual_ip_only_broadcasts_reach_the_host) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    server.SetHostDataCallback(host_cb, nullptr);
    const int a = join(server, VIP_A, 0x01);
    send_all(a, proxy_data(info(VIP_A, 1, 0, 2), {1}));
    settle();
    ASSERT_EQ(host_count(), 0u);
    send_all(a, proxy_data(info(VIP_A, 1, BROADCAST, 2), {1}));
    ASSERT_TRUE(wait_host(1));
    ::close(a);
}

TEST(broadcast_from_host_needs_the_host_virtual_ip) {
    reset_world();
    P2pProxyServer server;
    proto::ProxyDataHeader h{};
    h.info = info(0, 1, BROADCAST, 2);
    const uint8_t body[1] = {1};
    ASSERT_FALSE(server.BroadcastFromHost(h, body, 1));
}

TEST(broadcast_from_host_reaches_every_joiner) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    server.SetHostVirtualIp(HOST_VIP);
    const int a = join(server, VIP_A, 0x01);
    const int b = join(server, VIP_B, 0x02);
    proto::ProxyDataHeader h{};
    h.info = info(
        0, 1000, BROADCAST, 2000);  // source 0: filled with the host's address
    const uint8_t body[3] = {4, 5, 6};
    h.data_length = sizeof(body);
    ASSERT_TRUE(server.BroadcastFromHost(h, body, sizeof(body)));
    ASSERT_EQ(h.info.source_ipv4, HOST_VIP);
    Received r;
    proto::ProxyDataHeader got{};
    std::vector<uint8_t> data;
    ASSERT_TRUE(read_packet(a, r));
    ASSERT_TRUE(parse_proxy_data(r, got, data));
    ASSERT_EQ(got.info.source_ipv4, HOST_VIP);
    ASSERT_EQ(data.size(), 3u);
    ASSERT_TRUE(read_packet(b, r));

    h.info = info(HOST_VIP, 1000, 0xc0a800ff, 2000);  // legacy broadcast
    ASSERT_TRUE(server.BroadcastFromHost(h, body, sizeof(body)));
    ASSERT_EQ(h.info.dest_ipv4, BROADCAST);
    ASSERT_TRUE(read_packet(a, r));
    ASSERT_TRUE(read_packet(b, r));
    ::close(a);
    ::close(b);
}

TEST(broadcast_from_host_unicast_and_its_refusals) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    server.SetHostVirtualIp(HOST_VIP);
    proto::ProxyDataHeader h{};
    const uint8_t body[2] = {1, 2};

    h.info = info(HOST_VIP, 1, BROADCAST, 2);
    ASSERT_FALSE(server.BroadcastFromHost(h, body, 2));  // nobody is connected

    const int a = join(server, VIP_A, 0x01);
    const int b = join(server, VIP_B, 0x02);

    h.info = info(HOST_VIP, 1, VIP_B, 2);
    ASSERT_TRUE(server.BroadcastFromHost(h, body, 2));  // unicast
    Received r;
    ASSERT_TRUE(read_packet(b, r));
    settle();
    ASSERT_TRUE(nothing_pending(a));

    h.info = info(HOST_VIP, 1, 0x0A7200EE, 2);
    ASSERT_FALSE(server.BroadcastFromHost(h, body, 2));  // unknown destination

    h.info = info(VIP_A, 1, VIP_B, 2);
    ASSERT_FALSE(
        server.BroadcastFromHost(h, body, 2));  // not the host's address

    h.info = info(HOST_VIP, 1, VIP_B, 2);
    std::vector<uint8_t> huge(0x10000, 0);
    ASSERT_FALSE(server.BroadcastFromHost(
        h, huge.data(), huge.size()));  // does not fit a packet
    ::close(a);
    ::close(b);
}

// ---------------------------------------------------------------------------
// Disconnections
// ---------------------------------------------------------------------------

TEST(a_departing_joiner_is_reported_to_the_master) {
    reset_world();
    P2pProxyServer server(master_cb);
    ASSERT_TRUE(start_server(server));
    const int a = join(server, VIP_A, 0x01);
    ::close(a);
    ASSERT_TRUE(wait_master(1));
    std::lock_guard<std::mutex> lock(g_rec_mutex);
    proto::LdnHeader h{};
    std::memcpy(&h, g_master[0].data(), sizeof(h));
    ASSERT_TRUE(
        static_cast<proto::PacketId>(h.type) ==
        proto::PacketId::ExternalProxyState);
    proto::ExternalProxyConnectionState st{};
    std::memcpy(&st, g_master[0].data() + sizeof(h), sizeof(st));
    ASSERT_EQ(st.ip_address, VIP_A);
    ASSERT_EQ(st.connected, 0);
}

TEST(a_server_without_master_callback_still_drops_the_joiner) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    const int a = join(server, VIP_A, 0x01);
    ::close(a);
    settle();
    const int again = join(server, VIP_A, 0x02);  // the address is free again
    ASSERT_TRUE(again >= 0);
    ::close(again);
}

TEST(the_master_can_have_a_joiner_and_its_token_removed) {
    reset_world();
    P2pProxyServer server(master_cb);
    ASSERT_TRUE(start_server(server));
    server.HandleExternalProxyStateChange(
        VIP_A, true);  // a connect notification changes nothing
    const int a = join(server, VIP_A, 0x01);
    server.AddWaitingToken(make_token(VIP_B, 0x02));
    server.AddWaitingToken(make_token(0x0A720004, 0x03));
    server.HandleExternalProxyStateChange(VIP_A, false);
    ASSERT_TRUE(closed_by_server(a));
    ASSERT_TRUE(wait_master(1));
    server.HandleExternalProxyStateChange(
        VIP_B, false);  // drops the waiting token only
    ams::os::g_host_wait_divisor = 10;
    const int late = dial();
    send_all(late, auth_packet(0x02));
    ASSERT_TRUE(closed_by_server(late));  // its token is gone
    ::close(late);
    ::close(a);
}

TEST(stopping_the_server_disconnects_the_joiners_without_notifying_the_master) {
    reset_world();
    P2pProxyServer server(master_cb);
    ASSERT_TRUE(start_server(server));
    const int a = join(server, VIP_A, 0x01);
    server.AddWaitingToken(make_token(VIP_B, 0x02));
    server.Stop();
    ASSERT_TRUE(closed_by_server(a));
    ::close(a);
    settle();
    std::lock_guard<std::mutex> lock(g_rec_mutex);
    ASSERT_EQ(g_master.size(), 0u);
}

TEST(stop_waits_for_a_receive_thread_that_is_slow_to_end) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    const int a = join(server, VIP_A, 0x01);
    ASSERT_TRUE(a >= 0);
    track_reset();
    g_recv_delay_ms =
        150;  // the woken recv() returns late: the thread outlives the shutdown
    server.Stop();
    g_recv_delay_ms = 0;
    // Stop() did not return before the thread was done, and freed the session.
    ASSERT_TRUE(was_freed(g_last_session.load()));
    g_track = false;
    ::close(a);
}

TEST(stop_cannot_queue_a_session_when_the_zombie_pool_is_full) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    for (int i = 0; i < P2pProxyServer::MAX_PLAYERS * 2; ++i) {
        server.OnSessionDisconnected(new P2pProxySession(&server, -1, 0));
    }
    const int a = join(server, VIP_A, 0x01);
    ASSERT_TRUE(a >= 0);
    auto* live = static_cast<P2pProxySession*>(g_last_session.load());
    server.Stop();  // no queue slot: the session is disconnected but cannot be
                    // handed over
    ASSERT_FALSE(live->IsConnected());
    settle();     // its receive thread ends on its own
    delete live;  // the owner of last resort frees it
    ::close(a);
}

TEST(a_session_that_ends_by_itself_is_reaped) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    for (int round = 0; round < 3 * P2pProxyServer::MAX_PLAYERS; ++round) {
        const int fd = join(server, VIP_A, static_cast<uint8_t>(round + 1));
        ASSERT_TRUE(
            fd >= 0);  // would fail once the 8 stack slots were all leaked
        ::close(fd);
        settle();
    }
}

TEST(reaping_never_frees_a_session_whose_thread_has_not_finished) {
    reset_world();
    P2pProxyServer server;
    // A session that was queued but whose receive thread never ran stands for
    // one that is still unwinding: its thread has not flagged completion.
    auto* running = new P2pProxySession(&server, -1, 0);
    server.OnSessionDisconnected(running);

    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto* finished = new P2pProxySession(&server, pair[0], 0);
    finished->Start();
    ::close(pair[1]);  // the thread ends and queues the session itself
    track_reset();
    ASSERT_TRUE(wait_until([&] {
        server.ReapZombieSessions();
        return was_freed(finished);
    }));
    ASSERT_FALSE(was_freed(running));
    server.ReapZombieSessions();
    ASSERT_FALSE(was_freed(running));
    g_track = false;
}

TEST(a_full_zombie_pool_drops_the_new_session_pointer) {
    reset_world();
    P2pProxyServer server;
    std::vector<P2pProxySession*> queued;
    for (int i = 0; i < P2pProxyServer::MAX_PLAYERS * 2; ++i) {
        queued.push_back(new P2pProxySession(&server, -1, 0));
        server.OnSessionDisconnected(queued.back());
    }
    auto* overflow = new P2pProxySession(&server, -1, 0);
    server.OnSessionDisconnected(
        overflow);  // the pool refuses it: it is the caller's again
    delete overflow;
    track_reset();
    server.ReapZombieSessions();
    for (const auto* s : queued) {
        ASSERT_FALSE(was_freed(s));
    }
    g_track = false;
}

TEST(a_session_with_no_stack_slot_left_is_marked_disconnected) {
    reset_world();
    std::vector<P2pProxySession*> holders;
    P2pProxyServer server;
    int pairs[P2pProxyServer::MAX_PLAYERS + 1][2];
    for (int i = 0; i < P2pProxyServer::MAX_PLAYERS; ++i) {
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[i]), 0);
        holders.push_back(new P2pProxySession(&server, pairs[i][0], 0));
        holders.back()->Start();
    }
    ASSERT_EQ(
        ::socketpair(
            AF_UNIX, SOCK_STREAM, 0, pairs[P2pProxyServer::MAX_PLAYERS]),
        0);
    P2pProxySession starved(&server, pairs[P2pProxyServer::MAX_PLAYERS][0], 0);
    starved.Start();
    ASSERT_FALSE(starved.IsConnected());
    for (auto* s : holders) {
        s->Disconnect(true);  // from the owner: no report to the server
        delete s;             // joins the receive thread, returns the slot
    }
    // The peers close only now: closing them first would let each receive
    // thread report itself to the server and hand the session to its zombie
    // queue.
    for (int i = 0; i <= P2pProxyServer::MAX_PLAYERS; ++i) {
        ::close(pairs[i][1]);
    }
    ::close(pairs[P2pProxyServer::MAX_PLAYERS][0]);
}

// ---------------------------------------------------------------------------
// Sessions: sending and the receive loop
// ---------------------------------------------------------------------------

TEST(a_disconnected_session_does_not_send) {
    reset_world();
    P2pProxyServer server;
    P2pProxySession never_connected(&server, -1, 0x7F000001);
    ASSERT_EQ(never_connected.GetRemoteIp(), 0x7F000001u);
    ASSERT_EQ(never_connected.GetVirtualIpAddress(), 0u);
    ASSERT_FALSE(never_connected.IsAuthenticated());
    const uint8_t b[1] = {1};
    ASSERT_FALSE(never_connected.Send(b, 1));
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    P2pProxySession closed(&server, pair[0], 0);
    closed.Disconnect(true);
    ASSERT_FALSE(closed.IsConnected());
    ASSERT_FALSE(closed.Send(b, 1));
    closed.Disconnect(true);  // already disconnected
    ::close(pair[1]);
}

TEST(a_failed_or_short_send_to_a_joiner_is_reported_as_failure) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    server.SetHostVirtualIp(HOST_VIP);
    const int a = join(server, VIP_A, 0x01);
    proto::ProxyDataHeader h{};
    h.info = info(HOST_VIP, 1, VIP_A, 2);
    const uint8_t body[2] = {1, 2};
    g_send_mode = 1;
    ASSERT_FALSE(server.BroadcastFromHost(h, body, 2));
    g_send_mode = 2;
    ASSERT_FALSE(server.BroadcastFromHost(h, body, 2));
    g_send_mode = 0;
    ASSERT_TRUE(server.BroadcastFromHost(h, body, 2));
    ::close(a);
}

TEST(the_receive_loop_retries_an_interrupted_recv) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    g_recv_eintr = 3;
    const int a = join(server, VIP_A, 0x01);
    ASSERT_TRUE(a >= 0);
    ::close(a);
}

TEST(several_packets_in_one_segment_are_all_processed) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    const int a = join(server, VIP_A, 0x01);
    const int b = join(server, VIP_B, 0x02);
    std::vector<uint8_t> burst;
    append(burst, proxy_data(info(VIP_A, 1, VIP_B, 2), {1}));
    append(burst, proxy_data(info(VIP_A, 1, VIP_B, 2), {2}));
    send_all(a, burst);
    Received r;
    ASSERT_TRUE(read_packet(b, r));
    ASSERT_TRUE(read_packet(b, r));
    ::close(a);
    ::close(b);
}

TEST(malformed_input_is_ignored_or_ends_the_session) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    const int b = join(server, VIP_B, 0x02);

    // a partial header, then nothing: waits for more, no effect
    int fd = dial();
    send_all(fd, std::vector<uint8_t>(5, 0));
    settle();
    ::close(fd);

    // a header announcing more than was sent
    fd = dial();
    proto::LdnHeader h{};
    h.magic = proto::PROTOCOL_MAGIC;
    h.type = static_cast<uint8_t>(proto::PacketId::ProxyData);
    h.version = 1;
    h.data_size = 100;
    std::vector<uint8_t> cut(sizeof(h) + 10, 0);
    std::memcpy(cut.data(), &h, sizeof(h));
    send_all(fd, cut);
    settle();
    ::close(fd);

    // an unknown packet type is skipped, the packet behind it is processed
    fd = join(server, VIP_A, 0x01);
    std::vector<uint8_t> two;
    append(two, packet(static_cast<proto::PacketId>(200), "xy", 2));
    append(two, proxy_data(info(VIP_A, 1, VIP_B, 2), {3}));
    send_all(fd, two);
    Received r;
    ASSERT_TRUE(read_packet(b, r));
    ASSERT_TRUE(r.type == proto::PacketId::ProxyData);

    // packets too small for their type are skipped without being routed
    std::vector<uint8_t> tiny;
    const uint8_t few[2] = {0, 0};
    append(tiny, packet(proto::PacketId::ExternalProxy, few, 2));
    append(tiny, packet(proto::PacketId::ProxyData, few, 2));
    append(tiny, packet(proto::PacketId::ProxyConnect, few, 2));
    append(tiny, packet(proto::PacketId::ProxyConnectReply, few, 2));
    append(tiny, packet(proto::PacketId::ProxyDisconnect, few, 2));
    send_all(fd, tiny);
    settle();
    ASSERT_TRUE(nothing_pending(b));

    // a wrong magic ends the session
    std::vector<uint8_t> bad(sizeof(proto::LdnHeader), 0xEE);
    send_all(fd, bad);
    ASSERT_TRUE(closed_by_server(fd));
    ::close(fd);
    ::close(b);
}

// ---------------------------------------------------------------------------
// The accept loop under errors
// ---------------------------------------------------------------------------

TEST(the_accept_loop_survives_a_failing_accept_and_poll) {
    reset_world();
    P2pProxyServer server;
    g_poll_fail = 1;
    g_poll_errno = EBADF;
    ASSERT_TRUE(start_server(server));
    settle();
    g_poll_errno = EINTR;
    g_poll_fail = 1;
    settle();
    g_accept_fail = 1;
    const int a =
        join(server, VIP_A, 0x01);  // the first accept fails, the retry works
    ASSERT_TRUE(a >= 0);
    ::close(a);
}

TEST(the_accept_loop_ends_when_the_listen_socket_reports_an_error) {
    reset_world();
    P2pProxyServer server;
    g_poll_error_event = true;
    ASSERT_TRUE(start_server(server));
    settle();
    server.Stop();
    ASSERT_FALSE(server.IsRunning());
}

TEST(a_peer_is_served_even_when_nodelay_cannot_be_set) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    g_nodelay_fail = true;
    const int a = join(server, VIP_A, 0x01);
    ASSERT_TRUE(a >= 0);
    g_nodelay_fail = false;
    ::close(a);
}

TEST(a_connection_is_closed_when_the_session_cannot_be_allocated) {
    reset_world();
    P2pProxyServer server;
    ASSERT_TRUE(start_server(server));
    g_new_fail = true;
    const int fd = dial();
    ASSERT_TRUE(fd >= 0);
    ASSERT_TRUE(closed_by_server(fd));
    ::close(fd);
    const int a = join(server, VIP_A, 0x01);  // the next connection is fine
    ASSERT_TRUE(a >= 0);
    ::close(a);
}

int main() {
    return test_report("P2pProxyServer Tests");
}
