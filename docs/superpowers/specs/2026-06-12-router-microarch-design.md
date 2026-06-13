# Router Micro-Architecture Design

Date: 2026-06-12
Status: Implemented (single-router scope) — 2026-06-13
Scope: (a) single-router micro-architecture for the c_model NoC fabric; (b) `seq` header
field addition to the packet format. Mesh/fabric assembly, multi-router integration, NI
wiring, and the NSU request-reorder mechanism are out of scope (later rounds).
Image authority: `docs/image/router.jpg` matches this design unchanged. `docs/image/header.jpg`
becomes outdated once `seq` lands; see §10.

## 1. Decision log

| Decision | Outcome | Rationale |
|---|---|---|
| VC allocation (VA) stage | Rejected this round | Free-pool VA lets two packets of one flow take different VCs; per-VC FIFOs at the next hop arbitrate independently, so the later packet can overtake. The request path has no reorder mechanism today. Revisit only after the NSU request-reorder round (§10) lands. |
| Ordering strategy | Endpoint reorder, symmetric with RSP | RSP direction already restores order at the endpoint (`rob_req`/`rob_idx` + NMU per-ID RoB). REQ direction gets the same family: `seq` field now (§10), NSU request reorder + window flow control in a later round. Until then, ordering is preserved structurally (fixed VC, deterministic route) under the §10 deployment constraints. |
| `vc_id` mutability | Fixed end-to-end | NMU `VcArbiter` assigns at injection; routers index per-VC FIFOs by it, never rewrite it. Transport ordering contract stays "in order within `(src_id, dst_id, vc_id)`". |
| Pipeline | 3-stage wormhole (image-aligned) | FlooNoC mainline `floo_router.sv` form. |
| Reuse of NI `WormholeArbiter` | No | Its lock pairs sibling input channels (AW→W). The router locks one `(input port, vc)` per `(output port, vc)` until tail. Same FlooNoC lock semantic, different ownership model; implemented separately. |
| `seq` field | 5 bit, after `rob_idx` | §10. |
| Mesh default | 4 × 4 | All router positions (corner / edge / center) and port subsets exercised; range 2..16 bounded by `X_WIDTH`/`Y_WIDTH`. |

## 2. Position and instances

- Class `Router`, header `c_model/include/noc/router.hpp`, namespace `ni::cmodel::noc`.
- One instance per physical network: REQ and RSP fabrics are separate `Router` objects
  (protocol-deadlock separation; mirrors the existing `NocReq*` / `NocRsp*` interface split).
- Port set is the fixed enum `{LOCAL, NORTH, EAST, SOUTH, WEST}`. Each router holds its
  `(x, y)` coordinate plus mesh dimensions. Unused edge ports stay unconnected and never
  present flits.

## 3. Pipeline (3 stages, 1 cycle each)

| Stage | Function |
|---|---|
| 1 | Per-`(input port, vc)` FIFO; route computation (RC) on the FIFO head |
| 2 | Per-`(output port, vc)` wormhole arbitration + crossbar; per-output VC arbitration |
| 3 | Output FIFO → link |

- Zero-load latency through one router: 3 cycles. Verified by test (§12.5).
- No zero-cycle bypass and no look-ahead routing this round.

## 4. Route computation

- XY dimension-order routing (DOR), minimal, deterministic.
- `dst_id` splits into `(x, y)` with the same bit-slice as `nmu::addr_trans::xy_route`
  (X in low bits, widths `X_WIDTH`/`Y_WIDTH` from the params domain).
- Compare against router coordinate: `x` differs → EAST/WEST; else `y` differs →
  NORTH/SOUTH; else LOCAL. The function never produces a Y→X turn.
- Assumptions stated for §8: no wraparound links, all VCs share this routing function.

## 5. Arbitration (two levels, both FlooNoC-referenced)

**Grant** is defined as the event "flit transfers into the output FIFO". Eligibility
(§ below) already requires output-FIFO space and downstream credit, so a grant always
completes; lock acquisition, lock release, RR pointer advance, and credit decrement all
occur at this single event.

Packet level — one wormhole arbiter per `(output port, vc)`:

- Candidates: input ports whose head flit routes to this output on this VC.
- Grant of a head flit (`last = 0`) locks the arbiter to that input; grant of the tail
  (`last = 1`) releases it. A single-flit packet (`last = 1` at grant) locks and releases
  in the same cycle.
- Round-robin pointer advances on release (packet granularity). After a tail grant, the
  next packet's arbitration starts the following cycle (1-cycle turnaround).
- Routing is per packet: RC runs on the head flit only; body and tail flits follow the
  lock and their `dst_id` is not examined.
- A locked input VC with an empty FIFO idles the `(output port, vc)` arbiter; no other
  input may use that VC until the tail arrives and is granted.
- Reference: `floo_wormhole_arbiter.sv` lock semantic, ported with `(input port, vc)`
  ownership instead of NI channel pairing.

Flit level — one VC arbiter per output port:

- Eligible VC: a flit is available from the input currently holding (or acquiring)
  this VC's packet-level lock, downstream credit > 0, output FIFO not full.
  Same-cycle output-FIFO enqueue and dequeue is allowed (net occupancy unchanged).
- Round-robin across VCs, advancing per flit.
- Reference: `floo_vc_arbiter.sv`.

Starvation bounds (verified §12.4 under no downstream backpressure, i.e. credits never
exhausted and the link always accepts): packet level ≤ (competing inputs − 1) ×
`MAX_PACKET_FLITS` flit times, where `MAX_PACKET_FLITS` = 1 + max AXI burst beats;
flit level ≤ `NUM_VC` − 1 flit times. With backpressure the bounds scale by the
downstream stall time and are not asserted numerically.

## 6. Link contract and credit flow control

The four abstract interfaces (`noc_req_in/out`, `noc_rsp_in/out`) carry no credit-return
pulse, so the spec defines a router link contract on top of them. Concrete API on `Router`:

- Forward in: `input(port).push_flit(flit)` — at most one flit per cycle per link;
  always accepted, because the sender's credit counter guarantees FIFO space.
- Forward out: router calls `downstream(port).push_flit(flit)` when a flit leaves its
  output FIFO; binding via `set_downstream(port, link)`.
- Reverse: `receive_credit(port, vc)` — credit return pulse, emitted by the receiver one
  cycle after it dequeues a flit from its input VC FIFO (registered, 1-cycle latency).
  A LOCAL endpoint (NI) emits the same pulse when it consumes a flit.
- `tick()` advances one cycle; evaluate-then-commit inside, so same-cycle ordering between
  ports is deterministic and independent of call order.

Credit accounting, per `(output port, vc)`:

- Counter seeded at construction with the downstream input VC FIFO depth
  (`ROUTER_VC_DEPTH`; homogeneous fabric, both ends read the same parameter).
- Decrement at grant (§5), i.e. when the flit enters the output FIFO. The credit is
  thereby reserved; stage 3 transmits without rechecking. Same decrement point as
  BookSim2 `BufferState::SendingFlit`.
- Increment on receipt of a credit return pulse.
- Conservation invariant, sampled after each `tick()` (post-tick state):
  `counter + credits_in_flight + output_fifo_occupancy + link_in_flight
  + downstream_fifo_occupancy = ROUTER_VC_DEPTH`.

## 7. Output stage

- Output FIFO depth `ROUTER_OUTPUT_FIFO_DEPTH`. Stage-2 arbitration stalls while it is full.
- Credits are reserved at output-FIFO admission (§6), so a queued flit always has a
  downstream slot; the output FIFO is not a routing resource and adds no channel
  dependency (relevant to §8).

## 8. Deadlock freedom

- Routing: XY DOR forbids Y→X turns, so the channel dependency graph of each physical
  network is acyclic (Dally & Towles, *Principles and Practices of Interconnection
  Networks*, ch. 14). Holds per VC because all VCs use the same routing function.
- Assumptions: no wraparound links; LOCAL ejection eventually drains (NSU consumes and
  returns credit; endpoint consumption assumption); output FIFOs hold reserved credits
  (§7) and introduce no additional dependency edge.
- Protocol: REQ and RSP traverse disjoint router instances, so a request can never block
  behind a response or vice versa inside the fabric.

## 9. Error and reset behavior

| Condition | Behavior |
|---|---|
| `dst_id` outside mesh range at RC | assert + abort |
| Input flit `vc_id` ≥ `NUM_VC` | assert + abort |
| Credit counter underflow or overflow | assert + abort |
| `route_par` mismatch (when `EN_ROUTE_PAR`; parity over `dst_id`, `last` per `ni_packet.json`) | drop flit + increment test-visible drop counter (register-side counting stays at the NI sink per `ni_registers.json`). Fault model assumes a single-flit packet; mid-packet tail drop under an active wormhole lock is not recovered this round (would wedge that (output,vc) until reset). |
| Nonzero `multicast` or `commtype` (unsupported this round) | assert + abort |
| Construction with `NUM_VC` > 2^`VC_ID_WIDTH` or zero FIFO depths | assert + abort |
| Reset during an in-flight packet | Not modeled. Construction is the only reset; stated explicitly. |

A "head flit while packet active on the same input VC" check is deliberately absent: the
header has no head bit, so a new single-flit packet is indistinguishable from a lost tail.

## 10. Packet format change: `seq` field

New optional header field for request-path ordering, symmetric with the existing
response-path tag (`rob_req`/`rob_idx` + NMU per-ID RoB).

| Property | Value |
|---|---|
| Name / presence | `seq`, Option `EN_SEQ` |
| Width | `SEQ_WIDTH`, default 5 (window 32) |
| Position | Between `rob_idx` and `commtype` |
| Stamped by | NMU, per-`(src_id, dst_id)` counter, REQ network packets only; one value per packet (all flits of a packet carry the same `seq`), wraps mod 2^`SEQ_WIDTH` |
| Read by | Future NSU request reorder. Routers never examine it. |

- Position rationale: `rob_req`/`rob_idx`/`seq` form one contiguous ordering-metadata
  block; all router-read fields (`dst_id`, `vc_id`, `last`, `route_par` span) sit before
  it, so their offsets are unaffected; `flit_ecc` stays at the tail.
- Width rationale: the fabric is lossless and duplicate-free, so the reorder window is
  unambiguous iff in-flight packets per flow ≤ 2^`SEQ_WIDTH` (no TCP-style halving
  needed). 32 matches the `rob_idx` outstanding scale (32-entry RoB). Enforcement of the
  in-flight bound is the window flow control of the NSU reorder round.
- Bit budget: header total stays 56. With `seq` enabled the option sum is 59 > 56, so
  `EN_SEQ` + `EN_MULTICAST` + `EN_ECC` cannot be enabled together; the existing
  compile-time budget check rejects such configs.
- Authority handoff: `docs/image/header.jpg` does not show `seq` and is outdated once
  this lands. Authority for the header layout moves to `specgen/generated/json/ni_packet.json`
  plus this section until the image is regenerated (user-owned).

Deployment constraints until the NSU request-reorder round is implemented and verified:

| Constraint | Released by |
|---|---|
| NMU REQ injection: candidate set per AXI channel limited to a single VC (deterministic flow-to-VC; `MultiCandidate` effectively single-entry on REQ) | NSU request reorder + window flow control |
| NSU RSP injection: either single-candidate VC mapping, or `rob_req = 1` on all responses (bypass responses rely on fabric in-order delivery) | Same |

## 11. Parameters and specgen integration

| Parameter | Source | Default |
|---|---|---|
| `NUM_VC` | existing `noc.NUM_VC` (shared; NI and router must agree) | existing value |
| `ROUTER_VC_DEPTH` | new, `constants.yaml` — input VC FIFO depth = credit seed | 4 |
| `ROUTER_OUTPUT_FIFO_DEPTH` | new, `constants.yaml` | 2 |
| `MESH_X_DIM` / `MESH_Y_DIM` | new, `constants.yaml`; range 2..16, bounded by existing `X_WIDTH`/`Y_WIDTH` (4 bit) | 4 / 4 |
| `SEQ_WIDTH` | new, packet domain (`ni_packet.json` field widths) | 5 |

- Router enters `specgen/source/noc_function_blocks.json` as a new function block
  (feature inventory + drift gate; does not drive codegen, per existing invariant).
- `seq` enters `specgen/generated/json/ni_packet.json` (packet domain SSoT) and flows
  through existing codegen to `ni_flit_constants.h` / `ni_flit_pkg.sv`.
- Router parameters flow through the existing params domain (`constants.yaml` →
  `ni_params.h` / `ni_params_pkg.sv`). No new codegen domain.

## 12. Verification invariants

1. Credit conservation (§6 equation, sampled post-`tick()` every cycle).
2. Packet non-interleaving per `(output port, vc)`, including the single-flit
   lock-and-release edge case.
3. Per-VC independence: one head-blocked VC must not stall other VCs of the same input port.
4. All-to-one congestion under no downstream backpressure: no starvation; measured wait
   ≤ §5 bounds.
5. Zero-load latency: a flit pushed at cycle T reaches `downstream.push_flit` at T + 3.
6. Parameterized fixture over `NUM_VC` ∈ {1, 2, 4, 8} × `ROUTER_VC_DEPTH` ∈ {1, 2, 4, 8}.
7. `route_par` fault injection: corrupted parity → drop counter increments, stream
   continues (checker-first discipline).
8. `seq` transparency: header bits identical at router ingress and egress.
9. Multi-router paths: out of scope, fabric round.

## 13. References

- BookSim2 `src/routers/iq_router.cpp` — surveyed; its RC→VA→SA→ST form was evaluated and
  rejected this round (§1).
- FlooNoC `hw/floo_router.sv`, `hw/floo_wormhole_arbiter.sv`, `hw/floo_vc_arbiter.sv` —
  structure ported here; `hw/deprecated/floo_vc_router.sv` as the VA counter-example;
  `FLOO_TYPEDEF_HDR_T` (`rob_req`/`rob_idx`) as the endpoint-reorder precedent.
- Dally & Towles, *Principles and Practices of Interconnection Networks* — DOR deadlock
  argument, credit-based flow control.
