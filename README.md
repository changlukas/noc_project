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

- `src/c_model/` - C++17 model (`axi`, `nmu`, `nsu`, `router`, `wrap`) + GoogleTest suites
- `src/sv/` - SV wrapper modules around the model components
- `src/dpi/` - DPI bridge between SV wraps and the C++ model
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
| Linux (WSL Ubuntu, Verilator 5.048) | build + ctest + cosim | full flow (dry-run verified) |
| Windows 11 + MSYS2 mingw64 (Verilator 5.036) | build + ctest | declared |
| Linux workstation (VCS) | testbench build | declared |

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

Artifacts land under `build/` (gitignored); `make clean` removes them.

## Test

~~~bash
make test                                  # c_model ctest suite
make pytest                                # specgen + sim/tools suites, golden drift gate
python3 specgen/tools/codegen.py --check   # committed generated code matches sources
~~~

On Windows, invoke Python scripts with `py -3` instead of `python3`.

## Simulate (cosim)

`make sim` builds the chosen topology and runs one directed pattern.

| var | values |
|---|---|
| `TB` | topology YAML name from `sim/topologies/`: `mesh_2x2_config_narrow_vc1`, `mesh_2x4_config_narrow_vc1`, `mesh_2x4_vc1`, `mesh_4x4_vc1`, `mesh_4x4_vc2`, `mesh_4x4_vc4`, `mesh_4x4_vc8`; append `_rob` for the reorder-buffer variant. Every node gets a 1 MB memory tile; the two `*_config_narrow_*` maps add a 4 KB config tile beside it |
| `PATTERN` | `neighbor`, `transpose`, `uniform_random`, `hotspot` (`transpose` needs a square power-of-two mesh), `beat_exact` (per-lane-distinct bytes + walking WSTRB, DPI word-boundary check) |

`SEED` unset draws and prints a random seed; pass `SEED=<n>` to replay
a run.

~~~bash
make sim TB=mesh_4x4_vc1 PATTERN=neighbor
make sim TB=mesh_4x4_vc8_rob PATTERN=transpose
~~~

On success the make wrapper prints `DIRECTED PASS: <run-tag> scoreboard
clean, non-vacuous` to the console. The full log at
`sim/verilator/output/<run-tag>/run.log` ends with `PASS: all N nodes
done, non-vacuous` and carries per-node `[Monitor nodeN.master]`
latency/bandwidth lines and `[HWM]` R-RoB slot high-water marks.

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
| `INJECTION_COUNT` | `200` (modes 1, 2), `4` (mode 0) | transactions per node |
| `IDS_PER_TILE` | generator default (1) | distinct AXI ids per tile |
| `HOTSPOT` | `5` | target node for the `hotspot` pattern |

~~~bash
make sim TB=mesh_4x4_vc4_rob PATTERN=uniform_random INJECTION_MODE=1 INJECTION_RATE=0.3
make sim TB=mesh_4x4_vc4_rob PATTERN=uniform_random INJECTION_MODE=2 INJECTION_RATE=0.5
~~~

On success mode 1 prints `CONTINUOUS PASS: <run-tag>` and writes
`sim/verilator/output/continuous_<topo>_<pattern>_r<rate>_s<seed>/result.csv`
with the monitor's bandwidth and latency numbers; mode 2 prints
`CHECKED PASS: <run-tag> scoreboard clean, non-vacuous` with run tag
`checked_<topo>_<pattern>_r<rate>_s<seed>`.

`make sim-injection-sweep PATTERN=<p>` runs the full saturation sweep
(VC configs 1/2/4/8, nine rates each, overridable via `SWEEP_VCS` and
`SWEEP_RATES`), then merges every `result.csv` and plots
`sim/tools/injection_sweep.png`. The sweep rebuilds Verilator once per
VC config; expect a long run.

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
- [specgen sub-project guide](specgen/docs/guide/index.md)

## Contributing

Feature branches target `main` via PR. Required before merging:

- `make test` clean (builds c_model + full ctest)
- `clang-format -i` on every C++ file touched
- Commit message format: `type(scope): description` (English)
- Never `--no-verify`

## License

Proprietary and confidential, internal use only. No license is granted
without written permission of the copyright holder. See `LICENSE`.
