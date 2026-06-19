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
  The minimum of a class is its best observed latency. The spread up to its
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
cycle. All of them feed a single collector that writes the output. None of them
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

## 3. Operation

A monitor turns interface activity into counts in three steps.

**Figure 3: Measurement flow.**

```text
   INPUT                      COMPUTE                      OUTPUT
   tap interface, link,  -->  detect each handshake,  -->  derive throughput and
   and router-queue           update the counters          per-class latency,
   wires (read-only)          (one set per monitor)        write perf.json
                                                           and the summary
```

**Input.** A monitor taps the wires of one AXI interface or one link, and reads
the router queue levels. Every tap is read-only.

**Compute.** On each clock the monitor detects the handshakes on its interface and
updates the counters defined in Section 4. Each transaction's start cycle is held
in a per-id queue, so writes and reads stay matched while several are in flight.

**Output.** At the end of the run the collector gathers every counter, derives the
throughput and per-class latency, and writes `perf.json` and the on-screen summary.

## 4. Event counting (Profile view)

Each AXI monitor accumulates a fixed set of metrics for its interface:

- **Write / Read Transaction Count**: Total number of completed write or read
  transactions at the interface.
- **Write / Read Byte Count**: Total number of bytes written or read. Used to
  compute throughput.
- **Write Latency**: The time from write-address acceptance (`AWVALID & AWREADY`)
  to the write response (`BVALID & BREADY`).
- **Read Latency**: The time from read-address acceptance (`ARVALID & ARREADY`) to
  the last read beat (`RVALID & RREADY & RLAST`).
- **Slave Write Idle Cycle Count**: The number of clocks the write data is held
  between `WVALID` and `WREADY` assertion, when the receiving side is not ready.
- **Master Read Idle Cycle Count**: The number of clocks the read data is held
  between `RVALID` and `RREADY` assertion, when the reading side is not ready.
- **Outstanding**: The largest number of accepted but not completed transactions
  seen at once.

Three more groups of counters sit outside the per-interface set. The memory edge
adds a **service latency**, the time the memory takes to answer a transaction.
Each link reports its **flit count** and a **stall count**, the number of clocks
it has no downstream credit (`credit == 0`). Each router reports its peak **input
and output queue occupancy**.

The idle counts are interface-local. On the memory edge the write-idle count is
the memory stalling. On the manager edge the receiver is the network interface, so
the same count is the network interface holding off the master.

**Figure 4: Where each latency starts and stops.**

```text
Write transaction
  start = AWVALID & AWREADY       end = BVALID & BREADY
    |                                                  |
    v                                                  v
  --+--[ AW ]--[ W ... WLAST ]----------[ B ]----------+--
    |<================ write latency =================>|

Read transaction
  start = ARVALID & ARREADY end = RVALID & RREADY & RLAST
    |                                                  |
    v                                                  v
  --+--[ AR ]--------[ R ][ R ] ... [ R=RLAST ]--------+--
    |<================= read latency =================>|
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

| Symptom | Likely cause |
|---|---|
| A class's maximum far above its minimum | downstream contention on that flow |
| High peak outstanding | reorder or queue pressure at the interface |
| Link backpressure above zero | that link's downstream buffer filled |
| High write-idle | receiver slow to accept write data |
| High read-idle | reader slow to accept read data |

## 5. Event logging (Trace view)

The Trace view keeps one record per completed transaction: direction, source,
destination, the cycle it was accepted, the cycle it completed, its latency, and
its byte count. Use it to inspect the transaction behind a Profile outlier. These
records are written to the output file only, not the on-screen summary, and there
is no streaming trace output.

## 6. Latency composition

The numbers below are one measured instance, not a fixed specification: a single
32-byte transaction on the 2-node testbench. They move with scenario and
configuration, so re-measure before quoting.

**Figure 5: Round-trip latency composition. Segments are to scale, the number
above each boundary is the cumulative cycle, and Shell is the non-architectural
co-sim residual.**

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
| Shell boundary | 2 | 4 | co-sim wrapper registers, non-architectural residual |

The network interface and the router each advance one stage per cycle. The shell
term is the residual: the measured total minus the network-interface, router, and
memory contributions.

The full field-level layout of the output file is kept in the design spec.
