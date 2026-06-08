# noc_project - AXI4 NoC c_model + cosim

A behavioural C++ model and Verilator co-sim of an AXI4 Network-on-Chip
Interface (NMU + NSU). The c_model passes IHI 0022H AXI4 conformity
scenarios; the cosim verifies a subset through wb2axip protocol checkers
under Verilator.

## Status

Research / alpha. Stage 5b in progress; behavioural c_model + Verilator
cosim. Run `make test` for the current pass count and skip reasons; the
cosim side SKIPs wb2axip-blocked scenarios with documented reason codes
(see `docs/architecture.md`).

## Architecture

~~~
AXI Master --> NMU --> [NoC fabric] --> NSU --> AXI Slave
              behavioural c_model in C++17
              Verilator cosim with wb2axip protocol check
~~~

### Where code lives

- `c_model/` - C++17 behavioural model + GoogleTest
- `cosim/` - Verilator wire-level cosim + wb2axip checker
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
make test                                    # c_model gtest suite
make check                                   # lint + build + tests
make sim                                     # default scenario via cosim
make sim SCENARIO=AX4-BUR-002_incr_8beat     # specific scenario
~~~

Multi-beat and multi-outstanding scenarios are SKIPped by the cosim
integration test (reason codes WB2AXIP_MULTI_BEAT etc.) and will fire
faxi_slave.v assertions if run through `make sim` directly. See
`docs/architecture.md` for wb2axip structural limits.

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
- `cosim/sv/wb2axip/` is ZipCPU/wb2axip (Apache 2.0), used with modifications
  (see `cosim/sv/wb2axip/ATTRIBUTION.md` for the diff)
