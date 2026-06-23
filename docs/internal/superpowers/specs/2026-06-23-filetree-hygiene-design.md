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
- `sim/test_patterns/` → `sim/test_patterns/`;**刪頂層 `tests/`**(它只裝資料卻叫 tests)。
- 更新所有 consumer 的路徑(c_model tests 反向引用 `sim/test_patterns`,user 已接受此耦合):`c_model/tests/{axi,integration,wrap}/*`、`c_model/CMakeLists.txt`、`scenarios_list` 生成(原 `sim/test_patterns/CMakeLists.txt` + `scenarios_list.hpp.in` 隨之移至 `sim/test_patterns/`)、`tools/lint_scenarios.py` 路徑、root `Makefile`。

## ② File-tree:co-sim 移出 ctest
- 退役 `sim/tests/test_cosim_integration.cpp`(ctest 膠水)與 `sim/tests/`(命名把模擬當 tests)。
- 新增 **sim-owned 回歸 runner**:一支 script(`sim/run_regress.py` 或 `.sh`)**直接啟動 `Vtb_top` exe**(如 test_cosim_integration 用 `system()` 那樣,**繞開先前 make 的 `$(MAKE)` quirk**)、loop 驗證 subset scenario、檢查 `PASS: scenario complete, scoreboard clean`、回報 pass/fail;加薄 make target `sim-regress`。skip 邏輯(INF、無 committed node0/node1 變體)移植進 runner。
- `c_model/tests/CMakeLists.txt` 移除把 co-sim 接進 ctest 的 `add_subdirectory(.../sim/tests ...)`(原 :42)→ **ctest 變純 C++**。
- 結果樹:`c_model/tests/`(ctest)、`specgen/tests/`(ctest)、`sim/`(模擬 + test_patterns + run_regress)、頂層 `tests/` 刪除。

## ③ Source/config 分離(.f filelist)
> `sim/sources.mk` 現混了**兩種性質**:source 清單 + build 設定(BUILD_ROOT/DEPS_SRC/yaml-cpp/local.mk),且違反自身「ONLY file lists」註解。
- 抽出 **`.f` filelist**(`sim/filelist.f`):**只放** SV source 檔 + incdir(`+incdir+`)+ `+define+`(tool-native)。**make-evaluated / 條件式的東西不可進 .f**。
- 留 **build-config `.mk`**(`sim/build_config.mk`,原 sources.mk 去掉純 source 清單):`COSIM_ROOT`/`PROJ_ROOT`/`BUILD_ROOT`、`DEPS_SRC` 條件、`YAMLCPP_INC/LIB`、`STDCXXFS_LDFLAGS`、`CPP_INCLUDE_FLAGS`、`-include local.mk`(皆 make 條件式,`.f` 裝不了)。
- **路徑(Codex Important)**:`filelist.f` 用 **Make 產生的絕對路徑**(Verilator `-f`/`-F` 相對路徑語意特殊,絕對路徑兩個 sim 都穩);可由 Makefile 從變數 generate `.f`,或手寫絕對。落地前在 Verilator(`-f filelist.f --timing`)與 VCS dry-run 都驗過才刪 sources.mk。
- **genamba(Codex Important,別漏)**:`sources.mk` 同時定義 `GENAMBA_SV_SRC`/`GENAMBA_TESTER_SV_SRC`/`GENAMBA_INC_DEPS`/`DPI_HDR_DEPS`/`GENAMBA_DEFINES`(兩 Makefile 都用)。拆解時 genamba 的清單**要嘛各自 `.f`,要嘛留進 `build_config.mk`** —— 以不破 genamba build 為界,不可只搬 tb_top 而讓 genamba 變數消失。
- `sim/verilator/Makefile`、`sim/vcs/Makefile`:`include ../build_config.mk` + tb_top build 改用 `-f ../filelist.f`。

## ④ 減法
- **ni_regs 清理(已驗證死碼)**:`ni_regs_pkg.sv` 無人 import、不在 build list;`ni_regs.h` 僅 `ni_spec.hpp:7` 一行 dead-weight include、無符號使用者(register_file 移除後)。動作:
  - 移除 `c_model/include/ni_spec.hpp:7` 的 `#include "ni_regs.h"`。(ni_spec.hpp 目前唯一 live includer = `c_model/tests/test_pins_smoke.cpp:1`,確認移除後它仍編。)
  - **specgen registers domain 整個拔除(Codex Important — 範圍比「刪檔+golden」廣)**:
    - `specgen/tools/codegen.py`:從 `DOMAIN_TO_EMITTER`(:32-45)與 CLI choices(:219)移除 `"registers"`。
    - 移除 register 來源定義 → 重 codegen,使 generated `ni_regs.h`/`ni_regs_pkg.sv`/`json/ni_registers.json`+`.schema.json` 不再產出。
    - 移除 goldens(`specgen/tests/golden/ni_regs.h.golden`、`ni_regs_pkg.sv.golden`)+ 測試中的 register 迴圈/案例(`test_codegen.py:72,94,106`、`test_codegen_sv.py:166,208,212,278`、`test_byte_identical_golden.py:67,83`、`test_registers_validator.py`)。
    - `test_function_blocks.py:69-74` 載 `ni_registers.json` 的 xref:`noc_function_blocks.json` 的 `configured_by` 目前**全空**,故移除該 xref 低風險(連 `invariants.py` L2 register-exist 檢查一併處理)。
    - drift gate(`codegen.py --check`)+ byte-identical golden 測試維持綠。
- **深層 dead-code scan**:file 層已乾淨(0 orphan header / 0 漏 build .cpp),本步掃 **function/include 層**未用碼(plan task;有則清、無則記錄)。
- **local untracked scratch**:`rm` 掉 `cross-review/`、`dpi_ref/`、`issue/`、`Testing/`、root `master_wrap_read_dump*.txt`、`docs/*.pptx`/`~$*`/`.pdf` 等(**僅動本地工作目錄,皆 gitignored,不進 commit**)。

## ⑤ Consumer 更新清單(必做 — Codex 精確化的 live 引用)
scenario 路徑:`c_model/CMakeLists.txt:64`、`c_model/tests/axi/CMakeLists.txt:13,22,28`、`c_model/tests/integration/CMakeLists.txt:7,9,24,26,39,41`、`c_model/tests/wrap/CMakeLists.txt:56`、`sim/verilator/Makefile:108,212`、`sim/vcs/Makefile:114,142,185`、`tools/lint_scenarios.py:2,85`、root `Makefile:139`、**`README.md:25`**、scenarios_list 生成(`sim/test_patterns/CMakeLists.txt`+`scenarios_list.hpp.in` 隨資料移至 `sim/test_patterns/`)。
ctest 解鉤:`c_model/tests/CMakeLists.txt:31-41`(移除 cosim `add_subdirectory`);ctest 註冊點在 `sim/tests/CMakeLists.txt:53`(隨 sim/tests 退役)。注意 `sim/c/cmodel_dpi.cpp` 仍由 `c_model/tests/wrap/CMakeLists.txt:30` 當**純 C++ unit** 編(留著,不受影響)。

## 執行順序(Codex 建議)
1. 搬 scenario + 更新 consumers。
2. co-sim 移出 ctest + 加 sim runner。
3. 拆 source/config(`filelist.f` + `build_config.mk`,**含 genamba 清單**)。
4. 移除 ni_regs + 跑 codegen/drift/test gate。
每步 build + 對應 test 綠才下一步。

## Success Criteria
- **ctest 純 C++ 全綠**(c_model + specgen;不再含 co-sim entry)。
- `make sim-regress`(新 runner)跑 6 個驗證 subset scenario **全 PASS**(行為與現況一致)。
- Verilator(`-f filelist.f --timing`)與 VCS dry-run 皆從 `.f` 取 source、build/解析成功。
- specgen drift gate 綠;`ni_regs.*` / goldens / register validator 已移除;repo 無 `ni_regs` 引用。
- 無頂層 `tests/`、無 `sim/tests/test_cosim_integration.cpp`、無 `sources.mk`(已拆成 `filelist.f` + `build_config.mk`)。
- local scratch 已清(工作目錄)。
- `grep` 全 repo:無殘留 `sim/test_patterns`、`test_cosim_integration`、`ni_regs`、舊 `sources.mk` 路徑。
