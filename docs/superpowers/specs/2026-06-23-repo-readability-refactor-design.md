# Repo Readability Refactor — Design

- Date: 2026-06-23
- Driver: 對外可讀性 + proprietary 交付(repo 要給 reviewer / 新人 / demo 看)
- Source map: `docs/issue/ARCHITECTURE.md` §6.1–§6.3
- Review: Codex GPT-5.5 (ordering + blast-radius + drift-gate coupling),已納入

## Scope

排序標準 = 對外可讀性,非架構正確性。內部實作美化(對 reviewer 零貢獻)排除。

**In scope**(9 步,見執行順序)
- LICENSE、移除 register_file、docs/ 清場
- 改名:`noc/`→`router/`、頂層 `cosim/`→`sim/`、C++ `shell`→`wrap`、`flit_link_perf_monitor.sv`→`link_perf_monitor.sv`
- 新增 `include/ni/` 收容共用 primitive
- 文件同步成最終實況

**Deferred**(不服務本次 driver)
- 抽測試鷹架(`NullNoc*` / `*Standalone`)出 `nmu.hpp` / `nsu.hpp`(§6.2 #4)
- §6.3 NMU/NSU 共用核心抽取(`port_params` / `vc_arbiter` / `depacketize` / `packetize`)

## Decisions

| 決策 | 結論 | 理由 |
|---|---|---|
| LICENSE | proprietary / internal-use-only | 只給特定 reviewer/部門;非開源 |
| vendored MIT attribution | 保留 `c_model/include/axi/ATTRIBUTION.md` | cocotbext-axi (MIT) 義務不變 |
| shell 改名用詞 | `wrap`(非 `wrapper`) | user 指定;SV 側本就 `_wrap`,兩側收斂 |
| namespace 政策 | namespace 跟 dir 改 | `ni::cmodel::noc`→`router`(75 處)、`cosim`→`wrap`(86 處);目錄/namespace 一致。`Noc*` 型別名仍依 §6.1 保留 |
| register_file | 本輪移除 | AI 擅自生成、production 未接線、user 未要求 |
| generated `ni_regs.*` 命運 | 本輪保留(不清 specgen) | 屬 specgen 管線,風險類別不同;留孤兒不影響 drift gate |
| §6.3 / 抽鷹架 | defer | 純內部,對外可讀性零收益 |

## 執行順序

每步獨立 commit;**每步 build + ctest 全綠才進下一步**(可 bisect)。順序經 Codex 校正:`ni/` 在 `noc→router` 之後搬(來源無歧義),`shell→wrap` 在 `cosim→sim` 之後(落最終 `sim/` 路徑),文件同步嚴格最後。

### 1. LICENSE(風險:零)
- **OUTPUT**:repo root `LICENSE`,proprietary / internal-use-only 條款。
- 保留 `c_model/include/axi/ATTRIBUTION.md`(vendored MIT)。
- 無 include / path 衝擊。

### 2. 移除 register_file(風險:低)
- **DELETE**:`c_model/include/register_file.hpp`、`c_model/src/register_file.cpp`、`c_model/tests/test_register_file.cpp`、`c_model/tests/CMakeLists.txt` 對應 entry(約 `:24-26`)。
- **驗證引用**:`register_file` 僅自身 header/source/test/CMake 引用,無 production 接線。
- **drift gate 安全**:drift gate(`c_model/CMakeLists.txt` 跑 `codegen.py --check`)比對「generated vs JSON」,不涉 register_file → 刪它不觸發 drift。
- **generated map 保留(已定案 2026-06-23)**:`register_file.cpp` 是 generated `ni_regs.h` / `ni_regs_pkg.sv`(`*_ACCESS` / `ni::regs::ALL_OFFSETS` / reset 常數)的唯一消費者。刪它後 generated register map 變孤兒,但**本輪不清 specgen**(連帶移除須改 JSON source + 重 codegen + 更新 `specgen/tests` goldens,屬另一風險類別)。孤兒 generated map 不影響 drift gate。

### 3. docs/ 清場(風險:零)
- **MOVE**:`docs/superpowers/` + `docs/_archive/` → `docs/internal/`。
- 對外 docs 只留 `architecture.md` / `development.md` / `performance-probe.md` / `perf-probe-report.md` 等。
- 檢查 cross-link URL 不斷(本 spec 自身亦隨之搬,屬預期)。

### 4. `noc/` → `router/`(風險:中;blast radius 大)
> 文件 §6.1 謊稱已完成,實際 **repo 從未 apply**(仍 `c_model/include/noc/`,介面檔仍 `noc_` 前綴)。
- **目錄**:`c_model/include/noc` → `router`;`c_model/tests/noc` → `router`。
- **介面檔去前綴**:`noc_req_in.hpp`→`req_in.hpp`、`noc_req_out`、`noc_rsp_in`、`noc_rsp_out`。
- **namespace 改、型別名不變**:`ni::cmodel::noc` → `ni::cmodel::router`(75 處);`NocReqOut` 等型別名依 §6.1 保持(結果為 `ni::cmodel::router::NocReqOut`)。
- **更新引用(blast radius,Codex 補充)**:約 27 處 `#include "noc/..."`;75 處 `ni::cmodel::noc` namespace;跨層 consumer(`nmu/nmu.hpp`、`nsu/nsu.hpp`、`packetize.hpp`、`router_shell_adapter.hpp`);`cosim/c/cmodel_dpi.cpp` perf 取樣;`tests/noc/CMakeLists.txt`;`specgen/tools/gen_inventory.py` 路徑。

### 8. 建 `c_model/include/ni/`(風險:低)
> step 4 之後執行 → 來源已是 `router/`,無「noc/ or router/」歧義。
- **MOVE**:`nmu/ni_stage.hpp`、`router/pipeline_stage.hpp`、`router/wormhole_arbiter.hpp` → `ni/`。
- **更新 consumer**:`nmu.hpp`、`nsu.hpp`、`depacketize.hpp`、`tests/common/test_ni_stage.cpp`、`tests/router/test_pipeline_stage.cpp`。

### 5. 頂層 `cosim/` → `sim/`(風險:中)
- **目錄**:`cosim/` → `sim/`。
- **更新 hardcoded 路徑(Codex 補充)**:`Makefile`(`COSIM_VERILATOR` / `COSIM_VCS` / `-C` target,約 `:17-18,110,156,159`);`c_model/tests/CMakeLists.txt:42`;`c_model/tests/cosim/CMakeLists.txt:32,49`(指向 `../cosim/c`);`cosim/sources.mk`(`COSIM_ROOT` 衍生,SV 來源清單);`docs/development.md:185-222`。

### 6. C++ `shell` → `wrap`(風險:中)
- **目錄**:`c_model/include/cosim/` → `c_model/include/wrap/`。
- **檔名**:`*_shell_adapter.hpp` → `*_wrap.hpp`;`*_shell_io.hpp` → `*_wrap_io.hpp`(6 元件 × 2)。
- **型別**:`*ShellAdapter` → `*Wrap`;`*ShellIo` → `*WrapIo`。
- **namespace**:`ni::cmodel::cosim` → `ni::cmodel::wrap`(86 處)。
- **範圍**:約 270 處 shell 識別字,18 source + 6 test 檔 + CMake + docs;另 86 處 namespace。
- **handle(Codex 補充)**:`handle_block.hpp`、`cmodel_dpi.cpp` include 與型別面。
- **不動**:SV 側 `*_wrap.sv`(本就 `_wrap`);DPI 匯出名 `cmodel_<元件>_<op>`(不含 shell/wrap)。step 5 先行 → 此步落於 `sim/c/...` 最終路徑。

### 7. `flit_link_perf_monitor.sv` → `link_perf_monitor.sv`(風險:小)
- **RENAME** + 更新引用:`sim/sources.mk`(原 `cosim/sources.mk:73`)、`sim/sv/tb_top.sv` 實例化(原 `:319-340`)。

### 9. 文件同步成最終實況(風險:零;嚴格最後)
- **UPDATE**:`docs/issue/ARCHITECTURE.md`(消除 §6.1 假象;§3.5/§3.6/§1 改名)、`docs/architecture.md`、`CLAUDE.md`。
- 反映:`router/`、`sim/`、`wrap/`、`ni/`、register_file 已移除。
- 放最後才不致把過渡狀態寫進文件。

## Success Criteria

- 每步 build + ctest 全綠(co-sim Verilator + VCS 路徑不破)。
- specgen drift gate 維持綠(step 2 不觸發)。
- repo root 有 proprietary LICENSE;`axi/ATTRIBUTION.md` 保留。
- `docs/` 對外目錄只剩對外文件;過程產物在 `docs/internal/`。
- `grep` 全 repo:無殘留 `noc/` include、無 `ni::cmodel::noc` / `ni::cmodel::cosim`、無 `shell`/`ShellAdapter` 識別字、無頂層 `cosim/` 路徑、無 `register_file`。
- 文件與 repo 結構一致(§6.1 假象消除)。
