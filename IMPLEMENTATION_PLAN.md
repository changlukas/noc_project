# Per-(output, VC) wormhole lock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** DAT_NUM_VC becomes a real performance parameter: worms on different VCs share one physical link flit by flit, and the NSU reassembles each VC's W stream independently.

**Architecture:** Router `wormhole_` goes from one lock per output to one lock per (output, VC), with a per-cycle round-robin across the VCs that can send (FlooNoC `floo_wormhole_arbiter` per `[out][vc]` plus `floo_vc_arbiter` with `LockIn = 0`). The NSU DAT ingress becomes per-VC bounded queues of depth `NOC_ROUTER_VC_DEPTH` that return credit on consumption (FlooNoC chimney: VC demux before the spill registers, credit per VC). `DatMergeWrap` forwards the NSU's credit pulses instead of returning credit at demux time. NMU write VC becomes `(dst_id ^ id) % num_vc`, the rule the NSU read side already uses, so same-ID writes to one destination stay on one VC and keep issue order (Task 1.4). REQ/RSP `SimpleRouter` is untouched.

**Tech Stack:** C++17 header-only c_model, GoogleTest (ctest), Verilator co-sim via DPI (`ref_model/dpi/cmodel_dpi.{h,cpp}`, `ref_model/top/*.sv`). Build and run on WSL with `BUILD_ROOT=$HOME/noc_build`.

**Spec:** survey in this session (FlooNoC `E:/05_NoC/FlooNoC/hw/floo_router.sv:116-146,420-491`, `floo_wormhole_arbiter.sv:29-77`, `floo_vc_arbiter.sv:108-125`, `floo_nw_chimney.sv:276-311`). Docs to update in Stage 4.

## Global Constraints

- Branch `feat/per-vc-wormhole-lock` off `main`. One commit per task, `type(scope): description`.
- `clang-format -i` every edited `.hpp` / `.cpp`.
- Per-VC NSU ingress depth is `NOC_ROUTER_VC_DEPTH` (`ni_params.h`), the same constant the router seeds `credit_[LOCAL][vc]` with. No new specgen parameter.
- A worm never changes VC mid-flight (existing: NMU `current_aw_vc_`, router head-only VA). Within one VC, flits of one worm stay contiguous. Across VCs, flits may interleave on a link.
- At most one grant per output per tick (one flit per link per cycle, router-spec R11).
- `simple_router.hpp` and its tests are out of scope.
- Verification budget: full ctest once per stage. Directed co-sim `make sim CONFIG=mesh_4x4 PATTERN=uniform_random INJECTION_MODE=0 BURST_LEN=32` once at the end of Stage 1 and once in Stage 3.
- A subagent that hits a plan deviation or a failing gate reports BLOCKED with the verbatim output. No inline workarounds.

---

## Stage 1: Router lock per (output, VC)
Goal: `router::Router` holds `num_vc` wormhole locks per output and round-robins across VCs every cycle.
Success Criteria: new `RouterWormhole` tests pass, all router and fork tests pass, directed co-sim result recorded.
Status: Not Started

### Task 1.1: Lock state per (output, VC)

**Files:**
- Modify: `ref_model/c_model/include/router/router.hpp:196-204` (introspection), `:398-417` (`locked_branch_set`, `WormholeState`), `:430-431` (`wormhole_`, `vc_rr_`), `:151-156` (ctor sizing)
- Modify: `ref_model/c_model/tests/router/test_router_fork.cpp:159,218-219,236-237,392-395,596-597,646-647` (add VC argument)
- Modify: `ref_model/dpi/cmodel_dpi.cpp:835,864` (fabric state dump: loop over VCs, print the lock per (output, VC))

**Interfaces:**
- Produces: `std::optional<std::size_t> wormhole_locked_input(std::size_t out_port, uint8_t vc) const`, same shape for `wormhole_locked_input_vc` and `wormhole_locked_output_vc`. `wormhole_[out][vc]` is a `std::vector<WormholeState>` sized `cfg_.num_vc`. `rr_[out]` replaces `WormholeState::rr`.

- [ ] **Step 1: Change the state**

```cpp
struct WormholeState {
    std::optional<std::size_t> locked_input;
    std::optional<uint8_t> locked_input_vc;
    std::optional<uint8_t> locked_output_vc;  // always == the slot's own VC index
};
// [out][out_vc]: one lock per (output, VC). FlooNoC floo_wormhole_arbiter is
// instantiated per [out][vc] (floo_router.sv:420-446); a worm holds its VC's
// lock from head to tail, other VCs of the same output keep granting.
std::array<std::vector<WormholeState>, ROUTER_PORT_COUNT> wormhole_{};
std::array<std::size_t, ROUTER_PORT_COUNT> rr_{};        // input round-robin, per output
std::array<std::size_t, ROUTER_PORT_COUNT> in_vc_rr_{};  // input-VC round-robin, per output
std::array<std::size_t, ROUTER_PORT_COUNT> vc_rr_{};     // output-VC round-robin, per output
```

In the ctor loop: `wormhole_[p].assign(cfg_.num_vc, WormholeState{});`.

- [ ] **Step 2: Introspection and `locked_branch_set`**

```cpp
std::optional<std::size_t> wormhole_locked_input(std::size_t out_port, uint8_t vc) const {
    return wormhole_[out_port][vc].locked_input;
}
// same shape for wormhole_locked_input_vc / wormhole_locked_output_vc

PortMask locked_branch_set(std::size_t in, uint8_t vc) const {
    PortMask m = 0;
    for (std::size_t o = 0; o < ROUTER_PORT_COUNT; ++o)
        for (const auto& ws : wormhole_[o])
            if (ws.locked_input == in && ws.locked_input_vc == vc)
                m = static_cast<PortMask>(m | (1u << o));
    return m;
}
```

- [ ] **Step 3: Update fork test call sites.** Pass `0` as the VC where those tests run `num_vc = 1` (`:159,218-219,236-237,596-597,646-647`). At `:392-395` the EAST branch is expected on VC 1 and NORTH on VC 0, so the checks become `r.wormhole_locked_output_vc(E, 1).has_value()` / `r.wormhole_locked_output_vc(N, 0).has_value()` and the `EXPECT_EQ` values stay `1u` / `0u`.

- [ ] **Step 4: Build** `make build-cmodel`. It will not compile until Task 1.2 rewrites `tick()`; do Task 1.1 and 1.2 in one commit if the intermediate state is not worth keeping.

### Task 1.2: Grant loop round-robins across VCs

**Files:**
- Modify: `ref_model/c_model/include/router/router.hpp:473-628` (the per-output block of `tick()`)
- Test: `ref_model/c_model/tests/router/test_router.cpp:331-358` and `:505-526`

**Interfaces:**
- Consumes: `wormhole_[out][vc]`, `rr_[out]`, `vc_rr_[out]`, `vc_assignment(out, flit)`.

- [ ] **Step 1: Write the failing tests.** Replace `PacketsOnDifferentVcsDoNotInterleavePerOutput` (`:331-358`) and `OpenPacketHoldsOutputAndBlocksOtherVc` (`:505-526`) with these three (`make_pinned_flit` sets `fixed_vc = 1`, which is how NMU AW/W worms arrive; a `fixed_vc = 0` head takes the preferred VC of its next hop, so two such worms to the same next hop serialize on one VC by design):

```cpp
// Two pinned 3-flit worms from different inputs, different VCs, same output:
// both hold a lock at once and the link carries their flits interleaved.
// Each VC's own stream stays contiguous.
TEST(RouterWormhole, WormsOnDifferentVcsInterleavePerOutput) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    const auto S = static_cast<std::size_t>(RouterPort::SOUTH);
    int next_a = 0, next_b = 0;
    for (int t = 0; t < 24; ++t) {
        if (next_a < 3) {
            r.input(W).push_flit(make_pinned_flit(dst, 0, next_a == 2 ? 1 : 0, 0x10));
            ++next_a;
        }
        if (next_b < 3) {
            r.input(S).push_flit(make_pinned_flit(dst, 1, next_b == 2 ? 1 : 0, 0x20));
            ++next_b;
        }
        const std::size_t before = east.received.size();
        r.tick();
        for (std::size_t i = before; i < east.received.size(); ++i)
            r.receive_credit(E, static_cast<uint8_t>(east.received[i].get_header_field("vc_id")));
    }
    ASSERT_EQ(east.received.size(), 6u);
    int runs = 1;
    for (std::size_t i = 1; i < east.received.size(); ++i) {
        if (east.received[i].get_header_field("src_id") !=
            east.received[i - 1].get_header_field("src_id"))
            ++runs;
    }
    EXPECT_GT(runs, 2) << "worms on different VCs serialized on EAST";
    std::vector<uint64_t> tails0, tails1;
    for (const auto& f : east.received) {
        const auto vc = f.get_header_field("vc_id");
        EXPECT_EQ(f.get_header_field("src_id"), vc == 0 ? 0x10u : 0x20u);
        (vc == 0 ? tails0 : tails1).push_back(f.get_header_field("flit_tail"));
    }
    EXPECT_EQ(tails0, (std::vector<uint64_t>{0, 0, 1}));
    EXPECT_EQ(tails1, (std::vector<uint64_t>{0, 0, 1}));
}

// An unclosed worm on vc0 holds only vc0's lock: a complete worm on vc1 is
// delivered in full.
TEST(RouterWormhole, OpenWormOnVc0DoesNotBlockVc1) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    const auto S = static_cast<std::size_t>(RouterPort::SOUTH);
    r.input(W).push_flit(make_pinned_flit(dst, 0, /*flit_tail=*/0, 0x10));  // head, never closed
    int next_b = 0;
    for (int t = 0; t < 24; ++t) {
        if (next_b < 3) {
            r.input(S).push_flit(make_pinned_flit(dst, 1, next_b == 2 ? 1 : 0, 0x20));
            ++next_b;
        }
        const std::size_t before = east.received.size();
        r.tick();
        for (std::size_t i = before; i < east.received.size(); ++i)
            r.receive_credit(E, static_cast<uint8_t>(east.received[i].get_header_field("vc_id")));
    }
    ASSERT_EQ(east.received.size(), 4u);
    EXPECT_EQ(r.wormhole_locked_input(E, 0), std::optional<std::size_t>(W));
    EXPECT_FALSE(r.wormhole_locked_input(E, 1).has_value());
}

// A locked VC with no credit idles only itself (floo_vc_arbiter LockIn=0
// re-arbitrates every cycle among the VCs that can send).
TEST(RouterWormhole, CreditStarvedVcDoesNotIdleTheOutput) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    const auto S = static_cast<std::size_t>(RouterPort::SOUTH);
    // vc0: an open worm fed one body flit per tick; its credit is never returned,
    // so after NOC_ROUTER_VC_DEPTH grants vc0 is starved while still locked.
    int next_b = 0;
    for (int t = 0; t < 40; ++t) {
        if (r.input_fifo_size(W, 0) == 0) r.input(W).push_flit(make_pinned_flit(dst, 0, 0, 0x10));
        if (t >= 10 && next_b < 3) {
            r.input(S).push_flit(make_pinned_flit(dst, 1, next_b == 2 ? 1 : 0, 0x20));
            ++next_b;
        }
        const std::size_t before = east.received.size();
        r.tick();
        for (std::size_t i = before; i < east.received.size(); ++i) {
            const auto vc = static_cast<uint8_t>(east.received[i].get_header_field("vc_id"));
            if (vc == 1) r.receive_credit(E, vc);
        }
    }
    int vc0 = 0, vc1 = 0;
    for (const auto& f : east.received) (f.get_header_field("vc_id") == 0 ? vc0 : vc1)++;
    EXPECT_EQ(vc0, static_cast<int>(NOC_ROUTER_VC_DEPTH));
    EXPECT_EQ(vc1, 3);
    EXPECT_EQ(r.wormhole_locked_input(E, 0), std::optional<std::size_t>(W));
}
```

Add a fourth test for the tail-steal case:

```cpp
// A credit-blocked worm's tail must wait for its own VC's credit. A tail is
// the one flit vc_assignment lets overflow to another VC; granting it from
// another VC's slot would leave the worm's lock set forever.
TEST(RouterWormhole, CreditBlockedTailDoesNotOverflowToAnotherVc) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    // fixed_vc=0 worm: head + NOC_ROUTER_VC_DEPTH-1 bodies use up vc0's credit,
    // then the tail arrives with credit_[E][0] == 0 and credit left on vc1.
    const int bodies = static_cast<int>(NOC_ROUTER_VC_DEPTH) - 1;
    int fed = 0;
    for (int t = 0; t < 40; ++t) {
        if (r.input_fifo_size(W, 0) == 0 && fed <= bodies + 1) {
            const uint64_t tail = (fed == bodies + 1) ? 1 : 0;
            r.input(W).push_flit(make_tagged_flit(dst, 0, tail, 0x10));
            ++fed;
        }
        r.tick();  // no credit ever returned on vc0
    }
    EXPECT_EQ(east.received.size(), NOC_ROUTER_VC_DEPTH);  // tail never left
    EXPECT_TRUE(r.wormhole_locked_input(E, 0).has_value());
    EXPECT_FALSE(r.wormhole_locked_input(E, 1).has_value());
    for (const auto& f : east.received) EXPECT_EQ(f.get_header_field("vc_id"), 0u);
}
```

- [ ] **Step 2: Run** `ctest -R RouterWormhole`. Expected: `WormsOnDifferentVcsInterleavePerOutput`, `OpenWormOnVc0DoesNotBlockVc1`, `CreditStarvedVcDoesNotIdleTheOutput` FAIL (`runs == 2`; `received.size() == 1`; `vc1 == 0`). `CreditBlockedTailDoesNotOverflowToAnotherVc` passes before the change (single lock) and must still pass after.

- [ ] **Step 3: Rewrite the per-output block of `tick()`.** Shape (the locked-path body `:479-541` and the F1/F2/OUR RULE filters `:557-577` move verbatim; only the loop nesting and the lock indexing change):

```cpp
for (std::size_t out = 0; out < ROUTER_PORT_COUNT; ++out) {
    if (output_fifo_[out].size() >= cfg_.output_fifo_depth) continue;
    std::optional<std::size_t> candidate;
    uint8_t in_vc = 0;
    uint8_t out_vc = 0;
    // Output-VC round-robin, re-arbitrated every tick (floo_vc_arbiter.sv
    // LockIn=0): each VC offers either its locked worm's continuation or a
    // fresh head assigned to it. The first VC that can send wins.
    for (std::size_t kv = 0; kv < cfg_.num_vc && !candidate.has_value(); ++kv) {
        const auto v = static_cast<uint8_t>((vc_rr_[out] + kv) % cfg_.num_vc);
        auto& ws = wormhole_[out][v];
        if (ws.locked_input.has_value()) {
            // existing locked-path body with `ws` = wormhole_[out][v]; on
            // success: candidate = *ws.locked_input, in_vc = *ws.locked_input_vc,
            // out_vc = v. On empty FIFO / no credit / fork branch done: fall
            // through to the next VC.
        } else {
            for (std::size_t kiv = 0; kiv < cfg_.num_vc && !candidate.has_value(); ++kiv) {
                const auto ivc = static_cast<uint8_t>((in_vc_rr_[out] + kiv) % cfg_.num_vc);
                for (std::size_t j = 0; j < ROUTER_PORT_COUNT; ++j) {
                    const std::size_t in = (rr_[out] + j) % ROUTER_PORT_COUNT;
                    const auto& q = input_fifo_[in][ivc];
                    if (q.empty()) continue;
                    // A stream already locked somewhere is served only through
                    // its lock. Without this a credit-blocked worm's TAIL
                    // (vc_assignment lets a tail overflow to another VC,
                    // router.hpp:327-335) would be granted by an unlocked VC
                    // slot, leaving its own lock set forever.
                    if (q.front().get_header_field("collective_op") == ni::COLLECTIVE_OP_UNICAST &&
                        locked_branch_set(in, ivc) != 0)
                        continue;
                    // existing F1/F2 fork filters and OUR RULE guard (collective)
                    const auto assigned = vc_assignment(out, q.front());
                    if (!assigned.has_value() || *assigned != v) continue;
                    candidate = in;
                    in_vc = ivc;
                    out_vc = v;
                    break;
                }
            }
        }
    }
    if (!candidate.has_value()) continue;
    // existing grant body: pop or fork_done, credit consume, vc_id stamp,
    // output_fifo push, credit pulse
    auto& ws = wormhole_[out][out_vc];
    if (flit_tail == 0) {
        ws.locked_input = *candidate;
        ws.locked_input_vc = in_vc;
        ws.locked_output_vc = out_vc;
    } else {
        ws = WormholeState{};
        rr_[out] = (*candidate + 1) % ROUTER_PORT_COUNT;
        in_vc_rr_[out] = static_cast<std::size_t>((in_vc + 1) % cfg_.num_vc);
    }
    vc_rr_[out] = static_cast<std::size_t>((out_vc + 1) % cfg_.num_vc);  // every grant
}
```

- [ ] **Step 4: Run** `ctest -R 'Router'`. Expected: new tests PASS, fork tests PASS, `tests/wrap/test_ni_router_chain.cpp` PASS (NMU's source-side per-VC lock keeps one AW+W worm per NMU; two NMUs' worms land on different VCs or serialize on the same one).

- [ ] **Step 5: Full ctest** `make build-cmodel && ctest`. Expected all green.

- [ ] **Step 6: Commit** `feat(router): wormhole lock per (output, VC) with per-cycle VC round-robin`

### Task 1.3: Directed co-sim gate

- [ ] `make build-verilator`, then `make sim CONFIG=mesh_4x4 PATTERN=uniform_random INJECTION_MODE=0 BURST_LEN=32`. With `DAT_NUM_VC` 2 and NMU round-robin VC choice, two NMUs' W worms can now interleave on one link and the NSU (still single-stream until Stage 2) may assert in `pop_w` or wedge. Record the outcome verbatim under "Stage 1 findings" below, pass or fail, and continue. The gate is re-run in Stage 3.

---

### Task 1.4: NMU write VC is a function of (dst, id)

**Why:** today `nmu/vc_allocator.hpp:127-144` keeps a (dst, id) on one VC only for the length of a streak. id X to dst A (vc0), then to dst B, then to dst A again takes a fresh round-robin VC. With one lock per output the two dst-A worms still left every output in issue order. With one lock per (output, VC) the second can overtake the first whenever vc0 is credit-blocked, and same-ID writes reach the slave out of issue order (AXI4 A5.3 same-ID write ordering). NSU already maps R to `(dst_id ^ rid) % num_vc` (`nsu/vc_allocator.hpp:82`); the write side takes the same stateless rule.

**Files:**
- Modify: `ref_model/c_model/include/nmu/vc_allocator.hpp:127-144` (AW VC choice), the streak table and its comment
- Test: the NMU test file that covers the AW streak rule (`grep -rln streak ref_model/c_model/tests/nmu`)

- [ ] **Step 1: Write the failing test**

```cpp
// Same (dst, id) always rides the same VC, even with another destination's
// write in between: per-(output, VC) router locks only keep order within a VC.
TEST(NmuVcAllocator, SameDstAndIdAlwaysTakeTheSameVc) {
    // fixture as the existing AW streak tests in this file
    const uint8_t vc_first = vc_of_aw(/*dst=*/0x03, /*id=*/1);
    (void)vc_of_aw(/*dst=*/0x0A, /*id=*/1);
    EXPECT_EQ(vc_of_aw(/*dst=*/0x03, /*id=*/1), vc_first);
    EXPECT_EQ(vc_first, static_cast<uint8_t>((0x03 ^ 1) % num_vc));
}
```

`vc_of_aw` drives one AW through the allocator and reads the stamped `vc_id`; write it in the style of the existing streak tests.

- [ ] **Step 2: Run** it. Expected FAIL on the third call (fresh round-robin VC).
- [ ] **Step 3: Implement** `vc = (dst_id ^ id) % num_vc_` for AW (W inherits via `current_aw_vc_`, unchanged), `fixed_vc = 1` stamped as today. Delete the streak table and the round-robin pointer if nothing else reads them. Update the file comment `:7-28`.
- [ ] **Step 4:** `ctest -R 'Nmu'`. Expected PASS. Tests that assert a round-robin VC sequence for AWs are rewritten to the hash rule, not deleted.
- [ ] **Step 5: Commit** `fix(nmu): write VC is a hash of (dst, id), same as the NSU read rule`

---

## Stage 2: NSU per-VC DAT ingress and W reassembly
Goal: `nsu::Depacketize` holds one bounded flit queue per DAT VC, reassembles each VC's AW+W worm independently, and emits credit on consumption.
Success Criteria: new `NsuDepacketize` tests pass, all NSU and integration tests pass.
Status: Not Started

### Task 2.1: Per-VC queues replace the data-class S1 registers

**Files:**
- Modify: `ref_model/c_model/include/nsu/depacketize.hpp` (ctor `:47-77`, `s1_occupancy` `:89-105`, members `:120-170`, `drain_ingress_` `:271-364`, `tick` `:368-371`, `pop_aw` `:377-409`, `pop_w` `:410-426`)
- Modify: `ref_model/c_model/include/nsu/nsu.hpp:185-186` (ctor arg), add `take_dat_credit`
- Test: `ref_model/c_model/tests/nsu/test_nsu_depacketize.cpp`

**Interfaces:**
- Produces: `Depacketize(router::NocReqIn& req_in, MetaBuffer& meta, std::size_t max_unique_ids, router::NocReqIn& dat_req_in = router::null_req_in(), uint8_t src_id = 0, std::array<address_map::SpaceCoords, 2> space_coords = {}, uint8_t port_id = 0, uint8_t dat_num_vc = 1)`. `bool Depacketize::take_dat_credit(uint8_t vc)`. `bool Nsu::take_dat_credit(uint8_t vc)`.

- [ ] **Step 1: Write the failing tests.** `make_aw_flit` and `make_w_flit` in the test file gain a trailing `uint8_t vc = 0` parameter that sets `vc_id`. Delete `PendingHolBlockingS1WFullBlocksAwBehind` (`:285-306`). Append:

```cpp
// Two data-class worms on different VCs arrive flit-interleaved, which is
// what the per-(output, VC) router lock produces. Each VC reassembles its
// own burst; the AXI side sees A's beats, then B's, never mixed.
TEST(NsuDepacketize, InterleavedDataWormsReassemblePerVc) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, axi::NOC_ID_SPACE, ni::cmodel::router::null_req_in(),
                      0, {}, 0, /*dat_num_vc=*/2);
    auto aw_a = make_aw_flit(0x01, 0x0, 0x10, 0, 0, ni::AXI_CH_DataAw, /*vc=*/0);
    auto aw_b = make_aw_flit(0x02, 0x0, 0x11, 0, 0, ni::AXI_CH_DataAw, /*vc=*/1);
    aw_a.set_payload_field("AW", "awlen", 1);
    aw_b.set_payload_field("AW", "awlen", 1);
    ASSERT_TRUE(noc.req_out().push_flit(aw_a));
    ASSERT_TRUE(noc.req_out().push_flit(aw_b));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xA0, false, ni::AXI_CH_DataW, 0)));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xB0, false, ni::AXI_CH_DataW, 1)));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xA1, true, ni::AXI_CH_DataW, 0)));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xB1, true, ni::AXI_CH_DataW, 1)));
    for (int i = 0; i < 6; ++i) depkt.tick();
    auto a = depkt.pop_aw();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->id, 0x01);
    EXPECT_EQ(depkt.pop_w()->strb, 0xA0u);
    EXPECT_EQ(depkt.pop_w()->strb, 0xA1u);
    auto b = depkt.pop_aw();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 0x02);
    EXPECT_EQ(depkt.pop_w()->strb, 0xB0u);
    EXPECT_EQ(depkt.pop_w()->strb, 0xB1u);
    EXPECT_FALSE(depkt.pop_w().has_value());
}

// Credit is the VC queue slot, returned when the flit leaves the queue
// (pop_aw / pop_w), not when it arrives. One pulse per consumed flit.
TEST(NsuDepacketize, DatCreditPulsesOnConsumption) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, axi::NOC_ID_SPACE, ni::cmodel::router::null_req_in(),
                      0, {}, 0, /*dat_num_vc=*/2);
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x01, 0x0, 0x10, 0, 0, ni::AXI_CH_DataAw, 1)));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xFF, true, ni::AXI_CH_DataW, 1)));
    depkt.tick();
    depkt.tick();
    EXPECT_FALSE(depkt.take_dat_credit(1));  // arrived, not consumed
    ASSERT_TRUE(depkt.pop_aw().has_value());
    EXPECT_TRUE(depkt.take_dat_credit(1));
    EXPECT_FALSE(depkt.take_dat_credit(1));
    ASSERT_TRUE(depkt.pop_w().has_value());
    EXPECT_TRUE(depkt.take_dat_credit(1));
    EXPECT_FALSE(depkt.take_dat_credit(0));
}

// A second worm on the SAME VC queues behind the first worm's beats: a VC's
// W stream is one worm at a time. Same order semantics as the deleted
// PendingHolBlockingS1WFullBlocksAwBehind, without the ingress stash.
TEST(NsuDepacketize, SecondWormOnSameVcWaitsBehindFirstWormsBeats) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, axi::NOC_ID_SPACE);
    auto aw_owner = make_aw_flit(0x06, 0x0, 0x10, 0, 0, ni::AXI_CH_DataAw);
    aw_owner.set_payload_field("AW", "awlen", 1);
    ASSERT_TRUE(noc.req_out().push_flit(aw_owner));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xAA, false, ni::AXI_CH_DataW)));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xBB, true, ni::AXI_CH_DataW)));
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x07, 0x0, 0x10, 0, 0, ni::AXI_CH_DataAw)));
    for (int i = 0; i < 4; ++i) depkt.tick();
    ASSERT_TRUE(depkt.pop_aw().has_value());
    EXPECT_FALSE(depkt.pop_aw().has_value());  // vc0 front is W, not AW
    EXPECT_EQ(depkt.pop_w()->strb, 0xAAu);
    EXPECT_EQ(depkt.pop_w()->strb, 0xBBu);
    EXPECT_TRUE(depkt.pop_aw().has_value());
}

// Fault injection: a VC's W stream interrupted by another head means the
// fabric broke per-VC contiguity. Fail loud, never mis-pair.
TEST(NsuDepacketizeDeath, WStreamInterruptedOnItsVcAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, axi::NOC_ID_SPACE);
    auto aw_a = make_aw_flit(0x01, 0x0, 0x10, 0, 0, ni::AXI_CH_DataAw);
    aw_a.set_payload_field("AW", "awlen", 1);
    ASSERT_TRUE(noc.req_out().push_flit(aw_a));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xA0, false, ni::AXI_CH_DataW)));
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x02, 0x0, 0x10, 0, 0, ni::AXI_CH_DataAw)));
    for (int i = 0; i < 3; ++i) depkt.tick();
    ASSERT_TRUE(depkt.pop_aw().has_value());
    ASSERT_TRUE(depkt.pop_w().has_value());
    EXPECT_DEATH(depkt.pop_w(), "contiguity");
}
```

- [ ] **Step 2: Run** `ctest -R NsuDepacketize`. Expected: compile failure (no `dat_num_vc` ctor arg, no `take_dat_credit`).

- [ ] **Step 3: Implement.** Members, replacing `s1_data_aw_`, `s1_data_w_`, `pending_dat_`:

```cpp
// Data-class ingress, per VC (FlooNoC chimney demuxes VCs before its spill
// registers, floo_nw_chimney.sv:276-311). Depth = NOC_ROUTER_VC_DEPTH, the
// router's LOCAL credit seed: the sender never has more than that many
// unacknowledged flits per VC, so push cannot overflow. A slot is returned
// (dat_credit_pending_) when pop_aw / pop_w consume the flit. Both ingresses
// deposit data-class flits here (DAT in co-sim, REQ in the ctest stubs).
std::vector<std::deque<Flit>> dat_q_;
std::vector<std::size_t> dat_credit_pending_;
uint8_t dat_aw_rr_ = 0;  // VC round-robin for data-class AW admission

struct WBurst {
    AxiClass cls;
    uint32_t beats;
    uint8_t vc;  // data class: the VC queue its W beats arrive on
};
```

Ctor: `dat_q_.resize(dat_num_vc); dat_credit_pending_.assign(dat_num_vc, 0);`

`drain_ingress_`, data-class cases (`NarrowAw` keeps `s1_narrow_aw_` and the `w_addr_fifo_` push, `NarrowW` keeps `s1_narrow_w_`, the `pending` stash stays for narrow only):

```cpp
case ni::AXI_CH_DataAw:
case ni::AXI_CH_DataW: {
    const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
    assert(vc < dat_q_.size() && "nsu::Depacketize: data flit names a VC beyond dat_num_vc");
    assert(dat_q_[vc].size() < static_cast<std::size_t>(::ni::NOC_ROUTER_VC_DEPTH) &&
           "nsu::Depacketize: per-VC ingress overflow -- sender credit discipline broken");
    dat_q_[vc].push_back(f);
    break;
}
```

`tick()` keeps both `drain_ingress_` calls and the `pending_dat_` member, and asserts `!pending_dat_` after the DAT call (data flits never stash). Include `ni_params.h` for `NOC_ROUTER_VC_DEPTH`. The overflow assert also guards REQ-ingress data flits from the ctest stubs, whose senders have no credit: no test pushes more than `NOC_ROUTER_VC_DEPTH` data flits on one VC between ticks, and a future one that does must return credit like the wrap does.

`pop_aw`:

```cpp
const bool narrow_ready = s1_narrow_aw_.full();
std::optional<uint8_t> data_vc;
for (std::size_t k = 0; k < dat_q_.size(); ++k) {
    const auto v = static_cast<uint8_t>((dat_aw_rr_ + k) % dat_q_.size());
    if (!dat_q_[v].empty() &&
        dat_q_[v].front().get_header_field("axi_ch") == ni::AXI_CH_DataAw) {
        data_vc = v;
        break;
    }
}
const bool data_ready = data_vc.has_value();
if (!narrow_ready && !data_ready) return std::nullopt;
const bool take_data = data_ready && (!narrow_ready || aw_prefer_data_);
if (narrow_ready && data_ready) aw_prefer_data_ = !aw_prefer_data_;
if (meta_.write_full()) return std::nullopt;
Flit f;
if (take_data) {
    f = dat_q_[*data_vc].front();
    dat_q_[*data_vc].pop_front();
    ++dat_credit_pending_[*data_vc];
    dat_aw_rr_ = static_cast<uint8_t>((*data_vc + 1) % dat_q_.size());
} else {
    f = s1_narrow_aw_.take();
}
axi::AwBeat b = decode_aw(f);
w_order_.push_back({take_data ? AxiClass::Data : AxiClass::Narrow,
                    static_cast<uint32_t>(b.len) + 1u, take_data ? *data_vc : uint8_t{0}});
// rest unchanged
```

`pop_w`:

```cpp
if (w_order_.empty()) return std::nullopt;
const WBurst& front = w_order_.front();
axi::WBeat b;
if (front.cls == AxiClass::Data) {
    auto& q = dat_q_[front.vc];
    if (q.empty()) return std::nullopt;
    assert(q.front().get_header_field("axi_ch") == ni::AXI_CH_DataW &&
           "nsu::Depacketize::pop_w: W stream on this VC interrupted by a non-W flit -- "
           "fabric broke per-VC wormhole contiguity");
    b = decode_w(q.front());
    q.pop_front();
    ++dat_credit_pending_[front.vc];
} else {
    if (!s1_narrow_w_.full()) return std::nullopt;
    b = s1_narrow_w_.take();
}
// existing beat counting and WLAST assert unchanged
```

`take_dat_credit`:

```cpp
// One credit pulse per consumed data flit, at most one per VC per call
// (mirror of router::LinkCreditOut::take).
bool take_dat_credit(uint8_t vc) {
    if (dat_credit_pending_[vc] == 0) return false;
    --dat_credit_pending_[vc];
    return true;
}
```

`s1_occupancy`: `DataAw` / `DataW` return the count of flits with that `axi_ch` summed over `dat_q_` (0 to `NOC_ROUTER_VC_DEPTH` per VC, no longer a 0/1 register probe; the co-sim state dump at `cmodel_dpi.cpp:905+` prints it as is, note the range change in the dump's comment). Rewrite the class comment `:120-134` (per-class registers) to describe the per-VC queues.

`Nsu`: pass `cfg_.dat_num_vc` as the last `depacketize_` ctor arg. Add `bool take_dat_credit(uint8_t vc) { return depacketize_.take_dat_credit(vc); }`.

- [ ] **Step 4: Run** `ctest -R 'Nsu|Integration|NarrowClass|RequestResponse'`. Expected PASS, including the death test.

- [ ] **Step 5: Full ctest.** Expected all green. If `test_nsu_dat_face.cpp` or `test_nsu.cpp` relied on the one-tick S1 register timing, the data flit is still visible to `pop_*` on the tick after `tick()` deposits it; adjust only tick counts.

- [ ] **Step 6: Commit** `feat(nsu): per-VC DAT ingress queues, W reassembly per VC, credit on consumption`

### Task 2.2: Standalone credit path

**Files:**
- Modify: `ref_model/c_model/include/nsu/nsu_standalone.hpp:40-75` (`QueueNocReqIn`), `:154-155`, `:196`

- [ ] **Step 1:** `dat_req_take_credit(uint8_t vc)` returns `nsu_.take_dat_credit(vc)`. Remove `QueueNocReqIn::size_pending`, `take_credit`, `pending`, `pending_`, the `++pending_[vc]` in `pop_flit`, and the two `size_pending` calls: their only reader was `dat_req_take_credit`. `nmu_standalone.hpp` keeps its own copy (NMU R ingress unchanged).
- [ ] **Step 2:** `ctest -R 'Nsu|Wrap|Chain'`. Expected PASS.
- [ ] **Step 3: Commit** `refactor(nsu): standalone DAT credit comes from depacketize consumption`

---

## Stage 3: DatMergeWrap forwards NSU credit
Goal: the router's LOCAL DAT credit for NSU-bound flits is returned when the NSU consumes them, not at demux.
Success Criteria: merge tests pass, full ctest green, directed and continuous co-sim pass at vc2 and vc8.
Status: Not Started

### Task 3.1: Merge credit path

**Files:**
- Modify: `ref_model/c_model/include/wrap/dat_merge_wrap.hpp:44-50` (comment), `:96-110` (`DatMergeInputs`), `:132-148` (`init`), `:199-211` (ingress demux)
- Modify: `ref_model/dpi/cmodel_dpi.h:121-124`, `ref_model/dpi/cmodel_dpi.cpp:360-379`
- Modify: `ref_model/top/dat_merge_wrap.sv` (NSU-facing port `nsu_rx_dat_crdvalid_i`, DPI import, `set_inputs` marshalling), `ref_model/top/ni_wrap.sv:146,158` (connect `u_nsu.rx_dat_crdvalid_o`)
- Test: `ref_model/c_model/tests/wrap/test_dat_merge_wrap.cpp`

**Interfaces:**
- Produces: `DatMergeInputs::nsu_rx_dat_crdvalid` (`VcCreditVec`). DPI `cmodel_dat_merge_set_inputs(ctx, nmu_tx_dat_valid, nmu_tx_dat_flit, nsu_tx_dat_valid, nsu_tx_dat_flit, tx_dat_crdvalid, nsu_rx_dat_crdvalid, rx_dat_valid, rx_dat_flit)`.

- [ ] **Step 1: Write the failing tests.** The file already has VC-parameterized helpers `make_data_aw(awid, vc)`, `make_data_w(vc)`, `make_data_r(rid, vc)` (`test_dat_merge_wrap.cpp:22-49`); use them:

```cpp
// NSU-bound ingress (DataAw/DataW) returns credit when the NSU reports it
// consumed the flit. NMU-bound (DataR) returns at demux, as before.
TEST(DatMergeWrap, NsuBoundIngressCreditFollowsTheNsu) {
    DatMergeWrap m;
    m.init(/*dat_num_vc=*/2);
    DatMergeInputs in{};
    in.rx_dat_valid = true;
    in.rx_dat_flit = flit_to_bytes(make_data_w(/*vc=*/1));
    m.set_inputs(in);
    m.tick();
    DatMergeOutputs out{};
    m.get_outputs(out);
    EXPECT_TRUE(out.nsu_rx_dat_valid);
    EXPECT_FALSE(out.rx_dat_crdvalid[1]);
    in = DatMergeInputs{};
    in.nsu_rx_dat_crdvalid[1] = true;
    m.set_inputs(in);
    m.tick();
    m.get_outputs(out);
    EXPECT_TRUE(out.rx_dat_crdvalid[1]);
}

TEST(DatMergeWrap, NmuBoundIngressCreditIsImmediate) {
    DatMergeWrap m;
    m.init(2);
    DatMergeInputs in{};
    in.rx_dat_valid = true;
    in.rx_dat_flit = flit_to_bytes(make_data_r(/*rid=*/0, /*vc=*/0));
    m.set_inputs(in);
    m.tick();
    DatMergeOutputs out{};
    m.get_outputs(out);
    EXPECT_TRUE(out.nmu_rx_dat_valid);
    EXPECT_TRUE(out.rx_dat_crdvalid[0]);
}

// Same VC, same tick, one NMU-bound demux and one NSU pulse: two credits
// owed, one wire bit per tick, so the second is emitted next tick, not lost.
TEST(DatMergeWrap, SameVcCreditsFromBothSidesAreNotCollapsed) {
    DatMergeWrap m;
    m.init(2);
    DatMergeInputs in{};
    in.rx_dat_valid = true;
    in.rx_dat_flit = flit_to_bytes(make_data_r(0, 0));
    in.nsu_rx_dat_crdvalid[0] = true;
    m.set_inputs(in);
    m.tick();
    DatMergeOutputs out{};
    m.get_outputs(out);
    EXPECT_TRUE(out.rx_dat_crdvalid[0]);
    m.set_inputs(DatMergeInputs{});
    m.tick();
    m.get_outputs(out);
    EXPECT_TRUE(out.rx_dat_crdvalid[0]);
    m.tick();
    m.get_outputs(out);
    EXPECT_FALSE(out.rx_dat_crdvalid[0]);
}
```

- [ ] **Step 2: Run** `ctest -R DatMergeWrap`. Expected: compile failure on `nsu_rx_dat_crdvalid`.

- [ ] **Step 3: Implement**

```cpp
// DatMergeInputs
// From NSU: credit pulses for DataAw/DataW flits it consumed from its
// per-VC ingress queues (nsu_wrap rx_dat_crdvalid_o).
VcCreditVec nsu_rx_dat_crdvalid;

// DatMergeWrap member
// Credit owed to the router for its LOCAL output: NMU-bound flits at demux
// (NMU's R ingress is unbounded), NSU-bound flits when the NSU consumes
// them. One wire bit per VC per tick, so owed pulses queue here.
std::unique_ptr<router::LinkCreditOut> rx_credit_;  // same shape as router_wrap.hpp:83

// init()
rx_credit_ = std::make_unique<router::LinkCreditOut>(dat_num_vc);

// tick(), ingress demux
if (in_.rx_dat_valid) {
    const Flit f = flit_from_bytes(in_.rx_dat_flit);
    if (f.get_header_field("axi_ch") == ni::AXI_CH_DataR) {
        out_.nmu_rx_dat_valid = true;
        out_.nmu_rx_dat_flit = in_.rx_dat_flit;
        rx_credit_->receive_credit(static_cast<uint8_t>(f.get_header_field("vc_id")));
    } else {
        out_.nsu_rx_dat_valid = true;
        out_.nsu_rx_dat_flit = in_.rx_dat_flit;
    }
}
for (uint8_t vc = 0; vc < dat_num_vc_; ++vc) {
    if (in_.nsu_rx_dat_crdvalid[vc]) rx_credit_->receive_credit(vc);
    out_.rx_dat_crdvalid[vc] = rx_credit_->take(vc);
}
```

Include `router/router_adapters.hpp`. Rewrite the class comment at `:44-50` ("immediate credit-return" is no longer true for NSU-bound flits).

DPI: add `svBitVecVal* nsu_rx_dat_crdvalid` before `rx_dat_valid` in `cmodel_dpi.h` and `.cpp`; `in.nsu_rx_dat_crdvalid = unpack_vc_credit<VcCreditVec>(nsu_rx_dat_crdvalid, m->num_vc());`. SV: `input logic [DAT_NUM_VC-1:0] nsu_rx_dat_crdvalid_i` in the NSU-facing port group, `input bit [DAT_NUM_VC-1:0] nsu_rx_dat_crdvalid` in the import, marshalled like the existing `tx_dat_crdvalid_i` in the `set_inputs` call. `ni_wrap.sv`: declare `logic [DAT_NUM_VC-1:0] nsu_rx_dat_crdvalid;`, connect `u_nsu .rx_dat_crdvalid_o(nsu_rx_dat_crdvalid)` and `u_dat_merge .nsu_rx_dat_crdvalid_i(nsu_rx_dat_crdvalid)`. The NMU instance keeps its unconnected `rx_dat_crdvalid_o()` and its comment.

- [ ] **Step 4: Run** `ctest -R 'DatMergeWrap|Dpi|Chain'`. Expected PASS.
- [ ] **Step 5: Full ctest.** Expected all green.
- [ ] **Step 6: Commit** `feat(wrap): merge forwards NSU per-VC ingress credit to the DAT router`

### Task 3.2: Co-sim gates

- [ ] `make build-verilator` (DPI signature changed, both obj_dir classes rebuild).
- [ ] Directed: `make sim CONFIG=mesh_4x4 PATTERN=uniform_random INJECTION_MODE=0 BURST_LEN=32`. Expected PASS with scoreboard.
- [ ] Directed: `make sim CONFIG=mesh_4x4 PATTERN=bit_complement INJECTION_MODE=0 BURST_LEN=32`. Expected PASS.
- [ ] Continuous, vc2: `make sim-gen CONFIG=mesh_4x4 PATTERN=uniform_random INJECTION_MODE=1 INJECTION_RATE=0.9 BURST_LEN=32` then `make sim` with the same arguments. Expected `PASS (completed)`, no watchdog.
- [ ] Continuous, vc8: set `DAT_NUM_VC` to 8 in `specgen/source/constants.yaml`, regenerate, `make build-verilator`, same run, restore to 2 and rebuild. Expected PASS.
- [ ] Record any failure verbatim under "Stage 3 findings" below. Do not patch around it.

---

## Stage 4: Docs and measurement
Goal: specs describe the new lock and ingress; the report shows what VC count buys.
Success Criteria: every listed doc line updated, `sweep_summary.md` regenerated with vc2 and vc8 sets, 3 seeds each.
Status: Not Started

### Task 4.1: Specs

**Files:**
- `docs/router-spec.md:169` (stage 2 row), `:203-256` (§2.5: output-VC round-robin advances every grant, input RR on tail, the worked example redone), `:258-283` (§2.6 retitled "Wormhole lock rules (per output VC)", rule 2 and the example rewritten, the "never spans outputs" sentence kept), `:438` (R3 row), `:734-745` (SPEC 10 per-VC non-interleave, SPEC 11 per-VC persistence, test names `RouterWormhole.WormsOnDifferentVcsInterleavePerOutput`, `OpenWormOnVc0DoesNotBlockVc1`, `CreditStarvedVcDoesNotIdleTheOutput`), `:871-872` and `:923` diagram labels.
- `docs/nsu-spec.md:143` (a worm holds one VC of an output, not the output), `:366` (data-class ingress: `NOC_ROUTER_VC_DEPTH` flits per DAT VC, credit on consumption, at most one credit pulse per VC per cycle on the wire while `pop_aw` and `pop_w` may consume two flits of one VC in a cycle; narrow unchanged), `:370` rule 1 (flits of different packets interleave flit by flit across VCs, packet granularity within a VC).
- `docs/trade-off.md:28`, `:164-170`.
- `docs/verification-environment.md:271-272` (`floo_wormhole_arbiter.sv` per (output, VC) and `floo_vc_arbiter.sv` per-cycle VC mux, both ported).
- `docs/known-limitations.md`: drop any row stating R waits behind W worms at an output or that VC count has no effect.

- [ ] **Step 1:** Edit each location. Tables over prose. No semicolons or dashes in running text.
- [ ] **Step 2:** `grep -rn 'per-output wormhole\|per output, across VCs\|across VCs' docs/` returns nothing stale.
- [ ] **Step 3: Commit** `docs(router,nsu): per-(output, VC) wormhole lock and per-VC NSU ingress`

### Task 4.2: Measurement

- [ ] Same-vc2 A/B first: the LOCAL credit loop for NSU-bound flits is about 3 cycles longer than before (NSU consume, nsu_wrap latch, merge, router) against the unchanged `NOC_ROUTER_VC_DEPTH` 8. Run `uniform_random` and `neighbor` at vc2, 3 seeds, and compare BW against the archived pre-change runs of the same cells. A drop beyond seed spread is a finding for `docs/backlog.md`, not something to tune away here.
- [ ] Generate and run, `SEED` 1, 2, 3, `INJECTION_MODE=1 INJECTION_RATE=0.9 BURST_LEN=32 CONFIG=mesh_4x4`, patterns `uniform_random bit_complement neighbor all_to_all`, at `DAT_NUM_VC` 2 and 8 (yaml edit, rebuild, restore to 2 after). Output under `sim/verilator/output/` so `summarize_results.py` groups them.
- [ ] Re-run the Scenario 2 congestion probe (narrow and data, the `interference_*` recipe in `sim/verilator/output/`) at vc2.
- [ ] Regenerate `sim/verilator/output/sweep_summary.md` with the assembly command from this session. Add a "before" column from the archived pre-change runs for the same cells.
- [ ] Write the vc2 vs vc8 delta and the Scenario 2 data-probe delta into `docs/backlog.md` "Last round".
- [ ] Delete this file. Commit `docs(backlog): per-VC lock measurement`.

---

## Stage 1 findings

Task 1.3 gate, `make sim CONFIG=mesh_4x4 PATTERN=uniform_random INJECTION_MODE=0 BURST_LEN=32`
after the per-(output, VC) lock, at `DAT_NUM_VC` 2:

```
DIRECTED PASS: directed_mesh_4x4_uniform_random_s314104646 scoreboard clean, non-vacuous
```

No NSU `pop_w` assert, no wedge. The predicted risk (two NMUs' W worms
interleaving on one link past a single-stream NSU) did not fire at this seed and
pattern. The gate is re-run in Stage 3.

Deviation from the Task 1.2 brief, `CreditBlockedTailDoesNotOverflowToAnotherVc`:
the brief's `dst = make_dst(3, 1)` routes EAST here and EAST again at the next
hop, so its preferred output VC is 1 (floo_vc_assignment.sv:93), not the 0 every
assertion in that test names. `make_dst(2, 3)` routes EAST here and NORTH next,
preferred VC 0 (:91), which keeps every assertion in the brief verbatim and
leaves vc1 as the FVADA overflow the tail must not steal.

## Stage 3 findings

(none yet)
