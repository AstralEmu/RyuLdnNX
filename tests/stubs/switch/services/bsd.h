/**
 * @file bsd.h
 * @brief Host stand-in for the libnx pieces of <switch/services/bsd.h> the BSD MITM uses.
 */
#pragma once

#include <cstdint>
#include <poll.h>

using Handle = std::uint32_t;
constexpr Handle INVALID_HANDLE = 0xFFFFFFFFu;

/// Buffer attributes of a forwarded call (libnx SfBufferAttr).
enum SfBufferAttr : std::uint32_t {
    SfBufferAttr_In = 1u << 0,
    SfBufferAttr_Out = 1u << 1,
    SfBufferAttr_HipcMapAlias = 1u << 2,
    SfBufferAttr_HipcPointer = 1u << 3,
    SfBufferAttr_FixedSize = 1u << 4,
    SfBufferAttr_HipcAutoSelect = 1u << 5,
    SfBufferAttr_HipcMapTransferAllowsNonSecure = 1u << 6,
    SfBufferAttr_HipcMapTransferAllowsNonDevice = 1u << 7,
};
