/**
 * @file game_whitelist_tests.cpp
 * @brief Unit tests for the real game whitelist (config/game_whitelist.cpp)
 *
 * The whitelist is a load-once latch: after the first LoadWhitelist() the
 * outcome is frozen for the life of the process. Each scenario therefore runs
 * in its own process, selected by the WHITELIST_SCENARIO environment variable
 * (the Makefile target loops over them); a test whose scenario is not the
 * selected one returns immediately.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <stratosphere.hpp>

#include "test_harness.hpp"
#include "config/game_whitelist.hpp"

using ams::fs::g_host_fs;
using ryu_ldn::config::IsGameInWhitelist;
using ryu_ldn::config::LoadWhitelist;

static bool scenario(const char* name) {
    const char* s = std::getenv("WHITELIST_SCENARIO");
    return s != nullptr && std::strcmp(s, name) == 0;
}

TEST(open_failure_leaves_an_empty_whitelist_and_latches) {
    if (!scenario("open_fail")) {
        return;
    }
    ASSERT_FALSE(IsGameInWhitelist(0x0100152000022000ULL));  // not loaded yet
    g_host_fs.open_fails = true;
    LoadWhitelist();
    ASSERT_FALSE(IsGameInWhitelist(0x0100152000022000ULL));
    g_host_fs.open_fails = false;
    g_host_fs.content = "0x0100152000022000\n";
    LoadWhitelist();  // latched: the file is not read again
    ASSERT_FALSE(IsGameInWhitelist(0x0100152000022000ULL));
    ASSERT_EQ(g_host_fs.open_count, 0);
}

TEST(size_failure_closes_the_file) {
    if (!scenario("size_fail")) {
        return;
    }
    g_host_fs.size_fails = true;
    LoadWhitelist();
    ASSERT_EQ(g_host_fs.close_count, 1);
    ASSERT_FALSE(IsGameInWhitelist(1));
}

TEST(an_empty_file_closes_the_file) {
    if (!scenario("empty")) {
        return;
    }
    g_host_fs.content.clear();
    LoadWhitelist();
    ASSERT_EQ(g_host_fs.close_count, 1);
    ASSERT_FALSE(IsGameInWhitelist(1));
}

TEST(allocation_failure_closes_the_file_and_stays_empty) {
    if (!scenario("alloc_fail")) {
        return;
    }
    g_host_fs.size_override = static_cast<s64>(1)
                              << 62;  // far beyond any address space
    LoadWhitelist();
    ASSERT_EQ(g_host_fs.close_count, 1);
    ASSERT_FALSE(IsGameInWhitelist(1));
}

TEST(a_read_failure_stops_the_load_with_what_was_parsed) {
    if (!scenario("read_fail")) {
        return;
    }
    g_host_fs.content = "0x0100152000022000\n";
    g_host_fs.read_fails = true;
    LoadWhitelist();
    ASSERT_EQ(g_host_fs.close_count, 1);
    ASSERT_FALSE(IsGameInWhitelist(0x0100152000022000ULL));
}

TEST(lines_are_parsed_in_every_accepted_format) {
    if (!scenario("parse")) {
        return;
    }
    std::string c;
    c += "0x0100152000022000\n";      // prefix, lower-case
    c += "\r\n";                      // CRLF empty line
    c += "0X0100ABCDEF000000\r\n";    // upper-case prefix and digits
    c += "0100abcd12345678\n";        // no prefix, lower-case hex
    c += "0\n";                       // id 0 is ignored
    c += "zz\n";                      // invalid first character: id 0, ignored
    c += "0x0100000000000099zzzz\n";  // stops at the first invalid character
    c += "11111111111111111111111111111111\n";  // longer than the line buffer:
                                                // truncated
    for (int i = 0; i < 100; ++i) {             // spans several 512-byte chunks
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x01000000000%05X\n", i + 1);
        c += buf;
    }
    c += "0x0200000000000001";  // last line without a trailing newline
    g_host_fs.content = c;
    LoadWhitelist();
    ASSERT_EQ(g_host_fs.close_count, 1);

    ASSERT_TRUE(IsGameInWhitelist(0x0100152000022000ULL));
    ASSERT_TRUE(IsGameInWhitelist(0x0100ABCDEF000000ULL));
    ASSERT_TRUE(IsGameInWhitelist(0x0100abcd12345678ULL));
    ASSERT_TRUE(IsGameInWhitelist(0x0100000000000099ULL));
    ASSERT_TRUE(IsGameInWhitelist(0x1111111111111111ULL));
    ASSERT_TRUE(IsGameInWhitelist(0x0100000000000001ULL));
    ASSERT_TRUE(IsGameInWhitelist(0x0100000000000064ULL));
    ASSERT_TRUE(IsGameInWhitelist(0x0200000000000001ULL));
    ASSERT_FALSE(IsGameInWhitelist(0));
    ASSERT_FALSE(IsGameInWhitelist(0x0100000000000065ULL));

    // Latched: a second call does not read the file again.
    g_host_fs.content.clear();
    LoadWhitelist();
    ASSERT_EQ(g_host_fs.open_count, 1);
    ASSERT_TRUE(IsGameInWhitelist(0x0100152000022000ULL));
}

TEST(the_capacity_bounds_the_number_of_entries) {
    if (!scenario("capacity")) {
        return;
    }
    // 2 bytes per entry against a capacity of size/18 + 100: the array fills
    // before the file ends, so the loader must stop instead of overrunning it.
    std::string c;
    for (int i = 0; i < 400; ++i) {
        c += "1\n";
    }
    c += "2";  // pending last line while the array is already full
    g_host_fs.content = c;
    LoadWhitelist();
    ASSERT_EQ(g_host_fs.close_count, 1);
    ASSERT_TRUE(IsGameInWhitelist(1));
    ASSERT_FALSE(IsGameInWhitelist(2));
}

int main() {
    return test_report("GameWhitelist Tests");
}
