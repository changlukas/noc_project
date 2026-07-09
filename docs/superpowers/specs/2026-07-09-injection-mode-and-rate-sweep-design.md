# Injection mode and injection-rate sweep

Date: 2026-07-09
Status: Draft (brainstormed with user; booksim2 source survey + gem5/Garnet and literature survey; one Codex adversarial spec review, findings applied; pending user review)

## Goal

Expose **injection mode**, **injection rate** and **injection count** as run parameters of `make sim`,
and produce a latency-throughput curve per VC count from an injection-rate sweep.

## Motivation

The VC comparison this round needs the fabric to be the bottleneck. Three things currently stop that,
and a fourth makes the resulting numbers untraceable.

| # | problem | evidence |
|---|---|---|
| 1 | `make sim` has no injection-rate axis at all. Continuous injection lives only in a separate `run-traffic` recipe. | `Makefile:177-192`, `sim/verilator/Makefile:250-277` |
| 2 | `run-traffic` hardcodes `--pattern uniform_random`, and its tag omits the pattern. A second pattern would silently overwrite the first run's output. | `sim/verilator/Makefile:247,255` |
| 3 | The NSU's `max_unique_ids` (1 = collapse every manager onto one downstream ID) and `max_outstanding` (32) are compile-time constants unreachable from `sim/`. Both move the bottleneck off the fabric. | `src/c_model/include/wrap/wrap_defaults.hpp:20,25`; `grep -rn max_unique_ids sim/` returns nothing |
| 4 | The recorded `sim-saturation` series was invalidated because the configuration that produced it was not recorded with it. | `docs/backlog.md` |

## Reference: what booksim2 and gem5 call these things

| concept | booksim2 | gem5 / Garnet | here |
|---|---|---|---|
| spatial destination distribution | `traffic` (`booksim_config.cpp:158`) | `--synthetic` | `PATTERN` |
| per-cycle injection probability | `injection_rate` (`booksim_config.cpp:166`) | `--injectionrate` | `INJECTION_RATE` |
| injection bound | `batch_size` (sim_type=batch) | `--num-packets-max` | `INJECTION_COUNT` |
| drain-vs-no-drain measurement | `sim_type` (`booksim_config.cpp:229`) | absent | not ported, see Non-goals |

`PATTERN` keeps its name. Its values already are traffic patterns, `gen_test_patterns.py --pattern` is
used throughout, and renaming to `traffic` is churn without meaning.

Curve vocabulary follows the literature: x axis **offered injection rate**, y axis **accepted
throughput** (its ceiling is **saturation throughput**) or **average latency**. The plot is a
**latency-throughput curve** (GARNET paper, §3).

### Injection rate and checkability are orthogonal

Two independent surveys reached the same conclusion: every prior-art tool exposes injection rate
separately from stimulus discipline, never as one toggle. booksim has no checked mode at all; gem5's
synthetic traffic carries no payload to check.

This corrects a mistake in the first draft of this design. The `axi_scoreboard` is not invalidated by
pacing. It is invalidated by **read/write interleaving**: `user_node_endpoint.sv:298-304` collapses the
two-phase directed run into a single `fork`, so a read may precede the write to its address. The pulp
scoreboard backs absent addresses with X (`sim/dv/axi-0.39.7/src/axi_test.sv:2127-2130`) and warns on
the mismatch (`:2133-2142`).

The generator emits each read against the same address it just wrote (`gen_test_patterns.py:157-164`)
with full strobes (`:122-126`), so address reuse and partial-strobe holes are not contributing causes.
The single cause is AR overtaking its own write's commit.

A paced two-phase run is therefore a legal combination and would keep the scoreboard valid. It is not
built here (nothing needs it), but the reason the scoreboard is disabled must be recorded as
interleaving, not pacing, or the next reader will conclude that pacing breaks checkability.

## Decisions

| # | decision | rationale |
|---|---|---|
| **A** — injection mode is a run parameter, not a `RUN_CLASS` value | `RUN_CLASS` already decides which binary to compile (the `TB_DIRECTED` `ifdef`) and which recipe to run. Both injection modes share one binary, one `file_master`, one stimulus file set. Only the send loop differs. |
| **B** — `INJECTION_COUNT` has a mode-dependent default | Single mode checks data (4 per node, 64 transactions, fast enough for every regression). Continuous mode measures steady state (200 per node, so warm-up and drain do not dominate the window). One knob, two defaults, documented. A single default fails both ways: 4 gives a perf curve no steady state, 200 makes every directed regression 50x slower. |
| **C** — continuous mode disables the scoreboard explicitly | Today it runs and emits warnings that `sim/verilator/Makefile:270` deliberately does not gate on. A live checker whose output is ignored trains the reader to ignore it. Disabled, the run log is clean and any warning in it is real. |
| **D** — `max_unique_ids` and `max_outstanding` become plusargs, delivered through a new `cmodel_nsu_create_ex` | Both move the bottleneck off the fabric, and both must travel with the number in `result.csv`. There is **no** existing path: `cmodel_nsu_create(name, src_id, num_vc)` takes three arguments (`cmodel_dpi.h:130`), `gen_tb_top.py` emits that three-argument import (`:533-534`) and call (`:562`), and `gen_tb_top.py` reads only `+sam_config` today (`:545-551`). The repo's own precedent for widening a DPI create is a second symbol, not a changed one: `cmodel_nmu_create` and `cmodel_nmu_create_ex` coexist (`cmodel_dpi.h:95,97`). Do the same. The old three-argument `cmodel_nsu_create` survives untouched, so `tests/wrap/test_cmodel_dpi.cpp:65` and any hand-written testbench keep compiling. |
| **E** — each run writes its own `result.csv` | `collect_saturation.py` guesses the log path from a tag it did not build (`:26-28,43-44`), which is why adding a pattern breaks it. The run recipe knows its own tag. One row per run, six parameters plus the measurements. |
| **F** — no per-tile hop-count figure | It is computed statically from the pattern, not simulated, shares no data with the curve, and under `uniform_random` reduces to a mean over samples. `avg_hops` belongs as a CSV column, not a figure. This drops one of the two figures `docs/backlog.md` originally scoped. |

## Design

### Parameters

| parameter | range | default | consumed by |
|---|---|---|---|
| `PATTERN` | `neighbor` / `transpose` / `uniform_random` / `hotspot` | `neighbor` | `gen_test_patterns.py --pattern` |
| `INJECTION_MODE` | `0` single, `1` continuous | `0` | `+injection_mode` |
| `INJECTION_RATE` | `0.0` to `1.0` | `1.0`, only read when mode is 1 | `+injection_rate` |
| `INJECTION_COUNT` | positive integer, transactions per node | `4` when mode is 0, `200` when mode is 1 | `gen_test_patterns.py --transactions-per-node` |
| `MAX_UNIQUE_IDS` | `1` or `256` | `1` | `+max_unique_ids` |
| `MAX_OUTSTANDING` | positive integer | `32` | `+max_outstanding` |

```makefile
INJECTION_MODE  ?= 0
INJECTION_COUNT ?= $(if $(filter 1,$(INJECTION_MODE)),200,4)
```

`RUN_CLASS` keeps its two values, `directed` and `constrained_random`.

### The two injection modes

Both run inside the `TB_DIRECTED` binary, drive the same `axi_file_master`, and read the same stimulus
files. The send loop differs (`user_node_endpoint.sv:287-311`).

```
INJECTION_MODE=0 (single)            INJECTION_MODE=1 (continuous)
─────────────────────────            ────────────────────────────
fork run_aw; run_w; wait_b; join     fork
fork run_ar; wait_r; join              gated_run_aw; run_w; gated_run_ar
                                       wait_b; wait_r
two phases, writes complete first    join
                                     one phase, reads and writes interleave,
                                     each send gated per cycle on injection_rate
```

`INJECTION_COUNT` bounds the run. booksim needs `sim_type=throughput` to skip its drain because past
saturation the queues never empty (`trafficmanager.cpp:48`). Our run ends when `wait_b` and `wait_r`
drain, so a bounded injection count is what guarantees termination, not politeness.

### Checkers

| mode | checker | gate |
|---|---|---|
| 0 single | `axi_scoreboard` | zero mismatch, non-vacuous `PASS: all` |
| 1 continuous | `axi_bw_monitor` | run completes, non-zero bandwidth, zero `%Error` |

The scoreboard is constructed and armed unconditionally today (`user_node_endpoint.sv:251-256`). Under
mode 1, skip **both** `enable_all_checks()` and `monitor()`. Construction alone stores the interface and
clears `check_en` (`sim/dv/axi-0.39.7/src/axi_test.sv:2004-2014`); it hooks nothing. `monitor()` is what
forks the sampling tasks (`:2262-2280`), and it would still mutate the internal memory model even with
checks off, so skipping `enable_all_checks()` alone is not enough.

### NSU parameter path

All of this is new. Nothing on this path exists today.

**INPUT** `+max_unique_ids` and `+max_outstanding` plusargs.
**COMPUTE** the generated `tb_top` reads both with `$value$plusargs` beside the existing `+sam_config`
read (`gen_tb_top.py:545-551`), echoes them with `$display` so `run.log` records what actually ran, and
passes them to a new `cmodel_nsu_create_ex(name, src_id, num_vc, max_unique_ids, max_outstanding)`.
`NsuWrap::init` gains the two parameters, defaulted from `wrap_defaults.hpp`, and stops hardcoding them
(`nsu_wrap.hpp:69-70`). `Nsu` already forwards `max_unique_ids` to `Depacketize`, whose constructor
asserts it is 1 or `AXI_ID_SPACE` (`depacketize.hpp:50-51`).
**OUTPUT** every NSU in the fabric configured identically, and both values in `run.log`.

The old `cmodel_nsu_create` stays, forwarding the `wrap_defaults.hpp` values. That keeps
`tests/wrap/test_cmodel_dpi.cpp:65` and every ctest fixture compiling unchanged.

The six checked-in `sim/tb/tb_top_*.sv` files are generator output (`sim/tb/tb_top_mesh_4x4_vc1.sv:105`
declares the three-argument import, `:136` calls it). They are regenerated by the build and must be
regenerated and committed as part of this change.

### Per-run output

Tag: `<topology>_<pattern>_m<mode>_r<rate>_s<seed>`

```
sim/verilator/output/mesh_4x4_vc4_rob_uniform_random_m1_r0.4_s12345/
    run.log
    result.csv
```

`result.csv`, one header line and one row:

```
topology,vc,pattern,injection_mode,injection_rate,injection_count,seed,max_unique_ids,max_outstanding,avg_hops,accepted_bits_per_cycle,mean_latency
```

The recipe knows its own tag, so no path is guessed.

**`accepted_bits_per_cycle`**: sum the `BW` field of every `[Monitor ...][Read|Write]` line. Summing is
correct because every monitor shares one `cycle_cnt` window (`axi_bw_monitor.sv:140-144`).

**`mean_latency`**: a plain average of the 32 printed means is **wrong**. Each is already a mean over
that node-direction's own transaction count (`axi_bw_monitor.sv:105-121`), and those counts differ, so
the aggregate must be weighted. The count is not printed today. Add it:

```systemverilog
$display("[Monitor %s][Read] Latency: %0.2f +- %0.2f, N: %0d, BW: %0.2f Bits/cycle, Util: %0.2f%%",
         Name, read_latency_mean, read_latency_stddev, read_latency.size(), read_bw, read_util);
```

Then `mean_latency = sum(mean_i * N_i) / sum(N_i)`.

`axi_bw_monitor.sv` is vendored from FlooNoC (`sim/dv/floonoc-test/LICENSE`). Mark the file as locally
modified, as the licence requires.

The alternative — recovering `N_i` from the printed `Util`, which is proportional to the beat count —
holds only while the stimulus is single-beat (`--len 0`, `sim/verilator/Makefile:214`). Printing the
count costs one line and does not carry that assumption.

**`avg_hops`**: **new generator logic, does not exist today.** `gen_test_patterns.py` loads `x_dim` /
`y_dim` (`:367-374`) and has the coordinate helpers (`:175-177`), but computes no hop count and emits no
metadata (`:518-522`). Add: for each emitted transaction accumulate the XY Manhattan distance
`abs(dx) + abs(dy)` between source and destination coordinates, average over all transactions, and write
it to a sidecar `<stim_root>/pattern_info.json`. The recipe copies it into the row.

It is a property of the pattern and the topology, not a simulation result. It is carried so the curve's
shape can be explained, and it is the reason Decision F drops the separate hop-count figure.

### Sweep

`make sim-injection-sweep PATTERN=<p>` loops `make sim` over `vc1/vc2/vc4/vc8` at nine injection rates,
concatenates the `result.csv` rows, and plots.

```makefile
sim-injection-sweep:
	for vc in 1 2 4 8; do \
	  for r in 0.05 0.1 0.2 0.3 0.4 0.5 0.7 0.85 1.0; do \
	    $(MAKE) sim TB=tb_mesh_4x4_vc$${vc}_rob PATTERN=$(PATTERN) \
	        INJECTION_MODE=1 INJECTION_RATE=$$r \
	        MAX_UNIQUE_IDS=$(MAX_UNIQUE_IDS) MAX_OUTSTANDING=$(MAX_OUTSTANDING) || exit 1; \
	  done; \
	done
```

The sweep sets `MAX_UNIQUE_IDS ?= 256` for itself, because the shipped default of 1 serializes every
manager at each subordinate and would flatten the curve. `MAX_OUTSTANDING` it inherits, so the bring-up
step below can sweep it without editing the target.

The sweep knows nothing about any run. It knows how to call `make sim`. The Stage-5 matrix harness was
dropped as YAGNI on the same grounds and stays dropped.

### Figures

Both come from one CSV, four curves each (vc1/vc2/vc4/vc8), x axis offered injection rate.

1. **Accepted throughput**. The knee is saturation. VC value is how far right the knee moves.
2. **Average latency**. Divergence is the literature's definition of saturation.

The caption of each states `max_unique_ids` and `max_outstanding`, because both bound the result.

## Bring-up, before any figure is trusted

The previous injection-rate round turned on two bring-up findings, not on the sweep itself: the
`rand_slave` default `AX_MAX_WAIT=100` throttled responses to 1.2% utilisation, and a single AXI ID
serialised injection. Budget for the same here.

**Is the fabric the bottleneck?** Run `vc1` and `vc8` at `MAX_OUTSTANDING=32` and again at `256`. If the
two curves coincide at 32 but separate at 256, the NSU pool binds before the fabric does and the
headline figure must use a setting where the fabric binds. State the setting in the caption either way.

The NSU meta buffer change did cut NSU outstanding capacity. Before it, capacity was
`per_id_depth(16) x distinct ids in use`, up to 256 at a hotspot NSU. After it, one shared pool of 32
(`meta_buffer.hpp:52-56,70-74`; `wrap_defaults.hpp:20`). RoB Enabled admits up to 32 same-ID requests per
NMU (`rob.hpp:80`), so sixteen NMUs can offer far more than one NSU now accepts.

**That is a hypothesis, not a conclusion.** Three other finite resources sit on the same path and any of
them could bind first:

| resource | value | evidence |
|---|---|---|
| `AxiMasterPort` per-channel queue | 16 | `wrap_defaults.hpp:12`, gated at `nsu/axi_master_port.hpp:119-131` |
| router per-VC input depth | 4 | `specgen/source/constants.yaml:73-81` |
| router inject credit | per-VC | `router/router_adapters.hpp:48-53` |

The bring-up step measures which one binds. It does not assume.

## Non-goals

- No paced two-phase mode. Legal, and nothing needs it.
- No `sim_type` equivalent. `INJECTION_COUNT` bounds the run, so the drain always terminates.
- No `injection_rate_uses_flits`. Our injection unit is the AXI transaction.
- No per-tile hop-count figure (Decision F).
- No changes to `constrained_random`.

## Verification

| level | check |
|---|---|
| unit | `gen_test_patterns.py` emits `INJECTION_COUNT` transactions per node for each of the four patterns. Existing pytest, extended. |
| co-sim | Mode 0 on all four patterns, `mesh_4x4_vc1`: scoreboard clean, non-vacuous. This is today's behaviour and must not change. |
| co-sim | Mode 1 on all four patterns at `INJECTION_RATE=0.4`: non-zero bandwidth, zero `%Error`, **zero `Unexpected RData`** (the scoreboard is disarmed, so a warning means it was not). |
| co-sim | `MAX_UNIQUE_IDS=1` and `256`, and `MAX_OUTSTANDING=32` and `256`, reach the c_model. Verify by reading the values back out of `run.log`, not by assuming the plusarg was parsed. |
| sweep | One full `sim-injection-sweep` on `uniform_random`. The curve must show a knee. A flat line means the bottleneck is not the fabric, and the bring-up step above says what to do. |

Fault injection first, per project practice: pass `MAX_OUTSTANDING=1` and confirm throughput collapses.
A parameter that changes nothing when set to an absurd value was never wired.

## Files

| file | change |
|---|---|
| `Makefile` | `INJECTION_MODE` / `INJECTION_RATE` / `INJECTION_COUNT` / `MAX_UNIQUE_IDS` / `MAX_OUTSTANDING` pass-through into `sim/verilator/Makefile`; `sim-injection-sweep` |
| `sim/verilator/Makefile` | fold `run-traffic` into `run-directed`, gate branches on mode; retire `INJ_RATIO` / `TRAFFIC_TXNS` / `IDS_PER_TILE` / `TRAFFIC_TAG`; emit `result.csv` |
| `sim/tb/user_node_endpoint.sv` | `+injection_mode` replaces the `+traffic_inj_ratio` presence test; skip `enable_all_checks()` and `monitor()` under mode 1 |
| `sim/dv/floonoc-test/axi_bw_monitor.sv` | print the latency sample count; mark the file locally modified per its licence |
| `sim/tools/gen_tb_top.py` | read and echo `+max_unique_ids` / `+max_outstanding`; emit the `cmodel_nsu_create_ex` import and call |
| `sim/tb/tb_top_*.sv` (6 files) | regenerate and commit; they carry the DPI import (`tb_top_mesh_4x4_vc1.sv:105,136`) |
| `src/dpi/cmodel_dpi.{h,cpp}` | add `cmodel_nsu_create_ex`; leave `cmodel_nsu_create` intact |
| `src/c_model/include/wrap/nsu_wrap.hpp` | `init` gains two parameters, defaulted from `wrap_defaults.hpp`; stop hardcoding at `:69-70` |
| `sim/tools/gen_test_patterns.py` | compute mean XY Manhattan hop count, emit `pattern_info.json` |
| `sim/tools/collect_saturation.py` | delete |
| `sim/tools/plot_saturation.py` | replace with `plot_injection_sweep.py` |
| `docs/development.md`, `docs/backlog.md` | reconcile |

`tests/wrap/test_cmodel_dpi.cpp` needs no change: it calls the three-argument `cmodel_nsu_create`, which
survives.
