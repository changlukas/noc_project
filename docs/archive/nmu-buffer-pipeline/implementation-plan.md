# NMU buffer placement and output pipeline

## Stage 1: Architecture and storage audit
Goal: Fix buffer boundaries, AW/W context lifetime and credit accounting.
Success Criteria: Explicit capacity and throughput trade-offs, no payload FIFO inside pack/unpack.
Status: Complete

## Stage 2: RTL and integration
Goal: Move TX buffering after assignment, separate RX buffer, expose pipeline settings and remove candidate names.
Success Criteria: Parameter propagation, source lists and waveform paths consistent.
Status: Complete

## Stage 3: VCS validation
Goal: Validate defaults and register modes with directed, ordering, backpressure and credit tests.
Success Criteria: Correct payload/order, no credit loss, independent VC progress; unchanged C++ library.
Status: Complete

## Stage 4: Workstation and handoff
Goal: Synchronize sources, RC and evidence, preserve open acceptance issue.
Success Criteria: Verified source hashes and reviewable reports.
Status: Complete
