# NMU/NSU pipeline model (NI microarchitecture)

Date: 2026-06-18
Status: Draft rev 5 (NSU-req spike found a write-vs-read race; survey (Codex+web) — AXI4 gives no read-write ordering, fix is at the originating master, NSU AxiMasterPort stays transparent; §5.4 added)
token model + arbiter final-stage + ROB Enabled staging contract + ROB-default
resolved; §5.0-5.3, §6.1)

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
| **NMU req** | AXI→NoC | `AxiSlavePort` accept (AW/W/AR) + `Rob` admit/allocate + **Address Map** (`xy_route` dst, in `Rob::push_aw`/`push_ar`, `rob.hpp:180/218`) | `Packetize` consumes precomputed route/meta, builds flit | `WormholeArbiter`+`VcArbiter` (VC mapping + arbitration) → NoC | 3 |
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

### 5.0 Register-parked stages, not a push call-chain (the router model)

Today an NI beat traverses several blocks in **one push call-chain within a
single tick**: `AxiSlavePort::forward_aw → Rob::push_aw → Packetize::push_aw_with_meta
→ arbiter push_flit` (`axi_slave_port.hpp:151`, `rob.hpp:181`, `packetize.hpp:110`);
NSU response is symmetric (`axi_master_port.hpp:159 → packetize.hpp:32`). The
blocks communicate by **calling the next block's push**, and side-effects commit
only on downstream success (`w_meta_fifo_` `packetize.hpp:111`; MetaBuffer
`nsu/packetize.hpp:52/78`). This is why the path collapses to 0 cycles.

The router does the opposite and is the model: stages communicate **only through
shared stage-register/FIFO data members**, never by calling the next stage.
Stage 1 writes `input_fifo_`; Stage 2 reads `input_fifo_`, writes `output_fifo_`;
Stage 3 reads `output_fifo_`, writes the link (`router.hpp:157-163`, `:199-260`).
A `Flit` parked in a register IS the inter-stage hand-off; reverse-order tick
moves it exactly one register per tick.

**The refactor:** convert each NI block from "transform-and-call-next-push" into a
stage that **reads its input stage register, transforms, writes its output stage
register**. No block calls the next block's push. The path `tick()` drives the
reverse-order advance.

### 5.1 Inter-stage tokens

The router's `Flit` is self-describing. NI stage transforms compute metadata
(dst, local_addr, rob_idx) that must **ride with the beat** to the next stage, so
each stage register holds a `{beat + computed meta}` token. Token per boundary:

| Boundary | Token parked in the register |
|---|---|
| NMU req S1→S2 | `AdmittedReq{ Aw\|Ar beat, dst_id, local_addr, rob_req, rob_idx }` (Rob admit computed these); W carries the AW-inherited meta |
| NMU req S2→S3 | built `Flit` (Packetize output; self-describing) |
| NMU rsp S1→S2 | `{ B\|R beat + rsp meta }` from Depacketize |
| NMU rsp S2→S3 | reordered `{ B\|R beat }` (Rob Re-Ordering output) |
| NSU req S1→S2 | decoded `{ Aw\|W\|Ar beat }` (MetaBuffer snapshot done atomically in S1) |
| NSU rsp S1→S2 | accepted `{ B\|R beat }`; S2→S3 = built `Flit` (Packetize, MetaBuffer read) |

**Side-effect commit moves with the stage.** `w_meta_fifo_` mutation and MetaBuffer
`commit_*` move to the stage that performs the actual NoC `push_flit` (the
arbiter-feeding stage), so "commit on downstream success" still holds — success is
now "accepted into the next stage register / granted to NoC", not a deep call
return.

### 5.2 Arbiter as the final stage (no same-tick escape)

The `WormholeArbiter`/`VcArbiter` are single-grant-per-tick but their `push_flit`
enqueues immediately and their `tick()` pushes downstream the same tick
(`wormhole_arbiter.hpp:136/208`, `vc_arbiter.hpp:232`). To stop a same-tick
Packetize→NoC escape: Packetize writes the **S2→S3 stage register**, NOT the
arbiter input directly; the arbiter consumes from that register during the
reverse-order tick and drives NoC. The arbiter pending queues are the final-stage
buffer behind that one registered boundary.

### 5.3 Stage register + bounded advance

- **Stage register.** Each stage owns one **stage register** per AXI channel
  holding ≤1 token (the router's `landing_`/`output_fifo_` analogue,
  `router.hpp:157/163`). A stage advances **at most one beat per channel per tick** —
  the while-loop drains (`axi_slave_port.hpp:151`, `axi_master_port.hpp:138`,
  `depacketize.hpp` `while(true)`) are replaced by this bounded form.
- **Port response-before-request ordering preserved.** `AxiSlavePort::tick()`
  drains responses before forwarding requests (`axi_slave_port.hpp:79`); `AxiMasterPort`
  forwards responses before draining requests (`axi_master_port.hpp:83`). Since req
  and rsp are now separate staged paths, each path keeps its own reverse-order
  advance; the shared port object's two directions advance independently and retain
  their relative ordering within the path tick.
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

### 5.4 Write-vs-read ordering is the master's responsibility, not the NSU's

Surfaced by the NSU-req staging spike (Codex + web survey, 2026-06-18): once W
beats serialize one/tick, a read (independent AR channel) can reach the slave
before an in-flight write's data is committed. This is **AXI4-correct**, not a bug:

- AXI4 (ARM IHI 0022E §A5.3.4) gives **no ordering between read and write
  transactions** — even to the same address, even with `ARID == AWID`. To order a
  read after a write, the **master must wait for the write's `B` response** before
  issuing the read; `WLAST` is not completion (data sent ≠ committed).
- Real AXI-NoC slave units pass through: FlooNoC's NI reorders only same-TxnID
  same-direction responses; it does **not** enforce write-before-read at the
  slave-facing port. AXI interconnects (ARM/Xilinx) likewise leave cross-direction
  ordering to the master or to a slave that returns early `B`.

Therefore:
- The NSU `AxiMasterPort` stays a **transparent transport** — it does NOT
  manufacture write-before-read ordering (`axi_master_port.hpp:14/18`).
- The 0-cycle model accidentally over-ordered (AW→W→AR drained in one tick). The
  staged model correctly exposes the race.
- **The fix is at the originating master.** The testbench `AxiMaster` (scenario
  driver, shared by the c_model tests and the co-sim) enforces the AXI4 rule:
  **a read to an address that overlaps an outstanding (not-yet-`B`-responded) write
  is held until that write's `B`.** This makes the write-then-read-same-address
  data-integrity scenarios AXI-correct under any NI latency, and applies uniformly
  to the c_model port-pair test and the co-sim. Burst-scenario coverage is retained
  (never deleted to dodge the race).

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
- **Config default stays `Disabled` (= ROBLESS), as today** (`NmuConfig.read_rob_mode`/
  `write_rob_mode` already default `Disabled`, `nmu.hpp:51-52`). Decision: do NOT
  change the production default (avoids perturbing existing behavior/tests); ROB
  (`Enabled`) is opt-in per channel. (Supersedes the earlier "default ROB" wording —
  the spec aligns to the code default to resolve the contradiction.)

### 6.1 ROB (Enabled) staging contract — one-beat release timing

The current Enabled `pop_b`/`pop_r` chain-flush commits all ready heads for an id
into `committed_*_queue_` and frees slots + fires `notify_drained` in that flush
(`rob.hpp:272-284/327-348/281/345`). Limiting only the *output* to one beat/tick
would free slots and release request-side stalls **early** (Codex R3). Contract:

- The reorder **decision/commit** (which beats are now in-order) may compute a ready
  run, but **slot-free, `notify_drained`, and the per-id outstanding retire fire when
  a beat actually leaves the Re-Ordering stage register toward S3 (one beat/tick)** —
  not when it enters `committed_*_queue_`.
- `committed_*_queue_` becomes the reorder-storage occupancy behind the stage
  register; it drains one beat/tick into the rsp output path.
- Net: a request-side stall releases only when its response has actually advanced
  past the staged Re-Ordering output, keeping req/rsp timing consistent.

This is a real `Rob` internal change (not just bounding the pop caller); it is
scoped to the NMU rsp staging task and must preserve Enabled-mode reorder
correctness (out-of-order responses still buffer until their predecessor leaves).

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

Latency is measured at **internal stage boundaries**, excluding the co-sim wrapper
register (§7). Cycle epoch = accept handshake (§5).

**Introspection API (the hook the checks below consume).** `Nmu` and `Nsu` each
expose a const getter, analogous to `Router::input_fifo_size(port, vc)`:

```cpp
// beats currently held in stage register [stage] of [path] for AXI channel [axi_ch]
std::size_t stage_occupancy(NiPath path, std::size_t stage, uint8_t axi_ch) const;
```

where `NiPath ∈ {NmuReq, NmuRsp, NsuReq, NsuRsp}`, `stage` is the 0-based stage
index within that path, and `axi_ch` follows `ni_flit_constants` (AW=0…R=4). The
getter lives on `Nmu`/`Nsu` (surfaced through the shell adapters for co-sim
introspection, as the router getters already are). For ROB occupancy the existing
`Rob::write_occupancy()`/`read_occupancy()` are reused.

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
