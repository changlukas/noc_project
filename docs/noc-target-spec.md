# Collective-Capable AXI4 NoC Target Specification

Revision 0.2, 2026-07-23, draft.

## 1. Overview

This specification defines an AXI4 network-on-chip for accelerators running multi-head attention,
one compute tile per node on a 2D mesh.

The workload moves the same data to many places. A group of tiles computes one attention block,
every node in a row of that group needs the same query (Q) sub-tile, and every node in a column
needs the same key (K) and value (V) sub-tiles. On a unicast fabric every node fetches the shared
operand for itself, so one sub-tile is read once per node instead of once per row, and those
transfers all queue at one port. Control traffic waiting behind them is delayed with them.

A multicast delivers a sub-tile to the whole row or column from a single write, and the fabric
returns one response for it. Bulk data runs on a network of its own, so once injected a sub-tile
cannot hold up a control transaction.

---

## 2. Benefits

**Reduced operand distribution traffic.** In-network multicast replaces one unicast injection per
other node in the set with one injection total, reducing source-port traffic by a factor of 15 on a
16-node row when the source sits at one end of it.

**Reduced external memory traffic.** A shared operand is fetched once per row or column rather than
once per node, so external traffic for that operand falls by the length of the set.

**Control latency isolation from bulk data.** Tile payload never shares a network with control
traffic.

**AXI4 interface compatibility.** A master keeps its AXI4 channel set, handshake, and response
rules unchanged. Collective attributes ride in 10 of the 18 `AWUSER` bits, see §8.

---

## 3. Key Features

- 1 GHz NoC clock target
- Full AXI4 protocol support
- In-network multicast on the write path, with the responses it produces merged into one
- Three physical networks, `req`, `rsp` and `wide`, with no shared channel, buffer, or arbiter
- 64-bit narrow AXI class for control and synchronization, 256, 512, or 1024-bit wide class for
  bulk data
- One wide flit per AXI beat
- Wormhole switching, dimension-order (XY) routing, credit-based flow control
- Globally asynchronous, locally synchronous integration, endpoint clocks independent of the
  NoC clock

**Scope.**

- A multicast covers an aligned submesh, not an arbitrary node set
- Software assigns the submesh a group of tiles works in. The fabric provides the primitive
- Multicast exists on the write path. Reads and read data are always unicast
- No arithmetic is performed on payload data anywhere in the fabric. Combining values across nodes,
  including the output accumulation and the softmax statistics, is performed by the compute tiles
  exchanging partial results over ordinary unicast

---

## 4. Performance

Bandwidth below is arithmetic on the configuration parameters. Delivered throughput and latency
depend on traffic, routing delay and buffering, and follow from the models in this section once
those terms are measured.

### 4.1 The workload written twice

A group `G` of `Gx x Gy` tiles computes one attention block. Tile `(gx, gy)` holds the `Q` slice of
row `gy` and the `K` and `V` slices of column `gx`, and the sequence is walked in blocks indexed by
`j`. The two programs below compute the same block under the same tiling. They differ only in which
tile reads external memory.

**Algorithm 1  Per-tile fetch**

```text
 1: parallel for each tile (gx, gy) in G:
 2:     o = 0
 3:     q = hbm_read(Q[gy])                            # every tile of row gy issues this read
 4:     for j in 0 .. blocks-1:
 5:         k = hbm_read(K[gx][j])                     # every tile of column gx issues this read
 6:         v = hbm_read(V[gx][j])                     # every tile of column gx issues this read
 7:         s = matmul(q, transpose(k))
 8:         o = accumulate(o, softmax_step(s), v)      # row statistics exchanged in software
 9:     row_exchange(o, row gy)                        # software, over unicast
10:     hbm_write(O[gy][gx], o)
```

**Algorithm 2  Multicast**

```text
 1: parallel for gy in 0 .. Gy-1:
 2:     at tile (0, gy):  q = hbm_read(Q[gy])
 3:                       multicast(q, row gy of G)
 4: parallel for each tile (gx, gy) in G:  o = 0
 5: for j in 0 .. blocks-1:
 6:     parallel for gx in 0 .. Gx-1:
 7:         at tile (gx, 0):  k = hbm_read(K[gx][j])
 8:                           v = hbm_read(V[gx][j])
 9:                           multicast(k, column gx of G)
10:                           multicast(v, column gx of G)
11:     parallel for each tile (gx, gy) in G:
12:         s = matmul(q, transpose(k))
13:         o = accumulate(o, softmax_step(s), v)      # row statistics exchanged in software
14: parallel for each tile (gx, gy) in G:
15:     row_exchange(o, row gy)                        # software, over unicast
16:     hbm_write(O[gy][gx], o)
```

Only the tiles on the west edge of the group read `Q` from external memory (line 2), then multicast
it row-wise 3. At each sequence block only the tiles on the south edge read `K` and `V` 7 8,
followed by multicasting them column-wise 9 10. Lines 12 and 13 are the same computation as lines 7
and 8 of Algorithm 1.

Lines 15 and 16 are unchanged as well, and deliberately so. The fabric performs no arithmetic on
payload data, so the row exchange of `O` and the statistics inside `softmax_step` are carried out by
the compute tiles over ordinary unicast in both programs. Multicast changes operand distribution
and nothing else.

**Off-chip reads per group, per sequence block.**

```text
per-tile fetch  Gx Gy (|k| + |v|)  and  Gx Gy |q| once
multicast       Gx (|k| + |v|)     and  Gy |q| once
```

Each shared operand is read once per set rather than once per member, so external traffic for that
operand falls by the length of the set.

### 4.2 Off-chip access

This workload is bounded by off-chip memory bandwidth, and a row or column of `k` tiles reuses one
operand of size `S`. Under per-tile fetch every tile reads the operand from memory. A multicast
reads it once and shares it on-chip.

```text
                off-chip access    latency
per-tile fetch  k S                k S / B
multicast       S                  S / B

  k  tiles in the participating row or column
  S  operand sub-tile size, in bytes
  B  off-chip memory bandwidth
```

The off-chip access and the off-chip-bandwidth term of latency each drop by `k`, since that term
tracks access on a memory-bound path.

### 4.3 Payload bandwidth

A channel carries one flit per cycle and a flit holds at most one data width of AXI payload, so
payload bandwidth is data width x 1 GHz / 8. The wide class is shown at 512 b.

| AXI traffic | Data width | Physical network | Payload bandwidth |
|---|---:|---|---:|
| Narrow req | 64 b | `req` | 8 GB/s |
| Narrow rsp | 64 b | `rsp` | 8 GB/s |
| Wide req | 512 b | `wide` | 64 GB/s |
| Wide rsp | 512 b | `wide` | 64 GB/s |

Request traffic carries write data, response traffic carries read data. Narrow req and rsp take
separate networks and run concurrently. Wide req and rsp share the `wide` network, so 64 GB/s is
their combined ceiling. Flit width and per-channel utilization are in §8.

### 4.4 Latency

Zero-load latency is the floor reached with no queueing. A packet crossing `H` hops traverses `H`
links and passes `H` + 1 routers, counting the source router.

```text
T_zero_load = H t_wire + (H + 1) t_router + L

  H            Manhattan hop count from source to destination
  t_wire       link traversal delay per hop, in NoC cycles
  t_router     router pipeline latency per hop, in NoC cycles
  L            tile payload length in flits, injected one flit per cycle
```

Mean hop count in a `k x k` mesh is `2(k^2 - 1) / (3k)`, 2.5 at `k` = 4 and 10.625 at `k` = 16.

The forms below are for a multicast along one axis. A submesh multicast runs one phase per axis,
`2^b` row multicasts in parallel followed by one column multicast over their results.

Tail arrival at the farthest member of a one-axis multicast:

| Case | Form |
|---|---|
| Repeated unicast | `(k - 1)L + H_max t_wire + (H_max + 1) t_router` |
| In-network multicast | `L + H_max t_wire + (H_max + 1) t_router` |

```text
  k          nodes in the participating row or column
  L          sub-tile payload length in flits, sub-tile bytes / (W_data / 8)
  W_data     data width of the AXI class carrying the sub-tile, in bits
  H_max      maximum hop count from the source to a member of its set
```

The transport term is identical in both. A multicast replaces the `(k - 1)L` serialization at one
port with a single sub-tile. At one end of the set `H_max` is `k - 1`, at the center `⌊k/2⌋`.

### 4.5 Multicast traffic reduction

Counting results on a row or column of `k` nodes.

| Quantity | Unicast | In network | Ratio, `k` = 4 | Ratio, `k` = 16 |
|---|---|---|---:|---:|
| Source network port flits | `(k - 1)L` | `L` per path | 3 x | 15 x |
| Flit hops | `L [s(s + 1)/2 + (k - 1 - s)(k - s)/2]` | `L(k - 1)` | 2 x | 8 x |

```text
  s  source index in the row or column, 0 to k - 1
```

Both ratios are for a source at one end of the set. An interior one is served by one path per
direction, which halves the port ratio and lowers the flit-hop ratio to 1.33 x at `k` = 4 and
4.27 x at `k` = 16.

---

## 5. Architecture

Each node holds one compute tile and its local memory, so it is both an AXI master and an AXI
slave. Each link carries three physical networks. XY dimension-order routing fixes the path a
multicast takes, and its merged response retraces that path in reverse.

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
   │            │  addr 64 │   │                │       │  VC allocate │  rsp  ══▶
   │  (master)  │  ID 8    │   │  address map   │       │  replicate   │  wide ══▶
   │            │◀═════════════│  packetize     │◀──────│  merge       │  ◀══ 4 neighbours
   │            │          │   │  depacketize   │       │              │
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

The network interface carries the address map, packetization, depacketization, and response
ordering. The router routes, replicates, and merges, with no arithmetic unit and no address
decode.

**Requirements on the fabric.**

| Requirement | Statement |
|---|---|
| AXI4 response contract | One `B` per `Aw`, and an error `BRESP` dominates when a multicast write is answered by several targets |
| Ordering | Responses reach a master in AXI order within an ID in every configuration |
| Deadlock-free | Guaranteed at one virtual channel per network channel, with multicast enabled |
| Write data pairing | A target pairs each write request with its own write data, whatever the mix of sources |
| Credit capacity | A credit-controlled receiver holds, per virtual channel, at least one flit of buffer for each cycle of buffer turnaround time. Below that capacity a link idles with no contention present |

---

## 6. Interfaces

Each node exposes the data-plane interfaces below. The AXI ports carry the AXI4 signal set at the
widths configured in §7. The Source column names the driver of each signal. Signals are named at
the protocol level. At the port pins the AXI signals carry an `axi_` prefix and an `_i` or `_o`
suffix by direction.

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
Source column names the driver. When a node is configured with two endpoint interfaces (§7),
each AXI class has its own port at its class data width, with the same channel set.

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
| `AWUSER` | 21 | Manager | User signal. 13 bits carry the collective attributes, see §8 |
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
| Ordering | Outstanding transactions per ID | 1-32 (32) | 1 when same-ID response reordering is disabled. A master holds at most 256 in total, one per `ordering_tag`, see §8 |
| Ordering | Same-ID response reordering | enabled, disabled (enabled) | The §5 ordering requirement holds in either setting |
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
The value assignment is local to a configuration and is not fixed here. The `collective_op`
values are part of the interface, since a master writes them through `AWUSER`.

**`collective_op` encoding**

| Value | Name | Meaning |
|---|---|---|
| 0 | `UNICAST` | single destination, no replication or combining. `collective_mask` = 0 |
| 1 | `MULTICAST` | on a request, replicate to the set named by `collective_mask`. On a response, merge the returning responses, and an error `BRESP` dominates |
| 2-3 | reserved | |

Multicast exists only on the write path, so `Ar` and `R` are always `UNICAST`. An operand is
fetched once by an ordinary read, then distributed by one multicast write.

**`collective_mask` encoding.** A mask bit set to 1 marks the matching node-id bit as don't care.
The mask qualifies `dst_id` while a packet travels toward the set and `src_id` while it travels
from the set, so it names the same nodes on a request and on the response that answers it.

| `collective_mask` | Node set |
|---|---|
| all zero | a single node |
| `n` bits set | the `2^n` nodes reached by varying those id bits |
| x bits in use all ones | the whole row, Y fixed |
| y bits in use all ones | the whole column, X fixed |

Only the id bits in use by the configured mesh are set, so the row mask is `0000_1111` at
16 x 16 and `0000_0011` at 4 x 4.

Setting `a` of the x bits and `b` of the y bits names a `2^a` by `2^b` submesh whose origin is
`dst_id` with those bits cleared, so every set the encoding expresses is a power-of-two submesh
aligned to its own dimensions.

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
occupy a wide flit. `WideAw` is the exception and stays on `wide` with its
write data. AXI4 gives `W` no ID, so a target pairs write data with write requests by arrival
order alone. On separate networks under unequal congestion the requests could fall out of step
with their data and mispair at the target, so the request travels with its data.

**Channel bodies.** A body excludes the 56 b header.

| Channel | Fields | Body |
|---|---|---|
| `Aw` | id 8, addr 64, len 8, size 3, burst 2, cache 4, lock 1, prot 3, region 4, qos 4, user 8 (the opaque `AWUSER` remainder) | 109 b |
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
`AxCACHE`, `AxPROT`, `AxQOS`, `AxREGION`, and `xUSER`. `AxLOCK` applies to unicast, and a
collective transaction is not an exclusive access. Of the 18 `AWUSER` bits, 8 are carried
opaquely in the packet body and 10 hold collective attributes, which
the fabric lifts into the flit header and does not also carry in the body. A target receives those
10 bits cleared, since the fabric has already acted on them. Outstanding transactions may complete
out of order across IDs.
