# Collective-Capable AXI4 NoC Target Specification

Revision 0.2, 2026-07-23, draft.

## 1. Overview

This specification defines an AXI4 network-on-chip for accelerators running tiled GEMM and
multi-head attention, one compute tile per node on a 2D mesh.

These workloads move the same data to many places. Every node in a row needs the same query (Q)
tile, and every node in a column the same key (K) and value (V) tiles. The output (O) tile is then
summed back across the row. On a unicast fabric each of these becomes a separate transfer per node,
and they all queue at one port. Control traffic waiting behind them is delayed with them.

A multicast delivers a tile to a whole row or column from a single write. A reduction sums the
output tiles on their way to the destination, so one result arrives instead of many. Bulk data runs
on a network of its own, so once injected a tile cannot hold up a control transaction.

---

## 2. Benefits

**Reduced operand distribution traffic.** In-network multicast replaces one unicast injection per
other node in the set with one injection total, reducing source-port traffic by a factor of 15 on a
16-node row when the source sits at one end of it.

**Reduced root-port traffic for output accumulation.** Accumulating the output tile across a row
sends one full tile per other node into a single port under unicast. In-network reduction leaves
one arrival at an end root, or two at an interior root.

**Control latency isolation from bulk data.** Tile payload never shares a network with control
traffic.

**AXI4 interface compatibility.** A master keeps its AXI4 channel set, handshake, and response
rules unchanged. Collective attributes ride in 10 of the 18 `AWUSER` bits.

---

## 3. Key Features

- 1 GHz NoC clock target
- Full AXI4 protocol support
- In-network multicast and reduction on the write path
- Three physical networks, `req`, `rsp` and `wide`, with no shared channel, buffer, or arbiter
- 64-bit narrow AXI class for control and synchronization, 256, 512, or 1024-bit wide class for
  bulk data
- One wide flit per AXI beat
- Wormhole switching, dimension-order (XY) routing, credit-based flow control
- Globally asynchronous, locally synchronous integration, endpoint clocks independent of the
  NoC clock

**Scope.** Collectives cover a full row or a full column, not an arbitrary node set and not the
whole mesh. The only reduction operator is addition on floating-point data, and the members of one
reduction agree that type out of band. Scalar exchanges smaller than an operand tile are left to
software over ordinary unicast.

---

## 4. Performance

Bandwidth below is arithmetic on the configuration parameters. Delivered throughput and latency
depend on traffic, routing delay and buffering, and follow from the models in this section once
those terms are measured.

**Payload bandwidth.** A channel carries one flit per cycle and a flit holds at most one data width
of AXI payload, so payload bandwidth is data width x 1 GHz / 8. The wide class is shown at 512 b.

| AXI traffic | Data width | Physical network | Payload bandwidth |
|---|---:|---|---:|
| Narrow req | 64 b | `req` | 8 GB/s |
| Narrow rsp | 64 b | `rsp` | 8 GB/s |
| Wide req | 512 b | `wide` | 64 GB/s |
| Wide rsp | 512 b | `wide` | 64 GB/s |

Request traffic carries write data, response traffic carries read data. Narrow req and rsp take
separate networks and run concurrently. Wide req and rsp share the `wide` network, so 64 GB/s is
their combined ceiling. Flit width and per-channel utilization are in §8.

**Off-chip access.** These workloads are bounded by off-chip memory bandwidth, and a row or column
of `k` tiles reuses one operand of size `L`. Under per-tile fetch every tile reads the operand from
memory. A collective reads it once and shares it on-chip. Where partial outputs would otherwise be
written to memory independently, a reduction sums the `k` partial tiles to one before writeback.

```text
                off-chip access    latency
per-tile fetch  k L                k L / B
collective      L                  L / B

  k  tiles in the participating row or column
  L  operand tile size
  B  off-chip memory bandwidth
```

The off-chip access and the off-chip-bandwidth term of latency each drop by `k`, since that term
tracks access on a memory-bound path. The remaining subsections size the on-chip cost the collective
adds in place of that access.

**Latency.** Zero-load latency is the floor reached with no queueing. A packet crossing `H` hops
traverses `H` links and passes `H` + 1 routers, counting the source router.

```text
T_zero_load = H t_wire + (H + 1) t_router + L

  H            Manhattan hop count from source to destination
  t_wire       link traversal delay per hop, in NoC cycles
  t_router     router pipeline latency per hop, in NoC cycles
  L            tile payload length in flits, injected one flit per cycle
```

Mean hop count in a `k x k` mesh is `2(k^2 - 1) / (3k)`, 2.5 at `k` = 4 and 10.625 at `k` = 16.

Tail arrival at the farthest member of a row or column collective:

| Case | Form |
|---|---|
| Repeated unicast | `(k - 1)L + H_max t_wire + (H_max + 1) t_router` |
| In-network multicast | `L + H_max t_wire + (H_max + 1) t_router` |
| In-network reduction | `L + H_max t_wire + (H_max + 1) t_router + H_max C_offload` |

```text
  k          nodes in the participating row or column
  L          tile payload length in flits, tile bytes / (W_data / 8)
  H_max      maximum hop count from the source or root to a member of its set, and the
             number of joins on the critical path of a reduction
  C_offload  combine round trip to the compute tile, in NoC cycles
```

The transport term is identical in all three. A collective replaces the `(k - 1)L` serialization at
one port with a single tile. A reduction is the shorter form while `H_max C_offload` stays below
`(k - 2)L`. At one end of the set `H_max` is `k - 1`, at the centre `⌊k/2⌋`.

**Collective traffic reduction.** Counting results on a row or column of `k` nodes.

| Quantity | Unicast | In network | Ratio, `k` = 4 | Ratio, `k` = 16 |
|---|---|---|---:|---:|
| Source or root network port flits | `(k - 1)L` | `L` per path | 3 x | 15 x |
| Flit hops | `L [s(s + 1)/2 + (k - 1 - s)(k - s)/2]` | `L(k - 1)` | 2 x | 8 x |

```text
  s  source or root index in the row or column, 0 to k - 1
```

Both ratios are for a source or root at one end of the set. An interior one is served by one path
per direction, which halves the port ratio and lowers the flit-hop ratio to 1.33 x at `k` = 4 and
4.27 x at `k` = 16. A reduction moves the root's traffic to the joining nodes rather than removing
it, since each join passes its operand to the compute tile and takes the combined result back
across its own local port.

---

## 5. Architecture

Each node holds one compute tile and its local memory, so it is both an AXI master and an AXI
slave. Each link carries three physical networks. XY dimension-order routing fixes the path a
collective takes, and a reduction retraces the multicast path in reverse.

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
    y1    │ Tile  │───│ Tile  │───│ Tile  │───│ Tile  │   ◀── O row-wise reduction
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
          req 165 b + rsp 139 b + wide 353 / 641 / 1217 b
          4 x 4 shown, 16 x 16 max
~~~

Inside one node:

~~~
      AXI clk domain       │        NoC clk domain, 1 GHz
                           │
   ┌────────────┐          │   ┌────────────────┐       ┌──────────────┐
   │  Compute   │   AXI4   │   │   Network      │       │    Router    │  req  ══▶
   │  tile      │ ────────────▶│   interface    │──────▶│  XY route    │
   │  + FPU     │  addr 64 │   │                │       │  VC allocate │  rsp  ══▶
   │  (master)  │  ID 8    │   │  address map   │       │  replicate   │  wide ══▶
   │            │◀═════════════│  packetize     │◀═════▶│  merge       │  ◀══ 4 neighbours
   │            │  offload │   │  depacketize   │       │              │
   └────────────┘          │   │  response order│       │              │
   ┌────────────┐          │   │                │       │              │
   │  Local     │◀─────────────│                │◀──────│              │
   │  memory    │   AXI4   │   └────────────────┘       └──────────────┘
   │  (slave)   │ data 512 │
   └────────────┘          │
~~~

The split is physical rather than virtual, trading additional link wires for reduced sharing of
router buffering and arbitration. Separate request and response networks are required for
message-level forward progress, so that a node can always accept a response while its own request
path is blocked.

Complexity sits at the network interface. A router routes, replicates, and merges. It holds no
arithmetic unit and no address decode.

**Requirements on the fabric.**

| Requirement | Statement |
|---|---|
| AXI4 response contract | One `B` per `Aw`, and an error `BRESP` dominates when a multicast write is answered by several targets |
| Ordering | Responses reach a master in AXI order within an ID in every configuration |
| Reduction arithmetic | Performed by the compute tile. No router holds an arithmetic unit |
| Freedom from deadlock | Guaranteed at one virtual channel per network channel, with collectives enabled |
| Write data pairing | A target pairs each write request with its own write data, whatever the mix of sources |
| Credit capacity | Every credit-controlled receiver holds at least one flit per virtual channel per cycle of buffer turnaround time, below which a link idles with no contention present |

---

## 6. Interfaces

Each node exposes the data-plane interfaces below. The AXI ports carry the AXI4 signal set at the
widths configured in §7. The Source column names the driver of each signal. Signals are named at
the protocol level. A port pin prefixes the AXI signals with `axi_` and suffixes every signal with
`_i` or `_o` by its direction at that port.

### 6.1 Global signals

| Signal | Source | Description |
|---|---|---|
| `ACLK` | Clock source | AXI domain clock. AXI port signals are sampled on its rising edge |
| `ARESETn` | Reset source | AXI domain reset, active low, asynchronous |
| `noc_clk` | Clock source | NoC domain clock, 1 GHz target |
| `noc_rst_n` | Reset source | NoC domain reset, active low, asynchronous |

The two domains are asynchronous. Endpoint clocks are independent of the NoC clock, and the
crossing sits at the AXI boundary of the network interface.

### 6.2 AXI interfaces

A master attaches to the subordinate port of its network interface, and the fabric's manager port
drives each target. Both ports carry the same channels, so one set of tables covers them and the
Source column names the driver. With two endpoint interfaces per node, each AXI class carries its
own port at its class data width, with the same channel set.

**Write address channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `AWID` | 8 | Manager | Write transaction ID |
| `AWADDR` | 64 | Manager | Write address |
| `AWLEN` | 8 | Manager | Burst length |
| `AWSIZE` | 3 | Manager | Burst size |
| `AWBURST` | 2 | Manager | Burst type |
| `AWLOCK` | 1 | Manager | Lock type |
| `AWCACHE` | 4 | Manager | Memory type |
| `AWPROT` | 3 | Manager | Protection type |
| `AWQOS` | 4 | Manager | Quality of service |
| `AWREGION` | 4 | Manager | Region identifier |
| `AWUSER` | 18 | Manager | User signal. 10 bits carry the collective attributes, see §8 |
| `AWVALID` | 1 | Manager | Write address valid |
| `AWREADY` | 1 | Subordinate | Write address ready |

**Write data channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `WDATA` | `W_data` | Manager | Write data |
| `WSTRB` | `W_data`/8 | Manager | Write strobes |
| `WLAST` | 1 | Manager | Last beat of the burst |
| `WUSER` | 8 | Manager | User signal |
| `WVALID` | 1 | Manager | Write data valid |
| `WREADY` | 1 | Subordinate | Write data ready |

**Write response channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `BID` | 8 | Subordinate | Response transaction ID |
| `BRESP` | 2 | Subordinate | Write response |
| `BUSER` | 8 | Subordinate | User signal |
| `BVALID` | 1 | Subordinate | Write response valid |
| `BREADY` | 1 | Manager | Write response ready |

**Read address channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `ARID` | 8 | Manager | Read transaction ID |
| `ARADDR` | 64 | Manager | Read address |
| `ARLEN` | 8 | Manager | Burst length |
| `ARSIZE` | 3 | Manager | Burst size |
| `ARBURST` | 2 | Manager | Burst type |
| `ARLOCK` | 1 | Manager | Lock type |
| `ARCACHE` | 4 | Manager | Memory type |
| `ARPROT` | 3 | Manager | Protection type |
| `ARQOS` | 4 | Manager | Quality of service |
| `ARREGION` | 4 | Manager | Region identifier |
| `ARUSER` | 8 | Manager | User signal |
| `ARVALID` | 1 | Manager | Read address valid |
| `ARREADY` | 1 | Subordinate | Read address ready |

**Read data channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `RID` | 8 | Subordinate | Read transaction ID |
| `RDATA` | `W_data` | Subordinate | Read data |
| `RRESP` | 2 | Subordinate | Read response |
| `RLAST` | 1 | Subordinate | Last beat of the burst |
| `RUSER` | 8 | Subordinate | User signal |
| `RVALID` | 1 | Subordinate | Read data valid |
| `RREADY` | 1 | Manager | Read data ready |

### 6.3 NoC link interface

Each link carries the three physical networks, and each network is an independent signal group in
each direction. Flow control is credit based, with no ready signal. A flit transfers on a cycle
where valid is high and the transmitter holds a credit for the target virtual channel. The receiver
returns one credit per virtual channel as a buffer frees. Driven signals reset low.

| Signal | Width | Source | Description |
|---|---:|---|---|
| `noc_req_valid` | 1 | Transmitter | `req` flit valid |
| `noc_req_flit` | 165 | Transmitter | `req` flit, header and body |
| `noc_req_credit` | `NUM_VC` | Receiver | `req` credit return |
| `noc_rsp_valid` | 1 | Transmitter | `rsp` flit valid |
| `noc_rsp_flit` | 139 | Transmitter | `rsp` flit, header and body |
| `noc_rsp_credit` | `NUM_VC` | Receiver | `rsp` credit return |
| `noc_wide_valid` | 1 | Transmitter | `wide` flit valid |
| `noc_wide_flit` | 353, 641, or 1217 | Transmitter | `wide` flit, width by wide class, see §8 |
| `noc_wide_credit` | `NUM_VC` | Receiver | `wide` credit return |

---

## 7. Configuration Options

| Feature | Parameter | Values (default) | Comments |
|---|---|---|---|
| Topology | Mesh X and Y dimension | 2-16 (4) | Square meshes only. 256 nodes maximum, set by the 8-bit node ID |
| AXI interface | Address width | 64 b | - |
| AXI interface | ID width | 8 b | - |
| AXI interface | `AWUSER` width | 18 b | 10 bits hold collective attributes, see §8 |
| AXI interface | `ARUSER`, `WUSER`, `RUSER`, `BUSER` width | 8 b | - |
| AXI interface | Narrow class data width | 64 b | - |
| AXI interface | Wide class data width | 256, 512, 1024 b (512) | §4 quotes derived bandwidth at 512 b |
| AXI interface | Endpoint interfaces | 1, 2 (1) | One interface carries both classes with the class selected by address aperture. Two carry one class each |
| Flow control | Virtual channels per network channel | 1-8 (1) | - |
| Ordering | Outstanding transactions per ID | 1-32 (32) | 1 when same-ID response reordering is disabled. A master holds at most 256 in total, one per `ordering_tag` |
| Ordering | Same-ID response reordering | enabled, disabled (enabled) | - |
| Address map | Apertures per node | 1, 2 (2) | One per AXI class. Uniform across nodes, loaded at runtime |
| Address map | Aperture size | power of two | - |

---

## 8. Packet Format

**Flit header, 56 b.** 43 b allocated, 13 b reserved.

| Field | Bits | Width | Note |
|---|---|---|---|
| `axi_ch` | [3:0] | 4 | channel type, one of the ten in the mapping table |
| `src_id` | [11:4] | 8 | source node, `{y[3:0], x[3:0]}` |
| `dst_id` | [19:12] | 8 | destination node, same composition |
| `vc_id` | [22:20] | 3 | virtual channel index, 0 to one less than the configured count |
| `flit_tail` | [23] | 1 | 1 = last flit of the packet |
| `ordering_req` | [24] | 1 | 1 = `ordering_tag` is valid |
| `ordering_tag` | [32:25] | 8 | reorder slot handle, one per transaction, so a master holds at most 256 outstanding |
| `collective_op` | [34:33] | 2 | see encoding below |
| `collective_mask` | [42:35] | 8 | wildcard node mask, same composition as `dst_id` |
| `rsvd` | [55:43] | 13 | reserved, transmitted as zero |

`axi_ch` names which of the ten channel types in the mapping table a flit carries. Ten codes are
needed rather than five, since `req` carries both `NarrowAr` and `WideAr` and `rsp` carries both
`NarrowB` and `WideB`, so the network a flit arrives on does not by itself identify the AXI class.
The value assignment is local to a configuration and is not fixed here. `collective_op` is
different, since a master writes it through `AWUSER`, so its values are part of the interface.

**`collective_op` encoding**

| Value | Name | Meaning |
|---|---|---|
| 0 | `UNICAST` | single destination, no replication or combining. `collective_mask` = 0 |
| 1 | `MULTICAST` | on a request, replicate to the set named by `collective_mask`. On a response, merge the returning responses, and an error `BRESP` dominates |
| 2 | `REDUCE` | on a request, combine operands from the set named by `collective_mask` into one arrival at `dst_id`, with addition as the operator. On a response, replicate the single response back to the set |
| 3 | reserved | |

Collectives exist only on the write path, so `Ar` and `R` are always `UNICAST`. A response carries
the dual of the code its request carried, so a `MULTICAST` write is answered by a merge and a
`REDUCE` write by a replication. An operand is fetched once by an ordinary read, then distributed
by one multicast write.

**`collective_mask` encoding.** A mask bit set to 1 marks the matching node-id bit as don't care.
The mask qualifies `dst_id` while a packet travels toward the set and `src_id` while it travels
from the set, so it names the same nodes on a request and on the response that answers it.

| `collective_mask` | Node set |
|---|---|
| all zero | a single node |
| x bits in use all ones | the whole row, Y fixed |
| y bits in use all ones | the whole column, X fixed |

Only the id bits in use by the configured mesh are set, so the row mask is `0000_1111` at
16 x 16 and `0000_0011` at 4 x 4. Other patterns are reserved.

The named set holds `k` nodes and includes the source. A source is not a destination of its own
multicast and a root does not send to itself, so both collectives move `k - 1` operand tiles.

**`AWUSER` layout, 18 b.** `ARUSER` is 8 b.

| Field | Bits | Width |
|---|---|---|
| `user` | [7:0] | 8 |
| `collective_op` | [9:8] | 2 |
| `collective_mask` | [17:10] | 8 |

**Channel to network mapping.** A channel rides the network that suits its direction and its
message size. Narrow-class requests and responses take separate networks so that a receiver can
always accept a response. `WideAr` and `WideB` are small, so they ride `req` and `rsp` rather than
occupy a wide flit. `WideAw` is the exception: it stays on `wide` with its write data, because
AXI4 gives `W` no ID, a target pairs write data with write requests by arrival order alone, and
two networks do not carry equal congestion.

**Channel bodies.** A body excludes the 56 b header.

| Channel | Fields | Body |
|---|---|---|
| `Aw` | id 8, addr 64, len 8, size 3, burst 2, cache 4, lock 1, prot 3, region 4, qos 4, user 8, the opaque `AWUSER` remainder | 109 b |
| `Ar` | as `Aw`, carrying `ARUSER` in place of the opaque `AWUSER` bits | 109 b |
| `W` | last 1, user 8, strb `W_data`/8, data `W_data` | `9 + 9 W_data / 8` |
| `B` | id 8, resp 2, user 8 | 18 b |
| `R` | last 1, id 8, resp 2, user 8, data `W_data` | `19 + W_data` |

**Mapping.**

| AXI channel | Network | Body |
|---|---|---:|
| `NarrowAw` | `req` | 109 b |
| `NarrowAr` | `req` | 109 b |
| `NarrowW` | `req` | 81 b |
| `NarrowR` | `rsp` | 83 b |
| `NarrowB` | `rsp` | 18 b |
| `WideAw` | `wide` | 109 b |
| `WideAr` | `req` | 109 b |
| `WideW` | `wide` | `9 + 9 W_data / 8` |
| `WideR` | `wide` | `19 + W_data` |
| `WideB` | `rsp` | 18 b |

Each network is as wide as the widest body it carries, plus the header.

```text
req  = max(NarrowAw 109, NarrowAr 109, NarrowW 81, WideAr 109) + 56 = 165 b
rsp  = max(NarrowR 83, NarrowB 18, WideB 18)                   + 56 = 139 b
wide = max(WideAw 109, WideW, WideR)                           + 56
```

`req` is sized by its address channels and `rsp` by `NarrowR`, none of which depends on the wide
class, so both are fixed. Only `wide` follows `W_data`, and `WideW` exceeds `WideR` by
`W_data / 8 - 10` bits, so `WideW` sets the width at every supported value. The wide flit is
353, 641, or 1217 b for a wide class of 256, 512, or 1024 b, tabulated by channel below.

**Field utilization.** The 13 reserved header bits carry no information, so they count in `W_raw`
but never in the used total. Field utilization is what the flit carries. Data utilization is the
AXI payload alone, and applies only to a data-bearing channel.

```text
field utilization = (43 + body) / W_raw
data  utilization = W_data / W_raw

  43      allocated header bits, excluding the 13 reserved
  body    channel body width, from the channel bodies table
  W_raw   physical flit width of the network, in bits
  W_data  data width of the AXI class the channel belongs to, in bits
```

| Network | Channel | Body | Field | Data |
|---|---|---:|---:|---:|
| `req` 165 b | `NarrowAw`, `NarrowAr`, `WideAr` | 109 b | 92.1 % | |
| | `NarrowW` | 81 b | 75.2 % | 38.8 % |
| `rsp` 139 b | `NarrowR` | 83 b | 90.6 % | 46.0 % |
| | `NarrowB`, `WideB` | 18 b | 43.9 % | |
| `wide` 353 b, 256 b data | `WideW` | 297 b | 96.3 % | 72.5 % |
| | `WideR` | 275 b | 90.1 % | 72.5 % |
| | `WideAw` | 109 b | 43.1 % | |
| `wide` 641 b, 512 b data | `WideW` | 585 b | 98.0 % | 79.9 % |
| | `WideR` | 531 b | 89.5 % | 79.9 % |
| | `WideAw` | 109 b | 23.7 % | |
| `wide` 1217 b, 1024 b data | `WideW` | 1161 b | 98.9 % | 84.1 % |
| | `WideR` | 1043 b | 89.2 % | 84.1 % |
| | `WideAw` | 109 b | 12.5 % | |

The channel that sets each network width runs at 90 % or above, so the reserved bits and the
narrower channels account for the remainder. `WideAw` is the outlier and falls from 43.1 % to
12.5 % as the wide class widens, since it rides `wide` for transaction ordering rather than for
its size. That cost is one flit per write burst and amortizes over the burst length.

### 8.1 AXI4 Compliance and Ordering

Full AXI4 is supported: INCR / WRAP / FIXED bursts, narrow and unaligned transfers, `AxLOCK`,
`AxCACHE`, `AxPROT`, `AxQOS`, `AxREGION`, and `xUSER`. Of the 18 `AWUSER` bits, 8 are carried
opaquely in the packet body and 10 hold collective attributes, which
the fabric lifts into the flit header and does not also carry in the body. A target receives those
10 bits cleared, since the fabric has already acted on them. Outstanding transactions may complete
out of order across IDs.
