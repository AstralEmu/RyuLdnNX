# Flaky test: the_virtual_address_and_mask_are_never_torn_by_concurrent_proxy_configs

## Symptom
Pre-commit `COVERAGE=1` run: `ICommunicationService Tests Results: 126/127`.
Single failure, 1 in 25 runs on an idle machine, 0 in 80 runs with the
tidy job idle.

## Reproduction (proven)
12 busy-loop processes (2 x nproc) in the background while looping
`./run_ldn_icommunication_real_tests`: 8 failures in 24 runs, always the
same output:

    [TEST] the_virtual_address_and_mask_are_never_torn_by_concurrent_proxy_configs... FAIL
        Assertion failed: seen > 0
        at ldn_icommunication_real_tests.cpp:2163

The `ASSERT_FALSE(torn)` assertion (line 2162) never failed.

## Cause (proven by the assertion line)
The poll loop runs `while (!sender_done)`. The sender pushes its 500
packets into the mock socket and flags done immediately; the receive
thread of the service applies them asynchronously. Under CPU load the
poll loop ends before the receive thread applied a single ProxyConfig, so
no (ip, mask) pair was ever observed: `seen == 0`. No tear involved
(TSan is clean on the same test). It is a test-design race, not a product bug.

## Fix
Keep polling until the sender is done AND at least one pair was observed,
bounded by a deadline. The torn-pair check and `seen > 0` stay.
