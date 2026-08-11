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
| `address_map.decode: offset` is validated, not implemented: the loader checks the map would be legal under one global coordinate range pair, then every lookup still range-matches the SAM | never at the AXI boundary, since on a map meeting spec 5.1 both modes reach the same node at the same node-local offset. It bites at RTL handover, where the two are different hardware: 2N range compares against one bit slice | `nmu/sam_yaml.hpp`, `check_decode_mode` |

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
| Mixed-space sustained load on a `TILE_TARGETS = 2` tile | `NSU_META_BUFFER_MAX_UNIQUE_IDS = 1` collapses every tile transaction onto one AXI ID, and taxi's `thread_match_dest` blocks a same-ID AX aimed at a different master, so config-to-memory alternation serializes the tile port. Every shipped topology is `TILE_TARGETS = 2`, so reaching it needs only `INJECTION_MODE = 2` |
| AXI-side perf DPI hooks | never driven, so the `axi_bw_monitor` against `perf.json` cross-check has never run |
| Functional coverage, constrained-random stimulus, wire-level SVA | none exist |

Two co-sim checks are wider or narrower than intended: the merged-B checks run for every
pattern and mode rather than multicast alone, and `multicast` with `INJECTION_MODE != 0` is
guarded only at the root Makefile.

The cross-node config probe window carves one `0x40` slot per node from `0x800` up
(`_CONFIG_PROBE_BASE`, `_SLOT_STRIDE` in `sim/tools/gen_test_patterns.py`), so a 4 KB config
tile holds at most 32 nodes and the guard exits above that. This is a property of the stimulus,
not of the architecture. At the spec's 16x16 maximum the fix is a smaller stride or slot reuse,
not necessarily a larger config tile.

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
| `make test` restamps the provenance header in the tracked `specgen/generated/*` files it regenerates, six of them on an observed run | `codegen.py --check` strips both provenance lines (`_strip_provenance`), so it passes in either state and the drift gate cannot see the change. Symptom is six permanently dirty tracked files after any build |
| The regenerated `Source SHA` sometimes moves while git reports the input unchanged | observed on `ni_flit_constants.h` and `ni_flit_pkg.sv`, `13cc288fc4ad` to `123021553f78`, with `specgen/generated/json/ni_packet.json` clean in `git status`. The hash sees something git does not, most likely line-ending normalisation. Combined with the row above this means the drift gate can pass across a real input difference, so a clean `--check` is weaker evidence than it reads |
| The mingw64 GCC throws non-deterministic internal compiler errors under parallel builds | `try_forward_edges at cfgcleanup.cc:580` and segfaults, on a different source file each run. Hit on four consecutive tasks. Retry cleared it three times, a serial `-j 1` build was needed once |
| ctest discovery lists go stale in an incremental build tree and inflate the reported count | 665 tests registered against 705 present in the binaries after successive builds over each other. A pass count from a tree that was not wiped is not evidence |
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

Two hazards come with that mirror. The CMake cache under `$HOME/noc_build` records the mirror as
its source directory, so a `make` invoked from `/mnt/e` fails on the source-directory mismatch
rather than building the working tree, and a `cmake --build` aimed straight at the build
directory silently compiles the mirror instead. A pass count is evidence only once the mirror has
been re-synced. Separately, `cmake --build ... -j` in the root `Makefile` carries no job limit,
which on a 28-thread host has taken WSL down mid-build; `-j 6` completed the same build.

`SamTable::packed()` derives each base by accumulating sizes in list order, so an unmapped index
cannot be expressed. A mesh dimension that is not a power of two therefore cannot carry the
surplus padding `docs/noc-target-spec.md` §5.1 requires, and no such space can be a collective
target. Every shipped topology is 2x2 or 4x4, so nothing is affected today.

A tile interconnect decodes addresses and nothing else, so it cannot tell a collective write from
a unicast. A collective anchored at the issuing node's own region therefore looks local, and the
`user_node_endpoint` crossbar answers it instead of handing it to the NI. The endpoint works
around this by offsetting such a write into a NoC egress aperture derived from the address map
(`address_map.noc_egress_base`) and taking the offset back off at the NI port, which keeps the
crossbar stock pulp `axi_xbar`.

The structural answer is a decode that reads the transaction's sideband -- the collective op
already travels in AWUSER -- and selects the NI port directly, leaving `AWADDR` untouched end to
end. pulp `axi_xbar` exposes no such hook: its rules are address ranges (`addr_decode_dync.sv`)
and its per-slave-port default and `Connectivity` are both static. Reaching the select would mean
assembling a crossbar out of `axi_demux` and `axi_mux` and owning it. Not worth it for testbench
scaffolding; it is worth it for a real tile.
