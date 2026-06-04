# OSS Attribution — cosim/sv/wb2axip/

Verbatim copies of ZipCPU/wb2axip formal AXI4 protocol checker properties,
used as simulation-runtime observers via Verilator (immediate-assertion
subset compatible). Pattern matches `c_model/include/axi/ATTRIBUTION.md`.

## Upstream

- Source: https://github.com/ZipCPU/wb2axip
- Commit: 2e8d3bc2d26ddc33d1881022a2a2b9d3f0c16b9b
- License: Apache-2.0
- Files: `bench/formal/faxi_master.v`, `bench/formal/faxi_slave.v`

## Files

| This repo                              | Upstream path                       |
|----------------------------------------|--------------------------------------|
| `cosim/sv/wb2axip/faxi_master.v`       | `bench/formal/faxi_master.v`         |
| `cosim/sv/wb2axip/faxi_slave.v`        | `bench/formal/faxi_slave.v`          |

## Modifications

- No source-file modifications; the two `.v` files are byte-identical to upstream.
- Adaptation provided externally via `cosim/sv/wb2axip/sim_wrapper.svh`:
  - `` `define SLAVE_ASSUME assert `` (formal-only `assume` reinterpreted as
    `assert` in simulation; upstream's macro indirection lets us redefine
    the role mapping at include time).
  - Plain `assume(...)` calls inside the file are similarly mapped via a
    project-wide preprocessor option in the Verilator Makefile (`+define+assume=assert`).
  - `f_past_valid` is initialized to 0 in an `initial` block defined in the
    sim_wrapper to guard against Verilator default-X read on the first cycle.
