
## Push (2026-11-23)

- Wiki push denied: `git -C ../RyuLdnNX.wiki push` -> `remote: Permission to AstralEmu/RyuLdnNX.wiki.git denied to REMOVED. ... 403` (wiki origin is https://github.com/AstralEmu/RyuLdnNX.wiki.git, code origin is git@github-ethiquema:Ethiquema/ryu_ldn_nx.git).
- Code not pushed either: the owner asked for wiki and code to be pushed together.
- Needs the owner: credentials or the correct wiki remote.

## Owner actions needed (2026-11-23)

- `_FORTIFY_SOURCE=3` dropped from tests/Makefile: GCC 12.2 raises a false `-Wstringop-overflow` (tests/protocol_tests.cpp:680, :1837 via sysmodule/source/protocol/packet_buffer.hpp:128). Real fix needs a newer GCC in the test image.
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

## -fanalyzer (GCC) not a gate (2026-11-23)

- `g++ -std=c++20 -DTEST_BUILD -fanalyzer -c` over every `sysmodule/source/*.cpp` (build-logs/fanalyzer-all.log): 3 warnings. `config.hpp:332` `-Wanalyzer-malloc-leak` x2 (open_stream(), whose result is owned by a `std::unique_ptr<FILE>` in config.cpp:636/723/765) and `ldn_icommunication.cpp:3201` `-Wanalyzer-null-argument` (`this` of a captureless lambda's `_FUN` stub).
- Tried: replacing the `&std::fclose` deleter with a named functor calling `std::fclose` directly; the leak warnings stayed identical, change reverted. GCC documents C++ support of `-fanalyzer` as limited; no source-level fix without suppressing, which is forbidden. Owner decision: keep it out of the gates (clang-analyzer runs via clang-tidy).

## zizmor: 5 findings left in .github/workflows (2026-11-23)

Measured with `docker compose run -T --rm --entrypoint zizmor lint .github/workflows` (build-logs/zizmor.log): 52 findings at the start, 5 after fixing 13 `artipacked` (persist-credentials: false) and `excessive-permissions` (game-hub-manager.yml, permissions moved to the 5 jobs). Left:
- `artipacked` x3 (issue-automation.yml create-branch job, submodule-updater.yml, tag-release.yml): these jobs `git push` with the checkout credentials. Fix = pass the token to the push step explicitly; not testable locally, a mistake breaks release tagging. Owner decision.
- `unpinned-images` codeql.yml:23 `container: ghcr.io/ethiquema/ryu_ldn_nx:latest`: pinning by digest freezes a CI-built image that changes on every build; needs a versioned tag scheme in build-container.yml. Owner decision.
- `dangerous-triggers` issue-automation.yml:6 `issue_comment`: the job at :581 reacts to comments; needs a review of what it does with the comment text. Owner decision.

## Weak PRNG for generated passphrases (2026-11-23)

flawfinder (level >= 3, build-logs/flawfinder.log): `srand(time)` + `rand()` generate the `Ryujinx-xxxxxxxx` passphrase in sysmodule/source/config/config_manager.cpp:67-78, overlay/source/utils/formatters.hpp:131-137 and overlay/source/views/connection/main_dashboard_gui.cpp:30,373: predictable output. Real fix: `randomGet` (libnx, already used at sysmodule/source/network/client.cpp:1192) on Switch and `getrandom` on the host build, behind a test seam. It changes Switch-side code and needs a behaviour proof; not done unattended. The two format hits (config.cpp:380, log.cpp:110) are false positives (constant format strings).

## valgrind memcheck cannot judge the allocator-overriding suites (2026-11-23)

- Measured: `valgrind --error-exitcode=9 --leak-check=full --errors-for-leak-kinds=definite,indirect` over all 46 host binaries: 43 clean, 3 failing (`run_ldn_icommunication_real_tests` 124/127, `run_main_tests` 10/12, `run_p2p_proxy_server_real_tests` SIGSEGV at p2p_proxy_server.hpp:671). All three replace the global `operator new` (tests/main_tests.cpp unity build of main.cpp, the scoped allocation failure, the session tracker of p2p_proxy_server_real_tests.cpp).
- Cause, proven with a minimal probe (build-logs/vgprobe.cpp, build-logs/vgprobe.sh): a program defining its own `operator new` counts 1 call when run directly and 0 under valgrind. Memcheck redirects `operator new`/`delete` globally, so a suite that owns the allocator is judged against valgrind's allocator: its counters stay at 0, its injected failures never fire, its delete/free pairing no longer matches.
- No source fix exists (the tests are right; the tool replaces the function under test). Excluding those three files from a valgrind gate would be a forbidden exclusion, so valgrind is not wired. Memory checking stays with ASan + LSan + UBSan, which keep the program's own allocator and pass on all 49 suites.

## semgrep not measured (2026-11-23)

`docker compose run -T --rm --entrypoint semgrep lint --config p/cpp --metrics=off --quiet sysmodule/source overlay/source` exits rc=7 with no finding printed (registry rules need network from the container; build-logs/semgrep.log). Needs vendored rules or network access; not wired.

## Wiki and repo records blocked by regress-guard on the owner's pre-commit hook edit (2026-11-23)

- Two identical refusals when recording the -fanalyzer measurement in the wiki: `BLOCKED by regress-guard: error swallowed by added line` (the `style_staged=` assignment), in the owner's uncommitted edit of the pre-commit hook (visible in the working-tree diff of that file), not in my change; an earlier record with the same edit passed.
- Staged and ready: wiki (5 pages, -fanalyzer measurement) and repo (this file). Unblock: the owner records the hook edit, or records these two with the trailer `Regress-Accept: <hook path>:style_staged`.

## CodeQL: 3 alerts left on sysmodule/source (2026-11-23)

Measured with `scripts/codeql.sh` (database of `make -C tests COVERAGE=1 test` + the cross build, `.github/codeql-config.yml`, filtered by paths-ignore): 4 alerts at the start, 3 after fixing `cpp/useless-expression` at `sysmodule/source/network/dns_wrap.cpp:552` (setsockopt result is now checked; a failure closes the socket and returns EAI_FAIL; test `transport_failures_are_retryable` covers it, 49/49 suites pass, build-logs/test-all.log). Left:
- `cpp/unused-local-variable` `sysmodule/source/ldn/interfaces/iclient_process_monitor.hpp:35` and `sysmodule/source/config/config_ipc_service.hpp:456`: the variable `args` is declared inside the vendored `AMS_SF_DEFINE_INTERFACE` expansion (Atmosphere-libs libvapours `util_optional.hpp:141,248` and `util_typed_storage.hpp:58` raise the same alert, excluded as third party). The macro is the only way to declare an IPC interface; no source-level fix without editing the submodule.
- `cpp/unused-static-function` `sysmodule/source/config/config.cpp:152` `parse_uint32`: the function has a caller (`process_debug_key`, :274) in both builds. Proven with QL on the database: two `parse_uint32` entities exist (one per compilation, #53 aarch64 and #61 host), one has 1 call, the other 0, and `process_debug_key` exists once with 2 calls. Looks like the extractor linking the call of the second compilation to the first entity; not proven. No code change removes it without removing a used function.
- Suite: the config uses `security-and-quality`, a superset of `security-extended`; GitHub's default setup uses the same file, so the local result is the one CI will see. The bundle is not in the repo: set `CODEQL_HOME` to an extracted `codeql-bundle-linux64` (no checksum asset exists for it, so the download is unverified).
- Not added to hooks-proposal/pre-push-heavy-gates.sh: the gate exits 1 until these 3 are resolved.
