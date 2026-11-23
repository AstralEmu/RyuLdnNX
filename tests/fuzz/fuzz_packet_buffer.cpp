/**
 * @file fuzz_packet_buffer.cpp
 * @brief libFuzzer target for the TCP stream reassembly of the wire protocol
 *
 * Feeds arbitrary bytes, split in arbitrary chunks, to PacketBuffer and drains
 * it the way the receive thread does: header peek, packet extraction and
 * resynchronisation on invalid data. The target must never crash, read out of
 * bounds or leave the buffer in an inconsistent state, whatever the peer sends.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#include <cstddef>
#include <cstdint>

#include "protocol/packet_buffer.hpp"

namespace {

/// Largest packet copied out of the buffer (header + maximum payload)
constexpr size_t kOutputSize = 0x10000;

}  // namespace

/**
 * @brief libFuzzer entry point
 * @param data Fuzzer-provided input
 * @param size Size of the input in bytes
 * @return Always 0 (libFuzzer convention)
 */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static ryu_ldn::protocol::PacketBuffer<> buffer;
    static uint8_t out[kOutputSize];

    buffer.reset();
    if (size == 0) {
        return 0;
    }

    // The first byte selects the chunk size so that headers split across
    // several recv() calls are exercised as well.
    const size_t chunk = static_cast<size_t>(data[0]) + 1;
    size_t offset = 1;
    while (offset < size) {
        const size_t n = (size - offset < chunk) ? size - offset : chunk;
        if (buffer.append(data + offset, n) !=
            ryu_ldn::protocol::BufferResult::Success) {
            buffer.discard_until_valid();
            buffer.reset();
        }
        offset += n;

        size_t packet_size = 0;
        while (buffer.has_complete_packet()) {
            if (buffer.extract_packet(out, sizeof(out), packet_size) !=
                ryu_ldn::protocol::BufferResult::Success) {
                break;
            }
        }
        static_cast<void>(buffer.peek_packet_type());
        static_cast<void>(buffer.discard_until_valid());
    }
    return 0;
}
