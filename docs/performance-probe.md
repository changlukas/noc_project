# NoC performance probe

In-fabric, single-run PMU-style monitors that co-simulate with the DUT and emit
one `perf.json` plus one stdout summary per run. No second pass, no zero-load
characterization. Latency, throughput, occupancy, and backpressure are measured
on the running co-sim.

## How to run

```
make run-tb-top SCENARIO=<scenario-id>
```

Output, beside `run.log`:

```
cosim/verilator/output/<scenario-id>/perf.json
```

`perf_cli_summary.py` prints the stdout summary automatically after each run.

## Metrics

Full JSON schema: spec §5.1 (`docs/superpowers/specs/2026-06-18-perf-pmu-cosim-design.md`).

| Section | Content | Grain |
|---|---|---|
| `axi_slots[]` | write/read byte count, write/read transaction count, two AXI idle counters, outstanding peak; subordinate slots add `service_latency` | per AXI interface |
| `latency.transactions[]` | raw per-transaction end-to-end rows (JSON only) | manager slot |
| `latency.by_signature[]` | min/mean/max grouped by `(op, src, dst, len, size)`; `min` = best-case observed | per signature class |
| `latency.histogram[]` | per-`[low, high)` bin transaction count | whole run |
| `noc.routers[]` | input/output FIFO occupancy peak | per router |
| `noc.links[]` | flit count, credit-stall cycles | per link |

The PG037 agent metric set (transaction count, byte count, latency, idle cycles)
maps to `axi_slots[]`; the latency histogram maps to PG037's Range Incrementer.
Router flit throughput is not counted (the `Router` model exposes occupancy but
no flit counter, and adding one would modify the DUT); link flit count carries
throughput instead.

## Latency definitions

| Event | Definition |
|---|---|
| Start | Address-accept: `AWVALID & AWREADY` (write), `ARVALID & ARREADY` (read) |
| Read end | `RLAST & RVALID & RREADY` |
| Write end | B-response: `BVALID & BREADY` |

Write end is the B-response, not WLAST. A write transaction is not complete until
the initiator receives its response; the WLAST-to-B-response interval is latency
the initiator waits through. This diverges from the PG037 default (which measures
write latency to the last write beat) and understates nothing the manager observes.

Same-id correlation uses a per-`(id, direction)` FIFO of issue cycles; completion
pops the FIFO front. This handles multiple outstanding same-id transactions in
AXI4 issue order. FIFO underflow or overflow asserts (`$fatal`) — a measurement
error is never silently dropped.

## Credit-deficit link stall

The link monitors maintain a credit counter seeded to the downstream VC buffer
depth: each credit pulse increments it, each flit sent decrements it. A stall
cycle is any cycle with `credit_count == 0` (downstream buffer full).

The link credit signal is a single-cycle pulse, not a level. `!credit` is the
normal idle state, so `valid && !credit` is not a valid stall condition — the
router gates `valid` on credit, so that term never fires on the wire. Cross-
reference `noc.routers[].out_fifo_occ_max` to distinguish real backpressure from
an idle-but-full link.

## Best-case reference

`latency.by_signature[].min` is the minimum observed latency for that signature
class in this run — the least-congested completed transaction. It is labelled
"min observed", not "zero_load". On a lightly-loaded run (single sequential
transactions) min equals the no-queueing network floor.

## Latency composition and pipeline fidelity

End-to-end latency has three sources, all now reflecting real microarchitecture.
For a single 32-byte transaction (AX4-BAS-003, AxSIZE=5, AxLEN=0, 2-node tb_top),
the measured round-trip decomposes as:

**Write (27 cycles measured):**

| Source | Cycles | Nature |
|---|---|---|
| NI pipeline (NMU req + NSU req + NSU rsp + NMU rsp) | 10 | real register-parked staged pipeline (3 + 2 + 3 + 2 stages, ROBLESS) |
| Router pipeline | 12 | real 3-stage registered pipeline × 4 traversals (req + rsp, 2 routers each) |
| Slave service | 3 | memory model `write_latency` + slave-edge handshake |
| Co-sim shell boundary (residual) | 2 | `*_wrap.sv` registered output per module-boundary crossing (see below) |

**Read (28 cycles measured):**

| Source | Cycles | Nature |
|---|---|---|
| NI pipeline | 10 | same 4-path staged pipeline as write |
| Router pipeline | 12 | same 4 traversals |
| Slave service | 2 | memory model `read_latency` |
| Co-sim shell boundary (residual) | 4 | same `*_wrap.sv` artifact; read R-beat path adds 1 extra boundary cycle vs. write B-beat |

**NI stage allocation (ROBLESS mode, co-sim default):**

| Path | S1 | S2 | S3 | Stages |
|---|---|---|---|---|
| NMU req (AXI→NoC) | `AxiSlavePort` + `Rob` alloc | `Packetize` | `WormholeArbiter` + `VcArbiter` | 3 |
| NSU req (NoC→slave) | `Depacketize` + `MetaBuffer` snapshot | `AxiMasterPort` drain | — | 2 |
| NSU rsp (slave→NoC) | `AxiMasterPort` accept (B/R) | `Packetize` | `WormholeArbiter` + `VcArbiter` | 3 |
| NMU rsp (NoC→AXI, ROBLESS) | `Depacketize` | `AxiSlavePort` push | — | 2 |
| NMU rsp (NoC→AXI, ROB Enabled) | `Depacketize` | `Rob` re-order stage | `AxiSlavePort` push | 3 |

- **Router is a real pipeline.** `Router` advances a flit one stage per tick
  through landing → input FIFO → output FIFO (`c_model/include/noc/router.hpp`,
  3 stages). Four traversals (request path: 2 routers; response path: 2 routers)
  = 12 cycles.
- **NI is now a real staged pipeline.** Each NMU/NSU sub-module is register-parked;
  a beat advances exactly one stage per tick. The reverse-order tick (later stages
  drain before earlier fill) enforces no same-tick double-advance, matching the
  router model. NI accounts for 10 of the 27 write cycles (37%).
- **Co-sim shell boundary (separate, residual cost).** Each `*_wrap.sv` registers
  its DPI outputs once per clock (`set_inputs → tick → get_outputs → registered
  output`), adding 1 cycle per module-boundary crossing. This is a Verilator
  co-sim artifact, not NI microarchitecture. It is accounted separately and does
  not fold into the NI stage count.

Fidelity boundary: all three sources (NI pipeline, router pipeline, slave service)
now reflect real microarchitecture. The only non-architectural cost is the co-sim
shell boundary, clearly isolated in the table above.

## Non-intrusive

The SV monitors (`axi_perf_monitor.sv`, `flit_link_perf_monitor.sv`) are passive:
input-only ports, no drives. The C-side sampler (`cmodel_perf_sample_tick`) reads
only const getters. An A/B build — monitors present vs. absent — produces an
identical scoreboard result and identical per-transaction completion cycles.

## Known limitations

- **Window gating:** `+perf_start`/`+perf_end` are not implemented; the window is
  the whole run.
- **Stress coverage:** `stall_cyc` and the AXI idle counters are exercised only on
  lightly-loaded scenarios (observed 0). Pipelined same-id and deliberately
  back-pressured scenarios are needed to observe non-zero backpressure.
