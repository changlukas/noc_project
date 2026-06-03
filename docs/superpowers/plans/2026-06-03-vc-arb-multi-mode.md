# VcArb Multi-Mode + Header.last Wormhole Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add NMU + NSU virtual-channel arbiter with two modes (ReadWriteSplit, MultiCandidate), per-VC credit-gated round-robin, W-follows-AW invariant, plus a FlooNoC-aligned `header.last` fix in `nmu/packetize.hpp` — across 7 commits, growing ctest from 302 to 324 (spec §13.4 target was 320; +4 from Task 2 dedicated LoopbackNoc tests not separately counted in spec).

**Architecture:** Decorator pattern — `VcArb` implements `NocReqOut`/`NocRspOut` and wraps the real downstream (LoopbackNoc). NMU `Packetize` keeps its existing `NocReqOut&` injection; the reference now points at a `VcArb` instance which routes per-axi_ch to a per-VC pending queue, then round-robin drains to downstream gated by per-VC credit availability.

**Tech Stack:** C++17, GoogleTest (incl. EXPECT_DEATH), CMake, Windows PowerShell + `py -3`. Codegen-derived constants from `ni::header::VC_ID_WIDTH` and the JSON spec inventory.

**Spec:** `docs/superpowers/specs/2026-06-03-vc-arb-multi-mode-design.md` (Codex round-4 APPROVED).

**Branch:** `stage3/packetize-depacketize` (commits accumulate; no push until full Stage 3 merge).

---

## Implementation note: divergence from spec §6

Spec §6.1 sketches `LoopbackNoc` per-VC FIFO as a NEW per-VC deque replacing the per-NSU deque. The actual `LoopbackNoc` (lines 282-294 of `c_model/tests/common/loopback_noc.hpp`) is more layered: per-NSU FIFOs (`nsu_req_q_`), per-NSU latency queues (`nsu_rsp_delay_q_`), legacy global delay pipe (`req_pipe_`, `rsp_pipe_`), multi-NSU dst routing. Replacing per-NSU with per-VC breaks 302 existing tests + multi-NSU routing.

**Implementation chosen** (Task 2 details): keep `nsu_req_q_[nsu_idx]` (and analogous `rsp_q_`) as-is. ADD `std::array<std::size_t, NUM_VC_MAX> nmu_per_vc_in_flight_` credit counter at NMU side — incremented on `push_flit`, decremented on the NSU side's `pop_flit`. `credit_avail(vc)` queries the counter. Backward compat preserved: `per_vc_depth_` defaults to a value ≥ existing queue depth, so legacy tests never hit per-VC credit pressure. New VcArb tests opt in to smaller `per_vc_depth_` to exercise gating.

The behavioral guarantee from spec (per-VC credit-based flow control gating push) is preserved; the storage model is a counter not a separate queue.

---

## File Structure

### Production code (created or modified)

| File | Action | Responsibility |
|---|---|---|
| `c_model/include/noc/noc_req_out.hpp` | Modify | Add virtual `credit_avail(uint8_t) const` with default `return true` |
| `c_model/include/noc/noc_rsp_out.hpp` | Modify | Same — add `credit_avail` |
| `c_model/include/nmu/packetize.hpp` | Modify | Fix AW + W `header.last` per FlooNoC wormhole semantic |
| `c_model/include/nmu/vc_arb.hpp` | Create | `nmu::VcArb` — request-side VC arbiter (Mode A + B) |
| `c_model/include/nsu/vc_arb.hpp` | Create | `nsu::VcArb` — response-side VC arbiter (NSU mirror) |
| `c_model/tests/common/loopback_noc.hpp` | Modify | Per-VC credit counter + `credit_avail` override + introspection |

### Tests (created or modified)

| File | Action | Tests |
|---|---|---|
| `c_model/tests/nmu/test_packetize.cpp` | Modify | Update line 50 expectation; add `WHeaderLastMatchesWlast` test |
| `c_model/tests/nmu/test_vc_arb.cpp` | Create | 12 tests for `nmu::VcArb` (10 functional + 2 EXPECT_DEATH) |
| `c_model/tests/nsu/test_nsu_vc_arb.cpp` | Create | 5 tests for `nsu::VcArb` (target name distinct from nmu/test_vc_arb to avoid CMake target collision) |
| `c_model/tests/nmu/CMakeLists.txt` | Modify | Register `test_vc_arb` |
| `c_model/tests/nsu/CMakeLists.txt` | Modify | Register `test_vc_arb` |
| `c_model/tests/integration/test_request_response_loopback.cpp` | Modify | Insert VcArb between Packetize and LoopbackNoc adapter |
| `NEXT_STEPS.md` | Modify | Karpathy 4-lens summary + flip pointer to next round |

### Naming conventions (per `feedback_naming_no_camelcase` memory)

- Vars/methods: `snake_case` — `pending_w_routes_`, `read_write_split`, `credit_avail`
- Types: `PascalCase` — `VcArb`, `VcMode`
- Constants: `kCamelCase` for fixture-local (e.g., `kSrcId`); `SCREAMING_SNAKE` for codegen (e.g., `NUM_VC_MAX`)

### Drift gates (run before each commit)

```powershell
cd specgen
py -3 -m pytest -q
py -3 tools/codegen.py --check
py -3 tools/gen_inventory.py --check
cd ../c_model
cmake --build build
ctest --test-dir build -j 1
```

Expected at baseline (HEAD = `8e20ea9`): specgen 163 pass, codegen clean, inventory clean, ctest 302/302.

---

## Task 1: Extend `NocReqOut` / `NocRspOut` with `credit_avail`

**Goal:** Add a `credit_avail(uint8_t vc_id) const` virtual method to both interface bases with default `return true` impl. Preserves all 302 existing tests (all mocks inherit the default).

**Files:**
- Modify: `c_model/include/noc/noc_req_out.hpp`
- Modify: `c_model/include/noc/noc_rsp_out.hpp`
- Test: smoke-tested by build + 302 ctest pass (no new dedicated test — interface change verified via subsequent Task 2 LoopbackNoc override)

**Why no new test in Task 1**: Adding a virtual with default impl that returns `true` is mechanical. The behavior is only meaningful once an override exists (Task 2). Adding a "test that default returns true" would test the language, not behavior. Task 2's LoopbackNoc override IS the meaningful test.

- [ ] **Step 1: Modify `c_model/include/noc/noc_req_out.hpp`**

Replace the class body with:

```cpp
class NocReqOut {
 public:
  virtual ~NocReqOut() = default;

  // Push one request flit downstream. Returns false on backpressure.
  virtual bool push_flit(const Flit& flit) = 0;

  // Per-VC credit availability query (gem5 Garnet OutVcState /
  // BookSim BufferState::IsFullFor pattern). Caller SHOULD check
  // credit_avail(vc) before push_flit(flit_with_vc_id=vc) to model the
  // sender-side per-VC credit mirror. Default impl returns true: mocks
  // that don't track per-VC capacity remain valid (e.g., legacy
  // single-VC fixtures); overrides should return false when the
  // sender-side per-VC outstanding count has reached the configured
  // depth.
  virtual bool credit_avail(uint8_t /*vc_id*/) const { return true; }
};
```

- [ ] **Step 2: Modify `c_model/include/noc/noc_rsp_out.hpp`**

Replace the class body with:

```cpp
class NocRspOut {
 public:
  virtual ~NocRspOut() = default;

  // Push one response flit downstream. Returns false on backpressure.
  virtual bool push_flit(const Flit& flit) = 0;

  // Per-VC credit availability query — see noc_req_out.hpp for
  // semantics. Default impl returns true so legacy mocks compile.
  virtual bool credit_avail(uint8_t /*vc_id*/) const { return true; }
};
```

- [ ] **Step 3: Build full c_model**

Run: `cmake --build c_model/build`
Expected: clean build, no warnings.

- [ ] **Step 4: Run full ctest**

Run: `ctest --test-dir c_model/build -j 1`
Expected: `302 tests passed`.

- [ ] **Step 5: Commit**

```powershell
git add c_model/include/noc/noc_req_out.hpp c_model/include/noc/noc_rsp_out.hpp
git commit -m @'
feat(noc): add credit_avail to NocReqOut/NocRspOut interface

Add per-VC credit availability query to the abstract NoC sink interfaces,
matching the gem5 Garnet (OutVcState) / BookSim (BufferState::IsFullFor)
sender-side credit-mirror pattern. Default impl returns true so existing
mocks (LoopbackNoc adapters, test_loopback_latency, test_test_logger)
continue to compile unchanged.

VcArb (next round) will consult credit_avail before driving push_flit on
per-VC pending queues, enabling per-VC credit-gated round-robin
arbitration consistent with ni_signals.json noc_req_credit_i (width
NUM_VC).

Refs: docs/superpowers/specs/2026-06-03-vc-arb-multi-mode-design.md §5

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

Verify with `git log --oneline -1` — expected commit subject begins `feat(noc): add credit_avail`.

---

## Task 2: LoopbackNoc per-VC credit counter + `credit_avail` override

**Goal:** Add NMU-side per-VC outstanding counter to `LoopbackNoc`. `push_flit` increments the counter on success; NSU-side `pop_flit` decrements. `credit_avail(vc)` returns `counter[vc] < per_vc_depth_`. Same for the response side. Default `per_vc_depth_ = 16` (≥ any existing per-NSU queue depth used in tests) preserves all 302 existing tests.

**Files:**
- Modify: `c_model/tests/common/loopback_noc.hpp`
- Test: covered indirectly via Task 4 + a unit test added in this task

- [ ] **Step 1: Write failing test for per-VC credit counter**

Add a NEW test file `c_model/tests/common/test_loopback_noc_per_vc_credit.cpp`:

```cpp
#include "common/loopback_noc.hpp"
#include "common/scenario.hpp"
#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::testing::LoopbackNoc;

namespace {

Flit make_flit_on_vc(uint8_t vc_id, uint8_t dst_id, uint8_t axi_ch) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id",  vc_id);
    f.set_header_field("last",   1);
    return f;
}

}  // namespace

TEST(LoopbackNocPerVcCredit, DefaultDepthAllowsAtLeast16PushesOnSingleVc) {
    SCENARIO("LoopbackNoc: default per_vc_depth allows ≥16 successive pushes on VC=0 "
             "without exhausting credit (preserves backward compat for single-VC tests)");
    LoopbackNoc noc(/*req*/32, /*rsp*/32);
    for (int i = 0; i < 16; ++i) {
        EXPECT_TRUE(noc.req_out().credit_avail(0));
        ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_AW)));
    }
    // 16 in flight; default depth is 16 → credit now exhausted for VC=0
    EXPECT_FALSE(noc.req_out().credit_avail(0));
}

TEST(LoopbackNocPerVcCredit, PerVcDepthEnforcedIndependently) {
    SCENARIO("LoopbackNoc: per_vc_depth=2 → 2 pushes on VC=0 exhaust credit; "
             "VC=1 still has full credit (per-VC counters independent)");
    LoopbackNoc noc(/*req*/32, /*rsp*/32);
    noc.set_per_vc_depth(2);
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_AW)));
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_AW)));
    EXPECT_FALSE(noc.req_out().credit_avail(0));
    EXPECT_TRUE(noc.req_out().credit_avail(1));
}

TEST(LoopbackNocPerVcCredit, PopReleasesCredit) {
    SCENARIO("LoopbackNoc: pop_flit on NSU side decrements NMU per-VC counter, "
             "restoring credit_avail for the popped flit's VC");
    LoopbackNoc noc(/*req*/32, /*rsp*/32);
    noc.set_per_vc_depth(2);
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_AW)));
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_AW)));
    EXPECT_FALSE(noc.req_out().credit_avail(0));
    auto f = noc.req_in().pop_flit();
    ASSERT_TRUE(f.has_value());
    EXPECT_TRUE(noc.req_out().credit_avail(0));
    EXPECT_EQ(noc.nmu_req_per_vc_in_flight(0), 1u);
}

TEST(LoopbackNocPerVcCredit, RspSidePerVcCreditMirrorsReq) {
    SCENARIO("LoopbackNoc: NSU rsp side has symmetric per-VC credit; "
             "credit_avail on NSU rsp_out queries response-direction counter");
    LoopbackNoc noc(/*req*/32, /*rsp*/32);
    noc.set_per_vc_depth(2);
    ASSERT_TRUE(noc.rsp_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_B)));
    ASSERT_TRUE(noc.rsp_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_B)));
    EXPECT_FALSE(noc.rsp_out().credit_avail(0));
    auto f = noc.rsp_in().pop_flit();
    ASSERT_TRUE(f.has_value());
    EXPECT_TRUE(noc.rsp_out().credit_avail(0));
}
```

- [ ] **Step 2: Register new test in `c_model/tests/common/CMakeLists.txt`**

Append at the bottom of the file:

```cmake
add_cmodel_test(test_loopback_noc_per_vc_credit)
target_include_directories(test_loopback_noc_per_vc_credit PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

- [ ] **Step 3: Build — expect FAIL (credit_avail not overridden, no `set_per_vc_depth`, no `nmu_req_per_vc_in_flight`)**

Run: `cmake --build c_model/build`
Expected: compile error — `set_per_vc_depth` and `nmu_req_per_vc_in_flight` undefined on `LoopbackNoc`.

- [ ] **Step 4: Modify `c_model/tests/common/loopback_noc.hpp` — add per-VC credit counter**

Apply these targeted changes to the file:

**4a.** Add a static constexpr inside the `LoopbackNoc` class (place it just below `DST_ID_SPACE`):

```cpp
static constexpr std::size_t NUM_VC_MAX = 1u << ni::header::VC_ID_WIDTH;  // 8
static constexpr std::size_t kDefaultPerVcDepth = 16;
```

**4b.** Add new private data members near `req_q_depth_per_nsu_`:

```cpp
std::size_t                                  per_vc_depth_ = kDefaultPerVcDepth;
std::array<std::size_t, NUM_VC_MAX>          nmu_req_per_vc_in_flight_{};
std::array<std::size_t, NUM_VC_MAX>          nsu_rsp_per_vc_in_flight_{};
```

**4c.** Modify `NmuReqOutAdapter::push_flit` to gate on credit AND increment on success. Replace the entire `push_flit` body with:

```cpp
bool push_flit(const Flit& f) override {
    uint8_t vc  = static_cast<uint8_t>(f.get_header_field("vc_id"));
    uint8_t dst = static_cast<uint8_t>(f.get_header_field("dst_id"));
    int8_t  nsu = p->dst_to_nsu_[dst];
    assert(nsu >= 0 && "LoopbackNoc: unmapped dst_id");
    if (p->nmu_req_per_vc_in_flight_[vc] >= p->per_vc_depth_) return false;
    // Legacy global req delay path (single-NSU mode only; req_delay_
    // is gated by set_req_delay's num_nsu_==1 assert).
    if (p->req_delay_ > 0) {
        if (p->req_pipe_.size() + p->req_q_.size()
                >= p->req_q_depth_per_nsu_) {
            return false;
        }
        p->req_pipe_.emplace_back(f, p->req_delay_);
        ++p->nmu_req_per_vc_in_flight_[vc];
        return true;
    }
    if (p->nsu_req_q_[nsu].size() >= p->req_q_depth_per_nsu_) {
        return false;
    }
    p->nsu_req_q_[nsu].push_back(f);
    ++p->nmu_req_per_vc_in_flight_[vc];
    return true;
}
```

Also add the `credit_avail` override inside `NmuReqOutAdapter`:

```cpp
bool credit_avail(uint8_t vc_id) const override {
    return p->nmu_req_per_vc_in_flight_[vc_id] < p->per_vc_depth_;
}
```

**4d.** Modify `NsuReqInAdapter::pop_flit` — decrement the NMU per-VC counter on successful pop. Replace the entire `pop_flit` body with:

```cpp
std::optional<Flit> pop_flit() override {
    // Legacy global req delay path drains via req_q_ (single-NSU
    // only -- NSU_0 is the sole NSU when req_delay_ is non-zero).
    if (i == 0 && !p->req_q_.empty()) {
        Flit f = p->req_q_.front();
        p->req_q_.pop_front();
        uint8_t vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
        assert(p->nmu_req_per_vc_in_flight_[vc] > 0);
        --p->nmu_req_per_vc_in_flight_[vc];
        return f;
    }
    if (p->nsu_req_q_[i].empty()) return std::nullopt;
    Flit f = p->nsu_req_q_[i].front();
    p->nsu_req_q_[i].pop_front();
    uint8_t vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
    assert(p->nmu_req_per_vc_in_flight_[vc] > 0);
    --p->nmu_req_per_vc_in_flight_[vc];
    return f;
}
```

**4e.** Mirror the credit logic on the response side. Modify `NsuRspOutAdapter::push_flit`. Replace the entire body with:

```cpp
bool push_flit(const Flit& f) override {
    uint8_t vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
    if (p->nsu_rsp_per_vc_in_flight_[vc] >= p->per_vc_depth_) return false;
    const auto& cfg = p->nsu_latency_[i];
    std::size_t latency;
    if (cfg.is_random) {
        std::uniform_int_distribution<std::size_t> dist(cfg.min, cfg.max);
        latency = dist(p->rng_);
    } else {
        latency = cfg.value;
    }
    if (latency == 0) {
        // Fast path: bypass per-NSU delay queue.
        if (p->rsp_delay_ > 0) {
            // Legacy global rsp delay (single-NSU mode only).
            if (p->rsp_pipe_.size() + p->rsp_q_.size()
                    >= p->rsp_q_depth_total_) {
                return false;
            }
            p->rsp_pipe_.emplace_back(f, p->rsp_delay_);
        } else {
            if (p->rsp_q_.size() >= p->rsp_q_depth_total_) return false;
            p->rsp_q_.push_back(f);
        }
        ++p->nsu_rsp_per_vc_in_flight_[vc];
        return true;
    }
    // Per-NSU delay path. Aggregate capacity:
    //   total_delayed_rsp_count_ + rsp_pipe_.size() + rsp_q_.size()
    //       <= rsp_q_depth_total_
    if (p->total_delayed_rsp_count_ + p->rsp_pipe_.size()
            + p->rsp_q_.size() >= p->rsp_q_depth_total_) {
        return false;
    }
    p->nsu_rsp_delay_q_[i].push_back({f, latency});
    ++p->total_delayed_rsp_count_;
    ++p->nsu_rsp_per_vc_in_flight_[vc];
    return true;
}

bool credit_avail(uint8_t vc_id) const override {
    return p->nsu_rsp_per_vc_in_flight_[vc_id] < p->per_vc_depth_;
}
```

**4f.** Modify `NmuRspInAdapter::pop_flit` to decrement on successful pop. Replace its body with:

```cpp
std::optional<Flit> pop_flit() override {
    if (p->rsp_q_.empty()) return std::nullopt;
    Flit f = p->rsp_q_.front();
    p->rsp_q_.pop_front();
    uint8_t vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
    assert(p->nsu_rsp_per_vc_in_flight_[vc] > 0);
    --p->nsu_rsp_per_vc_in_flight_[vc];
    return f;
}
```

**4g.** Add public configuration + introspection methods near the other public methods (just below `set_random_seed`):

```cpp
void set_per_vc_depth(std::size_t depth) noexcept {
    assert(depth > 0);
    per_vc_depth_ = depth;
}
std::size_t per_vc_depth() const noexcept { return per_vc_depth_; }
std::size_t nmu_req_per_vc_in_flight(uint8_t vc_id) const noexcept {
    return nmu_req_per_vc_in_flight_[vc_id];
}
std::size_t nsu_rsp_per_vc_in_flight(uint8_t vc_id) const noexcept {
    return nsu_rsp_per_vc_in_flight_[vc_id];
}
```

- [ ] **Step 5: Build — expect PASS**

Run: `cmake --build c_model/build`
Expected: clean build.

- [ ] **Step 6: Run the new test — expect 4/4 PASS**

Run: `ctest --test-dir c_model/build -R LoopbackNocPerVcCredit -V`
Expected: `4 tests passed`.

- [ ] **Step 7: Run full ctest — expect 306/306 (302 prior + 4 new)**

Run: `ctest --test-dir c_model/build -j 1`
Expected: `306 tests passed`.

- [ ] **Step 8: Commit**

```powershell
git add c_model/tests/common/loopback_noc.hpp `
        c_model/tests/common/test_loopback_noc_per_vc_credit.cpp `
        c_model/tests/common/CMakeLists.txt
git commit -m @'
feat(tests/common/loopback_noc): per-VC credit counter + credit_avail impl

Add NMU-side and NSU-side per-VC outstanding counters
(nmu_req_per_vc_in_flight_, nsu_rsp_per_vc_in_flight_) to LoopbackNoc.
push_flit gates on counter < per_vc_depth_ AND existing queue capacity;
the counter is incremented on successful enqueue and decremented when
pop_flit on the receiving adapter dequeues. credit_avail(vc) overrides
on NmuReqOutAdapter + NsuRspOutAdapter return counter < depth.

Default per_vc_depth_ = 16 ≥ all per-NSU queue depths used by existing
fixtures; legacy tests do not hit per-VC credit pressure. Tests opt in
to smaller depths via set_per_vc_depth().

Per-VC introspection (nmu_req_per_vc_in_flight / nsu_rsp_per_vc_in_flight
getters + per_vc_depth getter) supports per-VC assertion in subsequent
VcArb tests.

4 new dedicated tests verify default-depth backward compat, per-VC
independence, pop-side credit release, and rsp-side mirror. ctest:
302 → 306.

Refs: docs/superpowers/specs/2026-06-03-vc-arb-multi-mode-design.md §6

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 3: Fix `nmu/packetize.hpp` `header.last` per FlooNoC wormhole semantic

**Goal:** AW flit `header.last` 1→0 (AW is wormhole packet START); W flit `header.last` 1→`b.last` (W's `wlast` marks packet END). NSU side (B, R) and NMU AR are already correct per FlooNoC (single-flit packets / ROB-based response). Add 1 new test, update 1 existing expectation.

**Files:**
- Modify: `c_model/include/nmu/packetize.hpp` (lines 109, 138)
- Modify: `c_model/tests/nmu/test_packetize.cpp` (line 50)

- [ ] **Step 1: Add failing test `WHeaderLastMatchesWlast` to `test_packetize.cpp`**

Append immediately after `MultiOutstandingAwInterleavedW` (after the existing test that pushes 2 AWs + 2 Ws), insert:

```cpp
TEST(NmuPacketize, WHeaderLastMatchesWlast) {
  SCENARIO("NMU Packetize: header.last on W flits matches payload.wlast — "
           "intermediate W beats stamp 0, terminal beat stamps 1 "
           "(FlooNoC wormhole packet boundary semantic; "
           "fixes pre-existing bug where every W flit stamped header.last=1)");
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  // Push 1 AW + 3 W beats; only the 3rd has wlast=1.
  ASSERT_TRUE(pkt.push_aw(make_aw(0x07, 0x340000, /*len*/2)));
  ASSERT_TRUE(pkt.push_w(make_w(0xFFFFFFFF, /*last*/false)));
  ASSERT_TRUE(pkt.push_w(make_w(0xFFFFFFFF, /*last*/false)));
  ASSERT_TRUE(pkt.push_w(make_w(0xFFFFFFFF, /*last*/true)));

  // Discard AW flit; verify W beats stamp header.last == payload.wlast.
  noc.req_in().pop_flit();  // AW
  for (int i = 0; i < 3; ++i) {
    auto f = noc.req_in().pop_flit();
    ASSERT_TRUE(f.has_value());
    uint64_t expected_last = (i == 2) ? 1u : 0u;
    EXPECT_EQ(f->get_header_field("last"), expected_last)
        << "W beat " << i << ": header.last expected " << expected_last;
    EXPECT_EQ(f->get_payload_field("W", "wlast"), expected_last);
  }
}
```

- [ ] **Step 2: Update existing AW expectation in `test_packetize.cpp:50`**

In `TEST(NmuPacketize, PushAwEmitsFlitWithCorrectFields)`, change:

```cpp
EXPECT_EQ(f.get_header_field("last"),     1u);
```
to:

```cpp
EXPECT_EQ(f.get_header_field("last"),     0u);  // AW starts wormhole packet (FlooNoC)
```

Also update the SCENARIO description on line 37 to match:

```cpp
SCENARIO("NMU Packetize: push_aw stamps src_id/axi_ch=AW/vc=0/last=0/awid/awaddr on emitted flit");
```

- [ ] **Step 3: Build + run packetize test — expect FAIL (production header.last still =1)**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NmuPacketize -V`
Expected: `WHeaderLastMatchesWlast` FAILS — first W beat reports `header.last=1` but expected 0. `PushAwEmitsFlitWithCorrectFields` FAILS — `header.last=1` but expected 0.

- [ ] **Step 4: Apply production fix to `nmu/packetize.hpp`**

In `c_model/include/nmu/packetize.hpp`, change line 109 (inside `push_aw_with_meta`):

```cpp
f.set_header_field("last",    1);
```
to:

```cpp
f.set_header_field("last",    0);  // AW starts wormhole packet (FlooNoC pattern)
```

Change line 138 (inside `push_w`):

```cpp
f.set_header_field("last",    1);
```
to:

```cpp
f.set_header_field("last",    b.last ? 1u : 0u);  // W's wlast ends wormhole packet (FlooNoC)
```

Also update the file-header comment block: change line 15

```
//   last        — always 1 (1 beat = 1 flit = 1 packet)
```

to:

```
//   last        — wormhole packet boundary marker (FlooNoC pattern):
//                 AW=0 (start of AW+W wormhole packet);
//                 W=wlast (end on last W beat of burst);
//                 AR=1 (single-flit read request packet).
```

- [ ] **Step 5: Build + run packetize test — expect PASS**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NmuPacketize -V`
Expected: all NmuPacketize tests PASS (existing 8 + new 1 = 9).

- [ ] **Step 6: Run full ctest — expect 307/307**

Run: `ctest --test-dir c_model/build -j 1`
Expected: `307 tests passed` (306 prior + 1 new = 307).

- [ ] **Step 7: Commit**

```powershell
git add c_model/include/nmu/packetize.hpp c_model/tests/nmu/test_packetize.cpp
git commit -m @'
fix(nmu/packetize): correct AW + W header.last per FlooNoC wormhole semantic

AW header.last was hardcoded to 1; per FlooNoC floo_axi_chimney.sv:568-577,
AW is the START of an AW+W wormhole packet so header.last = 0. W header.last
was also hardcoded to 1; per floo_axi_chimney.sv:584-591, header.last must
equal axi_req_in.w.last so the wormhole arbiter (floo_wormhole_arbiter.sv
LockIn=1, releases on last_out & ready_i) sees the correct packet
boundary. Without this fix, downstream wormhole routing would
prematurely release the VC lock and AW2's W beats could interleave AW1's
burst — AXI4 violation.

Scope: nmu/packetize.hpp lines 109 (AW) + 138 (W). AR (line 156) remains
1 (single-flit packet). NSU B (nsu/packetize.hpp:71) + R (:92) remain 1
— B is single-flit; R uses ROB not wormhole per floo_axi_chimney.sv:624-633
("no reason to do wormhole routing for R bursts").

1 new test (WHeaderLastMatchesWlast) verifies per-beat W stamping;
1 existing test expectation (PushAwEmitsFlitWithCorrectFields) updated
from 1u to 0u. ctest: 306 → 307.

Refs: docs/superpowers/specs/2026-06-03-vc-arb-multi-mode-design.md §12

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 4: `nmu::VcArb` class + 12 tests

**Goal:** Build `c_model/include/nmu/vc_arb.hpp` with Mode A (ReadWriteSplit) + Mode B (MultiCandidate), `tick()`-driven round-robin, per-VC credit gating, W-follows-AW invariant. 10 functional tests + 2 EXPECT_DEATH tests in `c_model/tests/nmu/test_vc_arb.cpp`.

**Files:**
- Create: `c_model/include/nmu/vc_arb.hpp`
- Create: `c_model/tests/nmu/test_vc_arb.cpp`
- Modify: `c_model/tests/nmu/CMakeLists.txt`

This is the largest task. To stay bite-sized, build the class skeleton + 1 test first, then add each remaining test+impl pair in subsequent steps.

- [ ] **Step 1: Create `c_model/include/nmu/vc_arb.hpp` with class skeleton**

```cpp
#pragma once
// NMU virtual channel arbiter. Decorator pattern over NocReqOut: receives
// packetized flits from nmu::Packetize, decides which VC each flit goes
// into (per-axi_ch mapping), enqueues into a per-VC pending queue, and
// drains to the wrapped downstream via tick() using credit-gated
// round-robin.
//
// Two modes (compile-time selected via factory):
//   Mode A (ReadWriteSplit, default): AW/W → write_vc; AR → read_vc.
//   Mode B (MultiCandidate): per-axi_ch candidate VC list; first VC with
//     pending space AND downstream credit wins.
//
// W-follows-AW invariant (both modes): all W beats of a burst route to
// the same VC as their preceding AW. Implementation mirrors Packetize's
// w_meta_fifo_ via a std::deque<uint8_t> pending_w_routes_ (push on AW,
// pop on W with payload.W.wlast=1).
//
// NUM_VC=1 degenerate behavior: both modes route everything to VC=0 and
// are observationally identical to the prior-round single-VC pipeline.
//
// References:
//   docs/superpowers/specs/2026-06-03-vc-arb-multi-mode-design.md
//   FlooNoC floo_wormhole_arbiter.sv (output-port wormhole lock)
//   FlooNoC floo_vc_arbiter.sv (VC arbiter without wormhole lock)
//   gem5 Garnet OutputUnit::has_credit / OutVcState::m_credit_count
#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include "noc/noc_req_out.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace ni::cmodel::nmu {

enum class VcMode {
    ReadWriteSplit,
    MultiCandidate,
};

class VcArb : public noc::NocReqOut {
public:
    static constexpr std::size_t NUM_VC_MAX   = 1u << ni::header::VC_ID_WIDTH;  // 8
    static constexpr std::size_t AXI_CH_COUNT = 5;  // AW, W, AR, B, R (B/R unused on NMU)
    static constexpr std::size_t kDefaultPendingDepth = 4;

    static VcArb read_write_split(noc::NocReqOut& downstream,
                                  std::size_t num_vc,
                                  uint8_t write_vc,
                                  uint8_t read_vc,
                                  std::size_t pending_depth = kDefaultPendingDepth) {
        std::array<std::vector<uint8_t>, AXI_CH_COUNT> empty_candidates{};
        return VcArb(downstream, num_vc, VcMode::ReadWriteSplit,
                     write_vc, read_vc, std::move(empty_candidates), pending_depth);
    }

    static VcArb multi_candidate(noc::NocReqOut& downstream,
                                 std::size_t num_vc,
                                 std::array<std::vector<uint8_t>, AXI_CH_COUNT> candidate_vcs,
                                 std::size_t pending_depth = kDefaultPendingDepth) {
        return VcArb(downstream, num_vc, VcMode::MultiCandidate,
                     /*write_vc*/0, /*read_vc*/0, std::move(candidate_vcs), pending_depth);
    }

    // NocReqOut decorator interface
    bool push_flit(const Flit& flit) override;
    bool credit_avail(uint8_t vc_id) const override;

    void tick();

    // Test introspection
    std::size_t pending_size(uint8_t vc_id) const noexcept { return pending_[vc_id].size(); }
    uint8_t     round_robin_ptr() const noexcept { return round_robin_ptr_; }
    std::size_t pending_w_routes_size() const noexcept { return pending_w_routes_.size(); }

private:
    VcArb(noc::NocReqOut& downstream,
          std::size_t num_vc,
          VcMode mode,
          uint8_t write_vc,
          uint8_t read_vc,
          std::array<std::vector<uint8_t>, AXI_CH_COUNT> candidate_vcs,
          std::size_t pending_depth)
        : downstream_(downstream),
          num_vc_(num_vc),
          mode_(mode),
          write_vc_(write_vc),
          read_vc_(read_vc),
          candidate_vcs_(std::move(candidate_vcs)),
          pending_depth_(pending_depth) {
        assert(num_vc_ >= 1 && num_vc_ <= NUM_VC_MAX);
        assert(write_vc_ < num_vc_);
        assert(read_vc_ < num_vc_);
    }

    std::optional<uint8_t> select_vc_for_axi_ch(uint8_t axi_ch);

    noc::NocReqOut&                                  downstream_;
    std::size_t                                      num_vc_;
    VcMode                                           mode_;
    uint8_t                                          write_vc_;
    uint8_t                                          read_vc_;
    std::array<std::vector<uint8_t>, AXI_CH_COUNT>   candidate_vcs_;
    std::array<std::deque<Flit>, NUM_VC_MAX>         pending_;
    std::size_t                                      pending_depth_;
    uint8_t                                          round_robin_ptr_ = 0;
    std::deque<uint8_t>                              pending_w_routes_;
};

inline std::optional<uint8_t> VcArb::select_vc_for_axi_ch(uint8_t axi_ch) {
    if (num_vc_ == 1) return uint8_t{0};

    if (axi_ch == ni::AXI_CH_W) {
        if (pending_w_routes_.empty()) {
            assert(false && "nmu::VcArb::push_flit: W arrived with empty "
                            "pending_w_routes_ — Packetize w_meta_fifo invariant violated "
                            "(W before AW); check upstream Rob credit gate or "
                            "AxiSlavePort routing");
            std::abort();
        }
        return pending_w_routes_.front();
    }

    if (mode_ == VcMode::ReadWriteSplit) {
        if (axi_ch == ni::AXI_CH_AW) return write_vc_;
        if (axi_ch == ni::AXI_CH_AR) return read_vc_;
        return std::nullopt;
    }

    // Mode B: MultiCandidate
    for (uint8_t vc : candidate_vcs_[axi_ch]) {
        if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) {
            return vc;
        }
    }
    return std::nullopt;
}

inline bool VcArb::push_flit(const Flit& flit) {
    uint8_t axi_ch = static_cast<uint8_t>(flit.get_header_field("axi_ch"));
    auto vc_opt = select_vc_for_axi_ch(axi_ch);
    if (!vc_opt.has_value()) return false;
    uint8_t vc_id = *vc_opt;
    if (pending_[vc_id].size() >= pending_depth_) return false;

    // Update W-follows-AW deque only after pass conditions (atomicity)
    if (axi_ch == ni::AXI_CH_AW) {
        pending_w_routes_.push_back(vc_id);
    } else if (axi_ch == ni::AXI_CH_W) {
        if (flit.get_payload_field("W", "wlast") != 0) {
            pending_w_routes_.pop_front();
        }
    }

    Flit stamped = flit;
    stamped.set_header_field("vc_id", vc_id);
    pending_[vc_id].push_back(stamped);
    return true;
}

inline bool VcArb::credit_avail(uint8_t vc_id) const {
    return pending_[vc_id].size() < pending_depth_;
}

inline void VcArb::tick() {
    for (std::size_t k = 0; k < num_vc_; ++k) {
        uint8_t vc = static_cast<uint8_t>((round_robin_ptr_ + k) % num_vc_);
        if (!pending_[vc].empty() && downstream_.credit_avail(vc)) {
            bool ok = downstream_.push_flit(pending_[vc].front());
            assert(ok && "nmu::VcArb::tick: downstream returned credit_avail=true "
                         "but push_flit refused — protocol violation, downstream "
                         "must not lie about credit availability");
            if (!ok) std::abort();  // belt-and-braces for NDEBUG
            pending_[vc].pop_front();
            round_robin_ptr_ = static_cast<uint8_t>((vc + 1) % num_vc_);
            return;
        }
    }
}

}  // namespace ni::cmodel::nmu
```

- [ ] **Step 2: Create `c_model/tests/nmu/test_vc_arb.cpp` with test fixture + Test 1 (Degenerate NUM_VC=1)**

```cpp
#include "nmu/vc_arb.hpp"
#include "common/loopback_noc.hpp"
#include "common/scenario.hpp"
#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
#include <array>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::nmu::VcArb;
using ni::cmodel::nmu::VcMode;
using ni::cmodel::testing::LoopbackNoc;

namespace {

Flit make_flit(uint8_t axi_ch, uint8_t dst_id = 0, uint8_t initial_vc = 0,
               uint64_t wlast = 0) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id",  initial_vc);
    f.set_header_field("src_id", 0x12);
    f.set_header_field("last",   1);  // legacy; VcArb does not consult header.last
    if (axi_ch == ni::AXI_CH_W) {
        f.set_payload_field("W", "wlast", wlast);
    }
    return f;
}

}  // namespace

TEST(NmuVcArb, Degenerate_NumVc1_AllModesPassthrough) {
    SCENARIO("NMU VcArb: NUM_VC=1, both Mode A (read_write_split) and Mode B "
             "(multi_candidate) route every axi_ch → VC=0; behavior "
             "observationally identical to direct Packetize → LoopbackNoc");

    // Mode A
    {
        LoopbackNoc noc(/*req*/32, /*rsp*/32);
        auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/1, 0, 0);
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/1)));
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
        EXPECT_EQ(arb.pending_size(0), 3u);
        arb.tick(); arb.tick(); arb.tick();
        EXPECT_EQ(arb.pending_size(0), 0u);
        for (int i = 0; i < 3; ++i) {
            auto f = noc.req_in().pop_flit();
            ASSERT_TRUE(f.has_value());
            EXPECT_EQ(f->get_header_field("vc_id"), 0u);
        }
    }
    // Mode B — even with multi_candidate, num_vc=1 forces VC=0.
    {
        LoopbackNoc noc(/*req*/32, /*rsp*/32);
        std::array<std::vector<uint8_t>, VcArb::AXI_CH_COUNT> candidates{};
        candidates[ni::AXI_CH_AW] = {0};
        candidates[ni::AXI_CH_W]  = {0};
        candidates[ni::AXI_CH_AR] = {0};
        auto arb = VcArb::multi_candidate(noc.req_out(), /*num_vc=*/1, candidates);
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/1)));
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
        EXPECT_EQ(arb.pending_size(0), 3u);
        arb.tick(); arb.tick(); arb.tick();
        EXPECT_EQ(arb.pending_size(0), 0u);
        for (int i = 0; i < 3; ++i) {
            auto f = noc.req_in().pop_flit();
            ASSERT_TRUE(f.has_value());
            EXPECT_EQ(f->get_header_field("vc_id"), 0u);
        }
    }
}
```

- [ ] **Step 3: Register `test_vc_arb` in `c_model/tests/nmu/CMakeLists.txt`**

Append at the bottom:

```cmake
add_cmodel_test(test_vc_arb)
target_include_directories(test_vc_arb PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

- [ ] **Step 4: Build + run Test 1 — expect PASS**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NmuVcArb.Degenerate -V`
Expected: 1/1 PASS.

- [ ] **Step 5: Add Test 2 — ReadWriteSplit splits AW vs AR**

Append to `test_vc_arb.cpp`:

```cpp
TEST(NmuVcArb, ReadWriteSplit_AW_AR_GoSeparateVcs) {
    SCENARIO("NMU VcArb Mode A NUM_VC=2: AW → write_vc=0, AR → read_vc=1; "
             "verify pending queues + downstream stamps");
    LoopbackNoc noc(/*req*/32, /*rsp*/32);
    auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/2,
                                       /*write_vc=*/0, /*read_vc=*/1);

    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_EQ(arb.pending_size(0), 1u);  // AW landed on write_vc
    EXPECT_EQ(arb.pending_size(1), 1u);  // AR landed on read_vc

    arb.tick(); arb.tick();
    auto f0 = noc.req_in().pop_flit(); ASSERT_TRUE(f0.has_value());
    auto f1 = noc.req_in().pop_flit(); ASSERT_TRUE(f1.has_value());
    // Round-robin starts at 0 → VC=0 (AW) drains first, then VC=1 (AR).
    EXPECT_EQ(f0->get_header_field("axi_ch"), ni::AXI_CH_AW);
    EXPECT_EQ(f0->get_header_field("vc_id"),  0u);
    EXPECT_EQ(f1->get_header_field("axi_ch"), ni::AXI_CH_AR);
    EXPECT_EQ(f1->get_header_field("vc_id"),  1u);
}
```

- [ ] **Step 6: Build + run — expect PASS**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NmuVcArb -V`
Expected: 2/2 PASS.

- [ ] **Step 7: Add Test 3 — MultiCandidate HoL avoidance**

Append to `test_vc_arb.cpp`:

```cpp
TEST(NmuVcArb, MultiCandidate_HoLAvoidance) {
    SCENARIO("NMU VcArb Mode B NUM_VC=4: AW candidates {0,1}, AR candidates {2,3}; "
             "saturate VC=0 pending → next AW picks VC=1, avoiding head-of-line block");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    std::array<std::vector<uint8_t>,
               VcArb::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_AW] = {0, 1};
    candidates[ni::AXI_CH_W]  = {0, 1};  // W follows AW via pending_w_routes_
    candidates[ni::AXI_CH_AR] = {2, 3};
    auto arb = VcArb::multi_candidate(noc.req_out(), /*num_vc=*/4,
                                      candidates, /*pending_depth=*/2);

    // Fill VC=0 pending to capacity (2 AWs land on VC=0).
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
    EXPECT_EQ(arb.pending_size(0), 2u);
    EXPECT_EQ(arb.pending_size(1), 0u);

    // Next AW: VC=0 full → candidate fallback picks VC=1.
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
    EXPECT_EQ(arb.pending_size(1), 1u);
}
```

- [ ] **Step 8: Build + run — expect PASS**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NmuVcArb -V`
Expected: 3/3 PASS.

- [ ] **Step 9: Add Test 4 — WFollowsAW invariant enforced across multiple outstanding AWs**

Append:

```cpp
TEST(NmuVcArb, WFollowsAW_InvariantEnforced) {
    SCENARIO("NMU VcArb Mode B NUM_VC=4: AW1 lands VC=0, AW2 lands VC=1 (Mode B "
             "candidate fallback). All W beats of burst 1 route to VC=0, all W "
             "beats of burst 2 route to VC=1 — even though Mode B candidate list "
             "for W is {0,1}, pending_w_routes_ forces correct routing.");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    std::array<std::vector<uint8_t>,
               VcArb::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_AW] = {0, 1};
    candidates[ni::AXI_CH_W]  = {0, 1};
    candidates[ni::AXI_CH_AR] = {2, 3};
    auto arb = VcArb::multi_candidate(noc.req_out(), /*num_vc=*/4,
                                      candidates, /*pending_depth=*/4);

    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));  // VC=0
    // Saturate VC=0 with extra non-W traffic so the next AW must spill to VC=1.
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));  // VC=0 (still room)
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));  // VC=0
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));  // VC=0 (full now)
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));  // VC=1 (overflow)
    EXPECT_EQ(arb.pending_size(0), 4u);
    EXPECT_EQ(arb.pending_size(1), 1u);
    EXPECT_EQ(arb.pending_w_routes_size(), 5u);

    // Drain all AWs.
    for (int i = 0; i < 5; ++i) { arb.tick(); auto _ = noc.req_in().pop_flit(); }

    // Now push 2 W beats for AW1 (VC=0) and 2 W beats for AW2 to AW4 (VC=0)
    // and 1 W beat for AW5 (VC=1) — all single-beat bursts (wlast=1).
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/1)));
    }
    // First 4 W beats correspond to AWs that landed on VC=0; 5th to AW on VC=1.
    EXPECT_EQ(arb.pending_size(0), 4u);
    EXPECT_EQ(arb.pending_size(1), 1u);
    EXPECT_EQ(arb.pending_w_routes_size(), 0u);  // all popped on wlast=1
}
```

- [ ] **Step 10: Build + run — expect PASS**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NmuVcArb -V`
Expected: 4/4 PASS.

- [ ] **Step 11: Add Test 5 — wlast read from payload, not header**

Append:

```cpp
TEST(NmuVcArb, WlastFromPayloadNotHeader) {
    SCENARIO("NMU VcArb: pending_w_routes_ pops based on payload.W.wlast, "
             "NOT header.last. Push AW + 3 W beats; only the 3rd has "
             "payload.wlast=1; verify deque only pops on the 3rd.");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/2, 0, 1);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
    EXPECT_EQ(arb.pending_w_routes_size(), 1u);

    // Beat 1: payload.wlast=0 (intermediate W beat); even if header.last=1 in
    // the input flit (legacy bug shape), pending_w_routes_ MUST NOT pop.
    Flit w1; w1.set_header_field("axi_ch", ni::AXI_CH_W);
            w1.set_header_field("last", 1);   // bait: legacy bug-shape header.last
            w1.set_payload_field("W", "wlast", 0);
    ASSERT_TRUE(arb.push_flit(w1));
    EXPECT_EQ(arb.pending_w_routes_size(), 1u) << "wlast=0 → must not pop";

    Flit w2; w2.set_header_field("axi_ch", ni::AXI_CH_W);
            w2.set_header_field("last", 1);
            w2.set_payload_field("W", "wlast", 0);
    ASSERT_TRUE(arb.push_flit(w2));
    EXPECT_EQ(arb.pending_w_routes_size(), 1u);

    Flit w3; w3.set_header_field("axi_ch", ni::AXI_CH_W);
            w3.set_header_field("last", 1);
            w3.set_payload_field("W", "wlast", 1);  // genuine burst end
    ASSERT_TRUE(arb.push_flit(w3));
    EXPECT_EQ(arb.pending_w_routes_size(), 0u);
}
```

- [ ] **Step 12: Build + run — expect PASS**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NmuVcArb -V`
Expected: 5/5 PASS.

- [ ] **Step 13: Add Test 6 — Round-robin fairness across 4 VCs**

Append:

```cpp
TEST(NmuVcArb, RoundRobinFairness_AllVcsServiced_NoStarvation) {
    SCENARIO("NMU VcArb NUM_VC=4: 4 ARs pre-routed to different VCs via Mode B "
             "candidates {0,1,2,3}; tick 4 times → 4 flits emerge in "
             "round-robin order (VC0, VC1, VC2, VC3 successively).");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    std::array<std::vector<uint8_t>,
               VcArb::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_AW] = {0};
    candidates[ni::AXI_CH_W]  = {0};
    // Each candidate list of size 1 → AR_i always picks VC=i via successive pushes.
    candidates[ni::AXI_CH_AR] = {0, 1, 2, 3};
    auto arb = VcArb::multi_candidate(noc.req_out(), /*num_vc=*/4,
                                      candidates, /*pending_depth=*/1);
    // Push 1 AR — picks VC=0 (first candidate with space).
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    // VC=0 now full; push AR → VC=1.
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_EQ(arb.pending_size(0), 1u);
    EXPECT_EQ(arb.pending_size(1), 1u);
    EXPECT_EQ(arb.pending_size(2), 1u);
    EXPECT_EQ(arb.pending_size(3), 1u);

    arb.tick(); arb.tick(); arb.tick(); arb.tick();
    EXPECT_EQ(arb.pending_size(0), 0u);
    EXPECT_EQ(arb.pending_size(1), 0u);
    EXPECT_EQ(arb.pending_size(2), 0u);
    EXPECT_EQ(arb.pending_size(3), 0u);

    for (uint8_t expected_vc = 0; expected_vc < 4; ++expected_vc) {
        auto f = noc.req_in().pop_flit();
        ASSERT_TRUE(f.has_value());
        EXPECT_EQ(f->get_header_field("vc_id"), expected_vc);
    }
}
```

- [ ] **Step 14: Build + run — expect PASS**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NmuVcArb -V`
Expected: 6/6 PASS.

- [ ] **Step 15: Add Test 7 — Credit gating: tick idle when all VCs blocked**

Append:

```cpp
TEST(NmuVcArb, CreditGating_TickIdleWhenAllVcsBlocked) {
    SCENARIO("NMU VcArb Mode A NUM_VC=1: all flits → VC=0; LoopbackNoc "
             "per_vc_depth=1 caps downstream credit. Push 4 AR flits, tick "
             "drains 1 (downstream credit consumed). Subsequent tick is idle "
             "(VC=0 has pending=3 but downstream credit_avail=false). Pop "
             "downstream → credit returns → next tick drains 1 more.");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    noc.set_per_vc_depth(1);  // downstream credit = 1 for VC=0
    auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/1, 0, 0,
                                       /*pending_depth=*/8);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_EQ(arb.pending_size(0), 4u);

    // First tick: VC=0 has pending + downstream credit → 1 flit out.
    arb.tick();
    EXPECT_EQ(arb.pending_size(0), 3u);
    EXPECT_EQ(noc.nmu_req_per_vc_in_flight(0), 1u);

    // Downstream credit exhausted (per_vc_depth=1) → next tick is idle.
    arb.tick();
    EXPECT_EQ(arb.pending_size(0), 3u) << "tick must be idle, no spurious push";
    EXPECT_EQ(noc.nmu_req_per_vc_in_flight(0), 1u);

    // Pop downstream → credit returns → next tick drains.
    auto f = noc.req_in().pop_flit(); ASSERT_TRUE(f.has_value());
    EXPECT_EQ(noc.nmu_req_per_vc_in_flight(0), 0u);
    arb.tick();
    EXPECT_EQ(arb.pending_size(0), 2u);
}
```

- [ ] **Step 16: Build + run — expect PASS**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NmuVcArb -V`
Expected: 7/7 PASS.

- [ ] **Step 17: Add Test 8 — Backpressure chain propagates to upstream**

Append:

```cpp
TEST(NmuVcArb, BackpressureChain_VcArbToUpstream) {
    SCENARIO("NMU VcArb: VcArb pending_depth=2, single VC. After 2 pushes, "
             "VcArb's own pending_[0] is full → push_flit returns false; "
             "credit_avail(0) also returns false. Backpressure visible to "
             "upstream Packetize.");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/1, 0, 0,
                                       /*pending_depth=*/2);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_EQ(arb.pending_size(0), 2u);
    EXPECT_FALSE(arb.credit_avail(0));
    EXPECT_FALSE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
}
```

- [ ] **Step 18: Build + run — expect PASS**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NmuVcArb -V`
Expected: 8/8 PASS.

- [ ] **Step 19: Add Test 9 — EnabledMode interop with prior-round Packetize**

Append:

```cpp
TEST(NmuVcArb, EnabledModeMixedWith_PriorRoundTests) {
    SCENARIO("NMU VcArb decorator is transparent to nmu::Packetize: wire "
             "Packetize → VcArb → LoopbackNoc with NUM_VC=1 and verify "
             "Packetize-emitted AW + W flits arrive intact at NSU side.");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/1, 0, 0);
    ni::cmodel::nmu::Packetize pkt(arb, /*src_id=*/0x12);

    ni::cmodel::axi::AwBeat aw{};
    aw.id = 0x07; aw.addr = 0x340000; aw.len = 0; aw.size = 5;
    aw.burst = ni::cmodel::axi::Burst::INCR;
    ASSERT_TRUE(pkt.push_aw(aw));

    ni::cmodel::axi::WBeat w{};
    for (int i = 0; i < 32; ++i) w.data[i] = static_cast<uint8_t>(i);
    w.strb = 0xFFFFFFFF; w.last = true;
    ASSERT_TRUE(pkt.push_w(w));

    arb.tick();
    arb.tick();
    auto f_aw = noc.req_in().pop_flit(); ASSERT_TRUE(f_aw.has_value());
    auto f_w  = noc.req_in().pop_flit(); ASSERT_TRUE(f_w.has_value());
    EXPECT_EQ(f_aw->get_header_field("axi_ch"), ni::AXI_CH_AW);
    EXPECT_EQ(f_w->get_header_field("axi_ch"),  ni::AXI_CH_W);
    EXPECT_EQ(f_aw->get_header_field("dst_id"), 0x34u);
    EXPECT_EQ(f_w->get_header_field("dst_id"),  0x34u);
}
```

Also add include at the top of `test_vc_arb.cpp`:

```cpp
#include "nmu/packetize.hpp"
#include "axi/types.hpp"
```

- [ ] **Step 20: Build + run — expect PASS**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NmuVcArb -V`
Expected: 9/9 PASS.

- [ ] **Step 21: Add Test 10 — `WHeaderLastMatchesWlast` (verify §12 fix surfaces through VcArb decorator)**

Append:

```cpp
TEST(NmuVcArb, WHeaderLastMatchesWlast) {
    SCENARIO("NMU VcArb: header.last on W flits emitted via Packetize → VcArb "
             "→ downstream matches payload.wlast (verifies §12 packetize fix "
             "is preserved end-to-end through the decorator pipeline).");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/1, 0, 0);
    ni::cmodel::nmu::Packetize pkt(arb, /*src_id=*/0x12);

    ni::cmodel::axi::AwBeat aw{};
    aw.id = 0x07; aw.addr = 0x340000; aw.len = 2; aw.size = 5;
    aw.burst = ni::cmodel::axi::Burst::INCR;
    ASSERT_TRUE(pkt.push_aw(aw));

    auto make_w = [](bool last) {
        ni::cmodel::axi::WBeat w{};
        for (int i = 0; i < 32; ++i) w.data[i] = 0;
        w.strb = 0xFFFFFFFF; w.last = last; return w;
    };
    ASSERT_TRUE(pkt.push_w(make_w(false)));
    ASSERT_TRUE(pkt.push_w(make_w(false)));
    ASSERT_TRUE(pkt.push_w(make_w(true)));

    // Drain AW + 3 W flits through tick().
    for (int i = 0; i < 4; ++i) arb.tick();

    noc.req_in().pop_flit();  // discard AW
    for (int i = 0; i < 3; ++i) {
        auto f = noc.req_in().pop_flit(); ASSERT_TRUE(f.has_value());
        uint64_t expected = (i == 2) ? 1u : 0u;
        EXPECT_EQ(f->get_header_field("last"), expected);
        EXPECT_EQ(f->get_payload_field("W", "wlast"), expected);
    }
}
```

- [ ] **Step 22: Build + run — expect PASS**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NmuVcArb -V`
Expected: 10/10 PASS.

- [ ] **Step 23: Add Test 11 — EXPECT_DEATH on W before AW**

Append:

```cpp
TEST(NmuVcArbDeath, WFollowsAW_WBeforeAW_DeathTest) {
    SCENARIO("NMU VcArb: push_flit(W) before any push_flit(AW) violates the "
             "W-follows-AW invariant; pending_w_routes_ is empty so VcArb "
             "must assert+abort instead of UB on front().");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/2, 0, 1);
    EXPECT_DEATH({
        Flit w; w.set_header_field("axi_ch", ni::AXI_CH_W);
                w.set_payload_field("W", "wlast", 1);
        arb.push_flit(w);
    }, "nmu::VcArb::push_flit: W arrived with empty pending_w_routes_");
}
```

- [ ] **Step 24: Build + run death test — expect PASS**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NmuVcArbDeath -V`
Expected: 1/1 PASS.

- [ ] **Step 25: Add Test 12 — EXPECT_DEATH on lying downstream**

Append:

```cpp
namespace {

class LyingDownstream : public ni::cmodel::noc::NocReqOut {
public:
    bool push_flit(const Flit&) override { return false; }
    bool credit_avail(uint8_t) const override { return true; }
};

}  // namespace

TEST(NmuVcArbDeath, ProtocolViolation_LyingDownstream_DeathTest) {
    SCENARIO("NMU VcArb: downstream lies — credit_avail returns true but "
             "push_flit returns false. VcArb::tick must assert+abort (the "
             "protocol guarantees credit_avail=true implies push_flit "
             "success on the next call).");
    LyingDownstream liar;
    auto arb = VcArb::read_write_split(liar, /*num_vc=*/1, 0, 0);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_DEATH({ arb.tick(); },
                 "nmu::VcArb::tick: downstream returned credit_avail=true");
}
```

- [ ] **Step 26: Build + run death test — expect PASS**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NmuVcArbDeath -V`
Expected: 2/2 PASS.

- [ ] **Step 27: Run full ctest — expect 319/319 (307 prior + 12 new)**

Run: `ctest --test-dir c_model/build -j 1`
Expected: `319 tests passed`.

- [ ] **Step 28: Commit**

```powershell
git add c_model/include/nmu/vc_arb.hpp `
        c_model/tests/nmu/test_vc_arb.cpp `
        c_model/tests/nmu/CMakeLists.txt
git commit -m @'
feat(nmu/vc_arb): VcArb class (Mode A + B) + 12 unit tests

Implement nmu::VcArb decorator over NocReqOut:
  Mode A (ReadWriteSplit, default): AW/W → write_vc, AR → read_vc.
  Mode B (MultiCandidate): per-axi_ch candidate VC list; first VC with
    pending space AND downstream credit_avail wins.

W-follows-AW invariant enforced in both modes via std::deque<uint8_t>
pending_w_routes_ (push on AW, pop on W with payload.W.wlast=1) — W
routes to the front of the deque regardless of mode. wlast read from
payload.W.wlast, NOT header.last, to decouple from the Task 3 packetize
fix.

tick() scans all NUM_VC VCs from round_robin_ptr_; first VC with
pending+credit wins; round_robin_ptr_ advances past the served VC for
fairness. NDEBUG-safe: empty-deque + lying-downstream protocol
violations use assert(false) + std::abort() so release builds also
fail-fast.

NUM_VC=1 degenerate: both modes early-return VC=0; behavior
observationally identical to prior-round pipeline.

12 new tests:
  Degenerate_NumVc1_AllModesPassthrough — NUM_VC=1 sanity
  ReadWriteSplit_AW_AR_GoSeparateVcs — Mode A NUM_VC=2 split
  MultiCandidate_HoLAvoidance — Mode B HoL avoidance NUM_VC=4
  WFollowsAW_InvariantEnforced — multi-outstanding AW + W routing
  WlastFromPayloadNotHeader — pop driven by payload.W.wlast only
  RoundRobinFairness_AllVcsServiced_NoStarvation — NUM_VC=4 RR
  CreditGating_TickIdleWhenAllVcsBlocked — credit_avail gating
  BackpressureChain_VcArbToUpstream — pending_depth backpressure
  EnabledModeMixedWith_PriorRoundTests — Packetize transparency
  WHeaderLastMatchesWlast — §12 fix end-to-end via VcArb
  WFollowsAW_WBeforeAW_DeathTest — EXPECT_DEATH invariant guard
  ProtocolViolation_LyingDownstream_DeathTest — EXPECT_DEATH credit guard

ctest: 307 → 319.

Refs: docs/superpowers/specs/2026-06-03-vc-arb-multi-mode-design.md §7-§11, §13

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 5: `nsu::VcArb` class + 5 tests (NSU mirror)

**Goal:** Mirror of `nmu::VcArb` for the response side. NSU sees B + R axi_ch (not AW/W/AR). NSU does NOT need `pending_w_routes_` — per FlooNoC (`floo_axi_chimney.sv:624-633`) R uses ROB not wormhole, and B is single-flit.

**Files:**
- Create: `c_model/include/nsu/vc_arb.hpp`
- Create: `c_model/tests/nsu/test_nsu_vc_arb.cpp` (NOTE: must be `test_nsu_vc_arb` not `test_vc_arb` — CMake target names are global; would collide with Task 4's `test_vc_arb` in nmu/CMakeLists.txt. Same convention as `test_nsu_packetize` vs `test_packetize` in existing tree.)
- Modify: `c_model/tests/nsu/CMakeLists.txt`

- [ ] **Step 1: Create `c_model/include/nsu/vc_arb.hpp`**

```cpp
#pragma once
// NSU virtual channel arbiter. Mirror of nmu::VcArb but for response
// side (B + R flits leaving NSU toward NMU). No W-follows-AW logic
// because NSU produces single-flit B (`floo_axi_chimney.sv:608-616`)
// and multi-flit R uses ROB not wormhole (`floo_axi_chimney.sv:624-633`).
//
// Two modes parallel nmu::VcArb:
//   Mode A (ReadWriteSplit, default): B → write_rsp_vc; R → read_rsp_vc.
//   Mode B (MultiCandidate): per-axi_ch candidate VC list.
//
// See docs/superpowers/specs/2026-06-03-vc-arb-multi-mode-design.md §7.2
#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include "noc/noc_rsp_out.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace ni::cmodel::nsu {

enum class VcMode {
    ReadWriteSplit,
    MultiCandidate,
};

class VcArb : public noc::NocRspOut {
public:
    static constexpr std::size_t NUM_VC_MAX   = 1u << ni::header::VC_ID_WIDTH;  // 8
    static constexpr std::size_t AXI_CH_COUNT = 5;
    static constexpr std::size_t kDefaultPendingDepth = 4;

    static VcArb read_write_split(noc::NocRspOut& downstream,
                                  std::size_t num_vc,
                                  uint8_t write_rsp_vc,
                                  uint8_t read_rsp_vc,
                                  std::size_t pending_depth = kDefaultPendingDepth) {
        std::array<std::vector<uint8_t>, AXI_CH_COUNT> empty_candidates{};
        return VcArb(downstream, num_vc, VcMode::ReadWriteSplit,
                     write_rsp_vc, read_rsp_vc,
                     std::move(empty_candidates), pending_depth);
    }

    static VcArb multi_candidate(noc::NocRspOut& downstream,
                                 std::size_t num_vc,
                                 std::array<std::vector<uint8_t>, AXI_CH_COUNT> candidate_vcs,
                                 std::size_t pending_depth = kDefaultPendingDepth) {
        return VcArb(downstream, num_vc, VcMode::MultiCandidate,
                     /*write_rsp_vc*/0, /*read_rsp_vc*/0,
                     std::move(candidate_vcs), pending_depth);
    }

    bool push_flit(const Flit& flit) override;
    bool credit_avail(uint8_t vc_id) const override;
    void tick();

    std::size_t pending_size(uint8_t vc_id) const noexcept { return pending_[vc_id].size(); }
    uint8_t     round_robin_ptr() const noexcept { return round_robin_ptr_; }

private:
    VcArb(noc::NocRspOut& downstream,
          std::size_t num_vc,
          VcMode mode,
          uint8_t write_rsp_vc,
          uint8_t read_rsp_vc,
          std::array<std::vector<uint8_t>, AXI_CH_COUNT> candidate_vcs,
          std::size_t pending_depth)
        : downstream_(downstream),
          num_vc_(num_vc),
          mode_(mode),
          write_rsp_vc_(write_rsp_vc),
          read_rsp_vc_(read_rsp_vc),
          candidate_vcs_(std::move(candidate_vcs)),
          pending_depth_(pending_depth) {
        assert(num_vc_ >= 1 && num_vc_ <= NUM_VC_MAX);
        assert(write_rsp_vc_ < num_vc_);
        assert(read_rsp_vc_ < num_vc_);
    }

    std::optional<uint8_t> select_vc_for_axi_ch(uint8_t axi_ch);

    noc::NocRspOut&                                  downstream_;
    std::size_t                                      num_vc_;
    VcMode                                           mode_;
    uint8_t                                          write_rsp_vc_;
    uint8_t                                          read_rsp_vc_;
    std::array<std::vector<uint8_t>, AXI_CH_COUNT>   candidate_vcs_;
    std::array<std::deque<Flit>, NUM_VC_MAX>         pending_;
    std::size_t                                      pending_depth_;
    uint8_t                                          round_robin_ptr_ = 0;
};

inline std::optional<uint8_t> VcArb::select_vc_for_axi_ch(uint8_t axi_ch) {
    if (num_vc_ == 1) return uint8_t{0};

    if (mode_ == VcMode::ReadWriteSplit) {
        if (axi_ch == ni::AXI_CH_B) return write_rsp_vc_;
        if (axi_ch == ni::AXI_CH_R) return read_rsp_vc_;
        return std::nullopt;
    }
    for (uint8_t vc : candidate_vcs_[axi_ch]) {
        if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) {
            return vc;
        }
    }
    return std::nullopt;
}

inline bool VcArb::push_flit(const Flit& flit) {
    uint8_t axi_ch = static_cast<uint8_t>(flit.get_header_field("axi_ch"));
    auto vc_opt = select_vc_for_axi_ch(axi_ch);
    if (!vc_opt.has_value()) return false;
    uint8_t vc_id = *vc_opt;
    if (pending_[vc_id].size() >= pending_depth_) return false;
    Flit stamped = flit;
    stamped.set_header_field("vc_id", vc_id);
    pending_[vc_id].push_back(stamped);
    return true;
}

inline bool VcArb::credit_avail(uint8_t vc_id) const {
    return pending_[vc_id].size() < pending_depth_;
}

inline void VcArb::tick() {
    for (std::size_t k = 0; k < num_vc_; ++k) {
        uint8_t vc = static_cast<uint8_t>((round_robin_ptr_ + k) % num_vc_);
        if (!pending_[vc].empty() && downstream_.credit_avail(vc)) {
            bool ok = downstream_.push_flit(pending_[vc].front());
            assert(ok && "nsu::VcArb::tick: downstream returned credit_avail=true "
                         "but push_flit refused — protocol violation");
            if (!ok) std::abort();
            pending_[vc].pop_front();
            round_robin_ptr_ = static_cast<uint8_t>((vc + 1) % num_vc_);
            return;
        }
    }
}

}  // namespace ni::cmodel::nsu
```

- [ ] **Step 2: Create `c_model/tests/nsu/test_nsu_vc_arb.cpp` with all 5 tests**

```cpp
#include "nsu/vc_arb.hpp"
#include "common/loopback_noc.hpp"
#include "common/scenario.hpp"
#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
#include <array>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::nsu::VcArb;
using ni::cmodel::nsu::VcMode;
using ni::cmodel::testing::LoopbackNoc;

namespace {

Flit make_rsp_flit(uint8_t axi_ch, uint8_t initial_vc = 0) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", 0x12);
    f.set_header_field("vc_id",  initial_vc);
    f.set_header_field("src_id", 0x34);
    f.set_header_field("last",   1);
    return f;
}

}  // namespace

TEST(NsuVcArb, Nsu_Degenerate_NumVc1_Passthrough) {
    SCENARIO("NSU VcArb: NUM_VC=1, both Mode A (read_write_split) and Mode B "
             "(multi_candidate) route B + R → VC=0");

    // Mode A
    {
        LoopbackNoc noc(/*req*/32, /*rsp*/32);
        auto arb = VcArb::read_write_split(noc.rsp_out(), /*num_vc=*/1, 0, 0);
        ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
        ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
        arb.tick(); arb.tick();
        for (int i = 0; i < 2; ++i) {
            auto f = noc.rsp_in().pop_flit(); ASSERT_TRUE(f.has_value());
            EXPECT_EQ(f->get_header_field("vc_id"), 0u);
        }
    }
    // Mode B
    {
        LoopbackNoc noc(/*req*/32, /*rsp*/32);
        std::array<std::vector<uint8_t>, VcArb::AXI_CH_COUNT> candidates{};
        candidates[ni::AXI_CH_B] = {0};
        candidates[ni::AXI_CH_R] = {0};
        auto arb = VcArb::multi_candidate(noc.rsp_out(), /*num_vc=*/1, candidates);
        ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
        ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
        arb.tick(); arb.tick();
        for (int i = 0; i < 2; ++i) {
            auto f = noc.rsp_in().pop_flit(); ASSERT_TRUE(f.has_value());
            EXPECT_EQ(f->get_header_field("vc_id"), 0u);
        }
    }
}

TEST(NsuVcArb, Nsu_ReadWriteSplit_B_R_GoSeparateVcs) {
    SCENARIO("NSU VcArb Mode A NUM_VC=2: B → write_rsp_vc=0, R → read_rsp_vc=1");
    LoopbackNoc noc(/*req*/32, /*rsp*/32);
    auto arb = VcArb::read_write_split(noc.rsp_out(), /*num_vc=*/2, 0, 1);

    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
    EXPECT_EQ(arb.pending_size(0), 1u);
    EXPECT_EQ(arb.pending_size(1), 1u);

    arb.tick(); arb.tick();
    auto f0 = noc.rsp_in().pop_flit(); ASSERT_TRUE(f0.has_value());
    auto f1 = noc.rsp_in().pop_flit(); ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f0->get_header_field("axi_ch"), ni::AXI_CH_B);
    EXPECT_EQ(f0->get_header_field("vc_id"),  0u);
    EXPECT_EQ(f1->get_header_field("axi_ch"), ni::AXI_CH_R);
    EXPECT_EQ(f1->get_header_field("vc_id"),  1u);
}

TEST(NsuVcArb, Nsu_MultiCandidate_HoLAvoidance) {
    SCENARIO("NSU VcArb Mode B NUM_VC=4: B candidates {0,1}; saturate VC=0, "
             "next B picks VC=1");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    std::array<std::vector<uint8_t>,
               VcArb::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_B] = {0, 1};
    candidates[ni::AXI_CH_R] = {2, 3};
    auto arb = VcArb::multi_candidate(noc.rsp_out(), /*num_vc=*/4,
                                      candidates, /*pending_depth=*/2);
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
    EXPECT_EQ(arb.pending_size(0), 2u);
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
    EXPECT_EQ(arb.pending_size(1), 1u);
}

TEST(NsuVcArb, Nsu_RoundRobinFairness) {
    SCENARIO("NSU VcArb NUM_VC=4: 4 R flits across 4 VCs; tick 4 → drained in RR order");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    std::array<std::vector<uint8_t>,
               VcArb::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_B] = {0};
    candidates[ni::AXI_CH_R] = {0, 1, 2, 3};
    auto arb = VcArb::multi_candidate(noc.rsp_out(), /*num_vc=*/4,
                                      candidates, /*pending_depth=*/1);
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
    arb.tick(); arb.tick(); arb.tick(); arb.tick();
    for (uint8_t v = 0; v < 4; ++v) {
        auto f = noc.rsp_in().pop_flit(); ASSERT_TRUE(f.has_value());
        EXPECT_EQ(f->get_header_field("vc_id"), v);
    }
}

TEST(NsuVcArb, Nsu_CreditGating) {
    SCENARIO("NSU VcArb: downstream per_vc_depth=1; push 2 R flits VC=0, "
             "VC=1; tick drains VC=0 first then VC=1 only after pop releases credit");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    noc.set_per_vc_depth(1);
    std::array<std::vector<uint8_t>,
               VcArb::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_B] = {0};
    candidates[ni::AXI_CH_R] = {0, 1};
    auto arb = VcArb::multi_candidate(noc.rsp_out(), /*num_vc=*/2,
                                      candidates, /*pending_depth=*/2);
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
    arb.tick();
    EXPECT_EQ(noc.nsu_rsp_per_vc_in_flight(0), 1u);
    EXPECT_EQ(arb.pending_size(0), 0u);
    arb.tick();
    EXPECT_EQ(noc.nsu_rsp_per_vc_in_flight(1), 1u);
}
```

- [ ] **Step 3: Register `test_nsu_vc_arb` in `c_model/tests/nsu/CMakeLists.txt`**

Append:

```cmake
# Note: distinct target name from nmu/test_vc_arb to avoid CMake target
# collision (add_cmodel_test creates a bare-named target). Source file
# mirrors the target name; gtest suite name remains NsuVcArb.
add_cmodel_test(test_nsu_vc_arb)
target_include_directories(test_nsu_vc_arb PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

- [ ] **Step 4: Build + run NSU tests — expect 5/5 PASS**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R NsuVcArb -V`
Expected: 5/5 PASS.

- [ ] **Step 5: Run full ctest — expect 324/324 (319 prior + 5 new)**

Run: `ctest --test-dir c_model/build -j 1`
Expected: `324 tests passed`.

Note: spec §13.4 target was 320; actual is 324 because Task 2 introduced 4 LoopbackNoc tests not counted in spec §13.4 (spec listed them as part of "LoopbackNoc per-VC FIFO" without explicit count). 4 (Task 2) + 1 (Task 3 net) + 12 (Task 4) + 5 (Task 5) = 22 new; 302 + 22 = 324. Acceptable — update NEXT_STEPS in Task 7 with actual final count.

- [ ] **Step 6: Commit**

```powershell
git add c_model/include/nsu/vc_arb.hpp `
        c_model/tests/nsu/test_nsu_vc_arb.cpp `
        c_model/tests/nsu/CMakeLists.txt
git commit -m @'
feat(nsu/vc_arb): VcArb class (NSU mirror) + 5 unit tests

Mirror of nmu::VcArb for the response side. NSU sees B + R axi_ch;
Mode A maps B → write_rsp_vc, R → read_rsp_vc. No W-follows-AW deque
because NSU B is single-flit and R uses ROB not wormhole (per FlooNoC
floo_axi_chimney.sv:624-633 "no reason to do wormhole routing for R
bursts").

5 new tests cover NUM_VC=1 degenerate, Mode A B/R split, Mode B HoL
avoidance, round-robin fairness across NUM_VC=4, and credit gating.

ctest: 319 → 324.

Refs: docs/superpowers/specs/2026-06-03-vc-arb-multi-mode-design.md §7.2, §13.1

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 6: Wire VcArb into integration testbench

**Goal:** Insert `nmu::VcArb` between `nmu::Packetize` and the LoopbackNoc req side, and a `nsu::VcArb` per NSU between `nsu::Packetize` and the LoopbackNoc rsp side, in `test_request_response_loopback.cpp`. NUM_VC=1, Mode A (default). All existing integration tests should pass unchanged because NUM_VC=1 makes VcArb observationally transparent.

**Lifetime concern**: the NSU side stores packetizers in `std::vector<std::unique_ptr<nsu::Packetize>>` (line 199, 215 of current fixture). NSU VcArbs must outlive the packetizers that hold a reference to them — they need parallel `std::unique_ptr` storage in a vector declared BEFORE `nsu_pkts`. NMU side is simpler (single stack instance).

**Files:**
- Modify: `c_model/tests/integration/test_request_response_loopback.cpp`

- [ ] **Step 1: Add includes near top of file**

After the existing `nmu::*` / `nsu::*` includes, add:

```cpp
#include "nmu/vc_arb.hpp"
#include "nsu/vc_arb.hpp"
```

- [ ] **Step 2: Insert NMU VcArb before `real_nmu_pkt` construction**

Locate line 183 (currently `nmu::Packetize real_nmu_pkt(loopback.nmu_req_out(), /*src_id=*/kNmuSrcId);`).

Replace it with:

```cpp
  // NMU VcArb decorator. NUM_VC=1, Mode A (ReadWriteSplit) — observationally
  // transparent to all existing fixtures; future rounds may up NUM_VC.
  auto nmu_arb = nmu::VcArb::read_write_split(
      loopback.nmu_req_out(), /*num_vc=*/1, /*write_vc=*/0, /*read_vc=*/0);
  nmu::Packetize    real_nmu_pkt(nmu_arb, /*src_id=*/kNmuSrcId);
```

- [ ] **Step 3: Add NSU VcArb vector before NSU packetizer vector + emplace per NSU**

Locate the block of `std::vector<std::unique_ptr<...>>` declarations starting at line 197. Add a new vector declaration immediately BEFORE `nsu_pkts`:

```cpp
  std::vector<std::unique_ptr<nsu::VcArb>>         nsu_arbs;
```

Add the reserve immediately before `nsu_pkts.reserve(nsu_count);`:

```cpp
  nsu_arbs.reserve(nsu_count);
```

Inside the per-NSU `for` loop, insert the VcArb emplacement BEFORE the `nsu_pkts.emplace_back(...)` call (currently line 215). The full updated emplacement block becomes:

```cpp
    nsu_metas.emplace_back(
        std::make_unique<nsu::MetaBuffer>(params.meta_buffer_per_id_depth));
    nsu_depkts.emplace_back(std::make_unique<nsu::Depacketize>(
        loopback.nsu_req_in(i), *nsu_metas[i],
        params.depkt_aw_q_depth,
        params.depkt_w_q_depth,
        params.depkt_ar_q_depth));
    // NSU VcArb decorator. NUM_VC=1, Mode A. Move-constructs from factory
    // return into unique_ptr so the arbiter outlives the packetizer that
    // holds it by reference.
    nsu_arbs.emplace_back(std::make_unique<nsu::VcArb>(
        nsu::VcArb::read_write_split(
            loopback.nsu_rsp_out(i), /*num_vc=*/1, /*write_rsp_vc=*/0, /*read_rsp_vc=*/0)));
    nsu_pkts.emplace_back(std::make_unique<nsu::Packetize>(
        *nsu_arbs[i], *nsu_metas[i], this_nsu_src));
    nsu_ports.emplace_back(std::make_unique<nsu::AxiMasterPort>(
        *nsu_depkts[i], *nsu_pkts[i], params));
```

- [ ] **Step 4: Add VcArb tick calls into per-cycle tick loop**

Locate the per-cycle while loop (line 289-393). The `loopback.tick();` call sits at line 393 just before the timeout check. Add VcArb tick calls IMMEDIATELY BEFORE `loopback.tick();`. Replace:

```cpp
    // Advance the loopback's per-cycle delay pipes after all producers /
    // consumers have run for this cycle. Mirrors port-pair test ordering.
    loopback.tick();
```

with:

```cpp
    // Advance VcArb pending queues before loopback ages: VcArb drains its
    // per-VC pending into loopback's per-NSU queues, then loopback ages its
    // per-cycle delay pipes. Same-cycle ordering matches the request-side
    // pipeline (Packetize → VcArb.push_flit during nmu_port.tick(), then
    // VcArb.tick() pushes into loopback in the same cycle).
    nmu_arb.tick();
    for (std::size_t i = 0; i < nsu_count; ++i) {
      nsu_arbs[i]->tick();
    }

    // Advance the loopback's per-cycle delay pipes after all producers /
    // consumers have run for this cycle. Mirrors port-pair test ordering.
    loopback.tick();
```

- [ ] **Step 5: Build + run integration tests — expect all PASS unchanged**

Run: `cmake --build c_model/build && ctest --test-dir c_model/build -R Loopback -V`
Expected: all `Loopback*` integration tests pass (counts unchanged from pre-task-6).

- [ ] **Step 6: Run full ctest — expect 324/324 unchanged**

Run: `ctest --test-dir c_model/build -j 1`
Expected: `324 tests passed`.

- [ ] **Step 7: Commit**

```powershell
git add c_model/tests/integration/test_request_response_loopback.cpp
git commit -m @'
feat(tests/integration): wire VcArb into PacketizeLoopback testbench

Insert nmu::VcArb between nmu::Packetize and LoopbackNoc.req_out(), and
nsu::VcArb between nsu::Packetize and LoopbackNoc.rsp_out(). Mode A
(ReadWriteSplit), NUM_VC=1, default pending_depth=4. Per-cycle tick
loop now calls nmu_arb.tick() + nsu_arb.tick() before noc.tick() to
drain VcArb pending queues.

NUM_VC=1 makes VcArb observationally transparent — all existing
integration tests pass unchanged. Exercising NUM_VC>=2 + Mode B in
this fixture (multi-NSU stress under HoL) is deferred to a future
round per spec §13.2.

ctest: 324 → 324 (decorator transparent).

Refs: docs/superpowers/specs/2026-06-03-vc-arb-multi-mode-design.md §13.2

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 7: Update `NEXT_STEPS.md`

**Goal:** Karpathy 4-lens summary of this round's deliverables + flip pointer to next planned round.

**Files:**
- Modify: `NEXT_STEPS.md`

- [ ] **Step 1: Read current `NEXT_STEPS.md`**

Run: Read `NEXT_STEPS.md`. Note the existing structure (likely a "Done" section and a "Next" pointer).

- [ ] **Step 2: Replace the "Next" pointer block**

Locate the section listing the next round (currently pointing at `vc_arb`). Replace its content with:

```markdown
## Just done (this round): vc_arb multi-mode + header.last wormhole fix

**Karpathy 4-lens summary**:
- **What we shipped**: nmu::VcArb + nsu::VcArb classes with Mode A
  (ReadWriteSplit) + Mode B (MultiCandidate); per-VC credit-gated
  round-robin; W-follows-AW invariant via pending_w_routes_ deque;
  LoopbackNoc per-VC credit counter; FlooNoC-aligned header.last fix
  in nmu/packetize.hpp (AW=0, W=wlast). 22 new tests; ctest 302→324.
- **What we proved**: parameterized for NUM_VC=1..8; test matrix covers
  all values; Mode B avoids HoL blocking; decorator transparent at
  NUM_VC=1 (existing tests pass unchanged); EXPECT_DEATH guards on
  W-before-AW + lying-downstream invariants survive NDEBUG.
- **What we owe**: defer to future rounds — wormhole_arbiter equivalent
  at NoC fabric level; multi-NSU testbench routing through VcArb (NUM_VC>=2);
  weighted/priority RR; VC starvation detection; dynamic per-cycle
  remapping; YAML-driven candidate_vcs config.
- **Why it matters now**: per main plan §3 file structure (rows 167-173),
  vc_mapping + vc_arb are gating items for Stage 3 NoC fabric integration;
  per spec §1, FlooNoC-aligned wormhole semantic prevents AXI4 W
  interleaving corruption in future multi-master + per-link arbiter scenarios.

## Next round: [TBD — choose from main plan §3 unimplemented rows]

Candidates (per `docs/noc_cmodel_rtl_plan.md` §3):
- route_par (header field; multi-path routing; main plan row [TBD])
- nmu.hpp top-level (assembles addr_trans + Packetize + Rob + VcArb)
- nsu.hpp top-level (assembles Depacketize + MetaBuffer + Packetize + VcArb)
- wormhole_arbiter at NoC fabric layer (consumes header.last from §12 fix)

Open: ask user which to pursue next at start of next session.
```

- [ ] **Step 3: Run full ctest one more time — expect 324/324**

Run: `ctest --test-dir c_model/build -j 1`
Expected: `324 tests passed`.

- [ ] **Step 4: Commit**

```powershell
git add NEXT_STEPS.md
git commit -m @'
docs(NEXT_STEPS): vc_arb multi-mode round done; next round TBD

vc_arb round delivered: nmu/nsu VcArb decorator (Mode A + B), per-VC
credit gating, W-follows-AW invariant, LoopbackNoc per-VC counter,
FlooNoC-aligned packetize header.last fix. 7 commits, 22 new tests
(302→324).

Karpathy 4-lens recap + candidate list for next round (route_par,
nmu.hpp top-level, nsu.hpp top-level, wormhole_arbiter at fabric).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Final verification

After all 7 commits, run the complete drift gate:

```powershell
cd specgen
py -3 -m pytest -q
py -3 tools/codegen.py --check
py -3 tools/gen_inventory.py --check
cd ../c_model
cmake --build build
ctest --test-dir build -j 1
git log --oneline 8e20ea9..HEAD
```

Expected output:
- specgen pytest: 163 passed
- codegen --check: clean
- gen_inventory --check: clean
- ctest: 324/324
- git log: 7 commits (Task 1 through Task 7) on `stage3/packetize-depacketize`

If any drift gate fails, do NOT skip via `--no-verify`. Investigate and fix at root cause.

---

## Self-review notes (controller, not for implementer)

1. **Spec §6 divergence**: spec sketches per-VC FIFO in LoopbackNoc; plan Task 2 uses per-VC credit COUNTER atop existing per-NSU FIFOs. Documented in "Implementation note: divergence from spec §6" at top. Preserves 302 existing tests + multi-NSU routing.

2. **Test count divergence**: spec §13.4 said target 320; actual is 324 because Task 2 added 4 new dedicated LoopbackNoc tests (spec lumped under "per-VC FIFO" without count). Acceptable — surfaced in Task 5 step 5 + Task 7 commit message.

3. **§9 protocol-violation pattern**: `assert(ok ...) + if(!ok) std::abort();` matches the §10.4 belt-and-braces pattern. Both tested by EXPECT_DEATH (Task 4 steps 23-26).

4. **W-before-AW death test setup**: uses NUM_VC=2 instead of NUM_VC=1 because NUM_VC=1 early-returns at top of `select_vc_for_axi_ch` and skips the empty-deque check.

5. **Integration testbench Step 1**: requires reading current source first to find exact wiring points; implementer must grep for `nmu::Packetize` and `nsu::Packetize` construction sites.
