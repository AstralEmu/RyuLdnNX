/**
 * @file ldn_config_service_tests.cpp
 * @brief Unit tests for the real LdnConfigService (ldn/ldn_config_service.cpp)
 *
 * Built against the host Stratosphere stand-in in tests/stubs; the service
 * runs on the real SharedState and ConfigManager singletons.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "test_harness.hpp"
#include "ldn/ldn_config_service.hpp"
#include "ldn/ldn_shared_state.hpp"
#include "config/config_manager.hpp"

using namespace ams::mitm::ldn;
using ams::sf::Out;
using ryu_ldn::config::ConfigManager;

namespace {

template <class T>
T get(ams::Result (*fn)(Out<T>)) {
    T value{};
    // ASSERT_* returns from the enclosing function: run it in a void lambda so
    // the value is still handed back after a failure has been recorded.
    [&] {
        ASSERT_TRUE(fn(Out<T>(value)).IsSuccess());
    }();
    return value;
}

SharedState& fresh_state() {
    SharedState& s = SharedState::GetInstance();
    s.Reset();
    s.ConsumeReconnectRequest();
    return s;
}

std::string temp_config_path() {
    char path[128];
    std::snprintf(path, sizeof(path), "/tmp/ldn_config_service_%d_%d.ini", static_cast<int>(std::rand()), 0);
    return path;
}

}  // namespace

// Must stay first: it relies on ConfigManager::Initialize() not having run yet.
TEST(saving_or_reloading_before_the_config_is_initialized_reports_io_error) {
    ASSERT_TRUE(get<ConfigResult>(&LdnConfigService::SaveConfig) == ConfigResult::IoError);
    ASSERT_TRUE(get<ConfigResult>(&LdnConfigService::ReloadConfig) == ConfigResult::IoError);
}

TEST(the_service_can_be_built_with_or_without_a_parent) {
    LdnConfigService without(nullptr);
    LdnConfigService with(reinterpret_cast<ICommunicationService*>(&without));
    (void)with;
    ASSERT_TRUE(true);
}

TEST(the_version_is_a_terminated_string) {
    auto version = get<std::array<char, 32>>(&LdnConfigService::GetVersion);
    ASSERT_TRUE(version[31] == '\0');
    ASSERT_TRUE(std::strlen(version.data()) > 0);
}

TEST(the_connection_status_follows_the_ldn_state) {
    SharedState& s = fresh_state();
    struct Case { CommState state; ConnectionStatus expected; };
    const Case cases[] = {
        {CommState::None, ConnectionStatus::Disconnected},
        {CommState::Error, ConnectionStatus::Disconnected},
        {CommState::Initialized, ConnectionStatus::Connecting},
        {CommState::AccessPoint, ConnectionStatus::Connected},
        {CommState::Station, ConnectionStatus::Connected},
        {CommState::AccessPointCreated, ConnectionStatus::Ready},
        {CommState::StationConnected, ConnectionStatus::Ready},
    };
    for (const Case& c : cases) {
        s.SetLdnState(c.state);
        ASSERT_TRUE(get<ConnectionStatus>(&LdnConfigService::GetConnectionStatus) == c.expected);
    }
}

TEST(the_raw_ldn_state_and_rtt_come_from_the_shared_state) {
    SharedState& s = fresh_state();
    s.SetLdnState(CommState::Station);
    s.SetLastRtt(77);
    ASSERT_EQ(get<u32>(&LdnConfigService::GetLdnState), static_cast<u32>(CommState::Station));
    ASSERT_EQ(get<u32>(&LdnConfigService::GetLastRtt), static_cast<u32>(77));
}

TEST(the_session_info_comes_from_the_shared_state) {
    SharedState& s = fresh_state();
    s.SetSessionInfo(3, 8, 1, true);
    auto info = get<SessionInfo>(&LdnConfigService::GetSessionInfo);
    ASSERT_EQ(info.node_count, 3);
    ASSERT_EQ(info.node_count_max, 8);
    ASSERT_EQ(info.local_node_id, 1);
    ASSERT_EQ(info.is_host, 1);
}

TEST(force_reconnect_requests_a_reconnection) {
    SharedState& s = fresh_state();
    ASSERT_TRUE(LdnConfigService::ForceReconnect().IsSuccess());
    ASSERT_TRUE(s.ConsumeReconnectRequest());
}

TEST(the_config_is_initialized_on_a_missing_file_and_reloads_as_not_found) {
    const std::string path = temp_config_path();
    ASSERT_TRUE(ConfigManager::Instance().Initialize(path.c_str()));
    ASSERT_TRUE(get<ConfigResult>(&LdnConfigService::ReloadConfig) == ConfigResult::FileNotFound);
}

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

TEST(debug_settings_round_trip) {
    ASSERT_TRUE(LdnConfigService::SetDebugEnabled(1).IsSuccess());
    ASSERT_EQ(get<u32>(&LdnConfigService::GetDebugEnabled), static_cast<u32>(1));
    ASSERT_EQ(get<u32>(&LdnConfigService::GetLogToFile), static_cast<u32>(1));
    ASSERT_TRUE(LdnConfigService::SetDebugEnabled(0).IsSuccess());
    ASSERT_EQ(get<u32>(&LdnConfigService::GetDebugEnabled), static_cast<u32>(0));

    ASSERT_TRUE(LdnConfigService::SetLogToFile(5).IsSuccess());
    ASSERT_EQ(get<u32>(&LdnConfigService::GetDebugEnabled), static_cast<u32>(1));
    ASSERT_TRUE(LdnConfigService::SetLogToFile(0).IsSuccess());
    ASSERT_EQ(get<u32>(&LdnConfigService::GetLogToFile), static_cast<u32>(0));

    ASSERT_TRUE(LdnConfigService::SetDebugLevel(2).IsSuccess());
    ASSERT_EQ(get<u32>(&LdnConfigService::GetDebugLevel), static_cast<u32>(2));
    ASSERT_TRUE(LdnConfigService::SetDebugLevel(99).IsSuccess());
    ASSERT_EQ(get<u32>(&LdnConfigService::GetDebugLevel), static_cast<u32>(3));
}

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

TEST(the_fixed_network_settings_keep_their_documented_values) {
    ASSERT_EQ(get<u32>(&LdnConfigService::GetConnectTimeout), static_cast<u32>(5000));
    ASSERT_EQ(get<u32>(&LdnConfigService::GetPingInterval), static_cast<u32>(0));
    ASSERT_EQ(get<u32>(&LdnConfigService::GetReconnectDelay), static_cast<u32>(1000));
    ASSERT_EQ(get<u32>(&LdnConfigService::GetMaxReconnectAttempts), static_cast<u32>(0));
    // The setters exist for IPC compatibility and change nothing.
    ASSERT_TRUE(LdnConfigService::SetConnectTimeout(1).IsSuccess());
    ASSERT_TRUE(LdnConfigService::SetPingInterval(1).IsSuccess());
    ASSERT_TRUE(LdnConfigService::SetReconnectDelay(1).IsSuccess());
    ASSERT_TRUE(LdnConfigService::SetMaxReconnectAttempts(1).IsSuccess());
    ASSERT_EQ(get<u32>(&LdnConfigService::GetConnectTimeout), static_cast<u32>(5000));
    ASSERT_EQ(get<u32>(&LdnConfigService::GetPingInterval), static_cast<u32>(0));
    ASSERT_EQ(get<u32>(&LdnConfigService::GetReconnectDelay), static_cast<u32>(1000));
    ASSERT_EQ(get<u32>(&LdnConfigService::GetMaxReconnectAttempts), static_cast<u32>(0));
}

TEST(save_then_reload_report_success_and_keep_the_saved_values) {
    ASSERT_TRUE(LdnConfigService::SetDebugLevel(1).IsSuccess());
    ASSERT_TRUE(get<ConfigResult>(&LdnConfigService::SaveConfig) == ConfigResult::Success);
    ASSERT_TRUE(LdnConfigService::SetDebugLevel(3).IsSuccess());
    ASSERT_TRUE(get<ConfigResult>(&LdnConfigService::ReloadConfig) == ConfigResult::Success);
    ASSERT_EQ(get<u32>(&LdnConfigService::GetDebugLevel), static_cast<u32>(1));
}

TEST(every_config_file_result_maps_to_its_ipc_counterpart) {
    using Src = ryu_ldn::config::ConfigResult;
    ASSERT_TRUE(LdnConfigService::ToIpcResult(Src::Success) == ConfigResult::Success);
    ASSERT_TRUE(LdnConfigService::ToIpcResult(Src::FileNotFound) == ConfigResult::FileNotFound);
    ASSERT_TRUE(LdnConfigService::ToIpcResult(Src::ParseError) == ConfigResult::ParseError);
    ASSERT_TRUE(LdnConfigService::ToIpcResult(Src::IoError) == ConfigResult::IoError);
    ASSERT_TRUE(LdnConfigService::ToIpcResult(static_cast<Src>(99)) == ConfigResult::IoError);
}

int main() {
    return test_report("LdnConfigService Tests");
}
