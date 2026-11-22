/**
 * @file overlay_tests.cpp
 * @brief Unit tests for Tesla overlay logic
 *
 * Tests the overlay helper functions and IPC structures without
 * requiring the actual Switch environment. Uses mocks for libnx types.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

//=============================================================================
// Mock Switch/libnx types for testing
//=============================================================================

// Basic types
/// Mock of the libnx 8-bit unsigned type.
typedef uint8_t u8;
/// Mock of the libnx 16-bit unsigned type.
typedef uint16_t u16;
/// Mock of the libnx 32-bit unsigned type.
typedef uint32_t u32;
/// Mock of the libnx 64-bit unsigned type.
typedef uint64_t u64;
/// Mock of the libnx Result code (0 means success).
typedef int32_t Result;

/**
 * @brief Mock of the libnx success check.
 * @param res Result code.
 */
#define R_SUCCEEDED(res) ((res) == 0)
/**
 * @brief Mock of the libnx failure check.
 * @param res Result code.
 */
#define R_FAILED(res) ((res) != 0)

/// Mock of the libnx Service session structure.
typedef struct {
    /// Mock service session handle.
    u32 handle;
} Service;

//=============================================================================
// Include overlay types (copy from ryu_ldn_ipc.h for testing)
//=============================================================================

/// Connection status of the sysmodule to the server.
typedef enum {
    /// Not connected to the server.
    RyuLdnStatus_Disconnected = 0,
    /// Connection attempt in progress.
    RyuLdnStatus_Connecting = 1,
    /// TCP connection established.
    RyuLdnStatus_Connected = 2,
    /// Connected and ready for LDN traffic.
    RyuLdnStatus_Ready = 3,
    /// Connection failed.
    RyuLdnStatus_Error = 4,
} RyuLdnConnectionStatus;

/// State of the emulated LDN communication service.
typedef enum {
    /// LDN not initialised.
    RyuLdnState_None = 0,
    /// LDN initialised, no role yet.
    RyuLdnState_Initialized = 1,
    /// Opened as access point, network not created.
    RyuLdnState_AccessPoint = 2,
    /// Network created and hosted.
    RyuLdnState_AccessPointCreated = 3,
    /// Opened as station, not connected.
    RyuLdnState_Station = 4,
    /// Connected to a network.
    RyuLdnState_StationConnected = 5,
    /// LDN error state.
    RyuLdnState_Error = 6,
} RyuLdnState;

/**
 * @brief Session information structure (matches IPC definition)
 *
 * 8 bytes total, same as SessionInfoIpc in sysmodule.
 */
typedef struct {
    u8 node_count;     ///< Current number of nodes in session
    u8 max_nodes;      ///< Maximum nodes allowed in session
    u8 local_node_id;  ///< This node's ID in the session
    u8 is_host;        ///< 1 if this node is the host, 0 otherwise
    u8 reserved[4];    ///< Reserved for future use
} RyuLdnSessionInfo;

/// Handle of the ryu:cfg service session (mock).
typedef struct {
    /// Underlying service session.
    Service s;
} RyuLdnConfigService;

//=============================================================================
// Helper functions from main.cpp (extracted for testing)
//=============================================================================

/**
 * @brief Get the label of a connection status.
 * @param status Status to name.
 * @return Static label, "Unknown" for an unlisted value.
 */
const char* ConnectionStatusToString(RyuLdnConnectionStatus status) {
    switch (status) {
        case RyuLdnStatus_Disconnected:
            return "Disconnected";
        case RyuLdnStatus_Connecting:
            return "Connecting...";
        case RyuLdnStatus_Connected:
            return "Connected";
        case RyuLdnStatus_Ready:
            return "Ready";
        case RyuLdnStatus_Error:
            return "Error";
        default:
            return "Unknown";
    }
}

/**
 * @brief Get the label of an LDN state.
 * @param state State to name.
 * @return Static label, "Unknown" for an unlisted value.
 */
const char* LdnStateToString(RyuLdnState state) {
    switch (state) {
        case RyuLdnState_None:
            return "None";
        case RyuLdnState_Initialized:
            return "Initialized";
        case RyuLdnState_AccessPoint:
            return "Access Point";
        case RyuLdnState_AccessPointCreated:
            return "Hosting";
        case RyuLdnState_Station:
            return "Station";
        case RyuLdnState_StationConnected:
            return "Connected";
        case RyuLdnState_Error:
            return "Error";
        default:
            return "Unknown";
    }
}

// Format session info string
/**
 * @brief Format the session summary shown by the overlay.
 * @param info Session information.
 * @param buf Output buffer; receives "Not in session" or
 *        "<nodes>/<max> players (Host|Client)".
 * @param bufSize Size of buf in bytes.
 */
void FormatSessionInfo(
    const RyuLdnSessionInfo* info, char* buf, size_t bufSize) {
    if (info->node_count == 0) {
        snprintf(buf, bufSize, "Not in session");
    } else {
        snprintf(
            buf,
            bufSize,
            "%d/%d players (%s)",
            info->node_count,
            info->max_nodes,
            info->is_host ? "Host" : "Client");
    }
}

// Format server address string
/**
 * @brief Format a server address as host:port.
 * @param host Server host name or IP.
 * @param port Server port.
 * @param buf Output buffer.
 * @param bufSize Size of buf in bytes.
 */
void FormatServerAddress(
    const char* host, u16 port, char* buf, size_t bufSize) {
    snprintf(buf, bufSize, "%s:%u", host, port);
}

// Format latency string
/**
 * @brief Format a round-trip time, "N/A" when it is 0.
 * @param rtt_ms Round-trip time in milliseconds.
 * @param buf Output buffer.
 * @param bufSize Size of buf in bytes.
 */
void FormatLatency(u32 rtt_ms, char* buf, size_t bufSize) {
    if (rtt_ms == 0) {
        snprintf(buf, bufSize, "N/A");
    } else {
        snprintf(buf, bufSize, "%u ms", rtt_ms);
    }
}

//=============================================================================
// Test Framework
//=============================================================================

/// Number of tests started.
static int g_tests_run = 0;
/// Number of tests that completed (a failed assertion exits the process).
static int g_tests_passed = 0;

/**
 * @brief Define a test as a plain void function run by RUN_TEST().
 * @param name Identifier of the test function.
 */
#define TEST(name) void name()
/**
 * @brief Run a test defined with TEST(), printing its name and PASS.
 * @param name Identifier of the test function.
 */
#define RUN_TEST(name)                   \
    do {                                 \
        g_tests_run++;                   \
        printf("  [TEST] %s...", #name); \
        name();                          \
        g_tests_passed++;                \
        printf(" PASS\n");               \
    } while (0)

/**
 * @brief Print the failure and exit(1) when cond is false.
 * @param cond Expression expected to be true.
 */
#define ASSERT(cond)                                               \
    do {                                                           \
        if (!(cond)) {                                             \
            printf(                                                \
                " FAIL\n    Assertion failed: %s\n    at %s:%d\n", \
                #cond,                                             \
                __FILE__,                                          \
                __LINE__);                                         \
            exit(1);                                               \
        }                                                          \
    } while (0)

/**
 * @brief Assert a == b.
 * @param a Actual value.
 * @param b Expected value.
 */
#define ASSERT_EQ(a, b) ASSERT((a) == (b))
/**
 * @brief Assert a != b.
 * @param a Actual value.
 * @param b Value a must differ from.
 */
#define ASSERT_NE(a, b) ASSERT((a) != (b))
/**
 * @brief Assert that two C strings are equal.
 * @param a Actual string.
 * @param b Expected string.
 */
#define ASSERT_STREQ(a, b) ASSERT(strcmp((a), (b)) == 0)

//=============================================================================
// Connection Status Tests
//=============================================================================

/**
 * @brief Disconnected maps to "Disconnected".
 */
TEST(status_disconnected_to_string) {
    ASSERT_STREQ(
        ConnectionStatusToString(RyuLdnStatus_Disconnected), "Disconnected");
}

/**
 * @brief Connecting maps to "Connecting...".
 */
TEST(status_connecting_to_string) {
    ASSERT_STREQ(
        ConnectionStatusToString(RyuLdnStatus_Connecting), "Connecting...");
}

/**
 * @brief Connected maps to "Connected".
 */
TEST(status_connected_to_string) {
    ASSERT_STREQ(ConnectionStatusToString(RyuLdnStatus_Connected), "Connected");
}

/**
 * @brief Ready maps to "Ready".
 */
TEST(status_ready_to_string) {
    ASSERT_STREQ(ConnectionStatusToString(RyuLdnStatus_Ready), "Ready");
}

/**
 * @brief Error maps to "Error".
 */
TEST(status_error_to_string) {
    ASSERT_STREQ(ConnectionStatusToString(RyuLdnStatus_Error), "Error");
}

/**
 * @brief An out-of-range status maps to "Unknown".
 */
TEST(status_unknown_to_string) {
    ASSERT_STREQ(
        ConnectionStatusToString((RyuLdnConnectionStatus)99), "Unknown");
}

//=============================================================================
// LDN State Tests
//=============================================================================

/**
 * @brief None maps to "None".
 */
TEST(ldn_state_none_to_string) {
    ASSERT_STREQ(LdnStateToString(RyuLdnState_None), "None");
}

/**
 * @brief Initialized maps to "Initialized".
 */
TEST(ldn_state_initialized_to_string) {
    ASSERT_STREQ(LdnStateToString(RyuLdnState_Initialized), "Initialized");
}

/**
 * @brief AccessPoint maps to "Access Point".
 */
TEST(ldn_state_access_point_to_string) {
    ASSERT_STREQ(LdnStateToString(RyuLdnState_AccessPoint), "Access Point");
}

/**
 * @brief AccessPointCreated maps to "Hosting".
 */
TEST(ldn_state_access_point_created_to_string) {
    ASSERT_STREQ(LdnStateToString(RyuLdnState_AccessPointCreated), "Hosting");
}

/**
 * @brief Station maps to "Station".
 */
TEST(ldn_state_station_to_string) {
    ASSERT_STREQ(LdnStateToString(RyuLdnState_Station), "Station");
}

/**
 * @brief StationConnected maps to "Connected".
 */
TEST(ldn_state_station_connected_to_string) {
    ASSERT_STREQ(LdnStateToString(RyuLdnState_StationConnected), "Connected");
}

/**
 * @brief Error maps to "Error".
 */
TEST(ldn_state_error_to_string) {
    ASSERT_STREQ(LdnStateToString(RyuLdnState_Error), "Error");
}

/**
 * @brief An out-of-range state maps to "Unknown".
 */
TEST(ldn_state_unknown_to_string) {
    ASSERT_STREQ(LdnStateToString((RyuLdnState)99), "Unknown");
}

//=============================================================================
// Session Info Formatting Tests
//=============================================================================

/**
 * @brief Zero nodes is shown as "Not in session".
 */
TEST(session_info_not_in_session) {
    RyuLdnSessionInfo info = {};
    info.node_count = 0;
    char buf[64];
    FormatSessionInfo(&info, buf, sizeof(buf));
    ASSERT_STREQ(buf, "Not in session");
}

/**
 * @brief One node as host is shown as "1/8 players (Host)".
 */
TEST(session_info_host_single_player) {
    RyuLdnSessionInfo info = {};
    info.node_count = 1;
    info.max_nodes = 8;
    info.is_host = 1;
    char buf[64];
    FormatSessionInfo(&info, buf, sizeof(buf));
    ASSERT_STREQ(buf, "1/8 players (Host)");
}

/**
 * @brief Four nodes as client is shown as "4/8 players (Client)".
 */
TEST(session_info_client_multi_player) {
    RyuLdnSessionInfo info = {};
    info.node_count = 4;
    info.max_nodes = 8;
    info.is_host = 0;
    char buf[64];
    FormatSessionInfo(&info, buf, sizeof(buf));
    ASSERT_STREQ(buf, "4/8 players (Client)");
}

/**
 * @brief A full session of 8 as host is shown as "8/8 players (Host)".
 */
TEST(session_info_full_session) {
    RyuLdnSessionInfo info = {};
    info.node_count = 8;
    info.max_nodes = 8;
    info.is_host = 1;
    char buf[64];
    FormatSessionInfo(&info, buf, sizeof(buf));
    ASSERT_STREQ(buf, "8/8 players (Host)");
}

/**
 * @brief A 2-player session as client is shown as "2/2 players (Client)".
 */
TEST(session_info_two_players) {
    RyuLdnSessionInfo info = {};
    info.node_count = 2;
    info.max_nodes = 2;
    info.is_host = 0;
    char buf[64];
    FormatSessionInfo(&info, buf, sizeof(buf));
    ASSERT_STREQ(buf, "2/2 players (Client)");
}

//=============================================================================
// Server Address Formatting Tests
//=============================================================================

/**
 * @brief localhost and port 39990 give "localhost:39990".
 */
TEST(server_address_localhost) {
    char buf[96];
    FormatServerAddress("localhost", 39990, buf, sizeof(buf));
    ASSERT_STREQ(buf, "localhost:39990");
}

/**
 * @brief An IPv4 address and port 39990 give "192.168.1.100:39990".
 */
TEST(server_address_ip) {
    char buf[96];
    FormatServerAddress("192.168.1.100", 39990, buf, sizeof(buf));
    ASSERT_STREQ(buf, "192.168.1.100:39990");
}

/**
 * @brief A host name and port 39990 give "ryu.example.com:39990".
 */
TEST(server_address_hostname) {
    char buf[96];
    FormatServerAddress("ryu.example.com", 39990, buf, sizeof(buf));
    ASSERT_STREQ(buf, "ryu.example.com:39990");
}

/**
 * @brief A non-default port is printed as given.
 */
TEST(server_address_custom_port) {
    char buf[96];
    FormatServerAddress("server.net", 12345, buf, sizeof(buf));
    ASSERT_STREQ(buf, "server.net:12345");
}

//=============================================================================
// Latency Formatting Tests
//=============================================================================

/**
 * @brief A zero latency is shown as "N/A".
 */
TEST(latency_zero_shows_na) {
    char buf[32];
    FormatLatency(0, buf, sizeof(buf));
    ASSERT_STREQ(buf, "N/A");
}

/**
 * @brief 5 ms is shown as "5 ms".
 */
TEST(latency_small_value) {
    char buf[32];
    FormatLatency(5, buf, sizeof(buf));
    ASSERT_STREQ(buf, "5 ms");
}

/**
 * @brief 42 ms is shown as "42 ms".
 */
TEST(latency_medium_value) {
    char buf[32];
    FormatLatency(42, buf, sizeof(buf));
    ASSERT_STREQ(buf, "42 ms");
}

/**
 * @brief 250 ms is shown as "250 ms".
 */
TEST(latency_high_value) {
    char buf[32];
    FormatLatency(250, buf, sizeof(buf));
    ASSERT_STREQ(buf, "250 ms");
}

/**
 * @brief 1500 ms is shown as "1500 ms".
 */
TEST(latency_very_high_value) {
    char buf[32];
    FormatLatency(1500, buf, sizeof(buf));
    ASSERT_STREQ(buf, "1500 ms");
}

//=============================================================================
// IPC Structure Tests
//=============================================================================

/**
 * @brief RyuLdnSessionInfo is exactly 8 bytes, as on the IPC wire.
 */
TEST(session_info_structure_size) {
    // Verify structure packing - must be exactly 8 bytes for IPC
    ASSERT_EQ(sizeof(RyuLdnSessionInfo), 8u);
}

/**
 * @brief A value-initialised RyuLdnSessionInfo is all zero.
 */
TEST(session_info_zero_initialized) {
    RyuLdnSessionInfo info = {};
    ASSERT_EQ(info.node_count, 0);
    ASSERT_EQ(info.max_nodes, 0);
    ASSERT_EQ(info.local_node_id, 0);
    ASSERT_EQ(info.is_host, 0);
    ASSERT_EQ(info.reserved[0], 0);
}

/**
 * @brief The connection status enumerators are 0 to 4 in IPC order.
 */
TEST(connection_status_enum_values) {
    ASSERT_EQ(RyuLdnStatus_Disconnected, 0);
    ASSERT_EQ(RyuLdnStatus_Connecting, 1);
    ASSERT_EQ(RyuLdnStatus_Connected, 2);
    ASSERT_EQ(RyuLdnStatus_Ready, 3);
    ASSERT_EQ(RyuLdnStatus_Error, 4);
}

/**
 * @brief The LDN state enumerators are 0 to 6 in IPC order.
 */
TEST(ldn_state_enum_values) {
    ASSERT_EQ(RyuLdnState_None, 0);
    ASSERT_EQ(RyuLdnState_Initialized, 1);
    ASSERT_EQ(RyuLdnState_AccessPoint, 2);
    ASSERT_EQ(RyuLdnState_AccessPointCreated, 3);
    ASSERT_EQ(RyuLdnState_Station, 4);
    ASSERT_EQ(RyuLdnState_StationConnected, 5);
    ASSERT_EQ(RyuLdnState_Error, 6);
}

//=============================================================================
// Edge Cases
//=============================================================================

/**
 * @brief A buffer too small for the session text is truncated without crashing.
 */
TEST(format_session_buffer_small) {
    RyuLdnSessionInfo info = {};
    info.node_count = 8;
    info.max_nodes = 8;
    info.is_host = 1;
    char buf[10];  // Too small, but should not crash
    FormatSessionInfo(&info, buf, sizeof(buf));
    // Just verify it doesn't crash and produces something
    ASSERT(strlen(buf) < 10);
}

/**
 * @brief The largest u32 latency still produces a non-empty string.
 */
TEST(format_latency_max_u32) {
    char buf[32];
    FormatLatency(4294967295u, buf, sizeof(buf));
    ASSERT(strlen(buf) > 0);
}

//=============================================================================
// Main
//=============================================================================

/**
 * @brief Run every test and print the totals.
 * @return 0 when all tests passed, 1 otherwise.
 */
int main() {
    printf("\n========================================\n");
    printf("  Overlay Tests - ryu_ldn_nx\n");
    printf("========================================\n\n");

    printf("--- Connection Status Tests ---\n");
    RUN_TEST(status_disconnected_to_string);
    RUN_TEST(status_connecting_to_string);
    RUN_TEST(status_connected_to_string);
    RUN_TEST(status_ready_to_string);
    RUN_TEST(status_error_to_string);
    RUN_TEST(status_unknown_to_string);

    printf("\n--- LDN State Tests ---\n");
    RUN_TEST(ldn_state_none_to_string);
    RUN_TEST(ldn_state_initialized_to_string);
    RUN_TEST(ldn_state_access_point_to_string);
    RUN_TEST(ldn_state_access_point_created_to_string);
    RUN_TEST(ldn_state_station_to_string);
    RUN_TEST(ldn_state_station_connected_to_string);
    RUN_TEST(ldn_state_error_to_string);
    RUN_TEST(ldn_state_unknown_to_string);

    printf("\n--- Session Info Formatting Tests ---\n");
    RUN_TEST(session_info_not_in_session);
    RUN_TEST(session_info_host_single_player);
    RUN_TEST(session_info_client_multi_player);
    RUN_TEST(session_info_full_session);
    RUN_TEST(session_info_two_players);

    printf("\n--- Server Address Formatting Tests ---\n");
    RUN_TEST(server_address_localhost);
    RUN_TEST(server_address_ip);
    RUN_TEST(server_address_hostname);
    RUN_TEST(server_address_custom_port);

    printf("\n--- Latency Formatting Tests ---\n");
    RUN_TEST(latency_zero_shows_na);
    RUN_TEST(latency_small_value);
    RUN_TEST(latency_medium_value);
    RUN_TEST(latency_high_value);
    RUN_TEST(latency_very_high_value);

    printf("\n--- IPC Structure Tests ---\n");
    RUN_TEST(session_info_structure_size);
    RUN_TEST(session_info_zero_initialized);
    RUN_TEST(connection_status_enum_values);
    RUN_TEST(ldn_state_enum_values);

    printf("\n--- Edge Cases ---\n");
    RUN_TEST(format_session_buffer_small);
    RUN_TEST(format_latency_max_u32);

    printf("\n========================================\n");
    printf("  Results: %d/%d passed\n", g_tests_passed, g_tests_run);
    printf("========================================\n\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
