# RTL implementation campaign

The design flow is top-down: freeze system and block contracts, establish each production top and
its request/response path boundaries, then implement children in transaction-flow order. Existing
verified leaves are retained and integrated when their path reaches them. GitHub issue IDs and
readiness are maintained in the campaign issue graph; this file records stage gates.

## Stage 1: Top-Level Contract
Goal: Freeze the NMU, NSU, and Router hierarchy, interfaces, clock/reset ownership, canonical RTL parameters, and independent per-block model/RTL DUT selection.
Success Criteria: The three block contracts are reviewed against their specs and wrappers; NMU, NSU, and Router can each select model or RTL without exposing DPI handles to production RTL; parameter names, defaults, and legal ranges have one canonical source; `codegen.py --check` passes; no placeholder RTL is committed.
Status: Complete

## Stage 2: Shared Foundation And DV Plans
Goal: Approve the reusable primitive sources and one verification plan for each major block.
Success Criteria: Production RTL uses reviewed upstream primitives directly or through a narrow handshake adapter where their contracts fit; block tops own project parameter guards and packed-record mapping without a second storage implementation; Router, NMU, and NSU DV plans identify assertions, reference-model obligations, coverage, provenance, and per-package acceptance evidence.
Progress: Shared primitives and the NMU, NSU, and Router verification plans are complete.
Status: Complete

## Stage 3: Block RTL
Goal: Implement and verify Router, NMU, and NSU through independently reviewable work packages.
Success Criteria: Every DE package has its paired DV evidence; focused tests pass from a clean tree; RTL NMU passes zero-hop co-sim with the reference NSU; RTL NSU passes zero-hop co-sim with the reference NMU; any model/target DAT flow-control mismatch is isolated in a verification-only adapter with no packet transformation; RTL Router passes its reference-driven differential harness within the documented conformance scope; Router, NMU, and NSU elaborate through the wrapper-facing RTL contract; reference reuse is classified and license-compliant.
Progress: Issue #43 freezes the shared generated topology/SAM interface before dependent NMU, NSU,
and generator implementation: one build-only per-config package, typed constant `SAM`, shared
combinational `ni_sam`, authored-first overlap priority, and deterministic 2x2/4x4 semantic DV.
Issue #44 freezes the generated per-channel AXI/flit containers, typed NMU/NSU leaf records,
ready/valid child ports, canonical source order, and the zero-time elaboration harness without
adding datapath RTL. Issue #21 consumes the SAM contract; top-down NMU issue #25 and NSU issue #29
consume the remaining generated child contracts. The recorded C++ overlap gap remains separate
implementation work.
Issue #48 implements the frozen generated SAM package and pure-combinational shared wrapper,
updates the approved primitive dependency, and closes the focused generator/elaboration evidence
without adding NMU/NSU datapath state.
Establish the NMU top and its request/response paths first, then complete each child in AXI-to-NoC
or NoC-to-AXI dataflow order and pass the NMU hybrid zero-hop loopback. Repeat for NSU. Router work
starts only after both NI loopback gates pass.
Status: In Progress

### Issue #21: NMU SAM decode and timing cuts

#### Stage 1: Contract and focused DV
Goal: Bind the generated SAM contract to the NMU child boundary and encode independent AW/AR decode and slice behavior in a focused harness.
Success Criteria: The harness checks authored-first route/class decode, unchanged AXI addresses, complete AW collective metadata, and independently stalled AW/AR streams for every legal slice mode.
Status: Complete

#### Stage 2: Leaf implementation
Goal: Implement `nmu_sam` solely from `ni_sam` and the approved stream register.
Success Criteria: Both decode paths use the generated constant `SAM`; modes 0, 1, and 2 preserve complete typed records; unsupported slice modes and incompatible SAM parameters fail explicitly.
Status: Complete

#### Stage 3: Focused verification and cleanup
Goal: Run the leaf lint/behavior suite and generated-contract checks, then remove build and generated artifacts.
Success Criteria: Focused checks pass, `make clean` completes, no generated or build artifact remains, and the issue change is committed from a clean worktree.
Status: Complete

### Issue #22: N0 DV for NMU SAM decode and timing cuts

#### Stage 1: Oracle and vector contract
Goal: Cross-check every generated SAM vector against the approved C++ table oracle and retain authored-first priority evidence.
Success Criteria: Generated memory, config, and peripheral vectors match the C++-checked SAM oracle in authored order; legal overlap and miss behavior are explicitly checked.
Status: Complete

#### Stage 2: Leaf behavior coverage
Goal: Exercise every generated entry boundary and all independent AW/AR timing-cut mode pairs under backpressure.
Success Criteria: Addresses are preserved; route metadata matches the generated vectors; simultaneous AW/AR traffic remains independent; modes 0, 1, and 2 conserve stable payloads without avoidable full-skid bubbles.
Investigation: Three queue-scoreboard attempts observed a testbench sampling-phase mismatch at 55 ns (`AR mode pair 0 changed transaction 2`). Alternatives considered were delaying the input sample, using a cycle-index offset, or separating deterministic vector checks from stream checks. The selected split keeps the existing backpressure checker and adds a mode-0 vector harness, avoiding a simulator-scheduling assumption.
Status: Complete

#### Stage 3: Fault-injection evidence and cleanup
Goal: Demonstrate the focused checkers fail for invalid parameter, miss, footprint, and collective stimulus, then clean all artifacts.
Success Criteria: Focused lint/behavior and oracle checks pass; expected-fail diagnostics are observed; `make clean` leaves no generated or build artifact; the issue change is committed from a clean worktree.
Status: Complete

## Stage 4: 2x2 Integration
Goal: Close the first end-to-end RTL write/readback path on a 2x2 mesh.
Success Criteria: The standing pre-clean runs first; clean RTL build and `2x2 verify` pass with a non-vacuous scoreboard result; no disabled checker or unresolved production tie-off remains.
Status: Not Started

## Stage 5: 4x4 Milestone
Goal: Run the standing 4x4 milestone regression after 2x2 integration is stable.
Success Criteria: The complete `4x4 verify` gate in `docs/backlog.md` passes from a clean tree and all campaign issues are closed or moved to tracked limitations.
Status: Not Started
