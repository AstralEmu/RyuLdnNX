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
#include <mutex>

namespace ams {

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

} // namespace os

} // namespace ams
