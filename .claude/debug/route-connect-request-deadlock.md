# RouteConnectRequest self-deadlock

## Symptom
New host suite `test-proxy-socket`, test `a_connect_request_reaches_the_matching_tcp_listener`, never returns
(6 runs of `./run_proxy_socket_tests` with `timeout 20`: it was the last line printed in 3 of them).

## Cause (proven by reading + reproduction)
- `ProxySocketManager::RouteConnectRequest` (proxy_socket_manager.cpp:327) takes `m_mutex`.
- While holding it, it calls `ProxySocket::IncomingConnection` (proxy_socket.cpp:365 call, :520 def).
- `IncomingConnection` calls `ProxySocketManager::SendProxyConnectReply` (proxy_socket.cpp:559+), which takes
  `m_mutex` again.
- `m_mutex` was `os::Mutex m_mutex{false}` (proxy_socket_manager.hpp:597): non-recursive, so the same thread
  blocks on itself. On the console this would hang the receive thread the first time a ProxyConnect request
  reaches a listening TCP proxy socket.

## Fix
`os::Mutex m_mutex{true}` (recursive). Releasing the lock before `IncomingConnection` was rejected: it would let
`CloseProxySocket` free the socket while it is being used.

## Status
Reproduced on the host (hang). Fix validated on the host by the same test. Not validated on a console.
