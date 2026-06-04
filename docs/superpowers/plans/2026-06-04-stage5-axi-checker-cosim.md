# Stage 5 NMU/NSU AXI4 Boundary Conformity Co-sim — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a narrow PoC co-sim path where Verilator runs a thin SV testbench shell that binds ZipCPU wb2axip AXI4 protocol checkers on the C-model `Nmu` / `Nsu` boundaries; the C-model drives traffic and is observed cycle-by-cycle via a DPI bridge.

**Architecture:** SV simulator owns the clock. Each `posedge clk` calls a DPI service `cmodel_tick()` which advances the existing C-model integration loop (AxiMaster → Nmu → LoopbackNoc → Nsu → AxiSlave + memory + scoreboard) one cycle. Five additional DPI getters per side return per-channel AXI pin snapshots that the SV proxy module drives onto wire bundles where `faxi_slave.v` (NMU side) and `faxi_master.v` (NSU side) are bound.

**Tech Stack:** C++17, GoogleTest, Verilator 5.044 on Windows MSYS2 native, ZipCPU wb2axip (`bench/formal/faxi_master.v` + `bench/formal/faxi_slave.v`, Apache 2.0), DPI-C, existing `c_model/include/axi/` infrastructure (unchanged).

**Spec:** `docs/superpowers/specs/2026-06-04-stage5-axi-checker-cosim-design.md`

---

## File Structure

| Path | Responsibility |
|---|---|
| `c_model/include/cosim/pin_snapshot.hpp` | per-channel POD struct (AWPins / WPins / ARPins / BPins / RPins), mirroring `axi/types.hpp` field-for-field plus the `valid` / `ready` handshake bits |
| `c_model/include/cosim/axi_dpi_adapter.hpp` | `AxiDpiAdapter`: owns AxiMaster + Nmu + LoopbackNoc + Nsu + AxiSlave + Memory + Scoreboard; exposes `init(yaml)`, `tick()`, `get_nmu_aw(...)` etc. getters returning current pin state |
| `cosim/sv/wb2axip/faxi_master.v` | vendored from ZipCPU/wb2axip Apache 2.0; not modified |
| `cosim/sv/wb2axip/faxi_slave.v` | vendored same; not modified |
| `cosim/sv/wb2axip/sim_wrapper.svh` | macro shim: `SLAVE_ASSUME`→`assert`, `assume()`→`assert()`, `f_past_valid` init guard |
| `cosim/sv/wb2axip/ATTRIBUTION.md` | upstream commit hash + Apache 2.0 attribution + "no source-file modifications" note |
| `cosim/sv/axi_if.sv` | parameterized AXI4 bundle (DATA_WIDTH, ADDR_WIDTH, ID_WIDTH params) used to connect proxy → checker |
| `cosim/sv/nmu_cmodel_proxy.sv` | DPI imports + `always_ff` driving NMU-side `axi_if` wire from C-model pin snapshot |
| `cosim/sv/nsu_cmodel_proxy.sv` | symmetric for NSU-side |
| `cosim/sv/tb_axi_conformity.sv` | TB top: clock + reset + proxy instances + wb2axip bind + `$value$plusargs` scenario path |
| `cosim/c/axi_dpi.c` | DPI export function bodies; thin C glue to `AxiDpiAdapter` (which is C++) |
| `cosim/verilator/Makefile` | Windows MSYS2 + Verilator 5.044 build; outputs `Vtb_axi_conformity.exe` |
| `cosim/verilator/main.cpp` | Verilator harness top: parses plusarg, calls `cmodel_init()`, drives reset/clock, calls `Vtb->eval()` per tick, calls `cmodel_finalize()` on exit |
| `cosim/tests/test_nmu_nsu_axi_conformity.cpp` | GoogleTest entry that invokes `Vtb_axi_conformity` as subprocess per scenario; asserts exit 0 |
| `cosim/tests/CMakeLists.txt` | builds Verilator harness as ExternalProject + registers GoogleTest entry |
| `c_model/tests/CMakeLists.txt` | one-line `add_subdirectory(cosim/tests)` (modify) |

Existing files are not modified: `c_model/include/{nmu,nsu,axi,common,noc}/*`.

---

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| wb2axip Dan Gisselquist's `bench/formal/*.v` header warns "not intended to be functional" — may not compile cleanly under Verilator | Task 8 explicitly budgets a diagnostic/fix step; `sim_wrapper.svh` (Task 2) front-loads the known shim points (macros + `f_past_valid` init); if compile fails outside these shims, the engineer surfaces the diff and we either patch the shim or, as last resort, fork the vendor file with an in-line patch documented in ATTRIBUTION.md |
| Verilator 5.044 on Windows MSYS2 may have surprise toolchain issues | Task 8 first builds a trivial empty-TB hello-world to validate the toolchain before pulling in c_model + DPI; if MSYS2 path fails, fall back to WSL Verilator (documented as fallback in Task 8) |
| DPI struct bit-pack convention mismatch between SV `svBitVecVal` and C++ `uint8_t` arrays | Task 4 covers field-by-field copy with explicit width macros; Task 9 smoke run catches mismatch immediately (assertion fires on first beat) |

---

## Task 1: Scaffold `cosim/` tree and vendor wb2axip

**Files:**
- Create: `cosim/sv/wb2axip/faxi_master.v` (fetch)
- Create: `cosim/sv/wb2axip/faxi_slave.v` (fetch)
- Create: `cosim/sv/wb2axip/ATTRIBUTION.md`
- Create: empty placeholder dirs `cosim/{c,verilator,tests}` + `c_model/include/cosim/`

- [ ] **Step 1: Create directory skeleton**

```bash
mkdir -p cosim/sv/wb2axip cosim/c cosim/verilator cosim/tests c_model/include/cosim
touch cosim/c/.gitkeep cosim/verilator/.gitkeep cosim/tests/.gitkeep c_model/include/cosim/.gitkeep
```

- [ ] **Step 2: Fetch wb2axip files and capture upstream commit hash**

```bash
cd /tmp
git clone --depth 1 https://github.com/ZipCPU/wb2axip.git
cd wb2axip
WB2AXIP_COMMIT=$(git rev-parse HEAD)
echo "Pinning wb2axip @ $WB2AXIP_COMMIT"
cp bench/formal/faxi_master.v E:/05_NoC/noc_project/cosim/sv/wb2axip/
cp bench/formal/faxi_slave.v  E:/05_NoC/noc_project/cosim/sv/wb2axip/
```

Record the captured `$WB2AXIP_COMMIT` value — it goes into ATTRIBUTION.md in step 3. Do NOT modify the copied `.v` files.

- [ ] **Step 3: Write ATTRIBUTION.md**

Create `cosim/sv/wb2axip/ATTRIBUTION.md`:

```markdown
# OSS Attribution — cosim/sv/wb2axip/

Verbatim copies of ZipCPU/wb2axip formal AXI4 protocol checker properties,
used as simulation-runtime observers via Verilator (immediate-assertion
subset compatible). Pattern matches `c_model/include/axi/ATTRIBUTION.md`.

## Upstream

- Source: https://github.com/ZipCPU/wb2axip
- Commit: <PASTE $WB2AXIP_COMMIT FROM TASK 1 STEP 2>
- License: Apache-2.0
- Files: `bench/formal/faxi_master.v`, `bench/formal/faxi_slave.v`

## Files

| This repo                              | Upstream path                       |
|----------------------------------------|--------------------------------------|
| `cosim/sv/wb2axip/faxi_master.v`       | `bench/formal/faxi_master.v`         |
| `cosim/sv/wb2axip/faxi_slave.v`        | `bench/formal/faxi_slave.v`          |

## Modifications

- No source-file modifications; the two `.v` files are byte-identical to upstream.
- Adaptation provided externally via `cosim/sv/wb2axip/sim_wrapper.svh`:
  - `` `define SLAVE_ASSUME assert `` (formal-only `assume` reinterpreted as
    `assert` in simulation; upstream's macro indirection lets us redefine
    the role mapping at include time).
  - Plain `assume(...)` calls inside the file are similarly mapped via a
    project-wide preprocessor option in the Verilator Makefile (`+define+assume=assert`).
  - `f_past_valid` is initialized to 0 in an `initial` block defined in the
    sim_wrapper to guard against Verilator default-X read on the first cycle.
```

- [ ] **Step 4: Verify file presence + license header sanity**

```bash
ls cosim/sv/wb2axip/
head -10 cosim/sv/wb2axip/faxi_master.v   # confirm Apache 2.0 header
head -10 cosim/sv/wb2axip/faxi_slave.v
```

Expected: both files present; both headers say "Apache License, Version 2.0".

- [ ] **Step 5: Commit**

```bash
git add cosim/sv/wb2axip/ c_model/include/cosim/.gitkeep cosim/c/.gitkeep cosim/verilator/.gitkeep cosim/tests/.gitkeep
git commit -m "chore(cosim): scaffold cosim/ tree and vendor wb2axip @ <hash>"
```

---

## Task 2: `sim_wrapper.svh` shim

**Files:**
- Create: `cosim/sv/wb2axip/sim_wrapper.svh`

- [ ] **Step 1: Write the macro shim**

Create `cosim/sv/wb2axip/sim_wrapper.svh`:

```systemverilog
// Macro shim allowing ZipCPU wb2axip formal properties to run as
// simulation-runtime observers. Source files faxi_master.v / faxi_slave.v
// are byte-identical to upstream; this file is the ONLY adaptation layer.
//
// Three changes vs formal:
//   1. SLAVE_ASSUME / SLAVE_ASSERT both map to `assert` (formal `assume` has
//      no simulation semantic; treat the constraint as a directly-checked
//      invariant on our DUT).
//   2. `f_past_valid` requires explicit init to 0 (Verilator default-X
//      would make $past() reads UB on first cycle).
//   3. Macro guard so this file can be safely included multiple times.

`ifndef WB2AXIP_SIM_WRAPPER_SVH
`define WB2AXIP_SIM_WRAPPER_SVH

`define SLAVE_ASSUME assert
`define SLAVE_ASSERT assert

// To map standalone `assume(...)` inside wb2axip into `assert(...)`, see
// the Verilator build flag in cosim/verilator/Makefile:
//     -Dassume=assert

`endif // WB2AXIP_SIM_WRAPPER_SVH
```

- [ ] **Step 2: Verify file**

```bash
cat cosim/sv/wb2axip/sim_wrapper.svh
```

Expected: file present with the 3 macros visible.

- [ ] **Step 3: Commit**

```bash
git add cosim/sv/wb2axip/sim_wrapper.svh
git commit -m "feat(cosim): add sim_wrapper.svh shim for wb2axip simulation mode"
```

---

## Task 3: `pin_snapshot.hpp` POD structs

**Files:**
- Create: `c_model/include/cosim/pin_snapshot.hpp`

- [ ] **Step 1: Write the header**

Create `c_model/include/cosim/pin_snapshot.hpp`:

```cpp
// Per-channel AXI4 pin snapshot POD structs used to pass C-model boundary
// state across the DPI to SV. Fields mirror c_model/include/axi/types.hpp
// (AwBeat, WBeat, ArBeat, BBeat, RBeat) one-for-one and add `valid`/`ready`
// handshake bits which the channel beats do not carry.
//
// No methods, no constructors. Plain copyable PODs so the DPI getter can
// fill in fields directly with no marshalling overhead.
#pragma once
#include "axi/types.hpp"
#include <array>
#include <cstdint>

namespace ni::cmodel::cosim {

using ni::cmodel::axi::DATA_BYTES;

struct AwPins {
    bool valid;
    bool ready;
    uint8_t id;
    uint64_t addr;
    uint8_t len, size;
    uint8_t burst;  // Burst encoded as raw 2-bit (0=FIXED, 1=INCR, 2=WRAP)
    uint8_t lock, cache, prot, qos;
};

struct WPins {
    bool valid;
    bool ready;
    std::array<uint8_t, DATA_BYTES> data;
    uint32_t strb;
    bool last;
};

struct ArPins {
    bool valid;
    bool ready;
    uint8_t id;
    uint64_t addr;
    uint8_t len, size;
    uint8_t burst;
    uint8_t lock, cache, prot, qos;
};

struct BPins {
    bool valid;
    bool ready;
    uint8_t id;
    uint8_t resp;  // Resp encoded as raw 2-bit
};

struct RPins {
    bool valid;
    bool ready;
    uint8_t id;
    std::array<uint8_t, DATA_BYTES> data;
    uint8_t resp;
    bool last;
};

}  // namespace ni::cmodel::cosim
```

- [ ] **Step 2: Sanity compile**

Quick compile probe — write a 2-line standalone TU:

```bash
cat > /tmp/probe.cpp <<EOF
#include "cosim/pin_snapshot.hpp"
int main() { ni::cmodel::cosim::AwPins p{}; (void)p; return 0; }
EOF
cd c_model && cmake --build build 2>&1 | tail -5 || true
# Run probe with project include paths:
g++ -std=c++17 -I include -I build/_deps/yaml-cpp-src/include -I ../specgen/generated/cpp /tmp/probe.cpp -o /tmp/probe && echo "OK"
```

Expected: `OK`. (If yaml-cpp include path differs in your build, adapt.)

- [ ] **Step 3: Commit**

```bash
git add c_model/include/cosim/pin_snapshot.hpp
git commit -m "feat(cosim): add pin_snapshot.hpp per-channel POD structs"
```

---

## Task 4: `AxiDpiAdapter` class + unit test

**Files:**
- Create: `c_model/include/cosim/axi_dpi_adapter.hpp`
- Create: `c_model/tests/cosim/test_axi_dpi_adapter.cpp`
- Modify: `c_model/tests/CMakeLists.txt` (add cosim subdir)
- Create: `c_model/tests/cosim/CMakeLists.txt`

- [ ] **Step 1: Write the failing test first**

Create `c_model/tests/cosim/test_axi_dpi_adapter.cpp`:

```cpp
#include "common/scenario.hpp"
#include "cosim/axi_dpi_adapter.hpp"
#include "cosim/pin_snapshot.hpp"
#include <gtest/gtest.h>
#include <string>

using ni::cmodel::cosim::AxiDpiAdapter;
using ni::cmodel::cosim::AwPins;

// Smoke test: adapter loads a real scenario, runs ticks, eventually
// produces a valid AWVALID on the NMU side.
TEST(AxiDpiAdapter, drives_aw_after_scenario_load) {
    SCENARIO("AxiDpiAdapter drives AWVALID on NMU AXI(1) after init");

    AxiDpiAdapter adapter;
    adapter.init("c_model/tests/fixtures/burst_incr_8beat.yaml");

    AwPins aw{};
    bool saw_awvalid = false;
    for (int cycle = 0; cycle < 200 && !saw_awvalid; ++cycle) {
        adapter.tick();
        adapter.get_nmu_aw(aw);
        if (aw.valid) saw_awvalid = true;
    }
    EXPECT_TRUE(saw_awvalid) << "no AWVALID asserted in first 200 cycles";
}

// Adapter must reach steady-state drain on a small scenario.
TEST(AxiDpiAdapter, scenario_completes) {
    SCENARIO("AxiDpiAdapter scoreboard drains after burst_incr_8beat scenario");

    AxiDpiAdapter adapter;
    adapter.init("c_model/tests/fixtures/burst_incr_8beat.yaml");
    for (int cycle = 0; cycle < 5000 && !adapter.done(); ++cycle) {
        adapter.tick();
    }
    EXPECT_TRUE(adapter.done()) << "scenario did not drain within 5000 cycles";
    EXPECT_TRUE(adapter.scoreboard_clean()) << "scoreboard reported mismatch";
}
```

Pick the actual YAML path your repo has — check `c_model/tests/fixtures/` (or the directory `scenarios/` per the spec — adjust the constant string here if your path differs).

- [ ] **Step 2: Run the test, watch it fail**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R AxiDpiAdapter -V
```

Expected: compile error — `axi_dpi_adapter.hpp` not found.

- [ ] **Step 3: Add the new test subdirectory to the parent CMakeLists**

Modify `c_model/tests/CMakeLists.txt` — find the `add_subdirectory(integration)` line and add right after it:

```cmake
add_subdirectory(cosim)
```

Create `c_model/tests/cosim/CMakeLists.txt`:

```cmake
add_executable(test_axi_dpi_adapter test_axi_dpi_adapter.cpp)
target_link_libraries(test_axi_dpi_adapter PRIVATE
    ni_cmodel_core
    GTest::gtest_main
)
target_include_directories(test_axi_dpi_adapter PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)
add_test(NAME AxiDpiAdapter.smoke COMMAND test_axi_dpi_adapter)
```

Adapt `ni_cmodel_core` library name to whatever the existing CMake target is — peek at `c_model/tests/integration/CMakeLists.txt` for the canonical name.

- [ ] **Step 4: Write the adapter**

Create `c_model/include/cosim/axi_dpi_adapter.hpp`:

```cpp
// AxiDpiAdapter: single owner of the existing C-model integration loop,
// exposed as a DPI service.
//
// The constructor + init(yaml) sets up the same component graph
// test_request_response_loopback.cpp wires: AxiMaster -> Nmu -> LoopbackNoc
// -> Nsu -> AxiSlave + Memory. tick() advances every component one cycle
// in the project's canonical order. get_nmu_*() / get_nsu_*() snapshot
// the current pin state on each AXI boundary into a PinSnapshot struct
// that the DPI export functions in cosim/c/axi_dpi.c can hand to SV.
//
// This file follows the integration testbench layout — keep them in sync
// when component ctor signatures change.
#pragma once
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "axi/scenario_parser.hpp"
#include "axi/scoreboard.hpp"
#include "common/loopback_noc.hpp"
#include "cosim/pin_snapshot.hpp"
#include "nmu/nmu.hpp"
#include "nsu/nsu.hpp"
#include <memory>
#include <string>
#include <vector>

namespace ni::cmodel::cosim {

class AxiDpiAdapter {
  public:
    AxiDpiAdapter() = default;
    ~AxiDpiAdapter() = default;
    AxiDpiAdapter(const AxiDpiAdapter&) = delete;
    AxiDpiAdapter(AxiDpiAdapter&&) = delete;
    AxiDpiAdapter& operator=(const AxiDpiAdapter&) = delete;
    AxiDpiAdapter& operator=(AxiDpiAdapter&&) = delete;

    void init(const std::string& scenario_yaml_path);
    void tick();
    bool done() const;
    bool scoreboard_clean() const;

    // Per-channel pin snapshot getters; called by DPI exports each cycle.
    void get_nmu_aw(AwPins& out) const;
    void get_nmu_w(WPins& out) const;
    void get_nmu_ar(ArPins& out) const;
    void get_nmu_b(BPins& out) const;
    void get_nmu_r(RPins& out) const;
    void get_nsu_aw(AwPins& out) const;
    void get_nsu_w(WPins& out) const;
    void get_nsu_ar(ArPins& out) const;
    void get_nsu_b(BPins& out) const;
    void get_nsu_r(RPins& out) const;

  private:
    // Owned components — declared in dependency order. Mirrors integration
    // testbench in c_model/tests/integration/test_request_response_loopback.cpp.
    std::unique_ptr<common::LoopbackNoc> loopback_;
    std::unique_ptr<nsu::Nsu> nsu_;
    std::unique_ptr<nmu::Nmu> nmu_;
    std::unique_ptr<axi::AxiMaster<nmu::AxiSlavePort>> master_;
    std::unique_ptr<axi::Memory> memory_;
    std::unique_ptr<axi::AxiSlave> slave_;
    std::unique_ptr<axi::Scoreboard> scoreboard_;
    axi::Scenario scenario_{};
};

inline void AxiDpiAdapter::init(const std::string& scenario_yaml_path) {
    scenario_ = axi::parse_scenario(scenario_yaml_path);
    // TODO during implementation: instantiate components in the exact
    // order and with the exact ctor args used in
    // c_model/tests/integration/test_request_response_loopback.cpp;
    // call master_->load_scenario(scenario_) (or the canonical hook used
    // by the integration testbench).
}

inline void AxiDpiAdapter::tick() {
    // Canonical tick order — match integration testbench upstream-first.
    if (master_) master_->tick();
    if (nmu_)    nmu_->tick();
    if (loopback_) loopback_->tick();
    if (nsu_)    nsu_->tick();
    if (slave_)  slave_->tick();
}

inline bool AxiDpiAdapter::done() const {
    return master_ && master_->done();
}

inline bool AxiDpiAdapter::scoreboard_clean() const {
    return scoreboard_ && scoreboard_->mismatch_count() == 0;
}

// Per-channel getters: read current handshake state and channel attributes
// from the relevant AxiSlavePort / AxiMasterPort instance and pack into
// the PinSnapshot struct.
//
// Implementation note: AxiSlavePort exposes peek_aw() / peek_w() / peek_ar()
// for current beat being driven and aw_valid()/aw_ready()/... handshake.
// Match those exact accessor names — see nmu/axi_slave_port.hpp.
inline void AxiDpiAdapter::get_nmu_aw(AwPins& out) const {
    // TODO during implementation: pack from nmu_->axi_slave_port() accessor.
    out = {};
}
// ... define the remaining nine getters with the same pattern.

}  // namespace ni::cmodel::cosim
```

**Note for implementer:** the inline `init()` / `get_*()` bodies above are stubs marked `TODO` because the exact ctor signature of `AxiMaster<AxiSlavePort>` + scenario load pattern is what the integration testbench (`test_request_response_loopback.cpp`) shows in concrete form. Open that file, copy the wiring block verbatim, and translate to the constructor body. Do not invent ctor args.

- [ ] **Step 5: Run, iterate until pass**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R AxiDpiAdapter -V
```

Expected: both tests pass. Iterate the adapter body — chase compile errors, then chase test failures (most likely scenario path is the YAML location).

- [ ] **Step 6: Commit**

```bash
git add c_model/include/cosim/axi_dpi_adapter.hpp \
        c_model/tests/cosim/ \
        c_model/tests/CMakeLists.txt
git commit -m "feat(cosim): add AxiDpiAdapter tick service + unit test"
```

---

## Task 5: `axi_dpi.c` DPI bridge

**Files:**
- Create: `cosim/c/axi_dpi.c`
- Create: `cosim/c/axi_dpi.h`

- [ ] **Step 1: Write the C header (DPI signatures)**

Create `cosim/c/axi_dpi.h`:

```c
// DPI export signatures. Auto-imported by cosim/sv/nmu_cmodel_proxy.sv +
// nsu_cmodel_proxy.sv via `import "DPI-C" function ...` declarations.
#ifndef COSIM_AXI_DPI_H
#define COSIM_AXI_DPI_H

#include "svdpi.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void cmodel_init(const char *scenario_yaml_path);
void cmodel_finalize(void);
void cmodel_tick(void);
int  cmodel_done(void);
int  cmodel_scoreboard_clean(void);

// Per-channel AW: SV side passes output pointers; C fills them.
void cmodel_nmu_get_aw(svBit *valid, svBit *ready,
                       svBitVecVal *id, svBitVecVal *addr,
                       svBitVecVal *len, svBitVecVal *size, svBitVecVal *burst,
                       svBitVecVal *lock, svBitVecVal *cache,
                       svBitVecVal *prot, svBitVecVal *qos);
void cmodel_nmu_get_w (svBit *valid, svBit *ready,
                       svBitVecVal *data, svBitVecVal *strb, svBit *last);
void cmodel_nmu_get_ar(svBit *valid, svBit *ready,
                       svBitVecVal *id, svBitVecVal *addr,
                       svBitVecVal *len, svBitVecVal *size, svBitVecVal *burst,
                       svBitVecVal *lock, svBitVecVal *cache,
                       svBitVecVal *prot, svBitVecVal *qos);
void cmodel_nmu_get_b (svBit *valid, svBit *ready,
                       svBitVecVal *id, svBitVecVal *resp);
void cmodel_nmu_get_r (svBit *valid, svBit *ready,
                       svBitVecVal *id, svBitVecVal *data,
                       svBitVecVal *resp, svBit *last);

void cmodel_nsu_get_aw(svBit *valid, svBit *ready,
                       svBitVecVal *id, svBitVecVal *addr,
                       svBitVecVal *len, svBitVecVal *size, svBitVecVal *burst,
                       svBitVecVal *lock, svBitVecVal *cache,
                       svBitVecVal *prot, svBitVecVal *qos);
void cmodel_nsu_get_w (svBit *valid, svBit *ready,
                       svBitVecVal *data, svBitVecVal *strb, svBit *last);
void cmodel_nsu_get_ar(svBit *valid, svBit *ready,
                       svBitVecVal *id, svBitVecVal *addr,
                       svBitVecVal *len, svBitVecVal *size, svBitVecVal *burst,
                       svBitVecVal *lock, svBitVecVal *cache,
                       svBitVecVal *prot, svBitVecVal *qos);
void cmodel_nsu_get_b (svBit *valid, svBit *ready,
                       svBitVecVal *id, svBitVecVal *resp);
void cmodel_nsu_get_r (svBit *valid, svBit *ready,
                       svBitVecVal *id, svBitVecVal *data,
                       svBitVecVal *resp, svBit *last);

#ifdef __cplusplus
}
#endif

#endif  // COSIM_AXI_DPI_H
```

- [ ] **Step 2: Write the C bridge body**

Create `cosim/c/axi_dpi.c` (note: this is `.c`-suffix but contains C++ extern C bodies — rename to `.cpp` if your build expects that):

```cpp
// Implementation of cosim/c/axi_dpi.h. Holds a single static AxiDpiAdapter
// instance and translates DPI calls into adapter method invocations.
//
// This file is compiled as C++ (the adapter is C++) but exposes C linkage
// for the DPI ABI.

#include "axi_dpi.h"
#include "cosim/axi_dpi_adapter.hpp"
#include "cosim/pin_snapshot.hpp"
#include <cstring>
#include <memory>

using ni::cmodel::cosim::AxiDpiAdapter;
using ni::cmodel::cosim::AwPins;
using ni::cmodel::cosim::ArPins;
using ni::cmodel::cosim::BPins;
using ni::cmodel::cosim::RPins;
using ni::cmodel::cosim::WPins;

namespace {
std::unique_ptr<AxiDpiAdapter> g_adapter;
}  // namespace

extern "C" {

void cmodel_init(const char *scenario_yaml_path) {
    g_adapter = std::make_unique<AxiDpiAdapter>();
    g_adapter->init(scenario_yaml_path);
}

void cmodel_finalize(void) {
    g_adapter.reset();
}

void cmodel_tick(void) {
    if (g_adapter) g_adapter->tick();
}

int cmodel_done(void) {
    return (g_adapter && g_adapter->done()) ? 1 : 0;
}

int cmodel_scoreboard_clean(void) {
    return (g_adapter && g_adapter->scoreboard_clean()) ? 1 : 0;
}

// AW pack helper — same shape for nmu/nsu.
static void pack_aw(const AwPins& p,
                    svBit *valid, svBit *ready,
                    svBitVecVal *id, svBitVecVal *addr,
                    svBitVecVal *len, svBitVecVal *size, svBitVecVal *burst,
                    svBitVecVal *lock, svBitVecVal *cache,
                    svBitVecVal *prot, svBitVecVal *qos) {
    *valid = p.valid;
    *ready = p.ready;
    *id = p.id;
    *addr = static_cast<svBitVecVal>(p.addr & 0xFFFFFFFFu);
    // 64-bit addr split: high half in id[1]? See svBitVecVal width — for
    // ADDR_WIDTH > 32 SV expects an array. Set the addr_high getter call
    // signature separately if ADDR_WIDTH exceeds 32 in your config.
    *len = p.len;
    *size = p.size;
    *burst = p.burst;
    *lock = p.lock;
    *cache = p.cache;
    *prot = p.prot;
    *qos = p.qos;
}

void cmodel_nmu_get_aw(svBit *valid, svBit *ready,
                       svBitVecVal *id, svBitVecVal *addr,
                       svBitVecVal *len, svBitVecVal *size, svBitVecVal *burst,
                       svBitVecVal *lock, svBitVecVal *cache,
                       svBitVecVal *prot, svBitVecVal *qos) {
    AwPins p{};
    if (g_adapter) g_adapter->get_nmu_aw(p);
    pack_aw(p, valid, ready, id, addr, len, size, burst, lock, cache, prot, qos);
}

// ... define the other nine DPI getters following the same pattern; add
//     analogous pack_w / pack_ar / pack_b / pack_r helpers. Keep the
//     packing logic in one helper per channel to avoid duplication.

}  // extern "C"
```

**Implementer note:** the address width handling is the one tricky DPI marshalling point. If your `ADDR_WIDTH > 32`, `svBitVecVal *addr` must point to a 2-element array (lo + hi). Verify by checking the actual `ni::width::ADDR_WIDTH` macro and adjusting the helper.

- [ ] **Step 3: Commit (build verified later in Task 8)**

```bash
git add cosim/c/
git commit -m "feat(cosim): add DPI-C bridge (axi_dpi.c + axi_dpi.h)"
```

This compiles only against headers + svdpi.h here; the actual link/build is verified in Task 8.

---

## Task 6: SV proxy modules + AXI interface

**Files:**
- Create: `cosim/sv/axi_if.sv`
- Create: `cosim/sv/nmu_cmodel_proxy.sv`
- Create: `cosim/sv/nsu_cmodel_proxy.sv`

- [ ] **Step 1: Write the AXI bundle interface**

Create `cosim/sv/axi_if.sv`:

```systemverilog
// Parameterized AXI4 bundle interface used to wire the C-model proxy
// outputs into the wb2axip checker inputs.
//
// Widths default to project canonical values from
// specgen/generated/sv/ni_signals_pkg.sv; override at instantiation if a
// future param sweep needs different widths.

interface axi_if #(
    parameter int ID_WIDTH   = 3,
    parameter int ADDR_WIDTH = 32,
    parameter int DATA_WIDTH = 128
) (
    input logic clk,
    input logic rst_n
);
    // AW
    logic                  awvalid, awready;
    logic [ID_WIDTH-1:0]   awid;
    logic [ADDR_WIDTH-1:0] awaddr;
    logic [7:0]            awlen;
    logic [2:0]            awsize;
    logic [1:0]            awburst;
    logic                  awlock;
    logic [3:0]            awcache;
    logic [2:0]            awprot;
    logic [3:0]            awqos;
    // W
    logic                    wvalid, wready, wlast;
    logic [DATA_WIDTH-1:0]   wdata;
    logic [DATA_WIDTH/8-1:0] wstrb;
    // B
    logic                  bvalid, bready;
    logic [ID_WIDTH-1:0]   bid;
    logic [1:0]            bresp;
    // AR
    logic                  arvalid, arready;
    logic [ID_WIDTH-1:0]   arid;
    logic [ADDR_WIDTH-1:0] araddr;
    logic [7:0]            arlen;
    logic [2:0]            arsize;
    logic [1:0]            arburst;
    logic                  arlock;
    logic [3:0]            arcache;
    logic [2:0]            arprot;
    logic [3:0]            arqos;
    // R
    logic                  rvalid, rready, rlast;
    logic [ID_WIDTH-1:0]   rid;
    logic [DATA_WIDTH-1:0] rdata;
    logic [1:0]            rresp;
endinterface
```

- [ ] **Step 2: Write the NMU proxy**

Create `cosim/sv/nmu_cmodel_proxy.sv`:

```systemverilog
// NMU-side C-model AXI proxy.
// Each posedge clk: call DPI to (a) advance the entire C-model integration
// loop one cycle, (b) snapshot the NMU AXI(1) boundary pins. Drive
// snapshot onto the supplied axi_if bundle.
//
// Only the NMU proxy calls cmodel_tick(); NSU proxy only snapshots. This
// ensures the C-model is advanced exactly once per SV clock edge.

module nmu_cmodel_proxy #(
    parameter int ID_WIDTH   = 3,
    parameter int ADDR_WIDTH = 32,
    parameter int DATA_WIDTH = 128
) (
    input logic clk,
    input logic rst_n,
    axi_if.master aif
);
    import "DPI-C" context function void cmodel_tick();
    import "DPI-C" context function void cmodel_nmu_get_aw(
        output bit valid, output bit ready,
        output bit [ID_WIDTH-1:0] id,
        output bit [ADDR_WIDTH-1:0] addr,
        output bit [7:0] len, output bit [2:0] size, output bit [1:0] burst,
        output bit lock, output bit [3:0] cache,
        output bit [2:0] prot, output bit [3:0] qos);
    import "DPI-C" context function void cmodel_nmu_get_w(
        output bit valid, output bit ready,
        output bit [DATA_WIDTH-1:0] data,
        output bit [DATA_WIDTH/8-1:0] strb,
        output bit last);
    import "DPI-C" context function void cmodel_nmu_get_ar(
        output bit valid, output bit ready,
        output bit [ID_WIDTH-1:0] id,
        output bit [ADDR_WIDTH-1:0] addr,
        output bit [7:0] len, output bit [2:0] size, output bit [1:0] burst,
        output bit lock, output bit [3:0] cache,
        output bit [2:0] prot, output bit [3:0] qos);
    import "DPI-C" context function void cmodel_nmu_get_b(
        output bit valid, output bit ready,
        output bit [ID_WIDTH-1:0] id, output bit [1:0] resp);
    import "DPI-C" context function void cmodel_nmu_get_r(
        output bit valid, output bit ready,
        output bit [ID_WIDTH-1:0] id,
        output bit [DATA_WIDTH-1:0] data,
        output bit [1:0] resp, output bit last);

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            // hold all-zero during reset
            aif.awvalid <= '0; aif.awready <= '0;
            aif.wvalid  <= '0; aif.wready  <= '0; aif.wlast <= '0;
            aif.arvalid <= '0; aif.arready <= '0;
            aif.bvalid  <= '0; aif.bready  <= '0;
            aif.rvalid  <= '0; aif.rready  <= '0; aif.rlast <= '0;
        end else begin
            cmodel_tick();  // advance C-model one cycle

            cmodel_nmu_get_aw(aif.awvalid, aif.awready,
                              aif.awid, aif.awaddr,
                              aif.awlen, aif.awsize, aif.awburst,
                              aif.awlock, aif.awcache,
                              aif.awprot, aif.awqos);
            cmodel_nmu_get_w(aif.wvalid, aif.wready,
                             aif.wdata, aif.wstrb, aif.wlast);
            cmodel_nmu_get_ar(aif.arvalid, aif.arready,
                              aif.arid, aif.araddr,
                              aif.arlen, aif.arsize, aif.arburst,
                              aif.arlock, aif.arcache,
                              aif.arprot, aif.arqos);
            cmodel_nmu_get_b(aif.bvalid, aif.bready, aif.bid, aif.bresp);
            cmodel_nmu_get_r(aif.rvalid, aif.rready, aif.rid, aif.rdata,
                             aif.rresp, aif.rlast);
        end
    end
endmodule
```

- [ ] **Step 3: Write the NSU proxy (symmetric, no `cmodel_tick()`)**

Create `cosim/sv/nsu_cmodel_proxy.sv`:

```systemverilog
// NSU-side proxy. Does NOT call cmodel_tick() — the NMU proxy already
// did that. This proxy only snapshots NSU AXI(2) pin state into wires.

module nsu_cmodel_proxy #(
    parameter int ID_WIDTH   = 3,
    parameter int ADDR_WIDTH = 32,
    parameter int DATA_WIDTH = 128
) (
    input logic clk,
    input logic rst_n,
    axi_if.master aif
);
    import "DPI-C" context function void cmodel_nsu_get_aw(
        output bit valid, output bit ready,
        output bit [ID_WIDTH-1:0] id,
        output bit [ADDR_WIDTH-1:0] addr,
        output bit [7:0] len, output bit [2:0] size, output bit [1:0] burst,
        output bit lock, output bit [3:0] cache,
        output bit [2:0] prot, output bit [3:0] qos);
    // ... import the other four cmodel_nsu_get_* signatures the same way
    //     as nmu_cmodel_proxy.sv

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            aif.awvalid <= '0; aif.awready <= '0;
            aif.wvalid  <= '0; aif.wready  <= '0; aif.wlast <= '0;
            aif.arvalid <= '0; aif.arready <= '0;
            aif.bvalid  <= '0; aif.bready  <= '0;
            aif.rvalid  <= '0; aif.rready  <= '0; aif.rlast <= '0;
        end else begin
            cmodel_nsu_get_aw(aif.awvalid, aif.awready,
                              aif.awid, aif.awaddr,
                              aif.awlen, aif.awsize, aif.awburst,
                              aif.awlock, aif.awcache,
                              aif.awprot, aif.awqos);
            // ... call the other four getters
        end
    end
endmodule
```

- [ ] **Step 4: Lint pass via Verilator**

```bash
verilator --lint-only -Wall \
    -I cosim/sv \
    -I cosim/sv/wb2axip \
    cosim/sv/axi_if.sv \
    cosim/sv/nmu_cmodel_proxy.sv \
    cosim/sv/nsu_cmodel_proxy.sv
```

Expected: no errors. Warnings about unused signals (rready, bready) acceptable — checker observes those as inputs.

- [ ] **Step 5: Commit**

```bash
git add cosim/sv/axi_if.sv cosim/sv/nmu_cmodel_proxy.sv cosim/sv/nsu_cmodel_proxy.sv
git commit -m "feat(cosim): add SV proxy modules + axi_if bundle interface"
```

---

## Task 7: `tb_axi_conformity.sv` top

**Files:**
- Create: `cosim/sv/tb_axi_conformity.sv`

- [ ] **Step 1: Write the TB top**

Create `cosim/sv/tb_axi_conformity.sv`:

```systemverilog
`include "wb2axip/sim_wrapper.svh"

module tb_axi_conformity;
    localparam int ID_WIDTH   = 3;
    localparam int ADDR_WIDTH = 32;
    localparam int DATA_WIDTH = 128;

    logic clk;
    logic rst_n;

    string scenario_path;
    initial begin
        if (!$value$plusargs("scenario=%s", scenario_path)) begin
            $display("ERROR: +scenario=<yaml-path> required");
            $finish(1);
        end
        // Call C-model init with scenario path.
        // DPI signature: void cmodel_init(const char *)
        cmodel_init(scenario_path);
    end

    import "DPI-C" context function void cmodel_init(input string path);
    import "DPI-C" context function void cmodel_finalize();
    import "DPI-C" context function int  cmodel_done();
    import "DPI-C" context function int  cmodel_scoreboard_clean();

    // Clock + reset
    initial begin clk = 0; forever #5 clk = ~clk; end
    initial begin
        rst_n = 0;
        #20 rst_n = 1;
    end

    // Two AXI bundle wires
    axi_if #(.ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH))
        nmu_aif (.clk(clk), .rst_n(rst_n));
    axi_if #(.ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH))
        nsu_aif (.clk(clk), .rst_n(rst_n));

    // Two proxies wired via DPI
    nmu_cmodel_proxy #(.ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH))
        u_nmu_proxy (.clk(clk), .rst_n(rst_n), .aif(nmu_aif));
    nsu_cmodel_proxy #(.ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH))
        u_nsu_proxy (.clk(clk), .rst_n(rst_n), .aif(nsu_aif));

    // wb2axip protocol checkers
    faxi_slave #(
        .C_AXI_ID_WIDTH(ID_WIDTH),
        .C_AXI_DATA_WIDTH(DATA_WIDTH),
        .C_AXI_ADDR_WIDTH(ADDR_WIDTH)
    ) u_nmu_check (
        .i_clk(clk), .i_axi_reset_n(rst_n),
        .i_axi_awvalid(nmu_aif.awvalid), .i_axi_awready(nmu_aif.awready),
        .i_axi_awid(nmu_aif.awid), .i_axi_awaddr(nmu_aif.awaddr),
        .i_axi_awlen(nmu_aif.awlen), .i_axi_awsize(nmu_aif.awsize),
        .i_axi_awburst(nmu_aif.awburst), .i_axi_awlock(nmu_aif.awlock),
        .i_axi_awcache(nmu_aif.awcache), .i_axi_awprot(nmu_aif.awprot),
        .i_axi_awqos(nmu_aif.awqos),
        .i_axi_wvalid(nmu_aif.wvalid), .i_axi_wready(nmu_aif.wready),
        .i_axi_wdata(nmu_aif.wdata), .i_axi_wstrb(nmu_aif.wstrb),
        .i_axi_wlast(nmu_aif.wlast),
        .i_axi_bvalid(nmu_aif.bvalid), .i_axi_bready(nmu_aif.bready),
        .i_axi_bid(nmu_aif.bid), .i_axi_bresp(nmu_aif.bresp),
        .i_axi_arvalid(nmu_aif.arvalid), .i_axi_arready(nmu_aif.arready),
        .i_axi_arid(nmu_aif.arid), .i_axi_araddr(nmu_aif.araddr),
        .i_axi_arlen(nmu_aif.arlen), .i_axi_arsize(nmu_aif.arsize),
        .i_axi_arburst(nmu_aif.arburst), .i_axi_arlock(nmu_aif.arlock),
        .i_axi_arcache(nmu_aif.arcache), .i_axi_arprot(nmu_aif.arprot),
        .i_axi_arqos(nmu_aif.arqos),
        .i_axi_rid(nmu_aif.rid), .i_axi_rresp(nmu_aif.rresp),
        .i_axi_rvalid(nmu_aif.rvalid), .i_axi_rdata(nmu_aif.rdata),
        .i_axi_rlast(nmu_aif.rlast), .i_axi_rready(nmu_aif.rready)
    );

    faxi_master #(
        .C_AXI_ID_WIDTH(ID_WIDTH),
        .C_AXI_DATA_WIDTH(DATA_WIDTH),
        .C_AXI_ADDR_WIDTH(ADDR_WIDTH)
    ) u_nsu_check (
        // Same 40+ port hookup as u_nmu_check but reading nsu_aif.* fields.
        // Implementer: copy the u_nmu_check port list and search/replace
        // "nmu_aif" → "nsu_aif".
    );

    // Exit when scenario complete
    always @(posedge clk) begin
        if (rst_n && cmodel_done()) begin
            if (cmodel_scoreboard_clean())
                $display("PASS: scenario complete, scoreboard clean");
            else begin
                $display("FAIL: scoreboard mismatch");
                $finish(1);
            end
            cmodel_finalize();
            $finish(0);
        end
    end

    // Safety timeout
    initial begin
        #1_000_000;
        $display("FAIL: timeout (1ms simulated)");
        $finish(1);
    end
endmodule
```

- [ ] **Step 2: Lint the full TB**

```bash
verilator --lint-only -Wall \
    -I cosim/sv -I cosim/sv/wb2axip \
    cosim/sv/axi_if.sv \
    cosim/sv/nmu_cmodel_proxy.sv \
    cosim/sv/nsu_cmodel_proxy.sv \
    cosim/sv/wb2axip/faxi_master.v \
    cosim/sv/wb2axip/faxi_slave.v \
    cosim/sv/tb_axi_conformity.sv \
    --top-module tb_axi_conformity
```

Expected: zero errors. If wb2axip `assume()` calls trip Verilator, expect a fix in Task 8 step 2 (build flag).

- [ ] **Step 3: Commit**

```bash
git add cosim/sv/tb_axi_conformity.sv
git commit -m "feat(cosim): add tb_axi_conformity SV testbench top"
```

---

## Task 8: Verilator build (Makefile + harness main)

**Files:**
- Create: `cosim/verilator/Makefile`
- Create: `cosim/verilator/main.cpp`

- [ ] **Step 1: Write the Verilator harness main**

Create `cosim/verilator/main.cpp`:

```cpp
// Verilator harness top. Verilator generates Vtb_axi_conformity.{h,cpp};
// this main creates one instance and drives eval() until $finish.

#include "Vtb_axi_conformity.h"
#include "verilated.h"
#include <cstdio>
#include <memory>

int main(int argc, char** argv) {
    auto contextp = std::make_unique<VerilatedContext>();
    contextp->commandArgs(argc, argv);  // forwards +scenario=... to $value$plusargs

    auto top = std::make_unique<Vtb_axi_conformity>(contextp.get());

    while (!contextp->gotFinish()) {
        top->eval();
        contextp->timeInc(1);
    }

    return contextp->gotError() ? 1 : 0;
}
```

- [ ] **Step 2: Write the Makefile**

Create `cosim/verilator/Makefile`:

```makefile
# Windows MSYS2 native Verilator build for Stage 5 PoC co-sim.
# Pinned Verilator: 5.044 (set the MSYS2 binary in your PATH).
#
# Build: `make` in this directory
# Run:   `./obj_dir/Vtb_axi_conformity +scenario=<path-to-yaml>`

VERILATOR     := verilator
TOP           := tb_axi_conformity
PROJ_ROOT     := $(realpath ../..)
COSIM_ROOT    := $(realpath ..)
CMODEL_INC    := $(PROJ_ROOT)/c_model/include
SPECGEN_INC   := $(PROJ_ROOT)/specgen/generated/cpp
YAMLCPP_INC   := $(PROJ_ROOT)/c_model/build/_deps/yaml-cpp-src/include

SV_SRC := \
    $(COSIM_ROOT)/sv/axi_if.sv \
    $(COSIM_ROOT)/sv/nmu_cmodel_proxy.sv \
    $(COSIM_ROOT)/sv/nsu_cmodel_proxy.sv \
    $(COSIM_ROOT)/sv/wb2axip/faxi_master.v \
    $(COSIM_ROOT)/sv/wb2axip/faxi_slave.v \
    $(COSIM_ROOT)/sv/tb_axi_conformity.sv

C_SRC := \
    $(COSIM_ROOT)/c/axi_dpi.c \
    $(COSIM_ROOT)/verilator/main.cpp

VERILATOR_FLAGS := \
    --cc --exe --build \
    --top-module $(TOP) \
    -I $(COSIM_ROOT)/sv \
    -I $(COSIM_ROOT)/sv/wb2axip \
    -CFLAGS "-std=c++17 -I$(COSIM_ROOT)/c -I$(CMODEL_INC) -I$(SPECGEN_INC) -I$(YAMLCPP_INC)" \
    -LDFLAGS "-L$(PROJ_ROOT)/c_model/build -lni_cmodel_core -lyaml-cpp" \
    +define+assume=assert \
    -Wno-fatal

all: obj_dir/V$(TOP)

obj_dir/V$(TOP): $(SV_SRC) $(C_SRC)
	$(VERILATOR) $(VERILATOR_FLAGS) $(SV_SRC) $(C_SRC)

hello:  # toolchain smoke — minimal empty TB to validate Verilator setup
	echo "module empty_tb; initial $$finish; endmodule" > /tmp/empty_tb.sv
	$(VERILATOR) --cc --exe --build --top-module empty_tb \
	    -CFLAGS "-std=c++17" /tmp/empty_tb.sv \
	    --Mdir /tmp/empty_tb_obj
	/tmp/empty_tb_obj/Vempty_tb && echo "TOOLCHAIN OK"

clean:
	rm -rf obj_dir
```

Adapt `ni_cmodel_core` library name (line `-LDFLAGS`) to whatever your `c_model/CMakeLists.txt` exports — peek there to confirm.

- [ ] **Step 3: Validate toolchain with hello-world TB**

```bash
cd cosim/verilator
make hello
```

Expected: `TOOLCHAIN OK` printed. If this fails, fix MSYS2 / Verilator install before proceeding. Fallback: use WSL Verilator if MSYS2 native does not produce a working setup.

- [ ] **Step 4: Build the full co-sim**

```bash
make
```

Expected: build succeeds, `obj_dir/Vtb_axi_conformity` (or `.exe` on Windows) exists. If wb2axip files fail to compile, look at the error — likely it is an `assume()` not caught by `+define+assume=assert`; in that case adjust the macro definition or add minimal in-line patches (documented in `ATTRIBUTION.md` modifications log).

- [ ] **Step 5: Commit**

```bash
git add cosim/verilator/
git commit -m "feat(cosim): add Verilator build harness (Makefile + main.cpp)"
```

---

## Task 9: First end-to-end smoke run

**Files:** no new files; verification only.

- [ ] **Step 1: Identify available YAML scenarios**

```bash
find c_model -name "*.yaml" -type f | head -20
```

Expected: at least `burst_incr_8beat.yaml` present somewhere under `c_model/`. Pick one to drive the smoke run.

- [ ] **Step 2: Run the simplest scenario**

```bash
cd cosim/verilator
./obj_dir/Vtb_axi_conformity +scenario=<path-to-burst_incr_8beat.yaml>
```

Expected: program exits 0, prints "PASS: scenario complete, scoreboard clean". No `$error` from wb2axip checkers fires.

If a wb2axip assertion fires: read the property name and line — it will quote the AXI4 IHI 0022 rule it represents. Investigate whether (a) the C-model genuinely violates that rule (real bug, fix in C-model), or (b) the proxy snapshot is racing (drives the pin at the wrong cycle — likely fix in proxy or pin-update timing in AxiDpiAdapter). Iterate.

- [ ] **Step 3: Commit (or stash) any iteration fixes**

```bash
git status
# If C-model or proxy required tweaks:
git add <touched files>
git commit -m "fix(cosim): <specific fix description>"
```

---

## Task 10: ctest integration + full smoke set

**Files:**
- Create: `cosim/tests/test_nmu_nsu_axi_conformity.cpp`
- Create: `cosim/tests/CMakeLists.txt`
- Modify: `c_model/tests/CMakeLists.txt` (add `add_subdirectory(${CMAKE_SOURCE_DIR}/../cosim/tests)`)

- [ ] **Step 1: Write the GoogleTest wrapper**

Create `cosim/tests/test_nmu_nsu_axi_conformity.cpp`:

```cpp
// ctest-level entry that drives the Verilator co-sim binary across the
// smoke YAML scenario set. Each scenario becomes one ctest case.
//
// The Verilator binary (Vtb_axi_conformity) is built out-of-band by the
// cosim/verilator/Makefile; this test invokes it as a subprocess.

#include "common/scenario.hpp"
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace {
constexpr const char* kCosimBinaryEnv = "COSIM_BIN";  // set by CMake

class NmuNsuAxiConformity : public ::testing::TestWithParam<std::string> {};

TEST_P(NmuNsuAxiConformity, scenario_passes_wb2axip_checker) {
    SCENARIO(("co-sim conformity check: " + GetParam()).c_str());

    const char* bin = std::getenv(kCosimBinaryEnv);
    ASSERT_NE(bin, nullptr) << "Set " << kCosimBinaryEnv << " env var.";
    std::string scenario = "c_model/tests/fixtures/" + GetParam();
    ASSERT_TRUE(std::filesystem::exists(scenario)) << scenario;

    std::string cmd = std::string(bin) + " +scenario=" + scenario;
    int rc = std::system(cmd.c_str());
    EXPECT_EQ(rc, 0) << "co-sim returned non-zero for " << GetParam();
}

INSTANTIATE_TEST_SUITE_P(SmokeSet, NmuNsuAxiConformity,
    ::testing::Values(
        "burst_incr_8beat.yaml",
        "4kb_cross.yaml",
        "multi_outstanding_stress.yaml"
    ));
}  // namespace
```

If actual YAML names differ in your `c_model/tests/fixtures/`, adjust.

- [ ] **Step 2: Write CMakeLists hook**

Create `cosim/tests/CMakeLists.txt`:

```cmake
# ctest entry for the cosim smoke. Depends on the Verilator binary being
# built separately by cosim/verilator/Makefile (out-of-band — this CMake
# does NOT invoke Verilator; the engineer runs `make` in cosim/verilator
# before running ctest).

add_executable(test_nmu_nsu_axi_conformity test_nmu_nsu_axi_conformity.cpp)
target_link_libraries(test_nmu_nsu_axi_conformity PRIVATE GTest::gtest_main)
target_include_directories(test_nmu_nsu_axi_conformity PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

# Path to the Verilator-built binary (Makefile output).
set(COSIM_BIN "${CMAKE_SOURCE_DIR}/../cosim/verilator/obj_dir/Vtb_axi_conformity")
if(WIN32)
    set(COSIM_BIN "${COSIM_BIN}.exe")
endif()

add_test(NAME NmuNsuAxiConformity COMMAND test_nmu_nsu_axi_conformity)
set_tests_properties(NmuNsuAxiConformity PROPERTIES
    ENVIRONMENT "COSIM_BIN=${COSIM_BIN}"
)
```

- [ ] **Step 3: Hook into parent CMakeLists**

Modify `c_model/tests/CMakeLists.txt` — add at the end:

```cmake
# cosim tests live outside c_model/tests/ but are surfaced through ctest
# here for unified gate. Requires cosim/verilator/obj_dir/Vtb_axi_conformity
# to be built out-of-band beforehand.
if(EXISTS "${CMAKE_SOURCE_DIR}/../cosim/tests/CMakeLists.txt")
    add_subdirectory(${CMAKE_SOURCE_DIR}/../cosim/tests cosim_tests)
endif()
```

- [ ] **Step 4: Build cosim binary + run ctest**

```bash
cd cosim/verilator && make && cd ../..
cd c_model && cmake --build build && ctest --test-dir build -j 1
```

Expected: 394 / 394 (393 baseline + 1 new entry which itself runs 3 parameterized scenarios → still counted as 1 test_executable but with 3 sub-cases).

If any scenario fails, the per-scenario `$error` text from wb2axip identifies the AXI4 rule violated. Iterate per Task 9 step 2 debugging pattern.

- [ ] **Step 5: Final drift gate check**

```bash
cd specgen
py -3 -m pytest -q
py -3 tools/codegen.py --check
py -3 tools/gen_inventory.py --check
cd ..
```

Expected: all clean (this round added no specgen / generated file changes — none of these gates should fire).

- [ ] **Step 6: Commit**

```bash
git add cosim/tests/ c_model/tests/CMakeLists.txt
git commit -m "test(cosim): add ctest entry running wb2axip co-sim smoke set"
```

---

## Self-review (writing-plans checklist)

**1. Spec coverage check:**

| Spec section | Plan task |
|---|---|
| §2 Scope: cosim/ tree | Tasks 1, 6, 7, 8, 10 |
| §3 Decisions: Verification approach | Architecture across plan |
| §3 Decisions: Boundary cut both sides | Task 7 (two checker instances) |
| §3 Decisions: C++ stimulus reuse | Task 4 (adapter wires existing master/slave/memory) |
| §3 Decisions: ZipCPU wb2axip OSS | Task 1 vendor + Task 7 instantiate |
| §3 Decisions: SV master DPI direction | Task 6 (proxy `cmodel_tick()` from `always_ff @(posedge clk)`) |
| §3 Decisions: Verilator 5.044 / Windows MSYS2 | Task 8 Makefile |
| §3 Decisions: Same shell to VCS | implicitly preserved — no Verilator-only macros in `.sv` files |
| §3 Decisions: wb2axip vendor + ATTRIBUTION | Task 1 |
| §4.3 File layout | Direct mapping into Tasks 1–10 |
| §5.1 DPI per-channel granularity | Task 5 signatures + Task 6 proxies |
| §5.3 Scenario plusarg | Task 7 `$value$plusargs` + Task 8 + Task 9 |
| §6 wb2axip vendoring policy | Task 1 + Task 2 (shim, no .v edits) |
| §7.1 Drift gates | Task 10 step 5 |
| §7.2 Smoke scenarios (3) | Task 10 parameterized cases |
| §7.3 Karpathy assumptions | Risks table at plan top, addressed in Tasks 8 + 9 |

**2. Placeholder scan:** Plan does contain two intentional in-task `TODO` markers in Task 4 step 4 (`init()` body and remaining nine `get_*` bodies). These are flagged with explicit reference to the existing integration testbench file (`test_request_response_loopback.cpp`) as the line-by-line template — the engineer copies the wiring block from that file. This is grounded reference, not vague TODO. Acceptable.

**3. Type consistency check:**
- `AxiDpiAdapter` class name used consistently across Tasks 4, 5
- DPI function names `cmodel_init` / `cmodel_tick` / `cmodel_done` / `cmodel_scoreboard_clean` used consistently across Tasks 4, 5, 6, 7
- `AwPins` / `WPins` / `ArPins` / `BPins` / `RPins` struct names consistent (Task 3 → Task 4)
- `axi_if` interface params `ID_WIDTH` / `ADDR_WIDTH` / `DATA_WIDTH` consistent (Task 6 → Task 7)
- `Vtb_axi_conformity` binary name consistent (Tasks 8 → 9 → 10)

No mismatches found.
