# NI design specification

As-built design spec for the AXI4 NoC network-interface behavioural model
(`src/c_model/`) and its Verilator co-simulation (`src/sv/`, `src/dpi/`,
`sim/`). Every mechanism described here exists in the tree; deliberate
simplifications and open gaps are listed under
[Known limitations](#known-limitations). Design alternatives and their
rejections live in `docs/trade-off.md`.

## Overview

~~~
AXI4 master --> NMU --> REQ/RSP router mesh --> NSU --> AXI4 slave
~~~

The NMU (master-side NI unit) packetizes AXI4 transactions into NoC flits
and reorders returning responses. The NSU (slave-side NI unit) depacketizes
request flits, drives the downstream AXI4 slave, and packetizes B/R
responses. Per-node wormhole VC routers carry the flits on two physical
networks (REQ and RSP).

Verification intent: demonstrate AXI4 (IHI 0022H) conformity at the NMU and
NSU pin boundaries before committing to a silicon implementation. The
router fabric is exercised as transport and checked end to end by a
write-to-readback scoreboard; AXI conformity claims attach to the NI
boundary, not to the fabric.

### Conformity scope

Covered (IHI 0022H):

| area | sections | where exercised |
|---|---|---|
| VALID/READY handshake, stall, backpressure | A3.2 | unit tests + wire-level co-sim (held-valid latches in every wrap) |
| Burst types INCR/WRAP/FIXED, length, alignment, 4 KB boundary | A3.4.1 | `protocol_rules.hpp` checks + unit tests + burst stimulus |
| Per-ID response ordering: same-ID R returns in AR-issue order; W follows AW (no WID in AXI4) | A5.3 | RoB / interlock design (below) + integration tests + co-sim scoreboard |
| Response codes; DECERR on out-of-bounds access | A3.4.4 | memory model returns DECERR past its bounds; unit tests |
| Exclusive access rules and monitor | A7.2.4 | `protocol_rules.hpp` + the C++ `AxiSlave` exclusive monitor, unit tests only |

Excluded, with reasons:

| exclusion | reason |
|---|---|
| Exclusive access through the fabric | co-sim stimulus issues no exclusive transactions; monitor coverage is unit-level |
| SLVERR | no component generates it and no scenario provokes it |
| Atomic transactions, user signals | out of scope; the endpoint bridge ties `aw_atop` and `*_user` off |
| Dual-clock CDC | single-clock model; CDC is a property of the RTL implementation |
| NoC-layer QoS | `NOC_QOS_WIDTH = 0`; `awqos`/`arqos` ride the AXI payload but drive nothing |

## Architecture

Dependency direction is bottom-up; a layer depends only on layers above it
in this table.

| layer | contents |
|---|---|
| `specgen/` | spec-as-code: JSON/YAML sources, `codegen.py` emits `ni_flit_constants.h`, `ni_params.h`, SV packages (committed, drift-gated) |
| `src/c_model/include/axi/` | AXI4 base types, master/slave BFM endpoints, memory model, `protocol_rules.hpp`, scoreboard |
| `src/c_model/include/ni/` | shared NI primitives: `PipelineStage`, `WormholeArbiter`, `make_virtual_networks` |
| `src/c_model/include/nmu/`, `nsu/` | the two NI units |
| `src/c_model/include/router/` | wormhole VC router + the four NI-to-NoC interface contracts (`NocReqOut`, `NocRspIn`, `NocReqIn`, `NocRspOut`) |
| `src/c_model/include/wrap/` | per-component co-sim adapters (`*Wrap` + `*WrapIo` POD wire bundles), `PerfCollector` |
| `src/dpi/`, `src/sv/`, `sim/` | DPI bridge, SV wrap modules, generated `tb_top_<topology>.sv`, testbench and tooling |

NMU/NSU asymmetries are role differences, not gaps:

| NMU | NSU | why |
|---|---|---|
| `AxiSlavePort` (accepts AW/W/AR) | `AxiMasterPort` (drives AW/W/AR) | opposite AXI faces |
| `Rob` (response reorder) | `MetaBuffer` (restore-and-home metadata) | response ordering toward the master is the master-side NI's job; the NSU carries no RoB by design |
| `addr_trans` SAM lookup | none | only the request side routes; the NSU homes responses via the echoed `src_id` |
| `staged_beats.hpp` S1 bridge (RoB-admitted beats + route meta) | packetize S1 stage registers | request-path vs response-path staging |

## Timing model

- Registered DPI tick: on every posedge `clk_i` each SV wrap samples the
  previous cycle's registered inputs, calls
  `cmodel_<comp>_set_inputs` -> `cmodel_<comp>_tick` ->
  `cmodel_<comp>_get_outputs`, then registers the outputs nonblocking so SV
  wires see them from the next cycle onward. One `tick()` equals one clock
  edge.
- Inside C++ a `tick()` mutates state immediately; on the SV side registered
  outputs settle after the edge. The wrap adapters absorb the difference by
  reading input wires before the tick and writing output wires after it.
- A beat advances one pipeline stage per tick. The router is a 3-stage
  pipeline: input register -> per-(input port, VC) FIFO with route compute at
  the head -> per-output wormhole/VC arbitration and crossbar -> output FIFO
  to the link.
- Handshake is registered: `can_accept_*()` reflects state latched at the
  previous tick, never combinational lookahead. Held-valid latches keep
  `bvalid`/`rvalid` asserted until the matching ready (IHI 0022H A3.2.1).
- The generated `tb_top` is self-clocked (10 ns clock, 4-cycle reset);
  `sim/verilator/main.cpp` is a minimal `eval()` + time-advance loop and
  never toggles the clock. The same file drives VCS (`-top tb_top`).

## Address translation

The System Address Map (SAM) is a range-lookup table, not a bit-slice.

- `addr_trans::SamTable` holds `{base, size, dst_id}` entries, loaded from
  the topology YAML `address_map` block (`sam_yaml.hpp`), the single source
  shared by the model, the stimulus generator, and the testbench.
  `SamTable::uniform` builds the regular map
  `dst_id = (y << X_WIDTH) | x`, `base = dst_id * tile_size`.
- `translate(addr)` first-matches a tile and returns `{dst_id, local_addr}`
  with `local_addr = addr - base`: the slave sees a 0-based local address,
  not a coordinate-bearing global one. The wire format is unchanged;
  `dst_id` was already a header field and the payload address field now
  carries the rebased value.
- `validate()` enforces 4 KB-aligned `base` and `size`, no overlaps, and
  destinations inside the mesh. An AXI-legal burst (at most 4 KB, never
  crossing a 4 KB boundary, IHI 0022H A3.4.1) therefore never crosses a
  tile; the NI never splits a burst. `burst_footprint_ok` plus
  `burst_last_byte` guard the full footprint, including the WRAP window.
- A lookup miss asserts. This is model policy (fail loud on a config or
  stimulus bug), not AXI-faithful behaviour; see
  [Known limitations](#known-limitations).

## Response ordering

Response ordering is owned by the NMU. Two header fields carry it across
the wire: `rob_req` (1 bit) and `rob_idx` (`ROB_IDX_WIDTH` = 8 bits). The
NSU stores both in its meta buffer and echoes them onto the B/R response;
the fabric never reads them.

`nmu::Rob` sits between `AxiSlavePort` and `{Packetize, Depacketize}`.
The B RoB is always on (a B slot is metadata only, so it is cheap; a
deliberate divergence from the upstream design where the B-side RoB is
optional). Only the R path has a mode, `RobMode`:

| mode | mechanism |
|---|---|
| `Disabled` (default) | per-AXI-ID single-outstanding interlock: `push_ar` refuses while `read_outstanding_[id]` is set; R(last) clears it. Same-ID ordering is a property of the interlock, not of the network. |
| `Enabled` | per-beat slot pool with `rob_idx` allocation, described below. Selected per topology by the `_rob` testbench suffix. |

`w_bursts_owed_` gates W beats behind their accepted AW. A single counter
suffices because AXI4 W beats strictly follow AW issue order (WID was
removed in AXI4).

### Admission and bypass

`push_aw` / `push_ar` (Enabled path) first gate on the per-ID order-list
depth `max_txns_per_id_`, then decide whether the transaction needs reorder
storage:

| branch | condition | effect |
|---|---|---|
| idle-ID bypass | this ID's order list is empty | no slot; nothing in flight can overtake the response. A bypassed read burst of any length is admissible, which is what makes bursts longer than the slot pool possible at all. |
| same-destination bypass | same `dst_id` as the previous same-ID push, and the ID has not yet fallen back | no slot; the response provably returns in order because the fixed VC id holds the streak on one VC per network (see [Virtual networks](#virtual-networks)) |
| fall-back | otherwise | allocate: AW takes 1 slot, AR takes `len + 1` slots (each R slot stores one beat of `rdata`); the `fallen_back_*` flag is sticky, so every later same-ID push allocates until a fresh streak starts on an empty list |

The allocated (or zero) base becomes `rob_idx` on the outbound flit with
`rob_req` marking slot ownership. Every accepted push appends
`{base, len + 1, rob_req}` to its ID's order list, slot or no slot.

### Allocator

The slot allocator is the upstream high-water scheme (see
[References](#references)): `alloc_write_` / `alloc_read_` bitsets mark
only the top of each allocated range, `write_free_space()` /
`read_free_space()` count the free run above the high-water mark, and space
returns only from the top. A hole below the mark stays unusable until the
mark retreats past it; the pool behaves as a stack. In RTL this is one
leading-zero count over the allocation bitmap; the model's linear scan
carries no timing cost (see [Known limitations](#known-limitations)).

### Release

- B: the arriving response marks its slot ready; the drain loop releases
  ready order-list heads into the committed queue.
- R: `rob_idx` on an R beat is the burst base, not the beat index; an
  arrival counter places beat i at `base + i`. Release is per beat:
  `read_release_offset_` walks forward while consecutive slots are ready,
  so a slot frees as its beat leaves rather than holding the whole burst.
- A slot returns to the pool when its release refcount reaches zero
  (`commit_b_exit` / `commit_r_exit`).

### Invariants

- A bypassed response must match the head of its ID's order list; the model
  aborts otherwise (integrity guard, `assert` plus `std::abort`).
- The per-ID order list never exceeds `max_txns_per_id_`; the admission
  gate refuses first.
- `rob_req = 1` traffic never enters the network without a reserved slot.
  This is the fabric's deadlock guarantee: a response with nowhere to go
  would stall in the NoC and block every flit behind it, so "no free slot,
  no request". Bypassed traffic needs no slot because its return order is
  guaranteed by construction (idle ID, or one fixed VC per network).
- `read_slot_hwm()` tracks peak R-slot occupancy (exported as
  `cmodel_nmu_read_slot_hwm`), the sizing telemetry for a future
  `r_rob_depth` cut.

### Parameter mapping

Upstream RoB sizes map onto constructor parameters (upstream names in
[References](#references)):

| upstream | here | default | meaning |
|---|---|---|---|
| `BRoBSize` | `b_rob_depth` | `NMU_ROB_B_DEPTH` = 32 | B slot pool (metadata only) |
| `RRoBSize` | `r_rob_depth` | `NMU_ROB_R_DEPTH` = 32 | R slot pool, one beat of `rdata` per slot |
| `MaxTxnsPerId` | `max_txns_per_id` | `NMU_MAX_TXNS_PER_ID` = 32 | per-ID order-list depth, gates admission |
| RoB `NumIds` | `AXI_ID_SPACE` | 256 | from `AXI_ID_WIDTH` = 8 |
| `MaxTxns` | NSU `max_outstanding` | 32 | meta buffer pool; belongs to the slave face, not the RoB |

`rob_idx` width (8) and pool depth are independent; the only constraint is
`2^ROB_IDX_WIDTH >= max(b_rob_depth, r_rob_depth)`. All three depths are
plumbed end to end: `NmuConfig` -> `NmuWrap::init` ->
`cmodel_nmu_create_ex` -> `make sim` variables `B_ROB_DEPTH`,
`R_ROB_DEPTH`, `MAX_TXNS_PER_ID`.

## NSU meta buffer

A request flit carries the master's AXI ID and, separately, the requesting
tile in `src_id`. `nsu::MetaBuffer` stores
`{src_id, upstream_id, rob_req, rob_idx}` per downstream ID at AW/AR egress
and restores the original ID into the B/R response via peek-plus-commit
(commit on `rlast` for reads). The response homes through `src_id`, never
through the AXI ID.

Capacity is a shared pool of `max_outstanding` entries per direction, not a
per-ID depth; a full pool backpressures through `write_full()` /
`read_full()`.

`remap_downstream_id(upstream_id, max_unique_ids)` chooses the ID presented
to the slave:

| `max_unique_ids` | downstream ID | consequence |
|---|---|---|
| 1 (default) | all-ones for every request | AXI forces the slave to respond in request order; the metadata store is FIFO-equivalent |
| `AXI_ID_SPACE` (256) | pass-through | the slave may return responses out of order across IDs; the store must be ID-addressable |

No other value is legal (the depacketize constructor asserts). The remap is
a function of `upstream_id` alone, matching the ported source; response
ordering no longer depends on this choice because the response-path fixed
VC id keys on `(dst_id ^ id)`, so same-ID streams from different sources
land on distinct keys. The C++ model keeps its 256-bucket array under both
settings, so neither the FIFO's area saving nor the ID-queue's cost is
modelled.

## Virtual networks

`make_virtual_networks(num_vc)` splits the VC space into two disjoint,
equal virtual networks (vnets): write vnet `{0 .. n/2-1}`, read vnet
`{n/2 .. n-1}`. `num_vc = 1` degenerates to one shared lane; odd
`num_vc > 1` is rejected loudly, message-class separation needs an equal
split for deadlock avoidance. REQ and RSP are separate physical networks
(one `Router` object each), so the two directions assign VCs
independently.

Request side (`nmu::VcArbiter`, a decorator over `NocReqOut`):

- AW draws from the write vnet, AR from the read vnet; candidates are
  scanned round-robin from a per-class pointer, first VC with pending space
  and downstream credit wins.
- Fixed VC id: a `rob_req = 0` AW/AR whose `(dst_id, id)` matches the ID's
  previous same-channel flit reuses that recorded VC instead of
  round-robining. A blocked fixed VC refuses rather than rerouting;
  rerouting a fixed-VC streak mid-flight is exactly the reorder the fixed
  VC exists to prevent. `rob_req = 1` flits are RoB-owned and order-free,
  so they always round-robin.
- W follows its AW's VC. The arbiter sits downstream of a
  `WormholeArbiter` that serializes AW and all its W beats, so one
  `current_aw_vc_` register suffices (reset on `wlast`).

Response side (`nsu::VcArbiter`, mirror over `NocRspOut`; no W logic
because B is single-flit and R streams through the RoB, not a wormhole):

- Fixed VC id, return path: a `rob_req = 0` B, and every R beat regardless
  of `rob_req`, maps to `vnet[(dst_id ^ id) % vnet.size()]`, a pure
  function with zero state. All beats of a burst share `(dst_id, rid)`, so
  burst coherence comes free. Full or no-credit refuses, never spills.
- `rob_req = 1` B is order-free at the NMU slot path and round-robins the
  write vnet.

Router (`router::Router`): XY dimension-order routing (X first, +y is
NORTH), computed at the input FIFO head. Per-output wormhole lock: an
output port is owned by one (input port, VC) pair from a packet's first
flit to its last; when unlocked, VC round-robin then input round-robin
picks the next packet. Flow control is credit-based per VC; the sender
reserves a credit at output-FIFO admission and the receiver pulses it back
when the input FIFO slot frees.

The fixed VC id is what makes the same-destination bypass safe under
multi-VC: the bypass assumes same-`(dst, id)` responses arrive in order,
and a round-robin VC spread would let two of them overtake each other
in-fabric. Fixing the streak to one VC per network restores the ordering
assumption while same-destination different-ID traffic keeps spreading.

## DPI ABI

- `cmodel_<comp>_create(name, ...)` returns a 64-bit integer handle (SV
  `longint unsigned`) encoding a `HandleBlock*`
  (`{magic, type, state, name, type-erased adapter}`), tracked in the
  process-wide `g_handle_registry`. A plain integer is used instead of SV
  `chandle` because VCS rejects `chandle` as a module port.
- Each wrap makes 3 calls per cycle: `set_inputs` / `tick` /
  `get_outputs`. Every ctx-taking handler validates through
  `REQUIRE_HANDLE` (session initialized, registry membership, magic/type
  self-consistency, Live state). `cmodel_finalize` walks the registry and
  runs each handle's typed deleter.
- Errors never cross the DPI boundary as exceptions: handlers catch, set a
  categorized error code, and the testbench polls `cmodel_check_error`
  every cycle, raising `$fatal` on non-zero.
- Wrap responsibility invariant: a `*Wrap::tick()` may only latch input
  wires, check `can_accept_*()` capacity, push beats, call the component's
  `tick()` exactly once, and read outputs into the output latch. Business
  logic (packetization, routing, reordering) is forbidden in the adapter;
  if an adapter needs logic, the c_model component is missing an API.

## Parameters

Every cross-language constant is single-sourced in
`specgen/source/constants.yaml`. `codegen.py` emits `ni_params.h` (C++) and
`ni_params_pkg.sv` (SV); both are committed and drift-gated
(`codegen.py --check` runs as a build gate, `make specgen_pytest` covers
the generator).

| parameter | default | consumer |
|---|---|---|
| `AXI_ID_WIDTH` / `AXI_ADDR_WIDTH` / `AXI_DATA_WIDTH` | 8 / 64 / 256 | all layers |
| `NOC_FLIT_WIDTH` | 408 | flit container, wraps, SV structs |
| `NOC_NUM_VC` | 1 (per-topology YAML override) | vnet split, router, wraps |
| `NOC_ROUTER_VC_DEPTH` | 4 | router input VC FIFOs, credit window, `link_perf_monitor` |
| `NOC_ROUTER_OUTPUT_FIFO_DEPTH` | 2 | router stage 3 |
| `NOC_SLAVE_VC_BUFFER_DEPTH` | 4 | NI-edge VC buffering |
| `NMU_ROB_B_DEPTH` / `NMU_ROB_R_DEPTH` / `NMU_MAX_TXNS_PER_ID` | 32 / 32 / 32 | `nmu::Rob` |
| `NMU_QUEUE_DEPTH` / `NMU_DEPKT_Q_DEPTH` / `NMU_ARBITER_FIFO_DEPTH` | 16 / 16 / 4 | NMU port and stage queues |
| `NSU_QUEUE_DEPTH` / `NSU_DEPKT_Q_DEPTH` / `NSU_ARBITER_FIFO_DEPTH` | 16 / 16 / 4 | NSU port and stage queues |
| `NSU_META_BUFFER_MAX_OUTSTANDING` / `NSU_META_BUFFER_MAX_UNIQUE_IDS` | 32 / 1 | `nsu::MetaBuffer` |

Runtime overrides ride `make sim` variables into plusargs:
`B_ROB_DEPTH`, `R_ROB_DEPTH`, `MAX_TXNS_PER_ID` (NMU RoB),
`MAX_UNIQUE_IDS`, `MAX_OUTSTANDING` (NSU). Topology (mesh dimensions,
`num_vc`, `address_map`) comes from `sim/topologies/<name>.yaml`, consumed
by `gen_tb_top.py` and the SAM loader.

## Verification environment

`docs/verification-environment.md` carries the full test-environment
description; this section fixes the design-relevant facts.

- DUT per node: NMU + REQ/RSP routers + NSU. Each node's
  `user_node_endpoint` bridges the flat wire structs to two `AXI_BUS_DV`
  interfaces, `master_dv` (drives the NMU) and `slave_dv` (driven by the
  NSU). The endpoint layout is symmetric per node; the fabric and NI wraps
  carry no DV code.
- Imported DV IP (provenance and modification flags in
  `sim/dv/README.md`): `axi_file_master` (directed two-phase driver:
  writes, barrier, reads), `axi_rand_slave` in MAPPED mode (tile memory),
  `axi_scoreboard` (master-face write-vs-readback data integrity),
  `axi_bw_monitor` (latency and throughput). An independent, widely used
  VIP set backs the conformity claim; a self-made BFM cannot certify
  itself.
- Non-vacuous pass: `PASS` requires `txn_cnt_o > 0` on every node, so a
  zero-transaction run cannot report clean. `DIRECTED PASS` additionally
  requires zero scoreboard mismatches.
- Watchdog: `TIMEOUT_BASE + K_CYC_PER_BEAT * (num_reads + num_writes) *
  MAX_BURST_BEATS * NUM_NODES` with `K_CYC_PER_BEAT = 40`, calibrated from
  the measured 15 to 30 cycles per beat at vc1 under full contention. On
  timeout the testbench prints per-node channel state and calls
  `cmodel_dump_fabric_state()` (every non-idle router FIFO, credit,
  wormhole lock, and NI stage) before `$fatal`.
- Checkers are trusted only after fault injection has shown they fire; a
  checker that has never caught a planted violation verifies nothing.
- The constrained-random axis was retired (2026-07-13): no sound
  data-integrity checker for random traffic exists yet. A future random
  axis must run on Linux; Verilator's constraint solver drives an external
  solver through a fork()-based pipe that Windows lacks.

## Traffic patterns

`gen_test_patterns.py` emits per-node stimulus files
(`node<i>/{write,read}.txt`) for `axi_file_master`. Destination rules are
ported from an established NoC simulator (see
[References](#references)):

| pattern | destination rule | note |
|---|---|---|
| `neighbor` | `dst = ((x+1) mod X, (y+1) mod Y)` | diagonal shift with wrap; deterministic bijection |
| `transpose` | `dst = (y, x)` | requires a square power-of-two mesh; diagonal nodes self-target |
| `uniform_random` | independent uniform draw per transaction | self-traffic permitted by default, seeded |
| `hotspot` | all (or weighted) traffic to chosen nodes | many-to-one congestion |

Addresses are SAM-composed (`dst tile base + src-partitioned local
offset`), so converging sources never collide; payload is
address-in-data, giving the scoreboard a computable golden value.

Two injection modes (`INJECTION_MODE`):

| mode | shape | checking |
|---|---|---|
| 0 (default) | directed two-phase: all writes drain, then reads | scoreboard armed; `DIRECTED PASS` gate |
| 1 | continuous, reads and writes interleaved, paced per cycle by `INJECTION_RATE` (a per-cycle Bernoulli gate, 0.0 to 1.0) | scoreboard disarmed (write-before-read does not hold); `axi_bw_monitor` gates, `result.csv` emitted |

`MAX_UNIQUE_IDS` and `MAX_OUTSTANDING` are NSU configuration, not
injection knobs. `make sim-injection-sweep PATTERN=<p>` sweeps the four VC
topologies over nine rates in mode 1.

## Performance counters

As-built instrumentation is NoC-side counters plus the DV bandwidth
monitor. No AXI-side per-transaction profile or trace machinery is built:
the `cmodel_perf_axi_txn` / `cmodel_perf_axi_backpressure` DPI signatures
exist but nothing drives them, and the corresponding output section was
dropped from the perf dump.

NoC counters (`PerfCollector`, dumped to `perf.json` via `+perf_out`):

| counter | source | semantics |
|---|---|---|
| `in_fifo_occ_max` / `out_fifo_occ_max` per router | `cmodel_perf_sample_tick` once per clock | max observed input/output FIFO occupancy |
| `flit_count` per link | `link_perf_monitor` (passive, per inter-router link) | cycles with a valid flit on the wire |
| `stall_cyc` per link | same | cycles with no flit moving while at least one VC credit counter is zero (credit-deficit backpressure) |

`link_perf_monitor` also asserts the credit protocol: valid with zero
credit on that VC, or an out-of-range `vc_id`, is an error.

DV bandwidth monitor (`axi_bw_monitor`, one `[Read]` + `[Write]` line per
node in `run.log`):

| field | semantics |
|---|---|
| `Latency: m +- s, N: n` | mean and stddev of per-transaction round-trip in cycles (response cycle minus request cycle) over n completed transactions |
| `BW` (bits/cycle) | accepted throughput: `beats * data_width / total_cycles` |
| `Util` (%) | `beats * 100 / total_cycles`; fraction of the one-beat-per-cycle peak. `BW = (Util/100) * data_width`: same measurement, two units |

Reading the numbers: directed runs show 1 to 2 percent Util by design (a
few transactions per node against a fast slave; read PASS and latency,
ignore Util; throughput questions belong to the mode-1 rate sweep).
Latency tracks hop distance: on a 4x4 mesh the `neighbor` pattern shows
three tiers, interior 2 hops, one-axis wrap 4, corner 6, because the mesh
has no torus links and the wrap routes back across the array. `[HWM]`
lines report buffer high-water marks, including the R-RoB peak from
`cmodel_nmu_read_slot_hwm`.

## Known limitations

| limitation | detail |
|---|---|
| SAM failure mode | `translate()` miss and a topology YAML without `address_map` fail via bare `assert`: fail-loud in a debug build, undefined under `NDEBUG`. Model policy only; a real interconnect returns DECERR on a decode miss, which the NI does not model. |
| Unswept sizing | `max_txns_per_id` = 32 is a placeholder, never depth-swept [TBD]. `r_rob_depth` = 256 (the full `rob_idx` space) is expressible via `R_ROB_DEPTH` but equally unswept. |
| RoB physical shape unmodelled | no SRAM/flip-flop distinction, no allocator timing (the model's linear scan stands in for a combinational leading-zero count), no area reporting. |
| Meta buffer storage | the 256-bucket array is kept under both `max_unique_ids` settings; the FIFO-vs-ID-queue cost difference is not modelled. |
| Verification gaps | no covergroups, no constrained-random axis, no wire-side SVA framework; no standing co-sim regression harness (fabric coverage relies on manual `make sim` runs); no slave-latency sweep axis. |
| Perf DPI in mode 1 | the perf dump is not wired for continuous-injection runs (`perf.json` is empty there), so the bandwidth-monitor cross-check is unrun. |
| VCS flow | build-only; no directed run target, never executed on a real VCS install. |
| Deferred header fields | `NOC_QOS_WIDTH`, `ROUTE_PAR_WIDTH`, `FLIT_ECC_WIDTH` are width-0 placeholders; QoS, route parity, and flit ECC are unbuilt. |
| Conformity exclusions | exclusive access is unit-level only; SLVERR unexercised; single-clock CDC approximation (see Conformity scope). |

## References

- IHI 0022H, AMBA AXI protocol specification (Arm Ltd.), cited by section
  throughout. The A5.3 rule set relied on for ordering: same-ID read data
  returns in AR-issue order, read data of different ARIDs may interleave,
  and AXI4 removed WID with write-data interleaving.
- Upstream RTL reference (FlooNoC, read-only): `floo_rob.sv` (slot pool,
  high-water allocator, idle-ID and same-destination bypass, per-beat
  release), `floo_rob_wrapper.sv` (RoB type selection), `floo_simple_rob.sv`
  (ring-pointer allocator, documented alternative, not chosen),
  `floo_meta_buffer.sv` (meta buffer, downstream-ID collapse),
  `floo_axi_chimney.sv` (single-flit B / RoB-side R split, `MaxTxns` on the
  slave face), `floo_wormhole_arbiter.sv` (per-output wormhole lock) and
  `floo_vc_arbiter.sv` (VC arbitration without the lock), `floo_pkg.sv` (parameter names `BRoBSize`, `RRoBSize`,
  `MaxTxnsPerId`, `MaxTxns`), `axi_bw_monitor.sv` (DV bandwidth monitor,
  imported with one flagged modification, `sim/dv/README.md`).
- ID-space narrowing for a small-`NumIds` RoB: `axi_id_remap.sv`
  (pulp-platform axi v0.39.7) is the standard component; noted as the RTL
  path if a dense ID space is ever needed. Imported DV IP provenance:
  `sim/dv/README.md`.
- Traffic patterns and injection process: BookSim2 `src/traffic.cpp`
  (`NeighborTrafficPattern`, `TransposeTrafficPattern`,
  `UniformRandomTrafficPattern`, `HotSpotTrafficPattern`) and
  `BernoulliInjectionProcess`; ported destination-rule by destination-rule
  in `gen_test_patterns.py`.
- Scenario and pattern schema: `sim/test_patterns/`, generated per run by
  `gen_test_patterns.py` from `sim/topologies/*.yaml`.
