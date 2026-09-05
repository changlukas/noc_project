# RR versus RRD Control Completion-Time Experiment

Revision 0.2, 2026-09-05.

## Goal

Measure how separating Data-class traffic onto DAT changes Control Completion Time when repeated
Pipeline P2P traffic overlaps the same directed mesh edge.

`RR` is the approved abbreviation for the REQ/RSP mapping. `RRD` is the approved abbreviation for
the REQ/RSP/DAT mapping. Both cases use the same three-network fabric; RR leaves DAT idle for the
background traffic. The comparison does not represent a two-network implementation and provides
no area or power evidence.

## Simulation path

Use the production Verilator co-simulation path:

```text
AXI endpoint -> C++ NMU -> C++ Router mesh -> C++ NSU -> AXI endpoint
```

Do not add a test-only mesh or modify the production fabric structure.

## Channel mapping

| Mapping | Control request | Control response | Data request | Data response |
|---|---|---|---|---|
| RR | REQ | RSP | REQ | RSP |
| RRD | REQ | RSP | DAT | DAT |

REQ, RSP, and DAT carry complete flits. Both mappings use complete flits with a 64-bit payload;
each channel retains its native header and physical width, and unused DAT payload bits are zero.
This normalization applies only to RR/RRD. Native AI throughput tests remain 512-bit, or
64 B/beat.

## Traffic geometry

- Topology: 4x4 mesh with XY routing.
- Control probes: node 0 to node 3, using the existing Control-memory SAM range.
- Control count: 64 single-transaction probes.
- Background: Pipeline P2P row-snake order
  `0,1,2,3,7,6,5,4,8,9,10,11,15,14,13,12`.
- Background initiators: the 14 active Pipeline sources other than the node 0 Control source.
- Background per flow: 16 rounds, two 256-beat transactions per round, 8 B/beat. This is
  4096 B/round, 32 bursts, and 8192 beats per flow per run.
- Source policy: Outstanding Depth 32, one AXI ID and one destination per background initiator.
- Read and Write are separate runs. Read memory and checker state are prefilled before measurement.
- Endpoint random stalls and memory delay are disabled.

Node 0 to node 3 uses eastbound links 0->1, 1->2, and 2->3. The checker derives a directed edge
shared with the active Pipeline background from the actual mapping and XY routes. RR must observe
Control and background traffic on the corresponding REQ/RSP resource. RRD must observe the same
background geometry on DAT. A run is invalid if this resource evidence is absent.

Every background interval must contain the complete interval from the first Control request to the
last Control response. This proves temporal overlap; simultaneous first-cycle launch alone is not
sufficient.

## Measurement

For each Control probe:

```text
Control Completion Time = Control completion handshake cycle
                          - Control request-valid cycle
```

Write completes on the B handshake. Read completes on the final R handshake. Report the arithmetic
mean across all 64 probes:

```text
Completion Time reduction = RR mean Completion Time
                            - RRD mean Completion Time
```

A positive reduction means that moving the background onto DAT reduced Control blocking in this
experiment. Report units as cycles/transaction. Do not report load, bandwidth, or link utilization
for this 64-bit comparison.

## Validity gates

A result is accepted only when all conditions hold:

1. The scoreboard records one non-vacuous PASS and all transactions complete.
2. Exactly 64 node 0 Control probes complete.
3. Every active background flow reports exactly 32 bursts and 8192 data beats.
4. The background interval contains the full Control interval.
5. The reported Control and background resources match the same derived directed edge.
6. The result records RR or RRD, Read or Write, the fixed workload settings, and a reproducibility
   manifest.

## Interpretation limit

The result is mean Control Completion Time under one deterministic Pipeline P2P contention
geometry. It is not native-width Accepted Throughput, a general workload average, or a PPA result.
