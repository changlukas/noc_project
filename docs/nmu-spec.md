# Design: Network Master Unit (NMU)

The NMU is the master-side network interface of the NoC. It sits between exactly one external 512-bit AXI4 master interface and the router mesh: it converts AXI4 request beats (AW / W / AR) into NoC flits, and converts response flits (B / R) back into AXI4 response beats. Narrow and Data are internal NoC traffic classes selected by the SAM, not separate AXI interfaces. This document specifies the as-built C++ behavior model (`ref_model/c_model/include/nmu/`), its wrap (`ref_model/c_model/include/wrap/nmu_wrap.hpp`), and target RTL overlays. Existing co-sim checks the as-built model; target CDC, Router-only VC ownership and asymmetric LOCAL DAT flow control require model alignment before cycle-exact RTL comparison.

The production top is `nmu`. Its wrapper-facing ports, clock/reset ownership, and reviewed child
boundaries are frozen in `rtl/README.md`; this document remains authoritative for behavior.

## 2. Design Description

### 2.1 Packetization

The NoC fabric moves fixed-size flits, not AXI beats. Packetization is a 1-to-1 mapping: every accepted AXI request beat becomes exactly one flit, and every response flit becomes exactly one AXI response beat. The NI remaps the external `AXI_ID_WIDTH` (1..8) to the fixed 3-bit `NOC_ID_WIDTH` before packetization. REQ, RSP and DAT are therefore fixed at 136, 126 and 633 bits. The header carries routing and ordering metadata (source node, destination node, virtual channel, wormhole packet boundary, reorder-buffer tag, collective op and mask). The payload carries the AXI channel fields and mapped NoC ID.

A write transaction of AWLEN+1 beats therefore becomes 1 AW flit followed by AWLEN+1 W flits. A read request becomes 1 AR flit. The write's flits form one wormhole packet (the AW flit opens it, the W flit with `wlast=1` closes it) so no other request flit from this NMU can interleave between an AW and its W beats on the link. AW+W is the only multi-flit packet the fabric builds, and AXI4 IHI 0022 A5.3.3 is why: W beats of different transactions may not interleave, so the AW and its burst have to travel as one indivisible unit. A read request and every response are single-flit packets, R beats included.

The current C++ NMU has three flit faces, one per physical network (Section 3.1):

- REQ egress (NMU produces): `NarrowAw` / `NarrowW` / `NarrowAr` plus `DataAr` — no AR rides DAT. Ready/valid.
- RSP ingress (NMU consumes): `NarrowB` / `DataB` plus `NarrowR`. Ready/valid.
- DAT egress + ingress: `DataAw` / `DataW` out, `DataR` in. The model uses credit flow control both directions.

The target RTL keeps credit only on DAT egress into the Router LOCAL input VC FIFOs. DAT ingress
from the Router uses ready/valid, with ready derived from the NMU DAT Read class FIFO. The NMU has
no receive VC FIFO.

### 2.2 Flit format

Source of truth: `specgen/generated/cpp/ni_flit_constants.h` (generated from `specgen/generated/json/ni_packet.json`, drift-gated at build). One 48-bit header layout, three flit widths, one per network:

| Network | `FLIT_WIDTH` | Payload region | Channels carried |
|---|---|---|---|
| REQ | 136 | `flit[135:48]`, 88 b | `NarrowAw`, `NarrowW`, `NarrowAr`, `DataAr` |
| RSP | 126 | `flit[125:48]`, 78 b | `NarrowB`, `DataB`, `NarrowR` |
| DAT | 633 | `flit[632:48]`, 585 b | `DataAw`, `DataW`, `DataR` |

Each network's width is the widest payload it carries plus the header. Payload field positions below are relative to the payload region: payload bit p is flit bit p+48. A payload shorter than its network's region leaves the upper bits 0.

Header (`flit[47:0]`), identical on all three networks:

| Field | Bits | Width | Definition |
|---|---|---|---|
| axi_ch | [3:0] | 4 | AXI channel of the payload. 4'd0 NarrowAw, 4'd1 NarrowW, 4'd2 NarrowAr, 4'd3 NarrowB, 4'd4 NarrowR, 4'd5 DataAw, 4'd6 DataW, 4'd7 DataAr, 4'd8 DataB, 4'd9 DataR. Values 4'd10 to 4'd15 never occur. |
| src_id | [11:4] | 8 | Source node id, {y[3:0], x[3:0]}. Constructor argument of the NMU instance. |
| dst_id | [19:12] | 8 | Destination node id, {y[3:0], x[3:0]}. From SAM lookup of the beat address (request path). |
| fixed_vc | [20] | 1 | 1: downstream routers keep `vc_id` unchanged instead of restamping it at VA (Section 2.4). |
| vc_id | [23:21] | 3 | Virtual channel, 0 <= vc_id < NUM_VC <= 8. Stamped by the `VcAllocator` on request flits. |
| flit_tail | [24] | 1 | Wormhole packet boundary. AW: 0 (opens the write packet). W: wlast (closes it). AR / B / R: 1 (single-flit packet). |
| ordering_req | [25] | 1 | 1: the response to this request owns reserved reorder-buffer slots. 0: bypassed, no slot. |
| ordering_tag | [33:26] | 8 | RoB slot tag. AW: the single slot. AR: base of the len+1 consecutive slots. 0 when ordering_req=0. |
| collective_op | [35:34] | 2 | 2'd0 UNICAST, 2'd1 MULTICAST, 2'd2-3 reserved (Section 2.8). |
| collective_mask | [43:36] | 8 | Node-id wildcard mask of a multicast. 0 on every unicast flit. |
| dst_port_id | [45:44] | 2 | Which endpoint at `dst_id` receives. 0 is the tile on the router's LOCAL port. |
| src_port_id | [47:46] | 2 | Which endpoint at `src_id` issued. The response is addressed back to it. |

There is no `rsvd` field: `PADDING_FIELDS_COUNT` = 0, the header is fully assigned.

AW payload (88 bits). AR payload is field-for-field identical with `ar*` names:

| Field | Payload bits | Width | Definition |
|---|---|---|---|
| awid | [2:0] | 3 | AXI ID. |
| awaddr | [50:3] | 48 | The global request address, emitted unchanged by the NMU (Sections 2.3 and 2.8). |
| awlen | [58:51] | 8 | Burst length minus 1. |
| awsize | [61:59] | 3 | Bytes per beat = 2^awsize, awsize <= 3'h6 (64 B, the 512-bit data bus). |
| awburst | [63:62] | 2 | 2'b00: FIXED. 2'b01: INCR. 2'b10: WRAP. 2'b11 never occurs. |
| awcache | [67:64] | 4 | Pass-through. |
| awlock | [68] | 1 | Pass-through. |
| awprot | [71:69] | 3 | Pass-through. |
| awregion | [75:72] | 4 | Carried in the flit, tied to 0 at the co-sim boundary (not in the DPI signature). |
| awqos | [79:76] | 4 | Pass-through. |
| awuser | [87:80] | 8 | Carried in the flit, tied to 0 at the co-sim boundary. The 58-bit AWUSER of Section 2.8 is consumed at the slave port, not carried here. |

W payload, `NarrowW` 81 b / `DataW` 585 b. The two differ only in the data and strobe widths:

| Field | NarrowW bits | DataW bits | Definition |
|---|---|---|---|
| wlast | [0] | [0] | Last beat of the burst. Mirrors header.flit_tail on W flits. |
| wuser | [8:1] | [8:1] | Carried, tied to 0 at the co-sim boundary. |
| wstrb | [16:9], 8 b | [72:9], 64 b | Byte strobes, one bit per data lane. |
| wdata | [80:17], 64 b | [584:73], 512 b | Write data. |

B payload (13 bits), consumed by the response path:

| Field | Payload bits | Width | Definition |
|---|---|---|---|
| bid | [2:0] | 3 | AXI ID of the completed write. |
| bresp | [4:3] | 2 | 2'b00 OKAY, 2'b01 EXOKAY, 2'b10 SLVERR, 2'b11 DECERR. |
| buser | [12:5] | 8 | Decoded from the flit, not driven onto the co-sim AXI face. |

R payload, `NarrowR` 78 b / `DataR` 526 b, consumed by the response path:

| Field | NarrowR bits | DataR bits | Definition |
|---|---|---|---|
| rlast | [0] | [0] | Last beat of the read burst. |
| rid | [3:1] | [3:1] | AXI ID. |
| rresp | [5:4] | [5:4] | Same encoding as bresp. |
| ruser | [13:6] | [13:6] | Decoded from the flit, not driven onto the co-sim AXI face. |
| rdata | [77:14], 64 b | [525:14], 512 b | Read data. |

### 2.3 SAM address translation

Destination derivation is a System Address Map (SAM) range lookup, not a bit-slice decode. The
selected `sim/configs/<CONFIG>.yml` `endpoints:` block is the sole source of entries. Each entry
contains its exclusive address interval and the generated `sam_idx_t`: `dst_id`, `dst_port_id`,
`is_data`, `collective_en`, and X/Y `{offset,len}` selectors. There is no `base_id` field.

**INPUT** wire address A. **COMPUTE** scan entries in YAML-authored order, first entry with base <=
A < base+size wins. Overlap is legal and deterministic. **OUTPUT** {dst_id, dst_port_id, AXI
class, collective enable, X/Y coordinate selectors, A}; a miss reports invalid/error and has no
default mapping.

The NMU forwards the global address unchanged and never subtracts the matched SAM region base. For unicast, the destination NSU presents that same address to the tile. For multicast, every branch carries the original AW address and each destination NSU overwrites only its node-coordinate field before presenting it to the tile; all non-coordinate bits remain unchanged. The destination's two spaces remain distinct through the global bases assigned by the map.

The C++ reference model loads the authored SAM at simulation startup through `nmu/sam_yaml.hpp`;
there is no built-in model default, and `NmuWrap::init` rejects a null or empty `config_path` with
`std::invalid_argument`. Synthesizable RTL does not parse YAML: the generator emits
`SAM_NUM_RULES`, `SAM_MASK_SEL_WIDTH`, `sam_mask_sel_t`, `sam_idx_t`, `sam_rule_t`, and constant
`SAM` into build-only `$(BUILD_ROOT)/generated/<CONFIG>/topology_pkg.sv`. The NMU passes the rule
count, SAM-related parameter types, and constant array down as elaboration-time parameters. There
is no runtime SAM programming interface.

The shared pure-combinational `ni_sam` wrapper instantiates the pinned `common_cells`
`cc_addr_decode`; it is not a second decoder. Because that primitive grants the highest matching
array index, the generator stores authored rule `i` at `SAM[SAM_NUM_RULES-1-i]` to preserve
authored first-match behavior. Independent AW and AR instances feed `nmu_packetize` destination,
port, and class results. X/Y selectors are nonzero only for a range that explicitly has
`en_collective: true`; `false` or absent remains unicast-only with zero-length selectors. The
matched rule selects those ranges for AWUSER mask translation, so Config and Memory may place
their coordinate fields at different address bits.

Example (`mesh_4x4`): A = 0x6_0000_0080. Matching entry: authored memory rule 6,
base = 6 * 0x1_0000_0000 = 0x6_0000_0000, size 0x0200_0000, generated array index 25,
dst_id = 8'h12 (x=2, y=1). Result: dst_id = 8'h12, local_addr = 0x6_0000_0080
(forwarded unchanged). Address 0x6_0200_0000 is outside this half-open region and misses unless a
separate authored rule covers it.

Both windows of one node, on `mesh_2x2`: A = 0x1000 hits node (0,0)'s memory entry
`[0x0, 0x0200_0000)` and translates to local_addr = 0x1000 (forwarded unchanged).
A = 0x0200_0010 hits the same node's config entry at base 0x0200_0000 and translates to
local_addr = 0x0200_0010 (forwarded unchanged). The two windows sit at distinct addresses because
the map put them apart, not because either address was rebased.

A SAM miss (address covered by no entry) cannot happen (Section 3.5, guarantee G1). The model aborts if violated. There is no DECERR generation in this block.

The RTL evaluates every SAM entry in parallel for AW and AR. After each decoder, an independent
elaboration-time register slice may be selected with `AW_SAM_REG_TYPE` or `AR_SAM_REG_TYPE`:
0 bypasses the slice, 1 uses one output register and may insert one bubble after backpressure, and
2 uses a full skid buffer with registered backpressure and one-request-per-cycle throughput. Modes
1 and 2 add one cycle in the zero-stall path. The slice stores the complete AXI beat together with
`dst_id`, `dst_port_id`, AXI class and collective metadata. W bypasses SAM decode and these slices.
The C++ reference model currently represents mode 0 only.

### 2.4 Request pipeline

In the target RTL, AW/W/AR first cross from `ACLK` into `noc_clk` through their existing channel
FIFOs. Every stage below then runs in `noc_clk`; no complete-flit CDC FIFO follows packetization.
The current C++ model is single-clock and uses the same logical five-channel queue boundary.

```
AXI master -> AxiSlavePort -> SAM(AW/AR) -> optional per-channel RegSlice -> Rob
                       W -----------------------------------------------> Rob
           -> S1 (NmuReqS1Bridge) -> Packetize
           -> REQ WormholeArbiter(3 in: AW, W, AR, pairing {AW->W}) -> REQ assignment
           -> DAT WormholeArbiter(2 in: AW, W, pairing {AW->W})     -> DAT assignment
```

The REQ and DAT branches are independent after `Packetize`: each has its own arbiter, AW-to-WLAST
lock, pending storage, channel assignment and physical output. One Narrow write flit may leave on
REQ in the same `noc_clk` cycle that one Data write flit leaves on DAT. REQ backpressure does not
stall an already-buffered DAT packet, and DAT credit exhaustion does not stall an already-buffered
REQ packet.

Because the external AXI interface has only one W channel, `w_meta_fifo_` records every accepted AW
in AW order. W is steered by the FIFO head's class and inherits that AW's route and ordering fields;
the entry retires on WLAST. Separate NoC drain never changes this association. The current C++
model already contains the two network-local arbiter/allocation pairs and tests cross-network
backpressure independence. A dedicated same-cycle REQ+DAT egress test is `[TBD]`.

- `AxiSlavePort`: current-model per-channel FIFOs (depth `NMU_QUEUE_DEPTH` = 16 each for AW / W / AR / B / R). Pure transport, FIFO order per channel regardless of AXI ID.
- `SAM`: parallel combinational AW and AR range decode. Optional AW and AR register slices are selected independently by Section 2.7 parameters.
- `Rob`: request admission gate and response reorder buffer (Section 2.5). Consumes decoded route metadata and stamps ordering_req / ordering_tag.
- S1 (`NmuReqS1Bridge`): one 1-entry pipeline register per channel (AW, W, AR), drained independently so a full AW register never blocks the W stream that must release the wormhole lock.
- `Packetize`: builds the flit (Section 2.2). W flits inherit dst_id / ordering_req / ordering_tag from a write-metadata FIFO (`w_meta_fifo_`), pushed per AW flit, popped on the wlast W flit. The model queue is unbounded, its occupancy is bounded by the number of accepted AW bursts whose wlast W flit has not yet been emitted.
- `WormholeArbiter`: one independent instance per request network. REQ has 3 inputs (AW, W, AR); DAT has 2 (AW, W). Each owns its AW->W lock and drains at most 1 flit per cycle, with per-input pending depth `NMU_ARBITER_FIFO_DEPTH` = 4. Draining an AW flit (header.flit_tail=0) locks only that network to W until the flit with header.flit_tail=1 drains.
- Channel assignment: one independent instance per request network. REQ is single-VC ready/valid.
  The current C++ DAT allocator uses per-VC pending depth `NMU_ARBITER_FIFO_DEPTH` = 4.

**Current C++ model.** The VC candidate set is every VC in {0 .. NUM_VC-1}, with no
read/write class split. Only the DAT face carries NUM_VC > 1 (REQ and RSP are single-VC),
so `DataAw` / `DataW` are its only request-side traffic. Legal NUM_VC values are 1 to 8.
The working co-sim configuration set is {1, 2, 4, 8}.

**Target RTL overlay.** `NOC_DAT_VC_MODE` defaults to `SHARED`, which preserves the
current candidate set. `READ_WRITE_SPLIT` requires `DAT_NUM_VC` in {2, 4, 6, 8} and
restricts NMU `DataAw` / `DataW` to the lower half [0, `DAT_NUM_VC/2`). `DataW` inherits
the VC selected for its owning `DataAw` in either mode. The mode is system-wide: every
DAT router output applies the same class mask before assigning or restamping `vc_id`.
The target allocator reads the heads of the DAT Write class FIFO, selects only among VCs with
Router credit, stamps `vc_id`, and has no per-VC pending queue. The current C++ model implements
`SHARED` only and retains per-VC pending queues; target alignment remains tracked in
`docs/known-limitations.md`.

Fixed VC stamping: header `fixed_vc` = 1 on every ordering_req = 0 AW of either class (the whole same-(dst_id, awid) streak, first flit included) and on the W beats that copy it from their owning AW; ordering_req = 1 AW and AR leave it 0. The rule keys on channel kind and ordering_req, not face -- a `NarrowAw` with ordering_req = 0 rides the single-VC REQ face and still carries fixed_vc = 1, vacuously true there. Downstream routers keep `vc_id` unchanged only where fixed_vc = 1 instead of restamping at VA (docs/router-spec.md R12/SPEC6).

Per-cycle evaluation order inside the model (`Nmu::tick`, this order is the spec): WormholeArbiter, VcAllocator, S1-to-Packetize, AxiSlavePort request forward, then the response drains (S2 stages, B always, R per mode), Depacketize last. Consequences that are cycle-visible: a flit entering the WormholeArbiter and the VcAllocator traverses both in the same cycle (wormhole runs first), and an AXI ID freed by this cycle's B / R response is not yet usable by this cycle's request side (request side runs first).

### 2.5 Response path and the reorder buffer

```
response faces -> Depacketize -> Rob -> S2 (1-entry register per channel) -> AxiSlavePort -> AXI master
```

Target RTL child ports use the generated per-channel AXI payloads and the packed records in
`ni_child_types_pkg`; `valid` and `ready` are independent wires. `nmu_sam` accepts
`nmu_sam_aw_t` and `axi_ar_t`, then produces `nmu_sam_aw_result_t` and
`nmu_sam_ar_result_t`. `nmu_rob` produces `nmu_aw_request_t` / `nmu_ar_request_t`, accepts
decoded `nmu_b_response_t` / `nmu_r_response_t`, and returns ordered `axi_b_t` / `axi_r_t`.
The complete ordering-domain record is `nmu_ordering_domain_t {dst_id, dst_port_id, is_data}`;
no address, AXI ID, VC, or ordering tag is part of that comparison key.

RoB storage is frozen as four records. `nmu_rob_order_entry_t` contains `base`, a nine-bit
`beat_count`, `ordering_req`, and the write-only `collective` interlock. `nmu_b_rob_entry_t`
contains `occupied`, `complete`, and one `axi_b_t beat`. `nmu_r_rob_entry_t` contains
`occupied`, `complete`, the three-bit `narrow_lane`, and one `axi_r_t beat`.
`nmu_read_context_t {local_addr, len, size, burst, beat_index}` retains the narrow-read address
basis for Enabled bypass and the structural `READ_ROB_ENABLED=0` path. Allocation/completion bits
are not overloaded: reservation sets `occupied`; response arrival sets `complete`; AXI-side
retirement releases the slot.

**Target integration overlay.** RSP and DAT Read enter independent `noc_clk` class FIFOs under
ready/valid. The NoC-to-AXI assigner depacketizes them and writes the B or R dual-clock FIFO;
`vc_id` is not used for NI queue selection. B and R may therefore assert `bvalid` and `rvalid` in
the same `ACLK` cycle and each waits only on its own ready. `DataB` and `DataR` use different
physical networks. `NarrowB` and `NarrowR` share RSP bandwidth but become independent after B/R
assignment. The current C++ model does not implement this CDC or asymmetric DAT overlay.

Why a reorder buffer exists: AXI4 requires that responses with the same ID return in issue order. The fabric does not guarantee this. Two same-ID reads to different destinations can return out of order (a near slave answers before a far one), and with multiple VCs even same-destination traffic could overtake if it changed VC mid-stream. The NMU owns same-ID response ordering: it either proves a request cannot be overtaken (bypass) or reserves reorder storage for it before it enters the network.

Admission decision per AW / AR, evaluated per AXI ID (`Rob::push_aw` / `push_ar`):

**IMPORTANT** (unique classification, exactly one branch applies, in this priority):
1. Idle-ID bypass: the ID's order list is empty (nothing in flight for this ID). No slot, ordering_req=0, and the sticky flag resets (fresh streak).
2. Same-ordering-domain bypass: the ID is not sticky-fallen-back AND `{dst_id, dst_port_id, AXI class}` equals the previous accepted push's key for this ID and direction. No slot, ordering_req=0.
3. Fall-back: allocate slots, ordering_req=1, and set the sticky flag. Once sticky, every later push of this ID allocates until the ID goes idle again (branch 1 is the only reset).

A collective AW (Section 2.8) is admitted only through branch 1, and while one is in flight nothing else for that ID is admitted at all.

Example, ID = 3'h3, write direction: AW#1 dst 8'h02 (list empty, branch 1, bypass). AW#2 dst 8'h02 (same dest, branch 2, bypass). AW#3 dst 8'h05 (dest changed, branch 3, allocate, sticky). AW#4 dst 8'h05 (same dest as #3 but sticky, branch 3, allocate). All four complete and the list empties. AW#5 dst 8'h05 (branch 1 again, bypass). A counterexample for branch 2: AW#2 with dst 8'h07 would take branch 3, because 8'h07 != 8'h02.

Slot pools, per direction: B pool depth `NMU_ROB_B_DEPTH` = 128, R pool depth `NMU_ROB_R_DEPTH` = 128. An AW reserves 1 slot (B is one beat). An AR reserves ARLEN+1 consecutive slots (one per R beat), refused when free space is short. The allocator is a high-water stack: one allocation bit marks each reserved range's top slot, free space is the slot count above the highest set bit (leading-zero-count in RTL), the next base is depth minus free space, and space returns only from the top (Appendix 7.1 walks it with numbers). ordering_tag stamps the base slot. On the response side, B fills its slot, the i-th R beat of a burst fills base+i, and beats release to the master only while the ID's oldest outstanding transaction is being served (per-ID issue order, one order list per ID per direction).

`RobMode` selects the R side only. RTL exposes the elaboration-time bit parameter `READ_ROB_ENABLED`, defaulted from `NMU_READ_ROB_ENABLED_DFLT`, and selects the two structural paths with `generate if`; no preprocessor conditional controls this choice. `RobMode::Enabled`: R responses use the slot pool as above. `RobMode::Disabled` (`READ_ROB_ENABLED = 0`): the R RoB is off. Each AXI ID keeps an outstanding counter and one latched ordering-domain key `{dst_id, dst_port_id, AXI class}`. An idle ID accepts any AR and latches its key. A non-idle ID accepts another AR only when the key matches and the counter is below `NMU_MAX_TXNS_PER_ID`; otherwise it stalls until the counter returns to zero. Acceptance increments the counter and retirement of an R beat with `rlast` decrements it. Every Disabled-mode AR carries `ordering_req=0` and reserves no R slot. The B-side RoB always runs in both modes as a per-ID metadata-only RoB: it preserves issue order within an ID while permitting responses of different IDs to pass independently. Wherever this document says "RobMode", it governs reads only.

Per-ID transaction gate, both directions and both R modes: at most `NMU_MAX_TXNS_PER_ID` = 32 outstanding transactions per ID. A burst is one transaction regardless of ARLEN. An entry or counter unit is taken when the request is accepted and released when the response is accepted at the AXI side, B on its single beat and R on rlast. A 33rd same-ID request is refused until one completes, which backpressures through the AxiSlavePort queue to awready / arready.

There is no aggregate pool above the per-ID gate, so in-flight requests cap at `NMU_MAX_TXNS_PER_ID` x 2^`AXI_ID_WIDTH` = 32 x 8 = 256 per direction. Two limiters coexist on the write side and on Enabled reads, the per-ID order-list depth and the RoB slot pool, and which one binds depends on the traffic: a bypassed transaction takes a list entry and reserves no slot, so a stream that stays in branches 1 and 2 meets only the per-ID gate. Disabled reads have no slot-pool limiter but cannot cross an ordering-domain boundary until that ID becomes idle.

### 2.6 Worked example: 2-beat write burst

NMU at node (x=0, y=0), src_id = 8'h00, `mesh_4x4` SAM, DAT_NUM_VC = 1. The master issues AW {awid = 3'h5, awaddr = 0x6_0000_0080, awlen = 8'h01, awsize = 3'h6, awburst = 2'b01} then W0 {wdata = 512'h...11, wstrb = 64'hFFFF_FFFF_FFFF_FFFF, wlast = 0} and W1 {wdata = 512'h...22, wstrb = 64'hFFFF_FFFF_FFFF_FFFF, wlast = 1}.

SAM: 0x6_0000_0080 hits the dst_id = 8'h12 entry, memory space, local_addr = 0x6_0000_0080 (forwarded unchanged). ID 5 is idle, so admission takes the idle-ID bypass: ordering_req = 0, ordering_tag = 0. The burst is data class, so the three flits leave on the DAT face, in this exact order, as one wormhole packet:

| Field | AW flit | W flit 0 | W flit 1 |
|---|---|---|---|
| header.axi_ch | 4'd5 (DataAw) | 4'd6 (DataW) | 4'd6 |
| header.src_id | 8'h00 | 8'h00 | 8'h00 |
| header.dst_id | 8'h12 | 8'h12 (inherited from AW) | 8'h12 |
| header.fixed_vc / vc_id | 1 / 3'd0 | 1 / 3'd0 (follows the AW's VC) | 1 / 3'd0 |
| header.flit_tail | 1'b0 (opens packet) | 1'b0 | 1'b1 (closes packet) |
| header.ordering_req / ordering_tag | 0 / 8'h00 | 0 / 8'h00 | 0 / 8'h00 |
| header.collective_op / collective_mask | 0 / 8'h00 | 0 / 8'h00 | 0 / 8'h00 |
| payload | awid=3'h5, awaddr=48'h6_0000_0080, awlen=8'h01, awsize=3'h6, awburst=2'b01, others 0 | wlast=0, wstrb=64'hFFFF_FFFF_FFFF_FFFF, wdata=512'h...11, wuser=0 | wlast=1, wstrb=64'hFFFF_FFFF_FFFF_FFFF, wdata=512'h...22 |

Later one B flit returns on the RSP face {axi_ch = 4'd8 (DataB), bid = 3'h5, bresp = 2'b00} and the NMU presents bvalid / bid = 3'h5 / bresp = 2'b00, held until bready.

### 2.7 Parameters

Functional parameters use `specgen/source/constants.yaml`, generated into `ni_params.h` and
`ni_params_pkg.sv` (drift-gated by `codegen.py --check`). The two `*_SAM_REG_TYPE` entries are
RTL module parameters because they select physical timing cuts rather than topology or protocol
behavior. Defaults below are the shipped values.

| Parameter | Default | Legal range | Consumed by |
|---|---|---|---|
| AXI_ID_WIDTH | 3 | 1..8 | External AXI ID fields and per-ID RoB arrays |
| NOC_ID_WIDTH | 3 | fixed 3 | NoC-carried transaction ID after NI remap |
| AXI_ADDR_WIDTH | 48 | 1..64 | Address fields |
| AXI_DATA_WIDTH | 512 | {32,64,128,256,512,1024} | wdata / rdata, WSTRB_WIDTH = 64 |
| AXI_AWUSER_WIDTH | 58 | 10..64 | AWUSER slave-port field and the DPI unpack mask: 8 b user + 2 b collective_op + 48 b collective address mask (Section 2.8) |
| NOC_DAT_NUM_VC | 2 | 1 to 8; Split requires {2,4,6,8} | DAT VC count and credit vector width; wrapper-local `DAT_NUM_VC` is an alias |
| NOC_DAT_VC_MODE | SHARED (0) | {SHARED (0), READ_WRITE_SPLIT (1)} | Target `VcAllocator` eligible mask; system-wide with DAT router VA; current model implements SHARED only |
| NOC_REQ_FLIT_WIDTH | 136 | fixed | REQ egress flit port |
| NOC_RSP_FLIT_WIDTH | 126 | fixed | RSP ingress flit port |
| NOC_DAT_FLIT_WIDTH | 633 | fixed | DAT flit ports, both directions |
| NOC_ROUTER_VC_DEPTH | 8 | power of two, >= 2 | Router LOCAL input VC FIFO depth and NMU DAT sender-credit seed |
| AXI_FIFO_DEPTH | 8 | power of two, >= 2 | Common AW/W/AR/B/R dual-clock FIFO depth |
| `NOC_FIFO_DEPTH` | 8 | positive power of two | Common REQ/RSP/DAT Write/DAT Read synchronous `noc_clk` FIFO depth |
| NMU_ROB_B_DEPTH | 128 | 1..256 | B slot pool |
| NMU_ROB_R_DEPTH | 128 | 1..256 | R slot pool |
| READ_ROB_ENABLED | 1 | {0,1} | RTL `generate if`: Normal R RoB or RoB-less per-ID ordering-domain counters |
| NMU_MAX_TXNS_PER_ID | 32 | 1..256 | Per-ID order-list depth |
| NMU_QUEUE_DEPTH [current model] | 16 | 1..1024 | Single-clock AxiSlavePort AW/W/AR/B/R queues |
| NMU_DEPKT_Q_DEPTH | 16 | 1..1024 | Depacketize B/R queues |
| NMU_ARBITER_FIFO_DEPTH [current model] | 4 | 1..64 | Wormhole per-input and VC pending queues; not target NI VC storage |
| AW_SAM_REG_TYPE | 0 | {0,1,2} | AW decode-to-RoB slice: bypass, simple register, full skid |
| AR_SAM_REG_TYPE | 0 | {0,1,2} | AR decode-to-RoB slice, independently selected |

C++ model runtime configuration per instance: src_id, SAM config path, RobMode and RoB depth overrides come through `cmodel_nmu_create` / `cmodel_nmu_create_ex` (Section 3.3). The generated testbench sets src_id = {y[3:0], x[3:0]} per node and forwards the plusargs `+sam_config=`, `+b_rob_depth=`, `+r_rob_depth=`, `+max_txns_per_id=`. These plusargs configure the reference model only; the RTL image uses the generated package selected at elaboration.

### 2.8 Collective writes

A collective write is one AW+W burst the master issues once and the fabric replicates to an aligned submesh, answered by one merged `B`. The master expresses it on AWUSER, `AXI_AWUSER_WIDTH` = 58 bits wide:

| AWUSER bits | Field | Meaning |
|---|---|---|
| [7:0] | user | opaque, carried in the flit payload |
| [9:8] | collective_op | 2'd0 UNICAST, 2'd1 MULTICAST, 2'd2-3 reserved |
| [57:10] | collective address mask | wildcard bits over the 48-bit address |

Reads have no collective surface: ARUSER is 8 bits, so every AR leaves with `collective_op` = UNICAST and a zero mask. Both AXI classes multicast. Narrow-class collectives are the config-space replication case, where one config write reaches every tile of a submesh at the same node-local offset, and they fork on the REQ network; Data-class collectives fork on DAT.

**Translate.** A set address-mask bit is a don't-care, so a mask with n set bits names 2^n replica addresses. The SAM is a first-match range lookup with no stored node-index bit offset, so the translate enumerates rather than bit-selects (`addr_trans::collective_translate`):

**INPUT** AWUSER, AWADDR, AWLEN / AWSIZE / AWBURST. **COMPUTE** enumerate all 2^n masked addresses and translate each through the SAM. Every replica must land in a tile, carry the same node-local offset, carry the same class, name a distinct `dst_id`, and pass the burst-footprint check for its own aperture. **OUTPUT** an 8-bit node mask, the OR of each replica's `dst_id` against the one the request address resolves to, stamped into the AW header beside `collective_op`. `dst_id` stays the one the request address resolves to. The W metadata FIFO latches both fields per AW and stamps every W beat of the burst, so the fabric forks the W beats to the AW's exact branch set.

n is capped at X_WIDTH + Y_WIDTH = 8, so the enumeration is at most 256 SAM lookups. It re-runs on every backpressure retry of the same AW. That is a model-only cost.

**Reject set.** These are permanent illegal inputs, not retryable backpressure, so each is a fatal assert and abort, the convention this block already uses for inputs that never clear on retry:

| Condition | Why |
|---|---|
| `collective_op` = UNICAST with a nonzero mask | mask without op contradicts the spec |
| `collective_op` = MULTICAST with a zero mask | empty destination set. Not downgraded to unicast: the op is explicit here, so a mismatch is a stimulus contradiction |
| reserved `collective_op` (2'd2, 2'd3) | undefined encoding |
| AWLOCK set on a collective | AxLOCK is unicast only |
| more mask bits set than a node id has | destination set larger than the mesh |
| the request address or a replica address maps to no tile | the set leaves the mesh |
| replicas disagree on the node-local offset | one aligned region per node is what makes the replicas addressable |
| replicas straddle the narrow and data classes | the class picks the network the worm forks on, and a packet rides one |
| the mask names one node twice | the set is not a wildcard over `dst_id` |
| a replica burst overruns its aperture | same footprint rule as a unicast burst |
| AWUSER bits above [57] set | they would be silently dropped by the field accessors |
| a collective on the direct `Packetize::push_aw` interface | that path bypasses `Rob`, which owns the validate and translate |

**R2 admission.** At most one collective is outstanding per (NMU, AXI ID). Two gates at `Rob::push_aw` entry, both returning retryable backpressure rather than an error:

1. An incoming collective is admitted only when the ID's write order list is empty. It then takes the idle-ID bypass: ordering_req = 0, no RoB slot, in both `RobMode` settings.
2. While the front entry of an ID's order list is collective, nothing for that ID is admitted. Testing the front is enough, because a collective only ever enters an empty list and is therefore the only entry.

Gate 2 is what closes the same-ordering-domain bypass: without it a later same-ID AW with the collective's `{dst_id, dst_port_id, AXI class}` key would stream past it with no slot and no ordering. The collective still takes one entry of the ID's write order list like any AW, released when its merged `B` retires.

**Merged B.** The merged `B` returns through the ordinary B ingress and releases the interlock exactly like a unicast `B`: the NMU stamps ordering_tag before fanout and the fabric's merge preserves it, so the response path needs no collective state. The `B` carries `collective_op` = MULTICAST and the echoed node mask; neither perturbs the `bid` / `bresp` decode.

Exactly one `B` per collective AW is a structural invariant of the merge, and two independent checks catch a violation. The order-list head check in `pop_b_staged` fires first on both callers: the first `B` pops the ID's only order-list entry, so a duplicate finds an empty list and aborts on "bypassed B does not match the head of its id's order list". The `write_txns_ > 0` underflow assert in `retire_b` is the backstop behind it.

## 3. Inputs and Outputs

All ports below are the current C++/DPI `nmu_wrap` ports (`ref_model/top/nmu_wrap.sv:54-77`). AXI signals are fields of the packed structs `ni_signals_pkg::axi_req_t` / `axi_rsp_t`, plus a dedicated `awuser_i` port (the generated `axi_req_t` has no awuser field). The three NoC faces are scalar signals, not structs: REQ and RSP are ready/valid, DAT is credit in both directions. The `axi_req_t` struct carries `awregion` / `arregion`, but they are not in the DPI signature: the model sees region = 0 and stamps 0 into the flit. No AXI user signals other than `awuser_i` exist on the wire face, the flit's user fields are stamped 0 and response user fields are dropped. The target DAT receive and clock-domain deltas are stated below the output table.

### 3.1 Inputs

| Signal | Bit Width | Definition |
|---|---|---|
| clk_i | 1 | Clock. All sampling on the positive edge. |
| rst_ni | 1 | Synchronous active-low reset. Given only once, at the beginning of simulation. |
| ctx_i | 64 | Model instance handle returned by `cmodel_nmu_create`. Constant after creation. |
| axi_req_i.awvalid | 1 | AW valid. Must stay high until awready is observed. |
| axi_req_i.awid | 3 | Write transaction ID. Sampled only on the awvalid && awready cycle. |
| axi_req_i.awaddr | 48 | Write address. Must hit a SAM entry (guarantee G1). |
| axi_req_i.awlen | 8 | Burst length minus 1, 0 <= awlen <= 8'hFF (256 beats max). |
| axi_req_i.awsize | 3 | Beats of 2^awsize bytes, awsize <= 3'h6 = 64 bytes. |
| axi_req_i.awburst | 2 | 2'b00 FIXED, 2'b01 INCR, 2'b10 WRAP. 2'b11 never occurs. |
| axi_req_i.awlock, awcache, awprot, awqos | 1, 4, 3, 4 | Attribute pass-through into the flit payload. |
| axi_req_i.awregion | 4 | Present in the struct, not sampled: the flit carries awregion = 4'h0. |
| awuser_i | 58 | Collective surface, Section 2.8: [7:0] user, [9:8] collective_op, [57:10] address mask. Sampled with AW. |
| axi_req_i.wvalid | 1 | W valid. Must stay high until wready is observed. |
| axi_req_i.wdata | 512 | Write data, valid only when wvalid is high. |
| axi_req_i.wstrb | 64 | Byte strobes. |
| axi_req_i.wlast | 1 | High on the final beat of each write burst. |
| axi_req_i.bready | 1 | Master ready for B. |
| axi_req_i.arvalid, arid, araddr, arlen, arsize, arburst, arlock, arcache, arprot, arqos, arregion | as AW | Read address channel, field-for-field mirror of AW (arregion likewise not sampled). ARUSER has no port: reads carry no collective surface. |
| axi_req_i.rready | 1 | Master ready for R. |
| tx_req_ready_i | 1 | REQ egress ready. Advisory, sampled two registrations late (Section 3.4 rule P4). |
| rx_rsp_valid_i | 1 | An RSP flit is on the wire this cycle. |
| rx_rsp_flit_i | 126 | RSP flit, format of Section 2.2. axi_ch must be `NarrowB`, `DataB` or `NarrowR` (guarantee G2). Unknown when valid is low. |
| rx_dat_valid_i | 1 | A DAT flit is on the wire this cycle. |
| rx_dat_flit_i | 633 | DAT flit. axi_ch must be `DataR` (guarantee G2). Unknown when valid is low. |
| tx_dat_crdvalid_i | DAT_NUM_VC | Per-VC one-cycle credit pulse: bit v high means the router drained one NMU DAT request flit from VC v. Multiple bits may pulse in one cycle. |

### 3.2 Outputs

Every output is a registered signal: it changes only at posedge clk_i and reflects the model state computed from the previous cycle's inputs.

| Signal | Bit Width | Definition |
|---|---|---|
| axi_rsp_o.awready | 1 | One-shot: high for exactly 1 cycle, only after awvalid is observed and the AW FIFO has space. See rule P7. |
| axi_rsp_o.wready | 1 | Pre-asserted: high whenever accepted AWs still owe W beats and the W FIFO has space. Does not wait for wvalid. |
| axi_rsp_o.arready | 1 | One-shot, same policy as awready, independent of the write side. |
| axi_rsp_o.bvalid | 1 | B beat available. Held high until bready is observed (AXI4 IHI 0022 A3.2.1). |
| axi_rsp_o.bid | 3 | B transaction ID. 0 when bvalid is low. |
| axi_rsp_o.bresp | 2 | Write response, masked to 2 bits. 0 when bvalid is low. |
| axi_rsp_o.rvalid | 1 | R beat available. Held high until rready is observed. |
| axi_rsp_o.rid, rdata, rresp, rlast | 3, 512, 2, 1 | Read response fields. 0 when rvalid is low. |
| tx_req_valid_o | 1 | REQ request flit on the wire. The model-facing wrapper holds `valid` and `tx_req_flit_o` until `tx_req_ready_i` is sampled high. At most one flit transfers per cycle. |
| tx_req_flit_o | 136 | REQ request flit, format of Section 2.2. 0 when valid is low. |
| rx_rsp_ready_o | 1 | RSP ingress ready. Tied true: the model's ingress queue is unbounded (`nmu_wrap.hpp`). |
| tx_dat_valid_o | 1 | DAT request flit on the wire this cycle. At most one flit per cycle. Not held: the flit is consumed by the credit protocol, there is no ready wire. |
| tx_dat_flit_o | 633 | DAT request flit. 0 when valid is low. |
| rx_dat_crdvalid_o | DAT_NUM_VC | Per-VC one-cycle consumer credit pulse: bit v high means the NMU consumed one DAT response flit from VC v. At most one pulse per VC per cycle (pending consumptions queue up and drain one per cycle). |

The table above is the current C++/DPI interface. The target RTL changes only the DAT receive
backpressure surface: `rx_dat_crdvalid_o` is removed and `rx_dat_ready_o` is added. A `DataR` flit
transfers from the Router when `rx_dat_valid_i && rx_dat_ready_o`; ready reflects DAT Read class
FIFO capacity. DAT transmit keeps `tx_dat_crdvalid_i` because the destination Router owns the
credited per-VC FIFO. The target also exposes separate `ACLK`/`ARESETn` and
`noc_clk`/`noc_rst_n` domains around the five AXI channel async FIFOs.

Both resets are generated above the NMU from one common system reset. Each reset asserts
asynchronously and deasserts synchronously in its own clock domain; release skew is legal. The NMU
has no `sys_rst_n` port and does not support one-sided reset recovery. A reset discards all queued
and in-flight NMU state, including FIFO contents, RoB/order state and credit counters.

### 3.3 DPI functions

Each posedge the SV wrap runs the 3-call discipline: `cmodel_nmu_set_inputs` (latch wires, no state
change), `cmodel_nmu_tick` (advance the model one cycle), and `cmodel_nmu_get_outputs` (copy the
output latch), then registers the outputs nonblocking. On model-facing REQ egress only, the
registered one-cycle C++ strobe feeds the approved `spill_register`; the wrapper returns the
primitive's input capacity to the model and holds the RTL-facing flit until `valid && ready`.
DAT bypasses this adaptation and remains credit-controlled. Declared in
`ref_model/dpi/cmodel_dpi.h:151-179`.

| Function | Signature (summary) | Semantics |
|---|---|---|
| cmodel_nmu_create | `unsigned long long (const char* name, int src_id, int dat_num_vc, const char* config_path)` | Constructs the instance, RobMode::Enabled (R side), default depths. `dat_num_vc` sizes the DAT face only; REQ and RSP are fixed single-VC. `config_path` is required: NULL or empty throws inside `NmuWrap::init`, which the DPI boundary catches into the error latch and returns handle 0. Returns the 64-bit handle for ctx_i. |
| cmodel_nmu_create_ex | `unsigned long long (const char* name, int src_id, int dat_num_vc, int rob_enabled, int b_rob_depth, int r_rob_depth, int max_txns_per_id, const char* config_path)` | As create, plus R-RoB enable and depth overrides. The generated testbench calls this in both RoB modes: it is the only entry point carrying the overrides, and the B-side RoB runs regardless of `rob_enabled` (Section 2.5). |
| cmodel_nmu_set_inputs | `(ctx, AXI args incl. a 58 b awuser, then the three NoC faces: tx_req_ready, rx_rsp_valid + flit, rx_dat_valid + flit, tx_dat_crdvalid)` | Latches inputs only. Packing: 8-bit fields in word[0] low byte, addresses 2 words little-endian, data 16 words little-endian, wstrb 2 words, awuser 2 words, flits little-endian at their own network's word count (REQ 5, RSP 4, DAT 20), credit vector 1 word bit-per-VC. |
| cmodel_nmu_tick | `(ctx)` | One full model cycle. One call = one clock edge. |
| cmodel_nmu_get_outputs | `(ctx, AXI response args, tx_req_valid + flit, rx_rsp_ready, tx_dat_valid + flit, rx_dat_crdvalid)` | Copies the output latch. bresp / rresp masked with 2'b11. |
| cmodel_nmu_read_slot_hwm | `unsigned int (ctx)` | Statistic: peak R-RoB slot occupancy. 0 when the handle is invalid or the R RoB is Disabled. Printed per node at testbench exit. |
| cmodel_nmu_admission_stats | `void (ctx, out aw_idle_bypass, aw_same_dest_bypass, aw_fallback_alloc, ar_idle_bypass, ar_same_dest_bypass, ar_fallback_alloc, order_list_hwm, write_txns_hwm, read_txns_hwm)` | Statistics: the SPEC 17 admission clause counts over accepted pushes (AW and AR counted separately, AR only in RobMode::Enabled), the deepest per-ID order list and the peak in-flight transaction count per direction. All outputs 0 when the handle is invalid. Printed per node at testbench exit on the same `[HWM]` line. |

An invalid handle raises a categorized model error which the testbench error poll turns into `$fatal`.

### 3.4 Protocol rules

1. Input rhythm: the AXI face is handshake-paced, there is no fixed delivery rhythm. Within one write, exactly awlen+1 W beats follow their AW in issue order and bursts never interleave W beats (AXI4 has no WID). Example: AW with awlen = 8'h01 is followed by 2 W beats, the second with wlast = 1. Each response face presents at most one flit per cycle. Credit vectors may pulse any subset of bits in any cycle.
2. Idle state: input payload buses are don't-care while their valid is low, the model reads them only on the handshake cycle (valid and ready both high) and reads a response flit only while that face's valid is high. All output payload buses are driven to 0 while their valid is low (example: `tx_dat_flit_o` = 633'h0 whenever `tx_dat_valid_o` = 0).
3. Sampling edge: all inputs are sampled at the positive edge of clk_i. One posedge = one model cycle via the 3-call DPI discipline of Section 3.3. Outputs computed from cycle-N inputs first enter the wrap register during cycle N+1. REQ then crosses the model-facing spill register before reaching the wire; the other outputs remain directly registered. The verification pattern captures outputs at the positive edge.
4. Valid behavior: bvalid and rvalid, once asserted, stay asserted with stable payload until the cycle their ready is high (AXI4 IHI 0022 A3.2.1). `tx_req_valid_o` follows the same held-ready/valid rule: a REQ transfer occurs only on `tx_req_valid_o && tx_req_ready_i`, and valid plus flit remain stable while stalled. `tx_dat_valid_o` remains a per-flit credit-qualified strobe and never waits on a ready; the per-VC credit counter guarantees receiver space.
5. Output idle value: bid, bresp, rid, rdata, rresp, rlast are 0 when their valid is low. `tx_req_flit_o` and `tx_dat_flit_o` are 0 when their valid is low. Credit outputs are 0 in non-pulse cycles.
6. Reset: rst_ni is given only once, at the beginning of simulation, synchronous active-low. While rst_ni is low all outputs are 0 (the wrap clears its output registers, the model starts reset by construction). There is no mid-run reset.
7. Handshake gaps: awready and arready are one-shot, they assert only after their valid is observed and drop the cycle after the handshake, so consecutive same-channel address handshakes are at least 2 cycles apart (handshakes on cycles 2, 4, 6 of the Section 6 waveform pattern). AW acceptance is not gated on the previous write's W burst finishing (multi-outstanding AW is supported and w-owed counts accumulate). wready is pre-asserted while owed W beats remain and FIFO space exists, so W beats can stream back-to-back, 1 per cycle. An incoming request credit pulse is usable in the same model cycle (replenished before the tick). At most one response credit pulse per VC per cycle is emitted.
8. Latency: measured from the positive edge ending the input's handshake cycle to the first cycle the corresponding output valid is high, in an otherwise idle NMU with the egress ready and DAT credit available. With `AW_SAM_REG_TYPE=0`, AW handshake to its REQ flit on the model-facing egress is exactly 4 cycles; values 1 and 2 add one cycle. `AR_SAM_REG_TYPE` changes the AR path by the same amount. The added cycle is the verification-only REQ spill register. Response B flit to bvalid is exactly 3 cycles. Response R flit to rvalid is exactly 2 cycles with the R RoB Disabled, exactly 3 cycles with it Enabled. Under load these are lower bounds, backpressure adds cycles without an upper bound.

### 3.5 Input guarantees

The implementation does not handle the following, they are guaranteed not to happen. The model asserts and aborts on each (assert locations in parentheses).

| # | Guarantee | Reason / model check |
|---|---|---|
| G1 | Every issued address hits a SAM entry. | The system address map covers all issued addresses. Miss aborts (`SamTable::translate`). No DECERR path exists. |
| G2 | Only B or R flits arrive on the response faces. | Fabric routes request and response classes on disjoint networks. Other axi_ch aborts (`Depacketize::drain_ingress_`). |
| G3 | No burst crosses a SAM region boundary. | Regions are 4 KiB aligned and sized and AXI4 forbids 4 KiB crossings, so the upstream master never issues one. The model asserts this only on the direct Packetize path (`Packetize::push_aw` / `push_ar`). |
| G4 | The SAM itself is well-formed: nonzero representable 4 KiB-aligned ranges, valid destination/port/class membership, and complete required space coverage. Overlap is legal and authored-first deterministic. | Target generator checks these constraints. The current C++ `SamTable::validate` still rejects overlap; this is a recorded follow-on implementation gap. |
| G5 | valid, once asserted, holds with stable payload until ready (both directions of the AXI face). | AXI4 A3.2.1. The one-shot ready policy depends on it: ready asserts one cycle after valid is first seen. |
| G6 | W beat counts match their AWs (exactly awlen+1 beats, wlast on the final beat, no spurious W). | AXI4 legality is the master's job. The owed-W counter floors at 0 and does not reject an unexpected W. |
| G7 | On DAT request injection, the Router never returns more credits than LOCAL input VC slots it freed. On target DAT response ejection, the Router holds valid/flit until NMU ready; the current model instead guarantees a response sender credit. | Request credit conservation uses `NOC_ROUTER_VC_DEPTH` = 8. Target response safety follows ready/valid; current-model credit lies abort in `VcAllocator::tick`. |
| G8 | awburst / arburst = 2'b11 never occurs, and header.axi_ch values 4'd10 to 4'd15 never occur. | Reserved encodings. |
| G9 | DAT_NUM_VC is 1 to 8 and the elaborated `noc_credit_t` width equals DAT_NUM_VC. | Out-of-range DAT_NUM_VC aborts at `VcAllocator` construction, width mismatch is `$fatal` at elaboration (`nmu_wrap.sv`). |
| G10 | A collective is never issued by a peripheral. | Collectives are a tile-to-tile primitive (`noc-target-spec.md` Scope). The fork spreads along the issuer's row and the join collects in its column, and a peripheral hangs off a boundary port, outside both. A peripheral is never a member either: its address space declares no coordinate ranges, so it is not a collective target. Aborts (`collective_translate`, on the issuer's `port_id`). |

## 4. Specifications

Each item names its verification and the failure condition. "ctest" items run in the pure C++ suite (`ref_model/c_model/tests/`), "co-sim" items run under the testbench (`make sim CONFIG=<config> PATTERN=<pattern>`) where correctness is judged by the scoreboard's per-transaction write-to-readback compare plus the model's internal asserts (any assert abort fails the run).

1. Interface: the block implements exactly the Section 3.1 / 3.2 ports of `nmu_wrap` with the given widths (REQ 136 b, RSP 126 b, DAT 633 b flits; DAT_NUM_VC-wide credit vectors). Verified: co-sim elaboration (AXI struct port binding, scalar NoC-face binding, `$fatal` width guard in `nmu_wrap.sv`). Failure: elaboration error or width-guard fatal.
2. Reset: after the single initial rst_ni assertion, every output is 0, and outputs stay 0 until traffic (rule P6). Verified: `TEST(NmuWrap, idle_adapter_keeps_readys_low)` (`ref_model/c_model/tests/wrap/test_nmu_wrap.cpp`) and cycle-1 sampling in co-sim. Failure: any nonzero output during or immediately after reset.
3. Registered outputs: outputs are functions of state as of the previous posedge, never combinational paths from same-cycle inputs (rule P3). Verified: `TEST(NmuWrap, single_aw_w_two_phase_handshake)`, which requires the 1-cycle valid-to-ready offset. Failure: same-cycle input-to-output dependence changes the handshake cycle count.
4. One-shot address ready: awready (arready) asserts only when awvalid (arvalid) was high the previous sampled cycle with FIFO space, stays high 1 cycle, and is low the cycle after a handshake. Consequence: at most one address handshake per 2 cycles per channel. Example: awvalid from cycle 1 gives awready only in cycle 2, and a second AW held from cycle 3 handshakes in cycle 4. Verified: `TEST(NmuWrap, single_aw_w_two_phase_handshake)`, `TEST(NmuWrap, multi_beat_w_burst_full_rate_aw_available)`. Failure: ready asserted before valid observed, held more than 1 cycle, or back-to-back address handshakes.
5. Pre-asserted wready: wready = (owed W beats > 0) AND W FIFO space, where the owed count increases by awlen+1 per accepted AW and decreases by 1 per accepted W beat, accumulating across outstanding AWs. Example: two accepted AWs with awlen = 8'h01 and 8'h00 give owed = 3, wready holds through 3 streaming W beats then drops. Verified: `TEST(NmuWrap, multi_beat_w_burst_full_rate_aw_available)`. Failure: wready high with no owed beats, or W throughput below 1 beat per cycle with space available.
6. Acceptance atomicity: a beat is consumed exactly on its valid-and-ready cycle, and ready is never asserted without guaranteed FIFO space, so an accepted beat is never dropped or duplicated. Verified: `TEST(NmuAxiSlavePort, AwBoundary_FailedPushDoesNotDuplicateOnRetry)` and co-sim scoreboard compare. Failure: lost or duplicated beat (scoreboard miscompare).
7. Per-channel FIFO order: beats of one channel travel in acceptance order regardless of AXI ID, through every request stage. Verified: `TEST(NmuAxiSlavePort, AwFifoOrder_PreservedAcrossMixedIds)`, `ArFifoOrder_PreservedAcrossMixedIds`. Failure: any same-channel reorder.
8. Flit format: emitted flits match Section 2.2 bit-exactly, every header field assigned (the header has no `rsvd`, `PADDING_FIELDS_COUNT` = 0), unused payload bits above the channel's width 0. Verified: `TEST(NmuPacketize, PushAwEmitsFlitWithCorrectFields)` for the header fields, `AwPayloadBitPerfect`, `WPayloadBitPerfect`, `ArEncodesAxiChAndOrderingTag` for the payloads (`ref_model/c_model/tests/nmu/test_packetize.cpp`). Failure: any mismatched bit. Note: `RsvdAndDisabledFieldsZero` is NOT evidence here — it calls `check_padding_is_zero()`, which is vacuously true once the header has no padding fields.
9. One beat, one flit, address forwarded: each accepted AW / W / AR beat emits exactly one flit, and the address payload carries the request address as it arrived. awregion / arregion and all user fields are carried in the flit and are 0 at the co-sim boundary. Verified: `TEST(AddrTrans, TileBaseStaysInTheForwardedAddress)`, `TEST(SamTable, PackedTranslateForwardsTheAddressUnchanged)`. Failure: an altered address in the payload, or wrong dst_id.
10. header.flit_tail stamping: AW = 0, W = wlast, AR = 1. Verified: `TEST(NmuPacketize, WHeaderFlitTailMatchesWlast)`, malformed stamping aborts in the wormhole arbiter (`WormholeArbiter::tick` defensive guards). Failure: assert abort or a wormhole packet that never closes.
11. AW before W: a W flit never enters the network before its AW flit. The RoB refuses W beats while no AW-accepted burst owes beats, and W flits inherit dst_id / ordering_req / ordering_tag from the AW-ordered metadata FIFO. Verified: `TEST(NmuRob, Disabled_WCreditBlocksWBeforeAw)`, `TEST(NmuPacketize, WMetaFifoInheritsAwDst)`, `TEST(NmuReqBridge, PushWBackpressuresOnEmptyMeta)`. Failure: W flit precedes its AW flit or carries wrong metadata.
12. Wormhole atomicity: after an AW flit drains on REQ or DAT, only W flits of that burst drain on that same network until the header.flit_tail = 1 W flit; the other physical network remains independently grantable. Verified: `TEST(NocWormholeArbiter, ArCannotInterleaveDuringLock)`, `MultiBeatWBurstFlowsAndUnlocks` (`ref_model/c_model/tests/router/test_wormhole_arbiter.cpp`), plus `TEST(NmuDatFace, ReqBackpressureDoesNotStallDat)` and `DatBackpressureDoesNotStallReq` for cross-network independence. Same-cycle dual-egress coverage is `[TBD]`. Failure: any foreign flit between an AW and its final W on one network, or backpressure crossing from one already-buffered network path into the other.
13. IMPORTANT wormhole tie-break: each network owns an independent round-robin pointer. REQ scans inputs 0 = AW, 1 = W, 2 = AR; DAT scans inputs 0 = AW, 1 = W. The scan starts after that network's last drained input and the first non-empty input wins, at most 1 flit per network per cycle. Example on REQ: pointer at 0 with AW and AR pending drains AW, locks to W, and after wlast drains from input 1 the pointer is 2, so AR wins the next free REQ cycle even if a new AW is pending. Verified: `TEST(NocWormholeArbiter, AwTriggersLock)` and the co-sim throughput scenarios. Failure: wrong winner or shared arbitration state between REQ and DAT.
14. SAM lookup: destination is the first authored entry whose `[base, base+size)` contains the
address, output `{dst_id, dst_port_id, class, collective metadata, addr}`. Overlap is a positive
priority case, not an error. Verified for non-overlap by `TEST(SamTable,
PackedTranslateForwardsTheAddressUnchanged)`, `TEST(SamTable,
TranslateIsInjectiveAcrossSpacesOfOneNode)`, and `TEST(SamYaml, SpaceAttributeSelectsClass)`.
Target overlap and generated-array reversal coverage is specified in `docs/nmu-verification-plan.md`.
Failure: wrong authored winner, wrong metadata, or an address altered on the way through.
15. SAM generation validation: reject zero or negative size, non-4-KiB-aligned base or size,
unrepresentable or overflowing base, end, or stride expansion, `start_addr >= end_addr`, invalid
destination/port/class/endpoint membership, duplicate node membership, or incomplete required
space coverage. Explicit
`en_collective: true` additionally requires a representable coordinate layout. Do not reject a
range overlap. Existing C++ tests cover the older validation paths, but
`TEST(SamValidator, RejectsOverlap)` documents current non-conforming behavior and must change in
the follow-on implementation task. Failure: an invalid range accepted, a legal overlap rejected,
or `false`/absent collective authorship producing nonzero coordinate selectors.
16. Same-ID response ordering: B and R beats presented to the master for one AXI ID follow the issue order of their requests, in every mode (B side always via the RoB, R side via slot pool when Enabled, via ordering-domain admission when Disabled). Cross-ID order and write-to-read same-address order are NOT guaranteed by this block. Verified for the current model by the existing Enabled tests; Disabled ordering-domain coverage is `[TBD]` until the model is aligned. Failure: any same-ID response inversion.
17. IMPORTANT admission classification: exactly one of {idle-ID bypass, same-ordering-domain bypass, fall-back allocate} applies per AW / AR, in that priority, per the Section 2.5 tree, with the sticky flag reset only by the idle-ID branch. The key is `{dst_id, dst_port_id, AXI class}`. Example: Section 2.5 five-AW trace, AW#4 allocates although its destination matches AW#3 because the ID is already sticky. Existing destination/class tests remain applicable; `dst_port_id` coverage is `[TBD]` until the model is aligned. Failure: wrong branch taken.
18. Slot reservation: AW takes 1 slot, AR takes arlen+1 consecutive slots, base = pool depth - free space, refused when free space is short, and refusal mutates no state. Free space is the count above the highest allocated range top (high-water stack, Appendix 7.1). Verified: `TEST(NmuRob, Enabled_PushAr_AllocatesConsecutiveSlotsForBurst)`, `Enabled_LzcAllocator_IsAStack`, `Enabled_PushAw_PoolFull_ReturnFalseAtomic`, `Enabled_PushAr_DownstreamBackpressure_AtomicRollback`. Failure: wrong base, overlapping ranges, or state change on refusal.
19. No free slot, no request: an ordering_req = 1 flit never enters the network without its slots already reserved, so a returning tagged response always finds its slot. Verified: `TEST(NmuRobDeath, Enabled_PopBWithUnallocatedOrderingTag_Abort)` (the model aborts on a tag with no slot). Failure: model abort on response arrival.
20. IMPORTANT per-ID release: responses release to the master only from the head of the ID's order list. A ready RoB'd entry behind an incomplete older entry waits, and a bypassed (ordering_req = 0) head is popped by its own response (B, or R with rlast) before anything behind it releases. Example: ID 3 issues bypassed AW#1 then RoB'd AW#2, B#2 arrives first and is held in slot, B#1 arrives and releases, then B#2 releases, master sees B#1 then B#2. Verified: `TEST(NmuRob, Enabled_PopB_OutOfOrder_HeldUntilHeadReady)`, `Enabled_PerBeatRelease_HeadBurstStreams`, `Enabled_BypassedBeat_ReleasesNoSlot`. Failure: release past a blocked head.
21. RobMode scope: `RobMode::Disabled` disables the R-side slot pool only. A same-ID AR with the same `{dst_id, dst_port_id, AXI class}` key is accepted until `NMU_MAX_TXNS_PER_ID`; a different key is refused until the ID becomes idle. The B-side RoB runs unconditionally in both modes. Verification is `[TBD]`: cover same-key acceptance, each key-field mismatch stalling, counter-full stalling, and release on `rlast`. Failure: a cross-domain AR enters early, a legal same-domain AR stalls below the limit, or B ordering changes with the R mode.
22. Per-ID transaction gate: at most max_txns_per_id (default 32 = 0x20) outstanding transactions per ID per direction, enforced before slot availability, refusal is stateless. Example: 32 outstanding ID-7 writes refuse the 33rd AW even with 31 free B slots. Verified: `TEST(NmuRob, Enabled_MaxTxnsPerIdGate_RefusesWithFreeSlotsAvailable)`, `Enabled_MaxTxnsPerIdDefaultIsThirtyTwo`. Failure: 33rd same-ID acceptance.
Items 23-25 verify the current `SHARED` C++ model. Target `READ_WRITE_SPLIT` coverage is
`[TBD]` until the model and RTL implement the Section 2.4 overlay.

23. VC range: every flit carries vc_id in {0 .. DAT_NUM_VC - 1}, with no per-class restriction — AW, W and AR draw from the same candidate set. With DAT_NUM_VC = 1 all flits carry vc_id = 0, which is also what REQ and RSP always carry. Verified: `TEST(NmuVcAllocator, Degenerate_NumVc1_AllModesPassthrough)`, `TEST(NmuVcAllocatorRoundRobin, DistinctReadIdsSpreadAcrossVcs)`. Failure: a flit outside the configured VC set.
24. IMPORTANT VC selection, per AW / AR flit, first matching rule wins: (a) W flits always take the VC of their AW (no selection). (b) DAT_NUM_VC = 1: VC0. (c) ordering_req = 0 AW whose ID has a recorded destination equal to the flit's dst_id: reuse the recorded VC, and if that VC lacks space or credit the flit stalls, it is never rerouted. (d) otherwise: scan all DAT_NUM_VC VCs round-robin from the selection pointer, first VC with pending space and credit wins, pointer moves past the winner. The (dst_id, VC) record is written on every accepted ordering_req = 0 AW of that ID and persists for the whole run until overwritten by the next such accept, it is never invalidated. AR flits and ordering_req = 1 flits always use rule (d) and never write the record. Example (DAT_NUM_VC = 4, pointer at 0): AW ID 5 dst 8'h12 ordering_req = 0 takes VC0 and records (8'h12, VC0), a second identical AW after the first burst's wlast reuses VC0 even though VC1 is idle, an AW ID 5 dst 8'h07 misses the record and round-robins to VC1. Verified: `TEST(NmuVcAllocatorRoundRobin, SameWriteIdDifferentDestRoundRobins)`, `RobbedFlitsRoundRobinRegardlessOfDest`, `NumVc1SameIdSameDestUnaffected`, `TEST(NmuVcAllocator, WFollowsAW_ReusedFixedVc)`. Header `fixed_vc` follows the same ordering_req = 0 / 1 split (2.4): 1 on every ordering_req = 0 AW of the streak and its W beats, 0 on ordering_req = 1 AW and on AR. Verified: `TEST_P(NmuVcAllocatorParam, FixedVcStampedOnOrderedAwStreak)`, `TEST_P(NmuVcAllocatorParam, FixedVcClearOnRobbedAwAndAr)`. Failure: wrong VC in any listed case, a mid-streak reroute, or wrong fixed_vc bit.
25. IMPORTANT VC drain tie-break: at most 1 flit per cycle leaves for the NoC, chosen by a single global round-robin over all DAT_NUM_VC VCs starting at the pointer, first VC that is non-empty and has sender credit wins, pointer set to winner+1. Example (DAT_NUM_VC = 4, pointer 2, flits pending on VC1 and VC3): VC3 wins (scan 2, 3), pointer becomes 0, VC1 wins next cycle. Verified: `TEST(NmuVcAllocatorRoundRobin, DistinctReadIdsSpreadAcrossVcs)` and co-sim link utilization scenarios. Failure: two flits in one cycle, a starved non-empty credited VC, or wrong winner in the tie case.
26. DAT request credit: the per-VC sender counter is seeded to NOC_ROUTER_VC_DEPTH = 8, decrements per emitted flit, increments per pulse on `tx_dat_crdvalid_i`, and a pulse arriving in cycle N is usable in cycle N. Invariant per VC: credit + un-credited in-flight flits = 8, and the NMU never emits on a VC with counter 0. REQ has no credit counter; the model-facing spill register accepts the C++ strobe only with input capacity, then holds it until the RTL-side `valid && ready` handshake. Verified: `TEST(NmuDatCreditConservation, BackpressureStallsAtSeedThenReopens)`, `TEST(RouterWrap, DatLocalCreditReturnReplenishesRouter)`, and `test_nmu_req_model_strobe_holds_until_rtl_handshake` (`sim/tools/test_model_egress_hold.py`). Failure: a 9th un-credited DAT flit, a DAT stall with credit available, or a REQ flit dropped/changed under backpressure.
27. Response ingress (current model): the NMU accepts every flit presented on either response face (RSP `ready` is tied true, DAT has no ready wire), demuxes B / R into queues of depth NMU_DEPKT_Q_DEPTH = 16 each, and holds at most one pending flit when the target queue is full. That pending flit blocks all later response flits behind it regardless of channel (single-ingress head-of-line blocking, as built). Verified: `TEST(NmuDepacketize, PendingFlitHolBlockingBFullStallsR)`, `DemuxMixedFlitsByAxiCh`. Failure: a dropped response flit, or R progress past a stalled pending flit. Target DAT ingress instead handshakes with `rx_dat_ready_o` before accepting the flit.
28. Held-valid responses: bvalid / rvalid with all payload fields hold unchanged until the cycle their ready is sampled high, then either the next beat or valid = 0 appears (rule P4). Verified: co-sim (the AXI master BFM samples with randomized ready delays, scoreboard compare) and `TEST(NmuWrap, single_aw_w_two_phase_handshake)`. Failure: valid deasserted or payload changed before ready.
29. DAT response credit return (current model only): one pulse on `rx_dat_crdvalid_o[v]` per `DataR` flit consumed from VC v, at most one pulse per VC per cycle, pending pulses accumulate and drain in later cycles, none is lost. RSP returns no credit — `rx_rsp_ready_o` is tied true (SPEC 27) and the face has no credit pin. Verified: `TEST(NmuDatCreditConservation, ConsumerPulseAccumulatesMultiConsumePerTick)`. Failure: pulse count not equal to consumed-flit count over any window. Target RTL supersedes this rule with `rx_dat_ready_o` as described in Section 3.2.
30. Unloaded latencies: the rule P8 values (AW handshake to model-facing AW flit = 4 cycles, B flit to bvalid = 3 cycles, R flit to rvalid = 2 cycles Disabled / 3 cycles Enabled) are exact in an idle, ready/credit-available NMU. The REQ count includes the verification-only spill register after the existing DPI output register; production RTL does not include this adapter. `TEST(NmuWrap, init_with_config_path_loads_sam_from_yaml)` confirms an AW flit emerges within a bounded window; no ctest pins the exact cycle count. Failure: any deviation from the stated count.

## 5. Block Diagram

```
TESTBENCH (generated tb_top, one per topology)
+---------------------------------------------------------------------------+
|  AXI master BFM (stimulus)          scoreboard (write -> readback compare)|
|      |axi_req_i          ^axi_rsp_o                                       |
|      v                   |                                                |
|  +--------------------- nmu_wrap (DPI: NmuWrap -> Nmu) ----------------+  |
|  |                                                                     |  |
|  |  REQ path (1 flit/cycle out)                                        |  |
|  |  AW/AR -> AxiSlavePort -> SAM -> RegSlice -> Rob -> S1 -> Packetize |  |
|  |    W --------(5 FIFOs x16)--------bypass----^     (1-entry reg/ch)  |  |
|  |                      (parallel) (type 0/1/2) (admission, slot alloc)|  |
|  |                                                     (flit build)    |  |
|  |                                            AW   W   AR  v           |  |
|  |                                          WormholeArbiter(3in,{AW->W})| |
|  |                                                   |                 |  |
|  |                                              VcAllocator -----------+--+--> tx_req_* (r/v)
|  |                                              (VC select+stamp,      |  |--> tx_dat_* (credit)
|  |                                               credit-gated RR drain)|  |<-- tx_dat_crdvalid_i
|  |                                                                     |  |
|  |  Response path                                                      |  |
|  |  B/R <- AxiSlavePort <- S2 <- Rob <------ Depacketize <-------------+--+<-- rx_rsp_* (r/v)
|  |  (held-valid B/R)  (1-entry  (reorder:    (B/R demux, x16 queues,   |  |<-- rx_dat_* (credit)
|  |                     reg)      slots +      1-slot HOL pending)      |  |--> rx_dat_crdvalid_o
|  |                               order lists)                          |  |
|  +---------------------------------------------------------------------+  |
|      egress faces -> router LOCAL input      router LOCAL output -> ingress
+---------------------------------------------------------------------------+
```

S2 note: the B response always crosses Rob and the S2 register. R crosses S2 only when the R RoB is Enabled, the Disabled R path drains Depacketize -> Rob interlock -> AxiSlavePort directly (hence the 2-cycle vs 3-cycle rule P8 values).

## 6. Sample Waveform

Write of Section 2.6 (awlen = 8'h01, DAT_NUM_VC = 1), unloaded, credit available. The burst is data class, so the request flits leave on DAT and the `DataB` returns on RSP. All values are wire values per cycle, sampled at posedge.

```
cycle              1    2    3    4    5    6    7   ...   N   N+1  N+2  N+3  N+4  N+5

awvalid            1    1    0    0    0    0    0
awready            0    1    0    0    0    0    0     <- one-shot: 1 cycle, only after
                        ^                                 awvalid observed (max 1 AW
                        AW handshake                      handshake per 2 cycles)
wvalid             0    0    1    1    0    0    0
wready             0    0    1    1    0    0    0     <- pre-asserted from cycle 3
                             ^    ^                       (2 owed beats), drops after wlast
                             W0   W1(wlast=1)
tx_dat_valid_o     0    0    0    0    1    1    1     <- 3 flits back-to-back
tx_dat_flit_o      0    0    0    0    AW   W0   W1    <- 633'h0 while valid low
                        |--------------|
                        3 cycles: AW handshake -> AW flit (SPEC 30)

                             ... B flit returns from the router on RSP ...

rx_rsp_valid_i                                     1    0    0    0    0    0
rx_rsp_flit_i                                      B    -    -    -    -    -
rx_rsp_ready_o                                     1    1    1    1    1    1
                                                   ^ tied true, the ingress queue is unbounded
bvalid                                             0    0    0    1    1    0
bready                                             0    0    0    0    1    0
bid / bresp                                        0    0    0    3'h5/2'b00 held
                                                   |--------------|    ^
                                                   3 cycles: B flit    B handshake,
                                                   -> bvalid (SPEC 30) bvalid held 2 cycles
                                                                       because bready was
                                                                       low in cycle N+3
```

Reset (not drawn): rst_ni low once before cycle 1, all outputs 0 during it, never re-asserted.

## 7. Appendix

### 7.1 RoB high-water stack allocator walk-through (informative)

The allocator is a high-water slot stack with leading-zero-count free-space derivation: one bitmap bit marks each reserved range's top slot, free space = depth - 1 - (highest set bit index), or depth when the bitmap is empty, and the next base = depth - free space. Mini example with r_rob_depth = 8:

| Step | Action | Bitmap (bit 7..0) | Free space | Result |
|---|---|---|---|---|
| 1 | AR#1, arlen = 3 (4 slots) | 0000_1000 (top = 3) | 8-1-3 = 4 | base 0, slots 0..3 |
| 2 | AR#2, arlen = 1 (2 slots) | 0010_1000 (top = 5) | 8-1-5 = 2 | base 4, slots 4..5 |
| 3 | AR#3, arlen = 3 (4 slots) | unchanged | 2 < 4 | refused, no state change |
| 4 | AR#1 fully released | 0010_0000 | still 2 | space returns only from the top: slots 0..3 are free but below the high-water mark |
| 5 | AR#2 fully released | 0000_0000 | 8 | next base = 0, whole pool reusable |

This trades utilization for allocator simplicity: RTL needs only the bitmap, an lzc, and an adder, no free list. `cmodel_nmu_read_slot_hwm` reports the observed peak occupancy for depth sizing.

Hint: the per-beat R fill (beat i of a burst lands at base+i via a per-base arrival counter) means the R RoB never stores out-of-order beats of one burst at wrong offsets even when the fabric interleaves bursts of different IDs, only the base tag travels in the flit. Implementation freedom beyond the observable behavior of Sections 3 and 4 remains with the reader.

### 7.2 Not guaranteed (informative)

For the integrator: this block does not order responses across different AXI IDs, and does not order a read after a write to the same address issued on a different ID. Both are the upstream master's or system's concern.
