# Proportional VC Allocation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split each AXI message class (read/write) across a disjoint, equal pool of virtual channels with round-robin within-pool selection, on both the NMU request side and the NSU response side, so multi-VC topologies (`num_vc = 4, 8`) genuinely exercise the extra channels and the NMU ROB.

**Architecture:** A shared pool-formula helper derives `{write_vcs, read_vcs}` from `num_vc`. The NMU arbiter already has a pools factory and per-id binding; it gains round-robin start rotation. The NSU arbiter is scalar-only and gains the full machinery: per-id (rid/bid) VC binding, round-robin, a pools factory, and self-release of a binding on the terminal flit (`R.rlast` / single `B`). Wrap layers derive pools and reject odd `num_vc`.

**Tech Stack:** C++17, header-only c_model, GoogleTest, CMake. Co-sim untouched by this plan.

## Global Constraints

- C++17; header-only c_model under `c_model/include/`.
- Namespaces: `ni::cmodel::nmu`, `ni::cmodel::nsu`, `ni::cmodel` (shared).
- Naming: snake_case for vars/methods, PascalCase for types; full words, no abbreviation, no camelCase.
- Continuation lines indent 4 spaces; run `clang-format -i` on every edited `.hpp`/`.cpp` (repo `.clang-format`: Google base, IndentWidth 4, ContinuationIndentWidth 4, ColumnLimit 100).
- Build env: `export PATH="$PATH:/c/Windows/System32"` before cmake build (gtest_discover_tests post-link needs `cmd.exe` on PATH on this host).
- Every commit compiles, passes all existing tests, includes tests for new behavior. Commit message format `type(scope): description` (English). No `--no-verify`.
- Public DPI ABI `cmodel_nmu_create(name, src_id, num_vc)` and `cmodel_nsu_create(...)` are unchanged — pools are derived inside the wrap layer.
- `AXI_ID_SPACE` = 256 (`axi::AXI_ID_SPACE`), single source of truth.

**Build + test commands (used throughout):**

```bash
export PATH="$PATH:/c/Windows/System32"
cmake --build build/cmodel -j
ctest --test-dir build/cmodel                    # full suite
ctest --test-dir build/cmodel -R "<regex>" -V    # focused
```

If `build/cmodel` does not exist yet:
```bash
cmake -S c_model -B build/cmodel -G Ninja
```

---

### Task 1: VC pool formula helper

**Files:**
- Create: `c_model/include/ni/vc_pools.hpp`
- Create: `c_model/tests/ni/test_vc_pools.cpp`
- Create: `c_model/tests/ni/CMakeLists.txt`
- Modify: `c_model/tests/CMakeLists.txt` (add `add_subdirectory(ni)`)

**Interfaces:**
- Produces: `struct ni::cmodel::VcPools { std::vector<uint8_t> write_vcs; std::vector<uint8_t> read_vcs; };` and `ni::cmodel::VcPools ni::cmodel::derive_vc_pools(std::size_t num_vc);`

- [ ] **Step 1: Write the failing test**

Create `c_model/tests/ni/test_vc_pools.cpp`:

```cpp
#include "ni/vc_pools.hpp"
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

using ni::cmodel::derive_vc_pools;
using ni::cmodel::VcPools;

TEST(VcPools, NumVc1_BothShareVc0) {
    VcPools p = derive_vc_pools(1);
    EXPECT_EQ(p.write_vcs, (std::vector<uint8_t>{0}));
    EXPECT_EQ(p.read_vcs, (std::vector<uint8_t>{0}));
}

TEST(VcPools, NumVc2_WriteLowReadHigh) {
    VcPools p = derive_vc_pools(2);
    EXPECT_EQ(p.write_vcs, (std::vector<uint8_t>{0}));
    EXPECT_EQ(p.read_vcs, (std::vector<uint8_t>{1}));
}

TEST(VcPools, NumVc4_TwoEach) {
    VcPools p = derive_vc_pools(4);
    EXPECT_EQ(p.write_vcs, (std::vector<uint8_t>{0, 1}));
    EXPECT_EQ(p.read_vcs, (std::vector<uint8_t>{2, 3}));
}

TEST(VcPools, NumVc8_FourEach) {
    VcPools p = derive_vc_pools(8);
    EXPECT_EQ(p.write_vcs, (std::vector<uint8_t>{0, 1, 2, 3}));
    EXPECT_EQ(p.read_vcs, (std::vector<uint8_t>{4, 5, 6, 7}));
}

TEST(VcPoolsDeath, OddNumVcAborts) {
    EXPECT_DEATH({ (void)derive_vc_pools(3); }, "num_vc must be 1 or even");
}
```

- [ ] **Step 2: Wire CMake**

Create `c_model/tests/ni/CMakeLists.txt`:

```cmake
add_cmodel_test(test_vc_pools)
target_include_directories(test_vc_pools PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

In `c_model/tests/CMakeLists.txt`, add after the existing `add_subdirectory(router)` line:

```cmake
add_subdirectory(ni)
```

- [ ] **Step 3: Run test to verify it fails (does not compile — header missing)**

```bash
export PATH="$PATH:/c/Windows/System32"
cmake -S c_model -B build/cmodel -G Ninja
cmake --build build/cmodel --target test_vc_pools -j
```
Expected: build FAIL, `fatal error: ni/vc_pools.hpp: No such file or directory`.

- [ ] **Step 4: Write the implementation**

Create `c_model/include/ni/vc_pools.hpp`:

```cpp
#pragma once
// Split a VC count into disjoint, equal read/write class pools.
//   num_vc == 1       -> write {0}, read {0}  (degenerate, shared lane)
//   num_vc >= 2, even -> write {0..n/2-1}, read {n/2..n-1}
// Odd num_vc (>1) has no equal split and is rejected loudly: message-class
// separation requires two disjoint, equal pools for deadlock avoidance.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ni::cmodel {

struct VcPools {
    std::vector<uint8_t> write_vcs;
    std::vector<uint8_t> read_vcs;
};

inline VcPools derive_vc_pools(std::size_t num_vc) {
    assert((num_vc == 1 || num_vc % 2 == 0) &&
           "derive_vc_pools: num_vc must be 1 or even (no equal read/write split otherwise)");
    VcPools pools;
    if (num_vc == 1) {
        pools.write_vcs = {0};
        pools.read_vcs = {0};
        return pools;
    }
    const std::size_t half = num_vc / 2;
    for (std::size_t i = 0; i < half; ++i)
        pools.write_vcs.push_back(static_cast<uint8_t>(i));
    for (std::size_t i = half; i < num_vc; ++i)
        pools.read_vcs.push_back(static_cast<uint8_t>(i));
    return pools;
}

}  // namespace ni::cmodel
```

- [ ] **Step 5: Run test to verify it passes**

```bash
export PATH="$PATH:/c/Windows/System32"
cmake --build build/cmodel --target test_vc_pools -j
ctest --test-dir build/cmodel -R "VcPools" -V
```
Expected: PASS, 5 tests.

- [ ] **Step 6: clang-format + commit**

```bash
clang-format -i c_model/include/ni/vc_pools.hpp c_model/tests/ni/test_vc_pools.cpp
git add c_model/include/ni/vc_pools.hpp c_model/tests/ni/test_vc_pools.cpp \
        c_model/tests/ni/CMakeLists.txt c_model/tests/CMakeLists.txt
git commit -m "feat(vc): VC pool formula helper with odd-num_vc guard"
```

---

### Task 2: NMU arbiter round-robin within-pool selection

**Files:**
- Modify: `c_model/include/nmu/vc_arbiter.hpp` (members + `select_vc_for_axi_ch`, ~line 147-177)
- Test: `c_model/tests/nmu/test_vc_arbiter.cpp` (append tests)

**Interfaces:**
- Consumes: existing `VcArbiter::read_write_split_pools(downstream, num_vc, write_vcs, read_vcs, pending_depth)` (`nmu/vc_arbiter.hpp:70`).
- Produces: no signature change. Behavior change: for an unbound id, the pool scan starts at a per-class rotating index instead of always index 0. Single-element pools (vc1/vc2) are unaffected.

- [ ] **Step 1: Write the failing test**

Append to `c_model/tests/nmu/test_vc_arbiter.cpp` (before the `INSTANTIATE_TEST_SUITE_P` line; uses the file's existing `make_flit` / `push_and_vc` helpers):

```cpp
// Round-robin spread: with a read pool {2,3} (num_vc=4), four DISTINCT unbound
// arids must not all land on the lowest pool VC. First-available would pin all
// to VC=2; round-robin alternates 2,3,2,3.
TEST(NmuVcArbiterRoundRobin, DistinctReadIdsSpreadAcrossPool) {
    SCENARIO("NMU VcArbiter pools: distinct unbound arids round-robin over read pool");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.req_out(), /*num_vc=*/4,
                                                 /*write_vcs=*/{0, 1}, /*read_vcs=*/{2, 3});
    uint8_t vc_a = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/0x10));
    uint8_t vc_b = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/0x11));
    uint8_t vc_c = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/0x12));
    uint8_t vc_d = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/0x13));
    EXPECT_EQ(vc_a, 2u);
    EXPECT_EQ(vc_b, 3u);
    EXPECT_EQ(vc_c, 2u);
    EXPECT_EQ(vc_d, 3u);
}

// Single id pins to one VC: a bound id reuses its VC; round-robin never fires.
// Documents the precondition that spread needs MULTIPLE distinct unbound ids.
TEST(NmuVcArbiterRoundRobin, SingleReadIdPinsToOneVc) {
    SCENARIO("NMU VcArbiter pools: one arid stays on its bound VC across bursts");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.req_out(), /*num_vc=*/4,
                                                 /*write_vcs=*/{0, 1}, /*read_vcs=*/{2, 3});
    uint8_t first = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/0x20));
    // Same id never drains its binding here (no on_id_drained call), so it sticks.
    uint8_t again = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/0x20));
    EXPECT_EQ(first, again);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
export PATH="$PATH:/c/Windows/System32"
cmake --build build/cmodel --target test_vc_arbiter -j
ctest --test-dir build/cmodel -R "NmuVcArbiterRoundRobin.DistinctReadIdsSpreadAcrossPool" -V
```
Expected: FAIL — `vc_b` is 2 (first-available pins to lowest), not 3.

- [ ] **Step 3: Write the implementation**

In `c_model/include/nmu/vc_arbiter.hpp`, add two members beside `round_robin_ptr_` (`:140`):

```cpp
    uint8_t round_robin_ptr_ = 0;
    uint8_t write_rr_start_ = 0;  // per-class round-robin scan start (selection)
    uint8_t read_rr_start_ = 0;
```

Replace the unbound-scan tail of `select_vc_for_axi_ch` (`:172-176`):

```cpp
    if ((*binding)[id].has_value()) return (*binding)[id];  // bound: stick (even if full)
    const std::vector<uint8_t>* cand = candidates_for(axi_ch);
    for (uint8_t vc : *cand) {  // unbound: first available
        if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) return vc;
    }
    return std::nullopt;
```

with:

```cpp
    if ((*binding)[id].has_value()) return (*binding)[id];  // bound: stick (even if full)
    const std::vector<uint8_t>* cand = candidates_for(axi_ch);
    uint8_t& rr = (axi_ch == ni::AXI_CH_AW) ? write_rr_start_ : read_rr_start_;
    const std::size_t n = cand->size();
    for (std::size_t k = 0; k < n; ++k) {  // unbound: round-robin from rr, first available
        uint8_t vc = (*cand)[(rr + k) % n];
        if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) {
            rr = static_cast<uint8_t>((static_cast<std::size_t>(rr) + k + 1) % n);
            return vc;
        }
    }
    return std::nullopt;
```

Rationale: rotation advances only on a successful unbound selection (bound ids return early above, never touching `rr`). A single-element pool makes `(rr + k) % 1 == 0` always, so vc1/vc2 are byte-identical to first-available.

- [ ] **Step 4: Run new + full arbiter tests**

```bash
export PATH="$PATH:/c/Windows/System32"
cmake --build build/cmodel --target test_vc_arbiter -j
ctest --test-dir build/cmodel -R "test_vc_arbiter|NmuVcArб" -V
ctest --test-dir build/cmodel -R "VcArb" 
```
Expected: new tests PASS. If any PRE-EXISTING pools test asserted first-available order across multiple distinct same-class ids, it now sees round-robin order — update that test's expected VC sequence to the rotation order (2,3,2,3...) and note it in the commit. Scalar/single-pool and cross-class (AW vs AR) tests are unaffected.

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i c_model/include/nmu/vc_arbiter.hpp c_model/tests/nmu/test_vc_arbiter.cpp
git add c_model/include/nmu/vc_arbiter.hpp c_model/tests/nmu/test_vc_arbiter.cpp
git commit -m "feat(vc): round-robin within-pool VC selection in NMU arbiter"
```

---

### Task 3: NMU wiring — config pools + wrap derivation

**Files:**
- Modify: `c_model/include/nmu/nmu.hpp` (`NmuConfig` ~line 127-133; `make_vc_arbiter` ~line 256-263)
- Modify: `c_model/include/wrap/nmu_wrap.hpp` (`init` ~line 51-54)
- Test: `c_model/tests/nmu/test_vc_arbiter.cpp` (one wiring test) — or reuse `test_nmu` if it builds NmuConfig; this plan uses a direct NmuConfig→make_vc_arbiter check kept in `test_vc_arbiter.cpp`.

**Interfaces:**
- Consumes: `ni::cmodel::derive_vc_pools` (Task 1); `VcArbiter::read_write_split_pools` (Task 2).
- Produces: `NmuConfig.write_vcs` / `NmuConfig.read_vcs` (`std::vector<uint8_t>`); when non-empty, `make_vc_arbiter` builds a pools arbiter. `NmuWrap::init` populates them from `derive_vc_pools(num_vc)`.

- [ ] **Step 1: Write the failing test**

Append to `c_model/tests/nmu/test_vc_arbiter.cpp` (before `INSTANTIATE_TEST_SUITE_P`). Add include at the top of the file if not present: `#include "nmu/nmu.hpp"` and `#include "ni/vc_pools.hpp"`.

```cpp
// Wiring: NmuConfig carrying pools builds a pools arbiter that spreads.
TEST(NmuConfigPools, ConfigPoolsBuildSpreadingArbiter) {
    using ni::cmodel::nmu::NmuConfig;
    using ni::cmodel::nmu::detail::make_vc_arbiter;  // factory lives in nmu::detail
    SCENARIO("NmuConfig.write_vcs/read_vcs -> make_vc_arbiter -> pools arbiter");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    NmuConfig cfg{};
    cfg.num_vc = 4;
    cfg.vc_mode = ni::cmodel::nmu::VcMode::ReadWriteSplit;
    cfg.write_vcs = {0, 1};
    cfg.read_vcs = {2, 3};
    auto arb = make_vc_arbiter(cfg, noc.req_out());
    uint8_t vc_a = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/0x30));
    uint8_t vc_b = push_and_vc(arb, noc, make_flit(ni::AXI_CH_AR, 0, 0, 0, /*id=*/0x31));
    EXPECT_EQ(vc_a, 2u);
    EXPECT_EQ(vc_b, 3u);
}
```

(`make_vc_arbiter` is a free `inline` function in `ni::cmodel::nmu::detail` at `nmu.hpp:254-266` — reference it via the `detail::` qualifier as above.)

- [ ] **Step 2: Run test to verify it fails**

```bash
export PATH="$PATH:/c/Windows/System32"
cmake --build build/cmodel --target test_vc_arbiter -j
```
Expected: build FAIL — `NmuConfig` has no member `write_vcs`.

- [ ] **Step 3: Write the implementation**

In `c_model/include/nmu/nmu.hpp`, add to `NmuConfig` after the `vc_candidates` field (`:133`):

```cpp
    // ReadWriteSplit pool variant: when non-empty, each class draws from a VC
    // pool with round-robin selection instead of the single write_vc/read_vc.
    std::vector<uint8_t> write_vcs{};
    std::vector<uint8_t> read_vcs{};
```

In `make_vc_arbiter` (`:256-259`), change the ReadWriteSplit branch:

```cpp
    if (cfg.vc_mode == VcMode::ReadWriteSplit) {
        return VcArbiter::read_write_split(downstream, cfg.num_vc, cfg.write_vc, cfg.read_vc,
                                           cfg.vc_arbiter_pending_depth);
    }
```

to:

```cpp
    if (cfg.vc_mode == VcMode::ReadWriteSplit) {
        if (!cfg.write_vcs.empty() || !cfg.read_vcs.empty()) {
            return VcArbiter::read_write_split_pools(downstream, cfg.num_vc, cfg.write_vcs,
                                                     cfg.read_vcs, cfg.vc_arbiter_pending_depth);
        }
        return VcArbiter::read_write_split(downstream, cfg.num_vc, cfg.write_vc, cfg.read_vc,
                                           cfg.vc_arbiter_pending_depth);
    }
```

In `c_model/include/wrap/nmu_wrap.hpp`, add include `#include "ni/vc_pools.hpp"` at the top, and replace the scalar assignment (`:52-53`):

```cpp
        cfg.write_vc = 0;
        cfg.read_vc = (num_vc >= 2) ? 1u : 0u;
```

with:

```cpp
        const auto vc_pools = ni::cmodel::derive_vc_pools(num_vc);  // asserts odd num_vc
        cfg.write_vcs = vc_pools.write_vcs;
        cfg.read_vcs = vc_pools.read_vcs;
```

- [ ] **Step 4: Run test + full NMU suite**

```bash
export PATH="$PATH:/c/Windows/System32"
cmake --build build/cmodel -j
ctest --test-dir build/cmodel -R "NmuConfigPools|test_vc_arbiter|test_nmu|Rob" 
```
Expected: new test PASS; all NMU/Rob tests still pass (vc1/vc2 unchanged; ROB co-sim path now uses pools but single-read-id behavior is identical).

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i c_model/include/nmu/nmu.hpp c_model/include/wrap/nmu_wrap.hpp \
              c_model/tests/nmu/test_vc_arbiter.cpp
git add c_model/include/nmu/nmu.hpp c_model/include/wrap/nmu_wrap.hpp \
        c_model/tests/nmu/test_vc_arbiter.cpp
git commit -m "feat(vc): NMU config pools + wrap derives read/write pools from num_vc"
```

---

### Task 4: NSU arbiter — per-id binding + round-robin + pools factory

**Files:**
- Modify: `c_model/include/nsu/vc_arbiter.hpp` (factory, constructor, members, `select_vc_for_axi_ch`, `push_flit`)
- Test: `c_model/tests/nsu/test_nsu_vc_arbiter.cpp` (append tests + extend `make_rsp_flit`)

**Interfaces:**
- Consumes: response flit payload fields `B.bid`, `R.rid`, `R.rlast` (`nsu/packetize.hpp:90,105,108`); `axi::AXI_ID_SPACE`.
- Produces: `static VcArbiter VcArbiter::read_write_split_pools(downstream, num_vc, write_rsp_vcs, read_rsp_vcs, pending_depth)`. In pools mode: B binds on `bid`, R binds on `rid`; a burst's beats stick to its bound VC; binding releases on the terminal flit (`R.rlast==1`, or any `B`); distinct ids round-robin across the class pool.

- [ ] **Step 1: Extend the test flit helper + write failing tests**

In `c_model/tests/nsu/test_nsu_vc_arbiter.cpp`, replace `make_rsp_flit` with an id/rlast-aware version:

```cpp
Flit make_rsp_flit(uint8_t axi_ch, uint8_t initial_vc = 0, uint8_t id = 0, uint64_t rlast = 1) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", 0x12);
    f.set_header_field("vc_id", initial_vc);
    f.set_header_field("src_id", 0x34);
    f.set_header_field("last", 1);
    if (axi_ch == ni::AXI_CH_R) {
        f.set_payload_field("R", "rid", id);
        f.set_payload_field("R", "rlast", rlast);
    } else if (axi_ch == ni::AXI_CH_B) {
        f.set_payload_field("B", "bid", id);
    }
    return f;
}
```

Append these tests (before `INSTANTIATE_TEST_SUITE_P`):

```cpp
// A multi-beat R burst (one rid) keeps every beat on its single bound VC.
TEST(NsuVcArbiterPools, RBurstStaysOnOneVc) {
    SCENARIO("NSU pools: all beats of one rid's R burst map to one VC");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    // 4-beat burst, rid=0x05; beats 1-3 rlast=0, beat 4 rlast=1.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));
    // All four beats on the first read-pool VC (2); none on VC=3.
    EXPECT_EQ(arb.pending_size(2), 4u);
    EXPECT_EQ(arb.pending_size(3), 0u);
}

// Distinct rids (each a single-beat read) round-robin across the read pool.
TEST(NsuVcArbiterPools, DistinctRidsSpreadAcrossPool) {
    SCENARIO("NSU pools: distinct rids round-robin over read pool {2,3}");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));  // rid5 -> VC2, releases
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x06, 1)));  // rid6 -> VC3, releases
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x07, 1)));  // rid7 -> VC2
    EXPECT_EQ(arb.pending_size(2), 2u);  // rid5 + rid7
    EXPECT_EQ(arb.pending_size(3), 1u);  // rid6
}

// B uses the write pool, R uses the read pool (response-class separation).
TEST(NsuVcArbiterPools, BUsesWritePoolRUsesReadPool) {
    SCENARIO("NSU pools: B -> write pool {0,1}, R -> read pool {2,3}");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B, 0, 0x05, 1)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));
    EXPECT_EQ(arb.pending_size(0), 1u);  // B on write pool
    EXPECT_EQ(arb.pending_size(2), 1u);  // R on read pool
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
export PATH="$PATH:/c/Windows/System32"
cmake --build build/cmodel --target test_nsu_vc_arbiter -j
```
Expected: build FAIL — `read_write_split_pools` not a member of `nsu::VcArbiter`.

- [ ] **Step 3: Write the implementation**

In `c_model/include/nsu/vc_arbiter.hpp`:

Add includes near the top:
```cpp
#include "axi/types.hpp"
```

Add the pools factory after `read_write_split` (`:47`):

```cpp
    static VcArbiter read_write_split_pools(router::NocRspOut& downstream, std::size_t num_vc,
                                            std::vector<uint8_t> write_rsp_vcs,
                                            std::vector<uint8_t> read_rsp_vcs,
                                            std::size_t pending_depth = kDefaultPendingDepth) {
        std::array<std::vector<uint8_t>, AXI_CH_COUNT> empty_candidates{};
        VcArbiter a(downstream, num_vc, VcMode::ReadWriteSplit, /*write_rsp_vc=*/0,
                    /*read_rsp_vc=*/0, std::move(empty_candidates), pending_depth);
        a.write_rsp_vcs_ = std::move(write_rsp_vcs);
        a.read_rsp_vcs_ = std::move(read_rsp_vcs);
        a.use_pools_ = true;
        return a;
    }
```

Add members (after `read_rsp_vc_`, `:89`):

```cpp
    std::vector<uint8_t> write_rsp_vcs_;
    std::vector<uint8_t> read_rsp_vcs_;
    bool use_pools_ = false;
    uint8_t write_rr_start_ = 0;
    uint8_t read_rr_start_ = 0;
    static constexpr std::size_t AXI_ID_SPACE = axi::AXI_ID_SPACE;  // 256
    std::array<std::optional<uint8_t>, AXI_ID_SPACE> write_binding_{};
    std::array<std::optional<uint8_t>, AXI_ID_SPACE> read_binding_{};
```

Change `select_vc_for_axi_ch` signature to take an id and add the pools path. Replace the whole function (`:96-112`):

```cpp
inline std::optional<uint8_t> VcArbiter::select_vc_for_axi_ch(uint8_t axi_ch, uint8_t id) {
    if (num_vc_ == 1) return uint8_t{0};

    if (mode_ == VcMode::MultiCandidate) {
        for (uint8_t vc : candidate_vcs_[axi_ch]) {
            if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) return vc;
        }
        return std::nullopt;
    }

    // ReadWriteSplit, scalar (no pools configured).
    if (!use_pools_) {
        if (axi_ch == ni::AXI_CH_B) return write_rsp_vc_;
        if (axi_ch == ni::AXI_CH_R) return read_rsp_vc_;
        return std::nullopt;
    }

    // ReadWriteSplit pools: per-id sticky binding + round-robin within the class pool.
    std::array<std::optional<uint8_t>, AXI_ID_SPACE>* binding = nullptr;
    const std::vector<uint8_t>* cand = nullptr;
    uint8_t* rr = nullptr;
    if (axi_ch == ni::AXI_CH_B) {
        binding = &write_binding_;
        cand = &write_rsp_vcs_;
        rr = &write_rr_start_;
    } else if (axi_ch == ni::AXI_CH_R) {
        binding = &read_binding_;
        cand = &read_rsp_vcs_;
        rr = &read_rr_start_;
    } else {
        return std::nullopt;
    }
    if ((*binding)[id].has_value()) return (*binding)[id];  // bound: stick
    const std::size_t n = cand->size();
    for (std::size_t k = 0; k < n; ++k) {  // unbound: round-robin from rr
        uint8_t vc = (*cand)[(*rr + k) % n];
        if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) {
            *rr = static_cast<uint8_t>((static_cast<std::size_t>(*rr) + k + 1) % n);
            return vc;
        }
    }
    return std::nullopt;
}
```

Update the declaration (`:83`) to `std::optional<uint8_t> select_vc_for_axi_ch(uint8_t axi_ch, uint8_t id);`.

Replace `push_flit` (`:114-124`) to read the id, commit the binding, and release on the terminal flit:

```cpp
inline bool VcArbiter::push_flit(const Flit& flit) {
    uint8_t axi_ch = static_cast<uint8_t>(flit.get_header_field("axi_ch"));
    uint8_t id = 0;
    if (use_pools_ && num_vc_ > 1) {
        if (axi_ch == ni::AXI_CH_B)
            id = static_cast<uint8_t>(flit.get_payload_field("B", "bid"));
        else if (axi_ch == ni::AXI_CH_R)
            id = static_cast<uint8_t>(flit.get_payload_field("R", "rid"));
    }
    auto vc_opt = select_vc_for_axi_ch(axi_ch, id);
    if (!vc_opt.has_value()) return false;
    uint8_t vc_id = *vc_opt;
    if (pending_[vc_id].size() >= pending_depth_) return false;

    // Commit the (class,id) -> vc binding after the accept conditions pass.
    if (use_pools_ && num_vc_ > 1) {
        if (axi_ch == ni::AXI_CH_B)
            write_binding_[id] = vc_id;
        else if (axi_ch == ni::AXI_CH_R)
            read_binding_[id] = vc_id;
    }

    Flit stamped = flit;
    stamped.set_header_field("vc_id", vc_id);
    pending_[vc_id].push_back(stamped);

    // Release the binding on the burst's terminal flit (payload R.rlast, or the
    // single-flit B) so the next same-id burst rebinds via round-robin. Header
    // `last` is always 1, so the payload field must be used for R.
    if (use_pools_ && num_vc_ > 1) {
        if (axi_ch == ni::AXI_CH_R) {
            if (flit.get_payload_field("R", "rlast") != 0) read_binding_[id].reset();
        } else if (axi_ch == ni::AXI_CH_B) {
            write_binding_[id].reset();
        }
    }
    return true;
}
```

- [ ] **Step 4: Run new + full NSU arbiter suite**

```bash
export PATH="$PATH:/c/Windows/System32"
cmake --build build/cmodel --target test_nsu_vc_arbiter -j
ctest --test-dir build/cmodel -R "NsuVcArb|test_nsu_vc_arbiter" -V
```
Expected: 3 new tests PASS; all existing NSU arbiter tests (scalar `read_write_split`, `multi_candidate`, NumVc1) PASS unchanged (`use_pools_` defaults false).

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i c_model/include/nsu/vc_arbiter.hpp c_model/tests/nsu/test_nsu_vc_arbiter.cpp
git add c_model/include/nsu/vc_arbiter.hpp c_model/tests/nsu/test_nsu_vc_arbiter.cpp
git commit -m "feat(vc): NSU arbiter per-id binding + round-robin pools for responses"
```

---

### Task 5: NSU wiring — config pools + wrap derivation

**Files:**
- Modify: `c_model/include/nsu/nsu.hpp` (`NsuConfig` ~line 55-59; `make_vc_arbiter` ~line 137-143)
- Modify: `c_model/include/wrap/nsu_wrap.hpp` (`init` ~line 56-59)
- Test: `c_model/tests/nsu/test_nsu_vc_arbiter.cpp` (one wiring test)

**Interfaces:**
- Consumes: `ni::cmodel::derive_vc_pools` (Task 1); `nsu::VcArbiter::read_write_split_pools` (Task 4).
- Produces: `NsuConfig.write_rsp_vcs` / `NsuConfig.read_rsp_vcs`; when non-empty, `make_vc_arbiter` builds a pools arbiter. `NsuWrap::init` populates them from `derive_vc_pools(num_vc)`.

- [ ] **Step 1: Write the failing test**

Append to `c_model/tests/nsu/test_nsu_vc_arbiter.cpp` (add includes `#include "nsu/nsu.hpp"` and `#include "ni/vc_pools.hpp"` at top if absent):

```cpp
TEST(NsuConfigPools, ConfigPoolsBuildSpreadingArbiter) {
    using ni::cmodel::nsu::NsuConfig;
    using ni::cmodel::nsu::detail::make_vc_arbiter;  // factory lives in nsu::detail
    SCENARIO("NsuConfig.write_rsp_vcs/read_rsp_vcs -> make_vc_arbiter -> pools arbiter");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    NsuConfig cfg{};
    cfg.num_vc = 4;
    cfg.write_rsp_vcs = {0, 1};
    cfg.read_rsp_vcs = {2, 3};
    auto arb = make_vc_arbiter(cfg, noc.rsp_out());
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x06, 1)));
    EXPECT_EQ(arb.pending_size(2), 1u);  // rid5
    EXPECT_EQ(arb.pending_size(3), 1u);  // rid6
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
export PATH="$PATH:/c/Windows/System32"
cmake --build build/cmodel --target test_nsu_vc_arbiter -j
```
Expected: build FAIL — `NsuConfig` has no member `write_rsp_vcs`.

- [ ] **Step 3: Write the implementation**

In `c_model/include/nsu/nsu.hpp`, add to `NsuConfig` after `vc_candidates` (`:59`):

```cpp
    // ReadWriteSplit pool variant (response side): non-empty -> per-class pool
    // with per-id binding + round-robin, mirroring the NMU request side.
    std::vector<uint8_t> write_rsp_vcs{};
    std::vector<uint8_t> read_rsp_vcs{};
```

Change `make_vc_arbiter` ReadWriteSplit branch (`:138-141`):

```cpp
    if (cfg.vc_mode == VcMode::ReadWriteSplit) {
        return VcArbiter::read_write_split(downstream, cfg.num_vc, cfg.write_rsp_vc,
                                           cfg.read_rsp_vc, cfg.vc_arbiter_pending_depth);
    }
```

to:

```cpp
    if (cfg.vc_mode == VcMode::ReadWriteSplit) {
        if (!cfg.write_rsp_vcs.empty() || !cfg.read_rsp_vcs.empty()) {
            return VcArbiter::read_write_split_pools(downstream, cfg.num_vc, cfg.write_rsp_vcs,
                                                     cfg.read_rsp_vcs, cfg.vc_arbiter_pending_depth);
        }
        return VcArbiter::read_write_split(downstream, cfg.num_vc, cfg.write_rsp_vc,
                                           cfg.read_rsp_vc, cfg.vc_arbiter_pending_depth);
    }
```

(Verify the exact `NsuConfig` field name for `vc_mode` and the pending-depth field at `nsu.hpp`; mirror the names used by the existing scalar call.)

In `c_model/include/wrap/nsu_wrap.hpp`, add include `#include "ni/vc_pools.hpp"`, and replace the scalar rsp-vc assignment (`:56-59`, currently `cfg.write_rsp_vc = 0; cfg.read_rsp_vc = (num_vc>=2)?1:0;`) with:

```cpp
        const auto vc_pools = ni::cmodel::derive_vc_pools(num_vc);  // asserts odd num_vc
        cfg.write_rsp_vcs = vc_pools.write_vcs;
        cfg.read_rsp_vcs = vc_pools.read_vcs;
```

- [ ] **Step 4: Run test + full NSU suite**

```bash
export PATH="$PATH:/c/Windows/System32"
cmake --build build/cmodel -j
ctest --test-dir build/cmodel -R "NsuConfigPools|test_nsu|Nsu" 
```
Expected: new test PASS; all NSU tests pass.

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i c_model/include/nsu/nsu.hpp c_model/include/wrap/nsu_wrap.hpp \
              c_model/tests/nsu/test_nsu_vc_arbiter.cpp
git add c_model/include/nsu/nsu.hpp c_model/include/wrap/nsu_wrap.hpp \
        c_model/tests/nsu/test_nsu_vc_arbiter.cpp
git commit -m "feat(vc): NSU config pools + wrap derives response pools from num_vc"
```

---

### Task 6: Full regression sweep + odd-num_vc wrap guard test

**Files:**
- Test: `c_model/tests/wrap/` (add a death test that a wrap init with odd num_vc aborts) — confirm exact wrap test file name first (`ls c_model/tests/wrap`).
- No production changes expected; this task is the integration gate.

**Interfaces:**
- Consumes: all prior tasks.
- Produces: confirmation that the full suite is green and odd num_vc is rejected at the wrap boundary.

- [ ] **Step 1: Write the failing guard test**

Pick the existing wrap test executable (e.g. `test_cmodel_dpi` or an NMU-wrap test under `c_model/tests/wrap/`). Add:

```cpp
TEST(WrapOddNumVcDeath, NmuWrapRejectsOddNumVc) {
    ni::cmodel::wrap::NmuWrap nmu;  // adjust to the actual wrap type/namespace in this file
    EXPECT_DEATH({ nmu.init(/*src_id=*/0, /*num_vc=*/3); }, "num_vc must be 1 or even");
}
```

(Confirm the wrap class name/namespace and `init` signature from the chosen test file's existing includes before writing.)

- [ ] **Step 2: Run to verify it fails or passes-by-construction**

```bash
export PATH="$PATH:/c/Windows/System32"
cmake --build build/cmodel -j
ctest --test-dir build/cmodel -R "WrapOddNumVcDeath" -V
```
Expected: PASS (the `derive_vc_pools` assert from Task 1/3/5 already fires). If it does NOT abort, the wrap is not routing through `derive_vc_pools` — fix the wrap wiring, do not weaken the test.

- [ ] **Step 3: Run the full suite**

```bash
export PATH="$PATH:/c/Windows/System32"
cmake --build build/cmodel -j
ctest --test-dir build/cmodel
```
Expected: 100% pass, count = prior 553 + new tests (Task 1: 5, Task 2: 2, Task 3: 1, Task 4: 3, Task 5: 1, Task 6: 1). Investigate any failure; the likely fallout is a pre-existing multi-id pools-order test now seeing round-robin order (update its expectation to the rotation order, do not revert the arbiter).

- [ ] **Step 4: clang-format + commit**

```bash
clang-format -i <edited test file>
git add <edited test file>
git commit -m "test(vc): odd-num_vc wrap guard + full-suite regression gate"
```

---

## Self-Review

**Spec coverage:**
- Pool formula + odd-num_vc reject → Task 1 (+ enforced at wrap in Tasks 3/5/6). ✓
- Contiguous write-low/read-high → Task 1 formula. ✓
- NMU pools wiring + round-robin → Tasks 2, 3. ✓
- NSU per-id binding + round-robin + pools factory + burst-end on payload `R.rlast` + release ordering → Task 4. ✓
- NSU wiring + response mirror → Task 5. ✓
- Multi-distinct-id precondition (spread) + single-id pins → Tasks 2 (`SingleReadIdPinsToOneVc`), 4 (`RBurstStaysOnOneVc`). ✓
- Backward compat vc1/vc2 unchanged → asserted by single-element-pool reasoning + full suite in Task 6. ✓
- Invariant: ROB fix survives (single burst one VC) → enforced by NSU per-id binding (Task 4 `RBurstStaysOnOneVc`). ✓
- Non-goal (no router/topology change) → no router files touched. ✓

**Placeholder scan:** Each code step shows complete code. Two explicit "confirm the exact name/linkage" notes (Task 3 `make_vc_arbiter` linkage, Task 5 `NsuConfig` field names, Task 6 wrap class name) are verification instructions, not placeholders — the surrounding code is complete and the fields are quoted from files read during planning.

**Type consistency:** `derive_vc_pools` / `VcPools.write_vcs` / `read_vcs` consistent across Tasks 1/3/5. `read_write_split_pools` signature matches the existing NMU factory (Task 2) and the new NSU factory (Task 4). `write_rr_start_`/`read_rr_start_` naming consistent in both arbiters. `use_pools_` only in NSU (NMU pools were pre-existing).

## Risk note

The single behavioral-change risk is a pre-existing NMU pools test that asserted first-available order across multiple distinct same-class ids; round-robin changes that order. Tasks 2/4/6 instruct updating such a test's expected VC sequence to the rotation order rather than reverting the arbiter. vc1/vc2 and cross-class tests are provably unaffected (single-element pool ⇒ rotation is a no-op).
