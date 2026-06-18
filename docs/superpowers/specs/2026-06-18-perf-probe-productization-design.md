# Perf probe productization: mesh-agnostic decomposition + scenario-driven

Date: 2026-06-18
Status: Approved (Codex GO, 2026-06-18); ready for implementation plan
Builds on: `2026-06-17-perf-probe-simplify-design.md` (the measured zero-load +
per-component latency probe). That probe is correct but partial: it emits one
lumped router, hard-codes NI occupancy to 0, drives a single hard-coded
transaction, and surfaces the log only in a test's stdout. This design closes
those gaps and makes the decomposition independent of mesh size.

## 1. Goals

1. **Mesh-agnostic decomposition**: the per-component latency method must hold for
   any mesh size. The current 2-node fabric is one instance (a 1-hop path); the
   method must not hard-code node or router counts.
2. **Complete, honest content**: emit every router on a transaction's path, wire
   NI occupancy to real getters (never print a placeholder zero as a measurement),
   add run metadata, and align the stdout and JSON field names.
3. **Scenario-driven artifact**: drive any scenario YAML (all its transactions),
   and produce a per-scenario JSON via a `make perf` target, not a buried test
   stdout.

Non-goal: building an N×M fabric. The 2-node fabric stays the only instance for
now; this design makes the *method* and *code* mesh-agnostic so an N×M fabric
plugs in later with no perf-probe change.

## 2. Mesh-agnostic path-driven decomposition

The decomposition is driven by each transaction's router path, derived from the
mesh, not from a fixed topology.

### 2.1 Router path

`router_path(src_id, dst_id, mesh_x, mesh_y)` returns the ordered node-coordinate
sequence a flit visits under XY routing: step X to the destination column, then Y.
For an `(X, Y)` mesh, node id encodes `(x, y)` as `x | (y << X_WIDTH)`.

- Request path: `src -> ... -> dst`. Response path: the reverse.
- The sequence has `k+1` nodes for a `k`-hop path; each node hosts one router.
- For 2-node `AX4-BAS-003` driven as a `node 0 -> node 1` flow: `[(0,0), (1,0)]`,
  a 1-hop path, two routers.

Note on the flow destination: a scenario's native addresses are local (under
`xy_route`, `dst_id = (addr >> 32) & mask`, and native addresses keep the high
bits clear, so they route to `dst 0`). To exercise the network, the harness runs
each scenario as one canonical non-local flow (`node 0 -> node 1` on the 2-node
fabric) by remapping the destination address bits. This is a single fixed flow
per scenario, not per-transaction destination routing; routing each transaction
to a varying destination is a future traffic-pattern concern. The remap is done
by the lossless writer (Section 5.1), which sets the destination while preserving
every other field.

This is the only place mesh dimensions enter. Everything downstream consumes the
path.

### 2.2 Per-hop direction

For a hop `coord_i -> coord_{i+1}`, the output port is derived from the delta:
`+x -> EAST, -x -> WEST, +y -> NORTH, -y -> SOUTH`. The downstream router receives
on the opposite port. XY routing changes X before Y, so each hop is a single axis
step (one unambiguous direction).

### 2.3 Boundary instrumentation (generic over the path)

For a transaction's path `[r_0, ..., r_k]`, the probe wraps these boundaries in
order:

- NI source edge: the NMU NoC-out at `r_0` (`NocReqOut` decorator).
- Each inter-router link `r_i -> r_{i+1}`: re-route `router(r_i).output(dir_i)`
  through a `LinkProbe` wrapping `router(r_{i+1}).input(opposite(dir_i))`
  (`Router::set_downstream` overwrites the downstream pointer, so this needs no
  fabric change).
- NI sink edge: the NSU NoC-in at `r_k` (`NocReqIn` decorator).

The response leg wraps the reverse path symmetrically. The harness loops over the
path; it never names a specific router. For the 2-node instance this reduces to
exactly the current `R(0,0)`/`R(1,0)` + one inter-router link.

Boundary conventions (state explicitly so the segments are unambiguous):

- The NI source edge crossing is the flit entering router `r_0`'s LOCAL input via
  the inject adapter (`InjectAdapter::push_flit`), i.e. the wrapped `NocReqOut`
  push. The NI sink edge crossing is the flit leaving router `r_k`'s LOCAL output
  to the eject adapter, i.e. the wrapped `NocReqIn` pop.
- A `LinkProbe` is data-path-only: it timestamps then forwards `push_flit`
  unchanged. The credit path is wired separately at fabric construction and must
  stay untouched, so the probe does not alter backpressure or timing.

### 2.4 Per-router latency

The wrapped boundaries produce an ordered crossing log per leg:
`[C_0 = NI-out, C_1 = link_0, ..., C_k = link_{k-1}, C_{k+1} = NI-in]` — `k+2`
crossings, `k+1` segments. Segment `i` is router `r_i`'s latency:

```
latency(r_i) = C_{i+1}.cycle - C_i.cycle      (flit enters r_i at C_i, leaves at C_{i+1})
```

Pairing consecutive crossings yields each router's latency, for any path length.
NI latency uses the AXI callback (accept) to the NI NoC-edge crossing, as in the
base design (Section 6, `2026-06-17-...`). The pull-based NI gives ~0; the
registered routers carry the latency.

### 2.5 Decomposition and zero-load

`zero_load(signature)` stays the isolated measured end-to-end latency. The
decomposition sums over the path:

```
zero_load = NMU + sum_{i in routers(path)} latency(r_i) + NSU      (request leg)
          + NSU + sum_{i in routers(rsp path)} latency(r_i) + NMU  (response leg)
          + slave_remainder
```

`slave_remainder = zero_load - sum(all measured component latencies)`, `>= 0`
(Section 6). No router count is baked in; the sum ranges over the actual routers
on each leg's path.

## 3. Fabric interface requirement

The harness needs, from whatever fabric it runs on:

- `router_at(x, y)` for request and response networks (by coordinate).
- The existing `Router::set_downstream(port, RouterLink&)` and `input(port)` for
  link wrapping.
- The NI NoC interfaces per node, for the NI-edge decorators.

The 2-node fabric exposes `req_router(node)` / `rsp_router(node)` for nodes 0 and
1; the harness maps `(x, y)` to a node id and uses these. An N×M fabric provides
the same accessors over the grid. The perf code depends only on this interface,
so it is unchanged when the fabric grows.

Add `req_router_at(x, y)` and `rsp_router_at(x, y)` accessors to `TwoNodeFabric`
now (each maps `(x, y)` to the node id and returns the existing per-node router).
The harness calls only the by-coordinate accessors, so it is already
mesh-agnostic; an N×M fabric implements the same two accessors. The request and
response networks are separate, so both accessors are required.

## 4. Content completeness

| Gap (current) | Fix |
|---|---|
| One lumped router (`R(1,0)` only) | Emit every router on the request and response paths, with its own `latency_cyc`. |
| NI occupancy hard-coded `0/0` | Wire NMU `Rob::write_occupancy`/`read_occupancy` (peak sampled per tick) + `ROB_CAPACITY`; NSU busiest `AxiMasterPort::*_q_size()` + `params()` depth. If a component's occupancy is genuinely unavailable, emit `n/a`, never `0` as a measurement. |
| No run metadata | Add a header to stdout and a top-level JSON block: `scenario`, mesh dims, VC count, total cycles, transaction count, and the JSON artifact path. |
| stdout vs JSON naming differs | Align: stdout uses the JSON field names (`latency_cyc`, `occupancy.max/capacity`) as a labelled lossy view, documented as the human summary. |
| Redundant `kind=` in stdout | Drop from the stdout line (the `NMU`/`NSU`/`R(x,y)` name prefix encodes it); keep `kind` in JSON only where the name does not disambiguate. |
| No path/component cross-check | Assert every router in a transaction's `request_path`/`response_path` has a component entry. |

## 5. Scenario-driven harness and `make perf`

### 5.1 Driving any scenario

A scenario is one `src -> dst` flow (the destination is whatever the transaction
addresses route to; the harness does not manage destination selection -- that is a
future traffic-pattern concern). The decomposition is per-flow: it takes the
flow's path from the mesh and holds regardless of how the traffic pattern grows.
Generalize the harness from one hard-coded write to any scenario YAML:

- Load the scenario (`axi::load_scenario`, already used). Drive all its
  transactions (read/write, multi-beat burst) through the one `src -> dst` flow.
- Pass 1: characterize each distinct signature in isolation -> `zero_load` +
  per-component latency per signature. The signature key is the full transaction
  shape: `(op, src, dst, len, size, burst, mem_latency_class)` -- latency varies
  with `size`, 4 KB split, and the memory model, so a coarse `(src,dst,type,len)`
  key would alias distinct transactions.
- Pass 1 isolation must preserve the transaction faithfully. `shifted_scenario_path`
  drops `strb_file`, `lock`, `qos`, `inject`, and max-outstanding fields, so it
  cannot characterize an arbitrary transaction. Use a lossless single-transaction
  writer (`ScenarioTransaction` / `ScenarioConfig` carry every field via
  `axi::load_scenario`) that copies all of them and applies the destination remap
  (Section 2.1) to establish the `node 0 -> node 1` flow.
- Pass 2: run the full scenario -> per-transaction measured latency (from the AXI
  callbacks, keyed by `scenario_line`), path, and per-component occupancy peak. One
  `transactions[]` row per real transaction.
- Aggregate per-component `latency_cyc{min,mean,max}` across all transactions that
  traverse that component.

Two passes are retained deliberately. The no-stall structural latency (`zero_load`,
the `min`) and the `queueing = measured - zero_load` split require an isolated
no-contention reference; a single pass cannot separate structural latency from
contention. The added cost is bounded by caching one characterization per distinct
signature. The two-pass harness already exists and passes; this design moves it to
a dedicated file and generalizes it, rather than re-deriving a single-pass scheme.

### 5.2 Coverage

All 37 scenarios are 2-node single-flow traffic patterns (the real co-sim is
2-node; under real `xy_route` a scenario's native addresses route to one
destination -- the `multi_dst` naming is about multiple transactions, not
multiple physical nodes). So each scenario drives one `src -> dst` flow on the
existing fabric; no N×M fabric is needed for coverage.

A small set of scenarios are error-injection / expected-fail (`AX4-INF-001` aborts
on init by design). The AXI integration test skips these with an inline prefix
check (`if scenario_id starts with "AX4-INF-" -> GTEST_SKIP`), not a shared skip
list. The perf harness defines its own explicit `expected_fail` set (the INF
prefix plus any other error-injection ids) and `make perf` records those as
`skipped`, never instantiating the probe against them.

### 5.3 `make perf`

```
make perf                      # all scenarios -> build/cmodel/perf/<id>.json each
make perf SCENARIO=AX4-BAS-003 # one scenario
```

Mirrors the `test` target (`build-cmodel` dep, `TOOLPATH`, `TEST_TMPDIR`,
`PYTHON3`). The ctest body runs from `build/cmodel`, and `emit()`'s default path
is CWD-relative, so the target must set an **absolute** `NOC_PERF_FILE` per run
(`$(abspath $(CMODEL_BUILD))/perf/<id>.json`) and create the directory first.
`emit()` prints the path it wrote. Artifacts land under `build/cmodel/perf/`,
mirroring `cosim/<sim>/output/<scenario>/run.log`.

## 6. Validation

Unchanged in intent, generalized to any path:

1. Decomposition sanity: `sum(component latencies) <= zero_load`
   (`slave_remainder >= 0`), a hard assert before the subtraction.
2. Min-latency floor: Pass-2 `min(measured) >= zero_load` per signature.
3. Path/component cross-check (new): every router on a path has a component entry
   (catches a path the instrumentation missed).
4. Non-intrusive A/B: identical cycle-to-completion with and without the probe.

## 7. Files

| File | Action |
|---|---|
| `c_model/tests/common/router_path.hpp` | new: `router_path(src,dst,mx,my)` + per-hop direction. Mesh-parameterized (replaces the deleted `xy_path`). |
| `c_model/tests/common/isolated_scenario.hpp` | new: lossless single-transaction scenario writer (copies every transaction + config field) for Pass-1 characterization. |
| `c_model/tests/common/perf_report.hpp` | add run-metadata block; align stdout names; drop redundant `kind=`; support `n/a` occupancy. |
| `c_model/tests/integration/test_perf_probe.cpp` | new dedicated perf harness: path-driven instrumentation loop, scenario-driven multi-transaction Pass 1/2, per-router + NI-occupancy population, path/component assert, expected-fail handling. `test_router_loopback.cpp` keeps only its loopback correctness + A/B. |
| `c_model/tests/noc/two_node_fabric.hpp` | add `req_router_at(x,y)` / `rsp_router_at(x,y)` accessors. |
| `c_model/tests/integration/test_perf_probe.cpp` (the harness) | defines its own explicit `expected_fail` set (INF prefix + any error-injection ids); no shared AXI skip list exists to reuse. |
| root `Makefile` | `perf` target (`make perf [SCENARIO=<id>]`), absolute `NOC_PERF_FILE`. |

## 8. Resolved decisions (were open questions)

- Coordinate access: add `req_router_at`/`rsp_router_at` to `TwoNodeFabric` now;
  the harness calls only these (Section 3).
- Harness location: a dedicated `test_perf_probe.cpp`; `test_router_loopback.cpp`
  already carries correctness + A/B + Pass 1/2 and would become unmaintainable.
- Pass-1 signature key: the full transaction shape
  `(op, src, dst, len, size, burst, mem_latency_class)`; characterize once per
  distinct signature, cache by that key (Section 5.1).
