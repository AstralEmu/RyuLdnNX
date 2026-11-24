/**
 * @file protocol_tests.cpp
 * @brief Unit tests for RyuLdn Protocol module
 *
 * Tests cover:
 * - Structure sizes and alignment (static_assert compile-time)
 * - Packet encoding
 * - Packet decoding
 * - Encode/decode round-trip
 * - Error handling (invalid packets)
 * - PacketBuffer TCP fragmentation handling
 */

#include "protocol/types.hpp"
#include "protocol/ryu_protocol.hpp"
#include "protocol/packet_buffer.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <type_traits>

using ryu_ldn::protocol::AcceptPolicy;
using ryu_ldn::protocol::BufferResult;
using ryu_ldn::protocol::check_complete_packet;
using ryu_ldn::protocol::CommonNetworkInfo;
using ryu_ldn::protocol::ConnectPrivateRequest;
using ryu_ldn::protocol::ConnectRequest;
using ryu_ldn::protocol::CreateAccessPointRequest;
using ryu_ldn::protocol::DecodeResult;
using ryu_ldn::protocol::DisconnectMessage;
using ryu_ldn::protocol::DisconnectReason;
using ryu_ldn::protocol::encode_disconnect;
using ryu_ldn::protocol::encode_passphrase;
using ryu_ldn::protocol::encode_ping;
using ryu_ldn::protocol::encode_reject_reply;
using ryu_ldn::protocol::encode_scan_reply_end;
using ryu_ldn::protocol::encode_set_advertise_data;
using ryu_ldn::protocol::EncodeResult;
using ryu_ldn::protocol::ExternalProxyConfig;
using ryu_ldn::protocol::ExternalProxyConnectionState;
using ryu_ldn::protocol::ExternalProxyToken;
using ryu_ldn::protocol::get_packet_size;
using ryu_ldn::protocol::get_packet_type;
using ryu_ldn::protocol::get_payload_ptr;
using ryu_ldn::protocol::get_payload_size;
using ryu_ldn::protocol::has_header;
using ryu_ldn::protocol::InitializeMessage;
using ryu_ldn::protocol::IntentId;
using ryu_ldn::protocol::LdnHeader;
using ryu_ldn::protocol::LdnNetworkInfo;
using ryu_ldn::protocol::MacAddress;
using ryu_ldn::protocol::MAX_PACKET_SIZE;
using ryu_ldn::protocol::NetworkConfig;
using ryu_ldn::protocol::NetworkId;
using ryu_ldn::protocol::NetworkInfo;
using ryu_ldn::protocol::NodeInfo;
using ryu_ldn::protocol::PacketBuffer;
using ryu_ldn::protocol::PacketId;
using ryu_ldn::protocol::PassphraseMessage;
using ryu_ldn::protocol::PingMessage;
using ryu_ldn::protocol::PROTOCOL_MAGIC;
using ryu_ldn::protocol::PROTOCOL_VERSION;
using ryu_ldn::protocol::ProtocolType;
using ryu_ldn::protocol::ProxyConfig;
using ryu_ldn::protocol::ProxyConnectRequest;
using ryu_ldn::protocol::ProxyConnectResponse;
using ryu_ldn::protocol::ProxyDataHeader;
using ryu_ldn::protocol::ProxyDisconnectMessage;
using ryu_ldn::protocol::ProxyInfo;
using ryu_ldn::protocol::RejectRequest;
using ryu_ldn::protocol::RyuNetworkConfig;
using ryu_ldn::protocol::ScanFilterFull;
using ryu_ldn::protocol::SecurityConfig;
using ryu_ldn::protocol::SecurityMode;
using ryu_ldn::protocol::SessionId;
using ryu_ldn::protocol::SetAcceptPolicyRequest;
using ryu_ldn::protocol::Ssid;
using ryu_ldn::protocol::UserConfig;

/**
 * @brief Build an enum value from its underlying raw value.
 *
 * A scoped enumeration has a fixed underlying type, so every value of that
 * type is a valid value of the enum; the value given here names no enumerator
 * on purpose, to exercise the "Unknown" fallback of the to_string helpers.
 *
 * @tparam Enum Enum type to build.
 * @param raw Underlying value to reinterpret.
 * @return The enum value holding raw.
 */
template <typename Enum>
Enum unknown_enum_value(std::underlying_type_t<Enum> raw) {
    Enum value{};
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

// ============================================================================
// Test Framework (minimal, no external dependencies)
// ============================================================================

/// Number of test cases started so far.
static int g_tests_run = 0;
/// Number of test cases that finished without a failed assertion.
static int g_tests_passed = 0;
/// Number of test cases that hit a failed assertion.
static int g_tests_failed = 0;

/// Defines a test function and registers it in the test table.
#define TEST(name)                             \
    static void test_##name();                 \
    static struct TestRegister_##name {        \
        TestRegister_##name() {                \
            register_test(#name, test_##name); \
        }                                      \
    } g_register_##name;                       \
    static void test_##name()

/// Fails the current test when the expression is false.
#define ASSERT_TRUE(expr)                                      \
    do {                                                       \
        if (!(expr)) {                                         \
            printf("  FAIL: %s (line %d)\n", #expr, __LINE__); \
            throw std::runtime_error("assertion failed");      \
        }                                                      \
    } while (0)

/// Fails the current test when the expression is true.
#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

/// Fails the current test when the two values differ.
#define ASSERT_EQ(a, b)                                               \
    do {                                                              \
        auto _a = static_cast<long long>(a);                          \
        auto _b = static_cast<long long>(b);                          \
        if (_a != _b) {                                               \
            printf("  FAIL: %s == %s (line %d)\n", #a, #b, __LINE__); \
            printf("    got: %lld vs %lld\n", _a, _b);                \
            throw std::runtime_error("assertion failed");             \
        }                                                             \
    } while (0)

/// Fails the current test when the two values are equal.
#define ASSERT_NE(a, b)                                               \
    do {                                                              \
        if ((a) == (b)) {                                             \
            printf("  FAIL: %s != %s (line %d)\n", #a, #b, __LINE__); \
            throw std::runtime_error("assertion failed");             \
        }                                                             \
    } while (0)

/// One registered test case (name and body).
struct TestEntry {
    /// Test name printed by main().
    const char* name;
    /// Test body; throws on a failed assertion.
    void (*func)();
};

/// Table of registered test cases.
static TestEntry g_tests[256];
/// Number of entries used in the test table.
static int g_test_count = 0;

/**
 * @brief Appends a test case to the test table.
 *
 * @param name test name printed by main()
 * @param func test body; throws on a failed assertion
 */
void register_test(const char* name, void (*func)()) {
    if (g_test_count < 256) {
        g_tests[g_test_count++] = {.name = name, .func = func};
    }
}

/// Runs every registered test case and counts the outcomes.
void run_all_tests() {
    printf("Running %d tests...\n\n", g_test_count);

    for (int i = 0; i < g_test_count; i++) {
        printf("[%d/%d] %s... ", i + 1, g_test_count, g_tests[i].name);
        fflush(stdout);
        g_tests_run++;

        try {
            g_tests[i].func();
            printf("OK\n");
            g_tests_passed++;
        } catch (...) {
            g_tests_failed++;
        }
    }

    printf("\n========================================\n");
    printf(
        "Results: %d passed, %d failed, %d total\n",
        g_tests_passed,
        g_tests_failed,
        g_tests_run);

    if (g_tests_failed > 0) {
        printf("FAILED\n");
    } else {
        printf("ALL TESTS PASSED\n");
    }
}

// ============================================================================
// Structure Size Tests (compile-time via static_assert in types.hpp)
// These tests verify at runtime that sizes match expected values
// ============================================================================

/** @brief Test case: structure sizes. */
TEST(structure_sizes) {
    // Core structures
    ASSERT_EQ(
        sizeof(LdnHeader), 0xC);  // 12 bytes (aligned for Switch compatibility)
    ASSERT_EQ(sizeof(MacAddress), 6);
    ASSERT_EQ(sizeof(Ssid), 0x22);       // 34 bytes
    ASSERT_EQ(sizeof(NetworkId), 0x20);  // 32 bytes
    ASSERT_EQ(sizeof(SessionId), 0x10);  // 16 bytes
    ASSERT_EQ(sizeof(IntentId), 0x10);   // 16 bytes

    // Node and Network Info
    ASSERT_EQ(sizeof(NodeInfo), 0x40);           // 64 bytes
    ASSERT_EQ(sizeof(CommonNetworkInfo), 0x30);  // 48 bytes
    ASSERT_EQ(sizeof(LdnNetworkInfo), 0x430);    // 1072 bytes
    ASSERT_EQ(sizeof(NetworkInfo), 0x480);       // 1152 bytes

    // Messages
    ASSERT_EQ(sizeof(InitializeMessage), 0x16);  // 22 bytes
    ASSERT_EQ(sizeof(PassphraseMessage), 0x80);  // 128 bytes
    ASSERT_EQ(sizeof(PingMessage), 2);
    ASSERT_EQ(sizeof(DisconnectMessage), 4);  // 4 bytes (DisconnectIP only)

    // Request structures
    ASSERT_EQ(sizeof(SecurityConfig), 0x44);
    ASSERT_EQ(sizeof(UserConfig), 0x30);
    ASSERT_EQ(sizeof(NetworkConfig), 0x20);
    ASSERT_EQ(sizeof(RyuNetworkConfig), 0x28);
    ASSERT_EQ(sizeof(CreateAccessPointRequest), 0xBC);
    ASSERT_EQ(sizeof(ScanFilterFull), 0x60);   // 96 bytes (Pack=8 alignment)
    ASSERT_EQ(sizeof(ConnectRequest), 0x500);  // 1280 bytes
    ASSERT_EQ(sizeof(RejectRequest), 8);  // 8 bytes (NodeId + DisconnectReason)

    // Proxy structures
    ASSERT_EQ(sizeof(ProxyInfo), 0x10);  // 16 bytes
    ASSERT_EQ(sizeof(ProxyConfig), 8);   // 8 bytes (ip + subnetmask)
    ASSERT_EQ(
        sizeof(ProxyDataHeader), 0x14);  // 20 bytes (ProxyInfo + DataLength)
    ASSERT_EQ(sizeof(ProxyConnectRequest), 0x10);   // 16 bytes (ProxyInfo)
    ASSERT_EQ(sizeof(ProxyConnectResponse), 0x10);  // 16 bytes (ProxyInfo)
    ASSERT_EQ(
        sizeof(ProxyDisconnectMessage), 0x14);  // 20 bytes (ProxyInfo + reason)
}

/** @brief Test case: protocol constants. */
TEST(protocol_constants) {
    // PROTOCOL_MAGIC = 'R' | ('L' << 8) | ('D' << 16) | ('N' << 24) =
    // 0x4E444C52
    ASSERT_EQ(PROTOCOL_MAGIC, 0x4E444C52U);
    ASSERT_EQ(PROTOCOL_VERSION, 1);
}

// ============================================================================
// Encode Tests
// ============================================================================

/** @brief Test case: encode header only packet. */
TEST(encode_header_only_packet) {
    uint8_t buffer[64];
    size_t out_size = 0;

    EncodeResult result =
        encode(buffer, sizeof(buffer), PacketId::ScanReplyEnd, out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader));

    // Verify header contents
    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    ASSERT_EQ(header->magic, PROTOCOL_MAGIC);
    ASSERT_EQ(header->type, static_cast<uint8_t>(PacketId::ScanReplyEnd));
    ASSERT_EQ(header->version, PROTOCOL_VERSION);
    ASSERT_EQ(header->data_size, 0);
}

/** @brief Test case: encode ping packet. */
TEST(encode_ping_packet) {
    uint8_t buffer[64];
    size_t out_size = 0;
    uint8_t requester = 1;
    uint8_t id = 42;

    EncodeResult result =
        encode_ping(buffer, sizeof(buffer), requester, id, out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader) + sizeof(PingMessage));

    // Verify header
    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    ASSERT_EQ(header->magic, PROTOCOL_MAGIC);
    ASSERT_EQ(header->type, static_cast<uint8_t>(PacketId::Ping));
    ASSERT_EQ(header->data_size, sizeof(PingMessage));

    // Verify payload
    auto* msg = reinterpret_cast<PingMessage*>(buffer + sizeof(LdnHeader));
    ASSERT_EQ(msg->requester, requester);
    ASSERT_EQ(msg->id, id);
}

/** @brief Test case: encode initialize packet. */
TEST(encode_initialize_packet) {
    uint8_t buffer[64];
    size_t out_size = 0;

    SessionId session_id = {};
    // Set first 8 bytes to a recognizable pattern
    session_id.data[0] = 0xDE;
    session_id.data[1] = 0xAD;
    session_id.data[2] = 0xBE;
    session_id.data[3] = 0xEF;

    MacAddress mac = {};
    mac.data[0] = 0x11;
    mac.data[1] = 0x22;
    mac.data[2] = 0x33;
    mac.data[3] = 0x44;
    mac.data[4] = 0x55;
    mac.data[5] = 0x66;

    EncodeResult result =
        encode_initialize(buffer, sizeof(buffer), session_id, mac, out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader) + sizeof(InitializeMessage));

    // Verify payload
    auto* msg =
        reinterpret_cast<InitializeMessage*>(buffer + sizeof(LdnHeader));
    ASSERT_EQ(msg->id.data[0], 0xDE);
    ASSERT_EQ(msg->id.data[3], 0xEF);
    ASSERT_EQ(msg->mac_address.data[0], 0x11);
    ASSERT_EQ(msg->mac_address.data[5], 0x66);
}

/** @brief Test case: encode buffer too small. */
TEST(encode_buffer_too_small) {
    uint8_t buffer[4];  // Too small for header
    size_t out_size = 0;

    EncodeResult result =
        encode(buffer, sizeof(buffer), PacketId::Ping, out_size);

    ASSERT_EQ(result, EncodeResult::BufferTooSmall);
    ASSERT_EQ(out_size, 0);
}

/** @brief Test case: encode disconnect packet. */
TEST(encode_disconnect_packet) {
    uint8_t buffer[64];
    size_t out_size = 0;

    // Disconnect message now contains IP address of disconnecting client
    uint32_t disconnect_ip = 0xC0A80101;  // 192.168.1.1
    EncodeResult result =
        encode_disconnect(buffer, sizeof(buffer), disconnect_ip, out_size);

    ASSERT_EQ(result, EncodeResult::Success);

    auto* msg =
        reinterpret_cast<DisconnectMessage*>(buffer + sizeof(LdnHeader));
    ASSERT_EQ(msg->disconnect_ip, disconnect_ip);
}

// ============================================================================
// Decode Tests
// ============================================================================

/** @brief Test case: decode valid header. */
TEST(decode_valid_header) {
    uint8_t buffer[sizeof(LdnHeader)];
    auto* header_in = reinterpret_cast<LdnHeader*>(buffer);
    header_in->magic = PROTOCOL_MAGIC;
    header_in->type = static_cast<uint8_t>(PacketId::Ping);
    header_in->version = PROTOCOL_VERSION;
    header_in->data_size = 8;

    LdnHeader header_out{};
    DecodeResult result = decode_header(buffer, sizeof(buffer), header_out);

    ASSERT_EQ(result, DecodeResult::Success);
    ASSERT_EQ(header_out.magic, PROTOCOL_MAGIC);
    ASSERT_EQ(header_out.type, static_cast<uint8_t>(PacketId::Ping));
    ASSERT_EQ(header_out.data_size, 8);
}

/** @brief Test case: decode invalid magic. */
TEST(decode_invalid_magic) {
    uint8_t buffer[sizeof(LdnHeader)];
    auto* header_in = reinterpret_cast<LdnHeader*>(buffer);
    header_in->magic = 0xDEADBEEF;  // Wrong magic
    header_in->type = 0;
    header_in->version = PROTOCOL_VERSION;
    header_in->data_size = 0;

    LdnHeader header_out{};
    DecodeResult result = decode_header(buffer, sizeof(buffer), header_out);

    ASSERT_EQ(result, DecodeResult::InvalidMagic);
}

/** @brief Test case: decode invalid version. */
TEST(decode_invalid_version) {
    uint8_t buffer[sizeof(LdnHeader)];
    auto* header_in = reinterpret_cast<LdnHeader*>(buffer);
    header_in->magic = PROTOCOL_MAGIC;
    header_in->type = 0;
    header_in->version = 99;  // Wrong version
    header_in->data_size = 0;

    LdnHeader header_out{};
    DecodeResult result = decode_header(buffer, sizeof(buffer), header_out);

    ASSERT_EQ(result, DecodeResult::InvalidVersion);
}

/** @brief Test case: decode packet too large. */
TEST(decode_packet_too_large) {
    uint8_t buffer[sizeof(LdnHeader)];
    auto* header_in = reinterpret_cast<LdnHeader*>(buffer);
    header_in->magic = PROTOCOL_MAGIC;
    header_in->type = 0;
    header_in->version = PROTOCOL_VERSION;
    // MAX_PACKET_SIZE is 131072, so 131073 should be rejected
    header_in->data_size = 131073;

    LdnHeader header_out{};
    DecodeResult result = decode_header(buffer, sizeof(buffer), header_out);

    ASSERT_EQ(result, DecodeResult::PacketTooLarge);
}

/** @brief Test case: decode buffer too small. */
TEST(decode_buffer_too_small) {
    uint8_t buffer[4] = {0};  // Less than header size, initialized

    LdnHeader header_out{};
    DecodeResult result = decode_header(buffer, sizeof(buffer), header_out);

    ASSERT_EQ(result, DecodeResult::BufferTooSmall);
}

/** @brief Test case: decode ping packet. */
TEST(decode_ping_packet) {
    uint8_t buffer[sizeof(LdnHeader) + sizeof(PingMessage)];

    // Build packet
    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    header->magic = PROTOCOL_MAGIC;
    header->type = static_cast<uint8_t>(PacketId::Ping);
    header->version = PROTOCOL_VERSION;
    header->data_size = sizeof(PingMessage);

    auto* msg_in = reinterpret_cast<PingMessage*>(buffer + sizeof(LdnHeader));
    msg_in->requester = 0;
    msg_in->id = 99;

    // Decode
    LdnHeader header_out{};
    PingMessage msg_out{};
    DecodeResult result =
        decode_ping(buffer, sizeof(buffer), header_out, msg_out);

    ASSERT_EQ(result, DecodeResult::Success);
    ASSERT_EQ(msg_out.requester, 0);
    ASSERT_EQ(msg_out.id, 99);
}

/** @brief Test case: check complete packet success. */
TEST(check_complete_packet_success) {
    uint8_t buffer[sizeof(LdnHeader) + 8];

    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    header->magic = PROTOCOL_MAGIC;
    header->type = static_cast<uint8_t>(PacketId::Ping);
    header->version = PROTOCOL_VERSION;
    header->data_size = 8;

    size_t packet_size;
    DecodeResult result =
        check_complete_packet(buffer, sizeof(buffer), packet_size);

    ASSERT_EQ(result, DecodeResult::Success);
    ASSERT_EQ(packet_size, sizeof(LdnHeader) + 8);
}

/** @brief Test case: check complete packet incomplete. */
TEST(check_complete_packet_incomplete) {
    uint8_t buffer[sizeof(LdnHeader) + 4];  // Only 4 bytes of payload

    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    header->magic = PROTOCOL_MAGIC;
    header->type = static_cast<uint8_t>(PacketId::Ping);
    header->version = PROTOCOL_VERSION;
    header->data_size = 8;  // Says 8 bytes, but buffer only has 4

    size_t packet_size;
    DecodeResult result =
        check_complete_packet(buffer, sizeof(buffer), packet_size);

    ASSERT_EQ(result, DecodeResult::IncompletePacket);
}

// ============================================================================
// Round-Trip Tests (Encode then Decode)
// ============================================================================

/** @brief Test case: roundtrip ping. */
TEST(roundtrip_ping) {
    uint8_t buffer[64];
    size_t encoded_size = 0;
    uint8_t original_requester = 0;
    uint8_t original_id = 55;

    // Encode
    EncodeResult enc_result = encode_ping(
        buffer, sizeof(buffer), original_requester, original_id, encoded_size);
    ASSERT_EQ(enc_result, EncodeResult::Success);

    // Decode
    LdnHeader header{};
    PingMessage msg{};
    DecodeResult dec_result = decode_ping(buffer, encoded_size, header, msg);

    ASSERT_EQ(dec_result, DecodeResult::Success);
    ASSERT_EQ(msg.requester, original_requester);
    ASSERT_EQ(msg.id, original_id);
}

/** @brief Test case: roundtrip disconnect. */
TEST(roundtrip_disconnect) {
    uint8_t buffer[64];
    size_t encoded_size = 0;

    // Encode with IP address (new format)
    uint32_t disconnect_ip = 0x0A000001;  // 10.0.0.1
    EncodeResult enc_result =
        encode_disconnect(buffer, sizeof(buffer), disconnect_ip, encoded_size);
    ASSERT_EQ(enc_result, EncodeResult::Success);

    // Decode
    LdnHeader header{};
    DisconnectMessage msg{};
    DecodeResult dec_result =
        decode_disconnect(buffer, encoded_size, header, msg);

    ASSERT_EQ(dec_result, DecodeResult::Success);
    ASSERT_EQ(msg.disconnect_ip, disconnect_ip);
}

/** @brief Test case: roundtrip initialize. */
TEST(roundtrip_initialize) {
    uint8_t buffer[64];
    size_t encoded_size = 0;

    SessionId session = {};
    session.data[0] = 0xCA;
    session.data[1] = 0xFE;
    session.data[2] = 0xBA;
    session.data[3] = 0xBE;

    MacAddress mac = {};
    mac.data[0] = 0xAA;
    mac.data[5] = 0xFF;

    // Encode
    EncodeResult enc_result =
        encode_initialize(buffer, sizeof(buffer), session, mac, encoded_size);
    ASSERT_EQ(enc_result, EncodeResult::Success);

    // Decode
    LdnHeader header{};
    InitializeMessage msg{};
    DecodeResult dec_result =
        decode_initialize(buffer, encoded_size, header, msg);

    ASSERT_EQ(dec_result, DecodeResult::Success);
    ASSERT_EQ(msg.id.data[0], session.data[0]);
    ASSERT_EQ(msg.id.data[3], session.data[3]);
    ASSERT_EQ(msg.mac_address.data[0], 0xAA);
    ASSERT_EQ(msg.mac_address.data[5], 0xFF);
}

// ============================================================================
// PacketBuffer Tests
// ============================================================================

/** @brief Test case: buffer empty initially. */
TEST(buffer_empty_initially) {
    PacketBuffer<1024> buffer;

    ASSERT_TRUE(buffer.empty());
    ASSERT_EQ(buffer.size(), 0);
    ASSERT_FALSE(buffer.has_complete_packet());
}

/** @brief Test case: buffer append data. */
TEST(buffer_append_data) {
    PacketBuffer<1024> buffer;
    uint8_t data[] = {1, 2, 3, 4, 5};

    BufferResult result = buffer.append(data, sizeof(data));

    ASSERT_EQ(result, BufferResult::Success);
    ASSERT_EQ(buffer.size(), 5);
    ASSERT_FALSE(buffer.empty());
}

/** @brief Test case: buffer complete packet single append. */
TEST(buffer_complete_packet_single_append) {
    PacketBuffer<1024> buffer;

    // Create a complete packet (requester=1, id=23)
    uint8_t packet[sizeof(LdnHeader) + sizeof(PingMessage)];
    size_t packet_size;
    encode_ping(packet, sizeof(packet), 1, 23, packet_size);

    // Append entire packet
    buffer.append(packet, packet_size);

    ASSERT_TRUE(buffer.has_complete_packet());

    size_t peek_size;
    const uint8_t* peek_data = buffer.peek_packet(peek_size);
    ASSERT_NE(peek_data, nullptr);
    ASSERT_EQ(peek_size, packet_size);
}

/** @brief Test case: buffer fragmented packet 2 parts. */
TEST(buffer_fragmented_packet_2_parts) {
    PacketBuffer<1024> buffer;

    // Create a complete packet (requester=1, id=23)
    uint8_t packet[sizeof(LdnHeader) + sizeof(PingMessage)];
    size_t packet_size;
    encode_ping(packet, sizeof(packet), 1, 23, packet_size);

    // Append in 2 parts
    size_t part1 = sizeof(LdnHeader) / 2;  // Half of header
    size_t part2 = packet_size - part1;

    buffer.append(packet, part1);
    ASSERT_FALSE(buffer.has_complete_packet());

    buffer.append(packet + part1, part2);
    ASSERT_TRUE(buffer.has_complete_packet());
}

/** @brief Test case: buffer fragmented packet n parts. */
TEST(buffer_fragmented_packet_n_parts) {
    PacketBuffer<1024> buffer;

    // Create a complete packet (requester=1, id=42)
    uint8_t packet[sizeof(LdnHeader) + sizeof(PingMessage)];
    size_t packet_size;
    encode_ping(packet, sizeof(packet), 1, 42, packet_size);

    // Append byte by byte
    for (size_t i = 0; i < packet_size - 1; i++) {
        buffer.append(&packet[i], 1);
        ASSERT_FALSE(buffer.has_complete_packet());
    }

    // Last byte completes the packet
    buffer.append(&packet[packet_size - 1], 1);
    ASSERT_TRUE(buffer.has_complete_packet());
}

/** @brief Test case: buffer multiple packets. */
TEST(buffer_multiple_packets) {
    PacketBuffer<1024> buffer;

    // Create 3 packets
    uint8_t packet1[32];
    uint8_t packet2[32];
    uint8_t packet3[32];
    size_t size1;
    size_t size2;
    size_t size3;

    encode_ping(packet1, sizeof(packet1), 1, 11, size1);
    encode_ping(packet2, sizeof(packet2), 0, 22, size2);
    encode_ping(packet3, sizeof(packet3), 1, 33, size3);

    // Append all at once
    buffer.append(packet1, size1);
    buffer.append(packet2, size2);
    buffer.append(packet3, size3);

    // Extract and verify each
    ASSERT_TRUE(buffer.has_complete_packet());

    size_t pkt_size;
    buffer.peek_packet(pkt_size);
    buffer.consume(pkt_size);

    ASSERT_TRUE(buffer.has_complete_packet());
    buffer.peek_packet(pkt_size);
    buffer.consume(pkt_size);

    ASSERT_TRUE(buffer.has_complete_packet());
    buffer.peek_packet(pkt_size);
    buffer.consume(pkt_size);

    ASSERT_FALSE(buffer.has_complete_packet());
    ASSERT_TRUE(buffer.empty());
}

/** @brief Test case: buffer extract packet. */
TEST(buffer_extract_packet) {
    PacketBuffer<1024> buffer;

    uint8_t packet[32];
    size_t packet_size;
    encode_ping(packet, sizeof(packet), 0, 77, packet_size);

    buffer.append(packet, packet_size);

    uint8_t out[32];
    size_t out_size;
    BufferResult result = buffer.extract_packet(out, sizeof(out), out_size);

    ASSERT_EQ(result, BufferResult::Success);
    ASSERT_EQ(out_size, packet_size);
    ASSERT_TRUE(buffer.empty());

    // Verify extracted data
    LdnHeader header{};
    PingMessage msg{};
    decode_ping(out, out_size, header, msg);
    ASSERT_EQ(msg.requester, 0);
    ASSERT_EQ(msg.id, 77);
}

/** @brief Test case: buffer overflow protection. */
TEST(buffer_overflow_protection) {
    PacketBuffer<64> buffer;  // Small buffer

    uint8_t data[128];
    std::memset(data, 0, sizeof(data));

    BufferResult result = buffer.append(data, sizeof(data));

    ASSERT_EQ(result, BufferResult::BufferFull);
}

/** @brief Test case: buffer reset. */
TEST(buffer_reset) {
    PacketBuffer<1024> buffer;

    uint8_t data[] = {1, 2, 3, 4, 5};
    buffer.append(data, sizeof(data));
    ASSERT_FALSE(buffer.empty());

    buffer.reset();

    ASSERT_TRUE(buffer.empty());
    ASSERT_EQ(buffer.size(), 0);
}

// ============================================================================
// Utility Function Tests
// ============================================================================

/** @brief Test case: result to string. */
TEST(result_to_string) {
    ASSERT_TRUE(
        std::strcmp(
            decode_result_to_string(DecodeResult::Success), "Success") == 0);
    ASSERT_TRUE(
        std::strcmp(
            decode_result_to_string(DecodeResult::InvalidMagic),
            "InvalidMagic") == 0);

    ASSERT_TRUE(
        std::strcmp(
            encode_result_to_string(EncodeResult::Success), "Success") == 0);
    ASSERT_TRUE(
        std::strcmp(
            encode_result_to_string(EncodeResult::BufferTooSmall),
            "BufferTooSmall") == 0);

    ASSERT_TRUE(
        std::strcmp(packet_id_to_string(PacketId::Initialize), "Initialize") ==
        0);
    ASSERT_TRUE(std::strcmp(packet_id_to_string(PacketId::Ping), "Ping") == 0);

    ASSERT_TRUE(
        std::strcmp(
            buffer_result_to_string(BufferResult::Success), "Success") == 0);
}

// ============================================================================
// Additional Encode/Decode Tests (Full Coverage)
// ============================================================================

/** @brief Test case: encode passphrase packet. */
TEST(encode_passphrase_packet) {
    uint8_t buffer[256];
    size_t out_size = 0;
    const char* passphrase = "TestPassphrase123";

    EncodeResult result = encode_passphrase(
        buffer,
        sizeof(buffer),
        reinterpret_cast<const uint8_t*>(passphrase),
        std::strlen(passphrase),
        out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader) + sizeof(PassphraseMessage));

    // Verify header
    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    ASSERT_EQ(header->type, static_cast<uint8_t>(PacketId::Passphrase));
    ASSERT_EQ(
        header->data_size, static_cast<int32_t>(sizeof(PassphraseMessage)));

    // Verify payload
    auto* msg =
        reinterpret_cast<PassphraseMessage*>(buffer + sizeof(LdnHeader));
    ASSERT_TRUE(
        std::memcmp(msg->passphrase, passphrase, std::strlen(passphrase)) == 0);
}

/** @brief Test case: roundtrip passphrase. */
TEST(roundtrip_passphrase) {
    uint8_t buffer[256];
    size_t encoded_size = 0;
    const char* original = "MySecretPass";

    // Encode
    EncodeResult enc_result = encode_passphrase(
        buffer,
        sizeof(buffer),
        reinterpret_cast<const uint8_t*>(original),
        std::strlen(original),
        encoded_size);
    ASSERT_EQ(enc_result, EncodeResult::Success);

    // Decode
    LdnHeader header{};
    PassphraseMessage msg{};
    DecodeResult dec_result =
        decode_passphrase(buffer, encoded_size, header, msg);

    ASSERT_EQ(dec_result, DecodeResult::Success);
    ASSERT_TRUE(
        std::memcmp(msg.passphrase, original, std::strlen(original)) == 0);
}

/** @brief Test case: encode scan packet. */
TEST(encode_scan_packet) {
    uint8_t buffer[256];
    size_t out_size = 0;

    ScanFilterFull filter{};
    filter.flag = 0x25;  // Some filter flags
    filter.network_id.intent_id.local_communication_id = 0xDEADBEEF;

    EncodeResult result = encode_scan(buffer, sizeof(buffer), filter, out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader) + sizeof(ScanFilterFull));

    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    ASSERT_EQ(header->type, static_cast<uint8_t>(PacketId::Scan));
}

/** @brief Test case: roundtrip scan. */
TEST(roundtrip_scan) {
    uint8_t buffer[256];
    size_t encoded_size = 0;

    ScanFilterFull original{};
    original.flag = 0x37;
    original.network_type = 2;
    original.network_id.intent_id.local_communication_id = 0xCAFEBABE;

    // Encode
    EncodeResult enc_result =
        encode_scan(buffer, sizeof(buffer), original, encoded_size);
    ASSERT_EQ(enc_result, EncodeResult::Success);

    // Decode
    LdnHeader header{};
    ScanFilterFull decoded{};
    DecodeResult dec_result =
        decode_scan(buffer, encoded_size, header, decoded);

    ASSERT_EQ(dec_result, DecodeResult::Success);
    ASSERT_EQ(decoded.flag, original.flag);
    ASSERT_EQ(decoded.network_type, original.network_type);
    ASSERT_EQ(
        decoded.network_id.intent_id.local_communication_id,
        original.network_id.intent_id.local_communication_id);
}

/** @brief Test case: encode connect packet. */
TEST(encode_connect_packet) {
    uint8_t buffer[2048];
    size_t out_size = 0;

    ConnectRequest request{};
    request.security_config.security_mode =
        static_cast<uint16_t>(SecurityMode::Product);
    request.security_config.passphrase_size = 32;
    request.user_config.user_name[0] = 'T';
    request.user_config.user_name[1] = 'e';
    request.user_config.user_name[2] = 's';
    request.user_config.user_name[3] = 't';
    request.network_info.ldn.security_mode =
        static_cast<uint16_t>(SecurityMode::Product);

    EncodeResult result =
        encode_connect(buffer, sizeof(buffer), request, out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader) + sizeof(ConnectRequest));
}

/** @brief Test case: roundtrip connect. */
TEST(roundtrip_connect) {
    uint8_t buffer[2048];
    size_t encoded_size = 0;

    ConnectRequest original{};
    original.security_config.security_mode =
        static_cast<uint16_t>(SecurityMode::Debug);
    original.local_communication_version = 5;
    original.option_unknown = 0xAB;

    // Encode
    EncodeResult enc_result =
        encode_connect(buffer, sizeof(buffer), original, encoded_size);
    ASSERT_EQ(enc_result, EncodeResult::Success);

    // Decode
    LdnHeader header{};
    ConnectRequest decoded{};
    DecodeResult dec_result =
        decode_connect(buffer, encoded_size, header, decoded);

    ASSERT_EQ(dec_result, DecodeResult::Success);
    ASSERT_EQ(
        decoded.security_config.security_mode,
        original.security_config.security_mode);
    ASSERT_EQ(
        decoded.local_communication_version,
        original.local_communication_version);
    ASSERT_EQ(decoded.option_unknown, original.option_unknown);
}

/** @brief Test case: encode create access point packet. */
TEST(encode_create_access_point_packet) {
    uint8_t buffer[512];
    size_t out_size = 0;

    CreateAccessPointRequest request{};
    request.security_config.security_mode =
        static_cast<uint16_t>(SecurityMode::Product);
    request.network_config.intent_id.local_communication_id = 0xAABBCCDD;
    request.ryu_network_config.internal_proxy_port = 8080;

    uint8_t advertise_data[] = {0x01, 0x02, 0x03, 0x04, 0x05};

    EncodeResult result = encode_create_access_point(
        buffer,
        sizeof(buffer),
        request,
        advertise_data,
        sizeof(advertise_data),
        out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(
        out_size,
        sizeof(LdnHeader) + sizeof(CreateAccessPointRequest) +
            sizeof(advertise_data));

    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    ASSERT_EQ(header->type, static_cast<uint8_t>(PacketId::CreateAccessPoint));
}

/** @brief Test case: roundtrip create access point. */
TEST(roundtrip_create_access_point) {
    uint8_t buffer[512];
    size_t encoded_size = 0;

    CreateAccessPointRequest original{};
    original.security_config.security_mode =
        static_cast<uint16_t>(SecurityMode::Debug);
    original.ryu_network_config.internal_proxy_port = 12345;
    original.ryu_network_config.external_proxy_port = 54321;

    uint8_t original_data[] = {0xDE, 0xAD, 0xBE, 0xEF};

    // Encode
    EncodeResult enc_result = encode_create_access_point(
        buffer,
        sizeof(buffer),
        original,
        original_data,
        sizeof(original_data),
        encoded_size);
    ASSERT_EQ(enc_result, EncodeResult::Success);

    // Decode
    LdnHeader header{};
    CreateAccessPointRequest decoded{};
    size_t decoded_offset;
    size_t decoded_size;
    DecodeResult dec_result = decode_create_access_point(
        buffer, encoded_size, header, decoded, decoded_offset, decoded_size);

    ASSERT_EQ(dec_result, DecodeResult::Success);
    ASSERT_EQ(
        decoded.ryu_network_config.internal_proxy_port,
        original.ryu_network_config.internal_proxy_port);
    ASSERT_EQ(
        decoded.ryu_network_config.external_proxy_port,
        original.ryu_network_config.external_proxy_port);
    ASSERT_EQ(decoded_size, sizeof(original_data));
    ASSERT_TRUE(
        std::memcmp(buffer + decoded_offset, original_data, decoded_size) == 0);
}

/** @brief Test case: encode set accept policy packet. */
TEST(encode_set_accept_policy_packet) {
    uint8_t buffer[64];
    size_t out_size = 0;

    EncodeResult result = encode_set_accept_policy(
        buffer, sizeof(buffer), AcceptPolicy::AcceptAll, out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader) + sizeof(SetAcceptPolicyRequest));

    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    ASSERT_EQ(header->type, static_cast<uint8_t>(PacketId::SetAcceptPolicy));
}

/** @brief Test case: roundtrip set accept policy. */
TEST(roundtrip_set_accept_policy) {
    uint8_t buffer[64];
    size_t encoded_size = 0;

    // Encode
    EncodeResult enc_result = encode_set_accept_policy(
        buffer, sizeof(buffer), AcceptPolicy::BlackList, encoded_size);
    ASSERT_EQ(enc_result, EncodeResult::Success);

    // Decode
    LdnHeader header{};
    SetAcceptPolicyRequest decoded{};
    DecodeResult dec_result =
        decode_set_accept_policy(buffer, encoded_size, header, decoded);

    ASSERT_EQ(dec_result, DecodeResult::Success);
    ASSERT_EQ(
        decoded.accept_policy, static_cast<uint8_t>(AcceptPolicy::BlackList));
}

/** @brief Test case: encode set advertise data packet. */
TEST(encode_set_advertise_data_packet) {
    uint8_t buffer[512];
    size_t out_size = 0;

    uint8_t data[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

    EncodeResult result = encode_set_advertise_data(
        buffer, sizeof(buffer), data, sizeof(data), out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader) + sizeof(data));

    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    ASSERT_EQ(header->type, static_cast<uint8_t>(PacketId::SetAdvertiseData));
    ASSERT_EQ(header->data_size, static_cast<int32_t>(sizeof(data)));
}

/** @brief Test case: roundtrip set advertise data. */
TEST(roundtrip_set_advertise_data) {
    uint8_t buffer[512];
    size_t encoded_size = 0;

    uint8_t original_data[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    // Encode
    EncodeResult enc_result = encode_set_advertise_data(
        buffer,
        sizeof(buffer),
        original_data,
        sizeof(original_data),
        encoded_size);
    ASSERT_EQ(enc_result, EncodeResult::Success);

    // Decode
    LdnHeader header{};
    const uint8_t* decoded_data;
    size_t decoded_size;
    DecodeResult dec_result = decode_set_advertise_data(
        buffer, encoded_size, header, decoded_data, decoded_size);

    ASSERT_EQ(dec_result, DecodeResult::Success);
    ASSERT_EQ(decoded_size, sizeof(original_data));
    ASSERT_TRUE(std::memcmp(decoded_data, original_data, decoded_size) == 0);
}

/** @brief Test case: encode proxy data packet. */
TEST(encode_proxy_data_packet) {
    uint8_t buffer[512];
    size_t out_size = 0;

    ProxyInfo info{};
    info.source_ipv4 = 0x0A720001;  // 10.114.0.1
    info.source_port = 12345;
    info.dest_ipv4 = 0x0A720002;  // 10.114.0.2
    info.dest_port = 54321;
    info.protocol = ProtocolType::Udp;

    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};

    EncodeResult result = encode_proxy_data(
        buffer, sizeof(buffer), info, payload, sizeof(payload), out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(
        out_size,
        sizeof(LdnHeader) + sizeof(ProxyDataHeader) + sizeof(payload));

    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    ASSERT_EQ(header->type, static_cast<uint8_t>(PacketId::ProxyData));
}

/** @brief Test case: roundtrip proxy data. */
TEST(roundtrip_proxy_data) {
    uint8_t buffer[512];
    size_t encoded_size = 0;

    ProxyInfo original_info{};
    original_info.source_ipv4 = 0xC0A80101;  // 192.168.1.1
    original_info.source_port = 8888;
    original_info.dest_ipv4 = 0xC0A80102;  // 192.168.1.2
    original_info.dest_port = 9999;
    original_info.protocol = ProtocolType::Tcp;

    uint8_t original_payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};

    // Encode
    EncodeResult enc_result = encode_proxy_data(
        buffer,
        sizeof(buffer),
        original_info,
        original_payload,
        sizeof(original_payload),
        encoded_size);
    ASSERT_EQ(enc_result, EncodeResult::Success);

    // Decode
    LdnHeader header{};
    ProxyDataHeader proxy_header{};
    size_t decoded_offset;
    size_t decoded_size;
    DecodeResult dec_result = decode_proxy_data(
        buffer,
        encoded_size,
        header,
        proxy_header,
        decoded_offset,
        decoded_size);

    ASSERT_EQ(dec_result, DecodeResult::Success);
    ASSERT_EQ(proxy_header.info.source_ipv4, original_info.source_ipv4);
    ASSERT_EQ(proxy_header.info.source_port, original_info.source_port);
    ASSERT_EQ(proxy_header.info.dest_ipv4, original_info.dest_ipv4);
    ASSERT_EQ(proxy_header.info.dest_port, original_info.dest_port);
    ASSERT_EQ(
        static_cast<int>(proxy_header.info.protocol),
        static_cast<int>(original_info.protocol));
    ASSERT_EQ(proxy_header.data_length, sizeof(original_payload));
    ASSERT_EQ(decoded_size, sizeof(original_payload));
    ASSERT_TRUE(
        std::memcmp(buffer + decoded_offset, original_payload, decoded_size) ==
        0);
}

/** @brief Test case: decode proxy connect packet. */
TEST(decode_proxy_connect_packet) {
    uint8_t buffer[64];

    // Build packet manually
    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    header->magic = PROTOCOL_MAGIC;
    header->type = static_cast<uint8_t>(PacketId::ProxyConnect);
    header->version = PROTOCOL_VERSION;
    header->data_size = sizeof(ProxyConnectRequest);

    auto* req =
        reinterpret_cast<ProxyConnectRequest*>(buffer + sizeof(LdnHeader));
    req->info.source_ipv4 = 0x0A000001;
    req->info.dest_ipv4 = 0x0A000002;
    req->info.source_port = 1234;
    req->info.dest_port = 5678;

    // Decode
    LdnHeader decoded_header{};
    ProxyConnectRequest decoded_req{};
    DecodeResult result = decode_proxy_connect(
        buffer,
        sizeof(LdnHeader) + sizeof(ProxyConnectRequest),
        decoded_header,
        decoded_req);

    ASSERT_EQ(result, DecodeResult::Success);
    ASSERT_EQ(decoded_req.info.source_ipv4, 0x0A000001U);
    ASSERT_EQ(decoded_req.info.dest_ipv4, 0x0A000002U);
    ASSERT_EQ(decoded_req.info.source_port, 1234U);
    ASSERT_EQ(decoded_req.info.dest_port, 5678U);
}

/** @brief Test case: decode proxy connect reply packet. */
TEST(decode_proxy_connect_reply_packet) {
    uint8_t buffer[64];

    // Build packet manually
    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    header->magic = PROTOCOL_MAGIC;
    header->type = static_cast<uint8_t>(PacketId::ProxyConnectReply);
    header->version = PROTOCOL_VERSION;
    header->data_size = sizeof(ProxyConnectResponse);

    auto* resp =
        reinterpret_cast<ProxyConnectResponse*>(buffer + sizeof(LdnHeader));
    resp->info.source_ipv4 = 0x0A000002;
    resp->info.dest_ipv4 = 0x0A000001;
    resp->info.source_port = 5678;
    resp->info.dest_port = 1234;

    // Decode
    LdnHeader decoded_header{};
    ProxyConnectResponse decoded_resp{};
    DecodeResult result = decode_proxy_connect_reply(
        buffer,
        sizeof(LdnHeader) + sizeof(ProxyConnectResponse),
        decoded_header,
        decoded_resp);

    ASSERT_EQ(result, DecodeResult::Success);
    ASSERT_EQ(decoded_resp.info.source_ipv4, 0x0A000002U);
    ASSERT_EQ(decoded_resp.info.dest_ipv4, 0x0A000001U);
}

/** @brief Test case: decode proxy disconnect packet. */
TEST(decode_proxy_disconnect_packet) {
    uint8_t buffer[64];

    // Build packet manually
    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    header->magic = PROTOCOL_MAGIC;
    header->type = static_cast<uint8_t>(PacketId::ProxyDisconnect);
    header->version = PROTOCOL_VERSION;
    header->data_size = sizeof(ProxyDisconnectMessage);

    auto* msg =
        reinterpret_cast<ProxyDisconnectMessage*>(buffer + sizeof(LdnHeader));
    msg->info.source_ipv4 = 0x0A000001;
    msg->info.dest_ipv4 = 0x0A000002;
    msg->disconnect_reason = static_cast<int32_t>(DisconnectReason::User);

    // Decode
    LdnHeader decoded_header{};
    ProxyDisconnectMessage decoded_msg{};
    DecodeResult result = decode_proxy_disconnect(
        buffer,
        sizeof(LdnHeader) + sizeof(ProxyDisconnectMessage),
        decoded_header,
        decoded_msg);

    ASSERT_EQ(result, DecodeResult::Success);
    ASSERT_EQ(decoded_msg.info.source_ipv4, 0x0A000001U);
    ASSERT_EQ(decoded_msg.info.dest_ipv4, 0x0A000002U);
    ASSERT_EQ(
        decoded_msg.disconnect_reason,
        static_cast<int>(DisconnectReason::User));
}

/** @brief Test case: decode reject packet. */
TEST(decode_reject_packet) {
    uint8_t buffer[64];

    // Build packet manually
    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    header->magic = PROTOCOL_MAGIC;
    header->type = static_cast<uint8_t>(PacketId::Reject);
    header->version = PROTOCOL_VERSION;
    header->data_size = sizeof(RejectRequest);

    auto* req = reinterpret_cast<RejectRequest*>(buffer + sizeof(LdnHeader));
    req->node_id = 3;
    req->disconnect_reason = static_cast<uint32_t>(DisconnectReason::Rejected);

    // Decode
    LdnHeader decoded_header{};
    RejectRequest decoded_req{};
    DecodeResult result = decode_reject(
        buffer,
        sizeof(LdnHeader) + sizeof(RejectRequest),
        decoded_header,
        decoded_req);

    ASSERT_EQ(result, DecodeResult::Success);
    ASSERT_EQ(decoded_req.node_id, 3U);
    ASSERT_EQ(
        static_cast<int>(decoded_req.disconnect_reason),
        static_cast<int>(DisconnectReason::Rejected));
}

/** @brief Test case: encode network info packet. */
TEST(encode_network_info_packet) {
    uint8_t buffer[2048];
    size_t out_size = 0;

    NetworkInfo info{};
    info.ldn.node_count = 4;
    info.ldn.node_count_max = 8;
    info.ldn.advertise_data_size = 100;
    info.common.mac_address.data[0] = 0xAA;
    info.common.channel = 6;

    EncodeResult result = encode_network_info(
        buffer, sizeof(buffer), PacketId::Connected, info, out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader) + sizeof(NetworkInfo));

    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    ASSERT_EQ(header->type, static_cast<uint8_t>(PacketId::Connected));
}

/** @brief Test case: roundtrip network info. */
TEST(roundtrip_network_info) {
    uint8_t buffer[2048];
    size_t encoded_size = 0;

    NetworkInfo original{};
    original.ldn.node_count = 3;
    original.ldn.node_count_max = 8;
    original.ldn.advertise_data_size = 50;
    original.common.mac_address.data[0] = 0xBB;
    original.common.mac_address.data[5] = 0xCC;
    original.common.channel = 11;

    // Encode
    EncodeResult enc_result = encode_network_info(
        buffer, sizeof(buffer), PacketId::ScanReply, original, encoded_size);
    ASSERT_EQ(enc_result, EncodeResult::Success);

    // Decode
    LdnHeader header{};
    NetworkInfo decoded{};
    DecodeResult dec_result =
        decode_network_info(buffer, encoded_size, header, decoded);

    ASSERT_EQ(dec_result, DecodeResult::Success);
    ASSERT_EQ(decoded.ldn.node_count, original.ldn.node_count);
    ASSERT_EQ(decoded.ldn.node_count_max, original.ldn.node_count_max);
    ASSERT_EQ(
        decoded.ldn.advertise_data_size, original.ldn.advertise_data_size);
    ASSERT_EQ(
        decoded.common.mac_address.data[0],
        original.common.mac_address.data[0]);
    ASSERT_EQ(
        decoded.common.mac_address.data[5],
        original.common.mac_address.data[5]);
    ASSERT_EQ(decoded.common.channel, original.common.channel);
}

/** @brief Test case: encode scan reply end packet. */
TEST(encode_scan_reply_end_packet) {
    uint8_t buffer[64];
    size_t out_size = 0;

    EncodeResult result =
        encode_scan_reply_end(buffer, sizeof(buffer), out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader));

    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    ASSERT_EQ(header->type, static_cast<uint8_t>(PacketId::ScanReplyEnd));
    ASSERT_EQ(header->data_size, 0);
}

/** @brief Test case: encode reject reply packet. */
TEST(encode_reject_reply_packet) {
    uint8_t buffer[64];
    size_t out_size = 0;

    EncodeResult result = encode_reject_reply(buffer, sizeof(buffer), out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader));

    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    ASSERT_EQ(header->type, static_cast<uint8_t>(PacketId::RejectReply));
    ASSERT_EQ(header->data_size, 0);
}

/** @brief Test case: encode raw packet. */
TEST(encode_raw_packet) {
    uint8_t buffer[128];
    size_t out_size = 0;

    uint8_t raw_data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

    EncodeResult result = encode_raw(
        buffer,
        sizeof(buffer),
        PacketId::SetAdvertiseData,
        raw_data,
        sizeof(raw_data),
        out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader) + sizeof(raw_data));

    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    ASSERT_EQ(header->data_size, static_cast<int32_t>(sizeof(raw_data)));

    // Verify raw data was copied
    ASSERT_TRUE(
        std::memcmp(buffer + sizeof(LdnHeader), raw_data, sizeof(raw_data)) ==
        0);
}

/** @brief Test case: roundtrip raw. */
TEST(roundtrip_raw) {
    uint8_t buffer[128];
    size_t encoded_size = 0;

    uint8_t original_data[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

    // Encode
    EncodeResult enc_result = encode_raw(
        buffer,
        sizeof(buffer),
        PacketId::SetAdvertiseData,
        original_data,
        sizeof(original_data),
        encoded_size);
    ASSERT_EQ(enc_result, EncodeResult::Success);

    // Decode
    LdnHeader header{};
    const uint8_t* decoded_data;
    size_t decoded_size;
    DecodeResult dec_result =
        decode_raw(buffer, encoded_size, header, decoded_data, decoded_size);

    ASSERT_EQ(dec_result, DecodeResult::Success);
    ASSERT_EQ(decoded_size, sizeof(original_data));
    ASSERT_TRUE(std::memcmp(decoded_data, original_data, decoded_size) == 0);
}

/** @brief Test case: encode header function. */
TEST(encode_header_function) {
    uint8_t buffer[32];

    size_t written = encode_header(buffer, PacketId::Ping, 100);

    ASSERT_EQ(written, sizeof(LdnHeader));

    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    ASSERT_EQ(header->magic, PROTOCOL_MAGIC);
    ASSERT_EQ(header->type, static_cast<uint8_t>(PacketId::Ping));
    ASSERT_EQ(header->version, PROTOCOL_VERSION);
    ASSERT_EQ(header->data_size, 100);
}

/** @brief Test case: get packet size functions. */
TEST(get_packet_size_functions) {
    ASSERT_EQ(get_packet_size(0), sizeof(LdnHeader));
    ASSERT_EQ(get_packet_size(100), sizeof(LdnHeader) + 100);
    ASSERT_EQ(
        get_packet_size<PingMessage>(),
        sizeof(LdnHeader) + sizeof(PingMessage));
    ASSERT_EQ(
        get_packet_size<NetworkInfo>(),
        sizeof(LdnHeader) + sizeof(NetworkInfo));
}

/** @brief Test case: has header function. */
TEST(has_header_function) {
    ASSERT_FALSE(has_header(0));
    ASSERT_FALSE(has_header(5));
    ASSERT_FALSE(has_header(sizeof(LdnHeader) - 1));
    ASSERT_TRUE(has_header(sizeof(LdnHeader)));
    ASSERT_TRUE(has_header(sizeof(LdnHeader) + 100));
}

/** @brief Test case: get packet type function. */
TEST(get_packet_type_function) {
    uint8_t buffer[32];
    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    header->magic = PROTOCOL_MAGIC;
    header->type = static_cast<uint8_t>(PacketId::ProxyData);
    header->version = PROTOCOL_VERSION;
    header->data_size = 0;

    PacketId type = get_packet_type(buffer);
    ASSERT_EQ(type, PacketId::ProxyData);
}

/** @brief Test case: get payload size function. */
TEST(get_payload_size_function) {
    uint8_t buffer[32];
    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    header->magic = PROTOCOL_MAGIC;
    header->type = static_cast<uint8_t>(PacketId::Ping);
    header->version = PROTOCOL_VERSION;
    header->data_size = 42;

    int32_t size = get_payload_size(buffer);
    ASSERT_EQ(size, 42);
}

/** @brief Test case: get payload ptr function. */
TEST(get_payload_ptr_function) {
    uint8_t buffer[64];
    std::memset(buffer, 0, sizeof(buffer));
    buffer[sizeof(LdnHeader)] = 0xDE;
    buffer[sizeof(LdnHeader) + 1] = 0xAD;

    const uint8_t* payload = get_payload_ptr(buffer);

    ASSERT_TRUE(payload == buffer + sizeof(LdnHeader));
    ASSERT_EQ(payload[0], 0xDE);
    ASSERT_EQ(payload[1], 0xAD);
}

/** @brief Test case: decode raw empty payload. */
TEST(decode_raw_empty_payload) {
    uint8_t buffer[32];

    // Build packet with no payload
    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    header->magic = PROTOCOL_MAGIC;
    header->type = static_cast<uint8_t>(PacketId::ScanReplyEnd);
    header->version = PROTOCOL_VERSION;
    header->data_size = 0;

    LdnHeader decoded_header{};
    const uint8_t* data;
    size_t data_size;
    DecodeResult result =
        decode_raw(buffer, sizeof(LdnHeader), decoded_header, data, data_size);

    ASSERT_EQ(result, DecodeResult::Success);
    ASSERT_EQ(data_size, 0U);
    ASSERT_TRUE(data == nullptr);
}

/** @brief Test case: encode with data template. */
TEST(encode_with_data_template) {
    uint8_t buffer[256];
    size_t out_size = 0;

    ProxyInfo info{};
    info.source_ipv4 = 0x0A000001;
    info.dest_ipv4 = 0x0A000002;

    uint8_t extra[] = {0x11, 0x22, 0x33};

    EncodeResult result = encode_with_data(
        buffer,
        sizeof(buffer),
        PacketId::ProxyConnect,
        info,
        extra,
        sizeof(extra),
        out_size);

    ASSERT_EQ(result, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader) + sizeof(ProxyInfo) + sizeof(extra));

    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    ASSERT_EQ(
        header->data_size,
        static_cast<int32_t>(sizeof(ProxyInfo) + sizeof(extra)));
}

/** @brief Test case: decode with data template. */
TEST(decode_with_data_template) {
    uint8_t buffer[256];
    size_t encoded_size = 0;

    ProxyInfo original_info{};
    original_info.source_ipv4 = 0xC0A80001;
    original_info.dest_ipv4 = 0xC0A80002;

    uint8_t original_extra[] = {0xAA, 0xBB, 0xCC, 0xDD};

    // Encode
    EncodeResult enc_result = encode_with_data(
        buffer,
        sizeof(buffer),
        PacketId::ProxyConnect,
        original_info,
        original_extra,
        sizeof(original_extra),
        encoded_size);
    ASSERT_EQ(enc_result, EncodeResult::Success);

    // Decode
    LdnHeader header{};
    ProxyInfo decoded_info{};
    size_t decoded_extra_offset;
    size_t decoded_extra_size;
    DecodeResult dec_result = decode_with_data(
        buffer,
        encoded_size,
        header,
        decoded_info,
        decoded_extra_offset,
        decoded_extra_size);

    ASSERT_EQ(dec_result, DecodeResult::Success);
    ASSERT_EQ(decoded_info.source_ipv4, original_info.source_ipv4);
    ASSERT_EQ(decoded_info.dest_ipv4, original_info.dest_ipv4);
    ASSERT_EQ(decoded_extra_size, sizeof(original_extra));
    ASSERT_TRUE(
        std::memcmp(
            buffer + decoded_extra_offset,
            original_extra,
            decoded_extra_size) == 0);
}

/** @brief Test case: all packet id to string. */
TEST(all_packet_id_to_string) {
    // Test all PacketId values have valid strings
    ASSERT_TRUE(
        std::strcmp(packet_id_to_string(PacketId::Initialize), "Initialize") ==
        0);
    ASSERT_TRUE(
        std::strcmp(packet_id_to_string(PacketId::Passphrase), "Passphrase") ==
        0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::CreateAccessPoint),
            "CreateAccessPoint") == 0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::CreateAccessPointPrivate),
            "CreateAccessPointPrivate") == 0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::ExternalProxy), "ExternalProxy") ==
        0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::ExternalProxyToken),
            "ExternalProxyToken") == 0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::ExternalProxyState),
            "ExternalProxyState") == 0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::SyncNetwork), "SyncNetwork") == 0);
    ASSERT_TRUE(
        std::strcmp(packet_id_to_string(PacketId::Reject), "Reject") == 0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::RejectReply), "RejectReply") == 0);
    ASSERT_TRUE(std::strcmp(packet_id_to_string(PacketId::Scan), "Scan") == 0);
    ASSERT_TRUE(
        std::strcmp(packet_id_to_string(PacketId::ScanReply), "ScanReply") ==
        0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::ScanReplyEnd), "ScanReplyEnd") == 0);
    ASSERT_TRUE(
        std::strcmp(packet_id_to_string(PacketId::Connect), "Connect") == 0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::ConnectPrivate), "ConnectPrivate") ==
        0);
    ASSERT_TRUE(
        std::strcmp(packet_id_to_string(PacketId::Connected), "Connected") ==
        0);
    ASSERT_TRUE(
        std::strcmp(packet_id_to_string(PacketId::Disconnect), "Disconnect") ==
        0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::ProxyConfig), "ProxyConfig") == 0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::ProxyConnect), "ProxyConnect") == 0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::ProxyConnectReply),
            "ProxyConnectReply") == 0);
    ASSERT_TRUE(
        std::strcmp(packet_id_to_string(PacketId::ProxyData), "ProxyData") ==
        0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::ProxyDisconnect),
            "ProxyDisconnect") == 0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::SetAcceptPolicy),
            "SetAcceptPolicy") == 0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::SetAdvertiseData),
            "SetAdvertiseData") == 0);
    ASSERT_TRUE(std::strcmp(packet_id_to_string(PacketId::Ping), "Ping") == 0);
    ASSERT_TRUE(
        std::strcmp(
            packet_id_to_string(PacketId::NetworkError), "NetworkError") == 0);
}

/** @brief Test case: all decode result to string. */
TEST(all_decode_result_to_string) {
    ASSERT_TRUE(
        std::strcmp(
            decode_result_to_string(DecodeResult::Success), "Success") == 0);
    ASSERT_TRUE(
        std::strcmp(
            decode_result_to_string(DecodeResult::BufferTooSmall),
            "BufferTooSmall") == 0);
    ASSERT_TRUE(
        std::strcmp(
            decode_result_to_string(DecodeResult::InvalidMagic),
            "InvalidMagic") == 0);
    ASSERT_TRUE(
        std::strcmp(
            decode_result_to_string(DecodeResult::InvalidVersion),
            "InvalidVersion") == 0);
    ASSERT_TRUE(
        std::strcmp(
            decode_result_to_string(DecodeResult::PacketTooLarge),
            "PacketTooLarge") == 0);
    ASSERT_TRUE(
        std::strcmp(
            decode_result_to_string(DecodeResult::IncompletePacket),
            "IncompletePacket") == 0);
}

/** @brief Test case: all encode result to string. */
TEST(all_encode_result_to_string) {
    ASSERT_TRUE(
        std::strcmp(
            encode_result_to_string(EncodeResult::Success), "Success") == 0);
    ASSERT_TRUE(
        std::strcmp(
            encode_result_to_string(EncodeResult::BufferTooSmall),
            "BufferTooSmall") == 0);
    ASSERT_TRUE(
        std::strcmp(
            encode_result_to_string(EncodeResult::InvalidPacketId),
            "InvalidPacketId") == 0);
}

// ============================================================================
// Additional Structure Size Tests
// ============================================================================

/** @brief Test case: additional structure sizes. */
TEST(additional_structure_sizes) {
    // Verify all structure sizes match RyuLDN protocol
    ASSERT_EQ(
        sizeof(SetAcceptPolicyRequest), 0x1);  // 1 byte per Ryujinx protocol
    ASSERT_EQ(sizeof(ExternalProxyConfig), 0x26);
    ASSERT_EQ(sizeof(ExternalProxyToken), 0x28);
    ASSERT_EQ(sizeof(ExternalProxyConnectionState), 0x08);
    ASSERT_EQ(sizeof(ProxyConnectRequest), 0x10);
    ASSERT_EQ(sizeof(ProxyConnectResponse), 0x10);
}

// ============================================================================
// Error Handling Tests
// ============================================================================

/** @brief Test case: encode buffer too small for payload. */
TEST(encode_buffer_too_small_for_payload) {
    uint8_t buffer[16];  // Too small for NetworkInfo
    size_t out_size = 0;

    NetworkInfo info{};
    EncodeResult result = encode_network_info(
        buffer, sizeof(buffer), PacketId::Connected, info, out_size);

    ASSERT_EQ(result, EncodeResult::BufferTooSmall);
    ASSERT_EQ(out_size, 0U);
}

/** @brief Test case: decode incomplete packet data. */
TEST(decode_incomplete_packet_data) {
    uint8_t buffer[32];

    // Build header saying we have 100 bytes of payload
    auto* header = reinterpret_cast<LdnHeader*>(buffer);
    header->magic = PROTOCOL_MAGIC;
    header->type = static_cast<uint8_t>(PacketId::ProxyData);
    header->version = PROTOCOL_VERSION;
    header->data_size = 100;  // Says 100 bytes

    // But buffer only has 32 bytes total
    LdnHeader decoded_header{};
    const uint8_t* data;
    size_t data_size;
    DecodeResult result =
        decode_raw(buffer, sizeof(buffer), decoded_header, data, data_size);

    ASSERT_EQ(result, DecodeResult::IncompletePacket);
}

/** @brief Test case: decode with data no extra. */
TEST(decode_with_data_no_extra) {
    uint8_t buffer[128];
    size_t encoded_size = 0;

    ProxyInfo info{};
    info.source_ipv4 = 0x01020304;

    // Encode with NO extra data
    EncodeResult enc_result = encode_with_data(
        buffer,
        sizeof(buffer),
        PacketId::ProxyConnect,
        info,
        nullptr,
        0,
        encoded_size);
    ASSERT_EQ(enc_result, EncodeResult::Success);

    // Decode
    LdnHeader header{};
    ProxyInfo decoded_info{};
    size_t extra_offset;
    size_t extra_size;
    DecodeResult dec_result = decode_with_data(
        buffer, encoded_size, header, decoded_info, extra_offset, extra_size);

    ASSERT_EQ(dec_result, DecodeResult::Success);
    ASSERT_EQ(extra_offset, 0U);
    ASSERT_EQ(extra_size, 0U);
}

// ============================================================================
// PacketBuffer Additional Coverage
// ============================================================================

/** @brief Test case: buffer initial state. */
TEST(buffer_initial_state) {
    PacketBuffer<256> buf;
    ASSERT_TRUE(buf.empty());
    ASSERT_EQ(buf.size(), 0U);
    ASSERT_EQ(buf.available(), 256U);
}

/** @brief Test case: buffer size and available after append. */
TEST(buffer_size_and_available_after_append) {
    PacketBuffer<256> buf;
    uint8_t data[10] = {};
    buf.append(data, 10);
    ASSERT_EQ(buf.size(), 10U);
    ASSERT_EQ(buf.available(), 246U);
    ASSERT_TRUE(!buf.empty());
}

/** @brief Test case: buffer data ptr. */
TEST(buffer_data_ptr) {
    PacketBuffer<256> buf;
    ASSERT_TRUE(buf.data() != nullptr);
    uint8_t data[5] = {1, 2, 3, 4, 5};
    buf.append(data, 5);
    ASSERT_EQ(buf.data()[0], 1);
    ASSERT_EQ(buf.data()[4], 5);
}

/** @brief Test case: buffer append zero size. */
TEST(buffer_append_zero_size) {
    PacketBuffer<256> buf;
    BufferResult r = buf.append(nullptr, 0);
    ASSERT_EQ(r, BufferResult::Success);
    ASSERT_EQ(buf.size(), 0U);
}

/** @brief Test case: buffer append overflow. */
TEST(buffer_append_overflow) {
    PacketBuffer<16> buf;
    uint8_t data[20] = {};
    BufferResult r = buf.append(data, 20);
    ASSERT_EQ(r, BufferResult::BufferFull);
}

/** @brief Test case: buffer has complete packet too small. */
TEST(buffer_has_complete_packet_too_small) {
    PacketBuffer<256> buf;
    uint8_t hdr[4] = {};  // Only 4 bytes, need 12 for header
    buf.append(hdr, 4);
    ASSERT_TRUE(!buf.has_complete_packet());
}

/** @brief Test case: buffer peek packet info incomplete. */
TEST(buffer_peek_packet_info_incomplete) {
    PacketBuffer<256> buf;
    size_t pkt_size = 0;
    BufferResult r = buf.peek_packet_info(pkt_size);
    ASSERT_EQ(r, BufferResult::NoCompletePacket);
    ASSERT_EQ(pkt_size, 0U);
}

/** @brief Test case: buffer peek packet info invalid magic. */
TEST(buffer_peek_packet_info_invalid_magic) {
    PacketBuffer<256> buf;
    uint8_t garbage[12] = {};
    buf.append(garbage, 12);
    size_t pkt_size = 0;
    BufferResult r = buf.peek_packet_info(pkt_size);
    ASSERT_EQ(r, BufferResult::InvalidPacket);
}

/** @brief Test case: buffer peek packet info valid. */
TEST(buffer_peek_packet_info_valid) {
    PacketBuffer<256> buf;
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.type = static_cast<uint8_t>(PacketId::Ping);
    hdr.version = PROTOCOL_VERSION;
    hdr.data_size = sizeof(PingMessage);
    PingMessage ping{};
    buf.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));
    buf.append(reinterpret_cast<uint8_t*>(&ping), sizeof(ping));
    size_t pkt_size = 0;
    BufferResult r = buf.peek_packet_info(pkt_size);
    ASSERT_EQ(r, BufferResult::Success);
    ASSERT_EQ(pkt_size, sizeof(LdnHeader) + sizeof(PingMessage));
}

/** @brief Test case: buffer peek packet nullptr. */
TEST(buffer_peek_packet_nullptr) {
    PacketBuffer<256> buf;
    size_t pkt_size = 0;
    const uint8_t* p = buf.peek_packet(pkt_size);
    ASSERT_TRUE(p == nullptr);
    ASSERT_EQ(pkt_size, 0U);
}

/** @brief Test case: buffer peek packet valid. */
TEST(buffer_peek_packet_valid) {
    PacketBuffer<256> buf;
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.type = static_cast<uint8_t>(PacketId::Ping);
    hdr.version = PROTOCOL_VERSION;
    hdr.data_size = 0;
    buf.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));
    size_t pkt_size = 0;
    const uint8_t* p = buf.peek_packet(pkt_size);
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(pkt_size, sizeof(LdnHeader));
}

/** @brief Test case: buffer peek packet type. */
TEST(buffer_peek_packet_type) {
    PacketBuffer<256> buf;
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.type = static_cast<uint8_t>(PacketId::Scan);
    hdr.version = PROTOCOL_VERSION;
    hdr.data_size = 0;
    buf.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));
    PacketId t = buf.peek_packet_type();
    ASSERT_EQ(t, PacketId::Scan);
}

/** @brief Test case: buffer peek packet type empty. */
TEST(buffer_peek_packet_type_empty) {
    PacketBuffer<256> buf;
    PacketId t = buf.peek_packet_type();
    ASSERT_EQ(t, PacketId::Initialize);
}

/** @brief Test case: buffer consume zero. */
TEST(buffer_consume_zero) {
    PacketBuffer<256> buf;
    uint8_t data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    buf.append(data, 10);
    buf.consume(0);
    ASSERT_EQ(buf.size(), 10U);
}

/** @brief Test case: buffer consume all. */
TEST(buffer_consume_all) {
    PacketBuffer<256> buf;
    uint8_t data[10] = {};
    buf.append(data, 10);
    buf.consume(10);
    ASSERT_EQ(buf.size(), 0U);
    ASSERT_TRUE(buf.empty());
}

/** @brief Test case: buffer consume partial shift. */
TEST(buffer_consume_partial_shift) {
    PacketBuffer<256> buf;
    uint8_t data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    buf.append(data, 10);
    buf.consume(3);
    ASSERT_EQ(buf.size(), 7U);
    ASSERT_EQ(buf.data()[0], 4);
    ASSERT_EQ(buf.data()[6], 10);
}

/** @brief Test case: buffer extract packet success. */
TEST(buffer_extract_packet_success) {
    PacketBuffer<256> buf;
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.type = static_cast<uint8_t>(PacketId::Ping);
    hdr.version = PROTOCOL_VERSION;
    hdr.data_size = 0;
    buf.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));

    uint8_t out[64];
    size_t pkt_size = 0;
    BufferResult r = buf.extract_packet(out, sizeof(out), pkt_size);
    ASSERT_EQ(r, BufferResult::Success);
    ASSERT_EQ(pkt_size, sizeof(LdnHeader));
    ASSERT_TRUE(buf.empty());
}

/** @brief Test case: buffer extract packet too small. */
TEST(buffer_extract_packet_too_small) {
    PacketBuffer<256> buf;
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.type = static_cast<uint8_t>(PacketId::Ping);
    hdr.version = PROTOCOL_VERSION;
    hdr.data_size = 0;
    buf.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));

    uint8_t out[4];  // too small
    size_t pkt_size = 0;
    BufferResult r = buf.extract_packet(out, sizeof(out), pkt_size);
    ASSERT_EQ(r, BufferResult::BufferFull);
}

/** @brief Test case: buffer discard until valid. */
TEST(buffer_discard_until_valid) {
    PacketBuffer<256> buf;
    // 3 garbage bytes + valid header
    uint8_t garbage[3] = {0xFF, 0xFF, 0xFF};
    buf.append(garbage, 3);

    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.type = static_cast<uint8_t>(PacketId::Ping);
    hdr.version = PROTOCOL_VERSION;
    hdr.data_size = 0;
    buf.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));

    size_t discarded = buf.discard_until_valid();
    ASSERT_EQ(discarded, 3U);
    ASSERT_TRUE(buf.has_complete_packet());
}

/** @brief Test case: buffer discard until valid empty. */
TEST(buffer_discard_until_valid_empty) {
    PacketBuffer<256> buf;
    size_t discarded = buf.discard_until_valid();
    ASSERT_EQ(discarded, 0U);
}

/** @brief Test case: buffer write ptr and advance. */
TEST(buffer_write_ptr_and_advance) {
    PacketBuffer<256> buf;
    uint8_t* wptr = buf.write_ptr();
    ASSERT_TRUE(wptr != nullptr);
    // Write 4 bytes directly
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.type = static_cast<uint8_t>(PacketId::Ping);
    hdr.version = PROTOCOL_VERSION;
    hdr.data_size = 0;
    std::memcpy(wptr, &hdr, sizeof(hdr));
    buf.advance_write(sizeof(hdr));
    ASSERT_EQ(buf.size(), sizeof(hdr));
    ASSERT_TRUE(buf.has_complete_packet());
}

/** @brief Test case: buffer advance write overflow. */
TEST(buffer_advance_write_overflow) {
    PacketBuffer<16> buf;
    buf.advance_write(20);      // exceeds capacity
    ASSERT_EQ(buf.size(), 0U);  // should not advance
}

/** @brief Test case: buffer capacity. */
TEST(buffer_capacity) {
    ASSERT_EQ(PacketBuffer<256>::capacity(), 256U);
    ASSERT_EQ(PacketBuffer<512>::capacity(), 512U);
}

/** @brief Test case: buffer packet too large. */
TEST(buffer_packet_too_large) {
    PacketBuffer<256> buf;
    // Create a header with data_size that exceeds buffer
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.type = static_cast<uint8_t>(PacketId::Ping);
    hdr.version = PROTOCOL_VERSION;
    hdr.data_size = 500000;  // way too large
    buf.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));
    size_t pkt_size = 0;
    BufferResult r = buf.peek_packet_info(pkt_size);
    ASSERT_EQ(r, BufferResult::PacketTooLarge);
}

// ============================================================================
// Protocol Encode/Decode Additional Coverage
// ============================================================================

/** @brief Test case: encode header only packet types. */
TEST(encode_header_only_packet_types) {
    uint8_t buffer[64];
    size_t out_size = 0;
    // ScanReplyEnd (no payload)
    EncodeResult r =
        encode(buffer, sizeof(buffer), PacketId::ScanReplyEnd, out_size);
    ASSERT_EQ(r, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader));

    // RejectReply (no payload)
    r = encode(buffer, sizeof(buffer), PacketId::RejectReply, out_size);
    ASSERT_EQ(r, EncodeResult::Success);

    // NetworkError (no payload)
    r = encode(buffer, sizeof(buffer), PacketId::NetworkError, out_size);
    ASSERT_EQ(r, EncodeResult::Success);
}

/** @brief Test case: encode header too small. */
TEST(encode_header_too_small) {
    uint8_t buffer[4];
    size_t out_size = 99;
    EncodeResult r = encode(buffer, sizeof(buffer), PacketId::Ping, out_size);
    ASSERT_EQ(r, EncodeResult::BufferTooSmall);
    ASSERT_EQ(out_size, 0U);
}

/** @brief Test case: encode with data buffer too small. */
TEST(encode_with_data_buffer_too_small) {
    uint8_t buffer[8];
    PingMessage msg{};
    size_t out_size = 99;
    EncodeResult r = encode_with_data(
        buffer, sizeof(buffer), PacketId::Ping, msg, nullptr, 0, out_size);
    ASSERT_EQ(r, EncodeResult::BufferTooSmall);
    ASSERT_EQ(out_size, 0U);
}

/** @brief Test case: encode with data with extra. */
TEST(encode_with_data_with_extra) {
    uint8_t buffer[256];
    PingMessage msg{};
    uint8_t extra[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    size_t out_size = 0;
    EncodeResult r = encode_with_data(
        buffer,
        sizeof(buffer),
        PacketId::Ping,
        msg,
        extra,
        sizeof(extra),
        out_size);
    ASSERT_EQ(r, EncodeResult::Success);
    ASSERT_EQ(
        out_size, sizeof(LdnHeader) + sizeof(PingMessage) + sizeof(extra));
}

/** @brief Test case: encode with data null extra. */
TEST(encode_with_data_null_extra) {
    uint8_t buffer[256];
    PingMessage msg{};
    size_t out_size = 0;
    EncodeResult r = encode_with_data(
        buffer, sizeof(buffer), PacketId::Ping, msg, nullptr, 0, out_size);
    ASSERT_EQ(r, EncodeResult::Success);
}

/** @brief Test case: encode raw null data zero size. */
TEST(encode_raw_null_data_zero_size) {
    uint8_t buffer[64];
    size_t out_size = 0;
    EncodeResult r = encode_raw(
        buffer,
        sizeof(buffer),
        PacketId::SetAdvertiseData,
        nullptr,
        0,
        out_size);
    ASSERT_EQ(r, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader));
}

/** @brief Test case: encode raw buffer too small. */
TEST(encode_raw_buffer_too_small) {
    uint8_t buffer[4];
    uint8_t data[10] = {};
    size_t out_size = 99;
    EncodeResult r = encode_raw(
        buffer,
        sizeof(buffer),
        PacketId::ProxyData,
        data,
        sizeof(data),
        out_size);
    ASSERT_EQ(r, EncodeResult::BufferTooSmall);
    ASSERT_EQ(out_size, 0U);
}

/** @brief Test case: encode raw with data. */
TEST(encode_raw_with_data) {
    uint8_t buffer[256];
    uint8_t data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    size_t out_size = 0;
    EncodeResult r = encode_raw(
        buffer,
        sizeof(buffer),
        PacketId::ProxyData,
        data,
        sizeof(data),
        out_size);
    ASSERT_EQ(r, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader) + sizeof(data));

    // Verify data
    ASSERT_EQ(std::memcmp(buffer + sizeof(LdnHeader), data, sizeof(data)), 0);
}

/** @brief Test case: encode convenience functions. */
TEST(encode_convenience_functions) {
    uint8_t buffer[2048];
    size_t out_size = 0;
    SessionId id{};
    MacAddress mac{};
    EncodeResult r;

    r = encode_initialize(buffer, sizeof(buffer), id, mac, out_size);
    ASSERT_EQ(r, EncodeResult::Success);
    ASSERT_EQ(out_size, sizeof(LdnHeader) + sizeof(InitializeMessage));

    r = encode_passphrase(
        buffer,
        sizeof(buffer),
        reinterpret_cast<const uint8_t*>("test"),
        4,
        out_size);
    ASSERT_EQ(r, EncodeResult::Success);

    r = encode_passphrase(buffer, sizeof(buffer), nullptr, 0, out_size);
    ASSERT_EQ(r, EncodeResult::Success);

    r = encode_ping(buffer, sizeof(buffer), 1, 42, out_size);
    ASSERT_EQ(r, EncodeResult::Success);

    r = encode_disconnect(buffer, sizeof(buffer), 0xABCD1234, out_size);
    ASSERT_EQ(r, EncodeResult::Success);

    ScanFilterFull filter{};
    r = encode_scan(buffer, sizeof(buffer), filter, out_size);
    ASSERT_EQ(r, EncodeResult::Success);

    ConnectRequest conn_req2{};
    r = encode_connect(buffer, sizeof(buffer), conn_req2, out_size);
    ASSERT_EQ(r, EncodeResult::Success);

    CreateAccessPointRequest cap_req{};
    uint8_t adv[16] = {};
    r = encode_create_access_point(
        buffer, sizeof(buffer), cap_req, adv, sizeof(adv), out_size);
    ASSERT_EQ(r, EncodeResult::Success);

    r = encode_create_access_point(
        buffer, sizeof(buffer), cap_req, nullptr, 0, out_size);
    ASSERT_EQ(r, EncodeResult::Success);

    r = encode_set_accept_policy(
        buffer, sizeof(buffer), AcceptPolicy::RejectAll, out_size);
    ASSERT_EQ(r, EncodeResult::Success);

    r = encode_set_advertise_data(
        buffer, sizeof(buffer), adv, sizeof(adv), out_size);
    ASSERT_EQ(r, EncodeResult::Success);

    ProxyInfo info{};
    r = encode_proxy_data(
        buffer, sizeof(buffer), info, adv, sizeof(adv), out_size);
    ASSERT_EQ(r, EncodeResult::Success);
}

/** @brief Test case: decode header buffer too small. */
TEST(decode_header_buffer_too_small) {
    uint8_t buffer[4] = {};
    LdnHeader hdr{};
    DecodeResult r = decode_header(buffer, sizeof(buffer), hdr);
    ASSERT_EQ(r, DecodeResult::BufferTooSmall);
}

/** @brief Test case: decode header invalid magic. */
TEST(decode_header_invalid_magic) {
    uint8_t buffer[12] = {};
    LdnHeader hdr{};
    hdr.magic = 0xDEADBEEF;  // invalid
    hdr.version = PROTOCOL_VERSION;
    hdr.type = 1;
    hdr.data_size = 0;
    std::memcpy(buffer, &hdr, sizeof(hdr));
    LdnHeader decoded{};
    DecodeResult r = decode_header(buffer, sizeof(buffer), decoded);
    ASSERT_EQ(r, DecodeResult::InvalidMagic);
}

/** @brief Test case: decode header invalid version. */
TEST(decode_header_invalid_version) {
    uint8_t buffer[12] = {};
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = 99;  // invalid
    hdr.type = 1;
    hdr.data_size = 0;
    std::memcpy(buffer, &hdr, sizeof(hdr));
    LdnHeader decoded{};
    DecodeResult r = decode_header(buffer, sizeof(buffer), decoded);
    ASSERT_EQ(r, DecodeResult::InvalidVersion);
}

/** @brief Test case: decode header negative data size. */
TEST(decode_header_negative_data_size) {
    uint8_t buffer[12] = {};
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = PROTOCOL_VERSION;
    hdr.type = 1;
    hdr.data_size = -1;  // negative
    std::memcpy(buffer, &hdr, sizeof(hdr));
    LdnHeader decoded{};
    DecodeResult r = decode_header(buffer, sizeof(buffer), decoded);
    ASSERT_EQ(r, DecodeResult::PacketTooLarge);
}

/** @brief Test case: check complete packet buffer too small. */
TEST(check_complete_packet_buffer_too_small) {
    uint8_t buffer[4] = {};
    size_t pkt_size = 0;
    DecodeResult r = check_complete_packet(buffer, sizeof(buffer), pkt_size);
    ASSERT_EQ(r, DecodeResult::BufferTooSmall);
}

/** @brief Test case: get packet type from buffer. */
TEST(get_packet_type_from_buffer) {
    uint8_t buffer[64];
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.type = static_cast<uint8_t>(PacketId::Connected);
    hdr.version = PROTOCOL_VERSION;
    hdr.data_size = 0;
    std::memcpy(buffer, &hdr, sizeof(hdr));
    PacketId t = get_packet_type(buffer);
    ASSERT_EQ(t, PacketId::Connected);
}

/** @brief Test case: get payload size from buffer. */
TEST(get_payload_size_from_buffer) {
    uint8_t buffer[64];
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.type = 1;
    hdr.version = PROTOCOL_VERSION;
    hdr.data_size = 42;
    std::memcpy(buffer, &hdr, sizeof(hdr));
    int32_t sz = get_payload_size(buffer);
    ASSERT_EQ(sz, 42);
}

/** @brief Test case: get payload ptr offset. */
TEST(get_payload_ptr_offset) {
    uint8_t buffer[64] = {};
    const uint8_t* p = get_payload_ptr(buffer);
    ASSERT_TRUE(p == buffer + sizeof(LdnHeader));
}

/** @brief Test case: decode convenience functions. */
TEST(decode_convenience_functions) {
    uint8_t buffer[2048];
    size_t out_size = 0;

    // Initialize round-trip
    SessionId id{};
    MacAddress mac{};
    encode_initialize(buffer, sizeof(buffer), id, mac, out_size);
    LdnHeader hdr{};
    InitializeMessage init_msg{};
    DecodeResult r = decode_initialize(buffer, out_size, hdr, init_msg);
    ASSERT_EQ(r, DecodeResult::Success);

    // Passphrase round-trip
    PassphraseMessage pass_msg{};
    encode(buffer, sizeof(buffer), PacketId::Passphrase, pass_msg, out_size);
    PassphraseMessage dec_pass{};
    r = decode_passphrase(buffer, out_size, hdr, dec_pass);
    ASSERT_EQ(r, DecodeResult::Success);

    // Ping round-trip
    PingMessage ping{};
    ping.requester = 1;
    ping.id = 42;
    encode(buffer, sizeof(buffer), PacketId::Ping, ping, out_size);
    PingMessage dec_ping{};
    r = decode_ping(buffer, out_size, hdr, dec_ping);
    ASSERT_EQ(r, DecodeResult::Success);
    ASSERT_EQ(dec_ping.requester, 1);
    ASSERT_EQ(dec_ping.id, 42);

    // Disconnect round-trip
    DisconnectMessage disc{};
    disc.disconnect_ip = 0x0A720001;
    encode(buffer, sizeof(buffer), PacketId::Disconnect, disc, out_size);
    DisconnectMessage dec_disc{};
    r = decode_disconnect(buffer, out_size, hdr, dec_disc);
    ASSERT_EQ(r, DecodeResult::Success);
    ASSERT_EQ(dec_disc.disconnect_ip, 0x0A720001U);

    // NetworkInfo round-trip
    NetworkInfo net_info{};
    encode_network_info(
        buffer, sizeof(buffer), PacketId::Connected, net_info, out_size);
    NetworkInfo dec_net{};
    r = decode_network_info(buffer, out_size, hdr, dec_net);
    ASSERT_EQ(r, DecodeResult::Success);
    ASSERT_EQ(hdr.type, static_cast<uint8_t>(PacketId::Connected));

    // Scan round-trip
    ScanFilterFull scan_filter{};
    encode_scan(buffer, sizeof(buffer), scan_filter, out_size);
    ScanFilterFull dec_scan{};
    r = decode_scan(buffer, out_size, hdr, dec_scan);
    ASSERT_EQ(r, DecodeResult::Success);

    // Connect round-trip
    ConnectRequest conn_req{};
    encode_connect(buffer, sizeof(buffer), conn_req, out_size);
    ConnectRequest dec_conn{};
    r = decode_connect(buffer, out_size, hdr, dec_conn);
    ASSERT_EQ(r, DecodeResult::Success);

    // CreateAccessPoint round-trip with extra data
    CreateAccessPointRequest cap_req{};
    uint8_t adv[16] = {};
    encode_create_access_point(
        buffer, sizeof(buffer), cap_req, adv, sizeof(adv), out_size);
    CreateAccessPointRequest dec_cap{};
    size_t dec_adv_offset;
    size_t dec_adv_size;
    r = decode_create_access_point(
        buffer, out_size, hdr, dec_cap, dec_adv_offset, dec_adv_size);
    ASSERT_EQ(r, DecodeResult::Success);
    ASSERT_EQ(dec_adv_size, sizeof(adv));

    // SetAcceptPolicy round-trip
    SetAcceptPolicyRequest sap_req{};
    encode(
        buffer, sizeof(buffer), PacketId::SetAcceptPolicy, sap_req, out_size);
    SetAcceptPolicyRequest dec_sap{};
    r = decode_set_accept_policy(buffer, out_size, hdr, dec_sap);
    ASSERT_EQ(r, DecodeResult::Success);

    // SetAdvertiseData round-trip
    uint8_t adv_data[32] = {};
    encode_set_advertise_data(
        buffer, sizeof(buffer), adv_data, sizeof(adv_data), out_size);
    const uint8_t* dec_adv_data;
    size_t dec_adv_data_size;
    r = decode_set_advertise_data(
        buffer, out_size, hdr, dec_adv_data, dec_adv_data_size);
    ASSERT_EQ(r, DecodeResult::Success);
    ASSERT_EQ(dec_adv_data_size, sizeof(adv_data));

    // ProxyData round-trip
    ProxyDataHeader proxy_hdr{};
    proxy_hdr.data_length = sizeof(adv);
    encode_proxy_data(
        buffer, sizeof(buffer), proxy_hdr.info, adv, sizeof(adv), out_size);
    ProxyDataHeader dec_proxy_hdr{};
    size_t dec_proxy_data_offset;
    size_t dec_proxy_data_size;
    r = decode_proxy_data(
        buffer,
        out_size,
        hdr,
        dec_proxy_hdr,
        dec_proxy_data_offset,
        dec_proxy_data_size);
    ASSERT_EQ(r, DecodeResult::Success);

    // ProxyConnect round-trip
    ProxyConnectRequest pc_req{};
    encode(buffer, sizeof(buffer), PacketId::ProxyConnect, pc_req, out_size);
    ProxyConnectRequest dec_pc{};
    r = decode_proxy_connect(buffer, out_size, hdr, dec_pc);
    ASSERT_EQ(r, DecodeResult::Success);

    // ProxyConnectReply round-trip
    ProxyConnectResponse pc_resp{};
    encode(
        buffer, sizeof(buffer), PacketId::ProxyConnectReply, pc_resp, out_size);
    ProxyConnectResponse dec_pc_resp{};
    r = decode_proxy_connect_reply(buffer, out_size, hdr, dec_pc_resp);
    ASSERT_EQ(r, DecodeResult::Success);

    // ProxyDisconnect round-trip
    ProxyDisconnectMessage pd_msg{};
    encode(buffer, sizeof(buffer), PacketId::ProxyDisconnect, pd_msg, out_size);
    ProxyDisconnectMessage dec_pd{};
    r = decode_proxy_disconnect(buffer, out_size, hdr, dec_pd);
    ASSERT_EQ(r, DecodeResult::Success);

    // Reject round-trip
    RejectRequest rej_req{};
    encode(buffer, sizeof(buffer), PacketId::Reject, rej_req, out_size);
    RejectRequest dec_rej{};
    r = decode_reject(buffer, out_size, hdr, dec_rej);
    ASSERT_EQ(r, DecodeResult::Success);
}

/** @brief Test case: decode with data buffer too small. */
TEST(decode_with_data_buffer_too_small) {
    // Use ConnectRequest (0x274 bytes) so that sizeof(LdnHeader) +
    // sizeof(ConnectRequest) exceeds a small buffer
    uint8_t buffer[64] = {};
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = PROTOCOL_VERSION;
    hdr.type = static_cast<uint8_t>(PacketId::Connect);
    hdr.data_size = sizeof(ConnectRequest);
    std::memcpy(buffer, &hdr, sizeof(hdr));

    LdnHeader dec_hdr{};
    ConnectRequest msg{};
    size_t extra;
    size_t extra_size;
    // Buffer has 64 bytes but needs sizeof(LdnHeader) + sizeof(ConnectRequest)
    // = 12 + 636 = 648
    DecodeResult r = decode_with_data(
        buffer, sizeof(buffer), dec_hdr, msg, extra, extra_size);
    ASSERT_EQ(r, DecodeResult::BufferTooSmall);
    ASSERT_EQ(extra, 0U);
    ASSERT_EQ(extra_size, 0U);
}

/** @brief Test case: decode with data incomplete extra. */
TEST(decode_with_data_incomplete_extra) {
    uint8_t buffer[256] = {};
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = PROTOCOL_VERSION;
    hdr.type = static_cast<uint8_t>(PacketId::ProxyData);
    hdr.data_size = sizeof(PingMessage) + 100;
    std::memcpy(buffer, &hdr, sizeof(hdr));
    PingMessage ping{};
    std::memcpy(buffer + sizeof(hdr), &ping, sizeof(ping));

    LdnHeader dec_hdr{};
    PingMessage dec_msg{};
    size_t extra;
    size_t extra_size;
    // Buffer only has header+struct, not the extra data
    DecodeResult r = decode_with_data(
        buffer,
        sizeof(hdr) + sizeof(ping),
        dec_hdr,
        dec_msg,
        extra,
        extra_size);
    ASSERT_EQ(r, DecodeResult::IncompletePacket);
    ASSERT_EQ(extra, 0U);
    ASSERT_EQ(extra_size, 0U);
}

/** @brief Test case: decode with data invalid header. */
TEST(decode_with_data_invalid_header) {
    uint8_t buffer[256] = {};
    LdnHeader dec_hdr{};
    PingMessage msg{};
    size_t extra;
    size_t extra_size;
    DecodeResult r = decode_with_data(
        buffer, sizeof(buffer), dec_hdr, msg, extra, extra_size);
    ASSERT_TRUE(r != DecodeResult::Success);
    ASSERT_EQ(extra, 0U);
    ASSERT_EQ(extra_size, 0U);
}

/** @brief Test case: decode raw zero payload. */
TEST(decode_raw_zero_payload) {
    uint8_t buffer[64] = {};
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = PROTOCOL_VERSION;
    hdr.type = 1;
    hdr.data_size = 0;
    std::memcpy(buffer, &hdr, sizeof(hdr));
    LdnHeader dec_hdr{};
    const uint8_t* data;
    size_t data_size;
    DecodeResult r = decode_raw(buffer, sizeof(hdr), dec_hdr, data, data_size);
    ASSERT_EQ(r, DecodeResult::Success);
    ASSERT_TRUE(data == nullptr);
    ASSERT_EQ(data_size, 0U);
}

/** @brief Test case: decode raw invalid header. */
TEST(decode_raw_invalid_header) {
    uint8_t buffer[64] = {};
    LdnHeader dec_hdr{};
    const uint8_t* data;
    size_t data_size;
    DecodeResult r =
        decode_raw(buffer, sizeof(buffer), dec_hdr, data, data_size);
    ASSERT_TRUE(r != DecodeResult::Success);
    ASSERT_TRUE(data == nullptr);
    ASSERT_EQ(data_size, 0U);
}

/** @brief Test case: decode raw incomplete. */
TEST(decode_raw_incomplete) {
    uint8_t buffer[256] = {};
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = PROTOCOL_VERSION;
    hdr.type = 1;
    hdr.data_size = 50;
    std::memcpy(buffer, &hdr, sizeof(hdr));
    LdnHeader dec_hdr{};
    const uint8_t* data;
    size_t data_size;
    // Only provide header, not payload
    DecodeResult r = decode_raw(buffer, sizeof(hdr), dec_hdr, data, data_size);
    ASSERT_EQ(r, DecodeResult::IncompletePacket);
    ASSERT_TRUE(data == nullptr);
    ASSERT_EQ(data_size, 0U);
}

/** @brief Test case: decode struct buffer too small. */
TEST(decode_struct_buffer_too_small) {
    // Use ConnectRequest (0x274 bytes) which exceeds small buffer
    uint8_t buffer[64] = {};
    LdnHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = PROTOCOL_VERSION;
    hdr.type = static_cast<uint8_t>(PacketId::Connect);
    hdr.data_size = sizeof(ConnectRequest);
    std::memcpy(buffer, &hdr, sizeof(hdr));

    LdnHeader dec_hdr{};
    ConnectRequest msg{};
    DecodeResult r = decode(buffer, sizeof(buffer), dec_hdr, msg);
    ASSERT_EQ(r, DecodeResult::BufferTooSmall);
}

/** @brief Test case: result to string all values. */
TEST(result_to_string_all_values) {
    // BufferResult
    ASSERT_TRUE(
        strcmp(buffer_result_to_string(BufferResult::Success), "Success") == 0);
    ASSERT_TRUE(
        strcmp(
            buffer_result_to_string(BufferResult::BufferFull), "BufferFull") ==
        0);
    ASSERT_TRUE(
        strcmp(
            buffer_result_to_string(BufferResult::NoCompletePacket),
            "NoCompletePacket") == 0);
    ASSERT_TRUE(
        strcmp(
            buffer_result_to_string(BufferResult::PacketTooLarge),
            "PacketTooLarge") == 0);
    ASSERT_TRUE(
        strcmp(
            buffer_result_to_string(BufferResult::InvalidPacket),
            "InvalidPacket") == 0);

    // DecodeResult
    ASSERT_TRUE(
        strcmp(decode_result_to_string(DecodeResult::Success), "Success") == 0);
    ASSERT_TRUE(
        strcmp(
            decode_result_to_string(DecodeResult::BufferTooSmall),
            "BufferTooSmall") == 0);
    ASSERT_TRUE(
        strcmp(
            decode_result_to_string(DecodeResult::InvalidMagic),
            "InvalidMagic") == 0);
    ASSERT_TRUE(
        strcmp(
            decode_result_to_string(DecodeResult::InvalidVersion),
            "InvalidVersion") == 0);
    ASSERT_TRUE(
        strcmp(
            decode_result_to_string(DecodeResult::PacketTooLarge),
            "PacketTooLarge") == 0);
    ASSERT_TRUE(
        strcmp(
            decode_result_to_string(DecodeResult::IncompletePacket),
            "IncompletePacket") == 0);

    // EncodeResult
    ASSERT_TRUE(
        strcmp(encode_result_to_string(EncodeResult::Success), "Success") == 0);
    ASSERT_TRUE(
        strcmp(
            encode_result_to_string(EncodeResult::BufferTooSmall),
            "BufferTooSmall") == 0);
    ASSERT_TRUE(
        strcmp(
            encode_result_to_string(EncodeResult::InvalidPacketId),
            "InvalidPacketId") == 0);

    // PacketId
    ASSERT_TRUE(
        strcmp(packet_id_to_string(PacketId::Initialize), "Initialize") == 0);
    ASSERT_TRUE(
        strcmp(packet_id_to_string(PacketId::Connected), "Connected") == 0);
    ASSERT_TRUE(
        strcmp(packet_id_to_string(PacketId::ProxyData), "ProxyData") == 0);
    ASSERT_TRUE(
        strcmp(packet_id_to_string(PacketId::NetworkError), "NetworkError") ==
        0);
}

// ============================================================================
// Failure paths of the generic encode/decode templates
// ============================================================================

namespace {

// Scratch buffer large enough for the biggest payload plus header and extras.
/// Test fixture value: SCRATCH.
constexpr size_t SCRATCH = 8192;

template <typename T>
/// Test helper: check encode too small.
void check_encode_too_small() {
    T payload{};
    std::memset(&payload, 0, sizeof(payload));
    uint8_t buf[SCRATCH];
    size_t out = 123;
    ASSERT_TRUE(get_packet_size<T>() <= SCRATCH);
    ASSERT_TRUE(
        encode(buf, get_packet_size<T>() - 1, PacketId::Ping, payload, out) ==
        EncodeResult::BufferTooSmall);
    ASSERT_EQ(out, 0);
    ASSERT_TRUE(
        encode(buf, get_packet_size<T>(), PacketId::Ping, payload, out) ==
        EncodeResult::Success);
    ASSERT_EQ(out, get_packet_size<T>());
}

template <typename T>
/// Test helper: check decode failures.
void check_decode_failures() {
    uint8_t buf[SCRATCH];
    LdnHeader header{};
    T payload{};
    ASSERT_TRUE(get_packet_size<T>() <= SCRATCH);

    // Header invalid: all-zero bytes carry no protocol magic.
    std::memset(buf, 0, sizeof(buf));
    ASSERT_TRUE(
        decode(buf, sizeof(buf), header, payload) ==
        DecodeResult::InvalidMagic);

    // Header valid, but the buffer stops right after it (payload missing).
    encode_header(buf, PacketId::Ping, static_cast<int32_t>(sizeof(T)));
    ASSERT_TRUE(
        decode(buf, sizeof(LdnHeader), header, payload) ==
        DecodeResult::BufferTooSmall);

    // Complete packet decodes.
    ASSERT_TRUE(
        decode(buf, get_packet_size<T>(), header, payload) ==
        DecodeResult::Success);
}

template <typename T>
/// Test helper: check decode with data paths.
void check_decode_with_data_paths() {
    uint8_t buf[SCRATCH];
    LdnHeader header{};
    T payload{};
    size_t extra = 99;
    size_t extra_size = 77;

    // Invalid header.
    std::memset(buf, 0, sizeof(buf));
    ASSERT_TRUE(
        decode_with_data(
            buf, sizeof(buf), header, payload, extra, extra_size) ==
        DecodeResult::InvalidMagic);
    ASSERT_EQ(extra, 0U);
    ASSERT_EQ(extra_size, 0);

    // Header only, payload structure missing.
    extra = 99;
    extra_size = 77;
    encode_header(buf, PacketId::Ping, static_cast<int32_t>(sizeof(T)));
    ASSERT_TRUE(
        decode_with_data(
            buf, sizeof(LdnHeader), header, payload, extra, extra_size) ==
        DecodeResult::BufferTooSmall);
    ASSERT_EQ(extra, 0U);
    ASSERT_EQ(extra_size, 0);

    // Structure only, no trailing data.
    extra = 99;
    extra_size = 77;
    ASSERT_TRUE(
        decode_with_data(
            buf, get_packet_size<T>(), header, payload, extra, extra_size) ==
        DecodeResult::Success);
    ASSERT_EQ(extra, 0U);
    ASSERT_EQ(extra_size, 0);

    // Trailing data announced but only partly received.
    encode_header(buf, PacketId::Ping, static_cast<int32_t>(sizeof(T) + 10));
    extra = 99;
    extra_size = 77;
    ASSERT_TRUE(
        decode_with_data(
            buf,
            get_packet_size<T>() + 9,
            header,
            payload,
            extra,
            extra_size) == DecodeResult::IncompletePacket);
    ASSERT_EQ(extra, 0U);
    ASSERT_EQ(extra_size, 0);

    // Trailing data complete.
    extra = 99;
    extra_size = 0;
    ASSERT_TRUE(
        decode_with_data(
            buf,
            get_packet_size<T>() + 10,
            header,
            payload,
            extra,
            extra_size) == DecodeResult::Success);
    ASSERT_EQ(extra_size, 10);
    ASSERT_EQ(extra, get_packet_size<T>());
}

template <typename T>
/// Test helper: check encode with data too small.
void check_encode_with_data_too_small() {
    T payload{};
    std::memset(&payload, 0, sizeof(payload));
    const uint8_t extra[5] = {1, 2, 3, 4, 5};
    uint8_t buf[SCRATCH];
    size_t out = 123;
    ASSERT_TRUE(
        encode_with_data(
            buf,
            get_packet_size<T>() + 4,
            PacketId::Ping,
            payload,
            extra,
            sizeof(extra),
            out) == EncodeResult::BufferTooSmall);
    ASSERT_EQ(out, 0);
    ASSERT_TRUE(
        encode_with_data(
            buf,
            get_packet_size<T>() + 5,
            PacketId::Ping,
            payload,
            extra,
            sizeof(extra),
            out) == EncodeResult::Success);
    ASSERT_EQ(out, get_packet_size<T>() + 5);
    ASSERT_EQ(std::memcmp(buf + get_packet_size<T>(), extra, sizeof(extra)), 0);
}

}  // namespace

/** @brief Test case: encode payload rejects a buffer one byte too small. */
TEST(encode_payload_rejects_a_buffer_one_byte_too_small) {
    check_encode_too_small<ConnectRequest>();
    check_encode_too_small<DisconnectMessage>();
    check_encode_too_small<InitializeMessage>();
    check_encode_too_small<PassphraseMessage>();
    check_encode_too_small<PingMessage>();
    check_encode_too_small<ProxyConnectRequest>();
    check_encode_too_small<ProxyConnectResponse>();
    check_encode_too_small<ProxyDisconnectMessage>();
    check_encode_too_small<RejectRequest>();
    check_encode_too_small<ScanFilterFull>();
    check_encode_too_small<SetAcceptPolicyRequest>();
}

/**
 * @brief Test case: decode payload rejects bad header and truncated payload.
 */
TEST(decode_payload_rejects_bad_header_and_truncated_payload) {
    check_decode_failures<ConnectRequest>();
    check_decode_failures<DisconnectMessage>();
    check_decode_failures<InitializeMessage>();
    check_decode_failures<NetworkInfo>();
    check_decode_failures<PassphraseMessage>();
    check_decode_failures<PingMessage>();
    check_decode_failures<ProxyConnectRequest>();
    check_decode_failures<ProxyConnectResponse>();
    check_decode_failures<ProxyDisconnectMessage>();
    check_decode_failures<RejectRequest>();
    check_decode_failures<ScanFilterFull>();
    check_decode_failures<SetAcceptPolicyRequest>();
}

/** @brief Test case: decode with data covers every outcome. */
TEST(decode_with_data_covers_every_outcome) {
    check_decode_with_data_paths<ConnectRequest>();
    check_decode_with_data_paths<CreateAccessPointRequest>();
    check_decode_with_data_paths<PingMessage>();
    check_decode_with_data_paths<ProxyDataHeader>();
    check_decode_with_data_paths<ProxyInfo>();
}

/**
 * @brief Test case: encode with data rejects a buffer too small for the extra
 *   bytes.
 */
TEST(encode_with_data_rejects_a_buffer_too_small_for_the_extra_bytes) {
    check_encode_with_data_too_small<CreateAccessPointRequest>();
    check_encode_with_data_too_small<ProxyInfo>();
}

/**
 * @brief Test case: to string helpers report unknown for out of range values.
 */
TEST(to_string_helpers_report_unknown_for_out_of_range_values) {
    ASSERT_TRUE(
        strcmp(
            buffer_result_to_string(unknown_enum_value<BufferResult>(99)),
            "Unknown") == 0);
    ASSERT_TRUE(
        strcmp(
            decode_result_to_string(unknown_enum_value<DecodeResult>(99)),
            "Unknown") == 0);
    ASSERT_TRUE(
        strcmp(
            encode_result_to_string(unknown_enum_value<EncodeResult>(99)),
            "Unknown") == 0);
    ASSERT_TRUE(
        strcmp(packet_id_to_string(static_cast<PacketId>(200)), "Unknown") ==
        0);
}

// ============================================================================
// PacketBuffer edge cases
// ============================================================================

namespace {

/**
 * @brief Builds a header-only packet (plus optional payload) in `out`.
 *
 * @param out buffer receiving the header and payload
 * @param type packet id
 * @param data_size payload size written to the header; the payload is filled
 * when positive
 * @param fill byte value of every payload byte
 * @return total packet size in bytes (header plus payload)
 */
size_t make_packet(
    uint8_t* out, PacketId type, int32_t data_size, uint8_t fill = 0xAB) {
    size_t n = encode_header(out, type, data_size);
    if (data_size > 0) {
        std::memset(out + n, fill, static_cast<size_t>(data_size));
        n += static_cast<size_t>(data_size);
    }
    return n;
}

}  // namespace

/**
 * @brief Test case: packet buffer append empty is a noop and overflow is
 *   refused.
 */
TEST(packet_buffer_append_empty_is_a_noop_and_overflow_is_refused) {
    PacketBuffer<64> pb;
    const uint8_t b[1] = {0};
    ASSERT_TRUE(pb.append(b, 0) == BufferResult::Success);
    ASSERT_EQ(pb.size(), 0);

    uint8_t big[65] = {0};
    ASSERT_TRUE(pb.append(big, sizeof(big)) == BufferResult::BufferFull);
    ASSERT_EQ(pb.size(), 0);
    ASSERT_TRUE(pb.append(big, 64) == BufferResult::Success);
    ASSERT_EQ(pb.size(), 64);
    ASSERT_TRUE(pb.append(b, 1) == BufferResult::BufferFull);
}

/**
 * @brief Test case: packet buffer append overflow on small and large
 *   capacities.
 */
TEST(packet_buffer_append_overflow_on_small_and_large_capacities) {
    uint8_t blob[300] = {0};
    PacketBuffer<16> p16;
    ASSERT_TRUE(p16.append(blob, 17) == BufferResult::BufferFull);
    ASSERT_TRUE(p16.append(blob, 0) == BufferResult::Success);
    PacketBuffer<256> p256;
    ASSERT_TRUE(p256.append(blob, 257) == BufferResult::BufferFull);
    ASSERT_TRUE(p256.append(blob, 0) == BufferResult::Success);
    PacketBuffer<8192> p8k;
    ASSERT_TRUE(p8k.append(blob, 0) == BufferResult::Success);
    PacketBuffer<1024> p1k;
    ASSERT_TRUE(p1k.append(blob, 0) == BufferResult::Success);
    ASSERT_TRUE(p1k.append(blob, 1025) == BufferResult::BufferFull);
    PacketBuffer<64> p64;
    ASSERT_TRUE(p64.append(blob, 0) == BufferResult::Success);
    ASSERT_TRUE(p64.append(blob, 65) == BufferResult::BufferFull);
}

/** @brief Test case: packet buffer peek info maps every header state. */
TEST(packet_buffer_peek_info_maps_every_header_state) {
    uint8_t pkt[256];
    size_t size = 99;

    // Fewer bytes than a header.
    PacketBuffer<1024> pb;
    const uint8_t few[3] = {1, 2, 3};
    pb.append(few, sizeof(few));
    ASSERT_TRUE(pb.peek_packet_info(size) == BufferResult::NoCompletePacket);
    ASSERT_EQ(size, 0);
    ASSERT_TRUE(pb.peek_packet(size) == nullptr);
    ASSERT_EQ(size, 0);
    ASSERT_TRUE(pb.peek_packet_type() == PacketId::Initialize);

    // Header announces more data than received.
    PacketBuffer<1024> partial;
    size_t n = make_packet(pkt, PacketId::Ping, 16);
    partial.append(pkt, n - 1);
    ASSERT_TRUE(
        partial.peek_packet_info(size) == BufferResult::NoCompletePacket);
    ASSERT_TRUE(partial.peek_packet(size) == nullptr);
    ASSERT_FALSE(partial.has_complete_packet());

    // Announced size beyond the protocol maximum.
    PacketBuffer<1024> huge;
    encode_header(
        pkt, PacketId::Ping, static_cast<int32_t>(MAX_PACKET_SIZE) + 1);
    huge.append(pkt, sizeof(LdnHeader));
    ASSERT_TRUE(huge.peek_packet_info(size) == BufferResult::PacketTooLarge);

    // Wrong magic and wrong version.
    PacketBuffer<1024> bad_magic;
    n = make_packet(pkt, PacketId::Ping, 0);
    pkt[0] ^= 0xFF;
    bad_magic.append(pkt, n);
    ASSERT_TRUE(
        bad_magic.peek_packet_info(size) == BufferResult::InvalidPacket);
    ASSERT_TRUE(bad_magic.peek_packet_type() == PacketId::Initialize);

    PacketBuffer<256> bad_version;
    n = make_packet(pkt, PacketId::Ping, 0);
    LdnHeader h{};
    std::memcpy(&h, pkt, sizeof(h));
    h.version = static_cast<decltype(h.version)>(h.version + 1);
    std::memcpy(pkt, &h, sizeof(h));
    bad_version.append(pkt, n);
    ASSERT_TRUE(
        bad_version.peek_packet_info(size) == BufferResult::InvalidPacket);

    // Same mapping on the 256-byte instantiation, plus a valid type read.
    PacketBuffer<256> ok;
    n = make_packet(pkt, PacketId::Ping, 4);
    ok.append(pkt, n);
    ASSERT_TRUE(ok.peek_packet_info(size) == BufferResult::Success);
    ASSERT_EQ(size, n);
    ASSERT_TRUE(ok.peek_packet_type() == PacketId::Ping);
    ASSERT_TRUE(ok.peek_packet(size) == ok.data());

    PacketBuffer<256> bad256;
    pkt[0] ^= 0xFF;
    bad256.append(pkt, n);
    ASSERT_TRUE(bad256.peek_packet_info(size) == BufferResult::InvalidPacket);
    ASSERT_TRUE(bad256.peek_packet_type() == PacketId::Initialize);

    PacketBuffer<256> huge256;
    encode_header(pkt, PacketId::Ping, -1);
    huge256.append(pkt, sizeof(LdnHeader));
    ASSERT_TRUE(huge256.peek_packet_info(size) == BufferResult::PacketTooLarge);
}

/** @brief Test case: packet buffer consume partial and empty. */
TEST(packet_buffer_consume_partial_and_empty) {
    PacketBuffer<8192> pb;
    const uint8_t data[6] = {1, 2, 3, 4, 5, 6};
    pb.append(data, sizeof(data));
    pb.consume(0);
    ASSERT_EQ(pb.size(), 6);
    pb.consume(2);
    ASSERT_EQ(pb.size(), 4);
    ASSERT_EQ(std::memcmp(pb.data(), data + 2, 4), 0);
    pb.consume(100);
    ASSERT_TRUE(pb.empty());

    PacketBuffer<1024> p1k;
    p1k.append(data, sizeof(data));
    p1k.consume(0);
    ASSERT_EQ(p1k.size(), 6);
}

/** @brief Test case: packet buffer extract reports failures. */
TEST(packet_buffer_extract_reports_failures) {
    uint8_t pkt[64];
    uint8_t out[64];
    size_t size = 0;

    PacketBuffer<1024> empty;
    ASSERT_TRUE(
        empty.extract_packet(out, sizeof(out), size) ==
        BufferResult::NoCompletePacket);

    PacketBuffer<1024> pb;
    size_t n = make_packet(pkt, PacketId::Ping, 8);
    pb.append(pkt, n);
    ASSERT_TRUE(
        pb.extract_packet(out, n - 1, size) == BufferResult::BufferFull);
    ASSERT_EQ(pb.size(), n);
    ASSERT_TRUE(
        pb.extract_packet(out, sizeof(out), size) == BufferResult::Success);
    ASSERT_EQ(size, n);
    ASSERT_EQ(std::memcmp(out, pkt, n), 0);
    ASSERT_TRUE(pb.empty());

    PacketBuffer<256> small;
    ASSERT_TRUE(
        small.extract_packet(out, sizeof(out), size) ==
        BufferResult::NoCompletePacket);
    small.append(pkt, n);
    ASSERT_TRUE(
        small.extract_packet(out, n - 1, size) == BufferResult::BufferFull);
}

/**
 * @brief Test case: packet buffer write ptr and advance write respect capacity.
 */
TEST(packet_buffer_write_ptr_and_advance_write_respect_capacity) {
    PacketBuffer<16> pb;
    ASSERT_EQ(pb.capacity(), 16);
    std::memset(pb.write_ptr(), 0x11, 10);
    pb.advance_write(10);
    ASSERT_EQ(pb.size(), 10);
    pb.advance_write(7);  // would exceed the capacity: ignored
    ASSERT_EQ(pb.size(), 10);
    pb.advance_write(6);  // exactly fills it
    ASSERT_EQ(pb.size(), 16);
    ASSERT_EQ(pb.available(), 0);
}

/** @brief Test case: packet buffer discard until valid skips garbage. */
TEST(packet_buffer_discard_until_valid_skips_garbage) {
    uint8_t pkt[64];
    PacketBuffer<64> pb;
    const uint8_t junk[3] = {0xDE, 0xAD, 0xBE};
    pb.append(junk, sizeof(junk));
    size_t n = make_packet(pkt, PacketId::Ping, 0);
    pb.append(pkt, n);
    ASSERT_EQ(pb.discard_until_valid(), 3);
    ASSERT_TRUE(pb.has_complete_packet());
    ASSERT_EQ(pb.discard_until_valid(), 0);

    PacketBuffer<16> tiny;
    tiny.append(junk, sizeof(junk));
    ASSERT_EQ(tiny.discard_until_valid(), 0);
    ASSERT_EQ(tiny.size(), 3);
}

namespace {

// Every public branch of PacketBuffer<N>; each size is a separate template
// instantiation and is measured as such by the line-coverage gate.
template <size_t N>
/// Test helper: check packet buffer api.
void check_packet_buffer_api() {
    uint8_t pkt[64];
    uint8_t out[64];
    size_t size = 99;
    const uint8_t junk[3] = {0xDE, 0xAD, 0xBE};
    LdnHeader h{};

    // --- empty / too short -------------------------------------------------
    PacketBuffer<N> pb;
    ASSERT_TRUE(pb.empty());
    ASSERT_FALSE(pb.has_complete_packet());
    ASSERT_TRUE(pb.peek_packet_type() == PacketId::Initialize);
    ASSERT_TRUE(pb.peek_packet_info(size) == BufferResult::NoCompletePacket);
    ASSERT_TRUE(
        pb.extract_packet(out, sizeof(out), size) ==
        BufferResult::NoCompletePacket);
    ASSERT_EQ(pb.discard_until_valid(), 0);
    ASSERT_TRUE(pb.append(junk, 0) == BufferResult::Success);
    pb.consume(0);
    ASSERT_TRUE(pb.append(junk, sizeof(junk)) == BufferResult::Success);
    ASSERT_FALSE(pb.has_complete_packet());
    ASSERT_TRUE(pb.peek_packet_type() == PacketId::Initialize);
    ASSERT_TRUE(pb.peek_packet(size) == nullptr);

    // --- garbage then a valid packet --------------------------------------
    size_t n = make_packet(pkt, PacketId::Ping, 1);
    ASSERT_TRUE(pb.append(pkt, n) == BufferResult::Success);
    ASSERT_EQ(pb.discard_until_valid(), 3);
    ASSERT_TRUE(pb.has_complete_packet());
    ASSERT_TRUE(pb.peek_packet_type() == PacketId::Ping);
    ASSERT_TRUE(pb.peek_packet(size) == pb.data());
    ASSERT_EQ(size, n);

    // --- partial consume keeps the tail ------------------------------------
    ASSERT_TRUE(pb.append(junk, sizeof(junk)) == BufferResult::Success);
    pb.consume(n);
    ASSERT_EQ(pb.size(), 3);
    ASSERT_EQ(std::memcmp(pb.data(), junk, 3), 0);
    pb.consume(sizeof(pkt));
    ASSERT_TRUE(pb.empty());

    // --- extract: buffer too small, then ok -------------------------------
    pb.append(pkt, n);
    ASSERT_TRUE(
        pb.extract_packet(out, n - 1, size) == BufferResult::BufferFull);
    ASSERT_EQ(pb.size(), n);
    ASSERT_TRUE(
        pb.extract_packet(out, sizeof(out), size) == BufferResult::Success);
    ASSERT_EQ(std::memcmp(out, pkt, n), 0);
    ASSERT_TRUE(pb.empty());

    // --- header states -----------------------------------------------------
    pb.append(pkt, n - 1);  // announced 1 byte, none present
    ASSERT_FALSE(pb.has_complete_packet());
    ASSERT_TRUE(pb.peek_packet_info(size) == BufferResult::NoCompletePacket);
    pb.consume(sizeof(pkt));

    encode_header(
        pkt, PacketId::Ping, static_cast<int32_t>(MAX_PACKET_SIZE) + 1);
    pb.append(pkt, sizeof(LdnHeader));
    ASSERT_TRUE(pb.peek_packet_info(size) == BufferResult::PacketTooLarge);
    pb.consume(sizeof(pkt));

    n = make_packet(pkt, PacketId::Ping, 0);
    pkt[0] ^= 0xFF;  // corrupt the magic
    pb.append(pkt, n);
    ASSERT_TRUE(pb.peek_packet_info(size) == BufferResult::InvalidPacket);
    ASSERT_TRUE(pb.peek_packet_type() == PacketId::Initialize);
    pb.consume(sizeof(pkt));

    n = make_packet(pkt, PacketId::Ping, 0);
    std::memcpy(&h, pkt, sizeof(h));
    h.version = static_cast<decltype(h.version)>(h.version + 1);
    std::memcpy(pkt, &h, sizeof(h));
    pb.append(pkt, n);
    ASSERT_TRUE(pb.peek_packet_info(size) == BufferResult::InvalidPacket);
    pb.consume(sizeof(pkt));

    // --- capacity ----------------------------------------------------------
    ASSERT_EQ(PacketBuffer<N>::capacity(), N);
    uint8_t big[N + 1];
    std::memset(big, 0, sizeof(big));
    ASSERT_TRUE(pb.append(big, N + 1) == BufferResult::BufferFull);
    ASSERT_TRUE(pb.append(big, N) == BufferResult::Success);
    ASSERT_EQ(pb.available(), 0);
    ASSERT_TRUE(pb.append(big, 1) == BufferResult::BufferFull);
    pb.consume(N);
    pb.advance_write(N + 1);  // beyond capacity: ignored
    ASSERT_EQ(pb.size(), 0);
    std::memset(pb.write_ptr(), 0, N);
    pb.advance_write(N);
    ASSERT_EQ(pb.size(), N);
}

}  // namespace

/**
 * @brief Test case: packet buffer api is exhaustive for every instantiated
 *   size.
 */
TEST(packet_buffer_api_is_exhaustive_for_every_instantiated_size) {
    check_packet_buffer_api<16>();
    check_packet_buffer_api<64>();
    check_packet_buffer_api<256>();
    check_packet_buffer_api<1024>();
    check_packet_buffer_api<8192>();
}

/** @brief Test case: has header threshold. */
TEST(has_header_threshold) {
    ASSERT_TRUE(has_header(12));
    ASSERT_TRUE(has_header(100));
    ASSERT_TRUE(!has_header(11));
    ASSERT_TRUE(!has_header(0));
}

/**
 * @brief Test case: encode refuses a buffer one byte too small for every
 *   payload type.
 */
TEST(encode_refuses_a_buffer_one_byte_too_small_for_every_payload_type) {
    uint8_t buffer[512] = {};
    size_t out_size = 99;
    ConnectPrivateRequest connect_private{};
    ASSERT_TRUE(
        encode(
            buffer,
            get_packet_size<ConnectPrivateRequest>() - 1,
            PacketId::ConnectPrivate,
            connect_private,
            out_size) == EncodeResult::BufferTooSmall);
    ASSERT_EQ(out_size, 0U);
    out_size = 99;
    ExternalProxyConfig external{};
    ASSERT_TRUE(
        encode(
            buffer,
            get_packet_size<ExternalProxyConfig>() - 1,
            PacketId::ExternalProxy,
            external,
            out_size) == EncodeResult::BufferTooSmall);
    ASSERT_EQ(out_size, 0U);
    out_size = 99;
    ProxyConfig config{};
    ASSERT_TRUE(
        encode(
            buffer,
            get_packet_size<ProxyConfig>() - 1,
            PacketId::ProxyConfig,
            config,
            out_size) == EncodeResult::BufferTooSmall);
    ASSERT_EQ(out_size, 0U);
    out_size = 99;
    ExternalProxyConnectionState state{};
    ASSERT_TRUE(
        encode(
            buffer,
            get_packet_size<ExternalProxyConnectionState>() - 1,
            PacketId::ExternalProxyState,
            state,
            out_size) == EncodeResult::BufferTooSmall);
    ASSERT_EQ(out_size, 0U);
}

/**
 * @brief Test case: encode writes header and payload for every payload type.
 */
TEST(encode_writes_header_and_payload_for_every_payload_type) {
    uint8_t buffer[512] = {};
    size_t out_size = 0;
    ConnectPrivateRequest connect_private{};
    ASSERT_TRUE(
        encode(
            buffer,
            sizeof(buffer),
            PacketId::ConnectPrivate,
            connect_private,
            out_size) == EncodeResult::Success);
    ASSERT_EQ(out_size, get_packet_size<ConnectPrivateRequest>());
    out_size = 0;
    ExternalProxyConfig external{};
    ASSERT_TRUE(
        encode(
            buffer,
            sizeof(buffer),
            PacketId::ExternalProxy,
            external,
            out_size) == EncodeResult::Success);
    ASSERT_EQ(out_size, get_packet_size<ExternalProxyConfig>());
    out_size = 0;
    ExternalProxyConnectionState state{};
    ASSERT_TRUE(
        encode(
            buffer,
            sizeof(buffer),
            PacketId::ExternalProxyState,
            state,
            out_size) == EncodeResult::Success);
    ASSERT_EQ(out_size, get_packet_size<ExternalProxyConnectionState>());
}

// ============================================================================
// Main
// ============================================================================

/**
 * @brief Runs the whole suite; the exit code is non-zero if any test failed.
 *
 * @return 0 when every test passed, non-zero otherwise
 */
int main() {
    printf("=== ryu_ldn_nx Protocol Unit Tests ===\n\n");
    run_all_tests();
    return g_tests_failed > 0 ? 1 : 0;
}
