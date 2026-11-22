/**
 * @file bsd_mitm_service_tests.cpp
 * @brief Unit tests for the real BsdMitmService (bsd/bsd_mitm_service.cpp)
 *
 * The service source is included into this translation unit (unity build) so
 * the tests can reach its file-local tables (socket tracking, PID counts,
 * abandoned forward services) and stage the exact states the code defends
 * against. The real bsd:u is played by host_mitm::g_dispatch_hook (see
 * stubs/stratosphere/sf/sf_mitm_dispatch.h): every forwarded call is recorded
 * (command id, input struct, buffers, flags) and answered with a canned reply.
 * The proxy sockets are the real ProxySocketManager / ProxySocket.
 */

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "test_harness.hpp"
#include "config/game_whitelist.hpp"
#include "bsd/bsd_mitm_service.cpp"

namespace sut = ams::mitm::bsd;
namespace bsdt = ryu_ldn::bsd;

namespace {

/// Program id of the whitelisted game under test.
constexpr std::uint64_t GAME = 0x0100152000022000ULL;
/// Program id of a game that is not whitelisted.
constexpr std::uint64_t OTHER_GAME = 0x0100000000abc000ULL;
/// Program id of this sysmodule itself.
constexpr std::uint64_t OUR_SYSMODULE = 0x4200000000000010ULL;
/// Virtual IP of the host, 10.114.0.1, in Ryujinx format.
constexpr std::uint32_t LDN_HOST = 0x0A720001;  // 10.114.0.1, Ryujinx format
/// Virtual IP of a peer, 10.114.0.2, in Ryujinx format.
constexpr std::uint32_t LDN_PEER = 0x0A720002;  // 10.114.0.2
/// Process id of the default client.
constexpr std::uint64_t PID = 100;

/// The whitelist is a load-once latch: load it once, with one known game.
struct WhitelistFixture {
    WhitelistFixture() {
        ams::fs::g_host_fs.content = "0x0100152000022000\n";
        ryu_ldn::config::LoadWhitelist();
    }
};
/// Loads the whitelist once at static initialization.
WhitelistFixture g_whitelist;

/// What the fake bsd:u saw on its last forwarded call.
struct Seen {
    /// Command id of the forwarded call.
    std::uint32_t command = 0;
    /// Input bytes of the forwarded call.
    std::vector<std::uint8_t> in;
    /// Size of the output buffer of the forwarded call.
    std::size_t out_size = 0;
    /// Dispatch options of the forwarded call.
    host_mitm::Options options{};
    /// Service the call was forwarded to.
    ::Service* service = nullptr;
};
/// What the fake bsd:u saw on its last call.
Seen g_seen;
std::vector<std::uint8_t> g_reply;  ///< bytes copied into the out struct
std::uint32_t g_rc = 0;             ///< Result the fake bsd:u answers with

/**
 * @brief Fake bsd:u dispatch hook; records the call and answers g_rc.
 * @param call Forwarded call.
 * @return Result built from g_rc.
 */
ams::Result fake_bsd(const host_mitm::Call& call) {
    g_seen.command = call.command;
    g_seen.in.clear();
    if (call.in != nullptr) {
        const auto* p = static_cast<const std::uint8_t*>(call.in);
        g_seen.in.assign(p, p + call.in_size);
    }
    g_seen.out_size = call.out_size;
    g_seen.options = *call.options;
    g_seen.service = call.service;
    if (call.out != nullptr && !g_reply.empty()) {
        std::memcpy(
            call.out, g_reply.data(), std::min(call.out_size, g_reply.size()));
    }
    return ams::Result(g_rc);
}

template <class T>
/**
 * @brief Set the bytes the fake bsd:u copies into the output struct.
 * @tparam T Type of the reply struct.
 * @param value Reply to send.
 */
void reply_with(const T& value) {
    g_reply.assign(
        reinterpret_cast<const std::uint8_t*>(&value),
        reinterpret_cast<const std::uint8_t*>(&value) + sizeof(value));
}

/// Two 32-bit values, as returned by some bsd:u calls.
struct Pair {
    /// First value.
    std::int32_t a;
    /// Second value.
    std::int32_t b;
};
/// Three 32-bit values, as returned by some bsd:u calls.
struct Triple {
    /// First value.
    std::int32_t a;
    /// Second value.
    std::int32_t b;
    /// Third value.
    std::uint32_t c;
};

/// Records the sends of the proxy sockets (the LDN server stand-in).
struct Sent {
    /// Number of sends recorded.
    unsigned count = 0;
    /// Destination IP of the last send.
    std::uint32_t dest_ip = 0;
    /// Destination port of the last send.
    std::uint16_t dest_port = 0;
    /// Payload length of the last send.
    std::size_t len = 0;
};
/// Sends recorded since reset().
Sent g_sent;
/**
 * @brief Send callback of the proxy sockets; records the send.
 * @param dest_ip Destination IP.
 * @param dest_port Destination port.
 * @param len Payload length.
 * @return Always true.
 */
bool record_send(
    std::uint32_t,
    std::uint16_t,
    std::uint32_t dest_ip,
    std::uint16_t dest_port,
    bsdt::ProtocolType,
    const void*,
    std::size_t len) {
    g_sent.count++;
    g_sent.dest_ip = dest_ip;
    g_sent.dest_port = dest_port;
    g_sent.len = len;
    return true;
}

/// @brief Reset the hooks, the tracking tables and the proxy manager.
void reset() {
    host_mitm::g_dispatch_hook = fake_bsd;
    host_mitm::g_dispatch_count = 0;
    ams::os::g_host_before_plain_lock = nullptr;
    ams::os::g_host_before_recursive_lock = nullptr;
    g_seen = Seen{};
    g_reply.clear();
    g_rc = 0;
    g_sent = Sent{};
    sut::g_socket_info = {};
    sut::g_mitm_pid_count = {};
    sut::g_abandoned_forward_services = {};
    auto& manager = sut::ProxySocketManager::GetInstance();
    manager.Reset();
    manager.SetLocalIp(LDN_HOST);
    manager.SetSendCallback(record_send);
}

/**
 * @brief Count the calls forwarded to the fake bsd:u.
 * @return Number of dispatches since reset().
 */
unsigned forwarded() {
    return host_mitm::g_dispatch_count;
}

/**
 * @brief Build the process info of a client.
 * @param pid Process id.
 * @param program_id Program id.
 * @return The process info.
 */
ams::sm::MitmProcessInfo client(std::uint64_t pid, std::uint64_t program_id) {
    ams::sm::MitmProcessInfo info{};
    info.process_id.value = pid;
    info.program_id.value = program_id;
    return info;
}

/**
 * @brief Create a service for the whitelisted game.
 * @param pid Process id of the client.
 * @return The new service.
 */
std::unique_ptr<sut::BsdMitmService> make(std::uint64_t pid = PID) {
    return std::make_unique<sut::BsdMitmService>(
        std::make_shared<::Service>(), client(pid, GAME));
}

/**
 * @brief Address of a peer/host of the LDN subnet, the way a game writes it.
 *
 * @param host_ip host-order IPv4 address of the LDN peer
 * @param port host-order port
 * @return address with the fields byte-swapped to the wire order
 */
bsdt::SockAddrIn ldn_addr(std::uint32_t host_ip, std::uint16_t port) {
    bsdt::SockAddrIn a{};
    a.sin_len = sizeof(a);
    a.sin_family = static_cast<std::uint8_t>(bsdt::AddressFamily::Inet);
    a.sin_port = __builtin_bswap16(port);
    a.sin_addr = __builtin_bswap32(host_ip);
    return a;
}

/**
 * @brief Wrap a socket address as an input buffer.
 * @param a Address to wrap (must outlive the buffer).
 * @return The buffer.
 */
ams::sf::InBuffer in_of(const bsdt::SockAddrIn& a) {
    return ams::sf::InBuffer(
        reinterpret_cast<const std::uint8_t*>(&a), sizeof(a));
}

/**
 * @brief Adds a socket straight to the tracking table.
 *
 * @param fd socket descriptor to register
 * @param type socket type recorded
 * @param proto protocol recorded
 * @param is_proxy true when the socket is an LDN proxy socket
 * @param broadcast true when SO_BROADCAST is set
 */
void track(
    std::int32_t fd,
    bsdt::SocketType type,
    bsdt::ProtocolType proto,
    bool is_proxy = false,
    bool broadcast = false) {
    auto* entry = sut::FindOrCreateSocketInfoEntry(fd);
    entry->info = sut::SocketInfo{type, proto, is_proxy, broadcast, 0};
}

/**
 * @brief Track a plain (non-proxy) UDP socket.
 * @param fd Socket descriptor.
 */
void track_udp(std::int32_t fd) {
    track(fd, bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp);
}

/**
 * @brief Binds a tracked UDP socket to an LDN port through the service.
 *
 * @param svc service whose Bind() is called
 * @param fd socket descriptor, tracked as UDP when unknown
 * @param port LDN port to bind
 * @return errno value reported by Bind()
 */
std::int32_t bind_ldn(
    sut::BsdMitmService& svc, std::int32_t fd, std::uint16_t port) {
    if (sut::FindSocketInfoEntry(fd) == nullptr) {
        track_udp(fd);
    }
    const auto addr = ldn_addr(LDN_HOST, port);
    std::int32_t err = -1;
    ams::sf::Out<std::int32_t> out(err);
    svc.Bind(out, fd, in_of(addr));
    return err;
}

/**
 * @brief Look up the proxy socket of a descriptor.
 * @param fd Socket descriptor.
 * @return The proxy socket, or nullptr.
 */
sut::ProxySocket* proxy_of(std::int32_t fd) {
    return sut::ProxySocketManager::GetInstance().GetProxySocket(fd);
}

/**
 * @brief Routes a datagram to the proxy socket bound for `fd`.
 *
 * @param fd descriptor of a bound proxy socket
 * @param text NUL-terminated datagram payload
 * @return true when the manager routed the datagram, false without a proxy
 * socket for fd
 */
bool deliver(std::int32_t fd, const char* text) {
    sut::ProxySocket* p = proxy_of(fd);
    if (p == nullptr) {
        return false;
    }
    const auto local = p->GetLocalAddr();
    return sut::ProxySocketManager::GetInstance().RouteIncomingData(
        LDN_PEER,
        6000,
        local.sin_addr,
        local.GetPort(),
        bsdt::ProtocolType::Udp,
        text,
        std::strlen(text));
}

/**
 * @brief Takes every ephemeral port of a protocol from the manager.
 *
 * @param proto protocol whose ephemeral ports are taken
 */
void exhaust_ports(bsdt::ProtocolType proto) {
    auto& manager = sut::ProxySocketManager::GetInstance();
    while (manager.AllocatePort(proto) != 0) {
    }
}

/// Opens proxy sockets until the manager refuses a new one.
void fill_proxy_table() {
    auto& manager = sut::ProxySocketManager::GetInstance();
    for (std::int32_t fd = 1000;
         manager.CreateProxySocket(
             fd, bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp) != nullptr;
         fd++) {
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// ShouldMitm
// ---------------------------------------------------------------------------

/**
 * @test Verify our own sysmodule is never intercepted.
 */
TEST(our_own_sysmodule_is_never_intercepted) {
    reset();
    ASSERT_FALSE(sut::BsdMitmService::ShouldMitm(client(PID, OUR_SYSMODULE)));
    ASSERT_TRUE(sut::FindMitmPidEntry(PID) == nullptr);
}

/**
 * @test Verify a system program is never intercepted.
 */
TEST(a_system_program_is_never_intercepted) {
    reset();
    ASSERT_FALSE(
        sut::BsdMitmService::ShouldMitm(
            client(PID, 0x0100000000000000ULL - 1)));
    ASSERT_TRUE(sut::FindMitmPidEntry(PID) == nullptr);
}

/**
 * @test Verify a game outside the whitelist is not intercepted and not counted.
 */
TEST(a_game_outside_the_whitelist_is_not_intercepted_and_not_counted) {
    reset();
    ASSERT_FALSE(sut::BsdMitmService::ShouldMitm(client(PID, OTHER_GAME)));
    ASSERT_TRUE(sut::FindMitmPidEntry(PID) == nullptr);
}

/**
 * @test Verify the first session of a pid is skipped and the following ones
 * intercepted.
 */
TEST(the_first_session_of_a_pid_is_skipped_and_the_following_ones_intercepted) {
    reset();
    ASSERT_FALSE(sut::BsdMitmService::ShouldMitm(client(PID, GAME)));
    ASSERT_EQ(sut::FindMitmPidEntry(PID)->count, 1u);
    ASSERT_TRUE(sut::BsdMitmService::ShouldMitm(client(PID, GAME)));
    ASSERT_TRUE(sut::BsdMitmService::ShouldMitm(client(PID, GAME)));
    ASSERT_EQ(sut::FindMitmPidEntry(PID)->count, 3u);
}

/**
 * @test Verify a full pid table falls back to pass through without counting.
 */
TEST(a_full_pid_table_falls_back_to_pass_through_without_counting) {
    reset();
    for (std::uint64_t pid = 1; pid <= sut::MAX_MITM_PIDS; pid++) {
        ASSERT_FALSE(sut::BsdMitmService::ShouldMitm(client(pid, GAME)));
    }
    // A known pid keeps being counted and intercepted...
    ASSERT_TRUE(sut::BsdMitmService::ShouldMitm(client(1, GAME)));
    // ...a new one is refused a slot, and stays out of the table.
    ASSERT_FALSE(sut::BsdMitmService::ShouldMitm(client(1000, GAME)));
    ASSERT_FALSE(sut::BsdMitmService::ShouldMitm(client(1000, GAME)));
    ASSERT_TRUE(sut::FindMitmPidEntry(1000) == nullptr);
}

// ---------------------------------------------------------------------------
// Construction and teardown
// ---------------------------------------------------------------------------

/**
 * @test Verify a session counts in the shared state while it lives.
 */
TEST(a_session_counts_in_the_shared_state_while_it_lives) {
    reset();
    auto& shared = ams::mitm::ldn::SharedState::GetInstance();
    const auto before = shared.GetActiveSessionCount();
    {
        auto svc = make();
        ASSERT_EQ(shared.GetActiveSessionCount(), before + 1);
    }
    ASSERT_EQ(shared.GetActiveSessionCount(), before);
}

/**
 * @test Verify a session without a forward service constructs and tears down.
 */
TEST(a_session_without_a_forward_service_constructs_and_tears_down) {
    reset();
    {
        sut::BsdMitmService svc(
            std::shared_ptr<::Service>(), client(PID, GAME));
    }
    unsigned abandoned = 0;
    for (const auto& slot : sut::g_abandoned_forward_services) {
        abandoned += slot.valid ? 1 : 0;
    }
    ASSERT_EQ(abandoned, 0u);
}

/**
 * @test Verify a session that never registered parks its forward service.
 */
TEST(a_session_that_never_registered_parks_its_forward_service) {
    reset();
    std::weak_ptr<::Service> fwd;
    {
        auto service = std::make_shared<::Service>();
        fwd = service;
        sut::BsdMitmService svc(std::move(service), client(PID, GAME));
    }
    ASSERT_FALSE(fwd.expired());
    unsigned abandoned = 0;
    for (const auto& slot : sut::g_abandoned_forward_services) {
        abandoned += slot.valid ? 1 : 0;
    }
    ASSERT_EQ(abandoned, 1u);
}

/**
 * @test Verify a registered session releases its forward service normally.
 */
TEST(a_registered_session_releases_its_forward_service_normally) {
    reset();
    std::weak_ptr<::Service> fwd;
    {
        auto service = std::make_shared<::Service>();
        fwd = service;
        sut::BsdMitmService svc(std::move(service), client(PID, GAME));
        std::uint64_t out_result = 0;
        ams::sf::Out<std::uint64_t> out(out_result);
        ams::sf::ClientProcessId cp;
        cp.process_id.value = PID;
        bsdt::LibraryConfigData cfg{};
        ASSERT_TRUE(svc.RegisterClient(
                           out, cfg, cp, 0x1000, ams::sf::CopyHandle(7, true))
                        .IsSuccess());
    }
    ASSERT_TRUE(fwd.expired());
    unsigned abandoned = 0;
    for (const auto& slot : sut::g_abandoned_forward_services) {
        abandoned += slot.valid ? 1 : 0;
    }
    ASSERT_EQ(abandoned, 0u);
}

/**
 * @test Verify the last session of a pid frees the abandoned services and its
 * pid entry.
 */
TEST(the_last_session_of_a_pid_frees_the_abandoned_services_and_its_pid_entry) {
    reset();
    std::weak_ptr<::Service> parked;
    {
        auto service = std::make_shared<::Service>();
        parked = service;
        sut::g_abandoned_forward_services[0].service = std::move(service);
        sut::g_abandoned_forward_services[0].valid = true;
    }
    sut::BsdMitmService::ShouldMitm(client(PID, GAME));  // count 1
    {
        auto svc = make();
    }
    ASSERT_TRUE(parked.expired());
    ASSERT_TRUE(sut::FindMitmPidEntry(PID) == nullptr);
}

/**
 * @test Verify a session that is not the last of its pid keeps the abandoned
 * services.
 */
TEST(a_session_that_is_not_the_last_of_its_pid_keeps_the_abandoned_services) {
    reset();
    std::weak_ptr<::Service> parked;
    {
        auto service = std::make_shared<::Service>();
        parked = service;
        sut::g_abandoned_forward_services[0].service = std::move(service);
        sut::g_abandoned_forward_services[0].valid = true;
    }
    sut::BsdMitmService::ShouldMitm(client(PID, GAME));
    sut::BsdMitmService::ShouldMitm(client(PID, GAME));  // count 2
    {
        auto svc = make();
    }
    ASSERT_FALSE(parked.expired());
    ASSERT_EQ(sut::FindMitmPidEntry(PID)->count, 1u);
}

/**
 * @test Verify a session without a pid entry runs the abandoned cleanup as a
 * fail safe.
 */
TEST(a_session_without_a_pid_entry_runs_the_abandoned_cleanup_as_a_fail_safe) {
    reset();
    std::weak_ptr<::Service> parked;
    {
        auto service = std::make_shared<::Service>();
        parked = service;
        sut::g_abandoned_forward_services[0].service = std::move(service);
        sut::g_abandoned_forward_services[0].valid = true;
    }
    {
        auto svc = make();  // never went through ShouldMitm
    }
    ASSERT_TRUE(parked.expired());
}

/**
 * @test Verify the cleanup of an empty abandoned table does nothing.
 */
TEST(the_cleanup_of_an_empty_abandoned_table_does_nothing) {
    reset();
    sut::BsdMitmService::CleanupAbandonedServices();
    for (const auto& slot : sut::g_abandoned_forward_services) {
        ASSERT_FALSE(slot.valid);
    }
}

/**
 * @test Verify the teardown frees a full abandoned table to park its own
 * service.
 */
TEST(the_teardown_frees_a_full_abandoned_table_to_park_its_own_service) {
    reset();
    std::vector<std::weak_ptr<::Service>> parked;
    for (auto& slot : sut::g_abandoned_forward_services) {
        auto service = std::make_shared<::Service>();
        parked.push_back(service);
        slot.service = std::move(service);
        slot.valid = true;
    }
    sut::BsdMitmService::ShouldMitm(client(PID, GAME));
    sut::BsdMitmService::ShouldMitm(
        client(PID, GAME));  // not the last: no PID cleanup
    std::weak_ptr<::Service> own;
    {
        auto service = std::make_shared<::Service>();
        own = service;
        sut::BsdMitmService svc(std::move(service), client(PID, GAME));
    }
    for (const auto& w : parked) {
        ASSERT_TRUE(w.expired());
    }
    ASSERT_FALSE(own.expired());
    unsigned abandoned = 0;
    for (const auto& slot : sut::g_abandoned_forward_services) {
        abandoned += slot.valid ? 1 : 0;
    }
    ASSERT_EQ(abandoned, 1u);
}

namespace {
/// Number of locks of the abandoned-services table seen so far.
unsigned g_abandoned_locks = 0;
/// Stages what a concurrent session would do between the cleanup and the
/// retry: on the third lock of the abandoned table (insert, cleanup, retry)
/// it refills every slot.
/// @param mutex Mutex about to be locked; only the abandoned table's counts.
void refill_on_third_lock(const void* mutex) {
    if (mutex != &sut::g_abandoned_services_mutex) {
        return;
    }
    if (++g_abandoned_locks != 3) {
        return;
    }
    for (auto& slot : sut::g_abandoned_forward_services) {
        slot.service = std::make_shared<::Service>();
        slot.valid = true;
    }
}
}  // namespace

/**
 * @test Verify a table refilled during cleanup leaves service to base
 * destructor.
 */
TEST(a_table_refilled_during_cleanup_leaves_service_to_base_destructor) {
    reset();
    for (auto& slot : sut::g_abandoned_forward_services) {
        slot.service = std::make_shared<::Service>();
        slot.valid = true;
    }
    sut::BsdMitmService::ShouldMitm(client(PID, GAME));
    sut::BsdMitmService::ShouldMitm(client(PID, GAME));
    g_abandoned_locks = 0;
    ams::os::g_host_before_plain_lock = refill_on_third_lock;
    std::weak_ptr<::Service> own;
    {
        auto service = std::make_shared<::Service>();
        own = service;
        sut::BsdMitmService svc(std::move(service), client(PID, GAME));
    }
    ams::os::g_host_before_plain_lock = nullptr;
    ASSERT_EQ(g_abandoned_locks, 3u);
    ASSERT_TRUE(own.expired());
    unsigned abandoned = 0;
    for (const auto& slot : sut::g_abandoned_forward_services) {
        abandoned += slot.valid ? 1 : 0;
    }
    ASSERT_EQ(abandoned, sut::MAX_ABANDONED_SERVICES);
}

/**
 * @test Verify the teardown forgets the sockets of its session and closes their
 * proxies.
 */
TEST(the_teardown_forgets_the_sockets_of_its_session_and_closes_their_proxies) {
    reset();
    std::int32_t fd_proxy = 11;
    std::int32_t fd_plain = 12;
    std::int32_t fd_other = 13;
    {
        auto svc = make();
        auto other = make(PID + 1);
        reply_with(Pair{0, fd_proxy});
        std::int32_t e = 0, fd = 0;
        ams::sf::Out<std::int32_t> oe(e), ofd(fd);
        svc->Socket(oe, ofd, 2, 2, 0);
        reply_with(Pair{0, fd_plain});
        svc->Socket(oe, ofd, 2, 2, 0);
        reply_with(Pair{0, fd_other});
        other->Socket(oe, ofd, 2, 2, 0);
        ASSERT_EQ(bind_ldn(*svc, fd_proxy, 5000), 0);
        ASSERT_TRUE(proxy_of(fd_proxy) != nullptr);
        ASSERT_TRUE(
            sut::FindSocketInfoEntry(fd_other)->info.session_id !=
            sut::FindSocketInfoEntry(fd_proxy)->info.session_id);
        {
            auto gone = make(PID + 2);
        }
        svc.reset();
        ASSERT_TRUE(proxy_of(fd_proxy) == nullptr);
        ASSERT_TRUE(sut::FindSocketInfoEntry(fd_proxy) == nullptr);
        ASSERT_TRUE(sut::FindSocketInfoEntry(fd_plain) == nullptr);
        ASSERT_TRUE(sut::FindSocketInfoEntry(fd_other) != nullptr);
    }
}

// ---------------------------------------------------------------------------
// Registration and plain forwarding
// ---------------------------------------------------------------------------

/**
 * @test Verify register client forwards the config the handle and the client
 * pid.
 */
TEST(register_client_forwards_the_config_the_handle_and_the_client_pid) {
    reset();
    auto svc = make();
    std::uint64_t out_result = 0;
    ams::sf::Out<std::uint64_t> out(out_result);
    ams::sf::ClientProcessId cp;
    cp.process_id.value = PID;
    bsdt::LibraryConfigData cfg{};
    cfg.version = 7;
    reply_with(std::uint64_t{0xABCD});
    const auto rc =
        svc->RegisterClient(out, cfg, cp, 0x4000, ams::sf::CopyHandle(9, true));
    ASSERT_TRUE(rc.IsSuccess());
    ASSERT_EQ(g_seen.command, 0u);
    ASSERT_TRUE(g_seen.options.in_send_pid);
    ASSERT_EQ(g_seen.options.in_num_handles, 1u);
    ASSERT_EQ(g_seen.options.in_handles[0], 9u);
    ASSERT_EQ(g_seen.options.override_pid, PID);
    ASSERT_EQ(out_result, 0xABCDu);
    std::uint64_t tmem_size = 0;
    std::memcpy(
        &tmem_size, g_seen.in.data() + sizeof(bsdt::LibraryConfigData) + 8, 8);
    ASSERT_EQ(tmem_size, 0x4000u);
}

/**
 * @test Verify register client invalid handle is forwarded and failure not
 * registered.
 */
TEST(register_client_invalid_handle_is_forwarded_and_failure_not_registered) {
    reset();
    auto svc = make();
    std::uint64_t out_result = 0;
    ams::sf::Out<std::uint64_t> out(out_result);
    ams::sf::ClientProcessId cp;
    cp.process_id.value = PID;
    bsdt::LibraryConfigData cfg{};
    g_rc = 0x1234;
    const auto rc = svc->RegisterClient(
        out, cfg, cp, 0x4000, ams::sf::CopyHandle(INVALID_HANDLE, false));
    ASSERT_EQ(rc.GetValue(), 0x1234u);
    ASSERT_EQ(g_seen.options.in_handles[0], INVALID_HANDLE);
}

/**
 * @test Verify register client shared forwards the pid and marks session
 * registered.
 */
TEST(register_client_shared_forwards_the_pid_and_marks_session_registered) {
    reset();
    auto svc = make();
    std::uint64_t out_result = 0;
    ams::sf::Out<std::uint64_t> out(out_result);
    ams::sf::ClientProcessId cp;
    cp.process_id.value = PID;
    bsdt::LibraryConfigData cfg{};
    reply_with(std::uint64_t{55});
    ASSERT_TRUE(svc->RegisterClientShared(out, cfg, cp, 0x2000).IsSuccess());
    ASSERT_EQ(g_seen.command, 33u);
    ASSERT_TRUE(g_seen.options.in_send_pid);
    ASSERT_EQ(g_seen.options.override_pid, PID);
    ASSERT_EQ(out_result, 55u);
    g_rc = 5;
    auto other = make(PID + 1);
    ASSERT_EQ(other->RegisterClientShared(out, cfg, cp, 0x2000).GetValue(), 5u);
}

/**
 * @test Verify start monitoring forwards the pid and returns the errno.
 */
TEST(start_monitoring_forwards_the_pid_and_returns_the_errno) {
    reset();
    auto svc = make();
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    reply_with(std::int32_t{3});
    ASSERT_TRUE(svc->StartMonitoring(oe, 0x77).IsSuccess());
    ASSERT_EQ(g_seen.command, 1u);
    std::uint64_t pid = 0;
    std::memcpy(&pid, g_seen.in.data(), 8);
    ASSERT_EQ(pid, 0x77u);
    ASSERT_EQ(e, 3);
}

/**
 * @test Verify socket tracks a new tcp or udp socket with the protocol the type
 * implies.
 */
TEST(socket_tracks_a_new_tcp_or_udp_socket_with_the_protocol_the_type_implies) {
    reset();
    auto svc = make();
    std::int32_t e = -1, fd = -1;
    ams::sf::Out<std::int32_t> oe(e), ofd(fd);
    reply_with(Pair{0, 20});
    ASSERT_TRUE(svc->Socket(oe, ofd, 2, 1, 0).IsSuccess());
    ASSERT_EQ(g_seen.command, 2u);
    ASSERT_EQ(fd, 20);
    ASSERT_TRUE(
        sut::FindSocketInfoEntry(20)->info.type == bsdt::SocketType::Stream);
    ASSERT_TRUE(
        sut::FindSocketInfoEntry(20)->info.protocol == bsdt::ProtocolType::Tcp);
    reply_with(Pair{0, 21});
    svc->Socket(oe, ofd, 2, 2, 0);
    ASSERT_TRUE(
        sut::FindSocketInfoEntry(21)->info.protocol == bsdt::ProtocolType::Udp);
    reply_with(Pair{0, 22});
    svc->Socket(oe, ofd, 2, 3, 0);
    ASSERT_TRUE(
        sut::FindSocketInfoEntry(22)->info.protocol ==
        bsdt::ProtocolType::Unspecified);
    reply_with(Pair{0, 23});
    svc->Socket(oe, ofd, 2, 2, 17);
    ASSERT_TRUE(
        sut::FindSocketInfoEntry(23)->info.protocol == bsdt::ProtocolType::Udp);
    ASSERT_FALSE(sut::FindSocketInfoEntry(23)->info.is_proxy);
}

/**
 * @test Verify socket does not track a failed creation.
 */
TEST(socket_does_not_track_a_failed_creation) {
    reset();
    auto svc = make();
    std::int32_t e = -1, fd = -1;
    ams::sf::Out<std::int32_t> oe(e), ofd(fd);
    reply_with(Pair{97, -1});
    svc->Socket(oe, ofd, 2, 2, 0);
    ASSERT_EQ(e, 97);
    reply_with(Pair{0, 30});
    g_rc = 9;
    ASSERT_EQ(svc->Socket(oe, ofd, 2, 2, 0).GetValue(), 9u);
    ASSERT_TRUE(sut::FindSocketInfoEntry(30) == nullptr);
}

/**
 * @test Verify socket with a full tracking table still returns the socket.
 */
TEST(socket_with_a_full_tracking_table_still_returns_the_socket) {
    reset();
    auto svc = make();
    for (std::size_t i = 0; i < sut::MAX_SOCKET_INFO; i++) {
        track_udp(static_cast<std::int32_t>(i));
    }
    std::int32_t e = -1, fd = -1;
    ams::sf::Out<std::int32_t> oe(e), ofd(fd);
    reply_with(Pair{0, 500});
    ASSERT_TRUE(svc->Socket(oe, ofd, 2, 2, 0).IsSuccess());
    ASSERT_EQ(fd, 500);
    ASSERT_TRUE(sut::FindSocketInfoEntry(500) == nullptr);
}

/**
 * @test Verify socket exempt tracks like socket and forwards command 3.
 */
TEST(socket_exempt_tracks_like_socket_and_forwards_command_3) {
    reset();
    auto svc = make();
    std::int32_t e = -1, fd = -1;
    ams::sf::Out<std::int32_t> oe(e), ofd(fd);
    reply_with(Pair{0, 40});
    ASSERT_TRUE(svc->SocketExempt(oe, ofd, 2, 1, 0).IsSuccess());
    ASSERT_EQ(g_seen.command, 3u);
    ASSERT_TRUE(
        sut::FindSocketInfoEntry(40)->info.protocol == bsdt::ProtocolType::Tcp);
    reply_with(Pair{0, 41});
    svc->SocketExempt(oe, ofd, 2, 2, 0);
    ASSERT_TRUE(
        sut::FindSocketInfoEntry(41)->info.protocol == bsdt::ProtocolType::Udp);
    reply_with(Pair{0, 42});
    svc->SocketExempt(oe, ofd, 2, 2, 6);
    ASSERT_TRUE(
        sut::FindSocketInfoEntry(42)->info.protocol == bsdt::ProtocolType::Tcp);
    reply_with(Pair{5, -1});
    svc->SocketExempt(oe, ofd, 2, 2, 0);
    ASSERT_EQ(e, 5);
    ASSERT_EQ(forwarded(), 4u);
}

/**
 * @test Verify socket exempt with a full tracking table still returns the
 * socket.
 */
TEST(socket_exempt_with_a_full_tracking_table_still_returns_the_socket) {
    reset();
    auto svc = make();
    for (std::size_t i = 0; i < sut::MAX_SOCKET_INFO; i++) {
        track_udp(static_cast<std::int32_t>(i));
    }
    std::int32_t e = -1, fd = -1;
    ams::sf::Out<std::int32_t> oe(e), ofd(fd);
    reply_with(Pair{0, 500});
    ASSERT_TRUE(svc->SocketExempt(oe, ofd, 2, 2, 0).IsSuccess());
    ASSERT_EQ(fd, 500);
    ASSERT_TRUE(sut::FindSocketInfoEntry(500) == nullptr);
}

/**
 * @test Verify open forwards the path buffer and returns the fd.
 */
TEST(open_forwards_the_path_buffer_and_returns_the_fd) {
    reset();
    auto svc = make();
    std::int32_t e = -1, fd = -1;
    ams::sf::Out<std::int32_t> oe(e), ofd(fd);
    const std::uint8_t path[] = "/dev/bpf";
    reply_with(Pair{0, 9});
    ASSERT_TRUE(
        svc->Open(oe, ofd, ams::sf::InBuffer(path, sizeof(path))).IsSuccess());
    ASSERT_EQ(g_seen.command, 4u);
    ASSERT_TRUE(g_seen.options.buffers[0].ptr == path);
    ASSERT_EQ(g_seen.options.buffers[0].size, sizeof(path));
    ASSERT_EQ(fd, 9);
}

/**
 * @test Verify close forgets the socket and closes its proxy.
 */
TEST(close_forgets_the_socket_and_closes_its_proxy) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 50, 5000), 0);
    track_udp(51);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    reply_with(std::int32_t{0});
    ASSERT_TRUE(svc->Close(oe, 50).IsSuccess());
    ASSERT_EQ(g_seen.command, 26u);
    ASSERT_TRUE(proxy_of(50) == nullptr);
    ASSERT_TRUE(sut::FindSocketInfoEntry(50) == nullptr);
    svc->Close(oe, 51);
    ASSERT_TRUE(sut::FindSocketInfoEntry(51) == nullptr);
    svc->Close(oe, 52);  // never tracked: only forwarded
    ASSERT_EQ(forwarded(), 3u);
}

/**
 * @test Verify close of a proxy flag without a proxy socket just forgets the
 * entry.
 */
TEST(close_of_a_proxy_flag_without_a_proxy_socket_just_forgets_the_entry) {
    reset();
    auto svc = make();
    track(60, bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp, true);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    svc->Close(oe, 60);
    ASSERT_TRUE(sut::FindSocketInfoEntry(60) == nullptr);
}

/**
 * @test Verify plain commands are forwarded with their command id and return
 * the reply.
 */
TEST(plain_commands_are_forwarded_with_their_command_id_and_return_the_reply) {
    reset();
    auto svc = make();
    std::int32_t e = -1, f = -1;
    ams::sf::Out<std::int32_t> oe(e), of(f);
    std::uint8_t buf[32] = {};
    ams::sf::OutAutoSelectBuffer out_buf(buf, sizeof(buf));
    ams::sf::InBuffer in_buf(buf, sizeof(buf));

    reply_with(Pair{4, 8});
    ASSERT_TRUE(svc->Accept(oe, of, 3, out_buf).IsSuccess());
    ASSERT_EQ(g_seen.command, 12u);
    ASSERT_EQ(e, 4);
    ASSERT_EQ(f, 8);
    ASSERT_TRUE(g_seen.options.buffers[0].ptr == buf);

    reply_with(std::int32_t{6});
    svc->Listen(oe, 3, 5);
    ASSERT_EQ(g_seen.command, 18u);
    ASSERT_EQ(e, 6);

    reply_with(std::int32_t{7});
    svc->Shutdown(oe, 3, 2);
    ASSERT_EQ(g_seen.command, 22u);
    ASSERT_EQ(e, 7);

    reply_with(std::int32_t{8});
    svc->ShutdownAllSockets(oe, 99, 1);
    ASSERT_EQ(g_seen.command, 23u);
    ASSERT_EQ(e, 8);

    reply_with(Pair{1, 2});
    svc->DuplicateSocket(oe, of, 3, 99);
    ASSERT_EQ(g_seen.command, 27u);
    ASSERT_EQ(f, 2);

    reply_with(std::int32_t{9});
    svc->GetResourceStatistics(oe, out_buf, 99);
    ASSERT_EQ(g_seen.command, 28u);
    ASSERT_EQ(e, 9);

    reply_with(Pair{0, 77});
    svc->EventFd(oe, of, 1, 0);
    ASSERT_EQ(g_seen.command, 31u);
    ASSERT_EQ(f, 77);

    reply_with(std::int32_t{10});
    svc->RegisterResourceStatisticsName(oe, 99, in_buf);
    ASSERT_EQ(g_seen.command, 32u);
    ASSERT_EQ(e, 10);

    reply_with(std::int32_t{11});
    svc->Sysctl(oe, in_buf, in_buf, out_buf, in_buf);
    ASSERT_EQ(g_seen.command, 7u);
    ASSERT_EQ(e, 11);
    ASSERT_EQ(g_seen.options.buffers[2].size, sizeof(buf));
}

/**
 * @test Verify multi message commands are forwarded for plain and proxy
 * sockets.
 */
TEST(multi_message_commands_are_forwarded_for_plain_and_proxy_sockets) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 70, 5000), 0);
    track_udp(71);
    std::int32_t e = -1, c = -1;
    ams::sf::Out<std::int32_t> oe(e), oc(c);
    std::uint8_t buf[16] = {};
    reply_with(Pair{0, 3});
    for (std::int32_t fd : {70, 71, 72}) {
        ASSERT_TRUE(svc->RecvMMsg(
                           oe,
                           oc,
                           fd,
                           2,
                           0,
                           0,
                           ams::sf::OutAutoSelectBuffer(buf, sizeof(buf)))
                        .IsSuccess());
        ASSERT_EQ(g_seen.command, 29u);
        ASSERT_EQ(c, 3);
        ASSERT_TRUE(
            svc->SendMMsg(oe, oc, fd, 2, 0, ams::sf::InBuffer(buf, sizeof(buf)))
                .IsSuccess());
        ASSERT_EQ(g_seen.command, 30u);
        ASSERT_EQ(c, 3);
    }
    ASSERT_EQ(forwarded(), 6u);
}

// ---------------------------------------------------------------------------
// Bind
// ---------------------------------------------------------------------------

/**
 * @test Verify bind to an ldn address creates a proxy socket and is not
 * forwarded.
 */
TEST(bind_to_an_ldn_address_creates_a_proxy_socket_and_is_not_forwarded) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 100, 5000), 0);
    ASSERT_EQ(forwarded(), 0u);
    ASSERT_TRUE(sut::FindSocketInfoEntry(100)->info.is_proxy);
    sut::ProxySocket* p = proxy_of(100);
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(p->GetLocalAddr().GetPort(), 5000u);
    ASSERT_TRUE(p->GetState() == sut::ProxySocketState::Bound);
}

/**
 * @test Verify bind to port zero allocates an ephemeral port.
 */
TEST(bind_to_port_zero_allocates_an_ephemeral_port) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 101, 0), 0);
    ASSERT_TRUE(proxy_of(101)->GetLocalAddr().GetPort() != 0);
}

/**
 * @test Verify bind applies the so broadcast saved before the proxy existed.
 */
TEST(bind_applies_the_so_broadcast_saved_before_the_proxy_existed) {
    reset();
    auto svc = make();
    track(102, bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp, false, true);
    const auto addr = ldn_addr(LDN_HOST, 5001);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    svc->Bind(oe, 102, in_of(addr));
    ASSERT_EQ(e, 0);
    ASSERT_TRUE(proxy_of(102)->IsBroadcastEnabled());
}

/**
 * @test Verify bind to inaddr any binds proxy to local ldn ip and real socket
 * too.
 */
TEST(bind_to_inaddr_any_binds_proxy_to_local_ldn_ip_and_real_socket_too) {
    reset();
    auto svc = make();
    track_udp(103);
    const auto addr = ldn_addr(0, 5002);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    reply_with(std::int32_t{0});
    ASSERT_TRUE(svc->Bind(oe, 103, in_of(addr)).IsSuccess());
    ASSERT_EQ(e, 0);
    ASSERT_EQ(proxy_of(103)->GetLocalAddr().sin_addr, LDN_HOST);
    ASSERT_EQ(g_seen.command, 13u);
    ASSERT_EQ(forwarded(), 1u);
    ASSERT_TRUE(sut::FindSocketInfoEntry(103)->info.is_proxy);
}

/**
 * @test Verify bind to inaddr any keeps the proxy going when the real bind
 * fails.
 */
TEST(bind_to_inaddr_any_keeps_the_proxy_going_when_the_real_bind_fails) {
    reset();
    auto svc = make();
    track_udp(104);
    track_udp(105);
    const auto addr = ldn_addr(0, 5003);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    g_rc = 3;
    ASSERT_TRUE(svc->Bind(oe, 104, in_of(addr)).IsSuccess());
    ASSERT_EQ(e, 0);
    g_rc = 0;
    reply_with(std::int32_t{98});
    const auto addr2 = ldn_addr(0, 5004);
    ASSERT_TRUE(svc->Bind(oe, 105, in_of(addr2)).IsSuccess());
    ASSERT_EQ(e, 0);
    ASSERT_TRUE(proxy_of(104) != nullptr);
    ASSERT_TRUE(proxy_of(105) != nullptr);
}

/**
 * @test Verify bind to inaddr any without a local ldn ip keeps inaddr any on
 * the proxy.
 */
TEST(bind_to_inaddr_any_without_a_local_ldn_ip_keeps_inaddr_any_on_the_proxy) {
    reset();
    auto svc = make();
    sut::ProxySocketManager::GetInstance().SetLocalIp(0);
    track_udp(106);
    const auto addr = ldn_addr(0, 5005);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    svc->Bind(oe, 106, in_of(addr));
    ASSERT_EQ(e, 0);
    ASSERT_EQ(proxy_of(106)->GetLocalAddr().sin_addr, 0u);
}

/**
 * @test Verify bind of an untracked socket is forwarded.
 */
TEST(bind_of_an_untracked_socket_is_forwarded) {
    reset();
    auto svc = make();
    const auto addr = ldn_addr(LDN_HOST, 5006);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    reply_with(std::int32_t{13});
    ASSERT_TRUE(svc->Bind(oe, 107, in_of(addr)).IsSuccess());
    ASSERT_EQ(g_seen.command, 13u);
    ASSERT_EQ(e, 13);
    ASSERT_TRUE(proxy_of(107) == nullptr);
}

/**
 * @test Verify bind of a foreign short or missing address is forwarded.
 */
TEST(bind_of_a_foreign_short_or_missing_address_is_forwarded) {
    reset();
    auto svc = make();
    track_udp(108);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    auto outside =
        ldn_addr(0xC0A80001, 5007);  // 192.168.0.1: neither LDN nor any
    svc->Bind(oe, 108, in_of(outside));
    ASSERT_EQ(forwarded(), 1u);
    auto inet6 = ldn_addr(LDN_HOST, 5007);
    inet6.sin_family = static_cast<std::uint8_t>(bsdt::AddressFamily::Inet6);
    svc->Bind(oe, 108, in_of(inet6));
    ASSERT_EQ(forwarded(), 2u);
    const std::uint8_t few[4] = {};
    svc->Bind(oe, 108, ams::sf::InBuffer(few, sizeof(few)));
    ASSERT_EQ(forwarded(), 3u);
    svc->Bind(oe, 108, ams::sf::InBuffer(nullptr, sizeof(bsdt::SockAddrIn)));
    ASSERT_EQ(forwarded(), 4u);
    ASSERT_TRUE(proxy_of(108) == nullptr);
}

/**
 * @test Verify bind reports enomem when the proxy table is full.
 */
TEST(bind_reports_enomem_when_the_proxy_table_is_full) {
    reset();
    auto svc = make();
    fill_proxy_table();
    ASSERT_EQ(
        bind_ldn(*svc, 109, 5008),
        static_cast<std::int32_t>(bsdt::BsdErrno::NoMem));
    ASSERT_EQ(forwarded(), 0u);
}

/**
 * @test Verify bind reports eaddrinuse when no ephemeral port is left.
 */
TEST(bind_reports_eaddrinuse_when_no_ephemeral_port_is_left) {
    reset();
    auto svc = make();
    exhaust_ports(bsdt::ProtocolType::Udp);
    ASSERT_EQ(
        bind_ldn(*svc, 110, 0),
        static_cast<std::int32_t>(bsdt::BsdErrno::AddrInUse));
}

/**
 * @test Verify bind reports eaddrinuse when the port is reserved.
 */
TEST(bind_reports_eaddrinuse_when_the_port_is_reserved) {
    reset();
    auto svc = make();
    ASSERT_TRUE(
        sut::ProxySocketManager::GetInstance().ReservePort(
            50000, bsdt::ProtocolType::Udp));
    ASSERT_EQ(
        bind_ldn(*svc, 111, 50000),
        static_cast<std::int32_t>(bsdt::BsdErrno::AddrInUse));
}

/**
 * @test Verify a second bind of the same socket is refused by the proxy.
 */
TEST(a_second_bind_of_the_same_socket_is_refused_by_the_proxy) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 112, 5010), 0);
    ASSERT_EQ(
        bind_ldn(*svc, 112, 5011),
        static_cast<std::int32_t>(bsdt::BsdErrno::Inval));
}

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------

/**
 * @test Verify connect of a udp socket to an ldn peer creates binds and
 * connects a proxy.
 */
TEST(
    connect_of_a_udp_socket_to_an_ldn_peer_creates_binds_and_connects_a_proxy) {
    reset();
    auto svc = make();
    track_udp(120);
    const auto peer = ldn_addr(LDN_PEER, 6000);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    ASSERT_TRUE(svc->Connect(oe, 120, in_of(peer)).IsSuccess());
    ASSERT_EQ(e, 0);
    ASSERT_EQ(forwarded(), 0u);
    sut::ProxySocket* p = proxy_of(120);
    ASSERT_TRUE(p != nullptr);
    ASSERT_TRUE(p->GetState() == sut::ProxySocketState::Connected);
    ASSERT_TRUE(p->GetLocalAddr().GetPort() != 0);
    ASSERT_EQ(p->GetRemoteAddr().GetPort(), 6000u);
    ASSERT_TRUE(sut::FindSocketInfoEntry(120)->info.is_proxy);
}

/**
 * @test Verify connect of a bound proxy keeps its local port.
 */
TEST(connect_of_a_bound_proxy_keeps_its_local_port) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 121, 5100), 0);
    const auto peer = ldn_addr(LDN_PEER, 6000);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    svc->Connect(oe, 121, in_of(peer));
    ASSERT_EQ(e, 0);
    ASSERT_EQ(proxy_of(121)->GetLocalAddr().GetPort(), 5100u);
    ASSERT_TRUE(proxy_of(121)->GetState() == sut::ProxySocketState::Connected);
}

/**
 * @test Verify connect of a tcp socket without a server link reports
 * enetunreach.
 */
TEST(connect_of_a_tcp_socket_without_a_server_link_reports_enetunreach) {
    reset();
    auto svc = make();
    track(122, bsdt::SocketType::Stream, bsdt::ProtocolType::Tcp);
    const auto peer = ldn_addr(LDN_PEER, 6000);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    ASSERT_TRUE(svc->Connect(oe, 122, in_of(peer)).IsSuccess());
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::NetUnreach));
    ASSERT_FALSE(sut::FindSocketInfoEntry(122)->info.is_proxy);
}

/**
 * @test Verify connect of a closed proxy reports ebadf.
 */
TEST(connect_of_a_closed_proxy_reports_ebadf) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 123, 5101), 0);
    proxy_of(123)->Close();
    const auto peer = ldn_addr(LDN_PEER, 6000);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    svc->Connect(oe, 123, in_of(peer));
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::BadF));
}

/**
 * @test Verify connect forwards untracked foreign short and missing addresses.
 */
TEST(connect_forwards_untracked_foreign_short_and_missing_addresses) {
    reset();
    auto svc = make();
    track_udp(124);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    const auto peer = ldn_addr(LDN_PEER, 6000);
    reply_with(std::int32_t{111});
    svc->Connect(oe, 125, in_of(peer));  // untracked
    ASSERT_EQ(g_seen.command, 14u);
    ASSERT_EQ(e, 111);
    const auto outside = ldn_addr(0xC0A80001, 80);
    svc->Connect(oe, 124, in_of(outside));
    const std::uint8_t few[4] = {};
    svc->Connect(oe, 124, ams::sf::InBuffer(few, sizeof(few)));
    svc->Connect(oe, 124, ams::sf::InBuffer(nullptr, sizeof(bsdt::SockAddrIn)));
    auto inet6 = peer;
    inet6.sin_family = static_cast<std::uint8_t>(bsdt::AddressFamily::Inet6);
    svc->Connect(oe, 124, in_of(inet6));
    ASSERT_EQ(forwarded(), 5u);
    ASSERT_TRUE(proxy_of(124) == nullptr);
}

/**
 * @test Verify connect reports enomem when the proxy table is full.
 */
TEST(connect_reports_enomem_when_the_proxy_table_is_full) {
    reset();
    auto svc = make();
    track_udp(126);
    fill_proxy_table();
    const auto peer = ldn_addr(LDN_PEER, 6000);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    svc->Connect(oe, 126, in_of(peer));
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::NoMem));
}

/**
 * @test Verify connect reports eaddrinuse when no ephemeral port is left.
 */
TEST(connect_reports_eaddrinuse_when_no_ephemeral_port_is_left) {
    reset();
    auto svc = make();
    track_udp(127);
    exhaust_ports(bsdt::ProtocolType::Udp);
    const auto peer = ldn_addr(LDN_PEER, 6000);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    svc->Connect(oe, 127, in_of(peer));
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::AddrInUse));
}

namespace {
/// Descriptor the staged concurrent thread binds.
std::int32_t g_race_fd = 0;
/// True while the concurrent bind is waiting to fire.
bool g_race_armed = false;
/// Number of proxy-manager locks seen while armed.
unsigned g_race_locks = 0;
/// Stages a concurrent IPC thread that binds the very socket a Connect or a
/// SendTo is about to create the proxy of: the creation call then returns the
/// already bound proxy. Fires on the lock of CreateProxySocket (the second
/// manager lock of the call, after GetProxySocket).
void bind_the_proxy_first() {
    if (!g_race_armed || ++g_race_locks != 2) {
        return;
    }
    g_race_armed = false;
    auto& manager = sut::ProxySocketManager::GetInstance();
    sut::ProxySocket* p = manager.CreateProxySocket(
        g_race_fd, bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp);
    p->Bind(ldn_addr(LDN_HOST, 5300));
}
}  // namespace

/**
 * @test Verify connect reports error and releases port when auto bind is
 * refused.
 */
TEST(connect_reports_error_and_releases_port_when_auto_bind_is_refused) {
    reset();
    auto svc = make();
    track_udp(128);
    g_race_fd = 128;
    g_race_locks = 0;
    g_race_armed = true;
    ams::os::g_host_before_recursive_lock = bind_the_proxy_first;
    const auto peer = ldn_addr(LDN_PEER, 6000);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    const auto before =
        sut::ProxySocketManager::GetInstance().GetAvailablePortCount(
            bsdt::ProtocolType::Udp);
    svc->Connect(oe, 128, in_of(peer));
    ams::os::g_host_before_recursive_lock = nullptr;
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::Inval));
    ASSERT_EQ(
        sut::ProxySocketManager::GetInstance().GetAvailablePortCount(
            bsdt::ProtocolType::Udp),
        before);
    ASSERT_FALSE(sut::FindSocketInfoEntry(128)->info.is_proxy);
}

// ---------------------------------------------------------------------------
// Send / SendTo / Write
// ---------------------------------------------------------------------------

/**
 * @test Verify sendto ldn peer from plain socket creates a proxy and sends
 * through it.
 */
TEST(sendto_ldn_peer_from_plain_socket_creates_a_proxy_and_sends_through_it) {
    reset();
    auto svc = make();
    track_udp(130);
    const auto peer = ldn_addr(LDN_PEER, 6000);
    const std::uint8_t data[5] = {1, 2, 3, 4, 5};
    std::int32_t e = -1, n = -1;
    ams::sf::Out<std::int32_t> oe(e), on(n);
    ASSERT_TRUE(svc->SendTo(
                       oe,
                       on,
                       130,
                       0,
                       ams::sf::InBuffer(data, sizeof(data)),
                       in_of(peer))
                    .IsSuccess());
    ASSERT_EQ(e, 0);
    ASSERT_EQ(n, 5);
    ASSERT_EQ(forwarded(), 0u);
    ASSERT_EQ(g_sent.count, 1u);
    ASSERT_EQ(g_sent.dest_ip, LDN_PEER);
    ASSERT_EQ(g_sent.dest_port, 6000u);
    ASSERT_TRUE(sut::FindSocketInfoEntry(130)->info.is_proxy);
    ASSERT_TRUE(proxy_of(130)->GetState() == sut::ProxySocketState::Bound);
}

/**
 * @test Verify sendto through an existing proxy does not create another.
 */
TEST(sendto_through_an_existing_proxy_does_not_create_another) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 131, 5200), 0);
    sut::FindSocketInfoEntry(131)->info.is_proxy =
        false;  // tracked as plain, proxy already there
    const auto peer = ldn_addr(LDN_PEER, 6000);
    const std::uint8_t data[2] = {1, 2};
    std::int32_t e = -1, n = -1;
    ams::sf::Out<std::int32_t> oe(e), on(n);
    svc->SendTo(
        oe, on, 131, 0, ams::sf::InBuffer(data, sizeof(data)), in_of(peer));
    ASSERT_EQ(n, 2);
    ASSERT_EQ(proxy_of(131)->GetLocalAddr().GetPort(), 5200u);
    ASSERT_TRUE(sut::FindSocketInfoEntry(131)->info.is_proxy);
}

/**
 * @test Verify sendto reports the proxy error as errno and zero bytes.
 */
TEST(sendto_reports_the_proxy_error_as_errno_and_zero_bytes) {
    reset();
    auto svc = make();
    track_udp(132);
    sut::ProxySocketManager::GetInstance().SetSendCallback(nullptr);
    const auto peer = ldn_addr(LDN_PEER, 6000);
    const std::uint8_t data[2] = {1, 2};
    std::int32_t e = -1, n = -1;
    ams::sf::Out<std::int32_t> oe(e), on(n);
    svc->SendTo(
        oe, on, 132, 0, ams::sf::InBuffer(data, sizeof(data)), in_of(peer));
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::NetUnreach));
    ASSERT_EQ(n, 0);
}

/**
 * @test Verify sendto reports enetunreach without a proxy or with a short
 * address.
 */
TEST(sendto_reports_enetunreach_without_a_proxy_or_with_a_short_address) {
    reset();
    auto svc = make();
    const auto peer = ldn_addr(LDN_PEER, 6000);
    const std::uint8_t data[2] = {1, 2};
    std::int32_t e = -1, n = -1;
    ams::sf::Out<std::int32_t> oe(e), on(n);
    svc->SendTo(
        oe,
        on,
        133,
        0,
        ams::sf::InBuffer(data, sizeof(data)),
        in_of(peer));  // untracked
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::NetUnreach));
    ASSERT_EQ(n, 0);
    ASSERT_EQ(bind_ldn(*svc, 134, 5201), 0);
    const std::uint8_t few[4] = {};
    e = -1;
    svc->SendTo(
        oe,
        on,
        134,
        0,
        ams::sf::InBuffer(data, sizeof(data)),
        ams::sf::InBuffer(few, sizeof(few)));
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::NetUnreach));
    ASSERT_EQ(forwarded(), 0u);
}

/**
 * @test Verify sendto with the proxy table full reports enetunreach.
 */
TEST(sendto_with_the_proxy_table_full_reports_enetunreach) {
    reset();
    auto svc = make();
    track_udp(135);
    fill_proxy_table();
    const auto peer = ldn_addr(LDN_PEER, 6000);
    const std::uint8_t data[2] = {1, 2};
    std::int32_t e = -1, n = -1;
    ams::sf::Out<std::int32_t> oe(e), on(n);
    svc->SendTo(
        oe, on, 135, 0, ams::sf::InBuffer(data, sizeof(data)), in_of(peer));
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::NetUnreach));
}

/**
 * @test Verify sendto without ephemeral port leaves proxy unbound and refuses
 * send.
 */
TEST(sendto_without_ephemeral_port_leaves_proxy_unbound_and_refuses_send) {
    reset();
    auto svc = make();
    track_udp(136);
    exhaust_ports(bsdt::ProtocolType::Udp);
    const auto peer = ldn_addr(LDN_PEER, 6000);
    const std::uint8_t data[2] = {1, 2};
    std::int32_t e = -1, n = -1;
    ams::sf::Out<std::int32_t> oe(e), on(n);
    svc->SendTo(
        oe, on, 136, 0, ams::sf::InBuffer(data, sizeof(data)), in_of(peer));
    ASSERT_TRUE(proxy_of(136)->GetState() == sut::ProxySocketState::Created);
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::DestAddrReq));
    ASSERT_EQ(g_sent.count, 0u);
}

/**
 * @test Verify sendto returns the error of a refused auto bind.
 */
TEST(sendto_returns_the_error_of_a_refused_auto_bind) {
    reset();
    auto svc = make();
    track_udp(137);
    g_race_fd = 137;
    g_race_locks = 0;
    g_race_armed = true;
    ams::os::g_host_before_recursive_lock = bind_the_proxy_first;
    const auto peer = ldn_addr(LDN_PEER, 6000);
    const std::uint8_t data[2] = {1, 2};
    std::int32_t e = -1, n = -1;
    ams::sf::Out<std::int32_t> oe(e), on(n);
    const auto rc = svc->SendTo(
        oe, on, 137, 0, ams::sf::InBuffer(data, sizeof(data)), in_of(peer));
    ams::os::g_host_before_recursive_lock = nullptr;
    ASSERT_EQ(rc.GetValue(), static_cast<std::uint32_t>(bsdt::BsdErrno::Inval));
    ASSERT_FALSE(sut::FindSocketInfoEntry(137)->info.is_proxy);
}

/**
 * @test Verify sendto a non ldn destination is forwarded with both buffers.
 */
TEST(sendto_a_non_ldn_destination_is_forwarded_with_both_buffers) {
    reset();
    auto svc = make();
    track_udp(138);
    const auto outside = ldn_addr(0xC0A80001, 80);
    const std::uint8_t data[3] = {1, 2, 3};
    std::int32_t e = -1, n = -1;
    ams::sf::Out<std::int32_t> oe(e), on(n);
    reply_with(Pair{0, 3});
    ASSERT_TRUE(svc->SendTo(
                       oe,
                       on,
                       138,
                       4,
                       ams::sf::InBuffer(data, sizeof(data)),
                       in_of(outside))
                    .IsSuccess());
    ASSERT_EQ(g_seen.command, 11u);
    ASSERT_EQ(n, 3);
    ASSERT_EQ(g_seen.options.buffers[0].size, sizeof(data));
    ASSERT_EQ(g_seen.options.buffers[1].size, sizeof(bsdt::SockAddrIn));
    // Untracked sockets and addresses too short to read are forwarded as well.
    svc->SendTo(
        oe, on, 139, 0, ams::sf::InBuffer(data, sizeof(data)), in_of(outside));
    const std::uint8_t few[4] = {};
    svc->SendTo(
        oe,
        on,
        138,
        0,
        ams::sf::InBuffer(data, sizeof(data)),
        ams::sf::InBuffer(few, sizeof(few)));
    svc->SendTo(
        oe,
        on,
        138,
        0,
        ams::sf::InBuffer(data, sizeof(data)),
        ams::sf::InBuffer(nullptr, sizeof(bsdt::SockAddrIn)));
    auto inet6 = ldn_addr(LDN_PEER, 80);
    inet6.sin_family = static_cast<std::uint8_t>(bsdt::AddressFamily::Inet6);
    svc->SendTo(
        oe, on, 138, 0, ams::sf::InBuffer(data, sizeof(data)), in_of(inet6));
    ASSERT_EQ(forwarded(), 5u);
}

/**
 * @test Verify send and write go through a connected proxy.
 */
TEST(send_and_write_go_through_a_connected_proxy) {
    reset();
    auto svc = make();
    track_udp(140);
    const auto peer = ldn_addr(LDN_PEER, 6000);
    std::int32_t e = -1, n = -1;
    ams::sf::Out<std::int32_t> oe(e), on(n);
    svc->Connect(oe, 140, in_of(peer));
    const std::uint8_t data[4] = {1, 2, 3, 4};
    ASSERT_TRUE(svc->Send(oe, on, 140, 0, ams::sf::InBuffer(data, sizeof(data)))
                    .IsSuccess());
    ASSERT_EQ(n, 4);
    ASSERT_EQ(e, 0);
    ASSERT_TRUE(
        svc->Write(oe, on, 140, ams::sf::InBuffer(data, 3)).IsSuccess());
    ASSERT_EQ(n, 3);
    ASSERT_EQ(g_sent.count, 2u);
    ASSERT_EQ(forwarded(), 0u);
}

/**
 * @test Verify send and write report the proxy error of an unconnected proxy.
 */
TEST(send_and_write_report_the_proxy_error_of_an_unconnected_proxy) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 141, 5400), 0);
    const std::uint8_t data[4] = {1, 2, 3, 4};
    std::int32_t e = -1, n = -1;
    ams::sf::Out<std::int32_t> oe(e), on(n);
    svc->Send(oe, on, 141, 0, ams::sf::InBuffer(data, sizeof(data)));
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::NotConn));
    ASSERT_EQ(n, 0);
    e = -1;
    svc->Write(oe, on, 141, ams::sf::InBuffer(data, sizeof(data)));
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::NotConn));
    ASSERT_EQ(n, 0);
}

/**
 * @test Verify send and write of a plain socket are forwarded.
 */
TEST(send_and_write_of_a_plain_socket_are_forwarded) {
    reset();
    auto svc = make();
    track(142, bsdt::SocketType::Stream, bsdt::ProtocolType::Tcp);
    track(
        143,
        bsdt::SocketType::Stream,
        bsdt::ProtocolType::Tcp,
        true);  // proxy flag, no proxy object
    const std::uint8_t data[4] = {1, 2, 3, 4};
    std::int32_t e = -1, n = -1;
    ams::sf::Out<std::int32_t> oe(e), on(n);
    reply_with(Pair{0, 4});
    for (std::int32_t fd : {142, 143}) {
        svc->Send(oe, on, fd, 0, ams::sf::InBuffer(data, sizeof(data)));
        ASSERT_EQ(g_seen.command, 10u);
        ASSERT_EQ(n, 4);
        svc->Write(oe, on, fd, ams::sf::InBuffer(data, sizeof(data)));
        ASSERT_EQ(g_seen.command, 24u);
        ASSERT_EQ(n, 4);
    }
    ASSERT_EQ(forwarded(), 4u);
}

// ---------------------------------------------------------------------------
// Recv / RecvFrom / Read
// ---------------------------------------------------------------------------

/**
 * @test Verify recv and read return the datagram of a connected proxy then
 * report eagain.
 */
TEST(
    recv_and_read_return_the_datagram_of_a_connected_proxy_then_report_eagain) {
    reset();
    auto svc = make();
    track_udp(150);
    const auto peer = ldn_addr(LDN_PEER, 6000);
    std::int32_t e = -1, n = -1;
    ams::sf::Out<std::int32_t> oe(e), on(n);
    svc->Connect(oe, 150, in_of(peer));
    proxy_of(150)->SetNonBlocking(true);
    ASSERT_TRUE(deliver(150, "hello"));
    ASSERT_TRUE(deliver(150, "world!"));
    std::uint8_t buf[16] = {};
    ASSERT_TRUE(
        svc->Recv(
               oe, on, 150, 0, ams::sf::OutAutoSelectBuffer(buf, sizeof(buf)))
            .IsSuccess());
    ASSERT_EQ(e, 0);
    ASSERT_EQ(n, 5);
    ASSERT_EQ(std::memcmp(buf, "hello", 5), 0);
    ASSERT_TRUE(
        svc->Read(oe, on, 150, ams::sf::OutAutoSelectBuffer(buf, sizeof(buf)))
            .IsSuccess());
    ASSERT_EQ(n, 6);
    ASSERT_EQ(std::memcmp(buf, "world!", 6), 0);
    svc->Recv(oe, on, 150, 0, ams::sf::OutAutoSelectBuffer(buf, sizeof(buf)));
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::Again));
    ASSERT_EQ(n, 0);
    e = -1;
    svc->Read(oe, on, 150, ams::sf::OutAutoSelectBuffer(buf, sizeof(buf)));
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::Again));
    ASSERT_EQ(forwarded(), 0u);
}

/**
 * @test Verify recvfrom returns the datagram with the sender address.
 */
TEST(recvfrom_returns_the_datagram_with_the_sender_address) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 151, 5500), 0);
    ASSERT_TRUE(deliver(151, "ping"));
    std::int32_t ret = -1, e = -1;
    std::uint32_t len = 99;
    ams::sf::Out<std::int32_t> oret(ret), oe(e);
    ams::sf::Out<std::uint32_t> olen(len);
    std::uint8_t buf[16] = {};
    bsdt::SockAddrIn from{};
    ASSERT_TRUE(
        svc->RecvFrom(
               oret,
               oe,
               olen,
               151,
               0,
               ams::sf::OutAutoSelectBuffer(buf, sizeof(buf)),
               ams::sf::OutAutoSelectBuffer(
                   reinterpret_cast<std::uint8_t*>(&from), sizeof(from)))
            .IsSuccess());
    ASSERT_EQ(ret, 4);
    ASSERT_EQ(e, 0);
    ASSERT_EQ(len, sizeof(bsdt::SockAddrIn));
    ASSERT_EQ(from.GetAddr(), LDN_PEER);
    ASSERT_EQ(from.GetPort(), 6000u);
    // A too small address buffer is left alone and reported as length 0.
    ASSERT_TRUE(deliver(151, "pong"));
    std::uint8_t tiny[4] = {};
    svc->RecvFrom(
        oret,
        oe,
        olen,
        151,
        0,
        ams::sf::OutAutoSelectBuffer(buf, sizeof(buf)),
        ams::sf::OutAutoSelectBuffer(tiny, sizeof(tiny)));
    ASSERT_EQ(ret, 4);
    ASSERT_EQ(len, 0u);
}

/**
 * @test Verify recvfrom reports the error of an empty non blocking proxy.
 */
TEST(recvfrom_reports_the_error_of_an_empty_non_blocking_proxy) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 152, 5501), 0);
    proxy_of(152)->SetNonBlocking(true);
    std::int32_t ret = 5, e = -1;
    std::uint32_t len = 99;
    ams::sf::Out<std::int32_t> oret(ret), oe(e);
    ams::sf::Out<std::uint32_t> olen(len);
    std::uint8_t buf[16] = {};
    svc->RecvFrom(
        oret,
        oe,
        olen,
        152,
        0,
        ams::sf::OutAutoSelectBuffer(buf, sizeof(buf)),
        ams::sf::OutAutoSelectBuffer(buf, sizeof(buf)));
    ASSERT_EQ(ret, -1);
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::Again));
    ASSERT_EQ(len, 0u);
}

/**
 * @test Verify recv recvfrom and read of a plain socket are forwarded.
 */
TEST(recv_recvfrom_and_read_of_a_plain_socket_are_forwarded) {
    reset();
    auto svc = make();
    track(153, bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp);
    track(
        154,
        bsdt::SocketType::Dgram,
        bsdt::ProtocolType::Udp,
        true);  // flag without proxy
    std::int32_t ret = -1, e = -1, n = -1;
    std::uint32_t len = 0;
    ams::sf::Out<std::int32_t> oret(ret), oe(e), on(n);
    ams::sf::Out<std::uint32_t> olen(len);
    std::uint8_t buf[16] = {};
    ams::sf::OutAutoSelectBuffer out_buf(buf, sizeof(buf));
    for (std::int32_t fd : {153, 154}) {
        reply_with(Pair{0, 6});
        svc->Recv(oe, on, fd, 0, out_buf);
        ASSERT_EQ(g_seen.command, 8u);
        ASSERT_EQ(n, 6);
        svc->Read(oe, on, fd, out_buf);
        ASSERT_EQ(g_seen.command, 25u);
        ASSERT_EQ(n, 6);
        reply_with(Triple{6, 0, 16});
        svc->RecvFrom(oret, oe, olen, fd, 0, out_buf, out_buf);
        ASSERT_EQ(g_seen.command, 9u);
        ASSERT_EQ(ret, 6);
        ASSERT_EQ(len, 16u);
    }
    ASSERT_EQ(forwarded(), 6u);
}

// ---------------------------------------------------------------------------
// Select / Poll
// ---------------------------------------------------------------------------

namespace {
/**
 * @brief Set a descriptor's bit in an fd_set-like bitmap.
 * @param[in,out] fds Bitmap.
 * @param fd Descriptor to set.
 */
void set_bit(std::uint8_t* fds, std::int32_t fd) {
    fds[fd / 8] = static_cast<std::uint8_t>(fds[fd / 8] | (1U << (fd % 8)));
}
/**
 * @brief Test a descriptor's bit in an fd_set-like bitmap.
 * @param fds Bitmap.
 * @param fd Descriptor to test.
 * @return true when the bit is set.
 */
bool has_bit(const std::uint8_t* fds, std::int32_t fd) {
    return (fds[fd / 8] & (1U << (fd % 8))) != 0;
}

/// Input and output buffers of one Select call.
struct SelectSets {
    /// Read set passed in.
    std::uint8_t r_in[16] = {};
    /// Write set passed in.
    std::uint8_t w_in[16] = {};
    /// Exception set passed in.
    std::uint8_t e_in[16] = {};
    /// Read set returned.
    std::uint8_t r_out[16] = {};
    /// Write set returned.
    std::uint8_t w_out[16] = {};
    /// Exception set returned.
    std::uint8_t e_out[16] = {};
    /// Timeout structure passed in.
    std::uint8_t timeout[16] = {};
    /// errno returned by Select.
    std::int32_t errno_out = -1;
    /// Ready count returned by Select.
    std::int32_t count = -1;

    /**
     * @brief Call Select with these sets.
     * @param[in,out] svc Service under test.
     * @param nfds Highest descriptor plus one.
     * @return The Select result.
     */
    ams::Result run(sut::BsdMitmService& svc, std::int32_t nfds) {
        ams::sf::Out<std::int32_t> oe(errno_out), oc(count);
        return svc.Select(
            oe,
            oc,
            nfds,
            ams::sf::InBuffer(r_in, sizeof(r_in)),
            ams::sf::InBuffer(w_in, sizeof(w_in)),
            ams::sf::InBuffer(e_in, sizeof(e_in)),
            ams::sf::InBuffer(timeout, sizeof(timeout)),
            ams::sf::OutAutoSelectBuffer(r_out, sizeof(r_out)),
            ams::sf::OutAutoSelectBuffer(w_out, sizeof(w_out)),
            ams::sf::OutAutoSelectBuffer(e_out, sizeof(e_out)));
    }
};
}  // namespace

/**
 * @test Verify select over proxy sockets only is answered locally.
 */
TEST(select_over_proxy_sockets_only_is_answered_locally) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 3, 5600), 0);
    ASSERT_EQ(bind_ldn(*svc, 4, 5601), 0);
    ASSERT_EQ(bind_ldn(*svc, 5, 5602), 0);
    ASSERT_TRUE(deliver(3, "x"));
    proxy_of(5)->Close();
    SelectSets s;
    set_bit(s.r_in, 3);
    set_bit(s.r_in, 4);  // nothing pending: not ready
    set_bit(s.w_in, 4);  // always writable
    set_bit(s.e_in, 5);  // closed: error
    set_bit(s.e_in, 4);  // open: no error
    ASSERT_TRUE(s.run(*svc, 8).IsSuccess());
    ASSERT_EQ(forwarded(), 0u);
    ASSERT_EQ(s.errno_out, 0);
    ASSERT_EQ(s.count, 3);
    ASSERT_TRUE(has_bit(s.r_out, 3));
    ASSERT_FALSE(has_bit(s.r_out, 4));
    ASSERT_TRUE(has_bit(s.w_out, 4));
    ASSERT_TRUE(has_bit(s.e_out, 5));
    ASSERT_FALSE(has_bit(s.e_out, 4));
}

/**
 * @test Verify select mixing real sockets answers locally when a proxy is
 * ready.
 */
TEST(select_mixing_real_sockets_answers_locally_when_a_proxy_is_ready) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 3, 5603), 0);
    ASSERT_TRUE(deliver(3, "x"));
    SelectSets s;
    set_bit(s.r_in, 3);
    set_bit(s.r_in, 6);  // a real socket
    ASSERT_TRUE(s.run(*svc, 8).IsSuccess());
    ASSERT_EQ(forwarded(), 0u);
    ASSERT_EQ(s.count, 1);
    ASSERT_TRUE(has_bit(s.r_out, 3));
}

/**
 * @test Verify select mixing real sockets forwards when no proxy is ready.
 */
TEST(select_mixing_real_sockets_forwards_when_no_proxy_is_ready) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 3, 5604), 0);
    SelectSets s;
    set_bit(s.r_in, 3);  // proxy with nothing pending
    set_bit(s.r_in, 6);  // a real socket
    reply_with(Pair{0, 1});
    ASSERT_TRUE(s.run(*svc, 8).IsSuccess());
    ASSERT_EQ(g_seen.command, 5u);
    ASSERT_EQ(g_seen.options.buffers[0].size, sizeof(s.r_in));
    ASSERT_EQ(s.errno_out, 0);
    ASSERT_EQ(s.count, 1);
    ASSERT_FALSE(has_bit(s.r_out, 3));
}

namespace {
/// Stages what a concurrent IPC thread does while the real select waits: the
/// game's fd 4 becomes a proxy socket that is then closed, and data lands on
/// the proxy of fd 3.
/// @param call Forwarded Select call; its output receives a fixed Pair.
/// @return Success.
ams::Result proxies_change_during_the_real_wait(const host_mitm::Call& call) {
    deliver(3, "late");
    auto& manager = sut::ProxySocketManager::GetInstance();
    manager
        .CreateProxySocket(4, bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp)
        ->Close();
    Pair p{0, 1};
    std::memcpy(call.out, &p, sizeof(p));
    return ams::Result();
}
}  // namespace

/**
 * @test Verify select adds the proxy readiness that appeared during the real
 * wait.
 */
TEST(select_adds_the_proxy_readiness_that_appeared_during_the_real_wait) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 3, 5606), 0);
    SelectSets s;
    set_bit(s.r_in, 3);  // proxy, empty before the wait
    set_bit(s.r_in, 4);  // still a real socket before the wait
    set_bit(s.w_in, 4);
    set_bit(s.e_in, 4);
    host_mitm::g_dispatch_hook = proxies_change_during_the_real_wait;
    ASSERT_TRUE(s.run(*svc, 8).IsSuccess());
    host_mitm::g_dispatch_hook = fake_bsd;
    ASSERT_EQ(
        s.count, 1 + 1 + 1 + 1);  // real count + read(3) + write(4) + error(4)
    ASSERT_TRUE(has_bit(s.r_out, 3));
    ASSERT_TRUE(has_bit(s.w_out, 4));
    ASSERT_TRUE(has_bit(s.e_out, 4));
}

/**
 * @test Verify select after a failed real wait does not add proxy results.
 */
TEST(select_after_a_failed_real_wait_does_not_add_proxy_results) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 3, 5607), 0);
    SelectSets s;
    set_bit(s.r_in, 3);
    set_bit(s.r_in, 6);
    g_rc = 11;
    reply_with(Pair{4, 0});
    ASSERT_EQ(s.run(*svc, 8).GetValue(), 11u);
    ASSERT_EQ(s.errno_out, 4);
    ASSERT_EQ(s.count, 0);
    ASSERT_FALSE(has_bit(s.r_out, 3));
}

/**
 * @test Verify select over real sockets only is forwarded.
 */
TEST(select_over_real_sockets_only_is_forwarded) {
    reset();
    auto svc = make();
    SelectSets s;
    set_bit(s.r_in, 6);
    set_bit(s.w_in, 7);
    reply_with(Pair{0, 2});
    ASSERT_TRUE(s.run(*svc, 8).IsSuccess());
    ASSERT_EQ(g_seen.command, 5u);
    ASSERT_EQ(s.count, 2);
    ASSERT_EQ(g_seen.options.buffers[6].size, sizeof(s.e_out));
}

/**
 * @test Verify select ignores descriptors past the set and works without sets.
 */
TEST(select_ignores_descriptors_past_the_set_and_works_without_sets) {
    reset();
    auto svc = make();
    std::int32_t e = -1, c = -1;
    ams::sf::Out<std::int32_t> oe(e), oc(c);
    // nfds larger than the 8-bit sets: fd 8.. are outside every set.
    std::uint8_t one[1] = {0};
    set_bit(one, 3);
    reply_with(Pair{0, 0});
    ASSERT_TRUE(svc->Select(
                       oe,
                       oc,
                       64,
                       ams::sf::InBuffer(one, 1),
                       ams::sf::InBuffer(one, 1),
                       ams::sf::InBuffer(one, 1),
                       ams::sf::InBuffer(nullptr, 0),
                       ams::sf::OutAutoSelectBuffer(nullptr, 0),
                       ams::sf::OutAutoSelectBuffer(nullptr, 0),
                       ams::sf::OutAutoSelectBuffer(nullptr, 0))
                    .IsSuccess());
    ASSERT_EQ(g_seen.command, 5u);
    // Null input sets: no descriptor is watched, no proxy involved: forwarded
    // as is.
    ASSERT_TRUE(svc->Select(
                       oe,
                       oc,
                       8,
                       ams::sf::InBuffer(nullptr, 0),
                       ams::sf::InBuffer(nullptr, 0),
                       ams::sf::InBuffer(nullptr, 0),
                       ams::sf::InBuffer(nullptr, 0),
                       ams::sf::OutAutoSelectBuffer(nullptr, 0),
                       ams::sf::OutAutoSelectBuffer(nullptr, 0),
                       ams::sf::OutAutoSelectBuffer(nullptr, 0))
                    .IsSuccess());
    ASSERT_EQ(forwarded(), 2u);
}

/**
 * @test Verify select never writes a ready proxy into missing output sets.
 */
TEST(select_never_writes_a_ready_proxy_into_missing_output_sets) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 3, 5609), 0);
    ASSERT_TRUE(deliver(3, "x"));
    std::int32_t e = -1, c = -1;
    ams::sf::Out<std::int32_t> oe(e), oc(c);
    std::uint8_t in[1] = {0};
    set_bit(in, 3);
    ASSERT_TRUE(svc->Select(
                       oe,
                       oc,
                       8,
                       ams::sf::InBuffer(in, 1),
                       ams::sf::InBuffer(in, 1),
                       ams::sf::InBuffer(in, 1),
                       ams::sf::InBuffer(nullptr, 0),
                       ams::sf::OutAutoSelectBuffer(nullptr, 0),
                       ams::sf::OutAutoSelectBuffer(nullptr, 0),
                       ams::sf::OutAutoSelectBuffer(nullptr, 0))
                    .IsSuccess());
    ASSERT_EQ(
        c, 2);  // read (pending) + write interest, counted though not reported
    // An output set too small for the descriptor is skipped as well.
    std::uint8_t big_in[2] = {0, 0};
    set_bit(big_in, 3);
    std::uint8_t tiny_out[1] = {};
    ASSERT_TRUE(svc->Select(
                       oe,
                       oc,
                       8,
                       ams::sf::InBuffer(big_in, 2),
                       ams::sf::InBuffer(nullptr, 0),
                       ams::sf::InBuffer(nullptr, 0),
                       ams::sf::InBuffer(nullptr, 0),
                       ams::sf::OutAutoSelectBuffer(tiny_out, 0),
                       ams::sf::OutAutoSelectBuffer(nullptr, 0),
                       ams::sf::OutAutoSelectBuffer(nullptr, 0))
                    .IsSuccess());
}

namespace {
/// Input and output arrays of one Poll call.
struct PollSet {
    /// Poll entries passed in.
    bsdt::PollFd in[4]{};
    /// Poll entries returned.
    bsdt::PollFd out[4]{};
    /// errno returned by Poll.
    std::int32_t errno_out = -1;
    /// Ready count returned by Poll.
    std::int32_t count = -1;

    /**
     * @brief Call Poll with these entries.
     * @param[in,out] svc Service under test.
     * @param nfds Number of entries in `in`.
     * @param out_bytes Size of the output buffer handed to Poll.
     * @return The Poll result.
     */
    ams::Result run(
        sut::BsdMitmService& svc,
        std::int32_t nfds,
        std::size_t out_bytes = sizeof(out)) {
        ams::sf::Out<std::int32_t> oe(errno_out), oc(count);
        return svc.Poll(
            oe,
            oc,
            ams::sf::InBuffer(
                reinterpret_cast<const std::uint8_t*>(in), sizeof(in)),
            ams::sf::OutAutoSelectBuffer(
                reinterpret_cast<std::uint8_t*>(out), out_bytes),
            nfds,
            0);
    }
};
}  // namespace

/**
 * @test Verify poll over proxy sockets only is answered locally.
 */
TEST(poll_over_proxy_sockets_only_is_answered_locally) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 3, 5700), 0);
    ASSERT_EQ(bind_ldn(*svc, 4, 5701), 0);
    ASSERT_EQ(bind_ldn(*svc, 5, 5702), 0);
    ASSERT_TRUE(deliver(3, "x"));
    proxy_of(5)->Close();
    PollSet p;
    p.in[0] = {3, POLLIN, 0};
    p.in[1] = {4, POLLIN, 0};   // nothing pending
    p.in[2] = {5, POLLOUT, 0};  // closed: writable bit and hang-up
    ASSERT_TRUE(p.run(*svc, 3).IsSuccess());
    ASSERT_EQ(forwarded(), 0u);
    ASSERT_EQ(p.count, 2);
    ASSERT_EQ(p.out[0].revents, POLLIN);
    ASSERT_EQ(p.out[1].revents, 0);
    ASSERT_EQ(p.out[2].revents, POLLOUT | POLLHUP);
}

/**
 * @test Verify poll mixing real sockets answers locally when a proxy is ready.
 */
TEST(poll_mixing_real_sockets_answers_locally_when_a_proxy_is_ready) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 3, 5703), 0);
    ASSERT_TRUE(deliver(3, "x"));
    PollSet p;
    p.in[0] = {3, POLLIN, 0};
    p.in[1] = {9, POLLIN, 0};  // real socket
    p.in[1].revents = POLLIN;  // stale value must not leak out
    ASSERT_TRUE(p.run(*svc, 2).IsSuccess());
    ASSERT_EQ(forwarded(), 0u);
    ASSERT_EQ(p.count, 1);
    ASSERT_EQ(p.out[0].revents, POLLIN);
    ASSERT_EQ(p.out[1].revents, 0);
}

/**
 * @test Verify poll mixing real sockets forwards if no proxy ready and adds
 * proxy events.
 */
TEST(
    poll_mixing_real_sockets_forwards_if_no_proxy_ready_and_adds_proxy_events) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 3, 5704), 0);
    PollSet p;
    p.in[0] = {3, POLLIN, 0};  // empty
    p.in[1] = {9, POLLIN, 0};
    reply_with(Pair{0, 1});
    ASSERT_TRUE(p.run(*svc, 2).IsSuccess());
    ASSERT_EQ(g_seen.command, 6u);
    ASSERT_EQ(p.count, 1);
    // Now with a proxy that has data, interest in writing and a closed one.
    ASSERT_EQ(bind_ldn(*svc, 4, 5705), 0);
    ASSERT_EQ(bind_ldn(*svc, 5, 5706), 0);
    proxy_of(5)->Close();
    PollSet q;
    q.in[0] = {4, POLLOUT, 0};
    q.in[1] = {9, POLLIN, 0};
    q.in[2] = {5, POLLIN, 0};
    // POLLOUT is always ready, so the proxy answers locally; use only the
    // closed one.
    PollSet r;
    r.in[0] = {5, 0, 0};
    r.in[1] = {9, POLLIN, 0};
    reply_with(Pair{0, 0});
    ASSERT_TRUE(r.run(*svc, 2).IsSuccess());
    ASSERT_EQ(r.out[0].revents, POLLHUP);
    ASSERT_EQ(r.count, 1);
}

/**
 * @test Verify poll adds the events that arrived while the real wait ran.
 */
TEST(poll_adds_the_events_that_arrived_while_the_real_wait_ran) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 3, 5707), 0);
    PollSet p;
    p.in[0] = {3, POLLIN, 0};
    p.in[1] = {9, POLLIN, 0};
    host_mitm::g_dispatch_hook =
        [](const host_mitm::Call& call) -> ams::Result {
        deliver(3, "late");
        Pair pr{0, 0};
        std::memcpy(call.out, &pr, sizeof(pr));
        return ams::Result();
    };
    ASSERT_TRUE(p.run(*svc, 2).IsSuccess());
    host_mitm::g_dispatch_hook = fake_bsd;
    ASSERT_EQ(p.out[0].revents, POLLIN);
    ASSERT_EQ(p.count, 1);
}

/**
 * @test Verify poll after a failed real wait does not add proxy events.
 */
TEST(poll_after_a_failed_real_wait_does_not_add_proxy_events) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 3, 5708), 0);
    PollSet p;
    p.in[0] = {3, POLLIN, 0};
    p.in[1] = {9, POLLIN, 0};
    g_rc = 9;
    ASSERT_EQ(p.run(*svc, 2).GetValue(), 9u);
    ASSERT_EQ(p.count, 0);
}

/**
 * @test Verify poll over real sockets only is forwarded with both buffers.
 */
TEST(poll_over_real_sockets_only_is_forwarded_with_both_buffers) {
    reset();
    auto svc = make();
    PollSet p;
    p.in[0] = {9, POLLIN, 0};
    reply_with(Pair{0, 1});
    ASSERT_TRUE(p.run(*svc, 1).IsSuccess());
    ASSERT_EQ(g_seen.command, 6u);
    ASSERT_EQ(g_seen.options.buffers[0].size, sizeof(p.in));
    ASSERT_EQ(g_seen.options.buffers[1].size, sizeof(p.out));
    ASSERT_EQ(p.count, 1);
}

/**
 * @test Verify poll with an output buffer smaller than the input does not copy
 * it.
 */
TEST(poll_with_an_output_buffer_smaller_than_the_input_does_not_copy_it) {
    reset();
    auto svc = make();
    PollSet p;
    p.in[0] = {9, POLLIN, 0};
    reply_with(Pair{0, 0});
    p.out[0].fd = 1234;
    ASSERT_TRUE(p.run(*svc, 4, sizeof(bsdt::PollFd)).IsSuccess());
    ASSERT_EQ(p.out[0].fd, 1234);  // not overwritten by the copy of the input
}

// ---------------------------------------------------------------------------
// Ioctl / Fcntl / socket options / names
// ---------------------------------------------------------------------------

/**
 * @test Verify ioctl fionread on a proxy is answered locally.
 */
TEST(ioctl_fionread_on_a_proxy_is_answered_locally) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 160, 5800), 0);
    ASSERT_TRUE(deliver(160, "abcd"));
    std::int32_t e = -1, r = -1;
    ams::sf::Out<std::int32_t> oe(e), orr(r);
    std::uint8_t in[4] = {};
    std::int32_t pending = -1;
    ASSERT_TRUE(
        svc->Ioctl(
               oe,
               orr,
               160,
               0x4004667F,
               1,
               ams::sf::InBuffer(in, sizeof(in)),
               ams::sf::OutAutoSelectBuffer(
                   reinterpret_cast<std::uint8_t*>(&pending), sizeof(pending)))
            .IsSuccess());
    ASSERT_EQ(e, 0);
    ASSERT_EQ(r, 0);
    ASSERT_EQ(pending, 4);
    ASSERT_EQ(forwarded(), 0u);
    // A buffer too small for the answer is left alone.
    std::uint8_t tiny[2] = {0x55, 0x55};
    svc->Ioctl(
        oe,
        orr,
        160,
        0x4004667F,
        1,
        ams::sf::InBuffer(in, sizeof(in)),
        ams::sf::OutAutoSelectBuffer(tiny, sizeof(tiny)));
    ASSERT_EQ(tiny[0], 0x55);
}

/**
 * @test Verify ioctl other requests on a proxy are refused.
 */
TEST(ioctl_other_requests_on_a_proxy_are_refused) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 161, 5801), 0);
    std::int32_t e = 0, r = 0;
    ams::sf::Out<std::int32_t> oe(e), orr(r);
    std::uint8_t in[4] = {};
    std::uint8_t out[4] = {};
    svc->Ioctl(
        oe,
        orr,
        161,
        0x1234,
        1,
        ams::sf::InBuffer(in, sizeof(in)),
        ams::sf::OutAutoSelectBuffer(out, sizeof(out)));
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::Inval));
    ASSERT_EQ(r, -1);
    ASSERT_EQ(forwarded(), 0u);
}

/**
 * @test Verify ioctl on a plain socket is forwarded.
 */
TEST(ioctl_on_a_plain_socket_is_forwarded) {
    reset();
    auto svc = make();
    std::int32_t e = -1, r = -1;
    ams::sf::Out<std::int32_t> oe(e), orr(r);
    std::uint8_t in[4] = {};
    std::uint8_t out[4] = {};
    reply_with(Pair{0, 12});
    ASSERT_TRUE(svc->Ioctl(
                       oe,
                       orr,
                       162,
                       0x1234,
                       1,
                       ams::sf::InBuffer(in, sizeof(in)),
                       ams::sf::OutAutoSelectBuffer(out, sizeof(out)))
                    .IsSuccess());
    ASSERT_EQ(g_seen.command, 19u);
    ASSERT_EQ(r, 12);
}

/**
 * @test Verify fcntl on a proxy reads and sets the non blocking flag.
 */
TEST(fcntl_on_a_proxy_reads_and_sets_the_non_blocking_flag) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 163, 5802), 0);
    std::int32_t e = -1, r = -1;
    ams::sf::Out<std::int32_t> oe(e), orr(r);
    svc->Fcntl(oe, orr, 163, 3, 0);
    ASSERT_EQ(r, 0);
    svc->Fcntl(oe, orr, 163, 4, 0x0004);
    ASSERT_EQ(e, 0);
    ASSERT_TRUE(proxy_of(163)->IsNonBlocking());
    svc->Fcntl(oe, orr, 163, 3, 0);
    ASSERT_EQ(r, 0x0004);
    svc->Fcntl(oe, orr, 163, 4, 0);
    ASSERT_FALSE(proxy_of(163)->IsNonBlocking());
    svc->Fcntl(oe, orr, 163, 99, 0);
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::Inval));
    ASSERT_EQ(r, -1);
    ASSERT_EQ(forwarded(), 0u);
}

/**
 * @test Verify fcntl on a plain socket is forwarded.
 */
TEST(fcntl_on_a_plain_socket_is_forwarded) {
    reset();
    auto svc = make();
    std::int32_t e = -1, r = -1;
    ams::sf::Out<std::int32_t> oe(e), orr(r);
    reply_with(Pair{0, 5});
    ASSERT_TRUE(svc->Fcntl(oe, orr, 164, 3, 0).IsSuccess());
    ASSERT_EQ(g_seen.command, 20u);
    ASSERT_EQ(r, 5);
}

/**
 * @test Verify getsockopt on a proxy is answered by the proxy.
 */
TEST(getsockopt_on_a_proxy_is_answered_by_the_proxy) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 165, 5803), 0);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    std::int32_t value = -1;
    svc->GetSockOpt(
        oe,
        165,
        static_cast<std::int32_t>(bsdt::SocketOptionLevel::Socket),
        static_cast<std::int32_t>(bsdt::SocketOption::Type),
        ams::sf::OutAutoSelectBuffer(
            reinterpret_cast<std::uint8_t*>(&value), sizeof(value)));
    ASSERT_EQ(e, 0);
    ASSERT_EQ(value, static_cast<std::int32_t>(bsdt::SocketType::Dgram));
    svc->GetSockOpt(
        oe,
        165,
        static_cast<std::int32_t>(bsdt::SocketOptionLevel::Socket),
        0x7777,
        ams::sf::OutAutoSelectBuffer(
            reinterpret_cast<std::uint8_t*>(&value), sizeof(value)));
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::NoProtoOpt));
    ASSERT_EQ(forwarded(), 0u);
}

/**
 * @test Verify getsockopt on a plain socket is forwarded.
 */
TEST(getsockopt_on_a_plain_socket_is_forwarded) {
    reset();
    auto svc = make();
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    std::int32_t value = 0;
    reply_with(std::int32_t{3});
    ASSERT_TRUE(
        svc->GetSockOpt(
               oe,
               166,
               0xFFFF,
               0x1008,
               ams::sf::OutAutoSelectBuffer(
                   reinterpret_cast<std::uint8_t*>(&value), sizeof(value)))
            .IsSuccess());
    ASSERT_EQ(g_seen.command, 17u);
    ASSERT_EQ(e, 3);
}

/**
 * @test Verify setsockopt on a proxy is applied by the proxy and reports its
 * error.
 */
TEST(setsockopt_on_a_proxy_is_applied_by_the_proxy_and_reports_its_error) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 167, 5804), 0);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    const std::int32_t on = 1;
    const auto level =
        static_cast<std::int32_t>(bsdt::SocketOptionLevel::Socket);
    const auto bcast = static_cast<std::int32_t>(bsdt::SocketOption::Broadcast);
    svc->SetSockOpt(
        oe,
        167,
        level,
        bcast,
        ams::sf::InBuffer(
            reinterpret_cast<const std::uint8_t*>(&on), sizeof(on)));
    ASSERT_EQ(e, 0);
    ASSERT_TRUE(proxy_of(167)->IsBroadcastEnabled());
    const std::uint8_t short_value[2] = {1, 0};
    svc->SetSockOpt(
        oe,
        167,
        level,
        bcast,
        ams::sf::InBuffer(short_value, sizeof(short_value)));
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::Inval));
    ASSERT_EQ(forwarded(), 0u);
}

/**
 * @test Verify setsockopt so broadcast on a plain socket is saved and
 * forwarded.
 */
TEST(setsockopt_so_broadcast_on_a_plain_socket_is_saved_and_forwarded) {
    reset();
    auto svc = make();
    track_udp(168);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    const auto level =
        static_cast<std::int32_t>(bsdt::SocketOptionLevel::Socket);
    const auto bcast = static_cast<std::int32_t>(bsdt::SocketOption::Broadcast);
    std::int32_t value = 1;
    reply_with(std::int32_t{0});
    svc->SetSockOpt(
        oe,
        168,
        level,
        bcast,
        ams::sf::InBuffer(
            reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)));
    ASSERT_EQ(g_seen.command, 21u);
    ASSERT_TRUE(sut::FindSocketInfoEntry(168)->info.broadcast);
    value = 0;
    svc->SetSockOpt(
        oe,
        168,
        level,
        bcast,
        ams::sf::InBuffer(
            reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)));
    ASSERT_FALSE(sut::FindSocketInfoEntry(168)->info.broadcast);
    value = 1;
    // Not saved: too short a value, another option, an untracked socket.
    const std::uint8_t short_value[2] = {1, 0};
    svc->SetSockOpt(
        oe,
        168,
        level,
        bcast,
        ams::sf::InBuffer(short_value, sizeof(short_value)));
    svc->SetSockOpt(
        oe,
        168,
        level,
        static_cast<std::int32_t>(bsdt::SocketOption::ReuseAddr),
        ams::sf::InBuffer(
            reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)));
    svc->SetSockOpt(
        oe,
        169,
        level,
        bcast,
        ams::sf::InBuffer(
            reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)));
    ASSERT_FALSE(sut::FindSocketInfoEntry(168)->info.broadcast);
    ASSERT_TRUE(sut::FindSocketInfoEntry(169) == nullptr);
    ASSERT_EQ(forwarded(), 5u);
}

/**
 * @test Verify getpeername of a connected proxy returns the peer address.
 */
TEST(getpeername_of_a_connected_proxy_returns_the_peer_address) {
    reset();
    auto svc = make();
    track_udp(170);
    const auto peer = ldn_addr(LDN_PEER, 6000);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    svc->Connect(oe, 170, in_of(peer));
    bsdt::SockAddrIn got{};
    svc->GetPeerName(
        oe,
        170,
        ams::sf::OutAutoSelectBuffer(
            reinterpret_cast<std::uint8_t*>(&got), sizeof(got)));
    ASSERT_EQ(e, 0);
    ASSERT_EQ(got.GetAddr(), LDN_PEER);
    ASSERT_EQ(got.GetPort(), 6000u);
    std::uint8_t tiny[4] = {0x55};
    svc->GetPeerName(oe, 170, ams::sf::OutAutoSelectBuffer(tiny, sizeof(tiny)));
    ASSERT_EQ(e, 0);
    ASSERT_EQ(tiny[0], 0x55);
    ASSERT_EQ(forwarded(), 1u - 1u);
}

/**
 * @test Verify getpeername of an unconnected proxy reports enotconn.
 */
TEST(getpeername_of_an_unconnected_proxy_reports_enotconn) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 171, 5805), 0);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    bsdt::SockAddrIn got{};
    svc->GetPeerName(
        oe,
        171,
        ams::sf::OutAutoSelectBuffer(
            reinterpret_cast<std::uint8_t*>(&got), sizeof(got)));
    ASSERT_EQ(e, static_cast<std::int32_t>(bsdt::BsdErrno::NotConn));
}

/**
 * @test Verify getsockname of a proxy returns the local address.
 */
TEST(getsockname_of_a_proxy_returns_the_local_address) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 172, 5806), 0);
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    bsdt::SockAddrIn got{};
    svc->GetSockName(
        oe,
        172,
        ams::sf::OutAutoSelectBuffer(
            reinterpret_cast<std::uint8_t*>(&got), sizeof(got)));
    ASSERT_EQ(e, 0);
    ASSERT_EQ(got.GetPort(), 5806u);
    std::uint8_t tiny[4] = {0x55};
    svc->GetSockName(oe, 172, ams::sf::OutAutoSelectBuffer(tiny, sizeof(tiny)));
    ASSERT_EQ(tiny[0], 0x55);
    ASSERT_EQ(forwarded(), 0u);
}

/**
 * @test Verify getpeername and getsockname of a plain socket are forwarded.
 */
TEST(getpeername_and_getsockname_of_a_plain_socket_are_forwarded) {
    reset();
    auto svc = make();
    std::int32_t e = -1;
    ams::sf::Out<std::int32_t> oe(e);
    std::uint8_t buf[16] = {};
    reply_with(std::int32_t{6});
    svc->GetPeerName(oe, 173, ams::sf::OutAutoSelectBuffer(buf, sizeof(buf)));
    ASSERT_EQ(g_seen.command, 15u);
    ASSERT_EQ(e, 6);
    svc->GetSockName(oe, 173, ams::sf::OutAutoSelectBuffer(buf, sizeof(buf)));
    ASSERT_EQ(g_seen.command, 16u);
}

/**
 * @test Verify socket reusing a tracked descriptor number overwrites its entry.
 */
TEST(socket_reusing_a_tracked_descriptor_number_overwrites_its_entry) {
    reset();
    auto svc = make();
    track(20, bsdt::SocketType::Stream, bsdt::ProtocolType::Tcp, true, true);
    std::int32_t e = -1, fd = -1;
    ams::sf::Out<std::int32_t> oe(e), ofd(fd);
    reply_with(Pair{0, 20});
    svc->Socket(oe, ofd, 2, 2, 0);
    const auto* entry = sut::FindSocketInfoEntry(20);
    ASSERT_TRUE(entry->info.protocol == bsdt::ProtocolType::Udp);
    ASSERT_FALSE(entry->info.is_proxy);
    ASSERT_FALSE(entry->info.broadcast);
}

namespace {
/// Stages a concurrent IPC thread: fd 4 becomes a closed proxy socket while
/// the real poll waits.
/// @param call Forwarded Poll call.
/// @return Success.
ams::Result a_proxy_closes_during_the_real_poll(const host_mitm::Call& call) {
    sut::ProxySocketManager::GetInstance()
        .CreateProxySocket(4, bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp)
        ->Close();
    Pair p{0, 1};
    std::memcpy(call.out, &p, sizeof(p));
    return ams::Result();
}
}  // namespace

/**
 * @test Verify poll adds writable and hangup events of proxy that appeared mid
 * wait.
 */
TEST(poll_adds_writable_and_hangup_events_of_proxy_that_appeared_mid_wait) {
    reset();
    auto svc = make();
    ASSERT_EQ(bind_ldn(*svc, 3, 5709), 0);
    PollSet p;
    p.in[0] = {3, POLLIN, 0};   // proxy, nothing pending: no early answer
    p.in[1] = {4, POLLOUT, 0};  // a real socket when the call starts
    p.in[2] = {9, POLLIN, 0};
    host_mitm::g_dispatch_hook = a_proxy_closes_during_the_real_poll;
    ASSERT_TRUE(p.run(*svc, 3).IsSuccess());
    host_mitm::g_dispatch_hook = fake_bsd;
    ASSERT_EQ(p.out[1].revents, POLLOUT | POLLHUP);
    ASSERT_EQ(p.count, 1 + 2);
}

/**
 * @brief Run the registered tests.
 * @return The harness exit status.
 */
int main() {
    return test_report("bsd_mitm_service");
}
