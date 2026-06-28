# RouterChannel Co-sim Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the production `RouterChannel` into the Verilator co-sim by rewriting `tb_top`
as a 2-node bidirectional testbench driving the real router fabric (no wb2axip protocol checkers), validated by a
bidirectional `CosimIntegration` over coordinate-bearing scenario variants.

**Architecture:** A `RouterChannelShellAdapter` mirrors `ChannelModelShellAdapter` but owns one
`RouterChannel` and exposes both nodes' NoC ports (its I/O struct = 2× the existing
`ChannelModel` structs). New DPI handlers + a 4-bundle `router_channel_wrap.sv` carry it into a
rewritten `tb_top` with 2×{master,NMU,NSU,slave}. Each master loads its own coordinate variant
(node0 = identity, node1 = `+0x1_0000_0000`); a single shared scoreboard validates both flows
(disjoint address ranges). num_vc pinned to 1 (SV wrappers fatal otherwise).

**Tech Stack:** C++17, Verilator DPI (chandle ABI), SystemVerilog, GoogleTest/ctest, Python 3
(scenario generator), yaml-cpp.

**Build/test note (Windows):** the cmodel ctest builds under `build/cmodel` (ninja). From Git
Bash, prepend `export PATH="/c/Windows/System32:$PATH"` so ninja's `cmd.exe` link rule resolves.
Verilator/`Vtb_top` builds via `MSYSTEM=MINGW64 LC_ALL=C /c/msys64/usr/bin/bash -lc 'cd <abs> && make ...'`.

---

## File Structure

| File | Responsibility |
|---|---|
| `c_model/include/cosim/router_channel_shell_io.hpp` (new) | `RouterChannelInputs/Outputs` = `std::array<ChannelModel{Inputs,Outputs},2>` |
| `c_model/include/cosim/router_channel_shell_adapter.hpp` (new) | `RouterChannelShellAdapter`: owns one `RouterChannel(num_vc=1)`; per-node tick |
| `c_model/tests/cosim/test_router_channel_shell_adapter.cpp` (new) | adapter unit test (both directions) |
| `cosim/c/cmodel_dpi.cpp` (modify) | `ShellType::RouterChannel` handlers; per-master/slave scenario path; `cmodel_master_count`/`cmodel_reads_checked` |
| `cosim/c/handle_block.hpp` (modify) | add `RouterChannel` to `ShellType` enum |
| `cosim/sources.mk` (modify) | swap `channel_model_wrap.sv` → `router_channel_wrap.sv` in the tb_top source list |
| `cosim/verilator/Makefile` (modify) | `run-tb-top` target → two-plusarg invocation |
| `cosim/c/cmodel_dpi.h` (modify) | declare new exported DPI functions |
| `cosim/tools/gen_coordinate_scenarios.py` (new) | emit node0/node1 coordinate variants |
| `cosim/sv/router_channel_wrap.sv` (new) | 4-bundle beta-tick DPI wrapper |
| `cosim/sv/tb_top.sv` (rewrite) | 2×{master,NMU,NSU,slave} + router_channel_wrap; non-vacuous exit guard |
| `cosim/tests/test_cosim_integration.cpp` (modify) | bidirectional invocation over the variant subset |
| `cosim/tests/CMakeLists.txt` (modify) | generate variants at build, pass variant root to the test |

---

## Task 1: RouterChannel NoC I/O struct

**Files:**
- Create: `c_model/include/cosim/router_channel_shell_io.hpp`

- [ ] **Step 1: Write the header**

```cpp
// RouterChannel shell IO — bidirectional 2-node NoC pin bundle.
//
// Each node's NoC interface is identical in shape to the single-NMU/single-NSU
// ChannelModel bundle, so RouterChannelInputs/Outputs are 2x the ChannelModel
// structs indexed by node (0,0)=node[0], (1,0)=node[1]. Per node the field
// meaning is: req_in = local NMU injects a request; rsp_in = local NSU injects a
// response; req_out = request ejected toward the local NSU; rsp_out = response
// ejected toward the local NMU; *_credit_return as in ChannelModel.
#pragma once
#include "cosim/channel_model_shell_io.hpp"
#include <array>
#include <cstddef>

namespace ni::cmodel::cosim {

inline constexpr std::size_t kRouterChannelNodes = 2;

struct RouterChannelInputs {
    std::array<ChannelModelInputs, kRouterChannelNodes> node{};
};

struct RouterChannelOutputs {
    std::array<ChannelModelOutputs, kRouterChannelNodes> node{};
};

}  // namespace ni::cmodel::cosim
```

- [ ] **Step 2: Verify it compiles**

Header-only; compiled transitively by Task 2's test. No standalone build step.

- [ ] **Step 3: Commit**

```bash
git add c_model/include/cosim/router_channel_shell_io.hpp
git commit -m "feat(cosim): RouterChannel shell IO struct (2-node NoC pin bundle)"
```

---

## Task 2: RouterChannelShellAdapter + unit test

**Files:**
- Create: `c_model/include/cosim/router_channel_shell_adapter.hpp`
- Create: `c_model/tests/cosim/test_router_channel_shell_adapter.cpp`
- Modify: `c_model/tests/cosim/CMakeLists.txt` (register the new test)

- [ ] **Step 1: Write the failing test**

Mirror `test_channel_model_shell_adapter.cpp` but both directions. The flit helpers
`flit_from_bytes`/`flit_to_bytes` live in `cosim/flit_byte_conv.hpp`; build a flit with the
`ni::cmodel::Flit` API. The single-flit traverses one router hop, so it takes a few ticks to
appear at the far node — loop until it ejects (cap the loop).

```cpp
#include "cosim/router_channel_shell_adapter.hpp"
#include "cosim/flit_byte_conv.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>

using namespace ni::cmodel::cosim;

namespace {

// Build a request flit destined for dst_id, vc 0.
ni::cmodel::Flit make_req(uint8_t dst_id) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_AR);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("last", 1);
    return f;
}

}  // namespace

// node1.NMU injects a request to dst=(0,0); it must eject at node0.NSU.
TEST(RouterChannelShellAdapter, ReqNode1ToNode0) {
    RouterChannelShellAdapter a;
    a.init();
    RouterChannelInputs in{};
    in.node[1].req_in_valid = true;
    in.node[1].req_in_flit = flit_to_bytes(make_req(/*dst=*/0x00));
    a.set_inputs(in);
    a.tick();
    // After the injection tick, stop driving and pump until ejection.
    a.set_inputs(RouterChannelInputs{});
    RouterChannelOutputs out{};
    bool ejected = false;
    for (int cyc = 0; cyc < 16 && !ejected; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.node[0].req_out_valid) {
            ejected = true;
            auto f = flit_from_bytes(out.node[0].req_out_flit);
            EXPECT_EQ(f.get_header_field("dst_id"), 0x00u);
        }
    }
    EXPECT_TRUE(ejected) << "request never ejected at node0 NSU";
}

// node0.NSU injects a response to dst=(1,0); it must eject at node1.NMU.
TEST(RouterChannelShellAdapter, RspNode0ToNode1) {
    RouterChannelShellAdapter a;
    a.init();
    ni::cmodel::Flit rsp;
    rsp.set_header_field("axi_ch", ni::AXI_CH_R);
    rsp.set_header_field("dst_id", 0x01);
    rsp.set_header_field("vc_id", 0);
    rsp.set_header_field("last", 1);
    RouterChannelInputs in{};
    in.node[0].rsp_in_valid = true;
    in.node[0].rsp_in_flit = flit_to_bytes(rsp);
    a.set_inputs(in);
    a.tick();
    a.set_inputs(RouterChannelInputs{});
    RouterChannelOutputs out{};
    bool ejected = false;
    for (int cyc = 0; cyc < 16 && !ejected; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.node[1].rsp_out_valid) ejected = true;
    }
    EXPECT_TRUE(ejected) << "response never ejected at node1 NMU";
}
```

Register in `c_model/tests/cosim/CMakeLists.txt` — copy the `test_channel_model_shell_adapter`
block, renaming target + source to `test_router_channel_shell_adapter`. (Read the existing
block for the exact `add_executable`/`target_link_libraries`/`gtest_discover_tests` form.)

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /e/05_NoC/noc_project/build/cmodel && export PATH="/c/Windows/System32:$PATH"
cmake --build . -j 4 2>&1 | tail -5
```
Expected: FAIL — `router_channel_shell_adapter.hpp: No such file`.

- [ ] **Step 3: Write the adapter**

```cpp
// RouterChannelShellAdapter — ShellAdapter wrapping the production RouterChannel.
//
// Owns one RouterChannel(num_vc=1) and drives BOTH nodes per tick. Per node the
// 3-step pattern is identical to ChannelModelShellAdapter, indexed by node:
//   req_in (NMU inject) -> nmu_req_out(n).push_flit
//   rsp_in (NSU inject) -> nsu_rsp_out(n).push_flit
//   req_out (to NSU)    <- nsu_req_in(n).pop_flit
//   rsp_out (to NMU)    <- nmu_rsp_in(n).pop_flit
//   *_out_credit_return  = inject-side credit_avail(0)
// The SV-provided *_in_credit_return inputs are accepted but unused: RouterChannel
// manages credit internally (eject pop returns router credit; inject mirror gates
// the NMU via *_out_credit_return). num_vc=1: the SV NoC wrappers fatal otherwise.
//
// Depth: RouterChannel's default vc_depth is 4 (NI_NOC_ROUTER_VC_DEPTH); the SV
// credit feedback is registered one cycle behind (beta-tick), so a too-shallow
// inject mirror risks a stale-credit overshoot. Pin vc_depth/out_fifo to the
// PoC ChannelModel depth (kPoCChannelModelDepth=64) for margin, and throw if a
// push is ever rejected so a silent flit-drop becomes a loud DPI failure (the
// NMU is credit-gated via *_out_credit_return, so a reject means a real bug).
#pragma once
#include "cosim/flit_byte_conv.hpp"  // flit_from_bytes, flit_to_bytes
#include "cosim/poc_defaults.hpp"    // kPoCChannelModelDepth
#include "cosim/router_channel_shell_io.hpp"
#include "noc/router_channel.hpp"
#include <memory>
#include <stdexcept>

namespace ni::cmodel::cosim {

class RouterChannelShellAdapter {
  public:
    void init() {
        channel_ = std::make_unique<noc::RouterChannel>(
            /*num_vc=*/1, /*vc_depth=*/kPoCChannelModelDepth,
            /*out_fifo_depth=*/kPoCChannelModelDepth);
        in_ = RouterChannelInputs{};
        out_ = RouterChannelOutputs{};
    }

    void set_inputs(const RouterChannelInputs& in) { in_ = in; }

    void tick() {
        for (std::size_t n = 0; n < kRouterChannelNodes; ++n) {
            if (in_.node[n].req_in_valid &&
                !channel_->nmu_req_out(n).push_flit(flit_from_bytes(in_.node[n].req_in_flit))) {
                throw std::runtime_error("RouterChannelShellAdapter: req push rejected (credit "
                                         "discipline violated at node " + std::to_string(n) + ")");
            }
            if (in_.node[n].rsp_in_valid &&
                !channel_->nsu_rsp_out(n).push_flit(flit_from_bytes(in_.node[n].rsp_in_flit))) {
                throw std::runtime_error("RouterChannelShellAdapter: rsp push rejected (credit "
                                         "discipline violated at node " + std::to_string(n) + ")");
            }
        }

        channel_->tick();

        out_ = RouterChannelOutputs{};
        for (std::size_t n = 0; n < kRouterChannelNodes; ++n) {
            if (auto f = channel_->nsu_req_in(n).pop_flit()) {
                out_.node[n].req_out_valid = true;
                out_.node[n].req_out_flit = flit_to_bytes(*f);
            }
            if (auto f = channel_->nmu_rsp_in(n).pop_flit()) {
                out_.node[n].rsp_out_valid = true;
                out_.node[n].rsp_out_flit = flit_to_bytes(*f);
            }
            out_.node[n].req_out_credit_return = channel_->nmu_req_out(n).credit_avail(0);
            out_.node[n].rsp_out_credit_return = channel_->nsu_rsp_out(n).credit_avail(0);
        }
    }

    void get_outputs(RouterChannelOutputs& out) const { out = out_; }

  private:
    std::unique_ptr<noc::RouterChannel> channel_;
    RouterChannelInputs in_{};
    RouterChannelOutputs out_{};
};

}  // namespace ni::cmodel::cosim
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd /e/05_NoC/noc_project/build/cmodel && export PATH="/c/Windows/System32:$PATH"
cmake --build . -j 4 2>&1 | tail -3 && ctest -R RouterChannelShellAdapter --output-on-failure
```
Expected: PASS (2/2). If ejection never happens, increase the loop cap or check the dst_id/route.

- [ ] **Step 5: Run clang-format + commit**

```bash
/c/msys64/mingw64/bin/clang-format -i c_model/include/cosim/router_channel_shell_adapter.hpp \
    c_model/include/cosim/router_channel_shell_io.hpp \
    c_model/tests/cosim/test_router_channel_shell_adapter.cpp
git add c_model/include/cosim/router_channel_shell_adapter.hpp \
    c_model/tests/cosim/test_router_channel_shell_adapter.cpp c_model/tests/cosim/CMakeLists.txt
git commit -m "feat(cosim): RouterChannelShellAdapter (2-node) + unit test"
```

---

## Task 3: DPI handlers for RouterChannel

**Files:**
- Modify: `cosim/c/handle_block.hpp` (add `RouterChannel` to `ShellType`)
- Modify: `cosim/c/cmodel_dpi.cpp` (handlers)
- Modify: `cosim/c/cmodel_dpi.h` (declarations)

- [ ] **Step 1: Add the ShellType enum value**

In `cosim/c/handle_block.hpp`, find `enum class ShellType` and add `RouterChannel` as a
new value (keep existing values; append to avoid renumbering — the magic sentinel stores the
numeric tag, so a stable order matters only within a session, but append regardless).

- [ ] **Step 2: Add the create/set_inputs/tick/get_outputs handlers**

In `cosim/c/cmodel_dpi.cpp`, after the ChannelModel handlers block (ends ~line 328), add. The
DPI carries both nodes' pins (suffix `_n0`/`_n1`). `unpack_flit`/`pack_flit` are already defined
above in the file.

```cpp
#include "cosim/router_channel_shell_adapter.hpp"  // add to includes at top

using ni::cmodel::cosim::RouterChannelInputs;
using ni::cmodel::cosim::RouterChannelOutputs;
using ni::cmodel::cosim::RouterChannelShellAdapter;

extern "C" void* cmodel_router_channel_create(const char* name) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED,
                             "cmodel_router_channel_create: not initialized");
        return nullptr;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_router_channel_create, nullptr) {
        auto adapter = std::make_unique<RouterChannelShellAdapter>();
        adapter->init();
        auto* h = new HandleBlock{
            static_cast<uint32_t>(ShellType::RouterChannel), ShellType::RouterChannel,
            HandleState::Live, std::string(name),
            std::unique_ptr<void, void (*)(void*)>(adapter.release(), [](void* p) {
                delete static_cast<RouterChannelShellAdapter*>(p);
            })};
        g_handle_registry.insert(h);
        return static_cast<void*>(h);
    }
    DPI_BOUNDARY_END_R(cmodel_router_channel_create);
}

extern "C" void cmodel_router_channel_set_inputs(
    void* ctx,
    svBit req_in_valid_n0, svBitVecVal* req_in_flit_n0, svBit req_in_credit_return_n0,
    svBit rsp_in_valid_n0, svBitVecVal* rsp_in_flit_n0, svBit rsp_in_credit_return_n0,
    svBit req_in_valid_n1, svBitVecVal* req_in_flit_n1, svBit req_in_credit_return_n1,
    svBit rsp_in_valid_n1, svBitVecVal* rsp_in_flit_n1, svBit rsp_in_credit_return_n1) {
    DPI_BOUNDARY_BEGIN(cmodel_router_channel_set_inputs) {
        REQUIRE_HANDLE(ctx, ShellType::RouterChannel, "cmodel_router_channel_set_inputs");
        auto* rc = static_cast<RouterChannelShellAdapter*>(_h->adapter.get());
        RouterChannelInputs in{};
        in.node[0].req_in_valid = static_cast<bool>(req_in_valid_n0);
        in.node[0].req_in_flit = unpack_flit(req_in_flit_n0);
        in.node[0].req_in_credit_return = static_cast<bool>(req_in_credit_return_n0);
        in.node[0].rsp_in_valid = static_cast<bool>(rsp_in_valid_n0);
        in.node[0].rsp_in_flit = unpack_flit(rsp_in_flit_n0);
        in.node[0].rsp_in_credit_return = static_cast<bool>(rsp_in_credit_return_n0);
        in.node[1].req_in_valid = static_cast<bool>(req_in_valid_n1);
        in.node[1].req_in_flit = unpack_flit(req_in_flit_n1);
        in.node[1].req_in_credit_return = static_cast<bool>(req_in_credit_return_n1);
        in.node[1].rsp_in_valid = static_cast<bool>(rsp_in_valid_n1);
        in.node[1].rsp_in_flit = unpack_flit(rsp_in_flit_n1);
        in.node[1].rsp_in_credit_return = static_cast<bool>(rsp_in_credit_return_n1);
        rc->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_router_channel_set_inputs);
}

extern "C" void cmodel_router_channel_tick(void* ctx) {
    DPI_BOUNDARY_BEGIN(cmodel_router_channel_tick) {
        REQUIRE_HANDLE(ctx, ShellType::RouterChannel, "cmodel_router_channel_tick");
        static_cast<RouterChannelShellAdapter*>(_h->adapter.get())->tick();
    }
    DPI_BOUNDARY_END(cmodel_router_channel_tick);
}

extern "C" void cmodel_router_channel_get_outputs(
    void* ctx,
    svBit* req_out_valid_n0, svBitVecVal* req_out_flit_n0, svBit* req_out_credit_return_n0,
    svBit* rsp_out_valid_n0, svBitVecVal* rsp_out_flit_n0, svBit* rsp_out_credit_return_n0,
    svBit* req_out_valid_n1, svBitVecVal* req_out_flit_n1, svBit* req_out_credit_return_n1,
    svBit* rsp_out_valid_n1, svBitVecVal* rsp_out_flit_n1, svBit* rsp_out_credit_return_n1) {
    DPI_BOUNDARY_BEGIN(cmodel_router_channel_get_outputs) {
        REQUIRE_HANDLE(ctx, ShellType::RouterChannel, "cmodel_router_channel_get_outputs");
        auto* rc = static_cast<RouterChannelShellAdapter*>(_h->adapter.get());
        RouterChannelOutputs out{};
        rc->get_outputs(out);
        *req_out_valid_n0 = static_cast<svBit>(out.node[0].req_out_valid);
        pack_flit(out.node[0].req_out_flit, req_out_flit_n0);
        *req_out_credit_return_n0 = static_cast<svBit>(out.node[0].req_out_credit_return);
        *rsp_out_valid_n0 = static_cast<svBit>(out.node[0].rsp_out_valid);
        pack_flit(out.node[0].rsp_out_flit, rsp_out_flit_n0);
        *rsp_out_credit_return_n0 = static_cast<svBit>(out.node[0].rsp_out_credit_return);
        *req_out_valid_n1 = static_cast<svBit>(out.node[1].req_out_valid);
        pack_flit(out.node[1].req_out_flit, req_out_flit_n1);
        *req_out_credit_return_n1 = static_cast<svBit>(out.node[1].req_out_credit_return);
        *rsp_out_valid_n1 = static_cast<svBit>(out.node[1].rsp_out_valid);
        pack_flit(out.node[1].rsp_out_flit, rsp_out_flit_n1);
        *rsp_out_credit_return_n1 = static_cast<svBit>(out.node[1].rsp_out_credit_return);
    }
    DPI_BOUNDARY_END(cmodel_router_channel_get_outputs);
}
```

- [ ] **Step 3: Declare the exports in `cosim/c/cmodel_dpi.h`**

Mirror the existing `cmodel_channel_model_*` declarations (same `extern "C"` block style) for the
four `cmodel_router_channel_*` functions with the signatures above.

- [ ] **Step 4: Build the DPI test target + run**

```bash
cd /e/05_NoC/noc_project/build/cmodel && export PATH="/c/Windows/System32:$PATH"
cmake --build . -j 4 2>&1 | tail -5 && ctest -R Cmodel.*Dpi -E Cosim --output-on-failure
```
Expected: existing test_cmodel_dpi still PASS (no regression; new handlers compile).

- [ ] **Step 5: Commit**

```bash
git add cosim/include/handle_block.hpp cosim/c/cmodel_dpi.cpp cosim/c/cmodel_dpi.h
git commit -m "feat(cosim): DPI handlers for RouterChannel (2-node set_inputs/tick/get_outputs)"
```

---

## Task 4: Per-master/slave scenario path + non-vacuous DPI

**Files:**
- Modify: `cosim/c/cmodel_dpi.cpp`, `cosim/c/cmodel_dpi.h`
- Modify: `cosim/tests/test_cmodel_dpi.cpp` (update create-call signatures)

- [ ] **Step 1: Change `cmodel_master_create` to take a scenario path**

Replace the signature and the `init` call (everything else in the body unchanged):

```cpp
extern "C" void* cmodel_master_create(const char* name, const char* scenario_path) {
    // ... unchanged state guard ...
    DPI_BOUNDARY_BEGIN_R(cmodel_master_create, nullptr) {
        const std::string dump_path = "master_shell_read_dump_" + std::string(name) + ".txt";
        auto adapter = std::make_unique<MasterShellAdapter>();
        adapter->init(std::string(scenario_path), dump_path,
                      g_scenario.config.max_outstanding_write,
                      g_scenario.config.max_outstanding_read);
        // ... unchanged configure_inject + scoreboard callbacks + handle registration ...
    }
    DPI_BOUNDARY_END_R(cmodel_master_create);
}
```
Rationale: the two variants share config (max_outstanding, inject) — those stay from
`g_scenario` (set by `cmodel_init`); only the transaction list comes from the per-master variant
at `scenario_path`.

- [ ] **Step 2: Change `cmodel_slave_create` to take a scenario path**

The slave's `memory_base` must come from its own variant (the generator baked the shift into
it). Load the variant for base/size; keep shared latencies from `g_scenario`:

```cpp
extern "C" void* cmodel_slave_create(const char* name, const char* scenario_path) {
    // ... unchanged state guard ...
    DPI_BOUNDARY_BEGIN_R(cmodel_slave_create, nullptr) {
        auto variant = ni::cmodel::axi::load_scenario(std::string(scenario_path));
        auto adapter = std::make_unique<SlaveShellAdapter>();
        adapter->init(variant.config.memory_base, variant.config.memory_size,
                      g_scenario.config.write_latency, g_scenario.config.read_latency);
        // ... unchanged handle registration ...
    }
    DPI_BOUNDARY_END_R(cmodel_slave_create);
}
```

- [ ] **Step 3: Change `cmodel_nmu_create` AND `cmodel_nsu_create` to take a node src_id**

Both `NmuShellAdapter::init(uint8_t src_id, ...)` and `NsuShellAdapter::init(uint8_t src_id, ...)`
already accept src_id (default 0). The NSU stamps the response flit's src_id from its own
coordinate, so node1's NSU must be src_id=1 too (not just the NMU). Thread src_id through both:

```cpp
extern "C" void* cmodel_nmu_create(const char* name, int src_id) {
    // ... unchanged state guard ...
    DPI_BOUNDARY_BEGIN_R(cmodel_nmu_create, nullptr) {
        auto adapter = std::make_unique<NmuShellAdapter>();
        adapter->init(static_cast<uint8_t>(src_id));
        // ... unchanged handle registration ...
    }
    DPI_BOUNDARY_END_R(cmodel_nmu_create);
}
// cmodel_nsu_create: identical edit — add `int src_id` arg, pass to adapter->init(src_id).
```
Update both declarations in `cmodel_dpi.h` to the 2-arg form.

- [ ] **Step 4: Add non-vacuous-guard DPI**

```cpp
// Master count + reads-checked, for tb_top's non-vacuous PASS guard.
extern "C" int cmodel_master_count(void) {
    return static_cast<int>(g_ever_created_master);
}
extern "C" int cmodel_reads_checked(void) {
    if (!g_scoreboard) return 0;
    return static_cast<int>(g_scoreboard->reads_checked());
}
```
Declare both in `cmodel_dpi.h`. Update the `cmodel_master_create`/`cmodel_slave_create`
declarations there to the new 2-arg signatures.

- [ ] **Step 5: Update `test_cmodel_dpi.cpp` call sites**

Find every `cmodel_master_create(...)` / `cmodel_slave_create(...)` / `cmodel_nmu_create(...)` in
the test. Pass the scenario path the test already uses (`CMODEL_TEST_SCENARIO_YAML` env, or the
test's local path) to master and slave; pass a src_id (e.g. `0`) to nmu. Add a smoke assertion:
`EXPECT_EQ(cmodel_master_count(), 1)` after one master create.

- [ ] **Step 6: Build + run the DPI test**

```bash
cd /e/05_NoC/noc_project/build/cmodel && export PATH="/c/Windows/System32:$PATH"
cmake --build . -j 4 2>&1 | tail -5 && ctest -R Cmodel.*Dpi -E Cosim --output-on-failure
```
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add cosim/c/cmodel_dpi.cpp cosim/c/cmodel_dpi.h cosim/tests/test_cmodel_dpi.cpp
git commit -m "feat(cosim): per-master/slave scenario path + master_count/reads_checked DPI"
```

---

## Task 5: Coordinate-variant scenario generator

**Files:**
- Create: `cosim/tools/gen_coordinate_scenarios.py`
- Create: `cosim/tools/test_gen_coordinate_scenarios.py`

- [ ] **Step 1: Write the failing test**

```python
import os, subprocess, sys, tempfile, yaml

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "gen_coordinate_scenarios.py")
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(REPO, "tests", "scenarios",
                   "AX4-BAS-001_single_write_no_read", "scenario.yaml")

def test_node1_variant_shifts_addr_and_base():
    with tempfile.TemporaryDirectory() as out:
        subprocess.run([sys.executable, GEN, SRC, out], check=True)
        n0 = yaml.safe_load(open(os.path.join(out, "node0", "scenario.yaml")))
        n1 = yaml.safe_load(open(os.path.join(out, "node1", "scenario.yaml")))
        # node0 = identity for addresses/base
        assert int(str(n0["transactions"][0]["addr"]), 0) == 0x1000
        assert int(str(n0["config"]["memory_base"]), 0) == 0x1000
        # node1 = +0x100000000 on addr and memory_base
        assert int(str(n1["transactions"][0]["addr"]), 0) == 0x1000 + 0x100000000
        assert int(str(n1["config"]["memory_base"]), 0) == 0x1000 + 0x100000000
        # data_file rewritten to an absolute path that exists
        assert os.path.isabs(n1["transactions"][0]["data_file"])
        assert os.path.exists(n1["transactions"][0]["data_file"])
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd /e/05_NoC/noc_project && py -3 -m pytest cosim/tools/test_gen_coordinate_scenarios.py -q
```
Expected: FAIL — `gen_coordinate_scenarios.py` not found.

- [ ] **Step 3: Write the generator**

```python
#!/usr/bin/env python3
"""Emit per-node coordinate-bearing scenario variants for bidirectional cosim.

Usage: gen_coordinate_scenarios.py <src scenario.yaml> <out_dir>
Writes <out_dir>/node0/scenario.yaml (identity) and <out_dir>/node1/scenario.yaml
(+NODE1_OFFSET on every transaction addr and on config.memory_base). File
references (data_file/dump_file/strb_file) are rewritten to absolute paths against
the source directory so they resolve from any cwd (load_scenario resolves relative
paths against the YAML's own dir).
"""
import copy
import os
import sys
import yaml

NODE1_OFFSET = 0x100000000  # bit 32 -> dst coordinate (1,0)
_FILE_KEYS = ("data_file", "dump_file", "strb_file")


def _as_int(v):
    return v if isinstance(v, int) else int(str(v), 0)


def _emit(sc, src_dir, out_dir, offset):
    sc = copy.deepcopy(sc)
    for t in sc.get("transactions", []):
        if offset:
            t["addr"] = _as_int(t["addr"]) + offset
        for k in _FILE_KEYS:
            if t.get(k) and not os.path.isabs(t[k]):
                t[k] = os.path.join(src_dir, t[k])
    if offset:
        cfg = sc.setdefault("config", {})
        cfg["memory_base"] = _as_int(cfg.get("memory_base", 0)) + offset
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "scenario.yaml"), "w") as f:
        yaml.safe_dump(sc, f, sort_keys=False)


def main(argv):
    src, out = argv[1], argv[2]
    src_dir = os.path.dirname(os.path.abspath(src))
    with open(src) as f:
        sc = yaml.safe_load(f)
    _emit(sc, src_dir, os.path.join(out, "node0"), 0)
    _emit(sc, src_dir, os.path.join(out, "node1"), NODE1_OFFSET)


if __name__ == "__main__":
    main(sys.argv)
```

- [ ] **Step 4: Run to verify it passes**

```bash
cd /e/05_NoC/noc_project && py -3 -m pytest cosim/tools/test_gen_coordinate_scenarios.py -q
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add cosim/tools/gen_coordinate_scenarios.py cosim/tools/test_gen_coordinate_scenarios.py
git commit -m "feat(cosim): coordinate-variant scenario generator (node0/node1)"
```

---

## Task 6: router_channel_wrap.sv

**Files:**
- Create: `cosim/sv/router_channel_wrap.sv`

This is `channel_model_wrap.sv` replicated for **4** `noc_intf` bundles. Read
`cosim/sv/channel_model_wrap.sv` for the beta-tick skeleton (sync-reset always_ff, set_inputs →
tick → get_outputs into `*_q` registers, drive interface outputs from `*_q`). Apply these
changes:

- [ ] **Step 1: Module ports — 4 bundles**

```systemverilog
module router_channel_wrap #(
    parameter int unsigned NUM_VC                = ni_params_pkg::NI_NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
) (
    input  logic   clk_i,
    input  logic   rst_ni,
    input  chandle ctx_i,
    noc_intf.miso  node0_nmu_i,   // node0 NMU-facing (NMU drives req, wrap drives rsp)
    noc_intf.mosi  node0_nsu_o,   // node0 NSU-facing (wrap drives req, NSU drives rsp)
    noc_intf.miso  node1_nmu_i,
    noc_intf.mosi  node1_nsu_o
);
```
Keep the `NUM_VC != 1 → $fatal` guard from `channel_model_wrap.sv`.

- [ ] **Step 2: DPI imports — the 2-node signatures from Task 3**

`import "DPI-C" context function void cmodel_router_channel_set_inputs(...)` and
`...get_outputs(...)` with exactly the `_n0`/`_n1` argument lists from Task 3 Step 2 (12 args
each: per node `{req_in_valid, req_in_flit[FLIT_WIDTH-1:0], req_in_credit_return, rsp_in_valid,
rsp_in_flit[FLIT_WIDTH-1:0], rsp_in_credit_return}` for set_inputs; the `*_out_*` mirror for
get_outputs). Plus `cmodel_router_channel_tick(chandle)`.

- [ ] **Step 3: Per-node signal mapping (the only non-mechanical part)**

Per node `n`, the mapping mirrors `channel_model_wrap.sv:115-170` exactly with the node's two
bundles (`nmu_i` = the miso bundle, `nsu_o` = the mosi bundle):

| DPI arg (set_inputs) | wire |
|---|---|
| `req_in_valid_nN` | `nodeN_nmu_i.req_valid` |
| `req_in_flit_nN` | `nodeN_nmu_i.req_flit` |
| `req_in_credit_return_nN` | `nodeN_nsu_o.req_credit_return[0]` |
| `rsp_in_valid_nN` | `nodeN_nsu_o.rsp_valid` |
| `rsp_in_flit_nN` | `nodeN_nsu_o.rsp_flit` |
| `rsp_in_credit_return_nN` | `nodeN_nmu_i.rsp_credit_return[0]` |

| get_outputs `*_q` → drive | wire |
|---|---|
| `req_out_valid_nN_q` | `nodeN_nsu_o.req_valid` |
| `req_out_flit_nN_q` | `nodeN_nsu_o.req_flit` |
| `req_out_credit_return_nN_q` | `nodeN_nmu_i.req_credit_return = {NUM_VC{...}}` |
| `rsp_out_valid_nN_q` | `nodeN_nmu_i.rsp_valid` |
| `rsp_out_flit_nN_q` | `nodeN_nmu_i.rsp_flit` |
| `rsp_out_credit_return_nN_q` | `nodeN_nsu_o.rsp_credit_return = {NUM_VC{...}}` |

Declare the `*_q` output registers for both nodes; clear them all on `!rst_ni`; in the active
branch call `set_inputs` (both nodes' wires) → `tick` → `get_outputs` (into per-node temporaries,
then nonblocking-assign to `*_q`), exactly as `channel_model_wrap.sv` does for one node.

- [ ] **Step 4: Register in the tb_top build sources**

In `cosim/sources.mk`, the tb_top source list (the `TB_TOP_*` block, ~line 23-35) lists
`$(COSIM_ROOT)/sv/channel_model_wrap.sv`. The rewritten tb_top no longer instantiates it — swap
that line to `$(COSIM_ROOT)/sv/router_channel_wrap.sv`. Leave `channel_model_wrap.sv` on disk
(unused by tb_top now; the file itself is not deleted).

- [ ] **Step 5: Lint-only elaboration check** (full build happens in Task 8)

`router_channel_wrap.sv` is elaborated as part of `Vtb_top` in Task 8; no standalone build here.
Sanity-read against `channel_model_wrap.sv` to confirm every `*_q` is driven and every DPI arg
is wired.

- [ ] **Step 6: Commit**

```bash
git add cosim/sv/router_channel_wrap.sv cosim/sources.mk
git commit -m "feat(cosim): router_channel_wrap.sv (4-bundle 2-node beta-tick DPI wrapper)"
```

---

## Task 7: tb_top.sv bidirectional rewrite

**Files:**
- Rewrite: `cosim/sv/tb_top.sv`

Read the current `cosim/sv/tb_top.sv`. The rewrite duplicates the component pipeline per node,
drops the two wb2axip protocol checkers, swaps `channel_model_wrap` for `router_channel_wrap`, and adds the
non-vacuous PASS guard. Keep the module port `(clk_i, rst_ni)`, the localparams, and the
centralized DPI error poll block unchanged.

- [ ] **Step 1: Two scenario plusargs + two-of-each create**

```systemverilog
string  scn_node0;  // drives node0.master (targets node1 -> high addr)
string  scn_node1;  // drives node1.master (targets node0 -> low addr)
chandle rc_ctx, m0_ctx, s0_ctx, n0_nmu_ctx, n0_nsu_ctx;
chandle             m1_ctx, s1_ctx, n1_nmu_ctx, n1_nsu_ctx;

import "DPI-C" context function chandle cmodel_router_channel_create(input string name);
import "DPI-C" context function chandle cmodel_master_create(input string name,
                                                             input string scenario_path);
import "DPI-C" context function chandle cmodel_slave_create(input string name,
                                                            input string scenario_path);
import "DPI-C" context function chandle cmodel_nmu_create(input string name, input int src_id);
import "DPI-C" context function chandle cmodel_nsu_create(input string name, input int src_id);
import "DPI-C" context function int cmodel_master_count();
import "DPI-C" context function int cmodel_reads_checked();

initial begin
    if (!$value$plusargs("scenario_node0=%s", scn_node0) ||
        !$value$plusargs("scenario_node1=%s", scn_node1)) begin
        $display("ERROR: +scenario_node0=<path> +scenario_node1=<path> required");
        $finish(1);
    end
    cmodel_init(scn_node1);  // shared config (latencies, max_outstanding); either variant is fine
    rc_ctx     = cmodel_router_channel_create("router_channel_0");
    // node0.master drives node1-variant (high addr, targets (1,0)); its req ejects at node1.NSU.
    m0_ctx     = cmodel_master_create("master_0", scn_node1);
    s1_ctx     = cmodel_slave_create ("slave_1",  scn_node1);  // node1.slave covers the high range
    n0_nmu_ctx = cmodel_nmu_create("nmu_0", 0);                // src_id = node0 coordinate
    n0_nsu_ctx = cmodel_nsu_create("nsu_0", 0);
    // node1.master drives node0-variant (low addr, targets (0,0)); its req ejects at node0.NSU.
    m1_ctx     = cmodel_master_create("master_1", scn_node0);
    s0_ctx     = cmodel_slave_create ("slave_0",  scn_node0);  // node0.slave covers the low range
    n1_nmu_ctx = cmodel_nmu_create("nmu_1", 1);                // src_id = node1 coordinate
    n1_nsu_ctx = cmodel_nsu_create("nsu_1", 1);
end
```
**Wiring identity (spec §2/§5/§8):** master at node `k` is fed the OTHER node's coordinate
variant so it targets across the link. node0.master←node1-variant (high) ejects at node1.NSU →
`slave_1` uses `scn_node1`. node1.master←node0-variant (low) ejects at node0.NSU → `slave_0` uses
`scn_node0`. `NmuShellAdapter::init(uint8_t src_id, ...)` already takes src_id (default 0); the
only NMU-side change is `cmodel_nmu_create` gaining a `src_id` arg (Task 4) — node0 NMU src_id=0,
node1 NMU src_id=1 — so each node's responses route back to it (NSU stamps response dst_id from
request src_id).

- [ ] **Step 2: Bundles + instances (2 nodes)**

Declare, per node `k ∈ {0,1}`: one `axi4_intf master_nmu_axi_k`, one `axi4_intf nsu_slave_axi_k`,
one `noc_intf nodeK_nmu`, one `noc_intf nodeK_nsu` (params `NUM_VC/FLIT_WIDTH/SLAVE_VC_BUFFER_DEPTH`).
Instantiate per node, copying the param lists from the current `tb_top.sv` instances:
`axi_master_wrap u_master_k(.ctx_i(mK_ctx), .axi_o(master_nmu_axi_k.master))`,
`nmu_wrap u_nmu_k(.ctx_i(nK_nmu_ctx), .axi_i(master_nmu_axi_k.slave), .noc_mosi_o(nodeK_nmu.mosi))`,
`nsu_wrap u_nsu_k(.ctx_i(nK_nsu_ctx), .noc_miso_i(nodeK_nsu.miso), .axi_o(nsu_slave_axi_k.master))`,
`axi_slave_wrap u_slave_k(.ctx_i(sK_ctx), .axi_i(nsu_slave_axi_k.slave))`. One
`router_channel_wrap u_rc(.ctx_i(rc_ctx), .node0_nmu_i(node0_nmu.miso), .node0_nsu_o(node0_nsu.mosi),
.node1_nmu_i(node1_nmu.miso), .node1_nsu_o(node1_nsu.mosi))`. **Delete both wb2axip protocol-checker blocks and their
induction-output wire declarations.**

- [ ] **Step 3: Non-vacuous PASS guard in the exit poll**

```systemverilog
always @(posedge clk_i) begin
    /* verilator lint_off WIDTHTRUNC */
    if (rst_ni && (cmodel_done() != 0)) begin
        if (cmodel_scoreboard_clean() != 0 &&
            cmodel_master_count() == 2 && cmodel_reads_checked() > 0) begin
    /* verilator lint_on WIDTHTRUNC */
            $display("PASS: scenario complete, scoreboard clean");
            cmodel_dump_scoreboard();
            $finish(0);
        end else begin
            $display("FAIL: scoreboard mismatch or vacuous run (masters=%0d reads=%0d)",
                     cmodel_master_count(), cmodel_reads_checked());
            cmodel_dump_scoreboard();
            $fatal(1, "tb_top: bidirectional run failed");
        end
    end
end
```
Keep the existing centralized `cmodel_check_error` poll block unchanged.

- [ ] **Step 4: Build Vtb_top (verify elaboration)**

```bash
MSYSTEM=MINGW64 LC_ALL=C /c/msys64/usr/bin/bash -lc \
  'cd /e/05_NoC/noc_project/build/verilator && make Vtb_top 2>&1 | tail -20'
```
Expected: builds clean. Fix elaboration errors (unconnected ports, modport mismatches) before
moving on. Do NOT run scenarios yet — that's Task 8 (needs the variant files).

- [ ] **Step 5: Commit**

```bash
git add cosim/sv/tb_top.sv
git commit -m "feat(cosim): rewrite tb_top as bidirectional 2-node router co-sim (no wb2axip checkers)"
```
(The DPI signature changes this tb_top depends on — `cmodel_master/slave/nmu_create` args,
`cmodel_master_count`/`cmodel_reads_checked` — already landed in Tasks 3-4.)

---

## Task 8: Bidirectional CosimIntegration + build wiring

**Files:**
- Modify: `cosim/tests/CMakeLists.txt` (generate variants; pass variant root)
- Modify: `cosim/tests/test_cosim_integration.cpp`
- Modify: `cosim/tests/wb2axip_block.hpp` if a read-bearing-subset predicate is needed

- [ ] **Step 1: Generate variants at configure/build time**

In `cosim/tests/CMakeLists.txt`, generate the variants into the build tree with a proper
output-dependency (not a bare `ALL` target — that does not order before the test). Use
`find_package(Python3)` so the generator runs under an interpreter that actually has PyYAML
(`py -3` on Windows may point at a Python without it), and fail configure early if `import yaml`
is absent. Reuse the existing `SCENARIO_TREE_ABS` for the source root.

```cmake
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} -c "import yaml"
                RESULT_VARIABLE _yaml_rc)
if(NOT _yaml_rc EQUAL 0)
    message(FATAL_ERROR "scenario generator needs PyYAML: pip install pyyaml for ${Python3_EXECUTABLE}")
endif()

set(COORD_VARIANT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/coord_scenarios")
set(_coord_outputs "")
foreach(id IN LISTS COSIM_BIDIR_SUBSET)   # the read-bearing wb2axip-runnable id list
    set(_out "${COORD_VARIANT_ROOT}/${id}/node1/scenario.yaml")
    add_custom_command(
        OUTPUT "${_out}" "${COORD_VARIANT_ROOT}/${id}/node0/scenario.yaml"
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cosim/tools/gen_coordinate_scenarios.py
                "${SCENARIO_TREE_ABS}/${id}/scenario.yaml" "${COORD_VARIANT_ROOT}/${id}"
        DEPENDS ${CMAKE_SOURCE_DIR}/cosim/tools/gen_coordinate_scenarios.py
                "${SCENARIO_TREE_ABS}/${id}/scenario.yaml"
        COMMENT "Generating coordinate variants for ${id}")
    list(APPEND _coord_outputs "${_out}")
endforeach()
add_custom_target(gen_coord_scenarios DEPENDS ${_coord_outputs})
add_dependencies(test_cosim_integration gen_coord_scenarios)
target_compile_definitions(test_cosim_integration PRIVATE
    COORD_VARIANT_ROOT="${COORD_VARIANT_ROOT}")
```
`COSIM_BIDIR_SUBSET` is the read-bearing wb2axip-runnable id list (define it in this file; cross-
check against `noc::tests::kAllAxi4Scenarios` filtered by Step 2's predicate). The test reads
`COORD_VARIANT_ROOT` from the compile def.

- [ ] **Step 2: Restrict the subset to read-bearing wb2axip-runnable patterns**

The non-vacuous guard requires `reads_checked > 0`, so write-only scenarios (e.g. BAS-001) are
excluded. In `test_cosim_integration.cpp`, after the existing `wb2axip_block_reason` skip, add:

```cpp
bool has_read = std::any_of(sc.transactions.begin(), sc.transactions.end(),
    [](const ni::cmodel::axi::ScenarioTransaction& t) {
        return t.op == ni::cmodel::axi::ScenarioTransaction::Op::Read;
    });
if (!has_read) GTEST_SKIP() << "BIDIR_REQUIRES_READ";
```

- [ ] **Step 3: Drive Vtb_top with both variant paths**

Replace the single-scenario invocation:

```cpp
auto base = std::string(COORD_VARIANT_ROOT) + "/" + scenario_id;
auto cmd = std::string(bin) +
           " +scenario_node0=" + base + "/node0/scenario.yaml" +
           " +scenario_node1=" + base + "/node1/scenario.yaml";
auto result = run_and_capture(cmd);
EXPECT_EQ(result.rc, 0) << result.output;
EXPECT_NE(result.output.find(kPassMarker), std::string::npos) << result.output;
```

- [ ] **Step 4: Update the Makefile manual-run target**

`cosim/verilator/Makefile`'s `run-tb-top` (~line 134-139) passes a single
`+scenario=$(SCENARIO_ABS)`; the bidirectional tb_top now requires two plusargs. The VCD trace
loop `run-all-tb-top` (~line 295) inherits it. Update `run-tb-top` to generate the variants for
`$(SCENARIO)` (invoke `cosim/tools/gen_coordinate_scenarios.py` into a local dir) and pass
`+scenario_node0=<dir>/node0/scenario.yaml +scenario_node1=<dir>/node1/scenario.yaml`. This
keeps manual runs and the trace loop working; the authoritative ctest path is unaffected (it
builds the command itself in Step 3).

- [ ] **Step 5: Build + run the bidirectional integration**

```bash
cd /e/05_NoC/noc_project/build/cmodel && export PATH="/c/Windows/System32:$PATH"
cmake --build . -j 4 2>&1 | tail -5
MSYSTEM=MINGW64 LC_ALL=C /c/msys64/usr/bin/bash -lc \
  'cd /e/05_NoC/noc_project/build/verilator && make Vtb_top 2>&1 | tail -5'
cd /e/05_NoC/noc_project/build/cmodel && ctest -R CosimIntegration --output-on-failure 2>&1 | tail -25
```
Expected: the read-bearing wb2axip subset PASSES bidirectionally through the real router;
write-only and wb2axip-blocked scenarios SKIP; 0 FAIL.

- [ ] **Step 6: Commit**

```bash
git add cosim/tests/CMakeLists.txt cosim/tests/test_cosim_integration.cpp cosim/verilator/Makefile
git commit -m "test(cosim): bidirectional CosimIntegration over coordinate variants"
```

---

## Self-Review notes (cross-check before execution)

- **src_id wiring (spec §8):** resolved — `NmuShellAdapter::init(uint8_t src_id, ...)` already
  takes src_id; Task 4 Step 3 adds the `src_id` arg to `cmodel_nmu_create`, and Task 7 sets node0
  NMU=0 / node1 NMU=1. Getting this wrong (both at 0) collides response routing, so keep the two
  create calls' src_ids matched to their node coordinates.
- **num_vc=1:** every NoC bundle and `RouterChannel(num_vc=1)` must agree; the SV wrapper guard
  enforces it at elaboration.
- **Variant pairing:** node1.master↔node0.slave (low), node0.master↔node1.slave (high). Getting
  this backwards routes traffic out of mesh (abort). Task 7 Step 1 documents the identity.
- **Vacuous guard depends on the subset having reads** (Task 8 Step 2) — keep both in sync.
- **Credit depth (Codex B1):** default `vc_depth=4` is too shallow for the beta-tick credit lag;
  Task 2 pins it to `kPoCChannelModelDepth` and throws on a rejected push so a silent drop fails
  loudly. NSU also needs its node src_id (Codex S1) — Task 4 Step 3 covers both NMU and NSU.
- **Build wiring (Codex B2/S2/S3/S4):** `cosim/sources.mk` must swap in `router_channel_wrap.sv`
  (Task 6 Step 4); the Makefile `run-tb-top` must pass two plusargs (Task 8 Step 4); the variant
  generator runs under `find_package(Python3)` with a PyYAML check and an `add_custom_command`
  output dependency so the test cannot run before variants exist (Task 8 Step 1).
- **Relocatability (Codex N1):** the generator rewrites `data_file` to absolute paths (same as
  `test_router_loopback`'s `shifted_scenario_path`), so the generated variant tree is not portable
  across machines — acceptable for an in-tree build artifact; revisit if variants are committed.
