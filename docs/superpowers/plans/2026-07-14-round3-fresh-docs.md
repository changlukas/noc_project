# Round 3 Fresh Doc Set Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the accumulated doc set with four fresh documents matching the current tree exactly, prove the build→sim path by execution, and finish the release packaging (spec: `docs/superpowers/specs/2026-07-14-round3-fresh-docs-design.md`).

**Architecture:** Nine tasks: two mechanical ride-along batches (source cleanups, packaging files) land BEFORE any doc is written so docs describe the final tree; the four docs are written next (each writer reads the dying docs before Task 7 deletes them); deletion + CLAUDE.md/specgen-README come after; a doc-driven dry run proves README; final gates + cross-review close.

**Tech Stack:** Markdown docs, GNU make, WSL-native gate flow (rsync → `~/noc_project`).

## Global Constraints

- Branch: continue on `refactor/commercialization-round2` (the whole commercialization stack merges together; no new branch).
- Style charter (spec §Style): current README shape, English, tables over prose, only what exists, NO file line numbers in docs, no AI-tell mannerisms (no em-dash runs, no "notably/additionally", no rule-of-three padding).
- External vendor/IP names in the fresh docs: ONLY inside verification-environment.md's provenance section. In-code "Ported from floo_*.sv" comments stay untouched.
- Every doc claim verified against the tree BEFORE writing; writers work from the checklists in their task, not memory.
- Commit format `type(scope): description`, English. NO push.
- Canonical gates (WSL foreground, single-session; E_UNEXPECTED once → retry, twice → STOP):
  - ctest: `wsl bash -lc "rsync -a --delete --exclude=/build --exclude=/.git /mnt/e/05_NoC/noc_project/ ~/noc_project/ && make -C ~/noc_project test BUILD_ROOT=\$HOME/noc_build PYTHON3=python3 2>&1 | grep -E 'tests passed|tests failed'"` — expect `100% tests passed, 0 tests failed out of 431`.
  - specgen pytest: `cd specgen && py -3 -m pytest tests/ -q` — expect `160 passed`; afterwards run `git restore specgen/generated` (pytest rewrites banners in-tree, known flaw).
  - sim: same rsync prefix + `make -C ~/noc_project sim TB=<t> PATTERN=<p> BUILD_ROOT=\$HOME/noc_build PYTHON3=python3` — expect `DIRECTED PASS`.
- Facts locked by the spec (do not re-litigate): CMake floor = 3.20; canonical TB form = bare topology name (`mesh_4x4_vc1`); topologies = the 7 YAMLs in `sim/topologies/` (+ `_rob` name variants); patterns = `neighbor transpose uniform_random hotspot`; platform matrix = Linux/WSL full flow (dry-run verified) / Windows ctest-only (declared) / VCS build-only (declared).

---

### Task 1: Source-side ride-alongs

**Files:**
- Delete: `src/c_model/include/axi/ATTRIBUTION.md`
- Modify: the 12 files with first-line ATTRIBUTION comments (list below), `specgen/tools/gen_inventory.py`, `src/c_model/FEATURE_INVENTORY.md` (regen), `Makefile` (one comment)

**Interfaces:** none; later doc tasks assume `grep -rn "ATTRIBUTION" src/` is empty.

- [ ] **Step 1: Delete the attribution file and its 12 reference comments (whole first line each)**

```bash
git rm src/c_model/include/axi/ATTRIBUTION.md
```

Delete line 1 in each of (user decision: entire line, including the "independent design" variants):
`src/c_model/include/axi/types.hpp`, `memory.hpp`, `memory_port.hpp`, `axi_master.hpp`,
`axi_slave.hpp`, `scoreboard.hpp`, `scenario_parser.hpp`;
`src/c_model/tests/axi/mock_memory_port.hpp`, `test_axi_master.cpp`, `test_axi_slave.cpp`,
`test_axi_master_inject.cpp`, `test_memory.cpp`.

Verify: `grep -rn "ATTRIBUTION" src/` → no output.

- [ ] **Step 2: gen_inventory.py content paths**

In `specgen/tools/gen_inventory.py` lines 27-47 region, change every content string
`c_model/include/...` → `src/c_model/include/...` (docstring, dict values, f-strings, argparse
description — content strings only; the DEFAULT_OUT path constant was already fixed in c75d549).
Then regenerate: `py -3 specgen/tools/gen_inventory.py` and stage the updated
`src/c_model/FEATURE_INVENTORY.md`. Verify: `py -3 specgen/tools/gen_inventory.py --check` → no diff.

- [ ] **Step 3: Makefile stale CMake comment**

`Makefile` (comment near line 82): `cmake >= 3.14` → `cmake >= 3.20` (align with
`src/c_model/CMakeLists.txt:1`).

- [ ] **Step 4: Gates + commit**

ctest gate → 431/431. specgen pytest → 160 passed (then `git restore specgen/generated`).

```bash
git add -u src specgen Makefile
git commit -m "chore(release): drop attribution file refs, fix inventory paths, cmake comment"
```

---

### Task 2: Packaging files

**Files:**
- Modify: `LICENSE`, `sim/dv/README.md`, `.gitignore`; untrack `docs/backlog.md`

**Interfaces:** Task 3's README License section must agree with the post-edit LICENSE.

- [ ] **Step 1: LICENSE — delete the third-party section**

Remove exactly this block (leaving the proprietary text + AS-IS disclaimer intact, one blank line
between the remaining paragraphs):

```
THIRD-PARTY COMPONENTS
Portions of the Software under c_model/include/axi/ are derived from
cocotbext-axi and remain licensed under the MIT License. See
c_model/include/axi/ATTRIBUTION.md for the applicable notice. This
proprietary license does not supersede that third-party license for those
files.
```

- [ ] **Step 2: sim/dv/README.md — D8 modified-flag**

Header line `# sim/dv — imported DV IP (verbatim, do not edit)` →
`# sim/dv — imported DV IP (do not edit; one flagged local modification below)`.
In the table, add a `modified` column: `-` for the three pulp packages, and for the
`floonoc-test` row: `yes — 2-line $display latency-N addition in axi_bw_monitor.sv (consumed by
sim/tools/emit_result_csv.py)`.

- [ ] **Step 3: .gitignore + backlog untrack**

- Remove the line `/docs/image/*.jpg` (contradicts 6 tracked jpgs, L4-011).
- Add `/docs/backlog.md` under the docs section.
- `git rm --cached docs/backlog.md` (file stays on disk as the local working doc).
- (`/cross-review/` and `/.superpowers/` are already ignored — verify, do not duplicate.)

- [ ] **Step 4: Commit**

No gates needed (no code touched):

```bash
git add LICENSE sim/dv/README.md .gitignore
git commit -m "chore(release): license third-party section out, dv modified-flag, backlog local-only"
```

---

### Task 3: README.md rewrite

**Files:**
- Rewrite: `README.md`
- Read first (they die in Task 7): `docs/development.md` (§3 toolchain, §4 regen, build
  portability), `docs/architecture.md` (§1 system context), current `README.md` (style/shape).

**Interfaces:**
- Produces: the Documentation section links `docs/spec.md`, `docs/trade-off.md`,
  `docs/verification-environment.md` (created by Tasks 4-6), `specgen/docs/guide/index.md`.
- Consumes: LICENSE wording from Task 2.

- [ ] **Step 1: Verify every operational fact against the tree** (checklist — record findings in
  your report; each item must come from the named source, not memory):

| fact | source of truth |
|---|---|
| make targets + variables shown by `make help` | root `Makefile` help text |
| CMake floor 3.20 | `src/c_model/CMakeLists.txt:1` |
| Verilator versions (5.048 WSL primary / 5.036 note) | `docs/development.md` §3 + `sim/build_config.mk` comments |
| topology list | `ls sim/topologies/` (7 YAMLs; `_rob` suffix = RoB-enabled tb variant, name-derived) |
| pattern list | `sim/tools/gen_test_patterns.py` choices |
| sim invocation + defaults (TB=mesh_4x4_vc1 PATTERN=neighbor, SEED behavior) | root `Makefile` sim target |
| output location `sim/<simulator>/output/<run-tag>/run.log` | `sim/verilator/Makefile` run rules |
| PASS line formats (`DIRECTED PASS: ... scoreboard clean, non-vacuous`, `[Monitor nodeN.master]`, `[HWM]`) | a real run.log or the emitting SV/tools |
| offline build (`DEPS_SRC`, `FETCHCONTENT_FULLY_DISCONNECTED`), `local.mk` overrides | root `Makefile` comments + `docs/development.md` |
| specgen regen commands (`codegen.py --target {cpp,sv} --domain {packet,signals,params,noc_types}`, `--check`) | `specgen/tools/codegen.py` docstring |
| contributing gate list | current README Contributing + CLAUDE.md quality gates |

- [ ] **Step 2: Write README.md** with exactly the spec's section table (header positioning /
  Status readiness statement without perishable numbers / Architecture ASCII + where-code-lives
  (`src/c_model`, `src/sv`, `src/dpi`, `sim/`, `specgen/`, `docs/`) / Prerequisites + platform
  matrix (Linux-WSL full — dry-run verified; Windows ctest-only — declared; VCS build-only —
  declared) / Build / Test (make test, make specgen_pytest, codegen --check) / Simulate (canonical
  bare-name TB form, TWO examples that Task 8 will execute, output reading) / Regenerate /
  Documentation / Contributing / License aligned with post-Task-2 LICENSE, no third-party text).
  Keep the current README's length class (~100 lines).

- [ ] **Step 3: Self-verify** — every command in the README copy-pasted and syntax-checked against
  the Makefiles (`make -n <target>` dry-run where possible); no reference to deleted/dying docs;
  no line numbers; grep the README for `run-tb-top|Stage|c_model/|tools/|test_patterns` → no
  stale-path hits (`src/c_model` is fine, bare `c_model/` is not).

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs(readme): rewrite against current tree with verified build-to-sim path"
```

---

### Task 4: docs/spec.md

**Files:**
- Create: `docs/spec.md`
- Read first (die in Task 7): `docs/architecture.md`, `docs/nmu-rob-microarchitecture.md`,
  `docs/verification-environment.md` (old), `docs/cosim-log.md`, `docs/issue/ARCHITECTURE.md`;
  plus `docs/superpowers/audit/2026-07-14-salvage-inventory.md` (the row checklist) and the code.

**Interfaces:**
- Produces: section anchors that trade-off.md (Task 5) cross-references: "Known limitations",
  "Response ordering", "Virtual networks".

- [ ] **Step 1: Work through the salvage checklist** — every salvage-inventory row whose target
  column says `spec.md` (Half A rows 1-2,4-5,7,9-12 by order; Half B nmu-rob rows §3,§4×2,§5,§6,§8,§9;
  probe rows AS-INTENDED-DROPPED per the pass-5 CAUTION — document as-built counters only;
  pulp-vip rows Motivation/D1/D4/exclusions/watchdog; pass-3 verification-environment rows;
  pass-2 backlog known-limitation rows). For each row: read the source section, verify the claim
  against current code (names may have changed in Round 2 — e.g. clause→bypass, pools→vnets,
  landing→input register), write it in the new vocabulary.

- [ ] **Step 2: Write docs/spec.md** with the spec's section list: Overview + conformity scope ·
  architecture layering + NMU/NSU asymmetry · timing model (registered DPI tick) · SAM · NMU RoB
  (modes, idle-ID + same-destination bypass, lzc allocator, invariants, slot-pool deadlock
  guarantee, param mapping) · NSU (meta buffer, id remap) · virtual networks + fixed VC id +
  wormhole lock · DPI ABI + wrap invariant · parameters (specgen single-source) · traffic
  patterns · perf counters (as-built: router FIFO occupancy, link flit/stall, DV bw monitor;
  state explicitly that AXI-side Profile/Trace is not built) · known limitations · references
  (floo_* file map, IHI clauses, booksim). Terse, tables where parallel.

- [ ] **Step 3: Self-verify** — grep the draft for retired vocabulary (`clause|pinned|RZ1|landing|
  pool[^s_a-z]|mosi|miso|beta-tick|manager|subordinate`) → only sanctioned uses; every identifier
  named in the doc exists in the tree (spot-grep 10 of them); no line numbers.

- [ ] **Step 4: Commit**

```bash
git add docs/spec.md
git commit -m "docs(spec): as-built design spec"
```

---

### Task 5: docs/trade-off.md

**Files:**
- Create: `docs/trade-off.md`
- Read first: `docs/superpowers/audit/2026-07-14-tradeoff-list.md` (skeleton),
  `2026-07-14-ledger.md` (keep+trade-off rationales), relevant code.

**Interfaces:** Consumes spec.md section anchors (Task 4) for cross-refs.

- [ ] **Step 1: Fill T-01..T-30** — one short section each: baseline vs ours vs rationale (2-5
  lines per entry, table where possible). Overrides the stale skeleton where Round 2 changed
  reality: T-04 = master/slave per D1 (identifiers were already master/slave; prose unified);
  T-12/T-13 vocabulary = fixed VC id / same-destination bypass; **T-18 = CLOSED by Round-2 D6a**
  (record as resolved trade-off, one line); T-23 = resolved via D8 manifest-note (Task 2);
  T-19/T-22 end with "see spec.md Known limitations".

- [ ] **Step 2: Self-verify** — same retired-vocabulary grep as Task 4 Step 3; every claim about
  what the code does spot-checked against the tree.

- [ ] **Step 3: Commit**

```bash
git add docs/trade-off.md
git commit -m "docs(trade-off): textbook baseline vs as-built rationale"
```

---

### Task 6: docs/verification-environment.md rewrite

**Files:**
- Rewrite: `docs/verification-environment.md`
- Read first: the OLD verification-environment.md (live-env rows per salvage pass-3),
  `sim/tb/user_node_endpoint.sv`, `sim/tools/gen_tb_top.py`, `sim/tools/gen_test_patterns.py`,
  `sim/dv/README.md` (post-Task-2).

**Interfaces:** carries the doc set's ONLY external-IP provenance section.

- [ ] **Step 1: Write sections** — testbench architecture (generated tb_top + noc_fabric per
  topology YAML, per-node user_node_endpoint layout) · VIP set (axi_file_master directed
  two-phase, axi_rand_slave MAPPED tile memory, axi_scoreboard manager-face integrity,
  axi_bw_monitor) · **provenance**: one paragraph — AXI master/slave/memory model algorithms in
  `src/c_model/include/axi/` ported from cocotbext-axi (MIT); vendored pulp/FlooNoC DV packages
  under `sim/dv/` (Solderpad 0.51), details in `sim/dv/README.md` · traffic-pattern semantics
  (neighbor diagonal-wrap, transpose y/x, hotspot many-to-one, uniform_random; booksim
  provenance) · checkers + non-vacuous pass (txn_cnt gate per node + scoreboard zero-mismatch)
  · topology YAML → generator → tb workflow · seed handling (SEED= or random 30-bit printed).

- [ ] **Step 2: Self-verify** — every module/plusarg/make-variable named exists (grep each);
  retired-vocabulary grep clean.

- [ ] **Step 3: Commit**

```bash
git add docs/verification-environment.md
git commit -m "docs(verification): test environment as built"
```

---

### Task 7: Deletion scope + CLAUDE.md + specgen README

**Files:**
- Delete: `docs/architecture.md`, `docs/development.md`, `docs/cosim-log.md`,
  `docs/nmu-rob-microarchitecture.md`, `docs/performance-probe.md`, `docs/pg037_axi_perf_mon.md`,
  `docs/2026-07-02-pulp-axi-vip-node-design.md`, `docs/internal/`, `docs/issue/`, `docs/slides/`,
  `docs/superpowers/`
- Modify: `CLAUDE.md`; Create: `specgen/README.md`

- [ ] **Step 1: Execute deletions**

```bash
git rm docs/architecture.md docs/development.md docs/cosim-log.md \
  docs/nmu-rob-microarchitecture.md docs/performance-probe.md docs/pg037_axi_perf_mon.md \
  docs/2026-07-02-pulp-axi-vip-node-design.md
git rm -r docs/internal docs/issue docs/slides docs/superpowers
```

Keep `docs/image/` untouched. `docs/backlog.md` already untracked (Task 2) — leave the file on disk.

- [ ] **Step 2: CLAUDE.md** — fix every L3-001 stale claim: router paragraph (there IS a router
  class under `src/c_model/include/router/`; ChannelModel is a tests-only stub, not the co-sim
  datapath), SAM range-lookup replaces the xy_route bit-slice claim, remove MasterWrap/SlaveWrap +
  `cmodel_master/slave_create` (only Nmu/Nsu/Router wraps exist — verify with
  `grep -rn "cmodel_.*_create" src/dpi/cmodel_dpi.h`), config claim ("YAML c_model/config, no
  JSON" is false — specgen JSON is the source; check what exists), all `c_model/...` path
  prefixes → `src/c_model/...`, drop the `docs/_archive` pointer, repoint Key Design Docs at
  `docs/spec.md` + `docs/backlog.md` (local, gitignored). Keep the rest (communication rules,
  quality gates) untouched.

- [ ] **Step 3: specgen/README.md** (new, one screen): what specgen owns (JSON/YAML sources →
  generated cpp/sv/json), regen entry points (codegen.py per-domain commands), the drift gates
  (`--check`, CMake codegen_check, `make specgen_pytest`), boundary statement (generated files
  never hand-edited; ni_signals.json hand-curated input). Match main-README style.

- [ ] **Step 4: Verify nothing references deleted paths**

```bash
grep -rn "docs/architecture\|docs/development\|docs/cosim-log\|docs/nmu-rob\|docs/performance-probe\|docs/pg037\|docs/internal\|docs/issue\|docs/slides\|docs/superpowers\|_archive" \
  --include="*.md" --include="*.py" --include="*.mk" --include="Makefile*" --include="*.cmake" --include="CMakeLists.txt" \
  README.md CLAUDE.md docs/ src/ sim/ specgen/ Makefile .gitignore
```
Expected: only hits inside `docs/backlog.md` (local working doc, allowed) — fix any other.

- [ ] **Step 5: Gates + commit**

ctest 431/431 + specgen pytest 160 (deletions must not break builds/tests).

```bash
git add -A
git commit -m "docs(release): delete superseded doc set, fix CLAUDE.md, add specgen boundary README"
```

---

### Task 8: Doc-driven dry run

**Files:** none modified by the runner; README.md fixes by the controller/fix agent if findings.

- [ ] **Step 1: Dispatch a FRESH subagent whose ONLY repo input is README.md** (it may not read
  Makefiles, docs/, or this plan). Instructions: on WSL, from a clean copy
  (`rsync -a --delete --exclude=/build --exclude=/.git /mnt/e/05_NoC/noc_project/ ~/noc_dryrun/`),
  follow README literally: prerequisites check → build → test → the README's BOTH sim examples →
  interpret outputs per the README's reading guide. Report every step where the doc was wrong,
  ambiguous, or insufficient (doc bugs), with the exact command run and observed output.
  BUILD_ROOT note: the runner uses whatever the README says — if the README needs a BUILD_ROOT
  hint for out-of-tree builds, that is a doc bug to surface, not to silently work around.

- [ ] **Step 2: Fix loop** — every finding = README edit (never code), re-run the failed step,
  until the runner completes end-to-end with both sims DIRECTED PASS.

- [ ] **Step 3: Commit fixes (if any)**

```bash
git add README.md
git commit -m "docs(readme): dry-run findings"
```

---

### Task 9: Final gates + cross-review + wrap

- [ ] **Step 1: Full gates on the final tree** — ctest 431/431; specgen pytest 160 (+
  `git restore specgen/generated`); codegen `--check` exit 0; one sim DIRECTED PASS.

- [ ] **Step 2: Cross-review** — Codex + fresh-context Claude review the four docs + the round's
  diff (`git diff d7ca611..HEAD`) for: doc-vs-tree truth (spot-verify claims), completeness vs the
  salvage checklist, style-charter compliance, broken links/paths. Fix findings (one fix wave),
  re-verify.

- [ ] **Step 3: Wrap** — update `.superpowers/sdd/progress.md` (Round 3 complete) and
  `docs/backlog.md` locally (strike Round-3 items; backlog is untracked now — no commit needed
  for it). Report final state to the user. NO push.
