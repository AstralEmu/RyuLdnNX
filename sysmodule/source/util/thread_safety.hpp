/**
 * @file thread_safety.hpp
 * @brief Clang thread-safety annotations and an annotated scoped lock.
 *
 * The macros expand to nothing outside clang, so the device build (GCC) is
 * unaffected. libstdc++'s std::scoped_lock carries no annotations, hence
 * RYU_GUARDED_BY members can only be checked through ScopedLock below.
 */
#ifndef SYSMODULE_SOURCE_UTIL_THREAD_SAFETY_HPP_
#define SYSMODULE_SOURCE_UTIL_THREAD_SAFETY_HPP_

#if defined(__clang__)
#define RYU_TSA(x) __attribute__((x))
#else
#define RYU_TSA(x)
#endif

#if defined(__clang__)
#define RYU_CLASS_TSA(x) [[clang::x]]
#define RYU_GUARDED_BY(m) __attribute__((guarded_by(m)))
#else
#define RYU_CLASS_TSA(x)
#define RYU_GUARDED_BY(m)
#endif
#define RYU_REQUIRES(m) RYU_TSA(requires_capability(m))

namespace ryu {

/// RAII lock over any Lockable, visible to -Wthread-safety.
template <class M>
class RYU_CLASS_TSA(scoped_lockable) ScopedLock {
public:
    explicit ScopedLock(M& mutex) RYU_TSA(acquire_capability(mutex)) : m_mutex(mutex) {
        m_mutex.lock();
    }
    ~ScopedLock() RYU_TSA(release_capability()) {
        m_mutex.unlock();
    }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

private:
    M& m_mutex;
};

}  // namespace ryu
#endif  // SYSMODULE_SOURCE_UTIL_THREAD_SAFETY_HPP_
