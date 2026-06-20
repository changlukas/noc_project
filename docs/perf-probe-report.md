# NoC performance probe

## 1. Probe architecture and scope

A passive probe for the NoC co-simulation. It monitors the running design and
writes one `perf.json` and one on-screen summary per scenario, without changing
the design's timing.

The probe implements a subset of a standard AXI performance monitor: its two
data-collection functions, event counting and event logging. Figure 1 gives the
top-level view. Monitors detect events at the interfaces and do the first-level
counting and correlation. The collector aggregates them into the two outputs:
event counting produces the aggregate metrics, event logging the per-transaction
records.

**Figure 1: Monitor structure and data flow.**

```text
   AXI interface signals         inter-router link signals       router queues
   (manager + subordinate)       (one per direction)
          |  input-only                 |  input-only                 |  sampled
          v                             v                             v
   +---------------+            +---------------+                     |
   | AXI monitor   |            | link monitor  |                     |
   | latency,      |            | flit count,   |                     |
   | idle          |            | stall         |                     |
   +-------+-------+            +-------+-------+                      |
           |                            |                             |
           +-------------+--------------+----------(occupancy)--------+
                         v
            +-----------------------------+
            | collector  (read-only)      |
            |   event counting  ----------|--> aggregate metrics
            |   event logging   ----------|--> per-transaction records
            +--------------+--------------+
                           v
                 perf.json + on-screen summary
```

- Non-intrusive: the design runs identical cycles with the monitors on or off.
- One run produces both outputs: aggregate metrics (event counting) and one record
  per transaction (event logging).

Not implemented: runtime metric selection, ID filtering, periodic time-series
snapshots (a sample-interval timer), external event and trigger inputs,
memory-mapped register access, interrupts, and a streaming event-log output.

## 2. Placement and testbench environment

**Figure 2: Probe placement on the 2-node testbench.** All monitors are input-only.

```text
        NODE 0                                              NODE 1
   AXI master                                            AXI master
      |                                                       |
    [M0] monitor                                  monitor [M1]   <- manager interface
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
    [S0] monitor                                  monitor [S1]   <- subordinate interface
      |                                                       |
   AXI memory                                            AXI memory
```

Components:

| File | Language | Role |
|---|---|---|
| `axi_perf_monitor.sv` | SystemVerilog | Monitors one AXI interface: latency, idle |
| `flit_link_perf_monitor.sv` | SystemVerilog | Monitors one link: flit count, stall |
| `cmodel_dpi.cpp` | C++ (DPI-C) | Receive monitor events from RTL |
| `main.cpp` | C++ | Verilator harness: drive per-cycle sampling and the final dump |
| `perf_collector.hpp` | C++ | Aggregate every monitor, write `perf.json` |
| `perf_cli_summary.py` | Python | Print the on-screen summary |

Testbench environment (`tb_top`), co-simulated with Verilator:

- 2 nodes. Each node chains AXI master to NMU to router, router to the peer router
  over links, then NSU to AXI subordinate (memory). Figure 2 shows the layout.
- The C++ NoC model runs inside RTL shells (`*_wrap.sv`). Each monitor reports its
  interface to the collector over DPI-C.
- Run one scenario with `make run-tb-top SCENARIO=<id>`, which writes `perf.json`
  and prints the summary.

## 3. Operation: input to output

- **Input**: the NI interfaces (AXI manager interface at the master, AXI
  subordinate interface at the memory) and the router interfaces (inter-router
  links, queue occupancy). The probe reads them, never drives.
- **Output**: per-transaction latency, per-interface throughput and byte counts,
  link stall, and router occupancy.

One run of `AX4-BAS-003` (a single 32-byte write and read at each manager interface
on the 2-node testbench) produces:

```
[perf] AX4-BAS-003_single_write_read_aligned   window [0,35) cyc
  AXI throughput / backpressure
    slot                     bytes_wr bytes_rd txn_wr txn_rd idle_wr idle_rd
    node0.manager                  32       32      1      1       0       0
    node0.subordinate              32       32      1      1       0       0
    node1.manager                  32       32      1      1       0       0
    node1.subordinate              32       32      1      1       0       0
  Latency -- end-to-end (manager; min = best-case observed)
    class                                   n   min   mean   max
    read node0->node1  len0 size5           1    28     28    28
    read node1->node0  len0 size5           1    28     28    28
    write node0->node1  len0 size5          1    27     27    27
    write node1->node0  len0 size5          1    27     27    27
    histogram (cyc): [16,32)=4
    slave service @node0.subordinate: write 3  read 2
    slave service @node1.subordinate: write 3  read 2
  NoC
    router           in_occ_max out_occ_max     link               flit stall
    req.router_0              1           1     req_0to1              3     0
    req.router_1              1           1     req_1to0              3     0
    rsp.router_0              1           1     rsp_0to1              2     0
    rsp.router_1              1           1     rsp_1to0              2     0
  4 transactions -> output/AX4-BAS-003.../perf.json
```

Sections 4 and 5 decode this output.

## 4. Event counting: AXI interface metrics

Event counting aggregates events into per-interface metrics. It splits into three
functions:

**Figure 3: Event counting functions.**

```text
event counting
   |
   +-- accumulator         running counts
   +-- range incrementer   histogram bins
   +-- latency stats       min / mean / max
```

| Function | Produces |
|---|---|
| Accumulator | Write / Read Byte Count, Write / Read Transaction Count, Slave Write Idle / Master Read Idle Cycle Count |
| Range incrementer | Latency histogram, one increment per transaction into a fixed bin (edges 0 16 32 64 128 256) |
| Latency stats | Per-class latency min / mean / max |

Counting is clocked. Each cycle, every monitor:

- detects the interface handshakes,
- updates its counters,
- pushes or pops a per-id queue to match a transaction's start with its completion,
- samples the router queue occupancy.

The aggregation (histogram bins, per-class min / mean / max) runs in the
collector at the end of the run, not per cycle.

**Per-interface counters** (`AXI throughput / backpressure` block):

| Metric | Description |
|---|---|
| Write / Read Byte Count (`bytes_wr` / `bytes_rd`) | Bytes written or read. Basis for throughput. |
| Write / Read Transaction Count (`txn_wr` / `txn_rd`) | Completed write or read transactions. |
| Slave Write Idle Cycle Count (`idle_wr`) | Clocks WVALID is held without WREADY. |
| Master Read Idle Cycle Count (`idle_rd`) | Clocks RVALID is held without RREADY. |

**Latency** (Write and Read measured end-to-end at the manager interface):

| Metric | Definition |
|---|---|
| Write Latency | Write-address acceptance (AWVALID & AWREADY) to the write response (BVALID & BREADY). |
| Read Latency | Read-address acceptance (ARVALID & ARREADY) to the last read beat (RVALID & RREADY & RLAST). |
| Slave service | The memory's service time, measured at the subordinate interface. |

The collector groups completed transactions by **class** (operation, source,
destination, burst length, transfer size) and reports count, min, mean, and max for
each. The operation is part of the class, so every read flow and every write flow
has its own min / mean / max, and the minimum is the best case observed in the run.
In the example each class has one transaction, so the three are equal (read 28,
write 27).

## 5. NoC and per-transaction records

`perf.json` carries two structured sections: the NoC counters (event counting on
the links and routers) and the per-transaction records (event logging).

```json
"noc": {
  "routers": [
    {"name": "req.router_0", "in_fifo_occ_max": 1, "out_fifo_occ_max": 1},
    {"name": "rsp.router_0", "in_fifo_occ_max": 1, "out_fifo_occ_max": 1}
  ],
  "links": [
    {"name": "req_0to1", "flit_count": 3, "stall_cyc": 0},
    {"name": "rsp_0to1", "flit_count": 2, "stall_cyc": 0}
  ]
},
"transactions": [
  {"id": 5, "dir": "write", "src": "node0", "dst": "node1", "accept_cyc": 2, "complete_cyc": 29, "latency": 27, "bytes": 32},
  {"id": 5, "dir": "read",  "src": "node0", "dst": "node1", "accept_cyc": 2, "complete_cyc": 30, "latency": 28, "bytes": 32}
]
```

(node1's mirror entries omitted.)

**Router and link metrics**:

- **in_fifo_occ_max / out_fifo_occ_max**: peak fill of a router's input and output
  queues.
- **flit_count**: flits on a link. Here the request link carries three (the write's
  AW and W, the read's AR) and the response link two (the write's B, the read's R).
- **stall_cyc**: clocks a link had no downstream credit (`credit == 0`), zero under
  this light load.

**Per-transaction records**:

Each record carries the AXI id, direction, source and destination node, the accept
and complete cycle, the latency, and the byte count. Latency is
`complete_cyc - accept_cyc` (write 29 - 2 = 27, read 30 - 2 = 28). These records are
JSON-only, not printed in the summary. Use them to inspect the transactions behind
an outlier in the aggregate metrics.

## 6. Latency case study

The 27-cycle write round-trip, traced through the actual NMU, router, and NSU
pipeline stages. Each row is a hardware block, the x-axis is cycles. The request
runs down to memory, then the response runs back up:

```text
cycle         0              5              10             15             20             25
node0 NMU     SP PK VA                                                             DP SP
node0 Router           RC SA LT                                           RC SA LT
node1 Router                    RC SA LT                         RC SA LT
node1 NSU                                DP MP          MP PK VA
node1 memory                                   ME ME ME
node0 shell                                                                              SH SH

SP AxiSlavePort  PK Packetize  VA VcArbiter  DP Depacketize  MP AxiMasterPort
RC route-compute  SA switch+VC alloc  LT link-traversal  ME memory  SH shell register
```

The read round-trip (28 cycles) follows the same path, with memory service of 2
cycles (not 3) and a 4-cycle shell residual (not 2).

| Component | Write | Read | Source |
|---|---|---|---|
| Network interface | 10 | 10 | staged pipeline, four paths |
| Router | 12 | 12 | three-stage pipeline, four traversals |
| Memory service | 3 | 2 | memory latency plus handshake |
| Shell boundary | 2 | 4 | co-sim wrapper registers |

The figures come from three places:

- **Measured directly**: the round-trip total and the memory service.
- **Pipeline depth**: the network-interface and router contributions.
- **Remainder**: the shell boundary (total minus the other three).
