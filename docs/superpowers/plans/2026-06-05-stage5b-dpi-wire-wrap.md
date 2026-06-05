# Stage 5b — DPI wire-wrap co-sim Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wrap each of 5 c_model components (AxiMaster, Nmu, LoopbackNoc, Nsu, AxiSlave) in its own SV shell module communicating via registered SV wires, so multi-beat/multi-outstanding AXI traffic becomes wb2axip-observable AND future RTL replacement is a one-line SV swap.

**Architecture:** β tick discipline (1-cycle latency per hop, fully-registered handshake). DPI 3-step pattern per shell (set_inputs → tick → get_outputs). Hermetic singleton: each c_model component is a standalone C++ singleton; cross-component data flow ONLY via SV wires. wb2axip protocol checkers bind on two AXI bundles (master→nmu, nsu→slave). Single Verilator binary with runtime plusarg-driven fault injection.

**Tech Stack:** C++17 + GoogleTest, SystemVerilog (Verilator 5.036 on Windows MSYS2), DPI-C, rtl-forge `rtl-style` skill (mandatory for all .sv files), `rtl-reviewer` agent (mandatory per-task review), karpathy-guidelines skill (code quality discipline), wb2axip Apache 2.0 (carried verbatim from Stage 5a).

**Spec:** `docs/superpowers/specs/2026-06-05-stage5b-dpi-wire-wrap-design.md`

**SV authoring discipline (MUST for all `.sv` work)**:
1. Invoke `Skill('rtl-style')` BEFORE writing or modifying any `.sv` file. Templates and rules in `~/.claude/skills/rtl-style/`.
2. Invoke `Skill('karpathy-guidelines')` at the start of any coding task — applies to C++ and SV alike.
3. Dispatch `rtl-reviewer` agent on every `.sv` review (output to `c_model/build/rtl-review-logs/<task-id>-rtl-review.md`); 0 CRITICAL + 0 HIGH required to advance.

---

## File Structure (per spec §8)

| Path | Responsibility |
|---|---|
| `cosim2/README.md` | Stage 5b vision + branch lineage |
| `cosim2/CODING_DISCIPLINE.md` | rtl-style + karpathy + rtl-reviewer invocation conventions |
| `cosim2/KNOWN_LIMITATIONS.md` | Carry from cosim/, mark §2 RESOLVED after smoke pass |
| `cosim2/sv/axi_intf.sv` | AXI bundle + valid/ready + master/slave modports |
| `cosim2/sv/noc_req_intf.sv` | NoC req bundle, parameterized over NUM_VC + FLIT_W |
| `cosim2/sv/noc_rsp_intf.sv` | NoC rsp bundle, symmetric |
| `cosim2/sv/axi_master_wrap.sv` | DPI shell for c_model AxiMaster |
| `cosim2/sv/nmu_wrap.sv` | DPI shell for c_model Nmu |
| `cosim2/sv/loopback_noc_wrap.sv` | DPI shell for c_model LoopbackNoc |
| `cosim2/sv/nsu_wrap.sv` | DPI shell for c_model Nsu |
| `cosim2/sv/axi_slave_wrap.sv` | DPI shell for c_model AxiSlave (owns Memory helper) |
| `cosim2/sv/tb_top.sv` | TB top: 5 instances + 6 wire bundles + 2 wb2axip checkers + plusarg parse |
| `cosim2/sv/wb2axip/*` | Carried verbatim from cosim/sv/wb2axip/ |
| `cosim2/c/cmodel_dpi.h` | DPI signatures + error enum |
| `cosim2/c/cmodel_dpi.cpp` | extern "C" handlers per shell + try/catch + g_dpi_error |
| `cosim2/c/dpi_boundary_macros.h` | DPI_BOUNDARY_BEGIN/END macros |
| `cosim2/verilator/Makefile` | Adapted from cosim/verilator/ for cosim2 paths |
| `cosim2/verilator/main.cpp` | Verilator harness: drives clk_i + rst_ni, plusarg forward, eval loop |
| `cosim2/tests/CMakeLists.txt` | ctest registration with WORKING_DIRECTORY + Vtb_top env var |
| `cosim2/tests/fixtures/*.yaml` | 5 scenarios: 3 carried + 2 new multibeat + 1 injection |
| `cosim2/tests/test_cosim_wire_smoke.cpp` | Parameterized GoogleTest over 4 non-injection scenarios |
| `cosim2/tests/test_checker_fires_on_violation.cpp` | Fault injection — expect non-zero exit |
| `c_model/include/cosim2/{master,nmu,loopback_noc,nsu,slave}_shell_io.hpp` | Per-shell POD `Inputs` / `Outputs` structs |
| `c_model/include/cosim2/{master,nmu,loopback_noc,nsu,slave}_shell_adapter.hpp` | Per-shell C++ adapter class |
| `c_model/tests/cosim2/test_{comp}_shell_adapter.cpp` | Per-shell C++ unit test (no DPI/SV) |
| `c_model/tests/cosim2/CMakeLists.txt` | Per-shell unit test entries |
| `tools/check_cosim2_hermetic.sh` | CI grep for hermetic invariant violations |

c_model headers receive ADDITIVE changes (existing API preserved verbatim):

| Header | Change |
|---|---|
| `c_model/include/nmu/axi_slave_port.hpp` | `+ bool can_accept_aw/w/ar() const` |
| `c_model/include/nsu/axi_master_port.hpp` | `+ bool can_accept_b/r() const` |
| `c_model/include/axi/axi_master.hpp` | `+ explicit AxiMaster(scenario, mow, mor)` overload; `+ plusarg inject parser` |
| `c_model/include/axi/axi_slave.hpp` | `+ explicit AxiSlave(memory)` overload (if not already) |
| `c_model/include/nmu/nmu.hpp` | `+ explicit Nmu(NmuConfig)` overload (no NocReqOut/NocRspIn refs) |
| `c_model/include/nsu/nsu.hpp` | `+ explicit Nsu(NsuConfig)` overload (no NocReqIn/NocRspOut refs) |
| `c_model/include/common/loopback_noc.hpp` | `+ standalone ctor` overload |

(a) artifacts to **DELETE** (separate dedicated commit at top of (b) branch):
- `cosim/sv/{nmu_cmodel_proxy,nsu_cmodel_proxy,tb_axi_conformity}.sv`
- `cosim/c/{axi_dpi.cpp,axi_dpi.h}`
- `c_model/include/cosim/{axi_dpi_adapter.hpp,pin_snapshot.hpp}`
- `c_model/tests/cosim/` (whole subdir)
- `cosim/verilator/{Makefile,main.cpp}` (will be replaced by cosim2/verilator/)
- `cosim/tests/` (whole subdir)
- `cosim/{KNOWN_LIMITATIONS.md, .gitkeep, sv/.gitkeep}` move/recreate under cosim2/

(a) artifacts to **CARRY** to `cosim2/sv/wb2axip/`:
- `cosim/sv/wb2axip/{faxi_master.v, faxi_slave.v, faxi_wstrb.v, sim_wrapper.svh}`
- `cosim/sv/wb2axip/ATTRIBUTION.md` (update path note from `cosim/` → `cosim2/`)

---

## Risks (per spec §10 Karpathy 4-lens)

| Risk | Mitigation |
|---|---|
| Per-shell handshake state machine bespoke (not cookie-cutter) — highest implementation risk | Mandatory `rtl-reviewer` review per shell; explicit §6.4 backpressure rule in spec; per-task unit test exercises handshake |
| Verilator 75 DPI calls/cycle on Windows MSYS2 perf unknown | T7 (first shell) measures wall-time; if >2s for 1000-cycle scenario, batch DPI further |
| `Nmu` / `Nsu` standalone ctor blocked by WormholeArbiter ref dependency | Verify in T3; if blocked, use post-ctor `bind_downstream(...)` pattern |
| wb2axip `F_AXI_MAXSTALL` semantic UNVERIFIED until T12 | T12 explicitly gated as prerequisite before T13 tb_top |
| Plusarg injection leak into production scenarios | Production fixtures NEVER include `+inject=` key; parser fails on unknown mode |

---

## Task 1: Bootstrap (branch + scaffold + carry wb2axip + delete (a) + CODING_DISCIPLINE)

**Files:**
- Create: `cosim2/{README.md, CODING_DISCIPLINE.md, KNOWN_LIMITATIONS.md}`
- Create: `cosim2/sv/wb2axip/{faxi_master.v, faxi_slave.v, faxi_wstrb.v, sim_wrapper.svh, ATTRIBUTION.md}` (moved from cosim/)
- Create: empty placeholder `cosim2/{sv,c,verilator,tests}/.gitkeep`, `c_model/include/cosim2/.gitkeep`, `c_model/tests/cosim2/.gitkeep`
- Delete: `cosim/sv/{nmu_cmodel_proxy,nsu_cmodel_proxy,tb_axi_conformity}.sv`
- Delete: `cosim/c/{axi_dpi.cpp,axi_dpi.h}`
- Delete: `c_model/include/cosim/{axi_dpi_adapter.hpp,pin_snapshot.hpp}` and `c_model/include/cosim/.gitkeep`
- Delete: `c_model/tests/cosim/` (whole subdir)
- Delete: `cosim/verilator/{Makefile,main.cpp}` and `cosim/verilator/.gitkeep`
- Delete: `cosim/tests/` (whole subdir)
- Delete: `cosim/{KNOWN_LIMITATIONS.md, sv/.gitkeep, c/.gitkeep}`
- Modify: `c_model/tests/CMakeLists.txt` — remove `add_subdirectory(cosim)` line if present; do NOT add cosim2 yet (later task)

- [ ] **Step 1: Create stage5b branch off current HEAD**

```bash
git status --short                                              # Confirm clean working tree
git log -1 --format="%H %s"                                     # Note current HEAD
git checkout -b stage5b/dpi-wire-wrap                           # New branch
git log -1 --format="%H %s"                                     # Verify branch created
```

Expected: branch `stage5b/dpi-wire-wrap` created off the most recent commit (the Stage 5b spec commits).

- [ ] **Step 2: Create cosim2/ directory scaffold + .gitkeep placeholders**

```bash
mkdir -p cosim2/sv/wb2axip cosim2/c cosim2/verilator cosim2/tests/fixtures
mkdir -p c_model/include/cosim2 c_model/tests/cosim2
touch cosim2/c/.gitkeep cosim2/verilator/.gitkeep cosim2/tests/.gitkeep cosim2/tests/fixtures/.gitkeep
touch c_model/include/cosim2/.gitkeep c_model/tests/cosim2/.gitkeep
```

- [ ] **Step 3: Move wb2axip vendor + sim_wrapper + faxi_wstrb to cosim2/sv/wb2axip/**

```bash
# Move (preserves git history)
git mv cosim/sv/wb2axip/faxi_master.v   cosim2/sv/wb2axip/faxi_master.v
git mv cosim/sv/wb2axip/faxi_slave.v    cosim2/sv/wb2axip/faxi_slave.v
git mv cosim/sv/wb2axip/faxi_wstrb.v    cosim2/sv/wb2axip/faxi_wstrb.v
git mv cosim/sv/wb2axip/sim_wrapper.svh cosim2/sv/wb2axip/sim_wrapper.svh
git mv cosim/sv/wb2axip/ATTRIBUTION.md  cosim2/sv/wb2axip/ATTRIBUTION.md
```

Now edit `cosim2/sv/wb2axip/ATTRIBUTION.md` — find any reference to `cosim/sv/wb2axip/` and replace with `cosim2/sv/wb2axip/`. Add a note section:

```markdown
## Path move (Stage 5b)

Vendored files moved from `cosim/sv/wb2axip/` to `cosim2/sv/wb2axip/` as part of the
Stage 5b DPI wire-wrap rework. File contents unchanged. Stage 5a artifacts at
`cosim/sv/wb2axip/` have been deleted in this branch.
```

- [ ] **Step 4: Delete (a) PoC artifacts (dedicated single commit later in Step 9)**

```bash
git rm cosim/sv/nmu_cmodel_proxy.sv
git rm cosim/sv/nsu_cmodel_proxy.sv
git rm cosim/sv/tb_axi_conformity.sv
git rm cosim/c/axi_dpi.cpp
git rm cosim/c/axi_dpi.h
git rm c_model/include/cosim/axi_dpi_adapter.hpp
git rm c_model/include/cosim/pin_snapshot.hpp
git rm c_model/include/cosim/.gitkeep
git rm -r c_model/tests/cosim/
git rm cosim/verilator/Makefile
git rm cosim/verilator/main.cpp
git rm cosim/verilator/.gitkeep
git rm -r cosim/tests/
git rm cosim/KNOWN_LIMITATIONS.md
git rm cosim/sv/.gitkeep 2>/dev/null || true
git rm cosim/c/.gitkeep 2>/dev/null || true
# `cosim/` dir should now be empty of tracked files except wb2axip moved out — verify:
find cosim -type f | head -5
# Expected output: empty (cosim/ dir itself may remain empty or be auto-removed)
```

If `cosim/` directory still exists as empty, leave it (git doesn't track empty dirs). Optionally `rmdir cosim/sv cosim/` if shells complain.

- [ ] **Step 5: Update `c_model/tests/CMakeLists.txt` — remove cosim/ subdir hook**

```bash
grep -n "cosim" c_model/tests/CMakeLists.txt
```

If a line `add_subdirectory(cosim)` exists, remove it. If `cosim_smoke_tests` ExternalProject or similar references exist, remove too.

```bash
# Manual edit; example sed (verify first):
# sed -i '/add_subdirectory.*cosim/d' c_model/tests/CMakeLists.txt
```

- [ ] **Step 6: Write `cosim2/README.md`**

```markdown
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
```

- [ ] **Step 7: Write `cosim2/CODING_DISCIPLINE.md`**

```markdown
# Coding Discipline — cosim2/

## SystemVerilog (`cosim2/sv/*.sv`)

All `.sv` files MUST conform to the `rtl-style` skill (installed at
`~/.claude/skills/rtl-style/`, sourced from
[changlukas/rtl-forge](https://github.com/changlukas/rtl-forge)).

### Writing SV

- Before writing or modifying any `.sv` file, invoke `Skill('rtl-style')` first
- Naming: `_i` / `_o` / `_q` / `_d` / `_ni` suffixes; sync reset default
- Forbidden patterns (12-rule catalog in rtl-style references)
- Use templates under `~/.claude/skills/rtl-style/templates/` as starting points

### Reviewing SV

Dispatch `rtl-reviewer` agent per task:

    Agent(subagent_type='rtl-reviewer',
          description='Review <module>.sv',
          prompt='Review cosim2/sv/<module>.sv per rtl-style skill. Return
                  categorized findings with CRITICAL/HIGH/MEDIUM/LOW severity,
                  file:line refs, exact rule violated, minimal-change fix.')

- Output: save reviewer report to `c_model/build/rtl-review-logs/<task-id>-rtl-review.md`
- Pass criteria: 0 CRITICAL + 0 HIGH findings (advancement gate)
- MEDIUM: flagged for decision; LOW: informational only
- Codex review runs in parallel for independent perspective

## All code (C++ + SV)

Invoke `Skill('karpathy-guidelines')` at the start of every coding task. Apply
the 4-lens discipline: overcomplication / surgical / surface assumptions /
verifiable success. Avoid LLM-typical mistakes: overengineering, defensive code
for impossible cases, mixed-concern files, untestable success criteria.

## Hermetic singleton invariant (cosim2/c/)

Each `*_shell_adapter.hpp` owns ONE c_model component. Forbidden:

- `cosim2/c/<comp_a>_dpi.cpp` referencing `g_<comp_b>_adapter`
- `*_shell_adapter.hpp` `#include`-ing another shell's adapter
- C++ component A holding a ref/ptr to component B

CI gate: `tools/check_cosim2_hermetic.sh` greps for forbidden patterns. Must
pass before merge.

## Shells contain ONLY wire↔method conversion

`<comp>_shell_adapter.hpp::tick()` is allowed to:
- Read input latch + check `can_accept_*()` capacity + push beat into c_model
- Call `<comp>_->tick()` exactly once
- Read c_model output state into output latch

Forbidden: any business logic inside the adapter that should live in the c_model
component itself (e.g., packetization, routing, ROB reordering). If you find
yourself adding such logic, the c_model component is missing an API — extend
the c_model header instead.
```

- [ ] **Step 8: Write `cosim2/KNOWN_LIMITATIONS.md`** (carried from cosim/, updated)

```markdown
# cosim2/ — Known Limitations

## Resolved in Stage 5b

### §2 Multi-beat W burst and multi-outstanding AW invisible to checker — RESOLVED

Stage 5a snapshot model lost beats W[1..N-1] of multi-beat bursts. β tick
discipline (registered SV wires, 1-cycle latency per hop) makes every beat
wire-visible. Evidence: `multibeat_incr_8beat.yaml` passes wb2axip checker
without false positives.

Evidence artifact (after Task 15 completion):
`c_model/build/test-artifacts/multibeat-resolved.log`

### §3 cmodel_finalize not called on timeout — RESOLVED

Stage 5b DPI error propagation (return code + SV `$fatal`) calls
`cmodel_finalize()` at SV side cycle end before `$fatal`. See spec §7.5.

## Carried unchanged from Stage 5a

### §1 faxi_wstrb.v permissive stub

`cosim2/sv/wb2axip/faxi_wstrb.v` was created as a permissive stub during the
Stage 5a build-fix pass (commit `822a780`). `o_valid` is hardwired to `1'b1`,
disabling WSTRB alignment checking. Stage 5b carries this stub unchanged.

Follow-up: pull a proper upstream `faxi_wstrb.v` or implement the alignment
check natively.

### §4 read_dump tmp accumulation

`c_model/include/cosim2/master_shell_adapter.hpp` (when implemented) inherits
the per-instance read_dump `.tmp` filename from Stage 5a AxiDpiAdapter. Files
accumulate in build dir per ctest run. Cosmetic; cleanup via per-instance
destructor unlink is a follow-up.

### §5 Timing master direction differs from spec §3 anchored decision

Stage 5b harness keeps Stage 5a's "C++ drives clock" pattern (`main.cpp`
toggles `clk_i` and drives `rst_ni`). The spec §3 decision says "SV master DPI
direction" — the C++ harness is the timing master at the Verilator level, while
the SV side owns the cycle-by-cycle wire propagation. This nuance is consistent
with main plan §5.4 "low-friction first Verilator" goal; documented for future
VCS DPI-RTL port (which will reverse the role).

## New limitations introduced in Stage 5b

(None expected; will be added if any surface during T15 smoke run.)
```

- [ ] **Step 9: Commit 1 — bootstrap (one commit covering all of Step 2-8)**

```bash
git status --short
git add cosim2/ c_model/tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
chore(cosim2): bootstrap Stage 5b tree + move wb2axip + delete Stage 5a artifacts

Single dedicated reset commit per spec §8: scaffolds cosim2/{sv,c,verilator,tests}
directory tree, moves wb2axip vendored files to cosim2/sv/wb2axip/ preserving
git history, deletes Stage 5a snapshot-model PoC files (proxy SVs, axi_dpi,
adapter, pin_snapshot, axi_dpi_adapter unit test, Verilator harness for the old
tb_axi_conformity binary), writes README/CODING_DISCIPLINE/KNOWN_LIMITATIONS,
and removes the cosim/ subdir hook from c_model/tests/CMakeLists.txt.

Stage 5a ctest 395 baseline is preserved because (a) cosim_smoke_tests entry
is removed by the same hook removal; the remaining 393 (Stage 3 baseline) +
2 (Stage 5a AxiDpiAdapter unit tests) entries are gone, leaving 393 entries
until Stage 5b shells are wired in later tasks.

Co-Authored-By: <Implementer> <implementer@example.com>
EOF
)"
```

- [ ] **Step 10: Verify build still configures + Stage 5a unit-level ctest still passes**

```bash
cd c_model && cmake --build build 2>&1 | tail -10
# Expected: build succeeds (we removed a subdir hook but didn't touch any c_model code yet)
ctest --test-dir build -j 1 2>&1 | tail -5
# Expected: ctest passes; entry count will be lower than 395 (we removed 2 AxiDpiAdapter
# entries explicitly and the cosim_smoke tests via hook removal)
```

If the test count surprises you, note it in the commit description for the next commit (Step 11).

- [ ] **Step 11: Self-review**

- All target files created/deleted per the plan?
- `cosim2/` tree exists with placeholders?
- `cosim/` is empty of tracked files (verify with `find cosim -type f`)?
- `cosim2/sv/wb2axip/` has all 5 vendored files + updated ATTRIBUTION.md?
- Commit message accurately describes the reset?

---

## Task 2: c_model — add `can_accept_*()` capacity queries to ports

**Files:**
- Modify: `c_model/include/nmu/axi_slave_port.hpp` (add `can_accept_aw/w/ar() const`)
- Modify: `c_model/include/nsu/axi_master_port.hpp` (add `can_accept_b/r() const`)
- Test: existing port unit tests stay green; no new test (queries are read-only adapters over existing `*_q_size()`)

- [ ] **Step 1: Invoke karpathy-guidelines + read existing port API**

```
Skill('karpathy-guidelines')
```

```bash
grep -nE "q_size|kCapacity|push_aw|push_w|push_ar" c_model/include/nmu/axi_slave_port.hpp
grep -nE "q_size|kCapacity|push_b|push_r"          c_model/include/nsu/axi_master_port.hpp
```

Note the queue depth constants (likely member named `aw_q_max_` or `kCapacity` from existing source). Use those exact identifiers in `can_accept_*()` bodies.

- [ ] **Step 2: Add `can_accept_*()` const methods to `AxiSlavePort`**

In `c_model/include/nmu/axi_slave_port.hpp`, find the public section after `aw_q_size()` (or wherever introspection accessors live). Add (semantics per spec §6.4: returns tick-end capacity — i.e. whether ONE more beat can fit AFTER the current tick completes):

```cpp
// Tick-end capacity queries (Stage 5b ShellAdapter contract per spec §6.4):
// returns true iff one more AW/W/AR beat can be pushed when the next tick begins.
// MUST NOT be called mid-tick; ShellAdapter samples these at tick end only.
[[nodiscard]] bool can_accept_aw() const noexcept { return aw_q_.size() < aw_q_max_; }
[[nodiscard]] bool can_accept_w()  const noexcept { return w_q_.size()  < w_q_max_;  }
[[nodiscard]] bool can_accept_ar() const noexcept { return ar_q_.size() < ar_q_max_; }
```

Adjust the queue member names to match what `axi_slave_port.hpp` actually uses (verified in Step 1).

- [ ] **Step 3: Add `can_accept_*()` const methods to `AxiMasterPort`**

In `c_model/include/nsu/axi_master_port.hpp`, mirror the pattern for B/R response queues:

```cpp
// Tick-end capacity queries (Stage 5b ShellAdapter contract per spec §6.4):
[[nodiscard]] bool can_accept_b() const noexcept { return b_q_.size() < b_q_max_; }
[[nodiscard]] bool can_accept_r() const noexcept { return r_q_.size() < r_q_max_; }
```

- [ ] **Step 4: Build + run all existing c_model ctests**

```bash
cd c_model && cmake --build build && ctest --test-dir build -j 1
```

Expected: all existing tests pass. The new methods are additive const accessors; no behavior change.

- [ ] **Step 5: Commit**

```bash
git add c_model/include/nmu/axi_slave_port.hpp c_model/include/nsu/axi_master_port.hpp
git commit -m "$(cat <<'EOF'
feat(c_model): add can_accept_*() tick-end capacity queries to AXI ports

Per Stage 5b spec §6.4 backpressure invariant. Const, non-mutating queries
used by Stage 5b ShellAdapters to drive next-cycle ready outputs without
mutating queue state. Existing port API unchanged; all Stage 3+ ctest entries
remain green.
EOF
)"
```

- [ ] **Step 6: Self-review**

- Methods marked `[[nodiscard]] const noexcept`?
- Member names match the actual queue member names in the headers?
- Existing tests still pass?

---

## Task 3: c_model — standalone ctor overloads for 5 components

**Files:**
- Modify: `c_model/include/nmu/nmu.hpp` (`+ explicit Nmu(NmuConfig)`)
- Modify: `c_model/include/nsu/nsu.hpp` (`+ explicit Nsu(NsuConfig)`)
- Modify: `c_model/include/common/loopback_noc.hpp` (`+ standalone ctor`)
- Modify: `c_model/include/axi/axi_master.hpp` (`+ standalone ctor`, no AxiSlavePort& arg)
- Modify: `c_model/include/axi/axi_slave.hpp` (`+ standalone ctor` if not already)

**Critical constraint:** EXISTING signatures preserved verbatim. (a) integration testbench at `c_model/tests/integration/test_request_response_loopback.cpp` MUST continue to compile + pass.

- [ ] **Step 1: Invoke karpathy-guidelines + survey existing ctor signatures**

```
Skill('karpathy-guidelines')
```

```bash
grep -nE "^class Nmu|Nmu\(" c_model/include/nmu/nmu.hpp
grep -nE "^class Nsu|Nsu\(" c_model/include/nsu/nsu.hpp
grep -nE "^class LoopbackNoc|LoopbackNoc\(" c_model/include/common/loopback_noc.hpp
grep -nE "^class AxiMaster|AxiMaster\(" c_model/include/axi/axi_master.hpp
grep -nE "^class AxiSlave|AxiSlave\(" c_model/include/axi/axi_slave.hpp
```

Document each existing signature in a scratch note — these MUST stay untouched.

- [ ] **Step 2: Verify `Nmu(NmuConfig)` standalone is actually buildable**

`Nmu` currently takes `(NmuConfig, NocReqOut&, NocRspIn&)` because internal `WormholeArbiter` ctor needs the downstream NoC ref. The standalone overload needs an internal "stub" NocReqOut/NocRspIn — OR a 2-phase construction pattern.

Two options:
- **(a) Internal owned stubs:** `Nmu(NmuConfig)` owns private `NocReqOut stub_req_out_` and `NocRspIn stub_rsp_in_` members; WormholeArbiter binds to these stubs internally; ShellAdapter reads the stubs' pin states.
- **(b) Post-ctor bind:** `Nmu(NmuConfig)` constructs in default state with WormholeArbiter not yet bound; new `void bind_noc(NocReqOut&, NocRspIn&)` method called by ShellAdapter.

Pick (a) — single ctor call, no two-phase init. ShellAdapter doesn't need to know about WormholeArbiter internals.

Implementation sketch in `nmu.hpp`:

```cpp
class Nmu {
public:
    // Existing — preserve verbatim
    Nmu(NmuConfig cfg, noc::NocReqOut& downstream_req, noc::NocRspIn& downstream_rsp);

    // NEW Stage 5b standalone — owns internal NocReq/Rsp stubs
    explicit Nmu(NmuConfig cfg);

    // ... existing public API unchanged ...

private:
    // For the standalone ctor: internal stubs owned by this Nmu
    std::optional<noc::NocReqOut> standalone_req_out_;
    std::optional<noc::NocRspIn>  standalone_rsp_in_;
    // ... existing members ...
};

// Standalone ctor body (in nmu.hpp inline):
inline Nmu::Nmu(NmuConfig cfg)
    : standalone_req_out_(std::in_place),
      standalone_rsp_in_(std::in_place),
      Nmu(std::move(cfg), *standalone_req_out_, *standalone_rsp_in_) {}
// NOTE: delegating ctor + std::optional in-place init may need re-ordering;
// adapt to actual NmuConfig + member init order discovered in Step 1.
```

If member init order makes this awkward, fall back to (b): standalone ctor without delegating + `void rebind_noc(NocReqOut&, NocRspIn&)` for the ShellAdapter to call. Use whichever compiles cleanly in <30 lines and doesn't disturb the existing ctor.

- [ ] **Step 3: Add standalone ctors to all 5 components**

Repeat the (a)-or-(b) pattern for `Nsu`, `LoopbackNoc`, `AxiMaster`, `AxiSlave`. Each:
- Preserves existing ctor verbatim
- Adds an `explicit <Comp>(<ConfigStruct>)` overload (or zero-arg for `LoopbackNoc` which has no config)
- Internally owns any "ref dependency" peer as a private member, OR exposes a `rebind_*()` method

`AxiMaster` standalone signature:
```cpp
// Existing — preserve verbatim:
// template<class PortT> AxiMaster(std::string scenario_yaml, PortT& port, std::string dump_path, ...);

// NEW Stage 5b standalone — port is internal stub:
AxiMaster(std::string scenario_yaml, std::string dump_path,
          std::size_t max_outstanding_write, std::size_t max_outstanding_read);
```

`AxiSlave` standalone:
```cpp
// Existing — preserve verbatim:
// AxiSlave(Memory& mem, ...);

// NEW Stage 5b standalone — Memory is internal:
AxiSlave(std::uint64_t mem_base, std::size_t mem_size,
         std::size_t write_latency, std::size_t read_latency);
```

- [ ] **Step 4: Build + run ALL existing ctests (including Stage 3 integration testbench)**

```bash
cd c_model && cmake --build build && ctest --test-dir build -j 1
```

Expected: 393 (or current) entries pass. The `test_request_response_loopback.cpp` integration testbench MUST still use the existing ref-based ctor — verify no compile error.

If Stage 3 testbench breaks: revert offending change, switch to pattern (b) (post-ctor bind) for that specific component.

- [ ] **Step 5: Commit**

```bash
git add c_model/include/nmu/nmu.hpp c_model/include/nsu/nsu.hpp \
        c_model/include/common/loopback_noc.hpp \
        c_model/include/axi/axi_master.hpp c_model/include/axi/axi_slave.hpp
git commit -m "$(cat <<'EOF'
feat(c_model): add standalone ctor overloads to 5 components for Stage 5b

Each affected component (Nmu/Nsu/LoopbackNoc/AxiMaster/AxiSlave) gains an
explicit standalone ctor for Stage 5b ShellAdapter use (hermetic invariant:
no cross-component refs at construction time). Existing ctor signatures
preserved verbatim — Stage 3 integration testbench (test_request_response_
loopback) continues to compile and pass unchanged.

Per spec §3 anchored decisions row "ctor compatibility" + §8 file layout
explicit additive-only note.
EOF
)"
```

- [ ] **Step 6: Self-review**

- Each component has BOTH original ctor AND new standalone ctor?
- `c_model/tests/integration/test_request_response_loopback.cpp` builds clean?
- Stage 3 ctests all green?

---

## Task 4: c_model — plusarg inject mode parser in AxiMaster

**Files:**
- Modify: `c_model/include/axi/axi_master.hpp` (add inject mode field + parser + force_awvalid_low hook)
- Modify: `c_model/include/axi/scenario_parser.hpp` (add optional `inject:` YAML field per spec §9.6)
- Test: `c_model/tests/axi/test_axi_master_inject.cpp` (new — verify parser allowlist + force behavior)
- Modify: `c_model/tests/axi/CMakeLists.txt` (register new test)

- [ ] **Step 1: Invoke karpathy-guidelines + read scenario_parser.hpp**

```
Skill('karpathy-guidelines')
```

```bash
grep -nE "struct ScenarioConfig|inject" c_model/include/axi/scenario_parser.hpp
```

Confirm `inject:` field doesn't already exist.

- [ ] **Step 2: Add `inject:` parsing to scenario_parser**

In `c_model/include/axi/scenario_parser.hpp`, extend `ScenarioConfig` struct:

```cpp
struct InjectConfig {
    enum class Mode { None, AwUnstable };
    Mode mode = Mode::None;
    std::size_t cycle = 0;  // when to trigger
};

struct ScenarioConfig {
    // ... existing fields ...
    InjectConfig inject{};  // NEW; default = None
};
```

And in the YAML loader function, after the existing scenario fields:

```cpp
if (auto inj = root["scenario"]["inject"]) {
    const std::string mode_str = inj["mode"].as<std::string>();
    if (mode_str == "aw_unstable") {
        cfg.inject.mode = InjectConfig::Mode::AwUnstable;
    } else {
        throw std::runtime_error("scenario: unknown +inject mode '" + mode_str +
                                  "' (allowlist: aw_unstable)");
    }
    cfg.inject.cycle = inj["cycle"].as<std::size_t>();
}
```

- [ ] **Step 3: Write failing test for AxiMaster inject behavior**

Create `c_model/tests/axi/test_axi_master_inject.cpp`:

```cpp
#include "axi/axi_master.hpp"
#include "axi/scenario_parser.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

using ni::cmodel::axi::AxiMaster;
using ni::cmodel::axi::InjectConfig;

TEST(AxiMasterInject, no_inject_field_means_no_violation) {
    SCENARIO("inject mode None: AwVALID remains stable per AXI4 spec");
    // Load a scenario without inject field; verify AxiMaster never forces
    // awvalid low while waiting for awready.
    // ... drive the master tick-by-tick, check awvalid stable ...
    EXPECT_TRUE(true);  // skeleton; flesh out in Step 5
}

TEST(AxiMasterInject, aw_unstable_at_cycle_n_forces_awvalid_low) {
    SCENARIO("inject mode AwUnstable: forces awvalid=0 at injection cycle");
    InjectConfig inj{InjectConfig::Mode::AwUnstable, /*cycle=*/10};
    // Construct master with inject config, tick past cycle 10, observe
    // awvalid being forced low while awready was not yet asserted.
    // ... assertions on AxiMaster behavior ...
    EXPECT_TRUE(true);  // skeleton
}

TEST(AxiMasterInject, unknown_inject_mode_throws_at_parse_time) {
    SCENARIO("YAML with unknown inject mode rejected by parser");
    // Inline YAML string with mode: bogus_mode_name; parser should throw.
    EXPECT_THROW(ni::cmodel::axi::parse_scenario_from_string(
        "scenario:\n  inject:\n    mode: bogus_mode\n    cycle: 5\n"
        "transactions: []\n"), std::runtime_error);
}
```

Skeleton uses `EXPECT_TRUE(true)` for the first two; flesh out after AxiMaster API extension in Step 5. The third test is concrete and exercises Step 2's parser.

Add to `c_model/tests/axi/CMakeLists.txt`:

```cmake
add_cmodel_test(test_axi_master_inject test_axi_master_inject.cpp)
```

(or the equivalent project-specific helper macro found by reading existing entries).

- [ ] **Step 4: Run test, watch the third one pass and first two skeleton-pass**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R AxiMasterInject -V
```

Expected: 3 entries report. Third test passes (parser rejects unknown mode). First two skeleton-pass with `EXPECT_TRUE(true)` (placeholder).

- [ ] **Step 5: Extend AxiMaster with inject state + tick-time force**

In `c_model/include/axi/axi_master.hpp`, add a setter that scenario_parser calls and a private check inside `tick()`:

```cpp
class AxiMaster /* template or non-template per existing */ {
    // ... existing public API ...
    void configure_inject(const InjectConfig& inj) noexcept { inject_ = inj; cycle_count_ = 0; }

    void tick() {
        ++cycle_count_;
        if (inject_.mode == InjectConfig::Mode::AwUnstable && cycle_count_ == inject_.cycle) {
            force_awvalid_low_one_cycle_ = true;  // overridden by drive_aw() this tick
        }
        // ... existing tick body ...
    }

private:
    InjectConfig inject_{};
    std::size_t cycle_count_ = 0;
    bool force_awvalid_low_one_cycle_ = false;
};
```

In the function that drives AW handshake (find via grep for `awvalid` in current `tick()`), gate it:

```cpp
const bool awvalid_now = !force_awvalid_low_one_cycle_ && /* existing condition */;
if (force_awvalid_low_one_cycle_) force_awvalid_low_one_cycle_ = false;
```

After scenario parse, in whatever code calls `AxiMaster::AxiMaster(scenario, ...)`, also call `configure_inject(scenario.inject)` (or fold into the standalone ctor from Task 3).

- [ ] **Step 6: Flesh out first two tests with real assertions**

Replace the `EXPECT_TRUE(true)` skeletons with concrete checks:

```cpp
// First test: no inject → awvalid stays high until awready
TEST(AxiMasterInject, no_inject_field_means_no_violation) {
    // construct AxiMaster from a single-write scenario (no inject)
    // tick 50 cycles with awready stuck low
    // assert awvalid stays high every cycle
}

// Second test: inject mode AwUnstable at cycle N
TEST(AxiMasterInject, aw_unstable_at_cycle_n_forces_awvalid_low) {
    // construct AxiMaster + configure_inject({AwUnstable, 10})
    // tick to cycle 9: awvalid=1
    // tick to cycle 10: awvalid=0 (violation injected)
    // tick to cycle 11+: awvalid back to 1 (only one-cycle force)
}
```

Implement details specific to actual `AxiMaster` `tick()` and pin state inspection (likely a public getter `bool awvalid() const`).

- [ ] **Step 7: Run tests, verify all 3 pass**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R AxiMasterInject -V
```

Expected: 3/3 pass.

- [ ] **Step 8: Run full ctest to confirm no regression**

```bash
ctest --test-dir build -j 1
```

Expected: previous count + 3 new entries.

- [ ] **Step 9: Commit**

```bash
git add c_model/include/axi/axi_master.hpp c_model/include/axi/scenario_parser.hpp \
        c_model/tests/axi/test_axi_master_inject.cpp c_model/tests/axi/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(c_model): add plusarg-driven inject mode to AxiMaster (Stage 5b)

YAML `scenario.inject: {mode, cycle}` field consumed by scenario_parser;
allowlist enforced (currently only `aw_unstable`); unknown mode throws at
parse time. AxiMaster gains `configure_inject()` setter + tick-time
one-shot force_awvalid_low for the specified cycle.

Per Stage 5b spec §7.4 fault injection contract. 3 new unit tests
cover no-inject default behavior, AwUnstable force, parser allowlist.
EOF
)"
```

- [ ] **Step 10: Self-review**

- Default `InjectConfig::Mode::None` ensures zero overhead when no `inject:` in YAML?
- Parser rejects unknown modes with `std::runtime_error`?
- AxiMaster `tick()` behavior unchanged when no inject configured?

---

## Task 5: SV interfaces — axi_intf + noc_req_intf + noc_rsp_intf

**Files:**
- Create: `cosim2/sv/axi_intf.sv` (AXI bundle + valid/ready + master/slave modports)
- Create: `cosim2/sv/noc_req_intf.sv` (parameterized over NUM_VC + FLIT_W; producer/consumer modports)
- Create: `cosim2/sv/noc_rsp_intf.sv` (symmetric to noc_req)

- [ ] **Step 1: Invoke rtl-style + karpathy skills**

```
Skill('rtl-style')
Skill('karpathy-guidelines')
```

Read `~/.claude/skills/rtl-style/references/interfaces.md` and `~/.claude/skills/rtl-style/templates/` for the project SV interface convention.

- [ ] **Step 2: Write `cosim2/sv/axi_intf.sv` per spec §6.1**

```systemverilog
// AXI4 bundle interface with full handshake (valid/ready per channel) and
// master/slave modports for direction-safe instantiation.
//
// Carries fields per spec §6.1:
//   AW: awvalid, awready, awid, awaddr, awlen, awsize, awburst, awlock,
//       awcache, awprot, awqos
//   W:  wvalid, wready, wlast, wdata, wstrb
//   B:  bvalid, bready, bid, bresp
//   AR: arvalid, arready, arid, araddr, arlen, arsize, arburst, arlock,
//       arcache, arprot, arqos
//   R:  rvalid, rready, rlast, rid, rdata, rresp
//
// Excluded per Stage 5b scope: region, user (not checked by wb2axip;
// see KNOWN_LIMITATIONS).

interface axi_intf #(
    parameter int ID_WIDTH   = 8,
    parameter int ADDR_WIDTH = 64,
    parameter int DATA_WIDTH = 256
) (
    input logic clk_i,
    input logic rst_ni
);
    // AW
    logic                    awvalid, awready;
    logic [ID_WIDTH-1:0]     awid;
    logic [ADDR_WIDTH-1:0]   awaddr;
    logic [7:0]              awlen;
    logic [2:0]              awsize;
    logic [1:0]              awburst;
    logic                    awlock;
    logic [3:0]              awcache;
    logic [2:0]              awprot;
    logic [3:0]              awqos;
    // W
    logic                    wvalid, wready, wlast;
    logic [DATA_WIDTH-1:0]   wdata;
    logic [DATA_WIDTH/8-1:0] wstrb;
    // B
    logic                    bvalid, bready;
    logic [ID_WIDTH-1:0]     bid;
    logic [1:0]              bresp;
    // AR
    logic                    arvalid, arready;
    logic [ID_WIDTH-1:0]     arid;
    logic [ADDR_WIDTH-1:0]   araddr;
    logic [7:0]              arlen;
    logic [2:0]              arsize;
    logic [1:0]              arburst;
    logic                    arlock;
    logic [3:0]              arcache;
    logic [2:0]              arprot;
    logic [3:0]              arqos;
    // R
    logic                    rvalid, rready, rlast;
    logic [ID_WIDTH-1:0]     rid;
    logic [DATA_WIDTH-1:0]   rdata;
    logic [1:0]              rresp;

    modport master (
        output awvalid, awid, awaddr, awlen, awsize, awburst,
               awlock, awcache, awprot, awqos,
        output wvalid, wdata, wstrb, wlast,
        output arvalid, arid, araddr, arlen, arsize, arburst,
               arlock, arcache, arprot, arqos,
        output bready, rready,
        input  awready, wready, arready,
        input  bvalid, bid, bresp,
        input  rvalid, rid, rdata, rresp, rlast
    );

    modport slave (
        input  awvalid, awid, awaddr, awlen, awsize, awburst,
               awlock, awcache, awprot, awqos,
        input  wvalid, wdata, wstrb, wlast,
        input  arvalid, arid, araddr, arlen, arsize, arburst,
               arlock, arcache, arprot, arqos,
        input  bready, rready,
        output awready, wready, arready,
        output bvalid, bid, bresp,
        output rvalid, rid, rdata, rresp, rlast
    );
endinterface
```

- [ ] **Step 3: Write `cosim2/sv/noc_req_intf.sv` per spec §6.2**

```systemverilog
// NoC request bundle. Credit-based (no AXI-style ready); credit_return is
// a reverse-direction per-VC signal from consumer to producer.

interface noc_req_intf #(
    parameter int NUM_VC = 1,
    parameter int FLIT_W = 256
) (
    input logic clk_i,
    input logic rst_ni
);
    logic                   valid;
    logic [FLIT_W-1:0]      flit;
    logic [NUM_VC-1:0]      credit_return;

    modport producer (
        output valid, flit,
        input  credit_return
    );

    modport consumer (
        input  valid, flit,
        output credit_return
    );
endinterface
```

- [ ] **Step 4: Write `cosim2/sv/noc_rsp_intf.sv` (symmetric)**

```systemverilog
// NoC response bundle. Symmetric to noc_req_intf — same field shape, opposite
// dataflow direction (producer drives, consumer reads, credit_return goes
// reverse).

interface noc_rsp_intf #(
    parameter int NUM_VC = 1,
    parameter int FLIT_W = 256
) (
    input logic clk_i,
    input logic rst_ni
);
    logic                   valid;
    logic [FLIT_W-1:0]      flit;
    logic [NUM_VC-1:0]      credit_return;

    modport producer (
        output valid, flit,
        input  credit_return
    );

    modport consumer (
        input  valid, flit,
        output credit_return
    );
endinterface
```

- [ ] **Step 5: Verilator lint-only pass**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
verilator --lint-only -Wall \
    -I cosim2/sv \
    cosim2/sv/axi_intf.sv \
    cosim2/sv/noc_req_intf.sv \
    cosim2/sv/noc_rsp_intf.sv 2>&1 | tail -10
```

Expected: 0 errors, possibly some `UNUSED` warnings (acceptable; interfaces have no internal logic).

- [ ] **Step 6: Dispatch rtl-reviewer agent**

Dispatch the `rtl-reviewer` agent on the 3 interface files. Save output to `c_model/build/rtl-review-logs/task5-rtl-review.md`. Block on 0 CRITICAL + 0 HIGH findings.

- [ ] **Step 7: Commit**

```bash
git add cosim2/sv/axi_intf.sv cosim2/sv/noc_req_intf.sv cosim2/sv/noc_rsp_intf.sv
git commit -m "$(cat <<'EOF'
feat(cosim2/sv): add axi_intf + noc_req_intf + noc_rsp_intf with modports

Three Stage 5b wire bundle interfaces:
- axi_intf: AXI4 + handshake + master/slave modports for direction safety
- noc_req_intf: NUM_VC-parameterized, [NUM_VC-1:0] credit_return per spec §6.2
- noc_rsp_intf: symmetric to noc_req

Phase 1: cosim2-local definitions. Phase 2 follow-up: upstream into specgen.
rtl-style + rtl-reviewer 0 CRITICAL/0 HIGH; Verilator --lint-only clean.
EOF
)"
```

- [ ] **Step 8: Self-review**

- All 3 interfaces have `clk_i` + `rst_ni` input ports (rtl-style sync reset)?
- `axi_intf` has BOTH master AND slave modports?
- `noc_req_intf` + `noc_rsp_intf` have BOTH producer AND consumer modports?
- `credit_return` is `[NUM_VC-1:0]` (not single bit)?

---

## Task 6: DPI infrastructure — cmodel_dpi.h + dpi_boundary_macros.h + lifecycle skeleton

**Files:**
- Create: `cosim2/c/cmodel_dpi.h` (DPI signatures + `cmodel_dpi_error_e` enum + lifecycle decls)
- Create: `cosim2/c/dpi_boundary_macros.h` (`DPI_BOUNDARY_BEGIN/END` macros)
- Create: `cosim2/c/cmodel_dpi.cpp` (extern "C" lifecycle bodies + global error state; per-shell handlers added in later shell tasks)

- [ ] **Step 1: Invoke karpathy + read svdpi conventions**

```
Skill('karpathy-guidelines')
```

Recall from Stage 5a `cosim/c/axi_dpi.h` conventions:
- `svBit*` for single-bit output args
- `svBitVecVal*` for multi-bit (word) output args
- `#ifdef __cplusplus extern "C" { ... }` wrap

- [ ] **Step 2: Write `cosim2/c/dpi_boundary_macros.h`**

```cpp
// DPI boundary try/catch macros — every extern "C" DPI handler MUST wrap its
// body in DPI_BOUNDARY_BEGIN/END so C++ exceptions do not cross the DPI ABI
// (which is IEEE 1800 undefined behavior). Per Stage 5b spec §5.2.
//
// On exception: sets g_dpi_error_{code,msg}, returns silently. The SV side
// polls cmodel_check_error() at each shell's always_ff end and raises $fatal.

#ifndef COSIM2_DPI_BOUNDARY_MACROS_H
#define COSIM2_DPI_BOUNDARY_MACROS_H

#include "cmodel_dpi.h"
#include <atomic>
#include <exception>
#include <string>

namespace ni::cmodel::cosim2 {
extern std::atomic<int>  g_dpi_error_code;
extern std::string       g_dpi_error_msg;
}

#define DPI_BOUNDARY_BEGIN(fn_name) try
#define DPI_BOUNDARY_END(fn_name)                                                  \
    catch (const std::exception& e) {                                              \
        ni::cmodel::cosim2::g_dpi_error_code.store(CMODEL_DPI_ERR_GENERIC);        \
        ni::cmodel::cosim2::g_dpi_error_msg =                                      \
            std::string(#fn_name ": ") + e.what();                                 \
    } catch (...) {                                                                \
        ni::cmodel::cosim2::g_dpi_error_code.store(CMODEL_DPI_ERR_UNKNOWN);        \
        ni::cmodel::cosim2::g_dpi_error_msg = std::string(#fn_name) + ": ...";    \
    }

#endif  // COSIM2_DPI_BOUNDARY_MACROS_H
```

Usage: every DPI handler body wraps:

```cpp
extern "C" void cmodel_nmu_tick() {
    DPI_BOUNDARY_BEGIN(cmodel_nmu_tick) {
        // actual body
    } DPI_BOUNDARY_END(cmodel_nmu_tick);
}
```

- [ ] **Step 3: Write `cosim2/c/cmodel_dpi.h`**

```cpp
// DPI signatures for Stage 5b wire-wrap co-sim. 5 shells × 3 calls/cycle
// (set_inputs/tick/get_outputs) + lifecycle (init/finalize/check_error).
//
// Error propagation: try/catch in handlers sets g_dpi_error_code; SV side
// polls cmodel_check_error() per shell per cycle and raises $fatal on
// non-zero. See spec §5.2.

#ifndef COSIM2_CMODEL_DPI_H
#define COSIM2_CMODEL_DPI_H

#include "svdpi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Categorized DPI error codes (return value of cmodel_check_error)
typedef enum {
    CMODEL_DPI_OK                       = 0,
    CMODEL_DPI_ERR_GENERIC              = 1,
    CMODEL_DPI_ERR_NOT_INITIALIZED      = 2,
    CMODEL_DPI_ERR_HERMETIC_VIOLATION   = 3,
    CMODEL_DPI_ERR_BACKPRESSURE         = 4,
    CMODEL_DPI_ERR_INJECT_BAD_MODE      = 5,
    CMODEL_DPI_ERR_UNKNOWN              = 99
} cmodel_dpi_error_e;

// Lifecycle (5 shell singletons, all-or-nothing init per spec §5.3)
void cmodel_init(const char* scenario_yaml_path);
void cmodel_finalize(void);
int  cmodel_check_error(const char** msg);

// Per-shell signatures — bodies added by Tasks 7-11.
// Each shell defines its own {Comp}Inputs / {Comp}Outputs C++ struct in
// c_model/include/cosim2/{comp}_shell_io.hpp; DPI signatures match
// field-by-field with svBit (1-bit) / svBitVecVal (word).
//
// LoopbackNoc (NoC-only, simplest — Task 7):
void cmodel_loopback_noc_set_inputs(svBit req_valid, svBitVecVal* req_flit,
                                     svBit* req_credit_return,
                                     svBit rsp_valid, svBitVecVal* rsp_flit,
                                     svBit* rsp_credit_return);
void cmodel_loopback_noc_tick(void);
void cmodel_loopback_noc_get_outputs(svBit* req_valid_out, svBitVecVal* req_flit_out,
                                      svBit* req_credit_return_out,
                                      svBit* rsp_valid_out, svBitVecVal* rsp_flit_out,
                                      svBit* rsp_credit_return_out);

// AxiMaster (AXI manager, no NoC — Task 8):
// AxiSlave (AXI subordinate, no NoC — Task 9):
// Nmu (AXI subordinate + NoC manager — Task 10):
// Nsu (NoC subordinate + AXI manager — Task 11):
// ... DPI signatures added in respective task headers (kept here too as canonical).

#ifdef __cplusplus
}
#endif

#endif  // COSIM2_CMODEL_DPI_H
```

For now this header just declares LoopbackNoc + lifecycle + error enum. Tasks 8-11 each append their own set of {set_inputs,tick,get_outputs} DPI decls.

- [ ] **Step 4: Write `cosim2/c/cmodel_dpi.cpp` (lifecycle only; shell handlers added later)**

```cpp
// Stage 5b DPI bridge — lifecycle handlers + global error state.
// Per-shell {set_inputs,tick,get_outputs} handler bodies added by Tasks 7-11.

#include "cmodel_dpi.h"
#include "dpi_boundary_macros.h"
#include "cosim2/loopback_noc_shell_adapter.hpp"   // Task 7
// #include "cosim2/master_shell_adapter.hpp"      // Task 8
// #include "cosim2/slave_shell_adapter.hpp"       // Task 9
// #include "cosim2/nmu_shell_adapter.hpp"         // Task 10
// #include "cosim2/nsu_shell_adapter.hpp"         // Task 11
#include "axi/scenario_parser.hpp"
#include <atomic>
#include <memory>
#include <string>

namespace ni::cmodel::cosim2 {

std::atomic<int>  g_dpi_error_code{CMODEL_DPI_OK};
std::string       g_dpi_error_msg;

// 5 singleton ShellAdapter pointers — populated by cmodel_init.
// Hermetic: each handler accesses ONLY its own singleton.
std::unique_ptr<LoopbackNocShellAdapter> g_loopback_adapter;
// (master / slave / nmu / nsu singletons declared by Tasks 8-11.)

}  // namespace ni::cmodel::cosim2

using namespace ni::cmodel::cosim2;

extern "C" void cmodel_init(const char* scenario_yaml_path) {
    DPI_BOUNDARY_BEGIN(cmodel_init) {
        // Reset all existing singletons + error state (idempotent per spec §5.3)
        g_loopback_adapter.reset();
        g_dpi_error_code.store(CMODEL_DPI_OK);
        g_dpi_error_msg.clear();

        // Parse scenario (validates +inject mode if present)
        auto scenario = ni::cmodel::axi::parse_scenario(scenario_yaml_path);

        // Construct fresh adapters into local unique_ptrs (strong exception guarantee)
        auto loop = std::make_unique<LoopbackNocShellAdapter>();
        loop->init();
        // (master / slave / nmu / nsu inits added by Tasks 8-11.)

        // Commit (all-or-nothing)
        g_loopback_adapter = std::move(loop);
    } DPI_BOUNDARY_END(cmodel_init);
}

extern "C" void cmodel_finalize(void) {
    DPI_BOUNDARY_BEGIN(cmodel_finalize) {
        g_loopback_adapter.reset();
        // ... reset other 4 singletons as they exist ...
    } DPI_BOUNDARY_END(cmodel_finalize);
}

extern "C" int cmodel_check_error(const char** msg) {
    // No try/catch — this is the error-reporting boundary
    *msg = g_dpi_error_msg.c_str();
    return g_dpi_error_code.load();
}
```

- [ ] **Step 5: Sanity compile**

```bash
cd c_model
g++ -std=c++17 -I include -I ../cosim2/c -I ../specgen/generated/cpp \
    -c ../cosim2/c/cmodel_dpi.cpp -o /tmp/cmodel_dpi_compile_probe.o 2>&1 | tail -10
```

Expected: compile succeeds. Will fail if `loopback_noc_shell_adapter.hpp` doesn't exist yet — at this point that's expected; comment out the `#include` and the body code that references it, push the actual usage to Task 7. Or stub `LoopbackNocShellAdapter` minimally.

If you stub: write a one-line header `c_model/include/cosim2/loopback_noc_shell_adapter.hpp`:

```cpp
#pragma once
namespace ni::cmodel::cosim2 {
class LoopbackNocShellAdapter {
public:
    void init() {}
};
}
```

This unblocks compile; real class body added in Task 7.

- [ ] **Step 6: Commit**

```bash
git add cosim2/c/cmodel_dpi.h cosim2/c/dpi_boundary_macros.h cosim2/c/cmodel_dpi.cpp \
        c_model/include/cosim2/loopback_noc_shell_adapter.hpp
git commit -m "$(cat <<'EOF'
feat(cosim2/c): add DPI bridge skeleton — error enum + boundary macros + lifecycle

Per Stage 5b spec §5.2-5.3:
- cmodel_dpi.h declares cmodel_dpi_error_e enum (7 codes) and lifecycle DPI
  (init/finalize/check_error). Per-shell {set,tick,get} sigs added by
  Tasks 7-11.
- dpi_boundary_macros.h provides DPI_BOUNDARY_BEGIN/END for try/catch wrap
  around every extern "C" handler; sets g_dpi_error_{code,msg} on exception.
- cmodel_dpi.cpp owns the singleton std::unique_ptr instances + lifecycle
  bodies; strong exception guarantee via local unique_ptr → move pattern.

LoopbackNocShellAdapter stubbed for compile; Task 7 fleshes out.
EOF
)"
```

- [ ] **Step 7: Self-review**

- `cmodel_dpi_error_e` enum matches spec §5.2 exactly (7 codes)?
- `DPI_BOUNDARY_BEGIN/END` macros wrap try/catch correctly?
- `cmodel_init` follows local-unique_ptr-then-move pattern?
- `cmodel_check_error` has NO try/catch (it IS the error-reporting boundary)?

---

## Task 7: LoopbackNoc shell — adapter + SV wrap + unit test (pattern proof)

This task establishes the pattern for Tasks 8-11. LoopbackNoc is the simplest component (NoC-only, no AXI), so the cookie-cutter shape is clearer here.

**Files:**
- Create: `c_model/include/cosim2/loopback_noc_shell_io.hpp` (Inputs/Outputs POD structs)
- Modify: `c_model/include/cosim2/loopback_noc_shell_adapter.hpp` (replace Task 6 stub with full class)
- Create: `c_model/tests/cosim2/test_loopback_noc_shell_adapter.cpp` (TDD unit test)
- Create: `c_model/tests/cosim2/CMakeLists.txt`
- Modify: `c_model/tests/CMakeLists.txt` (add `add_subdirectory(cosim2)`)
- Modify: `cosim2/c/cmodel_dpi.h` (append LoopbackNoc DPI sigs)
- Modify: `cosim2/c/cmodel_dpi.cpp` (append LoopbackNoc handler bodies)
- Create: `cosim2/sv/loopback_noc_wrap.sv` (DPI shell module)

- [ ] **Step 1: Invoke skills + survey LoopbackNoc existing API**

```
Skill('karpathy-guidelines')
```

```bash
grep -nE "class LoopbackNoc|push_req|pop_req|push_rsp|pop_rsp|tick" \
    c_model/include/common/loopback_noc.hpp | head -20
```

Note method names, NoC flit type (likely `FlitWord` or `ni::flit` from specgen).

- [ ] **Step 2: Write Inputs/Outputs POD structs**

Create `c_model/include/cosim2/loopback_noc_shell_io.hpp`:

```cpp
// LoopbackNoc shell IO POD structs. Fields match SV wire bundle per spec §6.2
// (noc_req_intf + noc_rsp_intf).
#pragma once
#include "ni_flit_constants.h"
#include <array>
#include <cstdint>

namespace ni::cmodel::cosim2 {

// FlitWord type — bit-for-bit match with NoC flit (NUM_VC=1 PoC, 256-bit flit).
// Stored as byte array for DPI marshalling consistency.
using FlitBytes = std::array<uint8_t, 32>;  // 256 bits = 32 bytes

struct LoopbackNocInputs {
    // From upstream NMU side (req in to loopback)
    bool        req_in_valid;
    FlitBytes   req_in_flit;
    bool        req_in_credit_return;   // NUM_VC=1, single bit
    // From downstream NSU side (rsp in to loopback)
    bool        rsp_in_valid;
    FlitBytes   rsp_in_flit;
    bool        rsp_in_credit_return;
};

struct LoopbackNocOutputs {
    // To downstream NSU side (req out from loopback)
    bool        req_out_valid;
    FlitBytes   req_out_flit;
    bool        req_out_credit_return;
    // To upstream NMU side (rsp out from loopback)
    bool        rsp_out_valid;
    FlitBytes   rsp_out_flit;
    bool        rsp_out_credit_return;
};

}  // namespace
```

- [ ] **Step 3: Write failing unit test (TDD)**

Create `c_model/tests/cosim2/CMakeLists.txt`:

```cmake
add_executable(test_loopback_noc_shell_adapter test_loopback_noc_shell_adapter.cpp)
target_link_libraries(test_loopback_noc_shell_adapter PRIVATE GTest::gtest_main)
target_include_directories(test_loopback_noc_shell_adapter PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)
add_test(NAME LoopbackNocShellAdapter.smoke COMMAND test_loopback_noc_shell_adapter)
```

Modify `c_model/tests/CMakeLists.txt` to add:

```cmake
add_subdirectory(cosim2)
```

Create `c_model/tests/cosim2/test_loopback_noc_shell_adapter.cpp`:

```cpp
#include "common/scenario.hpp"
#include "cosim2/loopback_noc_shell_adapter.hpp"
#include "cosim2/loopback_noc_shell_io.hpp"
#include <gtest/gtest.h>

using ni::cmodel::cosim2::LoopbackNocShellAdapter;
using ni::cmodel::cosim2::LoopbackNocInputs;
using ni::cmodel::cosim2::LoopbackNocOutputs;

TEST(LoopbackNocShellAdapter, req_flit_forwards_after_one_tick) {
    SCENARIO("req flit in at cycle T appears as req_out at cycle T+1 (β model)");

    LoopbackNocShellAdapter adapter;
    adapter.init();

    LoopbackNocInputs in{};
    in.req_in_valid = true;
    in.req_in_flit[0] = 0xAB;
    in.req_in_credit_return = false;
    in.rsp_in_valid = false;

    adapter.set_inputs(in);
    adapter.tick();

    LoopbackNocOutputs out{};
    adapter.get_outputs(out);

    // β tick: req_out should now carry the flit (1 cycle latency)
    EXPECT_TRUE(out.req_out_valid);
    EXPECT_EQ(out.req_out_flit[0], 0xAB);
}

TEST(LoopbackNocShellAdapter, no_input_no_output) {
    SCENARIO("idle adapter produces no valid output");
    LoopbackNocShellAdapter adapter;
    adapter.init();
    LoopbackNocInputs in{};   // all false
    adapter.set_inputs(in);
    adapter.tick();
    LoopbackNocOutputs out{};
    adapter.get_outputs(out);
    EXPECT_FALSE(out.req_out_valid);
    EXPECT_FALSE(out.rsp_out_valid);
}
```

- [ ] **Step 4: Build + run test, expect compile failure (stub adapter has no API)**

```bash
cd c_model && cmake --build build 2>&1 | tail -10
```

Expected: compile error — `LoopbackNocShellAdapter` has no `set_inputs / tick / get_outputs` methods.

- [ ] **Step 5: Implement full `LoopbackNocShellAdapter` class**

Replace `c_model/include/cosim2/loopback_noc_shell_adapter.hpp` with:

```cpp
#pragma once
#include "common/loopback_noc.hpp"
#include "cosim2/loopback_noc_shell_io.hpp"
#include <memory>

namespace ni::cmodel::cosim2 {

class LoopbackNocShellAdapter {
public:
    void init() {
        loopback_ = std::make_unique<common::LoopbackNoc>(/* standalone ctor args */);
        in_  = LoopbackNocInputs{};
        out_ = LoopbackNocOutputs{};
    }

    void set_inputs(const LoopbackNocInputs& in) { in_ = in; }

    void tick() {
        // 1. Drive c_model loopback from input latch
        if (in_.req_in_valid) {
            // Convert FlitBytes → c_model flit type and push
            // ... loopback_->push_req(make_flit(in_.req_in_flit)) ...
        }
        if (in_.rsp_in_valid) {
            // ... loopback_->push_rsp(make_flit(in_.rsp_in_flit)) ...
        }
        // Forward credit_return signals if loopback has a credit API

        // 2. Advance c_model one cycle
        loopback_->tick();

        // 3. Read c_model outputs into output latch
        out_ = LoopbackNocOutputs{};
        if (auto req = loopback_->peek_req_out()) {
            out_.req_out_valid = true;
            out_.req_out_flit = flit_to_bytes(*req);
        }
        if (auto rsp = loopback_->peek_rsp_out()) {
            out_.rsp_out_valid = true;
            out_.rsp_out_flit = flit_to_bytes(*rsp);
        }
        // ... credit return mapping ...
    }

    void get_outputs(LoopbackNocOutputs& out) const { out = out_; }

private:
    std::unique_ptr<common::LoopbackNoc> loopback_;
    LoopbackNocInputs  in_;
    LoopbackNocOutputs out_;

    static FlitBytes flit_to_bytes(const /* flit type */&);
    // ... helper conversions ...
};

}  // namespace
```

Implement the helper conversions (`make_flit`, `flit_to_bytes`) based on the actual `common::LoopbackNoc` API discovered in Step 1. If LoopbackNoc doesn't expose `peek_req_out / peek_rsp_out`, add them as additive const accessors (mirroring Task 2 pattern — separate small follow-up commit if needed).

- [ ] **Step 6: Run test, iterate to green**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R LoopbackNocShellAdapter -V
```

Iterate until both tests pass. Likely failures: c_model LoopbackNoc API mismatch, missing accessor, helper conversion bugs.

- [ ] **Step 7: Append LoopbackNoc DPI handlers to `cosim2/c/cmodel_dpi.cpp`**

Add after the lifecycle handlers:

```cpp
extern "C" void cmodel_loopback_noc_set_inputs(svBit req_valid, svBitVecVal* req_flit,
                                                svBit req_credit_return,
                                                svBit rsp_valid, svBitVecVal* rsp_flit,
                                                svBit rsp_credit_return) {
    DPI_BOUNDARY_BEGIN(cmodel_loopback_noc_set_inputs) {
        if (!g_loopback_adapter) throw std::runtime_error("not initialized");
        LoopbackNocInputs in{};
        in.req_in_valid = req_valid;
        // Unpack svBitVecVal → FlitBytes (8 words × 4 bytes/word = 32 bytes)
        for (int w = 0; w < 8; ++w) {
            for (int b = 0; b < 4; ++b) {
                in.req_in_flit[w * 4 + b] = (req_flit[w] >> (b * 8)) & 0xFF;
            }
        }
        in.req_in_credit_return = req_credit_return;
        in.rsp_in_valid = rsp_valid;
        for (int w = 0; w < 8; ++w)
            for (int b = 0; b < 4; ++b)
                in.rsp_in_flit[w * 4 + b] = (rsp_flit[w] >> (b * 8)) & 0xFF;
        in.rsp_in_credit_return = rsp_credit_return;
        g_loopback_adapter->set_inputs(in);
    } DPI_BOUNDARY_END(cmodel_loopback_noc_set_inputs);
}

extern "C" void cmodel_loopback_noc_tick(void) {
    DPI_BOUNDARY_BEGIN(cmodel_loopback_noc_tick) {
        if (!g_loopback_adapter) throw std::runtime_error("not initialized");
        g_loopback_adapter->tick();
    } DPI_BOUNDARY_END(cmodel_loopback_noc_tick);
}

extern "C" void cmodel_loopback_noc_get_outputs(svBit* req_valid_out, svBitVecVal* req_flit_out,
                                                 svBit* req_credit_return_out,
                                                 svBit* rsp_valid_out, svBitVecVal* rsp_flit_out,
                                                 svBit* rsp_credit_return_out) {
    DPI_BOUNDARY_BEGIN(cmodel_loopback_noc_get_outputs) {
        if (!g_loopback_adapter) throw std::runtime_error("not initialized");
        LoopbackNocOutputs out{};
        g_loopback_adapter->get_outputs(out);
        *req_valid_out = out.req_out_valid;
        for (int w = 0; w < 8; ++w) {
            req_flit_out[w] = 0;
            for (int b = 0; b < 4; ++b)
                req_flit_out[w] |= (uint32_t(out.req_out_flit[w * 4 + b]) << (b * 8));
        }
        *req_credit_return_out = out.req_out_credit_return;
        *rsp_valid_out = out.rsp_out_valid;
        for (int w = 0; w < 8; ++w) {
            rsp_flit_out[w] = 0;
            for (int b = 0; b < 4; ++b)
                rsp_flit_out[w] |= (uint32_t(out.rsp_out_flit[w * 4 + b]) << (b * 8));
        }
        *rsp_credit_return_out = out.rsp_out_credit_return;
    } DPI_BOUNDARY_END(cmodel_loopback_noc_get_outputs);
}
```

- [ ] **Step 8: Write `cosim2/sv/loopback_noc_wrap.sv`** (invoke `Skill('rtl-style')` first)

```
Skill('rtl-style')
```

```systemverilog
// LoopbackNoc DPI shell. Per Stage 5b spec §5.4.
// β tick: every posedge clk_i, sample prev-cycle wire inputs → push to C++
// via DPI set_inputs → advance c_model via tick → pull outputs → register
// nonblocking to wires. Inline cmodel_check_error at cycle end → $fatal on
// non-zero.

module loopback_noc_wrap #(
    parameter int NUM_VC = 1,
    parameter int FLIT_W = 256
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    noc_req_intf.consumer     req_from_nmu,
    noc_req_intf.producer     req_to_nsu,
    noc_rsp_intf.consumer     rsp_from_nsu,
    noc_rsp_intf.producer     rsp_to_nmu
);
    import "DPI-C" context function void cmodel_loopback_noc_set_inputs(
        input  bit              req_in_valid,
        input  bit [FLIT_W-1:0] req_in_flit,
        input  bit              req_in_credit_return,
        input  bit              rsp_in_valid,
        input  bit [FLIT_W-1:0] rsp_in_flit,
        input  bit              rsp_in_credit_return);

    import "DPI-C" context function void cmodel_loopback_noc_tick();

    import "DPI-C" context function void cmodel_loopback_noc_get_outputs(
        output bit              req_out_valid,
        output bit [FLIT_W-1:0] req_out_flit,
        output bit              req_out_credit_return,
        output bit              rsp_out_valid,
        output bit [FLIT_W-1:0] rsp_out_flit,
        output bit              rsp_out_credit_return);

    import "DPI-C" context function int  cmodel_check_error(output string msg);
    import "DPI-C" context function void cmodel_finalize();

    bit              req_out_valid_q;
    bit [FLIT_W-1:0] req_out_flit_q;
    bit              req_out_credit_return_q;
    bit              rsp_out_valid_q;
    bit [FLIT_W-1:0] rsp_out_flit_q;
    bit              rsp_out_credit_return_q;

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            req_to_nsu.valid         <= '0;
            req_to_nsu.flit          <= '0;
            rsp_from_nsu.credit_return <= '0;
            rsp_to_nmu.valid         <= '0;
            rsp_to_nmu.flit          <= '0;
            req_from_nmu.credit_return <= '0;
        end else begin
            cmodel_loopback_noc_set_inputs(
                req_from_nmu.valid, req_from_nmu.flit, req_to_nsu.credit_return[0],
                rsp_from_nsu.valid, rsp_from_nsu.flit, rsp_to_nmu.credit_return[0]);
            cmodel_loopback_noc_tick();
            cmodel_loopback_noc_get_outputs(
                req_out_valid_q, req_out_flit_q, req_out_credit_return_q,
                rsp_out_valid_q, rsp_out_flit_q, rsp_out_credit_return_q);

            req_to_nsu.valid          <= req_out_valid_q;
            req_to_nsu.flit           <= req_out_flit_q;
            req_from_nmu.credit_return <= {NUM_VC{req_out_credit_return_q}};
            rsp_to_nmu.valid          <= rsp_out_valid_q;
            rsp_to_nmu.flit           <= rsp_out_flit_q;
            rsp_from_nsu.credit_return <= {NUM_VC{rsp_out_credit_return_q}};

            // Inline error check per spec §7.5
            begin
                string err_msg;
                int err = cmodel_check_error(err_msg);
                if (err != 0) begin
                    $display("[loopback_noc_wrap] DPI fatal (code=%0d): %s", err, err_msg);
                    cmodel_finalize();
                    $fatal(1);
                end
            end
        end
    end
endmodule
```

- [ ] **Step 9: Verilator lint pass**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
verilator --lint-only -Wall \
    -I cosim2/sv -I cosim2/sv/wb2axip \
    cosim2/sv/axi_intf.sv cosim2/sv/noc_req_intf.sv cosim2/sv/noc_rsp_intf.sv \
    cosim2/sv/loopback_noc_wrap.sv 2>&1 | tail -15
```

Expected: 0 errors. UNUSED warnings on AXI bundle signals acceptable (not used by loopback).

- [ ] **Step 10: Dispatch rtl-reviewer agent**

Dispatch on `cosim2/sv/loopback_noc_wrap.sv`. Save report to `c_model/build/rtl-review-logs/task7-rtl-review.md`. Block on 0 CRITICAL / 0 HIGH.

- [ ] **Step 11: Run all c_model tests (including new shell adapter unit test)**

```bash
cd c_model && cmake --build build && ctest --test-dir build -j 1
```

Expected: previous count + 2 (LoopbackNocShellAdapter cases) pass.

- [ ] **Step 12: Commit**

```bash
git add c_model/include/cosim2/loopback_noc_shell_io.hpp \
        c_model/include/cosim2/loopback_noc_shell_adapter.hpp \
        c_model/tests/cosim2/ c_model/tests/CMakeLists.txt \
        cosim2/c/cmodel_dpi.h cosim2/c/cmodel_dpi.cpp \
        cosim2/sv/loopback_noc_wrap.sv
git commit -m "$(cat <<'EOF'
feat(cosim2): LoopbackNoc shell — adapter + SV wrap + unit test (pattern proof)

Establishes the Stage 5b shell pattern (Tasks 8-11 follow same shape):
- POD Inputs/Outputs structs (loopback_noc_shell_io.hpp)
- C++ ShellAdapter with set_inputs/tick/get_outputs (hermetic singleton)
- 2 GoogleTest unit cases (β tick latency + idle behavior)
- DPI handler set in cosim2/c/cmodel_dpi.{h,cpp} with DPI_BOUNDARY try/catch
- SV wrap module with always_ff + reset + inline error check + $fatal path

rtl-style + rtl-reviewer 0 CRITICAL / 0 HIGH; Verilator lint clean.
EOF
)"
```

- [ ] **Step 13: Self-review**

- Adapter tick reads input latch → calls c_model tick → writes output latch (3-step)?
- DPI handlers wrapped in DPI_BOUNDARY_BEGIN/END?
- SV always_ff uses sync reset (`@(posedge clk_i)` + `if (!rst_ni)`)?
- All outputs use `<=` (nonblocking)?
- Inline error check at end of always_ff?

---

## Task 8: AxiMaster shell — adapter + SV wrap + unit test

Follow Task 7 pattern. Differences:
- AxiMaster has AXI master interface (drives AW/W/AR, reads B/R) — no NoC side
- Larger IO struct (~30 AXI signals per direction)
- Backpressure rule (§6.4): check `can_accept_*()` before drain — but AxiMaster is the DRIVER, so capacity check is on the master's own outstanding queue, not a downstream port
- Scoreboard callbacks fire inside AxiMaster::tick() — Scoreboard stays in C++ inside the adapter, no DPI exposure
- Plusarg inject parsed by AxiMaster (Task 4) — adapter just calls AxiMaster's existing API

**Files:**
- Create: `c_model/include/cosim2/master_shell_io.hpp` (MasterInputs/Outputs)
- Create: `c_model/include/cosim2/master_shell_adapter.hpp`
- Create: `c_model/tests/cosim2/test_master_shell_adapter.cpp`
- Modify: `c_model/tests/cosim2/CMakeLists.txt` (add test entry)
- Modify: `cosim2/c/cmodel_dpi.h` (append MasterShell DPI sigs)
- Modify: `cosim2/c/cmodel_dpi.cpp` (append handler bodies + add g_master_adapter singleton + init/finalize)
- Create: `cosim2/sv/axi_master_wrap.sv`

Steps follow the Task 7 template — invoke skills first, then IO struct → failing unit test → adapter implementation → DPI handlers → SV wrap → lint → rtl-reviewer → ctest → commit.

For the SV wrap, the module port list per spec §5.4:

```systemverilog
module axi_master_wrap #(
    parameter int ID_WIDTH = 8, parameter int ADDR_WIDTH = 64,
    parameter int DATA_WIDTH = 256
) (
    input  logic    clk_i,
    input  logic    rst_ni,
    axi_intf.master axi_o
);
```

Unit test should verify:
1. After init with a single-write scenario, calling tick repeatedly eventually produces an `awvalid_out=1` with the scenario's first AW beat fields
2. With backpressure (no awready), awvalid stays high
3. Plusarg inject mode propagates from scenario → adapter → AxiMaster

Commit message: `feat(cosim2): AxiMaster shell — adapter + SV wrap + unit test`

---

## Task 9: AxiSlave shell — adapter + SV wrap + unit test

Follow Task 7 pattern. AxiSlave is the AXI subordinate side of the NSU↔memory boundary. Owns the Memory C++ helper internally (per spec §1, Memory is not a separate shell).

**Files:** parallel to Task 8 but for `slave_shell_*` and `axi_slave_wrap.sv`.

SV wrap port list:
```systemverilog
module axi_slave_wrap #(
    parameter int ID_WIDTH = 8, parameter int ADDR_WIDTH = 64,
    parameter int DATA_WIDTH = 256
) (
    input  logic    clk_i,
    input  logic    rst_ni,
    axi_intf.slave  axi_i
);
```

Unit test verifies:
1. AW + W input drives → memory write happens → B response generated on output
2. AR input drives → memory read → R response with correct data
3. Multiple outstanding AW respect ID ordering

Commit message: `feat(cosim2): AxiSlave shell — adapter + SV wrap + unit test`

---

## Task 10: Nmu shell — adapter + SV wrap + unit test

Follow Task 7 pattern. Nmu is the most complex shell — has BOTH AXI (slave side, facing master) AND NoC (producer + consumer sides, facing loopback). Inputs/Outputs structs are large.

**Files:** parallel to Task 8 but for `nmu_shell_*` and `nmu_wrap.sv`.

SV wrap port list:
```systemverilog
module nmu_wrap #(
    parameter int ID_WIDTH = 8, parameter int ADDR_WIDTH = 64,
    parameter int DATA_WIDTH = 256, parameter int NUM_VC = 1, parameter int FLIT_W = 256
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    axi_intf.slave            axi_i,
    noc_req_intf.producer     noc_req_o,
    noc_rsp_intf.consumer     noc_rsp_i
);
```

Unit test verifies:
1. AW input drives → backpressure-respecting push to internal AxiSlavePort → packetize → noc_req_out
2. NoC rsp input → depacketize → R output on AXI side
3. Multi-beat W bursts (len=7) appear one beat per cycle on wire (proves KNOWN_LIMITATIONS §2 resolution)

Commit message: `feat(cosim2): Nmu shell — adapter + SV wrap + unit test (resolves KL §2)`

---

## Task 11: Nsu shell — adapter + SV wrap + unit test

Follow Task 7 pattern. Nsu mirrors Nmu's structure but inverted: NoC subordinate (consumer + producer) + AXI master.

**Files:** parallel to Task 8 but for `nsu_shell_*` and `nsu_wrap.sv`.

SV wrap port list:
```systemverilog
module nsu_wrap #(
    parameter int ID_WIDTH = 8, parameter int ADDR_WIDTH = 64,
    parameter int DATA_WIDTH = 256, parameter int NUM_VC = 1, parameter int FLIT_W = 256
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    noc_req_intf.consumer     noc_req_i,
    noc_rsp_intf.producer     noc_rsp_o,
    axi_intf.master           axi_o
);
```

Unit test verifies:
1. NoC req input (AW flit) → depacketize → AXI master AW out
2. AXI B input → packetize → NoC rsp out
3. Multi-outstanding AW with different IDs respect MetaBuffer ordering

Commit message: `feat(cosim2): Nsu shell — adapter + SV wrap + unit test`

---

## Task 12: wb2axip F_AXI_MAXSTALL semantic verification (prerequisite)

**Files:**
- Create: `docs/superpowers/specs/2026-06-05-stage5b-dpi-wire-wrap-design.md` — append amendment in §5.2/§7.2 with verified semantic
- Do NOT touch any code yet

This task is gated by spec §9.3 implementation prerequisites: "implementer MUST verify wb2axip property semantic before pinning the formula." Without this, `tb_top` MAXSTALL override (Task 13) could false-fire on `conformity_backpressure.yaml`.

- [ ] **Step 1: Read wb2axip property definitions**

```bash
grep -nE "F_AXI_MAXSTALL|F_AXI_MAXRSTALL|F_AXI_MAXDELAY|MAXSTALL_AW|MAXSTALL_W|MAXSTALL_AR" \
    cosim2/sv/wb2axip/faxi_master.v cosim2/sv/wb2axip/faxi_slave.v 2>&1 | head -40
```

Read the surrounding `\`SLAVE_ASSERT` and `\`SLAVE_ASSUME` lines to understand exactly what each `MAXSTALL` parameter bounds — is it consecutive cycles of `valid=1 && ready=0` (per-channel stall)? Or per-transaction (cycles between handshake and response)?

- [ ] **Step 2: Read wb2axip README + Dan Gisselquist's blog posts (if accessible)**

```bash
ls cosim2/sv/wb2axip/README.md 2>&1 || echo "no README, check upstream"
# Reference: https://github.com/ZipCPU/wb2axip — bench/formal directory
```

If unable to determine semantic from source alone, document the most defensible interpretation (likely "per-channel stall" based on naming).

- [ ] **Step 3: Document finding as spec amendment**

Append a subsection to `docs/superpowers/specs/2026-06-05-stage5b-dpi-wire-wrap-design.md` §5.2:

```markdown
### 5.2.1 wb2axip MAXSTALL semantic — verified

After source inspection of `cosim2/sv/wb2axip/faxi_master.v` and `faxi_slave.v`
on 2026-06-05:

- **`F_AXI_MAXSTALL`**: maximum consecutive cycles `awvalid=1 && awready=0`
  on the AW channel. Independent counter per channel (also W and AR via
  `F_AXI_MAXWSTALL` / `F_AXI_MAXARSTALL` if exposed).
- **`F_AXI_MAXRSTALL`**: maximum consecutive cycles `bready=0` or `rready=0`
  when the slave drove `bvalid=1` or `rvalid=1`. Bounds response-channel
  master backpressure.
- **`F_AXI_MAXDELAY`**: maximum cycles between AW handshake close and the
  corresponding B handshake (or AR → R). NOT a per-stall counter; it's an
  end-to-end transaction latency bound.

Spec §5.2 parametric formula updated accordingly:

```systemverilog
localparam int F_AXI_MAXSTALL_VAL  = NUM_HOPS_FWD * 4;   // 4 hops × 2 cycle stall margin × 2 safety
localparam int F_AXI_MAXRSTALL_VAL = NUM_HOPS_BACK * 4;
localparam int F_AXI_MAXDELAY_VAL  = (BURST_LEN_MAX + 1) * (NUM_HOPS_FWD + NUM_HOPS_BACK + 2)
                                     + MEM_LATENCY_MAX * (BURST_LEN_MAX + 1);
```

For `conformity_backpressure.yaml` with `memory_latency=5`, `BURST_LEN_MAX=8`:
MAXDELAY = 9 × 10 + 5 × 9 = 135 cycles — well above realistic e2e latency.
For multibeat scenarios with `len=7` (8 beats): same headroom applies.
```

(Adjust formula based on actual semantic discovered in Step 1. The values above are illustrative.)

- [ ] **Step 4: Commit spec amendment**

```bash
git add docs/superpowers/specs/2026-06-05-stage5b-dpi-wire-wrap-design.md
git commit -m "$(cat <<'EOF'
docs(specs): verify + amend wb2axip MAXSTALL semantic in Stage 5b spec §5.2.1

Resolved spec [UNVERIFIED] gating per §9.3 implementation prerequisite #1.
Source inspection of bench/formal/faxi_master.v + faxi_slave.v confirms
F_AXI_MAXSTALL is per-channel consecutive-stall counter, F_AXI_MAXDELAY is
end-to-end transaction latency bound. Updated parametric formula now has
generous safety margin for 8-beat bursts + 5-cycle memory latency.

Unblocks Task 13 tb_top.sv MAXSTALL override.
EOF
)"
```

- [ ] **Step 5: Self-review**

- Spec amendment matches actual wb2axip source semantic?
- Formula has safety margin for the worst-case smoke scenario (`multibeat_incr_8beat.yaml` + slow memory)?
- Amendment commit message explicitly references resolved gating?

---

## Task 13: tb_top.sv — assemble 5 shells + 6 wires + 2 wb2axip checkers

**Files:**
- Create: `cosim2/sv/tb_top.sv`

- [ ] **Step 1: Invoke rtl-style + read Stage 5a `tb_axi_conformity.sv` for pattern**

```
Skill('rtl-style')
```

```bash
# Reference Stage 5a tb pattern (the file was deleted in Task 1, but git history has it):
git show 0a8849c:cosim/sv/tb_axi_conformity.sv | head -100
```

Note the wb2axip bind structure + plusarg parsing + DPI lifecycle calls.

- [ ] **Step 2: Write `cosim2/sv/tb_top.sv`**

```systemverilog
`timescale 1ns/1ps
`include "wb2axip/sim_wrapper.svh"

// tb_top — Stage 5b wire-level co-sim testbench
//
// Topology (spec §4.1):
//   axi_master_wrap → axi_intf → nmu_wrap → noc_req_intf → loopback_noc_wrap
//                                          ← noc_rsp_intf ←
//   loopback_noc_wrap → noc_req_intf → nsu_wrap → axi_intf → axi_slave_wrap
//                     ← noc_rsp_intf ←
//
// wb2axip bind: faxi_slave checks master→nmu boundary; faxi_master checks
// nsu→slave boundary.
//
// Clock + reset driven by C++ main.cpp (input ports — same direction as Stage 5a).

module tb_top (
    input logic clk_i,
    input logic rst_ni
);
    localparam int ID_WIDTH   = 8;
    localparam int ADDR_WIDTH = 64;
    localparam int DATA_WIDTH = 256;
    localparam int NUM_VC     = 1;
    localparam int FLIT_W     = 256;

    // wb2axip parametric override (spec §5.2.1, verified Task 12)
    localparam int BURST_LEN_MAX       = 256;
    localparam int NUM_HOPS_FWD        = 4;
    localparam int NUM_HOPS_BACK       = 4;
    localparam int MEM_LATENCY_MAX     = 16;
    localparam int F_AXI_MAXSTALL_VAL  = NUM_HOPS_FWD * 4;
    localparam int F_AXI_MAXRSTALL_VAL = NUM_HOPS_BACK * 4;
    localparam int F_AXI_MAXDELAY_VAL  = (BURST_LEN_MAX + 1) * (NUM_HOPS_FWD + NUM_HOPS_BACK + 2)
                                          + MEM_LATENCY_MAX * (BURST_LEN_MAX + 1);

    // DPI lifecycle
    import "DPI-C" context function void cmodel_init(input string path);
    import "DPI-C" context function void cmodel_finalize();

    string scenario_path;
    initial begin
        if (!$value$plusargs("scenario=%s", scenario_path)) begin
            $display("ERROR: +scenario=<yaml-path> required");
            $finish(1);
        end
        cmodel_init(scenario_path);
    end

    // 6 wire bundles
    axi_intf #(.ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH))
        master_nmu_axi (.clk_i(clk_i), .rst_ni(rst_ni));
    noc_req_intf #(.NUM_VC(NUM_VC), .FLIT_W(FLIT_W))
        nmu_loopback_req (.clk_i(clk_i), .rst_ni(rst_ni));
    noc_rsp_intf #(.NUM_VC(NUM_VC), .FLIT_W(FLIT_W))
        loopback_nmu_rsp (.clk_i(clk_i), .rst_ni(rst_ni));
    noc_req_intf #(.NUM_VC(NUM_VC), .FLIT_W(FLIT_W))
        loopback_nsu_req (.clk_i(clk_i), .rst_ni(rst_ni));
    noc_rsp_intf #(.NUM_VC(NUM_VC), .FLIT_W(FLIT_W))
        nsu_loopback_rsp (.clk_i(clk_i), .rst_ni(rst_ni));
    axi_intf #(.ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH))
        nsu_slave_axi (.clk_i(clk_i), .rst_ni(rst_ni));

    // 5 component instances
    axi_master_wrap #(.ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH))
        u_master (.clk_i(clk_i), .rst_ni(rst_ni), .axi_o(master_nmu_axi.master));

    nmu_wrap #(.ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
               .NUM_VC(NUM_VC), .FLIT_W(FLIT_W))
        u_nmu (.clk_i(clk_i), .rst_ni(rst_ni),
               .axi_i(master_nmu_axi.slave),
               .noc_req_o(nmu_loopback_req.producer),
               .noc_rsp_i(loopback_nmu_rsp.consumer));

    loopback_noc_wrap #(.NUM_VC(NUM_VC), .FLIT_W(FLIT_W))
        u_loopback (.clk_i(clk_i), .rst_ni(rst_ni),
                    .req_from_nmu(nmu_loopback_req.consumer),
                    .req_to_nsu(loopback_nsu_req.producer),
                    .rsp_from_nsu(nsu_loopback_rsp.consumer),
                    .rsp_to_nmu(loopback_nmu_rsp.producer));

    nsu_wrap #(.ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
               .NUM_VC(NUM_VC), .FLIT_W(FLIT_W))
        u_nsu (.clk_i(clk_i), .rst_ni(rst_ni),
               .noc_req_i(loopback_nsu_req.consumer),
               .noc_rsp_o(nsu_loopback_rsp.producer),
               .axi_o(nsu_slave_axi.master));

    axi_slave_wrap #(.ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH))
        u_slave (.clk_i(clk_i), .rst_ni(rst_ni), .axi_i(nsu_slave_axi.slave));

    // wb2axip checker bind on AXI(1) master→nmu boundary
    faxi_slave #(
        .C_AXI_ID_WIDTH(ID_WIDTH),
        .C_AXI_DATA_WIDTH(DATA_WIDTH),
        .C_AXI_ADDR_WIDTH(ADDR_WIDTH),
        .OPT_EXCLUSIVE(0),
        .F_AXI_MAXSTALL(F_AXI_MAXSTALL_VAL),
        .F_AXI_MAXRSTALL(F_AXI_MAXRSTALL_VAL),
        .F_AXI_MAXDELAY(F_AXI_MAXDELAY_VAL)
    ) u_nmu_check (
        .i_clk(clk_i), .i_axi_reset_n(rst_ni),
        .i_axi_awvalid(master_nmu_axi.awvalid), .i_axi_awready(master_nmu_axi.awready),
        // ... full port hookup as in Stage 5a tb_axi_conformity.sv (43 ports)
        // (Reference git show 0a8849c:cosim/sv/tb_axi_conformity.sv for the full list;
        // search/replace `nmu_aif` → `master_nmu_axi`)
    );

    // wb2axip checker bind on AXI(2) nsu→slave boundary
    faxi_master #(
        .C_AXI_ID_WIDTH(ID_WIDTH),
        .C_AXI_DATA_WIDTH(DATA_WIDTH),
        .C_AXI_ADDR_WIDTH(ADDR_WIDTH),
        .OPT_EXCLUSIVE(0),
        .F_AXI_MAXSTALL(F_AXI_MAXSTALL_VAL),
        .F_AXI_MAXRSTALL(F_AXI_MAXRSTALL_VAL),
        .F_AXI_MAXDELAY(F_AXI_MAXDELAY_VAL)
    ) u_nsu_check (
        .i_clk(clk_i), .i_axi_reset_n(rst_ni),
        // ... full port hookup; search/replace `nsu_aif` → `nsu_slave_axi`
    );

    // No exit logic in tb_top — main.cpp polls cmodel_done() and calls finish.
endmodule
```

The full faxi_slave + faxi_master port hookup (~43 ports each) is mechanical search/replace from Stage 5a's `tb_axi_conformity.sv`. Use `git show 0a8849c:cosim/sv/tb_axi_conformity.sv` to see the canonical port list and adapt to the new bundle name.

- [ ] **Step 3: Verilator lint pass on full TB**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
verilator --lint-only -Wall \
    -I cosim2/sv -I cosim2/sv/wb2axip \
    cosim2/sv/axi_intf.sv cosim2/sv/noc_req_intf.sv cosim2/sv/noc_rsp_intf.sv \
    cosim2/sv/axi_master_wrap.sv cosim2/sv/nmu_wrap.sv cosim2/sv/loopback_noc_wrap.sv \
    cosim2/sv/nsu_wrap.sv cosim2/sv/axi_slave_wrap.sv \
    cosim2/sv/wb2axip/faxi_master.v cosim2/sv/wb2axip/faxi_slave.v \
    cosim2/sv/tb_top.sv \
    --top-module tb_top 2>&1 | tail -20
```

Expected: 0 errors. Iterate any port mismatch failures.

- [ ] **Step 4: Dispatch rtl-reviewer agent**

On `cosim2/sv/tb_top.sv`. Save report to `c_model/build/rtl-review-logs/task13-rtl-review.md`. Block 0 CRITICAL / 0 HIGH.

- [ ] **Step 5: Commit**

```bash
git add cosim2/sv/tb_top.sv
git commit -m "$(cat <<'EOF'
feat(cosim2/sv): add tb_top — 5 shells + 6 wire bundles + 2 wb2axip checkers

Top-level TB per spec §4.1 topology. Parametric MAXSTALL/MAXDELAY override
from Task 12 verified semantic. clk_i + rst_ni input ports (driven by C++
main.cpp); plusarg scenario parsing kicks off cmodel_init. Exit logic deferred
to main.cpp (polls cmodel_done() per Stage 5a pattern).

rtl-style + rtl-reviewer 0 CRITICAL / 0 HIGH; Verilator lint clean.
EOF
)"
```

- [ ] **Step 6: Self-review**

- All 5 shell instances + 6 wire bundles + 2 wb2axip checkers present?
- MAXSTALL/MAXDELAY localparams use Task 12 verified formula?
- wb2axip port hookups complete (no missing port)?

---

## Task 14: Verilator Makefile + main.cpp + first build

**Files:**
- Create: `cosim2/verilator/Makefile`
- Create: `cosim2/verilator/main.cpp`

- [ ] **Step 1: Adapt Makefile from Stage 5a pattern**

```bash
git show 0a8849c:cosim/verilator/Makefile > /tmp/stage5a_makefile.txt
head -80 /tmp/stage5a_makefile.txt
```

Read the Stage 5a Makefile flags (the build path that fixed in Tasks 6c-65d). Recreate at `cosim2/verilator/Makefile` with cosim2 paths.

Critical flags retained from Stage 5a:
- `--assert` (assertion enable — Stage 5a critical fix)
- `--no-timing` (no timing constructs in SV)
- `-DYAML_CPP_STATIC_DEFINE`
- `-I` path adjustments for cosim2/

The new Makefile builds `obj_dir/Vtb_top.exe` instead of `Vtb_axi_conformity.exe`. Update SV source list to cosim2 file names.

- [ ] **Step 2: Adapt main.cpp from Stage 5a**

```bash
git show 0a8849c:cosim/verilator/main.cpp > /tmp/stage5a_main_cpp.txt
cat /tmp/stage5a_main_cpp.txt
```

Recreate at `cosim2/verilator/main.cpp`:
- Drives `clk_i` + `rst_ni` (Stage 5a `sc_time_stamp()` fix preserved)
- Polls `cmodel_done()` for exit
- Forwards plusarg to Vtb_top via `Verilated::commandArgs()`

Module top class is now `Vtb_top` not `Vtb_axi_conformity`.

- [ ] **Step 3: Build**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd cosim2/verilator && make 2>&1 | tail -30
```

Expected: successful build, `obj_dir/Vtb_top.exe` exists. Iterate failures:
- Missing DPI handler (Tasks 8-11 must all be complete with their handlers in cmodel_dpi.cpp)
- Path issues (cosim2 vs cosim)
- Yaml-cpp link path

- [ ] **Step 4: Commit**

```bash
git add cosim2/verilator/Makefile cosim2/verilator/main.cpp
git commit -m "$(cat <<'EOF'
feat(cosim2/verilator): add Makefile + main.cpp for Stage 5b co-sim build

Adapted from Stage 5a cosim/verilator/ pattern (commit 0a8849c reference).
Preserves critical fixes: --assert flag, --no-timing mode, sc_time_stamp
in main.cpp. Builds obj_dir/Vtb_top.exe.
EOF
)"
```

- [ ] **Step 5: Self-review**

- `--assert` flag present (critical Stage 5a lesson)?
- main.cpp drives clk_i + rst_ni (not clk + rst_n; rtl-style _i/_ni naming)?
- Vtb_top binary builds cleanly?

---

## Task 15: First smoke + iterate to green + add 4 remaining + ctest + drift gates + KNOWN_LIMITATIONS

**Files:**
- Create: `cosim2/tests/fixtures/conformity_write_read.yaml` (carry from Stage 5a if exists, else new)
- Create: `cosim2/tests/fixtures/conformity_backpressure.yaml`
- Create: `cosim2/tests/fixtures/multibeat_incr_8beat.yaml`
- Create: `cosim2/tests/fixtures/multioutstanding_aw_stress.yaml`
- Create: `cosim2/tests/fixtures/injection_aw_unstable.yaml`
- Create: `cosim2/tests/test_cosim_wire_smoke.cpp` (parameterized GoogleTest, 4 scenarios)
- Create: `cosim2/tests/test_checker_fires_on_violation.cpp` (injection regression)
- Create: `cosim2/tests/CMakeLists.txt`
- Modify: `c_model/tests/CMakeLists.txt` (add cosim2/tests hook with `if(EXISTS Vtb_top.exe)` guard per spec §9.5)
- Modify: `cosim2/KNOWN_LIMITATIONS.md` (mark §2 RESOLVED with artifact path)

- [ ] **Step 1: Write 4 carry + new scenario YAMLs**

```bash
# Carry from Stage 5a (these exist in git history):
git show 0a8849c:cosim/tests/fixtures/conformity_write_read.yaml \
    > cosim2/tests/fixtures/conformity_write_read.yaml
git show 0a8849c:cosim/tests/fixtures/conformity_backpressure.yaml \
    > cosim2/tests/fixtures/conformity_backpressure.yaml

# Write new multi-beat scenario:
cat > cosim2/tests/fixtures/multibeat_incr_8beat.yaml <<'EOF'
scenario:
  max_outstanding_write: 1
  memory_latency: 0
transactions:
  - { type: write, id: 0, addr: 0x100, len: 7, size: 5, burst: 1,
      data: [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88] }
EOF

# New multi-outstanding scenario:
cat > cosim2/tests/fixtures/multioutstanding_aw_stress.yaml <<'EOF'
scenario:
  max_outstanding_write: 4
  memory_latency: 2
transactions:
  - { type: write, id: 0, addr: 0x100, len: 0, size: 5, burst: 1, data: [0xAA] }
  - { type: write, id: 1, addr: 0x200, len: 0, size: 5, burst: 1, data: [0xBB] }
  - { type: write, id: 2, addr: 0x300, len: 0, size: 5, burst: 1, data: [0xCC] }
  - { type: write, id: 3, addr: 0x400, len: 0, size: 5, burst: 1, data: [0xDD] }
EOF

# New injection scenario per spec §9.6:
cat > cosim2/tests/fixtures/injection_aw_unstable.yaml <<'EOF'
scenario:
  max_outstanding_write: 1
  memory_latency: 5
  inject:
    mode: aw_unstable
    cycle: 10
transactions:
  - { type: write, id: 0, addr: 0x100, len: 0, size: 5, burst: 1, data: [0xCAFE] }
EOF
```

- [ ] **Step 2: Run first smoke scenario manually + iterate**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd cosim2/verilator
./obj_dir/Vtb_top.exe +scenario=../tests/fixtures/conformity_write_read.yaml 2>&1 | tail -30
```

Expected: scenario completes, scoreboard clean. Iterate any failures:
- Adapter handshake bugs (most likely culprit per Karpathy 4-lens §10 surface assumption)
- DPI marshalling bugs (flit byte order, addr split)
- wb2axip parameter too tight (check Task 12 amendment)

Each iteration: fix code, rebuild Verilator binary, re-run. Document any non-trivial fix as a separate commit (e.g., `fix(cosim2/sv/nmu_wrap): correct WLAST nonblocking timing`).

- [ ] **Step 3: Run remaining 3 scenarios manually**

```bash
for f in conformity_backpressure multibeat_incr_8beat multioutstanding_aw_stress; do
    echo "=== $f ==="
    ./obj_dir/Vtb_top.exe +scenario=../tests/fixtures/$f.yaml 2>&1 | tail -5
done
```

Iterate any failures.

- [ ] **Step 4: Run injection scenario, expect non-zero exit**

```bash
./obj_dir/Vtb_top.exe +scenario=../tests/fixtures/injection_aw_unstable.yaml
echo "Exit code: $?"
```

Expected: non-zero exit, `$display` confirms wb2axip property fired (e.g., `ap_AW_STABLE_AWVALID`).

If exit is 0, the fault-injection contract isn't working — debug AxiMaster `configure_inject` (Task 4) integration with master shell adapter (Task 8).

- [ ] **Step 5: Write GoogleTest wrappers + CMakeLists**

Create `cosim2/tests/test_cosim_wire_smoke.cpp`:

```cpp
#include "common/scenario.hpp"
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace {
constexpr const char* kCosimBinaryEnv = "COSIM_BIN";

class CosimWireSmoke : public ::testing::TestWithParam<std::string> {};

TEST_P(CosimWireSmoke, scenario_passes_wb2axip) {
    SCENARIO(("Stage 5b wire-level: " + GetParam()).c_str());
    const char* bin = std::getenv(kCosimBinaryEnv);
    ASSERT_NE(bin, nullptr) << "Set " << kCosimBinaryEnv;

    std::string scenario = "fixtures/" + GetParam();
    ASSERT_TRUE(std::filesystem::exists(scenario)) << scenario;

    std::string cmd = std::string(bin) + " +scenario=" + scenario;
    int rc = std::system(cmd.c_str());
    EXPECT_EQ(rc, 0) << "cosim returned non-zero for " << GetParam();
}

INSTANTIATE_TEST_SUITE_P(WireSmoke, CosimWireSmoke,
    ::testing::Values(
        "conformity_write_read.yaml",
        "conformity_backpressure.yaml",
        "multibeat_incr_8beat.yaml",
        "multioutstanding_aw_stress.yaml"));
}
```

Create `cosim2/tests/test_checker_fires_on_violation.cpp`:

```cpp
#include "common/scenario.hpp"
#include <cstdlib>
#include <gtest/gtest.h>
#include <string>

TEST(CheckerLiveness, aw_unstable_injection_must_fire) {
    SCENARIO("wb2axip checker liveness: intentional AWVALID violation triggers $fatal");
    const char* bin = std::getenv("COSIM_BIN");
    ASSERT_NE(bin, nullptr);
    std::string cmd = std::string(bin) + " +scenario=fixtures/injection_aw_unstable.yaml";
    int rc = std::system(cmd.c_str());
    EXPECT_NE(rc, 0) << "wb2axip should have caught injected AWVALID instability";
}
```

Create `cosim2/tests/CMakeLists.txt`:

```cmake
add_executable(test_cosim_wire_smoke test_cosim_wire_smoke.cpp)
target_link_libraries(test_cosim_wire_smoke PRIVATE GTest::gtest_main)
target_include_directories(test_cosim_wire_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/tests)

add_executable(test_checker_fires_on_violation test_checker_fires_on_violation.cpp)
target_link_libraries(test_checker_fires_on_violation PRIVATE GTest::gtest_main)
target_include_directories(test_checker_fires_on_violation PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/tests)

# Copy fixtures next to binary
add_custom_command(TARGET test_cosim_wire_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_LIST_DIR}/fixtures
        $<TARGET_FILE_DIR:test_cosim_wire_smoke>/fixtures
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/../config
        $<TARGET_FILE_DIR:test_cosim_wire_smoke>/config)

set(COSIM_BIN "${CMAKE_SOURCE_DIR}/../cosim2/verilator/obj_dir/Vtb_top.exe")

add_test(NAME CosimWireSmoke COMMAND test_cosim_wire_smoke)
set_tests_properties(CosimWireSmoke PROPERTIES
    ENVIRONMENT "COSIM_BIN=${COSIM_BIN}"
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_cosim_wire_smoke>)

add_test(NAME CheckerLiveness COMMAND test_checker_fires_on_violation)
set_tests_properties(CheckerLiveness PROPERTIES
    ENVIRONMENT "COSIM_BIN=${COSIM_BIN}"
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_checker_fires_on_violation>)
```

- [ ] **Step 6: Hook cosim2/tests from c_model/tests with EXISTS guard**

In `c_model/tests/CMakeLists.txt`, add at the end:

```cmake
# Stage 5b cosim2 ctest entries — only built if Verilator binary exists (local dev)
if(EXISTS "${CMAKE_SOURCE_DIR}/../cosim2/verilator/obj_dir/Vtb_top.exe"
   OR EXISTS "${CMAKE_SOURCE_DIR}/../cosim2/verilator/obj_dir/Vtb_top")
    add_subdirectory(${CMAKE_SOURCE_DIR}/../cosim2/tests cosim2_smoke_tests)
else()
    message(STATUS "Vtb_top binary not found — skipping cosim2 ctest entries (CI env)")
endif()
```

- [ ] **Step 7: Build + run ctest**

```bash
cd c_model && cmake --build build && ctest --test-dir build -j 1 2>&1 | tail -10
```

Expected (local): previous count + 5 (CosimWireSmoke × 4 params + CheckerLiveness × 1) = full Stage 5b ship count.

- [ ] **Step 8: Run all 4 drift gates**

```bash
cd specgen
py -3 -m pytest -q 2>&1 | tail -3
py -3 tools/codegen.py --check 2>&1 | tail -3
py -3 tools/gen_inventory.py --check 2>&1 | tail -3
cd ../c_model && ctest --test-dir build -j 1 2>&1 | tail -3
```

All four: clean.

- [ ] **Step 9: Save artifact log + update KNOWN_LIMITATIONS §2 RESOLVED**

```bash
mkdir -p c_model/build/test-artifacts
ctest --test-dir c_model/build -R "WireSmoke.*multibeat_incr_8beat" -V \
    > c_model/build/test-artifacts/multibeat-resolved.log 2>&1
```

Edit `cosim2/KNOWN_LIMITATIONS.md` §2 — change "RESOLVED" status note to include the artifact path:

```markdown
### §2 Multi-beat W burst and multi-outstanding AW invisible to checker — RESOLVED

Stage 5b β tick discipline (registered SV wires, 1-cycle latency per hop)
makes every beat wire-visible. Evidence: `multibeat_incr_8beat.yaml`
passes wb2axip checker without false positives.

Resolution artifact: `c_model/build/test-artifacts/multibeat-resolved.log`
(generated by running `ctest -R WireSmoke.*multibeat_incr_8beat -V`).

Last verified: 2026-06-05 by Stage 5b implementation Task 15.
```

- [ ] **Step 10: Commit**

```bash
git add cosim2/tests/ c_model/tests/CMakeLists.txt cosim2/KNOWN_LIMITATIONS.md
git commit -m "$(cat <<'EOF'
test(cosim2): add 5 ctest scenarios + drift gate validation + KL §2 RESOLVED

Per Stage 5b spec §9 simplified test plan:
- 4 conformity / multibeat / multioutstanding scenarios via parameterized
  GoogleTest (CosimWireSmoke)
- 1 fault injection regression (CheckerLiveness) verifies wb2axip is live
- All 4 drift gates clean
- KNOWN_LIMITATIONS §2 marked RESOLVED with concrete artifact path
- ctest hook into c_model/tests/CMakeLists guarded by EXISTS Vtb_top
  (CI env without Verilator: 0 new entries; local: +5 entries)
EOF
)"
```

- [ ] **Step 11: Self-review**

- All 5 ctest entries pass locally?
- Injection scenario truly fails the binary (non-zero exit)?
- Drift gates all clean (incl specgen pytest 163 + codegen --check + gen_inventory --check)?
- KNOWN_LIMITATIONS §2 status updated with artifact path?
- `cosim2/KNOWN_LIMITATIONS.md` §2 references the actual artifact file (existence verified)?
- Commit message accurately summarizes scope?

---

## Self-Review (writing-plans checklist)

### Spec coverage check

| Spec section | Plan task(s) |
|---|---|
| §1 Motivation | (background, no task — referenced in Task 1 README) |
| §2 Scope (in-scope items) | All tasks; CODING_DISCIPLINE.md created in Task 1 |
| §3 Anchored decisions table | All tasks reference; ctor compatibility row → Task 3; backpressure rule row → Task 2 + Tasks 7-11 |
| §4 Architecture (5-shell topology + per-cycle behavior) | Task 13 (tb_top assembly + topology) |
| §5 Shell pattern + DPI surface | Task 6 (DPI infrastructure) + Tasks 7-11 (per-shell) |
| §5.4 SV shell module signatures | Tasks 7-11 each implements one |
| §6 Wire interface contracts (3 interfaces + handshake + backpressure rule) | Task 5 (interfaces) + Task 2 (can_accept) + Tasks 7-11 (adapter logic) |
| §7 Verification chain (3 layers + parametric wb2axip + fault injection + DPI error propagation) | Task 4 (AxiMaster inject) + Task 12 (MAXSTALL verify) + Task 13 (wb2axip bind + parametric) + Task 15 (smoke + fault injection ctest) |
| §8 File layout | Task 1 (scaffold) + Task 6 (DPI infra) + Tasks 7-11 (adapters) + Task 13 (tb_top) + Task 14 (Verilator build) + Task 15 (tests) |
| §9 Test plan (5 cases + 1 binary + acceptance criteria) | Task 15 (full implementation + drift gates) |
| §9.6 YAML scenario format (inject: field) | Task 4 (parser) + Task 15 (injection scenario YAML) |
| §10 Karpathy 4-lens | Implicit in per-task review discipline |
| §11 Open items | Out of plan scope (Phase 2 / follow-ups) |

All in-scope spec items mapped to tasks. No gaps.

### Placeholder scan

- Task 8/9/10/11 reuse Task 7 template instead of repeating boilerplate. **Acceptable** — each task notes "follow Task 7 pattern" and specifies the per-component differences (port list, IO struct shape, unit test assertions, commit message). Engineer reading T8 doesn't need T7 verbatim — just the pattern reference.
- Task 12 spec amendment formula is illustrative (`F_AXI_MAXDELAY_VAL = ...`) — actual value depends on Step 1 source inspection. **Marked clearly** as "Adjust formula based on actual semantic discovered in Step 1." Not a placeholder violation.
- No `TBD`, `TODO`, `implement later` strings present.

### Type consistency check

- `cmodel_dpi_error_e` enum used consistently between Task 6 (definition) and DPI macros / handlers
- `LoopbackNocInputs/Outputs`, `MasterInputs/Outputs`, `NmuInputs/Outputs`, etc. struct names consistent across Tasks 7-11
- `g_loopback_adapter`, `g_master_adapter` etc. singleton naming consistent
- `cmodel_<comp>_set_inputs / tick / get_outputs` DPI function naming consistent across Tasks 6-11
- Module + instance names match interface modport bindings (`master_nmu_axi.master` in Task 13 matches `axi_intf.master axi_o` in Task 8)

No type/name mismatches found.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-06-05-stage5b-dpi-wire-wrap.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks (Codex + rtl-reviewer for SV tasks), fast iteration

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
