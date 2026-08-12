# Design: Network Master Unit (NMU)

The NMU is the master-side network interface of the NoC. It sits between an external AXI4 master and the router mesh: it converts AXI4 request beats (AW / W / AR) into NoC flits, and converts response flits (B / R) back into AXI4 response beats. This document specifies the as-built C++ behavior model (`src/c_model/include/nmu/`), its wrap (`src/c_model/include/wrap/nmu_wrap.hpp`), and the SV DPI module (`src/sv/nmu_wrap.sv`). An RTL implementation of this block is verified cycle-by-cycle against this model by the existing co-sim testbench.

## 2. Design Description

### 2.1 Packetization

The NoC fabric moves fixed-size flits, not AXI beats. Packetization is a 1-to-1 mapping: every accepted AXI request beat becomes exactly one flit, and every response flit becomes exactly one AXI response beat. A flit is a 44-bit header plus a payload sized per network (REQ 132 b, RSP 122 b, DAT 629 b total). The header carries routing and ordering metadata (source node, destination node, virtual channel, wormhole packet boundary, reorder-buffer tag, collective op and mask). The payload carries the AXI channel fields verbatim.

A write transaction of AWLEN+1 beats therefore becomes 1 AW flit followed by AWLEN+1 W flits. A read request becomes 1 AR flit. The write's flits form one wormhole packet (the AW flit opens it, the W flit with `wlast=1` closes it) so no other request flit from this NMU can interleave between an AW and its W beats on the link. AW+W is the only multi-flit packet the fabric builds, and AXI4 IHI 0022 A5.3.3 is why: W beats of different transactions may not interleave, so the AW and its burst have to travel as one indivisible unit. A read request and every response are single-flit packets, R beats included.

The NMU has three flit faces, one per physical network (Section 3.1):

- REQ egress (NMU produces): `NarrowAw` / `NarrowW` / `NarrowAr` plus `DataAr` — no AR rides DAT. Ready/valid.
- RSP ingress (NMU consumes): `NarrowB` / `DataB` plus `NarrowR`. Ready/valid.
- DAT egress + ingress: `DataAw` / `DataW` out, `DataR` in. Credit flow control both directions, no ready wire. DAT is the only face carrying more than one VC.

### 2.2 Flit format

Source of truth: `specgen/generated/cpp/ni_flit_constants.h` (generated from `specgen/generated/json/ni_packet.json`, drift-gated at build). One 44-bit header layout, three flit widths, one per network:

| Network | `FLIT_WIDTH` | Payload region | Channels carried |
|---|---|---|---|
| REQ | 132 | `flit[131:44]`, 88 b | `NarrowAw`, `NarrowW`, `NarrowAr`, `DataAr` |
| RSP | 122 | `flit[121:44]`, 78 b | `NarrowB`, `DataB`, `NarrowR` |
| DAT | 629 | `flit[628:44]`, 585 b | `DataAw`, `DataW`, `DataR` |

Each network's width is the widest payload it carries plus the header. Payload field positions below are relative to the payload region: payload bit p is flit bit p+44. A payload shorter than its network's region leaves the upper bits 0.

Header (`flit[43:0]`), identical on all three networks:

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

There is no `rsvd` field: `PADDING_FIELDS_COUNT` = 0, the header is fully assigned.

AW payload (88 bits). AR payload is field-for-field identical with `ar*` names:

| Field | Payload bits | Width | Definition |
|---|---|---|---|
| awid | [2:0] | 3 | AXI ID. |
| awaddr | [50:3] | 48 | The request address, forwarded unchanged (Section 2.3). |
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

Destination derivation is a System Address Map (SAM) range lookup, not a bit-slice decode. The SAM is a list of entries {base, size, dst_id, class} (`nmu::addr_trans::SamTable`). On every shipped topology a node owns one memory-space entry and one config-space entry, and the two are far apart in the system map.

**INPUT** wire address A. **COMPUTE** scan entries in list order, first entry with base <= A < base+size wins. **OUTPUT** {dst_id, class, A}.

The address is forwarded unchanged. There is one address domain end to end: what the master issued is what the flit carries, what the destination NSU emits, and what that tile's decoder matches. The destination's two spaces are told apart by the bases the map already gave them, so no second decode is needed anywhere.

That is also what upstream does. `floo_id_translation` turns an address into a node id and nothing else, and a search for `addr_decode` across FlooNoC's `hw/` finds it at exactly two sites, the source NI and the router — there is no endpoint-local decoder. A tile-local rebase existed here briefly and was removed: it created a second address domain, which a tile cannot have once its own initiator and its NI share one decoder.

The SAM is loaded at runtime from the topology YAML `address_map` block (`sim/topologies/*.yaml`, parsed by `nmu/sam_yaml.hpp`). There is no built-in default map: `NmuWrap::init` rejects a null or empty `config_path` with `std::invalid_argument`.

Example (`mesh_4x4_vc1`): A = 0x60_0080. Matching entry: raster index 6, base = 6 * 0x100000 = 0x60_0000, size 0x100000, dst_id = 8'h12 (x=2, y=1), memory space. Result: dst_id = 8'h12, local_addr = 0x100000 + 0x80 = 0x10_0080. Non-matching entry for contrast: the dst_id = 8'h11 entry covers [0x50_0000, 0x60_0000), which excludes A because A >= its base+size.

Both windows of one node, on `mesh_2x2_vc1`: A = 0x1000 hits node (0,0)'s memory entry [0x0, 0x100000) and translates to local_addr = 0x100000 + 0x1000 = 0x101000, above the config window. A = 0x400010 hits the same node's config entry at base 0x400000 and translates to local_addr = 0x0 + 0x10 = 0x10.

A SAM miss (address covered by no entry) cannot happen (Section 3.5, guarantee G1). The model aborts if violated. There is no DECERR generation in this block.

### 2.4 Request pipeline

```
AXI master -> AxiSlavePort -> Rob -> S1 (NmuReqS1Bridge) -> Packetize
           -> WormholeArbiter(3 in: AW, W, AR, pairing {AW->W})
           -> VcAllocator -> REQ egress (ready/valid) | DAT egress (credit-gated)
```

- `AxiSlavePort`: per-channel FIFOs (depth `NMU_QUEUE_DEPTH` = 16 each for AW / W / AR / B / R). Pure transport, FIFO order per channel regardless of AXI ID.
- `Rob`: request admission gate and response reorder buffer (Section 2.5). Stamps ordering_req / ordering_tag metadata and translates the address through its SAM.
- S1 (`NmuReqS1Bridge`): one 1-entry pipeline register per channel (AW, W, AR), drained independently so a full AW register never blocks the W stream that must release the wormhole lock.
- `Packetize`: builds the flit (Section 2.2). W flits inherit dst_id / ordering_req / ordering_tag from a write-metadata FIFO (`w_meta_fifo_`), pushed per AW flit, popped on the wlast W flit. The model queue is unbounded, its occupancy is bounded by the number of accepted AW bursts whose wlast W flit has not yet been emitted.
- `WormholeArbiter`: 3-input round-robin arbiter with the AW->W lock, 1 flit per cycle, per-input pending depth `NMU_ARBITER_FIFO_DEPTH` = 4. Draining an AW flit (header.flit_tail=0) locks the arbiter to the W input until the W flit with header.flit_tail=1 drains. AR flits (flit_tail=1) never lock.
- `VcAllocator`: assigns vc_id and stamps it into the header, per-VC pending queue depth `NMU_ARBITER_FIFO_DEPTH` = 4, drains at most 1 flit per cycle to the NoC, gated on per-VC sender credit.

VC candidate set: every VC in {0 .. NUM_VC-1}, with no read/write class split. Only the DAT face carries NUM_VC > 1 (REQ and RSP are single-VC), so the candidate set is the DAT VC set and `DataAw` / `DataW` are its only request-side traffic. Legal NUM_VC values are 1 to 8. The working co-sim configuration set is {1, 2, 4, 8}.

Fixed VC stamping: header `fixed_vc` = 1 on every ordering_req = 0 AW of either class (the whole same-(dst_id, awid) streak, first flit included) and on the W beats that copy it from their owning AW; ordering_req = 1 AW and AR leave it 0. The rule keys on channel kind and ordering_req, not face -- a `NarrowAw` with ordering_req = 0 rides the single-VC REQ face and still carries fixed_vc = 1, vacuously true there. Downstream routers keep `vc_id` unchanged only where fixed_vc = 1 instead of restamping at VA (docs/router-spec.md R12/SPEC6).

Per-cycle evaluation order inside the model (`Nmu::tick`, this order is the spec): WormholeArbiter, VcAllocator, S1-to-Packetize, AxiSlavePort request forward, then the response drains (S2 stages, B always, R per mode), Depacketize last. Consequences that are cycle-visible: a flit entering the WormholeArbiter and the VcAllocator traverses both in the same cycle (wormhole runs first), and an AXI ID freed by this cycle's B / R response is not yet usable by this cycle's request side (request side runs first).

### 2.5 Response path and the reorder buffer

```
response faces -> Depacketize -> Rob -> S2 (1-entry register per channel) -> AxiSlavePort -> AXI master
```

Why a reorder buffer exists: AXI4 requires that responses with the same ID return in issue order. The fabric does not guarantee this. Two same-ID reads to different destinations can return out of order (a near slave answers before a far one), and with multiple VCs even same-destination traffic could overtake if it changed VC mid-stream. The NMU owns same-ID response ordering: it either proves a request cannot be overtaken (bypass) or reserves reorder storage for it before it enters the network.

Admission decision per AW / AR, evaluated per AXI ID (`Rob::push_aw` / `push_ar`):

**IMPORTANT** (unique classification, exactly one branch applies, in this priority):
1. Idle-ID bypass: the ID's order list is empty (nothing in flight for this ID). No slot, ordering_req=0, and the sticky flag resets (fresh streak).
2. Same-destination bypass: the ID is not sticky-fallen-back AND dst_id equals the previous accepted push's dst_id for this ID and direction. No slot, ordering_req=0.
3. Fall-back: allocate slots, ordering_req=1, and set the sticky flag. Once sticky, every later push of this ID allocates until the ID goes idle again (branch 1 is the only reset).

A collective AW (Section 2.8) is admitted only through branch 1, and while one is in flight nothing else for that ID is admitted at all.

Example, ID = 3'h3, write direction: AW#1 dst 8'h02 (list empty, branch 1, bypass). AW#2 dst 8'h02 (same dest, branch 2, bypass). AW#3 dst 8'h05 (dest changed, branch 3, allocate, sticky). AW#4 dst 8'h05 (same dest as #3 but sticky, branch 3, allocate). All four complete and the list empties. AW#5 dst 8'h05 (branch 1 again, bypass). A counterexample for branch 2: AW#2 with dst 8'h07 would take branch 3, because 8'h07 != 8'h02.

Slot pools, per direction: B pool depth `NMU_ROB_B_DEPTH` = 128, R pool depth `NMU_ROB_R_DEPTH` = 128. An AW reserves 1 slot (B is one beat). An AR reserves ARLEN+1 consecutive slots (one per R beat), refused when free space is short. The allocator is a high-water stack: one allocation bit marks each reserved range's top slot, free space is the slot count above the highest set bit (leading-zero-count in RTL), the next base is depth minus free space, and space returns only from the top (Appendix 7.1 walks it with numbers). ordering_tag stamps the base slot. On the response side, B fills its slot, the i-th R beat of a burst fills base+i, and beats release to the master only while the ID's oldest outstanding transaction is being served (per-ID issue order, one order list per ID per direction).

`RobMode` selects the R side only. `RobMode::Enabled`: R responses use the slot pool as above. `RobMode::Disabled` (co-sim default): the R RoB is off and same-ID read ordering is enforced by a per-ID single-outstanding interlock, a second AR for an ID stalls until the first read's rlast returns. The B-side RoB always runs in both modes, writes never use an interlock. Wherever this document says "RobMode", it governs reads only.

Per-ID transaction gate, both modes' write side and Enabled reads: at most `NMU_MAX_TXNS_PER_ID` = 32 outstanding transactions per ID per direction (order-list depth). A burst is one entry regardless of ARLEN. An entry is taken when the request is accepted and released when the response is accepted at the AXI side, B on its single beat and R on rlast. The 33rd same-ID AW is refused until one completes, even with free slots, which backpressures through the AxiSlavePort queue to awready / arready.

There is no aggregate pool above the per-ID gate, so the order list is the admission bound and in-flight requests cap at `NMU_MAX_TXNS_PER_ID` x 2^`AXI_ID_WIDTH` = 32 x 8 = 256 writes per direction, and at 1 x 8 = 8 reads under `RobMode::Disabled`, where the per-ID single-outstanding interlock replaces the order-list depth. Two limiters coexist on the write side and on Enabled reads, the per-ID order-list depth and the RoB slot pool, and which one binds depends on the traffic: a bypassed transaction takes a list entry and reserves no slot, so a stream that stays in branches 1 and 2 meets only the per-ID gate.

### 2.6 Worked example: 2-beat write burst

NMU at node (x=0, y=0), src_id = 8'h00, `mesh_4x4_vc1` SAM, DAT_NUM_VC = 1. The master issues AW {awid = 3'h5, awaddr = 0x60_0080, awlen = 8'h01, awsize = 3'h6, awburst = 2'b01} then W0 {wdata = 512'h...11, wstrb = 64'hFFFF_FFFF_FFFF_FFFF, wlast = 0} and W1 {wdata = 512'h...22, wstrb = 64'hFFFF_FFFF_FFFF_FFFF, wlast = 1}.

SAM: 0x60_0080 hits the dst_id = 8'h12 entry, memory space, local_addr = 0x10_0080. ID 5 is idle, so admission takes the idle-ID bypass: ordering_req = 0, ordering_tag = 0. The burst is data class, so the three flits leave on the DAT face, in this exact order, as one wormhole packet:

| Field | AW flit | W flit 0 | W flit 1 |
|---|---|---|---|
| header.axi_ch | 4'd5 (DataAw) | 4'd6 (DataW) | 4'd6 |
| header.src_id | 8'h00 | 8'h00 | 8'h00 |
| header.dst_id | 8'h12 | 8'h12 (inherited from AW) | 8'h12 |
| header.fixed_vc / vc_id | 1 / 3'd0 | 1 / 3'd0 (follows the AW's VC) | 1 / 3'd0 |
| header.flit_tail | 1'b0 (opens packet) | 1'b0 | 1'b1 (closes packet) |
| header.ordering_req / ordering_tag | 0 / 8'h00 | 0 / 8'h00 | 0 / 8'h00 |
| header.collective_op / collective_mask | 0 / 8'h00 | 0 / 8'h00 | 0 / 8'h00 |
| payload | awid=3'h5, awaddr=48'h10_0080, awlen=8'h01, awsize=3'h6, awburst=2'b01, others 0 | wlast=0, wstrb=64'hFFFF_FFFF_FFFF_FFFF, wdata=512'h...11, wuser=0 | wlast=1, wstrb=64'hFFFF_FFFF_FFFF_FFFF, wdata=512'h...22 |

Later one B flit returns on the RSP face {axi_ch = 4'd8 (DataB), bid = 3'h5, bresp = 2'b00} and the NMU presents bvalid / bid = 3'h5 / bresp = 2'b00, held until bready.

### 2.7 Parameters

Single source `specgen/source/constants.yaml`, generated into `ni_params.h` and `ni_params_pkg.sv` (drift-gated by `codegen.py --check`). Defaults below are the shipped values.

| Parameter | Default | Legal range | Consumed by |
|---|---|---|---|
| AXI_ID_WIDTH | 3 | 1..32 (implementation locked at 3) | ID fields, RoB per-ID arrays (8 IDs) |
| AXI_ADDR_WIDTH | 48 | 1..64 | Address fields |
| AXI_DATA_WIDTH | 512 | {32,64,128,256,512,1024} | wdata / rdata, WSTRB_WIDTH = 64 |
| AXI_AWUSER_WIDTH | 58 | 10..64 | AWUSER slave-port field and the DPI unpack mask: 8 b user + 2 b collective_op + 48 b collective address mask (Section 2.8) |
| NOC_DAT_NUM_VC | 1 | 1 to 8 | `VcAllocator`, DAT credit vectors |
| NOC_REQ_FLIT_WIDTH | 132 | 64..1024 | REQ egress flit port |
| NOC_RSP_FLIT_WIDTH | 122 | 64..1024 | RSP ingress flit port |
| NOC_DAT_FLIT_WIDTH | 629 | 64..1024 | DAT flit ports, both directions |
| NOC_ROUTER_VC_DEPTH | 8 | 1..16 | DAT request sender credit seed per VC |
| NMU_ROB_B_DEPTH | 128 | 1..256 | B slot pool |
| NMU_ROB_R_DEPTH | 128 | 1..256 | R slot pool |
| NMU_MAX_TXNS_PER_ID | 32 | 1..256 | Per-ID order-list depth |
| NMU_QUEUE_DEPTH | 16 | 1..1024 | AxiSlavePort AW/W/AR/B/R FIFOs |
| NMU_DEPKT_Q_DEPTH | 16 | 1..1024 | Depacketize B/R queues |
| NMU_ARBITER_FIFO_DEPTH | 4 | 1..64 | Wormhole per-input and VC pending queues |

Runtime configuration per instance: src_id, SAM config path, RobMode and RoB depth overrides come through `cmodel_nmu_create` / `cmodel_nmu_create_ex` (Section 3.3). The generated testbench sets src_id = {y[3:0], x[3:0]} per node and forwards the plusargs `+sam_config=`, `+b_rob_depth=`, `+r_rob_depth=`, `+max_txns_per_id=`.

### 2.8 Collective writes

A collective write is one AW+W burst the master issues once and the fabric replicates to an aligned submesh, answered by one merged `B`. The master expresses it on AWUSER, `AXI_AWUSER_WIDTH` = 58 bits wide:

| AWUSER bits | Field | Meaning |
|---|---|---|
| [7:0] | user | opaque, carried in the flit payload |
| [9:8] | collective_op | 2'd0 UNICAST, 2'd1 MULTICAST, 2'd2-3 reserved |
| [57:10] | collective address mask | wildcard bits over the 48-bit address |

Reads have no collective surface: ARUSER is 8 bits, so every AR leaves with `collective_op` = UNICAST and a zero mask. Both AXI classes multicast. Narrow-class collectives are the config-space replication case, where one config write reaches every tile of a submesh at the same node-local offset, and they fork on the REQ network; Data-class collectives fork on DAT.

**Translate.** A set address-mask bit is a don't-care, so a mask with n set bits names 2^n replica addresses. The SAM is a first-match range lookup with no stored node-index bit offset, so the translate enumerates rather than bit-selects (`addr_trans::collective_translate`):

**INPUT** AWUSER, AWADDR, AWLEN / AWSIZE / AWBURST. **COMPUTE** enumerate all 2^n masked addresses and translate each through the SAM. Every replica must land in a tile, carry the same node-local offset, carry the same class, name a distinct `dst_id`, and pass the burst-footprint check for its own aperture. **OUTPUT** an 8-bit node mask, the OR of each replica's `dst_id` against the anchor's, stamped into the AW header beside `collective_op`. `dst_id` stays the anchor's. The W metadata FIFO latches both fields per AW and stamps every W beat of the burst, so the fabric forks the W beats to the AW's exact branch set.

n is capped at X_WIDTH + Y_WIDTH = 8, so the enumeration is at most 256 SAM lookups. It re-runs on every backpressure retry of the same AW. That is a model-only cost.

**Reject set.** These are permanent illegal inputs, not retryable backpressure, so each is a fatal assert and abort, the convention this block already uses for inputs that never clear on retry:

| Condition | Why |
|---|---|
| `collective_op` = UNICAST with a nonzero mask | mask without op contradicts the spec |
| `collective_op` = MULTICAST with a zero mask | empty destination set. Not downgraded to unicast: the op is explicit here, so a mismatch is a stimulus contradiction |
| reserved `collective_op` (2'd2, 2'd3) | undefined encoding |
| AWLOCK set on a collective | AxLOCK is unicast only |
| more mask bits set than a node id has | destination set larger than the mesh |
| an anchor or replica address maps to no tile | the set leaves the mesh |
| replicas disagree on the node-local offset | one aligned region per node is what makes the replicas addressable |
| replicas straddle the narrow and data classes | the class picks the network the worm forks on, and a packet rides one |
| the mask names one node twice | the set is not a wildcard over `dst_id` |
| a replica burst overruns its aperture | same footprint rule as a unicast burst |
| AWUSER bits above [57] set | they would be silently dropped by the field accessors |
| a collective on the direct `Packetize::push_aw` interface | that path bypasses `Rob`, which owns the validate and translate |

**R2 admission.** At most one collective is outstanding per (NMU, AXI ID). Two gates at `Rob::push_aw` entry, both returning retryable backpressure rather than an error:

1. An incoming collective is admitted only when the ID's write order list is empty. It then takes the idle-ID bypass: ordering_req = 0, no RoB slot, in both `RobMode` settings.
2. While the front entry of an ID's order list is collective, nothing for that ID is admitted. Testing the front is enough, because a collective only ever enters an empty list and is therefore the only entry.

Gate 2 is what closes the same-destination bypass: without it a later same-ID AW whose destination equals the collective's anchor would stream past it with no slot and no ordering. The collective still takes one entry of the ID's write order list like any AW, released when its merged `B` retires.

**Merged B.** The merged `B` returns through the ordinary B ingress and releases the interlock exactly like a unicast `B`: the NMU stamps ordering_tag before fanout and the fabric's merge preserves it, so the response path needs no collective state. The `B` carries `collective_op` = MULTICAST and the echoed node mask; neither perturbs the `bid` / `bresp` decode.

Exactly one `B` per collective AW is a structural invariant of the merge, and two independent checks catch a violation. The order-list head check in `pop_b_staged` fires first on both callers: the first `B` pops the ID's only order-list entry, so a duplicate finds an empty list and aborts on "bypassed B does not match the head of its id's order list". The `write_txns_ > 0` underflow assert in `retire_b` is the backstop behind it.

## 3. Inputs and Outputs

All ports below are the real `nmu_wrap` ports (`src/sv/nmu_wrap.sv:54-77`). AXI signals are fields of the packed structs `ni_signals_pkg::axi_req_t` / `axi_rsp_t`, plus a dedicated `awuser_i` port (the generated `axi_req_t` has no awuser field). The three NoC faces are scalar signals, not structs: REQ and RSP are ready/valid, DAT is credit in both directions. The `axi_req_t` struct carries `awregion` / `arregion`, but they are not in the DPI signature: the model sees region = 0 and stamps 0 into the flit. No AXI user signals other than `awuser_i` exist on the wire face, the flit's user fields are stamped 0 and response user fields are dropped.

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
| rx_rsp_flit_i | 122 | RSP flit, format of Section 2.2. axi_ch must be `NarrowB`, `DataB` or `NarrowR` (guarantee G2). Unknown when valid is low. |
| rx_dat_valid_i | 1 | A DAT flit is on the wire this cycle. |
| rx_dat_flit_i | 629 | DAT flit. axi_ch must be `DataR` (guarantee G2). Unknown when valid is low. |
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
| tx_req_valid_o | 1 | REQ request flit on the wire this cycle. At most one flit per cycle, not held. |
| tx_req_flit_o | 132 | REQ request flit, format of Section 2.2. 0 when valid is low. |
| rx_rsp_ready_o | 1 | RSP ingress ready. Tied true: the model's ingress queue is unbounded (`nmu_wrap.hpp`). |
| tx_dat_valid_o | 1 | DAT request flit on the wire this cycle. At most one flit per cycle. Not held: the flit is consumed by the credit protocol, there is no ready wire. |
| tx_dat_flit_o | 629 | DAT request flit. 0 when valid is low. |
| rx_dat_crdvalid_o | DAT_NUM_VC | Per-VC one-cycle consumer credit pulse: bit v high means the NMU consumed one DAT response flit from VC v. At most one pulse per VC per cycle (pending consumptions queue up and drain one per cycle). |

### 3.3 DPI functions

The SV wrap holds no behavior. Each posedge it runs the 3-call discipline: `cmodel_nmu_set_inputs` (latch wires, no state change), `cmodel_nmu_tick` (advance the model one cycle), `cmodel_nmu_get_outputs` (copy the output latch), then registers the outputs nonblocking. Declared in `src/dpi/cmodel_dpi.h:151-179`.

| Function | Signature (summary) | Semantics |
|---|---|---|
| cmodel_nmu_create | `unsigned long long (const char* name, int src_id, int dat_num_vc, const char* config_path)` | Constructs the instance, RobMode::Disabled (R side), default depths. `dat_num_vc` sizes the DAT face only; REQ and RSP are fixed single-VC. `config_path` is required: NULL or empty throws inside `NmuWrap::init`, which the DPI boundary catches into the error latch and returns handle 0. Returns the 64-bit handle for ctx_i. |
| cmodel_nmu_create_ex | `unsigned long long (const char* name, int src_id, int dat_num_vc, int rob_enabled, int b_rob_depth, int r_rob_depth, int max_txns_per_id, const char* config_path)` | As create, plus R-RoB enable and depth overrides. The generated testbench calls this in both RoB modes: it is the only entry point carrying the overrides, and the B-side RoB runs whatever `rob_enabled` says (Section 2.5). |
| cmodel_nmu_set_inputs | `(ctx, AXI args incl. a 58 b awuser, then the three NoC faces: tx_req_ready, rx_rsp_valid + flit, rx_dat_valid + flit, tx_dat_crdvalid)` | Latches inputs only. Packing: 8-bit fields in word[0] low byte, addresses 2 words little-endian, data 16 words little-endian, wstrb 2 words, awuser 2 words, flits little-endian at their own network's word count (REQ 5, RSP 4, DAT 20), credit vector 1 word bit-per-VC. |
| cmodel_nmu_tick | `(ctx)` | One full model cycle. One call = one clock edge. |
| cmodel_nmu_get_outputs | `(ctx, AXI response args, tx_req_valid + flit, rx_rsp_ready, tx_dat_valid + flit, rx_dat_crdvalid)` | Copies the output latch. bresp / rresp masked with 2'b11. |
| cmodel_nmu_read_slot_hwm | `unsigned int (ctx)` | Statistic: peak R-RoB slot occupancy. 0 when the handle is invalid or the R RoB is Disabled. Printed per node at testbench exit. |
| cmodel_nmu_admission_stats | `void (ctx, out aw_idle_bypass, aw_same_dest_bypass, aw_fallback_alloc, ar_idle_bypass, ar_same_dest_bypass, ar_fallback_alloc, order_list_hwm, write_txns_hwm, read_txns_hwm)` | Statistics: the SPEC 17 admission clause counts over accepted pushes (AW and AR counted separately, AR only in RobMode::Enabled), the deepest per-ID order list and the peak in-flight transaction count per direction. All outputs 0 when the handle is invalid. Printed per node at testbench exit on the same `[HWM]` line. |

An invalid handle raises a categorized model error which the testbench error poll turns into `$fatal`.

### 3.4 Protocol rules

1. Input rhythm: the AXI face is handshake-paced, there is no fixed delivery rhythm. Within one write, exactly awlen+1 W beats follow their AW in issue order and bursts never interleave W beats (AXI4 has no WID). Example: AW with awlen = 8'h01 is followed by 2 W beats, the second with wlast = 1. Each response face presents at most one flit per cycle. Credit vectors may pulse any subset of bits in any cycle.
2. Idle state: input payload buses are don't-care while their valid is low, the model reads them only on the handshake cycle (valid and ready both high) and reads a response flit only while that face's valid is high. All output payload buses are driven to 0 while their valid is low (example: `tx_dat_flit_o` = 629'h0 whenever `tx_dat_valid_o` = 0).
3. Sampling edge: all inputs are sampled at the positive edge of clk_i. One posedge = one model cycle via the 3-call DPI discipline of Section 3.3, and the outputs computed from cycle-N inputs appear on the wires during cycle N+1 (registered outputs). The verification pattern captures outputs at the positive edge.
4. Valid behavior: bvalid and rvalid, once asserted, stay asserted with stable payload until the cycle their ready is high (AXI4 IHI 0022 A3.2.1). `tx_req_valid_o` and `tx_dat_valid_o` are per-flit strobes, high only on cycles that carry a flit, at most one flit per cycle. `tx_dat_valid_o` never waits on a ready: the per-VC credit counter guarantees the receiver has buffer space. `tx_req_ready_i` is advisory, not a same-cycle accept — the NMU grants against a ready sampled two registrations earlier and the receiver pushes unconditionally on valid, so the transfer is `valid` alone.
5. Output idle value: bid, bresp, rid, rdata, rresp, rlast are 0 when their valid is low. `tx_req_flit_o` and `tx_dat_flit_o` are 0 when their valid is low. Credit outputs are 0 in non-pulse cycles.
6. Reset: rst_ni is given only once, at the beginning of simulation, synchronous active-low. While rst_ni is low all outputs are 0 (the wrap clears its output registers, the model starts reset by construction). There is no mid-run reset.
7. Handshake gaps: awready and arready are one-shot, they assert only after their valid is observed and drop the cycle after the handshake, so consecutive same-channel address handshakes are at least 2 cycles apart (handshakes on cycles 2, 4, 6 of the Section 6 waveform pattern). AW acceptance is not gated on the previous write's W burst finishing (multi-outstanding AW is supported and w-owed counts accumulate). wready is pre-asserted while owed W beats remain and FIFO space exists, so W beats can stream back-to-back, 1 per cycle. An incoming request credit pulse is usable in the same model cycle (replenished before the tick). At most one response credit pulse per VC per cycle is emitted.
8. Latency: measured from the positive edge ending the input's handshake cycle to the first cycle the corresponding output valid is high, in an otherwise idle NMU with credit available. AW handshake to its AW flit on the egress face: exactly 3 cycles. Response B flit to bvalid: exactly 3 cycles. Response R flit to rvalid: exactly 2 cycles with the R RoB Disabled, exactly 3 cycles with it Enabled. Under load these are lower bounds, backpressure adds cycles without an upper bound.

### 3.5 Input guarantees

The implementation does not handle the following, they are guaranteed not to happen. The model asserts and aborts on each (assert locations in parentheses).

| # | Guarantee | Reason / model check |
|---|---|---|
| G1 | Every issued address hits a SAM entry. | The system address map covers all issued addresses. Miss aborts (`SamTable::translate`). No DECERR path exists. |
| G2 | Only B or R flits arrive on the response faces. | Fabric routes request and response classes on disjoint networks. Other axi_ch aborts (`Depacketize::drain_ingress_`). |
| G3 | No burst crosses a SAM region boundary. | Regions are 4 KiB aligned and sized and AXI4 forbids 4 KiB crossings, so the upstream master never issues one. The model asserts this only on the direct Packetize path (`Packetize::push_aw` / `push_ar`). |
| G4 | The SAM itself is well-formed: nonzero sizes, 4 KiB aligned base and size, no overlap, dst inside the mesh. | Checked once at load (`SamTable::validate`). |
| G5 | valid, once asserted, holds with stable payload until ready (both directions of the AXI face). | AXI4 A3.2.1. The one-shot ready policy depends on it: ready asserts one cycle after valid is first seen. |
| G6 | W beat counts match their AWs (exactly awlen+1 beats, wlast on the final beat, no spurious W). | AXI4 legality is the master's job. The owed-W counter floors at 0 and does not reject an unexpected W. |
| G7 | On DAT, the router never pulses more request credits than flits it drained, and never presents a response flit without holding a credit for it. | Credit conservation: per VC, sender credit + in-flight flits = seed (`NOC_ROUTER_VC_DEPTH` = 8). A credit lie downstream of the `VcAllocator` aborts (`VcAllocator::tick`). |
| G8 | awburst / arburst = 2'b11 never occurs, and header.axi_ch values 4'd10 to 4'd15 never occur. | Reserved encodings. |
| G9 | DAT_NUM_VC is 1 to 8 and the elaborated `noc_credit_t` width equals DAT_NUM_VC. | Out-of-range DAT_NUM_VC aborts at `VcAllocator` construction, width mismatch is `$fatal` at elaboration (`nmu_wrap.sv`). |

## 4. Specifications

Each item names its verification and the failure condition. "ctest" items run in the pure C++ suite (`src/c_model/tests/`), "co-sim" items run under the generated testbench (`make -C sim TB=<topology> PATTERN=<pattern>`, regression via `sim/run_regress.py`) where correctness is judged by the scoreboard's per-transaction write-to-readback compare plus the model's internal asserts (any assert abort fails the run).

1. Interface: the block implements exactly the Section 3.1 / 3.2 ports of `nmu_wrap` with the given widths (REQ 132 b, RSP 122 b, DAT 629 b flits; DAT_NUM_VC-wide credit vectors). Verified: co-sim elaboration (AXI struct port binding, scalar NoC-face binding, `$fatal` width guard in `nmu_wrap.sv`). Failure: elaboration error or width-guard fatal.
2. Reset: after the single initial rst_ni assertion, every output is 0, and outputs stay 0 until traffic (rule P6). Verified: `TEST(NmuWrap, idle_adapter_keeps_readys_low)` (`src/c_model/tests/wrap/test_nmu_wrap.cpp`) and cycle-1 sampling in co-sim. Failure: any nonzero output during or immediately after reset.
3. Registered outputs: outputs are functions of state as of the previous posedge, never combinational paths from same-cycle inputs (rule P3). Verified: `TEST(NmuWrap, single_aw_w_two_phase_handshake)`, which requires the 1-cycle valid-to-ready offset. Failure: same-cycle input-to-output dependence changes the handshake cycle count.
4. One-shot address ready: awready (arready) asserts only when awvalid (arvalid) was high the previous sampled cycle with FIFO space, stays high 1 cycle, and is low the cycle after a handshake. Consequence: at most one address handshake per 2 cycles per channel. Example: awvalid from cycle 1 gives awready only in cycle 2, and a second AW held from cycle 3 handshakes in cycle 4. Verified: `TEST(NmuWrap, single_aw_w_two_phase_handshake)`, `TEST(NmuWrap, multi_beat_w_burst_full_rate_aw_available)`. Failure: ready asserted before valid observed, held more than 1 cycle, or back-to-back address handshakes.
5. Pre-asserted wready: wready = (owed W beats > 0) AND W FIFO space, where the owed count increases by awlen+1 per accepted AW and decreases by 1 per accepted W beat, accumulating across outstanding AWs. Example: two accepted AWs with awlen = 8'h01 and 8'h00 give owed = 3, wready holds through 3 streaming W beats then drops. Verified: `TEST(NmuWrap, multi_beat_w_burst_full_rate_aw_available)`. Failure: wready high with no owed beats, or W throughput below 1 beat per cycle with space available.
6. Acceptance atomicity: a beat is consumed exactly on its valid-and-ready cycle, and ready is never asserted without guaranteed FIFO space, so an accepted beat is never dropped or duplicated. Verified: `TEST(NmuAxiSlavePort, AwBoundary_FailedPushDoesNotDuplicateOnRetry)` and co-sim scoreboard compare. Failure: lost or duplicated beat (scoreboard miscompare).
7. Per-channel FIFO order: beats of one channel travel in acceptance order regardless of AXI ID, through every request stage. Verified: `TEST(NmuAxiSlavePort, AwFifoOrder_PreservedAcrossMixedIds)`, `ArFifoOrder_PreservedAcrossMixedIds`. Failure: any same-channel reorder.
8. Flit format: emitted flits match Section 2.2 bit-exactly, every header field assigned (the header has no `rsvd`, `PADDING_FIELDS_COUNT` = 0), unused payload bits above the channel's width 0. Verified: `TEST(NmuPacketize, PushAwEmitsFlitWithCorrectFields)` for the header fields, `AwPayloadBitPerfect`, `WPayloadBitPerfect`, `ArEncodesAxiChAndOrderingTag` for the payloads (`src/c_model/tests/nmu/test_packetize.cpp`). Failure: any mismatched bit. Note: `RsvdAndDisabledFieldsZero` is NOT evidence here — it calls `check_padding_is_zero()`, which is vacuously true once the header has no padding fields.
9. One beat, one flit, address forwarded: each accepted AW / W / AR beat emits exactly one flit, and the address payload carries the request address as it arrived. awregion / arregion and all user fields are carried in the flit and are 0 at the co-sim boundary. Verified: `TEST(AddrTrans, TileBaseStaysInTheForwardedAddress)`, `TEST(SamTable, PackedTranslateForwardsTheAddressUnchanged)`. Failure: an altered address in the payload, or wrong dst_id.
10. header.flit_tail stamping: AW = 0, W = wlast, AR = 1. Verified: `TEST(NmuPacketize, WHeaderFlitTailMatchesWlast)`, malformed stamping aborts in the wormhole arbiter (`WormholeArbiter::tick` defensive guards). Failure: assert abort or a wormhole packet that never closes.
11. AW before W: a W flit never enters the network before its AW flit. The RoB refuses W beats while no AW-accepted burst owes beats, and W flits inherit dst_id / ordering_req / ordering_tag from the AW-ordered metadata FIFO. Verified: `TEST(NmuRob, Disabled_WCreditBlocksWBeforeAw)`, `TEST(NmuPacketize, WMetaFifoInheritsAwDst)`, `TEST(NmuReqBridge, PushWBackpressuresOnEmptyMeta)`. Failure: W flit precedes its AW flit or carries wrong metadata.
12. Wormhole atomicity: after an AW flit drains, only W flits of that burst drain until the header.flit_tail = 1 W flit, AR flits wait. Verified: `TEST(NocWormholeArbiter, ArCannotInterleaveDuringLock)`, `MultiBeatWBurstFlowsAndUnlocks` (`src/c_model/tests/router/test_wormhole_arbiter.cpp`). Failure: any foreign flit between AW and its final W.
13. IMPORTANT wormhole tie-break: when unlocked, inputs (0 = AW, 1 = W, 2 = AR) are scanned round-robin starting at the input after the last drained one, first non-empty input wins, 1 flit per cycle. Example: pointer at 0 with AW and AR both pending drains AW (input 0), locks to W, and after the wlast W flit drains from input 1 the pointer is 2, so AR wins the next free cycle even if a new AW is pending. Verified: `TEST(NocWormholeArbiter, AwTriggersLock)` and the co-sim throughput scenarios. Failure: wrong winner in the tie case.
14. SAM lookup: destination is the first entry (list order) whose [base, base+size) contains the address, output {dst_id, class, addr}. Verified: `TEST(SamTable, PackedTranslateForwardsTheAddressUnchanged)`, `TEST(SamTable, TranslateIsInjectiveAcrossSpacesOfOneNode)`, `TEST(SamYaml, SpaceAttributeSelectsClass)` (both spaces of one node). Failure: wrong dst_id, or an address altered on the way through.
15. SAM validation: a loaded SAM is rejected at load if any entry is zero-size, has a base or size that is not 4 KiB aligned, has base + size overflowing 64 b, or names an out-of-mesh dst; if a node appears more than once in one space; if the memory space does not cover every mesh node exactly once; or if any two ranges overlap. Verified: `TEST(SamValidator, RejectsZeroSize)`, `RejectsNon4KBSize`, `RejectsBasePlusSizeOverflow`, `RejectsDstOutsideMesh`, `RejectsDuplicateNode`, `RejectsMissingNode`, `RejectsOverlap` (`src/c_model/tests/nmu/test_sam_table.cpp`). Failure: bad SAM accepted.
16. Same-ID response ordering: B and R beats presented to the master for one AXI ID follow the issue order of their requests, in every mode (B side always via the RoB, R side via slot pool when Enabled, via the single-outstanding interlock when Disabled). Cross-ID order and write-to-read same-address order are NOT guaranteed by this block. Verified: `TEST(NmuRob, Enabled_PopB_OutOfOrder_HeldUntilHeadReady)`, `Enabled_MixedList_OrderPreserved`, `Disabled_StallReleaseOnRlast`, and end-to-end by the co-sim scoreboard compare. Failure: any same-ID response inversion.
17. IMPORTANT admission classification: exactly one of {idle-ID bypass, same-destination bypass, fall-back allocate} applies per AW / AR, in that priority, per the Section 2.5 tree, with the sticky flag reset only by the idle-ID branch. Example: Section 2.5 five-AW trace, AW#4 allocates although its destination matches AW#3. Verified: `TEST(RobSameDestBypass, SameDestStreakBypassesAll)`, `DestChangeTriggersStickyFallback`, `TEST(NmuRob, Enabled_IdleIdBypass_FirstTxnPerIdAllocatesNoSlot)`. Failure: wrong branch taken.
18. Slot reservation: AW takes 1 slot, AR takes arlen+1 consecutive slots, base = pool depth - free space, refused when free space is short, and refusal mutates no state. Free space is the count above the highest allocated range top (high-water stack, Appendix 7.1). Verified: `TEST(NmuRob, Enabled_PushAr_AllocatesConsecutiveSlotsForBurst)`, `Enabled_LzcAllocator_IsAStack`, `Enabled_PushAw_PoolFull_ReturnFalseAtomic`, `Enabled_PushAr_DownstreamBackpressure_AtomicRollback`. Failure: wrong base, overlapping ranges, or state change on refusal.
19. No free slot, no request: an ordering_req = 1 flit never enters the network without its slots already reserved, so a returning tagged response always finds its slot. Verified: `TEST(NmuRobDeath, Enabled_PopBWithUnallocatedOrderingTag_Abort)` (the model aborts on a tag with no slot). Failure: model abort on response arrival.
20. IMPORTANT per-ID release: responses release to the master only from the head of the ID's order list. A ready RoB'd entry behind an incomplete older entry waits, and a bypassed (ordering_req = 0) head is popped by its own response (B, or R with rlast) before anything behind it releases. Example: ID 3 issues bypassed AW#1 then RoB'd AW#2, B#2 arrives first and is held in slot, B#1 arrives and releases, then B#2 releases, master sees B#1 then B#2. Verified: `TEST(NmuRob, Enabled_PopB_OutOfOrder_HeldUntilHeadReady)`, `Enabled_PerBeatRelease_HeadBurstStreams`, `Enabled_BypassedBeat_ReleasesNoSlot`. Failure: release past a blocked head.
21. RobMode scope: `RobMode::Disabled` disables the R-side slot pool only, replacing it with a per-ID single-outstanding read interlock (a second same-ID AR is refused until the first read's rlast). The B-side RoB runs unconditionally in both modes. Verified: `TEST(NmuRob, Disabled_StallReleaseOnRlast)` and B-path tests passing under Disabled construction (`cmodel_nmu_create` default). Failure: a Disabled-mode B bypassing order, or a Disabled-mode second same-ID AR entering the network early.
22. Per-ID transaction gate: at most max_txns_per_id (default 32 = 0x20) outstanding transactions per ID per direction, enforced before slot availability, refusal is stateless. Example: 32 outstanding ID-7 writes refuse the 33rd AW even with 31 free B slots. Verified: `TEST(NmuRob, Enabled_MaxTxnsPerIdGate_RefusesWithFreeSlotsAvailable)`, `Enabled_MaxTxnsPerIdDefaultIsThirtyTwo`. Failure: 33rd same-ID acceptance.
23. VC range: every flit carries vc_id in {0 .. DAT_NUM_VC - 1}, with no per-class restriction — AW, W and AR draw from the same candidate set. With DAT_NUM_VC = 1 all flits carry vc_id = 0, which is also what REQ and RSP always carry. Verified: `TEST(NmuVcAllocator, Degenerate_NumVc1_AllModesPassthrough)`, `TEST(NmuVcAllocatorRoundRobin, DistinctReadIdsSpreadAcrossVcs)`. Failure: a flit outside the configured VC set.
24. IMPORTANT VC selection, per AW / AR flit, first matching rule wins: (a) W flits always take the VC of their AW (no selection). (b) DAT_NUM_VC = 1: VC0. (c) ordering_req = 0 AW whose ID has a recorded destination equal to the flit's dst_id: reuse the recorded VC, and if that VC lacks space or credit the flit stalls, it is never rerouted. (d) otherwise: scan all DAT_NUM_VC VCs round-robin from the selection pointer, first VC with pending space and credit wins, pointer moves past the winner. The (dst_id, VC) record is written on every accepted ordering_req = 0 AW of that ID and persists for the whole run until overwritten by the next such accept, it is never invalidated. AR flits and ordering_req = 1 flits always use rule (d) and never write the record. Example (DAT_NUM_VC = 4, pointer at 0): AW ID 5 dst 8'h12 ordering_req = 0 takes VC0 and records (8'h12, VC0), a second identical AW after the first burst's wlast reuses VC0 even though VC1 is idle, an AW ID 5 dst 8'h07 misses the record and round-robins to VC1. Verified: `TEST(NmuVcAllocatorRoundRobin, SameWriteIdDifferentDestRoundRobins)`, `RobbedFlitsRoundRobinRegardlessOfDest`, `NumVc1SameIdSameDestUnaffected`, `TEST(NmuVcAllocator, WFollowsAW_ReusedFixedVc)`. Header `fixed_vc` follows the same ordering_req = 0 / 1 split (2.4): 1 on every ordering_req = 0 AW of the streak and its W beats, 0 on ordering_req = 1 AW and on AR. Verified: `TEST_P(NmuVcAllocatorParam, FixedVcStampedOnOrderedAwStreak)`, `TEST_P(NmuVcAllocatorParam, FixedVcClearOnRobbedAwAndAr)`. Failure: wrong VC in any listed case, a mid-streak reroute, or wrong fixed_vc bit.
25. IMPORTANT VC drain tie-break: at most 1 flit per cycle leaves for the NoC, chosen by a single global round-robin over all DAT_NUM_VC VCs starting at the pointer, first VC that is non-empty and has sender credit wins, pointer set to winner+1. Example (DAT_NUM_VC = 4, pointer 2, flits pending on VC1 and VC3): VC3 wins (scan 2, 3), pointer becomes 0, VC1 wins next cycle. Verified: `TEST(NmuVcAllocatorRoundRobin, DistinctReadIdsSpreadAcrossVcs)` and co-sim link utilization scenarios. Failure: two flits in one cycle, a starved non-empty credited VC, or wrong winner in the tie case.
26. DAT request credit: the per-VC sender counter is seeded to NOC_ROUTER_VC_DEPTH = 8, decrements per emitted flit, increments per pulse on `tx_dat_crdvalid_i`, and a pulse arriving in cycle N is usable in cycle N. Invariant per VC: credit + un-credited in-flight flits = 8, and the NMU never emits on a VC with counter 0. REQ has no credit counter, it grants against `tx_req_ready_i`. Verified: `TEST(NmuDatCreditConservation, BackpressureStallsAtSeedThenReopens)` (`src/c_model/tests/nmu/test_nmu_credit.cpp`), `TEST(RouterWrap, DatLocalCreditReturnReplenishesRouter)`. Failure: a 9th un-credited flit on one VC, or a stall with credit available.
27. Response ingress: the NMU accepts every flit presented on either response face (RSP `ready` is tied true, DAT has no ready wire), demuxes B / R into queues of depth NMU_DEPKT_Q_DEPTH = 16 each, and holds at most one pending flit when the target queue is full. That pending flit blocks all later response flits behind it regardless of channel (single-ingress head-of-line blocking, as built). Verified: `TEST(NmuDepacketize, PendingFlitHolBlockingBFullStallsR)`, `DemuxMixedFlitsByAxiCh`. Failure: a dropped response flit, or R progress past a stalled pending flit.
28. Held-valid responses: bvalid / rvalid with all payload fields hold unchanged until the cycle their ready is sampled high, then either the next beat or valid = 0 appears (rule P4). Verified: co-sim (the AXI master BFM samples with randomized ready delays, scoreboard compare) and `TEST(NmuWrap, single_aw_w_two_phase_handshake)`. Failure: valid deasserted or payload changed before ready.
29. DAT response credit return: one pulse on `rx_dat_crdvalid_o[v]` per `DataR` flit consumed from VC v, at most one pulse per VC per cycle, pending pulses accumulate and drain in later cycles, none is lost. RSP returns no credit — `rx_rsp_ready_o` is tied true (SPEC 27) and the face has no credit pin. Verified: `TEST(NmuDatCreditConservation, ConsumerPulseAccumulatesMultiConsumePerTick)`. Failure: pulse count not equal to consumed-flit count over any window.
30. Unloaded latencies: the rule P8 values (AW handshake to AW flit = 3 cycles, B flit to bvalid = 3 cycles, R flit to rvalid = 2 cycles Disabled / 3 cycles Enabled) are exact in an idle, credit-available NMU. These counts are asserted by construction: they follow from the fixed `Nmu::tick` stage order (Section 2.4) and the one-cycle wrap output register (rule P3), not from a dedicated cycle-count test. `TEST(NmuWrap, init_with_config_path_loads_sam_from_yaml)` confirms an AW flit emerges within a bounded window; no ctest pins the exact cycle count. Failure: any deviation from the tick-order-derived count.

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
|  |  AW/W/AR -> AxiSlavePort -> Rob -------> S1 ------> Packetize       |  |
|  |             (5 FIFOs x16)   (admission,  (1-entry   (flit build,    |  |
|  |                              SAM, slot    reg per    w_meta FIFO)   |  |
|  |                              alloc)       channel)      |           |  |
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
tx_dat_flit_o      0    0    0    0    AW   W0   W1    <- 629'h0 while valid low
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
