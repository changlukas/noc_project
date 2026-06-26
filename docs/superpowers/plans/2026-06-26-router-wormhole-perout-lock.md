# Router per-output wormhole lock + read-ROB guard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix Family B (cross-source write corruption) by making the c_model router hold each output to one wormhole packet across VCs, and guard the unreachable Family C read path with a fail-fast.

**Architecture:** Replace the router's per-`(output, vc)` wormhole lock with one lock per output that pins `(input, vc)` until the packet's `last` flit. Add a guard in the NMU ROB that aborts on Enabled-mode multi-beat reads (the unimplemented per-ID offset case).

**Tech Stack:** C++17, GoogleTest (ctest), Verilator co-sim (`make sim`).

**Spec:** `docs/superpowers/specs/2026-06-26-router-wormhole-perout-lock-design.md`

## Global Constraints

- Build: `make build-cmodel PYTHON3=python3`. Tests: `make test PYTHON3=python3`.
- Co-sim needs clean rebuild: `rm -rf build/verilator/obj_dir_<topo> build/verilator/output_<vcN>` before `make sim`.
- C++ continuation indent 4 spaces. Run `clang-format -i` on edited `.hpp`/`.cpp`.
- Commit format `type(scope): description`. Do not push.
- Files in scope: `c_model/include/router/router.hpp`, `c_model/tests/router/test_router.cpp`, `c_model/include/nmu/rob.hpp`, `c_model/tests/nmu/test_rob.cpp`.

---

### Task 1: Per-output wormhole lock (data structure + tick + core no-interleave test)

**Files:**
- Modify: `c_model/include/router/router.hpp` (WormholeState struct, `wormhole_` member, constructor, `tick()` stage 2)
- Test: `c_model/tests/router/test_router.cpp`

**Interfaces:**
- Consumes: existing `Router` API and the existing test infrastructure in `test_router.cpp`:
  `Packet{in_port, src_id}`, `feed_packet(r, pkt, dst, vc)` (pushes one flit/port/tick, a 3-flit
  head/body/tail packet), `FlitSink` (records delivered flits), `make_dst(x, y)`, `center_cfg()`
  (center is `x=1, y=1`). Reuse these; do not hand-roll a new sink or push flits directly (a link
  accepts only one flit per cycle, `router.hpp:185-189`).
- Produces: per-output wormhole semantics. No public API change.

- [ ] **Step 1: Write the failing test**

Model it on the existing `RouterWormhole.PacketsDoNotInterleavePerOutputVc` (`test_router.cpp:170`),
but put the two packets on DIFFERENT VCs (A vc0, B vc1) and return credit for each delivered flit's
own VC. Add to `c_model/tests/router/test_router.cpp`:

```cpp
TEST(RouterWormhole, PacketsOnDifferentVcsDoNotInterleavePerOutput) {
    SCENARIO(
        "Router per-output wormhole lock: two 3-flit packets to the same output "
        "on different VCs (A on vc0, B on vc1) drain as two contiguous packets. "
        "The current per-(output,vc) lock + VC round-robin interleaves them.");
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);  // routes EAST from center (x=1)
    Packet a{static_cast<std::size_t>(RouterPort::WEST), /*src_id=*/0x10};
    Packet b{static_cast<std::size_t>(RouterPort::SOUTH), /*src_id=*/0x20};
    for (int t = 0; t < 24; ++t) {
        feed_packet(r, a, dst, /*vc=*/0);
        if (t >= 1) feed_packet(r, b, dst, /*vc=*/1);
        const std::size_t before = east.received.size();
        r.tick();
        for (std::size_t i = before; i < east.received.size(); ++i)
            r.receive_credit(E, static_cast<uint8_t>(east.received[i].get_header_field("vc_id")));
    }
    ASSERT_EQ(east.received.size(), 6u);
    int runs = 1;
    for (std::size_t i = 1; i < east.received.size(); ++i) {
        const uint8_t s = static_cast<uint8_t>(east.received[i].get_header_field("src_id"));
        const uint8_t prev = static_cast<uint8_t>(east.received[i - 1].get_header_field("src_id"));
        if (s != prev) ++runs;
    }
    EXPECT_EQ(runs, 2) << "cross-VC packet flits interleaved on EAST output";
}
```

- [ ] **Step 2: Run the test, verify it fails**

Run: `make build-cmodel PYTHON3=python3 && ./build/cmodel/tests/router/test_router.exe --gtest_filter='*PacketsOnDifferentVcsDoNotInterleavePerOutput*'`
Expected: FAIL. The current VC round-robin alternates vc0/vc1 on EAST, so `runs` is 6 (A and B flits interleaved), not 2.

- [ ] **Step 3: Change WormholeState and the `wormhole_` member**

In `c_model/include/router/router.hpp`, replace the `WormholeState` struct (currently at `:151-154`):

```cpp
    struct WormholeState {
        std::optional<std::size_t> locked_input;
        std::optional<uint8_t> locked_vc;  // VC of the in-flight wormhole packet
        std::size_t rr = 0;                // input round-robin (unlocked scan)
    };
```

Replace the member declaration (currently `std::array<std::vector<WormholeState>, ROUTER_PORT_COUNT> wormhole_{};` at `:161`):

```cpp
    std::array<WormholeState, ROUTER_PORT_COUNT> wormhole_{};  // per-output (across VCs)
```

- [ ] **Step 4: Drop the per-VC resize in the constructor**

In the constructor loop (currently `:88-93`), delete the line `wormhole_[p].resize(cfg_.num_vc);`. The loop keeps `input_fifo_[p].resize`, `credit_[p].assign`, `input_adapters_.emplace_back`.

- [ ] **Step 5: Rewrite stage 2 of `tick()`**

Replace the stage-2 block (currently `:208-248`, the `for (out ...)` that scans VCs with per-`(out,vc)` `wormhole_[out][vc]`) with:

```cpp
    // Stage 2: per-output grant. One wormhole packet per output across VCs.
    for (std::size_t out = 0; out < ROUTER_PORT_COUNT; ++out) {
        if (output_fifo_[out].size() >= cfg_.output_fifo_depth) continue;
        auto& ws = wormhole_[out];
        std::optional<std::size_t> candidate;
        uint8_t vc = 0;
        if (ws.locked_input.has_value()) {
            // Locked: serve only the in-flight (input, vc) until its last flit.
            vc = *ws.locked_vc;
            auto& lq = input_fifo_[*ws.locked_input][vc];
            if (!lq.empty() && credit_[out][vc] > 0) {
                const auto dst = static_cast<uint8_t>(lq.front().get_header_field("dst_id"));
                if (static_cast<std::size_t>(route_compute(dst, cfg_)) != out) {
                    assert(false &&
                           "Router: locked wormhole continuation routes to a different output "
                           "(malformed packet: last=0 head not closed by last=1 on this (input,vc))");
                    std::abort();
                }
                candidate = ws.locked_input;
            }
        } else {
            // Unlocked: VC round-robin, then input round-robin; first flit routed
            // here with credit wins.
            for (std::size_t kv = 0; kv < cfg_.num_vc && !candidate.has_value(); ++kv) {
                vc = static_cast<uint8_t>((vc_rr_[out] + kv) % cfg_.num_vc);
                if (credit_[out][vc] == 0) continue;
                for (std::size_t j = 0; j < ROUTER_PORT_COUNT; ++j) {
                    const std::size_t in = (ws.rr + j) % ROUTER_PORT_COUNT;
                    const auto& q = input_fifo_[in][vc];
                    if (q.empty()) continue;
                    const auto dst = static_cast<uint8_t>(q.front().get_header_field("dst_id"));
                    if (static_cast<std::size_t>(route_compute(dst, cfg_)) == out) {
                        candidate = in;
                        break;
                    }
                }
            }
        }
        if (!candidate.has_value()) continue;

        // Grant (spec sec 5): single atomic event.
        auto& q = input_fifo_[*candidate][vc];
        const Flit flit = q.front();
        q.pop_front();
        assert(credit_[out][vc] > 0 && "Router: credit underflow");
        --credit_[out][vc];
        output_fifo_[out].push_back(flit);
        credit_pulse_pending_.emplace_back(*candidate, static_cast<uint8_t>(vc));
        const uint64_t last = flit.get_header_field("last");
        if (last == 0) {
            ws.locked_input = *candidate;
            ws.locked_vc = vc;
        } else {
            ws.locked_input.reset();
            ws.locked_vc.reset();
            ws.rr = (*candidate + 1) % ROUTER_PORT_COUNT;
            vc_rr_[out] = static_cast<std::size_t>((vc + 1) % cfg_.num_vc);
        }
    }
```

- [ ] **Step 6: Run the test, verify it passes**

Run: `clang-format -i c_model/include/router/router.hpp && make build-cmodel PYTHON3=python3 && ./build/cmodel/tests/router/test_router.exe --gtest_filter='*PacketsOnDifferentVcsDoNotInterleavePerOutput*'`
Expected: PASS.

- [ ] **Step 7: Run the full router suite**

Run: `./build/cmodel/tests/router/test_router.exe`
Expected: report PASS/FAIL counts. Some existing tests may now fail if they assumed cross-VC interleave; those are handled in Task 3. Note which fail.

- [ ] **Step 8: Commit**

```bash
git add c_model/include/router/router.hpp c_model/tests/router/test_router.cpp
git commit -m "fix(router): per-output wormhole lock across VCs (Family B)"
```

---

### Task 2: Wormhole edge-case unit tests

**Files:**
- Test: `c_model/tests/router/test_router.cpp`

**Interfaces:**
- Consumes: Task 1 semantics + existing infra (`Packet`, `feed_packet`, `FlitSink`, `make_dst`,
  `center_cfg`). Routing is XY (X before Y), so NORTH from center `(1,1)` needs `make_dst(1, 2)`
  (`router.hpp:68-69`); `make_dst(2, 2)` routes EAST, not NORTH.

- [ ] **Step 1: Write the edge-case tests**

Add to `c_model/tests/router/test_router.cpp`:

```cpp
TEST(RouterWormhole, LockedOutputIsLocalAndOtherOutputProceeds) {
    SCENARIO(
        "A 3-flit packet locks EAST. An independent NORTH packet still drains "
        "while EAST is busy. The EAST packet itself stays contiguous.");
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east, north;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto N = static_cast<std::size_t>(RouterPort::NORTH);
    r.set_downstream(E, east);
    r.set_downstream(N, north);
    Packet a{static_cast<std::size_t>(RouterPort::WEST), 0x10};   // -> EAST
    Packet b{static_cast<std::size_t>(RouterPort::SOUTH), 0x20};  // -> NORTH
    for (int t = 0; t < 24; ++t) {
        feed_packet(r, a, make_dst(3, 1), /*vc=*/0);  // EAST
        feed_packet(r, b, make_dst(1, 2), /*vc=*/0);  // NORTH
        const std::size_t be = east.received.size(), bn = north.received.size();
        r.tick();
        for (std::size_t i = be; i < east.received.size(); ++i) r.receive_credit(E, 0);
        for (std::size_t i = bn; i < north.received.size(); ++i) r.receive_credit(N, 0);
    }
    EXPECT_EQ(east.received.size(), 3u);
    EXPECT_EQ(north.received.size(), 3u);  // other output not blocked by EAST's lock
}

TEST(RouterWormhole, OpenPacketHoldsOutputAndBlocksOtherVc) {
    SCENARIO(
        "An EAST packet that sends its head (last=0) but never its tail keeps the "
        "EAST output locked: a full packet on EAST's other VC cannot drain. This "
        "is the per-output hold (and the malformed-no-final-last lifetime).");
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    Packet head_only{static_cast<std::size_t>(RouterPort::WEST), 0x10};
    Packet full{static_cast<std::size_t>(RouterPort::SOUTH), 0x20};
    feed_packet(r, head_only, dst, /*vc=*/0);  // one call = head, last=0, never closed
    for (int t = 0; t < 24; ++t) {
        feed_packet(r, full, dst, /*vc=*/1);  // a complete 3-flit packet on vc1
        const std::size_t before = east.received.size();
        r.tick();
        for (std::size_t i = before; i < east.received.size(); ++i)
            r.receive_credit(E, static_cast<uint8_t>(east.received[i].get_header_field("vc_id")));
    }
    ASSERT_EQ(east.received.size(), 1u);  // only the open packet's head
    EXPECT_EQ(static_cast<uint8_t>(east.received[0].get_header_field("src_id")), 0x10);
    // The vc1 packet is blocked behind the unclosed vc0 lock (no leak across VCs).
}
```

- [ ] **Step 2: Run the new tests, verify they pass**

Run: `make build-cmodel PYTHON3=python3 && ./build/cmodel/tests/router/test_router.exe --gtest_filter='RouterWormhole.*'`
Expected: PASS (including the Task 1 cross-VC test). If a hold/independence test fails, fix the Task 1 `tick()` logic, do not weaken the test.

- [ ] **Step 3: Commit**

```bash
git add c_model/tests/router/test_router.cpp
git commit -m "test(router): wormhole hold, mid-packet stall, output independence"
```

---

### Task 3: Reconcile existing router tests with per-output semantics

**Files:**
- Modify: `c_model/tests/router/test_router.cpp` (only tests that asserted cross-VC interleave)

**Interfaces:**
- Consumes: Task 1 semantics.

- [ ] **Step 1: Find tests that assert the old behavior**

Run: `./build/cmodel/tests/router/test_router.exe`
For each FAIL, read the test. A test fails legitimately only if it asserted that two packets to one output interleave across VCs (the removed behavior). Confirm by reading the SCENARIO text and assertions.

- [ ] **Step 2: Update each failing test to the per-output contract**

For a test that expected interleave, rewrite its assertions to expect contiguous per-packet draining (one packet fully, then the next), keeping the same stimulus. Do not delete the test and do not loosen unrelated assertions. If a test's intent no longer maps to any real behavior, replace it with the nearest meaningful per-output assertion and update its SCENARIO text.

- [ ] **Step 3: Run the full suite, verify green**

Run: `make test PYTHON3=python3`
Expected: `100% tests passed` (skips limited to pre-existing NumVc1 / INF-001). Record the N/N line.

- [ ] **Step 4: Commit**

```bash
git add c_model/tests/router/test_router.cpp
git commit -m "test(router): update existing cases to per-output wormhole semantics"
```

---

### Task 4: Family C guard (read-fill slot-overwrite detection)

**Why not guard `push_ar`:** existing tests legitimately call `push_ar` with `len>0` in Enabled mode
and expect success (they inject distinct per-beat `rob_idx`, e.g.
`Enabled_PushAr_AllocatesConsecutiveSlotsForBurst` at `test_rob.cpp:225-246`,
`Enabled_PopR_MultiBeatBurstCommitInOrder` at `:416-430`). Allocating a consecutive range is correct.
The hazard is the NSU stamping every R beat with the same base `rob_idx`, so the guard belongs at the
read fill, where the second beat lands on an already-filled slot.

**Files:**
- Modify: `c_model/include/nmu/rob.hpp` (`pop_r_staged` read fill, around `:355-361`)
- Test: `c_model/tests/nmu/test_rob.cpp`

**Interfaces:**
- Consumes: `Rob::pop_r_staged`, the read-slot fill (`read_entries_[meta.rob_idx]`, fields `occupied`,
  `ready`, `r_beat`). R beats reach the ROB through the file's local `ChannelModel`/`Depacketize`
  setup driven by `noc.rsp_out().push_flit` (the pattern in `Enabled_PopR_MultiBeatBurstCommitInOrder`,
  `test_rob.cpp:420-447`). Death tests live in the existing `NmuRobDeath` suite (`:519`, `:538`).
- Produces: fail-fast when two R beats target the same already-filled read slot.

- [ ] **Step 1: Confirm allocation clears `ready`**

Read `Rob::push_ar` Enabled allocation (`rob.hpp:232-249`). Confirm a freshly allocated read slot has
`ready == false` (so the guard cannot false-fire on a clean allocation). If allocation does not clear
`ready`, add that reset as part of this task.

- [ ] **Step 2: Write the failing death test**

Model it on `Enabled_PopR_MultiBeatBurstCommitInOrder` (`test_rob.cpp:416-430`), but instead of
injecting distinct per-beat `rob_idx` (0,1,2,...), inject TWO R beats with the SAME base `rob_idx`
(what the real NSU does), then drive `pop_r_staged` twice.

```cpp
TEST(NmuRobDeath, ReadFillSameBaseRobIdxOverwriteGuarded) {
    SCENARIO(
        "Enabled read ROB: two R beats stamped with the same base rob_idx (as NSU "
        "currently stamps every beat of a burst) collide on one slot. The model "
        "must fail fast (Family C: per-ID arrival offset not implemented), not "
        "silently overwrite and misorder the burst.");
    // Reuse this file's Enabled-read fixture: build an Enabled Rob, allocate a
    // multi-beat read via push_ar(len>=1), then enqueue two R beats into the
    // injection source BOTH with rob_idx = base. (Copy the exact construction
    // from Enabled_PopR_MultiBeatBurstCommitInOrder; change only the rob_idx of
    // the second injected beat to equal the first.)
    EXPECT_DEATH({ /* second pop_r_staged drives the colliding fill */ }, ".*");
}
```

Replace the placeholder with the concrete fixture copied from `Enabled_PopR_MultiBeatBurstCommitInOrder`.

- [ ] **Step 3: Run the test, verify it fails**

Run: `make build-cmodel PYTHON3=python3 && ./build/cmodel/tests/nmu/test_rob.exe --gtest_filter='*ReadFillSameBaseRobIdxOverwriteGuarded*'`
Expected: FAIL (no abort yet; the second beat silently overwrites slot `base`).

- [ ] **Step 4: Add the guard in `pop_r_staged`**

In `c_model/include/nmu/rob.hpp`, in `pop_r_staged` after the `if (!slot.occupied)` check and before
`slot.r_beat = r;` (`:355-360`), add:

```cpp
    if (slot.ready) {
        assert(false &&
               "nmu::Rob::pop_r_staged: a second R beat targets an already-filled read slot "
               "(Family C). NSU stamps every R beat of a burst with the same base rob_idx, and "
               "the per-ID arrival offset (floo_rob.sv base+offset_q[id]) is not implemented, so "
               "multi-beat reads would overwrite slot base. Use ReadWriteSplit / single-beat "
               "reads, or implement the per-ID read offset first.");
        std::abort();
    }
```

- [ ] **Step 5: Run the test, verify it passes**

Run: `clang-format -i c_model/include/nmu/rob.hpp && make build-cmodel PYTHON3=python3 && ./build/cmodel/tests/nmu/test_rob.exe --gtest_filter='*ReadFillSameBaseRobIdxOverwriteGuarded*'`
Expected: PASS.

- [ ] **Step 6: Run the NMU suite, verify existing Enabled tests still pass**

Run: `./build/cmodel/tests/nmu/test_rob.exe`
Expected: PASS, including `Enabled_PushAr_AllocatesConsecutiveSlotsForBurst` and
`Enabled_PopR_MultiBeatBurstCommitInOrder` (they inject distinct `rob_idx`, so no slot collides).

- [ ] **Step 7: Add the documenting test for the same-base-rob_idx hazard**

In `c_model/tests/nsu/test_nsu_packetize.cpp` (multi-beat R coverage exists around `:136-157`), add a
test that builds a multi-beat R burst via NSU packetize and asserts every R flit carries the same
`rob_idx` header value. This records, at the source, the stamping that the Task 4 guard catches.

- [ ] **Step 8: Run the NMU + NSU suites, verify green**

Run: `make test PYTHON3=python3`
Expected: full suite PASS.

- [ ] **Step 9: Commit**

```bash
git add c_model/include/nmu/rob.hpp c_model/tests/nmu/test_rob.cpp c_model/tests/nsu/test_nsu_packetize.cpp
git commit -m "feat(rob): guard same-base-rob_idx read fill (Family C deferred)"
```

---

### Task 5: Co-sim verification (Family B fixed at integration)

**Files:** none (verification only).

- [ ] **Step 1: Reproduce the three previously failing runs, now passing**

Run, each with a clean rebuild:

```bash
for VC in vc2 vc4 vc8; do
  rm -rf build/verilator/obj_dir_mesh_4x4_$VC build/verilator/output_$VC
  make sim TB=mesh_4x4_$VC PATTERN=hotspot HOTSPOT=0 \
    BASE=sim/test_patterns/AX4-BUR-002_incr_8beat/scenario.yaml PYTHON3=python3
done
```

Expected: each prints `scoreboard ... 0 mismatches` and `correctness gate: PASS`, exit 0.

- [ ] **Step 2: Re-run the single-beat and multibeat matrices, confirm no regression**

Run the 16-run (4 patterns x vc1/2/4/8, default base) and the 48-run multibeat matrices (the scripts used during Family A verification). Expected: 16/16 PASS and 48/48 PASS (the three BUR-002 hotspot cases now pass).

- [ ] **Step 3: Record results in `progress.md`**

Append the exit codes and pass counts to `progress.md` (hard-signal only).

- [ ] **Step 4: Final ctest confirmation**

Run: `make test PYTHON3=python3`
Expected: `100% tests passed`. Record the N/N line.

---

## Self-Review

**Spec coverage:**
- Design B per-output lock → Task 1. Stall/isolation/malformed behavior → Task 2. Existing-test reconciliation → Task 3.
- Design C guard → Task 4. Co-sim + matrices → Task 5.

**Open follow-ups (not in this plan, recorded in spec Out of scope):** full Family C per-ID read offset; latency re-baseline of multi-VC benchmarks.

**Type consistency:** `WormholeState{locked_input, locked_vc, rr}` defined in Task 1 and used only there. `Rob::pop_r_staged` read-fill guard matches `rob.hpp:355-361`. Test helpers used are the existing `Packet`, `feed_packet`, `FlitSink`, `make_dst`, `center_cfg` (no new sink helper). Death tests use the existing `NmuRobDeath` suite and the file's local `ChannelModel`/`Depacketize` setup with `noc.rsp_out().push_flit`.
