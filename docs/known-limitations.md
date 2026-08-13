# Known limitations

What the model does not do, and what the test suite does not check, as of 2026-08-07.
Sites cite a file and a symbol rather than a line number, so a refactor does not silently
invalidate a row.

## Model correctness

| Limitation | When it bites | Site |
|---|---|---|
| `remap_downstream_id` collapses every id to `3'h7` at `max_unique_ids == 1`, so a slave that violates B ordering can stamp a collective mask onto an unrelated B and inject a false CollectB into the join | only with a B-ordering-violating slave, which no shipped stimulus produces | `nsu/meta_buffer.hpp`, `remap_downstream_id` |
| Unicast AWs admitted through `Rob::push_aw` get no `burst_footprint_ok` check; only the direct `Packetize::push_aw` path asserts it | an oversized unicast burst reaches the fabric unchecked, where the collective path checks every replica | `nmu/rob.hpp`, `nmu/packetize.hpp` |
| The `ordering_req = 0` same-`(dst, id)` bypass streak holds order only while REQ/RSP stay single-VC and AR/B stay off DAT | opening a second REQ/RSP VC, or steering AR/B to DAT, loses in-fabric ordering. AR pinning and the B fixed hash were deleted in S3b. `ChannelModel` is VC-blind, so ctest cannot see it | `nmu/rob.hpp`, admission tree |
| The router's VA divergence assert sits behind the credit gate | a zero-credit diverging `fixed_vc = 0` worm idles silently instead of tripping the checker, a liveness gap rather than a wrong answer | `router/router.hpp`, VA stage |
| `SamTable::translate` asserts on a lookup miss, so a release build null-derefs instead of throwing | only under `NDEBUG`; every shipped test build keeps asserts on | `nmu/addr_trans.hpp`, `SamTable::translate` |
| `docs/noc-target-spec.md` derives DAT deadlock freedom from read data landing in reorder-buffer space reserved at request issue, and a bypassed read reserves no slot, so the argument does not cover it. This predates the 3 b id round; what that round changed is the count. Removing the shared outstanding pool takes the `RobMode::Enabled` read bound from 32 to 256, worst case all 256 unreserved -- 8 ids each running a 32-deep same-destination bypass streak allocates no RoB slot at all. `RobMode::Disabled`, the shipped default, moves the other way: `min(pool 32, per-id 256)` = 32 becomes 1 x 8 = 8 through the per-id single-outstanding interlock | only in a `RobMode::Enabled` build, and nothing constructs one: no topology YAML or generated fabric names it, `gen_tb_top.py` selects it from a topology name ending `_rob`, and the only user is the manual `sim-injection-sweep` target. Measuring it needs a consumer that backpressures R, which this testbench has not got: pulp's `axi_file_master` never stalls its R channel -- `wait_r` is a resident forked task that consumes a beat whenever `r_outst` is non-empty (`sim/dv/axi-0.39.7/src/axi_test.sv:2577-2586`) -- so the NMU always sinks R and the dependency cycle cannot form. A clean run on it shows occupancy and congestion and says nothing about deadlock freedom | `nmu/rob.hpp`, the `ordering_req = 0` branches of `push_ar`; `sim/tools/gen_tb_top.py`, `rob_enabled` |
| A peripheral cannot issue a collective. The fork spreads along the issuer's row and the join collects in the issuer's column, and routers exist only at tile coordinates, so an x-border issuer's `CollectB` never completes and a y-border issuer's replicas are never forked | any collective anchored anywhere but issued by a peripheral. Refused at `collective_translate`; a peripheral remains a legal collective *anchor*, which is the memory-controller-to-every-tile case | `nmu/addr_trans.hpp`, `collective_translate`; `router/route_mask.hpp` |
| `address_map.decode: offset` is validated, not implemented: the loader checks the map would be legal under one global coordinate range pair, then every lookup still range-matches the SAM | never at the AXI boundary, since on a map meeting spec 5.1 both modes reach the same node at the same node-local offset. It bites at RTL handover, where the two are different hardware: 2N range compares against one bit slice | `nmu/sam_yaml.hpp`, `check_decode_mode` |

## Performance cost

Full-mesh mask enumeration costs 256 x O(SAM entries) linear scans per `push_aw` attempt on a
16x16 mesh, and repeats the whole enumeration on every backpressure retry. Design K3 accepted
up to 256 lookups, not 256 x O(entries).

## Coverage gaps

| Untested | Why it is not reachable today |
|---|---|
| Over-delivery of a collective replica to a non-member node | neither scoreboard can see it: both compare only bytes the SOURCE reads -- `mcast_mem` walks the source's own readback set and the pulp scoreboard sits on `master_dv` -- so a replica landing where nothing reads is silent. The direct evidence is the peripheral link's flit count in `perf.json` and the armed tie-off `$fatal` on the unpopulated boundary ports, not the checkers (`sim/tb/user_node_endpoint.sv`, `sim/tools/gen_tb_top.py`) |
| Multi-hot to multi-hot fork completion across a link, and fork/join at `output_fifo_depth > 0` | no stimulus generates either shape |
| The held-join wait-for edge | probabilistic co-sim coverage only, never targeted |
| Narrow-class collectives under a deliberate fault | no narrow-class red run exists |
| Independent per-node master-face stall patterns | `axi_delayer_intf` does not expose `stream_delay`'s `Seed` and `axi_delayer` does not forward one, so every endpoint's LFSR starts from the same state. It advances per handshake, so instances decorrelate once their traffic differs, but symmetric traffic keeps them aligned: `neighbor` on a square mesh reports an identical stall count on every node, `multicast` does not. Reaching independent patterns means bypassing the component and wiring `stream_delay` directly (`sim/tb/user_node_endpoint.sv`, `i_mst_backpressure`) |
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
| NSU `max_unique_ids` takes only 1 or 8 | no intermediate ID-compression tier. Selectable N per-id FIFOs would close it |
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
| GCC threw non-deterministic internal compiler errors, on both hosts | **Cause found 2026-08-11, both sites fixed.** The root `Makefile` ran `cmake --build -j` with no limit, starting one compiler per core -- 28 against 15 GB on the WSL host -- so template-heavy gtest units drove it to swap; it now runs `-j $(JOBS)`, default 6. Separately `sim/verilator/Makefile` forced `--output-split 0`, emitting unsplit generated C++ in which one function reached 12648 lines; g++ 15 died on it during its own `no-opt dfinit` RTL pass. The flag is gone, Verilator's default splits the design into 247 files, and four consecutive clean builds produced no ICE. The failure rate was roughly one build in ten, so four is suggestive rather than conclusive |
| The simulator segfaulted about once in ten runs inside `axi_file_master::load_files`, and is not reproducible as of 2026-08-12 | The 2026-08-11 note called it "same binary, same arguments". It was not: `STIM_ROOT` carries no seed, `run-directed` regenerates the stimulus into that same directory on every run with `--seed $(SEED)`, and an unset `SEED` is drawn randomly (`sim/Makefile`, `_SEED`). The ten runs therefore had ten different stimulus sets, so the input was the varying quantity and not the environment. 48 runs with explicit seeds on `mesh_4x4_vc1` `neighbor`, 40 at `INJECTION_COUNT` 64 and 8 at 200, all reached 16 of 16 `[HWM]` lines and rc 0. At a one-in-ten rate that outcome has probability 0.006, so whatever it was does not hold for this configuration now. **On the next occurrence: take the seed off the `>>> sim` line, which every run prints, and re-run with `SEED=` set to it.** Reproducing means the stimulus content is the cause and `parse_write` is where to look; not reproducing rules the stimulus out and leaves the host. The first place to look inside `parse_write` is its W-beat loop, which is bounded by `current_aw.ax_len` even on the branch where the AW parse failed and that field was never assigned | `sim/verilator/Makefile`, `run-directed`; `sim/dv/axi-0.39.7/src/axi_test.sv`, `parse_write` |
| ctest discovery lists go stale in an incremental build tree and inflate the reported count | 665 tests registered against 705 present in the binaries after successive builds over each other. A pass count from a tree that was not wiped is not evidence |
| The AWUSER collective field layout is spelled with raw bit numbers in three places with no shared constant | `axi/types.hpp`, `sim/tools/gen_test_patterns.py`, `sim/tb/user_node_endpoint.sv`. The `static_assert` in `types.hpp` pins the width sum but not the offsets, so an offset change desynchronises all three silently. The SV copy also hardcodes `2'd1` where `ni_flit_pkg::COLLECTIVE_OP_MULTICAST` exists |
| `axi_bw_monitor.sv` carries a two-line local edit | upstream it or wrap it |
| The specgen pytest suite writes into the tree | `test_codegen.py` and `test_codegen_sv.py` each regenerate into `specgen/generated/` rather than a temp dir, so `make pytest` leaves six tracked files dirty with nothing but new timestamps. One such fixture is gone (`TestCheckModeWithSv.setup_method`, which also made its own tests tautological: regenerate, then assert a regen matches). Each file still dirties three. **This is the one thing standing between the repo and "build + sim + clean returns to what git tracks"** |
| Two specgen golden tests fail on column alignment | `emitted cpp output differs from spec-derived golden`, `WIDTH     = 8;` against `WIDTH           = 8;`. Adding `AXI_INITIATOR_ID_WIDTH` in Stage 2c lengthened the longest constant name, so the emitter's alignment column moved and the checked-in golden was never regenerated. Cosmetic in output, but it holds `make pytest` red |
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
been re-synced. The unbounded `-j` that used to take the host down mid-build is fixed; see the
compiler-error row above.

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

`axi_test::axi_rand_slave` in `MAPPED` mode aborts with a null-pointer dereference at
`axi_test.sv:1489` as soon as any of its wait bounds is non-zero: `recv_ws` reads `aw_queue[0]`
one line after `wait (aw_queue.size() > 0)`, and `send_bs` can `pop_front` the same entry in
between. The tile memory no longer uses it -- `axi_delayer` in front of `axi_sim_mem` replaced it
-- but `axi_test.sv` stays vendored for `axi_file_master` and `axi_scoreboard`, so the next
`MAPPED` user meets it again.

| | |
|---|---|
| trigger | `RESP_MAX_WAIT` or `R_MAX_WAIT` > 0. `AX_MAX_WAIT = 100` alone passes |
| not the trigger | write concurrency -- `MAX_OUTSTANDING=1` aborts identically |
| not the cause | Verilator `wait (q.size() > 0)` blocks correctly in a standalone 5.048 probe |
| why upstream never sees it | `MAPPED` appears nowhere in FlooNoC's `hw/` or pulp's `axi/test/`, and the whole `aw_queue` read is inside `if (MAPPED)` |

The last row is inference from reading, not measurement: a `$display` inserted at the read never
reached the built binary, so its silence proved nothing, and the fix landed by retiring the model
rather than by confirming the race directly.
