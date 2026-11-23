/**
 * @file main.cpp
 * @brief ryu_ldn_nx - Nintendo Switch LDN to Ryujinx Server Bridge
 *
 * This sysmodule enables Nintendo Switch games to use the Ryujinx LDN
 * servers for online multiplayer, replacing the need for local wireless
 * or complex LAN play setups.
 *
 * Built on Atmosphere's libstratosphere framework.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#include <stratosphere.hpp>

#include <atomic>

extern "C" {
#include <switch/services/bsd.h>
}

#include "main_internal.hpp"

#include "ldn/ldn_mitm_service.hpp"
#include "bsd/bsd_mitm_service.hpp"
#include "config/config.hpp"
#include "config/config_ipc_service.hpp"
#include "config/game_whitelist.hpp"
#include "debug/log.hpp"
#include "ldn/ldn_shared_state.hpp"

namespace ams {

namespace {

// ====================================================================
// Log maintenance thread configuration
// ====================================================================

/// @brief Stack size for the log maintenance thread (bytes)
constexpr size_t LOG_THREAD_STACK_SIZE = 0x1000;
/// @brief Sleep interval of the log maintenance thread (seconds)
constexpr uint64_t LOG_MAINTENANCE_INTERVAL_SEC = 2;
/// @brief Priority offset of the log thread relative to the cfg thread
constexpr s32 LOG_THREAD_PRIORITY_OFFSET = 5;

}  // namespace

namespace internal {

/// Backing store of the libc malloc heap, page aligned, zero-initialized at
/// load time (constinit).
alignas(os::MemoryPageSize) constinit u8 g_malloc_buffer[MallocBufferSize];

}  // namespace internal

namespace internal {

/// Socket transfer memory buffer
alignas(os::MemoryPageSize) constinit u8
    g_socket_tmem_buffer[GetLibnxBsdTransferMemorySize(
        std::addressof(LibnxSocketInitConfig))];

}  // namespace internal

// ========================================================================
// MITM Server Configuration
// ========================================================================

namespace mitm {

/// Thread priority for the MITM service
const s32 ThreadPriority = 6;

/// Thread stack size
// 32 KB: needed because miniupnpc's upnpDiscover() and minissdpc helpers
// each allocate ~2 KB of stack frames; cumulative with IPC dispatch +
// ICommunicationService::CreateNetwork → NatPunch → Discover, 16 KB
// overflowed and DABRT'd on the very first call into upnpDiscover.
const size_t ThreadStackSize = 0x8000;

/// Thread stack
alignas(os::MemoryPageSize) u8 g_thread_stack[ThreadStackSize];
/// Thread object of the main MITM server thread (uses g_thread_stack).
os::ThreadType g_thread;

// Heap for dynamic allocations
// NOTE: 384KB covers: game whitelist (~40KB), proxy socket receive queues
// (up to ~45KB under lobby traffic with 900+ byte packets), pending-packet
// buffer, ExpHeap overhead/fragmentation, and transient std::vector/std::deque
// allocations. 96KB saturated under real gameplay traffic and caused DABRT
// 0x101 on allocation failure.
/// Backing memory of the 384 KB expanded heap used by new/delete.
alignas(internal::HEAP_ALIGNMENT) constinit u8
    g_heap_memory[internal::HEAP_SIZE];
/// Handle of the expanded heap; valid once g_heap_initialized is true.
constinit lmem::HeapHandle g_heap_handle;
/// Set under g_heap_init_mutex when the heap has been created (lazy init).
/// Atomic because the fast path reads it without the lock: the release store
/// below publishes g_heap_handle to the acquire load in GetHeapHandle().
constinit std::atomic<bool> g_heap_initialized{false};
// Heap init mutex. `os::SdkMutex` provides a constexpr constructor that
// constant-initializes the underlying critical section on Horizon (the
// `AMS_OS_INTERNAL_CRITICAL_SECTION_IMPL_CONSTANT_INITIALIZER` path), so
// `constinit` alone yields a usable lock. We still re-initialize it
// explicitly in `InitializeSystemModule()` to make the dependency on
// lock availability audible and to harden against non-Horizon toolchain
// paths (pthread/windows) where the constexpr initializer may not fully
// seed the critical section. `InitializeSdkMutex` is idempotent on an
// already-initialized mutex — it just re-seeds the storage.

/// Guards the one-time creation of the expanded heap in GetHeapHandle().
constinit os::SdkMutex g_heap_init_mutex;

/**
 * @brief Returns the expanded heap handle, creating the heap on first use.
 *
 * Double-checked locking on g_heap_initialized; the heap is thread safe
 * (CreateOption_ThreadSafe).
 *
 * @return Handle of the 384 KB expanded heap.
 */
lmem::HeapHandle GetHeapHandle() {
    if (AMS_LIKELY(g_heap_initialized.load(std::memory_order_acquire))) {
        return g_heap_handle;
    }

    std::scoped_lock lk(g_heap_init_mutex);
    // Re-checked under the lock: another thread may have created the heap
    // while this one waited for it. Relaxed is enough here because the lock
    // itself orders this read against the creating thread's release store.
    if (!g_heap_initialized.load(std::memory_order_relaxed)) {
        g_heap_handle = lmem::CreateExpHeap(
            g_heap_memory,
            sizeof(g_heap_memory),
            lmem::CreateOption_ThreadSafe);
        g_heap_initialized.store(true, std::memory_order_release);
    }

    return g_heap_handle;
}

/**
 * @brief Allocates from the expanded heap (target of the new operators).
 *
 * @param size Number of bytes to allocate.
 * @return Pointer to the block, or nullptr if the heap is exhausted.
 */
void* Allocate(size_t size) {
    return lmem::AllocateFromExpHeap(GetHeapHandle(), size);
}

/**
 * @brief Returns a block to the expanded heap.
 *
 * @param p Block obtained from Allocate().
 * @param size Ignored; the heap tracks block sizes itself.
 */
void Deallocate(void* p, size_t size) {
    AMS_UNUSED(size);
    lmem::FreeToExpHeap(GetHeapHandle(), p);
}

/// HIPC server manager driving the ldn:u and bsd:u MITM services.
ServerManager g_server_manager;

/// Monotonic counter assigning a unique ID to each accepted ldn:u session.
u32 g_ldn_session_counter = 0;

/// Monotonic counter assigning a unique ID to each accepted bsd:u session.
u32 g_bsd_session_counter = 0;

Result ServerManager::OnNeedsToAccept(int port_index, Server* server) {
    LOG_INFO("OnNeedsToAccept: port_index=%d, server=%p", port_index, server);

    // Acknowledge the MITM session
    std::shared_ptr<::Service> fsrv;
    sm::MitmProcessInfo client_info{};
    server->AcknowledgeMitmSession(
        std::addressof(fsrv), std::addressof(client_info));
    // The emplaced services take the forwarded session by rvalue reference:
    // hand each one its own copy so `fsrv` stays valid for AcceptMitmImpl.
    std::shared_ptr<::Service> session_copy = fsrv;

    LOG_INFO(
        "OnNeedsToAccept: Acknowledged session for pid=%lu, "
        "program_id=0x%016lx, fsrv=%p (handle=0x%x)",
        client_info.process_id.value,
        client_info.program_id.value,
        fsrv.get(),
        fsrv ? fsrv->session : 0);

    Result rc;
    switch (port_index) {
        case PortIndex_LdnMitm: {
            u32 session_id = ++g_ldn_session_counter;
            LOG_INFO(
                "LDN MITM: Creating session #%u for pid=%lu",
                session_id,
                client_info.process_id.value);
            // LDN MITM service (ldn:u)
            rc = this->AcceptMitmImpl(
                server,
                sf::CreateSharedObjectEmplaced<
                    mitm::ldn::ILdnMitMService,
                    mitm::ldn::LdnMitMService>(
                    std::move(session_copy), client_info),
                fsrv);
            LOG_INFO(
                "LDN AcceptMitmImpl result: 0x%x (session #%u)",
                rc.GetValue(),
                session_id);
        }
            return rc;

        case PortIndex_BsdMitm: {
            u32 session_id = ++g_bsd_session_counter;
            LOG_INFO(
                "BSD MITM: Creating session #%u for pid=%lu",
                session_id,
                client_info.process_id.value);
            // BSD MITM service (bsd:u)
            rc = this->AcceptMitmImpl(
                server,
                sf::CreateSharedObjectEmplaced<
                    mitm::bsd::IBsdMitmService,
                    mitm::bsd::BsdMitmService>(
                    std::move(session_copy), client_info),
                fsrv);
            LOG_INFO(
                "BSD AcceptMitmImpl result: 0x%x (session #%u)",
                rc.GetValue(),
                session_id);
        }
            return rc;

        default:
            AMS_ABORT("Unknown port index");
    }
}

namespace {

// Extra threads for parallel request handling

/// One page-aligned stack per extra worker thread.
alignas(os::MemoryPageSize) u8
    g_extra_thread_stacks[NumExtraThreads][ThreadStackSize];
/// Thread objects of the extra MITM worker threads.
os::ThreadType g_extra_threads[NumExtraThreads];

/**
 * @brief Thread body: serves MITM IPC requests until the manager stops.
 *
 * The thread argument is not used.
 */
void LoopServerThread(void* /*unused*/) {
    g_server_manager.LoopProcess();
}

/**
 * @brief Entry of the main MITM thread: starts the extra workers, serves
 *        requests itself, then joins the workers.
 *
 * The thread argument is not used.
 */
void ProcessForServerOnAllThreads(void* /*unused*/) {
    // Create every extra thread first, then start them: the stacks and thread
    // objects only exist when NumExtraThreads is non-zero, hence the guard.
    if constexpr (NumExtraThreads > 0) {
        const s32 priority =
            os::GetThreadCurrentPriority(os::GetCurrentThread());
        for (size_t i = 0; i < NumExtraThreads; i++) {
            R_ABORT_UNLESS(
                os::CreateThread(
                    g_extra_threads + i,
                    LoopServerThread,
                    nullptr,
                    g_extra_thread_stacks[i],
                    ThreadStackSize,
                    priority));
            os::SetThreadNamePointer(g_extra_threads + i, "ryu_ldn::Thread");
        }
        for (size_t i = 0; i < NumExtraThreads; i++) {
            os::StartThread(g_extra_threads + i);
        }
    }

    // Loop this thread
    LoopServerThread(nullptr);

    // Wait for extra threads to finish
    if constexpr (NumExtraThreads > 0) {
        for (size_t i = 0; i < NumExtraThreads; i++) {
            os::WaitThread(g_extra_threads + i);
        }
    }
}

}  // namespace

}  // namespace mitm

// ========================================================================
// Configuration IPC Service (ryu:cfg)
// ========================================================================

namespace cfg {

/// Thread priority for config service
const s32 ThreadPriority = 10;

/// Thread stack size
const size_t ThreadStackSize = 0x2000;

/// Thread stack
alignas(os::MemoryPageSize) u8 g_thread_stack[ThreadStackSize];
/// Thread object of the ryu:cfg server thread (uses g_thread_stack).
os::ThreadType g_thread;

/// HIPC server manager driving the custom ryu:cfg IPC service.
ConfigServerManager g_config_server_manager;

/// Config service thread entry point
void LoopConfigServerThread(void* /*unused*/) {
    g_config_server_manager.LoopProcess();
}

/// Log maintenance thread stack
alignas(os::MemoryPageSize) u8 g_log_thread_stack[LOG_THREAD_STACK_SIZE];
/// Thread object of the log maintenance thread.
os::ThreadType g_log_thread;

/// Log maintenance thread entry point (checks file idle timeout)
void LoopLogMaintenanceThread(void* /*unused*/) {
    while (true) {
        // Sleep for 2 seconds
        svc::SleepThread(
            TimeSpan::FromSeconds(LOG_MAINTENANCE_INTERVAL_SEC)
                .GetNanoSeconds());

        // Check if log file should be closed due to idle timeout
        ryu_ldn::debug::g_logger.check_idle_timeout();
    }
}

}  // namespace cfg

// ========================================================================
// System Module Initialization
// ========================================================================

// ========================================================================
namespace init {

/**
 * @brief Boot-time initialization: sm, filesystem and SD mount, config and
 *        logger, game whitelist, then nifm, bsd and socket services.
 *
 * Any failure aborts (R_ABORT_UNLESS); config load problems fall back to
 * defaults.
 */
void InitializeSystemModule() {
    // Initialize service manager connection
    R_ABORT_UNLESS(sm::Initialize());

    // Host-test safety net: re-seeds the mutex storage on non-Horizon
    // platforms. On Horizon (`__SWITCH__`), `constinit` already
    // constant-initializes the underlying critical section, so the
    // explicit `InitializeSdkMutex` call is redundant and is skipped
    // to avoid a double-init on the SdkMutex. The host test build
    // (g++ with -DTEST_BUILD) has no `constinit` zero-init guarantee
    // for the mutex internals, so we re-seed there.
#ifndef __SWITCH__
    os::InitializeSdkMutex(std::addressof(mitm::g_heap_init_mutex));
#endif

    // Initialize filesystem
    fs::InitializeForSystem();
    fs::SetAllocator(mitm::Allocate, mitm::Deallocate);
    fs::SetEnabledAutoAbort(false);

    // Mount SD card for configuration
    R_ABORT_UNLESS(fs::MountSdCard("sdmc"));

    // Ensure config file exists (create with defaults if not)
    ryu_ldn::config::ensure_config_exists(ryu_ldn::config::CONFIG_PATH);

    // Load configuration
    ryu_ldn::config::Config config = ryu_ldn::config::get_default_config();
    ryu_ldn::config::load_config(ryu_ldn::config::CONFIG_PATH, config);

    // Initialize logger with debug settings
    ryu_ldn::debug::g_logger.init(config.debug, ryu_ldn::config::LOG_PATH);
    LOG_INFO("ryu_ldn_nx sysmodule starting");
    LOG_INFO("Config loaded from %s", ryu_ldn::config::CONFIG_PATH);

    // Load game whitelist from file (once at startup)
    ryu_ldn::config::LoadWhitelist();
    LOG_VERBOSE("Server: %s:%u", config.server.host, config.server.port);

    // Initialize network services
    R_ABORT_UNLESS(nifmInitialize(NifmServiceType_Admin));
    R_ABORT_UNLESS(bsdInitialize(
        &internal::LibnxBsdInitConfig,
        internal::LibnxSocketInitConfig.num_bsd_sessions,
        internal::LibnxSocketInitConfig.bsd_service_type));
    R_ABORT_UNLESS(socketInitialize(&internal::LibnxSocketInitConfig));
}

/// Flushes logs and shuts down socket, bsd, nifm and the SD mount, in
/// reverse initialization order.
void FinalizeSystemModule() {
    LOG_INFO("ryu_ldn_nx sysmodule shutting down");

    // Flush logs first, then tear down in reverse init order:
    // socket → bsd → nifm. The socket layer must exit before BSD
    // because libnx socket cleanup closes file descriptors that the
    // BSD service still tracks.
    ryu_ldn::debug::g_logger.flush();
    socketExit();
    bsdExit();
    nifmExit();
    fs::Unmount("sdmc");
}

/// Early boot hook: sets up the malloc allocator on g_malloc_buffer.
void Startup() {
    // Initialize the global malloc allocator
    init::InitializeAllocator(
        internal::g_malloc_buffer, sizeof(internal::g_malloc_buffer));
}

}  // namespace init

// ========================================================================
// Exit Handler (should never be called)
// ========================================================================

/**
 * @brief Process exit hook; must never run for this immortal sysmodule.
 *
 * @param rc Exit code (unused); the function aborts.
 * @return Never returns.
 */
void NORETURN Exit(int rc) {
    AMS_UNUSED(rc);
    AMS_ABORT("Exit called by immortal process");
}

// ========================================================================
// Main Entry Point
// ========================================================================

/**
 * @brief Sysmodule main: registers ryu:cfg, ldn:u and bsd:u, starts the
 *        config, log-maintenance and MITM threads, then blocks forever on
 *        the MITM thread.
 */
void Main() {
    // Initialize global configuration for IPC service
    ryu_ldn::ipc::InitializeConfig();

    // ====================================================================
    // Register ryu:cfg configuration service
    // ====================================================================
    LOG_INFO("Registering ryu:cfg config service");
    constexpr sm::ServiceName ConfigServiceName =
        sm::ServiceName::Encode("ryu:cfg");

    // Create the config service object and register it
    auto config_service = sf::CreateSharedObjectEmplaced<
        ryu_ldn::ipc::IConfigService,
        ryu_ldn::ipc::ConfigService>();

    R_ABORT_UNLESS(
        cfg::g_config_server_manager.RegisterObjectForServer(
            config_service, ConfigServiceName, cfg::MaxSessions));
    LOG_INFO("Config service ryu:cfg registered successfully");

    // Create config service thread
    R_ABORT_UNLESS(
        os::CreateThread(
            &cfg::g_thread,
            cfg::LoopConfigServerThread,
            nullptr,
            cfg::g_thread_stack,
            cfg::ThreadStackSize,
            cfg::ThreadPriority));

    os::SetThreadNamePointer(&cfg::g_thread, "ryu_ldn::CfgThread");
    os::StartThread(&cfg::g_thread);

    // Create log maintenance thread (for idle timeout)
    R_ABORT_UNLESS(
        os::CreateThread(
            &cfg::g_log_thread,
            cfg::LoopLogMaintenanceThread,
            nullptr,
            cfg::g_log_thread_stack,
            sizeof(cfg::g_log_thread_stack),
            cfg::ThreadPriority +
                LOG_THREAD_PRIORITY_OFFSET));  // Lower priority than config
                                               // service

    os::SetThreadNamePointer(&cfg::g_log_thread, "ryu_ldn::LogThread");
    os::StartThread(&cfg::g_log_thread);

    // ====================================================================
    // Register MITM services
    // ====================================================================

    // Register ldn:u MITM service (port 0)
    LOG_INFO("Registering ldn:u MITM service");
    constexpr sm::ServiceName LdnMitmServiceName =
        sm::ServiceName::Encode("ldn:u");
    R_ABORT_UNLESS(
        (mitm::g_server_manager.RegisterMitmServer<mitm::ldn::LdnMitMService>(
            mitm::PortIndex_LdnMitm, LdnMitmServiceName)));
    LOG_INFO("ldn:u MITM service registered successfully");

    // Register bsd:u MITM service (port 1)
    // This allows us to intercept game sockets that target LDN addresses
    // (10.114.x.x)
    LOG_INFO("Registering bsd:u MITM service");
    constexpr sm::ServiceName BsdMitmServiceName =
        sm::ServiceName::Encode("bsd:u");
    R_ABORT_UNLESS(
        (mitm::g_server_manager.RegisterMitmServer<mitm::bsd::BsdMitmService>(
            mitm::PortIndex_BsdMitm, BsdMitmServiceName)));
    LOG_INFO("bsd:u MITM service registered successfully");

    // Create MITM processing thread
    R_ABORT_UNLESS(
        os::CreateThread(
            &mitm::g_thread,
            mitm::ProcessForServerOnAllThreads,
            nullptr,
            mitm::g_thread_stack,
            mitm::ThreadStackSize,
            mitm::ThreadPriority));

    os::SetThreadNamePointer(&mitm::g_thread, "ryu_ldn::MainThread");
    os::StartThread(&mitm::g_thread);

    // Wait for MITM thread (runs forever)
    // Note: Config thread also runs forever in parallel
    os::WaitThread(&mitm::g_thread);
}

}  // namespace ams

// ============================================================================
// Custom Memory Allocators
// ============================================================================

// Exception specifications match the standard declarations (C++17
// [basic.stc.dynamic]): throwing new is implicitly noexcept(false), nothrow new
// and ALL delete overloads are noexcept. GCC accepts the mismatch but
// clang-tidy flags it, and an exception escaping a delete would terminate
// mid-heap-operation.

/**
 * @brief Global operator new, served by the 384 KB expanded heap.
 *
 * Returns nullptr on exhaustion instead of throwing (exceptions are not used
 * in this sysmodule).
 *
 * @param size Number of bytes requested.
 * @return Block address, or nullptr if the heap is exhausted.
 */
void* operator new(size_t size) {
    return ams::mitm::Allocate(size);
}

/**
 * @brief Nothrow operator new, served by the expanded heap.
 *
 * @param size Number of bytes requested.
 * @return Block address, or nullptr if the heap is exhausted.
 */
void* operator new(size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return ams::mitm::Allocate(size);
}

/**
 * @brief Global operator delete; returns the block to the expanded heap.
 *
 * @param p Block from operator new (must not come from another allocator).
 */
void operator delete(void* p) noexcept {
    ams::mitm::Deallocate(p, 0);
}

/**
 * @brief Sized operator delete; the size is ignored by the heap.
 *
 * @param p Block from operator new.
 * @param size Size of the block (unused).
 */
void operator delete(void* p, size_t size) noexcept {
    ams::mitm::Deallocate(p, size);
}

/**
 * @brief Array operator new, served by the expanded heap.
 *
 * @param size Number of bytes requested.
 * @return Block address, or nullptr if the heap is exhausted.
 */
void* operator new[](size_t size) {
    return ams::mitm::Allocate(size);
}

/**
 * @brief Nothrow array operator new, served by the expanded heap.
 *
 * @param size Number of bytes requested.
 * @return Block address, or nullptr if the heap is exhausted.
 */
void* operator new[](size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return ams::mitm::Allocate(size);
}

/**
 * @brief Array operator delete; returns the block to the expanded heap.
 *
 * @param p Block from operator new[].
 */
void operator delete[](void* p) noexcept {
    ams::mitm::Deallocate(p, 0);
}

/**
 * @brief Sized array operator delete; the size is ignored by the heap.
 *
 * @param p Block from operator new[].
 * @param size Size of the block (unused).
 */
void operator delete[](void* p, size_t size) noexcept {
    ams::mitm::Deallocate(p, size);
}

// ============================================================================
// C++17 aligned allocation overloads
// ============================================================================
// The custom expanded heap (lmem::ExpHeap) has a fixed alignment that is
// already sufficient for all standard types. These overloads delegate to the
// existing non-aligned versions, ignoring the requested alignment. This
// satisfies the C++17 standard library which calls these overloads when
// allocating over-aligned types (e.g., alignas(64) buffers).
//
// Without these overloads, the linker fails to resolve the aligned new/delete
// symbols on toolchains that define them (devkitPro/gcc 8.1+), causing a
// build error whenever std::aligned_alloc or an over-aligned type is used.

/**
 * @brief Aligned operator new; the requested alignment is ignored.
 *
 * @param size Number of bytes requested.
 * @return Block address, or nullptr if the heap is exhausted.
 */
void* operator new(size_t size, std::align_val_t /*alignment*/) {
    return ams::mitm::Allocate(size);
}

/**
 * @brief Aligned nothrow operator new; the requested alignment is ignored.
 *
 * @param size Number of bytes requested.
 * @return Block address, or nullptr if the heap is exhausted.
 */
void* operator new(
    size_t size,
    std::align_val_t /*alignment*/,
    const std::nothrow_t& /*tag*/) noexcept {
    return ams::mitm::Allocate(size);
}

/**
 * @brief Aligned operator delete.
 *
 * @param p Block from an aligned operator new.
 */
void operator delete(void* p, std::align_val_t /*alignment*/) noexcept {
    ams::mitm::Deallocate(p, 0);
}

/**
 * @brief Aligned sized operator delete; the size is ignored.
 *
 * @param p Block from an aligned operator new.
 * @param size Size of the block (unused).
 */
void operator delete(
    void* p, size_t size, std::align_val_t /*alignment*/) noexcept {
    ams::mitm::Deallocate(p, size);
}

/**
 * @brief Aligned array operator new; the requested alignment is ignored.
 *
 * @param size Number of bytes requested.
 * @return Block address, or nullptr if the heap is exhausted.
 */
void* operator new[](size_t size, std::align_val_t /*alignment*/) {
    return ams::mitm::Allocate(size);
}

/**
 * @brief Aligned nothrow array operator new; alignment is ignored.
 *
 * @param size Number of bytes requested.
 * @return Block address, or nullptr if the heap is exhausted.
 */
void* operator new[](
    size_t size,
    std::align_val_t /*alignment*/,
    const std::nothrow_t& /*tag*/) noexcept {
    return ams::mitm::Allocate(size);
}

/**
 * @brief Aligned array operator delete.
 *
 * @param p Block from an aligned operator new[].
 */
void operator delete[](void* p, std::align_val_t /*alignment*/) noexcept {
    ams::mitm::Deallocate(p, 0);
}

/**
 * @brief Aligned sized array operator delete; the size is ignored.
 *
 * @param p Block from an aligned operator new[].
 * @param size Size of the block (unused).
 */
void operator delete[](
    void* p, size_t size, std::align_val_t /*alignment*/) noexcept {
    ams::mitm::Deallocate(p, size);
}
