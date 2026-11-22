/**
 * @file sf_mitm_dispatch.h
 * @brief Host stand-in for Atmosphere's serviceMitmDispatch* forwarding macros.
 *
 * The real macros marshal an IPC request to the forwarded bsd:u session. On
 * the host there is no session: each call is handed to a hook the test
 * installs, together with the raw in/out structs and the buffer descriptors,
 * so a test can play the part of the real service (fill the out struct, fail
 * with any Result, read the buffers the MITM passed) and inspect what the
 * MITM forwarded.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include "stratosphere.hpp"

namespace host_mitm {

constexpr std::size_t MAX_BUFFERS = 8;

/// One buffer of a forwarded call.
struct Buffer {
    const void* ptr = nullptr;
    std::size_t size = 0;
};

/// The designated options of a serviceMitmDispatch* call, in libnx order.
struct Options {
    bool in_send_pid = false;
    std::uint32_t buffer_attrs[MAX_BUFFERS] = {};
    Buffer buffers[MAX_BUFFERS] = {};
    std::uint32_t in_num_handles = 0;
    std::uint32_t in_handles[4] = {};
    std::uint64_t override_pid = 0;
};

/// What a hook receives for one forwarded call.
struct Call {
    ::Service* service;
    std::uint32_t command;
    const void* in;
    std::size_t in_size;
    void* out;
    std::size_t out_size;
    const Options* options;
};

/// Plays the real service. Null: every call succeeds and leaves `out` alone.
using Hook = ams::Result (*)(const Call&);
inline Hook g_dispatch_hook = nullptr;

/// Number of calls forwarded since the last reset.
inline unsigned g_dispatch_count = 0;

inline ams::Result Dispatch(
    ::Service* service,
    std::uint32_t command,
    const void* in,
    std::size_t in_size,
    void* out,
    std::size_t out_size,
    const Options& options) {
    ++g_dispatch_count;
    if (g_dispatch_hook == nullptr) {
        return ams::ResultSuccess();
    }
    const Call call{service, command, in, in_size, out, out_size, &options};
    return g_dispatch_hook(call);
}

}  // namespace host_mitm

#define serviceMitmDispatchInOut(_srv, _cmd, _in, _out, ...) \
    ::host_mitm::Dispatch(                                   \
        (_srv),                                              \
        (_cmd),                                              \
        &(_in),                                              \
        sizeof(_in),                                         \
        &(_out),                                             \
        sizeof(_out),                                        \
        ::host_mitm::Options{__VA_ARGS__})
#define serviceMitmDispatchIn(_srv, _cmd, _in, ...) \
    ::host_mitm::Dispatch(                          \
        (_srv),                                     \
        (_cmd),                                     \
        &(_in),                                     \
        sizeof(_in),                                \
        nullptr,                                    \
        0,                                          \
        ::host_mitm::Options{__VA_ARGS__})
#define serviceMitmDispatchOut(_srv, _cmd, _out, ...) \
    ::host_mitm::Dispatch(                            \
        (_srv),                                       \
        (_cmd),                                       \
        nullptr,                                      \
        0,                                            \
        &(_out),                                      \
        sizeof(_out),                                 \
        ::host_mitm::Options{__VA_ARGS__})
#define serviceMitmDispatch(_srv, _cmd, ...) \
    ::host_mitm::Dispatch(                   \
        (_srv),                              \
        (_cmd),                              \
        nullptr,                             \
        0,                                   \
        nullptr,                             \
        0,                                   \
        ::host_mitm::Options{__VA_ARGS__})
