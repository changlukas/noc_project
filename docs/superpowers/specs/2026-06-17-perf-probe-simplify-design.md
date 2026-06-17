# Perf probe simplification: zero-load calculator + per-component dwell

Date: 2026-06-17
Status: Draft (pending user review)
Supersedes: the metric set of `2026-06-17-ni-router-perf-observer-design.md`
(merged as PR #3). This design keeps the transaction-latency collection and
the `Stats` accumulator, and replaces the occupancy/credit-stall/histogram/
phase machinery with a leaner, latency-centric probe.

## 1. Motivation

The first probe collected too many workload-coupled metrics (outstanding, RoB
occupancy, credit-stall, FIFO occupancy histograms) behind a three-phase
window. The user needs three things only:

1. Each read/write transaction's completion latency.
2. The path a transaction takes.
3. The no-stall (structural) cycles a flit spends in each component, and from
   it the zero-load latency.

This design delivers those three from one JSON log, drops the rest, and adds a
zero-load latency calculator with two validation checks.

## 2. Terms

- zero-load latency: the end-to-end latency of a transaction when it is alone
  in the network (no contention). Standard term (Dally & Towles).
- component depth: the no-stall structural latency of one component, in cycles
  (the pipeline depth a flit traverses with no backpressure).
- dwell: the cycles a flit actually spends in a component during a run
  (>= depth; the excess is queueing).
- queueing: `measured_latency - zero_load`, the contention delay.

## 3. Scope

In scope:

- Per-transaction measured latency, zero-load latency, queueing, and path.
- Per-component (NMU, NSU, each router) aggregate dwell (min/mean/max) and
  aggregate occupancy (busiest buffer peak + capacity).
- A zero-load calculator and its two validation checks.
- One JSON log plus a stdout summary.

Out of scope (removed from the v1 probe):

- Outstanding-depth, RoB-occupancy as standalone metrics, credit-stall counter,
  per-port/per-VC FIFO histograms, threshold-bin histograms, the three-phase
  PhaseController, and `Stats::variance` output.

## 4. Architecture

The probe is a set of testbench-side observer objects attached beside the DUT.
The DUT (NMU, NSU, Router) is not modified.

Observation surfaces, by boundary type:

- AXI edge (beats): `AxiMaster` issue/completion callbacks, for transaction
  latency. This is the only non-flit boundary.
- NoC flit boundaries (flits): testbench-only link wrappers on every NoC port
  where flits flow -- NMU NoC port, NSU NoC port, and each inter-router link.
  A wrapper is a decorator over the link interface (`NocReqOut` / `NocRspIn` /
  `RouterLink`) that timestamps each flit crossing, then forwards it unchanged.
- Occupancy: const introspection already on the components
  (`Router::input_fifo_size` / `output_fifo_size`, NI queue depths).

This makes the mechanism symmetric: every flit boundary is link-wrapped (NI and
router alike); only the AXI edge uses callbacks, because only the NI converts
between AXI beats and NoC flits.

### Why the NI uses both

The NI spans two protocols: AXI beats on the master side, NoC flits on the
fabric side. To measure a transaction's full journey through the NI, the probe
observes the AXI-side entry (callbacks) and the NoC-side flit boundary (link
wrapper). The router has only flit-to-flit boundaries, so it is purely
link-wrapped.

## 5. Two passes

The probe runs the scenario twice.

Pass 1 -- characterization (no contention): inject one transaction signature at
a time into an otherwise empty network. Each flit sees zero queueing, so its
per-component dwell equals the component depth. This pass yields:

- component depth per (component, direction), feeding the calculator and the
  `hop_latency_cyc.min` field.
- the per-signature zero-load ground truth (end-to-end via callbacks), used to
  validate the calculator (Section 7).

A signature is `(src, dst, type, burst_len)`; zero-load depends only on the
signature, so each distinct signature is characterized once.

Pass 2 -- measurement (the real scenario): run the scenario as authored. This
pass yields:

- measured latency per transaction (callbacks).
- per-component dwell min/mean/max (link wrappers).
- per-component occupancy peak (introspection).
- path per transaction.

## 6. Zero-load calculator

The calculator computes zero-load from the path and the component depths, rather
than re-measuring it per run:

```
zero_load(txn) =
    request leg:   NMU.depth(req) + sum ROUTER.depth(req) over request hops + NSU.depth(req)
  + response leg:  NSU.depth(rsp) + sum ROUTER.depth(rsp) over response hops + NMU.depth(rsp)
  + (num_data_flits - 1)        # serialization, applied once on the multi-beat leg
```

- A transaction's latency spans the request leg (master to slave) then the
  response leg (slave back to master); both legs are summed.
- `depth(req)` and `depth(rsp)` may differ per component (a NMU packetizes on
  the request leg, depacketizes on the response leg).
- `num_data_flits` is the beat count of the data-carrying direction (W beats for
  a write, R beats for a read). One flit per cycle injection makes the
  serialization term `(beats - 1)` cycles, added once.
- Component depths come from Pass 1 (measured, not statically derived from
  code).

The path itself is deterministic under XY routing, so it is derived from
`src`/`dst`, not traced.

## 7. Validation

Two checks gate correctness.

1. Calculator accuracy. For every signature, the calculator output must equal
   the Pass-1 ground truth (the same transaction's end-to-end latency measured
   in isolation): `zero_load_calc(sig) == zero_load_measured(sig)`. The model is
   deterministic, so this is exact equality. A mismatch means the calculator
   omits a latency term (for example an inter-component link cycle) -- fix the
   formula or the depths.

2. Min-latency floor. In Pass 2, for every signature, the minimum measured
   latency must be at least the zero-load latency:
   `min(measured_latency(sig)) >= zero_load(sig)`, equivalently
   `queueing >= 0` for every transaction. A violation means the calculator
   over-estimates or a measurement is wrong.

Both run as test assertions.

## 8. Metrics and JSON

The probe emits one JSON object per run and a one-line-per-component stdout
summary. Occupancy is aggregate: the peak fill of the component's busiest buffer
and that buffer's capacity.

```json
{
  "scenario": "AX4-BAS-003",
  "transactions": [
    { "line": 42, "type": "read", "id": 3, "src": "NMU0", "dst": "NSU1",
      "request_path":  ["NMU0", "R(0,0)", "R(1,0)", "NSU1"],
      "response_path": ["NSU1", "R(1,0)", "R(0,0)", "NMU0"],
      "measured_latency_cyc": 18, "zero_load_cyc": 9, "queueing_cyc": 9 }
  ],
  "ni": {
    "NMU0": { "kind": "nmu",
              "hop_latency_cyc": { "min": 2, "mean": 3.1, "max": 6 },
              "occupancy": { "max": 4, "capacity": 4 } },
    "NSU1": { "kind": "nsu", "hop_latency_cyc": { "min": 2, "mean": 2.4, "max": 5 },
              "occupancy": { "max": 3, "capacity": 32 } }
  },
  "router": {
    "R(0,0)": { "hop_latency_cyc": { "min": 3, "mean": 4.0, "max": 9 },
                "occupancy": { "max": 3, "capacity": 4 } },
    "R(1,0)": { "hop_latency_cyc": { "min": 3, "mean": 3.6, "max": 7 },
                "occupancy": { "max": 2, "capacity": 4 } }
  }
}
```

- `ni` holds both NMU and NSU entries, distinguished by `kind`; `router` holds
  the routers. Two top-level component groups, matching the NI / router split.
- `hop_latency_cyc.min` is the component depth (no-stall); `mean`/`max` show
  contention in Pass 2.
- `_cyc` suffix marks cycle-valued fields.

## 9. Files

New and changed, all testbench-only under `c_model/tests/` (namespace
`ni::cmodel::testing`), none linked into production:

| File | Action |
|---|---|
| `c_model/tests/common/perf_stats.hpp` | keep; emit only count/min/mean/max (variance, histogram unused) |
| `c_model/tests/common/perf_common.hpp` | delete (`PhaseController` unused) |
| `c_model/tests/common/router_perf_observer.hpp` | delete |
| `c_model/tests/common/ni_perf_observer.hpp` | reduce to the transaction-latency collector (drop outstanding, RoB occupancy) |
| `c_model/tests/common/flit_link_probe.hpp` | new: link-wrapper decorator that timestamps flit crossings |
| `c_model/tests/common/component_dwell_observer.hpp` | new: aggregates per-component dwell + occupancy from the link probes and introspection |
| `c_model/tests/common/zero_load_calculator.hpp` | new: path + depths -> zero-load latency |
| `c_model/tests/common/perf_report.hpp` | rewrite to emit the Section 8 JSON + stdout summary |

No production header changes. The router link wrappers attach where
`TwoNodeFabric` wires `set_downstream`; the NI link wrappers attach at the
`NmuStandalone` / `NsuStandalone` NoC accessors.

## 10. Log location

- stdout summary: printed during the test (visible in ctest output).
- JSON: written to a gitignored path, default `build/cmodel/perf/<scenario>.json`,
  overridable by the `NOC_PERF_FILE` environment variable. Generated artifact,
  not committed.

## 11. Testing

- `zero_load_calculator`: unit test on a known path and depth set; asserts the
  formula including the serialization term.
- Calculator accuracy (Section 7.1): integration assertion across all signatures.
- Min-latency floor (Section 7.2): integration assertion across Pass 2.
- Non-intrusive: the observers are read-only (callbacks, forwarding link
  wrappers, const introspection); an A/B run asserts identical cycle-to-
  completion with and without the probe attached.
- `flit_link_probe`: unit test that a wrapped link forwards every flit unchanged
  and records the crossing cycle.

## 12. References

- Dally and Towles, "Principles and Practices of Interconnection Networks" --
  zero-load latency and the `latency = zero_load + queueing` decomposition.
- `docs/superpowers/specs/2026-06-17-ni-router-perf-observer-design.md` -- the
  v1 probe this supersedes.
- `docs/architecture.md` sec. 3.2 -- the tick discipline (1 tick = 1 cycle,
  1-cycle-per-hop) underlying component depth.
