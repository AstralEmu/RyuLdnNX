/**
 * @file ipc_config_tests.cpp
 * @brief Unit tests for extended IPC configuration commands
 *
 * Tests the IPC structures and helper functions for the extended
 * configuration service that allows Tesla overlay to modify all settings.
 *
 * Extended IPC Commands (65011-65030):
 * - 65011: GetPassphrase / 65012: SetPassphrase
 * - 65013: GetLdnEnabled / 65014: SetLdnEnabled
 * - 65017: GetConnectTimeout / 65018: SetConnectTimeout
 * - 65019: GetPingInterval / 65020: SetPingInterval
 * - 65021: GetReconnectDelay / 65022: SetReconnectDelay
 * - 65023: GetMaxReconnectAttempts / 65024: SetMaxReconnectAttempts
 * - 65025: GetDebugLevel / 65026: SetDebugLevel
 * - 65029: SaveConfig
 * - 65030: ReloadConfig
 *
 * Also tests overlay helper functions for formatting settings values.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

//=============================================================================
// Mock Switch/libnx types for testing
//=============================================================================

/// Unsigned 8-bit integer, as in libnx types.
using u8 = uint8_t;
/// Unsigned 16-bit integer, as in libnx types.
using u16 = uint16_t;
/// Unsigned 32-bit integer, as in libnx types.
using u32 = uint32_t;
/// Unsigned 64-bit integer, as in libnx types.
using u64 = uint64_t;
/// Signed 32-bit integer, as in libnx types.
using s32 = int32_t;
/// Result code; 0 means success, as in libnx.
using Result = int32_t;

/**
 * @brief Check a Result for success.
 * @param res Result code to test.
 * @return true when res is 0.
 */
#define R_SUCCEEDED(res) ((res) == 0)
/**
 * @brief Check a Result for failure.
 * @param res Result code to test.
 * @return true when res is not 0.
 */
#define R_FAILED(res) ((res) != 0)

//=============================================================================
// IPC Command IDs (from ldn_config_service.hpp)
//=============================================================================

// Existing commands (65001-65010)
/// IPC command id of get version.
constexpr u32 IPC_CMD_GET_VERSION = 65001;
/// IPC command id of get connection status.
constexpr u32 IPC_CMD_GET_CONNECTION_STATUS = 65002;
/// IPC command id of get ldn state.
constexpr u32 IPC_CMD_GET_LDN_STATE = 65003;
/// IPC command id of get session info.
constexpr u32 IPC_CMD_GET_SESSION_INFO = 65004;
/// IPC command id of get server address.
constexpr u32 IPC_CMD_GET_SERVER_ADDRESS = 65005;
/// IPC command id of set server address.
constexpr u32 IPC_CMD_SET_SERVER_ADDRESS = 65006;
/// IPC command id of get debug enabled.
constexpr u32 IPC_CMD_GET_DEBUG_ENABLED = 65007;
/// IPC command id of set debug enabled.
constexpr u32 IPC_CMD_SET_DEBUG_ENABLED = 65008;
/// IPC command id of force reconnect.
constexpr u32 IPC_CMD_FORCE_RECONNECT = 65009;
/// IPC command id of get last rtt.
constexpr u32 IPC_CMD_GET_LAST_RTT = 65010;

// Extended commands (65011-65030)
/// IPC command id of get passphrase.
constexpr u32 IPC_CMD_GET_PASSPHRASE = 65011;
/// IPC command id of set passphrase.
constexpr u32 IPC_CMD_SET_PASSPHRASE = 65012;
/// IPC command id of get ldn enabled.
constexpr u32 IPC_CMD_GET_LDN_ENABLED = 65013;
/// IPC command id of set ldn enabled.
constexpr u32 IPC_CMD_SET_LDN_ENABLED = 65014;
/// IPC command id of get connect timeout.
constexpr u32 IPC_CMD_GET_CONNECT_TIMEOUT = 65017;
/// IPC command id of set connect timeout.
constexpr u32 IPC_CMD_SET_CONNECT_TIMEOUT = 65018;
/// IPC command id of get ping interval.
constexpr u32 IPC_CMD_GET_PING_INTERVAL = 65019;
/// IPC command id of set ping interval.
constexpr u32 IPC_CMD_SET_PING_INTERVAL = 65020;
/// IPC command id of get reconnect delay.
constexpr u32 IPC_CMD_GET_RECONNECT_DELAY = 65021;
/// IPC command id of set reconnect delay.
constexpr u32 IPC_CMD_SET_RECONNECT_DELAY = 65022;
/// IPC command id of get max reconnect attempts.
constexpr u32 IPC_CMD_GET_MAX_RECONNECT_ATTEMPTS = 65023;
/// IPC command id of set max reconnect attempts.
constexpr u32 IPC_CMD_SET_MAX_RECONNECT_ATTEMPTS = 65024;
/// IPC command id of get debug level.
constexpr u32 IPC_CMD_GET_DEBUG_LEVEL = 65025;
/// IPC command id of set debug level.
constexpr u32 IPC_CMD_SET_DEBUG_LEVEL = 65026;
/// IPC command id of save config.
constexpr u32 IPC_CMD_SAVE_CONFIG = 65029;
/// IPC command id of reload config.
constexpr u32 IPC_CMD_RELOAD_CONFIG = 65030;

//=============================================================================
// IPC Data Structures
//=============================================================================

// Passphrase structure (max 64 chars)
/// Passphrase payload of the Get/SetPassphrase commands.
struct IpcPassphrase {
    /// Passphrase (null-terminated, at most 63 characters).
    char passphrase[64];
};
static_assert(sizeof(IpcPassphrase) == 64, "IpcPassphrase must be 64 bytes");

// Server address structure (existing)
/// Server address payload of the Get/SetServerAddress commands.
struct IpcServerAddress {
    /// Server hostname or IP (null-terminated).
    char host[64];
    /// Server port number.
    u16 port;
    /// Padding for alignment.
    u16 padding;
};
static_assert(
    sizeof(IpcServerAddress) == 68, "IpcServerAddress must be 68 bytes");

// Debug settings structure (for batch get)
/// Debug settings payload (batch get).
struct IpcDebugSettings {
    /// Debug logging flag (0 or 1).
    u32 enabled;  // bool as u32
    /// Log verbosity level (0-3).
    u32 level;  // 0-3
};
static_assert(
    sizeof(IpcDebugSettings) == 8, "IpcDebugSettings must be 8 bytes");

// Config result codes
/// Result codes returned by the configuration commands.
enum class IpcConfigResult : u32 {
    /// The operation succeeded.
    Success = 0,
    /// The configuration file does not exist.
    FileNotFound = 1,
    /// The configuration file could not be parsed.
    ParseError = 2,
    /// Reading or writing the configuration file failed.
    IoError = 3,
    /// A value is outside its allowed range.
    InvalidValue = 4,
};

//=============================================================================
// Helper Functions for Overlay
//=============================================================================

// Format passphrase for display (masked)
/**
 * @brief Format a passphrase for display, masking the middle.
 * @param passphrase Passphrase, or null/empty for none.
 * @param[out] buf Receives the text.
 * @param bufSize Capacity of buf.
 */
void FormatPassphraseMasked(const char* passphrase, char* buf, size_t bufSize) {
    if (passphrase == nullptr || passphrase[0] == '\0') {
        snprintf(buf, bufSize, "(not set)");
    } else {
        size_t len = strlen(passphrase);
        if (len <= 4) {
            snprintf(buf, bufSize, "****");
        } else {
            snprintf(
                buf,
                bufSize,
                "%c%c****%c%c",
                passphrase[0],
                passphrase[1],
                passphrase[len - 2],
                passphrase[len - 1]);
        }
    }
}

// Format boolean setting
/**
 * @brief Format a boolean setting.
 * @param value Non-zero for enabled.
 * @return "Enabled" or "Disabled".
 */
const char* FormatBoolSetting(u32 value) {
    return value != 0 ? "Enabled" : "Disabled";
}

// Format timeout value
/**
 * @brief Format a timeout in ms below one second, else in seconds.
 * @param timeout_ms Timeout in milliseconds.
 * @param[out] buf Receives the text.
 * @param bufSize Capacity of buf.
 */
void FormatTimeout(u32 timeout_ms, char* buf, size_t bufSize) {
    if (timeout_ms < 1000) {
        snprintf(buf, bufSize, "%u ms", timeout_ms);
    } else {
        snprintf(buf, bufSize, "%.1f s", timeout_ms / 1000.0);
    }
}

// Format debug level
/**
 * @brief Name a debug level.
 * @param level Level (0-3).
 * @return Its name, or "Unknown".
 */
const char* FormatDebugLevel(u32 level) {
    switch (level) {
        case 0:
            return "Error";
        case 1:
            return "Warning";
        case 2:
            return "Info";
        case 3:
            return "Verbose";
        default:
            return "Unknown";
    }
}

// Validate port number
/**
 * @brief Validate a port number.
 * @param port Port to check.
 * @return true when port is non-zero.
 */
bool IsValidPort(u16 port) {
    return port > 0;  // u16 is always <= 65535
}

// Validate timeout (reasonable range)
/**
 * @brief Validate a timeout.
 * @param timeout_ms Timeout in milliseconds.
 * @return true when within 100 ms to 5 minutes.
 */
bool IsValidTimeout(u32 timeout_ms) {
    return timeout_ms >= 100 && timeout_ms <= 300000;  // 100ms to 5 minutes
}

// Validate debug level
/**
 * @brief Validate a debug level.
 * @param level Level to check.
 * @return true when level is at most 3.
 */
bool IsValidDebugLevel(u32 level) {
    return level <= 3;
}

//=============================================================================
// Test Framework
//=============================================================================

/// Number of tests started.
static int g_tests_run = 0;
/// Number of tests that finished without a failed assertion.
static int g_tests_passed = 0;

/**
 * @brief Define a test body as a void function.
 * @param name Identifier of the test.
 */
#define TEST(name) void name()
/**
 * @brief Run a test and count it as passed.
 * @param name Identifier of the test to run.
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
 * @brief Abort the process when cond is false.
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
 * @param a First value.
 * @param b Second value.
 */
#define ASSERT_EQ(a, b) ASSERT((a) == (b))
/**
 * @brief Assert a != b.
 * @param a First value.
 * @param b Second value.
 */
#define ASSERT_NE(a, b) ASSERT((a) != (b))
/**
 * @brief Assert two C strings are equal.
 * @param a First string.
 * @param b Second string.
 */
#define ASSERT_STREQ(a, b) ASSERT(strcmp((a), (b)) == 0)
/**
 * @brief Assert a is true.
 * @param a Value expected to be true.
 */
#define ASSERT_TRUE(a) ASSERT((a) == true)
/**
 * @brief Assert a is false.
 * @param a Value expected to be false.
 */
#define ASSERT_FALSE(a) ASSERT((a) == false)

//=============================================================================
// Command ID Tests
//=============================================================================

/// @test Verify command ids are sequential.
TEST(command_ids_are_sequential) {
    ASSERT_EQ(IPC_CMD_GET_PASSPHRASE, 65011U);
    ASSERT_EQ(IPC_CMD_SET_PASSPHRASE, 65012U);
    ASSERT_EQ(IPC_CMD_GET_LDN_ENABLED, 65013U);
    ASSERT_EQ(IPC_CMD_SET_LDN_ENABLED, 65014U);
    ASSERT_EQ(IPC_CMD_SAVE_CONFIG, 65029U);
    ASSERT_EQ(IPC_CMD_RELOAD_CONFIG, 65030U);
}

/// @test Verify command ids no overlap with existing.
TEST(command_ids_no_overlap_with_existing) {
    // Ensure new commands don't overlap with existing ones
    ASSERT(IPC_CMD_GET_PASSPHRASE > IPC_CMD_GET_LAST_RTT);
}

/// @test Verify command ids paired get set.
TEST(command_ids_paired_get_set) {
    // Verify get/set commands are paired correctly
    ASSERT_EQ(IPC_CMD_SET_PASSPHRASE, IPC_CMD_GET_PASSPHRASE + 1);
    ASSERT_EQ(IPC_CMD_SET_LDN_ENABLED, IPC_CMD_GET_LDN_ENABLED + 1);
    ASSERT_EQ(IPC_CMD_SET_CONNECT_TIMEOUT, IPC_CMD_GET_CONNECT_TIMEOUT + 1);
    ASSERT_EQ(IPC_CMD_SET_PING_INTERVAL, IPC_CMD_GET_PING_INTERVAL + 1);
    ASSERT_EQ(IPC_CMD_SET_RECONNECT_DELAY, IPC_CMD_GET_RECONNECT_DELAY + 1);
    ASSERT_EQ(
        IPC_CMD_SET_MAX_RECONNECT_ATTEMPTS,
        IPC_CMD_GET_MAX_RECONNECT_ATTEMPTS + 1);
    ASSERT_EQ(IPC_CMD_SET_DEBUG_LEVEL, IPC_CMD_GET_DEBUG_LEVEL + 1);
}

//=============================================================================
// Structure Size Tests
//=============================================================================

/// @test Verify ipc passphrase size.
TEST(ipc_passphrase_size) {
    ASSERT_EQ(sizeof(IpcPassphrase), 64U);
}

/// @test Verify ipc server address size.
TEST(ipc_server_address_size) {
    ASSERT_EQ(sizeof(IpcServerAddress), 68U);
}

/// @test Verify ipc debug settings size.
TEST(ipc_debug_settings_size) {
    ASSERT_EQ(sizeof(IpcDebugSettings), 8U);
}

/// @test Verify structures are pod.
TEST(structures_are_pod) {
    // Verify structures are trivially copyable (POD-like)
    IpcPassphrase p1 = {};
    IpcPassphrase p2{};
    memcpy(&p2, &p1, sizeof(IpcPassphrase));
    ASSERT_EQ(p2.passphrase[0], '\0');
}

//=============================================================================
// Passphrase Format Tests
//=============================================================================

/// @test Verify passphrase empty shows not set.
TEST(passphrase_empty_shows_not_set) {
    char buf[32];
    FormatPassphraseMasked("", buf, sizeof(buf));
    ASSERT_STREQ(buf, "(not set)");
}

/// @test Verify passphrase null shows not set.
TEST(passphrase_null_shows_not_set) {
    char buf[32];
    FormatPassphraseMasked(nullptr, buf, sizeof(buf));
    ASSERT_STREQ(buf, "(not set)");
}

/// @test Verify passphrase short shows masked.
TEST(passphrase_short_shows_masked) {
    char buf[32];
    FormatPassphraseMasked("abc", buf, sizeof(buf));
    ASSERT_STREQ(buf, "****");
}

/// @test Verify passphrase four chars shows masked.
TEST(passphrase_four_chars_shows_masked) {
    char buf[32];
    FormatPassphraseMasked("test", buf, sizeof(buf));
    ASSERT_STREQ(buf, "****");
}

/// @test Verify passphrase long shows partial.
TEST(passphrase_long_shows_partial) {
    char buf[32];
    FormatPassphraseMasked("mySecretPass", buf, sizeof(buf));
    ASSERT_STREQ(buf, "my****ss");
}

/// @test Verify passphrase five chars shows partial.
TEST(passphrase_five_chars_shows_partial) {
    char buf[32];
    FormatPassphraseMasked("hello", buf, sizeof(buf));
    ASSERT_STREQ(buf, "he****lo");
}

//=============================================================================
// Boolean Format Tests
//=============================================================================

/// @test Verify bool enabled format.
TEST(bool_enabled_format) {
    ASSERT_STREQ(FormatBoolSetting(1), "Enabled");
    ASSERT_STREQ(FormatBoolSetting(42), "Enabled");  // Any non-zero
}

/// @test Verify bool disabled format.
TEST(bool_disabled_format) {
    ASSERT_STREQ(FormatBoolSetting(0), "Disabled");
}

//=============================================================================
// Timeout Format Tests
//=============================================================================

/// @test Verify timeout milliseconds.
TEST(timeout_milliseconds) {
    char buf[32];
    FormatTimeout(500, buf, sizeof(buf));
    ASSERT_STREQ(buf, "500 ms");
}

/// @test Verify timeout one second.
TEST(timeout_one_second) {
    char buf[32];
    FormatTimeout(1000, buf, sizeof(buf));
    ASSERT_STREQ(buf, "1.0 s");
}

/// @test Verify timeout seconds decimal.
TEST(timeout_seconds_decimal) {
    char buf[32];
    FormatTimeout(5500, buf, sizeof(buf));
    ASSERT_STREQ(buf, "5.5 s");
}

/// @test Verify timeout large value.
TEST(timeout_large_value) {
    char buf[32];
    FormatTimeout(30000, buf, sizeof(buf));
    ASSERT_STREQ(buf, "30.0 s");
}

/// @test Verify timeout 999ms.
TEST(timeout_999ms) {
    char buf[32];
    FormatTimeout(999, buf, sizeof(buf));
    ASSERT_STREQ(buf, "999 ms");
}

//=============================================================================
// Debug Level Format Tests
//=============================================================================

/// @test Verify debug level error.
TEST(debug_level_error) {
    ASSERT_STREQ(FormatDebugLevel(0), "Error");
}

/// @test Verify debug level warning.
TEST(debug_level_warning) {
    ASSERT_STREQ(FormatDebugLevel(1), "Warning");
}

/// @test Verify debug level info.
TEST(debug_level_info) {
    ASSERT_STREQ(FormatDebugLevel(2), "Info");
}

/// @test Verify debug level verbose.
TEST(debug_level_verbose) {
    ASSERT_STREQ(FormatDebugLevel(3), "Verbose");
}

/// @test Verify debug level unknown.
TEST(debug_level_unknown) {
    ASSERT_STREQ(FormatDebugLevel(4), "Unknown");
    ASSERT_STREQ(FormatDebugLevel(99), "Unknown");
}

//=============================================================================
// Validation Tests
//=============================================================================

/// @test Verify port valid range.
TEST(port_valid_range) {
    ASSERT_TRUE(IsValidPort(1));
    ASSERT_TRUE(IsValidPort(80));
    ASSERT_TRUE(IsValidPort(39990));
    ASSERT_TRUE(IsValidPort(65535));
}

/// @test Verify port invalid zero.
TEST(port_invalid_zero) {
    ASSERT_FALSE(IsValidPort(0));
}

/// @test Verify timeout valid range.
TEST(timeout_valid_range) {
    ASSERT_TRUE(IsValidTimeout(100));     // Min
    ASSERT_TRUE(IsValidTimeout(5000));    // Typical
    ASSERT_TRUE(IsValidTimeout(30000));   // 30s
    ASSERT_TRUE(IsValidTimeout(300000));  // 5 min max
}

/// @test Verify timeout invalid too small.
TEST(timeout_invalid_too_small) {
    ASSERT_FALSE(IsValidTimeout(0));
    ASSERT_FALSE(IsValidTimeout(50));
    ASSERT_FALSE(IsValidTimeout(99));
}

/// @test Verify timeout invalid too large.
TEST(timeout_invalid_too_large) {
    ASSERT_FALSE(IsValidTimeout(300001));
    ASSERT_FALSE(IsValidTimeout(600000));
}

/// @test Verify debug level valid.
TEST(debug_level_valid) {
    ASSERT_TRUE(IsValidDebugLevel(0));
    ASSERT_TRUE(IsValidDebugLevel(1));
    ASSERT_TRUE(IsValidDebugLevel(2));
    ASSERT_TRUE(IsValidDebugLevel(3));
}

/// @test Verify debug level invalid.
TEST(debug_level_invalid) {
    ASSERT_FALSE(IsValidDebugLevel(4));
    ASSERT_FALSE(IsValidDebugLevel(10));
    ASSERT_FALSE(IsValidDebugLevel(255));
}

//=============================================================================
// Config Result Tests
//=============================================================================

/// @test Verify config result values.
TEST(config_result_values) {
    ASSERT_EQ(static_cast<u32>(IpcConfigResult::Success), 0U);
    ASSERT_EQ(static_cast<u32>(IpcConfigResult::FileNotFound), 1U);
    ASSERT_EQ(static_cast<u32>(IpcConfigResult::ParseError), 2U);
    ASSERT_EQ(static_cast<u32>(IpcConfigResult::IoError), 3U);
    ASSERT_EQ(static_cast<u32>(IpcConfigResult::InvalidValue), 4U);
}

//=============================================================================
// Debug Settings Tests
//=============================================================================

/// @test Verify debug settings zero init.
TEST(debug_settings_zero_init) {
    IpcDebugSettings settings = {};
    ASSERT_EQ(settings.enabled, 0U);
    ASSERT_EQ(settings.level, 0U);
}

/// @test Verify debug settings assignment.
TEST(debug_settings_assignment) {
    IpcDebugSettings settings = {};
    settings.enabled = 1;
    settings.level = 2;
    ASSERT_EQ(settings.enabled, 1U);
    ASSERT_EQ(settings.level, 2U);
}

//=============================================================================
// Server Address Tests
//=============================================================================

/// @test Verify server address default port.
TEST(server_address_default_port) {
    IpcServerAddress addr = {};
    strncpy(addr.host, "localhost", sizeof(addr.host));
    addr.port = 39990;
    ASSERT_STREQ(addr.host, "localhost");
    ASSERT_EQ(addr.port, 39990U);
}

/// @test Verify server address long hostname.
TEST(server_address_long_hostname) {
    IpcServerAddress addr = {};
    const char* longHost = "very-long-hostname.subdomain.example.com";
    strncpy(addr.host, longHost, sizeof(addr.host) - 1);
    addr.host[sizeof(addr.host) - 1] = '\0';
    ASSERT(strlen(addr.host) < sizeof(addr.host));
}

/// @test Verify server address truncates too long.
TEST(server_address_truncates_too_long) {
    IpcServerAddress addr = {};
    // Try to copy a 70+ char hostname into 64-byte buffer
    const char* tooLong =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "a";
    strncpy(addr.host, tooLong, sizeof(addr.host) - 1);
    addr.host[sizeof(addr.host) - 1] = '\0';
    ASSERT_EQ(strlen(addr.host), 63U);
}

//=============================================================================
// Passphrase Structure Tests
//=============================================================================

/// @test Verify passphrase empty.
TEST(passphrase_empty) {
    IpcPassphrase p = {};
    ASSERT_EQ(p.passphrase[0], '\0');
}

/// @test Verify passphrase copy.
TEST(passphrase_copy) {
    IpcPassphrase p = {};
    strncpy(p.passphrase, "mysecret", sizeof(p.passphrase));
    ASSERT_STREQ(p.passphrase, "mysecret");
}

/// @test Verify passphrase max length.
TEST(passphrase_max_length) {
    IpcPassphrase p = {};
    // Fill with 63 chars + null terminator
    memset(p.passphrase, 'x', 63);
    p.passphrase[63] = '\0';
    ASSERT_EQ(strlen(p.passphrase), 63U);
}

//=============================================================================
// Main
//=============================================================================

/**
 * @brief Run every test and print the summary.
 * @return 0 when all tests passed (a failed assertion exits with 1).
 */
int main() {
    printf("\n========================================\n");
    printf("  IPC Config Tests - ryu_ldn_nx\n");
    printf("========================================\n\n");

    printf("--- Command ID Tests ---\n");
    RUN_TEST(command_ids_are_sequential);
    RUN_TEST(command_ids_no_overlap_with_existing);
    RUN_TEST(command_ids_paired_get_set);

    printf("\n--- Structure Size Tests ---\n");
    RUN_TEST(ipc_passphrase_size);
    RUN_TEST(ipc_server_address_size);
    RUN_TEST(ipc_debug_settings_size);
    RUN_TEST(structures_are_pod);

    printf("\n--- Passphrase Format Tests ---\n");
    RUN_TEST(passphrase_empty_shows_not_set);
    RUN_TEST(passphrase_null_shows_not_set);
    RUN_TEST(passphrase_short_shows_masked);
    RUN_TEST(passphrase_four_chars_shows_masked);
    RUN_TEST(passphrase_long_shows_partial);
    RUN_TEST(passphrase_five_chars_shows_partial);

    printf("\n--- Boolean Format Tests ---\n");
    RUN_TEST(bool_enabled_format);
    RUN_TEST(bool_disabled_format);

    printf("\n--- Timeout Format Tests ---\n");
    RUN_TEST(timeout_milliseconds);
    RUN_TEST(timeout_one_second);
    RUN_TEST(timeout_seconds_decimal);
    RUN_TEST(timeout_large_value);
    RUN_TEST(timeout_999ms);

    printf("\n--- Debug Level Format Tests ---\n");
    RUN_TEST(debug_level_error);
    RUN_TEST(debug_level_warning);
    RUN_TEST(debug_level_info);
    RUN_TEST(debug_level_verbose);
    RUN_TEST(debug_level_unknown);

    printf("\n--- Validation Tests ---\n");
    RUN_TEST(port_valid_range);
    RUN_TEST(port_invalid_zero);
    RUN_TEST(timeout_valid_range);
    RUN_TEST(timeout_invalid_too_small);
    RUN_TEST(timeout_invalid_too_large);
    RUN_TEST(debug_level_valid);
    RUN_TEST(debug_level_invalid);

    printf("\n--- Config Result Tests ---\n");
    RUN_TEST(config_result_values);

    printf("\n--- Debug Settings Tests ---\n");
    RUN_TEST(debug_settings_zero_init);
    RUN_TEST(debug_settings_assignment);

    printf("\n--- Server Address Tests ---\n");
    RUN_TEST(server_address_default_port);
    RUN_TEST(server_address_long_hostname);
    RUN_TEST(server_address_truncates_too_long);

    printf("\n--- Passphrase Structure Tests ---\n");
    RUN_TEST(passphrase_empty);
    RUN_TEST(passphrase_copy);
    RUN_TEST(passphrase_max_length);

    printf("\n========================================\n");
    printf("  Results: %d/%d passed\n", g_tests_passed, g_tests_run);
    printf("========================================\n\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
