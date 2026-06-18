# NoC performance probe

Single-run, in-fabric PMU-style monitors co-simulate with the DUT. One
`perf.json` per run; one CLI summary to stdout. No second pass, no zero-load
characterization.

## How to run

```
make run-tb-top SCENARIO=<scenario-id>
```

Output lands beside `run.log`:

```
cosim/verilator/output/<scenario-id>/perf.json
```

The CLI summary (`perf_cli_summary.py`) prints automatically after each run.

## Metrics

Full JSON schema: spec §5.1 (`docs/superpowers/specs/2026-06-18-perf-pmu-cosim-design.md`).

| Section | Content | Grain |
|---|---|---|
| `axi_slots[]` | byte/transaction count, two AXI idle counters, outstanding peak; subordinate slots add `service_latency` | per AXI interface |
| `latency.transactions[]` | raw per-transaction end-to-end rows (JSON-only; not in CLI) | manager slot |
| `latency.by_signature[]` | min/mean/max grouped by `(op, len, size, dst)`; `min` = best-case observed | per signature class |
| `latency.histogram[]` | per-`[low, high)` bin transaction count | whole run |
| `noc.routers[]` | input/output FIFO occupancy peak | per router |
| `noc.links[]` | flit count + credit-stall cycles | per link |

## Latency definitions

| Event | Definition |
|---|---|
| Start | Address-accept: `AWVALID & AWREADY` (write), `ARVALID & ARREADY` (read) |
| Read end | `RLAST & RVALID & RREADY` |
| Write end | B-response: `BVALID & BREADY` |

**Write-end divergence from PG037:** PG037 measures write latency to first/last
write data (WLAST). This probe measures to the B-response, because a write
transaction is not architecturally complete until the initiator receives its
B-response — WLAST-to-B-response cycles are real latency the initiator must
wait for. Measuring to WLAST understates the latency a manager observes.

Same-id correlation: the AXI monitors maintain a per-(id, direction) FIFO of
issue cycles. On completion, the FRONT of the FIFO is popped. This correctly
handles multiple outstanding same-id transactions in AXI4 issue order.

## Credit-deficit link stall

The link monitors maintain a credit counter seeded to `BUFFER_DEPTH`. Each
credit pulse increments it; each flit sent decrements it. A **stall cycle** is
any cycle where `credit_count == 0` (downstream buffer full). The credit signal
is a single-cycle pulse (not a level), so `!credit` is the normal idle state
and is not counted as stall.

## Best-case reference

`latency.by_signature[].min` is the **minimum observed** latency for that
signature class in this run — the least-congested completed transaction. It is
labelled "min observed", not "zero_load". For a lightly-loaded run (single
sequential transactions), min equals the network latency floor with no
queueing.

## Non-intrusive

The SV monitors (`axi_perf_monitor.sv`, `flit_link_perf_monitor.sv`) are
passive: input-only ports, no drives. The C-side (`cmodel_perf_sample_tick`)
reads only const getters. An A/B build (monitors present vs. absent) must
produce identical scoreboard result and per-transaction completion order.
