# NMU co-simulation

The initial 14-case VCS functional matrix passes at DAT credit depth 32. See [acceptance results](../../../docs/archive/nmu-cosim-84/report.md) for counts and coverage limits.

RTL NMU connects to LOCAL and C++ NSU connects to WEST of one C++ Router at (0,0). Header endpoint ports are 0 and 1 respectively. No DAT merge is instantiated. Unused Router ports must not transmit.

The topology retains the existing 2x2 coordinate bounds, but only router (0,0) exists in the testbench. Both SAM windows select the single NSU. No mesh is instantiated.

Patterns reuse gen_standalone_patterns.py with --profile cosim. Single/burst control/data names remain shared. The initial 14 cases complete writes before dependent readback. The additional cases below add explicit initialization and concurrent traffic phases. Transactions use disjoint addresses, initialized byte lanes and INCR bursts supported by the existing AXI scoreboard. Random inputs retain a reproducible seed. The additional directed catalog supplies explicit partial-write initialization and read/write concurrency phases. FIXED/WRAP and forced reorder remain outside this co-simulation coverage.

C++ NMU ingress capacity is deferred because that model is absent. C++ NSU REQ ingress remains unbounded in the existing reference model, so this platform does not validate future NSU RTL backpressure or timing.

The approved co-simulation DAT credit depth is 32. profile.yml drives both SV and C++ receiver depths. NSU sender initialization uses the actual Router capacity; the legacy API/default remains unchanged.

Prepare with `make prepare` in this directory, using an existing standalone dependency stage via `RTL_STAGE`. `make sync` transfers manifest-owned sources directly over SSH with SHA256 verification.

On be16:

```sh
cd /home/mingwei/noc_project/nmu-standalone/cosim
make run CASE=ctrl_write_single
make sim CASE=request_rand
make regress
make corrupt
make run_wave CASE=data_write_burst
make nWave CASE=data_write_burst
```

`run` compiles only when needed. `sim` reuses the selected binary. Builds use content hashes instead of source timestamps to tolerate workstation clock skew. SV edits and waveform mode changes retain the cached C++ DPI library. Reports are in `build/report_wave0` and `build/report_wave1`. FSDB files are in the waveform report directory.

`pattern.txt` and `patterns/cases.list` list the prepared cases. Random patterns record their seed in each manifest; the initial set uses seed 1. Read cases initialize through the real write path; write cases include readback verification. All data checking stays enabled.

Boundary audit: NMU/NSU DAT RX depths, Router input VC depth and sender credit seeds all equal 32 in this profile. Global production defaults are unchanged. GCC 9.3 exists on be16 under `/cadtools/centos/spe21.1/tools.lnx86/cdsgcc/gcc/9.3/bin/g++`; VCS linking and runtime compatibility were verified by the 14-case matrix.

Forced reorder and in-flight reset remain outside this generated list. PASS requires correct counts, scenario coverage and clean checker diagnostics. The corruption test has a separate expected-failure marker.

## Additional directed cases

All eight additions pass VCS. See [results](../../../docs/archive/nmu-cosim-84/additional/report.md) and the linked waveform guide.

The shared catalog `sim/test_patterns/cosim/cases.json` adds these pairs:

| Control | Data | Purpose |
|---|---|---|
| ctrl_backpressure | data_backpressure | Delay B/R acceptance after VALID and check stable payload |
| ctrl_capacity_recover | data_capacity_recover | Fill NMU receive buffers and reach per-ID limits, then drain |
| ctrl_partial_write | data_partial_write | Initialize, partially overwrite, and compare preserved/updated bytes |
| ctrl_read_write | data_read_write | Read initialized region A while writing disjoint B, then read back B |

Run each with `make run CASE=<name>` or `make nWave CASE=<name>` on the workstation. `pattern.txt` lists all 22 cases. Only the explicit backpressure/capacity cases delay response acceptance. DUT parameters and the C++ model are unchanged.

Capacity cases require actual full receive buffers, remap-limit stalls and AW/AR stalls before completion. They exercise NMU resources, not the capacity of a future NSU RTL. Concurrent cases require live read/write overlap plus W/R transfers during opposite-direction outstanding traffic. All phases use the existing memory scoreboard.

`make nWave` loads `signals.rc` with NMU request/response groups followed by Router NoC ports and NSU NoC/memory AXI ports. Router arrays use port 0 (LOCAL, NMU) and port 4 (WEST, NSU) in this topology. The selected FSDB path is substituted into `build/report_wave1/<CASE>.rc`. Override the template with `WAVE_RC=/path/to/signals.rc`.
