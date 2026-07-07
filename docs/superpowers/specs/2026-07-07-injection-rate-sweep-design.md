# Injection-rate / saturation sweep — design

Date: 2026-07-07
Status: Draft (brainstormed, pending Codex + ponytail review)

## Goal

Produce a latency-vs-offered-load curve per VC count (vc1/2/4/8) on the 4x4 mesh, so the
value of adding VCs becomes measurable. Today vc1..vc8 end-to-end latency reads flat because
every co-sim run injects at a single greedy operating point, so no run applies enough congestion
to separate the configs.

## Success criteria

1. One CSV + plot per topology holding, for each vc config, points of (offered_load, achieved_bw, mean_latency).
2. The curve is non-vacuous: at low load latency is flat, past a knee it rises sharply.
3. The vc8 curve reaches its knee at a higher offered load than vc1 (VCs delay saturation). If it does
   not, that is itself a reportable result about the fabric, not a test failure.
4. The whole sweep runs on Verilator/WSL with no z3 in the loop.

## Method (locked in brainstorm)

Port FlooNoC's own bandwidth methodology. FlooNoC separates two orthogonal axes:

| axis | mechanism | FlooNoC source |
|---|---|---|
| spatial (where traffic goes) | deterministic pattern files | `util/gen_jobs.py` |
| temporal (offered rate) | per-cycle Bernoulli inject gate, runtime `+TRAFFIC_INJ_RATIO=%f` | `hw/test/floo_dma_test_node.sv` |
| measurement | achieved bandwidth + latency mean/std | `hw/test/axi_bw_monitor.sv` |

We mirror all three on the directed path. Directed addresses come from a stimulus file, so no
`randomize()` runs, so Verilator never invokes z3. z3 was only ever a cost on the constrained-random
conformance axis, which this work does not touch.

We do NOT use the c_model `perf.json` for this sweep. `perf.json` is project-authored. The perf
numbers here come from the ported upstream `axi_bw_monitor` so the results are trustworthy and
directly comparable to FlooNoC's own.

## Architecture

```
gen_test_patterns (existing: neighbor/transpose/uniform_random/hotspot)
     |  spatial pattern -> per-node file_master stimulus (no timing)
     v
per-node endpoint (sim/tb/user_node_endpoint.sv, NEW perf-sweep mode)
     file_master.load_files() -> aw_queue / ar_queue           (reuse, public members)
     sustained gated inject:  loop, interleave AW/AR, replay to sustain,
        each cycle: if !($urandom_range(0,99) < inject_ratio*100) @(posedge clk)  (FlooNoC Bernoulli)
        else drv.send_aw / drv.send_ar
     v  node AXI master bus
     axi_bw_monitor (ported, en_i gated to the steady-state window)
        -> $display: achieved_bw (bits/cyc), latency mean +/- std, util%
     v
sweep driver (NEW, thin, external — FlooNoC has no built-in loop either)
     for ratio in {points}:  make sim <perf-sweep mode> +inject_ratio=$ratio
     collect (offered=ratio*peak, achieved_bw, mean_latency) -> CSV -> curve
```

## Component 1 — injection-rate gate + sustained mode

**Where** `sim/tb/user_node_endpoint.sv`, a new runtime-selected perf-sweep mode (`+perf_sweep`
plusarg). The existing two-phase `TB_DIRECTED` scoreboard path is left untouched.

**Why a new mode, not a gate on the existing path.** The existing directed run is two-phase (all
writes drain, then all reads) because the scoreboard needs write-before-readback. A latency-load
curve needs sustained, interleaved, constant-rate traffic that reaches steady state. Gating the
two-phase run would measure a ramp plus drain, not steady state. The perf-sweep mode therefore runs
its own sustained interleaved loop.

**INPUT** `+inject_ratio=%f` (0.0 to 1.0, default 1.0), the parsed `aw_queue` / `ar_queue`.
**COMPUTE** interleave AW and AR launches, replay the queues to sustain traffic for the run window,
apply the per-cycle Bernoulli gate before each launch.
**OUTPUT** AXI transactions offered at rate `inject_ratio` of the greedy peak.

Zero upstream edit. `aw_queue`, `ar_queue`, `drv` are public in the pulp `axi_file_master`, and the
endpoint already calls `file_master.run_aw()` / `load_files()` today. The gate uses `$urandom_range`
(PRNG, not the constraint solver), so no z3.

`MAX_*_TXNS` (outstanding) must stay high enough that the Bernoulli gate, not the outstanding cap,
limits offered load across the whole sweep. Proposed 20 (FlooNoC's value). [TBD-tune] confirm the cap
never binds below fabric saturation.

## Component 2 — bw_monitor (ported)

**Provenance** copy `axi_bw_monitor.sv` from FlooNoC into `sim/tb/` (or `sim/dv/`), record source
+ license in an attribution note, following the repo pattern (`c_model/tests/axi/ATTRIBUTION.md`).
The copy may need its tap adapted to our AXI struct field names or driven from the DV interface. That
adaptation is on our copy, not the FlooNoC original.

**Wiring** one instance per manager node, tapping that node's AXI master bus (the bus the sustained
injector drives). `end_of_sim_i` pulsed at run end.

**Measurement window** `en_i` gates counting to the steady-state window. Enable after a warmup that
lets the fabric fill, disable before drain. Proposed warmup 500 cyc, measure 2000 cyc [TBD-tune]
against the observed fill time at the highest load.

**OUTPUT** per-node `$display` of achieved_bw, latency mean/std, util. The sweep driver parses these.

## Component 3 — sweep driver

**Where** a thin shell or make loop, external to the testbench. FlooNoC has no built-in sweep loop
either, it drives one `TRAFFIC_INJ_RATIO` point per run and loops externally.

**INPUT** a list of ratio points, the vc configs, the topology, the spatial pattern.
**COMPUTE** for each (vc, ratio): run `make sim` in perf-sweep mode with `+inject_ratio`, parse the
bw_monitor output.
**OUTPUT** one CSV row per run `(vc, pattern, inject_ratio, offered_load, achieved_bw, mean_latency,
p_latency_std)`, then a plot with one latency-vs-offered curve per vc.

**x-axis** offered load = `inject_ratio * peak_rate`, not achieved bandwidth. Past saturation achieved
plateaus while offered keeps rising and latency explodes, so plotting against achieved would collapse
the post-knee points. achieved_bw is reported as a second series, the gap achieved < offered marks the
saturation point.

**Point spacing** denser near the knee. Proposed `{0.1, 0.2, 0.3, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0}`
[TBD-tune] refine around the observed knee after a first coarse pass.

## Scope (one-shot measurement study)

| dimension | value |
|---|---|
| topology | mesh 4x4 |
| vc configs | vc1, vc2, vc4, vc8 |
| spatial pattern | uniform_random (primary). transpose / hotspot optional second pass |
| ratio points | ~9, knee-dense (above) |
| runs | 4 vc x ~9 = ~36, directed, no z3 |

Not a permanent framework. The sweep driver is a throwaway loop over the existing `make sim`. If a
reusable sweep is wanted later, build it fresh against this.

## What does NOT change

- pulp axi VIP (`sim/dv/axi-0.39.7`) source: untouched. We reuse public members only.
- FlooNoC `axi_bw_monitor` original: untouched. We bring in a copy.
- The existing two-phase `TB_DIRECTED` scoreboard path and the constrained_random axis: untouched.

## Risks

| risk | mitigation |
|---|---|
| warmup/measure window mis-sized, curve reads transient not steady state | tune window against fill time at max load, sanity-check low-load latency is flat |
| outstanding cap binds before fabric saturates, curve measures the master not the NoC | set cap high (20), verify it never binds below saturation |
| bw_monitor tap does not match our AXI struct | adapt the copy to our struct / DV interface, verify against a known single-txn latency |
| knee falls between sampled points | coarse pass first, then add points around the knee |
| replay-to-sustain changes address footprint vs a single pass | replay reuses the same pattern addresses, so spatial pattern is preserved across the window |

## Verification

1. Single-txn sanity: at very low load the mean latency matches a hand-computed zero-load latency
   (pipeline depth + hop count), confirming the monitor and window are correct.
2. Monotonic knee: latency rises with offered load, flat then sharp.
3. VC separation: compare the four curves, report where each saturates.
