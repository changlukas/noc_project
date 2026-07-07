# Saturation-throughput measurement (injection-rate driven) — design

Date: 2026-07-07
Status: Draft (brainstormed + Codex + booksim2 survey, pending final user review)

## Goal

Show the value of VC count on the 4x4 mesh by measuring the saturation throughput of each VC
configuration (vc1, vc2, vc4, vc8). Today vc1..vc8 end-to-end latency reads flat, because every
co-sim run injects at a single greedy operating point with no steady-state throughput readout, so the
configs cannot be separated.

## Deliverable and success criteria

1. A bar chart: saturation throughput (accepted payload bits/cycle) per VC config, plus the CSV behind it.
2. Non-vacuous: accepted throughput rises with offered load then plateaus, and the plateau is the reported number.
3. vc8 saturation throughput is higher than vc1 (more VCs sustain more load before the fabric saturates).
   If it is not, that is a reportable result about the fabric, not a test failure.
4. Runs on Verilator/WSL with no z3 in the loop.

## Why throughput, not a latency curve

booksim2 (`src/trafficmanager.cpp`, `src/injection.cpp`) is the canonical reference. It measures two
latencies: network latency `flat/nlat = atime - itime` (network entry to arrival, bounded even at
saturation) and packet latency `plat`, measured from packet GENERATION and including the source-queue
wait (`_partial_packets[source]`). Only `plat` diverges at saturation, because the source queue is what
explodes once offered load exceeds capacity.

A ported `axi_bw_monitor` taps the AXI bus, so it can only measure network-latency-equivalent (bus
issue to completion), which stays bounded at saturation. A latency curve built from it would read flat
across vc1..vc8, reproducing the exact problem we are trying to fix. Capturing the latency knee would
require timestamping each transaction at GENERATION inside the injector (the AXI analogue of booksim2's
source queue), which is a larger change.

Saturation throughput sidesteps this. Accepted throughput is what `axi_bw_monitor` measures accurately.
The plateau of accepted-vs-offered is the saturation point, and its height is the VC-value metric. This
also matches the relaxed deliverable (a bar chart is enough, no curve required).

## Method

Port FlooNoC's injection mechanism and measure with FlooNoC's monitor, using booksim2's steady-state
discipline.

| axis | mechanism | source |
|---|---|---|
| spatial pattern | deterministic stimulus file | our `gen_test_patterns` (uniform etc.), = FlooNoC `traffic_type` |
| offered load | per-cycle injection at rate `traffic_inj_ratio` | FlooNoC `TRAFFIC_INJ_RATIO`, = booksim2 `BernoulliInjectionProcess` |
| measurement | accepted throughput (payload bits/cycle) | ported FlooNoC `axi_bw_monitor` |
| steady state | run past convergence, sample in the stable region | booksim2 sample_period / stopping_thres |

Directed addresses come from the file, so no `randomize()` runs and Verilator never calls z3. z3 was
only ever a cost on the constrained-random conformance axis, which this work does not touch.

## Architecture

```
gen_test_patterns (existing: uniform_random etc.)  -> per-node stimulus file (spatial pattern, no timing)
        v
per-node endpoint (sim/tb/user_node_endpoint.sv, NEW traffic mode)
   file_master.load_files() -> aw_queue / ar_queue                (reuse public members)
   continuous injection at traffic_inj_ratio:
       each cycle, inject the next transaction with probability traffic_inj_ratio,
       else idle one cycle (RandomFloat gate, booksim2 Bernoulli; no z3)
       replay the queues to sustain traffic through the measurement window
        v  node AXI master bus
   axi_bw_monitor (ported): accepted throughput (payload bits/cyc), + network latency as sanity only
        v
collect (thin external script)
   for a few high traffic_inj_ratio points per VC config: run, read accepted throughput,
   confirm the plateau -> saturation throughput -> CSV -> bar chart
```

## Component 1 — continuous injection at traffic_inj_ratio

**Where** `sim/tb/user_node_endpoint.sv`, a new runtime-selected traffic mode (`+traffic_inj_ratio=%f`
present selects it). The existing two-phase `TB_DIRECTED` scoreboard path is left untouched.

**Why a new mode.** The existing directed run is two-phase (all writes drain, then all reads), needed
for write-before-readback scoreboarding. Saturation throughput needs sustained, continuous, mixed
traffic held in steady state. The traffic mode runs its own continuous injection loop.

**INPUT** `+traffic_inj_ratio=%f` (0.0 to 1.0), the parsed `aw_queue` / `ar_queue`.
**COMPUTE** each cycle inject the next transaction with probability `traffic_inj_ratio` (else idle one
cycle), interleaving AW and AR, replaying the queues to sustain traffic. Preserve the emitter's
read/write proportion. Rotate the replay start offset per node so replay does not create lockstep
periodic traffic (booksim2 keeps generation continuous, not looped).
**OUTPUT** transactions offered at a controlled fraction of peak.

Zero upstream edit. `aw_queue`, `ar_queue`, `drv` are public in the pulp `axi_file_master`, and the
endpoint already calls `file_master.run_aw()` / `load_files()` today. The gate uses `RandomFloat` /
`$urandom_range` (PRNG, not the constraint solver), so no z3.

`MAX_*_TXNS` (outstanding) must be high enough that the fabric saturates before the outstanding cap
binds, otherwise accepted throughput measures endpoint credit pressure, not fabric capacity. Proposed
20 (FlooNoC's value). Instrument attempted / accepted / cap-blocked injection cycles per node.
Acceptance rule: cap-blocked cycles at the reported plateau must be near zero. Raise the cap until the
plateau stops moving.

## Component 2 — bw_monitor (ported)

**Provenance** copy `axi_bw_monitor.sv` from FlooNoC into `sim/tb/`, record source + license in an
attribution note (repo pattern: `c_model/tests/axi/ATTRIBUTION.md`). Adapt the tap to our AXI struct or
DV interface in our copy, not the FlooNoC original.

**Wiring** one instance per manager node on its AXI master bus. Aggregate accepted throughput is the
sum across managers. Report per-node throughput too, to expose imbalance (uniform_random should be
symmetric, a large spread flags a problem).

**Primary output** accepted throughput = delivered payload bits / measurement-window cycles.
**Secondary (sanity only)** network latency mean, explicitly not used to locate saturation.

## Component 3 — collect saturation throughput

**Where** a thin external shell or make loop over `make sim` in traffic mode. FlooNoC has no built-in
sweep loop either.

**Steady state (booksim2-derived).** Do not use a fixed warmup/measure window. Run long, and in the
collector detect the steady region: compute accepted throughput over successive sample windows and take
the plateau once the value stops changing beyond a small threshold across consecutive windows. Flag a
point as unstable if accepted throughput keeps drifting (backlog growing), which means past saturation
or cap-bound. This mirrors booksim2's stopping_thres convergence rather than guessing window sizes.

**Saturation throughput.** Accepted throughput equals offered while below saturation, then plateaus.
Run a few high offered-load points per VC config (proposed `traffic_inj_ratio` in {0.7, 0.85, 1.0}
[TBD-tune]) and confirm accepted throughput has flattened across them. The flattened value is the
saturation throughput. A full low-to-high sweep is optional supporting evidence (the accepted-vs-offered
curve), not required for the bar chart.

**Offered load** defined as payload bits/cycle, not the AW/AR launch count:
`traffic_inj_ratio * peak_payload_rate`, where peak accounts for the read/write mix and bytes per
transaction. Record observed AW / AR / W-bytes / R-bytes per run so offered and accepted are
apples-to-apples.

**OUTPUT** CSV `(vc, pattern, traffic_inj_ratio, offered_bits_per_cyc, accepted_bits_per_cyc,
cap_blocked_frac, stable?)`, and a bar chart of saturation throughput per VC config built by a
throwaway script (no plotting infrastructure).

## Scope (one-shot measurement study)

| dimension | value |
|---|---|
| topology | mesh 4x4 |
| vc configs | vc1, vc2, vc4, vc8 |
| spatial pattern | uniform_random only (Phase 1). transpose / hotspot only if Phase 1 is inconclusive |
| offered-load points | a few high points to confirm the plateau (above) |
| runs | ~4 vc x ~3 points, directed, no z3 |

Not a permanent framework. The collector is a throwaway loop over the existing `make sim`. Build a
reusable sweep fresh against this only if later wanted.

## What does NOT change

- pulp axi VIP (`sim/dv/axi-0.39.7`) source: untouched, public members reused only.
- FlooNoC `axi_bw_monitor` original: untouched, brought in as a copy.
- The existing two-phase `TB_DIRECTED` scoreboard path and the constrained_random axis: untouched.

## Naming (aligned to FlooNoC + Dally)

| concept | term used | source |
|---|---|---|
| injection knob | `traffic_inj_ratio` | FlooNoC `TRAFFIC_INJ_RATIO` |
| per-cycle probabilistic injection | injection at rate `traffic_inj_ratio` | booksim2 BernoulliInjectionProcess |
| x-quantity | offered load (payload bits/cyc) | Dally interconnect texts |
| y-quantity | accepted throughput, saturation throughput | Dally, booksim2 |
| traffic pattern | uniform / transpose / hotspot | FlooNoC `traffic_type` |
| stable-region sampling | steady state, convergence | booksim2 |

## Risks

| risk | mitigation |
|---|---|
| outstanding cap binds before fabric saturates, plateau measures the endpoint not the NoC | instrument cap-blocked cycles, require near zero at the plateau, raise cap until plateau stops moving |
| replay creates periodic lockstep traffic instead of the intended pattern | long queues, per-node rotated replay start offset, report per-destination distribution |
| steady region mis-identified, plateau read off a transient | detect convergence across sample windows, flag drifting (unstable) points |
| offered vs accepted not comparable (AW/AR count vs payload bits) | define offered as payload bits/cyc, record observed AW/AR/W/R per run |
| bw_monitor tap does not match our AXI struct | adapt the copy, verify accepted throughput against a hand-computed single-stream case |
| read/write mix drift changes the plateau | preserve emitter R/W proportion, report the observed mix |

## Verification

1. Throughput sanity: a single stream at low load shows accepted equal to offered (linear region).
2. Cross-check (ponytail): at one point, compare bw_monitor accepted bytes against the c_model
   `perf.json` byte counts over the same window. Agreement validates both, disagreement shows which to
   trust.
3. Plateau: accepted throughput flattens across the high offered-load points, cap-blocked near zero.
4. VC separation: compare the four saturation numbers, report the bar chart.
