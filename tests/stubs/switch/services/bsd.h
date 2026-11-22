/**
 * @file bsd.h
 * @brief Host stand-in for the libnx pieces of <switch/services/bsd.h> the BSD
 * MITM uses.
 */
#pragma once

#include <cstdint>
#include <poll.h>

#include <stratosphere.hpp>

/// Kernel handle value
using Handle = std::uint32_t;
/// Handle value meaning "no handle"
constexpr Handle INVALID_HANDLE = 0xFFFFFFFFu;

/// Buffer attributes of a forwarded call (libnx SfBufferAttr).
enum SfBufferAttr : std::uint32_t {
    SfBufferAttr_In = 1u << 0,
    SfBufferAttr_Out = 1u << 1,
    SfBufferAttr_HipcMapAlias = 1u << 2,
    SfBufferAttr_HipcPointer = 1u << 3,
    SfBufferAttr_FixedSize = 1u << 4,
    SfBufferAttr_HipcAutoSelect = 1u << 5,
    SfBufferAttr_HipcMapTransferAllowsNonSecure = 1u << 6,
    SfBufferAttr_HipcMapTransferAllowsNonDevice = 1u << 7,
};

/// libnx socket/bsd initialization (the host records what it was given).
enum BsdServiceType : std::uint32_t {
    BsdServiceType_User = 1u << 0,
    BsdServiceType_System = 1u << 1,
    BsdServiceType_Auto = BsdServiceType_User | BsdServiceType_System,
};

/// Configuration given to bsdInitialize (libnx BsdInitConfig)
struct BsdInitConfig {
    /// Structure version
    std::uint32_t version;
    /// Transfer memory buffer backing the service
    void* tmem_buffer;
    /// Size of tmem_buffer in bytes
    std::size_t tmem_buffer_size;
    /// TCP transmit buffer size in bytes
    std::uint32_t tcp_tx_buf_size;
    /// TCP receive buffer size in bytes
    std::uint32_t tcp_rx_buf_size;
    /// TCP transmit buffer maximum size in bytes
    std::uint32_t tcp_tx_buf_max_size;
    /// TCP receive buffer maximum size in bytes
    std::uint32_t tcp_rx_buf_max_size;
    /// UDP transmit buffer size in bytes
    std::uint32_t udp_tx_buf_size;
    /// UDP receive buffer size in bytes
    std::uint32_t udp_rx_buf_size;
    /// Socket buffer efficiency factor
    std::uint32_t sb_efficiency;
};

/// Configuration given to socketInitialize (libnx SocketInitConfig)
struct SocketInitConfig {
    /// TCP transmit buffer size in bytes
    std::uint32_t tcp_tx_buf_size;
    /// TCP receive buffer size in bytes
    std::uint32_t tcp_rx_buf_size;
    /// TCP transmit buffer maximum size in bytes
    std::uint32_t tcp_tx_buf_max_size;
    /// TCP receive buffer maximum size in bytes
    std::uint32_t tcp_rx_buf_max_size;
    /// UDP transmit buffer size in bytes
    std::uint32_t udp_tx_buf_size;
    /// UDP receive buffer size in bytes
    std::uint32_t udp_rx_buf_size;
    /// Socket buffer efficiency factor
    std::uint32_t sb_efficiency;
    /// Number of BSD sessions to open
    std::uint32_t num_bsd_sessions;
    /// BSD service flavor to connect to
    BsdServiceType bsd_service_type;
};

/// Config given to the last bsdInitialize (not copied)
inline const BsdInitConfig* g_host_bsd_config = nullptr;
/// Session count given to the last bsdInitialize
inline std::uint32_t g_host_bsd_sessions = 0;
/// Service type given to the last bsdInitialize
inline BsdServiceType g_host_bsd_type = BsdServiceType_Auto;
/// Config given to the last socketInitialize (not copied)
inline const SocketInitConfig* g_host_socket_config = nullptr;

/**
 * @brief Records the call in g_host_boot_calls and keeps the arguments
 *
 * @param config bsd configuration, kept by pointer
 * @param sessions number of sessions
 * @param type service flavor
 * @return 0, success
 */
inline std::uint32_t bsdInitialize(
    const BsdInitConfig* config, std::uint32_t sessions, BsdServiceType type) {
    g_host_boot_calls.emplace_back("bsdInitialize");
    g_host_bsd_config = config;
    g_host_bsd_sessions = sessions;
    g_host_bsd_type = type;
    return 0;
}
/// Records the call in g_host_boot_calls
inline void bsdExit() {
    g_host_boot_calls.emplace_back("bsdExit");
}
/**
 * @brief Records the call in g_host_boot_calls and keeps the pointer
 *
 * @param config socket configuration, kept by pointer
 * @return 0, success
 */
inline std::uint32_t socketInitialize(const SocketInitConfig* config) {
    g_host_boot_calls.emplace_back("socketInitialize");
    g_host_socket_config = config;
    return 0;
}
/// Records the call in g_host_boot_calls
inline void socketExit() {
    g_host_boot_calls.emplace_back("socketExit");
}
