/**
 * @file ldn_config_service_tests.cpp
 * @brief Unit tests for the real LdnConfigService (ldn/ldn_config_service.cpp)
 *
 * Built against the host Stratosphere stand-in in tests/stubs; the service
 * runs on the real SharedState and ConfigManager singletons.
 */

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "test_harness.hpp"
#include "ldn/ldn_config_service.hpp"
#include "ldn/ldn_shared_state.hpp"
#include "config/config_manager.hpp"

using ams::mitm::ldn::CommState;
using ams::mitm::ldn::ConfigResult;
using ams::mitm::ldn::ConnectionStatus;
using ams::mitm::ldn::ICommunicationService;
using ams::mitm::ldn::LdnConfigService;
using ams::mitm::ldn::Passphrase;
using ams::mitm::ldn::ServerAddress;
using ams::mitm::ldn::SessionInfo;
using ams::mitm::ldn::SharedState;
using ams::sf::Out;
using ryu_ldn::config::ConfigManager;

namespace {

/**
 * @brief Calls an IPC getter and returns what it wrote
 *
 * @param fn LdnConfigService getter taking an Out<T>
 * @return the value written by the getter (value-initialized if the call
 *         failed)
 */
template <class T>
T get(ams::Result (*fn)(Out<T>)) {
    T value{};
    // ASSERT_* returns from the enclosing function: run it in a void lambda so
    // the value is still handed back after a failure has been recorded.
    [&] { ASSERT_TRUE(fn(Out<T>(value)).IsSuccess()); }();
    return value;
}

/**
 * @brief The shared state, cleared
 *
 * @return the SharedState singleton, reset and with no pending reconnect
 *         request
 */
SharedState& fresh_state() {
    SharedState& s = SharedState::GetInstance();
    s.Reset();
    s.ConsumeReconnectRequest();
    return s;
}

/**
 * @brief Unique config file path in /tmp, not created
 *
 * @return path of a not yet existing file
 */
std::string temp_config_path() {
    char path[128];
    std::snprintf(
        path,
        sizeof(path),
        "/tmp/ldn_config_service_%d_%d.ini",
        std::rand(),
        0);
    return path;
}

}  // namespace

/**
 * @brief Verifies that SaveConfig and ReloadConfig report IoError while
 *        ConfigManager is not initialized. Must stay first: it relies on
 *        ConfigManager::Initialize() not having run yet.
 */
TEST(saving_or_reloading_before_the_config_is_initialized_reports_io_error) {
    ASSERT_TRUE(
        get<ConfigResult>(&LdnConfigService::SaveConfig) ==
        ConfigResult::IoError);
    ASSERT_TRUE(
        get<ConfigResult>(&LdnConfigService::ReloadConfig) ==
        ConfigResult::IoError);
}

/**
 * @brief Verifies that the service can be constructed with a null parent and
 *        with a parent communication service.
 */
TEST(the_service_can_be_built_with_or_without_a_parent) {
    LdnConfigService without(nullptr);
    LdnConfigService with(reinterpret_cast<ICommunicationService*>(&without));
    (void)with;
    ASSERT_TRUE(true);
}

/// Verifies that GetVersion returns a non-empty, NUL-terminated string.
TEST(the_version_is_a_terminated_string) {
    auto version = get<std::array<char, 32>>(&LdnConfigService::GetVersion);
    ASSERT_TRUE(version[31] == '\0');
    ASSERT_TRUE(std::strlen(version.data()) > 0);
}

/**
 * @brief Verifies the CommState to ConnectionStatus mapping of
 *        GetConnectionStatus: None and Error are Disconnected, Initialized is
 *        Connecting, AccessPoint and Station are Connected, the two
 *        session-established states are Ready.
 */
TEST(the_connection_status_follows_the_ldn_state) {
    SharedState& s = fresh_state();
    struct Case {
        CommState state;
        ConnectionStatus expected;
    };
    const Case cases[] = {
        {.state = CommState::None, .expected = ConnectionStatus::Disconnected},
        {.state = CommState::Error, .expected = ConnectionStatus::Disconnected},
        {.state = CommState::Initialized,
         .expected = ConnectionStatus::Connecting},
        {.state = CommState::AccessPoint,
         .expected = ConnectionStatus::Connected},
        {.state = CommState::Station, .expected = ConnectionStatus::Connected},
        {.state = CommState::AccessPointCreated,
         .expected = ConnectionStatus::Ready},
        {.state = CommState::StationConnected,
         .expected = ConnectionStatus::Ready},
    };
    for (const Case& c : cases) {
        s.SetLdnState(c.state);
        ASSERT_TRUE(
            get<ConnectionStatus>(&LdnConfigService::GetConnectionStatus) ==
            c.expected);
    }
}

/**
 * @brief Verifies that GetLdnState and GetLastRtt return the raw values held by
 *        SharedState.
 */
TEST(the_raw_ldn_state_and_rtt_come_from_the_shared_state) {
    SharedState& s = fresh_state();
    s.SetLdnState(CommState::Station);
    s.SetLastRtt(77);
    ASSERT_EQ(
        get<u32>(&LdnConfigService::GetLdnState),
        static_cast<u32>(CommState::Station));
    ASSERT_EQ(get<u32>(&LdnConfigService::GetLastRtt), static_cast<u32>(77));
}

/**
 * @brief Verifies that GetSessionInfo returns the node count, node count max,
 *        local node id and host flag set in SharedState.
 */
TEST(the_session_info_comes_from_the_shared_state) {
    SharedState& s = fresh_state();
    s.SetSessionInfo(3, 8, 1, true);
    auto info = get<SessionInfo>(&LdnConfigService::GetSessionInfo);
    ASSERT_EQ(info.node_count, 3);
    ASSERT_EQ(info.node_count_max, 8);
    ASSERT_EQ(info.local_node_id, 1);
    ASSERT_EQ(info.is_host, 1);
}

/**
 * @brief Verifies that ForceReconnect succeeds and raises the reconnect request
 *        in SharedState.
 */
TEST(force_reconnect_requests_a_reconnection) {
    SharedState& s = fresh_state();
    ASSERT_TRUE(LdnConfigService::ForceReconnect().IsSuccess());
    ASSERT_TRUE(s.ConsumeReconnectRequest());
}

/**
 * @brief Verifies that initializing ConfigManager on a missing file succeeds
 *        and that ReloadConfig then reports FileNotFound.
 */
TEST(the_config_is_initialized_on_a_missing_file_and_reloads_as_not_found) {
    const std::string path = temp_config_path();
    ASSERT_TRUE(ConfigManager::Instance().Initialize(path.c_str()));
    ASSERT_TRUE(
        get<ConfigResult>(&LdnConfigService::ReloadConfig) ==
        ConfigResult::FileNotFound);
}

/**
 * @brief Verifies that SetServerAddress stores the address (read back by
 *        GetServerAddress) and asks for a reconnect only when an LDN session is
 *        active (Station), not in None or Initialized.
 */
TEST(setting_the_server_address_stores_it_and_reconnects_only_when_in_session) {
    SharedState& s = fresh_state();
    ServerAddress addr{};
    std::strcpy(addr.host, "ldn.example.org");
    addr.port = 31337;

    s.SetLdnState(CommState::None);
    ASSERT_TRUE(LdnConfigService::SetServerAddress(addr).IsSuccess());
    ASSERT_FALSE(s.ConsumeReconnectRequest());
    s.SetLdnState(CommState::Initialized);
    ASSERT_TRUE(LdnConfigService::SetServerAddress(addr).IsSuccess());
    ASSERT_FALSE(s.ConsumeReconnectRequest());
    s.SetLdnState(CommState::Station);
    ASSERT_TRUE(LdnConfigService::SetServerAddress(addr).IsSuccess());
    ASSERT_TRUE(s.ConsumeReconnectRequest());

    auto got = get<ServerAddress>(&LdnConfigService::GetServerAddress);
    ASSERT_TRUE(std::strcmp(got.host, "ldn.example.org") == 0);
    ASSERT_EQ(got.port, 31337);
}

/**
 * @brief Verifies the debug settings: SetDebugEnabled and SetLogToFile read
 *        back consistently, any non-zero value counts as 1, and SetDebugLevel
 *        clamps values above 3 to 3.
 */
TEST(debug_settings_round_trip) {
    ASSERT_TRUE(LdnConfigService::SetDebugEnabled(1).IsSuccess());
    ASSERT_EQ(
        get<u32>(&LdnConfigService::GetDebugEnabled), static_cast<u32>(1));
    ASSERT_EQ(get<u32>(&LdnConfigService::GetLogToFile), static_cast<u32>(1));
    ASSERT_TRUE(LdnConfigService::SetDebugEnabled(0).IsSuccess());
    ASSERT_EQ(
        get<u32>(&LdnConfigService::GetDebugEnabled), static_cast<u32>(0));

    ASSERT_TRUE(LdnConfigService::SetLogToFile(5).IsSuccess());
    ASSERT_EQ(
        get<u32>(&LdnConfigService::GetDebugEnabled), static_cast<u32>(1));
    ASSERT_TRUE(LdnConfigService::SetLogToFile(0).IsSuccess());
    ASSERT_EQ(get<u32>(&LdnConfigService::GetLogToFile), static_cast<u32>(0));

    ASSERT_TRUE(LdnConfigService::SetDebugLevel(2).IsSuccess());
    ASSERT_EQ(get<u32>(&LdnConfigService::GetDebugLevel), static_cast<u32>(2));
    ASSERT_TRUE(LdnConfigService::SetDebugLevel(99).IsSuccess());
    ASSERT_EQ(get<u32>(&LdnConfigService::GetDebugLevel), static_cast<u32>(3));
}

/**
 * @brief Verifies that the LDN-enabled flag and the passphrase are stored and
 *        read back unchanged.
 */
TEST(ldn_enabled_and_passphrase_round_trip) {
    ASSERT_TRUE(LdnConfigService::SetLdnEnabled(0).IsSuccess());
    ASSERT_EQ(get<u32>(&LdnConfigService::GetLdnEnabled), static_cast<u32>(0));
    ASSERT_TRUE(LdnConfigService::SetLdnEnabled(1).IsSuccess());
    ASSERT_EQ(get<u32>(&LdnConfigService::GetLdnEnabled), static_cast<u32>(1));

    Passphrase p{};
    std::strcpy(p.passphrase, "Ryujinx-0123abcd");
    ASSERT_TRUE(LdnConfigService::SetPassphrase(p).IsSuccess());
    auto got = get<Passphrase>(&LdnConfigService::GetPassphrase);
    ASSERT_TRUE(std::strcmp(got.passphrase, "Ryujinx-0123abcd") == 0);
}

/**
 * @brief Verifies that the connect timeout (5000), ping interval (0), reconnect
 *        delay (1000) and max reconnect attempts (0) are fixed values and that
 *        their setters succeed without changing them.
 */
TEST(the_fixed_network_settings_keep_their_documented_values) {
    ASSERT_EQ(
        get<u32>(&LdnConfigService::GetConnectTimeout), static_cast<u32>(5000));
    ASSERT_EQ(
        get<u32>(&LdnConfigService::GetPingInterval), static_cast<u32>(0));
    ASSERT_EQ(
        get<u32>(&LdnConfigService::GetReconnectDelay), static_cast<u32>(1000));
    ASSERT_EQ(
        get<u32>(&LdnConfigService::GetMaxReconnectAttempts),
        static_cast<u32>(0));
    // The setters exist for IPC compatibility and change nothing.
    ASSERT_TRUE(LdnConfigService::SetConnectTimeout(1).IsSuccess());
    ASSERT_TRUE(LdnConfigService::SetPingInterval(1).IsSuccess());
    ASSERT_TRUE(LdnConfigService::SetReconnectDelay(1).IsSuccess());
    ASSERT_TRUE(LdnConfigService::SetMaxReconnectAttempts(1).IsSuccess());
    ASSERT_EQ(
        get<u32>(&LdnConfigService::GetConnectTimeout), static_cast<u32>(5000));
    ASSERT_EQ(
        get<u32>(&LdnConfigService::GetPingInterval), static_cast<u32>(0));
    ASSERT_EQ(
        get<u32>(&LdnConfigService::GetReconnectDelay), static_cast<u32>(1000));
    ASSERT_EQ(
        get<u32>(&LdnConfigService::GetMaxReconnectAttempts),
        static_cast<u32>(0));
}

/**
 * @brief Verifies that SaveConfig and ReloadConfig succeed and that a reload
 *        restores the value saved earlier (debug level 1) over a later change
 *        (3).
 */
TEST(save_then_reload_report_success_and_keep_the_saved_values) {
    ASSERT_TRUE(LdnConfigService::SetDebugLevel(1).IsSuccess());
    ASSERT_TRUE(
        get<ConfigResult>(&LdnConfigService::SaveConfig) ==
        ConfigResult::Success);
    ASSERT_TRUE(LdnConfigService::SetDebugLevel(3).IsSuccess());
    ASSERT_TRUE(
        get<ConfigResult>(&LdnConfigService::ReloadConfig) ==
        ConfigResult::Success);
    ASSERT_EQ(get<u32>(&LdnConfigService::GetDebugLevel), static_cast<u32>(1));
}

/**
 * @brief Verifies that ToIpcResult maps each ConfigResult to its IPC
 *        counterpart and an unknown value to IoError.
 */
TEST(every_config_file_result_maps_to_its_ipc_counterpart) {
    using Src = ryu_ldn::config::ConfigResult;
    ASSERT_TRUE(
        LdnConfigService::ToIpcResult(Src::Success) == ConfigResult::Success);
    ASSERT_TRUE(
        LdnConfigService::ToIpcResult(Src::FileNotFound) ==
        ConfigResult::FileNotFound);
    ASSERT_TRUE(
        LdnConfigService::ToIpcResult(Src::ParseError) ==
        ConfigResult::ParseError);
    ASSERT_TRUE(
        LdnConfigService::ToIpcResult(Src::IoError) == ConfigResult::IoError);
    ASSERT_TRUE(
        LdnConfigService::ToIpcResult(std::bit_cast<Src>(99)) ==
        ConfigResult::IoError);
}

/**
 * @brief Runs every registered test of this suite
 *
 * @return 1 if at least one test failed, 0 otherwise
 */
int main() {
    return test_report("LdnConfigService Tests");
}
