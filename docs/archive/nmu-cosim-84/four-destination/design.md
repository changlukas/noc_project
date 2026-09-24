# Four-destination co-simulation

The approved topology uses one Router at (1,1), an RTL NMU on LOCAL, and four C++ NSU/memory endpoints at NORTH (1,2), EAST (2,1), SOUTH (1,0), WEST (0,1). Endpoint port ID is zero. The generator requires a power-of-two X dimension, so routing bounds are 4 by 4; only the central Router is instantiated. SAM stores coordinates, not Router-port indices. Return headers address the NMU at (1,1), endpoint port zero.

Address windows retain the existing WEST base. NORTH, EAST and SOUTH use successive 4 GiB windows. Each has memory at window base and config at base + 0x02000000. Existing capacities and clocks remain unchanged.

Each memory uses the existing axi_delayer_intf with request delay zero and random stalls disabled. Only WEST responses are delayed in ordering cases; all delay instances are bypassed otherwise. The bypass selection is fixed before reset release. The delay cell backpressures the source while preserving payload, and delays each B/R beat rather than a whole burst as one unit. The upstream FixedDelay values above one add counter-state cycles. A chain of unchanged one-cycle cells makes every integer delay representable. The chain is testbench-only; observed wait time is measured separately.

Two existing case names are reused: cross_id_out_of_order and same_id_cross_dst_reorder. MODE selects control, data or rand without duplicating case names. Each case visits all four destinations. Ingress completion order is checked against accepted NMU requests including ID, destination and ordering tag. Same-ID tests also require buffered retirement. Ordering writes use seeded, distinguishable payloads because address-in-data repeats every 256 bytes. The existing AXI memory scoreboard remains responsible for payload and read ordering; an independent B retirement check validates the response tag sequence.

No production datapath is added. Delay cells, memories and checker queues are testbench-only. Bypass preserves ordinary-case latency; fixed-delay experiments intentionally reduce response throughput. The C++ model and DPI library are reused.
