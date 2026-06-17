# NI + Router Performance Observer Design

Date: 2026-06-17
Status: Draft (pending Codex review + user review)
Scope: two testbench-only, non-intrusive performance observers for the c_model
NoC -- `NIPerfObserver` (NI-edge latency / outstanding / RoB occupancy) and
`RouterPerfObserver` (credit-stall / FIFO occupancy / output-port congestion) --
plus shared `Stats` / `PhaseController` helpers and a `PerfReport` writer.
Telemetry only; no CSR model.

## 1. Motivation

The c_model now drives a real wormhole VC router fabric
(`noc/router.hpp`, `RouterChannel`) on the `tb_top` path, so latency,
congestion, occupancy, and credit-stall are physically meaningful (router-cycle
level), not stub artifacts. We want architectural performance telemetry at two
layers without perturbing DUT timing.

Survey basis (not a committed doc): the Arteris/Qualcomm latency-probe patent
EP2724234A2 histogram model, Goossens/Ciordas event-based NoC monitoring, and
BookSim2 measurement methodology. This design ports BookSim2's three-phase
measurement, tail-retirement latency sampling, and `Stats` histogram form.

## 2. Tap layers

```
AxiMaster --[1 BFM end-to-end]--> NMU --[2 NI edge]--> Router fabric --> NSU --[2]--> AxiSlave
                                       (NIPerfObserver)   (RouterPerfObserver, layer 3)
```

- Layer 2 (NI edge): NMU master-facing `axi_slave_port` AW/AR accept + B/R emit;
  RoB introspection. Measured by `NIPerfObserver`.
- Layer 3 (fabric): `Router` credit / FIFO state. Measured by
  `RouterPerfObserver`.
- Layer 1 (BFM end-to-end, master-side queueing included): out of scope this
  round.

In the loopback testbenches the `AxiMaster` is bound directly to the NMU
`axi_slave_port`, so an AW accepted by the NMU is the NI-edge ingress and a B
emitted by the NMU is the NI-edge egress. `NIPerfObserver` latency is therefore
NI-edge latency, not BFM end-to-end latency.

## 3. Components

| Component | Purpose | Depends on |
|---|---|---|
| `Stats` | one metric's accumulator: count/sum/sumsq/min/max + optional threshold-bin histogram | none |
| `PhaseController` | warmup / measurement / drain gating | cycle ref |
| `NIPerfObserver` (per NMU) | NI-edge latency, NI outstanding, RoB occupancy, per-ID | cycle ref, `PhaseController`, `AxiMaster` callbacks, `Nmu` introspection |
| `RouterPerfObserver` | credit-stall, FIFO occupancy, output-port occupancy | cycle ref, `PhaseController`, `Router` introspection |
| `PerfReport` | aggregate all observers -> stdout summary + env-gated JSON | the observers |

All live in `c_model/tests/common/` under `ni::cmodel::testing` (same home and
namespace as `AxiMasterObserver`). None are linked into production builds.
`PhaseController` and the small free helpers share one `perf_common.hpp` (not a
separate class per concept -- v1 keeps the surface small).

### 3.1 Cycle source

No separate clock class (revised after review: a standalone `CycleClock` is
over-split for v1 and was not wired to the existing harness, which already owns
an `int cycle` loop counter). The harness owns a single `uint64_t cycle`,
incremented once per simulated cycle at loop top (cycle start). Every observer
holds a `const uint64_t& now_` bound to it; callbacks and `sample()` read
`now_`. The convention is fixed: `now_` is the cycle-start value for the whole
iteration, so an issue callback and a same-cycle sample see the same number. A
+/-1 skew vs the component tick is acceptable for histogram-grade telemetry.

### 3.2 Stats (BookSim2 port)

```cpp
struct StatsConfig {
    std::vector<uint64_t> bin_thresholds;  // ascending; bin i = [t[i-1], t[i])
};                                          // last bin = [t.back(), +inf)

class Stats {
  public:
    explicit Stats(StatsConfig cfg);
    void add(uint64_t value);   // updates count/sum/sumsq/min/max + histogram bin
    uint64_t count() const; double mean() const; double variance() const;
    uint64_t min() const; uint64_t max() const;
    const std::vector<uint64_t>& histogram() const;
};
```

Thresholds are configurable (parameterized design; no hardcoded bin edges).
Default latency thresholds and occupancy thresholds are supplied by the
observer configs, not baked into `Stats`.

### 3.3 PhaseController

```cpp
enum class Phase { Warmup, Measurement, Drain };

struct PhaseConfig { uint64_t warmup_cycles = 0; };

class PhaseController {
  public:
    PhaseController(const uint64_t& now, PhaseConfig cfg);
    void begin_drain();          // harness calls when all masters report done()
    Phase phase() const;         // Warmup if now<warmup_cycles; Drain if begin_drain seen; else Measurement
};
```

Recording rules (BookSim2 semantics), made explicit after review (H4):

- Outstanding counters update for *every* transaction (+1 on issue, -1 on
  completion) regardless of phase, so the count can never go negative when a
  Warmup-issued transaction completes in Drain.
- Latency samples are gated by a per-transaction `eligible_for_latency` flag,
  set at issue iff `phase()==Measurement`. Completion records a latency sample
  only when the flag is set; its completion cycle may fall in Drain. Each
  in-flight transaction record carries `{issue_cycle, eligible_for_latency}`.
- Per-cycle sampled metrics (occupancy, credit-stall, output-port occupancy):
  recorded only while `phase()==Measurement`.

For finite AXI scenarios the default `warmup_cycles=0` makes Measurement begin
at cycle 0; Drain begins when injection completes. The three-phase machinery is
present and exercised, and degrades to "measure the whole run" for finite
workloads.

## 4. Metrics and derivation

### 4.1 NIPerfObserver

One `NIPerfObserver` instance per NMU (per flow). This is the fix for the H2
key-collision finding: `scenario_line` is unique only *within* one master's
scenario, and the two loopback masters share a shifted copy of the same
scenario, so a global key would collide. Scoping one observer to one NMU's
issue/complete stream makes `scenario_line` a sufficient correlation key with no
global tx-id.

| Metric | Derivation | Recording |
|---|---|---|
| NI-edge write latency | `now(on B emit) - issue_cycle(on AW accept)`, keyed by scenario_line | `Stats`, per direction |
| NI-edge read latency | `now(on R last beat) - issue_cycle(on AR accept)` | `Stats` |
| NI outstanding (write/read) | running count, +1 on issue / -1 on completion, *all* transactions | peak + per-cycle `Stats` occupancy |
| Per-ID outstanding | per-id running count from the issue/complete stream | peak per id |
| RoB occupancy | poll `Nmu::rob().occupancy()` each cycle | `Stats` occupancy |

Correlation key is `scenario_line` within this observer's NMU. The observer
holds `map<scenario_line, {issue_cycle, eligible_for_latency}>`; completion looks
it up, computes latency (if eligible), erases. A transaction splits into one or
more sub-bursts (`axi_master.hpp`); the issue timestamp is the accept of the
*first* sub-burst's AW/AR, the completion is the final B / R-last of the logical
transaction.

Event sources: `AxiMaster::on_write_issued` / `on_read_issued` (new, see Sec. 5)
fire at AW/AR accept; `on_write_completed` / `on_read_observed` (existing) fire
at B / R-last. RoB occupancy is polled, so `NIPerfObserver::sample()` is called
once per cycle by the harness, after the component ticks.

### 4.2 RouterPerfObserver

Polls each observed `Router` once per cycle via `sample()`, called *after* the
router tick so the input FIFOs (not the stage-1 `landing_` register) hold the
flits being arbitrated this cycle:

| Metric | Derivation (per output port `p`, vc `v`) | Recording |
|---|---|---|
| Credit-stall cycles | cycles where an input FIFO front flit is routed to `p`/`v` AND `credit(p,v)==0` | counter + per-port `Stats` |
| Per-VC credit balance | `credit(p,v)` snapshot | `Stats` occupancy |
| Input FIFO occupancy | `input_fifo_size(port,vc)` | `Stats` occupancy |
| Output FIFO occupancy | `output_fifo_size(port)` | `Stats` occupancy |
| Output-port non-empty ratio | cycles `output_fifo_size(p)>0` / measurement cycles | ratio (occupancy proxy, not throughput) |

Credit-stall needs the front flit's routed output port, which is *not* derivable
from the existing public accessors (`input_fifo_size` returns a count only;
`input_fifo_` / `landing_` are private). This was the H3 finding -- the prior
"Router needs no change" claim was wrong. Resolution: add one const
test-introspection accessor to `Router` (see Sec. 5) that reports, per
`(in_port, vc)`, the front flit's `route_compute` output port (or
`std::nullopt` when empty), mirroring stage-2's eligibility check without side
effects.

Deeper contention (wormhole-lock loss, arbitration-loss attribution) is deferred
-- it needs event hooks inside `Router::tick`, a larger production change.

## 5. Production changes

Minimal, in-pattern, additive only:

1. **Multi-subscriber callbacks (H1 fix).** The existing `on_write_completed` /
   `on_read_observed` are single `std::function` slots (`axi_master.hpp:308-309,
   498-499`); the loopback already binds the scoreboard there, so a second
   `on_*` call would silently overwrite it. Convert the four callbacks to
   *append* semantics (each holds a `std::vector<std::function<...>>` and fans
   out in registration order). Audit confirms current callers register exactly
   once, so append is behaviour-preserving. The observer then coexists with the
   scoreboard without the harness having to hand-compose a combined lambda.

2. **`AxiMaster::on_write_issued(cb)` / `on_read_issued(cb)`** -- symmetric with
   the completion callbacks. Each fires immediately after the downstream port
   accepts the AW / AR (i.e. right after `slave_.push_aw(...)` / `push_ar(...)`
   returns true, `axi_master.hpp:435-436`), passing
   `{id, scenario_line, addr, size, len, burst}`. For a multi-sub-burst
   transaction the callback fires once, on the first sub-burst's AW/AR accept.

3. **`Rob::occupancy() const`** -- a const introspection accessor following the
   existing "Test introspection (optional getters; add only as test code needs)"
   note in `nmu.hpp`. `Nmu::rob()` already returns `const Rob&`; `Rob` currently
   exposes no depth getter (`rob.hpp`), so this is new (~3 lines).

4. **`Router` front-flit route accessor (H3 fix)** -- a const method, e.g.
   `std::optional<RouterPort> front_route(std::size_t in_port, uint8_t vc) const`,
   that returns `route_compute(front.dst_id)` for a non-empty `(in_port, vc)`
   input FIFO else `std::nullopt`. Pure read of existing private state, no side
   effects; joins the existing `credit()` / `input_fifo_size()` /
   `output_fifo_size()` introspection set.

All four are additive and const/append-only; no existing behaviour changes.

## 6. Output

Two-tier, gated like the existing `NOC_LOG`. The cheap scalars are always
collected; the full threshold-bin histograms are collected and emitted only
under `NOC_PERF=1` (review YAGNI note: histograms are over-weight for the small
finite loopback scenarios, so they stay behind the guard).

- Always: one-line stdout summary per observer at end of run, using only
  scalars (count / min / mean / max / peak -- no histogram), e.g.
  `[perf:NI] wr_lat(min/mean/max)=... rd_lat=... outstanding_peak=... rob_occ_peak=...`
  `[perf:ROUTER] credit_stall_cycles=... out_nonempty_ratio(p0..p4)=... in_fifo_peak=...`
- `NOC_PERF=1`: `Stats` additionally maintains the threshold-bin histogram and a
  structured JSON file is dumped (one object per run). p99 is read off the
  histogram (first bin whose cumulative count crosses 99%) and so appears only
  in the JSON, not the always-on summary. JSON schema:

```json
{
  "ni": {
    "write_latency": {"count":N,"min":..,"mean":..,"max":..,"histogram":{"thresholds":[..],"bins":[..]}},
    "read_latency":  {...},
    "outstanding":   {"peak":..,"histogram":{...}},
    "rob_occupancy": {"peak":..,"histogram":{...}}
  },
  "router": {
    "credit_stall_cycles": M,
    "per_port": [{"port":0,"out_nonempty_ratio":0.0,"in_fifo":{...},"out_fifo":{...},"credit":{...}}, ...]
  },
  "phases": {"warmup_cycles":..,"measurement_cycles":..,"drain_cycles":..}
}
```

With multiple NMUs the `ni` object is an array keyed by flow label (one entry
per `NIPerfObserver`). JSON path: `NOC_PERF_FILE` env override, else
`<cwd>/perf_<run-label>.json`, where `run-label` is supplied by the harness
(for the gtest integration: the test/param name plus scenario id); the
observers do not invent it.

## 7. Harness integration

The harness owns the `uint64_t cycle` the observers bind to (Sec. 3.1). Per
simulated cycle the loop does, in order:

```cpp
++cycle;                       // cycle-start value the observers read this iteration
// ... existing pre_tick / component ticks / post_tick (incl. router tick) ...
for (auto& ni : ni_obs) ni.sample();   // after ticks: RoB occupancy + per-cycle outstanding
for (auto& r  : router_obs) r.sample(); // after router tick: input-FIFO state (not landing_)
```

The issue / completion callbacks fire inside the component ticks and read the
same `cycle`. When every master reports `done()`, the harness calls
`phase.begin_drain()` once. At loop exit `PerfReport{ni_obs, router_obs}.emit()`
prints the summary and (if `NOC_PERF=1`) writes JSON.

Instance count for the `test_router_loopback` two-node bidirectional fabric:
**two** `NIPerfObserver` (one per NMU / flow, labelled by flow) and **one**
`RouterPerfObserver` observing **all four** `TwoNodeFabric` routers (REQ/RSP x
node0/node1), each router's metrics tagged `{network: req|rsp, node: 0|1}`.

Wiring is additive: existing tests that do not construct the observers are
unaffected. First integration target is `test_router_loopback.cpp` (real
fabric, both flows).

## 8. Testing

Per the project rule (one happy-path + one error/edge test per new file):

- `Stats`: known value sequence -> exact count/mean/min/max/bin counts; empty
  `Stats` (count==0) returns sane mean (no div-by-zero).
- `PhaseController`: warmup boundary excludes pre-warmup samples; `begin_drain`
  flips phase; a latency sample issued in Measurement but completing in Drain is
  recorded.
- `NIPerfObserver`: injected issue/complete pairs (test-injection ctor, mirroring
  `AxiMasterObserver`) produce the expected latency and outstanding peak; a
  never-completed (stuck) transaction is surfaced, not silently dropped.
- `RouterPerfObserver`: a fed `Router` with a forced `credit==0` and a non-empty
  input FIFO whose front flit routes to that output accrues credit-stall cycles;
  an idle router (or `credit==0` with no front flit demanding that output)
  accrues zero.
- `AxiMaster` multi-subscriber callbacks: registering two `on_write_completed`
  callbacks fires both, in order (guards the H1 fix so a future caller cannot
  silently regress to overwrite semantics).
- Non-intrusive A/B integration test in `test_router_loopback.cpp`: run the
  scenario once without observers and once with, and assert the loop's final
  `cycle` count and the scoreboard mismatch count are identical. This is the
  concrete check that sampling/callbacks do not perturb DUT timing.

## 9. Scope boundary

In scope: two testbench-only observers, shared helpers, AxiMaster issue
callbacks, Rob occupancy getter, JSON/stdout telemetry, `test_router_loopback`
integration.

Out of scope (non-goals this round):

- QoS / per-QoS breakdown -- NoC does not consume AXI `awqos`/`arqos`; `noc_qos`
  is field-only, unimplemented (`nmu/packetize.hpp:23`).
- CSR register model -- telemetry only; CSR shape (CXL CPMU style) deferred.
- Layer-1 BFM end-to-end latency observer -- add later to decompose master-queue
  vs NI vs fabric.
- Deep congestion (wormhole-lock / arbitration-loss attribution) -- needs
  `Router::tick` event hooks (production change), deferred.
- Credit-RTT modeling gap -- the NMU/NSU edge pulse credit lacks credit-RTT
  accounting; this is a router/credit-model item, not a probe item.
- ChannelModel-path telemetry -- numbers there are non-physical; observers target
  the real Router path.

## 10. References

- `c_model/tests/common/test_logger.hpp` -- `AxiMasterObserver` (the
  non-intrusive observer pattern this design follows).
- `c_model/include/axi/scoreboard.hpp` -- `Scoreboard` (per-run aggregate +
  report pattern).
- `c_model/include/noc/router.hpp` -- `Router` introspection accessors.
- `c_model/include/nmu/nmu.hpp` -- `Nmu::rob()` / `vc_arbiter()` introspection.
- BookSim2 (Jiang et al., ISPASS 2013) -- `TrafficManager` three-phase
  measurement, `_RetireFlit` tail-retirement latency, `stats.hpp`.
- EP2724234A2 -- programmable threshold-bin latency histogram.
```
