# Design: Network Master Unit (NMU)

The NMU is the master-side network interface of the NoC. It sits between an external AXI4 master and the router mesh: it converts AXI4 request beats (AW / W / AR) into NoC flits, and converts response flits (B / R) back into AXI4 response beats. This document specifies the as-built C++ behavior model (`src/c_model/include/nmu/`), its wrap (`src/c_model/include/wrap/nmu_wrap.hpp`), and the SV DPI module (`src/sv/nmu_wrap.sv`). An RTL implementation of this block is verified cycle-by-cycle against this model by the existing co-sim testbench.

## 2. Design Description

### 2.1 Packetization

The NoC fabric moves fixed-size 408-bit flits, not AXI beats. Packetization is a 1-to-1 mapping: every accepted AXI request beat becomes exactly one flit, and every response flit becomes exactly one AXI response beat. A flit is a 56-bit header plus a 352-bit payload. The header carries routing and ordering metadata (source node, destination node, virtual channel, wormhole packet boundary, reorder-buffer tag). The payload carries the AXI channel fields verbatim.

A write transaction of AWLEN+1 beats therefore becomes 1 AW flit followed by AWLEN+1 W flits. A read request becomes 1 AR flit. The write's flits form one wormhole packet (the AW flit opens it, the W flit with `wlast=1` closes it) so no other request flit from this NMU can interleave between an AW and its W beats on the link. A read request or a response is a single-flit packet.

The NMU has two independent flit streams:

- Request path (NMU produces): AW / W / AR flits toward the router, credit flow control, no ready wire.
- Response path (NMU consumes): B / R flits from the router, credit returned per consumed flit.

### 2.2 Flit format

> Pre-S1: this table predates the Stage 1 flit-layout change (44 b header, 48 b addr, 396 b flit) and is re-synced in campaign Stage 5.

Source of truth: `specgen/generated/cpp/ni_flit_constants.h` (generated from `specgen/generated/json/ni_packet.json`, drift-gated at build). Totals: `FLIT_WIDTH` = 408, `HEADER_WIDTH` = 56, `PAYLOAD_WIDTH` = 352. Header occupies `flit[55:0]`, payload occupies `flit[407:56]`. Payload field positions below are relative to the payload region: payload bit p is flit bit p+56.

Header (`flit[55:0]`):

| Field | Bits | Width | Definition |
|---|---|---|---|
| axi_ch | [2:0] | 3 | AXI channel of the payload. 3'd0: AW. 3'd1: W. 3'd2: AR. 3'd3: B. 3'd4: R. Values 3'd5 to 3'd7 never occur. |
| src_id | [10:3] | 8 | Source node id, {y[3:0], x[3:0]}. Constructor argument of the NMU instance. |
| dst_id | [18:11] | 8 | Destination node id, {y[3:0], x[3:0]}. From SAM lookup of the beat address (request path). |
| vc_id | [21:19] | 3 | Virtual channel, 0 <= vc_id < NUM_VC <= 8. Stamped by the VC arbiter on request flits. |
| flit_tail | [22] | 1 | Wormhole packet boundary. AW: 0 (opens the write packet). W: wlast (closes it). AR / B / R: 1 (single-flit packet). |
| ordering_req | [23] | 1 | 1: the response to this request owns reserved reorder-buffer slots. 0: bypassed, no slot. |
| ordering_tag | [31:24] | 8 | RoB slot tag. AW: the single slot. AR: base of the len+1 consecutive slots. 0 when ordering_req=0. |
| rsvd | [55:32] | 24 | Must be 0. Checked by `Flit::check_padding_is_zero`. |

AW payload (112 bits used, payload[351:112] = 0). AR payload is field-for-field identical with `ar*` names:

| Field | Payload bits | Width | Definition |
|---|---|---|---|
| awid | [7:0] | 8 | AXI ID. |
| awaddr | [71:8] | 64 | Rebased local address: wire address minus the SAM region base (Section 2.3). NOT the wire address. |
| awlen | [79:72] | 8 | Burst length minus 1. |
| awsize | [82:80] | 3 | Bytes per beat = 2^awsize, awsize <= 3'h5 (32 B, the 256-bit data bus). |
| awburst | [84:83] | 2 | 2'b00: FIXED. 2'b01: INCR. 2'b10: WRAP. 2'b11 never occurs. |
| awcache | [88:85] | 4 | Pass-through. |
| awlock | [89] | 1 | Pass-through. |
| awprot | [92:90] | 3 | Pass-through. |
| awregion | [96:93] | 4 | Carried in the flit, tied to 0 at the co-sim boundary (not in the DPI signature). |
| awqos | [100:97] | 4 | Pass-through. |
| awuser | [108:101] | 8 | Carried in the flit, tied to 0 at the co-sim boundary. |
| rsvd | [111:109] | 3 | 0. |

W payload (352 bits):

| Field | Payload bits | Width | Definition |
|---|---|---|---|
| wlast | [0] | 1 | Last beat of the burst. Mirrors header.flit_tail on W flits. |
| wuser | [8:1] | 8 | Carried, tied to 0 at the co-sim boundary. |
| wstrb | [40:9] | 32 | Byte strobes. |
| wdata | [296:41] | 256 | Write data. |
| rsvd | [351:297] | 55 | 0. |

B payload (64 bits used, payload[351:64] = 0), consumed by the response path:

| Field | Payload bits | Width | Definition |
|---|---|---|---|
| bid | [7:0] | 8 | AXI ID of the completed write. |
| bresp | [9:8] | 2 | 2'b00 OKAY, 2'b01 EXOKAY, 2'b10 SLVERR, 2'b11 DECERR. |
| buser | [17:10] | 8 | Decoded from the flit, not driven onto the co-sim AXI face. |
| rsvd | [63:18] | 46 | 0. |

R payload (352 bits), consumed by the response path:

| Field | Payload bits | Width | Definition |
|---|---|---|---|
| rlast | [0] | 1 | Last beat of the read burst. |
| rid | [8:1] | 8 | AXI ID. |
| rresp | [10:9] | 2 | Same encoding as bresp. |
| ruser | [18:11] | 8 | Decoded from the flit, not driven onto the co-sim AXI face. |
| rdata | [274:19] | 256 | Read data. |
| rsvd | [351:275] | 77 | 0. |

### 2.3 SAM address translation

Destination derivation is a System Address Map (SAM) range lookup, not a bit-slice decode. The SAM is a list of entries {base, size, dst_id} (`nmu::addr_trans::SamTable`). Translation of address A:

**INPUT** wire address A. **COMPUTE** scan entries in list order, first entry with base <= A < base+size wins. **OUTPUT** {dst_id, local_addr = A - base}. The flit carries the rebased local_addr, so the remote slave sees a 0-based address inside its region.

The SAM is loaded at runtime from the topology YAML `address_map` block (`sim/topologies/*.yaml`, parsed by `nmu/sam_yaml.hpp`). When no config path is given, the default is a 16x16 uniform map with tile_size = 4 GiB = 0x1_0000_0000: entry i has dst_id = i = {y[3:0], x[3:0]} and base = i * 0x1_0000_0000, which makes dst_id = addr[39:32].

Example (default SAM): A = 0x12_0000_0080. Matching entry: base = 0x12 * 0x1_0000_0000 = 0x12_0000_0000, size 0x1_0000_0000, dst_id = 8'h12 (x=2, y=1). Result: dst_id = 8'h12, local_addr = 0x80. Non-matching entry for contrast: the dst_id = 8'h11 entry covers [0x11_0000_0000, 0x12_0000_0000), which excludes A because A >= its base+size.

A SAM miss (address covered by no entry) cannot happen (Section 3.5, guarantee G1). The model aborts if violated. There is no DECERR generation in this block.

### 2.4 Request pipeline

```
AXI master -> AxiSlavePort -> Rob -> S1 (NmuReqS1Bridge) -> Packetize
           -> WormholeArbiter(3 in: AW, W, AR, pairing {AW->W})
           -> VcArbiter -> noc_req_o (credit-gated)
```

- `AxiSlavePort`: per-channel FIFOs (depth `NMU_QUEUE_DEPTH` = 16 each for AW / W / AR / B / R). Pure transport, FIFO order per channel regardless of AXI ID.
- `Rob`: request admission gate and response reorder buffer (Section 2.5). Stamps ordering_req / ordering_tag metadata and translates the address through its SAM.
- S1 (`NmuReqS1Bridge`): one 1-entry pipeline register per channel (AW, W, AR), drained independently so a full AW register never blocks the W stream that must release the wormhole lock.
- `Packetize`: builds the flit (Section 2.2). W flits inherit dst_id / ordering_req / ordering_tag from a write-metadata FIFO (`w_meta_fifo_`), pushed per AW flit, popped on the wlast W flit. The model queue is unbounded, its occupancy is bounded by the number of accepted AW bursts whose wlast W flit has not yet been emitted.
- `WormholeArbiter`: 3-input round-robin arbiter with the AW->W lock, 1 flit per cycle, per-input pending depth `NMU_ARBITER_FIFO_DEPTH` = 4. Draining an AW flit (header.flit_tail=0) locks the arbiter to the W input until the W flit with header.flit_tail=1 drains. AR flits (flit_tail=1) never lock.
- `VcArbiter`: assigns vc_id and stamps it into the header, per-VC pending queue depth `NMU_ARBITER_FIFO_DEPTH` = 4, drains at most 1 flit per cycle to the NoC, gated on per-VC sender credit.

Virtual-network read/write split (`ni/virtual_network.hpp`): NUM_VC = 1 shares VC0 for both directions. NUM_VC even: write class uses VCs {0 .. NUM_VC/2 - 1}, read class uses VCs {NUM_VC/2 .. NUM_VC-1}. Example NUM_VC = 4: AW/W candidates {0,1}, AR candidates {2,3}. Legal NUM_VC values are 1 and any even value up to 8 (1, 2, 4, 6, 8). Odd NUM_VC > 1 aborts at construction. The working co-sim configuration set is {1, 2, 4, 8}.

Per-cycle evaluation order inside the model (`Nmu::tick`, nmu.hpp:293-313, this order is the spec): WormholeArbiter, VcArbiter, S1-to-Packetize, AxiSlavePort request forward, then the response drains (S2 stages, B always, R per mode), Depacketize last. Consequences that are cycle-visible: a flit entering the WormholeArbiter and the VcArbiter traverses both in the same cycle (wormhole runs first), and an AXI ID freed by this cycle's B / R response is not yet usable by this cycle's request side (request side runs first).

### 2.5 Response path and the reorder buffer

```
noc_rsp_i -> Depacketize -> Rob -> S2 (1-entry register per channel) -> AxiSlavePort -> AXI master
```

Why a reorder buffer exists: AXI4 requires that responses with the same ID return in issue order. The fabric does not guarantee this. Two same-ID reads to different destinations can return out of order (a near slave answers before a far one), and with multiple VCs even same-destination traffic could overtake if it changed VC mid-stream. The NMU owns same-ID response ordering: it either proves a request cannot be overtaken (bypass) or reserves reorder storage for it before it enters the network.

Admission decision per AW / AR, evaluated per AXI ID (`Rob::push_aw` / `push_ar`):

**IMPORTANT** (unique classification, exactly one branch applies, in this priority):
1. Idle-ID bypass: the ID's order list is empty (nothing in flight for this ID). No slot, ordering_req=0, and the sticky flag resets (fresh streak).
2. Same-destination bypass: the ID is not sticky-fallen-back AND dst_id equals the previous accepted push's dst_id for this ID and direction. No slot, ordering_req=0.
3. Fall-back: allocate slots, ordering_req=1, and set the sticky flag. Once sticky, every later push of this ID allocates until the ID goes idle again (branch 1 is the only reset).

Example, ID = 8'h03, write direction: AW#1 dst 8'h02 (list empty, branch 1, bypass). AW#2 dst 8'h02 (same dest, branch 2, bypass). AW#3 dst 8'h05 (dest changed, branch 3, allocate, sticky). AW#4 dst 8'h05 (same dest as #3 but sticky, branch 3, allocate). All four complete and the list empties. AW#5 dst 8'h05 (branch 1 again, bypass). A counterexample for branch 2: AW#2 with dst 8'h07 would take branch 3, because 8'h07 != 8'h02.

Slot pools, per direction: B pool depth `NMU_ROB_B_DEPTH` = 32, R pool depth `NMU_ROB_R_DEPTH` = 32. An AW reserves 1 slot (B is one beat). An AR reserves ARLEN+1 consecutive slots (one per R beat), refused when free space is short. The allocator is a high-water stack: one allocation bit marks each reserved range's top slot, free space is the slot count above the highest set bit (leading-zero-count in RTL), the next base is depth minus free space, and space returns only from the top (Appendix 7.1 walks it with numbers). ordering_tag stamps the base slot. On the response side, B fills its slot, the i-th R beat of a burst fills base+i, and beats release to the master only while the ID's oldest outstanding transaction is being served (per-ID issue order, one order list per ID per direction).

`RobMode` selects the R side only. `RobMode::Enabled`: R responses use the slot pool as above. `RobMode::Disabled` (co-sim default): the R RoB is off and same-ID read ordering is enforced by a per-ID single-outstanding interlock, a second AR for an ID stalls until the first read's rlast returns. The B-side RoB always runs in both modes, writes never use an interlock. Wherever this document says "RobMode", it governs reads only.

Per-ID transaction gate, both modes' write side and Enabled reads: at most `NMU_MAX_TXNS_PER_ID` = 32 outstanding transactions per ID per direction (order-list depth). The 33rd same-ID AW is refused until one completes, even with free slots.

Shared outstanding gate, both modes, both directions: at most `NMU_OUTSTANDING_DEPTH` = 32 transactions in flight per direction, shared across all IDs. AW and AR draw from independent pools. A burst is one entry regardless of ARLEN. An entry is taken when the request is accepted and released when the response is accepted at the AXI side, B on its single beat and R on rlast. Full refuses the next request, which backpressures through the AxiSlavePort queue to awready / arready. This is the only aggregate gate on bypassed traffic, which reserves no slot: three limiters coexist and which one binds depends on the configuration. With `NMU_OUTSTANDING_DEPTH` at or below `NMU_MAX_TXNS_PER_ID` the per-ID gate never binds first; above it, a single-ID stream hits the per-ID gate while the shared pool still has room.

### 2.6 Worked example: 2-beat write burst

NMU at node (x=0, y=0), src_id = 8'h00, default SAM, NUM_VC = 1. The master issues AW {awid = 8'h05, awaddr = 0x12_0000_0080, awlen = 8'h01, awsize = 3'h5, awburst = 2'b01} then W0 {wdata = 256'h...11, wstrb = 32'hFFFF_FFFF, wlast = 0} and W1 {wdata = 256'h...22, wstrb = 32'hFFFF_FFFF, wlast = 1}.

SAM: 0x12_0000_0080 hits the dst_id = 8'h12 entry, local_addr = 0x80. ID 5 is idle, so admission takes the idle-ID bypass: ordering_req = 0, ordering_tag = 0. Three flits leave, in this exact order, as one wormhole packet:

| Field | AW flit | W flit 0 | W flit 1 |
|---|---|---|---|
| header.axi_ch | 3'd0 | 3'd1 | 3'd1 |
| header.src_id | 8'h00 | 8'h00 | 8'h00 |
| header.dst_id | 8'h12 | 8'h12 (inherited from AW) | 8'h12 |
| header.vc_id | 3'd0 | 3'd0 (follows the AW's VC) | 3'd0 |
| header.flit_tail | 1'b0 (opens packet) | 1'b0 | 1'b1 (closes packet) |
| header.ordering_req / ordering_tag | 0 / 8'h00 | 0 / 8'h00 | 0 / 8'h00 |
| payload | awid=8'h05, awaddr=64'h80, awlen=8'h01, awsize=3'h5, awburst=2'b01, others 0 | wlast=0, wstrb=32'hFFFF_FFFF, wdata=256'h...11, wuser=0 | wlast=1, wstrb=32'hFFFF_FFFF, wdata=256'h...22 |

Later one B flit returns {axi_ch = 3'd3, bid = 8'h05, bresp = 2'b00} and the NMU presents bvalid / bid = 8'h05 / bresp = 2'b00, held until bready.

### 2.7 Parameters

Single source `specgen/source/constants.yaml`, generated into `ni_params.h` and `ni_params_pkg.sv` (drift-gated by `codegen.py --check`). Defaults below are the shipped values.

| Parameter | Default | Legal range | Consumed by |
|---|---|---|---|
| AXI_ID_WIDTH | 8 | 1..32 (implementation locked at 8) | ID fields, RoB per-ID arrays (256 IDs) |
| AXI_ADDR_WIDTH | 64 | 1..64 (implementation locked at 64) | Address fields |
| AXI_DATA_WIDTH | 256 | {32,64,128,256,512,1024} (implementation locked at 256) | wdata / rdata, WSTRB_WIDTH = 32 |
| NOC_NUM_VC | 1 | 1 or even, up to 8 (odd > 1 aborts) | VC arbiter, credit vectors, virtual networks |
| NOC_FLIT_WIDTH | 408 | 64..1024 (implementation locked at 408 by the flit format) | Flit ports |
| NOC_ROUTER_VC_DEPTH | 4 | 1..16 | Request sender credit seed per VC |
| NMU_ROB_B_DEPTH | 32 | 1..256 | B slot pool |
| NMU_ROB_R_DEPTH | 32 | 1..256 | R slot pool |
| NMU_MAX_TXNS_PER_ID | 32 | 1..256 | Per-ID order-list depth |
| NMU_OUTSTANDING_DEPTH | 32 | 1..256 | Shared outstanding pool, per direction |
| NMU_QUEUE_DEPTH | 16 | 1..1024 | AxiSlavePort AW/W/AR/B/R FIFOs |
| NMU_DEPKT_Q_DEPTH | 16 | 1..1024 | Depacketize B/R queues |
| NMU_ARBITER_FIFO_DEPTH | 4 | 1..64 | Wormhole per-input and VC pending queues |

Runtime configuration per instance: src_id, SAM config path, RobMode, RoB depth and outstanding-depth overrides come through `cmodel_nmu_create` / `cmodel_nmu_create_ex` (Section 3.3). The generated testbench sets src_id = {y[3:0], x[3:0]} per node and forwards the plusargs `+sam_config=`, `+b_rob_depth=`, `+r_rob_depth=`, `+max_txns_per_id=`, `+outstanding_depth=`.

## 3. Inputs and Outputs

All ports below are the real `nmu_wrap` ports (`src/sv/nmu_wrap.sv:36-53`). AXI signals are fields of the packed structs `ni_signals_pkg::axi_req_t` / `axi_rsp_t`, NoC signals are fields of `ni_signals_pkg::noc_chan_t` and `noc_types_pkg::noc_credit_t`. The `axi_req_t` struct carries `awregion` / `arregion`, but they are not in the DPI signature: the model sees region = 0 and stamps 0 into the flit. No AXI user signals exist on the wire face, the flit's user fields are stamped 0 and response user fields are dropped.

### 3.1 Inputs

| Signal | Bit Width | Definition |
|---|---|---|
| clk_i | 1 | Clock. All sampling on the positive edge. |
| rst_ni | 1 | Synchronous active-low reset. Given only once, at the beginning of simulation. |
| ctx_i | 64 | Model instance handle returned by `cmodel_nmu_create`. Constant after creation. |
| axi_req_i.awvalid | 1 | AW valid. Must stay high until awready is observed. |
| axi_req_i.awid | 8 | Write transaction ID. Sampled only on the awvalid && awready cycle. |
| axi_req_i.awaddr | 64 | Write address. Must hit a SAM entry (guarantee G1). |
| axi_req_i.awlen | 8 | Burst length minus 1, 0 <= awlen <= 8'hFF (256 beats max). |
| axi_req_i.awsize | 3 | Beats of 2^awsize bytes, awsize <= 3'h5 = 32 bytes. |
| axi_req_i.awburst | 2 | 2'b00 FIXED, 2'b01 INCR, 2'b10 WRAP. 2'b11 never occurs. |
| axi_req_i.awlock, awcache, awprot, awqos | 1, 4, 3, 4 | Attribute pass-through into the flit payload. |
| axi_req_i.awregion | 4 | Present in the struct, not sampled: the flit carries awregion = 4'h0. |
| axi_req_i.wvalid | 1 | W valid. Must stay high until wready is observed. |
| axi_req_i.wdata | 256 | Write data, valid only when wvalid is high. |
| axi_req_i.wstrb | 32 | Byte strobes. |
| axi_req_i.wlast | 1 | High on the final beat of each write burst. |
| axi_req_i.bready | 1 | Master ready for B. |
| axi_req_i.arvalid, arid, araddr, arlen, arsize, arburst, arlock, arcache, arprot, arqos, arregion | as AW | Read address channel, field-for-field mirror of AW (arregion likewise not sampled). |
| axi_req_i.rready | 1 | Master ready for R. |
| noc_rsp_i.valid | 1 | A response flit is on the wire this cycle. |
| noc_rsp_i.flit | 408 | Response flit, format of Section 2.2. axi_ch must be 3'd3 (B) or 3'd4 (R) (guarantee G2). Unknown when valid is low. |
| noc_req_cred_i.credit | NUM_VC | Per-VC one-cycle credit pulse: bit v high means the router drained one NMU request flit from VC v. Multiple bits may pulse in one cycle. |

### 3.2 Outputs

Every output is a registered signal: it changes only at posedge clk_i and reflects the model state computed from the previous cycle's inputs.

| Signal | Bit Width | Definition |
|---|---|---|
| axi_rsp_o.awready | 1 | One-shot: high for exactly 1 cycle, only after awvalid is observed and the AW FIFO has space. See rule P7. |
| axi_rsp_o.wready | 1 | Pre-asserted: high whenever accepted AWs still owe W beats and the W FIFO has space. Does not wait for wvalid. |
| axi_rsp_o.arready | 1 | One-shot, same policy as awready, independent of the write side. |
| axi_rsp_o.bvalid | 1 | B beat available. Held high until bready is observed (AXI4 IHI 0022 A3.2.1). |
| axi_rsp_o.bid | 8 | B transaction ID. 0 when bvalid is low. |
| axi_rsp_o.bresp | 2 | Write response, masked to 2 bits. 0 when bvalid is low. |
| axi_rsp_o.rvalid | 1 | R beat available. Held high until rready is observed. |
| axi_rsp_o.rid, rdata, rresp, rlast | 8, 256, 2, 1 | Read response fields. 0 when rvalid is low. |
| noc_req_o.valid | 1 | Request flit on the wire this cycle. At most one flit per cycle. Not held: the flit is consumed by the credit protocol, there is no ready wire. |
| noc_req_o.flit | 408 | Request flit, format of Section 2.2. 0 when valid is low. |
| noc_rsp_cred_o.credit | NUM_VC | Per-VC one-cycle consumer credit pulse: bit v high means the NMU consumed one response flit from VC v. At most one pulse per VC per cycle (pending consumptions queue up and drain one per cycle). |

### 3.3 DPI functions

The SV wrap holds no behavior. Each posedge it runs the 3-call discipline: `cmodel_nmu_set_inputs` (latch wires, no state change), `cmodel_nmu_tick` (advance the model one cycle), `cmodel_nmu_get_outputs` (copy the output latch), then registers the outputs nonblocking. Declared in `src/dpi/cmodel_dpi.h:91-114`.

| Function | Signature (summary) | Semantics |
|---|---|---|
| cmodel_nmu_create | `unsigned long long (const char* name, int src_id, int num_vc, const char* config_path)` | Constructs the instance, RobMode::Disabled (R side), default depths. NULL/empty config_path selects the default 16x16 / 4 GiB SAM. Returns the 64-bit handle for ctx_i. |
| cmodel_nmu_create_ex | `(..., int rob_enabled, int b_rob_depth, int r_rob_depth, int max_txns_per_id, int outstanding_depth, const char* config_path)` | As create, plus R-RoB enable and depth overrides. The generated testbench calls this in both RoB modes, since the outstanding pool applies to either. |
| cmodel_nmu_set_inputs | `(ctx, 26 AXI args, svBit noc_rsp_valid, svBitVecVal* noc_rsp_flit, svBitVecVal* noc_req_credit_return)` | Latches inputs only. Packing: 8-bit fields in word[0] low byte, addresses 2 words little-endian, data 8 words little-endian, wstrb 1 word, flit 13 words little-endian, credit vector 1 word bit-per-VC. |
| cmodel_nmu_tick | `(ctx)` | One full model cycle. One call = one clock edge. |
| cmodel_nmu_get_outputs | `(ctx, 14 output args)` | Copies the output latch. bresp / rresp masked with 2'b11. |
| cmodel_nmu_read_slot_hwm | `unsigned int (ctx)` | Telemetry: peak R-RoB slot occupancy. 0 when the handle is invalid or the R RoB is Disabled. Printed per node at testbench exit. |

An invalid handle raises a categorized model error which the testbench error poll turns into `$fatal`.

### 3.4 Protocol rules

1. Input rhythm: the AXI face is handshake-paced, there is no fixed delivery rhythm. Within one write, exactly awlen+1 W beats follow their AW in issue order and bursts never interleave W beats (AXI4 has no WID). Example: AW with awlen = 8'h01 is followed by 2 W beats, the second with wlast = 1. `noc_rsp_i` presents at most one flit per cycle. Credit vectors may pulse any subset of bits in any cycle.
2. Idle state: input payload buses are don't-care while their valid is low, the model reads them only on the handshake cycle (valid and ready both high) and reads `noc_rsp_i.flit` only while `noc_rsp_i.valid` is high. All output payload buses are driven to 0 while their valid is low (example: `noc_req_o.flit` = 408'h0 whenever `noc_req_o.valid` = 0).
3. Sampling edge: all inputs are sampled at the positive edge of clk_i. One posedge = one model cycle via the 3-call DPI discipline of Section 3.3, and the outputs computed from cycle-N inputs appear on the wires during cycle N+1 (registered outputs). The verification pattern captures outputs at the positive edge.
4. Valid behavior: bvalid and rvalid, once asserted, stay asserted with stable payload until the cycle their ready is high (AXI4 IHI 0022 A3.2.1). `noc_req_o.valid` is a per-flit strobe, high only on cycles that carry a flit, at most one flit per cycle, never waiting on a ready (the per-VC credit counter guarantees the receiver has buffer space).
5. Output idle value: bid, bresp, rid, rdata, rresp, rlast are 0 when their valid is low. `noc_req_o.flit` is 0 when its valid is low. Credit outputs are 0 in non-pulse cycles.
6. Reset: rst_ni is given only once, at the beginning of simulation, synchronous active-low. While rst_ni is low all outputs are 0 (the wrap clears its output registers, the model starts reset by construction). There is no mid-run reset.
7. Handshake gaps: awready and arready are one-shot, they assert only after their valid is observed and drop the cycle after the handshake, so consecutive same-channel address handshakes are at least 2 cycles apart (handshakes on cycles 2, 4, 6 of the Section 6 waveform pattern). AW acceptance is not gated on the previous write's W burst finishing (multi-outstanding AW is supported and w-owed counts accumulate). wready is pre-asserted while owed W beats remain and FIFO space exists, so W beats can stream back-to-back, 1 per cycle. An incoming request credit pulse is usable in the same model cycle (replenished before the tick). At most one response credit pulse per VC per cycle is emitted.
8. Latency: measured from the positive edge ending the input's handshake cycle to the first cycle the corresponding output valid is high, in an otherwise idle NMU with credit available. AW handshake to its AW flit on `noc_req_o`: exactly 3 cycles. Response B flit on `noc_rsp_i` to bvalid: exactly 3 cycles. Response R flit to rvalid: exactly 2 cycles with the R RoB Disabled, exactly 3 cycles with it Enabled. Under load these are lower bounds, backpressure adds cycles without an upper bound.

### 3.5 Input guarantees

The implementation does not handle the following, they are guaranteed not to happen. The model asserts and aborts on each (assert locations in parentheses).

| # | Guarantee | Reason / model check |
|---|---|---|
| G1 | Every issued address hits a SAM entry. | The system address map covers all issued addresses. Miss aborts (`addr_trans.hpp:51`). No DECERR path exists. |
| G2 | Only B or R flits arrive at `noc_rsp_i`. | Fabric routes request and response classes on disjoint networks. Other axi_ch aborts (`depacketize.hpp:110-117`). |
| G3 | No burst crosses a SAM region boundary. | Regions are 4 KiB aligned and sized and AXI4 forbids 4 KiB crossings, so the upstream master never issues one. The model asserts this only on the direct Packetize path (`packetize.hpp:66-83`). |
| G4 | The SAM itself is well-formed: nonzero sizes, 4 KiB aligned base and size, no overlap, dst inside the mesh. | Checked once at load (`addr_trans.hpp:60-79`). |
| G5 | valid, once asserted, holds with stable payload until ready (both directions of the AXI face). | AXI4 A3.2.1. The one-shot ready policy depends on it: ready asserts one cycle after valid is first seen. |
| G6 | W beat counts match their AWs (exactly awlen+1 beats, wlast on the final beat, no spurious W). | AXI4 legality is the master's job. The owed-W counter floors at 0 and does not reject an unexpected W. |
| G7 | The router never pulses more request credits than flits it drained, and never presents a response flit without holding a credit for it. | Credit conservation: per VC, sender credit + in-flight flits = seed (4). A credit lie downstream of the VC arbiter aborts (`vc_arbiter.hpp:226-231`). |
| G8 | awburst / arburst = 2'b11 never occurs, and header.axi_ch values 3'd5 to 3'd7 never occur. | Reserved encodings. |
| G9 | NUM_VC is 1 or even, at most 8, and the elaborated `noc_credit_t` width equals NUM_VC. | Odd NUM_VC > 1 aborts (`virtual_network.hpp:21-26`), width mismatch is `$fatal` at elaboration (`nmu_wrap.sv:56-61`). |

## 4. Specifications

Each item names its verification and the failure condition. "ctest" items run in the pure C++ suite (`src/c_model/tests/`), "co-sim" items run under the generated testbench (`make sim TB=<topology> PATTERN=<pattern>`, regression via `sim/run_regress.py`) where correctness is judged by the scoreboard's per-transaction write-to-readback compare plus the model's internal asserts (any assert abort fails the run).

1. Interface: the block implements exactly the Section 3.1 / 3.2 ports of `nmu_wrap` with the given widths (FLIT_WIDTH = 408, NUM_VC-wide credit vectors). Verified: co-sim elaboration (struct port binding, `$fatal` width guard nmu_wrap.sv:56-61). Failure: elaboration error or width-guard fatal.
2. Reset: after the single initial rst_ni assertion, every output is 0, and outputs stay 0 until traffic (rule P6). Verified: `TEST(NmuWrap, idle_adapter_keeps_readys_low)` (`src/c_model/tests/wrap/test_nmu_wrap.cpp`) and cycle-1 sampling in co-sim. Failure: any nonzero output during or immediately after reset.
3. Registered outputs: outputs are functions of state as of the previous posedge, never combinational paths from same-cycle inputs (rule P3). Verified: `TEST(NmuWrap, single_aw_w_two_phase_handshake)`, which requires the 1-cycle valid-to-ready offset. Failure: same-cycle input-to-output dependence changes the handshake cycle count.
4. One-shot address ready: awready (arready) asserts only when awvalid (arvalid) was high the previous sampled cycle with FIFO space, stays high 1 cycle, and is low the cycle after a handshake. Consequence: at most one address handshake per 2 cycles per channel. Example: awvalid from cycle 1 gives awready only in cycle 2, and a second AW held from cycle 3 handshakes in cycle 4. Verified: `TEST(NmuWrap, single_aw_w_two_phase_handshake)`, `TEST(NmuWrap, multi_beat_w_burst_full_rate_aw_available)`. Failure: ready asserted before valid observed, held more than 1 cycle, or back-to-back address handshakes.
5. Pre-asserted wready: wready = (owed W beats > 0) AND W FIFO space, where the owed count increases by awlen+1 per accepted AW and decreases by 1 per accepted W beat, accumulating across outstanding AWs. Example: two accepted AWs with awlen = 8'h01 and 8'h00 give owed = 3, wready holds through 3 streaming W beats then drops. Verified: `TEST(NmuWrap, multi_beat_w_burst_full_rate_aw_available)`. Failure: wready high with no owed beats, or W throughput below 1 beat per cycle with space available.
6. Acceptance atomicity: a beat is consumed exactly on its valid-and-ready cycle, and ready is never asserted without guaranteed FIFO space, so an accepted beat is never dropped or duplicated. Verified: `TEST(NmuAxiSlavePort, AwBoundary_FailedPushDoesNotDuplicateOnRetry)` and co-sim scoreboard compare. Failure: lost or duplicated beat (scoreboard miscompare).
7. Per-channel FIFO order: beats of one channel travel in acceptance order regardless of AXI ID, through every request stage. Verified: `TEST(NmuAxiSlavePort, AwFifoOrder_PreservedAcrossMixedIds)`, `ArFifoOrder_PreservedAcrossMixedIds`. Failure: any same-channel reorder.
8. Flit format: emitted flits match Section 2.2 bit-exactly, header rsvd = 24'h0, unused payload bits 0, disabled placeholder fields absent. Verified: `TEST(NmuPacketize, AwPayloadBitPerfect)`, `WPayloadBitPerfect`, `ArEncodesAxiChAndOrderingTag`, `RsvdAndDisabledFieldsZero` (`src/c_model/tests/nmu/test_packetize.cpp`). Failure: any mismatched bit.
9. One beat, one flit, rebased address: each accepted AW / W / AR beat emits exactly one flit, address payloads carry local_addr = addr - region base (Section 2.3 example: 0x12_0000_0080 becomes 64'h80 with dst_id = 8'h12). awregion / arregion and all user fields are carried in the flit and are 0 at the co-sim boundary. Verified: `TEST(NmuPacketize, SamTranslateRebasesAddrAndSetsDstFromTable)`, `TEST(AddrTrans, RebasedLocalIsTileOffset)`. Failure: wire address in the payload or wrong dst_id.
10. header.flit_tail stamping: AW = 0, W = wlast, AR = 1. Verified: `TEST(NmuPacketize, WHeaderFlitTailMatchesWlast)`, malformed stamping aborts in the wormhole arbiter (`wormhole_arbiter.hpp:190-201`). Failure: assert abort or a wormhole packet that never closes.
11. AW before W: a W flit never enters the network before its AW flit. The RoB refuses W beats while no AW-accepted burst owes beats, and W flits inherit dst_id / ordering_req / ordering_tag from the AW-ordered metadata FIFO. Verified: `TEST(NmuRob, Disabled_WCreditBlocksWBeforeAw)`, `TEST(NmuPacketize, WMetaFifoInheritsAwDst)`, `TEST(NmuReqBridge, PushWBackpressuresOnEmptyMeta)`. Failure: W flit precedes its AW flit or carries wrong metadata.
12. Wormhole atomicity: after an AW flit drains, only W flits of that burst drain until the header.flit_tail = 1 W flit, AR flits wait. Verified: `TEST(NocWormholeArbiter, ArCannotInterleaveDuringLock)`, `MultiBeatWBurstFlowsAndUnlocks` (`src/c_model/tests/router/test_wormhole_arbiter.cpp`). Failure: any foreign flit between AW and its final W.
13. IMPORTANT wormhole tie-break: when unlocked, inputs (0 = AW, 1 = W, 2 = AR) are scanned round-robin starting at the input after the last drained one, first non-empty input wins, 1 flit per cycle. Example: pointer at 0 with AW and AR both pending drains AW (input 0), locks to W, and after the wlast W flit drains from input 1 the pointer is 2, so AR wins the next free cycle even if a new AW is pending. Verified: `TEST(NocWormholeArbiter, AwTriggersLock)` and the co-sim throughput scenarios. Failure: wrong winner in the tie case.
14. SAM lookup: destination is the first entry (list order) whose [base, base+size) contains the address, output {dst_id, addr - base}. Verified: `TEST(SamTable, UniformRebase_DstFromTableLocalRebased)`, `TEST(SamYaml, ExplicitTilesOverride)`. Failure: wrong dst_id or local_addr for a table with overlapping-candidate order dependence.
15. SAM validation: a loaded SAM with a zero-size entry, non-4 KiB alignment, address-space overflow, out-of-mesh dst, or overlapping ranges is rejected at load. Verified: `TEST(SamValidator, RejectsOverlap)`, `RejectsNon4KBSize`, `RejectsDstOutsideMesh`. Failure: bad SAM accepted.
16. Same-ID response ordering: B and R beats presented to the master for one AXI ID follow the issue order of their requests, in every mode (B side always via the RoB, R side via slot pool when Enabled, via the single-outstanding interlock when Disabled). Cross-ID order and write-to-read same-address order are NOT guaranteed by this block. Verified: `TEST(NmuRob, Enabled_PopB_OutOfOrder_HeldUntilHeadReady)`, `Enabled_MixedList_OrderPreserved`, `Disabled_StallReleaseOnRlast`, and end-to-end by the co-sim scoreboard compare. Failure: any same-ID response inversion.
17. IMPORTANT admission classification: exactly one of {idle-ID bypass, same-destination bypass, fall-back allocate} applies per AW / AR, in that priority, per the Section 2.5 tree, with the sticky flag reset only by the idle-ID branch. Example: Section 2.5 five-AW trace, AW#4 allocates although its destination matches AW#3. Verified: `TEST(RobSameDestBypass, SameDestStreakBypassesAll)`, `DestChangeTriggersStickyFallback`, `TEST(NmuRob, Enabled_IdleIdBypass_FirstTxnPerIdAllocatesNoSlot)`. Failure: wrong branch taken.
18. Slot reservation: AW takes 1 slot, AR takes arlen+1 consecutive slots, base = pool depth - free space, refused when free space is short, and refusal mutates no state. Free space is the count above the highest allocated range top (high-water stack, Appendix 7.1). Verified: `TEST(NmuRob, Enabled_PushAr_AllocatesConsecutiveSlotsForBurst)`, `Enabled_LzcAllocator_IsAStack`, `Enabled_PushAw_PoolFull_ReturnFalseAtomic`, `Enabled_PushAr_DownstreamBackpressure_AtomicRollback`. Failure: wrong base, overlapping ranges, or state change on refusal.
19. No free slot, no request: an ordering_req = 1 flit never enters the network without its slots already reserved, so a returning tagged response always finds its slot. Verified: `TEST(NmuRobDeath, Enabled_PopBWithUnallocatedOrderingTag_Abort)` (the model aborts on a tag with no slot). Failure: model abort on response arrival.
20. IMPORTANT per-ID release: responses release to the master only from the head of the ID's order list. A ready RoB'd entry behind an incomplete older entry waits, and a bypassed (ordering_req = 0) head is popped by its own response (B, or R with rlast) before anything behind it releases. Example: ID 3 issues bypassed AW#1 then RoB'd AW#2, B#2 arrives first and is held in slot, B#1 arrives and releases, then B#2 releases, master sees B#1 then B#2. Verified: `TEST(NmuRob, Enabled_PopB_OutOfOrder_HeldUntilHeadReady)`, `Enabled_PerBeatRelease_HeadBurstStreams`, `Enabled_BypassedBeat_ReleasesNoSlot`. Failure: release past a blocked head.
21. RobMode scope: `RobMode::Disabled` disables the R-side slot pool only, replacing it with a per-ID single-outstanding read interlock (a second same-ID AR is refused until the first read's rlast). The B-side RoB runs unconditionally in both modes. Verified: `TEST(NmuRob, Disabled_StallReleaseOnRlast)` and B-path tests passing under Disabled construction (`cmodel_nmu_create` default). Failure: a Disabled-mode B bypassing order, or a Disabled-mode second same-ID AR entering the network early.
22. Per-ID transaction gate: at most max_txns_per_id (default 32 = 0x20) outstanding transactions per ID per direction, enforced before slot availability, refusal is stateless. Example: 32 outstanding ID-7 writes refuse the 33rd AW even with 31 free B slots. Verified: `TEST(NmuRob, Enabled_MaxTxnsPerIdGate_RefusesWithFreeSlotsAvailable)`, `Enabled_MaxTxnsPerIdDefaultIsThirtyTwo`. Failure: 33rd same-ID acceptance.
23. Virtual networks: with NUM_VC even, AW / W flits only ever carry vc_id in {0 .. NUM_VC/2 - 1} and AR flits only in {NUM_VC/2 .. NUM_VC - 1}. With NUM_VC = 1 all flits carry vc_id = 0. Verified: `TEST(NmuConfigVnets, ConfigVnetsBuildSpreadingArbiter)`, `TEST(NmuVcArbiter, Degenerate_NumVc1_AllModesPassthrough)`. Failure: a flit outside its class's VC set.
24. IMPORTANT VC selection, per AW / AR flit, first matching rule wins: (a) W flits always take the VC of their AW (no selection). (b) NUM_VC = 1: VC0. (c) ordering_req = 0 and this (direction, ID) has a recorded destination equal to the flit's dst_id: reuse the recorded VC, and if that VC lacks space or credit the flit stalls, it is never rerouted. (d) otherwise: scan the direction's candidate VCs round-robin from the per-direction pointer, first VC with pending space and credit wins, pointer moves past the winner. The (dst_id, VC) record is written on every accepted ordering_req = 0 AW / AR of that (direction, ID) and persists for the whole run until overwritten by the next such accept, it is never invalidated. ordering_req = 1 flits always use rule (d) and never write the record. Example (NUM_VC = 4, read candidates {2, 3}, pointer at 2): AR ID 5 dst 8'h12 ordering_req = 0 takes VC2 and records (8'h12, VC2), a second identical AR reuses VC2 even though VC3 is idle, an AR ID 5 dst 8'h07 misses the record and round-robins to VC3. Verified: `TEST(NmuVcArbiterRoundRobin, SameReadIdSameDestFixedVcId)`, `SameReadIdDifferentDestRoundRobins`, `RobbedFlitsRoundRobinRegardlessOfDest`, `TEST(NmuVcArbiter, WFollowsAW_ReusedFixedVc)`. Failure: wrong VC in any listed case, or a mid-streak reroute.
25. IMPORTANT VC drain tie-break: at most 1 flit per cycle leaves for the NoC, chosen by a single global round-robin over all NUM_VC VCs starting at the pointer, first VC that is non-empty and has sender credit wins, pointer set to winner+1. Example (NUM_VC = 4, pointer 2, flits pending on VC1 and VC3): VC3 wins (scan 2, 3), pointer becomes 0, VC1 wins next cycle. Verified: `TEST(NmuVcArbiterRoundRobin, DistinctReadIdsSpreadAcrossVnet)` and co-sim link utilization scenarios. Failure: two flits in one cycle, a starved non-empty credited VC, or wrong winner in the tie case.
26. Request credit: the per-VC sender counter is seeded to NOC_ROUTER_VC_DEPTH = 4, decrements per emitted flit, increments per pulse on `noc_req_cred_i.credit`, and a pulse arriving in cycle N is usable in cycle N. Invariant per VC: credit + un-credited in-flight flits = 4, and the NMU never emits on a VC with counter 0. Verified: `TEST(NmuCreditConservation, BackpressureStallsAtSeedThenReopens)` (`src/c_model/tests/nmu/test_nmu_credit.cpp`), `TEST(RouterWrap, LocalInCreditReturnReplenishesRouter)`. Failure: a 5th un-credited flit on one VC, or a stall with credit available.
27. Response ingress: the NMU accepts every flit presented on `noc_rsp_i` (no ready wire), demuxes B / R into queues of depth NMU_DEPKT_Q_DEPTH = 16 each, and holds at most one pending flit when the target queue is full. That pending flit blocks all later response flits behind it regardless of channel (single-ingress head-of-line blocking, as built). Verified: `TEST(NmuDepacketize, PendingFlitHolBlockingBFullStallsR)`, `DemuxMixedFlitsByAxiCh`. Failure: a dropped response flit, or R progress past a stalled pending flit.
28. Held-valid responses: bvalid / rvalid with all payload fields hold unchanged until the cycle their ready is sampled high, then either the next beat or valid = 0 appears (rule P4). Verified: co-sim (the AXI master BFM samples with randomized ready delays, scoreboard compare) and `TEST(NmuWrap, single_aw_w_two_phase_handshake)`. Failure: valid deasserted or payload changed before ready.
29. Response credit return: one pulse on `noc_rsp_cred_o.credit[v]` per response flit consumed from VC v, at most one pulse per VC per cycle, pending pulses accumulate and drain in later cycles, none is lost. Verified: `TEST(NmuCreditConservation, ConsumerPulseAccumulatesMultiConsumePerTick)`. Failure: pulse count not equal to consumed-flit count over any window.
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
|  |                                              VcArbiter -------------+--+--> noc_req_o
|  |                                              (VC select+stamp,      |  |<-- noc_req_cred_i
|  |                                               credit-gated RR drain)|  |
|  |                                                                     |  |
|  |  RSP path                                                           |  |
|  |  B/R <- AxiSlavePort <- S2 <- Rob <------ Depacketize <-------------+--+<-- noc_rsp_i
|  |  (held-valid B/R)  (1-entry  (reorder:    (B/R demux, x16 queues,   |  |--> noc_rsp_cred_o
|  |                     reg)      slots +      1-slot HOL pending)      |  |
|  |                               order lists)                          |  |
|  +---------------------------------------------------------------------+  |
|      |noc_req_o -> router LOCAL input      router LOCAL output -> noc_rsp_i
+---------------------------------------------------------------------------+
```

S2 note: the B response always crosses Rob and the S2 register. R crosses S2 only when the R RoB is Enabled, the Disabled R path drains Depacketize -> Rob interlock -> AxiSlavePort directly (hence the 2-cycle vs 3-cycle rule P8 values).

## 6. Sample Waveform

Write of Section 2.6 (awlen = 8'h01, NUM_VC = 1), unloaded, credit available. All values are wire values per cycle, sampled at posedge.

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
noc_req_o.valid    0    0    0    0    1    1    1     <- 3 flits back-to-back
noc_req_o.flit     0    0    0    0    AW   W0   W1    <- 408'h0 while valid low
                        |--------------|
                        3 cycles: AW handshake -> AW flit (SPEC 30)

                             ... B flit returns from the router ...

noc_rsp_i.valid                                    1    0    0    0    0    0
noc_rsp_i.flit                                     B    -    -    -    -    -
noc_rsp_cred_o.credit[0]                           0    1    0    0    0    0
                                                        ^ 1-cycle consumer pulse
bvalid                                             0    0    0    1    1    0
bready                                             0    0    0    0    1    0
bid / bresp                                        0    0    0    8'h05/2'b00 held
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
