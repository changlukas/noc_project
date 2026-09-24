# NMU RX input-buffer and channel-assignment order

## Stage 1: Contract
Goal: Adopt one raw RSP FIFO before B/R assignment, retain independent DAT per-VC storage.
Success Criteria: Head-of-line wait, credit release boundary and PPA trade-off recorded.
Status: Complete

## Stage 2: RTL and integration
Goal: Separate receive buffering from channel assignment and update parameters, filelists, counters and RC.
Success Criteria: No B/R pre-FIFO demultiplexing or additional FIFO between assignment and unpack.
Status: Complete

## Stage 3: Validation
Goal: Run one focused RX test plus control/data burst read/write and cross-ID reorder co-simulation. No full regression.
Success Criteria: Focused VCS and affected co-simulation cases pass without C++ rebuild.
Status: Complete

## Stage 4: Handoff
Goal: SHA-verified workstation sources and reports, updated issue and documentation.
Success Criteria: Reviewable evidence and issue remains open.
Status: Complete
