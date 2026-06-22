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
- `cosim/` - Verilator wire-level cosim
- `tests/scenarios/` - AXI4 scenario tree (AX4-CAT-NNN_slug)
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
make test                                    # c_model gtest suite (+ cosim ctest if Vtb_top built)
make check                                   # lint + build + tests
~~~

## Simulate (cosim)

The root Makefile builds only. Simulation runs from each simulator's own
directory; run logs land in that directory's `output/<scenario>/run.log`:

~~~bash
cd cosim/verilator
make run-tb-top                                 # wire-level cosim, default scenario
make run-tb-top SCENARIO=AX4-BUR-002_incr_8beat # specific scenario
make run-genamba                                # gen_amba role-1 testbench (Tasks A-G)

cd cosim/vcs                                    # Linux workstation only (VCS)
make run-tb-top SCENARIO=<ax4-id>
make run-genamba
~~~

Waveform dumping is opt-in (default off, so regression is unaffected):

~~~bash
cd cosim/verilator && make run-tb-top SCENARIO=<id> TRACE=1  # VCD -> output/<id>/tb_top.vcd
cd cosim/verilator && make run-all-trace                     # one VCD per scenario + summary
cd cosim/vcs       && make run-tb-top SCENARIO=<id> FSDB=1   # FSDB (needs Verdi/VERDI_HOME)
cd cosim/vcs       && make run-all-fsdb                      # one FSDB per scenario + summary
~~~

See `docs/architecture.md` for the cosim architecture, and
`docs/development.md` for the full build/run/waveform reference.

## Documentation

- [Architecture overview](docs/architecture.md)
- [Development guide](docs/development.md)
- [specgen sub-project](specgen/docs/guide/index.md)
- [Historical archive](docs/_archive/README.md)

## Contributing

Branches target `main` via PR. Required before merging:

- `make check` clean (lint + build + tests)
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
