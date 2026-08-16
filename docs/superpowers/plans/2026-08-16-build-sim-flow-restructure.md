# Build→Sim Flow Restructure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut the pipeline into four stages with one job each — build the model, build a testbench binary, generate stimulus, run — and move every DUT parameter out of the make command line into a parameter file.

**Architecture:** One parameterized `noc_fabric.sv` and one parameterized `noc_tb_top.sv`, both checked in, replace five near-identical generated fabrics and six generated tops. Each configuration becomes a hand-written testbench declaring its geometry, VC count and RoB mode as localparams and importing a generated address-map package. `sim/topologies/*.yaml` shrinks to geometry plus address map. The generator survives only to emit that package and the DMA top.

**Tech Stack:** SystemVerilog (Verilator 5.036 + VCS), C++17 c_model over DPI, Python 3 generators, GNU make.

**Spec:** No separate spec file. This plan is the design record. It was reviewed twice by Codex acting as a senior DV engineer; the second review found three factual errors and three ordering hazards, all corrected here.

## Provenance

The target shape is FlooNoC's, verified against `pulp-platform/FlooNoC@main`:

| question | FlooNoC's answer | reference |
|---|---|---|
| Where do testbenches live? | `hw/tb/*.sv`, 8 files, hand-written, checked in | `hw/tb/` |
| How does a testbench get its address map? | imports a generated package: `import floo_axi_mesh_noc_pkg::*;` then `Sam[ClusterX0Y0+Index].start_addr` | `hw/tb/tb_floo_axi_mesh.sv` |
| Is that package checked in? | no — `.gitignore` carries `generated` | `.gitignore` |
| How is geometry declared? | hand-written literals: `localparam int unsigned NumX = 4;` | `hw/tb/tb_floo_axi_mesh.sv` |
| How does make map a testbench to its config? | an explicit `ifeq` table, one entry per testbench | `Makefile` |
| How is stimulus selected? | pre-generated into `generated/jobs`, named at run time via `+JOB_DIR=` | `Makefile`, `.gitignore` |

## Global Constraints

- `specgen/source/constants.yaml` stays the single source for anything C++ and SV must agree on. `codegen.py --check` must keep passing.
- The topology YAML is loaded at RUNTIME by the C++ SAM (`nmu_wrap.hpp:97`) and read at GENERATE time by the Python generators. Anything placed there must be legal for both.
- No new dependencies. C++17, CMake ≥ 3.20, Verilator, VCS.
- No bespoke manifest, registry, linter, or scenario-config system.
- Every commit compiles, passes ctest, and passes `make pytest`.
- Commit messages `type(scope): description` in English.
- Builds and co-sim run on WSL from `/mnt/e/05_NoC/noc_project` with `BUILD_ROOT=$HOME/noc_build` and `PYTHON3=python3`. Never `py -3`.
- Run `clang-format -i` on any touched C++ (Windows-side `C:\msys64\mingw64\bin\clang-format`).
- No push. Stop at the working tree unless told otherwise.
- **`git add` names paths. Never `git add -A`** — the working tree carries unrelated edits from before this plan.
- **DMA (`DMA=1`) is out of scope but must never break.** It keeps the generated-top path through `gen_tb_top.py --dma` for the whole of this plan, and its compatibility with the Stage 5 rename is Task 5.1's explicit responsibility.

  One deliberate exception, decided here rather than discovered later: **DMA gains the `sim-gen` step along with every other configuration.** Its job files are produced by the same `gen-directed` target — `sim/verilator/Makefile:291-292` generates them inside `run-directed` today, and Task 1.1 moves that block out with the other one. From Stage 1 onward the DMA gate is two commands, not one:

  ```bash
  make sim-gen TB=mesh_2x2_vc1 DMA=1 && make sim TB=mesh_2x2_vc1 DMA=1
  ```

  Every DMA verification step in this plan uses that pair. A bare `make sim TB=... DMA=1` from a clean tree is expected to fail on the stimulus guard — that is the contract working, not DMA breaking. No DMA stimulus set is tracked; Task 1.3's example is directed file-master stimulus only, because a DMA job set is bound to `DMA_JOBS_PER_NODE` / `DMA_LENGTH` / `DMA_RW` and no single one of them is the example.
- **VCS must stay buildable.** `sim/vcs/Makefile` calls `gen_tb_top.py` for a directed top at `sim/vcs/Makefile:113`; any stage that changes what the generator emits migrates VCS in the same stage, not later.

## Verified facts this plan rests on

Established by reading the files, not inferred. An implementer who finds one of these false must stop and report rather than work around it.

| fact | evidence |
|---|---|
| `gen_tb_top.py` accepts `--topology --out --read-rob --dma --jobs-per-node --length --rw --print-num-vc` and **nothing else**. There is no `--run-class`. | `sim/tools/gen_tb_top.py:1507-1530` |
| `DAT_NUM_VC` is **already** in specgen with default 1, max 8. Only the RoB-mode default is missing. | `specgen/source/constants.yaml:85-93` |
| `NmuWrap::init(config_path, src_id, port_id, dat_num_vc, queue_depth, rob_mode, b_rob_depth, r_rob_depth, max_txns_per_id)` does **not** receive mesh dimensions. `rob_mode` is already a parameter. | `ref_model/c_model/include/wrap/nmu_wrap.hpp:78-83` |
| The five DUT-depth make options have no `?=` default; unset leaves the plusarg unpassed (`$(if ...)`-guarded at `:298-302`) and the tb falls back to `ni_params_pkg::*_DFLT`. | `sim/verilator/Makefile:216-231`, `:298-302` |
| `+num_reads`/`+num_writes` feed only the watchdog budget. The real count comes from `aw_queue.size()`. | `sim/dv/axi-0.39.7/src/axi_test.sv:2537`, `sim/tb/test/user_node_endpoint.sv:1022`, `sim/tools/gen_tb_top.py:1200` |
| `test_patterns/` survives `make clean` today — `sim/verilator/Makefile`'s `clean` removes only `obj_dir_*` and `output`. This violates the invariant stated at `Makefile:163-165`. | `sim/verilator/Makefile` `clean:` |
| Generated artifacts are gitignored and absent from a clean checkout. | `.gitignore:45,51,52,53` |

## Parameter ownership after this plan

| to change | edit |
|---|---|
| NI/router depths, AXI widths, `DAT_NUM_VC` default, RoB default | `specgen/source/constants.yaml` |
| mesh size, address map, peripheral placement | `sim/topologies/<geom>.yaml` |
| a configuration's VC count, RoB mode, slave delay model | `sim/tb/tb_<name>.sv` |
| burst length, beat size, id distribution | `sim/tools/gen_test_patterns.py` header constants |
| add a configuration | new `sim/tb/tb_<name>.sv` + one `ifeq` line |

Surviving directed make options, ten total. DMA (`DMA_JOBS_PER_NODE`, `DMA_LENGTH`, `DMA_RW`) and tool/host options (`BUILD_ROOT`, `PYTHON3`, `VERILATOR`, `JOBS`) are outside this count and unchanged.

- `sim-gen`: `INJECTION_COUNT=` `SEED=` `MCAST_SHAPE=` `HOTSPOT_PERIPHERALS=`
- `sim-run`: `INJECTION_MODE=` `INJECTION_RATE=` `TIMEOUT_CYCLES=` `DECERR_FAULT=` `DECERR_FAULT_WR=` `MCAST_FAULT=`

## `make clean` through the plan

`Makefile:163-165` states the invariant: after `clean` the tree holds nothing but what git tracks, apart from `local.mk`, `docs/backlog.md` and `.vscode/`. The restructure moves files across the tracked/generated line in both directions, so `clean-generated` changes in four stages. Getting it wrong in either direction is a real defect: deleting a checked-in `noc_fabric.sv` destroys tracked work, and failing to delete a generated package leaves a stale one to be compiled.

One stimulus set is tracked as an example, so `clean` cannot simply `rm -rf` the stimulus root. See Task 1.3.

| stage | change to clean |
|---|---|
| 1 | `sim/verilator/Makefile`'s `clean` starts removing generated stimulus while sparing the tracked example — fixes the pre-existing violation without creating its mirror image |
| 2 | nothing. `rm -f ref_model/top/noc_fabric_*.sv` does not match `noc_fabric.sv`; make the non-match deliberate with a comment rather than incidental |
| 3 | `clean-generated` gains `sim/tb/topology_*_pkg.sv` |
| 4 | `rm -f sim/tb/test/tb_top_*.sv` narrows to the DMA top only |
| 6 | the invariant becomes a verified gate |

The gate, run once at the end of Stage 6 and after any stage that changes the list:

```bash
make clean && git status --porcelain && git status --porcelain --ignored -uall
```

Expected: the first is empty (no tracked file modified or deleted by clean), the second lists only `local.mk`, `docs/backlog.md`, `.vscode/`.

---

## Stage 1: Split stimulus generation from the run

**Goal:** `sim-gen` exists, `sim-run` no longer generates, `make sim` errors before building when stimulus is absent. Generated tops and fabrics are untouched, and topology names keep their `_vc<N>` suffix — geometry names do not exist until Stage 5.

**Success Criteria:** `make sim-gen TB=mesh_2x2_vc1 PATTERN=neighbor` produces `sim/verilator/test_patterns/mesh_2x2/neighbor/` and compiles nothing. `make sim TB=mesh_2x2_vc1 PATTERN=neighbor` against an absent stimulus tree exits non-zero **before** Verilator runs. After `sim-gen`, the same command passes. `make sim-injection-sweep` completes. `make clean` removes every generated stimulus tree and leaves the tracked example untouched, and a fresh clone runs `make sim TB=mesh_2x2_vc1 PATTERN=neighbor` with no `sim-gen` first.

**Status:** Not Started

### Task 1.1: Add `sim-gen` and stop the run from generating

**Files:**
- Modify: `sim/verilator/Makefile` — the `run-directed` recipe at `:279-292`, the guard block near `:178`
- Modify: `sim/Makefile:13`, `:94-110`
- Modify: `Makefile:151-159`

**Interfaces:**
- Produces: `sim-gen` (root and `sim/`), `gen-directed` (`sim/verilator/`). `sim-gen` takes `TB=`, the same variable every other target takes. `TOPO=` does not exist yet.

- [ ] **Step 1: Move the two generator invocations into their own target**

`sim/verilator/Makefile:286-292` holds them inside `run-directed`. Cut both `$(if $(filter 1,$(DMA)),...)` blocks — the `$(GEN_PATTERNS)` one and the `$(GEN_DMA_JOBS)` one — verbatim, and paste them into a new target above `run-directed`:

```make
.PHONY: gen-directed
gen-directed:
	@mkdir -p $(STIM_ROOT)
	<the two blocks, unchanged>
	@echo "stimulus: $(STIM_ROOT)"
```

`run-directed` keeps `@mkdir -p output/$(SIM_TAG)`, its `RUN_CLASS` guard, and everything from `@echo "running $(SIM_TAG)..."` onward.

- [ ] **Step 2: Guard at parse time, not as a prerequisite**

A prerequisite would run after `$(TBTOP_EXE)`, so a missing stimulus tree would cost a full build before reporting. `STIM_ROOT` is a parse-time `:=` composition (`sim/verilator/Makefile:236-260`), so the guard can fire before any recipe:

```make
# Parse-time, unlike the RUN_CLASS guard at :288 which must be a recipe check:
# nothing target-overrides STIM_ROOT, so its value is final here, and firing
# before the first prerequisite is what keeps a missing stimulus tree from
# costing a build first.
ifneq ($(filter run-directed,$(MAKECMDGOALS)),)
ifeq ($(wildcard $(STIM_ROOT)),)
$(error no stimulus at $(STIM_ROOT). Run: make sim-gen TB=$(TOPOLOGY) PATTERN=$(PATTERN))
endif
endif
```

- [ ] **Step 3: Forward from `sim/Makefile` and the root**

`sim/Makefile` gains a `gen` target beside `sim`/`run`/`build`, forwarding `gen-directed` with the same `TOPOLOGY=$(_TOPO)` and `$(_INJ_ARGS)` the `sim` target passes at `:94-104`. Add `gen` to `.PHONY` at `:13`. The root gains `sim-gen: ; @$(MAKE) -C sim gen` beside `sim-build`/`sim-run` at `Makefile:155-159`, and `sim-gen` joins `.PHONY` at `:151`.

- [ ] **Step 4: Make `clean` remove generated stimulus and spare the tracked one**

`sim/verilator/Makefile`'s `clean` is `rm -rf $(VL_BUILD)/obj_dir_* output` — it never removes stimulus, which is why `test_patterns/` survives clean today in violation of `Makefile:163-165`. A blunt `rm -rf $(STIM_BASE)` would fix that and immediately break the opposite half of the same invariant, because Task 1.3 makes one stimulus set tracked.

The invariant is stated in terms of git, so enforce it with git:

```make
# Generated stimulus goes; the tracked example stays. `clean` means "leave what
# git tracks", and -fdx is exactly that predicate -- an rm -rf here would delete
# the committed example, and leaving the directory alone is what put this
# Makefile in violation of the root's clean contract to begin with.
clean:
	rm -rf $(VL_BUILD)/obj_dir_* output
	@git clean -fdxq $(STIM_BASE) 2>/dev/null || rm -rf $(STIM_BASE)
```

The fallback keeps `clean` working outside a git checkout (a released tarball), where there is no tracked example to protect either.

Apply the same treatment to `sim/vcs/Makefile` if it has a stimulus root.

- [ ] **Step 5: Verify the guard fires before the build**

```bash
cd /mnt/e/05_NoC/noc_project && export BUILD_ROOT=$HOME/noc_build
rm -rf sim/verilator/test_patterns $BUILD_ROOT/verilator/obj_dir_*
time make sim TB=mesh_2x2_vc1 PATTERN=neighbor; echo "rc=$?"
```
Expected: `rc` non-zero, elapsed under two seconds, the message names `make sim-gen TB=mesh_2x2_vc1 PATTERN=neighbor`, and no Verilator or g++ line appears. Deleting `obj_dir_*` first is what makes this test meaningful — with a warm binary the build would be a no-op and prove nothing.

- [ ] **Step 6: Verify the full path and the clean invariant**

```bash
make sim-gen TB=mesh_2x2_vc1 PATTERN=neighbor
make sim TB=mesh_2x2_vc1 PATTERN=neighbor
make sim-gen TB=mesh_4x4_vc8 PATTERN=transpose
make clean && ls sim/verilator/test_patterns 2>&1
```
Expected: `DIRECTED PASS: ... scoreboard clean, non-vacuous`, then a listing with `mesh_4x4` gone. Whether `mesh_2x2` is gone too depends on Task 1.3 — before it lands the whole tree goes; after it, `mesh_2x2/neighbor` stays. Run this step again at the end of Task 1.3.

- [ ] **Step 7: Commit**

```bash
git add sim/Makefile sim/verilator/Makefile sim/vcs/Makefile Makefile
git commit -m "refactor(sim): make stimulus generation its own step"
```

### Task 1.2: Fix the consumers that assumed implicit generation

**Files:**
- Modify: `sim/Makefile:121-131`
- Modify: `README.md`, `docs/verification-environment.md`

- [ ] **Step 1: Make the sweep generate once, up front**

`sim-injection-sweep` calls `make sim` 36 times in a nested loop; after Task 1.1 all 36 fail on the missing-stimulus guard. Insert one `$(MAKE) gen TB=... PATTERN=$(PATTERN)` before the loop, using the sweep's own `INJECTION_COUNT` and `SEED`, and leave the loop calling `sim` unchanged.

All four sweep configurations (`tb_mesh_4x4_vc{1,2,4,8}`) share the `mesh_4x4` stimulus directory because `_GEOMETRY` strips `_vc<N>` (`sim/verilator/Makefile:251`). One generation covers all 36 points; say so in the comment.

- [ ] **Step 2: Run two points of the sweep**

```bash
make sim-injection-sweep PATTERN=uniform_random SWEEP_VCS=1 SWEEP_RATES="0.05 0.1"
```
Expected: both runs execute without the missing-stimulus error. The trailing `plot_injection_sweep.py` needs the full sweep and will exit non-zero; that is expected here — check only that the two runs ran.

- [ ] **Step 3: Update the two docs**

`README.md`'s "Simulate (cosim)" target table gains a `make sim-gen` row and the statement that a run never generates. `docs/verification-environment.md:46-52` is a table of the two files `gen_tb_top.py` emits -- accurate today, and rewritten in Stage 6, not here. What this step corrects is any text presenting stimulus generation as part of a run. Leave every description of generated tops alone — they stay accurate until Stage 4.

- [ ] **Step 4: Commit**

```bash
git add sim/Makefile README.md docs/verification-environment.md
git commit -m "fix(sim): generate the sweep's stimulus before the sweep runs"
```

### Task 1.3: Track one stimulus set as the worked example

**Files:**
- Create: `sim/verilator/test_patterns/mesh_2x2/neighbor/**` (tracked), `sim/verilator/test_patterns/README.md`
- Modify: `.gitignore:35`

**Interfaces:**
- Produces: the one stimulus tree that exists in a fresh clone, so `make sim TB=mesh_2x2_vc1 PATTERN=neighbor` runs with no `sim-gen` first.

- [ ] **Step 1: Choose the smallest set that shows the format**

`mesh_2x2` / `neighbor`: four nodes, a deterministic bijection destination, no seed dependence. Roughly 280 KB across four `node<i>/{write,read}.txt` pairs. Regenerate it deliberately with the defaults rather than committing whatever is on disk:

```bash
make sim-gen TB=mesh_2x2_vc1 PATTERN=neighbor SEED=1
```

`SEED=1` matters — an unset `SEED` draws a random one (`sim/Makefile:68`), and an example nobody can reproduce is not an example.

- [ ] **Step 2: Except it from the ignore rule**

`.gitignore:35` is `sim/*/test_patterns/`. Git will not re-include a path under an ignored *directory*, so the rule has to stop ignoring the directory itself and ignore its contents instead:

```
sim/*/test_patterns/*
!sim/*/test_patterns/README.md
!sim/*/test_patterns/mesh_2x2/
sim/*/test_patterns/mesh_2x2/*
!sim/*/test_patterns/mesh_2x2/neighbor/
```

Verify with `git check-ignore -v` on both an example file and a generated one before committing; a rule that silently ignores the example produces an empty commit that looks fine.

- [ ] **Step 3: Write the README beside it**

Short. What the directory is, that everything except the example is generated by `make sim-gen`, that `clean` spares the example, the per-line field order of `write.txt`/`read.txt` (`axid`, `addr`, `axlen`, `axsize`, `burst`, `lock`, `cache`, `prot`, `qos`, `region`, `atop` — write only — `user`), and that write files carry `0x<data> 0x<strb> 0` beats afterwards with an address-in-data payload. Take the field order from `_ax_fields` in `sim/tools/gen_test_patterns.py`, not from this plan.

- [ ] **Step 4: Prove a fresh clone can run it**

```bash
make clean
ls sim/verilator/test_patterns/mesh_2x2/neighbor/node0/
make sim TB=mesh_2x2_vc1 PATTERN=neighbor
git status --porcelain
```
Expected: the example survives `clean`, the run passes with no `sim-gen`, and porcelain is empty — proving `clean` neither deleted nor modified a tracked file.

- [ ] **Step 5: Commit**

```bash
git add .gitignore sim/verilator/test_patterns
git commit -m "docs(sim): track one stimulus set as the worked example"
```

---

## Stage 2: One parameterized fabric

**Goal:** `ref_model/top/noc_fabric.sv` is checked in and parameterized on geometry, VC count and peripheral attachment. The generator instantiates it instead of emitting a per-topology copy. Both simulators follow in this stage.

**Success Criteria:** `git ls-files ref_model/top/` lists `noc_fabric.sv`. `make sim` passes for `mesh_2x2_vc1`, `mesh_4x4_vc8` and `mesh_4x4_vc1_periph4`. `make sim-gen TB=mesh_2x2_vc1 DMA=1 && make sim TB=mesh_2x2_vc1 DMA=1` passes. `make -C sim/vcs tb_top TOPOLOGY=mesh_4x4_vc8` elaborates. `make clean` deletes no tracked file.

**Status:** Not Started

### Task 2.1: Write the fabric and switch every consumer in one step

**Files:**
- Create: `ref_model/top/noc_fabric.sv`
- Modify: `sim/tools/gen_tb_top.py` — delete the fabric emitter, change the top to instantiate `noc_fabric`
- Modify: `sim/build_config.mk:171`
- Modify: `sim/vcs/Makefile:113-120`
- Modify: `.gitignore:47-53`, `Makefile:180`

**Interfaces:**
- Produces: `module noc_fabric #(X_DIM, Y_DIM, DAT_NUM_VC, ROUTER_VC_DEPTH, REQ_FLIT_WIDTH, RSP_FLIT_WIDTH, DAT_FLIT_WIDTH, N_PERIPH, PERIPH_NODE, PERIPH_PORT)`.

- [ ] **Step 1: Produce the reference fabrics to work from**

Generated artifacts are gitignored and absent from a clean checkout, so generate them first. The generator takes `--topology` and nothing else here:

```bash
python3 sim/tools/gen_tb_top.py --topology mesh_2x2_vc1
python3 sim/tools/gen_tb_top.py --topology mesh_4x4_vc8
python3 sim/tools/gen_tb_top.py --topology mesh_4x4_vc1_periph4
wc -l ref_model/top/noc_fabric_*.sv
```

Expected shape: the two non-peripheral fabrics are 425 lines each and differ by 98 diff lines — module name, include guard, `DAT_NUM_VC`'s default, and array bounds `[4]` versus `[16]`. The peripheral one is 1002 lines because the generator unrolls a ~126-line block per peripheral, each identical but for host-router coordinate, face port index, and endpoint index.

- [ ] **Step 2: Write the module**

Node arrays become `[X_DIM*Y_DIM]`. The four unrolled peripheral blocks become one `generate for (genvar p = 0; p < N_PERIPH; p++)` over `PERIPH_NODE[p]` / `PERIPH_PORT[p]`, endpoint index `X_DIM*Y_DIM + p`. `N_PERIPH = 0` must elaborate to exactly the non-peripheral fabric.

Unpacked array parameters (`parameter int PERIPH_NODE [N_PERIPH]`) are SV-2012 but tool support varies. If Verilator 5.036 or VCS rejects them, fall back to a packed vector (`parameter logic [N_PERIPH-1:0][7:0] PERIPH_NODE`) and say so in the report — do not silently change the interface shape.

- [ ] **Step 3: Switch the generator, the build, VCS and clean together**

These cannot be split: while the generated top still `include`s and instantiates `noc_fabric_<topo>`, pointing `NOC_FABRIC_SV` at the shared file compiles a module nobody instantiates and leaves the old one undefined.

- `gen_tb_top.py` stops writing a fabric and emits an instantiation of `noc_fabric` with the parameters the topology implies. The DMA top gets the same treatment — it must not keep a per-topology fabric, or Stage 2's success criterion is unreachable.
- `sim/build_config.mk:171`: `NOC_FABRIC_SV = $(SRC_SV)/noc_fabric.sv`, no `$(TOPOLOGY)`.
- `sim/vcs/Makefile:113-120` regenerates the fabric alongside the top; drop the fabric half.
- `.gitignore:53` drops `ref_model/top/noc_fabric_*.sv`.
- `Makefile:180`: `rm -f ... ref_model/top/noc_fabric_*.sv` no longer matches anything. It also does not match `noc_fabric.sv` — the underscore saves it. Delete the pattern rather than leave a rule that would destroy tracked work if the name ever changed.

- [ ] **Step 4: Elaborate all three shapes, then run them**

```bash
for t in mesh_2x2_vc1 mesh_4x4_vc8 mesh_4x4_vc1_periph4; do
    make -C sim/verilator elaborate TOPOLOGY=$t RUN_CLASS=directed || echo "ELAB FAIL $t"
done
make sim-gen TB=mesh_2x2_vc1 PATTERN=neighbor && make sim TB=mesh_2x2_vc1 PATTERN=neighbor
make sim-gen TB=mesh_4x4_vc8 PATTERN=transpose && make sim TB=mesh_4x4_vc8 PATTERN=transpose
make sim-gen TB=mesh_4x4_vc1_periph4 PATTERN=neighbor && make sim TB=mesh_4x4_vc1_periph4 PATTERN=neighbor
```

`RUN_CLASS` is a make variable of `sim/verilator/Makefile`, not a generator flag — the distinction that broke the previous draft of this plan.

Expected: three clean elaborations, three `DIRECTED PASS`. The peripheral run's `run.log` must also carry `PASS: all 20 nodes done, non-vacuous`.

- [ ] **Step 5: Prove DMA and VCS survived**

```bash
make sim-gen TB=mesh_2x2_vc1 DMA=1 && make sim TB=mesh_2x2_vc1 DMA=1
make -C sim/vcs tb_top TOPOLOGY=mesh_4x4_vc8
```
Expected: a DMA pass and a VCS elaboration, both exiting zero. Do not append `|| echo` — a masked failure is not a gate.

- [ ] **Step 6: Gates, clean check, commit**

```bash
make test && make pytest
make clean && git status --porcelain
git add ref_model/top/noc_fabric.sv sim/tools/gen_tb_top.py sim/build_config.mk \
        sim/vcs/Makefile .gitignore Makefile sim/tools/test_*.py
git commit -m "refactor(fabric): collapse the generated fabrics into one parameterized module"
```
`git status --porcelain` after clean must be empty — if clean deleted the new tracked `noc_fabric.sv`, it shows here.

---

## Stage 3: The shared top and the first hand-written testbench

**Goal:** `sim/tb/noc_tb_top.sv` holds the generated top's body. A generated per-geometry package carries the address map. One hand-written `sim/tb/tb_mesh_2x2_vc1.sv` builds and passes through both.

**Success Criteria:** `make sim TB=mesh_2x2_vc1 PATTERN=neighbor` passes using the hand-written testbench, and `git ls-files sim/tb/` lists it. Every other configuration still passes on the generated path. `make clean` removes `sim/tb/topology_*_pkg.sv` and no tracked file.

**Status:** Not Started

### Task 3.1: Emit the address map as a per-geometry package

**Files:**
- Modify: `sim/tools/gen_tb_top.py` (add `--emit-topology-pkg`)
- Modify: `.gitignore`, `Makefile:180`, `sim/build_config.mk` (`TB_TOP_SV_SRC`)

**Interfaces:**
- Produces: package `topology_<geom>_pkg` exporting `X_DIM`, `Y_DIM`, `NUM_NODES`, `NUM_ENDPOINTS`, `TILE_TARGETS`, `TILE_BASE_ADDR`, `TILE_SIZE`, `NOC_EGRESS_BASE`, `REGION_BYTES`, `N_PERIPH`, `PERIPH_NODE`, `PERIPH_PORT`.

- [ ] **Step 1: Emit the package**

Every value already exists in the generated top's parameter block; this relocates them into a package, computed exactly as the generator computes them today from `address_map.node_windows()`. Nothing about their derivation changes — a value that differs from what the current top declares is a bug in this step.

Per geometry, not per configuration: one `topology_mesh_4x4_pkg` serves `tb_mesh_4x4_vc1`, `_vc8` and `_vc8_robless`. This mirrors `floo_axi_mesh_noc_pkg` supplying `Sam[]` to `tb_floo_axi_mesh.sv`.

Gitignored, like FlooNoC's `generated`. It is derived from a tracked YAML and the build always reproduces it, so no drift exists for a gate to catch — unlike specgen's output, which two languages must agree on. Add it to `clean-generated` at `Makefile:180`.

Until Stage 5 the geometry is still spelled with its VC suffix, so the package for `mesh_2x2_vc1` is named for whatever `_GEOMETRY` yields (`mesh_2x2`). Do not anticipate the rename.

- [ ] **Step 2: Verify the package against the current top**

Generate both for `mesh_2x2_vc1` and `mesh_4x4_vc1_periph4` and compare `TILE_BASE_ADDR`, `TILE_SIZE`, `NUM_ENDPOINTS` and the peripheral table character by character against the generated top's parameter block. Report any difference rather than adopting it.

- [ ] **Step 3: Commit**

```bash
git add sim/tools/gen_tb_top.py .gitignore Makefile sim/build_config.mk
git commit -m "feat(tb): emit the address map as a per-geometry package"
```

### Task 3.2: Write `noc_tb_top.sv` and the first testbench

**Files:**
- Create: `sim/tb/noc_tb_top.sv`, `sim/tb/tb_mesh_2x2_vc1.sv`
- Modify: `sim/build_config.mk:86-88`, `:171-185`

- [ ] **Step 1: Move the generated top's body into a parameterized module**

Generate `tb_top_mesh_2x2_vc1.sv` first (Task 2.1 Step 1's command). Its parameter block is the opening ~45 lines; everything after is either generic or sized by `NUM_NODES` / `NUM_ENDPOINTS` — liveness trace, watchdog, DPI lifecycle, AXI bus arrays, fabric instantiation, endpoint generate loop, FSDB, exit logic, perf instrumentation, DPI error poll. All of it becomes `noc_tb_top`'s body. Take the section boundaries from the file in front of you; do not trust a line range quoted anywhere.

- [ ] **Step 2: Pass the address map through the module boundary**

`user_node_endpoint` takes `TILE_BASE_ADDR` and `TILE_SIZE` as elaboration parameters. They must appear in `noc_tb_top`'s parameter list and be forwarded to every endpoint instance. A testbench that imports the package but does not pass these through re-creates exactly the fragility the package removes.

- [ ] **Step 3: Write the testbench**

```systemverilog
// Configuration: 2x2 mesh, 1 DAT VC, RoB enabled.
module tb_top;
    import topology_mesh_2x2_pkg::*;

    localparam int unsigned DAT_NUM_VC       = 1;
    localparam int unsigned READ_ROB_ENABLED = 1;

    noc_tb_top #(
        .X_DIM(X_DIM), .Y_DIM(Y_DIM),
        .NUM_ENDPOINTS(NUM_ENDPOINTS),
        .TILE_TARGETS(TILE_TARGETS),
        .TILE_BASE_ADDR(TILE_BASE_ADDR),
        .TILE_SIZE(TILE_SIZE),
        .NOC_EGRESS_BASE(NOC_EGRESS_BASE),
        .REGION_BYTES(REGION_BYTES),
        .N_PERIPH(N_PERIPH),
        .PERIPH_NODE(PERIPH_NODE),
        .PERIPH_PORT(PERIPH_PORT),
        .DAT_NUM_VC(DAT_NUM_VC),
        .READ_ROB_ENABLED(READ_ROB_ENABLED)
    ) u_tb ();
endmodule
```

The module stays named `tb_top` so `--top-module` needs no change. The slave-delay and master-backpressure localparams from the generated top's parameter block join this file as named constants — they are per-configuration DV parameters and belong here.

- [ ] **Step 4: Select it by an explicit table**

In `sim/build_config.mk`, one entry per hand-written testbench, FlooNoC's shape:

```make
# One line per checked-in testbench: which geometry package it imports and
# which VC-count flit package it needs. No filename parsing -- a suffix like
# _robless is not a geometry, and a scrape would have to learn every suffix.
ifeq ($(TOPOLOGY),mesh_2x2_vc1)
TB_HANDWRITTEN := 1
TB_GEOMETRY    := mesh_2x2
TB_NUM_VC      := 1
endif
```

`TB_TOP_SV` (`:86-88`) selects the hand-written file when `TB_HANDWRITTEN` is set and the generated one otherwise, so this stage migrates one configuration without disturbing five. `TOPOLOGY_NUM_VC`'s `--print-num-vc` query (`:178`) stays for everything not yet in the table.

- [ ] **Step 5: Build, run, and prove nothing else moved**

```bash
make sim-gen TB=mesh_2x2_vc1 PATTERN=neighbor && make sim TB=mesh_2x2_vc1 PATTERN=neighbor
make sim-gen TB=mesh_4x4_vc8 PATTERN=transpose && make sim TB=mesh_4x4_vc8 PATTERN=transpose
make sim-gen TB=mesh_2x2_vc1 DMA=1 && make sim TB=mesh_2x2_vc1 DMA=1
make test && make pytest
```
Expected: two `DIRECTED PASS`, a DMA pass, green gates. The `mesh_4x4_vc8` run must still go through the generated top — if it does not, the table is matching too widely.

- [ ] **Step 6: Commit**

```bash
git add sim/tb/noc_tb_top.sv sim/tb/tb_mesh_2x2_vc1.sv sim/build_config.mk
git commit -m "feat(tb): add the shared parameterized top and the first hand-written testbench"
```

---

## Stage 4: Migrate every directed configuration

**Goal:** All six directed configurations are hand-written testbenches. `gen_tb_top.py` no longer emits a directed top, and VCS moves off it in the same stage.

**Success Criteria:** `git ls-files sim/tb/tb_*.sv` lists six files and is the authoritative configuration list. All six pass. `make sim-gen TB=mesh_2x2_vc1 DMA=1 && make sim TB=mesh_2x2_vc1 DMA=1` passes. `make -C sim/vcs tb_top TOPOLOGY=mesh_4x4_vc8` elaborates, exit zero. `sim/tb/test/tb_top_*.sv` is no longer produced and its `.gitignore` line is gone.

**Status:** Not Started

### Task 4.1: Write the five remaining testbenches

**Files:**
- Create: `sim/tb/tb_mesh_4x4_vc{1,2,4,8}.sv`, `sim/tb/tb_mesh_4x4_vc1_periph4.sv`
- Modify: `sim/build_config.mk` (five more `ifeq` entries)

- [ ] **Step 1: Write them**

Each is Task 3.2 Step 3's shape with its own `DAT_NUM_VC` and `READ_ROB_ENABLED`. The peripheral configuration additionally passes `N_PERIPH`, `PERIPH_NODE` and `PERIPH_PORT` from its package.

- [ ] **Step 2: Run all six**

```bash
for g in mesh_2x2 mesh_4x4 mesh_4x4_periph4; do :; done   # geometries, for reference only
for t in mesh_2x2_vc1 mesh_4x4_vc1 mesh_4x4_vc2 mesh_4x4_vc4 mesh_4x4_vc8 mesh_4x4_vc1_periph4; do
    make sim-gen TB=$t PATTERN=neighbor && make sim TB=$t PATTERN=neighbor || echo "FAIL $t"
done
```
Expected: six `DIRECTED PASS`, no `FAIL` line.

- [ ] **Step 3: Commit**

```bash
git add sim/tb/tb_mesh_4x4_*.sv sim/build_config.mk
git commit -m "feat(tb): hand-write the remaining directed configurations"
```

### Task 4.2: Retire the directed top emitter and move VCS with it

**Files:**
- Modify: `sim/tools/gen_tb_top.py`, `sim/tools/test_*.py`
- Modify: `sim/vcs/Makefile:113-120`, `sim/build_config.mk:86-88,:178`
- Modify: `.gitignore:51`, `Makefile:180`

- [ ] **Step 1: Delete the directed emitter**

`gen_tb_top.py` keeps `--emit-topology-pkg`, `--dma` and `--print-num-vc`. Its directed top emitter goes, along with `.gitignore:51` (`sim/tb/test/tb_top_*.sv`) and that pattern in `clean-generated`. `sim/tb/soc/tb_top_dma_*.sv` stays on both lists — DMA is deferred.

- [ ] **Step 2: Move VCS in the same commit**

`sim/vcs/Makefile:113` calls the generator for a directed top. It must switch to the checked-in testbench in this task, not in a later stage — otherwise VCS is broken for every commit in between, and the "every commit compiles" constraint fails. Both simulators read the `ifeq` table from `sim/build_config.mk`, so the mapping is written once.

- [ ] **Step 3: Migrate the pytest suites**

Suites under `sim/tools/` assert on generated-top text. Move them to asserting on package contents and address-map packing. `gen_test_patterns.py` and `gen_dma_jobs.py` coverage is untouched — do not weaken it while moving the top tests.

- [ ] **Step 4: Full gates**

```bash
make test && make pytest
for t in mesh_2x2_vc1 mesh_4x4_vc8 mesh_4x4_vc1_periph4; do
    make sim-gen TB=$t PATTERN=neighbor && make sim TB=$t PATTERN=neighbor || echo "FAIL $t"
done
make sim-gen TB=mesh_2x2_vc1 DMA=1 && make sim TB=mesh_2x2_vc1 DMA=1
make -C sim/vcs tb_top TOPOLOGY=mesh_4x4_vc8
make clean && git status --porcelain
```
Expected: green gates, three passes, a DMA pass, a VCS elaboration at exit zero, and an empty porcelain after clean.

- [ ] **Step 5: Commit**

```bash
git add sim/tools/gen_tb_top.py sim/tools/test_*.py sim/vcs/Makefile \
        sim/build_config.mk .gitignore Makefile
git commit -m "refactor(tb): retire the directed top emitter and move VCS to the checked-in tops"
```

---

## Stage 5: Topology YAML keys on geometry

**Goal:** `sim/topologies/*.yaml` carries mesh size and address map only. VC count and RoB mode are specgen defaults overridden per testbench. `sim-gen` takes `TOPO=<geometry>`.

**Success Criteria:** `ls sim/topologies/` shows `mesh_2x2.yaml`, `mesh_4x4.yaml`, `mesh_4x4_periph4.yaml` and nothing VC-suffixed. No YAML contains `num_vc`. `make sim-gen TOPO=mesh_4x4 PATTERN=transpose` works. All six configurations pass, DMA passes through its `sim-gen` pair, VCS elaborates.

**Status:** Not Started

### Task 5.1: Rename the YAMLs and carry DMA across

**Files:**
- Rename: `sim/topologies/mesh_{2x2_vc1,4x4_vc1,4x4_vc2,4x4_vc4,4x4_vc8,4x4_vc1_periph4}.yaml` → `mesh_2x2.yaml`, `mesh_4x4.yaml`, `mesh_4x4_periph4.yaml`
- Modify: `sim/tools/gen_tb_top.py`, `gen_test_patterns.py`, `gen_dma_jobs.py`, `sim/build_config.mk`, `sim/Makefile`, `sim/verilator/Makefile`, `sim/vcs/Makefile`, `sim/tools/test_*.py`

- [ ] **Step 1: Collapse and rename**

`mesh_4x4_vc{1,2,4,8}.yaml` are byte-identical apart from `num_vc`, so they collapse to one `mesh_4x4.yaml` with `num_vc` removed. Confirm the byte-identity with `diff` before deleting anything.

- [ ] **Step 2: Give DMA an explicit geometry mapping**

`sim/Makefile:97` passes `TOPOLOGY=$(_TOPO)`, and the DMA path builds `TB_TOP_TOPO := .../$(TOPOLOGY).yaml` (`sim/verilator/Makefile:101`). After the rename, `TB=mesh_2x2_vc1 DMA=1` looks for a YAML that no longer exists. The `ifeq` table from Stage 3 already maps `TOPOLOGY` to `TB_GEOMETRY`; extend the YAML path to use `TB_GEOMETRY`, and add table entries for whatever DMA configurations exist. This is the task that keeps the standing DMA constraint true — it is not optional cleanup.

- [ ] **Step 3: Add `TOPO=` to `sim-gen`**

With geometry YAMLs in place, `sim-gen` takes `TOPO=<geometry>` directly and stops deriving it. Keep `TB=` working as an alias that resolves through the table, so Stage 1-4's commands and any muscle memory keep working.

- [ ] **Step 4: Everything runs**

```bash
for g in mesh_2x2 mesh_4x4 mesh_4x4_periph4; do make sim-gen TOPO=$g PATTERN=neighbor; done
for t in mesh_2x2_vc1 mesh_4x4_vc1 mesh_4x4_vc2 mesh_4x4_vc4 mesh_4x4_vc8 mesh_4x4_vc1_periph4; do
    make sim TB=$t PATTERN=neighbor || echo "FAIL $t"
done
make sim-gen TB=mesh_2x2_vc1 DMA=1 && make sim TB=mesh_2x2_vc1 DMA=1
make -C sim/vcs tb_top TOPOLOGY=mesh_4x4_vc8
make test && make pytest
```

- [ ] **Step 5: Commit**

```bash
git add sim/topologies sim/tools sim/build_config.mk sim/Makefile \
        sim/verilator/Makefile sim/vcs/Makefile
git commit -m "refactor(topology): key the topology YAML on geometry, not VC count"
```

### Task 5.2: The RoB-mode default and the geometry cross-check

**Files:**
- Modify: `specgen/source/constants.yaml`, regenerate `specgen/generated/`
- Modify: `ref_model/c_model/include/wrap/nmu_wrap.hpp`, `ref_model/dpi/cmodel_dpi.{h,cpp}`
- Create: a ctest for the mismatch

- [ ] **Step 1: Propose the RoB-mode default, then wait**

`DAT_NUM_VC` is already in specgen (`constants.yaml:85-93`, default 1, max 8) — nothing to add. The RoB mode is not. `nmu::RobMode` already reaches the model through `NmuWrap::init`'s `rob_mode` parameter (`nmu_wrap.hpp:81`), so this adds a specgen default for it, not a new mechanism.

**Stop here and propose the symbol name and default value to the owner. Do not write a parameter value without approval** (`CLAUDE.md`, Parameter Discipline). List every file referencing it afterwards and confirm consistency.

- [ ] **Step 2: Cross-check geometry at the DPI boundary**

Geometry now lives in the YAML (loaded at runtime by the C++ SAM) and in the package the testbench imports (derived from that same YAML at generate time). They cannot disagree today, but a hand-edited testbench could make them.

`NmuWrap::init` does **not** currently receive dimensions — its parameters are `config_path, src_id, port_id, dat_num_vc, queue_depth, rob_mode, b_rob_depth, r_rob_depth, max_txns_per_id` (`nmu_wrap.hpp:78-83`). The dimensions are loaded inside `sam_yaml.hpp`. Two options; pick one and say which in the report:

- add `x_dim`/`y_dim` parameters to `NmuWrap::init` and its DPI entry point, comparing against what `load_sam_table` produced — the same shape as the existing endpoint-identity validation at `nmu_wrap.hpp:98-117`;
- or place the check where dimensions already both exist: `cmodel_router_create` receives router dimensions at the DPI boundary.

Add a ctest that a mismatch aborts. Run `clang-format -i` on every touched C++ file.

- [ ] **Step 3: Gates and commit**

```bash
make test && make pytest
make sim TB=mesh_4x4_vc8 PATTERN=neighbor
git add specgen/source/constants.yaml specgen/generated \n        ref_model/c_model/include/wrap/nmu_wrap.hpp ref_model/dpi \n        ref_model/c_model/tests
git commit -m "feat(model): check the testbench geometry against the loaded topology"
```

---

## Stage 6: Delete the DUT-parameter make options, and close the clean invariant

**Goal:** No DUT parameter is reachable from a make command line, no document describes the old flow, and `make clean` is a verified gate.

**Success Criteria:** `B_ROB_DEPTH`, `R_ROB_DEPTH`, `MAX_TXNS_PER_ID`, `MAX_OUTSTANDING`, `MAX_UNIQUE_IDS`, `BURST_LEN` and `IDS_PER_INITIATOR` appear in no Makefile, and `READ_ROB` survives at exactly one place -- `sim/build_config.mk:102`, feeding the deferred DMA top. Ten directed options remain. `make clean` leaves a clean porcelain and only the three per-host files ignored. No `.md` outside `docs/superpowers/plans/` describes generated tops or implicit generation.

**Status:** Not Started

### Task 6.1: Remove the override hatches

**Files:**
- Modify: `sim/Makefile`, `sim/verilator/Makefile`, `sim/vcs/Makefile`, `sim/build_config.mk:102`
- Modify: `sim/tb/noc_tb_top.sv`, `sim/tools/gen_test_patterns.py`

- [ ] **Step 1: Delete the five DUT-depth options and their plusargs**

Each has no `?=` default and is unset by default, so the testbench already falls back to `ni_params_pkg::*_DFLT`. Removing them changes no default behaviour — it removes the ability to override a compiled-in parameter at run time, which is the point.

- [ ] **Step 2: Move the stimulus-shape knobs into the generator**

`BURST_LEN` and `IDS_PER_INITIATOR` become named constants at the top of `gen_test_patterns.py`, beside the already-hardcoded `--size 5`. Keep the argparse flags — they are how the pytest suites drive the generator — but drop the make forwarding.

- [ ] **Step 3: `READ_ROB` leaves the run tag and the DMA path**

`_TOPO_TAG := $(TOPOLOGY)_rob$(READ_ROB)` (`sim/verilator/Makefile:265`) becomes the testbench name, which now carries the mode. `READ_ROB ?= 1` at `sim/build_config.mk:102` still feeds `gen_tb_top.py --read-rob` for the DMA top — DMA is deferred, so this one occurrence stays. Say so in a comment beside it, or the next reader will delete it and break DMA.

Add `sim/tb/tb_mesh_4x4_vc8_robless.sv` in this task if the RoB-versus-RoBless comparison is to stay runnable.

- [ ] **Step 4: Confirm the count across every Makefile**

```bash
git grep -nE 'B_ROB_DEPTH|R_ROB_DEPTH|MAX_TXNS_PER_ID|MAX_OUTSTANDING|MAX_UNIQUE_IDS|BURST_LEN|IDS_PER_INITIATOR' \
    -- Makefile sim/Makefile sim/verilator/Makefile sim/vcs/Makefile sim/build_config.mk
git grep -n 'READ_ROB' -- Makefile 'sim/*.mk' 'sim/**/Makefile'
```
Expected: the first returns nothing; the second returns only the commented DMA occurrence.

- [ ] **Step 5: Commit**

```bash
git add sim/Makefile sim/verilator/Makefile sim/vcs/Makefile sim/build_config.mk \
        sim/tb/noc_tb_top.sv sim/tools/gen_test_patterns.py
git commit -m "refactor(sim): delete the DUT-parameter make options"
```

### Task 6.2: Documentation, help text, and the clean gate

**Files:**
- Modify: `README.md`, `docs/verification-environment.md`, `docs/nmu-spec.md`, `docs/router-spec.md`
- Modify: `Makefile` `help`, `sim/Makefile` `help`

- [ ] **Step 1: Rewrite the flow sections**

`docs/verification-environment.md:46-52` tabulates the two files `gen_tb_top.py` emits -- both gone by Stage 4. `README.md`'s "Simulate (cosim)" describes generation inside a run. Replace with the four steps and the parameter-ownership table from this plan's header. `docs/router-spec.md:14` says "Generated fabric wiring" and `:512`, `:728` reference `gen_tb_top.py`; correct all three. `docs/nmu-spec.md:349` and `docs/router-spec.md:618` cite the co-sim command — still correct, but they should name `sim-gen` as a prerequisite.

- [ ] **Step 2: Rewrite both help targets**

The root `help` gains `sim-gen` and drops every deleted option. `sim/Makefile`'s `help` lists 21 variables across four `@echo` blocks; it drops to the ten survivors, grouped by the step that consumes them, with DMA's three named separately as the deferred path.

- [ ] **Step 3: The clean invariant, as a gate**

```bash
make clean
git status --porcelain
git status --porcelain --ignored -uall
```
Expected: the first empty — in particular the Task 1.3 example neither deleted nor modified; the second listing only `local.mk`, `docs/backlog.md`, `.vscode/`. Anything else is a `clean-generated` entry that was never added, or a tracked file `clean` should not be touching. Fix it here rather than recording it.

- [ ] **Step 4: Confirm nothing stale survives**

```bash
git grep -nE 'tb_top_<|noc_fabric_<|gen_tb_top|B_ROB_DEPTH|MAX_UNIQUE_IDS|READ_ROB=|make -C sim ' -- '*.md'
```
Expected: hits only in `docs/superpowers/plans/` — historical execution records of finished campaigns, which must not be rewritten — and wherever the DMA path is deliberately described.

- [ ] **Step 5: Rebuild from clean and run the full set**

```bash
make build
for g in mesh_2x2 mesh_4x4 mesh_4x4_periph4; do make sim-gen TOPO=$g PATTERN=neighbor; done
for t in mesh_2x2_vc1 mesh_4x4_vc1 mesh_4x4_vc2 mesh_4x4_vc4 mesh_4x4_vc8 mesh_4x4_vc1_periph4; do
    make sim TB=$t PATTERN=neighbor || echo "FAIL $t"
done
make sim-gen TB=mesh_2x2_vc1 DMA=1 && make sim TB=mesh_2x2_vc1 DMA=1
make -C sim/vcs tb_top TOPOLOGY=mesh_4x4_vc8
make test && make pytest
```
This is the only full from-clean rebuild in the plan, and it is what proves the restructure works on a machine that has never built this tree.

- [ ] **Step 6: Commit**

```bash
git add README.md docs/verification-environment.md docs/nmu-spec.md \n        docs/router-spec.md Makefile sim/Makefile
git commit -m "docs(sim): describe the four-step flow and retire the generated-top path"
```

---

## Deferred

- **DMA.** `DMA=1` keeps `gen_tb_top.py --dma`, its own endpoint sources (`sim/build_config.mk:138-164`), its `READ_ROB` occurrence, and its `jobs.txt` stimulus. It is a parallel production line, not another directed configuration; migrating it needs its own `sim-gen-dma` and a shared top parameterized by endpoint type. Every stage above must leave it working.
- **`--exclude-self` and `--hotspot-rates`.** Generator arguments no make path has ever reached. They stay as argparse flags.
