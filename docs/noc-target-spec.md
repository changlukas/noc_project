# Collective-Capable AXI4 NoC Target Specification

Revision 0.2, 2026-07-23, draft.

## 1. Overview

This IP is a collective-capable, AMBA AXI4-compliant network-on-chip (NoC) for transformer
attention accelerators, organized as one compute tile per node on a 2D mesh. It accelerates
multi-head attention by distributing shared operands through hardware multicast and by isolating
latency-critical control traffic from bulk data on dedicated physical networks, while presenting a
standard AXI4 interface to every compute tile.

The two planes serve the two communication patterns of an attention layer.

- **Wide data plane, operand distribution (one-to-many).** Attention tiles a query-by-key matrix
  multiply across the mesh, so tiles in a row share one query operand and tiles in a column share one
  key and value operand. The wide plane carries these bulk tensors: the shared Q, K, and V operands,
  activation tiles between stages, and the output write-back. The fabric distributes each shared
  operand with a single hardware multicast instead of one copy per tile.
- **Narrow control plane, coordination and gather (many-to-one).** Attention then combines results
  across the group. The narrow plane carries the small, latency-critical messages of that step, the
  softmax statistics (row maximum and running denominator), inter-tile synchronization, barriers, and
  scheduling traffic, and it merges the responses of a multicast write into one.

**Highlights**

- **Hardware multicast** on the write path. One injection replaces one unicast per destination,
  reducing source-port bandwidth by up to 15x on a 16-node set.
- **Write-response aggregation.** Multicast responses are merged into a single AXI response, with an
  error response taking precedence.
- **Dual-plane traffic isolation.** A 64-bit narrow control plane and a 256, 512, or 1024-bit wide
  data plane on physically separate networks, isolating high-bandwidth data bursts from
  latency-sensitive control traffic.
- **AXI4-compatible interface.** Endpoints expose standard AXI4 channels. Unicast traffic needs no
  changes, and a collective write encodes its operation and mask in `AWUSER`.

---

## 2. Key Features

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

## 3. Performance

This section reports peak payload bandwidth derived from the configured data width and NoC clock.
Delivered throughput and latency depend on traffic, routing delay, and buffering, and follow from
the models below once those terms are measured.

### 3.1 Tiled attention dataflow

**Model-level execution flow.** The model runs an embedding step, then `N` attention layers in
sequence, then a detokenizer that produces the output tokens. Each layer carries two traffic classes:

- Each layer's output (its activations) is passed on as the next layer's input. This is the feature
  pipeline, and it travels on the wide data plane.
- A scheduler decides when each tile group starts and signals when its work is done. These are small
  control and synchronization messages, and they travel on the narrow control plane.

Weights are preloaded from LPDDR into each tile's local 3D DRAM and stay resident for the run, so a
layer does not fetch weights over the fabric.

```text
place once:  DMA copies weights from LPDDR into each tile's 3D DRAM   # stays resident
x = embed(tokens)                                                    # first activations
for layer = 1 .. N:                                                  # static schedule
    for each tile group in the layer:
        attention_block(group, x)                                    # Q/K/V on [wide], m/l on [narrow]
        scheduler marks the group done                               # control        [narrow]
    x = this layer's output activations, passed to the next layer    # feature pipeline [wide]
tokens = detokenize(x)
```

**Attention block mapping.** A tile group computes one attention block together. The score comes
from multiplying queries by keys, so it forms a grid whose rows come from the queries and columns
from the keys. The tile group maps this grid onto the mesh as follows:

- Each mesh **row** holds one slice of the **queries**. Every tile in the row uses it, so the query
  is shared along the row.
- Each mesh **column** holds one slice of the **keys and values**. Every tile in the column uses
  them, so they are shared down the column.
- Tile `(x, y)` computes the score cell for query-slice `y` and key-slice `x`.
- The softmax for query-slice `y` combines the scores against all key-slices, so it is combined
  across the tiles in that row.

```text
                       key / value slices  (each shared down its column)
                          x=0     x=1     x=2     x=3
                        ┌─────┬─────┬─────┬─────┐
   y=0  Q slice 0  ──▶  │  S  │  S  │  S  │  S  │   each row shares one query slice
   y=1  Q slice 1  ──▶  │  S  │  S  │  S  │  S  │
   y=2  Q slice 2  ──▶  │  S  │  S  │  S  │  S  │
   y=3  Q slice 3  ──▶  │  S  │  S  │  S  │  S  │
                        └─────┴─────┴─────┴─────┘
                           │     │     │     │
                           ▼     ▼     ▼     ▼
                       each column shares one key / value slice
   softmax for row y combines the scores across the whole row (all x)
```

Memory tiers:

- Backing memory: `LPDDR` (external, via host DMA) or the tile's local `3D DRAM`.
- `buf`: the on-chip compute buffer.
- The running state `O_i^y, m_i^y, l_i^y` stays on-tile across the loop.
- The fabric does no arithmetic, so each reduction is computed by the tiles and only its traffic crosses the fabric.

Running softmax update for query row-slice `y`, reduced across `x` (the key shards) within the group row:

```text
s_local = rowmax(S)                                                # on each tile (x,y)
m_new   = max(m_old, reduce_x max(s_local))                        # all-reduced across x   [narrow]
P       = exp(S - m_new)
l_new   = exp(m_old - m_new) * l_old + reduce_x sum(rowsum(P))     # all-reduced across x   [narrow]
O_i^y   = (exp(m_old - m_new) * l_old * O_i^y + reduce_x(P V_j^x)) / l_new   # PV to row owner [wide]
```

**Algorithm 1  Per-tile fetch**

```text
Require: Q, K, V in backing memory (LPDDR or 3D DRAM);  O written to 3D DRAM
 1: Br, Bc;  Tr = ceil(Sq/Br),  Tc = ceil(Sk/Bc)
 2: for j = 1 .. Tc:                                                # key/value blocks
 3:   for i = 1 .. Tr:                                              # query blocks
 4:     parallel for each tile (x,y) in G:
 5:        load Q_i^y from backing memory -> buf         # re-read on every column x  [wide]
 6:        load K_j^x, V_j^x from backing memory -> buf  # re-read on every row y     [wide]
 7:        S = Q_i^y (K_j^x)^T / sqrt(d)
 8:        running softmax update (O_i^y, m_i^y, l_i^y)
 9: row-owner tile of each row y:  store O_i^y -> 3D DRAM
```

Memory reads per block, per unique slice: `Q_i^y` read `Gx` times, `K_j^x`/`V_j^x` read `Gy` times.

**Algorithm 2  Multicast**

```text
Require: Q, K, V in backing memory (LPDDR or 3D DRAM);  O written to 3D DRAM
 1: Br, Bc;  Tr = ceil(Sq/Br),  Tc = ceil(Sk/Bc)
 2: for j = 1 .. Tc:                                                # key/value blocks
 3:   each column x:  source tile loads K_j^x, V_j^x -> buf   # once per column  [wide]
 4:   multicast K_j^x, V_j^x down the group column                    [wide]
 5:   for i = 1 .. Tr:                                              # query blocks
 6:     each row y:  source tile loads Q_i^y -> buf           # once per row      [wide]
 7:     multicast Q_i^y across the group row                          [wide]
 8:     parallel for each tile (x,y) in G:
 9:        S = Q_i^y (K_j^x)^T / sqrt(d)
10:        running softmax update (O_i^y, m_i^y, l_i^y)
11: row-owner tile of each row y:  store O_i^y -> 3D DRAM
```

Memory reads per block, per unique slice: `Q_i^y` and `K_j^x`/`V_j^x` read once. Relative to per-tile
fetch, multicast cuts each shared operand from `Gx` or `Gy` fetches to one source fetch plus on-chip
replication, so both the backing-memory reads and the memory-bound term of latency fall with the set size.

Source tiles follow memory-controller placement. Diagonal tiles balance row-wise and column-wise
multicast traffic.

### 3.2 Payload bandwidth

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
their combined ceiling. Flit width and per-channel utilization are in §7.

### 3.3 Latency and multicast reduction

Zero-load latency is the packet latency with no queueing or contention. A packet crossing `H` hops
traverses `H` links and passes `H + 1` routers, counting the source router.

```text
T_zero_load = H t_wire + (H + 1) t_router + L

  H         Manhattan hop count from source to destination
  t_wire    link traversal delay per hop, in NoC cycles
  t_router  router pipeline latency per hop, in NoC cycles
  L         payload length in flits, injected one flit per cycle
```

Mean hop count in a `k x k` mesh is `2(k^2 - 1) / (3k)`, which is 2.5 at `k = 4` and 10.625 at `k = 16`.

The forms below cover a multicast along one axis over a set of `k` nodes. A submesh multicast runs
in two phases, one per axis.

```text
  k        nodes in the participating row or column
  L        sub-tile payload length in flits, sub-tile bytes / (W_data / 8)
  W_data   data width of the AXI class carrying the sub-tile, in bits
  H_max    maximum hop count from the source to a member of the set
  s        source index in the set, 0 to k - 1
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

---

## 4. Architecture

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

The networks are physically separate rather than virtual channels on a shared link, trading wire
count for independent buffering and arbitration. Separate request and response networks are required
for message-level forward progress, so that a node can always accept a response while its own request
path is blocked.

The network interface holds the system address map (SAM), packetizes and depacketizes, and orders
responses. The SAM defines a global config space and a global memory space, and the NI looks up each
request address in it to select the destination node and the AXI class: a config-space access uses
the narrow class and a memory-space access uses the wide class. Each channel of the chosen class then
maps to a physical network as in §7, and the bulk wide data travels on the `wide` network. The router
performs route selection, multicast replication, and response aggregation. It does not decode AXI
addresses or perform arithmetic on payload data.

**Requirements on the fabric.**

| Requirement | Statement |
|---|---|
| AXI4 response contract | One `B` per `Aw`, and an error `BRESP` dominates when a multicast write is answered by several slaves |
| Ordering | Responses reach a master in AXI order within an ID in every configuration |
| Deadlock-free | Guaranteed at one virtual channel per network channel, with multicast enabled |
| Write data pairing | A slave pairs each write request with its own write data, whatever the mix of sources |
| Credit capacity | A credit-controlled receiver holds, per virtual channel, at least one flit of buffer for each cycle of buffer turnaround time. Below that capacity a link idles with no contention present |

---

## 5. Interfaces

Each node exposes the data-plane interfaces below. The AXI ports carry the AXI4 signal set at the
widths configured in §6. The Source column names the driver of each signal. Signals are named at
the protocol level. At the port pins the AXI signals carry an `axi_` prefix and an `_i` or `_o`
suffix by direction.

### 5.1 Global signals

| Signal | Source | Description |
|---|---|---|
| `ACLK` | Clock source | AXI domain clock. AXI port signals are sampled on its rising edge |
| `ARESETn` | Reset source | AXI domain reset, active low, asynchronous |
| `noc_clk` | Clock source | NoC domain clock, 1 GHz target |
| `noc_rst_n` | Reset source | NoC domain reset, active low, asynchronous |

The two domains are asynchronous. Endpoint clocks are independent of the NoC clock, and the
crossing sits at the AXI boundary of the network interface.

### 5.2 AXI interfaces

An AXI master connects to the slave-side port of its network interface, and the interface's
master-side port drives each AXI slave. Both ports carry the same channels, so one set of tables covers them and the
Source column names the driver. When a node is configured with two endpoint interfaces (§6),
each AXI class has its own port at its class data width, with the same channel set.

**Write address channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `AWID` | 8 | Master | Write transaction ID |
| `AWADDR` | 64 | Master | Write address |
| `AWLEN` | 8 | Master | Burst length |
| `AWSIZE` | 3 | Master | Burst size |
| `AWBURST` | 2 | Master | Burst type |
| `AWLOCK` | 1 | Master | Lock type |
| `AWCACHE` | 4 | Master | Memory type |
| `AWPROT` | 3 | Master | Protection type |
| `AWQOS` | 4 | Master | Quality of service |
| `AWREGION` | 4 | Master | Region identifier |
| `AWUSER` | 18 | Master | User signal. 10 bits carry the collective attributes, see §7 |
| `AWVALID` | 1 | Master | Write address valid |
| `AWREADY` | 1 | Slave | Write address ready |

**Write data channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `WDATA` | `W_data` | Master | Write data |
| `WSTRB` | `W_data`/8 | Master | Write strobes |
| `WLAST` | 1 | Master | Last beat of the burst |
| `WUSER` | 8 | Master | User signal |
| `WVALID` | 1 | Master | Write data valid |
| `WREADY` | 1 | Slave | Write data ready |

**Write response channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `BID` | 8 | Slave | Response transaction ID |
| `BRESP` | 2 | Slave | Write response |
| `BUSER` | 8 | Slave | User signal |
| `BVALID` | 1 | Slave | Write response valid |
| `BREADY` | 1 | Master | Write response ready |

**Read address channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `ARID` | 8 | Master | Read transaction ID |
| `ARADDR` | 64 | Master | Read address |
| `ARLEN` | 8 | Master | Burst length |
| `ARSIZE` | 3 | Master | Burst size |
| `ARBURST` | 2 | Master | Burst type |
| `ARLOCK` | 1 | Master | Lock type |
| `ARCACHE` | 4 | Master | Memory type |
| `ARPROT` | 3 | Master | Protection type |
| `ARQOS` | 4 | Master | Quality of service |
| `ARREGION` | 4 | Master | Region identifier |
| `ARUSER` | 8 | Master | User signal |
| `ARVALID` | 1 | Master | Read address valid |
| `ARREADY` | 1 | Slave | Read address ready |

**Read data channel**

| Signal | Width | Source | Description |
|---|---:|---|---|
| `RID` | 8 | Slave | Read transaction ID |
| `RDATA` | `W_data` | Slave | Read data |
| `RRESP` | 2 | Slave | Read response |
| `RLAST` | 1 | Slave | Last beat of the burst |
| `RUSER` | 8 | Slave | User signal |
| `RVALID` | 1 | Slave | Read data valid |
| `RREADY` | 1 | Master | Read data ready |

### 5.3 NoC link interface

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
| `noc_wide_flit` | 353, 641, or 1217 | Transmitter | `wide` flit, width by wide class, see §7 |
| `noc_wide_credit` | `NUM_VC` | Receiver | `wide` credit return |

---

## 6. Configuration Options

| Feature | Parameter | Values (default) | Comments |
|---|---|---|---|
| Topology | Mesh X and Y dimension | 2-16 (4) | Square meshes only. 256 nodes maximum, set by the 8-bit node ID |
| AXI interface | Address width | 64 b | - |
| AXI interface | ID width | 8 b | - |
| AXI interface | `AWUSER` width | 18 b | 10 bits hold collective attributes, see §7 |
| AXI interface | `ARUSER`, `WUSER`, `RUSER`, `BUSER` width | 8 b | - |
| AXI interface | Narrow class data width | 64 b | - |
| AXI interface | Wide class data width | 256, 512, 1024 b (512) | §3 quotes derived bandwidth at 512 b |
| AXI interface | Endpoint interfaces | 1, 2 (1) | One interface carries both classes, with the class selected by the SAM address space. Two carry one class each |
| Flow control | Virtual channels per network channel | 1-8 (1) | - |
| Ordering | Outstanding transactions per ID | 1-32 (32) | 1 when same-ID response reordering is disabled. A master holds at most 256 in total, one per `ordering_tag`, see §7 |
| Ordering | Same-ID response reordering | enabled, disabled (enabled) | The §4 ordering requirement holds in either setting |
| Address map | SAM address spaces | config, memory | Config space selects the narrow class, memory space the wide class. Uniform across nodes, loaded at runtime |
| Address map | Space region size | power of two | - |

---

## 7. Packet Format

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

**Channel to network mapping.** Each channel maps to the network chosen by its direction and
message size. Narrow-class requests and responses take separate networks so that a receiver can
always accept a response. `WideAr` and `WideB` are small, so they use `req` and `rsp` rather than
occupy a wide flit. `WideAw` is the exception and stays on `wide` with its
write data. AXI4 gives `W` no ID, so a slave associates write data with write requests by their
arrival order. On separate networks under unequal congestion, the requests could arrive in a
different order from their write data and be associated with the wrong request, so the request
travels with its data.

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
12.5 % as the wide class widens, since it uses `wide` for transaction ordering rather than for
its size. That cost is one flit per write burst and amortizes over the burst length.

### 7.1 AXI4 Compliance and Ordering

Full AXI4 is supported: INCR / WRAP / FIXED bursts, narrow and unaligned transfers, `AxLOCK`,
`AxCACHE`, `AxPROT`, `AxQOS`, `AxREGION`, and `xUSER`. `AxLOCK` applies to unicast, and a
collective transaction is not an exclusive access. Of the 18 `AWUSER` bits, 8 are carried
opaquely in the packet body and 10 hold collective attributes, which
the fabric lifts into the flit header and does not also carry in the body. A slave receives those
10 bits cleared, since the fabric has already acted on them. Outstanding transactions may complete
out of order across IDs.
