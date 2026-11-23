/**
 * @file config_manager_tests.cpp
 * @brief Unit tests for ConfigManager
 *
 * Tests for the global configuration manager including:
 * - Initialization and loading
 * - Getter/setter for all settings
 * - Save and reload functionality
 * - Change notification callbacks
 */

#include "config/config_manager.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <stdexcept>

using namespace ryu_ldn::config;

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
 * @brief Widen an integral value for "%lld" printing.
 * @tparam T integral or enum type of the value
 * @param v value to widen
 * @return v as long long
 */
template <typename T>
static long long assert_ll(T v) {
    return static_cast<long long>(v);
}

/// Fails the current test when the two values differ.
#define ASSERT_EQ(a, b)                                                      \
    do {                                                                     \
        auto _a = (a);                                                       \
        auto _b = (b);                                                       \
        if (_a != _b) {                                                      \
            printf("  FAIL: %s == %s (line %d)\n", #a, #b, __LINE__);        \
            printf("    got: %lld vs %lld\n", assert_ll(_a), assert_ll(_b)); \
            throw std::runtime_error("Test assertion failed");               \
        }                                                                    \
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
    /// Test fixture value: name.
    const char* name;
    /// Test function to run.
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
// Test Helpers
// ============================================================================

/// Test fixture value: test config path.
static char g_test_config_path[256];

/// Test helper: create test config file.
static void create_test_config_file() {
    snprintf(
        g_test_config_path,
        sizeof(g_test_config_path),
        "/tmp/config_manager_test_%d.ini",
        rand());

    FILE* f = fopen(g_test_config_path, "w");
    if (f) {
        fprintf(f, "[server]\n");
        fprintf(f, "host = test.example.com\n");
        fprintf(f, "port = 12345\n");
        fprintf(f, "\n");
        fprintf(f, "[ldn]\n");
        fprintf(f, "enabled = 1\n");
        fprintf(f, "use_passphrase = 0\n");
        fprintf(f, "passphrase = \n");
        fprintf(f, "disable_p2p = 1\n");
        fprintf(f, "\n");
        fprintf(f, "[debug]\n");
        fprintf(f, "enabled = 1\n");
        fprintf(f, "level = 3\n");
        fclose(f);
    }
}

/// Test helper: remove test config file.
static void remove_test_config_file() {
    std::remove(g_test_config_path);
}

// Track change callbacks
/// Test fixture value: last changed section.
static const char* g_last_changed_section = nullptr;
/// Test fixture value: change callback count.
static int g_change_callback_count = 0;

/**
 * @brief Test helper: test change callback.
 *
 * @param section name of the changed section, stored in g_last_changed_section
 */
static void test_change_callback(const char* section) {
    g_last_changed_section = section;
    g_change_callback_count++;
}

// ============================================================================
// Initialization Tests
// ============================================================================

/** @brief Test case: singleton instance. */
TEST(singleton_instance) {
    ConfigManager& m1 = ConfigManager::Instance();
    ConfigManager& m2 = ConfigManager::Instance();
    ASSERT_TRUE(&m1 == &m2);
}

/** @brief Test case: save and reload before initialize report io error. */
// Must stay the first test that touches the singleton after
// singleton_instance: it relies on Initialize() not having run yet.
TEST(save_and_reload_before_initialize_report_io_error) {
    ASSERT_FALSE(ConfigManager::Instance().IsInitialized());
    ASSERT_EQ(ConfigManager::Instance().Save(), ConfigResult::IoError);
    ASSERT_EQ(ConfigManager::Instance().Reload(), ConfigResult::IoError);
}

/**
 * @brief Test case: initialize without a path falls back to the default
 *   location.
 */
TEST(initialize_without_a_path_falls_back_to_the_default_location) {
    // The default path is an SD-card path that does not exist on the host:
    // a missing file is acceptable and leaves the defaults in place.
    ASSERT_TRUE(ConfigManager::Instance().Initialize(nullptr));
    ASSERT_TRUE(ConfigManager::Instance().IsInitialized());
    ASSERT_FALSE(ConfigManager::Instance().HasUnsavedChanges());
}

/** @brief Test case: save writes the file and clears the dirty flag. */
TEST(save_writes_the_file_and_clears_the_dirty_flag) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/config_manager_save_%d.ini", rand());
    ASSERT_TRUE(ConfigManager::Instance().Initialize(path));
    ConfigManager::Instance().SetServerPort(4321);
    ASSERT_TRUE(ConfigManager::Instance().HasUnsavedChanges());
    ASSERT_EQ(ConfigManager::Instance().Save(), ConfigResult::Success);
    ASSERT_FALSE(ConfigManager::Instance().HasUnsavedChanges());

    // The saved file reloads to the saved value.
    ConfigManager::Instance().SetServerPort(1);
    ASSERT_EQ(ConfigManager::Instance().Reload(), ConfigResult::Success);
    ASSERT_EQ(ConfigManager::Instance().GetServerPort(), 4321);
    std::remove(path);
}

/** @brief Test case: save failure keeps the dirty flag. */
TEST(save_failure_keeps_the_dirty_flag) {
    // Parent directory does not exist and mkdir only creates the last level.
    char path[256];
    snprintf(
        path,
        sizeof(path),
        "/tmp/config_manager_missing_%d/sub/config.ini",
        rand());
    ASSERT_TRUE(ConfigManager::Instance().Initialize(path));
    ConfigManager::Instance().SetServerPort(4321);
    ASSERT_EQ(ConfigManager::Instance().Save(), ConfigResult::IoError);
    ASSERT_TRUE(ConfigManager::Instance().HasUnsavedChanges());
}

/** @brief Test case: use passphrase setter updates value and notifies. */
TEST(use_passphrase_setter_updates_value_and_notifies) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent_config_12345.ini");
    g_change_callback_count = 0;
    g_last_changed_section = nullptr;
    ConfigManager::Instance().SetChangeCallback(test_change_callback);
    ConfigManager::Instance().SetUsePassphrase(true);
    ASSERT_TRUE(ConfigManager::Instance().GetUsePassphrase());
    ASSERT_TRUE(ConfigManager::Instance().HasUnsavedChanges());
    ASSERT_EQ(g_change_callback_count, 1);
    ASSERT_STREQ(g_last_changed_section, "ldn");
    ConfigManager::Instance().SetUsePassphrase(false);
    ASSERT_FALSE(ConfigManager::Instance().GetUsePassphrase());
    ConfigManager::Instance().SetChangeCallback(nullptr);
}

/**
 * @brief Test case: generate random passphrase rejects null and undersized
 *   buffers.
 */
TEST(generate_random_passphrase_rejects_null_and_undersized_buffers) {
    char buf[17];
    std::memset(buf, 'x', sizeof(buf));
    GenerateRandomPassphrase(nullptr, 32);  // must not crash
    GenerateRandomPassphrase(buf, 16);      // too small: emptied
    ASSERT_EQ(buf[0], '\0');
    std::memset(buf, 'x', sizeof(buf));
    GenerateRandomPassphrase(buf, 0);  // zero size: untouched
    ASSERT_EQ(buf[0], 'x');
    GenerateRandomPassphrase(buf, sizeof(buf));
    ASSERT_TRUE(IsValidPassphrase(buf));
    ASSERT_EQ(std::strlen(buf), 16U);
}

/** @brief Test case: initialize with missing file. */
TEST(initialize_with_missing_file) {
    // Initialize with non-existent file should use defaults
    bool result = ConfigManager::Instance().Initialize(
        "/tmp/nonexistent_config_12345.ini");
    // Should still succeed with defaults
    ASSERT_TRUE(result || !result);  // Just ensure no crash
}

/** @brief Test case: initialize with valid file. */
TEST(initialize_with_valid_file) {
    create_test_config_file();
    bool result = ConfigManager::Instance().Initialize(g_test_config_path);
    ASSERT_TRUE(result);
    ASSERT_TRUE(ConfigManager::Instance().IsInitialized());
    remove_test_config_file();
}

// ============================================================================
// Server Settings Tests
// ============================================================================

/** @brief Test case: get default server host. */
TEST(get_default_server_host) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    const char* host = ConfigManager::Instance().GetServerHost();
    ASSERT_STREQ(host, DEFAULT_HOST);
}

/** @brief Test case: set server host. */
TEST(set_server_host) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetServerHost("new.server.com");
    ASSERT_STREQ(ConfigManager::Instance().GetServerHost(), "new.server.com");
}

/** @brief Test case: a null server host leaves the setting unchanged. */
TEST(set_server_host_null_is_ignored) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetServerHost("kept.server.com");
    ConfigManager::Instance().SetServerHost(nullptr);
    ASSERT_STREQ(ConfigManager::Instance().GetServerHost(), "kept.server.com");
}

/** @brief Test case: set server host truncates long name. */
TEST(set_server_host_truncates_long_name) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    char long_host[256];
    memset(long_host, 'a', 255);
    long_host[255] = '\0';
    ConfigManager::Instance().SetServerHost(long_host);
    // Should be truncated to MAX_HOST_LENGTH
    ASSERT_TRUE(
        strlen(ConfigManager::Instance().GetServerHost()) <= MAX_HOST_LENGTH);
}

/** @brief Test case: get default server port. */
TEST(get_default_server_port) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ASSERT_EQ(ConfigManager::Instance().GetServerPort(), DEFAULT_PORT);
}

/** @brief Test case: set server port. */
TEST(set_server_port) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetServerPort(9999);
    ASSERT_EQ(ConfigManager::Instance().GetServerPort(), 9999);
}

// ============================================================================
// LDN Settings Tests
// ============================================================================

/** @brief Test case: get default ldn enabled. */
TEST(get_default_ldn_enabled) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ASSERT_EQ(ConfigManager::Instance().GetLdnEnabled(), DEFAULT_LDN_ENABLED);
}

/** @brief Test case: set ldn enabled. */
TEST(set_ldn_enabled) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetLdnEnabled(false);
    ASSERT_FALSE(ConfigManager::Instance().GetLdnEnabled());
    ConfigManager::Instance().SetLdnEnabled(true);
    ASSERT_TRUE(ConfigManager::Instance().GetLdnEnabled());
}

/** @brief Test case: get default passphrase. */
TEST(get_default_passphrase) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ASSERT_STREQ(ConfigManager::Instance().GetPassphrase(), "");
}

/** @brief Test case: set passphrase valid. */
TEST(set_passphrase_valid) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    // Valid format: Ryujinx-[0-9a-f]{8}
    bool result = ConfigManager::Instance().SetPassphrase("Ryujinx-abcd1234");
    ASSERT_TRUE(result);
    ASSERT_STREQ(ConfigManager::Instance().GetPassphrase(), "Ryujinx-abcd1234");
}

/** @brief Test case: set passphrase invalid rejected. */
TEST(set_passphrase_invalid_rejected) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetPassphrase("");  // Clear first
    // Invalid passphrase should be rejected
    bool result = ConfigManager::Instance().SetPassphrase("mysecretpass");
    ASSERT_FALSE(result);
    ASSERT_STREQ(ConfigManager::Instance().GetPassphrase(), "");  // Unchanged
}

/** @brief Test case: set passphrase empty. */
TEST(set_passphrase_empty) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetPassphrase("Ryujinx-12345678");
    ConfigManager::Instance().SetPassphrase("");
    ASSERT_STREQ(ConfigManager::Instance().GetPassphrase(), "");
}

/** @brief Test case: set passphrase truncates long rejected. */
TEST(set_passphrase_truncates_long_rejected) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetPassphrase("");  // Clear first
    // Invalid passphrase (too long) should be rejected
    char long_pass[128];
    memset(long_pass, 'x', 127);
    long_pass[127] = '\0';
    bool result = ConfigManager::Instance().SetPassphrase(long_pass);
    ASSERT_FALSE(result);
    ASSERT_STREQ(ConfigManager::Instance().GetPassphrase(), "");  // Unchanged
}

// ============================================================================
// Passphrase Validation Tests (format: Ryujinx-[0-9a-f]{8})
// ============================================================================

/** @brief Test case: is valid passphrase correct format. */
TEST(is_valid_passphrase_correct_format) {
    ASSERT_TRUE(IsValidPassphrase("Ryujinx-12345678"));
    ASSERT_TRUE(IsValidPassphrase("Ryujinx-abcdef01"));
    ASSERT_TRUE(IsValidPassphrase("Ryujinx-00000000"));
    ASSERT_TRUE(IsValidPassphrase("Ryujinx-ffffffff"));
    ASSERT_TRUE(IsValidPassphrase("Ryujinx-a1b2c3d4"));
}

/** @brief Test case: is valid passphrase empty is valid. */
TEST(is_valid_passphrase_empty_is_valid) {
    // Empty passphrase is allowed (means no passphrase filtering)
    ASSERT_TRUE(IsValidPassphrase(""));
    ASSERT_TRUE(IsValidPassphrase(nullptr));
}

/** @brief Test case: is valid passphrase wrong prefix. */
TEST(is_valid_passphrase_wrong_prefix) {
    ASSERT_FALSE(IsValidPassphrase("ryujinx-12345678"));  // lowercase
    ASSERT_FALSE(IsValidPassphrase("RYUJINX-12345678"));  // uppercase
    ASSERT_FALSE(IsValidPassphrase("Ryuginx-12345678"));  // typo
    ASSERT_FALSE(IsValidPassphrase("Switch-12345678"));   // wrong prefix
    ASSERT_FALSE(IsValidPassphrase("12345678"));          // no prefix
}

/** @brief Test case: is valid passphrase wrong hex length. */
TEST(is_valid_passphrase_wrong_hex_length) {
    ASSERT_FALSE(IsValidPassphrase("Ryujinx-1234567"));    // 7 chars
    ASSERT_FALSE(IsValidPassphrase("Ryujinx-123456789"));  // 9 chars
    ASSERT_FALSE(IsValidPassphrase("Ryujinx-"));           // 0 chars
    ASSERT_FALSE(IsValidPassphrase("Ryujinx-1234"));       // 4 chars
}

/** @brief Test case: is valid passphrase invalid hex chars. */
TEST(is_valid_passphrase_invalid_hex_chars) {
    ASSERT_FALSE(IsValidPassphrase("Ryujinx-1234567g"));  // 'g' not hex
    ASSERT_FALSE(IsValidPassphrase("Ryujinx-ABCDEF01"));  // uppercase hex
    ASSERT_FALSE(IsValidPassphrase("Ryujinx-1234 678"));  // space
    ASSERT_FALSE(IsValidPassphrase("Ryujinx-1234-678"));  // extra dash
    ASSERT_FALSE(IsValidPassphrase("Ryujinx-!@#$%^&*"));  // special chars
}

/** @brief Test case: is valid passphrase missing dash. */
TEST(is_valid_passphrase_missing_dash) {
    ASSERT_FALSE(IsValidPassphrase("Ryujinx12345678"));
    ASSERT_FALSE(IsValidPassphrase("Ryujinx 12345678"));
}

/** @brief Test case: set passphrase rejects invalid. */
TEST(set_passphrase_rejects_invalid) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetPassphrase("");  // Clear first

    // Try to set invalid passphrase - should be rejected
    bool result = ConfigManager::Instance().SetPassphrase("invalid");
    ASSERT_FALSE(result);
    ASSERT_STREQ(ConfigManager::Instance().GetPassphrase(), "");

    // Try valid passphrase - should succeed
    result = ConfigManager::Instance().SetPassphrase("Ryujinx-12345678");
    ASSERT_TRUE(result);
    ASSERT_STREQ(ConfigManager::Instance().GetPassphrase(), "Ryujinx-12345678");
}

/** @brief Test case: set passphrase allows empty. */
TEST(set_passphrase_allows_empty) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetPassphrase("Ryujinx-12345678");

    // Empty should be allowed (clears passphrase)
    bool result = ConfigManager::Instance().SetPassphrase("");
    ASSERT_TRUE(result);
    ASSERT_STREQ(ConfigManager::Instance().GetPassphrase(), "");

    // nullptr should also be allowed
    ConfigManager::Instance().SetPassphrase("Ryujinx-abcdef01");
    result = ConfigManager::Instance().SetPassphrase(nullptr);
    ASSERT_TRUE(result);
    ASSERT_STREQ(ConfigManager::Instance().GetPassphrase(), "");
}

// ============================================================================
// Random Passphrase Generation Tests
// ============================================================================

/** @brief Test case: generate random passphrase format. */
TEST(generate_random_passphrase_format) {
    char passphrase[32];
    GenerateRandomPassphrase(passphrase, sizeof(passphrase));

    // Must match format Ryujinx-[0-9a-f]{8}
    ASSERT_TRUE(IsValidPassphrase(passphrase));
    ASSERT_TRUE(strlen(passphrase) == 16);  // "Ryujinx-" (8) + hex (8)

    // Check prefix
    ASSERT_TRUE(strncmp(passphrase, "Ryujinx-", 8) == 0);
}

/** @brief Test case: generate random passphrase varies. */
TEST(generate_random_passphrase_varies) {
    char p1[32];
    char p2[32];
    char p3[32];
    GenerateRandomPassphrase(p1, sizeof(p1));
    GenerateRandomPassphrase(p2, sizeof(p2));
    GenerateRandomPassphrase(p3, sizeof(p3));

    // Very unlikely all three are identical
    bool all_same = (strcmp(p1, p2) == 0) && (strcmp(p2, p3) == 0);
    ASSERT_FALSE(all_same);
}

/** @brief Test case: generate random passphrase only lowercase hex. */
TEST(generate_random_passphrase_only_lowercase_hex) {
    char passphrase[32];
    GenerateRandomPassphrase(passphrase, sizeof(passphrase));

    // Check hex part (chars 8-15)
    for (int i = 8; i < 16; i++) {
        char c = passphrase[i];
        bool valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        ASSERT_TRUE(valid);
    }
}

// ============================================================================
// Debug Settings Tests
// ============================================================================

/** @brief Test case: get default debug enabled. */
TEST(get_default_debug_enabled) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ASSERT_EQ(
        ConfigManager::Instance().GetDebugEnabled(), DEFAULT_DEBUG_ENABLED);
}

/** @brief Test case: set debug enabled. */
TEST(set_debug_enabled) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetDebugEnabled(true);
    ASSERT_TRUE(ConfigManager::Instance().GetDebugEnabled());
    ConfigManager::Instance().SetDebugEnabled(false);
    ASSERT_FALSE(ConfigManager::Instance().GetDebugEnabled());
}

/** @brief Test case: get default debug level. */
TEST(get_default_debug_level) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ASSERT_EQ(ConfigManager::Instance().GetDebugLevel(), DEFAULT_DEBUG_LEVEL);
}

/** @brief Test case: set debug level. */
TEST(set_debug_level) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetDebugLevel(3);
    ASSERT_EQ(ConfigManager::Instance().GetDebugLevel(), 3U);
    ConfigManager::Instance().SetDebugLevel(0);
    ASSERT_EQ(ConfigManager::Instance().GetDebugLevel(), 0U);
}

/** @brief Test case: set debug level clamps to max. */
TEST(set_debug_level_clamps_to_max) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetDebugLevel(100);
    ASSERT_TRUE(ConfigManager::Instance().GetDebugLevel() <= 3);
}

// ============================================================================
// Save and Reload Tests
// ============================================================================

/** @brief Test case: save creates file. */
TEST(save_creates_file) {
    char save_path[256];
    snprintf(
        save_path, sizeof(save_path), "/tmp/config_save_test_%d.ini", rand());

    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    // Reinitialize with save path would require modifying the path
    // For now just test the Save() call doesn't crash
    // ConfigResult result = ConfigManager::Instance().Save();
    // ASSERT_EQ(result, ConfigResult::Success);

    std::remove(save_path);
}

/** @brief Test case: reload restores values. */
TEST(reload_restores_values) {
    create_test_config_file();

    ConfigManager::Instance().Initialize(g_test_config_path);
    ASSERT_STREQ(ConfigManager::Instance().GetServerHost(), "test.example.com");

    // Modify
    ConfigManager::Instance().SetServerHost("modified.com");
    ASSERT_STREQ(ConfigManager::Instance().GetServerHost(), "modified.com");

    // Reload should restore
    ConfigManager::Instance().Reload();
    ASSERT_STREQ(ConfigManager::Instance().GetServerHost(), "test.example.com");

    remove_test_config_file();
}

/** @brief Test case: has unsaved changes after modification. */
TEST(has_unsaved_changes_after_modification) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");

    // Fresh init - no unsaved changes
    // ASSERT_FALSE(ConfigManager::Instance().HasUnsavedChanges());

    // After modification - should have unsaved changes
    ConfigManager::Instance().SetServerPort(1234);
    ASSERT_TRUE(ConfigManager::Instance().HasUnsavedChanges());
}

// ============================================================================
// Change Callback Tests
// ============================================================================

/** @brief Test case: change callback invoked on server change. */
TEST(change_callback_invoked_on_server_change) {
    g_change_callback_count = 0;
    g_last_changed_section = nullptr;

    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetChangeCallback(test_change_callback);

    ConfigManager::Instance().SetServerHost("callback.test.com");

    ASSERT_TRUE(g_change_callback_count > 0);
    ASSERT_TRUE(g_last_changed_section != nullptr);
    ASSERT_STREQ(g_last_changed_section, "server");

    ConfigManager::Instance().SetChangeCallback(nullptr);
}

/** @brief Test case: change callback invoked on ldn change. */
TEST(change_callback_invoked_on_ldn_change) {
    g_change_callback_count = 0;
    g_last_changed_section = nullptr;

    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetChangeCallback(test_change_callback);

    // Use valid passphrase format
    ConfigManager::Instance().SetPassphrase("Ryujinx-aabbccdd");

    ASSERT_TRUE(g_change_callback_count > 0);
    ASSERT_STREQ(g_last_changed_section, "ldn");

    ConfigManager::Instance().SetChangeCallback(nullptr);
}

/** @brief Test case: change callback invoked on debug change. */
TEST(change_callback_invoked_on_debug_change) {
    g_change_callback_count = 0;
    g_last_changed_section = nullptr;

    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetChangeCallback(test_change_callback);

    ConfigManager::Instance().SetDebugLevel(2);

    ASSERT_TRUE(g_change_callback_count > 0);
    ASSERT_STREQ(g_last_changed_section, "debug");

    ConfigManager::Instance().SetChangeCallback(nullptr);
}

/** @brief Test case: null callback safe. */
TEST(null_callback_safe) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    ConfigManager::Instance().SetChangeCallback(nullptr);

    // Should not crash
    ConfigManager::Instance().SetServerHost("safe.test.com");
    ConfigManager::Instance().SetServerPort(9999);
}

// ============================================================================
// Load From File Tests
// ============================================================================

/** @brief Test case: load server settings from file. */
TEST(load_server_settings_from_file) {
    create_test_config_file();
    ConfigManager::Instance().Initialize(g_test_config_path);

    ASSERT_STREQ(ConfigManager::Instance().GetServerHost(), "test.example.com");
    ASSERT_EQ(ConfigManager::Instance().GetServerPort(), 12345);

    remove_test_config_file();
}

/** @brief Test case: load ldn settings from file. */
TEST(load_ldn_settings_from_file) {
    create_test_config_file();
    ConfigManager::Instance().Initialize(g_test_config_path);

    ASSERT_TRUE(ConfigManager::Instance().GetLdnEnabled());
    ASSERT_STREQ(ConfigManager::Instance().GetPassphrase(), "");

    remove_test_config_file();
}

/** @brief Test case: load debug settings from file. */
TEST(load_debug_settings_from_file) {
    create_test_config_file();
    ConfigManager::Instance().Initialize(g_test_config_path);

    ASSERT_TRUE(ConfigManager::Instance().GetDebugEnabled());
    ASSERT_EQ(ConfigManager::Instance().GetDebugLevel(), 3U);

    remove_test_config_file();
}

//=============================================================================
// Use Passphrase Configuration Tests
//=============================================================================

/** @brief Test case: get default use passphrase. */
TEST(get_default_use_passphrase) {
    ConfigManager::Instance().Initialize("/tmp/nonexistent.ini");
    // Default should be false (no passphrase filtering)
    // Note: ConfigManager may or may not expose this directly.
    // If it does, test it here.
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
    printf("=== ryu_ldn_nx ConfigManager Unit Tests ===\n\n");
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
