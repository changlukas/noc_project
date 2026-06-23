# Verilator --timing Unification — Design

- Date: 2026-06-23
- Goal: 把 testbench 收成**單一 self-clocked `tb_top.sv`**(合併原 `tb_top` 本體 + `tb_top_vcs` wrapper),Verilator 與 VCS 共用、皆 `-top tb_top`,Verilator 改用 `--timing`。退役 `--no-timing` + C++-打-clk 的 `main.cpp` + 多餘的 `tb_top_vcs` wrapper 層,消掉雙套驅動 + 常數 drift + `_vcs` 誤名。
- Driver: single source of truth(sim 基礎建設可讀性 / 維護性)。
- Validated by: spike `spike/verilator-timing`(commit 8cbd813)+ controller 全 suite 壓測。

## Spike 已證實(blueprint)
- `--timing --output-split 0`、top=`tb_top_vcs`、64MiB stack reserve:**build 乾淨,無 STATUS_STACK_OVERFLOW**(含 BUR-003 256-beat / STR / ORD 等深 eval)。
- ctest 驗的 6 個 bidirectional scenario(BAS-002/003/004/005、HSH-002、ORD-001)在 --timing **全 PASS**。
- per-transaction perf **byte-identical** vs --no-timing。
- 唯一 gap:`noc.links` 聚合(flit_count/stall_cyc)在 --timing 下空 → 本 design ① 修。
> spike 用 `tb_top_vcs` wrapper 當 top 驗證;productionization 把 wrapper 內容**合併進 `tb_top`**(同一份 self-clocked SV,只是收成一個 module)→ 行為等價,故 spike 的 build/parity 結論成立。

## ① link-perf 修法
**INPUT**:`sim/sv/link_perf_monitor.sv` — `flit_count`/`stall_cyc` 已在 `always_ff` 逐拍累加,但只在 `final` 推一次 `cmodel_perf_link(name, flit_count, stall_cyc)`。
**ROOT CAUSE**:SV 跨模組 `final` 順序未定義 — 在 `tb_top_vcs.final`(dump perf)之後才跑 `link_perf_monitor.final`(push)→ dump 時 link 還沒 push。VCS / Verilator-timing 同病。
**COMPUTE/FIX**:改成**逐拍 live push** running total、**移除 `final` block**。`cmodel_perf_link` 已是 set/覆寫語意(`g_perf.set_link` by name)→ 最後一拍的呼叫帶最終總額,正確。
**OUTPUT**:`noc.links` 在兩個 simulator 都有值;無 end-of-sim 順序依賴。collector(C 側)**不改**。
> 細節:在 reset 後的 `always_ff` 內、更新計數後呼叫 `cmodel_perf_link(LINK_NAME, flit_count, stall_cyc)`(set 語意,少數 link、每拍一呼可接受)。

## ② 合併成單一 self-clocked `tb_top.sv`(退役 --no-timing + wrapper)
> 背景:`tb_top.sv:21` 自述 clk/rst 做成 input port 的**唯一原因**是讓 `main.cpp` 從外面打 clock(--no-timing 遺留)。除 `tb_top_vcs` 與待刪的 `main.cpp` 外,無其他 external-clock consumer(genamba 是獨立 tb,用自己的 ACLK)。故退役 main.cpp 後,port 化的理由消失 → 合併。
- **`sim/sv/tb_top.sv`**:把 `tb_top_vcs.sv` 的 `always #5` clk / reset 序列 / timeout `$fatal` / `cmodel_perf_sample_tick` per-posedge / `final`(perf dump + finalize)/ `ifdef FSDB_DUMP` 全部搬進來;`clk_i`/`rst_ni` 從 **input port 改成內部自驅 `logic`**(移出 port list,tb_top 變無 port top);內部 `.clk_i(clk_i)` 等接線不變(改指內部 logic)。
- **刪 `sim/sv/tb_top_vcs.sv`**(已合併)。`_vcs` 誤名問題隨之消失。
- `sim/verilator/Makefile`:`TOP` 維持 `tb_top`;`VERILATOR_FLAGS` 的 `--no-timing` → `--timing --output-split 0`;保留 64MiB stack reserve(`-Wl,--stack,67108864`,spike 確認乾淨接受);SV_SRC 移除 tb_top_vcs.sv。
- C main:**刪舊 `sim/verilator/main.cpp`**(打 clk/reset/timeout/perf/VCD 那套),改為極簡 event-loop main(`sim/verilator/main.cpp`):`commandArgs` → 建 `Vtb_top` → `while(!gotFinish){ eval(); if(!eventsPending) break; time(nextTimeSlot); }` → `final()`。clk/reset/timeout/perf/finalize 全在 `tb_top` SV 內。(`main_genamba.cpp` 不動。)
- **Verilator VCD 放掉**(user 決定;波形走 VCS/FSDB)— 新 main 不含 `VM_TRACE`/`+vcd`;Makefile `TRACE=1` 的 VCD 路徑移除或標 VCS-only。
- obj_dir 用正式名(spike `obj_dir_timing` 收掉)。
- **必須透過正常 `make` build**:沿用 genamba `--timing` target 已解的 MSYS2 make/sed workaround(`--output-split 0`、path sed、`$(MAKE) -C`、`$(EXEEXT)`),不留 spike 的手動兩段 / `|| true` hack。

## ③ ctest
binary 仍是 **`Vtb_top`**(top 維持 `tb_top`)→ `sim/tests/test_cosim_integration.cpp` 的 binary 名/路徑**不用改**;測試邏輯不變(同 scenario set、同 PASS marker)。僅需確認在 `--timing` build 下仍綠。

## ④ VCS 側
`sim/vcs/Makefile`:`-top tb_top_vcs` → **`-top tb_top`**;source 清單移除 `tb_top_vcs.sv`(已合併進 tb_top.sv)。VCS 行為不變(tb_top 現自帶 clock,VCS 照樣 event-driven 跑),且附帶吃到 ① 的 link-perf 修法。

## ⑤ 文件
`docs/architecture.md`、`docs/development.md`、`docs/issue/ARCHITECTURE.md`:移除「`--no-timing` + `main.cpp` 打 clk + 雙驅動 / simulator-neutral tb_top(clk/rst input port)by C++ harness」與 `tb_top_vcs` wrapper 的敘述,改為**單一 self-clocked `tb_top.sv`**(Verilator `--timing` 與 VCS 共用 `-top tb_top`)。`main.cpp` 角色更新為 minimal event-loop entry。

## Success Criteria
- `make check PYTHON3=python3` 全綠:`--timing` build-verilator 經正常 `make` 成功(無 stack overflow)、ctest **544/544**。
- 6 個 bidirectional scenario 在統一 flow 全 PASS,perf parity vs 修前 --no-timing baseline,且 **`noc.links` 有值**(flit_count/stall_cyc 正確)。
- VCS dry-run(`cd sim/vcs && make -n tb_top`)路徑仍解析(`-top tb_top`)。
- repo 無殘留:`--no-timing`(tb_top flow)、`main.cpp` 打 clk 邏輯、孤兒 VCD 路徑、`tb_top_vcs`(檔/module/`-top`/`Vtb_top_vcs`)、spike 的 `main_tb_top_vcs.cpp` / `obj_dir_timing`。
- 文件與實況一致(單一 self-clocked tb_top 敘述)。

## Out of Scope
- genamba testbench(獨立,`--timing` 已用;去留是另一回事)。
- 多 host 驗證(spike 僅 Windows/Verilator 5.036;退役 --no-timing 後若未來他 host 撞 --timing 問題,屬後續)。
