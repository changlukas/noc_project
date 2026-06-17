# NoC performance probe

The performance probe is a testbench-only observer set that reports, from one
JSON log per run, where each AXI transaction's latency is spent in the NoC. It
gives four things: per-transaction completion latency, the path taken, the
per-component latency (how many cycles a flit spends in each NMU, router, and
NSU), and the busiest-buffer occupancy. The only production change is two
additive const getters on `Router`; the DUT timing is untouched.

## 1. The log and the pipeline at a glance

Read the log against the pipeline it measures. The same numbers appear in both.
The decomposition values -- `zero_load_cyc`, the `ni`/`router` latencies, and
`slave.remainder_cyc` -- are the measured isolated AX4-BAS-003 write. The
`measured_latency_cyc`/`queueing_cyc` pair (marked `*`) is illustrative: it shows
a contended run, where the single isolated write has `queueing_cyc = 0`.

```
JSON log (one write transaction + the per-component breakdown)
  transactions:[{ measured_latency_cyc:18*, zero_load_cyc:15, queueing_cyc:3* }]
  ni:     { NMU0:{latency_cyc.min:0},   NSU1:{latency_cyc.min:2} }
  router: { R(0,0):{latency_cyc...},    R(1,0):{latency_cyc...} }
  slave:  { remainder_cyc:6 }      (* illustrative; all other values measured)

Pipeline (one no-stall round trip = zero_load_cyc = 15 cycles)

  AXI AW    ┌──────┐   ┌────────┐   ┌────────┐   ┌──────┐      ┌───────┐
  accept ──►│ NMU0 │──►│ R(0,0) │──►│ R(1,0) │──►│ NSU1 │ ───► │ slave │
  (t=0)     └──────┘   └────────┘   └────────┘   └──────┘      └───────┘
   ▲        latency=0  └─ router latency_cyc ─┘  latency=2     remainder=6
   │       (pull-based)  3-stage registered                   (zero_load−Σlatency)
   │            │              │                   │                 │
   │            └──── ni / router / slave fields of the log ─────────┘
   │   ... B response returns NSU1 ◄ R(1,0) ◄ R(0,0) ◄ NMU0 to AXI (t=15) ...
   └── measured_latency_cyc (t=0 → completion); queueing_cyc = measured − zero_load
```

- `zero_load_cyc` (15) is one no-stall round trip: the sum of every component's
  no-stall latency plus the slave remainder. It is the latency floor.
- `latency_cyc` per component is what that component adds. `min` is the no-stall
  floor; `mean`/`max` grow under contention.
- `slave.remainder_cyc` (6) is `zero_load` minus the measured component
  latencies -- the slave's own processing, which the NoC probe does not break
  down further.
- `measured_latency_cyc` (18*, illustrative) is the Pass-2 latency under
  contention; `queueing_cyc` (3*) = `measured - zero_load` is the contention
  delay on top of the floor. For the isolated write, `measured = zero_load = 15`
  and `queueing = 0`.

The terms: **zero-load latency** is the end-to-end latency of a transaction alone
in the network (Dally & Towles). **Per-component latency** is the cycles a flit
spends in one component. **Queueing** is `measured - zero_load`. **Slave
remainder** is the unattributed tail, owned by the slave.

## 2. Tick model

The c_model advances on a tick: one `tick()` call on a component is one cycle
(`architecture.md` sec. 3.2). The harness holds a cycle counter `now` and stamps
it at each observed event, so a latency is a difference of two `now` values.

Two timing behaviours set the per-component latencies:

- The NMU and NSU sub-stages are **pull-based**. Their sub-modules are ticked
  upstream-first with no inter-stage register (`nmu.hpp` tick order), so a beat
  propagates through the whole NI in one tick: an AW accepted at tick N emits its
  flit on the NoC edge at the same tick N. The NI structural latency is therefore
  ~0 cycles -- a measured NI `latency_cyc.min` of 0 is correct, not a bug.
- The **router is registered**: a 3-stage wormhole-VC pipeline (Section 3). Its
  stages latch state between ticks, so a flit accrues real cycles crossing a
  router. This is where the no-contention NoC latency lives.

So `zero_load` is dominated by the router hops and the slave, with the NI
contributing little. Latency is counted from AW/AR accept at the NMU to the
B / last-R response at the NMU; master-side queueing before accept is excluded.

## 3. Pipeline structure and zero-load decomposition

`zero_load` is the measured isolated latency, not a formula. The probe decomposes
it into per-component latencies measured in the same no-contention run, so the
reader sees the pipeline-stage breakdown. The pipelines each latency measures:

- **NMU** request: `AxiSlavePort -> RoB -> Packetize (AXI->flit) ->
  WormholeArbiter -> VcArbiter -> NoC out`; response is the reverse via
  `Depacketize`.
- **NSU** request: `NoC in -> Depacketize (flit->AXI, meta->MetaBuffer) ->
  AxiMasterPort -> AXI to slave`; response: `AXI from slave -> AxiMasterPort ->
  Packetize -> WormholeArbiter -> VcArbiter -> NoC out`. The NSU has no RoB.
- **Router** (per hop), a registered 3-stage wormhole-VC pipeline:

```
   ┌──────┐   ┌─────────┐   ┌──────────────────┐   ┌────────────┐
   │ land │──►│ IB · RC │──►│   VA · SA · XB   │──►│ OB ──► link│
   └──────┘   └─────────┘   └──────────────────┘   └────────────┘
     in         stage 1           stage 2              stage 3
```

  IB = input FIFO, RC = route compute, VA = VC allocation, SA = switch
  allocation (wormhole lock + VC round-robin), XB = crossbar, OB = output FIFO.
  The stages latch between ticks, so at no contention these registered stages
  set the per-router `latency_cyc.min`.

The per-component latencies are measured at these boundaries:

| Component | latency = | start | end |
|---|---|---|---|
| NMU request | flit-out − AW/AR accept | `AxiMaster` issue callback | wrapped `NocReqOut` crossing |
| Router (per hop) | flit-out − flit-in | inject / link probe in | link / eject probe out |
| NSU request | slave-drive − flit-in | wrapped `NocReqIn` crossing | last request beat to slave (WLAST / AR) |
| response leg | symmetric | NSU / router / NMU response boundaries | |

`slave_remainder = zero_load - sum(component latencies)`. It absorbs the slave's
processing and any latency not attributed to a measured component.

## 4. Measurement

The probe attaches beside the DUT, never inside it. Three observation surfaces:

- **AXI edge** (the only non-flit boundary): the `AxiMaster` issue/completion
  callbacks give transaction latency. The callbacks carry no cycle, so the
  harness stamps `now` when each fires.
- **NoC flit boundaries**: testbench-only decorators wrap the four NI NoC
  interfaces (`NocReqOut`, `NocRspIn`, `NocReqIn`, `NocRspOut`) and each
  inter-router `RouterLink`. A decorator timestamps a flit crossing into a log,
  then forwards the flit unchanged.
- **Occupancy**: const introspection (`Router::input_fifo_size` /
  `output_fifo_size` for the peak, `vc_depth` / `output_fifo_depth` for the
  capacity; the NSU busiest queue size).

The probe runs the scenario twice:

- **Pass 1 -- characterization (no contention)**: one signature at a time in an
  empty network. Yields `zero_load(signature)` (callbacks) and the per-component
  no-stall latencies (boundary crossings). With one signature in flight, FIFO
  order pairs each crossing; no per-flit id is needed.
- **Pass 2 -- measurement (the real scenario)**: yields per-transaction measured
  latency, path, and the per-component occupancy peak.

The probe is non-intrusive: callbacks and const introspection read state; the
decorators forward every flit unchanged. An A/B run asserts identical
cycle-to-completion with and without the probe attached.

## 5. Validation

Two assertions gate correctness:

1. **Decomposition sanity**: `sum(component latencies) <= zero_load`, i.e.
   `slave_remainder >= 0`. A negative remainder means a component latency was
   over-measured (a mis-paired boundary). It runs as a hard assert before the
   subtraction, so it cannot be clamped away.
2. **Min-latency floor**: the Pass-2 minimum measured latency per signature is
   `>= zero_load`, i.e. `queueing >= 0`. The isolated latency is the floor;
   contention only adds cycles.

## 6. JSON fields and reading

One JSON object per run, plus a one-line-per-component stdout summary. Top-level
keys: `scenario`, `transactions[]`, `ni{}`, `router{}`, `slave{}`.

| Field | Meaning |
|---|---|
| `transactions[].measured_latency_cyc` | Pass-2 end-to-end latency (cycles) |
| `transactions[].zero_load_cyc` | no-contention floor for this signature |
| `transactions[].queueing_cyc` | `measured - zero_load` |
| `transactions[].request_path` / `response_path` | component names along the path |
| `ni.<name>.kind` | `nmu` or `nsu` |
| `ni` / `router` `.<name>.latency_cyc` | `{min, mean, max}`; `min` is the no-stall latency |
| `ni` / `router` `.<name>.occupancy` | `{max, capacity}` busiest buffer |
| `slave.remainder_cyc` | `zero_load - sum(component latencies)` |

The JSON is written to a gitignored path (`build/cmodel/perf/<scenario>.json`, or
`NOC_PERF_FILE`); the stdout summary always prints. `_cyc` marks cycle-valued
fields.

## 7. Scope and accuracy

- The path is deterministic under XY routing, derived from `src` / `dst`.
- `zero_load` and the per-component latencies depend only on the signature
  `(src, dst, type, burst_len)`, so each signature is characterized once.
- The decomposition attributes NoC latency to NMU, routers, and NSU; the slave's
  own processing is the remainder, not broken down.
- Out of scope (removed from the earlier probe): outstanding depth, RoB occupancy
  as a standalone metric, credit-stall counter, FIFO histograms, and the
  three-phase measurement window.

## 8. References

- Dally and Towles, *Principles and Practices of Interconnection Networks* --
  zero-load latency and `latency = zero_load + queueing`.
- Kwon et al., "OpenSMART," ISPASS 2017 -- the no-contention per-hop latency
  framing (router + link cycles) and the router-pipeline figure convention.
- `docs/architecture.md` sec. 3.2 -- the tick discipline (1 tick = 1 cycle).
- `docs/superpowers/specs/2026-06-17-perf-probe-simplify-design.md` -- the design
  this probe implements.
