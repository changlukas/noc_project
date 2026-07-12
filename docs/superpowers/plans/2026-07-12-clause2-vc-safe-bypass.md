# Clause-2 VC-safe bypass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make NMU RoB bypass clause 2 (same-id same-dest transactions skip a reorder slot) safe under multi-VC round-robin, so R-RoB SRAM can shrink, with zero new packet fields.

**Architecture:** Pin the injection VC by (dst, id) on both networks. Forward: `nmu::Rob` adds clause 2 (sticky `prev_dest`); `nmu::VcArbiter` keeps a per-id last-VC and reuses it for a same-dst bypass, else round-robins. Return: `nsu::VcArbiter` static-maps `rsp_vc = f(dst_id, id)` for bypassed responses and deletes `r_burst_vc_`. Both use existing header/payload fields (`dst_id`, `bid`/`rid`, `rob_req`).

**Tech Stack:** C++17 header-only model + GoogleTest (ctest); Python 3 generator (`sim/tools/gen_test_patterns.py`); Verilator co-sim (`make sim`).

**Spec:** `docs/superpowers/specs/2026-07-12-clause2-vc-safe-bypass-design.md`. Design rationale + trade-offs there; ordering trade-offs in `docs/nmu-rob-microarchitecture.md` section 5a.

## Global Constraints

- **Zero new packet fields.** No new header/payload field on any flit. Use existing `dst_id`, `bid`/`rid`, `rob_req`.
- **Both sides land together.** Forward (Stage 1) and return (Stage 2) are correct only jointly; do not merge one without the other. Stage 3 verifies the pair.
- **GATED.** Stage 0 is a go/no-go. Stages 1-3 execute only if Stage 0 funds an `r_rob_depth` cut. If not, stop — clause-1-only (status quo) is the correct answer at zero cost.
- **Fault-injection before trusting any checker** (Stage 3 first step): drive the failing case, confirm the abort/scoreboard fires, then enable the mechanism.
- C++17, header-only model. `clang-format -i` every `.hpp`/`.cpp` touched (repo-root `.clang-format`, Google base + IndentWidth 4 + ContinuationIndentWidth 4 + ColumnLimit 100).
- WSL build/co-sim only, foreground-blocking: `BUILD_ROOT=$HOME/noc_build`, `PYTHON3=python3`, `VERILATOR=verilator` (repo-root gitignored `local.mk`). ctest via `make test`.
- Commit incrementally: every commit compiles, passes ctest, message `type(scope): description`. End with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

---

## Stage 0 — GATE: measure the clause-2 area headroom (execute first)

**Purpose:** clause 2 fires only for multi-outstanding same-id **same-dest** streaks. Under clause-1-only (current HEAD) such a streak forces every 2nd+ same-id txn into an R slot. The high-water of that slot usage is the area clause 2 could reclaim. Measure it; build only if it funds an `r_rob_depth` cut.

### Task 0.1: R-slot high-water instrumentation in `nmu::Rob`

**Files:**
- Modify: `src/c_model/include/nmu/rob.hpp` (add `read_slot_hwm_` member + getter; update in `push_ar` Enabled path)
- Test: `src/c_model/tests/nmu/test_rob.cpp` (add one test)

**Interfaces:**
- Produces: `std::size_t Rob::read_slot_hwm() const noexcept` — max over the run of `r_rob_depth_ - read_free_space()` (occupied R slots), updated whenever an AR allocates in Enabled mode.

- [ ] **Step 1: Write the failing test.** In `test_rob.cpp`, after the existing burst tests:

```cpp
TEST(RobSlotHwm, SameIdSameDestStreakRaisesReadHwm) {
    ChannelModel chan;
    Packetize pkt(chan, chan, chan, kSrcId, legacy_sam());
    Depacketize depkt(/* mirror the existing Enabled-mode fixtures in this file */);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam(),
            /*b_rob_depth=*/32, /*r_rob_depth=*/32, /*max_txns_per_id=*/32);

    // 5 single-beat reads, same id, same dest (addr in one tile), none drained.
    const uint8_t id = 3;
    const uint64_t addr = 0x100000000ull * 4;  // dst tile 4, len 0
    for (int i = 0; i < 5; ++i) EXPECT_TRUE(rob.push_ar(make_ar(id, addr)));

    // Clause 1 bypasses the 1st (no slot); the next 4 each take a slot.
    EXPECT_EQ(rob.read_slot_hwm(), 4u);
}
```

- [ ] **Step 2: Run it, verify it fails** — `read_slot_hwm` undefined.
  Run: `make test` (or the single target once built). Expected: compile error / FAIL.

- [ ] **Step 3: Implement.** In `rob.hpp`, add the member near `read_release_offset_`:

```cpp
std::size_t read_slot_hwm_ = 0;
```

Add the getter next to `read_occupancy()`:

```cpp
std::size_t read_slot_hwm() const noexcept { return read_slot_hwm_; }
```

In `push_ar`, inside the `if (needs_rob)` block after `alloc_read_.set(base + n - 1)`:

```cpp
read_slot_hwm_ = std::max<std::size_t>(read_slot_hwm_, r_rob_depth_ - read_free_space());
```

(Add `#include <algorithm>` if not present.)

- [ ] **Step 4: Run test, verify pass.** Run: `make test`. Expected: PASS, full suite green.
- [ ] **Step 5: `clang-format -i src/c_model/include/nmu/rob.hpp src/c_model/tests/nmu/test_rob.cpp`, commit.**

```bash
git commit -am "feat(rob): add R-slot high-water instrumentation for clause-2 gate"
```

### Task 0.2: same-dest multi-outstanding stimulus

**Files:**
- Inspect first: `sim/tools/gen_test_patterns.py` (does `neighbor` + `--ids-per-tile 1` + high `--count` already produce one id, one dest, many outstanding?)
- Modify only if the inspection shows a gap.

- [ ] **Step 1: Confirm the existing path.** `neighbor` gives each src a single fixed dest. With `--ids-per-tile 1` each tile drives one AXI id. A high `--count` with the Enabled-mode master pipelining multiple outstanding is the same-dest streak clause 2 targets. Run the generator and read one node's stimulus file to confirm: single id, single dest tile, N transactions.

Run: `python3 sim/tools/gen_test_patterns.py --pattern neighbor --ids-per-tile 1 --count 32 --out /tmp/ss` then inspect a node file.

- [ ] **Step 2: If confirmed, no code change** — record the exact invocation in the plan's Stage-0 measurement (Task 0.3). If the master does NOT keep multiple outstanding for one id under Enabled mode (check `AxiMaster`/`axi_file_master` outstanding depth), add a generator note or a `--outstanding` knob; otherwise skip. (Do not add a knob speculatively — confirm the gap first.)

### Task 0.3: measure clause-1-only R-slot high-water at vc4/vc8

**Files:**
- Inspect: `src/c_model/include/wrap/nmu_wrap.hpp`, `src/dpi/cmodel_dpi.cpp` — how a per-NMU scalar is surfaced to the co-sim (existing perf path or an end-of-run print).

- [ ] **Step 1: Surface `read_slot_hwm()` from the co-sim.** Follow the existing perf/report path (the NSU MetaBuffer already emits a result CSV; mirror that). If no clean path exists, add an end-of-sim print in the NMU wrap that reports `read_slot_hwm()` per node. Keep it behind the existing run, no new knob.

- [ ] **Step 2: Run clause-1-only (current HEAD, Enabled mode) same-dest streaming at vc4 and vc8.**

```bash
make sim TB=tb_mesh_4x4_vc4_rob PATTERN=neighbor SEED=1 INJECTION_COUNT=32
make sim TB=tb_mesh_4x4_vc8_rob PATTERN=neighbor SEED=1 INJECTION_COUNT=32
```

Record the max `read_slot_hwm` across nodes for each.

- [ ] **Step 3: DECISION GATE.** Report the numbers. Interpretation:
  - High-water H is a large fraction of `r_rob_depth` (current 32) — say `H >= r_rob_depth/4` — means clause 2 can reclaim ~H slots and a real depth cut follows → **proceed to Stage 1**.
  - H is tiny (the master stays single-outstanding per id, so clause 2 rarely fires) → **STOP**. Record the number in `docs/backlog.md`; clause-1-only is the answer. Do NOT build Stages 1-3.
  - The exact threshold is a judgment call — present H to the user with the r_rob_depth it would justify, and get the go/no-go before Stage 1.

---

> **Stages 1-3 below are GATED on Stage 0's go decision.** They are specified at task granularity (files, interfaces, key design, test strategy). Before executing, the running session expands each task's steps against the code as it then stands (the exact line anchors will have shifted, and Stage 0's instrumentation may inform the shape). Do not implement any of Stage 1-3 until Stage 0 returns go.

## Stage 1 — Forward path (gated)

### Task 1.1: clause 2 in `nmu::Rob` (sticky prev_dest)

**Files:** Modify `src/c_model/include/nmu/rob.hpp`; test `src/c_model/tests/nmu/test_rob.cpp`.

**Interfaces:**
- Consumes: existing `push_aw`/`push_ar` Enabled path, `sam_.translate(addr).dst_id`.
- Produces: `needs_rob` is now false also when the id's order list is non-empty AND `dst == prev_dest[id]` AND the id has not fallen back. New per-direction state `prev_dest_[id]` and sticky `fallen_back_[id]` (arrays sized `AXI_ID_SPACE`), mirroring FlooNoC `floo_rob_status_table` (`floo_rob.sv:399,423-441`).

**Design:** On each `push_aw`/`push_ar`, compute `dst = sam_.translate(addr).dst_id`. Bypass decision:
- order list empty → clause 1 bypass (unchanged), record `prev_dest_[id] = dst`, clear `fallen_back_[id]`.
- else if `!fallen_back_[id] && dst == prev_dest_[id]` → **clause 2 bypass** (`needs_rob = false`).
- else → set `fallen_back_[id] = true`, allocate a slot (robbed). Always update `prev_dest_[id] = dst`.
- Clear `fallen_back_[id]` when the id's order list drains (in the pop path where the deque empties).

**Test strategy (write first, TDD):** (a) same-id same-dest streak: 2nd+ txn bypasses (order-list entries carry `rob_req=false`, no slot consumed — assert `read_slot_hwm()` stays 0). (b) dest change mid-streak: the changing txn and every later same-id txn robs until the id drains, then a fresh same-dest streak bypasses again. (c) invariant: at most one bypassed entry per id is NO LONGER true under clause 2 — assert `max_txns_per_id` gate still bounds the order list (admission refused past the limit). Keep the existing head-invariant abort tests green.

### Task 1.2: per-id last-VC follow in `nmu::VcArbiter`

**Files:** Modify `src/c_model/include/nmu/vc_arbiter.hpp`; test `src/c_model/tests/nmu/test_vc_arbiter*.cpp` (locate the existing NMU vc_arbiter test).

**Interfaces:**
- Consumes: flit header `dst_id`, `rob_req` (already stamped by `nmu::Packetize`, `packetize.hpp:113,116`).
- Produces: `last_aw_vc_[AXI_ID_SPACE]`, `last_ar_vc_[AXI_ID_SPACE]` (optional<uint8_t> or a valid-flag). A `rob_req==0` flit whose `dst_id == last_dst_[id]` reuses `last_vc_[id]`; else the existing credit-aware round-robin runs and updates `(last_dst_, last_vc_)[id]`.

**Design:** In `push_flit`, read `dst_id` and `rob_req`. For AW/AR with `rob_req==0`: if the id's `last_dst_` matches, force `vc = last_vc_[id]` (skip the round-robin scan); on a normal round-robin selection, store `(last_dst_[id], last_vc_[id])`. `rob_req==1` flits and W (via `current_aw_vc_`) are unchanged. Ordering is race-free: AW/AR each traverse one wormhole-input FIFO, so txn N sets `last_vc_[id]` before its follower reads it.

**Test strategy:** (a) two same-id same-dest bypass flits get the SAME vc_id even across a round-robin pointer bump. (b) same-id different-dest bypass flit round-robins fresh (last_dst mismatch). (c) robbed flit round-robins regardless. (d) NUM_VC=1 degenerate unchanged.

## Stage 2 — Return path (gated)

### Task 2.1: static-map rsp VC in `nsu::VcArbiter`, delete `r_burst_vc_`

**Files:** Modify `src/c_model/include/nsu/vc_arbiter.hpp`; test `src/c_model/tests/nsu/test_vc_arbiter*.cpp`.

**Interfaces:**
- Consumes: response flit header `dst_id` (= requester, `nsu/packetize.hpp:89,104`), payload `bid`/`rid`, header `rob_req` (`:92,107`).
- Produces: for `rob_req==0` B/R, `rsp_vc = pool[f(dst_id, id) % |pool|]` with `f = dst_id ^ id` (any fixed pure function); pinned-VC full → refuse. `rob_req==1` keeps round-robin. `r_burst_vc_` member and its set/reset logic (`vc_arbiter.hpp:95,116,144,152-153`) are removed.

**Design:** In `push_flit`/`select_vc_for_axi_ch`, read `rob_req` and `dst_id` from the flit (B and R alike; R already extracts payload `rid` at `:136-137`). Compute the static map for `rob_req==0`; for `rob_req==1` use the existing pool round-robin. Delete `r_burst_vc_` entirely — under a constant map, all beats of a bypassed R burst share `(dst_id, rid)` so intra-burst coherence is automatic; robbed R also no longer needs the burst pin (the NMU slot path is order-free). This also removes the `remap_downstream_id` src_id constraint noted at `meta_buffer.hpp:22-28` (W6) — but do NOT change `remap_downstream_id` in this task; just record that the constraint is now liftable.

**Test strategy:** (a) two same-(dst,id) bypass B responses map to the SAME rsp VC. (b) two bypass R bursts of the same (dst,id) map to the same VC and all beats follow. (c) different (dst,id) bypass responses can differ. (d) robbed responses round-robin. (e) W6 case: two sources (different `dst_id`) with the same restored rid get distinct pins — no contention.

## Stage 3 — Co-sim verification + area (gated)

### Task 3.1: fault-injection, functional co-sim, HoL acceptance, area cut

**Files:** co-sim run configs; `docs/backlog.md` (record results); `src/c_model/include/wrap` or Makefile for the `r_rob_depth` cut.

- [ ] **Step 1: Fault-injection FIRST.** With Stage 1+2 code present but the pin DISABLED (e.g. a temporary force to round-robin), run vc4/vc8 same-dest streaming and confirm the head-invariant abort (`rob.hpp:381/422`) fires. Proves the checker is live. Then enable the pin.
- [ ] **Step 2: Functional.** `make sim TB=tb_mesh_4x4_vc4_rob` and `vc8` with same-dest streaming and the standard directed patterns (neighbor/transpose/uniform_random/hotspot): scoreboard clean, non-vacuous, 16 nodes.
- [ ] **Step 3: HoL acceptance.** hotspot before/after (pin off vs on) saturation throughput + tail latency. The pin must not materially regress hotspot; if it does, the area win did not pay for itself — report and reconsider.
- [ ] **Step 4: Area.** Cut `r_rob_depth` to the Stage-0-justified value, re-run functional, confirm green. Record the area delta (pin state ~2 Kb flops; saving = freed R slots × one data beat of SRAM) in `docs/backlog.md`.
- [ ] **Step 5: Commit + close the backlog item.**

## Self-review notes

- **Spec coverage:** Stage 0 = spec Stage 0 gate; Task 1.1/1.2 = spec forward path; Task 2.1 = spec return path (RZ1 + r_burst_vc_ delete, option b); Task 3.1 = spec Stage 3 verification (fault-injection, functional, HoL, area). Dependency (forward+return together) enforced by Global Constraints and Stage 3 verifying the pair.
- **Gate honored:** Stages 1-3 explicitly gated; Stage 0 ends in a go/no-go with the user.
- **Zero-new-field:** every task uses existing fields (`dst_id`, `bid`/`rid`, `rob_req`); no task adds a header/payload field.
- **Granularity note:** Stage 0 is step-level (executes now). Stages 1-3 are task-level by design — they are gated and their line anchors will shift; the executing session expands steps against current code. This is deliberate (YAGNI: no per-line code for work that may not happen), not a placeholder gap.
