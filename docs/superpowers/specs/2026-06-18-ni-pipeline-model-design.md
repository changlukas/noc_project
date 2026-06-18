# NMU/NSU pipeline model (NI microarchitecture)

Date: 2026-06-18
Status: Draft (pre-Codex)

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
| **NMU req** | AXI→NoC | `AxiSlavePort` accept (AW/W/AR) + `Rob` allocate/tag | `Packetize` (Address Map = `xy_route` dst + build flit) | `WormholeArbiter`+`VcArbiter` (VC mapping + arbitration) → NoC | 3 |
| **NMU rsp** | NoC→AXI | `Depacketize` (flit → AXI beat) | `Rob` Re-Ordering (read **and** write) | `AxiSlavePort` → master | 3 |
| **NSU req** | NoC→slave | `Depacketize` (+ `MetaBuffer` snapshot) | `AxiMasterPort` → slave | — | 2 |
| **NSU rsp** | slave→NoC | `AxiMasterPort` accept (B/R) | `Packetize` (read `MetaBuffer`) | `VcArbiter` (arbitration) → NoC | 3 |

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

- **Reverse-order tick** (mirror `router.hpp:199`): each NMU/NSU `tick()` processes
  stages S_n → … → S_1 so one beat advances exactly one stage per tick. This
  replaces the current upstream-first combinational pull-through.
- **Per-stage depth is parameterized**, default 1 cycle/stage. Per-path depth knobs
  let the model match the eventual RTL.
- **Inter-stage backpressure:** a downstream-full stage stalls its upstream via the
  existing elastic per-stage queues / credit. A stalled beat holds its stage; the
  stall propagates upstream. No beat is dropped.
- **Reorder hold is backpressure, not depth:** the `Rob` Re-Ordering stage is a
  fixed 1-cycle lookup. When the response is the head-of-line for its (id,
  direction) it forwards immediately (0 hold). When out-of-order it holds in the
  RoB until its predecessor commits — a data-dependent stall, modeled as
  backpressure, not added pipeline depth.

## 6. Ordering modes (ORDERING_MODE)

Both modes are parameterized (the RTL will support both — user decision).

| Mode | Response side | Request side | Latency effect |
|---|---|---|---|
| `ROB` (default) | `Rob` Re-Ordering stage present (S2 of NMU rsp); per-(id,dir) lookup + conditional hold | `Rob` allocate/tag at NMU req S1 | rsp path 3 stages; hold when OoO |
| `ROBLESS` | Re-Ordering stage **removed** (NMU rsp becomes 2 stages: Depacketize → AxiSlavePort) | **injection counter-stall**: per-TxnID outstanding tracker; stall new request admission when a same-id request to a different destination is in flight (would risk OoO responses) | rsp path 2 stages; req admission stalls instead |

ROBLESS is not merely "one fewer stage" — it relocates ordering enforcement from
response-side buffering to request-side admission stalling (FlooNoC RoB-less
pattern). Under deterministic XY routing to a single destination, same-id responses
return in order, so the injection stall rarely fires.

**Reconcile with existing `RobMode`:** the current `RobMode::Disabled/Enabled`
(per read/write) governs whether the RoB does full slot-pool reordering vs simple
outstanding tracking. ORDERING_MODE (`ROB`/`ROBLESS`) is the higher-level choice of
reorder-buffer vs injection-stall. The implementation plan must define how
ORDERING_MODE maps onto / supersedes the existing `RobMode` knobs (open item for
the plan; do not leave both independently live without a defined interaction).

## 7. Scope and consequences

- **Existing tests:** many NMU/NSU/loopback tests assume 0-cycle pull-through
  latency. Staging changes observable cycle counts; those tests must be updated as
  part of implementation. This is expected, not a regression.
- **PMU / perf:** NI latency becomes real microarchitecture. The
  `docs/performance-probe.md` "pipeline fidelity" section (which currently labels NI
  latency a co-sim artifact) must be rewritten once this lands.
- **Co-sim wrap-boundary double-count (open item):** the Verilator shell wrap
  registers each component's outputs once per clock (1 cycle/module boundary). With
  a real internal NI pipeline, the boundary register composes with the internal
  stages. The plan must define attribution so the modeled internal stages are not
  double-counted with the wrap-boundary register (e.g., treat S1 ingress / S3 output
  as realized by the wrap register, or account the boundary separately in the PMU).

## 8. Validation

| Check | Proves |
|---|---|
| Per-stage advance | A beat advances exactly one stage per tick (no skip, no double-advance) under no backpressure. |
| Latency = stage count | Isolated (uncongested) NMU/NSU latency equals the configured stage depth per path (3/3/2/3 default). |
| Backpressure propagation | A stalled output stage holds upstream beats; no beat lost; occupancy bounded. |
| Reorder hold vs depth | In-order responses incur 0 hold (lookup only); out-of-order incur hold = predecessor wait; hold not counted as fixed depth. |
| ROB vs ROBLESS equivalence | Both modes deliver responses in AXI4-required order; ROBLESS stalls injection where ROB would buffer. |
| Parameterized depth | Changing a per-path depth knob changes measured latency by exactly that delta. |

## 9. References

- FlooNoC, PULP (arXiv 2409.17606, 2305.08562); `pulp-platform/FlooNoC`.
- NISAR AXI NI with reorder buffer (IEEE 4415774); Dally & Towles, *Principles and
  Practices of Interconnection Networks*.
- ARM AMBA AXI (IHI0022) register slices.
- In-repo: `router.hpp` (reverse-order-tick pipeline precedent); `nmu.hpp`,
  `nsu.hpp` (current 0-cycle structure); `docs/image/nmu.jpg`, `nsu.jpg` (block
  diagrams); `docs/performance-probe.md` (fidelity section to update).
