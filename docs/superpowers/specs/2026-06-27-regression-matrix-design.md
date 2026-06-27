# NoC co-sim regression matrix

Status: READY (codex 3-pass reviewed; one-cell design, preserve-addr + _rob resolution + out-root fixes incorporated)
Date: 2026-06-27

## Problem

No driver sweeps the wire-level mesh co-sim across topologies, ROB modes, and stimuli.
`make sim TB=<t> PATTERN=<p>` runs exactly one cell; `run_regress.py` was deferred and never built
(`docs/architecture.md:295-296`). The proportional-VC and channel_model work cleared the
prerequisites, so the matrix is now buildable.

## Goal / success criteria

- A declarative matrix runs, per cell, a Verilator co-sim of the mesh NoC and gates on the
  DUT-agnostic scoreboard marker `PASS: scenario complete, scoreboard clean`.
- Axes: topology `{mesh_4x4_vc1, vc2, vc4, vc8}` x rob `{disabled, enabled}` = 8 builds, crossed
  with a declared list of stimuli.
- One command runs a tier: `make sim-regress TIER=nightly`. Non-zero exit if any cell fails;
  excluded cells reported as skipped with reason.
- Machine-readable `matrix.json` + a human console summary.
- Gate survives a future c_model->RTL DUT swap (only the scoreboard PASS string is checked).

## Architecture

Declarative matrix YAML + thin Python runner. CTest stays scoped to the c_model suite; CMake is a
poor format for axes/exclusions, and this is a simulator regression needing topology build reuse,
scenario generation, and DUT-swap tolerance.

**One cell type, no special cases.** Every cell is the existing single-point runner
`sim/tools/run_benchmark.py` invoked as `(topology, --from <base_scenario>, --pattern <traffic_pattern>)`.
The two stimulus groups differ only in what `--from` feeds:
- **traffic patterns** (`neighbor`, `uniform_random`, `transpose`, `hotspot`): a spatial traffic
  algorithm computing each node's destination tile, over a simple base scenario (`AX4-BAS-003`).
- **AX4 conformity** (the curated `AX4-*` scenarios): each scenario fed as `--from` with the
  `neighbor` traffic pattern supplying mesh destinations, plus a new `--preserve-addr` mode (Stage 2)
  so the scenario's original per-transaction local offsets are kept (only the destination tile is
  OR-ed into `addr[39:32]`). `neighbor` is a bijection on the mesh (each dst has exactly one src), so
  preserving offsets cannot collide -- which lets address-sensitive scenarios (OOB / 4KB-boundary /
  decerr, whose conformity depends on the low address bits vs `memory_size`) traverse the fabric with
  their condition intact. This extends the path `make run-tb-top SCENARIO=<ax4>` already uses; no
  single-master injection seam is built.

```
sim/regress/matrix.yaml      declarative single source of truth (axes, tiers, stimuli, exclusions)
        |
sim/regress/run_regress.py   expand -> exclude -> prebuild 8 binaries (once) -> run each cell via
        |                    run_benchmark.py -> gate on PASS marker -> aggregate -> matrix.json
        +--> sim/tools/run_benchmark.py    the one cell runner (topology + --from + --pattern)
```

**Cell contract** (uniform, DUT-agnostic): `(topology, rob_mode, from_scenario, traffic_pattern,
output_dir)` -> build/locate `Vtb_top` -> run -> require the PASS marker.

New files live in `sim/regress/` (its own home, peer to `sim/topologies/` and `sim/test_patterns/`):
`matrix.yaml`, `run_regress.py`, `test_run_regress.py`. Per-run output -> `sim/regress/output/`
(already covered by the `sim/*/output/` gitignore rule).

## Scope (v1) and deferred work

**v1 delivers** the correctness regression: every topology x rob x stimulus is run and gated on the
scoreboard. This proves the fabric/router/NMU/NSU stay scoreboard-clean across all 8 builds, all 4
traffic patterns, and the curated AX4 conformity set.

**Deferred to a later round (documented, not silent):**
- **Traffic-pattern multi-id stimulus.** `gen_test_patterns.py` injects a single AXI id
  (`_emit_synthetic_node` hard-codes `id:0`; `--from` copies the base's single id). Because the
  c_model binds each active AXI id to one VC within its read/write class (`nmu/vc_arbiter.hpp`,
  `nsu/vc_arbiter.hpp`), single-id traffic-pattern stimulus does NOT exercise concurrent multi-VC
  occupancy on vc4/vc8 -- those rows validate scoreboard-clean execution, not VC spread, for the
  traffic-pattern group. The AX4 group partially covers the gap: many curated scenarios carry
  distinct ids (e.g. `AX4-ORD-002` uses 0x1-0x4), so AX4 cells exercise some VC spread on vc4/vc8.
  The `--id-policy` enhancement is a next-round item.
- **per-id VC binding re-evaluation.** A survey (IHI 0022 + FlooNoC) found the per-id binding
  deviates from FlooNoC (which selects VCs id-agnostically by message class + uses a RoB for same-id
  response order) and is redundant when the response ROB is enabled (load-bearing only in robless
  mode). Whether to keep or restructure it is a separate design round; it does not block this matrix.

The matrix report states the VC-spread limitation so vc4/vc8 PASS is not read as full multi-VC
coverage. v1 proves scoreboard-clean execution across builds, not exhaustive VC-spread or every
routing behavior.

## Change units (v1)

### Stage 1: ROB generated axis
ROB is a behavioral build axis, not a topology, so generate the 8 variants from 4 base YAMLs via a
flag rather than stamping `*_rob.yaml` files (which would drift as dims/VC counts evolve).
- `gen_tb_top.py`: load the BASE topology YAML (strip the `_rob` suffix from the requested topology
  name -- it currently loads `sim/topologies/<name>.yaml` directly at `gen_tb_top.py:54`) and derive
  `rob_enabled` from the `_rob` suffix instead of the YAML `rob_mode` key. Emit
  `tb_top_mesh_4x4_vc<N>_rob.sv` from `mesh_4x4_vc<N>.yaml`.
- The two other Python tools also load the per-name YAML and must learn the same base-topology
  resolution (strip `_rob` for the YAML path, keep the full name for the binary/obj-dir): node-count
  load in `run_benchmark.py:40` and `gen_test_patterns.py:261`. Otherwise `_rob` cells fail before
  simulation once the `*_rob.yaml` files are removed.
- Build wiring: both sim Makefiles require `sim/topologies/$(TOPOLOGY).yaml`
  (`sim/verilator/Makefile:87`, `sim/vcs/Makefile:100`) -- add a `TOPOLOGY_BASE = $(TOPOLOGY:_rob=)`
  for the YAML path while keeping the full variant name for obj-dir / tb-name / `.topology` stamp.
  `build_config.mk:77` already strips `_rob` for `noc_types_pkg`.
- Delete the hand-copied `sim/topologies/mesh_4x4_vc2_rob.yaml` + committed
  `sim/filelist_mesh_4x4_vc2_rob.f` (regenerated on demand). Update `sim/test_patterns/README.md`,
  which documents `mesh_4x4_vc2_rob` as a YAML-controlled topology.
- Verify all 8 variants build, and rob-enabled ones still select the `cmodel_nmu_create_ex(...,1)`
  DPI ctor. `make check` is unaffected (it builds default `mesh_4x4_vc1`, `Makefile:172-174`).

### Stage 2: preserve-addr mode + matrix runner + matrix.yaml
- `gen_test_patterns.py`: add an opt-in `--preserve-addr` mode. Default behavior (reallocate each
  source's offset to avoid converging-source collisions) is UNCHANGED, so the traffic-pattern cells
  and the existing path need no re-verification. Under `--preserve-addr`, keep each transaction's
  original local offset and only OR the destination tile into `addr[39:32]`. Covered by
  `test_gen_test_patterns.py`.
- `run_benchmark.py`: add a `--preserve-addr` CLI flag that forwards to the `gen_test_patterns.py`
  invocation (its generator command currently forwards only `--from`/memory/etc., `run_benchmark.py:191`).
  The full pass-through chain is: matrix `preserve_addr: true` -> `run_regress.py` passes
  `--preserve-addr` -> `run_benchmark.py` forwards it -> `gen_test_patterns.py` honors it. Used by the
  AX4 conformity cells (with `neighbor`, a bijection, so no collision).
- `matrix.yaml`: tiers, topologies, rob_modes, a stimuli list (each entry = `{from, patterns}`, with
  an optional `preserve_addr: true` on the AX4 group), exclusions with reason strings.
- `run_regress.py`: load matrix -> resolve abbreviated scenario ids (e.g. `AX4-BAS-003`) to full
  `sim/test_patterns/<id>*/scenario.yaml` paths -> expand cross-product -> apply exclusions (mark
  skipped+reason) -> prebuild the 8 topology binaries SERIALLY -> run each cell via `run_benchmark.py`
  always passing a UNIQUE `--out-root` (its default output name omits `--from`, so AX4 neighbor cells
  would otherwise collide) -> gate on the PASS marker -> aggregate -> write `matrix.json` + print a
  console summary.
- `make sim-regress TIER=nightly` entry.

**matrix.yaml shape:**
```yaml
tiers:
  nightly:
    topologies: [mesh_4x4_vc1, mesh_4x4_vc2, mesh_4x4_vc4, mesh_4x4_vc8]
    rob_modes: [disabled, enabled]
    stimuli:
      - from: AX4-BAS-003
        patterns: [neighbor, uniform_random, transpose, hotspot]
      - from: all_curated_ax4      # the AX4-* set minus AX4-INF-* (intentional DPI-fatal)
        patterns: [neighbor]
        preserve_addr: true        # keep per-txn offsets so OOB/4KB-boundary conformity survives
exclusions:
  - when: {rob_mode: enabled, from: AX4-BUR-003}
    reason: "burst len 256 > ROB_CAPACITY 32"
```

`all_curated_ax4` expands to the `sim/test_patterns/AX4-*` scenarios excluding the `AX4-INF-*`
intentional-DPI-fatal prefix (mirroring the Layer 2 integration convention) -- 36 scenarios at the
current set size.

## Error handling

Per-cell failure (missing PASS marker, timeout, build failure) is captured as a FAIL with the
captured log path; the runner continues the rest of the matrix and exits non-zero if any cell
failed. Excluded cells are SKIPPED with their reason. Each cell gets a unique output dir; the 8
binaries are prebuilt serially before any cell runs (the two top risks: shared-dir corruption under
parallelism, and silent build reuse across variants).

## Testing

- `sim/regress/test_run_regress.py` (pytest, mirroring `sim/tools/test_run_benchmark.py`): matrix
  expansion, exclusion application, scenario-id resolution, result aggregation, `matrix.json`
  emission -- driven by a mock cell runner so it needs no real sim.
- `--preserve-addr`: add a `test_gen_test_patterns.py` case asserting an OOB/boundary `--from`
  scenario keeps its original offset (only the dst tile is OR-ed in) under `--preserve-addr`, and that
  the default path is unchanged.
- Stage 1: a build smoke that all 8 variants verilate; rob-enabled selects the `_ex` ctor.
- Stage 2: a real `make sim-regress TIER=smoke` (a tiny tier: 1-2 topologies x 1 traffic pattern + 1
  AX4 with preserve_addr) runs green end to end, incl. one address-sensitive AX4 (e.g. AX4-RSP-002)
  passing scoreboard-clean through the fabric.

## Out of scope

- Traffic-pattern multi-id stimulus (`--id-policy`), the per-id VC binding re-evaluation, and JUnit
  XML reporting (next rounds; JUnit waits until a CI consumer exists).
- Injection-rate saturation sweeps, perf-threshold gating (the gate is correctness-only).
- VCS execution of the matrix (Linux-only; v1 targets Verilator).
