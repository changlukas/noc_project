# addr_trans + ROB Disabled + AXI4 Conformity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Relax Stage 2 `AxiMaster` to allow same-ID multi-outstanding per AXI4 IHI 0022 §A5.3 (per-id FIFO admission + ordered AW/W push + per-id FIFO B/R routing), add `nmu::addr_trans` XYRouting helper, add `nmu::Rob` per-AXI-ID Disabled-mode ordering filter as in-line Packetizer+Depacketizer layer, refactor `nmu::Packetize` to drop sticky setter API in favor of `push_*_with_meta`, and integrate Rob into the e2e loopback test with a new `multi_dst_stress.yaml` fixture exercising the ROB stall path.

**Architecture:** Multi-inheritance Rob class (`Packetizer + Depacketizer`) sits between `AxiSlavePort` and `nmu::Packetize`/`nmu::Depacketize`, using per-id deque state. `addr_trans::xy_route(addr)` is a stateless namespace function returning `{dst_id, local_addr}`. `Packetize` self-computes routing via `addr_trans` in its frozen interface path, and accepts full `AwHeaderMeta` (incl. future `rob_idx`) via `push_*_with_meta` from Rob. `AxiMaster`'s `active_*_ops_` switches from `std::map<id, Op>` to `std::map<id, std::deque<Op>>` with separate total-count counters; ordered same-id W/R request iteration breaks on first incomplete op.

**Tech Stack:** C++17, GoogleTest, CMake, Python 3 specgen (no specgen changes this round), Ninja (mingw64).

**Reference spec:** `docs/superpowers/specs/2026-06-02-addr-trans-rob-disabled-axi4-conformity-design.md` (commit `9f90483`).

**Drift gates** (every commit must pass):
```bash
cd specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
cd ../c_model && cmake --build build && ctest --test-dir build -j 1
```

**Coding style note:** C/C++ multi-line continuation uses **4-space indent** (per user feedback, recorded in memory). Apply to all new code.

---

## Phase A: Stage 2 AxiMaster AXI4 conformity

### Task 1: AxiMaster per-id FIFO + total-count counter + admission relaxation

**Files:**
- Modify: `c_model/include/axi/axi_master.hpp` (type change, counter additions, admission code)
- Modify: `c_model/tests/axi/test_axi_master.cpp` (new test exercises same-id concurrent)

**Goal:** Master can admit same-AXI-ID transactions concurrently. Outstanding-op storage groups by id (deque) with total counter for `max_outstanding` checks.

- [ ] **Step 1: Write failing test for same-id concurrent admission**

Append to `c_model/tests/axi/test_axi_master.cpp` (find existing class or add new test):
```cpp
// Two same-id writes; AxiMaster must admit both concurrently (was 1-at-a-time before fix).
TEST(AxiMaster, SameIdConcurrentAdmissionWithFifoOrderedB) {
    using namespace ni::cmodel::axi;
    // Use existing scenario_parser path; author small inline YAML fixture for this test
    auto yaml = ::testing::TempDir() + "/test_same_id_concurrent.yaml";
    std::ofstream(yaml) << R"yaml(
config:
  max_outstanding_write: 2
transactions:
  - { op: write, id: 0x05, addr: 0x100,   size: 5, len: 0, burst: INCR,
      data_file: /dev/null, strb_file: "" }
  - { op: write, id: 0x05, addr: 0x10100, size: 5, len: 0, burst: INCR,
      data_file: /dev/null, strb_file: "" }
)yaml";
    // Note: this test uses an AxiSlave stub; if scenario_parser can't accept /dev/null
    // data, substitute with a real small data file in temp dir.
    Memory memory(0, 0x20000, 0, 0);
    AxiSlave slave(memory);
    AxiMaster master(yaml, slave, ::testing::TempDir() + "/dump.txt",
        /*max_out_w=*/2, /*max_out_r=*/2);
    // Tick a few cycles
    for (int i = 0; i < 100 && !master.done(); ++i) {
        master.tick();
        slave.tick();
        memory.tick();
    }
    EXPECT_TRUE(master.done());
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R SameIdConcurrentAdmission -j 1
```
Expected: FAIL (current AxiMaster blocks same-id at admission so 2nd txn never issued; either watchdog times out or never reaches done()).

- [ ] **Step 3: Change AxiMaster type declarations**

In `c_model/include/axi/axi_master.hpp`, locate private members:
```cpp
std::map<uint8_t, OperationContext> active_write_ops_;
std::map<uint8_t, OperationContext> active_read_ops_;
```

Replace with:
```cpp
// Per-AXI-ID FIFO of outstanding ops (post AXI4 conformity fix — same-id concurrent allowed).
// AXI4 IHI 0022 §A5.3: responses for same id complete in submission order;
// per-id deque preserves submission order; B/R routing uses .front().
std::map<uint8_t, std::deque<OperationContext>> active_write_ops_;
std::map<uint8_t, std::deque<OperationContext>> active_read_ops_;

// Total active op counters (map.size() now counts active *ids*, not ops).
std::size_t active_write_count_ = 0;
std::size_t active_read_count_  = 0;
```

- [ ] **Step 4: Change admission code**

Find the admission loop:
```cpp
while (next_txn_idx_ < sc_.transactions.size()) {
    const auto& txn = sc_.transactions[next_txn_idx_];
    if (txn.op == ScenarioTransaction::Op::Write) {
        if (active_write_ops_.size() >= max_out_w_) break;
        if (active_write_ops_.count(txn.id)) break;       // ← DELETE this line
        // ... build op + active_write_ops_.emplace(txn.id, std::move(op));
    } else {
        if (active_read_ops_.size() >= max_out_r_) break;
        if (active_read_ops_.count(txn.id)) break;        // ← DELETE this line
        // ... build op + active_read_ops_.emplace(txn.id, std::move(op));
    }
    ++next_txn_idx_;
}
```

Replace with:
```cpp
while (next_txn_idx_ < sc_.transactions.size()) {
    const auto& txn = sc_.transactions[next_txn_idx_];
    if (txn.op == ScenarioTransaction::Op::Write) {
        if (active_write_count_ >= max_out_w_) break;
        OperationContext op;
        op.src_txn = txn;
        op.sub_bursts = split_into_sub_bursts(txn);
        op.data = load_write_data_(txn.data_file,
            static_cast<std::size_t>(txn.len + 1u) * (1ull << txn.size));
        op.strb_per_beat = load_strb_file_(txn.strb_file,
            static_cast<std::size_t>(txn.len + 1u));
        active_write_ops_[txn.id].push_back(std::move(op));
        active_write_count_++;
    } else {
        if (active_read_count_ >= max_out_r_) break;
        OperationContext op;
        op.src_txn = txn;
        op.sub_bursts = split_into_sub_bursts(txn);
        active_read_ops_[txn.id].push_back(std::move(op));
        active_read_count_++;
    }
    ++next_txn_idx_;
}
```

- [ ] **Step 5: Build — expect compile errors in B/R handling + push loops (those addressed in Task 2/3)**

```bash
cd c_model && cmake --build build 2>&1 | head -30
```
Expected: compile errors mentioning `active_write_ops_[...]` used in B handling expecting `OperationContext`, not deque. Task 2 + 3 fix these. STOP here; commit will happen at end of Task 3 when build is green.

- [ ] **Step 6: Don't commit yet** — wait until Task 3 completes (Task 1+2+3 are atomic for AxiMaster compilation)

---

### Task 2: AxiMaster ordered same-id W/R request push (`push_writes_`/`push_reads_` bool return)

**Files:**
- Modify: `c_model/include/axi/axi_master.hpp` (push_writes_/push_reads_ signature + outer iteration)

**Goal:** Within a per-id FIFO, walk ops in submission order; break on first op whose request phase (AW + all W beats, or AR for read) isn't fully pushed. Prevents AXI4 W stream ordering violations.

- [ ] **Step 1: Add `write_request_done` / `read_request_done` helpers**

Add to `OperationContext` struct (or as free helpers operating on it):
```cpp
struct OperationContext {
    // ... existing fields ...

    bool write_request_done() const {
        // All sub-bursts' AWs pushed AND all W beats for all sub-bursts pushed
        return next_aw_sub_idx_ == sub_bursts.size()
            && cur_w_sub_idx_ == sub_bursts.size();
    }
    bool read_request_done() const {
        return next_ar_sub_idx_ == sub_bursts.size();
    }
};
```

- [ ] **Step 2: Change `push_writes_` signature to `bool`**

Find:
```cpp
void push_writes_(uint8_t id, OperationContext& op) {
    // ... existing logic ...
}
```

Change to:
```cpp
// Returns true if op's write request phase (all AWs + all W beats across sub-bursts)
// is fully pushed downstream. Outer iteration breaks on first false to preserve
// AXI4 W stream ordering: same-id ops MUST emit their W phase in AW issue order.
bool push_writes_(uint8_t id, OperationContext& op) {
    // ... existing body unchanged in logic ...
    // ... at end of function:
    return op.write_request_done();
}
```

If existing function has early `return;` statements, replace each with `return op.write_request_done();`.

- [ ] **Step 3: Change `push_reads_` signature to `bool`**

If `push_reads_` exists as a function (or inline in the read-side admit loop), same pattern:
```cpp
bool push_reads_(uint8_t id, OperationContext& op) {
    // ... walk sub-bursts, push AR for each via slave_.push_ar(ar) ...
    return op.read_request_done();
}
```

If currently the read-side push is inline in `tick()`, extract it to a helper for symmetry with `push_writes_`.

- [ ] **Step 4: Change outer iteration in `tick()`**

Find:
```cpp
for (auto& [id, op] : active_write_ops_) {
    push_writes_(id, op);
}
```

Replace with:
```cpp
for (auto& [id, deq] : active_write_ops_) {
    for (auto& op : deq) {
        if (!push_writes_(id, op)) break;  // FIFO ordering: don't skip ahead
    }
}
```

Same for read side:
```cpp
for (auto& [id, deq] : active_read_ops_) {
    for (auto& op : deq) {
        if (!push_reads_(id, op)) break;
    }
}
```

- [ ] **Step 5: Build — expect remaining errors in B/R handling (Task 3 closes those)**

```bash
cd c_model && cmake --build build 2>&1 | head -20
```
Expected: errors only on B/R handling paths. STOP, no commit yet.

---

### Task 3: AxiMaster B/R routing via FIFO front + protocol_rules template helpers

**Files:**
- Modify: `c_model/include/axi/axi_master.hpp` (B/R handling)
- Modify: `c_model/include/axi/protocol_rules.hpp` (add 2 template helpers)
- Modify: `c_model/tests/axi/test_protocol_rules.cpp` (add 2 EXPECT_DEATH tests)

**Goal:** B/R responses route via per-id FIFO front (oldest outstanding for that id). Helpers validate the FIFO invariant.

- [ ] **Step 1: Write failing tests for new protocol helpers**

Append to `c_model/tests/axi/test_protocol_rules.cpp`:
```cpp
#include <deque>
#include <map>

// Minimal OpType stub matching what AxiMasterT::OperationContext exposes
// to the template helpers (b_count_, sub_bursts.size(), cur_r_sub_idx_,
// r_beats_in_cur_, sub_bursts[i].len). Use in isolation here for protocol-rule
// validation; the real AxiMasterT::OperationContext also satisfies the contract.
struct FakeOp {
    std::size_t b_count_ = 0;
    std::vector<struct { std::size_t len; }> sub_bursts;
    std::size_t cur_r_sub_idx_ = 0;
    std::size_t r_beats_in_cur_ = 0;
};

TEST(ProtocolRulesDeath, CheckBFrontNoOutstandingOrFullyResponded_Rejects) {
    using ni::cmodel::axi::rules::check_b_front_can_accept_response;
    std::map<uint8_t, std::deque<FakeOp>> m;
    // Case 1: empty deque
    EXPECT_FALSE(check_b_front_can_accept_response(/*bid=*/0x05, m));
    // Case 2: fully-responded front op
    FakeOp op;
    op.sub_bursts.resize(2);
    op.b_count_ = 2;  // already received all B responses
    m[0x05].push_back(op);
    EXPECT_FALSE(check_b_front_can_accept_response(0x05, m));
}

TEST(ProtocolRulesDeath, CheckRFrontBadBeatTimingOrRlast_Rejects) {
    using ni::cmodel::axi::rules::check_r_front_can_accept_beat;
    std::map<uint8_t, std::deque<FakeOp>> m;
    // Case 1: empty deque
    EXPECT_FALSE(check_r_front_can_accept_beat(/*rid=*/0x05, /*rlast=*/false, m));
    // Case 2: rlast=true on intermediate beat
    FakeOp op;
    op.sub_bursts.push_back({/*len=*/3});  // 4-beat burst
    op.r_beats_in_cur_ = 1;
    m[0x05].push_back(op);
    EXPECT_FALSE(check_r_front_can_accept_beat(0x05, /*rlast=*/true, m));
    // Case 3: rlast=false on final beat
    op.r_beats_in_cur_ = 3;
    m[0x05].clear();
    m[0x05].push_back(op);
    EXPECT_FALSE(check_r_front_can_accept_beat(0x05, /*rlast=*/false, m));
}
```

- [ ] **Step 2: Run to verify FAIL (helpers don't exist yet)**

```bash
cd c_model && cmake --build build 2>&1 | grep -E "(error|check_b_front|check_r_front)" | head -10
```
Expected: compile errors about undeclared `check_b_front_can_accept_response` / `check_r_front_can_accept_beat`.

- [ ] **Step 3: Add template helpers to `protocol_rules.hpp`**

In `c_model/include/axi/protocol_rules.hpp`, inside `namespace ni::cmodel::axi::rules`, add:
```cpp
#include <deque>
#include <map>

// AXI4 IHI 0022 §A5.3: response for an AXI ID routes to the oldest outstanding
// operation for that id (per-id FIFO). This helper verifies the FIFO invariant
// before B routing: deque non-empty AND front op has not yet received all B
// responses (i.e., b_count_ < sub_bursts.size()).
//
// OpType is structurally typed (avoids leaking AxiMasterT's private nested
// OperationContext into this header). Caller must supply a type with members
// `b_count_` (integral) and `sub_bursts` (sized container).
template<typename OpType>
inline bool check_b_front_can_accept_response(
    uint8_t bid,
    const std::map<uint8_t, std::deque<OpType>>& active_write_ops) {
    auto it = active_write_ops.find(bid);
    if (it == active_write_ops.end() || it->second.empty()) return false;
    const auto& op = it->second.front();
    return op.b_count_ < op.sub_bursts.size();
}

// Read mirror with multi-beat semantics. R(last) must occur iff the current
// beat is the final beat of the current sub-burst (r_beats_in_cur_ == sub.len).
// OpType structural requirements: `cur_r_sub_idx_`, `r_beats_in_cur_`,
// `sub_bursts[i].len`.
template<typename OpType>
inline bool check_r_front_can_accept_beat(
    uint8_t rid, bool rlast,
    const std::map<uint8_t, std::deque<OpType>>& active_read_ops) {
    auto it = active_read_ops.find(rid);
    if (it == active_read_ops.end() || it->second.empty()) return false;
    const auto& op = it->second.front();
    if (op.cur_r_sub_idx_ >= op.sub_bursts.size()) return false;
    const auto& sub = op.sub_bursts[op.cur_r_sub_idx_];
    if (op.r_beats_in_cur_ > sub.len) return false;
    return rlast == (op.r_beats_in_cur_ == sub.len);
}
```

- [ ] **Step 4: Update AxiMaster B handling to use new helper + FIFO front routing**

In `c_model/include/axi/axi_master.hpp`, find the B drain block:
```cpp
while (auto b = slave_.pop_b()) {
    AXI_PROTOCOL_ASSERT(rules::check_resp_encoding(b->resp), ...);
    AXI_PROTOCOL_ASSERT(rules::check_b_id_match_outstanding(b->id, active_write_ops_), ...);
    auto it = active_write_ops_.find(b->id);
    if (it == active_write_ops_.end()) continue;
    auto& op = it->second;
    // ... b_count_ logic, op.worst_resp_, on complete: callback + erase ...
}
```

Replace with:
```cpp
while (auto b = slave_.pop_b()) {
    AXI_PROTOCOL_ASSERT(rules::check_resp_encoding(b->resp),
        "RESP_ENCODING: B response must be a legal AXI4 response");
    AXI_PROTOCOL_ASSERT(rules::check_b_front_can_accept_response(b->id, active_write_ops_),
        "B_FRONT_CAN_ACCEPT: id deque empty or front op already fully responded");
    auto& deq = active_write_ops_[b->id];
    auto& op = deq.front();
    op.b_count_++;
    if (static_cast<uint8_t>(b->resp) > static_cast<uint8_t>(op.worst_resp_))
        op.worst_resp_ = b->resp;
    if (op.b_count_ == op.sub_bursts.size()) {
        if (wcb_) wcb_(WriteResult{op.src_txn.addr, op.src_txn.size,
            op.src_txn.len, op.src_txn.burst, op.src_txn.lock,
            op.data, op.strb_per_beat, op.worst_resp_, b->id,
            op.src_txn.scenario_line});
        deq.pop_front();
        active_write_count_--;
        if (deq.empty()) active_write_ops_.erase(b->id);
    }
}
```

- [ ] **Step 5: Update AxiMaster R handling similarly**

Find R drain block:
```cpp
while (auto r = slave_.pop_r()) {
    auto it = active_read_ops_.find(r->id);
    if (it == active_read_ops_.end()) continue;
    auto& op = it->second;
    // ... r_beats_in_cur_ logic, on r->last advance sub-burst, on complete: callback + erase ...
}
```

Replace with:
```cpp
while (auto r = slave_.pop_r()) {
    AXI_PROTOCOL_ASSERT(rules::check_r_front_can_accept_beat(r->id, r->last, active_read_ops_),
        "R_FRONT_CAN_ACCEPT: bad beat timing or rlast mismatch with sub-burst length");
    auto& deq = active_read_ops_[r->id];
    auto& op = deq.front();
    const auto& sub = op.sub_bursts[op.cur_r_sub_idx_];
    const std::size_t bpb = 1ull << sub.size;
    const uint64_t beat_addr_v = beat_addr(sub.addr, sub.len, sub.size, sub.burst,
        op.r_beats_in_cur_);
    const std::size_t byte_lane =
        static_cast<std::size_t>(beat_addr_v & (DATA_BYTES - 1));
    const std::size_t lane_room = (byte_lane < DATA_BYTES) ? (DATA_BYTES - byte_lane) : 0;
    const std::size_t copy_bytes = std::min(bpb, lane_room);
    for (std::size_t i = 0; i < copy_bytes; ++i)
        op.read_accumulator.push_back(r->data[byte_lane + i]);
    for (std::size_t i = copy_bytes; i < bpb; ++i)
        op.read_accumulator.push_back(0);
    ++op.r_beats_in_cur_;
    if (static_cast<uint8_t>(r->resp) > static_cast<uint8_t>(op.worst_resp_))
        op.worst_resp_ = r->resp;
    if (r->last) {
        ++op.cur_r_sub_idx_;
        op.r_beats_in_cur_ = 0;
    }
    if (op.cur_r_sub_idx_ == op.sub_bursts.size()) {
        // ... existing read_dump + rcb_ callback logic ...
        if (rcb_) rcb_(ReadResult{op.src_txn.addr, op.src_txn.size,
            op.src_txn.len, op.src_txn.burst, op.read_accumulator,
            op.worst_resp_, r->id, op.src_txn.scenario_line});
        deq.pop_front();
        active_read_count_--;
        if (deq.empty()) active_read_ops_.erase(r->id);
    }
}
```

- [ ] **Step 6: Audit existing AxiMaster `done()` method**

`done()` likely uses `active_write_ops_.empty() && active_read_ops_.empty()`. With per-id deque + counter, the check should use counters:
```cpp
bool done() const {
    return next_txn_idx_ >= sc_.transactions.size()
        && active_write_count_ == 0 && active_read_count_ == 0;
}
```

- [ ] **Step 7: Build + run new protocol_rules tests**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R "CheckBFront|CheckRFront" -j 1
```
Expected: 2 new EXPECT_DEATH tests pass.

- [ ] **Step 8: Build + run new same-id concurrent test (from Task 1 step 1)**

```bash
ctest --test-dir build -R SameIdConcurrentAdmission -j 1
```
Expected: PASS.

- [ ] **Step 9: Full ctest sweep**

```bash
ctest --test-dir build -j 1 2>&1 | tail -30
```
Expected: most tests PASS. Some Stage 2 `test_axi_master.cpp` tests may FAIL because they implicitly assumed `active_write_ops_` was `map<id, Op>` (e.g., accessing `[id]` directly without `front()`). Record which tests fail — Task 4 fixes them.

- [ ] **Step 10: Commit Tasks 1+2+3 atomically**

```bash
git add c_model/include/axi/axi_master.hpp \
        c_model/include/axi/protocol_rules.hpp \
        c_model/tests/axi/test_protocol_rules.cpp \
        c_model/tests/axi/test_axi_master.cpp
git commit -m "feat(axi): AXI4 conformity — same-id concurrent admission + per-id FIFO B/R routing

- AxiMaster active_*_ops_ types changed from map<id, Op> to map<id, deque<Op>>
- active_*_count_ counters added for total max_outstanding checks
- Admission code drops same-id concurrent block (was AXI4 IHI 0022 §A5.3 violation)
- push_writes_/push_reads_ return bool; outer FIFO loop breaks on first incomplete op
  (preserves AXI4 W stream ordering for same-id concurrent ops)
- B/R handling routes via per-id FIFO front; pop_front + counter-- on complete
- protocol_rules.hpp: 2 new template helpers (check_b_front_can_accept_response,
  check_r_front_can_accept_beat) with templated OpType to avoid private nested
  type dependency
- 2 new EXPECT_DEATH tests for the helpers
- 1 new same-id concurrent admission regression test"
```

---

### Task 4: Update Stage 2 AxiMaster tests affected by relaxation

**Files:**
- Modify: `c_model/tests/axi/test_axi_master.cpp` (5-8 affected tests)
- Possibly modify: `c_model/tests/axi/test_integration.cpp` (if any fixture-driven tests assume same-id 1-outstanding)

**Goal:** Existing tests that implicitly depended on same-id 1-outstanding behavior must be updated. The set is unknown until Task 3 ctest run reveals failures.

- [ ] **Step 1: Run full ctest, capture failures**

```bash
cd c_model && ctest --test-dir build -j 1 2>&1 | grep -E "(FAIL|Failed)" | head -20
```
Record the failing test names.

- [ ] **Step 2: Investigate each failing test**

For each failing test, read the test body and identify:
- Is it directly testing the now-deleted behavior (same-id concurrent block)? → Delete or rewrite to test new behavior.
- Is it indirectly affected (e.g., accessing `active_write_ops_[id]` as if it were `Op` not `deque<Op>`)? → Update access pattern.
- Is it a fixture-driven test where outcome depended on serial same-id issue? → Verify expected outcome still holds with concurrent issue (likely yes if scoreboard checks data integrity, not timing).

- [ ] **Step 3: Apply fixes (per-test, individually)**

Common patterns:
```cpp
// Before:
EXPECT_EQ(master.active_write_ops_.count(id), 1u);
// After:
EXPECT_EQ(master.active_write_ops_[id].size(), 1u);
// or simply:
EXPECT_TRUE(master.active_write_ops_.find(id) != master.active_write_ops_.end());
```

```cpp
// Before:
auto& op = master.active_write_ops_[id];
EXPECT_EQ(op.b_count_, 1u);
// After:
auto& op = master.active_write_ops_[id].front();
EXPECT_EQ(op.b_count_, 1u);
```

**Note:** if `active_write_ops_` is private and accessed via friend/test-only API, that API may need updating too.

- [ ] **Step 4: Run individual fixed tests to verify pass**

```bash
ctest --test-dir build -R <test_name_pattern> -j 1
```

- [ ] **Step 5: Full ctest sweep — confirm clean**

```bash
ctest --test-dir build -j 1
```
Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add c_model/tests/axi/test_axi_master.cpp c_model/tests/axi/test_integration.cpp
git commit -m "fix(tests/axi): update AxiMaster tests for per-id FIFO relaxation

Tests that implicitly assumed same-id 1-outstanding behavior updated to
use per-id deque access patterns (.front() instead of bare [id], .size()
instead of .count() for op count). Affected tests: [list specific ones]."
```

---

## Phase B: addr_trans + Packetize refactor

### Task 5: `nmu::addr_trans` helper + 3 unit tests

**Files:**
- Create: `c_model/include/nmu/addr_trans.hpp`
- Create: `c_model/tests/nmu/test_addr_trans.cpp`
- Modify: `c_model/tests/nmu/CMakeLists.txt`

**Goal:** Stateless pure XYRouting function returning `{dst_id, local_addr}`.

- [ ] **Step 1: Write failing tests**

Create `c_model/tests/nmu/test_addr_trans.cpp`:
```cpp
#include "nmu/addr_trans.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nmu::addr_trans::xy_route;
using ni::cmodel::nmu::addr_trans::Translated;

TEST(AddrTrans, XyRoute_LowBitsAreLocalAddr) {
    auto t = xy_route(0x1234);
    EXPECT_EQ(t.dst_id, 0x00u);
    EXPECT_EQ(t.local_addr, 0x1234u);
}

TEST(AddrTrans, XyRoute_HighBitsDecodeXY) {
    // addr[19:16]=0xF (x), addr[23:20]=0xF (y) → dst_id = (0xF << 4) | 0xF = 0xFF
    auto t = xy_route(0x00FF0000);
    EXPECT_EQ(t.dst_id, 0xFFu);
    EXPECT_EQ(t.local_addr, 0x00FF0000ull);
}

TEST(AddrTrans, XyRoute_LocalAddrPassesThroughFullWidth) {
    auto t = xy_route(0xABCDEF1234567890ull);
    EXPECT_EQ(t.local_addr, 0xABCDEF1234567890ull);
    // dst_id = addr[23:16] = 0x56
    EXPECT_EQ(t.dst_id, 0x56u);
}
```

- [ ] **Step 2: Add to nmu CMakeLists**

In `c_model/tests/nmu/CMakeLists.txt`, append:
```cmake
add_cmodel_test(test_addr_trans)
```

- [ ] **Step 3: Run to verify FAIL**

```bash
cd c_model && cmake --build build 2>&1 | head -10
```
Expected: build error `nmu/addr_trans.hpp: No such file`.

- [ ] **Step 4: Implement `addr_trans.hpp`**

Create `c_model/include/nmu/addr_trans.hpp`:
```cpp
#pragma once
#include "ni_flit_constants.h"  // for ni::width::X_WIDTH / Y_WIDTH (documentation)
#include <cstdint>

namespace ni::cmodel::nmu::addr_trans {

struct Translated {
    uint8_t  dst_id;     // X_WIDTH + Y_WIDTH = 8 bits per ni_packet.json
    uint64_t local_addr; // for c_model = addr (no remap)
};

// XYRouting bit allocation (c_model policy — will move when SAM table or
// remap added; per spec §4.3):
//   addr[15:0]  = local address (64 KB per dst)
//   addr[19:16] = x (low 4 bits of dst_id)
//   addr[23:20] = y (high 4 bits of dst_id)
//   addr[63:24] = unused (zero in current test fixtures)
//
// local_addr is unmodified — XYRouting only extracts dst_id; address space
// is global for c_model. Future remap (NSU subtracts base address) may set
// local_addr = addr - base.
inline Translated xy_route(uint64_t addr) noexcept {
    constexpr uint64_t LOCAL_ADDR_BITS = 16;
    uint8_t dst = static_cast<uint8_t>((addr >> LOCAL_ADDR_BITS) & 0xFF);
    return { dst, /*local_addr=*/addr };
}

}  // namespace ni::cmodel::nmu::addr_trans
```

- [ ] **Step 5: Build + run tests**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R AddrTrans -j 1
```
Expected: 3 tests PASS.

- [ ] **Step 6: Full ctest sweep**

```bash
ctest --test-dir build -j 1
```
Expected: prior count + 3 (3 addr_trans tests added), all PASS.

- [ ] **Step 7: Commit**

```bash
git add c_model/include/nmu/addr_trans.hpp c_model/tests/nmu/test_addr_trans.cpp c_model/tests/nmu/CMakeLists.txt
git commit -m "feat(c_model/nmu): add addr_trans XYRouting helper (pure namespace function)

addr_trans::xy_route(addr) returns {dst_id, local_addr}. Bit allocation
(c_model policy): addr[15:0]=local, addr[19:16]=x, addr[23:20]=y. No remap
in c_model; local_addr passes through unchanged."
```

---

### Task 6: `nmu::Packetize` refactor — drop sticky setter, add `push_*_with_meta`

**Files:**
- Modify: `c_model/include/nmu/packetize.hpp`
- Modify: `c_model/tests/nmu/test_packetize.cpp` (delete 4 sticky death tests, update 8, add 2)

**Goal:** Packetize self-computes `dst_id` via `addr_trans` for the frozen Packetizer interface path; accepts full `AwHeaderMeta` (with future `rob_idx`) via new non-interface `push_*_with_meta` methods.

- [ ] **Step 1: Update `test_packetize.cpp` — delete 4 sticky death tests**

Find and DELETE these 4 test functions in `c_model/tests/nmu/test_packetize.cpp`:
```
StickySetterAssertMissingSet
StickySetterAssertDoubleSet
StickySetterArMissingSet
StickySetterArDoubleSet
```

Also remove any `make_aw_with_sticky` helper or `pkt.set_aw_header_extras(...)` calls from other tests — replace with direct `pkt.push_aw(beat)` (Packetize will internally compute dst).

For tests that exercise CUSTOM dst (where the test deliberately wants a non-addr-derived dst), refactor to call new `push_aw_with_meta` directly.

- [ ] **Step 2: Add 2 new tests to `test_packetize.cpp`**

Append:
```cpp
TEST(NmuPacketize, PushAwWithMeta_OverrideDefault) {
    LoopbackNoc noc(/*req*/16, /*rsp*/16);
    Packetize pkt(noc.req_out(), /*src=*/0x01);
    axi::AwBeat b = make_aw(/*id=*/0x05, /*addr=*/0x100);  // addr → dst=0 by default
    ni::cmodel::nmu::AwHeaderMeta meta{
        /*dst_id=*/0x42,
        /*local_addr=*/0x9999,
        /*rob_req=*/1,
        /*rob_idx=*/0x07
    };
    ASSERT_TRUE(pkt.push_aw_with_meta(b, meta));
    auto f = *noc.req_in().pop_flit();
    EXPECT_EQ(f.get_header_field("dst_id"),  0x42u);
    EXPECT_EQ(f.get_header_field("rob_req"), 1u);
    EXPECT_EQ(f.get_header_field("rob_idx"), 0x07u);
    EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0x9999u);  // meta.local_addr, NOT b.addr
}

TEST(NmuPacketize, AddrTransIntegratedDstIdInHeader) {
    LoopbackNoc noc(16, 16);
    Packetize pkt(noc.req_out(), /*src=*/0x01);
    // addr 0x10100 → addr_trans gives dst=1
    axi::AwBeat b = make_aw(/*id=*/0x05, /*addr=*/0x10100);
    ASSERT_TRUE(pkt.push_aw(b));  // frozen interface auto-computes
    auto f = *noc.req_in().pop_flit();
    EXPECT_EQ(f.get_header_field("dst_id"), 0x01u);  // from addr_trans::xy_route
    EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0x10100u);  // local_addr = addr
}
```

- [ ] **Step 3: Run to verify FAIL (new methods + struct don't exist)**

```bash
cd c_model && cmake --build build 2>&1 | head -10
```
Expected: errors about `AwHeaderMeta`, `push_aw_with_meta`.

- [ ] **Step 4: Refactor `packetize.hpp`**

Replace the entire `nmu::Packetize` class with the refactored version. First, REMOVE:
```cpp
// Delete:
void set_aw_header_extras(uint8_t dst_id, uint8_t rob_req = 0, uint8_t rob_idx = 0);
void set_ar_header_extras(uint8_t dst_id, uint8_t rob_req = 0, uint8_t rob_idx = 0);
// Delete private state:
bool aw_extras_pending_ = false;
bool ar_extras_pending_ = false;
uint8_t aw_dst_id_ = 0, aw_rob_req_ = 0, aw_rob_idx_ = 0;
uint8_t ar_dst_id_ = 0, ar_rob_req_ = 0, ar_rob_idx_ = 0;
```

Then ADD at top of file (after includes):
```cpp
#include "nmu/addr_trans.hpp"

namespace ni::cmodel::nmu {

// Per-AW metadata for header stamping. Used by both the frozen Packetizer
// interface (auto-filled from addr_trans + rob_*=0) and the explicit
// push_aw_with_meta path (called by Rob with full metadata).
struct AwHeaderMeta {
    uint8_t  dst_id;     // from addr_trans
    uint64_t local_addr; // from addr_trans (= awaddr in c_model)
    uint8_t  rob_req;    // 0 in Disabled mode, 1 in Enabled mode
    uint8_t  rob_idx;    // 0 in Disabled, allocated in Enabled
};
}  // namespace ni::cmodel::nmu
```

Refactor `push_aw` override to delegate to new `push_aw_with_meta`:
```cpp
bool push_aw(const axi::AwBeat& b) override {
    auto t = addr_trans::xy_route(b.addr);
    return push_aw_with_meta(b, {t.dst_id, t.local_addr, 0, 0});
}
bool push_ar(const axi::ArBeat& b) override {
    auto t = addr_trans::xy_route(b.addr);
    return push_ar_with_meta(b, {t.dst_id, t.local_addr, 0, 0});
}
```

Add new public methods:
```cpp
// Non-interface, called by Rob with full metadata (future Enabled mode
// supplies rob_idx via this path).
bool push_aw_with_meta(const axi::AwBeat& b, AwHeaderMeta meta);
bool push_ar_with_meta(const axi::ArBeat& b, AwHeaderMeta meta);
```

Update WriteMeta struct (keep simple — W has no addr per AXI4):
```cpp
// W FIFO carries the meta inherited from AW. local_addr NOT stored:
// W payload has no address field; only header dst_id/rob_* needed.
struct WMeta { uint8_t dst_id; uint8_t rob_req; uint8_t rob_idx; };
std::deque<WMeta> w_meta_fifo_;
```

Add implementations:
```cpp
inline bool Packetize::push_aw_with_meta(const axi::AwBeat& b, AwHeaderMeta meta) {
    Flit f;
    f.set_header_field("axi_ch",  ni::AXI_CH_AW);
    f.set_header_field("src_id",  src_id_);
    f.set_header_field("dst_id",  meta.dst_id);
    f.set_header_field("vc_id",   0);
    f.set_header_field("last",    1);
    f.set_header_field("rob_req", meta.rob_req);
    f.set_header_field("rob_idx", meta.rob_idx);
    f.set_payload_field("AW", "awid",     b.id);
    f.set_payload_field("AW", "awaddr",   meta.local_addr);  // NOT b.addr (future remap-safe)
    f.set_payload_field("AW", "awlen",    b.len);
    f.set_payload_field("AW", "awsize",   b.size);
    f.set_payload_field("AW", "awburst",  static_cast<uint64_t>(b.burst));
    f.set_payload_field("AW", "awcache",  b.cache);
    f.set_payload_field("AW", "awlock",   b.lock);
    f.set_payload_field("AW", "awprot",   b.prot);
    f.set_payload_field("AW", "awregion", b.region);
    f.set_payload_field("AW", "awuser",   b.user);
    if (!req_out_.push_flit(f)) return false;
    w_meta_fifo_.push_back({meta.dst_id, meta.rob_req, meta.rob_idx});
    return true;
}

inline bool Packetize::push_ar_with_meta(const axi::ArBeat& b, AwHeaderMeta meta) {
    Flit f;
    f.set_header_field("axi_ch",  ni::AXI_CH_AR);
    f.set_header_field("src_id",  src_id_);
    f.set_header_field("dst_id",  meta.dst_id);
    f.set_header_field("vc_id",   0);
    f.set_header_field("last",    1);
    f.set_header_field("rob_req", meta.rob_req);
    f.set_header_field("rob_idx", meta.rob_idx);
    f.set_payload_field("AR", "arid",     b.id);
    f.set_payload_field("AR", "araddr",   meta.local_addr);
    f.set_payload_field("AR", "arlen",    b.len);
    f.set_payload_field("AR", "arsize",   b.size);
    f.set_payload_field("AR", "arburst",  static_cast<uint64_t>(b.burst));
    f.set_payload_field("AR", "arcache",  b.cache);
    f.set_payload_field("AR", "arlock",   b.lock);
    f.set_payload_field("AR", "arprot",   b.prot);
    f.set_payload_field("AR", "arregion", b.region);
    f.set_payload_field("AR", "aruser",   b.user);
    if (!req_out_.push_flit(f)) return false;
    return true;
}
```

Update old `push_aw` body that previously had sticky setter assertion:
```cpp
// DELETE the old inline body that did:
//   assert(aw_extras_pending_ && "set_aw_header_extras must be called");
//   ... build flit using aw_dst_id_/aw_rob_*_ ...
// Now push_aw delegates to push_aw_with_meta (defined as inline override above).
```

For `push_w` (unchanged — reads `w_meta_fifo_` for inherited meta):
```cpp
inline bool Packetize::push_w(const axi::WBeat& b) {
    assert(!w_meta_fifo_.empty() && "push_w called before any push_aw");
    const auto& meta = w_meta_fifo_.front();
    Flit f;
    f.set_header_field("axi_ch",  ni::AXI_CH_W);
    f.set_header_field("src_id",  src_id_);
    f.set_header_field("dst_id",  meta.dst_id);
    f.set_header_field("vc_id",   0);
    f.set_header_field("last",    1);
    f.set_header_field("rob_req", meta.rob_req);
    f.set_header_field("rob_idx", meta.rob_idx);
    f.set_payload_field("W", "wlast", b.last ? 1u : 0u);
    f.set_payload_field("W", "wuser", b.user);
    f.set_payload_field("W", "wstrb", b.strb);
    f.set_payload_bytes("W", "wdata", b.data.data(), 256);
    if (!req_out_.push_flit(f)) return false;
    if (b.last) w_meta_fifo_.pop_front();
    return true;
}
```

- [ ] **Step 5: Build + run packetize tests**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R NmuPacketize -j 1
```
Expected: ~10 tests PASS (8 unchanged + 2 new; 4 sticky death deleted).

- [ ] **Step 6: Full ctest sweep**

```bash
ctest --test-dir build -j 1
```
Expected: all PASS. NB: the existing integration test (`test_request_response_loopback.cpp`) still uses `TestPacketize` adapter; it should continue to work because `TestPacketize.push_aw` calls `real_.set_aw_header_extras(...)` then `real_.push_aw(b)` — but those calls no longer exist on `nmu::Packetize`. **THIS WILL BREAK INTEGRATION TEST.** Task 7+8 fix.

If you see integration test FAIL here, that's expected — don't commit. Continue to Task 7.

If you need to commit Task 6 separately before Task 7, temporarily comment out TestPacketize wiring OR delete `test_packetize_adapter.hpp` now. Recommend: commit Task 5+6 atomically with a known-failing integration test, document in commit message; OR roll forward to Task 7 first.

Decision: commit Task 6 NOW, leave integration test temporarily broken (will fix in Task 8).

- [ ] **Step 7: Commit Task 6**

```bash
git add c_model/include/nmu/packetize.hpp c_model/tests/nmu/test_packetize.cpp
git commit -m "feat(c_model/nmu/packetize): drop sticky setter, add push_*_with_meta

Packetize self-computes dst via addr_trans::xy_route in frozen Packetizer
interface path. push_aw_with_meta / push_ar_with_meta accept full AwHeaderMeta
(dst_id, local_addr, rob_req, rob_idx) for Rob layer use; future Enabled
mode supplies rob_idx via this path. Payload uses meta.local_addr (not
beat.addr) for future remap safety.

NOTE: integration test temporarily broken (TestPacketize calls deleted
sticky-setter API). Task 7+8 wire Rob in pipeline and retire TestPacketize."
```

---

## Phase C: ROB Disabled + integration

### Task 7: `nmu::Rob` class + 10 unit tests

**Files:**
- Create: `c_model/include/nmu/rob.hpp`
- Create: `c_model/tests/nmu/test_rob.cpp`
- Modify: `c_model/tests/nmu/CMakeLists.txt`

**Goal:** In-line layer between AxiSlavePort and Packetize+Depacketize. Implements Packetizer+Depacketizer. Disabled mode: per-AXI-ID ordering filter with per-id deque + W burst credit gate. Enabled mode all paths assert+abort.

- [ ] **Step 1: Write failing tests (10 tests)**

Create `c_model/tests/nmu/test_rob.cpp`:
```cpp
#include "nmu/rob.hpp"
#include "nmu/packetize.hpp"
#include "nmu/depacketize.hpp"
#include "common/loopback_noc.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>

using ni::cmodel::testing::LoopbackNoc;
using ni::cmodel::nmu::Packetize;
using ni::cmodel::nmu::Depacketize;
using ni::cmodel::nmu::Rob;
using ni::cmodel::nmu::RobMode;
namespace axi = ni::cmodel::axi;

namespace {
constexpr uint8_t kSrcId = 0x01;

axi::AwBeat make_aw(uint8_t id, uint64_t addr, uint8_t len = 0) {
    axi::AwBeat b{};
    b.id = id; b.addr = addr; b.len = len; b.size = 5;
    b.burst = axi::Burst::INCR;
    return b;
}
axi::ArBeat make_ar(uint8_t id, uint64_t addr) {
    axi::ArBeat b{};
    b.id = id; b.addr = addr; b.size = 5;
    b.burst = axi::Burst::INCR;
    return b;
}
axi::WBeat make_w(bool last) {
    axi::WBeat b{};
    b.last = last;
    return b;
}
// Helper to construct + feed a B flit to nmu::Depacketize for pop_b to return
ni::cmodel::Flit make_b_flit(uint8_t bid) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("last",   1);
    f.set_payload_field("B", "bid",   bid);
    f.set_payload_field("B", "bresp", 0);
    return f;
}
ni::cmodel::Flit make_r_flit(uint8_t rid, bool rlast) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_R);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("last",   1);
    f.set_payload_field("R", "rid",   rid);
    f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
    return f;
}

// Test rig: Rob wraps Packetize + Depacketize over LoopbackNoc.
struct RobRig {
    LoopbackNoc noc{16, 16};
    Packetize   pkt{noc.req_out(), kSrcId};
    Depacketize depkt{noc.rsp_in(), 16, 16};
    Rob         rob{pkt, depkt, RobMode::Disabled, RobMode::Disabled};
};
}  // namespace

// === ROB-specific core behavior (4 tests) ===

TEST(NmuRob, Disabled_StallSameIdDiffDst) {
    RobRig r;
    // 1st AW: id=5, addr=0x100 → dst=0
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x100)));
    // 2nd AW: same id, addr=0x10100 → dst=1 → must stall
    EXPECT_FALSE(r.rob.push_aw(make_aw(0x05, 0x10100)));
}

TEST(NmuRob, Disabled_StallReleaseOnBComplete) {
    RobRig r;
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x100)));
    EXPECT_FALSE(r.rob.push_aw(make_aw(0x05, 0x10100)));
    // Inject a B flit for id=5 via rsp_in
    ASSERT_TRUE(r.noc.rsp_out().push_flit(make_b_flit(0x05)));
    r.depkt.tick();  // demux into B queue
    auto b = r.rob.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 0x05);
    // After pop_b, outstanding for id=5 is empty → next push_aw should pass
    EXPECT_TRUE(r.rob.push_aw(make_aw(0x05, 0x10100)));
}

TEST(NmuRob, Disabled_StallReleaseOnRlast) {
    RobRig r;
    ASSERT_TRUE(r.rob.push_ar(make_ar(0x05, 0x100)));
    EXPECT_FALSE(r.rob.push_ar(make_ar(0x05, 0x10100)));
    // Inject R(last=true)
    ASSERT_TRUE(r.noc.rsp_out().push_flit(make_r_flit(0x05, /*rlast=*/true)));
    r.depkt.tick();
    auto rb = r.rob.pop_r();
    ASSERT_TRUE(rb.has_value());
    EXPECT_TRUE(rb->last);
    EXPECT_TRUE(r.rob.push_ar(make_ar(0x05, 0x10100)));
}

TEST(NmuRob, Disabled_WCreditBlocksWBeforeAw) {
    RobRig r;
    // No push_aw yet → credit=0 → push_w must return false
    EXPECT_FALSE(r.rob.push_w(make_w(/*last=*/true)));
}

// === ROB invariants (3 tests) ===

TEST(NmuRob, Disabled_BackpressureAtomicityPushAw) {
    // Force downstream NoC full via small req_depth
    LoopbackNoc noc(/*req*/1, /*rsp*/16);
    Packetize pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Disabled, RobMode::Disabled);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));   // 1st fills req queue
    // 2nd push: should also stall on downstream backpressure, but ROB state must NOT mutate
    EXPECT_FALSE(rob.push_aw(make_aw(0x06, 0x200)));  // different id, no ROB stall; downstream full
    // Drain
    noc.req_in().pop_flit();
    // Retry now succeeds (state was atomic — second push didn't leave dangling outstanding)
    EXPECT_TRUE(rob.push_aw(make_aw(0x06, 0x200)));
}

TEST(NmuRob, Disabled_MultiOutstandingSameIdSameDst_NoFalseStall) {
    RobRig r;
    // 2 AWs same id, same dst (both addr in 0x100-0xFFFF range → dst=0)
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x100)));
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x200)));
    // No stall — same dst
}

TEST(NmuRob, Disabled_WCreditMultiOutstandingCorrectDecrement) {
    RobRig r;
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x100)));
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x200)));
    // credit=2; push wlast twice
    ASSERT_TRUE(r.rob.push_w(make_w(/*last=*/true)));  // credit-- to 1
    ASSERT_TRUE(r.rob.push_w(make_w(/*last=*/true)));  // credit-- to 0
    // Now credit=0 → next push_w must fail
    EXPECT_FALSE(r.rob.push_w(make_w(/*last=*/true)));
}

// === Edge cases (2 tests) ===

TEST(NmuRob, Disabled_WBackpressureDoesNotConsumeCredit) {
    // Trigger backpressure: small req_depth fills after AW + W beats
    LoopbackNoc noc(/*req*/2, /*rsp*/16);
    Packetize pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Disabled, RobMode::Disabled);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // AW flit (req queue 1/2)
    ASSERT_TRUE(rob.push_w(make_w(/*last=*/false)));  // W beat 1 (req queue 2/2 full)
    // credit was 1, still 1 (wlast=false didn't decrement)
    EXPECT_FALSE(rob.push_w(make_w(/*last=*/false)));  // downstream full → false; credit unchanged
    // Drain + retry — verify credit still 1, succeeds without becoming negative
    noc.req_in().pop_flit();  // drain AW
    EXPECT_TRUE(rob.push_w(make_w(/*last=*/true)));  // now succeeds, credit-- to 0
}

TEST(NmuRob, Disabled_DifferentIdsIndependentNoInterference) {
    RobRig r;
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x100)));
    EXPECT_FALSE(r.rob.push_aw(make_aw(0x05, 0x10100)));  // id=5 stalled
    // id=6 should be independent
    EXPECT_TRUE(r.rob.push_aw(make_aw(0x06, 0x100)));
}

// === Defensive (1 test) ===

TEST(NmuRobDeath, Disabled_AbortPaths) {
    RobRig r;
    EXPECT_DEATH(r.rob.push_b(axi::BBeat{}), "Rob: push_b");
    EXPECT_DEATH(r.rob.push_r(axi::RBeat{}), "Rob: push_r");
    EXPECT_DEATH(r.rob.pop_aw(), "Rob: pop_aw");
    EXPECT_DEATH(r.rob.pop_w(),  "Rob: pop_w");
    EXPECT_DEATH(r.rob.pop_ar(), "Rob: pop_ar");
}
```

- [ ] **Step 2: Add to nmu CMakeLists**

In `c_model/tests/nmu/CMakeLists.txt`, append:
```cmake
add_cmodel_test(test_rob)
```

- [ ] **Step 3: Run to verify FAIL**

```bash
cd c_model && cmake --build build 2>&1 | head -10
```
Expected: build error `nmu/rob.hpp: No such file`.

- [ ] **Step 4: Implement `nmu/rob.hpp`**

Create `c_model/include/nmu/rob.hpp`:
```cpp
#pragma once
#include "axi/types.hpp"
#include "ni/depacketizer.hpp"
#include "ni/packetizer.hpp"
#include "nmu/addr_trans.hpp"
#include "nmu/packetize.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>

namespace ni::cmodel::nmu {

enum class RobMode { Disabled, Enabled };  // Enabled = next round

// In-line layer between AxiSlavePort and {Packetize, Depacketize}.
// Implements both Packetizer (request gate) and Depacketizer (response observe).
//
// Disabled mode (this round): per-AXI-ID transaction ordering filter.
//   - Same id + same dst → pass; outstanding deque grows
//   - Same id + different dst → stall until deque drained
//   - Different id → independent
//   - Response B/R observe to pop_front per-id deque
//
// Enabled mode (next round): per-AXI-ID reorder buffer + rob_idx allocator.
//   This round leaves Enabled-mode method bodies as assert(false) + std::abort.
//
// Single-threaded tick model: state mutations from Packetizer-side (push_aw/ar)
// and Depacketizer-side (pop_b/r) happen in the same thread, no synchronization.
// Tick order (per AxiSlavePort): drain B/R before forwarding AW/W/AR → response
// frees IDs in same cycle, request can use freed IDs after.
//
// ROB Disabled mode ONLY handles same-id DIFFERENT-dst stall. Same-id same-dst
// response ordering is guaranteed by AxiMaster's per-id FIFO + NoC's per-pair
// deterministic routing (XYRouting satisfies). ROB Disabled does NOT add that
// ordering — implicit invariant.
class Rob : public Packetizer, public Depacketizer {
public:
    Rob(Packetize& next_pkt,
        Depacketizer& next_depkt,
        RobMode mode_w,
        RobMode mode_r)
        : next_pkt_(next_pkt), next_depkt_(next_depkt),
          mode_w_(mode_w), mode_r_(mode_r) {}

    // ===== Packetizer interface (request side; B/R assert+abort) =====
    bool push_aw(const axi::AwBeat& b) override;
    bool push_w (const axi::WBeat&  b) override;
    bool push_ar(const axi::ArBeat& b) override;
    bool push_b(const axi::BBeat&) override {
        assert(false && "Rob: push_b not applicable"); std::abort(); return false;
    }
    bool push_r(const axi::RBeat&) override {
        assert(false && "Rob: push_r not applicable"); std::abort(); return false;
    }

    // ===== Depacketizer interface (response side; AW/W/AR assert+abort) =====
    std::optional<axi::BBeat> pop_b() override;
    std::optional<axi::RBeat> pop_r() override;
    std::optional<axi::AwBeat> pop_aw() override {
        assert(false && "Rob: pop_aw not applicable"); std::abort(); return std::nullopt;
    }
    std::optional<axi::WBeat>  pop_w()  override {
        assert(false && "Rob: pop_w not applicable"); std::abort(); return std::nullopt;
    }
    std::optional<axi::ArBeat> pop_ar() override {
        assert(false && "Rob: pop_ar not applicable"); std::abort(); return std::nullopt;
    }

private:
    Packetize&     next_pkt_;
    Depacketizer&  next_depkt_;
    RobMode mode_w_, mode_r_;

    // Per-AXI-ID FIFO of outstanding entries. Disabled mode invariant:
    // for any non-empty deque, all entries share the same dst_id (gate enforces).
    // Forward-compat: OutstandingEntry adds rob_idx field for Enabled mode.
    struct OutstandingEntry {
        uint8_t dst_id;
        // future Enabled mode: uint8_t rob_idx;
    };
    struct WriteState { std::deque<OutstandingEntry> outstanding; };
    struct ReadState  { std::deque<OutstandingEntry> outstanding; };
    std::array<WriteState, 256> write_;
    std::array<ReadState,  256> read_;

    // W burst credit gate: prevents W beats from reaching Packetize before
    // their corresponding AW has been ROB-accepted. Single counter (not per-id)
    // because AXI4 W beats follow AW issue order strictly (no WID).
    uint32_t w_burst_credit_ = 0;
};

// ===== inline impl =====

inline bool Rob::push_aw(const axi::AwBeat& b) {
    if (mode_w_ == RobMode::Enabled) {
        assert(false && "Rob: Enabled mode push_aw not yet implemented (next round)");
        std::abort();
    }
    auto t = addr_trans::xy_route(b.addr);
    auto& s = write_[b.id];
    if (!s.outstanding.empty() && s.outstanding.front().dst_id != t.dst_id) {
        return false;  // stall: same id, different dst
    }
    if (!next_pkt_.push_aw_with_meta(b, {t.dst_id, t.local_addr, 0, 0})) {
        return false;  // downstream backpressure: NO state change
    }
    s.outstanding.push_back({t.dst_id});
    w_burst_credit_++;
    return true;
}

inline bool Rob::push_w(const axi::WBeat& b) {
    if (w_burst_credit_ == 0) return false;  // W cannot proceed before its AW
    if (!next_pkt_.push_w(b)) return false;  // downstream backpressure: NO credit change
    if (b.last) w_burst_credit_--;
    return true;
}

inline bool Rob::push_ar(const axi::ArBeat& b) {
    if (mode_r_ == RobMode::Enabled) {
        assert(false && "Rob: Enabled mode push_ar not yet implemented (next round)");
        std::abort();
    }
    auto t = addr_trans::xy_route(b.addr);
    auto& s = read_[b.id];
    if (!s.outstanding.empty() && s.outstanding.front().dst_id != t.dst_id) {
        return false;
    }
    if (!next_pkt_.push_ar_with_meta(b, {t.dst_id, t.local_addr, 0, 0})) {
        return false;
    }
    s.outstanding.push_back({t.dst_id});
    return true;
}

inline std::optional<axi::BBeat> Rob::pop_b() {
    if (mode_w_ == RobMode::Enabled) {
        assert(false && "Rob: Enabled mode pop_b not yet implemented (next round)");
        std::abort();
    }
    auto opt = next_depkt_.pop_b();
    if (!opt) return std::nullopt;
    auto& s = write_[opt->id];
    assert(!s.outstanding.empty() && "B for id with no outstanding write");
    s.outstanding.pop_front();
    return opt;
}

inline std::optional<axi::RBeat> Rob::pop_r() {
    if (mode_r_ == RobMode::Enabled) {
        assert(false && "Rob: Enabled mode pop_r not yet implemented (next round)");
        std::abort();
    }
    auto opt = next_depkt_.pop_r();
    if (!opt) return std::nullopt;
    if (opt->last) {
        auto& s = read_[opt->id];
        assert(!s.outstanding.empty() && "R(last) for id with no outstanding read");
        s.outstanding.pop_front();
    }
    return opt;
}

}  // namespace ni::cmodel::nmu
```

- [ ] **Step 5: Build + run rob tests**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R NmuRob -j 1
```
Expected: 10 PASS (9 behavior + 1 death).

- [ ] **Step 6: Full ctest sweep**

```bash
ctest --test-dir build -j 1
```
Expected: most PASS. Integration test may still be broken from Task 6 (TestPacketize calls deleted API). Task 8 fixes.

- [ ] **Step 7: Commit Task 7**

```bash
git add c_model/include/nmu/rob.hpp c_model/tests/nmu/test_rob.cpp c_model/tests/nmu/CMakeLists.txt
git commit -m "feat(c_model/nmu): add Rob class (Disabled mode, per-id deque, W burst credit)

In-line layer implementing Packetizer + Depacketizer. Disabled mode logic:
per-AXI-ID outstanding deque, gate stalls same-id different-dst, releases
on B/R response. W burst credit prevents W beats from reaching Packetize
before their AW has been ROB-accepted. Enabled mode bodies stubbed with
assert+abort for next-round implementation. 10 unit tests cover stall,
release, credit, atomicity, multi-outstanding, independence, defensive
abort paths."
```

---

### Task 8: Wire Rob into integration test + multi_dst_stress.yaml + delete TestPacketize

**Files:**
- Modify: `c_model/tests/integration/test_request_response_loopback.cpp`
- Create: `c_model/tests/axi/fixtures/multi_dst_stress.yaml`
- Create: `c_model/tests/axi/fixtures/multi_dst_stress_data.txt` (data bytes for the writes)
- Delete: `c_model/tests/common/test_packetize_adapter.hpp`

**Goal:** Integration test wires Rob into pipeline (replaces TestPacketize wrapper hack). New fixture exercises ROB Disabled stall path end-to-end.

- [ ] **Step 1: Create new fixture data file**

Create `c_model/tests/axi/fixtures/multi_dst_stress_data.txt`:
```
DE AD BE EF CA FE BA BE 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF 00 01 02 03 04 05 06 07 08
12 34 56 78 9A BC DE F0 99 88 77 66 55 44 33 22 AA BB CC DD EE FF 11 22 33 44 55 66 77 88 99 AA
```
(64 bytes total — 2 beats × 32 bytes/beat at size=5; we use single-beat writes so only the first 32 bytes are read per write.)

- [ ] **Step 2: Create multi_dst_stress.yaml fixture**

Create `c_model/tests/axi/fixtures/multi_dst_stress.yaml`:
```yaml
# multi_dst_stress: exercises ROB Disabled stall path end-to-end.
#
# AxiMaster (post AXI4 conformity fix) admits both same-id writes concurrently.
# ROB sees AW1 (id=5, addr=0x100 → dst=0) pass, AW2 (id=5, addr=0x10100 → dst=1)
# stall until AW1's B response returns. After B1 arrives, ROB releases AW2,
# 2nd write completes. Scoreboard verifies both writes' data integrity.
#
# Integration test rig overrides max_outstanding_write to 2 for this fixture
# (no YAML schema change required).
transactions:
  - { op: write, id: 0x05, addr: 0x100,
      size: 5, len: 0, burst: INCR,
      data_file: multi_dst_stress_data.txt, strb_file: "" }
  - { op: write, id: 0x05, addr: 0x10100,
      size: 5, len: 0, burst: INCR,
      data_file: multi_dst_stress_data.txt, strb_file: "" }
```

- [ ] **Step 3: Update integration test — replace TestPacketize with Rob**

In `c_model/tests/integration/test_request_response_loopback.cpp`:

Find the test rig setup (around the existing TestPacketize wiring):
```cpp
nmu::Packetize     real_nmu_pkt(loopback.req_out(), /*src=*/0x01);
test::TestPacketize test_pkt(real_nmu_pkt, /*fixed_dst=*/0x02);
nmu::Depacketize   nmu_depkt(loopback.rsp_in(), params.depkt_b_q_depth, params.depkt_r_q_depth);
// ...
nmu::AxiSlavePort  nmu_port(test_pkt, nmu_depkt, params);
```

Replace with:
```cpp
nmu::Packetize     real_nmu_pkt(loopback.req_out(), /*src=*/0x01);
nmu::Depacketize   nmu_depkt(loopback.rsp_in(), params.depkt_b_q_depth, params.depkt_r_q_depth);
nmu::Rob           rob(real_nmu_pkt, nmu_depkt,
                       nmu::RobMode::Disabled, nmu::RobMode::Disabled);
// ...
nmu::AxiSlavePort  nmu_port(rob, rob, params);  // Rob is both Packetizer and Depacketizer
```

Remove:
```cpp
#include "common/test_packetize_adapter.hpp"
```

Add:
```cpp
#include "nmu/rob.hpp"
```

- [ ] **Step 4: Add multi_dst_stress fixture to INSTANTIATE_TEST_SUITE_P**

Find the `INSTANTIATE_TEST_SUITE_P` block (it lists 6 fixture tuples). Add:
```cpp
INSTANTIATE_TEST_SUITE_P(
    Fixtures, PacketizeLoopbackFixture,
    ::testing::Values(
        // existing 6 entries ...
        std::make_tuple("burst_incr_8beat.yaml", 0u, 0u),
        std::make_tuple("multi_outstanding_stress.yaml", 0u, 0u),
        std::make_tuple("wrap_burst_aligned.yaml", 0u, 0u),
        std::make_tuple("narrow_aligned_multibeat.yaml", 0u, 0u),
        std::make_tuple("sparse_multibeat.yaml", 0u, 0u),
        std::make_tuple("multi_outstanding_stress.yaml", 2u, 3u),  // delayed-loopback variant
        // NEW: ROB stall path coverage (needs max_outstanding_write >= 2)
        std::make_tuple("multi_dst_stress.yaml", 0u, 0u)
    )
);
```

- [ ] **Step 5: Override `max_outstanding_write` for the new fixture in the rig**

Find the test rig's `run_fixture` body where AxiMaster is constructed:
```cpp
axi::AxiMaster master(yaml_path, nmu_port, read_dump,
    /*max_out_w=*/sc.config.max_outstanding_write,
    /*max_out_r=*/sc.config.max_outstanding_read);
```

Add per-fixture override (insert before the `master` construction):
```cpp
// Per-fixture override for ROB stall coverage (multi_dst_stress requires
// >= 2 same-id concurrent writes; YAML config-driven default may be 1).
std::size_t mow = sc.config.max_outstanding_write;
std::size_t mor = sc.config.max_outstanding_read;
if (yaml_path.find("multi_dst_stress.yaml") != std::string::npos) {
    mow = std::max<std::size_t>(mow, 2);
}

axi::AxiMaster master(yaml_path, nmu_port, read_dump,
    /*max_out_w=*/mow,
    /*max_out_r=*/mor);
```

- [ ] **Step 6: Scale Memory size to fit new addresses (0x10100 + 32 bytes)**

Find Memory construction:
```cpp
axi::Memory memory(/*base=*/0, /*size=*/0x10000, /*write_lat=*/0, /*read_lat=*/0);
```

Change `size` to `0x12000` (~73 KB; covers up to 0x10100 + buffer):
```cpp
axi::Memory memory(/*base=*/0, /*size=*/0x12000, /*write_lat=*/0, /*read_lat=*/0);
```

- [ ] **Step 7: Delete TestPacketize**

```bash
git rm c_model/tests/common/test_packetize_adapter.hpp
```

- [ ] **Step 8: Build + run integration test**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R PacketizeLoopback -j 1
```
Expected: 7 tests PASS (6 existing + 1 new multi_dst_stress).

- [ ] **Step 9: Full ctest sweep**

```bash
ctest --test-dir build -j 1
```
Expected: all PASS. Total ctest count: ~285-300 (depending on Stage 2 test impact).

- [ ] **Step 10: Drift gates**

```bash
cd ../specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
```
Expected: all clean.

- [ ] **Step 11: Commit Task 8**

```bash
git add c_model/tests/integration/test_request_response_loopback.cpp \
        c_model/tests/axi/fixtures/multi_dst_stress.yaml \
        c_model/tests/axi/fixtures/multi_dst_stress_data.txt
git rm c_model/tests/common/test_packetize_adapter.hpp
git commit -m "feat(c_model/tests/integration): wire Rob into pipeline, add multi_dst_stress fixture

Replace TestPacketize wrapper hack with real Rob(Disabled, Disabled) in
the e2e loopback test rig. AxiSlavePort takes 'rob, rob' (same Rob instance
as both Packetizer and Depacketizer via multi-inheritance).

New multi_dst_stress.yaml fixture: 2 same-id writes at different XYRouting
dst boundaries (0x100 → dst=0, 0x10100 → dst=1). With AxiMaster relaxed
to admit same-id concurrent, ROB sees both AWs in-flight and stalls 2nd
until 1st B returns. Scoreboard zero mismatch confirms ROB serializes
correctly. Memory size scaled to 0x12000 to cover both addrs.

Per-fixture max_outstanding_write override in rig (no YAML schema change):
multi_dst_stress requires max_out_w >= 2 to exercise stall path.

TestPacketize adapter (c_model/tests/common/test_packetize_adapter.hpp)
deleted — Rob in pipeline replaces it.

Integration test count: 6 → 7 fixture variants."
```

---

## Phase D: Final sweep

### Task 9: Final drift gates + Karpathy 4-lens + NEXT_STEPS update

**Files:**
- Modify: `NEXT_STEPS.md` (flip headline + update next-round path)

**Goal:** Verify clean final state; document Karpathy 4-lens findings; flip NEXT_STEPS pointer to next round (ROB Enabled mode).

- [ ] **Step 1: Run all drift gates**

```bash
cd specgen && py -3 -m pytest -q
py -3 tools/codegen.py --check
py -3 tools/gen_inventory.py --check
cd ../c_model && ctest --test-dir build -j 1
```
Record exact counts. Expected:
- specgen pytest: 163 passed
- codegen --check: clean
- gen_inventory --check: clean
- ctest: ~285-300 passed (depending on Task 4 test impact scope)

If any gate fails: STOP and report BLOCKED.

- [ ] **Step 2: Karpathy 4-lens review (informal, write findings into Step 4 commit message)**

Read commits from this round (`git log --oneline main..HEAD`) and check:

**Overcomplication**:
- addr_trans is 6-line pure function — minimal ✓
- Rob is ~150-line single class — multi-inherit pattern justified by image + spec ✓
- Packetize refactor net-shrinks (sticky setter removed) ✓
- AxiMaster relaxation is targeted (no rewrite, only specific methods/types changed) ✓

**Surgical**:
- Stage 2 axi/ modified per AXI4 conformity; changes localized to AxiMaster + protocol_rules ✓
- Stage 3 port-pair + prior-round packetize/depacketize unchanged ✓
- NSU side untouched ✓
- Confirm via: `git diff main..HEAD -- c_model/include/axi/scoreboard.hpp c_model/include/axi/memory.hpp` should be empty

**Surface assumptions** (in spec, recap):
- XYRouting LOCAL_ADDR_BITS=16 is c_model policy
- ROB Disabled assumes deterministic per-pair routing
- Same-id same-dst ordering implicit invariant from AxiMaster + NoC routing
- AxiSlavePort tick order enables same-cycle ID free

**Verifiable success**:
- ~21 new/modified tests + 1 integration fixture all pass
- Drift gates clean
- ROB stall path validated end-to-end via multi_dst_stress.yaml

- [ ] **Step 3: Update NEXT_STEPS.md**

Read current `NEXT_STEPS.md`. Find the "Current status" headline section near the top. Replace with:

```markdown
## Current status (2026-06-02)

Stage 3 addr_trans + ROB Disabled + AXI4 conformity 完工：
- Stage 2 AxiMaster 修 AXI4 IHI 0022 §A5.3 違規（同 id concurrent 解禁，per-id FIFO + total counter, B/R 走 FIFO front）
- nmu/addr_trans.hpp pure helper (XYRouting)
- nmu/rob.hpp Packetizer + Depacketizer multi-inherit (per-id deque, W burst credit, Disabled mode complete, Enabled mode 全 stub)
- nmu/packetize.hpp 拿掉 sticky setter, +push_*_with_meta (Rob 與未來 Enabled mode 用)
- TestPacketize 完全退場（Rob 在 pipeline 取代）
- Integration test 7 fixtures (6 既有 + 1 新 multi_dst_stress 驗 ROB stall e2e)
- ~285+ ctest sequential, drift gates clean

**Next task per plan §3.1**: ROB Enabled mode — per-AXI-ID reorder buffer (Read 用 burst-aware reorder buffer, Write 用 FIFO metadata buffer)。
參考 FlooNoC `floo_rob.sv` (full SRAM RoB) + `floo_simple_rob.sv` (write metadata) + `tb_floo_rob.sv` (variable-latency slave test pattern)。
配套 LoopbackNoc per-dst latency 擴展（自然產生 out-of-order response）。

後續 `vc_arb` / `vc_mapping` / `route_par` / `flit_ecc` / `nmu.hpp` top-level assembly 各自獨立 round。
```

- [ ] **Step 4: Final commit**

```bash
git add NEXT_STEPS.md
git commit -m "$(cat <<'EOF'
docs(NEXT_STEPS): addr_trans + ROB Disabled + AXI4 conformity done; next is ROB Enabled

Karpathy 4-lens summary (per Task 9):
- Overcomplication: clean — addr_trans 6 lines, Rob ~150 lines (single
  class), Packetize refactor net-shrinks, AxiMaster targeted not rewritten
- Surgical: Stage 2 axi/ modified per AXI4 conformity but localized to
  AxiMaster + protocol_rules; Stage 3 port-pair + prior packetize/depacketize
  unchanged; NSU side untouched
- Surface assumptions: XYRouting bit allocation policy, deterministic
  routing dependency, same-id same-dst implicit ordering, tick-order
  freeing all documented in spec §4.6-4.7
- Verifiable success: ~21 new/modified unit tests + 1 integration fixture
  pass; ROB stall path validated end-to-end via multi_dst_stress

Drift gates final state:
- specgen pytest: 163 passed
- codegen.py --check: clean
- gen_inventory.py --check: clean
- c_model ctest: ~285+ sequential

9 tasks complete, all per implementation plan
docs/superpowers/plans/2026-06-02-addr-trans-rob-disabled-axi4-conformity.md.
PR ready (still on stage3/packetize-depacketize branch per user direction
to accumulate before push).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 5: Final ctest sanity**

```bash
cd c_model && ctest --test-dir build -j 1 2>&1 | tail -3
```
Confirm 100% pass.

---

## Self-review checklist (for plan author — Claude)

After writing the plan, verified:

- **Spec coverage**:
  - §4.1 AxiMaster relaxation → Tasks 1-4
  - §4.2 protocol_rules helpers → Task 3 (with §5.4 tests)
  - §4.3 addr_trans → Task 5
  - §4.4 Packetize refactor → Task 6
  - §4.5 Rob class → Task 7
  - §4.6 tick-order semantics → documented in Rob impl comments + spec
  - §4.7 ROB responsibility boundary → Rob class header doc-comment
  - §5.1-5.6 test plan → Tasks 5 (addr_trans), 6 (packetize updates), 7 (rob), 3 (protocol_rules), 4 (axi_master), 8 (integration)
  - §6 open follow-ups → Task 9 NEXT_STEPS update
- **Placeholder scan**: clean (no TBD / "implement later" / handwave). Code blocks present in every code step.
- **Type consistency**:
  - `AwHeaderMeta` defined in Task 6 (packetize.hpp); used in Tasks 6, 7 (Rob.push_aw)
  - `OutstandingEntry` defined in Task 7 (rob.hpp); future Enabled mode adds rob_idx (commented forward-compat)
  - `Translated` defined in Task 5 (addr_trans.hpp); used in Tasks 6 (Packetize), 7 (Rob)
  - `check_b_front_can_accept_response` / `check_r_front_can_accept_beat` defined in Task 3 (protocol_rules.hpp); used in Tasks 3 (AxiMaster), tested in Task 3 (test_protocol_rules)
  - `RobMode` enum (Disabled / Enabled) defined in Task 7; used by integration in Task 8
- **Cross-references valid**:
  - Tasks 1-3 are atomic (AxiMaster compiles only after all three). Task 1 step 6 + Task 2 step 5 explicitly say "don't commit yet"; Task 3 step 10 commits all atomically.
  - Task 6 commits with temporarily-broken integration test (documented in commit msg); Task 8 fixes.
  - Task 7's Rob unit tests work standalone (no AxiSlavePort needed — uses LoopbackNoc + Packetize + Depacketize directly).

---

## Execution

Plan complete and ready for commit.

**Two execution options:**

**1. Subagent-Driven (recommended)** — Dispatch fresh subagent per task, two-stage review (spec + quality) between tasks, fast iteration. Same pattern as prior packetize+depacketize round.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch with checkpoints.

Which approach?
