# Proportional VC allocation (read/write class pools + round-robin selection)

Status: Design approved, pre-implementation
Scope: NMU request-side + NSU response-side VC allocation, both made pool-aware
with per-id burst→VC binding and round-robin selection. Prerequisite for a
meaningful multi-VC regression matrix (`num_vc = {1,2,4,8}`).

## Problem

`NmuWrap::init` (`nmu_wrap.hpp:51-54`) and `NsuWrap`/`NsuConfig`
(`nsu_wrap.hpp:56-59`, `nsu.hpp:57-58`) pin each AXI message class to ONE virtual
channel regardless of `num_vc`:

| num_vc | read VC(s) | write VC(s) | VC 2..N-1 |
|--------|-----------|-------------|-----------|
| 2 | {1} | {0} | — |
| 4 | {1} | {0} | unused |
| 8 | {1} | {0} | unused |

`read_vc = (num_vc>=2)?1:0`. With 4 or 8 VCs, reads still ride a single VC and
VCs 2..N-1 carry no traffic. The model never exercises the extra channels, so
`vc4`/`vc8` are observationally identical to `vc2`. A regression matrix across
`num_vc` would silently rerun the same behavior at the higher VC counts.

## Why this matters

- **Faithfulness.** A multi-VC NoC splits the VC pool between message classes for
  deadlock avoidance, then uses surplus VCs within a class for load balancing
  (Dally & Towles, VC flow control). Pinning a class to one VC does not model how
  a real NI distributes a class across its allotted lanes.
- **Regression signal.** The NMU reorder buffer (ROB) only does cross-burst
  resequencing when responses from different read bursts arrive out of order,
  which requires those bursts to occupy different VCs. Single-VC pinning starves
  that path, so the ROB-Enabled regression at `vc4`/`vc8` tests nothing `vc2`
  does not already cover.

## Design

### Pool formula (parameterized, not per-topology literals)

```
num_vc == 1              -> write = {0},          read = {0}        (degenerate, shared)
num_vc >= 2 and even     -> write = {0 .. n/2-1},  read = {n/2 .. n-1}
num_vc even-check fails   -> rejected (see precondition)
```

| num_vc | write pool | read pool |
|--------|-----------|-----------|
| 1 | {0} | {0} |
| 2 | {0} | {1} |
| 4 | {0,1} | {2,3} |
| 8 | {0,1,2,3} | {4,5,6,7} |

Pools are disjoint (message-class separation, the deadlock-avoidance
precondition) and equal-sized. Contiguous (write low half, read high half).

**Precondition — odd num_vc rejected.** The wrap layer accepts a raw `uint8_t
num_vc` with no validation today (`nmu_wrap.hpp:46`, `nsu_wrap.hpp:51`). The pool
derivation adds an explicit guard: `assert(num_vc == 1 || num_vc % 2 == 0)`. Odd
`num_vc` has no equal-split and is out of scope; it must fail loudly, not split
silently. Only powers of two are emitted in practice.

**Index choice is perf-neutral.** Contiguous vs interleaved (`{0,2}/{1,3}`) makes
no throughput/latency difference when VC buffers are symmetric and the router's
output arbitration is round-robin (it is — no fixed-priority arbiter, no Duato
escape VC, no dateline ordering tied to VC number). Contiguous is chosen for
readability and the cleanest channel-dependency inspection.

### NMU request side (`nmu.hpp`, `nmu_wrap.hpp`)

The NMU arbiter already has the machinery: `VcArbiter::read_write_split_pools`
factory (`nmu/vc_arbiter.hpp:70`), per-id sticky binding (`write_binding_`/
`read_binding_`, `nmu/vc_arbiter.hpp:143-144`), and `on_id_drained` release
(`nmu/vc_arbiter.hpp:95-100`). Wiring:

**INPUT** `num_vc` from `cmodel_nmu_create`.

**COMPUTE** `NmuWrap::init` derives `write_vcs`/`read_vcs` from the pool formula
and passes them through `NmuConfig`. `make_vc_arbiter` (`nmu.hpp:256-259`) is
changed to call `read_write_split_pools(...)` for the ReadWriteSplit path instead
of the scalar `read_write_split(...)` it calls today.

**OUTPUT** AW flits draw from `write_vcs`, AR flits from `read_vcs`, each via the
round-robin selection below.

`NmuConfig` carries the pools (scalar `write_vc`/`read_vc` either kept as a
one-element-pool shorthand or replaced by vectors; resolved at plan stage). The
public `cmodel_nmu_create(name, src_id, num_vc)` ABI is unchanged — the wrap
layer derives pools internally.

### NSU response side (`nsu.hpp`, `nsu_wrap.hpp`, `nsu/vc_arbiter.hpp`) — the hard part

The NSU arbiter is NOT a structural mirror of the NMU one. Its
`select_vc_for_axi_ch(axi_ch)` (`nsu/vc_arbiter.hpp:83,96-111`) takes no id, has
no binding arrays, no `on_id_drained`. Its header notes "multi-flit R uses ROB
not wormhole" — R-response beats are not wormhole-paired and rely on the NMU ROB
to resequence.

**Correctness hazard (the reason this is in scope, not a config tweak).** The
just-shipped ROB Family-C fix (`rob.hpp`, per-base arrival counter) assumes the R
beats of one burst arrive IN ORDER — true today only because all R rides one VC
(FIFO, no overtaking). If R is spread across the read pool WITHOUT keeping each
burst on one VC, beats of one burst take different VCs, see different queueing
delays, and arrive out of order — which breaks the per-base counter (it places
beat-i at `base+i` assuming i-th arrival). So spreading must be per-burst, not
per-beat.

**Mechanism — port the NMU binding machinery to the NSU arbiter.** Give
`nsu::VcArbiter` per-id binding keyed on the response id: `rid → read-pool VC`,
`bid → write-pool VC`, released by an `on_id_drained` hook when the burst
completes. All beats of one R burst stick to that id's bound VC; different rids
round-robin onto different read-pool VCs. This is the same binding pattern the
NMU arbiter already uses for AR/AW, ported to B/R, plus a pools-capable factory
(`read_write_split_pools` for the NSU class — does not exist yet;
`nsu/vc_arbiter.hpp:41` is scalar-only). Do NOT reuse the existing
`MultiCandidate` mode as a stand-in: it scans from index 0 with no binding
(`nsu/vc_arbiter.hpp:105-111`) and is not behavior-equivalent to a bound pools
split.

**Burst-end detection.** The R header `last` field is always 1
(`nsu/packetize.hpp:102`); the arbiter MUST release the rid binding on the
payload `R.rlast` (`nsu/packetize.hpp:108`), not the header `last`. B is a single
flit (`nsu/packetize.hpp:90`), released on accept. Different rids' R beats can
interleave into the arbiter (`push_r` appends to `r_q_` blindly,
`nsu/axi_master_port.hpp:77-79`), which is exactly why per-id binding — not a
single in-flight `current_r_vc_` — is required.

**Release ordering vs the wormhole stage.** The NSU wormhole arbiter
(`nsu.hpp:154-157`) sits upstream of the VC binding and locks nothing (it treats
each flit as its own packet, `ni/wormhole_arbiter.hpp`). The rid binding must be
released only after its terminal flit (`R.rlast`, or the single B) has passed
into the VC arbiter, so a following id's beats cannot inherit a stale binding nor
race ahead of the release.

With this, intra-burst R order is preserved (one VC per burst) and cross-burst R
reorder is produced (different bursts on different VCs) — exactly what exercises
the ROB.

### Within-pool selection: round-robin (the real perf knob)

NMU select today scans the pool from index 0 and returns the first VC with space
+ credit ("first-available", `nmu/vc_arbiter.hpp:172-175`).

| policy | light load (1 txn/node) | heavy load |
|--------|-------------------------|-----------|
| first-available (current) | piles bursts onto the lowest-index VC; pool collapses to ~1 effective VC/class | bias weakens as early VCs fill |
| round-robin | spreads successive bursts across the pool even when all VCs are free | still spreads |

First-available under light load defeats the change: `vc4`/`vc8` collapse back to
`vc2` because every unbound burst picks the same lowest VC. Standard simulators
rotate (gem5 Garnet `calculateVC` advances a free-VC pointer; BookSim2
`arb_type=round_robin`, injection scans from `_last_vc`).

**Change:** add a per-class rotating start index to the unbound-id candidate scan
in BOTH arbiters. A bound id keeps its VC (binding bypasses the scan); rotation
only sets the START of the scan for a newly-bound id.

**Spread requires multiple distinct unbound ids — explicit precondition.** A
bound id bypasses selection entirely (`nmu/vc_arbiter.hpp:171`); bindings release
only on drain. A single AXI id binds once and all its bursts/beats reuse that one
VC — round-robin never fires. So the pool is exercised only when ≥ pool-size
DISTINCT ids are concurrently unbound. Both the unit tests here and the follow-on
regression stimulus MUST drive multiple distinct read/write ids per node; a
single-id pattern will pin to one VC regardless of RR and prove nothing about
multi-VC behavior.

## Invariants preserved (with their preconditions made explicit)

| Invariant | Why it holds |
|-----------|--------------|
| ROB Family-C fix (in-order beats within one burst) | Holds ONLY because each R burst stays on one VC — now guaranteed by the new NSU per-id binding, not by single-VC pinning. This is the load-bearing reason the NSU work is in scope. |
| W-follows-AW (Constraint A1, `current_aw_vc_`, `nmu/vc_arbiter.hpp:197-209`) | RR only rotates AW's VC pick; W still follows its AW's VC. Unaffected. |
| Same-burst wormhole pairing (`nmu.hpp:272-274`, `packetize.hpp` W last=wlast) | Stamped at packetize time, independent of VC selection. Unaffected. |
| Deadlock-free message-class separation | Pools disjoint by construction. |

## Tests (parameterized over num_vc = {1,2,4,8})

NMU arbiter (`test_vc_arbiter.cpp`):

| Test | Pins |
|------|------|
| pool split correct | write/read pools match the formula; disjoint |
| round-robin spreads | ≥ pool-size DISTINCT unbound read ids land on distinct read-pool VCs (not all lowest) |
| single id pins | one read id reuses one VC across many bursts (RR does not fire) — documents the precondition |
| sticky binding holds | bound id reuses its VC; burst beats stay on one VC |
| backward compat | vc1/vc2 selection observationally identical to current first-available |
| odd num_vc rejected | `num_vc=3` aborts at the guard |

NSU arbiter (new binding path):

| Test | Pins |
|------|------|
| R burst stays on one VC | all beats of one rid's R burst map to a single VC |
| distinct rids spread | different rids round-robin across the read pool |
| on_id_drained releases | a drained rid frees its VC for reuse |
| B uses write pool, R uses read pool | response-class separation matches NMU request-class pools |
| degenerate num_vc==1 | both classes route to VC 0 |

## Non-goals

- No change to router VC count, credit depth, or topology shapes (router already
  parameterizes `num_vc`/depth, `router.hpp`).
- No QoS/priority lanes (surplus VCs are load-balancing only, not priority).
- Odd `num_vc` is rejected, not supported (enforced precondition above, not a
  silent gap).
- The regression matrix itself is the follow-on sub-project.

## Review trail

Three read-only surveys (Codex + general NoC theory). Survey 1: the c_model pins
read to a single VC regardless of num_vc; standard practice splits a class across
its VC pool; a single burst stays on one VC, so the ROB fix survives IF that
invariant is upheld. Survey 2: VC index partition is perf-neutral labeling
(contiguous chosen); within-pool first-available is a light-load trap, so
round-robin selection is required. Survey 3 (design review of this spec): the NSU
arbiter has no id-aware binding, so a naive R-pool spread would break the ROB
in-order assumption — the NSU must GAIN per-id binding, not just a config change;
odd num_vc must be explicitly rejected; spread requires multiple distinct unbound
ids in both tests and stimulus. All folded in above.

## References (methodology, vendor-neutral)

- Dally & Towles, *Principles and Practices of Interconnection Networks* — VC flow
  control, message-class separation, channel-dependency reasoning.
- Duato et al., *Interconnection Networks: An Engineering Approach* — escape-VC /
  acyclic dependency exceptions (none used here).
- gem5 Garnet `NetworkInterface::calculateVC` — rotating free-VC allocation.
- BookSim2 (`arb_type=round_robin`, injection from `_last_vc`) — round-robin VC
  selection default.
