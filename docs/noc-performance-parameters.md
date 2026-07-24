# NoC Performance Parameters

Revision 0.1, 2026-07-24, draft.

This page maps each configurable parameter to the part of the latency-throughput curve it moves.
It answers which parameters affect performance and where the current defaults carry risk. Values
are single-sourced in `specgen/source/constants.yaml`, which is authoritative. Defaults are quoted
inline for reading only, and none of the effects below is a measured number. Each is a direction,
stated as the parameter raising, lowering, or gating a quantity.

Latency has two parts. Structural latency is the packet's progress through the router pipeline and
links with no contention, the floor. Queuing latency is the wait behind other traffic, and it
dominates the average once the network is loaded. Parameters that set hop count move the floor,
while buffer and outstanding depths move the queuing part.

## Parameters by curve region

| Parameter | Affects | Effect | Default (range) |
|---|---|---|---|
| `AXI_DATA_WIDTH` | Peak bandwidth, area | Sets flit width, so it sets both payload bandwidth and per-router buffer and crossbar area | 256 b (32, 64, 128, 256, 512, 1024) |
| `NUM_VC` | Peak bandwidth, area | Recovers link bandwidth lost to head-of-line blocking, at a buffer cost that is `flit width x depth x NUM_VC`, so it scales with data width | 1 (1 to 8) |
| `MESH_X_DIM`, `MESH_Y_DIM` | Latency floor | Set hop count, hence the structural transport term of every latency form in the spec | 4, 4 (1 to 16) |
| `ROUTER_VC_DEPTH` | Sustained throughput | Credit seed of the upstream sender, sized by rule 1 below | 4 (1 to 16) |
| `SLAVE_VC_BUFFER_DEPTH` | Sustained throughput | Initial credit exposed to the producer, sized by rule 1 below | 4 (1 to 64) |
| `ROUTER_OUTPUT_FIFO_DEPTH` | Sustained throughput | Output staging, not credit-counted, absorbs transient output-port contention | 2 (1 to 16) |
| `MAX_TXNS_PER_ID` | Latency hiding | Bounds outstanding transactions per AXI ID, hence the memory latency a master can hide behind concurrency | 32 (1 to 256) |
| `ROB_B_DEPTH`, `ROB_R_DEPTH` | Latency hiding | Reorder buffer pool depths, bound in-flight write and read responses awaiting in-order return | 32, 32 (1 to 256) |
| `META_BUFFER_MAX_OUTSTANDING` | Latency hiding | Slave-side outstanding pool per direction, bounds concurrency the slave sustains | 32 (1 to 256) |
| `NMU_QUEUE_DEPTH`, `NSU_QUEUE_DEPTH` | Burst absorption | AXI-channel FIFO depth at the master and slave ports, absorbs injection bursts | 16, 16 (1 to 1024) |
| `NMU_DEPKT_Q_DEPTH`, `NSU_DEPKT_Q_DEPTH` | Burst absorption | Depacketize demux FIFO depth | 16, 16 (1 to 1024) |
| `NMU_ARBITER_FIFO_DEPTH`, `NSU_ARBITER_FIFO_DEPTH` | Burst absorption | Wormhole and VC-arbiter staging depth | 4, 4 (1 to 64) |

## Sizing rules

Two parameters have a minimum below which they cap performance regardless of everything else. Both
are minimums for full rate, not target values. Every term is symbolic, none is measured.

**Rule 1, credit depth for full link rate.** A credit-counted buffer must hold one flit for each
cycle from a credit being consumed until the returned credit is usable at the sender.

```text
ROUTER_VC_DEPTH, SLAVE_VC_BUFFER_DEPTH  >=  C_rt

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
slot, so for read bursts its required depth scales with beats, not transactions. Across several IDs
a master multiplies concurrency, bounded in total by the `2^AXI_ID_WIDTH` tag space, 256 at the
current 8-bit ID.

## Worked example, absorbing one full outstanding window

A master that fills its outstanding window in one shot is the peak-injection case. This example
sizes what the NI must hold for the master to see no backpressure, per direction. `n` and `m` are
the example's local shorthand: `n` is the outstanding depth `MAX_TXNS_PER_ID` and `m` the AXI burst
length in beats. The window uses rule 2's outstanding value as its size and adds an absorption
dimension the rules do not cover.

**Write window.** At t = 0 the master drives both channels at rate: `n` AW back to back at one per
cycle, and `n x m` W beats contiguously at one per cycle. The NI egress drains one flit per cycle,
and each transaction occupies `m + 1` egress slots, one `Aw` header plus `m` W beats. Arrival
outruns drain, and both port queues peak at the same expression:

```text
aw_q peak = w_q peak = n m / (m + 1)          about n each, not n m
full absorption:  aw_q, w_q  >=  ceil(n m / (m + 1))
drain completes at n (m + 1) cycles

  n  outstanding depth, MAX_TXNS_PER_ID
  m  AXI burst length, in beats
```

At n = 32, m = 4 the peak is 25.6, so 26 entries each, and the window drains in 160 cycles. The
current port depth of 16 absorbs part of the window, after which the master sees ready fall.
Throughput is identical either way, since the egress caps it at `m / (m + 1)`:

| Capability | `aw_q`, `w_q` | Buys |
|---|---|---|
| Full absorption | `ceil(n m / (m + 1))` each | the master never stalls and the window transfers at channel rate |
| Throughput only | skid depth | the same sustained rate, with the master queuing at its own boundary |

The router is unaffected. The flood cannot pass the one-flit-per-cycle NI egress, so every router
sees line rate and rule 1 already covers it. The NI is the shock absorber. The return side is `n`
single-flit B responses, tracked by `ROB_B_DEPTH >= n`, which counts transactions and does not
scale with burst beats.

**Read window, the symmetric case.** The master issues `n` AR back to back, to several destinations
so responses can return out of order. Three asymmetries against the write window:

1. No request-side backlog. An AR is one flit, and with the request path accepting one flit per
   cycle in a pure-read stream, arrival and drain are rate-matched, so `ar_q` needs skid depth
   only. The flood is on the return: `n x m` R beats arrive at line rate on the response network.
2. The return parks in the reorder buffer, and undersizing it backpressures the fabric, not the
   master. The master drains R at one beat per cycle, rate-matched, but a same-ID window spread
   over several destinations returns out of order and parks in `ROB_R` until in-order commit. In
   the worst case the first-issued burst returns last. That burst commits as it arrives, so the
   exact parked peak is `(n - 1) m` beats and `n m` is the conservative budget:

```text
ROB_R  >=  (n - 1) m beats exact, n m as the conservative budget
```

   Below that, the response path fills, credits stop returning, and the stall spreads into the
   response network and congests other nodes. A write-window shortfall backpressures only the
   master's own AXI channels. This asymmetry is why the read reorder buffer carries a capacity
   budget and the port FIFOs do not.
3. No header tax on the return. Each R flit carries one data beat, so a read window sustains full
   line rate on the response network, against `m / (m + 1)` for writes, whose headers share the
   request network with the data.

Capacity of the 8 KB read reorder budget at n = 32, m = 4:

| `W_data` | `n m` beats | Bytes | Against 8 KB |
|---|---:|---:|---|
| 256 b | 128 | 4 KB | fits |
| 512 b | 128 | 8 KB | exactly fills it |
| 1024 b | 128 | 16 KB | exceeds it, 8 KB forces `m <= 2` |

At a fixed budget and outstanding count, widening the data width shortens the admissible burst. On
the target spec's three physical networks the same accounting moves to the wide network, where
write headers, write data, and read returns share one budget.

## Current defaults, where the risk is

The credit-path parameters are the ones to characterize first. `ROUTER_VC_DEPTH` is 4 today, and
whether 4 covers `C_rt` in rule 1 is not characterized. It is the first item to measure, since every
bandwidth figure in the spec assumes the bound is met. This mirrors how the spec leaves the buffer
turnaround time as an open characterization item rather than a fixed number.

The bandwidth and area parameters, `AXI_DATA_WIDTH` and `NUM_VC`, are the largest knobs on both
axes and interact. Router input buffering is their product, so raising the wide data width and the
channel count together raises buffer area faster than either alone.

Full-window absorption is not covered at the current defaults, a port depth of 16 against the 26
the write window needs, and a `ROB_R_DEPTH` of 32 beats against the 128 the read window budgets.
Of the two windows only the read side can back up into the fabric, which is why the read reorder
budget is fixed first, at 8 KB, and the 8 KB figure corresponds to 128 slots at the 512 b width.
