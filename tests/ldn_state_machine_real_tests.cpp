/**
 * @file ldn_state_machine_real_tests.cpp
 * @brief Unit tests for the real LdnStateMachine (ldn/ldn_state_machine.cpp)
 *
 * Built against the host Stratosphere stand-in in tests/stubs. The older
 * ldn_state_machine_tests.cpp exercises a private copy of the logic; this
 * file exercises the production source.
 */

#include <bit>
#include <cstdio>
#include <cstring>
#include <vector>

#include "test_harness.hpp"
#include "ldn/ldn_state_machine.hpp"

using ams::mitm::ldn::CommState;
using ams::mitm::ldn::LdnStateMachine;
using ams::mitm::ldn::StateTransitionResult;

/// Short alias for the result enum returned by every transition.
using R = StateTransitionResult;

namespace {

/// One state-change notification captured by record().
struct Seen {
    /// State the machine left.
    CommState from;
    /// State the machine entered.
    CommState to;
    /// The user-data pointer registered with SetStateCallback.
    void* user;
};

/// Notifications received by record(), in delivery order.
std::vector<Seen> g_seen;

/**
 * @brief State-change callback appending each notification to g_seen.
 *
 * @param from State left.
 * @param to State entered.
 * @param user User-data pointer registered with the callback.
 */
void record(CommState from, CommState to, void* user) {
    g_seen.push_back({from, to, user});
}

}  // namespace

/// A freshly built machine starts in None with nothing initialized or active.
TEST(a_new_machine_is_in_none_and_not_initialized) {
    LdnStateMachine sm;
    ASSERT_TRUE(sm.GetState() == CommState::None);
    ASSERT_TRUE(sm.IsInState(CommState::None));
    ASSERT_FALSE(sm.IsInitialized());
    ASSERT_FALSE(sm.IsNetworkActive());
}

/// @brief The AP path Initialize..Finalize succeeds step by step; the
/// network is active only between CreateNetwork and DestroyNetwork.
TEST(the_access_point_path_walks_up_and_back_down) {
    LdnStateMachine sm;
    ASSERT_TRUE(sm.Initialize() == R::Success);
    ASSERT_TRUE(sm.IsInitialized());
    ASSERT_TRUE(sm.OpenAccessPoint() == R::Success);
    ASSERT_FALSE(sm.IsNetworkActive());
    ASSERT_TRUE(sm.CreateNetwork() == R::Success);
    ASSERT_TRUE(sm.IsNetworkActive());
    ASSERT_TRUE(sm.DestroyNetwork() == R::Success);
    ASSERT_TRUE(sm.IsInState(CommState::AccessPoint));
    ASSERT_TRUE(sm.CloseAccessPoint() == R::Success);
    ASSERT_TRUE(sm.IsInState(CommState::Initialized));
    ASSERT_TRUE(sm.Finalize() == R::Success);
    ASSERT_TRUE(sm.IsInState(CommState::None));
}

/// The station path mirrors the AP path; active only while connected.
TEST(the_station_path_walks_up_and_back_down) {
    LdnStateMachine sm;
    ASSERT_TRUE(sm.Initialize() == R::Success);
    ASSERT_TRUE(sm.OpenStation() == R::Success);
    ASSERT_FALSE(sm.IsNetworkActive());
    ASSERT_TRUE(sm.Connect() == R::Success);
    ASSERT_TRUE(sm.IsNetworkActive());
    ASSERT_TRUE(sm.Disconnect() == R::Success);
    ASSERT_TRUE(sm.IsInState(CommState::Station));
    ASSERT_TRUE(sm.CloseStation() == R::Success);
    ASSERT_TRUE(sm.IsInState(CommState::Initialized));
}

/// @brief Closing the AP or station while the network is up drops straight to
/// Initialized without a prior Destroy/Disconnect.
TEST(closing_from_the_connected_states_goes_straight_to_initialized) {
    LdnStateMachine ap;
    ap.Initialize();
    ap.OpenAccessPoint();
    ap.CreateNetwork();
    ASSERT_TRUE(ap.CloseAccessPoint() == R::Success);
    ASSERT_TRUE(ap.IsInState(CommState::Initialized));

    LdnStateMachine st;
    st.Initialize();
    st.OpenStation();
    st.Connect();
    ASSERT_TRUE(st.CloseStation() == R::Success);
    ASSERT_TRUE(st.IsInState(CommState::Initialized));
}

/// Re-requesting the current state yields AlreadyInState, not an error.
TEST(repeating_a_transition_reports_already_in_state) {
    LdnStateMachine sm;
    ASSERT_TRUE(sm.Finalize() == R::AlreadyInState);
    sm.Initialize();
    ASSERT_TRUE(sm.Initialize() == R::AlreadyInState);
    ASSERT_TRUE(sm.CloseAccessPoint() == R::AlreadyInState);
    ASSERT_TRUE(sm.CloseStation() == R::AlreadyInState);
    sm.OpenAccessPoint();
    ASSERT_TRUE(sm.OpenAccessPoint() == R::AlreadyInState);
    ASSERT_TRUE(sm.DestroyNetwork() == R::AlreadyInState);
    sm.CreateNetwork();
    ASSERT_TRUE(sm.CreateNetwork() == R::AlreadyInState);
    sm.DestroyNetwork();
    sm.CloseAccessPoint();
    sm.OpenStation();
    ASSERT_TRUE(sm.OpenStation() == R::AlreadyInState);
    ASSERT_TRUE(sm.Disconnect() == R::AlreadyInState);
    sm.Connect();
    ASSERT_TRUE(sm.Connect() == R::AlreadyInState);
    ASSERT_TRUE(sm.IsInState(CommState::StationConnected));
}

/// Out-of-order calls return InvalidTransition and never change the state.
TEST(transitions_from_the_wrong_state_are_invalid_and_leave_the_state_alone) {
    LdnStateMachine sm;
    ASSERT_TRUE(sm.OpenAccessPoint() == R::InvalidTransition);
    ASSERT_TRUE(sm.CloseAccessPoint() == R::InvalidTransition);
    ASSERT_TRUE(sm.CreateNetwork() == R::InvalidTransition);
    ASSERT_TRUE(sm.DestroyNetwork() == R::InvalidTransition);
    ASSERT_TRUE(sm.OpenStation() == R::InvalidTransition);
    ASSERT_TRUE(sm.CloseStation() == R::InvalidTransition);
    ASSERT_TRUE(sm.Connect() == R::InvalidTransition);
    ASSERT_TRUE(sm.Disconnect() == R::InvalidTransition);
    ASSERT_TRUE(sm.IsInState(CommState::None));

    sm.Initialize();
    sm.OpenAccessPoint();
    ASSERT_TRUE(sm.Initialize() == R::InvalidTransition);
    ASSERT_TRUE(sm.OpenStation() == R::InvalidTransition);
    ASSERT_TRUE(sm.CloseStation() == R::InvalidTransition);
    ASSERT_TRUE(sm.Connect() == R::InvalidTransition);
    ASSERT_TRUE(sm.IsInState(CommState::AccessPoint));
}

/// @brief SetError works from any state, then only Finalize leaves Error, so
/// Initialize is refused until then.
TEST(set_error_works_from_any_state_and_blocks_initialize_until_finalize) {
    LdnStateMachine sm;
    ASSERT_TRUE(sm.SetError() == R::Success);
    ASSERT_TRUE(sm.IsInState(CommState::Error));
    ASSERT_FALSE(sm.IsInitialized());
    ASSERT_TRUE(sm.SetError() == R::AlreadyInState);
    ASSERT_TRUE(sm.Initialize() == R::InvalidTransition);
    ASSERT_TRUE(sm.Finalize() == R::Success);
    ASSERT_TRUE(sm.Initialize() == R::Success);
}

/// @brief Each machine has its own non-zero state-change event handle that
/// stays stable across Initialize and SignalStateChange.
TEST(each_machine_owns_a_stable_distinct_state_change_event) {
    LdnStateMachine a;
    LdnStateMachine b;
    const ams::os::NativeHandle h = a.GetStateChangeEventHandle();
    ASSERT_TRUE(h != 0);
    a.Initialize();
    a.SignalStateChange();
    ASSERT_EQ(a.GetStateChangeEventHandle(), h);
    ASSERT_TRUE(b.GetStateChangeEventHandle() != h);
}

/// @brief The callback fires once per real change with old, new and user data;
/// no-op and invalid transitions are silent, and a null callback unhooks.
TEST(the_callback_sees_each_change_with_old_new_and_user_data) {
    g_seen.clear();
    LdnStateMachine sm;
    int token = 0;
    sm.SetStateCallback(record, &token);

    sm.Initialize();
    sm.OpenAccessPoint();
    sm.CreateNetwork();
    sm.DestroyNetwork();
    sm.CloseAccessPoint();
    sm.OpenStation();
    sm.Connect();
    sm.Disconnect();
    sm.CloseStation();
    sm.SetError();
    sm.Finalize();
    sm.Finalize();     // already None: no callback
    sm.OpenStation();  // invalid: no callback

    ASSERT_EQ(g_seen.size(), static_cast<size_t>(11));
    ASSERT_EQ(g_seen[0].from, CommState::None);
    ASSERT_EQ(g_seen[0].to, CommState::Initialized);
    ASSERT_EQ(g_seen[2].from, CommState::AccessPoint);
    ASSERT_EQ(g_seen[2].to, CommState::AccessPointCreated);
    ASSERT_EQ(g_seen[9].from, CommState::Initialized);
    ASSERT_EQ(g_seen[9].to, CommState::Error);
    ASSERT_EQ(g_seen[10].from, CommState::Error);
    ASSERT_EQ(g_seen[10].to, CommState::None);
    for (const Seen& s : g_seen) {
        ASSERT_TRUE(s.user == &token);
    }

    sm.SetStateCallback(nullptr, nullptr);
    g_seen.clear();
    sm.Initialize();
    ASSERT_EQ(g_seen.size(), static_cast<size_t>(0));
}

/// @brief StateToString/ResultToString name every enum value and return
/// "Unknown" for out-of-range input.
TEST(state_and_result_names_cover_every_value_and_unknown) {
    ASSERT_TRUE(
        std::strcmp(LdnStateMachine::StateToString(CommState::None), "None") ==
        0);
    ASSERT_TRUE(
        std::strcmp(
            LdnStateMachine::StateToString(CommState::Initialized),
            "Initialized") == 0);
    ASSERT_TRUE(
        std::strcmp(
            LdnStateMachine::StateToString(CommState::AccessPoint),
            "AccessPoint") == 0);
    ASSERT_TRUE(
        std::strcmp(
            LdnStateMachine::StateToString(CommState::AccessPointCreated),
            "AccessPointCreated") == 0);
    ASSERT_TRUE(
        std::strcmp(
            LdnStateMachine::StateToString(CommState::Station), "Station") ==
        0);
    ASSERT_TRUE(
        std::strcmp(
            LdnStateMachine::StateToString(CommState::StationConnected),
            "StationConnected") == 0);
    ASSERT_TRUE(
        std::strcmp(
            LdnStateMachine::StateToString(CommState::Error), "Error") == 0);
    // bit_cast builds the out-of-enumerator state without a cast the
    // EnumCastOutOfRange check would reject; the enum type itself accepts any
    // value of its fixed underlying type.
    const auto unknown_state = std::bit_cast<CommState>(99U);
    ASSERT_TRUE(
        std::strcmp(LdnStateMachine::StateToString(unknown_state), "Unknown") ==
        0);

    ASSERT_TRUE(
        std::strcmp(LdnStateMachine::ResultToString(R::Success), "Success") ==
        0);
    ASSERT_TRUE(
        std::strcmp(
            LdnStateMachine::ResultToString(R::InvalidTransition),
            "InvalidTransition") == 0);
    ASSERT_TRUE(
        std::strcmp(
            LdnStateMachine::ResultToString(R::AlreadyInState),
            "AlreadyInState") == 0);
    // bit_cast, as for unknown_state: a result that matches no enumerator.
    const auto unknown_result = std::bit_cast<R>(99);
    ASSERT_TRUE(
        std::strcmp(
            LdnStateMachine::ResultToString(unknown_result), "Unknown") == 0);
}

/// @brief Print the totals.
///
/// @return Process exit code, non-zero when a test failed.
int main() {
    return test_report("LdnStateMachine (real) Tests");
}
