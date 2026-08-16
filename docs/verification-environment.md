# Verification environment

Environment details for the DUT (NMU + NoC routers + NSU) testbench: how the
testbench is generated, what drives and checks it, and how to reproduce a run.
For DUT design content (SAM, RoB, VC allocation), see the block specs
`docs/nmu-spec.md`, `docs/nsu-spec.md`, and `docs/router-spec.md`.

## Conformity scope

Verification intent: demonstrate AXI4 (IHI 0022H) conformity at the NMU and
NSU pin boundaries. The router fabric is exercised as transport and checked
end to end by the write-to-readback scoreboard; AXI conformity claims attach
to the NI boundary, not to the fabric.

Covered (IHI 0022H):

| area | sections | where exercised |
|---|---|---|
| VALID/READY handshake, stall, backpressure | A3.2 | unit tests + wire-level co-sim (held-valid latches in both NI wraps) |
| Burst types INCR/WRAP/FIXED, length, alignment, 4 KB boundary | A3.4.1 | `protocol_rules.hpp` checks + unit tests + burst stimulus |
| Per-ID response ordering: same-ID R returns in AR-issue order; W follows AW (no WID in AXI4) | A5.3 | RoB / interlock design + integration tests + co-sim scoreboard |
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

## Testbench architecture

The testbench sources sit in two directories, one per endpoint flavour, plus
what both compile:

| directory | endpoint | what it is |
|---|---|---|
| `sim/tb/test/` | `user_node_endpoint` | a pulp `axi_file_master` replaying generated stimulus against a tile crossbar of `axi_sim_mem` targets. The DV layer: the traffic is what a pattern named, so a run measures the fabric |
| `sim/tb/soc/` | `dma_node_endpoint` | a pulp iDMA backend in place of the replayer. The SoC layer: a real AXI manager picks its own bursts, outstanding depth and alignment, so a run measures conformance to one |
| `sim/tb/` | — | `axi_vip_types_pkg.sv` and `link_perf_monitor.sv`, compiled into both |

`sim/tools/gen_tb_top.py` reads a topology YAML (`sim/topologies/*.yaml`) and
emits two generated files, never hand-edited:

| file | content |
|---|---|
| `ref_model/top/noc_fabric_<topology>.sv` | N nodes (`ni_wrap` = NMU + NSU + `dat_merge_wrap`, plus `router_wrap`), joined by directional (N/E/S/W) inter-router links via a `genvar` generate loop. Boundary directions are tied off; a tied-off direction driving a valid flit is a `$fatal`. |
| `sim/tb/test/tb_top_<topology>.sv` | self-clocked (10 ns clock, 4-cycle reset) top: DPI create calls for every router/NMU/NSU context, the fabric instance, one `user_node_endpoint` per node, the watchdog, and the exit logic. |

`router_wrap` carries three physical networks, each with its own flit width
(`ni_params_pkg`: REQ 136 b, RSP 126 b, DAT 633 b):

| network | flow control | carries |
|---|---|---|
| REQ | `SimpleRouter`, ready/valid, 1-2 stages | Aw/W/Ar/DataAr requests |
| RSP | `SimpleRouter`, ready/valid, 1-2 stages | B/NarrowR responses |
| DAT | credit router, VC-capable | DataAw/DataW/DataR payload |

Link ports use a `tx_<net>_*`/`rx_<net>_*` pin contract (`<net>` = `req`/`rsp`/`dat`):
`tx` is this router's output to the peer, `rx` is the peer's output into this
router; adjacent nodes cross-wire `tx` to `rx`. DAT is the one shared
physical LOCAL port between NMU and NSU (NMU sources DataAw/DataW, NSU
sources DataR); `dat_merge_wrap` (`ref_model/top/dat_merge_wrap.sv`) arbitrates the
two onto `router_wrap`'s single DAT LOCAL rx/tx pair, sitting between
`nmu_wrap`/`nsu_wrap` and `router_wrap` inside `ni_wrap`.

Each node's `user_node_endpoint` (`sim/tb/test/user_node_endpoint.sv`, hand-written,
instantiated by the generator but not itself generated) presents two
`AXI_BUS_DV` interfaces and bridges them to the flat wire structs field by
field:

| interface | direction | drives / driven by |
|---|---|---|
| `master_dv` | feeds the NMU ingress | the directed file driver |
| `slave_dv` | driven by the NSU egress | the tile-memory slave |

```
directed driver --> master_dv --> NMU --> routers --> NSU --> slave_dv --> tile-memory slave
                     (scoreboard taps master_dv)
```

Every node carries the same endpoint layout; the fabric and the NI wraps
(`ni_wrap`, `router_wrap`) hold no verification code.

## VIP set

All four components are vendored under `sim/dv/` and run unmodified except
where noted.

| VIP | role | connects to |
|---|---|---|
| `axi_file_master` | directed driver, two-phase (drain writes, then issue reads) from per-node stimulus files | `master_dv` |
| `axi_sim_mem` | tile memory backing the node's address window, one per space | `tile_mem[t]` |
| `axi_delayer` | the tile memory's latency, separate from its storage; profile in `gen_tb_top.py` `_MEM_LATENCY` | between `tile_mst[t]` and `tile_mem[t]` |
| `axi_scoreboard` | in-endpoint, wired on `master_dv`; per-transaction write-vs-readback data-integrity check | `master_dv` |
| `axi_bw_monitor` | passive throughput and latency monitor | `master_dv` |

## Provenance

The AXI master/slave/memory model algorithms in `ref_model/c_model/include/axi/`
are ported to C++17 from cocotbext-axi (MIT license). The VIP set above is
vendored under `sim/dv/` from the pulp `axi` / `common_verification` /
`common_cells` packages and a FlooNoC test component (Solderpad 0.51);
per-package version, upstream commit, and the one flagged local modification
(`axi_bw_monitor.sv`, a two-line `$display` addition) are in
`sim/dv/README.md`. This is the only section in this document that names
external IP; the rest of this document describes the environment in the
DUT's own vocabulary.

Upstream references:

- IHI 0022H, AMBA AXI protocol specification (Arm Ltd.), cited by section
  throughout the block specs. The A5.3 rule set relied on for ordering:
  same-ID read data returns in AR-issue order, read data of different ARIDs
  may interleave, and AXI4 removed WID with write-data interleaving.
- FlooNoC RTL (read-only): `floo_rob.sv` (slot pool, high-water allocator,
  idle-ID and same-destination bypass, per-beat release), `floo_rob_wrapper.sv`
  (RoB type selection), `floo_simple_rob.sv` (ring-pointer allocator,
  documented alternative, not chosen), `floo_meta_buffer.sv` (meta buffer,
  downstream-ID collapse), `floo_axi_chimney.sv` (single-flit B / RoB-side R
  split, `MaxTxns` on the slave face), `floo_wormhole_arbiter.sv` (per-output
  wormhole lock), `floo_vc_arbiter.sv` (VC arbitration without the lock),
  `floo_vc_assignment.sv` (per-hop turn-model VC assignment, deprecated
  upstream, not ported), `floo_pkg.sv` (parameter names `BRoBSize`,
  `RRoBSize`, `MaxTxnsPerId`, `MaxTxns`), `axi_bw_monitor.sv` (DV bandwidth
  monitor, imported with one flagged modification).
- ID-space narrowing for a small-`NumIds` RoB: `axi_id_remap.sv`
  (pulp-platform axi v0.39.7), instantiated as `i_noc_id_remap` in
  `user_node_endpoint.sv`. Its input is the crossbar master-port id space,
  `AXI_INITIATOR_ID_WIDTH` + `$clog2(XBAR_SLV_PORTS)` = 5 b, not
  `AXI_INITIATOR_ID_WIDTH` itself, so it folds 32 tile ids onto the NI's
  `AXI_ID_WIDTH` = 3, 8 ids.
- Traffic patterns and injection process: BookSim2 `src/traffic.cpp`
  (`NeighborTrafficPattern`, `TransposeTrafficPattern`,
  `UniformRandomTrafficPattern`, `HotSpotTrafficPattern`) and
  `BernoulliInjectionProcess`, ported destination-rule by destination-rule
  in `gen_test_patterns.py`.

## Traffic-pattern semantics

`sim/tools/gen_test_patterns.py` emits per-node stimulus
(`<out>/node<i>/{write,read}.txt`) for the directed driver. Destination
rules are ported from an established NoC simulator's traffic-pattern
classes (upstream reference in Provenance):

| pattern | destination rule | note |
|---|---|---|
| `neighbor` | `dst = ((x+1) mod X, (y+1) mod Y)` | diagonal shift per axis, wraps at the mesh edge; deterministic bijection |
| `transpose` | `dst = (y, x)` | requires a square, power-of-two mesh; diagonal nodes (x==y) self-target |
| `uniform_random` | independent uniform draw per transaction | self-traffic permitted by default (`--exclude-self` opts out); seeded |
| `hotspot` | one or more chosen nodes draw the traffic, weighted by `--hotspot-rates` | many-to-one congestion. `HOTSPOT_PERIPHERALS=1` names the target set off the router array instead: every tile draws from the whole declared peripheral set, weighted by `--hotspot-rates` exactly as the tile target set is. The peripheral keeps its own initiator traffic; the partner tile's extra lines are dropped, since every tile now addresses the peripheral out of the same slot band the partner drew from. Rejected on any other pattern, where it would suppress those lines and steer nothing |
| `multicast` | source nodes issue masked AW writes over a row, column, or 2x2-submesh member set (`MCAST_SHAPE`, one shape per run), then read back every member replica; other nodes carry unicast filler | AWUSER carries the address mask. `INJECTION_MODE=0` only, since one write answers to N readbacks. On a config topology each source also issues one narrow-class multicast into the members' config tiles, the config-space replication case. Ignores `--ids-per-initiator` |

The `multicast` schedule is what satisfies restriction R1 (`docs/router-spec.md`
Section 2.10), which the fabric does not enforce: trees of one shape are pairwise
disjoint across sources, and each source's own multicasts share one AXI id so the
NMU's R2 gate serializes them on the merged `B`. Mixing shapes in one run would
overlap trees at shared eject outputs. Any future collective stimulus carries the
same obligation: concurrently in-flight multicasts must have disjoint spanning
trees or be serialized on the merged `B`. An R1 violation surfaces as a wedge, not
as an error message.

Addresses are allocated so converging sources never collide: local offset =
`base + src_node * stride + seq * (n_nodes * stride)` inside the destination
tile's window, with payload address-in-data so the checker has a computable
golden value without a separate scoreboard model.

### Stimulus file format

Each transaction is a fixed-order block of lines. Read (AR) and write (AW) share
the address-phase fields; write adds `atop` and the W-beat lines.

`read.txt`, 11 lines per AR:

| line | field |
|---|---|
| 1 | AXI id |
| 2 | address (`0x...`) |
| 3 | len (ARLEN, beats minus 1) |
| 4 | size (log2 bytes per beat; the data bus is 512 b / 64 B: 5 = 32 B half-bus, 6 = 64 B full-bus) |
| 5 | burst (1 = INCR) |
| 6 to 10 | lock, cache, prot, qos, region |
| 11 | user |

`write.txt`, 12 lines per AW then one line per W beat:
- lines 1 to 10 as above, then line 11 `atop`, line 12 `user`.
- then `len + 1` beat lines, each `0x<data> 0x<strb> 0` (`w_data`, `w_strb`,
  `w_user`), with address-in-data payload (byte at address A holds `A & 0xFF`),
  full strobe, sized to the data bus. `w_last` is not in the file; the driver
  derives it from the burst length.

The default stimulus (`--size 5`, single beat) is a 32 B half-bus write: 12
field lines plus 1 beat line; the matching read is the 11 field lines at the
same address.

## Checkers and non-vacuous pass

- `axi_scoreboard.enable_all_checks()` + `.monitor()` arm read-data, B-resp,
  and R-resp checks against the write golden, sampled on `master_dv`. This
  requires the write-before-read precondition (see injection modes below).
- `tb_top` counts AW/AR handshakes per node (`txn_cnt_o`). `PASS` requires
  `txn_cnt_o > 0` on every node, so a run where a node completed zero
  transactions cannot report clean.
- `DIRECTED PASS` (the `make -C sim` console line) additionally requires the
  scoreboard to report zero mismatches: the run log must reach `PASS: all N
  nodes done, non-vacuous` and carry no scoreboard-mismatch or protocol-error
  string.
- Injection mode selects the run shape and which checker gates:

  | `INJECTION_MODE` | shape | checking |
  |---|---|---|
  | `0` (default) | two-phase: all writes drain, then reads | scoreboard armed, `DIRECTED PASS` gate |
  | `1` | continuous, reads and writes interleaved, paced per cycle by `INJECTION_RATE` | scoreboard disarmed (write-before-read does not hold); `axi_bw_monitor` gates instead, `result.csv` emitted |
  | `2` | continuous, writes paced per cycle by `INJECTION_RATE`, each read issues after its paired write's B response | scoreboard armed, `CHECKED PASS` gate |

  Mode 1 exists to measure throughput and latency under load, not to check
  data. Mode 2 is the data-integrity axis under continuous write load: the
  per-pair B interlock restores the scoreboard's write-before-read
  precondition, at the cost of a read stream that couples to response
  latency (so it does not measure offered injection rate).
- The `multicast` pattern adds a second scoreboard in `user_node_endpoint.sv`,
  keyed by replica address instead of by transaction. It captures a byte golden
  from each W beat of a masked write, replicates it to every member address, and
  compares the per-member readback:

  | check | failure |
  |---|---|
  | replica readback | `[mcast_sb] node<i>: replica readback mismatch addr=... exp=... act=...` |
  | non-vacuous | golden captured but zero replica bytes compared. A clean source node prints `[mcast_sb] node<i>: <n> replica byte compares against <m> golden bytes` |
  | single merged B | B handshake count must equal the node's AW count, and no `B` may remain asserted after the last write retires: `duplicate or lost B` / `extra B` |
  | merged BRESP | any non-OKAY merged `B` |

  The unicast filler in the same run stays under the standard scoreboard, so a
  clean multicast run also carries zero `Unexpected RData` from the AXI VIP
  monitor.
- `MCAST_FAULT=1` is the fault-injection proof for that scoreboard: it XORs
  `8'h01` into the captured replica golden, so the run must fail on the replica
  readback mismatch. A green `MCAST_FAULT=1` run means the compare is vacuous.
- Checkers are trusted only once fault injection has shown they fire on a
  deliberately planted violation; a checker that has never caught a planted
  mismatch verifies nothing.
- The constrained-random axis (a random driver plus a reorder-based checker)
  was retired. No sound data-integrity checker for random traffic
  exists yet; a future random axis is deferred (see Known limitations).

Per-task verification tiers (which pattern/topology combination a change must
pass before it counts as done) are not duplicated here: they are a standing,
frequently-updated section of `docs/backlog.md` ("Verification (acceptance)").

## Performance counters

As-built instrumentation is NoC-side counters plus the DV bandwidth monitor.
No AXI-side per-transaction profile or trace machinery is built, and the perf
dump carries no AXI section.

NoC counters (`PerfCollector`, dumped to `perf.json` via `+perf_out` at the
end of every run, in all injection modes):

| counter | source | semantics |
|---|---|---|
| `in_fifo_occ_max` / `out_fifo_occ_max` per router | `cmodel_perf_sample_tick` once per clock | max observed input/output FIFO occupancy |
| `flit_count` per link | `link_perf_monitor` (passive, per inter-router link) | cycles with a valid flit on the wire |
| `stall_cyc` per link | same | cycles with no flit moving while at least one VC credit counter is zero (credit-deficit backpressure) |

`link_perf_monitor` also asserts the credit protocol: valid with zero credit
on that VC, or an out-of-range `vc_id`, is an error.

DV bandwidth monitor (`axi_bw_monitor`, one `[Read]` + `[Write]` line per node
in `run.log`):

| field | semantics |
|---|---|
| `Latency: m +- s, N: n` | mean and stddev of per-transaction round-trip in cycles (response cycle minus request cycle) over n completed transactions |
| `BW` (bits/cycle) | accepted throughput: `beats * data_width / total_cycles` |
| `Util` (%) | `beats * 100 / total_cycles`; fraction of the one-beat-per-cycle peak. `BW = (Util/100) * data_width`: same measurement, two units |

Reading the numbers: directed runs show 1 to 2 percent Util by design (a few
transactions per node against a fast slave; read PASS and latency, ignore Util;
throughput questions belong to the mode-1 rate sweep). Latency tracks hop
distance: on a 4x4 mesh the `neighbor` pattern shows three tiers, interior 2
hops, one-axis wrap 4, corner 6, because the mesh has no torus links and the
wrap routes back across the array. The `[HWM]` lines report the per-node NMU
sizing statistics: the R-RoB slot peak from `cmodel_nmu_read_slot_hwm`, plus the
order-list peak, the peak in-flight transaction count per direction and the
admission clause
split (AW and AR separately) from `cmodel_nmu_admission_stats`.

## Topology YAML to generator to testbench

A topology is one YAML file under `sim/topologies/`:

```yaml
topology:
  name: mesh_4x4_vc1
  x_dim: 4
  y_dim: 4
  num_vc: 1

address_map:
  block_size: 0x100000000     # every node's block; memory sits at offset 0, config above it
  tiles:                      # ordered, every mesh node exactly once, row-major (y outer, x inner)
    - { x: 0, y: 0, size: 0x2000000 }
    - { x: 1, y: 0, size: 0x2000000 }
    # ... one entry per node
    - { x: 0, y: 0, size: 0x1000, space: config }  # config tiles, one per node
```

`space` selects the AXI class the tile decodes into: `config` maps to the
narrow class, `memory` (the default when `space` is omitted) maps to the data
class. Every shipped topology gives each node one memory tile and one config
tile (`nmu::addr_trans::SamTable::validate`).

The SAM is a first-match `{base, size, dst_id}` range table, loaded from this
block and shared by the C++ loader and both Python generators: the generator
places each request at `base(dst) + offset`, and the NMU SAM translates the
address back to `dst_id`. One source, so the two never disagree.

`tiles:` gives each node its own `size`; there is no `tile_size` and no
`base` key. Bases come from the coordinate and the block stride -- the sim
YAML's `block_size` key, what `docs/noc-target-spec.md` §5.1 calls
`node_stride` -- not accumulation: `base = idx * block_size + offset[space]`, where
`idx = (y << x_bits) | x`, `x_bits` is `clog2(x_dim)`, and `offset[space]` is
0 for memory and the memory slot rounded up to the config slot for config
(`nmu::addr_trans::SamTable::packed`), `slot` being the largest size declared
in that space. `block_size` is either declared (`address_map.block_size`) or
defaults to the next power of two at or above what the spaces occupy
(`sam_yaml.hpp`'s `default_block_size`, mirrored in `sim/tools/address_map.py`'s
`pack()`). A tile smaller than its space's slot
leaves a gap inside its own node's block; nothing shifts, because every
node's base already comes from `idx * block_size`, never from a neighbor's
extent. The node index and the routing id use different shifts: `dst_id =
(y << X_WIDTH) | x`. The loader accepts a heterogeneous map (covered by
`test_node_windows_are_that_node_s_own_map_entries` in
`sim/tools/test_gen_test_patterns_filemaster.py`), but every shipped topology
uses a `0x100000000` block per node, holding a `0x2000000` memory tile at
offset 0 and a `0x1000` config tile at `0x2000000`, in raster order, config
entries appended after the memory entries. `TILE_TARGETS` is therefore 2 on
every topology, and the endpoint carries one decode path, the two-window
one. The windows are per node and global — nothing rebases, so the tile
decodes on the same bases the SAM matched. A disagreement between the two
shows up as an address outside both windows, which DECERRs; the endpoint's
`DECERR_FAULT_BIT` fault injection and the RRESP fatal in
`sim/tb/test/user_node_endpoint.sv` check that path.

Raster order is what makes the node index a contiguous bit field an AWUSER
address mask can wildcard: memory bases at `idx * block_size`, config bases
at `idx * block_size + 0x2000000`.

`gen_tb_top.py` rejects a topology whose mesh dimensions or `num_vc` exceed
the flit field capacity (`X_WIDTH`/`Y_WIDTH`/`VC_ID_WIDTH` from the flit
spec) before emitting anything. `sim/verilator/Makefile` regenerates
`tb_top_<TOPOLOGY>.sv` whenever the topology YAML, the generator, or the
`TOPOLOGY` make variable changes. Adding a topology needs only a new YAML
file; `make -C sim TB=<name> PATTERN=<p>` picks it up with no other change.
`READ_ROB=0|1` selects the NMU read response path: 1 (the default) the
reorder buffer, 0 the RoBless bypass. It reaches the generated top as its
`READ_ROB_ENABLED` localparam, and joins the topology stamp so a switch
re-emits the tb.

## Seed handling

`make -C sim gen` and `make -C sim sim` each accept `SEED=<n>`. Left unset,
each draws its own random 30-bit seed (`RANDOM*32768+RANDOM`, under
Verilator's `+verilator+seed+` int32 ceiling) and prints it:

```
>>> gen TB=mesh_4x4_vc1 PATTERN=neighbor SEED=538912734
>>> sim TB=mesh_4x4_vc1 PATTERN=neighbor SEED=538912734
```

`gen` uses `SEED` for the stimulus generator (`--seed`, used by
`uniform_random`/`hotspot`); `sim` uses it for the simulator's own
`+verilator+seed+`. Passing the same value to both reproduces a run exactly:
generation and simulation are separate commands, so the value has to be
supplied to each rather than drawn once and shared.

## Known limitations

| limitation | detail |
|---|---|
| SAM failure mode | `translate()` miss and a topology YAML without `address_map` fail via bare `assert`: fail-loud in a debug build, undefined under `NDEBUG`. Model policy only; a real interconnect returns DECERR on a decode miss, which the NI does not model. |
| Unswept sizing | `NMU_MAX_TXNS_PER_ID` = 32 (per-ID order-list depth) is the one depth that was swept: 32 down to 1, every point a non-vacuous PASS. At four ids the run peaked at 30 of the then-32-entry shared pool against a deepest per-ID list of 12, which is what identified the pool rather than the per-ID depth as the binding limit. No throughput figure was taken at any point of the sweep. `NMU_ROB_B_DEPTH`/`NMU_ROB_R_DEPTH` default to 128 (S2) and are expressible up to 256 (the full `ordering_tag` space) via `B_ROB_DEPTH`/`R_ROB_DEPTH`; a burst whose beats (len+1) exceed the RoB depth fails loud (`Rob::push_ar` assert) instead of wedging. Equally unswept at every setting. |
| RoB physical shape unmodelled | no SRAM/flip-flop distinction, no allocator timing (the model's linear scan stands in for a combinational leading-zero count), no area reporting. |
| Verification framework gaps | no covergroups, no wire-side SVA framework, no standing co-sim regression harness (fabric coverage relies on manual `make -C sim` runs), no slave-latency sweep axis. The retired constrained-random axis is covered under Checkers. |
| Meta buffer storage | the 8-bucket array is kept under both `max_unique_ids` settings; the FIFO-vs-ID-queue cost difference is not modelled. |
| AXI-side perf instrumentation absent | `perf.json` carries only the NoC section (dumped at the end of every run, all injection modes); no AXI-side per-transaction hooks exist, so nothing cross-checks `axi_bw_monitor` from the model side. |
| VCS flow | build-only; no directed run target, never executed on a real VCS install. |
| Deferred header fields | QoS, route parity, and flit ECC are unbuilt and have no header field at all: the 48 b header is fully assigned (`PADDING_FIELDS_COUNT` = 0) and carries no width-0 placeholder for them. |
| Conformity exclusions | exclusive access is unit-level only; SLVERR unexercised; single-clock CDC approximation (see Conformity scope). |
| NI ingress backpressure unmodelled | not modeled on any network as of S3a: `ready` tied true / DAT merge self-credits, ingress queues unbounded, LOCAL stall metrics 0 by construction. Reassessed at S3b: request-class (`DataAw`/`DataW`) and response-class (`DataR`) messages now share DAT VCs post-collapse. This stays deadlock-safe SOLELY because ingress queues are unbounded and always accept. Any future work that bounds ingress queues must first re-open message-class separation (or an equivalent VC-classing scheme). |
| SimpleRouter multi-read ruling (S3b) | grants up to one flit per OUTPUT per tick from the same input FIFO, matching the credit `Router`. Mainline `floo_router.sv` has one FIFO read port, a resource limit, not a protocol requirement. Kept as a deliberate c_model-optimistic divergence: multi-output fan-out from one input in a single tick that RTL would need more than one cycle for. |
| `ready_slack` calibration deferred (S3b) | `SimpleRouterConfig::ready_slack` default (2) is PROVISIONAL, never measured against a real wire loop. The compliant-sender high-water mark sits exactly at FIFO depth (zero spare). Needs a measured wire-loop experiment before the default is load-bearing. |
| vc{2,4,8} re-baseline (S3b) | the VA stage reassigns the downstream VC per hop for `fixed_vc=0` traffic. Co-sim/perf numbers on vc{2,4,8} topologies shift by construction versus pre-S3b. Matrix green after S3b means re-baselined, not bit-identical to the earlier numbers. |
