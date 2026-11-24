/**
 * @file ldn_error_handling_tests.cpp
 * @brief Unit tests for LDN error handling and recovery
 *
 * Tests for Story 3.8: Gestion erreurs et recovery
 * - Connection loss detection
 * - Error state transitions
 * - Disconnect reason tracking
 * - Recovery scenarios
 * - Timeout handling
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>

// ============================================================================
// Test Framework (minimal standalone)
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
    static struct TestRegistrar_##name {      \
        TestRegistrar_##name() {              \
            printf("  [TEST] " #name "... "); \
            fflush(stdout);                   \
            test_##name();                    \
            g_tests_passed++;                 \
            printf("PASS\n");                 \
        }                                     \
    } g_registrar_##name;                     \
    static void test_##name()

/**
 * @brief Fail the current test (print and return) if cond is false.
 *
 * @param cond Expression expected to be true.
 */
#define ASSERT_TRUE(cond)                                                   \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf(                                                         \
                "FAIL\n    Expected: " #cond " to be true\n    at %s:%d\n", \
                __FILE__,                                                   \
                __LINE__);                                                  \
            g_tests_failed++;                                               \
            return;                                                         \
        }                                                                   \
    } while (0)

/**
 * @brief Fail the current test (print and return) if cond is true.
 *
 * @param cond Expression expected to be false.
 */
#define ASSERT_FALSE(cond)                                                   \
    do {                                                                     \
        if (cond) {                                                          \
            printf(                                                          \
                "FAIL\n    Expected: " #cond " to be false\n    at %s:%d\n", \
                __FILE__,                                                    \
                __LINE__);                                                   \
            g_tests_failed++;                                                \
            return;                                                          \
        }                                                                    \
    } while (0)

/**
 * @brief Fail the current test (print and return) if a != b.
 *
 * @param a Actual value.
 * @param b Expected value.
 */
#define ASSERT_EQ(a, b)                                                 \
    do {                                                                \
        if ((a) != (b)) {                                               \
            printf(                                                     \
                "FAIL\n    Expected: " #a " == " #b "\n    at %s:%d\n", \
                __FILE__,                                               \
                __LINE__);                                              \
            g_tests_failed++;                                           \
            return;                                                     \
        }                                                               \
    } while (0)

/**
 * @brief Fail the current test (print and return) if a == b.
 *
 * @param a Actual value.
 * @param b Value that a must differ from.
 */
#define ASSERT_NE(a, b)                                                 \
    do {                                                                \
        if ((a) == (b)) {                                               \
            printf(                                                     \
                "FAIL\n    Expected: " #a " != " #b "\n    at %s:%d\n", \
                __FILE__,                                               \
                __LINE__);                                              \
            g_tests_failed++;                                           \
            return;                                                     \
        }                                                               \
    } while (0)

// ============================================================================
// Error Types (mirrors ldn_types.hpp)
// ============================================================================

/**
 * @brief Disconnect reasons
 */
enum class DisconnectReason : uint32_t {
    None = 0,               ///< No disconnect
    User = 1,               ///< User initiated disconnect
    SystemRequest = 2,      ///< System requested disconnect
    DestroyedByUser = 3,    ///< Network destroyed by host
    DestroyedBySystem = 4,  ///< Network destroyed by system
    Rejected = 5,           ///< Connection rejected
    ConnectionFailed = 6,   ///< Failed to establish connection
    SignalLost = 7,         ///< Lost signal/connection
};

/**
 * @brief Communication states
 */
enum class CommState : uint32_t {
    None = 0,
    Initialized = 1,
    AccessPoint = 2,
    AccessPointCreated = 3,
    Station = 4,
    StationConnected = 5,
    Error = 6
};

/**
 * @brief Error codes returned by LDN operations
 */
enum class LdnErrorCode : uint32_t {
    Success = 0,
    InvalidState = 0x100001,      ///< Operation not valid in current state
    NotConnected = 0x100002,      ///< Not connected to server
    SendFailed = 0x100003,        ///< Failed to send data
    Timeout = 0x100004,           ///< Operation timed out
    ServerError = 0x100005,       ///< Server returned error
    NetworkError = 0x100006,      ///< Network error occurred
    InvalidParameter = 0x100007,  ///< Invalid parameter
};

// ============================================================================
// Test Error Handler (simulates the error handling we'll implement)
// ============================================================================

/**
 * @brief Simulates error handling logic for LDN service
 */
class TestErrorHandler {
public:
    /** Callback fired when an error is signalled to the game. */
    using ErrorCallback = std::function<void(LdnErrorCode, DisconnectReason)>;
    /** Callback fired on every state change made through SetState(). */
    using StateCallback = std::function<void(CommState)>;

    TestErrorHandler() : m_error_callback(nullptr), m_state_callback(nullptr) {}

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Set how many connection losses are retried in setup states.
     *
     * @param max Retry budget before the handler goes to the Error state.
     */
    void SetMaxRetries(uint32_t max) {
        m_max_retries = max;
    }
    /**
     * @brief Register the error observer (may be nullptr).
     *
     * @param cb Called with the error code and disconnect reason.
     */
    void SetErrorCallback(ErrorCallback cb) {
        m_error_callback = std::move(cb);
    }
    /**
     * @brief Register the state-change observer (may be nullptr).
     *
     * @param cb Called with the new state on every SetState().
     */
    void SetStateCallback(StateCallback cb) {
        m_state_callback = std::move(cb);
    }

    // ========================================================================
    // State Management
    // ========================================================================

    /** @return Current communication state. */
    [[nodiscard]] CommState GetState() const {
        return m_current_state;
    }
    /** @return Reason recorded by the last error, None when clean. */
    [[nodiscard]] DisconnectReason GetDisconnectReason() const {
        return m_disconnect_reason;
    }
    /** @return Last error code recorded, Success when clean. */
    [[nodiscard]] LdnErrorCode GetLastError() const {
        return m_last_error;
    }
    /** @return Number of retries consumed since the last ResetError(). */
    [[nodiscard]] uint32_t GetRetryCount() const {
        return m_retry_count;
    }
    /** @return true once HandleConnectionLoss() has been called. */
    [[nodiscard]] bool IsConnectionLost() const {
        return m_connection_lost;
    }

    /**
     * @brief Move to a new state and notify the state callback.
     *
     * @param state New state to enter.
     */
    void SetState(CommState state) {
        m_current_state = state;
        if (m_state_callback) {
            m_state_callback(state);
        }
    }

    // ========================================================================
    // Error Handling
    // ========================================================================

    /**
     * @brief Handle connection loss event
     *
     * Called when TCP connection to server is lost.
     * Decides whether to retry or signal error to game.
     *
     * @return true if retry should be attempted
     */
    bool HandleConnectionLoss() {
        m_connection_lost = true;

        // Different behavior based on current state
        switch (m_current_state) {
            case CommState::None:
            case CommState::Initialized:
                // Not in active session, no error to report
                m_disconnect_reason = DisconnectReason::None;
                return false;

            case CommState::AccessPoint:
            case CommState::Station:
                // In setup phase, can retry
                if (m_retry_count < m_max_retries) {
                    m_retry_count++;
                    return true;
                }
                // Max retries exceeded
                m_last_error = LdnErrorCode::NetworkError;
                m_disconnect_reason = DisconnectReason::ConnectionFailed;
                SetState(CommState::Error);
                NotifyError(m_last_error, m_disconnect_reason);
                return false;

            case CommState::AccessPointCreated:
            case CommState::StationConnected:
                // In active session, signal loss to game
                m_last_error = LdnErrorCode::NetworkError;
                m_disconnect_reason = DisconnectReason::SignalLost;
                SetState(CommState::Error);
                NotifyError(m_last_error, m_disconnect_reason);
                return false;

            case CommState::Error:
            default:
                // Already in error state, or nothing to signal
                return false;
        }
    }

    /**
     * @brief Handle operation timeout
     *
     * @param operation Name of operation that timed out
     * @return LdnErrorCode to return to caller
     */
    LdnErrorCode HandleTimeout(const char* operation) {
        (void)operation;  // For logging in real implementation

        m_last_error = LdnErrorCode::Timeout;

        // Timeout during active session is connection loss
        if (m_current_state == CommState::AccessPointCreated ||
            m_current_state == CommState::StationConnected) {
            m_disconnect_reason = DisconnectReason::SignalLost;
            SetState(CommState::Error);
            NotifyError(m_last_error, m_disconnect_reason);
        }

        return LdnErrorCode::Timeout;
    }

    /**
     * @brief Handle server error response
     *
     * @param server_error_code Error code from server
     * @return LdnErrorCode to return to caller
     */
    LdnErrorCode HandleServerError(uint32_t server_error_code) {
        m_last_error = LdnErrorCode::ServerError;

        // Map server errors to disconnect reasons
        switch (server_error_code) {
            case 1:  // Rejected
                m_disconnect_reason = DisconnectReason::Rejected;
                break;
            case 2:  // Network destroyed
                m_disconnect_reason = DisconnectReason::DestroyedBySystem;
                break;
            default:
                m_disconnect_reason = DisconnectReason::SystemRequest;
                break;
        }

        // If in active session, transition to error
        if (m_current_state == CommState::AccessPointCreated ||
            m_current_state == CommState::StationConnected) {
            SetState(CommState::Error);
            NotifyError(m_last_error, m_disconnect_reason);
        }

        return LdnErrorCode::ServerError;
    }

    /**
     * @brief Reset error state after recovery
     */
    void ResetError() {
        m_last_error = LdnErrorCode::Success;
        m_disconnect_reason = DisconnectReason::None;
        m_retry_count = 0;
        m_connection_lost = false;
    }

    /**
     * @brief Check if in recoverable error state
     *
     * @return true if recovery is possible
     */
    [[nodiscard]] bool CanRecover() const {
        // Can only recover from certain disconnect reasons
        return m_current_state == CommState::Error &&
               (m_disconnect_reason == DisconnectReason::ConnectionFailed ||
                m_disconnect_reason == DisconnectReason::SignalLost);
    }

private:
    /**
     * @brief Invoke the error callback if one is registered.
     *
     * @param error Error code to report.
     * @param reason Disconnect reason to report.
     */
    void NotifyError(LdnErrorCode error, DisconnectReason reason) {
        if (m_error_callback) {
            m_error_callback(error, reason);
        }
    }

    /** Current communication state. */
    CommState m_current_state{CommState::None};
    /** Reason of the last disconnect. */
    DisconnectReason m_disconnect_reason{DisconnectReason::None};
    /** Last error code recorded. */
    LdnErrorCode m_last_error{LdnErrorCode::Success};
    /** Retries consumed so far. */
    uint32_t m_retry_count{0};
    /** Maximum retries allowed in setup states. */
    uint32_t m_max_retries{3};
    /** Set by HandleConnectionLoss(), cleared by ResetError(). */
    bool m_connection_lost{false};
    /** Observer notified on errors, may be empty. */
    ErrorCallback m_error_callback;
    /** Observer notified on state changes, may be empty. */
    StateCallback m_state_callback;
};

// ============================================================================
// Initial State Tests
// ============================================================================

/** A fresh handler is in None state with no error, no reason and zero retries,
 * so later tests start from a known baseline. */
TEST(error_handler_initial_state) {
    TestErrorHandler handler;
    ASSERT_EQ(handler.GetState(), CommState::None);
    ASSERT_EQ(handler.GetDisconnectReason(), DisconnectReason::None);
    ASSERT_EQ(handler.GetLastError(), LdnErrorCode::Success);
    ASSERT_EQ(handler.GetRetryCount(), 0U);
    ASSERT_FALSE(handler.IsConnectionLost());
}

// ============================================================================
// Connection Loss Tests
// ============================================================================

/** A connection loss while in None is not an active session: no retry, no state
 * change and no disconnect reason. */
TEST(connection_loss_from_none_no_error) {
    TestErrorHandler handler;
    handler.SetState(CommState::None);

    bool should_retry = handler.HandleConnectionLoss();

    ASSERT_FALSE(should_retry);
    ASSERT_EQ(handler.GetState(), CommState::None);
    ASSERT_EQ(handler.GetDisconnectReason(), DisconnectReason::None);
}

/** A connection loss in Initialized neither retries nor changes state. */
TEST(connection_loss_from_initialized_no_error) {
    TestErrorHandler handler;
    handler.SetState(CommState::Initialized);

    bool should_retry = handler.HandleConnectionLoss();

    ASSERT_FALSE(should_retry);
    ASSERT_EQ(handler.GetState(), CommState::Initialized);
}

/** A first loss in the AccessPoint setup phase asks for a retry, bumps the
 * retry count to 1 and keeps the state. */
TEST(connection_loss_from_access_point_retry) {
    TestErrorHandler handler;
    handler.SetState(CommState::AccessPoint);
    handler.SetMaxRetries(3);

    // First loss: should retry
    bool should_retry = handler.HandleConnectionLoss();
    ASSERT_TRUE(should_retry);
    ASSERT_EQ(handler.GetRetryCount(), 1U);
    ASSERT_EQ(handler.GetState(), CommState::AccessPoint);  // State unchanged
}

/** Once the retry budget (2) is spent, the next loss goes to Error with
 * ConnectionFailed and no further retry. */
TEST(connection_loss_max_retries_exceeded) {
    TestErrorHandler handler;
    handler.SetState(CommState::AccessPoint);
    handler.SetMaxRetries(2);

    handler.HandleConnectionLoss();  // Retry 1
    handler.HandleConnectionLoss();  // Retry 2
    bool should_retry =
        handler.HandleConnectionLoss();  // Retry 3 - exceeds max

    ASSERT_FALSE(should_retry);
    ASSERT_EQ(handler.GetState(), CommState::Error);
    ASSERT_EQ(
        handler.GetDisconnectReason(), DisconnectReason::ConnectionFailed);
}

/** A loss while StationConnected is unrecoverable by retry: Error state,
 * SignalLost reason and NetworkError code. */
TEST(connection_loss_during_active_session) {
    TestErrorHandler handler;
    handler.SetState(CommState::StationConnected);

    bool should_retry = handler.HandleConnectionLoss();

    ASSERT_FALSE(should_retry);
    ASSERT_EQ(handler.GetState(), CommState::Error);
    ASSERT_EQ(handler.GetDisconnectReason(), DisconnectReason::SignalLost);
    ASSERT_EQ(handler.GetLastError(), LdnErrorCode::NetworkError);
}

/** A loss while hosting (AccessPointCreated) also goes straight to Error with
 * SignalLost. */
TEST(connection_loss_during_host_session) {
    TestErrorHandler handler;
    handler.SetState(CommState::AccessPointCreated);

    bool should_retry = handler.HandleConnectionLoss();

    ASSERT_FALSE(should_retry);
    ASSERT_EQ(handler.GetState(), CommState::Error);
    ASSERT_EQ(handler.GetDisconnectReason(), DisconnectReason::SignalLost);
}

/** The error callback receives NetworkError and SignalLost when an active
 * session loses its connection. */
TEST(connection_loss_callback_invoked) {
    TestErrorHandler handler;
    handler.SetState(CommState::StationConnected);

    bool callback_called = false;
    LdnErrorCode received_error = LdnErrorCode::Success;
    DisconnectReason received_reason = DisconnectReason::None;

    handler.SetErrorCallback([&](LdnErrorCode err, DisconnectReason reason) {
        callback_called = true;
        received_error = err;
        received_reason = reason;
    });

    handler.HandleConnectionLoss();

    ASSERT_TRUE(callback_called);
    ASSERT_EQ(received_error, LdnErrorCode::NetworkError);
    ASSERT_EQ(received_reason, DisconnectReason::SignalLost);
}

// ============================================================================
// Timeout Tests
// ============================================================================

/** HandleTimeout returns Timeout and records it as last error, also outside an
 * active session. */
TEST(timeout_returns_error_code) {
    TestErrorHandler handler;
    handler.SetState(CommState::Station);

    LdnErrorCode result = handler.HandleTimeout("Scan");

    ASSERT_EQ(result, LdnErrorCode::Timeout);
    ASSERT_EQ(handler.GetLastError(), LdnErrorCode::Timeout);
}

/** A timeout during an active session is treated as lost signal: Error state
 * with SignalLost. */
TEST(timeout_during_session_triggers_error_state) {
    TestErrorHandler handler;
    handler.SetState(CommState::StationConnected);

    handler.HandleTimeout("ProxyData");

    ASSERT_EQ(handler.GetState(), CommState::Error);
    ASSERT_EQ(handler.GetDisconnectReason(), DisconnectReason::SignalLost);
}

/** A timeout during setup (AccessPoint) records Timeout but leaves the state
 * alone so the caller can retry. */
TEST(timeout_during_setup_no_state_change) {
    TestErrorHandler handler;
    handler.SetState(CommState::AccessPoint);

    handler.HandleTimeout("CreateNetwork");

    ASSERT_EQ(handler.GetState(), CommState::AccessPoint);
    ASSERT_EQ(handler.GetLastError(), LdnErrorCode::Timeout);
}

// ============================================================================
// Server Error Tests
// ============================================================================

/** Server error code 1 maps to the Rejected disconnect reason and ServerError
 * result. */
TEST(server_error_rejected) {
    TestErrorHandler handler;
    handler.SetState(CommState::Station);

    LdnErrorCode result = handler.HandleServerError(1);  // Rejected

    ASSERT_EQ(result, LdnErrorCode::ServerError);
    ASSERT_EQ(handler.GetDisconnectReason(), DisconnectReason::Rejected);
}

/** Server error code 2 in an active session maps to DestroyedBySystem and moves
 * to Error. */
TEST(server_error_network_destroyed) {
    TestErrorHandler handler;
    handler.SetState(CommState::StationConnected);

    handler.HandleServerError(2);  // Network destroyed

    ASSERT_EQ(handler.GetState(), CommState::Error);
    ASSERT_EQ(
        handler.GetDisconnectReason(), DisconnectReason::DestroyedBySystem);
}

/** Unknown server error codes fall back to the SystemRequest disconnect reason.
 */
TEST(server_error_unknown_maps_to_system_request) {
    TestErrorHandler handler;
    handler.SetState(CommState::StationConnected);

    handler.HandleServerError(99);  // Unknown error

    ASSERT_EQ(handler.GetDisconnectReason(), DisconnectReason::SystemRequest);
}

// ============================================================================
// Recovery Tests
// ============================================================================

/** ResetError clears error, reason, retry count and the connection-lost flag
 * after an exhausted-retry failure. */
TEST(reset_error_clears_state) {
    TestErrorHandler handler;
    handler.SetState(CommState::AccessPoint);
    handler.SetMaxRetries(2);        // Set max to 2 for faster test
    handler.HandleConnectionLoss();  // Retry 1
    handler.HandleConnectionLoss();  // Retry 2
    handler.HandleConnectionLoss();  // Exceeds max (2), triggers error

    ASSERT_EQ(handler.GetState(), CommState::Error);

    handler.ResetError();

    ASSERT_EQ(handler.GetLastError(), LdnErrorCode::Success);
    ASSERT_EQ(handler.GetDisconnectReason(), DisconnectReason::None);
    ASSERT_EQ(handler.GetRetryCount(), 0U);
    ASSERT_FALSE(handler.IsConnectionLost());
}

/** Error with ConnectionFailed (retries disabled) is recoverable. */
TEST(can_recover_from_connection_failed) {
    TestErrorHandler handler;
    handler.SetState(CommState::Station);
    handler.SetMaxRetries(0);  // No retries

    handler.HandleConnectionLoss();

    ASSERT_EQ(handler.GetState(), CommState::Error);
    ASSERT_EQ(
        handler.GetDisconnectReason(), DisconnectReason::ConnectionFailed);
    ASSERT_TRUE(handler.CanRecover());
}

/** Error with SignalLost is recoverable. */
TEST(can_recover_from_signal_lost) {
    TestErrorHandler handler;
    handler.SetState(CommState::StationConnected);

    handler.HandleConnectionLoss();

    ASSERT_EQ(handler.GetDisconnectReason(), DisconnectReason::SignalLost);
    ASSERT_TRUE(handler.CanRecover());
}

/** Error with Rejected is final: CanRecover must be false. */
TEST(cannot_recover_from_rejected) {
    TestErrorHandler handler;
    handler.SetState(CommState::StationConnected);

    handler.HandleServerError(1);  // Rejected

    ASSERT_EQ(handler.GetDisconnectReason(), DisconnectReason::Rejected);
    ASSERT_FALSE(handler.CanRecover());
}

/** CanRecover is false outside the Error state. */
TEST(cannot_recover_when_not_in_error) {
    TestErrorHandler handler;
    handler.SetState(CommState::Initialized);

    ASSERT_FALSE(handler.CanRecover());
}

// ============================================================================
// State Callback Tests
// ============================================================================

/** The state callback sees the Error state when a connection loss forces the
 * transition. */
TEST(state_callback_on_error_transition) {
    TestErrorHandler handler;
    handler.SetState(CommState::StationConnected);

    CommState last_state = CommState::None;
    handler.SetStateCallback([&](CommState state) { last_state = state; });

    handler.HandleConnectionLoss();

    ASSERT_EQ(last_state, CommState::Error);
}

// ============================================================================
// Edge Cases
// ============================================================================

/** Successive losses in Station accumulate in the retry count while the budget
 * (5) lasts, without leaving the state. */
TEST(multiple_connection_losses_accumulate_retries) {
    TestErrorHandler handler;
    handler.SetState(CommState::Station);
    handler.SetMaxRetries(5);

    for (int i = 0; i < 3; i++) {
        handler.HandleConnectionLoss();
    }

    ASSERT_EQ(handler.GetRetryCount(), 3U);
    ASSERT_EQ(handler.GetState(), CommState::Station);  // Still trying
}

/** A second loss while already in Error changes nothing (state stays Error). */
TEST(error_from_error_state_no_change) {
    TestErrorHandler handler;
    handler.SetState(CommState::StationConnected);
    handler.HandleConnectionLoss();

    CommState state_before = handler.GetState();
    handler.HandleConnectionLoss();  // Second loss

    ASSERT_EQ(handler.GetState(), state_before);
    ASSERT_EQ(handler.GetState(), CommState::Error);
}

/** Clearing both callbacks with nullptr must not crash when an error is
 * signalled. */
TEST(null_callbacks_safe) {
    TestErrorHandler handler;
    handler.SetState(CommState::StationConnected);
    handler.SetErrorCallback(nullptr);
    handler.SetStateCallback(nullptr);

    // Should not crash
    handler.HandleConnectionLoss();

    ASSERT_EQ(handler.GetState(), CommState::Error);
}

// ============================================================================
// Disconnect Reason Values
// ============================================================================

/** Pins the numeric values of DisconnectReason (0-7) to the LDN protocol
 * values. */
TEST(disconnect_reason_values) {
    // Verify enum values match Nintendo's LDN protocol
    ASSERT_EQ(static_cast<uint32_t>(DisconnectReason::None), 0U);
    ASSERT_EQ(static_cast<uint32_t>(DisconnectReason::User), 1U);
    ASSERT_EQ(static_cast<uint32_t>(DisconnectReason::SystemRequest), 2U);
    ASSERT_EQ(static_cast<uint32_t>(DisconnectReason::DestroyedByUser), 3U);
    ASSERT_EQ(static_cast<uint32_t>(DisconnectReason::DestroyedBySystem), 4U);
    ASSERT_EQ(static_cast<uint32_t>(DisconnectReason::Rejected), 5U);
    ASSERT_EQ(static_cast<uint32_t>(DisconnectReason::ConnectionFailed), 6U);
    ASSERT_EQ(static_cast<uint32_t>(DisconnectReason::SignalLost), 7U);
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
    printf("  LDN Error Handling Tests - ryu_ldn_nx\n");
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
