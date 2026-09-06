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
Issue #23 aligns the C++ RoB ordering key to `{dst_id, dst_port_id, AXI class}` and implements
the Disabled R-mode per-ID ordering-domain counter, matching the production `nmu_ordering` RTL.
Establish the NMU top and its request/response paths first, then complete each child in AXI-to-NoC
or NoC-to-AXI dataflow order and pass the NMU hybrid zero-hop loopback. Repeat for NSU. Router work
starts only after both NI loopback gates pass.
Status: In Progress

### Issue #76: NMU request/response path boundaries

#### Stage 1: Boundary contract and compile harness
Goal: Freeze the typed AW/AR/W-to-REQ/DAT and RSP/DAT-to-B/R boundaries from the approved NMU/NSU diagrams and RTL contract.
Success Criteria: A focused harness independently elaborates both path modules with generated AXI/flit records and all ready/credit directions.
Status: Complete

#### Stage 2: Boundary implementation
Goal: Add the two production path modules with no packetization, ordering, buffering, or state.
Success Criteria: Every port names its producer and consumer in the module contract; ports use only the canonical generated record types and existing DAT credit convention.
Status: Complete

#### Stage 3: Focused verification and cleanup
Goal: Run the path elaboration check from a clean tree, remove generated artifacts, and commit the focused change.
Success Criteria: Both modules elaborate independently, the production source list includes them, `make clean` leaves no generated/build artifacts, and the worktree is clean after commit.
Status: Complete

### Issue #53: AXI ID to fixed NoC ID boundary

#### Stage 1: Contract and focused DV
Goal: Separate the external AXI ID parameter from the fixed NoC ID field and capture the approved remap primitive contract.
Success Criteria: Generated C++/SV constants expose AXI_ID_WIDTH=1..8 and NOC_ID_WIDTH=3; flit IDs and NoC link widths remain fixed; focused tests cover widths 1, 3, 8 and illegal widths.
Status: Complete

#### Stage 2: Boundary integration
Goal: Bind the existing AXI ID remap primitive to the simulation endpoint and make C++/DPI use the fixed NoC ID width.
Success Criteria: The endpoint wires the primitive with a 3-bit NoC master ID; model containers and DPI/flit contracts use NOC_ID_WIDTH; source identity remains independent.
Status: Complete

#### Stage 3: Verification and cleanup
Goal: Run focused C++/SV/specgen checks, clean generated/build artifacts, and commit the complete change.
Success Criteria: Allocation/reuse, exhaustion/backpressure, response restoration, legal/illegal parameter checks, drift gate, and clean target pass.
Status: Complete

### Issue #75: NMU top-level boundary

#### Stage 1: Interface contract and focused DV
Goal: Confirm the frozen NMU production faces against the generated packages, wrapper, and specifications, then add a canonical elaboration check.
Success Criteria: The check instantiates all AXI and NoC ports and proves the canonical generated widths.
Status: Complete

#### Stage 2: Boundary implementation
Goal: Add the interface-only `nmu` production boundary with its legal-configuration guards.
Success Criteria: The boundary has no functional datapath, queue, or child instantiation.
Status: Complete

#### Stage 3: Verification and cleanup
Goal: Run the focused compile check from a clean tree and remove generated artifacts.
Success Criteria: Canonical elaboration passes and `make clean` leaves no build or generated files.
Status: Complete

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

### Issue #80: NMU request packetization and NoC channel assignment

#### Stage 1: Contract and packet formats
Goal: Freeze the post-ordering AW/W/AR boundary, generated REQ/DAT field mapping, global AW-to-W ownership rule, and symmetric DAT credit interface.
Success Criteria: The issue and canonical specs require independent REQ/DAT schedulers, Router-depth-seeded DAT credits, same-cycle credit reuse, and `transfer` terminology.
Status: Complete

#### Stage 2: Packetizer and schedulers
Goal: Implement request packetization, AW/W association, independent REQ/DAT arbitration, packet locks, and DAT VC credit accounting.
Success Criteria: Narrow writes and all reads use REQ; Data writes use DAT; W inherits its accepted AW metadata and selected VC; REQ and DAT may transfer in the same cycle.
Status: Complete

#### Stage 3: Focused DV and cleanup
Goal: Verify format mapping, backpressure, credit exhaustion/return, arbitration fairness, packet contiguity, and parallel REQ/DAT progress.
Success Criteria: Focused lint and behavior tests pass with no avoidable bubble under available downstream authority; generated-contract checks and affected regressions pass; all build artifacts are removed.
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

## Auxiliary task: AXI outstanding-driven injection mode

### Stage 1: Behavioral contract and regression
Goal: Add a failing focused test for a write-only injection mode controlled by accepted AXI write outstanding depth.
Success Criteria: The test requires a positive `SOURCE_OUTSTANDING_DEPTH`, independent AW/W issue after local admission, and B-handshake retirement.
Status: Complete

### Stage 2: Minimal implementation
Goal: Add `INJECTION_MODE=4` without changing modes 0-3.
Success Criteria: AW admission stops at the configured depth, W does not wait for AWREADY, each W burst remains contiguous and ordered, and a B handshake releases one slot.
Status: Complete

### Stage 3: Verification and measurement
Goal: Verify the new mode and run a focused 4x4 AI-traffic measurement.
Success Criteria: Focused pytest, generated-file drift, clean 2x2 smoke, and selected 4x4 depth points pass; logs expose configured and observed source outstanding depth.
Status: Complete

## Auxiliary task: DAT VC and Router buffer trade-off report

### Stage 1: Measurement and report contract
Goal: Freeze the short run label, seed-separated output layout, compared parameters, units, formulas, and four result fields: delivered payload bandwidth, completion latency, busiest DAT-link utilization, and DAT buffer entries per Router input.
Success Criteria: Focused tests distinguish Mode 4 rows from offered-load rows and require `m4_<traffic>_v<VC>_b<depth>_o<outstanding>` without seed in the displayed label; Pareto candidates appear only in the conclusion and are not presented as measured data; compute overlap coverage remains a separate workload-derived analysis with an explicit offered load and PE compute budget.
Status: Complete

### Stage 2: Minimal collection and rendering support
Goal: Reuse the current Makefile, CSV, and report generator for isolated VC/buffer results.
Success Criteria: Runs can write beneath a caller-selected output root; the main report adds one concise VC/buffer method and result section without changing the RTL datapath.
Status: Complete

### Stage 3: Screening, refinement, and confirmation
Goal: Measure VC count at Router depth 8, then measure depths 16 and 32 only for selected VC candidates and confirm Pareto finalists across seeds.
Success Criteria: Every cell passes the scoreboard and records the complete hardware/workload tuple; existing VC=2 results are reused only after exact validation.
Status: Complete

### Stage 4: Restore defaults and close evidence
Goal: Restore shipped parameters and publish the performance/cost trade-off in the main report.
Success Criteria: VC 2, Router depth 8, and NI DAT RX depth 8 are restored; codegen drift, focused tests, and clean 2x2/4x4 smokes pass; synthesis-only PPA remains explicitly unclaimed.
Status: Complete

## Auxiliary task: AI NoC performance efficiency evaluation

Detailed plan: `docs/superpowers/plans/2026-09-04-ai-performance-efficiency.md`

### Stage 1: Measurement window
Goal: Make one workload interval authoritative for every performance counter.
Success Criteria: Throughput, flit, utilization, stall, occupancy, and Completion Time values use the same non-zero Mode 4 start/end cycles.
Status: Complete

### Stage 2: Router diagnostics
Goal: Attribute DAT occupancy and credit blocking to the relevant Router port and VC.
Success Criteria: Per-VC HWM and selected-credit blocking distinguish active contention from idle zero-credit state.
Status: Complete

### Stage 3: Throughput bound
Goal: Calculate a pattern-specific Ideal Throughput Bound from resource serialization.
Success Criteria: Analytic resource counts match directed Read, Write, multicast, and repeated-unicast counter fixtures.
Status: Complete

### Stage 4: Experiment sweeps
Goal: Characterize Burst Length separately and compare DUT configurations, multicast, and Pipeline P2P RR/RRD at Outstanding Depth 32.
Success Criteria: Every accepted row passes metadata and checker gates; Burst Length is not ranked as a DUT setting; 512-bit throughput results remain separate from 64-bit RR/RRD latency results.
Status: Complete

### Stage 5: Campaign and report
Goal: Generate clean AI-workload results and measured-set Pareto candidates.
Success Criteria: The report uses standard metrics, lists storage categories separately, identifies limiting workloads, and makes no unsupported PPA claim.
Status: Complete

## Auxiliary task: MHA, MoE, and Pipeline report consolidation

### Stage 1: Report contract
Goal: Limit the report vocabulary to the approved MHA, MoE, and Pipeline traffic models and identify which models have measured evidence.
Success Criteria: Existing results are not relabeled as All-Gather or All-Reduce, standard NoC terms are used, and unrelated generated data is preserved.
Status: Complete

### Stage 2: Generator and focused tests
Goal: Simplify the VC and buffer comparison and remove formulaic prose from the generated report.
Success Criteria: The generator emits the approved structure, visible prose contains no semicolons or em dashes, and focused tests cover the new contract.
Status: Complete

### Stage 3: Report regeneration and verification
Goal: Regenerate the Markdown report from existing evidence and run focused checks.
Success Criteria: Focused pytest passes and the generated report matches the approved terminology without changing measured values.
Status: Complete

## Auxiliary task: Hierarchical All-to-All buffer trade-off plot

### Stage 1: Figure contract
Goal: Define one Pareto scatter plot using measured Hierarchical All-to-All Read and Write results.
Success Criteria: Tests require DAT Router Buffer Capacity in flits/input, Throughput in B/cycle, configuration labels, and Pareto status.
Status: Complete

### Stage 2: Report generation
Goal: Generate the plot from the existing trade-off rows and embed it in the VC and Buffer section.
Success Criteria: Regenerating the report produces the same SVG and preserves all measured values.
Status: Complete

### Stage 3: Verification
Goal: Run the focused report tests and verify the generated Markdown and SVG.
Success Criteria: Focused pytest passes and the report explains how to read cost, performance, Dominated points, and the Pareto front.
Status: Complete
