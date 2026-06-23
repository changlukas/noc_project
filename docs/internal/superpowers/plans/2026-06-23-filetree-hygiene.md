# File-Tree Hygiene + Subtraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 系統性 file-tree 重排 + 減法,落實「測試(ctest=純 C++)≠ 模擬(sim/,自有 runner)」:scenario 資料移進 sim/、co-sim 移出 ctest、`sources.mk` 拆 `.f`+config、拔死碼 ni_regs。

**Architecture:** 行為保留的結構重構。既有 c_model ctest(純 C++)+ 新 `sim/run_regress.py`(co-sim 回歸)+ specgen drift gate 為三層回歸網。每 task 機械改動 → 對應 gate 綠 → commit。

**Tech Stack:** CMake/GoogleTest(c_model)、Python(specgen + run_regress)、SystemVerilog/Verilator(`--timing`)/VCS、Git Bash + GNU make。

## Global Constraints
- **python**:`PYTHON3=python3`(mingw64),**不用 `py -3`**。
- **執行序固定**(Codex):1 scenario 搬移 → 2 co-sim 出 ctest → 3 source/config 拆 → 4 ni_regs 拔 → 5 dead-code scan + local scratch。
- **每 task gate** 綠才下一步;觸及 SV/build 用 `make check PYTHON3=python3`(stale 則先 `make clean-verilator`);specgen 改動跑 `specgen` pytest + drift gate。
- **commit 格式**:`type(scope): description`(English)。
- **不 push**;feature branch `refactor/filetree-hygiene`(已建,off main a81322c)。
- **`.f` 只放 SV 檔+incdir+define**;make 條件式留 `build_config.mk`;`filelist.f` 用**絕對路徑**(Verilator -f 相對語意特殊)。
- **genamba 清單不可遺失**(GENAMBA_* 變數)。
- **SURGICAL**:純搬移/改名/刪除 + 必要 consumer 更新;不改邏輯。
- **不碰**(→SP2):functional/code coverage、CRV、co-sim burst bug、protocol SVA。
- spec:`docs/internal/superpowers/specs/2026-06-23-filetree-hygiene-design.md`。

**基準**:開工前 `make check PYTHON3=python3` 全綠(544/544)。

---

### Task 1: scenario 資料移進 sim/test_patterns + 刪頂層 tests/

**Files:**
- Move: `tests/scenarios/` → `sim/test_patterns/`(含 `scenarios_list.hpp.in`、`scenario_helpers.hpp`、`CMakeLists.txt`、49 個 `AX4-*/`)
- Delete: 頂層 `tests/`(空後)
- Modify (path 更新):`c_model/CMakeLists.txt:64`、`c_model/tests/axi/CMakeLists.txt:13,22,28`、`c_model/tests/integration/CMakeLists.txt:7,9,24,26,39,41`、`c_model/tests/wrap/CMakeLists.txt:56`、`sim/verilator/Makefile:108,212`、`sim/vcs/Makefile:114,142,185`、`tools/lint_scenarios.py:2,85`、root `Makefile:139`、`README.md:25`、`sim/tests/CMakeLists.txt`(SCENARIO_TREE 路徑,本檔下個 task 才退役,先更新路徑保綠)

**Interfaces:** Produces:scenario 資料新根 = `sim/test_patterns/`;`scenarios_list.hpp` 由 `sim/test_patterns/CMakeLists.txt` 生成(glob `AX4-*`)。

- [ ] **Step 1: git mv 資料樹**
```bash
git mv tests/scenarios sim/test_patterns
rmdir tests 2>/dev/null || true   # 應已空
[ -d tests ] && echo "WARN: tests/ 非空:" && ls tests || echo "tests/ removed"
```
- [ ] **Step 2: 全 repo 改路徑引用**
```bash
for f in $(grep -rIl 'tests/scenarios' --include='*.txt' --include='*.cmake' --include='*.in' --include='*.py' --include='Makefile' --include='*.mk' --include='*.md' --include='*.cpp' --include='*.hpp' --exclude-dir=build --exclude-dir=.git .); do
  sed -i 's@tests/scenarios@sim/test_patterns@g' "$f"
done
```
- [ ] **Step 3: 殘留掃描**
```bash
grep -rIn 'tests/scenarios' --exclude-dir=build --exclude-dir=.git . | grep -v 'docs/internal\|\.superpowers'
```
Expected: 空(歷史 docs 不算)。
- [ ] **Step 4: gate**
Run: `make check PYTHON3=python3`(stale 則先 `make clean-verilator`)
Expected: ctest 544/544(scenario 由新路徑解析);lint_scenarios 綠。
- [ ] **Step 5: Commit**
```bash
git add -A
git commit -m "refactor: move tests/scenarios to sim/test_patterns; delete top-level tests/"
```

---

### Task 2: co-sim 移出 ctest → sim/run_regress.py

**Files:**
- Create: `sim/run_regress.py`(co-sim 回歸 runner)
- Modify: root `Makefile`(加 `sim-regress` target);`c_model/tests/CMakeLists.txt:31-41`(移除 cosim `add_subdirectory`)
- Delete: `sim/tests/test_cosim_integration.cpp`、`sim/tests/CMakeLists.txt`、`sim/tests/`(整個)

**Interfaces:**
- Produces:`python3 sim/run_regress.py` loop 驗證 subset、檢查 PASS marker、回 nonzero on fail;`make sim-regress` 包它。
- Consumes:Task 1 的 `sim/test_patterns/`;既有 `Vtb_top` exe + `sim/tools/gen_coordinate_scenarios.py`。

- [ ] **Step 1: 寫 `sim/run_regress.py`**(移植 test_cosim_integration.cpp 的 skip+check 邏輯)
```python
#!/usr/bin/env python3
"""Co-sim regression runner. Launches Vtb_top per validated scenario, checks PASS.
Replaces the retired ctest test_cosim_integration.cpp (sim != test: co-sim is a
simulation regression, run here, not in the C++ ctest suite)."""
import subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent          # repo root
PATTERNS = ROOT / "sim" / "test_patterns"
EXE = next((ROOT / "build/verilator").rglob("Vtb_top.exe"), None) \
      or next((ROOT / "build/verilator").rglob("Vtb_top"), None)
GEN = ROOT / "sim" / "tools" / "gen_coordinate_scenarios.py"
PASS = "PASS: scenario complete, scoreboard clean"

def main():
    if EXE is None:
        print("Vtb_top not built — run `make build-verilator` first"); return 2
    scns = sorted(p.name for p in PATTERNS.glob("AX4-*") if p.is_dir())
    run = fail = skip = 0
    for scn in scns:
        d = PATTERNS / scn
        # skip rules ported from test_cosim_integration.cpp:
        if scn.startswith("AX4-INF-"):            skip += 1; continue   # dedicated test
        n0 = d / "node0" / "scenario.yaml"; n1 = d / "node1" / "scenario.yaml"
        if not (n0.exists() and n1.exists()):     skip += 1; continue   # not in bidirectional subset
        run += 1
        out = (PATTERNS.parent / "verilator" / "output" / scn); out.mkdir(parents=True, exist_ok=True)
        r = subprocess.run([str(EXE),
            f"+scenario_node0={n0}", f"+scenario_node1={n1}",
            f"+perf_out={out/'perf.json'}", f"+perf_scenario={scn}"],
            capture_output=True, text=True, cwd=str(PATTERNS.parent / "verilator"))
        ok = r.returncode == 0 and PASS in r.stdout
        print(f"{'PASS' if ok else 'FAIL'}  {scn}")
        if not ok:
            fail += 1
            sys.stdout.write(r.stdout[-800:] + r.stderr[-400:])
    print(f"\nco-sim regress: {run-fail}/{run} passed, {skip} skipped")
    return 1 if fail else 0

if __name__ == "__main__":
    sys.exit(main())
```
> 用 committed `node0/node1` 變體(與 ctest 同);若日後要 on-the-fly gen,呼叫 `GEN`。先對齊現況(只跑有變體的 6 個)。
- [ ] **Step 2: root `Makefile` 加 target**
```make
.PHONY: sim-regress
sim-regress: build-verilator
	$(PYTHON3) sim/run_regress.py
```
- [ ] **Step 3: 移除 ctest 的 cosim 鉤 + 退役 sim/tests**
編輯 `c_model/tests/CMakeLists.txt` 移除 `:31-41` 的 cosim `add_subdirectory(${CMAKE_SOURCE_DIR}/../sim/tests ...)` 區塊(連同 `if(EXISTS Vtb_top)` 條件)。
```bash
git rm -r sim/tests
```
- [ ] **Step 4: gate**
```bash
make check PYTHON3=python3        # ctest 現為純 C++(無 cosim entry),應綠;數量會少掉 cosim 那幾條
make build-verilator PYTHON3=python3 && make sim-regress PYTHON3=python3
```
Expected: ctest 全綠(純 C++);`sim-regress` 印 `6/6 passed`(或當前有變體數)。
- [ ] **Step 5: Commit**
```bash
git add -A
git commit -m "refactor: move co-sim regression out of ctest into sim/run_regress.py (sim != test)"
```

---

### Task 3: 拆 sources.mk → filelist.f + build_config.mk

**Files:**
- Create: `sim/filelist.f`(tb_top SV filelist,絕對路徑)、`sim/build_config.mk`(make 設定 + genamba 清單)
- Delete: `sim/sources.mk`
- Modify: `sim/verilator/Makefile`(include build_config.mk;tb_top build 用 `-f filelist.f`)、`sim/vcs/Makefile`(同)

**Interfaces:** Produces:`sim/filelist.f`(SV 檔+incdir+define);`sim/build_config.mk`(`COSIM_ROOT`/`PROJ_ROOT`/`BUILD_ROOT`/`DEPS_SRC`/`YAMLCPP_*`/`STDCXXFS_LDFLAGS`/`CPP_INCLUDE_FLAGS`/`GENAMBA_*`/`DPI_HDR_DEPS`/`-include local.mk`)。

- [ ] **Step 1: 建 `sim/build_config.mk`** = 現 `sources.mk` **去掉純 tb_top SV 清單**後的全部(保留所有 make 變數/條件式 + genamba 清單 `GENAMBA_SV_SRC`/`GENAMBA_TESTER_SV_SRC`/`GENAMBA_INC_DEPS`/`DPI_HDR_DEPS`/`GENAMBA_DEFINES`)。`COSIM_ROOT` 改由本檔位置推導(同原邏輯)。
- [ ] **Step 2: 建 `sim/filelist.f`**(由 Makefile 從 `TB_TOP_SV_SRC` 變數 generate 成絕對路徑,或手寫絕對路徑 filelist):`+incdir+` 兩個 SPECGEN_SV_INC + 10 個 SV 檔(`ni_params_pkg.sv`…`tb_top.sv`,絕對路徑)。
> 建議:在 `build_config.mk` 留 `TB_TOP_SV_SRC` 變數,Makefile recipe `printf '%s\n' $(TB_TOP_SV_SRC) > filelist.f`(自動絕對化)→ 避免手寫漂移。
- [ ] **Step 3: 改兩個 Makefile**:`include ../sources.mk` → `include ../build_config.mk`;tb_top verilate 由 `$(SV_SRC)` 改 `-f $(COSIM_ROOT)/filelist.f`(verilator 與 vcs)。genamba target 仍用 `build_config.mk` 的 `GENAMBA_*` 變數(不動)。
```bash
git rm sim/sources.mk
```
- [ ] **Step 4: gate(含 VCS dry-run + genamba 變數存在)**
```bash
make clean-verilator PYTHON3=python3 && make check PYTHON3=python3   # verilator -f filelist.f --timing build + ctest
cd sim/vcs && make -n tb_top PYTHON3=python3; cd - >/dev/null         # VCS 從 -f 取 source,路徑解析
make sim-regress PYTHON3=python3                                      # 6/6 仍 PASS
make -n -C sim/verilator genamba PYTHON3=python3 2>&1 | grep -q 'GENAMBA\|Vtb_genamba' && echo "genamba vars OK"
```
Expected: 全綠;`.f` 被兩個 sim 吃到;genamba 變數未消失。
- [ ] **Step 5: Commit**
```bash
git add -A
git commit -m "refactor(build): split sources.mk into filelist.f filelist + build_config.mk"
```

---

### Task 4: 拔死碼 ni_regs(含 specgen registers domain)

**Files:**
- Modify: `c_model/include/ni_spec.hpp:7`(移除 `#include "ni_regs.h"`)、`specgen/tools/codegen.py`(DOMAIN_TO_EMITTER :32-45 + CLI :219)、`specgen/ni_spec/invariants.py`(L2 register-exist 檢查 :277-287)、`specgen/source/*`(register 來源定義)、`specgen/tests/{test_codegen.py,test_codegen_sv.py,test_byte_identical_golden.py,test_function_blocks.py,test_registers_validator.py}`
- Delete: generated `specgen/generated/cpp/ni_regs.h`、`sv/ni_regs_pkg.sv`、`json/ni_registers.json`、`json/ni_registers.schema.json`;goldens `specgen/tests/golden/ni_regs.h.golden`、`ni_regs_pkg.sv.golden`

**Interfaces:** Produces:specgen 不再有 registers domain;`ni_regs*` 全消失;drift gate 仍綠。

- [ ] **Step 1: 移除 ni_spec 的死 include**
`c_model/include/ni_spec.hpp:7` 刪 `#include "ni_regs.h"` 行(及該行尾註解)。
- [ ] **Step 2: specgen 拔 registers domain**
- `codegen.py`:`DOMAIN_TO_EMITTER` 移除 `"registers"` entry;CLI argparse `choices` 移除 `registers`。
- 移除 register 來源(spec source 中定義 registers 的部分)。
- `invariants.py:277-287`:移除 L2「configured_by register 必須存在於 ni_registers.json」檢查(configured_by 全空,無損)。
- `test_function_blocks.py:69-74`:移除載 `ni_registers.json` 的 xref。
- `test_codegen.py:72,94,106`、`test_codegen_sv.py:166,208,212,278`:移除 register 案例/迴圈。
- `test_byte_identical_golden.py:67,83`:移除 ni_regs golden 案例。
- 刪 `test_registers_validator.py`。
- [ ] **Step 3: 刪 generated + goldens + 重生**
```bash
git rm specgen/generated/cpp/ni_regs.h specgen/generated/sv/ni_regs_pkg.sv \
       specgen/generated/json/ni_registers.json specgen/generated/json/ni_registers.schema.json \
       specgen/tests/golden/ni_regs.h.golden specgen/tests/golden/ni_regs_pkg.sv.golden
python3 specgen/tools/codegen.py            # 重生(不應再吐 ni_regs*)
python3 specgen/tools/codegen.py --check    # drift gate
```
- [ ] **Step 4: 殘留掃描 + gate**
```bash
grep -rIn 'ni_regs\|ni::regs\|ni_registers\|csr_policy' --exclude-dir=build --exclude-dir=.git . | grep -v 'docs/internal\|\.superpowers'
```
Expected: 空。
```bash
cd specgen && python3 -m pytest -q; cd - >/dev/null    # specgen 測試綠(register 案例已移除)
make check PYTHON3=python3                              # c_model ctest 綠(ni_spec 仍編)
```
- [ ] **Step 5: Commit**
```bash
git add -A
git commit -m "refactor: remove dead ni_regs (drop ni_spec include + specgen registers domain)"
```

---

### Task 5: 深層 dead-code scan + 清 local scratch

**Files:** 視 scan 結果而定;`(本地)` untracked scratch

- [ ] **Step 1: function/include 層 dead-code scan**
```bash
# include 了卻沒用到符號的 header(粗篩,逐一人工確認)
# 未被任何 target 連結的 .cpp / 未被 reference 的 .hpp
grep -rIl '' c_model/include --include='*.hpp' | while read h; do :; done   # (用實際工具/人工判讀)
```
逐一確認候選(有就刪、附理由;**不確定就不刪、記錄**)。file 層已知乾淨(0 orphan),預期 yield 低。
- [ ] **Step 2: 清 local untracked scratch(僅本地、皆 gitignored)**
```bash
rm -rf cross-review dpi_ref issue Testing
rm -f master_wrap_read_dump_master_*.txt
rm -f "docs/20260622週報.pptx" "docs/~\$20260622週報.pptx" docs/pg037_axi_perf_mon.pdf 2>/dev/null || true
git status --porcelain | grep -v '^?? docs/issue/' | head   # 確認沒誤刪 tracked
```
> 這些不在 commit(gitignored);若 user 要保留某些,跳過該行。
- [ ] **Step 3: gate**
Run: `make check PYTHON3=python3` + `make sim-regress PYTHON3=python3`
Expected: 全綠(刪 scratch 不影響);若 Step 1 有刪碼,測試證明無回歸。
- [ ] **Step 4: Commit**(若 Step 1 有 tracked 改動;scratch 清理無 commit)
```bash
git add -A
git commit -m "refactor: remove dead code found in function/include-level scan"   # 若無則跳過
```

---

## Self-Review(plan vs spec)
- spec ① ↔ Task 1;② ↔ Task 2;③ ↔ Task 3;④ ↔ Task 4;深層 scan + scratch ↔ Task 5。✅
- Codex Important 全覆蓋:genamba 清單(T3 build_config.mk)、.f 只放 SV + 絕對路徑(T3)、specgen 全域拔(T4 含 codegen.py/invariants/各 test)、consumer 精確行(T1/T2)、執行順序(1→2→3→4→5)。✅
- Out-of-scope(coverage/CRV/burst/SVA)未出現於任何 task。✅
- No placeholder:run_regress.py 給完整骨架;specgen 拔除點具名;scan task 明確「不確定不刪」。
- 型別/路徑一致:`sim/test_patterns`、`Vtb_top`、`sim-regress`、`filelist.f`/`build_config.mk` 跨 task 一致。
