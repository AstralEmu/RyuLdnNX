/**
 * @file log_tests.cpp
 * @brief Unit tests for Logging System
 *
 * Tests for debug logging functionality including log levels,
 * file output, and conditional logging based on configuration.
 */

#include "debug/log.hpp"
#include "config/config.hpp"

#include <cinttypes>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>

using ryu_ldn::config::Config;
using ryu_ldn::config::DebugConfig;
using ryu_ldn::config::get_default_config;
using ryu_ldn::debug::format_log_message;
using ryu_ldn::debug::log_level_to_string;
using ryu_ldn::debug::LogBuffer;
using ryu_ldn::debug::Logger;
using ryu_ldn::debug::LogLevel;
using ryu_ldn::debug::MAX_LOG_BUFFER_ENTRIES;

// ============================================================================
// Test Framework (minimal, no external dependencies)
// ============================================================================

/// Number of test cases started so far.
static int g_tests_run = 0;
/// Number of test cases that finished without a failed assertion.
static int g_tests_passed = 0;
/// Number of test cases that hit a failed assertion.
static int g_tests_failed = 0;

/// Defines a test function and registers it in the test table.
#define TEST(name)                             \
    static void test_##name();                 \
    static struct TestRegister_##name {        \
        TestRegister_##name() {                \
            register_test(#name, test_##name); \
        }                                      \
    } g_test_register_##name;                  \
    static void test_##name()

/// Fails the current test when the expression is false.
#define ASSERT_TRUE(cond)                                      \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
            throw std::runtime_error("Test assertion failed"); \
        }                                                      \
    } while (0)

/// Fails the current test when the expression is true.
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

/**
 * @brief Widen an integral value for PRId64 printing.
 * @tparam T integral or enum type of the value
 * @param v value to widen
 * @return v as long long
 */
template <typename T>
static int64_t assert_ll(T v) {
    return static_cast<int64_t>(v);
}

/// Fails the current test when the two values differ.
#define ASSERT_EQ(a, b)                                               \
    do {                                                              \
        auto _a = (a);                                                \
        auto _b = (b);                                                \
        if (_a != _b) {                                               \
            printf("  FAIL: %s == %s (line %d)\n", #a, #b, __LINE__); \
            printf(                                                   \
                "    got: %" PRId64 " vs %" PRId64 "\n",              \
                assert_ll(_a),                                        \
                assert_ll(_b));                                       \
            throw std::runtime_error("Test assertion failed");        \
        }                                                             \
    } while (0)

/// Fails the current test when the two C strings differ.
#define ASSERT_STREQ(a, b)                                            \
    do {                                                              \
        const char* _a = (a);                                         \
        const char* _b = (b);                                         \
        if (std::strcmp(_a, _b) != 0) {                               \
            printf("  FAIL: %s == %s (line %d)\n", #a, #b, __LINE__); \
            printf("    got: \"%s\" vs \"%s\"\n", _a, _b);            \
            throw std::runtime_error("Test assertion failed");        \
        }                                                             \
    } while (0)

/// One registered test case (name and body).
struct TestEntry {
    /// Test name printed by main().
    const char* name;
    /// Test body; throws on a failed assertion.
    void (*func)();
};

/// Table of registered test cases.
static TestEntry g_tests[128];
/// Number of entries used in the test table.
static int g_test_count = 0;

/**
 * @brief Appends a test case to the test table.
 *
 * @param name test name printed by main()
 * @param func test body; throws on a failed assertion
 */
static void register_test(const char* name, void (*func)()) {
    if (g_test_count < 128) {
        g_tests[g_test_count++] = {.name = name, .func = func};
    }
}

// ============================================================================
// Log Level Tests
// ============================================================================

/** @brief Test case: log level to string. */
TEST(log_level_to_string) {
    ASSERT_STREQ(log_level_to_string(LogLevel::Error), "ERROR");
    ASSERT_STREQ(log_level_to_string(LogLevel::Warning), "WARN");
    ASSERT_STREQ(log_level_to_string(LogLevel::Info), "INFO");
    ASSERT_STREQ(log_level_to_string(LogLevel::Verbose), "VERBOSE");
}

/** @brief Test case: log level from config. */
TEST(log_level_from_config) {
    ASSERT_EQ(static_cast<uint32_t>(LogLevel::Error), 0U);
    ASSERT_EQ(static_cast<uint32_t>(LogLevel::Warning), 1U);
    ASSERT_EQ(static_cast<uint32_t>(LogLevel::Info), 2U);
    ASSERT_EQ(static_cast<uint32_t>(LogLevel::Verbose), 3U);
}

// ============================================================================
// Logger Initialization Tests
// ============================================================================

/** @brief Test case: logger init disabled. */
TEST(logger_init_disabled) {
    DebugConfig config{};
    config.enabled = false;
    config.level = 3;

    Logger logger;
    logger.init(config);

    ASSERT_FALSE(logger.is_enabled());
}

/** @brief Test case: logger init enabled. */
TEST(logger_init_enabled) {
    DebugConfig config{};
    config.enabled = true;
    config.level = 2;

    Logger logger;
    logger.init(config);

    ASSERT_TRUE(logger.is_enabled());
    ASSERT_EQ(static_cast<uint32_t>(logger.get_level()), 2U);
}

/** @brief Test case: logger should log level filtering. */
TEST(logger_should_log_level_filtering) {
    DebugConfig config{};
    config.enabled = true;
    config.level = 1;  // Warning level

    Logger logger;
    logger.init(config);

    // Should log Error (0) and Warning (1)
    ASSERT_TRUE(logger.should_log(LogLevel::Error));
    ASSERT_TRUE(logger.should_log(LogLevel::Warning));
    // Should NOT log Info (2) or Verbose (3)
    ASSERT_FALSE(logger.should_log(LogLevel::Info));
    ASSERT_FALSE(logger.should_log(LogLevel::Verbose));
}

/** @brief Test case: logger should log all levels. */
TEST(logger_should_log_all_levels) {
    DebugConfig config{};
    config.enabled = true;
    config.level = 3;  // Verbose (all levels)

    Logger logger;
    logger.init(config);

    ASSERT_TRUE(logger.should_log(LogLevel::Error));
    ASSERT_TRUE(logger.should_log(LogLevel::Warning));
    ASSERT_TRUE(logger.should_log(LogLevel::Info));
    ASSERT_TRUE(logger.should_log(LogLevel::Verbose));
}

/** @brief Test case: logger disabled never logs. */
TEST(logger_disabled_never_logs) {
    DebugConfig config{};
    config.enabled = false;
    config.level = 3;  // Even at verbose

    Logger logger;
    logger.init(config);

    ASSERT_FALSE(logger.should_log(LogLevel::Error));
    ASSERT_FALSE(logger.should_log(LogLevel::Warning));
    ASSERT_FALSE(logger.should_log(LogLevel::Info));
    ASSERT_FALSE(logger.should_log(LogLevel::Verbose));
}

// ============================================================================
// Log Message Formatting Tests
// ============================================================================

/** @brief Test case: format log message basic. */
TEST(format_log_message_basic) {
    char buffer[256];
    format_log_message(buffer, sizeof(buffer), LogLevel::Info, "Test message");

    // Should contain level and message
    ASSERT_TRUE(strstr(buffer, "[INFO]") != nullptr);
    ASSERT_TRUE(strstr(buffer, "Test message") != nullptr);
}

/** @brief Test case: format log message with args. */
TEST(format_log_message_with_args) {
    char buffer[256];
    format_log_message(
        buffer, sizeof(buffer), LogLevel::Error, "Error code: %d", 42);

    ASSERT_TRUE(strstr(buffer, "[ERROR]") != nullptr);
    ASSERT_TRUE(strstr(buffer, "Error code: 42") != nullptr);
}

/** @brief Test case: format log message truncation. */
TEST(format_log_message_truncation) {
    char buffer[32];
    format_log_message(
        buffer,
        sizeof(buffer),
        LogLevel::Info,
        "This is a very long message that should be truncated");

    // Buffer should be null-terminated and not overflow
    ASSERT_TRUE(strlen(buffer) < sizeof(buffer));
    const bool null_terminated =
        buffer[sizeof(buffer) - 1] == '\0' || strlen(buffer) < sizeof(buffer);
    ASSERT_TRUE(null_terminated);
}

// ============================================================================
// File Logging Tests
// ============================================================================

/** @brief Test case: logger file output enabled. */
TEST(logger_file_output_enabled) {
    char log_path[256];
    snprintf(log_path, sizeof(log_path), "/tmp/test_log_%d.log", rand());

    // Remove if exists
    std::remove(log_path);

    DebugConfig config{};
    config.enabled = true;
    config.level = 3;

    Logger logger;
    logger.init(config, log_path);

    // Log a message
    logger.log(LogLevel::Info, "Test file logging");
    logger.flush();

    // Verify file was created and contains our message
    std::ifstream f(log_path);
    ASSERT_TRUE(f.is_open());

    std::string content(
        (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    ASSERT_TRUE(content.find("Test file logging") != std::string::npos);

    std::remove(log_path);
}

/** @brief Test case: logger file output disabled. */
TEST(logger_file_output_disabled) {
    char log_path[256];
    snprintf(
        log_path, sizeof(log_path), "/tmp/test_log_disabled_%d.log", rand());

    // Remove if exists
    std::remove(log_path);

    DebugConfig config{};
    config.enabled = false;
    config.level = 3;
    // Debug disabled = no file output

    Logger logger;
    logger.init(config, log_path);

    // Log a message
    logger.log(LogLevel::Info, "This should not be logged");

    // File should NOT exist
    std::ifstream f(log_path);
    ASSERT_FALSE(f.is_open());
}

// ============================================================================
// Log Buffer Tests
// ============================================================================

/** @brief Test case: log buffer stores messages. */
TEST(log_buffer_stores_messages) {
    LogBuffer buffer;
    buffer.init(5);  // Capacity of 5 messages

    buffer.add("Message 1");
    buffer.add("Message 2");
    buffer.add("Message 3");

    ASSERT_EQ(buffer.count(), 3U);
}

/** @brief Test case: log buffer circular. */
TEST(log_buffer_circular) {
    LogBuffer buffer;
    buffer.init(3);  // Small capacity

    buffer.add("Message 1");
    buffer.add("Message 2");
    buffer.add("Message 3");
    buffer.add("Message 4");  // Should overwrite Message 1

    ASSERT_EQ(buffer.count(), 3U);

    // First message should now be Message 2
    const char* first = buffer.get(0);
    ASSERT_TRUE(first != nullptr);
    ASSERT_TRUE(strstr(first, "Message 2") != nullptr);
}

/** @brief Test case: log buffer get all. */
TEST(log_buffer_get_all) {
    LogBuffer buffer;
    buffer.init(10);

    buffer.add("Line 1");
    buffer.add("Line 2");
    buffer.add("Line 3");

    char output[512];
    buffer.get_all(output, sizeof(output));

    ASSERT_TRUE(strstr(output, "Line 1") != nullptr);
    ASSERT_TRUE(strstr(output, "Line 2") != nullptr);
    ASSERT_TRUE(strstr(output, "Line 3") != nullptr);
}

/** @brief Test case: get_all ignores a null or empty destination. */
TEST(log_buffer_get_all_rejects_unusable_destination) {
    LogBuffer buffer;
    buffer.init(4);
    buffer.add("Line 1");

    char output[8] = "keep";
    buffer.get_all(nullptr, sizeof(output));
    buffer.get_all(output, 0);

    ASSERT_STREQ(output, "keep");
}

/**
 * @brief Forward a printf-style message to Logger::log_v.
 * @param logger Logger under test
 * @param level Severity of the message
 * @param format printf-style format string
 */
static void call_log_v(
    Logger& logger, LogLevel level, const char* format, ...) {
    va_list args;
    va_start(args, format);
    logger.log_v(level, format, args);
    va_end(args);
}

/** @brief Test case: log_v itself drops a message below the level. */
TEST(logger_log_v_drops_message_below_level) {
    DebugConfig config{};
    config.enabled = true;
    config.level = 0;  // Error only

    Logger logger;
    logger.init(config);
    call_log_v(logger, LogLevel::Verbose, "must not appear %d", 1);

    ASSERT_EQ(logger.get_buffer().count(), 0U);
}

/** @brief Test case: a message below the level is dropped everywhere. */
TEST(logger_log_drops_message_below_level) {
    DebugConfig config{};
    config.enabled = true;
    config.level = 0;  // Error only

    char log_path[256];
    snprintf(log_path, sizeof(log_path), "/tmp/test_log_%d.log", rand());
    std::remove(log_path);

    Logger logger;
    logger.init(config, log_path);
    logger.log(LogLevel::Verbose, "must not appear");

    ASSERT_EQ(logger.get_buffer().count(), 0U);
    std::remove(log_path);
}

/** @brief Test case: log buffer clear. */
TEST(log_buffer_clear) {
    LogBuffer buffer;
    buffer.init(5);

    buffer.add("Message 1");
    buffer.add("Message 2");
    ASSERT_EQ(buffer.count(), 2U);

    buffer.clear();
    ASSERT_EQ(buffer.count(), 0U);
}

// ============================================================================
// Integration with Config Tests
// ============================================================================

/** @brief Test case: logger from full config. */
TEST(logger_from_full_config) {
    Config config = get_default_config();
    config.debug.enabled = true;
    config.debug.level = 2;

    Logger logger;
    logger.init(config.debug);

    ASSERT_TRUE(logger.is_enabled());
    ASSERT_EQ(static_cast<uint32_t>(logger.get_level()), 2U);
    ASSERT_TRUE(logger.should_log(LogLevel::Error));
    ASSERT_TRUE(logger.should_log(LogLevel::Warning));
    ASSERT_TRUE(logger.should_log(LogLevel::Info));
    ASSERT_FALSE(logger.should_log(LogLevel::Verbose));
}

/** @brief Test case: logger default config disabled. */
TEST(logger_default_config_disabled) {
    Config config = get_default_config();
    // Default should have debug disabled

    Logger logger;
    logger.init(config.debug);

    ASSERT_FALSE(logger.is_enabled());
}

// ============================================================================
// Log Buffer — Additional Coverage
// ============================================================================

/** @brief Test case: log buffer init capacity clamp. */
TEST(log_buffer_init_capacity_clamp) {
    LogBuffer buffer;
    buffer.init(MAX_LOG_BUFFER_ENTRIES + 100);  // Exceeds max

    // Should be clamped to MAX_LOG_BUFFER_ENTRIES
    ASSERT_EQ(buffer.count(), 0U);
}

/** @brief Test case: log buffer init small capacity. */
TEST(log_buffer_init_small_capacity) {
    LogBuffer buffer;
    buffer.init(2);

    buffer.add("A");
    buffer.add("B");
    buffer.add("C");  // Should overwrite A

    ASSERT_EQ(buffer.count(), 2U);
}

/** @brief Test case: log buffer get by index. */
TEST(log_buffer_get_by_index) {
    LogBuffer buffer;
    buffer.init(10);

    buffer.add("First");
    buffer.add("Second");
    buffer.add("Third");

    const char* msg0 = buffer.get(0);
    const char* msg1 = buffer.get(1);
    const char* msg2 = buffer.get(2);
    const char* msg_invalid = buffer.get(99);

    ASSERT_TRUE(msg0 != nullptr);
    ASSERT_TRUE(strstr(msg0, "First") != nullptr);
    ASSERT_TRUE(msg1 != nullptr);
    ASSERT_TRUE(strstr(msg1, "Second") != nullptr);
    ASSERT_TRUE(msg2 != nullptr);
    ASSERT_TRUE(strstr(msg2, "Third") != nullptr);
    ASSERT_TRUE(msg_invalid == nullptr);
}

/** @brief Test case: log buffer get empty. */
TEST(log_buffer_get_empty) {
    LogBuffer buffer;
    buffer.init(10);

    const char* msg = buffer.get(0);
    ASSERT_TRUE(msg == nullptr);
}

/** @brief Test case: log buffer add null ignored. */
TEST(log_buffer_add_null_ignored) {
    LogBuffer buffer;
    buffer.init(10);

    buffer.add(nullptr);
    ASSERT_EQ(buffer.count(), 0U);
}

// ============================================================================
// Logger — Additional Coverage
// ============================================================================

/** @brief Test case: logger flush without file. */
TEST(logger_flush_without_file) {
    Logger logger;
    DebugConfig cfg{};
    cfg.enabled = true;
    cfg.level = 3;

    logger.init(cfg);

    // flush() without open file should not crash
    logger.flush();
}

/** @brief Test case: logger flush with file. */
TEST(logger_flush_with_file) {
    Logger logger;
    DebugConfig cfg{};
    cfg.enabled = true;
    cfg.level = 3;

    char path[256];
    std::snprintf(path, sizeof(path), "/tmp/test_flush_%d.log", rand());
    logger.init(cfg, path);

    logger.log(LogLevel::Info, "Flush test");
    logger.flush();

    // Clean up
    std::remove(path);
}

/** @brief Test case: logger check idle timeout no file. */
TEST(logger_check_idle_timeout_no_file) {
    Logger logger;
    DebugConfig cfg{};
    cfg.enabled = true;
    cfg.level = 3;

    logger.init(cfg);

    // check_idle_timeout without open file should not crash
    logger.check_idle_timeout();
}

/** @brief Test case: logger check idle timeout with file. */
TEST(logger_check_idle_timeout_with_file) {
    Logger logger;
    DebugConfig cfg{};
    cfg.enabled = true;
    cfg.level = 3;

    char path[256];
    std::snprintf(path, sizeof(path), "/tmp/test_idle_%d.log", rand());
    logger.init(cfg, path);

    logger.log(LogLevel::Info, "Test message");
    // On host, check_idle_timeout closes the file
    logger.check_idle_timeout();

    // Clean up
    std::remove(path);
}

/** @brief Test case: log level to string unknown. */
TEST(log_level_to_string_unknown) {
    // bit_cast builds the out-of-enumerator level without a cast the
    // EnumCastOutOfRange check would reject; the enum type itself accepts any
    // value of its fixed underlying type.
    const auto unknown_level = std::bit_cast<LogLevel>(99U);
    ASSERT_TRUE(
        std::strcmp(log_level_to_string(unknown_level), "UNKNOWN") == 0);
}

/**
 * @brief Test case: logger init with explicit null path uses the default path.
 */
TEST(logger_init_with_explicit_null_path_uses_the_default_path) {
    DebugConfig config{};
    config.enabled = false;  // disabled: init must not touch the SD-card path
    config.level = 2;

    Logger logger;
    logger.init(config, nullptr);

    ASSERT_FALSE(logger.is_enabled());
    ASSERT_EQ(static_cast<uint32_t>(logger.get_level()), 2U);
}

/**
 * @brief Test case: format log message truncates when the prefix does not fit.
 */
TEST(format_log_message_truncates_when_the_prefix_does_not_fit) {
    char tiny[4];
    std::memset(tiny, 'x', sizeof(tiny));
    format_log_message(tiny, sizeof(tiny), LogLevel::Info, "message %d", 42);
    ASSERT_EQ(tiny[sizeof(tiny) - 1], '\0');
    ASSERT_TRUE(std::strlen(tiny) < sizeof(tiny));
}

// ============================================================================
// Main
// ============================================================================

/**
 * @brief Runs the whole suite; the exit code is non-zero if any test failed.
 *
 * @return 0 when every test passed, non-zero otherwise
 */
int main() {
    printf("=== ryu_ldn_nx Logging Unit Tests ===\n\n");
    printf("Running %d tests...\n\n", g_test_count);

    for (int i = 0; i < g_test_count; i++) {
        g_tests_run++;
        printf("[%d/%d] %s...", i + 1, g_test_count, g_tests[i].name);
        fflush(stdout);

        try {
            g_tests[i].func();
            printf(" OK\n");
            g_tests_passed++;
        } catch (...) {
            g_tests_failed++;
        }
    }

    printf("\n========================================\n");
    printf(
        "Results: %d passed, %d failed, %d total\n",
        g_tests_passed,
        g_tests_failed,
        g_tests_run);

    if (g_tests_failed == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("FAILED\n");
    }

    return g_tests_failed > 0 ? 1 : 0;
}
