/**
 * @file ldn_mitm_service_tests.cpp
 * @brief Unit tests for the real LdnMitMService (ldn/ldn_mitm_service.cpp)
 *
 * Linked with the real ICommunicationService, SharedState and game whitelist.
 * Built against the host Stratosphere stand-in in tests/stubs.
 */

#include <cstdio>
#include <cstring>
#include <memory>

#include "test_harness.hpp"
#include "ldn/ldn_mitm_service.hpp"
#include "ldn/ldn_shared_state.hpp"
#include "config/game_whitelist.hpp"
#include "bsd/bsd_mitm_service.hpp"

using ams::mitm::ldn::IClientProcessMonitor;
using ams::mitm::ldn::IClientProcessMonitorInterface;
using ams::mitm::ldn::ICommunicationInterface;
using ams::mitm::ldn::ICommunicationService;
using ams::mitm::ldn::LdnMitMService;
using ams::mitm::ldn::SharedState;

/// bsd/bsd_mitm_service.cpp is not hostable yet (it needs the Atmosphere MITM
/// dispatch); the communication service only calls this one function of it,
/// from its teardown. Once the BSD service has its own host suite, link the
/// real file and delete this definition.
void ams::mitm::bsd::BsdMitmService::CleanupAbandonedServices() {}

namespace {

constexpr std::uint64_t WHITELISTED_GAME = 0x0100152000022000ULL;
constexpr std::uint64_t OTHER_GAME = 0x0100000000abc000ULL;
constexpr std::uint64_t OUR_SYSMODULE = 0x4200000000000010ULL;

ams::sm::MitmProcessInfo client(std::uint64_t pid, std::uint64_t program_id) {
    ams::sm::MitmProcessInfo info{};
    info.process_id.value = pid;
    info.program_id.value = program_id;
    return info;
}

std::unique_ptr<LdnMitMService> make_service(
    std::uint64_t pid, std::uint64_t program_id) {
    return std::make_unique<LdnMitMService>(
        std::make_shared<::Service>(), client(pid, program_id));
}

/// The whitelist is a load-once latch: load it once, with one known game.
struct WhitelistFixture {
    WhitelistFixture() {
        ams::fs::g_host_fs.content = "0x0100152000022000\n";
        ryu_ldn::config::LoadWhitelist();
    }
};
WhitelistFixture g_whitelist;

}  // namespace

TEST(a_whitelisted_game_is_intercepted) {
    ASSERT_TRUE(LdnMitMService::ShouldMitm(client(10, WHITELISTED_GAME)));
}

TEST(a_game_outside_the_whitelist_is_not_intercepted) {
    ASSERT_FALSE(LdnMitMService::ShouldMitm(client(10, OTHER_GAME)));
}

TEST(our_own_sysmodule_is_never_intercepted) {
    ASSERT_FALSE(LdnMitMService::ShouldMitm(client(10, OUR_SYSMODULE)));
}

TEST(a_system_program_is_never_intercepted) {
    ASSERT_FALSE(
        LdnMitMService::ShouldMitm(client(10, 0x0100000000000000ULL - 1)));
}

TEST(the_first_application_id_is_judged_by_the_whitelist) {
    ASSERT_FALSE(LdnMitMService::ShouldMitm(client(10, 0x0100000000000000ULL)));
}

TEST(creating_the_service_registers_the_client_pid_for_the_bsd_mitm) {
    SharedState::GetInstance().SetLdnPid(0);
    auto service = make_service(77, WHITELISTED_GAME);
    ASSERT_EQ(SharedState::GetInstance().GetLdnPid(), 77u);
}

TEST(destroying_the_service_keeps_the_pid_for_the_communication_service) {
    SharedState::GetInstance().SetLdnPid(0);
    {
        auto service = make_service(78, WHITELISTED_GAME);
    }
    ASSERT_EQ(SharedState::GetInstance().GetLdnPid(), 78u);
}

TEST(the_communication_service_is_created_for_the_client_program) {
    auto service = make_service(79, WHITELISTED_GAME);
    const std::uint32_t before =
        SharedState::GetInstance().GetActiveSessionCount();
    ams::sf::SharedPointer<ICommunicationInterface> created;
    ASSERT_TRUE(
        service
            ->CreateUserLocalCommunicationService(
                ams::sf::Out<ams::sf::SharedPointer<ICommunicationInterface>>(
                    created))
            .IsSuccess());
    ASSERT_TRUE(static_cast<bool>(created));
    // The service counts as one IPC session until it is destroyed.
    ASSERT_EQ(SharedState::GetInstance().GetActiveSessionCount(), before + 1);
    created = {};
    ASSERT_EQ(SharedState::GetInstance().GetActiveSessionCount(), before);
}

TEST(the_client_process_monitor_is_created) {
    ams::sf::SharedPointer<IClientProcessMonitorInterface> created;
    ASSERT_TRUE(
        LdnMitMService::CreateClientProcessMonitor(
            ams::sf::Out<
                ams::sf::SharedPointer<IClientProcessMonitorInterface>>(
                created))
            .IsSuccess());
    ASSERT_TRUE(static_cast<bool>(created));
    ASSERT_TRUE(created.GetImpl<IClientProcessMonitor>() != nullptr);
}

TEST(the_network_config_is_extracted_from_the_network_info) {
    ams::mitm::ldn::NetworkInfo info{};
    std::memset(&info, 0, sizeof(info));
    for (std::size_t i = 0; i < sizeof(info.networkId.intentId); ++i) {
        reinterpret_cast<std::uint8_t*>(&info.networkId.intentId)[i] =
            static_cast<std::uint8_t>(i + 1);
    }
    info.common.channel = 36;
    info.ldn.nodeCountMax = 8;
    info.ldn.nodes[0].localCommunicationVersion = 3;

    ams::mitm::ldn::NetworkConfig config;
    std::memset(&config, 0xAA, sizeof(config));
    ams::mitm::ldn::NetworkInfo2NetworkConfig(&info, &config);

    ASSERT_EQ(
        std::memcmp(
            &config.intentId,
            &info.networkId.intentId,
            sizeof(config.intentId)),
        0);
    ASSERT_EQ(config.channel, 36);
    ASSERT_EQ(config.nodeCountMax, 8);
    ASSERT_EQ(config._unk1, 0);
    ASSERT_EQ(config.localCommunicationVersion, 3);
    for (std::size_t i = 0; i < sizeof(config._unk2); ++i) {
        ASSERT_EQ(config._unk2[i], 0);
    }
}

TEST(the_security_parameter_is_extracted_from_the_network_info) {
    ams::mitm::ldn::NetworkInfo info{};
    std::memset(&info, 0, sizeof(info));
    for (std::size_t i = 0; i < sizeof(info.ldn.unkRandom); ++i) {
        info.ldn.unkRandom[i] = static_cast<std::uint8_t>(0x10 + i);
        info.networkId.sessionId.raw[i] = static_cast<std::uint8_t>(0x80 + i);
    }

    ams::mitm::ldn::SecurityParameter param;
    std::memset(&param, 0, sizeof(param));
    ams::mitm::ldn::NetworkInfo2SecurityParameter(&info, &param);

    ASSERT_EQ(
        std::memcmp(
            param.unkRandom, info.ldn.unkRandom, sizeof(param.unkRandom)),
        0);
    ASSERT_EQ(
        std::memcmp(
            param.sessionId.raw,
            info.networkId.sessionId.raw,
            sizeof(param.sessionId.raw)),
        0);
}

int main() {
    return test_report("LdnMitMService Tests");
}
