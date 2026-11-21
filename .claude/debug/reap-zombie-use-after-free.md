# ReapZombieSessions frees a finished session before joining its thread

## Symptom
Running tests/p2p_proxy_server_real_tests.cpp (new suite) segfaults
non-deterministically (exit 139) at the first test that lets a session end
before the server is destroyed.

## Reproduction
Same binary rebuilt with `g++ -fsanitize=address -g` (devkit container):

    ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8
    #0 std::thread::joinable()
    #1 ams::os::WaitThread
    #2 P2pProxyServer::ReapZombieSessions() p2p_proxy_server.cpp:1645
    freed by thread T0 here:
    #6 P2pProxyServer::ReapZombieSessions() p2p_proxy_server.cpp:1632
    (std::unique_ptr<P2pProxySession>::reset)

## Cause (proved by the trace above + the code)
`m_zombie_sessions` is `std::unique_ptr<P2pProxySession>[]`. The snapshot loop
only copies the raw pointer of a finished zombie (`.get()`), it never releases
the slot. The compaction (`m_zombie_sessions[kept++].reset(z)`) then resets a
slot onto a pointer it already owns (reset(p) deletes the old pointer, i.e. the
object it just stored when kept == i), and the tail loop
(`for i = kept..MAX: reset()`) deletes every finished zombie that was not moved.
The later `os::WaitThread(&z->m_recv_thread)` / `delete z` therefore touch freed
memory and free it a second time. The comment in the code ("the owning slot was
already reset above") describes the intent, not what the code does.

## Fix
Take ownership explicitly: `release()` every slot while scanning, re-seat only the
still-running zombies with `reset()`, so a finished one is owned by the local
array alone until `delete z`.

## Status
Reproduced on host (ASAN). Fix validated by the same suite under ASAN.
Not validated on a console.

# Second and third defect found by the same suite

## OnSessionDisconnected deletes the session it then uses
`OnSessionDisconnected` (p2p_proxy_server.cpp, found branch) calls
`m_session.reset()` on the `m_sessions` slot: that deletes the P2pProxySession
from its own receive thread, then reads `session->GetVirtualIpAddress()` and
re-adopts the freed pointer in the zombie array (`.reset(session)`), which
deletes it a second time at reap. On the target the destructor does not join,
so the object (and its stack slot, released in the destructor) goes away while
its thread still unwinds. Fix: `release()` the slot, ownership moves to the
zombie queue only.

## Stop() deadlocks with a session thread that is disconnecting
Proof (gdb, host build of the suite, thread dump of the hung run):
- main: `Stop()` p2p_proxy_server.cpp:568 `m_session.reset()` while holding
  `m_mutex`, inside the session destructor chain waiting for the thread to end;
- recv thread: `ReceiveLoop` -> `Disconnect(false)` -> `OnSessionDisconnected`
  :1714 blocked on `m_mutex`.
Trigger: a peer closes its socket while the server is being stopped.
Fix: Stop() no longer destroys sessions under the lock: each live session is
disconnected and moved to the zombie queue (a second queueing of the same
session by its own thread is ignored), then Stop waits (bounded) for those
threads to finish and reaps them outside the lock.

## Leak when the session stack pool is exhausted

Symptom: LeakSanitizer, one direct leak of 65592 bytes (a `P2pProxySession`)
allocated in `P2pProxyServer::AcceptLoop`, after the test
`the_session_stack_pool_runs_out_after_eight_unauthenticated_peers`.

Cause (proved by the ASAN report and by reading the code): with no free stack
slot `P2pProxySession::Start` only sets `m_connected = false` and returns; no
thread exists, so nothing ever deletes the session, and `AcceptLoop` ignored
the outcome. The 64 KB session and the client fd leaked for every peer beyond
the pool size.

Fix: `AcceptLoop` deletes the session when `IsConnected()` is false after
`Start()` (the destructor closes the fd). Also dropped a dead
`if (session != nullptr)` that followed the null check.

Validation: same ASAN build, 51/51 pass, no sanitizer report.
Not validated on a console.

## Unauthenticated sessions outlive the server (use after free in a recv thread)

Symptom: the non-ASAN host build of `p2p_proxy_server_real_tests` crashed
intermittently (SIGSEGV, 1 run out of ~3). The ASAN build never showed it.

Reproduction (gdb, 8 runs): thread `SessionRecvThreadEntry -> ReceiveLoop ->
Disconnect(false) -> P2pProxyServer::OnSessionDisconnected` with `this` = a
server that no longer exists; the zombie-array walk at line 1749 read past the
stack (`this=0x7fff4b232000`). A second crash showed `~P2pProxySession` called
with `this=0x1` from `ReapZombieSessions` (memory of a dead server reused by the
next test's server).

Cause (proved by reading `AcceptLoop` + `Stop`): a session is only put in
`m_sessions[]` after authentication. A peer that connects and closes (or never
authenticates) runs a receive thread owned by nobody: `Stop()` could neither
disconnect it nor wait for it, so the thread later called into the freed server.
On the target the server is a `unique_ptr` of the LDN service, so the same
window exists when the host leaves the network.

Fix: `m_live_sessions[]` (non-owning, MAX_PLAYERS + 1) lists every started
session. `AcceptLoop` registers and starts the session under `m_mutex` (and
refuses when `m_running` is false); `OnSessionDisconnected` unregisters;
`Stop()` disconnects every live session, hands it to the zombie queue and waits
for its thread. Validation: ASAN 54/54 clean, non-ASAN build 15 runs without a
crash.

## Spoofed ProxyData still reached the host

Symptom (test `a_spoofed_source_never_reaches_the_host`, failed before the
fix with `host_count() == 0u` violated): a joiner sending ProxyData whose
source IP is another joiner's, addressed to the host's virtual IP or to a
broadcast, was dropped for the other joiners but delivered to the host's
in-process callback with the forged source.

Cause: `RouteMessage` rejected the spoof with a bare `return`; its caller
`HandleProxyData` ignored that and went on to the host delivery (the comment
there claimed the spoof had already been rejected).

Fix: `RouteMessage` returns false on rejection, `HandleProxyData` returns
immediately. Wire format and legitimate traffic are untouched (only forged
packets are now dropped). Consequence: `HostShouldReceive`'s legacy-broadcast
branch (`0xc0a800ff`) was reachable only through that bug, since `RouteMessage`
already rewrites the destination; it is removed as dead code.
