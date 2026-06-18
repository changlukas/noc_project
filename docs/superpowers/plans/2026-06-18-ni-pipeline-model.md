# NMU/NSU pipeline model Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert NMU/NSU from 0-cycle combinational pull-through into a real staged-across-ticks pipeline, mirroring the router, so the c_model captures realistic per-stage NI latency and is the golden reference for the NI RTL.

**Architecture:** Mirror `router.hpp` exactly: each NI path gets explicit per-(channel) **stage registers** as data members; `Nmu::tick()`/`Nsu::tick()` run stages in **reverse order** so one beat advances exactly one stage per tick; hand-offs are registered; each stage moves **at most one beat per channel per tick** (the unbounded `while`-drain loops are replaced). Stage transforms (Rob allocate / Packetize build / Depacketize decode) run as the per-stage combinational body. The WormholeArbiter/VcArbiter are already single-grant-per-tick and stay as the final stage.

**Tech Stack:** C++17, GoogleTest. Spec: `docs/superpowers/specs/2026-06-18-ni-pipeline-model-design.md`.

## Global Constraints

- Build: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3`. Test: `cd build/cmodel && ctest -R <name> --output-on-failure`.
- `clang-format -i` every edited `.hpp`/`.cpp` before commit. snake_case methods, PascalCase types. No `--no-verify`.
- **Mirror the router** (`router.hpp:156-261`): explicit stage register (`landing_` analogue), reverse-order `tick()` (`:199`), registered hand-off (`credit_pulse_pending_`, `:166`), overwrite assert on stage-register write (`:185`), `≤1 beat/channel/stage/tick`.
- Stage allocation is locked by the spec §4: **NMU req 3 / NMU rsp 3 / NSU req 2 / NSU rsp 3**, default 1 cycle/stage.
- Latency measured at internal stage boundaries via the new `stage_occupancy(NiPath, stage, axi_ch)` getter (spec §8), excluding the co-sim wrapper register.
- ORDERING_MODE = existing `RobMode` (ROB=`Enabled`, ROBLESS=`Disabled`); do not add a new knob (spec §6).
- Per-stage depth is parameterized; default = the §4 stage counts.
- Preserve coupled side-effects (spec §5 / Explore): MetaBuffer snapshot atomic with NSU-Depacketize queue push; NMU-Packetize `w_meta_fifo_` mutated only on push success; VcArbiter bindings unchanged; Depacketize `pending_` semantics.

## Risk register (per Explore; each cited task must respect these)

| R | Risk | Where |
|---|---|---|
| R1 | Unbounded `while`-drain loops move many beats/tick | `axi_slave_port.hpp:151-168`, `axi_master_port.hpp:138-170`, `nmu/depacketize.hpp:74`, `nsu/depacketize.hpp:99` |
| R2 | Single call spans AxiSlavePort→Rob→Packetize→push_flit (req) / AxiMasterPort→Packetize→MetaBuffer→push_flit (rsp) | `axi_slave_port.hpp:151`, `rob.hpp:191`, `packetize.hpp` |
| R3 | Rob Enabled-mode `pop_b`/`pop_r` chain-flush (2 logical stages/call) | `rob.hpp:243-352` |
| R4 | Coupled side-effects must stay atomic | MetaBuffer (`nsu/depacketize.hpp:119/142`), `w_meta_fifo_` (`packetize.hpp:111`), VcArbiter bindings |
| R5 | Test-endpoint `pending_` counters assume multi-pop/tick | `nmu.hpp:226`, `nsu.hpp:157` |
| R6 | Watchdog cycle bounds in integration tests may need raising | `test_request_response_loopback.cpp`, `test_port_pair_loopback.cpp` |

---

### Task 1: `PipelineStage` register primitive

**Files:**
- Create: `c_model/include/noc/pipeline_stage.hpp`
- Test: `c_model/tests/noc/test_pipeline_stage.cpp`
- Modify: `c_model/tests/noc/CMakeLists.txt`

**Interfaces:**
- Produces: `template <class Beat> class PipelineStage` — the per-channel one-beat stage register mirroring the router's `landing_` register. Consumed by Tasks 2-5.

The primitive generalizes the router's `std::optional<Flit> landing_` + overwrite-assert pattern into a typed, occupancy-introspectable one-beat register with a valid/ready hand-off.

- [ ] **Step 1: Register the test**

In `c_model/tests/noc/CMakeLists.txt`, add (next to the other `add_cmodel_test` lines):
```cmake
add_cmodel_test(test_pipeline_stage)
target_include_directories(test_pipeline_stage PRIVATE ${CMAKE_SOURCE_DIR}/include)
```

- [ ] **Step 2: Write the failing test**

`c_model/tests/noc/test_pipeline_stage.cpp`:
```cpp
#include "noc/pipeline_stage.hpp"

#include <gtest/gtest.h>

using ni::cmodel::noc::PipelineStage;

namespace {

TEST(PipelineStage, EmptyByDefault) {
    PipelineStage<int> s;
    EXPECT_FALSE(s.full());
    EXPECT_EQ(s.occupancy(), 0u);
    EXPECT_TRUE(s.ready());  // ready to accept when empty
}

TEST(PipelineStage, AcceptThenFull) {
    PipelineStage<int> s;
    s.accept(7);
    EXPECT_TRUE(s.full());
    EXPECT_EQ(s.occupancy(), 1u);
    EXPECT_FALSE(s.ready());
    EXPECT_EQ(s.peek(), 7);
}

TEST(PipelineStage, TakeFrees) {
    PipelineStage<int> s;
    s.accept(7);
    EXPECT_EQ(s.take(), 7);
    EXPECT_FALSE(s.full());
    EXPECT_TRUE(s.ready());
}

TEST(PipelineStage, OverwriteAsserts) {
    // Mirrors Router::accept_flit overwrite guard (router.hpp:185): >1 beat
    // into a one-beat stage register in one cycle is a discipline violation.
    PipelineStage<int> s;
    s.accept(1);
    EXPECT_DEATH(s.accept(2), "PipelineStage: overwrite");
}

}  // namespace
```

- [ ] **Step 3: Run the test (expect compile failure)**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3`
Expected: FAIL — `pipeline_stage.hpp` not found.

- [ ] **Step 4: Implement the primitive**

`c_model/include/noc/pipeline_stage.hpp`:
```cpp
#ifndef NI_CMODEL_NOC_PIPELINE_STAGE_HPP
#define NI_CMODEL_NOC_PIPELINE_STAGE_HPP

#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>

namespace ni::cmodel::noc {

// One-beat stage register, the building block of a staged-across-ticks NI
// pipeline. Mirrors Router::landing_ (router.hpp:157) + its overwrite assert
// (router.hpp:185): at most one beat per cycle enters; reverse-order tick
// drains (take) before the upstream fills (accept), so a beat advances exactly
// one stage per tick. `ready()` is the valid/ready back-pressure signal.
template <class Beat>
class PipelineStage {
  public:
    bool full() const noexcept { return slot_.has_value(); }
    bool ready() const noexcept { return !slot_.has_value(); }
    std::size_t occupancy() const noexcept { return slot_ ? 1u : 0u; }

    const Beat& peek() const {
        assert(slot_.has_value() && "PipelineStage: peek empty");
        return *slot_;
    }

    void accept(Beat b) {
        assert(!slot_.has_value() && "PipelineStage: overwrite (>1 beat/cycle)");
        slot_ = std::move(b);
    }

    Beat take() {
        assert(slot_.has_value() && "PipelineStage: take empty");
        Beat b = std::move(*slot_);
        slot_.reset();
        return b;
    }

    void clear() noexcept { slot_.reset(); }

  private:
    std::optional<Beat> slot_;
};

}  // namespace ni::cmodel::noc

#endif  // NI_CMODEL_NOC_PIPELINE_STAGE_HPP
```

- [ ] **Step 5: Run the test (expect pass)**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3 && cd build/cmodel && ctest -R PipelineStage --output-on-failure`
Expected: 4/4 PASS.

- [ ] **Step 6: clang-format + commit**

```bash
clang-format -i c_model/include/noc/pipeline_stage.hpp c_model/tests/noc/test_pipeline_stage.cpp
git add -A
git commit -m "feat(ni): add PipelineStage one-beat register primitive"
```

---

### Task 2: `NiPath` enum + `stage_occupancy` getter scaffold

**Files:**
- Create: `c_model/include/nmu/ni_stage.hpp` (shared enum)
- Modify: `c_model/include/nmu/nmu.hpp`, `c_model/include/nsu/nsu.hpp`
- Test: `c_model/tests/common/test_ni_stage.cpp`
- Modify: `c_model/tests/common/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `enum class NiPath { NmuReq, NmuRsp, NsuReq, NsuRsp }`; `std::size_t Nmu::stage_occupancy(NiPath, std::size_t stage, uint8_t axi_ch) const`; same on `Nsu`, `NmuStandalone`, `NsuStandalone`. Initially returns 0 (no stages wired yet); Tasks 3-6 fill it in per path.

This isolates the public introspection surface (spec §8) so later path tasks only add their wiring behind a stable signature.

- [ ] **Step 1: Register the test**

`c_model/tests/common/CMakeLists.txt`: add
```cmake
add_cmodel_test(test_ni_stage)
target_include_directories(test_ni_stage PRIVATE ${CMAKE_SOURCE_DIR}/include)
```

- [ ] **Step 2: Write the failing test**

`c_model/tests/common/test_ni_stage.cpp`:
```cpp
#include "nmu/ni_stage.hpp"

#include <gtest/gtest.h>

using ni::cmodel::NiPath;

TEST(NiStage, PathEnumValues) {
    // The four NI translation paths (spec §4).
    EXPECT_NE(NiPath::NmuReq, NiPath::NmuRsp);
    EXPECT_NE(NiPath::NsuReq, NiPath::NsuRsp);
}
```

- [ ] **Step 3: Run (expect compile failure)**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3`
Expected: FAIL — `nmu/ni_stage.hpp` not found.

- [ ] **Step 4: Implement the enum + getter scaffold**

`c_model/include/nmu/ni_stage.hpp`:
```cpp
#ifndef NI_CMODEL_NI_STAGE_HPP
#define NI_CMODEL_NI_STAGE_HPP

namespace ni::cmodel {

// The four AXI<->flit translation paths of the NI (spec §4).
enum class NiPath { NmuReq, NmuRsp, NsuReq, NsuRsp };

}  // namespace ni::cmodel

#endif  // NI_CMODEL_NI_STAGE_HPP
```

In `nmu.hpp`, add `#include "nmu/ni_stage.hpp"` and on `Nmu` (public):
```cpp
// Beats held in stage register [stage] of [path] for AXI channel [axi_ch]
// (ni_flit_constants AW=0..R=4). Internal-boundary latency probe (spec §8).
// Returns 0 for paths/stages not yet staged.
std::size_t stage_occupancy(NiPath path, std::size_t stage, uint8_t axi_ch) const {
    (void)path; (void)stage; (void)axi_ch;
    return 0;  // filled per-path in Tasks 3-6
}
```
Re-expose verbatim on `NmuStandalone` (mirror the existing `rob()`/`vc_arbiter()` passthroughs at `nmu.hpp:272-273`). Do the same on `Nsu` and `NsuStandalone` (`nsu.hpp` — add the passthrough that does not exist yet, per Explore).

- [ ] **Step 5: Run (expect pass)**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3 && cd build/cmodel && ctest -R NiStage --output-on-failure`
Expected: 1/1 PASS; full build green (getter compiles on all four classes).

- [ ] **Step 6: clang-format + commit**

```bash
clang-format -i c_model/include/nmu/ni_stage.hpp c_model/include/nmu/nmu.hpp c_model/include/nsu/nsu.hpp c_model/tests/common/test_ni_stage.cpp
git add -A
git commit -m "feat(ni): add NiPath enum + stage_occupancy getter scaffold"
```

---

### Task 3: NSU req path staging (2 stages) — simplest path first

**Files:**
- Modify: `c_model/include/nsu/nsu.hpp` (tick reverse-order + stage registers + `stage_occupancy`)
- Modify: `c_model/include/nsu/depacketize.hpp` (bound to one-beat advance), `c_model/include/nsu/axi_master_port.hpp` (bound req-drain to one-beat)
- Test: `c_model/tests/nsu/test_nsu_pipeline.cpp`
- Modify: `c_model/tests/nsu/CMakeLists.txt`

**Why first:** NSU req is the only 2-stage path, has no Rob, and its blocks are `Depacketize → AxiMasterPort` (Explore items 8/7). It validates the router-mirrored mechanism end-to-end on the least-coupled path before the Rob paths.

**Interfaces:**
- Consumes: `PipelineStage` (Task 1), `NiPath` (Task 2).
- Produces: the staging pattern (stage register members + reverse-order advance + bounded one-beat submodule advance) that Tasks 4-6 replicate.

**Mechanism (mirror router, spec §5):**
- Add stage registers in `Nsu`: one `PipelineStage<...>` per AXI request channel (AW/W/AR) at the S1→S2 boundary (between `Depacketize` output and `AxiMasterPort` input).
- `Nsu::tick()` request portion runs reverse-order: **first** advance S2 (`AxiMasterPort` drives the slave with ≤1 beat/channel from its stage register), **then** advance S1 (`Depacketize` decodes ≤1 flit into the S1 register). One beat advances one stage per tick.
- Replace the unbounded `while` in `Depacketize::tick()` (`nsu/depacketize.hpp:99`) and the `drain_*_from_depacketizer_` loops (`axi_master_port.hpp:138-158`) with **one-beat-per-channel-per-tick** moves. Preserve the MetaBuffer snapshot atomicity (R4): snapshot still happens with the single AW/AR decode.
- `stage_occupancy(NsuReq, stage, axi_ch)`: stage 0 = the S1 register occupancy, stage 1 = `AxiMasterPort`'s per-channel holding for that channel.

- [ ] **Step 1: Register the test**

`c_model/tests/nsu/CMakeLists.txt`: add `add_cmodel_test(test_nsu_pipeline)` + its `target_include_directories`.

- [ ] **Step 2: Write the failing test (latency = 2; one-stage-per-tick advance)**

`c_model/tests/nsu/test_nsu_pipeline.cpp`:
```cpp
#include "nsu/nsu.hpp"
#include "nmu/ni_stage.hpp"
#include "ni_flit_constants.h"

#include <gtest/gtest.h>

using ni::cmodel::NiPath;
using ni::cmodel::nsu::NsuStandalone;
using ni::cmodel::nsu::NsuConfig;

namespace {

// Inject one AR request flit at the NoC edge; it must take exactly 2 ticks
// (Depacketize S1, AxiMasterPort S2) to appear as an AR beat at the slave port.
TEST(NsuPipeline, ReqLatencyIsTwoStages) {
    NsuConfig cfg;  // defaults: 1 cycle/stage, depth per spec §4
    NsuStandalone nsu(cfg);

    // <inject a single AR request flit into nsu's NoC-req endpoint>
    // (use the NsuStandalone inject_req_flit() helper, Explore item 6)
    inject_single_ar_flit(nsu);  // helper defined in this TU; see Step 4 note

    // tick 1: AR advances into S1 (Depacketize register), not yet at slave.
    nsu.tick();
    EXPECT_EQ(nsu.stage_occupancy(NiPath::NsuReq, 0, ni::AXI_CH_AR), 1u);
    EXPECT_FALSE(nsu.axi_master_port().pop_ar().has_value());

    // tick 2: AR advances into S2 and is drivable at the slave port.
    nsu.tick();
    EXPECT_TRUE(nsu.axi_master_port().pop_ar().has_value());
}

}  // namespace
```
(The `inject_single_ar_flit` helper builds one AR flit via the existing flit/packetize encoding and pushes it through `NsuStandalone`'s req endpoint; model it on the existing `test_nsu_*` flit-injection helpers.)

- [ ] **Step 3: Run (expect fail — today it is 0-cycle, AR appears after 1 tick)**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3 && cd build/cmodel && ctest -R NsuPipeline --output-on-failure`
Expected: FAIL — current pull-through delivers the AR in one tick (no S1 occupancy observed / pop_ar succeeds at tick 1).

- [ ] **Step 4: Implement the staging**

Apply the mechanism above:
1. `nsu/depacketize.hpp`: change `tick()` (`:99` `while(true)`) to decode **at most one flit per request channel** into per-channel stage registers; keep the MetaBuffer snapshot atomic with the single decode (R4); keep `pending_` for the still-blocked case.
2. `nsu/axi_master_port.hpp`: change `drain_aw/w/ar_from_depacketizer_` (`:138-158`) to pull **at most one beat per channel per tick** from the S1 registers.
3. `nsu.hpp`: hold the S1 `PipelineStage` registers; rewrite the request portion of `tick()` to reverse-order (S2 advance, then S1 advance); implement `stage_occupancy(NsuReq, …)`.

- [ ] **Step 5: Run (expect pass) + no regression on existing NSU tests**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3 && cd build/cmodel && ctest -R "NsuPipeline|Nsu_" --output-on-failure`
Expected: NsuPipeline 2/2 PASS; existing NSU tests still pass (bounded-loop drivers tolerate the added cycle — Explore). If a direct submodule test asserted "all N beats visible after one tick," update it to tick N times (R1) and note it.

- [ ] **Step 6: clang-format + commit**

```bash
clang-format -i c_model/include/nsu/*.hpp c_model/tests/nsu/test_nsu_pipeline.cpp
git add -A
git commit -m "feat(ni): stage the NSU request path (2 stages, router-mirrored)"
```

---

### Task 4: NSU rsp path staging (3 stages)

**Files:**
- Modify: `c_model/include/nsu/nsu.hpp`, `c_model/include/nsu/axi_master_port.hpp` (bound B/R forward), `c_model/include/nsu/packetize.hpp` (one-beat build)
- Test: `c_model/tests/nsu/test_nsu_pipeline.cpp` (add cases)

**Interfaces:**
- Consumes: Task 3 pattern.
- Produces: `stage_occupancy(NsuRsp, …)`.

**Mechanism:** stages `AxiMasterPort accept (B/R) → Packetize → WormholeArbiter+VcArbiter → NoC`. The arbiters are already single-grant-per-tick (Explore 11/12) — they are the final stage as-is. Insert stage registers at AxiMasterPort-accept→Packetize and Packetize→arbiter-input. Reverse-order advance. Preserve `MetaBuffer` peek/commit ordering (R4): commit only on successful `push_flit`.

- [ ] **Step 1: Write the failing test (rsp latency = 3)**

Add to `test_nsu_pipeline.cpp`:
```cpp
// Push a single B response at the AXI master port; it must take 3 ticks
// (AxiMasterPort accept, Packetize, arbiter→NoC) to appear as a rsp flit.
TEST(NsuPipeline, RspLatencyIsThreeStages) {
    NsuConfig cfg;
    NsuStandalone nsu(cfg);
    seed_meta_for_b(nsu, /*id=*/3);  // snapshot meta so Packetize can build B
    nsu.axi_master_port().push_b(make_b_beat(/*id=*/3));
    for (int t = 0; t < 2; ++t) {
        nsu.tick();
        EXPECT_FALSE(nsu.pop_rsp_flit().has_value()) << "rsp flit emitted too early at tick " << t;
    }
    nsu.tick();  // 3rd tick: rsp flit at NoC edge
    EXPECT_TRUE(nsu.pop_rsp_flit().has_value());
}
```

- [ ] **Step 2: Run (expect fail)** — `ctest -R NsuPipeline.RspLatencyIsThreeStages`; current path emits in 1 tick.

- [ ] **Step 3: Implement** the rsp staging per the mechanism (bound `forward_b/r_to_packetizer_` `axi_master_port.hpp:159-170` to one beat; stage registers in `nsu.hpp` rsp portion; reverse-order; MetaBuffer commit-on-success preserved).

- [ ] **Step 4: Run (expect pass) + NSU regression** — `ctest -R "NsuPipeline|Nsu_"`. Expected: all pass.

- [ ] **Step 5: clang-format + commit**
```bash
git add -A && git commit -m "feat(ni): stage the NSU response path (3 stages)"
```

---

### Task 5: NMU req path staging (3 stages, Rob allocate + Address Map in S1)

**Files:**
- Modify: `c_model/include/nmu/nmu.hpp`, `c_model/include/nmu/axi_slave_port.hpp` (bound forward), `c_model/include/nmu/rob.hpp` (one-beat admit), `c_model/include/nmu/packetize.hpp` (one-beat build)
- Test: `c_model/tests/nmu/test_nmu_pipeline.cpp`; `c_model/tests/nmu/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 3/4 pattern.
- Produces: `stage_occupancy(NmuReq, …)`.

**Mechanism:** stages `AxiSlavePort accept + Rob admit/allocate + Address Map (xy_route, rob.hpp:180/218) → Packetize → WormholeArbiter+VcArbiter → NoC`. S1 includes the Rob admission (allocate + same-id/different-dst gate stays); S2 = Packetize builds the flit from precomputed route/meta. Break the AxiSlavePort→Rob→Packetize one-call chain (R2): AxiSlavePort advances ≤1 beat/channel into S1; Rob admit runs at S1; the S1 register feeds Packetize at S2. Preserve `w_meta_fifo_` mutate-on-success (R4).

- [ ] **Step 1: Write the failing test (req latency = 3)**

`c_model/tests/nmu/test_nmu_pipeline.cpp`:
```cpp
#include "nmu/nmu.hpp"
#include "nmu/ni_stage.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
using ni::cmodel::NiPath;
using ni::cmodel::nmu::NmuStandalone;
using ni::cmodel::nmu::NmuConfig;

TEST(NmuPipeline, ReqLatencyIsThreeStages) {
    NmuConfig cfg;  // RobMode::Disabled default per existing config
    NmuStandalone nmu(cfg);
    nmu.axi_slave_port().push_ar(make_ar_beat(/*id=*/5, /*addr=*/0x100000000ull));
    for (int t = 0; t < 2; ++t) {
        nmu.tick();
        EXPECT_FALSE(nmu.pop_req_flit().has_value()) << "req flit too early at tick " << t;
    }
    nmu.tick();  // 3rd tick: AR flit at NoC edge
    EXPECT_TRUE(nmu.pop_req_flit().has_value());
}
```

- [ ] **Step 2: Run (expect fail)** — current chain emits in 1 tick.

- [ ] **Step 3: Implement** per mechanism. Bound `forward_aw/w/ar_to_packetizer_` (`axi_slave_port.hpp:151-168`) to one beat; run Rob admit (`rob.hpp` push_* incl. `xy_route`) at S1 on the single beat; S1 register → Packetize at S2; reverse-order `tick()` req portion; `stage_occupancy(NmuReq, …)`. **Do not** change Rob mode behavior (allocate/gate logic stays; only the per-tick cardinality is bounded).

- [ ] **Step 4: Run (expect pass) + NMU regression** — `ctest -R "NmuPipeline|Nmu_"`. Expected: all pass.

- [ ] **Step 5: commit** — `git add -A && git commit -m "feat(ni): stage the NMU request path (3 stages, Rob+AddrMap in S1)"`

---

### Task 6: NMU rsp path staging (3 stages) + Re-Ordering + ORDERING_MODE + depth param

**Files:**
- Modify: `c_model/include/nmu/nmu.hpp`, `c_model/include/nmu/depacketize.hpp` (one-beat), `c_model/include/nmu/rob.hpp` (reorder stage as 1-cycle lookup; ROBLESS retire hook)
- Test: `c_model/tests/nmu/test_nmu_pipeline.cpp` (add cases)

**Interfaces:**
- Consumes: Task 5 pattern.
- Produces: `stage_occupancy(NmuRsp, …)`; ROB/ROBLESS behavior per spec §6.

**Mechanism:** stages `Depacketize → Rob Re-Ordering → AxiSlavePort → master` (code order, spec §4). **ROB** (`RobMode::Enabled`): the Re-Ordering stage is a fixed 1-cycle order-decision; in-order forwards (0 hold), out-of-order goes to finite reorder storage (NOT pipeline depth, spec §5). Confine the Enabled-mode chain-flush (R3, `rob.hpp:243-352`) to: 1-cycle lookup advances ≤1 ready beat/tick; the rest stays in reorder storage (occupancy), released one-per-tick. **ROBLESS** (`RobMode::Disabled`): Re-Ordering collapses to passthrough (2 stages); the response side still calls the per-id counter retire (`rob.hpp:289/353`) so request-side stalls release (spec §6). **Depth param**: a per-path depth knob inserts extra `PipelineStage` registers (N-deep shift = N-cycle latency).

- [ ] **Step 1: Write failing tests (rsp latency = 3 ROB / 2 ROBLESS; reorder hold; depth knob)**

Add to `test_nmu_pipeline.cpp`:
```cpp
TEST(NmuPipeline, RspLatencyRobIsThree) {
    NmuConfig cfg; cfg.read_rob_mode = ni::cmodel::nmu::RobMode::Enabled;
    NmuStandalone nmu(cfg);
    inject_single_r_flit(nmu, /*id=*/5);  // in-order head -> 0 hold
    for (int t = 0; t < 2; ++t) { nmu.tick(); EXPECT_FALSE(nmu.axi_slave_port().pop_r().has_value()); }
    nmu.tick();
    EXPECT_TRUE(nmu.axi_slave_port().pop_r().has_value());
}

TEST(NmuPipeline, RspLatencyRoblessIsTwo) {
    NmuConfig cfg; cfg.read_rob_mode = ni::cmodel::nmu::RobMode::Disabled;
    NmuStandalone nmu(cfg);
    inject_single_r_flit(nmu, /*id=*/5);
    nmu.tick(); EXPECT_FALSE(nmu.axi_slave_port().pop_r().has_value());
    nmu.tick();
    EXPECT_TRUE(nmu.axi_slave_port().pop_r().has_value());  // 2 stages: Depacketize -> AxiSlavePort
}

TEST(NmuPipeline, DepthKnobAddsLatency) {
    NmuConfig cfg; cfg.read_rob_mode = ni::cmodel::nmu::RobMode::Disabled;
    cfg.ni_rsp_extra_depth = 2;  // +2 shift registers on NMU rsp
    NmuStandalone nmu(cfg);
    inject_single_r_flit(nmu, /*id=*/5);
    for (int t = 0; t < 3; ++t) { nmu.tick(); EXPECT_FALSE(nmu.axi_slave_port().pop_r().has_value()); }
    nmu.tick();  // base 2 + extra 2 = 4 ticks
    EXPECT_TRUE(nmu.axi_slave_port().pop_r().has_value());
}
```
(Add `ni_rsp_extra_depth` and the analogous per-path knobs to `NmuConfig`/`NsuConfig`, default 0; document each in the config struct.)

- [ ] **Step 2: Run (expect fail).**

- [ ] **Step 3: Implement** the rsp staging + reorder-as-1-cycle-lookup + ROBLESS retire hook + depth knobs.

- [ ] **Step 4: Run (expect pass) + full NMU/NSU regression** — `ctest -R "NmuPipeline|Nmu_|Nsu" --output-on-failure`. Expected: all pass. Also run `ctest -R rob` to confirm Rob unit tests still pass (audit any that assumed multi-beat-per-tick, R1).

- [ ] **Step 5: commit** — `git commit -m "feat(ni): stage the NMU response path + ROB/ROBLESS modes + depth knobs"`

---

### Task 7: Integration regression, co-sim, and docs

**Files:**
- Modify: affected integration/unit tests (watchdog bounds, direct-submodule cardinality), `docs/performance-probe.md`
- Verify: co-sim still builds + runs

**Interfaces:** none new.

- [ ] **Step 1: Full c_model regression**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3 && cd build/cmodel && ctest --output-on-failure`
Fix fallout: raise `kMaxCycles` watchdog bounds in `test_request_response_loopback.cpp` / `test_port_pair_loopback.cpp` if staging pushed total cycles over the bound (R6); update any direct submodule unit test that asserted "all beats visible after one tick" to tick the staged number of times (R1). Update the stale "single tick sufficient" comments (`test_nmu.cpp:136-138`, `test_nsu.cpp:117`).

- [ ] **Step 2: Co-sim build + run**

Run: `make build-verilator PYTHON3=/c/msys64/mingw64/bin/python3 && cd cosim/verilator && make run-tb-top SCENARIO=AX4-BAS-003_single_write_read_aligned`
Expected: builds, scoreboard 0 mismatches. End-to-end latency rises (NI is now a real pipeline) — confirm the run still passes and emits `perf.json`.

- [ ] **Step 3: Rewrite the perf-doc fidelity section**

In `docs/performance-probe.md`, replace the "Latency composition and pipeline fidelity" section: NI is now a **real pipeline** (NMU req 3 / rsp 3 / NSU req 2 / rsp 3), no longer a co-sim wrap artifact. Keep the wrapper-register note (still a separate co-sim boundary cost per spec §7). Update the worked example numbers from a fresh measured run. Remove the "NI pipeline model planned" line from Known limitations (now done).

- [ ] **Step 4: commit**

```bash
git add -A
git commit -m "test(ni): fix regressions for staged NI; co-sim verified; perf-doc fidelity rewrite"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| §4 stage allocation (3/3/2/3) | T3 (NSU req 2), T4 (NSU rsp 3), T5 (NMU req 3), T6 (NMU rsp 3) |
| §5 stage register + reverse-order tick + valid/ready + per-channel throughput + depth + cycle epoch | T1 (primitive), T3-T6 (per path), T6 (depth knob) |
| §5 reorder hold = finite occupancy not depth | T6 |
| §6 ORDERING_MODE = RobMode (ROB/ROBLESS) + retire hook | T6 |
| §7 boundary attribution (internal stages, wrapper separate) | T3-T6 measure at `stage_occupancy`; T7 perf-doc |
| §8 validation (per-stage advance, latency=count, backpressure, reorder, ROB/ROBLESS, depth) | T1, T3-T6 tests |
| §8 `stage_occupancy(NiPath,stage,axi_ch)` API | T2 scaffold, T3-T6 fill |
| §3 scope (ECC/CDC excluded) | not implemented (correct) |
| §7 consequence: tests + perf-doc | T7 |

**Placeholder scan:** test helpers (`inject_single_ar_flit`, `make_b_beat`, `seed_meta_for_b`, `inject_single_r_flit`) are named and their role specified; they are modeled on existing `test_nmu_*`/`test_nsu_*` flit-injection helpers (the implementer copies the established pattern). No TBD/TODO. The per-submodule "bound the while-loop to one beat" edits reference exact current line ranges from the Explore so they are locatable.

**Type consistency:** `PipelineStage<Beat>` (T1) API (`full/ready/occupancy/peek/accept/take/clear`) is used uniformly in T3-T6. `stage_occupancy(NiPath, std::size_t, uint8_t)` (T2) signature is identical everywhere it is filled. `NiPath` enumerators match spec §4.

**Risk-coverage:** R1 (T3-T7 bound loops + test fixes), R2 (T5 break the chain), R3 (T6 confine chain-flush), R4 (T3-T6 preserve side-effect atomicity, called out per task), R5 (T3/T6 Depacketize cardinality — verify endpoint `pending_` counters), R6 (T7 watchdog bounds).

**Open dependency for the implementer:** the exact per-submodule edit (converting each `while`-drain to one-beat advance while preserving backpressure/`pending_`/MetaBuffer/`w_meta_fifo_` semantics) is the substantive work; each task names the file:line and the invariant to preserve. Tasks 3→6 are ordered easiest→hardest so the router-mirrored pattern is proven on NSU-req before the Rob paths.
