# Regression coverage round — design spec

Status: DRAFT (cross-reviewed by Codex GPT-5.5 + independent Claude subagent; revised per user decisions 2026-06-28)
Date: 2026-06-28
Supersedes scope of: `docs/backlog.md` "Next-session plan: full cross" section

## Problem

Regression matrix v1 (`2026-06-27-regression-matrix-design.md`, merged `6cb09a2`) gates 8 builds
(topology vc1/2/4/8 x rob on/off) on the scoreboard PASS marker, but its coverage is narrow:

- AX4 conformance runs `neighbor` only (whole group forced `preserve_addr`).
- Traffic patterns run one base scenario (`AX4-BAS-003`) x 4 patterns, single AXI id (`id:0`).
- The run has never completed green: H1 (hotspot missing default target) blocks it.

User decision: make coverage adequate THIS round, fix design bugs NEXT round.

## Scope

| In scope | Out of scope (this round) |
|---|---|
| transaction x spatial cross | 补 co-sim scenario count (write-only / response stay on Layer 2) |
| multi-id stimulus for concurrent ordering/correctness | id<->VC decoupling (the id->VC binding is a design bug, next round) |
| non-square topology case (2x4) | injection-rate / saturation sweep (this round keeps the VC axis intact FOR it) |
| capacity-aware classification + cell accounting | wire-side SVA / covergroup / CRV (named as the remaining blind spot) |
| per-build execution model (drop tier abstraction) | fixing fabric bugs ORD-002 / BND-006 / BND-007 (stay excluded) |

**multi-id intent (load-bearing).** The goal is multi-id concurrent ordering correctness (per-id RoB),
NOT VC spread. The id->VC binding is a separate design bug. Stimulus design and pass criteria must not
depend on which VC an id lands in.

## Execution model (replaces the tier abstraction)

No `smoke` / `nightly` / `tier` concept. The regression is one thing: run a build's full scenario set.

**INPUT** `matrix.yaml` declares `topologies`, `rob_modes`, `stimuli`, `exclusions` (flat, no `tiers:` wrapper).
**COMPUTE**
- `make sim-regress BUILD=<build>` runs ONE build (e.g. `mesh_4x4_vc1` or `mesh_4x4_vc1_rob`) through its
  full scenario set.
- `make sim-regress` with no `BUILD` runs every build, each through its full set (8 independent runs,
  dispatchable in parallel / at different times).
**OUTPUT** per-build `matrix.json` (raw / excluded / self-check-skipped / run, reported separately) +
a console pass/fail/skip summary; non-zero exit if any cell failed.

Each build runs its full set on purpose. The VC axis is kept complete (see V-keep below), so per-build
runtime is the cost of one build's full set, not of the 8-build cross in a single serial loop.

## Cross-review verified constraints

| # | Constraint | Evidence |
|---|---|---|
| V1 | `alloc_unique_offset` caps at **4 unique addresses** per node on the default mesh; overflow raises `ValueError` (generator crash, not a scoreboard fail). | `gen_test_patterns.py:223-227,247-253` |
| V2 | STR-002 has 8 unique addresses (16 txns). Classifying it address-independent (default reallocation) hard-crashes the generator on every pattern. It works today only because `preserve_addr` skips the allocator. BAS-005 (4 addr) sits exactly on the boundary. | `AX4-STR-002/scenario.yaml` (verified 16 op / 8 addr) |
| V3 | No `router_wrap` blocker. `LINK_PORTS=5`; the 4x4 fabric already exercises all four directions. Real gap = no non-square YAML exists. | `router_wrap.sv:45`, `gen_tb_top.py:49,196` |
| V5 | `_emit_synthetic_node` is dead in the matrix (`--from` is always passed). neighbor/transpose use `_emit_node` (`:625-630,656`); uniform_random/hotspot use `_emit_base_driven_node` (`:701`). The multi-id lever is an emit-path-agnostic id-rewrite on the loaded base (WI-2), NOT `_emit_synthetic_node`. | `run_benchmark.py` always sets `--from`; `gen_test_patterns.py:625-630,701` |
| V6 | Serial build + serial cells; hangs drain to `TIMEOUT_CYCLES=100000`. A single serial loop over 8 builds runs for hours. The per-build execution model splits this into dispatchable runs. | `run_regress.py:115-131`, `gen_tb_top.py:419` |
| V7 | bijection patterns = {neighbor, transpose}; converging = {uniform_random, hotspot}. `preserve_addr` is collision-free only on bijections. | `gen_test_patterns.py:320-338`, `README.md:39-42` |

### V-keep: the VC axis is kept complete (V4 from the review is rejected)

The review argued transaction conformance does not depend on `num_vc`, so the VC axis is redundant for
the conformance set. **Rejected.** The VC arbiter selects VCs by buffer occupancy plus downstream
credit: an unbound flow picks the first candidate VC with `pending_[vc].size() < pending_depth_ AND
downstream_.credit_avail(vc)` (`nmu/vc_arbiter.hpp:177-183`). Under load, and once the id->VC binding
bug is fixed (which today sticks a bound id to one VC even when full, `:173`), `num_vc` directly
changes VC assignment and congestion behavior. Collapsing the VC axis now would amputate the basis for
the future injection-rate sweep. Every build runs the full set; the VC axis stays at vc1/vc2/vc4/vc8.

## Capacity-aware classification

Classification is by **whether the scenario fits default reallocation**, not by "address-independent"
alone (V1, V2). Mechanism: a per-scenario `metadata.address_mode` tag (consistent with the existing
`metadata.category` keying), plus a classification self-check in `run_regress` (at expand time) that
raises if an `independent` scenario exceeds the 4-slot bound, so a misclassification is caught before
any sim runs (not only as the generator's later `ValueError`).

| address_mode | meaning | patterns | path |
|---|---|---|---|
| `independent` | <=4 unique addresses, dst-tile-only relocation safe | neighbor, uniform_random, transpose, hotspot | default reallocation |
| `dependent` | conformance depends on absolute/low bits, OR >4 unique addresses | neighbor (+ transpose, `[需 generator 驗證]`) | `preserve_addr` |

STR-002 lands in `dependent` by the capacity rule. The implementation plan builds the full
per-scenario table (`unique_addr_count`, `address_mode`, `fits_default_realloc`).

## Self-checking filter

The scoreboard verifies by write then readback, so it self-checks only scenarios that produce OKAY
reads of written data. Write-only scenarios (no read op) and error-response scenarios
(`metadata.category == response`, intentional DECERR) are not self-checking on the wire; they are
reported as skipped (not silent) and stay covered by the Layer 2 c_model integration suite.

## Contracts (the implicit pieces the plan needs)

**Flattened `matrix.yaml`** (drop the `tiers:` wrapper; `all_independent_ax4` / `all_dependent_ax4`
expand from the `address_mode` tag, replacing `all_curated_ax4`):
```yaml
topologies: [mesh_4x4_vc1, mesh_4x4_vc2, mesh_4x4_vc4, mesh_4x4_vc8]
rob_modes:  [disabled, enabled]
stimuli:
  - {from: all_independent_ax4, patterns: [neighbor, uniform_random, transpose, hotspot]}
  - {from: all_dependent_ax4,   patterns: [neighbor], preserve_addr: true}
exclusions:
  - {when: {from: AX4-ORD-002}, reason: "..."}
```
`make sim-regress BUILD=<build>` filters `topologies x rob_modes` to that one build; no `BUILD` runs all.

**`metadata.address_mode`** (new per-scenario field in `scenario.yaml`): `independent | dependent`.
Absent defaults to `dependent` (safe: neighbor + `preserve_addr`, today's behavior). The
classification self-check (`run_regress._ax4_by_address_mode`) raises if an `independent`-tagged
scenario exceeds the 4-slot bound (V1).

**matrix.json status enum** (per cell): `pass | fail | xfail | excluded | skipped_self_check`. The four
counts are reported separately. The coverage denominator = `pass + fail + xfail` (cells that actually
ran); `excluded` and `skipped_self_check` are reported but excluded from the denominator.

**H1 hotspot default target**: interior linear node id `(y_dim // 2) * x_dim + (x_dim // 2)`
(4x4 -> 10; a 2x4 with x_dim=4,y_dim=2 -> 6). Avoids edge tiles; shape-parameterized.

**`--id-policy`** (gen_test_patterns flag, forwarded by run_benchmark): `round_robin:N`, default off
(single id from the base, today's behavior).

## Work items and sequencing

```
WI-0  H1 fix: hotspot default target = a fixed interior linear node id, parameterized for shape.  [first, hard gate]
WI-0b allocator-capacity resolution (capacity-aware classification; see Open decisions).          [blocks WI-1]
WI-A  metadata classification (address_mode) + generator self-check + dry-run/list accounting mode.
WI-B  per-build execution model: BUILD=<build> runs one build's full set; no BUILD runs all; matrix.yaml flattened.
WI-1  transaction x spatial cross populated (address-independent x 4 patterns).
WI-3  non-square 2x4 case. parallel with WI-1; one YAML + generator proof.
WI-2  multi-id (emit-path-agnostic id-rewrite on the loaded base, W/R pair grouping preserved). last; framed xfail.
WI-4  xfail status distinct from excluded; add BND-007 to matrix.yaml for consistency.
```

H1 and the capacity fix are the long poles. WI-2 and WI-3 are independent of each other and of WI-1.

## multi-id (WI-2)

**INPUT** a base scenario fed via `--from`, plus `--id-policy round_robin:N` (N>=4).
**COMPUTE** id-rewrite is emit-path-agnostic. Right after the base scenario is loaded and BEFORE any
emit, rewrite each base transaction's AXI id round-robin across N ids, grouped by write+read pair
(same unique base address = same id, mirroring the pair grouping at `gen_test_patterns.py:507-521`).
Both emit paths then inherit the rewritten ids by deep-copy: `_emit_node` (neighbor / transpose,
`:625-630,656`) and `_emit_base_driven_node` (uniform_random / hotspot, `:701`). This keeps multi-id
independent of the spatial pattern.
**OUTPUT** multi-id concurrent stimulus; default carrier is `neighbor` (bijection, true concurrency).

**Regression wiring (load-bearing).** A multi-id `Cell` field and a `matrix.yaml` stimulus carrying
`id_policy` make this run as a regression cell (not just a CLI feature): `run_regress` forwards
`id_policy` through `run_cell` to `run_benchmark --id-policy`. Without this the multi-id work does not
reach the matrix.

This round it mostly produces xfails, not green cells: concurrent different-id ordering is ORD-002,
which is excluded for the multi-id write hang. Same-id ordering is already covered free by ORD-001 /
BAS-005 riding the cross. State this plainly so WI-2 is not read as new green coverage. A test must
assert that id-rewrite preserves matched W/R pairs (pair copy at `:535-539`; the PASS gate is
`run_benchmark.py:243-248`).

## non-square (WI-3)

Shape `2x4`. Patterns `{neighbor, uniform_random, hotspot}`. `transpose` excluded (the generator
requires `x_dim == y_dim`, `gen_test_patterns.py:329`). One new YAML, build + generator proof only.
8x8 is explicitly avoided this round (64 nodes per build is a runtime trap).

## Decisions (defaulted; revisit only with evidence)

1. **Allocator fix = capacity-aware classification (variant a).** STR-002 (>4 unique addrs) is tagged
   `dependent` and runs neighbor + `preserve_addr` only; it does not enter the address-independent
   cross. Raising `--memory-size` to keep it `independent` (variant b) is a documented follow-up, not
   this round.
2. **transpose stays OUT of the `dependent` group this round.** V7 says it is collision-free on a
   square mesh, but enabling it needs a generator dry-run first. Deferred to keep this round bounded.

## Remaining blind spot (named, not closed)

The only oracle is the scoreboard write->readback. No wire-side SVA, no covergroup, no CRV, and
response/error/write-only paths run only at Layer 2. Calling this round "coverage 做到位" is bounded
by that. The honest coverage figure is "how many of the 37 AX4 actually execute at co-sim vs Layer 2
only", which the dry-run accounting (WI-A) will report. SVA/covergroup is a separate future round
(`docs/backlog.md` verification-methodology gaps).
