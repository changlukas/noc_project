# NMU request-path HOL deadlock fix: design

Date: 2026-07-04
Branch: `feat/verilator-5048-axi-sv-bfm`
Status: rev1, pending Codex FlooNoC-alignment review + user approval

## Problem

Co-sim deadlocks under load (mesh_4x4_vc1, 8 reads + 8 writes per node, pulp rand VIP).
2R/2W passes. Watchdog fires after ~92K cycles of zero progress. Fabric state dump
(`sim/verilator/output/forensics/run_8r8w_s1.log`) localizes the freeze inside the NMU
request path, not the network.

**Decisive evidence.** Five NMUs (nmu_5/6/9/11/12) show `s1[aw,w,ar]=4,0,1  s2=0
req_credit_avail[vc0]=1`: wormhole AW-input full (4), W-input empty, VcArbiter empty,
and network credit available. Credit available + nothing injected = internally wedged,
not network-blocked. Refutes a network wormhole deadlock as the root.

## Root cause

`NmuReqS1Bridge::tick` (`src/c_model/include/nmu/nmu.hpp:70-93`) has a head-of-line early
return at line 78:

```
if (s1_aw_.full()) { try push AW to Packetize }
if (s1_aw_.full()) return;   // blocks W and AR when AW cannot drain
if (s1_w_.full())  { try push W }
if (s1_ar_.full()) { try push AR }
```

The WormholeArbiter locks on an AW head (`last=0`) via the AW->W ChannelPairing
(`nmu.hpp:271`) and releases only after the paired W with `wlast` drains
(`ni/wormhole_arbiter.hpp` lock/release). Self-deadlock:

**AW-input full -> bridge line 78 stalls W -> wormhole (locked, needs W) never releases
-> AW-input never drains -> AW-input stays full.**

The read AR is collateral: HOL-blocked behind the same full AW slot, so no AR injects and
no R returns (masters parked `rready=1`). Load-dependent: the depth-4 AW input only fills
when >=4 write AWs queue faster than their W bursts drain, never at 2R/2W.

This is a message-dependent deadlock (Dally & Towles, *Principles and Practices of
Interconnection Networks*, Ch. 14), not a routing deadlock. DOR + wormhole + single VC is
routing-deadlock-free (Dally & Seitz 1987), but that proof assumes non-blocking injection:
a head that locks network channels while its body is gated at the source violates the
assumption. The fix restores that assumption at the NI.

## Fix (Approach 1)

Decouple the bridge so each AXI sub-channel drains independently, moving the AW-before-W
ordering guarantee off the shared gate and onto per-write metadata.

**Change 1, bridge independent drain** (`nmu.hpp` `NmuReqS1Bridge::tick`). Remove the
line-78 early return. Drain `s1_aw_` / `s1_w_` / `s1_ar_` each on its own, with no
cross-channel gate.

**Change 2, W backpressure on missing metadata** (`packetize.hpp` `Packetize::push_w`).
When `w_meta_fifo_` is empty, return false (backpressure) instead of asserting. A W whose
AW has not yet been admitted to Packetize waits in `s1_w_` without blocking AW or AR.

WormholeArbiter, VcArbiter, Rob, AxiSlavePort unchanged. Naming unchanged (`w_meta_fifo_`).

## Correctness invariant: AW-before-W ordering preserved

`push_w` inherits the write's `dst_id` from `w_meta_fifo_.front()`. The front must always be
the write whose W is at `s1_w_`. This holds without the shared gate:

1. Rob `w_burst_credit_` pushes W-N to the bridge only after AW-N is accepted upstream.
2. `s1_w_` is single-slot, fed in strict issue order -> a write's W beats fully drain before
   the next write's enter.
3. `w_meta_fifo_` pushes at push_aw, pops at `wlast`, in issue order -> front is always the
   oldest open write (AW admitted, wlast not yet).
4. The only case where a W is ready but its AW is not yet in Packetize manifests as
   `w_meta_fifo_` EMPTY (all older metas already popped, this write's not yet pushed). The
   empty-guard backpressures it. No non-empty wrong-front is reachable: older metas are
   popped in order, newer metas sit behind in the FIFO.

**Deadlock broken.** W's drain path no longer depends on AW admission -> the locked write
always reaches its `wlast` -> the lock releases -> the AW-input drains -> no self-cycle.

**Required assumption (assert + comment).** AXI4 write data is non-interleaved: a source's W
beats follow its AW in issue order and are not interleaved with another write's W. AXI4
removed WID, so this is the protocol guarantee, not an added constraint. Documented at the
`w_meta_fifo_` and `push_w` sites.

## FlooNoC alignment

FlooNoC's source chimney (`hw/floo_axi_chimney.sv`) enforces AW-before-W with an explicit
`aw_w_sel_q` FSM (SelAw -> SelW on AW accept, SelW -> SelAw on W `last`), sources W directly
from `axi_req_in.w` (no deep W FIFO), and gives AW/AR independent spill queues; AW+W mux onto
one request route, AR onto the other.

| property | FlooNoC | this fix |
|---|---|---|
| ordering AW-before-W | explicit SelAw->SelW FSM | `w_meta_fifo_` empty-guard + issue-order draining |
| W drain independent of new AW/AR | W sourced direct from AXI | independent bridge W drain |
| AR path | separate request route | separate wormhole input (2) |
| burst lock on shared link | floo_wormhole_arbiter LockIn | WormholeArbiter ChannelPairing (unchanged) |

Verdict (Codex, against FlooNoC source): ALIGNED for num_vc=1. Same essential property (W has
an independent drain path after AW admission); the ordering mechanism differs in
implementation point (meta-FIFO state vs explicit FSM) but is semantically equivalent under
the non-interleaving assumption. An explicit SelAw/SelW state was considered and rejected: it
duplicates the `w_meta_fifo_` + Rob credit state already present.

## NSU alignment audit

After the fix, the NMU request ingress is structurally symmetric with NSU `Depacketize`
(`src/c_model/include/nsu/depacketize.hpp`): separate AW/W/AR S1 registers drained
independently by the next stage.

**Justified asymmetry (kept):** NMU packetize preserves AW-derived metadata for W stamping;
NSU depacketize receives already-ordered NoC flits and only demuxes. FlooNoC shows the same
source-vs-destination asymmetry, so keeping it is alignment, not divergence.

**NSU has no analogous self-deadlock.** Its single-ingress `pending_` HOL is inherent to a
single VC (AW/W/AR serialize on one channel) and drains because the S1 registers feed the AXI
subordinate, which always accepts (MAPPED, bounded wait). No circular self-dependency. Action:
add an invariant comment at `Depacketize::tick` `pending_` documenting this, mirroring the
statement at the NMU sites. No structural change to NSU.

## Scope

Changed: `src/c_model/include/nmu/nmu.hpp` (bridge tick), `src/c_model/include/nmu/packetize.hpp`
(push_w backpressure + comment), `src/c_model/include/nsu/depacketize.hpp` (invariant comment only).
Unchanged: WormholeArbiter, VcArbiter, Rob, AxiSlavePort, all SV wrap, DPI, fabric.

## Testing

Fast ctest (NMU request path, no z3):

| case | asserts |
|---|---|
| W-before-AW at Packetize | empty-guard backpressures W, AW/AR still drain, W drains once AW admitted |
| AW-input full while W must drain | W drains to wormhole input 1, lock releases, AW-input drains (direct deadlock trigger) |
| back-to-back bursts | W1.last pops meta1, W2 uses meta2, no mis-stamp |
| multi-ID writes | per-id order preserved, no wrong dst_id |

Integration: co-sim `mesh_4x4_vc1` 8R/8W seed 1 (previously deadlocked) runs to
`PASS: all 16 nodes done, non-vacuous`, zero compare errors.

## References

- W. J. Dally & C. L. Seitz, "Deadlock-Free Message Routing in Multiprocessor Interconnection
  Networks," IEEE ToC C-36(5), 1987.
- W. J. Dally & B. Towles, *Principles and Practices of Interconnection Networks*, Ch. 14
  (routing vs message-dependent deadlock).
- T. Fischer et al., "FlooNoC," arXiv:2409.17606; source `pulp-platform/FlooNoC`
  `hw/floo_axi_chimney.sv`, `hw/floo_wormhole_arbiter.sv`.
- ARM AMBA AXI Protocol Specification IHI 0022 (independent channels; W non-interleaving).
