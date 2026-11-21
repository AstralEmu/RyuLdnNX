/**
 * @file ldn_client_process_monitor_tests.cpp
 * @brief Unit tests for the real IClientProcessMonitor (ldn/ldn_client_process_monitor.cpp)
 *
 * Built against the host Stratosphere stand-in in tests/stubs.
 */

#include <cstdio>

#include "test_harness.hpp"
#include "ldn/ldn_client_process_monitor.hpp"

using ams::mitm::ldn::IClientProcessMonitor;

TEST(the_monitor_can_be_created_and_destroyed) {
    IClientProcessMonitor* monitor = new IClientProcessMonitor();
    ASSERT_TRUE(monitor != nullptr);
    delete monitor;
}

TEST(registering_a_client_always_succeeds) {
    ams::sf::ClientProcessId pid;
    pid.value = 4242;
    ASSERT_TRUE(IClientProcessMonitor::RegisterClient(pid).IsSuccess());
}

int main() {
    return test_report("IClientProcessMonitor Tests");
}
