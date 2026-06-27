# Retire tb_genamba + demote ChannelModel to test-only — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the production→test layering inversion by retiring the superseded `tb_genamba` referee co-sim and demoting `ChannelModel` to a test-only stub, with zero design-behavior change.

**Architecture:** This is a structural deletion + header rehome, not a feature. There is no new functionality to test-drive. Each task's "test cycle" is: existing ctest suite stays green, Verilator tb_top still builds, and the structural invariant (`grep '#include "common/"' under c_model/include/` returns nothing) holds. Tasks follow the build-safe order `(b) FlitBytes rehome → (c) retire genamba build flows → (a) sever inversion → (d) test+doc cleanup` so every intermediate commit compiles.

**Tech Stack:** C++17 / GoogleTest (ctest), Verilator + VCS co-sim, GNU Make, CMake.

**Spec:** `docs/superpowers/specs/2026-06-27-retire-tb-genamba-channel-model-design.md`

## Global Constraints

- Build python: pass `PYTHON3=python3` (mingw64) to every `make` that runs specgen/Verilator. Never `py -3`.
- VCS (`sim/vcs/`) is Linux-workstation only; it cannot be built on this Windows host. VCS Makefile edits are verified by inspection + grep, not by a local build.
- `tests/common/channel_model.hpp` and `tests/common/channel_model_params.hpp` (except one stale comment) are PRESERVED — ChannelModel stays as the unit-test NoC stub.
- The shared flit pack/unpack marshalling helpers in `sim/c/cmodel_dpi.cpp` serve router/NMU/NSU — they are NOT channel-specific and must survive.
- Every commit message: `type(scope): description` (English), ending with the Co-Authored-By trailer.
- C/C++ edits: run `clang-format -i` on touched `.hpp`/`.cpp` (repo `.clang-format`, Google base + IndentWidth 4 + ColumnLimit 100).
- Branch already created: `refactor/retire-tb-genamba`. Do not push unless the user says so.

---

### Task 1: FlitBytes rehome (spec unit b)

Move the shared flit-byte type out of the channel-model-named header into a neutral home, so no production wrap header is named after (or coupled to) ChannelModel.

**Files:**
- Create: `c_model/include/wrap/flit_bytes.hpp`
- Modify: `c_model/include/wrap/channel_model_wrap_io.hpp` (drop the type defs, keep only `ChannelModelInputs/Outputs`, include the new header)
- Modify (repoint include): `c_model/include/wrap/nmu_wrap.hpp`, `c_model/include/wrap/nsu_wrap.hpp`, `c_model/include/wrap/router_wrap_io.hpp`, `c_model/include/wrap/nmu_wrap_io.hpp`, `c_model/include/wrap/nsu_wrap_io.hpp`, `c_model/include/wrap/flit_byte_conv.hpp`
- NOT touched: `c_model/tests/wrap/test_nsu_wrap.cpp` uses `FlitBytes` but gets it transitively through `nsu_wrap.hpp` (no direct `channel_model_wrap_io.hpp` include) — repointing `nsu_wrap.hpp` carries it; leave the test file alone.
- Test: existing ctest suite (no new test — the type is unchanged, only relocated)

**Interfaces:**
- Produces: `c_model/include/wrap/flit_bytes.hpp` defining, in namespace `ni::cmodel::wrap`, `constexpr int FLIT_BYTES`, `using FlitBytes = std::array<uint8_t, FLIT_BYTES>`, `constexpr int FLIT_VEC_WORDS`. Tasks 3 reuses this header for `cmodel_dpi.cpp`.

- [ ] **Step 1: Create the new neutral header**

`c_model/include/wrap/flit_bytes.hpp`:
```cpp
// Shared flit byte-array type for the DPI wire boundary.
//
// FlitBytes carries the full c_model flit (ni::FLIT_WIDTH bits) as a byte array
// for DPI marshalling. Used by every *_wrap IO header and the DPI bridge; not
// specific to any one component.
#pragma once
#include "ni_flit_constants.h"
#include <array>
#include <cstdint>

namespace ni::cmodel::wrap {

// Full c_model flit stored as a byte array, size = ni::FLIT_WIDTH rounded up to bytes.
static constexpr int FLIT_BYTES = (ni::FLIT_WIDTH + 7) / 8;  // 51
using FlitBytes = std::array<uint8_t, FLIT_BYTES>;

// Number of 32-bit svBitVecVal words needed to carry one flit.
static constexpr int FLIT_VEC_WORDS = (ni::FLIT_WIDTH + 31) / 32;  // 13

}  // namespace ni::cmodel::wrap
```

- [ ] **Step 2: Gut `channel_model_wrap_io.hpp` to just the channel structs**

Replace the type definitions (the `#include "ni_flit_constants.h"` + the three `FLIT_BYTES`/`FlitBytes`/`FLIT_VEC_WORDS` lines) with a single `#include "wrap/flit_bytes.hpp"`. Keep `ChannelModelInputs` and `ChannelModelOutputs` exactly as-is. Result — `c_model/include/wrap/channel_model_wrap_io.hpp`:
```cpp
// ChannelModel wrap IO POD structs — Stage 5b spec §5.1 / §6.2.
// (Deleted in Task 3 together with ChannelModelWrap + the channel DPI.)
#pragma once
#include "wrap/flit_bytes.hpp"  // FlitBytes
#include <array>
#include <cstdint>

namespace ni::cmodel::wrap {

struct ChannelModelInputs {
    bool req_in_valid;
    FlitBytes req_in_flit;
    bool req_in_credit_return;

    bool rsp_in_valid;
    FlitBytes rsp_in_flit;
    bool rsp_in_credit_return;
};

struct ChannelModelOutputs {
    bool req_out_valid;
    FlitBytes req_out_flit;
    bool req_out_credit_return;

    bool rsp_out_valid;
    FlitBytes rsp_out_flit;
    bool rsp_out_credit_return;
};

}  // namespace ni::cmodel::wrap
```

- [ ] **Step 3: Repoint the FlitBytes-only includers**

In each of these 6 files, change the line `#include "wrap/channel_model_wrap_io.hpp"  // FlitBytes...` to `#include "wrap/flit_bytes.hpp"  // FlitBytes, FLIT_BYTES` (all under `c_model/include/wrap/`):
`nmu_wrap.hpp` (line 27), `nsu_wrap.hpp` (line 32), `router_wrap_io.hpp` (line 23), `nmu_wrap_io.hpp` (line 18), `nsu_wrap_io.hpp` (line 22), `flit_byte_conv.hpp` (line 10).

Verify none of these 6 reference `ChannelModelInputs`/`ChannelModelOutputs` (they don't):
Run: `grep -rln "ChannelModelInputs\|ChannelModelOutputs" c_model/include/wrap/nmu_wrap.hpp c_model/include/wrap/nsu_wrap.hpp c_model/include/wrap/router_wrap_io.hpp c_model/include/wrap/nmu_wrap_io.hpp c_model/include/wrap/nsu_wrap_io.hpp c_model/include/wrap/flit_byte_conv.hpp`
Expected: no output.

- [ ] **Step 4: clang-format touched files**

Run: `clang-format -i c_model/include/wrap/flit_bytes.hpp c_model/include/wrap/channel_model_wrap_io.hpp`

- [ ] **Step 5: Build + run ctest (must stay green)**

Run: `make test PYTHON3=python3`
Expected: configure + compile succeed; ctest reports the same pass count as baseline (567 passed + 20 skipped), 0 failed.

- [ ] **Step 6: Commit**

```bash
git add c_model/include/wrap/flit_bytes.hpp c_model/include/wrap/channel_model_wrap_io.hpp \
        c_model/include/wrap/nmu_wrap.hpp c_model/include/wrap/nsu_wrap.hpp \
        c_model/include/wrap/router_wrap_io.hpp c_model/include/wrap/nmu_wrap_io.hpp \
        c_model/include/wrap/nsu_wrap_io.hpp c_model/include/wrap/flit_byte_conv.hpp
git commit -m "refactor(wrap): rehome FlitBytes to wrap/flit_bytes.hpp

Decouple the shared DPI flit-byte type from the channel-model-named header.
channel_model_wrap_io.hpp now keeps only ChannelModelInputs/Outputs.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Retire genamba build flows (spec unit c)

Delete the legacy single-node testbench sources and every build-system reference to it. After this task, `ChannelModelWrap` + the `cmodel_channel_model_*` DPI still exist but are unreferenced by any build target — the tree still compiles.

**Files:**
- Delete: `sim/sv/tb_genamba.sv`, `sim/sv/tb_genamba_tester.sv`, `sim/sv/genamba/` (whole dir), `sim/sv/genamba_master_bfm.sv`, `sim/genamba_init.yaml`
- Delete: `sim/verilator/main_genamba.cpp`, `sim/verilator/run_genamba.sh`
- Modify: `sim/verilator/Makefile` (remove `genamba` / `run-genamba` / `genamba-tester` / `run-genamba-tester` / `clean-genamba` targets, their obj dirs, and the `clean: clean-genamba` dependency)
- Modify: `sim/vcs/Makefile` (remove `-top tb_genamba`, `genamba` / `run-genamba` targets, `GENAMBA_*` / `FSDB_PLUSARG_GENAMBA` / `GENAMBA_SCENARIO*` vars, and genamba from the `run-all-fsdb` batch)
- Modify: `sim/build_config.mk` (remove `GENAMBA_SV_SRC`, `GENAMBA_TESTER_SV_SRC`, `GENAMBA_INC_DEPS`, `GENAMBA_DEFINES`)
- Modify: `Makefile` (root — remove the two `run-genamba` lines from the `help:` echo block)

- [ ] **Step 1: Delete the SV + C sources**

```bash
git rm sim/sv/tb_genamba.sv sim/sv/tb_genamba_tester.sv sim/sv/genamba_master_bfm.sv \
       sim/genamba_init.yaml sim/verilator/main_genamba.cpp sim/verilator/run_genamba.sh
git rm -r sim/sv/genamba
```

- [ ] **Step 2: Strip genamba from the Verilator Makefile**

In `sim/verilator/Makefile`: delete the `GENAMBA_*` / `TESTER_*` variable blocks and the `.PHONY: genamba` / `genamba:` / `run-genamba:` / `genamba-tester` / `run-genamba-tester` / `clean-genamba:` recipes. In the `clean:` rule, remove the `clean-genamba` prerequisite.
Verify: `grep -ni genamba sim/verilator/Makefile`
Expected: no output.

- [ ] **Step 3: Strip genamba from the VCS Makefile (inspection-verified, no local VCS)**

In `sim/vcs/Makefile`: delete the `GENAMBA_*`, `FSDB_PLUSARG_GENAMBA`, `GENAMBA_SCENARIO*` vars, the `-top tb_genamba` build + `genamba` / `run-genamba` recipes, and any genamba entry in the `run-all-fsdb` batch target.
Verify: `grep -ni genamba sim/vcs/Makefile`
Expected: no output.

- [ ] **Step 4: Strip GENAMBA source lists + stale comments from build_config.mk and root**

In `sim/build_config.mk`: delete the `GENAMBA_SV_SRC`, `GENAMBA_TESTER_SV_SRC`, `GENAMBA_INC_DEPS`, `GENAMBA_DEFINES` blocks (~line 114) AND the stale genamba comment references at ~lines 3, 21, 109 (header comments describing the genamba build artifacts).
In the root `Makefile`: delete the two `run-genamba` lines in the `help:` echo block (~lines 31-34) AND the stale genamba comment references in the header block at ~lines 5, 6, 11.
Verify: `grep -rni genamba Makefile sim/build_config.mk`
Expected: no output (gate is comment-inclusive — every genamba mention is gone).

- [ ] **Step 5: Verilator tb_top still builds**

Run: `make build-verilator PYTHON3=python3`
Expected: tb_top (default topology) verilates + compiles with no missing-source / missing-target error.

- [ ] **Step 6: ctest still green (unaffected, sanity)**

Run: `make test PYTHON3=python3`
Expected: 567 passed + 20 skipped, 0 failed.

- [ ] **Step 7: Commit**

```bash
git add -A sim/ Makefile
git commit -m "chore(sim): retire tb_genamba referee co-sim build flows

Delete tb_genamba/tb_genamba_tester SV + gen_amba BFM + main_genamba.cpp and
all Verilator/VCS Makefile + build_config + root-help references. Superseded by
the mesh tb_top fabric co-sim.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Sever the inversion (spec unit a)

Now that nothing builds genamba, delete the production wrap + DPI that reverse-included the test fixture. This is the task that makes the structural invariant hold.

**Files:**
- Delete: `c_model/include/wrap/channel_model_wrap.hpp`, `c_model/include/wrap/channel_model_wrap_io.hpp`
- Modify: `sim/c/cmodel_dpi.cpp` (remove the 4 `cmodel_channel_model_*` exports + the `#include "wrap/channel_model_wrap.hpp"`; add `#include "wrap/flit_bytes.hpp"`; KEEP the shared flit marshalling helpers)
- Modify: `sim/c/cmodel_dpi.h` (remove the `cmodel_channel_model_*` declarations)
- Modify: `sim/c/handle_block.hpp` (remove the `WrapType::ChannelModel` enum value)

**Interfaces:**
- Consumes: `c_model/include/wrap/flit_bytes.hpp` from Task 1 (for `cmodel_dpi.cpp`'s `FlitBytes` use after its old transitive include is removed).

- [ ] **Step 1: Delete the production wrap + its IO header**

```bash
git rm c_model/include/wrap/channel_model_wrap.hpp c_model/include/wrap/channel_model_wrap_io.hpp
```

- [ ] **Step 2: Remove the channel DPI from cmodel_dpi.cpp, keep shared helpers**

In `sim/c/cmodel_dpi.cpp`:
- Remove `#include "wrap/channel_model_wrap.hpp"`.
- Add `#include "wrap/flit_bytes.hpp"` (cmodel_dpi.cpp uses `FlitBytes` and lost its transitive include).
- Delete the four `extern "C"` functions `cmodel_channel_model_create`, `cmodel_channel_model_set_inputs`, `cmodel_channel_model_tick`, `cmodel_channel_model_get_outputs` in full.
- DO NOT touch the flit pack/unpack marshalling helpers (used by router/NMU/NSU).

Verify the helpers survive and the channel exports are gone:
Run: `grep -n "cmodel_channel_model\|flit_to_bytes\|flit_from_bytes\|FlitBytes" sim/c/cmodel_dpi.cpp`
Expected: no `cmodel_channel_model*` lines; the flit-helper / FlitBytes lines remain.

- [ ] **Step 3: Remove the channel DPI declarations from the header**

In `sim/c/cmodel_dpi.h`: delete the four `cmodel_channel_model_*` prototype declarations.
Run: `grep -n "cmodel_channel_model" sim/c/cmodel_dpi.h`
Expected: no output.

- [ ] **Step 4: Remove the enum value**

In `sim/c/handle_block.hpp`: delete the `ChannelModel` entry from the `WrapType` enum and any `case WrapType::ChannelModel` / validation branch tied to it.
Run: `grep -rn "WrapType::ChannelModel\|ChannelModelWrap" sim/ c_model/include/`
Expected: no output.

- [ ] **Step 5: Prune the orphaned tests (same task, so the ctest gate is real)**

These tests reference the symbols just deleted, so they must go BEFORE this task runs ctest:
```bash
git rm c_model/tests/wrap/test_channel_model_wrap.cpp
```
In `c_model/tests/wrap/CMakeLists.txt`: remove the `test_channel_model_wrap` target/registration lines + its now-stale include-path comment.
In `c_model/tests/wrap/test_cmodel_dpi.cpp`: delete every block calling `cmodel_channel_model_create` / `_set_inputs` / `_tick` / `_get_outputs` (lines ~35, ~56, ~62, ~114) and any `ChannelModelInputs/Outputs` usage. Leave master/nmu/nsu/router DPI coverage intact.
Run: `grep -rn "cmodel_channel_model\|ChannelModelInputs\|ChannelModelOutputs\|channel_model_wrap" c_model/tests/wrap/`
Expected: no output.

- [ ] **Step 6: Assert the structural invariant**

Run: `grep -rn '#include "common/' c_model/include/`
Expected: NO output — production headers no longer reverse-include any test fixture. This is the primary success criterion.

- [ ] **Step 7: clang-format + build + ctest (real green gate)**

Run: `clang-format -i sim/c/cmodel_dpi.cpp sim/c/cmodel_dpi.h sim/c/handle_block.hpp`
Run: `make test PYTHON3=python3 && make build-verilator PYTHON3=python3`
Expected: build succeeds with no dangling channel symbols; ctest 0 failed (pass count drops by the removed channel-wrap test relative to the 567 baseline); tb_top verilates.

- [ ] **Step 8: Commit**

```bash
git add -A sim/c c_model/include/wrap c_model/tests/wrap
git commit -m "refactor(wrap): delete ChannelModelWrap + channel DPI, sever layering inversion

Production include/ no longer reverse-includes tests/common. ChannelModel is now
a test-only stub. Shared flit marshalling helpers in cmodel_dpi.cpp retained.
Orphaned test_channel_model_wrap + channel section of test_cmodel_dpi pruned.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Stale-comment + doc sync (spec unit d)

Now that the code is clean, sync the comment + docs that still describe the retired testbench / DPI. No build-graph dependency.

**Files:**
- Modify: `c_model/tests/common/channel_model_params.hpp` (fix the stale "production ChannelModelWrap" comment → test-only)
- Modify (doc sync): `docs/architecture.md`, `README.md`, `docs/development.md`, `docs/issue/ARCHITECTURE.md`, `CLAUDE.md`
- Untouched (historical record): `docs/slides/genamba-role1-port-SLIDES.md`

- [ ] **Step 1: Fix the stale params comment**

In `c_model/tests/common/channel_model_params.hpp`: update the comment that references "production ChannelModelWrap" to state ChannelModel is a test-only stub. Content-only; no code change.

- [ ] **Step 2: Doc sync**

Edit each doc to remove/correct descriptions of the retired `tb_genamba` flow and `cmodel_channel_model_*` DPI:
- `docs/architecture.md`, `README.md`, `docs/development.md`, `docs/issue/ARCHITECTURE.md`: drop `run-genamba` usage / tb_genamba diagrams / channel-DPI references; where a NoC stub is described, state ChannelModel is test-only.
- `CLAUDE.md`: SURGICAL — in the wrap-layer component list, remove `ChannelModelWrap` from the enumerated wraps. KEEP the architecture line that calls `ChannelModel` a "test stub" (still accurate). Do NOT touch the "no router class in c_model" line — that is pre-existing doc accuracy, orthogonal to this change.
- `docs/slides/genamba-role1-port-SLIDES.md`: leave unchanged (delivered milestone report, historical).

Verify no live doc still advertises the removed make target:
Run: `grep -rn "run-genamba\|cmodel_channel_model" README.md docs/architecture.md docs/development.md docs/issue/ARCHITECTURE.md CLAUDE.md`
Expected: no output.

- [ ] **Step 3: Commit**

```bash
git add -A c_model/tests/common docs README.md CLAUDE.md
git commit -m "docs: sync tb_genamba + channel-DPI references to test-only ChannelModel

Fix channel_model_params stale comment, sync architecture/README/development/issue
docs + CLAUDE wrap list. Slides kept as historical record.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Final regression gate

Confirm the full suite + co-sim are green end-to-end before handing back.

**Files:** none (verification only)

- [ ] **Step 1: Full check gate**

Run: `make check PYTHON3=python3`
Expected: lint_scenarios + lint_docs + specgen_pytest pass; build-cmodel + build-verilator succeed; full ctest 0 failed; neighbor smoke co-sim passes with the scoreboard `PASS` marker.

- [ ] **Step 2: One mesh co-sim spot-check across a multi-VC topology**

Run: `make sim TB=mesh_4x4_vc4 PATTERN=neighbor PYTHON3=python3`
Expected: run completes with `PASS: scenario complete, scoreboard clean` (confirms the FlitBytes rehome + DPI surgery did not disturb the live flit wire path).

- [ ] **Step 3: Re-assert the invariant one last time**

Run: `grep -rn '#include "common/' c_model/include/ ; grep -rn "genamba" Makefile sim/build_config.mk sim/verilator/Makefile sim/vcs/Makefile`
Expected: no output from either grep.

---

## Self-Review

**Spec coverage:** unit (b) → Task 1; unit (c) → Task 2; unit (a) → Task 3 (incl. orphan-test prune so its gate compiles); unit (d) → Task 4 (comment + doc sync); success criteria (invariant, ctest, tb_top build, targets gone) → Tasks 3/5. Sequencing (b→c→a→d) matches spec. All covered.

**Placeholder scan:** new header content is concrete; deletions name exact files; each grep/make command has an expected result. No TBD/TODO.

**Type consistency:** `FlitBytes` / `FLIT_BYTES` / `FLIT_VEC_WORDS` defined once in Task 1's `flit_bytes.hpp`, consumed unchanged in Tasks 1/3 (`test_nsu_wrap.cpp` gets it transitively via `nsu_wrap.hpp` — no direct edit). `WrapType::ChannelModel` removed in Task 3 only after its last consumer (channel DPI) is deleted in the same task. Build-order hazards both resolved: genamba-tops-vs-deleted-DPI by Task 2 preceding Task 3; orphan-tests-vs-deleted-symbols by folding the test prune into Task 3 (Step 5) before its ctest gate. `grep genamba` gate is comment-inclusive (Task 2 scrubs stale comments in root Makefile + build_config.mk).
