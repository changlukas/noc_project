# File-Tree Hygiene + Subtraction — Design

- Date: 2026-06-23
- Driver: 系統性 file-tree 重排 + 減法,落實「**測試 ≠ 模擬**」分離。
- Survey basis: 業界 coverage-driven verification 把「測軟體(unit test)」與「跑模擬驗證」分樹分 runner(Ibex `dv/`、cocotb 自有 runner);[Doulos CDV] / [Ibex] / [cocotb]。本輪只做**結構/減法**;functional coverage、CRV、wire-side assertion 等**方法學補強另開一輪(SP2)**。
- Out of scope (→ SP2):functional/code coverage、constrained-random、co-sim burst bug(BUR/STR 在 co-sim fail,c_model 層 PASS,屬 wire-level co-sim bug)、protocol SVA。

## 核心原則
**「tests」只指軟體單元測試**;模擬(co-sim)自成 sim/ 樹、用自己的 runner、不進 ctest。
- ctest 名冊 = **純 C++**(`c_model/tests/` + `specgen/tests/`)。
- co-sim = `sim/` 的模擬,由 sim-owned regress runner 跑。

## ① File-tree:scenario 資料搬家
- `tests/scenarios/` → `sim/test_patterns/`;**刪頂層 `tests/`**(它只裝資料卻叫 tests)。
- 更新所有 consumer 的路徑(c_model tests 反向引用 `sim/test_patterns`,user 已接受此耦合):`c_model/tests/{axi,integration,wrap}/*`、`c_model/CMakeLists.txt`、`scenarios_list` 生成(原 `tests/scenarios/CMakeLists.txt` + `scenarios_list.hpp.in` 隨之移至 `sim/test_patterns/`)、`tools/lint_scenarios.py` 路徑、root `Makefile`。

## ② File-tree:co-sim 移出 ctest
- 退役 `sim/tests/test_cosim_integration.cpp`(ctest 膠水)與 `sim/tests/`(命名把模擬當 tests)。
- 新增 **sim-owned 回歸 runner**:一支 script(`sim/run_regress.py` 或 `.sh`)**直接啟動 `Vtb_top` exe**(如 test_cosim_integration 用 `system()` 那樣,**繞開先前 make 的 `$(MAKE)` quirk**)、loop 驗證 subset scenario、檢查 `PASS: scenario complete, scoreboard clean`、回報 pass/fail;加薄 make target `sim-regress`。skip 邏輯(INF、無 committed node0/node1 變體)移植進 runner。
- `c_model/tests/CMakeLists.txt` 移除把 co-sim 接進 ctest 的 `add_subdirectory(.../sim/tests ...)`(原 :42)→ **ctest 變純 C++**。
- 結果樹:`c_model/tests/`(ctest)、`specgen/tests/`(ctest)、`sim/`(模擬 + test_patterns + run_regress)、頂層 `tests/` 刪除。

## ③ Source/config 分離(.f filelist)
> `sim/sources.mk` 現混了**兩種性質**:source 清單 + build 設定(BUILD_ROOT/DEPS_SRC/yaml-cpp/local.mk),且違反自身「ONLY file lists」註解。
- 抽出 **`.f` filelist**(`sim/tb_top.f`):純 SV source 清單 + incdir(`+incdir+`)+ `+define+`(tool-native、可攜)。
- 留 **build-config `.mk`**(`sim/build_config.mk`,原 sources.mk 去掉 source 清單):BUILD_ROOT / DEPS_SRC / yaml-cpp 路徑 / COSIM_ROOT / `-include local.mk`。
- `sim/verilator/Makefile`、`sim/vcs/Makefile`:`include ../build_config.mk` + 改用 `-f ../tb_top.f`(Verilator 與 VCS 皆原生支援 `-f`)。
- genamba 的 source list 同法處理(或維持,視其是否共用 `sources.mk` 變數;以不破 genamba build 為界)。

## ④ 減法
- **ni_regs 清理(已驗證死碼)**:`ni_regs_pkg.sv` 無人 import、不在 build list;`ni_regs.h` 僅 `ni_spec.hpp:7` 一行 dead-weight include、無符號使用者(register_file 移除後)。動作:
  - 移除 `c_model/include/ni_spec.hpp:7` 的 `#include "ni_regs.h"`。
  - **specgen**:從 spec source 移除 register 定義 → 重 codegen,使 generated `ni_regs.h` / `ni_regs_pkg.sv` / `json/ni_registers*.json` 不再產出;移除對應 goldens(`specgen/tests/golden/ni_regs.h.golden`、`ni_regs_pkg.sv.golden`)與 register validator 測試(`test_registers_validator.py` 及 `test_codegen*` 的 register 部分);drift gate 維持綠。
- **深層 dead-code scan**:file 層已乾淨(0 orphan header / 0 漏 build .cpp),本步掃 **function/include 層**未用碼(plan task;有則清、無則記錄)。
- **local untracked scratch**:`rm` 掉 `cross-review/`、`dpi_ref/`、`issue/`、`Testing/`、root `master_wrap_read_dump*.txt`、`docs/*.pptx`/`~$*`/`.pdf` 等(**僅動本地工作目錄,皆 gitignored,不進 commit**)。

## ⑤ Consumer 更新清單(必做)
`c_model/tests/{axi,integration,wrap}/*`(scenario 路徑)、`c_model/CMakeLists.txt`、`c_model/tests/CMakeLists.txt`(移除 cosim subdir)、`tools/lint_scenarios.py`、root `Makefile`、`sim/verilator/Makefile`、`sim/vcs/Makefile`、`sim/build_config.mk`/`sim/tb_top.f`、scenarios_list 生成、`ni_spec.hpp`、specgen register pipeline。

## Success Criteria
- **ctest 純 C++ 全綠**(c_model + specgen;不再含 co-sim entry)。
- `make sim-regress`(新 runner)跑 6 個驗證 subset scenario **全 PASS**(行為與現況一致)。
- Verilator(`-f tb_top.f --timing`)與 VCS dry-run 皆從 `.f` 取 source、build/解析成功。
- specgen drift gate 綠;`ni_regs.*` / goldens / register validator 已移除;repo 無 `ni_regs` 引用。
- 無頂層 `tests/`、無 `sim/tests/test_cosim_integration.cpp`、無 `sources.mk`(已拆成 `tb_top.f` + `build_config.mk`)。
- local scratch 已清(工作目錄)。
- `grep` 全 repo:無殘留 `tests/scenarios`、`test_cosim_integration`、`ni_regs`、舊 `sources.mk` 路徑。
