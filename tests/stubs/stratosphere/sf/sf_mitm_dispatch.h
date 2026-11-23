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

/// Capacity of the buffer descriptor arrays in Options.
constexpr std::size_t MAX_BUFFERS = 8;

/// One buffer of a forwarded call.
struct Buffer {
    /// Start of the buffer memory (not owned).
    const void* ptr = nullptr;
    /// Buffer length in bytes.
    std::size_t size = 0;
};

/// The designated options of a serviceMitmDispatch* call, in libnx order.
struct Options {
    /// Whether the call asked libnx to send the caller's process id.
    bool in_send_pid = false;
    /// libnx buffer attribute flags (SfBufferAttr), one per entry of `buffers`.
    std::uint32_t buffer_attrs[MAX_BUFFERS] = {};
    /// Buffers attached to the request, in the order given by the caller.
    Buffer buffers[MAX_BUFFERS] = {};
    /// Number of valid entries in `in_handles`.
    std::uint32_t in_num_handles = 0;
    /// Kernel handles copied into the request.
    std::uint32_t in_handles[4] = {};
    /// Process id substituted for the caller's when `in_send_pid` is set.
    std::uint64_t override_pid = 0;
};

/// What a hook receives for one forwarded call.
struct Call {
    /// Forwarded service session the call was issued on.
    ::Service* service;
    /// IPC command id (the `_cmd` argument).
    std::uint32_t command;
    /// Raw input struct, or nullptr when the call has no input.
    const void* in;
    /// Size in bytes of `in` (0 when there is none).
    std::size_t in_size;
    /// Raw output struct a hook may fill, or nullptr when there is none.
    void* out;
    /// Size in bytes of `out` (0 when there is none).
    std::size_t out_size;
    /// Options of the call; valid only for the duration of the hook.
    const Options* options;
};

/// Plays the real service. The hook answers with the raw result code, which is
/// what the forwarded IPC call returns on the wire; Dispatch wraps it in an
/// ams::Result.
using Hook = std::uint32_t (*)(const Call&);

/**
 * @brief Hook used when the test installs none: succeeds, leaves `out` alone.
 * @param call Forwarded call, ignored.
 * @return Success code.
 */
inline std::uint32_t Succeed(const Call& call) {
    (void)call;
    return 0;
}

/// Hook installed by the test; Succeed when none is installed. It starts on a
/// real function on purpose: the call then always reaches code that may read
/// the forwarded structs, which is what the real macro does.
inline Hook g_dispatch_hook = &Succeed;

/// Number of calls forwarded since the last reset.
inline unsigned g_dispatch_count = 0;

/**
 * @brief Count one forwarded call and run the installed hook.
 * @param service Session the call is forwarded on.
 * @param command IPC command id.
 * @param in Raw input struct, or nullptr.
 * @param in_size Size of `in` in bytes.
 * @param out Raw output struct, or nullptr.
 * @param out_size Size of `out` in bytes.
 * @param options Designated options of the call.
 * @return Result of g_dispatch_hook.
 */
inline ams::Result Dispatch(
    ::Service* service,
    std::uint32_t command,
    const void* in,
    std::size_t in_size,
    void* out,
    std::size_t out_size,
    const Options& options) {
    ++g_dispatch_count;
    const Call call{service, command, in, in_size, out, out_size, &options};
    return ams::Result(g_dispatch_hook(call));
}

}  // namespace host_mitm

/**
 * @brief Host stand-in forwarding a call with an input and an output struct.
 * @param _srv Service session pointer.
 * @param _cmd IPC command id.
 * @param _in Input lvalue; its address and sizeof are passed on.
 * @param _out Output lvalue; its address and sizeof are passed on.
 * @param ... Designated initializers for host_mitm::Options.
 */
#define serviceMitmDispatchInOut(_srv, _cmd, _in, _out, ...) \
    ::host_mitm::Dispatch(                                   \
        (_srv),                                              \
        (_cmd),                                              \
        &(_in),                                              \
        sizeof(_in),                                         \
        &(_out),                                             \
        sizeof(_out),                                        \
        ::host_mitm::Options{__VA_ARGS__})
/**
 * @brief Host stand-in forwarding a call with an input struct only.
 * @param _srv Service session pointer.
 * @param _cmd IPC command id.
 * @param _in Input lvalue; its address and sizeof are passed on.
 * @param ... Designated initializers for host_mitm::Options.
 */
#define serviceMitmDispatchIn(_srv, _cmd, _in, ...) \
    ::host_mitm::Dispatch(                          \
        (_srv),                                     \
        (_cmd),                                     \
        &(_in),                                     \
        sizeof(_in),                                \
        nullptr,                                    \
        0,                                          \
        ::host_mitm::Options{__VA_ARGS__})
/**
 * @brief Host stand-in forwarding a call with an output struct only.
 * @param _srv Service session pointer.
 * @param _cmd IPC command id.
 * @param _out Output lvalue; its address and sizeof are passed on.
 * @param ... Designated initializers for host_mitm::Options.
 */
#define serviceMitmDispatchOut(_srv, _cmd, _out, ...) \
    ::host_mitm::Dispatch(                            \
        (_srv),                                       \
        (_cmd),                                       \
        nullptr,                                      \
        0,                                            \
        &(_out),                                      \
        sizeof(_out),                                 \
        ::host_mitm::Options{__VA_ARGS__})
/**
 * @brief Host stand-in forwarding a call with neither input nor output.
 * @param _srv Service session pointer.
 * @param _cmd IPC command id.
 * @param ... Designated initializers for host_mitm::Options.
 */
#define serviceMitmDispatch(_srv, _cmd, ...) \
    ::host_mitm::Dispatch(                   \
        (_srv),                              \
        (_cmd),                              \
        nullptr,                             \
        0,                                   \
        nullptr,                             \
        0,                                   \
        ::host_mitm::Options{__VA_ARGS__})
