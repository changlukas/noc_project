# Clause-2 bypass safe under multi-VC — design

## Problem

RoB bypass clause 2 (same-id same-dest transactions skip a reorder slot, saving R-RoB
SRAM) is unsafe under our multi-VC fabric. The NMU and NSU VC arbiters pick VCs by
ID-agnostic round-robin, so same-id same-dest packets spread across VCs and the routers
round-robin them per output — responses can arrive out of order and trip the NMU bypass
head-invariant abort (`nmu/rob.hpp:381` B, `:422` R). Only clause 1 (an id's first txn)
is safe today. Goal: recover clause 2 on vc4/vc8 to save area, without surrendering the
VC-spread throughput the multi-VC axis exists to provide, and with **zero new packet
fields**.

Cross-AI verified (Codex GPT-5.5 + Fable 5, both against the RTL):
`cross-review/REVIEW_AGGREGATE.md`, `.../return-path_REVIEW_AGGREGATE.md`.

## Key facts this design rests on

- **Head-invariant is per-AXI-id, not per-VC** (`nmu/rob.hpp:381-383,422-424`). Correctness
  needs only: all same-(dst,id) bypassed packets traverse ONE VC on each network.
- **Routers never change a flit's VC** — no overflow (`router.hpp:181-187,242-243`). A fixed
  injection VC stays in-order end to end.
- **REQ and RSP are separate networks.** Forward and return VC choices are independent; the
  return path does NOT need to match the forward VC.
- **One-source lemma**: all concurrently in-flight bypassed responses of one (dst,id) come
  from a single NSU. Clause 2 robs on any dest change and is sticky (FlooNoC
  `floo_rob.sv:427-433`); a new bypass streak needs the id FIFO empty
  (`floo_rob.sv:423,437-441`) = full drain. So per-NSU-local VC choice is sufficient.
- Existing header/payload fields carry the ordering key: forward flit has `dst_id`
  (`nmu/packetize.hpp:113,166`); response flit has `dst_id` (= requester) + `bid`/`rid` +
  `rob_req` (`nsu/packetize.hpp:88-94,102-108`).

Philosophy (same two-layer split as FlooNoC): decide ordering at **injection VC selection**
with a deterministic key; leave the link-mux layer round-robin. FlooNoC keys injection by
next-hop direction (its VC is not a spreading tool); we key by (dst,id) because our VC IS a
spreading tool and we must keep same-dst-different-id spread.

## Design

### Forward path — `nmu::Rob` + `nmu::VcArbiter`

**COMPUTE (Rob, `rob.hpp`)** — add clause 2 to the bypass decision:
- Track `prev_dest[id]` per direction and a sticky fallen-back flag mirroring FlooNoC
  `ax_rob_req_q` (`floo_rob.sv:399,423-441`).
- `needs_rob = false` (bypass) when: id has an in-flight txn AND `dst == prev_dest[id]` AND
  not fallen back. Any different dest sets the sticky flag; the whole id reorders until its
  order list drains, then the flag clears.

**COMPUTE (VcArbiter, `vc_arbiter.hpp`)** — keep per-id `last_aw_vc[id]` / `last_ar_vc[id]`:
- `rob_req==0` flit: if `flit.dst_id == last_dst[id]` reuse `last_vc[id]`, else credit-aware
  round-robin and update `(last_dst, last_vc)[id]`.
- `rob_req==1` flit: round-robin (RoB reorders; VC is order-free).
- W follows its AW's VC via the existing `current_aw_vc_` (`vc_arbiter.hpp:102,108-118`).

Ordering is free: AW share one wormhole input FIFO, AR another, so txn N passes VcArbiter
before its follower — `last_vc[id]` is always populated when consulted. No feedback path.

### Return path — `nsu::VcArbiter::push_flit`

**COMPUTE** — deterministic static map (RZ1), zero state, zero new fields:
- `rob_req==0` B/R: `rsp_vc = rsp_pool[f(dst_id, id) % |rsp_pool|]`, `f` any fixed pure
  function (e.g. `dst_id ^ id`). Pinned-VC full → refuse (`return false`), never spill.
- `rob_req==1`: round-robin (order-free at the NMU slot path).
- **Delete `r_burst_vc_`** entirely (chosen: option b). The static map covers all R;
  intra-burst coherence is automatic (all beats share `(dst_id,rid)`). This is a net state
  deletion, dissolves W6 (same-rid multi-source contention, `meta_buffer.hpp:22-28`), and
  lifts the constraint that forced `remap_downstream_id` to ignore `src_id`.

### Dependency

Forward and return are correct **only jointly**. Neither half is safe alone; land them
together.

## Trade-offs settled

| decision | chosen | why |
|---|---|---|
| forward VC record | per-id `(last_dst,last_vc)` in VcArbiter, no follow-bit | uses existing `dst_id`; wormhole FIFO ordering makes it race-free (Codex/Fable) |
| return VC | RZ1 static map `f(dst_id,id)` | zero-field; one-source lemma makes NSU-local sufficient; dynamic follow (RZ2) has no NSU-observable streak boundary → provably needs a field |
| key granularity | `(dst,id)` not `dst`-only | dst-only (candidate A) also binds same-dst-different-id → kills the spread our VC exists for |
| echo request-VC field | rejected | provably unnecessary for correctness; only a spread optimization |
| `r_burst_vc_` | delete (option b) | net code deletion, W6 vanishes; fall back to rekey-(dst,rid) only if robbed-read spread regresses in perf |

## Stages

### Stage 0 — GATE (measure before building)

clause 2 fires ONLY for multi-outstanding same-id **same-dest** streaks (directed-style
traffic; random/uniform rarely fires it -- only on a chance same-dest repeat while the id is still outstanding, since the dest otherwise keeps changing → every 2nd txn
reorders). And clause 2 does not shrink `r_rob_depth` by itself — the saving banks only when
the constant is cut afterward.

- Add a multi-outstanding same-dest streaming pattern to `gen_test_patterns`.
- Measure robbed-slot high-water on the R RoB with **clause 1 only** (current HEAD) at vc4/vc8.
- **Go/no-go**: if the high-water shows a fundable `r_rob_depth` cut, proceed. If shipped
  stimulus stays single-outstanding per id (clause 2 never fires), STOP — clause-1-only is
  the correct answer at zero cost. Record the number either way.

### Stage 1 — forward path

Rob clause 2 (sticky prev_dest) + VcArbiter per-id last-VC follow. Unit tests for the
sticky fallback (dest change mid-streak → reorder until drain) and the last-VC follow.

### Stage 2 — return path

NSU static map + delete `r_burst_vc_`. Unit tests for the map determinism and B/R coverage.

### Stage 3 — co-sim verification + area

- **Fault-injection first**: drive same-dest bypass WITHOUT the pin, confirm the head-invariant
  abort fires (proves the checker is live), then enable the pin.
- vc4/vc8 same-dest streaming: scoreboard clean, non-vacuous.
- **Hotspot before/after**: saturation throughput + tail latency — the pin must not materially
  regress hotspot (W2 HoL acceptance). If it does, the area win did not pay for itself.
- Cut `r_rob_depth` per the Stage-0 number; record the area delta (pin state is ~2 Kb flops,
  second-order; the saving is R RoB slots, one slot = one data beat of SRAM).

## Verification summary

| property | check |
|---|---|
| forward same-(dst,id) stays on one req VC | unit test on VcArbiter last-VC follow |
| sticky fallback on dest change | unit test: dest change mid-streak reorders until drain |
| return same-(dst,id) stays on one rsp VC | unit test on static map determinism |
| end-to-end order under multi-VC | co-sim vc4/vc8 same-dest streaming, scoreboard clean |
| checker is live | fault-injection: unpinned bypass trips the abort |
| no throughput regression | hotspot before/after saturation + tail latency |
| area win real | `r_rob_depth` cut per Stage-0 high-water, area delta recorded |

## References

- `cross-review/REVIEW_AGGREGATE.md` (clause-2-unsafe-under-VC, per-class-pool-size gate)
- `cross-review/clause2-pinning_REVIEW_AGGREGATE.md` (forward path, follow-bit → last-VC)
- `cross-review/return-path_REVIEW_AGGREGATE.md` (RZ1 zero-field proof, RZ2 collapse, W6)
- FlooNoC: `floo_rob.sv` (clause 1/2, sticky), `floo_vc_assignment.sv` (deterministic
  injection VC), `floo_vc_arbiter.sv` (RR link mux) — two-layer split we mirror.
