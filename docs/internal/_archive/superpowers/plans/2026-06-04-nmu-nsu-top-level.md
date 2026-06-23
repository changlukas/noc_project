# Nmu / Nsu top-level assembly Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Encapsulate Stage 3 NI manual integration wiring (~7 NMU sub-modules + 6 NSU + tick orchestration) into single `nmu::Nmu` and `nsu::Nsu` classes each with one `tick()` entrypoint — across 3 commits, ctest 336 → 338 (+2 smoke tests for Nmu/Nsu ctor sequence verification).

**Architecture:** Stack-member composition (delete move/copy because WormholeArbiter is non-movable). NMU pipeline: `AxiSlavePort → Rob → Packetize → WormholeArbiter → VcArbiter → external NocReqOut`, plus `Depacketize ← external NocRspIn`. NSU pipeline: `Depacketize → AxiMasterPort → external AXI slave`, plus `Packetize → WormholeArbiter → VcArbiter → external NocRspOut`, with `MetaBuffer` shared between Depacketize and Packetize. AXI side exposed via getter (`axi_slave_port()` / `axi_master_port()`) — NOT ctor-bound, due to template type mismatch with testbench's `AxiMasterT<AxiSlavePort>`.

**Tech Stack:** C++17, GoogleTest, CMake, Windows PowerShell + git-bash. `.clang-format` enforced (Google base + 4-space indent + 100col).

**Spec:** `docs/superpowers/specs/2026-06-04-nmu-nsu-top-level-design.md` (Codex r2 APPROVED).

**Branch:** `stage3/packetize-depacketize` (commits accumulate; Stage 3 closing).

---

## Implementation notes

- **Bash heredoc for commit messages**: `$(cat <<'EOF' ... EOF)`. PowerShell `@'...'@` does NOT work in git-bash.
- **Run clang-format after every file edit**: `clang-format -i <file>` per memory `feedback-clang-format-industry-style`.
- **Member declaration order matters**: ctors run in declaration order; sub-modules that take ref to another sub-module MUST be declared AFTER what they reference.

---

## File Structure

| Path | Action | Responsibility |
|---|---|---|
| `c_model/include/nmu/nmu.hpp` | CREATE | `nmu::Nmu` class + `NmuConfig` struct |
| `c_model/include/nsu/nsu.hpp` | CREATE | `nsu::Nsu` class + `NsuConfig` struct |
| `c_model/tests/integration/test_request_response_loopback.cpp` | MODIFY | Replace ~7-module manual wiring with `Nmu`/`Nsu` instances; tick loop simplifies to ~4 calls |

No CMakeLists changes (existing tests/integration target picks up the new headers via include path).

### Drift gates (run before each commit)

```bash
cd "E:/05_NoC/noc_project"
cmake --build c_model/build -j 1
ctest --test-dir c_model/build -j 1   # expect 336/336
```

Baseline at HEAD `77c1f5b` (spec doc): 336/336 green.

---

## Task 1: `nmu::Nmu` top-level class

**Goal:** Create `c_model/include/nmu/nmu.hpp` with `NmuConfig` struct + `Nmu` class encapsulating AxiSlavePort + Rob + Packetize + WormholeArbiter + VcArbiter + Depacketize.

**Files:**
- Create: `c_model/include/nmu/nmu.hpp`

**Ctor refs from existing sub-modules** (verified against current source):
- `AxiSlavePort(Packetizer&, Depacketizer&, PortParams)` — Packetizer + Depacketizer are both `Rob` (Rob multi-inherits)
- `Rob(Packetize&, Depacketizer&, RobMode mode_w, RobMode mode_r)` — Packetize forwards request; Depacketize forwards response
- `Packetize(NocReqOut& aw_out, NocReqOut& w_out, NocReqOut& ar_out, uint8_t src_id)` — 3 outputs to WormholeArbiter inputs
- `Depacketize(NocRspIn& rsp_in, std::size_t b_q_depth, std::size_t r_q_depth)` — NMU side handles B/R from NoC
- `WormholeArbiter<NocReqOut>(Downstream&, num_inputs, pairings, per_input_depth)`
- `VcArbiter::read_write_split(NocReqOut& downstream, num_vc, write_vc, read_vc, pending_depth)` or `multi_candidate(...)` factory

- [ ] **Step 1: Create `c_model/include/nmu/nmu.hpp`**

```cpp
#pragma once
// NMU top-level assembly. Encapsulates Stage 3 NI sub-modules into one
// class with a single tick() entrypoint, hiding the manual wiring that
// previously lived in test_request_response_loopback.cpp.
//
// Pipeline (req path):
//   external AXI master ──> AxiSlavePort ──> Rob ──> Packetize{aw,w,ar}
//     ──> WormholeArbiter<NocReqOut>(3 in, pairing {{0,1}}) ──> VcArbiter
//     ──> external NocReqOut (LoopbackNoc or DPI bridge)
//
// Pipeline (rsp path):
//   external NocRspIn ──> Depacketize ──> Rob ──> AxiSlavePort
//     ──> back to external AXI master
//
// Per-cycle tick order (upstream-first; matches vc_arb/wormhole_arbiter
// round established pattern):
//   depacketize_.tick(); axi_slave_port_.tick();
//   wormhole_arbiter_.tick(); vc_arbiter_.tick();
//
// Lifetime: Nmu deletes move/copy (WormholeArbiter is non-movable).
// Member declaration order respects ctor ref dependencies — see private
// section comment for explanation.
//
// AXI binding: NOT via ctor (AxiMasterT<AxiSlavePort> template type
// collision in testbench). Use axi_slave_port() getter to obtain the
// AxiSlavePort& for the testbench's AxiMaster<AxiSlavePort> wiring.
//
// References:
//   docs/superpowers/specs/2026-06-04-nmu-nsu-top-level-design.md
//   docs/noc_cmodel_rtl_plan.md section 3 row 168
#include "ni/port_params.hpp"
#include "nmu/axi_slave_port.hpp"
#include "nmu/depacketize.hpp"
#include "nmu/packetize.hpp"
#include "nmu/rob.hpp"
#include "nmu/vc_arbiter.hpp"
#include "noc/noc_req_out.hpp"
#include "noc/noc_rsp_in.hpp"
#include "noc/wormhole_arbiter.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ni::cmodel::nmu {

struct NmuConfig {
    uint8_t src_id = 0;
    RobMode read_rob_mode = RobMode::Disabled;
    RobMode write_rob_mode = RobMode::Disabled;
    PortParams port_params{};
    std::size_t depkt_b_q_depth = 16;    // NMU Depacketize: B response queue
    std::size_t depkt_r_q_depth = 16;    // NMU Depacketize: R response queue
    std::size_t num_vc = 1;
    VcMode vc_mode = VcMode::ReadWriteSplit;
    uint8_t write_vc = 0;
    uint8_t read_vc = 0;
    // Mode B candidate arrays; ignored when vc_mode == ReadWriteSplit.
    // Indexed by axi_ch (AW=0, W=1, AR=2, B=3, R=4 per ni_flit_constants).
    std::array<std::vector<uint8_t>, 5> vc_candidates{};
    std::size_t wormhole_per_input_depth = 4;
    std::size_t vc_arbiter_pending_depth = 4;
};

class Nmu {
  public:
    Nmu(NmuConfig cfg, noc::NocReqOut& downstream_req, noc::NocRspIn& downstream_rsp);

    Nmu(const Nmu&) = delete;
    Nmu(Nmu&&) = delete;
    Nmu& operator=(const Nmu&) = delete;
    Nmu& operator=(Nmu&&) = delete;

    // AXI facade for testbench wiring (AxiMaster<AxiSlavePort> binds here).
    AxiSlavePort& axi_slave_port() noexcept { return axi_slave_port_; }

    // Per-cycle tick — orchestrates sub-modules in upstream-first order.
    void tick();

    // Test introspection (optional getters; add only as test code needs)
    const Rob& rob() const noexcept { return rob_; }
    const VcArbiter& vc_arbiter() const noexcept { return vc_arbiter_; }

  private:
    // Declaration order respects ctor ref dependencies:
    //   1. cfg_ + external downstream refs (no deps).
    //   2. vc_arbiter_ wraps downstream_req_.
    //   3. wormhole_arbiter_ wraps vc_arbiter_ as its Downstream.
    //   4. depacketize_ wraps downstream_rsp_ (req path independent).
    //   5. packetize_ takes wormhole_arbiter_.input(0/1/2) (req path).
    //   6. rob_ takes packetize_ + depacketize_.
    //   7. axi_slave_port_ takes rob_ (as Packetizer + Depacketizer via multi-inherit).
    NmuConfig cfg_;
    noc::NocReqOut& downstream_req_;
    noc::NocRspIn& downstream_rsp_;
    VcArbiter vc_arbiter_;
    noc::WormholeArbiter<noc::NocReqOut> wormhole_arbiter_;
    Depacketize depacketize_;
    Packetize packetize_;
    Rob rob_;
    AxiSlavePort axi_slave_port_;
};

namespace detail {

inline VcArbiter make_vc_arbiter(const NmuConfig& cfg, noc::NocReqOut& downstream) {
    if (cfg.vc_mode == VcMode::ReadWriteSplit) {
        return VcArbiter::read_write_split(downstream, cfg.num_vc, cfg.write_vc, cfg.read_vc,
                                           cfg.vc_arbiter_pending_depth);
    }
    auto candidates = cfg.vc_candidates;  // copy; factory consumes by value
    return VcArbiter::multi_candidate(downstream, cfg.num_vc, std::move(candidates),
                                      cfg.vc_arbiter_pending_depth);
}

}  // namespace detail

inline Nmu::Nmu(NmuConfig cfg, noc::NocReqOut& downstream_req, noc::NocRspIn& downstream_rsp)
    : cfg_(std::move(cfg)),
      downstream_req_(downstream_req),
      downstream_rsp_(downstream_rsp),
      vc_arbiter_(detail::make_vc_arbiter(cfg_, downstream_req_)),
      wormhole_arbiter_(vc_arbiter_, /*num_inputs=*/3,
                        std::vector<noc::ChannelPairing>{{0, 1}},
                        cfg_.wormhole_per_input_depth),
      depacketize_(downstream_rsp_, cfg_.depkt_b_q_depth, cfg_.depkt_r_q_depth),
      packetize_(wormhole_arbiter_.input(0), wormhole_arbiter_.input(1),
                 wormhole_arbiter_.input(2), cfg_.src_id),
      rob_(packetize_, depacketize_, cfg_.write_rob_mode, cfg_.read_rob_mode),
      axi_slave_port_(rob_, rob_, cfg_.port_params) {}

inline void Nmu::tick() {
    depacketize_.tick();
    axi_slave_port_.tick();
    wormhole_arbiter_.tick();
    vc_arbiter_.tick();
}

}  // namespace ni::cmodel::nmu
```

- [ ] **Step 2: Run clang-format**

```bash
clang-format -i c_model/include/nmu/nmu.hpp
```

- [ ] **Step 3: Build — verify compile clean**

```bash
cmake --build c_model/build -j 1
```
Expected: clean build (no errors, no warnings from new header). If `VcArbiter::read_write_split` factory return-by-value clashes with `delete move` on inner sub-objects, fix by storing factory result via direct ctor call in the member initializer list (member-init does NOT use move ctor when factory return is a prvalue; C++17 mandatory copy elision applies).

Note: if compile fails on `make_vc_arbiter` helper because VcArbiter has private ctor + only factories, the inline `detail::make_vc_arbiter` helper IS the factory call site — should work via prvalue mandatory elision.

- [ ] **Step 4: Create smoke test `c_model/tests/nmu/test_nmu.cpp`** (per spec §8 commit 1 smoke test requirement)

```cpp
// Smoke test: Nmu class constructs cleanly + tick() doesn't crash.
// Verifies ctor sequence (member init order, factory return-by-value via
// detail::make_vc_arbiter, sub-module ref dependencies) before Task 3
// integration. Does NOT exercise full e2e flow; that's integration testbench.
#include "common/loopback_noc.hpp"
#include "common/scenario.hpp"
#include "nmu/nmu.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nmu::Nmu;
using ni::cmodel::nmu::NmuConfig;
using ni::cmodel::testing::LoopbackNoc;

TEST(NmuTopLevel, ConstructsAndTicksWithoutCrash) {
    SCENARIO("Nmu top-level smoke: NUM_VC=1 default config, construct + tick 10x "
             "should not crash. Verifies ctor sequence + member init order.");
    LoopbackNoc loopback(/*req*/64, /*rsp*/64);
    NmuConfig cfg{};
    cfg.src_id = 0x12;
    Nmu nmu(cfg, loopback.nmu_req_out(), loopback.nmu_rsp_in());

    EXPECT_EQ(&nmu.axi_slave_port(), &nmu.axi_slave_port())
        << "axi_slave_port() returns stable reference";

    for (int i = 0; i < 10; ++i) {
        nmu.tick();
        loopback.tick();
    }
    SUCCEED();  // reaching here means no abort during ctor or tick
}
```

- [ ] **Step 5: Register smoke test in `c_model/tests/nmu/CMakeLists.txt`**

Append:
```cmake
add_cmodel_test(test_nmu)
target_include_directories(test_nmu PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

- [ ] **Step 6: Run clang-format on smoke test**

```bash
clang-format -i c_model/tests/nmu/test_nmu.cpp
```

- [ ] **Step 7: Run full ctest — expect 337/337**

```bash
cmake --build c_model/build -j 1
ctest --test-dir c_model/build -j 1
```
Expected: 337/337 (336 baseline + 1 new smoke test). If smoke test fails, ctor sequence has a bug — investigate before committing.

- [ ] **Step 8: Commit**

```bash
git add c_model/include/nmu/nmu.hpp c_model/tests/nmu/test_nmu.cpp c_model/tests/nmu/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(nmu): add Nmu top-level class encapsulating sub-modules

Nmu wraps AxiSlavePort + Rob + Packetize + WormholeArbiter<NocReqOut>
+ VcArbiter + Depacketize into one class. Single tick() entrypoint
orchestrates sub-modules in upstream-first order matching vc_arb /
wormhole_arbiter rounds (depacketize -> axi_slave_port -> wormhole ->
vc_arbiter).

Stack members with deleted move/copy (WormholeArbiter is non-movable).
Member declaration order respects ctor ref dependencies (vc_arbiter
before wormhole_arbiter wraps it, packetize takes wormhole inputs, etc.)
-- documented in private section comment.

AXI binding via axi_slave_port() getter, NOT ctor (avoids template type
mismatch with testbench AxiMasterT<AxiSlavePort>).

NmuConfig struct centralizes all sub-module config (src_id, rob modes,
NUM_VC, vc_mode, pairings, depths). Sensible defaults; testbench overrides
only what it needs.

Pure addition; existing tests untouched. 1 smoke test added (Nmu
constructs + ticks without crash, verifying ctor sequence + member
init order before Task 3 integration). ctest 336 -> 337.
Integration testbench refactor lands in commit 3 of this round.

Refs: docs/superpowers/specs/2026-06-04-nmu-nsu-top-level-design.md
      docs/noc_cmodel_rtl_plan.md section 3 row 168

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `nsu::Nsu` top-level class

**Goal:** Create `c_model/include/nsu/nsu.hpp` mirror of Nmu for NSU side. Asymmetric: no Rob, no addr_trans; MetaBuffer between Depacketize and Packetize.

**Files:**
- Create: `c_model/include/nsu/nsu.hpp`

**Ctor refs from existing sub-modules**:
- `AxiMasterPort(Depacketizer&, Packetizer&, PortParams)` — Depacketizer forwards req; Packetizer receives rsp
- `Depacketize(NocReqIn& req_in, MetaBuffer& meta, aw_q_depth, w_q_depth, ar_q_depth)`
- `MetaBuffer(per_id_depth)`
- `Packetize(NocRspOut& b_out, NocRspOut& r_out, MetaBuffer& meta, uint8_t src_id)`
- `WormholeArbiter<NocRspOut>(Downstream&, num_inputs, pairings, per_input_depth)` — 2 in, no pairing
- `VcArbiter::read_write_split(NocRspOut& downstream, num_vc, write_rsp_vc, read_rsp_vc, pending_depth)`

- [ ] **Step 1: Create `c_model/include/nsu/nsu.hpp`**

```cpp
#pragma once
// NSU top-level assembly. Encapsulates Stage 3 NI response-side
// sub-modules into one class with single tick() entrypoint. Mirror of
// nmu::Nmu but asymmetric: NSU has no Rob (no reorder buffer on response
// side) and no addr_trans (uses incoming flit dst_id directly).
//
// Pipeline (req in, AXI out):
//   external NocReqIn ──> Depacketize (snapshots meta to MetaBuffer)
//     ──> AxiMasterPort ──> external AXI slave
//
// Pipeline (rsp from AXI slave, NoC out):
//   external B/R from AXI slave ──> AxiMasterPort ──> Packetize{b,r}
//     (reads meta from MetaBuffer) ──> WormholeArbiter<NocRspOut>(2 in,
//     no pairing) ──> VcArbiter ──> external NocRspOut
//
// Per-cycle tick order (upstream-first):
//   depacketize_.tick(); axi_master_port_.tick();
//   wormhole_arbiter_.tick(); vc_arbiter_.tick();
//
// Lifetime: Nsu deletes move/copy. Member order respects ctor ref deps.
//
// AXI binding: axi_master_port() getter. Testbench wires its
// AxiSlave-side adapters through this getter.
//
// References:
//   docs/superpowers/specs/2026-06-04-nmu-nsu-top-level-design.md
//   docs/noc_cmodel_rtl_plan.md section 3 row 173
#include "ni/port_params.hpp"
#include "noc/noc_req_in.hpp"
#include "noc/noc_rsp_out.hpp"
#include "noc/wormhole_arbiter.hpp"
#include "nsu/axi_master_port.hpp"
#include "nsu/depacketize.hpp"
#include "nsu/meta_buffer.hpp"
#include "nsu/packetize.hpp"
#include "nsu/vc_arbiter.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ni::cmodel::nsu {

struct NsuConfig {
    uint8_t src_id = 0;
    PortParams port_params{};
    std::size_t meta_buffer_per_id_depth = 16;
    std::size_t depkt_aw_q_depth = 16;
    std::size_t depkt_w_q_depth = 16;
    std::size_t depkt_ar_q_depth = 16;
    std::size_t num_vc = 1;
    VcMode vc_mode = VcMode::ReadWriteSplit;
    uint8_t write_rsp_vc = 0;    // B → write_rsp_vc
    uint8_t read_rsp_vc = 0;     // R → read_rsp_vc
    std::array<std::vector<uint8_t>, 5> vc_candidates{};
    std::size_t wormhole_per_input_depth = 4;
    std::size_t vc_arbiter_pending_depth = 4;
};

class Nsu {
  public:
    Nsu(NsuConfig cfg, noc::NocReqIn& upstream_req, noc::NocRspOut& downstream_rsp);

    Nsu(const Nsu&) = delete;
    Nsu(Nsu&&) = delete;
    Nsu& operator=(const Nsu&) = delete;
    Nsu& operator=(Nsu&&) = delete;

    AxiMasterPort& axi_master_port() noexcept { return axi_master_port_; }

    void tick();

  private:
    // Declaration order:
    //   1. cfg_ + external refs.
    //   2. vc_arbiter_ wraps downstream_rsp_.
    //   3. wormhole_arbiter_ wraps vc_arbiter_.
    //   4. meta_buffer_ (no upstream dep).
    //   5. packetize_ takes wormhole_arbiter_.input(0/1) + meta_buffer_.
    //   6. depacketize_ takes upstream_req_ + meta_buffer_.
    //   7. axi_master_port_ takes depacketize_ + packetize_.
    NsuConfig cfg_;
    noc::NocReqIn& upstream_req_;
    noc::NocRspOut& downstream_rsp_;
    VcArbiter vc_arbiter_;
    noc::WormholeArbiter<noc::NocRspOut> wormhole_arbiter_;
    MetaBuffer meta_buffer_;
    Packetize packetize_;
    Depacketize depacketize_;
    AxiMasterPort axi_master_port_;
};

namespace detail {

inline VcArbiter make_vc_arbiter(const NsuConfig& cfg, noc::NocRspOut& downstream) {
    if (cfg.vc_mode == VcMode::ReadWriteSplit) {
        return VcArbiter::read_write_split(downstream, cfg.num_vc, cfg.write_rsp_vc,
                                           cfg.read_rsp_vc, cfg.vc_arbiter_pending_depth);
    }
    auto candidates = cfg.vc_candidates;
    return VcArbiter::multi_candidate(downstream, cfg.num_vc, std::move(candidates),
                                      cfg.vc_arbiter_pending_depth);
}

}  // namespace detail

inline Nsu::Nsu(NsuConfig cfg, noc::NocReqIn& upstream_req, noc::NocRspOut& downstream_rsp)
    : cfg_(std::move(cfg)),
      upstream_req_(upstream_req),
      downstream_rsp_(downstream_rsp),
      vc_arbiter_(detail::make_vc_arbiter(cfg_, downstream_rsp_)),
      wormhole_arbiter_(vc_arbiter_, /*num_inputs=*/2, std::vector<noc::ChannelPairing>{},
                        cfg_.wormhole_per_input_depth),
      meta_buffer_(cfg_.meta_buffer_per_id_depth),
      packetize_(wormhole_arbiter_.input(0), wormhole_arbiter_.input(1), meta_buffer_,
                 cfg_.src_id),
      depacketize_(upstream_req_, meta_buffer_, cfg_.depkt_aw_q_depth, cfg_.depkt_w_q_depth,
                   cfg_.depkt_ar_q_depth),
      axi_master_port_(depacketize_, packetize_, cfg_.port_params) {}

inline void Nsu::tick() {
    depacketize_.tick();
    axi_master_port_.tick();
    wormhole_arbiter_.tick();
    vc_arbiter_.tick();
}

}  // namespace ni::cmodel::nsu
```

- [ ] **Step 2: Run clang-format**

```bash
clang-format -i c_model/include/nsu/nsu.hpp
```

- [ ] **Step 3: Build — verify clean**

```bash
cmake --build c_model/build -j 1
```

- [ ] **Step 4: Create smoke test `c_model/tests/nsu/test_nsu.cpp`** (per spec §8 commit 2 smoke test requirement)

```cpp
// Smoke test: Nsu class constructs cleanly + tick() doesn't crash.
// Verifies ctor sequence (member init order, factory return-by-value, no-Rob
// asymmetry vs Nmu) before Task 3 integration.
#include "common/loopback_noc.hpp"
#include "common/scenario.hpp"
#include "nsu/nsu.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nsu::Nsu;
using ni::cmodel::nsu::NsuConfig;
using ni::cmodel::testing::LoopbackNoc;

TEST(NsuTopLevel, ConstructsAndTicksWithoutCrash) {
    SCENARIO("Nsu top-level smoke: NUM_VC=1 default config, construct + tick 10x "
             "should not crash. Verifies ctor sequence (no Rob, MetaBuffer shared "
             "between Depacketize and Packetize).");
    LoopbackNoc loopback(/*req*/64, /*rsp*/64);
    NsuConfig cfg{};
    cfg.src_id = 0x34;
    Nsu nsu(cfg, loopback.nsu_req_in(0), loopback.nsu_rsp_out(0));

    EXPECT_EQ(&nsu.axi_master_port(), &nsu.axi_master_port())
        << "axi_master_port() returns stable reference";

    for (int i = 0; i < 10; ++i) {
        nsu.tick();
        loopback.tick();
    }
    SUCCEED();
}
```

- [ ] **Step 5: Register smoke test in `c_model/tests/nsu/CMakeLists.txt`**

Append:
```cmake
add_cmodel_test(test_nsu)
target_include_directories(test_nsu PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

- [ ] **Step 6: Run clang-format on smoke test**

```bash
clang-format -i c_model/tests/nsu/test_nsu.cpp
```

- [ ] **Step 7: Run full ctest — expect 338/338**

```bash
cmake --build c_model/build -j 1
ctest --test-dir c_model/build -j 1
```
Expected: 338/338 (337 after Task 1 + 1 new NSU smoke test). If smoke fails, debug ctor sequence before committing.

- [ ] **Step 8: Commit**

```bash
git add c_model/include/nsu/nsu.hpp c_model/tests/nsu/test_nsu.cpp c_model/tests/nsu/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(nsu): add Nsu top-level class (NSU mirror, no Rob)

Nsu wraps Depacketize + AxiMasterPort + MetaBuffer + Packetize +
WormholeArbiter<NocRspOut> + VcArbiter into one class. Single tick()
entrypoint orchestrates sub-modules in upstream-first order
(depacketize -> axi_master_port -> wormhole -> vc_arbiter).

Asymmetric vs Nmu: no Rob (response side has no reorder buffer; that
is NMU's responsibility), no addr_trans (uses incoming flit dst_id).
WormholeArbiter has 2 inputs no pairing (B/R each single-flit per
FlooNoC pattern). MetaBuffer shared between Depacketize (snapshot on
AW/AR ingress) and Packetize (read on B/R egress) for rob_idx/src_id
matching.

NsuConfig uses write_rsp_vc / read_rsp_vc naming (rsp infix) per NSU
VcArbiter convention. No Rob fields.

1 smoke test added (Nsu constructs + ticks without crash, verifies
no-Rob ctor sequence + MetaBuffer sharing). ctest 337 -> 338.

Pure addition; existing tests untouched. ctest 337/337 unchanged.

Refs: docs/superpowers/specs/2026-06-04-nmu-nsu-top-level-design.md
      docs/noc_cmodel_rtl_plan.md section 3 row 173

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Refactor integration testbench to use Nmu / Nsu

**Goal:** Replace ~400 lines of manual sub-module wiring in `c_model/tests/integration/test_request_response_loopback.cpp` with `nmu::Nmu` instance + `std::vector<std::unique_ptr<nsu::Nsu>>` for multi-NSU. Tick loop simplifies from ~5 sub-module ticks to `master.tick(); nmu.tick(); for nsu : nsus nsu->tick(); slave.tick(); mem.tick(); loopback.tick();`.

**Files:**
- Modify: `c_model/tests/integration/test_request_response_loopback.cpp`

What stays in the testbench (cannot be absorbed):
- LoopbackNoc construction + `set_dst_route(dst_id, nsu_idx)` multi-NSU routing
- Per-NSU latency config `set_nsu_latency(...)`
- Per-NSU shuttle logic (slave ↔ AxiMasterPort B/R routing per owner tracking)
- AxiMaster / AxiSlave / Memory construction (Stage 2 endpoints, external to NI)

What moves into Nmu/Nsu:
- All NMU sub-module construction (Packetize, Rob, AxiSlavePort, Depacketize, WormholeArbiter, VcArbiter)
- All NSU sub-module construction (per i)
- Per-sub-module tick calls (replaced by `nmu.tick()` / `nsu->tick()`)

- [ ] **Step 1: Read current testbench to identify exact lines to replace**

```bash
grep -nE "nmu::|nsu::|wormhole_arbiter|vc_arbiter|loopback|tick\(\)" c_model/tests/integration/test_request_response_loopback.cpp | head -50
```

Note the line ranges of:
- NMU sub-module construction block (~lines 183-200, after vc_arb + wormhole rounds)
- NSU vectors + per-NSU construction loop (~lines 197-250)
- Per-cycle tick loop NMU + NSU sub-module ticks (~lines 311-440)

- [ ] **Step 2: Replace NMU sub-module construction block with `nmu::Nmu` instance**

Find the block starting `nmu::Packetize    real_nmu_pkt(...)` (was lines ~183-197 before wormhole_arbiter round; now after wormhole, ~lines 188-200 with wh_arb and vc_arb inserts). Replace the ENTIRE NMU block (Packetize, vc_arbiter, wormhole_arbiter, Rob, AxiSlavePort constructions) with:

```cpp
#include "nmu/nmu.hpp"   // add to includes near top

// Replace block:
//   nmu_arb (vc_arbiter), nmu_wh_arb (wormhole), real_nmu_pkt (Packetize),
//   nmu_depkt (Depacketize), rob, nmu_port (AxiSlavePort)
// with single Nmu instance:
nmu::NmuConfig nmu_cfg{};
nmu_cfg.src_id = kNmuSrcId;
nmu_cfg.write_rob_mode = rob_mode;
nmu_cfg.read_rob_mode = rob_mode;
nmu_cfg.port_params = params;
nmu::Nmu nmu(nmu_cfg, loopback.nmu_req_out(), loopback.nmu_rsp_in());
```

Replace all subsequent references to:
- `nmu_port` → `nmu.axi_slave_port()`
- `rob` → `nmu.rob()` (if accessed; mostly internal)
- `nmu_depkt`, `real_nmu_pkt`, `nmu_arb`, `nmu_wh_arb` → no direct access needed (encapsulated)

- [ ] **Step 3: Replace NSU vectors + per-NSU loop with `nsu::Nsu` vector**

Find the block declaring `std::vector<std::unique_ptr<nsu::MetaBuffer>> nsu_metas;` and subsequent `std::vector<std::unique_ptr<...>>` for nsu_depkts, nsu_pkts, nsu_ports, nsu_arbs, nsu_wh_arbs. Replace ALL of those with:

```cpp
#include "nsu/nsu.hpp"   // add to includes near top

// Replace:
//   nsu_metas, nsu_depkts, nsu_pkts, nsu_ports, nsu_arbs, nsu_wh_arbs vectors
// with single Nsu vector:
std::vector<std::unique_ptr<nsu::Nsu>> nsus;
nsus.reserve(nsu_count);
for (std::size_t i = 0; i < nsu_count; ++i) {
    const uint8_t this_nsu_src = is_multi_dst ? kNsuSrcIdsMulti[i] : kNsuSrcId;
    nsu::NsuConfig nsu_cfg{};
    nsu_cfg.src_id = this_nsu_src;
    nsu_cfg.port_params = params;
    nsu_cfg.meta_buffer_per_id_depth = params.meta_buffer_per_id_depth;
    nsu_cfg.depkt_aw_q_depth = params.depkt_aw_q_depth;
    nsu_cfg.depkt_w_q_depth = params.depkt_w_q_depth;
    nsu_cfg.depkt_ar_q_depth = params.depkt_ar_q_depth;
    nsus.emplace_back(std::make_unique<nsu::Nsu>(nsu_cfg, loopback.nsu_req_in(i),
                                                  loopback.nsu_rsp_out(i)));
}
```

Replace all subsequent references:
- `nsu_ports[i].get()` / `nsu_ports[i]->...` → `nsus[i]->axi_master_port()` / `&nsus[i]->axi_master_port()`
- `nsu_metas[i]`, `nsu_depkts[i]`, `nsu_pkts[i]`, `nsu_arbs[i]`, `nsu_wh_arbs[i]` → no direct access (encapsulated)

- [ ] **Step 4: Simplify per-cycle tick loop**

Find the per-cycle while loop. Replace the sub-module tick sequence:

```cpp
// Before:
master.tick();
nmu_depkt.tick();
for (...) { nsu_depkts[i]->tick(); }
nmu_port.tick();
for (...) { nsu_ports[i]->tick(); }
// ... shuttle logic ...
slave.tick(); mem.tick();
// ... shuttle B/R back ...
nmu_wh_arb.tick();
for (...) { nsu_wh_arbs[i]->tick(); }
nmu_arb.tick();
for (...) { nsu_arbs[i]->tick(); }
loopback.tick();

// After:
master.tick();
nmu.tick();
for (std::size_t i = 0; i < nsu_count; ++i) {
    nsus[i]->tick();
}
// ... shuttle logic (unchanged; uses nsus[i]->axi_master_port()) ...
slave.tick(); mem.tick();
// ... shuttle B/R back (uses nsus[i]->axi_master_port()) ...
loopback.tick();
```

Net change: replaces 6 explicit sub-module tick lines with 2 (`nmu.tick()` + per-NSU loop with `nsus[i]->tick()`). Shuttle logic stays — only port handle access changes (nsu_ports[i] → nsus[i]->axi_master_port()).

- [ ] **Step 5: Run clang-format on the testbench**

```bash
clang-format -i c_model/tests/integration/test_request_response_loopback.cpp
```

- [ ] **Step 6: Build — fix any compile errors iteratively**

```bash
cmake --build c_model/build -j 1 2>&1 | head -30
```

Common compile errors expected during this refactor:
- Missed reference to a now-encapsulated sub-module (e.g., `nmu_port.push_aw`) — replace with `nmu.axi_slave_port().push_aw`.
- AxiMaster template binding — verify `AxiMasterT<nmu::AxiSlavePort>` constructor takes `Nmu::axi_slave_port()` correctly (it should; getter returns `AxiSlavePort&`).
- Unused variable warnings (e.g., for old vectors) — remove dead declarations.

- [ ] **Step 7: Run full ctest — expect 338/338 unchanged**

```bash
ctest --test-dir c_model/build -j 1
```

Expected: 338 tests pass (baseline after Tasks 1+2 smoke tests). Integration tests (PacketizeLoopbackFixture.ScoreboardZeroMismatch/...) are the e2e validation. If any fail, the refactor changed observable behavior — debug the wiring.

- [ ] **Step 8: Per-file diff review (Spec §6 risk mitigation)**

```bash
git diff --stat c_model/tests/integration/test_request_response_loopback.cpp
```

Expected: large net deletion (~320 lines fewer). Verify:
- Only wiring changes; NO test logic / scoreboard / scenario changes
- Tick loop simpler
- Shuttle logic preserved (slave ↔ NSU B/R routing unchanged)

If diff shows changes to scoreboard assertions or test scenarios, STOP — those should not change in a pure refactor.

- [ ] **Step 9: Commit**

```bash
git add c_model/tests/integration/test_request_response_loopback.cpp
git commit -m "$(cat <<'EOF'
refactor(tests/integration): use Nmu/Nsu top-level in PacketizeLoopback testbench

Replace ~320 lines of manual NI sub-module wiring with single nmu::Nmu
instance + std::vector<std::unique_ptr<nsu::Nsu>> for multi-NSU.
Per-cycle tick loop simplifies from 5+ explicit sub-module ticks to:
  master.tick(); nmu.tick(); for nsu : nsus nsu->tick();
  slave.tick(); mem.tick(); loopback.tick();

What moved INTO Nmu/Nsu (encapsulated): all NMU sub-modules
(AxiSlavePort, Rob, Packetize, WormholeArbiter, VcArbiter, Depacketize)
+ all NSU sub-modules (Depacketize, AxiMasterPort, MetaBuffer,
Packetize, WormholeArbiter, VcArbiter).

What stayed in testbench (LoopbackNoc-specific): set_dst_route multi-NSU
routing, per-NSU latency config, per-NSU shuttle logic (slave <-> port
B/R owner tracking), AxiMaster/AxiSlave/Memory construction.

Pure wiring refactor: ctest 338/338 unchanged; zero test logic /
scoreboard / scenario changes. Stage 3 NI architecture now fully
encapsulated; ready for next follow-up (NUM_VC>1 e2e stress) and
subsequent Stage 5 axi_checker.sv co-sim (new session).

Refs: docs/superpowers/specs/2026-06-04-nmu-nsu-top-level-design.md
      docs/superpowers/plans/2026-06-04-nmu-nsu-top-level.md

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

After all 3 commits:

```bash
cd "E:/05_NoC/noc_project"
cmake --build c_model/build -j 1
ctest --test-dir c_model/build -j 1
git log --oneline 77c1f5b..HEAD
```

Expected:
- Build clean
- ctest 338/338
- 3 commits on `stage3/packetize-depacketize`

---

## Self-review notes (controller, not for implementer)

1. **Member initialization order**: critical for Nmu/Nsu correctness. Each sub-module ctor takes refs to others; declaration order locks initialization order. Mis-ordering = undefined behavior at construction. Codex r1 flagged this; spec §5 and member comments document it.

2. **VcArbiter factory return-by-value into member init**: relies on C++17 mandatory copy elision (NRVO/RVO) since VcArbiter deletes copy/move. The `detail::make_vc_arbiter` helper returns prvalue; member initializer constructs in-place. If compiler doesn't honor (very old toolchain), workaround is to construct VcArbiter directly via the (currently private) ctor — would need to add `friend class Nmu` / `friend class Nsu`.

3. **Backward compatibility of NUM_VC=1**: NmuConfig and NsuConfig default `num_vc = 1`; existing integration tests pass NMU/NSU with NUM_VC=1 transparently. After this round, follow-up round can change cfg fields to test NUM_VC>1 e2e without code structure changes.

4. **AXI master/slave construction** stays in testbench because AxiMasterT<AxiSlavePort> binds to the SPECIFIC AxiSlavePort instance. Could potentially template Nmu on slave type, but YAGNI for this round.
