# AXI4 Pattern Standardization — Design Spec

**Date:** 2026-06-07
**Status:** Design — ready for plan writing
**Branch:** `stage5b/dpi-wire-wrap`
**Companion research:** [2026-06-07-axi-pattern-standardization-research.md](2026-06-07-axi-pattern-standardization-research.md)
**Reviewers:** Claude (main), Codex (per-section)

---

## 1. Scope & non-goals

### In scope (this round)

- Rename 31 existing scenarios + 1 reclassification + 1 deletion → flat-by-ID layout
  `sim/test_patterns/AX4-CAT-NNN_slug/` (no layer sub-dirs)
- Add 5 new scenarios from IHI 0022H §A3.4.1 spec gap
- YAML schema additions: `schema_version: 1`, `metadata.name`, `metadata.category`
- `scenario_parser.hpp` strict mode gated by `schema_version`
- Generated `scenarios_list.hpp` via CMake glob + `CONFIGURE_DEPENDS`
- Runtime `wb2axip_block_reason()` helper (cosim side) that inspects each
  scenario's parsed content against wb2axip's structural constraints and
  SKIPs the test with a content-derived reason string. No maintained skip
  map. INF scenarios detected by ID prefix.
- Lint utility `tools/lint_scenarios.py` (8 invariants)
- Rewrite `sim/test_patterns/README.md`

### Non-goals (deferred to later rounds)

- `scenario_parser` extension for §6 medium/large patterns (SLVERR range, mid-sim
  reset, R-latency randomizer)
- Port cocotbext-axi tests (full table is structurally limited — 5 functional
  cells with parameter sweeps, net new ≈ 0 against current 32 scenarios)
- Port tim_axi4_vip subset (154 UVM tests) — next round candidate
- Cocotbext-axi-style parameterized sweep fixture infrastructure (`run_test_write`,
  `run_test_read`) — after router complete + NoC integration phase
- Replacement of wb2axip slave with a full AXI4 BFM (separate infra round)

### Deferred until router complete + NoC integration phase

- Multi-master / bus matrix scenarios
- CDC (no async-FIFO infrastructure)
- Multi-NSU integration coverage beyond current `multi_dst_stress`

### Permanently out of scope

- AXI3 features (write-data interleaving, locked transactions)
- AXI4 atomics (AtomicStore / Load / Swap / Compare)
- ACE / AXI5
- Power-domain isolation, REGION-based decode

---

## 2. Naming convention — `AX4-CAT-NNN_slug`

### Format

```
AX4-<CAT>-<NNN>_<slug>
```

- `AX4` — fixed prefix (AXI4 spec coverage marker)
- `CAT` — 3-letter category code (see enum below)
- `NNN` — 3-digit zero-padded sequence number within category, starting `001`
- `_slug` — `snake_case`, full-word (no abbreviation), describes specific case

### Category enum

| Code | Full name | Scope |
|---|---|---|
| `BAS` | `basic` | Basic serialized single-beat transfers |
| `HSH` | `handshake` | Handshake stall, backpressure |
| `BUR` | `burst` | INCR / WRAP / FIXED burst type and length |
| `BND` | `boundary` | Alignment, narrow transfer, 4 KB boundary |
| `ORD` | `ordering` | Multi-ID ordering |
| `EXC` | `exclusive` | Exclusive access |
| `RSP` | `response` | Error response (DECERR / SLVERR) |
| `STR` | `stress` | Stress / concurrency |
| `INF` | `infrastructure` | Non-AXI4-spec; testbench / DPI / bringup fixtures |

`CAT`↔`category` mapping is canonical; parser validates that the `CAT` prefix
in `metadata.name` agrees with the `metadata.category` value.

### Identity contract

- Directory basename === `metadata.name` === canonical identity
- YAML file is always `scenario.yaml`; never appears in identity
- Rename slug = rename identity (commit message must include mapping row)
- Cross-category rename = ID change (renumber-on-delete policy applies)

### Renumber policy

- IDs are **not stable**. Deleting `AX4-BUR-002` causes `AX4-BUR-003` to become
  `AX4-BUR-002`, and so on.
- Commit message that renames or renumbers must include a mapping table.
- Within-category ordering follows IHI 0022H spec narrative order (simpler →
  more complex within a section).

### Decisions captured

- **Codex Finding 1 (research brief) opaque-key alternative rejected** —
  renumber-on-delete already accepts ID non-stability; opaque IDs lose `CAT`
  grep-ability without buying real stability.
- **Codex Finding 6 (Section 3 round) `id` → `name` rename adopted** — avoids
  overloading the transaction-level AXI `id` field; aligns with Kubernetes
  precedent (`metadata.name`).

---

## 3. YAML schema

### Top-level layout

```yaml
schema_version: 1                            # required (enables strict parser mode)
metadata:                                    # required block
  name: AX4-BUR-002_incr_8beat               # full dir basename
  category: burst                            # full-word enum value

config:                                      # existing, unchanged
  memory_base: 0x1000
  memory_size: 0x1000
  write_latency: 1
  read_latency: 1
  max_outstanding_write: 1
  max_outstanding_read: 1
  inject: { mode: aw_unstable, cycle: 1 }    # optional, unchanged

transactions:                                # existing, unchanged
  - op: write
    addr: 0x1000
    id: 0x5
    len: 7
    size: 5
    burst: INCR
    data_file: data.txt
```

### Field contract

| Field | Type | Validation |
|---|---|---|
| `schema_version` | int | Required for new round; supported values: `1`. Missing = legacy lenient mode (existing ad-hoc YAMLs unaffected). |
| `metadata.name` | string | Regex `^AX4-(BAS\|BUR\|BND\|ORD\|EXC\|RSP\|STR\|HSH\|INF)-\d{3}_[a-z0-9_]+$`; must equal parent directory basename. |
| `metadata.category` | string | Enum {`basic`, `burst`, `boundary`, `ordering`, `exclusive`, `response`, `stress`, `handshake`, `infrastructure`}; `CAT` prefix mapping must agree. |
| `config.*` | unchanged | Existing validation in `scenario_parser` retained. |
| `transactions[*].*` | unchanged | Existing validation retained. |

`metadata.consumers` field considered and **rejected** — patterns are spec-coverage
artifacts independent of any checker / BFM / harness; infrastructure constraints
belong in test code, not pattern metadata.

### Parser changes

`c_model/include/axi/scenario_parser.hpp`:

1. Add root-level key whitelist: `schema_version`, `metadata`, `config`,
   `transactions`. Unknown root key = throw.
2. Add `Metadata` struct, store in `Scenario`.
3. When `schema_version: 1` present → strict mode (metadata required, all
   field validations enforced). Absent → today's behaviour (existing tests
   not in scenario tree continue to load).
4. Each metadata field load wrapped to report `path:field:value` on failure.
5. Existing `config` / `transactions` field loads also wrapped for consistent
   error context (Codex Finding 7 — opportunistic cleanup).

---

## 4. Migration

### Final mapping (36 scenarios after one delete, one reclassify, 5 additions)

```
BAS (5) — basic serialized single-beat transfers
  BAS-001 single_write_no_read
  BAS-002 single_read_default_fill
  BAS-003 single_write_read_aligned
  BAS-004 conformity_write_read
  BAS-005 multi_id_single_beat_sequential     ← moved from ORD

HSH (2) — handshake stall / backpressure
  HSH-001 backpressure_retry
  HSH-002 conformity_backpressure

BUR (9) — burst type / length
  BUR-001 incr_2beat
  BUR-002 incr_8beat
  BUR-003 incr_len_256                        [NEW] IHI 0022H §A3.4.1
  BUR-004 fixed_burst
  BUR-005 wrap_aligned
  BUR-006 wrap_actual_wrap
  BUR-007 wrap_len_2                          [NEW] IHI 0022H §A3.4.1
  BUR-008 wrap_len_4                          [NEW] IHI 0022H §A3.4.1
  BUR-009 wrap_len_16                         [NEW] IHI 0022H §A3.4.1

BND (7) — alignment / boundary
  BND-001 narrow_transfer_size0
  BND-002 narrow_transfer_size2
  BND-003 narrow_aligned_multibeat
  BND-004 unaligned_start
  BND-005 sparse_multibeat
  BND-006 cross_4kb_auto_split                ← moved from BUR
  BND-007 4kb_boundary_edges                  [NEW] IHI 0022H §A3.4.1

ORD (2) — multi-ID ordering
  ORD-001 multi_txn_same_id
  ORD-002 multi_txn_diff_id

EXC (4) — exclusive access
  EXC-001 exclusive_pair_success
  EXC-002 exclusive_no_prior_read
  EXC-003 exclusive_intervening_write
  EXC-004 exclusive_wrap_pair_success

RSP (3) — error response
  RSP-001 decerr_oob_read
  RSP-002 decerr_oob_write
  RSP-003 burst_crosses_oob_boundary

STR (3) — stress / concurrency
  STR-001 latency_stress
  STR-002 multi_outstanding_stress
  STR-003 multi_dst_stress

INF (1) — infrastructure (non-AXI4-spec)
  INF-001 dpi_fatal_on_init_failure           ← renamed from injection_aw_unstable
                                              ← reclassified from HSH

DELETED: debug_multi1                         (ad-hoc fixture, no spec reference)
```

### Reconciliation

- 32 existing − 1 (`debug_multi1`) + 5 new = 36
- Cross-category moves: `multi_id_single_beat_sequential` (ORD→BAS),
  `cross_4kb_auto_split` (BUR→BND), `4kb_boundary_edges` not applicable (new),
  `injection_aw_unstable` (HSH→INF)
- Slug renames: `injection_aw_unstable` → `dpi_fatal_on_init_failure`

### Within-category ordering rationale

IHI 0022H spec narrative order: simpler concepts first, then more complex.

| Category | Ordering principle |
|---|---|
| BAS | write-only → read-only → write-then-read → L1-tuned variant → multi-ID sequential |
| HSH | backpressure recovery first, L1-tuned variant after |
| BUR | INCR (lengths ascending) → FIXED → WRAP (positions, lengths ascending) |
| BND | narrow (size ascending) → unaligned → sparse → 4KB cases |
| ORD | same-ID → different-ID |
| EXC | canonical pair → no-prior-read → intervening-clear → wrap variant |
| RSP | read OOB → write OOB → burst-crosses |
| STR | single-txn latency → multi-outstanding → multi-destination |
| INF | n/a (single scenario) |

### Codex review findings adopted

1. `read_after_write_same_addr` removed from new patterns — already covered by
   BAS-003 (`single_write_read_aligned`).
2. `injection_aw_unstable` reclassified — its actual purpose is DPI fatal
   propagation infrastructure, not AXI4 §A3.2 handshake stability. Slug renamed
   to reflect actual behaviour; `INF` category introduced.
3. `multi_id_single_beat_sequential` moved from ORD to BAS — single outstanding
   does not exercise ordering; closer to basic ID-handling coverage.
4. `cross_4kb_auto_split` and `4kb_boundary_edges` moved to BND — 4 KB boundary
   is alignment-class, not burst-type-class.
5. Within-category ordering corrections (FIXED before WRAP; narrow before
   unaligned; `exclusive_no_prior_read` before `exclusive_intervening_write`;
   `latency_stress` before multi-outstanding).

---

## 5. Sharing mechanism

### 5.1 Generated header

New file `sim/test_patterns/CMakeLists.txt`:

```cmake
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

Template `sim/test_patterns/scenarios_list.hpp.in`:

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

Top-level `CMakeLists.txt` adds `add_subdirectory(sim/test_patterns)`.

### 5.2 Runtime skip predicate (no maintained map)

Rather than enumerate every wb2axip-blocked scenario in a hand-maintained
map, the cosim integration test inspects each scenario's parsed content
against wb2axip's structural limits at runtime. Adding a new pattern requires
zero infrastructure changes — if the pattern violates a known wb2axip
constraint, the helper auto-detects and the test SKIPs with a specific reason
string. Retiring wb2axip means deleting the helper file (or its body); all
auto-derived SKIPs vanish on the next test run.

New file `cosim/tests/wb2axip_block.hpp`:

```cpp
#pragma once
#include "axi/scenario_parser.hpp"
#include <optional>
#include <string>

namespace noc::tests {

// Returns a SKIP reason string if this scenario's content violates a known
// wb2axip structural constraint (or names a fault-injection mode that
// belongs in a dedicated test). nullopt = wb2axip can in principle run it.
//
// Verified constraints (wb2axip/rtl/faxi_slave.v audit, Codex review):
//   - AWLEN must be 0 (single-beat write)         line 805-807
//   - wr_pending <= 1 (single outstanding write)  line 805-807
//   - No exclusive-access monitor
//
// Not blockers (rejected from earlier draft after source audit):
//   - max_outstanding_read > 1 — wb2axip permits multiple outstanding reads
//   - burst != INCR — wb2axip handles WRAP/FIXED for single beat (len=0);
//     multi-beat WRAP/FIXED are already caught by the len>0 check
//   - OOB address (DECERR) — wb2axip is a protocol checker, not an
//     address-map predictor; DECERR responses are legal AXI4
//
// Fault injection (inject.mode != None) belongs in dedicated tests
// (e.g. test_checker_fires_on_violation), not run-all suites.
inline std::optional<std::string>
wb2axip_block_reason(ni::cmodel::axi::Scenario const& sc) {
    using namespace ni::cmodel::axi;
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

INF-class scenarios are detected by ID prefix (one-line check in each
run-all test, no helper needed):

```cpp
if (scenario_id.substr(0, 8) == "AX4-INF-") {
    GTEST_SKIP() << "INF_DEDICATED_TEST";
}
```

INF scenarios are reserved for their dedicated tests (e.g.
`test_checker_fires_on_violation` owns INF-001); run-all tests skip them
regardless of layer.

### 5.3 Test layer classification

Five existing test files split into two categories:

| Category | File | List source |
|---|---|---|
| Run-all | `c_model/tests/axi/test_integration.cpp` | `kAllAxi4Scenarios` + INF-prefix SKIP |
| Run-all | `cosim/tests/test_cosim_integration.cpp` (renamed from `test_cosim_wire_smoke.cpp`) | `kAllAxi4Scenarios` + INF-prefix SKIP + `wb2axip_block_reason()` SKIP |
| Scoped | `c_model/tests/integration/test_port_pair_loopback.cpp` | Curated list × delay sweep |
| Scoped | `c_model/tests/integration/test_request_response_loopback.cpp` | Curated list × num_vc variant |
| Scoped | `cosim/tests/test_checker_fires_on_violation.cpp` | INF-001 only |

Run-all tests use `kAllAxi4Scenarios` (automatic discovery via CMake glob);
scoped tests keep curated lists but reference scenarios by new AX4 IDs.

**Terminology — "smoke" dropped.** The legacy `test_cosim_wire_smoke.cpp`
filename historically reflected the small subset of wb2axip-compatible
scenarios. Under this design, the cosim-side test is the **peer of**
`c_model/tests/axi/test_integration.cpp` — both register the full
`kAllAxi4Scenarios`. The cosim side additionally consults
`wb2axip_block_reason()` at runtime so wb2axip-blocked rows SKIP with a
content-derived reason. The file is renamed to `test_cosim_integration.cpp`
in commit 2. The SKIPPED rows in cosim CI output are an audit trail of
wb2axip's structural blockers, not noise; they disappear automatically
when wb2axip is replaced and the helper body is removed.

### 5.4 Run-all test body shape

c_model side (`test_integration.cpp`):

```cpp
TEST_P(IntegrationP, RunsToCompletion) {
    auto scenario_id = std::string{GetParam()};
    auto scenario_path = std::string(SCENARIO_TREE_ROOT) + scenario_id
                       + "/scenario.yaml";

    // Validate-before-skip: broken YAML must fail loudly, not hide behind skip.
    // load_scenario throws (with path:field:value context) on schema violation.
    auto sc = ni::cmodel::axi::load_scenario(scenario_path);

    // After successful load, INF scenarios are reserved for dedicated tests.
    if (scenario_id.compare(0, 8, "AX4-INF-") == 0) {
        GTEST_SKIP() << "INF_DEDICATED_TEST";
    }

    // Run scenario.
    auto r = run_scenario(sc, ...);
    EXPECT_EQ(r.scoreboard_mismatches, 0u);
}

INSTANTIATE_TEST_SUITE_P(
    AxiFixtures, IntegrationP,
    ::testing::ValuesIn(noc::tests::kAllAxi4Scenarios),
    [](auto const& info) {
        std::string name{info.param};
        std::replace(name.begin(), name.end(), '-', '_');
        return name;
    });
```

cosim side (`test_cosim_integration.cpp`) follows the same load-first
ordering, then calls the wb2axip helper:

```cpp
TEST_P(CosimIntegrationP, ScenarioPassesWb2axip) {
    auto scenario_id = std::string{GetParam()};
    auto scenario_path = std::string(SCENARIO_TREE_ROOT) + scenario_id
                       + "/scenario.yaml";

    // Load + schema-validate first.
    auto sc = ni::cmodel::axi::load_scenario(scenario_path);

    // INF scenarios reserved for dedicated tests.
    if (scenario_id.compare(0, 8, "AX4-INF-") == 0) {
        GTEST_SKIP() << "INF_DEDICATED_TEST";
    }

    // wb2axip structural blockers (max_outstanding_write>1, len>0,
    // Exclusive, inject mode).
    if (auto reason = noc::tests::wb2axip_block_reason(sc); reason) {
        GTEST_SKIP() << *reason;
    }

    // shell out to Vtb_top binary, capture stdout+stderr, assert PASS marker
    // (existing mechanism from test_cosim_wire_smoke.cpp)
    ...
}
```

### 5.5 (removed — runtime predicate replaces enumerated skip map)

No per-test skip map is maintained. `wb2axip_block_reason()` derives the
SKIP reason from scenario content; INF is detected by ID prefix. Adding a
new pattern requires no skip-map edit.

### 5.6 Scoped tests — `RequireKnownScenario` helper

New file `sim/test_patterns/scenario_helpers.hpp`:

```cpp
#pragma once
#include "scenarios_list.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace noc::tests {

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

Used in scoped test instantiation:

```cpp
using noc::tests::RequireKnownScenario;

INSTANTIATE_TEST_SUITE_P(PortPairFixtures, PortPairLoopbackP,
    ::testing::Values(
        FixtureParam{RequireKnownScenario("AX4-BUR-002_incr_8beat"), 0},
        FixtureParam{RequireKnownScenario("AX4-STR-002_multi_outstanding_stress"), 0},
        // ...
    ),
    fixture_name_gen);
```

Stale IDs after a rename abort the test binary on startup instead of producing
silent skips.

### 5.7 Lint utility — `tools/lint_scenarios.py`

Runs as part of `make check`. Eight invariants:

1. Every non-`AX4-*` dir under `sim/test_patterns/` is an error (catches malformed
   names CMake glob would miss).
2. Every `AX4-*` dir has a `scenario.yaml`.
3. Every `scenario.yaml` parses + passes schema validation (full schema, not
   just metadata — broken YAML hidden behind skip otherwise).
4. `metadata.name` equals parent directory basename.
5. `metadata.name` is globally unique across repo.
6. `metadata.name` matches `AX4-CAT-NNN_slug` regex.
7. `metadata.category` value agrees with `CAT` prefix.
8. `kAllAxi4Scenarios` non-empty (zero tests = configuration error).

Skip-map invariants (8, 9 of the previous draft) are no longer needed —
there is no skip map to lint. The cosim `wb2axip_block_reason()` helper is
plain C++ inspected by compiler; INF detection is a one-line prefix check.

---

## 6. Commit batching + README

### Four commits

| # | Subject | Atomic with |
|---|---|---|
| 1 | `feat(scenarios): parser strict mode + generated list scaffold + wb2axip runtime predicate` | self-contained — adds parser fields, CMake glob template, helper headers; **no lint hookup yet** (would reject legacy dirs). Existing layered scenarios still load via lenient mode (no `schema_version` field). |
| 2 | `refactor(scenarios): migrate 31 existing to AX4-CAT-NNN_slug + lint + test rewrites + cosim test rename` | must atomic — ~30 dir moves + add metadata to each + activate lint in `make check` + 5 cpp file rewrites (including `test_cosim_wire_smoke.cpp` → `test_cosim_integration.cpp` + `INSTANTIATE_TEST_SUITE_P` label `WireSmoke` → `CosimIntegration`); intermediate states fail |
| 3 | `feat(scenarios): add 5 spec-gap patterns (BUR-003/007/008/009, BND-007)` | self-contained on top of commit 2 — each new scenario is a fresh AX4-* dir + YAML + data, no edits to existing scenarios; tests pick up via `kAllAxi4Scenarios` automatically |
| 4 | `docs(scenarios): rewrite sim/test_patterns/README.md` | independent |

Each commit individually compiles, runs `make check` clean, and contains
tests for any new functionality. Commit 2 is the only large commit by
necessity (rename + lint activation + test path updates are coupled).
Commit 2's commit message includes the full rename map. Commit 3 ships
the 5 spec-gap patterns separately so review can focus on each pattern's
spec citation.

### README rewrite outline

`sim/test_patterns/README.md` sections:

1. Naming convention `AX4-CAT-NNN_slug` (with category enum + IHI § mapping)
2. YAML schema (`schema_version` / `metadata.{name, category}` / `config` /
   `transactions`)
3. Test layer consumption (run-all vs scoped; `kAllAxi4Scenarios` +
   `wb2axip_block_reason()` runtime predicate for cosim)
4. Adding a new scenario (mkdir → YAML + data → `make check` → cosim
   integration will auto-classify via `wb2axip_block_reason()`; no skip-map
   edit needed)
5. Reference: IHI 0022H sections covered per category

Removed from old README:

- `common/` / `c-model-only/` / `sv-cosim-only/` layer description (flattened
  layout)
- Cross-scenario reuse via relative path example (`burst_crosses_oob_boundary`
  used `../../common/burst_incr_2beat/data.txt` — flatten changes the relative
  path; this round either updates the path or inlines the data file)

---

## 7. Open considerations

### INF category future review

`INF` currently holds one scenario (`dpi_fatal_on_init_failure`). If INF grows
to 5+ scenarios spanning multiple infrastructure concerns (DPI, simulator
lifecycle, c++ adapter, transport), split out into a layer-owned suite —
`kAllAxi4SpecScenarios` (BAS / BUR / BND / ORD / EXC / RSP / STR / HSH) vs
`kInfrastructureScenarios` (INF). Per-layer permanent INF skips would otherwise
turn into hidden negative-consumers metadata.

### wb2axip replacement workflow

When wb2axip is replaced with a full AXI4 BFM:

1. Delete `cosim/tests/wb2axip_block.hpp` (or empty its body so it returns
   `std::nullopt` for everything).
2. Build cosim binary. All previously skipped scenarios now run.
3. Treat resulting failures as BFM integration work (not as regressions of
   this round).
4. If the new BFM has its own structural limits, write an analogous
   `new_bfm_block_reason()` helper next to the test.

### Cross-scenario data file reuse

`burst_crosses_oob_boundary` currently references
`../../common/burst_incr_2beat/data.txt`. After the flatten, the relative path
either needs updating (`../AX4-BUR-001_incr_2beat/data.txt`) or the data file
should be inlined into the new scenario directory. Commit 2 inlines the data
file (simpler, no relative-path fragility).

---

## 8. References

### Spec

- ARM IHI 0022H AMBA AXI4 Protocol Specification

### Industry conventions surveyed

- Kubernetes manifest convention — `metadata.name` precedent
- OpenAPI 3 `info` block — metadata-block precedent
- pytest skip / xfail — skip-with-reason precedent
- GoogleTest parameterized tests — `INSTANTIATE_TEST_SUITE_P` + name generator
- CMake `file(GLOB CONFIGURE_DEPENDS)` — manifest generation precedent
- tim_axi4_vip (154-test UVM AXI4 VIP) — category taxonomy reference
- cocotbext-axi — surveyed and found insufficient for port-based scaling

### Companion docs

- [2026-06-07-axi-pattern-standardization-research.md](2026-06-07-axi-pattern-standardization-research.md) — research brief that seeded this design

### Codex review rounds

Per-section Codex review applied at sections 2, 3, 4, 4-fixup (user pivot on
consumers semantics), 5. All adopted findings noted inline.
