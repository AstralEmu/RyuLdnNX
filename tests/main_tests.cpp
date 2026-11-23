/**
 * @file main_tests.cpp
 * @brief Host tests of the sysmodule boot code (sysmodule/source/main.cpp)
 *
 * main.cpp is pulled in with an include directive (unity build): its server
 * managers, heap and socket configuration are file-local. The services it
 * registers are the real ones; the libnx boot calls and the SF server are the
 * host stand-ins of tests/stubs, which record what the boot code gave them.
 * That includes the global operator new/delete of main.cpp: every allocation of
 * this test binary goes through the sysmodule heap wrappers.
 */

#include "test_harness.hpp"

#include <exception>
#include <filesystem>
#include <new>
#include <unistd.h>
#include <utility>

#include "main.cpp"

namespace fsys = std::filesystem;
namespace boot = ams;
namespace lm = ams::lmem;

namespace {

/// A private working directory holding the SD card layout the boot code writes
/// to ("sdmc:/config/ryu_ldn_nx/..." is a relative path on the host).
class SdCardDir {
public:
    SdCardDir() {
        char tmpl[] = "/tmp/ryu_boot_XXXXXX";
        m_root = ::mkdtemp(tmpl);
        m_previous = fsys::current_path();
        fsys::create_directories(m_root / "sdmc:" / "config" / "ryu_ldn_nx");
        fsys::current_path(m_root);
    }
    ~SdCardDir() {
        fsys::current_path(m_previous);
        std::error_code ec;
        fsys::remove_all(m_root, ec);
    }
    SdCardDir(const SdCardDir&) = delete;
    SdCardDir& operator=(const SdCardDir&) = delete;

private:
    /// Test fixture value: root.
    fsys::path m_root;
    /// Test fixture value: previous.
    fsys::path m_previous;
};

template <class Fn>
/**
 * @brief Runs fn and reports whether it aborted.
 *
 * @param fn callable run under test
 * @return true when fn threw ams::HostAbort (the host build of an abort), false
 * when it returned
 */
bool aborts(Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const ams::HostAbort&) {
        return true;
    }
    return false;
}

/// Test fixture value: started threads.
std::vector<std::string> g_started_threads;
/**
 * @brief Thread-start stub that records the thread name.
 *
 * @param name name of the started thread, appended to g_started_threads
 */
void record_started_thread(const char* name) {
    g_started_threads.emplace_back(name);
}

/// Test fixture value: sleeps.
std::atomic<int> g_sleeps{0};
/// Test fixture value: last sleep ns.
std::int64_t g_last_sleep_ns = 0;
/// Test fixture value: sleeps before exit.
int g_sleeps_before_exit = 1;
/**
 * @brief Thread-exit signal carrying the stub's HostThreadExit.
 *
 * The host thread starter swallows ams::os::HostThreadExit to end a thread;
 * that stand-in type does not derive from std::exception, so this wrapper
 * adds the standard base while keeping the same catch contract.
 */
struct HostThreadExitSignal : ams::os::HostThreadExit, std::exception {};
/**
 * @brief Sleep stub that ends the log thread (throws HostThreadExit) once
 * g_sleeps_before_exit sleeps happened.
 *
 * @param ns requested sleep duration in nanoseconds, recorded in
 * g_last_sleep_ns
 */
void leave_log_thread_after_n_sleeps(std::int64_t ns) {
    g_last_sleep_ns = ns;
    if (++g_sleeps >= g_sleeps_before_exit) {
        throw HostThreadExitSignal{};
    }
}

}  // namespace

/**
 * @brief Test case: the malloc buffer is handed to the global allocator at
 *   startup.
 */
TEST(the_malloc_buffer_is_handed_to_the_global_allocator_at_startup) {
    g_host_boot_calls.clear();
    boot::init::Startup();
    ASSERT_EQ(g_host_boot_calls.size(), 1U);
    ASSERT_TRUE(g_host_boot_calls[0] == "InitializeAllocator");
    ASSERT_TRUE(
        boot::init::g_host_allocator_buffer ==
        static_cast<void*>(boot::g_malloc_buffer));
    ASSERT_EQ(
        boot::init::g_host_allocator_size, static_cast<size_t>(1024 * 1024));
}

/** @brief Test case: socket buffers match documented sizes and bsd config. */
TEST(socket_buffers_match_documented_sizes_and_bsd_config) {
    const auto& socket = boot::LibnxSocketInitConfig;
    ASSERT_EQ(socket.tcp_tx_buf_size, 0x800U);
    ASSERT_EQ(socket.tcp_rx_buf_size, 0x1000U);
    ASSERT_EQ(socket.tcp_tx_buf_max_size, 0x2000U);
    ASSERT_EQ(socket.tcp_rx_buf_max_size, 0x2000U);
    ASSERT_EQ(socket.udp_tx_buf_size, 0x2000U);
    ASSERT_EQ(socket.udp_rx_buf_size, 0x2000U);
    ASSERT_EQ(socket.sb_efficiency, 4U);
    ASSERT_EQ(socket.num_bsd_sessions, 14U);
    ASSERT_TRUE(socket.bsd_service_type == BsdServiceType_System);

    // 4 x page-aligned (max tcp tx + max tcp rx + udp tx + udp rx = 0x8000)
    ASSERT_EQ(
        sizeof(boot::g_socket_tmem_buffer), static_cast<size_t>(4 * 0x8000));

    const auto& bsd = boot::LibnxBsdInitConfig;
    ASSERT_EQ(bsd.version, 1U);
    ASSERT_TRUE(
        bsd.tmem_buffer == static_cast<void*>(boot::g_socket_tmem_buffer));
    ASSERT_EQ(bsd.tmem_buffer_size, sizeof(boot::g_socket_tmem_buffer));
    ASSERT_EQ(bsd.tcp_tx_buf_size, socket.tcp_tx_buf_size);
    ASSERT_EQ(bsd.tcp_rx_buf_size, socket.tcp_rx_buf_size);
    ASSERT_EQ(bsd.tcp_tx_buf_max_size, socket.tcp_tx_buf_max_size);
    ASSERT_EQ(bsd.tcp_rx_buf_max_size, socket.tcp_rx_buf_max_size);
    ASSERT_EQ(bsd.udp_tx_buf_size, socket.udp_tx_buf_size);
    ASSERT_EQ(bsd.udp_rx_buf_size, socket.udp_rx_buf_size);
    ASSERT_EQ(bsd.sb_efficiency, socket.sb_efficiency);
}

/**
 * @brief Test case: every operator new and delete goes through the sysmodule
 *   heap.
 */
TEST(every_operator_new_and_delete_goes_through_the_sysmodule_heap) {
    const std::align_val_t align{16};
    const std::nothrow_t& no = std::nothrow;
    void* p[8] = {};
    const int allocs_before = lm::g_host_heap_allocs;
    p[0] = ::operator new(16);
    p[1] = ::operator new(16, no);
    p[2] = ::operator new[](16);
    p[3] = ::operator new[](16, no);
    p[4] = ::operator new(16, align);
    p[5] = ::operator new(16, align, no);
    p[6] = ::operator new[](16, align);
    p[7] = ::operator new[](16, align, no);
    const int allocs = lm::g_host_heap_allocs - allocs_before;
    // Record the outcome before freeing: an assertion returns from the test,
    // so asserting first would leak every block still owned here.
    bool allocated[8] = {};
    for (std::size_t i = 0; i < sizeof(p) / sizeof(p[0]); ++i) {
        allocated[i] = p[i] != nullptr;
    }

    const int frees_before = lm::g_host_heap_frees;
    ::operator delete(p[0]);
    ::operator delete(p[1], size_t{16});
    ::operator delete[](p[2]);
    ::operator delete[](p[3], size_t{16});
    ::operator delete(p[4], align);
    ::operator delete(p[5], size_t{16}, align);
    ::operator delete[](p[6], align);
    ::operator delete[](p[7], size_t{16}, align);
    const int frees = lm::g_host_heap_frees - frees_before;

    ASSERT_EQ(allocs, 8);
    ASSERT_EQ(frees, 8);
    for (const bool ok : allocated) {
        ASSERT_TRUE(ok);
    }
}

/**
 * @brief Test case: the heap is created once on the static buffer and thread
 *   safe.
 */
TEST(the_heap_is_created_once_on_the_static_buffer_and_thread_safe) {
    ASSERT_EQ(lm::g_host_heaps_created, 1);
    ASSERT_TRUE(
        lm::g_host_heap_base == static_cast<void*>(boot::mitm::g_heap_memory));
    ASSERT_EQ(lm::g_host_heap_size, static_cast<size_t>(384 * 1024));
    ASSERT_EQ(lm::g_host_heap_option, lm::CreateOption_ThreadSafe);
    ASSERT_TRUE(boot::mitm::GetHeapHandle() == boot::mitm::GetHeapHandle());
    ASSERT_EQ(lm::g_host_heaps_created, 1);
}

/**
 * @brief Test case: boot initializes services in order with the documented
 *   socket setup.
 */
TEST(boot_initializes_services_in_order_with_the_documented_socket_setup) {
    SdCardDir sd;
    g_host_boot_calls.clear();
    boot::init::InitializeSystemModule();

    const std::vector<std::string> expected = {
        "sm::Initialize",
        "fs::InitializeForSystem",
        "fs::SetAllocator",
        "fs::SetEnabledAutoAbort",
        "fs::MountSdCard:sdmc",
        "nifmInitialize",
        "bsdInitialize",
        "socketInitialize"};
    ASSERT_TRUE(g_host_boot_calls == expected);

    ASSERT_TRUE(ams::fs::g_host_fs_alloc == &boot::mitm::Allocate);
    ASSERT_TRUE(ams::fs::g_host_fs_free == &boot::mitm::Deallocate);
    ASSERT_FALSE(ams::fs::g_host_fs_auto_abort);

    ASSERT_TRUE(g_host_bsd_config == &boot::LibnxBsdInitConfig);
    ASSERT_EQ(
        g_host_bsd_sessions, boot::LibnxSocketInitConfig.num_bsd_sessions);
    ASSERT_TRUE(g_host_bsd_type == BsdServiceType_System);
    ASSERT_TRUE(g_host_socket_config == &boot::LibnxSocketInitConfig);

    // the default configuration was written to the SD card
    ASSERT_TRUE(fsys::exists("sdmc:/config/ryu_ldn_nx/config.ini"));
}

/** @brief Test case: teardown runs in the reverse order of the boot. */
/** @brief Test case: teardown runs in the reverse order of the boot. */
TEST(teardown_runs_in_the_reverse_order_of_the_boot) {
    SdCardDir sd;
    g_host_boot_calls.clear();
    boot::init::FinalizeSystemModule();
    const std::vector<std::string> expected = {
        "socketExit", "bsdExit", "nifmExit", "fs::Unmount:sdmc"};
    ASSERT_TRUE(g_host_boot_calls == expected);
}

/** @brief Test case: exit aborts because the sysmodule is immortal. */
/** @brief Test case: exit aborts because the sysmodule is immortal. */
TEST(exit_aborts_because_the_sysmodule_is_immortal) {
    ASSERT_TRUE(aborts([] { boot::Exit(0); }));
}

/**
 * @brief Test case: an accepted ldn session gets the ldn service and a new
 *   session id.
 */
TEST(an_accepted_ldn_session_gets_the_ldn_service_and_a_new_session_id) {
    auto& manager = boot::mitm::g_server_manager;
    manager.accepted.clear();
    ams::sf::hipc::Server::s_client.process_id.value = 4321;
    ams::sf::hipc::Server::s_client.program_id.value = 0x0100152000022000ULL;
    ams::sf::hipc::Server server;
    const u32 before = boot::mitm::g_ldn_session_counter;

    manager.accept_result = ams::Result();
    ASSERT_TRUE(
        manager.HostAccept(boot::mitm::PortIndex_LdnMitm, &server).IsSuccess());
    ASSERT_EQ(boot::mitm::g_ldn_session_counter, before + 1);
    ASSERT_EQ(manager.accepted.size(), 1U);

    // the manager's verdict is returned as is
    manager.accept_result = ams::Result(0x1234);
    ASSERT_EQ(
        manager.HostAccept(boot::mitm::PortIndex_LdnMitm, &server).GetValue(),
        0x1234U);
    ASSERT_EQ(boot::mitm::g_ldn_session_counter, before + 2);

    manager.accept_result = ams::Result();
    manager.accepted.clear();
}

/**
 * @brief Test case: an accepted bsd session gets the bsd service and a new
 *   session id.
 */
TEST(an_accepted_bsd_session_gets_the_bsd_service_and_a_new_session_id) {
    auto& manager = boot::mitm::g_server_manager;
    manager.accepted.clear();
    ams::sf::hipc::Server server;
    const u32 before = boot::mitm::g_bsd_session_counter;
    const u32 ldn_before = boot::mitm::g_ldn_session_counter;

    manager.accept_result = ams::Result();
    ASSERT_TRUE(
        manager.HostAccept(boot::mitm::PortIndex_BsdMitm, &server).IsSuccess());
    ASSERT_EQ(boot::mitm::g_bsd_session_counter, before + 1);
    ASSERT_EQ(boot::mitm::g_ldn_session_counter, ldn_before);
    ASSERT_EQ(manager.accepted.size(), 1U);

    manager.accept_result = ams::Result(0x4321);
    ASSERT_EQ(
        manager.HostAccept(boot::mitm::PortIndex_BsdMitm, &server).GetValue(),
        0x4321U);

    manager.accept_result = ams::Result();
    manager.accepted.clear();
}

/** @brief Test case: a session on an unknown port aborts. */
/** @brief Test case: a session on an unknown port aborts. */
TEST(a_session_on_an_unknown_port_aborts) {
    ams::sf::hipc::Server server;
    ASSERT_TRUE(
        aborts([&] { boot::mitm::g_server_manager.HostAccept(7, &server); }));
}

/**
 * @brief Test case: the log maintenance thread sleeps two seconds between idle
 *   checks.
 */
TEST(the_log_maintenance_thread_sleeps_two_seconds_between_idle_checks) {
    g_sleeps = 0;
    g_sleeps_before_exit = 3;
    ams::svc::g_host_before_sleep_thread = leave_log_thread_after_n_sleeps;
    bool left = false;
    try {
        boot::cfg::LoopLogMaintenanceThread(nullptr);
    } catch (const ams::os::HostThreadExit&) {
        left = true;
    }
    ams::svc::g_host_before_sleep_thread = nullptr;
    ASSERT_TRUE(left);
    ASSERT_EQ(g_sleeps.load(), 3);
    ASSERT_EQ(g_last_sleep_ns, static_cast<std::int64_t>(2'000'000'000));
}

/**
 * @brief Test case: main registers the three services and starts the four
 *   threads.
 */
TEST(main_registers_the_three_services_and_starts_the_four_threads) {
    g_started_threads.clear();
    g_sleeps = 0;
    g_sleeps_before_exit = 1;
    ams::svc::g_host_before_sleep_thread = leave_log_thread_after_n_sleeps;
    ams::os::g_host_before_start_thread = record_started_thread;
    boot::mitm::g_server_manager.registrations.clear();
    boot::mitm::g_server_manager.loops = 0;
    boot::cfg::g_config_server_manager.registrations.clear();
    boot::cfg::g_config_server_manager.loops = 0;

    boot::Main();
    ams::os::WaitThread(&boot::cfg::g_thread);
    ams::os::WaitThread(&boot::cfg::g_log_thread);
    ams::svc::g_host_before_sleep_thread = nullptr;
    ams::os::g_host_before_start_thread = nullptr;

    const auto& cfg = boot::cfg::g_config_server_manager;
    ASSERT_EQ(cfg.registrations.size(), 1U);
    ASSERT_TRUE(cfg.registrations[0].name == "ryu:cfg");
    ASSERT_EQ(cfg.registrations[0].max_sessions, boot::cfg::MaxSessions);
    ASSERT_EQ(cfg.loops.load(), 1);

    const auto& mitm = boot::mitm::g_server_manager;
    ASSERT_EQ(mitm.registrations.size(), 2U);
    ASSERT_TRUE(mitm.registrations[0].name == "ldn:u");
    ASSERT_EQ(mitm.registrations[0].port_index, boot::mitm::PortIndex_LdnMitm);
    ASSERT_TRUE(mitm.registrations[1].name == "bsd:u");
    ASSERT_EQ(mitm.registrations[1].port_index, boot::mitm::PortIndex_BsdMitm);
    // the main MITM thread plus its extra worker threads all pump the manager
    ASSERT_EQ(
        mitm.loops.load(), static_cast<int>(1 + boot::mitm::NumExtraThreads));

    ASSERT_EQ(g_started_threads.size(), 4U);
    ASSERT_TRUE(g_started_threads[0] == "ryu_ldn::CfgThread");
    ASSERT_TRUE(g_started_threads[1] == "ryu_ldn::LogThread");
    ASSERT_TRUE(g_started_threads[2] == "ryu_ldn::MainThread");
    ASSERT_TRUE(g_started_threads[3] == "ryu_ldn::Thread");
    ASSERT_EQ(g_sleeps.load(), 1);
}

/**
 * @brief Runs the whole suite; the exit code is non-zero if any test failed.
 *
 * @return 0 when every test passed, non-zero otherwise
 */
int main() {
    return test_report("main");
}
