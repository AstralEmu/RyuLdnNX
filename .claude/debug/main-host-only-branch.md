# main.cpp: host-only branch did not compile

Symptom: unity-including `sysmodule/source/main.cpp` in a host test fails with
`'g_heap_init_mutex' was not declared in this scope` at the
`os::InitializeSdkMutex(...)` call in `init::InitializeSystemModule`.

Proof: the mutex is declared in `ams::mitm` (main.cpp, `constinit os::SdkMutex g_heap_init_mutex;`),
the call is in `ams::init`; g++ names the right scope in its hint
(`did you mean 'ams::mitm::g_heap_init_mutex'`). The call sits in `#ifndef __SWITCH__`, so the
console toolchain never compiles it.

Fix: qualify it (`mitm::g_heap_init_mutex`). Validated: `make run_main_tests` builds and the
12 tests of `tests/main_tests.cpp` pass; the console build is unaffected (branch preprocessed away).
Not verified: a devkitA64 build (not run for this one-token change in a preprocessed-out branch).
