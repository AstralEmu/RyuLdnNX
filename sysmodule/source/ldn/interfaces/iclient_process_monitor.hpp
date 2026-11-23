/**
 * @file iclient_process_monitor.hpp
 * @brief Client Process Monitor interface definition
 *
 * Required for firmware 18.0.0+ compatibility.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#ifndef SYSMODULE_SOURCE_LDN_INTERFACES_ICLIENT_PROCESS_MONITOR_HPP_
#define SYSMODULE_SOURCE_LDN_INTERFACES_ICLIENT_PROCESS_MONITOR_HPP_

#include <stratosphere.hpp>

/**
 * @brief Method table of IClientProcessMonitorInterface.
 *
 * Declares the single command 0, RegisterClient, taking the caller's
 * ClientProcessId. Instantiated by AMS_SF_DEFINE_INTERFACE below.
 *
 * @param C Interface class name supplied by the SF macro.
 * @param H Handler macro supplied by the SF macro.
 */
#define AMS_ICLIENT_PROCESS_MONITOR_INTERFACE(C, H)          \
    AMS_SF_METHOD_INFO(                                      \
        C,                                                   \
        H,                                                   \
        0,                                                   \
        Result,                                              \
        RegisterClient,                                      \
        (const ams::sf::ClientProcessId& client_process_id), \
        (client_process_id))

AMS_SF_DEFINE_INTERFACE(
    ams::mitm::ldn,
    IClientProcessMonitorInterface,
    AMS_ICLIENT_PROCESS_MONITOR_INTERFACE,
    0x4EF8C3F3)
#endif  // SYSMODULE_SOURCE_LDN_INTERFACES_ICLIENT_PROCESS_MONITOR_HPP_
