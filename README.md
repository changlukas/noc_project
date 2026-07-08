# noc_project - AXI4 NoC c_model + cosim

A behavioural C++ model and Verilator co-sim of an AXI4 Network-on-Chip
Interface (NMU + NSU). The c_model passes IHI 0022H AXI4 conformity
scenarios; the cosim runs them through a Verilator wire-level testbench
and checks results with the c_model scoreboard.

## Status

Research / alpha. Stage 5b in progress; behavioural c_model + Verilator
cosim. Run `make test` for the current pass count.

## Architecture

~~~
AXI Master --> NMU --> [NoC fabric] --> NSU --> AXI Slave
              behavioural c_model in C++17
              Verilator wire-level cosim with scoreboard check
~~~

### Where code lives

- `c_model/` - C++17 behavioural model + GoogleTest
- `sim/` - Verilator wire-level cosim
- `sim/test_patterns/` - AXI4 scenario tree (AX4-CAT-NNN_slug)
- `specgen/` - spec-to-header codegen sub-project
- `tools/` - repo-level tooling
- `docs/` - architecture + development guide

## Prerequisites

- CMake 3.20 or newer
- Verilator 5.036
- Python 3.9 or newer (with PyYAML)
- MSYS2 mingw64 toolchain (Windows host)

## Build

~~~bash
git clone <url> && cd noc_project
make build       # c_model + Verilator (correct dep order)
~~~

## Test

~~~bash
make test                                    # c_model gtest suite
~~~

## Simulate (cosim)

Cosim runs from the repo root via `make sim`; per-run logs land in
`sim/<simulator>/output/<run-tag>/run.log`:

~~~bash
make sim TB=mesh_4x4_vc1 PATTERN=neighbor        # directed cosim (Verilator)
make sim TB=mesh_4x4_vc8 PATTERN=uniform_random  # another topology / pattern
~~~

A self-contained wire-level smoke (random reads/writes, no stimulus files)
runs from each simulator's directory; add `FSDB=1` on VCS for a Verdi dump:

~~~bash
cd sim/verilator && make run-tb-top              # Verilator
cd sim/vcs       && make run-tb-top FSDB=1       # VCS (needs VERDI_HOME)
~~~

See `docs/architecture.md` for the cosim architecture, and
`docs/development.md` for the full build/run/waveform reference.

## Documentation

- [Architecture overview](docs/architecture.md)
- [Development guide](docs/development.md)
- [specgen sub-project](specgen/docs/guide/index.md)
- [Historical archive](docs/internal/_archive/README.md)

## Contributing

Branches target `main` via PR. Required before merging:

- `make test` clean (builds c_model + full ctest)
- `clang-format -i` on every C++ file touched
- Commit message format: `type(scope): description` (English)
- Never `--no-verify`

Detailed conventions and workflow: `docs/development.md`.

## License and third-party

No project-wide license has been selected. Until one is added,
project-owned material is not offered under an open-source license.

Vendored / derived material has its own license:

- `c_model/include/axi/` ported from cocotbext-axi (MIT); see
  `c_model/include/axi/ATTRIBUTION.md`
