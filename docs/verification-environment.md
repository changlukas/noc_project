# Verification environment

Environment details for the DUT (NMU + NoC routers + NSU) testbench: how the
testbench is generated, what drives and checks it, and how to reproduce a run.
For DUT design content (SAM, RoB, VC allocation, known limitations), see
`docs/spec.md`.

## Testbench architecture

`sim/tools/gen_tb_top.py` reads a topology YAML (`sim/topologies/*.yaml`) and
emits two generated files, never hand-edited:

| file | content |
|---|---|
| `src/sv/noc_fabric_<topology>.sv` | N nodes (`ni_wrap` = NMU + NSU, plus REQ/RSP `router_wrap`), joined by directional (N/E/S/W) inter-router links via a `genvar` generate loop. Boundary directions are tied off; a tied-off direction driving a valid flit is a `$fatal`. |
| `sim/tb/tb_top_<topology>.sv` | self-clocked (10 ns clock, 4-cycle reset) top: DPI create calls for every router/NMU/NSU context, the fabric instance, one `user_node_endpoint` per node, the watchdog, and the exit logic. |

Each node's `user_node_endpoint` (`sim/tb/user_node_endpoint.sv`, hand-written,
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
| `axi_rand_slave` (`MAPPED` mode) | tile memory backing the node's address window | `slave_dv` |
| `axi_scoreboard` | in-endpoint, wired on `master_dv`; per-transaction write-vs-readback data-integrity check | `master_dv` |
| `axi_bw_monitor` | passive throughput and latency monitor | `master_dv` |

## Provenance

The AXI master/slave/memory model algorithms in `src/c_model/include/axi/`
are ported to C++17 from cocotbext-axi (MIT license). The VIP set above is
vendored under `sim/dv/` from the pulp `axi` / `common_verification` /
`common_cells` packages and a FlooNoC test component (Solderpad 0.51);
per-package version, upstream commit, and the one flagged local modification
(`axi_bw_monitor.sv`, a two-line `$display` addition) are in
`sim/dv/README.md`. This is the only section in this document that names
external IP; the rest of this document describes the environment in the
DUT's own vocabulary. `docs/spec.md`'s References section carries the
upstream RTL file map.

## Traffic-pattern semantics

`sim/tools/gen_test_patterns.py` emits per-node stimulus
(`<out>/node<i>/{write,read}.txt`) for the directed driver. Destination
rules are ported from an established NoC simulator's traffic-pattern
classes (upstream reference in `docs/spec.md` References):

| pattern | destination rule | note |
|---|---|---|
| `neighbor` | `dst = ((x+1) mod X, (y+1) mod Y)` | diagonal shift per axis, wraps at the mesh edge; deterministic bijection |
| `transpose` | `dst = (y, x)` | requires a square, power-of-two mesh; diagonal nodes (x==y) self-target |
| `uniform_random` | independent uniform draw per transaction | self-traffic permitted by default (`--exclude-self` opts out); seeded |
| `hotspot` | one or more chosen nodes draw the traffic, weighted by `--hotspot-rates` | many-to-one congestion |

Addresses are allocated so converging sources never collide: local offset =
`base + src_node * stride + seq * (n_nodes * stride)` inside the destination
tile's window, with payload address-in-data so the checker has a computable
golden value without a separate scoreboard model.

## Checkers and non-vacuous pass

- `axi_scoreboard.enable_all_checks()` + `.monitor()` arm read-data, B-resp,
  and R-resp checks against the write golden, sampled on `master_dv`. This
  requires the write-before-read precondition (see injection modes below).
- `tb_top` counts AW/AR handshakes per node (`txn_cnt_o`). `PASS` requires
  `txn_cnt_o > 0` on every node, so a run where a node completed zero
  transactions cannot report clean.
- `DIRECTED PASS` (the `make sim` console line) additionally requires the
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
- Checkers are trusted only once fault injection has shown they fire on a
  deliberately planted violation; a checker that has never caught a planted
  mismatch verifies nothing.
- The constrained-random axis (a random driver plus a reorder-based checker)
  was retired 2026-07-13. No sound data-integrity checker for random traffic
  exists yet; a future random axis is deferred (see `docs/spec.md` Known
  limitations).

## Topology YAML to generator to testbench

A topology is one YAML file under `sim/topologies/`:

```yaml
topology:
  name: mesh_4x4_vc1
  x_dim: 4
  y_dim: 4
  num_vc: 1

address_map:            # optional; defaults to tile_size 0x1_0000_0000
  tile_size: 0x100000000
```

`gen_tb_top.py` rejects a topology whose mesh dimensions or `num_vc` exceed
the flit field capacity (`X_WIDTH`/`Y_WIDTH`/`VC_ID_WIDTH` from the flit
spec) before emitting anything. `sim/verilator/Makefile` regenerates
`tb_top_<TOPOLOGY>.sv` whenever the topology YAML, the generator, or the
`TOPOLOGY` make variable changes. Adding a topology needs only a new YAML
file; `make sim TB=<name> PATTERN=<p>` picks it up with no other change.
`_rob` appended to the topology name (e.g. `mesh_4x4_vc8_rob`) selects the
NMU reorder-buffer-enabled build of the same mesh.

## Seed handling

`make sim` accepts `SEED=<n>`. Left unset, it draws a random 30-bit seed
(`RANDOM*32768+RANDOM`, under Verilator's `+verilator+seed+` int32 ceiling)
and prints it:

```
>>> sim TB=mesh_4x4_vc1 PATTERN=neighbor SEED=538912734
```

The same seed drives both the stimulus generator (`--seed`, used by
`uniform_random`/`hotspot`) and the simulator's own `+verilator+seed+`, so
passing back the printed value reproduces the run exactly.
