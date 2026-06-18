# In-fabric AXI/NoC performance monitor in the co-sim (PMU-style)

Date: 2026-06-18
Status: Draft (pending Codex review + user approval)
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
| AMD AXI Performance Monitor (PG037) | Per-AXI-interface "slot"; 6 profile counters/slot (write/read byte count, transaction count, latency count); latency = Address-Accept (AxVALID&AxREADY) to Last-Data (xLAST&xVALID&xREADY); sample-interval snapshots; AXI-ID filter; register readout. |

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

- Latency per transaction = `Last-Data cycle - Address-Accept cycle`, correlated
  by AXI id (issue stamps `start[id]`; completion computes `now - start[id]`).
  Write data (no W-channel id) is handled by ordering within the id, per the
  AXI-REALM W-table note.
- A global clock counter provides the cycle stamp.
- NoC router/link counters (Ciordas concept; not AXI-level, so not in PG037):
  per-router flit count, valid-but-not-ready and ready-but-not-valid stall
  cycles, output/input FIFO occupancy (sum + max), backpressure cycles.
- Sample window: single-run, default the whole scenario is one window. Optional
  `+perf_start`/`+perf_end` plusargs gate the measured window (warmup/drain),
  with no second pass.

## 4. Implementation: C + DPI (decision)

The monitors are C++ objects at the co-sim's c_model boundary, sampled and dumped
via DPI; `tb_top` drives them. Rationale:

- The NoC router/link counters MUST be C: the data lives in the c_model `Router`
  (existing `output_fifo_size`/`input_fifo_size` getters). A uniform C readout
  (one `PerfCollector`, one end-of-run dump) avoids mixing an SV `$fwrite` path
  with a C DPI path.
- The co-sim AXI master IS a c_model `AxiMaster` (inside `MasterShellAdapter`); its
  `on_*_issued`/`on_*_completed` callbacks give transaction identity + cycles
  directly, reusing the existing `NIPerfObserver` latency logic. Only a small
  forwarding addition is needed (Section 6).
- C + DPI monitors run inside the co-sim and dump at end -> `make run-tb-top`
  produces the log. This meets the in-fabric / from-the-real-run goal.

Considered alternative (rejected, but noted for the reviewer): an SV passive
`axi_perf_monitor.sv` on the AXI wires (closer to PG037 RTL, no DPI for the
monitor logic). Rejected because it splits readout into SV + C paths and does not
reuse `NIPerfObserver`; the AXI signals it would tap originate from the c_model
master anyway. If the project later wants a true RTL DUT, the SV monitor becomes
the right choice -- the counter structure here is identical, so the port is local.

## 5. Single run, no zero-load

- Report measured per-transaction latency (min/mean/max) + the counters above.
- No isolated Pass-1. The `min observed latency` field is the same run's minimum;
  it is labelled "min observed", not "zero_load".
- `queueing` (measured - ideal) is dropped unless an external nominal is provided
  and clearly labelled.

## 6. Co-sim integration points

- `MasterShellAdapter` + `AxiMasterStandalone`: forward `on_write_issued` /
  `on_read_issued` (currently not forwarded in the cosim path). ~6 lines.
- `RouterShellAdapter`: add `rsp_router()` accessor (only `req_router()` exists);
  expose `output_fifo_size`/`input_fifo_size` sampling.
- `NmuShellAdapter`/`NsuShellAdapter`: add accessors for `Rob`/`AxiMasterPort`
  occupancy if NI-buffer occupancy is wanted.
- The SV inter-router link (`tb_top.sv` `link_*` wires): a small passive SV
  monitor (valid/ready/flit count + stall), counters read at end via DPI or a
  hierarchical reference -- this is the one piece that is necessarily SV, because
  the link is an SV wire, not a C++ object.
- A `cmodel_perf_sample_tick()` DPI function called from `tb_top` each cycle (or a
  free-running counter sampled lazily), and a `cmodel_perf_dump(path)` DPI called
  at end-of-sim that writes `cosim/verilator/output/<scenario>/perf.json`.
- `make run-tb-top` already runs per scenario; the dump lands beside `run.log`.

## 7. Survive / replace

| Current piece | Verdict |
|---|---|
| `NIPerfObserver` (on_issue/on_complete latency) | survive -- it is the AXI latency monitor core |
| Router/NI introspection getters | survive (occupancy counters) |
| `PerfReport` JSON structure | survive, reframed to counters/measured (drop zero_load/queueing fields) |
| flit-link decorators (LinkProbe) | survive only for the c_model unit tests; not used in the co-sim path |
| two-pass harness, `characterize_signature`, per-signature `zero_load` | replace (single-run) |
| `test_perf_probe.cpp` + `make perf` (c_model scenario suite) | `[TBD]` keep as a c_model-level perf view, or retire once the co-sim monitor lands -- user decision |

## 8. Files (anticipated)

| File | Action |
|---|---|
| `c_model/include/cosim/axi_perf_monitor.hpp` (or similar) | new: per-AXI-slot latency/throughput/outstanding counters (reuses `NIPerfObserver`) |
| `c_model/include/cosim/perf_collector.hpp` | new: aggregates all slots + router/link counters; `dump(path)` JSON |
| `cosim/c/cmodel_dpi.cpp` | add `cmodel_perf_sample_tick` / `cmodel_perf_dump` DPI; forward AXI issue callbacks |
| `c_model/include/cosim/*_shell_adapter.hpp` | additive accessors (rsp_router, occupancy, issue-callback forwarding) |
| `cosim/sv/tb_top.sv` | instantiate the SV link monitor; call the sample/dump DPI |
| `cosim/verilator/Makefile` | ensure `run-tb-top` emits `output/<scenario>/perf.json` |

## 9. References

- Kwon et al. / Ciordas et al. -- NoC monitoring (event-based, run-time).
- AXI-REALM, PULP platform (arXiv 2311.09662, 2501.10161; `pulp-platform/axi_rt`,
  SHL-0.51) -- AXI traffic monitoring, DOTQ id-correlation.
- AMD AXI Performance Monitor, PG037 -- per-AXI-interface counter set, latency
  definition (Address-Accept to Last-Data), sample interval.
- `docs/architecture.md` sec. 3.2 -- tick discipline (1 tick = 1 cycle).
