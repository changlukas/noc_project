# In-fabric AXI/NoC performance monitor in the co-sim (PMU-style)

Date: 2026-06-18
Status: Draft rev 3 (Codex GO; hybrid SV+C; PG037 blocks mapped; clean-break
cleanup of the two-pass c_model model per user decision 2026-06-18 -- Section 7)
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

Per-slot counters. The PG037 agent metric list (doc p.11) IS the per-slot output
contract; the survey additions extend it:

| Counter | Definition | Source |
|---|---|---|
| write/read transaction count | completed transactions (PG037: requests; we count completions) | PG037 |
| write/read byte count | beats x bytes/beat (throughput numerator) | PG037 |
| write/read latency sum | sum of per-transaction latency | PG037 |
| latency min / max | per-slot extrema (`min` = the min-observed reference, Section 5) | PG037 |
| slave write idle cycles | cycles with `WVALID && !WREADY` (slave slow to accept write data) | PG037 |
| master read idle cycles | cycles with `RVALID && !RREADY` (master slow to accept read data) | PG037 |
| outstanding max | peak in-flight per slot (DOTQ depth) | survey |
| latency histogram | per-`[LOW,HIGH)` bin transaction count (PG037 Range Incrementer) | PG037 |
| per-id breakdown | latency sum/count keyed by AXI id (PG037 ID Filter) | PG037 |

PG037 AXI4-Stream metrics (transfer/data/position/null byte, packet, idle) are
**not applicable** -- the fabric has no AXI4-Stream interface. System-level
throughput / interconnect latency are **derived** post-hoc (byte count / window
cycles; per-router/link sums), not stored counters.

Reporting location (dedup, Section 5.1): the monitor computes latency per slot,
but latency sum/min/max and histogram are **reported once** -- end-to-end in the
`latency` section (manager), slave-side in the subordinate slot's
`service_latency` -- never duplicated under `axi_slots[]`, which keeps only the
throughput/backpressure counters.

- Latency per transaction = completion cycle - address-accept cycle, where the
  end event differs by direction: **read** ends at RLAST (`RLAST & RVALID &
  RREADY`); **write** ends at the **B response** (`BVALID & BREADY`), NOT at WLAST
  -- a write transaction is not complete until its response returns, so measuring
  to WLAST (write-data done) would understate write latency by the
  data-to-response gap. **This write-end choice intentionally diverges from the
  PG037 default** (PG037 measures write latency to first/last write data; it does
  not offer a B-response endpoint). We take the transaction-completion semantics
  (matching AXI-REALM) over PG037 parity here; the divergence is noted in
  `docs/performance-probe.md`. Start event = address accept (`AWVALID&AWREADY` for
  write, `ARVALID&ARREADY` for read). Correlation
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
  backpressure stall cycles. **Stall is credit-counter-based, NOT a `!credit`
  level read**: the link credit signal is a single-cycle *pulse* returned when a
  downstream buffer slot frees (`tb_top.sv:193,198 // pulse`), so `!credit` is the
  normal idle state and a level test would count a stall almost every cycle. The
  SV link monitor maintains its own credit counter per link direction: seed =
  downstream VC buffer depth; `+1` on each credit pulse; `-1` on each flit sent
  (`valid && accepted`). A **stall cycle** = `valid && credit_count == 0` (a flit
  is offered but no credit remains). NoC credit/flit terminology -- NOT AXI
  ready/valid.
- Sample window: single-run, default the whole scenario is one window. Optional
  `+perf_start`/`+perf_end` plusargs gate the measured window (warmup/drain),
  with no second pass. **Window inclusion rule**: a transaction counts in the
  window iff its **completion** cycle falls in `[perf_start, perf_end)`
  (address-accept may precede the window). Byte/flit/stall/occupancy counters
  accumulate only on cycles within the window.

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
| Sampled Accumulator (Fig 1-4) | **DEFERRED** -- the first target is a single window (Section 5); periodic interval snapshots / time-series are not produced |
| Range Incrementer, Range LOW/HIGH (Fig 1-3) | latency-distribution bins: count transactions whose latency falls in `[LOW, HIGH)` -> a configurable latency histogram |
| Registers + AXI4-Lite (Fig 1-1) | the readout struct (dumped via DPI; no AXI4-Lite master needed) |
| Event Log + AXI4-Stream (Fig 1-1) | trace stream -- DEFERRED (profile/count mode first) |
| Timer (Fig 1-4) | **not used for periodic sampling**; only `+perf_start`/`+perf_end` gate a single window (Section 5) |

This adds two items over the bare counter set: an **ID filter** (per-id
breakdown) and a **Range Incrementer** (latency histogram bins), both first-class
in PG037. First target = counters + latency histogram + per-id breakdown over a
**single window**. Periodic sampled accumulators (time-series) and the
Event-Log/AXI4-Stream trace path are **deferred** -- consistent with Section 5's
single-run, no-second-pass scope and the Section 5.1 schema (no time-series array).

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
**A/B gate (normative)**: a perf-enabled build and a perf-disabled build of the
same scenario must produce (a) identical scoreboard pass/fail and (b) an identical
per-transaction completion-cycle sequence. Any difference means the monitor
perturbed the DUT and is a blocker. (This extends the cycle-equality idea of
`test_router_loopback.cpp`'s existing non-intrusive test to the co-sim.)

## 5. Single run, no zero-load

- No isolated Pass-1, no analytical `zero_load`. The best-case reference is the
  **minimum observed latency** of the same run -- a real measurement of the
  least-congested transaction -- labelled "min observed", never "zero_load".
- Report latency at **two grains** (user decision 2026-06-18):
  1. **per transaction** -- each completed transaction's measured latency, with
     its AXI id, direction, and slot (the raw rows; lets the reader see the
     distribution and outliers).
  2. **per signature class** -- transactions grouped by `(op, len, size, src->dst
     slot)`; report min / mean / max per class. The **per-class min observed** is
     that class's best-case reference line.
- `queueing` (measured - ideal) is dropped unless an external nominal is provided
  and clearly labelled (not the default output).

## 5.1 perf.json schema (normative)

One `perf.json` per scenario. All counts/cycles are unsigned 64-bit. Keys fixed;
an absent optional array is `[]`, never omitted. **Each datum has one home** (no
duplication): `axi_slots[]` = throughput + backpressure; `latency` = the single
home for end-to-end latency (raw + aggregates); `noc` = router/link.

| Section | Holds | Grain |
|---|---|---|
| `axi_slots[]` | byte/txn count, the two idle counters, outstanding_max; subordinate slots add `service_latency` (slave-side, summary only) | per AXI interface |
| `latency.transactions[]` | raw per-txn end-to-end rows (the only raw source) | manager slot |
| `latency.by_signature[]` | min/mean/max per `(op,len,size,src,dst)` -- aggregate of the rows; `min` = best-case ref | per signature |
| `latency.histogram[]` | per-`[low,high)` bin count -- aggregate of the rows | whole run |
| `noc.routers[]` / `noc.links[]` | flit count + occupancy max; link credit-stall | per router/link |

```json
{
  "schema_version": 1,
  "scenario": "AX4-BAS-003",
  "window": { "start_cyc": 0, "end_cyc": 64 },
  "axi_slots": [
    { "name": "node0.manager", "role": "manager",
      "write_txn_count": 1, "read_txn_count": 1,
      "write_byte_count": 64, "read_byte_count": 64,
      "slave_write_idle_cyc": 2, "master_read_idle_cyc": 0, "outstanding_max": 1 },
    { "name": "node1.subordinate", "role": "subordinate",
      "write_txn_count": 1, "read_txn_count": 1,
      "write_byte_count": 64, "read_byte_count": 64,
      "slave_write_idle_cyc": 3, "master_read_idle_cyc": 1, "outstanding_max": 1,
      "service_latency": { "write": { "min": 14, "mean": 14, "max": 14 },
                           "read":  { "min": 12, "mean": 12, "max": 12 } } },
    { "name": "node0.subordinate", "role": "subordinate",
      "write_txn_count": 0, "read_txn_count": 0, "write_byte_count": 0,
      "read_byte_count": 0, "slave_write_idle_cyc": 0, "master_read_idle_cyc": 0,
      "outstanding_max": 0 },
    { "name": "node1.manager", "role": "manager",
      "write_txn_count": 0, "read_txn_count": 0, "write_byte_count": 0,
      "read_byte_count": 0, "slave_write_idle_cyc": 0, "master_read_idle_cyc": 0,
      "outstanding_max": 0 }
  ],
  "latency": {
    "measured_at": "manager slot -- end-to-end (address-accept -> B-response/RLAST)",
    "transactions": [
      { "id": 3, "dir": "write", "src": "node0", "dst": "node1",
        "accept_cyc": 10, "complete_cyc": 52, "latency": 42, "bytes": 64 },
      { "id": 5, "dir": "read",  "src": "node0", "dst": "node1",
        "accept_cyc": 12, "complete_cyc": 50, "latency": 38, "bytes": 64 }
    ],
    "by_signature": [
      { "op": "write", "len": 8, "size": 3, "src": "node0", "dst": "node1",
        "count": 1, "min": 42, "mean": 42, "max": 42 },
      { "op": "read",  "len": 8, "size": 3, "src": "node0", "dst": "node1",
        "count": 1, "min": 38, "mean": 38, "max": 38 }
    ],
    "histogram": [ { "low": 0, "high": 16, "count": 0 },
                   { "low": 16, "high": 32, "count": 0 },
                   { "low": 32, "high": 64, "count": 2 } ],
    "per_id": "optional (config-gated, default off): same shape grouped by AXI id"
  },
  "noc": {
    "routers": [
      { "name": "req.R(0,0)", "flit_count": 8, "in_fifo_occ_max": 2, "out_fifo_occ_max": 2 },
      { "name": "req.R(1,0)", "flit_count": 8, "in_fifo_occ_max": 2, "out_fifo_occ_max": 2 },
      { "name": "rsp.R(1,0)", "flit_count": 6, "in_fifo_occ_max": 1, "out_fifo_occ_max": 2 },
      { "name": "rsp.R(0,0)", "flit_count": 6, "in_fifo_occ_max": 2, "out_fifo_occ_max": 1 }
    ],
    "links": [
      { "name": "req_0to1", "flit_count": 4, "stall_cyc": 1 },
      { "name": "rsp_1to0", "flit_count": 4, "stall_cyc": 0 }
    ]
  }
}
```

- `role` = `manager` (master->NMU) or `subordinate` (NSU->slave). `dir` = `write`/`read`.
- End-to-end latency lives ONLY in `latency`; slave-side service time ONLY in the
  subordinate slot's `service_latency`. Neither is repeated in the other.
- Histogram bins are config-driven; default `[0,16) [16,32) [32,64) [64,128)
  [128,256) [256,inf)` cycles.
- `min` is the **min observed** reference (Section 5), not a zero-load value.
- `per_id` breakdown (PG037 ID filter) is optional, config-gated, default off.
- `in/out_fifo_occ_max` only (occupancy sum dropped -- low reader value).
- The per-id correlation FIFO has finite capacity (max outstanding per id);
  overflow is asserted as a measurement error, never silently dropped.

### CLI summary (stdout)

`run-tb-top` prints an aggregate summary (no raw `transactions[]` -- that is
JSON-only, so the CLI never floods on large runs):

```text
[perf] AX4-BAS-003   window [0,64) cyc
  AXI throughput / backpressure
    slot                bytes_wr bytes_rd txn_wr txn_rd idle_wr idle_rd outst_max
    node0.manager          64       64      1      1      2       0       1
    node1.subordinate      64       64      1      1      3       1       1
  Latency -- end-to-end (manager; min = best-case observed)
    signature                      n  min  mean max
    write node0->node1 len8 size3  1   42   42   42
    read  node0->node1 len8 size3  1   38   38   38
    histogram (cyc): [32,64)=2
    slave service @node1.subordinate: write 14  read 12
  NoC
    router      flit in_occ_max out_occ_max     link      flit stall
    req.R(0,0)   8       2          2            req_0to1   4    1
    req.R(1,0)   8       2          2            rsp_1to0   4    0
    rsp.R(1,0)   6       1          2
    rsp.R(0,0)   6       2          1
  2 transactions -> cosim/verilator/output/AX4-BAS-003/perf.json
```

## 6. Co-sim integration points

- `cosim/sv/axi_perf_monitor.sv` (new): passive AXI slot monitor; instantiate on
  each AXI interface in `tb_top.sv` -- the manager edges (`master -> NMU`) and the
  subordinate edges (`NSU -> slave`), per node. Counts byte / transaction /
  latency / the two AXI idle-cycle metrics (Section 3) per slot, with a per-id
  issue-cycle FIFO for same-id correctness. No production `AxiMaster` change (the
  `on_*_issued`-forwarding the first draft needed is dropped -- the SV monitor
  reads the wires).
- `cosim/sv/flit_link_perf_monitor.sv` (new): passive counter on the inter-router
  link wires (`tb_top.sv link_*`): flit count, and credit-counter-based stall
  (`valid && credit_count == 0`; credit is a pulse -- see Section 3). One per link
  direction.
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

## 7. Survive / replace (clean break, user decision 2026-06-18)

The user chose a **clean break**: the two-pass `zero_load` c_model bypass model
is deleted, not kept as a secondary view. The PMU co-sim becomes the only perf
path. Neutral primitives (observer, stats, link probe) survive because the PMU
reuses their logic and they back unrelated c_model unit tests.

| Current piece | Verdict |
|---|---|
| `NIPerfObserver` (`ni_perf_observer.hpp`) | **survive** -- neutral primitive for c_model unit tests; co-sim AXI latency re-implemented in the SV slot monitor (same per-id-FIFO logic, on AXI wires) |
| `PerfStats` (`perf_stats.hpp`) | **survive** -- neutral min/mean/max accumulator |
| Router/NI introspection getters | **survive** -- feed the C-side occupancy counters |
| flit-link decorators (LinkProbe, `flit_link_probe.hpp`) | **survive** -- c_model unit tests only; the co-sim uses the SV link monitor |
| `test_router_loopback.cpp` `ObserversAreNonIntrusive` | **survive** -- the non-intrusive A/B gate model (Section 4) |
| two-pass harness, `characterize_signature`, per-signature `zero_load` | **delete** (lives entirely in `test_perf_probe.cpp`) |
| `test_perf_probe.cpp` + `make perf` target | **delete** -- the superseded two-pass scenario suite |
| `PerfReport` (`perf_report.hpp`) + `test_perf_report.cpp` | **delete entirely** -- its core data model IS the deleted two-pass model (`zero_load_cyc` / `queueing_cyc` / `slave_remainder`); both reviewers flagged that preserving it carries the deleted concept forward. The new readout is a fresh counter-oriented struct in `perf_collector.hpp` matching the Section 5.1 schema |
| `docs/performance-probe.md` | **rewrite** for single-run in-fabric PMU (`make run-tb-top`, `perf.json`) |
| `docs/superpowers/plans/2026-06-17-perf-probe-rework.md` (untracked, never executed) | **delete** -- orphan plan for an abandoned approach |

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
| `c_model/tests/integration/test_perf_probe.cpp` | **delete** -- two-pass `zero_load` scenario suite (incl. `characterize_signature`, `test_perf_probe.cpp:719-740`) |
| root `Makefile` | **delete** the `perf` target (`Makefile:122` + the `PERF_DIR`/`PERF_ENV`/`PERF_FILTER`/`PERF_CMD` vars ~105-120) AND remove `perf` from the `.PHONY` list (`Makefile:20`). (There is no separate "help line" -- earlier draft was wrong.) |
| `c_model/tests/common/perf_report.hpp` | **delete** -- replaced by `perf_collector.hpp` (Section 7) |
| `c_model/tests/common/test_perf_report.cpp` | **delete** -- validates the removed `zero_load`/`queueing`/`slave_remainder` concepts |
| `c_model/tests/common/CMakeLists.txt` | remove `add_cmodel_test(test_perf_report)` (`CMakeLists.txt:38-40`) |
| `c_model/tests/integration/CMakeLists.txt` | remove the `test_perf_probe` target (`CMakeLists.txt:48-58`, incl. its POST_BUILD config copy) |
| `docs/performance-probe.md` | rewrite for the single-run in-fabric PMU |
| `docs/superpowers/plans/2026-06-17-perf-probe-rework.md` | **delete** -- orphan plan (never executed) |

## 8.1 Validation matrix

Every normative clause has a check (reviewer requirement). The latency-correlation
and stall counters carry the most risk, so they are tested directly.

| Check | What it proves |
|---|---|
| same-id, multiple outstanding | per-(id,dir) FIFO pops in issue order; a 2nd same-id issue does not overwrite the 1st |
| mixed read+write, same AXI id | read and write use separate `(id, dir)` FIFOs; no cross-direction mismatch |
| W-channel with no W-id | write-data latency attributes to the AW id in order |
| per-id FIFO overflow | exceeding max-outstanding-per-id fires the measurement-error assert (not a silent drop) |
| credit-stall sanity | a congested link reports `stall_cyc > 0`; an uncongested single-flow run reports `stall_cyc == 0` (guards the C2 pulse/credit-counter fix) |
| non-intrusive A/B | perf-enabled vs perf-disabled build: identical scoreboard result + identical per-txn completion cycles (Section 4) |
| schema conformance | emitted `perf.json` parses and carries every required key of Section 5.1 |
| min-observed labelling | the best-case field is named `latency_min` / "min observed", never `zero_load` |

## 9. References

- Kwon et al. / Ciordas et al. -- NoC monitoring (event-based, run-time).
- AXI-REALM, PULP platform (arXiv 2311.09662, 2501.10161; `pulp-platform/axi_rt`,
  SHL-0.51) -- AXI traffic monitoring, DOTQ id-correlation.
- AMD AXI Performance Monitor, PG037 -- per-AXI-interface counter set, latency
  definition (Address-Accept to RLAST for reads, to B-response for writes),
  sample interval.
- `docs/architecture.md` sec. 3.2 -- tick discipline (1 tick = 1 cycle).
