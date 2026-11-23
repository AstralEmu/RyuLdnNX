/**
 * @file ldn_icommunication_real_tests.cpp
 * @brief Unit tests for the real ICommunicationService
 * (ldn/ldn_icommunication.cpp)
 *
 * The service is built exactly as the sysmodule builds it: it owns a real
 * RyuLdnClient and a real receive thread, and talks over a real TCP socket.
 * The Ryujinx LDN server is replaced by FakeServer, a loopback listener that
 * answers the handshake and then plays a script chosen by each test. Built
 * against the host Stratosphere stand-in in tests/stubs.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <new>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "test_harness.hpp"
#include "bsd/bsd_mitm_service.hpp"
#include "bsd/proxy_socket_manager.hpp"
#include "config/config.hpp"
#include "config/config_ipc_service.hpp"
#include "ldn/ldn_icommunication.hpp"
#include "ldn/ldn_shared_state.hpp"
#include "p2p/p2p_proxy_client.hpp"
#include "p2p/p2p_proxy_server.hpp"
#include "protocol/ryu_protocol.hpp"

namespace proto = ryu_ldn::protocol;
namespace lm = ams::mitm::ldn;

/**
 * @brief Host stand-in for BsdMitmService::CleanupAbandonedServices.
 *
 * bsd/bsd_mitm_service.cpp is not hostable yet (it needs the Atmosphere MITM
 * dispatch); the communication service only calls this one function of it.
 * Once the BSD service has its own host suite, link the real file and delete
 * this definition.
 */
void ams::mitm::bsd::BsdMitmService::CleanupAbandonedServices() {}

namespace {

/// Title id the service under test is built for.
constexpr std::uint64_t GAME_ID = 0x0100152000022000ULL;
/// Virtual LDN address of the host (10.114.0.1, Ryujinx byte order).
constexpr std::uint32_t HOST_VIP = 0x0A720001;
/// Subnet mask of the virtual LDN network (255.255.0.0).
constexpr std::uint32_t VIP_MASK = 0xFFFF0000;

/// Result the service returns for a command that the current state forbids.
constexpr std::uint32_t RC_BAD_STATE = MAKERESULT(0x10, 1);
/// Result returned when there is no connection to the server.
constexpr std::uint32_t RC_NOT_CONNECTED = MAKERESULT(0x10, 2);
/// Result returned when a packet could not be written to the server.
constexpr std::uint32_t RC_SEND_FAILED = MAKERESULT(0x10, 3);
/// Result returned for a failed handshake or a reject/refused answer.
constexpr std::uint32_t RC_HANDSHAKE_OR_REJECT = MAKERESULT(0x10, 4);
/// Result returned when the server does not answer in time.
constexpr std::uint32_t RC_TIMEOUT = MAKERESULT(0x10, 5);

// =============================================================================
// FakeServer: the Ryujinx LDN server on a loopback socket
// =============================================================================

/**
 * @brief Listens on an OS-assigned loopback port. It answers the client's
 * Initialize with an Initialize (which completes the handshake), records every
 * packet it receives and hands each one to the handler a test installed, so a
 * test can script the server's replies.
 */
class FakeServer {
public:
    /// A received packet: its id and its payload.
    using Packet = std::pair<proto::PacketId, std::vector<std::uint8_t>>;
    /// Script run for every packet received: the server, the id and the
    /// payload.
    using Handler = std::function<void(
        FakeServer&, proto::PacketId, const std::vector<std::uint8_t>&)>;

    /// Bind a loopback listener on an OS-assigned port and start the server
    /// thread.
    FakeServer() {
        m_listen = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(m_listen, 4);
        socklen_t len = sizeof(addr);
        ::getsockname(m_listen, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
        m_thread = std::thread([this] { run(); });
    }

    /// Stop the server thread, then close the connection and the listener.
    ~FakeServer() {
        m_stop = true;
        m_thread.join();
        close_connection();
        ::close(m_listen);
    }

    /// Not copyable: owns a socket and a thread.
    FakeServer(const FakeServer&) = delete;
    /// Not assignable: owns a socket and a thread.
    /// @return Never returns (deleted).
    FakeServer& operator=(const FakeServer&) = delete;

    /**
     * @brief Get the port the server listens on.
     * @return The OS-assigned loopback port.
     */
    std::uint16_t port() const {
        return m_port;
    }

    /**
     * @brief Install the script run for every received packet.
     * @param handler Script called after the built-in handshake answer.
     */
    void set_handler(Handler handler) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_handler = std::move(handler);
    }

    /**
     * @brief Choose whether the Initialize of the client is answered.
     *
     * When false the server accepts the TCP connection but never answers the
     * Initialize, so the handshake can only time out.
     *
     * @param answer True to answer with an Initialize (default behaviour).
     */
    void answer_handshake(bool answer) {
        m_answer_handshake = answer;
    }

    /**
     * @brief Choose how an EOF from the client is handled.
     *
     * When true, an EOF from the client (it shut its write side down) no
     * longer closes the connection: the server stops reading and keeps its
     * own side open, so the client stays "connected" yet every send fails.
     *
     * @param keep True to keep the connection open after the EOF.
     */
    void keep_open_on_eof(bool keep) {
        m_keep_open_on_eof = keep;
    }

    /**
     * @brief Send one packet to the connected client.
     * @param id Packet type to put in the LdnHeader.
     * @param payload Packet body, or nullptr for a header-only packet.
     * @param size Size of payload in bytes.
     * @return true when the packet was written; false when nobody is
     *         connected or it could not be encoded or written.
     */
    bool send(
        proto::PacketId id,
        const void* payload = nullptr,
        std::size_t size = 0) {
        std::vector<std::uint8_t> buf(sizeof(proto::LdnHeader) + size);
        std::size_t written = 0;
        if (proto::encode_raw(
                buf.data(),
                buf.size(),
                id,
                static_cast<const std::uint8_t*>(payload),
                size,
                written) != proto::EncodeResult::Success) {
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_conn >= 0 &&
               ::send(m_conn, buf.data(), written, MSG_NOSIGNAL) ==
                   static_cast<ssize_t>(written);
    }

    template <class T>
    /**
     * @brief Send a packet whose payload is a plain object.
     * @tparam T Trivially copyable payload type.
     * @param id Packet id.
     * @param payload Object sent byte for byte as the payload.
     * @return true when the whole packet was written.
     */
    bool send(proto::PacketId id, const T& payload) {
        return send(id, &payload, sizeof(payload));
    }

    /// Cut the connection from the server side (the client sees EOF).
    void drop() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_conn >= 0) {
            ::shutdown(m_conn, SHUT_RDWR);
        }
    }

    /**
     * @brief Tell whether a client connection is open on the server side.
     * @return true while a client connection is open.
     */
    bool has_client() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_conn >= 0;
    }

    /**
     * @brief Wait until `count` packets of this type were received.
     * @param id Packet type to wait for.
     * @param timeout_ms Longest wait, in milliseconds.
     * @param count Number of packets of that type to wait for (total).
     * @return true when the count was reached before the timeout.
     */
    bool wait_for(proto::PacketId id, int timeout_ms, std::size_t count = 1) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            return count_locked(id) >= count;
        });
    }

    /**
     * @brief Count the packets of a type received so far.
     * @param id Packet id to count.
     * @return Number of packets of that type.
     */
    std::size_t count(proto::PacketId id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return count_locked(id);
    }

    /**
     * @brief Payload of the most recent packet of this type.
     * @param id Packet type to look for.
     * @return The payload bytes; empty when none was received (or when the
     *         packet had no body).
     */
    std::vector<std::uint8_t> last(proto::PacketId id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_received.rbegin(); it != m_received.rend(); ++it) {
            if (it->first == id) {
                return it->second;
            }
        }
        return {};
    }

private:
    /**
     * @brief Count the packets of a type; the caller must hold m_mutex.
     * @param id Packet id to count.
     * @return Number of packets of that type.
     */
    std::size_t count_locked(proto::PacketId id) const {
        std::size_t n = 0;
        for (const auto& p : m_received) {
            n += (p.first == id) ? 1 : 0;
        }
        return n;
    }

    /// Close the client connection, if any.
    void close_connection() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_conn >= 0) {
            ::close(m_conn);
            m_conn = -1;
        }
    }

    /**
     * @brief Read exactly size bytes, giving up after one second of silence.
     * @param fd Connected socket.
     * @param out Destination buffer of at least size bytes.
     * @param size Number of bytes to read.
     * @return true when size bytes were read; false on timeout, EOF or error.
     */
    bool read_full(int fd, std::uint8_t* out, std::size_t size) {
        std::size_t got = 0;
        while (got < size) {
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 1000) <= 0) {
                return false;
            }
            const ssize_t n = ::recv(fd, out + got, size - got, 0);
            if (n <= 0) {
                return false;
            }
            got += static_cast<std::size_t>(n);
        }
        return true;
    }

    /**
     * @brief Server thread: accept a client, read its packets and dispatch
     * them.
     *
     * Runs until m_stop is set. A read failure closes the connection, except
     * with m_keep_open_on_eof where the server then only stops reading.
     */
    void run() {
        while (!m_stop) {
            int conn;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                conn = m_conn;
            }
            if (conn < 0) {
                pollfd p{m_listen, POLLIN, 0};
                if (::poll(&p, 1, 20) > 0) {
                    const int accepted = ::accept(m_listen, nullptr, nullptr);
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_conn = accepted;
                }
                continue;
            }
            if (m_keep_open_on_eof && m_client_half_closed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            pollfd p{conn, POLLIN, 0};
            if (::poll(&p, 1, 20) <= 0) {
                continue;
            }
            std::uint8_t raw[sizeof(proto::LdnHeader)];
            if (!read_full(conn, raw, sizeof(raw))) {
                if (m_keep_open_on_eof) {
                    m_client_half_closed = true;
                } else {
                    close_connection();
                }
                continue;
            }
            proto::LdnHeader header{};
            std::memcpy(&header, raw, sizeof(header));
            std::vector<std::uint8_t> body(
                static_cast<std::size_t>(header.data_size));
            if (!body.empty() && !read_full(conn, body.data(), body.size())) {
                close_connection();
                continue;
            }
            dispatch(static_cast<proto::PacketId>(header.type), body);
        }
    }

    /**
     * @brief Record a packet and run the answers.
     *
     * Wakes the waiters, answers an Initialize with an Initialize when
     * m_answer_handshake is set, then runs the installed handler.
     *
     * @param id Packet id.
     * @param body Packet payload.
     */
    void dispatch(proto::PacketId id, const std::vector<std::uint8_t>& body) {
        Handler handler;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_received.emplace_back(id, body);
            handler = m_handler;
        }
        m_cv.notify_all();
        if (id == proto::PacketId::Initialize && m_answer_handshake) {
            proto::InitializeMessage reply{};
            send(proto::PacketId::Initialize, reply);
        }
        if (handler) {
            handler(*this, id, body);
        }
    }

    /// Listening socket.
    int m_listen = -1;
    /// Connected client socket, -1 when nobody is connected.
    int m_conn = -1;
    /// Port the listening socket is bound to.
    std::uint16_t m_port = 0;
    /// Set to stop the server thread.
    std::atomic<bool> m_stop{false};
    /// Whether an Initialize is answered with an Initialize.
    std::atomic<bool> m_answer_handshake{true};
    /// Whether an EOF from the client leaves the connection open.
    std::atomic<bool> m_keep_open_on_eof{false};
    /// Set once the client shut its write side down (see m_keep_open_on_eof).
    std::atomic<bool> m_client_half_closed{false};
    /// Protects m_conn, m_received and m_handler.
    std::mutex m_mutex;
    /// Notified for every received packet (see wait_for()).
    std::condition_variable m_cv;
    /// Every packet received, in order.
    std::vector<Packet> m_received;
    /// Script run for every received packet.
    Handler m_handler;
    /// Thread running run().
    std::thread m_thread;
};

// =============================================================================
// Fixtures
// =============================================================================

/**
 * @brief Point the global configuration at `port` and build the service.
 * @param port Loopback port of the FakeServer to connect to.
 * @param p2p True to leave the P2P path enabled (disable_p2p = false).
 * @return A service built exactly as the sysmodule builds it.
 */
std::unique_ptr<lm::ICommunicationService> make_service(
    std::uint16_t port, bool p2p) {
    ryu_ldn::config::Config cfg = ryu_ldn::config::get_default_config();
    std::snprintf(cfg.server.host, sizeof(cfg.server.host), "127.0.0.1");
    cfg.server.port = port;
    cfg.ldn.disable_p2p = !p2p;
    ryu_ldn::ipc::g_config = cfg;
    return std::make_unique<lm::ICommunicationService>(
        ams::ncm::ProgramId{GAME_ID});
}

/// A service wired to a FakeServer.
struct Env {
    /// Fake server the service talks to.
    FakeServer server;
    /// Service under test.
    std::unique_ptr<lm::ICommunicationService> svc;

    /**
     * @brief Build the server and a service pointing at it.
     * @param p2p Enable the P2P path (sets the inverse of ldn.disable_p2p).
     */
    explicit Env(bool p2p = false) : svc(make_service(server.port(), p2p)) {}
    /// Destroy the service before the server it talks to.
    ~Env() {
        svc.reset();
    }

    /**
     * @brief Access the service.
     * @return The service.
     */
    lm::ICommunicationService& operator*() {
        return *svc;
    }
    /**
     * @brief Access the service members.
     * @return The service.
     */
    lm::ICommunicationService* operator->() {
        return svc.get();
    }
};

/**
 * @brief Build a client process id.
 * @param pid Process id value.
 * @return The id holding pid.
 */
ams::sf::ClientProcessId process(std::uint64_t pid) {
    ams::sf::ClientProcessId id;
    id.process_id.value = pid;
    return id;
}

/**
 * @brief Read the state through GetState().
 * @param svc Service to query.
 * @return The reported state, 0xFFFFFFFF when GetState wrote nothing.
 */
std::uint32_t get_state(lm::ICommunicationService& svc) {
    std::uint32_t state = 0xFFFFFFFF;
    svc.GetState(ams::sf::Out<std::uint32_t>(state));
    return state;
}

/**
 * @brief Convert a CommState to the value GetState() reports.
 * @param s State.
 * @return Its numeric value.
 */
constexpr std::uint32_t state_of(lm::CommState s) {
    return static_cast<std::uint32_t>(s);
}

/**
 * @brief Build a NetworkInfo with `nodes` connected nodes.
 * @param nodes Number of connected nodes (node n has IP HOST_VIP + n).
 * @return The zero-initialised NetworkInfo (node capacity 8).
 */
lm::NetworkInfo make_info(std::uint8_t nodes) {
    lm::NetworkInfo info{};
    std::memset(&info, 0, sizeof(info));
    info.ldn.nodeCount = nodes;
    info.ldn.nodeCountMax = 8;
    for (std::uint8_t n = 0; n < nodes; ++n) {
        info.ldn.nodes[n].ipv4Address = HOST_VIP + n;
        info.ldn.nodes[n].nodeId = static_cast<std::int8_t>(n);
        info.ldn.nodes[n].isConnected = 1;
    }
    return info;
}

/**
 * @brief Build a scan filter with every field zero.
 * @return The zeroed filter.
 */
lm::ScanFilter make_filter() {
    lm::ScanFilter filter{};
    std::memset(&filter, 0, sizeof(filter));
    return filter;
}

/**
 * @brief Script: answer a CreateAccessPoint with the proxy config then
 *        Connected, and a Reject with a RejectReply.
 * @param server Server to reply through.
 * @param id Type of the packet the server just received.
 */
void host_script(
    FakeServer& server, proto::PacketId id, const std::vector<std::uint8_t>&) {
    if (id == proto::PacketId::CreateAccessPoint) {
        proto::ProxyConfig config{};
        config.proxy_ip = HOST_VIP;
        config.proxy_subnet_mask = VIP_MASK;
        server.send(proto::PacketId::ProxyConfig, config);
        const lm::NetworkInfo info = make_info(1);
        server.send(proto::PacketId::Connected, &info, sizeof(info));
    } else if (id == proto::PacketId::Reject) {
        server.send(proto::PacketId::RejectReply);
    }
}

/**
 * @brief Initialize + OpenAccessPoint + CreateNetwork against `env`.
 * @param env Service under test; its server runs host_script.
 * @return true when every step succeeded.
 */
bool host_a_network(Env& env) {
    env.server.set_handler(host_script);
    if (env->Initialize(process(1)).IsFailure()) {
        return false;
    }
    if (env->OpenAccessPoint().IsFailure()) {
        return false;
    }
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    config.networkConfig.nodeCountMax = 8;
    return env->CreateNetwork(config).IsSuccess();
}

/**
 * @brief Initialize + OpenStation against `env`.
 * @param env Service under test.
 * @return true when both calls succeeded.
 */
bool open_station(Env& env) {
    return env->Initialize(process(1)).IsSuccess() &&
           env->OpenStation().IsSuccess();
}

template <class F>
/**
 * @brief Wait until a condition holds.
 *
 * The receive thread processes packets on its own, so a test polls.
 *
 * @tparam F Callable returning a value convertible to bool.
 * @param cond Condition to poll every 2 ms.
 * @param timeout_ms Maximum wait in milliseconds.
 * @return Whether cond held at the end of the wait.
 */
bool eventually(F cond, int timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return cond();
}

/**
 * @brief Round trip through the receive thread.
 *
 * The server pings, the client echoes. TCP keeps the order, so once the echo
 * is back every packet sent before the call has been dispatched.
 *
 * @param env Environment to synchronise.
 * @return true when the ping was sent and its echo came back in 2 s.
 */
bool round_trip(Env& env) {
    proto::PingMessage ping{};
    ping.requester = 0;
    ping.id = 0xB0;
    const std::size_t before = env.server.count(proto::PacketId::Ping);
    return env.server.send(proto::PacketId::Ping, ping) &&
           env.server.wait_for(proto::PacketId::Ping, 2000, before + 1);
}

/**
 * @brief Shut down the write side of the client connection towards a port.
 *
 * The server keeps its side open: the client still looks connected, but
 * every send now fails with EPIPE. This is the deterministic way to reach the
 * send-failure branches (a plain server close is noticed by the receive
 * thread and turns the failure into "not connected" instead). The client's
 * descriptor is the one whose peer is the server's port; descriptors 3 to
 * 1023 are scanned.
 *
 * @param port Port of the server.
 * @return true when such a descriptor was found and shut down.
 */
bool cut_write_side_towards(std::uint16_t port) {
    for (int fd = 3; fd < 1024; ++fd) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &len) == 0 &&
            peer.sin_family == AF_INET && ntohs(peer.sin_port) == port) {
            return ::shutdown(fd, SHUT_WR) == 0;
        }
    }
    return false;
}

/**
 * @brief Make every send of the service fail while it stays connected.
 *
 * Makes the server keep its side open on EOF, then shuts the client write
 * side down with cut_write_side_towards().
 *
 * @param env Environment whose connection is cut.
 * @return true when the client descriptor was found and shut down.
 */
bool cut_the_client_write_side(Env& env) {
    env.server.keep_open_on_eof(true);
    return cut_write_side_towards(env.server.port());
}

// -----------------------------------------------------------------------------
// Scripted concurrency: the stand-in calls these hooks just before an event is
// cleared / a timed multi-wait starts, on whichever thread does it. A test arms
// one action for the thread that installed it; the action runs once, at that
// exact point of the service's code, which makes the narrow windows of the
// handshake and WaitForResponse reproducible instead of racy. An action returns
// true when it ran, false to stay armed for the next call.
// -----------------------------------------------------------------------------

/**
 * @brief An armed action and the thread it is armed for.
 */
struct HookSlot {
    /// Protects action and owner.
    std::mutex mutex;
    /// Action to run, empty when nothing is armed; returns false to stay armed.
    std::function<bool()> action;
    /// Thread the action is armed for.
    std::thread::id owner;
};

HookSlot g_clear_hook;           ///< before Event::Clear()
HookSlot g_timed_wait_hook;      ///< before TimedWaitAny()
HookSlot g_recursive_lock_hook;  ///< before a recursive Mutex is taken

/// Runs `action` once, on whichever thread next starts a thread that is not the
/// P2P connect worker itself (the worker's own receive thread, started from
/// inside P2pProxyClient::Connect). Unlike a HookSlot it has no owner thread:
/// the thread to hold is not the one the test runs on.
struct StartGate {
    /// Protects action.
    std::mutex mutex;
    /// Action to run once on the next thread start, or empty.
    std::function<void()> action;
};

/// The start gate shared by every thread start.
StartGate g_start_gate;

/**
 * @brief Run and disarm the start gate action for a starting thread.
 * @param thread_name Name of the thread being started; "p2p_connect" is
 *        ignored.
 */
void run_start_gate(const char* thread_name) {
    if (std::strcmp(thread_name, "p2p_connect") == 0) {
        return;
    }
    std::function<void()> action;
    {
        std::lock_guard<std::mutex> lock(g_start_gate.mutex);
        action = std::move(g_start_gate.action);
        g_start_gate.action = nullptr;
    }
    if (action) {
        action();
    }
}

/**
 * @brief Arm the start gate.
 * @param action Action to run once at the next thread start.
 */
void arm_start_gate(std::function<void()> action) {
    std::lock_guard<std::mutex> lock(g_start_gate.mutex);
    g_start_gate.action = std::move(action);
}

/**
 * @brief Run the action armed on a slot for the calling thread.
 *
 * The action is consumed; it is armed again when it returns false.
 *
 * @param slot Slot to run.
 */
void run_hook(HookSlot& slot) {
    std::function<bool()> action;
    {
        std::lock_guard<std::mutex> lock(slot.mutex);
        if (!slot.action || slot.owner != std::this_thread::get_id()) {
            return;
        }
        action = std::move(slot.action);
        slot.action = nullptr;
    }
    if (!action()) {
        std::lock_guard<std::mutex> lock(slot.mutex);
        if (!slot.action) {
            slot.action = std::move(action);
        }
    }
}

/// Arms `action` on `slot` for the calling thread until the object dies.
class ScopedHook {
public:
    /**
     * @brief Arm an action on a slot for the calling thread.
     * @param slot Slot to arm.
     * @param action Action to run; returns true when done, false to stay armed.
     */
    ScopedHook(HookSlot& slot, std::function<bool()> action) : m_slot(slot) {
        std::lock_guard<std::mutex> lock(m_slot.mutex);
        m_slot.action = std::move(action);
        m_slot.owner = std::this_thread::get_id();
    }
    /// Disarm the slot.
    ~ScopedHook() {
        std::lock_guard<std::mutex> lock(m_slot.mutex);
        m_slot.action = nullptr;
    }
    /// Not copyable: owns the arming of one slot.
    ScopedHook(const ScopedHook&) = delete;
    /// Not assignable: owns the arming of one slot.
    /// @return Never returns (deleted).
    ScopedHook& operator=(const ScopedHook&) = delete;

private:
    /// Slot armed by this object.
    HookSlot& m_slot;
};

/**
 * @brief Wait for the handshake to complete.
 *
 * Blocks the calling thread until the server has received an Initialize, then
 * gives the receive thread time to process everything the server answered.
 *
 * @param env Environment to wait on.
 * @return false when no Initialize arrived in 2 s.
 */
bool let_the_handshake_play_out(Env& env) {
    if (!env.server.wait_for(proto::PacketId::Initialize, 2000)) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    return true;
}

/**
 * @brief Build a server script answering a CreateAccessPoint late.
 *
 * After delay_ms, each id of `before` is sent as an empty packet, gap_ms
 * apart, and then the usual host_script() answer follows (ProxyConfig and
 * Connected).
 *
 * @param delay_ms Delay before the first packet.
 * @param before Ids of the empty packets sent before the real answer.
 * @param gap_ms Pause after each packet of `before`.
 * @return The script, to install with set_handler().
 */
FakeServer::Handler delayed_host_script(
    int delay_ms, std::vector<proto::PacketId> before, int gap_ms) {
    return [=](FakeServer& server,
               proto::PacketId id,
               const std::vector<std::uint8_t>& body) {
        if (id != proto::PacketId::CreateAccessPoint) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        for (const auto packet : before) {
            server.send(packet);
            std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
        }
        host_script(server, id, body);
    };
}

}  // namespace

// =============================================================================
// Lifecycle, no server involved
// =============================================================================

/// A new service is in the None state.
TEST(a_new_service_starts_uninitialized) {
    Env env;
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::None));
}

/// Initialize moves to Initialized and records the game and its pid in the
/// shared state.
TEST(initialize_moves_to_initialized_and_registers_the_game) {
    Env env;
    ASSERT_TRUE(env->Initialize(process(4242)).IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Initialized));
    ASSERT_TRUE(lm::SharedState::GetInstance().IsGameActive());
    ASSERT_EQ(lm::SharedState::GetInstance().GetLdnPid(), 4242u);
}

/// A second Initialize fails with the bad-state result.
TEST(a_second_initialize_is_refused) {
    Env env;
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_EQ(env->Initialize(process(1)).GetValue(), RC_BAD_STATE);
}

/// InitializeSystem2 initializes the service like Initialize does.
TEST(initialize_system2_records_the_error_state_and_initializes) {
    Env env;
    ASSERT_TRUE(env->InitializeSystem2(1, process(7)).IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Initialized));
}

/// Finalize returns to None and clears the game flag and pid of the shared
/// state.
TEST(finalize_returns_to_none_and_clears_the_shared_state) {
    Env env;
    ASSERT_TRUE(env->Initialize(process(9)).IsSuccess());
    ASSERT_TRUE(env->Finalize().IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::None));
    ASSERT_FALSE(lm::SharedState::GetInstance().IsGameActive());
    ASSERT_EQ(lm::SharedState::GetInstance().GetLdnPid(), 0u);
}

/// A second Finalize also succeeds.
TEST(finalize_twice_is_harmless) {
    Env env;
    ASSERT_TRUE(env->Initialize(process(9)).IsSuccess());
    ASSERT_TRUE(env->Finalize().IsSuccess());
    ASSERT_TRUE(env->Finalize().IsSuccess());
}

/// Initialize works again after Finalize and records the new pid.
TEST(initialize_can_be_repeated_after_finalize) {
    Env env;
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->Finalize().IsSuccess());
    ASSERT_TRUE(env->Initialize(process(2)).IsSuccess());
    ASSERT_EQ(lm::SharedState::GetInstance().GetLdnPid(), 2u);
}

/// Creating a service adds one active IPC session to the shared state and
/// destroying it removes it.
TEST(the_service_counts_as_one_ipc_session) {
    const std::uint32_t before =
        lm::SharedState::GetInstance().GetActiveSessionCount();
    {
        Env env;
        ASSERT_EQ(
            lm::SharedState::GetInstance().GetActiveSessionCount(), before + 1);
    }
    ASSERT_EQ(lm::SharedState::GetInstance().GetActiveSessionCount(), before);
}

// =============================================================================
// Queries, no server involved
// =============================================================================

/// InitializeSystem2 with error state 1 and no disconnect reason pending:
/// GetState succeeds (the reported state itself is not asserted).
TEST(get_state_succeeds_in_the_error_state_without_a_disconnect_reason) {
    Env env;
    ASSERT_TRUE(env->InitializeSystem2(1, process(1)).IsSuccess());
    // No disconnect reason yet: the error state alone is not reported.
    std::uint32_t state = 0;
    ASSERT_TRUE(env->GetState(ams::sf::Out<std::uint32_t>(state)).IsSuccess());
}

/// GetNetworkInfo outside a network returns an info with no nodes.
TEST(the_empty_network_info_is_zeroed) {
    Env env;
    lm::NetworkInfo info = make_info(3);
    ASSERT_TRUE(
        env->GetNetworkInfo(ams::sf::Out<lm::NetworkInfo>(info)).IsSuccess());
    ASSERT_EQ(info.ldn.nodeCount, 0);
}

/// GetDisconnectReason is 0 on a new service.
TEST(the_disconnect_reason_starts_at_none) {
    Env env;
    std::uint32_t reason = 99;
    ASSERT_TRUE(env->GetDisconnectReason(ams::sf::Out<std::uint32_t>(reason))
                    .IsSuccess());
    ASSERT_EQ(reason, 0u);
}

/// Outside a network the security parameter random bytes, the channel and
/// the maximum node count read zero.
TEST(the_security_parameter_and_network_config_of_an_empty_network_are_zero) {
    Env env;
    lm::SecurityParameter param{};
    std::memset(&param, 0xAA, sizeof(param));
    ASSERT_TRUE(
        env->GetSecurityParameter(ams::sf::Out<lm::SecurityParameter>(param))
            .IsSuccess());
    for (std::size_t i = 0; i < sizeof(param.unkRandom); ++i) {
        ASSERT_EQ(param.unkRandom[i], 0);
    }
    lm::NetworkConfig config{};
    std::memset(&config, 0xAA, sizeof(config));
    ASSERT_TRUE(env->GetNetworkConfig(ams::sf::Out<lm::NetworkConfig>(config))
                    .IsSuccess());
    ASSERT_EQ(config.channel, 0);
    ASSERT_EQ(config.nodeCountMax, 0);
}

/// AttachStateChangeEvent succeeds and hands out an unmanaged handle.
TEST(the_state_change_event_handle_is_handed_out) {
    Env env;
    ams::sf::CopyHandle handle;
    ASSERT_TRUE(
        env->AttachStateChangeEvent(ams::sf::Out<ams::sf::CopyHandle>(handle))
            .IsSuccess());
    ASSERT_FALSE(handle.managed);
}

/// Without a network, GetIpv4Address returns the address and mask of the
/// system network configuration.
TEST(the_ipv4_address_falls_back_to_the_system_network_configuration) {
    Env env;
    g_host_nifm_result = 0;
    g_host_nifm_address = htonl(0xC0A80119);
    g_host_nifm_netmask = htonl(0xFFFFFF00);
    std::uint32_t address = 0;
    std::uint32_t mask = 0;
    ASSERT_TRUE(env->GetIpv4Address(
                       ams::sf::Out<std::uint32_t>(address),
                       ams::sf::Out<std::uint32_t>(mask))
                    .IsSuccess());
    ASSERT_EQ(address, 0xC0A80119u);
    ASSERT_EQ(mask, 0xFFFFFF00u);
}

/// A failing system network query is returned as is and the outputs are left
/// untouched.
TEST(a_failing_system_network_query_is_passed_through) {
    Env env;
    g_host_nifm_result = MAKERESULT(0xCD, 3);
    std::uint32_t address = 0x12345678;
    std::uint32_t mask = 0x9ABCDEF0;
    const auto rc = env->GetIpv4Address(
        ams::sf::Out<std::uint32_t>(address),
        ams::sf::Out<std::uint32_t>(mask));
    g_host_nifm_result = 0;
    ASSERT_EQ(rc.GetValue(), MAKERESULT(0xCD, 3));
    ASSERT_EQ(address, 0x12345678u);
    ASSERT_EQ(mask, 0x9ABCDEF0u);
}

/// For an empty network every latest-update entry has no state change and
/// zero unknown bytes.
TEST(the_latest_update_reports_nothing_for_an_empty_network) {
    Env env;
    lm::NetworkInfo info{};
    lm::NodeLatestUpdate updates[10];
    std::memset(updates, 0xEE, sizeof(updates));
    ASSERT_TRUE(env->GetNetworkInfoLatestUpdate(
                       ams::sf::Out<lm::NetworkInfo>(info),
                       ams::sf::OutArray<lm::NodeLatestUpdate>(updates, 10))
                    .IsSuccess());
    for (const auto& u : updates) {
        ASSERT_EQ(
            u.stateChange,
            static_cast<std::uint8_t>(lm::NodeStateChange::None));
        for (const auto b : u._unk) {
            ASSERT_EQ(b, 0);
        }
    }
}

/// The latest-update command accepts an empty update array.
TEST(the_latest_update_accepts_an_empty_update_buffer) {
    Env env;
    lm::NetworkInfo info{};
    ASSERT_TRUE(env->GetNetworkInfoLatestUpdate(
                       ams::sf::Out<lm::NetworkInfo>(info),
                       ams::sf::OutArray<lm::NodeLatestUpdate>(nullptr, 0))
                    .IsSuccess());
}

/// SetWirelessControllerRestriction, AddAcceptFilterEntry and
/// ClearAcceptFilter succeed.
TEST(the_stub_commands_always_succeed) {
    ASSERT_TRUE(
        lm::ICommunicationService::SetWirelessControllerRestriction()
            .IsSuccess());
    ASSERT_TRUE(lm::ICommunicationService::AddAcceptFilterEntry().IsSuccess());
    ASSERT_TRUE(lm::ICommunicationService::ClearAcceptFilter().IsSuccess());
}

// =============================================================================
// Commands that need a server connection, issued without one
// =============================================================================

/**
 * @brief Scan before any server connection returns the not-connected result.
 */
TEST(scan_without_a_server_connection_is_refused) {
    Env env;
    std::uint32_t count = 7;
    lm::NetworkInfo results[4];
    const lm::ScanFilter filter = make_filter();
    ASSERT_EQ(
        env->Scan(
               ams::sf::Out<std::uint32_t>(count),
               ams::sf::OutArray<lm::NetworkInfo>(results, 4),
               0,
               filter)
            .GetValue(),
        RC_NOT_CONNECTED);
}

/**
 * @brief A Scan whose filter carries the wildcard local communication id (all
 * ones) is still processed, and without a connection it fails as not connected.
 */
TEST(scan_replaces_a_wildcard_local_communication_id) {
    Env env;
    std::uint32_t count = 0;
    lm::NetworkInfo results[4];
    lm::ScanFilter filter = make_filter();
    filter.networkId.intentId.localCommunicationId =
        static_cast<std::uint64_t>(-1);
    ASSERT_EQ(
        env->Scan(
               ams::sf::Out<std::uint32_t>(count),
               ams::sf::OutArray<lm::NetworkInfo>(results, 4),
               0,
               filter)
            .GetValue(),
        RC_NOT_CONNECTED);
}

/**
 * @brief CreateNetwork with a wildcard communication id but no server
 * connection returns the not-connected result.
 */
TEST(creating_a_network_without_a_server_connection_is_refused) {
    Env env;
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    config.networkConfig.intentId.localCommunicationId =
        static_cast<std::uint64_t>(-1);
    ASSERT_EQ(env->CreateNetwork(config).GetValue(), RC_NOT_CONNECTED);
}

/**
 * @brief Connect without a server connection returns the not-connected result.
 */
TEST(connecting_without_a_server_connection_is_refused) {
    Env env;
    lm::ConnectNetworkData data{};
    std::memset(&data, 0, sizeof(data));
    const lm::NetworkInfo target = make_info(1);
    ASSERT_EQ(env->Connect(data, target).GetValue(), RC_NOT_CONNECTED);
}

/**
 * @brief CreateNetworkPrivate and ConnectPrivate without a server connection
 * return the not-connected result.
 */
TEST(the_private_commands_without_a_server_connection_are_refused) {
    Env env;
    lm::CreateNetworkPrivateConfig create{};
    std::memset(&create, 0, sizeof(create));
    std::uint8_t addresses[4] = {};
    ASSERT_EQ(
        env->CreateNetworkPrivate(
               create, ams::sf::InPointerBuffer(addresses, sizeof(addresses)))
            .GetValue(),
        RC_NOT_CONNECTED);
    lm::ConnectPrivateData connect{};
    std::memset(&connect, 0, sizeof(connect));
    ASSERT_EQ(env->ConnectPrivate(connect).GetValue(), RC_NOT_CONNECTED);
}

/**
 * @brief Reject without a server connection returns the not-connected result.
 */
TEST(rejecting_a_node_outside_a_network_is_refused) {
    Env env;
    ASSERT_EQ(env->Reject(1).GetValue(), RC_NOT_CONNECTED);
}

/**
 * @brief DestroyNetwork, CloseAccessPoint, CloseStation, Disconnect and
 *        OpenAccessPoint called in the None state are refused (bad state, never
 * a crash).
 */
TEST(the_commands_of_the_wrong_state_are_refused) {
    Env env;
    ASSERT_EQ(env->DestroyNetwork().GetValue(), RC_BAD_STATE);
    ASSERT_EQ(env->CloseAccessPoint().GetValue(), RC_BAD_STATE);
    ASSERT_EQ(env->CloseStation().GetValue(), RC_BAD_STATE);
    ASSERT_EQ(env->Disconnect().GetValue(), RC_BAD_STATE);
    ASSERT_EQ(env->OpenAccessPoint().GetValue() != 0, true);
}

/**
 * @brief OpenStation and OpenAccessPoint before Initialize return the bad-state
 *        result.
 */
TEST(opening_before_initializing_is_refused) {
    Env env;
    ASSERT_EQ(env->OpenStation().GetValue(), RC_BAD_STATE);
    ASSERT_EQ(env->OpenAccessPoint().GetValue(), RC_BAD_STATE);
}

/**
 * @brief Outside a hosted network SetAdvertiseData (also with an empty buffer)
 * and SetStationAcceptPolicy only keep the values locally: nothing is sent to
 *        the server.
 */
TEST(advertise_data_and_accept_policy_are_kept_locally_outside_a_network) {
    Env env;
    const std::uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_TRUE(
        env->SetAdvertiseData(ams::sf::InAutoSelectBuffer(data, sizeof(data)))
            .IsSuccess());
    ASSERT_TRUE(env->SetAdvertiseData(ams::sf::InAutoSelectBuffer(nullptr, 0))
                    .IsSuccess());
    ASSERT_TRUE(env->SetStationAcceptPolicy(1).IsSuccess());
    ASSERT_EQ(env.server.count(proto::PacketId::SetAdvertiseData), 0u);
    ASSERT_EQ(env.server.count(proto::PacketId::SetAcceptPolicy), 0u);
}

// =============================================================================
// Connecting to the server
// =============================================================================

/**
 * @brief OpenStation connects to the server, completes the Initialize handshake
 * and moves the service (and the shared state) to Station.
 */
TEST(opening_a_station_connects_to_the_server_and_completes_the_handshake) {
    Env env;
    ASSERT_TRUE(open_station(env));
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Station));
    ASSERT_TRUE(env.server.wait_for(proto::PacketId::Initialize, 1000));
    ASSERT_EQ(
        lm::SharedState::GetInstance().GetLdnState(), lm::CommState::Station);
}

/**
 * @brief OpenAccessPoint connects to the server and moves the service (and the
 *        shared state) to AccessPoint.
 */
TEST(opening_an_access_point_connects_to_the_server) {
    Env env;
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::AccessPoint));
    ASSERT_EQ(
        lm::SharedState::GetInstance().GetLdnState(),
        lm::CommState::AccessPoint);
}

/**
 * @brief Closing and reopening a station reuses the live server connection: the
 *        server sees a single Initialize.
 */
TEST(opening_a_station_twice_keeps_the_existing_connection) {
    Env env;
    ASSERT_TRUE(open_station(env));
    ASSERT_TRUE(env->CloseStation().IsSuccess());
    ASSERT_TRUE(env->OpenStation().IsSuccess());
    ASSERT_EQ(env.server.count(proto::PacketId::Initialize), 1u);
}

/**
 * @brief When nothing listens on the server port, OpenStation and
 * OpenAccessPoint fail as not connected and the state goes back to Initialized.
 */
TEST(a_server_that_is_not_listening_refuses_the_connection) {
    std::uint16_t dead_port = 0;
    {
        FakeServer gone;
        dead_port = gone.port();
    }
    auto svc = make_service(dead_port, false);
    ASSERT_TRUE(svc->Initialize(process(1)).IsSuccess());
    ASSERT_EQ(svc->OpenStation().GetValue(), RC_NOT_CONNECTED);
    // The state transition is rolled back.
    ASSERT_EQ(get_state(*svc), state_of(lm::CommState::Initialized));
    ASSERT_EQ(svc->OpenAccessPoint().GetValue(), RC_NOT_CONNECTED);
    ASSERT_EQ(get_state(*svc), state_of(lm::CommState::Initialized));
}

/**
 * @brief A server that accepts the TCP connection but never answers the
 * Initialize makes OpenStation fail on the handshake and roll back to
 * Initialized.
 */
TEST(a_server_that_never_answers_the_handshake_times_out) {
    Env env;
    env.server.answer_handshake(false);
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_EQ(env->OpenStation().GetValue(), RC_HANDSHAKE_OR_REJECT);
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Initialized));
}

// =============================================================================
// Scanning
// =============================================================================

namespace {

/**
 * @brief Script: answer a Scan with `replies` ScanReply then ScanReplyEnd.
 * @param replies Number of ScanReply packets to send.
 * @param scene Scene id written into every reply.
 * @return The handler to install with FakeServer::set_handler.
 */
FakeServer::Handler scan_script(int replies, std::uint64_t scene = 0) {
    return [replies, scene](
               FakeServer& server,
               proto::PacketId id,
               const std::vector<std::uint8_t>&) {
        if (id != proto::PacketId::Scan) {
            return;
        }
        for (int i = 0; i < replies; ++i) {
            lm::NetworkInfo info =
                make_info(static_cast<std::uint8_t>(1 + (i % 4)));
            info.networkId.intentId.sceneId = static_cast<std::uint16_t>(scene);
            info.common.channel = static_cast<std::int16_t>(i);
            server.send(proto::PacketId::ScanReply, &info, sizeof(info));
        }
        server.send(proto::PacketId::ScanReplyEnd);
    };
}

/**
 * @brief Issue a Scan and return its raw result.
 * @param env Service under test.
 * @param[out] out Buffer receiving the networks.
 * @param capacity Capacity of `out`, in elements.
 * @param filter Scan filter to pass.
 * @param[out] count Number of networks returned (preset to 99 first).
 * @return The raw result code of Scan.
 */
std::uint32_t run_scan(
    Env& env,
    lm::NetworkInfo* out,
    std::size_t capacity,
    lm::ScanFilter filter,
    std::uint32_t& count) {
    count = 99;
    return env
        ->Scan(
            ams::sf::Out<std::uint32_t>(count),
            ams::sf::OutArray<lm::NetworkInfo>(out, capacity),
            0,
            filter)
        .GetValue();
}

}  // namespace

/**
 * @brief Scan returns every ScanReply the server sent before ScanReplyEnd, with
 * the game's communication id filled in, sending a single Scan packet.
 */
TEST(scan_returns_every_network_the_server_reports) {
    Env env;
    env.server.set_handler(scan_script(2));
    ASSERT_TRUE(open_station(env));
    lm::NetworkInfo results[8];
    std::uint32_t count = 0;
    ASSERT_EQ(run_scan(env, results, 8, make_filter(), count), 0u);
    ASSERT_EQ(count, 2u);
    ASSERT_EQ(results[1].common.channel, 1);
    ASSERT_EQ(results[0].networkId.intentId.localCommunicationId, GAME_ID);
    ASSERT_EQ(env.server.count(proto::PacketId::Scan), 1u);
}

/**
 * @brief Scan stops at 8 results even when the server reports more and the
 * caller's buffer is larger.
 */
TEST(scan_keeps_at_most_eight_results) {
    Env env;
    env.server.set_handler(scan_script(12));
    ASSERT_TRUE(open_station(env));
    lm::NetworkInfo results[16];
    std::uint32_t count = 0;
    ASSERT_EQ(run_scan(env, results, 16, make_filter(), count), 0u);
    ASSERT_EQ(count, 8u);
}

/**
 * @brief Scan never returns more networks than the caller's buffer holds.
 */
TEST(scan_is_bounded_by_the_caller_buffer) {
    Env env;
    env.server.set_handler(scan_script(6));
    ASSERT_TRUE(open_station(env));
    lm::NetworkInfo results[3];
    std::uint32_t count = 0;
    ASSERT_EQ(run_scan(env, results, 3, make_filter(), count), 0u);
    ASSERT_EQ(count, 3u);
}

/**
 * @brief The scene id of every scan reply is rewritten to the one in the game's
 *        filter, so the game recognises the networks it asked for.
 */
TEST(scan_rewrites_the_scene_id_of_the_replies_to_the_filtered_one) {
    Env env;
    env.server.set_handler(scan_script(1, 0x55));
    ASSERT_TRUE(open_station(env));
    lm::NetworkInfo results[2];
    std::uint32_t count = 0;
    lm::ScanFilter filter = make_filter();
    filter.networkId.intentId.sceneId = 0x42;
    ASSERT_EQ(run_scan(env, results, 2, filter, count), 0u);
    ASSERT_EQ(count, 1u);
    ASSERT_EQ(results[0].networkId.intentId.sceneId, 0x42);
}

/**
 * @brief A Scan with the wildcard local communication id is sent for the
 * running game and still returns the server's networks.
 */
TEST(scan_with_a_wildcard_local_id_asks_for_the_running_game) {
    Env env;
    env.server.set_handler(scan_script(1));
    ASSERT_TRUE(open_station(env));
    lm::NetworkInfo results[2];
    std::uint32_t count = 0;
    lm::ScanFilter filter = make_filter();
    filter.networkId.intentId.localCommunicationId =
        static_cast<std::uint64_t>(-1);
    ASSERT_EQ(run_scan(env, results, 2, filter, count), 0u);
    ASSERT_EQ(count, 1u);
}

/**
 * @brief A NetworkError answering a Scan makes Scan fail with the timeout
 * result instead of returning networks.
 */
TEST(scan_fails_when_the_server_answers_with_an_error) {
    Env env;
    env.server.set_handler([](FakeServer& server,
                              proto::PacketId id,
                              const std::vector<std::uint8_t>&) {
        if (id == proto::PacketId::Scan) {
            proto::NetworkErrorMessage err{};
            err.error_code = 2;
            server.send(proto::PacketId::NetworkError, err);
        }
    });
    ASSERT_TRUE(open_station(env));
    lm::NetworkInfo results[2];
    std::uint32_t count = 0;
    ASSERT_EQ(run_scan(env, results, 2, make_filter(), count), RC_TIMEOUT);
}

/**
 * @brief When the server never answers, Scan ends at its (shortened) timeout
 * and succeeds with zero results.
 */
TEST(scan_returns_nothing_when_the_server_stays_silent) {
    Env env;
    ASSERT_TRUE(open_station(env));
    lm::NetworkInfo results[2];
    std::uint32_t count = 5;
    ASSERT_EQ(run_scan(env, results, 2, make_filter(), count), 0u);
    ASSERT_EQ(count, 0u);
}

/**
 * @brief ScanPrivate returns the same networks as Scan.
 */
TEST(scan_private_behaves_like_scan) {
    Env env;
    env.server.set_handler(scan_script(2));
    ASSERT_TRUE(open_station(env));
    lm::NetworkInfo results[4];
    std::uint32_t count = 0;
    ASSERT_TRUE(env->ScanPrivate(
                       ams::sf::Out<std::uint32_t>(count),
                       ams::sf::OutArray<lm::NetworkInfo>(results, 4),
                       0,
                       make_filter())
                    .IsSuccess());
    ASSERT_EQ(count, 2u);
}

/**
 * @brief Once the receive thread has seen the server drop the connection, Scan
 *        fails; Finalize still succeeds afterwards.
 */
TEST(scan_reports_a_dropped_connection_as_a_send_failure) {
    Env env;
    ASSERT_TRUE(open_station(env));
    env.server.drop();
    lm::NetworkInfo results[2];
    std::uint32_t count = 0;
    // The receive thread notices the EOF on its own; once it did, the client
    // is no longer connected and Scan is refused.
    ASSERT_TRUE(eventually(
        [&] { return run_scan(env, results, 2, make_filter(), count) != 0u; }));
    ASSERT_TRUE(env->Finalize().IsSuccess());
}

/**
 * @brief A station that stays idle after a scan is disconnected from the server
 * by the inactivity timeout (6 s of real time).
 */
TEST(the_inactivity_timeout_disconnects_an_idle_station) {
    Env env;
    env.server.set_handler(scan_script(1));
    ASSERT_TRUE(open_station(env));
    ASSERT_TRUE(env.server.has_client());
    // A finished scan arms the inactivity timeout.
    lm::NetworkInfo results[2];
    std::uint32_t count = 0;
    ASSERT_EQ(run_scan(env, results, 2, make_filter(), count), 0u);
    // NetworkTimeout counts real time (6 s), not the divided waits.
    ASSERT_TRUE(eventually([&] { return !env.server.has_client(); }, 12000));
    ASSERT_TRUE(env->Finalize().IsSuccess());
}

// =============================================================================
// Hosting
// =============================================================================

/**
 * @brief CreateNetwork builds the access point from the server's ProxyConfig
 * and Connected: state AccessPointCreated, the game's communication id, a
 * usable channel (6) when the server left 0, and the virtual address and mask.
 */
TEST(creating_a_network_sets_up_the_access_point_from_the_server_answer) {
    Env env;
    ASSERT_TRUE(host_a_network(env));
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::AccessPointCreated));
    ASSERT_EQ(
        lm::SharedState::GetInstance().GetLdnState(),
        lm::CommState::AccessPointCreated);

    lm::NetworkInfo info{};
    ASSERT_TRUE(
        env->GetNetworkInfo(ams::sf::Out<lm::NetworkInfo>(info)).IsSuccess());
    ASSERT_EQ(info.ldn.nodeCount, 1);
    ASSERT_EQ(info.networkId.intentId.localCommunicationId, GAME_ID);
    // The server left the channel at 0: the service picks a usable one.
    ASSERT_EQ(info.common.channel, 6);

    std::uint32_t address = 0;
    std::uint32_t mask = 0;
    ASSERT_TRUE(env->GetIpv4Address(
                       ams::sf::Out<std::uint32_t>(address),
                       ams::sf::Out<std::uint32_t>(mask))
                    .IsSuccess());
    ASSERT_EQ(address, HOST_VIP);
    ASSERT_EQ(mask, VIP_MASK);
}

/**
 * @brief Advertise data set before CreateNetwork is included in the
 *        CreateAccessPoint request.
 */
TEST(creating_a_network_forwards_the_advertise_data_buffered_before_it) {
    Env env;
    const std::uint8_t data[4] = {9, 8, 7, 6};
    ASSERT_TRUE(
        env->SetAdvertiseData(ams::sf::InAutoSelectBuffer(data, sizeof(data)))
            .IsSuccess());
    ASSERT_TRUE(host_a_network(env));
    ASSERT_TRUE(env.server.wait_for(proto::PacketId::CreateAccessPoint, 1000));
    const auto payload = env.server.last(proto::PacketId::CreateAccessPoint);
    ASSERT_TRUE(payload.size() >= sizeof(data));
    bool found = false;
    for (std::size_t i = 0; i + sizeof(data) <= payload.size() && !found; ++i) {
        found = std::memcmp(payload.data() + i, data, sizeof(data)) == 0;
    }
    ASSERT_TRUE(found);
}

/**
 * @brief Once the network is hosted, SetAdvertiseData and
 * SetStationAcceptPolicy are forwarded to the server with the exact bytes.
 */
TEST(advertise_data_and_accept_policy_reach_the_server_once_hosting) {
    Env env;
    ASSERT_TRUE(host_a_network(env));
    const std::uint8_t data[6] = {1, 2, 3, 4, 5, 6};
    ASSERT_TRUE(
        env->SetAdvertiseData(ams::sf::InAutoSelectBuffer(data, sizeof(data)))
            .IsSuccess());
    ASSERT_TRUE(env->SetStationAcceptPolicy(2).IsSuccess());
    ASSERT_TRUE(env.server.wait_for(proto::PacketId::SetAdvertiseData, 1000));
    ASSERT_TRUE(env.server.wait_for(proto::PacketId::SetAcceptPolicy, 1000));
    const auto advertise = env.server.last(proto::PacketId::SetAdvertiseData);
    ASSERT_EQ(advertise.size(), sizeof(data));
    ASSERT_EQ(std::memcmp(advertise.data(), data, sizeof(data)), 0);
    const auto policy = env.server.last(proto::PacketId::SetAcceptPolicy);
    ASSERT_EQ(policy.size(), 1u);
    ASSERT_EQ(policy[0], 2);
}

/**
 * @brief When the server never answers CreateAccessPoint, CreateNetwork times
 * out and the state goes back to AccessPoint.
 */
TEST(a_server_that_never_confirms_the_network_times_out_and_rolls_back) {
    Env env;
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    ASSERT_EQ(env->CreateNetwork(config).GetValue(), RC_TIMEOUT);
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::AccessPoint));
}

/**
 * @brief CreateNetwork while a network already exists returns the bad-state
 * result.
 */
TEST(creating_a_network_twice_is_refused) {
    Env env;
    ASSERT_TRUE(host_a_network(env));
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    ASSERT_EQ(env->CreateNetwork(config).GetValue(), RC_BAD_STATE);
}

/**
 * @brief GetNetworkInfoLatestUpdate reports Connect for a new node, nothing for
 * an unchanged snapshot, and Disconnect for a node that left after a
 *        SyncNetwork.
 */
TEST(the_latest_update_reports_nodes_joining_and_leaving) {
    Env env;
    ASSERT_TRUE(host_a_network(env));
    lm::NetworkInfo info{};
    lm::NodeLatestUpdate updates[4];
    ASSERT_TRUE(env->GetNetworkInfoLatestUpdate(
                       ams::sf::Out<lm::NetworkInfo>(info),
                       ams::sf::OutArray<lm::NodeLatestUpdate>(updates, 4))
                    .IsSuccess());
    ASSERT_EQ(
        updates[0].stateChange,
        static_cast<std::uint8_t>(lm::NodeStateChange::Connect));
    ASSERT_EQ(
        updates[1].stateChange,
        static_cast<std::uint8_t>(lm::NodeStateChange::None));

    // Same snapshot again: nothing changed.
    ASSERT_TRUE(env->GetNetworkInfoLatestUpdate(
                       ams::sf::Out<lm::NetworkInfo>(info),
                       ams::sf::OutArray<lm::NodeLatestUpdate>(updates, 4))
                    .IsSuccess());
    ASSERT_EQ(
        updates[0].stateChange,
        static_cast<std::uint8_t>(lm::NodeStateChange::None));

    // A second node joins, the first one leaves.
    lm::NetworkInfo next = make_info(2);
    next.ldn.nodes[0].isConnected = 0;
    ASSERT_TRUE(
        env.server.send(proto::PacketId::SyncNetwork, &next, sizeof(next)));
    ASSERT_TRUE(eventually([&] {
        env->GetNetworkInfoLatestUpdate(
            ams::sf::Out<lm::NetworkInfo>(info),
            ams::sf::OutArray<lm::NodeLatestUpdate>(updates, 4));
        return updates[0].stateChange !=
               static_cast<std::uint8_t>(lm::NodeStateChange::None);
    }));
    ASSERT_EQ(
        updates[0].stateChange,
        static_cast<std::uint8_t>(lm::NodeStateChange::Disconnect));
}

/**
 * @brief GetNetworkInfoLatestUpdate neither writes beyond the caller's update
 *        buffer nor fails with a buffer smaller than the node table.
 */
TEST(the_latest_update_ignores_nodes_beyond_the_buffer) {
    Env env;
    ASSERT_TRUE(host_a_network(env));
    lm::NetworkInfo info{};
    lm::NodeLatestUpdate updates[10];
    std::memset(updates, 0xEE, sizeof(updates));
    ASSERT_TRUE(env->GetNetworkInfoLatestUpdate(
                       ams::sf::Out<lm::NetworkInfo>(info),
                       ams::sf::OutArray<lm::NodeLatestUpdate>(updates, 10))
                    .IsSuccess());
    ASSERT_EQ(updates[8].stateChange, 0);
    ASSERT_EQ(updates[9].stateChange, 0);
    ASSERT_TRUE(env->GetNetworkInfoLatestUpdate(
                       ams::sf::Out<lm::NetworkInfo>(info),
                       ams::sf::OutArray<lm::NodeLatestUpdate>(updates, 1))
                    .IsSuccess());
}

/**
 * @brief DestroyNetwork goes back to AccessPoint and empties the network info;
 *        CloseAccessPoint then returns to Initialized.
 */
TEST(destroying_the_network_returns_to_the_access_point_state) {
    Env env;
    ASSERT_TRUE(host_a_network(env));
    ASSERT_TRUE(env->DestroyNetwork().IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::AccessPoint));
    lm::NetworkInfo info = make_info(3);
    ASSERT_TRUE(
        env->GetNetworkInfo(ams::sf::Out<lm::NetworkInfo>(info)).IsSuccess());
    ASSERT_EQ(info.ldn.nodeCount, 0);
    ASSERT_TRUE(env->CloseAccessPoint().IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Initialized));
}

/**
 * @brief Finalize while hosting returns to None (it drops the server
 * connection).
 */
TEST(finalizing_while_hosting_drops_the_server_connection) {
    Env env;
    ASSERT_TRUE(host_a_network(env));
    ASSERT_TRUE(env->Finalize().IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::None));
}

/**
 * @brief CreateNetworkPrivate sends a CreateAccessPointPrivate and ends in
 *        AccessPointCreated when the server answers Connected.
 */
TEST(a_private_network_is_created_with_its_address_list) {
    Env env;
    env.server.set_handler([](FakeServer& server,
                              proto::PacketId id,
                              const std::vector<std::uint8_t>&) {
        if (id == proto::PacketId::CreateAccessPointPrivate) {
            const lm::NetworkInfo info = make_info(1);
            server.send(proto::PacketId::Connected, &info, sizeof(info));
        }
    });
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    lm::CreateNetworkPrivateConfig config{};
    std::memset(&config, 0, sizeof(config));
    std::uint8_t addresses[sizeof(proto::AddressList)] = {};
    ASSERT_TRUE(
        env->CreateNetworkPrivate(
               config, ams::sf::InPointerBuffer(addresses, sizeof(addresses)))
            .IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::AccessPointCreated));
    ASSERT_TRUE(
        env.server.wait_for(proto::PacketId::CreateAccessPointPrivate, 1000));
}

/**
 * @brief CreateNetworkPrivate accepts an address list buffer shorter than the
 *        protocol's AddressList.
 */
TEST(a_private_network_with_a_short_address_list_is_still_created) {
    Env env;
    env.server.set_handler([](FakeServer& server,
                              proto::PacketId id,
                              const std::vector<std::uint8_t>&) {
        if (id == proto::PacketId::CreateAccessPointPrivate) {
            const lm::NetworkInfo info = make_info(1);
            server.send(proto::PacketId::Connected, &info, sizeof(info));
        }
    });
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    lm::CreateNetworkPrivateConfig config{};
    std::memset(&config, 0, sizeof(config));
    std::uint8_t addresses[2] = {};
    ASSERT_TRUE(
        env->CreateNetworkPrivate(
               config, ams::sf::InPointerBuffer(addresses, sizeof(addresses)))
            .IsSuccess());
}

/**
 * @brief When the server never confirms a private network, CreateNetworkPrivate
 *        times out and rolls back to AccessPoint.
 */
TEST(a_private_network_the_server_never_confirms_times_out) {
    Env env;
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    lm::CreateNetworkPrivateConfig config{};
    std::memset(&config, 0, sizeof(config));
    std::uint8_t addresses[sizeof(proto::AddressList)] = {};
    ASSERT_EQ(
        env->CreateNetworkPrivate(
               config, ams::sf::InPointerBuffer(addresses, sizeof(addresses)))
            .GetValue(),
        RC_TIMEOUT);
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::AccessPoint));
}

// =============================================================================
// Joining
// =============================================================================

namespace {

/**
 * @brief Script: answer Connect and ConnectPrivate with a 2-node Connected.
 * @param server Server to reply through.
 * @param id Type of the packet the server just received.
 */
void join_script(
    FakeServer& server, proto::PacketId id, const std::vector<std::uint8_t>&) {
    if (id == proto::PacketId::Connect ||
        id == proto::PacketId::ConnectPrivate) {
        const lm::NetworkInfo info = make_info(2);
        server.send(proto::PacketId::Connected, &info, sizeof(info));
    }
}

/**
 * @brief A zeroed ConnectNetworkData.
 * @return The connect data.
 */
lm::ConnectNetworkData make_connect() {
    lm::ConnectNetworkData data{};
    std::memset(&data, 0, sizeof(data));
    return data;
}

}  // namespace

/**
 * @brief Connect sends a Connect, joins the network from the server's Connected
 *        (state StationConnected, node count from the answer).
 */
TEST(connecting_to_a_network_joins_it) {
    Env env;
    env.server.set_handler(join_script);
    ASSERT_TRUE(open_station(env));
    const lm::NetworkInfo target = make_info(1);
    ASSERT_TRUE(env->Connect(make_connect(), target).IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::StationConnected));
    ASSERT_EQ(
        lm::SharedState::GetInstance().GetLdnState(),
        lm::CommState::StationConnected);
    ASSERT_TRUE(env.server.wait_for(proto::PacketId::Connect, 1000));
    lm::NetworkInfo info{};
    ASSERT_TRUE(
        env->GetNetworkInfo(ams::sf::Out<lm::NetworkInfo>(info)).IsSuccess());
    ASSERT_EQ(info.ldn.nodeCount, 2);
}

/**
 * @brief Connect fails with the timeout result when the server never answers.
 */
TEST(connecting_to_a_network_that_does_not_answer_times_out) {
    Env env;
    ASSERT_TRUE(open_station(env));
    const lm::NetworkInfo target = make_info(1);
    ASSERT_EQ(env->Connect(make_connect(), target).GetValue(), RC_TIMEOUT);
}

/**
 * @brief A second Connect while joined returns the bad-state result.
 */
TEST(connecting_while_already_joined_is_refused) {
    Env env;
    env.server.set_handler(join_script);
    ASSERT_TRUE(open_station(env));
    const lm::NetworkInfo target = make_info(1);
    ASSERT_TRUE(env->Connect(make_connect(), target).IsSuccess());
    ASSERT_EQ(env->Connect(make_connect(), target).GetValue(), RC_BAD_STATE);
}

/**
 * @brief Disconnect notifies the server, goes back to Station and records the
 * User disconnect reason.
 */
TEST(disconnecting_tells_the_server_and_goes_back_to_the_station_state) {
    Env env;
    env.server.set_handler(join_script);
    ASSERT_TRUE(open_station(env));
    const lm::NetworkInfo target = make_info(1);
    ASSERT_TRUE(env->Connect(make_connect(), target).IsSuccess());
    ASSERT_TRUE(env->Disconnect().IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Station));
    ASSERT_TRUE(env.server.wait_for(proto::PacketId::Disconnect, 1000));
    std::uint32_t reason = 0;
    ASSERT_TRUE(env->GetDisconnectReason(ams::sf::Out<std::uint32_t>(reason))
                    .IsSuccess());
    ASSERT_EQ(reason, static_cast<std::uint32_t>(lm::DisconnectReason::User));
    ASSERT_TRUE(env->CloseStation().IsSuccess());
}

/**
 * @brief ConnectPrivate sends a ConnectPrivate and joins like Connect.
 */
TEST(a_private_connect_joins_the_network) {
    Env env;
    env.server.set_handler(join_script);
    ASSERT_TRUE(open_station(env));
    lm::ConnectPrivateData data{};
    std::memset(&data, 0, sizeof(data));
    ASSERT_TRUE(env->ConnectPrivate(data).IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::StationConnected));
    ASSERT_TRUE(env.server.wait_for(proto::PacketId::ConnectPrivate, 1000));
}

/**
 * @brief ConnectPrivate fails with the timeout result when the server never
 *        answers.
 */
TEST(a_private_connect_that_does_not_answer_times_out) {
    Env env;
    ASSERT_TRUE(open_station(env));
    lm::ConnectPrivateData data{};
    std::memset(&data, 0, sizeof(data));
    ASSERT_EQ(env->ConnectPrivate(data).GetValue(), RC_TIMEOUT);
}

// =============================================================================
// Rejecting a node
// =============================================================================

/**
 * @brief Reject sends a RejectRequest with the node id and the Rejected reason
 * and succeeds on the server's RejectReply.
 */
TEST(rejecting_a_node_succeeds_when_the_server_acknowledges) {
    Env env;
    ASSERT_TRUE(host_a_network(env));
    ASSERT_TRUE(env->Reject(3).IsSuccess());
    const auto payload = env.server.last(proto::PacketId::Reject);
    ASSERT_EQ(payload.size(), sizeof(proto::RejectRequest));
    proto::RejectRequest request{};
    std::memcpy(&request, payload.data(), sizeof(request));
    ASSERT_EQ(request.node_id, 3u);
    ASSERT_EQ(
        request.disconnect_reason,
        static_cast<std::uint32_t>(lm::DisconnectReason::Rejected));
}

/**
 * @brief A NetworkError received before Reject is still stored when the
 * RejectReply arrives, so Reject reports it as an error.
 */
TEST(rejecting_a_node_reports_a_server_error) {
    Env env;
    env.server.set_handler([](FakeServer& server,
                              proto::PacketId id,
                              const std::vector<std::uint8_t>& body) {
        if (id == proto::PacketId::CreateAccessPoint) {
            host_script(server, id, body);
        } else if (id == proto::PacketId::Reject) {
            server.send(proto::PacketId::RejectReply);
        }
    });
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    ASSERT_TRUE(env->CreateNetwork(config).IsSuccess());
    // The error is stored by the receive thread before Reject runs (Reject only
    // clears the event, not the stored code), so the RejectReply that follows
    // is reported as an error, whatever the interleaving.
    proto::NetworkErrorMessage err{};
    err.error_code = 2;
    ASSERT_TRUE(env.server.send(proto::PacketId::NetworkError, err));
    ASSERT_TRUE(round_trip(env));
    ASSERT_EQ(env->Reject(1).GetValue(), RC_HANDSHAKE_OR_REJECT);
}

/**
 * @brief Reject without any server answer fails with the not-connected result
 * when the wait ends.
 */
TEST(rejecting_a_node_times_out_when_the_server_stays_silent) {
    Env env;
    env.server.set_handler([](FakeServer& server,
                              proto::PacketId id,
                              const std::vector<std::uint8_t>& body) {
        if (id == proto::PacketId::CreateAccessPoint) {
            host_script(server, id, body);
        }
    });
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    ASSERT_TRUE(env->CreateNetwork(config).IsSuccess());
    ASSERT_EQ(env->Reject(1).GetValue(), RC_NOT_CONNECTED);
}

// =============================================================================
// Packets pushed by the server
// =============================================================================

/**
 * @brief A Disconnect from the server is consumed by the client layer and never
 *        reaches the service: the disconnect reason stays None.
 */
TEST(a_server_disconnect_is_consumed_by_the_client_layer) {
    Env env;
    ASSERT_TRUE(host_a_network(env));
    ASSERT_TRUE(env.server.send(proto::PacketId::Disconnect));
    ASSERT_TRUE(round_trip(env));
    std::uint32_t reason = 99;
    env->GetDisconnectReason(ams::sf::Out<std::uint32_t>(reason));
    // RyuLdnClient::handle_packet consumes a server Disconnect for its own
    // connection state machine and never forwards it, so the
    // PacketId::Disconnect case of HandleServerPacket is not reached through
    // the client and the reason stays None.
    ASSERT_EQ(reason, static_cast<std::uint32_t>(lm::DisconnectReason::None));
}

/**
 * @brief A Reject pushed by the server stores the disconnect reason it carries.
 */
TEST(a_server_reject_records_the_reason_it_carries) {
    Env env;
    ASSERT_TRUE(open_station(env));
    proto::RejectRequest request{};
    request.node_id = 1;
    request.disconnect_reason =
        static_cast<std::uint32_t>(lm::DisconnectReason::DestroyedBySystem);
    ASSERT_TRUE(env.server.send(proto::PacketId::Reject, request));
    ASSERT_TRUE(eventually([&] {
        std::uint32_t reason = 0;
        env->GetDisconnectReason(ams::sf::Out<std::uint32_t>(reason));
        return reason == static_cast<std::uint32_t>(
                             lm::DisconnectReason::DestroyedBySystem);
    }));
}

/**
 * @brief A payload-less Reject from the server is recorded as the Rejected
 * reason.
 */
TEST(a_server_reject_without_a_payload_means_rejected) {
    Env env;
    ASSERT_TRUE(open_station(env));
    ASSERT_TRUE(env.server.send(proto::PacketId::Reject));
    ASSERT_TRUE(eventually([&] {
        std::uint32_t reason = 0;
        env->GetDisconnectReason(ams::sf::Out<std::uint32_t>(reason));
        return reason ==
               static_cast<std::uint32_t>(lm::DisconnectReason::Rejected);
    }));
}

/**
 * @brief A server ping that asks for an echo (requester 0) is echoed back with
 * the same id.
 */
TEST(a_server_ping_is_echoed_back) {
    Env env;
    ASSERT_TRUE(open_station(env));
    proto::PingMessage ping{};
    ping.requester = 0;
    ping.id = 77;
    const std::size_t before = env.server.count(proto::PacketId::Ping);
    ASSERT_TRUE(env.server.send(proto::PacketId::Ping, ping));
    ASSERT_TRUE(env.server.wait_for(proto::PacketId::Ping, 1000, before + 1));
    const auto echoed = env.server.last(proto::PacketId::Ping);
    ASSERT_EQ(echoed.size(), sizeof(proto::PingMessage));
    ASSERT_EQ(echoed[1], 77);
}

/**
 * @brief The server's answer to a ping the client sent (requester 1) and a too-
 *        short ping are not echoed: only the later real ping is.
 */
TEST(a_ping_the_client_asked_for_is_not_answered) {
    Env env;
    ASSERT_TRUE(open_station(env));
    const std::size_t before = env.server.count(proto::PacketId::Ping);
    proto::PingMessage ping{};
    ping.requester = 1;
    ping.id = 5;
    ASSERT_TRUE(env.server.send(proto::PacketId::Ping, ping));
    ASSERT_TRUE(env.server.send(proto::PacketId::Ping));
    // The echo of the next ping proves both earlier ones were consumed; it is
    // the only one answered.
    ASSERT_TRUE(round_trip(env));
    ASSERT_EQ(env.server.count(proto::PacketId::Ping), before + 1);
    ASSERT_EQ(env.server.last(proto::PacketId::Ping)[1], 0xB0);
    ASSERT_EQ(
        env.server.last(proto::PacketId::Ping).size(),
        sizeof(proto::PingMessage));
}

/**
 * @brief NetworkErrors (port unreachable, other codes, and an empty one) turn
 * the P2P path off but do not break the service: it still answers a Scan.
 */
TEST(a_port_unreachable_error_turns_the_p2p_path_off_without_failing_requests) {
    Env env(true);
    ASSERT_TRUE(open_station(env));
    proto::NetworkErrorMessage err{};
    err.error_code = 1;
    ASSERT_TRUE(env.server.send(proto::PacketId::NetworkError, err));
    err.error_code = 4;
    ASSERT_TRUE(env.server.send(proto::PacketId::NetworkError, err));
    ASSERT_TRUE(env.server.send(proto::PacketId::NetworkError));
    lm::NetworkInfo results[2];
    std::uint32_t count = 0;
    // The service is still alive and answers requests.
    ASSERT_TRUE(eventually(
        [&] { return run_scan(env, results, 2, make_filter(), count) == 0u; }));
}

/**
 * @brief Truncated Connected, SyncNetwork, ScanReply, ProxyConfig,
 * ExternalProxy*, ProxyData, ProxyConnectReply and SetAdvertiseData packets are
 * ignored: the state and the network info stay unchanged.
 */
TEST(a_short_connected_packet_is_ignored) {
    Env env;
    ASSERT_TRUE(open_station(env));
    const std::uint8_t tiny[4] = {};
    ASSERT_TRUE(
        env.server.send(proto::PacketId::Connected, tiny, sizeof(tiny)));
    ASSERT_TRUE(
        env.server.send(proto::PacketId::SyncNetwork, tiny, sizeof(tiny)));
    ASSERT_TRUE(
        env.server.send(proto::PacketId::ScanReply, tiny, sizeof(tiny)));
    ASSERT_TRUE(env.server.send(proto::PacketId::ProxyConfig, tiny, 2));
    ASSERT_TRUE(
        env.server.send(proto::PacketId::ExternalProxy, tiny, sizeof(tiny)));
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ExternalProxyToken, tiny, sizeof(tiny)));
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ExternalProxyState, tiny, sizeof(tiny)));
    ASSERT_TRUE(
        env.server.send(proto::PacketId::ProxyData, tiny, sizeof(tiny)));
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ProxyConnectReply, tiny, sizeof(tiny)));
    ASSERT_TRUE(
        env.server.send(proto::PacketId::SetAdvertiseData, tiny, sizeof(tiny)));
    // Every one of them was dispatched (the echo comes back behind them) and
    // none changed anything: still a station, no network joined.
    ASSERT_TRUE(round_trip(env));
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Station));
    lm::NetworkInfo info{};
    ASSERT_TRUE(
        env->GetNetworkInfo(ams::sf::Out<lm::NetworkInfo>(info)).IsSuccess());
    ASSERT_EQ(info.ldn.nodeCount, 0);
}

/**
 * @brief A ProxyConfig pushed by the server changes the virtual address
 *        GetIpv4Address reports.
 */
TEST(a_proxy_config_updates_the_virtual_address) {
    Env env;
    ASSERT_TRUE(open_station(env));
    proto::ProxyConfig config{};
    config.proxy_ip = 0x0A720005;
    config.proxy_subnet_mask = VIP_MASK;
    ASSERT_TRUE(env.server.send(proto::PacketId::ProxyConfig, config));
    ASSERT_TRUE(eventually([&] {
        std::uint32_t address = 0;
        std::uint32_t mask = 0;
        env->GetIpv4Address(
            ams::sf::Out<std::uint32_t>(address),
            ams::sf::Out<std::uint32_t>(mask));
        return address == 0x0A720005u;
    }));
}

/**
 * @brief ProxyData with no bound socket is buffered (UDP and TCP) and malformed
 *        data (declared length too long, unknown protocol) is dropped, without
 *        harming the receive thread.
 */
TEST(proxy_data_nobody_listens_for_is_buffered_and_malformed_data_is_dropped) {
    Env env;
    ASSERT_TRUE(open_station(env));
    std::vector<std::uint8_t> packet(sizeof(proto::ProxyDataHeader) + 4, 0xAB);
    proto::ProxyDataHeader header{};
    header.info.source_ipv4 = 0x0A720002;
    header.info.source_port = 5000;
    header.info.dest_ipv4 = 0x0A720001;
    header.info.dest_port = 6000;
    header.info.protocol = proto::ProtocolType::Udp;
    header.data_length = 4;
    std::memcpy(packet.data(), &header, sizeof(header));
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ProxyData, packet.data(), packet.size()));
    header.info.protocol = proto::ProtocolType::Tcp;
    std::memcpy(packet.data(), &header, sizeof(header));
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ProxyData, packet.data(), packet.size()));
    // Declared length longer than the payload.
    header.data_length = 100;
    std::memcpy(packet.data(), &header, sizeof(header));
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ProxyData, packet.data(), packet.size()));
    // Unknown transport protocol.
    header.data_length = 4;
    header.info.protocol = proto::ProtocolType::Icmp;
    std::memcpy(packet.data(), &header, sizeof(header));
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ProxyData, packet.data(), packet.size()));
    // The receive thread survived all of them and still answers.
    ASSERT_TRUE(round_trip(env));
    ASSERT_EQ(
        env.server.last(proto::PacketId::Ping).size(),
        sizeof(proto::PingMessage));
}

// =============================================================================
// P2P data plane
// =============================================================================

namespace {

/// Virtual address of a joining station (10.114.0.2).
constexpr std::uint32_t JOINER_VIP = 0x0A720002;
/// 127.0.0.1 as the 4 bytes ExternalProxyConfig/Token carry.
constexpr std::uint8_t LOOPBACK_IP[4] = {127, 0, 0, 1};

/// A P2P host as seen by a joiner: a loopback listener the test plays by hand.
class P2pHost {
public:
    /// Bind a loopback listener on an OS-assigned port.
    P2pHost() {
        m_listen = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(m_listen, 4);
        socklen_t len = sizeof(addr);
        ::getsockname(m_listen, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
    }

    /// Close the peer connection and the listener.
    ~P2pHost() {
        close_peer();
        close_listener();
    }

    /// Not copyable: owns sockets.
    P2pHost(const P2pHost&) = delete;
    /// Not assignable: owns sockets.
    /// @return Never returns (deleted).
    P2pHost& operator=(const P2pHost&) = delete;

    /**
     * @brief Port the listener is bound to.
     * @return TCP port on 127.0.0.1.
     */
    std::uint16_t port() const {
        return m_port;
    }

    /// Stop listening: later connects to port() are refused.
    void close_listener() {
        if (m_listen >= 0) {
            ::close(m_listen);
            m_listen = -1;
        }
    }

    /// Close the accepted connection with an orderly FIN.
    void close_peer() {
        if (m_peer >= 0) {
            ::close(m_peer);
            m_peer = -1;
        }
    }

    /// Abort the connection: the peer gets a RST instead of an orderly FIN.
    void reset_peer() {
        if (m_peer >= 0) {
            const linger abort_on_close{1, 0};
            ::setsockopt(
                m_peer,
                SOL_SOCKET,
                SO_LINGER,
                &abort_on_close,
                sizeof(abort_on_close));
            close_peer();
        }
    }

    /**
     * @brief Wait for the service's P2P client to connect, then accept it.
     * @param timeout_ms Longest wait, in milliseconds.
     * @return true when a peer was accepted.
     */
    bool accept_peer(int timeout_ms = 3000) {
        pollfd p{m_listen, POLLIN, 0};
        if (::poll(&p, 1, timeout_ms) <= 0) {
            return false;
        }
        m_peer = ::accept(m_listen, nullptr, nullptr);
        return m_peer >= 0;
    }

    /**
     * @brief Read one whole packet from the peer.
     * @param[out] id Type of the packet read.
     * @param[out] body Payload of the packet read.
     * @return false on EOF, error or a 3 s silence.
     */
    bool read_packet(proto::PacketId& id, std::vector<std::uint8_t>& body) {
        std::uint8_t raw[sizeof(proto::LdnHeader)];
        if (!read_full(raw, sizeof(raw))) {
            return false;
        }
        proto::LdnHeader header{};
        std::memcpy(&header, raw, sizeof(header));
        id = static_cast<proto::PacketId>(header.type);
        body.assign(static_cast<std::size_t>(header.data_size), 0);
        return body.empty() || read_full(body.data(), body.size());
    }

    /**
     * @brief Send one packet to the peer.
     * @param id Packet type to put in the LdnHeader.
     * @param payload Packet body.
     * @param size Size of payload in bytes.
     * @return true when the packet was written.
     */
    bool send(proto::PacketId id, const void* payload, std::size_t size) {
        std::vector<std::uint8_t> buf(sizeof(proto::LdnHeader) + size);
        std::size_t written = 0;
        if (proto::encode_raw(
                buf.data(),
                buf.size(),
                id,
                static_cast<const std::uint8_t*>(payload),
                size,
                written) != proto::EncodeResult::Success) {
            return false;
        }
        return m_peer >= 0 &&
               ::send(m_peer, buf.data(), written, MSG_NOSIGNAL) ==
                   static_cast<ssize_t>(written);
    }

    template <class T>
    /**
     * @brief Send a packet whose body is the object representation of
     * `payload`.
     * @tparam T Trivially copyable payload type.
     * @param id Packet type to put in the LdnHeader.
     * @param payload Body to send (sizeof(T) bytes).
     * @return true when the packet was written.
     */
    bool send(proto::PacketId id, const T& payload) {
        return send(id, &payload, sizeof(payload));
    }

private:
    /**
     * @brief Read exactly `size` bytes from the peer.
     * @param[out] out Buffer receiving the bytes.
     * @param size Number of bytes to read.
     * @return false on EOF, error or when no data came for 3 s.
     */
    bool read_full(std::uint8_t* out, std::size_t size) {
        std::size_t got = 0;
        while (got < size) {
            pollfd p{m_peer, POLLIN, 0};
            if (m_peer < 0 || ::poll(&p, 1, 3000) <= 0) {
                return false;
            }
            const ssize_t n = ::recv(m_peer, out + got, size - got, 0);
            if (n <= 0) {
                return false;
            }
            got += static_cast<std::size_t>(n);
        }
        return true;
    }

    /// Listening socket, or -1 once close_listener ran.
    int m_listen = -1;
    /// Accepted connection, or -1.
    int m_peer = -1;
    /// Port the listener is bound to.
    std::uint16_t m_port = 0;
};

/**
 * @brief Build the ExternalProxyConfig the master sends to a station.
 * @param port Port of the P2P host (127.0.0.1).
 * @param family Address family; 2 is IPv4, other values are unsupported.
 * @param token_fill Byte the authentication token is filled with.
 * @return The config.
 */
proto::ExternalProxyConfig external_proxy(
    std::uint16_t port,
    std::uint32_t family = 2,
    std::uint8_t token_fill = 0x5A) {
    proto::ExternalProxyConfig config{};
    std::memcpy(config.proxy_ip, LOOPBACK_IP, sizeof(LOOPBACK_IP));
    config.address_family = family;
    config.proxy_port = port;
    std::memset(config.token, token_fill, sizeof(config.token));
    return config;
}

/**
 * @brief Build a ProxyConfig on the virtual network.
 * @param vip Virtual address assigned to the receiver.
 * @return The config (mask VIP_MASK).
 */
proto::ProxyConfig proxy_config(std::uint32_t vip) {
    proto::ProxyConfig config{};
    config.proxy_ip = vip;
    config.proxy_subnet_mask = VIP_MASK;
    return config;
}

/**
 * @brief Read the service's virtual address through GetIpv4Address.
 * @param env Service under test.
 * @return The address in Ryujinx format.
 */
std::uint32_t address_of(Env& env) {
    std::uint32_t address = 0;
    std::uint32_t mask = 0;
    env->GetIpv4Address(
        ams::sf::Out<std::uint32_t>(address),
        ams::sf::Out<std::uint32_t>(mask));
    return address;
}

/**
 * @brief The master points the station at `host`; the host authenticates it
 *        and answers with a ProxyConfig carrying `vip`.
 * @param env Service under test (P2P enabled, station open).
 * @param host Hand-played P2P host the master names.
 * @param vip Virtual address the host assigns to the station.
 * @param token_fill Byte the authentication token is filled with; the host
 *        checks the token it receives against it.
 * @return true once the service took `vip` as its address.
 */
bool join_p2p_host(
    Env& env,
    P2pHost& host,
    std::uint32_t vip,
    std::uint8_t token_fill = 0x5A) {
    if (!env.server.send(
            proto::PacketId::ExternalProxy,
            external_proxy(host.port(), 2, token_fill)) ||
        !host.accept_peer()) {
        return false;
    }
    proto::PacketId id{};
    std::vector<std::uint8_t> body;
    if (!host.read_packet(id, body) || id != proto::PacketId::ExternalProxy ||
        body.size() != sizeof(proto::ExternalProxyConfig)) {
        return false;
    }
    if (std::memcmp(
            body.data() + offsetof(proto::ExternalProxyConfig, token),
            external_proxy(0, 2, token_fill).token,
            16) != 0) {
        return false;
    }
    return host.send(proto::PacketId::ProxyConfig, proxy_config(vip)) &&
           eventually([&] { return address_of(env) == vip; }, 5000);
}

/**
 * @brief Build a ProxyData packet body: ProxyDataHeader then payload.
 * @param src_ip Source virtual address.
 * @param src_port Source port.
 * @param dst_ip Destination virtual address.
 * @param dst_port Destination port.
 * @param protocol Transport protocol written in the header.
 * @param declared data_length written in the header, which tests may make
 *        differ from the real payload size.
 * @param body Payload bytes appended after the header.
 * @return The encoded body.
 */
std::vector<std::uint8_t> proxy_data(
    std::uint32_t src_ip,
    std::uint16_t src_port,
    std::uint32_t dst_ip,
    std::uint16_t dst_port,
    proto::ProtocolType protocol,
    std::uint32_t declared,
    const std::vector<std::uint8_t>& body) {
    proto::ProxyDataHeader header{};
    header.info.source_ipv4 = src_ip;
    header.info.source_port = src_port;
    header.info.dest_ipv4 = dst_ip;
    header.info.dest_port = dst_port;
    header.info.protocol = protocol;
    header.data_length = declared;
    std::vector<std::uint8_t> out(sizeof(header) + body.size());
    std::memcpy(out.data(), &header, sizeof(header));
    if (!body.empty()) {
        std::memcpy(out.data() + sizeof(header), body.data(), body.size());
    }
    return out;
}

/**
 * @brief A proxy socket of the BSD MITM bound to `ip:port` (what a game's UDP
 *        socket is).
 * @param fd Descriptor number the proxy socket is registered under.
 * @param ip Virtual IPv4 address to bind, in Ryujinx format.
 * @param port Port to bind, in host byte order.
 * @return The bound socket, or nullptr when creation or bind failed.
 */
ams::mitm::bsd::ProxySocket* bound_udp_socket(
    int fd, std::uint32_t ip, std::uint16_t port) {
    auto& manager = ams::mitm::bsd::ProxySocketManager::GetInstance();
    auto* sock = manager.CreateProxySocket(
        fd, ryu_ldn::bsd::SocketType::Dgram, ryu_ldn::bsd::ProtocolType::Udp);
    if (sock == nullptr) {
        return nullptr;
    }
    ryu_ldn::bsd::SockAddrIn addr{};
    addr.sin_len = sizeof(addr);
    addr.sin_family = 2;
    addr.sin_port = __builtin_bswap16(port);
    addr.sin_addr = ip;
    return sock->Bind(addr).IsSuccess() ? sock : nullptr;
}

/**
 * @brief Send proxy data the way a game's socket does, through the BSD
 *        proxy socket manager.
 * @param src_ip Source virtual address.
 * @param src_port Source port.
 * @param dst_ip Destination virtual address.
 * @param dst_port Destination port.
 * @param body Payload to send.
 * @param protocol Transport protocol of the payload.
 * @return true when the manager accepted and sent the payload.
 */
bool send_through_the_bsd_service(
    std::uint32_t src_ip,
    std::uint16_t src_port,
    std::uint32_t dst_ip,
    std::uint16_t dst_port,
    const std::vector<std::uint8_t>& body,
    ryu_ldn::bsd::ProtocolType protocol = ryu_ldn::bsd::ProtocolType::Udp) {
    return ams::mitm::bsd::ProxySocketManager::GetInstance().SendProxyData(
        src_ip, src_port, dst_ip, dst_port, protocol, body.data(), body.size());
}

}  // namespace

/**
 * @brief On an ExternalProxy from the master the station dials the P2P host,
 *        authenticates with the token, and takes the virtual address from the
 *        host's ProxyConfig (also in the proxy socket manager).
 */
TEST(a_station_follows_the_master_named_proxy_and_takes_the_host_address) {
    Env env(true);
    ASSERT_TRUE(open_station(env));
    P2pHost host;
    ASSERT_TRUE(join_p2p_host(env, host, 0x0A720009));
    std::uint32_t address = 0;
    std::uint32_t mask = 0;
    ASSERT_TRUE(env->GetIpv4Address(
                       ams::sf::Out<std::uint32_t>(address),
                       ams::sf::Out<std::uint32_t>(mask))
                    .IsSuccess());
    ASSERT_EQ(address, 0x0A720009u);
    ASSERT_EQ(mask, VIP_MASK);
    ASSERT_EQ(
        ams::mitm::bsd::ProxySocketManager::GetInstance().GetLocalIp(),
        0x0A720009u);
}

/**
 * @brief ProxyData from the P2P host is delivered to the bound socket; unknown
 *        protocol, over-long declared length, a too-short packet and TCP
 * towards a datagram socket are dropped.
 */
TEST(p2p_host_data_reaches_the_bound_socket_and_bad_data_is_dropped) {
    Env env(true);
    ASSERT_TRUE(open_station(env));
    P2pHost host;
    ASSERT_TRUE(join_p2p_host(env, host, 0x0A720009));
    auto* sock = bound_udp_socket(900, 0x0A720009, 7000);
    ASSERT_TRUE(sock != nullptr);
    const std::vector<std::uint8_t> body = {1, 2, 3, 4};
    // Dropped: unknown transport, declared length beyond the payload, shorter
    // than a ProxyDataHeader. Tcp towards a datagram socket is not delivered.
    ASSERT_TRUE(host.send(
        proto::PacketId::ProxyData,
        proxy_data(
            0x0A720001,
            5000,
            0x0A720009,
            7000,
            proto::ProtocolType::Icmp,
            4,
            body)
            .data(),
        sizeof(proto::ProxyDataHeader) + 4));
    ASSERT_TRUE(host.send(
        proto::PacketId::ProxyData,
        proxy_data(
            0x0A720001,
            5000,
            0x0A720009,
            7000,
            proto::ProtocolType::Udp,
            100,
            body)
            .data(),
        sizeof(proto::ProxyDataHeader) + 4));
    ASSERT_TRUE(
        host.send(proto::PacketId::ProxyData, body.data(), body.size()));
    ASSERT_TRUE(host.send(
        proto::PacketId::ProxyData,
        proxy_data(
            0x0A720001,
            5000,
            0x0A720009,
            7000,
            proto::ProtocolType::Tcp,
            4,
            body)
            .data(),
        sizeof(proto::ProxyDataHeader) + 4));
    const auto good = proxy_data(
        0x0A720001, 5000, 0x0A720009, 7000, proto::ProtocolType::Udp, 4, body);
    ASSERT_TRUE(
        host.send(proto::PacketId::ProxyData, good.data(), good.size()));
    ASSERT_TRUE(eventually([&] { return sock->HasPendingData(); }));
    // Only the well-formed datagram arrived (packets of one connection are
    // ordered).
    ASSERT_EQ(sock->GetPendingDataSize(), body.size());
    ams::mitm::bsd::ProxySocketManager::GetInstance().CloseProxySocket(900);
}

/**
 * @brief While a P2P host is connected, a game's proxy data goes to the host
 * with the right header and is not sent to the master.
 */
TEST(proxy_data_sent_by_a_game_goes_to_the_p2p_host_not_to_the_master) {
    Env env(true);
    ASSERT_TRUE(open_station(env));
    P2pHost host;
    ASSERT_TRUE(join_p2p_host(env, host, 0x0A720009));
    const std::size_t via_master = env.server.count(proto::PacketId::ProxyData);
    ASSERT_TRUE(send_through_the_bsd_service(
        0x0A720009, 7000, 0x0A720001, 7001, {9, 8, 7}));
    proto::PacketId id{};
    std::vector<std::uint8_t> body;
    ASSERT_TRUE(host.read_packet(id, body));
    ASSERT_TRUE(id == proto::PacketId::ProxyData);
    ASSERT_EQ(body.size(), sizeof(proto::ProxyDataHeader) + 3);
    proto::ProxyDataHeader header{};
    std::memcpy(&header, body.data(), sizeof(header));
    ASSERT_EQ(header.info.dest_ipv4, 0x0A720001u);
    ASSERT_EQ(header.info.dest_port, static_cast<std::uint16_t>(7001));
    ASSERT_EQ(header.data_length, 3u);
    ASSERT_EQ(body[sizeof(header)], 9);
    ASSERT_EQ(env.server.count(proto::PacketId::ProxyData), via_master);
}

/**
 * @brief When the P2P host connection is lost, proxy data falls back to the
 * master.
 */
TEST(when_the_p2p_host_goes_away_proxy_data_falls_back_to_the_master) {
    Env env(true);
    ASSERT_TRUE(open_station(env));
    P2pHost host;
    ASSERT_TRUE(join_p2p_host(env, host, 0x0A720009));
    host.close_peer();
    ASSERT_TRUE(eventually(
        [&] {
            return send_through_the_bsd_service(
                       0x0A720009, 7000, 0x0A720001, 7001, {1}) &&
                   env.server.count(proto::PacketId::ProxyData) > 0;
        },
        5000));
}

/**
 * @brief The BSD send path refuses an unsupported protocol, sends TCP/UDP
 * through the master while connected, and refuses once the service is finalized
 * or gone.
 */
TEST(a_game_cannot_send_unsupported_protocols_or_without_a_connected_service) {
    {
        Env env;
        ASSERT_TRUE(open_station(env));
        ASSERT_FALSE(send_through_the_bsd_service(
            0x0A720002,
            1,
            0x0A720001,
            2,
            {1},
            ryu_ldn::bsd::ProtocolType::Unspecified));
        ASSERT_TRUE(
            send_through_the_bsd_service(0x0A720002, 1, 0x0A720001, 2, {1}));
        ASSERT_TRUE(env.server.wait_for(proto::PacketId::ProxyData, 2000));
        ASSERT_TRUE(env->Finalize().IsSuccess());
        ASSERT_FALSE(
            send_through_the_bsd_service(0x0A720002, 1, 0x0A720001, 2, {1}));
    }
    ASSERT_FALSE(
        send_through_the_bsd_service(0x0A720002, 1, 0x0A720001, 2, {1}));
}

/**
 * @brief An ExternalProxy with an unsupported address family, or whose port
 * refuses the connection, is abandoned without changing the address, and the
 * connect worker stays reusable.
 */
TEST(external_proxy_with_bad_family_or_refused_port_is_abandoned) {
    Env env(true);
    ASSERT_TRUE(open_station(env));
    const std::uint32_t before = address_of(env);
    {
        P2pHost host;
        ASSERT_TRUE(env.server.send(
            proto::PacketId::ExternalProxy, external_proxy(host.port(), 23)));
        ASSERT_FALSE(host.accept_peer(400));
    }
    P2pHost gone;
    const std::uint16_t refused = gone.port();
    gone.close_listener();
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ExternalProxy, external_proxy(refused)));
    ASSERT_TRUE(round_trip(env));
    ASSERT_EQ(address_of(env), before);
    // The failed attempts left the connect worker reusable.
    P2pHost host;
    ASSERT_TRUE(
        eventually([&] { return join_p2p_host(env, host, 0x0A720009); }, 8000));
}

/**
 * @brief While an attempt waits on a host that never answers the auth, a second
 *        ExternalProxy is dropped and the address is unchanged.
 */
TEST(silent_p2p_host_times_out_the_attempt_and_a_second_one_is_dropped) {
    Env env(true);
    ASSERT_TRUE(open_station(env));
    const std::uint32_t before = address_of(env);
    P2pHost silent;
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ExternalProxy, external_proxy(silent.port())));
    ASSERT_TRUE(silent.accept_peer());
    // The first attempt is still waiting for the ProxyConfig: this one is
    // dropped.
    P2pHost other;
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ExternalProxy, external_proxy(other.port())));
    ASSERT_FALSE(other.accept_peer(400));
    ASSERT_EQ(address_of(env), before);
}

/**
 * @brief With P2P disabled an ExternalProxy is ignored: the service never dials
 * the named host.
 */
TEST(an_external_proxy_is_ignored_when_p2p_is_disabled) {
    Env env(false);
    ASSERT_TRUE(open_station(env));
    P2pHost host;
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ExternalProxy, external_proxy(host.port())));
    ASSERT_FALSE(host.accept_peer(400));
    ASSERT_TRUE(round_trip(env));
}

/**
 * @brief ExternalProxyToken and ExternalProxyState packets are ignored when the
 *        service hosts no P2P server.
 */
TEST(a_p2p_token_without_a_hosted_server_is_ignored) {
    Env env(true);
    ASSERT_TRUE(open_station(env));
    proto::ExternalProxyToken token{};
    token.virtual_ip = JOINER_VIP;
    token.address_family = 2;
    ASSERT_TRUE(env.server.send(proto::PacketId::ExternalProxyToken, token));
    proto::ExternalProxyConnectionState state{};
    state.ip_address = JOINER_VIP;
    ASSERT_TRUE(env.server.send(proto::PacketId::ExternalProxyState, state));
    ASSERT_TRUE(round_trip(env));
}

/**
 * @brief The port the hosted P2P server listens on, as announced to the
 *        master.
 * @param env Service under test, after CreateNetwork.
 * @return internal_proxy_port of the last CreateAccessPoint the server
 *         received; 0 when none was received or no P2P server runs.
 */
std::uint16_t announced_p2p_port(Env& env) {
    const auto body = env.server.last(proto::PacketId::CreateAccessPoint);
    if (body.size() < sizeof(proto::CreateAccessPointRequest)) {
        return 0;
    }
    proto::CreateAccessPointRequest request{};
    std::memcpy(&request, body.data(), sizeof(request));
    return request.ryu_network_config.internal_proxy_port;
}

/**
 * @brief A P2P host announces its server port to the master and serves a
 * joiner: token authentication, joiner data delivered to the host's sockets
 * (UDP and TCP, unsupported transport dropped), the host's sends going over P2P
 *        instead of the master, and ExternalProxy and ExternalProxyState
 * handling.
 */
TEST(hosting_with_p2p_announces_the_server_and_serves_joiners_through_it) {
    Env env(true);
    ASSERT_TRUE(host_a_network(env));
    const auto body = env.server.last(proto::PacketId::CreateAccessPoint);
    ASSERT_TRUE(body.size() >= sizeof(proto::CreateAccessPointRequest));
    proto::CreateAccessPointRequest request{};
    std::memcpy(&request, body.data(), sizeof(request));
    ASSERT_EQ(request.ryu_network_config.address_family, 2u);
    const std::uint16_t port = announced_p2p_port(env);
    ASSERT_TRUE(port != 0);

    // The master announces a joiner's token to the host.
    proto::ExternalProxyToken token{};
    token.virtual_ip = JOINER_VIP;
    token.address_family = 2;
    std::memset(token.token, 0xC3, sizeof(token.token));
    std::memcpy(token.physical_ip, LOOPBACK_IP, sizeof(LOOPBACK_IP));
    ASSERT_TRUE(env.server.send(proto::PacketId::ExternalProxyToken, token));
    ASSERT_TRUE(round_trip(env));

    // The joiner dials the hosted server and authenticates with that token.
    ams::mitm::p2p::P2pProxyClient joiner;
    ASSERT_TRUE(joiner.Connect("127.0.0.1", port));
    ASSERT_TRUE(joiner.PerformAuth(external_proxy(port, 2, 0xC3)));
    ASSERT_TRUE(joiner.EnsureProxyReady());

    // Joiner -> host: delivered to the host's own BSD sockets.
    auto* sock = bound_udp_socket(901, HOST_VIP, 7100);
    ASSERT_TRUE(sock != nullptr);
    const std::vector<std::uint8_t> body4 = {4, 3, 2, 1};
    proto::ProxyDataHeader header{};
    header.info.source_ipv4 = JOINER_VIP;
    header.info.source_port = 7101;
    header.info.dest_ipv4 = HOST_VIP;
    header.info.dest_port = 7100;
    header.info.protocol = proto::ProtocolType::Udp;
    header.data_length = 4;
    ASSERT_TRUE(joiner.SendProxyData(header, body4.data(), body4.size()));
    ASSERT_TRUE(eventually([&] { return sock->HasPendingData(); }));
    ASSERT_EQ(sock->GetPendingDataSize(), body4.size());

    // A TCP payload reaching the host is delivered like a UDP one.
    header.info.protocol = proto::ProtocolType::Tcp;
    ASSERT_TRUE(joiner.SendProxyData(header, body4.data(), body4.size()));

    // An unsupported transport reaching the host is dropped.
    header.info.protocol = proto::ProtocolType::Icmp;
    ASSERT_TRUE(joiner.SendProxyData(header, body4.data(), body4.size()));

    // Host -> joiner: a game's send is broadcast over P2P, not to the master.
    const std::size_t via_master = env.server.count(proto::PacketId::ProxyData);
    ASSERT_TRUE(
        send_through_the_bsd_service(HOST_VIP, 7100, JOINER_VIP, 7101, {5, 6}));
    ASSERT_EQ(env.server.count(proto::PacketId::ProxyData), via_master);

    // The ExternalProxy the master sends the host itself targets its own
    // server.
    ASSERT_TRUE(
        env.server.send(proto::PacketId::ExternalProxy, external_proxy(port)));
    ASSERT_TRUE(round_trip(env));

    // The master reports the joiner gone.
    proto::ExternalProxyConnectionState state{};
    state.ip_address = JOINER_VIP;
    state.connected = 0;
    ASSERT_TRUE(env.server.send(proto::PacketId::ExternalProxyState, state));
    ASSERT_TRUE(round_trip(env));

    joiner.Disconnect();
    ams::mitm::bsd::ProxySocketManager::GetInstance().CloseProxySocket(901);
    ASSERT_TRUE(env->Finalize().IsSuccess());
}

/**
 * @brief With a hosted P2P server and nobody joined, proxy data is not sent to
 * the master.
 */
TEST(a_hosted_p2p_server_with_nobody_joined_swallows_proxy_data) {
    Env env(true);
    ASSERT_TRUE(host_a_network(env));
    const std::size_t via_master = env.server.count(proto::PacketId::ProxyData);
    ASSERT_TRUE(
        send_through_the_bsd_service(HOST_VIP, 7100, 0x0A72FFFF, 7100, {1}));
    ASSERT_EQ(env.server.count(proto::PacketId::ProxyData), via_master);
}

/**
 * @brief A game's TCP payload is sent to the master in a ProxyData marked as
 * TCP.
 */
TEST(a_game_tcp_payload_is_sent_to_the_master_as_tcp) {
    Env env;
    ASSERT_TRUE(open_station(env));
    ASSERT_TRUE(send_through_the_bsd_service(
        0x0A720002, 1, 0x0A720001, 2, {7}, ryu_ldn::bsd::ProtocolType::Tcp));
    ASSERT_TRUE(env.server.wait_for(proto::PacketId::ProxyData, 2000));
    proto::ProxyDataHeader header{};
    const auto body = env.server.last(proto::PacketId::ProxyData);
    ASSERT_TRUE(body.size() >= sizeof(header));
    std::memcpy(&header, body.data(), sizeof(header));
    ASSERT_TRUE(header.info.protocol == proto::ProtocolType::Tcp);
}

/**
 * @brief When the write to the master fails, the BSD send path reports the
 * payload as not sent.
 */
TEST(unwritable_game_payload_is_reported_as_not_sent) {
    Env env;
    ASSERT_TRUE(open_station(env));
    ASSERT_TRUE(cut_the_client_write_side(env));
    ASSERT_FALSE(
        send_through_the_bsd_service(0x0A720002, 1, 0x0A720001, 2, {1}));
}

/**
 * @brief A service finalized between the manager's lock and the service lookup
 *        makes the send fail instead of using a dead service.
 */
TEST(a_service_finalized_while_a_game_send_is_in_flight_refuses_the_send) {
    Env env;
    ASSERT_TRUE(open_station(env));
    // The manager's own lock is the first recursive lock, the service
    // registry's the second: the service goes away between the two.
    ScopedHook hook(g_recursive_lock_hook, [&, taken = 0]() mutable {
        if (++taken < 2) {
            return false;
        }
        return env->Finalize().IsSuccess();
    });
    ASSERT_FALSE(
        send_through_the_bsd_service(0x0A720002, 1, 0x0A720001, 2, {1}));
}

/**
 * @brief A server connection that died while the station was closed is replaced
 * by a new one on OpenStation, so a Scan works again.
 */
TEST(
    a_connection_that_died_while_the_station_was_closed_is_replaced_on_reopen) {
    Env env;
    ASSERT_TRUE(open_station(env));
    ASSERT_TRUE(env->CloseStation().IsSuccess());
    env.server.answer_handshake(false);
    env.server.drop();
    lm::NetworkInfo results[2];
    std::uint32_t count = 0;
    // Scanning needs the Station state, which CloseStation left: only the
    // reopen below can notice the dead link. Give the receive thread time to
    // see the EOF first.
    ASSERT_TRUE(eventually([&] { return !env.server.has_client(); }));
    env.server.answer_handshake(true);
    ASSERT_TRUE(env->OpenStation().IsSuccess());
    ASSERT_TRUE(eventually(
        [&] { return run_scan(env, results, 2, make_filter(), count) == 0u; }));
}

// =============================================================================
// Send failures: the connection looks up, the write side is gone
// =============================================================================

/**
 * @brief With the write side cut, Scan returns the send-failed result and zero
 *        networks.
 */
TEST(scan_reports_a_failed_send) {
    Env env;
    ASSERT_TRUE(open_station(env));
    ASSERT_TRUE(cut_the_client_write_side(env));
    lm::NetworkInfo results[2];
    std::uint32_t count = 0;
    ASSERT_EQ(run_scan(env, results, 2, make_filter(), count), RC_SEND_FAILED);
    ASSERT_EQ(count, 0u);
}

/**
 * @brief With the write side cut, CreateNetwork returns the send-failed result
 * and the state goes back to AccessPoint.
 */
TEST(creating_a_network_reports_a_failed_send_and_rolls_back) {
    Env env(true);
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    ASSERT_TRUE(cut_the_client_write_side(env));
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    ASSERT_EQ(env->CreateNetwork(config).GetValue(), RC_SEND_FAILED);
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::AccessPoint));
}

/**
 * @brief With the write side cut, SetAdvertiseData and SetStationAcceptPolicy
 *        return the send-failed result while hosting.
 */
TEST(advertise_data_and_accept_policy_report_a_failed_send_while_hosting) {
    Env env;
    ASSERT_TRUE(host_a_network(env));
    ASSERT_TRUE(cut_the_client_write_side(env));
    const std::uint8_t data[2] = {1, 2};
    ASSERT_EQ(
        env->SetAdvertiseData(ams::sf::InAutoSelectBuffer(data, sizeof(data)))
            .GetValue(),
        RC_SEND_FAILED);
    ASSERT_EQ(env->SetStationAcceptPolicy(1).GetValue(), RC_SEND_FAILED);
}

/**
 * @brief With the write side cut, Connect fails (timeout result) and the
 * service stays in Station.
 */
TEST(connecting_reports_a_failed_send_and_stays_a_station) {
    Env env;
    ASSERT_TRUE(open_station(env));
    ASSERT_TRUE(cut_the_client_write_side(env));
    const lm::NetworkInfo target = make_info(1);
    ASSERT_EQ(env->Connect(make_connect(), target).GetValue(), RC_TIMEOUT);
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Station));
}

/**
 * @brief With the write side cut, ConnectPrivate returns the send-failed result
 * and the service stays in Station.
 */
TEST(a_private_connect_reports_a_failed_send_and_stays_a_station) {
    Env env;
    ASSERT_TRUE(open_station(env));
    ASSERT_TRUE(cut_the_client_write_side(env));
    lm::ConnectPrivateData data{};
    std::memset(&data, 0, sizeof(data));
    ASSERT_EQ(env->ConnectPrivate(data).GetValue(), RC_SEND_FAILED);
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Station));
}

/**
 * @brief With the write side cut, CreateNetworkPrivate returns the send-failed
 *        result and the state goes back to AccessPoint.
 */
TEST(a_private_network_reports_a_failed_send_and_rolls_back) {
    Env env;
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    ASSERT_TRUE(cut_the_client_write_side(env));
    lm::CreateNetworkPrivateConfig config{};
    std::memset(&config, 0, sizeof(config));
    std::uint8_t addresses[sizeof(proto::AddressList)] = {};
    ASSERT_EQ(
        env->CreateNetworkPrivate(
               config, ams::sf::InPointerBuffer(addresses, sizeof(addresses)))
            .GetValue(),
        RC_SEND_FAILED);
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::AccessPoint));
}

/**
 * @brief With the write side cut, Reject returns the send-failed result.
 */
TEST(rejecting_reports_a_failed_send) {
    Env env;
    ASSERT_TRUE(host_a_network(env));
    ASSERT_TRUE(cut_the_client_write_side(env));
    ASSERT_EQ(env->Reject(1).GetValue(), RC_SEND_FAILED);
}

/**
 * @brief Disconnect returns to Station even when the server notification cannot
 * be written.
 */
TEST(disconnect_leaves_the_network_even_if_the_notice_cannot_be_sent) {
    Env env;
    env.server.set_handler(join_script);
    ASSERT_TRUE(open_station(env));
    const lm::NetworkInfo target = make_info(1);
    ASSERT_TRUE(env->Connect(make_connect(), target).IsSuccess());
    ASSERT_TRUE(cut_the_client_write_side(env));
    ASSERT_TRUE(env->Disconnect().IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Station));
}

// =============================================================================
// Handshake and WaitForResponse windows (scripted with the stand-in hooks)
// =============================================================================

/**
 * @brief Pings the server sends during the handshake reach the service but are
 * not echoed (the client is not Ready yet); a later ping is echoed.
 */
TEST(pings_before_the_handshake_ends_are_not_echoed) {
    Env env;
    env.server.answer_handshake(false);
    env.server.set_handler([](FakeServer& server,
                              proto::PacketId id,
                              const std::vector<std::uint8_t>&) {
        if (id != proto::PacketId::Initialize) {
            return;
        }
        proto::PingMessage ping{};
        ping.requester = 0;
        ping.id = 0x31;
        server.send(proto::PacketId::Ping, ping);  // asks for an echo
        ping.requester = 1;
        ping.id = 0x32;
        server.send(proto::PacketId::Ping, ping);  // answer to a ping of ours
        server.send(proto::PacketId::Ping);        // too short to be a ping
        proto::InitializeMessage reply{};
        server.send(proto::PacketId::Initialize, reply);
    });
    ASSERT_TRUE(open_station(env));
    // The client is not Ready yet when the echo is requested, so it cannot
    // be sent (RyuLdnClient::send_ping_response needs the Ready state).
    ASSERT_EQ(env.server.count(proto::PacketId::Ping), 0u);
    ASSERT_TRUE(round_trip(env));
    ASSERT_EQ(env.server.count(proto::PacketId::Ping), 1u);
    ASSERT_EQ(env.server.last(proto::PacketId::Ping)[1], 0xB0);
}

/**
 * @brief After the server dropped the connection, CloseStation then OpenStation
 *        reconnects and a Scan works again.
 */
TEST(a_station_reopened_after_a_server_drop_reconnects) {
    Env env;
    ASSERT_TRUE(open_station(env));
    env.server.drop();
    lm::NetworkInfo results[2];
    std::uint32_t count = 0;
    ASSERT_TRUE(eventually(
        [&] { return run_scan(env, results, 2, make_filter(), count) != 0u; }));
    ASSERT_TRUE(env->CloseStation().IsSuccess());
    ASSERT_TRUE(env->OpenStation().IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Station));
    ASSERT_TRUE(eventually(
        [&] { return run_scan(env, results, 2, make_filter(), count) == 0u; }));
}

/**
 * @brief The handshake is accepted when the server answers before the service
 *        clears its handshake event (lost wake-up): the client is Ready anyway.
 */
TEST(a_handshake_that_finishes_before_the_wait_starts_is_still_accepted) {
    Env env;
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    // The server's answer lands before the service clears its handshake event:
    // the wake-up is lost and the wait times out, yet the client is Ready.
    ScopedHook hook(
        g_clear_hook, [&] { return let_the_handshake_play_out(env); });
    ASSERT_TRUE(env->OpenStation().IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Station));
}

/**
 * @brief A connection lost right after the handshake answer makes OpenStation
 * fail on the handshake.
 */
TEST(a_connection_lost_right_after_the_handshake_fails_the_open) {
    Env env;
    env.server.set_handler([](FakeServer& server,
                              proto::PacketId id,
                              const std::vector<std::uint8_t>&) {
        if (id == proto::PacketId::Initialize) {
            // The Initialize answer is already sent: cut the link and make
            // sure the client's automatic reconnection never gets answered.
            server.answer_handshake(false);
            server.drop();
        }
    });
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    // The handshake event is set, then the EOF takes the client out of Ready.
    ScopedHook hook(
        g_timed_wait_hook, [&] { return let_the_handshake_play_out(env); });
    ASSERT_EQ(env->OpenStation().GetValue(), RC_HANDSHAKE_OR_REJECT);
}

/**
 * @brief Finalize during the handshake wait ends it (error event) and
 * OpenStation fails on the handshake.
 */
TEST(finalizing_during_the_handshake_wait_fails_the_open) {
    Env env;
    env.server.answer_handshake(false);
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    // Finalize signals the error event the handshake wait also listens on.
    ScopedHook hook(
        g_timed_wait_hook, [&] { return env->Finalize().IsSuccess(); });
    ASSERT_EQ(env->OpenStation().GetValue(), RC_HANDSHAKE_OR_REJECT);
}

/**
 * @brief A NetworkError answering a CreateAccessPoint makes CreateNetwork fail
 * and roll back to AccessPoint.
 */
TEST(a_network_error_answers_a_create_request_as_a_failure) {
    Env env;
    env.server.set_handler([](FakeServer& server,
                              proto::PacketId id,
                              const std::vector<std::uint8_t>&) {
        if (id == proto::PacketId::CreateAccessPoint) {
            proto::NetworkErrorMessage err{};
            err.error_code = 4;
            server.send(proto::PacketId::NetworkError, err);
        }
    });
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    ASSERT_EQ(env->CreateNetwork(config).GetValue(), RC_TIMEOUT);
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::AccessPoint));
}

/**
 * @brief A ScanReplyEnd arriving before the Connected does not end the wait for
 * the create answer.
 */
TEST(scan_results_interleaved_with_the_create_answer_do_not_end_the_wait) {
    Env env;
    env.server.set_handler(
        delayed_host_script(30, {proto::PacketId::ScanReplyEnd}, 120));
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    ASSERT_TRUE(env->CreateNetwork(config).IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::AccessPointCreated));
}

/**
 * @brief An answer that is not the awaited one does not extend the wait: once
 * the deadline has passed (clock skewed past it) the wait times out even though
 *        Connected follows.
 */
TEST(unrelated_answer_cannot_stretch_the_wait_past_its_deadline) {
    Env env;
    env.server.set_handler(
        delayed_host_script(30, {proto::PacketId::ScanReplyEnd}, 0));
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    // The wait is given 5 s; jump the clock past that before the unrelated
    // answer arrives, so the next lap finds the deadline already gone even
    // though the real Connected comes right behind it.
    ScopedHook hook(g_timed_wait_hook, [] {
        ams::os::g_host_tick_skew_ns += 6'000'000'000ULL;
        return true;
    });
    const auto result = env->CreateNetwork(config).GetValue();
    ams::os::g_host_tick_skew_ns = 0;
    ASSERT_EQ(result, RC_TIMEOUT);
}

/**
 * @brief Finalize during a pending create wakes the wait at once (error event)
 *        instead of letting the timeout run out.
 */
TEST(finalizing_while_a_create_is_pending_ends_the_wait_as_a_lost_connection) {
    Env env;
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    std::chrono::steady_clock::time_point finalized;
    ScopedHook hook(g_timed_wait_hook, [&] {
        const bool ok = env->Finalize().IsSuccess();
        finalized = std::chrono::steady_clock::now();
        return ok;
    });
    ASSERT_EQ(env->CreateNetwork(config).GetValue(), RC_TIMEOUT);
    // The error event Finalize raised woke the wait at once instead of
    // letting the whole timeout run out.
    ASSERT_TRUE(
        std::chrono::steady_clock::now() - finalized <
        std::chrono::milliseconds(250));
}

// =============================================================================
// Query and state edge cases
// =============================================================================

/**
 * @brief In error-state mode a pending disconnect reason is reported by
 * GetState as an error carrying that reason.
 */
TEST(a_pending_disconnect_reason_is_reported_through_get_state_in_error_state) {
    Env env;
    ASSERT_TRUE(env->InitializeSystem2(1, process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenStation().IsSuccess());
    proto::RejectRequest request{};
    request.disconnect_reason =
        static_cast<std::uint32_t>(lm::DisconnectReason::DestroyedBySystem);
    ASSERT_TRUE(env.server.send(proto::PacketId::Reject, request));
    ASSERT_TRUE(eventually([&] {
        std::uint32_t state = 0;
        return env->GetState(ams::sf::Out<std::uint32_t>(state)).GetValue() ==
               MAKERESULT(
                   0x10,
                   static_cast<int>(lm::DisconnectReason::DestroyedBySystem));
    }));
}

/**
 * @brief CreateNetworkPrivate in the Station state returns the bad-state result
 * and keeps the state.
 */
TEST(a_private_network_cannot_be_created_from_the_station_state) {
    Env env;
    ASSERT_TRUE(open_station(env));
    lm::CreateNetworkPrivateConfig config{};
    std::memset(&config, 0, sizeof(config));
    std::uint8_t addresses[sizeof(proto::AddressList)] = {};
    ASSERT_EQ(
        env->CreateNetworkPrivate(
               config, ams::sf::InPointerBuffer(addresses, sizeof(addresses)))
            .GetValue(),
        RC_BAD_STATE);
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Station));
}

/**
 * @brief A P2P host whose server sent no ProxyConfig still ends with the
 * predicted host address and mask.
 */
TEST(a_p2p_host_without_a_proxy_config_predicts_its_virtual_address) {
    Env env(true);
    env.server.set_handler([](FakeServer& server,
                              proto::PacketId id,
                              const std::vector<std::uint8_t>&) {
        if (id == proto::PacketId::CreateAccessPoint) {
            const lm::NetworkInfo info = make_info(1);
            server.send(proto::PacketId::Connected, &info, sizeof(info));
        }
    });
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    ASSERT_TRUE(env->CreateNetwork(config).IsSuccess());
    std::uint32_t address = 0;
    std::uint32_t mask = 0;
    ASSERT_TRUE(env->GetIpv4Address(
                       ams::sf::Out<std::uint32_t>(address),
                       ams::sf::Out<std::uint32_t>(mask))
                    .IsSuccess());
    ASSERT_EQ(address, HOST_VIP);
    ASSERT_EQ(mask, VIP_MASK);
}

/**
 * @brief The scene id the game asked for at CreateNetwork is kept after
 * Connected and after a SyncNetwork carrying another one.
 */
TEST(
    the_scene_id_the_game_asked_for_is_restored_on_connected_and_sync_network) {
    Env env;
    env.server.set_handler(host_script);
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    config.networkConfig.intentId.sceneId = 7;
    ASSERT_TRUE(env->CreateNetwork(config).IsSuccess());
    lm::NetworkInfo info{};
    ASSERT_TRUE(
        env->GetNetworkInfo(ams::sf::Out<lm::NetworkInfo>(info)).IsSuccess());
    ASSERT_EQ(info.networkId.intentId.sceneId, 7);

    lm::NetworkInfo next = make_info(2);
    next.networkId.intentId.sceneId = 3;
    ASSERT_TRUE(
        env.server.send(proto::PacketId::SyncNetwork, &next, sizeof(next)));
    ASSERT_TRUE(eventually([&] {
        env->GetNetworkInfo(ams::sf::Out<lm::NetworkInfo>(info));
        return info.ldn.nodeCount == 2;
    }));
    ASSERT_EQ(info.networkId.intentId.sceneId, 7);
}

// =============================================================================
// Waiting for answers: the windows between "request sent" and "wait started"
// =============================================================================

/**
 * @brief A NetworkError answering a Reject ends the wait at once (error event)
 * with the not-connected result, without running the whole timeout.
 */
TEST(network_error_to_a_reject_ends_the_wait_without_an_ack) {
    Env env;
    env.server.set_handler([](FakeServer& server,
                              proto::PacketId id,
                              const std::vector<std::uint8_t>& body) {
        if (id == proto::PacketId::CreateAccessPoint) {
            host_script(server, id, body);
        } else if (id == proto::PacketId::Reject) {
            proto::NetworkErrorMessage err{};
            err.error_code = 2;
            server.send(proto::PacketId::NetworkError, err);
        }
    });
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    ASSERT_TRUE(env->CreateNetwork(config).IsSuccess());
    const auto begin = std::chrono::steady_clock::now();
    ASSERT_EQ(env->Reject(1).GetValue(), RC_NOT_CONNECTED);
    // The error event ended the wait; the 6 s (600 ms here) timeout did not
    // run.
    ASSERT_TRUE(
        std::chrono::steady_clock::now() - begin <
        std::chrono::milliseconds(300));
}

/**
 * @brief A Connected processed before Connect is accepted as its answer.
 */
TEST(a_connected_that_was_already_processed_satisfies_the_next_connect) {
    Env env;
    ASSERT_TRUE(open_station(env));
    const lm::NetworkInfo joined = make_info(2);
    ASSERT_TRUE(
        env.server.send(proto::PacketId::Connected, &joined, sizeof(joined)));
    ASSERT_TRUE(round_trip(env));
    const lm::NetworkInfo target = make_info(1);
    ASSERT_TRUE(env->Connect(make_connect(), target).IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::StationConnected));
}

/**
 * @brief A Connected followed by a NetworkError before the wait returns still
 *        counts as joined (the final re-check under the mutex accepts it).
 */
TEST(connected_then_error_before_the_wait_returns_still_counts_as_joined) {
    Env env;
    ASSERT_TRUE(open_station(env));
    // Armed after the open: the handshake wait also goes through TimedWaitAny.
    ScopedHook hook(g_timed_wait_hook, [&] {
        const lm::NetworkInfo joined = make_info(2);
        proto::NetworkErrorMessage err{};
        err.error_code = 2;
        return env.server.send(
                   proto::PacketId::Connected, &joined, sizeof(joined)) &&
               env.server.send(proto::PacketId::NetworkError, err) &&
               round_trip(env);
    });
    const lm::NetworkInfo target = make_info(1);
    // The wake-up carries the NetworkError (the last response), yet Connected
    // was processed first: the final re-check under the mutex accepts it.
    ASSERT_TRUE(env->Connect(make_connect(), target).IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::StationConnected));
}

/**
 * @brief When a port-unreachable error turns P2P off while a network is being
 *        created, the create still succeeds through the relay.
 */
TEST(p2p_host_whose_proxy_is_refused_while_creating_uses_the_relay) {
    Env env(true);
    ASSERT_TRUE(env->Initialize(process(1)).IsSuccess());
    ASSERT_TRUE(env->OpenAccessPoint().IsSuccess());
    lm::CreateNetworkConfig config{};
    std::memset(&config, 0, sizeof(config));
    // PortUnreachable (code 1) turns the P2P path off, then the create is
    // answered: the server the service had started is no longer wanted.
    ScopedHook hook(g_timed_wait_hook, [&] {
        proto::NetworkErrorMessage err{};
        err.error_code = 1;
        const lm::NetworkInfo created = make_info(1);
        return env.server.send(proto::PacketId::NetworkError, err) &&
               env.server.send(
                   proto::PacketId::Connected, &created, sizeof(created)) &&
               round_trip(env);
    });
    ASSERT_TRUE(env->CreateNetwork(config).IsSuccess());
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::AccessPointCreated));
}

/**
 * @brief The Connected of an earlier join is not taken for the answer to a
 * later Connect: it times out.
 */
TEST(connecting_again_after_a_disconnect_waits_for_the_new_answer) {
    Env env;
    env.server.set_handler(join_script);
    ASSERT_TRUE(open_station(env));
    const lm::NetworkInfo target = make_info(1);
    ASSERT_TRUE(env->Connect(make_connect(), target).IsSuccess());
    ASSERT_TRUE(env->Disconnect().IsSuccess());
    // The server no longer answers: the Connected of the first join must not
    // be taken for the answer to this second request.
    env.server.set_handler(
        [](FakeServer&, proto::PacketId, const std::vector<std::uint8_t>&) {});
    ASSERT_EQ(env->Connect(make_connect(), target).GetValue(), RC_TIMEOUT);
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Station));
}

// =============================================================================
// Failure injection around hosting and the P2P connect worker
// =============================================================================

/**
 * @brief Allocation failure injection for the `new (std::nothrow)` the
 *        service uses for its P2P objects.
 *
 * The console heap returns nullptr under pressure and its `new` never throws:
 * an allocation of exactly the armed size fails once armed.
 */
namespace {
/// Allocation size that fails; 0 when no failure is armed.
std::atomic<std::size_t> g_fail_nothrow_size{0};
/// Number of allocations failed since the last arming.
std::atomic<int> g_fail_nothrow_hits{0};

/**
 * @brief Decide whether a nothrow allocation must fail.
 *
 * The operator new overloads below go through the replaceable throwing forms
 * so that the matching default operator delete (whatever allocator the build
 * uses) still releases the memory.
 * @param size Requested allocation size in bytes.
 * @return true (and the hit counter is incremented) when `size` equals the
 *         armed size.
 */
bool nothrow_must_fail(std::size_t size) noexcept {
    if (size == g_fail_nothrow_size.load()) {
        g_fail_nothrow_hits.fetch_add(1);
        return true;
    }
    return false;
}

/// Arms the allocation failure for its lifetime.
class ScopedAllocationFailure {
public:
    /**
     * @brief Arm the failure and reset the hit counter.
     * @param size Allocation size that must fail.
     */
    explicit ScopedAllocationFailure(std::size_t size) {
        g_fail_nothrow_hits = 0;
        g_fail_nothrow_size = size;
    }
    /// Disarm the failure.
    ~ScopedAllocationFailure() {
        g_fail_nothrow_size = 0;
    }
    /// Not copyable: owns the arming.
    ScopedAllocationFailure(const ScopedAllocationFailure&) = delete;
    /// Not assignable: owns the arming.
    /// @return Never returns (deleted).
    ScopedAllocationFailure& operator=(const ScopedAllocationFailure&) = delete;
    /**
     * @brief Tell whether an allocation failed since the arming.
     * @return true when at least one allocation was failed.
     */
    static bool hit() {
        return g_fail_nothrow_hits.load() > 0;
    }
};

/// Holds a TCP port so the service cannot bind it.
class PortHog {
public:
    /**
     * @brief Bind and listen on `port` on all interfaces, if possible.
     * @param port TCP port to hold.
     */
    explicit PortHog(std::uint16_t port) {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        const int on = 1;
        ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
            0) {
            ::listen(m_fd, 1);
        }
    }
    /// Release the port.
    ~PortHog() {
        ::close(m_fd);
    }
    /// Not copyable: owns a socket.
    PortHog(const PortHog&) = delete;
    /// Not assignable: owns a socket.
    /// @return Never returns (deleted).
    PortHog& operator=(const PortHog&) = delete;

private:
    /// Socket holding the port.
    int m_fd = -1;
};

/// First port the P2P server tries (also the public mapping).
constexpr std::uint16_t P2P_PORT_FIRST =
    39990;  // first port the P2P server tries
/// Number of ports the P2P server tries.
constexpr int P2P_PORT_COUNT = 10;
}  // namespace

/**
 * @brief Nothrow new that fails once for the armed allocation size.
 * @param size Requested size in bytes.
 * @return The allocation, or nullptr when failure was injected or the
 *         allocation failed.
 */
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (nothrow_must_fail(size)) {
        return nullptr;
    }
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

/**
 * @brief Aligned nothrow new that fails once for the armed allocation size.
 * @param size Requested size in bytes.
 * @param alignment Requested alignment.
 * @return The allocation, or nullptr when failure was injected or the
 *         allocation failed.
 */
void* operator new(
    std::size_t size,
    std::align_val_t alignment,
    const std::nothrow_t&) noexcept {
    if (nothrow_must_fail(size)) {
        return nullptr;
    }
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

/**
 * @brief An ExternalProxy naming the hosted server's own private port is not
 * dialed (the connect worker is not made busy), so a later real host is still
 *        joined.
 */
TEST(external_proxy_at_the_hosted_servers_private_port_is_not_dialed) {
    // The public mapping is always the first port of the range; with that port
    // unavailable the server listens on another one, and the master may name
    // either. The port is either held here or still in TIME_WAIT from an
    // earlier test (the server binds without SO_REUSEADDR, so that blocks it
    // as well): both leave it unusable, which the check below asserts.
    PortHog first(P2P_PORT_FIRST);
    Env env(true);
    ASSERT_TRUE(host_a_network(env));
    const std::uint16_t private_port = announced_p2p_port(env);
    ASSERT_TRUE(private_port != 0);
    ASSERT_TRUE(private_port != P2P_PORT_FIRST);
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ExternalProxy, external_proxy(private_port)));
    ASSERT_TRUE(round_trip(env));
    // Had the service dialed itself, its connect worker would be busy and
    // would drop this one.
    P2pHost host;
    ASSERT_TRUE(
        eventually([&] { return join_p2p_host(env, host, 0x0A720009); }, 2500));
}

/**
 * @brief When no port of the P2P range can be bound the network is created
 * through the relay and the announced P2P port is 0.
 */
TEST(a_host_without_a_free_p2p_port_creates_the_network_through_the_relay) {
    // Every port of the range is held here or still in TIME_WAIT from an
    // earlier test (which blocks the server's plain bind just the same).
    std::vector<std::unique_ptr<PortHog>> hogs;
    for (int i = 0; i < P2P_PORT_COUNT; ++i) {
        hogs.push_back(
            std::make_unique<PortHog>(
                static_cast<std::uint16_t>(P2P_PORT_FIRST + i)));
    }
    Env env(true);
    ASSERT_TRUE(host_a_network(env));
    ASSERT_EQ(announced_p2p_port(env), static_cast<std::uint16_t>(0));
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::AccessPointCreated));
}

/**
 * @brief When the P2P server object cannot be allocated, the network is created
 *        through the relay and the announced P2P port is 0.
 */
TEST(host_without_p2p_server_memory_creates_the_network_via_the_relay) {
    Env env(true);
    ScopedAllocationFailure fail(sizeof(ams::mitm::p2p::P2pProxyServer));
    ASSERT_TRUE(host_a_network(env));
    ASSERT_TRUE(ScopedAllocationFailure::hit());
    ASSERT_EQ(announced_p2p_port(env), static_cast<std::uint16_t>(0));
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::AccessPointCreated));
}

/**
 * @brief When the P2P client cannot be allocated, the attempt is abandoned
 * without dialing and the connect worker is free for the next one.
 */
TEST(unallocatable_p2p_client_abandons_the_attempt_and_frees_the_worker) {
    Env env(true);
    ASSERT_TRUE(open_station(env));
    const std::uint32_t before = address_of(env);
    {
        ScopedAllocationFailure fail(sizeof(ams::mitm::p2p::P2pProxyClient));
        P2pHost host;
        ASSERT_TRUE(env.server.send(
            proto::PacketId::ExternalProxy, external_proxy(host.port())));
        ASSERT_TRUE(eventually([] { return ScopedAllocationFailure::hit(); }));
        ASSERT_FALSE(host.accept_peer(300));
    }
    ASSERT_EQ(address_of(env), before);
    P2pHost host;
    ASSERT_TRUE(
        eventually([&] { return join_p2p_host(env, host, 0x0A720009); }, 2500));
}

/**
 * @brief When the connect worker thread cannot be started, the ExternalProxy is
 *        dropped and the worker is not left marked busy.
 */
TEST(unstartable_connect_worker_drops_the_external_proxy_and_stays_free) {
    Env env(true);
    ASSERT_TRUE(open_station(env));
    P2pHost host;
    ams::os::g_host_create_thread_result = 0x1234;
    const bool sent = env.server.send(
        proto::PacketId::ExternalProxy, external_proxy(host.port()));
    const bool echoed = sent && round_trip(env);
    ams::os::g_host_create_thread_result = 0;
    ASSERT_TRUE(echoed);
    ASSERT_FALSE(host.accept_peer(300));
    // The failed spawn released the "worker busy" flag.
    ASSERT_TRUE(join_p2p_host(env, host, 0x0A720009));
}

/**
 * @brief A host that resets the connection before the auth makes the attempt
 * end at the authentication, within 2.5 s instead of the 4 s ProxyConfig wait.
 */
TEST(p2p_host_reset_before_the_auth_abandons_the_attempt_at_once) {
    Env env(true);
    ASSERT_TRUE(open_station(env));
    P2pHost host;
    std::atomic<bool> reset_done{false};
    // The client's receive thread is started inside Connect(), after the TCP
    // connection is up and before the auth is written: hold it there until the
    // host has torn the connection down.
    arm_start_gate([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!reset_done.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ExternalProxy, external_proxy(host.port())));
    ASSERT_TRUE(host.accept_peer());
    host.reset_peer();
    reset_done = true;
    // The ProxyConfig wait would last 4 s: a worker freed within 2.5 s gave up
    // at the authentication.
    P2pHost next;
    ASSERT_TRUE(
        eventually([&] { return join_p2p_host(env, next, 0x0A720009); }, 2500));
}

/**
 * @brief Finalize while the connect worker waits for the host's ProxyConfig
 * closes the P2P connection at once.
 */
TEST(
    finalizing_while_the_connect_worker_waits_for_the_host_aborts_the_attempt) {
    Env env(true);
    ASSERT_TRUE(open_station(env));
    P2pHost silent;
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ExternalProxy, external_proxy(silent.port())));
    ASSERT_TRUE(silent.accept_peer());
    proto::PacketId id{};
    std::vector<std::uint8_t> body;
    ASSERT_TRUE(silent.read_packet(id, body));
    ASSERT_TRUE(id == proto::PacketId::ExternalProxy);
    // The worker now waits for the ProxyConfig the host never sends.
    ASSERT_TRUE(env->Finalize().IsSuccess());
    // Finalizing closed the client's connection: the host sees the end of the
    // stream instead of the worker running out its 4 s wait.
    const auto begin = std::chrono::steady_clock::now();
    ASSERT_FALSE(silent.read_packet(id, body));
    ASSERT_TRUE(
        std::chrono::steady_clock::now() - begin <
        std::chrono::milliseconds(2000));
}

/**
 * @brief When the P2P client cannot write, proxy data falls back to the master.
 */
TEST(proxy_data_that_the_p2p_client_cannot_write_goes_to_the_master_instead) {
    Env env(true);
    ASSERT_TRUE(open_station(env));
    P2pHost host;
    ASSERT_TRUE(join_p2p_host(env, host, 0x0A720009));
    ASSERT_TRUE(cut_write_side_towards(host.port()));
    const std::size_t via_master = env.server.count(proto::PacketId::ProxyData);
    ASSERT_TRUE(send_through_the_bsd_service(
        0x0A720009, 7000, 0x0A720001, 7001, {1, 2, 3}));
    ASSERT_TRUE(
        env.server.wait_for(proto::PacketId::ProxyData, 2000, via_master + 1));
}

/**
 * @brief While the master connection is down and cannot be re-established, the
 * BSD send path refuses payloads.
 */
TEST(a_game_cannot_send_while_the_connection_to_the_master_is_down) {
    Env env;
    ASSERT_TRUE(open_station(env));
    env.server.answer_handshake(false);
    env.server.drop();
    // The service stays registered for the game's sockets but is not ready
    // until a handshake completes, which this server no longer grants.
    ASSERT_TRUE(eventually([&] {
        return !send_through_the_bsd_service(0x0A720002, 1, 0x0A720001, 2, {1});
    }));
}

/**
 * @brief ProxyData relayed by the master is delivered to the proxy socket bound
 * to the destination address and port.
 */
TEST(proxy_data_from_the_master_reaches_the_socket_bound_to_the_destination) {
    Env env;
    ASSERT_TRUE(open_station(env));
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ProxyConfig, proxy_config(JOINER_VIP)));
    ASSERT_TRUE(eventually([&] { return address_of(env) == JOINER_VIP; }));
    auto* sock = bound_udp_socket(902, JOINER_VIP, 7100);
    ASSERT_TRUE(sock != nullptr);
    const std::vector<std::uint8_t> body = {4, 3, 2, 1};
    const auto packet = proxy_data(
        HOST_VIP, 5000, JOINER_VIP, 7100, proto::ProtocolType::Udp, 4, body);
    ASSERT_TRUE(env.server.send(
        proto::PacketId::ProxyData, packet.data(), packet.size()));
    ASSERT_TRUE(eventually([&] { return sock->HasPendingData(); }));
    ASSERT_EQ(sock->GetPendingDataSize(), body.size());
    ams::mitm::bsd::ProxySocketManager::GetInstance().CloseProxySocket(902);
}

/**
 * @brief ProxyData nobody listens for is queued up to the fallback limit (16
 *        packets) and the rest is dropped, without harm to the service.
 */
TEST(
    proxy_data_nobody_can_receive_is_dropped_once_the_fallback_buffer_is_full) {
    Env env;
    ASSERT_TRUE(open_station(env));
    const auto packet = proxy_data(
        HOST_VIP,
        5000,
        JOINER_VIP,
        7200,
        proto::ProtocolType::Udp,
        4,
        {1, 2, 3, 4});
    // The fallback queue holds 16 packets; the rest is dropped, not queued.
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(env.server.send(
            proto::PacketId::ProxyData, packet.data(), packet.size()));
    }
    ASSERT_TRUE(round_trip(env));
    ASSERT_EQ(get_state(*env), state_of(lm::CommState::Station));
}

/**
 * @brief Shorten every timed wait of the service (handshake 5 s, scan 1 s,
 *        ...) by a factor of 10; loopback answers in well under a
 *        millisecond.
 *
 * TEST() bodies run from static initializers, i.e. before main(), so this
 * must be a prioritized constructor. The hooks are installed here too, once:
 * they only dispatch to the slot a test armed, so the function objects
 * themselves never change under a thread. Also ignores SIGPIPE.
 */
__attribute__((constructor(101))) static void shorten_the_service_waits() {
    ams::os::g_host_wait_divisor = 10;
    ams::os::g_host_before_event_clear = [] { run_hook(g_clear_hook); };
    ams::os::g_host_before_timed_wait_any = [] { run_hook(g_timed_wait_hook); };
    ams::os::g_host_before_recursive_lock = [] {
        run_hook(g_recursive_lock_hook);
    };
    ams::os::g_host_before_start_thread = [](const char* name) {
        run_start_gate(name);
    };
    // The P2P client writes with plain send(): a write to a peer that is gone
    // must fail with EPIPE here as it does on the console, not kill the runner.
    std::signal(SIGPIPE, SIG_IGN);
}

/**
 * @brief Run every registered test.
 * @return 0 when all tests passed, 1 otherwise.
 */
int main() {
    return test_report("ICommunicationService Tests");
}
