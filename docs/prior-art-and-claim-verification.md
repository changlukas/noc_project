# Prior Art and Claim Verification

Revision 0.1, 2026-07-26.

Verification of `noc-target-spec.md` r0.2 against primary sources. Five independent surveys:
FlatAttention dataflow, FlooNoC architecture, on-chip collective precedent and deadlock theory,
recent academic NoC collectives, commercial accelerator fabrics. Arithmetic claims recomputed
directly.

## 1. Claim verdicts

### Workload and dataflow

| Spec claim | Verdict | Source |
|---|---|---|
| Q multicast row-wise, K/V multicast column-wise, O sum-reduced row-wise | CONFIRMED, verbatim match | FlatAttention §III-B, arXiv 2505.18824 |
| Workload is tiled GEMM and multi-head attention on one compute tile per node | CONFIRMED | same, 32x32 tile mesh, 1024-bit links |

FlatAttention additionally performs a **row-wise max-reduction of softmax statistics followed by
re-multicast**, which the spec does not cover. The spec's reduction operator set (addition only)
does not serve it.

### Architecture borrowed from FlooNoC

| Spec claim | Verdict | Note |
|---|---|---|
| Three physical networks `req`, `rsp`, `wide` | CONFIRMED | FlooNoC docs state physical channels were chosen over virtual ones |
| 64 b narrow, 512 b wide | CONFIRMED as reference config | widths are generator parameters |
| One wide flit per AXI beat | CONFIRMED | sizing rule is "fit all packets into a single flit" |
| Wormhole, XY dimension-order | CONFIRMED | wormhole enabled per flit, `last` field gates it |
| `WideAr` on `req`, `WideB` on `rsp`, only wide AW/W/R on `wide` | CONFIRMED against RTL | `FLOO_TYPEDEF_NW_CHAN_ALL` in `typedef.svh` |
| Separate req and rsp for message-level forward progress | CONFIRMED | paper gives deadlock, latency isolation, width matching |
| Same-ID ordering by reorder buffer in the network interface | CONFIRMED | per-chimney B and R RoBs, `rob_idx` in header |
| Credit-based flow control | **Not FlooNoC** | FlooNoC baseline links are valid-ready. Credits existed only in the deprecated VC router. This is an own design decision |
| GALS, endpoint clocks independent of NoC clock | **Not FlooNoC** | the paper claims one synchronous domain. The repo has an optional link-level CDC module only. Own requirement, own cost |

### Arithmetic

Recomputed: mean hop count `2(k^2-1)/(3k)` (2.5 at k=4, 10.625 at k=16), the 15x port-traffic
ratio, the flit-hop ratio table including interior-source values, the full packet width chain
(header 43+13, bodies 109/81/18/83, networks 165/139/353/641/1217), every utilization percentage,
the three tail-latency forms, and the `kS -> S` off-chip result. All correct and internally
consistent.

## 2. Prior art

### The pattern is established

| Element | Status | Instances |
|---|---|---|
| Row/column multicast on a mesh | Established | Eyeriss v1 multicast controllers, Eyeriss v2 hierarchical mesh, Tenstorrent row-master/column-master matmul, Simba |
| In-network reduction | Established | MAERI ART and SIGMA FAN reduction trees, INA (SOCC 2022) router-level accumulation |
| Router without ALU, compute tile combines | Established | Direct Compute Access, arXiv 2603.26438 |
| AXI multicast via user-signal mask + write-response aggregation | Established, two instances | arXiv 2603.26438 (mesh), arXiv 2502.19215 (crossbar) |
| Error-conservative merge of aggregated B | Established | AXI4 §B1.3.1 itself, and arXiv 2502.19215 |

AXI4 already defines response combining for its own purposes. §B1.3.1 requires that where one
burst is converted into several transactions, "the component responsible for the conversion must
combine the responses for all of the generated transactions, to produce a single response for the
original burst. **Any error response is sticky.**" The spec's error-dominates merge therefore
follows AXI's own rule rather than inventing one.

One detail deviates. AXI breaks a SLVERR against DECERR tie by taking **the first response
received**, which on a mesh depends on arrival order and is not reproducible. A deterministic
merge needs its own tie-break rule, stated as a deviation from §B1.3.1.

### The nearest neighbours

**arXiv 2603.26438**, ETH/Bologna, open-sourced in FlooNoC v0.8.0. Extends FlooNoC with multicast,
reduction and barrier synchronization. The NI translates an `AWUSER` address mask into X and Y
masks appended to the AW flit header; `CollectB` merges the multiple B responses; Direct Compute
Access offloads the add to the compute cluster over the router's offload interface. 16.9% router
area, 5.3x and 2.8x geomean speedup. **Destination sets are power-of-two-aligned 2D submeshes,
which is broader than the spec's full-row-or-column restriction.** FlatAttention's simulated fabric
is this design.

**arXiv 2502.19215**, AICAS 2025, same group. 16x16 AXI crossbar, `aw_user` bit-mask multicast,
`stream_join_dynamic` aggregates B with conservative error merge, deadlock avoided by acquiring all
targets atomically. Write path only, no reduction, single-stage crossbar. +12% area, +6% timing,
+29% matmul on 288 cores.

**Intel US9923730B2** (filed 2014). Router fork tables record multicast branch directions; on the
return path routers wait for every branch and aggregate reduction messages. Claims 10.6x hop and
switching-energy reduction, ~3x throughput at 1% broadcast traffic. No shipping product identified.
Relevant to freedom-to-operate, not to novelty of publication.

### Commercial landscape

On-die multicast is shipping and hardware-supported. On-die arithmetic reduction inside routers is
not.

| Vendor | On-die multicast | On-die reduction |
|---|---|---|
| Tenstorrent | Coordinate-rectangle multicast write, `noc_async_write_multicast` kernel API, Blackhole adds strided and excluded shapes | No, Tensix compute + CCL |
| Cerebras WSE | Router duplicates a wavelet to several outputs at no cost, per-color routing config | No, PEs combine. Reduce/AllReduce are scheduled over multicast |
| NVIDIA Hopper/Blackwell | TMA `cp.async.bulk.tensor` multicast into a CTA-mask subset of SMs | No on-die. NVLink SHARP reduces in the off-chip switch |
| Graphcore IPU | Broadcast is a first-class measured collective on the exchange fabric | No, tile-level |
| Groq LPU | None by design, compiler-scheduled | None, compute units |
| SambaNova SN40L | Not public. Fabric is four physical networks, Y-X DOR | Not public |
| IBM NorthPole | Four function-specialized NoCs, one named for partial sums | Not established from public sources |
| Arteris, Baya | Broadcast/multicast advertised in FlexNoC and WeaveIP | Not advertised |

Three cross-cutting observations bear on the spec. Multicast addressing converges on **geometry
rather than member lists** everywhere, which the spec's mask encoding matches. Replication at the
router is universally described as free, and the design pressure is on **late fork**. **No vendor
publishes how multicast fork interacts with virtual channels or credit flow control.**

## 3. Positioning

The combination not found in any single published work: AXI4 **mesh** NoC carrying row/column
multicast, in-network reduction, write-response aggregation, and a stated deadlock argument. The
moat is narrow and must be written against two named works rather than a green field.

| Axis | Against 2603.26438 | Against 2502.19215 |
|---|---|---|
| Topology | same mesh | they are a single-stage crossbar |
| Destination sets | they are broader (2D submesh) | they are broader (strided, aligned) |
| Reduction | both offload to the compute tile | they have none |
| Deterministic FP combine order | unaddressed by them | not applicable |
| Full AXI4 feature coverage | to be established | they disable exclusive access |

**Deterministic reduction is the strongest available differentiator.** In-network reduction
literature accepts non-reproducible results because combine order follows packet arrival, and
reproducibility schemes cost up to 2x. A full-row or full-column combine under XY has an order
fixed by topology. Bitwise determinism follows **if and only if** each hop combines strictly
upstream-result with local operand and never reorders on arrival. This is a spec-level invariant
and is currently unstated.

## 4. Required spec changes

| Item | Change |
|---|---|
| §5 freedom from deadlock | Disambiguate "one virtual channel per network channel". Request and response already occupy separate physical networks; a reviewer reads the current wording as one channel total, which contradicts established AXI-over-NoC results |
| §5 or §8 | State the multicast anti-blocking rule. A row multicast that drops a copy and continues is a two-branch worm at every hop, and branch blocking is a documented wormhole multicast hazard independent of turns. Either asynchronous replication or a guaranteed ejection sink at every destination NI |
| §5 or §8 | State the reduction concurrency restriction. A combine point holds a partial operand while waiting for its sibling, a dependency the turn model does not cover. The comparable design permits one reduction per router at a time and still offers no proof |
| §8 `collective_op` | State the fixed combine order invariant for `REDUCE` |
| §5 credit capacity, §6.1 GALS | Mark as own design decisions, not inherited from the reference fabric |
| §1, §3 | Note the softmax max-reduction the workload also needs, and whether it is out of scope |

## 5. Sources

FlatAttention arXiv [2505.18824](https://arxiv.org/abs/2505.18824), journal version
[2604.02110](https://arxiv.org/abs/2604.02110).
FlooNoC [2409.17606](https://arxiv.org/abs/2409.17606), repo
[pulp-platform/FlooNoC](https://github.com/pulp-platform/FlooNoC).
Collective FlooNoC [2603.26438](https://arxiv.org/abs/2603.26438).
Multicast AXI crossbar [2502.19215](https://arxiv.org/abs/2502.19215).
Boppana and Chalasani, wormhole multicast resource deadlocks, IEEE TPDS 1998.
Hansson et al., message-dependent deadlock in network-based SoC.
Preemptive virtual channels for deadlock-free AXI NoC [2607.01430](https://arxiv.org/abs/2607.01430).
Eyeriss v1 thesis, Eyeriss v2 JETCAS 2019, MAERI ASPLOS 2018, Simba MICRO 2019.
INA [2209.10056](https://arxiv.org/abs/2209.10056), TidalMesh HPCA 2025, SuperMesh MICRO 2025,
Push Multicast HPCA 2025.
Cerebras collectives HPDC 2024 [2404.15888](https://arxiv.org/abs/2404.15888).
Groq ISCA 2022. TPU v4 [2304.01433](https://arxiv.org/abs/2304.01433).
Tenstorrent tt-metal documentation. Intel US9923730B2.
