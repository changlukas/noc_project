# NoC Performance Parameters

Revision 0.2, 2026-08-07.

Every parameter below moves a specific part of the latency-throughput curve. Values are
single-sourced in `specgen/source/constants.yaml`, which is authoritative. The parameters describe
the shipped c_model configuration: two AXI classes over three physical networks, REQ 137 b,
RSP 127 b and DAT 629 b, each with its own flit width and flow control (REQ and RSP ready/valid,
DAT credit). Defaults are quoted inline for convenience. Effects are stated as directions, except
in the measured baseline at the end.

Latency has two parts. Structural latency is the packet's progress through the router pipeline and
links with no contention, the floor. Queuing latency is the wait behind other traffic, and it
dominates the average once the network is loaded. Parameters that set hop count move the floor,
while buffer and outstanding depths move the queuing part.

## Parameters by curve region

| Parameter | Affects | Effect | Default (range) |
|---|---|---|---|
| `AXI_DATA_WIDTH` | Peak bandwidth, area | Sets the data-class payload, hence the DAT flit width (`DAT_FLIT_WIDTH` = 629 b = 44 b header + 585 b payload) and per-router buffer and crossbar area | 512 b (32, 64, 128, 256, 512, 1024) |
| `REQ_NUM_VC`, `RSP_NUM_VC`, `DAT_NUM_VC` | Peak bandwidth, area | Recover link bandwidth lost to head-of-line blocking, at a buffer cost that is `flit width x depth x NUM_VC` per network. Only DAT is swept by the topology set, REQ and RSP being scalar ready/valid | 1, 1, 1 (1 to 8) |
| `MESH_X_DIM`, `MESH_Y_DIM` | Latency floor | Set hop count, hence the structural transport term of every latency form in the spec | 4, 4 (2 to 16) |
| `ROUTER_VC_DEPTH` | Sustained throughput | Credit seed of the upstream sender on DAT, sized by rule 1 below | 8 (1 to 16) |
| `ROUTER_OUTPUT_FIFO_DEPTH` | Sustained throughput | Output staging, not credit-counted, absorbs transient output-port contention | 2 (1 to 16) |
| `MAX_TXNS_PER_ID` | Latency hiding | Bounds outstanding transactions per AXI ID, a sub-limit inside the shared pool below | 32 (1 to 256) |
| `ROB_B_DEPTH`, `ROB_R_DEPTH` | Latency hiding | Reorder buffer pool depths, bound in-flight write and read responses awaiting in-order return | 128, 128 (1 to 256) |
| `META_BUFFER_MAX_OUTSTANDING` | Latency hiding | Slave-side outstanding pool per direction, bounds concurrency the slave sustains | 32 (1 to 256) |
| `META_BUFFER_MAX_UNIQUE_IDS` | Endpoint concurrency | Distinct AXI IDs the NSU presents downstream. At 1 every transaction reaching a tile carries the same ID, so an endpoint that tracks IDs sees no concurrency to exploit | 1 (1 or 256) |
| `NMU_OUTSTANDING_DEPTH` | Latency hiding | Master-side shared outstanding pool per direction, all AXI IDs share it | 32 (1 to 256) |
| `NMU_QUEUE_DEPTH`, `NSU_QUEUE_DEPTH` | Burst absorption | AXI-channel FIFO depth at the master-side (NMU) and slave-side (NSU) network interfaces, absorbs injection bursts | 16, 16 (1 to 1024) |
| `NMU_DEPKT_Q_DEPTH` | Burst absorption | Depacketize demux FIFO depth | 16 (1 to 1024) |
| `NMU_ARBITER_FIFO_DEPTH`, `NSU_ARBITER_FIFO_DEPTH` | Burst absorption | Wormhole and VC-arbiter staging depth | 4, 4 (1 to 64) |

## Sizing rules

Two parameters have a minimum below which they cap performance regardless of the other parameters.
Both are minimums for full rate, not target values.

**Rule 1, credit depth for full link rate.** A credit-counted buffer must hold one flit for each
cycle from a credit being consumed until the returned credit is usable at the sender.

```text
ROUTER_VC_DEPTH  >=  C_rt

  C_rt  credit round-trip, in flits, from credit consumption to the returned credit usable
```

Below `C_rt` the link idles with no contention present, which puts the spec's derived bandwidth out
of reach at any traffic pattern. This is the spec's Credit capacity requirement.
`ROUTER_OUTPUT_FIFO_DEPTH` is not credit-counted and does not fall under this rule.

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
slot, so for read bursts its required depth scales with beats, not transactions.

Admission is a pool, not a per-ID multiplier. `NMU_OUTSTANDING_DEPTH` (32) bounds the total in
flight per direction across every ID, and `MAX_TXNS_PER_ID` (32) is a sub-limit inside it. Adding
IDs spreads that pool, it does not enlarge it, so the aggregate figure is the one every sizing
expression below is written against.

## Worked example: absorbing a full outstanding window

A master that fills its outstanding window in one shot is the peak-injection case. This example
sizes what the network interface (NI) must hold for the master to see no backpressure, per
direction. `n` and `m` are the example's local shorthand: `n` is the shared outstanding pool depth
`NMU_OUTSTANDING_DEPTH` and `m` the AXI burst length in beats.

**Write window.** At t = 0 the master drives both channels at full rate: `n` AW back to back at
one per cycle, and `n x m` W beats contiguously at one per cycle. The NI egress injects one flit
per cycle, and each transaction occupies `m + 1` egress slots, one `Aw` header plus `m` W beats.
Arrivals exceed the service rate, so both port queues peak at the same expression:

```text
aw_q peak = w_q peak = n m / (m + 1)          about n each, not n m
full absorption:  aw_q, w_q  >=  ceil(n m / (m + 1))
queues empty at n (m + 1) cycles

  n  shared outstanding pool depth, NMU_OUTSTANDING_DEPTH
  m  AXI burst length, in beats
```

At n = 32, m = 4 the peak is 25.6, so 26 entries each, and the window clears in 160 cycles. The
current port depth of 16 absorbs part of the window, after which the master sees ready fall.
Throughput is identical either way, since the egress caps it at `m / (m + 1)`:

| Capability | `aw_q`, `w_q` | Provides |
|---|---|---|
| Full absorption | `ceil(n m / (m + 1))` each | the master never stalls and the window transfers at channel rate |
| Throughput only | skid depth | the same sustained rate, with the master queuing at its own boundary |

The router needs no added depth. The NI egress injects one flit per cycle, so no router sees more
than line rate, which rule 1 already covers. The window is absorbed at the NI. The return side
is `n` single-flit B responses, tracked by `ROB_B_DEPTH >= n`, which counts transactions and
does not scale with burst beats.

**Read window.** The master issues `n` AR back to back, to several destinations so responses can
return out of order. It differs from the write window in three ways:

1. No request-side backlog. An AR is one flit, and with the request path accepting one flit per
   cycle in a pure-read stream, arrival and service are rate-matched, so `ar_q` needs skid depth
   only. The load is on the return: `n x m` R beats arrive at line rate on the response network.
2. The return parks in the reorder buffer, and undersizing it backpressures the fabric, not the
   master. The master accepts R at one beat per cycle, rate-matched, but a same-ID window spread
   over several destinations returns out of order and parks in `ROB_R` until in-order commit. In
   the worst case the first-issued burst returns last. That burst commits as it arrives, so the
   exact parked peak is `(n - 1) m` beats and `n m` is the conservative budget:

```text
ROB_R  >=  (n - 1) m beats exact, n m as the conservative budget
```

   Below that, the response path fills, credits stop returning, and the stall spreads into the
   response network and congests other nodes. A write-window shortfall backpressures only the
   master's own AXI channels. The read reorder buffer therefore carries a capacity budget and the
   port FIFOs do not.
3. No header overhead on the return. Each R flit carries one data beat, so a read window
   sustains full line rate on the response network, against `m / (m + 1)` for writes, whose
   headers share the request network with the data.

Capacity of an 8 KB read reorder budget at n = 32, m = 4. `ROB_R_DEPTH` is 128 slots, which is
exactly this budget at the shipped 512 b width:

| `AXI_DATA_WIDTH` | `n m` beats | Bytes | Against 8 KB |
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
write               32 bursts x 65 flits          = 2080 cycles
in-order read       32 bursts x 64 beats          = 2048 cycles
out-of-order read   8 KB reorder buffer / 64 B    =  128 cycles
```

Write and in-order read coverage sits far beyond any zero-load round trip, so those streams
hold the service rate unconditionally. The out-of-order read is the tight one:

```text
read efficiency = min(1, 128 / round_trip_cycles)
```

| Round trip | `<= 128` cycles | 256 | 512 | 1024 |
|---|---:|---:|---:|---:|
| Read efficiency | 100 % | 50 % | 25 % | 12.5 % |

Burst size does not change the 128-cycle coverage once the buffer fills (any burst `>= 256 B`
pends the full 8 KB). Below 256 B the 32-request cap binds first: a single-beat stream pends
only 2 KB and covers 32 cycles.

## Risk in the current defaults

The credit-path gap is closed. `ROUTER_VC_DEPTH` is 8 against a `C_rt` of 5 cycles: at the earlier
depth of 4 the link idled one cycle per credit loop and injection capped near 79 %, and depth 8
sustains 98.7 to 99.5 % (measured in `422ccdc`).

The bandwidth and area parameters, `AXI_DATA_WIDTH` and the per-network `NUM_VC`, are the largest
knobs on both axes and interact. Router input buffering is their product, so raising the data class
width and the channel count together raises buffer area faster than either alone.

Full-window absorption is still uncovered on the write side: the port depth is 16 against the 26
the write window needs. The read side is covered, `ROB_R_DEPTH` being 128 beats, which is the 8 KB
budget the read window asks for at the 512 b width. Only the read side can back up into the fabric,
which is why it was sized first.
