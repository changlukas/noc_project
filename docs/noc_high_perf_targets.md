# NoC High-Throughput / Low-Latency Target Spec (DRAFT)

Draft. NoC clock assumed **1 GHz**. Narrow and wide refer to AXI `DATA_WIDTH` (32 / 1024).
The wide plane uses **one flit per beat** (option a, matching the current implementation).
Header (56 b) and per-channel payloads are the design's real values from
`specgen/generated/cpp/ni_flit_constants.h`, and target flit widths are derived from them.

---

## 1. Design Goals

**Target use case.** This NoC targets **many-tile, compute-centric accelerator subsystems**
whose traffic is collective-heavy, meaning high-bandwidth unicast plus first-class
**in-network broadcast and reduction**. Representative workloads are AI/ML accelerators
(attention, GEMM, transformer), SIMD/SIMT spatial accelerators, and DSP or stencil accelerator
arrays. It pairs **narrow latency-critical control** with **very wide bulk data movement** on a
scalable 2D-mesh GALS fabric. It is positioned as an **accelerator subsystem interconnect**,
not a cache-coherent CPU fabric and not a general-purpose peripheral SoC bus.

**Not for.** This spec is over-engineered or mispositioned for cache-coherent CPU SoCs (it
defines no coherence protocol, and broadcast/reduction is not cache-coherence broadcast), MCU
or low-end embedded designs, peripheral-heavy control SoCs, pure point-to-point streaming with
no fan-out or fan-in, and hard real-time systems (it defines no admission control or bandwidth
reservation yet). The presence of a broadcast/reduction collective pattern is the line that
decides whether this NoC pays for itself.

**Goals.**

- **Plane isolation.** Control and data traffic on separate physical planes, so bulk data cannot head-of-line block control.
- **Metric per plane.** Narrow plane tuned for control latency, wide plane for data throughput, with no shared-network compromise.
- **In-network collectives.** Broadcast forked at branch routers, reduction combined toward the root, cutting injection and root bandwidth versus unicast.
- **Single-port ingress.** One AXI master port per endpoint, plane classified in the NI by SAM address decode.
- **Scalable mesh.** 2D mesh with GALS decoupling between the NoC and IP clock domains.

**Motivation from the current single-format design.** Every channel, including a 64-bit `B`
response, is carried in the same 408-bit flit, so control channels use only 18 to 31 % of the
payload (`AW/AR` 108/352 = 31 %, `B` 64/352 = 18 %) and the header is 13.7 % overhead. A
dedicated narrow plane is what recovers this.

---

## 2. Top-Level Block Diagram

~~~
            AXI clk domain │            NoC clk domain (1 GHz)            │ AXI clk domain
                           │                                             │
 ┌─────────┐   AXI4    ┌───┴───────┐   ══ narrow ══▶  ┌──────────┐  ══ narrow ══▶  ┌───────────┐   AXI4    ┌─────────┐
 │  AXI    │  addr 64  │    NMU    │  control ≈164b   │  Router  │  control ≈164b  │    NSU    │  addr 64  │  AXI    │
 │ Master  │─ ID 8   ─▶│ Initiator │                  │ 2D Mesh  │                 │  Target   │─ ID 8   ─▶│ Slave   │
 │  (M)    │  data     │    NI     │  ══ wide ════▶   │   4×4    │  ══ wide ════▶  │    NI     │  data     │  (S)    │
 │         │  32/1024  │ ·SAM class│  data ≈1217b     │ XY · WH  │  data ≈1217b    │ ·Depkt    │  32/1024  │         │
 │         │  AW·W·B·  │ ·Packetize│                  │  · VC    │                 │ ·MetaBuf  │           │         │
 │         │  AR·R     │ ·+56b hdr │                  │ narrow + │                 │           │           │         │
 │         │           │ ·per-ID   │                  │ wide     │                 │           │           │         │
 │         │           │  RoB      │                  │ planes   │                 │           │           │         │
 └─────────┘           └───────────┘                  └──────────┘                 └───────────┘           └─────────┘
~~~

---

## 3. Proposed NoC Key Features

- **Topology**: 2D Mesh, 4 × 4 default, scalable to 16 × 16 (256 nodes, `src/dst_id` 8-bit)
- **Switching mechanism**: flit-based wormhole
- **Routing method**: XY (dimension-order) routing
- **Flow control**: credit-based backpressure, initial credits seeded by the per-VC buffer depth
- **Virtual channels**: 1 per link default, up to 8 (`VC_ID` 3-bit)
- **Clocking**: GALS, router clock domain decoupled from IP clock domain, NoC target 1 GHz
- **AXI conformance**: AXI4 (IHI 0022H), supporting single transfer, burst transfer (INCR / WRAP / FIXED), outstanding transactions, out-of-order completion across IDs (same-ID order preserved), and read data interleaving
- **Ordering**: per-ID reorder buffer, 32 entries (`rob_idx` 5-bit), preserves AXI same-ID order, up to 32 outstanding transactions per ID
- **Header**: fixed **56-bit** (qos, axi_ch, src/dst_id, vc_id, route_par, last, rob_req/idx,
  commtype, multicast mask, flit_ecc, rsvd)
- **Plane separation**: dual physical network
  - **Control plane (narrow)**: AXI `DATA_WIDTH` = **32 b**, flit **≈ 164 b** (56 hdr + ~108 payload, address-led)
  - **Data plane (wide)**: AXI `DATA_WIDTH` = **1024 b**, flit **≈ 1217 b** (56 hdr + 1024 data + 128 wstrb + ctrl), one flit per beat
- **Traffic semantics**: unicast / broadcast / reduction, in-network collective (branch-point
  replication for broadcast, root-ward combining for reduction)

**NoC Simplex Bandwidth** (per link @ 1 GHz):

| Plane | AXI data | Flit width | Channel BW | Data BW |
|-------|---------:|-----------:|-------:|--------:|
| Narrow / control | 32 b | ≈ 164 b | ≈ 20.5 GB/s | 4 GB/s |
| Wide / data | 1024 b | ≈ 1217 b | ≈ 152 GB/s | 128 GB/s |

**Flit format utilization**:

| Plane | Data / flit | Header overhead | Payload / flit |
|-------|------------:|----------------:|---------------:|
| Narrow / control | 19.5 % (32/164) | 34.1 % (56/164) | 65.9 % (108/164) |
| Wide / data | 84.1 % (1024/1217) | 4.6 % (56/1217) | 95.4 % (1161/1217) |

The narrow flit is sized by the **address** (`AW/AR` = 108 b), not the 32-bit data, so its data
utilization is inherently low, because control traffic is address-heavy by nature.

---

## 4. Open items

- **Ordering policy** for same-ID traffic split across planes, one of same-ID pinning, distinct
  control-versus-data ID ranges, or endpoint RoB reorder (the existing NMU per-ID RoB).
- **Narrow header slimming**. 56 b is 34 % of the narrow flit, and a control-plane header could
  drop unused fields such as `flit_ecc`, though `multicast` and `commtype` must stay for collectives.
- **Collective expression over AXI**. AXI has no broadcast/reduction opcode, so we must decide
  how a single AXI transaction declares "broadcast to group G" or "reduction, op=sum" (candidate,
  an address aperture in the SAM).
- **Reduction datatypes and operators**, and whether a bit-exact deterministic mode is required.
- **Energy target** (`pJ/B/hop`) to be set. Open wide-link AXI NoC literature reports about 0.15 as a reference.

References. Open-source wide-physical-link AXI NoC (arXiv 2305.08562) for the narrow/wide
convention, and collective-capable NoC (arXiv 2603.26438) for in-network multicast and reduction.
