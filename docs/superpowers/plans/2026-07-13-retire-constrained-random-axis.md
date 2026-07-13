# Retire the constrained_random co-sim axis — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the constrained_random co-sim axis (rand_master + reorder_compare + RAND_RESP slave + its params/pattern) from the testbench, leaving the directed axis (`axi_file_master` + `axi_scoreboard` + `MAPPED=1` slave) as the sole co-sim data-integrity check.

**Architecture:** Deletion-only round. The testbench today carries two flavors behind `ifdef TB_DIRECTED`; collapse to the directed arm everywhere, drop the vendored `axi_reorder_compare.sv` from the build, and remove the `constrained_random` run path from all three Makefiles. No new code, no C++/RTL logic change.

**Tech Stack:** SystemVerilog (Verilator/VCS co-sim), Python tb generator (`gen_tb_top.py`), GNU Make, pulp AXI VIP.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-13-retire-constrained-random-axis-design.md`.
- Build/co-sim run on WSL/Linux only. Use `BUILD_ROOT=$HOME/noc_build`, `PYTHON3=python3` (the repo-root `local.mk` sets these). The `/mnt/e/...` build tree is Windows-COFF; WSL `ld` rejects it.
- Do NOT push. Stop at the working tree / local commits; the user pushes explicitly.
- Do NOT edit vendored IP internals. Removing an unused vendored FILE from the build + inventory is allowed; editing its contents is not.
- No `--no-verify`. Every commit compiles and keeps ctest green.
- This round touches only `.sv` / `.py` / `Makefile` / `.mk` / `.f` / `.md` — no `.hpp`/`.cpp`, so no clang-format step.

---

### Task 1: Remove the constrained_random axis from tb sources + build system

This is one atomic build unit: a partial removal does not compile (the tb instantiates classes the Makefile/filelist still reference, or vice versa). Edit all of it, then verify the directed build is green, then one commit.

**Files:**
- Modify: `sim/tb/user_node_endpoint.sv` (params `:49-50`, `MAX_BURST_LEN` `:176-180`, both `ifdef TB_DIRECTED` regions `:181-216` and `:299-365`, stale comments `:1-21,:78,:125,:192`)
- Modify: `sim/tb/axi_vip_types_pkg.sv` (stale comment `:24`)
- Modify: `sim/tools/gen_tb_top.py` (watchdog `:504-505`, reorder_compare block `:648-707`, exit guard `:764-768`, stale comments `:27,:430-437,:476`)
- Modify: `Makefile` (help text `:38`, `RUN_CLASS` default `:130`, launcher `:171-208`)
- Modify: `sim/verilator/Makefile` (`run-constrained-random` recipe, `RUN_CLASS` validation `:15`)
- Modify: `sim/vcs/Makefile` (`:5-9`, `:143`)
- Modify: `sim/build_config.mk` (`:100`)
- Delete: `sim/dv/floonoc-test/axi_reorder_compare.sv`
- Modify: `sim/dv/README.md` (vendored inventory entry for `axi_reorder_compare.sv`)
- Regenerate (NOT committed — `sim/filelist_*.f` are gitignored build artifacts, `.gitignore:48`): the `make build` step regenerates them without the `axi_reorder_compare.sv` line.

**Interfaces:**
- Consumes: nothing (start of round).
- Produces: a directed-only testbench. `make sim TB=<topo> PATTERN=<neighbor|transpose|uniform_random|hotspot>` is the only co-sim entry point; `PATTERN=constrained_random` no longer exists.

- [ ] **Step 1: `user_node_endpoint.sv` — delete the constrained_random typedefs**

In the `ifdef TB_DIRECTED` typedef region (`:181-216`), delete the `else` arm (the `axi_rand_master` typedef and the `MAPPED=0`/`RAND_RESP` `axi_rand_slave` typedef) and the `` `else ``/`` `endif `` markers, keeping the directed arm (`axi_file_master`, `axi_scoreboard`, `MAPPED=1` `axi_rand_slave`) as unconditional code.

- [ ] **Step 2: `user_node_endpoint.sv` — delete the constrained_random driver block**

In the driver region (`:299-365`), delete the `` `else `` arm (`:331-365`: `rand_master`/`rand_slave` declarations + the two `initial` blocks driving `rand_master.run` and `rand_slave.run`) and the `` `ifdef TB_DIRECTED ``/`` `endif `` markers, keeping the directed two-phase `initial` (`:299-330`) as unconditional code.

- [ ] **Step 3: `user_node_endpoint.sv` — delete now-dead params**

Delete the `MAX_READ_TXNS_IN_FLIGHT` / `MAX_WRITE_TXNS_IN_FLIGHT` parameters (`:49-50`) and the `MAX_BURST_LEN` localparam (`:176-180`). All three were consumed only by the deleted `rand_master`. Delete the trailing comma / fix the parameter list syntax after removing `:49-50`.

- [ ] **Step 4: `gen_tb_top.py` — delete the reorder_compare emission + cmp_eos declaration**

Delete the emission block that writes the comment header + `logic cmp_eos [NUM_NODES];` + the `` `ifndef TB_DIRECTED `` … `` `endif `` `g_compare` generate loop (`:648-707`). `cmp_eos` is removed entirely (its only remaining uses are handled in Steps 5-6).

- [ ] **Step 5: `gen_tb_top.py` — drop cmp_eos from the watchdog $display**

In the watchdog (`:504-511`), remove `cmp_eos=%0d` from the format string and `cmp_eos[i]` from the argument list. Result (the `$display` emitted line):

```python
    w('            $display("[WATCHDOG] node%0d txn_cnt=%0d end_of_sim=%0d mst[awv=%0d wv=%0d arv=%0d rr=%0d br=%0d] slv[awv=%0d wv=%0d arv=%0d rv=%0d bv=%0d]",')
    w("                     i, txn_cnt[i], end_of_sim[i],")
```

- [ ] **Step 6: `gen_tb_top.py` — collapse the exit-guard ifdef**

Replace the `` `ifdef TB_DIRECTED ``/`` `else ``/`` `endif `` in the exit guard (`:764-768`) with the directed arm only:

```python
    w("            for (int i = 0; i < NUM_NODES; i++)")
    w("                all_done &= end_of_sim[i];  // scoreboard is in-endpoint")
```

- [ ] **Step 7: Root `Makefile` — directed-only routing + fail-loud on unknown PATTERN**

Four edits in the root `Makefile`:

(a) `RUN_CLASS` default (`:130`) — **blocker if missed**: `make build` passes `RUN_CLASS` down, and Step 8 narrows the child validation to directed-only, so the default must flip:

```make
RUN_CLASS ?= directed
```

(b) Help text (`:38`) — drop the `... or constrained_random` clause.

(c) `_CLASS` (`:180`) — replace with PATTERN validation (the four directed patterns), error otherwise:

```make
_VALID_PATTERNS := neighbor transpose uniform_random hotspot
ifeq ($(filter $(PATTERN),$(_VALID_PATTERNS)),)
$(error PATTERN must be one of: $(_VALID_PATTERNS) (got '$(PATTERN)'))
endif
```

(d) `sim:` recipe (`:199-208`) — drop the `constrained_random` branch; update the comment block (`:171-174`) to remove the `constrained_random runs rand_master + reorder_compare` sentence:

```make
.PHONY: sim
sim:
	@echo ">>> sim TB=$(_TOPO) PATTERN=$(PATTERN) SEED=$(_SEED)"
	$(MAKE) -C sim/verilator run-directed TOPOLOGY=$(_TOPO) RUN_CLASS=directed \
	    PATTERN=$(PATTERN) SEED=$(_SEED) $(_INJ_ARGS) $(if $(HOTSPOT),HOTSPOT=$(HOTSPOT))
```

- [ ] **Step 8: `sim/verilator/Makefile` — delete the run-constrained-random recipe**

Find the `run-constrained-random:` target (and any `.PHONY`/help line naming it) and delete it. Leave `run-directed` untouched. If `RUN_CLASS` is validated here against `directed constrained_random`, narrow it to `directed`.

- [ ] **Step 9: `sim/vcs/Makefile` — directed-only**

Set `RUN_CLASS ?= directed` (`:6`), narrow the validation (`:7-9`) to `directed` only, and update the comment (`:5`). Make the `TB_DIRECTED` define (`:143`) unconditional (it is the only class now).

- [ ] **Step 10: `sim/build_config.mk` — drop reorder_compare from the source list**

Delete the `$(DV_ROOT)/floonoc-test/axi_reorder_compare.sv \` line (`:100`). Keep `axi_bw_monitor.sv` (`:99`).

- [ ] **Step 11: Delete the vendored file + inventory entry**

Delete `sim/dv/floonoc-test/axi_reorder_compare.sv`. Remove its line from the `sim/dv/README.md` vendored inventory. (`sim/filelist_*.f` are gitignored generated artifacts, `.gitignore:48` — they are NOT edited or committed; the `make build` in Step 14 regenerates them from the updated `build_config.mk` without the `axi_reorder_compare.sv` line.)

- [ ] **Step 12: Clean stale comments (so grep-clean passes)**

Remove or reword the comments that still name the removed axis, or Step 13's grep-clean fails:
- `sim/tb/user_node_endpoint.sv`: header block `:1-21`, and `:78,:125,:192` (references to `rand_master` / `axi_reorder_compare` / `default : constrained_random`).
- `sim/tb/axi_vip_types_pkg.sv:24`.
- `sim/tools/gen_tb_top.py`: `:27`, `:430-437` (the `Checking: one FlooNoC axi_reorder_compare...` comment emitted into the tb), `:476` (`rand_master's MAX_BURST_LEN`). Note `:430-437` is emitted into the generated tb_top, so it must go with the reorder_compare block.

- [ ] **Step 13: Grep-clean check**

Run:

```bash
grep -rnE "constrained_random|reorder_compare|rand_master|MAX_READ_TXNS_IN_FLIGHT|MAX_WRITE_TXNS_IN_FLIGHT|cmp_eos|MAX_BURST_LEN" \
  sim/tb sim/tools Makefile sim/verilator/Makefile sim/vcs/Makefile sim/build_config.mk 2>/dev/null
```

Expected: no hits. (Doc hits are handled in Task 2; `sim/dv/axi-0.39.7/src/axi_test.sv` legitimately still defines `axi_rand_master` — that is the vendored VIP, not our instantiation, and is not in the grep scope above.)

- [ ] **Step 14: Build + ctest (WSL)**

Run:

```bash
make test PYTHON3=python3
```

Expected: c_model builds, full ctest suite PASS (same count as before this round; no test referenced constrained_random).

- [ ] **Step 15: Directed co-sim smoke (WSL)**

Run:

```bash
make sim TB=tb_mesh_4x4_vc1 PATTERN=neighbor PYTHON3=python3
```

Expected: `DIRECTED PASS ... scoreboard clean, non-vacuous` (rc=0). tb generation, compile, and run all succeed with the collapsed directed-only tb.

- [ ] **Step 16: Unknown-PATTERN fail-loud check**

Run:

```bash
make sim TB=tb_mesh_4x4_vc1 PATTERN=constrained_random PYTHON3=python3; echo "rc=$?"
```

Expected: fails loud with `PATTERN must be one of: neighbor transpose uniform_random hotspot (got 'constrained_random')`, rc≠0.

- [ ] **Step 17: Commit**

```bash
git add sim/tb/user_node_endpoint.sv sim/tb/axi_vip_types_pkg.sv sim/tools/gen_tb_top.py \
    Makefile sim/verilator/Makefile sim/vcs/Makefile sim/build_config.mk sim/dv/README.md
git rm sim/dv/floonoc-test/axi_reorder_compare.sv
git commit -m "refactor(cosim): retire constrained_random axis, directed scoreboard is sole checker"
```

(`sim/filelist_*.f` are gitignored — not staged.)

---

### Task 2: Reconcile docs to the single directed axis

**Files:**
- Modify: `docs/architecture.md`, `docs/verification-environment.md`, `docs/development.md` (two-checker description)
- Modify: `docs/cosim-log.md` (`:36`, `CR PASS`)
- Modify: `docs/2026-07-02-pulp-axi-vip-node-design.md` (`:33`, unified `axi_reorder_compare`)
- Modify: `docs/backlog.md` (`:374`, `PATTERN=constrained_random` run interface)
- Modify: `docs/superpowers/specs/2026-07-04-checked-traffic-benchmark-design.md` (two-checker model)

**Interfaces:**
- Consumes: Task 1's directed-only testbench.
- Produces: docs consistent with a single directed scoreboard axis.

- [ ] **Step 1: Find every stale reference**

```bash
grep -rnE "constrained_random|reorder_compare|two-checker|transport checker" docs/
```

- [ ] **Step 2: Edit each hit**

For each file above: replace the two-checker / constrained_random description with the single directed scoreboard axis. Add one line where appropriate: the constrained_random axis is retired pending a proper random-traffic verification method (see the 2026-07-04 checked-traffic-benchmark spec for the prior model). In `docs/backlog.md`, strike the `PATTERN=constrained_random` run-interface claim and add a short "retired constrained_random axis; random-traffic verification deferred" backlog line.

- [ ] **Step 3: Grep-clean docs (except deliberate history)**

```bash
grep -rnE "constrained_random|reorder_compare" docs/
```

Expected: remaining hits are only deliberate history (the two design specs describing what WAS there, clearly past-tense) — no live doc claims the axis exists.

- [ ] **Step 4: Commit**

```bash
git add docs/
git commit -m "docs: reconcile co-sim docs to single directed scoreboard axis"
```

---

### Task 3: Verify the binding path still holds on HEAD

The removal cleared the constrained_random red mark (the `axi_reorder_compare` misuse). Confirm the response VC binding still holds under the same-dest bypass path the necessity proof used — the directed scoreboard is the data-integrity check.

**Files:**
- None modified. Verification run only.

**Interfaces:**
- Consumes: Task 1's directed-only testbench, the RoB-Enabled tb, and the binding on the current branch.
- Produces: evidence the binding is intact; readiness to close `docs/clause2-vc-safe-bypass`.

- [ ] **Step 1: Same-dest deep co-sim on the RoB-Enabled tb (WSL)**

Run (the vc8 RoB tb + hotspot at the necessity-proof depth):

```bash
make sim TB=tb_mesh_4x4_vc8_rob PATTERN=hotspot INJECTION_COUNT=200 BURST_LEN=63 PYTHON3=python3
```

Expected: `DIRECTED PASS ... scoreboard clean, non-vacuous` (rc=0). The scoreboard compares readback data; a binding regression here reorders same-(dst,id) responses and produces a `handle_read` data mismatch (demonstrated once in the necessity proof).

- [ ] **Step 2: Record the result**

Note the run + rc in the branch's verification log / commit message for the parked binding-verification commit. No re-fault-injection (dropped this round, per spec).

- [ ] **Step 3: Branch handoff**

Stop here for the branch-landing decision (merge / PR / close `docs/clause2-vc-safe-bypass`) — use the finishing-a-development-branch skill with the user. Do NOT push without explicit user say-so.

---

## Notes for the executor

- **Branch:** all commits land on the current `docs/clause2-vc-safe-bypass` branch (user decision — no separate branch). The removal unblocks the constrained_random red mark that was the branch's last blocker, so it belongs on the same branch as the binding work.
- **Do not commit `sim/filelist_*.f`** — they are gitignored generated artifacts (`.gitignore:48`), regenerated by `make build`.
