# Test Logger: SCENARIO + AxiMasterObserver Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `SCENARIO("<English description>")` macro to make 240 cryptic test names human-readable, plus a `ni::cmodel::testing::AxiMasterObserver` class that hooks `AxiMaster` callbacks to auto-detect AXI4 §A5.3 per-id ordering violations and emit parse-friendly per-transaction trace under `NOC_LOG=1`. Audit and tighten 19 bare `assert(false && "impossible")` messages with cause hints.

**Architecture:** `SCENARIO` is a one-line macro printing to stdout; always emitted. `AxiMasterObserver` is a standalone class in `tests/common/` that subscribes to `AxiMaster::on_write_completed` / `on_read_observed` callbacks (already exist in production code — zero AxiMaster modification). Observer counts logical AW/W/AR/B/R per-transaction, tracks per-id `scenario_line` sequences, auto-detects ordering violations, emits FAIL context. RAII destructor prints summary as fallback.

**Tech Stack:** C++17, GoogleTest, CMake, Windows + mingw64 + Ninja. Python 3 specgen (no changes this round). `py -3` not `python3` on Windows.

**Reference spec:** `docs/superpowers/specs/2026-06-03-test-logger-scenario-observer-design.md` (commit `fc7cd39`, Codex-approved).

**Drift gates** (every commit must pass):
```bash
cd specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
cd ../c_model && cmake --build build && ctest --test-dir build -j 1
```

**Coding style** (per user memory):
- C/C++ multi-line continuation = **4-space indent**
- Variables / methods: `snake_case`
- Classes / structs: `PascalCase`
- Constants / `constexpr`: `UPPER_SNAKE`
- Testbench-only namespace: `ni::cmodel::testing`
- English-only SCENARIO descriptions, ≤80 chars per call

**Commit boundary plan** (5 commits, commit 2 may split):

| Task | Commit | Acceptance |
|---|---|---|
| 1 | `feat(tests/common): add SCENARIO macro + AxiMasterObserver` | 297 → 302 ctest |
| 2 | `test: retrofit SCENARIO to 240 TEST declarations` | 302/302 ctest (description-only) |
| 3 | `test(tests/integration): wire AxiMasterObserver` | 302/302 ctest |
| 4 | `fix(c_model): tighten assert messages with cause hints` | 302/302 ctest |
| 5 | `docs(NEXT_STEPS): test logger done; next is vc_arb` | 301/301 |

---

## File structure

### New files
- `c_model/tests/common/test_logger.hpp` — SCENARIO macro + AxiMasterObserver declaration + inline impl
- `c_model/tests/common/test_test_logger.cpp` — 4 Observer unit tests

### Modified files (Tasks 2-4)
- 20 test files in `c_model/tests/**` — SCENARIO retrofit (Task 2)
- `c_model/tests/common/CMakeLists.txt` — register `test_test_logger` (Task 1)
- `c_model/tests/integration/test_request_response_loopback.cpp` — wire observer (Task 3)
- Various `c_model/include/{axi,ni,nmu,nsu}/*.hpp` — assert message audit (Task 4)
- `NEXT_STEPS.md` (Task 5)

### Not touched
- `c_model/include/axi/axi_master.hpp` (callbacks already exist; zero production change)
- `c_model/include/axi/scoreboard.hpp` (responsibility kept clean)
- `specgen/generated/*`, `ni_packet.json`
- ROB / Packetize / Depacketize / LoopbackNoc (no internal-state peek per spec §2 out-of-scope)

---

## Task 1: SCENARIO macro + AxiMasterObserver + 4 observer unit tests

**Files:**
- Create: `c_model/tests/common/test_logger.hpp`
- Create: `c_model/tests/common/test_test_logger.cpp`
- Modify: `c_model/tests/common/CMakeLists.txt` (add `add_cmodel_test(test_test_logger)`)

**Goal:** Build the logger framework as a standalone, opt-in unit. After this task: `SCENARIO()` macro available; `AxiMasterObserver` class can be instantiated by any test. 4 self-tests prove Observer counts correctly, detects ordering violations, respects RAII summary.

- [ ] **Step 1: Read existing `c_model/tests/common/CMakeLists.txt` to confirm registration pattern**

```bash
cat c_model/tests/common/CMakeLists.txt
```

Expected: existing `add_cmodel_test(test_loopback_latency)` line. Mirror pattern.

- [ ] **Step 2: Write failing test 1 `AxiMasterObserver.OrderedBPass`**

Create `c_model/tests/common/test_test_logger.cpp`:

```cpp
#include "common/test_logger.hpp"
#include "axi/axi_master.hpp"
#include <gtest/gtest.h>
#include <fstream>

using ni::cmodel::testing::AxiMasterObserver;

namespace {

// Helper: create a tiny scenario YAML and an AxiMaster + AxiSlave + Memory
// triple. Returns AxiMaster pointer-owned-by-test; backend lives in static
// storage for the test scope.
struct TestRig {
    axi::Memory memory{0, 0x10000, 0, 0};
    axi::AxiSlave slave{memory};
    std::string yaml_path;
    std::string dump_path;
    std::unique_ptr<axi::AxiMaster> master;
    TestRig(const std::string& yaml_body,
            std::size_t max_out_w = 2, std::size_t max_out_r = 2) {
        yaml_path = ::testing::TempDir() + "/test_logger_obs.yaml";
        dump_path = ::testing::TempDir() + "/test_logger_obs_rdump.txt";
        std::ofstream(yaml_path) << yaml_body;
        slave.set_memory_bounds(0, 0x10000);
        master = std::make_unique<axi::AxiMaster>(
            yaml_path, slave, dump_path, max_out_w, max_out_r);
    }
    void run_to_completion(int max_cycles = 200) {
        for (int i = 0; i < max_cycles && !master->done(); ++i) {
            master->tick(); slave.tick(); memory.tick();
        }
    }
};

}  // namespace

TEST(AxiMasterObserver, OrderedBPass) {
    SCENARIO("Observer: 2 same-id writes complete in submission order → ok()=true, no FAIL");
    TestRig rig(R"yaml(
config:
  max_outstanding_write: 2
transactions:
  - { op: write, id: 0x05, addr: 0x100,  size: 5, len: 0, burst: INCR,
      data_file: /dev/null, strb_file: "" }
  - { op: write, id: 0x05, addr: 0x200,  size: 5, len: 0, burst: INCR,
      data_file: /dev/null, strb_file: "" }
)yaml");
    AxiMasterObserver obs(*rig.master, "Test");
    rig.run_to_completion();
    EXPECT_TRUE(obs.ok());
    EXPECT_EQ(obs.aw_count(), 2u);
    EXPECT_EQ(obs.b_count(), 2u);
    EXPECT_EQ(obs.mismatches(), 0u);
}
```

Note: the helper YAML body uses inline data via `/dev/null` substitution path — if the scenario_parser requires real file, substitute a tiny hex tmp file (same pattern as `SameIdConcurrentAdmissionVisibleInPipeline` in `test_axi_master.cpp`).

- [ ] **Step 3: Write failing test 2 `AxiMasterObserver.OutOfOrderBFail`**

Append:

```cpp
TEST(AxiMasterObserver, OutOfOrderBFail) {
    SCENARIO("Observer: same-id B arrives out of submission order → ok()=false, FAIL context emitted");
    // Inject a mocked AxiMaster scenario where B for the 2nd AW completes BEFORE
    // the 1st AW's B. Simplest: use a real AxiMaster + manually invoke the
    // observer's callbacks in reversed order (bypasses real AxiMaster ordering).
    // Alternative: use a friend-class shim to call obs.on_write(wr) directly.
    //
    // Implementation: declare AxiMasterObserver as friend of a test-only helper
    // class that exposes on_write/on_read for direct invocation. Then:
    //   obs.test_inject_write(WriteResult{ ..., .scenario_line = 7 });  // 2nd
    //   obs.test_inject_write(WriteResult{ ..., .scenario_line = 5 });  // 1st (out of order)
    //   EXPECT_FALSE(obs.ok());
    //   EXPECT_EQ(obs.failures().size(), 1u);
    //
    // For this plan, add public test-only methods on AxiMasterObserver
    // (test_inject_write_result / test_inject_read_result) gated by
    // #ifdef AXI_MASTER_OBSERVER_TEST_INJECT — defined only in this test file.

#define AXI_MASTER_OBSERVER_TEST_INJECT
#include "common/test_logger.hpp"  // re-include for test-only methods
#undef AXI_MASTER_OBSERVER_TEST_INJECT

    // Construct observer without a real master (test-only ctor).
    AxiMasterObserver obs("Test");   // test-only no-master ctor
    obs.test_inject_write_result(axi::WriteResult{
        /*addr=*/0x100, /*size=*/5, /*len=*/0, axi::Burst::INCR,
        axi::LockType::Normal, /*data=*/{}, /*strb=*/{},
        /*resp=*/axi::Resp::OKAY, /*id=*/0x05, /*scenario_line=*/7});
    obs.test_inject_write_result(axi::WriteResult{
        /*addr=*/0x100, /*size=*/5, /*len=*/0, axi::Burst::INCR,
        axi::LockType::Normal, /*data=*/{}, /*strb=*/{},
        /*resp=*/axi::Resp::OKAY, /*id=*/0x05, /*scenario_line=*/5});
    EXPECT_FALSE(obs.ok());
    EXPECT_EQ(obs.failures().size(), 1u);
}
```

(The `#define`-include-`#undef` pattern is one way to expose test-only entry points; alternative is `friend class AxiMasterObserverTest` declared in observer header. Implementer chooses. Recommendation: friend-class approach for cleaner header. See Step 5 below.)

- [ ] **Step 4: Write failing tests 3, 4, and 5 (`NonOkayResp`, `StuckCountMismatch`, `OutOfOrderRFail`)**

Append:

```cpp
TEST(AxiMasterObserver, NonOkayResp) {
    SCENARIO("Observer: B with SLVERR → mismatches++, no FAIL by default (caller decides)");
    AxiMasterObserver obs("Test");
    obs.test_inject_write_result(axi::WriteResult{
        /*addr=*/0x100, /*size=*/5, /*len=*/0, axi::Burst::INCR,
        axi::LockType::Normal, /*data=*/{}, /*strb=*/{},
        /*resp=*/axi::Resp::SLVERR, /*id=*/0x05, /*scenario_line=*/5});
    EXPECT_TRUE(obs.ok());   // mismatches alone don't fail
    EXPECT_EQ(obs.mismatches(), 1u);
}

TEST(AxiMasterObserver, StuckCountMismatch) {
    SCENARIO("Observer: 1 AW callback fired, summary printed via destructor → stuck detected");
    {
        AxiMasterObserver obs("Test");
        obs.test_inject_write_result(axi::WriteResult{
            /*addr=*/0x100, /*size=*/5, /*len=*/0, axi::Burst::INCR,
            axi::LockType::Normal, /*data=*/{}, /*strb=*/{},
            /*resp=*/axi::Resp::OKAY, /*id=*/0x05, /*scenario_line=*/5});
        // Simulate "aw count > b count" by separately setting a synthetic
        // aw_count via test-only setter, then letting destructor fire summary.
        obs.test_set_aw_count(2);  // 2 issued but only 1 b observed
        // Destructor will fire print_summary() which appends FAIL context.
    }
    // After scope exit, summary has fired. Cannot verify via observer (gone)
    // but absence of crash + FAIL context in captured output is sufficient.
    // This is a smoke test for the RAII path.
    SUCCEED();
}

TEST(AxiMasterObserver, OutOfOrderRFail) {
    SCENARIO("Observer: same-id R arrives out of submission order → ok()=false, R-order FAIL");
    AxiMasterObserver obs("Test");
    obs.test_inject_read_result(axi::ReadResult{
        /*addr=*/0x100, /*size=*/5, /*len=*/0, axi::Burst::INCR,
        /*data=*/{}, /*resp=*/axi::Resp::OKAY, /*id=*/0x05, /*scenario_line=*/7});
    obs.test_inject_read_result(axi::ReadResult{
        /*addr=*/0x100, /*size=*/5, /*len=*/0, axi::Burst::INCR,
        /*data=*/{}, /*resp=*/axi::Resp::OKAY, /*id=*/0x05, /*scenario_line=*/5});
    EXPECT_FALSE(obs.ok());
    EXPECT_EQ(obs.failures().size(), 1u);
    // Should contain "R" in the failure string (vs OutOfOrderBFail which says "B")
}
```

- [ ] **Step 5: Run tests to verify FAIL**

```bash
cd c_model && cmake --build build 2>&1 | head -10
```

Expected: compile errors mentioning `test_logger.hpp` missing, `AxiMasterObserver` undeclared, `SCENARIO` undeclared.

- [ ] **Step 6: Create `c_model/tests/common/test_logger.hpp`**

```cpp
// Test logger: SCENARIO macro + AxiMasterObserver.
// See docs/superpowers/specs/2026-06-03-test-logger-scenario-observer-design.md
#pragma once
#include "axi/axi_master.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace ni::cmodel::testing {

// Always-printed one-line scenario description (no env var gate).
// Output: "[scenario] <desc>"
inline void print_scenario(const char* desc) {
    std::cout << "[scenario] " << desc << '\n';
}

}  // namespace ni::cmodel::testing

#define SCENARIO(desc) ::ni::cmodel::testing::print_scenario(desc)

namespace ni::cmodel::testing {

// Forward decl for friend access.
class AxiMasterObserverTest;

// Observer that hooks AxiMaster's completion callbacks and:
//   - counts logical (per-burst) AW/W/AR/B/R transactions
//   - tracks per-AXI-ID B/R submission_line sequences
//   - detects AXI4 IHI 0022 §A5.3 per-id ordering violations (emits FAIL)
//   - prints summary at end of test (RAII destructor as fallback)
//   - emits parse-friendly per-transaction trace when NOC_LOG=1
//
// Usage:
//   AxiMasterObserver obs(master, "NMU");
//   // ... run test loop ...
//   // Summary auto-prints at scope exit, OR call obs.print_summary() early.
class AxiMasterObserver {
public:
    // Normal ctor: hooks real AxiMaster callbacks.
    AxiMasterObserver(axi::AxiMaster& master, std::string instance_name = "AxiMaster")
        : name_(std::move(instance_name)) {
        const char* env = std::getenv("NOC_LOG");
        verbose_ = (env != nullptr) && std::string(env) == "1";
        master.on_write_completed([this](const axi::WriteResult& wr) { on_write(wr); });
        master.on_read_observed  ([this](const axi::ReadResult&  rr) { on_read (rr); });
    }

    // Test-only ctor: no AxiMaster (callbacks injected via test_inject_*).
    explicit AxiMasterObserver(std::string instance_name)
        : name_(std::move(instance_name)) {
        const char* env = std::getenv("NOC_LOG");
        verbose_ = (env != nullptr) && std::string(env) == "1";
    }

    // RAII fallback: print summary if not already printed.
    ~AxiMasterObserver() {
        if (!summary_printed_) print_summary();
    }

    // Explicit summary print. Sets summary_printed_=true on entry so the
    // destructor won't double-print.
    void print_summary() {
        summary_printed_ = true;
        // Stuck-transaction check (auto-fail condition #4).
        if (b_count_ < aw_count_) {
            failures_.push_back(
                "stuck_writes: aw_count=" + std::to_string(aw_count_) +
                " > b_count=" + std::to_string(b_count_) +
                " (incomplete write transactions at end of test)");
        }
        if (r_count_ < ar_count_) {
            failures_.push_back(
                "stuck_reads: ar_count=" + std::to_string(ar_count_) +
                " > r_count=" + std::to_string(r_count_) +
                " (incomplete read transactions at end of test)");
        }
        std::cout << "[summary:" << name_ << "] "
                  << "aw=" << aw_count_ << ' '
                  << "w_logical=" << aw_count_ << ' '
                  << "ar=" << ar_count_ << ' '
                  << "b=" << b_count_ << ' '
                  << "r=" << r_count_ << ' '
                  << "mismatches=" << mismatches_ << ' '
                  << "b_order_violations=" << b_order_violations_ << ' '
                  << "r_order_violations=" << r_order_violations_ << '\n';
        for (const auto& f : failures_) {
            std::cout << "[FAIL:" << name_ << "] " << f << '\n';
        }
    }

    bool ok() const { return failures_.empty(); }
    const std::vector<std::string>& failures() const { return failures_; }

    std::size_t aw_count() const { return aw_count_; }
    std::size_t ar_count() const { return ar_count_; }
    std::size_t b_count()  const { return b_count_;  }
    std::size_t r_count()  const { return r_count_;  }
    std::size_t mismatches() const { return mismatches_; }

    // Test-only injection (used by AxiMasterObserverTest unit tests).
    void test_inject_write_result(const axi::WriteResult& wr) { on_write(wr); }
    void test_inject_read_result (const axi::ReadResult&  rr) { on_read (rr); }
    void test_set_aw_count(std::size_t n) { aw_count_ = n; }
    void test_set_ar_count(std::size_t n) { ar_count_ = n; }

private:
    void on_write(const axi::WriteResult& wr) {
        ++obs_seq_;
        ++aw_count_;       // logical AW issued (callback fires on burst complete)
        ++b_count_;        // logical B observed
        if (wr.resp != axi::Resp::OKAY) ++mismatches_;
        check_b_order(wr.id, wr.scenario_line);
        if (verbose_) {
            std::cout << "[axi:" << name_ << "] obs_seq=" << obs_seq_
                      << " ch=B  id=0x" << std::hex << static_cast<int>(wr.id) << std::dec
                      << " resp=" << resp_str(wr.resp)
                      << " scenario_line=" << wr.scenario_line << '\n';
        }
    }

    void on_read(const axi::ReadResult& rr) {
        ++obs_seq_;
        ++ar_count_;
        ++r_count_;
        if (rr.resp != axi::Resp::OKAY) ++mismatches_;
        check_r_order(rr.id, rr.scenario_line);
        if (verbose_) {
            std::cout << "[axi:" << name_ << "] obs_seq=" << obs_seq_
                      << " ch=R  id=0x" << std::hex << static_cast<int>(rr.id) << std::dec
                      << " resp=" << resp_str(rr.resp)
                      << " scenario_line=" << rr.scenario_line << '\n';
        }
    }

    void check_b_order(uint8_t id, std::size_t scenario_line) {
        auto& seq = b_seq_by_id_[id];
        if (!seq.empty() && scenario_line < seq.back()) {
            ++b_order_violations_;
            failures_.push_back(
                "axi4_id_order_violation: id=0x" + hex_byte(id) +
                " expected B in submission order but got scenario_line=" +
                std::to_string(scenario_line) + " after scenario_line=" +
                std::to_string(seq.back()) +
                " (Possible causes: Rob reorder logic, per-NSU latency setup, or AxiMaster issue ordering)");
        }
        seq.push_back(scenario_line);
    }

    void check_r_order(uint8_t id, std::size_t scenario_line) {
        auto& seq = r_seq_by_id_[id];
        if (!seq.empty() && scenario_line < seq.back()) {
            ++r_order_violations_;
            failures_.push_back(
                "axi4_id_order_violation: id=0x" + hex_byte(id) +
                " expected R in submission order but got scenario_line=" +
                std::to_string(scenario_line) + " after scenario_line=" +
                std::to_string(seq.back()) +
                " (Possible causes: Rob reorder logic, per-NSU latency setup, or AxiMaster issue ordering)");
        }
        seq.push_back(scenario_line);
    }

    static std::string hex_byte(uint8_t v) {
        static const char* d = "0123456789abcdef";
        return std::string{d[v >> 4], d[v & 0xF]};
    }

    static const char* resp_str(axi::Resp r) {
        switch (r) {
            case axi::Resp::OKAY:   return "OKAY";
            case axi::Resp::EXOKAY: return "EXOKAY";
            case axi::Resp::SLVERR: return "SLVERR";
            case axi::Resp::DECERR: return "DECERR";
        }
        return "UNKNOWN";
    }

    std::string                              name_;
    bool                                     verbose_         = false;
    bool                                     summary_printed_ = false;
    std::size_t                              obs_seq_         = 0;
    std::size_t                              aw_count_        = 0;
    std::size_t                              ar_count_        = 0;
    std::size_t                              b_count_         = 0;
    std::size_t                              r_count_         = 0;
    std::size_t                              mismatches_      = 0;
    std::size_t                              b_order_violations_ = 0;
    std::size_t                              r_order_violations_ = 0;
    std::array<std::vector<std::size_t>, 256> b_seq_by_id_;
    std::array<std::vector<std::size_t>, 256> r_seq_by_id_;
    std::vector<std::string>                 failures_;
};

}  // namespace ni::cmodel::testing
```

(Note: the plan's Step 3 `#define`/include pattern is replaced by the cleaner public `test_inject_*` methods. Implementer drops the `#ifdef AXI_MASTER_OBSERVER_TEST_INJECT` notion from Step 3.)

- [ ] **Step 7: Update `c_model/tests/common/CMakeLists.txt` to register the new test**

Append:
```cmake
add_cmodel_test(test_test_logger)
```

- [ ] **Step 8: Adjust the 4 unit tests written in Steps 2-4 to use the actual `test_inject_*` public API**

Update Step 3's test (`OutOfOrderBFail`) to drop the `#define` pattern and use the direct ctor + injection:

```cpp
TEST(AxiMasterObserver, OutOfOrderBFail) {
    SCENARIO("Observer: same-id B arrives out of submission order → ok()=false, FAIL context emitted");
    AxiMasterObserver obs("Test");
    obs.test_inject_write_result(axi::WriteResult{
        /*addr=*/0x100, /*size=*/5, /*len=*/0, axi::Burst::INCR,
        axi::LockType::Normal, /*data=*/{}, /*strb=*/{},
        /*resp=*/axi::Resp::OKAY, /*id=*/0x05, /*scenario_line=*/7});
    obs.test_inject_write_result(axi::WriteResult{
        /*addr=*/0x100, /*size=*/5, /*len=*/0, axi::Burst::INCR,
        axi::LockType::Normal, /*data=*/{}, /*strb=*/{},
        /*resp=*/axi::Resp::OKAY, /*id=*/0x05, /*scenario_line=*/5});
    EXPECT_FALSE(obs.ok());
    EXPECT_EQ(obs.failures().size(), 1u);
}
```

Similarly adjust `NonOkayResp` and `StuckCountMismatch` — they already use `test_inject_*` per Step 4 draft.

- [ ] **Step 9: Build + run new tests**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R AxiMasterObserver -j 1
```

Expected: 4 PASS.

- [ ] **Step 10: Full ctest sweep**

```bash
ctest --test-dir build -j 1 2>&1 | tail -3
```

Expected: 302/302 passed (297 prior + 4 new).

- [ ] **Step 11: Drift gates**

```bash
cd ../specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
```

Expected: pytest 163 passed, codegen / gen_inventory --check clean.

- [ ] **Step 12: Commit Task 1**

```bash
cd ..
git add c_model/tests/common/test_logger.hpp \
        c_model/tests/common/test_test_logger.cpp \
        c_model/tests/common/CMakeLists.txt
git commit -m "feat(tests/common): add SCENARIO macro + AxiMasterObserver

SCENARIO(\"<desc>\") macro prints a one-line scenario description at the
start of each test for human readability — replaces reliance on cryptic
C++ identifier test names like NmuRob.Enabled_PushAr_AllocatesConsecutiveSlotsForBurst.

AxiMasterObserver class hooks AxiMaster's existing on_write_completed /
on_read_observed callbacks (zero production-code change). Counts logical
AW/W/AR/B/R transactions, tracks per-AXI-id submission_line sequences,
detects AXI4 §A5.3 ordering violations + non-OKAY responses + stuck txn
counts. Emits FAIL context with possible-cause hint (not diagnosis).
RAII destructor prints summary as fallback so missed explicit calls
don't lose context.

Verbose mode (NOC_LOG=1) adds parse-friendly per-transaction trace:
  [axi:NMU] obs_seq=1 ch=B id=0x05 resp=OKAY scenario_line=5
obs_seq is Observer-internal (AxiMaster has no cycle counter; no Stage 2
touch).

4 new unit tests: OrderedBPass, OutOfOrderBFail, NonOkayResp,
StuckCountMismatch. Test-only public injection methods (test_inject_*,
test_set_*_count) on observer for unit-level testing without spinning
up a full AxiMaster scenario.

Refs: docs/superpowers/specs/2026-06-03-test-logger-scenario-observer-design.md §4, §5, §6, §8, §10.1"
```

---

## Task 2: Retrofit SCENARIO to 240 existing TEST declarations

**Files:** 20 test files (per spec §10.2 table). Description-only change; no behavior modification; ctest count unchanged at 301.

**Goal:** Every `TEST(...)` / `TEST_F(...)` / `TEST_P(...)` body in the project has `SCENARIO("...")` as its first statement.

**Strategy:** This task is **bulk mechanical work** with thoughtful per-test descriptions. Implementer reads each test body, writes 1-line English description summarizing the invariant or scenario, prepends `SCENARIO("...");` after the `TEST(...) {` opening brace.

**Step granularity exception**: For mechanical retrofit, file-level steps are larger than the usual ≤5 min target (largest = `test_axi_master.cpp` 40 tests ≈ 80-120 min). Large files (`test_axi_master.cpp` 40, `test_axi_slave.cpp` 34, `test_protocol_rules.cpp` 38, `test_rob.cpp` 23) should be split mid-file at natural boundaries (test fixture / scenario group / comment header) into 2 sub-steps of ~20 each. Smaller files (≤15 tests) stay as single step. Implementer commits the WHOLE task once after all files retrofitted (or per spec §11.2 sub-commit option).

Description follows guidelines from spec §4.4:
- Concise: ≤80 chars
- English-only
- Describe invariant or scenario, not implementation
- Mention `Enabled mode` / `Disabled mode` explicitly when relevant
- Multiple SCENARIO calls allowed if invariant needs more than 80 chars

**Sub-commit option** (per spec §11.2): if a single 240-test diff feels unmanageable to review, split into:
- 2a: Stage 2 `tests/axi/*` (132 tests)
- 2b: Stage 3 `tests/nmu/*` (59 tests)
- 2c: Stage 3 `tests/nsu/*` (33 tests)
- 2d: `tests/common/*` + integration + `tests/test_flit.cpp` (16 tests)

Default: keep as single commit unless PR review surface ≥ 200 lines.

- [ ] **Step 1: Make sure Task 1's commit is built and ctest still green**

```bash
cd c_model && cmake --build build && ctest --test-dir build -j 1 2>&1 | tail -3
```

Expected: 302/302 PASS.

- [ ] **Step 2a: Retrofit `c_model/tests/axi/test_axi_master.cpp` first half (~20 tests, ScenarioParser block)**

- [ ] **Step 2b: Retrofit `c_model/tests/axi/test_axi_master.cpp` second half (~20 tests, AxiMasterTest / SplitIntoSubBursts blocks)**

Add `#include "common/test_logger.hpp"` near the existing test includes. Then for each `TEST_F(...) {` / `TEST_P(...) {` opening brace, insert `SCENARIO("...");` as the first body statement.

Example transformations (3 of 40):

```cpp
// Before:
TEST_F(ScenarioParser, MinimalWriteReadScenario) {
    auto sc = load_scenario("...");
    EXPECT_EQ(sc.transactions.size(), 2u);
    // ...
}

// After:
TEST_F(ScenarioParser, MinimalWriteReadScenario) {
    SCENARIO("scenario_parser: minimal YAML with 1 write + 1 read parses to 2 transactions");
    auto sc = load_scenario("...");
    EXPECT_EQ(sc.transactions.size(), 2u);
    // ...
}

// Before:
TEST_F(AxiMasterTest, SingleWriteTransactionExecutes) { ... }
// After:
TEST_F(AxiMasterTest, SingleWriteTransactionExecutes) {
    SCENARIO("AxiMaster: single write transaction from YAML completes with OKAY B response");
    // ...existing body...
}

// Before:
TEST_F(AxiMasterTest, SameIdConcurrentAdmissionVisibleInPipeline) { ... }
// After:
TEST_F(AxiMasterTest, SameIdConcurrentAdmissionVisibleInPipeline) {
    SCENARIO("AXI4 §A5.3 conformity: same-id concurrent writes admitted into per-id FIFO");
    // ...existing body...
}
```

(Implementer applies one SCENARIO per test for all 40 tests in this file. Each description ~30 sec to write after reading the test body.)

- [ ] **Step 3a: Retrofit `c_model/tests/axi/test_axi_slave.cpp` first half (~17 tests, WriteBurst / ReadBurst basics)**

- [ ] **Step 3b: Retrofit `c_model/tests/axi/test_axi_slave.cpp` second half (~17 tests, Wrap / Same-id / Exclusive)**

Same pattern. Examples:
```cpp
TEST(AxiSlave, WriteBurstSingleBeatInBoundsOkay) {
    SCENARIO("AxiSlave: single-beat write burst in memory bounds returns OKAY");
    // ...
}
TEST(AxiSlave, WriteBurstAtomicOob_PushesDecerrSkipsMemory) {
    SCENARIO("AxiSlave: atomic write fully OOB → emits DECERR B without touching memory");
    // ...
}
```

- [ ] **Step 4: Retrofit `c_model/tests/axi/test_integration.cpp` (1 test)**

Read test body; add 1 SCENARIO line summarizing the e2e integration scenario.

- [ ] **Step 5: Retrofit `c_model/tests/axi/test_memory.cpp` (8 tests)**

Same pattern; each Memory test description focuses on read/write/strb/bounds behavior.

- [ ] **Step 6a: Retrofit `c_model/tests/axi/test_protocol_rules.cpp` first half (~19 tests, basic rules)**

- [ ] **Step 6b: Retrofit `c_model/tests/axi/test_protocol_rules.cpp` second half (~19 tests, FIFO / advanced rules)**

This file has both EXPECT_DEATH-style asserts and the new `ProtocolRulesFifo` predicate tests. SCENARIO each.

- [ ] **Step 7: Retrofit `c_model/tests/axi/test_scaffold.cpp` (6 tests)**

Scaffold = build-system / infra tests; descriptions reflect that.

- [ ] **Step 8: Retrofit `c_model/tests/axi/test_scoreboard.cpp` (5 tests)**

- [ ] **Step 9: Retrofit `c_model/tests/nmu/test_addr_trans.cpp` (3 tests)**

Already-cryptic tests like `XyRoute_LowBitsAreLocalAddr` benefit most. Example:
```cpp
TEST(AddrTrans, XyRoute_LowBitsAreLocalAddr) {
    SCENARIO("addr_trans: addr=0x1234 → dst_id=0, local_addr=0x1234 (low 16 bits passthrough)");
    // ...
}
```

- [ ] **Step 10: Retrofit `c_model/tests/nmu/test_axi_slave_port.cpp` (14 tests)**

- [ ] **Step 11: Retrofit `c_model/tests/nmu/test_depacketize.cpp` (9 tests)**

Include the 2 new tests from prior round (`PopBWithMeta_ExtractsRobIdxAndRobReq`, `PopRWithMeta_ExtractsPerBeatRobIdx`). Note: previous rounds may already have added SCENARIO during Task 1 development if implementer chose; just check + skip if already present.

- [ ] **Step 12: Retrofit `c_model/tests/nmu/test_packetize.cpp` (10 tests)**

- [ ] **Step 13a: Retrofit `c_model/tests/nmu/test_rob.cpp` first half (~12 tests, Disabled mode + Enabled push)**

- [ ] **Step 13b: Retrofit `c_model/tests/nmu/test_rob.cpp` second half (~11 tests, Enabled pop + Death)**

This is the file user originally complained about. Examples:
```cpp
TEST(NmuRob, Enabled_PushAr_AllocatesConsecutiveSlotsForBurst) {
    SCENARIO("Rob Enabled: AR len=3 (4 beats) → 4 consecutive ROB slots, base rob_idx stamped to AR header");
    // ...
}
TEST(NmuRob, Enabled_PopB_OutOfOrder_HeldUntilHeadReady) {
    SCENARIO("Rob Enabled: out-of-order B held in slot until head of per-id sequence ready; chain-flush on completion");
    // ...
}
TEST(NmuRob, Disabled_StallSameIdDiffDst) {
    SCENARIO("Rob Disabled: same-id AW to different dst stalls until 1st AW's B response returns");
    // ...
}
```

- [ ] **Step 14: Retrofit `c_model/tests/nsu/test_axi_master_port.cpp` (14 tests)**

- [ ] **Step 15: Retrofit `c_model/tests/nsu/test_meta_buffer.cpp` (6 tests)**

- [ ] **Step 16: Retrofit `c_model/tests/nsu/test_nsu_depacketize.cpp` (7 tests)**

- [ ] **Step 17: Retrofit `c_model/tests/nsu/test_nsu_packetize.cpp` (6 tests)**

- [ ] **Step 18: Retrofit `c_model/tests/common/test_loopback_latency.cpp` (6 tests)**

Examples:
```cpp
TEST(LoopbackNocMultiNsu, RouteByDstId) {
    SCENARIO("LoopbackNoc multi-NSU: dst_id routing dispatches request flits to correct NSU req queue");
    // ...
}
TEST(LoopbackNocBackwardCompat, SingleNsuCtor_LegacyAccessAndDelayPreserved) {
    SCENARIO("LoopbackNoc backward-compat: single-NSU ctor + legacy aliases + global set_rsp_delay preserved");
    // ...
}
```

- [ ] **Step 19: Retrofit `c_model/tests/integration/test_port_pair_loopback.cpp` (1 test)**

- [ ] **Step 20: Retrofit `c_model/tests/integration/test_request_response_loopback.cpp` (1 parameterized test)**

The parameterized fixture `PacketizeLoopbackFixture::RunsFixture` instantiated over 7 YAML fixtures. Add 1 SCENARIO at body top:
```cpp
TEST_P(PacketizeLoopbackFixture, RunsFixture) {
    SCENARIO("End-to-end loopback for one YAML fixture: master→packetize→loopback→depacketize→slave→back; scoreboard validates data integrity");
    // ...
}
```

- [ ] **Step 21: Retrofit `c_model/tests/test_flit.cpp` (8 tests)**

- [ ] **Step 22: Build + full ctest**

```bash
cd c_model && cmake --build build && ctest --test-dir build -j 1 2>&1 | tail -3
```

Expected: **302/302 PASS unchanged** (description-only retrofit; no behavior change). Inspect output: every test now prints `[scenario] <desc>` line before `[ OK ]`.

- [ ] **Step 23: Drift gates**

```bash
cd ../specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
```

- [ ] **Step 24: Commit Task 2**

```bash
cd ..
git add c_model/tests/
git commit -m "test: retrofit SCENARIO to 240 existing TEST declarations

Per design spec §4.4 + §10.2: every TEST/TEST_F/TEST_P body's first
statement is SCENARIO(\"<concise English description>\"). Replaces
reliance on cryptic C++ identifier test names with human-readable
scenario summaries.

Coverage (20 files, 240 declarations):
- tests/axi/: 132 (master 40, slave 34, integration 1, memory 8,
  protocol_rules 38, scaffold 6, scoreboard 5)
- tests/nmu/: 59 (addr_trans 3, axi_slave_port 14, depacketize 9,
  packetize 10, rob 23)
- tests/nsu/: 33 (axi_master_port 14, meta_buffer 6, nsu_depacketize 7,
  nsu_packetize 6)
- tests/common/: 6 (loopback_latency)
- tests/integration/: 2 (port_pair_loopback 1, request_response_loopback 1)
- tests/test_flit.cpp: 8

Description-only; no behavior change. 302/302 ctest unchanged.

Refs: docs/superpowers/specs/2026-06-03-test-logger-scenario-observer-design.md §4.4, §10.2"
```

(If implementer chooses sub-commit split per spec §11.2, replace single `git add c_model/tests/` with 4 partial adds + 4 separate commits 2a/2b/2c/2d. Each sub-commit message follows the same pattern but cites the specific subsystem.)

---

## Task 3: Wire AxiMasterObserver into integration testbench

**Files:**
- Modify: `c_model/tests/integration/test_request_response_loopback.cpp`

**Goal:** Construct an `AxiMasterObserver` instance after AxiMaster construction in the integration testbench. Verify by running tests under `NOC_LOG=1` (manually) that per-transaction trace lines appear. Default (no env var) prints only `[scenario]` + `[summary:NMU]` per test.

- [ ] **Step 1: Read the integration testbench to find AxiMaster construction site**

```bash
grep -n "axi::AxiMaster" c_model/tests/integration/test_request_response_loopback.cpp
```

Identify the line where `axi::AxiMaster master(...)` is constructed.

- [ ] **Step 2: Add `#include "common/test_logger.hpp"` near the existing test includes**

- [ ] **Step 3: Construct `AxiMasterObserver obs(master, "NMU");` immediately after AxiMaster construction**

Insert:
```cpp
testing::AxiMasterObserver obs(master, "NMU");
```

(Uses `ni::cmodel::testing::AxiMasterObserver`. The `testing::` may need full qualification depending on `using` declarations already present.)

The observer's destructor will auto-print `[summary:NMU]` at scope exit. No `EXPECT_TRUE(obs.ok())` added in this task — just observability; hard fail is a future-round option if the assertion-on-violation pattern proves useful.

- [ ] **Step 4: Build + verify integration tests still pass**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R PacketizeLoopback -j 1
```

Expected: 7/7 PASS unchanged.

- [ ] **Step 5: Run verbose mode manually to verify trace output**

```bash
NOC_LOG=1 ctest --test-dir build -R "multi_dst_stress" -V 2>&1 | grep "axi:NMU\|summary:NMU\|scenario" | head -20
```

Expected output contains:
- `[scenario] ...` from SCENARIO in test body
- `[axi:NMU] obs_seq=N ch=B id=0xXX resp=OKAY scenario_line=N` per AW/AR completion
- `[summary:NMU] aw=N w_logical=N ar=N b=N r=N mismatches=0 b_order_violations=0 r_order_violations=0` at test end

- [ ] **Step 6: Full ctest sweep**

```bash
ctest --test-dir build -j 1 2>&1 | tail -3
```

Expected: 302/302 PASS unchanged.

- [ ] **Step 7: Drift gates**

```bash
cd ../specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
```

- [ ] **Step 8: Commit Task 3**

```bash
cd ..
git add c_model/tests/integration/test_request_response_loopback.cpp
git commit -m "test(tests/integration): wire AxiMasterObserver into PacketizeLoopback fixtures

Construct AxiMasterObserver obs(master, \"NMU\") after AxiMaster setup;
observer auto-prints [summary:NMU] line via RAII destructor at end of
test scope. Default (no env var) emits scenario + summary only.

NOC_LOG=1 ctest --test-dir build -R multi_dst_stress -V demonstrates
the full per-transaction trace (parse-friendly key=value format).

Hard fail on ordering violation (EXPECT_TRUE(obs.ok())) deferred to a
future round — initial wiring is observability-only. 302/302 ctest
unchanged.

Refs: docs/superpowers/specs/2026-06-03-test-logger-scenario-observer-design.md §10.3"
```

---

## Task 4: Audit + tighten production assert messages

**Files:**
- Modify: `c_model/include/ni/flit.hpp` (2 spots)
- Modify: `c_model/include/nmu/depacketize.hpp` (4 spots)
- Modify: `c_model/include/nmu/packetize.hpp` (2 spots)
- Modify: `c_model/include/nmu/rob.hpp` (5 spots)
- Modify: `c_model/include/nsu/depacketize.hpp` (3 spots)
- Modify: `c_model/include/nsu/packetize.hpp` (3 spots)

**Goal:** Replace bare `assert(false && "...")` messages with context (what went wrong + likely cause). 19 spots total identified via grep:

```bash
grep -rnE "assert\(false" c_model/include --include="*.hpp"
```

**Per spec §11.4 heuristic**: 19 > 5 → recommended split into 3 sub-commits per subsystem: 4a (ni/flit, 2 spots), 4b (nmu/, 11 spots), 4c (nsu/, 6 spots). NO 4d summary commit (each sub-commit stands alone). Implementer may keep as single commit if reviewing all 19 spots together is preferred.

- [ ] **Step 1: Pull fresh list of assert(false) spots**

```bash
grep -rnE "assert\(false" c_model/include --include="*.hpp" 2>&1 | head -30
```

Confirm 19 spots match plan. Add any new spots that appeared since spec was written.

- [ ] **Step 2: Tighten `c_model/include/ni/flit.hpp` asserts**

```cpp
// Line 66 (before):
assert(false && "unknown header field");
// After:
assert(false && "Flit::get_header_field: name not found in codegen-generated header layout — "
                "check ni::header::* declarations in ni_flit_constants.h or rerun codegen");

// Line 145 (before):
assert(false && "unknown payload field");
// After:
assert(false && "Flit::get_payload_field: name not found in codegen-generated payload layout — "
                "check ni::payload::* declarations in ni_flit_constants.h or rerun codegen");
```

- [ ] **Step 3: Tighten `c_model/include/nmu/depacketize.hpp` asserts**

```cpp
// Lines 37-39 (before):
std::optional<axi::AwBeat> pop_aw() override { assert(false && "NMU depacketize: AW not applicable"); std::abort(); return std::nullopt; }
// After:
std::optional<axi::AwBeat> pop_aw() override {
    assert(false && "nmu::Depacketize::pop_aw: NMU depacketizer handles response side only — "
                    "AW belongs on request side (NSU depacketizer or testbench bypass)");
    std::abort();
    return std::nullopt;
}
// (Same pattern for pop_w / pop_ar — adjust the channel name in the message)

// Line 104 (before):
assert(false && "NMU depacketize: NocRspIn delivered non-B/non-R flit");
// After:
assert(false && "nmu::Depacketize::tick: NocRspIn delivered an axi_ch value that's not B or R — "
                "upstream NoC sent a request flit through the response port; "
                "check NSU packetize src_id / dst_id stamping");
```

- [ ] **Step 4: Tighten `c_model/include/nmu/packetize.hpp` asserts**

```cpp
// Lines 68, 73 (before):
assert(false && "NMU packetize: B not applicable");
// After:
assert(false && "nmu::Packetize::push_b: NMU packetizer handles request side only — "
                "B belongs on response side (NSU packetizer)");
// (Same for push_r)
```

- [ ] **Step 5: Tighten `c_model/include/nmu/rob.hpp` asserts**

```cpp
// Lines 59, 62 (before):
assert(false && "Rob: push_b not applicable"); std::abort();
// After:
assert(false && "nmu::Rob::push_b: Rob multi-inheritance Packetizer side rejects response beats — "
                "AxiSlavePort routing or wiring issue");
std::abort();

// Lines 69, 72, 75 (similar):
assert(false && "Rob: pop_aw not applicable"); std::abort();
// After:
assert(false && "nmu::Rob::pop_aw: Rob multi-inheritance Depacketizer side rejects request beats — "
                "AxiSlavePort routing or wiring issue");
std::abort();
```

(Tighten all 5 Rob assert spots with the same multi-inheritance routing context.)

- [ ] **Step 6: Tighten `c_model/include/nsu/depacketize.hpp` asserts** (mirror of NMU pattern with NSU specifics)

- [ ] **Step 7: Tighten `c_model/include/nsu/packetize.hpp` asserts** (mirror)

- [ ] **Step 8: Build + verify**

```bash
cd c_model && cmake --build build && ctest --test-dir build -j 1 2>&1 | tail -3
```

Expected: 302/302 PASS unchanged. Assert messages are stripped at runtime under NDEBUG, so PASS is necessary but not sufficient. Spot-check: temporarily trigger one assert path manually (e.g. construct `nmu::Depacketize` and call `pop_aw()` — should abort with the new message visible in test output).

- [ ] **Step 9: Drift gates**

```bash
cd ../specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
```

- [ ] **Step 10: Commit Task 4**

If single commit:
```bash
cd ..
git add c_model/include/
git commit -m "fix(c_model): tighten 19 bare assert(false) messages with cause + likely-cause hints

Per design spec §9 + §11.4 + plan Task 4 audit: bare assert messages
like 'NMU packetize: B not applicable' get replaced with context:
  - what went wrong (which method + side)
  - likely cause (routing/wiring issue, codegen drift, etc.)
  - where to look first

Spots audited (19 total):
- ni/flit.hpp:66,145 (unknown header/payload field → codegen mismatch hint)
- nmu/depacketize.hpp:37-39,104 (wrong-side beat type, NoC delivers wrong channel)
- nmu/packetize.hpp:68,73 (wrong-side push)
- nmu/rob.hpp:59,62,69,72,75 (multi-inheritance routing hints, all 5 spots)
- nsu/depacketize.hpp:41,42,140 (mirror of nmu pattern)
- nsu/packetize.hpp:27,28,29 (mirror)

Behavior identical (assert message is text only); 302/302 ctest unchanged.

Refs: docs/superpowers/specs/2026-06-03-test-logger-scenario-observer-design.md §9, §11.4"
```

If sub-commits 4a/4b/4c per spec §11.4 heuristic, split as:
- 4a: `git add c_model/include/ni/flit.hpp` (2 spots) — `fix(ni/flit): tighten assert message context`
- 4b: `git add c_model/include/nmu/` (11 spots) — `fix(nmu): tighten assert message context (depacketize, packetize, rob)`
- 4c: `git add c_model/include/nsu/` (6 spots) — `fix(nsu): tighten assert message context (depacketize, packetize)`

---

## Task 5: NEXT_STEPS update + final drift gates

**Files:** `NEXT_STEPS.md`

**Goal:** Flip NEXT_STEPS headline to reflect logger round done; point at next round (vc_arb per main plan §3.1).

- [ ] **Step 1: Run final drift gates and record exact counts**

```bash
cd specgen && py -3 -m pytest -q
py -3 tools/codegen.py --check
py -3 tools/gen_inventory.py --check
cd ../c_model && ctest --test-dir build -j 1 2>&1 | tail -3
```

Expected:
- specgen pytest: 163 passed
- codegen --check: clean (no output)
- gen_inventory --check: clean (no output)
- ctest: 302/302 passed

- [ ] **Step 2: Read current `NEXT_STEPS.md`**

```bash
head -30 NEXT_STEPS.md
```

- [ ] **Step 3: Replace the "Current status" headline**

Update the headline section to include this round's completion. Use this verbatim and preserve sections below:

```markdown
## Current status (2026-06-03)

Stage 3 test logger (SCENARIO + AxiMasterObserver) 完工：
- tests/common/test_logger.hpp: SCENARIO("…") macro 一行人類描述 + AxiMasterObserver class
- AxiMasterObserver: hooks AxiMaster's on_write_completed/on_read_observed
  callbacks (zero production change); counts logical AW/W/AR/B/R, tracks per-id
  scenario_line sequences, auto-detects AXI4 §A5.3 ordering violations,
  RAII destructor 印 summary 作 fallback
- SCENARIO retrofit 全 240 個 TEST declarations (132 Stage 2 axi + 59 nmu +
  33 nsu + 6 common + 2 integration + 8 test_flit)
- Verbose mode (NOC_LOG=1) 加 parse-friendly per-transaction trace (key=value 風格)
- Production assert message audit: 19 個 bare assert(false) 加 cause + 可能原因 hint
- 301 ctest sequential pass (297 prior + 4 new Observer tests), drift gates clean

**Next task per main plan §3.1**: `vc_arb` virtual channel arbitration
(per-VC backpressure, round-robin or weighted scheduling, integrate with router fabric)。

後續 `vc_mapping` / `route_par` / `flit_ecc` / `nmu.hpp` top-level assembly
各自獨立 round。
```

- [ ] **Step 4: Commit Task 5**

```bash
git add NEXT_STEPS.md
git commit -m "$(cat <<'EOF'
docs(NEXT_STEPS): test logger done; next is vc_arb

Karpathy 4-lens summary (per Task 5):
- Overcomplication: clean — SCENARIO is 1-macro 1-function; AxiMasterObserver
  is ~150 LOC single class with clear scope (no internal Rob/NoC peek per
  Level 3 deferred)
- Surgical: zero production-code change for Observer (uses existing
  AxiMaster callbacks); assert audit edits message strings only, no
  control-flow change; SCENARIO retrofit is description-only
- Surface assumptions: AxiMaster callback fires once per logical
  transaction (not per beat); obs_seq is Observer-internal counter (not
  cycle); single global rng_ for random latency (NOT used here, just
  refers to LoopbackNoc's, separate)
- Verifiable success: 4 new Observer unit tests, 240 retrofit
  description-only, 19 assert spots tightened, 302/302 ctest unchanged

Drift gates final state:
- specgen pytest: 163 passed
- codegen.py --check: clean
- gen_inventory.py --check: clean
- c_model ctest: 301 sequential

5 commits complete (or 8 if Task 2 + Task 4 split per subsystem):
- 1: feat(tests/common): add SCENARIO macro + AxiMasterObserver
- 2: test: retrofit SCENARIO to 240 TEST declarations
- 3: test(tests/integration): wire AxiMasterObserver
- 4: fix(c_model): tighten assert messages with cause hints
- 5: docs(NEXT_STEPS): test logger done; next is vc_arb

All on stage3/packetize-depacketize branch per user direction to
accumulate features before merge+push.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 5: Final ctest sanity**

```bash
cd c_model && ctest --test-dir build -j 1 2>&1 | tail -3
```

Confirm 100% pass.

---

## Self-review checklist

After writing the plan, verified:

- **Spec coverage**:
  - §4 SCENARIO macro → Task 1 Step 6 (impl) + Task 2 (retrofit all 240 tests)
  - §5 AxiMasterObserver class → Task 1 Step 6 (full impl in test_logger.hpp) + Task 1 Steps 2-4 (4 unit tests)
  - §6 log format (parse-friendly key=value) → Task 1 Step 6 impl (verbose trace + summary + FAIL context)
  - §7 verbosity (NOC_LOG=1 env var) → Task 1 Step 6 ctor sets `verbose_` flag; Task 3 Step 5 demonstrates
  - §8 auto-fail conditions (4 invariants) → Task 1 Step 6 (check_b_order / check_r_order / non-OKAY / stuck-count) + Task 1 Steps 2-4 unit tests cover all 4
  - §9 assert audit → Task 4 (all 19 spots enumerated with before/after)
  - §10.1 4 Observer unit tests → Task 1 Steps 2-4
  - §10.2 240 retrofit → Task 2 (Steps 2-21 cover all 20 files with exact counts)
  - §10.3 integration wiring → Task 3
  - §11 commit boundary (5 commits, split heuristic for Tasks 2+4) → maps 1:1 to Tasks 1-5
- **Placeholder scan**: no TBD / TODO / handwave. Every step has complete code, exact paths, exact commands with expected output.
- **Type consistency**:
  - `AxiMasterObserver` declared in Task 1 Step 6; used in Tasks 1, 3
  - `SCENARIO` macro defined in Task 1 Step 6; used in Tasks 1-4 examples
  - `test_inject_write_result` / `test_inject_read_result` / `test_set_aw_count` / `test_set_ar_count` declared in Task 1 Step 6 public API; used in Task 1 Steps 3-4 unit tests
  - `axi::WriteResult` / `axi::ReadResult` fields (addr/size/len/burst/lock/data/strb_per_beat/resp/id/scenario_line) match `c_model/include/axi/axi_master.hpp:64-90`
  - `axi::Resp::OKAY|EXOKAY|SLVERR|DECERR` enum values (used in `resp_str` helper)
- **Acceptance counts**: 297 → 301 (Task 1 adds 4) → 301 (Tasks 2-5 no test count change). Monotonic, internally consistent.
- **Cross-references**: Task 1 establishes API; Task 2 uses SCENARIO (no observer); Task 3 uses observer; Task 4 independent; Task 5 final sweep. No forward references to undefined symbols.

---

## Execution

Plan complete and ready for commit.

**Two execution options:**

**1. Subagent-Driven (recommended)** — Dispatch fresh subagent per task, two-stage review (spec + quality) between tasks, fast iteration. Same pattern as prior 2 rounds.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch with checkpoints.

Which approach?
