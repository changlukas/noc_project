# Task 2 Report: mesh_2x1_vc8 NoC-struct early elaboration spike (REDO)

## Context

Prior spike (commit 746ea1f) used `ni_signals_pkg::noc_credit_t` for the credit face,
which no longer exists in `ni_signals_pkg` (removed in commit 595a25d).  That type was
also derived from `NOC_NUM_VC_DFLT=1`, so `credit[0:0]` (1-bit) — it never verified vc8
width.  This REDO replaces the credit face with `noc_types_pkg::noc_credit_t` from
`noc_types_pkg_vc8.sv`, which defines `credit[7:0]` (real 8-bit vc8).

---

## Step 1 — Verilator lint-only

**Command:**
```
VERILATOR_ROOT=/c/msys64/mingw64/share/verilator \
/c/msys64/mingw64/bin/verilator --lint-only --timing \
  -IE:/05_NoC/noc_project/specgen/generated/sv \
  -IE:/05_NoC/noc_project/sim/sv/spike \
  E:/05_NoC/noc_project/specgen/generated/sv/ni_params_pkg.sv \
  E:/05_NoC/noc_project/specgen/generated/sv/ni_signals_pkg.sv \
  E:/05_NoC/noc_project/specgen/generated/sv/ni_flit_pkg.sv \
  E:/05_NoC/noc_project/specgen/generated/sv/noc_types_pkg_vc8.sv \
  E:/05_NoC/noc_project/sim/sv/spike/noc_fabric_spike.sv \
  E:/05_NoC/noc_project/sim/sv/spike/tb_top_spike.sv
```

**Output:**
```
- V e r i l a t i o n   R e p o r t: Verilator 5.036 2025-04-27 rev UNKNOWN.REV
- Verilator: Built from 0.187 MB sources in 11 modules, into 0.045 MB in 5 C++ files needing 0.000 MB
- Verilator: Walltime 0.017 s (elab=0.001, cvt=0.002, bld=0.000); cpu 0.000 s on 1 threads; alloced 12.922 MB
```

Exit code: 0.  Zero errors, zero warnings.  11 modules (vs 10 in the prior spike — the
extra module is `noc_types_pkg`).

---

## Step 2 — Confirmed credit[7:0] width

Verilator-generated C++ header (`Vtb_top_spike___024root.h`):
```cpp
CData/*7:0*/ tb_top_spike__DOT__u_fabric__DOT____Vcellout__u_node1__cred_o;
VlUnpacked<CData/*7:0*/, 2> tb_top_spike__DOT__node_cred;
```

- `node_cred[0]` element type: `CData[7:0]` → 8-bit vc8 credit confirmed.
- `node_cred` array: `VlUnpacked<CData/*7:0*/, 2>` → struct-in-unpacked-array elaboration
  confirmed.

This is distinct from the prior spike where `ni_signals_pkg::noc_credit_t.credit` would
have been `[0:0]` (1-bit, `NOC_NUM_VC_DFLT=1`).

---

## Step 3 — Handshake simulation

**Build command:**
```
verilator --cc --exe --timing --build --top-module tb_top_spike \
  -I.../specgen/generated/sv -I.../sim/sv/spike \
  ni_params_pkg.sv ni_signals_pkg.sv ni_flit_pkg.sv noc_types_pkg_vc8.sv \
  noc_fabric_spike.sv tb_top_spike.sv spike_main.cpp \
  --Mdir obj_dir
```

**Run output:**
```
SPIKE PASS: struct-port-in-array elaboration + handshake OK
  node_req[0].valid=0 flit[7:0]=0xa5
  node_cred[0].credit=8'b00000000 (vc8 real 8-bit)
- .../tb_top_spike.sv:59: Verilog $finish
```

`flit[7:0]=0xa5` — data path through noc_chan_t.flit confirmed.
`credit=8'b00000000` — 8-bit format confirms vc8 credit field (cleared one cycle after
`cred_seen` latched — expected; display fires when `cred_seen[0]===1`, which is the cycle
after the 8-bit credit pulse).

---

## GO/NO-GO: GO

| Check | Result |
|---|---|
| lint-only exit code | 0 |
| noc_types_pkg::noc_credit_t width | `credit[7:0]` confirmed in generated C++ |
| struct-in-unpacked-array port | `VlUnpacked<CData/*7:0*/,2>` — elaborates cleanly |
| genvar generate wiring of struct-array ports | no error |
| one-cycle req→credit handshake | SPIKE PASS printed |

`noc_types_pkg::noc_credit_t` from `noc_types_pkg_vc8.sv` as a `[NUM_NODES]` unpacked
array port, wired by `genvar generate`, elaborates and simulates cleanly in Verilator
5.036 with real 8-bit credit width.  Task 3 (4x4-vc8 full struct generator) is unblocked.

---

## What changed vs prior spike (746ea1f)

| Face | Prior spike | This REDO |
|---|---|---|
| Channel (`noc_chan_t`) | `ni_signals_pkg::noc_chan_t` | unchanged |
| Credit | `ni_signals_pkg::noc_credit_t` (type no longer exists) | `noc_types_pkg::noc_credit_t` |
| Credit width | `credit[0:0]` (1-bit, `NOC_NUM_VC_DFLT=1`) | `credit[7:0]` (8-bit, real vc8) |
| File list | no `noc_types_pkg_vc8.sv` | added `noc_types_pkg_vc8.sv` |

---

## Files modified

- `sim/sv/spike/noc_fabric_spike.sv` — credit face: `noc_types_pkg::noc_credit_t`
- `sim/sv/spike/tb_top_spike.sv` — credit face: `noc_types_pkg::noc_credit_t`; display format `8'b%08b`
