# Validation record

## Routing and generator

The shared generator tests pass (27 tests), including the four coordinate/port pairs, four-destination ordering requests in control/data/rand modes, request/readback pairing and delay enable only on the two ordering cases. Original standalone recipes remain covered by the same suite.

The four-destination VCS baseline ctrl_write_single passes. All four destinations complete four writes and four reads in each directed ordering case. Initial VCS compilation exposed a declaration-before-use requirement in the older compiler; destination counter declarations were moved before the generate block.

## Minimum-delay method

The first experiment used one upstream axi_delayer_intf. Its zero-cycle and one-cycle configurations measured 0 and 1 cycles. Both control ordering cases failed only the required-disorder coverage at those settings. Random-mode cases already passed at zero because the burst mix creates unequal completion times.

For settings above one, upstream stream_delay adds counter states. The final TB instead chains unchanged one-cycle cells so RSP_DELAY=2 means two measured cycles rather than the original primitive's larger wait. Zero and one are functionally equivalent to the initial experiment. Final positive tests use the chain implementation, and logs distinguish the two experiments.

A common minimum means the smallest shared setting passing both B and R disorder coverage for both case names and all three modes. It is not a universal minimum for every seed, burst mix or topology. All cycle counts are on the common 10 ns clock. The WEST destination is delayed; the other three bypass. Request channels have zero added delay.

## Checker coverage

Ingress response completion order is matched against accepted requests using remapped ID, destination, ordering flag and tag. Same-ID coverage requires actual ingress disorder and buffered retirement on both B and R. Independent B retirement checking follows issue-order tags and rejects retirement before arrival. The existing scoreboard checks read payload, RLAST, AXI response status and per-ID read order. Every case must drain expected transactions and exercise all four endpoints when ordering coverage is enabled.

No production RTL or C++ source change. NSU REQ ingress capacity and in-flight reset remain outside this acceptance scope.


## Payload discrimination review

The original address-in-data bytes repeat modulo 256; 512-byte transaction spacing can therefore give distinct data requests identical payloads. The ordering cases now use the existing seeded random data generation for control/data writes as well. Generator tests verify distinct active-byte signatures for every transaction in each shipped ordering mode. Only six write.txt files changed (auto/control/data variants of the two case names). All other pattern files, including rand-mode ordering, are byte-identical. The 602 Python tests pass after this change. Affected control/data ordering runs and lower-delay negative probes are repeated with the strengthened payloads.
