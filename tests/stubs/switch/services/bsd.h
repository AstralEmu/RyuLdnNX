/**
 * @file bsd.h
 * @brief Host stand-in for the libnx pieces of <switch/services/bsd.h> the BSD
 * MITM uses.
 */
#ifndef TESTS_STUBS_SWITCH_SERVICES_BSD_H_
#define TESTS_STUBS_SWITCH_SERVICES_BSD_H_

#include <cstdint>
#include <poll.h>

#include <stratosphere.hpp>

/// Kernel handle value
using Handle = std::uint32_t;
/// Handle value meaning "no handle"
constexpr Handle INVALID_HANDLE = 0xFFFFFFFFU;

/// Buffer attributes of a forwarded call (libnx SfBufferAttr).
enum class SfBufferAttr : std::uint32_t {
    /// Input buffer
    In = 1U << 0,
    /// Output buffer
    Out = 1U << 1,
    /// Type-A/Type-B mapped buffer
    HipcMapAlias = 1U << 2,
    /// Pointer buffer
    HipcPointer = 1U << 3,
    /// Fixed-size buffer
    FixedSize = 1U << 4,
    /// Auto-selected transfer mode
    HipcAutoSelect = 1U << 5,
    /// Accepts a non-secure mapping
    HipcMapTransferAllowsNonSecure = 1U << 6,
    /// Accepts a non-device mapping
    HipcMapTransferAllowsNonDevice = 1U << 7,
};

/// libnx spelling of SfBufferAttr::In
inline constexpr SfBufferAttr SfBufferAttr_In = SfBufferAttr::In;
/// libnx spelling of SfBufferAttr::Out
inline constexpr SfBufferAttr SfBufferAttr_Out = SfBufferAttr::Out;
/// libnx spelling of SfBufferAttr::HipcMapAlias
inline constexpr SfBufferAttr SfBufferAttr_HipcMapAlias =
    SfBufferAttr::HipcMapAlias;
/// libnx spelling of SfBufferAttr::HipcPointer
inline constexpr SfBufferAttr SfBufferAttr_HipcPointer =
    SfBufferAttr::HipcPointer;
/// libnx spelling of SfBufferAttr::FixedSize
inline constexpr SfBufferAttr SfBufferAttr_FixedSize = SfBufferAttr::FixedSize;
/// libnx spelling of SfBufferAttr::HipcAutoSelect
inline constexpr SfBufferAttr SfBufferAttr_HipcAutoSelect =
    SfBufferAttr::HipcAutoSelect;
/// libnx spelling of SfBufferAttr::HipcMapTransferAllowsNonSecure
inline constexpr SfBufferAttr SfBufferAttr_HipcMapTransferAllowsNonSecure =
    SfBufferAttr::HipcMapTransferAllowsNonSecure;
/// libnx spelling of SfBufferAttr::HipcMapTransferAllowsNonDevice
inline constexpr SfBufferAttr SfBufferAttr_HipcMapTransferAllowsNonDevice =
    SfBufferAttr::HipcMapTransferAllowsNonDevice;

/**
 * @brief Combines two buffer attributes, as libnx call sites do with `|`.
 * @param lhs left attribute
 * @param rhs right attribute
 * @return the raw libnx flag bits of both
 */
constexpr std::uint32_t operator|(SfBufferAttr lhs, SfBufferAttr rhs) {
    return static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs);
}

/// libnx socket/bsd initialization (the host records what it was given).
enum class BsdServiceType : std::uint32_t {
    /// User-mode BSD service
    User = 1U << 0,
    /// System BSD service
    System = 1U << 1,
    /// Either service flavor
    Auto = (1U << 0) | (1U << 1),
};

/// libnx spelling of BsdServiceType::User
inline constexpr BsdServiceType BsdServiceType_User = BsdServiceType::User;
/// libnx spelling of BsdServiceType::System
inline constexpr BsdServiceType BsdServiceType_System = BsdServiceType::System;
/// libnx spelling of BsdServiceType::Auto
inline constexpr BsdServiceType BsdServiceType_Auto = BsdServiceType::Auto;

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

#endif  // TESTS_STUBS_SWITCH_SERVICES_BSD_H_
