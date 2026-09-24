# Parameter and hierarchy migration

- NMU top and response_path: B_RX_FIFO_DEPTH/R_RX_FIFO_DEPTH replaced by RSP_RX_FIFO_DEPTH, default 32.
- response_buffer: one RSP_FIFO_DEPTH, default 32. Its former B_FIFO_DEPTH/R_FIFO_DEPTH removed.
- AXI output B_FIFO_DEPTH/R_FIFO_DEPTH unchanged. DAT_RX_VC_DEPTH unchanged.
- Co-sim IO_FIFO_DEPTH connects once to RSP_RX_FIFO_DEPTH.
- Focused TB uses RSP_FIFO_DEPTH=2 to exercise full and head blocking.
- Updated source lists: rtl/Bender.yml, rtl/nmu/top/test_nmu.sh, rtl/nmu/response_depacketize/test_response_depacketize.sh and both workstation files.f.
- Current architecture documentation: docs/nmu-spec.md, docs/nmu-verification-plan.md, docs/trade-off.md, rtl/README.md. Historical reports retained.

## Waveform migration

- i_rx_buffer/i_b_fifo -> i_rx_buffer/i_rsp_fifo.
- Former i_rx_buffer/i_r_fifo entries removed, since both AXI classes share the RSP FIFO.
- i_rx_buffer/r_valid and r_ready -> i_rx_channel_assign/r_valid and r_ready.
- Added raw input and selected B/R output interfaces of i_rx_channel_assign.
- Existing user display settings and other groups retained; both original RC files backed up here.

Co-sim b_full_cnt/r_full_cnt now count shared RSP FIFO full cycles with a B/R head respectively. They no longer describe independent storage banks.
