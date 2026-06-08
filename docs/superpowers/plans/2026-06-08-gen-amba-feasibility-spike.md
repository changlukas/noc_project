# gen_amba point-to-point feasibility spike — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]` checkboxes.

**Goal:** Prove the NMU/NSU bridge reads/writes correctly between gen_amba's golden AXI master BFM and golden `mem_axi`, point-to-point, under Verilator `--timing`.

**Architecture:** `gen_amba BFM → NMU →(NoC flit ifaces, direct)→ NSU → gen_amba mem_axi`. No crossbar. Self-clocked SV top `tb_genamba`, built `--binary --timing` (no `main.cpp`). Reuse `nmu_wrap`/`nsu_wrap` + existing DPI (`cmodel_dpi.cpp`). PASS = BFM writes distinct patterns to several addresses, reads back, compares.

**Spec:** `docs/superpowers/specs/2026-06-08-gen-amba-integration-feasibility-design.md`.

## Preconditions
- gen_amba IP already vendored at `cosim/sv/genamba/` (this branch).
- Build `c_model` first so yaml-cpp exists at `c_model/build/_deps/yaml-cpp-build/libyaml-cpp.a` (same dep as the existing `tb_top` build).
- Verilator 5.040+. Windows/msys2: run via `cosim/verilator/run_genamba.ps1` (sets PATH incl. `C:\msys64\usr\bin` for `sh.exe`, `LC_ALL=C`, `PYTHON3=py`). Linux/macOS: `make -C cosim/verilator genamba`.

## Key facts (verified)
- `mem_axi` params: `AXI_WIDTH_CID=0, AXI_WIDTH_ID=8, AXI_WIDTH_AD=64, AXI_WIDTH_DA=256, SIZE_IN_BYTES=4096`. Ports UPPERCASE; AXI4 (`AWLEN[7:0]`,`AWLOCK` 1b) under `+define+AMBA_AXI4`; qos/region under `+define+AMBA_QOS`; cache/prot under `+define+AMBA_AXI_CACHE/AMBA_AXI_PROT`. Tie `CSYSREQ=1'b1`. `mem_axi.v` text-includes `mem_axi_dpram_sync.v` → list ONLY `mem_axi.v` as a source; rely on `-Icosim/sv/genamba`.
- **Use gen_amba's golden self-checking test, not the bare tasks.** `mem_test_tasks.v::mem_test(startA,endA,bnum,delay)` writes `get_data()&get_mask(addr,bnum)` per address — `get_data` fills the full `WIDTH_DA` with `$random` words, so each transfer carries **nonzero random data in its active lane** (both low and high 16-byte lanes across stepped addresses; distinct per address ⇒ aliasing-detecting). It then reads each back, compares, and sets the global `reg error_flag=1` on any mismatch. (Note: `mem_test_burst` uses `addr&mask`, which is **0** in the upper lane for small addresses — do NOT use it; use `mem_test`.) PASS = `error_flag==0` — no hand-written patterns or compares. `error_flag` also has an `always` block in `mem_test_tasks.v` that `$finish(2)` on failure. The BFM module must declare the AXI regs/wires + `WIDTH_AD/WIDTH_ID/WIDTH_DA` + `dataW[]/dataR[]` + clock and `` `include `` BOTH `genamba/axi_master_tasks.v` and `genamba/mem_test_tasks.v` (use `cosim/sv/genamba/axi_tester.v` as the exact signal-env template). Use `bnum=16` — gen_amba's `get_mask` caps at 16 bytes (128-bit), so on the 256-bit bus this is a valid **narrow** transfer (DUT supports narrow per `AX4-BND-*`). Full 256-bit-width single-beat coverage would need extending gen_amba's mask (out of scope; note in findings).
- DUT `axi4_intf` is lowercase, 1:1 with mem_axi uppercase at AXI4 widths. `awregion/arregion` exist but are NOT marshalled by `nmu_wrap/nsu_wrap/cmodel_dpi` → tie region to `4'b0` at the gen_amba boundary; do not treat NSU as driving region.
- DPI lifecycle (copy from `tb_top.sv:347-390`): `cmodel_init` via `+scenario=%s` plusarg before reset deassert; per-cycle `string m; int c; c=cmodel_check_error(m); if(c)$fatal;`; `cmodel_finalize()` once after `$finish`. Do NOT gate PASS on `cmodel_done()` (the master adapter is never ticked here). Use an existing scenario file, e.g. `tests/scenarios/AX4-BAS-001_single_write_no_read/scenario.yaml`, just to satisfy `cmodel_init` (its content is unused).

---

## Task 1: Build target + mem_axi standalone

**Files:** add `genamba` target to `cosim/verilator/Makefile`; create `cosim/sv/tb_genamba.sv`; create `cosim/verilator/run_genamba.ps1`.

- [ ] **Step 1:** Add to `cosim/verilator/Makefile` a `genamba` target reusing the existing vars (`SPECGEN_SV_INC, CMODEL_INC, YAMLCPP_INC, YAMLCPP_LIB`, all `-CFLAGS`), but: `--binary --timing` (not `--cc --exe --no-timing`), `--top-module tb_genamba`, `-Icosim/sv/genamba`, the genamba `+define+`s (`AMBA_AXI4 AMBA_QOS AMBA_AXI_CACHE AMBA_AXI_PROT assume=assert`), `-Wno-NULLPORT -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-REDEFMACRO`, `-LDFLAGS "$(YAMLCPP_LIB)"`, `-LDFLAGS "-Wl,--stack,67108864"`. SV list: `ni_params_pkg.sv` then `ni_signals_pkg.sv`, `nmu_wrap.sv`, `nsu_wrap.sv`, `genamba/mem_axi.v`, `wb2axip/{faxi_wstrb,faxi_master,faxi_slave}.v`, `tb_genamba.sv`. C: `cmodel_dpi.cpp` only (NO `main.cpp`).

- [ ] **Step 2:** Write `cosim/sv/tb_genamba.sv` (initial): `` `include "wb2axip/sim_wrapper.svh" `` at top; self-clock `reg ACLK=0; always #5 ACLK=~ACLK;`, `reg ARESETn=0;`; instantiate only `mem_axi #(.AXI_WIDTH_CID(0),.AXI_WIDTH_ID(8),.AXI_WIDTH_AD(64),.AXI_WIDTH_DA(256),.SIZE_IN_BYTES(4096))` with all AXI inputs tied idle and `CSYSREQ=1'b1`; `initial begin repeat(4)@(posedge ACLK); ARESETn=1; repeat(10)@(posedge ACLK); $finish; end`.

- [ ] **Step 3:** Write `run_genamba.ps1` (Windows driver):
```powershell
$env:Path="C:\msys64\mingw64\bin;C:\msys64\usr\bin;"+$env:Path; $env:LC_ALL="C"
make -C "$PSScriptRoot" genamba PYTHON3=py
if($LASTEXITCODE){exit 1}
& "$PSScriptRoot\obj_genamba\Vtb_genamba.exe" +scenario="$PSScriptRoot\..\..\tests\scenarios\AX4-BAS-001_single_write_no_read\scenario.yaml"
```

- [ ] **Step 4:** Build + run. Expected: exit 0, sim reaches `$finish`. Proves `mem_axi` compiles/runs in our build at AD=64/DA=256/ID=8 under `--timing`.

- [ ] **Step 5:** `git commit -m "build(cosim): genamba verilator target + mem_axi standalone"`

## Task 2: Golden BFM ↔ mem_axi data check

**Files:** create `cosim/sv/genamba_master_bfm.sv`; modify `tb_genamba.sv`, `Makefile` (add bfm to genamba SV list).

- [ ] **Step 1:** Write `genamba_master_bfm.sv` — copy the AXI reg/wire + `WIDTH_*` + `dataW[]/dataR[]` env from `cosim/sv/genamba/axi_tester.v`, `` `include "genamba/axi_master_tasks.v" `` and `` `include "genamba/mem_test_tasks.v" ``, expose AXI as a UPPERCASE port bundle + `ACLK/ARESETn`. Add (golden test only — no hand-written patterns):
```verilog
task run_spike;
  // address-as-data over a small window inside the 4 KiB mem; sets error_flag on mismatch
  mem_test(64'h0000, 64'h00FF, 16, 0); // 16 x 16-byte narrow xfers, full-width random data per lane
  if (error_flag) $fatal(1, "SPIKE FAIL: gen_amba mem_test mismatch");
  else            $display("SPIKE PASS");
endtask
```

- [ ] **Step 2:** In `tb_genamba.sv`, wire BFM ↔ mem_axi directly (UPPERCASE 1:1, region tied `4'b0`), `CSYSREQ=1`; `initial` calls `bfm.run_spike(); $finish;`.

- [ ] **Step 3:** Build + run. Expected: `SPIKE PASS`, no `$fatal`. Proves gen_amba BFM+mem interoperate (golden-vs-golden) in our build.

- [ ] **Step 4:** `git commit -m "test(cosim): gen_amba BFM<->mem_axi data readback passes"`

## Task 3: Insert the DUT bridge

**Files:** modify `tb_genamba.sv`, `Makefile` (add specgen pkgs already present; ensure nmu/nsu wraps in list).

- [ ] **Step 1:** Replace the direct BFM↔mem wiring with the bridge:
  - `axi4_intf bfm_nmu(); axi4_intf nsu_mem();` `noc_req_intf nmu_nsu_req(); noc_rsp_intf nsu_nmu_rsp();`
  - AXI adapter BFM(UPPERCASE) → `bfm_nmu` (lowercase); region tied 0.
  - `nmu_wrap u_nmu(.clk_i(ACLK),.rst_ni(ARESETn),.axi_i(bfm_nmu.slave),.noc_req_o(nmu_nsu_req.master),.noc_rsp_i(nsu_nmu_rsp.slave));`
  - direct: NMU.noc_req_o→NSU.noc_req_i, NSU.noc_rsp_o→NMU.noc_rsp_i.
  - `nsu_wrap u_nsu(.clk_i(ACLK),.rst_ni(ARESETn),.noc_req_i(nmu_nsu_req.slave),.noc_rsp_o(nsu_nmu_rsp.master),.axi_o(nsu_mem.master));`
  - AXI adapter `nsu_mem`(lowercase) → mem_axi (UPPERCASE); region inputs tied 0.
  - addresses `'h000/'h040/'h080` are < 4 KiB with distinct low-bit indices (no aliasing).

- [ ] **Step 2:** Add `cmodel_init` via `+scenario=%s` plusarg in `initial` before `ARESETn=1` (copy `tb_top.sv:56-62`).

- [ ] **Step 3:** Build + run. Expected: exit 0 (DUT DPI + gen_amba VIP link together — the coexistence proof); `SPIKE PASS` (data round-trips through the bridge).

- [ ] **Step 4:** `git commit -m "test(cosim): BFM->NMU->NoC->NSU->mem_axi readback passes"`

## Task 4: DPI lifecycle + wb2axip checker

**Files:** modify `tb_genamba.sv`.

- [ ] **Step 1:** Add centralized DPI lifecycle (adapt `tb_top.sv:373-390`): per-`posedge` `begin string m; int c; c=cmodel_check_error(m); if(c) $fatal(1,m); end`. Because `--binary` has **no `main.cpp`**, call finalize from SV via `final begin cmodel_finalize(); end` (tb_top relies on main.cpp for this). Do NOT use `cmodel_done()` for PASS — the master adapter is never ticked here; PASS comes from `run_spike`'s `error_flag` check.

- [ ] **Step 2:** Add `faxi_slave` on `bfm_nmu` (NMU as slave), params per `tb_top.sv:208-267` (`C_AXI_ID_WIDTH=8,DATA=256,ADDR=64,OPT_EXCLUSIVE=0,F_LGDEPTH=10,F_AXI_MAXSTALL=32,F_AXI_MAXRSTALL=32,F_AXI_MAXDELAY=500`). (Build already has `--assert +define+assume=assert -Wno-NULLPORT` and `sim_wrapper.svh` included.)

- [ ] **Step 3:** Build + run. Expected: `SPIKE PASS`, no checker violation, no DPI error.

- [ ] **Step 4:** `git commit -m "test(cosim): add DPI lifecycle + wb2axip checker on NMU boundary"`

## Task 5: Findings

**Files:** create `docs/superpowers/specs/2026-06-08-gen-amba-feasibility-findings.md`.

- [ ] **Step 1:** One page: go/no-go; what passed (DPI+VIP coexist build, per-address readback, checker silent); any signal/macro/reset tweak actually needed; residual work for the crossbar/role-2 multi-instance version. Then remove this plan (CLAUDE.md) or keep for the follow-on.
- [ ] **Step 2:** `git commit -m "docs(spike): gen_amba feasibility findings"`

---

## Coverage vs spec
§3.1 clocking→T1/T3; §3.2 widths/macros→T1 (defines)+T3 (adapters); region tie-0→T2/T3; §3.3 address window→T3; §3.4 components→T1-T4; lifecycle→T4; §3.5 stimulus→T2/T3; success criteria→T3 (coexist), T2/T3 (readback), T4 (checker), T5 (writeup).
