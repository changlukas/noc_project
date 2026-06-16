# VC ID Binding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a sticky `(channel, axi_id) → vc` binding to the NMU `VcArbiter` so a multi-VC config keeps each AXI flow on one VC end-to-end, with the binding released by a Rob per-id drain hook.

**Architecture:** Four tasks. (1) Generalize `ReadWriteSplit` to a per-channel candidate VC set and unify VC selection to "first-available over the set" — behavior-preserving refactor. (2) Add binding tables + payload-id read + bind-on-accept/reuse. (3) Add `VcArbiter::on_id_drained`. (4) Wire a Rob drain observer that fires `on_id_drained` when a per-id outstanding source empties, in `Nmu` and `NmuStandalone`.

**Tech Stack:** C++17 header-only c_model, GoogleTest, `make build-cmodel` / `ctest`.

**Spec:** `docs/superpowers/specs/2026-06-13-vc-id-binding-design.md`.

**Conventions (all tasks):**
- Build: `make build-cmodel`. Run a suite: `ctest --test-dir build/cmodel -R <regex> --output-on-failure` (set `TEST_TMPDIR` if a test needs it; vc_arbiter tests do not). Full: `make test`.
- After editing any `.hpp`/`.cpp`: `/c/msys64/mingw64/bin/clang-format -i <file>` (clang-format is NOT on PATH).
- Commit format `type(scope): description`, English body. Never `--no-verify`.
- Work on a feature branch: first step of Task 1 creates it.

**Key code facts (verified):**
- `c_model/include/nmu/vc_arbiter.hpp`: `select_vc_for_axi_ch(axi_ch)` (line 108), `push_flit` (140); members `write_vc_`/`read_vc_`/`candidate_vcs_[AXI_CH_COUNT]`/`pending_[NUM_VC_MAX]`/`current_aw_vc_`; factories `read_write_split` / `multi_candidate`; `NUM_VC_MAX=8`, `AXI_CH_COUNT=5`.
- Payload id fields: `flit.get_payload_field("AW","awid")`, `flit.get_payload_field("AR","arid")`.
- `c_model/include/nmu/rob.hpp`: Disabled drain in `pop_b` (line 259-264: `write_[opt->id].outstanding.pop_front()`) and `pop_r` (read_); Enabled drain in the chain-flush loops (`write_order_by_id_[id]` ~line 244-251, `read_order_by_id_[id]` ~line 296-303). `axi::AXI_ID_SPACE == 256`.
- `c_model/include/nmu/nmu.hpp`: `Nmu` holds `vc_arbiter_` + `rob_` as members (ctor line 118-130, empty body); `NmuStandalone` (line ~140+) is a second wrapper with its own Rob+VcArbiter used by shell adapters.
- `c_model/tests/nmu/test_vc_arbiter.cpp`: `make_flit(axi_ch, dst_id=0, initial_vc=0, wlast=0)` (line 20) does NOT set awid/arid; parameterized fixture `NmuVcArbParam` over num_vc.

---

### Task 1: Generalize ReadWriteSplit to candidate VC sets (behavior-preserving)

**Files:**
- Modify: `c_model/include/nmu/vc_arbiter.hpp`
- Modify: `c_model/tests/nmu/test_vc_arbiter.cpp` (extend `make_flit` only)

- [ ] **Step 1: Create the feature branch**

```bash
git checkout -b feat/vc-id-binding
```

- [ ] **Step 2: Extend `make_flit` to carry an AXI id (test helper, no behavior change yet)**

In `c_model/tests/nmu/test_vc_arbiter.cpp`, change the `make_flit` signature (line ~20) to accept an `id` and stamp the payload:
```cpp
Flit make_flit(uint8_t axi_ch, uint8_t dst_id = 0, uint8_t initial_vc = 0, uint64_t wlast = 0,
               uint8_t id = 0) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", initial_vc);
    f.set_header_field("src_id", 0x12);
    f.set_header_field("last", 1);
    if (axi_ch == ni::AXI_CH_W) {
        f.set_payload_field("W", "wlast", wlast);
    } else if (axi_ch == ni::AXI_CH_AW) {
        f.set_payload_field("AW", "awid", id);
    } else if (axi_ch == ni::AXI_CH_AR) {
        f.set_payload_field("AR", "arid", id);
    }
    return f;
}
```
Existing call sites pass no `id`, so they default to 0 — behavior unchanged this task.

- [ ] **Step 3: Run the vc_arbiter suite to confirm the helper change is green**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R "NmuVcArb" --output-on-failure`
Expected: all existing vc_arbiter tests PASS (28 cases across the num_vc grid).

- [ ] **Step 4: Replace single write_vc_/read_vc_ with candidate sets + add a pools factory**

In `vc_arbiter.hpp`:
- Replace the members `uint8_t write_vc_;` and `uint8_t read_vc_;` with:
```cpp
    std::vector<uint8_t> write_vcs_;
    std::vector<uint8_t> read_vcs_;
```
- Change the private ctor to take `std::vector<uint8_t> write_vcs, std::vector<uint8_t> read_vcs` instead of `uint8_t write_vc, uint8_t read_vc`, store them, and replace the asserts `assert(write_vc_ < num_vc_)` / `assert(read_vc_ < num_vc_)` with a loop asserting every element `< num_vc_` (only when the vector is non-empty):
```cpp
        for (uint8_t v : write_vcs_) assert(v < num_vc_);
        for (uint8_t v : read_vcs_) assert(v < num_vc_);
```
- Keep the `read_write_split(downstream, num_vc, write_vc, read_vc, depth)` factory signature for backward compatibility; internally build size-1 vectors:
```cpp
    static VcArbiter read_write_split(noc::NocReqOut& downstream, std::size_t num_vc,
                                      uint8_t write_vc, uint8_t read_vc,
                                      std::size_t pending_depth = kDefaultPendingDepth) {
        std::array<std::vector<uint8_t>, AXI_CH_COUNT> empty_candidates{};
        return VcArbiter(downstream, num_vc, VcMode::ReadWriteSplit,
                         std::vector<uint8_t>{write_vc}, std::vector<uint8_t>{read_vc},
                         std::move(empty_candidates), pending_depth);
    }
```
- Add a new pools factory:
```cpp
    static VcArbiter read_write_split_pools(noc::NocReqOut& downstream, std::size_t num_vc,
                                            std::vector<uint8_t> write_vcs,
                                            std::vector<uint8_t> read_vcs,
                                            std::size_t pending_depth = kDefaultPendingDepth) {
        std::array<std::vector<uint8_t>, AXI_CH_COUNT> empty_candidates{};
        return VcArbiter(downstream, num_vc, VcMode::ReadWriteSplit, std::move(write_vcs),
                         std::move(read_vcs), std::move(empty_candidates), pending_depth);
    }
```
- Update the `multi_candidate` factory call to pass empty `write_vcs`/`read_vcs` vectors instead of `0, 0`.

- [ ] **Step 5: Unify selection to "first-available over the candidate set"**

Replace `select_vc_for_axi_ch` (line 108) body's mode branches with a unified candidate scan. Keep the W branch and `num_vc_==1` short-circuit unchanged. After them:
```cpp
    const std::vector<uint8_t>* cand = nullptr;
    if (mode_ == VcMode::ReadWriteSplit) {
        if (axi_ch == ni::AXI_CH_AW) cand = &write_vcs_;
        else if (axi_ch == ni::AXI_CH_AR) cand = &read_vcs_;
        else return std::nullopt;
    } else {  // MultiCandidate
        cand = &candidate_vcs_[axi_ch];
    }
    for (uint8_t vc : *cand) {
        if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) return vc;
    }
    return std::nullopt;
```
Rationale: for a size-1 ReadWriteSplit set this returns the single vc when it has room and `nullopt` when full — observably identical to the old unconditional `return write_vc_` (which then backpressured in `push_flit`'s pending check). For MultiCandidate it is the existing first-available logic verbatim. No binding yet.

- [ ] **Step 6: Build + run the full vc_arbiter suite and the NMU integration tests**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R "NmuVcArb|Nmu\.|Packetize|Loopback" --output-on-failure`
Expected: all PASS (this is a behavior-preserving refactor). If `make_vc_arbiter` in `nmu.hpp` or any caller fails to compile because it referenced `write_vc_`/`read_vc_` directly, fix the call to use the unchanged `read_write_split(...)` factory (the config path in `nmu.hpp:106-114` already calls the factory, so no change expected). If a test fails, STOP — the refactor was not behavior-preserving; report the failing assertion.

- [ ] **Step 7: clang-format + commit**

```bash
/c/msys64/mingw64/bin/clang-format -i c_model/include/nmu/vc_arbiter.hpp c_model/tests/nmu/test_vc_arbiter.cpp
git add c_model/include/nmu/vc_arbiter.hpp c_model/tests/nmu/test_vc_arbiter.cpp
git commit -m "refactor(nmu): generalize ReadWriteSplit to per-channel candidate VC sets"
```

---

### Task 2: Sticky (channel, id) → vc binding (bind-on-accept + reuse)

**Files:**
- Modify: `c_model/include/nmu/vc_arbiter.hpp`
- Modify: `c_model/tests/nmu/test_vc_arbiter.cpp`

- [ ] **Step 1: Write failing tests for stickiness, independence, and the updated MultiCandidate semantics**

The existing tests use a real `ChannelModel noc(req_depth, rsp_depth)` as the downstream: push to the arbiter, `arb.tick()` to drain one flit into the channel, then `noc.req_in().pop_flit()->get_header_field("vc_id")` to observe the assigned VC (see lines 50-69 of the file). Add a small local helper near `make_flit` and use it:
```cpp
// Push a flit, drain one to the channel, return the assigned vc_id.
uint8_t push_and_vc(VcArbiter& arb, ChannelModel& noc, const Flit& f) {
    EXPECT_TRUE(arb.push_flit(f));
    arb.tick();
    auto out = noc.req_in().pop_flit();
    EXPECT_TRUE(out.has_value());
    return static_cast<uint8_t>(out->get_header_field("vc_id"));
}

TEST_P(NmuVcArbParam, Binding_SameWriteId_SameVc) {
    SCENARIO("VcArbiter: same AWID across packets binds to one VC (multi-element write set)");
    const auto num_vc = GetParam();
    if (num_vc < 4) GTEST_SKIP() << "needs >=4 VCs for a multi-element write set";
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.req_out(), num_vc, {0, 1}, {2, 3});
    uint8_t v1 = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AW, 0, 0, 0, /*id=*/7));
    // Close the AW's burst so current_aw_vc_ resets before the next AW.
    EXPECT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/1)));
    arb.tick();
    noc.req_in().pop_flit();
    uint8_t v2 = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AW, 0, 0, 0, /*id=*/7));
    EXPECT_EQ(v1, v2) << "same AWID must reuse the same VC";
}

TEST_P(NmuVcArbParam, Binding_PerIdStickyAndDistinctIdsIndependent) {
    SCENARIO("VcArbiter: ARID re-binds to its own VC; distinct ARIDs are independent");
    const auto num_vc = GetParam();
    if (num_vc < 4) GTEST_SKIP() << "needs >=4 VCs";
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.req_out(), num_vc, {0, 1}, {2, 3});
    uint8_t a = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/1));
    uint8_t b = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/2));
    EXPECT_TRUE(a == 2 || a == 3) << "ARID=1 must land in the read set {2,3}";
    EXPECT_TRUE(b == 2 || b == 3) << "ARID=2 must land in the read set {2,3}";
    uint8_t a2 = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/1));
    EXPECT_EQ(a2, a) << "ARID=1 must re-bind to its original VC";
}
```

- [ ] **Step 2: Run to verify the new tests fail**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R "Binding_" --output-on-failure`
Expected: FAIL — without binding, `Binding_DifferentReadIds_CanSpread`'s re-push of id=1 may not return the original VC, and stickiness isn't enforced.

- [ ] **Step 3: Add binding tables and bind-on-accept logic**

In `vc_arbiter.hpp`:
- Add members (after `current_aw_vc_`):
```cpp
    static constexpr std::size_t AXI_ID_SPACE = 256;  // matches axi::AXI_ID_SPACE
    std::array<std::optional<uint8_t>, AXI_ID_SPACE> write_binding_{};
    std::array<std::optional<uint8_t>, AXI_ID_SPACE> read_binding_{};
```
- Change `select_vc_for_axi_ch` to take the id and consult the binding first. New signature `select_vc_for_axi_ch(uint8_t axi_ch, uint8_t id)`; after the W and `num_vc_==1` branches:
```cpp
    std::array<std::optional<uint8_t>, AXI_ID_SPACE>* binding = nullptr;
    const std::vector<uint8_t>* cand = nullptr;
    if (axi_ch == ni::AXI_CH_AW) { binding = &write_binding_; cand = candidates_for(ni::AXI_CH_AW); }
    else if (axi_ch == ni::AXI_CH_AR) { binding = &read_binding_; cand = candidates_for(ni::AXI_CH_AR); }
    else return std::nullopt;
    if ((*binding)[id].has_value()) return (*binding)[id];   // bound: stick (even if full)
    for (uint8_t vc : *cand) {                                // unbound: first available
        if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) return vc;
    }
    return std::nullopt;
```
  where `candidates_for` is a small private helper returning the right set per mode:
```cpp
    const std::vector<uint8_t>* candidates_for(uint8_t axi_ch) const {
        if (mode_ == VcMode::ReadWriteSplit)
            return axi_ch == ni::AXI_CH_AW ? &write_vcs_ : &read_vcs_;
        return &candidate_vcs_[axi_ch];
    }
```
- In `push_flit` (line 140): read the id and commit the binding only after the pending-depth check passes (accepted flit):
```cpp
    uint8_t axi_ch = static_cast<uint8_t>(flit.get_header_field("axi_ch"));
    uint8_t id = 0;
    if (axi_ch == ni::AXI_CH_AW) id = static_cast<uint8_t>(flit.get_payload_field("AW", "awid"));
    else if (axi_ch == ni::AXI_CH_AR) id = static_cast<uint8_t>(flit.get_payload_field("AR", "arid"));
    auto vc_opt = select_vc_for_axi_ch(axi_ch, id);
    if (!vc_opt.has_value()) return false;
    uint8_t vc_id = *vc_opt;
    if (pending_[vc_id].size() >= pending_depth_) return false;
    // commit binding on accept (AW/AR only)
    if (axi_ch == ni::AXI_CH_AW) write_binding_[id] = vc_id;
    else if (axi_ch == ni::AXI_CH_AR) read_binding_[id] = vc_id;
    // ... existing current_aw_vc_ update + stamp + enqueue (unchanged) ...
```

- [ ] **Step 4: Run the new binding tests + fix the now-broken MultiCandidate spreading test**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R "Binding_|MultiCandidate" --output-on-failure`
Expected: `Binding_*` PASS. `MultiCandidate_HoLAvoidance` now FAILS — it pushes multiple ARs with id=0, which now all pin to one VC. UPDATE that test (line ~81) to give each AR a **distinct** `id` so HoL avoidance across distinct flows still holds:
```cpp
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/1)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/2)));
    // ... give the overflow AR a third distinct id so it can land on another VC ...
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/3)));
```
Keep the test's assertion that the ARs spread across VCs (now justified by distinct ids). Read the actual assertions and adjust the ids so the spreading still occurs.

- [ ] **Step 5: Run the full vc_arbiter suite**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R "NmuVcArb|Binding_" --output-on-failure`
Expected: all PASS. If any other existing test pushed same-channel flits expecting spread, update it to distinct ids the same way (the size-1 ReadWriteSplit tests are unaffected — one vc regardless).

- [ ] **Step 6: clang-format + commit**

```bash
/c/msys64/mingw64/bin/clang-format -i c_model/include/nmu/vc_arbiter.hpp c_model/tests/nmu/test_vc_arbiter.cpp
git add c_model/include/nmu/vc_arbiter.hpp c_model/tests/nmu/test_vc_arbiter.cpp
git commit -m "feat(nmu): sticky (channel,id)->vc binding in VcArbiter"
```

---

### Task 3: `on_id_drained` release API + rebind test

**Files:**
- Modify: `c_model/include/nmu/vc_arbiter.hpp`
- Modify: `c_model/tests/nmu/test_vc_arbiter.cpp`

- [ ] **Step 1: Write the failing rebind + mid-flight tests**

```cpp
TEST_P(NmuVcArbParam, Binding_RebindAfterDrain) {
    SCENARIO("VcArbiter: on_id_drained frees a binding; the id then re-binds (and sticks anew)");
    const auto num_vc = GetParam();
    if (num_vc < 4) GTEST_SKIP() << "needs >=4 VCs";
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.req_out(), num_vc, {0, 1}, {2, 3});
    uint8_t first = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/5));
    EXPECT_TRUE(first == 2 || first == 3);
    arb.on_id_drained(/*is_write=*/false, /*id=*/5);   // release the binding
    // After release the id is unbound: re-select picks a read-set VC and sticks again.
    uint8_t rebind = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/5));
    EXPECT_TRUE(rebind == 2 || rebind == 3);
    uint8_t rebind2 = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/5));
    EXPECT_EQ(rebind2, rebind) << "after re-binding, id=5 sticks to the new VC";
}

TEST_P(NmuVcArbParam, Binding_MidFlightSameIdReusesVc) {
    SCENARIO("VcArbiter: a second same-id packet before any drain reuses the same VC");
    const auto num_vc = GetParam();
    if (num_vc < 4) GTEST_SKIP() << "needs >=4 VCs";
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.req_out(), num_vc, {0, 1}, {2, 3});
    uint8_t v = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/9));
    uint8_t v2 = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/9));  // no drain release
    EXPECT_EQ(v2, v);
}
```
Note: this test does not try to force `rebind != first` (the harness can't deterministically evict a single-depth VC without extra plumbing); it verifies the release happened (the id is re-selectable) and that the new choice sticks. That is sufficient to exercise the lifecycle.

- [ ] **Step 2: Run to verify failure (compile error: `on_id_drained` missing)**

Run: `make build-cmodel 2>&1 | tail` — Expected: compile error, `on_id_drained` not a member.

- [ ] **Step 3: Implement `on_id_drained`**

In `vc_arbiter.hpp` public section:
```cpp
    // Release the (class,id) binding when its outstanding count reaches 0.
    // Called by the NMU Rob drain hook. is_write selects the write vs read table.
    void on_id_drained(bool is_write, uint8_t id) {
        if (is_write) write_binding_[id].reset();
        else read_binding_[id].reset();
    }
```

- [ ] **Step 4: Run the new tests**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R "Binding_" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: clang-format + commit**

```bash
/c/msys64/mingw64/bin/clang-format -i c_model/include/nmu/vc_arbiter.hpp c_model/tests/nmu/test_vc_arbiter.cpp
git add c_model/include/nmu/vc_arbiter.hpp c_model/tests/nmu/test_vc_arbiter.cpp
git commit -m "feat(nmu): VcArbiter::on_id_drained binding release API"
```

---

### Task 4: Rob drain hook + NMU wiring

**Files:**
- Modify: `c_model/include/nmu/rob.hpp`
- Modify: `c_model/include/nmu/nmu.hpp`
- Test: `c_model/tests/nmu/test_rob.cpp` (drain-fires-hook), then full suite

- [ ] **Step 1: Write a failing test that the Rob fires the drain observer on the empty transition**

In `c_model/tests/nmu/test_rob.cpp`, add (Disabled mode, the simplest path). Use the file's existing Rob test scaffold (it constructs a Rob with stub Packetize/Depacketize — reuse it):
```cpp
TEST(RobDrainHook, FiresOnPerIdOutstandingEmpty_Disabled) {
    SCENARIO("Rob: drain observer fires (is_write,id) exactly when that id's outstanding empties");
    // Build a Disabled-mode Rob wired to stubs (mirror the existing test_rob fixtures).
    std::vector<std::pair<bool, uint8_t>> drained;
    rob.set_drain_observer([&](bool w, uint8_t id) { drained.emplace_back(w, id); });
    // Push two AWs of id=4 (same dst), then deliver two Bs.
    // After the FIRST B: outstanding for id=4 still has 1 -> no fire.
    // After the SECOND B: outstanding empties -> exactly one fire (true,4).
    // ... drive push_aw x2 (same id,dst) through, then pop_b x2 with matching B beats ...
    ASSERT_EQ(drained.size(), 1u);
    EXPECT_EQ(drained[0].first, true);
    EXPECT_EQ(drained[0].second, 4);
}
```
Fill the push/pop drive code from the existing `test_rob.cpp` patterns (how it injects B beats via the stub depacketizer). If the existing scaffold cannot easily inject responses, model the test on whatever the file's other pop_b tests already do.

Add a second test for the Enabled drain source (spec §9.7 — Disabled and Enabled both free the binding). Construct the Rob with `RobMode::Enabled` for writes; the drain fires when `write_order_by_id_[id]` empties after the chain-flush in `pop_b`. Mirror the Disabled test's structure (same `set_drain_observer`, push two same-id AWs, deliver the matching Bs with their `rob_idx` meta, assert exactly one `(true, id)` fire on the empty transition):
```cpp
TEST(RobDrainHook, FiresOnPerIdOutstandingEmpty_Enabled) {
    SCENARIO("Rob Enabled mode: drain observer fires when write_order_by_id_[id] empties");
    // Build an Enabled-mode Rob; reuse the file's Enabled pop_b drive helpers
    // (push_aw assigns rob_idx; pop_b_with_meta delivers B with that rob_idx).
    // Same assertion shape as the Disabled test: exactly one (true, id) fire.
}
```
If `test_rob.cpp` has no Enabled-mode response-injection helper, model it on the file's existing Enabled `pop_b` tests (they already drive rob_idx-tagged responses). If Enabled response injection is genuinely not expressible with the existing scaffold, report DONE_WITH_CONCERNS rather than skipping the case — do not silently drop §9.7 coverage.

- [ ] **Step 2: Run to verify failure (compile error: `set_drain_observer` missing)**

Run: `make build-cmodel 2>&1 | tail` — Expected: compile error.

- [ ] **Step 3: Add the drain observer to Rob and fire it on the empty transition**

In `rob.hpp`:
- Include `<functional>`. Add a member + setter:
```cpp
  public:
    // Fired with (is_write, axi_id) when that id's outstanding source empties on a pop.
    void set_drain_observer(std::function<void(bool, uint8_t)> cb) { drain_observer_ = std::move(cb); }
  private:
    std::function<void(bool, uint8_t)> drain_observer_;
    void notify_drained(bool is_write, uint8_t id) { if (drain_observer_) drain_observer_(is_write, id); }
```
- Disabled `pop_b` (after `s.outstanding.pop_front();`, line ~263):
```cpp
    s.outstanding.pop_front();
    if (s.outstanding.empty()) notify_drained(/*is_write=*/true, opt->id);
    return opt;
```
- Disabled `pop_r` (mirror, with `is_write=false`, the read id).
- Enabled `pop_b` chain-flush loop: after the `while` that pops `write_order_by_id_[id]`, add:
```cpp
    if (write_order_by_id_[id].empty()) notify_drained(/*is_write=*/true, id);
```
  (id is `slot.axi_id`, already in scope.) Enabled `pop_r`: same against `read_order_by_id_[id]` with `is_write=false`.

- [ ] **Step 4: Run the Rob test**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R "RobDrainHook|Rob\\." --output-on-failure`
Expected: PASS, and all existing Rob tests still PASS (the observer is null by default → `notify_drained` is a no-op when unset).

- [ ] **Step 5: Wire the hook in `Nmu` and `NmuStandalone`**

In `nmu.hpp`, in the `Nmu` constructor body (currently empty `{}` at line 130), add:
```cpp
{
    rob_.set_drain_observer(
        [this](bool is_write, uint8_t id) { vc_arbiter_.on_id_drained(is_write, id); });
}
```
Do the identical wiring in the `NmuStandalone` constructor (find its `rob_` + `vc_arbiter_` members; add the same `set_drain_observer` in its ctor body). If `NmuStandalone` reuses `Nmu` internally rather than holding its own Rob/VcArbiter, wire only once where the real Rob+VcArbiter live.

- [ ] **Step 6: Build + run the FULL suite**

Run: `make build-cmodel && make test`
Expected: all green (the integration/loopback tests now exercise the hook on the response path with default single-VC, where binding is trivially id→vc0 and the hook just resets vc0 — harmless). Confirm the full count matches the pre-change baseline plus the new tests.

- [ ] **Step 7: clang-format + commit**

```bash
/c/msys64/mingw64/bin/clang-format -i c_model/include/nmu/rob.hpp c_model/include/nmu/nmu.hpp c_model/tests/nmu/test_rob.cpp
git add c_model/include/nmu/rob.hpp c_model/include/nmu/nmu.hpp c_model/tests/nmu/test_rob.cpp
git commit -m "feat(nmu): Rob per-id drain hook releases VcArbiter binding; wire in Nmu"
```

---

## Out of scope (later sub-projects)

- B: RouterChannel integration (drop-in for ChannelModel, 2-router path) — consumes this binding for ordering-safe multi-VC.
- C: cosim wrapper (RouterChannelShellAdapter + tb_top).
