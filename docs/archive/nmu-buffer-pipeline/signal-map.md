# Signal and hierarchy migration

| Before | After |
|---|---|
| request_path.req_candidates / dat_candidates | request_path.req_flit / dat_flit |
| channel_assign.candidate | channel_assign.vc_idx |
| depacketize.i_buffer | response_path.i_rx_buffer |
| channel_assign.gen_credit | tx_buffer.gen_dat_vc[vc].gen_write.i_credit |
| packetize.owner_empty / owner_full | write_context.active_reg (opposite polarity for empty) |
| packetize five payload FIFOs | removed; tx_buffer.i_req_fifo and per-VC i_fifo contain encoded flits |

RC backups retain the pre-change hierarchy. The migrated RC adds local enqueue, link dequeue and credit boundaries.

Standalone performance CSV replaces the removed owner/class FIFO columns with wr_context_empty, wr_context_active, req_fifo_full, dat_full_mask and dat_empty_mask. These describe the new storage boundaries; old per-class fullness counters have no one-to-one equivalent.
