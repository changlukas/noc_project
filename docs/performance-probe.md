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

End-to-end latency has three sources, only one of which is a real pipeline. For a
single 32-byte transaction (AX4-BAS-003, AxSIZE=5, AxLEN=0, 2-node tb_top), the
measured write round-trip of 21 cycles decomposes as:

| Source | Cycles | Nature |
|---|---|---|
| Router pipeline | 12 | real 3-stage registered pipeline × 4 traversals (request + response, 2 routers each) |
| NI boundary crossings | 6 | co-sim shell artifact (see below) + AXI/credit handshake |
| Slave service | 3 | memory model `write_latency` + slave-edge handshake |

- **Router is a real pipeline.** `Router` advances a flit one stage per tick
  through landing → input FIFO → output FIFO (`c_model/include/noc/router.hpp`,
  3 stages). Each traversal costs 3 cycles. Routers account for 12 of the 21
  cycles (57%).
- **NMU and NSU have no internal pipeline.** Their `tick()` is upstream-first and
  propagates a beat through all internal stages combinationally within one tick
  when unblocked (`c_model/include/nmu/nmu.hpp:135`,
  `c_model/include/nsu/nsu.hpp:118`). Internal pipeline depth is 0 stages. The
  queues they hold are elastic/reorder/handshake state, not fixed pipeline
  registers.
- **The per-boundary NI latency is a co-sim wrapping artifact.** Each `*_wrap.sv`
  registers its component outputs once per clock (`set_inputs → tick →
  get_outputs → registered output`), adding 1 cycle per module-boundary crossing
  regardless of internal logic. The 6 NI-boundary cycles are this shell register
  plus AXI/credit handshake, not NI microarchitecture.

Fidelity boundary: router latency reflects real microarchitecture; NI latency
does not. A consumer of `perf.json` must read the NI contribution as co-sim
boundary cost, not silicon NI pipeline depth.

## Non-intrusive

The SV monitors (`axi_perf_monitor.sv`, `flit_link_perf_monitor.sv`) are passive:
input-only ports, no drives. The C-side sampler (`cmodel_perf_sample_tick`) reads
only const getters. An A/B build — monitors present vs. absent — produces an
identical scoreboard result and identical per-transaction completion cycles.

## Known limitations

- **NI pipeline fidelity:** NMU/NSU are 0-cycle functional models; their measured
  latency is co-sim boundary cost, not a real pipeline. A real NI pipeline model
  is planned (the NI is DUT and will be implemented in RTL).
- **Window gating:** `+perf_start`/`+perf_end` are not implemented; the window is
  the whole run.
- **Stress coverage:** `stall_cyc` and the AXI idle counters are exercised only on
  lightly-loaded scenarios (observed 0). Pipelined same-id and deliberately
  back-pressured scenarios are needed to observe non-zero backpressure.
