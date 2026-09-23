# NMU co-simulation acceptance (#84)

## Stage 1: Boundary and build audit
Goal: Verify separate-port routing, SAM, receiver capacities and reusable VCS/DPI dependencies.
Success Criteria: Concrete topology and compatible existing interfaces without synthetic responses or early credits.
Status: Complete

## Stage 2: Platform integration
Goal: Add sim/cosim/nmu using RTL NMU, C++ Router/NSU and existing AXI memory/checker.
Success Criteria: Control/data single/burst memory readback passes VCS on workstation.
Status: Complete

## Stage 3: Directed and random acceptance
Goal: Reuse generators for outstanding, random and supported backpressure/reset cases.
Success Criteria: Nonzero independent comparisons, corruption detection and documented coverage limits.
Status: In Progress

## Stage 4: Delivery
Goal: Retrieve evidence and document commands, hashes and cached DPI build behavior.
Success Criteria: Reproducible workstation runs; issue remains open for user acceptance.
Status: Complete

## Current acceptance evidence
14 supported VCS cases and deliberate data corruption were verified. Advanced backpressure/reset coverage remains pending and is not claimed. FSDB/report delivery is complete. Full Python suite passes 598 tests after repairing 3 existing formatting assertions. Stage 3 remains open for deferred coverage and user review.
