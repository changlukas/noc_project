# Four-destination NMU co-simulation

## Stage 1: Routing and reuse audit
Goal: Confirm the approved central router and four endpoints, existing delay cell, and shared pattern/checker interfaces.
Success Criteria: Consistent coordinates, return routes and delay semantics.
Status: Complete

## Stage 2: Testbench and directed patterns
Goal: Connect four NSU/memory instances and bypassable response delay, reuse the generator and check actual ingress disorder plus AXI order.
Success Criteria: Ordinary cases bypass delay; ordering cases cover all directions and detect missing disorder.
Status: Complete

## Stage 3: VCS delay search and regression
Goal: Find the smallest response delay producing required disorder and validate existing cases.
Success Criteria: Every smaller delay measured; selected delay passes data/order/coverage checks; C++ library reused when unchanged.
Status: Complete

## Stage 4: Synchronization and handoff
Goal: Update waveform groups, case lists, issue and results.
Success Criteria: Workstation SHA verification and reviewable evidence; issue remains open.
Status: Complete
