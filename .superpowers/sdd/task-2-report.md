# Task 2 Report: mesh_2x1_vc8 NoC-struct early elaboration spike

## Verilator lint-only command

```bash
VERILATOR_ROOT=/c/msys64/mingw64/share/verilator \
/c/msys64/mingw64/bin/verilator_bin.exe --lint-only --timing \
  -IE:/05_NoC/noc_project/specgen/generated/sv \
  -IE:/05_NoC/noc_project/sim/sv/spike \
  E:/05_NoC/noc_project/specgen/generated/sv/ni_params_pkg.sv \
  E:/05_NoC/noc_project/specgen/generated/sv/ni_signals_pkg.sv \
  E:/05_NoC/noc_project/specgen/generated/sv/ni_flit_pkg.sv \
  E:/05_NoC/noc_project/sim/sv/spike/noc_fabric_spike.sv \
  E:/05_NoC/noc_project/sim/sv/spike/tb_top_spike.sv
```

## Lint-only output

```
- V e r i l a t i o n   R e p o r t: Verilator 5.036 2025-04-27 rev UNKNOWN.REV
- Verilator: Built from 0.153 MB sources in 10 modules, into 0.045 MB in 5 C++ files needing 0.000 MB
- Verilator: Walltime 0.010 s (elab=0.001, cvt=0.003, bld=0.000); cpu 0.000 s on 1 threads; alloced 12.906 MB
```

**Exit code: 0. Zero errors, zero warnings.**

## Handshake simulation output

```
SPIKE PASS: struct-port-in-array elaboration + handshake OK
  node_req[0].valid=0 flit[7:0]=0xa5
  node_cred[0].credit=0
- E:/05_NoC/noc_project/sim/sv/spike/tb_top_spike.sv:56: Verilog $finish
```

`flit[7:0]=0xa5` confirms the data path through the struct field. `cred_seen_o` latched in the FF; the display fires one cycle later when valid/credit have cleared — expected behavior.

## GO/NO-GO: GO

`ni_signals_pkg::noc_chan_t` and `ni_signals_pkg::noc_credit_t` as module ports in `[NUM_NODES]` unpacked arrays, wired by `genvar generate`, elaborate and simulate cleanly in Verilator 5.036. No unpacked-array element-type mismatch error.

## §vc8-width finding

`noc_credit_t.credit` is `logic [NOC_NUM_VC_DFLT-1:0]` = `logic [0:0]` (1 bit) because `ni_params_pkg::NOC_NUM_VC_DFLT = 1`. The spike exercises struct-in-array elaboration at this width. The struct typedef is **fixed-width** (package localparam, not a module parameter); a different NUM_VC value (e.g. 8) cannot be expressed via port-level parameterization of the existing `noc_credit_t` type.

**Implication for Task 3+:** if the generator needs per-topology VC widths (num_vc=8 vs num_vc=1), either (a) keep one struct per DFLT width and always compile at the default (generation-time substitution), or (b) generate separate typedef variants per VC count. The elaboration mechanism (struct-in-unpacked-array + generate) is confirmed working; the width question is a generator-design choice, not a Verilator limitation.

## Files added

- `sim/topologies/mesh_2x1_vc8.yaml`
- `sim/sv/spike/noc_fabric_spike.sv`
- `sim/sv/spike/tb_top_spike.sv`
- `sim/sv/spike/spike_main.cpp` (Verilator build driver; parallel to `sim/verilator/main.cpp`)

## Commit

`test(sim): mesh_2x1_vc8 noc-struct early elaboration spike`
