# Proportional VC allocation (read/write class pools + round-robin selection)

Status: Design approved, pre-implementation
Scope: NMU request-side + NSU response-side VC allocation. Prerequisite for a
meaningful multi-VC regression matrix (`num_vc = {1,2,4,8}`).

## Problem

`NmuWrap::init` (`nmu_wrap.hpp:51-54`) and `NsuConfig` (`nsu.hpp:57-58`) pin each
AXI message class to ONE virtual channel regardless of `num_vc`:

| num_vc | read VC(s) | write VC(s) | VC 2..N-1 |
|--------|-----------|-------------|-----------|
| 2 | {1} | {0} | — |
| 4 | {1} | {0} | unused |
| 8 | {1} | {0} | unused |

`read_vc = (num_vc>=2)?1:0`. With 4 or 8 VCs, reads still ride a single VC and
VCs 2..N-1 carry no traffic. The model never exercises the extra channels, so
`vc4`/`vc8` are observationally identical to `vc2` on the response path. A
regression matrix across `num_vc` would silently rerun the same behavior at the
higher VC counts.

## Why this matters

Two motivations, both pointing the same way:

- **Faithfulness.** A multi-VC NoC splits the VC pool between message classes for
  deadlock avoidance, then uses surplus VCs within a class for load balancing
  (Dally & Towles, VC flow control). Pinning a class to one VC is a simplification
  that does not model how a real NI distributes a class across its allotted lanes.
- **Regression signal.** The downstream NMU reorder buffer (ROB) only does
  cross-burst resequencing work when different bursts of one class arrive out of
  order, which requires those bursts to occupy different VCs. Single-VC pinning
  starves that path, so the ROB-Enabled regression at `vc4`/`vc8` would not test
  anything `vc2` does not already cover.

## Design

### Pool formula (parameterized, not per-topology literals)

```
num_vc == 1            -> write = {0},          read = {0}        (degenerate, shared)
num_vc >= 2 (even)     -> write = {0 .. n/2-1},  read = {n/2 .. n-1}
```

| num_vc | write pool | read pool |
|--------|-----------|-----------|
| 1 | {0} | {0} |
| 2 | {0} | {1} |
| 4 | {0,1} | {2,3} |
| 8 | {0,1,2,3} | {4,5,6,7} |

Pools are disjoint (message-class separation, the deadlock-avoidance precondition)
and equal-sized. Contiguous (write low half, read high half). Only `num_vc`
values reaching the wrap layer are powers of two; the formula is defined for any
even `num_vc` and the degenerate `num_vc==1` case.

**Index choice is perf-neutral.** Contiguous vs interleaved (`{0,2}/{1,3}`) makes
no throughput/latency difference when VC buffers are symmetric and the router's
output arbitration is round-robin (it is — no fixed-priority arbiter, no Duato
escape VC, no dateline ordering tied to VC number). Contiguous is chosen for
readability and the cleanest channel-dependency inspection.

### NMU request side (`nmu.hpp`, `nmu_wrap.hpp`)

**INPUT** `num_vc` from `cmodel_nmu_create`.

**COMPUTE** `NmuWrap::init` derives `write_vcs`/`read_vcs` from the pool formula
and passes them through `NmuConfig`. `make_arbiter` (`nmu.hpp:257`) calls the
existing `VcArbiter::read_write_split_pools(...)` factory (already present,
`vc_arbiter.hpp:70`) instead of the single-VC `read_write_split`.

**OUTPUT** AW flits draw from `write_vcs`, AR flits from `read_vcs`.

`NmuConfig` carries the pools. Keep the scalar `write_vc`/`read_vc` fields as a
single-element-pool shorthand, or replace them with the vectors; resolved at
plan stage. Either way the public `cmodel_nmu_create(name, src_id, num_vc)` ABI
is unchanged — the wrap layer derives pools internally.

### NSU response side (`nsu.hpp`, `nsu_wrap.hpp`, `nsu/vc_arbiter.hpp`)

Mirror of the request side for B/R responses: B draws from the write pool, R from
the read pool, using the SAME formula. Asymmetry to close: `nsu::VcArbiter`
currently exposes only the single-VC `read_write_split` factory
(`nsu/vc_arbiter.hpp:41`) — no pools variant. Add a pools-capable factory to the
NSU arbiter (or route through its existing `multi_candidate` mode). Without the
response-side mirror the requests spread but responses re-funnel onto one VC,
leaving the change half-done.

### Within-pool selection: round-robin (the real perf knob)

Current `select_vc_for_axi_ch` (`nmu/vc_arbiter.hpp:172-175`) scans the candidate
pool from index 0 and returns the first VC with space + credit ("first-available").

| policy | light load (regression default: 1 txn/node) | heavy load |
|--------|---------------------------------------------|-----------|
| first-available (current) | piles bursts onto the lowest-index VC; pool collapses to ~1 effective VC/class | bias weakens as early VCs fill |
| round-robin | spreads successive bursts across the pool even when all VCs are free | still spreads |

First-available under light load defeats the entire change: `vc4`/`vc8` would
collapse back to `vc2` behavior because every unbound burst picks the same lowest
VC. Standard NoC simulators rotate instead (gem5 Garnet `calculateVC` advances a
free-VC pointer; BookSim2 `arb_type=round_robin`, injection scans from `_last_vc`)
precisely to avoid lowest-index under-utilization.

**Change:** add a per-class rotating start index to the unbound-id candidate scan
in BOTH arbiters (`nmu::VcArbiter`, `nsu::VcArbiter`). Sticky per-id binding is
unchanged — a bound id keeps its VC, preserving same-id ordering and the
single-burst-one-VC wormhole invariant. Selection only rotates the START of the
scan for a newly-bound id.

**Backward compatibility:** for a single-element pool (`vc1`, `vc2`), round-robin
over one VC equals first-available. Existing `vc1`/`vc2` behavior and tests are
unchanged; only `vc4`/`vc8` change.

## Invariants preserved

| Invariant | Why it holds |
|-----------|--------------|
| ROB Family-C fix (per-base arrival counter, assumes in-order beats within one burst) | A single burst stays on one VC (wormhole + sticky binding); only cross-burst order changes, which `read_order_by_id` + per-base counter already handle. No ROB algorithm change. |
| W-follows-AW (Constraint A1, `current_aw_vc_`) | RR only rotates AW's VC pick; W still follows its AW's VC. |
| Deadlock-free message-class separation | Pools disjoint by construction. |

## Tests (`test_vc_arbiter.cpp` + NSU peer, parameterized over num_vc = {1,2,4,8})

| Test | Pins |
|------|------|
| pool split correct | write/read pools match the formula per num_vc; disjoint |
| round-robin spreads | N distinct unbound read ids land on N distinct VCs across the read pool (not all on the lowest) |
| sticky binding holds | same id reuses its bound VC across multiple bursts; burst beats stay on one VC |
| backward compat | vc1/vc2 selection observationally identical to current first-available |
| NSU response mirror | B spreads write pool, R spreads read pool, symmetric to NMU |
| degenerate num_vc==1 | both classes route to VC 0 |

## Non-goals

- No change to router VC count, credit depth, or topology shapes.
- No QoS/priority lanes (surplus VCs are load-balancing only, not priority).
- No odd-`num_vc` support beyond the formula's definition (wrap layer only emits
  powers of two).
- The regression matrix itself is the follow-on sub-project, not this spec.

## Review trail

Two read-only surveys (Codex + general NoC theory). Survey 1: the c_model pins
read to a single VC regardless of num_vc; standard practice splits a class across
its VC pool; a single burst still stays on one VC (wormhole invariant), so the ROB
fix survives. Survey 2: VC index partition is perf-neutral labeling (contiguous
chosen); within-pool first-available is a light-load trap that collapses extra VCs
to one effective lane, so round-robin selection is required for the extra VCs to
carry traffic. Recommendations folded in: contiguous pools, per-class round-robin,
sticky binding retained, NSU response side mirrored.

## References (methodology, vendor-neutral)

- Dally & Towles, *Principles and Practices of Interconnection Networks* — VC flow
  control, message-class separation, channel-dependency reasoning.
- Duato et al., *Interconnection Networks: An Engineering Approach* — escape-VC /
  acyclic dependency exceptions (none used here).
- gem5 Garnet `NetworkInterface::calculateVC` — rotating free-VC allocation.
- BookSim2 (`arb_type=round_robin`, injection from `_last_vc`) — round-robin VC
  selection default.
