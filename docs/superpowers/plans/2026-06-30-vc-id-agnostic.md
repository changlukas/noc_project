# Make VC selection id-agnostic + NMU single-outstanding interlock — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the cross-transaction AXI-id->VC pin so VC selection is id-agnostic round-robin by class (matching FlooNoC), while keeping same-id ordering correct (NMU RoB / Disabled single-outstanding) and R-burst VC coherence.

**Architecture:** Four tasks on the header-only c_model. (1) Delete the unit-test-only MultiCandidate VC mode to collapse `VcArbiter` to one mode. (2) Delete the NMU cross-transaction id pin + the now-dead RoB drain observer. (3) Change the NMU RoB Disabled gate to same-id single-outstanding. (4) Keep + rename the NSU R-burst follow, delete the NSU B no-op binding.

**Tech Stack:** C++17 header-only c_model, GoogleTest, CMake/ctest.

**Spec:** `docs/superpowers/specs/2026-06-30-vc-binding-removal-design.md`.

## Global Constraints

- Build: `make build-cmodel`. Run a suite: `ctest --test-dir build/cmodel -R <regex> --output-on-failure`. Full: `make test`. If `make build-cmodel` hits the pre-existing host GCC ICE on `test_pins_smoke.cpp` (backlog "Infra / portability"), report it BLOCKED — do not work around inline.
- After editing any `.hpp`/`.cpp`: run `/c/msys64/mingw64/bin/clang-format -i <file>` (clang-format is not on PATH).
- Commit format `type(scope): description`, English body, end with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Never `--no-verify`.
- Work on branch `feat/vc-id-agnostic` (already created; the spec commit `d1ddba5` is its tip).
- Naming: snake_case for vars/methods, PascalCase for types. Full words, no abbreviation.
- Every commit compiles, passes all existing tests, and includes tests for new behavior.

---

### Task 1: Delete the MultiCandidate VC mode (collapse to one mode)

`MultiCandidate` is unit-test-only — every production config wires `VcMode::ReadWriteSplit` (`nmu_wrap.hpp:54`, `nsu_wrap.hpp:58`). Removing it first simplifies the selection code the later tasks edit.

**Files:**
- Modify: `c_model/include/nmu/vc_arbiter.hpp`, `c_model/include/nsu/vc_arbiter.hpp`
- Modify: `c_model/include/nmu/nmu.hpp`, `c_model/include/nsu/nsu.hpp`
- Test: `c_model/tests/nmu/test_vc_arbiter.cpp`, `c_model/tests/nsu/test_nsu_vc_arbiter.cpp`

**Interfaces:**
- Produces: `VcArbiter` with no `VcMode`/`mode_`/`multi_candidate`/`candidate_vcs_`. Factories kept: `read_write_split(downstream, num_vc, write_vc, read_vc, depth)`, `read_write_split_pools(downstream, num_vc, write_vcs, read_vcs, depth)`. `NmuConfig`/`NsuConfig` lose `vc_mode` and `vc_candidates`.

- [ ] **Step 1: Delete the MultiCandidate test cases**

  Remove these `TEST`/`TEST_P` bodies entirely:
  - `test_vc_arbiter.cpp`: `MultiCandidate_HoLAvoidance`, `RoundRobinFairness_AllVcsServiced_NoStarvation`, and the Mode B block inside `Degenerate_NumVc1_AllModesPassthrough` (keep the Mode A block).
  - `test_nsu_vc_arbiter.cpp`: `Nsu_MultiCandidate_HoLAvoidance`, `Nsu_RoundRobinFairness`, `Nsu_CreditGating` (these three build via `multi_candidate`), and the Mode B block inside `Nsu_Degenerate_NumVc1_Passthrough`.

  For `RoundRobinFairness` and `Nsu_CreditGating` the round-robin / credit behavior they covered is re-covered on the pools path by the existing `DistinctReadIdsSpreadAcrossPool` / `DistinctRidsSpreadAcrossPool` and `CreditGating_TickIdleWhenAllVcsBlocked`, so no replacement is needed.

- [ ] **Step 2: Run the suites to confirm only the deleted tests are gone**

  Run: `make build-cmodel && ctest --test-dir build/cmodel -R "NmuVcArb|NsuVcArb|NmuVcArbiter|NsuVcArbiter" --output-on-failure`
  Expected: PASS (the remaining cases still build against `multi_candidate`, which is removed next).

- [ ] **Step 3: Remove MultiCandidate from `nmu/vc_arbiter.hpp`**

  - Delete the `enum class VcMode { ReadWriteSplit, MultiCandidate };`.
  - Delete the `multi_candidate(...)` factory and the `mode_` member + ctor `VcMode mode` parameter.
  - Delete the `candidate_vcs_` member and the `multi_candidate` ctor wiring.
  - Simplify `candidates_for`:
    ```cpp
    const std::vector<uint8_t>* candidates_for(uint8_t axi_ch) const {
        return axi_ch == ni::AXI_CH_AW ? &write_vcs_ : &read_vcs_;
    }
    ```
  - Update the two factories to call the ctor without the `VcMode`/`candidate_vcs_` args.

- [ ] **Step 4: Remove MultiCandidate from `nsu/vc_arbiter.hpp`**

  - Same enum/factory/`mode_`/`candidate_vcs_` deletion.
  - Delete the `if (mode_ == VcMode::MultiCandidate) { ... }` branch in `select_vc_for_axi_ch` (the per-candidate loop at lines ~122-126). The remaining body is the scalar (`!use_pools_`) and pools paths.

- [ ] **Step 5: Drop the config fields + factory branch**

  - `nmu/nmu.hpp`: delete `VcMode vc_mode` and the `vc_candidates` field from `NmuConfig`; in `make_vc_arbiter` delete the `if (cfg.vc_mode == VcMode::ReadWriteSplit) {...}` guard so it always builds ReadWriteSplit (pools if `write_vcs`/`read_vcs` set, else scalar), and delete the trailing `multi_candidate` branch (`nmu.hpp:269-271`).
  - `nsu/nsu.hpp`: same (`nsu.hpp:142,151-153`).
  - Fix the callers that set `cfg.vc_mode`: `test_vc_arbiter.cpp` `ConfigPoolsBuildSpreadingArbiter` and `test_nsu_vc_arbiter.cpp` `ConfigPoolsBuildSpreadingArbiter` — delete the `cfg.vc_mode = ...` line.
  - `nmu_wrap.hpp:54` / `nsu_wrap.hpp:58`: delete the `cfg.vc_mode = VcMode::ReadWriteSplit;` line.

- [ ] **Step 6: clang-format the 4 headers, build, run full suites**

  Run: `make build-cmodel && ctest --test-dir build/cmodel --output-on-failure`
  Expected: PASS, fewer tests than before (deleted ones gone), zero failures.

- [ ] **Step 7: Commit**

```bash
git add c_model/include/nmu/vc_arbiter.hpp c_model/include/nsu/vc_arbiter.hpp \
        c_model/include/nmu/nmu.hpp c_model/include/nsu/nsu.hpp \
        c_model/include/wrap/nmu_wrap.hpp c_model/include/wrap/nsu_wrap.hpp \
        c_model/tests/nmu/test_vc_arbiter.cpp c_model/tests/nsu/test_nsu_vc_arbiter.cpp
git commit -m "refactor(vc): delete unit-test-only MultiCandidate VC mode"
```

---

### Task 2: Delete NMU cross-transaction id pin + dead RoB drain observer

The id pin (`write_binding_`/`read_binding_`) and the RoB drain observer that releases it are one coupled unit — deleting the binding leaves `on_id_drained` callers dangling, so both go together.

**Files:**
- Modify: `c_model/include/nmu/vc_arbiter.hpp`, `c_model/include/nmu/rob.hpp`, `c_model/include/nmu/nmu.hpp`
- Test: `c_model/tests/nmu/test_vc_arbiter.cpp`, `c_model/tests/nmu/test_rob.cpp`, `c_model/tests/nmu/test_nmu_rob_staging.cpp`

**Interfaces:**
- Consumes: the single-mode `VcArbiter` from Task 1.
- Produces: `select_vc_for_axi_ch(uint8_t axi_ch)` (no `id` param); `push_flit` no longer reads awid/arid; `VcArbiter` has no `on_id_drained`. `Rob` has no `set_drain_observer`/`drain_observer_`/`notify_drained`.

- [ ] **Step 1: Replace the NMU binding tests with a round-robin test**

  In `test_vc_arbiter.cpp` delete: `Binding_SameWriteId_SameVc`, `Binding_PerIdStickyAndDistinctIdsIndependent`, `Binding_RebindAfterDrain` (uses `on_id_drained`), `Binding_MidFlightSameIdReusesVc`, `Binding_DistinctIdsSpreadUnderDepthPressure`, `SingleReadIdPinsToOneVc` (line 418).

  `DistinctReadIdsSpreadAcrossPool` (line 401) stays — it already proves id-agnostic round-robin. Add the same-id counterpart that the pin used to prevent:
  ```cpp
  // Same id no longer pins a VC: consecutive same-id ARs round-robin the read pool.
  TEST(NmuVcArbiterRoundRobin, SameReadIdRoundRobinsAcrossPool) {
      SCENARIO("NMU VcArbiter pools: same arid round-robins the read pool (no id pin)");
      ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
      auto arb = VcArbiter::read_write_split_pools(noc.req_out(), /*num_vc=*/4,
                                                   /*write_vcs=*/{0, 1}, /*read_vcs=*/{2, 3});
      uint8_t a = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/0x20));
      uint8_t b = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/0x20));
      EXPECT_EQ(a, 2u);
      EXPECT_EQ(b, 3u) << "same id must NOT pin; round-robin advances to the next pool VC";
  }
  ```

- [ ] **Step 2: Delete the RoB observer tests**

  - `test_rob.cpp`: delete `RobDrainHook, FiresOnPerIdOutstandingEmpty_Disabled` (line 544) and any other `RobDrainHook` cases below it.
  - `test_nmu_rob_staging.cpp`: this whole file verifies `notify_drained` timing (§6.1). Delete the file and its `add_test`/CMake entry, OR if the S2-drain-boundary timing is still worth keeping, strip every `notify_drained` reference and re-point the assertion to `pop_r()` availability only. Default: delete the file (its sole subject is the deleted observer).

- [ ] **Step 3: Run to verify those tests fail to compile (observer still present)**

  Run: `make build-cmodel 2>&1 | head -30`
  Expected: still builds (tests deleted, observer not yet removed). This step just confirms the test deletions are syntactically clean before touching the headers.

- [ ] **Step 4: Remove the id pin from `nmu/vc_arbiter.hpp`**

  - Delete members `write_binding_`, `read_binding_`, the `AXI_ID_SPACE` alias, and `on_id_drained`.
  - Change `select_vc_for_axi_ch(uint8_t axi_ch, uint8_t id)` to `select_vc_for_axi_ch(uint8_t axi_ch)`; delete the binding lookup block (lines ~166-173), keeping the W branch, the `num_vc_==1` short-circuit, and the round-robin loop:
    ```cpp
    inline std::optional<uint8_t> VcArbiter::select_vc_for_axi_ch(uint8_t axi_ch) {
        if (axi_ch == ni::AXI_CH_W) { /* current_aw_vc_ branch UNCHANGED */ }
        if (num_vc_ == 1) return uint8_t{0};
        const std::vector<uint8_t>* cand = candidates_for(axi_ch);
        if (axi_ch != ni::AXI_CH_AW && axi_ch != ni::AXI_CH_AR) return std::nullopt;
        uint8_t& rr = (axi_ch == ni::AXI_CH_AW) ? write_rr_start_ : read_rr_start_;
        const std::size_t n = cand->size();
        for (std::size_t k = 0; k < n; ++k) {
            uint8_t vc = (*cand)[(rr + k) % n];
            if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) {
                rr = static_cast<uint8_t>((static_cast<std::size_t>(rr) + k + 1) % n);
                return vc;
            }
        }
        return std::nullopt;
    }
    ```
  - In `push_flit`: delete the `num_vc_>1` awid/arid read block and the bind-on-accept commit block; call `select_vc_for_axi_ch(axi_ch)`.
  - Delete the `#include "axi/types.hpp"` if nothing else in the file uses it (it provided `AXI_ID_SPACE`). Rewrite the file-top comment block describing the binding.

- [ ] **Step 5: Remove the observer from `nmu/rob.hpp` and wiring from `nmu/nmu.hpp`**

  - `rob.hpp`: delete `set_drain_observer`, the `drain_observer_` member, `notify_drained`, and its call sites: `pop_b` (line ~285), `pop_r` (~302), `commit_b_exit` (~433), `commit_r_exit` (~446). Delete `#include <functional>` if unused elsewhere.
  - `nmu.hpp`: delete the `rob_.set_drain_observer([this]...)` wiring (lines ~294-295).

- [ ] **Step 6: clang-format, build, run full suites**

  Run: `make build-cmodel && ctest --test-dir build/cmodel --output-on-failure`
  Expected: PASS, including the new `SameReadIdRoundRobinsAcrossPool`.

- [ ] **Step 7: Commit**

```bash
git add c_model/include/nmu/vc_arbiter.hpp c_model/include/nmu/rob.hpp c_model/include/nmu/nmu.hpp \
        c_model/tests/nmu/test_vc_arbiter.cpp c_model/tests/nmu/test_rob.cpp
git rm c_model/tests/nmu/test_nmu_rob_staging.cpp   # if deleted in Step 2
git commit -m "refactor(vc): remove NMU id->VC pin and the dead RoB drain observer"
```

---

### Task 3: NMU RoB Disabled gate -> same-id single-outstanding

With the VC pin gone, Disabled mode must guarantee same-id response ordering by allowing only one outstanding transaction per id. This replaces the same-id different-dst stall (which assumed single-VC FIFO ordering).

**Files:**
- Modify: `c_model/include/nmu/rob.hpp`
- Test: `c_model/tests/nmu/test_rob.cpp`

**Interfaces:**
- Produces: `Rob` Disabled mode stalls `push_aw`/`push_ar` whenever that id already has one outstanding. `write_occupancy()`/`read_occupancy()` unchanged in signature (now count set flags).

- [ ] **Step 1: Flip the gate test that contradicts single-outstanding**

  In `test_rob.cpp`:
  - Delete `Disabled_MultiOutstandingSameIdSameDst_NoFalseStall` (line 149) — single-outstanding makes a second same-id-same-dst AW stall, the opposite of what it asserts.
  - Rename `Disabled_StallSameIdDiffDst` (line 82) to `Disabled_StallSecondSameId` and change the second AW to the SAME dst (single-outstanding stalls regardless of dst):
    ```cpp
    TEST(NmuRob, Disabled_StallSecondSameId) {
        SCENARIO("Rob Disabled: a second same-id AW stalls until the first AW's B returns (single-outstanding)");
        // Reuse the existing test_rob.cpp stub harness (pkt/depkt + Rob ctor, see line ~76):
        //   Rob rob{pkt, depkt, RobMode::Disabled, RobMode::Disabled};
        ni::cmodel::axi::AwBeat aw{}; aw.id = 5; aw.addr = 0x100000000;  // dst=0
        EXPECT_TRUE(rob.push_aw(aw));
        ni::cmodel::axi::AwBeat aw2{}; aw2.id = 5; aw2.addr = 0x100000040;  // SAME dst=0
        EXPECT_FALSE(rob.push_aw(aw2)) << "second same-id AW must stall (one outstanding per id)";
    }
    ```
  - `Disabled_StallReleaseOnBComplete` / `Disabled_StallReleaseOnRlast` / `Disabled_DifferentIdsIndependentNoInterference` stay (release on response + per-id independence are unchanged); adjust their second-request addr to the same dst if they relied on different-dst to trigger the stall.

- [ ] **Step 2: Run to confirm the renamed/edited test fails (gate still different-dst)**

  Run: `make build-cmodel && ctest --test-dir build/cmodel -R "NmuRob.Disabled" --output-on-failure`
  Expected: `Disabled_StallSecondSameId` FAILS (current gate admits same-dst).

- [ ] **Step 3: Change the gate + collapse the deque**

  In `rob.hpp`:
  - `push_aw` Disabled path: replace
    ```cpp
    if (!s.outstanding.empty() && s.outstanding.front().dst_id != t.dst_id) return false;
    ```
    with
    ```cpp
    if (s.write_outstanding) return false;   // single-outstanding per id
    ```
    and on accept set `s.write_outstanding = true;` instead of `push_back`.
  - `push_ar` Disabled path: same with `read_outstanding`.
  - `pop_b` Disabled: replace `s.outstanding.pop_front()` with `s.write_outstanding = false;` (drop the `notify_drained` already removed in Task 2). `pop_r` Disabled on `opt->last`: `s.read_outstanding = false;`.
  - Replace `struct WriteState { std::deque<OutstandingEntry> outstanding; };` (and ReadState) with a single `bool write_outstanding = false;` / `bool read_outstanding = false;`. Delete `struct OutstandingEntry`.
  - `write_occupancy()`/`read_occupancy()`: count set flags (`for (s : write_) n += s.write_outstanding ? 1 : 0;`).

- [ ] **Step 4: Rewrite the ordering-owner comment**

  Replace the `rob.hpp:40-43` comment so it states: RoB-less (Disabled) same-id response ordering is guaranteed by the NMU single-outstanding interlock (one transaction in flight per id), not by AxiMaster ordering or XY routing.

- [ ] **Step 5: clang-format, build, run**

  Run: `make build-cmodel && ctest --test-dir build/cmodel -R "NmuRob" --output-on-failure`
  Expected: PASS, including `Disabled_StallSecondSameId`. Then full `ctest --test-dir build/cmodel` PASS.

- [ ] **Step 6: Commit**

```bash
git add c_model/include/nmu/rob.hpp c_model/tests/nmu/test_rob.cpp
git commit -m "feat(vc): NMU RoB Disabled gate becomes same-id single-outstanding"
```

---

### Task 4: Rename the NSU R-burst follow, delete the B no-op binding

The NSU read-side follow is burst coherence (one R burst stays on one VC), not an id pin — keep it, rename for clarity. The B-side binding is reset on the same push (single-flit), a no-op — delete it.

**Files:**
- Modify: `c_model/include/nsu/vc_arbiter.hpp`
- Test: `c_model/tests/nsu/test_nsu_vc_arbiter.cpp`

**Interfaces:**
- Consumes: single-mode `VcArbiter` from Task 1.
- Produces: NSU `VcArbiter` keeps the read follow (renamed `r_burst_vc_`); B selects via round-robin directly.

- [ ] **Step 1: Confirm the protective tests exist and pass**

  `RBurstStaysOnOneVc` (line 184) and `InterleavedMultiBeatBurstsStayOnTheirOwnVc` (line 227) are the R-burst-coherence guards. They must keep passing across the rename. Run:
  `make build-cmodel && ctest --test-dir build/cmodel -R "NsuVcArbiterPools" --output-on-failure`
  Expected: PASS (baseline before the rename).

- [ ] **Step 2: Rename the read binding, delete the B binding**

  In `nsu/vc_arbiter.hpp`:
  - Rename member `read_binding_` -> `r_burst_vc_` (still `std::array<std::optional<uint8_t>, axi::AXI_ID_SPACE>`), and update the file-top + inline comments to: "R-follows-first-beat: the first R beat of an `rid` round-robins a VC; later beats of that `rid` reuse it; released on payload `rlast`. The id is a burst-grouping key, not a VC selector."
  - Delete `write_binding_` and its `B`-path read/commit/reset. In `push_flit`, the B branch no longer reads `bid` or touches a binding; B falls through to `select_vc_for_axi_ch` round-robin and stamps.
  - In `select_vc_for_axi_ch`, the `AXI_CH_B` pools path returns a round-robin pick over `write_rsp_vcs_` (no binding lookup); the `AXI_CH_R` path keeps the `r_burst_vc_` lookup -> round-robin -> stamp.
  - In `push_flit`, keep the R release on payload `rlast` (`r_burst_vc_[rid].reset()`); delete the B release block.

- [ ] **Step 3: Update the B-side test wording**

  `BUsesWritePoolRUsesReadPool` (line 213) stays (B still uses the write pool); if it asserted B-binding stickiness, weaken to "B uses the write pool" only. Add, if not already covered, that two same-id B responses may land on different write-pool VCs (round-robin, no pin):
  ```cpp
  TEST(NsuVcArbiterPools, SameBidRoundRobinsWritePool) {
      SCENARIO("NSU VcArbiter pools: same bid round-robins the write pool (B has no pin)");
      ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
      auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                   /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
      uint8_t a = push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_B, 0, /*id=*/0x40));
      uint8_t b = push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_B, 0, /*id=*/0x40));
      EXPECT_EQ(a, 0u);
      EXPECT_EQ(b, 1u) << "same bid must not pin; B round-robins the write pool";
  }
  ```
  (Use the file's existing `push_and_vc`/`make_rsp_flit` helpers; confirm `make_rsp_flit` signature `(axi_ch, initial_vc, id, rlast)`.)

- [ ] **Step 4: clang-format, build, run NSU + full**

  Run: `make build-cmodel && ctest --test-dir build/cmodel -R "NsuVcArb" --output-on-failure`
  Expected: PASS, including the unchanged `RBurstStaysOnOneVc` / `InterleavedMultiBeatBurstsStayOnTheirOwnVc` (rename did not change behavior) and the new `SameBidRoundRobinsWritePool`. Then full `ctest --test-dir build/cmodel` PASS.

- [ ] **Step 5: Commit**

```bash
git add c_model/include/nsu/vc_arbiter.hpp c_model/tests/nsu/test_nsu_vc_arbiter.cpp
git commit -m "refactor(vc): rename NSU R-burst follow, drop the B no-op binding"
```

---

## Post-plan verification (after all 4 tasks)

- [ ] Full suite green: `make test` (or `ctest --test-dir build/cmodel --output-on-failure`).
- [ ] Grep for stragglers: no remaining `write_binding_`/`read_binding_`/`on_id_drained`/`notify_drained`/`VcMode`/`multi_candidate` outside intended history.
- [ ] Co-sim re-run of `AX4-ORD-002` (multi-id concurrent write, currently excluded for a hang) to check whether removing the binding clears it — record the result in `docs/backlog.md` regardless of outcome. This is verification, not part of any task's commit.
- [ ] Update `docs/backlog.md`: strike the "per-id VC binding (NEXT ROUND, fix first)" item; note the MultiCandidate dead-mode prune is done; record the `ORD-002` re-run result.
