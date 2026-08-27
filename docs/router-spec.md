# Design: NoC Router (ROUTER)

Block-level design spec for the per-node mesh router of the NoC C++ behavior model.
The reader implements an RTL block whose cycle behavior is checked against this model
by the existing testbench. As-built references:

The target LOCAL DAT receive-FIFO timing is not implemented by the current model. Cycle-exact RTL
comparison for that port starts only after the model and wrapper are aligned; N/E/S/W credit
behavior remains the as-built reference.

The production top is `router`. Its wrapper-facing ports, fixed five-port hierarchy, and reviewed
child boundaries are frozen in `rtl/README.md`; this document remains authoritative for behavior.

The target Router belongs entirely to the `noc_clk` domain and receives only `noc_rst_n`. System
integration derives that reset and each NI's `ARESETn` from one common system reset; assertion is
asynchronous and deassertion is synchronized to the destination clock. The Router has no
`sys_rst_n` port. The current C++/DPI model retains the single synchronous initial-reset behavior
specified below.

| Layer | File |
|---|---|
| Router core (per network) | `ref_model/c_model/include/router/router.hpp` |
| Per-node wrap (REQ + RSP `SimpleRouter`, DAT `Router`, DPI I/O latch) | `ref_model/c_model/include/wrap/router_wrap.hpp` |
| SV DPI module (top-level pin contract) | `ref_model/top/router_wrap.sv` |
| Unit tests | `ref_model/c_model/tests/router/test_router.cpp` (DAT), `test_simple_router.cpp` (REQ/RSP), `test_route_mask.cpp` + `test_*_fork.cpp` + `test_simple_router_join.cpp` (collectives) |
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
48-bit header and a per-network payload. A link moves at most one flit per direction
per cycle.

| Network | flit width | payload | Flow control |
|---|---|---|---|
| REQ | 136 | [135:48], 88 b | ready/valid, 1 VC |
| RSP | 126 | [125:48], 78 b | ready/valid, 1 VC |
| DAT | 633 | [632:48], 585 b | credit, `NUM_VC` 1..8 |

This is the fixed `NOC_ID_WIDTH = 3` layout: REQ is 136 bits, RSP is 126 bits, and DAT is 633
bits. A router consumes these generated package widths and does not select flit widths independently.

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
after draining one flit from that input FIFO. Example with depth 8: a sender can transfer 8
back-to-back flits on VC 0, must then idle at credit 0, and resumes one flit per
returned pulse.

The paragraph above is the uniform five-port rule for the target RTL. LOCAL is also symmetric:
NI-to-Router credits represent this Router's LOCAL input VC FIFO, while Router-to-NI credits
represent the destination NI's per-VC receive FIFO. The LOCAL sender counters are seeded from
`NOC_NI_DAT_RX_VC_DEPTH`; N/E/S/W sender counters use `NOC_ROUTER_VC_DEPTH`.

REQ and RSP use ready/valid instead. The C++ core computes an almost-full early ready from
current occupancy, `ready = (occupancy + almost_full_offset <= depth)`. At the model-facing wire the
verification wrapper applies the standard contract: a transfer occurs only with `valid && ready`,
and a source holds valid plus flit while stalled. The wrapper converts each accepted wire transfer
back to the one-cycle ingress pulse expected by the C++ core and holds each one-cycle core
egress strobe in a single hold register freed by the wire handshake. The shipped
`almost_full_offset` = 2 is co-sim-calibrated: the worst measured overrun past the deassert
threshold is one entry, so 2 keeps one entry of margin (`simple_router.hpp`
`SimpleRouterConfig::almost_full_offset`).

### 2.2 Flit format

One 48-bit header layout on all three networks, from
`specgen/generated/cpp/ni_flit_constants.h`. Header occupies flit bits [47:0], payload
occupies the rest (REQ [135:48], RSP [125:48], DAT [632:48]).

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
| `dst_port_id` | [45:44] | 2 | Which endpoint at `dst_id` receives. 0 is the tile on the router's LOCAL port, non-zero a boundary-port peripheral. Read by the router at the destination coordinate: it selects the ejection port. |
| `src_port_id` | [47:46] | 2 | Which endpoint at `src_id` issued. The response is addressed back to it. Transparent to the router. |
| payload | per network | 88 / 78 / 585 | AXI channel payload. Transparent to the router. |

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

### 2.4 Pipeline: four stages, one stage per cycle (DAT)

The DAT `Router` is a 4-stage pipeline, the canonical input-buffered VC router. A flit advances
exactly one stage per cycle. A head flit passes all four stages; a body or tail flit skips
RC + VA and inherits the route and output VC its head obtained, so it passes three. A
single-flit packet is a head and a tail at once and pays four. Per router: DAT head 4 cycles,
DAT body or tail 3 (`RouterDatapath.ZeroLoadLatencyIsFourTicks`,
`RouterDatapath.BodyFlitsFollowHeadOneCycleApart`). The REQ/RSP `SimpleRouter` is unchanged: two
stages with `output_fifo_depth` = 0, the second driving the link, 2 cycles per router
(`SimpleRouterDatapath.ZeroLoadLatencyDirectModeTwoTicks`). The wrapper egress hold register
loads on the edge the model emits, so it adds no cycle at zero load. It adds cycles only while
the downstream ready is low.

| Stage | Storage | Action per cycle |
|---|---|---|
| 1. BW | per-port 1-deep input register, then per-(port, VC) FIFO, depth `NOC_ROUTER_VC_DEPTH` = 8 | file the registered flit into the FIFO selected by header `vc_id` |
| 2. RC + VA | per-(input, VC) state `ivc_`, per-(output, VC) wormhole lock, VA RR state | per output: pick one head at the front of a candidate input VC, route-compute it, and grant it one free credited output VC (section 2.5). On success record `out_vc` for that branch and lock (output, `out_vc`) to that (input, input VC). At most one VA grant per output per cycle, not gated by the output FIFO |
| 3. SA + ST | credit counters, `fork_done_`, output FIFO | per output whose FIFO is below depth: round-robin from `vc_rr_[out]` over the VCs a packet HOLDS, grant one flit, pop its input FIFO front, decrement `credit_[out][out_vc]`, restamp header `vc_id = out_vc`, push into the output FIFO, schedule one credit pulse (input-side VC) to the upstream of that input |
| 4. LT | per-output FIFO, depth `NOC_ROUTER_OUTPUT_FIFO_DEPTH` = 8 | drive at most one flit from each output FIFO onto the link |

Per input VC the model keeps the textbook G/R/O state (`InputVcState`, `router.hpp:458-467`):
`active`, true while some branch holds an output VC; `out_vc` per branch output; and
`head_parked`, true from the VA grant until that flit leaves the input FIFO. R, the route, is
not stored — it is a pure function of the parked flit's header and is recomputed where it is
needed. While the head is parked, SA skips its continuation checks (the `fixed_vc = 0`
preferred-VC check and the F9 branch-set check, which apply to continuations only) and a fork's
remaining branches may still allocate their own output VC off that same head.

In target RTL, stage 3 checks per-VC credit for N/E/S/W outputs. For LOCAL output it checks output
FIFO space but does not decrement a per-VC credit counter. Stage 4 holds the LOCAL flit stable until
the NI returns a credit. Input processing is unchanged: LOCAL DAT arrivals are filed by `vc_id`
and their dequeue returns the matching credit to the injecting NI.

The model evaluates stages in reverse order (4, then 3, then 2, then 1) within one tick
(`router.hpp:523-747`). Three observable consequences:

- An output FIFO that is full and drains one flit in stage 4 can accept one
  new grant in stage 3 of the same cycle.
- SA runs before VA, so an output VC a tail frees in stage 3 is allocatable by VA in stage 2 of
  the same cycle. Back-to-back single-flit packets on one input VC therefore leave at one per
  cycle (`RouterDatapath.BackToBackSingleFlitsOnePerCycle`); in RTL this is the SA grant ->
  VC free -> VA allocate combinational chain.
- One input VC FIFO pops at most one flit per cycle. Route and output VC belong to the input VC,
  only its front flit is eligible, and a unicast packet holds exactly one output, so only that
  output can pop it. A fork never pops at SA: the single fork pop pass runs once per tick.
  Credit pulses toward that input's upstream are therefore at most one per (port, VC) per cycle
  by construction, not by the wrap's draining (`router_adapters.hpp` `LinkCreditOut`).

Zero-load latency per router, input wire handshake at posedge N to output wire handshake at
posedge N+k with output ready high: k = 2 on REQ and RSP, k = 4 for a DAT head and k = 3 for a
DAT body or tail whose worm already holds its output VC. A path of H hops passes H + 1 routers.
Measured on the mesh_4x4 zero-load probe (`docs/backlog.md`, Scenario 2 recipe): 3 hops,
4 routers, 8 cycles on REQ/RSP and 16 on DAT.

On DAT the LOCAL port also passes `dat_merge_wrap`, 1 cycle each way (section 3.3). The TX cycle
is the DPI wrap's output register; target RTL folds the merge into the NI and has no TX cycle.
The RX cycle matches the FlooNoC chimney's spill register.

### 2.5 Arbitration: VA a cycle before SA

VA (stage 2) and SA (stage 3) are separate cycles and separate arbitrations. VA hands an
output VC to a head; SA picks, among the VCs a packet already holds, which one moves a flit
this cycle. Each grants at most one per output per cycle.

**VA, stage 2** (`router.hpp:692-735`). An input VC (in, ivc) is a candidate for output `out`
when all of:

| Condition | Meaning |
|---|---|
| `out_vc[out]` empty | this branch does not already hold a VC |
| FIFO non-empty | there is a front flit to allocate for |
| `out` in `head_expected_mask(front)` | this output is on the front flit's route: the one-hot `route_compute` port for a unicast, a branch of the fork set for a collective |
| collective: `out` not in `fork_done_` | this branch has not already granted the parked flit |
| `!(active && !(collective && head_parked))` | the input VC is idle, or is a fork head still waiting on another branch |

Candidates are scanned input-VC-major in order `in_vc_rr_[out], in_vc_rr_[out]+1, ...` modulo
`NUM_VC`, then input-minor in order `rr_[out], rr_[out]+1, ...` modulo 5. The first candidate
whose VC assignment succeeds wins, and both pointers move to one past it. A candidate whose
assignment fails is skipped and the scan continues in the same cycle (work-conserving,
`RouterVaWorkConserving.HeadVaFailAlternateCandidateGrantedSameTick`), and neither pointer
moves for a failure, so a candidate that cannot allocate keeps its place in the rotation
(`RouterVaWorkConserving.FailedVaLeavesTheArbitrationPointers`). VA is not gated by the output
FIFO.

The VA rule (`vc_assignment`, `router.hpp:346-377`, ported from the deprecated FlooNoC
`vc_router_util` suite) calls an output VC **eligible** when it is FREE (held by no packet) and
CREDITED (`credit_[out][vc] > 0`):

| Case | `out_vc` |
|---|---|
| `fixed_vc = 1` | header `vc_id` unchanged (NI pin), granted only if that VC is eligible; never overflowed |
| `fixed_vc = 0`, preferred VC eligible | preferred VC, a pure function of (output port, next-hop XY route) (`preferred_vc`, `router.hpp:123-140`) |
| `fixed_vc = 0`, preferred not eligible, `flit_tail = 1` | the highest-index other eligible VC (FVADA overflow) |
| `fixed_vc = 0`, preferred not eligible, `flit_tail = 0` | none — a wormhole head never overflows off its preferred VC. The head stays idle and retries next cycle |

`flit_tail = 1` on a flit reaching VA means the packet is one flit, so FVADA overflow is a
single-flit-packet rule. Two `fixed_vc = 0` worms to one destination must stay on one VC to keep
write order, which is why a worm head takes its preferred VC or nothing.

Credit is not reserved at VA. SA serves a held VC only through its holder, so nobody else can
spend that VC's credit between the holder's VA and its SA.

IMPORTANT (tie-break): when several candidates want the same output in one cycle, the unique
winner is the first match in VA scan order: lowest input-VC offset from `in_vc_rr_[out]`, then
lowest input offset from `rr_[out]`. The output VC no longer filters the candidate — VA assigns
a VC after picking one, so a candidate is skipped only when no VC is eligible for it at all.
Worked example, output EAST, `NUM_VC = 2`, both EAST VCs free and credited,
`in_vc_rr_[EAST] = 1`, `rr_[EAST] = 3` (SOUTH). Candidates: front of (WEST, VC0) and front of
(SOUTH, VC1), both routing EAST, both `fixed_vc = 1` with header `vc_id` equal to their input VC.

- Input-VC scan starts at input VC1, input scan at SOUTH. (SOUTH, VC1) routes EAST and its
  pinned VC1 is eligible -> **winner (SOUTH, VC1)**, holding (EAST, VC1). `in_vc_rr_[EAST]`
  moves to 0 and `rr_[EAST]` to WEST. (WEST, VC0) is not examined this cycle and allocates
  (EAST, VC0) the next one.
- Counter-case, same state but (EAST, VC1) already held by an open worm and (WEST, VC0) not yet
  arrived: (SOUTH, VC1)'s assignment fails, EAST grants nothing, and **both pointers stay**.
  When the worm's tail frees VC1 in SA, the scan still starts at input VC1 and (SOUTH, VC1)
  allocates ahead of the later arrival.

**SA, stage 3** (`router.hpp:545-662`). Per output whose FIFO is below depth, the output scans
its own VCs in order `vc_rr_[out], vc_rr_[out]+1, ...` modulo `NUM_VC` and takes the first that
can send. Only VCs a packet HOLDS are scanned — handing a VC out is VA's job. A held VC offers
the front flit of its holder's (input, input VC) FIFO and is grantable when that FIFO is
non-empty and `credit_[out][v] > 0`. A VC that offers nothing is skipped and the scan continues,
so a held VC with an empty FIFO or no credit idles only itself. Every VC is re-arbitrated each
cycle: the lock holds the VC, not the output.

A tail cannot overflow off its worm's VC: a unicast packet holds exactly one output VC and SA
scans held VCs only, so there is no unlocked path to overflow onto. The old unlocked-slot guard
that covered this is gone with the scan it guarded.

**Target RTL overlay.** Before the VA table above is applied, `NOC_DAT_VC_MODE` defines
the eligible output-VC set. `SHARED` admits all DAT VCs. `READ_WRITE_SPLIT` admits the
lower half for `DataAw` / `DataW` and the upper half for `DataR`. A `fixed_vc = 1` flit
must already name a class-eligible VC; a mismatch is illegal. Preferred selection and
FVADA overflow scan only the eligible set. Every DAT router output uses the same mode.
The current C++ router implements `SHARED` only.

The SA grant decrements `credit_[out][v]` and restamps `vc_id = v` into the departing header.
The credit pulse to the upstream carries the INPUT-side VC (the FIFO slot freed), which after
VA can differ from the VC consumed downstream. With `NUM_VC = 1` the assignment is the identity.

The three pointers belong to different stages (`router.hpp:661`, `router.hpp:730-731`):

| Pointer | Stage | New value | Advances on |
|---|---|---|---|
| `in_vc_rr_[out]`, input VC | VA | `winner_input_vc + 1` | every VA grant, never a failed VA |
| `rr_[out]`, input | VA | `winner_input + 1` | every VA grant, never a failed VA |
| `vc_rr_[out]`, output VC | SA | `out_vc + 1` | every SA grant |

Moving the output-VC pointer on every SA grant is what makes the VCs of one output share the
link: a worm holding VC0 gives up the link for one cycle whenever another VC has a flit to
send. The two VA pointers move at packet granularity because a packet reaches VA once, at its
head, so a worm keeps its place in the input rotation for as long as it runs. For streams of
single-flit packets all three degenerate to flit-level round-robin. There is no priority or
QoS input: the flit header carries no QoS field.

### 2.6 Wormhole lock rules (per output VC)

Each (output, output VC) holds its own lock record `(locked_input, locked_input_vc,
locked_output_vc)`: the input FIFO the worm drains from, and the VA-assigned output VC
every flit of the worm departs on. `locked_output_vc` always equals the slot's own VC
index, because the lock lives in the slot the head was assigned to.

1. The VA grant to a head locks the (output, `out_vc`) slot to that (input, input VC) pair, one
   cycle before the head's own SA grant. The head is still at the input FIFO front.
2. While a slot is locked, only the locked `(locked_input, locked_input_vc)` FIFO is served
   through it. Every flit of the packet departs on `locked_output_vc` and requires
   `credit_[out][locked_output_vc] > 0`. If the FIFO is empty or that credit is 0, the slot
   offers nothing this cycle and keeps the lock, and the output's scan falls through to its
   other VCs. Other inputs and other input VCs wait on this slot, even with credit
   available. For a `fixed_vc = 0` worm the model asserts that `locked_output_vc` equals
   the recomputed preferred VC. The check runs on continuations only: while `head_parked` is
   true the front flit is the one VA itself vetted, and a single-flit packet's FVADA overflow
   VC legitimately differs from its preferred one, as does a pinned `fixed_vc = 1` worm's
   NI-chosen VC.
3. The SA grant of a flit with `flit_tail = 1'b1` releases that slot's lock and clears the
   branch's `out_vc`. The input VC returns to idle once no branch holds a VC. Neither VA
   pointer moves here.
4. A single-flit packet (`flit_tail = 1'b1` on its head) locks at VA in cycle k and releases at
   its SA grant in cycle k+1: the slot is observed locked for exactly that one cycle.
5. A collective head allocates per branch: each branch output runs the VA rule on the parked
   head independently and locks its own (output, VC) the cycle it succeeds. The head leaves the
   input FIFO only once every expected branch has granted it at SA (section 2.10).

Example: a 3-flit packet (H `flit_tail=0`, B `flit_tail=0`, T `flit_tail=1`) from
(LOCAL, VC0) to EAST, with (EAST, VC0) free and `vc_rr_[EAST] = 0` at cycle k. Cycle k: VA
grants H output VC0 and locks (EAST, VC0) to (LOCAL, VC0). Cycle k+1: SA grants H and
`vc_rr_[EAST]` moves to 1. A competing single-flit packet at (WEST, VC1) routing EAST
(`fixed_vc = 1`, header `vc_id` = 1) that VA allocated (EAST, VC1) is granted at cycle k+2
through its own VC, ahead of B. B follows at k+3 and T at k+4, with the (EAST, VC0) lock held
from k and released by T's grant. A competing head whose VA lands on (EAST, VC0) cannot
allocate at all until T's grant frees it.

The lock never spans different outputs: locking is a per-(output, VC) property, so a packet
to EAST and a packet to NORTH from two inputs proceed in parallel.

### 2.7 Credit flow control rules (DAT only)

Counter granularity is per (output port, VC): `credit_[out][vc]`. Target RTL retains these
counters on all five DAT outputs. REQ and RSP have no counters.

1. **Seed**: N/E/S/W counters start at `NOC_ROUTER_VC_DEPTH`; LOCAL counters start at
   `NOC_NI_DAT_RX_VC_DEPTH`, each equal to the corresponding downstream receive-VC FIFO depth.
2. **Decrement**: by 1 at the grant event (stage-3 SA admission into the output FIFO,
   `router.hpp:643-644`), not at VA and not at link traversal. With seed 8, eight grants toward
   one (output, VC) with no returns leave the counter at 0 and stall further grants on
   that VC. VA does not reserve credit: it refuses a VC with none, and once a VC is held only
   its holder can spend it.
3. **Increment**: by 1 per received credit pulse on that (output, VC), including LOCAL when the NI
   pops an ejected flit from its receive-VC FIFO. A current-cycle return is eligible for a
   same-cycle `transfer`.
4. **Pulse generation**: when this router's stage 3 pops one flit from input FIFO
   (p, v), it owes one pulse to the upstream of port p on VC v. The pulse is
   registered: dequeue in cycle N, pulse leaves the core at cycle N+1, and after the
   wrap's output register it is sampled on the SV wire at cycle N+2 (rule R7 in
   section 3.5).
5. **At zero credit**: a head whose only otherwise-eligible output VC has no credit is not
   allocated at VA and stays idle, so it never holds a VC it cannot spend. A held VC idles and
   keeps its lock while `credit_[out][locked_output_vc]` is 0. A zero-credit VC never stalls
   another VC of the same output. Example: `NUM_VC = 2`, `credit_[EAST][0] = 0`,
   `credit_[EAST][1] = 3` -> flits assigned to VC1 keep flowing to EAST while flits
   assigned to VC0 wait.

The input FIFO slot frees at SA, one cycle later than in the merged stage this pipeline
replaced, so the credit round trip is one cycle longer. `NOC_ROUTER_VC_DEPTH` = 8 covers it.

The stage-4 output FIFO (default depth 8) is an architectural parameter of this design
(`NOC_ROUTER_OUTPUT_FIFO_DEPTH`) but is not credit-counted and is invisible to the
neighbor. Its only flow effect is the stage-3 admission gate: no grant to an output
whose FIFO already holds `NOC_ROUTER_OUTPUT_FIFO_DEPTH` flits. VA is not gated by it, so a
head may take a VC and then wait in SA behind a full output FIFO.

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
| 0 | stage 1 BW: F0 -> fifo[LOCAL][0] | idle | A `rx_dat_valid[LOCAL]` = 1 (F0) |
| 1 | stage 2 VA: F0 takes EAST VC0, locking (EAST, VC0) to (LOCAL, 0). stage 1: F1 filed | idle | F1 in |
| 2 | stage 3 SA: grant F0, `credit_[EAST][0]` 8->7, pop. stage 1: F2 filed | idle | F2 in |
| 3 | stage 4 LT: F0 -> link. stage 3: grant F1 (7->6) | idle | — |
| 4 | stage 4: F1. stage 3: grant F2 (6->5), tail -> unlock | stage 1: F0 filed | A `tx_dat_valid[EAST]` = 1 (F0). A `rx_dat_crdvalid[LOCAL][0]` pulse (F0's LOCAL dequeue at cycle 2) |
| 5 | stage 4: F2 | stage 2 VA: F0 takes LOCAL VC0. stage 1: F1 filed | F1 on link. NMU credit pulse (F1) |
| 6 | idle | stage 3: grant F0. stage 1: F2 filed | F2 on link. NMU credit pulse (F2) |
| 7 | idle | stage 4: F0 -> eject. stage 3: grant F1 | — |
| 8 | `credit_[EAST][0]` 5->6 (B's pulse for F0) | stage 4: F1. stage 3: grant F2, tail -> unlock | B `tx_dat_valid[LOCAL]` = 1 (F0). B's `rx_dat_crdvalid[WEST][0]` pulse reaches A |
| 9 | 6->7 | stage 4: F2 | F1 to NSU. Credit pulse (F1) |
| 10 | 7->8 (fully replenished) | idle | F2 to NSU. Credit pulse (F2) |

Head latency: injected cycle 0, at the destination NI cycle 8 = 2 routers x 4 cycles. F1 and F2
skip VA and inherit F0's route and output VC, so they pass three stages each and follow the flit
ahead of them one cycle apart (`RouterDatapath.BodyFlitsFollowHeadOneCycleApart`).
Tail: cycle 2 -> cycle 10. A's `credit_[EAST][0]` bottoms at 5 (three flits in flight)
and returns to 8 by cycle 10. The same packet on REQ would take 2 cycles a router and
transfer with `tx_req_valid && tx_req_ready` instead of consuming a counter.

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
| R3 | No dedicated multicast VC, no `fixed_vc` special case | Nothing to enforce. A fork branch takes the lock of the (output, VC) its head was assigned to, exactly as a unicast worm does, and `locked_branch_set` scans every VC slot of every output |

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

`router_wrap` SV parameters (`ref_model/top/router_wrap.sv:54-63`):

| Parameter | Default | Legal range | Meaning |
|---|---|---|---|
| `NOC_DAT_NUM_VC` (`DAT_NUM_VC` wrapper alias) | 2 | 1..8 (= 2^VC_ID_WIDTH); Split requires {2,4,6,8} | VCs on the DAT link. REQ/RSP are fixed single-VC. `$fatal` at time 0 if `$bits(noc_types_pkg::noc_credit_t)` disagrees. |
| `NOC_DAT_VC_MODE` | SHARED (0) | {SHARED (0), READ_WRITE_SPLIT (1)} | Eligible-VC mask applied by every target DAT output VA; current C++ router implements SHARED only. |
| `REQ_FLIT_WIDTH` | 136 | fixed | REQ flit bus width, bits |
| `RSP_FLIT_WIDTH` | 126 | fixed | RSP flit bus width, bits |
| `DAT_FLIT_WIDTH` | 633 | fixed | DAT flit bus width, bits |
| `LINK_PORTS` | 5 | fixed 5 | port array size = {LOCAL, NORTH, EAST, SOUTH, WEST} |

Router model configuration, fixed at `cmodel_router_create` time:

| Parameter | Default | Legal range | Meaning |
|---|---|---|---|
| `NOC_ROUTER_VC_DEPTH` | 8 | power of two, >= 2 | input VC FIFO depth; on DAT it is also the upstream credit seed, on REQ/RSP the depth the almost-full `ready` is computed against |
| `NOC_NI_DAT_RX_VC_DEPTH` | `NOC_ROUTER_VC_DEPTH` (8) | power of two, >= 2 | LOCAL DAT output sender-credit seed backed by the attached NI receive-VC FIFOs |
| `NOC_ROUTER_OUTPUT_FIFO_DEPTH` | 8 | positive power of two | DAT stage-3 output FIFO depth, not credit-counted. REQ/RSP run with output FIFO depth 0 (stage 2 drives the link directly) |
| `almost_full_offset` (REQ/RSP) | 2 | 1..`NOC_ROUTER_VC_DEPTH` - 1 | entries of headroom the almost-full `ready` reserves. Co-sim-calibrated: worst measured overrun is 1 entry, 2 keeps one entry of margin |
| `mesh_x_dim`, `mesh_y_dim` | 4, 4 | 2, 4, 8, 16 each | the router array, which the generated tb_top passes from the topology's `x_dim` / `y_dim`. A peripheral shares its host router's coordinate, so it adds none. X and Y are independent for unicast; powers of two keep every encoded coordinate valid. The first RTL target guarantees multicast/collective operation only when `mesh_x_dim == mesh_y_dim`. Minimum 2 per dimension; 1x1 and 1xN meshes are illegal. |
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

The current C++/DPI wrapper and target RTL use one uniform per-port array indexed
{LOCAL, N, E, S, W}: LOCAL carries this node's own NI traffic, N/E/S/W the inter-router links.
`noc_types_pkg::noc_credit_t` = `{credit[DAT_NUM_VC-1:0]}`, one bit per VC.

> REQ/RSP at the model-facing pins use standard held ready/valid. A transfer occurs only with
> `valid && ready`; the verification wrapper converts that handshake to/from the C++ core's
> one-cycle ingress/egress strobes. DAT remains credit-controlled.

Inputs:

| Signal | Bit width | Definition |
|---|---|---|
| `clk_i` | 1 | Clock. All sequential behavior on the posedge. |
| `rst_ni` | 1 | Synchronous active-low reset. Given only once, at the beginning of simulation (rule R9). |
| `ctx_i` | 64 | Model handle returned by `cmodel_router_create`. Constant after reset. From tb_top. |
| `rx_req_valid` | 5 | Bit p: the sender at port p drives one REQ flit this cycle. Bit 0 is the local NI's injection. |
| `rx_req_flit` | 136 x 5 (unpacked `[LINK_PORTS]`) | REQ flit from port p. Valid only when `rx_req_valid[p]` is high, all zeros otherwise. |
| `tx_req_ready` | 5 | Bit p: the receiver at port p accepts a REQ transfer when this and `tx_req_valid[p]` are high. |
| `rx_rsp_valid` / `rx_rsp_flit` / `tx_rsp_ready` | 5 / 126 x 5 / 5 | RSP mirror. |
| `rx_dat_valid` | 5 | Bit p: the sender at port p drives one DAT flit this cycle. |
| `rx_dat_flit` | 633 x 5 | DAT flit from port p. |
| `tx_dat_crdvalid` | DAT_NUM_VC x 5 (unpacked) | Per-VC credit pulse from the receiver at port p, for a DAT flit this node previously sent out of its p output. Increments `credit_[p][vc]`. |

Outputs (all registered, reset to 0):

| Signal | Bit width | Definition |
|---|---|---|
| `tx_req_valid` | 5 | Bit p: one REQ flit driven toward port p this cycle. Boundary bits always 0. |
| `tx_req_flit` | 136 x 5 | REQ flit toward port p. All zeros when `tx_req_valid[p]` is low. |
| `rx_req_ready` | 5 | Bit p: this node can take a REQ flit on port p (almost-full ready, section 2.1). |
| `tx_rsp_valid` / `tx_rsp_flit` / `rx_rsp_ready` | 5 / 126 x 5 / 5 | RSP mirror. |
| `tx_dat_valid` | 5 | Bit p: one DAT flit driven toward port p this cycle. Boundary bits always 0. |
| `tx_dat_flit` | 633 x 5 | DAT flit toward port p. All zeros when `tx_dat_valid[p]` is low. |
| `rx_dat_crdvalid` | DAT_NUM_VC x 5 | Per-VC credit pulse to the sender at port p: this node drained one flit from its p-direction DAT input FIFO, VC v. |

The tables above are the uniform target interface. `tx_dat_crdvalid` and `rx_dat_crdvalid` remain
per VC on every port, including LOCAL.

Target NI-edge flow control (LOCAL port, who answers whom): REQ/RSP use ready/valid. Both DAT
directions use receiver-owned per-VC credits.

| Flow | Flit pin | Back-pressure pin (opposite direction) |
|---|---|---|
| NMU injects REQ | `rx_req_valid/flit[LOCAL]` | `rx_req_ready[LOCAL]` (router -> NMU) |
| Router ejects REQ to NSU | `tx_req_valid/flit[LOCAL]` | `tx_req_ready[LOCAL]` (NSU -> router, tied true) |
| NSU injects RSP | `rx_rsp_valid/flit[LOCAL]` | `rx_rsp_ready[LOCAL]` (router -> NSU) |
| Router ejects RSP to NMU | `tx_rsp_valid/flit[LOCAL]` | `tx_rsp_ready[LOCAL]` (NMU -> router, tied true) |
| NI injects DAT | `rx_dat_valid/flit[LOCAL]` | `rx_dat_crdvalid[LOCAL]` (router -> NI) |
| Router ejects DAT to the NI | `tx_dat_valid/flit[LOCAL]` | `tx_dat_crdvalid[LOCAL]` (NI -> router) |

The current model's LOCAL DAT port is shared through `dat_merge_wrap`. Target integration keeps
the same class merge/demux function and terminates Router-to-NI credits in NMU DataR or NSU
DataAw/DataW receive-VC FIFOs. `dat_merge_wrap` adds 1 cycle on injection (NI DAT pins to
router LOCAL ingress) and 1 cycle on ejection (router LOCAL egress to NI DAT pins). REQ and RSP
have no merge stage.

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
| `cmodel_router_{req,rsp,dat}_set_inputs(ctx, ...)` | posedge, step 1 (one call per network) | samples the current SV wire values into the model input latch. For REQ/RSP, the wrapper passes ingress valid only on the wire's `valid && ready` transfer and passes spill-register input capacity as the model's egress ready. DAT is unchanged. Split per network so no DPI signature marshals more than one flit width |
| `cmodel_router_tick(ctx)` | posedge, step 2 | advances all three routers exactly one cycle |
| `cmodel_router_{req,rsp,dat}_get_outputs(ctx, ...)` | posedge, step 3 (one call per network) | reads the model output latch. The SV module registers these values nonblocking. REQ/RSP strobes then enter one `spill_register` per port and are held to the RTL-side handshake; DAT remains directly registered. |

Marshalling is port-major, at each network's own word count: flit = 5 (REQ) / 4 (RSP) /
20 (DAT) 32-bit words per port, DAT credit = one `[DAT_NUM_VC-1:0]` word per port,
valid and ready = one bit per port in a packed vector.

### 3.5 Protocol rules

R1 (input rhythm). At most one flit transfers per network per input port per cycle. REQ/RSP input
valid and flit may remain asserted across any number of stalled cycles and transfer only with
ready; DAT valid remains a credit-qualified one-cycle strobe. Back-to-back transfers on consecutive
cycles are legal. Flits of one packet need not be contiguous: gaps of any length may separate them
(the wormhole lock holds across gaps, rule 2.6.2).

R2 (idle bus state). When a `valid` bit is low, the corresponding flit bus carries all
zeros. This holds for the module's own outputs (registered zeros) and for
its inputs (each input wire is a peer's registered output or a fabric tie-off).
Credit vectors carry 0 in every non-pulsing bit position.

R3 (sampling edge). All inputs are sampled at the posedge of `clk_i`. All outputs are
registered and change only at the posedge. The verification environment (co-sim
scoreboard, `link_perf_monitor` assertions, boundary `$fatal` checks) samples at the
posedge.

R4 (valid behavior). On REQ/RSP, once an output `valid` bit rises it and its flit remain stable until
the cycle the matching ready is sampled high; the transfer occurs on that `valid && ready` edge.
On DAT each valid bit remains a credit-qualified one-cycle strobe, and a sender may assert it on VC
v only while its credit counter for that (port, VC) is nonzero.

R5 (credit pulse shape, DAT). Every credit signal bit is a single-cycle pulse. At most
one pulse per (port, VC) per cycle. Each pulse means exactly one freed buffer slot. One input VC
FIFO pops at most one flit per cycle (section 2.4), so the source owes at most one pulse per
(port, VC) per cycle, never a 2-cycle-wide level or a double-count.

R6 (credit seed, DAT). After reset, the sender-side counter for every (port, VC) equals
`NOC_ROUTER_VC_DEPTH` = 8. The `link_perf_monitor` on every directed DAT edge seeds its
mirror counter with the same value (`BUFFER_DEPTH = ROUTER_VC_DEPTH`, `gen_tb_top.py`).

R7 (credit-return latency, DAT). A flit granted (stage-3 SA dequeue) in cycle N produces its
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

R10 (latency definition). Per-router latency is measured from the posedge of the input transfer
to the posedge of the corresponding output transfer, with output ready high. At zero load (no
contention, nonzero DAT credit, output FIFO below depth) it is exactly 2 cycles on REQ and RSP,
and on DAT exactly 4 cycles for a head and exactly 3 for a body or tail whose worm already holds
its output VC (section 2.4). A single-flit packet is a head, so 4.

R11 (output uniqueness). At most one flit per output port per network per cycle: each
bit of `tx_req_valid` / `tx_rsp_valid` / `tx_dat_valid` covers exactly one flit bus.

R12 (VC on the wire, DAT). The `vc_id` field of an output flit equals the `vc_id` it
arrived with ONLY when `fixed_vc = 1` (NI-pinned); for `fixed_vc = 0` the VA stage assigns an
output VC and ST restamps `vc_id` with it, which may differ from the arrival VC.
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
| G4 | On DAT N/E/S/W and NI-to-Router LOCAL injection, no sender drives a flit on VC v while its credit for that (port, VC) is 0. Target Router-to-NI LOCAL ejection instead requires ready | input FIFO overflow assert, `router.hpp:284-286`; SVA `link_perf_monitor.sv:61-64` |
| G5 | Packets are well-formed per (input, VC): after a head (`flit_tail=0`), every following flit on that (input, VC) routes to the same output until a tail (`flit_tail=1`) closes the packet. Guaranteed because all flits of a packet share `dst_id` | abort, `router.hpp:228-234` |
| G6 | On credit-controlled DAT directions, no credit pulse arrives beyond the outstanding flit count (counter never exceeds the seed of 8). No target LOCAL-output counter exists | abort, `router.hpp:111-114` |
| G7 | Every response flit has `flit_tail = 1'b1` — every B and every R beat is a single-flit packet, on RSP and on DAT alike | consequence: only an AW+W request packet ever holds a wormhole lock across cycles |
| G8 | `rst_ni` is given once at simulation start; the handle from `cmodel_router_create` is valid and constant | tb_top sequencing, `gen_tb_top.py` |
| G9 | Boundary-direction inputs are tied to 0 and never pulse | generated tie-off, `gen_tb_top.py` |

## 4. Specifications

Each item names where it is verified and what constitutes failure. ctest names refer
to `ref_model/c_model/tests/router/test_router.cpp`. "Co-sim scoreboard" is the per-transaction
write -> readback compare of the co-simulation testbench (`make sim CONFIG=<config>`),
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

SPEC 4 (zero-load latency). Input transfer to output transfer at the model-facing wrapper pins is
exactly 4 cycles for a DAT head, exactly 3 for a DAT body or tail whose worm already holds its
output VC, and exactly 3 for REQ and RSP, when the granted output is uncontended, has credit
or ready, and its output FIFO is below depth. Verified by ctest
`RouterDatapath.ZeroLoadLatencyIsFourTicks` and `RouterDatapath.BodyFlitsFollowHeadOneCycleApart`
for DAT and `SimpleRouterDatapath.ZeroLoadLatencyDirectModeTwoTicks` for the 2-tick REQ/RSP core,
plus wrapper elaboration and co-sim for the spill-register cycle. Failure: the flit transfers
earlier or later than the stated edge.

SPEC 5 (bit transparency). Every flit leaves bit-identical to how it entered — all
bits, header and payload — except the header `vc_id` field, which ST restamps with the
VA-assigned output VC for `fixed_vc = 0` flits (rule R12). The router writes nothing else.
Verified by ctest `RouterDatapath.HeaderTransparency` (byte-for-byte compare of the
whole flit at `DAT_NUM_VC = 1`, where the restamp is the identity) and by the co-sim
scoreboard readback. Failure: any flipped bit outside `vc_id`.

SPEC 6 (VC handling). In the current `SHARED` C++ model, a `fixed_vc = 1` flit keeps its header `vc_id` end to end:
filed under it at stage 1, onward credit consumed on it, departs with it. A
`fixed_vc = 0` flit is filed under its arrival `vc_id`, then the VA stage assigns
the departure VC one cycle before the grant (preferred map, then FVADA overflow for a
single-flit packet only, section 2.5); the SA grant consumes credit on the ASSIGNED VC and
restamps `vc_id`. Verified by ctest
`RouterGrid.EndToEndTrafficAcrossParameterSpace` (pinned `vc_id` preserved across
the parameter space), `RouterVaCredit.ConsumeStampedVcReturnInputVc` (credit
consumed on the assigned VC, upstream pulse on the arrival VC),
`RouterVaFvada.PreferredThenHighestIndexOverflowThenStall` (assignment order), and
`RouterVaFabric.MultiHopPinnedVcPreservedUnderContention` (pin across hops).
Failure: a pinned flit's `vc_id` differs between ingress and egress, or credit
activity on a VC the flit was not assigned to. Target `READ_WRITE_SPLIT` must additionally
reject a pinned class/VC mismatch and restrict preferred/FVADA selection to the Section 2.5
eligible set; coverage is `[TBD]` until the model and RTL implement the overlay.

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

SPEC 10 (wormhole non-interleave per output VC). Flits of two packets never interleave on
one (output, VC): from a head's VA grant to its tail's SA grant, that VC
slot serves only the locked (input, VC). Packets on different VCs of one output do
interleave flit by flit, which is what the per-VC lock is for. Verified by ctest
`RouterWormhole.PacketsDoNotInterleavePerOutputVc`,
`RouterWormhole.WormsOnDifferentVcsInterleavePerOutput` and
`RouterWormhole.OpenWormOnVc0DoesNotBlockVc1`. Failure: any foreign flit between a head and
its tail on one (output, VC).

SPEC 11 (lock persistence per output VC). A locked VC slot whose locked (input, VC) is
empty or credit-blocked idles that slot and keeps the lock, while the same output keeps
granting from its other VCs. SA reaches a VC only through its holder, so no other candidate
can be granted on it. Verified by ctest `RouterWormhole.LockedEmptyVcIdlesButDoesNotLoseLock` and
`RouterWormhole.CreditStarvedVcDoesNotIdleTheOutput`. Failure: a grant to a non-locked
candidate on a locked slot, or an output idling while another VC of it could send.

SPEC 12 (arbitration order). VA selects input-VC-major from `in_vc_rr_[out]`, input-minor from
`rr_[out]`, with the tie-break of section 2.5, and grants at most one output VC per output per
cycle; both pointers advance on a VA grant and never on a failed VA. SA scans the output's own
held VCs from `vc_rr_[out]`, takes the first that can send, and advances `vc_rr_[out]` on every
grant. Verified by ctest `RouterWormhole.RrAdvancesPerPacket`,
`RouterVcArbitration.FlitLevelRrAcrossVcs` and
`RouterVaWorkConserving.FailedVaLeavesTheArbitrationPointers`. Failure: grant sequence deviates
from the scan-order prediction.

SPEC 13 (VC independence at an output). A zero-credit VC never blocks another VC of the
same output. SA skips a held VC with no credit and continues its scan (`router.hpp:560`). VA
never hands out a VC with no credit (`router.hpp:346-349`), so a head whose only otherwise
eligible VC is credit-blocked stays idle instead of holding it. Verified by ctest
`RouterVcArbitration.BlockedVcDoesNotStallOthers` and `RouterVa.ZeroCreditVcIsNeverHeld`.
Failure: traffic on a credited VC stalls while another VC of the same output is credit-blocked.

SPEC 14 (output FIFO). Stage 3 admits no flit to an output whose FIFO holds
`NOC_ROUTER_OUTPUT_FIFO_DEPTH` (default 8, positive power of two) flits, and a FIFO that drains
one flit in stage 4 can accept one grant in the same cycle. VA is not gated by the output FIFO,
so a head may hold a VC while waiting in SA behind a full one. Verified by ctest
`RouterVcArbitration.SameCycleOutputFifoEnqueueDequeue`. Failure: a grant into a full
FIFO, or a stall in the same-cycle drain-and-fill case.

SPEC 15 (withdrawn). Route and output VC belong to the input VC since VA became its own stage,
so only the front flit of an input VC is eligible and only the output holding it can pop it:
one input VC FIFO pops at most one flit per cycle (section 2.4) and the multi-pop property this
item pinned no longer exists.

SPEC 16 (fairness). Under sustained contention, every competing (input, VC) is
granted infinitely often (round-robin starvation freedom). VA's two pointers advance on a grant
only and never on a failed VA, so a candidate that cannot allocate keeps its place in the
rotation instead of losing its turn. Verified by ctest
`RouterFairness.AllToOneNoStarvation` (all inputs target one output, delivery counts
stay balanced) and `RouterVaWorkConserving.FailedVaLeavesTheArbitrationPointers`. Failure: any
starved input in those tests.

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
conditions: `DAT_NUM_VC` outside 1..8 (= 2^VC_ID_WIDTH), a VC/input or output FIFO depth
that violates its power-of-two/minimum rule, and an own coordinate outside the mesh
(`router.hpp:77-88`). FIFO depths have no architectural maximum. The mesh dimensions
remain bounded by the flit field widths (X and Y coordinate 4 bits each). The
mesh-dim lower bound (2 per dimension: a mesh communicating through NI + router
needs at least 2x2; 1x1/1xN illegal) is enforced at topology load time
(`gen_tb_top.py`, `gen_test_patterns.py`, `sam_yaml.hpp::load_sam_table`), not by
Router construction. Verified by ctest death tests
`RouterConstructionDeath.BadParametersAbort` (covers `num_vc = 9` and illegal FIFO depths),
`RouterRouteComputeDeath.DstOutsideMeshAborts`, `RouterDatapathDeath.BadVcIdAborts`.
Failure: construction succeeds on `dat_num_vc` outside 1..8, an illegal FIFO depth, or an
out-of-mesh coordinate.

The target RTL also rejects `NOC_DAT_VC_MODE=READ_WRITE_SPLIT` unless `DAT_NUM_VC` is
one of {2, 4, 6, 8}. This elaboration guard is `[TBD]`; the current C++ model has no mode
parameter.

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

Fabric context (one `router_wrap` per node, sized from the selected config file):

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

One Router (per network), 4-stage pipeline, 5 in / 5 out ports:

```
 DAT Router: input port p (x5: LOCAL,N,E,S,W)          output port q (x5)
 --------------------------------                      -----------------
             stage 1 BW         stage 2 RC+VA      stage 3 SA+ST   stage 4 LT
 flit ---> [input_reg_ 1-deep] --vc_id--> [FIFO vc0, depth 8] \
                                          [FIFO vc1, depth 8] -+--> per-output q:
                                              ...              |    lock per (q, vc)
                                          [FIFO vcN-1]        -+    (locked_input,
                                                               |     locked_input_vc,
                                                               |     locked_output_vc)
                                       route_compute(dst_id)   |    VA: in-VC RR in_vc_rr_[q]
                                       at each FIFO head ------+        input RR rr_[q]
                                                               |    SA: out-VC RR vc_rr_[q]
                                                               |        credit_[q][vc]
                                                               |        (seed 8, -- at
                                                               |         the SA grant)
                                                               v
                                            [output_fifo_[q], depth 8] --> link
 credit pulse to upstream of p  <--- registered 1 cycle after the
 (per VC, max 1/cycle)               stage-3 dequeue from (p, vc)
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
A internal stage of F    | S1 | S2 | S3 | S4 |    |   <- one stage per cycle
A credit_[EAST][0]       |  8 |  8 |  7 |  7 |  7 |   <- decrement at the SA grant (SPEC 7)
tx_dat_valid[EAST]       |  0 |  0 |  0 |  0 |  1 |   <- sampled by peer at N+4:
tx_dat_flit[EAST]        |  0 |  0 |  0 |  0 |  F |      zero-load head latency 4 (SPEC 4)
rx_dat_crdvalid[LOCAL][0]|  0 |  0 |  0 |  0 |  1 |   <- LOCAL dequeue at cycle 2,
                                                         pulse on wire at 2+2 = 4 (R7)
```

Annotations: reset was given once, before cycle 0 (R9). The VA grant is at cycle 1 and the SA
grant at cycle 2, the core-internal credit pulse fires at cycle 3, and the registered wire pulse
is sampled at cycle 4 (SA grant N -> core pulse N+1 -> SV wire N+2). A single-flit packet is a
head, so it pays all four stages.

Waveform 2: 3-flit DAT wormhole packet (section 2.9), link A(0,0).EAST -> B(1,0).WEST,
VC0, injected cycles 0..2. Credit counter shown as its value entering each cycle.

```
cycle (posedge idx)         |  0 |  1 |  2 |  3 |  4 |  5 |  6 |  7 |  8 |  9 | 10 |
----------------------------+----+----+----+----+----+----+----+----+----+----+----+
A rx_dat_valid[LOCAL]       |  1 |  1 |  1 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |
A rx_dat_flit[LOCAL]        | F0 | F1 | F2 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |
A wormhole_[EAST][0] lock   |  - |  L |  L |  L |  - |  - |  - |  - |  - |  - |  - |
A credit_[EAST][0]          |  8 |  8 |  8 |  7 |  6 |  5 |  5 |  5 |  5 |  6 |  7 |
A tx_dat_valid[EAST]        |  0 |  0 |  0 |  0 |  1 |  1 |  1 |  0 |  0 |  0 |  0 |
A tx_dat_flit[EAST]         |  0 |  0 |  0 |  0 | F0 | F1 | F2 |  0 |  0 |  0 |  0 |
A rx_dat_crdvalid[LOCAL][0] |  0 |  0 |  0 |  0 |  1 |  1 |  1 |  0 |  0 |  0 |  0 |
B rx_dat_crdvalid[WEST][0]  |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  1 |  1 |  1 |
B tx_dat_valid[LOCAL]       |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  1 |  1 |  1 |
B tx_dat_flit[LOCAL]        |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 | F0 | F1 | F2 |
```

Annotations: three flits, 0-gap injection (R8). Lock row `L` = (EAST, VC0) locked to
(LOCAL, VC0), set by F0's VA grant at cycle 1 and released by F2's tail grant at cycle 4;
nothing else departs on that VC in between (SPEC 10). `credit_[EAST][0]` bottoms at 5
with three flits outstanding and is back to the seed 8 after cycle 10 (conservation,
SPEC 8). A grants F0..F2 at cycles 2,3,4 and B at 6,7,8, so B's LOCAL wire carries them at
8,9,10. Head latency 8 = 2 routers x 4 cycles; F1 and F2 skip VA and follow one cycle apart.
Each credit wire pulse is exactly 1 cycle wide, one per freed slot (R5).

## 7. Appendix: Hints

Non-binding implementation notes.

Hint: evaluating the four stages in reverse order (4, 3, 2, 1) inside one clock
evaluation reproduces three required behaviors at once: a flit advances exactly one
stage per cycle, a full output FIFO that drains in stage 4 accepts a grant in the
same cycle (SPEC 14), and a VC a tail frees in SA is allocatable by VA in the same
cycle (section 2.4). Any structure with the same observable cycle behavior is
equally acceptable.

Hint: on unicast traffic no datapath logic ever needs to decode `axi_ch`, `src_id`,
`ordering_req`, `ordering_tag`, or the payload. Flit bits [11:0] and [43:25] plus the
whole payload form an opaque bundle steered by the 13 control bits
{`flit_tail` [24], `vc_id` [23:21], `fixed_vc` [20], `dst_id` [19:12]}. Collectives add
`collective_op` [35:34] and `collective_mask` [43:36], and the RSP join additionally
reads `axi_ch`, `src_id`, `ordering_tag` and the payload's `bresp` / `bid`.

Hint: the per-(port, VC) DAT credit counter needs
ceil(log2(`NOC_ROUTER_VC_DEPTH` + 1)) = 4 bits at the default depth 8 (values 0..8).
