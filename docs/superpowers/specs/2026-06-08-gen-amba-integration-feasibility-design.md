# gen_amba integration feasibility — design spec

Status: draft for review (rev 5)
Date: 2026-06-08

## 1. Problem

The NoC behaviour model (AXI Master → NMU → NoC → NSU → AXI Slave) is AI-authored
and self-verified (wb2axip checkers exercise our own model against our own harness).
We want independent evidence that the NMU/NSU AXI interfaces are correct by driving
them with `gen_amba_2021`'s externally AXI-compliant golden VIP. If our NMU/NSU
bridge reads/writes correctly between gen_amba's golden master BFM and golden
`mem_axi`, that is independent cross-tool confirmation.

This spec is a **feasibility spike** (path B): the minimal point-to-point form that
flushes out any remaining gen_amba-side risk cheaply. It validates **role 1**
(golden AXI endpoints around the bridge). The **crossbar / role 2** (NoC bridge as a
node in a real AXI fabric) is deferred to the next effort, which also needs the
flexible multi-instance cosim foundation (§4).

## 2. Ground truth references

- DUT AXI `axi4_intf` (`specgen/generated/sv/ni_signals_pkg.sv`): AXI4, `ID=8`,
  `ADDR=64`, `DATA=256`, with `aw/ar` `cache/prot/qos/region/lock`.
- NoC link `noc_req_intf` / `noc_rsp_intf`: `valid` + `flit[407:0]` +
  `credit_return[NUM_VC]`; `.master`/`.slave` modports mate point-to-point. cosim
  flow control is **stubbed** — NMU/NSU shell adapters hard-code `credit_return=false`
  (`nmu_shell_adapter.hpp:195`, `nsu_shell_adapter.hpp:194`); no real backpressure.
- Co-sim: `*_wrap.sv` (DPI beta-tick shells), `cmodel_dpi.cpp` (hermetic singleton —
  one adapter each; IDs stored `uint8_t & 0xFF`), `cosim/verilator/Makefile` +
  `main.cpp` (C++ drives clk/rst). Existing checker wiring in `tb_top.sv:208` (faxi_slave)
  and `:279` (faxi_master).
- gen_amba VIP: `axi_master_tasks.v` (golden BFM tasks), `mem_axi.v` (golden slave,
  `AXI_WIDTH_ID/CID/AD/DA` params; indexes `ADDR_LENGTH` low bits at `mem_axi.v:200`).
  Optional signals gated by `AMBA_QOS` (qos+region), `AMBA_AXI_CACHE`, `AMBA_AXI_PROT`;
  port/width set also depends on `AMBA_AXI4`. (NOT `AMBA_AXI_QOS`.)
- Simulators present: Verilator 5.040 (`--timing`), Icarus Verilog. No ModelSim/xsim.

### 2.1 Spike results already established this session

- Verilator 5.040 `--timing` builds gen_amba's full VIP into a runnable exe (verified).
  Central "can Verilator host gen_amba's VIP?" risk retired.
- gen_amba's native full self-test is impractically slow under `--timing` → drive
  minimal, per-transaction traffic.
- Build gen_amba and run Verilator via PowerShell (native gcc; Bash sandbox blocks cc1);
  `--binary` link needs `sh.exe` (`C:\msys64\usr\bin`) on PATH.

## 3. Design — point-to-point golden-endpoint spike

No crossbar. The bridge sits between two golden gen_amba endpoints:

```
gen_amba BFM (golden) ─AXI(ID8,AD64,DA256)→ NMU ── noc_req/noc_rsp (direct) ── NSU ─AXI→ gen_amba mem_axi (golden)
```

- **No crossbar** ⇒ no ID widening / CID, no `WIDTH_SID`, no ≥2 master/slave
  constraint, no S0/S1, no duplicate memory. AXI ID is plain 8-bit end to end.
- **NoC transport removed**: NMU and NSU connect directly via the NoC flit
  interfaces (`.master`/`.slave` mate). Credits are stubbed (no backpressure), so the
  direct link is safe only for **bounded** traffic; the spike drives few transactions.
- Validates role 1: the golden BFM exercises the NMU's AXI slave port; the NSU's AXI
  master port drives the golden `mem_axi`; data must round-trip.

### 3.1 Clocking / build

`tb_genamba` is a **self-clocked SV top** built `--timing --binary`: it generates its
own clock/reset (single domain for DUT wraps + gen_amba VIP), calls `cmodel_init`
before reset deassertion, and **does not reuse `main.cpp`** (whose C++-driven clock
would conflict with gen_amba's SV-timed BFM). A new Verilator target builds the DUT
DPI sources + gen_amba BFM/`mem_axi` (no crossbar RTL needed).

`tb_genamba` must replicate `tb_top`'s **centralized DPI lifecycle** (the wraps
delegate it): poll `cmodel_check_error()` every active cycle and `$fatal` on a
non-zero code, and call `cmodel_finalize()` exactly once after `$finish` — per
`tb_top.sv:373`. Without this, DPI-side failures pass silently.

### 3.2 Widths and optional signals

- `mem_axi` at `WIDTH_AD=64`, `WIDTH_DA=256`, plain 8-bit ID (`AXI_WIDTH_ID=8`,
  `AXI_WIDTH_CID=0`). The BFM drives the same widths.
- Build defines `AMBA_AXI4`, `AMBA_QOS`, `AMBA_AXI_CACHE`, `AMBA_AXI_PROT` so the
  BFM/`mem_axi` carry qos/cache/prot to match the NMU/NSU `axi4_intf`; any signal
  still absent on the gen_amba side is tied to a safe default at the boundary
  (qos=0, prot=0, cache=dflt).
- **`AWREGION/ARREGION` are NOT carried by the DUT DPI wrappers** (verified: no region
  in `nmu_wrap.sv` / `nsu_wrap.sv` / `cmodel_dpi.cpp`). Drive the BFM- and NSU-side
  region to 0; **region is untested** in this spike (transporting it would need a DPI
  extension — see §4).

### 3.3 Address window (anti-aliasing)

`mem_axi` indexes only `ADDR_LENGTH` low bits (`mem_axi.v:200`), so a single
write-then-read cannot detect aliasing. Pin numeric `mem_axi SIZE_IN_BYTES/ADDR_LENGTH`,
and (see §3.5) write distinct data to several addresses spread across the window
before reading them back.

### 3.4 Components

- Reuse unchanged: `nmu_wrap.sv`, `nsu_wrap.sv`, `cmodel_dpi.cpp` (`g_loopback_adapter`
  constructed but left undriven; no `loopback_noc_wrap`).
- New: `cosim/sv/tb_genamba.sv` — self-clocked top: golden BFM → NMU (AXI), NMU↔NSU NoC
  interfaces directly connected, NSU → golden `mem_axi` (AXI). The BFM is a thin module
  that `include`s gen_amba's `axi_master_tasks.v` and issues the golden write/read
  tasks against the NMU AXI. Thin signal adapters for optional-signal alignment.
- New: explicit wb2axip `faxi_slave` on the BFM↔NMU AXI (NMU as slave) and optionally
  `faxi_master` on the NSU↔`mem_axi` AXI (NSU as master), configured per `tb_top.sv:208/279`
  — `C_AXI_ID_WIDTH=8 / DATA=256 / ADDR=64`, `OPT_EXCLUSIVE=0`, the project's
  `F_LGDEPTH/F_AXI_MAXSTALL/MAXRSTALL/MAXDELAY`, `include "wb2axip/sim_wrapper.svh"`,
  built with `--assert` and `+define+assume=assert`. NOT inherited from `tb_top`.
- Scoreboarding at the **SV BFM boundary** (write/readback compare). The existing C++
  `g_scoreboard` is wired to `g_master_adapter` (unused here) and does not observe the BFM.
- New: a Verilator `--binary --timing` target for `tb_genamba`.
- Reuse gen_amba: `axi_master_tasks.v`, `mem_axi.v` (no crossbar).

### 3.5 Stimulus and checking

The golden BFM writes **distinct data patterns to several addresses spread across the
pinned window**, then reads each back and asserts **read data == the value written to
that address** (catches aliasing; a single address could not). In parallel the wb2axip
checker(s) must report no protocol violations. Traffic is deliberately tiny (bounded,
since cosim credits are stubbed).

## 4. Out of scope

- The crossbar / role 2 (NoC bridge as an AXI-fabric node) — the next effort, which
  needs both the gen_amba crossbar (`WIDTH_ID=7`⇒`WIDTH_SID=8` etc.) and the flexible
  multi-instance cosim foundation (instance-indexed DPI + adapter vectors +
  parameterized wraps, enabling N NI/router like RTL instantiation).
- gen_amba's full native self-test (too slow under `--timing`).
- Any change to the DUT's AXI ID width or `uint8_t` ID path.
- AXI `REGION` transport (not carried by the DPI wrappers; tied to 0, untested here).

## 5. Success criteria

- `tb_genamba` builds under Verilator `--binary --timing` as a self-clocked top with DUT
  DPI and gen_amba BFM/`mem_axi` coexisting (single executable).
- Per-transaction write-then-readback returns correct data for several distinct
  addresses through `BFM → NMU → NoC flits → NSU → mem_axi → back`.
- The explicitly-instantiated wb2axip checker(s) report no violations.
- A written go/no-go conclusion plus the residual work for the crossbar/multi-instance version.

## 6. Risks

- Clocking: self-clocked SV top + `--timing` scheduler coexisting with the existing
  clocked DUT DPI wraps in one binary (proven separately, not together).
- Address aliasing false pass — mitigated by pinned ranges + multi-address readback (§3.3/§3.5).
- No NoC backpressure (credits stubbed) — bound the spike traffic.
- Optional-signal alignment between gen_amba BFM/`mem_axi` and the DUT `axi4_intf`.
- Reset alignment: DUT sync active-low `rst_ni` vs gen_amba `ARESETn` / async reset in `mem_axi`.
