/**
 * @file ldn_types_tests.cpp
 * @brief Unit tests for LDN types and type conversions
 *
 * Tests the LDN data structures and conversions between
 * ams::mitm::ldn types and ryu_ldn::protocol types.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#include <cstdio>
#include <cstring>
#include <cstdint>

// Include protocol types (host-compatible)
#include "protocol/types.hpp"

// ============================================================================
// Test Framework (minimal)
// ============================================================================

/// Number of tests that ran to completion (incremented by TEST()).
static int g_tests_passed = 0;
/// Number of failed assertions (each one aborts its test and is counted).
static int g_tests_failed = 0;

/**
 * @brief Define and auto-run a test case.
 *
 * Declares test_<name>() and a static object whose constructor runs it
 * during static initialisation, printing PASS and counting it.
 *
 * @param name Identifier of the test (becomes test_<name>).
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
 * @brief Fail the current test and return from it when cond is false.
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
 * @brief Fail the current test and return from it when cond is true.
 * @param cond Expression expected to be false.
 */
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

/**
 * @brief Fail the current test and return from it when a != b.
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

/**
 * @brief Fail the current test and return from it when a == b.
 * @param a Actual value.
 * @param b Value a must differ from.
 */
#define ASSERT_NE(a, b)                                         \
    do {                                                        \
        if ((a) == (b)) {                                       \
            printf(                                             \
                "FAIL\n    Expected: %s != %s\n    at %s:%d\n", \
                #a,                                             \
                #b,                                             \
                __FILE__,                                       \
                __LINE__);                                      \
            g_tests_failed++;                                   \
            return;                                             \
        }                                                       \
    } while (0)

/**
 * @brief Fail the current test and return from it when the C strings differ.
 * @param a Actual NUL-terminated string.
 * @param b Expected NUL-terminated string.
 */
#define ASSERT_STREQ(a, b)                                              \
    do {                                                                \
        if (strcmp((a), (b)) != 0) {                                    \
            printf(                                                     \
                "FAIL\n    Expected: \"%s\" == \"%s\"\n    at %s:%d\n", \
                (a),                                                    \
                (b),                                                    \
                __FILE__,                                               \
                __LINE__);                                              \
            g_tests_failed++;                                           \
            return;                                                     \
        }                                                               \
    } while (0)

// ============================================================================
// Protocol Type Structure Size Tests
// ============================================================================

/**
 * @brief MacAddress is 6 bytes.
 */
TEST(protocol_mac_address_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::MacAddress), 6U);
}

/**
 * @brief Ssid is 34 bytes.
 */
TEST(protocol_ssid_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::Ssid), 34U);
}

/**
 * @brief SessionId is 16 bytes.
 */
TEST(protocol_session_id_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::SessionId), 16U);
}

/**
 * @brief IntentId is 16 bytes.
 */
TEST(protocol_intent_id_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::IntentId), 16U);
}

/**
 * @brief NetworkId is 32 bytes.
 */
TEST(protocol_network_id_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::NetworkId), 32U);
}

/**
 * @brief NodeInfo is 64 bytes.
 */
TEST(protocol_node_info_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::NodeInfo), 64U);
}

/**
 * @brief CommonNetworkInfo is 48 bytes.
 */
TEST(protocol_common_network_info_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::CommonNetworkInfo), 48U);
}

/**
 * @brief LdnNetworkInfo is 0x430 bytes.
 */
TEST(protocol_ldn_network_info_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::LdnNetworkInfo), 0x430U);
}

/**
 * @brief NetworkInfo is 0x480 bytes.
 */
TEST(protocol_network_info_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::NetworkInfo), 0x480U);
}

/**
 * @brief SecurityConfig is 68 bytes.
 */
TEST(protocol_security_config_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::SecurityConfig), 68U);
}

/**
 * @brief UserConfig is 48 bytes.
 */
TEST(protocol_user_config_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::UserConfig), 48U);
}

/**
 * @brief NetworkConfig is 32 bytes.
 */
TEST(protocol_network_config_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::NetworkConfig), 32U);
}

/**
 * @brief ScanFilterFull is 0x60 bytes (8-byte alignment of the C# layout).
 */
TEST(protocol_scan_filter_full_size) {
    ASSERT_EQ(
        sizeof(ryu_ldn::protocol::ScanFilterFull),
        0x60U);  // 96 bytes with Pack=8 alignment
}

/**
 * @brief ConnectRequest is 0x500 bytes.
 */
TEST(protocol_connect_request_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::ConnectRequest), 0x500U);  // 1280 bytes
}

/**
 * @brief CreateAccessPointRequest is 0xBC bytes (security, user, network and
 * Ryu network configs).
 */
TEST(protocol_create_access_point_request_size) {
    // SecurityConfig(0x44) + UserConfig(0x30) + NetworkConfig(0x20) +
    // RyuNetworkConfig(0x28) = 0xBC
    ASSERT_EQ(sizeof(ryu_ldn::protocol::CreateAccessPointRequest), 0xBCU);
}

// ============================================================================
// Protocol Type Initialization Tests
// ============================================================================

/**
 * @brief A value-initialised MacAddress is zero.
 */
TEST(mac_address_zero_initialized) {
    ryu_ldn::protocol::MacAddress mac{};
    ASSERT_TRUE(mac.is_zero());
}

/**
 * @brief A MacAddress with a byte set is not zero.
 */
TEST(mac_address_not_zero_after_set) {
    ryu_ldn::protocol::MacAddress mac{};
    mac.data[0] = 0x12;
    mac.data[1] = 0x34;
    mac.data[2] = 0x56;
    mac.data[3] = 0x78;
    mac.data[4] = 0x9A;
    mac.data[5] = 0xBC;
    ASSERT_FALSE(mac.is_zero());
}

/**
 * @brief A value-initialised SessionId is zero.
 */
TEST(session_id_zero_initialized) {
    ryu_ldn::protocol::SessionId sid{};
    ASSERT_TRUE(sid.is_zero());
}

/**
 * @brief A SessionId with a byte set is not zero.
 */
TEST(session_id_not_zero_after_set) {
    ryu_ldn::protocol::SessionId sid{};
    sid.data[0] = 0x01;
    ASSERT_FALSE(sid.is_zero());
}

/**
 * @brief A value-initialised Ssid has length 0.
 */
TEST(ssid_default_empty) {
    ryu_ldn::protocol::Ssid ssid{};
    ASSERT_EQ(ssid.length, 0U);
}

/**
 * @brief An Ssid keeps the length and name written to it.
 */
TEST(ssid_set_name) {
    ryu_ldn::protocol::Ssid ssid{};
    const char* name = "TestNetwork";
    ssid.length = static_cast<uint8_t>(strlen(name));
    memcpy(ssid.name, name, ssid.length);

    ASSERT_EQ(ssid.length, 11U);
    ASSERT_EQ(memcmp(ssid.name, "TestNetwork", 11), 0);
}

// ============================================================================
// Intent ID Tests
// ============================================================================

/**
 * @brief IntentId stores the local communication ID and scene ID.
 */
TEST(intent_id_fields) {
    ryu_ldn::protocol::IntentId id{};
    id.local_communication_id = 0x0100000000001234ULL;
    id.scene_id = 42;

    ASSERT_EQ(id.local_communication_id, 0x0100000000001234ULL);
    ASSERT_EQ(id.scene_id, 42U);
}

// ============================================================================
// Network ID Tests
// ============================================================================

/**
 * @brief NetworkId nests an IntentId and a SessionId that keep their values.
 */
TEST(network_id_structure) {
    ryu_ldn::protocol::NetworkId nid{};
    nid.intent_id.local_communication_id = 0x0100000000001234ULL;
    nid.intent_id.scene_id = 1;
    nid.session_id.data[0] = 0xAB;

    ASSERT_EQ(nid.intent_id.local_communication_id, 0x0100000000001234ULL);
    ASSERT_EQ(nid.intent_id.scene_id, 1U);
    ASSERT_EQ(nid.session_id.data[0], 0xABU);
}

// ============================================================================
// Security Config Tests
// ============================================================================

/**
 * @brief SecurityConfig stores the mode, passphrase size and passphrase.
 */
TEST(security_config_passphrase) {
    ryu_ldn::protocol::SecurityConfig cfg{};
    cfg.security_mode = 1;
    cfg.passphrase_size = 8;
    memcpy(cfg.passphrase, "password", 8);

    ASSERT_EQ(cfg.security_mode, 1U);
    ASSERT_EQ(cfg.passphrase_size, 8U);
    ASSERT_EQ(memcmp(cfg.passphrase, "password", 8), 0);
}

// ============================================================================
// User Config Tests
// ============================================================================

/**
 * @brief UserConfig stores a NUL-terminated user name.
 */
TEST(user_config_username) {
    ryu_ldn::protocol::UserConfig cfg{};
    const char* name = "Player1";
    memcpy(cfg.user_name, name, strlen(name) + 1);

    ASSERT_STREQ(cfg.user_name, "Player1");
}

// ============================================================================
// Network Config Tests
// ============================================================================

/**
 * @brief NetworkConfig stores intent, channel, node maximum and version.
 */
TEST(network_config_fields) {
    ryu_ldn::protocol::NetworkConfig cfg{};
    cfg.intent_id.local_communication_id = 0x0100000000005678ULL;
    cfg.intent_id.scene_id = 2;
    cfg.channel = 6;
    cfg.node_count_max = 8;
    cfg.local_communication_version = 1;

    ASSERT_EQ(cfg.intent_id.local_communication_id, 0x0100000000005678ULL);
    ASSERT_EQ(cfg.channel, 6U);
    ASSERT_EQ(cfg.node_count_max, 8U);
    ASSERT_EQ(cfg.local_communication_version, 1U);
}

// ============================================================================
// Node Info Tests
// ============================================================================

/**
 * @brief NodeInfo stores IP, node ID, connection flag and user name.
 */
TEST(node_info_fields) {
    ryu_ldn::protocol::NodeInfo node{};
    node.ipv4_address = 0x0A720001;  // 10.114.0.1
    node.node_id = 0;
    node.is_connected = 1;
    memcpy(node.user_name, "HostPlayer", 11);

    ASSERT_EQ(node.ipv4_address, 0x0A720001U);
    ASSERT_EQ(node.node_id, 0);
    ASSERT_EQ(node.is_connected, 1);
    ASSERT_STREQ(node.user_name, "HostPlayer");
}

// ============================================================================
// Scan Filter Tests
// ============================================================================

/**
 * @brief ScanFilterFull stores flag, network type, network ID and SSID.
 */
TEST(scan_filter_full_fields) {
    ryu_ldn::protocol::ScanFilterFull filter{};
    filter.flag = 0x01;
    filter.network_type = 2;
    filter.network_id.intent_id.local_communication_id = 0x0100000000001234ULL;
    filter.ssid.length = 4;
    memcpy(filter.ssid.name, "Test", 4);

    ASSERT_EQ(filter.flag, 0x01U);
    ASSERT_EQ(filter.network_type, 2U);
    ASSERT_EQ(
        filter.network_id.intent_id.local_communication_id,
        0x0100000000001234ULL);
    ASSERT_EQ(filter.ssid.length, 4U);
}

// ============================================================================
// Connect Request Tests
// ============================================================================

/**
 * @brief ConnectRequest stores its security, user, option and network info
 *        fields.
 */
TEST(connect_request_structure) {
    ryu_ldn::protocol::ConnectRequest req{};

    // Security config
    req.security_config.security_mode = 1;
    req.security_config.passphrase_size = 4;
    memcpy(req.security_config.passphrase, "pass", 4);

    // User config
    memcpy(req.user_config.user_name, "Client", 7);

    // Options
    req.local_communication_version = 1;
    req.option_unknown = 0;

    // Network info (just set network ID)
    req.network_info.network_id.intent_id.local_communication_id =
        0x0100000000001234ULL;

    ASSERT_EQ(req.security_config.security_mode, 1U);
    ASSERT_EQ(req.local_communication_version, 1U);
    ASSERT_EQ(
        req.network_info.network_id.intent_id.local_communication_id,
        0x0100000000001234ULL);
}

// ============================================================================
// Create Access Point Request Tests
// ============================================================================

/**
 * @brief CreateAccessPointRequest stores its security, user and network
 *        configs.
 */
TEST(create_access_point_request_structure) {
    ryu_ldn::protocol::CreateAccessPointRequest req{};

    // Security config
    req.security_config.security_mode = 2;

    // User config
    memcpy(req.user_config.user_name, "Host", 5);

    // Network config
    req.network_config.intent_id.local_communication_id = 0x0100000000005678ULL;
    req.network_config.intent_id.scene_id = 1;
    req.network_config.channel = 1;
    req.network_config.node_count_max = 4;
    req.network_config.local_communication_version = 1;

    ASSERT_EQ(req.security_config.security_mode, 2U);
    ASSERT_STREQ(req.user_config.user_name, "Host");
    ASSERT_EQ(req.network_config.node_count_max, 4U);
}

// ============================================================================
// LDN Network Info Tests
// ============================================================================

/**
 * @brief LdnNetworkInfo stores a host and a client node and their counts.
 */
TEST(ldn_network_info_nodes) {
    ryu_ldn::protocol::LdnNetworkInfo info{};
    info.node_count_max = 8;
    info.node_count = 2;

    // Set host node
    info.nodes[0].node_id = 0;
    info.nodes[0].is_connected = 1;
    info.nodes[0].ipv4_address = 0x0A720001;
    memcpy(info.nodes[0].user_name, "Host", 5);

    // Set client node
    info.nodes[1].node_id = 1;
    info.nodes[1].is_connected = 1;
    info.nodes[1].ipv4_address = 0x0A720002;
    memcpy(info.nodes[1].user_name, "Client", 7);

    ASSERT_EQ(info.node_count, 2U);
    ASSERT_EQ(info.nodes[0].node_id, 0);
    ASSERT_EQ(info.nodes[1].node_id, 1);
    ASSERT_STREQ(info.nodes[0].user_name, "Host");
    ASSERT_STREQ(info.nodes[1].user_name, "Client");
}

/**
 * @brief LdnNetworkInfo stores advertise data and its size.
 */
TEST(ldn_network_info_advertise_data) {
    ryu_ldn::protocol::LdnNetworkInfo info{};

    const uint8_t adv_data[] = {0x01, 0x02, 0x03, 0x04};
    info.advertise_data_size = sizeof(adv_data);
    memcpy(info.advertise_data, adv_data, sizeof(adv_data));

    ASSERT_EQ(info.advertise_data_size, 4U);
    ASSERT_EQ(info.advertise_data[0], 0x01U);
    ASSERT_EQ(info.advertise_data[3], 0x04U);
}

// ============================================================================
// Common Network Info Tests
// ============================================================================

/**
 * @brief CommonNetworkInfo stores BSSID, SSID, channel, link level and type.
 */
TEST(common_network_info_fields) {
    ryu_ldn::protocol::CommonNetworkInfo info{};

    // MAC Address (BSSID)
    info.mac_address.data[0] = 0x12;
    info.mac_address.data[5] = 0x78;

    // SSID
    info.ssid.length = 8;
    memcpy(info.ssid.name, "GameRoom", 8);

    // Channel and link level
    info.channel = 36;
    info.link_level = 3;  // Signal strength indicator (0-3)
    info.network_type = 2;

    ASSERT_EQ(info.mac_address.data[0], 0x12U);
    ASSERT_EQ(info.channel, 36);
    ASSERT_EQ(info.link_level, 3U);
    ASSERT_EQ(info.network_type, 2U);
}

// ============================================================================
// Full Network Info Tests
// ============================================================================

/**
 * @brief NetworkInfo nests the network ID, common and LDN parts, each
 *        keeping its values.
 */
TEST(network_info_complete_structure) {
    ryu_ldn::protocol::NetworkInfo info{};

    // Network ID
    info.network_id.intent_id.local_communication_id = 0x0100000000001234ULL;
    info.network_id.intent_id.scene_id = 1;

    // Common info
    info.common.channel = 6;
    info.common.network_type = 2;
    info.common.ssid.length = 4;
    memcpy(info.common.ssid.name, "Game", 4);

    // LDN info
    info.ldn.node_count_max = 8;
    info.ldn.node_count = 1;
    info.ldn.security_mode = 1;

    ASSERT_EQ(
        info.network_id.intent_id.local_communication_id,
        0x0100000000001234ULL);
    ASSERT_EQ(info.common.channel, 6);
    ASSERT_EQ(info.ldn.node_count_max, 8U);
}

// ============================================================================
// Proxy Header Tests
// ============================================================================

/**
 * @brief ProxyDataHeader is 0x14 (20) bytes.
 */
TEST(proxy_data_header_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::ProxyDataHeader), 0x14U);  // 20 bytes
}

/**
 * @brief ProxyDataHeader stores the addresses, ports, protocol and length.
 */
TEST(proxy_data_header_fields) {
    ryu_ldn::protocol::ProxyDataHeader header{};
    header.info.source_ipv4 = 0xC0A80101;  // 192.168.1.1
    header.info.source_port = 12345;
    header.info.dest_ipv4 = 0xC0A80102;  // 192.168.1.2
    header.info.dest_port = 54321;
    header.info.protocol = ryu_ldn::protocol::ProtocolType::Udp;
    header.data_length = 100;

    ASSERT_EQ(header.info.source_ipv4, 0xC0A80101U);
    ASSERT_EQ(header.info.dest_ipv4, 0xC0A80102U);
    ASSERT_EQ(header.data_length, 100U);
}

// ============================================================================
// Private Room Types Tests (Story 7.7)
// ============================================================================

/**
 * @brief SecurityParameter is 0x20 (32) bytes.
 */
TEST(protocol_security_parameter_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::SecurityParameter), 0x20U);  // 32 bytes
}

/**
 * @brief SecurityParameter stores its data and session ID bytes.
 */
TEST(protocol_security_parameter_fields) {
    ryu_ldn::protocol::SecurityParameter param{};
    // Set security data
    param.data[0] = 0x12;
    param.data[15] = 0x34;
    // Set session ID
    param.session_id[0] = 0xAB;
    param.session_id[15] = 0xCD;

    ASSERT_EQ(param.data[0], 0x12U);
    ASSERT_EQ(param.data[15], 0x34U);
    ASSERT_EQ(param.session_id[0], 0xABU);
    ASSERT_EQ(param.session_id[15], 0xCDU);
}

/**
 * @brief AddressEntry is 0x0C (12) bytes.
 */
TEST(protocol_address_entry_size) {
    ASSERT_EQ(sizeof(ryu_ldn::protocol::AddressEntry), 0x0CU);  // 12 bytes
}

/**
 * @brief AddressEntry stores an IP and a MAC address.
 */
TEST(protocol_address_entry_fields) {
    ryu_ldn::protocol::AddressEntry entry{};
    entry.ipv4_address = 0x0A720001;  // 10.114.0.1
    entry.mac_address.data[0] = 0x12;
    entry.mac_address.data[5] = 0x78;
    entry.reserved = 0;

    ASSERT_EQ(entry.ipv4_address, 0x0A720001U);
    ASSERT_EQ(entry.mac_address.data[0], 0x12U);
    ASSERT_EQ(entry.mac_address.data[5], 0x78U);
}

/**
 * @brief AddressList is 0x60 (8 entries of 12) bytes.
 */
TEST(protocol_address_list_size) {
    ASSERT_EQ(
        sizeof(ryu_ldn::protocol::AddressList), 0x60U);  // 96 bytes (8 * 12)
}

/**
 * @brief AddressList stores its first and last entries.
 */
TEST(protocol_address_list_fields) {
    ryu_ldn::protocol::AddressList list{};
    // Set first entry
    list.addresses[0].ipv4_address = 0x0A720001;
    list.addresses[0].mac_address.data[0] = 0x11;
    // Set last entry
    list.addresses[7].ipv4_address = 0x0A720008;
    list.addresses[7].mac_address.data[0] = 0x88;

    ASSERT_EQ(list.addresses[0].ipv4_address, 0x0A720001U);
    ASSERT_EQ(list.addresses[7].ipv4_address, 0x0A720008U);
}

/**
 * @brief CreateAccessPointPrivateRequest is 0x13C (316) bytes.
 */
TEST(protocol_create_access_point_private_request_size) {
    // 0x13C = 316 bytes
    // SecurityConfig(0x44) + SecurityParameter(0x20) + UserConfig(0x30) +
    // NetworkConfig(0x20) + AddressList(0x60) + RyuNetworkConfig(0x28)
    ASSERT_EQ(
        sizeof(ryu_ldn::protocol::CreateAccessPointPrivateRequest), 0x13CU);
}

/**
 * @brief CreateAccessPointPrivateRequest stores every nested config
 *        it carries.
 */
TEST(protocol_create_access_point_private_request_fields) {
    ryu_ldn::protocol::CreateAccessPointPrivateRequest req{};

    // Security config
    req.security_config.security_mode = 2;
    req.security_config.passphrase_size = 8;
    memcpy(req.security_config.passphrase, "password", 8);

    // Security parameter
    req.security_parameter.data[0] = 0xAA;
    req.security_parameter.session_id[0] = 0xBB;

    // User config
    memcpy(req.user_config.user_name, "PrivateHost", 12);

    // Network config
    req.network_config.intent_id.local_communication_id = 0x0100000000001234ULL;
    req.network_config.node_count_max = 8;

    // Address list
    req.address_list.addresses[0].ipv4_address = 0x0A720001;

    // Ryu network config
    req.ryu_network_config.address_family = 2;  // IPv4
    req.ryu_network_config.external_proxy_port = 30456;

    ASSERT_EQ(req.security_config.security_mode, 2U);
    ASSERT_EQ(req.security_parameter.data[0], 0xAAU);
    ASSERT_EQ(req.security_parameter.session_id[0], 0xBBU);
    ASSERT_STREQ(req.user_config.user_name, "PrivateHost");
    ASSERT_EQ(req.network_config.node_count_max, 8U);
    ASSERT_EQ(req.address_list.addresses[0].ipv4_address, 0x0A720001U);
}

/**
 * @brief ConnectPrivateRequest is 0xBC (188) bytes.
 */
TEST(protocol_connect_private_request_size) {
    // 0xBC = 188 bytes
    // SecurityConfig(0x44) + SecurityParameter(0x20) + UserConfig(0x30) +
    // u32(4) + u32(4) + NetworkConfig(0x20)
    ASSERT_EQ(sizeof(ryu_ldn::protocol::ConnectPrivateRequest), 0xBCU);
}

/**
 * @brief ConnectPrivateRequest stores its nested configs, version and
 *        options.
 */
TEST(protocol_connect_private_request_fields) {
    ryu_ldn::protocol::ConnectPrivateRequest req{};

    // Security config
    req.security_config.security_mode = 2;
    req.security_config.passphrase_size = 8;
    memcpy(req.security_config.passphrase, "password", 8);

    // Security parameter
    req.security_parameter.data[0] = 0xCC;
    req.security_parameter.session_id[0] = 0xDD;

    // User config
    memcpy(req.user_config.user_name, "PrivateClient", 14);

    // Version and options
    req.local_communication_version = 1;
    req.option_unknown = 0;

    // Network config
    req.network_config.intent_id.local_communication_id = 0x0100000000005678ULL;
    req.network_config.node_count_max = 4;

    ASSERT_EQ(req.security_config.security_mode, 2U);
    ASSERT_EQ(req.security_parameter.data[0], 0xCCU);
    ASSERT_EQ(req.security_parameter.session_id[0], 0xDDU);
    ASSERT_STREQ(req.user_config.user_name, "PrivateClient");
    ASSERT_EQ(req.local_communication_version, 1U);
    ASSERT_EQ(req.network_config.node_count_max, 4U);
}

// ============================================================================
// External Proxy Types Tests (Compatibility Fix)
// ============================================================================

/**
 * @brief ExternalProxyConfig is 0x26 (38) bytes.
 */
TEST(protocol_external_proxy_config_size) {
    ASSERT_EQ(
        sizeof(ryu_ldn::protocol::ExternalProxyConfig), 0x26U);  // 38 bytes
}

/**
 * @brief ExternalProxyConfig stores IP, family, port and token.
 */
TEST(protocol_external_proxy_config_fields) {
    ryu_ldn::protocol::ExternalProxyConfig cfg{};
    cfg.proxy_ip[0] = '1';
    cfg.proxy_ip[1] = '2';
    cfg.proxy_ip[2] = '7';
    cfg.address_family = 2;  // IPv4
    cfg.proxy_port = 30456;
    cfg.token[0] = 0xAA;

    ASSERT_EQ(cfg.proxy_ip[0], static_cast<uint8_t>('1'));
    ASSERT_EQ(cfg.address_family, 2U);
    ASSERT_EQ(cfg.proxy_port, 30456U);
    ASSERT_EQ(cfg.token[0], 0xAAU);
}

/**
 * @brief ExternalProxyToken is 0x28 (40) bytes.
 */
TEST(protocol_external_proxy_token_size) {
    ASSERT_EQ(
        sizeof(ryu_ldn::protocol::ExternalProxyToken), 0x28U);  // 40 bytes
}

/**
 * @brief ExternalProxyToken stores virtual IP, token, physical IP and
 *        address family.
 */
TEST(protocol_external_proxy_token_fields) {
    ryu_ldn::protocol::ExternalProxyToken tok{};
    tok.virtual_ip = 0x0A720001;  // 10.114.0.1
    tok.token[0] = 0xBB;
    tok.physical_ip[0] = '1';
    tok.address_family = 2;  // IPv4

    ASSERT_EQ(tok.virtual_ip, 0x0A720001U);
    ASSERT_EQ(tok.token[0], 0xBBU);
    ASSERT_EQ(tok.address_family, 2U);
}

/**
 * @brief ExternalProxyConnectionState is 8 bytes.
 */
TEST(protocol_external_proxy_connection_state_size) {
    ASSERT_EQ(
        sizeof(ryu_ldn::protocol::ExternalProxyConnectionState),
        0x08U);  // 8 bytes
}

/**
 * @brief ExternalProxyConnectionState stores the IP and the connected
 *        flag.
 */
TEST(protocol_external_proxy_connection_state_fields) {
    ryu_ldn::protocol::ExternalProxyConnectionState state{};
    state.ip_address = 0x0A720001;
    state.connected = 1;

    ASSERT_EQ(state.ip_address, 0x0A720001U);
    ASSERT_EQ(state.connected, 1U);
}

/**
 * @brief SetAcceptPolicyRequest is 1 byte, as on the Ryujinx server.
 */
TEST(protocol_set_accept_policy_request_size) {
    // Must be 1 byte to match Ryujinx/Server
    ASSERT_EQ(sizeof(ryu_ldn::protocol::SetAcceptPolicyRequest), 1U);
}

/**
 * @brief SetAcceptPolicyRequest stores the accept policy.
 */
TEST(protocol_set_accept_policy_request_fields) {
    ryu_ldn::protocol::SetAcceptPolicyRequest req{};
    req.accept_policy = 2;  // BlackList

    ASSERT_EQ(req.accept_policy, 2U);
}

// ============================================================================
// Main
// ============================================================================

/**
 * @brief Print the totals; tests have already run from static initialisers.
 * @return 1 when at least one test failed, otherwise 0.
 */
int main() {
    printf("\n========================================\n");
    printf("  LDN Types Tests - ryu_ldn_nx\n");
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