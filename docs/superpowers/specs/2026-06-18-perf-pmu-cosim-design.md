# In-fabric AXI/NoC performance monitor in the co-sim (PMU-style)

Date: 2026-06-18
Status: Draft rev 2 (Codex GO; hybrid SV+C decision applied; PG037 blocks mapped)
Supersedes the approach of: the c_model two-pass perf harness + `make perf`
(`2026-06-18-perf-probe-productization-design.md`). That harness measured the
c_model in isolation with a two-pass zero-load characterization. This design
replaces it with in-fabric, single-run performance monitors read out of the
real Verilator co-sim, so `make run-tb-top SCENARIO=<id>` produces the perf log.

## 1. Goal

Per-transaction latency + per-component throughput/occupancy/stall, measured by
monitors integrated into the running co-sim (not a separate offline model), read
out at end of run. The probe is non-intrusive (passive observation only).

## 2. Survey grounding (why single-run counters, not two-pass zero-load)

| Source | What it establishes |
|---|---|
| AXI-REALM (PULP, arXiv 2501.10161 / 2311.09662) | "Latency is measured online as a single run." Per-manager monitor at AXI ingress/egress; AXI-id outstanding tracking (DOTQ: Head-Tail + Linked-Data tables); memory-mapped register readout. No isolated characterization pass for the online output. |
| Ciordas et al., Event-Based Monitoring Service for NoC (TODAES 2005) + Transaction Monitoring in NoC | Hardware probes integrated into the running NoC at NI/router/link; run-time, single-run; only the NI sees end-to-end transaction identity. |
| AMD AXI Performance Monitor (PG037) | Per-AXI-interface "slot"; 6 profile counters/slot (write/read byte count, transaction count, latency count); latency start = Address-Accept (AxVALID&AxREADY); end = RLAST for reads, B-response (BVALID&BREADY) for writes (a write completes at its response, not at WLAST); sample-interval snapshots; AXI-ID filter; register readout. |

Conclusion: the standard is **single-run, counter/event-based, in-fabric**. The
two-pass per-signature `zero_load` (a Dally-textbook construct) is not how
in-fabric monitors work and is dropped. If an "ideal" reference is wanted, report
the **minimum observed latency** of the same run, or an externally configured
nominal -- never a value labelled as a measured zero-load.

OSS reuse reality: `pulp-platform/axi_rt` (SHL-0.51) publishes only the
**regulation** RTL (budget/burst-split/isolate); its latency-monitor (the paper's
`erealm` DOTQ + stage counters) is **not in the public repo**, and its regulation
modules are in-path (would perturb latency). So we reuse the published
**concepts** (DOTQ id-correlation, PG037 counter set + latency definition), not a
drop-in module.

## 3. Monitor structure (PG037-mirrored)

A monitor **slot per AXI interface** ("agent"). In the 2-node tb_top: the
manager edge (`master <-> NMU`) and the subordinate edge (`NSU <-> slave`), per
node -- up to 4 AXI slots.

Per-slot counters (PG037's six + survey additions):

| Counter | Definition |
|---|---|
| write/read byte count | beats x bytes/beat |
| write/read transaction count | completed transactions |
| write/read latency sum | sum of per-transaction latency |
| latency min / max | per-slot extrema |
| outstanding max | peak in-flight (DOTQ depth) |

- Latency per transaction = completion cycle - address-accept cycle, where the
  end event differs by direction: **read** ends at RLAST (`RLAST & RVALID &
  RREADY`); **write** ends at the **B response** (`BVALID & BREADY`), NOT at WLAST
  -- a write transaction is not complete until its response returns, so measuring
  to WLAST (write-data done) would understate write latency by the
  data-to-response gap. Start event = address accept (`AWVALID&AWREADY` for write,
  `ARVALID&ARREADY` for read). Correlation
  must handle MULTIPLE outstanding transactions with the SAME AXI id: a single
  `start[id]` is wrong (a second same-id issue overwrites the first). Use a
  per-(id, direction) FIFO of issue cycles; on completion, pop the FRONT of that
  id's FIFO -- valid because AXI4 requires same-id responses to return in issue
  order (this is the AXI-REALM DOTQ Head-Tail/Linked-Data idea reduced to an
  in-order queue). Capacity = max outstanding per id; overflow is a measurement
  error to assert, not silently drop. The SV slot monitor (Section 4) sees only
  the AXI wires: AXI id is present but `scenario_line` is NOT (it is a
  c_model-internal tag, not an AXI signal), so the monitor MUST correlate by
  `(id, direction)` + in-order FIFO. Write data (no W-channel id) follows the AW
  id order.
- A global clock counter provides the cycle stamp.
- NoC router/link counters (Ciordas concept; not AXI-level, so not in PG037):
  per-router flit count, output/input FIFO occupancy (sum + max), and link
  backpressure stall cycles defined as `valid && !credit_available` (NoC
  credit/flit terminology -- NOT AXI ready/valid).
- Sample window: single-run, default the whole scenario is one window. Optional
  `+perf_start`/`+perf_end` plusargs gate the measured window (warmup/drain),
  with no second pass.

### 3.1 PG037 block mapping

Mirror the AMD APM core blocks (`docs/image/Block Diagram of AXI Performance
Monitor core.jpg` Fig 1-1; `Event Count Module.jpg` Fig 1-2; `Accumulator and
Range Incrementer.jpg` Fig 1-3; `Event Counting in Profile Mode.jpg` Fig 1-4).
The APM datapath per monitor slot is:

`Monitor Slot -> ID Filter & Mask -> Metric Enable Generator -> Metric Selector
Mux -> Accumulator (+ Sampled Accumulator) & Range Incrementer -> Registers`,
with a Global Clock Counter (cycle base) and a Timer (sample interval).

Our C objects mirror these one-to-one:

| APM block (Fig) | Our equivalent |
|---|---|
| Monitor Slot (Fig 1-1) | one monitor per AXI interface |
| Global Clock Counter (Fig 1-1) | the co-sim cycle counter (`now`) |
| ID Filter & Mask (Fig 1-2) | optional per-AXI-id selection -> per-id latency/count breakdown |
| Metric Enable Generator + Metric Selector Mux (Fig 1-2) | which metric a counter accumulates (latency / byte / txn) -- config |
| Accumulator (Fig 1-3) | running sum/count counter |
| Sampled Accumulator (Fig 1-4) | snapshot at the sample-interval boundary (Δ -> window throughput/latency) |
| Range Incrementer, Range LOW/HIGH (Fig 1-3) | latency-distribution bins: count transactions whose latency falls in `[LOW, HIGH)` -> a configurable latency histogram |
| Registers + AXI4-Lite (Fig 1-1) | the readout struct (dumped via DPI; no AXI4-Lite master needed) |
| Event Log + AXI4-Stream (Fig 1-1) | trace stream -- DEFERRED (profile/count mode first) |
| Timer (Fig 1-4) | the sample-window driver |

This adds two items over the bare counter set: an **ID filter** (per-id
breakdown) and a **Range Incrementer** (latency histogram bins), both first-class
in PG037. Profile mode (counters + sampled accumulators) is the first target;
the Event-Log/AXI4-Stream trace path is deferred.

## 4. Implementation: hybrid (SV AXI slots + C/DPI internal), unified dump

Split by where the signal lives (Codex design review):

- **AXI slots -> SV passive monitor** (`axi_perf_monitor.sv`, one per AXI
  interface). It reads the AXI wires and counts bytes/transactions/latency with a
  per-id issue-cycle FIFO, using the direction-specific start/end events of
  Section 3 (start = address accept; end = RLAST for reads, B-response for writes). This is the
  PG037-faithful approach (counting on wire handshakes) and it is the ONLY way to
  cover the subordinate slot: the `NSU -> slave` AXI edge is driven by the NSU's
  AxiMasterPort and has NO c_model `AxiMaster` callback, so a callback-based C
  monitor cannot see it. SV monitors cover the manager (`master -> NMU`) and
  subordinate (`NSU -> slave`) slots uniformly. Bonus: no need to forward the
  `on_*_issued` callbacks, so production `AxiMaster` is untouched.
- **NoC router / NI internal state -> C + DPI**: the data lives in the c_model
  `Router` (`output_fifo_size`/`input_fifo_size`), `Rob` (occupancy),
  `AxiMasterPort` (queue sizes) -- C++ objects, sampled per tick via a DPI hook.
- **Inter-router link -> SV**: the link is an SV wire (`tb_top.sv link_*`), so a
  small SV counter taps valid/flit/credit there.
- **Unified readout -> C**: a `PerfCollector` (C++) gathers the SV slot/link
  counters (read at end via DPI / hierarchical reference) plus the C-side
  occupancy counters, and writes one `perf.json` at end of run.

Why not pure C+DPI (the first draft's choice): the subordinate AXI slot has no
c_model callback, so C alone leaves a slot uncovered. Why not pure SV: the NoC
router/NI occupancy lives in c_model C++ objects with no SV signal, so it must be
sampled in C. The split is forced by where each datum exists, not preference.

Non-intrusive: SV monitors only read wires; C sampling only reads const getters.
An A/B cycle-equality check (as in `test_router_loopback.cpp`'s existing
non-intrusive test) gates this.

## 5. Single run, no zero-load

- Report measured per-transaction latency (min/mean/max) + the counters above.
- No isolated Pass-1. The `min observed latency` field is the same run's minimum;
  it is labelled "min observed", not "zero_load".
- `queueing` (measured - ideal) is dropped unless an external nominal is provided
  and clearly labelled.

## 6. Co-sim integration points

- `cosim/sv/axi_perf_monitor.sv` (new): passive AXI slot monitor; instantiate on
  each AXI interface in `tb_top.sv` -- the manager edges (`master -> NMU`) and the
  subordinate edges (`NSU -> slave`), per node. Counts byte/transaction/latency
  per slot, per-id issue-cycle FIFO for same-id correctness. No production
  `AxiMaster` change (the `on_*_issued`-forwarding the first draft needed is
  dropped -- the SV monitor reads the wires).
- `cosim/sv/flit_link_perf_monitor.sv` (new): passive counter on the inter-router
  link wires (`tb_top.sv link_*`): flit count, and stall = `valid && !credit`
  (NoC credit/flit terminology, NOT AXI ready). One per link direction.
- `RouterShellAdapter`: add `rsp_router()` accessor (only `req_router()` exists;
  the `rsp_router_` member is there). `Router::output_fifo_size`/`input_fifo_size`
  already exist.
- `NmuShellAdapter` / `NsuShellAdapter`: add accessors to reach `Rob::*occupancy`
  and the NSU `AxiMasterPort` queue sizes (the c_model getters exist; the shells
  keep the objects private with no pass-through). Verify the NSU queue-size API
  shape.
- `cosim/c/cmodel_dpi.cpp`: add `cmodel_perf_sample_tick()` (sample the C-side
  router/NI occupancy each cycle) and `cmodel_perf_dump(path)` (collect the SV
  slot/link counters + the C occupancy into a `PerfCollector` and write JSON).
  None exist today -- new from scratch. Must not disturb the existing scoreboard
  callbacks.
- `tb_top.sv`: instantiate the two SV monitors; import + call the sample/dump DPI.
- `cosim/verilator/Makefile`: `run-tb-top` writes
  `output/<scenario>/perf.json` beside `run.log`.

## 7. Survive / replace

| Current piece | Verdict |
|---|---|
| `NIPerfObserver` (on_issue/on_complete latency) | survive for the c_model unit tests; the co-sim AXI latency is re-implemented in the SV slot monitor (same per-id-FIFO logic, but on AXI wires) |
| Router/NI introspection getters | survive (the C-side occupancy counters) |
| `PerfReport` JSON structure | survive as the readout shape, reframed to counters/measured (drop zero_load/queueing fields) |
| flit-link decorators (LinkProbe) | survive only for the c_model unit tests; the co-sim uses the SV link monitor |
| two-pass harness, `characterize_signature`, per-signature `zero_load` | replace (single-run) |
| `test_perf_probe.cpp` + `make perf` (c_model scenario suite) | keep as a c_model-level perf view for now; retire the `zero_load`/`queueing` fields once the co-sim monitor reaches parity (Codex review) |

## 8. Files (anticipated)

| File | Action |
|---|---|
| `cosim/sv/axi_perf_monitor.sv` | new SV: passive per-AXI-slot byte/transaction/latency counters + per-id issue-cycle FIFO; PG037 block structure (Section 3.1) |
| `cosim/sv/flit_link_perf_monitor.sv` | new SV: inter-router link flit count + `valid && !credit` stall counter |
| `c_model/include/cosim/perf_collector.hpp` | new C: gather SV slot/link counters + C-side router/NI occupancy; `dump(path)` JSON |
| `cosim/c/cmodel_dpi.cpp` + `cmodel_dpi.h` | add `cmodel_perf_sample_tick` / `cmodel_perf_dump` DPI (NO issue-callback forwarding -- SV reads the wires); must not disturb the scoreboard callbacks |
| `c_model/include/cosim/router_shell_adapter.hpp` | add `rsp_router()` accessor |
| `c_model/include/cosim/nmu_shell_adapter.hpp` / `nsu_shell_adapter.hpp` | add accessors to reach `Rob` / `AxiMasterPort` occupancy |
| `cosim/sv/tb_top.sv` | instantiate the AXI slot + link SV monitors; import + call the sample/dump DPI |
| `cosim/verilator/Makefile` | `run-tb-top` emits `output/<scenario>/perf.json` |

## 9. References

- Kwon et al. / Ciordas et al. -- NoC monitoring (event-based, run-time).
- AXI-REALM, PULP platform (arXiv 2311.09662, 2501.10161; `pulp-platform/axi_rt`,
  SHL-0.51) -- AXI traffic monitoring, DOTQ id-correlation.
- AMD AXI Performance Monitor, PG037 -- per-AXI-interface counter set, latency
  definition (Address-Accept to RLAST for reads, to B-response for writes),
  sample interval.
- `docs/architecture.md` sec. 3.2 -- tick discipline (1 tick = 1 cycle).
