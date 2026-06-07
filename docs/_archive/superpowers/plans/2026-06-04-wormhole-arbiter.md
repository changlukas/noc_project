# WormholeArbiter + Packetize multi-output refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `noc/wormhole_arbiter.hpp` (N-to-1 lock arbiter with AW→W pairing config), refactor NMU/NSU `Packetize` to per-AXI-channel multi-output, simplify NMU `VcArbiter` (deque→optional, enabled by upstream wormhole serialization), and rename `VcArb`→`VcArbiter` for full-word naming consistency — across 5 commits, growing ctest from 359 to 370 (+11 new wormhole tests).

**Architecture:** Pipeline `Packetize{aw_out,w_out,ar_out} → WormholeArbiter<NocReqOut> → VcArbiter → NocReqOut`. NSU mirror with 2 inputs and no pairing (B/R per-flit packets). WormholeArbiter absorbs FlooNoC's SelW state machine + wormhole_arbiter into one module with 3 inputs and an optional `ChannelPairing` config.

**Tech Stack:** C++17, GoogleTest (incl. EXPECT_DEATH), CMake, Windows PowerShell + `py -3`. Codegen-derived constants from `ni::header::*`.

**Spec:** `docs/superpowers/specs/2026-06-04-wormhole-arbiter-design.md` (Codex r2 APPROVED after math fix).

**Branch:** `stage3/packetize-depacketize` (commits accumulate; no push until Stage 3 full set merged).

---

## Implementation note: bash heredoc for commit messages

PowerShell `@'...'@` heredoc syntax **does NOT work** in the Bash tool (lesson from vc_arb round). Use:

```bash
git commit -m "$(cat <<'EOF'
subject line

body...
EOF
)"
```

---

## File Structure

| Path | Action | Responsibility |
|---|---|---|
| `c_model/include/noc/wormhole_arbiter.hpp` | CREATE | Template `WormholeArbiter<Downstream>`; N-to-1 lock arbiter; `ChannelPairing` config |
| `c_model/include/nmu/packetize.hpp` | MODIFY | Ctor takes 3 NocReqOut& (aw/w/ar); `w_meta_fifo_` stays |
| `c_model/include/nsu/packetize.hpp` | MODIFY | Ctor takes 2 NocRspOut& (b/r) |
| `c_model/include/nmu/vc_arb.hpp` → `vc_arbiter.hpp` | RENAME + simplify | Rename file + class; simplify `pending_w_routes_` deque → `current_aw_vc_` optional |
| `c_model/include/nsu/vc_arb.hpp` → `vc_arbiter.hpp` | RENAME | File + class rename (no logic change) |
| `c_model/tests/common/per_channel_capture.hpp` | CREATE | Template `PerChannelCapture<Interface>` mock — captures flits per output |
| `c_model/tests/noc/CMakeLists.txt` | CREATE | New test dir for noc/ unit tests |
| `c_model/tests/noc/test_wormhole_arbiter.cpp` | CREATE | 11 unit tests (7 functional + 4 EXPECT_DEATH) |
| `c_model/tests/nmu/test_packetize.cpp` | MODIFY | Fixture: 3× PerChannelCapture instead of LoopbackNoc |
| `c_model/tests/nsu/test_nsu_packetize.cpp` | MODIFY | Fixture: 2× PerChannelCapture |
| `c_model/tests/nmu/test_rob.cpp` | MODIFY | Fixture: pass 3 capture refs into Packetize |
| `c_model/tests/nmu/test_axi_slave_port.cpp` | MODIFY | Same |
| `c_model/tests/nmu/test_vc_arb.cpp` → `test_vc_arbiter.cpp` | RENAME + restructure | `WFollowsAW_InvariantEnforced` → single-AW scenario; CMake target rename |
| `c_model/tests/nsu/test_nsu_vc_arb.cpp` → `test_nsu_vc_arbiter.cpp` | RENAME | Pure rename |
| `c_model/tests/integration/test_request_response_loopback.cpp` | MODIFY | Wire WormholeArbiter between Packetize and VcArbiter; add tick |
| `c_model/tests/{nmu,nsu}/CMakeLists.txt` | MODIFY | Update target names |
| `c_model/tests/CMakeLists.txt` | MODIFY | Add `noc/` subdirectory |
| `NEXT_STEPS.md` | MODIFY | Karpathy 4-lens summary + next round pointer |

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

Baseline at HEAD (commit `e72bb43` spec): specgen 163 pass, codegen clean, inventory clean, ctest **359/359**.

---

## Task 1: Rename `VcArb` → `VcArbiter` (mechanical full-word naming)

**Goal:** Pure rename. File names, class names, CMake target names, all referencing test files. No logic change.

**Files:**
- Rename: `c_model/include/nmu/vc_arb.hpp` → `vc_arbiter.hpp`
- Rename: `c_model/include/nsu/vc_arb.hpp` → `vc_arbiter.hpp`
- Rename: `c_model/tests/nmu/test_vc_arb.cpp` → `test_vc_arbiter.cpp`
- Rename: `c_model/tests/nsu/test_nsu_vc_arb.cpp` → `test_nsu_vc_arbiter.cpp`
- Modify: `c_model/tests/nmu/CMakeLists.txt`
- Modify: `c_model/tests/nsu/CMakeLists.txt`
- Modify: all `.cpp` / `.hpp` files that include or reference `VcArb`

- [ ] **Step 1: git mv files**

```bash
git mv c_model/include/nmu/vc_arb.hpp c_model/include/nmu/vc_arbiter.hpp
git mv c_model/include/nsu/vc_arb.hpp c_model/include/nsu/vc_arbiter.hpp
git mv c_model/tests/nmu/test_vc_arb.cpp c_model/tests/nmu/test_vc_arbiter.cpp
git mv c_model/tests/nsu/test_nsu_vc_arb.cpp c_model/tests/nsu/test_nsu_vc_arbiter.cpp
```

- [ ] **Step 2: Inside the renamed `.hpp` files, rename `class VcArb` → `class VcArbiter`**

Replace 2 occurrences per file: `class VcArb : public noc::NocReqOut/RspOut` and `static VcArb read_write_split/multi_candidate(...)` returns. Adjust any constructor/factory using `VcArb(` to `VcArbiter(`.

- [ ] **Step 3: Find all references and rename**

```bash
grep -rln "VcArb\b\|vc_arb\.hpp\|vc_arb\b" c_model/ | xargs sed -i 's/VcArb\b/VcArbiter/g; s/vc_arb\.hpp/vc_arbiter.hpp/g; s/test_vc_arb\b/test_vc_arbiter/g'
```

After grep, manually verify no `VcArb` strings remain except in scenario descriptions / commit messages / comments referencing prior round name.

- [ ] **Step 4: Update CMake target names**

`c_model/tests/nmu/CMakeLists.txt`: change `add_cmodel_test(test_vc_arb)` → `add_cmodel_test(test_vc_arbiter)`; update `target_include_directories(test_vc_arb ...)` to `test_vc_arbiter`.

`c_model/tests/nsu/CMakeLists.txt`: same for `test_nsu_vc_arb` → `test_nsu_vc_arbiter`.

- [ ] **Step 5: Build + verify**

```bash
cmake --build c_model/build
ctest --test-dir c_model/build -j 1
```

Expected: build clean, 359/359 pass.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
refactor: rename VcArb -> VcArbiter (full-word naming consistency)

Renames nmu/vc_arb.hpp -> vc_arbiter.hpp, nsu/vc_arb.hpp -> vc_arbiter.hpp,
class VcArb -> VcArbiter (both NMU + NSU), CMake targets test_vc_arb ->
test_vc_arbiter (and test_nsu_vc_arb -> test_nsu_vc_arbiter), plus all
referring includes and test fixtures.

Pure mechanical rename; no logic change. ctest 359/359 unchanged.

Per feedback-naming-full-word-no-abbreviation memory: no Arb/arbiter
mixing; file name and class name aligned. Foundation for upcoming
WormholeArbiter (full-word) module.

Refs: docs/superpowers/specs/2026-06-04-wormhole-arbiter-design.md §3

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Add `WormholeArbiter` module + `PerChannelCapture` mock + 11 unit tests

**Goal:** Create the new arbiter module with full test coverage. Standalone — does not modify any existing module.

**Files:**
- Create: `c_model/include/noc/wormhole_arbiter.hpp`
- Create: `c_model/tests/common/per_channel_capture.hpp`
- Create: `c_model/tests/noc/CMakeLists.txt`
- Create: `c_model/tests/noc/test_wormhole_arbiter.cpp`
- Modify: `c_model/tests/CMakeLists.txt` (add `add_subdirectory(noc)`)

- [ ] **Step 1: Create `c_model/include/noc/wormhole_arbiter.hpp`**

```cpp
#pragma once
// Generic N-to-1 wormhole arbiter for c_model NoC behavior model.
//
// Inherits FlooNoC's wormhole locking semantic (hw/floo_wormhole_arbiter.sv
// LockIn=1, release on `last_out & ready_i`) and collapses FlooNoC's separate
// SelW state machine into a single arbiter via the optional ChannelPairing
// config. Used at NI side for 5->2 AXI-to-NoC channel mapping; reusable at
// NoC fabric router output ports (future Stage 4 round).
//
// Pipeline placement:
//   NMU: Packetize{aw,w,ar} -> WormholeArbiter<NocReqOut>(3 in, {{0,1}})
//        -> VcArbiter -> NocReqOut
//   NSU: Packetize{b,r} -> WormholeArbiter<NocRspOut>(2 in, {}) -> VcArbiter
//        -> NocRspOut
//
// Lock semantic:
//   * When a flit with header.last=0 (packet start, e.g., AW) is drained
//     from a `pairing.from` port, lock to the corresponding `pairing.to`
//     port (e.g., w_in). Only the `to` port is serviceable until released.
//   * When a flit with header.last=1 (packet end, e.g., W with wlast) is
//     drained from the currently locked `to` port, unlock.
//   * Without pairing (NSU case), every flit is its own packet; no lock.
//
// Constraint A2 (from spec): REQUIRES Packetize stamps header.last per
// FlooNoC pattern (AW=0, W=wlast, AR/B/R=1). Malformed AW (from-port flit
// with last=1) triggers assert+abort at runtime.
//
// Lifetime: heap-allocate via std::unique_ptr OR construct as a stable
// named member of an owning class. Do NOT push_back into a
// std::vector<WormholeArbiter> (deleted move/copy makes that a compile
// error). InputAdapter holds a raw `parent` pointer; the pointer must
// remain valid for the arbiter's lifetime.
//
// References:
//   docs/superpowers/specs/2026-06-04-wormhole-arbiter-design.md
//   FlooNoC hw/floo_wormhole_arbiter.sv, hw/floo_axi_chimney.sv:744 / :758

#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include "noc/noc_req_out.hpp"
#include "noc/noc_rsp_out.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace ni::cmodel::noc {

struct ChannelPairing {
    std::size_t from;
    std::size_t to;
};

template <typename Downstream>
class WormholeArbiter {
public:
    static constexpr std::size_t MAX_INPUTS = 8;
    static constexpr std::size_t kDefaultPerInputDepth = 4;

    WormholeArbiter(Downstream& downstream,
                    std::size_t num_inputs,
                    std::vector<ChannelPairing> pairings = {},
                    std::size_t per_input_depth = kDefaultPerInputDepth)
        : downstream_(downstream),
          num_inputs_(num_inputs),
          pairings_(std::move(pairings)),
          per_input_depth_(per_input_depth) {
        assert(num_inputs_ >= 1 && num_inputs_ <= MAX_INPUTS);
        assert(per_input_depth_ > 0);

        // Validate pairings: from/to in range, from != to, no duplicate from,
        // no nested chain (a `to` cannot also be a `from`).
        for (std::size_t i = 0; i < pairings_.size(); ++i) {
            const auto& p = pairings_[i];
            assert(p.from < num_inputs_ && p.to < num_inputs_ &&
                   "WormholeArbiter: pairing out of range");
            assert(p.from != p.to &&
                   "WormholeArbiter: pairing from == to");
            for (std::size_t j = i + 1; j < pairings_.size(); ++j) {
                assert(pairings_[j].from != p.from &&
                       "WormholeArbiter: duplicate pairing.from");
            }
            for (const auto& q : pairings_) {
                assert(!(q.from == p.to) &&
                       "WormholeArbiter: nested pairing chain (to is also a from)");
            }
        }

        pending_.resize(num_inputs_);
        for (std::size_t i = 0; i < num_inputs_; ++i) {
            input_adapters_.emplace_back(this, i);
        }
    }

    WormholeArbiter(const WormholeArbiter&) = delete;
    WormholeArbiter(WormholeArbiter&&)      = delete;
    WormholeArbiter& operator=(const WormholeArbiter&) = delete;
    WormholeArbiter& operator=(WormholeArbiter&&)      = delete;

    Downstream& input(std::size_t idx) {
        assert(idx < num_inputs_);
        return input_adapters_[idx];
    }

    void tick();

    // Test introspection
    std::size_t pending_size(std::size_t idx) const {
        assert(idx < num_inputs_);
        return pending_[idx].size();
    }
    bool is_locked() const noexcept { return locked_to_.has_value(); }
    std::optional<std::size_t> locked_to() const noexcept { return locked_to_; }

private:
    struct InputAdapter : Downstream {
        WormholeArbiter* parent;
        std::size_t      idx;

        InputAdapter(WormholeArbiter* p, std::size_t i) : parent(p), idx(i) {}

        bool push_flit(const Flit& f) override {
            if (parent->pending_[idx].size() >= parent->per_input_depth_) return false;
            parent->pending_[idx].push_back(f);
            return true;
        }
        bool credit_avail(uint8_t /*vc_id*/) const override {
            return parent->pending_[idx].size() < parent->per_input_depth_;
        }
    };

    bool is_from_port(std::size_t idx) const {
        for (const auto& p : pairings_) if (p.from == idx) return true;
        return false;
    }
    bool is_to_port(std::size_t idx) const {
        for (const auto& p : pairings_) if (p.to == idx) return true;
        return false;
    }

    Downstream&                  downstream_;
    std::size_t                  num_inputs_;
    std::vector<ChannelPairing>  pairings_;
    std::size_t                  per_input_depth_;
    std::vector<std::deque<Flit>> pending_;
    std::vector<InputAdapter>    input_adapters_;
    std::size_t                  round_robin_ptr_ = 0;
    std::optional<std::size_t>   locked_to_;
};

template <typename Downstream>
inline void WormholeArbiter<Downstream>::tick() {
    std::size_t target;

    if (locked_to_.has_value()) {
        target = *locked_to_;
        if (pending_[target].empty()) return;
    } else {
        bool found = false;
        for (std::size_t k = 0; k < num_inputs_; ++k) {
            std::size_t i = (round_robin_ptr_ + k) % num_inputs_;
            if (!pending_[i].empty()) {
                target = i;
                found = true;
                break;
            }
        }
        if (!found) return;
    }

    const Flit& flit = pending_[target].front();
    uint8_t vc_id = static_cast<uint8_t>(flit.get_header_field("vc_id"));
    if (!downstream_.credit_avail(vc_id)) return;

    uint64_t last = flit.get_header_field("last");

    // Defensive guards (Constraint A2)
    if (is_from_port(target) && last == 1) {
        assert(false && "WormholeArbiter::tick: from-port flit with header.last=1 -- malformed AW (Packetize regression on header.last stamping; vc_arb round commit 1f82ba8 stamps AW=0)");
        std::abort();
    }
    if (is_to_port(target) && !locked_to_.has_value()) {
        assert(false && "WormholeArbiter::tick: to-port flit pushed without preceding from-port flit (W before AW; upstream serialization broken)");
        std::abort();
    }

    bool ok = downstream_.push_flit(flit);
    assert(ok && "WormholeArbiter::tick: lying downstream (credit_avail=true but push_flit refused)");
    if (!ok) std::abort();  // belt-and-braces for NDEBUG

    pending_[target].pop_front();
    round_robin_ptr_ = (target + 1) % num_inputs_;

    // Lock/unlock transition
    if (last == 0 && !locked_to_.has_value()) {
        for (const auto& p : pairings_) {
            if (p.from == target) {
                locked_to_ = p.to;
                break;
            }
        }
    } else if (last == 1 && locked_to_.has_value()) {
        assert(*locked_to_ == target && "WormholeArbiter::tick: unlock target mismatch");
        locked_to_ = std::nullopt;
    }
}

}  // namespace ni::cmodel::noc
```

- [ ] **Step 2: Create `c_model/tests/common/per_channel_capture.hpp`**

```cpp
#pragma once
// Per-channel capture mock for testing modules that emit flits to multiple
// downstream channels (e.g., Packetize after multi-output refactor). Each
// instance captures flits pushed to it into an internal deque for later
// assertion. credit_avail uses the default (returns true).
#include "ni/flit.hpp"
#include "noc/noc_req_out.hpp"
#include "noc/noc_rsp_out.hpp"
#include <cstddef>
#include <deque>
#include <optional>

namespace ni::cmodel::testing {

template <typename Interface>
class PerChannelCapture : public Interface {
public:
    bool push_flit(const Flit& f) override {
        captured_.push_back(f);
        return true;
    }

    std::optional<Flit> pop() {
        if (captured_.empty()) return std::nullopt;
        Flit f = captured_.front();
        captured_.pop_front();
        return f;
    }
    std::size_t size() const noexcept { return captured_.size(); }
    void clear() noexcept { captured_.clear(); }

private:
    std::deque<Flit> captured_;
};

using ReqCapture = PerChannelCapture<noc::NocReqOut>;
using RspCapture = PerChannelCapture<noc::NocRspOut>;

}  // namespace ni::cmodel::testing
```

- [ ] **Step 3: Create `c_model/tests/noc/CMakeLists.txt`**

```cmake
add_cmodel_test(test_wormhole_arbiter)
target_include_directories(test_wormhole_arbiter PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

- [ ] **Step 4: Register noc subdirectory in `c_model/tests/CMakeLists.txt`**

Find the existing `add_subdirectory(...)` lines (likely for axi, common, integration, nmu, nsu) and add:

```cmake
add_subdirectory(noc)
```

- [ ] **Step 5: Create `c_model/tests/noc/test_wormhole_arbiter.cpp`**

```cpp
#include "noc/wormhole_arbiter.hpp"
#include "common/per_channel_capture.hpp"
#include "common/scenario.hpp"
#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::noc::WormholeArbiter;
using ni::cmodel::noc::ChannelPairing;
using ni::cmodel::testing::ReqCapture;

namespace {

Flit make_flit(uint8_t axi_ch, uint64_t last, uint64_t wlast = 0) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", 0);
    f.set_header_field("vc_id",  0);
    f.set_header_field("last",   last);
    if (axi_ch == ni::AXI_CH_W) {
        f.set_payload_field("W", "wlast", wlast);
    }
    return f;
}

}  // namespace

// ---- Functional tests (6) ----

TEST(NocWormholeArbiter, PassThroughNoPairing) {
    SCENARIO("WormholeArbiter NSU mode (2 inputs, no pairing): each pushed flit "
             "is its own packet (last=1), alternating push + tick drain in round-robin order");
    ReqCapture down;
    WormholeArbiter<ni::cmodel::noc::NocReqOut> arb(down, /*num_inputs=*/2, {});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));
    arb.tick();
    arb.tick();
    EXPECT_EQ(down.size(), 2u);
    EXPECT_FALSE(arb.is_locked());
}

TEST(NocWormholeArbiter, AwTriggersLock) {
    SCENARIO("WormholeArbiter NMU mode (3 inputs, pairing aw->w): pushing an AW "
             "(header.last=0) to aw input + tick locks the arbiter to the w input");
    ReqCapture down;
    WormholeArbiter<ni::cmodel::noc::NocReqOut> arb(
        down, /*num_inputs=*/3, {{0, 1}});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AW, /*last=*/0)));
    arb.tick();
    EXPECT_EQ(down.size(), 1u);
    EXPECT_TRUE(arb.is_locked());
    EXPECT_EQ(*arb.locked_to(), 1u);
}

TEST(NocWormholeArbiter, ArCannotInterleaveDuringLock) {
    SCENARIO("WormholeArbiter NMU mode: while locked to w input (after AW), an "
             "AR pushed to ar input cannot be drained until W with wlast unlocks");
    ReqCapture down;
    WormholeArbiter<ni::cmodel::noc::NocReqOut> arb(
        down, /*num_inputs=*/3, {{0, 1}});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AW, /*last=*/0)));
    arb.tick();  // AW drained, locked to w
    ASSERT_TRUE(arb.input(2).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));
    arb.tick();  // locked to w, w pending empty -> idle
    arb.tick();
    EXPECT_EQ(down.size(), 1u);            // only AW; AR still pending
    EXPECT_EQ(arb.pending_size(2), 1u);    // AR sitting
    EXPECT_TRUE(arb.is_locked());
}

TEST(NocWormholeArbiter, MultiBeatWBurstFlowsAndUnlocks) {
    SCENARIO("WormholeArbiter NMU mode: AW + 3 W beats (last 2 non-wlast, 3rd "
             "wlast) flow through in ORDER (AW, W, W, W-last) and arbiter "
             "unlocks after W with wlast");
    ReqCapture down;
    WormholeArbiter<ni::cmodel::noc::NocReqOut> arb(
        down, /*num_inputs=*/3, {{0, 1}});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AW, /*last=*/0)));
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_W,  /*last=*/0, /*wlast=*/0)));
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_W,  /*last=*/0, /*wlast=*/0)));
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_W,  /*last=*/1, /*wlast=*/1)));

    for (int i = 0; i < 4; ++i) arb.tick();
    ASSERT_EQ(down.size(), 4u);
    EXPECT_FALSE(arb.is_locked());

    // Verify emission ORDER + per-flit header.last is correct
    auto f1 = down.pop(); ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->get_header_field("axi_ch"), ni::AXI_CH_AW);
    EXPECT_EQ(f1->get_header_field("last"),   0u);

    auto f2 = down.pop(); ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(f2->get_header_field("axi_ch"), ni::AXI_CH_W);
    EXPECT_EQ(f2->get_header_field("last"),   0u);

    auto f3 = down.pop(); ASSERT_TRUE(f3.has_value());
    EXPECT_EQ(f3->get_header_field("axi_ch"), ni::AXI_CH_W);
    EXPECT_EQ(f3->get_header_field("last"),   0u);

    auto f4 = down.pop(); ASSERT_TRUE(f4.has_value());
    EXPECT_EQ(f4->get_header_field("axi_ch"), ni::AXI_CH_W);
    EXPECT_EQ(f4->get_header_field("last"),   1u);
}

TEST(NocWormholeArbiter, NocRspOutVariantPassThrough) {
    SCENARIO("WormholeArbiter<NocRspOut> NSU instantiation: 2 inputs (B + R), "
             "no pairing, each flit is its own packet; verify template "
             "compiles + behaves identically for NocRspOut downstream type");
    using ni::cmodel::testing::RspCapture;
    RspCapture down;
    WormholeArbiter<ni::cmodel::noc::NocRspOut> arb(down, /*num_inputs=*/2, {});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_B, /*last=*/1)));
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_R, /*last=*/1)));
    arb.tick();
    arb.tick();
    EXPECT_EQ(down.size(), 2u);
    EXPECT_FALSE(arb.is_locked());
}

TEST(NocWormholeArbiter, BackpressureUpstreamAndDownstream) {
    SCENARIO("WormholeArbiter backpressure: input pending full -> push_flit "
             "returns false. Downstream credit_avail=false -> tick is idle.");
    // Downstream that refuses credit
    struct NoCreditDown : ni::cmodel::noc::NocReqOut {
        bool push_flit(const Flit&) override { return true; }
        bool credit_avail(uint8_t) const override { return false; }
    } no_credit;
    WormholeArbiter<ni::cmodel::noc::NocReqOut> arb(
        no_credit, /*num_inputs=*/2, {}, /*per_input_depth=*/2);

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));
    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));
    EXPECT_FALSE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));  // full

    arb.tick();  // downstream credit=false -> idle
    EXPECT_EQ(arb.pending_size(0), 2u);  // unchanged
}

TEST(NocWormholeArbiter, LockLeakIdleStallNoDeadlock) {
    SCENARIO("WormholeArbiter lock-leak / idle stall: AW emits and triggers lock, "
             "but no W ever arrives. tick many times -> arbiter idles (no spurious "
             "emit, no deadlock; AR remains pending, lock held).");
    ReqCapture down;
    WormholeArbiter<ni::cmodel::noc::NocReqOut> arb(
        down, /*num_inputs=*/3, {{0, 1}});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AW, /*last=*/0)));
    ASSERT_TRUE(arb.input(2).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));
    arb.tick();  // AW drains, lock to w
    for (int i = 0; i < 10; ++i) arb.tick();  // w pending empty, locked -> idle
    EXPECT_EQ(down.size(), 1u);          // only AW
    EXPECT_EQ(arb.pending_size(2), 1u);  // AR still pending
    EXPECT_TRUE(arb.is_locked());
}

// ---- Death tests (3) ----

TEST(NocWormholeArbiterDeath, WBeforeAW) {
    SCENARIO("WormholeArbiter NMU mode: pushing W to w input while unlocked "
             "(no preceding AW) violates upstream serialization; tick must "
             "assert+abort to fail fast");
    ReqCapture down;
    WormholeArbiter<ni::cmodel::noc::NocReqOut> arb(
        down, /*num_inputs=*/3, {{0, 1}});
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_W, /*last=*/1, /*wlast=*/1)));
    EXPECT_DEATH({ arb.tick(); }, "to-port flit pushed without preceding from-port flit");
}

TEST(NocWormholeArbiterDeath, MalformedAwLastEquals1) {
    SCENARIO("WormholeArbiter NMU mode: AW pushed with header.last=1 is malformed "
             "(violates FlooNoC wormhole AW=0 stamping); tick must assert+abort");
    ReqCapture down;
    WormholeArbiter<ni::cmodel::noc::NocReqOut> arb(
        down, /*num_inputs=*/3, {{0, 1}});
    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AW, /*last=*/1)));
    EXPECT_DEATH({ arb.tick(); }, "from-port flit with header.last=1");
}

TEST(NocWormholeArbiterDeath, LyingDownstream) {
    SCENARIO("WormholeArbiter: downstream lies (credit_avail=true but "
             "push_flit=false). tick must assert+abort on protocol violation.");
    struct LyingDown : ni::cmodel::noc::NocReqOut {
        bool push_flit(const Flit&) override { return false; }
        bool credit_avail(uint8_t) const override { return true; }
    } liar;
    WormholeArbiter<ni::cmodel::noc::NocReqOut> arb(liar, /*num_inputs=*/2, {});
    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));
    EXPECT_DEATH({ arb.tick(); }, "lying downstream");
}

TEST(NocWormholeArbiterDeath, CtorPairingValidation) {
    SCENARIO("WormholeArbiter ctor validates pairings: out-of-range index, "
             "from==to, duplicate from, nested chain (to is also a from). "
             "Each violation triggers assert+abort.");
    ReqCapture down;
    // Out of range
    EXPECT_DEATH({
        WormholeArbiter<ni::cmodel::noc::NocReqOut> arb(
            down, /*num_inputs=*/2, {{0, 5}});  // to=5 >= num_inputs
    }, "pairing out of range");
    // from == to
    EXPECT_DEATH({
        WormholeArbiter<ni::cmodel::noc::NocReqOut> arb(
            down, /*num_inputs=*/2, {{1, 1}});
    }, "pairing from == to");
    // Duplicate from
    EXPECT_DEATH({
        WormholeArbiter<ni::cmodel::noc::NocReqOut> arb(
            down, /*num_inputs=*/3, {{0, 1}, {0, 2}});
    }, "duplicate pairing.from");
    // Nested chain: to of one pairing is from of another
    EXPECT_DEATH({
        WormholeArbiter<ni::cmodel::noc::NocReqOut> arb(
            down, /*num_inputs=*/3, {{0, 1}, {1, 2}});
    }, "nested pairing chain");
}
```

- [ ] **Step 6: Build + run new tests — expect 11/11 PASS**

```bash
cmake --build c_model/build
ctest --test-dir c_model/build -R NocWormholeArbiter -V
```

Expected: 11 tests pass (7 functional incl. NocRspOut variant + 4 death incl. ctor pairing validation).

- [ ] **Step 7: Run full ctest — expect 370/370**

```bash
ctest --test-dir c_model/build -j 1
```

Expected: 370 tests pass (359 prior + 11 new).

- [ ] **Step 8: Commit**

```bash
git add c_model/include/noc/wormhole_arbiter.hpp \
        c_model/tests/common/per_channel_capture.hpp \
        c_model/tests/noc/CMakeLists.txt \
        c_model/tests/noc/test_wormhole_arbiter.cpp \
        c_model/tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(noc): add WormholeArbiter module + 9 unit tests

Template WormholeArbiter<Downstream> implements N-to-1 lock arbiter with
optional ChannelPairing config for AW->W pairing semantic. Use sites:
NMU req side (3 inputs aw/w/ar with pairing {{0,1}}), NSU rsp side (2
inputs b/r, no pairing). Reusable for NoC fabric router output ports
(Stage 4 round).

Lock semantic mirrors FlooNoC hw/floo_wormhole_arbiter.sv: drain a flit
with header.last=0 from pairing.from -> lock to pairing.to until a flit
with header.last=1 is drained from the locked target. Defensive guards
(Constraint A2): malformed AW (from-port with last=1), W-before-AW
(to-port unlocked), lying downstream all assert+abort with NDEBUG-safe
std::abort belt-and-braces.

Public ctor + deleted move/copy. InputAdapter holds raw parent pointer;
lifetime via stable named member or unique_ptr (not stack inside
std::vector). Documented in header.

New testbench mock PerChannelCapture<Interface> template captures flits
per output channel; used by NMU/NSU Packetize tests in next commit.

11 unit tests: 7 functional (pass-through NocReqOut, AW lock, AR
no-interleave, multi-beat W burst + unlock with ordered emission
asserts, backpressure, lock-leak idle stall, NocRspOut variant
pass-through) + 4 EXPECT_DEATH (W-before-AW, malformed AW, lying
downstream, ctor pairing validation x4 sub-cases).

New tests dir: c_model/tests/noc/. ctest: 359 -> 370.

Refs: docs/superpowers/specs/2026-06-04-wormhole-arbiter-design.md §5 §8

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Packetize multi-output refactor + integration testbench wire (BIG)

**Goal:** Change NMU `Packetize` ctor to take 3 NocReqOut refs (aw/w/ar) and NSU `Packetize` ctor to take 2 NocRspOut refs (b/r). Update ALL callers in one indivisible commit: existing Packetize unit tests (fixture → PerChannelCapture), Rob test, AxiSlavePort test, AxiMasterPort test (if Packetize is used), VcArbiter test fixtures, AND integration testbench (wire wormhole_arbiter between Packetize and VcArbiter).

This is the largest task. There is no compile-clean intermediate state — Packetize ctor signature change breaks every caller until all are updated.

**Files:**
- Modify: `c_model/include/nmu/packetize.hpp`
- Modify: `c_model/include/nsu/packetize.hpp`
- Modify: `c_model/tests/nmu/test_packetize.cpp`
- Modify: `c_model/tests/nsu/test_nsu_packetize.cpp`
- Modify: `c_model/tests/nmu/test_rob.cpp`
- Modify: `c_model/tests/nmu/test_axi_slave_port.cpp`
- Modify: `c_model/tests/nsu/test_axi_master_port.cpp` (if it uses Packetize)
- Modify: `c_model/tests/nmu/test_vc_arbiter.cpp`
- Modify: `c_model/tests/nsu/test_nsu_vc_arbiter.cpp`
- Modify: `c_model/tests/integration/test_request_response_loopback.cpp`

- [ ] **Step 1: Modify NMU `c_model/include/nmu/packetize.hpp` ctor signature**

Replace the ctor:

```cpp
// Before:
Packetize(noc::NocReqOut& req_out, uint8_t src_id)
    : req_out_(req_out), src_id_(src_id) {}

// After:
Packetize(noc::NocReqOut& aw_out,
          noc::NocReqOut& w_out,
          noc::NocReqOut& ar_out,
          uint8_t src_id)
    : aw_out_(aw_out), w_out_(w_out), ar_out_(ar_out), src_id_(src_id) {}
```

Replace member declarations:

```cpp
// Before:
noc::NocReqOut& req_out_;

// After:
noc::NocReqOut& aw_out_;
noc::NocReqOut& w_out_;
noc::NocReqOut& ar_out_;
```

Update each `req_out_.push_flit(f)` call site:
- In `push_aw_with_meta` (around line 122): change `req_out_.push_flit(f)` → `aw_out_.push_flit(f)`
- In `push_w` (around line 145): change to `w_out_.push_flit(f)`
- In `push_ar_with_meta` (around line 169): change to `ar_out_.push_flit(f)`

- [ ] **Step 2: Modify NSU `c_model/include/nsu/packetize.hpp` ctor**

```cpp
// Before:
Packetize(noc::NocRspOut& rsp_out, MetaBuffer& meta, uint8_t src_id)
    : rsp_out_(rsp_out), meta_(meta), src_id_(src_id) {}

// After:
Packetize(noc::NocRspOut& b_out,
          noc::NocRspOut& r_out,
          MetaBuffer& meta,
          uint8_t src_id)
    : b_out_(b_out), r_out_(r_out), meta_(meta), src_id_(src_id) {}
```

Member rename:
```cpp
// Before:
noc::NocRspOut& rsp_out_;

// After:
noc::NocRspOut& b_out_;
noc::NocRspOut& r_out_;
```

Update push_b (line 77): `rsp_out_.push_flit(f)` → `b_out_.push_flit(f)`
Update push_r (line 100): `rsp_out_.push_flit(f)` → `r_out_.push_flit(f)`

- [ ] **Step 3: Update `c_model/tests/nmu/test_packetize.cpp` fixture**

Replace all `LoopbackNoc noc(...); Packetize pkt(noc.req_out(), kSrcId);` with:

```cpp
#include "common/per_channel_capture.hpp"
// ...

using ni::cmodel::testing::ReqCapture;
// ...

ReqCapture aw_cap, w_cap, ar_cap;
Packetize pkt(aw_cap, w_cap, ar_cap, kSrcId);

// Replace noc.req_in().pop_flit() with explicit per-channel pop:
auto aw_flit = aw_cap.pop();
auto w_flit  = w_cap.pop();
auto ar_flit = ar_cap.pop();
```

For each existing test that uses `noc.req_q_size()` or similar, replace with explicit `aw_cap.size() + w_cap.size() + ar_cap.size()` or per-channel size check as appropriate to the test's intent.

- [ ] **Step 4: Update `c_model/tests/nsu/test_nsu_packetize.cpp` fixture**

Similarly:

```cpp
#include "common/per_channel_capture.hpp"
using ni::cmodel::testing::RspCapture;
// ...

RspCapture b_cap, r_cap;
Packetize pkt(b_cap, r_cap, meta, src_id);
```

Pop helpers: `b_cap.pop()`, `r_cap.pop()`.

- [ ] **Step 5: Update `c_model/tests/nmu/test_rob.cpp` fixture**

Rob ctor takes Packetizer&. Inside test setup, replace Packetize construction:

```cpp
// Before:
LoopbackNoc noc(...);
Packetize pkt(noc.req_out(), kSrcId);
Rob rob(pkt, ...);

// After:
ReqCapture aw_cap, w_cap, ar_cap;
Packetize pkt(aw_cap, w_cap, ar_cap, kSrcId);
Rob rob(pkt, ...);
```

For tests that verify flits arrive at NoC, switch from `noc.req_in().pop_flit()` to per-channel `*_cap.pop()`.

- [ ] **Step 6: Update `c_model/tests/nmu/test_axi_slave_port.cpp` fixture**

Same pattern: if the test wires Packetize, replace with 3-capture pattern.

- [ ] **Step 7: Check + update `c_model/tests/nsu/test_axi_master_port.cpp`**

```bash
grep -n "nsu::Packetize\|rsp_out()" c_model/tests/nsu/test_axi_master_port.cpp
```

If Packetize is constructed there, apply 2-capture pattern. Otherwise leave untouched.

- [ ] **Step 8: Update `c_model/tests/nmu/test_vc_arbiter.cpp` Packetize fixtures**

Find `EnabledModeMixedWith_PriorRoundTests` and `WHeaderLastMatchesWlast` tests. They construct Packetize. Funnel into VcArbiter via WormholeArbiter (matches new architecture).

Add include near top of file:
```cpp
#include "noc/wormhole_arbiter.hpp"
```

Then in each affected test, rewire as:
```cpp
LoopbackNoc noc(/*req*/64, /*rsp*/64);
auto vc_arb = VcArbiter::read_write_split(noc.req_out(), /*num_vc=*/1, 0, 0);
ni::cmodel::noc::WormholeArbiter<ni::cmodel::noc::NocReqOut> wh_arb(
    vc_arb, /*num_inputs=*/3,
    std::vector<ni::cmodel::noc::ChannelPairing>{{0, 1}});
Packetize pkt(wh_arb.input(0), wh_arb.input(1), wh_arb.input(2), kSrcId);
// tick loop: wh_arb.tick() then vc_arb.tick()
```

- [ ] **Step 9: Wire WormholeArbiter into `c_model/tests/integration/test_request_response_loopback.cpp`**

Add includes near existing nmu/nsu includes:

```cpp
#include "noc/wormhole_arbiter.hpp"
```

For NMU (around the existing `nmu::Packetize real_nmu_pkt(...)` line):

```cpp
// Before:
nmu::Packetize    real_nmu_pkt(nmu_arb, /*src_id=*/kNmuSrcId);

// After:
ni::cmodel::noc::WormholeArbiter<ni::cmodel::noc::NocReqOut> nmu_wh_arb(
    nmu_arb, /*num_inputs=*/3, {{0, 1}});
nmu::Packetize    real_nmu_pkt(nmu_wh_arb.input(0),
                               nmu_wh_arb.input(1),
                               nmu_wh_arb.input(2),
                               /*src_id=*/kNmuSrcId);
```

For NSU vector storage (parallel to existing nsu_arbs):

Declare BEFORE `nsu_pkts`:

```cpp
std::vector<std::unique_ptr<
    ni::cmodel::noc::WormholeArbiter<ni::cmodel::noc::NocRspOut>>>  nsu_wh_arbs;
nsu_wh_arbs.reserve(nsu_count);
```

Inside the per-NSU for loop, BEFORE `nsu_pkts.emplace_back`:

```cpp
nsu_wh_arbs.emplace_back(std::make_unique<
    ni::cmodel::noc::WormholeArbiter<ni::cmodel::noc::NocRspOut>>(
    *nsu_arbs[i], /*num_inputs=*/2,
    std::vector<ni::cmodel::noc::ChannelPairing>{},
    /*per_input_depth=*/4));
nsu_pkts.emplace_back(std::make_unique<nsu::Packetize>(
    nsu_wh_arbs[i]->input(0),  // b_out
    nsu_wh_arbs[i]->input(1),  // r_out
    *nsu_metas[i],
    this_nsu_src));
```

Add tick calls in per-cycle loop BEFORE `nmu_arb.tick()`:

```cpp
nmu_wh_arb.tick();
for (std::size_t i = 0; i < nsu_count; ++i) {
    nsu_wh_arbs[i]->tick();
}
nmu_arb.tick();    // existing
for (std::size_t i = 0; i < nsu_count; ++i) {
    nsu_arbs[i]->tick();  // existing
}
loopback.tick();   // existing
```

- [ ] **Step 10: Build — iteratively fix compile errors**

```bash
cmake --build c_model/build 2>&1 | head -50
```

Compile errors will be in callsites missing the new ctor pattern. Fix file by file. Each fixture rewire should ONLY change wiring, NOT test logic.

- [ ] **Step 11: Run full ctest — expect 370/370**

```bash
ctest --test-dir c_model/build -j 1
```

Expected: 370 tests pass (no new tests in this commit; only fixture rewiring + production ctor change). If any test fails, investigate root cause (likely fixture missed a callsite or wiring detail).

- [ ] **Step 11.5: Per-file diff review (spec §9 risk mitigation for indivisible commit)**

This commit is large (10+ files). Spec §9 requires per-file review to ensure each touched file is **wiring-only, no test logic change**. Run:

```bash
git diff --stat
```

Expected diffstat shape: ~10 files changed; per-file deltas mostly small (production `packetize.hpp` ctor signature + member rename = ~10-15 lines each; test fixture rewiring = ~5-20 lines each; integration testbench wormhole wiring = ~30-50 lines). If any single file shows >100 lines changed, scrutinize — that's a smell for unintended test logic edits.

For each touched test file, manually verify:
- Production files (`packetize.hpp` × 2): ctor signature + per-output routing only; no other logic change.
- Test files: fixture construction switched to `PerChannelCapture` and pop sites switched accordingly; **NO** changes to `EXPECT_EQ` / `ASSERT_TRUE` assertion VALUES or test names.
- Integration testbench: only insertion of WormholeArbiter wiring + tick calls; existing Rob/Packetize/VcArbiter wiring unchanged where compatible.

If any file's diff goes beyond wiring (e.g., assertion expected value changed), STOP, investigate, and either confirm it's required by the refactor or revert. Do not commit until each file's diff is wiring-only confirmed.

- [ ] **Step 12: Commit**

```bash
git add c_model/include/nmu/packetize.hpp \
        c_model/include/nsu/packetize.hpp \
        c_model/tests/nmu/test_packetize.cpp \
        c_model/tests/nsu/test_nsu_packetize.cpp \
        c_model/tests/nmu/test_rob.cpp \
        c_model/tests/nmu/test_axi_slave_port.cpp \
        c_model/tests/nsu/test_axi_master_port.cpp \
        c_model/tests/nmu/test_vc_arbiter.cpp \
        c_model/tests/nsu/test_nsu_vc_arbiter.cpp \
        c_model/tests/integration/test_request_response_loopback.cpp
git commit -m "$(cat <<'EOF'
refactor(packetize): multi-output ctor + wire WormholeArbiter into integration

nmu::Packetize ctor now takes 3 NocReqOut& (aw/w/ar) instead of single
req_out. push_aw/push_w/push_ar route to corresponding output. w_meta_fifo_
stays in Packetize (W inherits AW's dst_id/rob_idx; orthogonal to channel
locking which is wormhole_arbiter's job).

nsu::Packetize ctor takes 2 NocRspOut& (b/r); push_b/push_r route
correspondingly.

Integration testbench (test_request_response_loopback.cpp):
- NMU: WormholeArbiter<NocReqOut> (3 inputs, pairing {{0,1}}) between
  Packetize and VcArbiter
- NSU: per-NSU std::vector<std::unique_ptr<WormholeArbiter<NocRspOut>>>
  (2 inputs, no pairing) declared before nsu_pkts for LIFO destruction
  safety
- Per-cycle tick loop: wormhole ticks before vc_arbiter ticks before
  loopback.tick()

Unit test fixture rewiring (Packetize tests, Rob test, AxiSlavePort test,
AxiMasterPort test if applicable, VcArbiter Packetize fixtures): use
new PerChannelCapture<Interface> mock to capture per-channel flits.
Test logic unchanged; only construction/assertion sites updated.

Indivisible commit: Packetize ctor signature change has no compile-clean
intermediate state. All Packetize callers updated together.

ctest: 370/370 unchanged.

Refs: docs/superpowers/specs/2026-06-04-wormhole-arbiter-design.md §6 §9

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Simplify NMU `VcArbiter` (deque → optional)

**Goal:** Replace NMU `VcArbiter::pending_w_routes_` deque with `current_aw_vc_` single optional. Asserts enforce Constraint A1 (must be downstream of WormholeArbiter). Restructure existing `WFollowsAW_InvariantEnforced` test to single-AW + multi-W scenario. NSU VcArbiter unchanged.

**Files:**
- Modify: `c_model/include/nmu/vc_arbiter.hpp`
- Modify: `c_model/tests/nmu/test_vc_arbiter.cpp`

- [ ] **Step 1: Modify `c_model/include/nmu/vc_arbiter.hpp`**

Replace member declaration (around line 111):

```cpp
// Before:
std::deque<uint8_t> pending_w_routes_;

// After:
std::optional<uint8_t> current_aw_vc_;
```

Remove `#include <deque>` if no longer used (verify with grep first; `deque` still used in `pending_` member).

Replace `pending_w_routes_size()` introspection method:

```cpp
// Before:
std::size_t pending_w_routes_size() const noexcept { return pending_w_routes_.size(); }

// After:
bool has_current_aw() const noexcept { return current_aw_vc_.has_value(); }
```

Replace `select_vc_for_axi_ch` — move W invariant check BEFORE `num_vc_==1` fast path so the assert fires regardless of NUM_VC (Constraint A1 holds universally). Replace the entire method body (lines ~114-138) with:

```cpp
inline std::optional<uint8_t> VcArbiter::select_vc_for_axi_ch(uint8_t axi_ch) {
    // W invariant fires regardless of NUM_VC (Constraint A1: must be
    // downstream of WormholeArbiter; W must always follow AW)
    if (axi_ch == ni::AXI_CH_W) {
        if (!current_aw_vc_.has_value()) {
            assert(false && "nmu::VcArbiter::push_flit: W arrived with no current AW VC -- "
                            "Constraint A1 violated: must be downstream of WormholeArbiter "
                            "(which serializes AW + all W beats before next AW). Standalone "
                            "VcArbiter use without upstream serialization is unsupported.");
            std::abort();
        }
        return *current_aw_vc_;
    }

    if (num_vc_ == 1) return uint8_t{0};

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
```

Replace push_flit AW/W transition (around lines 148-154):

```cpp
// Before:
if (axi_ch == ni::AXI_CH_AW) {
    pending_w_routes_.push_back(vc_id);
} else if (axi_ch == ni::AXI_CH_W) {
    if (flit.get_payload_field("W", "wlast") != 0) {
        pending_w_routes_.pop_front();
    }
}

// After:
if (axi_ch == ni::AXI_CH_AW) {
    assert(!current_aw_vc_.has_value() &&
           "nmu::VcArbiter::push_flit: AW arrived while previous AW's W burst "
           "still in progress -- Constraint A1 violated: must be downstream of "
           "WormholeArbiter (which holds next AW until current W burst ends).");
    current_aw_vc_ = vc_id;
} else if (axi_ch == ni::AXI_CH_W) {
    if (flit.get_payload_field("W", "wlast") != 0) {
        current_aw_vc_.reset();
    }
}
```

- [ ] **Step 2: Restructure `WFollowsAW_InvariantEnforced` in `c_model/tests/nmu/test_vc_arbiter.cpp`**

Replace the existing test (which pushed 5 AWs + 5 Ws in batched fashion) with a single-AW + multi-W scenario:

```cpp
TEST(NmuVcArbiterParam, WFollowsAW_InvariantEnforced) {
    SCENARIO("NMU VcArbiter NUM_VC=2: single outstanding AW + 3 W beats (last "
             "with wlast=1) — all W beats route to AW's VC. After W with wlast, "
             "current_aw_vc_ resets, allowing next AW to be pushed.");
    const std::size_t num_vc = GetParam();
    if (num_vc < 2) GTEST_SKIP() << "needs num_vc >= 2";

    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    auto arb = VcArbiter::read_write_split(noc.req_out(), num_vc, 0, 1);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
    EXPECT_TRUE(arb.has_current_aw());

    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/0)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/0)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/1)));
    EXPECT_FALSE(arb.has_current_aw());  // reset after wlast

    // All 4 flits land on VC=0 (write_vc)
    EXPECT_EQ(arb.pending_size(0), 4u);
    EXPECT_EQ(arb.pending_size(1), 0u);

    // Next AW can now be pushed (current_aw_vc_ is clear)
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
}
```

Find any other test that references `pending_w_routes_size()` and replace with `has_current_aw()` accordingly.

For `WlastFromPayloadNotHeader` test (also TEST_P), update assertions on pending_w_routes_size:

```cpp
// Before:
EXPECT_EQ(arb.pending_w_routes_size(), 1u);

// After:
EXPECT_TRUE(arb.has_current_aw());
```

- [ ] **Step 3: Build + run NMU VcArbiter tests**

```bash
cmake --build c_model/build
ctest --test-dir c_model/build -R "NmuVcArbiter\|NmuVcArbiterParam\|NmuVcArbiterDeath" -V
```

Expected: all pass. If `WFollowsAW_InvariantEnforced` was previously testing multi-AW scenarios in unparameterized form, those instantiations would now fail assert (Constraint A1) — that means the test SHOULD be restructured per Step 2, not adapted to bypass.

- [ ] **Step 4: Run full ctest — expect 370/370**

```bash
ctest --test-dir c_model/build -j 1
```

Expected: 370/370 (no count change; just internal refactor).

- [ ] **Step 5: Commit**

```bash
git add c_model/include/nmu/vc_arbiter.hpp c_model/tests/nmu/test_vc_arbiter.cpp
git commit -m "$(cat <<'EOF'
refactor(vc_arbiter): simplify pending_w_routes deque -> optional (Constraint A1)

WormholeArbiter (commit prev) now serializes AW + all W beats before next
AW upstream of VcArbiter. VcArbiter no longer needs to track multiple
outstanding AW VC assignments via deque; a single current_aw_vc_ optional
suffices.

Behavior change (Constraint A1):
- On AW push: assert current_aw_vc_ unset; set to chosen VC
- On W push: assert current_aw_vc_ set; route to that VC
- On W with payload.wlast=1: reset current_aw_vc_
- Standalone VcArbiter (without WormholeArbiter upstream) will fail assert
  on multi-outstanding AW push. Documented in assert message.

WFollowsAW_InvariantEnforced test restructured to single-AW + multi-W
scenario (the multi-AW scenario is no longer valid given Constraint A1).
WFollowsAW_WBeforeAW_DeathTest unchanged (still catches violation).

NSU VcArbiter unchanged (no AW/W concept).

ctest: 370/370 unchanged.

Refs: docs/superpowers/specs/2026-06-04-wormhole-arbiter-design.md §7 §10

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Update `NEXT_STEPS.md`

**Goal:** Karpathy 4-lens summary of this round + flip pointer to nmu/nsu top-level next round.

**Files:**
- Modify: `NEXT_STEPS.md`

- [ ] **Step 1: Read current NEXT_STEPS.md to find prior round's "Just done" + "Next round" section**

```bash
cat NEXT_STEPS.md
```

- [ ] **Step 2: Replace prior round's "Just done" + "Next round" sections**

Append (after existing history, if any preserved) OR replace the most recent "Just done" block:

```markdown
## Just done (this round): wormhole_arbiter + Packetize multi-output

**Karpathy 4-lens summary**:
- **What we shipped**: noc/wormhole_arbiter.hpp (template, N-to-1 lock
  arbiter with optional ChannelPairing config for AW->W pairing) + Packetize
  multi-output refactor (NMU 3 outputs aw/w/ar, NSU 2 outputs b/r) +
  VcArbiter simplification (deque -> optional, enabled by upstream wormhole
  serialization, Constraint A1) + VcArb -> VcArbiter full-word rename +
  integration testbench wiring (NMU + per-NSU wormhole). 9 new tests; ctest
  359 -> 368.
- **What we proved**: 5->2 AXI-to-NoC channel mapping now sits in dedicated
  module matching FlooNoC chimney pattern; AW+W wormhole packet locks
  correctly across multi-beat W bursts; defensive guards catch malformed
  AW / W-before-AW / lying-downstream at runtime (Constraint A2); integration
  testbench 324/324 unchanged (decorator transparent at NUM_VC=1).
- **What we owe**: defer to future rounds -- nmu.hpp/nsu.hpp top-level
  assembly (next round; absorbs the integration wiring), NoC fabric router
  (Stage 4; reuses WormholeArbiter at output ports), header field stubs
  (route_par/commtype/multicast/noc_qos/flit_ecc), addr_trans algorithm
  alternatives, VcArbiter weighted RR / starvation detection / dynamic
  remap / YAML-driven candidate config, num_inputs=1 pass-through degenerate
  mode (intentionally dropped).
- **Why it matters now**: NI architecture now FlooNoC-aligned end-to-end
  (header.last semantic + wormhole packet locking + per-VC arbitration).
  nmu.hpp / nsu.hpp top-level (next round) can package the entire pipeline
  as a single class instead of the integration testbench's manual wiring.

## Next round: nmu.hpp / nsu.hpp top-level assembly

Per main plan section 3 file structure rows 168 + 173: assemble
addr_trans + AxiSlavePort + Rob + Packetize + WormholeArbiter + VcArbiter
into nmu::Nmu class (NMU side) and Depacketize + MetaBuffer + Packetize +
WormholeArbiter + VcArbiter into nsu::Nsu class (NSU side). Each exposes
clean external pin-struct interface (AxiSlavePortPins on AXI side,
NocReqOut/NocRspIn on NoC side). Integration testbench can then use
nmu::Nmu / nsu::Nsu instances directly instead of manually wiring
sub-components.

After top-level done = Stage 3 NI completely closed; ready for Stage 4
NoC fabric (noc/router.hpp).
```

- [ ] **Step 3: Optional — add 1-line note to main plan §3 about wormhole_arbiter location**

```bash
grep -n "nmu/depacketize" docs/noc_cmodel_rtl_plan.md
```

Find the line listing NMU files (around line 167) and append after it a brief note:

> Note: `wormhole_arbiter` lives at NI side (matches FlooNoC `floo_axi_chimney.sv:744/758`), not at NoC fabric router level. Router output ports may reuse the same `noc/wormhole_arbiter.hpp` template at Stage 4.

(Skip if main plan editing is out of scope; commit message records the architectural decision either way.)

- [ ] **Step 4: Run full ctest — expect 370/370 unchanged**

```bash
ctest --test-dir c_model/build -j 1
```

- [ ] **Step 5: Commit**

```bash
git add NEXT_STEPS.md docs/noc_cmodel_rtl_plan.md
git commit -m "$(cat <<'EOF'
docs(NEXT_STEPS): wormhole_arbiter round done; next is nmu/nsu top-level

wormhole_arbiter round delivered: noc/wormhole_arbiter.hpp (template
N-to-1 lock arbiter), Packetize multi-output refactor (NMU 3 / NSU 2),
VcArbiter deque -> optional simplification (Constraint A1), VcArb ->
VcArbiter rename, integration testbench wiring. 5 commits, 9 new tests
(359 -> 368).

Next round: nmu.hpp + nsu.hpp top-level assembly. After that = Stage 3
NI closed; ready for Stage 4 NoC fabric (noc/router.hpp).

Karpathy 4-lens recap + main plan section 3 noted (wormhole_arbiter lives
at NI side per FlooNoC pattern, not at router level).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

After all 5 commits:

```bash
cd specgen
py -3 -m pytest -q
py -3 tools/codegen.py --check
py -3 tools/gen_inventory.py --check
cd ..
cmake --build c_model/build
ctest --test-dir c_model/build -j 1
git log --oneline e72bb43..HEAD
```

Expected:
- specgen pytest: 163 passed
- codegen / inventory: clean
- ctest: 370/370
- git log: 5 commits on `stage3/packetize-depacketize`

---

## Self-review notes (controller, not for implementer)

1. **Task 3 is large** (10+ files, 1 big commit). Risk mitigation per spec §9: each file reviewed individually for "wiring only, no logic change"; Codex review on diff before commit.
2. **VcArbiter assert messages** explicitly mention Constraint A1 ("must be downstream of WormholeArbiter") so future maintainers see the assumption at the failure site.
3. **`WFollowsAW_InvariantEnforced` restructure**: the original multi-AW + multi-W batched scenario is no longer valid (would fire Constraint A1 assert). Restructured to single-AW + multi-W which is the actual supported pattern.
4. **NUM_VC parameterized matrix from vc_arb round preserved**: TEST_P + INSTANTIATE_TEST_SUITE_P over {1,2,4,8} on VcArbiter tests carry forward; new wormhole_arbiter tests are NOT parameterized (per user "化繁為簡" preference + arbiter's NUM_VC is orthogonal — it sees vc_id only as a credit_avail parameter).
5. **PerChannelCapture mock** is a 1-file template, ~30 LOC, simple capture-and-pop semantic. Low risk.
