# pulp AXI VIP Test Node Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the self-made C++ AXI master/slave co-sim endpoints with pulp-platform/axi VIP (rand_master/rand_slave/scoreboard) + FlooNoC test components, per `docs/2026-07-02-pulp-axi-vip-node-design.md`, including the file-tree relayout (`src/` for DUT, `sim/dv/` for imported DV IP).

**Architecture:** Phase A relayouts the tree with everything green at each commit. Phase B imports the DV IP and proves the existing co-sim builds/runs on WSL. Phase C swaps the endpoint internals atomically (endpoint + generator + DPI excision in one green commit), then adds the transport flavor and rewires regression.

**Tech Stack:** SystemVerilog (Verilator 5.048 Windows compile-gate / WSL Ubuntu run), C++17 DPI, Python 3 generators, pulp axi 0.39.7, common_verification 0.2.5, FlooNoC hw/test components.

## Global Constraints

- Reply to user in Traditional Chinese; technical terms English. Commit messages English `type(scope): description`.
- Every commit compiles and passes: ctest full suite AND `make build-verilator TOPOLOGY=mesh_4x4_vc1` (Windows). Tasks that change runtime behavior additionally verify `run-tb-top` (WSL for random runs).
- Windows shell: build with `PYTHON3=python3` (never `py -3` — it pollutes verilator generated .cpp).
- WSL: `wsl.exe -d Ubuntu -u root`; set `export PATH=/usr/bin:/bin:/usr/local/bin` first (msys2 shadowing); Verilator 5.048 at `/usr/local/bin/verilator`, z3 4.13.3 at `/usr/bin/z3`; solver env `VERILATOR_SOLVER="z3 --in"`.
- pulp / FlooNoC sources land VERBATIM — never edit files under `sim/dv/`. Glue only in `sim/tb/`.
- All .hpp/.cpp edits: run `clang-format -i` (repo `.clang-format`). SV/Python: 4-space continuation indent, snake_case.
- NO push. Commit locally on branch `feat/verilator-5048-axi-sv-bfm`; user pushes.
- Do not run `--lint-only` on the full `axi_test.sv` (dead-class elaboration hits UNSUPPORTED `$bits` in `axi_chan_logger`; a real top prunes it — verified).
- ATOP and user-signal are OUT OF SCOPE (spec: struct has no `awatop`/`*user`; all runs `AXI_ATOPS=0`, `AX_USER_RAND=0`).
- If a step deviates from this plan or the spec (missing file, API mismatch, red test you didn't cause): STOP and report BLOCKED — no inline workarounds.

**Paths:** repo root `E:\05_NoC\noc_project` (Windows) = `/mnt/e/05_NoC/noc_project` (WSL). Scratchpad clones (source for dv import):
`C:\Users\USER\AppData\Local\Temp\claude\E--05-NoC-noc-project\b6a615f0-3df3-4741-a405-61e1ea019499\scratchpad\` — `axi_0397/` (tag v0.39.7), `cv_025/` (common_verification v0.2.5), `floonoc/` (hw/test components).

---

### Task 1: Relayout A — DUT SV wraps + DPI to `src/`

**Files:**
- Move: `sim/sv/{ni_wrap,nmu_wrap,nsu_wrap,router_wrap}.sv`, `sim/sv/noc_fabric_mesh_*.sv` → `src/sv/`
- Move: `sim/c/{cmodel_dpi.cpp,cmodel_dpi.h,dpi_boundary_macros.h,handle_block.hpp}` → `src/dpi/`
- Modify: `sim/build_config.mk`, `sim/verilator/Makefile`, `sim/vcs/Makefile`, `sim/tools/gen_tb_top.py`

**Interfaces:**
- Consumes: current build (green on Verilator 5.048, commit `1c14695`).
- Produces: `src/sv/` + `src/dpi/` paths that Tasks 2-9 reference. Make variables `SRC_SV := $(PROJ_ROOT)/src/sv`, `SRC_DPI := $(PROJ_ROOT)/src/dpi` in `build_config.mk`.

- [ ] **Step 1: git mv**

```bash
cd E:/05_NoC/noc_project
mkdir -p src/sv src/dpi
git mv sim/sv/ni_wrap.sv sim/sv/nmu_wrap.sv sim/sv/nsu_wrap.sv sim/sv/router_wrap.sv src/sv/
git mv sim/sv/noc_fabric_mesh_2x4_vc1.sv sim/sv/noc_fabric_mesh_4x4_vc1.sv \
       sim/sv/noc_fabric_mesh_4x4_vc2.sv sim/sv/noc_fabric_mesh_4x4_vc4.sv \
       sim/sv/noc_fabric_mesh_4x4_vc8.sv src/sv/
git mv sim/c/cmodel_dpi.cpp sim/c/cmodel_dpi.h sim/c/dpi_boundary_macros.h sim/c/handle_block.hpp src/dpi/
rmdir sim/c
```

Note: `noc_fabric_*.sv` are generated artifacts, but they are DUT-side; after this task the generator emits them into `src/sv/` (Step 3).

- [ ] **Step 2: build_config.mk path updates**

In `sim/build_config.mk`:

```makefile
# add near CMODEL_INC:
SRC_SV  := $(PROJ_ROOT)/src/sv
SRC_DPI := $(PROJ_ROOT)/src/dpi
```

Replace in `TB_TOP_SV_SRC` (keep list order — wraps before tb files):
`$(COSIM_ROOT)/sv/nmu_wrap.sv` → `$(SRC_SV)/nmu_wrap.sv`, same for `router_wrap.sv`, `nsu_wrap.sv`, `ni_wrap.sv`.
Replace `DPI_C_SRC := $(COSIM_ROOT)/c/cmodel_dpi.cpp` → `DPI_C_SRC := $(SRC_DPI)/cmodel_dpi.cpp`.
Replace `-I$(COSIM_ROOT)/c` in `CPP_INCLUDE_FLAGS` → `-I$(SRC_DPI)`.
In `FILELIST_GEN_ARGS`, add `$(SRC_SV)` to the incdir list: `$(SPECGEN_SV_INC) $(COSIM_ROOT)/sv $(SRC_SV) -- $(TB_TOP_SV_SRC)`.

- [ ] **Step 3: Makefile + generator incdir/out-path updates**

`sim/verilator/Makefile`: add `-I$(SRC_SV)` to `VERILATOR_COMMON_FLAGS` (next to `-I$(COSIM_ROOT)/sv`). `sim/vcs/Makefile`: mirror (find the `+incdir+` list; add `+incdir+$(SRC_SV)`).
`sim/tools/gen_tb_top.py` `_fabric_path()`: fabric emits beside the generator's knowledge of src:

```python
def _fabric_path(out_path: Path, topo: dict) -> Path:
    return ROOT / "src" / "sv" / f"noc_fabric_{topo['topology']['name']}.sv"
```

(tb_top's `` `include "noc_fabric_<name>.sv" `` resolves via the new `-I$(SRC_SV)`.)

- [ ] **Step 4: grep for stale references**

```bash
grep -rn "sim/sv/ni_wrap\|sim/sv/nmu_wrap\|sim/sv/nsu_wrap\|sim/sv/router_wrap\|sim/c/" \
    Makefile sim/ src/ docs/backlog.md .github 2>/dev/null
```

Expected: no build-file hits (docs hits are handled in Task 9). Fix any build-file stragglers the same way as Step 2.

- [ ] **Step 5: build + run green (Windows)**

```bash
cd E:/05_NoC/noc_project && make build-verilator TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3
cd sim/verilator && make run-tb-top TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3
```

Expected: build OK; run.log tail shows `PASS: scenario complete, scoreboard clean`.

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "refactor(tree): move DUT sv wraps and dpi bridge to src/"
```

---

### Task 2: Relayout B — testbench files to `sim/tb/`

**Files:**
- Move: everything left in `sim/sv/` (`axi_master_wrap.sv`, `axi_slave_wrap.sv`, `axi_perf_monitor.sv`, `link_perf_monitor.sv`, `user_node_endpoint.sv`, `tb_top_*.sv`) → `sim/tb/`
- Modify: `sim/build_config.mk`, `sim/verilator/Makefile`, `sim/vcs/Makefile`, `sim/tools/gen_tb_top.py`

**Interfaces:**
- Produces: `sim/tb/` as the only tb source dir; `TB_TOP_SV = $(COSIM_ROOT)/tb/tb_top_$(TOPOLOGY).sv`.

- [ ] **Step 1: git mv**

```bash
cd E:/05_NoC/noc_project
git mv sim/sv sim/tb
```

- [ ] **Step 2: path updates**

`sim/build_config.mk`: replace every `$(COSIM_ROOT)/sv/` with `$(COSIM_ROOT)/tb/` (TB_TOP_SV, TB_TOP_SV_SRC entries, FILELIST_GEN_ARGS incdir).
`sim/verilator/Makefile` + `sim/vcs/Makefile`: `-I$(COSIM_ROOT)/sv` → `-I$(COSIM_ROOT)/tb`.
`sim/tools/gen_tb_top.py` default out: `ROOT / "sim" / "sv"` → `ROOT / "sim" / "tb"` (in `main()`), and the module docstring/`--out` help text.

- [ ] **Step 3: grep stale `sim/sv` references in build files**

```bash
grep -rn "sim/sv\|COSIM_ROOT)/sv" Makefile sim/ src/ 2>/dev/null | grep -v output/
```

Expected: none in build files (docs deferred to Task 9).

- [ ] **Step 4: build + run green (same commands as Task 1 Step 5)**

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "refactor(tree): rename sim/sv to sim/tb (testbench-only after src/ split)"
```

---

### Task 3: Relayout C — `c_model/` to `src/c_model/`

**Files:**
- Move: `c_model/` → `src/c_model/`
- Modify: root `Makefile` (`CMODEL_DIR`), `src/c_model/CMakeLists.txt` (`../specgen` → `../../specgen`), `sim/build_config.mk` (`CMODEL_INC`, `CMODEL_TESTS`, `DPI_HDR_DEPS` wildcards)

**Interfaces:**
- Produces: `src/c_model/` paths used by Tasks 6, 9.

- [ ] **Step 1: git mv + reference sweep**

```bash
cd E:/05_NoC/noc_project
git mv c_model src/c_model
grep -rn "c_model" Makefile sim/build_config.mk sim/verilator/Makefile sim/vcs/Makefile \
    src/c_model/CMakeLists.txt src/c_model/tests --include=CMakeLists.txt -l
```

Fix every build-file hit:
- root `Makefile`: `CMODEL_DIR := c_model` → `CMODEL_DIR := src/c_model`
- `src/c_model/CMakeLists.txt`: `"${CMAKE_CURRENT_SOURCE_DIR}/../specgen/..."` → `"${CMAKE_CURRENT_SOURCE_DIR}/../../specgen/..."` (all occurrences — grep `../specgen` inside the file and any tests/*/CMakeLists.txt)
- `sim/build_config.mk`: `CMODEL_INC := $(PROJ_ROOT)/c_model/include` → `$(PROJ_ROOT)/src/c_model/include`; same for `CMODEL_TESTS`; `DPI_HDR_DEPS` wildcards `$(PROJ_ROOT)/c_model/...` → `$(PROJ_ROOT)/src/c_model/...`
- check `src/c_model/tests/**/CMakeLists.txt` for any relative `../../..` escapes to specgen or config — adjust depth by one.

- [ ] **Step 2: clean rebuild + full ctest**

```bash
rm -rf build/cmodel && make build-cmodel PYTHON3=python3
cd build/cmodel && ctest --output-on-failure -j 8
```

Expected: all tests pass (499 as of `01c91d8`; count may drift — zero failures is the gate). Stale-path failures mean a missed CMake config-copy step — grep `port_params.yaml` in CMakeLists for copy destinations.

- [ ] **Step 3: co-sim still green (Task 1 Step 5 commands)**

- [ ] **Step 4: Commit**

```bash
git add -A && git commit -m "refactor(tree): move c_model under src/"
```

---

### Task 4: Import DV IP into `sim/dv/`

**Files:**
- Create: `sim/dv/axi-0.39.7/` — from scratchpad `axi_0397/`: `src/axi_pkg.sv`, `src/axi_intf.sv`, `src/axi_test.sv`, `include/axi/typedef.svh`, `include/axi/assign.svh`, `LICENSE` (Solderpad), plus any other `include/axi/*.svh` the two svh include internally (check `grep include include/axi/*.svh`)
- Create: `sim/dv/common_verification-0.2.5/` — `src/rand_id_queue.sv`, `LICENSE`
- Create: `sim/dv/floonoc-test/` — `hw/test/axi_reorder_compare.sv`, `hw/test/axi_bw_monitor.sv`, FlooNoC `LICENSE` (SHL-0.51); record the FlooNoC git rev in the README
- Create: `sim/dv/README.md` — provenance table (upstream URL, tag/rev, files taken, license), one line per package

**Interfaces:**
- Produces: compile-ready VIP set. Verified minimal (WSL 2026-07-02): `rand_id_queue.sv + axi_pkg.sv + axi_intf.sv + axi_test.sv` + `+incdir+<axi>/include` builds the full rand VIP (32 modules). `common_cells` NOT needed.

- [ ] **Step 1: copy files (preserve upstream relative layout under each package dir)**

```bash
SP="/c/Users/USER/AppData/Local/Temp/claude/E--05-NoC-noc-project/b6a615f0-3df3-4741-a405-61e1ea019499/scratchpad"
cd E:/05_NoC/noc_project
mkdir -p sim/dv/axi-0.39.7/src sim/dv/axi-0.39.7/include/axi \
         sim/dv/common_verification-0.2.5/src sim/dv/floonoc-test
cp "$SP/axi_0397/src/axi_pkg.sv" "$SP/axi_0397/src/axi_intf.sv" "$SP/axi_0397/src/axi_test.sv" sim/dv/axi-0.39.7/src/
cp "$SP/axi_0397/include/axi/typedef.svh" "$SP/axi_0397/include/axi/assign.svh" sim/dv/axi-0.39.7/include/axi/
cp "$SP/axi_0397/LICENSE" sim/dv/axi-0.39.7/
cp "$SP/cv_025/src/rand_id_queue.sv" sim/dv/common_verification-0.2.5/src/
cp "$SP/cv_025/LICENSE" sim/dv/common_verification-0.2.5/ 2>/dev/null || cp "$SP/cv_025/LICENSE.md" sim/dv/common_verification-0.2.5/
cp "$SP/floonoc/hw/test/axi_reorder_compare.sv" "$SP/floonoc/hw/test/axi_bw_monitor.sv" sim/dv/floonoc-test/
cp "$SP/floonoc/LICENSE" sim/dv/floonoc-test/ 2>/dev/null || cp "$SP/floonoc/LICENSE.md" sim/dv/floonoc-test/
git -C "$SP/floonoc" rev-parse HEAD   # record in README.md
grep -n '`include' "$SP/axi_0397/include/axi/typedef.svh" "$SP/axi_0397/include/axi/assign.svh"
# if either pulls more svh files, copy those too
```

- [ ] **Step 2: write `sim/dv/README.md`**

```markdown
# sim/dv — imported DV IP (verbatim, do not edit)

| package | upstream | rev | files | license |
|---------|----------|-----|-------|---------|
| axi-0.39.7 | github.com/pulp-platform/axi | v0.39.7 | src/axi_pkg.sv src/axi_intf.sv src/axi_test.sv include/axi/*.svh | Solderpad 0.51 |
| common_verification-0.2.5 | github.com/pulp-platform/common_verification | v0.2.5 | src/rand_id_queue.sv | Solderpad 0.51 |
| floonoc-test | github.com/pulp-platform/FlooNoC | <rev from step 1> | hw/test/axi_reorder_compare.sv hw/test/axi_bw_monitor.sv | Solderpad 0.51 |
```

- [ ] **Step 3: Windows compile gate (probe top, no solver needed to compile)**

Write `sim/tb/dv_compile_probe.sv` (temporary, deleted in Task 6 when the real endpoint consumes the VIP):

```systemverilog
// Temporary compile gate for sim/dv import; superseded by user_node_endpoint.
module dv_compile_probe;
    AXI_BUS_DV #(.AXI_ADDR_WIDTH(32), .AXI_DATA_WIDTH(64),
                 .AXI_ID_WIDTH(4), .AXI_USER_WIDTH(1)) dv (clk);
    logic clk = 0;
    typedef axi_test::axi_rand_master #(.AW(32), .DW(64), .IW(4), .UW(1),
        .TA(2ns), .TT(8ns)) rand_mst_t;
    typedef axi_test::axi_rand_slave #(.AW(32), .DW(64), .IW(4), .UW(1),
        .TA(2ns), .TT(8ns), .MAPPED(1)) rand_slv_t;
    initial begin $display("DV_COMPILE_OK"); $finish; end
endmodule
```

```bash
cd E:/05_NoC/noc_project
verilator --binary --timing -Wno-fatal -j 0 --Mdir build/verilator/obj_dv_probe \
    --top-module dv_compile_probe \
    +incdir+sim/dv/axi-0.39.7/include \
    sim/dv/common_verification-0.2.5/src/rand_id_queue.sv \
    sim/dv/axi-0.39.7/src/axi_pkg.sv sim/dv/axi-0.39.7/src/axi_intf.sv \
    sim/dv/axi-0.39.7/src/axi_test.sv sim/tb/dv_compile_probe.sv
build/verilator/obj_dv_probe/Vdv_compile_probe
```

Expected: build rc=0, run prints `DV_COMPILE_OK`.

- [ ] **Step 4: Commit**

```bash
git add sim/dv sim/tb/dv_compile_probe.sv
git commit -m "build(dv): import pulp axi 0.39.7 + common_verification 0.2.5 + floonoc test components"
```

---

### Task 5: WSL co-sim bringup (existing endpoint, platform variable isolated)

**Files:**
- Modify: `docs/development.md` (WSL prerequisites section)
- No source changes expected; `local.mk` knobs only if needed (NOT committed — gitignored)

**Interfaces:**
- Produces: proven WSL build/run flow that Task 6 uses for the VIP smoke. Any WSL-specific build break found here is fixed here (report BLOCKED if it needs source changes beyond path/toolchain).

- [ ] **Step 1: WSL toolchain check**

```bash
wsl.exe -d Ubuntu -u root -- bash -c 'export PATH=/usr/bin:/bin:/usr/local/bin; \
  verilator --version && z3 --version && g++ --version | head -1 && \
  python3 -c "import yaml; print(\"pyyaml ok\")" && cmake --version | head -1'
```

If cmake/g++/pyyaml missing: `apt-get install -y build-essential cmake python3-yaml`.

- [ ] **Step 2: build c_model + verilator co-sim inside WSL**

```bash
wsl.exe -d Ubuntu -u root -- bash -c 'export PATH=/usr/bin:/bin:/usr/local/bin; \
  cd /mnt/e/05_NoC/noc_project && \
  make build-cmodel PYTHON3=python3 && \
  make build-verilator TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3'
```

Caveat: `build/` currently holds Windows artifacts. If CMake cache clashes, use a WSL-side build root: `BUILD_ROOT` is derived in build_config.mk from PROJ_ROOT — override via env `make ... BUILD_ROOT=/root/noc_build` if needed and note it in development.md. Expected: both builds rc=0.

- [ ] **Step 3: run existing scenario flow on WSL**

```bash
wsl.exe -d Ubuntu -u root -- bash -c 'export PATH=/usr/bin:/bin:/usr/local/bin; \
  cd /mnt/e/05_NoC/noc_project/sim/verilator && \
  make run-tb-top TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3'
```

Expected: `PASS: scenario complete, scoreboard clean`.

- [ ] **Step 4: document in docs/development.md**

Add a "WSL (random-run platform)" subsection: prerequisites (Verilator 5.048 self-built `--with-solver="z3 --in"`, z3, build-essential, cmake, python3-yaml), PATH note, BUILD_ROOT override if used, the three commands above.

- [ ] **Step 5: Commit**

```bash
git add docs/development.md && git commit -m "docs(dev): WSL co-sim build and run flow"
```

---

### Task 6: VIP cutover (atomic) — types pkg + endpoint + generator + DPI excision

Single commit: old endpoint flow out, VIP data-integrity flow in. Every sub-piece below lands together because the tb imports and the DPI surface interlock.

**Files:**
- Create: `sim/tb/axi_vip_types_pkg.sv`
- Rewrite: `sim/tb/user_node_endpoint.sv`
- Modify: `sim/tools/gen_tb_top.py` (tb emitter only; fabric emitter untouched)
- Modify: `src/dpi/cmodel_dpi.cpp` (excise master/slave/scoreboard), `src/dpi/cmodel_dpi.h` (matching decls)
- Delete: `sim/tb/axi_master_wrap.sv`, `sim/tb/axi_slave_wrap.sv`, `sim/tb/axi_perf_monitor.sv`, `sim/tb/dv_compile_probe.sv`, `src/c_model/include/wrap/master_wrap.hpp`, `src/c_model/include/wrap/slave_wrap.hpp`, `src/c_model/tests/wrap/test_master_wrap.cpp`, `src/c_model/tests/wrap/test_slave_wrap.cpp`
- Modify: `sim/build_config.mk` (TB_TOP_SV_SRC: drop deleted files, add dv + types pkg), `src/c_model/tests/wrap/CMakeLists.txt` (drop deleted tests; if the dir empties, remove it and its add_subdirectory)

**Interfaces:**
- Consumes: `sim/dv/` file set (Task 4), WSL flow (Task 5).
- Produces:
  - `user_node_endpoint` ports: `clk_i, rst_ni` (in), `master_axi_req_o` (out flat), `master_axi_rsp_i` (in flat), `slave_axi_req_i` (in flat), `slave_axi_rsp_o` (out flat), `end_of_sim_o` (out logic), `txn_cnt_o` (out int unsigned). Params: `NODE_ID`, `NUM_NODES`, `DEFAULT_NUM_READS`/`DEFAULT_NUM_WRITES` (plusarg-overridden), `REGION_BASE` (unpacked `logic [63:0] [NUM_NODES]` array param stamped by generator), `REGION_BYTES`.
  - `axi_vip_types_pkg`: `vip_req_t`, `vip_resp_t` (pulp nested structs; `AXI_TYPEDEF_ALL` emits `_resp_`, not `_rsp_`) + `vip_req_from_flat()`, `vip_rsp_from_flat()` functions.
  - DPI surface KEPT: `cmodel_init` (no-arg — signature change), `cmodel_finalize`, `cmodel_check_error`, `cmodel_router_create`, `cmodel_nmu_create[_ex]`, `cmodel_nsu_create`, all nmu/nsu/router cycle ops, `cmodel_perf_*`.
  - DPI surface REMOVED: `cmodel_master_create`, `cmodel_slave_create`, all `cmodel_master_*`/`cmodel_slave_*` cycle ops, `cmodel_done`, `cmodel_scoreboard_clean`, `cmodel_dump_scoreboard`, `cmodel_master_count`, `cmodel_reads_checked`, `g_scoreboard`, `g_scenario`, `g_ever_created_master`.

- [ ] **Step 1: `sim/tb/axi_vip_types_pkg.sv`**

```systemverilog
// Per-config pulp AXI struct typedefs + flat(ni_signals_pkg) -> pulp-struct
// mapping. Monitor taps derive BOTH stream sides from the flat structs so the
// (out-of-scope) user fields compare as constant 0 on both ends.
`include "axi/typedef.svh"

`ifndef AXI_VIP_TYPES_PKG_SV
`define AXI_VIP_TYPES_PKG_SV

package axi_vip_types_pkg;

    localparam int unsigned VIP_AW = ni_params_pkg::AXI_ADDR_WIDTH_DFLT;
    localparam int unsigned VIP_DW = ni_params_pkg::AXI_DATA_WIDTH_DFLT;
    localparam int unsigned VIP_IW = ni_params_pkg::AXI_ID_WIDTH_DFLT;
    localparam int unsigned VIP_UW = 1;  // pulp minimum; flat struct has no user

    typedef logic [VIP_AW-1:0]   vip_addr_t;
    typedef logic [VIP_DW-1:0]   vip_data_t;
    typedef logic [VIP_DW/8-1:0] vip_strb_t;
    typedef logic [VIP_IW-1:0]   vip_id_t;
    typedef logic [VIP_UW-1:0]   vip_user_t;

    `AXI_TYPEDEF_ALL(vip, vip_addr_t, vip_id_t, vip_data_t, vip_strb_t, vip_user_t)

    function automatic vip_req_t vip_req_from_flat(input ni_signals_pkg::axi_req_t f);
        vip_req_t r;
        r = '0;
        r.aw.id     = f.awid;     r.aw.addr  = f.awaddr;  r.aw.len   = f.awlen;
        r.aw.size   = f.awsize;   r.aw.burst = f.awburst; r.aw.lock  = f.awlock;
        r.aw.cache  = f.awcache;  r.aw.prot  = f.awprot;  r.aw.qos   = f.awqos;
        r.aw.region = f.awregion; r.aw_valid = f.awvalid;
        r.w.data    = f.wdata;    r.w.strb   = f.wstrb;   r.w.last   = f.wlast;
        r.w_valid   = f.wvalid;   r.b_ready  = f.bready;
        r.ar.id     = f.arid;     r.ar.addr  = f.araddr;  r.ar.len   = f.arlen;
        r.ar.size   = f.arsize;   r.ar.burst = f.arburst; r.ar.lock  = f.arlock;
        r.ar.cache  = f.arcache;  r.ar.prot  = f.arprot;  r.ar.qos   = f.arqos;
        r.ar.region = f.arregion; r.ar_valid = f.arvalid; r.r_ready  = f.rready;
        return r;
    endfunction

    function automatic vip_resp_t vip_rsp_from_flat(input ni_signals_pkg::axi_rsp_t f);
        vip_resp_t r;
        r = '0;
        r.aw_ready = f.awready; r.w_ready = f.wready;
        r.b.id     = f.bid;     r.b.resp  = f.bresp;  r.b_valid = f.bvalid;
        r.ar_ready = f.arready;
        r.r.id     = f.rid;     r.r.data  = f.rdata;  r.r.resp  = f.rresp;
        r.r.last   = f.rlast;   r.r_valid = f.rvalid;
        return r;
    endfunction

endpackage

`endif  // AXI_VIP_TYPES_PKG_SV
```

Verify against `sim/dv/axi-0.39.7/include/axi/typedef.svh`: `AXI_TYPEDEF_ALL(vip, ...)` emits `vip_req_t`/`vip_resp_t` with exactly the member names used above (`aw`, `aw_valid`, `w`, `w_valid`, `b_ready`, `ar`, `ar_valid`, `r_ready` / `aw_ready`, `w_ready`, `b`, `b_valid`, `ar_ready`, `r`, `r_valid`). Adjust field names to the macro's actual output if they differ — the macro is the ground truth.

- [ ] **Step 2: rewrite `sim/tb/user_node_endpoint.sv`**

```systemverilog
// user_node_endpoint — per-node test endpoint: pulp axi_rand_master +
// axi_rand_slave + axi_scoreboard (data-integrity) / monitor taps (transport),
// FlooNoC axi_bw_monitor. Bridges the fabric's flat ni_signals_pkg structs to
// pulp AXI_BUS_DV interfaces with explicit per-field wiring (no protocol logic).
//
// Run flavors (compile-time):
//   default            : data-integrity — MAPPED slave + axi_scoreboard,
//                        INCR/FIXED bursts, disjoint write regions.
//   +define+TB_TRANSPORT_RUN : transport — MAPPED=0 RAND_RESP=1, WRAP+EXC on,
//                        no scoreboard (checking via tb-level axi_reorder_compare).
//
// Plusargs: +num_reads=<n> +num_writes=<n> (per node, defaults below).

`include "axi/assign.svh"

`ifndef USER_NODE_ENDPOINT_SV
`define USER_NODE_ENDPOINT_SV

module user_node_endpoint #(
    parameter int unsigned NODE_ID      = 0,
    parameter int unsigned NUM_NODES    = 1,
    parameter int unsigned ID_WIDTH     = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH   = ni_params_pkg::AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH   = ni_params_pkg::AXI_DATA_WIDTH_DFLT,
    // region contract (spec): region(m -> s) = REGION_BASE[s] + m * REGION_BYTES.
    // REGION_BASE[s] = coord_id(s) << 32 (dst tile in addr bits 32+), stamped by
    // gen_tb_top.py from the topology YAML.
    parameter logic [63:0] REGION_BASE [NUM_NODES] = '{default: '0},
    parameter longint unsigned REGION_BYTES = 64'h1000,
    parameter int unsigned DEFAULT_NUM_READS  = 8,
    parameter int unsigned DEFAULT_NUM_WRITES = 8,
    parameter int unsigned MAX_TXNS_IN_FLIGHT = 8
) (
    input  logic                       clk_i,
    input  logic                       rst_ni,
    output ni_signals_pkg::axi_req_t   master_axi_req_o,
    input  ni_signals_pkg::axi_rsp_t   master_axi_rsp_i,
    input  ni_signals_pkg::axi_req_t   slave_axi_req_i,
    output ni_signals_pkg::axi_rsp_t   slave_axi_rsp_o,
    output logic                       end_of_sim_o,
    output int unsigned                txn_cnt_o
);

    localparam time ApplTime = 2ns;   // FlooNoC values; clk is 10 ns
    localparam time TestTime = 8ns;

    // ------------------------------------------------------------------
    // DV interfaces + flat-struct bridging (explicit wiring, both faces)
    // ------------------------------------------------------------------
    AXI_BUS_DV #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH), .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(ID_WIDTH),     .AXI_USER_WIDTH(1)
    ) master_dv (clk_i);

    AXI_BUS_DV #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH), .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(ID_WIDTH),     .AXI_USER_WIDTH(1)
    ) slave_dv (clk_i);

    // master face: rand_master drives master_dv; forward to the flat NMU port.
    always_comb begin
        master_axi_req_o = '0;
        master_axi_req_o.awid     = master_dv.aw_id;
        master_axi_req_o.awaddr   = master_dv.aw_addr;
        master_axi_req_o.awlen    = master_dv.aw_len;
        master_axi_req_o.awsize   = master_dv.aw_size;
        master_axi_req_o.awburst  = master_dv.aw_burst;
        master_axi_req_o.awlock   = master_dv.aw_lock;
        master_axi_req_o.awcache  = master_dv.aw_cache;
        master_axi_req_o.awprot   = master_dv.aw_prot;
        master_axi_req_o.awqos    = master_dv.aw_qos;
        master_axi_req_o.awregion = master_dv.aw_region;
        master_axi_req_o.awvalid  = master_dv.aw_valid;
        master_axi_req_o.wdata    = master_dv.w_data;
        master_axi_req_o.wstrb    = master_dv.w_strb;
        master_axi_req_o.wlast    = master_dv.w_last;
        master_axi_req_o.wvalid   = master_dv.w_valid;
        master_axi_req_o.bready   = master_dv.b_ready;
        master_axi_req_o.arid     = master_dv.ar_id;
        master_axi_req_o.araddr   = master_dv.ar_addr;
        master_axi_req_o.arlen    = master_dv.ar_len;
        master_axi_req_o.arsize   = master_dv.ar_size;
        master_axi_req_o.arburst  = master_dv.ar_burst;
        master_axi_req_o.arlock   = master_dv.ar_lock;
        master_axi_req_o.arcache  = master_dv.ar_cache;
        master_axi_req_o.arprot   = master_dv.ar_prot;
        master_axi_req_o.arqos    = master_dv.ar_qos;
        master_axi_req_o.arregion = master_dv.ar_region;
        master_axi_req_o.arvalid  = master_dv.ar_valid;
        master_axi_req_o.rready   = master_dv.r_ready;
    end
    assign master_dv.aw_ready = master_axi_rsp_i.awready;
    assign master_dv.w_ready  = master_axi_rsp_i.wready;
    assign master_dv.b_id     = master_axi_rsp_i.bid;
    assign master_dv.b_resp   = master_axi_rsp_i.bresp;
    assign master_dv.b_user   = '0;
    assign master_dv.b_valid  = master_axi_rsp_i.bvalid;
    assign master_dv.ar_ready = master_axi_rsp_i.arready;
    assign master_dv.r_id     = master_axi_rsp_i.rid;
    assign master_dv.r_data   = master_axi_rsp_i.rdata;
    assign master_dv.r_resp   = master_axi_rsp_i.rresp;
    assign master_dv.r_last   = master_axi_rsp_i.rlast;
    assign master_dv.r_user   = '0;
    assign master_dv.r_valid  = master_axi_rsp_i.rvalid;
    // aw_atop / *_user driven by the class are dropped (out of scope).

    // slave face: forward the flat NSU port into slave_dv; rand_slave responds.
    assign slave_dv.aw_id     = slave_axi_req_i.awid;
    assign slave_dv.aw_addr   = slave_axi_req_i.awaddr;
    assign slave_dv.aw_len    = slave_axi_req_i.awlen;
    assign slave_dv.aw_size   = slave_axi_req_i.awsize;
    assign slave_dv.aw_burst  = slave_axi_req_i.awburst;
    assign slave_dv.aw_lock   = slave_axi_req_i.awlock;
    assign slave_dv.aw_cache  = slave_axi_req_i.awcache;
    assign slave_dv.aw_prot   = slave_axi_req_i.awprot;
    assign slave_dv.aw_qos    = slave_axi_req_i.awqos;
    assign slave_dv.aw_region = slave_axi_req_i.awregion;
    assign slave_dv.aw_atop   = '0;
    assign slave_dv.aw_user   = '0;
    assign slave_dv.aw_valid  = slave_axi_req_i.awvalid;
    assign slave_dv.w_data    = slave_axi_req_i.wdata;
    assign slave_dv.w_strb    = slave_axi_req_i.wstrb;
    assign slave_dv.w_last    = slave_axi_req_i.wlast;
    assign slave_dv.w_user    = '0;
    assign slave_dv.w_valid   = slave_axi_req_i.wvalid;
    assign slave_dv.b_ready   = slave_axi_req_i.bready;
    assign slave_dv.ar_id     = slave_axi_req_i.arid;
    assign slave_dv.ar_addr   = slave_axi_req_i.araddr;
    assign slave_dv.ar_len    = slave_axi_req_i.arlen;
    assign slave_dv.ar_size   = slave_axi_req_i.arsize;
    assign slave_dv.ar_burst  = slave_axi_req_i.arburst;
    assign slave_dv.ar_lock   = slave_axi_req_i.arlock;
    assign slave_dv.ar_cache  = slave_axi_req_i.arcache;
    assign slave_dv.ar_prot   = slave_axi_req_i.arprot;
    assign slave_dv.ar_qos    = slave_axi_req_i.arqos;
    assign slave_dv.ar_region = slave_axi_req_i.arregion;
    assign slave_dv.ar_user   = '0;
    assign slave_dv.ar_valid  = slave_axi_req_i.arvalid;
    assign slave_dv.r_ready   = slave_axi_req_i.rready;
    always_comb begin
        slave_axi_rsp_o = '0;
        slave_axi_rsp_o.awready = slave_dv.aw_ready;
        slave_axi_rsp_o.wready  = slave_dv.w_ready;
        slave_axi_rsp_o.bid     = slave_dv.b_id;
        slave_axi_rsp_o.bresp   = slave_dv.b_resp;
        slave_axi_rsp_o.bvalid  = slave_dv.b_valid;
        slave_axi_rsp_o.arready = slave_dv.ar_ready;
        slave_axi_rsp_o.rid     = slave_dv.r_id;
        slave_axi_rsp_o.rdata   = slave_dv.r_data;
        slave_axi_rsp_o.rresp   = slave_dv.r_resp;
        slave_axi_rsp_o.rlast   = slave_dv.r_last;
        slave_axi_rsp_o.rvalid  = slave_dv.r_valid;
    end

    // ------------------------------------------------------------------
    // VIP classes
    // ------------------------------------------------------------------
`ifdef TB_TRANSPORT_RUN
    localparam bit RunMapped = 1'b0;
    typedef axi_test::axi_rand_master #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(1),
        .TA(ApplTime), .TT(TestTime),
        .MAX_READ_TXNS(MAX_TXNS_IN_FLIGHT), .MAX_WRITE_TXNS(MAX_TXNS_IN_FLIGHT),
        .AXI_EXCLS(1'b1), .AXI_ATOPS(1'b0), .UNIQUE_IDS(1'b0),
        .AXI_BURST_FIXED(1'b1), .AXI_BURST_INCR(1'b1), .AXI_BURST_WRAP(1'b1)
    ) rand_master_t;
    typedef axi_test::axi_rand_slave #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(1),
        .TA(ApplTime), .TT(TestTime), .MAPPED(1'b0), .RAND_RESP(1'b1)
    ) rand_slave_t;
`else
    localparam bit RunMapped = 1'b1;
    typedef axi_test::axi_rand_master #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(1),
        .TA(ApplTime), .TT(TestTime),
        .MAX_READ_TXNS(MAX_TXNS_IN_FLIGHT), .MAX_WRITE_TXNS(MAX_TXNS_IN_FLIGHT),
        .AXI_EXCLS(1'b0), .AXI_ATOPS(1'b0), .UNIQUE_IDS(1'b0),
        .AXI_BURST_FIXED(1'b1), .AXI_BURST_INCR(1'b1), .AXI_BURST_WRAP(1'b0)
    ) rand_master_t;
    typedef axi_test::axi_rand_slave #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(1),
        .TA(ApplTime), .TT(TestTime), .MAPPED(1'b1)
    ) rand_slave_t;
`endif

    rand_master_t rand_master;
    rand_slave_t  rand_slave;

    int unsigned num_reads;
    int unsigned num_writes;

    initial begin
        num_reads  = DEFAULT_NUM_READS;
        num_writes = DEFAULT_NUM_WRITES;
        void'($value$plusargs("num_reads=%d", num_reads));
        void'($value$plusargs("num_writes=%d", num_writes));

        rand_master = new(master_dv);
        end_of_sim_o = 1'b0;
`ifdef TB_TRANSPORT_RUN
        // transport: permutation pairing — this master targets ONLY node
        // (NUM_NODES-1-NODE_ID) so tb-level axi_reorder_compare can attribute
        // streams (see spec "transport run 限 permutation pairing").
        rand_master.add_memory_region(
            REGION_BASE[NUM_NODES-1-NODE_ID],
            REGION_BASE[NUM_NODES-1-NODE_ID] + REGION_BYTES,
            axi_pkg::DEVICE_NONBUFFERABLE);
`else
        // data-integrity: all-to-all, per-master disjoint window inside each
        // destination node's space: REGION_BASE[s] + NODE_ID*REGION_BYTES.
        for (int s = 0; s < NUM_NODES; s++) begin
            rand_master.add_memory_region(
                REGION_BASE[s] + longint'(NODE_ID) * REGION_BYTES,
                REGION_BASE[s] + longint'(NODE_ID) * REGION_BYTES + REGION_BYTES,
                axi_pkg::DEVICE_NONBUFFERABLE);
        end
`endif
        rand_master.reset();
        @(posedge rst_ni);
        rand_master.run(num_reads, num_writes);
        end_of_sim_o = 1'b1;
    end

    initial begin
        rand_slave = new(slave_dv);
        rand_slave.reset();
        @(posedge rst_ni);
        rand_slave.run();
    end

`ifndef TB_TRANSPORT_RUN
    // ------------------------------------------------------------------
    // pulp scoreboard — passive on the master face (disjoint regions make
    // per-master byte-model checking sound).
    // ------------------------------------------------------------------
    typedef axi_test::axi_scoreboard #(
        .IW(ID_WIDTH), .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .UW(1), .TT(TestTime)
    ) scoreboard_t;
    scoreboard_t scoreboard;
    initial begin
        scoreboard = new(master_dv);
        scoreboard.reset();
        @(posedge rst_ni);
        scoreboard.enable_all_checks();
        scoreboard.monitor();
    end
`endif

    // ------------------------------------------------------------------
    // FlooNoC bw monitor (endpoint perf; $display at end_of_sim) +
    // non-vacuous handshake counter.
    // ------------------------------------------------------------------
    axi_vip_types_pkg::vip_req_t  mon_mst_req;
    axi_vip_types_pkg::vip_resp_t mon_mst_rsp;
    assign mon_mst_req = axi_vip_types_pkg::vip_req_from_flat(master_axi_req_o);
    assign mon_mst_rsp = axi_vip_types_pkg::vip_rsp_from_flat(master_axi_rsp_i);

    axi_bw_monitor #(
        .req_t(axi_vip_types_pkg::vip_req_t),
        .rsp_t(axi_vip_types_pkg::vip_resp_t),
        .AxiIdWidth(ID_WIDTH),
        .Name($sformatf("node%0d.manager", NODE_ID))
    ) u_bw_mst (
        .clk_i(clk_i), .en_i(rst_ni), .end_of_sim_i(end_of_sim_o),
        .req_i(mon_mst_req), .rsp_i(mon_mst_rsp),
        .ar_in_flight_o(), .aw_in_flight_o()
    );

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) txn_cnt_o <= 0;
        else begin
            if (master_axi_req_o.awvalid && master_axi_rsp_i.awready) txn_cnt_o <= txn_cnt_o + 1;
            if (master_axi_req_o.arvalid && master_axi_rsp_i.arready) txn_cnt_o <= txn_cnt_o + 1;
        end
    end

endmodule

`endif  // USER_NODE_ENDPOINT_SV
```

Field-name ground truth is `sim/dv/axi-0.39.7/src/axi_intf.sv` (`AXI_BUS_DV` signal list) — verify each `master_dv.*`/`slave_dv.*` name against it before compiling; fix wiring, never the interface.

Scoreboard never-written-read check: before Step 8, read the `handle_read` task in `sim/dv/axi-0.39.7/src/axi_test.sv` and confirm what `axi_scoreboard` does when R data arrives for a byte its model never saw written (expected: it skips or learns unknown bytes; the MAPPED rand_slave randomly initializes memory on first read, so read-before-write IS a legal random sequence here). If the scoreboard instead flags unknown-byte reads as errors, constrain it: call `enable_b_resp_check()`/`enable_r_resp_check()` + `enable_read_check()` only if tolerant, else report BLOCKED with the source excerpt — do not tweak dv sources.

- [ ] **Step 3: gen_tb_top.py tb emitter rewrite**

In `emit_tb_top()` only (fabric emitter untouched). Replace the old blocks as follows.

(a) DPI import block: KEEP `cmodel_init` (now no-arg), `cmodel_finalize`, `cmodel_router_create`, `cmodel_nmu_create[_ex]`, `cmodel_nsu_create`, perf imports, `cmodel_check_error`. DELETE imports of `cmodel_done`, `cmodel_scoreboard_clean`, `cmodel_dump_scoreboard`, `cmodel_master_create`, `cmodel_slave_create`, `cmodel_master_count`, `cmodel_reads_checked`.

(b) Scenario plusargs + `m_ctx`/`s_ctx`: DELETE entirely. The create initial becomes:

```systemverilog
    initial begin
        cmodel_init();
        // router/nmu/nsu creates unchanged (per node, as before)
        ...
    end
```

(c) Watchdog (replaces fixed TIMEOUT_CYCLES initial):

```systemverilog
    localparam int unsigned TIMEOUT_BASE = 100000;
    localparam int unsigned TIMEOUT_K    = 200;
    int unsigned tb_num_reads  = 8;   // mirror endpoint defaults
    int unsigned tb_num_writes = 8;
    initial begin
        int unsigned timeout_cycles;
        void'($value$plusargs("num_reads=%d",  tb_num_reads));
        void'($value$plusargs("num_writes=%d", tb_num_writes));
        timeout_cycles = TIMEOUT_BASE
            + TIMEOUT_K * (tb_num_reads + tb_num_writes) * NUM_NODES;
        repeat (timeout_cycles) @(posedge clk_i);
        $fatal(1, "tb_top: timeout after %0d cycles", timeout_cycles);
    end
```

(NUM_NODES = the generator's `n`, emitted as a localparam.)

(d) Endpoint generate: new ports/params. Generator computes `REGION_BASE` from coord ids:

```python
region_base = ", ".join(f"64'h{(c << 32):016X}" for (_i, _x, _y, c) in nodes)
```

```systemverilog
    logic        end_of_sim [NUM_NODES];
    int unsigned txn_cnt    [NUM_NODES];
    for (genvar i = 0; i < NUM_NODES; i++) begin : g_endpoint
        user_node_endpoint #(
            .NODE_ID(i), .NUM_NODES(NUM_NODES),
            .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
            .REGION_BASE('{<region_base list>})
        ) u_endpoint (
            .clk_i(clk_i), .rst_ni(rst_ni),
            .master_axi_req_o(master_axi_req[i]), .master_axi_rsp_i(master_axi_rsp[i]),
            .slave_axi_req_i(slave_axi_req[i]),   .slave_axi_rsp_o(slave_axi_rsp[i]),
            .end_of_sim_o(end_of_sim[i]), .txn_cnt_o(txn_cnt[i])
        );
    end : g_endpoint
```

(e) Exit logic (replaces the cmodel_done block):

```systemverilog
    localparam int unsigned SETTLE_CYCLES = 100;
    initial begin
        bit vacuous;
        wait (rst_ni);
        for (int i = 0; i < NUM_NODES; i++) wait (end_of_sim[i]);
        repeat (SETTLE_CYCLES) @(posedge clk_i);
        vacuous = 1'b0;
        for (int i = 0; i < NUM_NODES; i++) begin
            if (txn_cnt[i] == 0) begin
                vacuous = 1'b1;
                $display("FAIL: node%0d completed zero transactions (vacuous)", i);
            end
        end
        if (vacuous) $fatal(1, "tb_top: vacuous run");
        $display("PASS: all %0d nodes done, non-vacuous", NUM_NODES);
        $finish(0);
    end
```

(f) Keep unchanged: clk/rst block, perf instrumentation block, FSDB block, DPI error poll block, fabric instance. Delete the `MASTER_SLOT_NAME`/`SLAVE_SLOT_NAME` endpoint params (bw_monitor takes Name internally).

(g) Docstring/header comments: update to describe the VIP endpoint flow (no scenario plusargs; `+num_reads/+num_writes`, seed via `+verilator+seed+<N>`).

- [ ] **Step 4: DPI excision in `src/dpi/cmodel_dpi.cpp` / `.h`**

Delete (function granularity — grep each name for its full body):
`cmodel_master_create`, `cmodel_slave_create`, every `cmodel_master_*` / `cmodel_slave_*` cycle op (set_inputs/tick/get_outputs and friends between the create functions), `cmodel_done`, `cmodel_scoreboard_clean`, `cmodel_dump_scoreboard`, `cmodel_master_count`, `cmodel_reads_checked`; globals `g_scoreboard`, `g_scenario`, `g_scenario_yaml_path`, `g_ever_created_master`; includes that become unused (`scenario_parser.hpp`, `scoreboard.hpp`, `master_wrap.hpp`, `slave_wrap.hpp` — verify with a compile).
`cmodel_init` signature: `void cmodel_init(const char* scenario_yaml_path)` → `void cmodel_init(void)`; body keeps the session-state machine + `g_perf` reset, drops scenario load + scoreboard construction. Update `cmodel_dpi.h` decls to match.
Run `clang-format -i src/dpi/cmodel_dpi.cpp src/dpi/cmodel_dpi.h`.
Check `sim/verilator/main.cpp` for references to removed symbols (`grep -n "cmodel_" sim/verilator/main.cpp`) — if it polls `cmodel_done`, switch it to plain `$finish`-driven exit (Verilator `--timing` main loop runs until `Verilated::gotFinish()`; that is already the standard pattern).

- [ ] **Step 5: delete retired files + build lists**

```bash
git rm sim/tb/axi_master_wrap.sv sim/tb/axi_slave_wrap.sv sim/tb/axi_perf_monitor.sv \
       sim/tb/dv_compile_probe.sv \
       src/c_model/include/wrap/master_wrap.hpp src/c_model/include/wrap/slave_wrap.hpp \
       src/c_model/tests/wrap/test_master_wrap.cpp src/c_model/tests/wrap/test_slave_wrap.cpp
```

`sim/build_config.mk` `TB_TOP_SV_SRC`: drop the three deleted .sv; ADD (order matters — packages before consumers, dv before tb):

```makefile
DV_ROOT := $(COSIM_ROOT)/dv
TB_TOP_SV_SRC := \
    $(SPECGEN_SV_INC)/ni_params_pkg.sv \
    $(SPECGEN_SV_INC)/ni_signals_pkg.sv \
    $(TOPOLOGY_NOC_TYPES_PKG) \
    $(SPECGEN_SV_INC)/ni_flit_pkg.sv \
    $(DV_ROOT)/common_verification-0.2.5/src/rand_id_queue.sv \
    $(DV_ROOT)/axi-0.39.7/src/axi_pkg.sv \
    $(DV_ROOT)/axi-0.39.7/src/axi_intf.sv \
    $(DV_ROOT)/axi-0.39.7/src/axi_test.sv \
    $(DV_ROOT)/floonoc-test/axi_bw_monitor.sv \
    $(DV_ROOT)/floonoc-test/axi_reorder_compare.sv \
    $(COSIM_ROOT)/tb/axi_vip_types_pkg.sv \
    $(SRC_SV)/nmu_wrap.sv \
    $(SRC_SV)/router_wrap.sv \
    $(SRC_SV)/nsu_wrap.sv \
    $(SRC_SV)/ni_wrap.sv \
    $(COSIM_ROOT)/tb/user_node_endpoint.sv \
    $(COSIM_ROOT)/tb/link_perf_monitor.sv \
    $(TB_TOP_SV)
```

Add `+incdir` for the svh: append `$(DV_ROOT)/axi-0.39.7/include` to `FILELIST_GEN_ARGS` incdirs and `-I$(DV_ROOT)/axi-0.39.7/include` to both simulator Makefiles.
`src/c_model/tests/wrap/CMakeLists.txt`: remove the two deleted test targets; if nothing remains, delete the file + its `add_subdirectory(wrap)` in the parent.
`sim/verilator/Makefile`: delete the `master_wrap_read_dump` mv/clean lines (dump feature retired with the C++ master), and REWRITE the `run-tb-top` recipe — the scenario flow (`gen_test_patterns.py` call + `+scenario_node*` args) is dead. New recipe:

```makefile
NUM_READS  ?= 8
NUM_WRITES ?= 8
SEED       ?= 1
RUN_TAG    ?= vip_$(TOPOLOGY)_s$(SEED)

.PHONY: run-tb-top
run-tb-top: $(TBTOP_EXE)
	@mkdir -p output/$(RUN_TAG)
	@echo "running $(RUN_TAG) (reads=$(NUM_READS) writes=$(NUM_WRITES) seed=$(SEED))"
	$(TBTOP_EXE) \
	    "+num_reads=$(NUM_READS)" "+num_writes=$(NUM_WRITES)" \
	    "+verilator+seed+$(SEED)" \
	    "+perf_out=output/$(RUN_TAG)/perf.json" \
	    "+perf_scenario=$(RUN_TAG)" \
	    > output/$(RUN_TAG)/run.log 2>&1; \
	rc=$$?; \
	echo "--- run.log (tail) ---"; \
	tail -8 output/$(RUN_TAG)/run.log; \
	$(PYTHON3) perf_cli_summary.py output/$(RUN_TAG)/perf.json || true; \
	exit $$rc
```

Also delete the now-unused `SCENARIO`/`SCENARIO_ABS`/`GEN_TEST_PATTERNS` variables in this Makefile, and mirror the recipe change in `sim/vcs/Makefile` (its seed plusarg is `+ntb_random_seed=$(SEED)`). `sim/tools/gen_test_patterns.py` itself stays (25 patterns remain on disk; converter backlog).

- [ ] **Step 6: regenerate + ctest green (Windows)**

```bash
cd E:/05_NoC/noc_project
rm -rf build/cmodel && make build-cmodel PYTHON3=python3
cd build/cmodel && ctest --output-on-failure -j 8
```

Expected: zero failures (count drops by the two wrap tests).

- [ ] **Step 7: co-sim compile gate (Windows — compiles, must NOT run randomize)**

```bash
cd E:/05_NoC/noc_project && make build-verilator TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3
```

Expected: rc=0. (Running it on Windows would fail at first `std::randomize` — solver unavailable; that is expected and not a gate.)

- [ ] **Step 8: WSL smoke run (data-integrity flavor, through run-tb-top)**

```bash
wsl.exe -d Ubuntu -u root -- bash -c 'export PATH=/usr/bin:/bin:/usr/local/bin; \
  export VERILATOR_SOLVER="z3 --in"; \
  cd /mnt/e/05_NoC/noc_project/sim/verilator && \
  make run-tb-top TOPOLOGY=mesh_4x4_vc1 NUM_READS=8 NUM_WRITES=8 SEED=1 PYTHON3=python3'
```

Expected: `[Monitor node*.manager]` bw lines, `PASS: all 16 nodes done, non-vacuous`, no `%Error`/scoreboard messages, rc=0. Debug loop if hung: reduce to `TOPOLOGY=mesh_2x4_vc1`, add `+num_reads=1 +num_writes=1`; check the TA/TT vs DPI phase first (spec 實作驗證項 4) — the DPI wraps register on posedge, the VIP applies at +2 ns and samples at +8 ns, which lines up with a 10 ns clock; a hang at the very first AW usually means a DV-interface field wiring mistake (Step 2 ground-truth check). After 3 failed hypotheses: STOP, report BLOCKED with the run.log.

- [ ] **Step 9: Commit**

```bash
git add -A && git commit -m "feat(sim): pulp AXI VIP endpoint cutover (data-integrity flavor)"
```

---

### Task 7: Transport flavor (`TB_TRANSPORT_RUN`)

**Files:**
- Modify: `sim/tools/gen_tb_top.py` (emit reorder_compare block under a generator flag), `sim/verilator/Makefile` + `sim/vcs/Makefile` (RUN_CLASS knob), `sim/build_config.mk` (obj dir per run class)

**Interfaces:**
- Consumes: endpoint `TB_TRANSPORT_RUN` branch (Task 6 already wrote it), `axi_reorder_compare` (ports: `mon_mst_req_i/rsp_i`, `mon_slv_req_i/rsp_i [NumSlaves]`, `AddrRegions`, `end_of_sim_o`).
- Produces: `make build-verilator TOPOLOGY=<t> RUN_CLASS=transport` → separate obj dir `obj_dir_<t>_transport`; tb waits on compare `end_of_sim_o` too.

- [ ] **Step 1: RUN_CLASS build knob**

`sim/verilator/Makefile`:

```makefile
RUN_CLASS ?= data_integrity
ifeq ($(RUN_CLASS),transport)
VERILATOR_FLAGS += +define+TB_TRANSPORT_RUN
endif
```

and make the obj dir per-class for BOTH classes (uniform naming — Task 8's runner depends on it): every `obj_dir_$(TOPOLOGY)` occurrence in `sim/verilator/Makefile` becomes `obj_dir_$(TOPOLOGY)_$(RUN_CLASS)`. There are FOUR spots: the `--Mdir` flag in `VERILATOR_FLAGS`, the `TBTOP_EXE` definition, the `mkdir -p` line and the `sed -i` + sub-`$(MAKE)` lines inside the `$(TBTOP_EXE)` recipe. Mirror the define in `sim/vcs/Makefile` (`+define+TB_TRANSPORT_RUN`). Regenerate-on-switch: extend the `TOPO_STAMP` content compare to `"$(TOPOLOGY)_$(RUN_CLASS)"`.

- [ ] **Step 2: generator — transport compare block**

`gen_tb_top.py` emits, guarded by the same SV `` `ifdef TB_TRANSPORT_RUN `` (single tb source serves both classes):

```systemverilog
`ifdef TB_TRANSPORT_RUN
    // Transport checking: permutation pairing master m -> node (N-1-m).
    // One axi_reorder_compare per master; its single monitored slave is the
    // paired node's NSU egress. Streams converted flat -> pulp struct on BOTH
    // sides so out-of-scope fields (user) compare as 0 == 0.
    axi_vip_types_pkg::vip_req_t  cmp_mst_req [NUM_NODES];
    axi_vip_types_pkg::vip_resp_t cmp_mst_rsp [NUM_NODES];
    axi_vip_types_pkg::vip_req_t  cmp_slv_req [NUM_NODES];
    axi_vip_types_pkg::vip_resp_t cmp_slv_rsp [NUM_NODES];
    logic cmp_end_of_sim [NUM_NODES];
    for (genvar i = 0; i < NUM_NODES; i++) begin : g_cmp
        localparam int unsigned PAIR = NUM_NODES - 1 - i;
        assign cmp_mst_req[i] = axi_vip_types_pkg::vip_req_from_flat(master_axi_req[i]);
        assign cmp_mst_rsp[i] = axi_vip_types_pkg::vip_rsp_from_flat(master_axi_rsp[i]);
        assign cmp_slv_req[i] = axi_vip_types_pkg::vip_req_from_flat(slave_axi_req[PAIR]);
        assign cmp_slv_rsp[i] = axi_vip_types_pkg::vip_rsp_from_flat(slave_axi_rsp[PAIR]);
        axi_reorder_compare #(
            .NumSlaves(1), .AxiIdWidth(ID_WIDTH), .NumAddrRegions(1),
            .addr_t(axi_vip_types_pkg::vip_addr_t),
            .rule_t(<rule_t from the compare source>),
            .AddrRegions(<single region covering the pair window>),
            .aw_chan_t(axi_vip_types_pkg::vip_aw_chan_t),
            .w_chan_t(axi_vip_types_pkg::vip_w_chan_t),
            .b_chan_t(axi_vip_types_pkg::vip_b_chan_t),
            .ar_chan_t(axi_vip_types_pkg::vip_ar_chan_t),
            .r_chan_t(axi_vip_types_pkg::vip_r_chan_t),
            .req_t(axi_vip_types_pkg::vip_req_t),
            .rsp_t(axi_vip_types_pkg::vip_resp_t)
        ) u_cmp (
            .clk_i(clk_i), .rst_ni(rst_ni),
            .mon_mst_req_i(cmp_mst_req[i]), .mon_mst_rsp_i(cmp_mst_rsp[i]),
            .mon_slv_req_i('{cmp_slv_req[i]}), .mon_slv_rsp_i('{cmp_slv_rsp[i]}),
            .end_of_sim_o(cmp_end_of_sim[i])
        );
    end
`endif
```

Before coding: read `sim/dv/floonoc-test/axi_reorder_compare.sv` for the exact `rule_t` shape (expected pulp addr_decode style: `{idx, start_addr, end_addr}`) and its `end_of_sim_o` semantics (queues-empty), and adjust the two `<...>` slots to the real types/literals. Extend the exit block: under `TB_TRANSPORT_RUN`, also `for (i) wait(cmp_end_of_sim[i]);` before the settle window.

- [ ] **Step 3: WSL transport run**

```bash
wsl.exe -d Ubuntu -u root -- bash -c 'export PATH=/usr/bin:/bin:/usr/local/bin; \
  cd /mnt/e/05_NoC/noc_project && \
  make build-verilator TOPOLOGY=mesh_4x4_vc1 RUN_CLASS=transport PYTHON3=python3 && \
  VERILATOR_SOLVER="z3 --in" \
  build/verilator/obj_dir_mesh_4x4_vc1_transport/Vtb_top \
    +num_reads=8 +num_writes=8 +verilator+seed+1 2>&1 | tail -20'
```

Expected: `PASS`, zero compare mismatch prints, rc=0. WRAP bursts + exclusive + random resp codes flow (verify at least one `expected/received` never fires; a mismatch print is a real finding — root-cause per "don't silence the checker", report if DUT bug).

- [ ] **Step 4: Windows compile gate for the transport build (build only), then Commit**

```bash
git add -A && git commit -m "feat(sim): transport run class with reorder-compare checking"
```

---

### Task 8: Regression rewire — matrix v2 + runner

**Files:**
- Rewrite: `sim/regress/matrix.yaml`
- Modify: `sim/regress/run_regress.py`

**Interfaces:**
- Consumes: `make build-verilator TOPOLOGY=<t> RUN_CLASS=<c>`, tb plusargs `+num_reads/+num_writes/+verilator+seed+<N>`.
- Produces: `make sim-regress` (WSL) green over the new matrix; `matrix.json` rows carry `run_class`, `seed`, `toolchain`.

- [ ] **Step 1: matrix.yaml v2**

```yaml
# Co-sim regression matrix v2 (pulp VIP endpoints). Runs on WSL/Linux
# (constrained-random needs the SAT solver; Windows compile-gates only).
# Every cell: topology x rob_mode x run_class x seed. num_reads/num_writes
# are per-node transaction counts.
topologies: [mesh_4x4_vc1, mesh_4x4_vc2, mesh_4x4_vc4, mesh_4x4_vc8]
rob_modes:  [disabled, enabled]
run_classes:
  - {name: data_integrity, num_reads: 32, num_writes: 32}
  - {name: transport,      num_reads: 32, num_writes: 32}
seeds: [1, 2, 3]
exclusions: []
```

- [ ] **Step 2: run_regress.py rewrite of Cell/expand/run_cell**

Replace the scenario machinery (`_ax4_by_address_mode`, `resolve_scenario`, `is_self_checking`, `unique_addr_count`, `_address_mode`, `CAPACITY_SLOTS`, `_interior_hotspot`, RUN_BENCH usage) with direct exe invocation:

```python
@dataclass(frozen=True)
class Cell:
    topology: str
    rob_mode: str
    run_class: str
    num_reads: int
    num_writes: int
    seed: int

    def effective_topology(self) -> str:
        return self.topology + ("_rob" if self.rob_mode == "enabled" else "")

    def label(self) -> str:
        return (f"{self.effective_topology()}__{self.run_class}__s{self.seed}")


def expand(matrix: dict) -> list:
    cells = []
    for topo in matrix["topologies"]:
        for rob in matrix["rob_modes"]:
            for rc in matrix["run_classes"]:
                for seed in matrix["seeds"]:
                    cells.append(Cell(topo, rob, rc["name"],
                                      rc["num_reads"], rc["num_writes"], seed))
    return cells


def run_cell(cell: Cell, out_dir: Path, run_cmd=None) -> bool:
    exe = (ROOT / "build" / "verilator"
           / f"obj_dir_{cell.effective_topology()}_{cell.run_class}" / "Vtb_top")
    out_dir.mkdir(parents=True, exist_ok=True)
    log = out_dir / "run.log"
    # per-simulator seed plusarg (spec seed table); this runner drives Verilator,
    # the vcs entry documents the mapping for the workstation flow.
    SEED_ARG = {"verilator": "+verilator+seed+{n}", "vcs": "+ntb_random_seed={n}"}
    args = [str(exe),
            f"+num_reads={cell.num_reads}", f"+num_writes={cell.num_writes}",
            SEED_ARG["verilator"].format(n=cell.seed),
            f"+perf_out={out_dir / 'perf.json'}",
            f"+perf_scenario={cell.label()}"]
    env = {**os.environ, "VERILATOR_SOLVER": "z3 --in"}
    with open(log, "w") as f:
        rc = subprocess.run(args, stdout=f, stderr=subprocess.STDOUT, env=env).returncode
    text = log.read_text(errors="replace")
    ok = (rc == 0 and "PASS: all" in text
          and "%Error" not in text and "mismatch" not in text.lower())
    return ok
```

Build loop: one `make build-verilator TOPOLOGY=<t> RUN_CLASS=<c> PYTHON3=python3` per (effective_topology, run_class) pair present in planned run cells. Record in each result row: `run_class`, `seed`, and `toolchain` = output of `verilator --version` captured once (Codex D1: explicit toolchain label). The obj-dir naming must match the Task 7 Makefile change — note `obj_dir_<topo>` (data_integrity default) vs `obj_dir_<topo>_transport`: unify by making the Makefile ALWAYS suffix `_$(RUN_CLASS)` (both classes), and update this runner accordingly.

- [ ] **Step 3: dry-run accounting then WSL subset**

```bash
python3 sim/regress/run_regress.py --dry-run
wsl.exe -d Ubuntu -u root -- bash -c 'export PATH=/usr/bin:/bin:/usr/local/bin; \
  cd /mnt/e/05_NoC/noc_project && python3 sim/regress/run_regress.py --build mesh_4x4_vc1'
```

Expected: dry-run prints raw=48 (4 topo × 2 rob × 2 class × 3 seeds); subset `--build mesh_4x4_vc1` selects the rob-DISABLED build only (effective_topology exact match) = 2 class × 3 seeds = 6 cells → pass=6 fail=0. Full matrix run is allowed to take long; if wall-clock is prohibitive, trim `seeds: [1]` in a follow-up commit with the trade-off noted in `docs/backlog.md` (per "class-fix record trade-off").

- [ ] **Step 4: Commit**

```bash
git add -A && git commit -m "test(regress): matrix v2 — run_class x seed cells over VIP endpoints"
```

---

### Task 9: Docs sync + backlog + plan closure

**Files:**
- Modify: `CLAUDE.md` (tree references: `c_model/include/` → `src/c_model/include/`, `c_model/tests/common/` → `src/c_model/tests/common/`, `c_model/config/` → `src/c_model/config/`, wrap-layer description — MasterWrap/SlaveWrap sentence now NMU/NSU/router only, ChannelModel path)
- Modify: `docs/architecture.md`, `docs/development.md` (path refs + endpoint description), `docs/backlog.md`, `IMPLEMENTATION_PLAN.md`
- Delete stale root-level debug dumps note: none tracked (untracked local files — leave)

**Interfaces:** none (docs only).

- [ ] **Step 1: path sweep across docs**

```bash
grep -rn "sim/sv\|sim/c/\|c_model/" CLAUDE.md docs/*.md | grep -v "src/c_model\|_archive"
```

Fix every hit to the new tree (`src/c_model/`, `src/sv/`, `src/dpi/`, `sim/tb/`, `sim/dv/`). Do not touch `docs/_archive/`.

- [ ] **Step 2: backlog entries**

Append to `docs/backlog.md` (one line each, table style consistent with the file):
- spatial traffic patterns (neighbor/transpose/hotspot) port to region-based VIP stimulus; `run_benchmark.py` + injection-rate sweep depend on it — currently stale post-cutover
- all-to-all transport-integrity checking (reorder_compare attribution limit; v1 = permutation pairing)
- named seeds for exact AXI boundary corners + seed soak sweep
- `scenario.yaml → axi_file_master` converter (25 directed patterns on disk, unused)
- PMU cross-check: compare `axi_bw_monitor` endpoint numbers vs in-fabric PMU perf.json on one run; discrepancy = bug hunt
- VCS compile gate for the VIP tb (filelist + `+define+TB_TRANSPORT_RUN` path untested on VCS)

- [ ] **Step 3: IMPLEMENTATION_PLAN.md**

Mark Stage 2 (VIP port) Complete with a one-line result; Stage 3 scope now = the regression/backlog follow-ups; keep file until user closes it out.

- [ ] **Step 4: full green sweep + Commit**

```bash
cd E:/05_NoC/noc_project && make build-cmodel PYTHON3=python3 && \
  (cd build/cmodel && ctest -j 8 --output-on-failure) && \
  make build-verilator TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3
git add -A && git commit -m "docs: sync tree paths and backlog after VIP cutover"
```

---

## Self-Review Notes

- Spec coverage: D1 (Task 5, 8 toolchain label), D2 (Task 6 knobs, Task 8 seeds; config-matrix knob AXES beyond the two run classes deferred — recorded in backlog Step 2), D3 (Task 6 scoreboard + Task 7 compare + region contract in endpoint params), D4 (Tasks 6-7 endpoint/generator scope), Scope exclusions (endpoint ties + constraints in Global Constraints), file tree (Tasks 1-4), monitors (Task 6 bw_monitor + txn counter), end-of-sim (Task 6 Step 3(c)(e), Task 7 Step 2 extension), seeds (Task 8), 元件處置 (Tasks 4, 6), 環境 (Task 5), 實作驗證項 1→Task 6 Step 8, 2→Task 6 Steps 1-2 ground-truth checks, 3→Task 6 Step 2 scoreboard block, 4→Task 6 Step 8 debug note, 5→backlog (VCS gate).
- Known deliberate deferrals (NOT gaps): QoS-weighted stimulus knob, wait-cycle stress axis, spatial patterns — backlog Step 2 lines.
- Two `<...>` slots in Task 7 Step 2 are read-the-source-first fills with the expected shape stated — the compare module is imported verbatim in Task 4, so the executor has the file.
