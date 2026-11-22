/**
 * @file config_tests.cpp
 * @brief Unit tests for Configuration Manager
 *
 * Tests for parsing INI config files and default values.
 */

#include "config/config.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>

using namespace ryu_ldn::config;

// ============================================================================
// Test Framework (minimal, no external dependencies)
// ============================================================================

/// Number of registered tests that were executed.
static int g_tests_run = 0;
/// Number of tests that returned without a failed assertion.
static int g_tests_passed = 0;
/// Number of tests aborted by a failed assertion.
static int g_tests_failed = 0;

/**
 * @brief Define a test and register it for main() to run.
 *
 * Declares test_<name>() and a static object whose constructor registers it
 * through register_test(); the body follows the macro.
 *
 * @param name Identifier of the test (becomes test_<name>).
 */
#define TEST(name)                             \
    static void test_##name();                 \
    static struct TestRegister_##name {        \
        TestRegister_##name() {                \
            register_test(#name, test_##name); \
        }                                      \
    } g_test_register_##name;                  \
    static void test_##name()

/**
 * @brief Abort the current test by exception when cond is false.
 * @param cond Expression expected to be true.
 */
#define ASSERT_TRUE(cond)                                      \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
            throw std::runtime_error("Test assertion failed"); \
        }                                                      \
    } while (0)

/**
 * @brief Abort the current test by exception when cond is true.
 * @param cond Expression expected to be false.
 */
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

/**
 * @brief Abort the current test by exception when a != b.
 * @param a Actual value (integral).
 * @param b Expected value (integral).
 */
#define ASSERT_EQ(a, b)                                                      \
    do {                                                                     \
        auto _a = (a);                                                       \
        auto _b = (b);                                                       \
        if (_a != _b) {                                                      \
            printf("  FAIL: %s == %s (line %d)\n", #a, #b, __LINE__);        \
            printf("    got: %lld vs %lld\n", (long long)_a, (long long)_b); \
            throw std::runtime_error("Test assertion failed");               \
        }                                                                    \
    } while (0)

/**
 * @brief Abort the current test by exception when the C strings differ.
 * @param a Actual NUL-terminated string.
 * @param b Expected NUL-terminated string.
 */
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

/// A registered test: its name and its body.
struct TestEntry {
    /// Test name as printed in the run log.
    const char* name;
    /// Test body.
    void (*func)();
};

/// Registered tests, in registration order.
static TestEntry g_tests[128];
/// Number of valid entries in g_tests.
static int g_test_count = 0;

/**
 * @brief Add a test to g_tests (silently ignored once 128 are registered).
 * @param name Test name.
 * @param func Test body.
 */
static void register_test(const char* name, void (*func)()) {
    if (g_test_count < 128) {
        g_tests[g_test_count++] = {name, func};
    }
}

// Helper to create temp config file
/**
 * @brief RAII temporary INI file under /tmp, removed on destruction.
 */
class TempConfigFile {
public:
    /**
     * @brief Create the file with the given content.
     * @param content Text written to the file.
     */
    TempConfigFile(const char* content) {
        snprintf(m_path, sizeof(m_path), "/tmp/test_config_%d.ini", rand());
        std::ofstream f(m_path);
        if (f.is_open()) {
            f << content;
            f.close();
        }
    }

    ~TempConfigFile() {
        std::remove(m_path);
    }

    /**
     * @brief Get the path of the temporary file.
     * @return NUL-terminated path, valid for the object's lifetime.
     */
    const char* path() const {
        return m_path;
    }

private:
    /// Path of the temporary file.
    char m_path[256];
};

// ============================================================================
// Default Values Tests
// ============================================================================

/**
 * @brief get_default_config() returns the documented defaults.
 */
TEST(default_values) {
    Config config = get_default_config();

    // Server defaults
    ASSERT_STREQ(config.server.host, "ryuldnnx.ddns.net");
    ASSERT_EQ(config.server.port, 30456);

    // LDN defaults
    ASSERT_EQ(config.ldn.enabled, true);
    ASSERT_STREQ(config.ldn.passphrase, "");

    // Debug defaults
    ASSERT_EQ(config.debug.enabled, false);
    ASSERT_EQ(config.debug.level, 1u);

    // LDN passphrase filtering default
    ASSERT_EQ(config.ldn.use_passphrase, false);

    // P2P proxy default (disabled by default)
    ASSERT_EQ(config.ldn.disable_p2p, true);
}

// ============================================================================
// Parse Tests
// ============================================================================

/**
 * @brief An empty file loads successfully and keeps the defaults.
 */
TEST(parse_empty_file) {
    TempConfigFile file("");
    Config config = get_default_config();
    ConfigResult result = load_config(file.path(), config);

    ASSERT_EQ(result, ConfigResult::Success);
    // Should have defaults
    ASSERT_STREQ(config.server.host, "ryuldnnx.ddns.net");
}

/**
 * @brief The [server] host and port are parsed.
 */
TEST(parse_server_section) {
    const char* content =
        "[server]\n"
        "host = 192.168.1.100\n"
        "port = 12345\n";

    TempConfigFile file(content);
    Config config = get_default_config();
    ConfigResult result = load_config(file.path(), config);

    ASSERT_EQ(result, ConfigResult::Success);
    ASSERT_STREQ(config.server.host, "192.168.1.100");
    ASSERT_EQ(config.server.port, 12345);
}

/**
 * @brief The [ldn] flags and passphrase are parsed.
 */
TEST(parse_ldn_section) {
    const char* content =
        "[ldn]\n"
        "enabled = 0\n"
        "use_passphrase = 1\n"
        "passphrase = secret123\n"
        "disable_p2p = 0\n";

    TempConfigFile file(content);
    Config config = get_default_config();
    ConfigResult result = load_config(file.path(), config);

    ASSERT_EQ(result, ConfigResult::Success);
    ASSERT_EQ(config.ldn.enabled, false);
    ASSERT_EQ(config.ldn.use_passphrase, true);
    ASSERT_STREQ(config.ldn.passphrase, "secret123");
    ASSERT_EQ(config.ldn.disable_p2p, false);
}

/**
 * @brief The [debug] enabled flag and level are parsed.
 */
TEST(parse_debug_section) {
    const char* content =
        "[debug]\n"
        "enabled = 1\n"
        "level = 3\n";

    TempConfigFile file(content);
    Config config = get_default_config();
    ConfigResult result = load_config(file.path(), config);

    ASSERT_EQ(result, ConfigResult::Success);
    ASSERT_EQ(config.debug.enabled, true);
    ASSERT_EQ(config.debug.level, 3u);
}

/**
 * @brief Lines starting with ';' (even indented) are ignored.
 */
TEST(parse_comments_ignored) {
    const char* content =
        "; This is a comment\n"
        "[server]\n"
        "; Another comment\n"
        "host = test.server.com\n"
        "  ; Indented comment\n"
        "port = 9999\n";

    TempConfigFile file(content);
    Config config = get_default_config();
    ConfigResult result = load_config(file.path(), config);

    ASSERT_EQ(result, ConfigResult::Success);
    ASSERT_STREQ(config.server.host, "test.server.com");
    ASSERT_EQ(config.server.port, 9999);
}

/**
 * @brief Spaces around keys and values are trimmed; none are required.
 */
TEST(parse_whitespace_handling) {
    const char* content =
        "[server]\n"
        "  host   =   spaced.server.com  \n"
        "port=12345\n";  // No spaces

    TempConfigFile file(content);
    Config config = get_default_config();
    ConfigResult result = load_config(file.path(), config);

    ASSERT_EQ(result, ConfigResult::Success);
    ASSERT_STREQ(config.server.host, "spaced.server.com");
    ASSERT_EQ(config.server.port, 12345);
}

/**
 * @brief An unknown section is skipped and later sections still load.
 */
TEST(parse_unknown_section_ignored) {
    const char* content =
        "[unknown]\n"
        "foo = bar\n"
        "[server]\n"
        "port = 11111\n";

    TempConfigFile file(content);
    Config config = get_default_config();
    ConfigResult result = load_config(file.path(), config);

    ASSERT_EQ(result, ConfigResult::Success);
    ASSERT_EQ(config.server.port, 11111);
}

/**
 * @brief An unknown key is skipped and later keys still load.
 */
TEST(parse_unknown_key_ignored) {
    const char* content =
        "[server]\n"
        "unknown_key = value\n"
        "port = 22222\n";

    TempConfigFile file(content);
    Config config = get_default_config();
    ConfigResult result = load_config(file.path(), config);

    ASSERT_EQ(result, ConfigResult::Success);
    ASSERT_EQ(config.server.port, 22222);
}

/**
 * @brief A missing file reports FileNotFound and leaves the defaults.
 */
TEST(file_not_found) {
    Config config = get_default_config();
    ConfigResult result = load_config("/nonexistent/path/config.ini", config);

    ASSERT_EQ(result, ConfigResult::FileNotFound);
    // Config should still have defaults
    ASSERT_STREQ(config.server.host, "ryuldnnx.ddns.net");
}

/**
 * @brief A passphrase longer than 64 characters is truncated to 64.
 */
TEST(passphrase_truncated) {
    // Passphrase > 64 chars should be truncated
    const char* content =
        "[ldn]\n"
        "passphrase = "
        "1234567890123456789012345678901234567890123456789012345678901234567890"
        "1234567890\n";

    TempConfigFile file(content);
    Config config = get_default_config();
    ConfigResult result = load_config(file.path(), config);

    ASSERT_EQ(result, ConfigResult::Success);
    ASSERT_EQ(strlen(config.ldn.passphrase), 64u);
}

/**
 * @brief A host longer than 128 characters is truncated to 128.
 */
TEST(host_truncated) {
    // Host > 128 chars should be truncated
    char long_host[200];
    memset(long_host, 'a', sizeof(long_host) - 1);
    long_host[sizeof(long_host) - 1] = '\0';

    char content[512];
    snprintf(content, sizeof(content), "[server]\nhost = %s\n", long_host);

    TempConfigFile file(content);
    Config config = get_default_config();
    ConfigResult result = load_config(file.path(), config);

    ASSERT_EQ(result, ConfigResult::Success);
    ASSERT_EQ(strlen(config.server.host), 128u);
}

/**
 * @brief A file with all three sections loads every value.
 */
TEST(full_config_example) {
    const char* content =
        "; Full example\n"
        "[server]\n"
        "host = custom.ldn.server\n"
        "port = 30000\n"
        "\n"
        "[ldn]\n"
        "enabled = 1\n"
        "use_passphrase = 0\n"
        "passphrase = myroom\n"
        "disable_p2p = 0\n"
        "\n"
        "[debug]\n"
        "enabled = 1\n"
        "level = 2\n";

    TempConfigFile file(content);
    Config config = get_default_config();
    ConfigResult result = load_config(file.path(), config);

    ASSERT_EQ(result, ConfigResult::Success);
    ASSERT_STREQ(config.server.host, "custom.ldn.server");
    ASSERT_EQ(config.server.port, 30000);
    ASSERT_EQ(config.ldn.enabled, true);
    ASSERT_STREQ(config.ldn.passphrase, "myroom");
    ASSERT_EQ(config.ldn.use_passphrase, false);
    ASSERT_EQ(config.ldn.disable_p2p, false);
    ASSERT_EQ(config.debug.enabled, true);
    ASSERT_EQ(config.debug.level, 2u);
}

// ============================================================================
// Save Config Tests
// ============================================================================

/**
 * @brief save_config() writes a file that load_config() reads back.
 */
TEST(save_config_creates_file) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/test_save_config_%d.ini", rand());

    Config config = get_default_config();
    ConfigResult result = save_config(path, config);

    ASSERT_EQ(result, ConfigResult::Success);

    // Verify file exists and can be loaded back
    Config loaded = get_default_config();
    loaded.server.port = 0;  // Change to verify it gets loaded
    result = load_config(path, loaded);

    ASSERT_EQ(result, ConfigResult::Success);
    ASSERT_EQ(loaded.server.port, DEFAULT_PORT);

    std::remove(path);
}

/**
 * @brief Every modified setting survives a save and reload.
 */
TEST(save_config_preserves_values) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/test_save_preserve_%d.ini", rand());

    Config config = get_default_config();
    // Modify some values
    strcpy(config.server.host, "test.example.com");
    config.server.port = 12345;
    config.ldn.enabled = false;
    config.ldn.use_passphrase = true;
    strcpy(config.ldn.passphrase, "testpass");
    config.ldn.disable_p2p = false;
    config.debug.enabled = true;
    config.debug.level = 3;

    ConfigResult result = save_config(path, config);
    ASSERT_EQ(result, ConfigResult::Success);

    // Load it back
    Config loaded = get_default_config();
    result = load_config(path, loaded);

    ASSERT_EQ(result, ConfigResult::Success);
    ASSERT_STREQ(loaded.server.host, "test.example.com");
    ASSERT_EQ(loaded.server.port, 12345);
    ASSERT_EQ(loaded.ldn.enabled, false);
    ASSERT_STREQ(loaded.ldn.passphrase, "testpass");
    ASSERT_EQ(loaded.ldn.use_passphrase, true);
    ASSERT_EQ(loaded.ldn.disable_p2p, false);
    ASSERT_EQ(loaded.debug.enabled, true);
    ASSERT_EQ(loaded.debug.level, 3u);

    std::remove(path);
}

// ============================================================================
// Ensure Config Exists Tests
// ============================================================================

/**
 * @brief ensure_config_exists() creates a missing file with the defaults.
 */
TEST(ensure_config_exists_creates_new) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/test_ensure_%d.ini", rand());

    // Make sure file doesn't exist
    std::remove(path);

    ConfigResult result = ensure_config_exists(path);
    ASSERT_EQ(result, ConfigResult::Success);

    // Verify file was created with defaults
    Config loaded = get_default_config();
    loaded.server.port = 0;  // Change to verify
    result = load_config(path, loaded);

    ASSERT_EQ(result, ConfigResult::Success);
    ASSERT_EQ(loaded.server.port, DEFAULT_PORT);
    ASSERT_STREQ(loaded.server.host, DEFAULT_HOST);

    std::remove(path);
}

/**
 * @brief ensure_config_exists() does not overwrite an existing file.
 */
TEST(ensure_config_exists_keeps_existing) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/test_ensure_existing_%d.ini", rand());

    // Create a config with custom values
    const char* content =
        "[server]\n"
        "host = custom.host.com\n"
        "port = 54321\n";

    std::ofstream f(path);
    f << content;
    f.close();

    // ensure_config_exists should not overwrite
    ConfigResult result = ensure_config_exists(path);
    ASSERT_EQ(result, ConfigResult::Success);

    // Verify original values are preserved
    Config loaded = get_default_config();
    result = load_config(path, loaded);

    ASSERT_EQ(result, ConfigResult::Success);
    ASSERT_STREQ(loaded.server.host, "custom.host.com");
    ASSERT_EQ(loaded.server.port, 54321);

    std::remove(path);
}

// ============================================================================
// Value parsing: booleans, integers, ports, unknown sections, save failure
// ============================================================================

// Loads `[ldn]\nenabled = <value>` over a config whose `enabled` is the
// opposite of `expected`, so the assertion proves the parser actually decided.
/**
 * @brief Assert how the parser reads an [ldn] enabled value as a boolean.
 * @param value Text written after "enabled =".
 * @param expected Boolean the parser must produce.
 */
static void expect_bool(const char* value, bool expected) {
    char content[128];
    snprintf(content, sizeof(content), "[ldn]\nenabled = %s\n", value);
    TempConfigFile file(content);
    Config config = get_default_config();
    config.ldn.enabled = !expected;
    ASSERT_EQ(load_config(file.path(), config), ConfigResult::Success);
    ASSERT_EQ(config.ldn.enabled, expected);
}

/**
 * @brief config_result_to_string() names every ConfigResult, and Unknown
 * otherwise.
 */
TEST(every_config_result_has_its_own_name) {
    using ryu_ldn::config::config_result_to_string;
    using ryu_ldn::config::ConfigResult;
    ASSERT_STREQ(config_result_to_string(ConfigResult::Success), "Success");
    ASSERT_STREQ(
        config_result_to_string(ConfigResult::FileNotFound), "FileNotFound");
    ASSERT_STREQ(
        config_result_to_string(ConfigResult::ParseError), "ParseError");
    ASSERT_STREQ(config_result_to_string(ConfigResult::IoError), "IoError");
    ASSERT_STREQ(
        config_result_to_string(static_cast<ConfigResult>(99)), "Unknown");
}

/**
 * @brief Booleans accept 1, true (any case), 0 and false (any case).
 */
TEST(parse_bool_accepts_only_the_four_documented_spellings) {
    expect_bool("1", true);
    expect_bool("true", true);
    expect_bool("TrUe", true);
    expect_bool("0", false);
    expect_bool("false", false);
    expect_bool("FALSE", false);
}

/**
 * @brief Any other boolean spelling, including prefixes and an empty value,
 * reads as false.
 */
TEST(parse_bool_rejects_anything_else_as_false) {
    expect_bool("yes", false);
    expect_bool("on", false);
    expect_bool("2", false);
    expect_bool("tru", false);    // prefix of a valid token
    expect_bool("truee", false);  // valid token plus a trailing character
    expect_bool("", false);       // empty value
}

// Loads `[debug]\nlevel = <value>` over a config whose level is 77.
/**
 * @brief Load a [debug] level value and return what the parser stored.
 * @param value Text written after "level =".
 * @return The resulting debug level (preset to 77 before loading).
 */
static long long load_debug_level(const char* value) {
    char content[128];
    snprintf(content, sizeof(content), "[debug]\nlevel = %s\n", value);
    TempConfigFile file(content);
    Config config = get_default_config();
    config.debug.level = 77;
    ASSERT_EQ(load_config(file.path(), config), ConfigResult::Success);
    return static_cast<long long>(config.debug.level);
}

/**
 * @brief Unsigned values from 0 to 4294967295 are parsed exactly.
 */
TEST(parse_uint32_accepts_clean_integers) {
    ASSERT_EQ(load_debug_level("0"), 0);
    ASSERT_EQ(load_debug_level("3"), 3);
    ASSERT_EQ(load_debug_level("4294967295"), 4294967295LL);
}

/**
 * @brief Empty, negative, overflowing or garbled integers become 0.
 */
TEST(parse_uint32_turns_malformed_values_into_zero) {
    ASSERT_EQ(load_debug_level(""), 0);            // empty
    ASSERT_EQ(load_debug_level("-1"), 0);          // negative
    ASSERT_EQ(load_debug_level("4294967296"), 0);  // 2^32: exceeds uint32_t
    ASSERT_EQ(load_debug_level("99999999999999999999999"), 0);  // ERANGE
    ASSERT_EQ(load_debug_level("12abc"), 0);  // trailing garbage
    ASSERT_EQ(load_debug_level("abc"), 0);    // nothing parsed
}

/**
 * @brief An empty or negative server port becomes 0.
 */
TEST(server_port_with_empty_or_negative_value_is_zero) {
    const char* values[] = {"", "-1"};
    for (const char* v : values) {
        char content[64];
        snprintf(content, sizeof(content), "[server]\nport = %s\n", v);
        TempConfigFile file(content);
        Config config = get_default_config();
        ASSERT_EQ(load_config(file.path(), config), ConfigResult::Success);
        ASSERT_EQ(config.server.port, 0);
    }
}

/**
 * @brief Keys before any section, in an unknown section or without '=' change
 * nothing.
 */
TEST(keys_outside_a_known_section_are_ignored) {
    const char* content =
        "orphan = 1\n"  // before any section
        "[nonsense]\n"
        "host = evil.example\n"  // unknown section
        "port = 1\n"
        "[server]\n"
        "no_equals_sign_here\n"  // known section, malformed line
        "port = 4242\n";
    TempConfigFile file(content);
    Config config = get_default_config();
    Config defaults = get_default_config();
    ASSERT_EQ(load_config(file.path(), config), ConfigResult::Success);
    ASSERT_STREQ(config.server.host, defaults.server.host);
    ASSERT_EQ(config.server.port, 4242);
}

/**
 * @brief save_config() returns IoError when the parent path is a regular file.
 */
TEST(save_config_reports_io_error_when_the_file_cannot_be_created) {
    // The parent "directory" is a regular file, so neither mkdir nor fopen can
    // succeed.
    TempConfigFile blocker("x");
    char path[300];
    snprintf(path, sizeof(path), "%s/config.ini", blocker.path());
    Config config = get_default_config();
    ASSERT_EQ(save_config(path, config), ConfigResult::IoError);
}

// ============================================================================
// Main
// ============================================================================

/**
 * @brief Run every registered test and print the totals.
 * @return 1 when at least one test failed, otherwise 0.
 */
int main() {
    printf("=== ryu_ldn_nx Config Unit Tests ===\n\n");
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
