/**
 * @file ldn_client_process_monitor.hpp
 * @brief Client Process Monitor implementation
 *
 * Required for firmware 18.0.0+ compatibility.
 * This is a stub implementation - the real functionality is not needed
 * but the interface must exist for games to work.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#pragma once

#include <stratosphere.hpp>
#include "interfaces/iclient_process_monitor.hpp"

namespace ams::mitm::ldn {

/**
 * @brief Stateless implementation of the client process monitor interface.
 *
 * Exposed to games alongside the LDN communication service; it only logs
 * the registration of the client process.
 */
class IClientProcessMonitor {
public:
    /// @gdb{tag="LDN:LIFECYCLE", msg="ClientProcessMonitor created"}
    IClientProcessMonitor();
    /// @gdb{tag="LDN:LIFECYCLE", msg="ClientProcessMonitor destroyed"}
    ~IClientProcessMonitor();

    /**
     * @brief Registers the calling client process (logged only).
     *
     * @param client_process_id Process id of the client, from the IPC
     *                          context.
     * @return Always ResultSuccess.
     */
    static Result RegisterClient(const sf::ClientProcessId& client_process_id);
};

static_assert(
    ams::mitm::ldn::IsIClientProcessMonitorInterface<IClientProcessMonitor>);

}  // namespace ams::mitm::ldn
