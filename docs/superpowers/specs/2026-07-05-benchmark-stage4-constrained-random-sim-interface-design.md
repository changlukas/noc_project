# Benchmark Stage 4: constrained-random axis + unified `make sim` interface — design

Date: 2026-07-05
Relates to: `docs/superpowers/specs/2026-07-04-checked-traffic-benchmark-design.md`（parent，Stage 4 = rand conformance）。本 spec 把 Stage 4 從「只加 rand gate」擴為：**加 constrained-random 軸 + 對齊 DV 命名 + 統一 test-run 介面**。三者是使用者本輪（2026-07-05）brainstorming 定案的一組。

## Motivation

Stage 3 已落 directed 軸（`axi_file_master` + `axi_scoreboard` + two-phase）。Stage 4 補上 constrained-random 軸（`axi_rand_master` + `axi_reorder_compare`），做成**可 gated、seed 可重現**的 run。同時兩個 side-ask 併入：
- **命名對齊 DV 標準**：退役自造的 `data_integrity` / `transport`，改 DV 公認的 `directed` / `constrained_random`。
- **指令太冗**：現行逐 run 要打 `RUN_CLASS=.. BUILD_ROOT=.. FILELIST_F=.. PYTHON3=.. VERILATOR=.. NUM_READS/WRITES=..` 一長串。改成單一入口 `make sim TB= PATTERN= [SEED=]`（此 target 已存在於 root Makefile:187-195，但 body 還在呼叫 D7 判死的 `run_benchmark.py` → 本輪換掉）。

## Survey basis（DV 標準，me + FlooNoC 源碼 + UVM/Verilator 文件）

| 題 | 標準答案 | 出處 |
|---|---|---|
| move-only NoC transport checker | transaction **comparator**（逐欄位 AW/W/AR/B/R + same-id 保序 + drain 排空）;非 memory scoreboard | FlooNoC `tb_floo_rob.sv` 原生用 `axi_reorder_compare` |
| CR run pass/fail | checker error-free + 發完 txn + drain（compare queue 空）+ 非空 + 無 timeout;**coverage 才是 closure 信號**（跨 seed，另一軸） | UVM CDV;Doulos |
| $error gate | 別純靠 grep（`$error` 留 rc==0）;優先 SV 端 error count → `$fatal`;grep 當防禦 | Codex/UVM report server |
| seed | 一個 top seed，time-0 印、烤進 run 名/log，同 binary 同 seed 精準重播;記下失敗 seed = bug handle | random stability（Doulos / systemverilog.io / Verilator guide） |
| naming | stimulus 軸 `directed` / `constrained_random`;checker `scoreboard`（model-based）/ `comparator`（actual-vs-actual） | UVM / Art of Verification |

## Decisions

| # | 決策 | 依據 |
|---|------|------|
| S4-1 | 命名收斂成兩軸 **`directed`** / **`constrained_random`**;退役 `data_integrity` + `transport` | DV 標準;`data_integrity` 是冗餘 INCR CR 變體，資料正確性已由 directed/scoreboard 接管;pulp scoreboard 是 memory model 只支援 INCR，不是 CR transport 的 golden（survey 共識，兩邊都建議砍） |
| S4-2 | **PATTERN 決定 run-class**：`{neighbor, transpose, uniform_random, hotspot}` → directed;`{constrained_random}` → CR。無獨立 run-class 選項 | 使用者定;PATTERN 是唯一 traffic 選擇軸，class 是其後果 |
| S4-3 | 單一入口 `make sim TB=tb_<topo> PATTERN=<pat> [SEED=<n>]`（root）;`TB` 接受 `tb_` 前綴（`$(TB:tb_%=%)` 去前綴 → topology）;host 路徑用**現有 root `local.mk`**（已 gitignored `/local.mk` + 由 `sim/build_config.mk:13-16` include，不另立新檔）;`FILELIST_F` 由 `BUILD_ROOT`+topology 推導。**class 選擇必須用 recursive make**（root `sim` 先由 PATTERN 算出 class，再 `$(MAKE) build-verilator RUN_CLASS=<class>`;不可用 target-specific override，因 `OBJ_DIR`/`TBTOP_EXE` 是 `:=` parse-time 固定，`sim/verilator/Makefile:31`） | DV 慣例（per-host config + 單一 run flow）;`sim` target 已存在，只換 body |
| S4-8 | 退役 `data_integrity` 需**同步 repoint root default 與兩個 validator**（`Makefile:124`、`sim/verilator/Makefile:15-17`、`sim/vcs/Makefile:5-8`）:root default `RUN_CLASS` 改 `constrained_random`;validator 白名單改 `directed constrained_random`。CR PATTERN **不進 emitter**（`gen_test_patterns` 只吃 4 個空間 pattern），直接跑 tb | Codex：`run_regress`/`check` 的 build leg 靠 root `build-verilator` 的預設 class（`run_regress.py:198`，不帶 RUN_CLASS），改名不同步會斷 build |
| S4-4 | `SEED` 不給 → recipe **隨機抽一個**（`$(shell python3 -c ...)`）、time-0 印出、烤進 run tag/log;給則原封重播 | random stability;記下 seed = 重現手把，不 double-run（使用者定） |
| S4-5 | CR checker 沿用現有 `axi_reorder_compare`（**tb-top 層生成**、per-master、permutation pairing、id blank/restore;非 endpoint 內，`gen_tb_top` 於 `ifndef TB_DIRECTED` 已產出）;gate = comparator error-free + compare drained（`cmp_eos`）+ 非空 | move-only NoC transport fidelity 的標準 = comparator;現行 tb 佈局即此 |
| S4-6 | CR gate 的 error 偵測 = grep `%Error`（同 directed，已證可用）+ tb 收尾要求 `cmp_eos` 全排空 | `reorder_compare` 是原封使用的 IP，不暴露 error count port，`$error` 只印 `%Error`;directed 已用 grep `%Error` 且 fault-injection 驗過。不為投機的 SV-`$fatal` 路徑加第二機制（ponytail） |
| S4-7 | 先 **fault-injection** 證 `reorder_compare` 會 fire（故意搬歪一 beat → comparator error → gate FAIL），再信 clean run | 專案 checker-bringup 紀律（同 Stage 3 fault-first） |

## Scope

| | 項目 |
|---|------|
| In | constrained-random gated run + seed 隨機抽/記錄;命名對齊（directed/constrained_random，退役 data_integrity/transport）;統一 `make sim TB= PATTERN= SEED=` + `local.mk`;把 Stage 3 `run-directed` 收進 `sim` |
| Out（Stage 5） | `matrix.yaml`/`run_regress` regression sweep（多 seed/多 topo fan-out）;coverage plan;VCS;`run_benchmark.py` 全刪（本輪只讓 `sim` 停用它） |

## 指令介面（本輪核心產出）

```
make sim TB=tb_mesh_2x4_vc1 PATTERN=hotspot            # directed;SEED 沒給 → 隨機抽並印
make sim TB=tb_mesh_4x4_vc1 PATTERN=constrained_random SEED=42
make sim TB=mesh_4x4_vc1    PATTERN=neighbor           # tb_ 前綴可省
```

| 旋鈕 | 意義 | 預設 |
|---|---|---|
| `TB=tb_<topo>` | 選 testbench = topology（去 `tb_` 前綴 → `mesh_4x4_vc1` 等） | `mesh_4x4_vc1` |
| `PATTERN=` | traffic;**同時決定 class**：4 個空間 pattern→directed、`constrained_random`→CR | `neighbor` |
| `SEED=` | 種子;**不給 → 隨機抽 + 印出 + 記錄** | 隨機抽 |

**INPUT** `make sim TB= PATTERN= [SEED=]`
**COMPUTE** `TOPOLOGY=$(TB:tb_%=%)` → 由 PATTERN 選 `+define+TB_DIRECTED` 或 `+define+TB_CONSTRAINED_RANDOM` → obj_dir 依 topology×class → `build-verilator` → directed class 先跑 emitter（`gen_test_patterns --format file_master --pattern <PATTERN>`）產 per-node stimulus → 跑 tb（帶 `+verilator+seed+<SEED>`;directed 另帶 `+stim_dir`;CR 不需 stimulus 檔）
**OUTPUT** `output/<class>_<topo>_<pattern>_s<seed>/run.log`（**log 首行印 seed**）+ `perf.json`;gate → `SIM PASS`（class 對應的 checker 乾淨 + drain + 非空）或 `SIM FAIL`;FAIL 看 tag 的 `_s<seed>` 直接重播

host 路徑一次性寫進 `root local.mk`（gitignored）：`BUILD_ROOT := $(HOME)/noc_build`、`PYTHON3 := python3`、`VERILATOR := verilator`。使用者從此不打這些。

## 命名對照（old → new）

| 概念 | new（DV 標準） | old |
|---|---|---|
| CR run-class / endpoint define | `constrained_random` / `TB_CONSTRAINED_RANDOM` | `transport` / `TB_TRANSPORT_RUN` |
| directed 軸 | `directed` / `TB_DIRECTED` | 不動 |
| 冗餘 INCR CR 變體 | 退役 | `data_integrity`（default run-class） |
| checker | `scoreboard`（directed）/ `reorder_compare` comparator（CR） | 不動（IP 原名，已標準） |
| 入口 | `make sim TB= PATTERN= SEED=`（改 body） | 舊 body 呼叫 `run_benchmark.py`;`run-directed`/`run-tb-top` 併入 |

## Checker / gate（依 class）

- **directed**（PATTERN=空間 pattern）：Stage 3 不變 — grep `Unexpected RData|Unexpected W last|Not supported (AW|AR) burst|Atomic|%Error` + `PASS: all N nodes` + 非空。
- **constrained_random**（PATTERN=constrained_random）：comparator error-free（grep `%Error`，見 S4-6）+ `cmp_eos` 全排空（tb 收尾已要求）+ 非空。

## Seed

`+verilator+seed+N`（Verilator 每 C-thread 一個 seed）。`SEED` 未給時 recipe 用 bash **`$$RANDOM$$RANDOM`**（recipe SHELL 已是 bash;單一 `$RANDOM` 只 15-bit 太窄，Codex 指定用雙抽拼 30-bit）抽一個，印在 run 起始 + 進 tag/log/output 路徑。同 source+binary+seed → bit 可重現（但書：改 code / 換模擬器 / 換 plusarg 會使序列漂移，random stability 只在同 code+sim+seed 成立）。

## 元件處置

| 處置 | 元件 |
|------|------|
| 改寫 | root `Makefile`（`sim` target body：PATTERN→class、`TB:tb_%=%`、seed 隨機抽、gate、停用 `run_benchmark.py`;`RUN_CLASS` default;include `root local.mk`）;`sim/verilator/Makefile`（`run-directed` 邏輯併入 / 當 backend、`transport`→`constrained_random`、退役 `data_integrity`、RUN_CLASS filter）;`sim/vcs/Makefile`（RUN_CLASS filter 同步改名，VCS 為 Linux-workstation dry-run）;`user_node_endpoint.sv`（`TB_TRANSPORT_RUN`→`TB_CONSTRAINED_RANDOM`，刪 `data_integrity` 專屬分支）;`gen_tb_top.py`（comment/naming） |
| 新增 | 無新檔;host config = gitignored `root local.mk`（使用者手建 3 行），範本寫成 root Makefile 的一段註解，不另立 `.example` 檔（ponytail） |
| 退役 | run-class 名 `data_integrity`/`transport`;`sim` 對 `run_benchmark.py` 的依賴（腳本本體 Stage 5 D7 刪） |
| 不動 | fabric / NI wraps（DUT）、`axi_reorder_compare` / `axi_scoreboard` / `axi_rand_master` / `axi_rand_slave` IP、emitter、in-fabric PMU |

## Stages（本輪 = parent Stage 4;plan 會拆 task）

| # | 子階段 | success criteria |
|---|-------|------------------|
| a | 命名 + endpoint + 預設 | `TB_TRANSPORT_RUN`→`TB_CONSTRAINED_RANDOM`、刪 `data_integrity` 專屬分支;**明示行為變更**：非-directed 預設 flavor 從舊 `data_integrity`(INCR+MAPPED) 變成 `constrained_random`(WRAP/EXC/RAND_RESP);root default `RUN_CLASS` + verilator/vcs validator 同步改（S4-8）;`build-verilator` 預設編得過 |
| b | CR gate + fault-first | fault-injection 證 `reorder_compare` fire（搬歪一 beat → gate FAIL）;clean CR run 4x4 comparator 乾淨 + drain + 非空 |
| c | 統一 `make sim` | `make sim TB= PATTERN= [SEED=]` 跑 directed（4 pattern，沿用 Stage 3 gate 全綠）與 constrained_random 皆 PASS;`local.mk` 生效;seed 未給隨機抽並記錄、給則重播一致 |

## 環境

WSL（Verilator 5.048 + z3）;host 路徑進 `root local.mk`（`BUILD_ROOT=$HOME/noc_build`，見 [[feedback_run_on_wsl_linux]] 的 COFF/BUILD_ROOT gotcha）。Windows 只保留 Python/lint。

## 風險

| 風險 | 處置 |
|------|------|
| 統一 `sim` 重塑 Stage 3 入口 | Stage 3 directed 行為必須不變 → 子階段 c 重跑 directed 4-pattern 仍全綠當 regression gate |
| 退役 `data_integrity`/`transport` 撞到現有 consumer | **已查（2026-07-05）：consumer 僅 3 個 Makefile（root/verilator/vcs 的 `RUN_CLASS`）+ endpoint define/comment，全在本輪改寫清單內。`sim/regress/`（run_regress.py + matrix.yaml）完全 run-class-agnostic（grep 零命中）→ 退役對 Stage 5 harness 無衝擊** |
| `sim` 停用 `run_benchmark.py` 但腳本還在 | 本輪只斷 `sim` 的呼叫（`sim` 從「benchmark runner」變「DV run launcher」= 明示的 user-visible 行為變更;失去 YAML scenario flow / `bench_summary.json` / `--from`）;`make check` 仍呼叫 `run_benchmark.py`（`Makefile:182`）不動;腳本本體 Stage 5 D7 刪 |
| 改 root default class → `run_regress`/`check` 的 build leg 跟著變 | `run_regress.py:198` 用不帶 RUN_CLASS 的 `build-verilator`，吃 root 預設。預設從 INCR+MAPPED 變 constrained_random 後，過渡期（Stage 4→5）run_regress 建/跑的是新 flavor;run_regress 屬 Stage 5 重寫標的、且現況已 red，過渡不一致可接受，列 Stage 5 交接 |
