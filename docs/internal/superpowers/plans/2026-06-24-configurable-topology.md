# Configurable NoC Topology + Generated Testbench — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** NoC 拓樸從寫死 2-node 變成可設定 N×M mesh,testbench 由 Python generator 程式化產生,並把既有 c_model multi-VC 接出到 wire-level co-sim。

**Architecture:** specgen 持有拓樸維度 + flit-binding invariant;`gen_tb_top.py` 讀 specgen 常數展開 `noc_fabric_<topo>.sv`(fabric,每節點露 AXI port)+ `tb_top.sv`(test harness),產物納 drift gate。wire 層 DPI 一次改成 per-PORT × per-VC,先 single-VC/2-node 重現再逐步加 multi-VC、方向 port、N×M。

**Tech Stack:** Python(specgen + generator + run_regress)、SystemVerilog/Verilator(`--timing`)/VCS、C++17 DPI、CMake/GoogleTest、Git Bash + GNU make。

## Global Constraints

- **python**:`PYTHON3=python3`(mingw64),**不用 `py -3`**。
- **spec**:`docs/internal/superpowers/specs/2026-06-24-configurable-topology-design.md`(e52befa);本 plan 每 task 隱含 spec 全部約束。
- **commit 格式**:`type(scope): description`(English);不 `--no-verify`;不 amend(失敗就新 commit)。
- **不 push**;feature branch `feature/configurable-topology`(off main e52befa)。
- **每 task gate 綠才下一步**;觸及 SV/build 用 `make check PYTHON3=python3`(stale 先 `make clean-verilator`)。
- **不新增 single-VC 寫死**;移除既有 single-VC 限制時三個 wrap(`router_wrap`/`nmu_wrap`/`nsu_wrap`)同步。
- **generated 產物**(`gen_tb_top.py` 輸出、specgen 輸出)一律附 `--check` drift gate;手改 generated 檔要被擋。
- **拓樸維度單一真相 = specgen**`MESH_X_DIM`/`MESH_Y_DIM`;flit binding 為硬上限(`X_WIDTH+Y_WIDTH=DST_ID_WIDTH=8`、`NUM_VC≤2^VC_ID_WIDTH=8`)。
- **SURGICAL**:每 task 只動該 task 範圍;`git add` 明確路徑,不 `git add -A`(`docs/issue/` gitignored)。
- **subagent 遇 spec/repo 偏差先 BLOCKED 回報**,不 inline workaround。

**基準**:開工前 `make check PYTHON3=python3` 全綠 + `python3 specgen/tools/codegen.py --check` exit 0。

---

### Task 1: specgen 參數去 `NI_` 冗餘前綴（S-1）

**Files:**
- Modify: `specgen/source/constants.yaml`(各參數 `cpp_symbol`/`sv_symbol` 去 `NI_`)、`specgen/ni_spec/constants.py`(emitter 若有 `NI_` 前綴組裝邏輯)
- Regen: `specgen/generated/cpp/ni_params.h`、`specgen/generated/sv/ni_params_pkg.sv`、`ni_signals_pkg.sv`(經 `codegen.py`,不手改)
- Modify(consumer — **以 grep 全掃為準,不靠寫死清單**):已知含 `NI_NOC_`/`NI_AXI_` 者包含 `c_model/include/router/router.hpp`、`c_model/tests/router/test_router.cpp`、`c_model/tests/router/two_node_fabric.hpp`、`c_model/tests/router/test_router_adapters.cpp`、`sim/sv/channel_model_wrap.sv`、`sim/sv/nmu_wrap.sv`、`sim/sv/nsu_wrap.sv`、`sim/sv/router_wrap.sv`、`sim/sv/tb_top.sv`、`sim/sv/axi_master_wrap.sv`、`sim/sv/axi_slave_wrap.sv`、specgen goldens/tests(如 `specgen/tests/test_handshake_schema.py`、`specgen/tests/golden/*`)

**Interfaces:**
- Produces:`ni::NOC_NUM_VC`/`NOC_FLIT_WIDTH`/`NOC_MESH_X_DIM`/`NOC_MESH_Y_DIM`/`NOC_ROUTER_VC_DEPTH`/`NOC_ROUTER_OUTPUT_FIFO_DEPTH`/`NOC_SLAVE_VC_BUFFER_DEPTH`;`ni::AXI_ID_WIDTH`/`AXI_ADDR_WIDTH`/`AXI_DATA_WIDTH`/`AXI_WSTRB_WIDTH`;SV 對應 `ni_params_pkg::NOC_*_DFLT` / `AXI_*_DFLT`。

- [ ] **Step 1: 改 source 的 symbol 名**
在 `specgen/source/constants.yaml`,把每個參數的 `cpp_symbol: NI_NOC_X` → `NOC_X`、`cpp_symbol: NI_AXI_X` → `AXI_X`,`sv_symbol` 同樣去 `NI_`(保留 `_DFLT` 尾綴)。若 `constants.py` 有「組 `NI_` 前綴」的邏輯,改成不加 `NI_`。
- [ ] **Step 2: 重生 + drift gate(此時會 fail,因 consumer 未改)**
Run: `python3 specgen/tools/codegen.py --target cpp --domain params && python3 specgen/tools/codegen.py --target sv --domain params`
Expected: 重生成功;generated header 出現 `NOC_NUM_VC` 等新名。
- [ ] **Step 3: 全 repo 改 consumer 引用(grep-driven,確保完整)**
```bash
cd "E:/05_NoC/noc_project"
# 注意:specgen goldens 改名後須由 codegen 重生對齊,golden 測試會比對;若 golden 是 NI_ 文本,
# 隨 generated 一起更新(Step 2 重生後 golden 也要更新)。原始碼/SV/測試逐檔 sed:
for f in $(git ls-files | grep -vE 'docs/internal|cross-review|\.superpowers|specgen/generated' | xargs grep -lE 'NI_NOC_|NI_AXI_' 2>/dev/null); do
  sed -i 's/NI_NOC_/NOC_/g; s/NI_AXI_/AXI_/g' "$f"
done
```
- [ ] **Step 4: 殘留掃描**
```bash
git ls-files | grep -vE 'docs/internal|cross-review|\.superpowers' | xargs grep -lE 'NI_NOC_|NI_AXI_' 2>/dev/null
```
Expected: 空。
- [ ] **Step 5: gate**
Run: `python3 specgen/tools/codegen.py --check && make check PYTHON3=python3`
Expected: drift exit 0;ctest 全綠。
- [ ] **Step 6: Commit**
```bash
git add specgen/source/constants.yaml specgen/ni_spec/constants.py specgen/generated c_model/include/router/router.hpp c_model/tests/router/test_router.cpp c_model/tests/router/two_node_fabric.hpp sim/sv/channel_model_wrap.sv sim/sv/nmu_wrap.sv sim/sv/nsu_wrap.sv sim/sv/router_wrap.sv sim/sv/tb_top.sv
git commit -m "refactor(specgen): drop redundant NI_ prefix from NOC_*/AXI_* params"
```

---

### Task 2: flit-binding L2 invariant（S-1，D6）

**Files:**
- Modify: `specgen/ni_spec/invariants.py`(加 mesh-vs-flit 上限檢查)
- Test: `specgen/tests/test_invariants*.py`(若無對應檔則加 case 至既有 invariants 測試)

**Interfaces:**
- Consumes:generated `X_WIDTH`/`Y_WIDTH`/`DST_ID_WIDTH`/`VC_ID_WIDTH`(`ni_flit_constants`)、`MESH_X_DIM`/`MESH_Y_DIM`/`NOC_NUM_VC`(params)。
- Produces:`check_mesh_within_flit()` invariant,違反時 `codegen.py --check` 回非零。

- [ ] **Step 1: 寫 invariant(對齊真實 API:回 `List[Issue]` 用 `_err`,非 assert)**
`invariants.py` 的 check 模式:`def check_x(spec) -> List[Issue]`,append `_err(TAG, msg)`,由 `check_all(bundle)` 聚合(`invariants.py:28,32,252`)。加:
```python
def check_mesh_within_flit(constants) -> List[Issue]:
    """L2: mesh dims must fit flit dst_id / vc_id field capacity (spec 2026-06-24 sec 3)."""
    TAG = "L2-MESH-FLIT"
    issues: List[Issue] = []
    g = lambda k: int(constants[k]["value"])   # accessor shape per load_constants() output; adapt at impl
    x_w, y_w, dst_w, vc_w = g("X_WIDTH"), g("Y_WIDTH"), g("DST_ID_WIDTH"), g("VC_ID_WIDTH")
    mx, my, nvc = g("MESH_X_DIM"), g("MESH_Y_DIM"), g("NOC_NUM_VC")
    if x_w + y_w != dst_w:
        issues.append(_err(TAG, f"X_WIDTH+Y_WIDTH ({x_w}+{y_w}) != DST_ID_WIDTH ({dst_w})"))
    if mx > (1 << x_w):
        issues.append(_err(TAG, f"MESH_X_DIM {mx} > 2^X_WIDTH {1 << x_w}"))
    if my > (1 << y_w):
        issues.append(_err(TAG, f"MESH_Y_DIM {my} > 2^Y_WIDTH {1 << y_w}"))
    if mx * my > (1 << dst_w):
        issues.append(_err(TAG, f"MESH_X_DIM*MESH_Y_DIM {mx*my} > 2^DST_ID_WIDTH {1 << dst_w}"))
    if nvc > (1 << vc_w):
        issues.append(_err(TAG, f"NOC_NUM_VC {nvc} > 2^VC_ID_WIDTH {1 << vc_w}"))
    return issues
```
> 在 `check_all(bundle)` 內以 `bundle.constants` 呼叫並 `extend` 進總 issues(`loader.py:64-70` 提供 `bundle.constants`)。`g` 的取值形狀以 `load_constants()` 實際輸出為準(可能是 `name→value` 或 `name→{value,...}`),實作時對 `constants.py` 確認。drift gate 在有 ERROR issue 時 fail。
- [ ] **Step 2: 加測試 — 合法值通過**
在對應測試檔加:預設 `MESH_X_DIM=4,MESH_Y_DIM=4,NOC_NUM_VC=1` → `check_mesh_within_flit` 不 raise。
- [ ] **Step 3: 加測試 — 超限 fail**
構造 `MESH_X_DIM=32`(>2^4)→ `check_mesh_within_flit` raise `AssertionError`。
- [ ] **Step 4: gate**
Run: `python3 specgen/tools/codegen.py --check && make check PYTHON3=python3`
Expected: drift exit 0(預設值合法);ctest 全綠。
- [ ] **Step 5: Commit**
```bash
git add specgen/ni_spec/invariants.py specgen/tests
git commit -m "feat(specgen): add L2 invariant binding mesh dims to flit dst_id/vc_id capacity"
```

---

### Task 3: run_regress.py run-count 斷言（S0 前置）

**Files:**
- Modify: `sim/run_regress.py`(加預期跑數,跑 0 視為 fail)

**Interfaces:**
- Produces:`run_regress.py` 在實跑數 < 預期(或 0)時回非零;接受 `--expect N` 或從 discoverable 變體數推導。

- [ ] **Step 1: 改 main() 末尾的判定**
把 `return 1 if fail else 0` 改成同時檢查 run-count:
```python
    print(f"\nco-sim regress: {run-fail}/{run} passed, {skip} skipped")
    if run == 0:
        print("FAIL: 0 scenarios ran (expected >=1) — vacuous pass guard"); return 1
    return 1 if fail else 0
```
- [ ] **Step 2: gate**
Run: `make build-verilator PYTHON3=python3 && make sim-regress PYTHON3=python3`
Expected: 仍印 `6/6 passed`(或當前變體數),return 0;若把 PATTERNS 指到空目錄則 return 1。
- [ ] **Step 3: Commit**
```bash
git add sim/run_regress.py
git commit -m "fix(sim): run_regress fails on zero scenarios run (non-vacuous gate)"
```

---

### Task 4: gen_tb_top.py 重現今日 2-node tb（S0）

**Files:**
- Create: `sim/tools/gen_tb_top.py`(generator + `--check` drift gate)
- Create: `sim/topologies/mesh_2x1.yaml`(描述今日拓樸:x_dim=2,y_dim=1,num_vc=1)
- Modify: `sim/sv/tb_top.sv` → 變成 **generated 產物**(內容由 generator 產,行為等價現況);加 generated header marker
- Modify: `sim/verilator/Makefile`、`sim/vcs/Makefile`(build tb_top 前先跑 `gen_tb_top.py`,如 `filelist.f` 模式);root `Makefile`(若有 lint hook)

**Interfaces:**
- Consumes:specgen 常數(`MESH_X_DIM`/`MESH_Y_DIM`/`NOC_NUM_VC`/`X_WIDTH`)、`sim/topologies/<name>.yaml`。
- Produces:`gen_tb_top.py --topology mesh_2x1 --out sim/sv/tb_top.sv`;`gen_tb_top.py --check`(重生到 scratch + diff committed,drift 回非零);產出 `tb_top.sv` 行為等價現況手寫版。

- [ ] **Step 1: 抽今日 tb_top.sv 的結構為 template 心智模型**
讀現 `sim/sv/tb_top.sv`,列出**可由拓樸推導**的部分(node instance、scenario plusarg、PASS guard master 數、PMU 命名、cross-link 接線)vs **常數樣板**(clk/rst 自鐘、DPI lifecycle)。記錄到 generator 註解。
- [ ] **Step 2: 寫 generator 骨架**
`sim/tools/gen_tb_top.py`:
```python
#!/usr/bin/env python3
"""Generate tb_top.sv (+ later noc_fabric) from a topology config + specgen constants.
S0 scope: reproduce today's 2-node single-VC tb behaviour. Generated artifact: edit the
generator/template, never the emitted .sv. `--check` regenerates to a scratch path and
diffs vs the committed .sv (drift gate)."""
import argparse, sys, difflib
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]

def load_topology(name):
    import yaml
    return yaml.safe_load((ROOT / "sim" / "topologies" / f"{name}.yaml").read_text())

def emit_tb_top(topo):
    x, y = topo["topology"]["x_dim"], topo["topology"]["y_dim"]
    nodes = [(i % x, i // x) for i in range(x * y)]
    # ... emit module header, per-node instances, neighbor link wiring,
    #     +scenario_node<i> plusargs, PASS guard = len(nodes) masters,
    #     per-node PMU/link monitor names. (S0: x=2,y=1 reproduces today.)
    return TEMPLATE_RENDER(nodes, topo)   # concrete render below in Step 3

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--topology", default="mesh_2x1")
    ap.add_argument("--out", default=str(ROOT / "sim/sv/tb_top.sv"))
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    text = emit_tb_top(load_topology(a.topology))
    if a.check:
        cur = Path(a.out).read_text()
        if cur != text:
            sys.stdout.writelines(difflib.unified_diff(
                cur.splitlines(True), text.splitlines(True), "committed", "regenerated"))
            print("DRIFT: tb_top.sv differs from generator output"); return 1
        return 0
    Path(a.out).write_text(text); return 0

if __name__ == "__main__":
    sys.exit(main())
```
- [ ] **Step 3: 實作 `TEMPLATE_RENDER` 至 byte-輸出今日行為**
逐段把現 `tb_top.sv` 轉成 f-string/template,參數化 node 迴圈。**目標是行為等價**(co-sim 6/6 不變),非與舊檔 byte-identical(舊檔有手寫配對)。先讓 `--out` 產生的檔通過 build + co-sim,再把該產物 commit 成新 `tb_top.sv`。
- [ ] **Step 4: Makefile 接 generator**
`sim/verilator/Makefile` 與 `sim/vcs/Makefile`:在 tb_top build 規則前加 prerequisite/recipe 跑 `python3 sim/tools/gen_tb_top.py --topology $(TOPOLOGY) --out sim/sv/tb_top.sv`(`TOPOLOGY ?= mesh_2x1`)。沿用 `filelist.f` 的 generated-artifact 模式。
- [ ] **Step 5: gate**
```bash
make clean-verilator PYTHON3=python3
make build-verilator PYTHON3=python3
make sim-regress PYTHON3=python3        # 6 全 PASS,run-count 斷言生效
python3 sim/tools/gen_tb_top.py --check  # exit 0(committed == regenerated)
```
Expected: co-sim 6 全 PASS;drift exit 0。
- [ ] **Step 6: Commit**
```bash
git add sim/tools/gen_tb_top.py sim/topologies/mesh_2x1.yaml sim/sv/tb_top.sv sim/verilator/Makefile sim/vcs/Makefile
git commit -m "feat(sim): generate tb_top.sv from topology config (S0: reproduce 2-node behaviour)"
```

---

### Task 5: scenario/config 接入 num_vc（S1）

**Files:**
- Modify: scenario/config schema(`sim/test_patterns/` 的 scenario YAML schema + loader;`sim/topologies/*.yaml` 的 `num_vc`)
- Modify: `sim/tools/gen_tb_top.py`(把 `num_vc` 透傳成 wrap 參數 override)
- Modify: `sim/sv/tb_top.sv`(generated:wrap 實例化帶 `NUM_VC` 參數,來源單一 = config)

**Interfaces:**
- Consumes:`topology.num_vc`(config)。
- Produces:generated `tb_top.sv` 的 wrap 實例化用 config 的 `num_vc` 設定 `NUM_VC` parameter(目前仍 =1,為 S2 鋪路);c_model NMU→NSU multi-VC 路徑已由 `test_request_response_loopback`/`test_router_loopback` 的 `MultiVc` instantiation(num_vc {2,4,8})涵蓋,本 task 不重證。

- [ ] **Step 1: 確認 c_model multi-VC scenario 路徑現況綠**
Run: `cd build/cmodel && ctest -R 'MultiVc|RequestResponse|RouterLoopback' --output-on-failure`
Expected: 全綠(基準;本 task 不改 c_model)。
- [ ] **Step 2: config 加 num_vc 並透傳**
`sim/topologies/mesh_2x1.yaml` 已有 `num_vc`(Task 4);在 `gen_tb_top.py` 的 render 把 `num_vc` 帶進每個 wrap 實例化的 `NUM_VC` parameter(取代散落的 `NOC_NUM_VC` 預設依賴),確保**單一來源**。
- [ ] **Step 3: gate(num_vc=1 維持行為)**
```bash
make build-verilator PYTHON3=python3 && make sim-regress PYTHON3=python3
python3 sim/tools/gen_tb_top.py --check
```
Expected: co-sim 6 全 PASS;drift exit 0(此時 num_vc 仍 1)。
- [ ] **Step 4: Commit**
```bash
git add sim/topologies sim/tools/gen_tb_top.py sim/sv/tb_top.sv
git commit -m "feat(sim): thread topology num_vc into generated tb wrap params (S1)"
```

---

### Task 6: wire-level multi-VC（S2）

**Files:**
- Modify: `sim/c/cmodel_dpi.h`、`sim/c/cmodel_dpi.cpp`:
  - `set_inputs`/`get_outputs` 的 credit 欄位 scalar → `[NUM_VC]`(三個 component)。
  - **create 簽章全部帶 `num_vc`**:`cmodel_router_create(name,x,y,mesh_x,mesh_y,num_vc)`、`cmodel_nmu_create(name,src_id,num_vc)`、`cmodel_nsu_create(name,src_id,num_vc)`(現皆無 num_vc;`cmodel_dpi.h:135,164`)。SV import/呼叫端同步。
- Modify: **`*_wrap_io.hpp` 的 credit struct scalar → per-VC**:`c_model/include/wrap/router_wrap_io.hpp:32,41,55,66`、`nmu_wrap_io.hpp:66,90`、`nsu_wrap_io.hpp:39,65`(註解已寫「pulse per VC」,現型別仍 scalar)。
- Modify: `sim/sv/router_wrap.sv`、`sim/sv/nmu_wrap.sv`、`sim/sv/nsu_wrap.sv`(拔 `NUM_VC!=1` 的 `$fatal`;credit marshalling 改 per-VC,不再 `{NUM_VC{...}}` 壓扁 / 取 `[0]`)
- Modify: `c_model/include/wrap/nmu_wrap.hpp:47`、`c_model/include/wrap/nsu_wrap.hpp:52`、`c_model/include/wrap/router_wrap.hpp`(`cfg.num_vc` 由 create 參數帶入,不寫死 1)
- Modify: `sim/run_regress.py`(傳 `+num_vc=` plusarg);`sim/sv/tb_top.sv`(generated:plusarg → wrap NUM_VC)
- Create: 一個 multi-VC co-sim driver scenario(於 `sim/test_patterns/`,Mode A read/write split)

**Interfaces:**
- Consumes:Task 5 的 config `num_vc`;`noc_intf.req/rsp_credit_return[NUM_VC]`(已 per-VC)。
- Produces:`cmodel_router_set_inputs/get_outputs` 的 credit 欄位為 `[NUM_VC]` 向量;三個 wrap 接受 `NUM_VC>1`;一個指名 scenario 在 num_vc=2 co-sim 綠。
> **DPI ABI 注意**:本 task 即把 router link 的 valid/flit/credit 全部設計成 `[PORT]` indexable 形(即使現只用 LOCAL+1 link),避免 S3 再改 signature(spec §5)。

- [ ] **Step 1: 隔離已知 burst bug(避免誤判)**
記錄基準:`make sim-regress` 現有 6 scenario(非 BUR/STR)綠。**不**把 BUR/STR co-sim 納入本 task gate(已知 wire-level burst bug,屬 SP2;見 spec §7)。multi-VC driver 選 Mode A read/write split 的非-burst scenario。
- [ ] **Step 2: 改 DPI signature 為 per-PORT × per-VC + create 帶 num_vc**
`cmodel_dpi.h`/`.cpp`:
  - `cmodel_router_set_inputs/get_outputs` 的 link **valid/flit/credit 全 `[PORT]` 化**,credit 再 `[NUM_VC]`(SV `bit [NUM_VC-1:0]` ↔ C++ `svBitVecVal*`)。NMU/NSU NI-face credit 同樣 `[NUM_VC]`。
  - create 簽章:`cmodel_router_create(name,x,y,mesh_x,mesh_y,num_vc)`、`cmodel_nmu_create(name,src_id,num_vc)`、`cmodel_nsu_create(name,src_id,num_vc)`。
  - `*_wrap_io.hpp`(router/nmu/nsu)的 credit 欄位 scalar → `std::array<.., NUM_VC>` 或 per-VC vector(對齊 c_model `credit_[port][vc]`)。
  先讓 C++ 端編譯通過(marshalling 搬 per-VC 全向量,wrap config `num_vc` 由 create 帶入)。
- [ ] **Step 3: 拔三個 $fatal + wrap num_vc 來源**
`router_wrap.sv`/`nmu_wrap.sv`/`nsu_wrap.sv` 移除 `if (NUM_VC!=1) $fatal`;credit 改逐 VC marshalling(不 `{NUM_VC{...}}`、不取 `[0]`)。`nmu_wrap.hpp`/`nsu_wrap.hpp`/`router_wrap.hpp` 的 `cfg.num_vc` 由 create 參數設。
- [ ] **Step 4: run_regress + tb 傳 num_vc**
`run_regress.py` 對 multi-VC scenario 加 `+num_vc=2`;generated `tb_top.sv` 把 plusarg 餵進 wrap `NUM_VC`。
- [ ] **Step 5: gate**
```bash
make clean-verilator PYTHON3=python3 && make check PYTHON3=python3   # ctest 全綠(C++ DPI 改動不破 unit)
make build-verilator PYTHON3=python3
make sim-regress PYTHON3=python3                                     # 原 6 綠 + 指名 multi-VC scenario num_vc=2 綠(run-count 斷言)
python3 sim/tools/gen_tb_top.py --check
```
Expected: ctest 全綠;co-sim 原 6 + multi-VC driver 全 PASS。
- [ ] **Step 6: Commit**
```bash
git add sim/c/cmodel_dpi.h sim/c/cmodel_dpi.cpp sim/sv/router_wrap.sv sim/sv/nmu_wrap.sv sim/sv/nsu_wrap.sv c_model/include/wrap/nmu_wrap.hpp c_model/include/wrap/nsu_wrap.hpp c_model/include/wrap/router_wrap.hpp c_model/include/wrap/router_wrap_io.hpp c_model/include/wrap/nmu_wrap_io.hpp c_model/include/wrap/nsu_wrap_io.hpp sim/run_regress.py sim/sv/tb_top.sv sim/test_patterns
git commit -m "feat(sim): wire-level multi-VC via per-PORT x per-VC DPI; drop single-VC fatals (S2)"
```

---

### Task 7: directional N/E/S/W ports + 2×2 mesh（S3）

**Files:**
- Modify: `sim/c/cmodel_dpi.cpp`、`c_model/include/wrap/router_wrap.hpp`(wire 全 5 port:`LOCAL`+`NORTH`/`EAST`/`SOUTH`/`WEST`,依 create 的 x/y/mesh 接 c_model router 的 directional adapter)
- Modify: `sim/sv/router_wrap.sv`(暴露 NORTH/EAST/SOUTH/WEST link bundle;沿用 S2 的 `[PORT]` ABI,不改 signature)
- Modify: `sim/tools/gen_tb_top.py`(產 `noc_fabric_<topo>.sv`:N node + 方向 link 連線 + 邊界 tie-off + assertion;`tb_top.sv` 改為 instantiate fabric)
- Modify: `sim/build_config.mk`(`TB_TOP_SV_SRC` 加 generated `noc_fabric_<topo>.sv`)或 `sim/gen_filelist.py`(把 fabric 納 SV source set);或 generated `tb_top.sv` `\`include` fabric。擇一,確保 fabric 進 Verilator/VCS 編譯。
- Create: `sim/topologies/mesh_2x2.yaml`

**Interfaces:**
- Consumes:S2 的 per-PORT DPI ABI;`Router` 的 5-port `RouterPort{LOCAL,NORTH,EAST,SOUTH,WEST}`(`router.hpp:30`)。
- Produces:`noc_fabric_<topo>.sv`(每 node 露 AXI port,ctx 由 port 收入,create 留 tb_top);2×2 mesh co-sim 綠。

- [ ] **Step 1: router_wrap 接全 5 port(C++)**
`router_wrap.hpp`:依 create 的 (x,y,mesh_x,mesh_y) 對存在的鄰居方向 `wire_port`,邊界方向不接(由 generator tie-off + assertion 保護)。
- [ ] **Step 2: SV router_wrap 暴露方向 bundle**
`router_wrap.sv` 加 N/E/S/W link port(沿用 S2 的 `[PORT]` 結構,signature 不變,只是 PORT 維度填滿)。
- [ ] **Step 3: generator 產 fabric + tie-off assertion**
`gen_tb_top.py` 產 `noc_fabric_<topo>.sv`:節點 grid、相鄰 router 對接、邊界未用方向 tie-off,並 emit assertion「未接方向 port 收到 valid flit 即 `$fatal`」(spec §4)。`tb_top.sv` 改成 instantiate fabric + 每 node 接 test master/slave,ctx 在 tb_top `cmodel_*_create` 後傳入 fabric port。
- [ ] **Step 4: gate(2×2)**
```bash
make build-verilator PYTHON3=python3 TOPOLOGY=mesh_2x2
make sim-regress PYTHON3=python3 TOPOLOGY=mesh_2x2     # 2×2 co-sim 綠(run-count 斷言)
python3 sim/tools/gen_tb_top.py --topology mesh_2x2 --check
```
Expected: 2×2 mesh co-sim PASS;drift exit 0。亦驗 `mesh_2x1` 仍綠(回歸)。
- [ ] **Step 5: Commit**
```bash
git add sim/c/cmodel_dpi.cpp c_model/include/wrap/router_wrap.hpp sim/sv/router_wrap.sv sim/tools/gen_tb_top.py sim/build_config.mk sim/topologies/mesh_2x2.yaml sim/sv/tb_top.sv sim/sv/noc_fabric_mesh_2x2.sv
git commit -m "feat(sim): directional N/E/S/W ports + generated noc_fabric; 2x2 mesh co-sim (S3)"
```

---

### Task 8: N×M parameterized + per-node scenario 指派（S4）

**Files:**
- Modify: `sim/tools/gen_coordinate_scenarios.py`(2-node `NODE1_OFFSET` 推廣為 per-node `node_id<<32`)
- Modify: `sim/tools/gen_tb_top.py`(任意 N×M:scenario_i → node i)
- Create: `sim/topologies/mesh_3x2.yaml`(或其他 N 或 M >2)

**Interfaces:**
- Consumes:`node_id = (y<<X_WIDTH)|x`;flit-binding invariant(Task 2)保證 N×M ≤ flit 容量。
- Produces:任意 N×M(在 flit 上限內)的 fabric + per-node scenario;N×M co-sim 綠。

- [ ] **Step 1: 推廣 scenario 定址**
`gen_coordinate_scenarios.py`:把寫死的 node0/node1 + `NODE1_OFFSET=0x100000000` 改成 per-node:`offset_i = node_id_i << 32`,`node_id_i = (y_i<<X_WIDTH)|x_i`。每 node 產 scenario 變體。
- [ ] **Step 2: generator 任意 N×M 指派**
`gen_tb_top.py`:node 迴圈用 `MESH_X_DIM×MESH_Y_DIM`,scenario_i 指到 node i,PASS guard = N×M master 數。
- [ ] **Step 3: gate(N×M,N 或 M >2)**
```bash
make build-verilator PYTHON3=python3 TOPOLOGY=mesh_3x2
make sim-regress PYTHON3=python3 TOPOLOGY=mesh_3x2     # 3×2 co-sim 綠(run-count 斷言)
python3 sim/tools/gen_tb_top.py --topology mesh_3x2 --check
```
Expected: 3×2 mesh co-sim PASS;mesh_2x1/2x2 回歸仍綠。
- [ ] **Step 4: 超限 config 防呆驗證**
構造 `MESH_X_DIM=32` 的 config → `codegen.py --check`(或 generator 前置驗證)fail,訊息指向 flit 上限。
- [ ] **Step 5: Commit**
```bash
git add sim/tools/gen_coordinate_scenarios.py sim/tools/gen_tb_top.py sim/topologies/mesh_3x2.yaml
git commit -m "feat(sim): parameterized NxM mesh + per-node scenario assignment (S4)"
```

---

## Self-Review(plan vs spec）

- **Spec §2 D1-D8**:D1/D3 → Task 4/7;D2 → Task 5/6;D4 → Task 4(`--check`);D5/D6 → Task 1/2;D7 → Task 7;D8 → Task 1。✅
- **Spec §3 flit binding**:Task 2 invariant(`X_WIDTH+Y_WIDTH=DST_ID_WIDTH`、`NUM_VC≤2^VC_ID_WIDTH`)。✅
- **Spec §5 DPI ABI 一次定型**:Task 6 Step 2(valid/flit/credit 全 `[PORT]`),Task 7 Step 2 沿用不改 signature。✅
- **Spec §6 staging gate**:S-1→Task 1/2;S0→Task 3/4;S1→Task 5;S2→Task 6;S3→Task 7;S4→Task 8;每 task 有 run-count 斷言 / drift / ctest gate。✅
- **Spec §7 blind-spots**:三 wrap $fatal(Task 6)、create ABI(Task 6)、edge tie-off assertion(Task 7 Step 3)、fabric 收 ctx port(Task 7 Step 3)、PASS guard/PMU 命名(Task 4/8)、per-node scenario(Task 8)、burst bug 隔離(Task 6 Step 1)。✅
- **Type 一致**:`cmodel_router_create(name,x,y,mesh_x,mesh_y,num_vc)`、`node_id=(y<<X_WIDTH)|x`、`NOC_*`/`AXI_*` 全 plan 一致。✅
- **已知不確定(非 placeholder,屬 TDD 執行時定案)**:Task 4 的 `TEMPLATE_RENDER` 確切 SV 文字、Task 6/7 的 DPI marshalling 逐行 — 以「行為等價 + gate 綠」為 done 準則,執行時對碼定案(SV/DPI 最終文字依賴前一 stage 落地後狀態,刻意不預先杜撰)。
