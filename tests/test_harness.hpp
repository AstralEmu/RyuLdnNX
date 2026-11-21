/**
 * @file test_harness.hpp
 * @brief Minimal self-registering test harness shared by the host unit tests
 *
 * TEST(name) registers a test that runs from a static initializer; main()
 * only prints the totals and returns non-zero when an assertion failed.
 */

#pragma once

#include <cstdio>

inline int g_tests_passed = 0;
inline int g_tests_failed = 0;

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


/// Print the totals and return the process exit code.
inline int test_report(const char* suite) {
    printf("\n========================================\n");
    printf("  %s\n", suite);
    printf("  Results: %d/%d passed\n", g_tests_passed - g_tests_failed, g_tests_passed);
    printf("========================================\n\n");
    return g_tests_failed > 0 ? 1 : 0;
}
