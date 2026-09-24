# RX channel-assignment order

TX: AXI input CDC FIFO -> SAM/ordering -> pack -> channel assignment -> REQ/per-VC DAT output FIFO -> NoC.

RX: NoC -> shared RSP/per-VC DAT input FIFO -> RX channel assignment -> unpack -> ordering/ROB -> AXI output CDC FIFO.

RSP is one raw-flit queue. Its head selects B or control R, and only the selected output acceptance pops it. Later RSP flits wait behind a blocked head. DAT storage remains independent by receive VC. B uses the RSP head directly; control R and DAT heads share the existing held-beat round-robin arbiter. A blocked RSP B head cannot block eligible DAT-to-R traffic. R backpressure can stop both read sources.

No payload queue exists between RX assignment and unpack. Optional unpack slices keep their existing B_REG_TYPE/R_REG_TYPE controls and default to bypass. DAT credit returns one cycle after receive FIFO pop, including when an unpack slice accepts the beat before final retirement. ROB and AXI CDC capacities are unchanged.

RSP_RX_FIFO_DEPTH=32 replaces the former two 32-entry B/control-R ingress FIFOs, reducing raw RSP storage by 32 flits. One RSP flit can leave per cycle; simultaneous B and control-R departures from separate ingress queues are intentionally removed. Ready at NoC RSP ingress depends only on shared FIFO space and reset. No measured synthesis area/frequency/power claim is made.
