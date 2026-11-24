# bsd_mitm_service.cpp: dead code removed, findings kept

## Removed (proven dead)
`EraseSocketInfoEntry` began with `if (entry == nullptr) return;`.
Callers (grep `EraseSocketInfoEntry` in bsd_mitm_service.cpp):
- the destructor passes `&entry` of a range-for reference: never null;
- `Close` passes `entry` inside `if (entry != nullptr)`.
Line 176 stayed at count 0 in gcov with 94 tests exercising both callers.
Verified after removal: 95/95 tests, 968/968 lines.

## Observed, not changed (behaviour of the shipped sysmodule)
- `Bind`: when `ProxySocket::Bind` fails the allocated ephemeral port or the
  reserved port is not released (`Connect` releases it). Reachable by binding
  the same fd twice.
- `Bind`, `GetSockOpt`, `SetSockOpt` on proxy sockets return the `Result`
  value as errno.
- `ReservePort` below 49152 is a no-op (pool only tracks the ephemeral range):
  two sockets can bind the same low port.
- Branches reachable only through a race (staged with the stand-in lock
  hooks): auto-bind refused in Connect/SendTo, abandoned table refilled
  between cleanup and retry, a proxy appearing during the real select/poll.
