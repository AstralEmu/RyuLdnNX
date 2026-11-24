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
using ams::mitm::ldn::LdnMitMService;
using ams::mitm::ldn::SharedState;

// bsd/bsd_mitm_service.cpp is not hostable yet (it needs the Atmosphere MITM
// dispatch); the communication service only calls this one function of it,
// from its teardown. Once the BSD service has its own host suite, link the
// real file and delete this definition.
void ams::mitm::bsd::BsdMitmService::CleanupAbandonedServices() {}

namespace {

/// Program id present in the loaded whitelist
constexpr std::uint64_t WHITELISTED_GAME = 0x0100152000022000ULL;
/// Application program id absent from the whitelist
constexpr std::uint64_t OTHER_GAME = 0x0100000000abc000ULL;
/// Program id of this sysmodule (TitleID 4200000000000010)
constexpr std::uint64_t OUR_SYSMODULE = 0x4200000000000010ULL;

/**
 * @brief Builds the process info of a client
 *
 * @param pid process id
 * @param program_id program id
 * @return info with both ids set
 */
ams::sm::MitmProcessInfo client(std::uint64_t pid, std::uint64_t program_id) {
    ams::sm::MitmProcessInfo info{};
    info.process_id.value = pid;
    info.program_id.value = program_id;
    return info;
}

/**
 * @brief Builds an LdnMitMService for a client
 *
 * @param pid process id of the client
 * @param program_id program id of the client
 * @return the service, over a fresh forwarding Service
 */
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
/// Loads the whitelist once before any test runs
WhitelistFixture g_whitelist;

}  // namespace

/**
 * @brief Verifies that ShouldMitm accepts a client whose program id is in the
 *        whitelist.
 */
TEST(a_whitelisted_game_is_intercepted) {
    ASSERT_TRUE(LdnMitMService::ShouldMitm(client(10, WHITELISTED_GAME)));
}

/**
 * @brief Verifies that ShouldMitm rejects an application that is not in the
 *        whitelist, so non-listed games keep the real LDN.
 */
TEST(a_game_outside_the_whitelist_is_not_intercepted) {
    ASSERT_FALSE(LdnMitMService::ShouldMitm(client(10, OTHER_GAME)));
}

/**
 * @brief Verifies that ShouldMitm never intercepts this sysmodule itself, which
 *        would otherwise loop on its own LDN calls.
 */
TEST(our_own_sysmodule_is_never_intercepted) {
    ASSERT_FALSE(LdnMitMService::ShouldMitm(client(10, OUR_SYSMODULE)));
}

/**
 * @brief Verifies that ShouldMitm rejects a program id just below the first
 *        application id (0x0100000000000000), i.e. a system program.
 */
TEST(a_system_program_is_never_intercepted) {
    ASSERT_FALSE(
        LdnMitMService::ShouldMitm(client(10, 0x0100000000000000ULL - 1)));
}

/**
 * @brief Verifies that the first application id (0x0100000000000000) is not a
 *        system program: the whitelist decides, and it is not listed here.
 */
TEST(the_first_application_id_is_judged_by_the_whitelist) {
    ASSERT_FALSE(LdnMitMService::ShouldMitm(client(10, 0x0100000000000000ULL)));
}

/**
 * @brief Verifies that creating the service stores the client pid in
 *        SharedState (SetLdnPid), where the BSD MITM reads it.
 */
TEST(creating_the_service_registers_the_client_pid_for_the_bsd_mitm) {
    SharedState::GetInstance().SetLdnPid(0);
    auto service = make_service(77, WHITELISTED_GAME);
    ASSERT_EQ(SharedState::GetInstance().GetLdnPid(), 77U);
}

/**
 * @brief Verifies that destroying the service does not clear the pid in
 *        SharedState, because the communication service created from it still
 *        needs it.
 */
TEST(destroying_the_service_keeps_the_pid_for_the_communication_service) {
    SharedState::GetInstance().SetLdnPid(0);
    {
        auto service = make_service(78, WHITELISTED_GAME);
    }
    ASSERT_EQ(SharedState::GetInstance().GetLdnPid(), 78U);
}

/**
 * @brief Verifies that CreateUserLocalCommunicationService returns a
 *        communication service and that it counts as one active IPC session
 *        until it is released.
 */
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

/**
 * @brief Verifies that CreateClientProcessMonitor returns a valid
 *        IClientProcessMonitor object.
 */
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

/**
 * @brief Verifies NetworkInfo2NetworkConfig: intent id, channel, max node count
 *        and local communication version are copied, and the unknown fields are
 *        zeroed even over a dirty (0xAA) destination.
 */
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

    ams::mitm::ldn::NetworkConfig config{};
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
    for (const auto byte : config._unk2) {
        ASSERT_EQ(byte, 0);
    }
}

/**
 * @brief Verifies NetworkInfo2SecurityParameter: the random bytes and the
 *        session id are copied from the NetworkInfo.
 */
TEST(the_security_parameter_is_extracted_from_the_network_info) {
    ams::mitm::ldn::NetworkInfo info{};
    std::memset(&info, 0, sizeof(info));
    for (std::size_t i = 0; i < sizeof(info.ldn.unkRandom); ++i) {
        info.ldn.unkRandom[i] = static_cast<std::uint8_t>(0x10 + i);
        info.networkId.sessionId.raw[i] = static_cast<std::uint8_t>(0x80 + i);
    }

    ams::mitm::ldn::SecurityParameter param{};
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

/**
 * @brief Runs every registered test of this suite
 *
 * @return 1 if at least one test failed, 0 otherwise
 */
int main() {
    return test_report("LdnMitMService Tests");
}
