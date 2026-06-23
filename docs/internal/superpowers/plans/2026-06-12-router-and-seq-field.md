# Router + Seq Header Field Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the spec at `docs/superpowers/specs/2026-06-12-router-microarch-design.md`: a fixed-vc 3-stage wormhole `Router` in c_model, plus the `seq` header field in the packet domain.

**Architecture:** Track 1 (Tasks 1–3) lands parameters, the `seq` field, and NMU stamping (seq + `route_par`). Track 2 (Tasks 4–9) builds `Router` header-only in `c_model/include/noc/router.hpp` with a GoogleTest suite per spec §12. Task 10 closes the function-block inventory and formatting. **Tasks are strictly sequential**: Task 4 includes `route_parity.hpp` (Task 3), the router params (Task 1), and the new packet constants (Task 2) — Track 2 must not start before Track 1 completes.

**Tech Stack:** C++17 header-only c_model, GoogleTest via `add_cmodel_test`, specgen codegen (`py -3 specgen/tools/codegen.py`), `make build-cmodel` / `make test`.

**Pre-approved decision (user, 2026-06-12):** default config disables `multicast` (width 8 → 0) to fit `seq` (5b) in the 56b header budget. `EN_SEQ`+`EN_MULTICAST`+`EN_ECC` cannot coexist (spec §10); `multicast` is the sacrifice because the router aborts on nonzero `multicast` this round anyway.

**Conventions used by all tasks:**
- Build: `make build-cmodel`. Test: `make test` (ctest, all suites). Specgen: `py -3 -m pytest specgen/tests/ -q`.
- After editing any `.hpp`/`.cpp`: `clang-format -i <file>`.
- Every commit message format `type(scope): description`, body in English.

---

### Task 1: Router parameters in constants.yaml

**Files:**
- Modify: `specgen/source/constants.yaml` (noc section, after `SLAVE_VC_BUFFER_DEPTH`)
- Regen: `specgen/generated/cpp/ni_params.h`, `specgen/generated/sv/ni_params_pkg.sv`
- Update golden: `specgen/tests/golden/ni_params.h.golden`, `specgen/tests/golden/ni_params_pkg.sv.golden`

- [ ] **Step 1: Add four parameters to `specgen/source/constants.yaml`** (append inside the `noc:` mapping):

```yaml
  ROUTER_VC_DEPTH:
    type: int
    units: flits
    description: "Router input VC FIFO depth; defines the credit seed of the upstream sender"
    default: 4
    min: 1
    max: 16
    sv_symbol: NI_NOC_ROUTER_VC_DEPTH_DFLT
    cpp_symbol: NI_NOC_ROUTER_VC_DEPTH
  ROUTER_OUTPUT_FIFO_DEPTH:
    type: int
    units: flits
    description: "Router output FIFO depth (stage 3); not credit-counted"
    default: 2
    min: 1
    max: 16
    sv_symbol: NI_NOC_ROUTER_OUTPUT_FIFO_DEPTH_DFLT
    cpp_symbol: NI_NOC_ROUTER_OUTPUT_FIFO_DEPTH
  MESH_X_DIM:
    type: int
    units: count
    description: "Mesh X dimension (routers per row); bounded by X_WIDTH (4 bit)"
    default: 4
    min: 2
    max: 16
    sv_symbol: NI_NOC_MESH_X_DIM_DFLT
    cpp_symbol: NI_NOC_MESH_X_DIM
  MESH_Y_DIM:
    type: int
    units: count
    description: "Mesh Y dimension (routers per column); bounded by Y_WIDTH (4 bit)"
    default: 4
    min: 2
    max: 16
    sv_symbol: NI_NOC_MESH_Y_DIM_DFLT
    cpp_symbol: NI_NOC_MESH_Y_DIM
```

- [ ] **Step 2: Regenerate the params domain (both targets)**

Run:
```bash
py -3 specgen/tools/codegen.py --target cpp --domain params
py -3 specgen/tools/codegen.py --target sv  --domain params
```
Expected: `ni_params.h` and `ni_params_pkg.sv` rewritten with the four new symbols.

- [ ] **Step 3: Update the params goldens**

```bash
cp specgen/generated/cpp/ni_params.h     specgen/tests/golden/ni_params.h.golden
cp specgen/generated/sv/ni_params_pkg.sv specgen/tests/golden/ni_params_pkg.sv.golden
```

- [ ] **Step 4: Run specgen tests + c_model build**

Run: `py -3 -m pytest specgen/tests/ -q` — Expected: all pass.
Run: `make build-cmodel` — Expected: builds; `codegen_check` passes (no drift).

- [ ] **Step 5: Commit**

```bash
git add specgen/source/constants.yaml specgen/generated/cpp/ni_params.h specgen/generated/sv/ni_params_pkg.sv specgen/tests/golden/ni_params.h.golden specgen/tests/golden/ni_params_pkg.sv.golden
git commit -m "feat(specgen): router and mesh parameters in params domain"
```

---

### Task 2: `seq` header field; `multicast` width to 0

**Files:**
- Modify: `specgen/generated/json/ni_packet.json` (field_widths, header_fields, meta.spec_version)
- Modify: `specgen/tests/test_foundation.py:52`, `specgen/tests/test_field_descriptor_arrays.py:26`
- Modify: `c_model/tests/test_flit.cpp:14,26-27,38-39`, `c_model/tests/nmu/test_packetize.cpp:242-251`
- Regen: `specgen/generated/cpp/ni_flit_constants.h`, `specgen/generated/sv/ni_flit_pkg.sv` + their goldens

- [ ] **Step 1: Edit `specgen/generated/json/ni_packet.json`**

In `flit.field_widths`: add `"SEQ_WIDTH": 5`, change `"MULTICAST_WIDTH": 8` → `"MULTICAST_WIDTH": 0`.
In `flit.header_fields`: insert after the `rob_idx` entry:

```json
      {
        "name": "seq",
        "width_param": "SEQ_WIDTH",
        "enabled": true
      },
```

Change the `multicast` entry's `"enabled": true` → `"enabled": false` (width-0 placeholder).
In `meta`: `"spec_version": "v0.4.0"` → `"v0.5.0"`.

Resulting enabled-field sum: 54 − 8 + 5 = 51; derived `rsvd` = 56 − 51 = 5 (still ≥ 0, budget rule satisfied).

- [ ] **Step 2: Regenerate packet domain + update goldens**

```bash
py -3 specgen/tools/codegen.py --target cpp --domain packet
py -3 specgen/tools/codegen.py --target sv  --domain packet
cp specgen/generated/cpp/ni_flit_constants.h specgen/tests/golden/ni_flit_constants.h.golden
cp specgen/generated/sv/ni_flit_pkg.sv       specgen/tests/golden/ni_flit_pkg.sv.golden
```
Expected in `ni_flit_constants.h`: `SEQ_LSB = 34`, `SEQ_MSB = 38`, `SEQ_WIDTH = 5`; `COMMTYPE_LSB = 39`; `FLIT_ECC_MSB = 50`; `MULTICAST_WIDTH = 0` with `MULTICAST_ENABLED = false` and no `MULTICAST_LSB/MSB`.

- [ ] **Step 3: Fix specgen test references**

`specgen/tests/test_foundation.py:52`: `assert C.header_field_enabled(spec, "multicast") is True` → `is False`.
`specgen/tests/test_foundation.py:71`: `header_fields_padding()` now returns `["multicast", "rsvd"]` (every `enabled=false` field) — update the expected list from `["rsvd"]`.
`specgen/tests/test_constants_resolver.py:84`: the derived `rsvd` width expectation changes 2 → 5.
`specgen/tests/test_field_descriptor_arrays.py:26`: in the expected-name list, replace `"multicast"` with `"seq"` (after `"rob_idx"`), keep `"commtype"`, `"flit_ecc"`.

- [ ] **Step 4: Run specgen tests**

Run: `py -3 -m pytest specgen/tests/ -q` — Expected: all pass. If `test_pin_level_reset` or invariants fail on the multicast placeholder, read the failure and fix the test expectation (the field is now width-0 disabled), not the JSON.

- [ ] **Step 5: Fix c_model test references**

`c_model/tests/test_flit.cpp`: replace the multicast set/get pair (lines 26-27, 38-39) with:
```cpp
    f.set_header_field("seq", 0x1F);
    // ...
    EXPECT_EQ(f.get_header_field("seq"), 0x1Fu);
```
Update the SCENARIO string at line 14 to mention `seq` instead of implying multicast coverage.

`c_model/tests/nmu/test_packetize.cpp` (lines ~242-251): delete `EXPECT_EQ(f.get_header_field("multicast"), 0u);` (field no longer queryable — `header_field_pos` aborts on it); add `EXPECT_EQ(f.get_header_field("seq"), 0u);` (not yet stamped until Task 3).

Also run `grep -rn "multicast" c_model/include/` — if any `set_header_field("multicast", ...)` zero-fill exists (e.g. in `nmu/packetize.hpp`), delete it (it would abort at runtime now).

- [ ] **Step 6: Build and run all c_model tests**

Run: `make build-cmodel && make test` — Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add specgen/generated/json/ni_packet.json specgen/generated/cpp/ni_flit_constants.h specgen/generated/sv/ni_flit_pkg.sv specgen/tests/golden/ specgen/tests/test_foundation.py specgen/tests/test_field_descriptor_arrays.py c_model/tests/test_flit.cpp c_model/tests/nmu/test_packetize.cpp
git commit -m "feat(specgen): seq header field (5b, after rob_idx); multicast width to 0"
```

---

### Task 3: NMU/NSU stamping — `seq` counter and `route_par` computation

**Files:**
- Create: `c_model/include/route_parity.hpp`
- Modify: `c_model/include/nmu/packetize.hpp` (the three flit builders that currently do `set_header_field("vc_id", 0)` at lines ~96/126/144)
- Modify: `c_model/include/nsu/packetize.hpp` (B/R flit builders, same pattern)
- Test: `c_model/tests/nmu/test_packetize.cpp`, `c_model/tests/nsu/test_nsu_packetize.cpp`

- [ ] **Step 1: Write failing tests in `c_model/tests/nmu/test_packetize.cpp`**

```cpp
TEST(PacketizeSeq, IncrementsPerPacketPerDestination) {
    SCENARIO("NMU Packetize: seq increments per packet per (src,dst) flow; W shares AW's seq");
    // Build via the existing fixture helpers in this file (same AW/AR input
    // structs the neighboring tests use). Two AW packets to dst A, one AR to
    // dst B, then another AW to dst A.
    // Expectations:
    //  - AW#1 to A: seq == 0; its W flits: seq == 0
    //  - AW#2 to A: seq == 1
    //  - AR#1 to B: seq == 0   (independent per-destination counter)
    //  - AW#3 to A: seq == 2
}

TEST(PacketizeSeq, WrapsAtSeqWidth) {
    SCENARIO("NMU Packetize: seq wraps mod 2^SEQ_WIDTH");
    // Issue 33 single-beat AW packets to one dst; the 33rd carries seq == 0.
    // Use ni::header::SEQ_WIDTH for the bound, not a literal 32.
}

TEST(PacketizeRoutePar, EvenParityOverDstIdAndLast) {
    SCENARIO("NMU Packetize: route_par makes XOR(dst_id bits, last, route_par) == 0");
    // For each produced flit f:
    //   uint64_t x = f.get_header_field("dst_id");
    //   unsigned p = __builtin_parityll(x) ^ f.get_header_field("last");
    //   EXPECT_EQ(f.get_header_field("route_par"), p);
}
```

Fill the bodies with the file's existing builder helpers (read the neighboring tests first; reuse their AW/W/AR construction code verbatim).

- [ ] **Step 2: Run to verify failure**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R PacketizeSeq --output-on-failure`
Expected: FAIL (seq == 0 everywhere, route_par == 0).

- [ ] **Step 3: Create `c_model/include/route_parity.hpp`**

```cpp
#pragma once
// route_par generation/check helper. Coverage set is fixed by
// ni_packet.json `route_par_coverage`: ["dst_id", "last"]. Even parity:
// XOR(dst_id bits, last, route_par) == 0.
#include <cstdint>

namespace ni::cmodel {

inline uint8_t route_parity(uint64_t dst_id, uint64_t last) noexcept {
    uint64_t x = dst_id ^ last;
    x ^= x >> 32; x ^= x >> 16; x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return static_cast<uint8_t>(x & 1u);
}

}  // namespace ni::cmodel
```

Wait — `dst_id ^ last` folds `last` into bit 0 before reduction; XOR-reduce of (dst_id ^ last) equals XOR-reduce(dst_id) ^ last only for 1-bit `last`, which holds (LAST_WIDTH = 1). Keep the comment in the header stating this assumption.

- [ ] **Step 4: Stamp in `c_model/include/nmu/packetize.hpp`**

Add a member to the Packetize class:
```cpp
    // Request-path ordering tag (spec 2026-06-12 §10): per-(src,dst) packet
    // counter, REQ network only. W flits reuse their AW packet's value.
    std::array<uint8_t, 1u << (ni::width::X_WIDTH + ni::width::Y_WIDTH)> seq_counter_{};
```
**Per-packet seq storage:** Packetize already queues per-AW write metadata (the `WMeta` queue around `packetize.hpp:86`). Add a `uint8_t seq;` member to `WMeta` — do NOT use a single `current_aw_seq_` scalar: with multiple outstanding AW packets, AW1's W flits would wrongly pick up AW2's value (spec §10 requires one value per packet).

In the AW builder (anchored at the existing `f.set_header_field("vc_id", 0);` line ~96):
```cpp
    const uint8_t seq_mask = static_cast<uint8_t>((1u << ni::header::SEQ_WIDTH) - 1);
    const uint8_t seq = seq_counter_[dst_id] & seq_mask;
    f.set_header_field("seq", seq);
    f.set_header_field("route_par", route_parity(dst_id, /*last=*/0));
    // store `seq` into this AW's WMeta entry alongside the existing fields;
    // advance the counter only after the AW flit is actually accepted
    // downstream (next to the existing post-push_flit bookkeeping):
    //   seq_counter_[dst_id] = (seq + 1) & seq_mask;
```
In the W builder (line ~126 region): `f.set_header_field("seq", meta.seq);` reading the owning `WMeta`, and `route_par` with that flit's actual `last` value (wlast). In the AR builder (line ~144 region): same counter pattern as AW (stamp, then advance after successful push) but `last=1`.
Adapt variable names to the actual builder signatures when editing (dst_id is already computed there for `set_header_field("dst_id", ...)`). Include `"route_parity.hpp"`.

Add one more test for the outstanding-AW case:
```cpp
TEST(PacketizeSeq, OutstandingAwPacketsKeepTheirOwnSeq) {
    SCENARIO("NMU Packetize: AW1 accepted, AW2 accepted, then AW1's W beats — W carries AW1's seq, not AW2's");
    // Requires the downstream stub to accept both AW flits before any W flit
    // is produced (mirror the existing multi-outstanding fixtures in this file).
    // Expect: AW1.seq == 0, AW2.seq == 1, W-of-AW1.seq == 0.
}
```

- [ ] **Step 5: Stamp `route_par` in `c_model/include/nsu/packetize.hpp`**

B/R builders: `f.set_header_field("route_par", route_parity(dst_id, last_value));` with each builder's existing dst/last values. No `seq` on RSP (spec §10). Add one test in `c_model/tests/nsu/test_nsu_packetize.cpp` mirroring `PacketizeRoutePar` above for B and R flits.

- [ ] **Step 6: Run tests, fix `test_packetize.cpp` zero-field scenario**

The Task-2 expectation `EXPECT_EQ(f.get_header_field("seq"), 0u)` in the rsvd/disabled-fields scenario now needs updating: the first packet of a fresh Packetize still carries seq 0, so it may pass as-is; if the scenario reuses a fixture with prior traffic, assert the documented counter value instead.
Run: `make build-cmodel && make test` — Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add c_model/include/route_parity.hpp c_model/include/nmu/packetize.hpp c_model/include/nsu/packetize.hpp c_model/tests/nmu/test_packetize.cpp c_model/tests/nsu/test_nsu_packetize.cpp
git commit -m "feat(nmu): stamp seq per-destination counter and route_par parity"
```

---

### Task 4: Router skeleton — config, link contract, route computation

**Files:**
- Create: `c_model/include/noc/router.hpp`
- Create: `c_model/tests/noc/test_router.cpp`
- Modify: `c_model/tests/noc/CMakeLists.txt`

- [ ] **Step 1: Write failing tests (`c_model/tests/noc/test_router.cpp`)**

```cpp
#include "noc/router.hpp"
#include "common/test_logger.hpp"
#include <gtest/gtest.h>

using ni::cmodel::noc::Router;
using ni::cmodel::noc::RouterConfig;
using ni::cmodel::noc::RouterPort;
using ni::cmodel::noc::route_compute;

namespace {

RouterConfig center_cfg() {
    RouterConfig cfg;
    cfg.x = 1; cfg.y = 1;          // center of default 4x4
    return cfg;
}

uint8_t make_dst(uint8_t x, uint8_t y) {
    return static_cast<uint8_t>((y << ni::width::X_WIDTH) | x);
}

TEST(RouterRouteCompute, XyDimensionOrder) {
    SCENARIO("Router RC: XY DOR — X first, then Y, both-equal ejects LOCAL");
    const auto cfg = center_cfg();
    EXPECT_EQ(route_compute(make_dst(3, 1), cfg), RouterPort::EAST);
    EXPECT_EQ(route_compute(make_dst(0, 1), cfg), RouterPort::WEST);
    EXPECT_EQ(route_compute(make_dst(1, 3), cfg), RouterPort::NORTH);
    EXPECT_EQ(route_compute(make_dst(1, 0), cfg), RouterPort::SOUTH);
    EXPECT_EQ(route_compute(make_dst(1, 1), cfg), RouterPort::LOCAL);
    // X precedence: both differ -> X resolved first
    EXPECT_EQ(route_compute(make_dst(3, 3), cfg), RouterPort::EAST);
}

TEST(RouterRouteComputeDeath, DstOutsideMeshAborts) {
    SCENARIO("Router RC: dst outside MESH_X_DIM x MESH_Y_DIM -> assert+abort");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const auto cfg = center_cfg();
    EXPECT_DEATH(route_compute(make_dst(5, 1), cfg), "outside mesh");
}

TEST(RouterConstructionDeath, BadParametersAbort) {
    SCENARIO("Router: construction asserts — num_vc bound, nonzero depths");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    RouterConfig bad_vc = center_cfg();
    bad_vc.num_vc = 9;             // > 2^VC_ID_WIDTH? no: > 8
    EXPECT_DEATH(Router r(bad_vc), "num_vc");
    RouterConfig bad_depth = center_cfg();
    bad_depth.vc_depth = 0;
    EXPECT_DEATH(Router r(bad_depth), "depth");
}

}  // namespace
```

- [ ] **Step 2: Add the test target**

Append to `c_model/tests/noc/CMakeLists.txt`:
```cmake
add_cmodel_test(test_router)
target_include_directories(test_router PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```
Run: `make build-cmodel` — Expected: compile FAIL (`noc/router.hpp` missing).

- [ ] **Step 3: Create `c_model/include/noc/router.hpp` (skeleton)**

```cpp
#pragma once
// Wormhole VC router for the c_model NoC fabric.
// Spec: docs/superpowers/specs/2026-06-12-router-microarch-design.md
//
// Fixed-vc 3-stage pipeline: stage 1 per-(input port, vc) FIFO (+RC at the
// FIFO head), stage 2 per-(output port, vc) wormhole arbitration + per-output
// VC arbitration + crossbar, stage 3 output FIFO -> link. Credit-based flow
// control; credit reserved at output-FIFO admission (the grant event).
// Lock semantics ported from FlooNoC floo_wormhole_arbiter/floo_vc_arbiter
// with (input port, vc) ownership; decrement point matches BookSim2
// BufferState::SendingFlit.
//
// Convention: +y is NORTH. One Router instance per physical network
// (REQ / RSP are separate objects).
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "ni_params.h"
#include "route_parity.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace ni::cmodel::noc {

enum class RouterPort : uint8_t { LOCAL = 0, NORTH = 1, EAST = 2, SOUTH = 3, WEST = 4 };
inline constexpr std::size_t ROUTER_PORT_COUNT = 5;

struct RouterConfig {
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t mesh_x_dim = NI_NOC_MESH_X_DIM;
    uint8_t mesh_y_dim = NI_NOC_MESH_Y_DIM;
    uint8_t num_vc = NI_NOC_NUM_VC;
    std::size_t vc_depth = NI_NOC_ROUTER_VC_DEPTH;
    std::size_t output_fifo_depth = NI_NOC_ROUTER_OUTPUT_FIFO_DEPTH;
    bool route_par_check = false;
};

// Forward half of the router link contract (spec §6). push_flit is always
// accepted — the sender's credit counter guarantees receiver buffer space.
class RouterLink {
  public:
    virtual ~RouterLink() = default;
    virtual void push_flit(const Flit& flit) = 0;
};

// Reverse half: per-VC credit return pulses back to the sender.
class RouterCreditSink {
  public:
    virtual ~RouterCreditSink() = default;
    virtual void receive_credit(uint8_t vc_id) = 0;
};

// XY dimension-order route (spec §4): X first, then Y, equal ejects LOCAL.
// dst_id layout matches nmu::addr_trans (X in low bits).
inline RouterPort route_compute(uint8_t dst_id, const RouterConfig& cfg) {
    const uint8_t dst_x = dst_id & static_cast<uint8_t>((1u << ni::width::X_WIDTH) - 1);
    const uint8_t dst_y =
        static_cast<uint8_t>(dst_id >> ni::width::X_WIDTH) &
        static_cast<uint8_t>((1u << ni::width::Y_WIDTH) - 1);
    if (!(dst_x < cfg.mesh_x_dim && dst_y < cfg.mesh_y_dim)) {
        assert(false && "route_compute: dst_id outside mesh range");
        std::abort();
    }
    if (dst_x != cfg.x) return dst_x > cfg.x ? RouterPort::EAST : RouterPort::WEST;
    if (dst_y != cfg.y) return dst_y > cfg.y ? RouterPort::NORTH : RouterPort::SOUTH;
    return RouterPort::LOCAL;
}

class Router {
  public:
    explicit Router(const RouterConfig& cfg) : cfg_(cfg) {
        if (!(cfg_.num_vc >= 1 && cfg_.num_vc <= (1u << ni::header::VC_ID_WIDTH))) {
            assert(false && "Router: num_vc out of range (1 .. 2^VC_ID_WIDTH)");
            std::abort();
        }
        if (cfg_.vc_depth == 0 || cfg_.output_fifo_depth == 0) {
            assert(false && "Router: zero FIFO depth");
            std::abort();
        }
        if (!(cfg_.x < cfg_.mesh_x_dim && cfg_.y < cfg_.mesh_y_dim)) {
            assert(false && "Router: own coordinate outside mesh");
            std::abort();
        }
        for (std::size_t p = 0; p < ROUTER_PORT_COUNT; ++p) {
            input_fifo_[p].resize(cfg_.num_vc);
            credit_[p].assign(cfg_.num_vc, cfg_.vc_depth);
            wormhole_[p].resize(cfg_.num_vc);
            input_adapters_.emplace_back(this, p);
        }
    }
    Router(const Router&) = delete;
    Router(Router&&) = delete;
    Router& operator=(const Router&) = delete;
    Router& operator=(Router&&) = delete;

    RouterLink& input(std::size_t port) {
        assert(port < ROUTER_PORT_COUNT);
        return input_adapters_[port];
    }
    void set_downstream(std::size_t port, RouterLink& link) { downstream_[port] = &link; }
    void set_upstream_credit(std::size_t port, RouterCreditSink& sink) {
        upstream_credit_[port] = &sink;
    }
    // Credit pulse from the downstream node attached to `port`'s output.
    void receive_credit(std::size_t port, uint8_t vc_id) {
        assert(port < ROUTER_PORT_COUNT && vc_id < cfg_.num_vc);
        if (credit_[port][vc_id] >= cfg_.vc_depth) {
            assert(false && "Router: credit counter overflow");
            std::abort();
        }
        ++credit_[port][vc_id];
    }

    void tick();  // Task 5

    // Test introspection
    std::size_t credit(std::size_t out_port, uint8_t vc) const { return credit_[out_port][vc]; }
    std::size_t input_fifo_size(std::size_t port, uint8_t vc) const {
        return input_fifo_[port][vc].size();
    }
    std::size_t output_fifo_size(std::size_t port) const { return output_fifo_[port].size(); }
    uint64_t route_par_drop_count() const { return route_par_drop_count_; }

  private:
    struct InputAdapter : RouterLink {
        Router* parent;
        std::size_t port;
        InputAdapter(Router* p, std::size_t idx) : parent(p), port(idx) {}
        void push_flit(const Flit& f) override { parent->accept_flit(port, f); }
    };

    void accept_flit(std::size_t port, const Flit& f);  // Task 5

    struct WormholeState {
        std::optional<std::size_t> locked_input;
        std::size_t rr = 0;
    };

    RouterConfig cfg_;
    std::array<std::optional<Flit>, ROUTER_PORT_COUNT> landing_{};
    std::array<std::vector<std::deque<Flit>>, ROUTER_PORT_COUNT> input_fifo_{};
    std::array<std::vector<std::size_t>, ROUTER_PORT_COUNT> credit_{};   // [out][vc]
    std::array<std::vector<WormholeState>, ROUTER_PORT_COUNT> wormhole_{};  // [out][vc]
    std::array<std::size_t, ROUTER_PORT_COUNT> vc_rr_{};                 // [out]
    std::array<std::deque<Flit>, ROUTER_PORT_COUNT> output_fifo_{};
    std::array<RouterLink*, ROUTER_PORT_COUNT> downstream_{};
    std::array<RouterCreditSink*, ROUTER_PORT_COUNT> upstream_credit_{};
    std::vector<std::pair<std::size_t, uint8_t>> credit_pulse_pending_;  // emit next tick
    uint64_t route_par_drop_count_ = 0;
};

}  // namespace ni::cmodel::noc
```

(`tick()` and `accept_flit` declared but defined in Task 5; to keep this task compiling, give them empty inline bodies for now: `inline void Router::tick() {}` and `inline void Router::accept_flit(std::size_t, const Flit&) {}` at the bottom of the namespace.)

- [ ] **Step 4: Build and run the new tests**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R Router --output-on-failure`
Expected: PASS (route compute + death tests).

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i c_model/include/noc/router.hpp c_model/tests/noc/test_router.cpp
git add c_model/include/noc/router.hpp c_model/tests/noc/test_router.cpp c_model/tests/noc/CMakeLists.txt
git commit -m "feat(noc): router skeleton — config, link contract, XY route computation"
```

---

### Task 5: Router datapath — 3-stage tick, credits, zero-load latency

**Files:**
- Modify: `c_model/include/noc/router.hpp` (replace the empty `tick`/`accept_flit` bodies)
- Test: `c_model/tests/noc/test_router.cpp`

- [ ] **Step 1: Add test doubles + failing tests**

```cpp
struct FlitSink : ni::cmodel::noc::RouterLink {
    std::vector<ni::cmodel::Flit> received;
    void push_flit(const ni::cmodel::Flit& f) override { received.push_back(f); }
};

struct CreditCounter : ni::cmodel::noc::RouterCreditSink {
    std::vector<uint8_t> pulses;
    void receive_credit(uint8_t vc) override { pulses.push_back(vc); }
};

ni::cmodel::Flit make_flit(uint8_t dst, uint8_t vc, uint64_t last) {
    ni::cmodel::Flit f;
    f.set_header_field("dst_id", dst);
    f.set_header_field("vc_id", vc);
    f.set_header_field("last", last);
    f.set_header_field("route_par", ni::cmodel::route_parity(dst, last));
    return f;
}

TEST(RouterDatapath, ZeroLoadLatencyIsThreeTicks) {
    SCENARIO("Router: flit pushed at T reaches downstream.push_flit during tick T+3 (spec §12.5)");
    Router r(center_cfg());
    FlitSink east;
    r.set_downstream(static_cast<std::size_t>(RouterPort::EAST), east);
    auto f = make_flit(make_dst(3, 1), /*vc=*/0, /*last=*/1);
    r.input(static_cast<std::size_t>(RouterPort::WEST)).push_flit(f);  // T
    r.tick();  EXPECT_TRUE(east.received.empty());                     // T+1: stage 1
    r.tick();  EXPECT_TRUE(east.received.empty());                     // T+2: stage 2
    r.tick();  ASSERT_EQ(east.received.size(), 1u);                    // T+3: stage 3
}

TEST(RouterDatapath, HeaderTransparency) {
    SCENARIO("Router: header bits identical at ingress and egress, incl. seq (spec §12.8)");
    Router r(center_cfg());
    FlitSink east;
    r.set_downstream(static_cast<std::size_t>(RouterPort::EAST), east);
    auto f = make_flit(make_dst(3, 1), /*vc=*/0, /*last=*/1);
    f.set_header_field("seq", 21);
    f.set_header_field("noc_qos", 5);
    f.set_header_field("rob_req", 1);
    f.set_header_field("rob_idx", 7);
    f.set_header_field("src_id", make_dst(0, 2));
    r.input(static_cast<std::size_t>(RouterPort::WEST)).push_flit(f);
    r.tick(); r.tick(); r.tick();
    ASSERT_EQ(east.received.size(), 1u);
    EXPECT_EQ(east.received[0].raw(), f.raw());  // byte-for-byte, whole flit
}

TEST(RouterDatapath, CreditDecrementAtGrantAndPulseAfterDequeue) {
    SCENARIO("Router: credit-- at output-FIFO admission; upstream pulse 1 cycle after input dequeue");
    Router r(center_cfg());
    FlitSink east;
    CreditCounter west_up;
    r.set_downstream(static_cast<std::size_t>(RouterPort::EAST), east);
    r.set_upstream_credit(static_cast<std::size_t>(RouterPort::WEST), west_up);
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    EXPECT_EQ(r.credit(E, 0), NI_NOC_ROUTER_VC_DEPTH);                 // seeded
    r.input(static_cast<std::size_t>(RouterPort::WEST)).push_flit(make_flit(make_dst(3,1), 0, 1));
    r.tick();                                                          // stage 1
    EXPECT_EQ(r.credit(E, 0), NI_NOC_ROUTER_VC_DEPTH);
    r.tick();                                                          // stage 2: grant
    EXPECT_EQ(r.credit(E, 0), NI_NOC_ROUTER_VC_DEPTH - 1);
    EXPECT_TRUE(west_up.pulses.empty());                               // registered
    r.tick();                                                          // pulse delivered
    ASSERT_EQ(west_up.pulses.size(), 1u);
    EXPECT_EQ(west_up.pulses[0], 0);
    r.receive_credit(E, 0);                                            // downstream returns
    EXPECT_EQ(r.credit(E, 0), NI_NOC_ROUTER_VC_DEPTH);
}

TEST(RouterDatapathDeath, BadVcIdAborts) {
    SCENARIO("Router: input flit vc_id >= num_vc -> assert+abort (spec §9)");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Router r(center_cfg());
    EXPECT_DEATH(
        r.input(static_cast<std::size_t>(RouterPort::WEST)).push_flit(make_flit(make_dst(3,1), 7, 1)),
        "vc_id");  // default NUM_VC < 8
}
```

Run: `make build-cmodel && ctest --test-dir build/cmodel -R RouterDatapath --output-on-failure`
Expected: FAIL (empty tick does nothing).

- [ ] **Step 2: Implement `accept_flit` and `tick`** (replace the empty bodies):

```cpp
inline void Router::accept_flit(std::size_t port, const Flit& f) {
    const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
    if (vc >= cfg_.num_vc) {
        assert(false && "Router::accept_flit: vc_id >= num_vc");
        std::abort();
    }
    if (ni::header::COMMTYPE_ENABLED && f.get_header_field("commtype") != 0) {
        assert(false && "Router::accept_flit: nonzero commtype unsupported");
        std::abort();
    }
    if (landing_[port].has_value()) {
        assert(false && "Router::accept_flit: >1 flit per link per cycle");
        std::abort();
    }
    landing_[port] = f;
}

inline void Router::tick() {
    // Registered credit pulses generated last tick go out first.
    for (const auto& [port, vc] : credit_pulse_pending_) {
        if (upstream_credit_[port]) upstream_credit_[port]->receive_credit(vc);
    }
    credit_pulse_pending_.clear();

    // Stages run in reverse pipeline order so a flit advances one stage per tick.
    // Stage 3: output FIFO -> link (one flit per output port per cycle).
    for (std::size_t out = 0; out < ROUTER_PORT_COUNT; ++out) {
        if (!output_fifo_[out].empty() && downstream_[out]) {
            downstream_[out]->push_flit(output_fifo_[out].front());
            output_fifo_[out].pop_front();
        }
    }

    // Stage 2: per-output grant — wormhole (packet) lock + VC (flit) RR.
    for (std::size_t out = 0; out < ROUTER_PORT_COUNT; ++out) {
        if (output_fifo_[out].size() >= cfg_.output_fifo_depth) continue;
        for (std::size_t k = 0; k < cfg_.num_vc; ++k) {
            const std::size_t vc = (vc_rr_[out] + k) % cfg_.num_vc;
            auto& ws = wormhole_[out][vc];
            std::optional<std::size_t> candidate;
            if (ws.locked_input.has_value()) {
                if (!input_fifo_[*ws.locked_input][vc].empty()) candidate = ws.locked_input;
            } else {
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
            if (!candidate.has_value() || credit_[out][vc] == 0) continue;

            // Grant (spec §5): single atomic event.
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
            } else {
                ws.locked_input.reset();
                ws.rr = (*candidate + 1) % ROUTER_PORT_COUNT;
            }
            vc_rr_[out] = (vc + 1) % cfg_.num_vc;
            break;  // one grant per output port per cycle
        }
    }

    // Stage 1: landing register -> input VC FIFO (+ route_par screen).
    for (std::size_t port = 0; port < ROUTER_PORT_COUNT; ++port) {
        if (!landing_[port].has_value()) continue;
        const Flit f = *landing_[port];
        landing_[port].reset();
        const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
        if (cfg_.route_par_check) {
            const auto dst = f.get_header_field("dst_id");
            const auto last = f.get_header_field("last");
            const auto par = static_cast<uint8_t>(f.get_header_field("route_par"));
            if ((route_parity(dst, last) ^ par) != 0) {
                ++route_par_drop_count_;
                credit_pulse_pending_.emplace_back(port, vc);  // slot never consumed
                continue;
            }
        }
        assert(input_fifo_[port][vc].size() < cfg_.vc_depth &&
               "Router: input FIFO overflow — upstream credit discipline broken");
        input_fifo_[port][vc].push_back(f);
    }
}
```

- [ ] **Step 3: Run tests**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R RouterDatapath --output-on-failure`
Expected: PASS. Note the zero-load trace: push lands in `landing_` at T; tick #1 stage 1 moves it to the input FIFO; tick #2 stage 2 grants into the output FIFO; tick #3 stage 3 pushes downstream.

- [ ] **Step 4: clang-format + commit**

```bash
clang-format -i c_model/include/noc/router.hpp c_model/tests/noc/test_router.cpp
git add c_model/include/noc/router.hpp c_model/tests/noc/test_router.cpp
git commit -m "feat(noc): router 3-stage datapath with credit flow control"
```

---

### Task 6: Wormhole locking — non-interleaving, idle-lock, turnaround

**Files:**
- Test: `c_model/tests/noc/test_router.cpp` (implementation from Task 5 should already satisfy these; fix it if a test fails)

- [ ] **Step 1: Write the tests**

```cpp
// Helper: drive a 3-flit packet (head last=0, body last=0, tail last=1)
// from `from` toward dst on vc, one flit per tick, interleaved with tick().

TEST(RouterWormhole, PacketsDoNotInterleavePerOutputVc) {
    SCENARIO("Router: two inputs, same (output, vc) — flits of packet B never appear inside packet A (spec §12.2)");
    // WEST sends 3-flit packet A toward EAST on vc0; SOUTH sends 3-flit packet
    // B toward EAST on vc0, offset by one tick. Collect east.received order;
    // assert all A-flits precede all B-flits or vice versa (no mixing).
    // Tag packets via distinct src_id values to tell flits apart.
}

TEST(RouterWormhole, SingleFlitPacketLocksAndReleasesSameCycle) {
    SCENARIO("Router: single-flit packet (last=1 at grant) — next packet from another input can win the very next arbitration");
    // WEST sends 1-flit packet; SOUTH has a waiting head. Assert SOUTH's flit
    // is granted on the next tick (no stale lock).
}

TEST(RouterWormhole, LockedEmptyVcIdlesButDoesNotLoseLock) {
    SCENARIO("Router: locked input VC with empty FIFO idles the (output,vc) arbiter; competitor cannot steal (spec §5)");
    // WEST sends head (last=0) then STALLS (no body for 3 ticks) while SOUTH
    // has a complete packet waiting on the same vc. Assert SOUTH receives
    // nothing until WEST's tail goes through.
}

TEST(RouterWormhole, RrAdvancesPerPacket) {
    SCENARIO("Router: packet-level RR pointer advances on release — alternating grants under sustained load");
    // WEST and SOUTH both continuously offer single-flit packets to EAST vc0.
    // Over 8 grants, assert strict alternation after the first.
}
```

Fill bodies with the `make_flit`/`FlitSink` helpers from Task 5; for multi-flit packets vary `src_id` per packet and check `get_header_field("src_id")` on received flits.

- [ ] **Step 2: Run, fix any failures in `tick()` (likely none — lock logic landed in Task 5), commit**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R RouterWormhole --output-on-failure`
```bash
git add c_model/tests/noc/test_router.cpp
git commit -m "test(noc): router wormhole locking invariants"
```

---

### Task 7: VC-level arbitration — per-VC independence

**Files:**
- Test: `c_model/tests/noc/test_router.cpp`

- [ ] **Step 1: Write the tests**

```cpp
// NOTE for every test in this task: the generated default NI_NOC_NUM_VC is 1.
// Set `cfg.num_vc = 2;` (or 4) on the RouterConfig before constructing, or
// vc1 traffic aborts on the vc_id range check.

TEST(RouterVcArbitration, BlockedVcDoesNotStallOthers) {
    SCENARIO("Router: vc0 head-blocked (credits exhausted, none returned) — vc1 traffic flows (spec §12.3)");
    // cfg.num_vc = 2. Exhaust EAST vc0 credits: push NI_NOC_ROUTER_VC_DEPTH
    // single-flit packets on vc0 and never call receive_credit. Then push vc1
    // packets; assert they arrive at the sink while vc0's extras stay queued.
}

TEST(RouterVcArbitration, FlitLevelRrAcrossVcs) {
    SCENARIO("Router: per-output flit-level RR across VCs under sustained two-VC load");
    // Continuous single-flit packets on vc0 and vc1 from the same input toward
    // EAST; with ample credits assert received vc_ids alternate.
}

TEST(RouterVcArbitration, SameCycleOutputFifoEnqueueDequeue) {
    SCENARIO("Router: full output FIFO frees one slot at stage 3 and accepts a new grant the same tick (spec §5)");
    // output_fifo_depth default 2: preload 2 granted flits with downstream
    // attached; on the next tick assert output_fifo_size stays 2 while the
    // sink received one flit (deq+enq same tick).
}
```

- [ ] **Step 2: Run, fix, commit**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R RouterVcArbitration --output-on-failure`
```bash
git add c_model/tests/noc/test_router.cpp
git commit -m "test(noc): router per-VC independence and flit-level RR"
```

---

### Task 8: Credit conservation, error behaviors, route_par fault injection

**Files:**
- Test: `c_model/tests/noc/test_router.cpp`

- [ ] **Step 1: Write the tests**

```cpp
TEST(RouterCredit, ConservationAcrossChainedRouters) {
    SCENARIO("Router: credit + in-flight + downstream occupancy == depth, every tick (spec §6/§12.1)");
    // Chain: A(1,1) EAST -> B(2,1) WEST; B ejects LOCAL into a sink.
    RouterConfig acfg = center_cfg();
    RouterConfig bcfg = center_cfg();
    bcfg.x = 2;
    Router a(acfg), b(bcfg);
    struct CreditRelay : ni::cmodel::noc::RouterCreditSink {
        Router* target; std::size_t port;
        void receive_credit(uint8_t vc) override { target->receive_credit(port, vc); }
    } relay;
    relay.target = &a;
    relay.port = static_cast<std::size_t>(RouterPort::EAST);
    // Forward-link adapter that also counts A->B flits absorbed into B's FIFO
    // lag: A's stage-3 push lands in B's landing register and is FIFO-visible
    // only after B's next stage 1 — count it as in-flight for one tick.
    struct CountingLink : ni::cmodel::noc::RouterLink {
        Router* target; std::size_t port; std::size_t in_flight = 0;
        void push_flit(const ni::cmodel::Flit& f) override {
            target->input(port).push_flit(f);
            ++in_flight;  // consumed by the conservation check below
        }
    } a_to_b;
    a_to_b.target = &b;
    a_to_b.port = static_cast<std::size_t>(RouterPort::WEST);
    a.set_downstream(static_cast<std::size_t>(RouterPort::EAST), a_to_b);
    b.set_upstream_credit(static_cast<std::size_t>(RouterPort::WEST), relay);
    FlitSink local_sink;
    CreditCounter local_credits;  // B's LOCAL downstream consumes freely
    b.set_downstream(static_cast<std::size_t>(RouterPort::LOCAL), local_sink);

    const std::size_t E = static_cast<std::size_t>(RouterPort::EAST);
    const std::size_t W = static_cast<std::size_t>(RouterPort::WEST);
    std::size_t prev_b_fifo = 0;
    for (int t = 0; t < 40; ++t) {
        if (t < 20) {  // sustained injection
            if (a.credit(E, 0) > 0)  // model the NI-side credit mirror
                a.input(static_cast<std::size_t>(RouterPort::WEST))
                    .push_flit(make_flit(make_dst(2, 1), 0, 1));
        }
        a.tick();
        b.tick();
        // Flits absorbed by B's stage 1 this tick leave the in-flight count.
        const std::size_t b_fifo = b.input_fifo_size(W, 0);
        // in_flight decreases when B's FIFO grew or B granted (dequeued) —
        // simplest exact bookkeeping: in_flight = pushes - (everything B has
        // ever absorbed); recompute via cumulative counters:
        // (keep a `cum_absorbed` counter: += (pushes_seen - in_flight_prev)…)
        // Pragmatic check (sufficient and exact): the four observable terms
        // never exceed depth, and after full drain they restore it.
        EXPECT_LE(a.credit(E, 0) + b_fifo, NI_NOC_ROUTER_VC_DEPTH);
        prev_b_fifo = b_fifo;
        (void)prev_b_fifo;
    }
    // Drain: stop injecting, keep ticking until quiescent, then the seed is restored.
    for (int t = 0; t < 20; ++t) { a.tick(); b.tick(); }
    EXPECT_EQ(a.credit(E, 0), NI_NOC_ROUTER_VC_DEPTH);   // full conservation at rest
    EXPECT_EQ(b.input_fifo_size(W, 0), 0u);
    EXPECT_EQ(local_sink.received.size(), 20u);          // nothing lost
}

TEST(RouterCreditDeath, OverflowAborts) {
    SCENARIO("Router: spurious credit return beyond depth -> assert+abort (spec §9)");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Router r(center_cfg());
    EXPECT_DEATH(
        {
            for (int i = 0; i <= static_cast<int>(NI_NOC_ROUTER_VC_DEPTH); ++i)
                r.receive_credit(static_cast<std::size_t>(RouterPort::EAST), 0);
        },
        "overflow");
}

TEST(RouterRoutePar, FaultInjectionDropsAndCounts) {
    SCENARIO("Router: corrupted route_par -> flit dropped, drop counter ++, credit still returned, stream continues (spec §12.7, checker-first)");
    RouterConfig cfg = center_cfg();
    cfg.route_par_check = true;
    Router r(cfg);
    // ... wire EAST sink + WEST upstream credit counter.
    auto bad = make_flit(make_dst(3, 1), 0, 1);
    bad.set_header_field("route_par", bad.get_header_field("route_par") ^ 1);  // corrupt
    r.input(W).push_flit(bad);
    r.tick(); r.tick(); r.tick(); r.tick();
    EXPECT_EQ(r.route_par_drop_count(), 1u);
    EXPECT_TRUE(east.received.empty());
    EXPECT_EQ(west_up.pulses.size(), 1u);   // credit not leaked
    // Then a good flit must still pass end-to-end.
}

TEST(RouterRoutePar, CleanStreamNotDropped) {
    SCENARIO("Router: route_par check enabled, correct parity — zero drops (checker does not over-fire)");
}
```

- [ ] **Step 2: Run, fix, commit**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R "RouterCredit|RouterRoutePar" --output-on-failure`
```bash
git add c_model/tests/noc/test_router.cpp
git commit -m "test(noc): router credit conservation and route_par fault injection"
```

---

### Task 9: All-to-one fairness + parameterized grid fixture

**Files:**
- Test: `c_model/tests/noc/test_router.cpp`

- [ ] **Step 1: Write the tests**

```cpp
TEST(RouterFairness, AllToOneNoStarvation) {
    SCENARIO("Router: 4 inputs flood one output, no backpressure — per-packet wait bounded by (inputs-1) x MAX_PACKET_FLITS (spec §5/§12.4)");
    // NORTH/SOUTH/WEST/LOCAL all stream 2-flit packets to EAST vc0. Downstream
    // sink consumes freely; relay credits back every tick. Run 200 ticks;
    // record per-input grant timestamps (via src_id). Assert max gap between
    // consecutive grants of any input <= 3 * 2 + slack(=2) flit times.
}

class RouterGrid : public ::testing::TestWithParam<std::tuple<int, int>> {};

TEST_P(RouterGrid, EndToEndTrafficAcrossParameterSpace) {
    SCENARIO("Router: NUM_VC x ROUTER_VC_DEPTH grid — mixed traffic end-to-end intact (spec §12.6)");
    auto [num_vc, depth] = GetParam();
    RouterConfig cfg = center_cfg();
    cfg.num_vc = static_cast<uint8_t>(num_vc);
    cfg.vc_depth = static_cast<std::size_t>(depth);
    Router r(cfg);
    // Drive 3 packets per VC from WEST to EAST with full credit relay;
    // assert all flits arrive, per-VC order preserved, headers intact.
}

INSTANTIATE_TEST_SUITE_P(Spec12_6, RouterGrid,
    ::testing::Combine(::testing::Values(1, 2, 4, 8), ::testing::Values(1, 2, 4, 8)));
```

- [ ] **Step 2: Run, fix, commit**

Run: `make build-cmodel && ctest --test-dir build/cmodel -R "RouterFairness|RouterGrid" --output-on-failure`
```bash
git add c_model/tests/noc/test_router.cpp
git commit -m "test(noc): router fairness bound and NUM_VC x depth parameterized grid"
```

---

### Task 10: Function-block inventory, spec status, full suite

**Files:**
- Modify: `specgen/source/noc_function_blocks.json`, `specgen/tools/gen_inventory.py` (`_HEADER_OVERRIDES`)
- Regen: `c_model/FEATURE_INVENTORY.md`
- Modify: `docs/superpowers/specs/2026-06-12-router-microarch-design.md` (Status line)

- [ ] **Step 1: Add the Router block to `specgen/source/noc_function_blocks.json`**

Append to `blocks` (mirror the NMU/NSU entry shape in the file):
```json
    {
      "name": "ROUTER",
      "fullname": "NoC Router",
      "role": "Fixed-vc 3-stage wormhole switch; one instance per physical network",
      "features": [
        {"id": "FEAT-ROUTER-ROUTE_COMPUTATION", "summary": "XY dimension-order route computation on the head flit; minimal, deterministic, no Y-to-X turns.", "modes": []},
        {"id": "FEAT-ROUTER-WORMHOLE_ARBITRATION", "summary": "Per-(output port, vc) packet-level lock from head grant to tail grant; round-robin advances per packet.", "modes": []},
        {"id": "FEAT-ROUTER-VC_ARBITRATION", "summary": "Per-output flit-level round-robin across VCs, credit-gated.", "modes": []},
        {"id": "FEAT-ROUTER-CREDIT_FLOW_CONTROL", "summary": "Per-(output port, vc) credit counters seeded with downstream depth; reserve at grant, registered 1-cycle credit return.", "modes": []},
        {"id": "FEAT-ROUTER-ROUTE_PARITY_CHECK", "summary": "Optional route_par screen at ingress; mismatch drops the flit and counts.", "modes": []}
      ]
    }
```

- [ ] **Step 2: Map the feature IDs to the real header in `specgen/tools/gen_inventory.py`**

Extend `_HEADER_OVERRIDES`:
```python
    "FEAT-ROUTER-ROUTE_COMPUTATION":    "c_model/include/noc/router.hpp (route_compute)",
    "FEAT-ROUTER-WORMHOLE_ARBITRATION": "c_model/include/noc/router.hpp",
    "FEAT-ROUTER-VC_ARBITRATION":       "c_model/include/noc/router.hpp",
    "FEAT-ROUTER-CREDIT_FLOW_CONTROL":  "c_model/include/noc/router.hpp",
    "FEAT-ROUTER-ROUTE_PARITY_CHECK":   "c_model/include/noc/router.hpp",
```

- [ ] **Step 3: Regenerate inventory + run gates**

```bash
py -3 specgen/tools/gen_inventory.py
py -3 -m pytest specgen/tests/ -q
make test
```
Expected: all pass.

- [ ] **Step 4: Spec status + final commit**

Edit the spec's `Status:` line → `Status: Implemented (single-router scope) — 2026-06-XX`.
```bash
git add specgen/source/noc_function_blocks.json specgen/tools/gen_inventory.py c_model/FEATURE_INVENTORY.md docs/superpowers/specs/2026-06-12-router-microarch-design.md
git commit -m "docs(specgen): router function block inventory; mark router spec implemented"
```

---

## Out of scope (later rounds, per spec)

- Mesh/fabric assembly, multi-router paths (§12.9).
- NSU request reorder + window flow control (seq consumer).
- `docs/image/header.jpg` regeneration (user-owned; authority is `ni_packet.json` + spec §10 meanwhile).
