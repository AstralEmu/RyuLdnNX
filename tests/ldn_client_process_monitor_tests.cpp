/**
 * @file ldn_client_process_monitor_tests.cpp
 * @brief Unit tests for the real IClientProcessMonitor
 * (ldn/ldn_client_process_monitor.cpp)
 *
 * Built against the host Stratosphere stand-in in tests/stubs.
 */

#include <cstdio>
#include <memory>

#include "test_harness.hpp"
#include "ldn/ldn_client_process_monitor.hpp"

using ams::mitm::ldn::IClientProcessMonitor;

/**
 * @brief The monitor can be heap-allocated and deleted without crashing.
 * It has no required construction arguments, which the MITM service relies on
 * when it instantiates one per session.
 */
TEST(the_monitor_can_be_created_and_destroyed) {
    auto monitor = std::make_unique<IClientProcessMonitor>();
    ASSERT_TRUE(monitor != nullptr);
    monitor.reset();
}

/**
 * @brief RegisterClient accepts any client process id and returns success.
 * The monitor only exists to satisfy firmware 18.0.0+ and performs no
 * validation, so a failure here would break every LDN client at startup.
 */
TEST(registering_a_client_always_succeeds) {
    ams::sf::ClientProcessId pid;
    pid.process_id.value = 4242;
    ASSERT_TRUE(IClientProcessMonitor::RegisterClient(pid).IsSuccess());
}

/**
 * @brief Print the test totals (the tests ran during static initialization).
 * @return Non-zero when at least one assertion failed.
 */
int main() {
    return test_report("IClientProcessMonitor Tests");
}
