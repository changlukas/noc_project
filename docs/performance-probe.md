# NoC performance probe

The probe emits one `perf.json` and one stdout summary per co-sim run. It measures
per-transaction latency, per-interface throughput and occupancy, and link
backpressure on the running DUT.

Verified properties:

- Monitors tap signals only; an A/B run (monitors on vs. off) gives identical DUT
  cycles (see *Non-intrusive and overhead*).
- Single pass; the best-case reference is the same-run minimum (see *Profile view*).

## How to run

```
make run-tb-top SCENARIO=<scenario-id>
```

Output: `cosim/verilator/output/<scenario-id>/perf.json`, beside `run.log`.
`perf_cli_summary.py` prints the summary after each run.

## Probe placement

| Tap point | Monitor |
|---|---|
| AXI manager edge (master ↔ NMU), per node | `axi_perf_monitor.sv` |
| AXI subordinate edge (NSU ↔ slave), per node | `axi_perf_monitor.sv` |
| Inter-router link, per direction | `flit_link_perf_monitor.sv` |
| Router FIFO occupancy, per router | C sampler `cmodel_perf_sample_tick` |

The 2-node `tb_top` instantiates 4 AXI monitors and one monitor per link direction.
A C-side `PerfCollector` gathers the SV counters via DPI plus the sampled occupancy
and writes `perf.json`.

**Figure 1: Probe placement on the 2-node `tb_top`.** All taps are input-only.

```text
        NODE 0                                              NODE 1
   AXI master                                            AXI master
      |                                                       |
    [M0] axi_perf_monitor                  axi_perf_monitor [M1]   <- manager edge
      |                                                       |
     NMU0                                                   NMU1
      |                                                       |
   +--------+      [L0>1] flit_link_perf_monitor      +--------+
   |Router 0|========================================>|Router 1|
   |        |<========================================|        |
   +--------+      [L1>0] flit_link_perf_monitor      +--------+
      |   ^ in/out FIFO occ                                |
     NSU0   sampled by C: cmodel_perf_sample_tick()/cyc  NSU1
      |                                                       |
    [S0] axi_perf_monitor                  axi_perf_monitor [S1]   <- subordinate edge
      |                                                       |
   AXI slave                                             AXI slave

   [M*],[S*] = axi_perf_monitor (passive AXI tap, input-only, no drives)
   [L*]      = flit_link_perf_monitor (one instance per link direction)
   Router in/out FIFO occupancy snapshotted once per cycle by the C sampler.
   A C-side PerfCollector aggregates all taps via DPI and writes perf.json.
```

## Measurement definitions

**Figure 2: Latency measurement points (when each counter starts and stops).**

```text
Write transaction
  start = AWVALID & AWREADY                      end = BVALID & BREADY
    |                                                 |
    v                                                 v
  --+--[ AW ]--[ W ... WLAST ]------------[ B ]-------+--
    |<================ write latency =================>|

Read transaction
  start = ARVALID & ARREADY                        end = RLAST & RVALID & RREADY
    |                                                   |
    v                                                   v
  --+--[ AR ]------------[ R ][ R ] ... [ R = RLAST ]---+--
    |<================ read latency ===================>|
```

| Quantity | Definition |
|---|---|
| Latency start | `AWVALID & AWREADY` (write), `ARVALID & ARREADY` (read) |
| Read latency end | `RLAST & RVALID & RREADY` |
| Write latency end | `BVALID & BREADY` |
| Byte count | beats × bytes/beat = `(len+1)<<size`, per interface |
| Outstanding (peak) | issued-but-not-completed transactions, sampled per cycle |
| Link stall | cycles with `credit_count == 0` (downstream VC buffer full) |

- Write latency ends at the B-response: the initiator observes completion at
  `BVALID/BREADY`, not at WLAST.
- Same-id correlation uses a per-`(id, direction)` FIFO of issue cycles; completion
  pops the front. FIFO underflow or overflow calls `$fatal`.
- Link stall reads the credit counter, not `valid`: the upstream router gates
  `valid` on credit, so `valid && !credit` never occurs on the wire. `stall_cyc`
  therefore counts *downstream-buffer-full* cycles: a necessary condition for a
  flit to block, not a confirmation that one did. Read it together with
  `flit_count`.

The idle and latency counters are **interface-local**: they describe the AXI
interface the monitor is tapping, and the physical agent behind a field depends on
which slot it is.

- On a **subordinate-edge** slot (`NSU ↔ slave`), `slave_write_idle_cyc` is the
  memory slave stalling.
- On a **manager-edge** slot (`master ↔ NMU`), the NMU drives `WREADY`, so the same
  `slave_write_idle_cyc` field is the **NMU** back-pressuring the external master,
  not a slave. `master_read_idle_cyc` is symmetric (the agent driving `RREADY` on
  that interface).

## Counters and derived metrics

`perf.json` stores counters and latency aggregates; rates are derived. Full schema: spec §5.1
(`docs/superpowers/specs/2026-06-18-perf-pmu-cosim-design.md`).

| Section | Raw fields | Derived |
|---|---|---|
| `axi_slots[]` | `role` (manager/subordinate), write/read byte count, write/read txn count, `slave_write_idle_cyc`, `master_read_idle_cyc`, `outstanding_max`; subordinate slots add `service_latency` | throughput = byte_count ÷ window cycles |
| `latency.by_signature[]` | per `(op, src, dst, len, size)`: count, min, mean, max | `min` = best-case observed |
| `latency.histogram[]` | per-`[low, high)` bin count | latency distribution |
| `latency.transactions[]` | per-transaction rows (id, dir, src, dst, accept_cyc, complete_cyc, latency, bytes) | drill-down (see *Drill-down view*) |
| `noc.routers[]` | input/output FIFO occupancy peak | n/a |
| `noc.links[]` | flit count, `stall_cyc` | utilization, backpressure |

Router flit throughput is not counted: the `Router` model exposes occupancy but no
flit counter. Link flit count carries throughput.

Metric definitions:

- Write/read transaction count: number of completed write/read transactions at the
  interface.
- Write/read byte count: total bytes written/read; used to compute throughput.
- Slave write idle cycles: cycles on this interface where `WVALID` is held without
  `WREADY` (subordinate side slow to accept write data). Interface-local; see the
  agent note under *Measurement definitions*.
- Master read idle cycles: cycles on this interface where `RVALID` is held without
  `RREADY` (manager side slow to accept read data). Interface-local.
- Latency histogram: counts transactions whose latency falls in each bin of a
  **fixed compile-time ladder**: `[0,16) [16,32) [32,64) [64,128) [128,256)
  [256,∞)` cycles (edges `{0,16,32,64,128,256}` in `emit_histogram`; the last bin is
  open-ended, encoded `high=0`). The ladder is not run-time configurable.

Not implemented: per-id breakdown, periodic time-series snapshots, an event-log /
streaming trace path, memory-mapped register readout, and external-trigger gating.

Diagnostics:

| Signal | Likely cause |
|---|---|
| `by_signature.max` ≫ `min` for a class | downstream contention for that flow |
| `outstanding_max` high | NMU RoB / NSU queue pressure |
| `stall_cyc > 0` on a link | that link's downstream VC buffer filled (necessary, not sufficient, for a blocked flit; cross-check `flit_count`) |
| `slave_write_idle_cyc` high (subordinate slot) | slave slow to accept write data |
| `slave_write_idle_cyc` high (manager slot) | NMU slow to accept write data from the master |
| `master_read_idle_cyc` high | the `RREADY`-driving side of that interface slow to accept read data |

## Profile view (aggregate)

| View | Field |
|---|---|
| Per-class min/mean/max | `latency.by_signature[]`, grouped by `(op, src, dst, len, size)` |
| Distribution | `latency.histogram[]` |
| Best-case reference | `by_signature[].min`, the same-run minimum |

The stdout summary (`perf_cli_summary.py`) prints the AXI throughput / backpressure
table, the per-class signatures, the histogram, the subordinate-slot service
latency, and the NoC router/link table. Per-transaction rows are JSON-only and are
never printed.

## Drill-down view (per transaction)

`latency.transactions[]` holds one row per completed manager-slot transaction,
JSON-only. Use it to inspect the transaction behind a profile outlier.

## Latency composition

The figures below are a **measured instance**, not a fixed spec: a single 32-byte
transaction (`AX4-BAS-003`, AxSIZE=5, AxLEN=0, 2-node `tb_top`, ROBLESS). Numbers
move with scenario and configuration; re-measure before quoting.

**Figure 3: Round-trip latency composition (segments to scale; numbers above each
boundary are the cumulative cycle, Shell is the non-architectural co-sim residual).**

```text
Write round-trip = 27 cyc
  0            10                22      25   27
  |--- NI:10 --|--- Router:12 ---|-Slv:3-|Sh:2|

Read round-trip = 28 cyc
  0            10                22    24     28
  |--- NI:10 --|--- Router:12 ---|Slv:2|-Sh:4-|
```

| Component | Write (27) | Read (28) | Source |
|---|---|---|---|
| NI pipeline | 10 | 10 | staged pipeline, 4 paths (3+2+3+2) |
| Router pipeline | 12 | 12 | 3-stage pipeline × 4 traversals |
| Slave service | 3 | 2 | memory model latency + slave handshake |
| Shell boundary (residual) | 2 | 4 | `*_wrap.sv` registered output; co-sim, non-architectural |

NI stage allocation (ROB adds one NMU-rsp stage):

| Path | Stages | Blocks |
|---|---|---|
| NMU req | 3 | `AxiSlavePort`+`Rob` alloc → `Packetize` → `WormholeArbiter`+`VcArbiter` |
| NSU req | 2 | `Depacketize`+`MetaBuffer` → `AxiMasterPort` |
| NSU rsp | 3 | `AxiMasterPort` → `Packetize` → `WormholeArbiter`+`VcArbiter` |
| NMU rsp | 2 / 3 ROB | `Depacketize` → [`Rob` re-order] → `AxiSlavePort` |

- Router and NI advance one stage per tick via a reverse-order tick (later stages
  drain before earlier stages fill).
- The shell-boundary term is the residual: measured total minus NI, router, and
  slave. Per-stage occupancy is readable via `Nmu/Nsu::stage_occupancy(path, stage,
  axi_ch)`.

## Non-intrusive and overhead

- Monitors are passive: input-only ports, no drives. The C sampler reads FIFO
  occupancy and drives no DUT signal.
- A manual A/B comparison (monitors on vs. off) gave identical scoreboard results
  and completion cycles. Spec §4 requires an automated gate; none is checked in yet.
- No area or power numbers: the probe is co-sim only, not synthesized.

## Known limitations

- One window per run; periodic time-series snapshots are not implemented (spec §3.1).
- `+perf_start`/`+perf_end` window gating not implemented.
- `stall_cyc` and the idle counters are exercised only on lightly-loaded scenarios
  (observed 0); stress scenarios are needed.
- The read vs. write shell-boundary residual (4 vs. 2 cycles) is a co-sim artifact,
  not yet root-caused.
