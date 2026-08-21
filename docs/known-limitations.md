# Known limitations

What the model does not do, and what the test suite does not check, as of 2026-08-19.
Sites cite a file and a symbol rather than a line number, so a refactor does not silently
invalidate a row.

## Model correctness

| Limitation | When it bites | Site |
|---|---|---|
| The production-pinned `cc_addr_decode` functional loop explicitly permits overlap and gives the highest array index priority, but its dynamic submodule's simulation-only `check_overlap` assumption and `more_than_1_bit_set` final assertion still report a legal multiple match when assertions are enabled | follow-on RTL/DV must provide a selective treatment for those two overlap diagnostics while keeping all other primitive and project assertions active; globally disabling assertions, changing the approved overlap policy, or replacing the decoder is not acceptable | production-pinned `cc_addr_decode.sv` / `cc_addr_decode_dync.sv`; `docs/verification-environment.md`, Provenance; `docs/nmu-verification-plan.md`, N0-SAM-06 |
| The approved generated-SAM contract permits overlapping ranges with authored-first priority, but current `SamTable::validate` rejects every overlap and `RejectsOverlap` locks in that obsolete behavior. The runtime loader also derives coordinate selectors from a representable layout without yet requiring an explicit range-level `en_collective: true` | target generator/model parity on a legal overlap, or a false/absent collective flag whose stride happens to expose coordinate bits; follow-on implementation must accept the overlap, preserve authored order, and zero selectors unless collective is explicit | `nmu/addr_trans.hpp`, `SamTable::validate`; `nmu/sam_yaml.hpp`, `load_config_table` / coordinate declarations; `tests/nmu/test_sam_table.cpp`, `RejectsOverlap`; `tests/nsu/test_nsu_depacketize.cpp`, `UndeclaredCoordsLeaveTheAddressAlone`; `docs/nmu-verification-plan.md`, N0-SAM-06 and N0-SAM-14 |
| The first RTL target does not guarantee multicast/collective operation on rectangular meshes; unicast remains supported when X and Y are independently selected from 2, 4, 8 and 16 | a configuration requests multicast/collective traffic with `mesh_x_dim != mesh_y_dim`; the current C++ route-mask tests cover some rectangular cases, but that is not production signoff evidence | `docs/noc-target-spec.md`, Section 5; `docs/router-spec.md`, Section 3.1; `docs/router-verification-plan.md`, Sections 5 and 6 |
| The approved RoB-less read policy is not implemented: the model still permits only one outstanding read per ID instead of a same-ordering-domain streak, and Enabled-mode bypass compares `{dst_id, class}` without `dst_port_id` | `READ_ROB_ENABLED=0`, or one ID addresses a tile and peripheral sharing a coordinate | `nmu/rob.hpp`, `Rob::push_aw` / `Rob::push_ar`; `specgen/source/constants.yaml`, `READ_ROB_ENABLED`; `docs/nmu-verification-plan.md`, Section 9 |
| `remap_downstream_id` collapses every id to `3'h7` at `max_unique_ids == 1`, so a slave that violates B ordering can stamp a collective mask onto an unrelated B and inject a false CollectB into the join | only with a B-ordering-violating slave, which no shipped stimulus produces | `nsu/meta_buffer.hpp`, `remap_downstream_id` |
| The C++ NSU Response Queue surrogate is `MetaBuffer`, keyed only by downstream AXI ID with collapse/pass-through modes. It does not implement the target source-aware `{src_id, src_port_id, noc_id}` dynamic mapping, identity-preferred/lowest-free allocation, or mapping reference counts | target S0 downstream-ID comparison and multi-source same-ID concurrency; an ID-only model cannot be the mapping oracle | `nsu/meta_buffer.hpp`; `nsu/depacketize.hpp`, `remap_downstream_id`; `docs/nsu-verification-plan.md`, Section 9 |
| Unicast AWs admitted through `Rob::push_aw` get no `burst_footprint_ok` check; only the direct `Packetize::push_aw` path asserts it | an oversized unicast burst reaches the fabric unchecked, where the collective path checks every replica | `nmu/rob.hpp`, `nmu/packetize.hpp` |
| The `ordering_req = 0` same-`(dst, id)` bypass streak holds order only while REQ/RSP stay single-VC and AR/B stay off DAT | opening a second REQ/RSP VC, or steering AR/B to DAT, loses in-fabric ordering. AR pinning and the B fixed hash were deleted in S3b. `ChannelModel` is VC-blind, so ctest cannot see it | `nmu/rob.hpp`, admission tree |
| The router's VA divergence assert sits behind the credit gate | a zero-credit diverging `fixed_vc = 0` worm idles silently instead of tripping the checker, a liveness gap rather than a wrong answer | `router/router.hpp`, VA stage |
| The C++ Router can pop successive flits from one input VC FIFO into different outputs in one tick because it evaluates outputs sequentially; target RTL is not required to provide a multi-read FIFO | an otherwise correct one-read implementation diverges in cycle placement while preserving per-output order and conservation | `router/router.hpp`, `Router::tick`; `docs/router-verification-plan.md`, R2-X02 |
| The C++ REQ/RSP `almost_full_offset` deassertion cycle is not a target oracle (the value itself is co-sim-calibrated: worst measured overrun 1 entry, shipped 2) | differential input-ready timing near almost-full occupancy | `router/simple_router.hpp`, `SimpleRouterConfig::almost_full_offset`; `docs/router-verification-plan.md`, R2-X06 |
| `SamTable::translate` asserts on a lookup miss, so a release build null-derefs instead of throwing | only under `NDEBUG`; every shipped test build keeps asserts on | `nmu/addr_trans.hpp`, `SamTable::translate` |
| The model is single-clock and does not implement the target five-channel AXI async-FIFO boundary (`AXI_FIFO_DEPTH=8`). Model-wrapper reset clears only the SV/output shell, not an already-running C++ core, so a hybrid cannot use the reference NI as a nonempty mid-run reset peer. Its DAT allocators own per-VC pending queues, while target NI has no VC FIFO; its Router-to-NI LOCAL DAT path returns per-VC credits, while target ejection uses ready/valid. The model implements only the `SHARED` candidate set; `READ_WRITE_SPLIT` masks are absent from NI allocation and router VA | cycle-exact CDC, nonempty hybrid reset, target LOCAL DAT backpressure, Router-only VC ownership, or asynchronous AXI/NoC verification | `ref_model/top/nmu_wrap.sv`; `ref_model/top/nsu_wrap.sv`; `wrap/dat_merge_wrap.hpp`, `DatMergeWrap::tick`; `router/router.hpp`, `Router::vc_assignment`; `nmu/nmu_standalone.hpp`; `nsu/nsu_standalone.hpp`; `nmu/vc_allocator.hpp`; `nsu/vc_allocator.hpp`; `docs/nmu-verification-plan.md`, Sections 9 and 11 |
| The NMU has independent REQ and DAT arbiter/allocation instances and tests that backpressure does not cross between them, but no test proves both faces emit in the same cycle from traffic captured through the one shared AXI interface | a later integration change could serialize channel assignment without failing the existing one-face-at-a-time tests | `nmu/nmu.hpp`, `Nmu::tick`; `nmu/test_nmu_dat_face.cpp`; `docs/nmu-verification-plan.md`, Sections 5.2 and 9 |
| The NSU implements the adopted NoC-to-AXI write merge with independent REQ/DAT ingress, round-robin AW class selection and strict W-order FIFO service, but no direct test presents both AW classes together or proves that a ready non-head W class cannot bypass a stalled head class | a change to AW arbitration or W gating could starve one class or misassociate W data without failing the existing sequential class tests | `nsu/depacketize.hpp`, `Depacketize::pop_aw` / `Depacketize::pop_w`; `nsu/test_nsu_depacketize.cpp` |
| `Depacketize::pop_aw` flips `aw_prefer_data_` before checking `MetaBuffer::write_full()`, so each rejected full-pool attempt rotates the next simultaneous-class AW winner even though no AW was accepted | target round-robin comparison under write-admission backpressure; the current model cannot be the accepted-grant scheduling oracle until the update is acceptance-qualified | `nsu/depacketize.hpp`, `Depacketize::pop_aw`; `docs/nsu-verification-plan.md`, Sections 4.3 and 9 |
| `docs/noc-target-spec.md` derives DAT deadlock freedom from read data landing in reorder-buffer space reserved at request issue, and a bypassed read reserves no slot, so the argument does not cover it. This predates the 3 b id round; what that round changed is the count. Removing the shared outstanding pool takes the `RobMode::Enabled` read bound from 32 to 256, worst case all 256 unreserved -- 8 ids each running a 32-deep same-destination bypass streak allocates no RoB slot at all. `RobMode::Disabled`, which a co-sim run now reaches only through `nmu.READ_ROB_ENABLED: 0` and a ctest by setting `cfg.read_rob_mode` itself, moves the other way: `min(pool 32, per-id 256)` = 32 becomes 1 x 8 = 8 through the per-id single-outstanding interlock | in every build: `RobMode::Enabled` is the default the `NmuConfig` default constructor, `NmuWrap::init` and `cmodel_nmu_create` all take, and `nmu.READ_ROB_ENABLED` defaults to 1. Condition one is now observed rather than argued. `gen_dma_jobs` emits read jobs sourced from another node's window, so reads cross the fabric: the DMA gate reports `read_txns_hwm=1` and `ar_clause={idle=2 same_dest=0 alloc=0}` on every node, against `read_txns_hwm=0` and every AR clause at zero before it. Both ARs took the `ordering_req = 0` idle bypass and `read_slot_hwm` stayed 0, so neither reserved a slot. The cycle cannot form at that run, and not merely did not: at `read_txns_hwm = 1` a node never has two unreserved reads outstanding at once. Condition two is unchanged. The iDMA endpoint has a mechanism to stall R, which pulp's `axi_file_master` has not -- `wait_r` is a resident forked task that consumes a beat whenever `r_outst` is non-empty (`sim/dv/axi-0.39.7/src/axi_test.sv:2577-2586`) -- but the run does not show it stalling a fabric-sourced read. The endpoint's `[mst_bp]` counter sits on the DMA's own master port, which carries local reads too, and the gate runs `gen_tb_top._MST_BACKPRESSURE = "random"`, so what it counts is the delayer knob rather than write-path congestion (`sim/tb/soc/dma_node_endpoint.sv`, `i_mst_backpressure` / `mst_r_stall_cycles`) | `nmu/rob.hpp`, the `ordering_req = 0` branches of `push_ar`; `sim/tools/gen_tb_top.py`, `READ_ROB_ENABLED` |

## Performance cost

Full-mesh mask enumeration costs 256 x O(SAM entries) linear scans per `push_aw` attempt on a
16x16 mesh, and repeats the whole enumeration on every backpressure retry. Design K3 accepted
up to 256 lookups, not 256 x O(entries).

Offset decode is deferred. The first RTL milestone range-matches the SAM table for every request
and rejects `routing.use_id_table: false`. Add a fixed address-bit slice only if table comparator
area or timing becomes a measured problem on a topology without peripheral endpoints.

`AW_SAM_REG_TYPE` and `AR_SAM_REG_TYPE` are approved RTL timing parameters, but the current C++
model represents only their default value 0. Modes 1 and 2 need cycle-model support before they
can enter cycle-exact co-simulation.

## Coverage gaps

| Untested | Why it is not reachable today |
|---|---|
| Over-delivery of a collective replica to a non-member node | neither scoreboard can see it: both compare only bytes the SOURCE reads -- `mcast_mem` walks the source's own readback set and the pulp scoreboard sits on `master_dv` -- so a replica landing where nothing reads is silent. The direct evidence is the peripheral link's flit count in `perf.json` and the armed tie-off `$fatal` on the unpopulated boundary ports, not the checkers (`sim/tb/test/user_node_endpoint.sv`, `sim/tools/gen_tb_top.py`) |
| Multi-hot to multi-hot fork completion across a link, and fork/join at `output_fifo_depth > 0` | no stimulus generates either shape |
| The held-join wait-for edge | probabilistic co-sim coverage only, never targeted |
| Narrow-class collectives under a deliberate fault | no narrow-class red run exists |
| Independent per-node master-face stall patterns | `axi_delayer_intf` does not expose `stream_delay`'s `Seed` and `axi_delayer` does not forward one, so every endpoint's LFSR starts from the same state. It advances per handshake, so instances decorrelate once their traffic differs, but symmetric traffic keeps them aligned: `neighbor` on a square mesh reports an identical stall count on every node, `multicast` does not. Reaching independent patterns means bypassing the component and wiring `stream_delay` directly (`sim/tb/test/user_node_endpoint.sv`, `i_mst_backpressure`) |
| Mixed-space sustained load on a `TILE_TARGETS = 2` tile | `NSU_META_BUFFER_MAX_UNIQUE_IDS = 1` collapses every tile transaction onto one AXI ID, and taxi's `thread_match_dest` blocks a same-ID AX aimed at a different master, so config-to-memory alternation serializes the tile port. Every shipped topology is `TILE_TARGETS = 2`, but the shipped default is passthrough (`NSU_META_BUFFER_MAX_UNIQUE_IDS = 8`), so reaching it needs `MAX_UNIQUE_IDS = 1` plus `INJECTION_MODE = 2` |
| AXI-side perf DPI hooks | never driven, so the `axi_bw_monitor` against `perf.json` cross-check has never run |
| A write landing in a tile memory, observed independently of the response that reports it | `axi_sim_mem`'s write monitor reads 0 at every posedge under Verilator 5.048, so nothing can gate on it. Counted two ways in one instrumented DMA run -- through a port chain and read straight off `i_sim_mem` -- both 0 on a line where the job counters read `issued=4 retired=4`. It is driven from an `initial ... wait(rst_ni); forever` with `<= #(ApplDelay)` assigns, the same procedurally-assigned-output shape `idma_job_driver.sv` already registers its counts to work around; `axi_sim_mem_intf` does forward the signals by name, so the wiring is not the cause. The DMA top gates on job retirement instead: the destination's own B is what retires the job, and `axi_sim_mem` pushes B only after the last W beat has landed in `mem[]` (`sim/dv/axi-0.39.7/src/axi_sim_mem.sv`, the `mon_w_valid_o` output register) |
| A destination byte the DMA never wrote, at an address whose preload pattern is itself `0x00` | the DMA region compare reads an unwritten byte as 0 (no `mem[]` key) and compares it against the source's `a[7:0] ^ a[15:8] ^ a[23:16] ^ 8'ha5`, so it is caught wherever that pattern is non-zero -- 255 of every 256 addresses. At the remaining one the two are indistinguishable: about 4 bytes of a 1024 B job region. A whole unwritten region therefore still fails on ~1020 bytes; an isolated single-byte write loss landing on one of those 4 does not. Reaching it needs exactly that, so it is recorded rather than fixed -- a pattern that is never zero costs more than it buys (`sim/tools/gen_tb_top.py`, `mem_pattern` / `compare_region`) |
| More than one AXI ID from a DMA endpoint | an iDMA backend cannot issue them. `idma_axi_read.sv` never reads `r.id`: every R beat is masked into the buffer against the head of the read datapath queue (`buffer_in_o`, `r_dp_ready_o`), and the legalizer fills that queue in AR-issue order across job boundaries, so the backend requires read data in global AR order. AXI guarantees that per ID only. Measured, one variable, geometry and depths untouched: the id-per-job emitter that shipped before fabric reads existed fails `%Fatal: 8192 bytes differ` with the payloads of consecutive jobs exchanged byte for byte -- `mem_pattern(0x1800)` arriving at the read job's destination and `mem_pattern(0x101400)` at the write job's -- and `axi_id = 0` passes. One ID is upstream's own usage rather than a concession: FlooNoC's job format has no ID field, its job class constructor sets `id = '0`, and iDMA's multi-channel frontend (`idma_inst64_top`) gives a channel its own ID by replicating the whole backend onto its own top-level AXI port, enforcing the same order through port separation. The cost is on the NMU side and it is real: `nmu/rob.hpp` keeps `write_order_by_id_` and `read_order_by_id_` as arrays of `NOC_ID_SPACE = 8` and keys every admission decision on the id -- the idle bypass on `read_order_by_id_[b.id].empty()`, the same-destination bypass on `prev_dest_read_[b.id]` and the sticky `fallen_back_read_[b.id]`. A single-ID stream reaches one bucket of the eight, so seven are unreached and the shape the DAT deadlock argument is about -- one ID's fallback-allocate interleaved with another ID's bypass streak -- is unreachable by construction from a DMA endpoint. Downstream is where the ID does not matter: the default passthrough (`NSU_META_BUFFER_MAX_UNIQUE_IDS = 8`) forwards the DMA's single ID unchanged, and the collapse setting (1) folds it onto one ID -- a single-ID stream looks the same to the NSU either way. Reaching the NMU's other seven buckets needs a multi-ID master, not a stimulus change (`sim/tools/gen_dma_jobs.py`, `_AXI_ID`) |
| Bandwidth on a DMA run | `dma_node_endpoint` ties `end_of_sim_o` to 0 and `axi_bw_monitor` reports on that edge, so a DMA run emits no `[Monitor ...]` line |
| `--dma` on a topology carrying a peripheral | `gen_dma_jobs.job_table` walks the router array only, so a peripheral endpoint has no `jobs.txt` and its `idma_job_driver` `$fatal`s on the missing file. Refused at generate time: `gen_tb_top.main()` rejects `--dma` on a config that attaches an endpoint to a boundary port and cites this row |
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
| The current C++ `max_unique_ids` accepts only 1 or 8 and does not model the approved independent `NSU_MAX_ACTIVE_IDS` and `NSU_MAX_OUTSTANDING` capacities | the current collapse/pass-through model cannot stand in for the approved dynamic mapper or independently sized Response Queues |
| Per-tile compute rate appears in no spec or perf doc | every utilization figure and the minimum viable tile size depend on a number the model does not carry (`docs/noc-workload-benchmark.md` section 9) |

## Input validation

`gen_test_patterns.py` validates neither AxLEN nor the AXI 4 KB boundary rule, so an illegal
`BURST_LEN` surfaces as the RoB's oversized-burst abort rather than a stimulus error.
`sam_yaml` reports a missing `address_map` without naming it, `gen_tb_top` accepts an empty
`requested_name`, and some test helpers still take `uint32_t` strb parameters, which cannot
express a bus of 32 lanes or more. The co-sim default beat is half-bus (`--size 5` on a 64 B
bus), and since `beat_exact` was removed no pattern sets a write-strobe bit above lane 15, so
no run exercises a strobe crossing a DPI 32-bit word boundary.

## Build and tooling

| Item | Risk |
|---|---|
| `make test` restamps the provenance header in the tracked `specgen/generated/*` files it regenerates, six of them on an observed run | `codegen.py --check` strips both provenance lines (`_strip_provenance`), so it passes in either state and the drift gate cannot see the change. Symptom is six permanently dirty tracked files after any build |
| The regenerated `Source SHA` sometimes moves while git reports the input unchanged | observed on `ni_flit_constants.h` and `ni_flit_pkg.sv`, `13cc288fc4ad` to `123021553f78`, with `specgen/generated/json/ni_packet.json` clean in `git status`. The hash sees something git does not, most likely line-ending normalisation. Combined with the row above this means the drift gate can pass across a real input difference, so a clean `--check` is weaker evidence than it reads |
| Parallel C++ builds are unreliable on this WSL/Docker host | The original unbounded `-j` caused compiler failures under memory pressure. A later controlled comparison found `JOBS=2` could also produce a linked gtest executable that segfaulted on every `--gtest_list_tests` invocation, while the same source, image and ccache rebuilt at `JOBS=1` passed 20/20 list runs and two complete `make check` gates. The root `Makefile` therefore defaults to `JOBS=1`; override only on a Linux host verified under parallel load. Separately, removing Verilator's forced `--output-split 0` keeps generated C++ split into manageable translation units | `Makefile`, `JOBS`; `sim/verilator/Makefile` |
| The simulator segfaulted about once in ten runs inside `axi_file_master::load_files`, and is not reproducible as of 2026-08-12 | The 2026-08-11 note called it "same binary, same arguments". It was not: `STIM_ROOT` carries no seed, `gen` regenerates the stimulus into that same directory on every run with `--seed $(SEED)`, and an unset `SEED` is drawn randomly (`sim/verilator/Makefile`, `SEED`). The ten runs therefore had ten different stimulus sets, so the input was the varying quantity and not the environment. 48 runs with explicit seeds on `mesh_4x4` at 1 VC `neighbor`, 40 at `INJECTION_COUNT` 64 and 8 at 200, all reached 16 of 16 `[HWM]` lines and rc 0. At a one-in-ten rate that outcome has probability 0.006, so whatever it was does not hold for this configuration now. **On the next occurrence: take the seed off the `>>> sim` line, which every run prints, and re-run with `SEED=` set to it.** Reproducing means the stimulus content is the cause and `parse_write` is where to look; not reproducing rules the stimulus out and leaves the host. The first place to look inside `parse_write` is its W-beat loop, which is bounded by `current_aw.ax_len` even on the branch where the AW parse failed and that field was never assigned | `sim/verilator/Makefile`, the `sim run` recipe; `sim/dv/axi-0.39.7/src/axi_test.sv`, `parse_write` |
| ctest discovery lists go stale in an incremental build tree and inflate the reported count | 665 tests registered against 705 present in the binaries after successive builds over each other. A pass count from a tree that was not wiped is not evidence |
| The AWUSER collective field layout is spelled with raw bit numbers in three places with no shared constant | `axi/types.hpp`, `sim/tools/gen_test_patterns.py`, `sim/tb/test/user_node_endpoint.sv`. The `static_assert` in `types.hpp` pins the width sum but not the offsets, so an offset change desynchronises all three silently. The SV copy also hardcodes `2'd1` where `ni_flit_pkg::COLLECTIVE_OP_MULTICAST` exists |
| `axi_bw_monitor.sv` carries a two-line local edit | upstream it or wrap it |
| specgen `examples/quickstart` printf column padding | misaligned since the S0 rename, cosmetic |

## Simulator flows

The VCS flow builds and has never been executed. Two things to check before the first run: the
generated fabric drives one packed vector from a port connection (bit 0) alongside an
`always_comb` (bits 1-4), which Verilator tolerates and VCS may reject; and deleting only
`noc_fabric_<topo>.sv` leaves the build failing on a missing file rather than regenerating it.

The former `/mnt/e` checkout plus rsync mirror workflow is retired. The canonical WSL working
copy is the Linux-native `$HOME/work/noc_project` clone, and Windows copies synchronize through
Git. `tools/ic-team.sh` rejects `/mnt/*` before launching an agent. The unbounded `-j` that used
to take the host down mid-build is fixed; see the compiler-error row above.

A tile interconnect decodes addresses and nothing else, so it cannot tell a collective write from
a unicast. A collective whose address names the issuing node's own region therefore looks local,
and the
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
