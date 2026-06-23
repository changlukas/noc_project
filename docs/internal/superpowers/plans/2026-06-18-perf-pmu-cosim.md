# In-fabric PMU (co-sim) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Single-run, in-fabric AXI/NoC performance monitor read out of `make run-tb-top SCENARIO=<id>` as `perf.json`, replacing the deleted two-pass c_model harness.

**Architecture:** Hybrid. SV passive monitors tap the AXI slot wires (per-id latency FIFO, idle/byte counters) and the inter-router link wires (flit count + credit-deficit stall); a C++ `PerfCollector` aggregates via DPI and writes the schema JSON. C-side per-cycle DPI samples router/NI occupancy (no production change beyond const getters).

**Tech Stack:** C++17, GoogleTest, SystemVerilog (Verilator), DPI-C.

Spec: `docs/superpowers/specs/2026-06-18-perf-pmu-cosim-design.md` (§5.1 = JSON schema; §8.1 = validation matrix).

## Global Constraints

- Build c_model: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3` (never `py -3`).
- Build co-sim: `make build-verilator PYTHON3=/c/msys64/mingw64/bin/python3`.
- c_model tests: `cd build/cmodel && ctest -R <name> --output-on-failure`.
- Run co-sim: `cd cosim/verilator && make run-tb-top SCENARIO=<id>`.
- `clang-format -i` every edited `.hpp`/`.cpp` before commit.
- snake_case vars/methods, PascalCase types. No `--no-verify`. No production `AxiMaster`/`Router` behavior change (getters only).
- Monitors are passive: read wires / const getters only. Non-intrusive A/B (Task 7) must pass.
- Latency: start = address-accept; read end = RLAST; write end = B-response. Same-id correlation = per-(id,direction) in-order FIFO; underflow/overflow `$fatal`.
- Link stall = `credit_count == 0` cycles (NOT `valid && credit_count==0`; valid is gated on credit).
- Commit message format `type(scope): description`.

## DPI ABI (defined here; consumed by all tasks)

```
// SV -> C, per completed transaction (manager + subordinate slots)
void cmodel_perf_axi_txn(const char* slot, int id, int is_write,
                         long long addr, int len, int size,
                         long long accept_cyc, long long complete_cyc);
// SV -> C, once per slot at end of run (wire-level, not txn-derivable)
void cmodel_perf_axi_backpressure(const char* slot, long long slave_write_idle_cyc,
                                  long long master_read_idle_cyc, long long outstanding_max);
// SV -> C, once per link at end of run
void cmodel_perf_link(const char* name, long long flit_count, long long stall_cyc);
// C-side, called once per cycle from tb_top: sample router occupancy (max)
void cmodel_perf_sample_tick();
// C-side, called once before cmodel_finalize: write perf.json
void cmodel_perf_dump(const char* path);
```

## PerfCollector API (Task 2; consumed by Task 3)

```cpp
namespace ni::cmodel::cosim {
class PerfCollector {
 public:
    void set_scenario(std::string scenario);
    void set_window(uint64_t start_cyc, uint64_t end_cyc);
    void add_txn(const std::string& slot, uint32_t id, bool is_write, uint64_t addr,
                 uint32_t len, uint32_t size, uint64_t accept_cyc, uint64_t complete_cyc);
    void set_slot_backpressure(const std::string& slot, uint64_t slave_write_idle_cyc,
                               uint64_t master_read_idle_cyc, uint64_t outstanding_max);
    void sample_router(const std::string& name, uint64_t in_occ, uint64_t out_occ);  // tracks max
    void set_link(const std::string& name, uint64_t flit_count, uint64_t stall_cyc);
    std::string to_json() const;
    void dump(const std::string& path) const;
};
}  // namespace ni::cmodel::cosim
```

Derivations in the collector: `bytes = (uint64_t(len)+1) << size`; `dst` node = `nmu::addr_trans::xy_route(addr).dst_id`; slot `role` from the name (`*.manager` / `*.subordinate`); end-to-end latency rows + `by_signature` + `histogram` from **manager** slots; `service_latency` summary from **subordinate** slots.

---

### Task 1: Clean break — delete the two-pass model

**Files:**
- Delete: `c_model/tests/integration/test_perf_probe.cpp`, `c_model/tests/common/perf_report.hpp`, `c_model/tests/common/test_perf_report.cpp`, `docs/superpowers/plans/2026-06-17-perf-probe-rework.md`
- Modify: `Makefile`, `c_model/tests/integration/CMakeLists.txt`, `c_model/tests/common/CMakeLists.txt`

- [ ] **Step 1: Remove the deleted files**

```bash
git rm c_model/tests/integration/test_perf_probe.cpp \
       c_model/tests/common/perf_report.hpp \
       c_model/tests/common/test_perf_report.cpp \
       docs/superpowers/plans/2026-06-17-perf-probe-rework.md
```

- [ ] **Step 2: Remove the `make perf` target and its `.PHONY` entry**

In `Makefile`: delete `perf` from the `.PHONY:` list (line ~20) and delete the whole `perf:` target block plus its `PERF_DIR`/`PERF_ENV`/`PERF_FILTER`/`PERF_CMD` vars and the comment block (lines ~105-123).

- [ ] **Step 3: Remove the CMake registrations**

In `c_model/tests/integration/CMakeLists.txt`: delete the `add_cmodel_test(test_perf_probe)` block (the `add_cmodel_test` line + its `target_link_libraries`/`target_include_directories`/`target_compile_definitions`/`add_custom_command` for `test_perf_probe`, lines ~48-58).

In `c_model/tests/common/CMakeLists.txt`: delete the `add_cmodel_test(test_perf_report)` line and its `target_include_directories(test_perf_report ...)` (lines ~38-40).

- [ ] **Step 4: Build to confirm green (no dangling references)**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3`
Expected: configures + builds with no `test_perf_probe`/`test_perf_report`/`perf_report.hpp` errors.

- [ ] **Step 5: Run the surviving perf-primitive tests**

Run: `cd build/cmodel && ctest -R "perf_stats|ni_perf_observer|flit_link_probe" --output-on-failure`
Expected: all PASS (these neutral primitives survive).

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "test(perf): remove two-pass zero_load harness (clean break)"
```

---

### Task 2: `PerfCollector` (C++) — aggregation + JSON

**Files:**
- Create: `c_model/include/cosim/perf_collector.hpp`
- Test: `c_model/tests/common/test_perf_collector.cpp`
- Modify: `c_model/tests/common/CMakeLists.txt`

**Interfaces:**
- Produces: the `PerfCollector` API above (consumed by Task 3).

- [ ] **Step 1: Register the test**

In `c_model/tests/common/CMakeLists.txt`, after the surviving entries add:
```cmake
add_cmodel_test(test_perf_collector)
target_include_directories(test_perf_collector PRIVATE ${CMAKE_SOURCE_DIR}/include)
```

- [ ] **Step 2: Write the failing test**

`c_model/tests/common/test_perf_collector.cpp`:
```cpp
#include "cosim/perf_collector.hpp"

#include <gtest/gtest.h>

using ni::cmodel::cosim::PerfCollector;

namespace {

// One write (lat 42) + one read (lat 38) at the manager slot; the same two at
// the subordinate slot (service latencies 14/12). Mirrors spec §5.1 example.
PerfCollector make_populated() {
    PerfCollector pc;
    pc.set_scenario("AX4-BAS-003");
    pc.set_window(0, 64);
    // addr 0x100000000 routes to node1 via xy_route (bit32 remap).
    pc.add_txn("node0.manager", 3, true, 0x100000000ull, 7, 3, 10, 52);
    pc.add_txn("node0.manager", 5, false, 0x100000000ull, 7, 3, 12, 50);
    pc.add_txn("node1.subordinate", 3, true, 0x100000000ull, 7, 3, 30, 44);
    pc.add_txn("node1.subordinate", 5, false, 0x100000000ull, 7, 3, 28, 40);
    pc.set_slot_backpressure("node0.manager", 2, 0, 1);
    pc.set_slot_backpressure("node1.subordinate", 3, 1, 1);
    pc.set_link("req_0to1", 4, 1);
    pc.sample_router("req.R(0,0)", 2, 2);
    pc.sample_router("req.R(0,0)", 1, 1);  // max must stay 2/2
    return pc;
}

TEST(PerfCollector, ByteCountFromLenSize) {
    // (len+1)<<size = (7+1)<<3 = 64.
    PerfCollector pc = make_populated();
    const std::string j = pc.to_json();
    EXPECT_NE(j.find("\"write_byte_count\":64"), std::string::npos);
}

TEST(PerfCollector, SignatureMinMeanMax) {
    PerfCollector pc;
    pc.add_txn("node0.manager", 1, true, 0x100000000ull, 7, 3, 0, 40);
    pc.add_txn("node0.manager", 1, true, 0x100000000ull, 7, 3, 0, 60);
    const std::string j = pc.to_json();
    EXPECT_NE(j.find("\"min\":40"), std::string::npos);
    EXPECT_NE(j.find("\"max\":60"), std::string::npos);
    EXPECT_NE(j.find("\"mean\":50"), std::string::npos);
}

TEST(PerfCollector, ServiceLatencyOnSubordinateOnly) {
    const std::string j = make_populated().to_json();
    EXPECT_NE(j.find("\"service_latency\""), std::string::npos);
    // service_latency appears under the subordinate slot, not the manager.
    const std::size_t mgr = j.find("node0.manager");
    const std::size_t sub = j.find("node1.subordinate");
    const std::size_t svc = j.find("\"service_latency\"");
    EXPECT_GT(svc, sub);
    EXPECT_TRUE(svc < mgr || mgr > sub);
}

TEST(PerfCollector, RouterOccupancyTracksMax) {
    const std::string j = make_populated().to_json();
    EXPECT_NE(j.find("\"in_fifo_occ_max\":2"), std::string::npos);
}

TEST(PerfCollector, HistogramBinsByDefaultLadder) {
    const std::string j = make_populated().to_json();
    // 42 and 38 both fall in [32,64): count 2.
    EXPECT_NE(j.find("\"low\":32,\"high\":64,\"count\":2"), std::string::npos);
}

}  // namespace
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3`
Expected: FAIL to compile (`perf_collector.hpp` not found).

- [ ] **Step 4: Implement `perf_collector.hpp`**

`c_model/include/cosim/perf_collector.hpp`:
```cpp
#ifndef NI_CMODEL_COSIM_PERF_COLLECTOR_HPP
#define NI_CMODEL_COSIM_PERF_COLLECTOR_HPP

#include "nmu/addr_trans.hpp"

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace ni::cmodel::cosim {

// Single-run perf readout. SV monitors push per-txn + end-of-run counters;
// C-side sampling pushes router occupancy. dump() writes the spec §5.1 schema.
class PerfCollector {
 public:
    void set_scenario(std::string scenario) { scenario_ = std::move(scenario); }
    void set_window(uint64_t start_cyc, uint64_t end_cyc) {
        win_start_ = start_cyc;
        win_end_ = end_cyc;
    }

    void add_txn(const std::string& slot, uint32_t id, bool is_write, uint64_t addr,
                 uint32_t len, uint32_t size, uint64_t accept_cyc, uint64_t complete_cyc) {
        Slot& s = slot_(slot);
        Txn t;
        t.id = id;
        t.is_write = is_write;
        t.dst = nmu::addr_trans::xy_route(addr).dst_id;
        t.len = len;
        t.size = size;
        t.bytes = (static_cast<uint64_t>(len) + 1) << size;
        t.accept_cyc = accept_cyc;
        t.complete_cyc = complete_cyc;
        t.latency = complete_cyc - accept_cyc;
        s.txns.push_back(t);
    }

    void set_slot_backpressure(const std::string& slot, uint64_t slave_write_idle_cyc,
                               uint64_t master_read_idle_cyc, uint64_t outstanding_max) {
        Slot& s = slot_(slot);
        s.slave_write_idle_cyc = slave_write_idle_cyc;
        s.master_read_idle_cyc = master_read_idle_cyc;
        s.outstanding_max = outstanding_max;
    }

    void sample_router(const std::string& name, uint64_t in_occ, uint64_t out_occ) {
        Router& r = routers_[name];
        if (in_occ > r.in_max) r.in_max = in_occ;
        if (out_occ > r.out_max) r.out_max = out_occ;
    }

    void set_link(const std::string& name, uint64_t flit_count, uint64_t stall_cyc) {
        links_[name] = Link{flit_count, stall_cyc};
    }

    std::string to_json() const {
        std::ostringstream os;
        os << "{\"schema_version\":1,\"scenario\":\"" << scenario_ << "\","
           << "\"window\":{\"start_cyc\":" << win_start_ << ",\"end_cyc\":" << win_end_ << "},";
        emit_slots(os);
        os << ',';
        emit_latency(os);
        os << ',';
        emit_noc(os);
        os << '}';
        return os.str();
    }

    void dump(const std::string& path) const {
        std::ofstream f(path);
        f << to_json() << '\n';
    }

 private:
    struct Txn {
        uint32_t id = 0;
        bool is_write = false;
        uint32_t dst = 0;
        uint32_t len = 0;
        uint32_t size = 0;
        uint64_t bytes = 0;
        uint64_t accept_cyc = 0;
        uint64_t complete_cyc = 0;
        uint64_t latency = 0;
    };
    struct Slot {
        std::vector<Txn> txns;
        uint64_t slave_write_idle_cyc = 0;
        uint64_t master_read_idle_cyc = 0;
        uint64_t outstanding_max = 0;
    };
    struct Router {
        uint64_t in_max = 0;
        uint64_t out_max = 0;
    };
    struct Link {
        uint64_t flit_count = 0;
        uint64_t stall_cyc = 0;
    };

    Slot& slot_(const std::string& name) {
        auto it = slots_.find(name);
        if (it == slots_.end()) it = slots_.emplace(name, Slot{}).first;
        return it->second;
    }
    static bool is_manager(const std::string& name) {
        return name.find("manager") != std::string::npos;
    }

    void emit_slots(std::ostringstream& os) const {
        os << "\"axi_slots\":[";
        bool first = true;
        for (const auto& [name, s] : slots_) {
            if (!first) os << ',';
            first = false;
            uint64_t wt = 0, rt = 0, wb = 0, rb = 0;
            for (const Txn& t : s.txns) {
                if (t.is_write) {
                    ++wt;
                    wb += t.bytes;
                } else {
                    ++rt;
                    rb += t.bytes;
                }
            }
            os << "{\"name\":\"" << name << "\",\"role\":\""
               << (is_manager(name) ? "manager" : "subordinate") << "\","
               << "\"write_txn_count\":" << wt << ",\"read_txn_count\":" << rt << ','
               << "\"write_byte_count\":" << wb << ",\"read_byte_count\":" << rb << ','
               << "\"slave_write_idle_cyc\":" << s.slave_write_idle_cyc
               << ",\"master_read_idle_cyc\":" << s.master_read_idle_cyc
               << ",\"outstanding_max\":" << s.outstanding_max;
            if (!is_manager(name)) {
                os << ",\"service_latency\":{";
                emit_stats(os, "write", s.txns, true);
                os << ',';
                emit_stats(os, "read", s.txns, false);
                os << '}';
            }
            os << '}';
        }
        os << ']';
    }

    static void emit_stats(std::ostringstream& os, const char* key,
                           const std::vector<Txn>& txns, bool is_write) {
        uint64_t mn = 0, mx = 0, sum = 0, n = 0;
        for (const Txn& t : txns) {
            if (t.is_write != is_write) continue;
            if (n == 0 || t.latency < mn) mn = t.latency;
            if (t.latency > mx) mx = t.latency;
            sum += t.latency;
            ++n;
        }
        os << '"' << key << "\":{\"min\":" << mn << ",\"mean\":" << (n ? sum / n : 0)
           << ",\"max\":" << mx << '}';
    }

    void emit_latency(std::ostringstream& os) const {
        os << "\"latency\":{\"measured_at\":\"manager slot -- end-to-end\",\"transactions\":[";
        bool first = true;
        std::vector<Txn> mgr;
        for (const auto& [name, s] : slots_) {
            if (!is_manager(name)) continue;
            for (const Txn& t : s.txns) {
                mgr.push_back(t);
                if (!first) os << ',';
                first = false;
                os << "{\"id\":" << t.id << ",\"dir\":\"" << (t.is_write ? "write" : "read")
                   << "\",\"dst\":\"node" << t.dst << "\",\"accept_cyc\":" << t.accept_cyc
                   << ",\"complete_cyc\":" << t.complete_cyc << ",\"latency\":" << t.latency
                   << ",\"bytes\":" << t.bytes << '}';
            }
        }
        os << "],\"by_signature\":[";
        emit_signatures(os, mgr);
        os << "],\"histogram\":[";
        emit_histogram(os, mgr);
        os << "]}";
    }

    static void emit_signatures(std::ostringstream& os, const std::vector<Txn>& txns) {
        // key = (is_write, len, size, dst)
        std::map<std::tuple<bool, uint32_t, uint32_t, uint32_t>, std::vector<uint64_t>> g;
        for (const Txn& t : txns) g[{t.is_write, t.len, t.size, t.dst}].push_back(t.latency);
        bool first = true;
        for (const auto& [k, lats] : g) {
            if (!first) os << ',';
            first = false;
            uint64_t mn = lats[0], mx = lats[0], sum = 0;
            for (uint64_t l : lats) {
                if (l < mn) mn = l;
                if (l > mx) mx = l;
                sum += l;
            }
            os << "{\"op\":\"" << (std::get<0>(k) ? "write" : "read") << "\",\"len\":"
               << std::get<1>(k) << ",\"size\":" << std::get<2>(k) << ",\"dst\":\"node"
               << std::get<3>(k) << "\",\"count\":" << lats.size() << ",\"min\":" << mn
               << ",\"mean\":" << (sum / lats.size()) << ",\"max\":" << mx << '}';
        }
    }

    void emit_histogram(std::ostringstream& os, const std::vector<Txn>& txns) const {
        // default ladder; last bin open-ended.
        static const uint64_t edges[] = {0, 16, 32, 64, 128, 256};
        const std::size_t n = sizeof(edges) / sizeof(edges[0]);
        for (std::size_t i = 0; i < n; ++i) {
            const uint64_t lo = edges[i];
            const uint64_t hi = (i + 1 < n) ? edges[i + 1] : 0;  // 0 = open
            uint64_t c = 0;
            for (const Txn& t : txns)
                if (t.latency >= lo && (hi == 0 || t.latency < hi)) ++c;
            if (i) os << ',';
            os << "{\"low\":" << lo << ",\"high\":" << hi << ",\"count\":" << c << '}';
        }
    }

    void emit_noc(std::ostringstream& os) const {
        os << "\"noc\":{\"routers\":[";
        bool first = true;
        for (const auto& [name, r] : routers_) {
            if (!first) os << ',';
            first = false;
            os << "{\"name\":\"" << name << "\",\"in_fifo_occ_max\":" << r.in_max
               << ",\"out_fifo_occ_max\":" << r.out_max << '}';
        }
        os << "],\"links\":[";
        first = true;
        for (const auto& [name, l] : links_) {
            if (!first) os << ',';
            first = false;
            os << "{\"name\":\"" << name << "\",\"flit_count\":" << l.flit_count
               << ",\"stall_cyc\":" << l.stall_cyc << '}';
        }
        os << "]}";
    }

    std::string scenario_;
    uint64_t win_start_ = 0, win_end_ = 0;
    std::map<std::string, Slot> slots_;
    std::map<std::string, Router> routers_;
    std::map<std::string, Link> links_;
};

}  // namespace ni::cmodel::cosim

#endif  // NI_CMODEL_COSIM_PERF_COLLECTOR_HPP
```
Add `#include <fstream>` at the top (used by `dump`).

- [ ] **Step 5: Run the test to verify it passes**

Run: `make build-cmodel PYTHON3=/c/msys64/mingw64/bin/python3 && cd build/cmodel && ctest -R perf_collector --output-on-failure`
Expected: all 5 PASS.

- [ ] **Step 6: clang-format + commit**

```bash
clang-format -i c_model/include/cosim/perf_collector.hpp c_model/tests/common/test_perf_collector.cpp
git add -A
git commit -m "feat(perf): add PerfCollector aggregation + perf.json writer"
```

---

### Task 3: DPI perf functions + shell-adapter getters

**Files:**
- Modify: `cosim/c/cmodel_dpi.h`, `cosim/c/cmodel_dpi.cpp`
- Modify: `c_model/include/cosim/router_shell_adapter.hpp`, `nsu_shell_adapter.hpp`, `nmu_shell_adapter.hpp`

**Interfaces:**
- Consumes: `PerfCollector` (Task 2); the DPI ABI above.
- Produces: the five `cmodel_perf_*` C functions (consumed by Tasks 4-6); `RouterShellAdapter::rsp_router()`, `NsuShellAdapter::axi_master_port()`, `NmuShellAdapter::rob()`.

- [ ] **Step 1: Add the shell-adapter getters**

`router_shell_adapter.hpp` — after the existing `req_router()` (line ~145):
```cpp
noc::Router& rsp_router() { return *rsp_router_; }
```
`nsu_shell_adapter.hpp` — in the public section:
```cpp
nsu::AxiMasterPort& axi_master_port() { return nsu_->axi_master_port(); }
```
`nmu_shell_adapter.hpp` — in the public section:
```cpp
const nmu::Rob& rob() const { return nmu_->rob(); }
```

- [ ] **Step 2: Declare the DPI functions**

`cmodel_dpi.h`, inside the `extern "C"` block (after `cmodel_reads_checked`):
```c
void cmodel_perf_axi_txn(const char* slot, int id, int is_write,
                         long long addr, int len, int size,
                         long long accept_cyc, long long complete_cyc);
void cmodel_perf_axi_backpressure(const char* slot, long long slave_write_idle_cyc,
                                  long long master_read_idle_cyc, long long outstanding_max);
void cmodel_perf_link(const char* name, long long flit_count, long long stall_cyc);
void cmodel_perf_sample_tick(void);
void cmodel_perf_dump(const char* path);
```

- [ ] **Step 3: Define them in `cmodel_dpi.cpp`**

Add the include near the others: `#include "cosim/perf_collector.hpp"`.
Add a file-scope global next to `g_scoreboard` (~line 117):
```cpp
static ni::cmodel::cosim::PerfCollector g_perf;
```
Reset it in `cmodel_init` (alongside the scoreboard reset, ~line 135): `g_perf = ni::cmodel::cosim::PerfCollector{};`.
Add the definitions (after `cmodel_reads_checked`):
```cpp
extern "C" void cmodel_perf_axi_txn(const char* slot, int id, int is_write,
                                    long long addr, int len, int size,
                                    long long accept_cyc, long long complete_cyc) {
    g_perf.add_txn(slot, static_cast<uint32_t>(id), is_write != 0,
                   static_cast<uint64_t>(addr), static_cast<uint32_t>(len),
                   static_cast<uint32_t>(size), static_cast<uint64_t>(accept_cyc),
                   static_cast<uint64_t>(complete_cyc));
}

extern "C" void cmodel_perf_axi_backpressure(const char* slot, long long swi,
                                             long long mri, long long om) {
    g_perf.set_slot_backpressure(slot, static_cast<uint64_t>(swi),
                                 static_cast<uint64_t>(mri), static_cast<uint64_t>(om));
}

extern "C" void cmodel_perf_link(const char* name, long long flit, long long stall) {
    g_perf.set_link(name, static_cast<uint64_t>(flit), static_cast<uint64_t>(stall));
}

extern "C" void cmodel_perf_sample_tick() {
    using namespace ni::cmodel::cosim;
    for (HandleBlock* h : g_handle_registry) {
        if (h->type != ShellType::Router) continue;
        auto* r = static_cast<RouterShellAdapter*>(h->adapter.get());
        sample_one_router(r->name(), r->req_router(), "req");
        sample_one_router(r->name(), r->rsp_router(), "rsp");
    }
}

extern "C" void cmodel_perf_dump(const char* path) { g_perf.dump(path); }
```
Add the helper above the functions (sums occupancy across ports/VCs, reports max):
```cpp
namespace {
void sample_one_router(const std::string& node, ni::cmodel::noc::Router& r,
                       const char* plane) {
    using ni::cmodel::noc::ROUTER_PORT_COUNT;
    std::size_t in_occ = 0, out_occ = 0;
    for (std::size_t p = 0; p < ROUTER_PORT_COUNT; ++p) {
        out_occ += r.output_fifo_size(p);
        for (uint8_t vc = 0; vc < r.num_vc(); ++vc) in_occ += r.input_fifo_size(p, vc);
    }
    g_perf.sample_router(std::string(plane) + "." + node, in_occ, out_occ);
}
}  // namespace
```
(`RouterShellAdapter` exposes its instance name via `HandleBlock::name`; if no `name()` getter exists, pass `h->name` from the loop instead.)

- [ ] **Step 4: Build the co-sim**

Run: `make build-verilator PYTHON3=/c/msys64/mingw64/bin/python3`
Expected: compiles (DPI symbols defined; getters resolve).

- [ ] **Step 5: Commit**

```bash
clang-format -i cosim/c/cmodel_dpi.cpp c_model/include/cosim/*_shell_adapter.hpp
git add -A
git commit -m "feat(perf): add cmodel_perf DPI + shell-adapter occupancy getters"
```

---

### Task 4: `axi_perf_monitor.sv` — passive AXI slot monitor

**Files:**
- Create: `cosim/sv/axi_perf_monitor.sv`
- Modify: `cosim/sources.mk`

**Interfaces:**
- Consumes: `cmodel_perf_axi_txn`, `cmodel_perf_axi_backpressure` (Task 3).
- Produces: module `axi_perf_monitor` (instantiated in Task 6).

- [ ] **Step 1: Write the monitor**

`cosim/sv/axi_perf_monitor.sv`:
```systemverilog
// Passive AXI slot monitor (PG037-style). Reads one AXI interface's wires;
// correlates latency by per-(id,dir) in-order FIFO; counts idle/outstanding;
// reports each completion + end-of-run backpressure via DPI. No drives.
module axi_perf_monitor #(
    parameter string SLOT_NAME = "slot",
    parameter int    ID_W = 8,
    parameter int    MAX_OUTSTANDING = 64
) (
    input logic clk_i,
    input logic rst_ni,
    input logic                awvalid, input logic awready,
    input logic [ID_W-1:0]     awid,
    input logic [63:0]         awaddr,
    input logic [7:0]          awlen,  input logic [2:0] awsize,
    input logic                wvalid, input logic wready,
    input logic                bvalid, input logic bready, input logic [ID_W-1:0] bid,
    input logic                arvalid, input logic arready,
    input logic [ID_W-1:0]     arid,
    input logic [63:0]         araddr,
    input logic [7:0]          arlen,  input logic [2:0] arsize,
    input logic                rvalid, input logic rready, input logic rlast,
    input logic [ID_W-1:0]     rid
);
    import "DPI-C" context function void cmodel_perf_axi_txn(
        input string slot, input int id, input int is_write,
        input longint addr, input int len, input int size,
        input longint accept_cyc, input longint complete_cyc);
    import "DPI-C" context function void cmodel_perf_axi_backpressure(
        input string slot, input longint slave_write_idle_cyc,
        input longint master_read_idle_cyc, input longint outstanding_max);

    localparam int NID = 1 << ID_W;
    longint cyc;
    longint slave_write_idle, master_read_idle, outstanding_max;
    int     outstanding;
    // per-id in-order issue queues (separate write/read), holding accept cycle +
    // the addr/len/size needed to reconstruct the completion record.
    longint w_acc [NID][$]; longint w_addr [NID][$]; int w_len [NID][$]; int w_sz [NID][$];
    longint r_acc [NID][$]; longint r_addr [NID][$]; int r_len [NID][$]; int r_sz [NID][$];

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            cyc <= 0; slave_write_idle <= 0; master_read_idle <= 0;
            outstanding_max <= 0; outstanding <= 0;
        end else begin
            cyc <= cyc + 1;
            if (wvalid && !wready) slave_write_idle  <= slave_write_idle  + 1;
            if (rvalid && !rready) master_read_idle   <= master_read_idle  + 1;

            if (awvalid && awready) begin
                if (w_acc[awid].size() >= MAX_OUTSTANDING)
                    $fatal(1, "%s: write id=%0d exceeds MAX_OUTSTANDING", SLOT_NAME, awid);
                w_acc[awid].push_back(cyc);  w_addr[awid].push_back(awaddr);
                w_len[awid].push_back(int'(awlen)); w_sz[awid].push_back(int'(awsize));
                outstanding++;
            end
            if (arvalid && arready) begin
                if (r_acc[arid].size() >= MAX_OUTSTANDING)
                    $fatal(1, "%s: read id=%0d exceeds MAX_OUTSTANDING", SLOT_NAME, arid);
                r_acc[arid].push_back(cyc);  r_addr[arid].push_back(araddr);
                r_len[arid].push_back(int'(arlen)); r_sz[arid].push_back(int'(arsize));
                outstanding++;
            end
            if (outstanding > outstanding_max) outstanding_max <= outstanding;

            if (bvalid && bready) begin
                if (w_acc[bid].size() == 0)
                    $fatal(1, "%s: B with no outstanding write id=%0d", SLOT_NAME, bid);
                cmodel_perf_axi_txn(SLOT_NAME, bid, 1, w_addr[bid][0],
                                    w_len[bid][0], w_sz[bid][0], w_acc[bid][0], cyc);
                void'(w_acc[bid].pop_front());  void'(w_addr[bid].pop_front());
                void'(w_len[bid].pop_front());  void'(w_sz[bid].pop_front());
                outstanding--;
            end
            if (rvalid && rready && rlast) begin
                if (r_acc[rid].size() == 0)
                    $fatal(1, "%s: R(last) with no outstanding read id=%0d", SLOT_NAME, rid);
                cmodel_perf_axi_txn(SLOT_NAME, rid, 0, r_addr[rid][0],
                                    r_len[rid][0], r_sz[rid][0], r_acc[rid][0], cyc);
                void'(r_acc[rid].pop_front());  void'(r_addr[rid].pop_front());
                void'(r_len[rid].pop_front());  void'(r_sz[rid].pop_front());
                outstanding--;
            end
        end
    end

    final cmodel_perf_axi_backpressure(SLOT_NAME, slave_write_idle,
                                       master_read_idle, outstanding_max);
endmodule
```

- [ ] **Step 2: Register it in the SV source list**

`cosim/sources.mk`, in `TB_TOP_SV_SRC` before `tb_top.sv`:
```make
    $(COSIM_ROOT)/sv/axi_perf_monitor.sv \
```

- [ ] **Step 3: Build (module elaborates standalone, not yet instantiated)**

Run: `make build-verilator PYTHON3=/c/msys64/mingw64/bin/python3`
Expected: compiles (Verilator parses the new module; unused until Task 6).

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat(perf): add passive axi_perf_monitor.sv (per-id latency FIFO)"
```

---

### Task 5: `flit_link_perf_monitor.sv` — link flit + credit-stall

**Files:**
- Create: `cosim/sv/flit_link_perf_monitor.sv`
- Modify: `cosim/sources.mk`

**Interfaces:**
- Consumes: `cmodel_perf_link` (Task 3).
- Produces: module `flit_link_perf_monitor` (instantiated in Task 6).

- [ ] **Step 1: Write the monitor**

`cosim/sv/flit_link_perf_monitor.sv`:
```systemverilog
// Passive inter-router link monitor. Counts flits (valid high) and credit-deficit
// stall cycles. Credit is a single-cycle pulse; valid is gated on credit upstream,
// so backpressure is observable only as credit_count==0 (downstream buffer full).
module flit_link_perf_monitor #(
    parameter string LINK_NAME = "link",
    parameter int    BUFFER_DEPTH = 4
) (
    input logic clk_i,
    input logic rst_ni,
    input logic valid,         // a flit is on the wire this cycle
    input logic credit_pulse   // a downstream slot freed this cycle
);
    import "DPI-C" context function void cmodel_perf_link(
        input string name, input longint flit_count, input longint stall_cyc);

    longint flit_count, stall_cyc;
    int     credit;

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            flit_count <= 0; stall_cyc <= 0; credit <= BUFFER_DEPTH;
        end else begin
            // credit accounting: -1 on flit sent, +1 on pulse (net 0 if both).
            if (valid) flit_count <= flit_count + 1;
            credit <= credit - (valid ? 1 : 0) + (credit_pulse ? 1 : 0);
            if (credit == 0) stall_cyc <= stall_cyc + 1;  // downstream buffer full
        end
    end

    final cmodel_perf_link(LINK_NAME, flit_count, stall_cyc);
endmodule
```

- [ ] **Step 2: Register it**

`cosim/sources.mk`, in `TB_TOP_SV_SRC` before `tb_top.sv`:
```make
    $(COSIM_ROOT)/sv/flit_link_perf_monitor.sv \
```

- [ ] **Step 3: Build**

Run: `make build-verilator PYTHON3=/c/msys64/mingw64/bin/python3`
Expected: compiles.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat(perf): add passive flit_link_perf_monitor.sv (credit-deficit stall)"
```

---

### Task 6: Wire monitors into `tb_top.sv` + emit `perf.json`

**Files:**
- Modify: `cosim/sv/tb_top.sv`, `cosim/verilator/main.cpp`, `cosim/verilator/Makefile`

**Interfaces:**
- Consumes: `axi_perf_monitor`, `flit_link_perf_monitor`, `cmodel_perf_sample_tick`, `cmodel_perf_dump`.

- [ ] **Step 1: Import the sample/dump DPI in tb_top**

In the DPI import block (`tb_top.sv:47-60`) add:
```systemverilog
    import "DPI-C" context function void cmodel_perf_sample_tick();
    import "DPI-C" context function void cmodel_perf_dump(input string path);
```

- [ ] **Step 2: Instantiate one `axi_perf_monitor` per AXI edge (4 total)**

After the wrap instances, add (showing node0; repeat for node1 with `_1` instances + `node1.*` names):
```systemverilog
    axi_perf_monitor #(.SLOT_NAME("node0.manager"),
                       .ID_W($bits(master_nmu_axi_0.awid))) u_perf_mgr_0 (
        .clk_i, .rst_ni,
        .awvalid(master_nmu_axi_0.awvalid), .awready(master_nmu_axi_0.awready),
        .awid(master_nmu_axi_0.awid), .awaddr(master_nmu_axi_0.awaddr),
        .awlen(master_nmu_axi_0.awlen), .awsize(master_nmu_axi_0.awsize),
        .wvalid(master_nmu_axi_0.wvalid), .wready(master_nmu_axi_0.wready),
        .bvalid(master_nmu_axi_0.bvalid), .bready(master_nmu_axi_0.bready),
        .bid(master_nmu_axi_0.bid),
        .arvalid(master_nmu_axi_0.arvalid), .arready(master_nmu_axi_0.arready),
        .arid(master_nmu_axi_0.arid), .araddr(master_nmu_axi_0.araddr),
        .arlen(master_nmu_axi_0.arlen), .arsize(master_nmu_axi_0.arsize),
        .rvalid(master_nmu_axi_0.rvalid), .rready(master_nmu_axi_0.rready),
        .rlast(master_nmu_axi_0.rlast), .rid(master_nmu_axi_0.rid));

    axi_perf_monitor #(.SLOT_NAME("node1.subordinate"),
                       .ID_W($bits(nsu_slave_axi_1.awid))) u_perf_sub_1 (
        .clk_i, .rst_ni,
        .awvalid(nsu_slave_axi_1.awvalid), .awready(nsu_slave_axi_1.awready),
        .awid(nsu_slave_axi_1.awid), .awaddr(nsu_slave_axi_1.awaddr),
        .awlen(nsu_slave_axi_1.awlen), .awsize(nsu_slave_axi_1.awsize),
        .wvalid(nsu_slave_axi_1.wvalid), .wready(nsu_slave_axi_1.wready),
        .bvalid(nsu_slave_axi_1.bvalid), .bready(nsu_slave_axi_1.bready),
        .bid(nsu_slave_axi_1.bid),
        .arvalid(nsu_slave_axi_1.arvalid), .arready(nsu_slave_axi_1.arready),
        .arid(nsu_slave_axi_1.arid), .araddr(nsu_slave_axi_1.araddr),
        .arlen(nsu_slave_axi_1.arlen), .arsize(nsu_slave_axi_1.arsize),
        .rvalid(nsu_slave_axi_1.rvalid), .rready(nsu_slave_axi_1.rready),
        .rlast(nsu_slave_axi_1.rlast), .rid(nsu_slave_axi_1.rid));
```
Also add `u_perf_sub_0` (`nsu_slave_axi_0`, `node0.subordinate`) and `u_perf_mgr_1` (`master_nmu_axi_1`, `node1.manager`) by the same pattern.

- [ ] **Step 3: Instantiate the link monitors (req + rsp, both directions)**

```systemverilog
    flit_link_perf_monitor #(.LINK_NAME("req_0to1"), .BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH))
        u_perf_link_req01 (.clk_i, .rst_ni,
            .valid(link_req_0to1_valid), .credit_pulse(link_req_0to1_credit));
    flit_link_perf_monitor #(.LINK_NAME("rsp_1to0"), .BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH))
        u_perf_link_rsp10 (.clk_i, .rst_ni,
            .valid(link_rsp_1to0_valid), .credit_pulse(link_rsp_1to0_credit));
```
Add `req_1to0` and `rsp_0to1` by the same pattern.

- [ ] **Step 4: Sample occupancy each cycle**

In the per-cycle `always` block that already polls `cmodel_done()` (the exit block, `tb_top.sv:252`), add at the top of the cycle action:
```systemverilog
        cmodel_perf_sample_tick();
```

- [ ] **Step 5: Dump before `$finish(0)`**

Just before the success `$finish(0)` (`tb_top.sv:260`):
```systemverilog
        cmodel_perf_dump(perf_out_path);
```
Declare `perf_out_path` and parse it from a plusarg in the `initial` block (next to the scenario plusargs):
```systemverilog
    string perf_out_path = "perf.json";
    initial begin
        if (!$value$plusargs("perf_out=%s", perf_out_path)) perf_out_path = "perf.json";
    end
```

- [ ] **Step 6: Pass `+perf_out=` from the Makefile**

In `cosim/verilator/Makefile` `run-tb-top`, add to the `$(TBTOP_EXE)` invocation args:
```make
	    "+perf_out=output/$(SCENARIO)/perf.json" \
```

- [ ] **Step 7: Build + run**

```bash
make build-verilator PYTHON3=/c/msys64/mingw64/bin/python3
cd cosim/verilator && make run-tb-top SCENARIO=AX4-BAS-003_single_write_read_aligned
```
Expected: run completes; `output/AX4-BAS-003_single_write_read_aligned/perf.json` exists and parses.

- [ ] **Step 8: Inspect the JSON**

Run: `cat output/AX4-BAS-003_single_write_read_aligned/perf.json | py -3 -m json.tool` (or `python3`).
Expected: top-level keys `schema_version, scenario, window, axi_slots, latency, noc`; `axi_slots` has 4 entries; `latency.transactions` non-empty.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "feat(perf): instantiate PMU monitors in tb_top + emit perf.json"
```

---

### Task 7: Validation matrix + CLI summary + docs

**Files:**
- Create: `cosim/verilator/perf_cli_summary.py` (stdout pretty-printer reading perf.json)
- Modify: `cosim/verilator/Makefile` (print the summary after the run), `docs/performance-probe.md` (rewrite)

**Interfaces:**
- Consumes: `perf.json` (Task 6).

- [ ] **Step 1: Non-intrusive A/B check**

Run the scenario once with the monitors built in (current) and capture the per-transaction completion cycles from the existing scoreboard dump / `run.log`. Then build with the monitors disabled (comment out the four `u_perf_*` + two link instances + the `sample_tick`/`dump` calls) and re-run.
Run: `cd cosim/verilator && diff <(grep -E 'complete|scoreboard' output/<id>/run.log) <prior>`
Expected: identical scoreboard result and completion cycles (monitors are passive). If they differ, a monitor is driving a wire — fix before continuing. Restore the instances after.

- [ ] **Step 2: Same-id correlation check**

Pick a scenario with ≥2 outstanding same-id transactions (or add one under `sim/test_patterns/`); run it; confirm no `$fatal` ("no outstanding ... id") fired and each completion's latency is non-negative and monotonic with issue order.
Run: `cd cosim/verilator && make run-tb-top SCENARIO=<multi-outstanding-id> && grep -c fatal output/<id>/run.log`
Expected: `0`.

- [ ] **Step 3: Credit-stall sanity**

Confirm in `run.log` (add a `$display` of `credit`/`stall_cyc` at `final`, or read `perf.json`): the link monitor's `stall_cyc` is finite and the credit counter never went negative (no Verilator range warning). On a back-pressured scenario `stall_cyc > 0`.
Expected: credit stays in `[0, BUFFER_DEPTH]`; `stall_cyc` plausible.

- [ ] **Step 4: Write the CLI summary printer**

`cosim/verilator/perf_cli_summary.py` — read `perf.json` (argv[1]) and print the spec §5.1 "CLI summary (stdout)" layout (axi_slots table, latency by_signature + histogram + slave service, NoC tables). Plain `json.load` + formatted prints; no raw `transactions[]` (JSON-only).

- [ ] **Step 5: Print the summary after the run**

In `cosim/verilator/Makefile` `run-tb-top`, after the `tail -8 ... run.log` line:
```make
	@$(PYTHON3) perf_cli_summary.py output/$(SCENARIO)/perf.json || true
```

- [ ] **Step 6: Run and eyeball the summary**

Run: `cd cosim/verilator && make run-tb-top SCENARIO=AX4-BAS-003_single_write_read_aligned`
Expected: the `[perf]` summary prints, matching the JSON numbers.

- [ ] **Step 7: Rewrite `docs/performance-probe.md`**

Replace the two-pass content with the single-run in-fabric PMU: how to run (`make run-tb-top SCENARIO=<id>`), the perf.json schema (link to spec §5.1), the CLI summary, the latency definitions (start/end events, B-response divergence note), the credit-stall definition, and the min-observed-not-zero_load framing. Terse; tables over prose.

- [ ] **Step 8: Full co-sim regression**

Run: `cd cosim/verilator && make run-tb-top SCENARIO=AX4-BAS-003_single_write_read_aligned` and any existing co-sim ctest target.
Expected: pass; perf.json emitted; no scoreboard regressions.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "feat(perf): perf CLI summary, validation, docs rewrite"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| §3 PG037 per-slot metrics (byte/txn/latency/idle/outstanding) | T4 (count) + T2 (aggregate) |
| §3 latency start/end (B-response), per-(id,dir) FIFO | T4 |
| §3 credit-deficit link stall | T5 |
| §3 router/NI occupancy (C sampling) | T3 (`sample_tick` + getters) |
| §5 min observed, two grains | T2 (`by_signature` + `transactions`) |
| §5.1 perf.json schema | T2 (`to_json`) |
| §5.1 CLI summary | T7 |
| §6 integration points (DPI, shells, tb_top, Makefile) | T3, T6 |
| §7/§8 clean break (delete two-pass + orphans) | T1 |
| §8.1 validation matrix | T7 (+ T4 `$fatal` for FIFO under/overflow) |

**Placeholder scan:** none — every code step has complete code; commands have expected output.

**Type consistency:** DPI ABI (`cmodel_perf_axi_txn(slot,id,is_write,addr,len,size,accept,complete)`) matches the SV import (T4) and the C definition (T3) and `PerfCollector::add_txn` (T2). `rsp_router()`/`axi_master_port()`/`rob()` getters defined in T3 match their use in T3's `sample_one_router`. Link monitor `(valid, credit_pulse)` ports match the T6 instantiation against `link_*_valid`/`link_*_credit`.

**Open items for the implementer to confirm (report if blocked, do not work around):**
- `RouterShellAdapter` instance name source for `sample_one_router` — use `h->name` if no `name()` getter.
- `nmu::Rob` occupancy is Disabled-mode outstanding (not the 32-entry pool); NI occupancy reporting via the RoB getter is optional/secondary — wire it only if it lands cleanly, else leave routers-only NoC occupancy and note it.
- Verilator support for unpacked-array-of-queues (`longint q[NID][$]`); if unsupported on the pinned version, switch the per-id store to an associative array keyed by id.
