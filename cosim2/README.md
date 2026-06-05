# cosim2/ — Stage 5b DPI wire-wrap co-sim

This tree houses the Stage 5b co-simulation infrastructure where each c_model
component is wrapped in its own SystemVerilog shell module, communicating with
its peers via real registered SV wires (β tick discipline, 1-cycle latency per
hop).

See `docs/superpowers/specs/2026-06-05-stage5b-dpi-wire-wrap-design.md` for the
full design rationale and `docs/superpowers/plans/2026-06-05-stage5b-dpi-wire-wrap.md`
for the implementation breakdown.

## Branch lineage

- Branched off Stage 5a tip (commit `0a8849c` at the time of brainstorm; current
  branch HEAD includes the Stage 5b spec + plan commits added afterward).
- Stage 5a artifacts under `cosim/` were deleted on this branch in the first
  dedicated commit; wb2axip vendored files were moved to `cosim2/sv/wb2axip/`
  preserving git history.
- After Stage 5b is stable, plan to rename `cosim2/` → `cosim/` and drop the
  Stage 5a `cosim/` history reference (separate clean rename commit).

## Layout

- `sv/` — 5 SV wrap modules, 3 interface defs, top-level TB, vendored wb2axip
- `c/` — DPI bridge + error code enum + boundary try/catch macros
- `verilator/` — build script + harness main.cpp
- `tests/fixtures/` — 5 YAML scenarios (3 carried + 2 multibeat + 1 injection)
- `tests/` — GoogleTest entries hooked into ctest

## Coding discipline

See `CODING_DISCIPLINE.md`. All `.sv` files conform to rtl-style skill.
