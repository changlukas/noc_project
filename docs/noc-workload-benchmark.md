# NoC AI Workload Benchmark Definition

Revision 0.2, 2026-09-05.

## 1. Purpose

This benchmark evaluates NoC traffic produced by AI dataflows. It does not model compute latency,
PE utilization, or an end-to-end neural-network layer. Each pattern defines a deterministic
producer-to-consumer mapping on a 4x4 mesh so that workload behavior and DUT configuration can be
measured separately.

The generated report is `sim/verilator/output/perf_report.md`. It contains only measured results
that pass the validity gates in Section 8.

## 2. Communication types

Node numbers use row-major order on the 4x4 mesh. Read and Write runs preserve the same AI payload
direction: a Read request travels from consumer to producer, then the Read response carries the
payload from producer to consumer.

| Communication type | Producer-to-consumer mapping | AI traffic represented | Directions |
|---|---|---|---|
| Broadcast - Row | Nodes 0, 4, 8, and 12 each distribute to the four members of their row | Row operand or activation distribution | Write |
| Broadcast - Column | Nodes 0, 1, 2, and 3 each distribute to the four members of their column | Column operand or activation distribution | Write |
| Broadcast - Local 2x2 | Nodes 0, 2, 8, and 10 each distribute inside one 2x2 region | Local tensor distribution | Write |
| Broadcast - Global | Node 0 distributes to all 16 mesh members | Global parameter or activation distribution | Write |
| Gather - Global, root 0 | Nodes 1 through 15 send to node 0 | Global result collection | Read, Write |
| Gather - Local 2x2 | Each 2x2 region sends to root 5, 6, 9, or 10 | Local result collection | Read, Write |
| All-to-All | Every node sends once to every other node per round | Token or expert exchange | Read, Write |
| Neighbor Exchange | Every node sends to each valid west, east, north, and south neighbor | Halo or boundary exchange | Read, Write |
| Pipeline P2P | Nodes follow the row-snake path `0,1,2,3,7,6,5,4,8,9,10,11,15,14,13,12` | Feature or tensor transfer between pipeline stages | Read, Write |
| Regional Exchange | Four 2x2 regions perform complete clockwise exchange to the next region | Grouped many-to-many exchange | Read, Write |

Broadcast destination count includes the producer's local member. Local delivery does not inject a
flit into the mesh. Broadcast Read is excluded because the current collective operation is a
one-to-many Write.

## 3. Common workload

### 3.1 Native-width runs

| Setting | Value |
|---|---:|
| Topology and routing | 4x4 mesh, XY |
| AXI data width | 512 bits |
| Bytes per beat | 64 B |
| Logical payload per flow per round | 4096 B |
| Rounds | 16 |
| Source Outstanding Depth | 32 transactions/initiator |
| Max Transactions/ID | 32 transactions/ID |
| Read and Write | Separate runs |
| Read data | Memory and checker prefilled before the measurement window |
| Seed | 1; deterministic mappings and no random endpoint or memory stalls |

The reference point uses Burst Length 64 beats, or one 4096 B transaction per flow per round.
Burst Length is the number of AXI transfers, not the encoded `AxLEN` value.

| Burst Length (beats) | Transactions per flow per round | Payload per flow per round |
|---:|---:|---:|
| 1 | 64 | 4096 B |
| 4 | 16 | 4096 B |
| 16 | 4 | 4096 B |
| 64 | 1 | 4096 B |

### 3.2 Baseline DUT

| DUT parameter | Baseline value |
|---|---:|
| DAT VCs | 2 |
| Router VC depth | 8 flits/VC |
| NI RX DAT depth | 8 flits/VC |
| NI TX DAT depth | 8 entries |
| Read RoB | 128 beat slots |

Burst Length and Outstanding Depth are workload conditions. DAT VCs, Router VC depth, NI RX DAT
depth, NI TX DAT depth, and Read RoB depth are DUT configuration or cost dimensions. The report
does not rank Burst Length as hardware.

## 4. Measurement window and metrics

One common workload window gates Completion Time, Accepted Throughput, flit counts, Link
Utilization, stall counters, and Buffer Occupancy HWM.

```text
Workload Completion Time = last active-source B or RLAST cycle
                           - common workload start cycle

Transaction bytes = Burst Length * 64 B/beat

Logical delivered bytes = payload deliveries * Transaction bytes

Accepted Throughput (B/cycle) = Logical delivered bytes
                                / Workload Completion Time

DAT Link Utilization (%) = transferred DAT flits
                           / measured link cycles * 100
```

The window opens when all active sources are ready to issue and closes when the last active source
completes. Reset, Read prefill, warm-up, and post-run settling are outside the window.

### 4.1 Ideal Throughput Bound

The analytic model enumerates the actual mapping and XY routes. REQ, RSP, and DAT are independent
physical resources and are not added together.

```text
Resource serialization cycles(r) = flits carried by resource r
                                   / resource capacity in flits/cycle

Ideal serialization cycles = max over every resource r
                             of Resource serialization cycles(r)

Ideal Throughput Bound (B/cycle) = Logical delivered bytes
                                   / Ideal serialization cycles

% of Ideal Throughput = Accepted Throughput at Outstanding Depth 32
                        / Ideal Throughput Bound * 100
```

Ideal Throughput Bound is pattern-specific. Absolute Accepted Throughput and `% of Ideal
Throughput` must be read together: one states delivered work per cycle, while the other states how
closely that mapping approaches its own resource bound.

Zero-load Latency and Saturation Throughput require a latency-versus-offered-load curve. They are
not inferred from this fixed-Outstanding campaign.

## 5. Campaign matrix

| Campaign | Result root | Cells | Purpose |
|---|---|---:|---|
| Main baseline | `output/baseline` | 16 | One reference row for every supported communication type and direction |
| Burst Length characterization | `output/burst` | 28 | Sensitivity to 1, 4, 16, and 64 beats on the baseline DUT |
| Multicast comparison | `output/multicast_compare` | 8 | Hardware multicast versus repeated unicast for four Broadcast shapes |
| RR/RRD comparison | `output/rr_vs_rrd` | 4 | Mean Control Completion Time under matched Pipeline P2P Read or Write background |
| DUT configuration trade-off | `output/tradeoff` | 35 | Five stress workloads on seven DUT candidates; baseline is read from the main campaign |

### 5.1 Burst Length characterization

Compare Accepted Throughput and Completion Time across Burst Length values while holding logical
payload, mapping, configuration, and Outstanding Depth fixed. A short-Burst loss identifies
request/response overhead or admission behavior; Burst Length is not a DUT candidate.

### 5.2 Hardware multicast versus repeated unicast

Both modes use the same producers, destination members, issue order, AXI-ID policy, logical
payload, and baseline DUT.

```text
Hardware speedup = repeated-unicast Completion Time
                   / hardware-multicast Completion Time
```

`Source injected flits` counts forward producer-side AW and W flits. It excludes B and CollectB
traffic injected by destinations. The raw flit counts remain visible; no aggregate
flit-reduction score is introduced.

### 5.3 RR versus RRD

`RR` is the 2-channel REQ/RSP mapping. `RRD` is the 3-channel REQ/RSP/DAT mapping. Both use the
same three physical networks; RR leaves DAT idle for the background traffic. This comparison does
not provide area or power evidence for a two-network implementation.

RR/RRD is an independent 64-bit common-payload experiment:

- Node 0 issues 64 single-transaction Control probes to node 3.
- Fourteen active Pipeline P2P initiators run 16 rounds.
- Each background flow sends two 256-beat transactions per round at 8 B/beat, or 4096 B/round.
- Each background initiator uses one AXI ID and one destination with Outstanding Depth 32.
- RR must prove shared directed REQ/RSP use. RRD must prove background DAT use on the same
  directed geometric edge.
- Every background interval must contain the complete Control-probe interval.

Control Completion Time starts at the first request `VALID` assertion and ends at its B/R
handshake, so source admission backpressure is included. The reported metric is node 0's mean
Control Completion Time in cycles/transaction. These 64-bit results do not enter the native
512-bit throughput or DUT Pareto comparison.

## 6. DUT configuration trade-off

The measured set contains the baseline `(VC 2, depth 8)` and seven sweep candidates:

```text
(VC 1, depth 8)   (VC 4, depth 8)   (VC 8, depth 8)
(VC 1, depth 32)  (VC 2, depth 16)  (VC 2, depth 32)  (VC 4, depth 16)
```

For every sweep candidate, NI RX DAT depth equals Router VC depth. Outstanding Depth and Max
Transactions/ID remain 32. NI TX DAT stays at 8 entries and Read RoB stays at 128 beat slots.

The Pareto comparison keeps these costs separate:

1. Router DAT entries per input = DAT VCs * Router VC depth.
2. NI RX DAT entries per NI = DAT VCs * NI RX DAT depth.
3. NI TX DAT entries per NI.
4. Read RoB beat slots per NI.

Each stress workload's Accepted Throughput and Completion Time is also an independent objective.
A candidate is dominated only when another measured candidate is no worse in every cost and every
workload objective, and strictly better in at least one. No scalar score combines unlike storage
or workloads.

Read RoB is extended only when its HWM reaches capacity and a resource-specific admission-stall
counter is non-zero. Router depth is attributed only when the selected input VC reaches capacity
and eligible traffic is blocked by zero credit. More VCs require head-of-line-blocking evidence.

Area, Power, and Timing remain `[TBD]` until synthesis results and limits are approved. The report
therefore lists measured-set Pareto candidates but does not select a final configuration.

## 7. Compute overlap

Compute overlap is a workload-specific check, not a DUT score. It requires an approved PE workload
and Compute Time for the same unit of work.

```text
Communication Time = Workload Completion Time
Communication is fully hidden only when Communication Time <= Compute Time
```

The current benchmark has no approved PE Compute Time, so Compute overlap coverage remains
`[TBD]`. No PE latency or utilization is guessed.

## 8. Validity and reproducibility

A result is accepted only when all checks pass:

1. The scoreboard reaches one non-vacuous PASS and every issued transaction completes.
2. Read data matches the documented prefill image.
3. Compared modes deliver the same logical bytes to the same consumers.
4. The common workload window is non-zero and matches the source-start and final-completion
   markers.
5. Analytic resource counts match emitted flit counters.
6. The result row records topology, direction, workload geometry, DUT configuration, seed, and
   units.
7. Its `manifest.json` records git revision, config-file hash, generated-parameter hash, simulator
   version, seed, exact reproduction command, and the repository-relative path and SHA-256 of the
   campaign `source.patch`. The patch applies to the recorded git revision and contains the
   relevant tracked and untracked build/test source changes under `ref_model/`, `sim/`, and
   `specgen/`; generated run/output artifacts are excluded.

## 9. Out of scope

- Compute execution, PE utilization, and end-to-end model accuracy.
- In-network reduction; Gather measures transport to a root, not arithmetic combination.
- Broadcast Read.
- A full Cartesian product of workload and DUT parameters.
- Frequency, area, power, or timing claims without synthesis evidence.
