# Design: Network Slave Unit (NSU)

Top module: `nsu_wrap` (`src/sv/nsu_wrap.sv`), driving a cycle-accurate C++ model core (`src/c_model/include/nsu/`) through the DPI handle ABI. This document specifies the as-built behavior of that model. An RTL implementation of the NSU is verified cycle-by-cycle against it by the existing co-sim testbench.

## 2. Design Description

### 2.1 Function

The NSU is the slave-side network interface of the NoC. It terminates the request stream that the fabric delivers to one tile and originates that tile's response stream.

**INPUT** request flits (AW, W, AR) from the router LOCAL output port, one flit per cycle at most.
**COMPUTE** depacketize each flit into exactly one AXI beat, remap the AXI id, remember per-request metadata, drive the beats out of an AXI4 master face in arrival order. When the tile slave answers, packetize each B/R beat into exactly one response flit, restoring the original id and the requester's node id from the remembered metadata, then arbitrate the flit onto a virtual channel.
**OUTPUT** response flits (B, R) to the router LOCAL input port, plus per-VC credit pulses in both directions.

The NSU contains no reorder buffer and no address map. Requests are issued toward the slave in NoC arrival order per channel, regardless of AXI id. Reordering same-id responses back into request order is the job of the master-side NI, which the NSU supports only by echoing the `ordering_req` / `ordering_tag` header fields verbatim into every response flit.

Depacketization is a one-flit-one-beat mapping. There is no burst splitting, merging, or reassembly anywhere in the NSU:

| AXI object | flits |
|---|---|
| AW (one address phase) | 1 AW flit |
| write burst of N data beats (`awlen` = N-1) | N W flits, one full data beat each |
| AR (one address phase) | 1 AR flit |
| B (one write response) | 1 B flit |
| read burst of N data beats (`arlen` = N-1) | N R flits, one full data beat each |

### 2.2 Flit format

> Pre-S1: this table predates the Stage 1 flit-layout change (44 b header, 48 b addr, 396 b flit) and is re-synced in campaign Stage 5.

`FLIT_WIDTH` = 408 = `HEADER_WIDTH` 56 + `PAYLOAD_WIDTH` 352 (`specgen/generated/cpp/ni_flit_constants.h`). Bit numbering is LSB-first over the whole flit. Payload bit 0 is flit bit 56.

Header, flit bits [55:0]:

| field | flit bits | width | definition |
|---|---|---|---|
| `axi_ch` | [2:0] | 3 | 3'd0 AW, 3'd1 W, 3'd2 AR, 3'd3 B, 3'd4 R. Values 3'd5 to 3'd7 never occur on any link. |
| `src_id` | [10:3] | 8 | Sender node id, `(y << 4) \| x`. On a request flit this is the requester. On a response flit the NSU stamps its own node id. |
| `dst_id` | [18:11] | 8 | Destination node id. On a response flit the NSU writes the `src_id` it captured from the matching request. |
| `vc_id` | [21:19] | 3 | Virtual channel carrying the flit. On egress the VC arbiter stamps it. On ingress it selects the VC of the returned credit pulse, nothing else. |
| `flit_tail` | [22] | 1 | Fabric wormhole packet delimiter. See the IMPORTANT note in 2.4. |
| `ordering_req` | [23] | 1 | Reorder-buffer flag from the source NI. The NSU echoes it into the response, it also selects the B VC policy (2.4). |
| `ordering_tag` | [31:24] | 8 | Reorder-buffer slot from the source NI. Echoed verbatim. |
| `rsvd` | [55:32] | 24 | Padding, always 0. |

Request payloads consumed by the NSU (bit positions payload-relative):

| channel | field layout (LSB to MSB) |
|---|---|
| AW, 112 b | `awid` [7:0], `awaddr` [71:8], `awlen` [79:72], `awsize` [82:80], `awburst` [84:83], `awcache` [88:85], `awlock` [89], `awprot` [92:90], `awregion` [96:93], `awqos` [100:97], `awuser` [108:101], rsvd [111:109] |
| AR, 112 b | same layout with `ar*` names |
| W, 352 b | `wlast` [0], `wuser` [8:1], `wstrb` [40:9], `wdata` [296:41], rsvd [351:297] |

Response payloads produced by the NSU:

| channel | field layout (LSB to MSB) |
|---|---|
| B, 64 b | `bid` [7:0], `bresp` [9:8], `buser` [17:10], rsvd [63:18] |
| R, 352 b | `rlast` [0], `rid` [8:1], `rresp` [10:9], `ruser` [18:11], `rdata` [274:19], rsvd [351:275] |

`awregion`/`arregion` are carried in the flit but tied to 4'h0 at the AXI face (`nsu_wrap.sv` assigns `axi_req_o.awregion = '0`, same for AR). All AXI user fields are dropped at the co-sim boundary: `awuser`/`wuser`/`aruser` have no output port, and the wrap forces `buser` = `ruser` = 8'h00 in every response flit.

### 2.3 Metadata buffer and id remap

An AXI B or R beat carries only an id. It does not say which node asked, nor which reorder slot the master-side NI reserved. The NSU therefore captures, at the moment an AW or AR is admitted toward the slave, the 4-tuple

`{src_id, upstream_id, ordering_req, ordering_tag}`

into the `MetaBuffer` (`nsu/meta_buffer.hpp`), keyed by the **downstream** id it presents to the slave. The buffer is a per-downstream-id FIFO bucket array (256 buckets per direction) with a **shared** occupancy pool of `NSU_META_BUFFER_MAX_OUTSTANDING` = 32 entries per direction (write and read pools are independent). AXI4 guarantees per-id in-order completion (IHI 0022, A6.3), so the front of a bucket is always the oldest outstanding request on that id, and every arriving B/R matches its bucket front.

The downstream id is `remap_downstream_id(upstream_id, max_unique_ids)`:

| `NSU_META_BUFFER_MAX_UNIQUE_IDS` | downstream id | consequence |
|---|---|---|
| 1 (default) | constant 8'hFF (255 = 2^8 - 1) for every request | the slave sees one id stream, AXI per-id ordering then forces the slave globally in order, so responses always match the single FIFO bucket |
| 256 | passthrough (`upstream_id`) | the slave may complete different ids in any order, per-id buckets absorb it |

Only {1, 256} are legal. The constructor throws on any other value (`nsu/depacketize.hpp`), and the check survives NDEBUG builds. Example: `upstream_id` = 8'h07 with `max_unique_ids` = 1 becomes awid 8'hFF on the wire, and the B response with bid 8'hFF is translated back to bid 8'h07 in the flit.

### 2.4 Response packetization and VC selection

Each B/R beat is looked up against its MetaBuffer bucket front, built into one flit (id restored, `dst_id` = stored `src_id`, `src_id` = NSU node id, `ordering_req`/`ordering_tag` echoed), then committed: a B entry is retired when its flit is accepted by the arbiter, an R entry is retired only when the beat with `rlast` = 1 is accepted (every earlier beat of the burst peeks the same front entry).

**IMPORTANT:** header `flit_tail` = 1 on every B flit and on **every** R beat flit. Header `flit_tail` is the packet delimiter for fabric wormhole arbitration only. AXI burst framing lives exclusively in the payload `rlast` bit. Every response packet is therefore single-flit, and no wormhole lock is ever taken on the response path. Example: a 4-beat read returns 4 R flits, each with header `flit_tail` = 1, and payload `rlast` = 1 only in the fourth.

B and R merge through a 2-input round-robin `WormholeArbiter` (input 0 = B, input 1 = R, no channel pairing, at most 1 flit per cycle) into the `VcAllocator`, which assigns the VC. The candidate set is every VC in {0 .. `num_vc`-1}, with no read/write class split. Only the DAT face carries `num_vc` > 1 (RSP is single-VC), and DAT carries R only.

**IMPORTANT:** VC selection is:

| flit | rule | header `fixed_vc` |
|---|---|---|
| R (always, any `ordering_req`) | fixed hash `(dst_id ^ rid) % num_vc` | 1 |
| B (always, any `ordering_req`) | id-agnostic round-robin (order-free at the source RoB) | 0 |

The hash is a pure function with zero state. Every beat of an R burst shares `(dst_id, rid)`, so a whole burst lands on one VC, and a same-`(dst, id)` response stream can never be reordered in-fabric. A fixed-hash flit whose VC is full or has no downstream credit is **refused** and retried, it never spills to another VC (spilling would reorder the stream). Downstream routers keep this `vc_id` unchanged only where `fixed_vc` = 1, restamping `fixed_vc` = 0 (B) flits at VA (docs/router-spec.md R12/SPEC6).

Worked example, `num_vc` = 4:

- R flit, `dst_id` = 8'h12, `rid` = 8'h07: 8'h12 ^ 8'h07 = 8'h15 = 21, 21 % 4 = 1, so **VC 1**. All beats of this burst take VC 1.
- Comparison, R flit with `rid` = 8'h06: 8'h12 ^ 8'h06 = 8'h14 = 20, 20 % 4 = 0, so **VC 0**. Different id, different VC, the two bursts may interleave in the fabric.
- B flit: round-robin pointer starts at 0 after reset, so the first B takes VC 0 (if it has space and credit), the next takes VC 1.
- If VC 1 is full or creditless, the VC-1-hashed R flit waits in the wormhole arbiter pending queue. It is never sent on another VC.

Admission by configuration: for `num_vc` > 1 a flit is admitted to its per-VC pending queue only if that queue has space and the downstream credit counter for that VC is nonzero (round-robin B scans for the first VC meeting both). For `num_vc` = 1 the selection returns VC 0 without the entry-time credit check, only the pending-space check applies at admission, and the credit gate is enforced at the drain stage. This asymmetry is as-built. In all configurations the drain (`VcAllocator::tick`) sends at most 1 flit per cycle, round-robin over VCs, only to a VC with downstream credit.

### 2.5 Worked example, 2-beat write end to end

Configuration: `max_unique_ids` = 1, `num_vc` = 2, NSU node id 8'h34, requester node 8'h12.

Request packet arriving on `noc_req_i` (3 flits):

| flit | header | payload |
|---|---|---|
| AW | `axi_ch`=3'd0, `src_id`=8'h12, `dst_id`=8'h34, `flit_tail`=0, `ordering_req`=1, `ordering_tag`=8'h05 | `awid`=8'h07, `awaddr`=64'h0000_0000_0000_0200, `awlen`=8'd1, `awsize`=3'd5, `awburst`=2'd1 (INCR) |
| W beat 0 | `axi_ch`=3'd1, `flit_tail`=0 | `wlast`=0, `wstrb`=32'hFFFF_FFFF, `wdata`=beat 0 |
| W beat 1 | `axi_ch`=3'd1, `flit_tail`=1 | `wlast`=1, `wstrb`=32'hFFFF_FFFF, `wdata`=beat 1 |

Depacketize admits the AW: downstream id = `remap(8'h07, 1)` = 8'hFF, MetaBuffer write bucket 8'hFF gets `{src_id=8'h12, upstream_id=8'h07, ordering_req=1, ordering_tag=8'h05}`, write pool count 0 to 1.

AXI master face issues: `awid`=8'hFF, `awaddr`=64'h200, `awlen`=8'd1, `awsize`=3'd5, then 2 W beats, `wlast` on the second. The slave responds `bvalid` with `bid`=8'hFF, `bresp`=2'b00.

Packetize peeks write bucket 8'hFF, builds the single B flit: header `axi_ch`=3'd3, `src_id`=8'h34, `dst_id`=8'h12, `flit_tail`=1, `ordering_req`=1, `ordering_tag`=8'h05, payload `bid`=8'h07 (restored), `bresp`=2'b00, `buser`=8'h00. B round-robins, and the pointer starts at 0, so VC 0. On acceptance the MetaBuffer entry is committed, pool count back to 0.

### 2.6 Pipeline stages and latency

The model advances all stages once per clock in reverse order (later stages drain before earlier stages fill, `nsu/nsu.hpp` `Nsu::tick`), so a beat moves one stage per cycle, except the one zero-cycle hop called out below.

Request path stages (tick T = the posedge at which `noc_req_i` is sampled):

| tick | stage |
|---|---|
| T | ingress: flit demuxed on `axi_ch` into one of three 1-entry stage registers (AW, W, AR). W is decoded here, AW/AR are parked raw because admission needs their header fields. |
| T+1 | admission: `AxiMasterPort` pops at most 1 beat per channel from the stage registers into its per-channel FIFOs (depth `NSU_QUEUE_DEPTH` = 16). AW/AR admission performs the id remap and MetaBuffer allocation, and refuses while its own direction pool is full (a full write pool stalls AW only, AR is untouched, and vice versa). Same tick, the wrap moves the beat into the held output latch. |
| T+2 | the slave first samples `awvalid`/`arvalid` high (uncontended). |

The ingress is a single serialized stream from the router LOCAL port, VC-blind, with one head-of-line pending slot: if a flit's stage register is still occupied, that flit waits in the single `pending_` slot and everything behind it stalls. At most 1 flit per channel is parked per cycle (3 total when a backlog exists).

Response path stages (tick T = the posedge at which the B/R wire handshake is sampled):

| tick | stage |
|---|---|
| T | beat pushed into the port response FIFO (`b_q_`/`r_q_`, depth 16) |
| T+1 | forwarded into the Packetize stage register (at most 1 B + 1 R per cycle) |
| T+2 | flit built (MetaBuffer peek, commit on acceptance), pushed into the wormhole arbiter per-input pending queue (depth `NSU_ARBITER_FIFO_DEPTH` = 4) |
| T+3 | wormhole arbiter moves the flit to the VC arbiter per-VC pending queue (depth 4), and the VC arbiter drains it to the NoC output queue **in the same tick**. This wormhole-to-VC hop is zero-cycle. The wrap pops the flit the same tick. |
| T+4 | the router first samples `noc_rsp_o.valid` high (uncontended). |

Back-pressure chains, request: stage register occupied stalls ingress (no credit pulse returned, router LOCAL sender counter starves, router stops ejecting), MetaBuffer pool full stalls the AW or AR register, port FIFO full stalls admission, slave not ready holds the output latch. Response: no downstream credit holds the VC arbiter, pending queues fill back to `b_q_`/`r_q_`, `can_accept_b/r` goes false, `bready`/`rready` deassert, the slave holds (IHI 0022, A3.2.1).

## 3. Inputs and Outputs

### 3.1 NoC face (`nsu_wrap` ports)

> Pre-S3a: the single `noc_chan_t` face below predates the Stage 3a three-network split. As-built, `nsu_wrap` has three separate faces (REQ ingress, RSP egress, DAT ingress+egress) with per-network flit widths (REQ 137 b, RSP 127 b, DAT 629 b), scalar `rx_req_*`/`tx_rsp_*`/`tx_dat_*`/`rx_dat_*` ports, not one `noc_chan_t` pair. Re-synced in campaign Stage 5.

`noc_chan_t` = {`valid` (1 b), `flit` (`FLIT_WIDTH` = 408 b)}. `noc_credit_t` = {`credit` [`NUM_VC`-1:0]}. Elaboration fatals if `$bits(noc_credit_t)` != `NUM_VC`.

| Signal | Bit Width | Definition |
|---|---|---|
| `clk_i` | 1 | Clock. All sampling and registration on the positive edge. |
| `rst_ni` | 1 | Synchronous active-low reset. Given only once at the beginning of simulation. |
| `ctx_i` | 64 | Model handle from `cmodel_nsu_create`. Constant after time 0. |
| `noc_req_i` | 409 | From router LOCAL output. Request flit, valid 1 cycle per flit. No ready wire, flow control is pure credit: the router sends only while it holds sender credit, the NSU accepts every valid flit. Flit ignored while `valid` is low. |
| `noc_req_cred_o` | NUM_VC | To router. Consumer credit pulse vector: bit v pulses for exactly 1 cycle when the depacketizer consumed one request flit whose header `vc_id` = v. At most 1 pulse per VC per cycle. Replenishes the router LOCAL sender counter. |
| `noc_rsp_o` | 409 | To router LOCAL input. Response flit, `valid` high exactly 1 cycle per flit, at most 1 flit per cycle, back-to-back cycles legal. `flit` = 408'h0 while `valid` is low. |
| `noc_rsp_cred_i` | NUM_VC | From router. Credit pulse vector: bit v pulses when the router drained one NSU response flit from VC v. Replenishes the NSU per-VC sender counter (seed `NOC_ROUTER_VC_DEPTH` = 4). |

### 3.2 AXI master face (`axi_req_t` driven, `axi_rsp_t` consumed)

Widths from `ni_params_pkg`: ID 8, ADDR 64, DATA 256, WSTRB 32. Column From/To names the driver.

| Signal | Bit Width | From | Definition |
|---|---|---|---|
| `axi_req_o.awvalid` | 1 | NSU | High while an AW beat is presented. Held with stable fields until `awready` (IHI 0022, A3.2.1). 0 with all AW fields 0 when idle. |
| `axi_req_o.awid` | 8 | NSU | Downstream id, 8'hFF constant when `max_unique_ids` = 1, passthrough when 256. |
| `axi_req_o.awaddr` | 64 | NSU | From the AW flit payload, unmodified. |
| `axi_req_o.awlen` | 8 | NSU | Beats - 1. Unmodified. |
| `axi_req_o.awsize` | 3 | NSU | Unmodified. |
| `axi_req_o.awburst` | 2 | NSU | Unmodified. |
| `axi_req_o.awlock` | 1 | NSU | Unmodified. |
| `axi_req_o.awcache` | 4 | NSU | Unmodified. |
| `axi_req_o.awprot` | 3 | NSU | Unmodified. |
| `axi_req_o.awqos` | 4 | NSU | Unmodified. |
| `axi_req_o.awregion` | 4 | NSU | Tied to 4'h0. |
| `axi_req_o.wvalid` | 1 | NSU | High while a W beat is presented. Never rises for a beat whose owning AW has not completed its handshake (rule 9). |
| `axi_req_o.wdata` | 256 | NSU | One full flit data beat. |
| `axi_req_o.wstrb` | 32 | NSU | Unmodified. |
| `axi_req_o.wlast` | 1 | NSU | Unmodified from the W flit payload. |
| `axi_req_o.bready` | 1 | NSU | Context-gated pre-assert level, rule 10. Not qualified by `bvalid`. |
| `axi_req_o.arvalid` | 1 | NSU | As `awvalid`, for AR. |
| `axi_req_o.arid` .. `axi_req_o.arqos` | mirror of AW | NSU | Same semantics as the AW group. `arregion` tied to 4'h0. |
| `axi_req_o.rready` | 1 | NSU | Context-gated pre-assert level, rule 10. |
| `axi_rsp_i.awready` | 1 | slave | AW handshake when high with `awvalid`. |
| `axi_rsp_i.wready` | 1 | slave | W handshake when high with `wvalid`. |
| `axi_rsp_i.bvalid` | 1 | slave | B beat offered. Must be held until `bready` (input guarantee G6). |
| `axi_rsp_i.bid` | 8 | slave | Echo of the downstream awid. |
| `axi_rsp_i.bresp` | 2 | slave | Carried into the B flit unmodified. |
| `axi_rsp_i.arready` | 1 | slave | AR handshake when high with `arvalid`. |
| `axi_rsp_i.rvalid` | 1 | slave | R beat offered. Held until `rready`. |
| `axi_rsp_i.rid` | 8 | slave | Echo of the downstream arid. |
| `axi_rsp_i.rdata` | 256 | slave | Carried into the R flit unmodified. |
| `axi_rsp_i.rresp` | 2 | slave | Carried unmodified. |
| `axi_rsp_i.rlast` | 1 | slave | Carried into the R flit payload `rlast`. Triggers the MetaBuffer read commit. |

No `*user` and no `*region` signals cross this face in either direction.

### 3.3 DPI functions (`src/dpi/cmodel_dpi.cpp`)

| Function | Signature | Cycle semantics |
|---|---|---|
| `cmodel_nsu_create` | `unsigned long long cmodel_nsu_create(const char* name, int src_id, int num_vc, int max_unique_ids, int max_outstanding)` | Once at time 0 after `cmodel_init`. Returns the integer handle wired to `ctx_i`. `src_id` is the node coordinate id `(y << 4) \| x`. AXI channel FIFO depth is fixed at `NSU_QUEUE_DEPTH`. |
| `cmodel_nsu_set_inputs` | `void cmodel_nsu_set_inputs(ctx, noc_req_valid, noc_req_flit[13w], noc_rsp_credit_return[1w], awready, wready, bvalid, bid, bresp, arready, rvalid, rid, rdata[8w], rresp, rlast)` | First call on every non-reset posedge. Latches the sampled wires. |
| `cmodel_nsu_tick` | `void cmodel_nsu_tick(ctx)` | Second call. Advances the model exactly one clock. |
| `cmodel_nsu_get_outputs` | `void cmodel_nsu_get_outputs(ctx, noc_rsp_valid, noc_rsp_flit, noc_req_credit_return, awvalid, awid, awaddr, awlen, awsize, awburst, awlock, awcache, awprot, awqos, wvalid, wdata, wstrb, wlast, bready, arvalid, arid, araddr, arlen, arsize, arburst, arlock, arcache, arprot, arqos, rready)` | Third call. Results are registered nonblocking, visible on the wires from the next cycle. |

Marshalling: the 408-bit flit is 13 `svBitVecVal` words of little-endian bytes, 64-bit addresses split 32/32, 256-bit data is 8 words, the credit vector is 1 word with bit v = VC v. Handles are validated per call, a wrong-type or dead handle latches a DPI error polled centrally by `tb_top`.

### 3.4 Parameters

Single-sourced in `specgen/source/constants.yaml`, generated into `ni_params.h` / `ni_params_pkg.sv`, drift-gated at build by `codegen.py --check`.

| Parameter | Default | Legal range | Consumed at |
|---|---|---|---|
| `NSU_QUEUE_DEPTH` | 16 | 1 to 1024 | per-channel AW/W/AR/B/R FIFO depth in `AxiMasterPort` |
| `NSU_META_BUFFER_MAX_OUTSTANDING` | 32 | 1 to 256 | MetaBuffer shared pool, per direction |
| `NSU_META_BUFFER_MAX_UNIQUE_IDS` | 1 | {1, 256} only, constructor throws otherwise | id remap in Depacketize |
| `NSU_ARBITER_FIFO_DEPTH` | 4 | 1 to 64 | wormhole per-input and VC-arbiter per-VC pending depths |
| `NOC_DAT_NUM_VC` | 1 | 1 to 8 | DAT VC count, credit vector widths |
| `NOC_ROUTER_VC_DEPTH` | 4 | 1 to 16 | response sender credit seed per VC |
| `NOC_FLIT_WIDTH` | 408 | fixed at 408 in this implementation | flit container and DPI marshalling |
| `AXI_ID_WIDTH` / `AXI_ADDR_WIDTH` / `AXI_DATA_WIDTH` | 8 / 64 / 256 | fixed at defaults in this implementation | beat structs and DPI |
| create-time `src_id` | 0 | 8 bit | stamped into every response flit `src_id` |

The request ingress stage is a 1-entry register per channel plus the single pending slot. It has no configurable depth.

### 3.5 Protocol rules

1. **Input rhythm.** `noc_req_i` delivers at most one flit per cycle, each valid for exactly 1 cycle. Within one packet stream the AW flit precedes its W flits and the N W flits of an `awlen` = N-1 burst arrive in beat order (gaps allowed, example: AW at cycle 0, W0 at cycle 1, W1 at cycle 5 is legal). AR packets are single flits. Flits of different packets may interleave only at packet granularity per the fabric wormhole rules.
2. **Input idle state.** While `noc_req_i.valid` is low, `noc_req_i.flit` may carry any value and is ignored.
3. **Sampling edge.** All inputs are sampled at the positive edge of `clk_i` by the 3-call DPI sequence `set_inputs`, `tick`, `get_outputs`, in that order, every non-reset posedge. Outputs are registered at the same posedge and visible from the next cycle. The verification pattern checks outputs at the positive edge.
4. **Output valid behavior.** `awvalid`, `wvalid`, `arvalid`, once high, stay high with stable fields until the corresponding ready is sampled high (IHI 0022, A3.2.1). `noc_rsp_o.valid` is high exactly 1 cycle per flit, at most 1 flit per cycle, and may be high in consecutive cycles for distinct flits.
5. **Output idle value.** Every output field whose valid is low is 0. Example: with `awvalid` = 0, `awaddr` = 64'h0. `noc_rsp_o.flit` = 408'h0 while `noc_rsp_o.valid` = 0. `bready`/`rready` are policy levels (rule 10) and carry meaning while low.
6. **Reset.** `rst_ni` is synchronous active-low, asserted only once at the beginning of simulation. All `nsu_wrap` output registers clear to 0 during reset. Model state is initialized by `cmodel_nsu_create` at time 0. There is no mid-run reset.
7. **Gap and rate.** No minimum gap anywhere: request flits may arrive every cycle, response flits may leave every cycle, subject only to credit. Each credit pulse is exactly 1 cycle wide, at most 1 per VC per cycle on each credit port.
8. **Latency.** Request: from the posedge at which an AW (or AR) flit is sampled on `noc_req_i` to the posedge at which the slave first samples `awvalid` (`arvalid`) high is exactly 2 cycles when uncontended (empty queues, MetaBuffer pool not full, slave ready). Response: from the posedge at which the B/R wire handshake is sampled to the posedge at which the router first samples `noc_rsp_o.valid` high is exactly 4 cycles when uncontended (empty queues, sender credit available). Under contention the latency grows with backpressure and has no bound in this spec.
9. **W presentation budget.** `wvalid` never rises for a beat whose owning AW handshake has not completed. The budget counter `w_pop_budget_` (model type uint32) increments by `awlen` + 1 at each AW handshake and decrements by 1 per W beat presented. Example: AW with `awlen` = 8'd1 handshakes at cycle 2, budget 0 to 2, W beat 0 may be presented from cycle 3, never earlier.
10. **Context-gated ready pre-assert.** `bready` = (`outstanding_w_` > 0) and B FIFO has space. `rready` = (`expected_r_beats_` > 0) and R FIFO has space. Both are asserted without waiting for valid. `outstanding_w_` (uint32) increments when a `wlast` beat completes its W handshake and decrements at each B wire handshake. `expected_r_beats_` (uint32) increments by `arlen` + 1 at each AR handshake and decrements at each R beat wire handshake. Example: after one AR with `arlen` = 8'd3 handshakes, `expected_r_beats_` = 4 and `rready` stays high until 4 R beats have been accepted (given FIFO space). A B/R beat is accepted into the model only on a true wire handshake, valid together with the ready level the NSU drove in that cycle.
11. **Request credit.** One `noc_req_cred_o` pulse is returned for each flit the depacketizer ingress consumes, on the consumed flit's header `vc_id`, at most 1 per VC per cycle. Uncontended, the pulse for a flit sampled at posedge T is on the wire during the next cycle. A stalled ingress returns no pulses, which is the request-side backpressure mechanism.
12. **Response credit.** The NSU holds one sender credit counter per VC, seeded to `NOC_ROUTER_VC_DEPTH` = 4 at time 0. Sending a flit on VC v decrements counter v, a `noc_rsp_cred_i` pulse on bit v increments it. A flit is sent only while its counter is nonzero. Invariant: counter + flits in flight toward the router on that VC = 4 at all times.

### 3.6 Input guarantees

The implementer does not handle any of the following, they are guaranteed not to happen:

| # | Guarantee |
|---|---|
| G1 | Every `noc_req_i` flit has `axi_ch` in {3'd0, 3'd1, 3'd2}. The model asserts and aborts otherwise. |
| G2 | Every `noc_req_i` flit is addressed to this node. The fabric delivers only matching `dst_id`, the NSU never checks it. |
| G3 | Header `rsvd` [55:32] is 0 in every flit. |
| G4 | The router sends a request flit only while it holds sender credit, so ingress occupancy is bounded by the credit window. No overflow handling at `noc_req_i`. |
| G5 | Every burst targets a single tile address window (validated by the source NI's address map at packetize time). The NSU never splits a burst. |
| G6 | The slave holds `bvalid`/`rvalid` and all response fields stable until the matching ready is sampled high (IHI 0022, A3.2.1). |
| G7 | The slave responds only to requests the NSU issued and completes same-id transactions in order (IHI 0022, A6.3), so every B/R beat matches its MetaBuffer bucket front. The model asserts and aborts on a missing entry. |
| G8 | Downstream credit is truthful: a VC reported as having credit accepts the push. The model asserts and aborts otherwise. |
| G9 | Reset is given once, before traffic. No mid-run reset. |
| G10 | Since W flits carry no id, the fabric wormhole serialization guarantees every W flit follows its AW flit on the same stream. |

## 4. Specifications

Each item names its verification and failure condition. ctest paths are under `src/c_model/tests/`. "Co-sim scoreboard" means the per-transaction write-then-readback compare of the generated testbench (`make sim`), where any lost, duplicated, or corrupted beat fails the run.

1. Module name `nsu_wrap`, ports exactly per 3.1/3.2, DPI functions exactly per 3.3. Verify: elaboration (credit-width `$fatal` guard) and `wrap/test_cmodel_dpi.cpp` (handle type guards, create-after-init). Fail: elaboration fatal or DPI error latch raised.
2. One flit maps to exactly one AXI beat in both directions, no burst split or merge. Verify: `nsu/test_nsu_depacketize.cpp` `DemuxMixedAwWAr`, co-sim scoreboard. Fail: beat count differs from flit count for any transaction.
3. Request beats are presented to the slave in NoC arrival order per channel, regardless of AXI id. Verify: `nsu/test_nsu_depacketize.cpp` `FifoOrderPreservedAcrossChannels`. Fail: any beat overtakes an earlier beat of the same channel.
4. Request ingress is one serialized VC-blind stream with a single head-of-line pending slot and a 1-entry stage register per channel. A stalled channel blocks flits behind it, never reorders or drops them. Verify: `nsu/test_nsu_depacketize.cpp` `PendingHolBlockingWFullBlocksAwBehind`. Fail: a blocked flit is dropped or a later flit passes it.
5. AW/AR admission captures `{src_id, upstream_id, ordering_req, ordering_tag}` keyed by downstream id, within a shared per-direction pool of `NSU_META_BUFFER_MAX_OUTSTANDING` entries. A full pool reports backpressure, never aborts. Verify: `nsu/test_nsu_depacketize.cpp` `AwFlitSnapshotsMetadataAndPopsBeat`, `ArFlitSnapshotsReadMeta`, `nsu/test_meta_buffer.cpp` `SharedPoolFullReportsInsteadOfAborting`. Fail: wrong tuple stored or admission proceeds past a full pool.
6. Admission gating is per direction: a full write pool stalls AW only, a full read pool stalls AR only, W is never gated by either pool. Verify: `nsu/test_nsu_depacketize.cpp` `WFlitNoMetaSideEffect`, co-sim regression under write-hotspot patterns (reads keep progressing). Fail: cross-channel stall.
7. Id remap: `max_unique_ids` = 1 presents constant 8'hFF downstream, 256 presents the upstream id, any other value is rejected at construction with a throw that survives NDEBUG. Verify: `nsu/test_meta_buffer.cpp` `CollapsesToAllOnesWhenSingleUniqueId`, `IdentityWhenFullIdSpace`, `nsu/test_nsu_depacketize.cpp` `CtorRejectsIntermediateMaxUniqueIds`. Fail: wrong downstream id or an illegal configuration constructs.
8. Every response flit restores the original upstream id into `bid`/`rid`, sets `dst_id` to the captured requester `src_id`, and sets `src_id` to the NSU node id. Verify: `nsu/test_nsu_packetize.cpp` `PushBLooksUpMetaAndEmitsFlit`, `RPayloadBitPerfect`, `nsu/test_nsu.cpp` `WriteRoundTripDecodesReqFlitsAndProducesBRspFlit`, co-sim scoreboard. Fail: any field differs.
9. MetaBuffer commit discipline: the bucket front is matched, a write entry is retired when the B flit is accepted by the arbiter (not before, not on a refused push), a read entry is retired only on the accepted `rlast` beat, every earlier burst beat peeks the same entry. Verify: `nsu/test_nsu_packetize.cpp` `PushRMultiBeatPeekUntilRLast`, `PushBNoCommitOnNocFull`, `nsu/test_meta_buffer.cpp` `MultiOutstandingSameIdFifoOrder`. Fail: early or skipped commit, wrong entry consumed.
10. Header `flit_tail` = 1 on every B flit and every R beat flit, burst framing only in payload `rlast`. Verify: `nsu/test_nsu_packetize.cpp` flit field checks, co-sim (a `flit_tail` = 0 response flit wedges downstream wormhole arbitration and hangs the run). Fail: any response flit with header `flit_tail` = 0.
11. `ordering_req` and `ordering_tag` are echoed verbatim from the matching request into every response flit, including every beat of a multi-beat R burst. Verify: `nsu/test_nsu_packetize.cpp` `MultiBeatR_AllFlitsCarrySameOrderingTag`. Fail: any beat carries a different value than its request.
12. VC selection implements the 2.4 table exactly, including refuse-not-spill for the hashed R flits, the `num_vc` = 1 degenerate path (everything on VC 0, no entry-time credit check), and header `fixed_vc` (R = 1, B = 0). Verify: `nsu/test_nsu_vc_allocator.cpp` `RBurstStaysOnOneVc`, `RobbedRBurstStaysOnOneVcToo`, `DistinctRidsSpreadAcrossVcs`, `SameRidDifferentDstYieldsDistinctVcs`, `SameBidRoundRobins`, `FixedVcFullRefusesInsteadOfSpilling`, `FixedVcStampedOnRNotB`, `Nsu_Degenerate_NumVc1_Passthrough`. Fail: flit observed on any VC other than the rule's choice, or wrong fixed_vc bit.
13. Response arbitration sends at most 1 flit per cycle, round-robin between B and R at the wormhole stage and round-robin over VCs at the drain, no starvation of a nonempty input. Verify: `router/test_wormhole_arbiter.cpp` for the B/R stage, `nsu/test_nsu_vc_allocator.cpp` `Nsu_Degenerate_NumVc1_Passthrough` for the one-flit-per-tick drain, and co-sim link monitors for the multi-VC drain order; no ctest pins the multi-VC drain round-robin at the NSU. Fail: 2 flits in one cycle or a nonempty input never served while the other drains.
14. Credit conformance per rules 11 and 12, including the seed-4 sender invariant. Verify: co-sim link monitors and the model abort on downstream credit lies (G8). Fail: counter divergence, pulse without a consumed/drained flit, or flit sent without credit.
15. AXI master face conformance: held-valid on AW/W/AR (rule 4), W budget (rule 9), context-gated `bready`/`rready` with handshake-qualified acceptance (rule 10). Verify: co-sim scoreboard (a violation surfaces as a lost or duplicated beat and a readback mismatch or hang). Fail: readback mismatch, hang, or a valid deasserted before ready.
16. Reset behavior per rule 6: every output 0 in the first cycle after `rst_ni` deasserts, no X. Verify: co-sim waveform at simulation start. Fail: any nonzero or unknown output before the first request completes the pipeline.
17. Uncontended latency exactly 2 cycles (request, flit to AXI valid) and exactly 4 cycles (response, handshake to `noc_rsp_o.valid`) per rule 8, with the zero-cycle wormhole-to-VC hop of 2.6. Verify: co-sim waveform inspection of an isolated transaction. Fail: valid earlier or later than the specified edge in the uncontended case.
18. All parameters of 3.4 are consumed from the generated headers, never redefined locally. Verify: `codegen.py --check` build gate. Fail: drift gate error at build.

## 5. Block Diagram

```
 tb_top (generated per topology)
 ┌────────────────────────────────────────────────────────────────────────────┐
 │  router_wrap (LOCAL port)          nsu_wrap / NsuWrap / Nsu        SV AXI  │
 │                                                                    slave   │
 │  REQ path                                                                  │
 │  noc_req_i ──> ingress ──> Depacketize ──────> AxiMasterPort ──> axi_req_o │
 │  (1 flit/cyc)  (1 pending  S1 regs: aw|w|ar    aw_q_ w_q_ ar_q_  held_aw_  │
 │                 slot, HOL)  (1 entry each)     (depth 16 each)   held_w_   │
 │       ▲              │            │                              held_ar_  │
 │  noc_req_cred_o <────┘            │ allocate on pop_aw/pop_ar    (A3.2.1)  │
 │  (pulse/VC)                       ▼                                        │
 │                          MetaBuffer {src_id,id,ordering_req,ordering_tag}  │
 │                          write pool 32 | read pool 32                      │
 │                                   ▲                                        │
 │  RSP path                         │ peek/commit (restore id, dst_id)       │
 │  noc_rsp_o <── VcAllocator <── WormholeArbiter <── Packetize <── b_q_ r_q_ │
 │  (1 flit/cyc)  per-VC        2-in RR, B=0 R=1    s1_b_ s1_r_   (depth 16)  │
 │                pending x4    pending x4/input    (1 entry)         ▲       │
 │  noc_rsp_cred_i ─> sender credit counters                     axi_rsp_i    │
 │  (pulse/VC)        (seed 8 per VC)                            (B/R beats)  │
 └────────────────────────────────────────────────────────────────────────────┘
   Verification: c_model scoreboard write-then-readback compare + model asserts
```

## 6. Sample Waveform

2-beat write of 2.5, ideal slave (`awready`/`wready` tied high, B one cycle after `wlast`). Column N is the wire level during clock cycle N. A signal driven in cycle N is sampled by its consumer at the posedge ending cycle N, and `nsu_wrap` registers its DPI outputs at that posedge, so an input in cycle N first affects an output in cycle N+1.

```
cycle               0    1    2    3    4    5    6    7    8    9   10
noc_req_i.valid    ┌AW─┐┌W0─┐┌W1─┐__________________________________________
noc_req_cred_o[0]  _____┌──┐_┌──┐_┌──┐______________________________________
                        AW$  W0$  W1$   1 pulse per consumed flit, 1 cy wide
axi awvalid        __________┌───┐__________________________________________
axi awready        ──────────────────────── (ideal slave, tied high)
axi wvalid         _______________┌────────┐________________________________
axi wlast          ____________________┌───┐________________________________
axi bready         _________________________┌────────┐______________________
axi bvalid (slave) ______________________________┌───┐______________________
axi bid   (slave)  ______________________________ FF _______________________
noc_rsp_o.valid    ______________________________________________________┌───┐
noc_rsp_o.flit     ______________________________________________________ B__
                        │         │                   │                   │
   AW flit cycle 0 ─────┘         │                   │                   │
   awvalid cycle 2 = flit + 2 ────┘ (rule 8)          │                   │
   B wire handshake cycle 6 ──────────────────────────┘                   │
   noc_rsp_o.valid cycle 10 = handshake + 4 ──────────────────────────────┘
```

Annotations:

- `noc_req_cred_o[0]` pulses in cycles 1, 2, 3, one cycle after each flit is consumed (rule 11). All three flits arrived on VC 0.
- `awvalid` in cycle 2, exactly 2 cycles after the AW flit (rule 8). With `awready` high it is a 1-cycle presentation.
- `wvalid` first rises in cycle 3, one cycle after the AW handshake in cycle 2, never earlier: the W budget went 0 to 2 at the AW handshake (rule 9). W0 in cycle 3, W1 with `wlast` in cycle 4, back to back.
- `bready` rises in cycle 5, after the `wlast` handshake made `outstanding_w_` = 1, and without waiting for `bvalid` (rule 10). It falls in cycle 7 after the B handshake in cycle 6 returned `outstanding_w_` to 0.
- The B wire handshake in cycle 6 (bvalid and bready both high) starts the response pipeline: `b_q_` end of 6, Packetize S1 end of 7, wormhole pending end of 8, VC pending and NoC output queue end of 9 (zero-cycle hop, 2.6), `noc_rsp_o.valid` in cycle 10. The flit carries `bid` = 8'h07, `dst_id` = 8'h12, header `flit_tail` = 1, VC 0. The VC 0 sender credit counter went 4 to 3 at end of cycle 9 and returns to 4 when `noc_rsp_cred_i[0]` later pulses.
