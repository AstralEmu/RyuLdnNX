# clang-analyzer-unix.BlockInCriticalSection on the stub Mutex

Symptom: lint image (LLVM 21.1.8) reports the check on p2p_proxy_client.cpp:620 and p2p_proxy_server.cpp:2239 (recv() in a loop after a scoped lock); host clang-tidy 23.1.5 does not.

Tests (probes p_O, p_Q in the scratchpad):
- Lock class with mutex + recursive_mutex and an if on the recursive branch in lock()/unlock(): alert.
- std::scoped_lock vs ryu::ScopedLock: same alert (hypothesis "scoped_lock not modelled" refuted).
- One std::recursive_mutex, lock()/unlock() without branch around the call: 0 alert.

Cause (proved): the branch around the mutex call makes the analyzer lose the lock/unlock pairing, so the lock looks held after unlock().
Fix: tests/stubs/stratosphere.hpp Mutex = one recursive_mutex plus a depth counter; relock of a non-recursive Mutex aborts.
Validation: docker compose run --rm tidy-host -> 0 findings on the stubs; docker compose run --rm test passes.
