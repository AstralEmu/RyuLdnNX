/**
 * @file main_internal.hpp
 * @brief Test seam of the sysmodule entry point (sysmodule/source/main.cpp)
 *
 * The host boot tests (tests/main_tests.cpp) drive the real boot code of
 * main.cpp: its server managers, its 384 KB heap, its libnx socket
 * configuration and its startup/shutdown entry points. Those symbols are
 * defined by main.cpp and declared here, so the test links against them as an
 * ordinary translation unit instead of pulling main.cpp in with an include.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#ifndef SYSMODULE_SOURCE_MAIN_INTERNAL_HPP_
#define SYSMODULE_SOURCE_MAIN_INTERNAL_HPP_

#include <stratosphere.hpp>

#include <cstddef>
#include <memory>

extern "C" {
#include <switch/services/bsd.h>
}

namespace ams::internal {

// ====================================================================
// Memory and socket configuration
// ====================================================================
// The constants and the configuration objects below are defined in this
// header because the socket transfer memory buffer is sized from them with a
// constant expression. Their values, types and initialisers are the ones
// main.cpp used when it owned them.

/// @brief TCP transmit buffer initial size (bytes)
constexpr u32 TCP_TX_BUF_SIZE = 0x800;
/// @brief TCP receive buffer initial size (bytes)
constexpr u32 TCP_RX_BUF_SIZE = 0x1000;
/// @brief TCP transmit buffer max size (bytes)
constexpr u32 TCP_TX_BUF_MAX_SIZE = 0x2000;
/// @brief TCP receive buffer max size (bytes)
constexpr u32 TCP_RX_BUF_MAX_SIZE = 0x2000;
/// @brief UDP transmit buffer size (bytes)
constexpr u32 UDP_TX_BUF_SIZE = 0x2000;
/// @brief UDP receive buffer size (bytes)
constexpr u32 UDP_RX_BUF_SIZE = 0x2000;
/// @brief Socket buffer efficiency factor (sb_efficiency in libnx)
constexpr u32 SOCKET_BUFFER_EFFICIENCY = 4;
/// @brief Maximum number of concurrent BSD IPC sessions (ConcurrencyLimitMax)
constexpr u32 BSD_MAX_SESSIONS = 14;
/// @brief BSD init protocol version
constexpr u32 BSD_INIT_VERSION = 1;
/// @brief Alignment of the expanded heap backing buffer (bytes)
constexpr size_t HEAP_ALIGNMENT = 0x40;
/// @brief Number of MITM ports handled by ServerManager (ldn:u + bsd:u)
constexpr size_t MITM_PORT_COUNT = 2;
/// @brief Number of ryu:cfg config IPC ports
constexpr size_t CFG_PORT_COUNT = 1;

/**
 * @brief Main malloc buffer size.
 *
 * Switch sysmodules share ~10 MB total, keep this small. 1 MB is sufficient
 * for TlsHeapCentral and gameplay traffic.
 */
constexpr size_t MallocBufferSize = 1_MB;

/// @brief Size of the expanded heap backing buffer (bytes)
constexpr size_t HEAP_SIZE = 384_KB;

/**
 * @brief Backing store of the libc malloc heap.
 *
 * Page aligned, zero-initialized at load time (constinit). Defined by
 * main.cpp.
 */
alignas(os::MemoryPageSize) extern constinit u8
    g_malloc_buffer[MallocBufferSize];

/**
 * @brief Compute, at compile time, the transfer memory libnx needs for BSD.
 *
 * Mirrors libnx's own sizing: the TCP max sizes (falling back to the plain
 * sizes when 0) plus the UDP sizes, page aligned, times sb_efficiency.
 *
 * @param config Socket buffer configuration; must be a constant expression.
 * @return Transfer memory size in bytes.
 */
consteval size_t GetLibnxBsdTransferMemorySize(
    const ::SocketInitConfig* config) {
    const u32 tcp_tx_buf_max_size = config->tcp_tx_buf_max_size != 0
                                        ? config->tcp_tx_buf_max_size
                                        : config->tcp_tx_buf_size;
    const u32 tcp_rx_buf_max_size = config->tcp_rx_buf_max_size != 0
                                        ? config->tcp_rx_buf_max_size
                                        : config->tcp_rx_buf_size;
    const u32 sum = tcp_tx_buf_max_size + tcp_rx_buf_max_size +
                    config->udp_tx_buf_size + config->udp_rx_buf_size;

    return static_cast<size_t>(config->sb_efficiency) *
           util::AlignUp(sum, os::MemoryPageSize);
}

/// @brief Socket initialization configuration
inline constexpr ::SocketInitConfig LibnxSocketInitConfig = {
    .tcp_tx_buf_size = TCP_TX_BUF_SIZE,
    .tcp_rx_buf_size = TCP_RX_BUF_SIZE,
    .tcp_tx_buf_max_size = TCP_TX_BUF_MAX_SIZE,
    .tcp_rx_buf_max_size = TCP_RX_BUF_MAX_SIZE,
    .udp_tx_buf_size = UDP_TX_BUF_SIZE,
    .udp_rx_buf_size = UDP_RX_BUF_SIZE,
    .sb_efficiency = SOCKET_BUFFER_EFFICIENCY,
    // num_bsd_sessions = number of IPC sessions to bsd:s — i.e. the
    // max concurrency for blocking BSD calls. Each blocked recv()/
    // accept()/send() holds one session for the entire IPC round-trip.
    //
    // Worst-case host-side concurrency in P2P mode (LDN = 8 players
    // total = host + 7 joiners):
    //   - master TCP recv          : 1
    //   - P2pProxyServer accept    : 1
    //   - P2pProxyClient (loopback) recv : 1
    //   - 8 P2pProxySession recv   (1 loopback + 7 joiners)
    //   = 11 blocking calls + marge for setsockopt/bind/send.
    //
    // The default of 3 saturated as soon as the loopback session was
    // alive (master recv + accept + 2 loopback recvs = 4 already > 3),
    // and bsd:s on Switch returns errno=113 (EHOSTUNREACH-mapped
    // "no resources") on the 4th concurrent call instead of blocking
    // — which is what made the AcceptLoop spin endlessly.
    //
    // 14 is ConcurrencyLimitMax in libstratosphere
    // (socket_constants.hpp:35), the highest value the kernel will
    // accept; transfer-memory size is independent of this count, so
    // headroom is free.
    .num_bsd_sessions = BSD_MAX_SESSIONS,
    // bsd:s (System) is required for privileged socket options like
    // IP_MULTICAST_TTL / IP_MULTICAST_IF / IP_ADD_MEMBERSHIP that
    // miniupnpc's upnpDiscover() relies on. bsd:u returned EPERM and
    // miniupnpc crashed instead of handling the error gracefully.
    .bsd_service_type = BsdServiceType_System,
};

/**
 * @brief Socket transfer memory buffer.
 *
 * Sized from LibnxSocketInitConfig at compile time, page aligned.
 * Defined by main.cpp.
 */
alignas(os::MemoryPageSize) extern constinit u8
    g_socket_tmem_buffer[GetLibnxBsdTransferMemorySize(
        std::addressof(LibnxSocketInitConfig))];

/// @brief BSD initialization configuration
inline constexpr ::BsdInitConfig LibnxBsdInitConfig = {
    .version = BSD_INIT_VERSION,
    .tmem_buffer = g_socket_tmem_buffer,
    .tmem_buffer_size = sizeof(g_socket_tmem_buffer),
    .tcp_tx_buf_size = LibnxSocketInitConfig.tcp_tx_buf_size,
    .tcp_rx_buf_size = LibnxSocketInitConfig.tcp_rx_buf_size,
    .tcp_tx_buf_max_size = LibnxSocketInitConfig.tcp_tx_buf_max_size,
    .tcp_rx_buf_max_size = LibnxSocketInitConfig.tcp_rx_buf_max_size,
    .udp_tx_buf_size = LibnxSocketInitConfig.udp_tx_buf_size,
    .udp_rx_buf_size = LibnxSocketInitConfig.udp_rx_buf_size,
    .sb_efficiency = LibnxSocketInitConfig.sb_efficiency,
};

}  // namespace ams::internal

namespace ams::mitm {

// ========================================================================
// MITM Server Configuration
// ========================================================================

/// @brief Total number of threads for request processing
constexpr size_t TotalThreads = 2;
/// @brief Worker threads started in addition to the main thread, which also
/// runs the server loop.
constexpr size_t NumExtraThreads = TotalThreads - 1;

/// @brief Port index of the ldn:u MITM service
constexpr int PortIndex_LdnMitm = 0;
/// @brief Port index of the bsd:u MITM service (second port registered)
constexpr int PortIndex_BsdMitm = 1;

/// @brief Maximum concurrent sessions.
///
/// Higher value needed when intercepting all applications for BSD MITM.
constexpr size_t MaxSessions = 16;

/// @brief Server manager options.
struct LdnMitmManagerOptions {
    /// @brief Size of the per-session IPC pointer buffer (bytes).
    static constexpr size_t PointerBufferSize = 0x1000;
    /// @brief Maximum number of HIPC domains.
    static constexpr size_t MaxDomains = 0x10;
    /// @brief Maximum number of objects across all domains.
    static constexpr size_t MaxDomainObjects = 0x100;
    /// @brief Deferred IPC requests are not used.
    static constexpr bool CanDeferInvokeRequest = false;
    /// @brief Required: this manager hosts MITM ports.
    static constexpr bool CanManageMitmServers = true;

    // The options above are only consumed by the atmosphere template, so
    // state their non-negotiable contract here, next to the values.
    static_assert(PointerBufferSize > 0, "a session needs a pointer buffer");
    static_assert(MaxDomains > 0, "a MITM manager needs a domain");
    static_assert(MaxDomainObjects > 0, "a domain needs at least one object");
    static_assert(
        !CanDeferInvokeRequest, "the LDN service never defers a request");
    static_assert(CanManageMitmServers, "this manager hosts ldn:u and bsd:u");
};

/// @brief Custom server manager for MITM (ldn:u + bsd:u)
class ServerManager final : public sf::hipc::ServerManager<
                                internal::MITM_PORT_COUNT,
                                LdnMitmManagerOptions,
                                MaxSessions> {
private:
    /**
     * @brief Accepts a MITM session on the given port and creates its
     *        service object.
     *
     * @param port_index PortIndex_LdnMitm or PortIndex_BsdMitm.
     * @param server Server that owns the pending session.
     * @return Result of AcceptMitmImpl; an unknown port index aborts
     *         (AMS_ABORT).
     */
    ams::Result OnNeedsToAccept(int port_index, Server* server) override;
};

/**
 * @brief HIPC server manager driving the ldn:u and bsd:u MITM services.
 *
 * @role Owns the two MITM ports (PortIndex_LdnMitm, PortIndex_BsdMitm) and
 * dispatches incoming IPC sessions to the corresponding service implementations
 * via `OnNeedsToAccept`. Pumped by `LoopProcess()` from the main server thread
 *       (and the extra worker threads), so all acceptance is single-threaded
 * per server manager instance.
 * @modified_by `mitm::InitializeSystemModule()` registers the MITM ports once
 * at boot; `ServerManager::OnNeedsToAccept` accepts sessions at runtime. No
 * external code mutates this object after initialization.
 * @thread_safety Atmosphere's `sf::hipc::ServerManager` is not internally
 * synchronized; it is driven by `LoopProcess()` which must be called from a
 * single thread per instance. Here it is pumped from the main thread plus the
 *                `NumExtraThreads` worker threads, all calling `LoopProcess()`
 * on the same object — the manager's internal queue serializes dispatch safely
 *                across those threads. No other concurrent access occurs.
 */
extern ServerManager g_server_manager;

/**
 * @brief Monotonic counter assigning a unique ID to each accepted ldn:u MITM
 * session.
 *
 * @role Generates the per-session identifier surfaced in `LOG_INFO` traces so
 * concurrent LDN sessions can be told apart in logs.
 * @modified_by `ServerManager::OnNeedsToAccept` (this file, PortIndex_LdnMitm
 * branch), via `++g_ldn_session_counter` when a new ldn:u session is accepted.
 * @thread_safety Not protected by any mutex. Acceptance is single-threaded by
 * Atmosphere's server manager dispatch loop, so the increment is implicitly
 * serialized. Reads from other threads would be racy; no such reads exist
 * today.
 */
extern u32 g_ldn_session_counter;

/**
 * @brief Monotonic counter assigning a unique ID to each accepted bsd:u MITM
 * session.
 *
 * @role Same purpose as g_ldn_session_counter but for the bsd:u MITM, so BSD
 * sessions can be correlated in logs.
 * @modified_by `ServerManager::OnNeedsToAccept` (this file, PortIndex_BsdMitm
 * branch), via `++g_bsd_session_counter` when a new bsd:u session is accepted.
 * @thread_safety Not protected by any mutex. Same single-threaded-accept
 * rationale as g_ldn_session_counter; only `OnNeedsToAccept` mutates it.
 */
extern u32 g_bsd_session_counter;

/// @brief Backing memory of the 384 KB expanded heap used by new/delete.
alignas(internal::HEAP_ALIGNMENT) extern constinit u8
    g_heap_memory[internal::HEAP_SIZE];

/**
 * @brief Returns the expanded heap handle, creating the heap on first use.
 *
 * @return Handle of the 384 KB expanded heap.
 */
lmem::HeapHandle GetHeapHandle();

/**
 * @brief Allocates from the expanded heap (target of the new operators).
 *
 * @param size Number of bytes to allocate.
 * @return Pointer to the block, or nullptr if the heap is exhausted.
 */
void* Allocate(size_t size);

/**
 * @brief Returns a block to the expanded heap.
 *
 * @param p Block obtained from Allocate().
 * @param size Ignored; the heap tracks block sizes itself.
 */
void Deallocate(void* p, size_t size);

}  // namespace ams::mitm

namespace ams::cfg {

// ========================================================================
// Configuration IPC Service (ryu:cfg)
// ========================================================================

/// @brief Maximum concurrent sessions for config service
constexpr size_t MaxSessions = 2;

/// @brief Server manager options for config service
struct ConfigServerManagerOptions {
    /// @brief Size of the per-session IPC pointer buffer (bytes).
    static constexpr size_t PointerBufferSize = 0x100;
    /// @brief No HIPC domains are used.
    static constexpr size_t MaxDomains = 0;
    /// @brief No domain objects are used.
    static constexpr size_t MaxDomainObjects = 0;
    /// @brief Deferred IPC requests are not used.
    static constexpr bool CanDeferInvokeRequest = false;
    /// @brief ryu:cfg is a regular service, not a MITM.
    static constexpr bool CanManageMitmServers = false;

    // The options above are only consumed by the atmosphere template, so
    // state their non-negotiable contract here, next to the values.
    static_assert(PointerBufferSize > 0, "a session needs a pointer buffer");
    static_assert(MaxDomains == 0, "ryu:cfg hands out no domain");
    static_assert(MaxDomainObjects == 0, "ryu:cfg hands out no domain object");
    static_assert(!CanDeferInvokeRequest, "ryu:cfg never defers a request");
    static_assert(!CanManageMitmServers, "ryu:cfg is not a MITM service");
};

/// @brief Server manager for ryu:cfg service
using ConfigServerManager = sf::hipc::ServerManager<
    internal::CFG_PORT_COUNT,
    ConfigServerManagerOptions,
    MaxSessions>;

/**
 * @brief HIPC server manager driving the custom `ryu:cfg` IPC service.
 *
 * @role Owns the single config service port that the Tesla overlay talks to for
 * live configuration changes and LDN status display. Pumped by `LoopProcess()`
 * from the dedicated config thread started in `InitializeSystemModule()`.
 * @modified_by `cfg::InitializeConfig()` registers the `ConfigService` object
 * once at boot; no further mutation occurs after registration. Runtime traffic
 * is read-only with respect to the manager itself.
 * @thread_safety Same caveat as `mitm::g_server_manager`:
 * `sf::hipc::ServerManager` is not internally synchronized and must be pumped
 * from a single thread. Here it is driven only by the config thread via
 * `LoopConfigServerThread`, so the implicit single-pumper contract is
 * satisfied.
 */
extern ConfigServerManager g_config_server_manager;

/// @brief Thread object of the ryu:cfg server thread (uses g_thread_stack).
extern os::ThreadType g_thread;

/// @brief Thread object of the log maintenance thread.
extern os::ThreadType g_log_thread;

/**
 * @brief Log maintenance thread entry point (checks file idle timeout).
 *
 * @param unused Thread argument, not used.
 */
void LoopLogMaintenanceThread(void* unused);

}  // namespace ams::cfg

namespace ams::init {

/**
 * @brief Boot-time initialization: sm, filesystem and SD mount, config and
 *        logger, game whitelist, then nifm, bsd and socket services.
 *
 * Any failure aborts (R_ABORT_UNLESS); config load problems fall back to
 * defaults.
 */
void InitializeSystemModule();

/**
 * @brief Flushes logs and shuts down socket, bsd, nifm and the SD mount, in
 *        reverse initialization order.
 */
void FinalizeSystemModule();

/// @brief Early boot hook: sets up the malloc allocator on g_malloc_buffer.
void Startup();

}  // namespace ams::init

namespace ams {

/**
 * @brief Process exit hook; must never run for this immortal sysmodule.
 *
 * @param rc Exit code (unused); the function aborts.
 * @return Never returns.
 */
NORETURN void Exit(int rc);

/**
 * @brief Sysmodule main: registers ryu:cfg, ldn:u and bsd:u, starts the
 *        config, log-maintenance and MITM threads, then blocks forever on
 *        the MITM thread.
 */
void Main();

}  // namespace ams

#endif  // SYSMODULE_SOURCE_MAIN_INTERNAL_HPP_
