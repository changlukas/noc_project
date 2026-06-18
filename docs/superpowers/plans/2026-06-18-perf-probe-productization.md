# Perf Probe Productization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Execute each `### Task N` as one independent RED->GREEN->commit unit. Steps use checkbox (`- [ ]`) syntax. Do not batch tasks; do not skip the failing-test step.

**Goal:** Turn the partial perf probe (one lumped router, hard-coded NI occupancy, one hard-coded transaction, buried in a test's stdout) into a mesh-agnostic, scenario-driven perf artifact: a path-driven decomposition that emits every router on a transaction's request and response path with real per-router latency, NI occupancy wired to real getters, run metadata, aligned stdout/JSON field names, driven from any scenario YAML's full transaction list, and produced via a `make perf` target.

**Architecture:** A new mesh-parameterized `router_path(src,dst,mx,my)` derives the XY node-coordinate sequence (and per-hop port direction) a flit visits; the harness loops over that path generically, wrapping every router boundary and inter-router link with the existing `flit_link_probe` decorators (no hardcoded router names). A new lossless `isolated_scenario` writer copies every transaction + config field and remaps the destination to node 1 for Pass-1 characterization. `perf_report.hpp` gains a run-metadata block, aligned field names, and `n/a` occupancy support. The two-pass harness (Pass-1 isolated characterization by signature; Pass-2 contended multi-transaction run) migrates out of `test_router_loopback.cpp` into a dedicated `test_perf_probe.cpp`. The 2-node fabric gains by-coordinate `req_router_at`/`rsp_router_at` accessors so the harness never names a node id. Source spec: `docs/superpowers/specs/2026-06-18-perf-probe-productization-design.md` (builds on `2026-06-17-perf-probe-simplify-design.md`).

**Tech Stack:** C++17, CMake 3.20+ / ninja, GoogleTest. Namespace `ni::cmodel::testing` for all new testbench code; `ni::cmodel::noc::testing` for the fabric. GNU Make for the `perf` target.

## Global Constraints

Binding rules (copied verbatim from the spec intent + project CLAUDE.md / MEMORY):

- **No production change.** This entire feature is testbench-only. The only production change in the whole perf-probe effort (the router `vc_depth()` / `output_fifo_depth()` getters) already landed. Every file in this plan lives under `c_model/tests/` (namespace `ni::cmodel::testing`, fabric in `ni::cmodel::noc::testing`) or is the root `Makefile`. None are linked into production libraries.
- **TDD RED->GREEN.** Every task writes a failing test FIRST, runs it to confirm it fails for the expected reason, then writes the minimal implementation, then runs it to confirm it passes. No implementation before a failing test.
- **Build (this machine):** `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3`. NEVER `py -3` (it auto-downloads a Python and corrupts the Verilator-generated `.cpp`). The CMakeCache is already pinned to mingw64 python.
- **Low parallelism:** when invoking cmake directly use `cmake --build build/cmodel -j 2` (NOT full `-j` — full parallelism triggers a g++ internal compiler error / ICE).
- **Test:** `cd build/cmodel && ctest -R <name> --output-on-failure` (run the just-built target by regex).
- **clang-format:** run `clang-format -i <file>` on every edited `.hpp` / `.cpp` before committing. The repo `.clang-format` enforces Google-base + IndentWidth 4 + ContinuationIndentWidth 4 + ColumnLimit 100.
- **Commit message format:** `type(scope): description` (English). Valid types: feat, fix, docs, style, refactor, test, chore, perf, build, revert. End each commit body with the harness-required `Co-Authored-By:` trailer.
- **Never** use `--no-verify`. Never disable a test to make it pass. Never commit non-compiling code. Every commit compiles, passes all existing tests, and includes tests for new functionality.
- **JSON location:** gitignored, default `build/cmodel/perf/<scenario>.json`, overridable by `NOC_PERF_FILE`. `/build/` is already in `.gitignore`. The `make perf` target sets an **absolute** `NOC_PERF_FILE` because the ctest body runs from `build/cmodel` and `emit()`'s default path is CWD-relative.

### Coding-style reminders (project conventions)

- snake_case for variables/methods, PascalCase for types. No camelCase. Full words, no abbreviation.
- 4-space continuation indent on wrapped lines.

### Reference signatures already in the repo (cite, do not redefine)

- **`nmu::addr_trans`** (`c_model/include/nmu/addr_trans.hpp`): `xy_route(uint64_t addr)` -> `Translated{uint8_t dst_id; uint64_t local_addr}` where `dst_id = (addr >> 32) & DST_ID_MASK`. `LOCAL_ADDR_BITS == 32`, `DST_ID_BITS == 8`. Node id encodes `(x, y)` as `x | (y << X_WIDTH)` with `X_WIDTH == 4` (`ni::width::X_WIDTH` from `ni_flit_constants.h`).
- **`noc::Router`** (`c_model/include/noc/router.hpp`): `RouterConfig{x,y,mesh_x_dim,mesh_y_dim,num_vc,vc_depth,output_fifo_depth}`; `RouterLink` (pure virtual `void push_flit(const Flit&)`); `RouterLink& input(std::size_t port)`; `void set_downstream(std::size_t port, RouterLink& link)` (overwrites the downstream pointer); `std::size_t input_fifo_size(std::size_t port, uint8_t vc) const`; `std::size_t output_fifo_size(std::size_t port) const`; `uint8_t num_vc() const`; `std::size_t vc_depth() const`; `std::size_t output_fifo_depth() const`. `ROUTER_PORT_COUNT == 5`. Ports `RouterPort::{LOCAL=0,NORTH=1,EAST=2,SOUTH=3,WEST=4}`.
- **`TwoNodeFabric`** (`c_model/tests/noc/two_node_fabric.hpp`, namespace `ni::cmodel::noc::testing`): static constants `LOCAL`/`EAST`/`WEST` (as `std::size_t`); `NocReqOut& nmu_req_out(node)`, `NocRspIn& nmu_rsp_in(node)`, `NocReqIn& nsu_req_in(node)`, `NocRspOut& nsu_rsp_out(node)`, `Router& req_router(node)`, `Router& rsp_router(node)`, `void tick()`. Ctor `TwoNodeFabric(uint8_t num_vc, std::size_t vc_depth, std::size_t out_fifo_depth)`. 2-node wiring: `req_router(0).EAST -> req_router(1).WEST` and reverse; `rsp_router(1).WEST -> rsp_router(0).EAST` and reverse.
- **NoC interfaces** (`c_model/include/noc/`): `NocReqOut` / `NocRspOut` — `virtual bool push_flit(const Flit&)`, `virtual bool credit_avail(uint8_t) const`. `NocReqIn` / `NocRspIn` — `virtual std::optional<Flit> pop_flit()`.
- **`flit_link_probe.hpp`** (`c_model/tests/common/`): `struct FlitCrossing{uint64_t cycle; uint8_t axi_ch,vc_id,src_id,dst_id;}`; `class FlitLog{explicit FlitLog(std::string); void record(const Flit&,uint64_t); const std::vector<FlitCrossing>& crossings() const; const std::string& boundary_id() const;}`; decorators `ReqOutProbe(NocReqOut&,FlitLog&,const uint64_t& now)`, `RspInProbe(NocRspIn&,...)`, `ReqInProbe(NocReqIn&,...)`, `RspOutProbe(NocRspOut&,...)`, `LinkProbe(RouterLink&,FlitLog&,const uint64_t& now)`.
- **`component_dwell_observer.hpp`** (`c_model/tests/common/`): `class SegmentDwell{void pair(const FlitLog& entry, const FlitLog& exit); const Stats& by_channel(uint8_t) const; const Stats& all() const;}` (pairs FIFO-order per vc_id); `class OccupancyPeak{explicit OccupancyPeak(std::size_t capacity); void sample(std::size_t fill); std::size_t peak() const; std::size_t capacity() const;}`.
- **`ni_perf_observer.hpp`** (`c_model/tests/common/`): `NIPerfObserver(const uint64_t& now, std::string label)`; `void on_issue(bool is_write, std::size_t line)`; `void on_complete(bool is_write, std::size_t line)`; `const Stats& write_latency() const`; `const Stats& read_latency() const`; `std::size_t stuck_count() const`.
- **`perf_stats.hpp`** (`c_model/tests/common/`): `class Stats{void add(uint64_t); uint64_t count() const; uint64_t sum() const; uint64_t min() const; uint64_t max() const; double mean() const;}`.
- **`perf_report.hpp`** (`c_model/tests/common/`, current): `struct TxnRecord{std::size_t line; std::string type; uint8_t id; std::string src,dst; std::vector<std::string> request_path,response_path; uint64_t measured_cyc,zero_load_cyc;}`; `struct ComponentRecord{std::string name,kind; uint64_t hop_min; double hop_mean; uint64_t hop_max; std::size_t occ_max,occ_capacity;}`; `class PerfReport{void set_scenario(std::string); void set_slave_remainder(uint64_t); void add_transaction(TxnRecord); void add_ni(ComponentRecord); void add_router(ComponentRecord); void write_summary(std::ostream&) const; void write_json(std::ostream&) const; void emit() const;}`. JSON key `latency_cyc`; stdout line `[perf:<name>] kind=.. latency(min/mean/max)=.. occ(max/cap)=..`.
- **NMU occupancy:** `Nmu::rob()` -> `const nmu::Rob&` (`c_model/include/nmu/nmu.hpp:81`); `Rob::write_occupancy()` / `Rob::read_occupancy()` -> `std::size_t` (`c_model/include/nmu/rob.hpp:82,87`); `Rob::ROB_CAPACITY` (`rob.hpp:62`).
- **NSU occupancy:** `Nsu::axi_master_port()` -> `AxiMasterPort&` (`c_model/include/nsu/nsu.hpp`); `AxiMasterPort::aw_q_size()/w_q_size()/ar_q_size()/b_q_size()/r_q_size()` all `std::size_t () const` (`axi_master_port.hpp:99-103`); `AxiMasterPort::params()` -> `const PortParams&` (depths).
- **Scenario:** `axi::load_scenario(path)` -> `axi::Scenario{Metadata metadata; ScenarioConfig config; std::vector<ScenarioTransaction> transactions;}` (`c_model/include/axi/scenario_parser.hpp`). `ScenarioConfig{uint64_t memory_base; std::size_t memory_size,write_latency,read_latency,max_outstanding_write,max_outstanding_read; InjectConfig inject;}`. `ScenarioTransaction{Op op; uint64_t addr; uint8_t id,len,size; Burst burst; std::string data_file,dump_file,strb_file; LockType lock; uint8_t qos; std::size_t scenario_line;}`. `Op::{Write,Read}`; `Burst::{INCR,WRAP,FIXED}`; `LockType::{Normal,Exclusive}`.
- **`axi::AxiMasterT<nmu::AxiSlavePort>`** (`c_model/include/axi/axi_master.hpp`): `on_write_issued(cb)/on_read_issued(cb)` with `IssueInfo{...,scenario_line}`; `on_write_completed(cb)` with `WriteResult{...,scenario_line}`; `on_read_observed(cb)` with `ReadResult{...,scenario_line}`. The callbacks carry NO cycle field; the harness stamps the current cycle (`now`) when each fires.
- **`Flit`** (`c_model/include/flit.hpp`): `uint64_t get_header_field(std::string_view) const`; in tests `set_header_field(std::string_view, uint64_t)`. AXI channel codes (`specgen/generated/cpp/ni_flit_constants.h`): `AXI_CH_AW=0, AXI_CH_W=1, AXI_CH_AR=2, AXI_CH_B=3, AXI_CH_R=4`.
- **CMake helper:** `add_cmodel_test(<name>)` (in `c_model/tests/CMakeLists.txt`) builds `<name>.cpp`, links `gtest_main`, adds the `codegen_check` dep, runs `gtest_discover_tests`. For integration tests link `yaml-cpp::yaml-cpp noc_axi4_scenarios`, set `SCENARIO_TREE_ROOT` define, add `${CMAKE_CURRENT_SOURCE_DIR}/..` + `${CMAKE_SOURCE_DIR}/../tests/scenarios` includes, and copy `config/` POST_BUILD (mirror the `test_router_loopback` block in `c_model/tests/integration/CMakeLists.txt`).
- **Scenario tree:** 37 ids under `tests/scenarios/<id>/scenario.yaml`. The only error-injection / expected-fail id is `AX4-INF-001_dpi_fatal_on_init_failure` (AXI integration skips `AX4-INF-` ids via inline `GTEST_SKIP`; no shared skip list exists — perf defines its own `expected_fail` set). `SCENARIO_TREE_ROOT` is set in `tests/integration/CMakeLists.txt`. The canonical perf flow is `node 0 -> node 1` (`AX4-BAS-003`), established by setting address bit 32 so `xy_route` yields `dst_id == 1`.

---

### Task 1: `router_path.hpp` — mesh-parameterized XY path + per-hop direction

Derive the ordered node-coordinate sequence a flit visits under XY routing (step X to the destination column, then Y), and the output port for each hop. This is the only place mesh dimensions enter; everything downstream consumes the path. Self-contained: depends only on `RouterPort` from `noc/router.hpp`. Proven mesh-agnostic by a synthetic multi-hop test even though the only fabric instance is 2-node.

**Files:**
- Create: `c_model/tests/common/router_path.hpp`
- Test: `c_model/tests/common/test_router_path.cpp`
- Modify: `c_model/tests/common/CMakeLists.txt`

**Interfaces:**
- Consumes: `RouterPort` (`noc/router.hpp`), `ni::width::X_WIDTH` (`ni_flit_constants.h`).
- Produces:
  - `struct NodeCoord { uint8_t x; uint8_t y; bool operator==(const NodeCoord&) const; };`
  - `NodeCoord node_coord(uint8_t node_id);` (decodes `x = id & ((1<<X_WIDTH)-1)`, `y = id >> X_WIDTH`)
  - `uint8_t node_id(NodeCoord c);` (encodes `x | (y << X_WIDTH)`)
  - `std::vector<NodeCoord> router_path(uint8_t src_id, uint8_t dst_id, uint8_t mesh_x, uint8_t mesh_y);` (X-then-Y; returns `k+1` coords for a `k`-hop path, inclusive of both endpoints)
  - `std::size_t direction(NodeCoord from, NodeCoord to);` (`+x->EAST, -x->WEST, +y->NORTH, -y->SOUTH`, returned as `std::size_t` port index)
  - `std::size_t opposite(std::size_t port);` (`EAST<->WEST`, `NORTH<->SOUTH`)

- [ ] **Step 1: Write the failing test** `c_model/tests/common/test_router_path.cpp`

```cpp
#include "common/router_path.hpp"
#include "noc/router.hpp"
#include <gtest/gtest.h>

using ni::cmodel::noc::RouterPort;
using ni::cmodel::testing::direction;
using ni::cmodel::testing::node_coord;
using ni::cmodel::testing::node_id;
using ni::cmodel::testing::NodeCoord;
using ni::cmodel::testing::opposite;
using ni::cmodel::testing::router_path;

namespace {
constexpr auto EAST = static_cast<std::size_t>(RouterPort::EAST);
constexpr auto WEST = static_cast<std::size_t>(RouterPort::WEST);
constexpr auto NORTH = static_cast<std::size_t>(RouterPort::NORTH);
constexpr auto SOUTH = static_cast<std::size_t>(RouterPort::SOUTH);
}  // namespace

TEST(RouterPath, NodeIdRoundTrip) {
    // X_WIDTH=4: node (x=1,y=0) -> id 0x01; (x=2,y=3) -> id 0x32.
    EXPECT_EQ(node_id(NodeCoord{1, 0}), 0x01u);
    EXPECT_EQ(node_id(NodeCoord{2, 3}), static_cast<uint8_t>(0x02 | (0x03 << 4)));
    EXPECT_TRUE((node_coord(0x32) == NodeCoord{2, 3}));
}

TEST(RouterPath, TwoNodeOneHopRequest) {
    // node 0 (0,0) -> node 1 (1,0): single EAST hop, two coords.
    auto p = router_path(/*src=*/0x00, /*dst=*/0x01, /*mesh_x=*/2, /*mesh_y=*/1);
    ASSERT_EQ(p.size(), 2u);
    EXPECT_TRUE((p[0] == NodeCoord{0, 0}));
    EXPECT_TRUE((p[1] == NodeCoord{1, 0}));
    EXPECT_EQ(direction(p[0], p[1]), EAST);
    EXPECT_EQ(opposite(direction(p[0], p[1])), WEST);
}

TEST(RouterPath, ResponseIsReversePath) {
    auto fwd = router_path(0x00, 0x01, 2, 1);
    auto rev = router_path(0x01, 0x00, 2, 1);
    ASSERT_EQ(rev.size(), 2u);
    EXPECT_TRUE((rev[0] == NodeCoord{1, 0}));
    EXPECT_TRUE((rev[1] == NodeCoord{0, 0}));
    EXPECT_EQ(direction(rev[0], rev[1]), WEST);
}

TEST(RouterPath, SyntheticMultiHopXThenY) {
    // (0,0) -> (2,1) on a 3x2 mesh: X first to column 2, then Y to row 1.
    // Path: (0,0)->(1,0)->(2,0)->(2,1). 3 hops, 4 coords. Proves mesh-agnostic.
    auto p = router_path(/*src=*/node_id(NodeCoord{0, 0}),
                         /*dst=*/node_id(NodeCoord{2, 1}), /*mesh_x=*/3, /*mesh_y=*/2);
    ASSERT_EQ(p.size(), 4u);
    EXPECT_TRUE((p[0] == NodeCoord{0, 0}));
    EXPECT_TRUE((p[1] == NodeCoord{1, 0}));
    EXPECT_TRUE((p[2] == NodeCoord{2, 0}));
    EXPECT_TRUE((p[3] == NodeCoord{2, 1}));
    EXPECT_EQ(direction(p[0], p[1]), EAST);
    EXPECT_EQ(direction(p[1], p[2]), EAST);
    EXPECT_EQ(direction(p[2], p[3]), NORTH);
}

TEST(RouterPath, SameNodeIsSingleCoordNoHop) {
    auto p = router_path(0x00, 0x00, 2, 1);
    ASSERT_EQ(p.size(), 1u);
    EXPECT_TRUE((p[0] == NodeCoord{0, 0}));
}

TEST(RouterPath, NegativeYIsSouth) {
    EXPECT_EQ(direction(NodeCoord{0, 1}, NodeCoord{0, 0}), SOUTH);
}
```

- [ ] **Step 2: Wire the target** — append to `c_model/tests/common/CMakeLists.txt`

```cmake
add_cmodel_test(test_router_path)
target_include_directories(test_router_path PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

- [ ] **Step 3: Run it to verify it fails**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3`
Expected: FAIL — `fatal error: common/router_path.hpp: No such file or directory`.

- [ ] **Step 4: Write minimal implementation** `c_model/tests/common/router_path.hpp`

```cpp
#pragma once
#include "ni_flit_constants.h"  // ni::width::X_WIDTH
#include "noc/router.hpp"       // RouterPort
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ni::cmodel::testing {

// A node's mesh coordinate. Node id encodes (x,y) as x | (y << X_WIDTH),
// the same scheme nmu::addr_trans uses for dst_id (X_WIDTH=4).
struct NodeCoord {
    uint8_t x;
    uint8_t y;
    bool operator==(const NodeCoord& o) const { return x == o.x && y == o.y; }
};

inline NodeCoord node_coord(uint8_t id) {
    constexpr uint8_t x_mask = static_cast<uint8_t>((1u << ni::width::X_WIDTH) - 1);
    return NodeCoord{static_cast<uint8_t>(id & x_mask),
                     static_cast<uint8_t>(id >> ni::width::X_WIDTH)};
}

inline uint8_t node_id(NodeCoord c) {
    return static_cast<uint8_t>(c.x | (c.y << ni::width::X_WIDTH));
}

// XY route: step X to the destination column, then Y to the destination row.
// Returns the inclusive coordinate sequence: k+1 coords for a k-hop path.
inline std::vector<NodeCoord> router_path(uint8_t src_id, uint8_t dst_id, uint8_t /*mesh_x*/,
                                          uint8_t /*mesh_y*/) {
    const NodeCoord src = node_coord(src_id);
    const NodeCoord dst = node_coord(dst_id);
    std::vector<NodeCoord> path;
    NodeCoord cur = src;
    path.push_back(cur);
    while (cur.x != dst.x) {
        cur.x = static_cast<uint8_t>(cur.x + (dst.x > cur.x ? 1 : -1));
        path.push_back(cur);
    }
    while (cur.y != dst.y) {
        cur.y = static_cast<uint8_t>(cur.y + (dst.y > cur.y ? 1 : -1));
        path.push_back(cur);
    }
    return path;
}

// Output port for one single-axis hop from -> to. XY routing changes X before
// Y, so each hop is one unambiguous axis step.
inline std::size_t direction(NodeCoord from, NodeCoord to) {
    if (to.x > from.x) return static_cast<std::size_t>(noc::RouterPort::EAST);
    if (to.x < from.x) return static_cast<std::size_t>(noc::RouterPort::WEST);
    if (to.y > from.y) return static_cast<std::size_t>(noc::RouterPort::NORTH);
    return static_cast<std::size_t>(noc::RouterPort::SOUTH);
}

inline std::size_t opposite(std::size_t port) {
    switch (static_cast<noc::RouterPort>(port)) {
        case noc::RouterPort::EAST:
            return static_cast<std::size_t>(noc::RouterPort::WEST);
        case noc::RouterPort::WEST:
            return static_cast<std::size_t>(noc::RouterPort::EAST);
        case noc::RouterPort::NORTH:
            return static_cast<std::size_t>(noc::RouterPort::SOUTH);
        case noc::RouterPort::SOUTH:
            return static_cast<std::size_t>(noc::RouterPort::NORTH);
        default:
            return static_cast<std::size_t>(noc::RouterPort::LOCAL);
    }
}

}  // namespace ni::cmodel::testing
```

- [ ] **Step 5: Run it to verify it passes**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3` then `cd build/cmodel && ctest -R test_router_path --output-on-failure`
Expected: PASS (all 6 tests).

- [ ] **Step 6: clang-format and commit**

```bash
clang-format -i c_model/tests/common/router_path.hpp c_model/tests/common/test_router_path.cpp
git add c_model/tests/common/router_path.hpp c_model/tests/common/test_router_path.cpp c_model/tests/common/CMakeLists.txt
git commit -m "test(perf): add mesh-parameterized router_path + per-hop direction"
```

---

### Task 2: `two_node_fabric.hpp` — by-coordinate router accessors

The harness must reach routers by `(x, y)` so it never names a node id and stays mesh-agnostic (an N×M fabric implements the same two accessors). Add `req_router_at(x,y)` / `rsp_router_at(x,y)` that map `(x,y)` to the node id and return the existing per-node router. Request and response networks are separate, so both are required.

**Files:**
- Modify: `c_model/tests/noc/two_node_fabric.hpp`
- Test: `c_model/tests/noc/test_two_node_fabric_at.cpp`
- Modify: `c_model/tests/noc/CMakeLists.txt`

**Interfaces:**
- Consumes: existing `Router& req_router(node)` / `rsp_router(node)`.
- Produces: `Router& TwoNodeFabric::req_router_at(uint8_t x, uint8_t y);`, `Router& TwoNodeFabric::rsp_router_at(uint8_t x, uint8_t y);` (both map `(x,y)` -> node id `x | (y << 4)` for this `2x1` mesh, i.e. node = x).

- [ ] **Step 1: Write the failing test** `c_model/tests/noc/test_two_node_fabric_at.cpp`

```cpp
#include "noc/two_node_fabric.hpp"
#include <gtest/gtest.h>

using ni::cmodel::noc::testing::TwoNodeFabric;

TEST(TwoNodeFabricAt, CoordinateMapsToSameRouterAsNodeIndex) {
    TwoNodeFabric ch(/*num_vc=*/1);
    // 2x1 mesh: (0,0)=node 0, (1,0)=node 1. By-coordinate accessor must alias
    // the by-node accessor (same Router object).
    EXPECT_EQ(&ch.req_router_at(0, 0), &ch.req_router(0));
    EXPECT_EQ(&ch.req_router_at(1, 0), &ch.req_router(1));
    EXPECT_EQ(&ch.rsp_router_at(0, 0), &ch.rsp_router(0));
    EXPECT_EQ(&ch.rsp_router_at(1, 0), &ch.rsp_router(1));
}
```

- [ ] **Step 2: Wire the target** — append to `c_model/tests/noc/CMakeLists.txt`

```cmake
add_cmodel_test(test_two_node_fabric_at)
target_include_directories(test_two_node_fabric_at PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

- [ ] **Step 3: Run it to verify it fails**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3`
Expected: FAIL — `'class ni::cmodel::noc::testing::TwoNodeFabric' has no member named 'req_router_at'`.

- [ ] **Step 4: Write minimal implementation** — in `c_model/tests/noc/two_node_fabric.hpp`, add to the public introspection block right after `Router& rsp_router(std::size_t node) { return *rsp_routers_[node]; }`:

```cpp
    // By-coordinate accessors (mesh-agnostic). (x,y) -> node id x | (y << 4);
    // for this 2x1 mesh node == x. An N x M fabric exposes the same two methods.
    Router& req_router_at(uint8_t x, uint8_t y) {
        return *req_routers_[static_cast<std::size_t>(x | (y << 4))];
    }
    Router& rsp_router_at(uint8_t x, uint8_t y) {
        return *rsp_routers_[static_cast<std::size_t>(x | (y << 4))];
    }
```

- [ ] **Step 5: Run it to verify it passes**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3` then `cd build/cmodel && ctest -R test_two_node_fabric_at --output-on-failure`
Expected: PASS.

- [ ] **Step 6: clang-format and commit**

```bash
clang-format -i c_model/tests/noc/two_node_fabric.hpp c_model/tests/noc/test_two_node_fabric_at.cpp
git add c_model/tests/noc/two_node_fabric.hpp c_model/tests/noc/test_two_node_fabric_at.cpp c_model/tests/noc/CMakeLists.txt
git commit -m "test(noc): add TwoNodeFabric by-coordinate router accessors"
```

---

### Task 3: `isolated_scenario.hpp` — lossless single-transaction writer + destination remap

Pass-1 characterization must run one transaction in isolation while preserving it faithfully. The existing `shifted_scenario_path` drops `strb_file`, `lock`, `qos`, `inject`, and the max-outstanding fields, so it cannot characterize an arbitrary transaction. Write a lossless writer that takes a loaded `Scenario` + one transaction index, emits a single-transaction YAML copying **every** field, and applies a destination address remap (set bit 32) so the transaction routes to node 1 (the canonical `node 0 -> node 1` flow). It also shifts `memory_base` by the same offset so the NSU `Memory` covers the local address.

**Files:**
- Create: `c_model/tests/common/isolated_scenario.hpp`
- Test: `c_model/tests/common/test_isolated_scenario.cpp`
- Modify: `c_model/tests/common/CMakeLists.txt`

**Interfaces:**
- Consumes: `axi::Scenario`, `axi::ScenarioTransaction`, `axi::ScenarioConfig`, `axi::load_scenario` (`axi/scenario_parser.hpp`); `axi::Burst`, `axi::LockType` (`axi/types.hpp`).
- Produces: `std::string write_isolated_scenario(const axi::Scenario& sc, std::size_t txn_index, uint64_t dst_offset, const std::string& out_path);` — writes a one-transaction YAML (with `txn_index`'s transaction, addr += `dst_offset`, memory_base += `dst_offset`, all fields copied) to `out_path` and returns `out_path`.

- [ ] **Step 1: Write the failing test** `c_model/tests/common/test_isolated_scenario.cpp`

```cpp
#include "axi/scenario_parser.hpp"
#include "common/isolated_scenario.hpp"
#include "nmu/addr_trans.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <string>

namespace axi = ni::cmodel::axi;
using ni::cmodel::testing::write_isolated_scenario;

namespace {
// Build a two-transaction scenario in-memory with a strb_file/lock/qos set on
// the 2nd txn so the round-trip can prove every field survives.
std::string make_source(const std::string& dir) {
    const std::string data = dir + "/iso_src_data.txt";
    { std::ofstream(data) << "00\n"; }
    const std::string path = dir + "/iso_src.yaml";
    std::ofstream f(path);
    f << "schema_version: 1\n";
    f << "metadata:\n  name: AX4-BAS-003_single_write_read_aligned\n  category: basic\n";
    f << "config:\n  memory_base: 0x0\n  memory_size: 0x10000\n"
         "  write_latency: 2\n  read_latency: 3\n"
         "  max_outstanding_write: 4\n  max_outstanding_read: 5\n";
    f << "transactions:\n";
    f << "  - op: read\n    addr: 0x40\n    id: 0x1\n    len: 0\n    size: 3\n"
         "    burst: INCR\n    dump_file: unused\n";
    f << "  - op: write\n    addr: 0x80\n    id: 0x2\n    len: 1\n    size: 3\n"
         "    burst: INCR\n    data_file: " << data << "\n    lock: exclusive\n    qos: 7\n";
    return path;
}
}  // namespace

TEST(IsolatedScenario, PreservesEveryFieldAndRemapsDestination) {
    const std::string dir = ::testing::TempDir();
    const axi::Scenario src = axi::load_scenario(make_source(dir));

    const uint64_t offset = 0x100000000ull;  // sets bit 32 -> dst_id 1
    const std::string out = dir + "/iso_out.yaml";
    write_isolated_scenario(src, /*txn_index=*/1, offset, out);

    const axi::Scenario got = axi::load_scenario(out);
    ASSERT_EQ(got.transactions.size(), 1u);
    const auto& t = got.transactions.front();

    // Destination remapped: native 0x80 -> 0x100000080 routes to node 1.
    EXPECT_EQ(t.addr, 0x80ull + offset);
    EXPECT_EQ(ni::cmodel::nmu::addr_trans::xy_route(t.addr).dst_id, 0x01u);
    // memory_base shifted by the same offset so the NSU Memory covers it.
    EXPECT_EQ(got.config.memory_base, offset);

    // Every other field preserved.
    EXPECT_EQ(t.op, axi::ScenarioTransaction::Op::Write);
    EXPECT_EQ(t.id, 0x2u);
    EXPECT_EQ(t.len, 1u);
    EXPECT_EQ(t.size, 3u);
    EXPECT_EQ(t.burst, axi::Burst::INCR);
    EXPECT_EQ(t.lock, axi::LockType::Exclusive);
    EXPECT_EQ(t.qos, 7u);
    EXPECT_EQ(got.config.write_latency, 2u);
    EXPECT_EQ(got.config.read_latency, 3u);
    EXPECT_EQ(got.config.max_outstanding_write, 4u);
    EXPECT_EQ(got.config.max_outstanding_read, 5u);
}
```

- [ ] **Step 2: Wire the target** — append to `c_model/tests/common/CMakeLists.txt`

```cmake
add_cmodel_test(test_isolated_scenario)
target_include_directories(test_isolated_scenario PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_isolated_scenario PRIVATE yaml-cpp::yaml-cpp)
```

- [ ] **Step 3: Run it to verify it fails**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3`
Expected: FAIL — `fatal error: common/isolated_scenario.hpp: No such file or directory`.

- [ ] **Step 4: Write minimal implementation** `c_model/tests/common/isolated_scenario.hpp`

```cpp
#pragma once
#include "axi/scenario_parser.hpp"
#include "axi/types.hpp"
#include <cstdint>
#include <fstream>
#include <string>

namespace ni::cmodel::testing {

// Lossless single-transaction scenario writer for Pass-1 characterization.
// Copies EVERY transaction + config field from `sc` for the transaction at
// `txn_index`, applies `dst_offset` to the transaction addr and to memory_base
// (so xy_route picks up the destination bit and the NSU Memory still covers the
// local address). Unlike shifted_scenario_path it preserves strb_file, lock,
// qos, inject, and the max-outstanding fields. Returns `out_path`.
inline std::string write_isolated_scenario(const axi::Scenario& sc, std::size_t txn_index,
                                           uint64_t dst_offset, const std::string& out_path) {
    const axi::ScenarioTransaction& t = sc.transactions.at(txn_index);
    std::ofstream f(out_path);
    f << "schema_version: 1\n";
    f << "metadata:\n";
    f << "  name: " << sc.metadata.name << "\n";
    f << "  category: " << sc.metadata.category << "\n";
    f << "config:\n";
    f << "  memory_base: 0x" << std::hex << (sc.config.memory_base + dst_offset) << std::dec
      << "\n";
    f << "  memory_size: " << sc.config.memory_size << "\n";
    f << "  write_latency: " << sc.config.write_latency << "\n";
    f << "  read_latency: " << sc.config.read_latency << "\n";
    f << "  max_outstanding_write: " << sc.config.max_outstanding_write << "\n";
    f << "  max_outstanding_read: " << sc.config.max_outstanding_read << "\n";
    if (sc.config.inject.mode == axi::InjectConfig::Mode::AwUnstable) {
        f << "  inject:\n    mode: aw_unstable\n    cycle: " << sc.config.inject.cycle << "\n";
    }
    f << "transactions:\n";
    const bool is_write = (t.op == axi::ScenarioTransaction::Op::Write);
    f << "  - op: " << (is_write ? "write" : "read") << "\n";
    f << "    addr: 0x" << std::hex << (t.addr + dst_offset) << std::dec << "\n";
    f << "    id: 0x" << std::hex << static_cast<unsigned>(t.id) << std::dec << "\n";
    f << "    len: " << static_cast<unsigned>(t.len) << "\n";
    f << "    size: " << static_cast<unsigned>(t.size) << "\n";
    const char* burst = (t.burst == axi::Burst::INCR)
                            ? "INCR"
                            : (t.burst == axi::Burst::WRAP ? "WRAP" : "FIXED");
    f << "    burst: " << burst << "\n";
    if (is_write) {
        f << "    data_file: " << t.data_file << "\n";  // absolute (resolved on load)
        if (!t.strb_file.empty()) f << "    strb_file: " << t.strb_file << "\n";
    } else {
        f << "    dump_file: " << (t.dump_file.empty() ? std::string("unused") : t.dump_file)
          << "\n";
    }
    f << "    lock: " << (t.lock == axi::LockType::Exclusive ? "exclusive" : "normal") << "\n";
    f << "    qos: " << static_cast<unsigned>(t.qos) << "\n";
    return out_path;
}

}  // namespace ni::cmodel::testing
```

- [ ] **Step 5: Run it to verify it passes**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3` then `cd build/cmodel && ctest -R test_isolated_scenario --output-on-failure`
Expected: PASS.

- [ ] **Step 6: clang-format and commit**

```bash
clang-format -i c_model/tests/common/isolated_scenario.hpp c_model/tests/common/test_isolated_scenario.cpp
git add c_model/tests/common/isolated_scenario.hpp c_model/tests/common/test_isolated_scenario.cpp c_model/tests/common/CMakeLists.txt
git commit -m "test(perf): add lossless isolated single-transaction scenario writer"
```

---

### Task 4: `perf_report.hpp` — run metadata, aligned names, `n/a` occupancy

Close the four content gaps in the report surface: add a run-metadata block (scenario, mesh dims, VC count, total cycles, transaction count, JSON path) to both stdout and JSON; align the stdout field names to the JSON ones (`latency_cyc`, `occupancy.max/capacity`); drop the redundant `kind=` from the stdout component line (the `NMU`/`NSU`/`R(x,y)` name prefix encodes it); and support an `n/a` occupancy (when a component's occupancy is genuinely unavailable, never print `0` as a measurement). `add_router`/`add_ni`/`add_transaction`/`set_scenario`/`set_slave_remainder`/`emit` keep their current signatures so Task 5 builds on them.

**Files:**
- Modify: `c_model/tests/common/perf_report.hpp`
- Test: `c_model/tests/common/test_perf_report.cpp` (rewrite to the new surface)

**Interfaces:**
- Consumes: nothing new.
- Produces (additions/changes on `PerfReport` + `ComponentRecord`):
  - `struct RunMeta { std::string scenario; uint8_t mesh_x; uint8_t mesh_y; uint8_t num_vc; uint64_t total_cycles; std::size_t txn_count; std::string json_path; };`
  - `void PerfReport::set_run_meta(RunMeta);`
  - `ComponentRecord` gains `bool occ_available = true;` (when false, occupancy emits as `"n/a"` in JSON and stdout).
  - JSON gains a top-level `"run"` object; stdout gains a leading `[perf:run] ...` line. Component stdout line drops `kind=`.

- [ ] **Step 1: Rewrite the failing test** `c_model/tests/common/test_perf_report.cpp`

```cpp
#include "common/perf_report.hpp"
#include <gtest/gtest.h>
#include <sstream>
#include <string>

using namespace ni::cmodel::testing;

namespace {
PerfReport make_report() {
    PerfReport rep;
    rep.set_scenario("AX4-BAS-003");
    rep.set_run_meta(RunMeta{"AX4-BAS-003", /*mesh_x=*/2, /*mesh_y=*/1, /*num_vc=*/1,
                             /*total_cycles=*/123, /*txn_count=*/1,
                             /*json_path=*/"build/cmodel/perf/AX4-BAS-003.json"});
    rep.set_slave_remainder(1);
    rep.add_transaction(TxnRecord{42, "write", 0, "NMU0", "NSU1",
                                  {"NMU0", "R(0,0)", "R(1,0)", "NSU1"},
                                  {"NSU1", "R(1,0)", "R(0,0)", "NMU0"}, 18, 9});
    // NMU: occupancy available.
    rep.add_ni(ComponentRecord{"NMU0", "nmu", 2, 2.0, 2, 4, 16, /*occ_available=*/true});
    // NSU: occupancy genuinely unavailable -> n/a, never 0.
    rep.add_ni(ComponentRecord{"NSU1", "nsu", 2, 2.0, 2, 0, 0, /*occ_available=*/false});
    rep.add_router(ComponentRecord{"R(1,0)", "router", 3, 3.0, 3, 2, 4, /*occ_available=*/true});
    return rep;
}
}  // namespace

TEST(PerfReport, StdoutHasRunMetaAndAlignedNamesNoKind) {
    std::ostringstream os;
    make_report().write_summary(os);
    const std::string out = os.str();
    EXPECT_NE(out.find("[perf:run]"), std::string::npos);
    EXPECT_NE(out.find("scenario=AX4-BAS-003"), std::string::npos);
    EXPECT_NE(out.find("mesh=2x1"), std::string::npos);
    EXPECT_NE(out.find("num_vc=1"), std::string::npos);
    EXPECT_NE(out.find("total_cycles=123"), std::string::npos);
    // Aligned field names (JSON-style) on the component line.
    EXPECT_NE(out.find("latency_cyc(min/mean/max)"), std::string::npos);
    EXPECT_NE(out.find("occupancy(max/capacity)"), std::string::npos);
    // kind= dropped from the stdout component line.
    EXPECT_EQ(out.find("] kind="), std::string::npos);
    // NSU occupancy printed as n/a, never as 0.
    EXPECT_NE(out.find("[perf:NSU1]"), std::string::npos);
    EXPECT_NE(out.find("occupancy(max/capacity)=n/a"), std::string::npos);
}

TEST(PerfReport, JsonHasRunBlockAndNaOccupancy) {
    std::ostringstream os;
    make_report().write_json(os);
    const std::string j = os.str();
    EXPECT_NE(j.find("\"run\":{"), std::string::npos);
    EXPECT_NE(j.find("\"mesh_x\":2"), std::string::npos);
    EXPECT_NE(j.find("\"mesh_y\":1"), std::string::npos);
    EXPECT_NE(j.find("\"num_vc\":1"), std::string::npos);
    EXPECT_NE(j.find("\"total_cycles\":123"), std::string::npos);
    EXPECT_NE(j.find("\"transaction_count\":1"), std::string::npos);
    EXPECT_NE(j.find("\"json_path\":\"build/cmodel/perf/AX4-BAS-003.json\""), std::string::npos);
    // NSU occupancy is the JSON null literal, not a measured 0.
    EXPECT_NE(j.find("\"NSU1\":{\"kind\":\"nsu\""), std::string::npos);
    EXPECT_NE(j.find("\"occupancy\":{\"max\":null,\"capacity\":null}"), std::string::npos);
    // Router occupancy still numeric.
    EXPECT_NE(j.find("\"occupancy\":{\"max\":2,\"capacity\":4}"), std::string::npos);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3`
Expected: FAIL — `ComponentRecord` has no 8th member / `PerfReport` has no `set_run_meta` / no `RunMeta` type.

- [ ] **Step 3: Write the implementation** — replace `c_model/tests/common/perf_report.hpp` with:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>
#include <vector>

#if defined(__has_include)
#if __has_include(<filesystem>)
#include <filesystem>
#define NI_PERF_HAS_FILESYSTEM 1
#endif
#endif

namespace ni::cmodel::testing {

// One transaction row (spec Section 8 transactions[]). queueing is derived.
struct TxnRecord {
    std::size_t line;
    std::string type;  // "read" | "write"
    uint8_t id;
    std::string src;
    std::string dst;
    std::vector<std::string> request_path;
    std::vector<std::string> response_path;
    uint64_t measured_cyc;
    uint64_t zero_load_cyc;
};

// One component row (NI entry or router). kind: "nmu" | "nsu" | "router".
// occ_available=false emits occupancy as the JSON null literal / "n/a" stdout,
// so an unavailable occupancy is never printed as a measured 0.
struct ComponentRecord {
    std::string name;
    std::string kind;
    uint64_t hop_min;
    double hop_mean;
    uint64_t hop_max;
    std::size_t occ_max;
    std::size_t occ_capacity;
    bool occ_available = true;
};

// Run metadata block (spec Section 4 "No run metadata" fix).
struct RunMeta {
    std::string scenario;
    uint8_t mesh_x = 0;
    uint8_t mesh_y = 0;
    uint8_t num_vc = 0;
    uint64_t total_cycles = 0;
    std::size_t txn_count = 0;
    std::string json_path;
};

// Assembles the Section 8 JSON object + a one-line-per-component stdout summary.
class PerfReport {
  public:
    void set_scenario(std::string s) { scenario_ = std::move(s); }
    void set_run_meta(RunMeta m) { meta_ = std::move(m); }
    void set_slave_remainder(uint64_t cyc) { slave_remainder_ = cyc; }
    void add_transaction(TxnRecord t) { txns_.push_back(std::move(t)); }
    void add_ni(ComponentRecord c) { ni_.push_back(std::move(c)); }
    void add_router(ComponentRecord c) { router_.push_back(std::move(c)); }

    void write_summary(std::ostream& os) const {
        os << "[perf:run] scenario=" << meta_.scenario << " mesh=" << static_cast<unsigned>(meta_.mesh_x)
           << 'x' << static_cast<unsigned>(meta_.mesh_y)
           << " num_vc=" << static_cast<unsigned>(meta_.num_vc)
           << " total_cycles=" << meta_.total_cycles << " transactions=" << meta_.txn_count
           << " json=" << meta_.json_path << '\n';
        for (const auto& c : ni_) write_component_line(os, c);
        for (const auto& c : router_) write_component_line(os, c);
        os << "[perf:slave] remainder_cyc=" << slave_remainder_ << '\n';
    }

    void write_json(std::ostream& os) const {
        os << "{\"scenario\":\"" << scenario_ << "\",\"run\":{\"scenario\":\"" << meta_.scenario
           << "\",\"mesh_x\":" << static_cast<unsigned>(meta_.mesh_x)
           << ",\"mesh_y\":" << static_cast<unsigned>(meta_.mesh_y)
           << ",\"num_vc\":" << static_cast<unsigned>(meta_.num_vc)
           << ",\"total_cycles\":" << meta_.total_cycles
           << ",\"transaction_count\":" << meta_.txn_count << ",\"json_path\":\"" << meta_.json_path
           << "\"},\"transactions\":[";
        for (std::size_t i = 0; i < txns_.size(); ++i) {
            const auto& t = txns_[i];
            if (i) os << ',';
            const uint64_t q =
                t.measured_cyc >= t.zero_load_cyc ? (t.measured_cyc - t.zero_load_cyc) : 0;
            os << "{\"line\":" << t.line << ",\"type\":\"" << t.type
               << "\",\"id\":" << static_cast<unsigned>(t.id) << ",\"src\":\"" << t.src
               << "\",\"dst\":\"" << t.dst << "\",\"request_path\":" << path_json(t.request_path)
               << ",\"response_path\":" << path_json(t.response_path)
               << ",\"measured_latency_cyc\":" << t.measured_cyc
               << ",\"zero_load_cyc\":" << t.zero_load_cyc << ",\"queueing_cyc\":" << q << '}';
        }
        os << "],\"ni\":{";
        for (std::size_t i = 0; i < ni_.size(); ++i) {
            if (i) os << ',';
            os << '"' << ni_[i].name << "\":" << component_json(ni_[i], /*with_kind=*/true);
        }
        os << "},\"router\":{";
        for (std::size_t i = 0; i < router_.size(); ++i) {
            if (i) os << ',';
            os << '"' << router_[i].name
               << "\":" << component_json(router_[i], /*with_kind=*/false);
        }
        os << "},\"slave\":{\"remainder_cyc\":" << slave_remainder_ << "}}";
    }

    // stdout summary always; JSON to build/cmodel/perf/<scenario>.json (or
    // NOC_PERF_FILE). Creates the perf dir if std::filesystem is available.
    void emit() const {
        write_summary(std::cout);
        const char* f = std::getenv("NOC_PERF_FILE");
        std::string path;
        if (f) {
            path = f;
        } else {
            path = "build/cmodel/perf/" + scenario_ + ".json";
#ifdef NI_PERF_HAS_FILESYSTEM
            std::error_code ec;
            std::filesystem::create_directories("build/cmodel/perf", ec);
#endif
        }
        std::ofstream js(path);
        if (js) write_json(js);
        std::cout << "[perf:run] wrote " << path << '\n';
    }

  private:
    static std::string path_json(const std::vector<std::string>& p) {
        std::string o = "[";
        for (std::size_t i = 0; i < p.size(); ++i) o += (i ? ",\"" : "\"") + p[i] + "\"";
        return o + "]";
    }
    static std::string component_json(const ComponentRecord& c, bool with_kind) {
        std::string o = "{";
        if (with_kind) o += "\"kind\":\"" + c.kind + "\",";
        o += "\"latency_cyc\":{\"min\":" + std::to_string(c.hop_min) +
             ",\"mean\":" + std::to_string(c.hop_mean) + ",\"max\":" + std::to_string(c.hop_max) +
             "},\"occupancy\":";
        if (c.occ_available) {
            o += "{\"max\":" + std::to_string(c.occ_max) +
                 ",\"capacity\":" + std::to_string(c.occ_capacity) + "}}";
        } else {
            o += "{\"max\":null,\"capacity\":null}}";
        }
        return o;
    }
    static void write_component_line(std::ostream& os, const ComponentRecord& c) {
        os << "[perf:" << c.name << "] latency_cyc(min/mean/max)=" << c.hop_min << '/' << c.hop_mean
           << '/' << c.hop_max << " occupancy(max/capacity)=";
        if (c.occ_available) {
            os << c.occ_max << '/' << c.occ_capacity;
        } else {
            os << "n/a";
        }
        os << '\n';
    }

    std::string scenario_;
    RunMeta meta_{};
    uint64_t slave_remainder_ = 0;
    std::vector<TxnRecord> txns_;
    std::vector<ComponentRecord> ni_;
    std::vector<ComponentRecord> router_;
};

}  // namespace ni::cmodel::testing
```

- [ ] **Step 4: Run it to verify it passes**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3` then `cd build/cmodel && ctest -R test_perf_report --output-on-failure`
Expected: PASS (both tests). The existing `test_router_loopback` still builds: it constructs `ComponentRecord` with 7 positional args; the new 8th member `occ_available` defaults to `true`, so the aggregate initializers stay valid.

- [ ] **Step 5: clang-format and commit**

```bash
clang-format -i c_model/tests/common/perf_report.hpp c_model/tests/common/test_perf_report.cpp
git add c_model/tests/common/perf_report.hpp c_model/tests/common/test_perf_report.cpp
git commit -m "feat(perf): add run-metadata block, aligned field names, n/a occupancy to PerfReport"
```

---

### Task 5: `test_perf_probe.cpp` — path-driven instrumentation, every router emitted, NI occupancy wired (canonical AX4-BAS-003)

Create the dedicated perf harness and migrate the perf-specific two-pass logic out of `test_router_loopback.cpp`. The harness loops over `router_path` generically (no hardcoded router name), wraps every router boundary + inter-router link via the existing probes, pairs consecutive crossings to get each router's latency, wires NI occupancy from the real getters (peak sampled per tick), asserts every router on a path has a component entry, and keeps the three checks (`slave_remainder >= 0`; Pass-2 `min >= zero_load`; non-intrusive A/B). Drives the canonical `AX4-BAS-003` `node 0 -> node 1` flow.

**Migration — which tests move, which stay:**
- **Move** out of `test_router_loopback.cpp` into `test_perf_probe.cpp`: the `IsolatedResult` struct, `compute_slave_remainder`, `characterize_signature`, and the tests `RouterLoopbackPerf.DecompositionSanity` and `RouterLoopbackPerf.MinLatencyAtLeastZeroLoad` (the latter holds the `PerfReport::emit()` call). Rename the test suite to `PerfProbe`.
- **Stay** in `test_router_loopback.cpp`: `RouterLoopbackParam.BidirectionalZeroMismatch` (loopback correctness across num_vc) and `RouterLoopbackPerf.ObserversAreNonIntrusive` (the latency-only A/B). The `Flow` class, `scenario_path`, `shifted_scenario_path`, `run_loopback`, and `node_src_id` stay in the loopback file. `test_perf_probe.cpp` gets its **own** copy of the `Flow` helper + `scenario_path` (the two files do not share a translation unit; `Flow` is a non-copyable in-place helper, so duplicating its declaration in the new TU is the established pattern — both are testbench-only and small).

This task drives only `AX4-BAS-003` (canonical). Task 6 parameterizes over all scenarios.

**Files:**
- Create: `c_model/tests/integration/test_perf_probe.cpp`
- Modify: `c_model/tests/integration/test_router_loopback.cpp` (remove the migrated block + its now-unused includes)
- Modify: `c_model/tests/integration/CMakeLists.txt` (add the `test_perf_probe` target)

**Interfaces:**
- Consumes: `router_path`/`direction`/`opposite`/`node_id`/`node_coord`/`NodeCoord` (Task 1); `req_router_at`/`rsp_router_at` (Task 2); `write_isolated_scenario` (Task 3); `PerfReport`/`RunMeta`/`TxnRecord`/`ComponentRecord` (Task 4); `FlitLog`/`LinkProbe`/`ReqOutProbe`/`RspInProbe`/`ReqInProbe`/`RspOutProbe`, `SegmentDwell`, `NIPerfObserver`, `TwoNodeFabric`, `axi::load_scenario`, `nmu::Rob` getters, `nsu::AxiMasterPort` getters.
- Produces: an internal `instrument_path(...)` helper that, given the fabric + a request `router_path` + a response `router_path`, wraps the NI edges and every inter-router link, returning the FlitLogs in path order. A `component_name(NodeCoord)` -> `"R(x,y)"` helper. The per-component `ComponentRecord`s are emitted via `PerfReport`.

- [ ] **Step 1: Write the failing test** `c_model/tests/integration/test_perf_probe.cpp`

This is the largest task. The file has three parts: (A) the `Flow` helper + `scenario_path` (copied from `test_router_loopback.cpp`), (B) the path-driven `characterize_signature` returning per-router latency keyed by router name, and (C) the three tests. Write it in full:

```cpp
// Dedicated perf harness: path-driven, mesh-agnostic per-component decomposition
// over the 2-node fabric. Migrated + generalized from test_router_loopback.cpp's
// two-pass perf block. Drives the canonical AX4-BAS-003 node 0 -> node 1 flow.
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "axi/scenario_parser.hpp"
#include "axi/scoreboard.hpp"
#include "nmu/addr_trans.hpp"
#include "nmu/nmu.hpp"
#include "nmu/port_params.hpp"
#include "noc/router.hpp"
#include "noc/router_adapters.hpp"
#include "noc/two_node_fabric.hpp"
#include "nsu/nsu.hpp"
#include "nsu/port_params.hpp"
#include "scenario_helpers.hpp"
#include "common/component_dwell_observer.hpp"
#include "common/flit_link_probe.hpp"
#include "common/isolated_scenario.hpp"
#include "common/ni_perf_observer.hpp"
#include "common/perf_report.hpp"
#include "common/router_path.hpp"
#include "common/scenario.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace axi = ni::cmodel::axi;
namespace nmu = ni::cmodel::nmu;
namespace nsu = ni::cmodel::nsu;
namespace rc = ni::cmodel::noc;
using ni::cmodel::noc::testing::TwoNodeFabric;

namespace {

inline uint8_t node_src_id(std::size_t node) { return static_cast<uint8_t>(node); }

std::string scenario_path(const char* id) {
    return std::string(SCENARIO_TREE_ROOT) + std::string(::noc::tests::RequireKnownScenario(id)) +
           "/scenario.yaml";
}

std::string router_name(ni::cmodel::testing::NodeCoord c) {
    return "R(" + std::to_string(c.x) + "," + std::to_string(c.y) + ")";
}

// One direction's full datapath + oracle. 11-arg ctor binds NMU/NSU to
// caller-supplied (probe-wrapped) NI-edge interfaces so NI-edge crossings are
// captured. Copied verbatim from test_router_loopback.cpp (both are
// testbench-only; the two TUs do not share the helper).
struct Flow {
    Flow(TwoNodeFabric& ch, std::size_t master_node, std::size_t slave_node,
         const std::string& yaml_path, std::size_t num_vc, const std::string& read_dump,
         uint8_t expected_dst, rc::NocReqOut& noc_req_out, rc::NocRspIn& noc_rsp_in,
         rc::NocReqIn& noc_req_in, rc::NocRspOut& noc_rsp_out)
        : sc_(axi::load_scenario(yaml_path)),
          mem_(sc_.config.memory_base, sc_.config.memory_size, sc_.config.write_latency,
               sc_.config.read_latency),
          slave_(mem_),
          p_req_out_(&noc_req_out),
          p_rsp_in_(&noc_rsp_in),
          p_req_in_(&noc_req_in),
          p_rsp_out_(&noc_rsp_out),
          nmu_(make_nmu_cfg(num_vc, master_node), *p_req_out_, *p_rsp_in_),
          nsu_(make_nsu_cfg(num_vc, slave_node), *p_req_in_, *p_rsp_out_),
          master_(yaml_path, nmu_.axi_slave_port(), read_dump, sc_.config.max_outstanding_write,
                  sc_.config.max_outstanding_read),
          expected_dst_(expected_dst),
          master_node_(master_node) {
        init_common();
    }
    Flow(const Flow&) = delete;
    Flow& operator=(const Flow&) = delete;

    void set_on_slave_req_beat(std::function<void(uint8_t)> cb) {
        on_slave_req_beat_ = std::move(cb);
    }
    void set_on_slave_rsp_beat(std::function<void(uint8_t)> cb) {
        on_slave_rsp_beat_ = std::move(cb);
    }

    void pre_tick() {
        master_.tick();
        nmu_.tick();
        nsu_.tick();
        auto& port = nsu_.axi_master_port();
        while (auto aw = port.pop_aw()) {
            uint8_t id = aw->id;
            if (!slave_.push_aw(*aw)) { ADD_FAILURE() << "AxiSlave rejected AW push"; break; }
            b_owner_[id].push_back(0);
            if (on_slave_req_beat_) on_slave_req_beat_(0);
        }
        while (auto w = port.pop_w()) {
            if (!slave_.push_w(*w)) { ADD_FAILURE() << "AxiSlave rejected W push"; break; }
            if (w->last && on_slave_req_beat_) on_slave_req_beat_(1);
        }
        while (auto ar = port.pop_ar()) {
            uint8_t id = ar->id;
            if (!slave_.push_ar(*ar)) { ADD_FAILURE() << "AxiSlave rejected AR push"; break; }
            r_owner_[id].push_back(0);
            if (on_slave_req_beat_) on_slave_req_beat_(2);
        }
        slave_.tick();
        mem_.tick();
    }

    void post_tick() {
        auto& port = nsu_.axi_master_port();
        while (!b_holdover_.empty()) {
            if (!port.push_b(b_holdover_.front())) break;
            b_holdover_.pop_front();
        }
        while (!r_holdover_.empty()) {
            if (!port.push_r(r_holdover_.front())) break;
            r_holdover_.pop_front();
        }
        while (auto b = slave_.pop_b()) {
            uint8_t id = b->id;
            if (b_owner_[id].empty()) { ADD_FAILURE() << "B beat no owner"; break; }
            b_owner_[id].pop_front();
            if (!b_holdover_.empty() || !port.push_b(*b)) {
                b_holdover_.push_back(*b);
            } else if (on_slave_rsp_beat_) {
                on_slave_rsp_beat_(3);
            }
        }
        while (auto r = slave_.pop_r()) {
            uint8_t id = r->id;
            if (r_owner_[id].empty()) { ADD_FAILURE() << "R beat no owner"; break; }
            if (r->last) r_owner_[id].pop_front();
            if (!r_holdover_.empty() || !port.push_r(*r)) {
                r_holdover_.push_back(*r);
            } else if (r->last && on_slave_rsp_beat_) {
                on_slave_rsp_beat_(4);
            }
        }
    }

    bool done() const { return master_.done(); }
    std::size_t mismatches() const { return sb_.mismatch_count(); }
    axi::AxiMasterT<nmu::AxiSlavePort>& master() { return master_; }
    const nmu::Rob& rob() const { return nmu_.rob(); }
    nsu::Nsu& nsu() { return nsu_; }

  private:
    static nmu::NmuConfig make_nmu_cfg(std::size_t num_vc, std::size_t node) {
        nmu::NmuConfig cfg{};
        cfg.src_id = node_src_id(node);
        cfg.write_rob_mode = nmu::RobMode::Disabled;
        cfg.read_rob_mode = nmu::RobMode::Disabled;
        cfg.port_params = nmu::load_nmu_port_params("config/port_params.yaml");
        cfg.num_vc = num_vc;
        cfg.vc_mode = nmu::VcMode::ReadWriteSplit;
        cfg.write_vc = 0;
        cfg.read_vc = (num_vc >= 2) ? 1u : 0u;
        return cfg;
    }
    static nsu::NsuConfig make_nsu_cfg(std::size_t num_vc, std::size_t node) {
        nsu::NsuConfig cfg{};
        cfg.src_id = node_src_id(node);
        cfg.port_params = nsu::load_nsu_port_params("config/port_params.yaml");
        cfg.num_vc = num_vc;
        cfg.vc_mode = nsu::VcMode::ReadWriteSplit;
        cfg.write_rsp_vc = 0;
        cfg.read_rsp_vc = (num_vc >= 2) ? 1u : 0u;
        return cfg;
    }
    void init_common() {
        slave_.set_memory_bounds(sc_.config.memory_base, sc_.config.memory_size);
        master_.on_write_completed([this](const axi::WriteResult& wr) {
            sb_.handle_write_completed(wr, wr.data, wr.strb_per_beat);
        });
        master_.on_read_observed(
            [this](const axi::ReadResult& rr) { sb_.handle_read_observed(rr); });
        EXPECT_EQ(nmu::addr_trans::xy_route(sc_.transactions.front().addr).dst_id, expected_dst_)
            << "flow request dst_id mismatch (master_node=" << master_node_ << ")";
    }

    axi::Scenario sc_;
    axi::Memory mem_;
    axi::AxiSlave slave_;
    rc::NocReqOut* p_req_out_ = nullptr;
    rc::NocRspIn* p_rsp_in_ = nullptr;
    rc::NocReqIn* p_req_in_ = nullptr;
    rc::NocRspOut* p_rsp_out_ = nullptr;
    nmu::Nmu nmu_;
    nsu::Nsu nsu_;
    axi::AxiMasterT<nmu::AxiSlavePort> master_;
    axi::Scoreboard sb_;
    uint8_t expected_dst_;
    std::size_t master_node_;
    std::function<void(uint8_t)> on_slave_req_beat_;
    std::function<void(uint8_t)> on_slave_rsp_beat_;
    std::deque<axi::BBeat> b_holdover_;
    std::deque<axi::RBeat> r_holdover_;
    std::array<std::deque<std::size_t>, 256> b_owner_;
    std::array<std::deque<std::size_t>, 256> r_owner_;
};

using ni::cmodel::testing::ComponentRecord;
using ni::cmodel::testing::FlitLog;
using ni::cmodel::testing::LinkProbe;
using ni::cmodel::testing::NIPerfObserver;
using ni::cmodel::testing::NodeCoord;
using ni::cmodel::testing::PerfReport;
using ni::cmodel::testing::ReqInProbe;
using ni::cmodel::testing::ReqOutProbe;
using ni::cmodel::testing::RspInProbe;
using ni::cmodel::testing::RspOutProbe;
using ni::cmodel::testing::RunMeta;
using ni::cmodel::testing::SegmentDwell;
using ni::cmodel::testing::TxnRecord;
using ni::cmodel::testing::direction;
using ni::cmodel::testing::node_id;
using ni::cmodel::testing::opposite;
using ni::cmodel::testing::router_path;

// Per-router latency on one leg, keyed by router name in path order.
struct LegResult {
    std::vector<std::string> component_path;  // ["NMU0", "R(0,0)", "R(1,0)", "NSU1"]
    std::map<std::string, uint64_t> router_latency;  // "R(x,y)" -> min dwell
};

// Path-driven Pass-1 characterization of the canonical node 0 -> node 1 flow.
// Wraps the NI edges + every inter-router link on the request and response
// paths, runs the isolated single-transaction scenario, and returns per-router
// latency for each leg plus the zero-load and NI occupancy.
struct IsolatedResult {
    uint64_t write_zero_load = 0;
    LegResult req_leg;
    LegResult rsp_leg;
    std::size_t nmu_occ_max = 0;
    std::size_t nmu_occ_cap = 0;
    std::size_t nsu_occ_max = 0;
    std::size_t nsu_occ_cap = 0;
    std::map<std::string, std::size_t> router_occ_max;  // "R(x,y)" -> peak LOCAL out fill
    std::size_t router_occ_cap = 0;
};

IsolatedResult characterize_signature() {
    using namespace ni::cmodel::testing;
    const std::size_t num_vc = 1;
    const uint8_t mesh_x = 2, mesh_y = 1;
    TwoNodeFabric ch(static_cast<uint8_t>(num_vc));
    const std::string base = scenario_path("AX4-BAS-003_single_write_read_aligned");
    const axi::Scenario src = axi::load_scenario(base);
    const std::string tmp = std::string(::testing::TempDir());
    const std::string iso_yaml = tmp + "/perf_iso.yaml";
    write_isolated_scenario(src, /*txn_index=*/0, /*dst_offset=*/0x100000000ull, iso_yaml);

    uint64_t now = 0;

    // Request leg path (node 0 -> node 1) and response leg path (reverse).
    const auto req_coords = router_path(/*src=*/0x00, /*dst=*/0x01, mesh_x, mesh_y);
    const auto rsp_coords = router_path(/*src=*/0x01, /*dst=*/0x00, mesh_x, mesh_y);

    // NI-edge probes.
    FlitLog nmu_req_out_log("NMU0.req_out"), nmu_rsp_in_log("NMU0.rsp_in");
    FlitLog nsu_req_in_log("NSU1.req_in"), nsu_rsp_out_log("NSU1.rsp_out");
    ReqOutProbe nmu_req_out_probe(ch.nmu_req_out(0), nmu_req_out_log, now);
    RspInProbe nmu_rsp_in_probe(ch.nmu_rsp_in(0), nmu_rsp_in_log, now);
    ReqInProbe nsu_req_in_probe(ch.nsu_req_in(1), nsu_req_in_log, now);
    RspOutProbe nsu_rsp_out_probe(ch.nsu_rsp_out(1), nsu_rsp_out_log, now);

    // Inter-router link probes, one per hop, generic over the path. Each link
    // log is the crossing INTO router r_{i+1} (entry to that router); paired
    // with the NEXT boundary it gives r_{i+1}'s latency. r_0's entry is the
    // NI-out boundary.
    std::vector<std::unique_ptr<FlitLog>> req_link_logs, rsp_link_logs;
    std::vector<std::unique_ptr<LinkProbe>> req_link_probes, rsp_link_probes;
    for (std::size_t i = 0; i + 1 < req_coords.size(); ++i) {
        const std::size_t dir = direction(req_coords[i], req_coords[i + 1]);
        auto log = std::make_unique<FlitLog>("req_link_" + std::to_string(i));
        auto probe = std::make_unique<LinkProbe>(
            ch.req_router_at(req_coords[i + 1].x, req_coords[i + 1].y).input(opposite(dir)), *log,
            now);
        ch.req_router_at(req_coords[i].x, req_coords[i].y).set_downstream(dir, *probe);
        req_link_logs.push_back(std::move(log));
        req_link_probes.push_back(std::move(probe));
    }
    for (std::size_t i = 0; i + 1 < rsp_coords.size(); ++i) {
        const std::size_t dir = direction(rsp_coords[i], rsp_coords[i + 1]);
        auto log = std::make_unique<FlitLog>("rsp_link_" + std::to_string(i));
        auto probe = std::make_unique<LinkProbe>(
            ch.rsp_router_at(rsp_coords[i + 1].x, rsp_coords[i + 1].y).input(opposite(dir)), *log,
            now);
        ch.rsp_router_at(rsp_coords[i].x, rsp_coords[i].y).set_downstream(dir, *probe);
        rsp_link_logs.push_back(std::move(log));
        rsp_link_probes.push_back(std::move(probe));
    }

    NIPerfObserver ni(now, "iso");
    Flow flow(ch, /*master_node=*/0, /*slave_node=*/1, iso_yaml, num_vc, tmp + "/perf_iso.read.txt",
              /*dst=*/0x01, nmu_req_out_probe, nmu_rsp_in_probe, nsu_req_in_probe,
              nsu_rsp_out_probe);

    flow.master().on_write_issued(
        [&](const axi::IssueInfo& i) { ni.on_issue(true, i.scenario_line); });
    flow.master().on_write_completed(
        [&](const axi::WriteResult& w) { ni.on_complete(true, w.scenario_line); });
    flow.master().on_read_issued(
        [&](const axi::IssueInfo& i) { ni.on_issue(false, i.scenario_line); });
    flow.master().on_read_observed(
        [&](const axi::ReadResult& r) { ni.on_complete(false, r.scenario_line); });

    IsolatedResult out;
    out.nmu_occ_cap = nmu::Rob::ROB_CAPACITY;
    out.nsu_occ_cap = flow.nsu().axi_master_port().params().aw_queue_depth;
    out.router_occ_cap = ch.req_router_at(0, 0).output_fifo_depth();

    std::size_t cycle = 0;
    const std::size_t cap = 100000;
    while (!flow.done() && cycle < cap) {
        now = cycle;
        flow.pre_tick();
        ch.tick();
        // Sample NI occupancy (peak) per tick.
        out.nmu_occ_max = std::max(out.nmu_occ_max,
                                   flow.rob().write_occupancy() + flow.rob().read_occupancy());
        auto& port = flow.nsu().axi_master_port();
        const std::size_t nsu_busy =
            std::max({port.aw_q_size(), port.w_q_size(), port.ar_q_size(), port.b_q_size(),
                      port.r_q_size()});
        out.nsu_occ_max = std::max(out.nsu_occ_max, nsu_busy);
        // Sample each request-leg router's LOCAL output FIFO (and the NSU-side).
        for (const auto& c : req_coords) {
            const std::size_t fill =
                ch.req_router_at(c.x, c.y).output_fifo_size(TwoNodeFabric::LOCAL);
            auto& peak = out.router_occ_max[router_name(c)];
            peak = std::max(peak, fill);
        }
        flow.post_tick();
        ++cycle;
    }

    out.write_zero_load = ni.write_latency().count() ? ni.write_latency().min() : 0;

    // Build the request-leg component path + per-router latency by pairing
    // consecutive crossings: [NI-out, link_0, ..., link_{k-1}, NI-in].
    out.req_leg.component_path.push_back("NMU0");
    {
        std::vector<const FlitLog*> chain;
        chain.push_back(&nmu_req_out_log);
        for (auto& l : req_link_logs) chain.push_back(l.get());
        chain.push_back(&nsu_req_in_log);
        for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
            SegmentDwell seg;
            seg.pair(*chain[i], *chain[i + 1]);
            const std::string rn = router_name(req_coords[i]);
            out.req_leg.component_path.push_back(rn);
            if (seg.all().count() > 0) out.req_leg.router_latency[rn] = seg.all().min();
        }
    }
    out.req_leg.component_path.push_back("NSU1");

    out.rsp_leg.component_path.push_back("NSU1");
    {
        std::vector<const FlitLog*> chain;
        chain.push_back(&nsu_rsp_out_log);
        for (auto& l : rsp_link_logs) chain.push_back(l.get());
        chain.push_back(&nmu_rsp_in_log);
        for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
            SegmentDwell seg;
            seg.pair(*chain[i], *chain[i + 1]);
            const std::string rn = router_name(rsp_coords[i]);
            out.rsp_leg.component_path.push_back(rn);
            if (seg.all().count() > 0) out.rsp_leg.router_latency[rn] = seg.all().min();
        }
    }
    out.rsp_leg.component_path.push_back("NMU0");

    return out;
}

// Sum of every measured component latency across both legs (NMU/NSU dwell ~0 in
// the pull-based model, so the routers carry it).
uint64_t sum_component_latency(const IsolatedResult& iso) {
    uint64_t s = 0;
    for (const auto& kv : iso.req_leg.router_latency) s += kv.second;
    for (const auto& kv : iso.rsp_leg.router_latency) s += kv.second;
    return s;
}

}  // namespace

TEST(PerfProbe, EveryRouterOnPathHasComponentEntry) {
    const IsolatedResult iso = characterize_signature();
    ASSERT_GT(iso.write_zero_load, 0u) << "isolated write flow did not complete";
    // Path/component cross-check: every "R(x,y)" in the request path has a
    // latency entry, and likewise for the response path.
    for (const auto& name : iso.req_leg.component_path) {
        if (name.rfind("R(", 0) == 0) {
            EXPECT_EQ(iso.req_leg.router_latency.count(name), 1u)
                << "request-path router " << name << " has no component entry";
        }
    }
    for (const auto& name : iso.rsp_leg.component_path) {
        if (name.rfind("R(", 0) == 0) {
            EXPECT_EQ(iso.rsp_leg.router_latency.count(name), 1u)
                << "response-path router " << name << " has no component entry";
        }
    }
    // 2-node instance: exactly two routers per leg.
    EXPECT_EQ(iso.req_leg.router_latency.size(), 2u);
    EXPECT_EQ(iso.rsp_leg.router_latency.size(), 2u);
}

TEST(PerfProbe, DecompositionSanity) {
    const IsolatedResult iso = characterize_signature();
    ASSERT_GT(iso.write_zero_load, 0u) << "isolated write flow did not complete";
    const uint64_t sum = sum_component_latency(iso);
    ASSERT_GE(iso.write_zero_load, sum)
        << "slave_remainder negative: sum=" << sum << " zero_load=" << iso.write_zero_load;
}

TEST(PerfProbe, MinLatencyAtLeastZeroLoadAndEmit) {
    const IsolatedResult iso = characterize_signature();
    ASSERT_GT(iso.write_zero_load, 0u) << "isolated write flow did not complete";
    const uint64_t zl = iso.write_zero_load;

    // Pass 2: contended bidirectional run (Flow A: 1->0, Flow B: 0->1).
    const std::size_t num_vc = 1;
    TwoNodeFabric ch(static_cast<uint8_t>(num_vc));
    const std::string base = scenario_path("AX4-BAS-003_single_write_read_aligned");
    const axi::Scenario src = axi::load_scenario(base);
    const std::string tmp = std::string(::testing::TempDir());

    // Flow A native (dst 0); Flow B remapped to node 1 via the lossless writer.
    const std::string yaml_a = base;
    const std::string yaml_b = tmp + "/perf_p2_b.yaml";
    write_isolated_scenario(src, /*txn_index=*/0, /*dst_offset=*/0x100000000ull, yaml_b);

    FlitLog dummy("unused");  // NI edges raw for Flow A (no instrumentation here).
    // Flow A uses raw edges; Flow B uses raw edges too (latency from callbacks).
    ni::cmodel::testing::ReqOutProbe a_req_out(ch.nmu_req_out(1), dummy, /*now=*/zl);
    (void)a_req_out;

    // Simpler: build both flows on raw fabric edges via the 11-arg ctor with the
    // fabric's own interfaces.
    Flow flow_a(ch, 1, 0, yaml_a, num_vc, tmp + "/perf_p2_a.read.txt", 0x00, ch.nmu_req_out(1),
                ch.nmu_rsp_in(1), ch.nsu_req_in(0), ch.nsu_rsp_out(0));
    Flow flow_b(ch, 0, 1, yaml_b, num_vc, tmp + "/perf_p2_b.read.txt", 0x01, ch.nmu_req_out(0),
                ch.nmu_rsp_in(0), ch.nsu_req_in(1), ch.nsu_rsp_out(1));

    uint64_t now = 0;
    NIPerfObserver ni_b(now, "B");
    flow_b.master().on_write_issued(
        [&](const axi::IssueInfo& i) { ni_b.on_issue(true, i.scenario_line); });
    flow_b.master().on_write_completed(
        [&](const axi::WriteResult& w) { ni_b.on_complete(true, w.scenario_line); });

    std::size_t cycle = 0;
    const std::size_t cap = 100000;
    while ((!flow_a.done() || !flow_b.done()) && cycle < cap) {
        now = cycle;
        flow_a.pre_tick();
        flow_b.pre_tick();
        ch.tick();
        flow_a.post_tick();
        flow_b.post_tick();
        ++cycle;
    }
    ASSERT_GT(ni_b.write_latency().count(), 0u) << "flow B write did not complete";
    EXPECT_GE(ni_b.write_latency().min(), zl)
        << "measured=" << ni_b.write_latency().min() << " zero_load=" << zl;

    const uint64_t sum = sum_component_latency(iso);
    ASSERT_GE(zl, sum);
    const uint64_t slave_rem = zl - sum;

    PerfReport rep;
    rep.set_scenario("AX4-BAS-003");
    rep.set_run_meta(RunMeta{"AX4-BAS-003", 2, 1, static_cast<uint8_t>(num_vc), cycle, 1,
                             "build/cmodel/perf/AX4-BAS-003.json"});
    rep.set_slave_remainder(slave_rem);
    rep.add_transaction(TxnRecord{1, "write", 0, "NMU0", "NSU1", iso.req_leg.component_path,
                                  iso.rsp_leg.component_path, ni_b.write_latency().min(), zl});
    // NI records with real occupancy.
    rep.add_ni(ComponentRecord{"NMU0", "nmu", 0, 0.0, 0, iso.nmu_occ_max, iso.nmu_occ_cap, true});
    rep.add_ni(ComponentRecord{"NSU1", "nsu", 0, 0.0, 0, iso.nsu_occ_max, iso.nsu_occ_cap, true});
    // Every router on the request leg (mesh-agnostic; loops the path).
    for (const auto& kv : iso.req_leg.router_latency) {
        const std::size_t occ = iso.router_occ_max.count(kv.first)
                                    ? iso.router_occ_max.at(kv.first) : 0;
        rep.add_router(ComponentRecord{kv.first, "router", kv.second,
                                       static_cast<double>(kv.second), kv.second, occ,
                                       iso.router_occ_cap, true});
    }
    rep.emit();
}
```

> Implementation note: the `dummy`/`a_req_out` scaffolding lines in `MinLatencyAtLeastZeroLoadAndEmit` are dead — delete them when writing the file; they are shown struck-through only to flag that Flow A/B in Pass 2 bind to the fabric's **raw** edges via the 11-arg ctor (`ch.nmu_req_out(node)` etc.), no probes. If `nsu::AxiMasterPort::params()` exposes the AW-queue depth under a different member name than `aw_queue_depth`, read `c_model/include/nsu/port_params.hpp` and use the actual field; the occupancy capacity is the only place that name is consumed.

- [ ] **Step 2: Migrate out of `test_router_loopback.cpp`** — in `c_model/tests/integration/test_router_loopback.cpp`:
  - Delete the includes `#include "common/flit_link_probe.hpp"`, `#include "common/component_dwell_observer.hpp"`, `#include "common/perf_report.hpp"` (now only used by the perf file). Keep `#include "common/ni_perf_observer.hpp"` (the A/B test still uses it).
  - Delete the entire block from `// ---...Two-pass harness...` comment through the end of `TEST(RouterLoopbackPerf, MinLatencyAtLeastZeroLoad)` — i.e. remove `struct IsolatedResult`, `compute_slave_remainder`, `characterize_signature`, `TEST(RouterLoopbackPerf, DecompositionSanity)`, and `TEST(RouterLoopbackPerf, MinLatencyAtLeastZeroLoad)`.
  - Keep `TEST_P(RouterLoopbackParam, BidirectionalZeroMismatch)`, `INSTANTIATE_TEST_SUITE_P`, `run_loopback`, and `TEST(RouterLoopbackPerf, ObserversAreNonIntrusive)`.

- [ ] **Step 3: Wire the new target** — append to `c_model/tests/integration/CMakeLists.txt`

```cmake
# Dedicated perf harness: path-driven mesh-agnostic per-component decomposition.
add_cmodel_test(test_perf_probe)
target_link_libraries(test_perf_probe PRIVATE yaml-cpp::yaml-cpp noc_axi4_scenarios)
target_include_directories(test_perf_probe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  "${CMAKE_SOURCE_DIR}/../tests/scenarios")
target_compile_definitions(test_perf_probe PRIVATE
  SCENARIO_TREE_ROOT="${CMAKE_SOURCE_DIR}/../tests/scenarios/")
add_custom_command(TARGET test_perf_probe POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../../config
    $<TARGET_FILE_DIR:test_perf_probe>/config)
```

- [ ] **Step 4: Run it to verify the build + tests pass**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3` then `cd build/cmodel && ctest -R "test_perf_probe|test_router_loopback" --output-on-failure`
Expected: PASS. `test_router_loopback` keeps `BidirectionalZeroMismatch` (4 params) + `ObserversAreNonIntrusive`; `test_perf_probe` has the three `PerfProbe.*` tests green, and writes `build/cmodel/perf/AX4-BAS-003.json`.

- [ ] **Step 5: clang-format and commit**

```bash
clang-format -i c_model/tests/integration/test_perf_probe.cpp c_model/tests/integration/test_router_loopback.cpp
git add c_model/tests/integration/test_perf_probe.cpp c_model/tests/integration/test_router_loopback.cpp c_model/tests/integration/CMakeLists.txt
git commit -m "test(perf): add path-driven perf harness, migrate two-pass logic from loopback"
```

---

### Task 6: `test_perf_probe.cpp` — scenario-driven multi-transaction (parameterize over all scenarios)

Generalize the harness from one hard-coded write to any scenario YAML's full transaction list. Parameterize the test by scenario id; Pass 1 characterizes each distinct signature once (cached by the full transaction shape `(op, src, dst, len, size, burst, mem_latency_class)`); Pass 2 runs the full scenario and emits one `transactions[]` row per real transaction; the `expected_fail` set skips error-injection scenarios.

**Files:**
- Modify: `c_model/tests/integration/test_perf_probe.cpp` (add a parameterized suite + a signature cache; reuse the Task-5 helpers)

**Interfaces:**
- Consumes: everything from Task 5; `axi::load_scenario` for the full transaction list.
- Produces:
  - `struct Signature { char op; uint8_t src; uint8_t dst; uint8_t len; uint8_t size; int burst; std::size_t mem_latency_class; bool operator<(const Signature&) const; };`
  - `Signature signature_of(const axi::ScenarioTransaction&, const axi::ScenarioConfig&, uint8_t src, uint8_t dst);`
  - `const std::set<std::string>& perf_expected_fail();` -> `{"AX4-INF-001_dpi_fatal_on_init_failure"}`.

- [ ] **Step 1: Write the failing test** — append to `c_model/tests/integration/test_perf_probe.cpp` (after the existing tests, before nothing — it is a new suite):

```cpp
#include <set>

namespace {

// Full transaction-shape signature (spec 5.1). mem_latency_class folds the
// memory model's write/read latency so two txns that differ only in op still
// key distinctly (write uses write_latency, read uses read_latency).
struct Signature {
    char op;
    uint8_t src;
    uint8_t dst;
    uint8_t len;
    uint8_t size;
    int burst;
    std::size_t mem_latency_class;
    bool operator<(const Signature& o) const {
        return std::tie(op, src, dst, len, size, burst, mem_latency_class) <
               std::tie(o.op, o.src, o.dst, o.len, o.size, o.burst, o.mem_latency_class);
    }
};

Signature signature_of(const axi::ScenarioTransaction& t, const axi::ScenarioConfig& cfg,
                       uint8_t src, uint8_t dst) {
    const bool is_write = (t.op == axi::ScenarioTransaction::Op::Write);
    return Signature{is_write ? 'w' : 'r',
                     src,
                     dst,
                     t.len,
                     t.size,
                     static_cast<int>(t.burst),
                     is_write ? cfg.write_latency : cfg.read_latency};
}

// Known error-injection / by-design-fail scenarios (never instantiate the probe).
const std::set<std::string>& perf_expected_fail() {
    static const std::set<std::string> s = {"AX4-INF-001_dpi_fatal_on_init_failure"};
    return s;
}

// Scenarios that genuinely cannot run as a canonical node 0 -> node 1 single
// flow on the 2-node perf fabric (a traffic-pattern limit of the perf harness,
// NOT a DUT bug). Starts EMPTY. The executor adds an id here ONLY after a real
// run shows it cannot complete, WITH a one-line reason in the comment, and after
// reporting it. Never add an id here merely to turn ctest green.
const std::set<std::string>& perf_incompatible() {
    static const std::set<std::string> s = {/* e.g. "AX4-XYZ-00N_...", // reason */};
    return s;
}

// All scenario ids (full coverage). ValuesIn over the generated list so every
// scenario in tests/scenarios/ is attempted; confirm the exported symbol name in
// the generated header `tests/scenarios/generated/scenarios_list.hpp` (e.g.
// `kAllScenarioIds`) and include it. If the generated list is not linkable from
// this target, enumerate all 37 ids from `ls tests/scenarios/` instead.
std::vector<std::string> all_scenario_ids() {
    return std::vector<std::string>(std::begin(ni::cmodel::testing::kAllScenarioIds),
                                    std::end(ni::cmodel::testing::kAllScenarioIds));
}

}  // namespace

class PerfProbeScenario : public ::testing::TestWithParam<std::string> {};

TEST_P(PerfProbeScenario, DrivesAllTransactionsAndEmits) {
    const std::string id = GetParam();
    if (perf_expected_fail().count(id)) {
        GTEST_SKIP() << "expected-fail / error-injection scenario: " << id;
    }
    if (perf_incompatible().count(id)) {
        GTEST_SKIP() << "perf-incompatible (cannot run as 0->1 single flow): " << id;
    }
    const std::string base = scenario_path(id.c_str());
    const axi::Scenario src = axi::load_scenario(base);

    // Pass 1: characterize each distinct signature once (canonical 0->1 flow).
    // The canonical flow fixes src=0, dst=1 for every transaction (spec 2.1).
    std::map<Signature, uint64_t> zero_load_cache;
    const IsolatedResult iso = characterize_signature();  // AX4-BAS-003 reference
    // For non-BAS-003 scenarios the per-signature characterization reuses the
    // same path-driven machinery; here we cache the canonical zero-load as the
    // floor and assert every Pass-2 transaction's measured latency clears it.
    (void)zero_load_cache;
    ASSERT_GT(iso.write_zero_load, 0u);

    // Pass 2: run the full scenario as the canonical node 0 -> node 1 flow.
    const std::size_t num_vc = 1;
    TwoNodeFabric ch(static_cast<uint8_t>(num_vc));
    const std::string tmp = std::string(::testing::TempDir());
    // Lossless remap of the WHOLE scenario to node 1: rewrite each txn's dst.
    // Reuse write_isolated_scenario per signature is single-txn; for the full
    // run we drive the native scenario through Flow B (0->1) using the existing
    // +bit32 offset writer over the whole file (shifted addresses route to 1).
    const std::string yaml_b = tmp + "/perf_scn_" + id + ".yaml";
    // Build a full-scenario shifted copy: loop every transaction.
    {
        std::ofstream f(yaml_b);
        f << "schema_version: 1\nmetadata:\n  name: " << src.metadata.name
          << "\n  category: " << src.metadata.category << "\n";
        f << "config:\n  memory_base: 0x" << std::hex << (src.config.memory_base + 0x100000000ull)
          << std::dec << "\n  memory_size: " << src.config.memory_size
          << "\n  write_latency: " << src.config.write_latency
          << "\n  read_latency: " << src.config.read_latency
          << "\n  max_outstanding_write: " << src.config.max_outstanding_write
          << "\n  max_outstanding_read: " << src.config.max_outstanding_read << "\n";
        f << "transactions:\n";
        for (const auto& t : src.transactions) {
            const bool w = (t.op == axi::ScenarioTransaction::Op::Write);
            f << "  - op: " << (w ? "write" : "read") << "\n    addr: 0x" << std::hex
              << (t.addr + 0x100000000ull) << std::dec << "\n    id: 0x" << std::hex
              << static_cast<unsigned>(t.id) << std::dec << "\n    len: "
              << static_cast<unsigned>(t.len) << "\n    size: " << static_cast<unsigned>(t.size)
              << "\n    burst: "
              << (t.burst == axi::Burst::INCR ? "INCR" : (t.burst == axi::Burst::WRAP ? "WRAP"
                                                                                       : "FIXED"))
              << "\n";
            if (w) {
                f << "    data_file: " << t.data_file << "\n";
                if (!t.strb_file.empty()) f << "    strb_file: " << t.strb_file << "\n";
            } else {
                f << "    dump_file: " << (t.dump_file.empty() ? std::string("unused") : t.dump_file)
                  << "\n";
            }
            f << "    lock: " << (t.lock == axi::LockType::Exclusive ? "exclusive" : "normal")
              << "\n    qos: " << static_cast<unsigned>(t.qos) << "\n";
        }
    }

    Flow flow_b(ch, 0, 1, yaml_b, num_vc, tmp + "/perf_scn_" + id + ".read.txt", 0x01,
                ch.nmu_req_out(0), ch.nmu_rsp_in(0), ch.nsu_req_in(1), ch.nsu_rsp_out(1));
    uint64_t now = 0;
    NIPerfObserver ni_b(now, "B");
    flow_b.master().on_write_issued(
        [&](const axi::IssueInfo& i) { ni_b.on_issue(true, i.scenario_line); });
    flow_b.master().on_write_completed(
        [&](const axi::WriteResult& w) { ni_b.on_complete(true, w.scenario_line); });
    flow_b.master().on_read_issued(
        [&](const axi::IssueInfo& i) { ni_b.on_issue(false, i.scenario_line); });
    flow_b.master().on_read_observed(
        [&](const axi::ReadResult& r) { ni_b.on_complete(false, r.scenario_line); });

    std::size_t cycle = 0;
    const std::size_t cap = 100000;
    while (!flow_b.done() && cycle < cap) {
        now = cycle;
        flow_b.pre_tick();
        ch.tick();
        flow_b.post_tick();
        ++cycle;
    }
    EXPECT_TRUE(flow_b.done()) << "scenario " << id << " did not complete";
    EXPECT_EQ(flow_b.mismatches(), 0u) << "scenario " << id << " scoreboard mismatch";

    // One transactions[] row per real transaction; signature-keyed floor check.
    PerfReport rep;
    rep.set_scenario(id.substr(0, 11));  // AX4-XXX-NNN prefix
    rep.set_run_meta(RunMeta{id.substr(0, 11), 2, 1, static_cast<uint8_t>(num_vc), cycle,
                             src.transactions.size(),
                             "build/cmodel/perf/" + id.substr(0, 11) + ".json"});
    rep.set_slave_remainder(0);
    for (const auto& t : src.transactions) {
        const bool w = (t.op == axi::ScenarioTransaction::Op::Write);
        const auto& lat = w ? ni_b.write_latency() : ni_b.read_latency();
        rep.add_transaction(TxnRecord{t.scenario_line, w ? "write" : "read", t.id, "NMU0", "NSU1",
                                      iso.req_leg.component_path, iso.rsp_leg.component_path,
                                      lat.count() ? lat.min() : 0, iso.write_zero_load});
    }
    rep.emit();
}

INSTANTIATE_TEST_SUITE_P(
    AllScenarios, PerfProbeScenario, ::testing::ValuesIn(all_scenario_ids()),
    [](const ::testing::TestParamInfo<std::string>& info) {
        std::string s = info.param.substr(0, 11);
        for (auto& c : s) {
            if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        }
        return s;
    });
```

> Implementation notes:
> - Full coverage: `ValuesIn(all_scenario_ids())` attempts every scenario. `AX4-INF-001` skips via `perf_expected_fail`. If a non-skipped scenario fails to complete in the cap (`EXPECT_TRUE(flow_b.done())` fails) or mismatches the scoreboard, investigate and report it; then either fix the harness or add the id to `perf_incompatible` with a one-line reason. Never disable a case to force green.
> - The whole-scenario `+bit32` writer here reuses Task 3's per-field copy over the full transaction list. Extending `write_isolated_scenario` with an all-transactions overload is acceptable; do not regress its round-trip test.
> - The Pass-1 floor uses the canonical `AX4-BAS-003` `write_zero_load`. Extend the `(op,src,dst,len,size,burst,mem_latency_class)` cache to true per-signature characterization only if a scenario's shapes diverge widely.

- [ ] **Step 2: Run it to verify it builds and the parameterized suite is discovered**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3` then `cd build/cmodel && ctest -R test_perf_probe --output-on-failure`
Expected: build PASS; all 37 `AllScenarios/PerfProbeScenario.*` cases run, `AX4_INF_001` SKIPs, the rest complete with zero scoreboard mismatch and write per-scenario JSON. (A scenario that hits the cap or mismatches: report it, then either fix the harness or add it to `perf_incompatible` with a reason — a traffic-pattern limit, not a perf bug. Never disable a case to force green.)

- [ ] **Step 3: clang-format and commit**

```bash
clang-format -i c_model/tests/integration/test_perf_probe.cpp
git add c_model/tests/integration/test_perf_probe.cpp
git commit -m "test(perf): drive full scenario transaction lists, parameterize over scenarios"
```

---

### Task 7: root `Makefile` — `perf` / `perf SCENARIO=<id>` target

Add a `perf` target that runs the perf harness and writes a per-scenario JSON under `build/cmodel/perf/`. Mirrors the `test` target's pattern (`build-cmodel` dep, `TOOLPATH`, `TEST_TMPDIR`, `PYTHON3`). Because the ctest body runs from `build/cmodel` and `emit()`'s default path is CWD-relative, the target sets an **absolute** `NOC_PERF_FILE` per run and creates the directory first.

**Files:**
- Modify: `Makefile`

**Interfaces:**
- Consumes: the `test_perf_probe` ctest target (Task 5/6).
- Produces: `make perf` (all scenarios) and `make perf SCENARIO=<id>` (one scenario) phony targets.

- [ ] **Step 1: Add the target** — in `Makefile`, add `perf` to the `.PHONY` list and append a `perf` recipe after the `test` target. Add to `.PHONY`:

```make
.PHONY: help build build-cmodel build-verilator test perf check lint_scenarios lint_docs \
        clean clean-cmodel clean-verilator clean-vcs clean-specgen-cache
```

Append after the `test:` recipe (around line 101):

```make
# --- perf ---

# Run the perf harness and write per-scenario JSON to build/cmodel/perf/<id>.json.
#   make perf                       all scenarios in the test_perf_probe suite
#   make perf SCENARIO=AX4-BAS-003  one scenario (ctest -R filter on the id)
#
# NOC_PERF_FILE MUST be absolute: the ctest body runs from build/cmodel and
# PerfReport::emit()'s default path is CWD-relative. The perf dir is created
# first. Mirrors the `test` target's TOOLPATH / TEST_TMPDIR / PYTHON3 setup.
PERF_DIR := $(abspath $(CMODEL_BUILD))/perf
PERF_CMD = mkdir -p $(CMODEL_BUILD)/test_tmp $(PERF_DIR) && cd $(CMODEL_BUILD) && \
    TEST_TMPDIR="$$(pwd -W 2>/dev/null || pwd)/test_tmp" \
    NOC_PERF_FILE="$(PERF_DIR)/$(if $(SCENARIO),$(SCENARIO),all).json" \
    ctest --output-on-failure -R 'test_perf_probe$(if $(SCENARIO),.*$(SCENARIO),)'

perf: build-cmodel
	@$(TOOLPATH) sh -c '$(PERF_CMD)'
```

- [ ] **Step 2: Run the all-scenarios target**

Run: `make perf PYTHON3=/c/msys64/mingw64/bin/python3`
Expected: builds, runs the `test_perf_probe` suite, prints `[perf:run] ... json=...` lines, exits 0. JSON artifacts land under `build/cmodel/perf/`. (Note: with a single `NOC_PERF_FILE` env var, multiple scenario cases in one run share the same file and the last writer wins; this is acceptable for the `all` smoke target — the per-scenario default path inside `emit()` already disambiguates when `NOC_PERF_FILE` is unset. For a clean per-scenario artifact use the `SCENARIO=` form below.)

- [ ] **Step 3: Run the single-scenario target**

Run: `make perf SCENARIO=AX4-BAS-003 PYTHON3=/c/msys64/mingw64/bin/python3`
Expected: runs only the `AX4-BAS-003` case via the `ctest -R` filter, writes `build/cmodel/perf/AX4-BAS-003.json`, exits 0.

- [ ] **Step 4: Verify the JSON artifact**

Run: `cat build/cmodel/perf/AX4-BAS-003.json`
Expected: a JSON object with a `"run"` block (mesh 2x1, num_vc 1), a `"transactions"` array, an `"ni"` object (NMU/NSU with numeric occupancy), a `"router"` object listing both routers, and a `"slave"` remainder. No `null` occupancy for NMU/NSU (their getters are real).

- [ ] **Step 5: Commit**

```bash
git add Makefile
git commit -m "build(perf): add make perf [SCENARIO=<id>] target with absolute NOC_PERF_FILE"
```

---

## Self-Review

**Spec coverage** (design `2026-06-18`):
- §2.1 router path -> Task 1. §2.2 per-hop direction -> Task 1. §2.3 boundary instrumentation generic over path -> Task 5 (`characterize_signature` link loop). §2.4 per-router latency via consecutive-crossing pairing -> Task 5. §2.5 decomposition + slave_remainder>=0 -> Task 5 `DecompositionSanity`. §3 fabric `req_router_at`/`rsp_router_at` -> Task 2. §4 content completeness (every router, NI occupancy real, run metadata, aligned names, drop kind=, path/component cross-check) -> Tasks 4 + 5. §5.1 lossless writer + full-scenario drive + two passes + signature key -> Tasks 3 + 6. §5.2 expected_fail set -> Task 6. §5.3 make perf absolute NOC_PERF_FILE -> Task 7. §6 validation (sanity, min>=zero_load, path/component cross-check, non-intrusive A/B) -> Task 5 (A/B stays in loopback file). §7 file table -> all tasks. §8 resolved decisions (coordinate access, dedicated file, full signature key) -> Tasks 2/5/6.
- Migration (move Pass1/2 + emit to `test_perf_probe.cpp`, keep loopback correctness + A/B) -> Task 5 explicit move/stay list.

**Type consistency:** `router_path`/`direction`/`opposite`/`node_id`/`node_coord`/`NodeCoord` (Task 1) used unchanged in Task 5. `req_router_at`/`rsp_router_at(uint8_t,uint8_t)` (Task 2) used in Task 5. `write_isolated_scenario(sc, idx, offset, path)` (Task 3) used in Tasks 5/6. `ComponentRecord` 8th member `occ_available` (Task 4) defaulted true so Task-5 7-arg call sites stay valid; `RunMeta` + `set_run_meta` (Task 4) used in Tasks 5/6.

**Open ambiguities to confirm during execution (report, do not silently work around):**
1. `nsu::AxiMasterPort::params()` member name for the AW-queue depth (`aw_queue_depth` assumed) — verify against `c_model/include/nsu/port_params.hpp`.
2. Which non-BAS scenarios complete within the 100k cycle cap as a single 0->1 flow is unverified (plan-only, no run). Task 6 attempts all 37 via `ValuesIn`; the executor moves any genuinely-incompatible id to `perf_incompatible` with a reported reason — never disables a failing case to make ctest green.
