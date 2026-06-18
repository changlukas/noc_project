# NMU/NSU pipeline model (NI microarchitecture)

Date: 2026-06-18
Status: Draft rev 2 (Codex round 1 applied: mechanism pinned, address-map →
NMU-req S1, NSU-rsp WormholeArbiter added, ORDERING_MODE = existing RobMode,
co-sim boundary attribution decided, validation made executable)

Replace the 0-cycle functional NMU/NSU with a real, staged-across-ticks pipeline
model that reflects the intended NI RTL microarchitecture. The c_model becomes the
golden reference the RTL follows. Goal: capture realistic per-stage NI latency.

## 1. Problem

NMU and NSU are currently functional models with **0 internal pipeline stages**:
`tick()` is upstream-first and propagates a beat through all internal blocks
combinationally within one tick (`nmu.hpp:135`, `nsu.hpp:118`). Measured co-sim NI
latency is therefore a Verilator shell-wrap artifact (1 register/module boundary),
not microarchitecture (see `docs/performance-probe.md` fidelity section). The NI is
DUT and will be built in RTL, so its behavior model must have a real pipeline.

The router is the in-repo precedent: a 3-stage registered pipeline
(`router.hpp:156-261`) modeled via a reverse-order tick so a flit advances one
stage per tick.

## 2. Survey grounding

| Source | Establishes |
|---|---|
| FlooNoC (PULP, arXiv 2409.17606) | NI adds "three cycles of latency for translating an AXI4 request or response to a flit and vice-versa." Ordering at endpoints (RoB or RoB-less counter-stall), not in routers. RoB bypass: first/in-order response forwarded directly. |
| NISAR (IEEE 4415774), Dally & Towles | Canonical NI: request 2-3 stages (ingress reg → decode/header → output reg); response 3-4 (ejection reg → depacketize → RoB lookup → AXI output reg). 3-4 cycles. |
| ARM AMBA (IHI0022) | Register slice = 1 cycle/channel; the per-stage register building block. |

Conclusion: ~3 register stages per AXI↔flit translation; reorder **lookup** is a
fixed stage, reorder **hold** is data-dependent backpressure (not counted as
pipeline depth). The design mirrors this.

## 3. Authoritative block structure

Source of truth: `docs/image/nmu.jpg`, `docs/image/nsu.jpg` (NI block diagrams) +
the actual `nmu.hpp` / `nsu.hpp` block order. Where the diagram and code disagree,
the **code order governs** (user decision 2026-06-18; the diagrams are not fully
accurate to the implementation).

Excluded from this model (user decision 2026-06-18): **ECC Gen/Check** and the
**Async Data Boundary Crossing (CDC)** shown in the diagrams. They are not modeled
as stages here (deferred).

## 4. Stage allocation (locked)

Each stage = 1 registered cycle. Stages map to real c_model blocks. `|` between
columns = a pipeline register boundary.

| Path | Direction | S1 | S2 | S3 | Stages |
|---|---|---|---|---|---|
| **NMU req** | AXI→NoC | `AxiSlavePort` accept (AW/W/AR) + `Rob` admit/allocate + **Address Map** (`xy_route` dst, in `Rob::push_aw/ar`, `rob.hpp:180/191`) | `Packetize` consumes precomputed route/meta, builds flit | `WormholeArbiter`+`VcArbiter` (VC mapping + arbitration) → NoC | 3 |
| **NMU rsp** | NoC→AXI | `Depacketize` (flit → AXI beat) | `Rob` Re-Ordering (read **and** write) | `AxiSlavePort` → master | 3 |
| **NSU req** | NoC→slave | `Depacketize` (+ `MetaBuffer` snapshot) | `AxiMasterPort` → slave | — | 2 |
| **NSU rsp** | slave→NoC | `AxiMasterPort` accept (B/R) | `Packetize` (read `MetaBuffer`) | `WormholeArbiter`+`VcArbiter` (arbitration) → NoC | 3 |

- Round-trip NI latency ≈ **11 cycles** (3 + 2 + 3 + 3), excluding router, slave,
  and any reorder hold.
- **Asymmetry comes only from `Rob`**: present on both NMU paths (allocate on req,
  re-order on rsp), absent on NSU.
- VC Mapping and VC arbitration are **merged into one stage** (user decision); NSU
  has no VC Mapping block (arbitration only) — already reflected.
- NMU rsp order follows code (`Depacketize → Rob → AxiSlavePort`), not the
  diagram's `Read Re-Ordering → De-packetizing`.
- Re-Ordering covers **both B and R** (user: the RTL has a write-response reorder
  buffer too; matches `NmuConfig.read_rob_mode` + `write_rob_mode`).

## 5. Mechanism

The current submodules drain multiple items per tick (while loops, e.g.
`axi_slave_port.hpp:151`, `depacketize.hpp:74`). This is replaced by bounded
one-token-per-stage advance, mirroring the router.

- **Stage register.** Each stage owns one **stage register** per AXI channel
  holding ≤1 in-flight beat/flit (the router's `landing_`/`output_fifo_` analogue,
  `router.hpp:157/163`). A stage advances **at most one beat per channel per tick** —
  no drain-until-empty loop. The while-loop submodules are restructured into this
  bounded form.
- **Reverse-order tick** (mirror `router.hpp:199`): each NMU/NSU `tick()` processes
  stages S_n → … → S_1, so one beat advances exactly one stage per tick and the
  downstream stage frees its register before the upstream fills it (single-tick
  hand-off, no double buffer). Replaces the current upstream-first pull-through.
- **Per-channel throughput preserved.** Each AXI channel (AW/W/AR/B/R) advances
  independently — ≤1 beat/channel/stage/tick — so under no backpressure the path
  sustains 1 beat/cycle/channel. Stages add latency, not a throughput throttle.
- **Depth semantics.** Per-path depth `N` = an **N-deep shift register** (N-cycle
  latency, full throughput), NOT a variable-dequeue FIFO. Default depths are the
  §4 stage counts (3/3/2/3). A depth knob inserts additional shift registers on
  that path. `depth` is latency, not buffer capacity.
- **Cycle epoch.** The **accept handshake** (`AxVALID & AxREADY`) is cycle 0. The
  external `push_*` enqueues into the S1 register at accept; **S1's advance is the
  first internal tick after acceptance.** End event per `docs/performance-probe.md`
  (read = RLAST, write = B-response).
- **Inter-stage valid/ready contract.** Each boundary is a one-entry handshake:
  downstream is ready when its stage register is free; upstream presents valid; the
  transfer occurs within the same reverse-order tick (downstream drains → frees →
  upstream fills). A stalled beat holds its stage register; the stall propagates
  upstream one stage per tick. No beat is dropped (assert on register overwrite,
  as the router does, `router.hpp:185`).
- **Reorder hold ≠ pipeline depth.** The `Rob` Re-Ordering stage is a fixed
  1-cycle order-decision (lookup). In-order (head-of-line for its id) → forward
  immediately, 0 hold. Out-of-order → accepted into **finite reorder storage**
  (`RobMode::Enabled` slot pool, `rob.hpp:251/270`) and returned later when the head
  is ready; upstream backpressure occurs only when that storage / request-side
  allocation is exhausted. Modeled as finite reorder occupancy + stall-when-full,
  NOT as fixed pipeline depth.

## 6. Ordering modes — reuse existing `RobMode`

Ordering mode is **not a new knob**: it IS the existing per-channel
`RobMode` (`NmuConfig.read_rob_mode` / `write_rob_mode`, `rob.hpp`). The RTL
supports both; both already exist in the c_model. Read and write are configured
independently, as today.

| Mode | = existing | Response side (NMU rsp) | Request side (NMU req) |
|---|---|---|---|
| **ROB** | `RobMode::Enabled` (`rob.hpp:124/243/298`) | Re-Ordering stage present (3 stages); slot-pool finite reorder storage + hold-when-OoO | allocate a reorder slot at admission |
| **ROBLESS** | `RobMode::Disabled` (`rob.hpp:26/193/233`) | no reorder storage; Re-Ordering stage collapses to passthrough → **2 stages** (Depacketize → AxiSlavePort) | **injection counter-stall** already implemented: per-id outstanding tracker stalls a same-id request to a different destination |

- ROBLESS is the FlooNoC RoB-less pattern: ordering enforced by request-admission
  stalling, not response buffering. Under deterministic XY routing to one
  destination, same-id responses return in order, so the stall rarely fires.
- **ROBLESS retire hook (required):** even with no reorder buffering, the response
  side MUST still observe `pop_b`/`pop_r` to retire the per-id outstanding counter
  (`rob.hpp:289/353`); otherwise request-side stalls never release. So the NMU-rsp
  passthrough still calls the counter-retire — it does decrement bookkeeping, just
  no buffering.
- Default = ROB (`Enabled`) for the golden-reference latency; ROBLESS selectable
  per channel.

## 7. Scope and consequences

- **Existing tests:** many NMU/NSU/loopback tests assume 0-cycle pull-through
  latency. Staging changes observable cycle counts; those tests must be updated as
  part of implementation. This is expected, not a regression.
- **PMU / perf:** NI latency becomes real microarchitecture. The
  `docs/performance-probe.md` "pipeline fidelity" section (which currently labels NI
  latency a co-sim artifact) must be rewritten once this lands.
- **Co-sim wrap-boundary attribution (decided):** NI latency is defined by the
  **modeled internal stages, measured inside the c_model**, EXCLUDING the Verilator
  shell-wrap output register. The wrapper's 1-cycle/module-boundary register stays a
  co-sim implementation artifact, accounted separately by the PMU (consistent with
  the current `docs/performance-probe.md` fidelity framing). RTL comparison uses the
  internal stage count. Consequently §8 latency checks are measured at **internal
  stage boundaries** (stage occupancy probes / per-component getters), not at the
  co-sim AXI wires (which include the wrapper register). This removes the
  double-count: the internal pipeline is the source of truth; the wrapper boundary
  is never folded into the stage count.

## 8. Validation

Latency is measured at **internal stage boundaries** (per-stage occupancy probe /
per-component getter), excluding the co-sim wrapper register (§7). Cycle epoch =
accept handshake (§5). Each path has a per-stage occupancy getter (the introspection
hook the checks below consume).

| Check | Hook / method | Proves |
|---|---|---|
| Per-stage advance | drive one beat into an isolated NMU/NSU; sample each stage's occupancy probe per tick | beat occupies S1, then S2, then S3 on successive ticks — exactly one stage/tick, no skip/double-advance |
| Latency = stage count | isolated path, no backpressure; cycles from accept (cycle 0) to the beat leaving the last stage | equals configured depth (default 3/3/2/3) |
| Backpressure propagation | hold the last stage's downstream-ready low; feed beats | beats stack one-per-stage upstream, no beat lost, occupancy ≤ depth, no register-overwrite assert |
| Reorder hold vs depth (ROB) | `RobMode::Enabled`: deliver an out-of-order response then its predecessor | OoO response sits in reorder storage (occupancy>0), released when head ready; in-order response forwards with 0 hold; the fixed stage latency is unchanged |
| ROBLESS retire | `RobMode::Disabled`: issue a stalled same-id/different-dst request, then complete the prior response | the per-id counter retires on `pop_b`/`pop_r` and the stalled request is admitted (stall releases) |
| ROB vs ROBLESS order | both modes, multi-outstanding | responses reach the master in AXI4-required per-id order in both; NMU-rsp depth is 3 (ROB) vs 2 (ROBLESS) |
| Parameterized depth | bump one path's depth knob by k | measured isolated latency on that path increases by exactly k |

## 9. References

- FlooNoC, PULP (arXiv 2409.17606, 2305.08562); `pulp-platform/FlooNoC`.
- NISAR AXI NI with reorder buffer (IEEE 4415774); Dally & Towles, *Principles and
  Practices of Interconnection Networks*.
- ARM AMBA AXI (IHI0022) register slices.
- In-repo: `router.hpp` (reverse-order-tick pipeline precedent); `nmu.hpp`,
  `nsu.hpp` (current 0-cycle structure); `docs/image/nmu.jpg`, `nsu.jpg` (block
  diagrams); `docs/performance-probe.md` (fidelity section to update).
