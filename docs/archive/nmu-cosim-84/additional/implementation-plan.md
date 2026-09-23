# NMU co-simulation additional acceptance (#84)

## Stage 1: Reuse and coverage definition
Goal: Reuse file-master channel methods and scoreboard for the four approved scenarios.
Success Criteria: Explicit checks for stall stability, resource saturation/recovery, masked-byte preservation and real read/write overlap. No production parameter changes.
Status: Complete

## Stage 2: Shared patterns and TB
Goal: Add control/data cases for backpressure, capacity recovery, partial writes and concurrent reads/writes.
Success Criteria: Generator tests validate initialized data and disjoint concurrent addresses. VCS elaboration succeeds.
Status: Complete

## Stage 3: Workstation acceptance
Goal: Run all new cases and affected baseline cases using cached DPI.
Success Criteria: Nonzero scenario coverage and independent memory checking. Retrieve SHA256-verified evidence.
Status: Complete

## Stage 4: Delivery for review
Goal: Publish commands, patterns and results and update issue #84.
Success Criteria: User can inspect FSDB and reproduce each case. Issue stays open pending user acceptance.
Status: Complete
