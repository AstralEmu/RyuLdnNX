/**
 * @file ldn_network_timeout_tests.cpp
 * @brief Unit tests for the real NetworkTimeout (ldn/ldn_network_timeout.cpp)
 *
 * Built against the host Stratosphere stand-in in tests/stubs.
 */

#include <cstdio>
#include <cstdint>

#include <cstring>

#include "ldn/ldn_network_timeout.hpp"

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) \
    static void test_##name(); \
    static struct test_##name##_register { \
        test_##name##_register() { \
            printf("  [TEST] %s... ", #name); \
            fflush(stdout); \
            test_##name(); \
            printf("PASS\n"); \
            g_tests_passed++; \
        } \
    } test_##name##_instance; \
    static void test_##name()

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL\n    Assertion failed: %s\n    at %s:%d\n", #cond, __FILE__, __LINE__); \
            g_tests_failed++; \
            return; \
        } \
    } while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b) \
    do { \
        if (!((a) == (b))) { \
            printf("FAIL\n    Expected: %s == %s\n    at %s:%d\n", #a, #b, __FILE__, __LINE__); \
            g_tests_failed++; \
            return; \
        } \
    } while(0)

using ams::mitm::ldn::NetworkTimeout;

static int g_fired = 0;
static void on_timeout() { g_fired++; }

static void set_now(uint64_t ms) { ams::os::g_host_tick_ms = ms; }

TEST(a_new_timeout_is_inactive_and_never_fires) {
    g_fired = 0;
    NetworkTimeout t(1000, on_timeout);
    ASSERT_FALSE(t.IsActive());
    ASSERT_FALSE(t.CheckTimeout(1000000));
    ASSERT_EQ(g_fired, 0);
}

TEST(refresh_activates_and_always_returns_true) {
    g_fired = 0;
    set_now(100);
    NetworkTimeout t(1000, on_timeout);
    ASSERT_TRUE(t.RefreshTimeout());
    ASSERT_TRUE(t.IsActive());
}

TEST(it_fires_exactly_when_the_idle_time_has_elapsed) {
    g_fired = 0;
    set_now(100);
    NetworkTimeout t(1000, on_timeout);
    t.RefreshTimeout();
    ASSERT_FALSE(t.CheckTimeout(1099));
    ASSERT_EQ(g_fired, 0);
    ASSERT_TRUE(t.IsActive());
    ASSERT_TRUE(t.CheckTimeout(1100));
    ASSERT_EQ(g_fired, 1);
    ASSERT_FALSE(t.IsActive());
}

TEST(it_fires_only_once_until_refreshed_again) {
    g_fired = 0;
    set_now(0);
    NetworkTimeout t(10, on_timeout);
    t.RefreshTimeout();
    ASSERT_TRUE(t.CheckTimeout(50));
    ASSERT_FALSE(t.CheckTimeout(60));
    ASSERT_EQ(g_fired, 1);
    set_now(70);
    t.RefreshTimeout();
    ASSERT_FALSE(t.CheckTimeout(75));
    ASSERT_TRUE(t.CheckTimeout(80));
    ASSERT_EQ(g_fired, 2);
}

TEST(refreshing_restarts_the_countdown) {
    g_fired = 0;
    set_now(0);
    NetworkTimeout t(100, on_timeout);
    t.RefreshTimeout();
    set_now(90);
    t.RefreshTimeout();
    ASSERT_FALSE(t.CheckTimeout(150));
    ASSERT_TRUE(t.CheckTimeout(190));
}

TEST(disable_cancels_a_pending_timeout) {
    g_fired = 0;
    set_now(0);
    NetworkTimeout t(10, on_timeout);
    t.RefreshTimeout();
    t.DisableTimeout();
    ASSERT_FALSE(t.IsActive());
    ASSERT_FALSE(t.CheckTimeout(1000));
    ASSERT_EQ(g_fired, 0);
}

TEST(a_null_callback_never_fires_and_stays_active) {
    set_now(0);
    NetworkTimeout t(10, nullptr);
    t.RefreshTimeout();
    ASSERT_FALSE(t.CheckTimeout(1000));
    ASSERT_TRUE(t.IsActive());
}

TEST(the_default_idle_timeout_matches_the_reference_client) {
    ASSERT_EQ(NetworkTimeout::DEFAULT_IDLE_TIMEOUT_MS, static_cast<uint64_t>(6000));
}

int main() {
    printf("\n========================================\n");
    printf("  NetworkTimeout Tests\n");
    printf("========================================\n\n");

    printf("\n========================================\n");
    printf("  Results: %d/%d passed\n", g_tests_passed, g_tests_passed + g_tests_failed);
    printf("========================================\n\n");

    return g_tests_failed > 0 ? 1 : 0;
}
