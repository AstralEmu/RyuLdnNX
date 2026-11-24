/**
 * @file shared_state_tests.cpp
 * @brief Unit tests for SharedState singleton (runtime LDN state)
 *
 * Tests for the SharedState class that shares runtime LDN state between
 * the MITM service and the standalone ryu:cfg configuration service.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#include <cstdio>
#include <cstdint>

// ============================================================================
// Test Framework (minimal)
// ============================================================================

/// Number of tests that finished without a failed assertion.
static int g_tests_passed = 0;
/// Number of tests aborted by a failed assertion (non-zero fails the run).
static int g_tests_failed = 0;

/**
 * @brief Define and auto-run a test.
 * Declares test_<name>() and a static registrar whose constructor runs the
 * body during static initialization and counts it as passed.
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
 * Prints the expression with file and line, counts a failure and returns
 * from the enclosing void test body.
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

/**
 * @brief Fail the current test when cond is true.
 * @param cond Expression expected to be false.
 */
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

/**
 * @brief Fail the current test when a != b.
 * @param a Actual value.
 * @param b Expected value.
 */
#define ASSERT_EQ(a, b)                                         \
    do {                                                        \
        if ((a) != (b)) {                                       \
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

// ============================================================================
// Types (mirroring sysmodule definitions)
// ============================================================================

/// Unsigned 8-bit integer, as in libnx types.
using u8 = uint8_t;
/// Unsigned 32-bit integer, as in libnx types.
using u32 = uint32_t;
/// Unsigned 64-bit integer, as in libnx types.
using u64 = uint64_t;

/**
 * @brief LDN communication state (mirrors ams::mitm::ldn::CommState)
 */
enum class CommState : u32 {
    None = 0,
    Initialized = 1,
    AccessPoint = 2,
    AccessPointCreated = 3,
    Station = 4,
    StationConnected = 5,
    Error = 6
};

// ============================================================================
// Test Implementation of SharedState (mirrors the real implementation)
// ============================================================================

#include <mutex>

/**
 * @brief Shared runtime state singleton
 *
 * This class provides a thread-safe singleton for sharing runtime LDN state
 * between the MITM service (which updates the state) and the ryu:cfg service
 * (which exposes it to the overlay).
 */
class SharedState {
public:
    /**
     * @brief Get the process-wide instance.
     * @return The single SharedState (function-local static).
     */
    static SharedState& GetInstance() {
        static SharedState instance;
        return instance;
    }

    /**
     * @brief Reset all state (for testing)
     */
    void Reset() {
        std::scoped_lock lock(m_mutex);
        m_game_active = false;
        m_process_id = 0;
        m_ldn_state = CommState::None;
        m_node_count = 0;
        m_max_nodes = 0;
        m_local_node_id = 0;
        m_is_host = false;
        m_last_rtt_ms = 0;
        m_reconnect_requested = false;
    }

    // =========================================================================
    // Game Active State
    // =========================================================================

    /**
     * @brief Record whether a game is running.
     * Deactivating also resets the LDN state, session info and RTT.
     * @param active New game-active flag.
     * @param process_id Process id of the game; stored only when `active`.
     */
    void SetGameActive(bool active, u64 process_id) {
        std::scoped_lock lock(m_mutex);
        m_game_active = active;
        m_process_id = active ? process_id : 0;
        if (!active) {
            // Reset runtime state when game exits
            m_ldn_state = CommState::None;
            m_node_count = 0;
            m_max_nodes = 0;
            m_local_node_id = 0;
            m_is_host = false;
            m_last_rtt_ms = 0;
        }
    }

    /**
     * @brief Check whether a game is running.
     * @return The game-active flag.
     */
    bool IsGameActive() const {
        std::scoped_lock lock(m_mutex);
        return m_game_active;
    }

    /**
     * @brief Get the process id of the active game.
     * @return Process id, or 0 when no game is active.
     */
    u64 GetActiveProcessId() const {
        std::scoped_lock lock(m_mutex);
        return m_process_id;
    }

    // =========================================================================
    // LDN State
    // =========================================================================

    /**
     * @brief Store the current LDN communication state.
     * @param state New state.
     */
    void SetLdnState(CommState state) {
        std::scoped_lock lock(m_mutex);
        m_ldn_state = state;
    }

    /**
     * @brief Get the current LDN communication state.
     * @return The stored state (None after Reset).
     */
    CommState GetLdnState() const {
        std::scoped_lock lock(m_mutex);
        return m_ldn_state;
    }

    // =========================================================================
    // Session Info
    // =========================================================================

    /**
     * @brief Store the session summary shown by the overlay.
     * @param node_count Number of nodes currently connected.
     * @param max_nodes Maximum number of nodes of the session.
     * @param local_node_id Node id of this console.
     * @param is_host Whether this console hosts the session.
     */
    void SetSessionInfo(
        u8 node_count, u8 max_nodes, u8 local_node_id, bool is_host) {
        std::scoped_lock lock(m_mutex);
        m_node_count = node_count;
        m_max_nodes = max_nodes;
        m_local_node_id = local_node_id;
        m_is_host = is_host;
    }

    /**
     * @brief Read the session summary.
     * @param[out] node_count Number of nodes currently connected.
     * @param[out] max_nodes Maximum number of nodes of the session.
     * @param[out] local_node_id Node id of this console.
     * @param[out] is_host Whether this console hosts the session.
     */
    void GetSessionInfo(
        u8& node_count, u8& max_nodes, u8& local_node_id, bool& is_host) const {
        std::scoped_lock lock(m_mutex);
        node_count = m_node_count;
        max_nodes = m_max_nodes;
        local_node_id = m_local_node_id;
        is_host = m_is_host;
    }

    // =========================================================================
    // RTT (Round-Trip Time)
    // =========================================================================

    /**
     * @brief Store the last measured round-trip time.
     * @param rtt_ms Round-trip time in milliseconds.
     */
    void SetLastRtt(u32 rtt_ms) {
        std::scoped_lock lock(m_mutex);
        m_last_rtt_ms = rtt_ms;
    }

    /**
     * @brief Get the last measured round-trip time.
     * @return Round-trip time in milliseconds (0 when unknown).
     */
    u32 GetLastRtt() const {
        std::scoped_lock lock(m_mutex);
        return m_last_rtt_ms;
    }

    // =========================================================================
    // Reconnect Request
    // =========================================================================

    /// @brief Ask the network layer to reconnect; the request is a one-shot
    /// flag.
    void RequestReconnect() {
        std::scoped_lock lock(m_mutex);
        m_reconnect_requested = true;
    }

    /**
     * @brief Read and clear the reconnect request flag.
     * @return true if a reconnect had been requested since the last consume.
     */
    bool ConsumeReconnectRequest() {
        std::scoped_lock lock(m_mutex);
        bool was_requested = m_reconnect_requested;
        m_reconnect_requested = false;
        return was_requested;
    }

    // Non-copyable
    SharedState(const SharedState&) = delete;
    SharedState& operator=(const SharedState&) = delete;

private:
    SharedState() = default;

    /// Guards every field below.
    mutable std::mutex m_mutex;
    /// True while a game is running.
    bool m_game_active = false;
    /// Process id of the active game, 0 when none.
    u64 m_process_id = 0;
    /// Current LDN communication state.
    CommState m_ldn_state = CommState::None;
    /// Nodes currently connected to the session.
    u8 m_node_count = 0;
    /// Maximum node count of the session.
    u8 m_max_nodes = 0;
    /// Node id of this console in the session.
    u8 m_local_node_id = 0;
    /// True when this console hosts the session.
    bool m_is_host = false;
    /// Last measured round-trip time in milliseconds.
    u32 m_last_rtt_ms = 0;
    /// Pending reconnect request, cleared by ConsumeReconnectRequest().
    bool m_reconnect_requested = false;
};

// ============================================================================
// Singleton Tests
// ============================================================================

/// @brief GetInstance returns the same object on every call.
TEST(singleton_returns_same_instance) {
    auto& s1 = SharedState::GetInstance();
    auto& s2 = SharedState::GetInstance();
    ASSERT_EQ(&s1, &s2);
}

// ============================================================================
// Game Active State Tests
// ============================================================================

/// @brief After Reset no game is active.
TEST(initially_game_not_active) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    ASSERT_FALSE(state.IsGameActive());
}

/// @brief After Reset the active process id is 0.
TEST(initially_process_id_zero) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    ASSERT_EQ(state.GetActiveProcessId(), static_cast<u64>(0));
}

/// @brief SetGameActive(true, pid) marks the game active and stores the pid.
TEST(set_game_active_true) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    state.SetGameActive(true, 0x12345678);
    ASSERT_TRUE(state.IsGameActive());
    ASSERT_EQ(state.GetActiveProcessId(), static_cast<u64>(0x12345678));
}

/// @brief Deactivating the game clears the stored process id.
TEST(set_game_active_false_resets_pid) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    state.SetGameActive(true, 0x12345678);
    state.SetGameActive(false, 0);
    ASSERT_FALSE(state.IsGameActive());
    ASSERT_EQ(state.GetActiveProcessId(), static_cast<u64>(0));
}

/// @brief Deactivating the game puts the LDN state back to None.
TEST(set_game_active_false_resets_ldn_state) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    state.SetGameActive(true, 0x1234);
    state.SetLdnState(CommState::StationConnected);
    state.SetGameActive(false, 0);
    ASSERT_EQ(state.GetLdnState(), CommState::None);
}

/**
 * @brief Deactivating the game zeroes the session info (counts, node id and
 * host flag).
 */
TEST(set_game_active_false_resets_session_info) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    state.SetGameActive(true, 0x1234);
    state.SetSessionInfo(4, 8, 2, true);
    state.SetGameActive(false, 0);

    u8 count;
    u8 max;
    u8 local;
    bool is_host;
    state.GetSessionInfo(count, max, local, is_host);
    ASSERT_EQ(count, 0);
    ASSERT_EQ(max, 0);
    ASSERT_EQ(local, 0);
    ASSERT_FALSE(is_host);
}

// ============================================================================
// LDN State Tests
// ============================================================================

/// @brief After Reset the LDN state is None.
TEST(initially_ldn_state_none) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    ASSERT_EQ(state.GetLdnState(), CommState::None);
}

/// @brief SetLdnState(Initialized) is read back unchanged.
TEST(set_ldn_state_initialized) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    state.SetLdnState(CommState::Initialized);
    ASSERT_EQ(state.GetLdnState(), CommState::Initialized);
}

/// @brief SetLdnState(StationConnected) is read back unchanged.
TEST(set_ldn_state_station_connected) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    state.SetLdnState(CommState::StationConnected);
    ASSERT_EQ(state.GetLdnState(), CommState::StationConnected);
}

/// @brief SetLdnState(AccessPointCreated) is read back unchanged.
TEST(set_ldn_state_access_point_created) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    state.SetLdnState(CommState::AccessPointCreated);
    ASSERT_EQ(state.GetLdnState(), CommState::AccessPointCreated);
}

/// @brief SetLdnState(Error) is read back unchanged.
TEST(set_ldn_state_error) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    state.SetLdnState(CommState::Error);
    ASSERT_EQ(state.GetLdnState(), CommState::Error);
}

/**
 * @brief The state follows a typical station sequence Initialized, Station,
 * StationConnected, None without interference between steps.
 */
TEST(ldn_state_transitions) {
    auto& state = SharedState::GetInstance();
    state.Reset();

    state.SetLdnState(CommState::Initialized);
    ASSERT_EQ(state.GetLdnState(), CommState::Initialized);

    state.SetLdnState(CommState::Station);
    ASSERT_EQ(state.GetLdnState(), CommState::Station);

    state.SetLdnState(CommState::StationConnected);
    ASSERT_EQ(state.GetLdnState(), CommState::StationConnected);

    state.SetLdnState(CommState::None);
    ASSERT_EQ(state.GetLdnState(), CommState::None);
}

// ============================================================================
// Session Info Tests
// ============================================================================

/// @brief After Reset the session info is all zero and not host.
TEST(initially_session_info_empty) {
    auto& state = SharedState::GetInstance();
    state.Reset();

    u8 count;
    u8 max;
    u8 local;
    bool is_host;
    state.GetSessionInfo(count, max, local, is_host);

    ASSERT_EQ(count, 0);
    ASSERT_EQ(max, 0);
    ASSERT_EQ(local, 0);
    ASSERT_FALSE(is_host);
}

/// @brief Session info set as host is read back unchanged.
TEST(set_session_info_as_host) {
    auto& state = SharedState::GetInstance();
    state.Reset();

    state.SetSessionInfo(4, 8, 0, true);

    u8 count;
    u8 max;
    u8 local;
    bool is_host;
    state.GetSessionInfo(count, max, local, is_host);

    ASSERT_EQ(count, 4);
    ASSERT_EQ(max, 8);
    ASSERT_EQ(local, 0);
    ASSERT_TRUE(is_host);
}

/**
 * @brief Session info set as client (non-zero local node id, not host) is
 * read back unchanged.
 */
TEST(set_session_info_as_client) {
    auto& state = SharedState::GetInstance();
    state.Reset();

    state.SetSessionInfo(3, 8, 2, false);

    u8 count;
    u8 max;
    u8 local;
    bool is_host;
    state.GetSessionInfo(count, max, local, is_host);

    ASSERT_EQ(count, 3);
    ASSERT_EQ(max, 8);
    ASSERT_EQ(local, 2);
    ASSERT_FALSE(is_host);
}

/// @brief A second SetSessionInfo overwrites the node count (players joined).
TEST(update_session_info_node_count) {
    auto& state = SharedState::GetInstance();
    state.Reset();

    state.SetSessionInfo(2, 8, 0, true);
    state.SetSessionInfo(5, 8, 0, true);  // More players joined

    u8 count;
    u8 max;
    u8 local;
    bool is_host;
    state.GetSessionInfo(count, max, local, is_host);

    ASSERT_EQ(count, 5);
}

// ============================================================================
// RTT Tests
// ============================================================================

/// @brief After Reset the RTT is 0.
TEST(initially_rtt_zero) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    ASSERT_EQ(state.GetLastRtt(), static_cast<u32>(0));
}

/// @brief SetLastRtt is read back unchanged.
TEST(set_last_rtt) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    state.SetLastRtt(42);
    ASSERT_EQ(state.GetLastRtt(), static_cast<u32>(42));
}

/// @brief Each new RTT replaces the previous one, up or down.
TEST(update_last_rtt) {
    auto& state = SharedState::GetInstance();
    state.Reset();

    state.SetLastRtt(100);
    ASSERT_EQ(state.GetLastRtt(), static_cast<u32>(100));

    state.SetLastRtt(150);
    ASSERT_EQ(state.GetLastRtt(), static_cast<u32>(150));

    state.SetLastRtt(80);
    ASSERT_EQ(state.GetLastRtt(), static_cast<u32>(80));
}

/// @brief LAN, medium and high latency values are stored without clamping.
TEST(rtt_typical_values) {
    auto& state = SharedState::GetInstance();
    state.Reset();

    // Low latency (LAN)
    state.SetLastRtt(5);
    ASSERT_EQ(state.GetLastRtt(), static_cast<u32>(5));

    // Medium latency
    state.SetLastRtt(50);
    ASSERT_EQ(state.GetLastRtt(), static_cast<u32>(50));

    // High latency
    state.SetLastRtt(200);
    ASSERT_EQ(state.GetLastRtt(), static_cast<u32>(200));
}

// ============================================================================
// Reconnect Request Tests
// ============================================================================

/// @brief After Reset no reconnect request is pending.
TEST(initially_no_reconnect_request) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    ASSERT_FALSE(state.ConsumeReconnectRequest());
}

/// @brief RequestReconnect makes ConsumeReconnectRequest return true.
TEST(request_reconnect_sets_flag) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    state.RequestReconnect();
    ASSERT_TRUE(state.ConsumeReconnectRequest());
}

/**
 * @brief ConsumeReconnectRequest clears the flag, so the second call
 * returns false.
 */
TEST(consume_reconnect_clears_flag) {
    auto& state = SharedState::GetInstance();
    state.Reset();
    state.RequestReconnect();
    ASSERT_TRUE(state.ConsumeReconnectRequest());  // First consume
    ASSERT_FALSE(
        state.ConsumeReconnectRequest());  // Second consume should be false
}

/// @brief Several requests collapse into a single consume.
TEST(multiple_reconnect_requests) {
    auto& state = SharedState::GetInstance();
    state.Reset();

    state.RequestReconnect();
    state.RequestReconnect();
    state.RequestReconnect();

    // Should only consume once
    ASSERT_TRUE(state.ConsumeReconnectRequest());
    ASSERT_FALSE(state.ConsumeReconnectRequest());
}

/// @brief A new request after a consume is honored again.
TEST(reconnect_after_consume) {
    auto& state = SharedState::GetInstance();
    state.Reset();

    state.RequestReconnect();
    ASSERT_TRUE(state.ConsumeReconnectRequest());

    // New request after consumption
    state.RequestReconnect();
    ASSERT_TRUE(state.ConsumeReconnectRequest());
}

// ============================================================================
// Integration Tests
// ============================================================================

/**
 * @brief Client lifecycle: game start, LDN init, station, connected with
 * session info and RTT, then game exit resets the LDN state.
 */
TEST(full_game_session_lifecycle) {
    auto& state = SharedState::GetInstance();
    state.Reset();

    // Game starts
    state.SetGameActive(true, 0xABCD1234);
    ASSERT_TRUE(state.IsGameActive());
    ASSERT_EQ(state.GetActiveProcessId(), static_cast<u64>(0xABCD1234));

    // LDN initializes
    state.SetLdnState(CommState::Initialized);
    ASSERT_EQ(state.GetLdnState(), CommState::Initialized);

    // Opens station mode
    state.SetLdnState(CommState::Station);
    ASSERT_EQ(state.GetLdnState(), CommState::Station);

    // Connects to network
    state.SetLdnState(CommState::StationConnected);
    state.SetSessionInfo(4, 8, 2, false);

    u8 count;
    u8 max;
    u8 local;
    bool is_host;
    state.GetSessionInfo(count, max, local, is_host);
    ASSERT_EQ(count, 4);
    ASSERT_FALSE(is_host);

    // RTT updates
    state.SetLastRtt(45);
    ASSERT_EQ(state.GetLastRtt(), static_cast<u32>(45));

    // Game exits
    state.SetGameActive(false, 0);
    ASSERT_FALSE(state.IsGameActive());
    ASSERT_EQ(state.GetLdnState(), CommState::None);
}

/**
 * @brief Host lifecycle: access point created, session info updated as
 * players join, reconnect request consumed, then game exit.
 */
TEST(host_session_lifecycle) {
    auto& state = SharedState::GetInstance();
    state.Reset();

    // Game starts as host
    state.SetGameActive(true, 0x5678);
    state.SetLdnState(CommState::Initialized);
    state.SetLdnState(CommState::AccessPoint);
    state.SetLdnState(CommState::AccessPointCreated);

    state.SetSessionInfo(1, 8, 0, true);

    u8 count;
    u8 max;
    u8 local;
    bool is_host;
    state.GetSessionInfo(count, max, local, is_host);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(local, 0);
    ASSERT_TRUE(is_host);

    // Players join
    state.SetSessionInfo(3, 8, 0, true);
    state.GetSessionInfo(count, max, local, is_host);
    ASSERT_EQ(count, 3);

    // Force reconnect requested
    state.RequestReconnect();
    ASSERT_TRUE(state.ConsumeReconnectRequest());

    // Game ends
    state.SetGameActive(false, 0);
    ASSERT_FALSE(state.IsGameActive());
}

// ============================================================================
// Main
// ============================================================================

/**
 * @brief Print the banner and the totals (the tests ran during static
 * initialization).
 * @return 1 when at least one test failed, 0 otherwise.
 */
int main() {
    printf("\n========================================\n");
    printf("  SharedState Tests - ryu_ldn_nx\n");
    printf("========================================\n\n");

    // Tests run automatically via static initializers

    printf("\n========================================\n");
    printf(
        "  Results: %d/%d passed\n",
        g_tests_passed,
        g_tests_passed + g_tests_failed);
    printf("========================================\n\n");

    return g_tests_failed > 0 ? 1 : 0;
}
