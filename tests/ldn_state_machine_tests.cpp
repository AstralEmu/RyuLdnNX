/**
 * @file ldn_state_machine_tests.cpp
 * @brief Unit tests for LDN State Machine logic
 *
 * Since the actual LdnStateMachine class depends on stratosphere (Switch-only),
 * we test the state transition logic using a standalone test implementation
 * that mirrors the same state machine behavior.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#include <cstdio>
#include <cstring>
#include <cstdint>

// ============================================================================
// Test Framework (minimal)
// ============================================================================

/** Count of tests that ran to completion (incremented by the TEST registrar).
 */
static int g_tests_passed = 0;
/** Count of failed assertions; main() returns non-zero when above 0. */
static int g_tests_failed = 0;

/**
 * @brief Declare a test and run it from a static initializer.
 *
 * Defines `test_<name>()` plus a static registrar object whose constructor
 * prints the name, runs the body before main() and bumps g_tests_passed.
 *
 * @param name Identifier of the test (token-pasted into the function name).
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
 * @brief Fail the current test (print and return) if cond is false.
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

/**
 * @brief Fail the current test if cond is true (negated ASSERT_TRUE).
 *
 * @param cond Expression expected to be false.
 */
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

/**
 * @brief Fail the current test (print and return) if a != b.
 *
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
// Standalone State Machine Implementation (mirrors LdnStateMachine)
// ============================================================================

/** Communication states, mirrors the real CommState of ldn_types.hpp. */
enum class CommState : uint32_t {
    None = 0,
    Initialized = 1,
    AccessPoint = 2,
    AccessPointCreated = 3,
    Station = 4,
    StationConnected = 5,
    Error = 6
};

/** Outcome of a state transition request. */
enum class StateTransitionResult { Success, InvalidTransition, AlreadyInState };

/**
 * @brief Test version of LDN State Machine
 *
 * Implements the same state transition logic as the real LdnStateMachine
 * but without stratosphere dependencies.
 */
class TestLdnStateMachine {
public:
    /** Start in None with the state-change event not signaled. */
    TestLdnStateMachine() = default;

    /** @return Current state. */
    [[nodiscard]] CommState GetState() const {
        return m_state;
    }
    /** @return true if a transition happened since the last ClearEventFlag().
     */
    [[nodiscard]] bool WasEventSignaled() const {
        return m_event_signaled;
    }
    /** Reset the simulated state-change event flag. */
    void ClearEventFlag() {
        m_event_signaled = false;
    }

    // State queries
    /**
     * @param state State to compare against.
     * @return true if the machine is exactly in that state.
     */
    [[nodiscard]] bool IsInState(CommState state) const {
        return m_state == state;
    }
    /** @return true in every state except None and Error. */
    [[nodiscard]] bool IsInitialized() const {
        return m_state != CommState::None && m_state != CommState::Error;
    }
    /** @return true when a network exists (AccessPointCreated or
     * StationConnected). */
    [[nodiscard]] bool IsNetworkActive() const {
        return m_state == CommState::AccessPointCreated ||
               m_state == CommState::StationConnected;
    }

    // Transitions
    /**
     * @brief None -> Initialized.
     *
     * @return Success, or InvalidTransition if not in None.
     */
    StateTransitionResult Initialize() {
        if (m_state != CommState::None) {
            return StateTransitionResult::InvalidTransition;
        }
        return TransitionTo(CommState::Initialized);
    }

    /**
     * @brief Any state -> None.
     *
     * @return Success, or AlreadyInState if already None.
     */
    StateTransitionResult Finalize() {
        // Can finalize from any state
        return TransitionTo(CommState::None);
    }

    /**
     * @brief Initialized -> AccessPoint.
     *
     * @return Success, or InvalidTransition from any other state.
     */
    StateTransitionResult OpenAccessPoint() {
        if (m_state != CommState::Initialized) {
            return StateTransitionResult::InvalidTransition;
        }
        return TransitionTo(CommState::AccessPoint);
    }

    /**
     * @brief AccessPoint or AccessPointCreated -> Initialized.
     *
     * @return Success, or InvalidTransition from any other state.
     */
    StateTransitionResult CloseAccessPoint() {
        if (m_state != CommState::AccessPoint &&
            m_state != CommState::AccessPointCreated) {
            return StateTransitionResult::InvalidTransition;
        }
        return TransitionTo(CommState::Initialized);
    }

    /**
     * @brief AccessPoint -> AccessPointCreated.
     *
     * @return Success, or InvalidTransition from any other state.
     */
    StateTransitionResult CreateNetwork() {
        if (m_state != CommState::AccessPoint) {
            return StateTransitionResult::InvalidTransition;
        }
        return TransitionTo(CommState::AccessPointCreated);
    }

    /**
     * @brief AccessPointCreated -> AccessPoint.
     *
     * @return Success, or InvalidTransition from any other state.
     */
    StateTransitionResult DestroyNetwork() {
        if (m_state != CommState::AccessPointCreated) {
            return StateTransitionResult::InvalidTransition;
        }
        return TransitionTo(CommState::AccessPoint);
    }

    /**
     * @brief Initialized -> Station.
     *
     * @return Success, or InvalidTransition from any other state.
     */
    StateTransitionResult OpenStation() {
        if (m_state != CommState::Initialized) {
            return StateTransitionResult::InvalidTransition;
        }
        return TransitionTo(CommState::Station);
    }

    /**
     * @brief Station or StationConnected -> Initialized.
     *
     * @return Success, or InvalidTransition from any other state.
     */
    StateTransitionResult CloseStation() {
        if (m_state != CommState::Station &&
            m_state != CommState::StationConnected) {
            return StateTransitionResult::InvalidTransition;
        }
        return TransitionTo(CommState::Initialized);
    }

    /**
     * @brief Station -> StationConnected.
     *
     * @return Success, or InvalidTransition from any other state.
     */
    StateTransitionResult Connect() {
        if (m_state != CommState::Station) {
            return StateTransitionResult::InvalidTransition;
        }
        return TransitionTo(CommState::StationConnected);
    }

    /**
     * @brief StationConnected -> Station.
     *
     * @return Success, or InvalidTransition from any other state.
     */
    StateTransitionResult Disconnect() {
        if (m_state != CommState::StationConnected) {
            return StateTransitionResult::InvalidTransition;
        }
        return TransitionTo(CommState::Station);
    }

    /**
     * @brief Any state -> Error.
     *
     * @return Success, or AlreadyInState if already in Error.
     */
    StateTransitionResult SetError() {
        if (m_state == CommState::Error) {
            return StateTransitionResult::AlreadyInState;
        }
        return TransitionTo(CommState::Error);
    }

private:
    /**
     * @brief Apply a transition and signal the event.
     *
     * @param new_state Target state.
     * @return Success, or AlreadyInState (event untouched) if unchanged.
     */
    StateTransitionResult TransitionTo(CommState new_state) {
        if (m_state == new_state) {
            return StateTransitionResult::AlreadyInState;
        }
        m_state = new_state;
        m_event_signaled = true;
        return StateTransitionResult::Success;
    }

    /** Current state. */
    CommState m_state{CommState::None};
    /** Set by every successful transition, cleared by ClearEventFlag(). */
    bool m_event_signaled{false};
};

// ============================================================================
// Initial State Tests
// ============================================================================

/** A new machine starts in None. */
TEST(initial_state_is_none) {
    TestLdnStateMachine sm;
    ASSERT_EQ(sm.GetState(), CommState::None);
}

/** None does not count as initialized. */
TEST(initial_not_initialized) {
    TestLdnStateMachine sm;
    ASSERT_FALSE(sm.IsInitialized());
}

/** No network is active in the initial state. */
TEST(initial_not_network_active) {
    TestLdnStateMachine sm;
    ASSERT_FALSE(sm.IsNetworkActive());
}

/** The state-change event is not signaled before any transition. */
TEST(initial_event_not_signaled) {
    TestLdnStateMachine sm;
    ASSERT_FALSE(sm.WasEventSignaled());
}

// ============================================================================
// Initialize Tests
// ============================================================================

/** Initialize from None succeeds and lands in Initialized. */
TEST(initialize_from_none_succeeds) {
    TestLdnStateMachine sm;
    auto result = sm.Initialize();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::Initialized);
}

/** A successful transition signals the state-change event the game waits on. */
TEST(initialize_signals_event) {
    TestLdnStateMachine sm;
    sm.Initialize();
    ASSERT_TRUE(sm.WasEventSignaled());
}

/** Initializing twice is rejected as InvalidTransition. */
TEST(initialize_from_initialized_fails) {
    TestLdnStateMachine sm;
    sm.Initialize();
    auto result = sm.Initialize();
    ASSERT_EQ(result, StateTransitionResult::InvalidTransition);
}

/** IsInitialized is true once Initialize succeeded. */
TEST(is_initialized_after_init) {
    TestLdnStateMachine sm;
    sm.Initialize();
    ASSERT_TRUE(sm.IsInitialized());
}

// ============================================================================
// Finalize Tests
// ============================================================================

/** Finalize from Initialized returns to None. */
TEST(finalize_from_initialized_succeeds) {
    TestLdnStateMachine sm;
    sm.Initialize();
    auto result = sm.Finalize();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::None);
}

/** Finalize is allowed from AccessPoint (game exits without closing). */
TEST(finalize_from_access_point_succeeds) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenAccessPoint();
    auto result = sm.Finalize();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::None);
}

/** Finalize is allowed from StationConnected (game exits while connected). */
TEST(finalize_from_station_connected_succeeds) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenStation();
    sm.Connect();
    auto result = sm.Finalize();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::None);
}

/** Finalize from None reports AlreadyInState instead of failing. */
TEST(finalize_from_none_already_in_state) {
    TestLdnStateMachine sm;
    auto result = sm.Finalize();
    ASSERT_EQ(result, StateTransitionResult::AlreadyInState);
}

// ============================================================================
// Access Point Flow Tests
// ============================================================================

/** OpenAccessPoint from Initialized lands in AccessPoint. */
TEST(open_access_point_from_initialized) {
    TestLdnStateMachine sm;
    sm.Initialize();
    auto result = sm.OpenAccessPoint();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::AccessPoint);
}

/** OpenAccessPoint needs Initialization first (InvalidTransition). */
TEST(open_access_point_from_none_fails) {
    TestLdnStateMachine sm;
    auto result = sm.OpenAccessPoint();
    ASSERT_EQ(result, StateTransitionResult::InvalidTransition);
}

/** CreateNetwork from AccessPoint lands in AccessPointCreated. */
TEST(create_network_from_access_point) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenAccessPoint();
    auto result = sm.CreateNetwork();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::AccessPointCreated);
}

/** CreateNetwork without OpenAccessPoint is InvalidTransition. */
TEST(create_network_from_initialized_fails) {
    TestLdnStateMachine sm;
    sm.Initialize();
    auto result = sm.CreateNetwork();
    ASSERT_EQ(result, StateTransitionResult::InvalidTransition);
}

/** A created host network counts as active. */
TEST(is_network_active_when_ap_created) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenAccessPoint();
    sm.CreateNetwork();
    ASSERT_TRUE(sm.IsNetworkActive());
}

/** DestroyNetwork goes back to AccessPoint. */
TEST(destroy_network_from_ap_created) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenAccessPoint();
    sm.CreateNetwork();
    auto result = sm.DestroyNetwork();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::AccessPoint);
}

/** DestroyNetwork without a created network is InvalidTransition. */
TEST(destroy_network_from_access_point_fails) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenAccessPoint();
    auto result = sm.DestroyNetwork();
    ASSERT_EQ(result, StateTransitionResult::InvalidTransition);
}

/** CloseAccessPoint from AccessPoint returns to Initialized. */
TEST(close_access_point_from_ap) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenAccessPoint();
    auto result = sm.CloseAccessPoint();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::Initialized);
}

/** CloseAccessPoint also works while the network exists. */
TEST(close_access_point_from_ap_created) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenAccessPoint();
    sm.CreateNetwork();
    auto result = sm.CloseAccessPoint();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::Initialized);
}

/** CloseAccessPoint from Station is rejected: AP and station modes are
 * distinct. */
TEST(close_access_point_from_station_fails) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenStation();
    auto result = sm.CloseAccessPoint();
    ASSERT_EQ(result, StateTransitionResult::InvalidTransition);
}

// ============================================================================
// Station Flow Tests
// ============================================================================

/** OpenStation from Initialized lands in Station. */
TEST(open_station_from_initialized) {
    TestLdnStateMachine sm;
    sm.Initialize();
    auto result = sm.OpenStation();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::Station);
}

/** OpenStation needs Initialization first (InvalidTransition). */
TEST(open_station_from_none_fails) {
    TestLdnStateMachine sm;
    auto result = sm.OpenStation();
    ASSERT_EQ(result, StateTransitionResult::InvalidTransition);
}

/** Connect from Station lands in StationConnected. */
TEST(connect_from_station) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenStation();
    auto result = sm.Connect();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::StationConnected);
}

/** Connect without OpenStation is InvalidTransition. */
TEST(connect_from_initialized_fails) {
    TestLdnStateMachine sm;
    sm.Initialize();
    auto result = sm.Connect();
    ASSERT_EQ(result, StateTransitionResult::InvalidTransition);
}

/** A connected station counts as network active. */
TEST(is_network_active_when_connected) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenStation();
    sm.Connect();
    ASSERT_TRUE(sm.IsNetworkActive());
}

/** Disconnect returns StationConnected to Station. */
TEST(disconnect_from_connected) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenStation();
    sm.Connect();
    auto result = sm.Disconnect();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::Station);
}

/** Disconnect when not connected is InvalidTransition. */
TEST(disconnect_from_station_fails) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenStation();
    auto result = sm.Disconnect();
    ASSERT_EQ(result, StateTransitionResult::InvalidTransition);
}

/** CloseStation from Station returns to Initialized. */
TEST(close_station_from_station) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenStation();
    auto result = sm.CloseStation();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::Initialized);
}

/** CloseStation also works while connected. */
TEST(close_station_from_connected) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenStation();
    sm.Connect();
    auto result = sm.CloseStation();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::Initialized);
}

/** CloseStation from AccessPoint is rejected: modes are distinct. */
TEST(close_station_from_ap_fails) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenAccessPoint();
    auto result = sm.CloseStation();
    ASSERT_EQ(result, StateTransitionResult::InvalidTransition);
}

// ============================================================================
// Cannot Mix AP and Station Modes Tests
// ============================================================================

/** A machine in AccessPoint cannot also open a station. */
TEST(cannot_open_station_from_access_point) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenAccessPoint();
    auto result = sm.OpenStation();
    ASSERT_EQ(result, StateTransitionResult::InvalidTransition);
}

/** A machine in Station cannot also open an access point. */
TEST(cannot_open_access_point_from_station) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenStation();
    auto result = sm.OpenAccessPoint();
    ASSERT_EQ(result, StateTransitionResult::InvalidTransition);
}

// ============================================================================
// Error State Tests
// ============================================================================

/** SetError moves any state (here AccessPoint) to Error. */
TEST(set_error_from_any_state) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.OpenAccessPoint();
    auto result = sm.SetError();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::Error);
}

/** SetError on an Error machine reports AlreadyInState. */
TEST(set_error_already_in_error) {
    TestLdnStateMachine sm;
    sm.SetError();
    auto result = sm.SetError();
    ASSERT_EQ(result, StateTransitionResult::AlreadyInState);
}

/** Error does not count as initialized. */
TEST(not_initialized_when_error) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.SetError();
    ASSERT_FALSE(sm.IsInitialized());
}

/** Finalize is the way out of Error, back to None. */
TEST(finalize_from_error_succeeds) {
    TestLdnStateMachine sm;
    sm.Initialize();
    sm.SetError();
    auto result = sm.Finalize();
    ASSERT_EQ(result, StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::None);
}

// ============================================================================
// Full Flow Tests
// ============================================================================

/** Walks the whole host lifecycle Initialize to Finalize checking every
 * intermediate state. */
TEST(full_host_flow) {
    TestLdnStateMachine sm;

    // Initialize
    ASSERT_EQ(sm.Initialize(), StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::Initialized);

    // Open AP
    ASSERT_EQ(sm.OpenAccessPoint(), StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::AccessPoint);

    // Create network
    ASSERT_EQ(sm.CreateNetwork(), StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::AccessPointCreated);
    ASSERT_TRUE(sm.IsNetworkActive());

    // Destroy network
    ASSERT_EQ(sm.DestroyNetwork(), StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::AccessPoint);

    // Close AP
    ASSERT_EQ(sm.CloseAccessPoint(), StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::Initialized);

    // Finalize
    ASSERT_EQ(sm.Finalize(), StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::None);
}

/** Walks the whole client lifecycle Initialize to Finalize checking every
 * intermediate state. */
TEST(full_client_flow) {
    TestLdnStateMachine sm;

    // Initialize
    ASSERT_EQ(sm.Initialize(), StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::Initialized);

    // Open Station
    ASSERT_EQ(sm.OpenStation(), StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::Station);

    // Connect
    ASSERT_EQ(sm.Connect(), StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::StationConnected);
    ASSERT_TRUE(sm.IsNetworkActive());

    // Disconnect
    ASSERT_EQ(sm.Disconnect(), StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::Station);

    // Close Station
    ASSERT_EQ(sm.CloseStation(), StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::Initialized);

    // Finalize
    ASSERT_EQ(sm.Finalize(), StateTransitionResult::Success);
    ASSERT_EQ(sm.GetState(), CommState::None);
}

/** The event flag can be cleared and is signaled again by the next transition.
 */
TEST(event_cleared_and_resignaled) {
    TestLdnStateMachine sm;

    sm.Initialize();
    ASSERT_TRUE(sm.WasEventSignaled());

    sm.ClearEventFlag();
    ASSERT_FALSE(sm.WasEventSignaled());

    sm.OpenAccessPoint();
    ASSERT_TRUE(sm.WasEventSignaled());
}

// ============================================================================
// IsInState Tests
// ============================================================================

/** IsInState is true only for the exact current state, before and after a
 * transition. */
TEST(is_in_state_correct) {
    TestLdnStateMachine sm;
    ASSERT_TRUE(sm.IsInState(CommState::None));
    ASSERT_FALSE(sm.IsInState(CommState::Initialized));

    sm.Initialize();
    ASSERT_FALSE(sm.IsInState(CommState::None));
    ASSERT_TRUE(sm.IsInState(CommState::Initialized));
}

// ============================================================================
// Main
// ============================================================================

/**
 * @brief Test entry point.
 *
 * Tests already ran from static initializers; this only prints the summary.
 *
 * @return 1 if any assertion failed, 0 otherwise.
 */
int main() {
    printf("\n========================================\n");
    printf("  LDN State Machine Tests - ryu_ldn_nx\n");
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
