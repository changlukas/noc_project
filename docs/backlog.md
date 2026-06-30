# NoC backlog

Running action items and open bugs, maintained across iteration rounds. Each round adds what it
surfaces and strikes what it closes. Read it at session start. An item is not started unless a round
picks it up.

## Bugs

### Pre-existing fabric bugs (the matrix caught these, which is its purpose)

Each is excluded in `sim/regress/matrix.yaml` with a reason and re-included once the fabric bug is
fixed.

| id | symptom | suspected root cause | status |
|---|---|---|---|
| `AX4-ORD-002` | multi-id concurrent write (ids 0x1-0x4, `max_outstanding_write: 4`) hangs to the 100k-cycle co-sim timeout. Reproduces without preserve_addr. | RAW-release / NSU per-id response path | excluded |
| `AX4-BND-006` | 4KB-crossing burst at `0x0FE0` (`len:7`, `size:5`): write OKAY, read phase hangs under 16-node load. NMU 4KB auto-split works for write, read-split does not. | NMU 4KB read-split under concurrent load | excluded |
| `AX4-BND-007` | same boundary-edge class. Manual single-cell check was inconclusive (data-file relpath artifact); excluded preemptively until first full run confirms. | same as BND-006 (unconfirmed) | excluded (matrix.yaml) |

The first full `make sim-regress` is a discovery run. Sweeping the curated set through the
concurrent 16-node fabric will surface more pre-existing co-sim bugs. Add each to `matrix.yaml`
exclusions with a reason as it is confirmed.

**Discovery run — `mesh_4x4_vc1`, 2026-06-30** (first end-to-end run after the build decouple; 74
cells executed, 64 pass / 10 fail). New fails beyond the excluded set:

| id | failing patterns | note | survives prune? |
|---|---|---|---|
| `AX4-BUR-003` | neighbor / uniform_random / transpose / hotspot (all 4, non-rob) | len 256; rob already excluded, non-rob fails on every pattern -> scenario-level fabric bug | yes (burst) |
| `AX4-HSH-001` | all 4 patterns | backpressure/retry, traffic-independent | no (HSH deleted) |
| `AX4-BUR-002` | hotspot only | other 3 pass -> hotspot congestion | yes (burst) |
| `AX4-STR-002` | neighbor only | outstanding stress | yes (outstanding) |

After the prune the real-bug worklist is `BUR-003` (all patterns + rob exclusion), `BUR-002`@hotspot,
`STR-002`@neighbor, plus the still-excluded `ORD-002` hang. The `HSH-001` fails leave with the HSH delete.

### ~~Confirmed design bug — per-id VC binding~~ RESOLVED 2026-06-30

Fixed on branch `feat/vc-id-agnostic` (merged to main local, commits `253b744..e39d8bd`; spec
`docs/superpowers/specs/2026-06-30-vc-binding-removal-design.md`, plan `docs/superpowers/plans/2026-06-30-vc-id-agnostic.md`).
Root-cause confirmed: the cited "spec §8" does not exist (architecture.md ends at §7); the NMU pin was a
self-added cross-transaction mechanism. RTL-verified FlooNoC alignment (floo_vc_arbiter round-robin,
floo_rob arrival-offset, R bursts on one VC). Outcome: VC selection id-agnostic round-robin by class;
same-id ordering = Enabled RoB (rob_idx) / Disabled single-outstanding interlock; NSU R-burst follow
kept (renamed `r_burst_vc_`, burst coherence not an id pin); unit-test-only MultiCandidate mode deleted.
ctest 522/522. Codex final review: 0 correctness findings.

**NOT yet verified — moved to next round (pattern/co-sim):** the `ORD-002` hang hypothesis (binding was
the root cause) is UNPROVEN — this round ran unit tests only, no co-sim, no traffic pattern. Re-run
`ORD-002` co-sim is the first verification step of the next prune→regress round (sequence unchanged:
prune scenarios -> re-run regression -> triage). Fix H1 (below) before that run or every `*×hotspot`
cell errors.

**Follow-up (#5, pre-existing, not introduced this round):** NSU `VcArbiter` still carries `use_pools_`
+ scalar `write_rsp_vc_`/`read_rsp_vc_` + a `!use_pools_` branch; NMU collapsed scalar into a size-1
vector pool (no flag, no branch). Mirror NMU on the NSU side to remove the asymmetry. Shrink, ~-20 lines.

### Matrix harness bugs (codex-found)

| id | location | symptom | fix |
|---|---|---|---|
| H1 | `sim/regress/run_regress.py:91-98` (`run_cell`) + `sim/tools/gen_test_patterns.py:691` | `pattern=hotspot` cells error out. `run_cell` passes `--preserve-addr` but never `--hotspot`, and `gen_test_patterns` calls `ap.error("--hotspot is required")` when it is absent. The current nightly group-1 `BAS-003 x hotspot` cell hits this on the first complete run. | `run_cell` passes a default `--hotspot` target (e.g. the center tile) when `pattern == hotspot`. Hotspot stays in the matrix (user decision). |
| H2 | `sim/tools/gen_test_patterns.py:701-702` | `preserve_addr` is ignored on the `uniform_random` / `hotspot` paths (they call `_emit_base_driven_node` without the preserve option). The deterministic paths honor it at `:630` and `:657`. Dormant today because the AX4 group runs only on `neighbor`. | Honor `preserve_addr` on all paths, or keep the address-agnostic group on default reallocation (which needs no preserve) and document the constraint. |
| H3 | `sim/regress/run_regress.py:62-69` (`is_excluded`) | Exclusion matches only `from` / `pattern` / `topology` / `rob_mode`. A `from` scenario shared by a preserve group and a non-preserve group makes one exclusion hit both. | The full-cross split puts each `from` in one group, so this stays dormant. Add `preserve_addr` to the `when` key only if a scenario is ever shared. |

## Next-session plan (2): prune scenario set to four shapes

(Plan 1 = fix the per-id VC binding bug, see Bugs. This prune runs after it.)

User decision (2026-06-30): keep only the single / burst / outstanding / out-of-order read/write
shapes; delete the rest. Sequence: fix VC binding -> prune -> re-run `make sim-regress
BUILD=mesh_4x4_vc1` -> triage remaining fails. A second-AI review (2026-06-30) flagged three traps in the literal keep-set:

1. **STR family trap.** STR bundles latency / outstanding / multi-dst under one family name. The keep
   target is outstanding only -> keep `STR-002`, drop `STR-001` (latency) and `STR-003` (multi-dst).
   Keeping the whole family by name would smuggle in two dimensions not asked for.
2. **OoO bucket is empty until the `ORD-002` hang is fixed.** `ORD-001` is same-id (AXI requires
   same-id in-order, so it verifies "in-order stays in-order"). The real OoO case (diff-id may return
   out of order) is `ORD-002`, which is the very scenario excluded for the multi-id concurrent-write
   hang. Coverage reporting MUST state OoO is not yet exercised until that hang is fixed; do not count
   it as covered.
3. **ROB axis collapses on this subset.** ROB only acts on multiple-outstanding + may-reorder traffic.
   single / in-order burst / same-id behave identically ROB on/off, so the ROB axis is redundant for
   them. Only `STR-002` and `ORD-002` actually bite the ROB (these are the ROB-verifying scenarios).

**Strict wire-verifiable keep set (15):** `BAS-002/003/004` + `BUR-001..009` + `STR-002` +
`ORD-001/002`. `BAS-001` (single write, no read) and `QOS-001` are write-only -> the wire-verifiable
filter skips them anyway, so they leave with the prune.

**Delete:** `BND-001..007` (narrow / unaligned / sparse / 4KB), `EXC-001..004` (exclusive),
`HSH-001..002` (backpressure), `INF-001` (dpi-fatal), `QOS-001`, `RSP-001..003` (error response),
`STR-001` (latency), `STR-003` (multi-dst), `BAS-001` (write-only).

**Resolved — `BAS-005` drop (strict-15 = 15 scenarios).** VC binding is per-id: `num_vc>1` binds each id round-robin
to one VC (`nmu/vc_arbiter.hpp:225,227`, NSU `:180,182`), so a single-id flow only ever touches one VC
and vc4/vc8 spread needs >=4/>=8 distinct ids in flight. `STR-002` already injects ids `0x1..0x8` (8
distinct) and runs, lighting all of vc8/vc4; `ORD-002` injects `0x1..0x4` (excluded, hang). BAS-005 is
round_robin:4 -> its VC-spread is a subset of STR-002's and it adds no ROB coverage. Its only unique
angle is single-beat x multi-id, marginal. (Earlier "only id-spread carrier" framing was wrong —
STR-002 out-covers it.)

**ROB axis — leave as-is.** Keep `rob_modes: [disabled, enabled]` across the matrix; no per-scenario
ROB on/off prescription. Just record which scenarios actually verify ROB: **`STR-002`** (multi-
outstanding) and **`ORD-002`** (diff-id reorder; excluded until the hang / VC-binding bug clears). The
rest (single / in-order burst / same-id) are ROB-transparent.

**matrix.yaml fallout:** drop the `BND-006`/`BND-007` exclusions (scenarios gone); keep `ORD-002`
(out-of-order, still a fabric bug) and the `BUR-003`@rob exclusion. The full-cross plan below shrinks
to the kept set (agnostic = `BAS-002..004`, `BUR-001..004`, `ORD-001`, `STR-002`; sensitive =
`BUR-005..009`).

## Full cross (orthogonal shape x spatial) — scope shrinks after the prune above

Traffic pattern decides the destination tile (spatial). AX4 transaction pattern decides the read/write
shape. The two axes are independent. The current matrix runs all 36 curated AX4 scenarios on `neighbor`
only because it applies `preserve_addr` to the whole AX4 set. The runner imposes no such limit
(`expand` cross-products the declared groups). Address-agnostic scenarios can run all 4 patterns
through default offset reallocation: `_emit_base_driven_node` copies the base shape and moves only the
address, grouping transactions by original local address so paired and ordered accesses stay
consistent.

`transpose` is a bijection on a square mesh (`gen_test_patterns.py:320-336` requires square power-of-two),
so it does not converge. Only `uniform_random` and `hotspot` converge.

Classify each AX4 scenario by whether its conformity depends on the absolute or low address bits.

| group | scenarios | patterns | address mode |
|---|---|---|---|
| address-agnostic | `BAS-002..005`, `BUR-001..004` (INCR / FIXED), `EXC-001..003`, `HSH-*`, `ORD-001`, `STR-001/002` | neighbor, uniform_random, transpose, hotspot | default reallocation |
| address-sensitive | `BND-*` (narrow / unaligned / sparse / 4KB), `BUR-005..009` (WRAP, wrap phase depends on start addr), `EXC-004` (WRAP pair), `RSP-*` (OOB vs memory window), `STR-003` (addr encodes dst) | neighbor | preserve_addr |

`BAS-001` and `QOS-001` are write-only and `RSP-*` is `category: response`, so the wire-verifiable
filter skips them in either group. `ORD-002` and `BND-006` stay excluded (fabric bugs above).

**Cell count.** 16 agnostic scenarios x 4 patterns + 13 sensitive x 1 = 77 stimulus combinations x 8
builds = 616 raw runnable cells, against 264 today (2.3x). The gain is the transaction-shape x
spatial-pattern cross that the current single-carrier design omits.

**Prerequisites (land with the cross):**
- Fix H1 (hotspot default target), or every `agnostic x hotspot` cell errors.
- Confirm EXC and ORD survive default reallocation under `uniform_random` / `transpose` / `hotspot`
  (paired addresses stay paired, ids are copied unchanged, so likely fine).

**Open decisions (ask at start of next session):**
- Classification mechanism: hardcoded family/id lists in `run_regress._ax4_curated`, or a per-scenario
  `metadata.address_sensitive: true` tag. The tag is self-documenting and survives new scenarios. The
  lists need no per-scenario edits.
- Fold the `BAS-003` carrier group into the agnostic group, or keep it explicit.
- Accept the 2.3x cell-count, or start with a 1-2-per-family sentinel and grow by evidence.

## Verification methodology gaps

| item | summary |
|---|---|
| injection-rate / saturation sweep | The benchmark runner measures one operating point and labels itself `greedy-finite-trace-stress`, "single operating point, no injection-rate sweep" (`sim/tools/run_benchmark.py:16-18,84-86,118`). A latency-vs-offered-load sweep is the measurement that exposes VC-count differences. Today vc1..vc8 latency reads flat because no sweep applies congestion. Needs an AxiMaster injection schedule driving the c_model interface. Recurs across the benchmark-generator, struct-refactor, and congestion-bugfix rounds. |
| coverage + CRV + wire-side SVA | The matrix gates on the scoreboard only and skips non-wire-verifiable response/write-only cases (`sim/regress/README.md:17-23`, `run_regress.py:80-89`). No covergroup, no constrained-random framework, no wire-side protocol assertions. Make it actionable: a coverage plan plus co-sim scenario-coverage accounting (how many of the 37 AX4 actually run at co-sim), not a vague bucket. |

## Design rounds (broader)

| item | summary |
|---|---|
| SAM remap | The NI has a second memory-mapping mechanism (dst via SAM table lookup, `local_addr = addr - base`) per `addr_trans.hpp:7-8,25,28-30` and spec sec 4.3. Today `local_addr = addr` (decode, `:25`). Under remap, stimulus address generation changes from "synthesize dst into the high bits" to "produce flat SAM-mapped addresses, dst by lookup". Reworks `gen_test_patterns`, preserve_addr, and NMU/NSU. Own survey (AMBA System Address Map / FlooNoC) plus brainstorm round. |
| per-id VC binding re-eval | RESOLVED 2026-06-30: confirmed a design bug, promoted to Bugs (per-id VC binding, fix first next round). Survey (IHI 0022 + FlooNoC) basis kept there. |
| multi-id stimulus (`--id-policy`) | Traffic-pattern cells inject a single AXI id (`_emit_synthetic_node` id:0, `--from` copies the base single id), so vc4/vc8 VC-spread is under-exercised for the traffic-pattern group. The AX4 multi-id scenarios cover part of it. |
| NoC-layer QoS | AXI QoS passthrough is done (`packetize.hpp:117,165`, `depacketize.hpp:94,119`) and the NSU has response VC pools plus per-id binding (`nsu/vc_arbiter.hpp:50-56,113-196`). What remains is NoC-layer QoS arbitration and mapping: `NOC_QOS_WIDTH=0` (`ni_flit_pkg.sv:23-24`), `noc_qos` is zero-filled (`docs/architecture.md:66,133`). |
| non-4x4 topology | The generator path already builds `x_dim*y_dim` nodes from YAML (`gen_tb_top.py:21-28,108-122,214-242`) and `router_wrap` exposes 5 link ports (`router_wrap.sv:43-76`). Only 4x4 YAMLs exist (`sim/topologies/mesh_4x4_vc*.yaml`). Add a non-4x4 YAML smoke to prove the generator path on a different shape. |

## Infra / portability

The VCS regression path is documented as Linux-workstation and dry-run pending a real run
(`docs/development.md:227-234`, `sim/vcs/Makefile:8-15`). The matrix is Verilator-only by design
(`docs/superpowers/specs/2026-06-27-regression-matrix-design.md:174`).

- **GCC ICE on `test_pins_smoke.cpp`** (pre-existing, Windows host): GCC internal compiler error
  (segfault) when compiling the `build-cmodel` CMake target on this toolchain. Breaks any CI path
  running `make build` or `make check` (ctest gate). The co-sim Verilator binary is unaffected
  (c_model is header-only; build directly via `make -C sim/verilator`). Investigate toolchain upgrade
  or workaround.

## Dead-code prune candidates (ponytail-audit, 2026-06-30)

Audit ran read-only, no working-tree change. Net ~-1606 lines, 0 dependencies removable. Zero-caller
claims NOT independently verified — confirm each against existing code before deleting (CLAUDE.md
verify-before-change). Lowest-risk first batch = orphans that die with their own self-test (zero
behavior change). Pick a batch and open a round; do not bulk-delete.

| area | candidate | kind | ~lines |
|---|---|---|---|
| tests | `router_path.hpp` + `test_router_path.cpp` (XY predictor, self-test only) | delete | ~-470 (whole tests batch) |
| tests | `isolated_scenario.hpp` + test (self-test only) | delete | |
| tests | `component_dwell_observer.hpp` (SegmentDwell/OccupancyPeak) + test (no harness) | delete | |
| tests | `req_router_at`/`rsp_router_at` + `test_two_node_fabric_at.cpp` (alias assert only) | delete | |
| tests | `test_scaffold.cpp` `*StructsAreConstructible`, `test_ni_stage.cpp` `EnumValuesAreDistinct` (compile-trivial) | delete | |
| docs | `perf-probe-report.md` vs `performance-probe.md` near-duplicate (keep one) | delete | -265 |
| docs | `build_perf_probe_slides.py` (input .pptx + output both gitignored, unrunnable on clone) | delete | -271 |
| sim | `check_perf_parity.py` + test + 4 golden JSON (no Makefile/CI ref) | yagni | -135 |
| sim | `gen_test_patterns.py` synthetic-scenario dead path (`--from` always passed) | delete | -75 |
| sim | `gen_tb_top.py --check` drift gate (no CI, diffs gitignored file) | yagni | -17 |
| sim | `run_regress.py` `is_excluded`/`is_xfail` -> one `_match` (mirrors coverage-round M-item) | shrink | |
| specgen | `ni_spec/report.py` (0 caller) | delete | ~-190 (specgen area) |
| specgen | 3 zero-caller invariant checkers (pin-uniqueness / related-features-symmetric / id-uniqueness) | delete | |
| specgen | `cpp_params.py`/`sv_params.py` duplicate emitter; `constants.py` re-rolled safe-eval; `exceptions.py` 5-class tree | shrink | |
| c_model | ~~VcArbiter `MultiCandidate`~~ DONE (deleted on `feat/vc-id-agnostic`, commit `253b744`) | yagni | ~-130 (headers) |
| c_model | `peek_aw/w/ar/b/r` (10 methods; AxiDpiAdapter caller does not exist) | delete | |
| c_model | `detail::NullSlavePort` (real owner is WireSlavePort), `check_b_one_response_per_write`, dead config `ni_req_extra_depth` | delete | |

Correctly excluded as live (do not touch): `RobMode::Enabled`, `IMemoryPort` (2 impl), `PerfCollector`,
fault injection. Cross-repo theme: dead code kept alive only by its own self-test.

## Cosmetic / cheap (defer)

M-items from the matrix final review: `preserve_addr=True` still runs the ignored `pair_offset` loop,
test inline imports, `gen_tb_top.emit_tb_top(requested_name="")` default is a silent-failure trap,
`is_excluded` KeyError on an unknown `when` key, prebuild `check=True` aborts the whole run instead of
failing one cell. JUnit XML reporting waits until a CI consumer exists.
(Closed in the coverage round: `run_regress` `PASS_MARKER` and `_ax4_curated` orphans deleted, commit
`aa235f5`.)

M-items from the coverage-round final review (none gate merge): `is_xfail`/`is_excluded` are verbatim
duplicates -> factor a private `_match_when(cell, rules)` both delegate to (removes a second place the
`when`-key map can drift); `gen_test_patterns._rewrite_ids` does `t["addr"]` unguarded while
`unique_addr_count` guards with `if "addr" in t` -> mirror the guard (a base scenario with an addr-less
transaction would `KeyError`); `test_run_regress.py` `import pathlib as _pl` sits after the test
functions and is a redundant alias of the top-level `pathlib` import -> hoist and drop the alias;
`_ax4_by_address_mode` parses `unique_addr_count` twice on the raise path -> use a local var; the
`effective_topology()` `_rob`-append branch is only indirectly covered after `test_rob_topology_suffix`
was dropped -> one assert restores it; BND-007 wording differs between the `docs/backlog.md` row
("excluded (matrix.yaml)") and the `matrix.yaml` reason ("re-check on first full run") -> align if the
file is touched.
