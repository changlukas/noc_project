# Test Logger: SCENARIO macro + AxiMasterObserver — design

**Status**: design approved (Codex round-1 NEEDS-REVISION → all fixes folded into this spec; user skipped round-2 verify)
**Date**: 2026-06-03
**Owner**: c_model NoC behavior model (`noc::` namespace)
**Branch**: `stage3/packetize-depacketize` (accumulating; no push until full feature set merged)

**Prior round**: `2026-06-03-rob-enabled-mode-multi-nsu-testbench-design.md` — shipped ROB Enabled mode + multi-NSU testbench. 297 ctest passing but test names like `NmuRob.Enabled_PushAr_AllocatesConsecutiveSlotsForBurst` are cryptic — user can't tell what each test exercises by reading the name.

**References for this round**:
- FlooNoC `axi_dumper` / `axi_reorder_compare` (`pulp-platform/FlooNoC/hw/tb/tb_floo_rob.sv:223-235, :496-508`)
- FlooNoC `axi_bw_monitor` end-of-test summary pattern
- OpenTitan lowRISC DV `DVCodingStyle.md` (one event per line, parse-friendly, simulation-time prefix)
- gem5 `DPRINTF` flag-gated trace pattern
- Verified Codex survey: industry consensus is quiet-by-default + env-var-gated verbose + master-side observation sufficient

---

## 1. Motivation

Test output today is cryptic:
```
240/297 Test #240: NmuRob.Enabled_PushAr_AllocatesConsecutiveSlotsForBurst ... Passed   0.01 sec
```

Test name is a C++ identifier — can't be Chinese, can't be free-form. User reading log can't infer what scenario was exercised. When a test fails, GoogleTest's default output gives line number + expected/actual but no surrounding NoC/AXI context (which transactions were issued, which ROB slot was involved, etc.).

This round delivers:

1. **`SCENARIO("<English description>")` macro** added to every existing 46 tests + new tests going forward — always-printed one-line human description.
2. **`AxiMasterObserver` class** — opt-in observer that hooks `AxiMaster` callbacks for integration tests; counts AW/W/AR/B/R logical transactions, tracks per-id submission order, prints summary at test end. Verbose mode (`NOC_LOG=1`) adds per-transaction trace.
3. **Auto-fail context dumps** — Observer detects AXI4 §A5.3 per-id ordering violations and emits structured FAIL context (expected vs observed, possible causes).
4. **Production assert message audit** — bare `assert(false && "impossible")` strings replaced with cause + likely-cause hint.

Anchored decisions (all pre-debated; do not re-open without justification):
- 2 verbosity tiers (quiet + `NOC_LOG=1` verbose). Skip Level 3 internal-state peek (no `Rob` slot table, no NoC queue snapshot).
- SCENARIO retrofitted to all 46 existing tests in one commit (no half-state).
- English-only SCENARIO descriptions (concise, cross-team-readable).
- Standalone `AxiMasterObserver` class (NOT embedded in Scoreboard; NOT free functions).
- Master-side observation only (no `Rob` / `LoopbackNoc` instrumentation). Matches FlooNoC `axi_reorder_compare` pattern.
- `obs_seq=N` Observer-internal counter (NOT wall-clock cycle) — `AxiMaster` has no cycle counter and we don't touch Stage 2 production code.
- RAII destructor as summary-print fallback so missed explicit calls don't lose context.

## 2. Scope

**In scope**:
- New `c_model/tests/common/test_logger.hpp` with `SCENARIO` macro + `AxiMasterObserver` class
- 4 unit tests for `AxiMasterObserver`
- Retrofit 46 existing tests with `SCENARIO("…")` first line
- Wire `AxiMasterObserver` into 7 integration testbench fixtures
- Audit `c_model/include/{axi,ni,nmu,nsu}/*.hpp` for bare `assert(false && "impossible")` → add cause + possible-cause hints (~10-15 spots)

**Out of scope** (deferred / future round):
- Internal-state peek (Rob slot occupancy table, LoopbackNoc queue contents, flit-level trace) — Level 3 in design
- Ring-buffer last-N-transaction failure dump (FlooNoC-style) — Level 3 in design
- File-based trace output (`NOC_LOG_FILE=...`) — future if CI needs persistent artifacts
- Coverage tracking (per-fixture which AXI modes hit) — separate concern
- Cycle counter in AxiMaster — production change, breaks "Stage 2 untouched" principle

## 3. Component flow

```
─────────────────────────────────────────────────────────────────
Test body                  ┌───────────────────────────────────┐
   │  SCENARIO("…") ──────►│ print_scenario() → stdout         │
   │                        │   format: [scenario] <desc>       │
   │                        └───────────────────────────────────┘
   │
   │  AxiMasterObserver obs(master, "NMU");
   │     ↓ ctor hooks
   │   master.on_write_completed([&](auto& wr){ obs.on_write(wr); })
   │   master.on_read_observed  ([&](auto& rr){ obs.on_read (rr); })
   │
   │  ... test loop runs (master.tick(), nsu_port.tick(), etc.) ...
   │     each completed AW burst → wr callback → obs.on_write:
   │       - increment counters
   │       - record b_seq_by_id_[id].push_back(scenario_line)
   │       - check_b_order(id, scenario_line) — auto-fail if violation
   │       - if NOC_LOG=1: print verbose trace line
   │     (same pattern for read)
   │
End of test (or ~obs):
   │  obs.print_summary() ──► [summary:NMU] aw=N w=N ar=N b=N r=N
   │                          [FAIL:NMU] <context> if violations
─────────────────────────────────────────────────────────────────
```

`SCENARIO` and `AxiMasterObserver` are independent — unit tests use `SCENARIO` only (no `AxiMaster` exists in test_rob.cpp); integration tests use both.

## 4. `SCENARIO` macro

### 4.1 API

```cpp
// c_model/tests/common/test_logger.hpp
#pragma once
#include <iostream>

namespace ni::cmodel::testing {

// Print a one-line scenario description at the start of a test.
// Output: "[scenario] <desc>"
// Always printed (no env var gate) so even quiet mode is informative.
inline void print_scenario(const char* desc) {
    std::cout << "[scenario] " << desc << '\n';
}

}  // namespace ni::cmodel::testing

#define SCENARIO(desc) ::ni::cmodel::testing::print_scenario(desc)
```

### 4.2 Usage convention

Every `TEST(...)` body's **first line** is `SCENARIO("...")` describing what invariant or scenario the test exercises.

Examples (existing test retrofit):
```cpp
TEST(NmuRob, Enabled_PushAr_AllocatesConsecutiveSlotsForBurst) {
    SCENARIO("Enabled mode: 4-beat read AR allocates 4 consecutive ROB slots, AR header stamps base rob_idx");
    // ... existing body unchanged
}

TEST(NmuDepacketize, PopBWithMeta_ExtractsRobIdxAndRobReq) {
    SCENARIO("pop_b_with_meta extracts rob_idx + rob_req from flit header alongside BBeat");
    // ...
}

TEST_P(PacketizeLoopbackFixture, RunsFixture) {
    SCENARIO("End-to-end loopback for one YAML fixture: master issues transactions, scoreboard verifies data integrity");
    // ...
}
```

### 4.3 Output

Always printed to stdout. GoogleTest's default output stream merges with stdout under `ctest -V`, so the user sees:

```
[ RUN      ] NmuRob.Enabled_PushAr_AllocatesConsecutiveSlotsForBurst
[scenario] Enabled mode: 4-beat read AR allocates 4 consecutive ROB slots, AR header stamps base rob_idx
[       OK ] NmuRob.Enabled_PushAr_AllocatesConsecutiveSlotsForBurst (1 ms)
```

In default `ctest` mode (no `-V`), the scenario line is suppressed (GoogleTest captures stdout per-test). User wanting to see scenarios runs `ctest -V` or `ctest --output-on-failure` (for failure-only context).

### 4.4 Description guidelines

- **Concise**: must fit ≤80 chars per line. If invariant needs more, split into multiple SCENARIO calls, each ≤80 chars.
- **English only**: no Chinese / no special chars beyond standard ASCII.
- **Describe invariant or scenario, not implementation**: "expects 4 consecutive ROB slots allocated" not "calls find_consecutive_free with n=4".
- **Mention `Enabled mode` / `Disabled mode`** explicitly when relevant — context for mode-specific tests.

## 5. `AxiMasterObserver` class

### 5.1 API

```cpp
// c_model/tests/common/test_logger.hpp (same file as SCENARIO)
#include "axi/axi_master.hpp"
#include <array>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace ni::cmodel::testing {

// Observer that hooks AxiMaster's completion callbacks and:
//   - counts logical (per-transaction) AW/W/AR/B/R
//   - tracks per-AXI-ID B/R submission order
//   - detects AXI4 IHI 0022 §A5.3 per-id ordering violations (auto-fail context)
//   - prints summary at end of test (RAII destructor as fallback)
//   - emits parse-friendly per-transaction trace when NOC_LOG=1
//
// Usage (integration test):
//   AxiMasterObserver obs(master, /*name=*/"NMU");
//   // ... run test ...
//   EXPECT_TRUE(obs.ok());   // optional: hard fail on detected violations
//   // Summary auto-prints at scope exit if not already printed.
class AxiMasterObserver {
public:
    AxiMasterObserver(axi::AxiMaster& master, std::string instance_name = "AxiMaster")
        : name_(std::move(instance_name)) {
        const char* env = std::getenv("NOC_LOG");
        verbose_ = env && std::string(env) == "1";
        master.on_write_completed([this](const axi::WriteResult& wr) { on_write(wr); });
        master.on_read_observed  ([this](const axi::ReadResult&  rr) { on_read (rr); });
    }

    ~AxiMasterObserver() {
        if (!summary_printed_) print_summary();
    }

    // Explicit summary print (optional; destructor auto-prints if not called).
    // Sets summary_printed_ = true on entry to prevent destructor double-print.
    void print_summary();

    // Test-side query for hard-fail assertion.
    bool ok() const { return failures_.empty(); }
    const std::vector<std::string>& failures() const { return failures_; }

    // Public counter accessors (for test introspection if needed).
    std::size_t aw_count() const { return aw_count_; }
    std::size_t b_count()  const { return b_count_;  }
    std::size_t ar_count() const { return ar_count_; }
    std::size_t r_count()  const { return r_count_;  }
    std::size_t mismatches() const { return mismatches_; }

private:
    void on_write(const axi::WriteResult& wr);
    void on_read (const axi::ReadResult&  rr);
    void check_b_order(uint8_t id, std::size_t scenario_line);
    void check_r_order(uint8_t id, std::size_t scenario_line);
    void emit_trace(const char* ch, uint8_t id, std::size_t scenario_line, /*details*/);

    std::string                              name_;
    bool                                     verbose_         = false;
    bool                                     summary_printed_ = false;
    std::size_t                              obs_seq_         = 0;   // Observer-internal sequence
    std::size_t                              aw_count_        = 0;
    std::size_t                              ar_count_        = 0;
    std::size_t                              b_count_         = 0;
    std::size_t                              r_count_         = 0;
    std::size_t                              mismatches_      = 0;
    std::size_t                              b_order_violations_ = 0;
    std::size_t                              r_order_violations_ = 0;
    std::array<std::vector<std::size_t>, 256> b_seq_by_id_;   // per-id observed scenario_line sequence
    std::array<std::vector<std::size_t>, 256> r_seq_by_id_;
    std::vector<std::string>                 failures_;

    // Note: `obs_seq_` is Observer-internal — AxiMaster does NOT expose a cycle
    // counter, and we do NOT touch Stage 2 production code. `obs_seq_` increments
    // once per WriteResult/ReadResult callback observation. For per-id ordering
    // verification this is sufficient; for wall-clock cycle precision a future
    // round may add an AxiMaster-side counter.
};

}  // namespace ni::cmodel::testing
```

### 5.2 Callback contract clarification

`AxiMaster::on_write_completed(cb)` fires the callback **ONCE per logical write transaction** when all sub-burst B responses have arrived. `AxiMaster::on_read_observed(cb)` fires **ONCE per logical read transaction** when all R beats have arrived (`r->last` on final sub-burst).

Counter semantics: `aw_count_` counts **logical AW transactions** (one per scenario line). `b_count_` counts **logical B observations** (one per completed write). At test end:
- `aw_count_ == b_count_` ⇒ all writes complete
- `aw_count_ > b_count_` ⇒ stuck writes (auto-fail)
- `b_count_ > aw_count_` ⇒ extra B (programmer error or scoreboard bug)

Same for `ar_count_` vs `r_count_`. The observer does NOT count W beats or R beats individually — those are sub-transaction events not exposed by the callback contract.

### 5.3 Why standalone class (not Scoreboard extension)

- Scoreboard's job is data-integrity verification (compare actual vs expected bytes per address). Adding logging there is responsibility creep.
- Observer hooks the existing `on_*_completed` callbacks — zero modification to `AxiMaster` or any production code.
- Test that doesn't construct `AxiMasterObserver` sees no logger output (opt-in).
- Lives in `tests/common/` (testbench-only namespace `ni::cmodel::testing`).

## 6. Log format

All output goes to `std::cout`. Format is parse-friendly (key=value, space-separated) AND human-scannable. No color, no terminal escapes (avoids issues with CI log capture).

### 6.1 SCENARIO line (always printed)

```
[scenario] <desc>
```

### 6.2 Per-transaction trace line (NOC_LOG=1 only)

```
[axi:NMU] obs_seq=1 ch=AW id=0x05 addr=0x100 size=5 len=0 burst=INCR scenario_line=5
[axi:NMU] obs_seq=2 ch=B  id=0x05 resp=OKAY scenario_line=5
[axi:NMU] obs_seq=3 ch=AR id=0x06 addr=0x200 size=5 len=3 burst=INCR scenario_line=7
[axi:NMU] obs_seq=4 ch=R  id=0x06 resp=OKAY scenario_line=7
```

- `obs_seq=N` — Observer's internal counter (not cycle)
- `ch=AW|W|AR|B|R` — but only AW/AR/B/R appear (callback is per-burst, no W beat trace)
- `id=0xNN` — hex AXI ID (consistent with project convention)
- `addr=0xNN` — hex address
- `size/len/burst` — AXI4 fields, decoded enum for `burst` (INCR/WRAP/FIXED)
- `resp=OKAY|SLVERR|DECERR|EXOKAY` — decoded enum
- `scenario_line=N` — YAML line number from fixture

Note: `ch=W` is never emitted because `on_write_completed` fires per-burst, not per-W-beat. If future need arises, add a separate W-beat hook.

### 6.3 Summary line (always printed at end of test)

```
[summary:NMU] aw=2 w_logical=2 ar=0 b=2 r=0 mismatches=0 b_order_violations=0 r_order_violations=0
```

- `aw / b / ar / r` — logical transaction counts
- `w_logical` = `aw` (no separate W beat count tracked; field present for symmetry with future hooks)
- `mismatches` — count of B/R with non-OKAY response
- `b_order_violations / r_order_violations` — count of per-id ordering breaks

### 6.4 FAIL context (always printed when violation detected)

```
[FAIL:NMU] axi4_id_order_violation: id=0x05 expected B in submission order [line=5,line=6] but got [line=6,line=5]
  AW1 scenario_line=5 observed at obs_seq=2 (out of order)
  AW2 scenario_line=6 observed at obs_seq=1
  Possible causes: Rob reorder logic, per-NSU latency setup, or AxiMaster issue ordering
```

`Possible causes` framed as hint not diagnosis — invites user investigation, won't drift.

## 7. Verbosity control: `NOC_LOG` env var

| Env state | Behavior |
|---|---|
| Unset or `NOC_LOG=0` | Quiet: SCENARIO + summary + FAIL context only |
| `NOC_LOG=1` | Verbose: above + per-transaction trace line |

```cpp
const char* env = std::getenv("NOC_LOG");
verbose_ = env && std::string(env) == "1";
```

Explicit string compare to `"1"` (not generic truthy) for predictable behavior.

Run example:
```bash
ctest --test-dir build -j 1                              # quiet
NOC_LOG=1 ctest --test-dir build -R multi_dst_stress -V  # verbose, see all traces
```

No CMake flag (avoids build matrix drift; runtime control is enough).

## 8. Auto-fail conditions

Observer auto-detects 4 invariants and accumulates FAIL context (does NOT abort test; test calls `EXPECT_TRUE(obs.ok())` for hard fail):

| # | Invariant | Detection |
|---|---|---|
| 1 | AXI4 §A5.3 per-id B order | `b_seq_by_id_[id]` must be monotonically non-decreasing in `scenario_line` |
| 2 | AXI4 §A5.3 per-id R order | Same for `r_seq_by_id_[id]` |
| 3 | B/R `resp != OKAY` | Each non-OKAY response → `mismatches_++` (no FAIL by default, just count; test may assert specific count) |
| 4 | Stuck transactions at test end | `b_count_ < aw_count_` or `r_count_ < ar_count_` checked in `print_summary()` |

Invariants 1+2 emit immediate FAIL context lines. Invariants 3+4 only contribute to summary counters; user inspects summary line and decides if test should hard-fail.

## 9. Production assert message audit

Grep `c_model/include/{axi,ni,nmu,nsu}/*.hpp` for bare assert messages and add 1-line cause hints. Examples:

| Before | After |
|---|---|
| `assert(false && "impossible")` | `assert(false && "axi_master: drained B for id with empty deque — likely missing AW or duplicate B")` |
| `assert(false && "Rob: pop_b not applicable")` | `assert(false && "Rob: Disabled-mode interface ambiguity — push_b/pop_b on the wrong base; check multi-inheritance routing")` |

Goal: when the assert fires, the user reads the message and gets immediate context, not just `"impossible"`. Apply to all bare `assert(false)` strings discovered in audit. Estimated 10-15 spots.

## 10. Test plan

### 10.1 New unit tests (4) for `AxiMasterObserver`

| Test | Invariant |
|---|---|
| `AxiMasterObserver.OrderedBPass` | 2 same-id writes, in-order B → `ok() == true`, `b_order_violations == 0` |
| `AxiMasterObserver.OutOfOrderBFail` | 2 same-id writes, reversed B order → `ok() == false`, `b_order_violations == 1`, FAIL context emitted |
| `AxiMasterObserver.NonOkayResp` | B with `resp=SLVERR` → `mismatches == 1`, FAIL not triggered (test-side decision) |
| `AxiMasterObserver.StuckCountMismatch` | 1 AW issued, 0 B at test end → `print_summary()` fires `[FAIL:…] stuck` context |

Each test uses a tiny test fixture (`MockAxiMaster` or mocked `WriteResult` injection). Implementation detail: can avoid full AxiMaster by directly calling `on_write` / `on_read` private methods via friend class, OR use a minimal `AxiMaster` with hand-crafted callback fires.

### 10.2 SCENARIO retrofit (46 existing tests)

One-by-one read each test body, write a concise English scenario description, prepend `SCENARIO("…");` as first line. Estimated 2 hours of focused work (not 30 min — quality requires reading + thinking).

| File | Test count to retrofit |
|---|---|
| `c_model/tests/axi/test_axi_master.cpp` | ~5 |
| `c_model/tests/axi/test_protocol_rules.cpp` | ~2 |
| `c_model/tests/nmu/test_addr_trans.cpp` | 3 |
| `c_model/tests/nmu/test_depacketize.cpp` | ~7 |
| `c_model/tests/nmu/test_packetize.cpp` | ~10 |
| `c_model/tests/nmu/test_rob.cpp` | ~23 |
| `c_model/tests/common/test_loopback_latency.cpp` | 6 |
| `c_model/tests/integration/test_request_response_loopback.cpp` | 1 (the parameterized fixture) |
| **Total** | **~46** (rounding errors OK) |

Plan task will list the actual test names per file (after grepping `TEST\(` patterns).

### 10.3 Integration testbench wiring

Add 1 line to construct `AxiMasterObserver obs(master, "NMU");` after the existing AxiMaster construction in `test_request_response_loopback.cpp`. Observer's RAII destructor fires at end of `RunsFixture` scope. Optionally add `EXPECT_TRUE(obs.ok())` for hard fail on detected violations.

### 10.4 Test count summary

| Source | Count |
|---|---|
| New `test_logger.cpp` (Observer unit tests) | +4 |
| SCENARIO retrofit (46 existing tests) | 0 new tests (description-only) |
| Integration testbench Observer wiring | 0 new tests (observability-only) |
| Production assert audit | 0 new tests |
| **Total new tests** | **4** |
| Prior round total | 297 |
| **Final ctest target** | **301/301** |

### 10.5 Drift gates (per commit)

```bash
cd specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
cd ../c_model && cmake --build build && ctest --test-dir build -j 1
```

Expected at HEAD:
- specgen pytest: 163 passed
- codegen --check: clean
- gen_inventory --check: clean
- ctest: 301/301

## 11. Commit boundary plan

### Commit 1: `feat(tests/common): add SCENARIO macro + AxiMasterObserver`

**Files**:
- Create `c_model/tests/common/test_logger.hpp` (~150 LOC)
- Create `c_model/tests/common/test_logger.cpp` (impl out-of-line, optional)
- Create `c_model/tests/common/test_test_logger.cpp` (4 Observer unit tests)
- Modify `c_model/tests/common/CMakeLists.txt` (register test_test_logger)

**Acceptance**: 297 → 301 ctest. Standalone framework, no existing tests touched.

### Commit 2: `test: retrofit SCENARIO to 46 existing tests`

**Files**: 8 test files listed in §10.2.

**Acceptance**: 301/301 ctest unchanged. Pure description-add, no behavior change. Each test's body adds 1 line (`SCENARIO("…");`) as first line.

### Commit 3: `test(tests/integration): wire AxiMasterObserver into PacketizeLoopback fixtures`

**Files**:
- Modify `c_model/tests/integration/test_request_response_loopback.cpp` (~5 LOC)

**Acceptance**: 301/301 ctest unchanged. Observer added but `EXPECT_TRUE(obs.ok())` is optional — initial commit doesn't enforce hard fail, just adds observability. Summary lines visible in `ctest -V`.

### Commit 4: `fix(c_model): tighten assert messages with cause + possible-cause context`

**Files**: scope by subsystem to reduce blast radius (per Codex Important#5):
- Sub-commit 4a: `c_model/include/axi/*.hpp` (Stage 2 — careful)
- Sub-commit 4b: `c_model/include/ni/*.hpp`
- Sub-commit 4c: `c_model/include/nmu/*.hpp`
- Sub-commit 4d: `c_model/include/nsu/*.hpp`

OR keep as single Commit 4 if audit reveals ≤5 spots total (heuristic: split iff total spots > 5).

**Acceptance**: 301/301 ctest unchanged. Behavior identical (assert strings only).

### Commit 5: `docs(NEXT_STEPS): test logger done; next is vc_arb`

**Files**: `NEXT_STEPS.md`

**Acceptance**: drift gates clean, ctest 301/301.

### Commit count: 5 (or 8 if Commit 4 splits per subsystem)

## 12. Risk register

| Risk | Mitigation |
|---|---|
| Commit 2 SCENARIO retrofit produces shallow descriptions due to volume (46 tests) | Allocate 2hr focused, not 30min. Plan task lists each test name so implementer can't skip. |
| Commit 4 assert audit accidentally changes behavior | Each sub-commit modifies one subsystem only; reviewer must verify message-only changes |
| Observer's RAII destructor double-prints if test calls `print_summary()` explicitly | Internal `summary_printed_` flag prevents double-print |
| `AxiMasterObserver` ctor takes `axi::AxiMaster&` by reference — lifetime must exceed observer | Observer is local to test scope; AxiMaster constructed first → destructor order naturally observer-first |
| `NOC_LOG=1` verbose trace in CI clutter | Default off; CI runs without env var → quiet. Verbose only for local debug. |
| `obs_seq` is not wall-clock cycle — confusing? | Doc says "Observer-internal sequence", not cycle. Future round may add cycle if needed. |

## 13. References

- Prior round design: `docs/superpowers/specs/2026-06-03-rob-enabled-mode-multi-nsu-testbench-design.md`
- Main plan: `docs/noc_cmodel_rtl_plan.md` §3.1
- FlooNoC `axi_dumper`: `pulp-platform/FlooNoC/hw/tb/tb_floo_rob.sv:223-235, :496-508` (instantiated but disabled by default — `LogAW/AR/W/B/R = 0`)
- FlooNoC `axi_reorder_compare`: `pulp-platform/FlooNoC/hw/test/axi_reorder_compare.sv:34-116, :157-285` (per-AXI-field expected/observed table on mismatch; default `.Verbose(0)`)
- FlooNoC `axi_bw_monitor`: `pulp-platform/FlooNoC/hw/test/axi_bw_monitor.sv:140-145` (end-of-test latency mean/stddev/BW summary only)
- OpenTitan lowRISC DV: `lowRISC/style-guides/blob/master/DVCodingStyle.md` (one-event-per-line, parse-friendly, sim-time prefix, UVM verbosity tiers)
- gem5 `DPRINTF`: gem5.org/documentation/general_docs/debugging_and_testing/debugging/trace_based_debugging
- AXI4 spec: ARM IHI 0022 §A5.3 (same-ID response ordering)
- AxiMaster callback API: `c_model/include/axi/axi_master.hpp:291-292` (`on_write_completed`, `on_read_observed`)
- WriteResult / ReadResult structs: `c_model/include/axi/axi_master.hpp:64-90`
- AXI_ID_WIDTH=8 (256 entries): `specgen/generated/cpp/ni_flit_constants.h` `ni::width::AXI_ID_WIDTH`
