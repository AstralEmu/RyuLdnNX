/**
 * @file test_harness.hpp
 * @brief Minimal self-registering test harness shared by the host unit tests
 *
 * TEST(name) registers a test that runs from a static initializer; main()
 * only prints the totals and returns non-zero when an assertion failed.
 */

#pragma once

#include <cstdio>

/// Number of tests that ran (incremented by TEST before assertions fail).
inline int g_tests_passed = 0;
/// Number of tests aborted by a failed assertion; non-zero fails the run.
inline int g_tests_failed = 0;

/**
 * @brief Define and auto-run a test.
 *
 * Declares test_<name>() and a static object whose constructor prints the
 * name, runs the body during static initialization and counts it as passed.
 * The body follows the macro like a function body.
 *
 * @param name Identifier of the test (also printed, stringified).
 */
#define TEST(name)                            \
    static void test_##name();                \
    static struct test_##name##_register {    \
        test_##name##_register() {            \
            printf("  [TEST] %s... ", #name); \
            fflush(stdout);                   \
            test_##name();                    \
            printf("PASS\n");                 \
            g_tests_passed++;                 \
        }                                     \
    } test_##name##_instance;                 \
    static void test_##name()

/**
 * @brief Fail the current test when cond is false.
 *
 * Prints the expression with file and line, counts a failure and returns
 * from the enclosing void test body.
 *
 * @param cond Expression expected to be true.
 */
#define ASSERT_TRUE(cond)                                         \
    do {                                                          \
        if (!(cond)) {                                            \
            printf(                                               \
                "FAIL\n    Assertion failed: %s\n    at %s:%d\n", \
                #cond,                                            \
                __FILE__,                                         \
                __LINE__);                                        \
            g_tests_failed++;                                     \
            return;                                               \
        }                                                         \
    } while (0)

/// @brief Fail the current test when cond is true (see ASSERT_TRUE).
///
/// @param cond Expression expected to be false.
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

/**
 * @brief Fail the current test when a == b is false.
 *
 * Prints both expressions with file and line, counts a failure and returns
 * from the enclosing void test body.
 *
 * @param a Left operand.
 * @param b Right operand.
 */
#define ASSERT_EQ(a, b)                                         \
    do {                                                        \
        if (!((a) == (b))) {                                    \
            printf(                                             \
                "FAIL\n    Expected: %s == %s\n    at %s:%d\n", \
                #a,                                             \
                #b,                                             \
                __FILE__,                                       \
                __LINE__);                                      \
            g_tests_failed++;                                   \
            return;                                             \
        }                                                       \
    } while (0)

/**
 * @brief Print the totals and return the process exit code.
 * @param[in] suite Suite title printed in the banner.
 * @return 1 when at least one test failed, otherwise 0.
 */
inline int test_report(const char* suite) {
    printf("\n========================================\n");
    printf("  %s\n", suite);
    printf(
        "  Results: %d/%d passed\n",
        g_tests_passed - g_tests_failed,
        g_tests_passed);
    printf("========================================\n\n");
    return g_tests_failed > 0 ? 1 : 0;
}
