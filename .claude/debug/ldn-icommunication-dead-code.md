# Dead code in ldn/ldn_icommunication.cpp

## Symptom
Host coverage of `ldn_icommunication.cpp` could not reach 100 % because two pieces of code have no reachable caller. Removed only after proof.

## 1. `case PacketId::Disconnect` in `HandleServerPacket`
Proof that the case is unreachable:
1. `HandleServerPacket` is only called through `OnServerPacket`, the packet callback of `RyuLdnClient` (`ldn_icommunication.cpp`, `set_packet_callback(OnServerPacket, this)`).
2. `RyuLdnClient::handle_packet` (`network/client.cpp`, switch on the packet id) consumes `Ping` and `Disconnect` itself and forwards only the `default:` case to the callback.
3. `RyuLdnClient::process_handshake_response` (same file) also consumes `Disconnect` (it turns it into `HandshakeFailed`) and forwards only its own `default:`.
4. Host test `a_server_disconnect_is_consumed_by_the_client_layer` sends a server `Disconnect` while hosting: the disconnect reason of the service stays `None`, so the case body (which would set `SystemRequest`) never runs.
`Ping` is NOT dead: during the handshake a `Ping` takes the `default:` path of `process_handshake_response` and reaches the service (covered by `pings_that_arrive_before_the_handshake_ends_reach_the_service`).

## 2. `ICommunicationService::SetGameVersion`
Proof: it is a private member (`ldn_icommunication.hpp`, private section starts at line 476), `grep -rn game_version sysmodule/source tests` shows no call anywhere; the only other reference to `m_game_version` is the zero-initialised copy into `RyuNetworkConfig::game_version` of `CreateNetwork`/`CreateNetworkPrivate`.
Behaviour is unchanged by the removal: `game_version` is and was always zero on the wire.
Open question for the owner (not changed): the original code intended to send a game version; nothing wires it today.

## Validation
Host suite `ldn-icommunication-real` passes after the removal; the sysmodule build is unaffected (no references).

# Stale `m_last_response_id` accepted as the answer to a new request

## Symptom
`WaitForResponse(Connected)` returned success at once for a second `Connect` after `Disconnect`, although the server never answered the second request.

## Reproduction (before the fix)
Test `connecting_again_after_a_disconnect_waits_for_the_new_answer` (join, `Disconnect`, silent server, `Connect`): `Expected: Connect(...) == RC_TIMEOUT` failed at `ldn_icommunication_real_tests.cpp:2295`, 112/113 passed, the other 112 green.

## Cause (code)
`WaitForResponse` entry shortcut `if (m_last_response_id == expected_id) return true;`: `m_last_response_id` is only written by the receive thread and never reset, so after a first `Connected` it stays `Connected` while `m_network_connected` goes back to false (Disconnect / CloseAccessPoint). The only expected id ever passed is `Connected`, whose "already arrived" race is already covered by the `m_network_connected` check at entry and by the re-check after the loop.

## Fix
Shortcut removed. Validation: same suite, 124/124 passed (8 consecutive runs), the reproducing test now passes; ASan build 3/3 clean.
