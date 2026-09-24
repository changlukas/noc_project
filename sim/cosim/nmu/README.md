# NMU co-simulation

The initial 14-case VCS functional matrix passes at DAT credit depth 32. See [acceptance results](../../../docs/archive/nmu-cosim-84/report.md) for counts and coverage limits.

RTL NMU connects to LOCAL of one C++ Router at (1,1). Four C++ NSU/memory endpoints connect directly to NORTH (1,2), EAST (2,1), SOUTH (1,0), and WEST (0,1). All endpoint port IDs are zero. Routing bounds are 4x4 for the existing generator's power-of-two X requirement; only the central Router is instantiated. No DAT merge or mesh is instantiated.

Each destination has a memory and config SAM window. WEST retains the original base; NORTH, EAST and SOUTH use successive 4 GiB windows. Response headers return to NMU coordinate (1,1), endpoint port zero.

Patterns reuse gen_standalone_patterns.py with --profile cosim. Single/burst control/data names remain shared. The initial 14 cases complete writes before dependent readback. The additional cases below add explicit initialization and concurrent traffic phases. Transactions use disjoint addresses, initialized byte lanes and INCR bursts supported by the existing AXI scoreboard. Random inputs retain a reproducible seed. The additional directed catalog supplies explicit partial-write initialization and read/write concurrency phases. FIXED/WRAP remain outside this co-simulation coverage.

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

In-flight reset remains outside this generated list. PASS requires correct counts, scenario coverage and clean checker diagnostics. The corruption test has a separate expected-failure marker.

## Additional directed cases

All eight additions pass VCS. See [results](../../../docs/archive/nmu-cosim-84/additional/report.md) and the linked waveform guide.

The shared catalog `sim/test_patterns/cosim/cases.json` adds these pairs:

| Control | Data | Purpose |
|---|---|---|
| ctrl_backpressure | data_backpressure | Delay B/R acceptance after VALID and check stable payload |
| ctrl_capacity_recover | data_capacity_recover | Fill NMU receive buffers and reach per-ID limits, then drain |
| ctrl_partial_write | data_partial_write | Initialize, partially overwrite, and compare preserved/updated bytes |
| ctrl_read_write | data_read_write | Read initialized region A while writing disjoint B, then read back B |

Run each with `make run CASE=<name>` or `make nWave CASE=<name>` on the workstation. `pattern.txt` lists all 24 cases. Explicit backpressure/capacity cases delay response acceptance; ordering cases delay selected memory responses. Production capacity defaults and the C++ model are unchanged.

Capacity cases require actual full receive buffers, remap-limit stalls and AW/AR stalls before completion. They exercise NMU resources, not the capacity of a future NSU RTL. Concurrent cases require live read/write overlap plus W/R transfers during opposite-direction outstanding traffic. All phases use the existing memory scoreboard.

`make nWave` loads `signals.rc` with NMU request/response groups followed by Router NoC ports and NSU NoC/memory AXI ports. Router arrays use ports 0/1/2/3/4 for LOCAL/NORTH/EAST/SOUTH/WEST. NSU instances are gen_nsu[0..3] for NORTH/EAST/SOUTH/WEST. The selected FSDB path is substituted into `build/report_wave1/<CASE>.rc`. Override the template with `WAVE_RC=/path/to/signals.rc`.

## Ordering cases

`cross_id_out_of_order` and `same_id_cross_dst_reorder` reuse the shared case names and generator. Use `MODE=control`, `MODE=data` or `MODE=rand`; `auto` selects control for these two cases. All four destinations must receive writes and reads. Ordinary cases bypass every response delayer. Ordering cases enable the WEST memory's B/R delayer, with AW/W/AR delay zero and random stalls disabled.

```sh
make run_wave CASE=cross_id_out_of_order MODE=control
make nWave CASE=same_id_cross_dst_reorder MODE=data
```

From nmu-standalone/, add `TESTBENCH=cosim`. Reports for explicit MODE values are placed below `build/report_wave0/<MODE>/` or `build/report_wave1/<MODE>/`. The measured minimum common setting is `RSP_DELAY=2`: both case names pass in control, data and rand modes; control fails required disorder at 0 and 1 cycle. Random bursts can reorder naturally at zero. This threshold applies to the shipped seed and topology.

`RSP_DELAY` selects a chain of upstream one-cycle response delayers for delay-search experiments. The chain covers every integer cycle count without changing the upstream implementation. Each B/R beat is delayed; observed source backpressure is reported separately.

Ordering writes use seeded payloads with distinct active-byte signatures for the shipped cases, so exchanged read responses cannot hide behind repeating address bytes.

PASS requires actual B and R disorder at the NMU ingress, correct AXI data/counts, and per-ID ordering. The same-ID case also requires buffered B/R retirement. Delay zero is a coverage experiment and may correctly fail the required-disorder check. Reset during traffic and the standalone legacy mixed-capacity recipe remain unsupported; use the explicit co-simulation capacity cases.
