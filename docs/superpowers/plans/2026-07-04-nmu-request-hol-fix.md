# NMU request-path HOL deadlock fix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the NMU request-path self-deadlock by decoupling the `NmuReqS1Bridge` so AW/W/AR drain independently, with AW-before-W ordering preserved by the write-metadata FIFO empty-guard.

**Architecture:** Two localized code changes in the NMU request path (`nmu.hpp` bridge tick, `packetize.hpp` push_w), plus an invariant comment in the NSU depacketize. A deterministic ctest reproduces the deadlock at the bridge+wormhole+packetize level, then a co-sim run confirms the previously-deadlocking 8R/8W load completes.

**Tech Stack:** C++17, GoogleTest, CMake; co-sim on WSL Verilator 5.048 + z3.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-04-nmu-request-hol-fix-design.md`.
- Fix approach: independent per-channel bridge drain + `push_w` backpressure on empty `w_meta_fifo_`. No structural change to WormholeArbiter / VcArbiter / Rob / AxiSlavePort / SV / DPI / fabric.
- Load-bearing assumption: AXI4 write data is non-interleaved (a source's W beats follow its AW in issue order). Document at the `w_meta_fifo_` and `push_w` sites.
- Naming unchanged: keep `w_meta_fifo_`.
- Validation scope this round: `mesh_4x4_vc1`, RobMode Disabled. RobMode Enabled and vc2/4/8 share the code path but stay out of the pass claim.
- Run `clang-format -i` on every edited `.hpp`/`.cpp`.
- Build/run on Windows uses `PYTHON3=python3`; co-sim runs on WSL detached (z3-bound, ~34 sim-cycles/s).
- No vendor/product-guide names in code or docs. FlooNoC (open-source) references are acceptable, matching existing code comments.

---

### Task 1: Decouple the NMU request bridge (the fix)

**Files:**
- Create: `src/c_model/tests/nmu/test_nmu_req_bridge.cpp`
- Modify: `src/c_model/tests/nmu/CMakeLists.txt` (register the new test)
- Modify: `src/c_model/include/nmu/nmu.hpp:70-93` (`NmuReqS1Bridge::tick`)
- Modify: `src/c_model/include/nmu/packetize.hpp:127-145` (`Packetize::push_w`)
- Modify: `src/c_model/include/nsu/depacketize.hpp:128` (invariant comment only)

**Interfaces:**
- Consumes: `NmuReqS1Bridge` (push_aw_with_meta / push_w / push_ar_with_meta / tick(Packetize&) / occupancy(uint8_t)), `Packetize(NocReqOut&, NocReqOut&, NocReqOut&, uint8_t)`, `WormholeArbiter<NocReqOut>(Downstream&, size_t num_inputs, {ChannelPairing{0,1}}, size_t depth)` with `.input(i)` / `.tick()` / `.pending_size(i)` / `.is_locked()`, `ReqCapture` (push_flit / pop / size), `AwHeaderMeta{dst_id, local_addr, rob_req, rob_idx}`.
- Produces: no new public API. Behavior change only: bridge drains channels independently; `push_w` returns false instead of asserting when `w_meta_fifo_` is empty.

**Test coverage note.** The two tests below are the decisive ones (deadlock trigger + empty-meta backpressure). The spec's other two cases are already covered without new tests: back-to-back / multi-beat W-meta ordering by `test_packetize` (existing burst tests, `test_packetize.cpp:99-122`), and multi-ID writes by the co-sim 8R/8W integration in Task 2 (random ids). Not padded here per test-meaningfulness.

- [ ] **Step 1: Write the failing test**

Create `src/c_model/tests/nmu/test_nmu_req_bridge.cpp`:

```cpp
// Regression: the NMU request bridge must NOT head-of-line-block W/AR behind a
// full AW wormhole input. Reproduces the 8R/8W co-sim self-deadlock
// deterministically at the bridge+packetize+wormhole level (spec
// 2026-07-04-nmu-request-hol-fix-design.md).
#include "nmu/nmu.hpp"
#include "nmu/packetize.hpp"
#include "ni/wormhole_arbiter.hpp"
#include "common/per_channel_capture.hpp"
#include "common/scenario.hpp"
#include "axi/types.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
#include <vector>

using ni::cmodel::nmu::AwHeaderMeta;
using ni::cmodel::nmu::NmuReqS1Bridge;
using ni::cmodel::nmu::Packetize;
using ni::cmodel::router::NocReqOut;
using ni::cmodel::router::WormholeArbiter;
using ni::cmodel::router::ChannelPairing;
using ni::cmodel::testing::ReqCapture;
namespace axi = ni::cmodel::axi;

namespace {
constexpr uint8_t kSrcId = 0x12;
constexpr uint8_t kDst = 0x03;
constexpr std::size_t kAwInputDepth = 4;

axi::AwBeat make_aw(uint8_t id) {
    axi::AwBeat b{};
    b.id = id; b.addr = 0x1000; b.len = 0; b.size = 5; b.burst = axi::Burst::INCR;
    return b;
}
axi::WBeat make_w(bool last) {
    axi::WBeat b{};
    for (int i = 0; i < 32; ++i) b.data[i] = static_cast<uint8_t>(i);
    b.strb = 0xFFFFFFFF; b.last = last;
    return b;
}
axi::ArBeat make_ar(uint8_t id) {
    axi::ArBeat b{};
    b.id = id; b.addr = 0x2000; b.len = 0; b.size = 5; b.burst = axi::Burst::INCR;
    return b;
}
AwHeaderMeta meta() { return AwHeaderMeta{kDst, 0x1000, 0, 0}; }
}  // namespace

// Wires the request sub-assembly as Nmu does: Packetize -> WormholeArbiter
// (3 inputs, AW->W pairing {0,1}, per-input depth 4) -> ReqCapture; the bridge
// feeds Packetize. step() advances the bridge THEN the arbiter (drain then
// grant). This is the opposite local order from Nmu::tick (which ticks the
// arbiter before the bridge). It is intentional: combining enqueue and grant in
// one helper step keeps the isolated regression deterministic. The deadlock
// under test is order-independent (a held lock never releases either way).
TEST(NmuReqBridge, WAndArDrainDespiteFullAwInput) {
    SCENARIO(
        "Bridge must drain W (for the locked write) and admit AR even when the "
        "wormhole AW-input is full and a later AW is stuck in the bridge. Pre-fix "
        "this self-deadlocks (line-78 HOL); post-fix W/AR flow.");
    ReqCapture out;
    WormholeArbiter<NocReqOut> wh(out, /*num_inputs=*/3,
                                  std::vector<ChannelPairing>{{0, 1}}, kAwInputDepth);
    Packetize pkt(wh.input(0), wh.input(1), wh.input(2), kSrcId);
    NmuReqS1Bridge bridge;

    auto step = [&] { bridge.tick(pkt); wh.tick(); };

    // AW0 flows through and locks the arbiter to the W input (pairing {0,1}).
    ASSERT_TRUE(bridge.push_aw_with_meta(make_aw(0), meta()));
    step();
    EXPECT_TRUE(wh.is_locked()) << "AW0 (last=0) must lock the arbiter to its W";

    // AW1..AW4 accumulate in the wormhole AW input (locked -> not granted),
    // filling it to depth 4.
    for (uint8_t id = 1; id <= kAwInputDepth; ++id) {
        ASSERT_TRUE(bridge.push_aw_with_meta(make_aw(id), meta()));
        step();
    }
    EXPECT_EQ(wh.pending_size(0), kAwInputDepth) << "AW input full";

    // A further AW is stuck in the bridge slot (wormhole AW input full).
    ASSERT_TRUE(bridge.push_aw_with_meta(make_aw(99), meta()));
    step();
    EXPECT_EQ(bridge.occupancy(ni::AXI_CH_AW), 1u) << "5th AW stuck in bridge";

    // The locked write's W and an independent AR are now offered.
    ASSERT_TRUE(bridge.push_w(make_w(/*last=*/true)));
    ASSERT_TRUE(bridge.push_ar_with_meta(make_ar(9), meta()));
    for (int i = 0; i < 6; ++i) step();

    bool got_w = false;
    while (auto f = out.pop())
        if (f->get_header_field("axi_ch") == static_cast<uint64_t>(ni::AXI_CH_W)) got_w = true;
    EXPECT_TRUE(got_w) << "W must drain despite full AW input (pre-fix deadlocks here)";
    EXPECT_EQ(bridge.occupancy(ni::AXI_CH_W), 0u) << "W left the bridge";
    EXPECT_EQ(bridge.occupancy(ni::AXI_CH_AR), 0u) << "AR admitted independently of stuck AW";
}

// push_w with no prior push_aw must backpressure (return false), not assert, so
// a W whose AW is not yet admitted waits without aborting or blocking AW/AR.
TEST(NmuReqBridge, PushWBackpressuresOnEmptyMeta) {
    SCENARIO("Packetize::push_w returns false when w_meta_fifo_ is empty.");
    ReqCapture aw_out, w_out, ar_out;
    Packetize pkt(aw_out, w_out, ar_out, kSrcId);
    EXPECT_FALSE(pkt.push_w(make_w(/*last=*/true)))
        << "W with no admitted AW must backpressure, not abort";
}
```

- [ ] **Step 2: Register the test and run it to verify it fails**

Add to `src/c_model/tests/nmu/CMakeLists.txt` (after the `add_cmodel_test(test_packetize)` line).
The test includes `nmu/nmu.hpp`, which transitively pulls yaml-cpp via `nmu/port_params.hpp`, so
it must link yaml-cpp (matching the sibling `test_nmu` / `test_nmu_credit` / `test_nmu_pipeline`
targets that include the full Nmu; `test_packetize` does not link it because it only includes
`packetize.hpp`):

```cmake
add_cmodel_test(test_nmu_req_bridge)
target_link_libraries(test_nmu_req_bridge PRIVATE yaml-cpp::yaml-cpp)
```

Run:

```bash
cd E:/05_NoC/noc_project && rm -rf build/cmodel && make build-cmodel PYTHON3=python3
cd build/cmodel && ctest -R test_nmu_req_bridge --output-on-failure
```

Expected: FAIL. `WAndArDrainDespiteFullAwInput` fails at `EXPECT_TRUE(got_w)` (W never emitted, line-78 HOL). `PushWBackpressuresOnEmptyMeta` aborts on the `assert` in `push_w` (pre-fix). Both are the RED state.

- [ ] **Step 3: Apply the fix — bridge independent drain**

In `src/c_model/include/nmu/nmu.hpp`, replace `NmuReqS1Bridge::tick` (lines 70-93):

```cpp
    // Drain each AXI sub-channel to Packetize INDEPENDENTLY. A full AW wormhole
    // input must not block W (the in-flight write's body, needed to release the
    // wormhole AW->W lock) or AR. Cross-channel HOL here self-deadlocks the
    // request path under load (spec 2026-07-04-nmu-request-hol-fix-design.md).
    // AW-before-W ordering is preserved downstream by Packetize's w_meta_fifo_,
    // not by gating W on AW admission.
    void tick(Packetize& packetize) {
        if (s1_aw_.full()) {
            const auto& e = s1_aw_.peek();
            if (packetize.push_aw_with_meta(e.beat,
                                            {e.dst_id, e.local_addr, e.rob_req, e.rob_idx})) {
                s1_aw_.take();
            }
        }
        if (s1_w_.full()) {
            const auto& e = s1_w_.peek();
            if (packetize.push_w(e.beat)) {
                s1_w_.take();
            }
        }
        if (s1_ar_.full()) {
            const auto& e = s1_ar_.peek();
            if (packetize.push_ar_with_meta(e.beat,
                                            {e.dst_id, e.local_addr, e.rob_req, e.rob_idx})) {
                s1_ar_.take();
            }
        }
    }
```

- [ ] **Step 4: Apply the fix — push_w backpressure on empty meta**

In `src/c_model/include/nmu/packetize.hpp`, change `Packetize::push_w` (line 127-129). Replace the assert with a backpressure return:

```cpp
inline bool Packetize::push_w(const axi::WBeat& b) {
    // A W beat inherits its AW's dst/rob metadata from the front of w_meta_fifo_.
    // If empty, the W's AW has not yet been admitted to Packetize (its AW is
    // still upstream in the bridge). Backpressure so the W waits WITHOUT blocking
    // AW/AR admission. AXI4 W is non-interleaved (no WID), so the FIFO front is
    // always the correct write for the next W beat in issue order.
    if (w_meta_fifo_.empty()) return false;
    const auto& meta = w_meta_fifo_.front();
```

Leave the rest of `push_w` unchanged.

- [ ] **Step 5: Add the NSU invariant comment (no behavior change)**

In `src/c_model/include/nsu/depacketize.hpp`, at `Depacketize::tick` (line 128), prepend a comment documenting why the single-ingress HOL cannot self-deadlock:

```cpp
// Single-ingress HOL note: unlike the NMU request path, NSU depacketize has NO
// source-side pairing lock on ingress. It demuxes into independent S1 registers
// that drain into bounded AxiMasterPort queues, which drain to the subordinate.
// The pending_ HOL is inherent to a single VC (AW/W/AR serialize on one channel)
// but cannot self-cycle: no ingress resource waits on a downstream that waits
// back on it. Given the subordinate eventually drains, pending_ always clears.
inline void Depacketize::tick() {
```

- [ ] **Step 6: clang-format and run the new test to verify it passes**

```bash
clang-format -i src/c_model/include/nmu/nmu.hpp src/c_model/include/nmu/packetize.hpp \
    src/c_model/include/nsu/depacketize.hpp src/c_model/tests/nmu/test_nmu_req_bridge.cpp
cd E:/05_NoC/noc_project && make build-cmodel PYTHON3=python3
cd build/cmodel && ctest -R test_nmu_req_bridge --output-on-failure
```

Expected: PASS (both tests).

- [ ] **Step 7: Run the NMU/NSU/packetize regression to confirm no break**

```bash
cd build/cmodel && ctest -R "test_nmu|test_nsu|test_packetize|test_depacketize|test_rob|test_vc_arbiter" --output-on-failure
```

Expected: all PASS (the fix touches no existing tested behavior; push_w's empty-meta case had no prior test).

- [ ] **Step 8: Commit**

```bash
git add src/c_model/include/nmu/nmu.hpp src/c_model/include/nmu/packetize.hpp \
    src/c_model/include/nsu/depacketize.hpp \
    src/c_model/tests/nmu/test_nmu_req_bridge.cpp src/c_model/tests/nmu/CMakeLists.txt
git commit -m "fix(nmu): decouple request bridge to break AW-input HOL self-deadlock

NmuReqS1Bridge::tick gated W/AR drain on AW admission (line 78); with the
wormhole AW->W pairing needing W to release the AW lock, a full AW input
self-deadlocked the request path under load. Drain AW/W/AR independently;
Packetize::push_w backpressures on empty w_meta_fifo_ instead of asserting,
preserving AW-before-W via the meta FIFO. FlooNoC-aligned (Codex-confirmed).
NSU audited: no analogous self-deadlock (comment added).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Integration verification + backlog closure

**Files:**
- Modify: `docs/backlog.md` (close #0), `.superpowers/sdd/progress.md` (ledger), `docs/superpowers/specs/2026-07-04-nmu-request-hol-fix-design.md` (status -> landed)

**Interfaces:**
- Consumes: the Task 1 fix (header-only, so the co-sim binary picks it up on rebuild).
- Produces: verified deadlock-free 8R/8W co-sim run.

- [ ] **Step 1: Windows compile gate**

```bash
cd E:/05_NoC/noc_project && make build-verilator TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3
```

Expected: rc=0.

- [ ] **Step 2: WSL rebuild + 8R/8W run (the exact prior repro)**

```bash
wsl.exe -d Ubuntu -u root -- bash -c 'export PATH=/usr/local/bin:/usr/bin:/bin; \
  cd /mnt/e/05_NoC/noc_project/sim/verilator && \
  make BUILD_ROOT=/root/noc_build FILELIST_F=/root/noc_build/filelist_mesh_4x4_vc1.f \
       TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3'
```

Then launch the run detached (z3-bound, budget hours; do NOT block the session — poll):

```bash
wsl.exe -d Ubuntu -u root -- bash -c 'export PATH=/usr/local/bin:/usr/bin:/bin; \
  export VERILATOR_SOLVER="z3 --in"; \
  cd /mnt/e/05_NoC/noc_project/sim/verilator && \
  setsid bash -c "/root/noc_build/verilator/obj_dir_mesh_4x4_vc1_data_integrity/Vtb_top \
    +num_reads=8 +num_writes=8 +verilator+seed+1 \
    > output/forensics/run_8r8w_s1_postfix.log 2>&1" < /dev/null > /dev/null 2>&1 &'
```

Expected on completion: `PASS: all 16 nodes done, non-vacuous`, no `[FABRIC-DUMP]`, no `timeout`, no `%Error`. If it still deadlocks (watchdog + dump), STOP and re-open debugging — the fix is insufficient.

- [ ] **Step 3: ctest full regression**

```bash
cd E:/05_NoC/noc_project/build/cmodel && ctest --output-on-failure -j 8
```

Expected: pass count = prior baseline + 2 (the new bridge tests), 0 fail (GCC ICE on test_pins_smoke is a known pre-existing toolchain flake, not this change).

- [ ] **Step 4: Close backlog #0 + update ledger + spec status**

- `docs/backlog.md`: mark item #0 (load-dependent DUT deadlock) RESOLVED with the root cause (NmuReqS1Bridge line-78 HOL + wormhole AW->W pairing) and the fix commit.
- `.superpowers/sdd/progress.md`: add the fix + verification line.
- Spec header `Status:` -> `landed <commit>`.

- [ ] **Step 5: Commit**

```bash
git add docs/backlog.md .superpowers/sdd/progress.md \
    docs/superpowers/specs/2026-07-04-nmu-request-hol-fix-design.md
git commit -m "docs(backlog): close #0 NMU request-path HOL deadlock (fixed)"
```
