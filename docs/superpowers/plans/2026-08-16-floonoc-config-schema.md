# FlooNoC Config Schema Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** replace the topology YAML with FlooNoC-shaped config files, collapse eight testbenches to one, and move the VC count and RoB mode into `specgen/source/constants.yaml`.

**Spec:** `docs/superpowers/specs/2026-08-16-floonoc-config-schema-design.md`. Read it before Task 1; it carries every design decision and the evidence behind it. This plan does not re-argue any of them.

**Architecture:** four config files, one per geometry, in FlooNoC's declarative shape. The address map is derived from array-expanded endpoint declarations rather than hand-listed. Two readers consume it — `sim/tools/address_map.py` at generate time and `ref_model/c_model/include/nmu/sam_yaml.hpp` at simulation runtime — and their agreement is the plan's central risk.

## Global Constraints

- Every commit compiles, passes `make test` (ctest), and passes `make pytest`.
- Commit messages `type(scope): description` in English.
- WSL, `BUILD_ROOT=$HOME/noc_build`, `PYTHON3=python3`. Never `py -3`. Foreground, never backgrounded.
- **`git add` names paths. Never `git add -A`.** The tree carries pre-existing unrelated modifications under `specgen/generated/` (5 files) and untracked `docs/image/*.jpg`.
- DMA (`DMA=1`) must keep working. Its gate is `make sim-gen TB=<t> DMA=1 && make sim TB=<t> DMA=1`.
- VCS must stay buildable. No `vcs` binary on this host — report its absence as such, neither pass nor defect.
- **Before changing any parameter value or range in `constants.yaml`, propose it and wait.** Adding a parameter with a value equal to what the code already hardcodes is not a change and needs no approval; anything else does.
- Run `clang-format -i` on touched C++ (Windows-side `C:\msys64\mingw64\bin\clang-format`).
- No push. Commit only.

## The risk this plan is built around

The two readers must expand a config identically. `sim/tools/address_map.py:1-5` states it mirrors the C++ SAM contract; the runtime loads the SAM through `+sam_config`. **If the expansions diverge, the generated package and the generated stimulus agree with each other and disagree with the runtime address translator** — every artifact looks self-consistent while traffic decodes to the wrong node.

Stage 1 therefore proves parity directly, before anything depends on it. A passing co-sim run is not evidence of parity: the scoreboard only sees the two halves that agree.

## What is being deleted, and why it is deletion rather than revert

This branch landed work that the spec's decisions obsolete: `sim/tb/tb_mesh_4x4_vc8_robless.sv`, the `TB_TOPOLOGY_YAML` table field, and the `TB_NUM_VC` / `TB_READ_ROB` fields. They sit in commits that also carry work which survives (`noc_tb_top.sv`, `noc_fabric.sv`, the `sim-gen` split), so a revert would take the good with the bad. They are deleted in Stage 3 with a commit message saying why.

---

## Stage 1: The config files and both readers, with parity proven

**Goal:** four config files exist, both readers parse them, and the two expansions are proven identical. Nothing consumes them yet — the old path still builds and runs.

**Success Criteria:** for all four geometries, the Python and C++ expansions of the same config produce identical rule sets, compared directly rather than inferred. `make test` and `make pytest` green. Every existing configuration still builds and runs through the old topology YAML.

**Status:** Not Started

### Task 1.1: Write the four config files

**Files:**
- Create: `sim/configs/mesh_2x2.yml`, `mesh_2x2_periph.yml`, `mesh_4x4.yml`, `mesh_4x4_periph4.yml`

- [ ] **Step 1: Transcribe each existing topology YAML into the new shape**

Take the worked example in the spec's section 6 as the template. Each file carries `name`, `description`, `network_type: "axi"`, `routing:`, `endpoints:`, `routers:`, `connections:`. No `protocols:`, no `num_vc`, no RoB mode — those are decisions 2, 3 and 4.

The address map collapses from a hand-listed tile table to two array-expanded ranges per geometry (memory and config), plus a peripheral endpoint where the geometry has one. Derive `base`, `size` and `stride` from the existing YAML's `tiles` and `block_size`; do not invent values.

`mesh_2x2_periph` and `mesh_4x4_periph4` add a peripheral endpoint attached with `dst_dir` read as a port index — spec 3.4. Take each peripheral's host router and face from the existing YAML's `peripherals:` list.

- [ ] **Step 2: Verify each file describes the same address map as the YAML it replaces**

For each geometry, compute the full set of (base, size, space, destination) tuples the old `tiles:` list declares, and the set the new array expansion produces. They must be identical. Show both sets in your report for at least `mesh_4x4` and `mesh_4x4_periph4`.

A difference here is a transcription error, not an improvement.

- [ ] **Step 3: Commit**

```bash
git add sim/configs
git commit -m "feat(config): add the FlooNoC-shaped config files"
```

### Task 1.2: Teach both readers the new schema, and prove they agree

**Files:**
- Modify: `sim/tools/address_map.py`
- Modify: `ref_model/c_model/include/nmu/sam_yaml.hpp`
- Create: a parity test under `sim/tools/` and a ctest exercising the C++ side

**Interfaces:**
- Produces: both readers accept a config file and yield the same ordered rule set.

**Both readers must parse BOTH formats until Stage 2 deletes the old one.** They are
shared code: `address_map.py` is called by the current generators
(`gen_tb_top.py:310-311`), and `sam_yaml.hpp` is exercised by existing ctests through
`TOPOLOGY_DIR` (`tests/nmu/test_sam_yaml.cpp:93-118`) and at runtime through
`+sam_config` (`sim/verilator/Makefile:345`). A reader that only understands the new
shape breaks every one of those the moment it lands. Dispatch on the file's shape, not
on a flag.

- [ ] **Step 1: Extend the Python reader**

`pack()` currently takes a hand-listed `tiles` array. Add array expansion: for an endpoint with `array: [m, n]` and a range carrying `base`, `size`, `stride` and `space`, member `k` lands at `base + stride * k` in row-major order, with `size` as the aperture. Keep `space` semantics exactly as today — it selects the AXI class, spec 3.1.

Where `stride` is absent it defaults to `size`, which is FlooNoC's own behaviour.

- [ ] **Step 2: Extend the C++ reader identically**

`load_sam_table` reads `topology.{x_dim,y_dim}` and `address_map` today. It must read the new shape **in addition to the old one** and perform the same expansion at runtime. The offset-mode validation in `sam_yaml.hpp:78-117` and `addr_trans.hpp`'s `declare_space_coords` are kept — spec 3.5 — and `decode` becomes `use_id_table` (true = table, false + `addr_offset_bits` = offset).

Peripheral endpoints: take the port meaning of `dst_dir` and **do not apply FlooNoC's coordinate derivation** — spec 3.4. A peripheral shares its host router's coordinate.

Protocol labels are presence markers this reader ignores — spec 3.6. Write that as an explicit line with a comment saying why, not as silence.

- [ ] **Step 3: Prove parity directly**

This is the step the stage exists for. A plain `sim/tools` pytest cannot do it alone: the
C++ reader is a header consumed by gtest and by the DPI library, with no callable
interface from Python (`tests/CMakeLists.txt:13-19`, `tests/nmu/CMakeLists.txt:32-40`).

Smallest workable form: a C++ ctest that runs `load_sam_table()` over each of the four
configs and writes its rule tuples out, plus a pytest that expands the same configs with
`address_map.py` and compares against that output. Compare element by element — base,
size, space, destination id, in order.

Do not settle for "both produce N rules" or for a passing simulation. The failure being guarded against is two expansions that are each internally consistent and differ from one another; only a direct comparison catches it.

Report the actual compared output for `mesh_4x4_periph4`, the geometry with the most structure.

- [ ] **Step 4: Confirm nothing else moved**

```bash
make test && make pytest
make sim TB=mesh_2x2_vc1 PATTERN=neighbor
make sim-gen TB=mesh_4x4_vc8 PATTERN=transpose && make sim TB=mesh_4x4_vc8 PATTERN=transpose
```
Every existing configuration still runs through the old topology YAML; this stage only adds a second path.

- [ ] **Step 5: Commit**

```bash
git add sim/tools ref_model/c_model
git commit -m "feat(sam): read the FlooNoC-shaped config in both readers"
```

---

## Stage 2: Switch generation to the new configs

**Goal:** the generators read the config files; the old topology YAMLs are gone.

**Success Criteria:** `sim/topologies/` no longer exists. All eight configurations still build and run, now sourcing their address map from `sim/configs/`. DMA passes. `make test`, `make pytest` green.

**Status:** Not Started

### Task 2.1: Point every generator at the config files

**Files:**
- Modify: `sim/tools/gen_tb_top.py`, `gen_test_patterns.py`, `gen_dma_jobs.py`, `sim/build_config.mk`, `sim/verilator/Makefile`, `sim/vcs/Makefile`
- Modify: `ref_model/c_model/tests/{nmu,wrap,integration}/CMakeLists.txt` — all three hard-code `TOPOLOGY_DIR` to `sim/topologies` (`tests/nmu/CMakeLists.txt:27-40`, `tests/wrap/CMakeLists.txt:8-16`, `tests/integration/CMakeLists.txt:19-24`). **`make test` fails the moment the YAMLs are deleted unless these move in the same commit.**
- Delete: `sim/topologies/*.yaml`
- Modify: `sim/tools/test_*.py`, and the ctests that load a topology by path

- [ ] **Step 1: Switch the generators**

`gen_tb_top.py`'s `load_topology()` reads the config file. The topology package it emits keeps the same contents — the constants the testbench imports do not change in this stage, only where they came from.

`_check_flit_capacity` must keep running: it validates dimensions and VC count against the flit header's capacity, and it runs today as a side effect of `load_topology()` being called unconditionally before every branch. Preserve that property and say in your report how you confirmed it.

- [ ] **Step 2: Re-source the flit-package selection before anything deletes `topology.num_vc`**

`sim/build_config.mk:275-298` picks `noc_types_pkg_vc<N>.sv` by calling
`gen_tb_top.py --print-num-vc`, which reads `topology.num_vc` from the YAML
(`gen_tb_top.py:1222-1237`). Once the VC count lives in `constants.yaml` (Stage 3), that
selection must come from there instead. Do the re-sourcing here, while both sources still
exist, so Stage 3's deletion cannot strand it.

- [ ] **Step 3: Delete the old YAMLs and update every reference**

`sim/topologies/` goes. `TB_TOP_TOPO` and every path that names it changes to the config file. Note `TB_TOPOLOGY_YAML`, the fifth table field added earlier on this branch, exists to map a configuration to its YAML — it survives this stage only if something still needs it; Stage 3 deletes the table entirely either way.

- [ ] **Step 4: Everything runs**

```bash
for t in mesh_2x2_vc1 mesh_2x2_vc1_periph mesh_4x4_vc1 mesh_4x4_vc2 mesh_4x4_vc4 mesh_4x4_vc8 mesh_4x4_vc8_robless mesh_4x4_vc1_periph4; do
    make sim-gen TB=$t PATTERN=neighbor && make sim TB=$t PATTERN=neighbor || exit 1
done
make sim-gen TB=mesh_2x2_vc1 DMA=1 && make sim TB=mesh_2x2_vc1 DMA=1
make test && make pytest
```
Expected: eight passes, a DMA pass, green gates. `mesh_4x4_vc1_periph4` must log `PASS: all 20 nodes done`; `mesh_2x2_vc1_periph` must log 6.

- [ ] **Step 5: Commit**

```bash
git add sim ref_model
git commit -m "refactor(config): source the address map from the config files"
```

---

## Stage 3: One testbench, and the parameters go home

**Goal:** one testbench; `num_vc` and the RoB mode live only in `constants.yaml`; the `ifeq` table is gone.

**Success Criteria:** `git ls-files sim/tb/tb_*.sv` lists one file. `sim/build_config.mk` contains no `ifeq ($(TOPOLOGY),...)` block. `grep -rn "TB_NUM_VC\|TB_READ_ROB\|TB_TOPOLOGY_YAML" sim/` returns nothing. Four geometries build and run. DMA passes.

**Status:** Not Started

### Task 3.1: Move the two parameters into specgen

**Files:**
- Modify: `specgen/source/constants.yaml`, regenerate `specgen/generated/`
- Modify: `sim/tb/noc_tb_top.sv`, `ref_model/c_model/include/wrap/nmu_wrap.hpp`, `ref_model/c_model/include/nmu/nmu.hpp` (`:161` also carries a `RobMode::Enabled` default — all three must move together or specgen is not the only source)

- [ ] **Step 1: Add the RoB-mode parameter**

`noc.DAT_NUM_VC` already exists (default 1, allowed to 8) — nothing to add for the VC count. The RoB mode does not. Add it under `nmu:` with default 1, matching what `nmu_wrap.hpp:80`, `noc_tb_top.sv:62` and every testbench hardcode today.

Proposing the symbol name is part of this step; the value is 1 because that is what the code already does, so it is not a parameter change. Follow the file's convention (`ROB_B_DEPTH` → `NMU_ROB_B_DEPTH_DFLT` / `NMU_ROB_B_DEPTH`).

- [ ] **Step 2: Make the generated constants the only source**

`noc_tb_top.sv:62` and `nmu_wrap.hpp:80` stop hardcoding and read the generated symbols. The C++ default for `rob_mode` comes from specgen rather than a literal `RobMode::Enabled`.

- [ ] **Step 3: Gates**

```bash
make test && make pytest
python3 specgen/tools/codegen.py --check
```

- [ ] **Step 4: Commit**

```bash
git add specgen ref_model/c_model sim/tb/noc_tb_top.sv
git commit -m "refactor(specgen): make constants.yaml the only home for the RoB mode"
```

### Task 3.2: Collapse to one testbench and delete the table

**Files:**
- Create: `sim/tb/tb_noc_mesh.sv` (or keep `noc_tb_top.sv` as the testbench directly — say which you chose and why)
- Delete: all eight `sim/tb/tb_mesh_*.sv`
- Modify: `sim/build_config.mk`, `sim/verilator/Makefile`, `sim/vcs/Makefile`

- [ ] **Step 1: Make the testbench take its configuration from the package**

The eight wrappers differ only in the package they import and in two localparams that are
now specgen constants. With those gone, one testbench importing a fixed-name package
serves every geometry.

**The package is named per geometry today** — `topology_<geom>_pkg`, from
`_geometry_name()` (`gen_tb_top.py:1113-1139`, `:1170`). One testbench needs one import,
so the emitted package name becomes fixed and its *contents* vary with the selected
config. That is the change; say in your report what name you chose. Selecting a different
config regenerates it, so two geometries cannot coexist without regeneration — FlooNoC's
property too, and accepted.

**Dimensions change source.** Both readers take `topology.x_dim` / `y_dim` today
(`gen_tb_top.py:89-92`); under the new schema they come from `routers[].array`. Make that
derivation explicit rather than incidental.

**`READ_ROB` is in more places than the table.** It appears in `sim/Makefile:27`'s help,
in `sim/verilator/Makefile:33-41` and `:287-291` (object directory and run tag), and in
`sim/vcs/Makefile:155-159`. All of them go with it.

- [ ] **Step 2: Delete the eight wrappers and the table**

Including `tb_mesh_4x4_vc8_robless.sv` and the `TB_TOPOLOGY_YAML` field, both added earlier on this branch. The commit message says so plainly — they were built against a design the spec then changed.

- [ ] **Step 3: Four geometries run**

```bash
for g in mesh_2x2 mesh_2x2_periph mesh_4x4 mesh_4x4_periph4; do
    make sim-gen CONFIG=$g PATTERN=neighbor && make sim CONFIG=$g PATTERN=neighbor || exit 1
done
make sim-gen CONFIG=mesh_2x2 DMA=1 && make sim CONFIG=mesh_2x2 DMA=1
make test && make pytest
```
The variable name replacing `TB=` is yours to pick; say which and update the help text with it.

- [ ] **Step 4: Prove the exact combination that was lost still runs**

The configuration decisions 3 and 4 removed is specifically **4x4, 8 VCs, RoBless**
(`tb_mesh_4x4_vc8_robless.sv:15-16`, `build_config.mk:131-136`). Showing that 8 VCs work
somewhere and that RoBless works somewhere does not demonstrate it; the two have to hold
together, and the comparison it existed for needs its partner.

Run both halves on `mesh_4x4`:

1. `constants.yaml` at `DAT_NUM_VC: 8` with the RoB mode enabled — build, run, keep the log.
2. Same, RoB mode disabled — build, run, keep the log.

For (2), show the `[HWM]` line carries the RoBless signature: `read_slot_hwm=0` and
`ar_clause` all-zero **while `aw_clause` stays non-zero**, since B always takes the RoB
in this model (`noc_tb_top.sv:409-413`). An all-zero `aw_clause` would mean dead counters,
not RoBless. For both, show from the log that the fabric elaborated with 8 VCs
(`noc_tb_top.sv:257`, `:270-277`).

Then restore `constants.yaml` and confirm `git diff --stat specgen/source/constants.yaml`
is empty. Do **not** expect a clean `git status --porcelain` here — this task deletes
eight testbenches and has uncommitted work by design.

This is the step that proves decisions 3 and 4 cost no coverage. Without both runs it
proves only that the knobs reach a build.

- [ ] **Step 5: Commit**

```bash
git add sim ref_model
git commit -m "refactor(tb): collapse the eight testbenches into one"
```

---

## Stage 4: Sweep, docs, and the clean invariant

**Goal:** nothing describes the old flow; `make clean` is a verified gate.

**Success Criteria:** `make clean` leaves an empty porcelain and only the three per-host ignored files. No `.md` outside `docs/superpowers/` describes topology YAMLs or the eight configurations. `make sim-injection-sweep` completes.

**Status:** Not Started

### Task 4.1: The sweep loses its VC dimension

**Files:**
- Modify: `sim/Makefile`

- [ ] **Step 1: Remove `SWEEP_VCS`**

The sweep now sweeps injection rate only, at whatever VC count `constants.yaml` holds — spec decision 8. Four curves means four edits and four sweeps. `plot_injection_sweep.py` globs `continuous_*/result.csv`, so results from separate sweeps accumulate into one figure without new logic; confirm that by reading the plotter rather than assuming.

Say in the comment why the dimension is gone, so the next reader does not restore it.

- [ ] **Step 2: Run two points and commit**

```bash
make sim-injection-sweep PATTERN=uniform_random SWEEP_RATES="0.05 0.1"
git add sim/Makefile
git commit -m "refactor(sim): sweep injection rate only, not VC count"
```

### Task 4.2: Documentation and the clean gate

**Files:**
- Modify: `README.md`, `docs/verification-environment.md`, `docs/nmu-spec.md`, `docs/router-spec.md`, `Makefile`, `sim/Makefile`

- [ ] **Step 1: Rewrite the flow sections**

Four geometries, not eight configurations. Config files, not topology YAMLs. The parameter-ownership table from the spec's section 8 belongs in `README.md` — it is the thing a new reader most needs.

- [ ] **Step 2: Rewrite both help targets**

- [ ] **Step 3: The clean invariant, as a gate**

```bash
make clean
git status --porcelain
git status --porcelain --ignored -uall
```
Expected: the first shows **only** the pre-existing unrelated entries this branch never
touched — the five modified `specgen/generated/` files and the untracked
`docs/image/*.jpg`. Anything else means `clean` deleted or modified a tracked file, and
the tracked stimulus example is the one most at risk. The second adds `local.mk`,
`docs/backlog.md` and `.vscode/`.

An earlier draft expected an empty porcelain, which this branch's own starting state
makes impossible.

- [ ] **Step 4: Rebuild from clean and run everything**

```bash
make build
for g in mesh_2x2 mesh_2x2_periph mesh_4x4 mesh_4x4_periph4; do
    make sim-gen CONFIG=$g PATTERN=neighbor && make sim CONFIG=$g PATTERN=neighbor || exit 1
done
make sim-gen CONFIG=mesh_2x2 DMA=1 && make sim CONFIG=mesh_2x2 DMA=1
make -C sim/vcs tb_top CONFIG=mesh_4x4
make test && make pytest
```
The only full from-clean rebuild in the plan, and what proves the restructure works on a machine that has never built this tree.

- [ ] **Step 5: Confirm nothing stale survives**

```bash
git grep -nE 'sim/topologies|topology YAML|tb_mesh_|TB_NUM_VC|TB_READ_ROB' -- '*.md' Makefile 'sim/**'
```
Expected: hits only in `docs/superpowers/`, which records history and must not be rewritten.

- [ ] **Step 6: Commit**

```bash
git add README.md docs Makefile sim
git commit -m "docs(config): describe the four-geometry config flow"
```

---

## Deferred

- **The `sim/configs/` directory layer.** Decision 7 keeps the files flat. If a geometry ever gains several configurations, revisit.
- **`en_collective` replacing the AWUSER mask.** The config files declare it (spec section 2), but nothing consumes it yet; collectives still express membership through the AWUSER address mask. Wiring the declaration to the model is a separate campaign.
