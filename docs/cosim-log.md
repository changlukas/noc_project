# Reading co-sim logs

Full per-run log: `sim/<simulator>/output/<run-tag>/run.log`. `make sim` echoes
the per-node monitor block plus the verdict to the terminal.

## Per-node bandwidth monitor

One `[Read]` + `[Write]` pair per node, printed at end of simulation by the
FlooNoC `axi_bw_monitor` (`sim/dv/floonoc-test/axi_bw_monitor.sv`, imported
unmodified). Tapped on each node's AXI master bus.

```
[Monitor node0.master][Read] Latency: 72.00 +- 31.30, N: 4, BW: 4.11 Bits/cycle, Util: 1.61%
```

| field | meaning | computed as |
|---|---|---|
| `node0.master` | node id + the master (AXI manager) bus | `Name` set per endpoint |
| `[Read]` / `[Write]` | AR to R channel / AW+W to B channel | one line each |
| `Latency: 72.00` | mean per-transaction round-trip, in **cycles** | `cycle(response) - cycle(request)`, averaged (`axi_bw_monitor.sv:96,101`) |
| `+- 31.30` | standard deviation across that node's own transactions | spread from queueing and contention, not from multiple destinations |
| `N: 4` | number of latency samples the mean and stddev are over | that channel's completed transaction count (`axi_bw_monitor.sv:150,152`) |
| `BW: 4.11 Bits/cycle` | accepted throughput on that channel | `beats * data_width / total_cycles` (`:141-142`) |
| `Util: 1.61%` | how busy the data channel was | `beats * 100 / total_cycles` (`:143-144`) |

BW and Util are the same measurement in two units: `BW = (Util/100) * data_width`
(here `0.0161 * 256 = 4.11`). Util is the fraction of bus peak (one beat every
cycle). BW is the absolute bits per cycle.

## Verdict lines

| line | meaning |
|---|---|
| `PASS: all 16 nodes done, non-vacuous` | every node finished its stimulus and did real work (not a zero-transaction pass). From the tb. |
| `DIRECTED PASS: ... scoreboard clean, non-vacuous` | directed axis, the master-face `axi_scoreboard` saw no data mismatch. From the `run-directed` recipe. |
| `DIRECTED FAIL` | a mismatch, an assertion, or the run never reached the non-vacuous pass. |

## Reading the numbers

- **Directed runs show low Util (~1-2%) by design.** They inject a few
  transactions per node against a fast slave to check correctness (scoreboard),
  not to load the fabric. Read PASS and Latency, ignore Util. For throughput use
  the injection-rate sweep (`make sim-injection-sweep PATTERN=<p>`, mode 1
  paced by `INJECTION_RATE`); see
  [verification-environment.md](verification-environment.md#injection-rate).
- **Latency tracks hop distance.** The `neighbor` pattern is booksim2's
  NeighborTrafficPattern (`dst = (x+1, y+1) mod dim`), a diagonal shift with
  wrap, not a single hop. On a mesh (no torus links) the wrap routes edge nodes
  back across the array, so a 4x4 shows three tiers: interior 2 hops, one-axis
  wrap 4 hops, corner 6 hops.

## See every node on the CLI

`make sim` prints the whole monitor block. To re-read or sort a past run:

```
grep -F '[Monitor' sim/verilator/output/<run-tag>/run.log
```
