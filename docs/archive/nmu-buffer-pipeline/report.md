# NMU FIFO placement and output pipeline validation

Implementation: TX input CDC ? admission/context ? pack/slices ? channel assignment ? REQ/per-VC DAT output cc_fifo ? link. RX input cc_fifo ? unpack/slices ? ordering/ROB ? output CDC. Pack/unpack contain no transaction FIFO. Optional slices default to bypass. Transport depth defaults are 32; ROB capacities are unchanged.

The upstream FIFO, counter, arbiter and slice primitives were reused without editing their source. DAT credit is consumed only on an actual link transfer; same-cycle returned credit is usable. Local output space and downstream credit are separate resources. Candidate signal names were replaced and both waveform RC files migrated.

## Validation

- VCS default REG_TYPE=0, FSDB-enabled co-simulation: 24/24 cases pass, totaling 1,100 writes, 1,076 reads, 5,740 R beats and 214,709 checked bytes.
- REG_TYPE=1 and 2: control/data write burst, request_rand, same_id_cross_dst_reorder and data_backpressure pass in each mode (10 runs).
- Focused RTL: 10/10 pass, covering packet smoke, backpressure/reset, types 0/1/2, independent AW/W slice settings, one/two/six VCs, split mode, receive capacity, and exact credit conservation. A credit-starved VC did not stop another eligible VC.
- Original standalone: data_write_burst, same_id_cross_dst_reorder and request_rand pass after its response credit seed was tied to the actual DUT receive depth.
- Deliberate read-data corruption is detected. Python sim/tools: 602/602 pass.
- Final syntax/latch lint passes. Existing upstream/testbench warnings remain. The final alignment was lexically compared against all 12 exact tested source blobs; a formatting error found by VCS was corrected before acceptance.
- FSDB checks resolve all 509 co-simulation and 284 standalone RC paths. Both source snapshots and RC files are synchronized to the workstation (674 and 203 files, SHA256 verified).

Focused tests exercise reset with occupied buffers. Reset during active full co-simulation traffic and synthesis PPA are not newly claimed.

C++ DPI sources were not changed or rebuilt. Expected library SHA256: b4eacba6139b468691c890de702c5ec92a712b5f470b7f5f08653c7a42f99196.

See architecture.md for parameter contracts and storage/timing costs, parameter-references.md for propagation, and signal-map.md for waveform migration. No synthesis frequency, area or power claim is made.

## Workstation commands

From `/home/mingwei/noc_project/nmu-standalone`:

```sh
make run_wave_view TESTBENCH=cosim CASE=data_write_burst
make run_wave_view TESTBENCH=cosim CASE=same_id_cross_dst_reorder MODE=control
make run TESTBENCH=cosim CASE=request_rand REG_TYPE=1
make run TESTBENCH=cosim CASE=data_backpressure REG_TYPE=2
```

`REG_TYPE` is a co-simulation test override that sets all seven pack/unpack slices together. Production NMU parameters remain independent. Omit it to use bypass. `IO_FIFO_DEPTH` is a co-simulation override for AXI, REQ, DAT TX and RSP class depths; DAT RX remains tied to the 32-credit platform contract.

Existing modified design documents were updated in place. Their changes from this round are preserved in the zero-context document-updates.patch; earlier unrelated working-tree edits remain separate from the RTL commit.

## Storage accounting

The default TX payload/context storage is 4,352 REQ bits + 40,512 DAT bits + 185 context/state bits = 45,049 bits. The former six packetizer queues contain 68,800 payload bits at the same depth 32, or 17,200 at their former depth 8. This is record-width accounting, excluding FIFO pointers, credit/arbitration state, CDC and optional slices; it is not synthesis area. The approved depth increase raises capacity and storage relative to the former depth-8 defaults.

Machine-readable results are in results.json. Raw reports and inputs were downloaded with 584 SHA256 checks and remain under remote/ in the development checkout; the transfer manifest is retained.
