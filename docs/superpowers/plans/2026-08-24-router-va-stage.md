# Router VA stage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The DAT `router::Router` allocates the output VC in its own stage: head 4 cycles per router, body and tail 3, single flit 4. `SimpleRouter` unchanged.

**Architecture:** Per input VC state {idle, active} with route and output VC. A tick runs LT, SA+ST, VA, fork pop, BW in that order (reverse pipeline, as today). VA runs on the FIFO front head of every idle input VC and locks `wormhole_[out][out_vc]`. SA serves held VCs only. Details and every ruling: the spec below.

**Tech Stack:** C++17 header only, GoogleTest, Verilator co-sim on WSL (`BUILD_ROOT=$HOME/noc_build`).

**Spec:** `docs/superpowers/specs/2026-08-24-router-va-stage.md` (binding). Current as-built text to supersede: `docs/router-spec.md` 2.4 to 2.7, 2.9, R10, SPEC 4, 6, 10 to 16.

## Global Constraints

- Branch `feat/router-va-stage`. Commits `type(scope): description`, body ends with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`. `clang-format -i` on every edited `.hpp`/`.cpp`.
- `simple_router.hpp` and its tests are untouched. NI code, `dat_merge_wrap.hpp`, DPI and SV wraps untouched.
- No new specgen parameter. No behaviour flag: the VA stage is the design, not an option.
- Credit accounting is unchanged: decrement at SA grant, pulse on input FIFO pop, `LinkCreditOut` unchanged.
- Build and test on WSL only, foreground, explicit Bash timeout up to 600000 ms: `wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && BUILD_ROOT=$HOME/noc_build make build-cmodel'`, ctest from the build dir the Makefile names. Python on /mnt/e may crash transiently, retry once. `VERILATOR_EXTRA_FLAGS="-CFLAGS -g0"` if a Verilator build hits the gcc 15 ICE.
- `IMPLEMENTATION_PLAN.md` in the tree belongs to the RTL campaign. Do not edit it.
- A subagent that hits a spec deviation or a failing gate reports BLOCKED with verbatim output. No inline workarounds, no weakened tests.

---

## Stage 1: Router
Goal: `router.hpp` implements the four stage pipeline per the spec, router tests updated and extended.
Success Criteria: new and rewritten `RouterDatapath` / `RouterVa` tests pass, full ctest green.
Status: Not Started

### Task 1: VA stage in `router::Router`

**Files:**
- Modify: `ref_model/c_model/include/router/router.hpp` (`WormholeState` ~:420, per input VC state, `vc_assignment` ~:321, `tick()` ~:470-708)
- Modify: `ref_model/c_model/tests/router/test_router.cpp` (`kPipelineDepth` :21, `ZeroLoadLatencyIsThreeTicks` :187, `RouterVaWorkConserving` :1255, `RouterVaFvada`, hard coded tick counts), `test_router_fork.cpp`, `tests/wrap/test_ni_router_chain.cpp` (tick counts only)
- Modify: `ref_model/dpi/cmodel_dpi.cpp` fabric dump (~:864) if the introspection accessors change shape

**Interfaces:**
- Produces: per input VC introspection `std::optional<uint8_t> va_out_vc(std::size_t in, uint8_t vc) const` (nullopt while idle). `wormhole_locked_input/_input_vc/_output_vc(out, vc)` unchanged. `credit()`, `input_fifo_size()`, `output_fifo_size()`, `fork_expected_mask()`, `fork_done_mask()` unchanged.

- [ ] **Step 1: Read** the spec in full, then `router.hpp` in full, then the tests listed. Do not start editing before both reads are done.

- [ ] **Step 2: Write the failing tests** in `test_router.cpp` (helpers `center_cfg`, `make_dst`, `make_flit`, `make_pinned_flit`, `make_tagged_flit`, `FlitSink`, `feed_packet` exist there). Set `kPipelineDepth = 4` and add `constexpr int kBodyDepth = 3;`.

```cpp
TEST(RouterDatapath, ZeroLoadLatencyIsFourTicks) {
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    r.input(W).push_flit(make_flit(make_dst(3, 1), 0, /*flit_tail=*/1));
    for (int t = 0; t < 3; ++t) {
        r.tick();
        EXPECT_TRUE(east.received.empty()) << "tick " << t;
    }
    r.tick();
    ASSERT_EQ(east.received.size(), 1u);
}

// Head pays VA, body and tail do not: a 3 flit worm delivers at ticks 4, 5, 6.
TEST(RouterDatapath, BodyFlitsFollowHeadOneCycleApart) {
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    std::vector<std::size_t> arrivals;
    for (int t = 1; t <= 8; ++t) {
        if (t <= 3) r.input(W).push_flit(make_flit(dst, 0, t == 3 ? 1 : 0));
        const std::size_t before = east.received.size();
        r.tick();
        if (east.received.size() > before) arrivals.push_back(static_cast<std::size_t>(t));
    }
    EXPECT_EQ(arrivals, (std::vector<std::size_t>{4, 5, 6}));
}

// A tail frees its VC in SA and the next head takes it in VA of the same
// cycle: eight single flit packets on one input VC arrive one per cycle.
TEST(RouterDatapath, BackToBackSingleFlitsOnePerCycle) {
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    std::vector<std::size_t> arrivals;
    for (int t = 1; t <= 14; ++t) {
        if (t <= 8) r.input(W).push_flit(make_flit(dst, 0, 1));
        const std::size_t before = east.received.size();
        r.tick();
        for (std::size_t i = before; i < east.received.size(); ++i) {
            r.receive_credit(E, static_cast<uint8_t>(east.received[i].get_header_field("vc_id")));
            arrivals.push_back(static_cast<std::size_t>(t));
        }
    }
    ASSERT_EQ(arrivals.size(), 8u);
    for (std::size_t i = 1; i < arrivals.size(); ++i) EXPECT_EQ(arrivals[i], arrivals[i - 1] + 1);
}

// VA needs a free AND credited VC. Preferred VC held by another worm: a
// single flit takes the highest index other free credited VC; a worm head
// stays idle and takes nothing.
TEST(RouterVa, HeldPreferredVcSingleFlitOverflowsWormHeadWaits) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    const auto S = static_cast<std::size_t>(RouterPort::SOUTH);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(2, 3);  // preferred VC 0 on EAST (next hop NORTH)
    r.input(S).push_flit(make_tagged_flit(dst, 0, /*flit_tail=*/0, 0x20));  // open worm holds vc0
    r.tick();
    r.tick();
    ASSERT_EQ(r.wormhole_locked_input(E, 0), std::optional<std::size_t>(S));
    r.input(W).push_flit(make_tagged_flit(dst, 0, 1, 0x10));  // single flit
    r.tick();
    EXPECT_EQ(r.va_out_vc(W, 0), std::optional<uint8_t>(1));  // overflowed to vc1
    for (int t = 0; t < 4; ++t) r.tick();
    r.input(W).push_flit(make_tagged_flit(dst, 0, 0, 0x30));  // worm head
    for (int t = 0; t < 4; ++t) r.tick();
    EXPECT_FALSE(r.va_out_vc(W, 0).has_value());  // still idle
    EXPECT_FALSE(r.wormhole_locked_input(E, 1).has_value());
}

// A free VC with credit 0 is never held: the head stays idle until credit
// returns, then allocates and departs.
TEST(RouterVa, ZeroCreditVcIsNeverHeld) {
    Router r(center_cfg());  // num_vc 1
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    for (std::size_t i = 0; i < NOC_ROUTER_VC_DEPTH; ++i) {
        r.input(W).push_flit(make_flit(dst, 0, 1));
        r.tick();
    }
    for (int t = 0; t < 8; ++t) r.tick();  // credit_[E][0] is now 0, no returns
    r.input(W).push_flit(make_flit(dst, 0, 1));
    for (int t = 0; t < 4; ++t) r.tick();
    EXPECT_FALSE(r.va_out_vc(W, 0).has_value());
    EXPECT_FALSE(r.wormhole_locked_input(E, 0).has_value());
    r.receive_credit(E, 0);
    for (int t = 0; t < 4; ++t) r.tick();
    EXPECT_EQ(east.received.size(), NOC_ROUTER_VC_DEPTH + 1);
}
```

Rewrite `RouterVaWorkConserving.HeadVaFailAlternateCandidateGrantedSameTick` so the VA failure is a held preferred VC (an open worm from another input on that VC) rather than a credit refusal, keeping its assertion that the other candidate is granted the same tick. Rename `ZeroLoadLatencyIsThreeTicks` away (replace it with the new test). In `test_router_fork.cpp` add:

```cpp
// Fork branches lock at VA independently: with NORTH's preferred VC held by
// another worm, the EAST branch locks and waits, NORTH locks once freed, and
// the head pops only after both branches granted it.
TEST(RouterFork, BranchesLockAtVaIndependently) { /* build with this file's make_mc_flit / feed_worm helpers, two branch mask NORTH|EAST from WEST, num_vc 2, hold NORTH vc0 with an open pinned worm from SOUTH first; assert wormhole_locked_input(E, 0) == W while wormhole_locked_input(N, 0) == S and input_fifo_size(W, 0) == 1; close the SOUTH worm, tick, assert wormhole_locked_input(N, 0) == W; drain and assert both sinks received the head. */ }
```

Write the body in that file's style.

- [ ] **Step 3: Run** `ctest -R 'RouterDatapath|RouterVa|RouterFork'`. Expected: the new tests fail (3 tick delivery, no `va_out_vc`).

- [ ] **Step 4: Implement.** State:

```cpp
// Per input VC pipeline state (textbook G, R, O). idle: the FIFO front, if a
// head, is a VA candidate. active: route and out_vc are held from the VA grant
// until the tail passes SA.
struct InputVcState {
    bool active = false;
    PortMask route = 0;                       // branch set (one bit for unicast)
    std::array<std::optional<uint8_t>, ROUTER_PORT_COUNT> out_vc{};  // per branch
};
std::array<std::vector<InputVcState>, ROUTER_PORT_COUNT> ivc_{};  // [in][vc]
```

`WormholeState` keeps `locked_input`, `locked_input_vc`, `locked_output_vc`. `vc_assignment(out, f)` becomes the VA rule: `fixed_vc = 1` → that VC if `wormhole_[out][vc]` is free and `credit_[out][vc] > 0`, else nullopt; `fixed_vc = 0` → preferred if free and credited, else if `flit_tail = 1` the highest index other free credited VC (scan upward, overwrite), else nullopt.

`tick()` order:
1. credit pulses out (unchanged).
2. Stage 4 LT: output FIFO front to link (unchanged, was stage 3).
3. Stage 3 SA+ST per output: skip if output FIFO full. Round robin from `vc_rr_[out]` over `v`: slot `wormhole_[out][v]` held by `(in, ivc)`, `input_fifo_[in][ivc]` non empty, `credit_[out][v] > 0`; for a fork, skip if `fork_done_[in][ivc]` has this output. Keep the existing continuation checks (collective F9 branch set, unicast route_compute equals out, `fixed_vc = 0` locked VC equals recomputed preferred). Grant: unicast pops now, fork sets `fork_done_`; decrement credit, stamp `vc_id = v`, push output FIFO, unicast schedules the pulse; `vc_rr_[out] = v + 1`. If `flit_tail = 1`: clear `wormhole_[out][v]`, clear `ivc_[in][ivc].out_vc[out]`; when no branch of `ivc_[in][ivc]` holds a VC any more set `active = false`; advance `rr_[out]`, `in_vc_rr_[out]`? No: those two advance at VA. Remove the unlocked slot scan and the tail steal guard.
4. Fork pop pass (unchanged).
5. Stage 2 RC+VA per output: at most one VA grant per output per cycle. Scan input VCs in `in_vc_rr_[out]` major, `rr_[out]` minor order; candidate = FIFO front of an input VC whose `out_vc[out]` is empty and whose front flit's `head_expected_mask` contains `out` (unicast: `route_compute`; fork: the mask) and, for a unicast, whose state is idle (a unicast continuation never reaches VA because `active` covers it); apply `vc_assignment(out, front)`; on success set `ivc_[in][ivc].route |= bit(out)`, `out_vc[out] = v`, `active = true`, `wormhole_[out][v] = {in, ivc, v}`, advance `in_vc_rr_[out]`, `rr_[out]`; on failure leave the pointers. Keep the collective guard from the old unlocked scan (a continuation reaching VA is a corrupted branch set: abort) and the empty fork set abort.
6. Stage 1 BW (unchanged).

`locked_branch_set` unchanged (it reads `wormhole_`). Fabric dump in `cmodel_dpi.cpp`: unchanged unless a call signature changed.

- [ ] **Step 5: Run** `ctest -R Router` then the full ctest. Fix tick counts in the fork and chain tests (+1 per router for a head) where they fail; never loosen an assertion beyond the +1. Expected all green.

- [ ] **Step 6: Commit** `feat(router): VC allocation as its own stage, head 4 cycles, body 3`

---

## Stage 2: Docs
Goal: specs describe the four stage router.
Success Criteria: every section in the spec's Docs list updated, grep for the old numbers clean.
Status: Not Started

### Task 2: router-spec and target spec

- [ ] Edit `docs/router-spec.md`: 2.4 (stage table gains a VA row, per router head 4 body 3, SA before VA within a cycle, one pop per input VC per cycle, delete the multi pop worked example), 2.5 (VA a cycle before SA, VA rule single flit overflow only, pointer stages), 2.6 (lock at VA, fork per branch at VA), 2.7 (credit round trip one cycle longer), 2.9 worked example redone (head cycle 0 in, VA cycle 1, SA cycle 2, link cycle 3, at B cycle 4), R10 (head 4, body 3), SPEC 4 (DAT head 4 body 3 at the wrapper pins), SPEC 6 (VA rule, cited test kept), SPEC 10 to 14 wording to VA/SA, SPEC 15 withdrawn with one line saying so, SPEC 16 pointer rule. Add one line in 2.4: `dat_merge_wrap` TX 1 cycle is the DPI wrap output register, target RTL folds the merge into the NI with no TX cycle, RX 1 cycle matches the FlooNoC chimney spill register.
- [ ] `docs/noc-target-spec.md` 7.4 as built line: DAT head 4, body 3 per router.
- [ ] `docs/known-limitations.md`: remove the multi pop row.
- [ ] `grep -rn 'exactly 3 cycles\|3 cycles on DAT\|several pops\|multi-grant' docs/` returns only SimpleRouter statements.
- [ ] Commit `docs(router): four stage DAT router, VA a cycle before SA`

---

## Stage 3: Gates and measurement
Goal: co-sim green, zero-load numbers re-measured.
Success Criteria: gates pass, Scenario 2 zero load data read 41 write 42, narrow unchanged.
Status: Not Started

### Task 3: co-sim and Scenario 2

- [ ] `make build-verilator`. Directed `make sim CONFIG=mesh_4x4 PATTERN=uniform_random INJECTION_MODE=0 BURST_LEN=32` and `PATTERN=bit_complement`. Continuous vc2 `PATTERN=uniform_random INJECTION_MODE=1 INJECTION_RATE=0.9 BURST_LEN=32` (sim-gen then sim). vc8: `DAT_NUM_VC` 8 in `specgen/source/constants.yaml`, rebuild, same run, restore, rebuild, `codegen.py --check` clean.
- [ ] Scenario 2 zero load per `docs/backlog.md` recipe "Zero load (mesh_4x4, rate 0.005)", tags `s2zl_va_narrow` / `s2zl_va_data`. Expected narrow 31.0 / 32.0, data 41.0 / 42.0. Record the actual numbers; a mismatch is a finding, not a tuning target.
- [ ] Update the Zero load subsection of `sim/verilator/output/sweep_summary.md` (table and the per stage table: response routers on DAT 4 cycles each for the single flit R, 16) and the scratch `scenario2.md`. Append the runs to the backlog recipe.
- [ ] `docs/backlog.md` "Last round": one paragraph, the VA stage landed, measured numbers, next candidates (low load bypass, speculative VA).
- [ ] Commit `docs(backlog): router VA stage measurement`
