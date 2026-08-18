# Collective-Capable AXI4 NoC Target Specification

Revision 0.2, 2026-07-23, draft.

## 1. Overview

A collective-capable, AMBA AXI4-compliant NoC for transformer attention accelerators. One
compute tile per node on a 2D mesh, standard AXI4 at every endpoint.

**Highlights**

- **Hardware multicast** on the write path. Mesh rows share a `Q` operand, columns share a
  `K/V` operand, one injection serves them all: up to **15x** less source-port traffic on a
  16-node set
- **Write-response aggregation.** One AXI response per multicast write, error `BRESP` wins
- **Traffic-class isolation.** Three physical networks, bulk bursts never queue ahead of
  control messages
- **AXI4 endpoints.** Unicast traffic unchanged, a collective write encodes operation and mask
  in `AWUSER`

**Heterogeneous traffic, one network per class**

| Class | Traffic | Properties | Network |
|---|---|---|---|
| High bandwidth | `Q`, `K`, `V` multicast, activations, KV cache blocks | Burst-based, up to 4 KB, latency tolerant | `DAT`, 512 b data plane, high utilization |
| Latency sensitive | Softmax statistics `m` and `l`, synchronization, scheduler start and done | Single-beat messages | Narrow 64 b control plane on `REQ` and `RSP`, low utilization |
| Auxiliary | Data-class requests `DataAr`, acknowledgments `DataB`, the merged multicast `B` | Small fixed-size flits | `REQ` and `RSP` |

---

## 2. Key Features

- 512 b data, **64 GB/s @ 1 GHz**
- Header overhead **7 %**
- Three physical networks `REQ` / `RSP` / `DAT`
- `REQ`/`RSP` full-duplex, W/R utilization **81.4 %** on `DAT`
- Support AXI Outstanding / Interleaving / Out-of-Order
- 2x2 to 16x16 mesh, 256 nodes
- Write multicast / Response reduction support
- 1 to 8 virtual channels on `DAT`, default 2, with selectable shared or equal Read/Write-split
  allocation; credit-based on `DAT`, ready/valid on `REQ` / `RSP`
- XY routing on all three networks, wormhole switching for the one multi-flit packet type
  (`AW`+`W`; every response packet is single-flit). Deadlock-free for unicast and
  non-overlapping multicast trees; concurrent overlapping multicasts are serialized by
  software (see Scope)
- GALS, per-endpoint clocks

**Scope.**

- A multicast covers an aligned submesh, not an arbitrary node set. Software assigns it, the
  fabric provides the primitive
- Two multicasts whose spanning trees share a node may not be in flight together. Software
  either assigns disjoint trees or waits for the merged `B` before issuing the overlapping one
- Collectives are issued and received by mesh tiles. A peripheral neither issues one nor appears
  in a destination set, and reaches the mesh by unicast only
- Write path only. Reads and read data are always unicast
- No arithmetic on payload anywhere in the fabric. Tiles combine partial results, softmax
  statistics included, over ordinary unicast

---

## 3. Architecture

- Each node holds one compute tile and its local memory, so it is both an AXI master and an AXI slave
- XY routing fixes both the path a multicast takes and the path its merged response is aggregated
  along. The two are different trees: the request spreads from the issuer, the response aggregates
  along each member's own route back to it

~~~
                 K, V column-wise multicast
                             │
                             ▼
             x0          x1          x2          x3
          ┌───────┐   ┌───────┐   ┌───────┐   ┌───────┐
    y0    │ Tile  │───│ Tile  │───│ Tile  │───│ Tile  │   ──▶ Q row-wise multicast
          └───┬───┘   └───┬───┘   └───┬───┘   └───┬───┘
              │           │           │           │
          ┌───┴───┐   ┌───┴───┐   ┌───┴───┐   ┌───┴───┐
    y1    │ Tile  │───│ Tile  │───│ Tile  │───│ Tile  │
          └───┬───┘   └───┬───┘   └───┬───┘   └───┬───┘
              │           │           │           │
          ┌───┴───┐   ┌───┴───┐   ┌───┴───┐   ┌───┴───┐
    y2    │ Tile  │───│ Tile  │───│ Tile  │───│ Tile  │
          └───┬───┘   └───┬───┘   └───┬───┘   └───┬───┘
              │           │           │           │
          ┌───┴───┐   ┌───┴───┐   ┌───┴───┐   ┌───┴───┐
    y3    │ Tile  │───│ Tile  │───│ Tile  │───│ Tile  │
          └───────┘   └───────┘   └───────┘   └───────┘

          every link, per direction, flit wires only
          default: REQ 136 b + RSP 126 b + DAT 633 b
          4 x 4 shown, 16 x 16 max
~~~

Inside one node:

~~~
      AXI clk domain       │        NoC clk domain, 1 GHz
                           │
   ┌────────────┐          │   ┌────────────────┐       ┌──────────────┐
   │  Compute   │   AXI4   │   │   Network      │       │    Router    │  REQ  ══▶
   │  tile      │ ────────────▶│   interface    │──────▶│  XY route    │
   │            │  addr 48 │   │                │       │  VC allocate │  RSP  ══▶
   │  (master)  │ ID 1..8  │   │  address map   │       │  replicate   │  DAT  ══▶
   │            │◀═════════════│  packetize     │◀──────│  merge       │  ◀══ 4 neighbours
   │            │          │   │  depacketize   │       │              │
   └────────────┘          │   │  response order│       │              │
   ┌────────────┐          │   │                │       │              │
   │  Local     │◀─────────────│                │◀──────│              │
   │  memory    │   AXI4   │   └────────────────┘       └──────────────┘
   │  (slave)   │ data 512 │
   └────────────┘          │
~~~

Physically separate networks, not virtual channels on a shared link:

- Independent buffering and arbitration per network, traded against wire count
- Separate `REQ` and `RSP` for forward progress: a node accepts responses while its own request
  path is blocked
- A dedicated wide `DAT` network for the payload channels: a whole beat and its header move in
  one cycle instead of serializing over a narrow link

| Physical link | Size | Narrow class | Data class |
|---|---|---|---|
| `REQ` | `133 + AXI_ID_WIDTH` b (default 136 b) | `Aw`, `Ar`: 48 b address. `W`: 64 b data | `Ar`: 48 b address |
| `RSP` | `123 + AXI_ID_WIDTH` b (default 126 b) | `R`: 64 b data. `B`: 2 b response | `B`: 2 b response |
| `DAT` | 633 b for `AXI_ID_WIDTH` 1..8 | - | `Aw`: 48 b address. `W`, `R`: 512 b data |

Physical widths are elaboration-time derivatives of the packet layout, not independent tuning
parameters. The 585-bit `DataW` payload is wider than `DataAw` and `DataR` throughout the legal
ID range, so DAT remains 633 bits while REQ and RSP track `AXI_ID_WIDTH`.

One `DAT` network, not a request and response pair, even though it carries request-direction
`DataW` and response-direction `DataR`:

- The `REQ`/`RSP` split is a correctness requirement, a node must accept responses while its
  own request path is blocked
- A `DAT` endpoint sinks every delivered flit, write data into slave-side buffers, read data
  into reorder-buffer space reserved at request issue, so the two payload flows form no
  blocking cycle
- A split would double the widest wires of the design for no correctness gain

**Network interface (NI).** Two units: NMU serves an AXI master, NSU serves an AXI slave.

- Owns the system address map (SAM): global config space and global memory space
- Address lookup selects the destination node and the AXI class, config space narrow, memory
  space data
- Unicast addresses are global end to end: the NMU names a destination and forwards `AxADDR`
  unchanged, and the NSU presents that same address to the AXI slave. For a multicast AW, each
  destination NSU overwrites only the address's node-coordinate field with its own coordinate;
  every other bit stays unchanged. No path subtracts the matched SAM region base. Address decode
  below the node boundary belongs to the endpoint, on the bases the map already assigned
- Packetizes, depacketizes, orders responses

**Router.** Route selection, multicast replication, response aggregation. No AXI address
decode, no arithmetic on payload.

*NMU (Network Master Unit).* AXI slave interface toward the AXI master, NoC initiator interface
toward the fabric.

![NMU block diagram](image/nmu.jpg)

- Request path: `AW`+`W` and `AR` buffers, round-robin arbitration with wormhole lock, SAM
  lookup, packetizer with VC allocation on `DAT`
- Response path: packet buffer, reorder buffer, depacketizer to `B` and `R`
- Same-ID ordering: same-destination streams bypass the reorder buffer and hold one VC end to
  end, different-destination streams reorder through it
- Reorder buffer: 8 KB SRAM, two responses at the AXI 4 KB maximum burst

*NSU (Network Slave Unit).* NoC target interface toward the fabric, AXI master interface toward
the AXI slave.

![NSU block diagram](image/nsu.jpg)

- Request path: packet buffer, ID compression, depacketizer to `AW`, `AR`, `W`
- Header FIFO keeps each transaction's header for response packetization
- Response path: `B` and `R` buffers, round-robin arbitration, packetizer with VC allocation on
  `DAT`
- ID compression fits a narrower downstream ID width. Streams sharing a compressed ID return in
  order

**Router modes.** Both route XY, replicate multicast, merge responses.

- Standard mode: credit-based flow control with virtual channels, 3 to 5 pipeline stages per
  hop, serves `DAT`
- Simple mode: ready/valid, single channel, 1 to 2 pipeline stages per hop, serves `REQ`
  and `RSP`
- Per node: one standard-mode router on `DAT`, one simple-mode router each on `REQ` and `RSP`

**Requirements on the fabric.**

| Requirement | Statement |
|---|---|
| AXI4 response contract | One `B` per `Aw`, and an error `BRESP` takes precedence when a multicast write is answered by several slaves |
| Ordering | Responses reach a master in AXI order within an ID in every configuration |
| Deadlock-free | XY routing removes routing cycles. The AXI message dependency is acyclic, `REQ -> DAT -> RSP`; DAT VC count does not provide this guarantee |
| Write data pairing | A slave pairs each write request with its own write data, whatever the mix of sources |
| Credit capacity | A credit-controlled receiver holds, per virtual channel, at least one flit of buffer for each cycle of buffer turnaround time. Below that capacity a link idles with no contention present |

---

## 4. Interfaces

| Unit | AXI interface | NoC interface |
|---|---|---|
| NMU | AXI slave interface | NoC initiator interface |
| NSU | AXI master interface | NoC target interface |
| Router | - | NoC link interface per port, one local and four directional (N, E, S, W) |

### 4.1 Global signals

| Signal | Source | Description |
|---|---|---|
| `ACLK` | Clock source | AXI domain clock. AXI port signals are sampled on its rising edge |
| `ARESETn` | Reset source | AXI domain reset, active low; asynchronous assert and synchronous deassert on `ACLK` |
| `noc_clk` | Clock source | NoC domain clock, 1 GHz target |
| `noc_rst_n` | Reset source | NoC domain reset, active low; asynchronous assert and synchronous deassert on `noc_clk` |

The two domains are asynchronous. The five existing AXI channel FIFOs are the CDC boundary; the NI
does not add a second complete-flit CDC stage. All SAM, ordering, packetization, depacketization,
channel assignment, VC selection and NoC class queues operate in `noc_clk` after this boundary.

System integration derives `ARESETn` and `noc_rst_n` from one common active-low system reset using
one reset synchronizer per clock domain. The synchronizers reside above the NI; NMU and NSU do not
have a `sys_rst_n` port. Assertion reaches both domains asynchronously. Deassertion is synchronized
independently, so release skew between the two domains is legal and the async FIFOs must tolerate
it. Independent AXI-domain or NoC-domain reset and one-sided reset recovery are unsupported. A
system reset clears all five AXI async FIFOs, all four NoC class FIFOs, ordering and RoB state, and
credit counters; in-flight transactions are discarded rather than preserved or replayed.

| AXI channel FIFO | NMU direction | NSU direction |
|---|---|---|
| AW | `ACLK` to `noc_clk` | `noc_clk` to `ACLK` |
| W | `ACLK` to `noc_clk` | `noc_clk` to `ACLK` |
| AR | `ACLK` to `noc_clk` | `noc_clk` to `ACLK` |
| B | `noc_clk` to `ACLK` | `ACLK` to `noc_clk` |
| R | `noc_clk` to `ACLK` | `ACLK` to `noc_clk` |

Each AXI interface therefore has five dual-clock FIFOs. Their entries are AXI channel records, not
physical flits. `AXI_FIFO_DEPTH` is their common entry count. The NoC side has four logical
`noc_clk` class FIFOs: REQ, RSP, DAT Write and DAT Read. `NOC_FIFO_DEPTH`, default 8 with
legal values 4, 8 and 16, is their common entry count. They are not CDC FIFOs and are not
replicated per VC.

| NoC class FIFO | NMU direction | NSU direction | AXI objects |
|---|---|---|---|
| REQ | transmit | receive | Narrow AW/W/AR and Data AR |
| RSP | receive | transmit | all B and Narrow R |
| DAT Write | transmit | receive | Data AW/W |
| DAT Read | receive | transmit | Data R |

On injection, the AXI-to-NoC assigner selects REQ, RSP, DAT Write or DAT Read after packetization.
REQ and RSP use ready/valid. For DAT, sender-side Credit Management holds one counter per Router VC,
applies `NOC_DAT_VC_MODE`, selects an eligible credited VC, stamps `vc_id`, and decrements that
counter when the flit is sent. `DataW` inherits its owning `DataAw` VC through WLAST. The NI has no
per-VC queue; the selected Router LOCAL input VC FIFO owns the credited storage.

On ejection, the Router presents REQ, RSP and DAT to the NI with ready/valid. The NoC-to-AXI
assigner classifies the accepted flit and writes the corresponding AXI channel FIFO. It does not
demultiplex into VC queues, and `vc_id` has no NI queue-selection role after acceptance. DAT ready
is derived from the destination DAT Write or DAT Read class FIFO capacity.

At the NSU, REQ Narrow and DAT Data write flits enqueue independently before the one downstream
AXI AW/W interface. The NoC-to-AXI AW assignment selects the only admissible class or round-robins
when both are admissible. Every accepted AW appends `{class, burst_beats}` to a W-order FIFO. AW may
continue to accept later transactions while W serves the FIFO head class through its final beat;
if that class has no W beat ready, the other class cannot bypass it. This permits multiple AW
transactions to remain outstanding without mispairing or interleaving their W data.

The first RTL does not implement NoC QoS. It retains `AWQOS` and `ARQOS` on the AXI interface and
transports them in the AW and AR payloads, but `AxQOS` does not select a VC or affect router
arbitration. The NoC header remains 48 bits. A future DAT-only, best-effort priority design is
recorded in `trade-off.md`; it is not part of this target.

AW-order metadata selects the W path for the whole burst, and the physical-channel arbiter locks
from AW through its WLAST beat. The Read/Write DAT split therefore does not permit W interleaving
or a later write burst to bypass the active burst. The five independent AXI channel async FIFOs
provide the required channel concurrency before packetization; there is no additional CDC queue.
Within one AXI channel FIFO, Narrow and Data records remain in acceptance order, so a blocked head
may delay the other class until classification. Separate class FIFOs remove cross-class coupling
only after that point.

Narrow and Data writes share the external AXI AW/W channels through the async boundary and up to
classification and capture in `noc_clk`.
An AW-order context FIFO records each accepted AW's NoC class and route; every W beat follows the
context at the FIFO head, which retires on WLAST. After packetization, Narrow AW/W uses the REQ TX
pipeline and Data AW/W uses the DAT Write TX pipeline. Their channel assignment, class FIFO, flow
control and physical output are independent, so REQ and DAT may each emit one write flit in the same
`noc_clk` cycle. Each physical network has its own AW-to-WLAST lock. Backpressure on one network
must not stall the other after classification into the separate NoC class FIFOs.
Parallel drain may change arrival order across Narrow and Data address spaces, which are distinct
ordering domains. It must not reorder writes within one `{AXI ID, dst_id, dst_port_id, AXI class}`
domain. The NMU B ordering state returns same-ID responses in issue order.

At the NMU response side, B and R use independent async FIFOs and may assert `BVALID` and `RVALID`
in the same cycle. `DataB` on RSP and `DataR` on DAT have independent physical and class queues.
`NarrowB` and `NarrowR` share the single-flit RSP link and cannot traverse that link in the same
cycle, but once assigned to B and R neither AXI response channel waits on the other's handshake.

### 4.2 AXI channel signals

Each NMU and NSU exposes exactly one AXI4 interface. Both ports carry the same channel set at the
§5 widths, including a fixed 512-bit data bus; Narrow and Data are internal NoC traffic classes,
not separate AXI interfaces.

**Write address channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `AWID` | 3 | Master | Write address ID |
| `AWADDR` | 48 | Master | Write address |
| `AWLEN` | 8 | Master | Burst length |
| `AWSIZE` | 3 | Master | Burst size |
| `AWBURST` | 2 | Master | Burst type |
| `AWLOCK` | 1 | Master | Lock type |
| `AWCACHE` | 4 | Master | Memory type |
| `AWPROT` | 3 | Master | Protection type |
| `AWQOS` | 4 | Master | QoS identifier |
| `AWREGION` | 4 | Master | Region identifier |
| `AWUSER` | 58 | Master | User signal. 50 bits carry the collective attributes, see §6 |
| `AWVALID` | 1 | Master | Write address valid |
| `AWREADY` | 1 | Slave | Write address ready |

**Write data channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `WDATA` | 512 | Master | Write data. A Narrow transfer carries the addressed 64 b lane in the Narrow NoC payload; a Data transfer carries all 512 b |
| `WSTRB` | 64 | Master | Write strobes. Narrow packetization selects the 8 strobes for the addressed 64 b lane |
| `WLAST` | 1 | Master | Write last, the last transfer in a write burst |
| `WUSER` | 8 | Master | User signal |
| `WVALID` | 1 | Master | Write data valid |
| `WREADY` | 1 | Slave | Write data ready |

**Write response channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `BID` | 3 | Slave | Write response ID |
| `BRESP` | 2 | Slave | Write response |
| `BUSER` | 8 | Slave | User signal |
| `BVALID` | 1 | Slave | Write response valid |
| `BREADY` | 1 | Master | Write response ready |

**Read address channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `ARID` | 3 | Master | Read address ID |
| `ARADDR` | 48 | Master | Read address |
| `ARLEN` | 8 | Master | Burst length |
| `ARSIZE` | 3 | Master | Burst size |
| `ARBURST` | 2 | Master | Burst type |
| `ARLOCK` | 1 | Master | Lock type |
| `ARCACHE` | 4 | Master | Memory type |
| `ARPROT` | 3 | Master | Protection type |
| `ARQOS` | 4 | Master | QoS identifier |
| `ARREGION` | 4 | Master | Region identifier |
| `ARUSER` | 8 | Master | User signal |
| `ARVALID` | 1 | Master | Read address valid |
| `ARREADY` | 1 | Slave | Read address ready |

**Read data channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `RID` | 3 | Slave | Read ID tag |
| `RDATA` | 512 | Slave | Read data. A Narrow response is restored into its addressed 64 b lane; a Data response carries all 512 b |
| `RRESP` | 2 | Slave | Read response |
| `RLAST` | 1 | Slave | Read last, the last transfer in a read burst |
| `RUSER` | 8 | Slave | User signal |
| `RVALID` | 1 | Slave | Read data valid |
| `RREADY` | 1 | Master | Read data ready |

### 4.3 NoC link interface

- Each link carries the three physical networks, each an independent signal group per direction.
- `REQ` and `RSP` use ready/valid flow control. A flit transfers when valid and ready are both high.
- DAT between routers uses per-VC credit flow control in both directions.
- The Router LOCAL DAT port is asymmetric: NI-to-Router injection uses per-VC credit because the
  Router input owns VC FIFOs; Router-to-NI ejection uses ready/valid because the NI owns only shared
  class FIFOs and no per-VC storage.
- Driven signals reset low.

The table below is the NI-facing contract. Directions are from the NI's view.

| Signal | Width | Direction | Description |
|---|---:|---|---|
| `TXREQFLIT` | `133 + AXI_ID_WIDTH` | Output | `REQ` transmit flit, header and payload |
| `TXREQVALID` | 1 | Output | `REQ` transmit valid |
| `TXREQREADY` | 1 | Input | `REQ` transmit ready, from the receiver |
| `RXREQFLIT` | `133 + AXI_ID_WIDTH` | Input | `REQ` receive flit |
| `RXREQVALID` | 1 | Input | `REQ` receive valid |
| `RXREQREADY` | 1 | Output | `REQ` receive ready |
| `TXRSPFLIT` | `123 + AXI_ID_WIDTH` | Output | `RSP` transmit flit, header and payload |
| `TXRSPVALID` | 1 | Output | `RSP` transmit valid |
| `TXRSPREADY` | 1 | Input | `RSP` transmit ready, from the receiver |
| `RXRSPFLIT` | `123 + AXI_ID_WIDTH` | Input | `RSP` receive flit |
| `RXRSPVALID` | 1 | Input | `RSP` receive valid |
| `RXRSPREADY` | 1 | Output | `RSP` receive ready |
| `TXDATFLIT` | 633 | Output | `DAT` transmit flit, flit type in `axi_ch`, see §6 |
| `TXDATVALID` | 1 | Output | `DAT` transmit valid |
| `TXDATCRDVALID` | `DAT_NUM_VC` | Input | Router input-FIFO credit return, one pulse per freed VC slot |
| `RXDATFLIT` | 633 | Input | `DAT` receive flit |
| `RXDATVALID` | 1 | Input | `DAT` receive valid |
| `RXDATREADY` | 1 | Output | NI class-FIFO capacity for Router-to-NI DAT ejection |

For NI-to-Router DAT injection, Credit Management initializes every sender counter from
`NOC_ROUTER_VC_DEPTH`, the actual depth of the Router LOCAL input VC FIFO. A transfer consumes one
credit for the selected VC; a `TXDATCRDVALID` pulse restores one credit when the Router drains that
VC slot. The NI must not transmit with a zero counter, and a counter must not underflow or exceed
the Router depth.

For Router-to-NI DAT ejection, a transfer occurs only when `RXDATVALID` and `RXDATREADY` are both
high. No NI receive-credit counter or `RXDATCRDVALID` exists. Inter-router DAT ports retain the
symmetric per-VC credit signals specified by the Router.

The five AXI channel async FIFOs use `AXI_FIFO_DEPTH`, default 8 with legal values 4, 8 and 16.
The implementation derives Gray-pointer widths from this entry count. These FIFOs absorb clock-ratio
variation and temporary AXI backpressure; they do not define the number of outstanding AXI
transactions. Outstanding capacity is owned separately by the per-ID order lists, RoBs and NSU
Response Queue.

---

## 5. Attributes and Configuration

**Fixed attributes**

| Attribute | Value | Comments |
|---|---|---|
| Address width | 48 b | - |
| `AWUSER` width | 58 b | 50 bits hold collective attributes, see §6 |
| `ARUSER`, `WUSER`, `RUSER`, `BUSER` width | 8 b | - |
| Narrow class data width | 64 b | - |
| Data class data width | 512 b | - |

**Configuration options**

The authored configuration is one YAML file. Before RTL elaboration, the project generator emits
its topology, SAM entries and coordinate metadata into `topology_pkg.sv`; the NI and
router consume those values as elaboration-time constants. Synthesizable RTL does not parse YAML
and has no CSR path for changing the SAM at runtime. The C++ reference model loads the same YAML
at simulation startup, and the verification flow checks the generated and runtime views for parity.

| Feature | Parameter | Values (default) | Comments |
|---|---|---|---|
| Topology | Mesh X and Y dimension | 2-16 (4) | Square meshes only. 256 nodes maximum, set by the 8-bit node ID |
| AXI interface | Endpoint interfaces | 1 | One fixed 512-bit interface carries both classes; the SAM address space selects the internal NoC class |
| AXI interface | `AXI_ID_WIDTH` | 1-8 (3) | NMU AXI ID and NoC-carried ID width; REQ/RSP flit widths derive from it at elaboration |
| Flow control | `DAT_NUM_VC` | 1-8 (2) | Total DAT VC count and the Section 4.3 credit signal width; mode-specific legality applies below |
| Flow control | `NOC_DAT_VC_MODE` | `SHARED`, `READ_WRITE_SPLIT` (`SHARED`) | System-wide eligible-VC policy for NI allocators and DAT router VA |
| Flow control | `NOC_ROUTER_VC_DEPTH` | 1-16 (8) | Router DAT input FIFO depth and NI-to-Router sender-credit seed |
| CDC | `AXI_FIFO_DEPTH` | 4, 8, 16 (8) | Common entry count of the AW/W/AR/B/R dual-clock FIFOs on each AXI interface |
| NoC class queues | `NOC_FIFO_DEPTH` | 4, 8, 16 (8) | Common entry count of the REQ/RSP/DAT Write/DAT Read synchronous FIFOs |
| Ordering | Outstanding transactions per ID | 1-32 (32) | Applies to both R modes. A master holds at most `32 x 2^AXI_ID_WIDTH` in total, 256 at the default; Enabled requests that require reordering additionally reserve an `ordering_tag`, see §6 |
| Ordering | `READ_ROB_ENABLED` | enabled, disabled (enabled) | Selects the R path at elaboration with `generate if`. Disabled permits a same-ID streak only within one `{dst_id, dst_port_id, AXI class}` ordering domain. B always uses a per-ID metadata-only RoB. The §3 ordering requirement holds in either setting |
| Address map | SAM address spaces | config, memory | Config space selects the narrow class, memory space the data class. Uniform across nodes and fixed for one elaborated RTL image |
| Address map | Space region size | power of two | - |
| Address map | Destination decode | table | First-match range lookup returns `dst_id`, `dst_port_id` and AXI class. Offset decode is a deferred area optimization, not part of the current RTL target. See §5.1 |
| NMU timing | `AW_SAM_REG_TYPE` | 0, 1, 2 (0) | AW SAM-output slice: bypass, simple register, or full skid buffer |
| NMU timing | `AR_SAM_REG_TYPE` | 0, 1, 2 (0) | AR SAM-output slice, independent of AW |

`NOC_DAT_VC_MODE=SHARED` allows `DAT_NUM_VC` from 1 to 8 and lets `DataAw`, `DataW` and `DataR`
use every VC. `NOC_DAT_VC_MODE=READ_WRITE_SPLIT` requires an even `DAT_NUM_VC` in {2, 4, 6, 8};
the lower half serves `DataAw`/`DataW` and the upper half serves `DataR`. The NI allocator and every
DAT router output VA apply this same class mask. Illegal combinations fail elaboration.

`DataW` inherits the VC selected for its owning `DataAw` in both modes. The modes change only the
eligible Router VC set: the NI still has one DAT Write and one DAT Read class FIFO and no per-VC
queue. One shared VC remains protocol-deadlock-free. Split mode reduces cross-class contention in
the Router but may strand Router VC capacity under asymmetric traffic. The first RTL does not map
`AxQOS` to these VCs and uses no QoS-aware arbitration.

### 5.1 Address map requirements

`routing.use_id_table` must be `true`. The generator and C++ loader reject `false` so a config
cannot select an offset-decode path that the RTL does not implement.

A space the map declares must give every node exactly one region. A region is a node's
allocation, not an endpoint's: several endpoints may share one. A space that leaves a node
without one is rejected by both the RTL-package generator and the C++ model loader. A map may
omit a space entirely.

Four further conditions decide whether that space is also a collective target. Its regions must

- be equal in size across all nodes
- be a power of two in size
- be aligned to an integer multiple of that size
- be mapped at a uniform power-of-two stride in coordinate order

The node index then occupies a contiguous address field at `log2(node_stride)`, `clog2(x_dim)`
bits of X below `clog2(y_dim)` bits of Y, and a mask over that field names an aligned set of
nodes at one shared node-local offset. `node_stride` is the power-of-two spacing between adjacent
node regions within that space. It is uniform within one collective-capable space but may differ
between spaces. Where a space is packed with its stride equal to its region size the two coincide.
A space meeting all four conditions is a legal collective target. A space failing any of them is
a legal unicast target and not a collective target.

**Mesh dimensions are powers of two**, so the coordinate field is exactly as wide as the
dimension and every index a mask names is a node. The generator and C++ loader refuse anything
else.

**Peripherals.** A peripheral shares the coordinate of the router it hangs off and is told apart
by the port, `dst_port_id`: 0 is the tile on the router's LOCAL port, non-zero a boundary port.
Two things follow for the conditions above.

- A peripheral takes its own region, in its own address space, placed above the tile array rather
  than derived from a coordinate. That space declares no coordinate ranges, so it is a unicast
  target and never a collective target
- The conditions are read over the **tile spaces**. A peripheral's region may differ in size and
  alignment without costing a tile space its collective eligibility
- A collective's request address names the region its burst footprint is measured against, and that
  one measurement stands for every replica, so **every region in the space must be one size**. The
  uniform-aperture condition above is what holds it

**Class.** The address space a request falls in selects the AXI class, config space narrow and
memory space data. This is one compare per space, not per node, and it is independent of how the
destination is reached.

**Destination.** The NMU first-match range lookup returns the entry's `dst_id`, `dst_port_id` and
AXI class. Each tile space derives its own coordinate ranges from its declared stride; those ranges
support collective address-mask translation and may differ between spaces. A future offset decoder
would require one fixed coordinate field across the whole map, but the current target does not
implement that optimization.

The AW and AR decoders are parallel combinational range comparators. Their independent
elaboration-time `*_SAM_REG_TYPE` parameters select the boundary between decode and RoB admission:

| Value | Datapath | Zero-stall latency | Backpressure behavior |
|---:|---|---:|---|
| 0 | combinational bypass | no added cycle | ready may remain combinational |
| 1 | one output register | +1 cycle | one bubble may follow a stall |
| 2 | output plus skid register | +1 cycle | registered backpressure, sustains one request per cycle |

These are physical timing parameters, not topology properties, so they do not appear in the YAML.
W does not perform a SAM lookup and has no corresponding parameter. A register slice carries the
complete accepted beat and its decode metadata; while stalled it holds both unchanged.

---

## 6. Packet Format

**Flit header, 48 b.**

| Field | Bits | Width | Description |
|---|---|---|---|
| `axi_ch` | [3:0] | 4 | AXI channel of the flit.<br>0: `NarrowAw`, on `REQ`<br>1: `NarrowW`, on `REQ`<br>2: `NarrowAr`, on `REQ`<br>3: `NarrowB`, on `RSP`<br>4: `NarrowR`, on `RSP`<br>5: `DataAw`, on `DAT`<br>6: `DataW`, on `DAT`<br>7: `DataAr`, on `REQ`<br>8: `DataB`, on `RSP`<br>9: `DataR`, on `DAT` |
| `src_id` | [11:4] | 8 | Source node id, composed as `{y[3:0], x[3:0]}` |
| `dst_id` | [19:12] | 8 | Destination node id, same composition |
| `fixed_vc` | [20] | 1 | When asserted, the flit holds `vc_id` end to end and routers do not reallocate it. Set by the NI for ordered same-destination streams |
| `vc_id` | [23:21] | 3 | Virtual channel index, 0 to the configured VC count minus one |
| `flit_tail` | [24] | 1 | Indicates the last flit of a packet |
| `ordering_req` | [25] | 1 | When asserted, `ordering_tag` is valid |
| `ordering_tag` | [33:26] | 8 | Reorder slot handle, allocated per transaction. Bounds a master to 256 outstanding transactions |
| `collective_op` | [35:34] | 2 | Collective operation.<br>0: `UNICAST`, single destination, `collective_mask` zero<br>1: `MULTICAST`, replicate a request to the node set of `collective_mask`, merge its responses, error `BRESP` first<br>2-3: reserved<br>Write path only, `Ar` and `R` are always `UNICAST` |
| `collective_mask` | [43:36] | 8 | Names the multicast node set: wildcard over `dst_id` on a request, `src_id` on its response. Derived from the `AWUSER` address mask |
| `dst_port_id` | [45:44] | 2 | Which endpoint at `dst_id` receives. 0 is the tile on the router's LOCAL port |
| `src_port_id` | [47:46] | 2 | Which endpoint at `src_id` issued. The response is addressed back to it |

**Flit payload.**

The field tables below show the default `AXI_ID_WIDTH = 3` layout. For another legal value, the
generator changes each `id` field width and shifts the following fields; it then derives the
physical network widths from the resulting maximum payload. Software must consume generated field
positions rather than hard-code these default offsets.

`addr` in `Aw` and `Ar` carries the global `AWADDR` / `ARADDR` accepted by the NMU; the NMU does
not subtract the matched SAM region base. A destination NSU presents a unicast address unchanged.
For a multicast `Aw`, each destination NSU overwrites only the node-coordinate field with its own
coordinate before issuing `AWADDR`. `Ar` is always unicast.

`Aw`, 88 b:

| Field | Bits | Width |
|---|---|---:|
| `id` | [2:0] | 3 |
| `addr` | [50:3] | 48 |
| `len` | [58:51] | 8 |
| `size` | [61:59] | 3 |
| `burst` | [63:62] | 2 |
| `cache` | [67:64] | 4 |
| `lock` | [68] | 1 |
| `prot` | [71:69] | 3 |
| `region` | [75:72] | 4 |
| `qos` | [79:76] | 4 |
| `user` | [87:80] | 8 |

`user` carries `AWUSER[7:0]`, user defined. `AWUSER[57:8]` holds `collective_op` and `collective_mask`, consumed by the NMU at packetize time, not carried in the payload.

`Ar`, 88 b:

| Field | Bits | Width |
|---|---|---:|
| `id` | [2:0] | 3 |
| `addr` | [50:3] | 48 |
| `len` | [58:51] | 8 |
| `size` | [61:59] | 3 |
| `burst` | [63:62] | 2 |
| `cache` | [67:64] | 4 |
| `lock` | [68] | 1 |
| `prot` | [71:69] | 3 |
| `region` | [75:72] | 4 |
| `qos` | [79:76] | 4 |
| `user` | [87:80] | 8 |

`user` carries the full 8 b `ARUSER`.

`NarrowW`, 81 b:

| Field | Bits | Width |
|---|---|---:|
| `last` | [0] | 1 |
| `user` | [8:1] | 8 |
| `strb` | [16:9] | 8 |
| `data` | [80:17] | 64 |

`DataW`, 585 b:

| Field | Bits | Width |
|---|---|---:|
| `last` | [0] | 1 |
| `user` | [8:1] | 8 |
| `strb` | [72:9] | 64 |
| `data` | [584:73] | 512 |

`B`, 13 b:

| Field | Bits | Width |
|---|---|---:|
| `id` | [2:0] | 3 |
| `resp` | [4:3] | 2 |
| `user` | [12:5] | 8 |

`NarrowR`, 78 b:

| Field | Bits | Width |
|---|---|---:|
| `last` | [0] | 1 |
| `id` | [3:1] | 3 |
| `resp` | [5:4] | 2 |
| `user` | [13:6] | 8 |
| `data` | [77:14] | 64 |

`DataR`, 526 b:

| Field | Bits | Width |
|---|---|---:|
| `last` | [0] | 1 |
| `id` | [3:1] | 3 |
| `resp` | [5:4] | 2 |
| `user` | [13:6] | 8 |
| `data` | [525:14] | 512 |

**`AWUSER` layout, 58 b.** `ARUSER` is 8 b.

| Field | Bits | Width |
|---|---|---|
| `user` | [7:0] | 8 |
| `collective_op` | [9:8] | 2 |
| `collective_mask` | [57:10] | 48 |

The `AWUSER` mask is an address mask:

- A set bit marks the matching `AWADDR` bit as don't care, `n` set bits name `2^n` addresses
- Set bits are limited to the node-index field of the address, so the named addresses differ
  only in destination node. §5.1 gives the region conditions that put that field in place
- The node-index field sits at `log2(node_stride)` for the address's matched space. Different
  spaces may place it at different bits; the matched SAM rule supplies the corresponding X/Y
  ranges
- The NMU translates the address mask into the 8 b flit `collective_mask` at SAM lookup and
  rejects a mask outside these constraints. Nothing downstream of the NMU sees an address mask,
  or an address space: a router is given `dst_id`, `collective_mask` and the original global
  address in the AW payload, and performs no address arithmetic
- Every replica leaves the router with the same AW payload. Each destination NSU overwrites the
  coordinate field with its own node coordinate, so all non-coordinate bits, including the
  node-local offset, remain equal. Each destination aperture covers the full burst footprint

**Header overhead and W/R utilization at the default `AXI_ID_WIDTH = 3`.** Header overhead counts
the 48 b header over the flit width. W and R utilization count the AXI data field over the flit
width.

| Network | Header overhead | `W` utilization | `R` utilization |
|---|---:|---:|---:|
| `REQ` 136 b | 35.3 % | 47.1 %, `NarrowW` | - |
| `RSP` 126 b | 38.1 % | - | 50.8 %, `NarrowR` |
| `DAT` 633 b | **7.6 %** | 80.9 %, `DataW` | 80.9 %, `DataR` |

`DataAw` rides `DAT` for ordering, not size: AXI4 write data carries no ID, a slave pairs data
with requests by arrival order, so the request travels bundled ahead of its data. The cost is
one flit per write burst and amortizes over the burst length.

### 6.1 AXI4 Compliance and Ordering

The interface rules follow the [AMBA AXI and ACE Protocol Specification, IHI 0022H,
Chapters A3, A5 and A6](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/IHI0022H_amba_axi_protocol_spec.pdf).

- Full AXI4: INCR, WRAP, and FIXED bursts, narrow and unaligned transfers, `AxLOCK`,
  `AxCACHE`, `AxPROT`, `AxQOS`, `AxREGION`, `xUSER`
- `AxLOCK` is unicast only, a collective transaction is not an exclusive access
- `AWUSER`: 8 b opaque in the flit payload, 50 b collective attributes consumed by the NMU,
  the address mask translated into the flit header, a slave receives the 8 b alone
- Each AXI channel uses its own VALID/READY handshake. Once asserted, VALID and its payload remain
  stable until the handshake; a source does not wait for READY before asserting VALID
- The CDC partition permits independent channel handshakes and introduces no combinational path
  between `ACLK` and `noc_clk`
- AXI4 write data has no WID. Accepted AW descriptors define W association in AW acceptance order;
  an emitted AW locks its physical NoC channel through the matching WLAST beat
- A B response is not presented before both its AW handshake and final W/WLAST handshake complete
- Different IDs may complete in any order. B responses with one BID and R responses with one RID
  complete in issue order; AXI defines no B-to-R response ordering

---

## 7. Performance

### 7.1 Metrics

| Metric | Definition |
|---|---|
| Channel bandwidth | One flit per cycle per link direction, flit width x clock / 8 |
| Payload bandwidth | AXI payload the same link carries, data width x clock / 8 |
| Payload efficiency | Data beats over flits carried |
| Zero-load latency | Packet latency with no queueing and no contention |
| Sustained bandwidth | Payload bandwidth a channel delivers over a continuous stream |

All figures assume AXI and NoC on one 1 GHz clock, the 512 b data class, and 4 KB bursts.

### 7.2 Payload bandwidth

A physical network carries one flit per cycle per direction and a flit holds at most one beat of
AXI payload.

**AXI interface**, one 512 b port per endpoint:

- Physical `W`, `R` bus: 512 b @ 1 GHz = **64 GB/s** each
- Narrow useful payload: 64 b per beat, limited by `REQ` / `RSP` to **8 GB/s** each

**NoC channel**, per link direction:

- `REQ`: default flit 136 b @ 1 GHz = 17.0 GB/s channel, 8 GB/s payload
- `RSP`: default flit 126 b @ 1 GHz = 15.75 GB/s channel, 8 GB/s payload
- `DAT`: flit 633 b @ 1 GHz = 79.1 GB/s channel, **64 GB/s** payload

- `NarrowW` rides `REQ`, `NarrowR` rides `RSP`: concurrent.
- Each `DAT` direction is its own wire bundle, a node's write data outbound and its read data
  inbound, both at 64 GB/s. Flows sharing one direction split it.

### 7.3 Payload efficiency

A flit carries at most one beat, so the NoC supplies flits, not bits, and the channel bandwidth
above the payload bandwidth buys nothing. The service rate of an AXI channel is the smaller of its
beat rate and the flit rate of the network it maps to:

![Service rate formula and worked example](image/perf_service_rate.png)

- Write: `DataW` shares the `DAT` network with `DataAw`, one flit per burst, so an `N`-beat
  burst sustains `N/(N + 1)`, 98.5 % at 4 KB, a ceiling of 63.0 GB/s.
- Read: `DataR` has the response-direction `DAT` network to itself, 100 %, the full 64 GB/s.

Input buffering absorbs the `DataAw` flit of an isolated burst as one cycle of added latency. At
an offered rate above the write payload efficiency the deficit accumulates one beat per burst and
backpressure begins once it reaches the buffer depth, at any depth.

### 7.4 Zero-load latency

A packet crossing `H` hops traverses `H` links and passes `H + 1` routers, counting the source
router. Each network has its own router, so `t_router` is per network:

- `REQ` / `RSP`: simple-mode router, ready/valid, **1 to 2** pipeline stages per hop
- `DAT`: standard-mode router, multi-VC credit-based flow control, **3 to 5** pipeline stages
  per hop

![Zero-load latency formula](image/perf_zero_load.png)

### 7.5 Sustained bandwidth

In-order reads hold the full rate, **64 GB/s**. Out-of-order reads lift efficiency across
destinations, but the reorder-buffer size is their bottleneck: every reordered response
passes through it, and once its **8 KB** is fully allocated the next request waits.

Sustained bandwidth follows two cases, split by whether responses return before the
outstanding depth runs out:

![Sustained bandwidth formula](image/perf_sustained_bw.png)

**Write pays in the channel, not in reordering.**

- `Aw` shares `DAT` with its write data: **63.0 GB/s** at 4 KB bursts
- Reordering is free, a `B` response is one flit with nothing to buffer

**Read pays in reordering, not in the channel.**

- Read data owns the response-direction `DAT`: 100 % payload efficiency
- An out-of-order read reserves reorder-buffer space for its whole burst before it issues
- An in-order read within one `{dst_id, dst_port_id, AXI class}` ordering domain skips the buffer

Out-of-order read depth, 64 B per beat, against the 32-request cap and the 8 KB buffer:

| Burst size | Outstanding requests | Limited by | Pending data |
|---|---:|---|---:|
| 64 B | 32 | the 32-request cap | 2 KB |
| 256 B | 32 | both at once | 8 KB |
| 1 KB | 8 | the 8 KB buffer | 8 KB |
| 4 KB | 2 | the 8 KB buffer | 8 KB |

---

## 8. Evaluation Model

The evaluation platform is the Edge LPDDR PIM 3D scalable architecture. The NoC under test is the
router mesh and network interfaces of its NPU mesh region.

![Edge LPDDR PIM 3D scalable architecture](image/Edge%20LPDDR%20PIM%203D%20Scalable%20Architecture.jpg)

- Host control region: a scheduler CPU and a sequencer DMA stage data from LPDDR and external
  storage into the mesh.
- NPU mesh region: one tile per node. A tile holds an NPU, its controller, tile-local 3D DRAM,
  and a router.
- The 3D DRAM arrays occupy the top layers. The control and interface logic sits at the base.

Memory tiers, ordered from cold storage to compute:

| Tier | Location | Role |
|---|---|---|
| External storage | Host control region, loaded from Flash | Persistent model store, read once at model load |
| `LPDDR` | External DRAM behind the LPDDR interface | Capacity tier. Holds the model image and feeds weight staging |
| Host `3D DRAM` | Host control region | Bandwidth tier on the host side. KV cache offload target and sink of mesh output |
| Interface buffer | Host control region | Staging buffer between the external interfaces and the sequencer DMA |
| Local SRAM | Host control region | Scheduler working memory, holds DMA descriptors |
| Tile `3D DRAM` | Every mesh tile | Near-memory tier. Resident weights, the tile's `Q`, `K`, `V`, `O` blocks, and hot KV cache. Tile-local, so a source-tile load generates no NoC traffic |
| `L1` | Inside the NPU | Compute scratchpad. Sub-tiles, partial sums, and online-softmax state, filled by tiling with double buffering |

Data moves between tiers on four paths: weights, activations, KV cache, and results. Arrows
crossing a region boundary are NoC traffic on the marked network.

![Memory tier dataflow](image/memory_tier_dataflow.svg)

Attention scores never enter this flow: a score block is produced and consumed inside `L1` by
the online softmax, no tier holds it, no NoC traffic carries it.

### 8.1 Tiled attention dataflow

Three algorithms specify the dataflow: model-level execution flow (Algorithm 0) at the run
level, and the attention block it invokes as per-tile fetch (Algorithm 1) or multicast
(Algorithm 2), differing only in how shared operands reach the tile group.

**Attention block mapping.** A tile group computes one attention block: the score grid
`S = Q K^T` maps onto the mesh, rows share `Q`, columns share `K/V`.

![Attention block mapping onto a 4x4 tile group](image/attention_block_mapping.png)

**Model-level execution flow (Algorithm 0).** Weights load once into per-tile 3D DRAM and stay
resident, no layer fetches weights over the fabric (line 1). A layer puts two traffic classes on
the fabric:

- Feature pipeline: activations passed to the next layer, on the data plane (line 9).
- Scheduler control: per-group start and done messages, on the narrow control plane (line 8).

Layers map to tile groups round-robin as pipeline stages. A group holds the weights and KV cache
of its layers, so only the activations `X` cross group boundaries:

![Layer pipeline across tile groups](image/layer_pipeline.png)

After the last layer the output returns to host 3D DRAM, where the scheduler samples the next
token (line 11). Cold KV cache blocks evict to host 3D DRAM and reload on demand (line 6).

![Algorithm 0: Model-level execution flow on the tile array](image/algorithm0_model_flow.png)

Algorithms 1 and 2 differ in one place, how a shared slice reaches its consumers:

- Algorithm 1 (per-tile fetch): every consumer tile pulls the slice itself, `Gx` unicast reads per
  `Q` slice and `Gy` per `K/V` slice (lines 5 and 7).
- Algorithm 2 (multicast): the source tile reads its slice locally once and multicasts it, one
  transfer per slice (lines 5-6 and 8-9).

In both figures the comments mark plane use: `data` for the `Q`, `K`, `V`, `O` tensors, `narrow`
for the `m` and `l` statistics, `local read/write` for 3D DRAM accesses that generate no NoC
traffic.

**Algorithm 1  Per-tile fetch.**

![Algorithm 1: Per-tile fetch (unicast baseline) on a Gx x Gy tile group](image/algorithm1_per_tile_fetch.png)

**Algorithm 2  Multicast.**

![Algorithm 2: Multicast on a Gx x Gy tile group](image/algorithm2_multicast.png)

Multicast cuts the shared-operand transfers from `Gx` or `Gy` to one.

### 8.2 Multicast latency and traffic

The row and column multicasts of Algorithm 2 follow the model below, a multicast along one axis
over a set of `k` nodes. A submesh multicast runs in two phases, one per axis.

```text
  k        nodes in the participating row or column
  L        sub-tile payload length in flits, one beat per flit,
           sub-tile bytes / 64 on the data class, / 8 on the narrow class
  H_max    maximum hop count from the source to a member of the set
  s        source index in the set, 0 to k - 1
  t_wire, t_router  per-hop link and router delay, as in the zero-load latency model
```

Tail arrival at the farthest member of the set:

| Case | Latency |
|---|---|
| Repeated unicast | `(k - 1)L + H_max t_wire + (H_max + 1) t_router` |
| In-network multicast | `L + H_max t_wire + (H_max + 1) t_router` |

Traffic generated by one multicast, source at one end of the set:

| Quantity | Unicast | In-network | Ratio, `k = 4` | Ratio, `k = 16` |
|---|---|---|---:|---:|
| Source port flits | `(k - 1)L` | `L` per path | 3x | 15x |
| Flit hops | `L [s(s + 1)/2 + (k - 1 - s)(k - s)/2]` | `L(k - 1)` | 2x | 8x |

The transport term of latency is identical in both cases. Multicast replaces the `(k - 1)L`
serialization at one source port with a single sub-tile, so both the tail latency and the source
traffic fall with the set size. A source at one end of the set uses one path with `H_max = k - 1`. An
interior source uses one path per direction, so `H_max = floor(k/2)`, the port ratio halves, and the
flit-hop ratio drops to 1.33x at `k = 4` and 4.27x at `k = 16`.
