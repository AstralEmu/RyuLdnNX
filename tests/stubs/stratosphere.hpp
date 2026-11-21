/**
 * @file stratosphere.hpp
 * @brief Host-side stand-in for Atmosphere's <stratosphere.hpp>
 *
 * Lets sysmodule sources that only need a few Stratosphere primitives be
 * compiled and measured by the host unit tests. It is on the include path of
 * tests/Makefile only (-Istubs); the sysmodule build never sees it, so it
 * cannot change what ships on the console.
 *
 * Grow it only with what the code under test really uses, and keep each
 * primitive behaviourally faithful (same contract, host implementation).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <array>
#include <mutex>
#include <string>

// libnx scalar aliases (global in the real toolchain)
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

namespace ams {

/// Result code: 0 is success, like the real ams::Result (module/description
/// packing is not modelled, none of the host-tested code depends on it).
class Result {
public:
    constexpr Result() : m_value(0) {}
    explicit constexpr Result(std::uint32_t value) : m_value(value) {}
    constexpr bool IsSuccess() const { return m_value == 0; }
    constexpr bool IsFailure() const { return m_value != 0; }
    constexpr std::uint32_t GetValue() const { return m_value; }

private:
    std::uint32_t m_value;
};

constexpr Result ResultSuccess() { return Result(); }

namespace os {

/// Mutex with the Stratosphere constructor signature (recursive flag).
/// Satisfies Lockable, so std::scoped_lock / std::unique_lock work on it.
class Mutex {
public:
    explicit Mutex(bool recursive) : m_recursive(recursive) {}

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() {
        if (m_recursive) {
            m_recursive_mutex.lock();
        } else {
            m_mutex.lock();
        }
    }

    bool try_lock() {
        return m_recursive ? m_recursive_mutex.try_lock() : m_mutex.try_lock();
    }

    void unlock() {
        if (m_recursive) {
            m_recursive_mutex.unlock();
        } else {
            m_mutex.unlock();
        }
    }

private:
    bool m_recursive;
    std::mutex m_mutex;
    std::recursive_mutex m_recursive_mutex;
};

/// Non-recursive mutex of the SDK (Lockable)
class SdkMutex {
public:
    SdkMutex() = default;
    SdkMutex(const SdkMutex&) = delete;
    SdkMutex& operator=(const SdkMutex&) = delete;

    void lock() { m_mutex.lock(); }
    bool try_lock() { return m_mutex.try_lock(); }
    void unlock() { m_mutex.unlock(); }

private:
    std::mutex m_mutex;
};

/// Host clock: tests set the "system tick" directly, in milliseconds.
inline std::uint64_t g_host_tick_ms = 0;

class TimeSpan {
public:
    explicit TimeSpan(std::uint64_t ms) : m_ms(ms) {}
    std::uint64_t GetMilliSeconds() const { return m_ms; }

private:
    std::uint64_t m_ms;
};

struct Tick {
    std::uint64_t ms;
};

inline Tick GetSystemTick() { return Tick{g_host_tick_ms}; }
inline TimeSpan ConvertToTimeSpan(Tick tick) { return TimeSpan(tick.ms); }

using NativeHandle = std::uint32_t;

enum EventClearMode {
    EventClearMode_ManualClear,
    EventClearMode_AutoClear,
};

/// Event with the Stratosphere contract: Signal() latches it, and the
/// readable handle is stable and distinct per instance. The host keeps the
/// number of signals so tests can observe them.
class SystemEvent {
public:
    SystemEvent(EventClearMode mode, bool inter_process)
        : m_mode(mode), m_inter_process(inter_process), m_handle(++s_next_handle) {}

    SystemEvent(const SystemEvent&) = delete;
    SystemEvent& operator=(const SystemEvent&) = delete;

    void Signal() { ++m_signal_count; }
    NativeHandle GetReadableHandle() const { return m_handle; }

    unsigned SignalCount() const { return m_signal_count; }

private:
    inline static NativeHandle s_next_handle = 0;
    EventClearMode m_mode;
    bool m_inter_process;
    NativeHandle m_handle;
    unsigned m_signal_count = 0;
};

} // namespace os

namespace fs {

/// Host file system stand-in: one in-memory file, with failure injection.
/// Tests fill g_host_fs before calling the code under test.
struct HostFs {
    std::string content;          ///< bytes served by ReadFile
    bool open_fails = false;      ///< OpenFile returns a failure
    bool size_fails = false;      ///< GetFileSize returns a failure
    bool read_fails = false;      ///< ReadFile returns a failure
    s64 size_override = -1;       ///< >= 0: GetFileSize reports this instead
    int open_count = 0;
    int close_count = 0;
};
inline HostFs g_host_fs;

struct FileHandle { int id = 0; };
constexpr int OpenMode_Read = 1;

inline Result OpenFile(FileHandle* out, const char*, int) {
    if (g_host_fs.open_fails) { return Result(0x202); }
    ++g_host_fs.open_count;
    out->id = g_host_fs.open_count;
    return ResultSuccess();
}

inline Result GetFileSize(s64* out, FileHandle) {
    if (g_host_fs.size_fails) { return Result(0x203); }
    *out = g_host_fs.size_override >= 0 ? g_host_fs.size_override
                                        : static_cast<s64>(g_host_fs.content.size());
    return ResultSuccess();
}

inline Result ReadFile(std::size_t* out_read, FileHandle, s64 offset, void* buffer, std::size_t size) {
    if (g_host_fs.read_fails) { return Result(0x204); }
    const auto total = static_cast<s64>(g_host_fs.content.size());
    s64 avail = total - offset;
    if (avail < 0) { avail = 0; }
    const std::size_t n = std::min(size, static_cast<std::size_t>(avail));
    std::memcpy(buffer, g_host_fs.content.data() + offset, n);
    *out_read = n;
    return ResultSuccess();
}

inline void CloseFile(FileHandle) { ++g_host_fs.close_count; }

} // namespace fs

namespace sf {

/// Marker bases used by IPC structs; they carry no data on the host.
struct LargeData {};

/// Process id of the IPC client, as carried by sf::ClientProcessId.
struct ClientProcessId {
    std::uint64_t value = 0;
    constexpr std::uint64_t GetValue() const { return value; }
};
struct PrefersPointerTransferMode {};

/// Output parameter of an IPC command: a reference the handler writes through.
template<class T>
class Out {
public:
    explicit Out(T& target) : m_target(&target) {}
    T& operator*() const { return *m_target; }
    T* operator->() const { return m_target; }

private:
    T* m_target;
};

} // namespace sf

} // namespace ams

#define R_SUCCEED() return ::ams::ResultSuccess()
#define R_FAILED(expr) ((expr).IsFailure())
#define R_THROW(res) return (res)

/// Result module/description: the host Result keeps the same bit packing as
/// the real one (module in bits 0-8, description from bit 9), so a code
/// defined here is non-zero and distinguishable from success.
#define R_DEFINE_NAMESPACE_RESULT_MODULE(NS, MOD) \
    namespace NS { inline constexpr std::uint32_t ResultModuleId = (MOD); }
#define R_DEFINE_ERROR_RESULT(NAME, DESC) \
    constexpr ::ams::Result Result##NAME() { return ::ams::Result(ResultModuleId | ((DESC) << 9)); }

/// Stand-in for the sf interface description macros. Real Stratosphere
/// generates a concept checking every method signature; the host version
/// checks that each listed method exists on the class (not its signature),
/// which is what the static_assert in the sources guards at build time.
#define AMS_SF_METHOD_INFO(C, H, ID, RET, NAME, ARGS, CALLARGS, ...) (void)&C::NAME;
#define AMS_SF_DEFINE_INTERFACE(NS, NAME, METHODS, ID) \
    namespace NS { template<class T> concept Is##NAME = requires { METHODS(T, 0) }; }
