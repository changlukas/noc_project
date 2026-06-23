# Verilator --timing Unification — Design

- Date: 2026-06-23
- Goal: Verilator 與 VCS 共用單一 testbench harness `tb_top_vcs`(自帶 clk/reset/timeout/perf/finalize),Verilator 改用 `--timing` 驅動。退役 `--no-timing` + C++-打-clk 的 `main.cpp`,消掉雙套驅動 + 常數 drift。
- Driver: single source of truth(sim 基礎建設可讀性 / 維護性)。
- Validated by: spike `spike/verilator-timing`(commit 8cbd813)+ controller 全 suite 壓測。

## Spike 已證實(blueprint)
- `--timing --output-split 0`、top=`tb_top_vcs`、64MiB stack reserve:**build 乾淨,無 STATUS_STACK_OVERFLOW**(含 BUR-003 256-beat / STR / ORD 等深 eval)。
- ctest 驗的 6 個 bidirectional scenario(BAS-002/003/004/005、HSH-002、ORD-001)在 --timing **全 PASS**。
- per-transaction perf **byte-identical** vs --no-timing。
- 唯一 gap:`noc.links` 聚合(flit_count/stall_cyc)在 --timing 下空 → 本 design ① 修。

## ① link-perf 修法
**INPUT**:`sim/sv/link_perf_monitor.sv` — `flit_count`/`stall_cyc` 已在 `always_ff` 逐拍累加,但只在 `final` 推一次 `cmodel_perf_link(name, flit_count, stall_cyc)`。
**ROOT CAUSE**:SV 跨模組 `final` 順序未定義 — 在 `tb_top_vcs.final`(dump perf)之後才跑 `link_perf_monitor.final`(push)→ dump 時 link 還沒 push。VCS / Verilator-timing 同病。
**COMPUTE/FIX**:改成**逐拍 live push** running total、**移除 `final` block**。`cmodel_perf_link` 已是 set/覆寫語意(`g_perf.set_link` by name)→ 最後一拍的呼叫帶最終總額,正確。
**OUTPUT**:`noc.links` 在兩個 simulator 都有值;無 end-of-sim 順序依賴。collector(C 側)**不改**。
> 細節:在 reset 後的 `always_ff` 內、更新計數後呼叫 `cmodel_perf_link(LINK_NAME, flit_count, stall_cyc)`(set 語意,少數 link、每拍一呼可接受)。

## ② Verilator flow 切換(退役 --no-timing)
- `sim/verilator/Makefile`:`TOP := tb_top` → `tb_top_vcs`;`VERILATOR_FLAGS` 的 `--no-timing` → `--timing --output-split 0`;保留 64MiB stack reserve(`-Wl,--stack,67108864`,spike 確認乾淨接受)。
- C main:**刪 `sim/verilator/main.cpp`**(打 clk/reset/timeout/perf/VCD 那套)。新增極簡 event-loop main(spike 的 `main_tb_top_vcs.cpp` 內容)**命名為 `sim/verilator/main.cpp`**(單一 canonical Verilator tb_top main;`main_genamba.cpp` 另存不動)。内容:`commandArgs` → 建 `Vtb_top_vcs` → `while(!gotFinish){ eval(); if(!eventsPending) break; time(nextTimeSlot); }` → `final()`。clk/reset/timeout/perf/finalize 全在 `tb_top_vcs` SV 內。
- **Verilator VCD 放掉**(user 決定;波形走 VCS/FSDB)— 新 main 不含 `VM_TRACE`/`+vcd`;Makefile `TRACE=1` 的 VCD 路徑移除或標 VCS-only。
- obj_dir 用正式名(spike `obj_dir_timing` 收掉)。
- **必須透過正常 `make` build**:沿用 genamba `--timing` target 已解的 MSYS2 make/sed workaround(`--output-split 0`、path sed、`$(MAKE) -C`、`$(EXEEXT)`),不留 spike 的手動兩段 / `|| true` hack。

## ③ ctest
`sim/tests/test_cosim_integration.cpp` + 其 CMake:期望的 binary 由 `Vtb_top` 改為 `Vtb_top_vcs`(路徑隨 obj_dir)。其餘測試邏輯不變(同 scenario set、同 PASS marker)。

## ④ VCS 側
**不動**。`sim/vcs/Makefile` 本來就 `-top tb_top_vcs`;統一 = Verilator 收斂過來。link-perf 修法附帶修好 VCS 的同個 gap。

## ⑤ 文件
`docs/architecture.md`、`docs/development.md`、`docs/issue/ARCHITECTURE.md`:移除「`--no-timing` + `main.cpp` 打 clk + 雙驅動 / simulator-neutral tb_top by C++ harness」敘述,改為統一的 `tb_top_vcs` + `--timing`(Verilator 與 VCS 共用)。`main.cpp` 角色更新為 event-loop entry。

## Success Criteria
- `make check PYTHON3=python3` 全綠:`--timing` build-verilator 經正常 `make` 成功(無 stack overflow)、ctest **544/544**。
- 6 個 bidirectional scenario 在統一 flow 全 PASS,perf parity vs 修前 --no-timing baseline,且 **`noc.links` 有值**(flit_count/stall_cyc 正確)。
- VCS dry-run(`cd sim/vcs && make -n tb_top`)路徑仍解析。
- repo 無殘留 `--no-timing`(tb_top flow)、無 `main.cpp` 打 clk 邏輯、無孤兒 VCD 路徑。
- 文件與實況一致(單一 harness 敘述)。

## Out of Scope
- genamba testbench(獨立,`--timing` 已用;去留是另一回事)。
- 多 host 驗證(spike 僅 Windows/Verilator 5.036;退役 --no-timing 後若未來他 host 撞 --timing 問題,屬後續)。
