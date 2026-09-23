# Waveform review

On be16, change to /home/mingwei/noc_project/nmu-cosim and run make nWave CASE=<case>. This builds/runs the waveform configuration and opens the resulting FSDB. pattern.txt lists all prepared cases.

| Scenario | Signals to inspect beneath tb_nmu_cosim |
|---|---|
| Backpressure | vip.b_valid, vip.b_ready, vip.b_id, vip.b_resp; vip.r_valid, vip.r_ready, vip.r_id, vip.r_data, vip.r_last |
| Capacity recovery | dut.i_request_path.i_id_remap.wr_exists_full / rd_exists_full; dut.i_response_path.i_depacketize.i_buffer.b_full / r_full / dat_full; vip.aw_ready / ar_ready; b_count / r_count |
| Partial write | vip.aw_addr, vip.w_data, vip.w_strb, vip.ar_addr, vip.r_data. The first 12 writes initialize memory, the next 12 partially overwrite it, then 12 reads verify it. |
| Concurrent read/write | concurrent_active, vip.aw_valid / aw_ready, vip.w_valid / w_ready, vip.ar_valid / ar_ready, vip.r_valid / r_ready. Initialization precedes concurrent_active; final write-region readback follows it. |

Backpressure uses short response-receiver pauses. Capacity cases hold each response channel for 4096 cycles after its first VALID, then receive at the driver's normal rate. No artificial request-channel pacing or new DUT capacity is introduced.

Concurrent cases use initialized region A and disjoint write region B. A covers offsets 0..1023 for control or 0..8191 for data. B starts at offset 0x800 for control or 0x10000 for data. The control SAM base is 0x2000000, the data base is 0. All bursts in these cases are INCR.
