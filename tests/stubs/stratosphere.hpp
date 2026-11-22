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
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    constexpr Result(std::uint32_t value) : m_value(value) {}
    constexpr bool IsSuccess() const { return m_value == 0; }
    constexpr bool IsFailure() const { return m_value != 0; }
    constexpr std::uint32_t GetValue() const { return m_value; }

private:
    std::uint32_t m_value;
};

constexpr Result ResultSuccess() { return Result(); }

namespace os {

/// Test hook: runs before every lock of a RECURSIVE Mutex (the lock of a
/// plain one is untouched), on the locking thread, before it takes the lock
/// (null = off). Same contract as the other g_host_before_* hooks below.
inline void (*g_host_before_recursive_lock)() = nullptr;

/// Mutex with the Stratosphere constructor signature (recursive flag).
/// Satisfies Lockable, so std::scoped_lock / std::unique_lock work on it.
class Mutex {
public:
    explicit Mutex(bool recursive) : m_recursive(recursive) {}

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() {
        if (m_recursive) {
            if (g_host_before_recursive_lock) { g_host_before_recursive_lock(); }
            m_recursive_mutex.lock();
        } else {
            m_mutex.lock();
        }
    }

    bool try_lock() {
        return m_recursive ? m_recursive_mutex.try_lock() : m_mutex.try_lock();
    }

    /// Stratosphere spelling of lock()/unlock()
    void Lock() { lock(); }
    void Unlock() { unlock(); }

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
    static TimeSpan FromSeconds(std::uint64_t s) { return TimeSpan(s * 1000); }
    static TimeSpan FromMilliSeconds(std::uint64_t ms) { return TimeSpan(ms); }
    /// The host clock has a millisecond resolution, like the rest of this stand-in.
    static TimeSpan FromNanoSeconds(std::int64_t ns) { return TimeSpan(static_cast<std::uint64_t>(ns > 0 ? ns / 1000000 : 0)); }
    std::uint64_t GetMilliSeconds() const { return m_ms; }
    std::int64_t GetNanoSeconds() const { return static_cast<std::int64_t>(m_ms) * 1000000; }

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

/// Host shortcut: real waits last (requested ms / divisor), so a test of a
/// multi-second timeout does not take multiple seconds. 1 = faithful.
inline std::uint64_t g_host_wait_divisor = 1;

/// Test hooks: run on the calling thread right before the operation, with no
/// stand-in lock held, so a test can script what happens concurrently at that
/// exact point of the code under test (null = off).
inline void (*g_host_before_event_clear)() = nullptr;
inline void (*g_host_before_timed_wait_any)() = nullptr;

/// Nanoseconds added to armGetSystemTick(): a test moves the clock forward to
/// cross a deadline without waiting for it.
inline std::atomic<std::uint64_t> g_host_tick_skew_ns{0};

/// Mutex and condition variable shared by every multi wait: a Signal() of any
/// event wakes the waiters, which then look at their own holders.
inline std::mutex& MultiWaitMutex() { static std::mutex m; return m; }
inline std::condition_variable& MultiWaitCv() { static std::condition_variable cv; return cv; }

/// Event (os::Event): ManualClear stays signaled until Clear(); AutoClear is
/// consumed by the wait that observes it.
class Event {
public:
    explicit Event(EventClearMode mode) : m_mode(mode) {}
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    void Signal() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_signaled = true;
            m_generation++;
            m_cv.notify_all();
        }
        // Wake the multi waiters: taking their mutex first guarantees one that
        // checked the event just before the signal is already blocked in its wait.
        { std::lock_guard<std::mutex> lock(MultiWaitMutex()); }
        MultiWaitCv().notify_all();
    }
    void Clear() {
        if (g_host_before_event_clear) { g_host_before_event_clear(); }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_signaled = false;
    }
    /// Event object handed to the multi wait holders (the host event itself).
    Event* GetBase() { return this; }
    /// Consumes the signal if there is one (what a satisfied wait does).
    bool TryConsume() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_signaled) { return false; }
        Consume();
        return true;
    }
    void Wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        const auto generation = m_generation;
        m_cv.wait(lock, [&] { return m_signaled || m_generation != generation; });
        Consume();
    }
    bool TimedWait(TimeSpan timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        const auto ms = timeout.GetMilliSeconds() / g_host_wait_divisor;
        const auto generation = m_generation;
        if (!m_cv.wait_for(lock, std::chrono::milliseconds(ms),
                           [&] { return m_signaled || m_generation != generation; })) {
            return false;
        }
        Consume();
        return true;
    }

private:
    void Consume() {
        if (m_mode == EventClearMode_AutoClear) { m_signaled = false; }
    }
    EventClearMode m_mode;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_signaled = false;
    /// A Signal() releases every waiter already blocked, even if Clear() ran
    /// before they woke (kernel semantics).
    unsigned long m_generation = 0;
};

/// Holder of one event inside a MultiWaitType (os::MultiWaitHolderType).
struct MultiWaitHolderType {
    Event* event = nullptr;
};

/// Set of holders waited on together (os::MultiWaitType).
struct MultiWaitType {
    std::vector<MultiWaitHolderType*> holders;
};

inline void InitializeMultiWait(MultiWaitType* multi_wait) { multi_wait->holders.clear(); }
inline void FinalizeMultiWait(MultiWaitType* multi_wait) { multi_wait->holders.clear(); }
inline void InitializeMultiWaitHolder(MultiWaitHolderType* holder, Event* event) { holder->event = event; }
inline void FinalizeMultiWaitHolder(MultiWaitHolderType* holder) { holder->event = nullptr; }
inline void LinkMultiWaitHolder(MultiWaitType* multi_wait, MultiWaitHolderType* holder) {
    multi_wait->holders.push_back(holder);
}
inline void UnlinkAllMultiWaitHolder(MultiWaitType* multi_wait) { multi_wait->holders.clear(); }

/// Waits until one linked event is signaled (and consumes it, like the kernel
/// does for an auto-clear event) or the timeout expires: nullptr then.
inline MultiWaitHolderType* TimedWaitAny(MultiWaitType* multi_wait, TimeSpan timeout) {
    if (g_host_before_timed_wait_any) { g_host_before_timed_wait_any(); }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout.GetMilliSeconds() / g_host_wait_divisor);
    std::unique_lock<std::mutex> lock(MultiWaitMutex());
    for (;;) {
        for (auto* holder : multi_wait->holders) {
            if (holder->event->TryConsume()) { return holder; }
        }
        if (MultiWaitCv().wait_until(lock, deadline) == std::cv_status::timeout) {
            for (auto* holder : multi_wait->holders) {
                if (holder->event->TryConsume()) { return holder; }
            }
            return nullptr;
        }
    }
}

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

/// Condition variable working with os::Mutex (Stratosphere contract: the mutex
/// passed to the wait is held by the caller and released while waiting).
class ConditionVariable {
public:
    ConditionVariable() = default;
    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    void Broadcast() { m_cv.notify_all(); }
    void Signal() { m_cv.notify_one(); }
    void Wait(Mutex& mutex) { m_cv.wait(mutex); }
    void TimedWait(Mutex& mutex, TimeSpan timeout) {
        m_cv.wait_for(mutex, std::chrono::milliseconds(timeout.GetMilliSeconds() / g_host_wait_divisor));
    }

private:
    std::condition_variable_any m_cv;
};

/// Thread stack alignment of the kernel (page size)
constexpr std::size_t ThreadStackAlignment = 0x1000;

/// Thread object: the entry point and its argument are kept by CreateThread
/// and the host thread is started by StartThread, like the kernel does.
struct ThreadType {
    void (*entry)(void*) = nullptr;
    void* arg = nullptr;
    const char* name = nullptr;
    std::thread thread;

    ThreadType() = default;
    ThreadType(const ThreadType&) = delete;
    ThreadType& operator=(const ThreadType&) = delete;
    /// A kernel thread that already exited needs no join; the host must
    /// reclaim it. Only reached for an exited thread, otherwise it waits.
    ~ThreadType() { if (thread.joinable()) { thread.join(); } }
};

/// Failure injection: the next CreateThread fails with this result (0 = off).
inline std::uint32_t g_host_create_thread_result = 0;

inline Result CreateThread(ThreadType* out, void (*entry)(void*), void* arg,
                           void* /*stack*/, std::size_t /*stack_size*/, int /*priority*/) {
    if (g_host_create_thread_result != 0) { return Result(g_host_create_thread_result); }
    if (out->thread.joinable()) { out->thread.join(); }
    out->entry = entry;
    out->arg = arg;
    return ResultSuccess();
}
/// Test hook: runs on the starting thread right before the new thread starts,
/// with the name given to SetThreadNamePointer ("" when none).
inline void (*g_host_before_start_thread)(const char*) = nullptr;
inline void StartThread(ThreadType* thread) {
    if (g_host_before_start_thread) { g_host_before_start_thread(thread->name != nullptr ? thread->name : ""); }
    thread->thread = std::thread(thread->entry, thread->arg);
}
inline void WaitThread(ThreadType* thread) { if (thread->thread.joinable()) { thread->thread.join(); } }
inline void DestroyThread(ThreadType*) {}

/// Lowest user-mode priority of the kernel (the host ignores priorities)
constexpr int LowestThreadPriority = 63;
inline void SetThreadNamePointer(ThreadType* thread, const char* name) { thread->name = name; }

} // namespace os

namespace svc {

/// Sleep of the calling thread, in nanoseconds; shortened by the same host
/// divisor as the timed waits so a test of a long period stays fast.
inline void SleepThread(std::int64_t ns) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns / static_cast<std::int64_t>(os::g_host_wait_divisor)));
}

} // namespace svc

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

namespace ncm {

/// Program (title) id of a process.
struct ProgramId {
    std::uint64_t value = 0;
    constexpr bool operator==(const ProgramId&) const = default;
};

} // namespace ncm

namespace os {

/// Process id of a process.
struct ProcessId {
    std::uint64_t value = 0;
};

} // namespace os

namespace sm {

/// What Atmosphere tells a MITM service about the client it serves.
struct MitmProcessInfo {
    os::ProcessId process_id{};
    ncm::ProgramId program_id{};
    std::uint32_t override_status = 0;
};

} // namespace sm

} // namespace ams

/// Forwarded session of the original service (libnx).
struct Service {
    std::uint32_t session = 0;
};

namespace ams {

namespace sf {

/// Marker bases used by IPC structs; they carry no data on the host.
struct LargeData {};

/// Process id of the IPC client, as carried by sf::ClientProcessId.
struct ClientProcessId {
    os::ProcessId process_id{};
    constexpr os::ProcessId GetValue() const { return process_id; }
};
struct PrefersPointerTransferMode {};

/// Handle copied to the client (sf::CopyHandle).
struct CopyHandle {
    std::uint32_t handle = 0;
    bool managed = false;
    CopyHandle() = default;
    CopyHandle(std::uint32_t h, bool m) : handle(h), managed(m) {}
};

/// Output parameter of an IPC command: a reference the handler writes through.
template<class T>
class Out {
public:
    explicit Out(T& target) : m_target(&target) {}
    T& operator*() const { return *m_target; }
    T* operator->() const { return m_target; }
    template<class... A>
    void SetValue(A&&... args) const { *m_target = T(std::forward<A>(args)...); }

private:
    T* m_target;
};

/// Output array / buffer the handler fills (sf::OutArray, sf::OutBuffer, ...).
template<class T>
class OutArray {
public:
    OutArray(T* data, std::size_t count) : m_data(data), m_count(count) {}
    std::size_t GetSize() const { return m_count; }
    T* GetPointer() const { return m_data; }
    T& operator[](std::size_t i) const { return m_data[i]; }

private:
    T* m_data;
    std::size_t m_count;
};
template<class T> using OutAutoSelectArray = OutArray<T>;
using OutBuffer = OutArray<std::uint8_t>;
using OutAutoSelectBuffer = OutArray<std::uint8_t>;

/// Input buffer the handler reads (sf::InBuffer, sf::InPointerBuffer, ...).
class InBuffer {
public:
    InBuffer(const std::uint8_t* data, std::size_t size) : m_data(data), m_size(size) {}
    std::size_t GetSize() const { return m_size; }
    const std::uint8_t* GetPointer() const { return m_data; }

private:
    const std::uint8_t* m_data;
    std::size_t m_size;
};
using InAutoSelectBuffer = InBuffer;
using InPointerBuffer = InBuffer;

/// Reference to a service object created by the MITM (sf::SharedPointer). The
/// real one is type erased too: the interface is only a tag, so the host keeps
/// the implementation behind a void pointer and hands it back to the test.
template<class Interface>
class SharedPointer {
public:
    SharedPointer() = default;
    template<class Impl>
    explicit SharedPointer(std::shared_ptr<Impl> object) : m_object(object), m_raw(object.get()) {}
    explicit operator bool() const { return m_raw != nullptr; }
    template<class Impl> Impl* GetImpl() const { return static_cast<Impl*>(m_raw); }

private:
    std::shared_ptr<void> m_object;
    void* m_raw = nullptr;
};

template<class Interface, class Impl, class... Args>
SharedPointer<Interface> CreateSharedObjectEmplaced(Args&&... args) {
    return SharedPointer<Interface>(std::make_shared<Impl>(std::forward<Args>(args)...));
}

/// Base of a MITM service: keeps the forwarding session and the client info.
class MitmServiceImplBase {
public:
    MitmServiceImplBase(std::shared_ptr<::Service>&& forward, const sm::MitmProcessInfo& client)
        : m_forward_service(std::move(forward)), m_client_info(client) {}

protected:
    std::shared_ptr<::Service> m_forward_service;
    sm::MitmProcessInfo m_client_info;
};

} // namespace sf

using os::TimeSpan;

namespace impl {
template<class... Ts> constexpr void UnusedImpl(const Ts&...) {}
}

} // namespace ams

/// libnx system tick: the host counts nanoseconds, so a tick is one nanosecond
inline std::uint64_t armGetSystemTick() {
    return ams::os::g_host_tick_skew_ns.load() + static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline std::uint64_t armTicksToNs(std::uint64_t ticks) { return ticks; }

/// Aborts the process on a failed Result, like the SDK macro does
#define R_ABORT_UNLESS(expr) \
    do { if (::ams::Result r_abort_ = (expr); r_abort_.IsFailure()) { std::abort(); } } while (false)

#define AMS_UNUSED(...) ::ams::impl::UnusedImpl(__VA_ARGS__)
#define R_SUCCEED() return ::ams::ResultSuccess()
#define R_FAILED(expr) ((expr).IsFailure())
#define R_THROW(res) return (res)
#define R_RETURN(res) return (res)
#define R_UNLESS(cond, res) do { if (!(cond)) { R_RETURN(res); } } while (false)
#define R_SUCCEEDED(expr) ((expr).IsSuccess())
/// Same bit packing as the SDK: module in bits 0-8, description from bit 9
#define MAKERESULT(module, description) \
    (static_cast<std::uint32_t>(((module) & 0x1FF) | (((description) & 0x1FFF) << 9)))

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
    namespace NS { class NAME; template<class T> concept Is##NAME = requires { METHODS(T, 0) }; }
#define AMS_SF_DEFINE_MITM_INTERFACE(NS, NAME, METHODS, ID) AMS_SF_DEFINE_INTERFACE(NS, NAME, METHODS, ID)

/// libnx nifm: the host serves whatever the test put in these variables.
inline std::uint32_t g_host_nifm_result = 0;
inline std::uint32_t g_host_nifm_address = 0;
inline std::uint32_t g_host_nifm_netmask = 0;
inline std::uint32_t nifmGetCurrentIpConfigInfo(std::uint32_t* address, std::uint32_t* netmask,
                                                std::uint32_t* gateway, std::uint32_t* primary_dns,
                                                std::uint32_t* secondary_dns) {
    if (g_host_nifm_result != 0) { return g_host_nifm_result; }
    *address = g_host_nifm_address;
    *netmask = g_host_nifm_netmask;
    *gateway = 0;
    *primary_dns = 0;
    *secondary_dns = 0;
    return 0;
}

/// libnx svcSleepThread, shortened by the same divisor as the timed waits
inline void svcSleepThread(std::int64_t ns) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns / static_cast<std::int64_t>(ams::os::g_host_wait_divisor)));
}
