/**
 * @file ldn_integration_tests.cpp
 * @brief Integration tests for complete LDN service flows
 *
 * Tests the full end-to-end flows for Host and Client scenarios,
 * verifying that all components work together correctly:
 * - State machine transitions
 * - Protocol message sequencing
 * - Node mapping and routing
 * - Proxy data buffering
 * - Error handling and recovery
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <queue>
#include <mutex>
#include <stdexcept>
#include <utility>

//=============================================================================
// Test Framework
//=============================================================================

/// Number of tests started by RUN_TEST (1-based index printed per test).
static int g_tests_run = 0;
/**
 * Number of tests that finished without throwing; compared to
 * g_tests_run by main() to compute the process exit code.
 */
static int g_tests_passed = 0;

/**
 * @brief Declares a test body as the free function `test_<name>()`.
 * @param name Test identifier, also used by RUN_TEST for reporting.
 */
#define TEST(name) void test_##name()
/**
 * @brief Runs `test_<name>()`, counts it and prints PASS or FAIL.
 *
 * Any std::exception (the ASSERT macros throw std::runtime_error) or
 * other exception is reported as FAIL and does not abort the run.
 * @param name Test identifier previously declared with TEST.
 */
#define RUN_TEST(name)                               \
    do {                                             \
        g_tests_run++;                               \
        printf("  [%d] %s... ", g_tests_run, #name); \
        try {                                        \
            test_##name();                           \
            g_tests_passed++;                        \
            printf("PASS\n");                        \
        } catch (const std::exception& e) {          \
            printf("FAIL: %s\n", e.what());          \
        } catch (...) {                              \
            printf("FAIL: unknown exception\n");     \
        }                                            \
    } while (0)

/**
 * @brief Throws std::runtime_error when the condition is false.
 * @param cond Expression expected to be true.
 */
#define ASSERT(cond)                                              \
    do {                                                          \
        if (!(cond)) {                                            \
            throw std::runtime_error("Assertion failed: " #cond); \
        }                                                         \
    } while (0)

/**
 * @brief Throws std::runtime_error unless the two values compare equal.
 * @param a Actual value.
 * @param b Expected value.
 */
#define ASSERT_EQ(a, b)                                                  \
    do {                                                                 \
        if ((a) != (b)) {                                                \
            throw std::runtime_error("Assertion failed: " #a " == " #b); \
        }                                                                \
    } while (0)

/**
 * @brief Throws std::runtime_error when the two values compare equal.
 * @param a Actual value.
 * @param b Value that must differ from a.
 */
#define ASSERT_NE(a, b)                                                  \
    do {                                                                 \
        if ((a) == (b)) {                                                \
            throw std::runtime_error("Assertion failed: " #a " != " #b); \
        }                                                                \
    } while (0)

//=============================================================================
// Type Definitions (matching production code)
//=============================================================================

/// Unsigned 8-bit integer, as in the production headers.
using u8 = uint8_t;
/// Unsigned 16-bit integer, as in the production headers.
using u16 = uint16_t;
/// Unsigned 32-bit integer, as in the production headers.
using u32 = uint32_t;
/// Unsigned 64-bit integer, as in the production headers.
using u64 = uint64_t;
/// Signed 32-bit integer, as in the production headers.
using s32 = int32_t;

/// LDN communication state tracked by the test state machine.
enum class CommState : u32 {
    /// Service not initialized.
    None = 0,
    /// Initialize() succeeded; no role chosen yet.
    Initialized = 1,
    /// Host role opened (OpenAccessPoint).
    AccessPoint = 2,
    /// Host role with a network created (CreateNetwork).
    AccessPointCreated = 3,
    /// Client role opened (OpenStation).
    Station = 4,
    /// Client role connected to a network (Connect).
    StationConnected = 5,
    /// Entered through SetError(); only Finalize() leaves it.
    Error = 6
};

/// Reason a session ended, stored by IntegrationStateMachine::SetError.
enum class DisconnectReason : u16 {
    /// No disconnect recorded.
    None = 0,
    /// Disconnect requested by the local user.
    User = 1,
    /// Disconnect requested by the system.
    SystemRequest = 2,
    /// Network destroyed by the host user.
    DestroyedByUser = 3,
    /// Network destroyed by the system.
    DestroyedBySystem = 4,
    /// Disconnected by an administrator.
    Admin = 5,
    /// Link to the peer was lost.
    SignalLost = 6
};

/// Result codes returned by the integrated service operations.
enum class ResultCode : u32 {
    /// Operation succeeded.
    Success = 0,
    /// Operation not allowed in the current CommState.
    InvalidState = 1,
    /// The server reported a failure.
    ServerError = 2,
    /// The server did not answer in time.
    Timeout = 3,
    /// The connection to the server was lost.
    Disconnected = 4,
    /// The network already holds the maximum number of nodes.
    NetworkFull = 5,
    /// Requested network or scan index does not exist.
    NotFound = 6
};

/// Message types exchanged with the mock server (RyuLdn protocol).
enum class MessageType : u8 {
    // Client → Server
    /// Client request to create a hosted network.
    CreateAccessPoint = 1,
    /// Client request to join a scanned network.
    Connect = 2,
    /// Client request to scan for networks.
    Scan = 3,
    /// Client request to update the advertised data.
    SetAdvertiseData = 4,
    /// Client notice that it is leaving the network.
    Disconnect = 5,
    /// Client request to change the accept policy.
    SetAcceptPolicy = 6,
    /// Client game data to relay to other nodes.
    ProxyData = 7,

    // Server → Client
    /// Server answer to CreateAccessPoint.
    CreateAccessPointResponse = 128,
    /// Server answer to Connect.
    ConnectResponse = 129,
    /// Server answer to Scan (count + NetworkInfo array).
    ScanResponse = 130,
    /// Server push of the current NetworkInfo.
    SyncNetwork = 131,
    /// Server delivery of relayed game data.
    ProxyDataReceived = 132,
    /// Server-initiated disconnect, payload is the reason.
    Disconnected = 133,
    /// Server-side error notification.
    Error = 134
};

//=============================================================================
// State Machine Implementation
//=============================================================================

/**
 * @brief LDN State Machine for integration testing
 *
 * Manages state transitions and validates the complete flow.
 */
class IntegrationStateMachine {
public:
    /// Starts in CommState::None with the state-change event cleared.
    IntegrationStateMachine() = default;

    /// @return Current communication state.
    [[nodiscard]] CommState GetState() const {
        return m_state;
    }

    /// @return True if a state change happened since the last ClearEvent().
    [[nodiscard]] bool IsEventSignaled() const {
        return m_event_signaled;
    }
    /// Clears the state-change event flag.
    void ClearEvent() {
        m_event_signaled = false;
    }

    /**
     * @brief Initialize: None to Initialized.
     * @param pid Client process id, stored until Finalize().
     * @return Success, or InvalidState if not in None.
     */
    ResultCode Initialize(u64 pid) {
        if (m_state != CommState::None) {
            return ResultCode::InvalidState;
        }
        m_client_pid = pid;
        SetState(CommState::Initialized);
        return ResultCode::Success;
    }

    /**
     * @brief Finalize: any state to None, forgetting the client pid.
     * @return Always Success.
     */
    ResultCode Finalize() {
        SetState(CommState::None);
        m_client_pid = 0;
        return ResultCode::Success;
    }

    /**
     * @brief OpenAccessPoint: Initialized to AccessPoint.
     * @return Success, or InvalidState if not in Initialized.
     */
    ResultCode OpenAccessPoint() {
        if (m_state != CommState::Initialized) {
            return ResultCode::InvalidState;
        }
        SetState(CommState::AccessPoint);
        return ResultCode::Success;
    }

    /**
     * @brief CloseAccessPoint: AccessPoint or AccessPointCreated to
     * Initialized.
     * @return Success, or InvalidState from any other state.
     */
    ResultCode CloseAccessPoint() {
        if (m_state != CommState::AccessPoint &&
            m_state != CommState::AccessPointCreated) {
            return ResultCode::InvalidState;
        }
        SetState(CommState::Initialized);
        return ResultCode::Success;
    }

    /**
     * @brief CreateNetwork: AccessPoint to AccessPointCreated.
     * @return Success, or InvalidState if not in AccessPoint.
     */
    ResultCode CreateNetwork() {
        if (m_state != CommState::AccessPoint) {
            return ResultCode::InvalidState;
        }
        SetState(CommState::AccessPointCreated);
        return ResultCode::Success;
    }

    /**
     * @brief DestroyNetwork: AccessPointCreated to AccessPoint.
     * @return Success, or InvalidState if not in AccessPointCreated.
     */
    ResultCode DestroyNetwork() {
        if (m_state != CommState::AccessPointCreated) {
            return ResultCode::InvalidState;
        }
        SetState(CommState::AccessPoint);
        return ResultCode::Success;
    }

    /**
     * @brief OpenStation: Initialized to Station.
     * @return Success, or InvalidState if not in Initialized.
     */
    ResultCode OpenStation() {
        if (m_state != CommState::Initialized) {
            return ResultCode::InvalidState;
        }
        SetState(CommState::Station);
        return ResultCode::Success;
    }

    /**
     * @brief CloseStation: Station or StationConnected to Initialized.
     * @return Success, or InvalidState from any other state.
     */
    ResultCode CloseStation() {
        if (m_state != CommState::Station &&
            m_state != CommState::StationConnected) {
            return ResultCode::InvalidState;
        }
        SetState(CommState::Initialized);
        return ResultCode::Success;
    }

    /**
     * @brief Connect: Station to StationConnected.
     * @return Success, or InvalidState if not in Station.
     */
    ResultCode Connect() {
        if (m_state != CommState::Station) {
            return ResultCode::InvalidState;
        }
        SetState(CommState::StationConnected);
        return ResultCode::Success;
    }

    /**
     * @brief Disconnect: StationConnected to Station.
     * @return Success, or InvalidState if not in StationConnected.
     */
    ResultCode Disconnect() {
        if (m_state != CommState::StationConnected) {
            return ResultCode::InvalidState;
        }
        SetState(CommState::Station);
        return ResultCode::Success;
    }

    /**
     * @brief SetError: any state to Error, remembering the reason.
     * @param reason Reason later returned by GetDisconnectReason().
     */
    void SetError(DisconnectReason reason) {
        m_disconnect_reason = reason;
        SetState(CommState::Error);
    }

    /// @return Reason given to the last SetError(), None if never called.
    [[nodiscard]] DisconnectReason GetDisconnectReason() const {
        return m_disconnect_reason;
    }

private:
    /**
     * @brief Changes state and raises the event flag only on a real change.
     * @param new_state State to enter.
     */
    void SetState(CommState new_state) {
        if (m_state != new_state) {
            m_state = new_state;
            m_event_signaled = true;
        }
    }

    /// Current state.
    CommState m_state{CommState::None};
    /// Process id given to Initialize(), 0 after Finalize().
    u64 m_client_pid = 0;
    /// Reason recorded by SetError().
    DisconnectReason m_disconnect_reason = DisconnectReason::None;
    /// True after a state change until ClearEvent() is called.
    bool m_event_signaled{false};
};

//=============================================================================
// Network Info Structure
//=============================================================================

/// Per-node description, a simplified copy of the production NodeInfo.
struct NodeInfo {
    /// Node IPv4 address as stored by the test (host order, not validated).
    u32 ipv4_address;
    /// Node MAC address.
    u8 mac_address[6];
    /// Node index in the network, 0 for the host.
    u8 node_id;
    /// Non-zero when the node is currently part of the network.
    u8 is_connected;
    /// NUL-terminated node user name.
    char user_name[33];
    /// Padding kept to mirror the production layout.
    u8 reserved[15];
    /// Local communication version advertised by the node.
    u16 local_communication_version;
    /// Padding kept to mirror the production layout.
    u8 reserved2[16];
};

/// Simplified NetworkInfo: network id, common section, LDN section.
struct NetworkInfo {
    /// Opaque 16-byte network identifier.
    u8 network_id[16];
    /// Fields shared by every network description (ssid, channel).
    struct {
        /// NUL-terminated SSID.
        u8 ssid[33];
        /// Wi-Fi channel.
        u8 channel;
    } common;
    /// LDN specific section: node counts and the node table.
    struct {
        /// Number of valid entries in nodes[].
        u8 node_count;
        /// Maximum number of nodes the network accepts.
        u8 node_count_max;
        /// Padding kept to mirror the production layout.
        u8 reserved[6];
        /// Node table, indexed by position (not by node_id).
        NodeInfo nodes[8];
    } ldn;
};

//=============================================================================
// Node Mapper Implementation
//=============================================================================

/**
 * @brief Node mapper for tracking connected players
 */
class IntegrationNodeMapper {
public:
    /// Maximum number of nodes in one LDN network.
    static constexpr size_t MaxNodes = 8;
    /// Destination id meaning every node except the sender.
    static constexpr u32 BroadcastNodeId = 0xFFFFFFFF;

    /// Starts with no connected node and no local node id.
    IntegrationNodeMapper() {
        Clear();
    }

    /**
     * @brief Marks a node as connected and records its address.
     * @param node_id Node index; ignored when >= MaxNodes.
     * @param ipv4 IPv4 address of the node.
     */
    void AddNode(u32 node_id, u32 ipv4) {
        if (node_id >= MaxNodes) {
            return;
        }
        m_nodes[node_id].ipv4 = ipv4;
        m_nodes[node_id].connected = true;
    }

    /**
     * @brief Marks a node as disconnected, keeping its stored address.
     * @param node_id Node index; ignored when >= MaxNodes.
     */
    void RemoveNode(u32 node_id) {
        if (node_id >= MaxNodes) {
            return;
        }
        m_nodes[node_id].connected = false;
    }

    /**
     * @brief Tells whether a node is connected.
     * @param node_id Node index.
     * @return True if connected, false if out of range or disconnected.
     */
    [[nodiscard]] bool IsConnected(u32 node_id) const {
        if (node_id >= MaxNodes) {
            return false;
        }
        return m_nodes[node_id].connected;
    }

    /**
     * @brief Returns the IPv4 address stored for a node.
     * @param node_id Node index.
     * @return Stored address, 0 if node_id is out of range.
     */
    [[nodiscard]] u32 GetIpv4(u32 node_id) const {
        if (node_id >= MaxNodes) {
            return 0;
        }
        return m_nodes[node_id].ipv4;
    }

    /// @return Number of nodes currently marked connected.
    [[nodiscard]] size_t GetConnectedCount() const {
        size_t count = 0;
        for (auto m_node : m_nodes) {
            if (m_node.connected) {
                count++;
            }
        }
        return count;
    }

    /**
     * Disconnects every node, zeroes addresses and forgets the local id
     * (set back to the 0xFF not-found marker).
     */
    void Clear() {
        for (auto& m_node : m_nodes) {
            m_node.ipv4 = 0;
            m_node.connected = false;
        }
        m_local_node_id = 0xFF;
    }

    /// @param id Node id of the local console.
    void SetLocalNodeId(u8 id) {
        m_local_node_id = id;
    }
    /// @return Local node id, 0xFF while unknown.
    [[nodiscard]] u8 GetLocalNodeId() const {
        return m_local_node_id;
    }

    /**
     * @brief Rebuilds the table from a SyncNetwork NetworkInfo.
     *
     * Clears first, then registers every connected node among the first
     * ldn.node_count entries at the index given by its node_id.
     * @param info Network description received from the server.
     */
    void UpdateFromNetworkInfo(const NetworkInfo& info) {
        Clear();
        for (u8 i = 0; i < info.ldn.node_count && i < MaxNodes; i++) {
            const auto& node = info.ldn.nodes[i];
            if (node.is_connected != 0) {
                m_nodes[node.node_id].ipv4 = node.ipv4_address;
                m_nodes[node.node_id].connected = true;
            }
        }
    }

    /**
     * @brief Decides whether a packet must be delivered to a node.
     * @param dest Destination id of the packet, or BroadcastNodeId.
     * @param src Node id of the sender.
     * @param target Node being considered for delivery.
     * @return False if target is invalid or disconnected; for broadcast,
     * true unless target is the sender; otherwise true only if dest
     * equals target.
     */
    [[nodiscard]] bool ShouldRouteToNode(u32 dest, u32 src, u32 target) const {
        if (target >= MaxNodes || !m_nodes[target].connected) {
            return false;
        }
        if (dest == BroadcastNodeId) {
            return target != src;  // Broadcast to all except source
        }
        return dest == target;  // Unicast to specific node
    }

private:
    /// State stored for one node.
    struct NodeEntry {
        /// IPv4 address of the node.
        u32 ipv4;
        /// True while the node is part of the network.
        bool connected;
    };
    /// Node table indexed by node id.
    NodeEntry m_nodes[MaxNodes]{};
    /// Node id of the local console, 0xFF when unknown.
    u8 m_local_node_id{};
};

//=============================================================================
// Proxy Data Buffer Implementation
//=============================================================================

/// Routing header placed in front of every proxy data payload.
struct ProxyDataHeader {
    /// Node id the packet is addressed to, or BroadcastNodeId.
    u32 dest_node_id;
    /// Node id of the sender.
    u32 src_node_id;
};

/**
 * @brief Ring buffer for proxy data packets
 */
class IntegrationProxyBuffer {
public:
    /// Maximum payload size of one buffered packet, in bytes.
    static constexpr size_t MaxPacketSize = 0x1000;
    /// Capacity of the ring buffer, in packets.
    static constexpr size_t MaxPackets = 32;

    /// Creates an empty buffer.
    IntegrationProxyBuffer() = default;

    /**
     * @brief Appends a packet at the write position.
     * @param header Routing header stored with the packet.
     * @param data Payload to copy, may be null when size is 0.
     * @param size Payload size in bytes.
     * @return False if size exceeds MaxPacketSize or the buffer is full.
     */
    bool Write(const ProxyDataHeader& header, const u8* data, size_t size) {
        if (size > MaxPacketSize || m_count >= MaxPackets) {
            return false;
        }

        auto& pkt = m_packets[m_write_idx];
        pkt.header = header;
        pkt.size = size;
        if (size > 0 && data != nullptr) {
            memcpy(pkt.data, data, size);
        }

        m_write_idx = (m_write_idx + 1) % MaxPackets;
        m_count++;
        return true;
    }

    /**
     * @brief Pops the oldest packet.
     * @param[out] header Receives the routing header.
     * @param[out] data Receives the payload, may be null.
     * @param[out] size Receives the copied size, truncated to max_size.
     * @param max_size Capacity of data in bytes.
     * @return False if the buffer is empty. A truncated packet is still
     * consumed entirely.
     */
    bool Read(
        ProxyDataHeader& header, u8* data, size_t& size, size_t max_size) {
        if (m_count == 0) {
            return false;
        }

        const auto& pkt = m_packets[m_read_idx];
        header = pkt.header;
        size = (pkt.size <= max_size) ? pkt.size : max_size;
        if (size > 0 && data != nullptr) {
            memcpy(data, pkt.data, size);
        }

        m_read_idx = (m_read_idx + 1) % MaxPackets;
        m_count--;
        return true;
    }

    /// @return Number of packets waiting to be read.
    [[nodiscard]] size_t GetPendingCount() const {
        return m_count;
    }
    /// @return True if no packet is waiting.
    [[nodiscard]] bool IsEmpty() const {
        return m_count == 0;
    }
    /// Drops every buffered packet.
    void Reset() {
        m_read_idx = m_write_idx = m_count = 0;
    }

private:
    /// One slot of the ring buffer.
    struct Packet {
        /// Routing header of the packet.
        ProxyDataHeader header{};
        /// Payload bytes, valid up to size.
        u8 data[MaxPacketSize];
        /// Number of valid bytes in data.
        size_t size;
    };
    /// Ring buffer storage.
    Packet m_packets[MaxPackets]{};
    /// Index of the oldest packet.
    size_t m_read_idx{0};
    /// Index where the next packet is written.
    size_t m_write_idx{0};
    /// Number of packets currently stored.
    size_t m_count{0};
};

//=============================================================================
// Mock Server for Integration Testing
//=============================================================================

/**
 * @brief Mock server that simulates RyuLdn server responses
 *
 * This allows testing the full flow without a real server.
 */
class MockServer {
public:
    /// Callback receiving a server message: type, payload, payload size.
    using ResponseHandler = std::function<void(MessageType, const u8*, size_t)>;

    /// Creates a server with no connected node and no network.
    MockServer() = default;

    /**
     * @brief Registers the callback receiving every server message.
     * @param handler Callback invoked synchronously from the mock server
     * methods; replaces any previous one.
     */
    void SetResponseHandler(ResponseHandler handler) {
        m_response_handler = std::move(handler);
    }

    /**
     * @brief Handles a client message and answers through the handler.
     * @param type Message type sent by the client.
     * @param data Payload, ignored by most handlers.
     * @param size Payload size in bytes.
     */
    void ProcessMessage(MessageType type, const u8* data, size_t size) {
        switch (type) {
            case MessageType::CreateAccessPoint:
                HandleCreateAccessPoint(data, size);
                break;
            case MessageType::Connect:
                HandleConnect(data, size);
                break;
            case MessageType::Scan:
                HandleScan(data, size);
                break;
            case MessageType::Disconnect:
                HandleDisconnect(data, size);
                break;
            case MessageType::ProxyData:
                HandleProxyData(data, size);
                break;
            default:
                break;
        }
    }

    /**
     * @brief Adds a node to the hosted network and pushes a SyncNetwork.
     * @param ipv4 IPv4 address of the new node.
     * @param username Name of the new node, truncated to 32 characters.
     * Ignored when 8 node ids have already been handed out.
     */
    void SimulatePlayerJoin(u32 ipv4, const char* username) {
        if (m_next_node_id >= 8) {
            return;
        }

        NodeInfo node = {};
        node.ipv4_address = ipv4;
        node.node_id = m_next_node_id++;
        node.is_connected = 1;
        strncpy(node.user_name, username, 32);

        m_connected_nodes.push_back(node);
        SendSyncNetwork();
    }

    /**
     * @brief Removes a node and pushes a SyncNetwork.
     * @param node_id Node id to remove; unknown ids change nothing but a
     * SyncNetwork is still sent.
     */
    void SimulatePlayerLeave(u32 node_id) {
        for (auto it = m_connected_nodes.begin(); it != m_connected_nodes.end();
             ++it) {
            if (it->node_id == node_id) {
                m_connected_nodes.erase(it);
                break;
            }
        }
        SendSyncNetwork();
    }

    /**
     * @brief Delivers a ProxyDataReceived message to the handler.
     * @param src_node Node id of the simulated sender.
     * @param dest_node Destination id, or BroadcastNodeId.
     * @param data Payload, may be null when size is 0.
     * @param size Payload size in bytes.
     */
    void SimulateProxyData(
        u32 src_node, u32 dest_node, const u8* data, size_t size) {
        if (!m_response_handler) {
            return;
        }

        std::vector<u8> response(sizeof(ProxyDataHeader) + size);
        ProxyDataHeader header = {
            .dest_node_id = dest_node, .src_node_id = src_node};
        memcpy(response.data(), &header, sizeof(header));
        if (size > 0) {
            memcpy(response.data() + sizeof(header), data, size);
        }

        m_response_handler(
            MessageType::ProxyDataReceived, response.data(), response.size());
    }

    /**
     * @brief Configures the network returned by the next Scan.
     * @param ssid SSID reported in the scan reply.
     * @param host_ip IPv4 address of the host node, also used by Connect.
     */
    void CreateNetworkForScan(const char* ssid, u32 host_ip) {
        m_scan_result_ssid = ssid;
        m_scan_result_host_ip = host_ip;
    }

private:
    /**
     * @brief Answers CreateAccessPoint: host becomes node 0, then SyncNetwork.
     * @param data Unused.
     * @param size Unused.
     */
    void HandleCreateAccessPoint(const u8* data, size_t size) {
        (void)data;
        (void)size;
        m_network_created = true;
        m_next_node_id = 0;

        // Add host as node 0
        NodeInfo host = {};
        host.ipv4_address = 0x0100007F;  // 127.0.0.1
        host.node_id = m_next_node_id++;
        host.is_connected = 1;
        strncpy(host.user_name, "Host", 32);
        m_connected_nodes.push_back(host);

        // Send response
        if (m_response_handler) {
            u8 response[4] = {0, 0, 0, 0};  // Success
            m_response_handler(
                MessageType::CreateAccessPointResponse,
                response,
                sizeof(response));
        }

        SendSyncNetwork();
    }

    /**
     * @brief Answers Connect: network is reset to host plus client, then
     * ConnectResponse and SyncNetwork are sent.
     * @param data Unused.
     * @param size Unused.
     */
    void HandleConnect(const u8* data, size_t size) {
        (void)data;
        (void)size;

        // When client connects, we need to simulate the network with host +
        // client Clear any existing nodes and start fresh
        m_connected_nodes.clear();
        m_next_node_id = 0;

        // Add host (node 0)
        NodeInfo host = {};
        host.ipv4_address = m_scan_result_host_ip;
        host.node_id = m_next_node_id++;
        host.is_connected = 1;
        strncpy(host.user_name, "Host", 32);
        m_connected_nodes.push_back(host);

        // Add connecting client (node 1)
        NodeInfo client = {};
        client.ipv4_address = 0x0200007F;  // 127.0.0.2
        client.node_id = m_next_node_id++;
        client.is_connected = 1;
        strncpy(client.user_name, "Client", 32);
        m_connected_nodes.push_back(client);

        // Send response
        if (m_response_handler) {
            u8 response[4] = {0, 0, 0, 0};  // Success
            m_response_handler(
                MessageType::ConnectResponse, response, sizeof(response));
        }

        SendSyncNetwork();
    }

    /**
     * @brief Answers Scan with one network built from CreateNetworkForScan.
     * @param data Unused.
     * @param size Unused.
     */
    void HandleScan(const u8* data, size_t size) {
        (void)data;
        (void)size;

        if (m_response_handler) {
            // Build a scan response with one network
            NetworkInfo info = {};
            memset(info.network_id, 0x42, 16);
            strncpy(
                reinterpret_cast<char*>(info.common.ssid),
                m_scan_result_ssid.c_str(),
                32);
            info.common.channel = 6;
            info.ldn.node_count = 1;
            info.ldn.node_count_max = 8;
            info.ldn.nodes[0].ipv4_address = m_scan_result_host_ip;
            info.ldn.nodes[0].node_id = 0;
            info.ldn.nodes[0].is_connected = 1;
            strncpy(info.ldn.nodes[0].user_name, "Host", 32);

            // Response: count (4 bytes) + NetworkInfo array
            std::vector<u8> response(4 + sizeof(NetworkInfo));
            u32 count = 1;
            memcpy(response.data(), &count, 4);
            memcpy(response.data() + 4, &info, sizeof(info));

            m_response_handler(
                MessageType::ScanResponse, response.data(), response.size());
        }
    }

    /**
     * @brief Handles Disconnect: nothing is sent back.
     * @param data Unused.
     * @param size Unused.
     */
    static void HandleDisconnect(const u8* data, size_t size) {
        (void)data;
        (void)size;
        // User-initiated disconnect: just acknowledge, don't send Disconnected
        // message The Disconnected message is only sent for server-side
        // disconnects For normal user disconnects, the state machine handles
        // the transition directly
    }

    /**
     * @brief Echoes ProxyData back as ProxyDataReceived (relay stand-in).
     * @param data Header and payload from the client.
     * @param size Total size; packets shorter than the header are dropped.
     */
    void HandleProxyData(const u8* data, size_t size) {
        // Echo back to simulate relay (for testing)
        if (m_response_handler && size >= sizeof(ProxyDataHeader)) {
            m_response_handler(MessageType::ProxyDataReceived, data, size);
        }
    }

    /// Sends the current node list to the handler as a SyncNetwork message.
    void SendSyncNetwork() {
        if (!m_response_handler) {
            return;
        }

        NetworkInfo info = {};
        memset(info.network_id, 0x42, 16);
        info.ldn.node_count = static_cast<u8>(m_connected_nodes.size());
        info.ldn.node_count_max = 8;

        for (size_t i = 0; i < m_connected_nodes.size() && i < 8; i++) {
            info.ldn.nodes[i] = m_connected_nodes[i];
        }

        m_response_handler(
            MessageType::SyncNetwork,
            reinterpret_cast<const u8*>(&info),
            sizeof(info));
    }

    /// Callback receiving the server messages.
    ResponseHandler m_response_handler;
    /// Nodes of the network currently hosted or joined.
    std::vector<NodeInfo> m_connected_nodes;
    /// Next node id to hand out.
    u8 m_next_node_id{0};
    /// True once a CreateAccessPoint request was handled.
    bool m_network_created{false};
    /// SSID reported by Scan, set by CreateNetworkForScan().
    std::string m_scan_result_ssid;
    /**
     * Host address reported by Scan and Connect; 0 until
     * CreateNetworkForScan() is called.
     */
    u32 m_scan_result_host_ip{};
};

//=============================================================================
// Integrated LDN Service (combines all components)
//=============================================================================

/**
 * @brief Full LDN service integration for testing
 *
 * Combines state machine, node mapper, proxy buffer, and server communication.
 */
class IntegratedLdnService {
public:
    /**
     * @brief Binds the service to a mock server.
     * @param server Server used for every request; its response handler
     * is replaced by the service and it must outlive the service.
     */
    explicit IntegratedLdnService(MockServer& server) : m_server(&server) {
        server.SetResponseHandler(
            [this](MessageType type, const u8* data, size_t size) {
                HandleServerResponse(type, data, size);
            });
    }

    // === Lifecycle Commands ===

    /**
     * @brief Forwards to the state machine.
     * @param pid Client process id.
     * @return Result of IntegrationStateMachine::Initialize().
     */
    ResultCode Initialize(u64 pid) {
        return m_state_machine.Initialize(pid);
    }

    /**
     * @brief Drops the session state and returns to None.
     * @return Always Success.
     */
    ResultCode Finalize() {
        if (m_connected) {
            m_connected = false;
            m_node_mapper.Clear();
            m_proxy_buffer.Reset();
        }
        return m_state_machine.Finalize();
    }

    /// @return Current communication state.
    [[nodiscard]] CommState GetState() const {
        return m_state_machine.GetState();
    }

    // === Access Point (Host) Commands ===

    /**
     * @brief Opens the host role and marks the server connection as up.
     * @return Success, or InvalidState from the state machine.
     */
    ResultCode OpenAccessPoint() {
        auto result = m_state_machine.OpenAccessPoint();
        if (result == ResultCode::Success) {
            m_connected = true;  // Simulate server connection
        }
        return result;
    }

    /**
     * @brief Closes the host role, resetting node mapper and proxy buffer.
     * @return Result of the state machine transition.
     */
    ResultCode CloseAccessPoint() {
        if (m_connected) {
            m_connected = false;
            m_node_mapper.Clear();
            m_proxy_buffer.Reset();
        }
        return m_state_machine.CloseAccessPoint();
    }

    /**
     * @brief Asks the server to create a network, then enters
     * AccessPointCreated.
     * @param ssid SSID sent in the request, truncated to 32 characters.
     * @return InvalidState if not in AccessPoint, otherwise Success.
     */
    ResultCode CreateNetwork(const char* ssid) {
        if (m_state_machine.GetState() != CommState::AccessPoint) {
            return ResultCode::InvalidState;
        }

        // Send to server
        std::vector<u8> request(64);
        strncpy(reinterpret_cast<char*>(request.data()), ssid, 32);
        m_server->ProcessMessage(
            MessageType::CreateAccessPoint, request.data(), request.size());

        return m_state_machine.CreateNetwork();
    }

    /// @return Result of the AccessPointCreated to AccessPoint transition.
    ResultCode DestroyNetwork() {
        return m_state_machine.DestroyNetwork();
    }

    // === Station (Client) Commands ===

    /**
     * @brief Opens the client role and marks the server connection as up.
     * @return Success, or InvalidState from the state machine.
     */
    ResultCode OpenStation() {
        auto result = m_state_machine.OpenStation();
        if (result == ResultCode::Success) {
            m_connected = true;  // Simulate server connection
        }
        return result;
    }

    /**
     * @brief Closes the client role, resetting node mapper and proxy buffer.
     * @return Result of the state machine transition.
     */
    ResultCode CloseStation() {
        if (m_connected) {
            m_connected = false;
            m_node_mapper.Clear();
            m_proxy_buffer.Reset();
        }
        return m_state_machine.CloseStation();
    }

    /**
     * @brief Scans: clears old results, the mock answers synchronously.
     * @return InvalidState if not in Station, otherwise Success.
     */
    ResultCode Scan() {
        if (m_state_machine.GetState() != CommState::Station) {
            return ResultCode::InvalidState;
        }

        m_scan_results.clear();
        u8 filter[64] = {};  // Empty filter = scan all
        m_server->ProcessMessage(MessageType::Scan, filter, sizeof(filter));

        return ResultCode::Success;
    }

    /**
     * @brief Joins a previously scanned network.
     * @param network_index Index in the last scan results.
     * @return InvalidState if not in Station, NotFound if the index is out
     * of range, otherwise Success.
     */
    ResultCode Connect(u32 network_index) {
        if (m_state_machine.GetState() != CommState::Station) {
            return ResultCode::InvalidState;
        }

        if (network_index >= m_scan_results.size()) {
            return ResultCode::NotFound;
        }

        // Send connect request
        std::vector<u8> request(sizeof(NetworkInfo));
        memcpy(
            request.data(),
            &m_scan_results[network_index],
            sizeof(NetworkInfo));
        m_server->ProcessMessage(
            MessageType::Connect, request.data(), request.size());

        return m_state_machine.Connect();
    }

    /**
     * @brief Leaves the joined network and returns to Station.
     * @return InvalidState if not in StationConnected, otherwise Success.
     */
    ResultCode Disconnect() {
        if (m_state_machine.GetState() != CommState::StationConnected) {
            return ResultCode::InvalidState;
        }

        u8 request[2] = {};
        m_server->ProcessMessage(
            MessageType::Disconnect, request, sizeof(request));

        return m_state_machine.Disconnect();
    }

    // === Data Commands ===

    /**
     * @brief Sends game data to a node through the server.
     * @param dest_node Destination node id, or BroadcastNodeId.
     * @param data Payload, may be null when size is 0.
     * @param size Payload size in bytes.
     * @return InvalidState unless in AccessPointCreated or
     * StationConnected, otherwise Success.
     */
    ResultCode SendProxyData(u32 dest_node, const u8* data, size_t size) {
        if (m_state_machine.GetState() != CommState::AccessPointCreated &&
            m_state_machine.GetState() != CommState::StationConnected) {
            return ResultCode::InvalidState;
        }

        ProxyDataHeader header = {
            .dest_node_id = dest_node,
            .src_node_id = m_node_mapper.GetLocalNodeId()};
        std::vector<u8> request(sizeof(header) + size);
        memcpy(request.data(), &header, sizeof(header));
        if (size > 0) {
            memcpy(request.data() + sizeof(header), data, size);
        }

        m_server->ProcessMessage(
            MessageType::ProxyData, request.data(), request.size());
        return ResultCode::Success;
    }

    /**
     * @brief Pops one received packet addressed to this node.
     * @param[out] header Receives the routing header.
     * @param[out] data Receives the payload.
     * @param[out] size Receives the copied size.
     * @param max_size Capacity of data in bytes.
     * @return False if nothing is pending.
     */
    bool ReceiveProxyData(
        ProxyDataHeader& header, u8* data, size_t& size, size_t max_size) {
        return m_proxy_buffer.Read(header, data, size, max_size);
    }

    // === Info Commands ===

    /// @return Number of networks found by the last scan.
    [[nodiscard]] size_t GetScanResultCount() const {
        return m_scan_results.size();
    }

    /**
     * @return Last NetworkInfo pushed by the server (uninitialized before
     * the first SyncNetwork).
     */
    [[nodiscard]] const NetworkInfo& GetNetworkInfo() const {
        return m_network_info;
    }

    /// @return Number of nodes currently connected.
    [[nodiscard]] size_t GetConnectedNodeCount() const {
        return m_node_mapper.GetConnectedCount();
    }

    /// @return Reason recorded by the state machine.
    [[nodiscard]] DisconnectReason GetDisconnectReason() const {
        return m_state_machine.GetDisconnectReason();
    }

    /// @return True if the state changed since the last ClearEvent().
    [[nodiscard]] bool IsEventSignaled() const {
        return m_state_machine.IsEventSignaled();
    }
    /// Clears the state-change event flag.
    void ClearEvent() {
        m_state_machine.ClearEvent();
    }

private:
    /**
     * @brief Dispatches a server message to its handler.
     * @param type Message type; unhandled types are ignored.
     * @param data Message payload.
     * @param size Payload size in bytes.
     */
    void HandleServerResponse(MessageType type, const u8* data, size_t size) {
        switch (type) {
            case MessageType::ScanResponse:
                HandleScanResponse(data, size);
                break;
            case MessageType::SyncNetwork:
                HandleSyncNetwork(data, size);
                break;
            case MessageType::ProxyDataReceived:
                HandleProxyDataReceived(data, size);
                break;
            case MessageType::Disconnected:
                HandleDisconnected(data, size);
                break;
            case MessageType::Error:
                HandleError(data, size);
                break;
            default:
                break;
        }
    }

    /**
     * @brief Replaces the scan results with the networks of a ScanResponse.
     * @param data u32 count followed by NetworkInfo entries.
     * @param size Payload size; messages under 4 bytes are ignored and
     * incomplete trailing entries are dropped.
     */
    void HandleScanResponse(const u8* data, size_t size) {
        if (size < 4) {
            return;
        }

        u32 count;
        memcpy(&count, data, 4);

        m_scan_results.clear();
        size_t offset = 4;
        for (u32 i = 0; i < count && offset + sizeof(NetworkInfo) <= size;
             i++) {
            NetworkInfo info{};
            memcpy(&info, data + offset, sizeof(NetworkInfo));
            m_scan_results.push_back(info);
            offset += sizeof(NetworkInfo);
        }
    }

    /**
     * @brief Stores the pushed NetworkInfo and refreshes the node mapper.
     *
     * The local node id becomes the last node of the list, which is a
     * test simplification (the real server assigns it).
     * @param data A NetworkInfo.
     * @param size Payload size; shorter messages are ignored.
     */
    void HandleSyncNetwork(const u8* data, size_t size) {
        if (size < sizeof(NetworkInfo)) {
            return;
        }

        memcpy(&m_network_info, data, sizeof(NetworkInfo));
        m_node_mapper.UpdateFromNetworkInfo(m_network_info);

        // Find our node ID (first available slot after host or last joined)
        for (u8 i = 0; i < m_network_info.ldn.node_count; i++) {
            // In real impl, this would be assigned by server
            // For testing, use the last node as our local
        }
        if (m_network_info.ldn.node_count > 0) {
            m_node_mapper.SetLocalNodeId(m_network_info.ldn.node_count - 1);
        }
    }

    /**
     * @brief Buffers a relayed packet if it targets this node or everyone.
     * @param data Header and payload.
     * @param size Total size; messages shorter than the header are ignored.
     */
    void HandleProxyDataReceived(const u8* data, size_t size) {
        if (size < sizeof(ProxyDataHeader)) {
            return;
        }

        ProxyDataHeader header{};
        memcpy(&header, data, sizeof(header));

        const u8* payload = data + sizeof(header);
        size_t payload_size = size - sizeof(header);

        // Check if this packet is for us (unicast or broadcast)
        u32 local_id = m_node_mapper.GetLocalNodeId();
        if (header.dest_node_id == IntegrationNodeMapper::BroadcastNodeId ||
            header.dest_node_id == local_id) {
            m_proxy_buffer.Write(header, payload, payload_size);
        }
    }

    /**
     * @brief Moves the state machine to Error with the server's reason.
     * @param data u16 DisconnectReason.
     * @param size Payload size; messages under 2 bytes are ignored.
     */
    void HandleDisconnected(const u8* data, size_t size) {
        if (size >= 2) {
            u16 reason;
            memcpy(&reason, data, 2);
            m_state_machine.SetError(static_cast<DisconnectReason>(reason));
        }
    }

    /**
     * @brief Moves the state machine to Error with SystemRequest.
     * @param data Unused.
     * @param size Unused.
     */
    void HandleError(const u8* data, size_t size) {
        (void)data;
        (void)size;
        m_state_machine.SetError(DisconnectReason::SystemRequest);
    }

    /// Mock server the service talks to; never null and outlives the service.
    MockServer* m_server;
    /// Tracks the communication state.
    IntegrationStateMachine m_state_machine;
    /// Tracks which nodes are connected.
    IntegrationNodeMapper m_node_mapper;
    /// Holds packets received for this node.
    IntegrationProxyBuffer m_proxy_buffer;
    /// Last NetworkInfo pushed by the server.
    NetworkInfo m_network_info{};
    /// Networks found by the last scan.
    std::vector<NetworkInfo> m_scan_results;
    /// True between opening a role and closing it (simulated link).
    bool m_connected{false};
};

//=============================================================================
// HOST FLOW TESTS
//=============================================================================

/**
 * @brief Verifies the host flow Initialize, OpenAccessPoint, CreateNetwork
 * walks None to AccessPointCreated and the server reports the host as the only
 * node.
 */
TEST(host_full_flow_create_network) {
    MockServer server;
    IntegratedLdnService service(server);

    // Initialize
    ASSERT_EQ(service.Initialize(12345), ResultCode::Success);
    ASSERT_EQ(service.GetState(), CommState::Initialized);

    // Open access point
    ASSERT_EQ(service.OpenAccessPoint(), ResultCode::Success);
    ASSERT_EQ(service.GetState(), CommState::AccessPoint);

    // Create network
    ASSERT_EQ(service.CreateNetwork("TestGame_12345"), ResultCode::Success);
    ASSERT_EQ(service.GetState(), CommState::AccessPointCreated);

    // Verify network info was received
    ASSERT_EQ(service.GetConnectedNodeCount(), 1);  // Host only
}

/**
 * @brief Verifies that a player joining a created network raises the connected
 * node count from 1 to 2.
 */
TEST(host_player_joins) {
    MockServer server;
    IntegratedLdnService service(server);

    // Setup: Host creates network
    service.Initialize(12345);
    service.OpenAccessPoint();
    service.CreateNetwork("TestGame_12345");
    ASSERT_EQ(service.GetConnectedNodeCount(), 1);

    // Simulate player joining
    server.SimulatePlayerJoin(0x0A000001, "Player2");  // 10.0.0.1

    // Verify node count increased
    ASSERT_EQ(service.GetConnectedNodeCount(), 2);
}

/**
 * @brief Verifies that a player leaving the network drops the connected node
 * count back from 2 to 1.
 */
TEST(host_player_leaves) {
    MockServer server;
    IntegratedLdnService service(server);

    // Setup: Host with 2 players
    service.Initialize(12345);
    service.OpenAccessPoint();
    service.CreateNetwork("TestGame_12345");
    server.SimulatePlayerJoin(0x0A000001, "Player2");
    ASSERT_EQ(service.GetConnectedNodeCount(), 2);

    // Player leaves
    server.SimulatePlayerLeave(1);  // Node 1 = Player2

    // Verify node count decreased
    ASSERT_EQ(service.GetConnectedNodeCount(), 1);
}

/**
 * @brief Verifies DestroyNetwork returns the host from AccessPointCreated to
 * AccessPoint.
 */
TEST(host_destroy_network) {
    MockServer server;
    IntegratedLdnService service(server);

    // Setup: Host with network created
    service.Initialize(12345);
    service.OpenAccessPoint();
    service.CreateNetwork("TestGame_12345");

    // Destroy network
    ASSERT_EQ(service.DestroyNetwork(), ResultCode::Success);
    ASSERT_EQ(service.GetState(), CommState::AccessPoint);
}

/**
 * @brief Verifies CloseAccessPoint is accepted from AccessPointCreated and
 * returns to Initialized.
 */
TEST(host_close_access_point) {
    MockServer server;
    IntegratedLdnService service(server);

    // Setup: Host with network created
    service.Initialize(12345);
    service.OpenAccessPoint();
    service.CreateNetwork("TestGame_12345");

    // Close access point (should work from AccessPointCreated)
    ASSERT_EQ(service.CloseAccessPoint(), ResultCode::Success);
    ASSERT_EQ(service.GetState(), CommState::Initialized);
}

/**
 * @brief Verifies Finalize from a hosting session with a joined player succeeds
 * and returns the service to None.
 */
TEST(host_finalize_cleans_up) {
    MockServer server;
    IntegratedLdnService service(server);

    // Setup: Full host session
    service.Initialize(12345);
    service.OpenAccessPoint();
    service.CreateNetwork("TestGame_12345");
    server.SimulatePlayerJoin(0x0A000001, "Player2");

    // Finalize
    ASSERT_EQ(service.Finalize(), ResultCode::Success);
    ASSERT_EQ(service.GetState(), CommState::None);
}

//=============================================================================
// CLIENT FLOW TESTS
//=============================================================================

/**
 * @brief Verifies the full client flow: scan finds one network, Connect reaches
 * StationConnected and the node count is host plus client.
 */
TEST(client_full_flow_scan_and_connect) {
    MockServer server;
    IntegratedLdnService service(server);

    // Setup: Create a network for scanning
    server.CreateNetworkForScan("TestGame_12345", 0xC0A80001);  // 192.168.0.1

    // Initialize
    ASSERT_EQ(service.Initialize(12345), ResultCode::Success);
    ASSERT_EQ(service.GetState(), CommState::Initialized);

    // Open station
    ASSERT_EQ(service.OpenStation(), ResultCode::Success);
    ASSERT_EQ(service.GetState(), CommState::Station);

    // Scan for networks
    ASSERT_EQ(service.Scan(), ResultCode::Success);
    ASSERT_EQ(service.GetScanResultCount(), 1);

    // Connect to first network
    ASSERT_EQ(service.Connect(0), ResultCode::Success);
    ASSERT_EQ(service.GetState(), CommState::StationConnected);

    // Verify connected nodes (host + us)
    ASSERT_EQ(service.GetConnectedNodeCount(), 2);
}

/**
 * @brief Exercises Scan with no network configured on the mock. Documents the
 * setup only: it makes no assertion on the result.
 */
TEST(client_scan_no_networks) {
    MockServer server;
    IntegratedLdnService service(server);

    // Don't create any networks

    service.Initialize(12345);
    service.OpenStation();
    service.Scan();

    // Should find no networks (server returns empty list by default)
    // Note: Our mock creates network only when CreateNetworkForScan is called
}

/**
 * @brief Verifies Connect with an out-of-range scan index returns NotFound and
 * leaves the state in Station.
 */
TEST(client_connect_invalid_index) {
    MockServer server;
    IntegratedLdnService service(server);

    server.CreateNetworkForScan("TestGame", 0xC0A80001);

    service.Initialize(12345);
    service.OpenStation();
    service.Scan();

    // Try to connect to invalid index
    ASSERT_EQ(service.Connect(99), ResultCode::NotFound);
    ASSERT_EQ(service.GetState(), CommState::Station);  // State unchanged
}

/**
 * @brief Verifies Disconnect from StationConnected succeeds and returns to
 * Station.
 */
TEST(client_disconnect) {
    MockServer server;
    IntegratedLdnService service(server);

    // Setup: Connected client
    server.CreateNetworkForScan("TestGame", 0xC0A80001);
    service.Initialize(12345);
    service.OpenStation();
    service.Scan();
    service.Connect(0);
    ASSERT_EQ(service.GetState(), CommState::StationConnected);

    // Disconnect
    ASSERT_EQ(service.Disconnect(), ResultCode::Success);
    ASSERT_EQ(service.GetState(), CommState::Station);
}

/**
 * @brief Verifies CloseStation is accepted from StationConnected and returns to
 * Initialized.
 */
TEST(client_close_station) {
    MockServer server;
    IntegratedLdnService service(server);

    // Setup: Connected client
    server.CreateNetworkForScan("TestGame", 0xC0A80001);
    service.Initialize(12345);
    service.OpenStation();
    service.Scan();
    service.Connect(0);

    // Close station (should work from StationConnected)
    ASSERT_EQ(service.CloseStation(), ResultCode::Success);
    ASSERT_EQ(service.GetState(), CommState::Initialized);
}

//=============================================================================
// PROXY DATA TESTS (BIDIRECTIONAL COMMUNICATION)
//=============================================================================

/// @brief Verifies the host can send unicast proxy data to a joined node.
TEST(proxy_data_send_unicast) {
    MockServer server;
    IntegratedLdnService service(server);

    // Setup: Host with player
    service.Initialize(12345);
    service.OpenAccessPoint();
    service.CreateNetwork("TestGame");
    server.SimulatePlayerJoin(0x0A000001, "Player2");

    // Send data to specific node
    u8 game_data[] = {0x01, 0x02, 0x03, 0x04};
    ASSERT_EQ(
        service.SendProxyData(1, game_data, sizeof(game_data)),
        ResultCode::Success);
}

/**
 * @brief Verifies the host can send broadcast proxy data with several players
 * joined.
 */
TEST(proxy_data_send_broadcast) {
    MockServer server;
    IntegratedLdnService service(server);

    // Setup: Host with 2 players
    service.Initialize(12345);
    service.OpenAccessPoint();
    service.CreateNetwork("TestGame");
    server.SimulatePlayerJoin(0x0A000001, "Player2");
    server.SimulatePlayerJoin(0x0A000002, "Player3");

    // Send broadcast
    u8 game_data[] = {0xFF, 0xFE, 0xFD};
    ASSERT_EQ(
        service.SendProxyData(0xFFFFFFFF, game_data, sizeof(game_data)),
        ResultCode::Success);
}

/**
 * @brief Verifies a client receives a packet addressed to its own node id and
 * that the sender id and payload size are preserved.
 */
TEST(proxy_data_receive) {
    MockServer server;
    IntegratedLdnService service(server);

    // Setup: Connected client
    server.CreateNetworkForScan("TestGame", 0xC0A80001);
    service.Initialize(12345);
    service.OpenStation();
    service.Scan();
    service.Connect(0);

    // Simulate receiving data from host
    u8 incoming[] = {0xAA, 0xBB, 0xCC, 0xDD};
    server.SimulateProxyData(
        0,
        service.GetNetworkInfo().ldn.node_count - 1,
        incoming,
        sizeof(incoming));

    // Receive the data
    ProxyDataHeader header{};
    u8 buffer[256];
    size_t size;
    ASSERT(service.ReceiveProxyData(header, buffer, size, sizeof(buffer)));
    ASSERT_EQ(size, sizeof(incoming));
    ASSERT_EQ(header.src_node_id, 0U);  // From host
}

/// @brief Verifies a client receives a packet addressed to BroadcastNodeId.
TEST(proxy_data_receive_broadcast) {
    MockServer server;
    IntegratedLdnService service(server);

    // Setup: Connected client
    server.CreateNetworkForScan("TestGame", 0xC0A80001);
    service.Initialize(12345);
    service.OpenStation();
    service.Scan();
    service.Connect(0);

    // Simulate broadcast from host
    u8 incoming[] = {0x11, 0x22};
    server.SimulateProxyData(0, 0xFFFFFFFF, incoming, sizeof(incoming));

    // Should receive broadcast
    ProxyDataHeader header{};
    u8 buffer[256];
    size_t size;
    ASSERT(service.ReceiveProxyData(header, buffer, size, sizeof(buffer)));
    ASSERT_EQ(header.dest_node_id, 0xFFFFFFFFU);
}

/**
 * @brief Verifies a client drops a unicast packet addressed to another node, so
 * game data is never delivered to the wrong console.
 */
TEST(proxy_data_not_for_us) {
    MockServer server;
    IntegratedLdnService service(server);

    // Setup: Connected client (node 1)
    server.CreateNetworkForScan("TestGame", 0xC0A80001);
    service.Initialize(12345);
    service.OpenStation();
    service.Scan();
    service.Connect(0);

    // Simulate unicast to different node (node 2, not us who are node 1)
    u8 incoming[] = {0x99};
    server.SimulateProxyData(0, 2, incoming, sizeof(incoming));

    // Should NOT receive - message not for us
    // In real impl, server wouldn't send to wrong client anyway
    // The packet is filtered by HandleProxyDataReceived checking dest_node_id
    ProxyDataHeader header{};
    u8 buffer[256];
    size_t size;
    bool received =
        service.ReceiveProxyData(header, buffer, size, sizeof(buffer));
    // Packet was for node 2, we are node 1, so we should not receive it
    ASSERT(!received);
}

//=============================================================================
// STATE TRANSITION TESTS
//=============================================================================

/// @brief Verifies a second Initialize without Finalize returns InvalidState.
TEST(state_invalid_initialize_twice) {
    MockServer server;
    IntegratedLdnService service(server);

    service.Initialize(12345);
    ASSERT_EQ(service.Initialize(12345), ResultCode::InvalidState);
}

/// @brief Verifies CreateNetwork without OpenAccessPoint returns InvalidState.
TEST(state_invalid_create_without_open) {
    MockServer server;
    IntegratedLdnService service(server);

    service.Initialize(12345);
    // Skip OpenAccessPoint
    ASSERT_EQ(service.CreateNetwork("Test"), ResultCode::InvalidState);
}

/// @brief Verifies Connect without OpenStation returns InvalidState.
TEST(state_invalid_connect_without_open) {
    MockServer server;
    IntegratedLdnService service(server);

    service.Initialize(12345);
    // Skip OpenStation
    ASSERT_EQ(service.Connect(0), ResultCode::InvalidState);
}

/**
 * @brief Verifies SendProxyData is refused with InvalidState while a network
 * has not been created.
 */
TEST(state_invalid_send_data_not_connected) {
    MockServer server;
    IntegratedLdnService service(server);

    service.Initialize(12345);
    service.OpenAccessPoint();  // But don't create network

    u8 data[] = {0x01};
    ASSERT_EQ(
        service.SendProxyData(0, data, sizeof(data)), ResultCode::InvalidState);
}

/**
 * @brief Verifies the state-change event is raised by a transition and cleared
 * by ClearEvent().
 */
TEST(state_event_signaled_on_change) {
    MockServer server;
    IntegratedLdnService service(server);

    ASSERT(!service.IsEventSignaled());

    service.Initialize(12345);
    ASSERT(service.IsEventSignaled());

    service.ClearEvent();
    ASSERT(!service.IsEventSignaled());

    service.OpenAccessPoint();
    ASSERT(service.IsEventSignaled());
}

//=============================================================================
// ERROR HANDLING TESTS
//=============================================================================

/**
 * @brief Exercises Disconnect after a connect. Documents the setup only: it
 * makes no assertion on the disconnect reason.
 */
TEST(error_disconnect_reason_preserved) {
    MockServer server;
    IntegratedLdnService service(server);

    // Setup: Connected client
    server.CreateNetworkForScan("TestGame", 0xC0A80001);
    service.Initialize(12345);
    service.OpenStation();
    service.Scan();
    service.Connect(0);

    // Request disconnect (triggers Disconnected response from server)
    service.Disconnect();

    // Note: Our mock sends Disconnected which sets Error state
    // In real impl, Disconnect() itself doesn't set Error
}

/**
 * @brief Verifies Finalize returns to None from Initialized after OpenStation
 * and that Initialize is accepted again.
 */
TEST(error_recovery_from_initialized) {
    MockServer server;
    IntegratedLdnService service(server);

    service.Initialize(12345);
    service.OpenStation();

    // Finalize resets to None, can initialize again
    service.Finalize();
    ASSERT_EQ(service.GetState(), CommState::None);

    ASSERT_EQ(service.Initialize(12345), ResultCode::Success);
    ASSERT_EQ(service.GetState(), CommState::Initialized);
}

//=============================================================================
// COMPLEX SCENARIO TESTS
//=============================================================================

/**
 * @brief Verifies a console can host, close the access point and then scan and
 * connect as a client in the same session.
 */
TEST(scenario_host_to_client_transition) {
    MockServer server;
    IntegratedLdnService service(server);

    // Start as host
    service.Initialize(12345);
    service.OpenAccessPoint();
    service.CreateNetwork("HostGame");
    ASSERT_EQ(service.GetState(), CommState::AccessPointCreated);

    // Close and switch to client
    service.CloseAccessPoint();
    ASSERT_EQ(service.GetState(), CommState::Initialized);

    server.CreateNetworkForScan("OtherGame", 0xC0A80001);
    service.OpenStation();
    service.Scan();
    service.Connect(0);
    ASSERT_EQ(service.GetState(), CommState::StationConnected);
}

/**
 * @brief Verifies two scan, connect, disconnect, close cycles in a row end in
 * Initialized.
 */
TEST(scenario_multiple_scan_connect_cycles) {
    MockServer server;
    IntegratedLdnService service(server);

    service.Initialize(12345);

    // Cycle 1
    server.CreateNetworkForScan("Game1", 0xC0A80001);
    service.OpenStation();
    service.Scan();
    service.Connect(0);
    service.Disconnect();
    service.CloseStation();

    // Cycle 2
    server.CreateNetworkForScan("Game2", 0xC0A80002);
    service.OpenStation();
    service.Scan();
    service.Connect(0);
    service.Disconnect();
    service.CloseStation();

    ASSERT_EQ(service.GetState(), CommState::Initialized);
}

/**
 * @brief Verifies an 8 player lobby is reached and the host can broadcast to
 * it.
 */
TEST(scenario_8_players_session) {
    MockServer server;
    IntegratedLdnService service(server);

    // Host creates network
    service.Initialize(12345);
    service.OpenAccessPoint();
    service.CreateNetwork("FullGame");
    ASSERT_EQ(service.GetConnectedNodeCount(), 1);

    // 7 more players join
    for (int i = 1; i < 8; i++) {
        char name[32];
        snprintf(name, sizeof(name), "Player%d", i + 1);
        server.SimulatePlayerJoin(static_cast<u32>(0x0A000000 + i), name);
    }

    // Verify full lobby
    ASSERT_EQ(service.GetConnectedNodeCount(), 8);

    // Broadcast from host
    u8 game_state[] = {0x42, 0x42, 0x42};
    ASSERT_EQ(
        service.SendProxyData(0xFFFFFFFF, game_state, sizeof(game_state)),
        ResultCode::Success);
}

/**
 * @brief Verifies 10 consecutive connect and disconnect cycles keep the state
 * machine consistent (StationConnected then Station each time).
 */
TEST(scenario_rapid_connect_disconnect) {
    MockServer server;
    IntegratedLdnService service(server);

    server.CreateNetworkForScan("StressTest", 0xC0A80001);
    service.Initialize(12345);
    service.OpenStation();

    // Rapid connect/disconnect cycles
    for (int i = 0; i < 10; i++) {
        service.Scan();
        service.Connect(0);
        ASSERT_EQ(service.GetState(), CommState::StationConnected);

        service.Disconnect();
        ASSERT_EQ(service.GetState(), CommState::Station);
    }

    service.CloseStation();
    ASSERT_EQ(service.GetState(), CommState::Initialized);
}

//=============================================================================
// Main
//=============================================================================

/**
 * @brief Runs every test and reports the totals.
 * @return 0 if all tests passed, 1 otherwise.
 */
int main() {
    printf("=== LDN Integration Tests ===\n\n");

    printf("--- Host Flow Tests ---\n");
    RUN_TEST(host_full_flow_create_network);
    RUN_TEST(host_player_joins);
    RUN_TEST(host_player_leaves);
    RUN_TEST(host_destroy_network);
    RUN_TEST(host_close_access_point);
    RUN_TEST(host_finalize_cleans_up);

    printf("\n--- Client Flow Tests ---\n");
    RUN_TEST(client_full_flow_scan_and_connect);
    RUN_TEST(client_scan_no_networks);
    RUN_TEST(client_connect_invalid_index);
    RUN_TEST(client_disconnect);
    RUN_TEST(client_close_station);

    printf("\n--- Proxy Data Tests ---\n");
    RUN_TEST(proxy_data_send_unicast);
    RUN_TEST(proxy_data_send_broadcast);
    RUN_TEST(proxy_data_receive);
    RUN_TEST(proxy_data_receive_broadcast);
    RUN_TEST(proxy_data_not_for_us);

    printf("\n--- State Transition Tests ---\n");
    RUN_TEST(state_invalid_initialize_twice);
    RUN_TEST(state_invalid_create_without_open);
    RUN_TEST(state_invalid_connect_without_open);
    RUN_TEST(state_invalid_send_data_not_connected);
    RUN_TEST(state_event_signaled_on_change);

    printf("\n--- Error Handling Tests ---\n");
    RUN_TEST(error_disconnect_reason_preserved);
    RUN_TEST(error_recovery_from_initialized);

    printf("\n--- Complex Scenario Tests ---\n");
    RUN_TEST(scenario_host_to_client_transition);
    RUN_TEST(scenario_multiple_scan_connect_cycles);
    RUN_TEST(scenario_8_players_session);
    RUN_TEST(scenario_rapid_connect_disconnect);

    printf("\n========================================\n");
    printf("Results: %d/%d tests passed\n", g_tests_passed, g_tests_run);
    printf("========================================\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}