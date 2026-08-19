# RTL implementation campaign

The design flow is top-down: freeze system and block contracts before child RTL. Commits and
merges follow implementation dependencies from verified leaf units back to each block top. GitHub
issue IDs and readiness are maintained in the campaign issue graph; this file records stage gates.

## Stage 1: Top-Level Contract
Goal: Freeze the NMU, NSU, and Router hierarchy, interfaces, clock/reset ownership, canonical RTL parameters, and independent per-block model/RTL DUT selection.
Success Criteria: The three block contracts are reviewed against their specs and wrappers; NMU, NSU, and Router can each select model or RTL without exposing DPI handles to production RTL; parameter names, defaults, and legal ranges have one canonical source; `codegen.py --check` passes; no placeholder RTL is committed.
Status: Complete

## Stage 2: Shared Foundation And DV Plans
Goal: Integrate and verify the pinned common_cells FIFO/register primitives and approve one verification plan for each major block.
Success Criteria: Project adapters for the synchronous FIFO, AXI async FIFO, and register primitives pass across legal modes without reimplementing the library; Router, NMU, and NSU DV plans identify assertions, reference-model obligations, coverage, provenance, and per-package acceptance evidence.
Progress: Shared primitives and the NMU, NSU, and Router verification plans are complete.
Status: Complete

## Stage 3: Block RTL
Goal: Implement and verify Router, NMU, and NSU through independently reviewable work packages.
Success Criteria: Every DE package has its paired DV evidence; focused tests pass from a clean tree; RTL NMU passes zero-hop co-sim with the reference NSU; RTL NSU passes zero-hop co-sim with the reference NMU; any model/target DAT flow-control mismatch is isolated in a verification-only adapter with no packet transformation; RTL Router passes its reference-driven differential harness within the documented conformance scope; Router, NMU, and NSU elaborate through the wrapper-facing RTL contract; reference reuse is classified and license-compliant.
Progress: Issue #43 freezes the shared generated topology/SAM interface before dependent NMU, NSU,
and generator implementation: one build-only per-config package, typed constant `SAM`, shared
combinational `ni_sam`, authored-first overlap priority, and deterministic 2x2/4x4 semantic DV.
Follow-on issues #21, #44, and #27 implement that contract and close the recorded C++ overlap gap.
Issue #23 aligns the C++ RoB ordering key to `{dst_id, dst_port_id, AXI class}` and implements
the Disabled R-mode per-ID ordering-domain counter; its production `nmu_rob` RTL remains blocked
until issue #11 supplies the NMU child interfaces and typed records.
Complete NMU and NSU leaf RTL and paired DV first, then integrate each NI top and pass its hybrid
zero-hop loopback. Router work starts only after both NI loopback gates pass.
Status: In Progress

## Stage 4: 2x2 Integration
Goal: Close the first end-to-end RTL write/readback path on a 2x2 mesh.
Success Criteria: The standing pre-clean runs first; clean RTL build and `2x2 verify` pass with a non-vacuous scoreboard result; no disabled checker or unresolved production tie-off remains.
Status: Not Started

## Stage 5: 4x4 Milestone
Goal: Run the standing 4x4 milestone regression after 2x2 integration is stable.
Success Criteria: The complete `4x4 verify` gate in `docs/backlog.md` passes from a clean tree and all campaign issues are closed or moved to tracked limitations.
Status: Not Started
