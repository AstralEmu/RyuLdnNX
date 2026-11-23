
## Push (2026-11-23)

- Wiki push denied: `git -C ../RyuLdnNX.wiki push` -> `remote: Permission to AstralEmu/RyuLdnNX.wiki.git denied to REMOVED. ... 403` (wiki origin is https://github.com/AstralEmu/RyuLdnNX.wiki.git, code origin is git@github-ethiquema:Ethiquema/ryu_ldn_nx.git).
- Code not pushed either: the owner asked for wiki and code to be pushed together.
- Needs the owner: credentials or the correct wiki remote.

## Owner actions needed (2026-11-23)

- `_FORTIFY_SOURCE=3` dropped from tests/Makefile: GCC 12.2 raises a false `-Wstringop-overflow` (tests/protocol_tests.cpp:680, :1837 via sysmodule/source/protocol/packet_buffer.hpp:128). Real fix needs a newer GCC in the test image.
- `scripts/codeql-create.sh` and `scripts/codeql-analyze.sh` swallow command failures through forced-success constructs; protected path for the agent, owner must fix or delete them.
- Untracked stray `sysmodule/source/ldn/ldn_icommunication.cpp.bak`: owner to delete.
- `hooks/pre-commit` is modified in the working tree; the owner commits it.
- Untracked stray `tests/_probe_move.cpp` (my use-after-move probe): deleting it from the shell is refused by `.claude/hooks/guard-no-test-deletion.py`; owner to delete it, never commit it.

## clang-tidy findings not fixable in sysmodule/source (2026-11-23)

- `/usr/include/.../sys/socket.h` + `sys/poll.h` readability-inconsistent-declaration-parameter-name: the "definition seen here" notes all point to tests/socket_tests.cpp:1404,1443,1463,1486,1504,1522 and tests/dns_wrap_tests.cpp:166,192 (forbidden path), none to sysmodule/overlay.
- `sysmodule/source/bsd/bsd_mitm_service.cpp:749` `sf::CopyHandle&& transfer_memory`: with `std::move` -> hicpp/performance-move-const-arg ("trivially-copyable"); with `std::forward`/`static_cast` -> cppcoreguidelines-rvalue-reference-param-not-moved. Cause: tests/stubs CopyHandle is trivially copyable, the real one is not (move needed on target). The signature is fixed by the IPC macros (ibsd_mitm_service.hpp:66). Left with std::move.
- `p2p_proxy_client.cpp:620` and `p2p_proxy_server.cpp:2239` clang-analyzer-unix.BlockInCriticalSection: notes show "Entering critical section here" at tests/stubs/stratosphere.hpp:143 (stub Mutex::lock) and no exit after the scoped_lock/unique_lock destructor (stub unlock() re-branches on m_recursive after the g_host_before_plain_lock(this) callback). Stub is in tests/ (forbidden); lock_guard/unique_lock variants tried, same result.

## clang-tidy: bugprone-suspicious-include (2026-11-23)

- `tests/main_tests.cpp:21` (`#include "main.cpp"`) and `tests/bsd_mitm_service_tests.cpp:21` (`#include "bsd/bsd_mitm_service.cpp"`) are deliberate unity builds (see the comments above `$(TARGET_MAIN)` and `$(TARGET_BSD_MITM_SERVICE)` in tests/Makefile) that reach file-local state (`g_socket_info`, `g_mitm_pid_count`, server managers, heap).
- Real fix: an internal test-seam header exposing that state from the sources, which changes Switch-side translation units; not done without a behaviour-preserving proof. Owner decision needed.

## -Wthread-safety not wired (2026-11-23)

- Measured: `grep -rEc "GUARDED_BY|REQUIRES\(|ACQUIRE\(|lockable|capability\(" sysmodule/source tests/stubs` returns no file with a match, while 22 files under sysmodule/source use a mutex. clang 21 is in the lint image, but without capability annotations `-Wthread-safety` has nothing to check, so a green run would prove nothing.
- Real fix: annotate the stub `Mutex` (tests/stubs/stratosphere.hpp, protected path) as a capability and every guarded member through macros that expand to nothing under devkitA64 GCC. Touches the Switch-side headers and the stub; needs the owner's decision and a behaviour-preserving proof.
