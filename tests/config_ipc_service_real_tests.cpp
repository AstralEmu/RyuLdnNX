/**
 * @file config_ipc_service_real_tests.cpp
 * @brief Unit tests for the real ryu:cfg service
 * (config/config_ipc_service.cpp)
 *
 * Built against the host Stratosphere stand-in in tests/stubs. Save, reload and
 * InitializeConfig work on config::CONFIG_PATH, a fixed absolute path: the
 * tests create and remove that path themselves, so they must run where /config
 * can be created (the test container runs as root).
 */

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "test_harness.hpp"
#include "config/config_ipc_service.hpp"
#include "ldn/ldn_shared_state.hpp"

using namespace ryu_ldn::ipc;
using ams::sf::Out;
namespace fsys = std::filesystem;
namespace config = ryu_ldn::config;

namespace {

/**
 * @brief Call a ryu:cfg getter and return its output value.
 *
 * A failing call is recorded as a test failure but the (default-constructed)
 * value is still returned.
 *
 * @tparam T Type of the output value.
 * @tparam Fn Callable taking an Out<T>, i.e. a ConfigService getter.
 * @param fn Getter to invoke.
 * @return The value the getter wrote.
 */
template <class T, class Fn>
T get(Fn fn) {
    T value{};
    // ASSERT_* returns from the enclosing function: run it in a void lambda so
    // the value is still handed back after a failure has been recorded.
    [&] { ASSERT_TRUE(fn(Out<T>(value)).IsSuccess()); }();
    return value;
}

/**
 * @brief Shortcut of get<u32>() for the getters that output a u32.
 * @param fn ConfigService getter with an Out<u32> output.
 * @return The u32 the getter wrote.
 */
u32 get_u32(ams::Result (*fn)(Out<u32>)) {
    return get<u32>(fn);
}

/**
 * @brief Build a 64-byte IPC passphrase buffer.
 * @param text NUL-terminated initial content.
 * @param fill_to When greater than strlen(text), pad with 'a' up to this many
 *        bytes (capped at the buffer size); no terminator is added by the
 *        padding, so 64 yields an unterminated buffer.
 * @return The zero-initialised buffer holding text and the padding.
 */
RyuCfgPassphraseString make_passphrase(
    const char* text, std::size_t fill_to = 0) {
    RyuCfgPassphraseString p{};
    p.buf.fill(0);
    std::size_t n = std::strlen(text);
    std::memcpy(p.buf.data(), text, n);
    // fill_to > n pads with 'a' up to fill_to bytes, without terminator
    for (std::size_t i = n; i < fill_to && i < p.buf.size(); ++i) {
        p.buf[i] = 'a';
    }
    return p;
}

/**
 * @brief Build a server address IPC structure.
 * @param host Host name, truncated to leave room for the terminator.
 * @param port Server port.
 * @return The zero-initialised structure holding host and port.
 */
ServerAddressIpc make_address(const char* host, u16 port) {
    ServerAddressIpc a{};
    std::memset(&a, 0, sizeof(a));
    std::strncpy(a.host, host, sizeof(a.host) - 1);
    a.port = port;
    return a;
}

/**
 * @brief Reset the process-wide SharedState and drop any pending reconnect.
 * @return The SharedState singleton, in its initial state.
 */
ams::mitm::ldn::SharedState& fresh_state() {
    auto& s = ams::mitm::ldn::SharedState::GetInstance();
    s.Reset();
    s.ConsumeReconnectRequest();
    return s;
}

/// Absolute path of the config file the service reads and writes.
const fsys::path config_file{config::CONFIG_PATH};

/// Remove the grandparent directory of the config file (the config tree).
void remove_config_tree() {
    std::error_code ec;
    fsys::remove_all(config_file.parent_path().parent_path(), ec);
}

}  // namespace

/**
 * @brief The status getters report version "1.0.0", connection status 0 and an
 *        active service, as the overlay expects.
 */
TEST(status_getters_report_version_and_liveness) {
    auto version = get<RyuCfgVersionString>(&ConfigService::GetVersion);
    ASSERT_TRUE(std::strcmp(version.buf.data(), "1.0.0") == 0);
    ASSERT_EQ(
        get_u32(&ConfigService::GetConnectionStatus), static_cast<u32>(0));
    ASSERT_EQ(get_u32(&ConfigService::IsServiceActive), static_cast<u32>(1));
}

/**
 * @brief The LDN enable, use-passphrase and disable-P2P flags read back what
 *        was set; a non-zero LDN-enabled value is normalised to 1.
 */
TEST(the_ldn_flags_round_trip) {
    ASSERT_TRUE(ConfigService::SetLdnEnabled(0).IsSuccess());
    ASSERT_EQ(get_u32(&ConfigService::GetLdnEnabled), static_cast<u32>(0));
    ASSERT_TRUE(ConfigService::SetLdnEnabled(7).IsSuccess());
    ASSERT_EQ(get_u32(&ConfigService::GetLdnEnabled), static_cast<u32>(1));

    ASSERT_TRUE(ConfigService::SetUsePassphrase(1).IsSuccess());
    ASSERT_EQ(get_u32(&ConfigService::GetUsePassphrase), static_cast<u32>(1));
    ASSERT_TRUE(ConfigService::SetUsePassphrase(0).IsSuccess());
    ASSERT_EQ(get_u32(&ConfigService::GetUsePassphrase), static_cast<u32>(0));

    ASSERT_TRUE(ConfigService::SetDisableP2p(1).IsSuccess());
    ASSERT_EQ(get_u32(&ConfigService::GetDisableP2p), static_cast<u32>(1));
    ASSERT_TRUE(ConfigService::SetDisableP2p(0).IsSuccess());
    ASSERT_EQ(get_u32(&ConfigService::GetDisableP2p), static_cast<u32>(0));
}

/**
 * @brief A printable-ASCII passphrase is accepted and GetPassphrase returns it.
 */
TEST(a_valid_passphrase_is_stored_and_read_back) {
    ASSERT_TRUE(
        ConfigService::SetPassphrase(make_passphrase("room 42 !~"))
            .IsSuccess());
    auto got = get<RyuCfgPassphraseString>(&ConfigService::GetPassphrase);
    ASSERT_TRUE(std::strcmp(got.buf.data(), "room 42 !~") == 0);
}

/**
 * @brief A 63-character passphrase is accepted, a 64-byte buffer without
 *        terminator is rejected and leaves the stored value unchanged.
 */
TEST(the_longest_passphrase_is_63_chars_and_unterminated_is_rejected) {
    // Regression: the scan used to be bounded by MAX_PASSPHRASE_LENGTH (64),
    // one more than the 64-byte IPC buffer holds with its terminator, so a
    // buffer full of characters was read one byte past its end.
    auto longest = make_passphrase("", 63);
    ASSERT_TRUE(ConfigService::SetPassphrase(longest).IsSuccess());
    auto got = get<RyuCfgPassphraseString>(&ConfigService::GetPassphrase);
    ASSERT_EQ(std::strlen(got.buf.data()), static_cast<std::size_t>(63));

    auto unterminated = make_passphrase("", 64);
    ASSERT_TRUE(ConfigService::SetPassphrase(unterminated).IsFailure());
    got = get<RyuCfgPassphraseString>(&ConfigService::GetPassphrase);
    ASSERT_EQ(std::strlen(got.buf.data()), static_cast<std::size_t>(63));
}

/**
 * @brief Over-long, control-character, DEL and non-ASCII passphrases fail with
 *        ResultInvalidIpcInput and never overwrite the stored passphrase.
 */
TEST(invalid_passphrases_are_rejected_and_leave_the_stored_one_untouched) {
    ASSERT_TRUE(
        ConfigService::SetPassphrase(make_passphrase("keep")).IsSuccess());

    // Longer than the maximum: the 64-byte buffer is full, no terminator.
    RyuCfgPassphraseString too_long{};
    too_long.buf.fill('a');
    ams::Result r = ConfigService::SetPassphrase(too_long);
    ASSERT_TRUE(r.IsFailure());
    ASSERT_EQ(r.GetValue(), result::ResultInvalidIpcInput().GetValue());

    ASSERT_TRUE(
        ConfigService::SetPassphrase(make_passphrase(
                                         "bad\x01"
                                         "char"))
            .IsFailure());
    ASSERT_TRUE(
        ConfigService::SetPassphrase(make_passphrase(
                                         "bad\x7f"
                                         "char"))
            .IsFailure());
    ASSERT_TRUE(
        ConfigService::SetPassphrase(make_passphrase("bad\xc3\xa9"))
            .IsFailure());

    auto got = get<RyuCfgPassphraseString>(&ConfigService::GetPassphrase);
    ASSERT_TRUE(std::strcmp(got.buf.data(), "keep") == 0);
}

/**
 * @brief SetServerAddress followed by GetServerAddress returns the same host
 *        and port.
 */
TEST(the_server_address_round_trips_through_the_service) {
    ASSERT_TRUE(
        ConfigService::SetServerAddress(make_address("ldn.example.org", 31337))
            .IsSuccess());
    auto got = get<ServerAddressIpc>(&ConfigService::GetServerAddress);
    ASSERT_TRUE(std::strcmp(got.host, "ldn.example.org") == 0);
    ASSERT_EQ(got.port, 31337);
}

/**
 * @brief Hosts containing ';', '|', '&' or a backtick are refused with
 *        ResultInvalidIpcInput (shell injection guard) and the stored address
 *        is untouched.
 */
TEST(shell_metacharacters_in_a_host_are_rejected_and_keep_the_address) {
    ASSERT_TRUE(
        ConfigService::SetServerAddress(make_address("keep.example.org", 1))
            .IsSuccess());
    const char* bad[] = {"a;b", "a|b", "a&b", "a`b"};
    for (const char* host : bad) {
        ams::Result r = ConfigService::SetServerAddress(make_address(host, 2));
        ASSERT_TRUE(r.IsFailure());
        ASSERT_EQ(r.GetValue(), result::ResultInvalidIpcInput().GetValue());
    }
    auto got = get<ServerAddressIpc>(&ConfigService::GetServerAddress);
    ASSERT_TRUE(std::strcmp(got.host, "keep.example.org") == 0);
    ASSERT_EQ(got.port, 1);
}

/**
 * @brief A 64-byte host without terminator is rejected, while 63 characters
 *        plus the terminator is the longest accepted host.
 */
TEST(a_host_filling_the_whole_field_without_terminator_is_rejected) {
    // Regression: the scan used to be bounded by MAX_HOST_LENGTH (128) while
    // the IPC field is 64 bytes, so it ran on into the port/padding bytes.
    ASSERT_TRUE(
        ConfigService::SetServerAddress(make_address("keep.example.org", 1))
            .IsSuccess());
    ServerAddressIpc a{};
    std::memset(a.host, 'h', sizeof(a.host));
    a.port = 3;
    ASSERT_TRUE(ConfigService::SetServerAddress(a).IsFailure());
    auto got = get<ServerAddressIpc>(&ConfigService::GetServerAddress);
    ASSERT_TRUE(std::strcmp(got.host, "keep.example.org") == 0);

    // 63 characters plus the terminator is the longest accepted host.
    ServerAddressIpc longest{};
    std::memset(longest.host, 'h', sizeof(longest.host) - 1);
    ASSERT_TRUE(ConfigService::SetServerAddress(longest).IsSuccess());
}

/**
 * @brief Debug enable and level read back what was set; level 4 is accepted,
 *        5 is rejected with ResultInvalidIpcInput and the level is kept.
 */
TEST(the_debug_settings_round_trip_and_reject_levels_above_four) {
    ASSERT_TRUE(ConfigService::SetDebugEnabled(1).IsSuccess());
    ASSERT_EQ(get_u32(&ConfigService::GetDebugEnabled), static_cast<u32>(1));
    ASSERT_TRUE(ConfigService::SetDebugEnabled(0).IsSuccess());
    ASSERT_EQ(get_u32(&ConfigService::GetDebugEnabled), static_cast<u32>(0));

    ASSERT_TRUE(ConfigService::SetDebugLevel(4).IsSuccess());
    ASSERT_EQ(get_u32(&ConfigService::GetDebugLevel), static_cast<u32>(4));
    ams::Result r = ConfigService::SetDebugLevel(5);
    ASSERT_TRUE(r.IsFailure());
    ASSERT_EQ(r.GetValue(), result::ResultInvalidIpcInput().GetValue());
    ASSERT_EQ(get_u32(&ConfigService::GetDebugLevel), static_cast<u32>(4));
}

/**
 * @brief Game-active flag, LDN state, last RTT, active process id and session
 *        info reported by the service mirror what is set on SharedState.
 */
TEST(the_runtime_state_comes_from_the_shared_state) {
    auto& s = fresh_state();
    s.SetGameActive(true, 0x1234);
    s.SetLdnState(ams::mitm::ldn::CommState::Station);
    s.SetLastRtt(55);
    s.SetSessionInfo(3, 8, 1, true);

    ASSERT_EQ(get_u32(&ConfigService::IsGameActive), static_cast<u32>(1));
    ASSERT_EQ(
        get_u32(&ConfigService::GetLdnState),
        static_cast<u32>(ams::mitm::ldn::CommState::Station));
    ASSERT_EQ(get_u32(&ConfigService::GetLastRtt), static_cast<u32>(55));
    ASSERT_EQ(
        get<u64>(&ConfigService::GetActiveProcessId), static_cast<u64>(0x1234));

    auto info = get<SessionInfoIpc>(&ConfigService::GetSessionInfo);
    ASSERT_EQ(info.node_count, 3);
    ASSERT_EQ(info.max_nodes, 8);
    ASSERT_EQ(info.local_node_id, 1);
    ASSERT_EQ(info.is_host, 1);

    s.SetGameActive(false, 0);
    ASSERT_EQ(get_u32(&ConfigService::IsGameActive), static_cast<u32>(0));
    s.SetSessionInfo(1, 2, 0, false);
    ASSERT_EQ(get<SessionInfoIpc>(&ConfigService::GetSessionInfo).is_host, 0);
}

/**
 * @brief ForceReconnect succeeds and leaves a reconnect request on
 *        SharedState for the network thread to consume.
 */
TEST(force_reconnect_requests_a_reconnection) {
    auto& s = fresh_state();
    ASSERT_TRUE(ConfigService::ForceReconnect().IsSuccess());
    ASSERT_TRUE(s.ConsumeReconnectRequest());
}

/**
 * @brief ReloadConfig reports FileNotFound without a file, SaveConfig creates
 *        it, ReloadConfig restores the saved values and SaveConfig reports
 *        IoError when a directory occupies the file path.
 */
TEST(save_and_reload_follow_the_state_of_the_config_file) {
    remove_config_tree();
    std::error_code ec;
    fsys::create_directories(config_file.parent_path().parent_path(), ec);
    ASSERT_FALSE(ec);

    // No file yet.
    ASSERT_TRUE(
        get<ConfigResult>(&ConfigService::ReloadConfig) ==
        ConfigResult::FileNotFound);

    // Save creates it and a reload brings the saved values back.
    ASSERT_TRUE(ConfigService::SetDebugLevel(1).IsSuccess());
    ASSERT_TRUE(
        get<ConfigResult>(&ConfigService::SaveConfig) == ConfigResult::Success);
    ASSERT_TRUE(fsys::exists(config_file));
    ASSERT_TRUE(ConfigService::SetDebugLevel(3).IsSuccess());
    ASSERT_TRUE(
        get<ConfigResult>(&ConfigService::ReloadConfig) ==
        ConfigResult::Success);
    ASSERT_EQ(get_u32(&ConfigService::GetDebugLevel), static_cast<u32>(1));

    // A directory in place of the file: it cannot be written.
    fsys::remove(config_file, ec);
    fsys::create_directory(config_file, ec);
    ASSERT_FALSE(ec);
    ASSERT_TRUE(
        get<ConfigResult>(&ConfigService::SaveConfig) == ConfigResult::IoError);

    remove_config_tree();
}

/**
 * @brief InitializeConfig loads the values of an existing config file and
 *        falls back to the built-in defaults when the file is missing.
 */
TEST(initialize_config_loads_the_defaults_then_the_file) {
    remove_config_tree();
    std::error_code ec;
    fsys::create_directories(config_file.parent_path(), ec);
    ASSERT_FALSE(ec);
    {
        std::ofstream out(config_file);
        out << "[server]\nhost = from-file.example.org\nport = 4242\n";
    }

    InitializeConfig();
    auto got = get<ServerAddressIpc>(&ConfigService::GetServerAddress);
    ASSERT_TRUE(std::strcmp(got.host, "from-file.example.org") == 0);
    ASSERT_EQ(got.port, 4242);

    // Without the file the defaults come back.
    remove_config_tree();
    InitializeConfig();
    ASSERT_TRUE(
        std::strcmp(
            get<ServerAddressIpc>(&ConfigService::GetServerAddress).host,
            config::get_default_config().server.host) == 0);
}

/**
 * @brief Test entry point.
 * @return 0 when every test passed, 1 otherwise (see test_report()).
 */
int main() {
    return test_report("ryu:cfg ConfigService Tests");
}
