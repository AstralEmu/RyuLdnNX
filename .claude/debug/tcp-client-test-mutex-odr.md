# Heap corruption in the host build: Mutex defined twice

## Symptom
`the_communication_service_is_created_for_the_client_program` (tests/ldn_mitm_service_tests.cpp) aborted with a glibc malloc assertion.

## Reproduction
Direct g++ build of the suite with `-fsanitize=address`, same sources as the Makefile.

## Proof
AddressSanitizer: heap-buffer-overflow while constructing the `ams::os::Mutex` member inside `TcpClient::TcpClient()`.

## Cause
`network/tcp_client.hpp` carried `#ifdef TEST_BUILD` with its own `class Mutex`, while other translation units of the same binary got `Mutex` from `tests/stubs/stratosphere.hpp`. Two definitions of one name with different `sizeof` (ODR violation): the object was allocated with one layout and constructed with the other.

## Fix
Shim removed; the header always includes `<stratosphere.hpp>`. The Switch build never compiled the `TEST_BUILD` branch.

## Validation
Suite 9/9 under ASAN, then the full `COVERAGE=1 docker compose run --rm test` run exits 0 with no failing test (tcp_client, client and the other suites including this header included).
