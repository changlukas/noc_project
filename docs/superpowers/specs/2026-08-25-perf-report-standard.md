# Performance report aligned to the textbook method

Replaces `sim/verilator/output/sweep_summary.md` (Scenarios 1 to 5) and the tooling behind it.
Source of the method: On-Chip Networks 2nd ed. ch 1 and ch 7, booksim2 `trafficmanager.cpp`,
Codex survey 2026-08-25. No DUT change.

## Metrics

| Metric | Definition | Unit |
|---|---|---|
| Offered load | injection probability per node per cycle times packet size | flits per node per cycle, and B per node per cycle |
| Accepted throughput | bytes the AXI master handshakes per cycle, per node, read plus write | B per node per cycle, and flits |
| Network latency `nlat` | AX handshake at the master to last response beat (today's monitor value) | cycles |
| Packet latency `plat` | intended issue time (open loop source queue) to last response beat, `plat = nlat + source queue delay` | cycles |
| Zero-load latency | `plat` at the lowest sweep point, offered 0.067 flits per node per cycle (`p` = 0.001 per channel) | cycles |
| Saturation throughput | offered load at which `plat` reaches 3 times zero-load (textbook rule) | flits per node per cycle |
| Accepted at max load | accepted throughput at the highest offered load, booksim's accepted rate at saturation | flits per node per cycle |
| Ideal throughput | 1 / max channel load for the pattern under XY on the mesh, analytic | flits per node per cycle |
| Avg hops | mean XY hop count of the pattern, analytic | hops |

Flit counting: two bases, never mixed in one column. Network flits count every flit on DAT
(AW header, W beats, R beats): at AxLEN 32 a write is 34 flits, a read 33. Payload flits count
W and R beats only. all_to_all excludes self traffic and uniform_random permits it, per the
generator; the analytic script follows the generator. Ideal throughput and link
utilization use network flits. Accepted bytes count the full 64 B bus per beat, as the third
party monitor does (`$bits(r.data)` in `axi_bw_monitor.sv`), so at AxSIZE 5 they equal payload
and at narrower sizes they overstate it. Offered bytes charge the same, so the two compare.

## Open loop injection

booksim2 keeps a per source `_qtime` advanced by the injection process independent of the
network and stamps each packet with it (`trafficmanager.cpp:922-945, :863`), then reports `plat`
from that stamp and `nlat` from the actual injection (`:731-732`). The testbench does the same:

| Point | Rule |
|---|---|
| `qtime` | per node, per channel (AW, AR), advanced every cycle by the Bernoulli trial at `injection_rate`, never blocked by `awready` or `arready` |
| Issue | the next AX is sent when `qtime` has passed its slot and the channel is ready |
| Source queue delay | AX handshake cycle minus the slot's `qtime`, recorded per transaction |
| Report | per node at end of sim: `[SrcQueue nodeN][Read] mean: <float>, N: <int>` and `[Write]` |
| Outstanding | unchanged, NMU `max_txns_per_id` 32 is the network's backpressure, it becomes queue delay |
| Run length | fixed 200 transactions per node. At AxLEN 32 the sweep rates are at most `p` = 0.018, so the offered window is at least 11000 cycles, stated in Method. Above saturation `plat` grows with run length, which is the expected signature |

`sim/dv/floonoc-test/axi_bw_monitor.sv` is not modified. `emit_result_csv.py` adds the source
queue delay to the monitor mean, sample weighted, into `mean_latency_open`, and keeps the monitor
mean as `mean_latency_network`.

## Analytic pattern metrics

`sim/tools/pattern_metrics.py`, importing the destination functions of `gen_test_patterns.py`:
for every source node enumerate its destinations (uniform_random and all_to_all: every other
node with equal weight, hotspot: its hotspot set), walk XY, count load per directed link, report
avg hops, max channel load and ideal throughput = 1 / max load. Checked against the textbook
mesh value, max injection = 4 / k for uniform random on a k by k mesh. Self traffic follows the
generator's default (permitted).

## Report

One generator, `sim/tools/perf_report.py <output dir>`, writes `sim/verilator/output/perf_report.md`:

| Section | Content |
|---|---|
| 1 Method | topology, router pipeline (REQ/RSP 2 cycles, DAT head 4 then one per cycle), flow control, NI parameters, injection model (open loop, Bernoulli, fixed count), seeds, latency definitions, flit bases |
| 2 Zero-load latency | narrow and data read and write: measured `plat`, per stage decomposition, ideal fabric `H t_wire + L/b`, gap |
| 3 Latency vs offered load | per pattern table (and a plot if matplotlib is present): offered load, accepted, `nlat`, `plat`, seed spread; the 3x point marked |
| 4 Pattern summary | per pattern: avg hops, ideal throughput, saturation throughput (3x), accepted at max load, percent of ideal, zero-load `plat` |
| Appendix A | link utilization avg min max per pattern at the highest offered load |
| Appendix B | parameter sensitivity single points: vc8, router VC depth 16 and 32, NI RX depth |

Runs are grouped by their parameter tuple from `result.csv` as today. Patterns with a full
curve: uniform_random, tornado, shuffle, bit_complement, bit_reverse, transpose (textbook Table
7.2 set). Offered load 0.067 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1.0 1.2 flits per node per cycle (the Bernoulli rate per channel is offered / 67 at AxLEN 32), seed 1, plus seeds 2 and 3 at the two
rates around the 3x saturation point. The other four patterns (neighbor, bit_rotation, all_to_all, hotspot)
appear in section 4 from their rate 0.9 runs with a note.

## Removed

`sim/tools/summarize_results.py` and its test, `sim/tools/plot_injection_sweep.py`,
`sim/verilator/perf_cli_summary.py`, `sim/verilator/output/sweep_summary.md`,
`sweep_summary_mesh_2x2.md`, the scratch `scenario2.md` and `scenario3.md`, the sed based
assembly command, the `plot_injection_sweep.py` call in `make sim-injection-sweep`. The Scenario
2 congestion probe and the depth sweeps stay documented in `docs/backlog.md` only.
