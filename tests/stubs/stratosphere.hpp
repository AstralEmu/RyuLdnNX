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
/// libnx unsigned 8-bit integer alias
using u8 = std::uint8_t;
/// libnx unsigned 16-bit integer alias
using u16 = std::uint16_t;
/// libnx unsigned 32-bit integer alias
using u32 = std::uint32_t;
/// libnx unsigned 64-bit integer alias
using u64 = std::uint64_t;
/// libnx signed 8-bit integer alias
using s8 = std::int8_t;
/// libnx signed 16-bit integer alias
using s16 = std::int16_t;
/// libnx signed 32-bit integer alias
using s32 = std::int32_t;
/// libnx signed 64-bit integer alias
using s64 = std::int64_t;

namespace ams {

/// Result code: 0 is success, like the real ams::Result (module/description
/// packing is not modelled, none of the host-tested code depends on it).
class Result {
public:
    constexpr Result() : m_value(0) {}
    /**
     * @brief Builds a Result from a raw code
     *
     * @param value raw result code, 0 for success
     */
    constexpr Result(std::uint32_t value) : m_value(value) {}
    /**
     * @brief Whether the code is 0
     *
     * @return true on success
     */
    constexpr bool IsSuccess() const {
        return m_value == 0;
    }
    /**
     * @brief Whether the code is not 0
     *
     * @return true on failure
     */
    constexpr bool IsFailure() const {
        return m_value != 0;
    }
    /**
     * @brief Raw result code
     *
     * @return the code, 0 on success
     */
    constexpr std::uint32_t GetValue() const {
        return m_value;
    }

private:
    /// Raw result code
    std::uint32_t m_value;
};

/**
 * @brief Success result
 *
 * @return Result holding code 0
 */
constexpr Result ResultSuccess() {
    return Result();
}

namespace os {

/// Test hook: runs before every lock of a RECURSIVE Mutex (the lock of a
/// plain one is untouched), on the locking thread, before it takes the lock
/// (null = off). Same contract as the other g_host_before_* hooks below.
inline void (*g_host_before_recursive_lock)() = nullptr;

/// Test hook: runs before every lock of a NON-recursive Mutex, with the mutex
/// being locked, on the locking thread, before it takes the lock (null = off).
/// Lets a test stage the state another thread would have left between two
/// critical sections without needing a second thread.
inline void (*g_host_before_plain_lock)(const void* mutex) = nullptr;

/// Mutex with the Stratosphere constructor signature (recursive flag).
/// Satisfies Lockable, so std::scoped_lock / std::unique_lock work on it.
class Mutex {
public:
    /**
     * @brief Creates the mutex
     *
     * @param recursive true lets the owning thread lock it again
     */
    explicit Mutex(bool recursive) : m_recursive(recursive) {}

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    /// Locks the mutex, after running the matching g_host_before_* test hook
    void lock() {
        if (m_recursive) {
            if (g_host_before_recursive_lock) {
                g_host_before_recursive_lock();
            }
            m_recursive_mutex.lock();
        } else {
            if (g_host_before_plain_lock) {
                g_host_before_plain_lock(this);
            }
            m_mutex.lock();
        }
    }

    /**
     * @brief Tries to lock without blocking
     *
     * @return true if the lock was taken
     */
    bool try_lock() {
        return m_recursive ? m_recursive_mutex.try_lock() : m_mutex.try_lock();
    }

    /// Stratosphere spelling of lock()/unlock()
    void Lock() {
        lock();
    }
    /// Unlocks the mutex (same as unlock())
    void Unlock() {
        unlock();
    }

    /// Unlocks the mutex of the flavor chosen at construction
    void unlock() {
        if (m_recursive) {
            m_recursive_mutex.unlock();
        } else {
            m_mutex.unlock();
        }
    }

private:
    /// Selects which of the two backing mutexes is used
    bool m_recursive;
    /// Backing mutex when not recursive
    std::mutex m_mutex;
    /// Backing mutex when recursive
    std::recursive_mutex m_recursive_mutex;
};

/// Non-recursive mutex of the SDK (Lockable)
class SdkMutex {
public:
    SdkMutex() = default;
    SdkMutex(const SdkMutex&) = delete;
    SdkMutex& operator=(const SdkMutex&) = delete;

    /// Locks the mutex
    void lock() {
        m_mutex.lock();
    }
    /**
     * @brief Tries to lock without blocking
     *
     * @return true if the lock was taken
     */
    bool try_lock() {
        return m_mutex.try_lock();
    }
    /// Unlocks the mutex
    void unlock() {
        m_mutex.unlock();
    }

private:
    /// Backing standard mutex
    std::mutex m_mutex;
};

/// Host clock: tests set the "system tick" directly, in milliseconds.
inline std::int64_t g_host_tick_ms = 0;

/// Duration with a millisecond resolution (os::TimeSpan stand-in)
class TimeSpan {
public:
    /**
     * @brief Builds a span
     *
     * @param ms duration in milliseconds
     */
    explicit TimeSpan(std::int64_t ms) : m_ms(ms) {}
    /**
     * @brief Span of whole seconds
     *
     * @param s duration in seconds
     * @return span of s * 1000 ms
     */
    static TimeSpan FromSeconds(std::int64_t s) {
        return TimeSpan(s * 1000);
    }
    /**
     * @brief Span of milliseconds
     *
     * @param ms duration in milliseconds
     * @return span of ms
     */
    static TimeSpan FromMilliSeconds(std::int64_t ms) {
        return TimeSpan(ms);
    }
    /**
     * @brief Span of nanoseconds
     *
     * The host clock has a millisecond resolution, like the rest of this
     * stand-in, so the value is truncated to whole milliseconds.
     *
     * @param ns Duration in nanoseconds; negative values give an empty span
     * @return Span of ns / 1000000 ms
     */
    static TimeSpan FromNanoSeconds(std::int64_t ns) {
        return TimeSpan(ns > 0 ? ns / 1000000 : 0);
    }
    /**
     * @brief Duration in milliseconds
     *
     * @return the span in ms
     */
    std::int64_t GetMilliSeconds() const {
        return m_ms;
    }
    /**
     * @brief Duration in nanoseconds
     *
     * @return the span in ns (ms * 1000000)
     */
    std::int64_t GetNanoSeconds() const {
        return m_ms * 1000000;
    }

private:
    /// Duration in milliseconds
    std::int64_t m_ms;
};

/// Tick of the host clock (os::Tick stand-in), in milliseconds
struct Tick {
    /// Tick value, in milliseconds
    std::int64_t ms;
};

/**
 * @brief Current tick of the host clock
 *
 * @return tick holding g_host_tick_ms
 */
inline Tick GetSystemTick() {
    return Tick{g_host_tick_ms};
}
/**
 * @brief Converts a tick to a duration
 *
 * @param tick tick to convert
 * @return span of tick.ms milliseconds
 */
inline TimeSpan ConvertToTimeSpan(Tick tick) {
    return TimeSpan(tick.ms);
}

/// Kernel handle value
using NativeHandle = std::uint32_t;

/// Whether a satisfied wait consumes the event signal (os::EventClearMode)
enum EventClearMode {
    EventClearMode_ManualClear,
    EventClearMode_AutoClear,
};

/// Host shortcut: real waits last (requested ms / divisor), so a test of a
/// multi-second timeout does not take multiple seconds. 1 = faithful.
inline std::int64_t g_host_wait_divisor = 1;

/// Test hooks: run on the calling thread right before the operation, with no
/// stand-in lock held, so a test can script what happens concurrently at that
/// exact point of the code under test (null = off).
inline void (*g_host_before_event_clear)() = nullptr;
/// Test hook: runs before TimedWaitAny scans the holders (null = off)
inline void (*g_host_before_timed_wait_any)() = nullptr;

/// Nanoseconds added to armGetSystemTick(): a test moves the clock forward to
/// cross a deadline without waiting for it.
inline std::atomic<std::uint64_t> g_host_tick_skew_ns{0};

/**
 * @brief Mutex shared by every multi wait
 *
 * Together with MultiWaitCv(): a Signal() of any event wakes the waiters,
 * which then look at their own holders.
 *
 * @return The shared mutex
 */
inline std::mutex& MultiWaitMutex() {
    static std::mutex m;
    return m;
}
/**
 * @brief Condition variable paired with MultiWaitMutex()
 *
 * @return the shared instance
 */
inline std::condition_variable& MultiWaitCv() {
    static std::condition_variable cv;
    return cv;
}

/// Event (os::Event): ManualClear stays signaled until Clear(); AutoClear is
/// consumed by the wait that observes it.
class Event {
public:
    /**
     * @brief Creates the event, not signaled
     *
     * @param mode ManualClear or AutoClear
     */
    explicit Event(EventClearMode mode) : m_mode(mode) {}
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    /**
     * @brief Latches the event, releases the blocked waiters and wakes the
     *        multi waiters
     */
    void Signal() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_signaled = true;
            m_generation++;
            m_cv.notify_all();
        }
        // Wake the multi waiters: taking their mutex first guarantees one that
        // checked the event just before the signal is already blocked in its
        // wait.
        {
            std::lock_guard<std::mutex> lock(MultiWaitMutex());
        }
        MultiWaitCv().notify_all();
    }
    /// Clears the signal, after running the g_host_before_event_clear hook
    void Clear() {
        if (g_host_before_event_clear) {
            g_host_before_event_clear();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_signaled = false;
    }
    /**
     * @brief Event object handed to the multi wait holders (the host event
     * itself).
     *
     * @return this event
     */
    Event* GetBase() {
        return this;
    }
    /**
     * @brief Consumes the signal if there is one (what a satisfied wait does).
     *
     * @return true when the event was signaled (the signal is consumed per its
     * clear mode), false otherwise
     */
    bool TryConsume() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_signaled) {
            return false;
        }
        Consume();
        return true;
    }
    /// Blocks until the event is signaled, then consumes the signal as per mode
    void Wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        const auto generation = m_generation;
        m_cv.wait(
            lock, [&] { return m_signaled || m_generation != generation; });
        Consume();
    }
    /**
     * @brief Like Wait() but gives up after a timeout, scaled by
     *        g_host_wait_divisor
     *
     * @param timeout longest time to wait
     * @return true if signaled, false on timeout
     */
    bool TimedWait(TimeSpan timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        const auto ms = timeout.GetMilliSeconds() / g_host_wait_divisor;
        const auto generation = m_generation;
        if (!m_cv.wait_for(lock, std::chrono::milliseconds(ms), [&] {
                return m_signaled || m_generation != generation;
            })) {
            return false;
        }
        Consume();
        return true;
    }

private:
    /// Clears the signal for an AutoClear event; the caller holds m_mutex
    void Consume() {
        if (m_mode == EventClearMode_AutoClear) {
            m_signaled = false;
        }
    }
    /// Clear mode chosen at construction
    EventClearMode m_mode;
    /// Protects m_signaled and m_generation
    std::mutex m_mutex;
    /// Wakes the waiters of this event
    std::condition_variable m_cv;
    /// Whether the event is currently signaled
    bool m_signaled = false;
    /// A Signal() releases every waiter already blocked, even if Clear() ran
    /// before they woke (kernel semantics).
    unsigned long m_generation = 0;
};

/// Holder of one event inside a MultiWaitType (os::MultiWaitHolderType).
struct MultiWaitHolderType {
    /// Event the holder waits on (null when finalized)
    Event* event = nullptr;
};

/// Set of holders waited on together (os::MultiWaitType).
struct MultiWaitType {
    /// Holders linked to this multi wait, in link order
    std::vector<MultiWaitHolderType*> holders;
};

/**
 * @brief Empties the multi wait
 *
 * @param multi_wait multi wait to initialize
 */
inline void InitializeMultiWait(MultiWaitType* multi_wait) {
    multi_wait->holders.clear();
}
/**
 * @brief Empties the multi wait
 *
 * @param multi_wait multi wait to finalize
 */
inline void FinalizeMultiWait(MultiWaitType* multi_wait) {
    multi_wait->holders.clear();
}
/**
 * @brief Binds a holder to an event
 *
 * @param holder holder to initialize
 * @param event event it waits on
 */
inline void InitializeMultiWaitHolder(
    MultiWaitHolderType* holder, Event* event) {
    holder->event = event;
}
/**
 * @brief Unbinds a holder from its event
 *
 * @param holder holder to finalize
 */
inline void FinalizeMultiWaitHolder(MultiWaitHolderType* holder) {
    holder->event = nullptr;
}
/**
 * @brief Adds a holder to a multi wait
 *
 * @param multi_wait multi wait to extend
 * @param holder holder to append
 */
inline void LinkMultiWaitHolder(
    MultiWaitType* multi_wait, MultiWaitHolderType* holder) {
    multi_wait->holders.push_back(holder);
}
/**
 * @brief Removes every holder from a multi wait
 *
 * @param multi_wait multi wait to empty
 */
inline void UnlinkAllMultiWaitHolder(MultiWaitType* multi_wait) {
    multi_wait->holders.clear();
}

/**
 * @brief Wait for one linked event
 *
 * Waits until one linked event is signaled (and consumes it, like the kernel
 * does for an auto-clear event) or the timeout expires: nullptr then. The
 * timeout is scaled by g_host_wait_divisor.
 *
 * @param multi_wait Multi wait whose holders are scanned
 * @param timeout Longest time to wait
 * @return The holder whose event was consumed, or nullptr on timeout
 */
inline MultiWaitHolderType* TimedWaitAny(
    MultiWaitType* multi_wait, TimeSpan timeout) {
    if (g_host_before_timed_wait_any) {
        g_host_before_timed_wait_any();
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              timeout.GetMilliSeconds() / g_host_wait_divisor);
    std::unique_lock<std::mutex> lock(MultiWaitMutex());
    for (;;) {
        for (auto* holder : multi_wait->holders) {
            if (holder->event->TryConsume()) {
                return holder;
            }
        }
        if (MultiWaitCv().wait_until(lock, deadline) ==
            std::cv_status::timeout) {
            for (auto* holder : multi_wait->holders) {
                if (holder->event->TryConsume()) {
                    return holder;
                }
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
    /**
     * @brief Creates the event; neither argument changes host behavior
     *
     * @param mode clear mode, recorded only
     * @param inter_process whether shared across processes, recorded only
     */
    SystemEvent(EventClearMode mode, bool inter_process)
        : m_mode(mode),
          m_inter_process(inter_process),
          m_handle(++s_next_handle) {}

    SystemEvent(const SystemEvent&) = delete;
    SystemEvent& operator=(const SystemEvent&) = delete;

    /// Counts one signal (see SignalCount())
    void Signal() {
        ++m_signal_count;
    }
    /**
     * @brief Readable handle of the event
     *
     * @return handle, stable and distinct per instance
     */
    NativeHandle GetReadableHandle() const {
        return m_handle;
    }

    /**
     * @brief Number of Signal() calls so far
     *
     * @return the count
     */
    unsigned SignalCount() const {
        return m_signal_count;
    }

private:
    /// Last handle handed out; every instance takes the next value
    inline static NativeHandle s_next_handle = 0;
    /// Clear mode given at construction
    EventClearMode m_mode;
    /// Inter-process flag given at construction
    bool m_inter_process;
    /// Readable handle of this instance
    NativeHandle m_handle;
    /// Number of Signal() calls
    unsigned m_signal_count = 0;
};

/// Condition variable working with os::Mutex (Stratosphere contract: the mutex
/// passed to the wait is held by the caller and released while waiting).
class ConditionVariable {
public:
    ConditionVariable() = default;
    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    /// Wakes every waiter
    void Broadcast() {
        m_cv.notify_all();
    }
    /// Wakes one waiter
    void Signal() {
        m_cv.notify_one();
    }
    /**
     * @brief Releases the mutex, blocks until woken, then takes the mutex again
     *
     * @param mutex mutex held by the caller
     */
    void Wait(Mutex& mutex) {
        m_cv.wait(mutex);
    }
    /**
     * @brief Like Wait() but with a timeout, scaled by g_host_wait_divisor
     *
     * @param mutex mutex held by the caller
     * @param timeout longest time to wait
     */
    void TimedWait(Mutex& mutex, TimeSpan timeout) {
        m_cv.wait_for(
            mutex,
            std::chrono::milliseconds(
                timeout.GetMilliSeconds() / g_host_wait_divisor));
    }

private:
    /// Underlying condition variable, usable with any Lockable
    std::condition_variable_any m_cv;
};

/// Thread stack alignment of the kernel (page size)
constexpr std::size_t ThreadStackAlignment = 0x1000;

/// Thread object: the entry point and its argument are kept by CreateThread
/// and the host thread is started by StartThread, like the kernel does.
struct ThreadType {
    /// Entry point set by CreateThread
    void (*entry)(void*) = nullptr;
    /// Argument passed to the entry point
    void* arg = nullptr;
    /// Name set by SetThreadNamePointer (not copied, may be null)
    const char* name = nullptr;
    /// Host thread, running between StartThread and the join
    std::thread thread;

    ThreadType() = default;
    ThreadType(const ThreadType&) = delete;
    ThreadType& operator=(const ThreadType&) = delete;
    /// A kernel thread that already exited needs no join; the host must
    /// reclaim it. Only reached for an exited thread, otherwise it waits.
    ~ThreadType() {
        if (thread.joinable()) {
            thread.join();
        }
    }
};

/// Failure injection: the next CreateThread fails with this result (0 = off).
inline std::uint32_t g_host_create_thread_result = 0;

/**
 * @brief Records the entry point; the host thread starts in StartThread. Joins
 *        a previous run of the same object first
 *
 * @param out thread object to prepare
 * @param entry thread entry point
 * @param arg argument given to the entry point
 * @return the injected g_host_create_thread_result if not 0, else success
 */
inline Result CreateThread(
    ThreadType* out,
    void (*entry)(void*),
    void* arg,
    void* /*stack*/,
    std::size_t /*stack_size*/,
    int /*priority*/) {
    if (g_host_create_thread_result != 0) {
        return Result(g_host_create_thread_result);
    }
    if (out->thread.joinable()) {
        out->thread.join();
    }
    out->entry = entry;
    out->arg = arg;
    return ResultSuccess();
}
/// Test hook: runs on the starting thread right before the new thread starts,
/// with the name given to SetThreadNamePointer ("" when none).
inline void (*g_host_before_start_thread)(const char*) = nullptr;
/// Thrown by a test hook to end a thread whose entry point loops forever (the
/// kernel thread would be killed with the process); StartThread swallows it.
struct HostThreadExit {};
/**
 * @brief Starts the host thread running the recorded entry point;
 *        HostThreadExit thrown by it is swallowed
 *
 * @param thread thread prepared by CreateThread
 */
inline void StartThread(ThreadType* thread) {
    if (g_host_before_start_thread) {
        g_host_before_start_thread(thread->name != nullptr ? thread->name : "");
    }
    thread->thread = std::thread([entry = thread->entry, arg = thread->arg] {
        try {
            entry(arg);
        } catch (const HostThreadExit&) {
        }
    });
}
/**
 * @brief Joins the host thread if it is running
 *
 * @param thread thread to wait for
 */
inline void WaitThread(ThreadType* thread) {
    if (thread->thread.joinable()) {
        thread->thread.join();
    }
}
/// Does nothing: the destructor of ThreadType joins
inline void DestroyThread(ThreadType*) {}

/// Lowest user-mode priority of the kernel (the host ignores priorities)
constexpr int LowestThreadPriority = 63;
/**
 * @brief Records the thread name (the pointer is kept, not copied)
 *
 * @param thread thread to name
 * @param name NUL-terminated name, must outlive the thread
 */
inline void SetThreadNamePointer(ThreadType* thread, const char* name) {
    thread->name = name;
}

}  // namespace os

namespace svc {

/// Test hook: runs on the sleeping thread at the start of SleepThread, with the
/// requested duration in nanoseconds (null = off).
inline void (*g_host_before_sleep_thread)(std::int64_t ns) = nullptr;
/**
 * @brief Sleep of the calling thread
 *
 * The duration is shortened by the same host divisor as the timed waits so a
 * test of a long period stays fast.
 *
 * @param ns Requested duration in nanoseconds
 */
inline void SleepThread(std::int64_t ns) {
    if (g_host_before_sleep_thread) {
        g_host_before_sleep_thread(ns);
    }
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(ns / os::g_host_wait_divisor));
}

}  // namespace svc

namespace fs {

/// Host file system stand-in: one in-memory file, with failure injection.
/// Tests fill g_host_fs before calling the code under test.
struct HostFs {
    std::string content;      ///< bytes served by ReadFile
    bool open_fails = false;  ///< OpenFile returns a failure
    bool size_fails = false;  ///< GetFileSize returns a failure
    bool read_fails = false;  ///< ReadFile returns a failure
    s64 size_override = -1;   ///< >= 0: GetFileSize reports this instead
    /// Number of successful OpenFile calls
    int open_count = 0;
    /// Number of CloseFile calls
    int close_count = 0;
};
/// The one in-memory file served by the fs stand-ins
inline HostFs g_host_fs;

/// Handle of a file opened on the host stand-in
struct FileHandle {
    /// Handle value: the open_count after the OpenFile that made it
    int id = 0;
};
/// Read access mode flag
constexpr int OpenMode_Read = 1;

/**
 * @brief Opens the in-memory file; fails with 0x202 if open_fails
 *
 * @param out receives the handle
 * @return success or 0x202
 */
inline Result OpenFile(FileHandle* out, const char*, int) {
    if (g_host_fs.open_fails) {
        return Result(0x202);
    }
    ++g_host_fs.open_count;
    out->id = g_host_fs.open_count;
    return ResultSuccess();
}

/**
 * @brief Size of the in-memory file; fails with 0x203 if size_fails
 *
 * @param out receives the size, or size_override when >= 0
 * @return success or 0x203
 */
inline Result GetFileSize(s64* out, FileHandle) {
    if (g_host_fs.size_fails) {
        return Result(0x203);
    }
    *out = g_host_fs.size_override >= 0
               ? g_host_fs.size_override
               : static_cast<s64>(g_host_fs.content.size());
    return ResultSuccess();
}

/**
 * @brief Reads from the in-memory file; fails with 0x204 if read_fails. Reading
 *        past the end copies 0 bytes
 *
 * @param out_read receives the byte count copied
 * @param offset first byte to read
 * @param buffer destination
 * @param size capacity of buffer in bytes
 * @return success or 0x204
 */
inline Result ReadFile(
    std::size_t* out_read,
    FileHandle,
    s64 offset,
    void* buffer,
    std::size_t size) {
    if (g_host_fs.read_fails) {
        return Result(0x204);
    }
    const auto total = static_cast<s64>(g_host_fs.content.size());
    s64 avail = total - offset;
    if (avail < 0) {
        avail = 0;
    }
    const std::size_t n = std::min(size, static_cast<std::size_t>(avail));
    std::memcpy(buffer, g_host_fs.content.data() + offset, n);
    *out_read = n;
    return ResultSuccess();
}

/// Counts the close in close_count
inline void CloseFile(FileHandle) {
    ++g_host_fs.close_count;
}

}  // namespace fs

namespace ncm {

/// Program (title) id of a process.
struct ProgramId {
    /// Raw 64-bit program id
    std::uint64_t value = 0;
    /**
     * @brief Compares the raw ids
     *
     * @return true if both ids are equal
     */
    constexpr bool operator==(const ProgramId&) const = default;
};

}  // namespace ncm

namespace os {

/// Process id of a process.
struct ProcessId {
    /// Raw 64-bit process id
    std::uint64_t value = 0;
};

}  // namespace os

namespace sm {

/// What Atmosphere tells a MITM service about the client it serves.
struct MitmProcessInfo {
    /// Process id of the client
    os::ProcessId process_id{};
    /// Program id of the client
    ncm::ProgramId program_id{};
    /// Override status reported by the service manager
    std::uint32_t override_status = 0;
};

}  // namespace sm

}  // namespace ams

/// Forwarded session of the original service (libnx).
struct Service {
    /// Session handle of the forwarded service
    std::uint32_t session = 0;
    /// Object id inside a domain session
    std::uint32_t object_id = 0;
    /// Whether the session is a domain
    bool domain = false;
};

/**
 * @brief libnx: whether the forwarded session is a domain.
 *
 * @param s forwarded session
 * @return the domain flag of s
 */
inline bool serviceIsDomain(const Service* s) {
    return s->domain;
}

namespace ams {

namespace sf {

/// Marker bases used by IPC structs; they carry no data on the host.
struct LargeData {};

/// Process id of the IPC client, as carried by sf::ClientProcessId.
struct ClientProcessId {
    /// Process id of the client
    os::ProcessId process_id{};
    /**
     * @brief Process id of the client
     *
     * @return the process id
     */
    constexpr os::ProcessId GetValue() const {
        return process_id;
    }
};
struct PrefersPointerTransferMode {};

/// Handle copied to the client (sf::CopyHandle).
struct CopyHandle {
    /// Raw handle value
    std::uint32_t handle = 0;
    /// Whether the real one would close the handle on destruction
    bool managed = false;
    CopyHandle() = default;
    /**
     * @brief Wraps a handle
     *
     * @param h raw handle value
     * @param m whether the handle is managed
     */
    CopyHandle(std::uint32_t h, bool m) : handle(h), managed(m) {}
    /**
     * @brief Raw handle
     *
     * @return the handle value
     */
    std::uint32_t GetOsHandle() const {
        return handle;
    }
    /// Gives the handle up: the destructor of the real one closes it otherwise.
    void Detach() {
        handle = 0xFFFFFFFFu;
        managed = false;
    }
};

/// Output parameter of an IPC command: a reference the handler writes through.
template <class T>
class Out {
public:
    /**
     * @brief Binds the output to a target
     *
     * @param target object the handler writes through
     */
    explicit Out(T& target) : m_target(&target) {}
    /**
     * @brief Dereferences the output
     *
     * @return reference to the target
     */
    T& operator*() const {
        return *m_target;
    }
    /**
     * @brief Member access on the output
     *
     * @return pointer to the target
     */
    T* operator->() const {
        return m_target;
    }
    /**
     * @brief Assigns T(args...) to the target
     *
     * @param args arguments forwarded to the constructor of T
     */
    template <class... A>
    void SetValue(A&&... args) const {
        *m_target = T(std::forward<A>(args)...);
    }

private:
    /// Target of the output parameter
    T* m_target;
};

/// Output array / buffer the handler fills (sf::OutArray, sf::OutBuffer, ...).
template <class T>
class OutArray {
public:
    /**
     * @brief Binds the output to a buffer
     *
     * @param data first element
     * @param count number of elements
     */
    OutArray(T* data, std::size_t count) : m_data(data), m_count(count) {}
    /**
     * @brief Element count
     *
     * @return number of elements
     */
    std::size_t GetSize() const {
        return m_count;
    }
    /**
     * @brief Buffer start
     *
     * @return pointer to the first element
     */
    T* GetPointer() const {
        return m_data;
    }
    /**
     * @brief Element access
     *
     * @param i element index, not range checked
     * @return reference to the element
     */
    T& operator[](std::size_t i) const {
        return m_data[i];
    }

private:
    /// First element of the buffer
    T* m_data;
    /// Number of elements
    std::size_t m_count;
};
template <class T>
/// Output array whose transfer mode the real code selects automatically
using OutAutoSelectArray = OutArray<T>;
/// Output byte buffer
using OutBuffer = OutArray<std::uint8_t>;
/// Output byte buffer whose transfer mode the real code selects automatically
using OutAutoSelectBuffer = OutArray<std::uint8_t>;

/// Input buffer the handler reads (sf::InBuffer, sf::InPointerBuffer, ...).
class InBuffer {
public:
    /**
     * @brief Binds the input to a buffer
     *
     * @param data first byte
     * @param size size in bytes
     */
    InBuffer(const std::uint8_t* data, std::size_t size)
        : m_data(data), m_size(size) {}
    /**
     * @brief Size in bytes
     *
     * @return the size
     */
    std::size_t GetSize() const {
        return m_size;
    }
    /**
     * @brief Buffer start
     *
     * @return pointer to the first byte
     */
    const std::uint8_t* GetPointer() const {
        return m_data;
    }

private:
    /// First byte of the buffer
    const std::uint8_t* m_data;
    /// Size of the buffer in bytes
    std::size_t m_size;
};
/// Input buffer whose transfer mode the real code selects automatically
using InAutoSelectBuffer = InBuffer;
/// Input buffer passed through a pointer transfer
using InPointerBuffer = InBuffer;

/// Reference to a service object created by the MITM (sf::SharedPointer). The
/// real one is type erased too: the interface is only a tag, so the host keeps
/// the implementation behind a void pointer and hands it back to the test.
template <class Interface>
class SharedPointer {
public:
    SharedPointer() = default;
    /**
     * @brief Wraps an implementation object, hiding its type
     *
     * @param object shared owner of the implementation
     */
    template <class Impl>
    explicit SharedPointer(std::shared_ptr<Impl> object)
        : m_object(object), m_raw(object.get()) {}
    /**
     * @brief Whether an object is held
     *
     * @return true if the pointer is not empty
     */
    explicit operator bool() const {
        return m_raw != nullptr;
    }
    /**
     * @brief Typed view of the held object
     *
     * @return the object as Impl*; the caller must name the type it was built
     *         with
     */
    template <class Impl>
    Impl* GetImpl() const {
        return static_cast<Impl*>(m_raw);
    }
    /**
     * @brief Keeps the object alive after the pointer itself is gone.
     *
     * @return shared owner of the wrapped object
     */
    std::shared_ptr<void> Owner() const {
        return m_object;
    }

private:
    /// Shared owner keeping the object alive
    std::shared_ptr<void> m_object;
    /// Raw pointer to the object
    void* m_raw = nullptr;
};

/**
 * @brief Builds an Impl and wraps it
 *
 * @param args constructor arguments of Impl
 * @return pointer owning the new object
 */
template <class Interface, class Impl, class... Args>
SharedPointer<Interface> CreateSharedObjectEmplaced(Args&&... args) {
    return SharedPointer<Interface>(
        std::make_shared<Impl>(std::forward<Args>(args)...));
}

/// Base of a MITM service: keeps the forwarding session and the client info.
class MitmServiceImplBase {
public:
    /**
     * @brief Stores the forwarding session and client info
     *
     * @param forward session of the original service
     * @param client process the service is created for
     */
    MitmServiceImplBase(
        std::shared_ptr<::Service>&& forward, const sm::MitmProcessInfo& client)
        : m_forward_service(std::move(forward)), m_client_info(client) {}

protected:
    /// Session of the original service
    std::shared_ptr<::Service> m_forward_service;
    /// Process the service is created for
    sm::MitmProcessInfo m_client_info;
};

}  // namespace sf

using os::TimeSpan;

namespace impl {
/**
 * @brief Swallows its arguments, to silence unused warnings
 *
 * @param ... arguments, ignored
 */
template <class... Ts>
constexpr void UnusedImpl(const Ts&...) {}
}  // namespace impl

}  // namespace ams

/**
 * @brief libnx system tick: the host counts nanoseconds, so a tick is one
 * nanosecond
 *
 * @return monotonic tick count in nanoseconds, including the test skew
 * g_host_tick_skew_ns
 */
inline std::uint64_t armGetSystemTick() {
    return ams::os::g_host_tick_skew_ns.load() +
           static_cast<std::uint64_t>(
               std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}
/**
 * @brief Converts ticks to nanoseconds
 *
 * @param ticks tick count
 * @return nanoseconds (one host tick is one nanosecond)
 */
inline std::uint64_t armTicksToNs(std::uint64_t ticks) {
    return ticks;
}

/// Aborts the process on a failed Result, like the SDK macro does
#define R_ABORT_UNLESS(expr)                                         \
    do {                                                             \
        if (::ams::Result r_abort_ = (expr); r_abort_.IsFailure()) { \
            std::abort();                                            \
        }                                                            \
    } while (false)

/// Marks the arguments as intentionally unused
#define AMS_UNUSED(...) ::ams::impl::UnusedImpl(__VA_ARGS__)
/// Returns a success Result from the enclosing function
#define R_SUCCEED() return ::ams::ResultSuccess()
/**
 * @brief Whether a Result is a failure
 *
 * @param expr expression yielding a Result
 */
#define R_FAILED(expr) ((expr).IsFailure())
/**
 * @brief Returns the given Result from the enclosing function
 *
 * @param res Result to return
 */
#define R_THROW(res) return (res)
/**
 * @brief Returns the given Result from the enclosing function
 *
 * @param res Result to return
 */
#define R_RETURN(res) return (res)
/**
 * @brief Returns res from the enclosing function when cond is false
 *
 * @param cond condition that must hold
 * @param res Result returned otherwise
 */
#define R_UNLESS(cond, res) \
    do {                    \
        if (!(cond)) {      \
            R_RETURN(res);  \
        }                   \
    } while (false)
/**
 * @brief Evaluates expr and returns its Result from the enclosing function if
 *        it is a failure
 *
 * @param expr expression yielding a Result
 */
#define R_TRY(expr)                 \
    do {                            \
        const auto _r_try = (expr); \
        if (_r_try.IsFailure()) {   \
            return _r_try;          \
        }                           \
    } while (false)
/**
 * @brief Whether a Result is a success
 *
 * @param expr expression yielding a Result
 */
#define R_SUCCEEDED(expr) ((expr).IsSuccess())
/// Same bit packing as the SDK: module in bits 0-8, description from bit 9
#define MAKERESULT(module, description) \
    (((module) & 0x1FF) | (((description) & 0x1FFF) << 9))

/// Result module/description: the host Result keeps the same bit packing as
/// the real one (module in bits 0-8, description from bit 9), so a code
/// defined here is non-zero and distinguishable from success.
#define R_DEFINE_NAMESPACE_RESULT_MODULE(NS, MOD)          \
    namespace NS {                                         \
    inline constexpr std::uint32_t ResultModuleId = (MOD); \
    }
/**
 * @brief Defines the function Result<NAME>() in the current namespace, using
 *        the ResultModuleId of R_DEFINE_NAMESPACE_RESULT_MODULE
 *
 * @param NAME error name
 * @param DESC description value, packed from bit 9
 */
#define R_DEFINE_ERROR_RESULT(NAME, DESC)                     \
    constexpr ::ams::Result Result##NAME() {                  \
        return ::ams::Result(ResultModuleId | ((DESC) << 9)); \
    }

/// Stand-in for the sf interface description macros. Real Stratosphere
/// generates a concept checking every method signature; the host version
/// checks that each listed method exists on the class (not its signature),
/// which is what the static_assert in the sources guards at build time.
#define AMS_SF_METHOD_INFO(C, H, ID, RET, NAME, ARGS, CALLARGS, ...) \
    (void)&C::NAME;
/**
 * @brief Declares the class NAME and a concept IsNAME checking that every
 *        listed method exists on T
 *
 * @param NS namespace of the interface
 * @param NAME interface name
 * @param METHODS method list macro
 * @param ID interface id, unused on the host
 */
#define AMS_SF_DEFINE_INTERFACE(NS, NAME, METHODS, ID) \
    namespace NS {                                     \
    class NAME;                                        \
    template <class T>                                 \
    concept Is##NAME = requires { METHODS(T, 0) };     \
    }
/**
 * @brief Same host stand-in as AMS_SF_DEFINE_INTERFACE
 *
 * @param NS namespace of the interface
 * @param NAME interface name
 * @param METHODS method list macro
 * @param ID interface id, unused on the host
 */
#define AMS_SF_DEFINE_MITM_INTERFACE(NS, NAME, METHODS, ID) \
    AMS_SF_DEFINE_INTERFACE(NS, NAME, METHODS, ID)

/// libnx nifm: the host serves whatever the test put in these variables.
inline std::uint32_t g_host_nifm_result = 0;
/// IPv4 address nifmGetCurrentIpConfigInfo reports
inline std::uint32_t g_host_nifm_address = 0;
/// Netmask nifmGetCurrentIpConfigInfo reports
inline std::uint32_t g_host_nifm_netmask = 0;
/**
 * @brief Reports the configured address and netmask; gateway and DNS are always
 *        0
 *
 * @param address receives g_host_nifm_address
 * @param netmask receives g_host_nifm_netmask
 * @param gateway receives 0
 * @param primary_dns receives 0
 * @param secondary_dns receives 0
 * @return g_host_nifm_result (0 on success; outputs untouched on failure)
 */
inline std::uint32_t nifmGetCurrentIpConfigInfo(
    std::uint32_t* address,
    std::uint32_t* netmask,
    std::uint32_t* gateway,
    std::uint32_t* primary_dns,
    std::uint32_t* secondary_dns) {
    if (g_host_nifm_result != 0) {
        return g_host_nifm_result;
    }
    *address = g_host_nifm_address;
    *netmask = g_host_nifm_netmask;
    *gateway = 0;
    *primary_dns = 0;
    *secondary_dns = 0;
    return 0;
}

/**
 * @brief libnx svcSleepThread, shortened by the same divisor as the timed waits
 *
 * @param ns requested sleep in nanoseconds, divided by g_host_wait_divisor
 */
inline void svcSleepThread(std::int64_t ns) {
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(ns / ams::os::g_host_wait_divisor));
}

// ---------------------------------------------------------------------------
// Boot, heap and server-manager stand-ins (used by sysmodule/source/main.cpp)
// ---------------------------------------------------------------------------

/// Order of the boot/teardown calls the code under test made, one label each.
inline std::vector<std::string> g_host_boot_calls;

/// Raised by AMS_ABORT instead of killing the process, so a test can observe
/// that an unreachable-in-production branch aborts.
namespace ams {
struct HostAbort {};
}  // namespace ams
/// Throws HostAbort instead of aborting the process
#define AMS_ABORT(...) \
    throw ::ams::HostAbort {}
/**
 * @brief Branch hint, ignored on the host
 *
 * @param expr Condition, passed through unchanged
 */
#define AMS_LIKELY(expr) (expr)
/**
 * @brief Branch hint, ignored on the host
 *
 * @param expr Condition, passed through unchanged
 */
#define AMS_UNLIKELY(expr) (expr)
/// Attribute of a function that never returns
#define NORETURN __attribute__((noreturn))

namespace ams {

/**
 * @brief Literal suffix for kilobytes
 *
 * @param v value in KB
 * @return v * 1024
 */
constexpr unsigned long long operator""_KB(unsigned long long v) {
    return v * 1024ULL;
}
/**
 * @brief Literal suffix for megabytes
 *
 * @param v value in MB
 * @return v * 1024 * 1024
 */
constexpr unsigned long long operator""_MB(unsigned long long v) {
    return v * 1024ULL * 1024ULL;
}

namespace util {
/**
 * @brief Rounds a value up to a multiple of an alignment
 *
 * @param value value to round
 * @param align alignment in bytes
 * @return smallest multiple of align that is >= value
 */
template <class T>
constexpr T AlignUp(T value, std::size_t align) {
    return static_cast<T>(
        (static_cast<std::size_t>(value) + align - 1) / align * align);
}
}  // namespace util

namespace os {

/// Kernel page size in bytes
constexpr std::size_t MemoryPageSize = 0x1000;
/// Does nothing: the host mutex needs no initialization
inline void InitializeSdkMutex(SdkMutex*) {}
/**
 * @brief Current thread
 *
 * @return a dummy thread object shared by every caller
 */
inline ThreadType* GetCurrentThread() {
    static ThreadType t;
    return &t;
}
/**
 * @brief Priority of a thread
 *
 * @return always 16, priorities are not modelled
 */
inline int GetThreadCurrentPriority(ThreadType*) {
    return 16;
}

}  // namespace os

namespace lmem {

/// Expanded heap: the host serves the allocations from malloc, and counts the
/// heaps created so a test can check the lazy, once-only creation.
using HeapHandle = void*;
/// Creation flag asking for a thread-safe heap
constexpr int CreateOption_ThreadSafe = 1;
/// Number of heaps created by CreateExpHeap
inline int g_host_heaps_created = 0;
/// Base given to the last CreateExpHeap
inline void* g_host_heap_base = nullptr;
/// Size given to the last CreateExpHeap
inline std::size_t g_host_heap_size = 0;
/// Option given to the last CreateExpHeap
inline int g_host_heap_option = 0;
/**
 * @brief Records the arguments and counts the heap
 *
 * @param base heap memory
 * @param size heap size in bytes
 * @param option creation flags
 * @return the base pointer, used as handle
 */
inline HeapHandle CreateExpHeap(void* base, std::size_t size, int option) {
    ++g_host_heaps_created;
    g_host_heap_base = base;
    g_host_heap_size = size;
    g_host_heap_option = option;
    return base;
}
/// Number of AllocateFromExpHeap calls
inline std::atomic<int> g_host_heap_allocs{0};
/// Number of FreeToExpHeap calls
inline std::atomic<int> g_host_heap_frees{0};
/**
 * @brief Allocates with malloc and counts the call
 *
 * @param size size in bytes
 * @return the block, or null
 */
inline void* AllocateFromExpHeap(HeapHandle, std::size_t size) {
    ++g_host_heap_allocs;
    return std::malloc(size);
}
/**
 * @brief Frees with free and counts the call
 *
 * @param p block returned by AllocateFromExpHeap
 */
inline void FreeToExpHeap(HeapHandle, void* p) {
    ++g_host_heap_frees;
    std::free(p);
}

}  // namespace lmem

namespace init {

/// Buffer given to the last InitializeAllocator
inline void* g_host_allocator_buffer = nullptr;
/// Size given to the last InitializeAllocator
inline std::size_t g_host_allocator_size = 0;
/**
 * @brief Records the call in g_host_boot_calls
 *
 * @param buffer allocator memory
 * @param size size in bytes
 */
inline void InitializeAllocator(void* buffer, std::size_t size) {
    g_host_boot_calls.emplace_back("InitializeAllocator");
    g_host_allocator_buffer = buffer;
    g_host_allocator_size = size;
}

}  // namespace init

namespace sm {

/**
 * @brief Records the call in g_host_boot_calls
 *
 * @return success
 */
inline Result Initialize() {
    g_host_boot_calls.emplace_back("sm::Initialize");
    return ResultSuccess();
}

/// Eight-character service name, like the real one.
struct ServiceName {
    /// NUL-terminated service name, up to 8 characters
    char name[9];
    /**
     * @brief Builds a name from text, truncated to 8 characters
     *
     * @param text NUL-terminated service name
     * @return the encoded name
     */
    static constexpr ServiceName Encode(const char* text) {
        ServiceName n{};
        for (std::size_t i = 0; i < 8 && text[i] != '\0'; ++i) {
            n.name[i] = text[i];
        }
        return n;
    }
};

}  // namespace sm

namespace fs {

/// Allocator given to the last fs::SetAllocator
inline void* (*g_host_fs_alloc)(std::size_t) = nullptr;
/// Deallocator given to the last fs::SetAllocator
inline void (*g_host_fs_free)(void*, std::size_t) = nullptr;
/// Auto-abort flag given to the last fs::SetEnabledAutoAbort (true by default)
inline bool g_host_fs_auto_abort = true;
/// Records the call in g_host_boot_calls
inline void InitializeForSystem() {
    g_host_boot_calls.emplace_back("fs::InitializeForSystem");
}
/**
 * @brief Records the call and keeps both callbacks
 *
 * @param alloc allocation callback
 * @param dealloc deallocation callback
 */
inline void SetAllocator(
    void* (*alloc)(std::size_t), void (*dealloc)(void*, std::size_t)) {
    g_host_boot_calls.emplace_back("fs::SetAllocator");
    g_host_fs_alloc = alloc;
    g_host_fs_free = dealloc;
}
/**
 * @brief Records the call and keeps the flag
 *
 * @param enabled new auto-abort setting
 */
inline void SetEnabledAutoAbort(bool enabled) {
    g_host_boot_calls.emplace_back("fs::SetEnabledAutoAbort");
    g_host_fs_auto_abort = enabled;
}
/**
 * @brief Records "fs::MountSdCard:<name>" in g_host_boot_calls
 *
 * @param name mount name
 * @return success
 */
inline Result MountSdCard(const char* name) {
    g_host_boot_calls.emplace_back(std::string("fs::MountSdCard:") + name);
    return ResultSuccess();
}
/**
 * @brief Records "fs::Unmount:<name>" in g_host_boot_calls
 *
 * @param name mount name
 */
inline void Unmount(const char* name) {
    g_host_boot_calls.emplace_back(std::string("fs::Unmount:") + name);
}

}  // namespace fs

namespace sf::hipc {

/// A session waiting to be accepted: hands out what a test configured.
class Server {
public:
    /// Session handle given to the Service of the next accepted session
    inline static std::uint32_t s_session = 0x77;
    /// Client info given to the next accepted session
    inline static sm::MitmProcessInfo s_client{};
    /**
     * @brief Hands out a Service carrying s_session and the client info
     *        s_client
     *
     * @param out_service receives the forwarded service
     * @param out_info receives the client info
     */
    void AcknowledgeMitmSession(
        std::shared_ptr<::Service>* out_service,
        sm::MitmProcessInfo* out_info) {
        auto service = std::make_shared<::Service>();
        service->session = s_session;
        *out_service = std::move(service);
        *out_info = s_client;
    }
};

/// Server manager: records what is registered and what is accepted; a
/// session loop returns at once (or runs the test's hook) instead of serving.
template <std::size_t Ports, class Options, std::size_t MaxSessions>
class ServerManager {
public:
    /// The server type this manager serves
    using Server = ::ams::sf::hipc::Server;

    /// One registration made on the manager
    struct Registration {
        /// Service name
        std::string name;
        /// Port index, -1 for a non-MITM object
        int port_index;
        /// Session limit given at registration (0 for MITM)
        std::size_t max_sessions;
    };
    /// Registrations made so far, in order
    std::vector<Registration> registrations;
    /// Owners of the objects accepted so far
    std::vector<std::shared_ptr<void>> accepted;
    /// Number of LoopProcess calls
    std::atomic<int> loops{0};
    /// Test hook run by LoopProcess (null = off)
    void (*loop_hook)() = nullptr;
    /// Result AcceptMitmImpl returns
    Result accept_result{};
    /// Result the Register* methods return
    Result register_result{};

    virtual ~ServerManager() = default;

    /**
     * @brief Counts one loop and runs loop_hook, then returns at once instead
     *        of serving
     */
    void LoopProcess() {
        ++loops;
        if (loop_hook != nullptr) {
            loop_hook();
        }
    }

    /**
     * @brief Records a registration with port index -1
     *
     * @param name service name
     * @param max_sessions session limit
     * @return register_result
     */
    template <class Interface>
    Result RegisterObjectForServer(
        SharedPointer<Interface>&&,
        sm::ServiceName name,
        std::size_t max_sessions) {
        registrations.push_back({name.name, -1, max_sessions});
        return register_result;
    }

    /**
     * @brief Records a MITM registration with a session limit of 0
     *
     * @param port_index port of the MITM server
     * @param name service name
     * @return register_result
     */
    template <class Impl>
    Result RegisterMitmServer(int port_index, sm::ServiceName name) {
        registrations.push_back({name.name, port_index, 0});
        return register_result;
    }

    /**
     * @brief Keeps the accepted object alive
     *
     * @param object object created for the session
     * @return accept_result
     */
    template <class Interface>
    Result AcceptMitmImpl(
        Server*, SharedPointer<Interface> object, std::shared_ptr<::Service>) {
        accepted.push_back(object.Owner());
        return accept_result;
    }

    /**
     * @brief Entry point for a test: the real manager calls OnNeedsToAccept
     * itself.
     *
     * @param port_index index of the port with a pending session
     * @param server server the session belongs to
     * @return result of OnNeedsToAccept(), accept_result by default
     */
    Result HostAccept(int port_index, Server* server) {
        return OnNeedsToAccept(port_index, server);
    }

protected:
    /**
     * @brief Default accept handler, overridable by a test
     *
     * @return success
     */
    virtual Result OnNeedsToAccept(int, Server*) {
        return ResultSuccess();
    }
};

}  // namespace sf::hipc

}  // namespace ams

/// libnx nifm service lifecycle
enum NifmServiceType { NifmServiceType_User = 0, NifmServiceType_Admin = 1 };
/**
 * @brief Records the call in g_host_boot_calls
 *
 * @return 0, success
 */
inline std::uint32_t nifmInitialize(NifmServiceType) {
    g_host_boot_calls.emplace_back("nifmInitialize");
    return 0;
}
/// Records the call in g_host_boot_calls
inline void nifmExit() {
    g_host_boot_calls.emplace_back("nifmExit");
}
