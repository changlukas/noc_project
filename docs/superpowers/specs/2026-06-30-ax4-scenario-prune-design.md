# AX4 scenario prune — standard-aligned, de-duplicated

**Goal:** Cut AI-generated / duplicate AX4 scenarios that carry no marginal AXI-stimulus coverage, keep
the industry-standard set, renumber each family gap-free, then re-run the co-sim regression so triage
sees only real fabric bugs.

## Survey basis

Standard AXI4 verification coverage points, cross-checked against the AMBA AXI4 spec and three
reference VIPs ([tim_axi4_vip](https://github.com/moonslide/tim_axi4_vip),
[cocotbext-axi](https://github.com/alexforencich/cocotbext-axi),
[OSVVM/AXI4](https://github.com/OSVVM/AXI4)):

burst type FIXED / INCR / WRAP (WRAP aligned addr, len ∈ {2,4,8,16}); narrow transfer; unaligned
address + auto-split; 4KB boundary split; WSTRB sparse; exclusive access (success / fail-no-monitor /
fail-intervening); multiple outstanding + reorder across IDs; in-order same-ID / OoO diff-ID; QoS;
error response DECERR.

The 37-scenario taxonomy maps cleanly to these. The defect is within-family duplication plus a few
non-AXI / wrong-layer scenarios — not the taxonomy.

## Key distinction: stimulus vs environment

An AXI scenario is **master stimulus** — the AW/W/AR/B/R control info the master drives
(`addr/id/len/size/burst/lock/qos` + data/strb). It is NOT the subordinate's response timing. Two
scenarios with identical transactions are the same AXI pattern even if other config differs.

`config.write_latency` / `config.read_latency` are **not** stimulus. They flow
`scenario_parser` → `SlaveConfig` → `axi::Memory(write_lat_ticks, read_lat_ticks)`, which holds the
B/R response for N ticks before OKAY (`c_model/tests/axi/test_memory.cpp:29`). In co-sim,
`cmodel_dpi.cpp:602-612` passes them into `SlaveWrap.init(...)` — they configure the **slave endpoint
model's** wait-state behavior. Slave-side response latency is a real verification knob, but it belongs
on a **testbench axis** (sweep a base scenario's slave latency, like `rob_modes`), not bundled into
duplicate scenario files. See Out of scope.

## Decision: hybrid — standard ∩ marginal value

Keep every scenario mapping to a cited standard coverage point AND adding marginal value over its
siblings as **AXI stimulus**. Cut scenarios whose transactions duplicate another's, or that relabel a
basic transaction with a "stress"/"backpressure" name carried only by the slave-latency knob.

## Delete (12 — no marginal stimulus coverage)

**Stimulus duplicates (identical transactions; differ only in a non-stimulus field):**

| delete | same stimulus as | difference | evidence |
|---|---|---|---|
| `BUR-007` wrap_len_2 | `BUR-005` wrap_aligned | id only | same addr/len/size/burst |
| `BAS-004` conformity_write_read | `BAS-003` | id only | same single write+read |
| `HSH-001` backpressure_retry | `ORD-002` | slave `write_latency` 5 vs 1 | same 4 diff-id writes (0x1-0x4) |
| `HSH-002` conformity_backpressure | `BAS-003` | slave latency 5/5 vs 1/1 | same single write+read |
| `STR-001` latency_stress | `BAS-003` | slave latency 20/20 vs 1/1 | same single write+read |

The HSH/STR-001 deltas are the slave-latency knob (environment, not stimulus). As AXI patterns they
duplicate ORD-002 / BAS-003.

**Non-AXI / wrong layer:** `INF-001` (DPI-fatal infra, not AXI); `BAS-001` (write-only half-step, not a
coverage point); `BAS-002` (reads default-fill, tests model behavior not protocol).

**Standard but redundant within family:** `BUR-008/009` (aligned WRAP never wraps → degenerates to
INCR; lengths already covered by INCR; the only real wrap is `BUR-006`); `BND-002` (narrow size2 single
⊂ `BND-003` size2 multibeat); `EXC-004` (exclusive+WRAP — a genuine stimulus combo, but exclusive is
covered by `EXC-001/002/003` and WRAP by `BUR-005/006`; cut on marginal-value grounds, not as a dup).

## Reclassify (1)

`STR-003` multi_dst_stress → `ORD-003`. The only same-ID-different-dst ordering case (exercises the
ROB-Disabled stall path `ORD-001` does not). It is ordering, not stress. Set `category: ordering`; keep
`address_mode: dependent` (addr encodes dst).

## Keep on disk, Layer-2 only (not wire-verifiable)

`QOS-001` (AWQOS write-only), `RSP-001/002/003` (error response, intentional DECERR). Already skipped by
`run_regress.is_self_checking` (response category / no read op); kept on disk for the Layer-2 c_model
suite. Numbers unchanged (no gap within family).

## Keep in wire matrix (21)

`BAS-003/005` · `BUR-001..006` · `BND-001/003/004/005/006/007` · `EXC-001/002/003` · `ORD-001/002` +
reclassified `ORD-003` · `STR-002`. Excluded fabric-bug carriers (`BUR-003@rob`, `BND-006`, `ORD-002`)
stay — real coverage exposing real bugs.

## Renumber (gap-free per family)

Applied after delete + reclassify. Touches the directory name and `metadata.name` (descriptive suffix
kept). BUR (001-006), EXC (001-003), ORD (001-003), QOS, RSP are already contiguous — no change.

| family | old → new |
|---|---|
| BAS | 003→001, 005→002 |
| BND | 001→001, 003→002, 004→003, 005→004, 006→005, 007→006 |
| STR | 002→001 |

Old→new recorded for traceability: prior backlog rows and git history use the old ids. Notably the 4KB
fabric-bug carrier `BND-006` (cross_4kb_auto_split) → `BND-005`, and `BND-007` (4kb_boundary_edges) →
`BND-006`.

## Reference updates (delete + renumber fallout)

The scenario list (`sim/test_patterns/CMakeLists.txt`) globs `AX4-*` with `CONFIGURE_DEPENDS`, so the
Layer-2 gtest list auto-updates. But hard-coded ids exist and must be updated:

- **`c_model/tests/wrap/CMakeLists.txt:49`** — `CMODEL_TEST_SCENARIO_YAML` points at deleted
  `AX4-BAS-001`. Repoint to the kept `AX4-BAS-001` (= old `BAS-003`, single write+read — a better
  fixture than the old write-only one).
- **`c_model/tests/integration/test_request_response_loopback.cpp`** + **`test_port_pair_loopback.cpp`**
  — `RequireKnownScenario(...)` hard-codes `STR-002`→`STR-001`, `BND-003`→`BND-002`,
  `BND-005`→`BND-004`, and `STR-003`→`ORD-003` (incl. the `:135` path substring
  `"AX4-STR-003_multi_dst_stress/"`).
- **`sim/regress/test_run_regress.py`** — `BAS-003`→`BAS-001`, `BAS-005`→`BAS-002`, `BND-007`→`BND-006`,
  `STR-002`→`STR-001`.
- **`sim/regress/matrix.yaml`** exclusions — `BND-006`→`BND-005`, `BND-007`→`BND-006`; BAS-005 stimulus
  line → `BAS-002`. `BUR-003` / `ORD-002` unchanged. `all_independent_ax4` / `all_dependent_ax4`
  auto-expand — no list edit.
- **`sim/tools/`** (`run_benchmark.py`, `gen_test_patterns.py`, their tests) + `c_model/tests/common/`
  fixtures referencing renumbered ids — audit and update each.
- **`docs/backlog.md`** — replace the stale strict-15 plan with this hybrid result; update scenario-id
  references; record the old→new map.

`test_scenario_metadata.cpp`'s `AX4-BAS-001_dummy` strings are inline parser fixtures (a made-up name),
unrelated to the deleted directory — no change.

The exact line edits are enumerated in the implementation plan.

## Verification

Re-run `make sim-regress BUILD=mesh_4x4_vc1`. The deleted `HSH-001` fail disappears. Expected triage
set: `BUR-003` (all patterns, non-rob), `BUR-002`@hotspot, `STR-002`(→`STR-001`)@neighbor real fabric
bugs, plus still-excluded `ORD-002` hang. Fixing those is a later round, not this prune.

## Out of scope

- **Slave-latency testbench axis.** If slave-side backpressure coverage is wanted, add it as a matrix
  axis sweeping a base scenario's `write_latency`/`read_latency` (analogous to `rob_modes`), not as
  duplicate scenario files. Deferred; record in backlog.
- Root-causing the fabric bugs (`BUR-003` / `BUR-002` / `STR-002` / `ORD-002`) — triage only.
- Regenerating proper actual-wrap WRAP coverage at len 8/16 (current set has only one real wrap,
  `BUR-006` len4). Note it; do not add scenarios this round.
- Layer-2 gtest changes for QOS/RSP.
