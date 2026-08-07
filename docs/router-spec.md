# Design: NoC Router (ROUTER)

Block-level design spec for the per-node mesh router of the NoC C++ behavior model.
The reader implements an RTL block whose cycle behavior is checked against this model
by the existing testbench. As-built references:

| Layer | File |
|---|---|
| Router core (per network) | `src/c_model/include/router/router.hpp` |
| Per-node wrap (REQ + RSP `SimpleRouter`, DAT `Router`, DPI I/O latch) | `src/c_model/include/wrap/router_wrap.hpp` |
| SV DPI module (top-level pin contract) | `src/sv/router_wrap.sv` |
| Unit tests | `src/c_model/tests/router/test_router.cpp` (DAT), `test_simple_router.cpp` (REQ/RSP), `test_route_mask.cpp` + `test_*_fork.cpp` + `test_simple_router_join.cpp` (collectives) |
| Wire-level credit assertions | `sim/tb/link_perf_monitor.sv` |
| Generated fabric wiring | `sim/tools/gen_tb_top.py` |

Cycle convention used throughout: "signal value at cycle N" means the value sampled at
posedge N of `clk_i`. An output registered at posedge K is first sampled at posedge K+1.

## 2. Design Description

### 2.1 Concepts

**Three networks per node.** `router_wrap` holds three independent routers, one per
physical network: a REQ `SimpleRouter` and an RSP `SimpleRouter` (ready/valid,
single-VC, no credit, no VC assignment) and a DAT `Router` (credit, VC-assigning). They
share nothing — separate FIFOs, locks, and pins. Every rule below that names credit,
VC assignment, or `NUM_VC` is a DAT rule; where the two shapes differ the section says
which one it is describing.

**Flit.** The unit of transfer on a NoC link is one flit: a fixed-width word carrying a
44-bit header and a per-network payload. A link moves at most one flit per direction
per cycle.

| Network | flit width | payload | Flow control |
|---|---|---|---|
| REQ | 137 | [136:44], 93 b | ready/valid, 1 VC |
| RSP | 127 | [126:44], 83 b | ready/valid, 1 VC |
| DAT | 629 | [628:44], 585 b | credit, `NUM_VC` 1..8 |

**Packet and wormhole switching.** An AXI transaction is packetized by the NI into one
or more flits sharing the same header `dst_id`. The header bit `flit_tail` marks packet
boundaries: `flit_tail = 1'b0` on every flit except the final one, `flit_tail = 1'b1` on the final
flit. A single-flit packet has `flit_tail = 1'b1` on its only flit. The router forwards
wormhole style: it does not wait for a whole packet before forwarding, and once a
packet's head flit has been granted to an output, that output serves only that packet
until its tail (`flit_tail = 1'b1`) passes. Flits of two packets therefore never interleave
on one output.

The fabric builds exactly one multi-flit packet type, AW+W on the request path, because
AXI4 IHI 0022 A5.3.3 forbids interleaving the W beats of different transactions. Every
response packet — B and every R beat alike — is single-flit, so the output-hold cost of
the lock is confined to the request path.

**Virtual channels (VC), DAT only.** Each DAT input port holds `NUM_VC` independent
FIFOs. A flit's header `vc_id` selects which FIFO it lands in and which credit counter
it consumes. The physical link is one flit-wide channel per direction per network;
`vc_id` travels in the header and there are no per-VC lanes on the wire. The arrival VC
is not necessarily the departure VC: the VA stage (section 2.5) assigns an output VC per
grant and restamps the header, except on `fixed_vc = 1` flits, which the NI pinned and
the router carries through unchanged. REQ and RSP hold one FIFO per input port and
never restamp.

**Flow control, per network.** DAT is credit-based, with no ready signal: a sender may
drive a flit on VC v only when its credit counter for that (output, VC) is nonzero. The
counter is seeded to the receiver's per-VC input FIFO depth
(`NOC_ROUTER_VC_DEPTH` = 8), decrements by 1 when a flit is committed toward that
output, and increments by 1 for each single-cycle credit pulse the receiver returns
after draining one flit from that input FIFO. Example with depth 8: a sender can fire 8
back-to-back flits on VC 0, must then idle at credit 0, and resumes one flit per
returned pulse.

REQ and RSP use ready/valid instead. `ready` is an almost-full early ready computed off
current occupancy, `ready = (occupancy + ready_slack <= depth)`, so it deasserts
`ready_slack` flits before the FIFO physically fills — that covers the multi-cycle round
trip of a registered ready wire between two nodes. It is advisory, not a same-cycle
accept: the sender grants against a `ready` sampled two registrations earlier and the
receiver pushes unconditionally on `valid`, so the transfer is `valid` alone. The
shipped `ready_slack` = 2 is PROVISIONAL and awaits a measured wire-loop calibration
(`simple_router.hpp` `SimpleRouterConfig::ready_slack`).

### 2.2 Flit format

One 44-bit header layout on all three networks, from
`specgen/generated/cpp/ni_flit_constants.h`. Header occupies flit bits [43:0], payload
occupies the rest (REQ [136:44], RSP [126:44], DAT [628:44]).

| Field | Flit bits | Width | Meaning |
|---|---|---|---|
| `axi_ch` | [3:0] | 4 | AXI channel code: 4'd0 `NarrowAw`, 4'd1 `NarrowW`, 4'd2 `NarrowAr`, 4'd3 `NarrowB`, 4'd4 `NarrowR`, 4'd5 `DataAw`, 4'd6 `DataW`, 4'd7 `DataAr`, 4'd8 `DataB`, 4'd9 `DataR`. Values 4'd10..4'd15 never occur. Read by the RSP router only, to identify a CollectB (section 2.10). |
| `src_id` | [11:4] | 8 | Source node id, `{y[3:0], x[3:0]}`. Read for the join's expected-input set (section 2.10). |
| `dst_id` | [19:12] | 8 | Destination node id, `{y[3:0], x[3:0]}`. Read by the router for routing. |
| `fixed_vc` | [20] | 1 | 1'b1: the NI pinned `vc_id`; the DAT router keeps it instead of restamping at VA (SPEC 6). |
| `vc_id` | [23:21] | 3 | Virtual channel index, `0 <= vc_id < NUM_VC`. Read and, for `fixed_vc = 0`, rewritten by the DAT router. |
| `flit_tail` | [24] | 1 | 1'b1 on the final flit of a packet. Read by the router. |
| `ordering_req` | [25] | 1 | NI reorder-buffer flag. Transparent to the router. |
| `ordering_tag` | [33:26] | 8 | NI reorder-buffer index. Transparent to the router, except that the join checks joined heads agree on it. |
| `collective_op` | [35:34] | 2 | 2'd0 UNICAST, 2'd1 MULTICAST. Read by both request routers (fork) and the RSP router (join). |
| `collective_mask` | [43:36] | 8 | Node-id wildcard mask. Read with `collective_op`. |
| payload | per network | 93 / 83 / 585 | AXI channel payload. Transparent to the router. |

There is no `rsvd` field: `PADDING_FIELDS_COUNT` = 0, the header is fully assigned.

IMPORTANT: on unicast traffic the router reads only `dst_id`, `vc_id`, `fixed_vc` and
`flit_tail`. Collectives add `collective_op` and `collective_mask` on all three networks,
plus `axi_ch`, `src_id`, `ordering_tag` and the payload's `bresp` / `bid` at the RSP
join. Every other bit, header and payload alike, passes through unmodified, byte for
byte, and only `vc_id` is ever rewritten. Payload layout is owned by the NMU/NSU specs and is out of scope here.

Node id composition: `node_id = (y << 4) | x` (X_WIDTH = Y_WIDTH = 4). Example: node
(x=3, y=2) has id `(2 << 4) | 3` = 8'h23 = 8'b0010_0011 = 35.

### 2.3 Routing: XY dimension order

Route computation happens per hop, at the head of each input VC FIFO. The flit carries
no route field. Given this router's own coordinate (x, y) and the flit's
`dst_id = {dst_y, dst_x}`:

**INPUT** `dst_id`, own (x, y) -> **COMPUTE**

1. If `dst_x != x`: output EAST when `dst_x > x`, else WEST.
2. Else if `dst_y != y`: output NORTH when `dst_y > y`, else SOUTH (+y is NORTH).
3. Else: output LOCAL (eject to this node's NI).

-> **OUTPUT** one of {LOCAL, NORTH, EAST, SOUTH, WEST}.

Example, 4x4 mesh, destination (3,2), `dst_id` = 8'h23: at node (1,1) `dst_x=3 > 1` ->
EAST. At (2,1) -> EAST. At (3,1) `dst_x == x`, `dst_y=2 > 1` -> NORTH. At (3,2) ->
LOCAL. X always resolves before Y, so the path is (1,1) -> (2,1) -> (3,1) -> (3,2) and
never turns from a Y move back to an X move (deadlock-free on the mesh).

A `dst_id` outside the mesh cannot occur (see Input Guarantees, G3). The model aborts
if it ever does (`route_compute`, `router.hpp:65-68`).

### 2.4 Pipeline: three stages, one stage per cycle (DAT)

The DAT `Router` is a 3-stage pipeline. A flit advances exactly one stage per cycle.
The REQ/RSP `SimpleRouter` runs the same stages 1 and 2 but with `output_fifo_depth` = 0,
so stage 2 drives the downstream link directly and there is no stage 3 — 2 cycles per
hop instead of 3 (`SimpleRouterDatapath.ZeroLoadLatencyDirectModeTwoTicks`).

| Stage | Storage | Action per cycle |
|---|---|---|
| 1. Input | per-port 1-deep input register, then per-(port, VC) FIFO, depth `NOC_ROUTER_VC_DEPTH` = 8 | file the registered flit into the FIFO selected by header `vc_id` |
| 2. Grant | per-output wormhole lock + RR state + credit counters | per output: pick one (input, VC) candidate, assign the output-side VC `out_vc` (VA, section 2.5), pop its FIFO front, decrement `credit_[out][out_vc]`, restamp header `vc_id = out_vc`, push into the output FIFO, schedule one credit pulse (input-side VC) to the upstream of that input |
| 3. Link | per-output FIFO, depth `NOC_ROUTER_OUTPUT_FIFO_DEPTH` = 2 | drive at most one flit from each output FIFO onto the link |

The model evaluates stages in reverse order (3, then 2, then 1) within one tick
(`router.hpp:201-288`). Two observable consequences:

- An output FIFO that is full at depth 2 and drains one flit in stage 3 can accept one
  new grant in stage 2 of the same cycle.
- Stage 2 iterates the five outputs in fixed order LOCAL(0), NORTH(1), EAST(2),
  SOUTH(3), WEST(4) and pops immediately. Up to 5 grants happen per cycle (one per
  output), and one input VC FIFO may be popped for multiple outputs in the same cycle
  when its successive front flits route to different outputs. Example: FIFO
  (WEST, VC0) holds [F_a -> NORTH, F_b -> EAST], both single-flit. NORTH (index 1)
  is evaluated first and pops F_a. EAST (index 2) is evaluated next, sees the new
  front F_b, and pops it too. Both flits leave the same input VC FIFO in one cycle.
  Counter-case with the flits swapped, [F_a -> EAST, F_b -> NORTH]: NORTH sees F_a at
  the front routing EAST and skips this FIFO, EAST grants F_a, and NORTH grants F_b
  one cycle later. Only one flit leaves that cycle, because an output only ever
  examines the current FIFO front. Wire-level credit pulses toward that input's upstream remain at
  most one per (port, VC) per cycle. The surplus pulse is delivered on the following
  cycle (drained one per VC per cycle by the wrap, `router_adapters.hpp` `LinkCreditOut`).

Zero-load latency is exactly 3 cycles per hop on DAT and exactly 2 on REQ/RSP: a flit
sampled from the input wire at posedge N is sampled on the output wire (by the neighbor
or the NI) at posedge N+3 / N+2. Verified by `RouterDatapath.ZeroLoadLatencyIsThreeTicks`
and `SimpleRouterDatapath.ZeroLoadLatencyDirectModeTwoTicks`.

### 2.5 Arbitration: two-level round-robin per output

When an output is not wormhole-locked, stage 2 selects a candidate with two nested
round-robin scans (`router.hpp:239-254`):

1. **Outer, VC-major**: input VCs are scanned in order `vc_rr_[out], vc_rr_[out]+1, ...`
   modulo `NUM_VC`. There is no credit pre-filter on the input VC: credit eligibility
   depends on the VC-assignment result (below), not on the VC the flit arrived on.
2. **Inner, input-minor**: for the chosen VC, inputs are scanned in order
   `rr, rr+1, ...` modulo 5 (per-output pointer `ws.rr`). The first input whose FIFO
   front flit routes to this output AND passes VC assignment wins; a candidate whose
   assignment fails (no eligible output VC with credit) is skipped and the scan
   continues (work-conserving).

IMPORTANT (tie-break): when several (input, VC) pairs simultaneously want the same
output, the unique winner is the first match in scan order: lowest VC offset from
`vc_rr_[out]` first, then lowest input offset from `ws.rr`. Worked example, output
EAST, `NUM_VC = 2`, `vc_rr_[EAST] = 1`, `ws.rr = 3` (SOUTH). Candidates: front of
(WEST, VC0) and front of (SOUTH, VC1), both routing EAST with assignable credit.

- VC scan starts at VC1. Input scan starts at SOUTH. (SOUTH, VC1) routes EAST ->
  **winner (SOUTH, VC1)**. (WEST, VC0) is never examined this cycle.
- Counter-case: if (SOUTH, VC1)'s assignment fails (no eligible output VC with
  credit), the scan continues; with no other VC1 candidate, VC0 is scanned and
  (WEST, VC0) wins if its own assignment passes.

After the scan picks a candidate, stage 2 assigns the OUTPUT-side VC `out_vc`
(VC assignment, ported from the deprecated FlooNoC `vc_router_util` suite):

| Case | `out_vc` |
|---|---|
| `fixed_vc = 1` | header `vc_id` unchanged (NI pin); grantable only if `credit_[out][vc_id] > 0` |
| `fixed_vc = 0`, preferred VC has credit | preferred VC, a pure function of (output port, next-hop XY route) (`preferred_vc`, `router.hpp`) |
| `fixed_vc = 0`, preferred full, `flit_tail = 1` | the highest-index other VC with credit (FVADA overflow) |
| `fixed_vc = 0`, preferred full, `flit_tail = 0` | none — a wormhole head never overflows off its preferred VC; the candidate is not grantable |

The grant decrements `credit_[out][out_vc]` and restamps `vc_id = out_vc` into the
departing header. The credit pulse to the upstream carries the INPUT-side VC (the
FIFO slot freed), which after VA can differ from the VC consumed downstream. With
`NUM_VC = 1` the assignment is the identity.

Both pointers advance only when a tail flit (`flit_tail = 1'b1`) is granted:
`ws.rr = winner_input + 1`, `vc_rr_[out] = winner_vc + 1` (packet-granularity
round-robin, `router.hpp:273-274`). In the example above, if the (SOUTH, VC1) flit is a
tail, the next unlocked scan starts at VC0 and input WEST. For streams of single-flit
packets this degenerates to flit-level round-robin. There is no priority or QoS input:
the flit header has no QoS field (`NOC_QOS_WIDTH = 0`).

### 2.6 Wormhole lock rules (per output, across VCs)

Each output holds one lock record `(locked_input, locked_input_vc, locked_output_vc)`:
the input FIFO the worm drains from, and the VA-assigned output VC every flit of the
worm departs on.

1. Granting a flit with `flit_tail = 1'b0` locks the output to that (input, input VC)
   pair and records the assigned `out_vc` as `locked_output_vc`.
2. While locked, only the locked `(locked_input, locked_input_vc)` FIFO is served at
   this output; every continuation departs on `locked_output_vc` and requires
   `credit_[out][locked_output_vc] > 0`. If the FIFO is empty or that credit is 0, the
   output idles this cycle and keeps the lock. Other inputs and other VCs wait, even
   with credit available. For a `fixed_vc = 0` worm the model asserts that
   `locked_output_vc` equals the continuation's recomputed preferred VC (a pinned
   `fixed_vc = 1` worm's NI-chosen VC legitimately differs).
3. Granting a flit with `flit_tail = 1'b1` releases the lock and advances both RR pointers.
4. A single-flit packet (`flit_tail = 1'b1` on its head) locks and releases within the one
   grant: the output is never observed locked between cycles.

Example: a 3-flit packet (H `flit_tail=0`, B `flit_tail=0`, T `flit_tail=1`) from (LOCAL, VC0) to
EAST. Cycle k grants H and locks EAST to (LOCAL, VC0). Cycle k+1 grants B, lock held.
Cycle k+2 grants T, lock released, `ws.rr` and `vc_rr_[EAST]` advance. A competing
packet at (WEST, VC1) routing EAST waits cycles k..k+2 even though VC1 has credit.

The lock never spans different outputs: locking is a per-output property, so a packet
to EAST and a packet to NORTH from two inputs proceed in parallel.

### 2.7 Credit flow control rules (DAT only)

Counter granularity is per (output port, VC): `credit_[out][vc]`. REQ and RSP have no
counters; they gate on the almost-full `ready` of section 2.1.

1. **Seed**: every counter starts at `NOC_ROUTER_VC_DEPTH` = 8, equal to the
   downstream input VC FIFO depth (`router.hpp:91`).
2. **Decrement**: by 1 at the grant event (stage-2 admission into the output FIFO,
   `router.hpp:262-263`), not at link traversal. With seed 8, eight grants toward one
   (output, VC) with no returns leave the counter at 0 and stall further grants on
   that VC.
3. **Increment**: by 1 per received credit pulse on that (output, VC). A pulse means
   the downstream entity (neighbor router stage-2 dequeue, or the local NI consuming
   an ejected flit) freed one slot.
4. **Pulse generation**: when this router's stage 2 pops one flit from input FIFO
   (p, v), it owes one pulse to the upstream of port p on VC v. The pulse is
   registered: dequeue in cycle N, pulse leaves the core at cycle N+1, and after the
   wrap's output register it is sampled on the SV wire at cycle N+2 (rule R7 in
   section 3.5).
5. **At zero credit**: an unlocked candidate whose ASSIGNED output VC has no credit
   is not grantable and the scan moves on. A locked output idles and holds its lock
   while `credit_[out][locked_output_vc]` is 0. A zero-credit VC never stalls another
   VC at an unlocked output. Example: `NUM_VC = 2`, `credit_[EAST][0] = 0`,
   `credit_[EAST][1] = 3` -> flits assigned to VC1 keep flowing to EAST while flits
   assigned to VC0 wait.

The stage-3 output FIFO (depth 2) is an architectural parameter of this design
(`NOC_ROUTER_OUTPUT_FIFO_DEPTH`) but is not credit-counted and is invisible to the
neighbor. Its only flow effect is the stage-2 admission gate: no grant to an output
whose FIFO already holds `NOC_ROUTER_OUTPUT_FIFO_DEPTH` flits.

### 2.8 Three networks per node

Each mesh node instantiates one `router_wrap` containing three independent router
instances. They share nothing: separate FIFOs, locks, credit or ready state, and
separate `tx_*` / `rx_*` pin groups.

| Network | Class | Carries | Direction |
|---|---|---|---|
| REQ `SimpleRouter` | ready/valid, 1 VC | `NarrowAw`, `NarrowW`, `NarrowAr`, `DataAr` | NMU -> NSU |
| RSP `SimpleRouter` | ready/valid, 1 VC | `NarrowB`, `DataB`, `NarrowR` | NSU -> NMU |
| DAT `Router` | credit, `NUM_VC` VCs | `DataAw`, `DataW`, `DataR` | both |

Splitting request from response is what removes request-response protocol deadlock;
splitting the wide data class off REQ/RSP is what keeps a 585-bit payload off the two
narrow links.

On the RSP network every packet is single-flit: the NSU emits `flit_tail = 1'b1` on every B
flit and on every R beat flit. The RSP router's wormhole lock therefore only ever
engages degenerately (lock and release within one grant, rule 2.6.4), and RSP
arbitration behaves as flit-level round-robin. The same holds for `DataR` on DAT, so the
only worm any router ever holds open across cycles is an AW+W request packet.

### 2.9 Worked example: 3-flit packet, 2 hops (DAT)

Topology: nodes A = (0,0) and B = (1,0). The NMU at A sends one 3-flit `DataAw` + `DataW`
packet (F0 `flit_tail=0`, F1 `flit_tail=0`, F2 `flit_tail=1`, all VC0, `dst_id` = 8'h01) to
the NSU at B. Flits enter A's `rx_dat_*[LOCAL]` at cycles 0, 1, 2. All credit counters
start at 8.

| Cycle | Router A (x=0,y=0) | Router B (x=1,y=0) | Wires (sampled this cycle) |
|---|---|---|---|
| 0 | stage 1: F0 -> fifo[LOCAL][0] | idle | A `rx_dat_valid[LOCAL]` = 1 (F0) |
| 1 | stage 2: grant F0 to EAST, `credit_[EAST][0]` 8->7, lock EAST to (LOCAL,0). stage 1: F1 filed | idle | F1 in |
| 2 | stage 3: F0 -> link. stage 2: grant F1 (7->6). stage 1: F2 filed | idle | F2 in |
| 3 | stage 3: F1. stage 2: grant F2 (6->5), tail -> unlock, RR advance | stage 1: F0 filed | A `tx_dat_valid[EAST]` = 1 (F0). A `rx_dat_crdvalid[LOCAL][0]` pulse (F0's LOCAL dequeue at cycle 1) |
| 4 | stage 3: F2 | stage 2: grant F0 to LOCAL | F1 on link. NMU credit pulse (F1) |
| 5 | idle | stage 3: F0 -> eject. stage 2: grant F1 | F2 on link. NMU credit pulse (F2) |
| 6 | `credit_[EAST][0]` 5->6 (B's pulse for F0) | stage 3: F1. stage 2: grant F2 | B `tx_dat_valid[LOCAL]` = 1 (F0). B's `rx_dat_crdvalid[WEST][0]` pulse reaches A |
| 7 | 6->7 | stage 3: F2 | F1 to NSU. Credit pulse (F1) |
| 8 | 7->8 (fully replenished) | idle | F2 to NSU. Credit pulse (F2) |

Head latency: injected cycle 0, at the destination NI cycle 6 = 2 hops x 3 cycles.
Tail: cycle 2 -> cycle 8. A's `credit_[EAST][0]` bottoms at 5 (three flits in flight)
and returns to 8 by cycle 8. The same packet on REQ would take 2 cycles a hop and gate on
`tx_req_ready` instead of a counter.

### 2.10 Collectives: multicast fork and CollectB join

A collective write is one AW+W worm the fabric replicates to an aligned submesh, and one
merged `B` that retraces that tree. The header fields `collective_op` and
`collective_mask` carry it. Every other flit has `collective_op = 2'd0` (UNICAST) and
takes none of the paths below, so a run without collectives is bit-identical to the
pre-collective model.

**Route-mask dual function** (`route_mask.hpp`, ported from FlooNoC
`floo_route_xymask.sv`). A set `collective_mask` bit is a don't-care on that bit of the
node id, so a mask with n set bits names 2^n nodes. Two pure functions of (`dst_id`,
`src_id`, `collective_mask`, this router's coordinate and mesh dims):

| Function | Wildcard side | Result |
|---|---|---|
| `route_mask_fork` | `dst_id` | the output ports a multicast flit forks to here: X spread along the source's row, the N/S turn in every column the set covers, LOCAL where both coordinates match |
| `route_mask_join` | `src_id` | the input ports a collector waits for replicas on: each member's own XY return path |

They are not mirror images. Same member set and same hop count both ways, but the
interior edges differ for any mask with both X and Y bits set.

**Fork discipline (both request routers).** Data-class multicast forks in the DAT credit
`Router`; narrow-class multicast forks in the REQ `SimpleRouter`. The rules are the same
on both:

1. A head flit with `collective_op != UNICAST` takes the multi-hot branch set of
   `route_mask_fork` in place of the one-hot `route_compute` result. An empty branch set
   at a router the flit reached is fatal.
2. Each branch output arbitrates, locks, and grants on its own. A branch that has already
   accepted the head is masked off (`done_mask`, per input and VC) and idles with its lock
   held.
3. The input FIFO pops, and the single upstream credit pulse leaves, only once every
   expected branch has accepted. Never one pulse per branch.
4. AW and W stay one indivisible worm: the AW carries `flit_tail = 1'b0` and the last W
   beat closes the packet, so every W beat replicates to the AW's exact branch set. Each
   continuation recomputes its branch set from its own header and aborts on divergence.
5. All branches always sit on the same flit. A fast branch is throttled to the slowest
   until the head advances. This is a performance property of the ported discipline, not
   a correctness one.

On DAT each branch additionally runs VC assignment for itself, so branches legitimately
ride different output VCs; a `fixed_vc = 1` collective keeps the NI-pinned `vc_id` on
every branch, credit-gated per output on that same index. The REQ router has neither
credit nor VA, so its branch grants gate on downstream ready and output-FIFO space alone.

**CollectB join (RSP `SimpleRouter` only).** The NSU echoes the AW's `collective_op` and
`collective_mask` onto its `B` (nsu-spec section 2.4). The header has no third opcode: on
RSP the only collective flits are Bs, so `collective_op != UNICAST` together with
`axi_ch` in {`NarrowB`, `DataB`} is the CollectB case. A collective flit on an RSP read
channel is fatal.

| Step | Rule |
|---|---|
| Exclusion | a CollectB head is never a unicast candidate. This is what stops one `B` per member reaching the NMU instead of the one merged `B` it waits for |
| Expected set | `route_mask_join` of the head. An empty set is fatal; so is a CollectB that arrived on a port outside its own expected set, which means the echoed mask disagrees with the delivery path |
| Qualification | fires only when every expected input holds a head of the same collect, equal on `dst_id` and `collective_mask`. Joined heads disagreeing on `ordering_tag`, `axi_ch`, or `bid` is a model bug and aborts |
| Grant | one whole input flit is forwarded, never a rebuilt header, and every contributing head pops in the same handshake |
| Priority | with the output not mid-worm the reduction takes priority over a frozen unicast winner. That winner is delayed, never stolen |
| State | none. Replicas that have not arrived wait in their input FIFOs and the join re-evaluates every tick |

Four properties of the merge diverge from a reference, all deliberate:

| Item | As built | Diverges from |
|---|---|---|
| BRESP precedence | scan the expected inputs in route-index order, first `SLVERR` wins and breaks; `DECERR` is never elevated | AXI worst-response. Ported verbatim from `floo_reduction_arbiter.sv:116-131` |
| Survivor index domain | scan order LOCAL, N, E, S, W | upstream's North = 0 .. Eject = 4. Deterministic-first-`SLVERR` is preserved; the concrete survivor differs under multiple `SLVERR` and under all-OKAY |
| Worm-boundary hold | the join holds while its output is mid-worm and grants at the boundary | upstream's per-beat prio arbiter. Today's NSU emits every RSP packet single-flit, so nothing on RSP is mid-worm and the hold does not engage in the fabric. It is what keeps the join correct if a multi-beat RSP packet ever exists: an unguarded grant inside a foreign worm either aborts legal traffic at the next hop's held route latch or, where the routes coincide, ends that worm's latch early and bypasses its own join, duplicating the `B` at the collector. Cost is latency only, since the join is stateless and re-fires |
| Reduction priority | strict and unbounded | nothing. Faithful to the upstream prio arbiter, and the consequence is that back-to-back collects at one output can starve a frozen unicast winner indefinitely |

**Restrictions.**

| R# | Restriction | Enforced by |
|---|---|---|
| R1 | Two multicasts whose spanning trees overlap are never in flight together | Software (`docs/noc-target-spec.md`, Scope). Not fabric-enforced. The fork state `{expected_mask, done_mask}` per (input, VC) is exposed read-only, so a violation triages as a `done_mask != expected_mask` frozen across ticks with locks held, instead of a bare timeout |
| R2 | At most one outstanding collective per (NMU, AXI id) | NMU `Rob::push_aw` admission (`docs/nmu-spec.md` Section 2.8) |
| R3 | No dedicated multicast VC, no `fixed_vc` special case | Nothing to enforce. The wormhole lock is per output across VCs, so a VC restriction buys nothing |

R1 exists because the ported discipline deadlocks when two multicast trees contend for two
routers' outputs in opposite orders: each holds an output the other needs, neither worm
can reach its tail, and no arbitration order avoids it once both heads are granted. The
cycle is inherent to fork-with-hold and is present in the upstream ready/valid form as
well.

Verified by ctest `test_route_mask.cpp` (fork and join sets cell-verified against
hand-computed meshes, square and not), `test_router_fork.cpp`,
`test_simple_router_fork.cpp` and `test_simple_router_join.cpp`, including
`RouterFork.OneHotForkSetIsBitIdenticalToPlainUnicast`,
`SimpleRouterJoin.FirstSlverrInRouteIndexOrderWins`,
`SimpleRouterJoin.DecerrIsNotElevated`,
`SimpleRouterJoinChain.MidWormHoldKeepsTheDownstreamLatchIntact`, and the bounded-tick
R1 wedge tests `RouterForkWedge.OverlappingTreesOppositeOrderWedgeDetectedWithinBound`
and its `SimpleRouterForkWedge` twin, and by the co-sim `multicast` pattern
(`docs/verification-environment.md`). Contract entries: SPEC 20 (fork) and SPEC 21 (join).

## 3. Inputs and Outputs

### 3.1 Parameters

`router_wrap` SV parameters (`src/sv/router_wrap.sv:54-63`):

| Parameter | Default | Legal range | Meaning |
|---|---|---|---|
| `DAT_NUM_VC` | `ni_params_pkg::NOC_DAT_NUM_VC_DFLT` = 1 | 1..8 (= 2^VC_ID_WIDTH) | VCs on the DAT link. REQ/RSP are fixed single-VC. Topology YAML overrides per run. `initial`-block `$fatal` at time 0 if `$bits(noc_types_pkg::noc_credit_t) != DAT_NUM_VC`. |
| `REQ_FLIT_WIDTH` | 137 | 64..1024 | REQ flit bus width, bits |
| `RSP_FLIT_WIDTH` | 127 | 64..1024 | RSP flit bus width, bits |
| `DAT_FLIT_WIDTH` | 629 | 64..1024 | DAT flit bus width, bits |
| `LINK_PORTS` | 5 | fixed 5 | port array size = {LOCAL, NORTH, EAST, SOUTH, WEST} |

Router model configuration, fixed at `cmodel_router_create` time:

| Parameter | Default | Legal range | Meaning |
|---|---|---|---|
| `NOC_ROUTER_VC_DEPTH` | 8 | 1..16 | input VC FIFO depth; on DAT it is also the upstream credit seed, on REQ/RSP the depth the almost-full `ready` is computed against |
| `NOC_ROUTER_OUTPUT_FIFO_DEPTH` | 2 | 1..16 | DAT stage-3 output FIFO depth, not credit-counted. REQ/RSP run with output FIFO depth 0 (stage 2 drives the link directly) |
| `ready_slack` (REQ/RSP) | 2 | 1..`NOC_ROUTER_VC_DEPTH` - 1 | flits of headroom the almost-full `ready` reserves. PROVISIONAL, awaits a measured wire-loop calibration |
| `mesh_x_dim`, `mesh_y_dim` | 4, 4 | 2..16 each | mesh dimensions. Minimum 2 per dimension: a mesh communicating through NI + router needs at least 2x2; 1x1 and 1xN meshes are illegal. |
| `x_coord`, `y_coord` | per node | `x < mesh_x_dim`, `y < mesh_y_dim` | this node's coordinate |

### 3.2 Port index encoding

All `[LINK_PORTS]` arrays are indexed by direction:

| Index | Direction | LINK-face use |
|---|---|---|
| 0 | LOCAL | this node's own NI traffic (NMU injection, NSU ejection, and the shared DAT merge point). Not a link direction. |
| 1 | NORTH (+y) | link to node (x, y+1) |
| 2 | EAST (+x) | link to node (x+1, y) |
| 3 | SOUTH | link to node (x, y-1) |
| 4 | WEST | link to node (x-1, y) |

Boundary directions (no neighbor) are left unwired by the generated fabric: inputs tied
to 0, outputs must stay 0 (SPEC 17).

### 3.3 Signal tables

Every network's pins are ONE uniform per-port array indexed {LOCAL, N, E, S, W}: LOCAL
carries this node's own NI traffic, N/E/S/W the inter-router links. There is no separate
`noc_nmu_*` / `noc_nsu_*` pin group. `noc_types_pkg::noc_credit_t` =
`{credit[DAT_NUM_VC-1:0]}`, one bit per VC.

> REQ/RSP `ready` is advisory, not a same-cycle accept: the sender grants against a `ready` sampled ~2 registrations earlier, and the receiver pushes unconditionally on `valid`, so a real transfer is `valid` alone.

Inputs:

| Signal | Bit width | Definition |
|---|---|---|
| `clk_i` | 1 | Clock. All sequential behavior on the posedge. |
| `rst_ni` | 1 | Synchronous active-low reset. Given only once, at the beginning of simulation (rule R9). |
| `ctx_i` | 64 | Model handle returned by `cmodel_router_create`. Constant after reset. From tb_top. |
| `rx_req_valid` | 5 | Bit p: the sender at port p drives one REQ flit this cycle. Bit 0 is the local NI's injection. |
| `rx_req_flit` | 137 x 5 (unpacked `[LINK_PORTS]`) | REQ flit from port p. Valid only when `rx_req_valid[p]` is high, all zeros otherwise. |
| `tx_req_ready` | 5 | Bit p: the receiver at port p can take a REQ flit. Advisory (see above). |
| `rx_rsp_valid` / `rx_rsp_flit` / `tx_rsp_ready` | 5 / 127 x 5 / 5 | RSP mirror. |
| `rx_dat_valid` | 5 | Bit p: the sender at port p drives one DAT flit this cycle. |
| `rx_dat_flit` | 629 x 5 | DAT flit from port p. |
| `tx_dat_crdvalid` | DAT_NUM_VC x 5 (unpacked) | Per-VC credit pulse from the receiver at port p, for a DAT flit this node previously sent out of its p output. Increments `credit_[p][vc]`. |

Outputs (all registered, reset to 0):

| Signal | Bit width | Definition |
|---|---|---|
| `tx_req_valid` | 5 | Bit p: one REQ flit driven toward port p this cycle. Boundary bits always 0. |
| `tx_req_flit` | 137 x 5 | REQ flit toward port p. All zeros when `tx_req_valid[p]` is low. |
| `rx_req_ready` | 5 | Bit p: this node can take a REQ flit on port p (almost-full ready, section 2.1). |
| `tx_rsp_valid` / `tx_rsp_flit` / `rx_rsp_ready` | 5 / 127 x 5 / 5 | RSP mirror. |
| `tx_dat_valid` | 5 | Bit p: one DAT flit driven toward port p this cycle. Boundary bits always 0. |
| `tx_dat_flit` | 629 x 5 | DAT flit toward port p. All zeros when `tx_dat_valid[p]` is low. |
| `rx_dat_crdvalid` | DAT_NUM_VC x 5 | Per-VC credit pulse to the sender at port p: this node drained one flit from its p-direction DAT input FIFO, VC v. |

NI-edge flow control (LOCAL port, who answers whom): on REQ/RSP the receiver drives the
`ready` back; on DAT the entity that consumes a flit returns its credit.

| Flow | Flit pin | Back-pressure pin (opposite direction) |
|---|---|---|
| NMU injects REQ | `rx_req_valid/flit[LOCAL]` | `rx_req_ready[LOCAL]` (router -> NMU) |
| Router ejects REQ to NSU | `tx_req_valid/flit[LOCAL]` | `tx_req_ready[LOCAL]` (NSU -> router, tied true) |
| NSU injects RSP | `rx_rsp_valid/flit[LOCAL]` | `rx_rsp_ready[LOCAL]` (router -> NSU) |
| Router ejects RSP to NMU | `tx_rsp_valid/flit[LOCAL]` | `tx_rsp_ready[LOCAL]` (NMU -> router, tied true) |
| NI injects DAT | `rx_dat_valid/flit[LOCAL]` | `rx_dat_crdvalid[LOCAL]` (router -> NI) |
| Router ejects DAT to the NI | `tx_dat_valid/flit[LOCAL]` | `tx_dat_crdvalid[LOCAL]` (NI -> router) |

The LOCAL DAT port is shared: `dat_merge_wrap` sits between it and the NMU/NSU DAT pins,
merging `DataAw`/`DataW` from the NMU with `DataR` from the NSU on egress and demuxing on
`axi_ch` on ingress.

Fabric wiring between nodes pairs opposite ports: node i's `rx_*_valid/flit[NORTH]` comes
from its north peer's `tx_*_valid/flit[SOUTH]`, and node i's `tx_dat_crdvalid[NORTH]`
comes from that peer's `rx_dat_crdvalid[SOUTH]` (`gen_tb_top.py`).

### 3.4 DPI function table

The SV module drives the model with three calls per posedge, in this order
(`router_wrap.sv:160-267`). One `cmodel_router_tick` = one modeled clock cycle for
all three routers.

| Function | When | Semantics |
|---|---|---|
| `cmodel_router_create(name, x_coord, y_coord, mesh_x_dim, mesh_y_dim, dat_num_vc)` | once, from the tb_top `initial` block, after `rst_ni` deassertion | constructs all three routers. Construction is reset: all FIFOs empty, all credits at seed. Returns the 64-bit `ctx` handle. |
| `cmodel_router_{req,rsp,dat}_set_inputs(ctx, ...)` | posedge, step 1 (one call per network) | samples the current SV wire values (the previous cycle's registered outputs of the peers) into the model input latch. Split per network so no DPI signature marshals more than one flit width |
| `cmodel_router_tick(ctx)` | posedge, step 2 | advances all three routers exactly one cycle |
| `cmodel_router_{req,rsp,dat}_get_outputs(ctx, ...)` | posedge, step 3 (one call per network) | reads the model output latch. The SV module registers these values nonblocking, so they appear on the output pins one cycle later. |

Marshalling is port-major, at each network's own word count: flit = 5 (REQ) / 4 (RSP) /
20 (DAT) 32-bit words per port, DAT credit = one `[DAT_NUM_VC-1:0]` word per port,
valid and ready = one bit per port in a packed vector.

### 3.5 Protocol rules

R1 (input rhythm). At most one flit per network per input port per cycle: one on each
of `rx_req_*`, `rx_rsp_*`, `rx_dat_*` per port, LOCAL included. Back-to-back flits on consecutive cycles are legal
without limit while credit lasts. Flits of one packet need not be contiguous: gaps of
any length may separate them (the wormhole lock holds across gaps, rule 2.6.2).

R2 (idle bus state). When a `valid` bit is low, the corresponding flit bus carries all
zeros. This holds for the module's own outputs (registered zeros) and for
its inputs (each input wire is a peer's registered output or a fabric tie-off).
Credit vectors carry 0 in every non-pulsing bit position.

R3 (sampling edge). All inputs are sampled at the posedge of `clk_i`. All outputs are
registered and change only at the posedge. The verification environment (co-sim
scoreboard, `link_perf_monitor` assertions, boundary `$fatal` checks) samples at the
posedge.

R4 (valid behavior). Each `valid` bit is high for exactly 1 cycle per flit, and there is
no retraction: a driven flit is committed. On DAT a sender may assert valid on VC v
toward a port only while its credit counter for that (port, VC) is nonzero. On REQ/RSP
the sender grants against the port's `ready`, sampled two registrations earlier, so
`ready` never gates the transfer in the same cycle.

R5 (credit pulse shape, DAT). Every credit signal bit is a single-cycle pulse. At most
one pulse per (port, VC) per cycle. Each pulse means exactly one freed buffer slot.
Example: two same-cycle stage-2 dequeues from (WEST, VC0) (section 2.4) produce pulses
on `rx_dat_crdvalid[WEST][0]` in two consecutive cycles, never a 2-cycle-wide level
or a double-count.

R6 (credit seed, DAT). After reset, the sender-side counter for every (port, VC) equals
`NOC_ROUTER_VC_DEPTH` = 8. The `link_perf_monitor` on every directed DAT edge seeds its
mirror counter with the same value (`BUFFER_DEPTH = ROUTER_VC_DEPTH`, `gen_tb_top.py`).

R7 (credit-return latency, DAT). A flit granted (stage-2 dequeue) in cycle N produces its
credit pulse on the upstream-facing output wire at cycle N+2: the core registers the
pulse one cycle (dequeue N -> core pulse N+1, verified by
`CreditDecrementAtGrantAndPulseAfterDequeue`), and the SV output register adds one more
(wire sampled N+2, `router_wrap.sv:248-264`).

R8 (transaction gap). No minimum gap exists anywhere: 0 idle cycles between flits,
between packets, and between a credit pulse and the flit it enables are all legal.

R9 (reset). `rst_ni` is synchronous active-low and is given only once, at the
beginning of simulation, before `cmodel_router_create` and before any traffic. While
`rst_ni` is low, every output register is 0. The C++ model resets by construction (it
is created after reset deassertion). Mid-simulation reset does not occur and is not
modeled.

R10 (latency definition). Per-hop latency is measured from the posedge at which a flit
is sampled on an input pin to the posedge at which it is sampled on the corresponding
output pin (this module's registered output, as seen by the next sampler). At zero
load (no contention on the granted output, nonzero credit, output FIFO below depth)
this latency is exactly 3 cycles per hop on DAT and 2 on REQ/RSP (section 2.4).

R11 (output uniqueness). At most one flit per output port per network per cycle: each
bit of `tx_req_valid` / `tx_rsp_valid` / `tx_dat_valid` covers exactly one flit bus.

R12 (VC on the wire, DAT). The `vc_id` field of an output flit equals the `vc_id` it
arrived with ONLY when `fixed_vc = 1` (NI-pinned); for `fixed_vc = 0` the VA stage
restamps `vc_id` with the assigned output VC, which may differ from the arrival VC.
Per hop, the credit pulse back to the sender carries the ARRIVAL (input-side) VC —
the FIFO slot freed — while the flit's onward credit is consumed on the restamped
VC.

### 3.6 Input guarantees

The environment (NMU, NSU, neighbor routers, generated fabric) guarantees the
following. The implementer does not handle these cases. The model enforces each with
an abort or assertion at the cited line, so any violation is an environment bug, not a
router obligation.

| # | Guarantee | Model enforcement |
|---|---|---|
| G1 | Never two flits on one input port of one network in one cycle | abort, `router.hpp:194-197` |
| G2 | Every valid DAT flit has `vc_id < DAT_NUM_VC`; REQ/RSP flits carry `vc_id` = 0 | abort, `router.hpp:182-185`; SVA `link_perf_monitor.sv:69-72` |
| G3 | Every valid flit has `dst_id` inside the mesh (`dst_x < mesh_x_dim`, `dst_y < mesh_y_dim`). The NMU SAM lookup validates destinations at packetize time, so an out-of-mesh `dst_id` cannot happen | abort, `router.hpp:65-68` |
| G4 | On DAT, no sender drives a flit on VC v while its credit for that (port, VC) is 0 | input FIFO overflow assert, `router.hpp:284-286`; SVA `link_perf_monitor.sv:61-64` |
| G5 | Packets are well-formed per (input, VC): after a head (`flit_tail=0`), every following flit on that (input, VC) routes to the same output until a tail (`flit_tail=1`) closes the packet. Guaranteed because all flits of a packet share `dst_id` | abort, `router.hpp:228-234` |
| G6 | On DAT, no credit pulse arrives beyond the outstanding flit count (counter never exceeds the seed of 8) | abort, `router.hpp:111-114` |
| G7 | Every response flit has `flit_tail = 1'b1` — every B and every R beat is a single-flit packet, on RSP and on DAT alike | consequence: only an AW+W request packet ever holds a wormhole lock across cycles |
| G8 | `rst_ni` is given once at simulation start; the handle from `cmodel_router_create` is valid and constant | tb_top sequencing, `gen_tb_top.py` |
| G9 | Boundary-direction inputs are tied to 0 and never pulse | generated tie-off, `gen_tb_top.py` |

## 4. Specifications

Each item names where it is verified and what constitutes failure. ctest names refer
to `src/c_model/tests/router/test_router.cpp`. "Co-sim scoreboard" is the per-transaction
write -> readback compare of the co-simulation testbench (`make sim TB=<topology>`),
which fails on any data or ordering divergence from this model.

SPEC 1 (interface). The top module is `router_wrap` with exactly the ports and
parameters of sections 3.1-3.3. Verified at build and at the start of the generated
testbench. Failure: build/port-binding error, or the `initial`-block `$fatal` guard
that fires at time 0 when `$bits(noc_credit_t) != DAT_NUM_VC` (`router_wrap.sv`).

SPEC 2 (reset). While `rst_ni` is 0, every output signal is 0. After the single
reset, the block starts with empty FIFOs, no locks, and all DAT credit counters at 8.
Verified by the tb_top reset window preceding all traffic. Failure: any nonzero
output during reset trips the boundary checks or the co-sim scoreboard.

SPEC 3 (routing). Every flit leaves on the port given by XY dimension-order routing
of its `dst_id` (section 2.3), recomputed at every hop. Verified by ctest
`RouterRouteCompute.XyDimensionOrder` and the co-sim scoreboard (a misroute delivers
data to the wrong NSU). Failure: wrong output port on any flit.

SPEC 4 (zero-load latency). Input-pin sample edge to output-pin sample edge is exactly
3 cycles on DAT and 2 on REQ/RSP, when the granted output is uncontended, has credit or
ready, and its output FIFO is below depth. Verified by ctest
`RouterDatapath.ZeroLoadLatencyIsThreeTicks` and
`SimpleRouterDatapath.ZeroLoadLatencyDirectModeTwoTicks`. Failure: the flit appears on
the output wire earlier or later than that edge.

SPEC 5 (bit transparency). Every flit leaves bit-identical to how it entered — all
bits, header and payload — except the header `vc_id` field, which the VA stage
restamps for `fixed_vc = 0` flits (rule R12). The router writes nothing else.
Verified by ctest `RouterDatapath.HeaderTransparency` (byte-for-byte compare of the
whole flit at `DAT_NUM_VC = 1`, where the restamp is the identity) and by the co-sim
scoreboard readback. Failure: any flipped bit outside `vc_id`.

SPEC 6 (VC handling). A `fixed_vc = 1` flit keeps its header `vc_id` end to end:
filed under it at stage 1, onward credit consumed on it, departs with it. A
`fixed_vc = 0` flit is filed under its arrival `vc_id`, then the VA stage assigns
the departure VC (preferred map + FVADA overflow, section 2.5), consumes credit on
the ASSIGNED VC, and restamps `vc_id`. Verified by ctest
`RouterGrid.EndToEndTrafficAcrossParameterSpace` (pinned `vc_id` preserved across
the parameter space), `RouterVaCredit.ConsumeStampedVcReturnInputVc` (credit
consumed on the assigned VC, upstream pulse on the arrival VC),
`RouterVaFvada.PreferredThenHighestIndexOverflowThenStall` (assignment order), and
`RouterVaFabric.MultiHopPinnedVcPreservedUnderContention` (pin across hops).
Failure: a pinned flit's `vc_id` differs between ingress and egress, or credit
activity on a VC the flit was not assigned to.

SPEC 7 (credit decrement point, DAT). The per-(output, VC) counter is seeded to
`NOC_ROUTER_VC_DEPTH` and decremented exactly at the grant event (admission into the
output FIFO), not at link traversal. Verified by ctest
`RouterDatapath.CreditDecrementAtGrantAndPulseAfterDequeue` (counter reads the seed after
stage 1, one less after the stage-2 grant). Failure: counter value
wrong at either observation point, or the model underflow assert (`router.hpp:262`).

SPEC 8 (credit pulse discipline). Each credit output bit pulses for exactly 1 cycle
per freed slot, at most one pulse per (port, VC) per cycle, and the pulse for a grant
in cycle N is on the wire at cycle N+2 (rule R7). Verified by ctest
`RouterDatapath.CreditDecrementAtGrantAndPulseAfterDequeue` (core N+1 half) plus the
registered SV output (SV wire half), and by
`RouterCredit.ConservationAcrossChainedRouters` for end-to-end conservation. Failure:
missing, doubled, widened, or mistimed pulse (conservation breaks, or the model
overflow abort of `RouterCreditDeath.OverflowAborts` fires).

SPEC 9 (never send without credit). The block never asserts a flit valid toward a
(port, VC) whose credit is 0. Verified by the `link_perf_monitor.sv` assertion on
every live directed edge: `valid && credit[vc_id] == 0` raises
`$error("[%s] credit underflow on VC%0d ...")`. Failure: that assertion fires.

SPEC 10 (wormhole non-interleave). Flits of two packets never interleave on one
output: from a granted head (`flit_tail=0`) to its tail (`flit_tail=1`), the output serves only
the locked (input, VC). Verified by ctest
`RouterWormhole.PacketsDoNotInterleavePerOutputVc` and
`RouterWormhole.OpenPacketHoldsOutputAndBlocksOtherVc`. Failure: any foreign flit
between a head and its tail on one output.

SPEC 11 (lock persistence). A locked output whose locked (input, VC) is empty or
credit-blocked idles that cycle and keeps the lock. It never grants another candidate.
Verified by ctest `RouterWormhole.LockedEmptyVcIdlesButDoesNotLoseLock`. Failure: a
grant to a non-locked candidate while locked.

SPEC 12 (arbitration order). Unlocked outputs select by VC-major, input-minor
round-robin with the tie-break of section 2.5, and both pointers advance only on a
tail grant. Verified by ctest `RouterWormhole.RrAdvancesPerPacket` and
`RouterVcArbitration.FlitLevelRrAcrossVcs`. Failure: grant sequence deviates from the
scan-order prediction.

SPEC 13 (VC independence at an output). A zero-credit VC is skipped before its
inputs are examined and never blocks another VC at an unlocked output. Verified by
ctest `RouterVcArbitration.BlockedVcDoesNotStallOthers`. Failure: traffic on a
credited VC stalls while another VC is credit-blocked and the output is unlocked.

SPEC 14 (output FIFO). Stage 2 admits no flit to an output whose FIFO holds
`NOC_ROUTER_OUTPUT_FIFO_DEPTH` (default 2, range 1..16) flits, and a FIFO that drains
one flit in stage 3 can accept one grant in the same cycle. Verified by ctest
`RouterVcArbitration.SameCycleOutputFifoEnqueueDequeue`. Failure: a grant into a full
FIFO, or a stall in the same-cycle drain-and-fill case.

SPEC 15 (multi-grant per cycle). Stage 2 evaluates the five outputs in fixed index
order LOCAL, NORTH, EAST, SOUTH, WEST and may grant up to 5 flits per cycle, including
several pops of the same input VC FIFO in one cycle (section 2.4). Wire behavior stays
bounded by R5 and R11. Verified by the co-sim cycle compare against this model under
the regression patterns (`sim/run_regress.py`), with `link_perf_monitor` guarding the
wire bounds. Failure: cycle-level divergence from the model, or a wire-bound
assertion.

SPEC 16 (fairness). Under sustained contention, every competing (input, VC) is
granted infinitely often (round-robin starvation freedom). Verified by ctest
`RouterFairness.AllToOneNoStarvation` (all inputs target one output, delivery counts
stay balanced). Failure: any starved input in that test.

SPEC 17 (boundary silence). A boundary direction (no neighbor) never asserts
`link_*_out_valid`. Verified by the generated fabric assertion: `$fatal(1,
"noc_fabric: node%0d drove a flit on tied-off ...")` (`gen_tb_top.py:354-368`).
Failure: that `$fatal`.

SPEC 18 (network independence). The REQ, RSP and DAT routers share no state: traffic,
stalls, ready deassertion, or credit exhaustion on one network never affects the other
two. Verified by structure (three separate model instances) and by the co-sim scoreboard
under bidirectional regression traffic. Failure: cross-network coupling observable as a
scoreboard divergence.

SPEC 19 (parameter legality). Construction rejects (assert then abort) exactly three
conditions: `DAT_NUM_VC` outside 1..8 (= 2^VC_ID_WIDTH), a zero VC depth or zero output
FIFO depth, and an own coordinate outside the mesh (`router.hpp:77-88`). The upper
bounds on VC depth, output FIFO depth, and mesh dims (VC depth and output FIFO depth
stated as 1..16, mesh dims stated as 2..16) are design assumptions bounded by the
flit field widths (X and Y coordinate 4 bits each), not construction-checked. The
mesh-dim lower bound (2 per dimension: a mesh communicating through NI + router
needs at least 2x2; 1x1/1xN illegal) is enforced at topology load time
(`gen_tb_top.py`, `gen_test_patterns.py`, `sam_yaml.hpp::load_sam_table`), not by
Router construction. Verified by ctest death tests
`RouterConstructionDeath.BadParametersAbort` (covers `num_vc = 9` and `vc_depth = 0`),
`RouterRouteComputeDeath.DstOutsideMeshAborts`, `RouterDatapathDeath.BadVcIdAborts`.
Failure: construction succeeds on `dat_num_vc` outside 1..8, a zero depth, or an
out-of-mesh coordinate.

SPEC 20 (multicast fork). A head flit with `collective_op != UNICAST` leaves on the
multi-hot branch set of `route_mask_fork` instead of the one-hot `route_compute` result;
each branch arbitrates and locks on its own; the input FIFO pops and the single upstream
credit pulse leaves only once every expected branch has accepted; every continuation of
the worm recomputes its own branch set and aborts on divergence (section 2.10). An empty
branch set at a router the flit reached is fatal. Verified by ctest
`RouterFork.OneHotForkSetIsBitIdenticalToPlainUnicast` and the `test_router_fork.cpp` /
`test_simple_router_fork.cpp` suites, and by the co-sim `multicast` pattern. Failure: a
replica short of the branch set, more than one credit pulse per forked flit, or a
continuation on a branch set its own header does not name.

SPEC 21 (CollectB join, RSP only). A CollectB (`collective_op != UNICAST` with `axi_ch`
in {`NarrowB`, `DataB`}) is never a unicast candidate; it is forwarded once, as one whole
input flit, only when every input of `route_mask_join` holds a head of the same collect,
and every contributing head pops in that same handshake. `BRESP` precedence is
first-`SLVERR` in route-index order with `DECERR` never elevated; the join is stateless
and re-evaluates every tick; it holds while its output is mid-worm and grants at the
boundary. An empty expected set, a CollectB arriving outside its own expected set, a
collective on an RSP read channel, or joined heads disagreeing on `ordering_tag` /
`axi_ch` / `bid` are each fatal (section 2.10). Verified by ctest
`SimpleRouterJoin.FirstSlverrInRouteIndexOrderWins`,
`SimpleRouterJoin.DecerrIsNotElevated`,
`SimpleRouterJoinChain.MidWormHoldKeepsTheDownstreamLatchIntact` and the rest of
`test_simple_router_join.cpp`, plus the co-sim `multicast` pattern's merged-B checks.
Failure: more than one `B` per collective reaching the NMU, a merge before every replica
arrived, or a rebuilt rather than forwarded header.

## 5. Block Diagram

Fabric context (generated per topology YAML, one `router_wrap` per node):

```
                          node (x, y+1)
                    port[SOUTH] tx ^ | rx
   flit + valid down, ready/credit up | v  (opposite-port pairing)
   +---------------------------------------------------------+
   |  node (x, y)               router_wrap                   |
   |                                                          |
   |   NMU --rx_req[LOCAL]-->  +--------------+ --tx_req[E]--> EAST peer
   |   NMU <-rx_req_ready------|REQ SimpleRtr | <-rx_req[E]--- (ports
   |   NSU <--tx_req[LOCAL]--  | ready/valid  | --rx_req_ready[E]->  N/E/S/W
   |   NSU --tx_req_ready----> +--------------+                |    + LOCAL)
   |                                                          |
   |   NSU --rx_rsp[LOCAL]-->  +--------------+ --tx_rsp[E]-->
   |   NSU <-rx_rsp_ready------|RSP SimpleRtr | <-rx_rsp[E]---
   |   NMU <--tx_rsp[LOCAL]--  | ready/valid  |   ...
   |   NMU --tx_rsp_ready----> +--------------+                |
   |                                                          |
   |  DatMerge --rx_dat[LOCAL]-> +------------+ --tx_dat[E]-->
   |  (NMU AW/W + NSU R)         | DAT Router | <-rx_dat[E]---
   |  DatMerge <-tx_dat[LOCAL]-- | credit, VA | --rx_dat_crdvalid[E]->
   |  (axi_ch demux to NMU/NSU)  +------------+ <-tx_dat_crdvalid[E]-
   +---------------------------------------------------------+
                     port[NORTH] of node (x, y-1)

   one passive link_perf_monitor per live directed edge (req + rsp + dat),
   BUFFER_DEPTH = ROUTER_VC_DEPTH, asserts SPEC 9 / G2 on the wire
```

One Router (per network), 3-stage pipeline, 5 in / 5 out ports:

```
 DAT Router: input port p (x5: LOCAL,N,E,S,W)          output port q (x5)
 --------------------------------                      -----------------
                    stage 1              stage 2                stage 3
 flit ---> [input_reg_ 1-deep] --vc_id--> [FIFO vc0, depth 8] \
                                          [FIFO vc1, depth 8] -+--> per-output q:
                                              ...              |    wormhole lock
                                          [FIFO vcN-1]        -+    (locked_input,
                                                               |     locked_input_vc,
                                                               |     locked_output_vc)
                                       route_compute(dst_id)   |    VC RR vc_rr_[q]
                                       at each FIFO head ------+    input RR ws.rr
                                                               |    credit_[q][vc]
                                                               |    (seed 8, -- at
                                                               |     grant)
                                                               v
                                            [output_fifo_[q], depth 2] --> link
 credit pulse to upstream of p  <--- registered 1 cycle after the
 (per VC, max 1/cycle)               stage-2 dequeue from (p, vc)
```

## 6. Sample Waveform

Both waveforms use the sampling convention of section 1: the value shown in column N
is the value sampled at posedge N.

Waveform 1: zero-load single-flit DAT hop plus credit return. Node A (0,0), single-flit
packet F (`flit_tail=1`, VC0, `dst_id`=8'h01) injected by the NMU, routed EAST.

```
cycle (posedge idx)      |  0 |  1 |  2 |  3 |  4 |
-------------------------+----+----+----+----+----+
rx_dat_valid[LOCAL]      |  1 |  0 |  0 |  0 |  0 |   <- valid exactly 1 cycle (R4)
rx_dat_flit[LOCAL]       |  F |  0 |  0 |  0 |  0 |   <- all-zero when valid low (R2)
A internal stage of F    | S1 | S2 | S3 |    |    |   <- one stage per cycle
A credit_[EAST][0]       |  8 |  7 |  7 |  7 |  7 |   <- decrement at grant (SPEC 7)
tx_dat_valid[EAST]       |  0 |  0 |  0 |  1 |  0 |   <- sampled by peer at N+3:
tx_dat_flit[EAST]        |  0 |  0 |  0 |  F |  0 |      zero-load latency 3 (SPEC 4)
rx_dat_crdvalid[LOCAL][0]|  0 |  0 |  0 |  1 |  0 |   <- LOCAL dequeue at cycle 1,
                                                         pulse on wire at 1+2 = 3 (R7)
```

Annotations: reset was given once, before cycle 0 (R9). The grant is at cycle 1, the
core-internal credit pulse fires at cycle 2, the registered wire pulse is sampled at
cycle 3 (grant N -> core pulse N+1 -> SV wire N+2).

Waveform 2: 3-flit DAT wormhole packet (section 2.9), link A(0,0).EAST -> B(1,0).WEST,
VC0, injected cycles 0..2. Credit counter shown as its value entering each cycle.

```
cycle (posedge idx)         |  0 |  1 |  2 |  3 |  4 |  5 |  6 |  7 |  8 |
----------------------------+----+----+----+----+----+----+----+----+----+
A rx_dat_valid[LOCAL]       |  1 |  1 |  1 |  0 |  0 |  0 |  0 |  0 |  0 |  3 flits,
A rx_dat_flit[LOCAL]        | F0 | F1 | F2 |  0 |  0 |  0 |  0 |  0 |  0 |  0-gap (R8)
A wormhole_[EAST] lock      |  - |  L |  L |  - |  - |  - |  - |  - |  - |  head locks,
A credit_[EAST][0]          |  8 |  8 |  7 |  6 |  5 |  5 |  5 |  6 |  7 |  tail frees
A tx_dat_valid[EAST]        |  0 |  0 |  0 |  1 |  1 |  1 |  0 |  0 |  0 |  no
A tx_dat_flit[EAST]         |  0 |  0 |  0 | F0 | F1 | F2 |  0 |  0 |  0 |  interleave
A rx_dat_crdvalid[LOCAL][0] |  0 |  0 |  0 |  1 |  1 |  1 |  0 |  0 |  0 |  (SPEC 10)
B rx_dat_crdvalid[WEST][0]  |  0 |  0 |  0 |  0 |  0 |  0 |  1 |  1 |  1 |  B grants at
B tx_dat_valid[LOCAL]       |  0 |  0 |  0 |  0 |  0 |  0 |  1 |  1 |  1 |  4,5,6 ->
B tx_dat_flit[LOCAL]        |  0 |  0 |  0 |  0 |  0 |  0 | F0 | F1 | F2 |  wire 6,7,8
```

Annotations: lock row `L` = EAST locked to (LOCAL, VC0), set by the F0 grant at
cycle 1, released by the F2 tail grant at cycle 3. `credit_[EAST][0]` bottoms at 5
with three flits outstanding and is back to the seed 8 after cycle 8 (conservation,
SPEC 8). Head latency 6 = 2 hops x 3 cycles. Each credit wire pulse is exactly 1 cycle
wide, one per freed slot (R5).

## 7. Appendix: Hints

Non-binding implementation notes.

Hint: evaluating the three stages in reverse order (3, 2, 1) inside one clock
evaluation reproduces two required behaviors at once: a flit advances exactly one
stage per cycle, and a full output FIFO that drains in stage 3 accepts a grant in the
same cycle (SPEC 14). Any structure with the same observable cycle behavior is
equally acceptable.

Hint: on unicast traffic no datapath logic ever needs to decode `axi_ch`, `src_id`,
`ordering_req`, `ordering_tag`, or the payload. Flit bits [11:0] and [43:25] plus the
whole payload form an opaque bundle steered by the 13 control bits
{`flit_tail` [24], `vc_id` [23:21], `fixed_vc` [20], `dst_id` [19:12]}. Collectives add
`collective_op` [35:34] and `collective_mask` [43:36], and the RSP join additionally
reads `axi_ch`, `src_id`, `ordering_tag` and the payload's `bresp` / `bid`.

Hint: the per-(port, VC) DAT credit counter needs
ceil(log2(`NOC_ROUTER_VC_DEPTH` + 1)) = 4 bits at the default depth 8 (values 0..8).
