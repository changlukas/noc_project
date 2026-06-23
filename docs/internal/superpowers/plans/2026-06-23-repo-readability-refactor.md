# Repo Readability Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 noc_project repo 整理成可對外交付狀態(proprietary):補 LICENSE、移除 AI 擅自生成的 register_file、清 docs 門面、四組改名(`noc→router`、`cosim→sim`、`shell→wrap`、`flit_link_perf_monitor`)、抽共用 primitive 進 `ni/`、文件同步實況。

**Architecture:** 純結構重構,無新邏輯。既有 ctest + co-sim suite 為回歸網;每個 task 機械改動後,build + 全 ctest 維持綠才 commit。改名一律 `git mv` + `sed` 批次改引用 + grep 殘留 + clang-format。

**Tech Stack:** C++17 / CMake / GoogleTest / Verilator co-sim;Git Bash + GNU sed/grep。

## Global Constraints

- **build/test python**:一律 `PYTHON3=python3`(mingw64),**不用 `py -3`**(py -3 會自動下載污染 verilator generated .cpp)。
- **每步 gate**:該 task 的 build + 對應 test 全綠才能 commit、才進下一 task。c_model-only 用 `make test PYTHON3=python3`;觸及 SV/DPI/build 路徑用 `make check PYTHON3=python3`(含 lint + build-verilator)。
- **執行順序固定**:1 → 2 → 3 → 4 → 8 → 5 → 6 → 7 → 9(本檔 task 已照此排)。
- **commit 格式**:`type(scope): description`(English);type ∈ feat/fix/docs/style/refactor/test/chore/perf/build/revert。
- **clang-format**:每個被 sed 改過的 `.hpp`/`.cpp` 跑 `clang-format -i`(repo root `.clang-format`,Google base + IndentWidth 4)。
- **禁止**:`--no-verify`、停用 test、commit 非編譯碼。
- **grep 一律排除 build 與 .git**:`--exclude-dir=build --exclude-dir=.git`。
- **不 push**:全部停在 local,user 明示才 push。
- **型別名 `Noc*` 保留**(§6.1);namespace 跟 dir 改(`noc→router`、`cosim→wrap`)。
- **不碰**:specgen generated `ni_regs.*`(register_file 移除後留孤兒,不影響 drift gate);§6.3 共用核心抽取;`NullNoc*`/`*Standalone` �bench 鷹架。

**基準確認(開工前一次性)**:`make check PYTHON3=python3` 全綠,作為重構前 baseline。任何 task 後若紅燈,先比對是否本 task 引入。

---

### Task 1: 加 proprietary LICENSE

**Files:**
- Create: `LICENSE`
- 不動:`c_model/include/axi/ATTRIBUTION.md`(vendored cocotbext-axi MIT,保留)

**Interfaces:** 無程式介面。

- [ ] **Step 1: 建立 LICENSE**

寫入 repo root `LICENSE`:

```
Copyright (c) 2026 <版權持有人>. All rights reserved.

PROPRIETARY AND CONFIDENTIAL — INTERNAL USE ONLY.

This software and associated documentation files (the "Software") are the
confidential and proprietary property of the copyright holder. No license,
express or implied, to use, copy, modify, merge, publish, distribute,
sublicense, or sell copies of the Software is granted to any party without
the prior written permission of the copyright holder.

THIRD-PARTY COMPONENTS
Portions of the Software under c_model/include/axi/ are derived from
cocotbext-axi and remain licensed under the MIT License. See
c_model/include/axi/ATTRIBUTION.md for the applicable notice. This
proprietary license does not supersede that third-party license for those
files.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
```

> `<版權持有人>` 由 user 在 review 時填入(個人名 / 組織名)。

- [ ] **Step 2: 確認 build 未受影響**

Run: `make test PYTHON3=python3`
Expected: PASS(LICENSE 不進 build,只確認沒誤動)。

- [ ] **Step 3: Commit**

```bash
git add LICENSE
git commit -m "chore: add proprietary internal-use-only LICENSE"
```

---

### Task 2: 移除 register_file

**Files:**
- Delete: `c_model/include/register_file.hpp`
- Delete: `c_model/src/register_file.cpp`
- Delete: `c_model/tests/test_register_file.cpp`
- Modify: `c_model/tests/CMakeLists.txt`(移除 test_register_file entry,約 `:24-26`)
- Modify: `c_model/CMakeLists.txt` / `c_model/src/CMakeLists.txt`(若有把 `register_file.cpp` 列入 library sources,移除該行)

**Interfaces:** 無下游消費者(production 未接線)。

- [ ] **Step 1: 確認無外部引用**

Run:
```bash
grep -rIn 'register_file\|RegisterFile\|AbiResponse' --include='*.hpp' --include='*.cpp' --include='*.h' --exclude-dir=build --exclude-dir=.git .
```
Expected: 只命中 `register_file.hpp` / `register_file.cpp` / `test_register_file.cpp` 自身與其 CMake entry。若命中其他 production 檔 → STOP 回報(超出預期,不 inline workaround)。

- [ ] **Step 2: 刪檔 + 移除 CMake entry**

```bash
git rm c_model/include/register_file.hpp c_model/src/register_file.cpp c_model/tests/test_register_file.cpp
```
手動編輯 `c_model/tests/CMakeLists.txt` 移除 `test_register_file` 的 `add_executable` / `gtest_discover_tests` / `target_link_libraries` 三行區塊;若 `register_file.cpp` 出現在任何 library `add_library(... register_file.cpp ...)`,一併移除該來源行。

- [ ] **Step 3: 確認 generated regs 仍在(不清 specgen)**

Run:
```bash
ls specgen/generated/cpp/ni_regs.h specgen/generated/sv/ni_regs_pkg.sv
```
Expected: 兩檔仍存在(本輪刻意保留孤兒,不動 specgen)。

- [ ] **Step 4: build + ctest(含 specgen drift gate)**

Run: `make test PYTHON3=python3`
Expected: PASS。drift gate 在 build-cmodel configure 階段跑 `codegen.py --check`,比對 generated vs JSON,與 register_file 無關 → 應綠。

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "refactor: remove unused register_file (AI-generated, production-unwired)"
```

---

### Task 3: docs/ 清場

**Files:**
- Move: `docs/superpowers/` → `docs/internal/superpowers/`
- Move: `docs/_archive/` → `docs/internal/_archive/`
- Modify(若需):`Makefile`(`MAINTAINED_DOCS` 變數,若含被移動路徑)

**Interfaces:** 無程式介面。

- [ ] **Step 1: 確認 lint_docs 維護清單不含被移目錄**

Run: `grep -n 'MAINTAINED_DOCS' Makefile`
檢查 `MAINTAINED_DOCS` 列出的 .md 路徑是否含 `docs/superpowers/` 或 `docs/_archive/`。若含 → 同步改路徑;若不含(預期)→ 不動。

- [ ] **Step 2: 搬移**

```bash
mkdir -p docs/internal
git mv docs/superpowers docs/internal/superpowers
git mv docs/_archive docs/internal/_archive
```

- [ ] **Step 3: 檢查對外 docs 是否有連到被移路徑的連結**

Run:
```bash
grep -rIn 'docs/superpowers\|docs/_archive\|](_archive\|](superpowers' docs/architecture.md docs/development.md docs/performance-probe.md docs/perf-probe-report.md README.md --exclude-dir=.git
```
Expected: 無命中。若有,改成 `docs/internal/...` 對應路徑。

- [ ] **Step 4: lint_docs gate**

Run: `make lint_docs PYTHON3=python3`
Expected: PASS。

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "docs: move process artifacts (superpowers, _archive) under docs/internal"
```

> 註:本 plan 與其 spec 自身位於 `docs/superpowers/`,本步會一併搬到 `docs/internal/superpowers/`(預期行為)。

---

### Task 4: `noc/` → `router/`(目錄 + 介面檔去前綴 + namespace)

**Files:**
- Move dir: `c_model/include/noc` → `c_model/include/router`;`c_model/tests/noc` → `c_model/tests/router`
- Rename: `router/noc_{req_in,req_out,rsp_in,rsp_out}.hpp` → `router/{req_in,req_out,rsp_in,rsp_out}.hpp`
- Modify: 約 27 處 `#include "noc/..."`;75 處 `ni::cmodel::noc`;`c_model/tests/CMakeLists.txt`;`specgen/tools/gen_inventory.py`;`cosim/c/cmodel_dpi.cpp`(perf 取樣引用)

**Interfaces:**
- Produces:介面型別 `NocReqIn/NocReqOut/NocRspIn/NocRspOut` 不變,但其 namespace 變 `ni::cmodel::router`;header 路徑變 `router/req_in.hpp` 等。
- Consumes:無(本 task 為基礎改名,後續 task 8 依賴它)。

- [ ] **Step 1: 目錄與介面檔改名**

```bash
git mv c_model/include/noc c_model/include/router
git mv c_model/include/router/noc_req_in.hpp  c_model/include/router/req_in.hpp
git mv c_model/include/router/noc_req_out.hpp c_model/include/router/req_out.hpp
git mv c_model/include/router/noc_rsp_in.hpp  c_model/include/router/rsp_in.hpp
git mv c_model/include/router/noc_rsp_out.hpp c_model/include/router/rsp_out.hpp
git mv c_model/tests/noc c_model/tests/router
```

- [ ] **Step 2: 改寫 include 路徑(先雙-noc 介面檔,再一般前綴)**

```bash
for f in $(grep -rIl '"noc/' --include='*.hpp' --include='*.cpp' --include='*.h' --exclude-dir=build --exclude-dir=.git .); do
  sed -i -E 's@"noc/noc_(req|rsp)_(in|out)\.hpp"@"router/\1_\2.hpp"@g' "$f"
  sed -i -E 's@"noc/@"router/@g' "$f"
done
```

- [ ] **Step 3: 改 namespace `ni::cmodel::noc` → `ni::cmodel::router`**

```bash
# pass 1: 全限定 namespace
for f in $(grep -rIl 'ni::cmodel::noc' --include='*.hpp' --include='*.cpp' --include='*.h' --exclude-dir=build --exclude-dir=.git .); do
  sed -i 's/ni::cmodel::noc/ni::cmodel::router/g' "$f"
done
# pass 2: 非全限定 noc:: 短引用(router_shell_adapter.hpp 等大量使用 noc::Router / noc::RouterPort / noc::LinkEjectAdapter ...)。
# pass 1 已把 ni::cmodel::noc:: 改掉,此處剩下的 \bnoc:: 必為 namespace 短引用。
for f in $(grep -rIlE '\bnoc::' --include='*.hpp' --include='*.cpp' --include='*.h' --exclude-dir=build --exclude-dir=.git .); do
  sed -i -E 's@\bnoc::@router::@g' "$f"
done
```

- [ ] **Step 4: CMake、specgen inventory、DPI、殘留註解**

```bash
sed -i 's/add_subdirectory(noc)/add_subdirectory(router)/' c_model/tests/CMakeLists.txt
sed -i 's@noc_req_out\.hpp@req_out.hpp@' c_model/include/router/rsp_out.hpp
sed -i -E 's@(include/|tests/)noc@\1router@g' specgen/tools/gen_inventory.py
```
手動檢查 `cosim/c/cmodel_dpi.cpp`(perf 取樣處,約 `:977-978`)若仍 `#include "noc/..."` 或用 `ni::cmodel::noc`,已由 Step 2/3 涵蓋;若有 target 名/路徑字串殘留,手動修。

- [ ] **Step 5: 殘留掃描(應為空,scenario lib 名例外)**

```bash
grep -rInE '"noc/|ni::cmodel::noc|\bnoc::|add_subdirectory\(noc\)|include/noc|tests/noc' \
  --include='*.hpp' --include='*.cpp' --include='*.h' --include='CMakeLists.txt' --include='*.py' \
  --exclude-dir=build --exclude-dir=.git .
```
Expected: 空。**刻意保留**(非殘漏):scenario lib 名 `noc_axi4_scenarios`、型別名 `Noc*`、`docs/internal/_archive/` 歷史記錄。若命中其他 → 修。

- [ ] **Step 6: clang-format 改過的 C++ 檔**

```bash
for f in $(git diff --name-only --diff-filter=ACMR HEAD | grep -E '\.(hpp|cpp|h)$'); do clang-format -i "$f"; done
```

- [ ] **Step 7: build + co-sim gate**

Run: `make check PYTHON3=python3`
Expected: PASS(含 build-verilator,確認 DPI 路徑未破)。

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "refactor: rename noc/ to router/ (dir, interface files, namespace)"
```

---

### Task 8: 建 `c_model/include/ni/`(搬共用 primitive)

> 在 Task 4 之後 → 來源已是 `router/`,無「noc/ or router/」歧義。

**Files:**
- Create dir: `c_model/include/ni/`
- Move: `nmu/ni_stage.hpp`、`router/pipeline_stage.hpp`、`router/wormhole_arbiter.hpp` → `ni/`
- Modify consumers:`nmu/nmu.hpp`、`nsu/nsu.hpp`、`nmu/depacketize.hpp`(及任何 `#include "nmu/ni_stage.hpp"` / `"router/pipeline_stage.hpp"` / `"router/wormhole_arbiter.hpp"`)、`tests/common/test_ni_stage.cpp`、`tests/router/test_pipeline_stage.cpp`

**Interfaces:**
- Produces:`ni/ni_stage.hpp`、`ni/pipeline_stage.hpp`、`ni/wormhole_arbiter.hpp`(namespace 維持原樣,不在本 task 改;見備註)。
- Consumes:Task 4 的 `router/` 路徑。

> 備註:`pipeline_stage`/`wormhole_arbiter` 原 namespace 為 `ni::cmodel::router`(Task 4 後);`ni_stage` 原為 `ni::cmodel::nmu`。本 task **只搬檔不改 namespace**(避免與搬移混在一起放大風險);namespace 是否進一步收斂到 `ni::cmodel::ni` 列為後續 backlog,不在本輪。

- [ ] **Step 1: 搬檔**

```bash
mkdir -p c_model/include/ni
git mv c_model/include/nmu/ni_stage.hpp        c_model/include/ni/ni_stage.hpp
git mv c_model/include/router/pipeline_stage.hpp    c_model/include/ni/pipeline_stage.hpp
git mv c_model/include/router/wormhole_arbiter.hpp  c_model/include/ni/wormhole_arbiter.hpp
```

- [ ] **Step 2: 改寫 include 路徑**

```bash
for f in $(grep -rIl '"nmu/ni_stage\.hpp"\|"router/pipeline_stage\.hpp"\|"router/wormhole_arbiter\.hpp"' \
    --include='*.hpp' --include='*.cpp' --include='*.h' --exclude-dir=build --exclude-dir=.git .); do
  sed -i -E 's@"(nmu|router)/(ni_stage|pipeline_stage|wormhole_arbiter)\.hpp"@"ni/\2.hpp"@g' "$f"
done
```

- [ ] **Step 3: test CMake 掛載(若 test 依目錄分組)**

檢查 `c_model/tests/common/CMakeLists.txt` 與 `c_model/tests/router/CMakeLists.txt` 對 `test_ni_stage` / `test_pipeline_stage` 的來源/ include 路徑;若指向舊路徑則更新。`test_*` target 名不改。

- [ ] **Step 4: 殘留掃描**

```bash
grep -rIn '"nmu/ni_stage\.hpp"\|"router/pipeline_stage\.hpp"\|"router/wormhole_arbiter\.hpp"' \
  --include='*.hpp' --include='*.cpp' --include='*.h' --exclude-dir=build --exclude-dir=.git .
```
Expected: 空。

- [ ] **Step 5: clang-format + build gate**

```bash
for f in $(git diff --name-only --diff-filter=ACMR HEAD | grep -E '\.(hpp|cpp|h)$'); do clang-format -i "$f"; done
```
Run: `make check PYTHON3=python3`
Expected: PASS。

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor: extract shared NI primitives (ni_stage, pipeline_stage, wormhole_arbiter) to include/ni"
```

---

### Task 5: 頂層 `cosim/` → `sim/`

**Files:**
- Move dir: `cosim/` → `sim/`
- Modify hardcoded 路徑:`Makefile`(`COSIM_VERILATOR`/`COSIM_VCS`/`-C` targets,約 `:17-18,110,156,159`)、`c_model/tests/CMakeLists.txt:42`、`c_model/tests/cosim/CMakeLists.txt:32,49`(指向 `../cosim/c`)、`sim/sources.mk`(`COSIM_ROOT`)、`docs/development.md:185-222`

**Interfaces:**
- Produces:co-sim 整合層位於 `sim/`(`sim/c/`、`sim/sv/`、`sim/verilator/`、`sim/vcs/`)。
- Consumes:無新依賴。

- [ ] **Step 1: 目錄改名**

```bash
git mv cosim sim
```

- [ ] **Step 2: 改寫所有 `cosim` 路徑引用**

```bash
# Makefile 變數與 -C 目標
sed -i -E 's@cosim/(verilator|vcs|c|sv|tests|tools)@sim/\1@g; s@COSIM_VERILATOR := cosim@COSIM_VERILATOR := sim@; s@COSIM_VCS := cosim@COSIM_VCS := sim@' Makefile
# c_model 測試 CMake 對頂層 cosim 的相對路徑。
# 實際寫法是 ${CMAKE_SOURCE_DIR}/../cosim/{c,tests}(單層 ../),故只把 cosim→sim,不可加層。
sed -i -E 's@\.\./cosim@../sim@g' c_model/tests/CMakeLists.txt c_model/tests/cosim/CMakeLists.txt
# sources.mk COSIM_ROOT(若以路徑字串定義)
grep -rIl 'cosim' sim/sources.mk sim/*.mk 2>/dev/null | xargs -r sed -i 's@cosim@sim@g'
# specgen yaml/json 內的 cosim 路徑(constants.yaml 有 cosim/c/cmodel_dpi.cpp)
grep -rIl 'cosim/' specgen --include='*.yaml' --include='*.json' --exclude-dir=build 2>/dev/null \
  | xargs -r sed -i 's@cosim/@sim/@g'
```
> 相對路徑層數以實際檔案位置為準,Step 4 build 會驗證;若不符依編譯錯誤修正,不要硬套。`${CMAKE_BINARY_DIR}/cosim_smoke_tests` 是 build artifact 目錄名(無 slash),本步不動,可不改。

- [ ] **Step 3: docs 與殘留路徑**

```bash
sed -i 's@cosim/@sim/@g; s@`cosim`@`sim`@g; s@ cosim @ sim @g' docs/development.md
grep -rIn '\bcosim/\|COSIM_\|-C cosim\|cosim/c\|cosim/sv\|cosim/verilator\|cosim/vcs' \
  --include='Makefile' --include='*.mk' --include='*.cmake' --include='CMakeLists.txt' --include='*.md' \
  --exclude-dir=build --exclude-dir=.git .
```
Expected(掃描):空(C++ namespace `ni::cmodel::cosim` 不是路徑,Task 6 才處理,不應被本步命中)。

- [ ] **Step 4: build + co-sim gate(含 VCS 路徑 dry-run)**

Run: `make check PYTHON3=python3`
Expected: PASS(build-verilator 走 `sim/verilator`)。

`make check` **只跑 Verilator,不跑 VCS**。VCS Makefile(`sim/vcs/Makefile`)引用 `$(COSIM_ROOT)/sv/tb_top_vcs.sv` 等,改名後須手動 dry-run 確認路徑解析:
```bash
cd sim/vcs && make -n tb_top PYTHON3=python3; cd - >/dev/null
```
Expected: 展開的指令裡所有路徑都是 `sim/...`,無殘留 `cosim/`、無 "file not found" 類報錯。(無 VCS 授權則只看路徑字串正確。)

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "refactor: rename top-level cosim/ to sim/ (dir + build paths)"
```

---

### Task 6: C++ `shell` → `wrap`(目錄 + 檔名 + 型別 + namespace)

> 在 Task 5 之後 → DPI 側已在 `sim/c/`,本 task 落最終路徑。

**Files:**
- Move dir: `c_model/include/cosim` → `c_model/include/wrap`;`c_model/tests/cosim` → `c_model/tests/wrap`
- Rename: `{nmu,nsu,master,slave,router,channel_model}_shell_adapter.hpp` → `*_wrap.hpp`;`*_shell_io.hpp` → `*_wrap_io.hpp`;`test_*_shell_adapter.cpp` → `test_*_wrap.cpp`
- Modify:型別 `*ShellAdapter`→`*Wrap`、`*ShellIo`→`*WrapIo`;namespace `ni::cmodel::cosim`→`ni::cmodel::wrap`(86 處);`sim/c/cmodel_dpi.cpp`、`sim/c/handle_block.hpp` include 與型別;CMake `add_subdirectory(cosim)`;docs

**Interfaces:**
- Produces:wrap 層型別 `NmuWrap/NsuWrap/MasterWrap/SlaveWrap/RouterWrap/ChannelModelWrap` + `*WrapIo`,namespace `ni::cmodel::wrap`,header `wrap/*_wrap.hpp`。
- Consumes:Task 5 的 `sim/` 路徑。

- [ ] **Step 1: 目錄與檔名改名**

```bash
git mv c_model/include/cosim c_model/include/wrap
git mv c_model/tests/cosim   c_model/tests/wrap
cd c_model/include/wrap
for c in nmu nsu master slave router channel_model; do
  git mv ${c}_shell_adapter.hpp ${c}_wrap.hpp
  git mv ${c}_shell_io.hpp      ${c}_wrap_io.hpp
done
cd - >/dev/null
cd c_model/tests/wrap
for c in nmu nsu master slave router channel_model; do
  [ -f test_${c}_shell_adapter.cpp ] && git mv test_${c}_shell_adapter.cpp test_${c}_wrap.cpp
done
cd - >/dev/null
```

- [ ] **Step 2: 批次改識別字(include / 型別 / namespace / 檔名引用)**

```bash
FILES=$(grep -rIl 'shell\|Shell\|ni::cmodel::cosim\|"cosim/' \
  --include='*.hpp' --include='*.cpp' --include='*.h' --include='*.sv' --include='CMakeLists.txt' \
  --exclude-dir=build --exclude-dir=.git .)
for f in $FILES; do
  sed -i -E \
    -e 's@ni::cmodel::cosim@ni::cmodel::wrap@g' \
    -e 's@(nmu|nsu|master|slave|router|channel_model)_shell_adapter\.hpp@\1_wrap.hpp@g' \
    -e 's@(nmu|nsu|master|slave|router|channel_model)_shell_io\.hpp@\1_wrap_io.hpp@g' \
    -e 's@"cosim/@"wrap/@g' \
    -e 's@ShellAdapter@Wrap@g' \
    -e 's@ShellIo@WrapIo@g' \
    -e 's@shell_adapter@wrap@g' \
    -e 's@shell_io@wrap_io@g' \
    "$f"
done
```
> 警告:`s@shell_adapter@wrap@g` 不可改成過寬的 `s@shell@wrap@g`(會誤傷無關字)。`s@"cosim/@"wrap/@g` 改的是 C++ include 前綴(`#include "cosim/flit_byte_conv.hpp"` 等);Task 5 已把頂層 `cosim/` 路徑改為 `sim/`,故此處剩的 `"cosim/` 必為 c_model include-path 前綴,改成 `wrap/` 正確。涵蓋 `sim/c/cmodel_dpi.cpp`(含 shell include,在 FILES 內)。SV 側 `*_wrap.sv` 本就 `_wrap`,不受影響。

- [ ] **Step 3: CMake 掛載 + 殘留註解清掃**

```bash
sed -i 's/add_subdirectory(cosim)/add_subdirectory(wrap)/' c_model/tests/CMakeLists.txt
# 殘留掃描(comment/字串裡的 shell + 漏網 cosim include 前綴)
grep -rInE 'shell|Shell|ni::cmodel::cosim|"cosim/' \
  --include='*.hpp' --include='*.cpp' --include='*.h' --include='CMakeLists.txt' \
  --exclude-dir=build --exclude-dir=.git .
```
Expected: 僅剩語意正確或無關的 `shell`(如外部工具名);`*ShellAdapter`/`shell_adapter`/`ni::cmodel::cosim`/`"cosim/` 應為空。逐一檢視殘留,該改的手動改。

- [ ] **Step 4: clang-format**

```bash
for f in $(git diff --name-only --diff-filter=ACMR HEAD | grep -E '\.(hpp|cpp|h)$'); do clang-format -i "$f"; done
```

- [ ] **Step 5: build + co-sim gate(含 VCS dry-run)**

Run: `make check PYTHON3=python3`
Expected: PASS(DPI 匯出名 `cmodel_<元件>_<op>` 不含 shell/wrap,SV↔DPI ABI 不變)。

VCS 編 `sim/c/cmodel_dpi.cpp`(本 task 改了它的 include/namespace),make check 不含 VCS,手動 dry-run:
```bash
cd sim/vcs && make -n tb_top PYTHON3=python3; cd - >/dev/null
```
Expected: 無 "file not found"、include 路徑解析正確。

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor: rename c_model shell layer to wrap (dir, files, types, namespace)"
```

---

### Task 7: `flit_link_perf_monitor.sv` → `link_perf_monitor.sv`

> Task 5 後檔案位於 `sim/sv/`。

**Files:**
- Rename: `sim/sv/flit_link_perf_monitor.sv` → `sim/sv/link_perf_monitor.sv`
- Modify:`sim/sources.mk`(來源清單,原 `:73`)、`sim/sv/tb_top.sv`(module 實例化,原 `:319-340`)、`sim/sv/tb_top_vcs.sv`(若有同實例)

**Interfaces:**
- Produces:SV module 檔名/路徑 `link_perf_monitor.sv`。module 名是否一併改見 Step 2。

- [ ] **Step 1: 檔案改名**

```bash
git mv sim/sv/flit_link_perf_monitor.sv sim/sv/link_perf_monitor.sv
```

- [ ] **Step 2: 改 module 名與所有引用**

先確認 module 宣告名:
```bash
grep -n 'module .*flit_link_perf_monitor\|flit_link_perf_monitor' sim/sv/link_perf_monitor.sv sim/sources.mk sim/sv/tb_top.sv sim/sv/tb_top_vcs.sv
```
把 module 宣告、實例化型別、sources.mk 檔名一致改:
```bash
grep -rIl 'flit_link_perf_monitor' --include='*.sv' --include='*.mk' --exclude-dir=build --exclude-dir=.git . \
  | xargs -r sed -i 's@flit_link_perf_monitor@link_perf_monitor@g'
```

- [ ] **Step 3: 殘留掃描**

```bash
grep -rIn 'flit_link_perf_monitor' --exclude-dir=build --exclude-dir=.git .
```
Expected: 空。

- [ ] **Step 4: co-sim gate(含 VCS dry-run)**

Run: `make check PYTHON3=python3`
Expected: PASS(verilator 編 tb_top,確認 module 連得上)。

VCS 也實例化此 monitor,手動 dry-run 確認 SV 來源清單更新:
```bash
cd sim/vcs && make -n tb_top PYTHON3=python3; cd - >/dev/null
```
Expected: 來源清單含 `link_perf_monitor.sv`,無殘留 `flit_link_perf_monitor.sv`。

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "refactor: rename flit_link_perf_monitor.sv to link_perf_monitor.sv"
```

---

### Task 9: 文件同步成最終實況(嚴格最後)

**Files:**
- Modify: `docs/issue/ARCHITECTURE.md`、`docs/architecture.md`、`CLAUDE.md`、`README.md`、`tests/scenarios/README.md`
- 並修正 Step 6 最終 repo-wide 殘留掃描命中的**任何**檔(noc/cosim/shell 殘留),不限上列
- (若 `docs/issue/` 仍 untracked,先 `git add` 納入版控由 user 決定)

**Interfaces:** 無程式介面。

> 註:Task 5 fix 已清掉大部分 `cosim/` path 殘留(build-檔註解、ATTRIBUTION.md、README.md/tests README 的 cosim 部分)。本 task 處理 noc→router、shell→wrap、register_file 移除的 doc 殘留 + 最終全 repo sweep。

- [ ] **Step 1: `docs/issue/ARCHITECTURE.md` 改寫**

- §6.1:把「已執行 / 改名已套用」改為實況(本輪才真正 apply);移除「拋棄 clone 驗證」的假完成敘述,改為紀錄本次實際 commit。
- §3.5 標題與 §1 分層圖:`noc/`→`router/` 已生效;`pipeline_stage`/`wormhole_arbiter`/`ni_stage` 移至新 `ni/` 層。
- §3.6:`c_model/include/cosim/`→`wrap/`;`*_shell_adapter`→`*_wrap`、`*ShellAdapter`→`*Wrap`。
- §1 / §4:頂層 `cosim/`→`sim/`;`flit_link_perf_monitor.sv`→`link_perf_monitor.sv`。
- §3.1 / §3.7 / §6.2:移除 register_file 相關行(已刪);§6.2 #1/#2/#6/#7 標為已完成或移除。
- namespace:`ni::cmodel::noc`→`router`、`cosim`→`wrap`。

- [ ] **Step 2: `docs/architecture.md` 同步**

```bash
grep -n 'c_model/include/noc\|/noc/\|cosim/\|shell\|register_file\|ni::cmodel::noc\|ni::cmodel::cosim' docs/architecture.md
```
逐處改為 `router/`、`sim/`、`wrap/`、移除 register_file、namespace 更新。

- [ ] **Step 3: `CLAUDE.md` 同步**

- production sub-namespaces 清單 `axi, cosim, nmu, noc, nsu` → `axi, nmu, nsu, router, wrap`(新增 `ni`;移除 `noc`/`cosim`)。
- 「`*_shell_adapter.hpp`」「ChannelModelShellAdapter」等敘述 → `*_wrap` / `*Wrap`。
- ShellAdapter / chandle ABI 段落確認與現況一致。

- [ ] **Step 4: 重生 FEATURE_INVENTORY.md(路徑改名後 stale)**

`c_model/FEATURE_INVENTORY.md` 為 `specgen/tools/gen_inventory.py` 產物(非 build-gated),含 stale `c_model/include/noc/...`、`cosim/...`、`shell` 等路徑。重生:
```bash
python3 specgen/tools/gen_inventory.py
git diff --stat c_model/FEATURE_INVENTORY.md
```
Expected: diff 反映 `router/`、`ni/`、`wrap/`、`sim/`、register_file 移除。

- [ ] **Step 5: lint_docs + 全綠最終確認**

Run: `make check PYTHON3=python3`
Expected: PASS。

- [ ] **Step 6: 全 repo 最終殘留掃描(Success Criteria)**

```bash
grep -rIn '"noc/\|ni::cmodel::noc\|ni::cmodel::cosim\|ShellAdapter\|shell_adapter\|register_file\|flit_link_perf_monitor\|\bcosim/' \
  --include='*.hpp' --include='*.cpp' --include='*.h' --include='*.sv' --include='*.md' --include='Makefile' --include='*.mk' --include='CMakeLists.txt' \
  --exclude-dir=build --exclude-dir=.git .
```
Expected: 僅 `docs/internal/_archive/`(歷史封存,刻意保留)與 scenario lib 名 `noc_axi4_scenarios`、型別名 `Noc*` 命中;其餘為空。

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "docs: sync ARCHITECTURE.md, architecture.md, CLAUDE.md + regen FEATURE_INVENTORY"
```

---

## Self-Review(plan vs spec)

- **Spec coverage**:spec 9 步 ↔ plan Task 1/2/3/4/8/5/6/7/9 一一對應(含 §6.1 真正 apply、namespace 政策、register_file flagged 已定案保留 specgen)。✅
- **Deferred 一致**:§6.3 + 抽鷹架 + specgen 清理皆未出現於任何 task。✅
- **型別/命名一致性**:`*ShellAdapter`→`*Wrap`、`*ShellIo`→`*WrapIo` 全 plan 統一;namespace `noc→router`/`cosim→wrap` 統一;`Noc*` 型別保留一致。✅
- **No placeholder**:LICENSE `<版權持有人>` 為刻意留給 user 的單一填空(review 時填);其餘步驟皆具體指令。
- **順序依賴**:Task 8 在 4 後(來源 router/)、Task 6 在 5 後(落 sim/c)、Task 9 最後。✅
