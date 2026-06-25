# FlooNoC Struct Refactor + Unified `make sim` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `noc_intf`/`axi4_intf` 兩個 SV interface 改成 specgen 生成的 in-package packed-struct typedef，fabric/tb generator 從攤平改 `genvar generate` + struct array（對齊 FlooNoC `tb_floo_axi_mesh`）;同時統一 `make sim` 入口、四個 traffic pattern 全部 base-driven。

**Architecture:** struct typedef 由 `sv_signals.py` emit 進 `ni_signals_pkg`（packed struct 當 cross-module port type 必須在 package 內）。一條 NMU↔router edge = 4 個 directed struct port（req-fwd / req-credit-bwd / rsp-fwd / rsp-credit-bwd，credit 方向與 channel 相反）。DPI 邊界**不變**（wrap 早已手動把 member 拆成 scalar 餵 DPI）。**port contract 與其 generated instantiation 必須在同一 commit 改**（wrap port 改 struct 的同一 task 內改 generator，否則 build 破）。

**Tech Stack:** Python（specgen codegen + generator）、SystemVerilog/Verilator(`--timing`)/VCS、CMake/GoogleTest、Git Bash/MSYS2、GNU make。

## Global Constraints
- **python**:`PYTHON3=python3`(mingw64),**不用 `py -3`**。pytest;無 pytest 時用 `python3 -c` harness 跑同斷言。
- **spec**:`docs/internal/superpowers/specs/2026-06-25-floonoc-struct-refactor-design.md`;每 task 隱含 spec 全約束。
- **branch**:feature branch off main;**不 push**;commit `type(scope): description`;不 amend、不 `--no-verify`。
- **specgen source of truth**:interface body 由 `specgen/source/interface_handshake.json`(field set)+ `sv_signals.py`(emit,`_emit_axi4_intf`/`_emit_noc_intf` 在 `endpackage` 後 append,`:261-270`)生成,**不是** `ni_signals.json`。`ni_signals.json` 不動。
- **fixed-width typedef in package**:`noc_chan_t`/`axi_req_t`/`axi_rsp_t`(寬度固定)放 `ni_signals_pkg`(struct 須在 package 才能當 cross-module port type);width 用 fully-qualified `ni_params_pkg::AXI_ID_WIDTH_DFLT`/`NOC_FLIT_WIDTH_DFLT` 等(struct 不可帶 parameter)。
- **per-config `noc_credit_t`(forward-compatible synthesizable RTL)**:struct typedef 不能帶 runtime parameter(Verilator issue#2783;class typedef 非 synth)。vc-dependent 的 `noc_credit_t {logic [num_vc-1:0] credit;}` 由 specgen **per-topology 生成**(新 codegen target `--num-vc N`),放獨立檔 `specgen/generated/sv/noc_types_pkg_vc{N}.sv`(**package 名固定 `noc_types_pkg`**;4 份預生成+committed,如同 `tb_top_<topo>.sv`)。**selector 是 `build_config.mk`(+ simulator Makefile),不是 `gen_filelist.py`**(後者只 serialize make 傳的 path):`build_config.mk` 的 `TB_TOP_SV_SRC` 依 `TOPOLOGY` 的 num_vc 把對應 `noc_types_pkg_vc{N}.sv` 列入(排 `ni_signals_pkg` 後、wrap 前);simulator Makefile 加 prereq 確保該檔在 filelist/build 前存在。每 build 只含一份→同名 package 無衝突(specgen pytest 只比對輸出文字、不 elaborate)。wrap/fabric port type 用 `noc_types_pkg::noc_credit_t`;`noc_chan_t`/`axi_*` 用 `ni_signals_pkg::`。drift gate `codegen --check` per-config 涵蓋 4 份(literal `[N-1:0]`,不 emit unresolved `NUM_VC`)。`VC_ID_WIDTH` 在 `ni_flit_pkg` 非 `ni_params_pkg`(若需 max 引用)。
- **wrap NUM_VC 對齊**:wrap `NUM_VC` parameter 必須 == 該 build 選到的 `noc_credit_t` baked width;加 elaboration assert(`$bits(noc_types_pkg::noc_credit_t)==NUM_VC`)防 silent truncation。DPI 不變:wrap 餵 `credit[NUM_VC-1:0]`(== baked width == topology num_vc),同現行 interface,C++ 端 max-array(`NMU_NUM_VC_MAX=1<<VC_ID_WIDTH`)對齊。
- **DPI invariant(normative)**:struct member 在每個 DPI 邊界**個別 unpack**;**禁止**把整 struct 傳 DPI。`cmodel_dpi.cpp`/`.h` 是 **hard no-touch**(且 `cmodel_dpi.cpp` 透過 `channel_model_wrap_io.hpp` 鏈到 C++ channel_model,不得牽動)。
- **channel_model 刪除範圍**:只刪 **SV** `sim/sv/channel_model_wrap.sv`(它 reference `noc_intf`)。**C++ channel_model 全保留**(`channel_model_wrap.hpp`/`_io.hpp` 被 `nmu_wrap_io.hpp:18` 依賴、`test_channel_model_wrap.cpp` 在 ctest 用);不得動 C++ side。
- **ADDR mirror**:`gen_test_patterns.ADDR_DST_SHIFT == c_model LOCAL_ADDR_BITS == 32`,address scheme 不動。
- **field-name contract(Task 1 鎖定,後續不得改)**:見 Task 1 Interfaces。
- **perf parity 是 gate**:每次改 generator 後 `perf.json` 對 **預存的 baseline** byte-compare(見 Task 0 baseline harness),非假設。
- **每 task gate** 綠才下一步:`make check PYTHON3=python3`(ctest 545,純 C++ 不受影響)+ 該 stage 的 co-sim/lint。
- **accepted regression(decision Y)**:刪 `run_regress.py` → AX4 bidirectional co-sim sweep 暫時流失,replacement deferred,**不宣稱 retained**;Task 6 gate 明示。

**基準**:開工前 `make check PYTHON3=python3` 全綠 + `make sim-regress TOPOLOGY=mesh_4x4_vc1` 6/6 + `python3 sim/tools/gen_tb_top.py --topology mesh_4x4_vc1 --check` exit 0。

---

### Task 0: 預存 perf baseline + 建 byte-compare harness

**Files:**
- Create: `sim/tools/perf_baseline/`(預存各 topology 一個 scenario 的 pre-refactor `perf.json`)
- Create: `sim/tools/check_perf_parity.py`(golden compare,排除已知 `noc.links` 浮動欄位)

- [ ] **Step 1: 跑 pre-refactor 一組 perf.json 當 golden**
```bash
for N in 1 2 4 8; do
  make build-verilator TOPOLOGY=mesh_4x4_vc$N PYTHON3=python3
  TOPOLOGY=mesh_4x4_vc$N python3 sim/run_regress.py    # 產 sim/verilator/output/AX4-BAS-005*/perf.json
  cp sim/verilator/output/AX4-BAS-005*/perf.json sim/tools/perf_baseline/mesh_4x4_vc$N.json
done
```
- [ ] **Step 2: 寫 check_perf_parity.py**
```python
# 比對兩個 perf.json,忽略 noc.links(re-emit 浮動);其餘須 byte/結構一致
import json, sys
def load(p): d=json.load(open(p)); d.get("noc",{}).pop("links",None); return d
sys.exit(0 if load(sys.argv[1])==load(sys.argv[2]) else 1)
```
- [ ] **Step 3: Commit**
```bash
git add sim/tools/perf_baseline/ sim/tools/check_perf_parity.py
git commit -m "test(sim): pin pre-refactor perf.json baselines + parity checker"
```

---

## Stage 1 — struct typedef + spike

### Task 1: specgen — ni_signals_pkg 固定 typedef + per-config noc_types_pkg(與 interface 並存)

> **REDO 註**:既有 commit 1fc2fce 已把 4 typedef **全部**放進 ni_signals_pkg(含 `noc_credit_t.credit[NOC_NUM_VC_DFLT-1:0]`,固定 1 bit — multi-VC 錯)。本 task 把 `noc_credit_t` **移出** ni_signals_pkg → 改由 per-config `noc_types_pkg` 提供;`noc_chan_t`/`axi_req_t`/`axi_rsp_t`(固定寬度)留 ni_signals_pkg。原因:struct 不能帶 parameter,credit 寬度須 per-topology baked(見 Global Constraints)。

**Files:**
- Modify: `specgen/tools/elaborate/sv_signals.py`(ni_signals_pkg 移除 `noc_credit_t`、留固定 typedef;新增 `noc_types_pkg` per-config emitter;**保留** interface emitter)
- Modify: `specgen/tools/codegen.py`(新 target `("sv","noc_types")`,接 `--num-vc N` → 輸出 `noc_types_pkg_vc{N}.sv`)
- Create: `specgen/generated/sv/noc_types_pkg_vc{1,2,4,8}.sv`(4 份預生成,**package 名固定 `noc_types_pkg`**)
- Test: `specgen/tests/test_codegen_sv.py`(沿用既有 `run_codegen` + `read_text` 風格)

**Interfaces:**
- Produces — **field-name contract(全 stage 凍結)**:
  ```systemverilog
  // ni_signals_pkg (固定寬度;width 用 ni_params_pkg::*_DFLT):
  typedef struct packed { logic valid; logic [ni_params_pkg::NOC_FLIT_WIDTH_DFLT-1:0] flit; } noc_chan_t;
  typedef struct packed { /* AW+W+AR + bready + rready (master drives) */ } axi_req_t;
  typedef struct packed { /* B+R + awready/wready/arready (slave drives) */ } axi_rsp_t;
  // noc_types_pkg (per-topology;package 名固定;num_vc baked at generate time):
  typedef struct packed { logic [num_vc-1:0] credit; } noc_credit_t;   // num_vc=8 → credit[7:0]
  ```
  存取:`noc_o.req.valid`;`axi_req_i.awvalid`;`noc_types_pkg::noc_credit_t`。`axi_*` 沿用現有 interface member 名;`awregion`/`arregion` 保留為 carried-but-unused(Task 4 policy)。

- [ ] **Step 1: 失敗測試(固定 typedef 與 per-config credit 分家)**
`test_codegen_sv.py`(既有 helper `run_codegen` `:25`、`read_text` `:43`)。signals domain class 加:
```python
def test_ni_signals_fixed_typedefs_no_credit(self):
    run_codegen("--target","sv","--domain","signals","--out",str(RTL_PKG_DIR))
    sv = (RTL_PKG_DIR / "ni_signals_pkg.sv").read_text(encoding="ascii")
    assert "} noc_chan_t;" in sv and "} axi_req_t;" in sv and "} axi_rsp_t;" in sv
    assert "noc_credit_t" not in sv          # 已移到 noc_types_pkg
def test_noc_types_pkg_per_vc(self, tmp_path):
    run_codegen("--target","sv","--domain","noc_types","--num-vc","8","--out",str(tmp_path))
    sv8 = (tmp_path / "noc_types_pkg_vc8.sv").read_text(encoding="ascii")
    assert "package noc_types_pkg" in sv8 and "} noc_credit_t;" in sv8 and "[7:0]" in sv8
    run_codegen("--target","sv","--domain","noc_types","--num-vc","1","--out",str(tmp_path))
    assert "[0:0]" in (tmp_path / "noc_types_pkg_vc1.sv").read_text(encoding="ascii")
```
> interface 消失的斷言**不在此 task**;Task 4 移除 interface 後才加。
- [ ] **Step 2: 跑測試確認 fail**
Run: `cd specgen && python3 -m pytest tests/test_codegen_sv.py -k "credit or noc_types" -v` → FAIL。
- [ ] **Step 3: 實作**
- `sv_signals.py`:ni_signals_pkg 的 struct emitter **移除 noc_credit_t**(留 noc_chan_t + axi_req_t/axi_rsp_t,axi 從 `_AXI_CHANNEL_SIGNALS` matrix `:28-78` 依 master modport 切,含 awregion/arregion)。**保留** interface emitter(`:267-270`,並存)。
- 新增 `_emit_noc_types_pkg(num_vc)` → `\`ifndef ... package noc_types_pkg; typedef struct packed { logic [num_vc-1:0] credit; } noc_credit_t; endpackage ...`(include guard,package 名固定)。
- `codegen.py` 加 `("sv","noc_types")` target + `--num-vc` arg,輸出檔名 `noc_types_pkg_vc{N}.sv`。
- [ ] **Step 4: pass + 生成 4 份 + drift**
```bash
cd specgen && python3 -m pytest tests/test_codegen_sv.py -v
cd /e/05_NoC/noc_project && python3 specgen/tools/codegen.py                              # ni_signals_pkg(無 credit)
for N in 1 2 4 8; do python3 specgen/tools/codegen.py --target sv --domain noc_types --num-vc $N --out specgen/generated/sv; done
python3 specgen/tools/codegen.py --check                                                  # ni_signals drift
for N in 1 2 4 8; do python3 specgen/tools/codegen.py --target sv --domain noc_types --num-vc $N --check; done   # noc_types per-config drift
```
Expected: pytest 綠;`ni_signals_pkg.sv` 無 `noc_credit_t`;4 份 `noc_types_pkg_vc{N}.sv` 各 `credit[N-1:0]`;所有 `--check` exit 0。
- [ ] **Step 5: Commit**
```bash
git add specgen/tools/elaborate/sv_signals.py specgen/tools/codegen.py specgen/tests/test_codegen_sv.py specgen/generated/sv/ni_signals_pkg.sv specgen/generated/sv/noc_types_pkg_vc*.sv
git commit -m "feat(specgen): per-config noc_types_pkg(noc_credit_t) + fixed-width typedefs in ni_signals_pkg"
```

### Task 2: mesh_2x1_vc8 NoC-struct early spike(快速 GO/NO-GO,非完整 de-risk)

> **範圍誠實**:這只是 early signal — 驗「struct port + `[N]`/`[LINK_PORTS]` array + vc8 `credit[8]`」基本能 elaborate。**完整的 generated multi-node / tie-off / perf-order elaboration 在 Task 3 用 4x4-vc8 驗**(spike 過 ≠ generator 過)。

**Files:**
- Create: `sim/topologies/mesh_2x1_vc8.yaml`、`sim/sv/spike/{noc_fabric_spike,tb_top_spike}.sv`(手寫,spike 後刪)

- [ ] **Step 1: 生成 vc8 noc_types_pkg + 手寫 2-node struct fabric + stub**
先 `python3 specgen/tools/codegen.py --target sv --domain noc_types --num-vc 8 --out specgen/generated/sv`(產 `noc_types_pkg_vc8.sv`,須含 `credit[7:0]`)。`mesh_2x1_vc8.yaml`(`x_dim:2 y_dim:1 num_vc:8`);手寫最小 fabric,NoC NI face 用 `ni_signals_pkg::noc_chan_t req[2]` + `noc_types_pkg::noc_credit_t req_cred[2]`(**真 credit[8]**,這才是 spike 該驗的形狀)等 struct array,node/link 放 `generate for`;wrap 可用 stub(只驗 elaboration + 一拍 handshake)。
- [ ] **Step 2: lint-only(關鍵風險:struct-in-array port + 真 vc8 寬度)**
```bash
verilator --lint-only --timing -I specgen/generated/sv -I sim/sv/spike \
  specgen/generated/sv/ni_params_pkg.sv specgen/generated/sv/ni_signals_pkg.sv \
  specgen/generated/sv/ni_flit_pkg.sv specgen/generated/sv/noc_types_pkg_vc8.sv \
  sim/sv/spike/noc_fabric_spike.sv sim/sv/spike/tb_top_spike.sv
```
Expected: 無 elaboration error;確認 `noc_types_pkg::noc_credit_t` 真的是 `credit[8]`(非舊 spike 的 credit[1])。報 unpacked-array element type mismatch → 用 `router_wrap.sv:164-181` 的 `logic`→`bit` mirror 手法。
- [ ] **Step 3: 一拍 handshake PASS + Commit**
build + run spike,driver 打一個 req flit、收 credit。
```bash
git add sim/topologies/mesh_2x1_vc8.yaml sim/sv/spike/
git commit -m "test(sim): mesh_2x1_vc8 noc-struct early elaboration spike"
```
> GO/NO-GO:綠 → Task 3。紅 → STOP,systematic-debugging。

---

## Stage 2 — NoC full struct (wrap + generator 同 commit)

### Task 3: NoC wrap port + fabric emitter 同步改 struct;刪 SV channel_model_wrap

> wrap port contract 與 generated fabric 的 instantiation **必須同 commit 改**,否則 build 破(Codex top concern)。本 task 一次改 4 個 wrap 的 NoC port + `emit_fabric` + 刪 SV channel_model,gate 直接 build generated fabric。

**Files:**
- Modify: `sim/sv/nmu_wrap.sv`、`nsu_wrap.sv`、`router_wrap.sv`、`ni_wrap.sv`(NoC port `noc_intf`→struct;DPI 取值點改 struct field;每個用 `noc_types_pkg::noc_credit_t` 的 module 加 `$bits(noc_types_pkg::noc_credit_t)==NUM_VC` elaboration assert 防 silent truncation)
- Modify: `sim/tools/gen_tb_top.py`(`emit_fabric` `:214-299`:per-node `noc_intf` instance → struct array;node/link/tie-off/perf 進 `generate`)
- Modify: `sim/build_config.mk`(`TB_TOP_SV_SRC` 依 `TOPOLOGY` num_vc 列入對應 `noc_types_pkg_vc{N}.sv`,排 `ni_signals_pkg` 後、wrap 前)+ `sim/verilator/Makefile`/`sim/vcs/Makefile`(prereq:該 `noc_types_pkg` 在 filelist/build 前存在)
- Delete: `sim/sv/channel_model_wrap.sv`(SV only)

**Interfaces:**
- Consumes:Task 1 `ni_signals_pkg::noc_chan_t` + per-config `noc_types_pkg::noc_credit_t`(該 topology num_vc 的 baked body)。
- Produces:wrap NoC port = `output ni_signals_pkg::noc_chan_t noc_req_o, input noc_types_pkg::noc_credit_t noc_req_cred_i, input noc_chan_t noc_rsp_i, output noc_credit_t noc_rsp_cred_o`(nmu 視角;router/nsu 對應方向)。fabric 用 `ni_signals_pkg::noc_chan_t req[N]` + `noc_types_pkg::noc_credit_t req_cred[N]` array。

- [ ] **Step 1: 刪 SV channel_model_wrap(C++ 不動)**
```bash
git rm sim/sv/channel_model_wrap.sv
# SV-build-source gate(只查 SV filelist/build,C++ refs 是 intentional 保留):
rg -n "channel_model_wrap\.sv|channel_model_wrap\b" sim/filelist_*.f sim/build_config.mk sim/sv/ || echo "no SV refs"
```
Expected: SV build source 無殘留(C++ `channel_model_wrap.hpp` 等保留,不在此 gate)。
- [ ] **Step 2: 改 4 個 wrap 的 NoC port + DPI 取值**
- `nmu_wrap.sv:48` `noc_intf.mosi noc_mosi_o` → 4 struct port。DPI 取值(`:195-198`):`noc_mosi_o.rsp_valid`→`noc_rsp_i.valid`、`.rsp_flit`→`noc_rsp_i.flit`、`.req_credit_return`→`noc_req_cred_i.credit`。drive(`:267-272`):`noc_mosi_o.req_valid`→`noc_req_o.valid` 等。
- `nsu_wrap.sv` `noc_miso_i` → struct(對照該檔行號逐一改)。
- `router_wrap.sv:49-51` `noc_nmu_i`/`noc_nsu_o` → struct;取值 `:184-189`;**LINK face(`:56-68`)不動**(plain `logic [LINK_PORTS]`)。
- `ni_wrap.sv:37-38` `noc_nmu_o`/`noc_nsu_i` → struct,接 u_nmu/u_nsu 的 struct port。
- **DPI 函式簽章一律不動**(invariant)。
- [ ] **Step 3: 改 emit_fabric — NoC face struct array + generate**
`gen_tb_top.py:214-220` per-node `noc_intf` instance → `ni_signals_pkg::noc_chan_t node_req[n]; noc_credit_t node_req_cred[n]; node_rsp[n]; node_rsp_cred[n];`。node 實例(`:238-273`)`.noc_nmu_o(node{i}_nmu.mosi)` → `.noc_req_o(node_req[i]), .noc_req_cred_i(node_req_cred[i]), ...`,包進 `generate for`。**array-indexing contract**:emit 順序維持 `_nodes()` raster(`:107-121`);link wiring(`:282-299`)/tie-off(`:318-328`,逐 missing direction 的 `$fatal` assertion 必須保留)/perf(`:336-363`)的 `seen` 去重順序不變 → perf label 穩定。LINK perf taps 不動。
- [ ] **Step 4: gate — 重生 + build generated fabric + co-sim + perf parity(含 4x4-vc8)**
```bash
for N in 1 2 4 8; do python3 sim/tools/gen_tb_top.py --topology mesh_4x4_vc$N; done
for N in 1 2 4 8; do
  make build-verilator TOPOLOGY=mesh_4x4_vc$N PYTHON3=python3      # 真 build generated fabric
  TOPOLOGY=mesh_4x4_vc$N python3 sim/run_regress.py                # 6/6 scoreboard clean
  python3 sim/tools/check_perf_parity.py sim/verilator/output/AX4-BAS-005*/perf.json sim/tools/perf_baseline/mesh_4x4_vc$N.json
done
make check PYTHON3=python3                                          # ctest 545(C++ 不受影響)
python3 sim/tools/gen_tb_top.py --topology mesh_4x4_vc1 --check
```
Expected: 四 topology build 綠 + 6/6 + perf parity 0 + ctest 545 + drift exit 0。**4x4-vc8 build 是真正的 multi-node/tie-off/vc8 de-risk**(spike 之外)。
- [ ] **Step 5: Commit**
```bash
git add sim/sv/nmu_wrap.sv sim/sv/nsu_wrap.sv sim/sv/router_wrap.sv sim/sv/ni_wrap.sv sim/tools/gen_tb_top.py sim/sv/noc_fabric_*.sv
git rm sim/sv/channel_model_wrap.sv
git commit -m "refactor(sim): noc wrap+fabric interface->struct array+generate; drop SV channel_model_wrap"
```

---

## Stage 3 — AXI full struct (wrap + tb + interface 移除 同 commit)

### Task 4: AXI wrap+endpoint+tb_top struct + node/ctx genvar generate;移除 interface;awregion policy

> Task 3 把 NoC face 改 struct-array 但 node 實例仍 Python-unrolled(因 ctx handle 還是 individual port)。本 task 把 ctx handle **array 化**(tb_top create→array port),於是 `emit_fabric` 的 node(ni_wrap+router_wrap)實例 + `emit_tb_top` 的 endpoint 都能收進 SV `genvar generate` — 這才真正消攤平、達成「短」的目標。AXI struct + interface 移除一併在此 commit。

**Files:**
- Modify: `sim/sv/axi_master_wrap.sv`、`axi_slave_wrap.sv`、`ni_wrap.sv`(AXI face)、`user_node_endpoint.sv`(monitor tap)
- Modify: `sim/tools/gen_tb_top.py`(`emit_tb_top` `:507-557` axi struct array + ctx-handle array + endpoint generate;`emit_fabric` node 實例 + ctx port → `genvar generate`)
- Modify: `specgen/tools/elaborate/sv_signals.py`(移除 interface emitter)、`specgen/tests/test_codegen_sv.py`(加 interface-absent 斷言)

**Interfaces:**
- Consumes:Task 1 `ni_signals_pkg::axi_req_t`/`axi_rsp_t`;Task 3 noc struct fabric。
- Produces:AXI port = `output axi_req_t axi_req_o, input axi_rsp_t axi_rsp_i`(master 視角);tb_top ctx handle 改 array(`longint unsigned nmu_ctx[N]` 等)、fabric node 實例與 endpoint 用 `genvar generate`;`ni_signals_pkg.sv` 無 interface。

- [ ] **Step 1: awregion/arregion policy + grep**
```bash
rg -n "awregion|arregion" sim/sv/axi_master_wrap.sv sim/sv/axi_slave_wrap.sv sim/sv/user_node_endpoint.sv
```
policy:`axi_req_t` **保留** `awregion`/`arregion` field(carried-but-unused);master_wrap 不 drive 它們(維持現狀,struct field 留預設 `'0`);不新增 DPI arg。
- [ ] **Step 2: 改 axi_master/slave_wrap + ni_wrap AXI port + DPI 取值**
`axi4_intf.master`/`.slave` → `axi_req_t`/`axi_rsp_t` struct port。DPI 取值 `axi_i.awvalid`→`axi_req_i.awvalid` 逐 field。DPI 簽章不動。`ni_wrap.sv:35-36` AXI face → struct。
- [ ] **Step 3: 改 user_node_endpoint monitor tap**
`user_node_endpoint.sv:60-88` 的 30+ 行 tap(`master_axi_o.awvalid`…`slave_axi_i.rid`)→ struct field(`axi_req.awvalid`…`axi_rsp.rid`);`axi_perf_monitor` 模組(scalar port)不動。
- [ ] **Step 4: emit_tb_top AXI struct array + ctx-handle array + node/endpoint genvar generate**
- **AXI array**:`gen_tb_top.py:507-513` per-node `axi4_intf` → `ni_signals_pkg::axi_req_t master_axi_req[n]; axi_rsp_t master_axi_rsp[n];`(NMU+NSU 側)。
- **ctx array 化**:tb_top 的 `nmu_ctx`/`nsu_ctx`/`router_ctx`/`m_ctx`/`s_ctx` 從 per-node individual 變 `longint unsigned nmu_ctx[n]` 等 array;`cmodel_*_create` 在 `initial` loop 逐一填 array 元素。fabric 的 ctx port 也改 array(`input longint unsigned nmu_ctx[N]`)。
- **fabric node generate**:`emit_fabric` 的 node 實例(ni_wrap+router_wrap,Task 3 仍 Python-unrolled)+ inter-router wiring + tie-off + perf 收進 `for (genvar i=0;i<N;i++) begin: g_node ... end`;ctx 用 `nmu_ctx[i]`;coord 用 `localparam X=i%X_DIM, Y=i/X_DIM`(mirror `_nodes()` raster,保 routing id)。
- **endpoint generate**:`emit_tb_top` 的 `user_node_endpoint`(`:546-557`)收進 generate,`MASTER_SLOT_NAME` 用 `$sformatf("node%0d.manager",i)`(保 perf label 字串等價、byte-identical)。
- emit 順序(raster + `seen` dedup)與 LINK perf taps 維持 → perf parity gate 會抓任何 drift。
- [ ] **Step 5: 移除 interface emitter + 加 interface-absent 測試**
`sv_signals.py`:刪 `endpackage` 後 `_emit_interfaces_from_handshake_schema`(`:267-270`)+ `_emit_axi4_intf`/`_emit_noc_intf`。`test_codegen_sv.py` 加:
```python
def test_interfaces_removed(self):
    sv = (RTL_PKG_DIR / "ni_signals_pkg.sv").read_text(encoding="ascii")
    assert "interface noc_intf" not in sv and "interface axi4_intf" not in sv
```
```bash
cd specgen && python3 -m pytest tests/test_codegen_sv.py -v && cd .. && python3 specgen/tools/codegen.py && python3 specgen/tools/codegen.py --check
```
- [ ] **Step 6: gate — 全 topology co-sim + VCS one-topology + tb line down + perf parity**
```bash
for N in 1 2 4 8; do python3 sim/tools/gen_tb_top.py --topology mesh_4x4_vc$N; done
for N in 1 2 4 8; do
  make build-verilator TOPOLOGY=mesh_4x4_vc$N PYTHON3=python3 && TOPOLOGY=mesh_4x4_vc$N python3 sim/run_regress.py
  python3 sim/tools/check_perf_parity.py sim/verilator/output/AX4-BAS-005*/perf.json sim/tools/perf_baseline/mesh_4x4_vc$N.json
done
make -C sim/vcs tb_top TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3   # VCS gate(非 defer;tb_top 是 generated-top build target,sim/vcs/Makefile:202)
make check PYTHON3=python3
wc -l sim/sv/tb_top_mesh_4x4_vc1.sv                       # 對比 pre-refactor ~630 行,預期降 >=50%
```
Expected: 四 topology 6/6 + VCS 綠 + tb_top 行數降 >=50% + perf parity + ctest 545 + `ni_signals_pkg.sv` 無 interface。
- [ ] **Step 7: Commit**
```bash
git add sim/sv/axi_master_wrap.sv sim/sv/axi_slave_wrap.sv sim/sv/ni_wrap.sv sim/sv/user_node_endpoint.sv sim/tools/gen_tb_top.py specgen/tools/elaborate/sv_signals.py specgen/tests/test_codegen_sv.py specgen/generated/sv/ni_signals_pkg.sv sim/sv/tb_top_*.sv sim/sv/noc_fabric_*.sv
git commit -m "refactor(sim): axi wrap+tb->struct array+generate; remove SV interfaces"
```

---

## Stage 4 — A (unified make sim) + B (patterns from base)

### Task 5: B — gen_test_patterns 四 pattern 統一 base-driven

**Files:**
- Modify: `sim/tools/gen_test_patterns.py`(`uniform_random`/`hotspot` 改吃 `--from` base shape;`transpose` 已 dual-mode)
- Test: `sim/tools/test_gen_test_patterns.py`

**Interfaces:**
- Consumes:base `scenario.yaml`(size/len/burst/payload)。
- Produces:四 pattern 都走 `--from`;`pattern(i)` 只決定 dst;`alloc_unique_offset`(`:210-254`)複用。

- [ ] **Step 1: 失敗測試 — uniform/hotspot 從 base 取 shape + capacity 斷言**
```python
def test_uniform_random_uses_base_shape(tmp_path):
    base = write_base(tmp_path, size=3, length=4)
    out = run_gen(base, pattern="uniform_random", topo="mesh_4x4_vc1", seed=1, txn=2, frm=True)
    txns = load(out / "node0" / "scenario.yaml")["transactions"]
    assert all(t["size"] == 3 and t["len"] == 4 for t in txns)   # shape from base
def test_base_driven_respects_alloc_capacity(tmp_path):
    # 每 node txn 超過 memory window(mesh_4x4 @0x1000 = 4 txn 上限)須 raise,不靜默溢出
    base = write_base(tmp_path, size=2, length=0)
    with pytest.raises(ValueError):
        run_gen(base, pattern="uniform_random", topo="mesh_4x4_vc1", seed=1, txn=8, frm=True)
```
- [ ] **Step 2: 跑測試確認 fail**
Run: `cd sim/tools && python3 -m pytest test_gen_test_patterns.py -k "base_shape or capacity" -v` → FAIL。
- [ ] **Step 3: 實作 — uniform/hotspot 讀 base shape**
`gen_test_patterns.py:585-620` uniform/hotspot 路徑:`--from` 提供時,copy base transaction 的 size/len/burst/payload,只把 addr 換成 `pattern(i)` dst + `alloc_unique_offset`(其既有 bound assert `:247` 自然涵蓋 capacity 測試)。保留 `--size`/`--len` 當無 `--from` fallback。`transpose`(`:554-583`)確認一致。
- [ ] **Step 4: 測試 + 不回歸**
```bash
cd sim/tools && python3 -m pytest test_gen_test_patterns.py -v
```
Expected: 新測試 PASS;neighbor/transpose 既有測試不回歸。
- [ ] **Step 5: Commit**
```bash
git add sim/tools/gen_test_patterns.py sim/tools/test_gen_test_patterns.py
git commit -m "feat(sim): all four patterns base-driven (shape from --from base)"
```

### Task 6: A — 統一 make sim;run_benchmark 加 --from;刪 run_regress

**Files:**
- Modify: `sim/tools/run_benchmark.py`(加 `--from`/`--base`;當 `make sim` 後端)
- Modify: `Makefile`(`sim` target;`TB` 為 `TOPOLOGY` alias;`check` 加 smoke)
- Delete: `sim/run_regress.py`

**Interfaces:**
- Produces:`make sim TB=<topo> PATTERN=<p> [TXN= SEED= HOTSPOT= BASE=]`。
- Consumes:Task 5 base-driven `gen_test_patterns`。

- [ ] **Step 1: run_benchmark 加 --from/--base**
`run_benchmark.py` argparse 加 `--from`(`:135` 附近),轉傳 `gen_test_patterns --from`;無指定時用 canonical base `sim/test_patterns/AX4-BAS-003_single_write_read_aligned/scenario.yaml`。加最小 CLI 測試(arg 解析 + 預設 base 存在)。
- [ ] **Step 2: Makefile sim target(TB 為 TOPOLOGY alias)**
```makefile
TB ?= mesh_4x4_vc1
sim:                                                 # 不用 depend(會丟 TB);recipe 內顯式傳 TOPOLOGY=$(TB)
	$(MAKE) build-verilator TOPOLOGY=$(TB) PYTHON3=$(PYTHON3)
	$(PYTHON3) sim/tools/run_benchmark.py --topology $(TB) --pattern $(PATTERN) \
	  $(if $(TXN),--transactions-per-node $(TXN)) $(if $(SEED),--seed $(SEED)) \
	  $(if $(HOTSPOT),--hotspot $(HOTSPOT)) $(if $(BASE),--from $(BASE))
```
**關鍵**:`build-verilator` 用 `TOPOLOGY`(default `mesh_4x4_vc1`,`Makefile:111,152`),故 `sim` recipe **顯式** `$(MAKE) build-verilator TOPOLOGY=$(TB)`,否則 `make sim TB=mesh_4x4_vc8` 會 build 錯 topology。保留既有 `TOPOLOGY` 使用者相容(TB 預設取 mesh_4x4_vc1)。移除 `bench`/`sim-regress` target;`check` 末加 `$(PYTHON3) sim/tools/run_benchmark.py --topology mesh_4x4_vc1 --pattern neighbor`(smoke)。
- [ ] **Step 3: 刪 run_regress + 殘留 call sites**
```bash
git rm sim/run_regress.py
rg -n "run_regress|sim-regress" Makefile sim/ docs/ || echo "no refs"
```
更新任何 `make sim-regress` 引用。
- [ ] **Step 4: gate — make sim 各 pattern + make check**
```bash
for P in neighbor transpose uniform_random; do make sim TB=mesh_4x4_vc1 PATTERN=$P PYTHON3=python3; done
make sim TB=mesh_4x4_vc1 PATTERN=hotspot HOTSPOT=5 PYTHON3=python3
make sim TB=mesh_4x4_vc8 PATTERN=neighbor PYTHON3=python3      # 驗 TB 真的傳給 build(非 default topology)
make check PYTHON3=python3                       # ctest 545 + neighbor smoke
python3 sim/tools/gen_tb_top.py --topology mesh_4x4_vc1 --check
```
Expected: 各 pattern PASS;`make check` 綠;drift exit 0。
> **coverage 註記(decision Y)**:此 commit 刪除 `run_regress.py` 的 AX4 bidirectional sweep,以 neighbor smoke 為最低 gate;**AX4 curated 覆蓋暫時流失,replacement deferred**,不宣稱 retained。
- [ ] **Step 5: Commit + spike 清理**
```bash
git rm -r sim/sv/spike sim/topologies/mesh_2x1_vc8.yaml   # Task 2 spike 全部移除(含 topology yaml)
git add Makefile sim/tools/run_benchmark.py
git rm sim/run_regress.py
git commit -m "feat(sim): unified make sim (replaces bench+sim-regress); run_benchmark --from; drop run_regress + spike"
```

---

## Self-Review notes
- **每 commit 可編譯**:wrap port 與 generator instantiation 同 task 改(Task 3 noc、Task 4 axi);interface 與 typedef 並存到 Task 4 才移除;每 task gate 都 build generated fabric(非僅 wrap lint)。
- **spike vs 真 gate**:Task 2 spike 是 early signal;真正 multi-node/tie-off/vc8 de-risk 在 Task 3 的 4x4-vc8 build。
- **channel_model**:只刪 SV;C++ 全留(nmu_wrap_io 依賴);gate 只查 SV build source。
- **DPI**:`cmodel_dpi.cpp`/`.h` hard no-touch;簽章每 task 不動。
- **perf**:Task 0 預存 baseline + checker,Task 3/4 byte-compare。
- **field-name contract**:Task 1 鎖 NoC+AXI;awregion/arregion carried-but-unused(Task 4 policy)。
- **VCS**:Task 4 Step 6(非 defer)。
- **coverage regression**:Task 6 明示 deferred,不宣稱 retained。
