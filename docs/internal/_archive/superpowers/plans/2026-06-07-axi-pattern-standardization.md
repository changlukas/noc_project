# AXI4 Pattern Standardization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Flatten `sim/test_patterns/` to AX4-CAT-NNN_slug IDs, add YAML metadata + parser strict mode, share scenario list via CMake glob, replace wb2axip-blocked maintenance with a runtime predicate, add 5 spec-gap patterns.

**Architecture:** Four commits. Commit 1 lands infra (parser strict mode + helpers + CMake scaffold + lint script) without touching scenarios. Commit 2 is the atomic migration (31 renames + metadata + lint hookup + 5 cpp file rewrites + cosim test rename). Commit 3 adds 5 new spec-gap patterns. Commit 4 rewrites README.

**Tech Stack:** C++17 + GoogleTest, CMake 3.12+ (CONFIGURE_DEPENDS), yaml-cpp (already in tree), Python 3 (lint), Verilator 5.036 + wb2axip (unchanged).

**Spec:** `docs/superpowers/specs/2026-06-07-axi-pattern-standardization-design.md`

**Project-specific discipline (per CLAUDE.md + memory):**
- `clang-format -i` after every C++ edit (Google base + 4-space indent + 100 col)
- Commit messages `type(scope): description` (English) — types: feat, fix, docs, style, refactor, test, chore, perf
- Never `--no-verify`
- Test naming: full-word snake_case (no abbreviation)
- Match ceremony to feature complexity — this round is structural refactor, not new feature
- Pattern reference: every new scenario must cite IHI 0022H § or tim_axi4_vip test name in the commit message

---

## File Structure

### Files created (commit 1)

| Path | Responsibility |
|---|---|
| `sim/test_patterns/CMakeLists.txt` | CMake glob over `AX4-*/scenario.yaml`, generate `scenarios_list.hpp`, expose `noc_axi4_scenarios` INTERFACE library |
| `sim/test_patterns/scenarios_list.hpp.in` | Template for generated header (`std::array<std::string_view, N> kAllAxi4Scenarios`) |
| `sim/test_patterns/scenario_helpers.hpp` | `RequireKnownScenario(id)` — abort on unknown ID at startup; for scoped tests |
| `cosim/tests/wb2axip_block.hpp` | `wb2axip_block_reason(Scenario const&)` — runtime predicate for cosim integration test |
| `cosim/tests/test_wb2axip_block.cpp` | Unit tests for the predicate |
| `c_model/tests/axi/test_scenario_metadata.cpp` | Unit tests for new metadata parsing |
| `tools/lint_scenarios.py` | 8-invariant lint utility (not wired to make check yet) |

### Files modified (commit 1)

| Path | Change |
|---|---|
| `c_model/include/axi/scenario_parser.hpp` | Add `Metadata` struct, `Scenario::metadata`, `schema_version` gating, strict-mode validation; wrap field loads with path/field/value context |
| `c_model/tests/axi/CMakeLists.txt` | Register `test_scenario_metadata` |
| `cosim/tests/CMakeLists.txt` | Register `test_wb2axip_block` |
| `CMakeLists.txt` (top-level) | `add_subdirectory(sim/test_patterns)` |

### Files modified (commit 2 — atomic migration)

| Path | Change |
|---|---|
| `sim/test_patterns/AX4-*/scenario.yaml` × 31 | Move from `common/`, `c-model-only/`, `sv-cosim-only/`; add `schema_version: 1` + `metadata:` block |
| `sim/test_patterns/AX4-INF-001_dpi_fatal_on_init_failure/scenario.yaml` | Renamed from `sv-cosim-only/injection_aw_unstable/`; reclassified |
| `sim/test_patterns/AX4-RSP-003_burst_crosses_oob_boundary/data.txt` | New inlined copy (was relative path to `common/burst_incr_2beat/data.txt`) |
| (deleted) `sim/test_patterns/sv-cosim-only/debug_multi1/` | Removed entirely |
| `c_model/tests/axi/test_integration.cpp` | Use `kAllAxi4Scenarios`, load-then-INF-skip ordering, name generator |
| `cosim/tests/test_cosim_integration.cpp` | Renamed from `test_cosim_wire_smoke.cpp`, label `WireSmoke` → `CosimIntegration`, use `kAllAxi4Scenarios` + `wb2axip_block_reason()` |
| `c_model/tests/integration/test_port_pair_loopback.cpp` | String-replace IDs, wrap with `RequireKnownScenario()` |
| `c_model/tests/integration/test_request_response_loopback.cpp` | Same |
| `cosim/tests/test_checker_fires_on_violation.cpp` | Update INF-001 ID |
| `c_model/tests/axi/CMakeLists.txt` | Link `noc_axi4_scenarios` to `test_integration` |
| `cosim/tests/CMakeLists.txt` | Link `noc_axi4_scenarios` to `test_cosim_integration`; rename target |
| `c_model/tests/integration/CMakeLists.txt` | Link `noc_axi4_scenarios` to both port-pair + request-response loopback |
| Top-level Makefile or `CMakeLists.txt` | Hook `tools/lint_scenarios.py` into `make check` |

### Files created (commit 3)

| Path | Responsibility |
|---|---|
| `sim/test_patterns/AX4-BUR-003_incr_len_256/` | New: IHI 0022H §A3.4.1 AxLEN=255 |
| `sim/test_patterns/AX4-BUR-007_wrap_len_2/` | New: IHI 0022H §A3.4.1 WRAP len=1 (2-beat) |
| `sim/test_patterns/AX4-BUR-008_wrap_len_4/` | New: IHI 0022H §A3.4.1 WRAP len=3 (4-beat) |
| `sim/test_patterns/AX4-BUR-009_wrap_len_16/` | New: IHI 0022H §A3.4.1 WRAP len=15 (16-beat) |
| `sim/test_patterns/AX4-BND-007_4kb_boundary_edges/` | New: IHI 0022H §A3.4.1 4 KB boundary exact-end / would-cross |

### Files modified (commit 4)

| Path | Change |
|---|---|
| `sim/test_patterns/README.md` | Full rewrite per spec §6 |

---

## Commit 1 — Infra prep

Add parser strict mode, helpers, CMake scaffold, lint script. **No behavioral
change to existing tests.** Existing scenarios still load via lenient mode
(no `schema_version` field).

### Task 1: Parser — add `Metadata` struct (no validation yet)

**Files:**
- Modify: `c_model/include/axi/scenario_parser.hpp`
- Test: `c_model/tests/axi/test_scenario_metadata.cpp` (new)

- [ ] **Step 1: Write the failing test**

Create `c_model/tests/axi/test_scenario_metadata.cpp`:

```cpp
// Tests for metadata block parsing (schema_version: 1 strict mode).
#include "axi/scenario_parser.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace {

std::string write_tmp_yaml(std::string const& body) {
    auto p = std::filesystem::temp_directory_path() /
             ("scn_meta_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
              ".yaml");
    std::ofstream(p) << body;
    return p.string();
}

TEST(ScenarioMetadata, parses_name_and_category) {
    auto path = write_tmp_yaml(R"(
schema_version: 1
metadata:
  name: AX4-BAS-001_dummy
  category: basic
config:
  memory_base: 0x1000
  memory_size: 0x1000
transactions:
  - { op: write, addr: 0x1000, id: 0, len: 0, size: 5, burst: INCR, data_file: nofile }
)");
    auto sc = ni::cmodel::axi::load_scenario(path);
    EXPECT_EQ(sc.metadata.name, "AX4-BAS-001_dummy");
    EXPECT_EQ(sc.metadata.category, "basic");
}

}  // namespace
```

- [ ] **Step 2: Add file to CMakeLists**

Edit `c_model/tests/axi/CMakeLists.txt` — add a new executable entry mirroring
the existing `test_integration` entry (link gtest_main, set SCENARIO_TREE_ROOT,
add to ctest).

- [ ] **Step 3: Run test to verify it fails**

Run: `cmake --build build && ctest --test-dir build -R ScenarioMetadata -V`
Expected: FAIL — `sc.metadata` undefined / `metadata` field not parsed.

- [ ] **Step 4: Implement Metadata struct + parsing**

In `c_model/include/axi/scenario_parser.hpp`, add after `struct InjectConfig`:

```cpp
struct Metadata {
    std::string name;
    std::string category;
};
```

In `struct Scenario`, add `Metadata metadata;` field (peer of `config` and
`transactions`).

In `load_scenario`, before `if (root["config"])`, add:

```cpp
if (root["metadata"]) {
    auto md = root["metadata"];
    if (md["name"])     sc.metadata.name     = md["name"].as<std::string>();
    if (md["category"]) sc.metadata.category = md["category"].as<std::string>();
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R ScenarioMetadata -V`
Expected: PASS.

- [ ] **Step 6: clang-format**

Run: `clang-format -i c_model/include/axi/scenario_parser.hpp c_model/tests/axi/test_scenario_metadata.cpp`

- [ ] **Step 7: Run existing tests, ensure no regression**

Run: `ctest --test-dir build`
Expected: every test that ran before still passes (existing scenarios load
via lenient mode — no `metadata:` key, no error, sc.metadata fields empty).

### Task 2: Parser — strict mode (schema_version: 1)

**Files:**
- Modify: `c_model/include/axi/scenario_parser.hpp`
- Modify: `c_model/tests/axi/test_scenario_metadata.cpp`

- [ ] **Step 1: Add failing tests for strict mode**

Append to `test_scenario_metadata.cpp`:

```cpp
TEST(ScenarioMetadata, strict_mode_rejects_missing_metadata) {
    auto path = write_tmp_yaml(R"(
schema_version: 1
config: { memory_base: 0x1000, memory_size: 0x1000 }
transactions:
  - { op: write, addr: 0x1000, id: 0, len: 0, size: 5, burst: INCR, data_file: nofile }
)");
    EXPECT_THROW(ni::cmodel::axi::load_scenario(path), std::runtime_error);
}

TEST(ScenarioMetadata, strict_mode_rejects_bad_name_regex) {
    auto path = write_tmp_yaml(R"(
schema_version: 1
metadata: { name: not_axi_pattern, category: basic }
config: { memory_base: 0x1000, memory_size: 0x1000 }
transactions:
  - { op: write, addr: 0x1000, id: 0, len: 0, size: 5, burst: INCR, data_file: nofile }
)");
    EXPECT_THROW(ni::cmodel::axi::load_scenario(path), std::runtime_error);
}

TEST(ScenarioMetadata, strict_mode_rejects_cat_category_mismatch) {
    auto path = write_tmp_yaml(R"(
schema_version: 1
metadata: { name: AX4-BAS-001_dummy, category: burst }
config: { memory_base: 0x1000, memory_size: 0x1000 }
transactions:
  - { op: write, addr: 0x1000, id: 0, len: 0, size: 5, burst: INCR, data_file: nofile }
)");
    EXPECT_THROW(ni::cmodel::axi::load_scenario(path), std::runtime_error);
}

TEST(ScenarioMetadata, lenient_mode_unchanged) {
    // No schema_version, no metadata block — should load fine (legacy path).
    auto path = write_tmp_yaml(R"(
config: { memory_base: 0x1000, memory_size: 0x1000 }
transactions:
  - { op: write, addr: 0x1000, id: 0, len: 0, size: 5, burst: INCR, data_file: nofile }
)");
    auto sc = ni::cmodel::axi::load_scenario(path);
    EXPECT_TRUE(sc.metadata.name.empty());
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir build -R ScenarioMetadata -V`
Expected: 3 new tests FAIL, 1 (lenient_mode_unchanged) PASS.

- [ ] **Step 3: Implement strict mode**

In `scenario_parser.hpp`, after `Scenario sc;` and before the `metadata` block,
add:

```cpp
static const std::regex kNameRegex(
    R"(^AX4-(BAS|BUR|BND|ORD|EXC|RSP|STR|HSH|INF)-\d{3}_[a-z0-9_]+$)");
static const std::map<std::string, std::string> kCatCategory = {
    {"BAS", "basic"},        {"BUR", "burst"},     {"BND", "boundary"},
    {"ORD", "ordering"},     {"EXC", "exclusive"}, {"RSP", "response"},
    {"STR", "stress"},       {"HSH", "handshake"}, {"INF", "infrastructure"},
};

bool strict = false;
if (root["schema_version"]) {
    auto v = root["schema_version"].as<int>();
    if (v != 1) {
        throw std::runtime_error("scenario: unsupported schema_version " +
                                 std::to_string(v));
    }
    strict = true;
}

if (strict && !root["metadata"]) {
    throw std::runtime_error("scenario: schema_version 1 requires metadata block");
}
```

In the existing `if (root["metadata"])` block, after reading name/category, add:

```cpp
if (strict) {
    if (sc.metadata.name.empty() || sc.metadata.category.empty()) {
        throw std::runtime_error("scenario: metadata.name and metadata.category required");
    }
    if (!std::regex_match(sc.metadata.name, kNameRegex)) {
        throw std::runtime_error("scenario: metadata.name '" + sc.metadata.name +
                                 "' does not match AX4-CAT-NNN_slug regex");
    }
    auto cat3 = sc.metadata.name.substr(4, 3);
    auto it = kCatCategory.find(cat3);
    if (it == kCatCategory.end() || it->second != sc.metadata.category) {
        throw std::runtime_error("scenario: metadata.name CAT '" + cat3 +
                                 "' does not match metadata.category '" +
                                 sc.metadata.category + "'");
    }
}
```

Add `#include <map>` and `#include <regex>` at the top of the header.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --test-dir build -R ScenarioMetadata -V`
Expected: all 4 tests PASS.

- [ ] **Step 5: clang-format + regression check**

Run: `clang-format -i c_model/include/axi/scenario_parser.hpp c_model/tests/axi/test_scenario_metadata.cpp`
Run: `ctest --test-dir build`
Expected: all pre-existing tests still PASS (lenient mode preserved).

### Task 3: CMake glob + INTERFACE library

**Files:**
- Create: `sim/test_patterns/CMakeLists.txt`
- Create: `sim/test_patterns/scenarios_list.hpp.in`
- Modify: `CMakeLists.txt` (top-level) — add `add_subdirectory(sim/test_patterns)`

- [ ] **Step 1: Create template header**

`sim/test_patterns/scenarios_list.hpp.in`:

```cpp
#pragma once
#include <array>
#include <string_view>

namespace noc::tests {

inline constexpr std::array<std::string_view, @scenario_count@>
    kAllAxi4Scenarios = {
@scenario_ids@};

}  // namespace noc::tests
```

- [ ] **Step 2: Create CMakeLists.txt**

`sim/test_patterns/CMakeLists.txt`:

```cmake
# Generates scenarios_list.hpp by globbing AX4-* directories.
# CONFIGURE_DEPENDS triggers re-glob when files change.
file(GLOB scenario_yamls CONFIGURE_DEPENDS LIST_DIRECTORIES false
     "${CMAKE_CURRENT_SOURCE_DIR}/AX4-*/scenario.yaml")
list(SORT scenario_yamls)

set(scenario_ids "")
foreach(yaml ${scenario_yamls})
    get_filename_component(dir "${yaml}" DIRECTORY)
    get_filename_component(id  "${dir}" NAME)
    string(APPEND scenario_ids "    \"${id}\",\n")
endforeach()
list(LENGTH scenario_yamls scenario_count)

configure_file(scenarios_list.hpp.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/scenarios_list.hpp"
               @ONLY)

add_library(noc_axi4_scenarios INTERFACE)
target_include_directories(noc_axi4_scenarios INTERFACE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_sources(noc_axi4_scenarios INTERFACE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/scenarios_list.hpp")
```

- [ ] **Step 3: Wire into top-level CMakeLists.txt**

Edit top-level `CMakeLists.txt` — add `add_subdirectory(sim/test_patterns)` near
the other `add_subdirectory(...)` calls for c_model / cosim.

- [ ] **Step 4: Verify generated header exists**

Run: `cmake -S . -B build && cmake --build build`
Run: `ls build/sim/test_patterns/generated/scenarios_list.hpp`
Expected: file exists; `scenario_count` is `0` because no AX4-* dirs exist yet.

Inspect the file:
```cpp
inline constexpr std::array<std::string_view, 0>
    kAllAxi4Scenarios = {
};
```
Expected: empty array, no compile error.

### Task 4: `wb2axip_block_reason()` helper + unit tests

**Files:**
- Create: `cosim/tests/wb2axip_block.hpp`
- Create: `cosim/tests/test_wb2axip_block.cpp`
- Modify: `cosim/tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

`cosim/tests/test_wb2axip_block.cpp`:

```cpp
// Unit tests for wb2axip_block_reason predicate (Codex-audited blockers).
#include "wb2axip_block.hpp"
#include <gtest/gtest.h>

namespace {

using ni::cmodel::axi::Scenario;
using ni::cmodel::axi::ScenarioTransaction;
using ni::cmodel::axi::Burst;
using ni::cmodel::axi::LockType;

Scenario base_scenario() {
    Scenario sc{};
    sc.config.memory_base = 0x1000;
    sc.config.memory_size = 0x1000;
    sc.config.max_outstanding_write = 1;
    sc.config.max_outstanding_read  = 1;
    ScenarioTransaction t{};
    t.op = ScenarioTransaction::Op::Write;
    t.addr = 0x1000;
    t.id = 0;
    t.len = 0;
    t.size = 5;
    t.burst = Burst::INCR;
    t.lock = LockType::Normal;
    sc.transactions.push_back(t);
    return sc;
}

TEST(Wb2axipBlock, accepts_single_beat_single_outstanding) {
    auto sc = base_scenario();
    EXPECT_FALSE(noc::tests::wb2axip_block_reason(sc).has_value());
}

TEST(Wb2axipBlock, blocks_multi_outstanding_write) {
    auto sc = base_scenario();
    sc.config.max_outstanding_write = 4;
    auto r = noc::tests::wb2axip_block_reason(sc);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "WB2AXIP_MAX_OUT_WRITE");
}

TEST(Wb2axipBlock, blocks_multi_beat) {
    auto sc = base_scenario();
    sc.transactions[0].len = 7;
    auto r = noc::tests::wb2axip_block_reason(sc);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "WB2AXIP_MULTI_BEAT");
}

TEST(Wb2axipBlock, blocks_exclusive) {
    auto sc = base_scenario();
    sc.transactions[0].lock = LockType::Exclusive;
    auto r = noc::tests::wb2axip_block_reason(sc);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "WB2AXIP_EXCLUSIVE");
}

TEST(Wb2axipBlock, blocks_inject_mode) {
    auto sc = base_scenario();
    sc.config.inject.mode = ni::cmodel::axi::InjectConfig::Mode::AwUnstable;
    auto r = noc::tests::wb2axip_block_reason(sc);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "INJECTION_DEDICATED_TEST");
}

TEST(Wb2axipBlock, allows_multi_outstanding_read) {
    // Codex Finding 2: wb2axip does NOT block multi-outstanding-READ.
    auto sc = base_scenario();
    sc.config.max_outstanding_read = 4;
    EXPECT_FALSE(noc::tests::wb2axip_block_reason(sc).has_value());
}

TEST(Wb2axipBlock, allows_non_incr_single_beat) {
    // Codex Finding 2: WRAP/FIXED with len=0 are not structurally blocked.
    auto sc = base_scenario();
    sc.transactions[0].burst = Burst::FIXED;
    EXPECT_FALSE(noc::tests::wb2axip_block_reason(sc).has_value());
}

TEST(Wb2axipBlock, allows_oob_addr) {
    // Codex Finding 2: wb2axip is a protocol checker, not address-map predictor.
    auto sc = base_scenario();
    sc.transactions[0].addr = 0xDEADBEEF;
    EXPECT_FALSE(noc::tests::wb2axip_block_reason(sc).has_value());
}

}  // namespace
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir build -R Wb2axipBlock -V`
Expected: build error — `wb2axip_block.hpp` not found.

- [ ] **Step 3: Implement helper**

`cosim/tests/wb2axip_block.hpp`:

```cpp
// Returns a SKIP reason string if this scenario's content violates a known
// wb2axip structural constraint (or names a fault-injection mode reserved
// for dedicated tests). nullopt = wb2axip can in principle run it.
//
// Verified constraints (wb2axip/rtl/faxi_slave.v audit, Codex review):
//   - AWLEN must be 0 (single-beat write)         line 805-807
//   - wr_pending <= 1 (single outstanding write)  line 805-807
//   - No exclusive-access monitor
//
// Not blockers (rejected after source audit):
//   - max_outstanding_read > 1 — wb2axip permits multiple outstanding reads
//   - burst != INCR — wb2axip handles WRAP/FIXED for single beat (len=0);
//     multi-beat variants are already caught by the len>0 check
//   - OOB address — wb2axip is a protocol checker, not address-map predictor
#pragma once
#include "axi/scenario_parser.hpp"
#include <optional>
#include <string>

namespace noc::tests {

inline std::optional<std::string>
wb2axip_block_reason(ni::cmodel::axi::Scenario const& sc) {
    using ni::cmodel::axi::Burst;
    using ni::cmodel::axi::InjectConfig;
    using ni::cmodel::axi::LockType;
    if (sc.config.max_outstanding_write > 1) return "WB2AXIP_MAX_OUT_WRITE";
    if (sc.config.inject.mode != InjectConfig::Mode::None)
        return "INJECTION_DEDICATED_TEST";
    for (auto const& t : sc.transactions) {
        if (t.len > 0)                     return "WB2AXIP_MULTI_BEAT";
        if (t.lock == LockType::Exclusive) return "WB2AXIP_EXCLUSIVE";
    }
    return std::nullopt;
}

}  // namespace noc::tests
```

- [ ] **Step 4: Register the test**

Edit `cosim/tests/CMakeLists.txt` — add an executable entry for
`test_wb2axip_block` mirroring the existing `test_cosim_wire_smoke` block
(link `gtest_main`, no `SCENARIO_TREE_ROOT` needed since this is a unit
test of pure C++ logic). Include path must reach `cosim/tests/` and
`c_model/include/` for `axi/scenario_parser.hpp`.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -R Wb2axipBlock -V`
Expected: all 8 tests PASS.

- [ ] **Step 6: clang-format**

Run: `clang-format -i cosim/tests/wb2axip_block.hpp cosim/tests/test_wb2axip_block.cpp`

### Task 5: `RequireKnownScenario()` helper

**Files:**
- Create: `sim/test_patterns/scenario_helpers.hpp`

This helper has no unit test of its own — its abort behavior is hard to test
in-process without spawning a subprocess. Validation comes from compile-time
membership check in commit 2 when scoped tests use it.

- [ ] **Step 1: Implement helper**

`sim/test_patterns/scenario_helpers.hpp`:

```cpp
#pragma once
#include "scenarios_list.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace noc::tests {

// Aborts the test binary at startup if `id` is not present in
// kAllAxi4Scenarios. Wrap every hand-written scenario reference in scoped
// tests with this so stale IDs after a rename surface as immediate abort,
// not a silent skip.
inline std::string_view RequireKnownScenario(std::string_view id) {
    auto const it = std::find(kAllAxi4Scenarios.begin(),
                              kAllAxi4Scenarios.end(), id);
    if (it == kAllAxi4Scenarios.end()) {
        std::fprintf(stderr,
                     "FATAL: unknown scenario id '%.*s' "
                     "(not in sim/test_patterns/AX4-*)\n",
                     int(id.size()), id.data());
        std::abort();
    }
    return id;
}

}  // namespace noc::tests
```

- [ ] **Step 2: Verify it compiles standalone**

Add to `c_model/tests/axi/test_scenario_metadata.cpp` (temporary smoke):

```cpp
TEST(ScenarioHelpers, helper_compiles) {
    // kAllAxi4Scenarios is empty in commit 1; RequireKnownScenario can't be
    // exercised positively yet — just check the header includes cleanly.
    using noc::tests::kAllAxi4Scenarios;
    EXPECT_EQ(kAllAxi4Scenarios.size(), 0u);
}
```

The test depends on `noc_axi4_scenarios` — link it in
`c_model/tests/axi/CMakeLists.txt`:

```cmake
target_link_libraries(test_scenario_metadata PRIVATE
    gtest_main noc_axi4_scenarios)
target_include_directories(test_scenario_metadata PRIVATE
    "${CMAKE_SOURCE_DIR}/../sim/test_patterns")  # for scenario_helpers.hpp
```

- [ ] **Step 3: Build + run**

Run: `cmake --build build && ctest --test-dir build -R ScenarioHelpers -V`
Expected: PASS, `kAllAxi4Scenarios.size() == 0` confirmed.

- [ ] **Step 4: clang-format**

Run: `clang-format -i sim/test_patterns/scenario_helpers.hpp`

### Task 6: Lint script (standalone, not yet wired)

**Files:**
- Create: `tools/lint_scenarios.py`

- [ ] **Step 1: Implement lint**

`tools/lint_scenarios.py`:

```python
#!/usr/bin/env python3
"""Lint sim/test_patterns/ — 8 invariants per design spec §5.7.

Exits 0 on clean, prints errors and exits 1 on any violation.
"""
import os
import re
import sys
import yaml

ROOT = os.path.join(os.path.dirname(__file__), "..", "tests", "scenarios")
NAME_RE = re.compile(
    r"^AX4-(BAS|BUR|BND|ORD|EXC|RSP|STR|HSH|INF)-\d{3}_[a-z0-9_]+$"
)
CAT_CATEGORY = {
    "BAS": "basic", "BUR": "burst", "BND": "boundary", "ORD": "ordering",
    "EXC": "exclusive", "RSP": "response", "STR": "stress",
    "HSH": "handshake", "INF": "infrastructure",
}

def main() -> int:
    errors: list[str] = []
    seen_names: dict[str, str] = {}
    ax4_count = 0

    for entry in sorted(os.listdir(ROOT)):
        path = os.path.join(ROOT, entry)
        if not os.path.isdir(path):
            continue
        if entry in ("_data",):  # known shared data dir
            continue
        if not entry.startswith("AX4-"):
            # Invariant 1: only AX4-* dirs allowed (after migration).
            errors.append(f"unknown dir: {entry} (expected AX4-CAT-NNN_slug)")
            continue
        ax4_count += 1
        yaml_path = os.path.join(path, "scenario.yaml")
        # Invariant 2: each AX4-* dir has scenario.yaml.
        if not os.path.isfile(yaml_path):
            errors.append(f"{entry}: missing scenario.yaml")
            continue
        # Invariant 3: parses successfully.
        try:
            with open(yaml_path) as f:
                doc = yaml.safe_load(f)
        except Exception as e:
            errors.append(f"{entry}: YAML parse error: {e}")
            continue
        md = doc.get("metadata") or {}
        name = md.get("name", "")
        category = md.get("category", "")
        # Invariant 4: name equals dir basename.
        if name != entry:
            errors.append(f"{entry}: metadata.name '{name}' != dir basename")
        # Invariant 5: name globally unique.
        if name in seen_names:
            errors.append(f"{entry}: duplicate metadata.name; also at {seen_names[name]}")
        else:
            seen_names[name] = entry
        # Invariant 6: name matches regex.
        if name and not NAME_RE.match(name):
            errors.append(f"{entry}: metadata.name '{name}' fails AX4-CAT-NNN_slug regex")
        # Invariant 7: category matches CAT prefix.
        if name and len(name) > 7:
            cat3 = name[4:7]
            expect = CAT_CATEGORY.get(cat3)
            if expect is None:
                errors.append(f"{entry}: unknown CAT prefix '{cat3}'")
            elif expect != category:
                errors.append(
                    f"{entry}: CAT '{cat3}' implies category '{expect}', got '{category}'"
                )

    # Invariant 8: non-empty scenario set.
    # Skipped in commit 1 (zero AX4-* dirs is the legitimate transient state);
    # the lint becomes mandatory in commit 2.
    if "--require-nonempty" in sys.argv and ax4_count == 0:
        errors.append("sim/test_patterns/ contains zero AX4-* dirs")

    if errors:
        for e in errors:
            print(f"LINT: {e}", file=sys.stderr)
        return 1
    print(f"lint: {ax4_count} scenario dirs OK")
    return 0

if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Run standalone (zero scenarios — should pass)**

Run: `py -3 tools/lint_scenarios.py`
Expected: `lint: 0 scenario dirs OK`, exit 0.

Note: invariant 1 (unknown dir error) WILL fire for the existing
`common/`, `c-model-only/`, `sv-cosim-only/`, `cpp-adapter-only/` dirs. This is
expected and intentional — commit 1 does NOT wire lint into `make check`, so
those errors are not currently blocking. They will block in commit 2 after
the migration removes those legacy dirs.

Run: `py -3 tools/lint_scenarios.py 2>&1 | head -20`
Expected: error output listing 4 legacy dirs (common, c-model-only, sv-cosim-only, cpp-adapter-only); exit 1.

### Task 7: Commit 1

- [ ] **Step 1: Run full test suite**

Run: `cmake --build build && ctest --test-dir build`
Expected: all existing tests + 12 new metadata + wb2axip-block tests PASS.

- [ ] **Step 2: Final clang-format pass**

Run:
```
clang-format -i c_model/include/axi/scenario_parser.hpp \
                c_model/tests/axi/test_scenario_metadata.cpp \
                cosim/tests/wb2axip_block.hpp \
                cosim/tests/test_wb2axip_block.cpp \
                sim/test_patterns/scenario_helpers.hpp
```

- [ ] **Step 3: Stage + commit**

```bash
git add c_model/include/axi/scenario_parser.hpp \
        c_model/tests/axi/test_scenario_metadata.cpp \
        c_model/tests/axi/CMakeLists.txt \
        cosim/tests/wb2axip_block.hpp \
        cosim/tests/test_wb2axip_block.cpp \
        cosim/tests/CMakeLists.txt \
        sim/test_patterns/CMakeLists.txt \
        sim/test_patterns/scenarios_list.hpp.in \
        sim/test_patterns/scenario_helpers.hpp \
        tools/lint_scenarios.py \
        CMakeLists.txt

git commit -m "$(cat <<'EOF'
feat(scenarios): parser strict mode + generated list scaffold + wb2axip runtime predicate

Lands all infrastructure for AXI4 pattern standardization round without
touching existing scenarios. Behavior preserved: scenarios without
schema_version load via lenient mode unchanged.

Additions:
- scenario_parser strict mode (schema_version: 1 gated): Metadata struct
  with name/category, AX4-CAT-NNN_slug regex, CAT-category mapping.
- sim/test_patterns/CMakeLists.txt: file(GLOB CONFIGURE_DEPENDS) over
  AX4-*/scenario.yaml; generates scenarios_list.hpp with kAllAxi4Scenarios
  (empty array in commit 1, populated as commit 2 migration lands).
- noc_axi4_scenarios INTERFACE library.
- cosim/tests/wb2axip_block.hpp: 4-condition runtime predicate after
  faxi_slave.v audit (max_outstanding_write>1, len>0, Exclusive, inject).
- sim/test_patterns/scenario_helpers.hpp: RequireKnownScenario aborts on
  unknown id at startup; used by scoped tests in commit 2.
- tools/lint_scenarios.py: 8 invariants (not yet wired to make check).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 4: Verify commit**

Run: `git log --oneline -1`
Expected: shows the new commit.

Run: `git status`
Expected: clean working tree (or only untracked dev files not related to this work).

---

## Commit 2 — Atomic migration

Move 31 existing scenarios to AX4-CAT-NNN_slug, add metadata, delete debug
fixture, reclassify INF-001, rewrite all 5 test files, rename cosim test,
inline cross-scenario data file, hook lint into make check. **All atomic.**

### Task 8: Build rename map (one-time reference)

This task is non-code. Document the rename map for use by tasks 9-13.

**Rename map (sorted by target ID):**

```
common/single_write_no_read              → AX4-BAS-001_single_write_no_read
c-model-only/single_read_default_fill    → AX4-BAS-002_single_read_default_fill
common/single_write_read_aligned         → AX4-BAS-003_single_write_read_aligned
sv-cosim-only/conformity_write_read      → AX4-BAS-004_conformity_write_read
sv-cosim-only/multi_id_single_beat_sequential → AX4-BAS-005_multi_id_single_beat_sequential
common/backpressure_retry                → AX4-HSH-001_backpressure_retry
sv-cosim-only/conformity_backpressure    → AX4-HSH-002_conformity_backpressure
common/burst_incr_2beat                  → AX4-BUR-001_incr_2beat
common/burst_incr_8beat                  → AX4-BUR-002_incr_8beat
c-model-only/fixed_burst                 → AX4-BUR-004_fixed_burst
c-model-only/wrap_burst_aligned          → AX4-BUR-005_wrap_aligned
c-model-only/wrap_burst_actual_wrap      → AX4-BUR-006_wrap_actual_wrap
c-model-only/narrow_transfer_size0       → AX4-BND-001_narrow_transfer_size0
c-model-only/narrow_transfer_size2       → AX4-BND-002_narrow_transfer_size2
c-model-only/narrow_aligned_multibeat    → AX4-BND-003_narrow_aligned_multibeat
c-model-only/unaligned_start             → AX4-BND-004_unaligned_start
c-model-only/sparse_multibeat            → AX4-BND-005_sparse_multibeat
c-model-only/cross_4kb_auto_split        → AX4-BND-006_cross_4kb_auto_split
common/multi_txn_same_id                 → AX4-ORD-001_multi_txn_same_id
common/multi_txn_diff_id                 → AX4-ORD-002_multi_txn_diff_id
c-model-only/exclusive_pair_success      → AX4-EXC-001_exclusive_pair_success
c-model-only/exclusive_no_prior_read     → AX4-EXC-002_exclusive_no_prior_read
c-model-only/exclusive_intervening_write → AX4-EXC-003_exclusive_intervening_write
c-model-only/exclusive_wrap_pair_success → AX4-EXC-004_exclusive_wrap_pair_success
c-model-only/decerr_oob_read             → AX4-RSP-001_decerr_oob_read
c-model-only/decerr_oob_write            → AX4-RSP-002_decerr_oob_write
c-model-only/burst_crosses_oob_boundary  → AX4-RSP-003_burst_crosses_oob_boundary
c-model-only/latency_stress              → AX4-STR-001_latency_stress
c-model-only/multi_outstanding_stress    → AX4-STR-002_multi_outstanding_stress
c-model-only/multi_dst_stress            → AX4-STR-003_multi_dst_stress
sv-cosim-only/injection_aw_unstable      → AX4-INF-001_dpi_fatal_on_init_failure (rename + reclassify)

DELETED: sv-cosim-only/debug_multi1
```

BUR-003 / BUR-007 / BUR-008 / BUR-009 / BND-007 are added in **commit 3**, not
here.

- [ ] **Step 1: Save the rename map as a scratch file**

Run:
```bash
cat > /tmp/rename_map.txt << 'EOF'
[paste the table above]
EOF
```

Used in subsequent tasks for sed-style replacements.

### Task 9: Rename 31 dirs + add metadata block

**Files:**
- Move: 31 scenario directories
- Modify: 31 `scenario.yaml` files (add `schema_version: 1` + `metadata:` block)

For each row in the rename map (31 rows excluding INF-001's slug rename):

- [ ] **Step 1: `git mv` the directory**

Example for BUR-001:
```bash
git mv sim/test_patterns/common/burst_incr_2beat sim/test_patterns/AX4-BUR-001_incr_2beat
```

Repeat for all 31. Use a shell loop reading the rename map for efficiency.

- [ ] **Step 2: Prepend `schema_version` + `metadata` to each YAML**

For each scenario.yaml, prepend at the top:

```yaml
schema_version: 1
metadata:
  name: AX4-<CAT>-<NNN>_<slug>
  category: <category-full-word>

```

Example, for `sim/test_patterns/AX4-BUR-001_incr_2beat/scenario.yaml`:

```yaml
schema_version: 1
metadata:
  name: AX4-BUR-001_incr_2beat
  category: burst

config:
  memory_base: 0x1000
  # ... existing content unchanged
```

Mechanical via per-file Edit using the rename map. CAT→category mapping from
spec §2:

```
BAS → basic       BUR → burst       BND → boundary    ORD → ordering
EXC → exclusive   RSP → response    STR → stress      HSH → handshake
INF → infrastructure
```

- [ ] **Step 3: Verify parser accepts each modified YAML**

Run:
```bash
for d in sim/test_patterns/AX4-*; do
  py -3 -c "import yaml; yaml.safe_load(open('$d/scenario.yaml'))" || echo "FAIL: $d"
done
```
Expected: no FAIL output.

- [ ] **Step 4: Run lint to verify all 31 pass**

Run: `py -3 tools/lint_scenarios.py --require-nonempty`
Expected: errors for `common/`, `c-model-only/`, `sv-cosim-only/`,
`cpp-adapter-only/` (still present); 31 AX4-* dirs OK.

### Task 10: Delete `debug_multi1`

- [ ] **Step 1: Verify it has no references**

Run: `grep -rn "debug_multi1" .`
Expected: hits only in `sv-cosim-only/debug_multi1/` itself and possibly old
test code (the only test that referenced it should be removable).

- [ ] **Step 2: `git rm` the directory**

```bash
git rm -r sim/test_patterns/sv-cosim-only/debug_multi1
```

- [ ] **Step 3: Re-grep to confirm zero hits**

Run: `grep -rn "debug_multi1" .`
Expected: zero matches.

### Task 11: Rename + reclassify INF-001

**Files:**
- Move: `sim/test_patterns/sv-cosim-only/injection_aw_unstable/` → `sim/test_patterns/AX4-INF-001_dpi_fatal_on_init_failure/`
- Modify: the YAML inside

- [ ] **Step 1: `git mv` the directory**

```bash
git mv sim/test_patterns/sv-cosim-only/injection_aw_unstable \
       sim/test_patterns/AX4-INF-001_dpi_fatal_on_init_failure
```

- [ ] **Step 2: Add metadata block to its scenario.yaml**

Prepend:
```yaml
schema_version: 1
metadata:
  name: AX4-INF-001_dpi_fatal_on_init_failure
  category: infrastructure

```

Also update the leading comment to reflect the new intent (it tests DPI fatal
propagation through nonexistent data file, not §A3.2 AW stability).

### Task 12: Inline cross-scenario data file

`burst_crosses_oob_boundary` referenced `../../common/burst_incr_2beat/data.txt`.
After flatten this path is broken (and even if updated, cross-scenario reuse
fights the flat layout per spec §7).

**Files:**
- Modify: `sim/test_patterns/AX4-RSP-003_burst_crosses_oob_boundary/scenario.yaml`
- Create: `sim/test_patterns/AX4-RSP-003_burst_crosses_oob_boundary/data.txt`

- [ ] **Step 1: Copy data file in**

```bash
cp sim/test_patterns/AX4-BUR-001_incr_2beat/data.txt \
   sim/test_patterns/AX4-RSP-003_burst_crosses_oob_boundary/data.txt
```

- [ ] **Step 2: Update YAML to reference local file**

In `AX4-RSP-003_burst_crosses_oob_boundary/scenario.yaml`, change any
`data_file: ../../common/burst_incr_2beat/data.txt` to `data_file: data.txt`.

- [ ] **Step 3: Verify load_scenario still resolves correctly**

Quick test by running c_model integration test (will be migrated in next
task) — defer verification to Task 17 full-build check.

### Task 13: Cleanup legacy dirs

After tasks 9-12, the legacy `common/`, `c-model-only/`, `sv-cosim-only/`,
`cpp-adapter-only/` directories should be empty of scenarios (their contents
moved or deleted).

- [ ] **Step 1: Verify dirs are empty (except non-scenario children)**

Run:
```bash
for d in common c-model-only sv-cosim-only; do
  echo "=== sim/test_patterns/$d ==="
  ls sim/test_patterns/$d 2>/dev/null
done
```
Expected: `common/`, `c-model-only/`, `sv-cosim-only/` may contain `_data/`
(shared data dir for sv-cosim-only) and possibly README files. Cpp-adapter-only
has README only.

- [ ] **Step 2: Move `_data/` out of `sv-cosim-only/` to a neutral location**

The `_data/` dir holds `data_aa.txt` / `data_bb.txt` used by 3 AX4-BAS-* and
AX4-HSH-* scenarios that came from sv-cosim-only/. Move to a flat location:

```bash
git mv sim/test_patterns/sv-cosim-only/_data sim/test_patterns/_data
```

- [ ] **Step 3: Update affected YAMLs to point at new `_data/` path**

For AX4-BAS-004 / AX4-BAS-005 / AX4-HSH-002, change `data_file: ../_data/...` 
to `data_file: ../_data/...` (path becomes `../_data/data_aa.txt` since the
scenarios are now flat at `AX4-*/`).

- [ ] **Step 4: Remove now-empty legacy dirs**

```bash
git rm -r sim/test_patterns/common sim/test_patterns/c-model-only \
          sim/test_patterns/sv-cosim-only sim/test_patterns/cpp-adapter-only
```

- [ ] **Step 5: Update lint to exclude `_data/`**

Edit `tools/lint_scenarios.py` — the `if entry in ("_data",): continue` already
handles this. No code change needed.

- [ ] **Step 6: Run lint, expect clean**

Run: `py -3 tools/lint_scenarios.py --require-nonempty`
Expected: `lint: 32 scenario dirs OK` (31 renames + INF-001), exit 0.

### Task 14: Rewrite `test_integration.cpp`

**Files:**
- Modify: `c_model/tests/axi/test_integration.cpp`
- Modify: `c_model/tests/axi/CMakeLists.txt` (link noc_axi4_scenarios)

- [ ] **Step 1: Replace INSTANTIATE list with kAllAxi4Scenarios**

Replace the existing `INSTANTIATE_TEST_SUITE_P` block (~lines 117-200) with:

```cpp
#include "scenarios_list.hpp"
#include <algorithm>

INSTANTIATE_TEST_SUITE_P(
    AxiFixtures, IntegrationP,
    ::testing::ValuesIn(noc::tests::kAllAxi4Scenarios),
    [](::testing::TestParamInfo<std::string_view> const& info) {
        std::string name{info.param};
        std::replace(name.begin(), name.end(), '-', '_');
        return name;
    });
```

- [ ] **Step 2: Refactor test body — load-then-skip ordering**

The existing test body uses a `FixtureParam` struct with extra fields
(write_data, expect_file_diff_pass, expect_zero_mismatches). Since we now
just pass scenario_id, those expectations move into the test body and derive
from scenario content:

```cpp
TEST_P(IntegrationP, RunsToCompletion) {
    SCENARIO("axi integration: scenario runs to completion under watchdog");
    auto scenario_id = std::string{GetParam()};
    auto scenario_path = std::string(SCENARIO_TREE_ROOT) + scenario_id
                       + "/scenario.yaml";

    // Validate-before-skip: malformed YAML must fail loudly.
    auto sc = ni::cmodel::axi::load_scenario(scenario_path);

    // INF scenarios are reserved for dedicated tests.
    if (scenario_id.compare(0, 8, "AX4-INF-") == 0) {
        GTEST_SKIP() << "INF_DEDICATED_TEST";
    }

    // Run the scenario through the integration stack.
    auto write_data = first_write_data_file(sc);   // "" if no write/dump diff applicable
    std::string rpath =
        std::string(::testing::TempDir()) + "/" + scenario_id + ".read.txt";
    auto r = run_scenario(scenario_path, write_data, rpath);

    EXPECT_EQ(r.scoreboard_mismatches, 0u) << "scoreboard mismatches: " << scenario_id;
    EXPECT_LE(r.cycle_count, kMaxCycles)   << "watchdog tripped: " << scenario_id;
}
```

Where `first_write_data_file(sc)` is a static helper near the top:

```cpp
// Returns the resolved write data_file for the first write txn that has one,
// else empty string. The integration test does an optional file-diff check
// when this is non-empty; backpressure_retry / multi_outstanding produce
// non-deterministic dump order so they intentionally leave data_file but
// the test's expect_file_diff flag is dropped — scoreboard remains the
// authoritative correctness signal.
static std::string first_write_data_file(ni::cmodel::axi::Scenario const& sc) {
    for (auto const& t : sc.transactions) {
        if (t.op == ni::cmodel::axi::ScenarioTransaction::Op::Write &&
            !t.data_file.empty()) {
            return t.data_file;
        }
    }
    return "";
}
```

Drop the old per-scenario file_diff expectation. The scoreboard catches every
real correctness issue.

- [ ] **Step 3: Link noc_axi4_scenarios**

Edit `c_model/tests/axi/CMakeLists.txt`:
```cmake
target_link_libraries(test_integration PRIVATE
    gtest_main noc_axi4_scenarios ...)  # other existing libs preserved
target_include_directories(test_integration PRIVATE
    "${CMAKE_SOURCE_DIR}/../sim/test_patterns")  # for scenario_helpers.hpp if used
```

- [ ] **Step 4: Build + run**

Run: `cmake --build build && ctest --test-dir build -R IntegrationP -V`
Expected: 32 PASS + 1 SKIP (AX4-INF-001 with reason INF_DEDICATED_TEST).

### Task 15: Rename + rewrite cosim integration test

**Files:**
- Rename + Rewrite: `cosim/tests/test_cosim_wire_smoke.cpp` → `cosim/tests/test_cosim_integration.cpp`
- Modify: `cosim/tests/CMakeLists.txt`

- [ ] **Step 1: `git mv` the file**

```bash
git mv cosim/tests/test_cosim_wire_smoke.cpp cosim/tests/test_cosim_integration.cpp
```

- [ ] **Step 2: Rewrite contents**

Replace the entire file:

```cpp
// AXI4 cosim integration: runs each AX4-* scenario through Vtb_top (Verilator
// + wb2axip slave), asserts exit 0 AND PASS marker from tb_top.sv. Peer of
// c_model/tests/axi/test_integration.cpp — same scenario set, different
// execution path. wb2axip-blocked scenarios SKIP with a content-derived
// reason from wb2axip_block_reason(); INF scenarios SKIP by id prefix.
#include "axi/scenario_parser.hpp"
#include "common/scenario.hpp"
#include "scenarios_list.hpp"
#include "wb2axip_block.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <gtest/gtest.h>
#include <string>

namespace {

constexpr const char* kCosimBinaryEnv = "COSIM_BIN";
constexpr const char* kPassMarker = "PASS: scenario complete, scoreboard clean";

struct ProcResult { int rc; std::string output; };

ProcResult run_and_capture(std::string const& cmd) {
    ProcResult r{};
    auto full = cmd + " 2>&1";
#ifdef _WIN32
    FILE* p = _popen(full.c_str(), "r");
#else
    FILE* p = popen(full.c_str(), "r");
#endif
    if (!p) { r.rc = -1; return r; }
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), p)) r.output += buf;
#ifdef _WIN32
    r.rc = _pclose(p);
#else
    r.rc = pclose(p);
#endif
    return r;
}

class CosimIntegration : public ::testing::TestWithParam<std::string_view> {};

TEST_P(CosimIntegration, ScenarioPassesWb2axip) {
    SCENARIO(("cosim integration: " + std::string{GetParam()}).c_str());
    auto scenario_id = std::string{GetParam()};
    auto scenario_path = std::string(SCENARIO_TREE_ROOT) + scenario_id
                       + "/scenario.yaml";

    // Validate-before-skip.
    auto sc = ni::cmodel::axi::load_scenario(scenario_path);

    if (scenario_id.compare(0, 8, "AX4-INF-") == 0) {
        GTEST_SKIP() << "INF_DEDICATED_TEST";
    }
    if (auto reason = noc::tests::wb2axip_block_reason(sc); reason) {
        GTEST_SKIP() << *reason;
    }

    const char* bin = std::getenv(kCosimBinaryEnv);
    ASSERT_NE(bin, nullptr) << "COSIM_BIN env var not set";
    auto cmd = std::string(bin) + " +scenario=" + scenario_path;
    auto result = run_and_capture(cmd);
    EXPECT_EQ(result.rc, 0) << "scenario " << scenario_id
                            << " failed (exit " << result.rc << ")\n"
                            << result.output;
    EXPECT_NE(result.output.find(kPassMarker), std::string::npos)
        << "no PASS marker for " << scenario_id << "\n" << result.output;
}

INSTANTIATE_TEST_SUITE_P(
    CosimIntegration, CosimIntegration,
    ::testing::ValuesIn(noc::tests::kAllAxi4Scenarios),
    [](::testing::TestParamInfo<std::string_view> const& info) {
        std::string name{info.param};
        std::replace(name.begin(), name.end(), '-', '_');
        return name;
    });

}  // namespace
```

- [ ] **Step 3: Update CMakeLists**

Edit `cosim/tests/CMakeLists.txt`:
- Rename target `test_cosim_wire_smoke` → `test_cosim_integration`
- Update source file name
- Link `noc_axi4_scenarios`

- [ ] **Step 4: Build + run**

Run: `cmake --build build && ctest --test-dir build -R CosimIntegration -V`
Expected: 5 PASS (BAS-001/003/004/005, HSH-002, ORD-001) + ~27 SKIP with
specific reasons (WB2AXIP_MULTI_BEAT, WB2AXIP_MAX_OUT_WRITE, WB2AXIP_EXCLUSIVE,
INJECTION_DEDICATED_TEST, INF_DEDICATED_TEST).

### Task 16: Migrate scoped tests (port_pair_loopback)

**Files:**
- Modify: `c_model/tests/integration/test_port_pair_loopback.cpp`
- Modify: `c_model/tests/integration/CMakeLists.txt`

- [ ] **Step 1: Update scenario references via rename map**

Replace string literals using the rename map:
```
"common/burst_incr_8beat"               → "AX4-BUR-002_incr_8beat"
"c-model-only/multi_outstanding_stress" → "AX4-STR-002_multi_outstanding_stress"
"c-model-only/wrap_burst_aligned"       → "AX4-BUR-005_wrap_aligned"
"c-model-only/narrow_aligned_multibeat" → "AX4-BND-003_narrow_aligned_multibeat"
```

- [ ] **Step 2: Wrap each id with `RequireKnownScenario`**

Add `#include "scenario_helpers.hpp"` at the top, then in the INSTANTIATE block:

```cpp
using noc::tests::RequireKnownScenario;

INSTANTIATE_TEST_SUITE_P(PortPairFixtures, PortPairLoopbackP,
    ::testing::Values(
        FixtureParam{std::string{RequireKnownScenario("AX4-BUR-002_incr_8beat")}, 0},
        FixtureParam{std::string{RequireKnownScenario("AX4-STR-002_multi_outstanding_stress")}, 0},
        FixtureParam{std::string{RequireKnownScenario("AX4-BUR-005_wrap_aligned")}, 0},
        FixtureParam{std::string{RequireKnownScenario("AX4-BND-003_narrow_aligned_multibeat")}, 0},
        FixtureParam{std::string{RequireKnownScenario("AX4-BUR-002_incr_8beat")}, 2},
        FixtureParam{std::string{RequireKnownScenario("AX4-STR-002_multi_outstanding_stress")}, 3}),
    /* name_gen */ ...);
```

The name generator at the bottom of the existing INSTANTIATE — keep it, but
the `rfind('/')` slash-strip no longer applies (no slash in new IDs). Update
the name generator to hyphen-replacement instead:

```cpp
[](const ::testing::TestParamInfo<FixtureParam>& info) {
    std::string n = info.param.yaml;
    std::replace(n.begin(), n.end(), '-', '_');
    return n + "_d" + std::to_string(info.param.delay_cycles);
}
```

- [ ] **Step 3: Update CMakeLists to link**

Edit `c_model/tests/integration/CMakeLists.txt`:
```cmake
target_link_libraries(test_port_pair_loopback PRIVATE
    noc_axi4_scenarios ...)  # existing libs preserved
target_include_directories(test_port_pair_loopback PRIVATE
    "${CMAKE_SOURCE_DIR}/../sim/test_patterns")
```

- [ ] **Step 4: Build + run**

Run: `cmake --build build && ctest --test-dir build -R PortPair -V`
Expected: all variants PASS.

### Task 17: Migrate scoped tests (request_response_loopback)

**Files:**
- Modify: `c_model/tests/integration/test_request_response_loopback.cpp`

Apply the same pattern as Task 16: string-replace IDs, wrap with
`RequireKnownScenario`, update name generator. The Fixtures + MultiVc
INSTANTIATE blocks both need updating (28 entries total — mechanical).

- [ ] **Step 1: String-replace all old paths**

Mapping for this file:
```
"common/burst_incr_8beat"                → "AX4-BUR-002_incr_8beat"
"c-model-only/multi_outstanding_stress"  → "AX4-STR-002_multi_outstanding_stress"
"c-model-only/wrap_burst_aligned"        → "AX4-BUR-005_wrap_aligned"
"c-model-only/narrow_aligned_multibeat"  → "AX4-BND-003_narrow_aligned_multibeat"
"c-model-only/sparse_multibeat"          → "AX4-BND-005_sparse_multibeat"
"c-model-only/multi_dst_stress"          → "AX4-STR-003_multi_dst_stress"
```

- [ ] **Step 2: Wrap with RequireKnownScenario + update name generator**

Same pattern as Task 16. Add `#include "scenario_helpers.hpp"` and wrap each
literal.

- [ ] **Step 3: Build + run**

Run: `cmake --build build && ctest --test-dir build -R PacketizeLoopback -V`
Expected: all variants PASS.

### Task 18: Migrate scoped test (checker_fires_on_violation)

**Files:**
- Modify: `cosim/tests/test_checker_fires_on_violation.cpp`

- [ ] **Step 1: Update INF-001 reference**

Replace `"sv-cosim-only/injection_aw_unstable"` →
`"AX4-INF-001_dpi_fatal_on_init_failure"`. Wrap with `RequireKnownScenario` and
add the include.

- [ ] **Step 2: Build + run**

Run: `cmake --build build && ctest --test-dir build -R CheckerFires -V`
Expected: PASS (this test expects the cosim binary to exit non-zero — fault
propagation still works since INF-001's content is unchanged).

### Task 19: Hook lint into `make check`

**Files:**
- Modify: `Makefile` or top-level `CMakeLists.txt` (whichever defines `make check`)

- [ ] **Step 1: Locate `make check` definition**

Run: `grep -rn "check:" Makefile CMakeLists.txt 2>/dev/null`

- [ ] **Step 2: Add lint invocation**

In the `check` target, prepend:
```makefile
check: lint_scenarios
	$(MAKE) -C build test

lint_scenarios:
	py -3 tools/lint_scenarios.py --require-nonempty
```

(Adjust syntax to actual project Makefile / CMake structure.)

- [ ] **Step 3: Verify clean**

Run: `make check`
Expected: lint prints `lint: 32 scenario dirs OK`, then ctest runs and passes.

### Task 20: Full regression + Commit 2

- [ ] **Step 1: Full clean rebuild**

```bash
rm -rf build
cmake -S . -B build
cmake --build build
```
Expected: clean build, no warnings about missing scenarios or stale paths.

- [ ] **Step 2: Run all tests + lint**

Run: `make check`
Expected:
- Lint: 32 scenario dirs OK
- c_model tests: ~all PASS including 31 IntegrationP fixtures (INF-001 SKIP)
- cosim tests: 5 PASS + ~27 SKIP in CosimIntegration; PASS for CheckerFires; PASS for Wb2axipBlock unit
- Scoped tests: PortPair + PacketizeLoopback all PASS with new IDs

- [ ] **Step 3: clang-format pass**

```bash
clang-format -i c_model/tests/axi/test_integration.cpp \
                cosim/tests/test_cosim_integration.cpp \
                c_model/tests/integration/test_port_pair_loopback.cpp \
                c_model/tests/integration/test_request_response_loopback.cpp \
                cosim/tests/test_checker_fires_on_violation.cpp
```

- [ ] **Step 4: Stage everything**

```bash
git add sim/test_patterns/ \
        c_model/tests/axi/test_integration.cpp \
        c_model/tests/axi/CMakeLists.txt \
        c_model/tests/integration/test_port_pair_loopback.cpp \
        c_model/tests/integration/test_request_response_loopback.cpp \
        c_model/tests/integration/CMakeLists.txt \
        cosim/tests/test_cosim_integration.cpp \
        cosim/tests/test_checker_fires_on_violation.cpp \
        cosim/tests/CMakeLists.txt \
        Makefile  # or wherever lint hookup lives
```

- [ ] **Step 5: Commit**

```bash
git commit -m "$(cat <<'EOF'
refactor(scenarios): migrate to AX4-CAT-NNN_slug + lint + test rewrites + cosim test rename

Atomic migration of 31 existing scenarios to flat-by-ID AX4-CAT-NNN_slug
layout, delete debug_multi1, reclassify+rename injection_aw_unstable to
AX4-INF-001_dpi_fatal_on_init_failure. Add schema_version: 1 + metadata
block to every scenario. Inline cross-scenario data file for
AX4-RSP-003. Activate tools/lint_scenarios.py in make check.

Rename map:
  common/single_write_no_read              -> AX4-BAS-001_single_write_no_read
  c-model-only/single_read_default_fill    -> AX4-BAS-002_single_read_default_fill
  common/single_write_read_aligned         -> AX4-BAS-003_single_write_read_aligned
  sv-cosim-only/conformity_write_read      -> AX4-BAS-004_conformity_write_read
  sv-cosim-only/multi_id_single_beat_sequential -> AX4-BAS-005_multi_id_single_beat_sequential
  common/backpressure_retry                -> AX4-HSH-001_backpressure_retry
  sv-cosim-only/conformity_backpressure    -> AX4-HSH-002_conformity_backpressure
  common/burst_incr_2beat                  -> AX4-BUR-001_incr_2beat
  common/burst_incr_8beat                  -> AX4-BUR-002_incr_8beat
  c-model-only/fixed_burst                 -> AX4-BUR-004_fixed_burst
  c-model-only/wrap_burst_aligned          -> AX4-BUR-005_wrap_aligned
  c-model-only/wrap_burst_actual_wrap      -> AX4-BUR-006_wrap_actual_wrap
  c-model-only/narrow_transfer_size0       -> AX4-BND-001_narrow_transfer_size0
  c-model-only/narrow_transfer_size2       -> AX4-BND-002_narrow_transfer_size2
  c-model-only/narrow_aligned_multibeat    -> AX4-BND-003_narrow_aligned_multibeat
  c-model-only/unaligned_start             -> AX4-BND-004_unaligned_start
  c-model-only/sparse_multibeat            -> AX4-BND-005_sparse_multibeat
  c-model-only/cross_4kb_auto_split        -> AX4-BND-006_cross_4kb_auto_split
  common/multi_txn_same_id                 -> AX4-ORD-001_multi_txn_same_id
  common/multi_txn_diff_id                 -> AX4-ORD-002_multi_txn_diff_id
  c-model-only/exclusive_pair_success      -> AX4-EXC-001_exclusive_pair_success
  c-model-only/exclusive_no_prior_read     -> AX4-EXC-002_exclusive_no_prior_read
  c-model-only/exclusive_intervening_write -> AX4-EXC-003_exclusive_intervening_write
  c-model-only/exclusive_wrap_pair_success -> AX4-EXC-004_exclusive_wrap_pair_success
  c-model-only/decerr_oob_read             -> AX4-RSP-001_decerr_oob_read
  c-model-only/decerr_oob_write            -> AX4-RSP-002_decerr_oob_write
  c-model-only/burst_crosses_oob_boundary  -> AX4-RSP-003_burst_crosses_oob_boundary
  c-model-only/latency_stress              -> AX4-STR-001_latency_stress
  c-model-only/multi_outstanding_stress    -> AX4-STR-002_multi_outstanding_stress
  c-model-only/multi_dst_stress            -> AX4-STR-003_multi_dst_stress
  sv-cosim-only/injection_aw_unstable      -> AX4-INF-001_dpi_fatal_on_init_failure (rename + reclassify)
  DELETED: sv-cosim-only/debug_multi1

Test rewrites:
- c_model/tests/axi/test_integration.cpp: kAllAxi4Scenarios + INF skip
- cosim/tests/test_cosim_integration.cpp: renamed from test_cosim_wire_smoke.cpp,
  uses kAllAxi4Scenarios + wb2axip_block_reason() runtime predicate
- 3 scoped tests (port_pair_loopback, request_response_loopback,
  checker_fires_on_violation): IDs updated, wrapped with RequireKnownScenario

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 6: Verify**

Run: `git log --oneline -2 && make check`
Expected: commit visible, full make check clean.

---

## Commit 3 — Add 5 spec-gap patterns

Each pattern is a fresh `AX4-*` directory with `scenario.yaml` and any needed
data files. Adding them automatically populates `kAllAxi4Scenarios` via the
CMake glob; both c_model and cosim integration tests pick them up on next
build. No C++ edits required.

Cite IHI 0022H § in each YAML's leading comment so the spec trace is
preserved.

### Task 21: AX4-BUR-003_incr_len_256

**Files:**
- Create: `sim/test_patterns/AX4-BUR-003_incr_len_256/scenario.yaml`
- Create: `sim/test_patterns/AX4-BUR-003_incr_len_256/data.txt`

- [ ] **Step 1: Write scenario.yaml**

```yaml
# IHI 0022H §A3.4.1: AxLEN = 255 (256-beat INCR burst, max valid length).
# Each beat is size=5 (32 bytes). Total transfer = 8192 bytes; memory_size
# enlarged accordingly.
schema_version: 1
metadata:
  name: AX4-BUR-003_incr_len_256
  category: burst

config:
  memory_base: 0x1000
  memory_size: 0x4000
  write_latency: 1
  read_latency: 1
transactions:
  - { op: write, addr: 0x1000, id: 0, len: 255, size: 5, burst: INCR, data_file: data.txt }
  - { op: read,  addr: 0x1000, id: 0, len: 255, size: 5, burst: INCR, dump_file: unused }
```

- [ ] **Step 2: Generate data.txt (256 lines, 32 hex bytes each)**

Use a one-liner:
```bash
py -3 -c "
for i in range(256):
    print(' '.join(f'{(i + j) & 0xFF:02x}' for j in range(32)))
" > sim/test_patterns/AX4-BUR-003_incr_len_256/data.txt
```

- [ ] **Step 3: Verify load + lint**

Run: `py -3 tools/lint_scenarios.py --require-nonempty`
Expected: includes BUR-003 in count.

Run: `cmake --build build && ctest --test-dir build -R BUR_003 -V`
Expected: c_model PASS, cosim SKIP with WB2AXIP_MULTI_BEAT.

### Task 22: AX4-BUR-007_wrap_len_2

**Files:**
- Create: `sim/test_patterns/AX4-BUR-007_wrap_len_2/scenario.yaml`
- Create: `sim/test_patterns/AX4-BUR-007_wrap_len_2/data.txt`

- [ ] **Step 1: Write scenario.yaml**

```yaml
# IHI 0022H §A3.4.1: WRAP burst, AxLEN=1 (2-beat). Wrap boundary equals
# 2 * (1<<size) = 64 bytes. Start addr aligned to size and within wrap
# region.
schema_version: 1
metadata:
  name: AX4-BUR-007_wrap_len_2
  category: burst

config:
  memory_base: 0x1000
  memory_size: 0x1000
  write_latency: 1
  read_latency: 1
transactions:
  - { op: write, addr: 0x1000, id: 0, len: 1, size: 5, burst: WRAP, data_file: data.txt }
  - { op: read,  addr: 0x1000, id: 0, len: 1, size: 5, burst: WRAP, dump_file: unused }
```

- [ ] **Step 2: Generate data.txt (2 lines)**

```bash
py -3 -c "
for i in range(2):
    print(' '.join(f'{(i*16 + j) & 0xFF:02x}' for j in range(32)))
" > sim/test_patterns/AX4-BUR-007_wrap_len_2/data.txt
```

- [ ] **Step 3: Verify**

Run: `cmake --build build && ctest --test-dir build -R BUR_007 -V`
Expected: c_model PASS, cosim SKIP with WB2AXIP_MULTI_BEAT.

### Task 23: AX4-BUR-008_wrap_len_4

**Files:**
- Create: `sim/test_patterns/AX4-BUR-008_wrap_len_4/scenario.yaml`
- Create: `sim/test_patterns/AX4-BUR-008_wrap_len_4/data.txt`

- [ ] **Step 1: Write scenario.yaml**

```yaml
# IHI 0022H §A3.4.1: WRAP burst, AxLEN=3 (4-beat). Wrap boundary 128 bytes.
schema_version: 1
metadata:
  name: AX4-BUR-008_wrap_len_4
  category: burst

config:
  memory_base: 0x1000
  memory_size: 0x1000
  write_latency: 1
  read_latency: 1
transactions:
  - { op: write, addr: 0x1000, id: 0, len: 3, size: 5, burst: WRAP, data_file: data.txt }
  - { op: read,  addr: 0x1000, id: 0, len: 3, size: 5, burst: WRAP, dump_file: unused }
```

- [ ] **Step 2: Generate data.txt (4 lines)**

```bash
py -3 -c "
for i in range(4):
    print(' '.join(f'{(i*16 + j) & 0xFF:02x}' for j in range(32)))
" > sim/test_patterns/AX4-BUR-008_wrap_len_4/data.txt
```

- [ ] **Step 3: Verify**

Run: `cmake --build build && ctest --test-dir build -R BUR_008 -V`
Expected: c_model PASS, cosim SKIP.

### Task 24: AX4-BUR-009_wrap_len_16

**Files:**
- Create: `sim/test_patterns/AX4-BUR-009_wrap_len_16/scenario.yaml`
- Create: `sim/test_patterns/AX4-BUR-009_wrap_len_16/data.txt`

- [ ] **Step 1: Write scenario.yaml**

```yaml
# IHI 0022H §A3.4.1: WRAP burst, AxLEN=15 (16-beat). Wrap boundary 512 bytes.
schema_version: 1
metadata:
  name: AX4-BUR-009_wrap_len_16
  category: burst

config:
  memory_base: 0x1000
  memory_size: 0x1000
  write_latency: 1
  read_latency: 1
transactions:
  - { op: write, addr: 0x1000, id: 0, len: 15, size: 5, burst: WRAP, data_file: data.txt }
  - { op: read,  addr: 0x1000, id: 0, len: 15, size: 5, burst: WRAP, dump_file: unused }
```

- [ ] **Step 2: Generate data.txt (16 lines)**

```bash
py -3 -c "
for i in range(16):
    print(' '.join(f'{(i*16 + j) & 0xFF:02x}' for j in range(32)))
" > sim/test_patterns/AX4-BUR-009_wrap_len_16/data.txt
```

- [ ] **Step 3: Verify**

Run: `cmake --build build && ctest --test-dir build -R BUR_009 -V`
Expected: c_model PASS, cosim SKIP.

### Task 25: AX4-BND-007_4kb_boundary_edges

**Files:**
- Create: `sim/test_patterns/AX4-BND-007_4kb_boundary_edges/scenario.yaml`
- Create: `sim/test_patterns/AX4-BND-007_4kb_boundary_edges/data.txt`

- [ ] **Step 1: Write scenario.yaml**

```yaml
# IHI 0022H §A3.4.1: 4KB boundary edges. Two transactions: one ending exactly
# at the 4KB boundary (no split needed), one starting one beat short of
# crossing (also no split). Both INCR.
#
# burst1: addr=0x0F80, len=3, size=5  -> 4 beats * 32 = 128 bytes ending at 0x0FFF
# burst2: addr=0x1000, len=3, size=5  -> next 4KB region, same pattern
schema_version: 1
metadata:
  name: AX4-BND-007_4kb_boundary_edges
  category: boundary

config:
  memory_base: 0x0F80
  memory_size: 0x2000
  write_latency: 1
  read_latency: 1
transactions:
  - { op: write, addr: 0x0F80, id: 0, len: 3, size: 5, burst: INCR, data_file: data.txt }
  - { op: write, addr: 0x1000, id: 0, len: 3, size: 5, burst: INCR, data_file: data.txt }
  - { op: read,  addr: 0x0F80, id: 0, len: 3, size: 5, burst: INCR, dump_file: unused }
  - { op: read,  addr: 0x1000, id: 0, len: 3, size: 5, burst: INCR, dump_file: unused }
```

- [ ] **Step 2: Generate data.txt (4 lines, reused by both writes)**

```bash
py -3 -c "
for i in range(4):
    print(' '.join(f'{(i*16 + j) & 0xFF:02x}' for j in range(32)))
" > sim/test_patterns/AX4-BND-007_4kb_boundary_edges/data.txt
```

- [ ] **Step 3: Verify**

Run: `cmake --build build && ctest --test-dir build -R BND_007 -V`
Expected: c_model PASS, cosim SKIP.

### Task 26: Commit 3

- [ ] **Step 1: Full make check**

Run: `make check`
Expected: 36 scenarios lint clean; c_model has 36 IntegrationP fixtures
(35 PASS + 1 INF SKIP); cosim has 36 CosimIntegration fixtures (5 PASS +
31 SKIP including 5 newly added wb2axip-blocked WRAP/multi-beat).

- [ ] **Step 2: Stage**

```bash
git add sim/test_patterns/AX4-BUR-003_incr_len_256 \
        sim/test_patterns/AX4-BUR-007_wrap_len_2 \
        sim/test_patterns/AX4-BUR-008_wrap_len_4 \
        sim/test_patterns/AX4-BUR-009_wrap_len_16 \
        sim/test_patterns/AX4-BND-007_4kb_boundary_edges
```

- [ ] **Step 3: Commit**

```bash
git commit -m "$(cat <<'EOF'
feat(scenarios): add 5 IHI 0022H spec-gap patterns

All five fill gaps in IHI 0022H §A3.4.1 coverage:

  AX4-BUR-003_incr_len_256        AxLEN=255 (max INCR length)
  AX4-BUR-007_wrap_len_2          WRAP len=1 (2-beat)
  AX4-BUR-008_wrap_len_4          WRAP len=3 (4-beat)
  AX4-BUR-009_wrap_len_16         WRAP len=15 (16-beat)
  AX4-BND-007_4kb_boundary_edges  4KB boundary exact-end + would-cross

Picked up automatically by kAllAxi4Scenarios glob; both c_model and
cosim integration tests register without any C++ change. Cosim side
SKIPs all 5 (wb2axip multi-beat block) until wb2axip is replaced.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Commit 4 — README rewrite

### Task 27: Rewrite `sim/test_patterns/README.md`

**Files:**
- Modify: `sim/test_patterns/README.md`

- [ ] **Step 1: Replace contents**

`sim/test_patterns/README.md`:

````markdown
# sim/test_patterns — AXI4 Scenario Tree

Single source of truth for AXI4 scenario YAMLs. Both c_model integration test
(`c_model/tests/axi/test_integration.cpp`) and cosim integration test
(`cosim/tests/test_cosim_integration.cpp`) consume the full set via a
CMake-generated header. Three scoped tests (`test_port_pair_loopback`,
`test_request_response_loopback`, `test_checker_fires_on_violation`) consume
hand-curated subsets.

## Naming convention — `AX4-CAT-NNN_slug`

| Code | Category | Scope |
|---|---|---|
| `BAS` | basic         | Basic serialized single-beat transfers |
| `HSH` | handshake     | Handshake stall, backpressure (IHI 0022H §A3.2) |
| `BUR` | burst         | INCR / WRAP / FIXED burst type and length (§A3.4.1) |
| `BND` | boundary      | Alignment, narrow transfer, 4 KB boundary (§A3.4.1) |
| `ORD` | ordering      | Multi-ID ordering (§A5, §A6) |
| `EXC` | exclusive     | Exclusive access (§A7.2.4) |
| `RSP` | response      | Error response — DECERR/SLVERR (§A3.4.5) |
| `STR` | stress        | Stress / concurrency |
| `INF` | infrastructure | Non-AXI4-spec; testbench / DPI / bringup fixtures |

ID format: `AX4-<CAT>-<NNN>_<slug>` where NNN is 3-digit zero-padded sequence
number within category (e.g. `AX4-BUR-002_incr_8beat`). IDs are not stable —
deleting a scenario renumbers later siblings; commit messages include the
rename map.

## YAML schema

```yaml
schema_version: 1                          # required
metadata:                                  # required
  name: AX4-BUR-002_incr_8beat             # equals parent directory basename
  category: burst                          # CAT prefix must agree

config:                                    # AXI scenario config (see scenario_parser.hpp)
  memory_base: 0x1000
  memory_size: 0x1000
  write_latency: 1
  read_latency: 1
  max_outstanding_write: 1                 # optional, default 1
  max_outstanding_read: 1                  # optional, default 1
  inject: { mode: aw_unstable, cycle: 1 }  # optional, INF-only

transactions:
  - op: write
    addr: 0x1000
    id: 0x5
    len: 7
    size: 5
    burst: INCR
    data_file: data.txt
```

Each scenario directory contains `scenario.yaml` plus per-beat `data.txt`
(and optionally `strb.txt`, `excl.txt`, indexed `data_<n>.txt`). `data_file:`
and friends use bare filenames; `scenario_parser` resolves them relative to
the scenario's own directory.

## Test layer consumption

| Test | List source | Skips |
|---|---|---|
| `c_model/tests/axi/test_integration.cpp` | `kAllAxi4Scenarios` | INF prefix |
| `cosim/tests/test_cosim_integration.cpp` | `kAllAxi4Scenarios` | INF prefix + `wb2axip_block_reason()` runtime predicate |
| `c_model/tests/integration/test_port_pair_loopback.cpp` | Curated 4 scenarios × delay sweep | n/a |
| `c_model/tests/integration/test_request_response_loopback.cpp` | Curated 7 scenarios × num_vc variants | n/a |
| `cosim/tests/test_checker_fires_on_violation.cpp` | INF-001 only | n/a |

`kAllAxi4Scenarios` is generated at CMake configure time from
`sim/test_patterns/AX4-*/scenario.yaml` via `file(GLOB CONFIGURE_DEPENDS)`.
Adding a new pattern automatically propagates to both run-all tests on the
next build.

`wb2axip_block_reason()` (in `cosim/tests/wb2axip_block.hpp`) inspects each
scenario's parsed content against wb2axip's structural limits and returns a
SKIP reason on hit. No skip map is maintained. When wb2axip is replaced with
a full AXI4 BFM, deleting the helper body activates all previously skipped
scenarios.

## Adding a new scenario

1. Pick CAT + next NNN; create `sim/test_patterns/AX4-CAT-NNN_slug/`
2. Write `scenario.yaml` with `schema_version: 1` and full `metadata:` block
3. Add `data.txt` (and any other data files referenced)
4. Run `make check` — lint + both integration tests pick it up automatically
5. If cosim SKIPs the new pattern with `WB2AXIP_*`, that's expected — wb2axip
   doesn't model that case. No action needed; SKIP is documentation
6. Commit with a body citing the IHI 0022H § or VIP test the scenario was
   derived from

## Reference: IHI 0022H sections covered per category

| CAT | IHI § |
|---|---|
| BAS | §A3.2 (basic VALID/READY) |
| HSH | §A3.2 (handshake stalls) |
| BUR | §A3.4.1 (burst type, length, size) |
| BND | §A3.4.1 (alignment, 4KB boundary) |
| ORD | §A5, §A6 (ID-based ordering) |
| EXC | §A7.2.4 (exclusive access) |
| RSP | §A3.4.5 (response codes) |
| STR | §A5 (multi-outstanding traffic) |
| INF | (none — testbench infrastructure) |
````

- [ ] **Step 2: Commit**

```bash
git add sim/test_patterns/README.md
git commit -m "$(cat <<'EOF'
docs(scenarios): rewrite README for AX4-CAT-NNN_slug + CMake-glob sharing

Replaces the layered (common/c-model-only/sv-cosim-only) explanation with
the AX4-CAT-NNN_slug naming + category enum + IHI 0022H § mapping. Documents
the kAllAxi4Scenarios CMake-glob sharing mechanism and wb2axip_block_reason()
runtime predicate. Adds the "add a new scenario" workflow.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 3: Final verification**

Run: `git log --oneline -4`
Expected: 4 commits (parser scaffold, atomic migration, 5 patterns, README).

Run: `make check`
Expected: everything still passes — README is doc-only.

---

## Self-Review checklist (run after writing)

- [x] Every spec section maps to a task (§2 naming → Task 1+2 parser + Task 9 YAML; §3 schema → Task 1+2; §4 migration → Task 8-13; §5 sharing → Task 3-5 + Task 14-18; §6 commit batching → 4 commit boundaries; §7 wb2axip retire = future, INF future = future, cross-file inline → Task 12)
- [x] No "TBD" / "TODO" / "Add appropriate error handling" placeholders
- [x] Type / function names consistent (kAllAxi4Scenarios, wb2axip_block_reason, RequireKnownScenario used identically across all references)
- [x] Each task has explicit file paths, commands, and exact code

## What this plan does NOT do

- It does not write `tools/lint_scenarios.py` to validate `config` / `transactions` schema beyond `metadata`. The C++ parser remains the authoritative schema validator. Lint is intentionally metadata-focused (per Codex Finding 8 of Section 3 round).
- It does not change `scenario_parser`'s behavior on legacy YAMLs (no `schema_version` field) — those continue to load unchanged. Future rounds may tighten this.
- It does not address tests beyond the 5 listed consumer test files. If another test starts consuming `sim/test_patterns/` after this round, it must adopt `RequireKnownScenario` or join the run-all population.

## Out of scope (deferred to later rounds)

- `scenario_parser` extension for SLVERR range, mid-sim reset, R-latency randomizer (spec §6 medium/large)
- Port cocotbext-axi tests or tim_axi4_vip subset
- Cocotbext-axi-style parameterized sweep fixture infrastructure
- wb2axip replacement with full AXI4 BFM
