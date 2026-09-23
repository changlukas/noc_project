# NMU co-simulation: four additional acceptance scenarios

All eight control/data cases passed VCS on be16 at DAT credit depth 32. They completed 272 writes, 248 reads, 1,904 R beats and 65,416 checked bytes, including initialization and final readback phases. The production RTL, model sources and DUT parameters are unchanged.

## Functional results

| Case | Writes | Reads | R beats | Checked bytes |
|---|---:|---:|---:|---:|
| ctrl_backpressure | 16 | 16 | 128 | 1024 |
| ctrl_capacity_recover | 64 | 64 | 512 | 4096 |
| ctrl_partial_write | 24 | 12 | 56 | 210 |
| ctrl_read_write | 32 | 32 | 256 | 2048 |
| data_backpressure | 16 | 16 | 128 | 8192 |
| data_capacity_recover | 64 | 64 | 512 | 32768 |
| data_partial_write | 24 | 12 | 56 | 694 |
| data_read_write | 32 | 32 | 256 | 16384 |

## Scenario coverage

Backpressure cases observed 4 B stall cycles and 512 R stall cycles each. Assertions checked that VALID and response payload remained stable until acceptance. Normal partial-write, concurrent and baseline burst cases observed no artificial B/R stalls.

Capacity cases held each response channel for 4,096 cycles after its first VALID, reached 32 outstanding transactions per ID in both directions, then completed every response and data comparison. The following are observed cycle counts, not buffer depths:

| Case | AW stall | AR stall | B FIFO full | R FIFO full | DAT VC FIFO full |
|---|---:|---:|---:|---:|---:|
| ctrl_capacity_recover | 4192 | 4310 | 3880 | 4080 | 0 |
| data_capacity_recover | 4194 | 4312 | 3880 | 0 | 4064 |

The capacity tests also require both read/write remap limits to assert. Completion after release proves recovery for the exercised workload. These checks target NMU resources. They do not establish the capacity or timing of future NSU RTL, since the existing C++ NSU REQ ingress is unbounded.

Partial-write cases initialize all active bytes and complete B before overwriting with complemented data. Zero, alternating and single-byte strobes produce both preserved and changed bytes. The unchanged existing scoreboard compares the complete active readback. The generator test independently confirms initialized addresses, legal bursts, changed bytes and preserved bytes.

Concurrent cases read initialized region A while writing disjoint region B through the existing file-master run() method, then read back B. Control/data observed 178/150 cycles with both directions outstanding. Each observed 128 W transfers during read outstanding and 128 R transfers during write outstanding. These are functional concurrency observations, not throughput claims.

## Reuse and validation

- Reuse the existing file master, its channel/receive/parser methods, AXI memory and scoreboard. Explicit response pauses are confined to the backpressure/capacity cases. No custom request pacing or new responder was introduced.
- Existing ctrl_write_burst, data_write_burst and request_rand pass on the updated TB. The 28 AXI input files of the initial 14 cases remain byte-identical to the earlier acceptance archive.
- Deliberate R-data corruption still produces Unexpected RData and the separate CORRUPTION_DETECTED marker.
- Full Python sim/tools suite: 599 passed. No C++ rebuild was required by these TB/pattern changes.
- The initial short pause did not cause a real B stall, and the coverage check rejected it. The receiver now waits for VALID before applying the explicit pause.
- Empty-file parsing during initialization produced an upstream warning, which the runner correctly rejected. Phase loading now calls the existing parser for the populated direction. No warning was suppressed and no upstream driver was changed.

The first four cases passed with vcs_wave0_cecb991160d1. The later initialization-loader correction affects only partial/concurrent cases, which were rerun with vcs_wave0_53905bc621e5 together with the three baseline cases and corruption test. Exact command output is in vcs-additional-final.log and vcs-phases.log. The earlier rejected attempts are retained and excluded from acceptance.

## Reproduction

On be16:

```sh
cd /home/mingwei/noc_project/nmu-cosim
cat pattern.txt
make run CASE=ctrl_partial_write
make nWave CASE=data_capacity_recover
make nWave CASE=data_read_write
```

pattern.txt lists all 22 prepared cases. [Waveform guide](waveform-guide.md) lists useful signal paths. Full measurements are in metrics.json.

Issue #84 remains open for user acceptance. In-flight reset, forced reorder, FIXED/WRAP and additional random seeds remain outside this round.

Waveform runs also pass for data_capacity_recover and data_read_write with vcs_wave1_a20acfd813c1, including the final initialization-loader correction. FSDB files are in workstation build/report_wave1/. Exact logs are in vcs-waves.log. Downloaded source/input/report manifests and binary/library/waveform hashes are in remote/.
