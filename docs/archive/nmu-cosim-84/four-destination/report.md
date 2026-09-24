# Four-destination NMU co-simulation acceptance

Implementation and workstation validation are complete. Issue #84 remains open for user acceptance.

## Topology

One Router at (1,1), RTL NMU on LOCAL, four independent C++ NSU/AXI memories: NORTH (1,2), EAST (2,1), SOUTH (1,0), WEST (0,1). Routing bounds are 4x4; only one Router is instantiated. DAT credit depth remains 32.

## Minimum response delay

Only cross_id_out_of_order and same_id_cross_dst_reorder enable the WEST B/R delay. Other destinations and ordinary cases bypass it; AW/W/AR receive no injected delay. The TB chains unchanged upstream one-cycle AXI delay cells.

The minimum common setting for both case names and MODE=control/data/rand at seed 1 is **2 cycles**. Control cases fail the required-disorder check at 0 and 1; all six combinations pass at 2, with measured added delay exactly 2. Random bursts already produce disorder at zero. This result applies to the measured topology and patterns, not arbitrary future seeds.

Both directed control/data cases observe B out-of-order count 1 and R count 12. The same-ID case also observes 2 buffered B retirements and 12 buffered R retirements. Memory payload, response status, counts and per-ID retirement checks pass.

## Validation

- VCS FSDB-enabled regression: 24/24 cases pass; 588 writes, 564 reads, 2924 R beats, 77493 checked bytes.
- Six ordering mode combinations pass. After strengthening control/data payload discrimination, all four affected auto/data runs were repeated and pass; auto and control inputs are identical. Rand inputs are unchanged.
- Repeated 0/1-cycle negative probes fail only required disorder. Deliberately corrupted read data is detected.
- Python sim/tools suite: 602 passed.
- All 490 RC signal paths resolve in the actual FSDB; 26 case-specific wave/RC files are available on the workstation.
- Source synchronization verified 666 files; 547 downloaded report/input files pass SHA256 verification.
- C++ DPI library was reused unchanged: `b4eacba6139b468691c890de702c5ec92a712b5f470b7f5f08653c7a42f99196`.

See [validation details](validation.md), [full regression](remote/build/four-destination/regression.json), [delay measurements](remote/build/four-destination/chain-sweep.json), [final payload runs](remote/build/four-destination/payload-validation.json), and [waveform checks](remote/build/four-destination/final-checks.json).

## Workstation commands

```sh
cd /home/mingwei/noc_project/nmu-standalone
make run_wave_view TESTBENCH=cosim CASE=cross_id_out_of_order MODE=control
make run_wave_view TESTBENCH=cosim CASE=same_id_cross_dst_reorder MODE=data
```

Use MODE=rand for mixed control/data requests. RSP_DELAY defaults to 2. Existing FSDBs can be opened with make nWave using the same TESTBENCH, CASE and MODE; explicit control can first use run_wave_view because the recorded equivalent control run uses auto mode.

## Remaining limits

No production RTL or C++ model changes. In-flight reset, FIXED/WRAP bursts, additional random seeds and the reference NSU's unbounded REQ ingress queue are not covered by this acceptance. Deterministic standalone remains available separately.
