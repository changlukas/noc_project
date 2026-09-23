# NMU co-simulation acceptance at DAT depth 32

The initial 14-case VCS matrix passed on be16 with RTL NMU, one C++ Router, one C++ NSU and the existing AXI memory/scoreboard. It completed 284 writes, 284 readbacks, 988 R beats and 8,503 active-byte comparisons. Random cases use seed 1. These are functional results, not sustained-throughput measurements.

## Configuration

- Workstation: `/home/mingwei/noc_project/nmu-cosim`, VCS M-2017.03-SP1, GCC 9.3.
- NMU at Router LOCAL, NSU at WEST. Both address spaces select the one NSU. No DAT merge or C++ NMU.
- AXI and NoC clocks: 10 ns. Existing two-stage synchronized reset release.
- `sim/cosim/nmu/profile.yml`: DAT credit depth 32. Prepared `constants.yml`, SV `ni_params_pkg.sv` and C++ `ni_params.h` derive Router VC and NI RX depths from the same profile. RTL NMU receiver and sender use these generated parameters. NSU sender initialization uses the Router depth through the new pre-tick DPI setter. Global source defaults remain unchanged.
- File-master AW/W channels run concurrently. All B responses complete before dependent readback starts. No intentional stimulus stalls.
- Existing scoreboard compares original AXI write data with returned data. Supplemental checks reject uninitialized bytes, invalid IDs/responses/RLAST, missing or duplicate transactions, and unmet outstanding/ID coverage.

## Results

| Case | Writes / reads | R beats | Checked bytes | Peak W / R outstanding | Active W / R IDs |
|---|---:|---:|---:|---:|---:|
| ctrl_write_single | 1 / 1 | 1 | 8 | 1 / 1 | 1 / 1 |
| ctrl_read_single | 1 / 1 | 1 | 8 | 1 / 1 | 1 / 1 |
| ctrl_write_burst | 12 / 12 | 56 | 210 | 12 / 12 | 1 / 1 |
| ctrl_read_burst | 12 / 12 | 56 | 210 | 12 / 12 | 1 / 1 |
| same_id_in_order | 8 / 8 | 8 | 64 | 8 / 8 | 1 / 1 |
| same_id_outstanding | 16 / 16 | 16 | 128 | 16 / 16 | 1 / 1 |
| multi_id_outstanding | 16 / 16 | 16 | 128 | 16 / 16 | 8 / 8 |
| data_write_single | 1 / 1 | 1 | 64 | 1 / 1 | 1 / 1 |
| data_write_burst | 12 / 12 | 56 | 694 | 12 / 12 | 1 / 1 |
| data_read_single | 1 / 1 | 1 | 64 | 1 / 1 | 1 / 1 |
| data_read_burst | 12 / 12 | 56 | 694 | 12 / 12 | 1 / 1 |
| ctrl_rand | 64 / 64 | 240 | 848 | 22 / 53 | 8 / 8 |
| data_rand | 64 / 64 | 240 | 3658 | 23 / 53 | 8 / 8 |
| request_rand | 64 / 64 | 240 | 1725 | 25 / 47 | 8 / 8 |

`make corrupt` flips one returned data bit and must report `Unexpected RData`. The negative test detected the mismatch. Its expected warning is not a normal positive-case pass. The final runner emits PASS only after checking diagnostics and completion counts.

## Coverage limits

This initial matrix covers initialized, aligned INCR transfers with all active byte lanes strobed. Partial-strobe holes, FIXED/WRAP bursts, forced cross-destination reorder, explicit response stalls, full-capacity recovery and in-flight reset are not claimed. Existing standalone cases remain available for their existing scope. Random cases are one seed, not exhaustive coverage.

C++ NSU REQ ingress remains unbounded in the reference model. This platform therefore does not establish future NSU RTL capacity, backpressure or timing. The C++ NMU ingress limitation is deferred because that model is absent.

## Build and evidence

`remote/acceptance-final.log` records the complete 14-case batch and negative test using `vcs_wave0_fa4f6c58f245`. `metrics.json` records the positive-case measurements. The final output-label and build-cache cleanup is checked separately with request_rand, corruption and a waveform run. Raw reports and original pattern inputs are retrieved with SHA256 verification. `remote/artifacts.json` identifies executable, library and waveform hashes.

Timestamp-only incremental compilation reused stale SV under workstation clock skew during an earlier attempt. That attempt was excluded. Content-hashed build directories now select binaries by sources and flags, including the DPI library path. The C++ model/DPI library is reused for SV, pattern and waveform changes.

Local focused C++ tests: 6 passed. Full Python sim/tools suite: 598 passed. The first run exposed 3 existing whitespace-sensitive source assertions, whose inputs were unchanged from HEAD. Their formatting tolerance was corrected without removing checks or modifying the inspected RTL. See python-baseline.md and python-tests-final.log.

Final checks also passed: request_rand using the final result-marker logic, deliberate corruption with CORRUPTION_DETECTED and no PASS marker, and data_write_burst with FSDB enabled. The latter checked 694 bytes over 56 R beats. Waveform: /home/mingwei/noc_project/nmu-cosim/build/report_wave1/data_write_burst.fsdb. Raw VCS-generated make still reports clock skew; content-hashed directories prevent reuse across source/flag changes. No repeated C++ DPI compilation was needed.

Issue #84 remains open for user review and the explicitly deferred coverage. Production RTL was not changed by this integration.
