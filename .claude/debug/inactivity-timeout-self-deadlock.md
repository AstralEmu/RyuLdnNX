# Inactivity timeout re-locks g_active_service_mutex

## Symptom
Host suite `ldn_icommunication_real_tests`: `Finalize()` never returned after a station had been idle for the 6 s inactivity timeout (`DEFAULT_IDLE_TIMEOUT_MS`).

## Reproduction
Open a station against the loopback fake server, finish a scan (this arms the timeout), stay idle past 6 s, then call `Finalize()`.

## Proof
1. Temporary `fprintf` around the receive loop (removed) showed the receive thread entering `m_inactivity_timeout.CheckTimeout()` and never leaving it; `/proc/<pid>/task/*/wchan` showed `futex_wait`.
2. `CheckTimeout` calls `ICommunicationService::OnInactivityTimeout`, which takes `g_active_service_mutex` and then calls `DisconnectFromServer()`, which takes the same mutex again to unregister the BSD callback. The mutex was declared `os::Mutex{false}` (non-recursive).
3. After switching it to `{true}` the hang persisted. A trace in the stub `Mutex::lock()` printed `block ... rec=0 me==owner`, i.e. the blocked mutex was NOT recursive even though the source says `{true}`. A trace in the constructor never printed for that address, and a static initializer placed around the definition never ran before the tests.
4. Cause of 3: `test_harness.hpp` runs every `TEST()` body from a static initializer, and the test object was first in the link line, so the tests ran before the static initializers of the service sources. `g_active_service_mutex` was still zero-filled (a valid, non-recursive `std::mutex`). `main()` had not run either, so `g_host_wait_divisor = 10` was also not in effect.

## Cause
Two independent things:
- Product: relocking a non-recursive `os::Mutex` from the same thread. On the host stand-in it blocks forever; on Atmosphere it aborts the process. The idle timeout firing while a server connection is open and no network is joined takes this path. Not validated on a console.
- Test build: the suite ran the tests before the sources' statics were constructed (link order + `TEST()` semantics).

## Fix
- `g_active_service_mutex` is recursive (`os::Mutex{true}`); every holder is a same-thread nesting, no other lock order changes.
- In `tests/Makefile` the test object is linked last for this suite, and the wait divisor is set from a `constructor(101)` function instead of `main()`.

## Validation
With the tests ordered correctly and the mutex put back to `{false}`, the suite hangs (killed by `timeout 60`) at `scan_reports_a_dropped_connection_as_a_send_failure`, right after the scan tests armed the timeout: the defect is real, independent of the ordering problem.
With `{true}`: `run_ldn_icommunication_real_tests` 72/72 pass, including `the_inactivity_timeout_disconnects_an_idle_station` (server sees the connection closed within 12 s, `Finalize()` returns).
