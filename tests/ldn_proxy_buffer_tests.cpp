/**
 * @file ldn_proxy_buffer_tests.cpp
 * @brief Unit tests for the real LdnProxyBuffer (ldn/ldn_proxy_buffer.cpp)
 *
 * Built against the host Stratosphere stand-in in tests/stubs.
 */

#include <cstdio>
#include <cstdint>

#include <cstring>

#include <vector>

#include "test_harness.hpp"
#include "ldn/ldn_proxy_buffer.hpp"

using ams::mitm::ldn::LdnProxyBuffer;
using ryu_ldn::protocol::ProxyDataHeader;

/**
 * @brief Test helper: header with tag.
 *
 * @param tag value stored in info.source_ipv4 to identify the packet
 * @param length value stored in data_length
 * @return zeroed header carrying tag and length
 */
static ProxyDataHeader header_with_tag(uint32_t tag, uint32_t length) {
    ProxyDataHeader h;
    std::memset(&h, 0, sizeof(h));
    h.info.source_ipv4 = tag;
    h.data_length = length;
    return h;
}

/**
 * @brief Test helper: payload.
 *
 * @param size number of bytes
 * @param fill byte value every element is set to
 * @return vector of size bytes, all equal to fill
 */
static std::vector<u8> payload(size_t size, u8 fill) {
    return std::vector<u8>(size, fill);
}

/**
 * @brief Test helper: write packet.
 *
 * @param b buffer to write to
 * @param tag packet tag, see header_with_tag()
 * @param size payload size in bytes
 * @param fill payload byte value
 * @return result of LdnProxyBuffer::Write(): true when the packet was stored
 */
static bool write_packet(
    LdnProxyBuffer& b, uint32_t tag, size_t size, u8 fill) {
    const auto data = payload(size, fill);
    return b.Write(
        header_with_tag(tag, static_cast<uint32_t>(size)), data.data(), size);
}

/** @brief Test case: a new buffer is empty. */
TEST(a_new_buffer_is_empty) {
    LdnProxyBuffer b;
    ASSERT_TRUE(b.IsEmpty());
    ASSERT_EQ(b.GetPendingCount(), static_cast<size_t>(0));
    ASSERT_EQ(b.GetUsedBytes(), static_cast<size_t>(0));
    ProxyDataHeader h;
    size_t size = 0;
    u8 out[8];
    ASSERT_FALSE(b.Peek(h, size));
    ASSERT_FALSE(b.Read(h, out, size, sizeof(out)));
}

/** @brief Test case: a packet is read back with its header and payload. */
TEST(a_packet_is_read_back_with_its_header_and_payload) {
    LdnProxyBuffer b;
    ASSERT_TRUE(write_packet(b, 7, 5, 0xAB));
    ASSERT_EQ(b.GetPendingCount(), static_cast<size_t>(1));
    ASSERT_EQ(b.GetUsedBytes(), static_cast<size_t>(5));

    ProxyDataHeader h;
    size_t size = 0;
    ASSERT_TRUE(b.Peek(h, size));
    ASSERT_EQ(h.info.source_ipv4, 7u);
    ASSERT_EQ(size, static_cast<size_t>(5));
    ASSERT_EQ(b.GetPendingCount(), static_cast<size_t>(1));

    u8 out[16] = {};
    ASSERT_TRUE(b.Read(h, out, size, sizeof(out)));
    ASSERT_EQ(h.data_length, 5u);
    ASSERT_EQ(size, static_cast<size_t>(5));
    ASSERT_EQ(out[0], static_cast<u8>(0xAB));
    ASSERT_EQ(out[4], static_cast<u8>(0xAB));
    ASSERT_TRUE(b.IsEmpty());
    ASSERT_EQ(b.GetUsedBytes(), static_cast<size_t>(0));
}

/** @brief Test case: packets come out in arrival order. */
TEST(packets_come_out_in_arrival_order) {
    LdnProxyBuffer b;
    ASSERT_TRUE(write_packet(b, 1, 10, 0x11));
    ASSERT_TRUE(write_packet(b, 2, 20, 0x22));
    ASSERT_TRUE(write_packet(b, 3, 30, 0x33));
    ProxyDataHeader h;
    size_t size = 0;
    u8 out[64];
    for (uint32_t tag = 1; tag <= 3; ++tag) {
        ASSERT_TRUE(b.Read(h, out, size, sizeof(out)));
        ASSERT_EQ(h.info.source_ipv4, tag);
        ASSERT_EQ(size, static_cast<size_t>(tag * 10));
        ASSERT_EQ(out[0], static_cast<u8>(tag * 0x11));
    }
}

/**
 * @brief Test case: read truncates to the caller buffer and still consumes the
 *   packet.
 */
TEST(read_truncates_to_the_caller_buffer_and_still_consumes_the_packet) {
    LdnProxyBuffer b;
    ASSERT_TRUE(write_packet(b, 1, 100, 0x5A));
    ProxyDataHeader h;
    size_t size = 0;
    u8 out[10];
    ASSERT_TRUE(b.Read(h, out, size, sizeof(out)));
    ASSERT_EQ(size, static_cast<size_t>(10));
    ASSERT_EQ(h.data_length, 100u);
    ASSERT_TRUE(b.IsEmpty());
}

/** @brief Test case: a zero length packet and a null payload are accepted. */
TEST(a_zero_length_packet_and_a_null_payload_are_accepted) {
    LdnProxyBuffer b;
    ASSERT_TRUE(b.Write(header_with_tag(9, 0), nullptr, 0));
    ProxyDataHeader h;
    size_t size = 99;
    ASSERT_TRUE(b.Read(h, nullptr, size, 0));
    ASSERT_EQ(size, static_cast<size_t>(0));
    ASSERT_EQ(h.info.source_ipv4, 9u);
}

/** @brief Test case: a null output buffer still consumes the packet. */
TEST(a_null_output_buffer_still_consumes_the_packet) {
    LdnProxyBuffer b;
    ASSERT_TRUE(write_packet(b, 1, 8, 0x01));
    ProxyDataHeader h;
    size_t size = 0;
    ASSERT_TRUE(b.Read(h, nullptr, size, 8));
    ASSERT_EQ(size, static_cast<size_t>(8));
    ASSERT_TRUE(b.IsEmpty());
}

/** @brief Test case: a payload above the maximum is rejected. */
TEST(a_payload_above_the_maximum_is_rejected) {
    LdnProxyBuffer b;
    ASSERT_FALSE(
        write_packet(b, 1, LdnProxyBuffer::MaxPacketDataSize + 1, 0x01));
    ASSERT_TRUE(b.IsEmpty());
    ASSERT_TRUE(write_packet(b, 1, LdnProxyBuffer::MaxPacketDataSize, 0x01));
}

/** @brief Test case: the queue refuses packets beyond its capacity. */
TEST(the_queue_refuses_packets_beyond_its_capacity) {
    LdnProxyBuffer b;
    for (size_t i = 0; i < LdnProxyBuffer::MaxQueuedPackets; ++i) {
        ASSERT_TRUE(write_packet(b, static_cast<uint32_t>(i), 4, 0x01));
    }
    ASSERT_FALSE(write_packet(b, 99, 4, 0x01));
    ASSERT_EQ(b.GetPendingCount(), LdnProxyBuffer::MaxQueuedPackets);
}

/**
 * @brief Test case: the data area refuses a packet that does not fit anywhere.
 */
TEST(the_data_area_refuses_a_packet_that_does_not_fit_anywhere) {
    LdnProxyBuffer b;
    const size_t big = LdnProxyBuffer::MaxPacketDataSize;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(write_packet(b, static_cast<uint32_t>(i), big, 0x01));
    }
    // 256 bytes remain at the end and nothing was consumed yet.
    ASSERT_FALSE(write_packet(b, 50, big, 0x02));
    ASSERT_EQ(b.GetPendingCount(), static_cast<size_t>(4));
}

/** @brief Test case: reset empties the queue and the byte counters. */
TEST(reset_empties_the_queue_and_the_byte_counters) {
    LdnProxyBuffer b;
    write_packet(b, 1, 100, 0x01);
    write_packet(b, 2, 100, 0x02);
    b.Reset();
    ASSERT_TRUE(b.IsEmpty());
    ASSERT_EQ(b.GetUsedBytes(), static_cast<size_t>(0));
    ASSERT_TRUE(write_packet(b, 3, 10, 0x03));
}

/**
 * @brief Test case: used bytes follow the unread data when the writer has
 *   wrapped.
 */
TEST(used_bytes_follow_the_unread_data_when_the_writer_has_wrapped) {
    LdnProxyBuffer b;
    const size_t big = LdnProxyBuffer::MaxPacketDataSize;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(write_packet(b, static_cast<uint32_t>(i), big, 0x01));
    }
    ProxyDataHeader h;
    size_t size = 0;
    u8 out[LdnProxyBuffer::MaxPacketDataSize];
    ASSERT_TRUE(b.Read(h, out, size, sizeof(out)));
    ASSERT_TRUE(b.Read(h, out, size, sizeof(out)));
    // Wrap: only 256 bytes remain at the end, so the writer restarts at
    // offset 0 while the oldest unread data starts at 2 * big.
    ASSERT_TRUE(write_packet(b, 10, big, 0x0A));
    ASSERT_EQ(b.GetUsedBytes(), LdnProxyBuffer::BufferSize - 2 * big + big);
}

/**
 * @brief Test case: unread packets are never overwritten after the writer
 *   wraps.
 */
TEST(unread_packets_are_never_overwritten_after_the_writer_wraps) {
    LdnProxyBuffer b;
    const size_t big = LdnProxyBuffer::MaxPacketDataSize;
    ProxyDataHeader h;
    size_t size = 0;
    u8 out[LdnProxyBuffer::MaxPacketDataSize];

    for (uint32_t i = 1; i <= 4; ++i) {
        ASSERT_TRUE(write_packet(b, i, big, static_cast<u8>(i)));
    }
    ASSERT_TRUE(b.Read(h, out, size, sizeof(out)));  // packet 1
    ASSERT_TRUE(b.Read(h, out, size, sizeof(out)));  // packet 2

    // Packets 3 and 4 are still unread. Keep writing: every packet that is
    // accepted must come back intact, in order.
    uint32_t next = 5;
    while (next < 5 + 6 && write_packet(b, next, big, static_cast<u8>(next))) {
        ++next;
    }

    for (uint32_t expected = 3; expected < next; ++expected) {
        ASSERT_TRUE(b.Read(h, out, size, sizeof(out)));
        ASSERT_EQ(h.info.source_ipv4, expected);
        ASSERT_EQ(out[0], static_cast<u8>(expected));
        ASSERT_EQ(out[big - 1], static_cast<u8>(expected));
    }
    ASSERT_TRUE(b.IsEmpty());
}

/**
 * @brief Runs the whole suite; the exit code is non-zero if any test failed.
 *
 * @return 0 when every test passed, non-zero otherwise
 */
int main() {
    return test_report("LdnProxyBuffer Tests");
}
