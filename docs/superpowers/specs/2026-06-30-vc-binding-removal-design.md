# Make VC selection id-agnostic + NMU single-outstanding interlock — design

## Problem

For `num_vc>1` the NMU `VcArbiter` pins each AXI id to one VC for its whole
outstanding window: a sticky cross-transaction `(class, id) -> vc` binding
(`nmu/vc_arbiter.hpp:145-228`). The code comment cites "spec §8". The NSU has a
similarly-shaped `(class, id) -> vc` binding (`nsu/vc_arbiter.hpp:115-198`), but
it is a different mechanism (burst-lifetime, not transaction-lifetime — see below).

Survey findings (2026-06-30):

| source | finding |
|---|---|
| FlooNoC `floo_vc_arbiter.sv` | VC selection is `rr_arb_tree` round-robin over the VC index. It does not read any AXI id / packet id. |
| FlooNoC AXI chimney | same-id ordering is the NI's job — RoB reorder table + end-to-end flow control, or RoB-less ordering. Not VC pinning. |
| `docs/architecture.md` | has no §8 (ends at §7). The "spec §8" citation is unfounded; the binding is a self-added mechanism (design doc `2026-06-13-vc-id-binding-design.md` line 16, "closes the same-id cross-VC overtake gap"). |

Root cause: the NMU binding is a designer-added cross-transaction id pin
mis-attributed to a non-existent spec section. It conflicts with the FlooNoC
model the project follows.

The NSU `read_binding_` looks like the same mechanism but is not: it is
burst-lifetime (released on payload `rlast`, `nsu/vc_arbiter.hpp:192-196`), not
transaction-lifetime. It keeps every beat of one R burst on one VC. Because the
NSU AxiMasterPort delivers R beats in per-channel FIFO order regardless of id
(`nsu/axi_master_port.hpp:18-20`), R bursts of different ids can interleave at the
arbiter, so this coherence must be keyed by id (each in-flight R burst records the
VC its first beat selected). The VC is still chosen by round-robin; the id is only
a burst-grouping key, not a VC selector. This is the NSU mirror of the NMU
`current_aw_vc_` W-follows-AW invariant and must be kept.

## Decision

VC is chosen by id-agnostic round-robin by message class (read vs write). The id
never selects a VC. Two orthogonal ordering guarantees remain, both NMU-owned
except the NSU burst coherence:

**Transaction-level same-id response ordering (NMU):**

| mode | mechanism | VC dependence |
|---|---|---|
| Enabled (RoB) | flit `rob_idx` reorder (`write_order_by_id_`/`read_order_by_id_`) | none — RoB reorders regardless of VC count. Unchanged. |
| Disabled (RoB-less) | same-id single-outstanding interlock (one in flight per id, next released on response) | none — only one same-id transaction in flight. |

The interlock is unconditional in Disabled mode (independent of `num_vc` and dst).

**Beat-level burst coherence (wormhole, both NIs):** every beat of one burst
stays on the VC its first beat selected, so a multi-beat burst cannot split across
VCs and reorder within itself.
- NMU W burst: `current_aw_vc_` (W-follows-AW), single tracker — upstream wormhole
  pairing serializes AW+W so bursts never interleave. Unchanged.
- NSU R burst: id-keyed follow (R-follows-first-beat) — R bursts of different ids
  interleave at the arbiter, so the follow VC is tracked per id, released on
  `rlast`. This is the existing `read_binding_`, kept and renamed.

## FlooNoC alignment (RTL-verified 2026-06-30)

Every choice mirrors FlooNoC; nothing here diverges.

| aspect | FlooNoC RTL | this design |
|---|---|---|
| VC selection | `floo_vc_arbiter.sv`: `rr_arb_tree` round-robin over VC index, id-agnostic | id-agnostic round-robin by class |
| same-id cross-transaction ordering | `floo_rob.sv` RoB, or RoB-less stalls injection until in-order | Enabled RoB / Disabled single-outstanding interlock |
| R-beat-in-burst ordering | `floo_rob.sv` uses arrival counting (`offset = rsp_last ? 0 : offset+1`), assumes beats arrive in order | NMU RoB uses the same arrival-offset (`read_arrival_offset_`) |
| R burst VC | `floo_axi_chimney.sv`: R route/VC taken from the stored request header, all beats one VC | NSU R-follows-first-beat, all beats one VC |

The carried-index alternative (per-beat unique `rob_idx`, R free to cross VCs) is
rejected: FlooNoC does not do it, and it would not help Disabled mode (no RoB to
reassemble), which would still need burst-follow.

## Components

**1a. NMU — delete the cross-transaction id pin — `nmu/vc_arbiter.hpp`**
- Delete `write_binding_`/`read_binding_` (256-entry arrays each) and `on_id_drained`.
- Delete the awid/arid read in `push_flit` and the bind-on-accept commit.
- `select_vc_for_axi_ch` drops the `id` parameter: round-robin the class candidate set, pick the first VC with pending space + downstream credit, stamp `vc_id`. AW selects, W follows via `current_aw_vc_` (untouched), AR is a single flit that selects.

**1b. NSU — keep R-burst coherence, drop the B no-op — `nsu/vc_arbiter.hpp`**
- Keep the read-side follow (`read_binding_`): first R beat of an id round-robins a VC, later beats of that id reuse it, released on payload `rlast`. Rename to make intent explicit (e.g. `r_burst_vc_` / "R-follows-first-beat"), and rewrite the comment: this is burst coherence keyed by id, not an id->VC pin; the VC is round-robin-selected.
- Delete `write_binding_` (B is a single flit; the binding is reset on the same push, a no-op). B round-robins a write VC directly.

**1c. Collapse VcArbiter to a single mode (delete MultiCandidate) — both `vc_arbiter.hpp`**
- `MultiCandidate` is unit-test-only: every production config wires `VcMode::ReadWriteSplit` (`nmu_wrap.hpp:54`, `nsu_wrap.hpp:58`), and its `select_vc_for_axi_ch` path (`nsu/vc_arbiter.hpp:122-126`) bypasses the R-burst follow — a latent coherence hole. Remove it rather than patch it.
- Delete the `VcMode` enum, `mode_`, the `multi_candidate` factory, `candidate_vcs_`, and the `candidates_for` mode branch (ReadWriteSplit returns `write_vcs_`/`read_vcs_` directly). Drop `vc_mode`/`vc_candidates` from `NmuConfig`/`NsuConfig` and the `make_vc_arbiter` MultiCandidate branch (`nmu.hpp:269-271`, `nsu.hpp:151-153`).
- Keep both ReadWriteSplit shapes: scalar (single VC per class, no follow needed — one VC is trivially coherent) and pools (multi-VC + the kept R-burst follow).
- Delete the MultiCandidate cases in `test_vc_arbiter.cpp` / `test_nsu_vc_arbiter.cpp`.

**2. NMU Rob Disabled gate -> single-outstanding — `nmu/rob.hpp`**
- `push_aw`/`push_ar` Disabled path: stall when the per-id outstanding source is non-empty (`if (!s.outstanding.empty()) return false;`), replacing the same-id different-dst stall.
- `outstanding` now holds at most one entry per id. Collapse the per-id `std::deque<OutstandingEntry>` to a per-id `bool` (outstanding flag) and remove `OutstandingEntry`/`dst_id` (no longer compared). `write_occupancy()`/`read_occupancy()` count the set flags.
- Rewrite the `rob.hpp:40-43` ordering-owner comment: RoB-less ordering is guaranteed by the NMU single-outstanding interlock, not by AxiMaster / XY routing.

**3. Remove the now-dead Rob drain observer — `nmu/rob.hpp`, `nmu/nmu.hpp`**
- Delete `set_drain_observer`/`drain_observer_`/`notify_drained` and every `notify_drained` call site (`pop_b`/`pop_r` in Disabled mode, `commit_b_exit`/`commit_r_exit` in Enabled mode). Its sole consumer was the VC binding release.
- Delete the `rob_.set_drain_observer(...)` wiring at `nmu.hpp:294-295`.

**Dead includes after the deletions** (drop if no other user remains):
- `nmu/rob.hpp`: `<functional>` (observer gone).
- `nmu/vc_arbiter.hpp`: `axi/types.hpp` + the local `AXI_ID_SPACE` alias (binding arrays gone).

## What stays

- Enabled (RoB) mode: `rob_idx` reorder path untouched; same-id may stay multi-outstanding.
- `current_aw_vc_` wormhole burst invariant (Constraint A1) untouched.

## Testing

| suite | change |
|---|---|
| `test_vc_arbiter` (NMU) | drop binding-stickiness cases; add "same id across pushes round-robins to a different VC" (NMU id pin is gone). |
| `test_nsu_vc_arbiter` (NSU) | drop the MultiCandidate cases; add "same-id R beats within one burst stay on one VC; a new VC is chosen only after the prior `rlast`" (R-burst follow, NOT round-robin within a burst). |
| `test_rob` (Disabled) | add "same-id second request stalls until the first response pops" (single-outstanding, dst-independent). |
| existing callers | update/remove tests referencing the deleted API: `on_id_drained` / `notify_drained` / `set_drain_observer` (`test_vc_arbiter.cpp:174,181,424`, `test_rob.cpp:548,578`, `test_nmu_rob_staging.cpp`). |
| co-sim | re-run `AX4-ORD-002` (multi-id concurrent write, currently excluded for a hang) to check whether removing the binding clears it. |

## Trade-off

Disabled mode loses same-id request pipelining (one in flight per id). Acceptable:
multi-id traffic is unaffected (interlock is per-id), and Enabled mode keeps
multi-outstanding. This is the RoB-less ordering guarantee the NMU must provide
once VC pinning is gone.
