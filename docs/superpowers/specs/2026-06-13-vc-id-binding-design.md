# VC ID Binding Design (same-id → same-vc)

Date: 2026-06-13
Status: Approved pending user review
Scope: NMU `VcArbiter` only — add a sticky `axi_id → vc` binding so a multi-VC config
preserves same-flow ordering. Generalize `ReadWriteSplit` to a per-channel candidate VC
set. No router, no integration, no cosim (those are later sub-projects B / C).

## 1. Motivation

The router has no VC allocation stage and never rewrites `vc_id`; with the request-path
`seq`/reorder mechanism removed, ordering is preserved structurally: a flow must keep one
VC end-to-end so it stays in one per-VC FIFO chain. A multi-VC config only preserves
same-AXI-id ordering if the NI pins each id to one VC. `select_vc_for_axi_ch` currently
keys on `axi_ch` alone, so `MultiCandidate` (first-available) can scatter same-id packets
across VCs and let a later packet overtake. This adds the binding that closes that gap.

## 2. Decision log

| Decision | Outcome | Rationale |
|---|---|---|
| Binding key | `(channel class, id)` — separate `write_binding[awid]` / `read_binding[arid]` | AXI AWID/ARID are independent ID spaces; Rob already tracks read/write separately. |
| Binding lifetime | Sticky; exists iff that id is outstanding | Same-id keeps one VC for its whole outstanding window. |
| Release trigger | Rob-completion hook (response-drain) | Rob's per-id outstanding deque is the source of truth; VcArbiter keeps no own counter. |
| Binding location | In `VcArbiter` | VcArbiter owns VC selection (candidate set, depth, credit). Rob only fires a drain callback — it does not touch VC selection. |
| `ReadWriteSplit` shape | Per-channel candidate VC set (default size-1) | Size-1 default preserves current behavior + the existing 28 tests; multi-element sets enable real multi-VC. |
| Buffer model | Unchanged — one dedicated FIFO per VC | No shared buffer pool; "candidate set" = allowed VC indices, not shared storage. |

## 3. Binding tables

- `write_binding : map<awid, vc>`, `read_binding : map<arid, vc>`.
- Invariant: `binding[id]` exists ⟺ that `(class, id)` is outstanding in Rob.
- Each VC retains its own dedicated FIFO (`pending_[vc]`); the binding selects an index,
  it does not pool storage.

## 4. Allocation (first-touch)

On the first packet of a `(class, id)` reaching `VcArbiter` (no existing binding):

- Read the id from the flit **payload** (`awid` for AW, `arid` for AR) — currently
  `select_vc_for_axi_ch` reads only `axi_ch` from the header; add payload-id read on the
  AW/AR paths.
- Pick a VC from that channel's candidate set using the existing availability logic
  (`pending_[vc].size() < depth && downstream_.credit_avail(vc)`); record `binding[id]=vc`.
- If no candidate is "available", fall back to the first VC in the set (a VC must always be
  returned); the binding still sticks.

## 5. Reuse and W-follows-AW

- Subsequent same-`(class, id)` packets reuse `binding[id]` (no re-selection).
- W beats keep using `current_aw_vc_` (the AW's chosen VC carried across the
  WormholeArbiter serialization), which now equals `write_binding[awid]`. The W path is
  unchanged — it does not do an ID lookup.

## 6. Release (Rob-completion hook)

- `VcArbiter` exposes `on_id_drained(channel_class, id)`; it erases `binding[id]`.
- Rob calls it from `pop_b` / `pop_r` **only when that id's per-id outstanding source
  becomes empty** on this pop:
  - Disabled mode: the per-id `outstanding` deque (`rob.hpp` IdState).
  - Enabled mode: the per-id order structure (`*_order_by_id_`, `rob.hpp:263`).
  These are two source paths; the binding-table contract is identical for both.
- Precondition (required for the binding-exists-iff-outstanding invariant): Rob updates
  outstanding at request **push-accept** time (it already does), so a same-id request still
  mid-pipeline between Rob and VcArbiter cannot trigger a premature drain. A new same-id
  request that arrives after drain belongs to a fresh outstanding epoch with nothing in
  flight to overtake, so it may rebind to any VC safely.

## 7. Ordering guarantee and cross-destination

- Same `(class, id)` keeps one VC while outstanding → one per-VC FIFO chain end-to-end (the
  router preserves `vc_id`) → in order for that flow. This is the structural ordering
  guarantee that replaces `seq` for the request path.
- **Cross-dst same-id is not the binding's job and is not a new constraint.** A same-id
  packet to a different `dst` takes a different route, so the single-chain argument does not
  span destinations. This is handled exactly as before:
  - Disabled Rob mode already requires same-id → same-dst (the dst check in `rob.hpp`), so
    the binding is trivially consistent.
  - Enabled Rob mode reorders cross-dst same-id **responses** back to AXI order via the
    response-path RoB (`rob_idx`), which this design does not touch.
  The binding inherits this division of labor; it does not weaken it.

## 8. Backward compatibility

- Default `ReadWriteSplit` candidate sets are size-1 (`{write_vc}`, `{read_vc}`), so
  behavior is identical to today and the existing 28 `test_vc_arbiter` tests pass unchanged.
- Payload-id read happens only on AW/AR. `num_vc == 1` short-circuits to VC 0 before any
  binding logic (no id read), so single-VC fixtures are unaffected.
- After implementation, run the full `test_vc_arbiter` suite to confirm zero regressions.

## 9. Verification (VcArbiter unit tests, no router)

1. Stickiness: repeated same-`(class, id)` packets all get the same VC across a multi-element
   candidate set.
2. Independence: distinct ids spread across the candidate set (not all forced to one VC).
3. Rebind after drain: `on_id_drained(class, id)` frees the binding; a later same-id packet
   may bind to a different VC.
4. W-follows-AW: a write burst's W beats use the AW's bound VC.
5. Mid-flight safety: a same-id second packet issued before the first drains reuses the same
   VC (no overtake window).
6. Parameterized over `num_vc` ∈ {1, 2, 4, 8} × candidate-set configurations.
7. Disabled vs Enabled Rob drain source both free the binding (drive `pop_b`/`pop_r` to the
   empty transition).

## 10. Scope boundary

NI-only: `VcArbiter` (binding tables, payload-id read, `on_id_drained`) + one Rob drain
callback. No router, no `RouterChannel`, no cosim. Independently verifiable by the
`test_vc_arbiter` suite. Sub-project B (RouterChannel integration) consumes this for
ordering-safe multi-VC; sub-project C (cosim) follows B.
