/**
 * @file ldn_shared_state_real_tests.cpp
 * @brief Unit tests for the real SharedState (ldn/ldn_shared_state.cpp)
 *
 * Built against the host Stratosphere stand-in in tests/stubs. The older
 * shared_state_tests.cpp exercises a private copy of the logic; this file
 * exercises the production source.
 */

#include <cstdio>
#include <vector>

#include "test_harness.hpp"
#include "ldn/ldn_shared_state.hpp"

using ams::mitm::ldn::CommState;
using ams::mitm::ldn::SharedState;

/**
 * @brief The state is a process-wide singleton: every test starts from a clean
 * slate.
 *
 * @return the SharedState singleton, reset with pid 0 and an empty whitelist
 */
static SharedState& fresh() {
    SharedState& s = SharedState::GetInstance();
    s.Reset();
    s.SetLdnPid(0);
    s.LoadLdnWhitelist({});
    return s;
}

/**
 * @brief GetInstance always returns the same object, so every IPC service
 * and the overlay see one shared state.
 */
TEST(the_instance_is_a_singleton) {
    ASSERT_TRUE(&SharedState::GetInstance() == &SharedState::GetInstance());
}

/**
 * @brief SetGameActive(true, pid) stores the pid; SetGameActive(false, ...)
 * clears the flag and the active process id.
 */
TEST(game_active_records_the_pid_and_clears_it_when_inactive) {
    SharedState& s = fresh();
    ASSERT_FALSE(s.IsGameActive());
    s.SetGameActive(true, 1234);
    ASSERT_TRUE(s.IsGameActive());
    ASSERT_EQ(s.GetActiveProcessId(), static_cast<u64>(1234));
    s.SetGameActive(false, 1234);
    ASSERT_FALSE(s.IsGameActive());
    ASSERT_EQ(s.GetActiveProcessId(), static_cast<u64>(0));
}

/**
 * @brief Deactivating the game resets LDN state, session info and RTT but
 * keeps the registered ldn:u pid, which outlives a game session.
 */
TEST(deactivating_the_game_resets_the_runtime_state_but_not_the_ldn_pid) {
    SharedState& s = fresh();
    s.SetGameActive(true, 7);
    s.SetLdnPid(7);
    s.SetLdnState(CommState::StationConnected);
    s.SetSessionInfo(3, 8, 2, true);
    s.SetLastRtt(42);
    s.SetGameActive(false, 0);

    u8 n = 1;
    u8 m = 1;
    u8 l = 1;
    bool h = true;
    s.GetSessionInfo(n, m, l, h);
    ASSERT_TRUE(s.GetLdnState() == CommState::None);
    ASSERT_EQ(n, 0);
    ASSERT_EQ(m, 0);
    ASSERT_EQ(l, 0);
    ASSERT_FALSE(h);
    ASSERT_EQ(s.GetLastRtt(), static_cast<u32>(0));
    ASSERT_EQ(s.GetLdnPid(), static_cast<u64>(7));
}

/**
 * @brief Reset clears the game flag, LDN state, RTT, session info and any
 * pending reconnect request.
 */
TEST(reset_clears_runtime_state_and_the_reconnect_flag) {
    SharedState& s = SharedState::GetInstance();
    s.SetGameActive(true, 9);
    s.SetLdnState(CommState::AccessPointCreated);
    s.SetSessionInfo(2, 4, 1, true);
    s.SetLastRtt(10);
    s.RequestReconnect();
    s.Reset();
    ASSERT_FALSE(s.IsGameActive());
    ASSERT_TRUE(s.GetLdnState() == CommState::None);
    ASSERT_EQ(s.GetLastRtt(), static_cast<u32>(0));
    ASSERT_FALSE(s.ConsumeReconnectRequest());
    const auto info = s.GetSessionInfoStruct();
    ASSERT_EQ(info.node_count, 0);
    ASSERT_EQ(info.is_host, 0);
}

/**
 * @brief IsLdnPid matches only the registered pid, and pid 0 (unset) never
 * matches, even when asked about 0.
 */
TEST(only_a_non_zero_registered_pid_is_an_ldn_pid) {
    SharedState& s = fresh();
    ASSERT_FALSE(s.IsLdnPid(0));
    ASSERT_FALSE(s.IsLdnPid(55));
    s.SetLdnPid(55);
    ASSERT_EQ(s.GetLdnPid(), static_cast<u64>(55));
    ASSERT_TRUE(s.IsLdnPid(55));
    ASSERT_FALSE(s.IsLdnPid(56));
    s.SetLdnPid(0);
    ASSERT_FALSE(s.IsLdnPid(0));
}

/**
 * @brief LoadLdnWhitelist replaces the old list and de-duplicates entries,
 * so the size counts distinct title ids.
 */
TEST(loading_the_whitelist_replaces_the_previous_one) {
    SharedState& s = fresh();
    ASSERT_EQ(s.GetWhitelistSize(), static_cast<size_t>(0));
    s.LoadLdnWhitelist(std::vector<u64>{0x100, 0x200, 0x200});
    ASSERT_EQ(s.GetWhitelistSize(), static_cast<size_t>(2));
    ASSERT_TRUE(s.IsLdnGame(0x100));
    ASSERT_FALSE(s.IsLdnGame(0x300));
    s.LoadLdnWhitelist(std::vector<u64>{0x300});
    ASSERT_FALSE(s.IsLdnGame(0x100));
    ASSERT_TRUE(s.IsLdnGame(0x300));
}

/// @brief SetLdnState/GetLdnState round-trip the CommState unchanged.
TEST(ldn_state_is_stored_as_given) {
    SharedState& s = fresh();
    s.SetLdnState(CommState::Station);
    ASSERT_TRUE(s.GetLdnState() == CommState::Station);
}

/**
 * @brief SetSessionInfo is readable through GetSessionInfo and through the
 * IPC struct (is_host as 0/1, reserved bytes zeroed).
 */
TEST(session_info_round_trips_through_both_accessors) {
    SharedState& s = fresh();
    s.SetSessionInfo(5, 8, 3, true);
    u8 n = 0;
    u8 m = 0;
    u8 l = 0;
    bool h = false;
    s.GetSessionInfo(n, m, l, h);
    ASSERT_EQ(n, 5);
    ASSERT_EQ(m, 8);
    ASSERT_EQ(l, 3);
    ASSERT_TRUE(h);
    auto info = s.GetSessionInfoStruct();
    ASSERT_EQ(info.node_count, 5);
    ASSERT_EQ(info.node_count_max, 8);
    ASSERT_EQ(info.local_node_id, 3);
    ASSERT_EQ(info.is_host, 1);
    for (u8 b : info.reserved) {
        ASSERT_EQ(b, 0);
    }
    s.SetSessionInfo(1, 2, 0, false);
    ASSERT_EQ(s.GetSessionInfoStruct().is_host, 0);
}

/// @brief SetLastRtt/GetLastRtt round-trip the round-trip time value.
TEST(rtt_is_stored) {
    SharedState& s = fresh();
    s.SetLastRtt(123);
    ASSERT_EQ(s.GetLastRtt(), static_cast<u32>(123));
}

/**
 * @brief RequestReconnect sets a one-shot flag: the first
 * ConsumeReconnectRequest returns true and clears it, the next returns false.
 */
TEST(a_reconnect_request_is_consumed_exactly_once) {
    SharedState& s = fresh();
    ASSERT_FALSE(s.ConsumeReconnectRequest());
    s.RequestReconnect();
    ASSERT_TRUE(s.ConsumeReconnectRequest());
    ASSERT_FALSE(s.ConsumeReconnectRequest());
}

/**
 * @brief IncrementSessionCount raises the active session count, and
 * DecrementSessionCount returns the value after decrementing.
 */
TEST(the_session_counter_returns_the_new_value_on_decrement) {
    SharedState& s = fresh();
    const u32 base = s.GetActiveSessionCount();
    s.IncrementSessionCount();
    s.IncrementSessionCount();
    ASSERT_EQ(s.GetActiveSessionCount(), base + 2);
    ASSERT_EQ(s.DecrementSessionCount(), base + 1);
    ASSERT_EQ(s.DecrementSessionCount(), base);
}

/**
 * @brief Print the test totals (the tests ran during static initialization).
 * @return Non-zero when at least one assertion failed.
 */
int main() {
    return test_report("SharedState (real) Tests");
}
