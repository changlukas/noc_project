# Perf probe simplification: zero-load (measured) + per-component pipeline dwell

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

This design delivers those three from one JSON log and drops the rest. Zero-load
is the isolated measured latency; the probe decomposes it into per-component
pipeline dwells (with the slave as a remainder), under two validation checks.

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

- Per-transaction measured latency, zero-load (isolated) latency, queueing, path.
- Per-component (NMU, NSU, each router) measured pipeline dwell (min/mean/max) and
  aggregate occupancy (busiest buffer peak + capacity), plus a slave remainder.
- Two validation checks (decomposition sanity + min-latency floor).
- One JSON log plus a stdout summary.

Out of scope (removed from the v1 probe):

- Outstanding-depth, RoB-occupancy as standalone metrics, credit-stall counter,
  per-port/per-VC FIFO histograms, threshold-bin histograms, the three-phase
  PhaseController, and `Stats::variance` output.

## 4. Architecture

The probe is a set of testbench-side observer objects attached beside the DUT.
The DUT's behaviour is unchanged; the only production additions are additive,
const-only introspection getters (Section 9) that expose configuration constants
already held internally, so they cannot alter timing or state.

Observation surfaces, by boundary type:

- AXI edge (beats): `AxiMaster` issue/completion callbacks, for transaction
  latency. This is the only non-flit boundary.
- NoC flit boundaries (flits): testbench-only decorators over the flit interfaces
  that the perf testbench wires between the NI and the fabric. The perf testbench
  uses the full `Nmu` / `Nsu` (constructed with injected NoC interfaces), not the
  `*Standalone` pop/inject accessors, so all four NI NoC interfaces are
  wrappable -- `NocReqOut` (NMU request out), `NocRspIn` (NMU response in),
  `NocReqIn` (NSU request in), `NocRspOut` (NSU response out) -- plus each
  inter-router `RouterLink` (attached at `TwoNodeFabric::set_downstream`). A
  decorator timestamps each flit crossing, then forwards it unchanged.
- Occupancy: const introspection. `Router::input_fifo_size` / `output_fifo_size`
  exist; the capacities (`vc_depth`, `output_fifo_depth`) and the NSU
  busiest-queue occupancy are not yet exposed and are added as additive const
  getters (Section 9), in the same pattern as the v1 `Rob`/`Router` getters.

This makes the mechanism symmetric: every flit boundary is decorator-wrapped (NI
and router alike); only the AXI edge uses callbacks, because only the NI converts
between AXI beats and NoC flits.

Flit pairing: a flit's entry and exit at a boundary are matched by per-link,
per-VC FIFO order (the links and FIFOs preserve order, so the Nth flit in is the
Nth flit out). Per-component dwell is measured only in Pass 1 with one signature
in flight, so FIFO order alone pairs each crossing unambiguously; no per-flit
transaction id is needed.

### Why the NI uses both

The NI spans two protocols: AXI beats on the master side, NoC flits on the
fabric side. To measure a transaction's full journey through the NI, the probe
observes the AXI-side entry (callbacks) and the NoC-side flit boundary (link
wrapper). The router has only flit-to-flit boundaries, so it is purely
link-wrapped.

## 5. Two passes

The probe runs the scenario twice.

Timing boundary: latency is counted from when the NMU accepts the AXI request
(AW/AR accept) to when the NMU returns the AXI response (B / last R). Master-side
queueing before AW/AR accept is excluded.

Pass 1 -- characterization (no contention): inject one signature at a time into
an otherwise empty network, so every flit sees zero queueing. This pass yields:

- `zero_load(signature)` = the isolated end-to-end latency over the boundary
  above, taken directly from the `AxiMaster` issue/completion callbacks. The
  callbacks carry no cycle field, so the harness stamps the current cycle (`now`)
  when each fires.
- the per-component no-stall pipeline dwell (Section 6): NMU, each router, and
  NSU, from the boundary-crossing timestamps collected this pass.

A signature is `(src, dst, type, burst_len)`; zero-load and the dwells depend
only on the signature, so each distinct signature is characterized once.

Pass 2 -- measurement (the real scenario): run the scenario as authored. This
pass yields per transaction: measured latency (callbacks), path, and the
per-component occupancy peak (introspection).

## 6. Zero-load and per-component dwell decomposition

`zero_load` is the isolated measured latency (Section 5), not a formula. The
probe additionally decomposes it into per-component no-stall pipeline dwells, so
a reader sees where the latency goes -- a pipeline-stage view. The pipeline
structure each dwell measures is in Section 6.1.

Per-component dwell (measured in Pass 1, no contention):

- NMU request dwell = (cycle the first request flit leaves the NMU NoC edge) -
  (AW/AR accept cycle). The start is the `AxiMaster` issue callback; the end is
  the wrapped `NocReqOut` crossing. This dwell includes the NMU's AXI-to-flit
  conversion -- the AXI side is not split out, it is the NMU pipeline's first
  stage.
- Router dwell per hop = (flit leaves the router) - (flit enters the router),
  from the inject / inter-router-link / eject `LinkProbe`s.
- NSU request dwell = (cycle the last request beat is driven to the slave --
  WLAST for a write, AR for a read) - (cycle the request flit enters the NSU).
  The slave-drive cycle is observed at the harness shuttle point where the NSU
  AXI master port hands the beat to the slave (`pop_aw`/`pop_w`/`pop_ar` ->
  `slave.push_*`); no AxiMasterPort change is needed.
- The response leg measures the symmetric dwells (NSU response, router, NMU
  response) the same way.

slave_remainder = `zero_load` - sum(all measured component dwells). It is shown
as a `slave` entry. It absorbs the slave's own processing plus any latency not
attributed to a measured component. By construction it cannot be the source of
its own validation (Section 7), so the validation checks `slave_remainder >= 0`
rather than an equality.

### 6.1 Pipeline structure (for the doc and to ground the dwells)

The dwells measure these real pipelines (from the c_model):

- NMU request: `AxiSlavePort -> RoB -> Packetize (AXI->flit) -> WormholeArbiter
  -> VcArbiter -> NoC out`. NMU response is the reverse via `Depacketize`.
- NSU request: `NoC in -> Depacketize (flit->AXI, meta->MetaBuffer) ->
  AxiMasterPort -> AXI to slave`. NSU response: `AXI from slave -> AxiMasterPort
  -> Packetize -> WormholeArbiter -> VcArbiter -> NoC out`. (NSU has no RoB.)
- Router (per hop, 3-stage wormhole-VC): `landing reg -> stage1 input FIFO +
  route-compute -> stage2 VC alloc + switch alloc (wormhole lock + VC RR) +
  crossbar -> stage3 output FIFO -> link`.

The NMU and NSU sub-stages are pull-based: in one `tick()` a beat propagates
through all of them (ticked upstream-first, no inter-stage register;
`nmu.hpp:135`), so the NI adds ~0 structural cycles -- an AW accepted at tick N
emits its flit on the NoC edge at the same tick N. The registered latency lives
in the router 3-stage pipeline (one cycle per hop) and in the slave. So
`zero_load` is dominated by the router hops and the slave remainder, and a
measured NI dwell near zero is the correct model behaviour, not a measurement
error.

## 7. Validation

Two checks gate correctness.

1. Decomposition sanity. The sum of the measured per-component dwells must not
   exceed the isolated latency: `sum(component dwells) <= zero_load(sig)`,
   equivalently `slave_remainder >= 0`. A negative remainder means a component
   dwell was over-measured (for example a mis-paired boundary, or a first/last
   flit boundary that double-counts). The model is deterministic, so under a
   consistent boundary convention the components cannot take longer than the
   whole.

2. Min-latency floor. In Pass 2, for every signature, the minimum measured
   latency must be at least the zero-load latency:
   `min(measured_latency(sig)) >= zero_load(sig)`, equivalently `queueing >= 0`
   for every transaction. The isolated latency is the no-contention floor, so
   contention can only add cycles. A violation means a signature mismatch or a
   timing-convention error.

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
  },
  "slave": { "remainder_cyc": 1 }
}
```

- `ni` holds both NMU and NSU entries, distinguished by `kind`; `router` holds
  the routers; `slave` holds the remainder. Together with `transactions[]` these
  give the pipeline-stage breakdown of `zero_load`.
- `hop_latency_cyc.min` is the no-stall component dwell measured in Pass 1; the NI
  dwell is measured from the AXI accept callback to the NoC-edge flit crossing
  (Section 6); `mean`/`max` show contention in Pass 2.
- `slave.remainder_cyc` = `zero_load` minus the summed component dwells (Section
  6); it must be `>= 0` (Section 7.1).
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
| `c_model/tests/common/flit_link_probe.hpp` | new: decorators that timestamp flit crossings into a `FlitLog` |
| `c_model/tests/common/component_dwell_observer.hpp` | new: per-component dwell (`SegmentDwell`, FIFO-order pairing) + occupancy peak. Router dwell from the inject/link/eject probes; NI dwell is computed in the harness from the AXI callback (accept) to the NoC-edge crossing; NSU request dwell ends at the harness slave-drive shuttle point. |
| `c_model/tests/common/perf_report.hpp` | rewrite to emit the Section 8 JSON (incl. `slave` remainder) + stdout summary |
| `c_model/tests/integration/test_router_loopback.cpp` | the two-pass harness + the Section 7 asserts + the non-intrusive A/B. Builds the fixed 2-node path list inline. |

Occupancy reuses existing getters, so the per-component occupancy needs no new
state accessor:

- NMU occupancy: the `Rob` occupancy getter added in v1.
- NSU occupancy: the busiest of the already-exposed `AxiMasterPort` queue sizes.
- Router occupancy: the existing `input_fifo_size` / `output_fifo_size`.

The only new production code is two router config-constant accessors for the
`capacity` field (additive, const, behaviour-neutral):

| File | Getter |
|---|---|
| `c_model/include/noc/router.hpp` | `vc_depth()`, `output_fifo_depth()` -- return the configured capacities (constants already in `RouterConfig`) |

Router decorators attach by re-routing the inter-router links through a
`LinkProbe` (`Router::set_downstream` overwrites the downstream pointer, so this
needs no fabric change). NI decorators wrap the four NoC interfaces; for them to
sit in the flit path, the `Flow` test helper's constructor is extended to accept
the four NI-edge interfaces (`NocReqOut` / `NocRspIn` / `NocReqIn` / `NocRspOut`)
to bind `Nmu` / `Nsu` against, so the perf testbench passes probe-wrapped
interfaces. The existing callers pass the raw `TwoNodeFabric` edges unchanged.

Coordinated changeset. Deleting `perf_common.hpp` and `router_perf_observer.hpp`,
shrinking `NIPerfObserver`, and rewriting `PerfReport` all break each other's
callers and the tests `test_perf_phase` / `test_router_perf_observer`. The
implementation plan applies these as one changeset and updates
`c_model/tests/common/CMakeLists.txt` (remove the two dead test targets, add the
new ones) in the same step.

## 10. Log location

- stdout summary: printed during the test (visible in ctest output).
- JSON: written to a gitignored path, default `build/cmodel/perf/<scenario>.json`,
  overridable by the `NOC_PERF_FILE` environment variable. Generated artifact,
  not committed.

## 11. Testing

- Decomposition sanity (Section 7.1): integration assertion that
  `slave_remainder >= 0` (component dwells do not exceed the isolated latency)
  for the characterized signatures.
- Min-latency floor (Section 7.2): integration assertion that the Pass-2 minimum
  measured latency per signature is `>= zero_load` (queueing >= 0).
- Non-intrusive: the observers are read-only (callbacks, forwarding decorators,
  const introspection); an A/B run asserts identical cycle-to-completion with and
  without the probe attached.
- `flit_link_probe`: unit test that a wrapped interface forwards every flit
  unchanged and records the crossing cycle.
- `component_dwell_observer`: unit test of the FIFO-order pairing and
  per-component dwell accumulation.

## 12. References

- Dally and Towles, "Principles and Practices of Interconnection Networks" --
  zero-load latency and the `latency = zero_load + queueing` decomposition.
- `docs/superpowers/specs/2026-06-17-ni-router-perf-observer-design.md` -- the
  v1 probe this supersedes.
- `docs/architecture.md` sec. 3.2 -- the tick discipline (1 tick = 1 cycle,
  1-cycle-per-hop) underlying component depth.
