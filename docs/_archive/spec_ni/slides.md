# NI Presentation Slides

Single source of truth for slide drafting. Technical terms, mode names, and verbatim quotes stay in English. Signal names, register names, parameter constants, and rule IDs do not appear in slide bodies (they live in the spec docs). Each slide follows a Takeaway → Body → Source layout.

---

## Slide 1. AXI4-over-NoC Network Interface

**Design walkthrough, v0.4.0, A5 wave 2026-05-08**

**Takeaway:** An AXI4-NoC boundary IP with built-in QoS, ECC, and RoB. A single wide flit carries one AXI message end-to-end. No chopping overhead.

- **Scope**
  - One NI per tile, single-NI-per-tile topology.
  - Covers NMU, NSU, CSR file, and the interrupt output.
  - The fabric router (NPS) is a separate spec and is not expanded in this deck.
- **Chapter alignment**
  - Section order follows NoC Architecture chapter structure.
- **Design inspiration**
  - **Two-layer ECC, Data Integrity policy, log-and-forward fabric error reporting.**
  - **Arteris FlexNoC:** 4-mode QoS Generator and the heterogeneous traffic class taxonomy.
  - **Wormhole arbiter, RoB and MetaBuffer pattern.**

> **Deck roadmap (13 slides):**
> Overview (S2 to S3) → NMU internals (S4) → Address map (S5) → QoS (S6) → ECC (S7 to S8) → RoB (S9) → NSU internals (S10, includes Exclusive Monitor) → Width conversion (S11) → Credit flow control (S12) → DV status and roadmap (S13)

---

## Slide 2. NoC Communication Overview

**Takeaway:** One wide flit per AXI message. Packetization at the NI boundary only. Routers stay protocol-agnostic.

> **AXI messages must reach a remote slave through a packet-switched fabric.**

**Visual:** End-to-end flow — AXI master tile → NMU → router · router · router → NSU → AXI slave tile. Annotated "one wide flit (header + data)" along the path.

- **NMU / NSU split**
  - NMU packetizes AXI requests into flits at the source tile.
  - NSU de-packetizes flits and drives the local AXI slave at the destination tile.
- **One wide flit per AXI message**
  - Each flit carries one AXI header plus its data payload.
  - The longest AXI burst stays wormhole-locked from injection to last beat.
- **Single shared link pair**
  - Request and response traffic multiplex on one bidirectional link pair per tile.
  - Master and slave IPs are identified by the AXI ID — no extra header field.

**Speaker notes:**

- Design philosophy: *"Wide flits transmit entire AXI4 messages in a single cycle, with no chopping and no serialization tax."*
- NPS feature envelope (adjacent spec): full-duplex switch, multiple virtual channels per port, credit-based flow control, two-cycle minimum through-switch latency, boot-time programmable routing table.
- Trade-off: wider flit lanes for clock-frequency headroom. Chopping-based NoCs pay an endpoint serialization tax per hop. This design avoids it.
- Multiple IPs in a tile (CPU, DMA, accelerator) share one NI through an upstream AXI crossbar.

---

## Slide 3. NoC Components

**Takeaway:** Two boundary units (NMU + NSU) per tile, plus a CSR file and one aggregated interrupt. Each NI isolates the AXI clock domain from the NoC clock domain.

> **A tile may host AXI master traffic, AXI slave traffic, or both — one NI covers all cases.**

**Visual:** NI block diagram — NMU box and NSU box side-by-side. AXI master / slave ports on the left, NoC link pair on the right, CSR file and `irq_o` below.

- **NMU + NSU pair**
  - NMU faces the local AXI master and the NoC request / response links.
  - NSU faces the local AXI slave and the same NoC link pair.
- **Independently enabled halves**
  - Build-time parameters select NMU-only, NSU-only, or both — one RTL covers all three scenarios.
- **CSR file and interrupt**
  - AXI4-Lite configuration port — QoS, Probe, Error, and Quiesce settings.
  - One level-sensitive `irq_o` aggregates every unmasked error event.

**Speaker notes:**

- Per-tile single NI: the NMU + NSU pair sharing one NoC link pair per tile.
- Clock domain split — AXI domain (`aclk_i`) and NoC domain (`noc_clk_i`) are independent. CDC FIFOs sit at the NMU / NSU boundary inside the NI.
- The fabric closes timing independently of any external IP.
- NPS (NoC Packet Switch) is the adjacent NoC router. Not part of the NI. Covered briefly on S2.

---

## Slide 4. NMU Overview

**Takeaway:** AXI4 ↔ NoC flit conversion at the source tile. Handles address translation, QoS shaping, response reordering, and async CDC.

> **AXI4 semantics must hold across a packet-switched NoC boundary.**

**Visual:** NMU block diagram (per `docs/images/NMU.png`, §NoC Master Unit style). Request path: AXI Slave I/F → [Async Boundary] → Address Map → Packetizing → QoS Order Control → VC Mapping → ECC Gen → NoC. Response path: NoC → ECC Check → De-packetizing → Read Re-Ordering → AXI Slave I/F.

- **Protocol bridging**
  - AXI4 ↔ NoC flit conversion across 5 channels.
  - Async CDC between AXI domain (`aclk_i`) and NoC domain (`noc_clk_i`).
- **Per-transaction shaping**
  - Address → destination coordinate plus local offset (→ Address slide).
  - QoS-based priority and VC mapping at ingress (→ QoS slide).
- **Numeric envelope**
  - AXI data widths: 64 / 128 / 256 / 512-bit.
  - Outstanding txns: 32 reads plus 32 writes.
  - Synthesis target: 1.2 GHz NoC, 800 MHz AXI (7 nm representative).

**Speaker notes:**

- Spec verbatim: *"Asynchronous clock domain crossing and rate matching between the AXI master and the NoC."*
- Pipeline depth — AW / AR injection takes 1 to 2 cycles (optional spill register). CDC traversal adds 3 to 4 cycles each direction.
- RoB reorders cross-AXI-ID responses before driving the AXI master (covered in S9).
- Sub-block list (left-to-right on block diagram): Address Map · Packetizing · QoS Order Control · VC Mapping · ECC Gen · ECC Check · De-packetizing · Read Re-Ordering.

---

## Slide 5. Address Decoding and Map

**Takeaway:** Three routing modes — XY (default), Source-routed, ID-table. Differ in area cost and topology support.

> **The NI extracts a destination from the AXI address before injecting any flit.**

**Visual:** Address bit-extraction diagram — AXI address bits split into Y / X / local-offset (XY mode). Plus SAM lookup callouts for Source-routed and ID-table modes.

- **XY-routed (DOR, default)**
  - NI bit-extracts (X, Y) from the AXI address.
  - Routers compute the next hop from XY arithmetic. Deadlock-free.
- **Source-routed**
  - NI SAM lookup. Port sequence packed into flit header.
  - Each router pops the next port.
- **ID-table**
  - NI SAM lookup. Destination ID packed into flit header.
  - Each router maps ID to port via its elaboration-time table.

**Speaker notes:**

- Topology range: 4 × 4 mesh (16 tiles) baseline. 16 × 16 (256 tiles) max. Same parameter envelope.
- Compile-time only: SAM and id-to-port mappings are RTL parameters baked at elaboration. Not runtime-modifiable in v0.4.0. Re-elaborate to change.
- No routing-mode latency penalty: all three modes complete address resolution in the same cycle as packetizing. SAM lookup is combinational, folded into the packetize stage.
- Mode costs — XY: combinational only, no SRAM. Source-routed: NI SAM plus wider flit header (route bits scale with mesh diameter). ID-table: NI SAM plus per-router id-to-port mapping. Cycle-free routes and mappings are the integrator's responsibility.
- Design choice: this NI exposes a uniform 3-mode selector for the integrator to pick per topology.

---

## Slide 6. End-to-End Quality of Service

**Takeaway:** A 4-mode QoS Generator at the NMU ingress decouples heterogeneous IPs sharing one NoC fabric. QoS shapes priority before VC mapping.

> **Real-time and best-effort IPs share one NoC fabric. Without QoS, the real-time IPs starve.**

**Heterogeneous traffic classes (Arteris verbatim):**

| Initiator | Traffic Profile | Reason |
|---|---|---|
| **CPU** | *Latency sensitive* | Processing stops for many cycles on a cache miss. |
| **Video Display** | *Real time & latency critical* | The display buffer must never be empty, or the user sees black pixels. |
| **Imaging System** | *Real time & bandwidth sensitive* | The imaging system operates on several frames in advance and adjusts output quality. |
| **Background File Download** | *Best effort* | File downloads stall without affecting the user. |

- **Static modes**
  - Bypass passes the master's `qos` through unchanged.
  - Fixed overrides every flit with one configured value.
- **Adaptive modes (CSR-programmable thresholds)**
  - Bandwidth Limiter drops `qos` once throughput exceeds the cap.
  - Bandwidth Regulator (urgency-mode) raises `qos` against observed response bandwidth.
- **VC mapping**
  - `qos` tier bit-extracts to VC ID. Each physical channel partitions to 1 / 2 / 4 / 8 VCs.
  - Wormhole arbitration is per-packet at the HEAD flit. W-burst path locks until last beat.

**Speaker notes:**

- Arteris verbatim, Bandwidth Limiter: *"prevent a socket from accepting new requests once a programmable throughput threshold is exceeded."*
- Arteris verbatim, Rate Regulator: *"demote socket transactions when bandwidth usage exceeds configured levels ... smooths traffic without halting initiators entirely."* Our "Bandwidth Regulator (urgency-mode)" is the urgency-feedback variant. The earlier internal label "Urgency Regulator" was renamed to align with Arteris public docs (per PRESENTATION_STYLE.md §11).
- Design choice — QoS lives at the NMU ingress, not at the router. Aligned with end-to-end QoS philosophy: coordinated control of latency, bandwidth, arbitration, and flow management, preventing interference among real-time, best-effort, and background classes.
- All 4 modes are CSR-programmable and runtime-selectable.
- VC mapping rationale: independent VC pools eliminate priority inversion in hardware. Bit-extract is combinational (no SRAM, no programmable map).

---

## Slide 7. Data Integrity

**Takeaway:** Two-layer fabric ECC (per-hop parity + end-to-end SECDED) plus AXI boundary parity. Fabric errors log only — no SLVERR synthesis from the NoC.

> **A flit must arrive at the correct destination with intact payload across multiple router hops.**

**Visual:** Three-layer stack diagram — Layer 1 (per-hop routing parity, every router output), Layer 2 (whole-flit Hsiao SEC-DED, end-to-end), Layer 3 (AXI host-side parity, NI boundary).

- **Layer 1 — Per-hop routing parity**
  - 1-bit parity over `dst_id` and the last-flit indicator.
  - Mismatch drops the flit immediately. Raises a fatal interrupt.
- **Layer 2 — End-to-end whole-flit SECDED**
  - Hsiao SEC-DED over the entire flit (header plus payload).
  - Uncorrectable errors at the sink — log and forward. NSU does not rewrite `BRESP`.
- **Layer 3 — AXI host-side parity (boundary, optional)**
  - 1 bit per byte of data and address.
  - Checked at the AXI boundary. Regenerated when the NI rewrites an address field.

**Speaker notes:**

- Spec §Data Integrity verbatim: *"No ECC checking is performed in the switch fabric."* / *"Uncorrectable ECC errors result in a fatal interrupt."*
- Why two layers (per-hop parity + end-to-end SECDED, not per-hop SECDED everywhere): router logic stays combinational (1-bit XOR over routing fields per hop). Per-hop SECDED would add +1 cycle per hop and roughly 10× the gate count. Payload integrity is preserved by the end-to-end check at the sink.
- Single-bit errors are silently corrected and counted. Double-bit errors are forwarded unchanged and reported via interrupt.
- Fabric error policy (replaces internal "(B)-philosophy" label, per PRESENTATION_STYLE.md §11.1): synthesized `SLVERR` from the NoC fabric is disallowed by design. Uncorrectable ECC at the NSU sink — log and forward. AXI `BRESP` carries the target slave's own response unchanged.
- Hsiao SEC-DED H-matrix MUST be a shared source-of-truth artifact between BFM and RTL (per `protocol_rules.md NOC_FLIT_HDR_FLIT_ECC_GEN`).

---

## Slide 8. End-to-End Protection Flow

**Takeaway:** Generate at boundaries. Check at every fault domain transition. AXI, NoC, and slave error domains stay isolated.

> **Each error class must reach the correct log register without crossing into another fault domain.**

**Data Integrity layer mapping (reference architecture → 3-layer NI):**

| Reference layer | This NI mechanism | Generate / Check points |
|---|---|---|
| Data Parity + Address Parity | AXI host-side parity | AXI master and slave boundaries |
| ECC (NoC packet domain) | Whole-flit SECDED | Generated at NMU / NSU injection. Checked only at the destination NI. |
| DST ID Parity | Per-hop routing parity | Generated at NMU / NSU. Checked at every router output and at the destination NI. |

- **Request path (Master → Slave)** — 🟢 generate · 🔺 check
  - 🔺 NMU checks AXI data and address parity at ingress.
  - 🟢 NMU generates routing parity and whole-flit SECDED.
  - 🔺 Every router output checks routing parity. Mismatches drop on the spot.
  - 🔺 NSU checks SECDED and routing parity on arrival.
  - 🟢 NSU regenerates AXI parity for the local slave.
- **Response path (Slave → Master)**
  - Same flow in reverse.
  - 🟢 NMU regenerates read-data parity for the master after the SECDED check.

**Speaker notes:**

- Spec verbatim: *"Data parity for read responses is generated as 1 bit per byte after the ECC check stage."*
- The 🟢 generate / 🔺 check handshake model makes fault domain transitions explicit. A mismatch at any 🔺 point logs to the error status register for that domain (per S7 layer-to-counter mapping).
- AXI host-side parity (Layer 3) is configurable. When disabled, only Layers 1 (per-hop parity) and 2 (end-to-end SECDED) are active. Disable trades boundary checking for area at the AXI parity ports.

---

## Slide 9. Managing the ordering requirement of AXI

**Takeaway:** RoB enforces same-AXI-ID ordering. Out-of-order responses queue in the Buffer. In-order responses bypass.

> **Transactions with the same ID must be ordered in AXI.**

**Visual:** RoB block diagram (matches `spec/ni/1.jpg`) — Encoder, Reorder Table (control), Reorder Buffer (data), Decoder, meta FIFO. Bypass paths show that in-order responses skip the Buffer.

- **Reorder Table**
  - Keeps track of outstanding transactions of each AXI ID.
  - Keeps state of the order of responses.
- **Reorder Buffer**
  - Dynamically allocated.
  - Temporarily stores AXI responses that are out of order.
- **Optimized for deterministic routing**
  - First / single response is always in order.
  - Order is guaranteed for the same destination.

**Speaker notes:**

- Spec verbatim: *"Read re-tagging via linked-list RROB structure to maintain AXI ordering compliance."*
- Not all response data flows through the Reorder Buffer. In-order arrivals (same-AXI-ID + same destination, or single-issue, or first response) bypass directly to the AXI master via the Decoder. Only out-of-order arrivals occupy a Buffer entry. The `prev_dest` fast path — when consecutive same-AXI-ID requests target the same destination, release latency is ~1 cycle (combinational) instead of ~3 cycles (linked-list walk).
- Three RoB modes — design-time selectable, trade area for cross-ID concurrency:
  - **NormalRoB** — per-AXI-ID linked-list with `prev_dest` adaptive bypass. Largest area. Multi-destination concurrent traffic.
  - **SimpleRoB** — single shared release pointer (single FIFO). Medium area. Cross-ID head-of-line blocking is acceptable.
  - **NoRoB** — no allocation. Relies on NoC same-source-same-dest-same-VC in-order delivery. Forces single-VC configuration.
- B and R RoBs configured independently. Typical: R uses NormalRoB (multi-beat data), B uses SimpleRoB (metadata only).
- Allocator policy — lowest-index-first among FREE entries. AW / AR ties in the same cycle resolved by fair round-robin.

---

## Slide 10. NSU Overview

**Takeaway:** Reverses NoC flits back into AXI semantics. Provides the MetaBuffer, the Read Response Buffer, and the Exclusive Monitor at the egress boundary.

- **Protocol conversion and re-sizing**
  - NoC packetized data ↔ AXI4 reverse decoding.
  - Multiple AXI data widths handled by NSU Downsize (Slide 11).
- **MetaBuffer (project naming; NSU-side request header snapshot)**
  - Snapshots the original request metadata when the request flit arrives.
  - Looked up when the response is generated. The NoC never carries round-trip metadata.
- **Egress Read Response Buffer**
  - Buffers the slave's R responses.
  - Decouples slave-side timing from NoC injection back-pressure.
  - Keeps high-speed NoC injection bubble-free even when the slave runs slower.
- **AXI Exclusive Access Monitor**
  - Local reservation table at NSU — no global bus lock, single-NI scope.
  - Multiple concurrent reservations. CSR clear knob for OS release on process kill.
- **Clock crossing**
  - Asynchronous bridge from the NoC domain to the AXI domain.
  - Smooths the frequency gap between a fast NoC and a slow slave.

> **Block diagram** (per `docs/images/NSU.png`, §NoC Slave Unit style):
> Request path: NoC → ECC Check → De-packetizing → W Reassembly → Downsize → AXI Master I/F.
> Response path: AXI Master I/F → Read Response Buffer → MetaBuffer Lookup → Packetizing B/R → ECC Gen → NoC.

> **Spec verbatim:** *"Read responses are buffered before forwarding to minimize bubbles."* / *"Conversion of NoC packetized data (NPD) to and from AXI protocol data."*

---

## Slide 11. Data Width Conversion

**Takeaway:** NMU Upsize and NSU Downsize decouple the internal NoC payload width from the external AXI width. Saves routing resources while preserving AXI4 byte-level semantics.

- **Configuration examples**
  - 64-bit AXI master into a 256-bit NoC. NMU Upsize packs 4 AXI W beats into one wide flit.
  - 512-bit AXI slave behind a 256-bit NoC. NSU Downsize splits one wide flit into 2 AXI W beats.
- **NMU Upsize (narrow AXI)**
  - Write path: narrow beats map to wide-flit lanes by address offset, then accumulate before injection.
  - Read path: wide flits split back into narrow beats, carrying the original AXI ID and last-beat marker.
- **NSU Downsize (wide AXI)**
  - Write path: a wide flit splits into multiple narrow beats written to the slave in order.
  - Read path: narrow beats accumulate into wide flits before injection.
- **Byte strobe flows end-to-end and preserves consistency**
  - NMU regenerates the wide-flit byte strobe to reflect the bytes the master actually drove.
  - NSU filters writes by byte strobe. Over-fetched bytes are harmlessly ignored by the slave.
- **Synthesis optimization**
  - When widths match, the conversion block degenerates to a zero-delay pass-through.
- **No-chop policy**
  - The NI never chops an AXI burst.
  - The longest burst rides through under a single wormhole-lock.

---

## Slide 12. Credit-Based Flow Control

**Takeaway:** Credits decouple injection from long-wire latency. Per-VC accounting prevents head-of-line blocking.

> **A NoC link can span many cycles of wire latency. A round-trip valid / ready handshake would cap throughput at one flit per round-trip.**

- **Credit mechanics**
  - Senders inject based on credit balance, not a round-trip valid / ready handshake.
  - Each VC has an independent credit pool. Receiver returns up to one credit per cycle per VC.
- **Initialization (post-reset)**
  - Bi-directional ready handshake establishes credit exchange.
  - Initial credit pool is seeded from the receiver's buffer depth.
- **Starvation handling**
  - Permanent zero-credit on a VC = permanent stall on that VC.
  - v0.4.0 does not auto-synthesize SLVERR. Software observes via outstanding-count CSR and `irq_o`.

**Speaker notes:**

- Spec §Credit-Based Flow Control verbatim: *"Each NMU, NPS, and NSU source needs to have credit before it can send data to the receiver. After a reset, every NoC component has its source-credit reset to zero. The source unit connects to the destination unit using a bi-directional ready signal that indicates credit exchange is ready. ... The destination unit can send up to one credit per cycle, per virtual channel."*
- Long-link latency hiding rationale — credit-based flow control is the canonical solution for high-frequency NoC links where a round-trip handshake would cap throughput. Same mechanism in CHI and most modern interconnects.
- Why no auto-SLVERR on starvation — fabric-level stalls indicate a configuration or interconnect-design bug, not a transient error. AXI SLVERR is reserved for end-to-end memory errors (per fabric error policy on S7).

---

## Slide 13. Closing

**Takeaway:** v0.4.0 spec and DV foundation are locked. 136 protocol rules, 50 testpoints, 136 ABV properties. Next focus is ATOPs and Debug / Safety.

- **Dual implementation (BFM + RTL)**
  - C++ BFM and synthesizable RTL.
  - Behaviorally equivalent at the AXI4 and NoC pin boundaries.
  - The BFM exposes test-only knobs (ECC error injection, response delay, ACTIVE / PASSIVE mode).
  - The RTL has fixed pipeline timing.
  - Both share the same CSR memory map.
- **Spec deliverables (post-A5 baseline)**
  - **Protocol rules:** 136 total, 126 FAIL plus 10 RECOMMEND.
  - **DV testpoints (UVM 1.2):** 50, covering AXI, NoC, CDC, RoB, ECC, QoS, Probes, IRQ, Quiesce, and Exclusive.
  - **ABV assertions:** 136 SVA properties, one-to-one with protocol rules.
  - **FPV scope:** RoB allocator state machine, SECDED gen + check round-trip, routing-parity drop, interrupt function, CDC async FIFO, reset entry sequencing.
- **Next steps**
  - Complete ATOPs (AXI4 Atomic Operations). Currently sample-only. Estimated ~3 weeks of design + DV.
  - Factor out a Protocol Reference Library for shared DV modules.
  - Layer on Debug / Safety: outstanding-tx timeout watchdog and cross-NI coherency (directory or snoop).

> **A5 wave baseline locked 2026-05-08.** Outstanding-tx Timeout removed from v0.4.0 (moved post-v1). Error status and IRQ enable compressed to three event classes.

---

## Appendix. Reference Sources

- **Arteris FlexNoC Interconnect IP**
  - End-to-End QoS whitepaper. FlexNoC 5 datasheet.
