# NoC performance probe

The probe taps the running design and reports per-transaction latency,
per-interface throughput and occupancy, and link backpressure. One run produces
one machine-readable file and one on-screen summary. The monitors only observe,
so the design behaves identically whether they are on or off.

## 1. Overview

The probe answers two questions about a run: how the design performs across all
traffic, and what happened to any single transaction. It observes the running
design without perturbing it, so the numbers reflect the design's own behavior.

- **Profile** view: aggregate counters and per-class latency for each interface.
  The minimum of a class is its best observed latency; the spread up to the
  maximum reflects interference from competing traffic.
- **Trace** view: one record per completed transaction, for drilling into an
  outlier the Profile view flags.

Both views come from one run. The best-case reference is the smallest latency
seen in that same run, not a separate idle pass.

**Figure 1: Probe placement on the 2-node testbench.** All taps are input-only.

```text
        NODE 0                                              NODE 1
   AXI master                                            AXI master
      |                                                       |
    [M0] monitor                                  monitor [M1]   <- manager edge
      |                                                       |
   network interface                                  network interface
      |                                                       |
   +--------+         [L0>1] link monitor          +--------+
   |Router 0|========================================>|Router 1|
   |        |<========================================|        |
   +--------+         [L1>0] link monitor          +--------+
      |                                                       |
   network interface                                  network interface
      |                                                       |
    [S0] monitor                                  monitor [S1]   <- memory edge
      |                                                       |
   AXI memory                                            AXI memory
```

## 2. Probe architecture

One monitor sits on each AXI interface (the manager edge between master and
network interface, and the memory edge between network interface and memory), one
monitor sits on each link direction, and router queue occupancy is sampled every
cycle. All of them feed a single collector that writes the output; none of them
drive any signal in the design. Each scenario is launched with
`make run-tb-top SCENARIO=<scenario-id>`, which produces `perf.json` beside the
run log and prints the summary.

**Figure 2: Monitor structure and data flow.**

```text
   AXI interface wires                       inter-router link wires
   (manager + memory edges)                  (one per direction)
          |  input-only tap                         |  input-only tap
          v                                         v
   +-----------------------------+           +-----------------------------+
   | AXI interface monitor       |           | link monitor                |
   |   transaction latency       |           |   flit count                |
   |   idle / outstanding        |           |   backpressure (stall)      |
   +--------------+--------------+           +--------------+--------------+
                  |                                         |
                  +----------------+------------------------+
   router queue occupancy -------->|  (sampled every cycle)
                                   v
                  +------------------------------------------+
                  | collector   (observe only, no drives)    |
                  +---------------------+--------------------+
                                        v
                            perf.json + on-screen summary
```

## 3. Event counting (Profile view)

Each AXI monitor accumulates a fixed set of metrics for its interface:

| Metric | Meaning |
|---|---|
| Write / Read Transaction Count | Completed write / read transactions. |
| Write / Read Byte Count | Bytes written / read; the basis for throughput. |
| Write Latency | From write-address acceptance to the write response. |
| Read Latency | From read-address acceptance to the last read data beat. |
| Slave Write Idle Cycle Count | Cycles the write data waits because the receiving side is not ready. |
| Master Read Idle Cycle Count | Cycles the read data waits because the reading side is not ready. |
| Outstanding (peak) | Most issued-but-uncompleted transactions seen at once. |

The memory edge adds a **service latency** (how long the memory takes to answer),
each link reports **backpressure** (cycles stalled because the downstream buffer
is full), and each router reports its peak **input and output queue occupancy**.

The idle counts are interface-local. On the memory edge the write-idle count is
the memory stalling; on the manager edge the receiver is the network interface,
so the same count is the network interface holding off the master.

**Figure 3: Where each latency starts and stops.**

```text
Write transaction
  start = write address accepted                 end = write response
    |                                                 |
    v                                                 v
  --+--[ AW ]--[ W ... last ]-------------[ B ]-------+--
    |<================ write latency =================>|

Read transaction
  start = read address accepted                    end = last read beat
    |                                                   |
    v                                                   v
  --+--[ AR ]------------[ R ][ R ] ... [ last R ]------+--
    |<================ read latency ===================>|
```

From these counters the Profile view derives:

- **Throughput**: byte count over the run window.
- **Per-class latency**: minimum, mean, and maximum, grouped by operation,
  source, destination, burst length, and transfer size.
- **Latency distribution**: a count of transactions in each fixed range
  (0-16, 16-32, 32-64, 64-128, 128-256, and 256+ cycles).

Per-class latency is measured at the manager edge, end to end: the master's view
of the whole round trip.

Reading the Profile view:

| Observation | Likely cause |
|---|---|
| A class's maximum far above its minimum | downstream contention on that flow |
| High peak outstanding | reorder or queue pressure at the interface |
| Link backpressure above zero | that link's downstream buffer filled |
| High write-idle | receiver slow to accept write data |
| High read-idle | reader slow to accept read data |

## 4. Event logging (Trace view)

The Trace view keeps one record per completed transaction: direction, source,
destination, the cycle it was accepted, the cycle it completed, its latency, and
its byte count. Use it to inspect the transaction behind a Profile outlier. These
records are written to the output file only, not the on-screen summary, and there
is no streaming trace output.

## 5. Latency composition

The numbers below are one measured instance, not a fixed specification: a single
32-byte transaction on the 2-node testbench. They move with scenario and
configuration, so re-measure before quoting.

**Figure 4: Round-trip latency composition (segments to scale; the number above
each boundary is the cumulative cycle; Shell is the non-architectural co-sim
residual).**

```text
Write round-trip = 27 cyc
  0            10                22      25   27
  |--- NI:10 --|--- Router:12 ---|-Slv:3-|Sh:2|

Read round-trip = 28 cyc
  0            10                22    24     28
  |--- NI:10 --|--- Router:12 ---|Slv:2|-Sh:4-|
```

| Component | Write | Read | Source |
|---|---|---|---|
| Network interface | 10 | 10 | staged pipeline, four paths |
| Router | 12 | 12 | three-stage pipeline, four traversals |
| Memory service | 3 | 2 | memory latency plus handshake |
| Shell boundary | 2 | 4 | co-sim wrapper registers; non-architectural residual |

The network interface and the router each advance one stage per cycle. The shell
term is the residual: the measured total minus the network-interface, router, and
memory contributions.

## 6. Limitations

- Not implemented: per-id aggregate breakdown, periodic time-series snapshots, a
  streaming event-log output, memory-mapped register readout, and external-trigger
  gating.
- Backpressure and idle counts have so far been exercised only on lightly loaded
  scenarios, where they read zero; stress scenarios are still needed.
- The read-versus-write difference in the shell-boundary residual is a co-sim
  artifact and is not yet root-caused.
- The probe is co-sim only; it reports no area or power.

The full field-level layout of the output file is kept in the design spec.
