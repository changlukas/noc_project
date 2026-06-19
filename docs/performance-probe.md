# NoC performance probe

The probe emits one `perf.json` and one stdout summary per co-sim run. It measures
per-transaction latency, per-interface throughput and occupancy, and link
backpressure on the running DUT.

Verified properties:

- Monitors tap signals only; an A/B run (monitors on vs. off) gives identical DUT
  cycles (§7).
- Single pass; the best-case reference is the same-run minimum (§4).

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

## Measurement definitions

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
  `valid` on credit, so `valid && !credit` never occurs on the wire.

## Counters and derived metrics

`perf.json` stores raw counters; rates are derived. Full schema: spec §5.1
(`docs/superpowers/specs/2026-06-18-perf-pmu-cosim-design.md`).

| Section | Raw fields | Derived |
|---|---|---|
| `axi_slots[]` | write/read byte count, txn count, `slave_write_idle_cyc` (`WVALID && !WREADY`), `master_read_idle_cyc` (`RVALID && !RREADY`), `outstanding_max`; subordinate slots add `service_latency` | throughput = byte_count ÷ window cycles |
| `latency.by_signature[]` | per `(op, src, dst, len, size)`: count, min, mean, max | `min` = best-case observed |
| `latency.histogram[]` | per-`[low, high)` bin count | latency distribution |
| `latency.transactions[]` | per-transaction rows (id, dir, src, dst, accept_cyc, complete_cyc, latency, bytes) | §5 |
| `noc.routers[]` | input/output FIFO occupancy peak | — |
| `noc.links[]` | flit count, `stall_cyc` | utilization, backpressure |

Router flit throughput is not counted: the `Router` model exposes occupancy but no
flit counter, and adding one would change the DUT. Link flit count carries
throughput.

The AXI counter set mirrors the AMD AXI Performance Monitor (PG037):

| perf.json | PG037 |
|---|---|
| `axi_slots[]` counters | per-interface monitor slot, profile-mode accumulators |
| byte count, txn count, latency | per-agent metric set |
| `slave_write_idle_cyc` / `master_read_idle_cyc` | slave-write-idle / master-read-idle |
| `latency.histogram[]` | Range Incrementer |
| run window cycle base | Global Clock Counter |

Not in this model: per-id breakdown (PG037 ID Filter), periodic sampled-accumulator
snapshots, the event-log/AXI4-Stream trace path, AXI4-Lite register readout, and
external-trigger gating. Write latency ends at B-response (initiator-observed
completion), beyond PG037's write-data endpoints (first/last write).

Diagnostics:

| Signal | Likely cause |
|---|---|
| `by_signature.max` ≫ `min` for a class | downstream contention for that flow |
| `outstanding_max` high | NMU RoB / NSU queue pressure |
| `stall_cyc > 0` on a link | that link's downstream VC buffer filled |
| `slave_write_idle_cyc` high | slave slow to accept write data |
| `master_read_idle_cyc` high | master slow to accept read data |

## Profile view (aggregate)

| View | Field |
|---|---|
| Per-class min/mean/max | `latency.by_signature[]`, grouped by `(op, src, dst, len, size)` |
| Distribution | `latency.histogram[]` |
| Best-case reference | `by_signature[].min`, the same-run minimum |

The stdout summary prints the signatures and the histogram.

## Drill-down view (per transaction)

`latency.transactions[]` holds one row per completed manager-slot transaction,
JSON-only. It is the drill-down for an outlier seen in the profile.

## Latency composition

For a single 32-byte transaction (`AX4-BAS-003`, AxSIZE=5, AxLEN=0, 2-node
`tb_top`, ROBLESS), the measured round-trip:

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

- Monitors are passive: input-only ports, no drives. The C sampler calls only const
  accessors.
- A manual A/B comparison (monitors on vs. off) gave identical scoreboard results
  and completion cycles. Spec §4 requires an automated gate; none is checked in yet.
- No area or power numbers: the probe is co-sim only, not synthesized.

## Known limitations

- One window per run; periodic time-series snapshots are deferred (spec §3.1).
- `+perf_start`/`+perf_end` window gating not implemented.
- `stall_cyc` and the idle counters are exercised only on lightly-loaded scenarios
  (observed 0); stress scenarios are needed.
- The read vs. write shell-boundary residual (4 vs. 2 cycles) is an unrooted co-sim
  artifact.
