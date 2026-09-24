# Parameter propagation

- NMU top forwards AW/W/AR depth, REQ depth, DAT TX per-VC depth and five pack register types through request_path.
- request_fifo forwards each AXI channel depth to the existing axi_async_fifo and unchanged cc_cdc_fifo_gray.
- request_path forwards REQ/DAT output depths to request_buffer. Router credit depth remains a separate parameter.
- NMU top forwards B/R input depths, DAT RX per-VC depth, B/R output CDC depths and two unpack register types through response_path.
- response_path forwards receive depths to response_buffer, register types to response_depacketize, and CDC depths to response_fifo.
- co-simulation TB exposes REG_TYPE and IO_FIFO_DEPTH overrides for focused validation. Default IO depth is 32; DUT DAT RX depth remains 32 to match Router credit configuration.
- Original standalone uses NMU defaults and seeds its response sender from dut.DAT_RX_VC_DEPTH. Existing unit tests retain explicit small-depth profiles.
- No generated package or C++ model parameter default is edited. B/R ROB depths are unchanged.

Active descriptions updated: docs/nmu-spec.md, docs/nmu-verification-plan.md, rtl/README.md, docs/trade-off.md, docs/noc_high_perf_targets.md, docs/verification-environment.md, and this directory's architecture.md. Older acceptance archives describe the prior implementation and are retained as historical records.
