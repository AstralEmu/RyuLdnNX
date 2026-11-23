/**
 * @file ldn_types.hpp
 * @brief Nintendo LDN (Local Data Network) protocol types
 *
 * This module defines the data structures used by Nintendo's LDN service
 * for local wireless communication between Switch consoles. These types
 * are used by games when calling the ldn:u service.
 *
 * Based on reverse engineering from ldn_mitm and switchbrew documentation.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#pragma once

#include <stratosphere.hpp>

namespace ams::mitm::ldn {

// ============================================================================
// Constants
// ============================================================================

/// Maximum length of SSID (network name)
constexpr size_t SsidLengthMax = 32;

/// Maximum size of advertise data
constexpr size_t AdvertiseDataSizeMax = 384;

/// Maximum length of user name
constexpr size_t UserNameBytesMax = 32;

/// Maximum number of nodes in a network
constexpr int NodeCountMax = 8;

/// Maximum number of stations (clients) - one less than nodes (host excluded)
constexpr int StationCountMax = NodeCountMax - 1;

/// Maximum length of passphrase
constexpr size_t PassphraseLengthMax = 64;

// ============================================================================
// Enumerations
// ============================================================================

/**
 * @brief LDN communication state
 *
 * Represents the current state of the LDN service for a client.
 */
enum class CommState : u32 {
    None = 0,                ///< Not initialized
    Initialized = 1,         ///< Initialized, ready to open AP or Station
    AccessPoint = 2,         ///< Access point mode, ready to create network
    AccessPointCreated = 3,  ///< Network created, accepting connections
    Station = 4,             ///< Station mode, ready to scan/connect
    StationConnected = 5,    ///< Connected to a network
    Error = 6                ///< Error state
};

/**
 * @brief Node state change types
 *
 * Used to notify applications of player join/leave events.
 */
enum class NodeStateChange : u8 {
    None = 0,                 ///< No change
    Connect = 1,              ///< Node connected
    Disconnect = 2,           ///< Node disconnected
    DisconnectAndConnect = 3  ///< Node disconnected then reconnected
};

/**
 * @brief Scan filter flags
 *
 * Flags to control what networks are returned by Scan().
 */
enum ScanFilterFlag : u32 {
    ScanFilterFlag_LocalCommunicationId = 1 << 0,
    ScanFilterFlag_SessionId = 1 << 1,
    ScanFilterFlag_NetworkType = 1 << 2,
    ScanFilterFlag_Ssid = 1 << 4,
    ScanFilterFlag_SceneId = 1 << 5,
    ScanFilterFlag_IntentId =
        ScanFilterFlag_LocalCommunicationId | ScanFilterFlag_SceneId,
    ScanFilterFlag_NetworkId =
        ScanFilterFlag_IntentId | ScanFilterFlag_SessionId
};

/**
 * @brief Disconnect reason codes
 */
enum class DisconnectReason : u32 {
    None = 0,
    User = 1,               ///< User requested disconnect
    SystemRequest = 2,      ///< System requested disconnect
    DestroyedByUser = 3,    ///< Network destroyed by host
    DestroyedBySystem = 4,  ///< Network destroyed by system
    Rejected = 5,           ///< Connection rejected by host
    SignalLost = 6          ///< Connection lost
};

// ============================================================================
// Basic Types
// ============================================================================

/**
 * @brief MAC address structure
 */
struct MacAddress {
    /// The six address bytes, in transmission order.
    u8 raw[6];

    /// @param b Address to compare with.
    /// @return True when all six bytes are equal.
    bool operator==(const MacAddress& b) const {
        return std::memcmp(raw, b.raw, sizeof(raw)) == 0;
    }
};
static_assert(sizeof(MacAddress) == 6);

/**
 * @brief Network SSID (name)
 */
struct Ssid {
    /// Number of meaningful characters in raw (0 to SsidLengthMax).
    u8 length;
    /// SSID characters, NUL terminated at raw[length] by operator=.
    char raw[SsidLengthMax + 1];

    /// @param b SSID to compare with.
    /// @return True when lengths match and the first length bytes are equal.
    bool operator==(const Ssid& b) const {
        if (length != b.length)
            return false;
        return std::memcmp(raw, b.raw, length) == 0;
    }

    /// Assigns from a C string, truncating to SsidLengthMax characters.
    /// @param s NUL-terminated string; must not be null.
    /// @return This object.
    Ssid& operator=(const char* s) {
        size_t len = std::strlen(s);
        if (len > SsidLengthMax)
            len = SsidLengthMax;
        length = static_cast<u8>(len);
        std::memcpy(raw, s, len);
        raw[len] = '\0';
        return *this;
    }
};
static_assert(sizeof(Ssid) == 34);

/**
 * @brief Session identifier
 *
 * Stored as raw bytes to avoid alignment issues in packed IPC structures.
 * The Switch IPC expects structures packed without padding.
 */
struct SessionId {
    /// The 16 opaque identifier bytes.
    u8 raw[16];

    /// @param b Session id to compare with.
    /// @return True when all 16 bytes are equal.
    bool operator==(const SessionId& b) const {
        return std::memcmp(raw, b.raw, sizeof(raw)) == 0;
    }
};
static_assert(sizeof(SessionId) == 16);

/**
 * @brief Intent identifier (game + scene)
 *
 * Packed to avoid alignment issues in IPC structures.
 */
struct __attribute__((packed)) IntentId {
    u64 localCommunicationId;  ///< Title ID / Game ID
    u8 _unk1[2];               ///< Unknown, kept for layout
    u16 sceneId;               ///< Scene ID within game
    u8 _unk2[4];               ///< Unknown, kept for layout
};
static_assert(sizeof(IntentId) == 16);

/**
 * @brief Network identifier
 *
 * Packed to ensure correct IPC layout.
 */
struct __attribute__((packed)) NetworkId {
    IntentId intentId;    ///< 16 bytes
    SessionId sessionId;  ///< 16 bytes
};
static_assert(sizeof(NetworkId) == 32);

// ============================================================================
// Network Structures
// ============================================================================

/**
 * @brief Common network information
 */
struct CommonNetworkInfo {
    /// BSSID of the access point.
    MacAddress bssid;
    /// Network name.
    Ssid ssid;
    /// Wi-Fi channel number.
    s16 channel;
    /// Signal quality level reported to the game.
    s8 linkLevel;
    /// Network type (LDN).
    u8 networkType;
    /// Unknown, layout padding to keep the 48-byte IPC size.
    u32 _unk;
};
static_assert(sizeof(CommonNetworkInfo) == 48);

/**
 * @brief Information about a node (player) in the network
 */
struct NodeInfo {
    /// Virtual LDN IPv4 address of the node (Ryujinx format, 10.114.x.x).
    u32 ipv4Address;
    /// MAC address of the node.
    MacAddress macAddress;
    /// Node index in the network (0 is the host).
    s8 nodeId;
    /// Non-zero while the node is connected.
    s8 isConnected;
    /// Player name, NUL terminated.
    char userName[UserNameBytesMax + 1];
    /// Unknown, layout padding.
    u8 _unk1;
    /// Local communication version advertised by the node.
    s16 localCommunicationVersion;
    /// Unknown, layout padding.
    u8 _unk2[16];
};
static_assert(sizeof(NodeInfo) == 64);

/**
 * @brief LDN-specific network information
 */
struct LdnNetworkInfo {
    /// Unknown random bytes.
    u8 unkRandom[16];
    /// Security mode of the network.
    u16 securityMode;
    /// Whether new stations are accepted (accept policy).
    u8 stationAcceptPolicy;
    /// Unknown, layout padding.
    u8 _unk1[3];
    /// Maximum number of nodes allowed (at most NodeCountMax).
    u8 nodeCountMax;
    /// Number of nodes currently in the network.
    u8 nodeCount;
    /// Node table; the first nodeCount entries are meaningful.
    NodeInfo nodes[NodeCountMax];
    /// Unknown, layout padding.
    u16 _unk2;
    /// Valid bytes in advertiseData.
    u16 advertiseDataSize;
    /// Game-defined advertise payload.
    u8 advertiseData[AdvertiseDataSizeMax];
    /// Unknown, layout padding.
    u8 _unk3[148];
};
static_assert(sizeof(LdnNetworkInfo) == 1072);

/**
 * @brief Complete network information
 *
 * Returned by Scan() and used for Connect().
 */
struct NetworkInfo : public ams::sf::LargeData {
    /// Intent and session identifiers.
    NetworkId networkId;
    /// Fields shared with other Nintendo network types.
    CommonNetworkInfo common;
    /// LDN-specific fields including the node table.
    LdnNetworkInfo ldn;
};
static_assert(sizeof(NetworkInfo) == 0x480);

// ============================================================================
// Configuration Structures
// ============================================================================

/**
 * @brief Security configuration
 */
struct SecurityConfig {
    /// Requested security mode.
    u16 securityMode;
    /// Valid bytes in passphrase.
    u16 passphraseSize;
    /// Passphrase bytes (not NUL terminated).
    u8 passphrase[PassphraseLengthMax];
};
static_assert(sizeof(SecurityConfig) == 68);

/**
 * @brief User configuration (player name)
 */
struct UserConfig {
    /// Player name, NUL terminated.
    char userName[UserNameBytesMax + 1];
    /// Unknown, layout padding.
    u8 _unk[15];
};
static_assert(sizeof(UserConfig) == 48);

/**
 * @brief Network configuration for creating a network
 */
struct NetworkConfig {
    IntentId intentId;  ///< 16 bytes
    /// Requested Wi-Fi channel.
    u16 channel;
    /// Maximum number of nodes requested.
    u8 nodeCountMax;
    /// Unknown, layout padding.
    u8 _unk1;
    /// Local communication version of the application.
    u16 localCommunicationVersion;
    /// Unknown, layout padding.
    u8 _unk2[10];
};
static_assert(sizeof(NetworkConfig) == 32);

/**
 * @brief Combined configuration for CreateNetwork()
 */
struct CreateNetworkConfig {
    /// Security settings.
    SecurityConfig securityConfig;
    /// Host player settings.
    UserConfig userConfig;
    /// Unknown, padding aligning networkConfig to 8 bytes.
    u8 _unk[4];
    /// Network parameters.
    NetworkConfig networkConfig;
};
static_assert(sizeof(CreateNetworkConfig) == 152);

/**
 * @brief Connection data for Connect()
 */
struct ConnectNetworkData {
    /// Security settings.
    SecurityConfig securityConfig;
    /// Joining player settings.
    UserConfig userConfig;
    /// Local communication version of the application.
    u32 localCommunicationVersion;
    /// Connect option flags.
    u32 option;
};
static_assert(sizeof(ConnectNetworkData) == 124);

/**
 * @brief Node update notification
 */
struct NodeLatestUpdate : public ams::sf::PrefersPointerTransferMode {
    /// A NodeStateChange value.
    u8 stateChange;
    /// Unknown, layout padding.
    u8 _unk[7];
};
static_assert(sizeof(NodeLatestUpdate) == 8);

/**
 * @brief Session information for IPC with overlay
 *
 * Contains information about the current LDN session, exposed via ryu:cfg.
 * Matches RyuLdnSessionInfo in overlay/source/ryu_ldn_ipc.h
 */
struct SessionInfo {
    u8 node_count;      ///< Number of players in session
    u8 node_count_max;  ///< Maximum players allowed
    u8 local_node_id;   ///< Our node ID in the session
    u8 is_host;         ///< 1 if we are the host, 0 if client
    u8 reserved[4];     ///< Reserved for future use
};
static_assert(sizeof(SessionInfo) == 8);

/**
 * @brief Security parameters from a network
 */
struct SecurityParameter {
    /// Unknown random bytes.
    u8 unkRandom[16];
    /// Session identifier of the network.
    SessionId sessionId;
};
static_assert(sizeof(SecurityParameter) == 32);

/**
 * @brief Address entry for private networks
 */
struct AddressEntry {
    /// IPv4 address of the node.
    u32 ipv4Address;
    /// MAC address of the node.
    MacAddress macAddress;
    /// Reserved padding.
    u16 reserved;
};
static_assert(sizeof(AddressEntry) == 12);

/**
 * @brief Address list for private networks (up to 8 entries)
 */
struct AddressList {
    /// Entries, one per possible node.
    AddressEntry addresses[8];
};
static_assert(sizeof(AddressList) == 96);

/**
 * @brief Configuration for CreateNetworkPrivate()
 *
 * This structure is passed to CreateNetworkPrivate via IPC.
 * AddressList is passed separately as a buffer.
 * Total size: 0xB8 = 184 bytes (matches Nintendo Switch IPC layout).
 *
 * Packed to prevent compiler from adding extra padding for u64 alignment.
 */
struct __attribute__((packed)) CreateNetworkPrivateConfig {
    SecurityConfig securityConfig;        ///< 0x00: 68 bytes
    SecurityParameter securityParameter;  ///< 0x44: 32 bytes
    UserConfig userConfig;                ///< 0x64: 48 bytes
    u8 _unk[4];  ///< 0x94: 4 bytes padding (align NetworkConfig to 8)
    NetworkConfig networkConfig;  ///< 0x98: 32 bytes
};
static_assert(sizeof(CreateNetworkPrivateConfig) == 184);

/**
 * @brief Connection data for ConnectPrivate()
 *
 * Total size: 0xC0 = 192 bytes (matches Nintendo Switch IPC layout).
 * NetworkConfig requires 8-byte alignment, hence 4 bytes padding at 0x9C.
 *
 * Packed to prevent compiler from adding extra padding for u64 alignment.
 */
struct __attribute__((packed)) ConnectPrivateData {
    SecurityConfig securityConfig;        ///< 0x00: 68 bytes
    SecurityParameter securityParameter;  ///< 0x44: 32 bytes
    UserConfig userConfig;                ///< 0x64: 48 bytes
    u32 localCommunicationVersion;        ///< 0x94: 4 bytes
    u32 option;                           ///< 0x98: 4 bytes
    u8 _pad[4];  ///< 0x9C: 4 bytes padding (align NetworkConfig to 8)
    NetworkConfig networkConfig;  ///< 0xA0: 32 bytes
};
static_assert(sizeof(ConnectPrivateData) == 192);

/**
 * @brief Scan filter configuration
 *
 * Must match Nintendo's exact layout from switchbrew: NetworkId (32 bytes) at
 * 0x00, NetworkType (u32) at 0x20, Bssid (6 bytes) at 0x24, Ssid (34 bytes) at
 * 0x2A, Reserved (16 bytes) at 0x4C and Flag (u32) at 0x5C.
 */
struct __attribute__((packed)) ScanFilter {
    NetworkId networkId;  ///< Network id to match (ScanFilterFlag_NetworkId).
    u32 networkType;      ///< Network type to match.
    MacAddress bssid;     ///< BSSID to match.
    Ssid ssid;            ///< SSID to match.
    u8 unk[16];           ///< Reserved.
    u32 flag;  ///< ScanFilterFlag bits selecting the fields to match.
};
static_assert(sizeof(ScanFilter) == 96);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * @brief Extract NetworkConfig from NetworkInfo
 *
 * @param info Source network info
 * @param out Destination network config
 */
inline void NetworkInfo2NetworkConfig(
    const NetworkInfo* info, NetworkConfig* out) {
    out->intentId = info->networkId.intentId;
    out->channel = static_cast<u16>(info->common.channel);
    out->nodeCountMax = info->ldn.nodeCountMax;
    out->_unk1 = 0;
    out->localCommunicationVersion =
        static_cast<u16>(info->ldn.nodes[0].localCommunicationVersion);
    std::memset(out->_unk2, 0, sizeof(out->_unk2));
}

/**
 * @brief Extract SecurityParameter from NetworkInfo
 *
 * @param info Source network info
 * @param out Destination security parameter
 */
inline void NetworkInfo2SecurityParameter(
    const NetworkInfo* info, SecurityParameter* out) {
    std::memcpy(out->unkRandom, info->ldn.unkRandom, sizeof(out->unkRandom));
    out->sessionId = info->networkId.sessionId;
}

/**
 * @brief Convert CommState to string for logging
 *
 * @param state State to convert
 * @return Human-readable state name
 */
inline const char* CommStateToString(CommState state) {
    switch (state) {
        case CommState::None:
            return "None";
        case CommState::Initialized:
            return "Initialized";
        case CommState::AccessPoint:
            return "AccessPoint";
        case CommState::AccessPointCreated:
            return "AccessPointCreated";
        case CommState::Station:
            return "Station";
        case CommState::StationConnected:
            return "StationConnected";
        case CommState::Error:
            return "Error";
        default:
            return "Unknown";
    }
}

}  // namespace ams::mitm::ldn
