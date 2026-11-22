/**
 * @file bsd.h
 * @brief Host stand-in for the libnx pieces of <switch/services/bsd.h> the BSD
 * MITM uses.
 */
#pragma once

#include <cstdint>
#include <poll.h>

#include <stratosphere.hpp>

using Handle = std::uint32_t;
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

struct BsdInitConfig {
    std::uint32_t version;
    void* tmem_buffer;
    std::size_t tmem_buffer_size;
    std::uint32_t tcp_tx_buf_size;
    std::uint32_t tcp_rx_buf_size;
    std::uint32_t tcp_tx_buf_max_size;
    std::uint32_t tcp_rx_buf_max_size;
    std::uint32_t udp_tx_buf_size;
    std::uint32_t udp_rx_buf_size;
    std::uint32_t sb_efficiency;
};

struct SocketInitConfig {
    std::uint32_t tcp_tx_buf_size;
    std::uint32_t tcp_rx_buf_size;
    std::uint32_t tcp_tx_buf_max_size;
    std::uint32_t tcp_rx_buf_max_size;
    std::uint32_t udp_tx_buf_size;
    std::uint32_t udp_rx_buf_size;
    std::uint32_t sb_efficiency;
    std::uint32_t num_bsd_sessions;
    BsdServiceType bsd_service_type;
};

inline const BsdInitConfig* g_host_bsd_config = nullptr;
inline std::uint32_t g_host_bsd_sessions = 0;
inline BsdServiceType g_host_bsd_type = BsdServiceType_Auto;
inline const SocketInitConfig* g_host_socket_config = nullptr;

inline std::uint32_t bsdInitialize(
    const BsdInitConfig* config, std::uint32_t sessions, BsdServiceType type) {
    g_host_boot_calls.emplace_back("bsdInitialize");
    g_host_bsd_config = config;
    g_host_bsd_sessions = sessions;
    g_host_bsd_type = type;
    return 0;
}
inline void bsdExit() {
    g_host_boot_calls.emplace_back("bsdExit");
}
inline std::uint32_t socketInitialize(const SocketInitConfig* config) {
    g_host_boot_calls.emplace_back("socketInitialize");
    g_host_socket_config = config;
    return 0;
}
inline void socketExit() {
    g_host_boot_calls.emplace_back("socketExit");
}
