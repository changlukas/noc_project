# Four approved acceptance additions

Eight cases use control/data variants of backpressure, capacity_recover, partial_write and read_write. The existing 14-case inputs retain their semantics. DAT depth remains 32, AXI FIFO depth 8, per-ID outstanding limit 32 and RSP class FIFO depth 16. No production RTL or C++ changes.

## Reuse

Use existing axi_file_master channel methods and axi_driver.recv_b/recv_r. The testbench adds delay around the existing response receiver only for explicit stall cases. Concurrent traffic invokes the existing master.run(). Initialization and final readback use separate file-master objects sequentially on the same interface. The existing scoreboard retains all checks and memory state across phases.

## Required observations

- Backpressure: BVALID/BREADY and RVALID/RREADY stalls must occur. SVA checks stable valid and payload until acceptance. All responses and bytes must drain correctly.
- Capacity recovery: 64 same-ID eight-beat transfers per direction, with a 4096-cycle hold after the first B/R becomes valid. Both AW and AR must stall, remap per-ID limits must be reached, B receive FIFO must become full, and the R class FIFO (control) or a DAT receive VC FIFO (data) must become full. Final counts, data and timeout check recovery.
- Partial writes: initialize full active byte lanes and complete B, then write complemented data with zero, alternating and single-byte strobes. Read back every active byte. Untouched bytes and overwritten bytes have different expected values.
- Concurrent reads/writes: initialize region A, then issue reads of A concurrently with writes of disjoint region B. Observe simultaneous live transactions, W transfers while reads are outstanding and R transfers while writes are outstanding. Read back B after the concurrent phase.

No reset-during-traffic, forced reorder, FIXED/WRAP or additional seeds are claimed by these cases.

## Scheduling correction during validation

The first backpressure run failed the non-vacuous stall check because a four-cycle delay started before BVALID arrived. The response receiver now waits for VALID before the explicit delay. Required coverage and payload-stability assertions remain enabled. The failed attempt is retained in vcs-additional.log and is not counted as acceptance.

Partial-write data checking passed in the next run, but the upstream file-master emitted a warning when load_files parsed an empty read file during write-only initialization. Acceptance was correctly rejected. The phase loader now calls the existing parse_write or parse_read method for its populated direction, without modifying the upstream parser or suppressing warnings.
