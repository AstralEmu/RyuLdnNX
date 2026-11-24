# P2P connect worker raced with the disconnect paths

## Symptoms (host suite `ldn-icommunication-real`)
- Intermittent SIGSEGV in the stand-in `CreateThread` (`join()` on garbage), reached from `P2pProxyClient::Connect` (`p2p_proxy_client.cpp:321`) on the `p2p_connect` thread (gdb backtrace).
- Intermittent hang (about 1 run in 4, twice in a row on two hunts, always while the test `a_host_that_never_answers_the_auth_...` was ending): main thread inside `~ICommunicationService` -> `DisconnectFromServer` -> `DisconnectP2pProxy` -> `P2pProxyClient::Disconnect`, once blocked on the client mutex (`p2p_proxy_client.cpp:348`), once blocked joining the recv thread.

## Cause (code order, observed state)
`~ICommunicationService` calls `DisconnectFromServer()` (cpp:350) BEFORE it joins the connect worker (cpp:367). `DisconnectFromServer` -> `DisconnectP2pProxy` (cpp:557) resets `m_p2p_client`, while the worker is still inside `HandleExternalProxyConnect` using the same pointer (Connect, PerformAuth, EnsureProxyReady and its own `DisconnectP2pProxy` on failure). Two threads then run `Disconnect()`/`reset()` on one client, or one frees it under the other. `Finalize`, `CloseAccessPoint`, the inactivity timer and `SendProxyDataToServer` reach the same pointer with no lock: the same race exists in the production build, not only in the tests.
Not proven: the exact interleaving of each hang/crash (no trace of both threads was taken). The fix removes the shared ownership, and the symptoms are gone (below).

## Fix
- `m_p2p_client_mutex` guards the pointer (and the IPC send through it).
- While the worker is in flight (`m_p2p_connect_thread_active`), `DisconnectP2pProxy` only calls `Disconnect()` (which aborts the worker's blocking steps); the worker frees the client with `ResetP2pClient()`.
- Otherwise the client is detached under the lock and destroyed outside it.
- No `shared_ptr`: the overridden `new` does not throw, a control-block allocation failure would be unchecked.
Residual (not changed): `Disconnect()` called before `Connect()` has published its socket cannot abort that `Connect()`; the worker then completes a connection that the owner frees at destruction.

## Validation
ASan build of the suite, 14 consecutive runs: 14/14 `104/104 passed`, no hang, no report. Before the change the same loop hung at run 4 (two independent loops).

# Overlapping strncpy in `RyuLdnClient::connect`

ASan (`-fsanitize=address`, first test that opens a station): `strncpy-param-overlap` at `network/client.cpp:420`. `connect()` calls `connect(m_config.host, m_config.port)`, so the source and the destination are the same buffer: undefined behaviour in C. Fixed by copying only when `host != m_config.host`.
