# NMU/NSU pipeline model Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert NMU/NSU from a 0-cycle push call-chain into a real staged-across-ticks pipeline, mirroring the router, so the c_model captures realistic per-stage NI latency and is the golden reference for the NI RTL.

**Architecture:** Mirror `router.hpp`: stages communicate **only through shared stage-register data members** (never by calling the next block's push), and `tick()` runs stages **reverse-order** so one token advances one stage per tick. This is a refactor from "transform-and-call-next-push" to "read input register → transform → write output register" (spec §5.0). Each stage register holds a `{beat + computed meta}` token (spec §5.1). The arbiter is the final stage fed from a registered S2→S3 boundary (no same-tick NoC escape, spec §5.2).

**Tech Stack:** C++17, GoogleTest. Spec: `docs/superpowers/specs/2026-06-18-ni-pipeline-model-design.md` (read §5.0-5.3 and §6.1 — the mechanism lives there; tasks below reference it rather than restate it).

## Global Constraints

- Build: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3`. Test: `cd build/cmodel && ctest -R <name> --output-on-failure`.
- `clang-format -i` every edited `.hpp`/`.cpp` before commit. snake_case methods, PascalCase types. No `--no-verify`.
- Stage allocation locked (spec §4): NMU req 3 / NMU rsp 3 / NSU req 2 / NSU rsp 3; default 1 cycle/stage.
- Latency measured at internal stage registers via `stage_occupancy(NiPath, stage, axi_ch)` (spec §8), excluding the co-sim wrapper register (spec §7).
- ORDERING_MODE = existing `RobMode`; config default stays `Disabled` (= ROBLESS); ROB (`Enabled`) opt-in (spec §6).
- Preserve side-effects by moving commit to the stage that performs the NoC push (spec §5.1): `w_meta_fifo_` (`packetize.hpp:111`), MetaBuffer `commit_*` (`nsu/packetize.hpp:52/78`).

## Task coupling (why the sequence below; Codex plan-review)

The four paths share submodules, so they are NOT independent workers:
- `Rob` is shared by **NMU req (admit/allocate)** and **NMU rsp (re-order)** → they are **one task** (Task 5).
- The arbiter final-stage pattern is shared by the two packetize paths (NSU rsp, NMU req) → established once in Task 4, reused in Task 5.
- `Packetize`/port objects are touched by multiple paths → later tasks may re-touch earlier files; the per-task reviewer must re-run the earlier path's tests.

Sequence (easiest→hardest, dependency-safe): primitive+tokens → scaffold → **NSU req spike** (register-parked, no Rob, no arbiter) → **NSU rsp** (first arbiter final-stage) → **NMU req+rsp together** (shared Rob + ROB/ROBLESS + depth) → integration/co-sim/docs.

## Risk register (per the structure survey)

| R | Risk | Where |
|---|---|---|
| R1 | Unbounded `while`-drain loops | `axi_slave_port.hpp:151`, `axi_master_port.hpp:138`, both `depacketize.hpp` `while(true)` |
| R2 | Single call spans multiple blocks (push chain) | `axi_slave_port.hpp:151→rob.hpp:181→packetize.hpp:110` |
| R3 | Rob Enabled chain-flush frees slots / `notify_drained` early | `rob.hpp:272-348` — staging contract spec §6.1 |
| R4 | Side-effect commit atomicity (`w_meta_fifo_`, MetaBuffer) | spec §5.1 |
| R5 | Test-endpoint `pending_` counters assume multi-pop/tick | `nmu.hpp:226`, `nsu.hpp:157` |
| R6 | Integration watchdog bounds | `test_request_response_loopback.cpp`, `test_port_pair_loopback.cpp` |

---

### Task 1: `PipelineStage` primitive + inter-stage token types + depth config

**Files:**
- Create: `c_model/include/noc/pipeline_stage.hpp`, `c_model/include/nmu/ni_tokens.hpp`
- Modify: `c_model/include/nmu/nmu.hpp` (`NmuConfig` depth fields), `c_model/include/nsu/nsu.hpp` (`NsuConfig` depth fields)
- Test: `c_model/tests/noc/test_pipeline_stage.cpp`; `c_model/tests/noc/CMakeLists.txt`

**Interfaces:**
- Produces: `template<class Token> class PipelineStage` (`full/ready/occupancy/peek/accept/take/clear`); token structs (`AdmittedReq`, …); per-path depth config fields. Consumed by Tasks 3-5.

- [ ] **Step 1: Register the test** — in `c_model/tests/noc/CMakeLists.txt` add `add_cmodel_test(test_pipeline_stage)` + `target_include_directories(... PRIVATE ${CMAKE_SOURCE_DIR}/include)`.

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
    EXPECT_TRUE(s.ready());
}
TEST(PipelineStage, AcceptThenTake) {
    PipelineStage<int> s;
    s.accept(7);
    EXPECT_TRUE(s.full());
    EXPECT_EQ(s.occupancy(), 1u);
    EXPECT_FALSE(s.ready());
    EXPECT_EQ(s.peek(), 7);
    EXPECT_EQ(s.take(), 7);
    EXPECT_TRUE(s.ready());
}
TEST(PipelineStage, OverwriteAsserts) {  // mirrors router.hpp:185
    PipelineStage<int> s;
    s.accept(1);
    EXPECT_DEATH(s.accept(2), "PipelineStage: overwrite");
}
}  // namespace
```

- [ ] **Step 3: Run (expect compile failure)** — `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3`. Expected: FAIL (header missing).

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
// One-token stage register; the staged-NI building block. Mirrors Router::landing_
// (router.hpp:157) + overwrite assert (router.hpp:185). Reverse-order tick drains
// (take) before upstream fills (accept), so a token advances one stage/tick.
template <class Token>
class PipelineStage {
  public:
    bool full() const noexcept { return slot_.has_value(); }
    bool ready() const noexcept { return !slot_.has_value(); }
    std::size_t occupancy() const noexcept { return slot_ ? 1u : 0u; }
    const Token& peek() const { assert(slot_ && "PipelineStage: peek empty"); return *slot_; }
    void accept(Token t) { assert(!slot_ && "PipelineStage: overwrite (>1/cycle)"); slot_ = std::move(t); }
    Token take() { assert(slot_ && "PipelineStage: take empty"); Token t = std::move(*slot_); slot_.reset(); return t; }
    void clear() noexcept { slot_.reset(); }
  private:
    std::optional<Token> slot_;
};
}  // namespace ni::cmodel::noc
#endif
```

`c_model/include/nmu/ni_tokens.hpp` — the `{beat+meta}` tokens (spec §5.1). Define structs grouping the existing beat types with the computed meta they must carry between stages, e.g.:
```cpp
#ifndef NI_CMODEL_NI_TOKENS_HPP
#define NI_CMODEL_NI_TOKENS_HPP
#include "axi/types.hpp"
#include <cstdint>
namespace ni::cmodel {
// NMU req S1->S2: AW/AR admitted by Rob with route+rob meta computed in S1.
struct AdmittedAw { axi::AwBeat beat; uint8_t dst_id; uint64_t local_addr; uint8_t rob_req; uint16_t rob_idx; };
struct AdmittedAr { axi::ArBeat beat; uint8_t dst_id; uint64_t local_addr; uint8_t rob_req; uint16_t rob_idx; };
// W carries AW-inherited meta (matches packetize.hpp w_meta_fifo_ contract).
struct AdmittedW  { axi::WBeat beat; };
}  // namespace ni::cmodel
#endif
```
(Field set mirrors the existing `AwHeaderMeta` passed to `push_aw_with_meta`, `packetize.hpp:69`. Add the NSU-side and rsp-side token structs as each path task needs them — keep them in this one header.)

In `NmuConfig` (`nmu.hpp`) and `NsuConfig` (`nsu.hpp`) add per-path extra-depth knobs, all default 0:
```cpp
std::size_t ni_req_extra_depth = 0;   // extra shift stages on the request path
std::size_t ni_rsp_extra_depth = 0;   // extra shift stages on the response path
```

- [ ] **Step 5: Run (expect pass)** — `make build-cmodel ... && cd build/cmodel && ctest -R PipelineStage --output-on-failure`. Expected: 3/3 PASS; full build green (config fields compile).

- [ ] **Step 6: clang-format + commit**
```bash
clang-format -i c_model/include/noc/pipeline_stage.hpp c_model/include/nmu/ni_tokens.hpp c_model/include/nmu/nmu.hpp c_model/include/nsu/nsu.hpp c_model/tests/noc/test_pipeline_stage.cpp
git add -A && git commit -m "feat(ni): PipelineStage primitive + inter-stage token types + depth config"
```

---

### Task 2: `NiPath` enum + `stage_occupancy` getter scaffold

(Unchanged from the prior plan revision.) Create `c_model/include/nmu/ni_stage.hpp` with `enum class NiPath { NmuReq, NmuRsp, NsuReq, NsuRsp }`; add `std::size_t stage_occupancy(NiPath, std::size_t stage, uint8_t axi_ch) const` returning 0 on `Nmu`/`Nsu`/`NmuStandalone`/`NsuStandalone` (filled per path in Tasks 3-5). Test `test_ni_stage.cpp` asserts the enum distinct. Commit `feat(ni): NiPath enum + stage_occupancy scaffold`.

- [ ] **Step 1:** register `add_cmodel_test(test_ni_stage)` in `c_model/tests/common/CMakeLists.txt`.
- [ ] **Step 2:** write `test_ni_stage.cpp` (enum distinctness, as prior revision).
- [ ] **Step 3:** run → expect compile fail.
- [ ] **Step 4:** implement `ni_stage.hpp` + the 4 getters (return 0). Re-expose on the `*Standalone` wrappers (mirror `nmu.hpp:272`; add the NSU passthrough that does not exist yet).
- [ ] **Step 5:** run → `ctest -R NiStage`, full build green.
- [ ] **Step 6:** clang-format + commit.

---

### Task 3: AxiMaster write→read ordering (prereq) + NSU req vertical slice (2 stages)

Two coupled deliverables in one task: the staged NSU-req path correctly exposes a
read racing an in-flight write (AXI4-legal, spec §5.4), so the burst data-integrity
scenarios only pass once the **originating master** orders write→read per AXI4. Both
must land together; **burst-scenario coverage is kept, never deleted.**

**Part A — AxiMaster RAW ordering (prerequisite, spec §5.4).**
- File: `c_model/include/axi/axi_master.hpp` (the scenario-driver master). Test: `c_model/tests/axi/test_axi_master_raw_order.cpp` (+ CMake).
- Mechanism: before admitting/issuing a read, check whether its address range overlaps any **outstanding write** (issued, `B` not yet observed). If so, **hold the read** until that write's `B` arrives (the master already tracks write completion via `on_write_completed`, the B-driven signal used by the scoreboard — `test_port_pair_loopback.cpp:231`, `scoreboard.hpp:26`). Track outstanding-write address ranges `{addr, (len+1)<<size}`; release held reads on the matching `B`. Reads to non-overlapping addresses are NOT held (preserve concurrency). Do NOT add ordering to NSU `AxiMasterPort` — it is a transparent transport (spec §5.4).
- This is AXI4 §A5.3.4 master behavior; it changes nothing on the 0-cycle model except the read now waits for `B` (the read still sees the written data).

**Part B — NSU req register-parked staging (2 stages).**
- Files: `c_model/include/nsu/nsu.hpp`, `c_model/include/nsu/depacketize.hpp`, `c_model/include/nsu/axi_master_port.hpp`.
- Mechanism (spec §5.0/§5.3): `Depacketize` decodes ≤1 req flit/channel into S1 stage registers (MetaBuffer snapshot atomic with the decode, R4); `AxiMasterPort` consumes ≤1 beat/channel from S1 (S2). `Nsu::tick()` req portion reverse-order: S2 advance, then S1 advance. Replace the `while(true)` (`nsu/depacketize.hpp:99`) and `drain_*` loops (`axi_master_port.hpp:138`) with one-beat advance reading/writing the stage registers — NOT a direct `depkt_.pop_*` call-chain.

**Why this is the spike:** NSU req has no Rob and no arbiter — it proves the register-parked refactor end-to-end on the least-coupled path. Part A makes the existing burst scenarios AXI-correct so they survive staging.

**Interfaces:** Consumes `PipelineStage`/tokens (T1), `NiPath` (T2). Produces the register-parked pattern, the NSU flit-injection test helper, AxiMaster RAW ordering; fills `stage_occupancy(NsuReq,…)`.

**Part A steps:**
- [ ] **A1: failing test** — `c_model/tests/axi/test_axi_master_raw_order.cpp` (+ register in CMake): drive a write then a read to the SAME address with NO B-wait built into the scenario; assert the read's AR is issued only AFTER the write's `B` (observe via the master's issue-order / `on_write_completed` timing), and the read returns the written data. On the current model this currently issues the read immediately → test FAILs.
- [ ] **A2: implement** the RAW ordering in `axi_master.hpp` (track outstanding-write address ranges; hold an overlapping read until the write's `B`; release on `B`; non-overlapping reads not held). Run the new test PASS, then full c_model regression `cd build/cmodel && ctest --output-on-failure` — existing tests must pass; if any asserted a read issued before an overlapping write's B (relied on undefined ordering), fix it AXI-correctly (do NOT delete).
- [ ] **A3: commit** `fix(axi): master orders a read after an overlapping outstanding write (wait B)`.

**Part B steps:**
- [ ] **B1: failing test (req latency = 2)** — register `add_cmodel_test(test_nsu_pipeline)`; write:
```cpp
#include "nsu/nsu.hpp"
#include "nmu/ni_stage.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
using ni::cmodel::NiPath; using ni::cmodel::nsu::NsuStandalone; using ni::cmodel::nsu::NsuConfig;
namespace {
static void inject_single_ar_flit(NsuStandalone& nsu, uint8_t id, uint64_t addr);  // model on test_nsu_*
TEST(NsuPipeline, ReqLatencyIsTwoStages) {
    NsuConfig cfg; NsuStandalone nsu(cfg);
    inject_single_ar_flit(nsu, /*id=*/5, /*addr=*/0x40);
    nsu.tick();  // S1: Depacketize register holds AR; not yet at slave
    EXPECT_EQ(nsu.stage_occupancy(NiPath::NsuReq, 0, ni::AXI_CH_AR), 1u);
    EXPECT_FALSE(nsu.axi_master_port().pop_ar().has_value());
    nsu.tick();  // S2: AR drivable at slave port
    EXPECT_TRUE(nsu.axi_master_port().pop_ar().has_value());
}
}  // namespace
```
- [ ] **B2: run → FAIL** (today AR appears in 1 tick).
- [ ] **B3: implement** the register-parked NSU-req staging + the `inject_single_ar_flit` helper + `stage_occupancy(NsuReq,…)`.
- [ ] **B4: run** → `ctest -R "NsuPipeline|Nsu_|PortPair" --output-on-failure`. NsuPipeline passes; **the port-pair burst scenarios `AX4-BUR-002`/`AX4-BUR-005`/`AX4-BND-003` are KEPT and now pass** (Part A made write→read AXI-correct). Audit any direct `test_depacketize`/`test_axi_master_port` that assumed all-beats-after-one-tick → update to tick-per-stage (R1), do NOT delete coverage.
- [ ] **B5: commit** `feat(ni): NSU request path register-parked staging (2 stages)`.

---

### Task 4: NSU rsp path (3 stages) + arbiter final-stage pattern

**Establishes once:** the arbiter final-stage register (spec §5.2) — Packetize writes the S2→S3 stage register; the arbiter consumes it in the reverse-order tick; no same-tick Packetize→NoC escape. Reused by Task 5's NMU req.

**Files:** `c_model/include/nsu/nsu.hpp`, `c_model/include/nsu/axi_master_port.hpp`, `c_model/include/nsu/packetize.hpp`; add cases to `test_nsu_pipeline.cpp`.

**Mechanism:** `AxiMasterPort accept (B/R) → Packetize → [S2→S3 reg] → Wormhole+VcArbiter → NoC`. MetaBuffer `commit_*` moves to the stage that pushes into the S2→S3 register success (R4). `stage_occupancy(NsuRsp,…)`.

- [ ] **Step 1: failing test (rsp latency = 3, + no same-tick escape):**
```cpp
TEST(NsuPipeline, RspLatencyIsThreeStages) {
    NsuConfig cfg; NsuStandalone nsu(cfg);
    seed_meta_for_b(nsu, /*id=*/3);                  // helper: snapshot meta so Packetize can build B
    nsu.axi_master_port().push_b(make_b_beat(3));
    for (int t = 0; t < 2; ++t) { nsu.tick(); EXPECT_FALSE(nsu.pop_rsp_flit().has_value()) << "escape at " << t; }
    nsu.tick();
    EXPECT_TRUE(nsu.pop_rsp_flit().has_value());
}
```
- [ ] **Step 2:** run → FAIL (emits in 1 tick today).
- [ ] **Step 3:** implement the rsp staging + the arbiter final-stage register + MetaBuffer commit-on-stage-push. Bound `forward_b/r_to_packetizer_` (`axi_master_port.hpp:159`).
- [ ] **Step 4:** run → `ctest -R "NsuPipeline|Nsu_"`, all pass.
- [ ] **Step 5:** clang-format + commit `feat(ni): NSU response path staging (3 stages) + arbiter final-stage`.

---

### Task 5: NMU req + rsp (shared `Rob`) — staging + ROB/ROBLESS + depth

**One task** because both NMU paths edit `Rob` (R2/R3). Implements: NMU req (3 stages), NMU rsp (3 stages, code order Depacketize→Rob→AxiSlavePort), the ROB Enabled one-beat-release staging contract (spec §6.1), the ROBLESS retire hook (spec §6), and the depth knobs.

**Files:** `c_model/include/nmu/nmu.hpp`, `axi_slave_port.hpp`, `rob.hpp`, `packetize.hpp`, `depacketize.hpp`; test `c_model/tests/nmu/test_nmu_pipeline.cpp` (+ CMake).

**Mechanism:**
- **req:** `AxiSlavePort accept + Rob admit/allocate + Address Map (xy_route, rob.hpp:180/218) → [S1 reg: AdmittedAw/Ar token] → Packetize build → [S2→S3 reg] → arbiter → NoC`. Rob admit writes the S1 token register; it does **not** call Packetize (R2). `w_meta_fifo_` mutate moves to the S2→S3 push (R4). Reuse Task 4's arbiter final-stage.
- **rsp:** `Depacketize → [S1 reg] → Rob Re-Ordering → [S2 reg] → AxiSlavePort → master`. ROB Enabled: reorder decision computes ready run, but slot-free/`notify_drained`/outstanding-retire fire **when a beat leaves the Re-Ordering stage register** (one beat/tick), per spec §6.1 — not on chain-flush. ROBLESS (`Disabled`): Re-Ordering collapses to passthrough (2 stages); the rsp side still retires the per-id counter (`rob.hpp:289/353`).
- **depth:** `ni_req_extra_depth`/`ni_rsp_extra_depth` insert extra `PipelineStage` shift registers.

- [ ] **Step 1: failing tests** (`test_nmu_pipeline.cpp`):
```cpp
TEST(NmuPipeline, ReqLatencyIsThreeStages) {
    NmuConfig cfg; NmuStandalone nmu(cfg);                 // RobMode default Disabled
    nmu.axi_slave_port().push_ar(make_ar_beat(5, 0x100000000ull));
    for (int t = 0; t < 2; ++t) { nmu.tick(); EXPECT_FALSE(nmu.pop_req_flit().has_value()); }
    nmu.tick(); EXPECT_TRUE(nmu.pop_req_flit().has_value());
}
TEST(NmuPipeline, RspLatencyRobIsThree) {
    NmuConfig cfg; cfg.read_rob_mode = ni::cmodel::nmu::RobMode::Enabled;
    NmuStandalone nmu(cfg); inject_single_r_flit(nmu, 5);  // in-order head -> 0 hold
    for (int t = 0; t < 2; ++t) { nmu.tick(); EXPECT_FALSE(nmu.axi_slave_port().pop_r().has_value()); }
    nmu.tick(); EXPECT_TRUE(nmu.axi_slave_port().pop_r().has_value());
}
TEST(NmuPipeline, RspLatencyRoblessIsTwo) {
    NmuConfig cfg; cfg.read_rob_mode = ni::cmodel::nmu::RobMode::Disabled;
    NmuStandalone nmu(cfg); inject_single_r_flit(nmu, 5);
    nmu.tick(); EXPECT_FALSE(nmu.axi_slave_port().pop_r().has_value());
    nmu.tick(); EXPECT_TRUE(nmu.axi_slave_port().pop_r().has_value());
}
TEST(NmuPipeline, DepthKnobAddsLatency) {
    NmuConfig cfg; cfg.read_rob_mode = ni::cmodel::nmu::RobMode::Disabled; cfg.ni_rsp_extra_depth = 2;
    NmuStandalone nmu(cfg); inject_single_r_flit(nmu, 5);
    for (int t = 0; t < 3; ++t) { nmu.tick(); EXPECT_FALSE(nmu.axi_slave_port().pop_r().has_value()); }
    nmu.tick(); EXPECT_TRUE(nmu.axi_slave_port().pop_r().has_value());  // 2 base + 2 extra
}
```
- [ ] **Step 2:** run → FAIL.
- [ ] **Step 3:** implement req staging, then rsp staging + the §6.1 ROB Enabled contract + ROBLESS retire + depth knobs. Add NSU/NMU rsp+req token structs to `ni_tokens.hpp` as needed.
- [ ] **Step 4: run + full NMU/NSU/Rob regression** — `ctest -R "NmuPipeline|Nmu_|Nsu|rob" --output-on-failure`. Expected: all pass. Audit `test_rob.cpp` for any "all beats after one tick / one pop" assumption (R1) and update.
- [ ] **Step 5:** clang-format + commit `feat(ni): NMU req+rsp staging + ROB/ROBLESS modes + depth knobs`.

---

### Task 6: Integration regression, co-sim, docs

**Files:** affected integration/unit tests; `docs/performance-probe.md`. Verify co-sim.

- [ ] **Step 1: full c_model regression** — `make build-cmodel ... && cd build/cmodel && ctest --output-on-failure`. Raise `kMaxCycles` watchdogs (`test_request_response_loopback.cpp`, `test_port_pair_loopback.cpp`) if staging pushed totals over bound (R6); update stale "single tick sufficient" comments (`test_nmu.cpp:136`, `test_nsu.cpp:117`); verify endpoint `pending_` credit accounting still holds (R5).
- [ ] **Step 2: co-sim** — `make build-verilator PYTHON3=/c/msys64/mingw64/bin/python3 && cd cosim/verilator && make run-tb-top SCENARIO=AX4-BAS-003_single_write_read_aligned`. Expected: builds, scoreboard 0 mismatches, perf.json emitted; end-to-end latency higher (NI now real pipeline).
- [ ] **Step 3: perf-doc rewrite** — `docs/performance-probe.md` "pipeline fidelity": NI is now a real pipeline (3/3/2/3), not a wrap artifact; keep the wrapper-register note (separate, spec §7); refresh worked-example numbers from a fresh run; drop "NI pipeline model planned" from Known limitations.
- [ ] **Step 4:** commit `test(ni): integration+co-sim regression; perf-doc fidelity rewrite`.

---

## Self-Review

**Spec coverage:** §4 stage allocation → T3/T4/T5; §5.0-5.3 register-parked + tokens + arbiter final-stage + bounded advance → T1 (primitive/tokens) + T3/T4/T5; §6 ORDERING_MODE + §6.1 ROB Enabled contract → T5; §7 boundary attribution → stage_occupancy in T2-T5 + T6 perf-doc; §8 validation (advance/latency/backpressure/reorder/ROB-ROBLESS/depth) → T1/T3/T4/T5 tests; §8 API → T2 scaffold + T3/T4/T5 fill; §7 consequences (tests/perf-doc) → T6.

**Placeholder scan:** test helpers (`inject_single_ar_flit`, `seed_meta_for_b`, `make_b_beat`, `inject_single_r_flit`, `make_ar_beat`) are implemented in their first-using task (T3/T4/T5), modeled on existing `test_nsu_*`/`test_nmu_*` flit encoders; named, not deferred. Mechanism detail lives in spec §5 (referenced, not restated). No TBD.

**Type consistency:** `PipelineStage<Token>` API uniform T1→T5; `stage_occupancy(NiPath, std::size_t, uint8_t)` identical everywhere; token structs centralized in `ni_tokens.hpp`; depth knobs `ni_req_extra_depth`/`ni_rsp_extra_depth` consistent.

**Sequencing safety:** T3 (NSU req) is the spike; T4 establishes the arbiter final-stage; T5 combines both NMU paths (shared Rob) so no two workers edit `rob.hpp` concurrently. Each path task re-runs the prior path's tests (T4 re-runs Nsu_, T5 re-runs Nmu_+Nsu+rob) to catch shared-file regressions. T6 is the integration net.

**Risk coverage:** R1 (T3-T6 bound loops + test audits), R2 (T3/T5 break the chain via stage registers), R3 (T5 per spec §6.1 contract), R4 (T3-T5 commit-on-stage-push), R5 (T6 endpoint counters), R6 (T6 watchdogs).
