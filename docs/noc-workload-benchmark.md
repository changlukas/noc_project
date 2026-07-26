# NoC Workload Benchmark Definition

Revision 0.1, 2026-07-26, draft.

## 1. Purpose

Synthetic patterns measure whether the fabric works. They do not say whether it fits the workload.
This document defines what to run, what to measure, and what number counts as passing, for tiled
GEMM and multi-head attention on a 2D mesh with one compute tile per node.

The goal every benchmark below serves:

> **Communication must finish inside the compute it overlaps.**

A ratio below one passes, a ratio above one fails, and the margin is the headroom.

## 1.1 Summary

| | B1 Multicast | B2 Reduction | B3 GEMM step | B4 Attention layer |
|---|---|---|---|---|
| Stresses | operand distribution from one source to one axis of a group | partial sums from one axis to one root | B1 and B2 composed under real compute | the whole layer, prefill and decode |
| Regime | bandwidth, kilobyte payloads | latency, hundred-byte payloads | mixed | mixed |
| Primary metric | source port occupancy, tail completion | tail completion latency | overlap ratio `rho`, crossing tile edge | compute tile utilization, external bytes per token |
| Baseline | repeated unicast | unicast gather | same step, all unicast | unicast, and the ideal fabric bound |
| Passes when | port occupancy independent of set size | root occupancy independent of set size, and beats gather across the whole payload range | measured crossing near the predicted `T*` | utilization target held across the swept sequence lengths |
| Answers | is distribution free | is decode-scale collection fast | do the primitives coexist | does any of it reach the application |
| Measurable now | baseline column only | baseline column only | baseline column only | baseline column only |

Predicted columns follow from §3. Measured columns stay empty until the model carries multicast
and reduction. Baseline columns can be filled now.

## 2. What the workload demands of the fabric

### 2.1 The same GEMM, written twice

Tiled GEMM `C = A x B`, output-stationary, on a group `G` of `Gx x Gy` tiles. Node `(gx, gy)` owns
output tile `C[gy][gx]`. The group is an aligned submesh and a collective never crosses its
boundary, so a row means a row of `G`, not a row of the mesh.

**Unicast. Every node fetches its own operands.**

```text
parallel for each node (gx, gy) in G:
    for kt in 0 .. steps-1:
        a = hbm_read(A[gy][kt])          # the Gx nodes of row gy each issue this same read
        b = hbm_read(B[kt][gx])          # the Gy nodes of column gx each issue this same read
        c[gy][gx] += matmul(a, b)

off-chip reads per step:  Gx Gy (|a| + |b|)
```

**Multicast. One node fetches, the group shares.**

```text
for kt in 0 .. steps-1:
    parallel for gy in 0 .. Gy-1:
        at node (0, gy):
            a = hbm_read(A[gy][kt])
            multicast(a, to = row gy of G)

    parallel for gx in 0 .. Gx-1:
        at node (gx, 0):
            b = hbm_read(B[kt][gx])
            multicast(b, to = column gx of G)

    barrier(G)

    parallel for each node (gx, gy) in G:
        c[gy][gx] += matmul(a, b)

off-chip reads per step:  Gy |a| + Gx |b|
```

The two programs compute the same result. The only difference is who reads memory.

| | Unicast | Multicast | Square group, `Gx = Gy = G` |
|---|---|---|---|
| Off-chip reads per step | `Gx Gy (|a| + |b|)` | `Gy |a| + Gx |b|` | **`G` times fewer** |
| Reads of one `A` tile | `Gx`, one per node in the row | 1 | |
| Injections to distribute one `A` tile | 0, nothing is shared | 1 | `Gx - 1` without multicast |

The third column is the arithmetic case for multicast. Everything below measures whether a real
fabric delivers it.

**Where reduction enters.** Neither program above needs one, because each node accumulates its own
output tile locally across steps. A reduction appears when the contraction dimension is split
across nodes instead, which is what decoding does. See §2.2.

**Axis asymmetry.** The two multicasts are not equivalent under XY routing. A row multicast travels
only in X and never turns. A column multicast from a source outside the destination column turns
once. Benchmarks measure the axes separately for this reason.

### 2.2 Reduction follows from partitioning the contraction dimension

Plain output-stationary GEMM needs no reduction. Each node accumulates its own output tile locally
across steps. Reduction appears only when the contraction dimension `K` is split across nodes,
which happens when the output is too small to occupy the mesh.

The case that matters is autoregressive decoding. One query token attends to `S_kv` cached keys, so
`QK^T` is `[1,D] x [D,S_kv]` and `PV` is `[1,S_kv] x [S_kv,D]`. The KV cache is distributed along
`S_kv`, every node holds a slice, and every node produces a partial `[1,D]` output. Those partials
must be summed.

| Phase | Output shape per layer | Dominant collective |
|---|---|---|
| Prefill and training | `S x D` per head, large | multicast, both axes |
| Decode | `1 x D` per head, tiny | reduction along the partitioned axis |

### 2.3 The two collectives sit in different regimes

| | Multicast | Reduction |
|---|---|---|
| Payload | one operand tile, kilobytes | one output row, hundreds of bytes |
| Frequency | once per tile step | once per decoded token |
| Limited by | injection bandwidth at the source port | round-trip latency across the set |
| Right metric | accepted throughput, port occupancy | tail completion latency |

B1 and B2 therefore carry different primary metrics.

## 3. The overlap criterion

Per node, per tile step, with `T x T` output tile, contraction depth `K_t`, element size `b` bytes,
tile compute rate `F` FLOP per cycle, and network interface injection bandwidth `B_inj` bytes per
cycle:

```text
T_comp = 2 T^2 K_t / F              cycles of compute
T_recv = 2 T K_t b / B_inj          cycles to receive both operand sub-tiles
rho    = T_recv / T_comp = b F / (T B_inj)
```

Compute grows with the square of the tile edge and traffic grows linearly, so `rho` falls as tiles
grow. Setting `rho <= 1` gives the smallest tile at which the fabric keeps up:

```text
T* = b F / B_inj
```

`T*` does not depend on `K_t` or on mesh size. At `b` = 2 bytes and `B_inj` = 64 B per cycle, which
is the 512 b wide class at one flit per cycle:

| Tile compute rate `F` | `T*` | Local operand working set at `T*` |
|---:|---:|---|
| 512 FLOP/cycle | 16 | small |
| 1024 | 32 | reference point |
| 2048 | 64 | |
| 4096 | 128 | |

**Source-side constraint under unicast.** The constraint moves from the receiving node to the
sourcing node, which injects the same sub-tile `Gx - 1` times:

```text
T_send = (Gx - 1) T K_t b / B_inj
rho_send <= 1   requires   T >= (Gx - 1) / 2 * T*
```

At `Gx` = 16 the tile edge must be 7.5 times larger, so the local operand memory grows by a factor
of 56 for the same overlap.

**Off-chip access.** Each shared operand is fetched once per group axis rather than once per node,
so external traffic for that operand falls by the axis length. This is arithmetic on the mapping
and holds for any fabric that can multicast.

**Check against published measurement.** FlatAttention (arXiv 2505.18824) runs FP16 on a 32 x 32
mesh at roughly 1000 FLOP per cycle per tile with 1024 b links, which is 128 B per cycle. The
formula gives `T*` = 2 x 1000 / 128, near 16. That work reports a per-tile slice of exactly 16 at a
sequence length of 512 on that mesh, with matrix engine utilization at 20 percent.

`rho` counts operand transfer against compute and omits fixed per-step costs, synchronization and
external memory latency among them, which that work names as the reason for the remaining gap.
`T*` is therefore a lower bound and the measured crossing sits above it. B3 measures the distance.

## 4. Benchmark set

Four benchmarks. The first two isolate a primitive, the third checks that they compose, the fourth
is the system claim.

### B1 Operand multicast

**INPUT.** One source node, destination set is one axis of the group, payload swept from one flit
to one full operand sub-tile. Run both axes and both a source at the end of the set and a source at
its centre.

**MEASURE.** Tail completion time at the farthest member, source port occupancy in flits, peak link
utilization, accepted throughput.

**BASELINE.** Repeated unicast from the same source on the same fabric.

**PASSES WHEN.** Source port occupancy is independent of the set size, and tail completion is
within the zero-load bound plus measured queueing.

### B2 Partial-sum reduction

**INPUT.** All members of one group axis hold a partial result, one root collects. Payload swept
across the decode-sized range, hundreds of bytes, up to one full output tile. Root at the end of
the set and at its centre.

**MEASURE.** Tail completion latency, root port occupancy, local port traffic at each combining
node, and the offload round trip per hop.

**BASELINE.** Unicast gather to the root, summed at the root.

**PASSES WHEN.** Root port occupancy is independent of the set size, and total latency beats
unicast gather across the whole swept payload range, not only at large payloads.

### B3 GEMM tile step

**INPUT.** B1 on both axes and B2 running as one step of a real tiled GEMM, with compute time
modelled at rate `F`. Sweep tile edge `T` across `T*`.

**MEASURE.** `rho`, and the tile edge at which `rho` crosses one.

**BASELINE.** The same step with every collective replaced by repeated unicast.

**PASSES WHEN.** The measured crossing point is within a stated tolerance of the predicted `T*`.
A crossing far above `T*` means queueing or arbitration is eating the margin, and the diagnostic
metrics say where.

### B4 Attention layer

**INPUT.** A full attention layer, both phases. Prefill exercises the multicast path across the
sequence. Decode exercises the reduction path across the partitioned KV cache.

**MEASURE.** Compute tile utilization, external bytes per token, and end-to-end layer time.

**BASELINE.** Repeated unicast on the same fabric, and the ideal-fabric bound of §6.

**PASSES WHEN.** Utilization stays above the stated target across the swept sequence lengths, and
external bytes per token match the `1/Gx` prediction of §3.

## 5. Metrics

**Primary.** These state the goal and appear in any summary.

| Metric | Definition |
|---|---|
| Overlap ratio `rho` | communication time over compute time for one tile step |
| Minimum viable tile `T*` | smallest tile edge at which `rho <= 1` |
| Compute tile utilization | compute cycles over total cycles |
| External bytes per token | off-chip traffic normalized to work done |

**Diagnostic.** These explain a primary metric that misses, and are not targets themselves.

Source and root port occupancy in flits, peak and mean link utilization, tail completion latency
against the zero-load bound, queueing latency by component, and accepted throughput against offered
load.

**Correctness gates.** A benchmark result is void unless these hold.

| Gate | Statement |
|---|---|
| Result equivalence | Both programs produce the same output to a stated tolerance. They sum in different orders, so floating-point results differ in the last bits and the tolerance is part of the benchmark definition |
| Response conformance | One write response per write request, and an error response dominates a combined one |
| Ordering | Same-ID responses reach the master in AXI order in every configuration |
| Completion | Every issued transaction completes. No benchmark passes with outstanding work at the end |

## 6. Baselines

Three, and each answers a different question.

| Baseline | Question it answers |
|---|---|
| Repeated unicast, same fabric | What did the collective buy, with everything else held constant |
| Ideal fabric, `T_ideal = D/v + L/b` | How much of the physically achievable is delivered |
| Zero-load latency | How much of the measured latency is queueing rather than structure |

The published convention for a well-built single-cycle virtual channel router is roughly 80 percent
of ideal throughput, and saturation is conventionally read where latency reaches three times the
low-load value. Both are reference lines for reading the curves, not pass criteria.

## 7. Targets

The table below is the goal statement. Predicted values follow from §3 and from the target
specification. Measured values are empty because they have not been measured, and filling them is
the work.

| Item | Predicted | Measured | Unicast baseline |
|---|---|---|---|
| B1 source port flits, `Gx` = 16, source at end | `L`, independent of `Gx` | | `15 L` |
| B1 tail completion, row axis | `L + H t_wire + (H+1) t_router` | | `15 L + same transport` |
| B1 row versus column axis penalty | one turn on the column axis | | not applicable |
| B2 root port arrivals | one at an end root, two at an interior root | | `Gx - 1` |
| B2 latency crossover payload | where `H C_offload` falls below `(Gx-2) L` | | not applicable |
| B3 crossing tile edge | `T* = b F / B_inj` | | `(Gx-1)/2 * T*` |
| B4 external bytes per token | `1/Gx` of per-node fetch | | per-node fetch |
| B4 compute tile utilization | [TBD], set once `F` is fixed | | |

Two entries need a decision before they can be predicted. `F`, the per-tile compute rate, is not
in the target specification and every utilization number depends on it. The utilization target
itself has no basis yet and is marked `[TBD]` rather than guessed.

## 8. Parameters

| Parameter | Swept over | Fixed by |
|---|---|---|
| Group size `Gx x Gy` | 4, 8, 16 per axis | software submesh assignment |
| Wide class width | 256, 512, 1024 b | target specification §7 |
| Tile edge `T` | across `T*`, both sides | B3 |
| Payload | one flit to one operand sub-tile | B1, B2 |
| Sequence length | short enough to reach `T = T*` at the chosen group | B4 |
| Head dimension | 64, 128 | B4 |
| Element size `b` | 2 bytes | B4 |
| Source or root position | end of set, centre of set | B1, B2 |
| Virtual channels | 1 and the configured maximum | result equivalence gate |

The stress point in B4 is the short-sequence end. Sequence length sets the output tile edge as
`T = S / Gx`, so the fabric stops hiding below `S = Gx T*`. At `Gx` = 16 and `T*` = 32 that is a
sequence of 512. Sweeping through this point locates the design's lower bound.

## 9. Open items

| Item | Needed for |
|---|---|
| Per-tile compute rate `F` | every utilization and `T*` number |
| Reduction operator set | attention also needs a maximum reduction for its softmax statistics, and the target specification allows addition only |
| Which FlatAttention mapping variant to align with | the conference and journal versions place the K and V sources differently, on the south edge and on the diagonal |
| Collective support in the model | B1 to B4 have no measured column until multicast and reduction exist in the C model and the fabric |

The unicast baseline column can be filled now. It needs no new hardware and it is the reference
every other number is read against.
