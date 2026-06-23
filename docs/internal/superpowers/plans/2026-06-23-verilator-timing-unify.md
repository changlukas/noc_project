# Verilator --timing Unification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 testbench 收成單一 self-clocked `tb_top.sv`,Verilator 改 `--timing` 與 VCS 共用 `-top tb_top`;退役 `--no-timing` + C++-打-clk 的 `main.cpp` + `tb_top_vcs` wrapper;順手修兩個 perf monitor 的 `final`-ordering gap。

**Architecture:** spike(`spike/verilator-timing` 8cbd813)已證 `--timing` self-clocked top 在本機 build 乾淨(無 stack overflow)、6 個驗證 scenario 全過、per-transaction perf parity。本 plan 把 wrapper 合併進 tb_top、切 Verilator 到 --timing、並把 link/axi 聚合改成逐拍 live push(消除 SV `final` 跨模組順序依賴)。

**Tech Stack:** SystemVerilog + Verilator 5.036(`--timing`)+ VCS;C++ DPI;CMake/GoogleTest;Git Bash + GNU make。

## Global Constraints
- **build/test python**:`PYTHON3=python3`(mingw64),**不用 `py -3`**。
- **每 task gate**:`make check PYTHON3=python3`(lint + build-verilator + ctest)全綠才下一 task;觸及 SV/build 時 stale verilator `.d` 先 `make clean-verilator`。
- **執行序固定**:1 → 2 → 3 → 4。
- **commit 格式**:`type(scope): description`(English)。
- **不 push**;feature branch `refactor/verilator-timing-unify`(已建,off main 3be287e)。
- **NBA 正確性**:SV 逐拍 push 必推 **next 值**(pre-update 訊號算出 next、assign register、再 push),不可推 NBA 前舊值。
- **DPI set 語意**:`g_perf.set_link` 確認 last-write-wins(`perf_collector.hpp:56`);`cmodel_perf_axi_backpressure` 須驗同為 set-by-slot。
- **carry --timing workarounds**(沿用 genamba target):`--output-split 0`、Windows path `sed`、`$(MAKE) -C`、`$(EXEEXT)`、`-Wl,--stack,67108864`。
- spec:`docs/internal/superpowers/specs/2026-06-23-verilator-timing-unify-design.md`。

**基準(開工前)**:`make check PYTHON3=python3` 全綠(此 branch 仍 --no-timing)。記 baseline:跑 `make -C sim/verilator run-tb-top SCENARIO=AX4-BAS-005_multi_id_single_beat_sequential PYTHON3=python3`,存其 `perf.json` 的 `noc.links` 與 axi backpressure 值,供 Task 1 parity 比對。

---

### Task 1: perf monitor 改逐拍 live push(link + axi backpressure)

> 先在**現行 --no-timing build** 做+驗 → 與 timing 切換解耦。修法對兩個 simulator 都對。

**Files:**
- Modify: `sim/sv/link_perf_monitor.sv`
- Modify: `sim/sv/axi_perf_monitor.sv`
- Verify (read): `c_model/include/wrap/perf_collector.hpp`、`sim/c/cmodel_dpi.cpp`(`cmodel_perf_link` / `cmodel_perf_axi_backpressure`)

**Interfaces:**
- Consumes:DPI `cmodel_perf_link(name, flit_count, stall_cyc)`、`cmodel_perf_axi_backpressure(slot, sw_idle, mr_idle)`。
- Produces:兩 monitor 改為逐拍推 next-value running total、移除各自 `final`;聚合不再依賴 end-of-sim `final` 順序。

- [ ] **Step 1: 確認 DPI 為 set 語意**

Run:
```bash
grep -nA6 'void cmodel_perf_axi_backpressure' E:/05_NoC/noc_project/sim/c/cmodel_dpi.cpp
grep -nA4 'set_link\|set_axi_backpressure\|backpressure' E:/05_NoC/noc_project/c_model/include/wrap/perf_collector.hpp | head
```
Expected:`cmodel_perf_link` → `g_perf.set_link(...)`(覆寫);`cmodel_perf_axi_backpressure` → 對應 set-by-slot(覆寫)。若 backpressure 是 accumulate(`+=`)→ STOP 回報(需先把 collector 改 set,否則逐拍 push 會爆加);若是 set → 繼續。

- [ ] **Step 2: 改 `link_perf_monitor.sv` 為 NBA-safe live push**

把現有 `always_ff` + `final`(原 :19-37)改為:
```systemverilog
    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            flit_count <= 0; stall_cyc <= 0; credit <= BUFFER_DEPTH;
        end else begin
            // next-value (pre-NBA) so the live push is not one cycle short.
            automatic longint next_flit  = flit_count + (valid ? 1 : 0);
            automatic longint next_stall = stall_cyc  + ((credit == 0) ? 1 : 0);
            flit_count <= next_flit;
            credit     <= credit - (valid ? 1 : 0) + (credit_pulse ? 1 : 0);
            stall_cyc  <= next_stall;
            // live push (set/last-write-wins): final cycle's call carries the total.
            cmodel_perf_link(LINK_NAME, next_flit, next_stall);
        end
    end
```
刪除原 `final cmodel_perf_link(...)`(原 :37)。assertion(原 :33-35)保留不動。

- [ ] **Step 3: 改 `axi_perf_monitor.sv` 為 NBA-safe live push**

在其 `always_ff @(posedge clk_i)` 的 `else`(reset 後)分支,於更新 `slave_write_idle`/`master_read_idle`(原 :48-50)處改為 next-value + live push:
```systemverilog
            automatic longint next_sw_idle = slave_write_idle + ((wvalid && !wready) ? 1 : 0);
            automatic longint next_mr_idle = master_read_idle + ((rvalid && !rready) ? 1 : 0);
            cyc              <= cyc + 1;
            slave_write_idle <= next_sw_idle;
            master_read_idle <= next_mr_idle;
            cmodel_perf_axi_backpressure(SLOT_NAME, next_sw_idle, next_mr_idle);
```
（保留既有 per-txn `cmodel_perf_axi_txn` 推送 :68/:76 不動。）刪除原 `final cmodel_perf_axi_backpressure(...)`(原 :84-85)。

- [ ] **Step 4: build + ctest gate**

Run: `make check PYTHON3=python3`(stale 則先 `make clean-verilator`)
Expected: ctest 544/544;build-verilator 綠。

- [ ] **Step 5: perf parity 驗證(對 baseline)**

Run:
```bash
make -C sim/verilator run-tb-top SCENARIO=AX4-BAS-005_multi_id_single_beat_sequential PYTHON3=python3
```
比對其 `perf.json` 的 `noc.links`(flit_count/stall_cyc)與 axi `slave_write_idle`/`master_read_idle` 對開工前 baseline:**值相同**(live push 與舊 final 等價)。不同 → 診斷 NBA off-by-one(回 Step 2/3)。

- [ ] **Step 6: Commit**

```bash
git add sim/sv/link_perf_monitor.sv sim/sv/axi_perf_monitor.sv
git commit -m "fix(cosim): live-push link + axi backpressure perf (drop final-block ordering dependency)"
```

---

### Task 2: 合併 tb_top_vcs→tb_top + Verilator 切 --timing(atomic)

> 一旦 tb_top 自帶 clock,舊 `main.cpp` 打 clk 即衝突且 `clk_i` 不再是 port → 合併 / main / Makefile / VCS -top 必須**同一 task 一起改**(中間態不 build)。

**Files:**
- Modify: `sim/sv/tb_top.sv`(併入 self-clock/reset/timeout/perf/final/FSDB;clk/rst 改內部 logic、移出 port list)
- Delete: `sim/sv/tb_top_vcs.sv`
- Replace: `sim/verilator/main.cpp`(改極簡 --timing event-loop)
- Modify: `sim/verilator/Makefile`(`--no-timing`→`--timing --output-split 0`、carry workarounds、SV_SRC 去 tb_top_vcs、移除 VM_TRACE 編譯路徑)
- Modify: `sim/vcs/Makefile`(`-top tb_top_vcs`→`tb_top`、source 去 tb_top_vcs.sv)
- Reference: `sim/verilator/main_genamba.cpp`(event-loop 範本)、`sim/sv/tb_top_vcs.sv`(被併入的內容來源)

**Interfaces:**
- Produces:單一 `tb_top`(無 port,自帶 clock),Verilator `Vtb_top`(--timing)、VCS `-top tb_top` 共用。`main.cpp`:`commandArgs → Vtb_top → event-loop → final()`。
- Consumes:Task 1 修好的 monitor(link/backpressure 已 live);`tb_top_vcs.sv` 現有 self-clock/perf/final/FSDB 內容。

- [ ] **Step 1: 併 self-clock 段進 `tb_top.sv`**

把 `tb_top.sv` 的 module header(原 `module tb_top ( input logic clk_i, input logic rst_ni );`,:28-30)改為**無 port** `module tb_top;`,並在 module 內最前面宣告 + 自驅(內容取自 `tb_top_vcs.sv`):
```systemverilog
module tb_top;
    logic clk_i  = 1'b0;
    logic rst_ni = 1'b0;
    always #5 clk_i = ~clk_i;
    initial begin
        repeat (4) @(posedge clk_i);
        rst_ni = 1'b1;
    end
    localparam int unsigned TIMEOUT_CYCLES = 100000;
    initial begin
        repeat (TIMEOUT_CYCLES) @(posedge clk_i);
        $fatal(1, "tb_top: timeout after %0d cycles", TIMEOUT_CYCLES);
    end
```
（其餘 `.clk_i(clk_i)`/`.rst_ni(rst_ni)` 子模組接線不變 — 現在 bind 到內部 logic。）

- [ ] **Step 2: 併 perf + finalize + FSDB 段進 `tb_top.sv`**

把 `tb_top_vcs.sv` 的 perf imports / per-posedge `cmodel_perf_sample_tick` + `perf_cycle` / `final`(perf_set_run + perf_dump + finalize)/ `ifdef FSDB_DUMP` 整段(原 tb_top_vcs.sv:38-64,74-82)搬進 `tb_top.sv`(module 內、子模組實例化之後)。`$fsdbDumpvars` 的 scope 由 `tb_top_vcs` 改 `tb_top`。

- [ ] **Step 3: 刪 wrapper**

```bash
git rm sim/sv/tb_top_vcs.sv
```

- [ ] **Step 4: 改 `sim/verilator/main.cpp` 為極簡 --timing event-loop**

整檔取代為:
```cpp
// Verilator --timing entry for the self-clocked tb_top. Clock/reset/timeout/
// perf/finalize all live in tb_top.sv; this just advances the event loop.
#include "Vtb_top.h"
#include "verilated.h"
#include <memory>

// Legacy SystemC timestamp stub (must NOT call VerilatedContext::time()).
double sc_time_stamp() { return 0.0; }

int main(int argc, char** argv) {
    auto contextp = std::make_unique<VerilatedContext>();
    contextp->commandArgs(argc, argv);  // forwards +scenario_node*/+perf_* to SV
    contextp->threads(1);
    auto top = std::make_unique<Vtb_top>(contextp.get());
    while (!contextp->gotFinish()) {
        top->eval();
        if (!top->eventsPending()) break;
        contextp->time(top->nextTimeSlot());
    }
    top->final();  // fires tb_top SV final (perf dump + cmodel_finalize)
    return contextp->gotError() ? 1 : 0;
}
```

- [ ] **Step 5: 改 `sim/verilator/Makefile`**

- `VERILATOR_FLAGS`:`--no-timing`(原 :81)→ `--timing --output-split 0`。
- 保留 `-Wl,--stack,67108864`、`$(MAKE) -C`、`$(EXEEXT)`;把 genamba target 的 Windows path `sed` workaround(原 :214-217)套到 tb_top 的兩段 build(若 Verilator 同樣吐 backslash 路徑)。
- `SV_SRC`/`TB_TOP_SV_SRC`:確認不含 `tb_top_vcs.sv`(已刪;`sources.mk` 本就不含 wrapper,VCS 才加)。tb_top 自驅後 `tb_top_vcs.sv` 不再需要加入 Verilator 來源。
- 移除 `main.cpp` 的 `VM_TRACE`/`+vcd` 編譯路徑(新 main 已無)。(VCD 的 Makefile target 由 Task 3 清。)

- [ ] **Step 6: 改 `sim/vcs/Makefile`**

`-top tb_top_vcs` → `-top tb_top`(原 :159);source 清單去 `$(COSIM_ROOT)/sv/tb_top_vcs.sv`(原 :155,:160);header 註解(原 :12)同步。

- [ ] **Step 7: clean + build + gate(含 VCS dry-run)**

```bash
make clean-verilator PYTHON3=python3
make check PYTHON3=python3
```
Expected: build-verilator 經正常 `make` 在 `--timing` 下成功(無 STATUS_STACK_OVERFLOW);ctest **544/544**。
VCS dry-run:
```bash
cd sim/vcs && make -n tb_top PYTHON3=python3; cd - >/dev/null
```
Expected: `-top tb_top`,來源無 `tb_top_vcs.sv`,路徑解析無 "file not found"。

- [ ] **Step 8: perf parity(links + backpressure 在 --timing 下有值)**

```bash
make -C sim/verilator run-tb-top SCENARIO=AX4-BAS-005_multi_id_single_beat_sequential PYTHON3=python3
```
Expected:`perf.json` 的 `noc.links` flit_count/stall_cyc **有值且**等於 Task 1 baseline;axi `slave_write_idle`/`master_read_idle` 有值。per-transaction latency 與 baseline 一致。

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "refactor(cosim): unify on self-clocked tb_top under Verilator --timing; retire tb_top_vcs + --no-timing main"
```

---

### Task 3: 清 Verilator VCD Makefile 設施

> 新 main 已無 VCD;殘留的 VCD make target 會讓 `TRACE=1`/`run-all-trace` 假性失敗。

**Files:**
- Modify: `sim/verilator/Makefile`(移除/retarget VCD 設施)

- [ ] **Step 1: 移除 VCD 設施**

`sim/verilator/Makefile` 移除:`TRACE`/`VM_TRACE`/`TRACE_SUFFIX`/`TRACE_FLAGS`/`TRACE_PLUSARG_TB_TOP`(原 :15-37 附近)、`run-tb-top` 內 `+vcd` 相關行、`run-all-trace` target(要求 `tb_top.vcd`,原 :290-330/:306)。波形改由 VCS/FSDB 提供(README 註明)。

- [ ] **Step 2: gate**

Run: `make check PYTHON3=python3`
Expected: 綠(VCD 無關 ctest)。
Run: `grep -nE 'VM_TRACE|TRACE_PLUSARG|run-all-trace|\+vcd|\.vcd' sim/verilator/Makefile`
Expected: 空(或僅無害註解)。

- [ ] **Step 3: Commit**

```bash
git add sim/verilator/Makefile
git commit -m "build(cosim): drop Verilator VCD targets (waveforms via VCS/FSDB)"
```

---

### Task 4: 文件 / README 同步 + 最終殘留掃描

**Files:**
- Modify: `docs/architecture.md`、`docs/development.md`、`docs/issue/ARCHITECTURE.md`、`README.md`

- [ ] **Step 1: 改寫敘述**

把「`tb_top` simulator-neutral(clk/rst input port)+ `main.cpp` 打 clk + `tb_top_vcs` wrapper + `--no-timing` + `TRACE=1` VCD」敘述,改為:**單一 self-clocked `tb_top`**,Verilator(`--timing`)與 VCS 共用 `-top tb_top`;`main.cpp` = minimal event-loop entry;波形走 VCS/FSDB。涵蓋 `docs/development.md` 的 dual-simulator 段、`docs/architecture.md` 的 cosim 段、`ARCHITECTURE.md` §4、`README.md` quick-start。

- [ ] **Step 2: 最終殘留掃描**

```bash
git ls-files | grep -vE '^(docs/internal/|cross-review/|\.superpowers/|docs/slides/|docs/issue/)' \
  | xargs grep -InE 'tb_top_vcs|--no-timing|VM_TRACE|TRACE=1|main_tb_top_vcs|obj_dir_timing' 2>/dev/null
```
Expected: 空(除歷史/process 目錄)。命中即修。

- [ ] **Step 3: gate + Commit**

```bash
make lint_docs PYTHON3=python3
git add -A
git commit -m "docs: sync to unified self-clocked tb_top (drop tb_top_vcs/--no-timing/VCD)"
```

---

## Self-Review(plan vs spec)
- Spec ① ↔ Task 1(link + axi,NBA-safe live push,parity 驗)。✅
- Spec ② ↔ Task 2(合併/刪 wrapper/main/Makefile/VCS -top,atomic;carry workarounds)。✅
- Spec ③ ↔ Task 2 Step 7 gate(ctest 仍 `Vtb_top`,無需改 test)。✅
- Spec ④ ↔ Task 2 Step 6(VCS -top)。✅
- Spec ⑤ + VCD Makefile cleanup ↔ Task 3 + Task 4。✅
- Codex Important 1(NBA next-value)→ Task 1 Step 2/3;Important 2(axi 同病)→ Task 1 Step 3;Important 3(VCD makefile)→ Task 3。✅
- No placeholder:DPI set 語意於 Task 1 Step 1 驗(唯一條件分支,有明確 STOP 準則)。
- Type/名稱一致:`Vtb_top`、`tb_top`、`cmodel_perf_link`/`cmodel_perf_axi_backpressure` 跨 task 一致。
