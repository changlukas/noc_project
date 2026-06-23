# NI + Router Performance Observer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two testbench-only, non-intrusive performance observers (`NIPerfObserver`, `RouterPerfObserver`) plus shared `Stats` / `PhaseController` / `PerfReport` helpers, wired into `test_router_loopback`, emitting stdout summary + `NOC_PERF=1` JSON.

**Architecture:** Observers are decoupled from concrete DUT types: `NIPerfObserver` consumes `AxiMaster` issue/completion callbacks + a `std::function<std::size_t()>` RoB-occupancy probe; `RouterPerfObserver` polls `const noc::Router&` introspection. A harness-owned `uint64_t cycle` is the single clock; both observers hold a `const uint64_t&`. Latency is gated by a per-transaction eligible flag; sampled metrics record only in the Measurement phase.

**Tech Stack:** C++17, GoogleTest, header-only under `c_model/tests/common/` (namespace `ni::cmodel::testing`). Spec: `docs/superpowers/specs/2026-06-17-ni-router-perf-observer-design.md`.

---

## File structure

New (all under `c_model/tests/common/`, header-only):
- `perf_stats.hpp` -- `Stats` + `StatsConfig` (count/sum/min/max/mean + optional threshold-bin histogram).
- `perf_common.hpp` -- `Phase`, `PhaseConfig`, `PhaseController`.
- `ni_perf_observer.hpp` -- `NIPerfObserver` + `NIPerfConfig`.
- `router_perf_observer.hpp` -- `RouterPerfObserver` + `RouterPerfConfig`.
- `perf_report.hpp` -- `PerfReport` (stdout summary + JSON writer).

New tests (`c_model/tests/common/`):
- `test_perf_stats.cpp`, `test_perf_phase.cpp`, `test_ni_perf_observer.cpp`,
  `test_router_perf_observer.cpp`, `test_perf_report.cpp`.

Modified production headers (additive, const/append-only):
- `c_model/include/axi/axi_master.hpp` -- multi-subscriber callbacks, `on_*_issued`, `IssueInfo`, wrapper forwarding.
- `c_model/include/nmu/rob.hpp` -- `write_occupancy()` / `read_occupancy()`.
- `c_model/include/noc/router.hpp` -- `front_route()`, `num_vc()`.

Modified tests:
- `c_model/tests/integration/test_router_loopback.cpp` -- observer wiring + A/B non-intrusive test.
- `c_model/tests/common/CMakeLists.txt`, `c_model/tests/noc/CMakeLists.txt` -- register new tests (and front_route test).

Reformat every touched `.hpp`/`.cpp` with `clang-format -i` before each commit.

---

## Task 1: Stats accumulator

**Files:**
- Create: `c_model/tests/common/perf_stats.hpp`
- Test: `c_model/tests/common/test_perf_stats.cpp`
- Modify: `c_model/tests/common/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

`c_model/tests/common/test_perf_stats.cpp`:

```cpp
#include "common/perf_stats.hpp"
#include <gtest/gtest.h>

using ni::cmodel::testing::Stats;
using ni::cmodel::testing::StatsConfig;

TEST(PerfStats, ScalarsOverKnownSequence) {
    Stats s(StatsConfig{});  // no histogram
    for (uint64_t v : {2u, 4u, 6u, 8u}) s.add(v);
    EXPECT_EQ(s.count(), 4u);
    EXPECT_EQ(s.sum(), 20u);
    EXPECT_EQ(s.min(), 2u);
    EXPECT_EQ(s.max(), 8u);
    EXPECT_DOUBLE_EQ(s.mean(), 5.0);
    EXPECT_DOUBLE_EQ(s.variance(), 5.0);  // ((9+1+1+9)/4) = 5
    EXPECT_TRUE(s.histogram().empty());
}

TEST(PerfStats, EmptyIsSafe) {
    Stats s(StatsConfig{});
    EXPECT_EQ(s.count(), 0u);
    EXPECT_EQ(s.min(), 0u);
    EXPECT_EQ(s.max(), 0u);
    EXPECT_DOUBLE_EQ(s.mean(), 0.0);  // no div-by-zero
}

TEST(PerfStats, ThresholdBinsCountCorrectly) {
    // thresholds {10,20}: bin0=[0,10) bin1=[10,20) bin2=[20,inf)
    Stats s(StatsConfig{{10, 20}});
    for (uint64_t v : {5u, 9u, 10u, 15u, 20u, 99u}) s.add(v);
    ASSERT_EQ(s.histogram().size(), 3u);
    EXPECT_EQ(s.histogram()[0], 2u);  // 5,9
    EXPECT_EQ(s.histogram()[1], 2u);  // 10,15
    EXPECT_EQ(s.histogram()[2], 2u);  // 20,99
}
```

- [ ] **Step 2: Register the test and run it to verify it fails**

Append to `c_model/tests/common/CMakeLists.txt`:

```cmake
add_cmodel_test(test_perf_stats)
target_include_directories(test_perf_stats PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

Run: `make build-cmodel PYTHON3="py -3"` then `cd build/cmodel && ctest -R PerfStats`
Expected: build FAIL (`perf_stats.hpp` not found).

- [ ] **Step 3: Write minimal implementation**

`c_model/tests/common/perf_stats.hpp`:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace ni::cmodel::testing {

struct StatsConfig {
    std::vector<uint64_t> bin_thresholds;  // ascending; empty = scalars only
};

// One metric's accumulator: count/sum/min/max + optional threshold-bin histogram.
// Histogram is maintained only when bin_thresholds is non-empty (NOC_PERF gate).
class Stats {
  public:
    explicit Stats(StatsConfig cfg) : thresholds_(std::move(cfg.bin_thresholds)) {
        if (!thresholds_.empty()) bins_.assign(thresholds_.size() + 1, 0);
    }
    void add(uint64_t v) {
        ++count_;
        sum_ += v;
        sumsq_ += v * v;
        if (v < min_) min_ = v;
        if (v > max_) max_ = v;
        if (!thresholds_.empty()) ++bins_[bin_index_(v)];
    }
    uint64_t count() const { return count_; }
    uint64_t sum() const { return sum_; }
    uint64_t min() const { return count_ ? min_ : 0; }
    uint64_t max() const { return count_ ? max_ : 0; }
    double mean() const { return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0; }
    double variance() const {
        if (!count_) return 0.0;
        const double m = mean();
        return static_cast<double>(sumsq_) / static_cast<double>(count_) - m * m;
    }
    const std::vector<uint64_t>& thresholds() const { return thresholds_; }
    const std::vector<uint64_t>& histogram() const { return bins_; }

  private:
    std::size_t bin_index_(uint64_t v) const {
        for (std::size_t i = 0; i < thresholds_.size(); ++i)
            if (v < thresholds_[i]) return i;
        return thresholds_.size();
    }
    std::vector<uint64_t> thresholds_;
    std::vector<uint64_t> bins_;
    uint64_t count_ = 0;
    uint64_t sum_ = 0;
    uint64_t sumsq_ = 0;
    uint64_t min_ = std::numeric_limits<uint64_t>::max();
    uint64_t max_ = 0;
};

}  // namespace ni::cmodel::testing
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd build/cmodel && ctest -R PerfStats --output-on-failure`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
clang-format -i c_model/tests/common/perf_stats.hpp c_model/tests/common/test_perf_stats.cpp
git add c_model/tests/common/perf_stats.hpp c_model/tests/common/test_perf_stats.cpp c_model/tests/common/CMakeLists.txt
git commit -m "test(perf): add Stats accumulator with threshold-bin histogram"
```

---

## Task 2: PhaseController

**Files:**
- Create: `c_model/tests/common/perf_common.hpp`
- Test: `c_model/tests/common/test_perf_phase.cpp`
- Modify: `c_model/tests/common/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

`c_model/tests/common/test_perf_phase.cpp`:

```cpp
#include "common/perf_common.hpp"
#include <gtest/gtest.h>

using ni::cmodel::testing::Phase;
using ni::cmodel::testing::PhaseConfig;
using ni::cmodel::testing::PhaseController;

TEST(PerfPhase, WarmupThenMeasurementThenDrain) {
    uint64_t now = 0;
    PhaseController p(now, PhaseConfig{/*warmup_cycles=*/3});
    now = 0; EXPECT_EQ(p.phase(), Phase::Warmup);
    now = 2; EXPECT_EQ(p.phase(), Phase::Warmup);
    now = 3; EXPECT_EQ(p.phase(), Phase::Measurement);
    now = 9; EXPECT_EQ(p.phase(), Phase::Measurement);
    p.begin_drain();
    EXPECT_EQ(p.phase(), Phase::Drain);
}

TEST(PerfPhase, ZeroWarmupStartsInMeasurement) {
    uint64_t now = 0;
    PhaseController p(now, PhaseConfig{});  // warmup_cycles=0
    EXPECT_EQ(p.phase(), Phase::Measurement);
}
```

- [ ] **Step 2: Register and run to verify it fails**

Append to `c_model/tests/common/CMakeLists.txt`:

```cmake
add_cmodel_test(test_perf_phase)
target_include_directories(test_perf_phase PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

Run: `make build-cmodel PYTHON3="py -3"`
Expected: FAIL (`perf_common.hpp` not found).

- [ ] **Step 3: Write minimal implementation**

`c_model/tests/common/perf_common.hpp`:

```cpp
#pragma once
#include <cstdint>

namespace ni::cmodel::testing {

enum class Phase { Warmup, Measurement, Drain };

struct PhaseConfig {
    uint64_t warmup_cycles = 0;
};

// Three-phase gating bound to the harness cycle counter by const reference.
// Drain is one-way: begin_drain() is called once when all masters report done().
class PhaseController {
  public:
    PhaseController(const uint64_t& now, PhaseConfig cfg) : now_(now), cfg_(cfg) {}
    void begin_drain() { draining_ = true; }
    Phase phase() const {
        if (draining_) return Phase::Drain;
        if (now_ < cfg_.warmup_cycles) return Phase::Warmup;
        return Phase::Measurement;
    }

  private:
    const uint64_t& now_;
    PhaseConfig cfg_;
    bool draining_ = false;
};

}  // namespace ni::cmodel::testing
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd build/cmodel && ctest -R PerfPhase --output-on-failure`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
clang-format -i c_model/tests/common/perf_common.hpp c_model/tests/common/test_perf_phase.cpp
git add c_model/tests/common/perf_common.hpp c_model/tests/common/test_perf_phase.cpp c_model/tests/common/CMakeLists.txt
git commit -m "test(perf): add PhaseController three-phase gating"
```

---

## Task 3: AxiMaster multi-subscriber + issue callbacks (H1, issue hook)

**Files:**
- Modify: `c_model/include/axi/axi_master.hpp` (callback members 308-309 / 498-499; completion invokes 159 / 225; AW accept 435-436; AR accept 487-488; wrapper ~683-688)
- Create: `c_model/tests/common/test_axi_master_callbacks.cpp`
- Modify: `c_model/tests/common/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

This test stands up an `AxiMaster` over an `AxiSlave`+`Memory` with a one-write/one-read inline scenario, mirroring the construction already used in `c_model/tests/common/test_test_logger.cpp` (include the same headers and build the master from an inline YAML written to `::testing::TempDir()`). It registers TWO completion callbacks and ONE issue callback per direction and asserts: both completion callbacks fire, the issue callback fires exactly once before its completion, and the issue scenario_line equals the completion scenario_line.

`c_model/tests/common/test_axi_master_callbacks.cpp`:

```cpp
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "axi/scenario_parser.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include <vector>

namespace axi = ni::cmodel::axi;

namespace {
// Minimal single-write + single-read scenario, 1 beat, 1 byte.
std::string write_inline_scenario() {
    const std::string dir = std::string(::testing::TempDir()) + "/cb_scn";
    std::ofstream(dir + ".yaml") <<
        "schema_version: 1\n"
        "metadata: {name: cb_scn, category: BAS}\n"
        "config: {data_width_bytes: 32, addr_width_bits: 40, id_width_bits: 8,\n"
        "         memory_base: 0, memory_size: 4096}\n"
        "transactions:\n"
        "  - {op: write, id: 0, addr: 0, size: 0, len: 0, burst: INCR}\n"
        "  - {op: read,  id: 0, addr: 0, size: 0, len: 0, burst: INCR}\n";
    std::ofstream(dir + ".data") << "AA\n";
    return dir + ".yaml";
}
}  // namespace

TEST(AxiMasterCallbacks, FanoutAndIssueBeforeCompletion) {
    const std::string scn = write_inline_scenario();
    axi::Memory mem(/*base=*/0, /*size=*/4096, /*write_latency_ticks=*/0, /*read_latency_ticks=*/0);
    axi::AxiSlave slave(mem);
    // axi::AxiMaster = AxiMasterT<AxiSlave> (axi_master.hpp:520). 3rd arg is the
    // read-dump path (the master opens it on construction).
    axi::AxiMaster master(scn, slave, std::string(::testing::TempDir()) + "/cb.read.txt",
                          /*max_out_w=*/1, /*max_out_r=*/1);

    int wc1 = 0, wc2 = 0, w_issue = 0;
    std::size_t issue_line = 999, complete_line = 888;
    bool issue_seen_before_complete = false;

    master.on_write_issued([&](const axi::IssueInfo& ii) {
        ++w_issue;
        issue_line = ii.scenario_line;
    });
    master.on_write_completed([&](const axi::WriteResult& wr) {
        ++wc1;
        complete_line = wr.scenario_line;
        issue_seen_before_complete = (w_issue == 1);
    });
    master.on_write_completed([&](const axi::WriteResult&) { ++wc2; });

    for (int cycle = 0; cycle < 200 && !master.done(); ++cycle) {
        master.tick();
        slave.tick();
        mem.tick();
    }
    ASSERT_TRUE(master.done());
    EXPECT_EQ(wc1, 1);   // first subscriber fired
    EXPECT_EQ(wc2, 1);   // second subscriber fired (H1: not overwritten)
    EXPECT_EQ(w_issue, 1);
    EXPECT_TRUE(issue_seen_before_complete);
    EXPECT_EQ(issue_line, complete_line);
}
```

> Implementer note: signatures are verified against `axi_master.hpp` /
> `memory.hpp` (Memory takes base/size/write_latency_ticks/read_latency_ticks;
> AxiMaster takes scenario/slave/read_dump_path/max_w/max_r). The inline YAML
> field names must match `scenario_parser.hpp`; if the parser rejects a key,
> align it with an existing `tests/scenarios/AX4-BAS-*/scenario.yaml`. The
> assertions are the contract.

- [ ] **Step 2: Register and run to verify it fails**

Append to `c_model/tests/common/CMakeLists.txt`:

```cmake
add_cmodel_test(test_axi_master_callbacks)
target_include_directories(test_axi_master_callbacks PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_axi_master_callbacks PRIVATE yaml-cpp::yaml-cpp)
```

Run: `make build-cmodel PYTHON3="py -3"`
Expected: FAIL -- `on_write_issued` / `IssueInfo` do not exist; second `on_write_completed` overwrites the first.

- [ ] **Step 3: Add IssueInfo and change callbacks to multi-subscriber + issue hooks**

In `c_model/include/axi/axi_master.hpp`, after the `ReadResult` struct (~line 88) add:

```cpp
struct IssueInfo {
    bool is_write;
    uint8_t id;
    std::size_t scenario_line;
    uint64_t addr;
    uint8_t size;
    uint8_t len;
    Burst burst;
};
```

Change the callback members (498-499) from single slots to vectors and add issue vectors:

```cpp
    std::vector<std::function<void(const WriteResult&)>> wcb_;
    std::vector<std::function<void(const ReadResult&)>> rcb_;
    std::vector<std::function<void(const IssueInfo&)>> iwcb_;
    std::vector<std::function<void(const IssueInfo&)>> ircb_;
```

Change the registrars (308-309) to append and add the issue registrars:

```cpp
    void on_write_completed(std::function<void(const WriteResult&)> cb) { wcb_.push_back(std::move(cb)); }
    void on_read_observed(std::function<void(const ReadResult&)> cb) { rcb_.push_back(std::move(cb)); }
    void on_write_issued(std::function<void(const IssueInfo&)> cb) { iwcb_.push_back(std::move(cb)); }
    void on_read_issued(std::function<void(const IssueInfo&)> cb) { ircb_.push_back(std::move(cb)); }
```

Change the completion invocation at line 159 (`wcb_(WriteResult{...})`) to fan out:

```cpp
                    for (auto& cb : wcb_)
                        cb(WriteResult{op.src_txn.addr, op.src_txn.size, op.src_txn.len,
                                       op.src_txn.burst, op.src_txn.lock, op.data,
                                       op.strb_per_beat, op.worst_resp_, b->id,
                                       op.src_txn.scenario_line});
```

Change the completion invocation at line 225 (`rcb_(ReadResult{...})`) to fan out:

```cpp
                    for (auto& cb : rcb_)
                        cb(ReadResult{op.src_txn.addr, op.src_txn.size, op.src_txn.len,
                                      op.src_txn.burst, op.read_accumulator, op.worst_resp_,
                                      r->id, op.src_txn.scenario_line});
```

Fire the write issue callback on first-sub-burst AW accept. Replace lines 435-436:

```cpp
                const bool first_aw = (op.next_aw_sub_idx_ == 0);
                if (!slave_.push_aw(aw)) return op.write_request_done();
                ++op.next_aw_sub_idx_;
                if (first_aw)
                    for (auto& cb : iwcb_)
                        cb(IssueInfo{true, id, op.src_txn.scenario_line, op.src_txn.addr,
                                     op.src_txn.size, op.src_txn.len, op.src_txn.burst});
```

Fire the read issue callback on first-sub-burst AR accept. Replace lines 487-488:

```cpp
            const bool first_ar = (op.next_ar_sub_idx_ == 0);
            if (!slave_.push_ar(ar)) return op.read_request_done();
            ++op.next_ar_sub_idx_;
            if (first_ar)
                for (auto& cb : ircb_)
                    cb(IssueInfo{false, id, op.src_txn.scenario_line, op.src_txn.addr,
                                 op.src_txn.size, op.src_txn.len, op.src_txn.burst});
```

The four `on_*` methods (including the two new `on_*_issued`) all live on the
**`AxiMasterT` template** (registrars at 308-309). Both `using AxiMaster =
AxiMasterT<AxiSlave>` (line 520, used by this test) and the loopback's
`AxiMasterT<nmu::AxiSlavePort>` inherit them automatically -- no alias-level
change needed. The `AxiMasterStandalone` cosim wrapper (683-688) is NOT in scope
this round; forwarding `on_*_issued` there is optional follow-on for the cosim
path and is omitted here.

(`<functional>` and `<vector>` are already included via the existing callback members.)

- [ ] **Step 4: Run to verify it passes**

Run: `cd build/cmodel && ctest -R AxiMasterCallbacks --output-on-failure`
Expected: PASS. Then `ctest -R 'AxiMaster|Logger|Integration'` to confirm no regression from the fanout change.

- [ ] **Step 5: Commit**

```bash
clang-format -i c_model/include/axi/axi_master.hpp c_model/tests/common/test_axi_master_callbacks.cpp
git add c_model/include/axi/axi_master.hpp c_model/tests/common/test_axi_master_callbacks.cpp c_model/tests/common/CMakeLists.txt
git commit -m "feat(axi): multi-subscriber completion callbacks + on_*_issued issue hooks"
```

---

## Task 4: Rob occupancy getters (NI occupancy introspection, feeds NIPerfObserver)

**Files:**
- Modify: `c_model/include/nmu/rob.hpp` (public section, after `set_drain_observer` ~line 76; members `write_`/`read_` at 101-102)
- Create: `c_model/tests/nmu/test_rob_occupancy.cpp`
- Modify: `c_model/tests/nmu/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

`c_model/tests/nmu/test_rob_occupancy.cpp` (uses the hermetic `NmuStandalone`,
which owns null NoC stubs and exposes `axi_slave_port()` / `tick()` / `rob()` --
`nmu.hpp:255-272`):

```cpp
#include "axi/types.hpp"
#include "nmu/nmu.hpp"
#include <gtest/gtest.h>

namespace nmu = ni::cmodel::nmu;
namespace axi = ni::cmodel::axi;

// Empty RoB reports zero occupancy; after one AW is accepted and forwarded
// (push_aw queues in the port; tick() forwards it into the RoB) the write
// occupancy is non-zero. With no B response arriving (null rsp stub) the
// outstanding entry persists, so occupancy stays > 0.
TEST(RobOccupancy, EmptyThenNonZeroAfterAw) {
    nmu::NmuConfig cfg;  // defaults
    nmu::NmuStandalone n(cfg);
    EXPECT_EQ(n.rob().write_occupancy(), 0u);
    EXPECT_EQ(n.rob().read_occupancy(), 0u);

    axi::AwBeat aw{/*id=*/0, /*addr=*/0x1000, /*len=*/0, /*size=*/5,
                   axi::Burst::INCR, /*cache=*/0xA, /*lock=*/0, /*prot=*/0x3,
                   /*region=*/0xF, /*user=*/0x55, /*qos=*/0xC};
    ASSERT_TRUE(n.axi_slave_port().push_aw(aw));
    n.tick();  // port forwards AW into the RoB
    EXPECT_GT(n.rob().write_occupancy(), 0u);
}
```

> Implementer note: if `NmuConfig` requires any non-default field for a legal
> single-NMU build, copy the minimal config from `test_request_response_loopback.cpp`.
> The contract under test is: `write_occupancy()==0` before any AW, `>0` while one
> AW is outstanding.

- [ ] **Step 2: Register and run to verify it fails**

Append to `c_model/tests/nmu/CMakeLists.txt`:

```cmake
add_cmodel_test(test_rob_occupancy)
target_include_directories(test_rob_occupancy PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

Run: `make build-cmodel PYTHON3="py -3"`
Expected: FAIL -- `write_occupancy()` does not exist.

- [ ] **Step 3: Add the getters**

In `c_model/include/nmu/rob.hpp`, in the public section (after `set_drain_observer`, before `private:`):

```cpp
    // Test introspection: current outstanding-entry count summed over all ids.
    // Counts the Disabled-mode per-id `outstanding` deques (the mode the c_model
    // runs this round). Enabled-mode slot-pool occupancy (write_entries_) is not
    // counted -- extend here if/when Enabled mode is exercised.
    std::size_t write_occupancy() const {
        std::size_t n = 0;
        for (const auto& s : write_) n += s.outstanding.size();
        return n;
    }
    std::size_t read_occupancy() const {
        std::size_t n = 0;
        for (const auto& s : read_) n += s.outstanding.size();
        return n;
    }
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd build/cmodel && ctest -R RobOccupancy --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
clang-format -i c_model/include/nmu/rob.hpp c_model/tests/nmu/test_rob_occupancy.cpp
git add c_model/include/nmu/rob.hpp c_model/tests/nmu/test_rob_occupancy.cpp c_model/tests/nmu/CMakeLists.txt
git commit -m "feat(nmu): add Rob write_occupancy/read_occupancy introspection"
```

---

## Task 5: Router front_route + num_vc (H3 fix)

**Files:**
- Modify: `c_model/include/noc/router.hpp` (introspection block ~120-125; uses private `input_fifo_` 145, `cfg_` 142, `route_compute` 60)
- Test: `c_model/tests/noc/test_router_front_route.cpp`
- Modify: `c_model/tests/noc/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

`c_model/tests/noc/test_router_front_route.cpp`:

```cpp
#include "noc/router.hpp"
#include "flit.hpp"
#include <gtest/gtest.h>

namespace noc = ni::cmodel::noc;
using ni::cmodel::Flit;

static Flit make_flit(uint8_t dst_id, uint8_t vc) {
    Flit f;
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", vc);
    f.set_header_field("axi_ch", 0);
    f.set_header_field("last", 1);
    return f;
}

TEST(RouterFrontRoute, EmptyReturnsNullopt) {
    noc::RouterConfig c;
    c.x = 0; c.y = 0; c.mesh_x_dim = 2; c.mesh_y_dim = 1; c.num_vc = 1;
    noc::Router r(c);
    EXPECT_EQ(r.num_vc(), 1u);
    EXPECT_FALSE(r.front_route(static_cast<std::size_t>(noc::RouterPort::LOCAL), 0).has_value());
}

TEST(RouterFrontRoute, FrontFlitRoutesEast) {
    noc::RouterConfig c;
    c.x = 0; c.y = 0; c.mesh_x_dim = 2; c.mesh_y_dim = 1; c.num_vc = 1;
    noc::Router r(c);
    r.input(static_cast<std::size_t>(noc::RouterPort::LOCAL)).push_flit(make_flit(/*dst=(1,0)*/ 0x01, 0));
    r.tick();  // landing -> input FIFO
    auto out = r.front_route(static_cast<std::size_t>(noc::RouterPort::LOCAL), 0);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, noc::RouterPort::EAST);
}
```

- [ ] **Step 2: Register and run to verify it fails**

Append to `c_model/tests/noc/CMakeLists.txt`:

```cmake
add_cmodel_test(test_router_front_route)
target_include_directories(test_router_front_route PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

Run: `make build-cmodel PYTHON3="py -3"`
Expected: FAIL -- `front_route` / `num_vc` do not exist.

- [ ] **Step 3: Add the accessors**

In `c_model/include/noc/router.hpp`, in the `// Test introspection` block (after `output_fifo_size`, ~line 125):

```cpp
    uint8_t num_vc() const { return cfg_.num_vc; }
    // Front flit's routed output port for (in_port, vc), or nullopt if empty.
    // Pure read; mirrors stage-2's route check without side effects.
    std::optional<RouterPort> front_route(std::size_t in_port, uint8_t vc) const {
        if (in_port >= ROUTER_PORT_COUNT || vc >= cfg_.num_vc) return std::nullopt;
        const auto& q = input_fifo_[in_port][vc];
        if (q.empty()) return std::nullopt;
        const auto dst = static_cast<uint8_t>(q.front().get_header_field("dst_id"));
        return route_compute(dst, cfg_);
    }
```

(`<optional>` is already included at router.hpp:24.)

- [ ] **Step 4: Run to verify it passes**

Run: `cd build/cmodel && ctest -R RouterFrontRoute --output-on-failure`
Expected: PASS (2 tests). Then `ctest -R Router` to confirm no regression.

- [ ] **Step 5: Commit**

```bash
clang-format -i c_model/include/noc/router.hpp c_model/tests/noc/test_router_front_route.cpp
git add c_model/include/noc/router.hpp c_model/tests/noc/test_router_front_route.cpp c_model/tests/noc/CMakeLists.txt
git commit -m "feat(noc): add Router front_route + num_vc introspection for perf probe"
```

---

## Task 6: NIPerfObserver

**Files:**
- Create: `c_model/tests/common/ni_perf_observer.hpp`
- Test: `c_model/tests/common/test_ni_perf_observer.cpp`
- Modify: `c_model/tests/common/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

`c_model/tests/common/test_ni_perf_observer.cpp`:

```cpp
#include "common/ni_perf_observer.hpp"
#include "common/perf_common.hpp"
#include <gtest/gtest.h>

using ni::cmodel::testing::NIPerfConfig;
using ni::cmodel::testing::NIPerfObserver;
using ni::cmodel::testing::PhaseConfig;
using ni::cmodel::testing::PhaseController;

TEST(NIPerfObserver, LatencyAndOutstandingPeak) {
    uint64_t now = 0;
    PhaseController phase(now, PhaseConfig{});  // measurement from cycle 0
    std::size_t fake_rob = 0;
    NIPerfObserver obs(now, phase, [&] { return fake_rob; }, NIPerfConfig{"NI"});

    now = 5;  obs.on_issue(/*is_write=*/true, /*line=*/1);   // w outstanding=1
    now = 6;  obs.on_issue(true, 2);                          // w outstanding=2 (peak)
    now = 10; obs.on_complete(true, 1);                       // latency 10-5=5
    now = 12; obs.on_complete(true, 2);                       // latency 12-6=6

    EXPECT_EQ(obs.outstanding_peak(), 2u);
    EXPECT_EQ(obs.write_latency().count(), 2u);
    EXPECT_EQ(obs.write_latency().min(), 5u);
    EXPECT_EQ(obs.write_latency().max(), 6u);
    EXPECT_EQ(obs.stuck_count(), 0u);
}

TEST(NIPerfObserver, WarmupIssuedNotCountedButOutstandingBalances) {
    uint64_t now = 0;
    PhaseController phase(now, PhaseConfig{/*warmup_cycles=*/10});
    NIPerfObserver obs(now, phase, [] { return std::size_t{0}; }, NIPerfConfig{"NI"});

    now = 2; obs.on_issue(true, 1);     // issued in Warmup -> not eligible
    now = 15; obs.on_complete(true, 1); // completes in Measurement
    EXPECT_EQ(obs.write_latency().count(), 0u);  // not recorded
    EXPECT_EQ(obs.stuck_count(), 0u);            // outstanding still balanced
}

TEST(NIPerfObserver, StuckTransactionSurfaced) {
    uint64_t now = 0;
    PhaseController phase(now, PhaseConfig{});
    NIPerfObserver obs(now, phase, [] { return std::size_t{0}; }, NIPerfConfig{"NI"});
    now = 1; obs.on_issue(false, 7);  // read issued, never completed
    EXPECT_EQ(obs.stuck_count(), 1u);
}

// H4: a transaction issued during Measurement but completing during Drain is
// still recorded, because eligibility is latched at issue, not re-checked at
// completion.
TEST(NIPerfObserver, MeasurementIssuedDrainCompletedStillRecorded) {
    uint64_t now = 0;
    PhaseController phase(now, PhaseConfig{});  // measurement from cycle 0
    NIPerfObserver obs(now, phase, [] { return std::size_t{0}; }, NIPerfConfig{"NI"});
    now = 3; obs.on_issue(true, 1);    // issued in Measurement -> eligible latched
    phase.begin_drain();               // now draining
    now = 9; obs.on_complete(true, 1); // completes in Drain
    EXPECT_EQ(obs.write_latency().count(), 1u);  // still recorded
    EXPECT_EQ(obs.write_latency().max(), 6u);    // 9 - 3
}
```

- [ ] **Step 2: Register and run to verify it fails**

Append to `c_model/tests/common/CMakeLists.txt`:

```cmake
add_cmodel_test(test_ni_perf_observer)
target_include_directories(test_ni_perf_observer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

Run: `make build-cmodel PYTHON3="py -3"`
Expected: FAIL (`ni_perf_observer.hpp` not found).

- [ ] **Step 3: Write minimal implementation**

`c_model/tests/common/ni_perf_observer.hpp`:

```cpp
#pragma once
#include "common/perf_common.hpp"
#include "common/perf_stats.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>

namespace ni::cmodel::testing {

struct NIPerfConfig {
    std::string flow_label = "NI";
    StatsConfig latency_stats{};    // thresholds when NOC_PERF=1, else empty
    StatsConfig occupancy_stats{};
};

// NI-edge observer. Driven by AxiMaster issue/completion callbacks (template-free:
// the harness forwards them) plus a per-cycle sample() polling a RoB-occupancy
// probe. Latency keyed by scenario_line within ONE NMU (one instance per NMU).
class NIPerfObserver {
  public:
    NIPerfObserver(const uint64_t& now, PhaseController& phase,
                   std::function<std::size_t()> rob_occupancy_probe, NIPerfConfig cfg)
        : now_(now), phase_(phase), rob_probe_(std::move(rob_occupancy_probe)),
          cfg_(std::move(cfg)), wr_lat_(cfg_.latency_stats), rd_lat_(cfg_.latency_stats),
          outstanding_(cfg_.occupancy_stats), rob_occ_(cfg_.occupancy_stats) {}

    void on_issue(bool is_write, std::size_t line) {
        auto& m = is_write ? wr_inflight_ : rd_inflight_;
        m[line] = InFlight{now_, phase_.phase() == Phase::Measurement};
        if (is_write) ++w_out_; else ++r_out_;
        const std::size_t cur = w_out_ + r_out_;
        if (cur > out_peak_) out_peak_ = cur;
    }
    void on_complete(bool is_write, std::size_t line) {
        auto& m = is_write ? wr_inflight_ : rd_inflight_;
        auto it = m.find(line);
        if (it != m.end()) {
            if (it->second.eligible)
                (is_write ? wr_lat_ : rd_lat_).add(now_ - it->second.issue_cycle);
            m.erase(it);
        }
        if (is_write) { if (w_out_) --w_out_; } else { if (r_out_) --r_out_; }
    }
    // Called once per cycle by the harness, AFTER component ticks.
    void sample() {
        if (phase_.phase() != Phase::Measurement) return;
        outstanding_.add(w_out_ + r_out_);
        rob_occ_.add(rob_probe_ ? rob_probe_() : 0);
    }

    const std::string& label() const { return cfg_.flow_label; }
    const Stats& write_latency() const { return wr_lat_; }
    const Stats& read_latency() const { return rd_lat_; }
    const Stats& outstanding() const { return outstanding_; }
    const Stats& rob_occupancy() const { return rob_occ_; }
    std::size_t outstanding_peak() const { return out_peak_; }
    std::size_t stuck_count() const { return wr_inflight_.size() + rd_inflight_.size(); }

  private:
    struct InFlight {
        uint64_t issue_cycle;
        bool eligible;
    };
    const uint64_t& now_;
    PhaseController& phase_;
    std::function<std::size_t()> rob_probe_;
    NIPerfConfig cfg_;
    Stats wr_lat_, rd_lat_, outstanding_, rob_occ_;
    std::map<std::size_t, InFlight> wr_inflight_, rd_inflight_;
    std::size_t w_out_ = 0, r_out_ = 0, out_peak_ = 0;
};

}  // namespace ni::cmodel::testing
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd build/cmodel && ctest -R NIPerfObserver --output-on-failure`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
clang-format -i c_model/tests/common/ni_perf_observer.hpp c_model/tests/common/test_ni_perf_observer.cpp
git add c_model/tests/common/ni_perf_observer.hpp c_model/tests/common/test_ni_perf_observer.cpp c_model/tests/common/CMakeLists.txt
git commit -m "test(perf): add NIPerfObserver (NI-edge latency/outstanding/rob occupancy)"
```

---

## Task 7: RouterPerfObserver

**Files:**
- Create: `c_model/tests/common/router_perf_observer.hpp`
- Test: `c_model/tests/common/test_router_perf_observer.cpp`
- Modify: `c_model/tests/common/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

`c_model/tests/common/test_router_perf_observer.cpp`:

```cpp
#include "common/router_perf_observer.hpp"
#include "common/perf_common.hpp"
#include "noc/router.hpp"
#include "flit.hpp"
#include <gtest/gtest.h>

namespace noc = ni::cmodel::noc;
using ni::cmodel::Flit;
using ni::cmodel::testing::PhaseConfig;
using ni::cmodel::testing::PhaseController;
using ni::cmodel::testing::RouterPerfConfig;
using ni::cmodel::testing::RouterPerfObserver;

static Flit make_flit(uint8_t dst_id) {
    Flit f;
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("axi_ch", 0);
    f.set_header_field("last", 1);
    return f;
}

TEST(RouterPerfObserver, IdleRouterZeroStall) {
    noc::RouterConfig c; c.x = 0; c.y = 0; c.mesh_x_dim = 2; c.mesh_y_dim = 1; c.num_vc = 1;
    noc::Router r(c);
    uint64_t now = 0;
    PhaseController phase(now, PhaseConfig{});
    RouterPerfObserver obs(now, phase, {{"req0", &r}}, RouterPerfConfig{});
    obs.sample();
    EXPECT_EQ(obs.credit_stall_cycles(), 0u);
}

TEST(RouterPerfObserver, FrontFlitNoCreditAccruesStall) {
    // Deterministic stall: vc_depth=1, output_fifo_depth=1, EAST has no
    // downstream wired. flit1 consumes the single EAST credit (1->0) and parks
    // in the output FIFO (stage-3 finds no downstream, so no credit return).
    // flit2 then sits in the input FIFO routed EAST with credit(EAST,0)==0.
    noc::RouterConfig c; c.x = 0; c.y = 0; c.mesh_x_dim = 2; c.mesh_y_dim = 1;
    c.num_vc = 1; c.vc_depth = 1; c.output_fifo_depth = 1;
    noc::Router r(c);
    const auto LOCAL = static_cast<std::size_t>(noc::RouterPort::LOCAL);

    r.input(LOCAL).push_flit(make_flit(0x01));  // flit1, dst (1,0) -> EAST
    r.tick();  // stage1: flit1 landing -> input FIFO
    r.tick();  // stage2: flit1 granted, EAST credit 1->0, parks in output FIFO

    uint64_t now = 0;
    PhaseController phase(now, PhaseConfig{});
    RouterPerfObserver obs(now, phase, {{"req0", &r}}, RouterPerfConfig{});

    r.input(LOCAL).push_flit(make_flit(0x01));  // flit2, dst EAST
    r.tick();  // flit2 -> input FIFO; cannot be granted (credit(EAST,0)==0)
    obs.sample();
    EXPECT_GT(obs.credit_stall_cycles(), 0u);
}
```

- [ ] **Step 2: Register and run to verify it fails**

Append to `c_model/tests/common/CMakeLists.txt`:

```cmake
add_cmodel_test(test_router_perf_observer)
target_include_directories(test_router_perf_observer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

Run: `make build-cmodel PYTHON3="py -3"`
Expected: FAIL (`router_perf_observer.hpp` not found).

- [ ] **Step 3: Write minimal implementation**

`c_model/tests/common/router_perf_observer.hpp`:

```cpp
#pragma once
#include "common/perf_common.hpp"
#include "common/perf_stats.hpp"
#include "noc/router.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ni::cmodel::testing {

struct ObservedRouter {
    std::string label;
    const noc::Router* router;
};

struct RouterPerfConfig {
    StatsConfig occupancy_stats{};  // thresholds when NOC_PERF=1, else empty
};

// Polls a set of Routers once per cycle (after their tick). Credit-stall =
// a (in_port, vc) front flit routed to output p with credit(p,vc)==0.
class RouterPerfObserver {
  public:
    RouterPerfObserver(const uint64_t& now, PhaseController& phase,
                       std::vector<ObservedRouter> routers, RouterPerfConfig cfg)
        : now_(now), phase_(phase), routers_(std::move(routers)), cfg_(std::move(cfg)) {
        per_router_.reserve(routers_.size());
        for (const auto& r : routers_) per_router_.push_back(PerRouter{cfg_.occupancy_stats});
    }

    void sample() {
        (void)now_;
        if (phase_.phase() != Phase::Measurement) return;
        for (std::size_t ri = 0; ri < routers_.size(); ++ri) {
            const noc::Router& r = *routers_[ri].router;
            PerRouter& pr = per_router_[ri];
            const uint8_t nvc = r.num_vc();
            for (std::size_t in = 0; in < noc::ROUTER_PORT_COUNT; ++in) {
                for (uint8_t vc = 0; vc < nvc; ++vc) {
                    auto out = r.front_route(in, vc);
                    if (out && r.credit(static_cast<std::size_t>(*out), vc) == 0) {
                        ++credit_stall_cycles_;
                        ++pr.stall;
                    }
                    pr.in_fifo.add(r.input_fifo_size(in, vc));
                }
            }
            for (std::size_t p = 0; p < noc::ROUTER_PORT_COUNT; ++p) {
                pr.out_fifo.add(r.output_fifo_size(p));
                ++pr.out_cycles[p];
                if (r.output_fifo_size(p) > 0) ++pr.out_nonempty[p];
            }
        }
    }

    std::size_t credit_stall_cycles() const { return credit_stall_cycles_; }
    std::size_t router_count() const { return routers_.size(); }
    const std::string& label(std::size_t i) const { return routers_[i].label; }
    double out_nonempty_ratio(std::size_t i, std::size_t port) const {
        const auto& pr = per_router_[i];
        return pr.out_cycles[port] ? static_cast<double>(pr.out_nonempty[port]) /
                                         static_cast<double>(pr.out_cycles[port])
                                   : 0.0;
    }
    const Stats& in_fifo(std::size_t i) const { return per_router_[i].in_fifo; }
    const Stats& out_fifo(std::size_t i) const { return per_router_[i].out_fifo; }
    std::size_t stall(std::size_t i) const { return per_router_[i].stall; }

  private:
    struct PerRouter {
        explicit PerRouter(StatsConfig c) : in_fifo(c), out_fifo(c) {}
        Stats in_fifo;
        Stats out_fifo;
        std::size_t stall = 0;
        std::size_t out_cycles[noc::ROUTER_PORT_COUNT] = {};
        std::size_t out_nonempty[noc::ROUTER_PORT_COUNT] = {};
    };
    const uint64_t& now_;
    PhaseController& phase_;
    std::vector<ObservedRouter> routers_;
    RouterPerfConfig cfg_;
    std::vector<PerRouter> per_router_;
    std::size_t credit_stall_cycles_ = 0;
};

}  // namespace ni::cmodel::testing
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd build/cmodel && ctest -R RouterPerfObserver --output-on-failure`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
clang-format -i c_model/tests/common/router_perf_observer.hpp c_model/tests/common/test_router_perf_observer.cpp
git add c_model/tests/common/router_perf_observer.hpp c_model/tests/common/test_router_perf_observer.cpp c_model/tests/common/CMakeLists.txt
git commit -m "test(perf): add RouterPerfObserver (credit-stall/fifo occupancy)"
```

---

## Task 8: PerfReport (stdout summary + JSON)

**Files:**
- Create: `c_model/tests/common/perf_report.hpp`
- Test: `c_model/tests/common/test_perf_report.cpp`
- Modify: `c_model/tests/common/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

`c_model/tests/common/test_perf_report.cpp`:

```cpp
#include "common/ni_perf_observer.hpp"
#include "common/perf_report.hpp"
#include "common/perf_common.hpp"
#include <gtest/gtest.h>
#include <sstream>
#include <string>

using namespace ni::cmodel::testing;

TEST(PerfReport, SummaryLineContainsLabels) {
    uint64_t now = 0;
    PhaseController phase(now, PhaseConfig{});
    NIPerfObserver ni(now, phase, [] { return std::size_t{0}; }, NIPerfConfig{"flowA"});
    now = 1; ni.on_issue(true, 1);
    now = 4; ni.on_complete(true, 1);

    std::ostringstream os;
    PerfReport rep;
    rep.add_ni(&ni);
    rep.write_summary(os);
    const std::string out = os.str();
    EXPECT_NE(out.find("[perf:flowA]"), std::string::npos);
    EXPECT_NE(out.find("wr_lat"), std::string::npos);
}
```

- [ ] **Step 2: Register and run to verify it fails**

Append to `c_model/tests/common/CMakeLists.txt`:

```cmake
add_cmodel_test(test_perf_report)
target_include_directories(test_perf_report PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

Run: `make build-cmodel PYTHON3="py -3"`
Expected: FAIL (`perf_report.hpp` not found).

- [ ] **Step 3: Write minimal implementation**

`c_model/tests/common/perf_report.hpp`:

```cpp
#pragma once
#include "common/ni_perf_observer.hpp"
#include "common/router_perf_observer.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>
#include <vector>

namespace ni::cmodel::testing {

// Aggregates NI + Router observers. Always prints a scalar stdout summary;
// writes a JSON file only under NOC_PERF=1.
class PerfReport {
  public:
    void add_ni(const NIPerfObserver* ni) { ni_.push_back(ni); }
    void add_router(const RouterPerfObserver* r) { router_ = r; }

    void write_summary(std::ostream& os) const {
        for (const auto* ni : ni_) {
            os << "[perf:" << ni->label() << "] "
               << "wr_lat(min/mean/max)=" << ni->write_latency().min() << '/'
               << ni->write_latency().mean() << '/' << ni->write_latency().max() << ' '
               << "rd_lat(min/mean/max)=" << ni->read_latency().min() << '/'
               << ni->read_latency().mean() << '/' << ni->read_latency().max() << ' '
               << "outstanding_peak=" << ni->outstanding_peak() << ' '
               << "rob_occ_peak=" << ni->rob_occupancy().max() << ' '
               << "stuck=" << ni->stuck_count() << '\n';
        }
        if (router_) {
            os << "[perf:ROUTER] credit_stall_cycles=" << router_->credit_stall_cycles() << '\n';
        }
    }

    // Always summary; JSON only under NOC_PERF=1.
    void emit(const std::string& run_label) const {
        write_summary(std::cout);
        const char* g = std::getenv("NOC_PERF");
        if (!g || std::string(g) != "1") return;
        const char* f = std::getenv("NOC_PERF_FILE");
        const std::string path = f ? std::string(f) : ("perf_" + run_label + ".json");
        std::ofstream js(path);
        if (js) write_json(js);
    }

    void write_json(std::ostream& os) const {
        os << "{\"ni\":[";
        for (std::size_t i = 0; i < ni_.size(); ++i) {
            const auto* ni = ni_[i];
            if (i) os << ',';
            os << "{\"label\":\"" << ni->label() << "\","
               << "\"write_latency\":" << stat_json(ni->write_latency()) << ','
               << "\"read_latency\":" << stat_json(ni->read_latency()) << ','
               << "\"outstanding_peak\":" << ni->outstanding_peak() << ','
               << "\"outstanding\":" << stat_json(ni->outstanding()) << ','
               << "\"rob_occupancy\":" << stat_json(ni->rob_occupancy()) << '}';
        }
        os << "],\"router\":{";
        if (router_) {
            os << "\"credit_stall_cycles\":" << router_->credit_stall_cycles() << ",\"per_router\":[";
            for (std::size_t i = 0; i < router_->router_count(); ++i) {
                if (i) os << ',';
                os << "{\"label\":\"" << router_->label(i) << "\","
                   << "\"stall\":" << router_->stall(i) << ','
                   << "\"in_fifo\":" << stat_json(router_->in_fifo(i)) << ','
                   << "\"out_fifo\":" << stat_json(router_->out_fifo(i)) << '}';
            }
            os << ']';
        } else {
            os << "\"credit_stall_cycles\":0";
        }
        os << "}}";
    }

  private:
    static std::string stat_json(const Stats& s) {
        std::string o = "{\"count\":" + std::to_string(s.count()) +
                        ",\"min\":" + std::to_string(s.min()) +
                        ",\"max\":" + std::to_string(s.max()) +
                        ",\"mean\":" + std::to_string(s.mean()) +
                        ",\"variance\":" + std::to_string(s.variance());
        if (!s.histogram().empty()) {
            o += ",\"histogram\":{\"thresholds\":[";
            for (std::size_t i = 0; i < s.thresholds().size(); ++i)
                o += (i ? "," : "") + std::to_string(s.thresholds()[i]);
            o += "],\"bins\":[";
            for (std::size_t i = 0; i < s.histogram().size(); ++i)
                o += (i ? "," : "") + std::to_string(s.histogram()[i]);
            o += "]}";
        }
        return o + "}";
    }
    std::vector<const NIPerfObserver*> ni_;
    const RouterPerfObserver* router_ = nullptr;
};

}  // namespace ni::cmodel::testing
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd build/cmodel && ctest -R PerfReport --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
clang-format -i c_model/tests/common/perf_report.hpp c_model/tests/common/test_perf_report.cpp
git add c_model/tests/common/perf_report.hpp c_model/tests/common/test_perf_report.cpp c_model/tests/common/CMakeLists.txt
git commit -m "test(perf): add PerfReport stdout summary + NOC_PERF JSON"
```

---

## Task 9: Wire observers into test_router_loopback + non-intrusive A/B test

**Files:**
- Modify: `c_model/tests/integration/test_router_loopback.cpp` (Flow class ~95-233; driving loop 299-313)

The `Flow` class owns `master_`, `nmu_`, scoreboard. Add accessors so the test
can attach an `NIPerfObserver`, and run the existing parametrised body once more
with observers attached, asserting the loop's `cycle` and scoreboard mismatches
are identical to a clean run.

- [ ] **Step 1: Add Flow accessors**

In the `Flow` class public section (near `done()` / `mismatches()` ~line 233):

```cpp
    auto& master() { return master_; }
    const ni::cmodel::nmu::Rob& rob() const { return nmu_.rob(); }
    uint8_t node_index() const { return static_cast<uint8_t>(master_node_); }
```

(If `master_node_` is not stored, add `std::size_t master_node_;` set in the ctor.)

- [ ] **Step 2: Write the failing non-intrusive test**

Add to `c_model/tests/integration/test_router_loopback.cpp` (after the existing
`INSTANTIATE_TEST_SUITE_P`), a standalone TEST that runs the num_vc=1 fabric
twice -- once plain, once with observers -- and asserts identical cycle count
and mismatches:

```cpp
#include "common/ni_perf_observer.hpp"
#include "common/router_perf_observer.hpp"
#include "common/perf_common.hpp"

namespace {

std::size_t run_loopback(bool with_observers, std::size_t* stall_out) {
    using namespace ni::cmodel::testing;
    const std::size_t num_vc = 1;
    TwoNodeFabric ch;
    const std::string base = scenario_path("AX4-BAS-003_single_write_read_aligned");
    const std::string tmp = std::string(::testing::TempDir());
    Flow flow_a(ch, 1, 0, base, num_vc, tmp + "/ab_a.read.txt", 0x00);
    const std::string yaml_b = shifted_scenario_path(base, 0x100000000, 9001);
    Flow flow_b(ch, 0, 1, yaml_b, num_vc, tmp + "/ab_b.read.txt", 0x01);

    uint64_t now = 0;
    PhaseController phase(now, PhaseConfig{});
    NIPerfObserver ni_a(now, phase, [&] { return flow_a.rob().write_occupancy() + flow_a.rob().read_occupancy(); }, NIPerfConfig{"A"});
    NIPerfObserver ni_b(now, phase, [&] { return flow_b.rob().write_occupancy() + flow_b.rob().read_occupancy(); }, NIPerfConfig{"B"});
    RouterPerfObserver router_obs(now, phase,
        {{"req0", &ch.req_router(0)}, {"req1", &ch.req_router(1)},
         {"rsp0", &ch.rsp_router(0)}, {"rsp1", &ch.rsp_router(1)}}, RouterPerfConfig{});

    if (with_observers) {
        flow_a.master().on_write_issued([&](const ni::cmodel::axi::IssueInfo& i){ ni_a.on_issue(true, i.scenario_line); });
        flow_a.master().on_read_issued([&](const ni::cmodel::axi::IssueInfo& i){ ni_a.on_issue(false, i.scenario_line); });
        flow_a.master().on_write_completed([&](const ni::cmodel::axi::WriteResult& w){ ni_a.on_complete(true, w.scenario_line); });
        flow_a.master().on_read_observed([&](const ni::cmodel::axi::ReadResult& r){ ni_a.on_complete(false, r.scenario_line); });
        flow_b.master().on_write_issued([&](const ni::cmodel::axi::IssueInfo& i){ ni_b.on_issue(true, i.scenario_line); });
        flow_b.master().on_read_issued([&](const ni::cmodel::axi::IssueInfo& i){ ni_b.on_issue(false, i.scenario_line); });
        flow_b.master().on_write_completed([&](const ni::cmodel::axi::WriteResult& w){ ni_b.on_complete(true, w.scenario_line); });
        flow_b.master().on_read_observed([&](const ni::cmodel::axi::ReadResult& r){ ni_b.on_complete(false, r.scenario_line); });
    }

    std::size_t cycle = 0;
    const std::size_t cap = 100000;
    while ((!flow_a.done() || !flow_b.done()) && cycle < cap) {
        now = cycle;  // cycle-start timestamp: callbacks (fired inside the ticks)
                      // and sample() below all read the SAME now this iteration
        flow_a.pre_tick();
        flow_b.pre_tick();
        ch.tick();
        flow_a.post_tick();
        flow_b.post_tick();
        if (with_observers) { ni_a.sample(); ni_b.sample(); router_obs.sample(); }
        ++cycle;
    }
    if (with_observers && (flow_a.done() && flow_b.done())) phase.begin_drain();
    if (stall_out) *stall_out = router_obs.credit_stall_cycles();
    EXPECT_EQ(flow_a.mismatches(), 0u);
    EXPECT_EQ(flow_b.mismatches(), 0u);
    return cycle;
}

}  // namespace

TEST(RouterLoopbackPerf, ObserversAreNonIntrusive) {
    std::size_t stall = 0;
    const std::size_t plain = run_loopback(/*with_observers=*/false, nullptr);
    const std::size_t obs = run_loopback(/*with_observers=*/true, &stall);
    EXPECT_EQ(plain, obs) << "attaching observers changed cycle-to-completion";
}
```

- [ ] **Step 3: Run to verify it fails, then build**

Run: `make build-cmodel PYTHON3="py -3"`
If `test_router_loopback` does not yet include the common headers, add to its
`target_include_directories` in `c_model/tests/integration/CMakeLists.txt`:
`${CMAKE_CURRENT_SOURCE_DIR}/..` (so `common/...` resolves).
Expected first run: build error until Flow accessors (Step 1) are present.

- [ ] **Step 4: Run to verify it passes**

Run: `cd build/cmodel && ctest -R RouterLoopbackPerf --output-on-failure`
Expected: PASS -- identical cycle count with and without observers.

- [ ] **Step 5: Commit**

```bash
clang-format -i c_model/tests/integration/test_router_loopback.cpp
git add c_model/tests/integration/test_router_loopback.cpp c_model/tests/integration/CMakeLists.txt
git commit -m "test(perf): wire NI+Router observers into router loopback, assert non-intrusive"
```

---

## Task 10: Full gate + plan cleanup

- [ ] **Step 1: Run the full pre-submit gate**

Run: `make check PYTHON3="py -3"`
Expected: lint_scenarios + lint_docs + build + full ctest all green, including the
new `PerfStats`, `PerfPhase`, `AxiMasterCallbacks`, `RobOccupancy`,
`RouterFrontRoute`, `NIPerfObserver`, `RouterPerfObserver`, `PerfReport`,
`RouterLoopbackPerf` tests.

- [ ] **Step 2: Optional manual JSON smoke**

Run: `cd build/cmodel && NOC_PERF=1 ctest -R RouterLoopbackPerf` and confirm a
`perf_*.json` is produced and parses (the integration test does not assert JSON;
this is a manual confirmation only).

- [ ] **Step 3: Remove this plan and the spec's Draft status**

Once all tasks are merged, delete `docs/superpowers/plans/2026-06-17-ni-router-perf-observer.md`
and update the spec header `Status:` to `Implemented`.

```bash
git rm docs/superpowers/plans/2026-06-17-ni-router-perf-observer.md
git commit -m "chore(perf): remove completed implementation plan"
```

---

## Self-review notes

- Spec coverage: Stats (T1), PhaseController (T2), AxiMaster issue+fanout / H1 (T3),
  Rob occupancy (T4), Router front_route / H3 (T5), NIPerfObserver + per-NMU key /
  H2 + eligible flag / H4 (T6), RouterPerfObserver (T7), PerfReport stdout+JSON (T8),
  loopback wiring + non-intrusive A/B (T9). QoS, CSR, layer-1 BFM, deep congestion,
  credit-RTT are spec non-goals -- no tasks, intentional.
- Type consistency: `Stats(StatsConfig)`, `PhaseController(const uint64_t&, PhaseConfig)`,
  `NIPerfObserver(now, phase, std::function<std::size_t()>, NIPerfConfig)`,
  `RouterPerfObserver(now, phase, std::vector<ObservedRouter>, RouterPerfConfig)`,
  `IssueInfo{is_write,id,scenario_line,addr,size,len,burst}` are used identically
  across tasks.
- Codex plan-review (2026-06-17) resolved: T3 Memory/AxiMaster ctor signatures
  corrected and verified against `memory.hpp` / `axi_master.hpp:103,520`; T4 uses
  the concrete hermetic `NmuStandalone`; T6 adds the H4 drain-latency test
  (Measurement-issued + Drain-completed -> recorded); T7 uses a deterministic
  EAST-credit-exhaustion stall sequence; T9 sets the cycle timestamp at loop top
  so callbacks and `sample()` share `now`. `Stats` gains `variance()`; JSON gains
  `outstanding`/`variance`/`per_router` (spec §6 marks `phases` + per-port ratio
  array deferred).
```
