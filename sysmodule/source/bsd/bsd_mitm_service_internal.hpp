/**
 * @file bsd_mitm_service_internal.hpp
 * @brief Test seam of the BSD MITM service (bsd/bsd_mitm_service.cpp)
 *
 * The host tests (tests/bsd_mitm_service_tests.cpp) stage the exact states the
 * service defends against: the socket tracking table, the per-PID session
 * counters and the abandoned forward services. The fixed-size types and caps
 * are defined here, and the tables and find helpers are declared here, so the
 * test links against bsd_mitm_service.cpp as an ordinary translation unit
 * instead of pulling the implementation in with an include.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#ifndef SYSMODULE_SOURCE_BSD_BSD_MITM_SERVICE_INTERNAL_HPP_
#define SYSMODULE_SOURCE_BSD_BSD_MITM_SERVICE_INTERNAL_HPP_

#include <stratosphere.hpp>
#include <stratosphere/sf/sf_mitm_dispatch.h>

extern "C" {
#include <switch/services/bsd.h>
}

#include <array>
#include <cstddef>
#include <memory>

#include "../ldn/ldn_shared_state.hpp"
#include "bsd_mitm_service.hpp"
#include "bsd_types.hpp"
#include "proxy_socket_manager.hpp"

namespace ams::mitm::bsd {

// =============================================================================
// Fixed-size tracking tables
// =============================================================================
// The caps and entry types below are defined in this header because the tests
// size their loops from the caps and dereference entries returned by the find
// helpers. Their values and layout are the ones bsd_mitm_service.cpp used when
// it owned them.

/// @brief Cap for the g_socket_info entries.
constexpr size_t MAX_SOCKET_INFO = 128;
/// @brief Cap for the g_mitm_pid_count entries.
constexpr size_t MAX_MITM_PIDS = 32;
/// @brief Cap for the g_abandoned_forward_services entries.
constexpr size_t MAX_ABANDONED_SERVICES = 32;

/**
 * @brief Info stored for each socket created via Socket()
 *
 * Needed because when Bind/Connect is called later, the socket type and
 * protocol are required to create the ProxySocket.
 */
struct SocketInfo {
    /// Socket type (stream or datagram) requested in Socket().
    ryu_ldn::bsd::SocketType type;
    /// Protocol (TCP or UDP) requested in Socket().
    ryu_ldn::bsd::ProtocolType protocol;
    /// True if this socket is an LDN proxy socket.
    bool is_proxy;
    /// SO_BROADCAST option (tracked before proxy creation).
    bool broadcast;
    /// Session ID that owns this socket (for cleanup on session close).
    u32 session_id;
};

/**
 * @brief One slot of the socket info tracking table.
 */
struct SocketInfoEntry {
    /// File descriptor key, -1 when the slot is free.
    s32 fd = -1;
    /// Tracked type/protocol/proxy state of the fd.
    SocketInfo info{};
    /// True when the slot is in use.
    bool valid = false;
};

/**
 * @brief One slot of the per-PID BSD session counter table.
 */
struct MitmPidEntry {
    /// Process id key.
    u64 pid = 0;
    /// Number of BSD sessions intercepted for this pid.
    u32 count = 0;
    /// True when the slot is in use.
    bool valid = false;
};

/**
 * @brief One slot of the abandoned forward services table.
 */
struct AbandonedServiceEntry {
    /// Kept-alive forward service of the abandoned session.
    std::shared_ptr<::Service> service;
    /// True when the slot is in use.
    bool valid = false;
};

/**
 * @brief Global socket info tracking table (fixed-size flat array).
 *
 * @role Maps every fd created via the BSD MITM `Socket()` to its SocketInfo so
 *       later `Bind`/`Connect` calls can recover the type/protocol and decide
 *       whether to wrap the fd in a ProxySocket for LDN tunneling.
 * @modified_by `BsdMitmService` member functions in bsd_mitm_service.cpp
 *              (Socket, Bind, Connect, Close, plus session cleanup) via
 *              `FindOrCreateSocketInfoEntry` and `EraseSocketInfoEntry` — all
 *              under g_socket_info_mutex.
 * @thread_safety Protected by g_socket_info_mutex. Every reader and writer in
 *                bsd_mitm_service.cpp takes a scoped lock on it before touching
 *                the array; the proxy socket close path copies entries out
 *                under the mutex and closes the proxy socket without holding
 *                it to avoid lock-order inversion.
 */
extern std::array<SocketInfoEntry, MAX_SOCKET_INFO> g_socket_info;

/**
 * @brief Global per-PID BSD session counter table (fixed-size flat array).
 *
 * @role Tracks how many BSD sessions have been intercepted per process so the
 *       MITM can keep intercepting ALL BSD sessions from whitelisted games
 *       (some games, e.g. Mario Kart 8, open several bsd:u sessions).
 * @modified_by `BsdMitmService` constructor and destructor paths via
 *              `FindOrCreateMitmPidEntry` and direct count increments, all
 *              under g_mitm_pids_mutex.
 * @thread_safety Protected by g_mitm_pids_mutex. Every access in
 *                bsd_mitm_service.cpp is wrapped in a scoped lock on it.
 */
extern std::array<MitmPidEntry, MAX_MITM_PIDS> g_mitm_pid_count;

/**
 * @brief Global table of abandoned forward services (fixed-size flat array).
 *
 * @role When a BSD MITM session is closed without ever receiving
 *       `RegisterClient`, its forward_service is moved here instead of being
 *       closed, to avoid the system freeze that closing an unregistered
 *       bsd:u session triggers. Entries are cleaned up on game process exit
 *       or on explicit LDN-disconnect cleanup.
 * @modified_by `BsdMitmService` destructor / cleanup paths in
 *              bsd_mitm_service.cpp (e.g. `CleanupAbandonedServices`, the
 *              destructor's forward service abandonment path) — all under
 *              g_abandoned_services_mutex.
 * @thread_safety Protected by g_abandoned_services_mutex. Every reader and
 *                writer in bsd_mitm_service.cpp takes a scoped lock on it
 *                first.
 */
extern std::array<AbandonedServiceEntry, MAX_ABANDONED_SERVICES>
    g_abandoned_forward_services;

/**
 * @brief Mutex protecting g_abandoned_forward_services.
 *
 * Declared here because the tests install a lock hook that refills the table
 * while a cleanup holds it, and compare the hook argument to this address.
 */
extern os::Mutex g_abandoned_services_mutex;

/**
 * @brief Find a socket info entry by fd.
 *
 * The caller must hold g_socket_info_mutex.
 *
 * @param fd File descriptor to look up.
 * @return The valid entry for @p fd, or nullptr if none is tracked.
 */
SocketInfoEntry* FindSocketInfoEntry(s32 fd);

/**
 * @brief Find an existing socket info entry or allocate a new one.
 *
 * The caller must hold g_socket_info_mutex. A new entry has
 * default-initialized info.
 *
 * @param fd File descriptor to look up or register.
 * @return The entry for @p fd, or nullptr when the table is full.
 */
SocketInfoEntry* FindOrCreateSocketInfoEntry(s32 fd);

/**
 * @brief Find a mitm pid entry by pid.
 *
 * The caller must hold g_mitm_pids_mutex.
 *
 * @param pid Process id to look up.
 * @return The valid entry for @p pid, or nullptr if none exists.
 */
MitmPidEntry* FindMitmPidEntry(u64 pid);

}  // namespace ams::mitm::bsd

#endif  // SYSMODULE_SOURCE_BSD_BSD_MITM_SERVICE_INTERNAL_HPP_
