# RouterChannel Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A production `RouterChannel` (2-node, 1-hop, full-duplex fabric segment) that wires the existing `Router` into the NMU↔NSU datapath, plus its unit tests and a bidirectional loopback.

**Architecture:** Tasks 1-3 build the three adapters (Inject/Eject/CreditRelay) and `RouterChannel` (4 routers + 8 adapters + relays) in `c_model/include/noc/router_channel.hpp`, each with focused unit tests. Task 4 adds the credit-conservation + full-backpressure unit tests. Task 5 builds the bidirectional loopback by adapting the existing single-flow loopback harness to two full-NI nodes wired to one `RouterChannel`.

**Tech Stack:** C++17 header-only c_model, GoogleTest, `make build-cmodel` / `ctest`.

**Spec:** `docs/superpowers/specs/2026-06-13-router-channel-design.md`.

**Conventions (all tasks):**
- Build `make build-cmodel`. Tests: `cd build/cmodel && export TEST_TMPDIR="$(pwd -W)/test_tmp"; mkdir -p test_tmp; ctest -R <regex> --output-on-failure`. Full: `make test`.
- After editing `.hpp`/`.cpp`: `/c/msys64/mingw64/bin/clang-format -i <file>` (NOT on PATH). ColumnLimit 100, IndentWidth 4. Never `--no-verify`.
- Feature branch: Task 1 Step 1 creates `feat/router-channel`.

**Verified API facts:**
- `c_model/include/noc/router.hpp`: `enum class RouterPort { LOCAL=0, NORTH=1, EAST=2, SOUTH=3, WEST=4 }`; `RouterConfig { uint8_t x,y,mesh_x_dim,mesh_y_dim,num_vc; std::size_t vc_depth, output_fifo_depth; }`. `class RouterLink { virtual void push_flit(const Flit&)=0; }`. `class RouterCreditSink { virtual void receive_credit(uint8_t vc_id)=0; }`. `Router(cfg)`; `RouterLink& input(std::size_t port)`; `void set_downstream(std::size_t port, RouterLink&)`; `void set_upstream_credit(std::size_t port, RouterCreditSink&)`; `void receive_credit(std::size_t port, uint8_t vc_id)` (downstream returns an OUTPUT-port credit); `void tick()`; introspection `std::size_t credit(std::size_t out_port, uint8_t vc) const`, `input_fifo_size(port,vc)`, `output_fifo_size(port)`. Router asserts on a 2nd flit pushed to the same input port before a tick clears the landing register; `num_vc` must be `1..2^VC_ID_WIDTH (=8)`.
- NoC interfaces (`c_model/include/noc/noc_*`): `NocReqOut`/`NocRspOut { virtual bool push_flit(const Flit&)=0; virtual bool credit_avail(uint8_t vc) const {return true;} }`; `NocReqIn`/`NocRspIn { virtual std::optional<Flit> pop_flit()=0; }`.
- `ChannelModel` accessors to mirror (`c_model/tests/common/channel_model.hpp`): `nmu_req_out()→NocReqOut&`, `nmu_rsp_in()→NocRspIn&`, `nsu_req_in(idx)→NocReqIn&`, `nsu_rsp_out(idx)→NocRspOut&`, `tick()`. (We do NOT mirror set_dst_route/set_nsu_latency/set_req_delay — out of scope §10.)
- `Flit` header field `vc_id` via `flit.get_header_field("vc_id")`. `ni::header::VC_ID_WIDTH`.
- Params: `NI_NOC_ROUTER_VC_DEPTH`, `NI_NOC_ROUTER_OUTPUT_FIFO_DEPTH`, `NI_NOC_MESH_X_DIM`, `NI_NOC_MESH_Y_DIM`, `NI_NOC_NUM_VC` from `ni_params.h`.

---

### Task 1: InjectAdapter (NI→router, credit mirror + landing guard)

**Files:**
- Create: `c_model/include/noc/router_channel.hpp`
- Create: `c_model/tests/noc/test_router_channel.cpp`
- Modify: `c_model/tests/noc/CMakeLists.txt`

- [ ] **Step 1: Create the branch**

```bash
git checkout -b feat/router-channel
```

- [ ] **Step 2: Write the failing InjectAdapter test**

Create `c_model/tests/noc/test_router_channel.cpp`:
```cpp
#include "noc/router_channel.hpp"
#include "noc/router.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::noc::Router;
using ni::cmodel::noc::RouterConfig;
using ni::cmodel::noc::RouterPort;
using ni::cmodel::noc::InjectAdapter;

namespace {

RouterConfig cfg_at(uint8_t x, uint8_t y) {
    RouterConfig c;
    c.x = x; c.y = y;
    c.num_vc = 2; c.vc_depth = 2;
    return c;
}
Flit req_flit(uint8_t dst, uint8_t vc) {
    Flit f; f.set_header_field("dst_id", dst); f.set_header_field("vc_id", vc);
    f.set_header_field("last", 1);
    return f;
}

TEST(InjectAdapter, CreditMirrorGatesPush) {
    SCENARIO("InjectAdapter: push_flit honors the per-VC credit mirror (seeded to vc_depth)");
    Router r(cfg_at(0, 0));
    InjectAdapter inj(r, static_cast<std::size_t>(RouterPort::LOCAL), /*num_vc=*/2, /*depth=*/2);
    r.set_upstream_credit(static_cast<std::size_t>(RouterPort::LOCAL), inj);
    EXPECT_TRUE(inj.credit_avail(0));
    EXPECT_TRUE(inj.push_flit(req_flit(0, 0)));   // 1st this tick: ok (mirror 2->1)
    EXPECT_FALSE(inj.push_flit(req_flit(0, 0)))    // 2nd this tick: landing guard -> false
        << "second push in the same tick must backpressure, not hit the router assert";
}

TEST(InjectAdapter, LandingGuardResetsOnTick) {
    SCENARIO("InjectAdapter: the per-tick push flag resets so the next tick accepts again");
    Router r(cfg_at(0, 0));
    InjectAdapter inj(r, static_cast<std::size_t>(RouterPort::LOCAL), 2, 2);
    r.set_upstream_credit(static_cast<std::size_t>(RouterPort::LOCAL), inj);
    EXPECT_TRUE(inj.push_flit(req_flit(0, 0)));
    EXPECT_FALSE(inj.push_flit(req_flit(0, 0)));
    inj.on_tick();                                 // cycle boundary (called by RouterChannel.tick)
    r.tick();
    EXPECT_TRUE(inj.push_flit(req_flit(0, 0))) << "new tick: landing free, one push allowed";
}

}  // namespace
```
Add to `c_model/tests/noc/CMakeLists.txt`:
```cmake
add_cmodel_test(test_router_channel)
target_include_directories(test_router_channel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

- [ ] **Step 3: Run to verify failure (router_channel.hpp missing)**

Run: `make build-cmodel` — Expected: compile error, `noc/router_channel.hpp` not found.

- [ ] **Step 4: Create router_channel.hpp with InjectAdapter**

```cpp
#pragma once
// RouterChannel: production 2-node, 1-hop, full-duplex fabric segment wiring the
// Router into the NMU<->NSU datapath. Spec:
// docs/superpowers/specs/2026-06-13-router-channel-design.md
//
// Three adapters bridge the NoC interface (retryable push + credit query + pull)
// to the Router link contract (void push + registered credit pulse):
//   InjectAdapter : NocReqOut/NocRspOut + RouterCreditSink  (NI -> router LOCAL input)
//   EjectAdapter  : NocReqIn/NocRspIn  + RouterLink         (router LOCAL output -> NI)
//   CreditRelay   : RouterCreditSink                        (downstream input credit
//                                                            -> upstream output credit)
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "ni_params.h"
#include "noc/noc_req_in.hpp"
#include "noc/noc_req_out.hpp"
#include "noc/noc_rsp_in.hpp"
#include "noc/noc_rsp_out.hpp"
#include "noc/router.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace ni::cmodel::noc {

// NI -> router LOCAL input. Implements all four producer-side NoC interfaces
// (NocReqOut and NocRspOut share the same shape) and is the router's
// RouterCreditSink for that input port. A per-VC credit mirror (seeded to the
// router input FIFO depth) plus a per-tick landing-register guard translate the
// router's void/assert push into a retryable false.
class InjectAdapter : public NocReqOut, public NocRspOut, public RouterCreditSink {
  public:
    InjectAdapter(Router& router, std::size_t port, uint8_t num_vc, std::size_t depth)
        : router_(router), port_(port), credit_(num_vc, depth) {}

    bool credit_avail(uint8_t vc) const override {
        return !pushed_this_tick_ && credit_[vc] > 0;
    }
    bool push_flit(const Flit& flit) override {
        const auto vc = static_cast<uint8_t>(flit.get_header_field("vc_id"));
        if (pushed_this_tick_ || credit_[vc] == 0) return false;
        router_.input(port_).push_flit(flit);
        --credit_[vc];
        pushed_this_tick_ = true;
        return true;
    }
    // Router returns an input-FIFO credit (registered, start of next tick).
    void receive_credit(uint8_t vc) override { ++credit_[vc]; }

    // Called once per cycle by RouterChannel.tick() (the cycle boundary).
    void on_tick() { pushed_this_tick_ = false; }

  private:
    Router& router_;
    std::size_t port_;
    std::vector<std::size_t> credit_;   // per-VC mirror
    bool pushed_this_tick_ = false;
};

}  // namespace ni::cmodel::noc
```

- [ ] **Step 5: Build + run the InjectAdapter tests**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R "InjectAdapter" --output-on-failure` — Expected: PASS.

- [ ] **Step 6: clang-format + commit**

```bash
/c/msys64/mingw64/bin/clang-format -i c_model/include/noc/router_channel.hpp c_model/tests/noc/test_router_channel.cpp
git add c_model/include/noc/router_channel.hpp c_model/tests/noc/test_router_channel.cpp c_model/tests/noc/CMakeLists.txt
git commit -m "feat(noc): RouterChannel InjectAdapter (credit mirror + landing guard)"
```

---

### Task 2: EjectAdapter + CreditRelay

**Files:**
- Modify: `c_model/include/noc/router_channel.hpp`, `c_model/tests/noc/test_router_channel.cpp`

- [ ] **Step 1: Write failing tests**

Add to `test_router_channel.cpp`:
```cpp
TEST(EjectAdapter, BuffersEjectedFlitAndReturnsCredit) {
    SCENARIO("EjectAdapter: router push buffers; pop_flit serves it and returns a credit");
    Router r(cfg_at(0, 0));
    const auto LOCAL = static_cast<std::size_t>(RouterPort::LOCAL);
    InjectAdapter inj(r, LOCAL, 2, 2);
    ni::cmodel::noc::EjectAdapter ej(r, LOCAL, /*depth=*/2);
    r.set_upstream_credit(LOCAL, inj);
    r.set_downstream(LOCAL, ej);
    // Inject a flit whose dst is this node (0,0) -> routes to LOCAL output -> ejects.
    EXPECT_TRUE(inj.push_flit(req_flit(/*dst=*/0, /*vc=*/0)));
    inj.on_tick(); r.tick();   // stage1: landing->fifo
    r.tick();                  // stage2: grant->output fifo (LOCAL)
    r.tick();                  // stage3: output fifo -> downstream (ej buffers)
    auto out = ej.pop_flit();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->get_header_field("dst_id"), 0u);
}

TEST(CreditRelay, ForwardsToUpstreamOutputPort) {
    SCENARIO("CreditRelay: a downstream input-credit pulse becomes upstream receive_credit(port)");
    Router up(cfg_at(1, 0));
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    ni::cmodel::noc::CreditRelay relay(up, WEST);
    EXPECT_EQ(up.credit(WEST, 0), up_vc_depth());   // helper: seeded full
    up.receive_credit(WEST, 0);                      // pretend full first (will assert if >seed)
    // Actually drive via relay: relay.receive_credit(vc) must call up.receive_credit(WEST,vc).
}
```
Note: the `CreditRelay` test must not overflow the upstream credit counter (the router asserts on overflow). Restructure it to first DECREMENT the upstream output credit by routing a flit out of `up`'s WEST, then call `relay.receive_credit(0)` and assert `up.credit(WEST,0)` incremented back. If expressing that standalone is awkward, defer the CreditRelay assertion to the RouterChannel end-to-end test (Task 3) and keep here only a compile/smoke check that `relay.receive_credit(0)` runs. Add a small `up_vc_depth()` helper returning `NI_NOC_ROUTER_VC_DEPTH`.

- [ ] **Step 2: Run to verify failure** — `make build-cmodel` → compile error (EjectAdapter/CreditRelay missing).

- [ ] **Step 3: Implement EjectAdapter + CreditRelay** (append to router_channel.hpp, in the namespace):
```cpp
// router LOCAL output -> NI. Implements the consumer-side NoC interfaces and is
// the router's downstream RouterLink. Buffer depth MUST equal the router's
// LOCAL-output credit seed so the void push_flit never overflows (credit gating
// is the only backpressure — see spec §4).
class EjectAdapter : public NocReqIn, public NocRspIn, public RouterLink {
  public:
    EjectAdapter(Router& router, std::size_t port, std::size_t depth)
        : router_(router), port_(port), depth_(depth) {}

    void push_flit(const Flit& flit) override {
        assert(queue_.size() < depth_ &&
               "EjectAdapter overflow: queue depth must equal the router LOCAL-output credit "
               "seed (credit gating should have prevented this)");
        queue_.push_back(flit);
    }
    std::optional<Flit> pop_flit() override {
        if (queue_.empty()) return std::nullopt;
        Flit f = queue_.front();
        queue_.pop_front();
        const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
        router_.receive_credit(port_, vc);   // return the LOCAL-output slot
        return f;
    }
    std::size_t buffered() const { return queue_.size(); }

  private:
    Router& router_;
    std::size_t port_;
    std::size_t depth_;
    std::deque<Flit> queue_;
};

// Forwards a downstream router's input-credit pulse to the upstream router's
// matching OUTPUT port. Registered on the downstream via set_upstream_credit.
class CreditRelay : public RouterCreditSink {
  public:
    CreditRelay(Router& upstream, std::size_t upstream_out_port)
        : upstream_(upstream), port_(upstream_out_port) {}
    void receive_credit(uint8_t vc) override { upstream_.receive_credit(port_, vc); }

  private:
    Router& upstream_;
    std::size_t port_;
};
```
Add `#include <cassert>` to the header.

- [ ] **Step 4: Run** — `make build-cmodel && ctest --test-dir build/cmodel -R "EjectAdapter|CreditRelay" --output-on-failure` → PASS.

- [ ] **Step 5: clang-format + commit**
```bash
/c/msys64/mingw64/bin/clang-format -i c_model/include/noc/router_channel.hpp c_model/tests/noc/test_router_channel.cpp
git add c_model/include/noc/router_channel.hpp c_model/tests/noc/test_router_channel.cpp
git commit -m "feat(noc): RouterChannel EjectAdapter + CreditRelay"
```

---

### Task 3: RouterChannel (4 routers + 8 adapters wired) + single-flit end-to-end

**Files:** Modify `c_model/include/noc/router_channel.hpp`, `c_model/tests/noc/test_router_channel.cpp`

- [ ] **Step 1: Write the failing end-to-end test**
```cpp
TEST(RouterChannel, SingleFlitReqEndToEnd) {
    SCENARIO("RouterChannel: a REQ flit injected at NMU(1,0) ejects at NSU(0,0) through 2 routers");
    using ni::cmodel::noc::RouterChannel;
    RouterChannel ch(/*num_vc=*/2);   // nodes (0,0) and (1,0), defaults from params
    // Node (1,0) NMU injects a request to dst=(0,0).
    Flit f = req_flit(/*dst=*/0x00, /*vc=*/0);
    f.set_header_field("src_id", 0x10);
    ASSERT_TRUE(ch.nmu_req_out(/*node=*/1).push_flit(f));
    // Drive enough ticks for: inject -> R(1,0) 3 stages -> link -> R(0,0) 3 stages -> eject.
    std::optional<Flit> got;
    for (int t = 0; t < 12 && !got; ++t) {
        ch.tick();
        got = ch.nsu_req_in(/*node=*/0).pop_flit();
    }
    ASSERT_TRUE(got.has_value()) << "request did not arrive at NSU(0,0)";
    EXPECT_EQ(got->get_header_field("dst_id"), 0x00u);
    EXPECT_EQ(got->get_header_field("src_id"), 0x10u);
}
```
(`req_flit` already exists from Task 1.)

- [ ] **Step 2: Run to verify failure** — compile error (RouterChannel missing).

- [ ] **Step 3: Implement RouterChannel** (append to router_channel.hpp). Node index: 0 = (0,0), 1 = (1,0). Mesh 2x1 (mesh_x_dim=2, mesh_y_dim=1). For each network (REQ, RSP) there are 2 routers; the inter-router link uses WEST/EAST: router(1).WEST ↔ router(0).EAST.
```cpp
class RouterChannel {
  public:
    static constexpr std::size_t kNodes = 2;
    static constexpr std::size_t LOCAL = static_cast<std::size_t>(RouterPort::LOCAL);
    static constexpr std::size_t EAST = static_cast<std::size_t>(RouterPort::EAST);
    static constexpr std::size_t WEST = static_cast<std::size_t>(RouterPort::WEST);

    explicit RouterChannel(uint8_t num_vc = NI_NOC_NUM_VC,
                           std::size_t vc_depth = NI_NOC_ROUTER_VC_DEPTH,
                           std::size_t out_fifo_depth = NI_NOC_ROUTER_OUTPUT_FIFO_DEPTH)
        : num_vc_(num_vc), vc_depth_(vc_depth) {
        for (std::size_t n = 0; n < kNodes; ++n) {
            RouterConfig c;
            c.x = static_cast<uint8_t>(n);  // node 0 -> (0,0), node 1 -> (1,0)
            c.y = 0;
            c.mesh_x_dim = 2;
            c.mesh_y_dim = 1;
            c.num_vc = num_vc;
            c.vc_depth = vc_depth;
            c.output_fifo_depth = out_fifo_depth;
            req_routers_.push_back(std::make_unique<Router>(c));
            rsp_routers_.push_back(std::make_unique<Router>(c));
        }
        // Per node, per network: inject at LOCAL input, eject at LOCAL output.
        // EjectAdapter buffer depth = LOCAL-output credit seed = vc_depth (spec §4).
        for (std::size_t n = 0; n < kNodes; ++n) {
            // REQ network: NMU injects req at LOCAL in; NSU receives req at LOCAL out.
            wire_local(*req_routers_[n], req_inject_[n], req_eject_[n]);
            // RSP network: NSU injects rsp at LOCAL in; NMU receives rsp at LOCAL out.
            wire_local(*rsp_routers_[n], rsp_inject_[n], rsp_eject_[n]);
        }
        // Inter-router full-duplex links (WEST/EAST) + credit relays, per network.
        wire_link(*req_routers_[1], *req_routers_[0], req_relay_10_, req_relay_01_);
        wire_link(*rsp_routers_[1], *rsp_routers_[0], rsp_relay_10_, rsp_relay_01_);
    }

    // NI-facing accessors (idx = node 0 or 1). NMU side = REQ inject + RSP eject.
    NocReqOut& nmu_req_out(std::size_t node) { return *req_inject_[node]; }
    NocRspIn&  nmu_rsp_in(std::size_t node) { return *rsp_eject_[node]; }
    // NSU side = REQ eject + RSP inject.
    NocReqIn&  nsu_req_in(std::size_t node) { return *req_eject_[node]; }
    NocRspOut& nsu_rsp_out(std::size_t node) { return *rsp_inject_[node]; }

    void tick() {
        // Cycle boundary: reset all inject landing guards first.
        for (auto& a : req_inject_) a->on_tick();
        for (auto& a : rsp_inject_) a->on_tick();
        // Tick all routers (each is evaluate-then-commit internally).
        for (auto& r : req_routers_) r->tick();
        for (auto& r : rsp_routers_) r->tick();
    }

  private:
    void wire_local(Router& r, std::unique_ptr<InjectAdapter>& inj,
                    std::unique_ptr<EjectAdapter>& ej) {
        inj = std::make_unique<InjectAdapter>(r, LOCAL, num_vc_, vc_depth_);
        ej = std::make_unique<EjectAdapter>(r, LOCAL, vc_depth_);
        r.set_upstream_credit(LOCAL, *inj);
        r.set_downstream(LOCAL, *ej);
    }
    // a is the EAST-of node (higher x, uses WEST to reach b); b is WEST-of (uses EAST).
    void wire_link(Router& a, Router& b, std::unique_ptr<CreditRelay>& relay_a,
                   std::unique_ptr<CreditRelay>& relay_b) {
        a.set_downstream(WEST, b.input(EAST));   // a.WEST_out -> b.EAST_in
        b.set_downstream(EAST, a.input(WEST));   // b.EAST_out -> a.WEST_in
        relay_a = std::make_unique<CreditRelay>(a, WEST);  // b.EAST_in credit -> a.WEST_out
        relay_b = std::make_unique<CreditRelay>(b, EAST);  // a.WEST_in credit -> b.EAST_out
        b.set_upstream_credit(EAST, *relay_a);
        a.set_upstream_credit(WEST, *relay_b);
    }

    uint8_t num_vc_;
    std::size_t vc_depth_;
    std::vector<std::unique_ptr<Router>> req_routers_, rsp_routers_;
    std::array<std::unique_ptr<InjectAdapter>, kNodes> req_inject_, rsp_inject_;
    std::array<std::unique_ptr<EjectAdapter>, kNodes> req_eject_, rsp_eject_;
    std::unique_ptr<CreditRelay> req_relay_10_, req_relay_01_, rsp_relay_10_, rsp_relay_01_;
};
```
Add `#include <array>` and `#include <memory>`. NOTE: `RouterLink&`/`RouterCreditSink&` are stored by reference inside `Router`, so the adapters must outlive the routers' use — `unique_ptr` members give stable addresses; do not store adapters in a reallocating vector. Construction order in the ctor body (after the routers exist) is fine because `set_downstream`/`set_upstream_credit` only store pointers.

- [ ] **Step 4: Run** — `make build-cmodel && ctest --test-dir build/cmodel -R "RouterChannel" --output-on-failure` → PASS. If the flit doesn't arrive, check the WEST/EAST wiring direction against spec §3 and the tick count (2 routers × 3 stages + link ≈ 7-8 ticks; the loop allows 12).

- [ ] **Step 5: clang-format + commit**
```bash
/c/msys64/mingw64/bin/clang-format -i c_model/include/noc/router_channel.hpp c_model/tests/noc/test_router_channel.cpp
git add c_model/include/noc/router_channel.hpp c_model/tests/noc/test_router_channel.cpp
git commit -m "feat(noc): RouterChannel wires 4 routers + 8 adapters; single-flit e2e"
```

---

### Task 4: Credit conservation + full backpressure unit tests

**Files:** Modify `c_model/tests/noc/test_router_channel.cpp`

- [ ] **Step 1: Write the tests**
```cpp
TEST(RouterChannel, FullBackpressureWhenConsumerStalls) {
    SCENARIO("RouterChannel: NSU never pops -> credit drains -> NMU inject backpressures (no assert)");
    using ni::cmodel::noc::RouterChannel;
    RouterChannel ch(/*num_vc=*/2);
    // Inject single-flit requests to dst=(0,0) and NEVER pop at NSU(0,0).
    int accepted = 0;
    for (int t = 0; t < 200; ++t) {
        Flit f = req_flit(0x00, 0);
        if (ch.nmu_req_out(1).push_flit(f)) ++accepted;
        ch.tick();
        // deliberately do NOT call ch.nsu_req_in(0).pop_flit()
    }
    // The path has finite buffering (router FIFOs + eject buffer + credits); once
    // it fills, push_flit must return false forever after. Assert it stopped
    // accepting (bounded) and never aborted (reaching here means no assert fired).
    EXPECT_GT(accepted, 0);
    EXPECT_LT(accepted, 200) << "with the consumer stalled, inject must eventually backpressure";
}

TEST(RouterChannel, CreditConservationAtRest) {
    SCENARIO("RouterChannel: credits return to full after a flit fully drains (no credit lost)");
    using ni::cmodel::noc::RouterChannel;
    RouterChannel ch(/*num_vc=*/2);
    // Send one flit end-to-end and pop it; then drive idle ticks. Inject must be
    // able to accept exactly vc_depth flits again (mirror restored).
    Flit f = req_flit(0x00, 0);
    ASSERT_TRUE(ch.nmu_req_out(1).push_flit(f));
    for (int t = 0; t < 12; ++t) { ch.tick(); ch.nsu_req_in(0).pop_flit(); }
    // After full drain + credit return, the inject mirror should be back to depth:
    // count how many we can push in fresh ticks before backpressure.
    int burst = 0;
    for (int t = 0; t < 8; ++t) {
        if (ch.nmu_req_out(1).push_flit(req_flit(0x00, 0))) ++burst;
        ch.tick();
        ch.nsu_req_in(0).pop_flit();
    }
    EXPECT_GT(burst, 0) << "credits were not returned after drain (mirror stuck empty)";
}
```
Note: keep these as behavioral bounds (accepted-then-backpressured; able-to-push-after-drain). A tight per-tick credit-sum invariant across 4 routers + relays + adapters is brittle to express from outside; the bounds above catch credit loss (stuck empty) and credit fabrication (never backpressures). If you can cleanly add an internal per-(network,link,vc) sum assertion using the router `credit()` + eject `buffered()` accessors, do so as a third test — but do NOT block on it.

- [ ] **Step 2: Run** — `make build-cmodel && ctest --test-dir build/cmodel -R "RouterChannel" --output-on-failure` → PASS. If `FullBackpressure` aborts (router assert) instead of backpressuring, the landing guard or the eject-depth==credit-seed invariant is wrong — STOP and report.

- [ ] **Step 3: clang-format + commit**
```bash
/c/msys64/mingw64/bin/clang-format -i c_model/tests/noc/test_router_channel.cpp
git add c_model/tests/noc/test_router_channel.cpp
git commit -m "test(noc): RouterChannel credit conservation + full backpressure"
```

---

### Task 5: Bidirectional loopback harness

**Files:**
- Create: `c_model/tests/integration/test_router_loopback.cpp`
- Modify: `c_model/tests/integration/CMakeLists.txt`

This adapts the existing single-flow loopback (`test_request_response_loopback.cpp`) to two full-NI nodes wired to one `RouterChannel`. READ that file first — its body (master/nmu/nsu construction, the response-shuttling between NSU `axi_master_port()` and the `AxiSlave`/`Memory`, the holdover deques, the `b_owner_nsu`/`r_owner_nsu` tracking, and the scoreboard wiring) is the pattern to instantiate TWICE.

- [ ] **Step 1: Factor the single-flow wiring into a reusable local struct**

In the new file, define a `Flow` struct that owns one direction's endpoints, mirroring the existing test's per-flow objects: an `nmu::Nmu`, an `axi::AxiMasterT<nmu::AxiSlavePort>`, an `nsu::Nsu`, the backing `AxiSlave`/`Memory`, an `axi::Scoreboard`, and the holdover/owner bookkeeping. Its ctor takes the `RouterChannel` NI-facing refs for its node (e.g. flow (1,0)->(0,0): NMU at node 1 via `ch.nmu_req_out(1)`/`ch.nmu_rsp_in(1)`, NSU at node 0 via `ch.nsu_req_in(0)`/`ch.nsu_rsp_out(0)`), the scenario YAML path, and `num_vc`. Wire the scoreboard callbacks exactly as the existing test does (`master.on_write_completed`/`on_read_observed` → `sb.handle_*`). Provide `tick()` (calls master/nmu/nsu tick + runs that flow's response shuttle + holdovers) and `done()`/`mismatches()` accessors.

Copy the response-shuttle + holdover + owner-tracking logic verbatim from `test_request_response_loopback.cpp` (the `while (auto aw = port.pop_aw())` block and the B/R return path) into `Flow::tick()`, scoped to this flow's single NSU. Do not invent new logic — it is the same single-NSU shuttle the existing test already validates.

- [ ] **Step 2: Write the parameterized bidirectional test**
```cpp
class RouterLoopbackParam : public ::testing::TestWithParam<std::size_t> {};  // num_vc

TEST_P(RouterLoopbackParam, BidirectionalZeroMismatch) {
    SCENARIO("RouterChannel: two simultaneous flows (1,0)<->(0,0), both scoreboards clean");
    const std::size_t num_vc = GetParam();
    ni::cmodel::noc::RouterChannel ch(static_cast<uint8_t>(num_vc));
    // Flow A: master at node 1, slave at node 0; scenario addresses map to dst=(0,0).
    Flow flow_a(ch, /*master_node=*/1, /*slave_node=*/0,
                scenario_path("AX4-BAS-003_single_write_read_aligned"), num_vc);
    // Flow B: master at node 0, slave at node 1; scenario addresses map to dst=(1,0).
    // Use an address-offset variant so xy_route yields dst_x=1 (high X bit set).
    Flow flow_b(ch, /*master_node=*/0, /*slave_node=*/1,
                scenario_path("AX4-BAS-003_single_write_read_aligned"), num_vc,
                /*addr_dst_x=*/1);
    std::size_t cycle = 0;
    while ((!flow_a.done() || !flow_b.done()) && cycle < 100000) {
        flow_a.pre_tick();   // master/nmu/nsu tick that push into the channel
        flow_b.pre_tick();
        ch.tick();           // advance the shared fabric once
        flow_a.post_tick();  // drain channel -> nsu, run response shuttle
        flow_b.post_tick();
        ++cycle;
    }
    EXPECT_TRUE(flow_a.done()) << "flow A did not complete";
    EXPECT_TRUE(flow_b.done()) << "flow B did not complete";
    EXPECT_EQ(flow_a.mismatches(), 0u);
    EXPECT_EQ(flow_b.mismatches(), 0u);
}

INSTANTIATE_TEST_SUITE_P(NumVc, RouterLoopbackParam, ::testing::Values(1, 2, 4, 8));
```
Note the tick structure: split each flow's `tick()` into `pre_tick()` (the producer side — master.tick/nmu.tick/nsu.tick that PUSH into the channel) and `post_tick()` (the consumer side — drain channel to NSU, response shuttle), with the single `ch.tick()` between them. This honors the §5 tick-boundary contract (each inject sees at most one push between `ch.tick()` calls). If the existing single-flow test does not separate producer/consumer cleanly, adopt the simplest ordering that keeps one push-per-inject-per-cycle and document it; if zero mismatch cannot be reached because the existing shuttle assumes a specific tick order, report NEEDS_CONTEXT with the specific ordering conflict rather than weakening the scoreboard.

For Flow B's address→dst mapping: `nmu::addr_trans::xy_route` puts X in bits `[LOCAL_ADDR_BITS + X_WIDTH - 1 : LOCAL_ADDR_BITS]` (LOCAL_ADDR_BITS=16). To make a scenario route to dst_x=1, the flow must offset transaction addresses by `1 << 16` (0x10000). Provide that offset in `Flow` (the `addr_dst_x` param) by adding `addr_dst_x << 16` to each scenario address before injecting — OR, simpler, author/point Flow B at scenario addresses already in the 0x10000+ range. Pick whichever the existing scenario/master injection path supports cleanly; if address rewriting is not exposed, use a scenario whose addresses already land in dst=(1,0).

Add to `c_model/tests/integration/CMakeLists.txt`:
```cmake
add_cmodel_test(test_router_loopback)
target_include_directories(test_router_loopback PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

- [ ] **Step 3: Build + run**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R "RouterLoopback" --output-on-failure`. Expected: 4 instantiations (num_vc 1/2/4/8) PASS, both scoreboards zero mismatch. If a flow deadlocks (cycle hits 100000), the credit loop or tick ordering is wrong — STOP and report with which flow stalled and the channel introspection (`output_fifo_size`, eject `buffered()`).

- [ ] **Step 4: Full suite + commit**

Run: `make test` — all green. Then:
```bash
/c/msys64/mingw64/bin/clang-format -i c_model/tests/integration/test_router_loopback.cpp
git add c_model/tests/integration/test_router_loopback.cpp c_model/tests/integration/CMakeLists.txt
git commit -m "test(noc): bidirectional RouterChannel loopback over num_vc grid"
```

---

## Out of scope (later rounds)

- Multi-hop / NxM mesh / >2 NSU per channel; per-NSU latency/route programming.
- Cosim wrapper (sub-project C: RouterChannelShellAdapter + tb_top).
- Reset (router "construction = reset" stands).
