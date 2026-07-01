# A′ — Verilator 5.048 migration + pulp AXI SV BFM

Goal: replace the C++ AXI master/slave with SystemVerilog behavioral BFMs ported from
pulp-platform/axi (`axi_test.sv`); keep the memory in C++ (reached from the SV slave via DPI).
Requires Verilator >= 5.048 (5.036 rejects pulp's vif-in-class; #5265/#5044 fixed in 5.048).

## Stage 1: Verilator 5.036 -> 5.048 migration
Goal: existing co-sim builds + runs green on 5.048, no functional change.
Success Criteria: full `sim-regress` matrix (vc1/2/4/8 x {,_rob}) green on 5.048.
Status: Migration proven; formal full-matrix run deferred to packaging time.
- DONE: `router_wrap.sv` 4 unpacked LINK `_q` arrays `bit`->`logic` + DPI temp copy element-wise
  (5.048 enforces IEEE 1800-2023 6.22.2 element-type equivalence on whole unpacked-array assigns).
  ONLY existing-RTL change needed for the whole tb.
- DONE: spot-checked on 5.048, all build+run+scoreboard clean (16 reads, 0 mismatch), non-invasive
  (scratchpad-local 5.048 via shim; system 5.036 untouched): mesh_4x4_vc1, mesh_4x4_vc4 (wide
  vectors), mesh_4x4_vc2_rob (rob mode). Remaining builds share the identical code path.
- TODO (at packaging time): formal full 8-build `sim-regress` on the official 5.048.
- DEFERRED (packaging decision, user's call): how 5.048 lands officially — `pacman -S` system
  upgrade vs project-pinned verilator. Decide before the formal matrix run.

## Stage 2: pulp AXI BFM port + DPI slave
Goal: SV master (scenario-driven, `axi_driver`/`axi_file_master` based) + SV slave responder
that forwards read/write data to the existing C++ `axi::Memory` via DPI. Replace the C++
master/slave in the testbench.
Success Criteria: one scenario runs end-to-end through the SV BFMs, scoreboard clean.
Status: Not Started
- Provision deps: pulp-platform/axi + common_cells (Bender pins common_cells 1.39.0).
- Design the SV-slave <-> C++-memory DPI seam (the core interface).
- Skip pulp `axi_rand_*` (randomize/rand_id_queue_pkg) — deterministic scenario-driven path only.

## Stage 3: scenario wiring + regression
Goal: existing YAML scenarios drive the SV BFM; scoreboard + full regression matrix green.
Success Criteria: AX4 curated set passes co-sim through the SV master/slave on the 5.048 flow.
Status: Not Started
