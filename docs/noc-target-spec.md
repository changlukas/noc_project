# Collective-Capable AXI4 NoC Target Specification

Revision 0.1, 2026-07-22, draft.

## 1. Overview

This specification defines an AXI4 network-on-chip for accelerators running tiled GEMM and
multi-head attention, one compute tile per node on a 2D mesh.

These workloads move the same data to many places. Every node in a row needs the same Q tile, and
every node in a column the same K and V tiles. The output tile is then summed back across the row.
On a unicast fabric each of these becomes a separate transfer per node, and they all queue at one
port. Control traffic waiting behind them is delayed with them.

This fabric carries each pattern as one operation. A multicast delivers a tile to a whole row or
column from a single write. A reduction sums the output tiles on their way to the destination, so
one result arrives instead of many. Bulk data runs on a network of its own, so once injected a
tile cannot hold up a control transaction.

---

## 2. Benefits

**Reduced operand distribution traffic.** In-network multicast replaces `N-1` unicast injections
with one, cutting source-port traffic 15 x on a 16-node row when the source sits at one end of it.

**Reduced root-port traffic for output accumulation.** Accumulating the output tile across a row
sends `N-1` full tiles into one port under unicast. In-network combining leaves one arrival, or
two when the root sits inside the row and each direction converges separately.

**Control latency isolated from bulk data.** Tile payload never shares a network with control
traffic, and the networks share no channel, buffer, or arbiter. With one endpoint interface the
isolation starts once a transaction is injected. With one interface per AXI class it starts at the
endpoint.

**AXI4 interface compatibility.** A master keeps its AXI4 channel set, handshake, and response
rules unchanged. Collective attributes ride in 10 of the 18 `AWUSER` bits.

---

## 3. Key Features

- 1 GHz NoC clock target
- Full AXI4 protocol support
- In-network multicast and reduction on the write path
- Three physical networks, `req`, `rsp` and `wide`, sharing no channel, buffer, or arbiter
- 64-bit narrow AXI class for control and synchronization, up to 1024-bit wide class for bulk data
- One wide flit per AXI beat, so an endpoint is never throttled by flit serialization
- Complexity at the network interface, leaving routers to route, replicate and merge
- Wormhole switching, dimension-order (XY) routing, credit-based flow control
- GALS multi-domain integration, endpoint clocks independent of the NoC clock

**Scope.** Collectives cover a full row or a full column, not an arbitrary node set and not the
whole mesh. On the data path the only reduction operator is addition on floating-point data.
Reduction arithmetic is performed by the compute tile. No router contains an arithmetic unit, and
no packet carries an operand format, since every node that joins a reduction also contributes to
it and so already holds the operand type. All members of one reduction agree that type out of
band. Scalar exchanges smaller than an operand tile are left to software over ordinary unicast.

---

## 4. Performance Targets

Bandwidth is given as raw / data and applies to the `wide` network. Derivations are in §7.

| Metric | 16 x 16 (max) | 4 x 4 (default) |
|---|---:|---:|
| Bisection bandwidth | 4.87 / 4.10 TB/s | 1.22 / 1.02 TB/s |
| Aggregate injection bandwidth | 38.9 / 32.8 TB/s | 2.43 / 2.05 TB/s |
| Source or root port traffic ratio, at one end of the set | 15 x | 3 x |

**Collective phase latency.** For a row or column collective with `N` members and tile length `L`:

```text
unicast              (N-1)L + (H_max + 1) t_r
in-network multicast      L + (H_max + 1) t_r
in-network reduction      L + (H_max + 1) t_r + D C_offload

  N          nodes in the set, including the source or root
  L          tile payload length, in data flits
  H_max      hops from the source or root to the farthest member
  t_r        router pipeline latency per hop, in cycles
  D          joins on the critical path of a reduction
  C_offload  combine round trip to the compute tile and back, in cycles
```

Injection is one flit per cycle, so `L` is in cycles, and the forms above take a source or root at
one end of its set. An interior one is served by two paths, which doubles the serialization term.
The transport term is identical in all three, and a collective replaces the `(N-1)L` serialization
at one port with a single tile. A multicast carries no further term. A reduction adds one offload
round trip per join on the critical path, `D` of them, and is the shorter form while
`D C_offload` stays below `(N-2)L`. The inner loop issues three multicasts and one reduction, all
on a tile of the same size. Absolute latency follows from `L` and `C_offload`, neither of which is
characterized.

Area and energy are open characterization items, concentrated in the additional crossbars, since
the three networks share no datapath.

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

          every link = req 175 b + rsp 139 b + wide 1217 b, 4 x 4 shown, 16 x 16 max
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
   │            │◀═════════════│  packetise     │◀═════▶│  merge       │  ◀══ 4 neighbours
   │            │  offload │   │  depacketise   │       │              │
   └────────────┘          │   │  reorder buf   │       │              │
   ┌────────────┐          │   │                │       │              │
   │  Local     │◀─────────────│                │◀──────│              │
   │  memory    │   AXI4   │   └────────────────┘       └──────────────┘
   │  (slave)   │  64/1024 │
   └────────────┘          │
~~~

**Network separation.** Separating bulk data from the rest keeps a tile transfer from delaying a
control transaction, and keeps small messages off a wide flit they would barely fill. The
separation is physical rather than virtual because link wires are plentiful in modern nodes while
router buffers and crossbar area are not.

**Deadlock freedom.** Across networks, a target answers a request or write data taken from `req`
on either `wide` or `rsp`, and answers the same taken from `wide` on `rsp`. The ordering `req`
before `wide` before `rsp` therefore holds and no answer returns to an earlier network. `wide`
carries requests and read data together, but the two never answer each other: its write requests
are answered on `rsp`, and its read data answers a request that arrived on `req`.

Within a network, replication is path-based rather than tree-based. A source emits one packet per
direction along its row or column, each following a single dimension-order path, so no packet ever
holds two network output channels at once and the channel dependency graph stays acyclic.
Tree-based replication would not have that property. A source at one end of the set emits one
packet, an interior source two.

A member in the middle of a path both ejects the packet and forwards it, so it holds an ejection
port alongside a network channel. Every endpoint shall therefore accept any packet delivered to it
in bounded time, and the offload port shall accept operands and return results in bounded time, so
neither an ejection port nor a compute tile can stall the path it shares.

A response merge waits on an arrival from the network while holding only its own local response,
so it allocates its output once every response is present and blocks nothing upstream.

A join is the other way round: it waits on a local contribution while an operand already sits in
the channel. A node shall therefore take an arriving operand into local storage rather than hold
it in the channel, so that traffic behind it keeps moving and the compute that produces the local
contribution is never queued behind the join waiting for it. With that, one virtual channel per
network channel stays sufficient.

**Edge complexity.** The network interface holds the address map, packetisation, the reorder
buffer, the per-direction split, and the offload interface. A router routes, replicates, and
merges collective responses. It holds no arithmetic, no address decode, and no ordering state
beyond the collective responses in flight through it. Keeping the router that thin is what puts a
3 to 5 cycle pipeline within reach at 1 GHz.

**Per-direction paths.** A source or root inside its set is served by one path per direction. The
network interface splits a collective into those paths on the way out and combines their responses
on the way in, so the AXI interface issues one write and receives one response wherever the source
or root sits. Only the network ports see two.

**Reduction offload.** Operands cross to the compute tile through the network interface and the
combined result returns the same way.

**Response merging.** A multicast write produces one `B` per destination, and AXI4 permits exactly
one `B` per `AW`. Because a collective spans one row or one column, the return path retraces the
outbound one. A multicast merges the responses along it on `rsp` into a single response, and an
error `BRESP` dominates. A reduction runs the other way: the root answers once, and the fabric
replicates that response back along the same path so that every contributing node receives the one
`B` its own `AW` is owed. Each replica carries the `ordering_tag` that contributor injected, which
the join that absorbed the operand retains for the return.

**Ordering.** Responses reach the master in AXI order within an ID in every configuration.
`ordering_tag` carries the slot the initiator NI reserved before injection, so a response can be
placed back into per-ID issue order on arrival.

---

## 6. Configuration Options

| Option | Supported values |
|---|---|
| Mesh dimensions | up to 16 x 16, 256 nodes |
| Virtual channels per network channel | 1 to 8 |
| Outstanding transactions per ID | up to 32, within the 256 a master holds in total |
| Narrow AXI class data width | 64 b |
| Wide AXI class data width | up to 1024 b |
| Address apertures per node | one per AXI class, uniform across nodes, power of two, loaded at runtime |
| Endpoint AXI interfaces | one at the wide data width, class selected by address aperture, or two, one per class |
| Per-ID reorder buffer | enabled, or disabled with same-ID outstanding limited to 1 |

---

## 7. Performance Model

### 7.1 Notation

| Symbol | Meaning |
|---|---|
| `k` | Mesh dimension. The fabric has `k x k` nodes |
| `N` | Nodes in the participating row or column, so `N` = `k` in this scope |
| `s` | Source index in a row or column, numbered `0` to `k - 1` |
| `L` | Tile payload length in data flits, excluding address and response flits |
| `H` | Manhattan hop count from source to destination |
| `H_max` | Maximum hop count from a source or root to a member of its set |
| `H_mean` | Mean hop count between two uniformly selected nodes |
| `H_worst` | Corner to corner hop count |
| `t_r` | Router pipeline latency per hop, in NoC cycles |
| `C_offload` | Combine round trip to the compute tile and back, in NoC cycles |
| `D` | Joins on the critical path of a reduction |
| `f_noc` | NoC clock frequency |
| `W_raw` | Physical flit width, in bits |
| `W_data` | AXI data width, the delivered data per beat, in bits |
| `B_link` | Bandwidth of one simplex channel, in B/s |
| `T_head` | Zero-load head-flit latency |

| Parameter | Value |
|---|---:|
| `f_noc` | 1 GHz |
| `t_r` | 3 to 5 cycles |
| `req` `W_raw` / `W_data` | 175 / 64 b |
| `rsp` `W_raw` / `W_data` | 139 / 64 b |
| `wide` `W_raw` / `W_data` | 1217 / 1024 b |

### 7.2 Throughput

```text
B_link_raw  = W_raw  f_noc / 8
B_link_data = W_data f_noc / 8

  B_link_raw   bandwidth of one simplex channel carrying flits, in B/s
  B_link_data  the part of it that is AXI data, in B/s
  W_raw        physical flit width, in bits
  W_data       AXI data width, the delivered data per beat, in bits
  f_noc        NoC clock frequency

req      W_raw  175 b ->  21.875 GB/s,  W_data   64 b ->   8 GB/s
rsp      W_raw  139 b ->  17.375 GB/s,  W_data   64 b ->   8 GB/s
wide     W_raw 1217 b -> 152.125 GB/s,  W_data 1024 b -> 128 GB/s
```

Bisection counts the `2k` simplex channels crossing a vertical bisection, where a bidirectional
link counts as two channels. Aggregate injection counts all `k^2` local ports feeding the `wide`
network at line rate.

```text
B_bisection = 2k  B_link
B_injection = k^2 B_link

  B_bisection  bandwidth across a vertical cut of the mesh
  B_injection  bandwidth of all local ports injecting at line rate
  B_link       bandwidth of one simplex channel, in B/s
  k            mesh dimension, so the fabric holds k x k nodes
```

| Mesh | Bisection raw | Bisection data | Aggregate raw | Aggregate data |
|---|---:|---:|---:|---:|
| `4 x 4` | 1.217 TB/s | 1.024 TB/s | 2.434 TB/s | 2.048 TB/s |
| `16 x 16` | 4.868 TB/s | 4.096 TB/s | 38.944 TB/s | 32.768 TB/s |

### 7.3 Zero-Load Head-Flit Latency

A head flit crossing `H` hops passes `H + 1` routers, counting the source router. Serialization
occupies the source port for `L` cycles and is counted separately in §7.6.

```text
T_head = (H + 1) t_r

  T_head  zero-load head-flit latency
  H       Manhattan hop count from source to destination
  t_r     router pipeline latency per hop, in cycles
```

Mean Manhattan distance between two uniformly selected nodes in a `k x k` mesh is
`2(k^2 - 1) / 3k`. Worst case is corner to corner, `2(k - 1)`.

| Mesh | `H_mean` | `H_worst` |
|---|---:|---:|
| `4 x 4` | 2.5 | 6 |
| `16 x 16` | 10.625 | 30 |

A collective sets `H_max` from the shape of its set rather than from these, see §7.6.

### 7.4 Source and Root Network Port Traffic

| Case | Flits at the source or root network port |
|---|---:|
| Unicast baseline | `(N - 1)L` |
| In-network collective | `L` per path |

The traffic ratio is measured at the source port for multicast and at the root port for
reduction, against a baseline of repeated unicast from a single port. With the source or root at
one end of the set the ratio is `N - 1`. An interior one is served by two paths, so a source emits
one packet per direction and a root receives one per direction, which halves it either way.

### 7.5 Flit-Hop Traffic

Traffic is counted in flit-hops.

| Case | Flit-hops, row or column of `k` with the source at `s` |
|---|---:|
| Unicast baseline | `L [s(s + 1)/2 + (k - 1 - s)(k - s)/2]` |
| In-network multicast | `L(k - 1)` |

For a source at one end, `s = 0` or `s = k - 1`, the baseline reduces to `L k(k - 1)/2` and the
flit-hop ratio to `k/2`. A centered source gains less because its unicast destinations are
closer on average.

| Row or column size | Flit-hop ratio, source at one end | Source centered |
|---|---:|---:|
| `k = 4` | `2 x` | `1.33 x` |
| `k = 16` | `8 x` | `4.27 x` |

A join point combines two operands and forwards one, so every downstream link carries `L` flits
once and a reduction has the same flit-hop count as a multicast.

### 7.6 Collective Tail Latency

The three forms in §4 give tail arrival at the farthest destination, with one flit per cycle
injection, and subtracting the unicast and reduction forms gives the condition stated there.

Both `H_max` and `D` follow from where the source or root sits. At one end of the set the chain
is serial, giving `H_max` = `k - 1` and `D` = `k - 1`. At the centre the two halves converge
in parallel, giving `H_max` = `⌊k/2⌋` and `D` about `k/2`, while the two paths share one network
port and so double the serialization term to `2L`.

### 7.7 Model Assumptions

| Assumption | Effect |
|---|---|
| Zero load | No queueing, contention, or credit-stall delay is included |
| Synchronous endpoints | GALS clock-domain crossing at the AXI boundary is excluded |
| `C_offload` not characterized | The combine round trip appears in the model but has no measured value |
| Back-to-back unicast baseline | The baseline injects `N - 1` copies from one source port or receives `N - 1` arrivals at one root port |

---

## 8. Packet Format

**Flit header, 56 b.** 42 b allocated, 14 b reserved.

| Field | Bits | Width | Note |
|---|---|---|---|
| `axi_ch` | [2:0] | 3 | AXI channel code |
| `src_id` | [10:3] | 8 | source node, `{y[3:0], x[3:0]}` |
| `dst_id` | [18:11] | 8 | destination node, same composition |
| `vc_id` | [21:19] | 3 | virtual channel index, 0 to one less than the configured count |
| `flit_tail` | [22] | 1 | 1 = last flit of the packet |
| `ordering_req` | [23] | 1 | 1 = `ordering_tag` is valid |
| `ordering_tag` | [31:24] | 8 | reorder slot handle, one per transaction, so a master holds at most 256 outstanding |
| `collective_op` | [33:32] | 2 | see encoding below |
| `collective_mask` | [41:34] | 8 | wildcard node mask, same composition as `dst_id` |
| `rsvd` | [55:42] | 14 | reserved, transmitted as zero |

The 8-bit node ID limits the mesh to 16 x 16 nodes.

**`axi_ch` encoding**

| Value | Channel |
|---|---|
| 0 | AW |
| 1 | W |
| 2 | AR |
| 3 | B |
| 4 | R |
| 5 to 7 | reserved |

**`collective_op` encoding**

| Value | Name | Meaning |
|---|---|---|
| 0 | `UNICAST` | single destination, no replication or combining. `collective_mask` = 0 |
| 1 | `MULTICAST` | on a request, replicate to the set named by `collective_mask`. On a response, merge the returning responses, and an error `BRESP` dominates |
| 2 | `REDUCE` | on a request, combine operands from the set named by `collective_mask` into one arrival at `dst_id`, with addition as the operator. On a response, replicate the single response back to the set |
| 3 | reserved | |

Collectives exist only on the write path, so `AR` and `R` are always `UNICAST`. A response carries
the dual of the code its request carried, so a `MULTICAST` write is answered by a merge and a
`REDUCE` write by a replication. An operand is fetched once by an ordinary read, then distributed
by one multicast write.

**`collective_mask` encoding.** A mask bit set to 1 marks the matching node-id bit as don't care.
The mask qualifies `dst_id` while a packet travels toward the set and `src_id` while it travels
from the set, so it names the same nodes on a request and on the response that answers it.

| `collective_mask` | Node set |
|---|---|
| all zero | a single node |
| x field all ones | the whole row, Y fixed |
| y field all ones | the whole column, X fixed |

Only the id bits in use by the configured mesh are set, so the row mask is `0000_1111` at
16 x 16 and `0000_0011` at 4 x 4. Other patterns are reserved.

The named set holds `k` nodes and includes the source. A source is not a destination of its own
multicast and a root does not send to itself, so both collectives move `k - 1` operand tiles.

**`awuser` layout, 18 b.** `aruser` is 8 b.

| Field | Bits | Width |
|---|---|---|
| `user` | [7:0] | 8 |
| `collective_op` | [9:8] | 2 |
| `collective_mask` | [17:10] | 8 |

**Channel to network mapping.** A channel rides the network that suits its direction and its
message size. Narrow-class requests and responses take separate networks so that a receiver can
always accept a response. A wide read request and a wide write response are small, so they ride
`req` and `rsp` rather than occupy a wide flit. A wide write request is the exception: it stays on
`wide` with its write data, because AXI4 gives `W` no ID and a target pairs write data with write
requests by arrival order alone. A shared network does not by itself order flits that take
different virtual channels, so all `AW` and `W` from one source to one destination take the same
virtual channel, and wormhole allocation keeps each write burst contiguous. Ordering across sources
is settled at the destination: the target NI presents each write request together with its own
write data, so bursts from different sources cannot be paired with the wrong request.

| Channel | Fields | 64-bit class | 1024-bit class |
|---|---|---|---|
| `AW` | id 8, addr 64, len 8, size 3, burst 2, cache 4, lock 1, prot 3, region 4, qos 4, user 18 | 119 b, `req` | 119 b, `wide` |
| `AR` | as `AW` with `aruser` 8 b | 109 b, `req` | 109 b, `req` |
| `W` | last 1, user 8, strb `W_data`/8, data `W_data` | 81 b, `req` | 1161 b, `wide` |
| `B` | id 8, resp 2, user 8 | 18 b, `rsp` | 18 b, `rsp` |
| `R` | last 1, id 8, resp 2, user 8, data `W_data` | 83 b, `rsp` | 1043 b, `wide` |

Each network is as wide as the widest body it carries. `req` is sized by `AW` at 119 b, `rsp` by
the narrow `R` at 83 b, and `wide` by `W` at 1161 b, giving flits of 175 b, 139 b and 1217 b.

### 8.1 AXI4 Compliance and Ordering

The full AXI4 signal set is transported on every channel: INCR / WRAP / FIXED bursts, narrow and
unaligned transfers, `AxLOCK`, `AxCACHE`, `AxPROT`, `AxQOS`, `AxREGION`, and `xUSER`. Of the 18
`AWUSER` bits, 8 are carried opaquely and 10 hold collective attributes. Outstanding transactions
may complete out of order across IDs.
