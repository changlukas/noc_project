# NoC performance probe

In-fabric, single-run performance monitors that co-simulate with the DUT and emit
one `perf.json` plus one stdout summary per run. The probe measures per-transaction
latency, per-interface throughput/occupancy, and link backpressure on the running
co-sim. Two properties hold and are verified, not asserted:

- **Non-intrusive:** the monitors tap signals passively; the DUT's cycle behavior
  is unchanged — passive by construction, A/B-confirmed (§7).
- **Single-run:** no second pass and no isolated zero-load characterization; the
  best-case reference is the minimum observed in the same run (§4).

Read it top-down: aggregate first (§4 profile), then drill into a single
transaction (§5).

## How to run

```
make run-tb-top SCENARIO=<scenario-id>
```

Output, beside `run.log`:

```
cosim/verilator/output/<scenario-id>/perf.json
```

`perf_cli_summary.py` prints the stdout summary automatically after each run.

## 1. Probe placement

Monitors tap the AXI interfaces and the inter-router links; router occupancy is
sampled on the C side. All are read-only.

| Tap point | Monitor | Where |
|---|---|---|
| AXI manager edge (master ↔ NMU) | `axi_perf_monitor.sv` | per node |
| AXI subordinate edge (NSU ↔ slave) | `axi_perf_monitor.sv` | per node |
| Inter-router link | `flit_link_perf_monitor.sv` | per link direction |
| Router input/output FIFO occupancy | C sampler `cmodel_perf_sample_tick` | per router |

The 2-node `tb_top` instantiates 4 AXI-slot monitors (manager + subordinate per
node) and one link monitor per link direction. A C-side `PerfCollector` gathers the
SV counters via DPI plus the C-sampled occupancy and writes `perf.json`. Split
rationale (some data is on the AXI wire, some only in the c_model): spec §4.

## 2. Measurement definitions

These fix what each counter means; they are the reference for every number below.

| Quantity | Definition |
|---|---|
| Latency start | Address-accept: `AWVALID & AWREADY` (write), `ARVALID & ARREADY` (read) |
| Read latency end | `RLAST & RVALID & RREADY` |
| Write latency end | B-response: `BVALID & BREADY` (not WLAST) |
| Byte count | beats × bytes/beat, accumulated per interface |
| Outstanding (peak) | issued-but-not-completed transactions, sampled per cycle |
| Link stall | cycles with `credit_count == 0` (downstream VC buffer full) |

Latency definitions follow AMD PG037 (AXI Performance Monitor), which defines read
latency as "the time from the start of read address issuance/acceptance to the
beginning/ending of the read data transaction" and notes the start/end points are
"user configurable" (PG037 §"Metrics computed for an AXI4 agent"). Our choices:
read latency ends at RLAST (= PG037's default "to the last read"); write latency
ends at the **B-response, not WLAST**. The B-response choice diverges from the
PG037 default (last write data): a write is not complete until the initiator
receives its response, and PG037 itself states "sending all of the write data ...
must not be taken as indicating completion." Measuring to WLAST would understate
the latency a manager observes.

Same-id correlation uses a per-`(id, direction)` FIFO of issue cycles; completion
pops the FIFO front (valid because AXI4 returns same-id responses in issue order).
FIFO underflow or overflow asserts (`$fatal`) — a measurement error is never
silently dropped.

Link stall is `credit_count == 0`, not `valid && !credit`. The link credit is a
single-cycle pulse, so `!credit` is the normal idle state, and the upstream router
gates `valid` on credit, so `valid && !credit` never fires on the wire. Cross-
reference `noc.routers[].out_fifo_occ_max` to separate real backpressure from an
idle-but-full link.

## 3. Counters and derived metrics

`perf.json` carries raw counters; rates are derived from them. Full schema: spec
§5.1 (`docs/superpowers/specs/2026-06-18-perf-pmu-cosim-design.md`).

| Section | Raw fields | Derived |
|---|---|---|
| `axi_slots[]` | write/read byte count, write/read txn count, `slave_write_idle_cyc`, `master_read_idle_cyc`, `outstanding_max`; subordinate slots add `service_latency` | throughput = byte_count ÷ window cycles |
| `latency.by_signature[]` | per `(op, src, dst, len, size)`: count, min, mean, max | `min` = best-case observed reference |
| `latency.histogram[]` | per-`[low, high)` bin transaction count | latency distribution |
| `latency.transactions[]` | raw per-transaction rows (id, dir, src, dst, accept_cyc, complete_cyc, latency, bytes) | §5 drill-down |
| `noc.routers[]` | input/output FIFO occupancy peak | — |
| `noc.links[]` | flit count, `stall_cyc` | link utilization, backpressure |

The AXI metric set follows AMD PG037 (AXI Performance Monitor): one monitor slot per
AXI interface, with per-slot accumulators (PG037's profile mode). Mapping:

| Our element | PG037 block / metric |
|---|---|
| `axi_slots[]` per-interface counters | Monitor slot + profile-mode Accumulators (PG037 Fig 1-4) |
| write/read byte count, transaction count, latency, slave-write-idle, master-read-idle | the per-agent metric list (PG037 §"Metrics computed for an AXI4 agent") |
| `latency.histogram[]` | Range Incrementer (PG037 Fig 1-3) |
| run window cycle base | Global Clock Counter (PG037 Fig 1-1) |
| `perf.json` readout | the register read-out (PG037 uses AXI4-Lite; here a DPI JSON dump) |

Implemented metric definitions, with the matching PG037 metric named:

- byte count = beats × bytes/beat (`(len+1)<<size`), `perf_collector.hpp` — PG037
  "byte count ... helpful when calculating throughput".
- `slave_write_idle_cyc` = count of cycles with `WVALID && !WREADY`
  (`axi_perf_monitor.sv`) — PG037 slave-write-idle ("clocks between WVALID and
  WREADY assertion").
- `master_read_idle_cyc` = count of cycles with `RVALID && !RREADY` — PG037
  master-read-idle.

Divergences from PG037 (not implemented): write latency ends at B-response not
last-write (§2); per-id breakdown (PG037 ID Filter), the periodic
sampled-accumulator time-series, the event-log/AXI4-Stream trace path, and
external-trigger gating. Router flit throughput is not counted — the `Router` model
exposes occupancy but no flit counter, and adding one would change the DUT; link
flit count carries throughput instead.

Diagnostic reading:

- `by_signature.max` rising above `min` for a class → downstream contention for that
  flow.
- `outstanding_max` high → NMU RoB / NSU queue pressure at that interface.
- `stall_cyc > 0` on a link → that link's downstream VC buffer filled.
- `slave_write_idle_cyc` / `master_read_idle_cyc` → AXI handshake idle (slave slow to
  accept write data / master slow to accept read data).

## 4. Profile view (aggregate)

`latency.by_signature[]` aggregates transactions by `(op, src, dst, len, size)` and
reports min/mean/max; `latency.histogram[]` gives the distribution. The stdout
summary prints these per signature.

`min` is the minimum observed latency for that class in this run — the
least-congested completed transaction. It is labelled "min observed", not
"zero_load". On a lightly-loaded run (single sequential transactions) it equals the
no-queueing network floor.

## 5. Drill-down view (per transaction)

`latency.transactions[]` holds one row per completed transaction (id, direction,
src, dst, accept/complete cycle, latency, bytes), JSON-only (not printed). It is the
drill-down for an outlier seen in §4: profile gives the trend, the per-transaction
rows give the individual transaction that caused it.

## 6. Latency composition and fidelity

End-to-end latency has three architectural sources plus one co-sim residual. For a
single 32-byte transaction (`AX4-BAS-003`, AxSIZE=5, AxLEN=0, 2-node `tb_top`,
ROBLESS mode), the measured round-trip decomposes as:

| Source | Write (27 cyc) | Read (28 cyc) | Nature |
|---|---|---|---|
| NI pipeline | 10 | 10 | real register-parked staged pipeline, 4 paths |
| Router pipeline | 12 | 12 | real 3-stage pipeline × 4 traversals |
| Slave service | 3 | 2 | memory model write/read latency + slave-edge handshake |
| Co-sim shell boundary | 2 | 4 | `*_wrap.sv` registered output per module crossing (non-architectural) |

NI stage allocation (ROBLESS = co-sim default; ROB adds one NMU-rsp stage):

| Path | Stages | Blocks |
|---|---|---|
| NMU req (AXI→NoC) | 3 | `AxiSlavePort`+`Rob` alloc → `Packetize` → `WormholeArbiter`+`VcArbiter` |
| NSU req (NoC→slave) | 2 | `Depacketize`+`MetaBuffer` snapshot → `AxiMasterPort` |
| NSU rsp (slave→NoC) | 3 | `AxiMasterPort` → `Packetize` → `WormholeArbiter`+`VcArbiter` |
| NMU rsp (NoC→AXI) | 2 ROBLESS / 3 ROB | `Depacketize` → [`Rob` re-order] → `AxiSlavePort` |

- **Router and NI are both real pipelines.** Each advances a beat one stage per
  tick via a reverse-order tick (later stages drain before earlier fill, so no
  same-tick double-advance). NI design: spec §5. NI contributes 10 of the 27 write
  cycles.
- **Co-sim shell boundary is a residual, not an independently measured counter.**
  The NI (stage count), router (3×4), and slave (config latency) terms are from the
  model; the shell-boundary term is the **residual** = measured total − those three.
  Each `*_wrap.sv` registers its DPI outputs once per clock, which accounts for it.
  The residual is 2 (write) / 4 (read); the read's extra boundary cycle is an
  unrooted co-sim artifact. It does not fold into the NI stage count.

Per-stage occupancy is observable via `Nmu/Nsu::stage_occupancy(path, stage,
axi_ch)`, mirroring the router's `input_fifo_size`/`output_fifo_size`.

## 7. Non-intrusive and overhead

The probe adds zero cycles to the DUT, by construction:

- SV monitors (`axi_perf_monitor.sv`, `flit_link_perf_monitor.sv`): input-only
  ports, no drives.
- C-side sampler (`cmodel_perf_sample_tick`): calls only const accessors
  (`Router::input_fifo_size`/`output_fifo_size`); does not mutate the model.

A/B equivalence (monitors present vs. absent → identical scoreboard result and
per-transaction completion cycles) was confirmed by a manual build comparison. An
automated A/B gate is mandated by the spec (§4) but is not yet a checked-in test.

Area/power overhead is not reported: this is a co-simulation behavior monitor, not
synthesized RTL. The only relevant overhead — perturbation of the DUT — is zero
(passive by construction).

## 8. Known limitations

- **Time-series profile:** a single window per run; periodic sampled-accumulator
  snapshots (latency/throughput over time) are deferred (spec §3.1).
- **Window gating:** `+perf_start`/`+perf_end` are not implemented; the window is
  the whole run.
- **Stress coverage:** `stall_cyc` and the AXI idle counters are exercised only on
  lightly-loaded scenarios (observed 0); pipelined same-id and deliberately
  back-pressured scenarios are needed to observe non-zero backpressure.
- **Co-sim shell residual:** the read vs. write boundary asymmetry (4 vs. 2 cycles,
  §6) is an unrooted co-sim artifact, not microarchitecture.
