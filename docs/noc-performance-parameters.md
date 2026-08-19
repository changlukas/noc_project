# NoC Performance Parameters

Revision 0.2, 2026-08-07.

Every parameter below moves a specific part of the latency-throughput curve. Implemented values
are single-sourced in `specgen/source/constants.yaml`, which is authoritative. Target-only
parameters are marked and enter specgen when their RTL is implemented. The parameters describe two
AXI classes over three physical networks. At the default `AXI_ID_WIDTH = 3`, REQ is 136 b, RSP is
126 b and DAT is 633 b, each with its own flow control (REQ and RSP ready/valid, DAT credit).
Defaults are quoted inline for convenience. Effects are stated as directions, except in the
measured baseline at the end.

Latency has two parts. Structural latency is the packet's progress through the router pipeline and
links with no contention, the floor. Queuing latency is the wait behind other traffic, and it
dominates the average once the network is loaded. Parameters that set hop count move the floor,
while buffer and outstanding depths move the queuing part.

## Parameters by curve region

| Parameter | Affects | Effect | Default (range) |
|---|---|---|---|
| `AXI_ID_WIDTH` [target derived width] | Outstanding capacity, REQ/RSP area | Sets the NoC-carried ID width and the per-ID state count. REQ flit width is `133 + AXI_ID_WIDTH`; RSP is `123 + AXI_ID_WIDTH`; DAT remains 633 b over the legal range because DataW is wider than DataR | 3 (1 to 8); current model locked at 3 |
| `AXI_DATA_WIDTH` | Peak bandwidth, area | Sets the data-class payload, hence the DAT flit width (`DAT_FLIT_WIDTH` = 633 b = 48 b header + 585 b payload) and per-router buffer and crossbar area | 512 b (32, 64, 128, 256, 512, 1024) |
| `REQ_NUM_VC`, `RSP_NUM_VC`, `DAT_NUM_VC` | Peak bandwidth, area | Recover link bandwidth lost to head-of-line blocking, at a buffer cost that is `flit width x depth x NUM_VC` per network. Only DAT is swept by the topology set, REQ and RSP being scalar ready/valid | implemented 1, 1, 1; target 1, 1, 2 (1 to 8) |
| `NOC_DAT_VC_MODE` [target] | Head-of-line blocking, usable capacity | `SHARED` lets every DAT class use every VC. `READ_WRITE_SPLIT` reserves equal lower/upper halves for Write/Read, removing cross-class blocking but potentially stranding capacity under asymmetric traffic | `SHARED` (`SHARED`, `READ_WRITE_SPLIT`); Split requires `DAT_NUM_VC` in {2, 4, 6, 8} |
| `MESH_X_DIM`, `MESH_Y_DIM` | Latency floor | Set hop count, hence the structural transport term of every latency form in the spec | 4, 4 ({2, 4, 8, 16} independently for unicast; equal dimensions for v1 collective signoff) |
| `NOC_ROUTER_VC_DEPTH` | Sustained throughput | Credit seed of the upstream sender on DAT, sized by rule 1 below | 8 `[TBD]` (power of two, >= 2) |
| `AXI_FIFO_DEPTH` [target] | Clock-domain elasticity, area | Common entry count of the AW/W/AR/B/R dual-clock FIFOs on each AXI interface. Absorbs clock-ratio and temporary AXI backpressure rather than live transaction state | 8 `[TBD]` (power of two, >= 2) |
| `NOC_FIFO_DEPTH` [target] | Burst absorption | Common depth of the synchronous `noc_clk` REQ/RSP/DATW/DATR FIFOs after channel assignment; not replicated per VC | 8 `[TBD]` (positive power of two) |
| `ROUTER_OUTPUT_FIFO_DEPTH` | Sustained throughput | Output staging, not credit-counted, absorbs transient output-port contention | 8 `[TBD]` (positive power of two) |
| `MAX_TXNS_PER_ID` | Latency hiding | Bounds outstanding transactions per AXI ID. Nothing sits above it, so it is the master-side admission limit and `MAX_TXNS_PER_ID x 2^AXI_ID_WIDTH` is the whole window. Measured on `mesh_4x4` at 4 VCs, `all_to_all`: exactly its cap at one id per initiator, 31 of 32 at four ids under continuous checked injection, 21 of 32 on the directed run. Whether it binds follows the traffic's id count and load | 32 (1 to 256) |
| `ROB_B_DEPTH`, `ROB_R_DEPTH` | Latency hiding | Reorder buffer pool depths, bound in-flight write and read responses awaiting in-order return. `ROB_R_DEPTH` is what binds first under sustained load: measured full, 128 of 128, on `mesh_4x4` at 4 VCs, `all_to_all` under continuous checked injection, against 60 on the directed run | 128, 128 (1 to 256) |
| `META_BUFFER_MAX_OUTSTANDING` | Latency hiding | Slave-side outstanding pool per direction, bounds concurrency the slave sustains | 32 (1 to 256) |
| `META_BUFFER_MAX_UNIQUE_IDS` | Endpoint concurrency | Distinct AXI IDs the NSU presents downstream. At 1 every transaction reaching a tile carries the same ID, so an endpoint that tracks IDs sees no concurrency to exploit | 1 (1 or 8) |
| `NMU_QUEUE_DEPTH`, `NSU_QUEUE_DEPTH` [current model] | Burst absorption | Single-clock AXI-channel queue depth in the C++ model; target CDC uses `AXI_FIFO_DEPTH` | 16, 16 (1 to 1024) |
| `NMU_DEPKT_Q_DEPTH` | Burst absorption | Depacketize demux FIFO depth | 16 (1 to 1024) |
| `NMU_ARBITER_FIFO_DEPTH`, `NSU_ARBITER_FIFO_DEPTH` [current model] | Burst absorption | Current C++ wormhole and VC-arbiter staging depth; not a target NI per-VC FIFO | 4, 4 (1 to 64) |

## Sizing rules

Two parameters have a minimum below which they cap performance regardless of the other parameters.
Both are minimums for full rate, not target values.

**Rule 1, credit depth for full link rate.** A credit-counted buffer must hold one flit for each
cycle from a credit being consumed until the returned credit is usable at the sender.

```text
CREDITED_INPUT_DEPTH  >=  C_rt

  C_rt  credit round-trip, in flits, from credit consumption to the returned credit usable
```

For DAT, `CREDITED_INPUT_DEPTH` is `NOC_ROUTER_VC_DEPTH`: only Router inputs own per-VC credited
storage. Router-to-NI ejection uses ready/valid and therefore has no NI credit-depth term. Below
`C_rt` an inter-router or NI-to-Router link idles with no contention present, which puts the spec's
derived bandwidth out of reach. `ROUTER_OUTPUT_FIFO_DEPTH` is not credit-counted and does not fall
under this rule.

**Rule 2, outstanding depth to hide latency.** To keep a single ID stream at its request rate, the
in-flight transactions must cover the round-trip, the bandwidth-delay product.

```text
MAX_TXNS_PER_ID  >=  T_rt  r_id      per AXI ID

  T_rt   round-trip, from a transaction accepted to its per-ID slot freed, in cycles
  r_id   request rate the master drives on that ID, in transactions per cycle
```

Below the bound a single ID stream is latency-limited, its throughput capped at
`MAX_TXNS_PER_ID / T_rt` rather than the link rate. `ROB_B_DEPTH` and `META_BUFFER_MAX_OUTSTANDING`
must not be the tighter bound on the same concurrency. `ROB_R_DEPTH` holds one read-data beat per
slot, so for read bursts its required depth scales with beats, not transactions. NI DAT and CDC
FIFO depths are not terms in this outstanding bound; they can cause backpressure without losing
the transaction state held in the order lists, RoBs and Response Queue.

Admission is a per-ID multiplier, not a pool. `MAX_TXNS_PER_ID` (32) bounds each ID's order list
and nothing sits above it, so the master-side bound per direction is
`MAX_TXNS_PER_ID x 2^AXI_ID_WIDTH` = 32 x 8 = 256. Adding IDs enlarges the window instead of
sharing one out, and a single-ID stream still sees 32. `ROB_B_DEPTH` and `ROB_R_DEPTH` gate only
the transactions that reserve a slot, so they bind before 256 as soon as traffic leaves the
bypass branches.

## Worked example: absorbing a full outstanding window

A master that fills its outstanding window in one shot is the peak-injection case. This example
sizes what the network interface (NI) must hold for the master to see no backpressure, per
direction. `n` and `m` are the example's local shorthand: `n` is the master-side outstanding bound
`MAX_TXNS_PER_ID x 2^AXI_ID_WIDTH` = 256, reachable only with all 8 IDs in use, and `m` the AXI
burst length in beats.

**Write window.** At t = 0 the master drives both channels at full rate: `n` AW back to back at
one per cycle, and `n x m` W beats contiguously at one per cycle. The NI egress injects one flit
per cycle, and each transaction occupies `m + 1` egress slots, one `Aw` header plus `m` W beats.
Arrivals exceed the service rate, so both port queues peak at the same expression:

```text
aw_q peak = w_q peak = n m / (m + 1)          about n each, not n m
full absorption:  aw_q, w_q  >=  ceil(n m / (m + 1))
queues empty at n (m + 1) cycles

  n  master-side outstanding bound, MAX_TXNS_PER_ID x 2^AXI_ID_WIDTH
  m  AXI burst length, in beats
```

At n = 256, m = 4 the peak is 204.8, so 205 entries each, and the window clears in 1280 cycles. The
current port depth of 16 absorbs part of the window, after which the master sees ready fall.
Throughput is identical either way, since the egress caps it at `m / (m + 1)`:

| Capability | `aw_q`, `w_q` | Provides |
|---|---|---|
| Full absorption | `ceil(n m / (m + 1))` each | the master never stalls and the window transfers at channel rate |
| Throughput only | skid depth | the same sustained rate, with the master queuing at its own boundary |

The router needs no added depth. The NI egress injects one flit per cycle, so no router sees more
than line rate, which rule 1 already covers. The window is absorbed at the NI. The return side
is `n` single-flit B responses. `ROB_B_DEPTH` tracks only the ones that reserve a slot and
refuses an allocating AW when the pool is short, so the shipped 128 caps the slot-allocating
share of a 256-deep window. It counts transactions and does not scale with burst beats.

**Read window.** The master issues `n` AR back to back, to several destinations so responses can
return out of order. It differs from the write window in three ways:

1. No request-side backlog. An AR is one flit, and with the request path accepting one flit per
   cycle in a pure-read stream, arrival and service are rate-matched, so `ar_q` needs skid depth
   only. The load is on the return: `n x m` R beats arrive at line rate on the response network.
2. The return parks in the reorder buffer, and undersizing it backpressures the fabric, not the
   master. The master accepts R at one beat per cycle, rate-matched, but a same-ID window spread
   over several destinations returns out of order and parks in `ROB_R` until in-order commit. In
   the worst case the first-issued burst returns last. That burst commits as it arrives, so
   covering `k` concurrent reordered bursts needs `(k - 1) m` beats exactly, `k m` as the
   conservative budget:

```text
ROB_R  >=  (k - 1) m beats exact, k m as the conservative budget

  k  concurrent reordered read bursts, at most n
```

   Below that, the response path fills, credits stop returning, and the stall spreads into the
   response network and congests other nodes. A write-window shortfall backpressures only the
   master's own AXI channels. The read reorder buffer therefore carries a capacity budget and the
   port FIFOs do not.
3. No header overhead on the return. Each R flit carries one data beat, so a read window
   sustains full line rate on the response network, against `m / (m + 1)` for writes, whose
   headers share the request network with the data.

The pool is its own admission gate: an allocating AR is refused when free slots are short, so `k`
never exceeds `ROB_R_DEPTH / m`, which is 32 reordered bursts at the shipped 128 slots and m = 4.
Those 128 slots are exactly the 8 KB budget at the shipped 512 b width:

| `AXI_DATA_WIDTH` | Beats parked | Bytes | Against 8 KB |
|---|---:|---:|---|
| 256 b | 128 | 4 KB | fits |
| 512 b (shipped) | 128 | 8 KB | exactly fills it |
| 1024 b | 128 | 16 KB | exceeds it, 8 KB forces `m <= 2` |

At a fixed budget and outstanding count, widening the data width shortens the admissible burst.
This accounting lands on the `DAT` network, which carries write headers, write data and read
returns against one budget.

**Efficiency versus round trip.** Moved from target spec §7.5, which keeps the qualitative
conclusion only. Round trip = request issue to the last returned beat, in NoC cycles. Each
depth covers a round trip as long as its own streaming time (target spec numbers, 512 b data
class, 4 KB bursts):

```text
write                     256 bursts x 65 flits         = 16640 cycles
in-order read, Enabled    256 bursts x 64 beats         = 16384 cycles
in-order read, Disabled   256 bursts x 64 beats         = 16384 cycles
out-of-order read         8 KB reorder buffer / 64 B    =   128 cycles
```

`RobMode::Disabled` (`nmu.READ_ROB_ENABLED: 0`) keeps up to 32 same-ordering-domain reads per ID,
so its in-order read window remains 32 x 8 IDs. A `{dst_id, dst_port_id, AXI class}` change waits
until that ID becomes idle. The current C++ model has not yet been aligned to this target behavior.

Write and in-order read coverage sits far beyond any zero-load round trip, so those streams
hold the service rate unconditionally. The out-of-order read is the tight one:

```text
read efficiency = min(1, 128 / round_trip_cycles)
```

| Round trip | `<= 128` cycles | 256 | 512 | 1024 |
|---|---:|---:|---:|---:|
| Read efficiency | 100 % | 50 % | 25 % | 12.5 % |

Burst size does not change the 128-cycle coverage as long as enough IDs are in play: the 128-slot
pool pends the full 8 KB at any burst length, but filling it with single-beat requests takes 128
concurrent transactions, which needs at least 4 IDs at `MAX_TXNS_PER_ID` = 32 each. A single-ID
single-beat stream hits the per-ID gate at 32 requests, pends only 2 KB and covers 32 cycles.

## Risk in the current defaults

The credit-path gap is closed. `NOC_ROUTER_VC_DEPTH` is 8 against a `C_rt` of 5 cycles: at the earlier
depth of 4 the link idled one cycle per credit loop and injection capped near 79 %, and depth 8
sustains 98.7 to 99.5 % (measured in `422ccdc`).

The bandwidth and area parameters, `AXI_DATA_WIDTH` and the per-network `NUM_VC`, are the largest
knobs on both axes and interact. Router input buffering is their product, so raising the data class
width and the channel count together raises buffer area faster than either alone.
`NOC_DAT_VC_MODE` does not change the number or depth of NI buffers: it changes only the
eligible-VC mask used by NI injection and every DAT router output VA. The NI has no per-VC receive
scheduler. Router-to-NI DAT traffic enters the Read or Write class FIFO under ready/valid, while
Router VC arbitration remains responsible for choosing the ejected flit.
After NMU classification, Narrow and Data writes use independent REQ and DAT assignment and class
FIFO paths. They may each emit one flit per `noc_clk`; their combined throughput is limited by the
shared AXI source only while filling those buffers, not while draining an existing backlog.

Full-window absorption is still uncovered on the write side: the port depth is 16 against the 205
the write window needs. The read side is covered, `ROB_R_DEPTH` being 128 beats, which is the 8 KB
budget the read window asks for at the 512 b width. Only the read side can back up into the fabric,
which is why it was sized first.
