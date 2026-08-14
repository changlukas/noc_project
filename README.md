# noc_project - AXI4 NoC c_model + cosim

A behavioural C++ model of an AXI4 Network-on-Chip interface (NMU, NSU,
router mesh) with a Verilator wire-level co-simulation. The cosim drives
generated traffic through a per-topology testbench and checks every
transaction with a write-to-readback scoreboard plus AXI protocol
assertions.

## Status

Internal engineering release. Build, unit tests, and directed cosim are
verified end-to-end by a documentation-driven dry run for this release on
the platforms below. `make test` reports the current unit suite;
`make help` lists the main targets.

## Architecture

~~~
AXI4 Master --> NMU --> router mesh --> NSU --> AXI4 Slave
                C++17 model, one SV wrap per component (DPI handle ABI)
                generated per-topology tb_top + scoreboard
~~~

### Where code lives

- `ref_model/` - the reference model, what `rtl/` is checked against
  - `c_model/` - C++17 model (`axi`, `nmu`, `nsu`, `router`, `wrap`) + GoogleTest suites
  - `top/` - SV wrapper modules around the model components
  - `dpi/` - DPI bridge between SV wraps and the C++ model
- `rtl/` - the synthesizable implementation, per block
- `sim/` - testbench sources, topology YAMLs, stimulus/plot tooling, `verilator/` and `vcs/` flows
- `specgen/` - spec-to-code generator (C++ headers + SV packages)
- `docs/` - spec, trade-off record, verification environment

## Prerequisites

- CMake 3.20 or newer, GCC with C++17, GNU make
- Verilator 5.048 (primary, WSL); 5.036 also works (the Makefile carries its workarounds)
- Python 3 with PyYAML, pytest (specgen drift gate)
- GoogleTest and yaml-cpp are fetched by CMake; no system install

| Platform | Scope | Verified |
|---|---|---|
| Linux (WSL Ubuntu, Verilator 5.048) | build + ctest + cosim | full flow |
| Linux workstation (VCS) | testbench build | declared |

Linux only. The build used to carry a native-Windows path (MSYS2 mingw64) and
it is gone: it doubled every toolchain behaviour, which is how a test file that
compiled under one and not the other went unnoticed.

On WSL, work from a native-filesystem copy of the repo; `/mnt/*` mounts
are slow and unreliable under parallel builds. Per-host settings
(`BUILD_ROOT`, `PYTHON3`, `VERILATOR`, `CMAKE`) go in a gitignored
`local.mk` at the repo root. Offline hosts: unpack pre-fetched
`googletest-src/` and `yaml-cpp-src/` into `~/noc_offline_deps`; the
build auto-detects it and configures fully disconnected.

## Build

~~~bash
git clone <url> && cd noc_project
make build       # c_model (CMake) + Verilator testbench, correct dep order
~~~

Simulation-only setups can skip the c_model test binaries: `make
build-verilator` builds the yaml-cpp library and the Verilator testbench
and nothing else. `make build` (or `make build-cmodel`) is needed only
before `make test`.

Artifacts land under `$(BUILD_ROOT)` (gitignored), which defaults to
`build/` in the repo; `make clean` removes them. On WSL set
`BUILD_ROOT := $(HOME)/noc_build` in `local.mk` — a build under `/mnt/`
produces a Windows-COFF yaml-cpp that the Linux linker rejects. The CMake
cache records the source directory it was configured against, and
`make build-cmodel` honours whatever the cache names, so after moving the
checkout delete the cache rather than editing it, and confirm with
`grep CMAKE_HOME_DIRECTORY $BUILD_ROOT/cmodel/CMakeCache.txt`.

## Test

~~~bash
make test                                  # c_model ctest suite
make pytest                                # specgen + sim/tools suites, golden drift gate
python3 specgen/tools/codegen.py --check   # committed generated code matches sources
~~~


## Simulate (cosim)

`make -C sim` builds the chosen topology and runs one directed pattern.

| var | values |
|---|---|
| `TB` | topology YAML name from `sim/topologies/`: `mesh_2x2_vc1`, `mesh_2x2_vc1_periph`, `mesh_4x4_vc1`, `mesh_4x4_vc2`, `mesh_4x4_vc4`, `mesh_4x4_vc8`. Every node gets a 4 GiB memory tile and a 4 KB config tile. The `_periph` map adds an off-mesh peripheral at a border route coordinate outside the tile region |
| `PATTERN` | `neighbor`, `transpose`, `bit_complement`, `bit_reverse`, `shuffle`, `bit_rotation`, `tornado` (the booksim2 permutation set; the bit permutations need a power-of-two node count, `transpose` and `tornado` a square mesh), `uniform_random`, `all_to_all` (each node walks every other node in turn, so the destination changes on every transaction and, at one id per initiator, every one of them allocates a reorder-buffer slot), `hotspot` (`HOTSPOT=` names the target node), `beat_exact` (per-lane-distinct bytes + walking WSTRB, DPI word-boundary check), `multicast` (collective write, shape from `MCAST_SHAPE`) |
| `MCAST_SHAPE` | `row` (default), `col`, `submesh`. `multicast` only. One shape per run, concurrent multicast trees must stay pairwise disjoint |

`SEED` unset draws and prints a random seed; pass `SEED=<n>` to replay
a run.

~~~bash
make -C sim TB=mesh_4x4_vc1 PATTERN=neighbor
make -C sim TB=mesh_4x4_vc8 PATTERN=transpose
~~~

On success the make wrapper prints `DIRECTED PASS: <run-tag> scoreboard
clean, non-vacuous` to the console. The full log at
`sim/verilator/output/<run-tag>/run.log` ends with `PASS: all N nodes
done, non-vacuous` and carries per-node `[Monitor nodeN.master]`
latency/bandwidth lines and `[HWM]` NMU sizing telemetry (RoB slot,
order-list and outstanding-pool high-water marks plus the admission clause
split).

### Injection modes

`INJECTION_MODE` selects the run shape:

| mode | shape | checking |
|---|---|---|
| `0` (default) | two-phase: writes drain, then reads | scoreboard armed |
| `1` | continuous: reads and writes interleave, both paced per cycle by `INJECTION_RATE` (0.0 to 1.0) | scoreboard disarmed (write-before-read fails); bandwidth monitor gates, `result.csv` emitted |
| `2` | checked-continuous: writes paced as mode 1, each read issues after its paired write's B response | scoreboard armed |

Mode 1 measures bandwidth and latency without data checking. Mode 2
checks data integrity under continuous write load; its read stream
couples to write response latency, so it does not measure offered
injection rate and mode 1 stays the saturation-curve instrument.

| var | default | meaning |
|---|---|---|
| `INJECTION_RATE` | `1.0` | per-cycle injection probability; `0.0` never injects and the run ends at the testbench watchdog |
| `INJECTION_COUNT` | `200` (modes 1, 2), `64` (mode 0) | transactions per node |
| `IDS_PER_INITIATOR` | generator default (1) | distinct AXI ids one initiator draws from |
| `HOTSPOT` | `5` | target node for the `hotspot` pattern |
| `HOTSPOT_PERIPHERALS` | unset | `1` aims `hotspot` at the peripherals instead of a tile |
| `READ_ROB` | `1` | NMU read response path: `1` the reorder buffer, `0` the RoBless bypass |
| `BURST_LEN` | `0` | AXI `len` for the generated stimulus; `0` is a single beat. At `--size 5` (32 B/beat) `63` gives 64 beats = 2048 B, inside the 4 KB boundary |
| `MAX_UNIQUE_IDS` | `NSU_META_BUFFER_MAX_UNIQUE_IDS_DFLT` | NSU meta buffer: distinct upstream ids tracked at once |
| `MAX_OUTSTANDING` | `NSU_META_BUFFER_*_DFLT` | NSU meta buffer: outstanding entries |
| `B_ROB_DEPTH`, `R_ROB_DEPTH` | `NMU_ROB_*_DFLT` | NMU reorder-buffer pool depth per direction. Both ≤ 256 — `ordering_tag` is 8 bits |
| `MAX_TXNS_PER_ID` | tb local | per-AXI-id order-list depth (FlooNoC `MaxRoTxnsPerId`, `floo_rob.sv:12`) |

Leaving one of the last four unset passes no plusarg, so the testbench keeps
its `ni_params_pkg` default. That is not the same as passing the default
value — it is what the shipped parameter set actually builds.

Fault injection, for proving a checker fires rather than assuming it does:

| var | effect |
|---|---|
| `MCAST_FAULT=1` | corrupts one multicast replica so the collective checker must fire |
| `DECERR_FAULT=1` / `DECERR_FAULT_WR=1` | forces a DECERR on the read / write path |
| `TIMEOUT_CYCLES=<n>` | fires the watchdog early, so a hang dumps per-node outstanding and `last_progress` instead of waiting out the formula |
| `ELABORATE_ONLY=1` | elaborates and stops, without running |

~~~bash
make -C sim TB=mesh_4x4_vc4 PATTERN=uniform_random INJECTION_MODE=1 INJECTION_RATE=0.3
make -C sim TB=mesh_4x4_vc4 PATTERN=uniform_random INJECTION_MODE=2 INJECTION_RATE=0.5
~~~

On success mode 1 prints `CONTINUOUS PASS: <run-tag>` and writes
`sim/verilator/output/continuous_<topo>_rob<READ_ROB>_<pattern>_r<rate>_s<seed>/result.csv`
with the monitor's bandwidth and latency numbers; mode 2 prints
`CHECKED PASS: <run-tag> scoreboard clean, non-vacuous` with run tag
`checked_<topo>_rob<READ_ROB>_<pattern>_r<rate>_s<seed>`.

`make -C sim sim-injection-sweep PATTERN=<p>` runs the full saturation sweep
(VC configs 1/2/4/8, nine rates each, overridable via `SWEEP_VCS` and
`SWEEP_RATES`), then merges every `result.csv` and plots
`sim/tools/injection_sweep.png`. The sweep rebuilds Verilator once per
VC config; expect a long run.

### DMA endpoint

`DMA=1` selects a separate top that puts a pulp iDMA backend on every node's
master face, in place of the stimulus replayer. The point is conformance: the
replayer issues transactions this project chose, while a real AXI manager picks
its own bursts, outstanding depth and alignment.

`DMA=1` ignores `PATTERN`. Stimulus is `sim/tools/gen_dma_jobs.py`'s per-node
job files, and the run ends when every DMA has retired every job it was given
and every destination region matches its source byte for byte.

| var | default | meaning |
|---|---|---|
| `DMA_RW` | `read` | direction for the whole run. `read` sources from the neighbour's window, `write` from the node's own. One direction per run, as upstream does it |
| `DMA_JOBS_PER_NODE` | `100` | jobs each node issues |
| `DMA_LENGTH` | `0x400` | bytes per job. Capped at `0xFFFFF` by `TF_LEN_WIDTH`; a larger value is rejected rather than truncated |

The three defaults are FlooNoC's own (`util/gen_jobs.py`: `num_wide_bursts`,
`wide_burst_length` × 512 b, run-level `--rw`). Concurrency comes from many
back-to-back same-direction jobs, not from large ones — a job under 4 KB is one
burst, so alternating direction or lowering the count is what suppresses
outstanding transactions.

All three feed the job files **and** the generated top's compare, so they cannot
disagree. Each direction gets its own stimulus directory and run tag, so a read
run and a write run do not overwrite one another.

~~~bash
make -C sim TB=mesh_4x4_vc1 DMA=1                 # 100 read jobs per node
make -C sim TB=mesh_4x4_vc1 DMA=1 DMA_RW=write
~~~

On success the wrapper prints `DMA PASS: <run-tag> every job retired, every
region intact`. `DECERR_FAULT=1` and `DECERR_FAULT_WR=1` work here too.

## Regenerate

Packet, signal, and parameter definitions are single-sourced in
`specgen/`; generated headers and packages are committed and
drift-gated at build time.

~~~bash
python3 specgen/tools/codegen.py --target cpp --domain packet     # domains: packet, signals, params, noc_types
python3 specgen/tools/codegen.py --target sv --domain noc_types --num-vc 4
python3 specgen/tools/codegen.py --check
~~~

## Documentation

- [NMU design spec](docs/nmu-spec.md)
- [NSU design spec](docs/nsu-spec.md)
- [Router design spec](docs/router-spec.md)
- [Response-ordering trade-off record](docs/trade-off.md)
- [Verification environment](docs/verification-environment.md)
- [Known limitations](docs/known-limitations.md)

## Contributing

Feature branches target `main` via PR. Required before merging:

- `make test` clean (builds c_model + full ctest)
- `clang-format -i` on every C++ file touched
- Commit message format: `type(scope): description` (English)
- Never `--no-verify`

## License

Proprietary and confidential, internal use only. No license is granted
without written permission of the copyright holder. See `LICENSE`.
