# Design: NoC Router (ROUTER)

Block-level design spec for the per-node mesh router of the NoC C++ behavior model.
The reader implements an RTL block whose cycle behavior is checked against this model
by the existing testbench. As-built references:

| Layer | File |
|---|---|
| Router core (per network) | `src/c_model/include/router/router.hpp` |
| Per-node wrap (REQ + RSP routers, DPI I/O latch) | `src/c_model/include/wrap/router_wrap.hpp` |
| SV DPI module (top-level pin contract) | `src/sv/router_wrap.sv` |
| Unit tests | `src/c_model/tests/router/test_router.cpp` |
| Wire-level credit assertions | `sim/tb/link_perf_monitor.sv` |
| Generated fabric wiring | `sim/tools/gen_tb_top.py` |

Cycle convention used throughout: "signal value at cycle N" means the value sampled at
posedge N of `clk_i`. An output registered at posedge K is first sampled at posedge K+1.

## 2. Design Description

### 2.1 Concepts

**Flit.** The unit of transfer on every NoC link is one flit: a fixed 408-bit
(= 0x198 = 51-byte) word carrying a 56-bit header and a 352-bit payload. A link moves
at most one flit per direction per cycle.

**Packet and wormhole switching.** An AXI transaction is packetized by the NI into one
or more flits sharing the same header `dst_id`. The header bit `flit_tail` marks packet
boundaries: `flit_tail = 1'b0` on every flit except the final one, `flit_tail = 1'b1` on the final
flit. A single-flit packet has `flit_tail = 1'b1` on its only flit. The router forwards
wormhole style: it does not wait for a whole packet before forwarding, and once a
packet's head flit has been granted to an output, that output serves only that packet
until its tail (`flit_tail = 1'b1`) passes. Flits of two packets therefore never interleave
on one output.

**Virtual channels (VC).** Each input port holds `NUM_VC` independent FIFOs. A flit's
header `vc_id` selects which FIFO it lands in and which credit counter it consumes.
The physical link is one flit-wide channel per direction per network. `vc_id` travels
in the header. There are no per-VC lanes on the wire. The router never changes a
flit's VC: a flit that enters on VC 1 leaves on VC 1 and is filed under VC 1 by the
next hop. VC assignment is done once, at the NI.

**Credit-based flow control.** There is no ready signal. A sender may drive a flit on
VC v only when its credit counter for that (output, VC) is nonzero. The counter is
seeded to the receiver's per-VC input FIFO depth (`NOC_ROUTER_VC_DEPTH = 4`), decrements
by 1 when a flit is committed toward that output, and increments by 1 for each
single-cycle credit pulse the receiver returns after draining one flit from that input
FIFO. Example with depth 4: a sender can fire 4 back-to-back flits on VC 0, must then
idle at credit 0, and resumes one flit per returned pulse.

### 2.2 Flit format

> Pre-S1: this table predates the Stage 1 flit-layout change (44 b header, 48 b addr, 396 b flit) and is re-synced in campaign Stage 5.

Bit positions inside the 408-bit flit, from `specgen/generated/cpp/ni_flit_constants.h`.
Header occupies flit bits [55:0], payload occupies flit bits [407:56].

| Field | Flit bits | Width | Meaning |
|---|---|---|---|
| `axi_ch` | [2:0] | 3 | AXI channel code: 3'd0 AW, 3'd1 W, 3'd2 AR, 3'd3 B, 3'd4 R. Values 3'd5..3'd7 never occur. |
| `src_id` | [10:3] | 8 | Source node id, `{y[3:0], x[3:0]}`. |
| `dst_id` | [18:11] | 8 | Destination node id, `{y[3:0], x[3:0]}`. Read by the router for routing. |
| `vc_id` | [21:19] | 3 | Virtual channel index, `0 <= vc_id < NUM_VC`. Read by the router. |
| `flit_tail` | [22] | 1 | 1'b1 on the final flit of a packet. Read by the router. |
| `ordering_req` | [23] | 1 | NI reorder-buffer flag. Transparent to the router. |
| `ordering_tag` | [31:24] | 8 | NI reorder-buffer index. Transparent to the router. |
| `rsvd` | [55:32] | 24 | Header padding, driven 0 by the NI. Transparent to the router. |
| payload | [407:56] | 352 | AXI channel payload. Transparent to the router. |

IMPORTANT: the router reads only `dst_id`, `vc_id`, and `flit_tail`. Every other bit,
header and payload alike, passes through unmodified, byte for byte. Payload layout is
owned by the NMU/NSU specs and is out of scope here.

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

### 2.4 Pipeline: three stages, one stage per cycle

Each Router is a 3-stage pipeline. A flit advances exactly one stage per cycle.

| Stage | Storage | Action per cycle |
|---|---|---|
| 1. Input | per-port 1-deep input register, then per-(port, VC) FIFO, depth `NOC_ROUTER_VC_DEPTH` = 4 | file the registered flit into the FIFO selected by header `vc_id` |
| 2. Grant | per-output wormhole lock + RR state + credit counters | per output: pick one (input, VC) candidate, pop its FIFO front, decrement `credit_[out][vc]`, push into the output FIFO, schedule one credit pulse to the upstream of that input |
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

Zero-load latency is exactly 3 cycles per hop: a flit sampled from the input wire at
posedge N is sampled on the output wire (by the neighbor or the NI) at posedge N+3.
Verified by `ZeroLoadLatencyIsThreeTicks`.

### 2.5 Arbitration: two-level round-robin per output

When an output is not wormhole-locked, stage 2 selects a candidate with two nested
round-robin scans (`router.hpp:239-254`):

1. **Outer, VC-major**: VCs are scanned in order `vc_rr_[out], vc_rr_[out]+1, ...`
   modulo `NUM_VC`. A VC whose credit counter `credit_[out][vc]` is 0 is skipped before
   any input is looked at.
2. **Inner, input-minor**: for the chosen VC, inputs are scanned in order
   `rr, rr+1, ...` modulo 5 (per-output pointer `ws.rr`). The first input whose FIFO
   front flit routes to this output wins.

IMPORTANT (tie-break): when several (input, VC) pairs simultaneously want the same
output, the unique winner is the first match in scan order: lowest VC offset from
`vc_rr_[out]` first, then lowest input offset from `ws.rr`. Worked example, output
EAST, `NUM_VC = 2`, `vc_rr_[EAST] = 1`, `ws.rr = 3` (SOUTH), credits
`credit_[EAST][0] = 2`, `credit_[EAST][1] = 1`. Candidates: front of (WEST, VC0) and
front of (SOUTH, VC1), both routing EAST.

- VC scan starts at VC1. Credit 1 > 0, so VC1 is examined first.
- Input scan starts at SOUTH. (SOUTH, VC1) routes EAST -> **winner (SOUTH, VC1)**.
  (WEST, VC0) is never examined this cycle.
- Counter-case: with `credit_[EAST][1] = 0`, VC1 is skipped entirely. VC0 is scanned;
  (SOUTH, VC0) is empty, next input is WEST -> winner (WEST, VC0).

Both pointers advance only when a tail flit (`flit_tail = 1'b1`) is granted:
`ws.rr = winner_input + 1`, `vc_rr_[out] = winner_vc + 1` (packet-granularity
round-robin, `router.hpp:273-274`). In the example above, if the (SOUTH, VC1) flit is a
tail, the next unlocked scan starts at VC0 and input WEST. For streams of single-flit
packets this degenerates to flit-level round-robin. There is no priority or QoS input:
the flit header has no QoS field (`NOC_QOS_WIDTH = 0`).

### 2.6 Wormhole lock rules (per output, across VCs)

Each output holds one lock record `(locked_input, locked_vc)` (`router.hpp:159-163`).

1. Granting a flit with `flit_tail = 1'b0` locks the output to that (input, VC) pair.
2. While locked, only the locked (input, VC) FIFO is served at this output. If it is
   empty, or `credit_[out][locked_vc]` is 0, the output idles this cycle and keeps the
   lock. Other inputs and other VCs wait, even with credit available.
3. Granting a flit with `flit_tail = 1'b1` releases the lock and advances both RR pointers.
4. A single-flit packet (`flit_tail = 1'b1` on its head) locks and releases within the one
   grant: the output is never observed locked between cycles.

Example: a 3-flit packet (H `flit_tail=0`, B `flit_tail=0`, T `flit_tail=1`) from (LOCAL, VC0) to
EAST. Cycle k grants H and locks EAST to (LOCAL, VC0). Cycle k+1 grants B, lock held.
Cycle k+2 grants T, lock released, `ws.rr` and `vc_rr_[EAST]` advance. A competing
packet at (WEST, VC1) routing EAST waits cycles k..k+2 even though VC1 has credit.

The lock never spans different outputs: locking is a per-output property, so a packet
to EAST and a packet to NORTH from two inputs proceed in parallel.

### 2.7 Credit flow control rules

Counter granularity is per (output port, VC): `credit_[out][vc]`.

1. **Seed**: every counter starts at `NOC_ROUTER_VC_DEPTH` = 4, equal to the
   downstream input VC FIFO depth (`router.hpp:91`).
2. **Decrement**: by 1 at the grant event (stage-2 admission into the output FIFO,
   `router.hpp:262-263`), not at link traversal. With seed 4, four grants toward one
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
5. **At zero credit**: the unlocked scan skips the VC before examining inputs. A
   locked output idles and holds its lock. A zero-credit VC never stalls another VC
   at an unlocked output. Example: `NUM_VC = 2`, `credit_[EAST][0] = 0`,
   `credit_[EAST][1] = 3`, traffic waiting on both VCs -> VC1 flits keep flowing to
   EAST while VC0 waits.

The stage-3 output FIFO (depth 2) is an architectural parameter of this design
(`NOC_ROUTER_OUTPUT_FIFO_DEPTH`) but is not credit-counted and is invisible to the
neighbor. Its only flow effect is the stage-2 admission gate: no grant to an output
whose FIFO already holds `NOC_ROUTER_OUTPUT_FIFO_DEPTH` flits.

### 2.8 Two networks per node

Each mesh node instantiates one `router_wrap` containing two independent Router
instances: a REQ router (carries AW/W/AR flits, NMU -> NSU direction) and an RSP
router (carries B/R flits, NSU -> NMU direction). They share nothing: separate FIFOs,
credits, locks, and separate `link_req_*` / `link_rsp_*` pins. This REQ/RSP physical
split removes request-response protocol deadlock.

On the RSP network every packet is single-flit: the NSU emits `flit_tail = 1'b1` on every B
flit and on every R beat flit. The RSP router's wormhole lock therefore only ever
engages degenerately (lock and release within one grant, rule 2.6.4), and RSP
arbitration behaves as flit-level round-robin.

### 2.9 Worked example: 3-flit packet, 2 hops

Topology: nodes A = (0,0) and B = (1,0). The NMU at A sends one 3-flit REQ packet
(F0 `flit_tail=0`, F1 `flit_tail=0`, F2 `flit_tail=1`, all VC0, `dst_id` = 8'h01) to the NSU at B.
Flits enter A's `noc_nmu_req_i` at cycles 0, 1, 2. All credit counters start at 4.

| Cycle | Router A (x=0,y=0) | Router B (x=1,y=0) | Wires (sampled this cycle) |
|---|---|---|---|
| 0 | stage 1: F0 -> fifo[LOCAL][0] | idle | `noc_nmu_req_i.valid` = 1 (F0) |
| 1 | stage 2: grant F0 to EAST, `credit_[EAST][0]` 4->3, lock EAST to (LOCAL,0). stage 1: F1 filed | idle | F1 in |
| 2 | stage 3: F0 -> link. stage 2: grant F1 (3->2). stage 1: F2 filed | idle | F2 in |
| 3 | stage 3: F1. stage 2: grant F2 (2->1), tail -> unlock, RR advance | stage 1: F0 filed | `link_req_out_valid[EAST]` = 1 (F0). `noc_nmu_req_cred_o[0]` pulse (F0's LOCAL dequeue at cycle 1) |
| 4 | stage 3: F2 | stage 2: grant F0 to LOCAL | F1 on link. NMU credit pulse (F1) |
| 5 | idle | stage 3: F0 -> eject. stage 2: grant F1 | F2 on link. NMU credit pulse (F2) |
| 6 | `credit_[EAST][0]` 1->2 (B's pulse for F0) | stage 3: F1. stage 2: grant F2 | B's `noc_nsu_req_o.valid` = 1 (F0). B's `link_req_in_credit[WEST][0]` pulse reaches A |
| 7 | 2->3 | stage 3: F2 | F1 to NSU. Credit pulse (F1) |
| 8 | 3->4 (fully replenished) | idle | F2 to NSU. Credit pulse (F2) |

Head latency: injected cycle 0, at the destination NI cycle 6 = 2 hops x 3 cycles.
Tail: cycle 2 -> cycle 8. A's `credit_[EAST][0]` bottoms at 1 (three flits in flight)
and returns to 4 by cycle 8.

## 3. Inputs and Outputs

### 3.1 Parameters

`router_wrap` SV parameters (`src/sv/router_wrap.sv:39-46`):

> Pre-S3a: the single `FLIT_WIDTH` row below predates the Stage 3a three-network split. As-built, `router_wrap` takes three per-network flit-width parameters instead (`REQ_FLIT_WIDTH` = 137, `RSP_FLIT_WIDTH` = 127, `DAT_FLIT_WIDTH` = 629); `NOC_FLIT_WIDTH_DFLT` no longer exists. Re-synced in campaign Stage 5.

| Parameter | Default | Legal range | Meaning |
|---|---|---|---|
| `NUM_VC` | `ni_params_pkg::NOC_DAT_NUM_VC_DFLT` = 1 | 1..8 (= 2^VC_ID_WIDTH) | VCs per link, per network. Topology YAML overrides per run. `initial`-block `$fatal` at time 0 if `$bits(noc_types_pkg::noc_credit_t) != NUM_VC`. |
| `FLIT_WIDTH` | `NOC_FLIT_WIDTH_DFLT` = 408 | fixed 408 in this design | flit bus width, bits |
| `LINK_PORTS` | 5 | fixed 5 | port array size = {LOCAL, NORTH, EAST, SOUTH, WEST} |

Router model configuration, fixed at `cmodel_router_create` time:

| Parameter | Default | Legal range | Meaning |
|---|---|---|---|
| `NOC_ROUTER_VC_DEPTH` | 4 | 1..16 | input VC FIFO depth and the upstream credit seed |
| `NOC_ROUTER_OUTPUT_FIFO_DEPTH` | 2 | 1..16 | stage-3 output FIFO depth, not credit-counted |
| `mesh_x_dim`, `mesh_y_dim` | 4, 4 | 2..16 each | mesh dimensions. Minimum 2 per dimension: a mesh communicating through NI + router needs at least 2x2; 1x1 and 1xN meshes are illegal. |
| `x_coord`, `y_coord` | per node | `x < mesh_x_dim`, `y < mesh_y_dim` | this node's coordinate |

### 3.2 Port index encoding

All `[LINK_PORTS]` arrays are indexed by direction:

| Index | Direction | LINK-face use |
|---|---|---|
| 0 | LOCAL | unused on the LINK face (NI traffic uses the `noc_nmu_*` / `noc_nsu_*` pins). Slot 0 of every LINK array is tied 0 / ignored. |
| 1 | NORTH (+y) | link to node (x, y+1) |
| 2 | EAST (+x) | link to node (x+1, y) |
| 3 | SOUTH | link to node (x, y-1) |
| 4 | WEST | link to node (x-1, y) |

Boundary directions (no neighbor) are left unwired by the generated fabric: inputs tied
to 0, outputs must stay 0 (SPEC 17).

### 3.3 Signal tables

> Pre-S3a: the single `noc_chan_t` struct below predates the Stage 3a three-network split. As-built, `router_wrap` ports are per-network scalars (`tx_req_*`/`rx_req_*`, `tx_rsp_*`/`rx_rsp_*` ready/valid; `tx_dat_*`/`rx_dat_*` credit) with per-network flit widths (REQ 137 b, RSP 127 b, DAT 629 b), not one `noc_chan_t` struct. Re-synced in campaign Stage 5.

> REQ/RSP `ready` is advisory, not a same-cycle accept: the sender grants against a `ready` sampled ~2 registrations earlier, and the receiver pushes unconditionally on `valid`, so a real transfer is `valid` alone.

Struct types: `ni_signals_pkg::noc_chan_t` = `{valid (1 bit), flit[407:0]}`, 409 bits.
`noc_types_pkg::noc_credit_t` = `{credit[NUM_VC-1:0]}`, one bit per VC.

Inputs:

| Signal | Bit width | Definition |
|---|---|---|
| `clk_i` | 1 | Clock. All sequential behavior on the posedge. |
| `rst_ni` | 1 | Synchronous active-low reset. Given only once, at the beginning of simulation (rule R9). |
| `ctx_i` | 64 | Model handle returned by `cmodel_router_create`. Constant after reset. From tb_top. |
| `noc_nmu_req_i` | 409 | REQ flit injection from this node's NMU. `.valid` high for 1 cycle per flit. `.flit` valid only when `.valid` is high, all zeros otherwise. |
| `noc_nmu_rsp_cred_i` | NUM_VC | Per-VC credit pulse from the NMU: bit v = the NMU consumed one ejected RSP flit on VC v. Increments the RSP router's LOCAL-output credit. |
| `noc_nsu_req_cred_i` | NUM_VC | Per-VC credit pulse from the NSU: bit v = the NSU consumed one ejected REQ flit on VC v. Increments the REQ router's LOCAL-output credit. |
| `noc_nsu_rsp_i` | 409 | RSP flit injection from this node's NSU. Same valid/flit rules as `noc_nmu_req_i`. |
| `link_req_in_valid` | 5 | Bit p: the neighbor at direction p drives one REQ flit this cycle. Bit 0 (LOCAL) always 0. |
| `link_req_in_flit` | 408 x 5 (unpacked `[LINK_PORTS]`) | REQ flit from the neighbor at direction p. Valid only when `link_req_in_valid[p]` is high, all zeros otherwise. |
| `link_req_out_credit` | NUM_VC x 5 (unpacked) | Per-VC credit pulse from the neighbor at direction p, for a REQ flit this node previously sent out of its p output. Increments `credit_[p][vc]` of the REQ router. |
| `link_rsp_in_valid` | 5 | RSP mirror of `link_req_in_valid`. |
| `link_rsp_in_flit` | 408 x 5 | RSP mirror of `link_req_in_flit`. |
| `link_rsp_out_credit` | NUM_VC x 5 | RSP mirror of `link_req_out_credit`. |

Outputs (all registered, reset to 0):

| Signal | Bit width | Definition |
|---|---|---|
| `noc_nmu_req_cred_o` | NUM_VC | Per-VC credit pulse to the NMU: bit v = the REQ router drained one flit from its LOCAL input FIFO, VC v. The NMU may inject one more flit on VC v. |
| `noc_nmu_rsp_o` | 409 | RSP flit ejected toward the NMU. `.valid` high 1 cycle per flit, `.flit` all zeros when `.valid` is low. |
| `noc_nsu_req_o` | 409 | REQ flit ejected toward the NSU. Same rules. |
| `noc_nsu_rsp_cred_o` | NUM_VC | Per-VC credit pulse to the NSU: RSP router drained one flit from its LOCAL input FIFO. |
| `link_req_out_valid` | 5 | Bit p: one REQ flit driven toward the neighbor at direction p this cycle. Bit 0 always 0. Boundary bits always 0. |
| `link_req_out_flit` | 408 x 5 | REQ flit toward the neighbor at direction p. All zeros when `link_req_out_valid[p]` is low. |
| `link_req_in_credit` | NUM_VC x 5 | Per-VC credit pulse to the neighbor at direction p: this node drained one flit from its p-direction REQ input FIFO, VC v. |
| `link_rsp_out_valid` | 5 | RSP mirror. |
| `link_rsp_out_flit` | 408 x 5 | RSP mirror. |
| `link_rsp_in_credit` | NUM_VC x 5 | RSP mirror. |

NI-edge credit crossing (who returns which credit): the NI that consumes an ejected
flit returns the eject credit, and the router returns the injection credit.

| Flow | Flit pin | Credit pin (opposite direction) |
|---|---|---|
| NMU injects REQ | `noc_nmu_req_i` | `noc_nmu_req_cred_o` (router -> NMU) |
| Router ejects REQ to NSU | `noc_nsu_req_o` | `noc_nsu_req_cred_i` (NSU -> router) |
| NSU injects RSP | `noc_nsu_rsp_i` | `noc_nsu_rsp_cred_o` (router -> NSU) |
| Router ejects RSP to NMU | `noc_nmu_rsp_o` | `noc_nmu_rsp_cred_i` (NMU -> router) |

Fabric wiring between nodes pairs opposite ports: node i's `link_*_in_valid/flit[NORTH]`
comes from its north peer's `link_*_out_valid/flit[SOUTH]`, and node i's
`link_*_out_credit[NORTH]` comes from that peer's `link_*_in_credit[SOUTH]`
(`gen_tb_top.py:330-352`).

### 3.4 DPI function table

The SV module drives the model with three calls per posedge, in this order
(`router_wrap.sv:160-267`). One `cmodel_router_tick` = one modeled clock cycle for
both the REQ and RSP routers.

| Function | When | Semantics |
|---|---|---|
| `cmodel_router_create(name, x_coord, y_coord, mesh_x_dim, mesh_y_dim, num_vc)` | once, from the tb_top `initial` block, after `rst_ni` deassertion | constructs both routers. Construction is reset: all FIFOs empty, all credits at seed. Returns the 64-bit `ctx` handle. |
| `cmodel_router_set_inputs(ctx, ...)` | posedge, step 1 | samples the current SV wire values (the previous cycle's registered outputs of the peers) into the model input latch |
| `cmodel_router_tick(ctx)` | posedge, step 2 | advances both routers exactly one cycle |
| `cmodel_router_get_outputs(ctx, ...)` | posedge, step 3 | reads the model output latch. The SV module registers these values nonblocking, so they appear on the output pins one cycle later. |

LINK-face marshalling is port-major: flit = 13 x 32-bit words per port, credit = one
`[NUM_VC-1:0]` word per port, valid = one bit per port in a packed vector.

### 3.5 Protocol rules

R1 (input rhythm). At most one flit per network per input face per cycle: one on
`noc_nmu_req_i`, one on `noc_nsu_rsp_i`, and one per direction on
`link_req_in_*` / `link_rsp_in_*`. Back-to-back flits on consecutive cycles are legal
without limit while credit lasts. Flits of one packet need not be contiguous: gaps of
any length may separate them (the wormhole lock holds across gaps, rule 2.6.2).

R2 (idle bus state). When a `valid` bit is low, the corresponding 408-bit flit bus
carries all zeros. This holds for the module's own outputs (registered zeros) and for
its inputs (each input wire is a peer's registered output or a fabric tie-off).
Credit vectors carry 0 in every non-pulsing bit position.

R3 (sampling edge). All inputs are sampled at the posedge of `clk_i`. All outputs are
registered and change only at the posedge. The verification environment (co-sim
scoreboard, `link_perf_monitor` assertions, boundary `$fatal` checks) samples at the
posedge.

R4 (valid behavior). Each `valid` bit is high for exactly 1 cycle per flit. There is
no ready signal and no retraction: a driven flit is committed. A sender may assert
valid on VC v toward a port only while its credit counter for that (port, VC) is
nonzero.

R5 (credit pulse shape). Every credit signal bit is a single-cycle pulse. At most one
pulse per (port, VC) per cycle. Each pulse means exactly one freed buffer slot.
Example: two same-cycle stage-2 dequeues from (WEST, VC0) (section 2.4) produce pulses
on `link_req_in_credit[WEST][0]` in two consecutive cycles, never a 2-cycle-wide level
or a double-count.

R6 (credit seed). After reset, the sender-side counter for every (port, VC) equals
`NOC_ROUTER_VC_DEPTH` = 4. The `link_perf_monitor` on every directed edge seeds its
mirror counter with the same value (`BUFFER_DEPTH = ROUTER_VC_DEPTH`,
`gen_tb_top.py:387`).

R7 (credit-return latency). A flit granted (stage-2 dequeue) in cycle N produces its
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
this latency is exactly 3 cycles, every hop, both networks.

R11 (output uniqueness). At most one flit per output port per network per cycle: each
`link_*_out_valid` bit and each NI-face `.valid` covers exactly one 408-bit flit bus.

R12 (VC transparency on the wire). The `vc_id` field of an output flit equals the
`vc_id` it arrived with. Credit pulses for that flit, on both sides of the hop, occur
on that same VC index.

### 3.6 Input guarantees

The environment (NMU, NSU, neighbor routers, generated fabric) guarantees the
following. The implementer does not handle these cases. The model enforces each with
an abort or assertion at the cited line, so any violation is an environment bug, not a
router obligation.

| # | Guarantee | Model enforcement |
|---|---|---|
| G1 | Never two flits on one input port of one network in one cycle | abort, `router.hpp:194-197` |
| G2 | Every valid flit has `vc_id < NUM_VC` | abort, `router.hpp:182-185`; SVA `link_perf_monitor.sv:69-72` |
| G3 | Every valid flit has `dst_id` inside the mesh (`dst_x < mesh_x_dim`, `dst_y < mesh_y_dim`). The NMU SAM lookup validates destinations at packetize time, so an out-of-mesh `dst_id` cannot happen | abort, `router.hpp:65-68` |
| G4 | No sender drives a flit on VC v while its credit for that (port, VC) is 0 | input FIFO overflow assert, `router.hpp:284-286`; SVA `link_perf_monitor.sv:61-64` |
| G5 | Packets are well-formed per (input, VC): after a head (`flit_tail=0`), every following flit on that (input, VC) routes to the same output until a tail (`flit_tail=1`) closes the packet. Guaranteed because all flits of a packet share `dst_id` | abort, `router.hpp:228-234` |
| G6 | No credit pulse arrives beyond the outstanding flit count (counter never exceeds the seed of 4) | abort, `router.hpp:111-114` |
| G7 | On the RSP network every flit has `flit_tail = 1'b1` (the NSU emits each B and each R beat as a single-flit packet) | consequence: RSP wormhole lock only engages degenerately |
| G8 | `rst_ni` is given once at simulation start; the handle from `cmodel_router_create` is valid and constant | tb_top sequencing, `gen_tb_top.py:585-587` |
| G9 | Boundary-direction inputs are tied to 0 and never pulse | generated tie-off, `gen_tb_top.py:330-336` |

## 4. Specifications

Each item names where it is verified and what constitutes failure. ctest names refer
to `src/c_model/tests/router/test_router.cpp`. "Co-sim scoreboard" is the per-transaction
write -> readback compare of the co-simulation testbench (`make sim TB=<topology>`),
which fails on any data or ordering divergence from this model.

SPEC 1 (interface). The top module is `router_wrap` with exactly the ports and
parameters of sections 3.1-3.3. Verified at build and at the start of the generated
testbench. Failure: build/port-binding error, or the `initial`-block `$fatal` guard
that fires at time 0 when `$bits(noc_credit_t) != NUM_VC` (`router_wrap.sv:81-86`).

SPEC 2 (reset). While `rst_ni` is 0, every output signal is 0. After the single
reset, the block starts with empty FIFOs, no locks, and all credit counters at 4.
Verified by the tb_top reset window preceding all traffic. Failure: any nonzero
output during reset trips the boundary checks or the co-sim scoreboard.

SPEC 3 (routing). Every flit leaves on the port given by XY dimension-order routing
of its `dst_id` (section 2.3), recomputed at every hop. Verified by ctest
`RouterRouteCompute.XyDimensionOrder` and the co-sim scoreboard (a misroute delivers
data to the wrong NSU). Failure: wrong output port on any flit.

SPEC 4 (zero-load latency). Input-pin sample edge to output-pin sample edge is
exactly 3 cycles when the granted output is uncontended, has credit, and its output
FIFO is below depth. Verified by ctest `RouterDatapath.ZeroLoadLatencyIsThreeTicks`.
Failure: the flit appears on the output wire earlier or later than cycle N+3.

SPEC 5 (bit transparency). Every flit leaves bit-identical to how it entered: all
408 bits, header and payload. The router writes nothing. Verified by ctest
`RouterDatapath.HeaderTransparency` (byte-for-byte compare of the whole flit) and by
the co-sim scoreboard readback. Failure: any flipped bit.

SPEC 6 (VC preservation). The router never reassigns a flit's VC. The flit is filed
under its header `vc_id` at stage 1, consumes credit on that VC, and departs with the
same `vc_id`. Verified by ctest `RouterGrid.EndToEndTrafficAcrossParameterSpace`
(ingress `vc_id` preserved to egress across the parameter space),
`RouterWormhole.PacketsOnDifferentVcsDoNotInterleavePerOutput` (cross-VC
non-interleave per output), and by the per-VC credit accounting of
`link_perf_monitor.sv` (a switched VC underflows the true VC's mirror counter).
Failure: `vc_id` differs between ingress and egress, or credit activity on a VC the
flit did not use.

SPEC 7 (credit decrement point). The per-(output, VC) counter is seeded to 4 and
decremented exactly at the grant event (admission into the output FIFO), not at link
traversal. Verified by ctest `RouterDatapath.CreditDecrementAtGrantAndPulseAfterDequeue`
(counter reads 4 after stage 1, 3 after the stage-2 grant). Failure: counter value
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

SPEC 18 (network independence). The REQ and RSP routers share no state: traffic,
stalls, or credit exhaustion on one network never affects the other. Verified by
structure (two separate model instances) and by the co-sim scoreboard under
bidirectional regression traffic. Failure: cross-network coupling observable as a
scoreboard divergence.

SPEC 19 (parameter legality). Construction rejects (assert then abort) exactly three
conditions: `NUM_VC` outside 1..8 (= 2^VC_ID_WIDTH), a zero VC depth or zero output
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
Failure: construction succeeds on `num_vc` outside 1..8, a zero depth, or an
out-of-mesh coordinate.

## 5. Block Diagram

Fabric context (generated per topology YAML, one `router_wrap` per node):

```
                          node (x, y+1)
                     link[SOUTH] out ^ | in
        flit + valid down, credit up | v  (opposite-port pairing)
   +---------------------------------------------------------+
   |  node (x, y)               router_wrap                   |
   |                                                          |
   |   NMU --noc_nmu_req_i-->  +-----------+ --link_req_out--> EAST peer
   |   NMU <-noc_nmu_req_cred- | REQ Router| <-link_req_in---  (per dir
   |   NSU <--noc_nsu_req_o--  |           | <-link_req_out_credit  N/E/S/W,
   |   NSU --noc_nsu_req_cred> +-----------+ --link_req_in_credit-> LOCAL slot
   |                                                          |     unused)
   |   NSU --noc_nsu_rsp_i-->  +-----------+ --link_rsp_out-->
   |   NSU <-noc_nsu_rsp_cred- | RSP Router| <-link_rsp_in---
   |   NMU <--noc_nmu_rsp_o--  |           |   ...
   |   NMU --noc_nmu_rsp_cred> +-----------+                  |
   +---------------------------------------------------------+
                     link[NORTH] of node (x, y-1)

   one passive link_perf_monitor per live directed edge (req + rsp),
   BUFFER_DEPTH = ROUTER_VC_DEPTH, asserts SPEC 9 / G2 on the wire
```

One Router (per network), 3-stage pipeline, 5 in / 5 out ports:

```
 input port p (x5: LOCAL,N,E,S,W)                      output port q (x5)
 --------------------------------                      -----------------
                    stage 1              stage 2                stage 3
 flit ---> [input_reg_ 1-deep] --vc_id--> [FIFO vc0, depth 4] \
                                          [FIFO vc1, depth 4] -+--> per-output q:
                                              ...              |    wormhole lock
                                          [FIFO vcN-1]        -+    (locked_input,
                                                               |     locked_vc)
                                       route_compute(dst_id)   |    VC RR vc_rr_[q]
                                       at each FIFO head ------+    input RR ws.rr
                                                               |    credit_[q][vc]
                                                               |    (seed 4, -- at
                                                               |     grant)
                                                               v
                                            [output_fifo_[q], depth 2] --> link
 credit pulse to upstream of p  <--- registered 1 cycle after the
 (per VC, max 1/cycle)               stage-2 dequeue from (p, vc)
```

## 6. Sample Waveform

Both waveforms use the sampling convention of section 1: the value shown in column N
is the value sampled at posedge N.

Waveform 1: zero-load single-flit hop plus credit return. Node A (0,0), single-flit
packet F (`flit_tail=1`, VC0, `dst_id`=8'h01) injected by the NMU, routed EAST.

```
cycle (posedge idx)      |  0 |  1 |  2 |  3 |  4 |
-------------------------+----+----+----+----+----+
noc_nmu_req_i.valid      |  1 |  0 |  0 |  0 |  0 |   <- valid exactly 1 cycle (R4)
noc_nmu_req_i.flit       |  F |  0 |  0 |  0 |  0 |   <- all-zero when valid low (R2)
A internal stage of F    | S1 | S2 | S3 |    |    |   <- one stage per cycle
A credit_[EAST][0]       |  4 |  3 |  3 |  3 |  3 |   <- decrement at grant (SPEC 7)
link_req_out_valid[EAST] |  0 |  0 |  0 |  1 |  0 |   <- sampled by peer at N+3:
link_req_out_flit[EAST]  |  0 |  0 |  0 |  F |  0 |      zero-load latency 3 (SPEC 4)
noc_nmu_req_cred_o[0]    |  0 |  0 |  0 |  1 |  0 |   <- LOCAL dequeue at cycle 1,
                                                         pulse on wire at 1+2 = 3 (R7)
```

Annotations: reset was given once, before cycle 0 (R9). The grant is at cycle 1, the
core-internal credit pulse fires at cycle 2, the registered wire pulse is sampled at
cycle 3 (grant N -> core pulse N+1 -> SV wire N+2).

Waveform 2: 3-flit wormhole packet (section 2.9), link A(0,0).EAST -> B(1,0).WEST,
VC0, injected cycles 0..2. Credit counter shown as its value entering each cycle.

```
cycle (posedge idx)         |  0 |  1 |  2 |  3 |  4 |  5 |  6 |  7 |  8 |
----------------------------+----+----+----+----+----+----+----+----+----+
A noc_nmu_req_i.valid       |  1 |  1 |  1 |  0 |  0 |  0 |  0 |  0 |  0 |  3 flits,
A noc_nmu_req_i.flit        | F0 | F1 | F2 |  0 |  0 |  0 |  0 |  0 |  0 |  0-gap (R8)
A wormhole_[EAST] lock      |  - |  L |  L |  - |  - |  - |  - |  - |  - |  head locks,
A credit_[EAST][0]          |  4 |  4 |  3 |  2 |  1 |  1 |  1 |  2 |  3 |  tail frees
A link_req_out_valid[EAST]  |  0 |  0 |  0 |  1 |  1 |  1 |  0 |  0 |  0 |  no
A link_req_out_flit[EAST]   |  0 |  0 |  0 | F0 | F1 | F2 |  0 |  0 |  0 |  interleave
A noc_nmu_req_cred_o[0]     |  0 |  0 |  0 |  1 |  1 |  1 |  0 |  0 |  0 |  (SPEC 10)
B link_req_in_credit[WEST][0]| 0 |  0 |  0 |  0 |  0 |  0 |  1 |  1 |  1 |  B grants at
B noc_nsu_req_o.valid       |  0 |  0 |  0 |  0 |  0 |  0 |  1 |  1 |  1 |  4,5,6 ->
B noc_nsu_req_o.flit        |  0 |  0 |  0 |  0 |  0 |  0 | F0 | F1 | F2 |  wire 6,7,8
```

Annotations: lock row `L` = EAST locked to (LOCAL, VC0), set by the F0 grant at
cycle 1, released by the F2 tail grant at cycle 3. `credit_[EAST][0]` bottoms at 1
with three flits outstanding and is back to the seed 4 after cycle 8 (conservation,
SPEC 8). Head latency 6 = 2 hops x 3 cycles. Each credit wire pulse is exactly 1 cycle
wide, one per freed slot (R5).

## 7. Appendix: Hints

Non-binding implementation notes.

Hint: evaluating the three stages in reverse order (3, 2, 1) inside one clock
evaluation reproduces two required behaviors at once: a flit advances exactly one
stage per cycle, and a full output FIFO that drains in stage 3 accepts a grant in the
same cycle (SPEC 14). Any structure with the same observable cycle behavior is
equally acceptable.

Hint: no datapath logic ever needs to decode `axi_ch`, `src_id`, `ordering_req`, `ordering_tag`,
or the payload. Flit bits [10:0] and [407:23] form an opaque bundle steered by the
12 control bits [22:11] = {`flit_tail` [22], `vc_id` [21:19], `dst_id` [18:11]}.

Hint: the per-(port, VC) credit counter needs ceil(log2(`NOC_ROUTER_VC_DEPTH`+1)) = 3
bits at the default depth 4 (values 0..4).
