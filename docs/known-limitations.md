# Known limitations

What the model does not do, and what the test suite does not check, as of 2026-08-07.
Sites cite a file and a symbol rather than a line number, so a refactor does not silently
invalidate a row.

## Model correctness

| Limitation | When it bites | Site |
|---|---|---|
| `remap_downstream_id` collapses every id to `8'hFF` at `max_unique_ids == 1`, so a slave that violates B ordering can stamp a collective mask onto an unrelated B and inject a false CollectB into the join | only with a B-ordering-violating slave, which no shipped stimulus produces | `nsu/meta_buffer.hpp`, `remap_downstream_id` |
| Unicast AWs admitted through `Rob::push_aw` get no `burst_footprint_ok` check; only the direct `Packetize::push_aw` path asserts it | an oversized unicast burst reaches the fabric unchecked, where the collective path checks every replica | `nmu/rob.hpp`, `nmu/packetize.hpp` |
| The `ordering_req = 0` same-`(dst, id)` bypass streak holds order only while REQ/RSP stay single-VC and AR/B stay off DAT | opening a second REQ/RSP VC, or steering AR/B to DAT, loses in-fabric ordering. AR pinning and the B fixed hash were deleted in S3b. `ChannelModel` is VC-blind, so ctest cannot see it | `nmu/rob.hpp`, admission tree |
| The router's VA divergence assert sits behind the credit gate | a zero-credit diverging `fixed_vc = 0` worm idles silently instead of tripping the checker, a liveness gap rather than a wrong answer | `router/router.hpp`, VA stage |
| `SamTable::translate` asserts on a lookup miss, so a release build null-derefs instead of throwing | only under `NDEBUG`; every shipped test build keeps asserts on | `nmu/addr_trans.hpp`, `SamTable::translate` |

## Performance cost

Full-mesh mask enumeration costs 256 x O(SAM entries) linear scans per `push_aw` attempt on a
16x16 mesh, and repeats the whole enumeration on every backpressure retry. Design K3 accepted
up to 256 lookups, not 256 x O(entries).

## Coverage gaps

| Untested | Why it is not reachable today |
|---|---|
| Multi-hot to multi-hot fork completion across a link, and fork/join at `output_fifo_depth > 0` | no stimulus generates either shape |
| The held-join wait-for edge | probabilistic co-sim coverage only, never targeted |
| Narrow-class collectives under a deliberate fault | no narrow-class red run exists |
| Mixed-space sustained load on a `TILE_TARGETS = 2` tile | `NSU_META_BUFFER_MAX_UNIQUE_IDS = 1` collapses every tile transaction onto one AXI ID, and taxi's `thread_match_dest` blocks a same-ID AX aimed at a different master, so config-to-memory alternation serializes the tile port. `docs/noc-performance-parameters.md` states the mechanism. Reaching it needs `INJECTION_MODE = 2` on a config topology |
| AXI-side perf DPI hooks | never driven, so the `axi_bw_monitor` against `perf.json` cross-check has never run |
| Functional coverage, constrained-random stimulus, wire-level SVA | none exist |

Three co-sim checks are wider or narrower than intended: the merged-B checks run for every
pattern and mode rather than multicast alone, the probe-window guard keys on `base_local`
instead of the config tile size, and `multicast` with `INJECTION_MODE != 0` is guarded only at
the root Makefile.

## Missing parameterization

| Gap | Consequence |
|---|---|
| NSU `max_unique_ids` takes only 1 or 256 | no intermediate ID-compression tier. Selectable N per-id FIFOs would close it |
| Per-tile compute rate appears in no spec or perf doc | every utilization figure and the minimum viable tile size depend on a number the model does not carry (`docs/noc-workload-benchmark.md` section 9) |

## Input validation

`gen_test_patterns.py` validates neither AxLEN nor the AXI 4 KB boundary rule, so an illegal
`BURST_LEN` surfaces as the RoB's oversized-burst abort rather than a stimulus error.
`sam_yaml` reports a missing `address_map` without naming it, `gen_tb_top` accepts an empty
`requested_name`, and some test helpers still take `uint32_t` strb parameters, which cannot
express a bus of 32 lanes or more. Outside `beat_exact`, the co-sim default beat is half-bus
(`--size 5` on a 64 B bus).

## Build and tooling

| Item | Risk |
|---|---|
| `src/c_model/tests/integration/CMakeLists.txt` mirrors `sim/topologies/` with `cmake -E copy_directory`, which never prunes | its copy still carries `mesh_1x1_vc1.yaml` and `mesh_2x2_nonuniform_vc1.yaml`, both deleted in `6cb12b3`. Harmless while `test_narrow_class_smoke` names one file by hand, a trap the moment it enumerates. The `TOPOLOGY_DIR` compile definition already used by the nmu test fits here |
| The AWUSER collective field layout is spelled with raw bit numbers in three places with no shared constant | `axi/types.hpp`, `sim/tools/gen_test_patterns.py`, `sim/tb/user_node_endpoint.sv`. The `static_assert` in `types.hpp` pins the width sum but not the offsets, so an offset change desynchronises all three silently. The SV copy also hardcodes `2'd1` where `ni_flit_pkg::COLLECTIVE_OP_MULTICAST` exists |
| `axi_bw_monitor.sv` carries a two-line local edit | upstream it or wrap it |
| The specgen pytest suite writes into the tree | it rewrites committed banners instead of using a temp dir |
| specgen `examples/quickstart` printf column padding | misaligned since the S0 rename, cosmetic |

## Simulator flows

The VCS flow builds and has never been executed. Two things to check before the first run: the
generated fabric drives one packed vector from a port connection (bit 0) alongside an
`always_comb` (bits 1-4), which Verilator tolerates and VCS may reject; and deleting only
`noc_fabric_<topo>.sv` leaves the build failing on a missing file rather than regenerating it.

The WSL host is unstable under load. Working practice: rsync to `~/noc_project`, one foreground
session at a time, and an echo-marker retry around each invocation.
