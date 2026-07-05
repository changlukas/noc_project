# Checked traffic benchmark on pulp VIP: scoreboard (directed) + reorder_compare (random) — design

Date: 2026-07-04
Relates to: `docs/2026-07-02-pulp-axi-vip-node-design.md`（VIP cutover）。本 spec 接續其 checker 決策：保留
`axi_reorder_compare` 給 random 路徑，另加 `axi_scoreboard` 給 directed 路徑（雙 checker，非單一）

## Motivation

VIP cutover 換了 driver（`axi_rand_master`）與 checker（`axi_reorder_compare`），但 regression/benchmark
harness（`matrix.yaml` / `run_regress.py` / `gen_test_patterns.py`）仍停在退役的「YAML scenario → C++
scoreboard」flow，與新 tb 斷線（新 tb 不讀 scenario、不印舊 PASS 字串）。本 spec 在 VIP 上重建 benchmark
能力，**每個 regression run 都帶比對**：所有 spatial traffic pattern（含 hotspot/uniform）帶資料完整性比對、
random conformance 帶 transport 比對。（唯一非比對角落 random×all-to-all 見 Driver×pattern，非 regression run）

需求兩軸並存：
- spatial traffic pattern benchmark（neighbor / uniform_random / hotspot / transpose）
- constrained-random conformance

## Decisions

| # | 決策 | 依據 |
|---|------|------|
| D1 | driver 依 stimulus 性質分：deterministic pattern → `axi_file_master`；constrained-random → `axi_rand_master`。兩者都 write+read | file_master replay 固定清單、rand_master 亂數；pattern 一旦 seed 固定即 deterministic |
| D2 | **checker 依 driver 分**：directed(`axi_file_master`) → `axi_scoreboard`（data integrity）；random(`axi_rand_master`) → `axi_reorder_compare`（transport fidelity）| 兩者物理互斥：reorder_compare 需 1:1 attribution，驗不了多對一；scoreboard 需 readback，驗不了隨機（讀打到未寫位址）。move-only NoC：directed 用 readback 核帳、random 用進出對拍。FlooNoC 原生即 rand→reorder_compare（`tb_floo_rob.sv`）|
| D3 | directed/scoreboard 路徑 invariant：**寫位址按 src 分割**（無兩 master 寫同位置）→ golden 恆單一 writer → 含 hotspot/uniform 皆可 data-check。random/reorder_compare 路徑不需分割，只需 permutation pairing（1:1）| write-write race 是「多對一不可 data-check」唯一根因；分割即消除。reorder_compare 不 readback，無此需求 |
| D4 | directed two-phase 執行：全寫 → barrier → 全讀，讀相才能驗寫相。**barrier 為 per-node（非 cross-node）**：每 node 自己 `wait_b` 後才發 AR | AXI 不保證 W-before-R ordering；scoreboard 讀相需 golden 已建立。random 路徑不需（reorder_compare 逐 beat 對，不 readback）。**per-node 已證足夠（Stage 3 cross-review 2026-07-04）**：MAPPED slave 於送 B 前已 commit W 至 memory（`axi_test.sv:1494/1508/1517`），且 D3 令每 master 只讀自己寫過的位址 → node A 的讀不依賴任何他 node 的寫，cross-node barrier 為多餘 port/wiring。原「全系統寫 drain」措辭收斂為 per-node|
| D5 | emitter 自寫（Python，`gen_test_patterns` 換 output emitter，走現有 **synthetic 路徑無 `--from`**，`_emit_synthetic_node`），出 file_master `$fscanf` 格式。**scoreboard 前提約束：INCR-only、atop=0、full readback（讀集合⊇寫集合）、full strobe、`data=f(addr)`** | 上游無 file_master generator；synthetic 路徑已 LIVE（不需 base scenario）；scoreboard 只 model INCR、拒 atomic（`axi_test.sv:2036,2112`）；pattern 數學沿用 |
| D7 | **全面換新，舊不留**：刪 `sim/test_patterns/` 全部舊 AX4 + Layer-2 scenario YAMLs、`run_benchmark.py`、`gen_test_patterns` 的 `--from` base-driven 路徑。emitter 生成物改放 `sim/test_patterns/` | 舊 corner（BND/EXC/QOS/RSP）覆蓋移交 rand_master conformance；directed 只需 synthetic INCR。user 定案不保留舊物 |
| D6 | ~~scoreboard 2-state 相容性 = gating spike~~ **RESOLVED 2026-07-04（spike）**：directed two-phase full-readback clean 案 = **0 warning**；fault 案（讀未寫 `0x2000`）= 8× `Unexpected RData`（axi_test.sv:2142，checker live）。**scoreboard 直接可用於 Verilator directed 軸，無需 patch、無需退 VCS** | X-collapse 只咬未寫位址讀；directed 讀集合⊆寫集合 + full strobe 天生避開。backlog 0c 的「2-state incompatible」對受控 directed 案 REFUTED |

## Scope

| | 項目 |
|---|------|
| In | file_master + scoreboard + two-phase（directed 資料軸）、rand_master + reorder_compare（random transport 軸）、emitter、harness 改寫 |
| Out（本 spec） | injection-rate / saturation sweep（獨立 round）、non-4x4 topology、SAM remap |

排除理由同 2026-07-02 spec：ATOP（struct 無 `awatop`）、user signal（struct 無 `*user`）。

## Checker model

兩把 checker，按 driver 路徑各驗一種正確性。

### directed → `axi_scoreboard`（data integrity）

單介面 monitor：監看一條 AXI 介面，W beat 更新 golden memory，R beat 對 golden 檢查
（`enable_read_check` / `enable_b_resp_check` / `enable_r_resp_check`）。

**INPUT** 掛在每個 master face（`master_dv`），一 node 一 scoreboard
**COMPUTE** golden 由該 master 的 W 建立（intended data）；該 master 的 R 對 golden 比對
**OUTPUT** mismatch ⇒ NoC 在寫 / 讀傳輸改寫了資料。**失配動作是 `$warning` 非 `$error`（`axi_test.sv:2141`）
→ harness gate 必須 grep mismatch 訊息，不能靠 sim 退出碼**

master-face 掛法即端到端：golden = 意圖寫入，check = 繞 NoC 回來的值。invariant D3 保證此 master 只讀自己
寫過的 slice → golden 恆命中、無 cross-master 汙染。

**可觀測性前提（Codex item 2）**：D3 消除 write-write 歧義，但「檢查得到」≠「每個 corruption 都看得見」
—— 未讀回 / readback 前被覆寫 / 讀覆蓋不足的 byte 仍可藏 corruption。故 emitter 必須保證**讀集合涵蓋寫集合
（full readback）**、W 用 **full strobe** 覆蓋所有稍後會讀的 byte。

### random → `axi_reorder_compare`（transport fidelity）

per-master monitor：比對 master-face 送出的 beat 與其到達 slave-face 的樣子（逐欄位 + same-id order），
**不建 memory model、不 readback**。因此隨機讀打到未寫位址對它無妨 —— 它只驗「bits 有沒有被 NoC 搬歪」。

**前提**：permutation pairing（master m 只打 node p(m)），checker 才能把 slave-face handshake 歸屬到唯一
master（沿用現行 tb 佈局，FlooNoC `tb_floo_rob.sv` 原生用法）。id 兩側塗銷（`id='X` 雙邊，2-state-safe）。

## Addressing / partitioning（invariant D3 的實現）

沿用現行定址：`addr[63:32] = dst_id`（選 destination tile），`addr[31:0] = local offset`。

| pattern 類 | 分割方式 | 現況 |
|---|---|---|
| permutation（neighbor / transpose）| 一 dst tile 僅一 src → 天生 disjoint | pattern 數學已具 bijection 性質 |
| hotspot / uniform（多 src → 一 tile）| tile 內每 src 一 disjoint offset slice | `alloc_unique_offset`（column in src），上輪 slot-overlap fix 已加固 |
| random conformance（reorder_compare）| 不需位址分割（不 readback）；只需 permutation pairing（1:1）| 現行 tb 佈局即此 |

payload `data = f(addr)`（deterministic function，如 address-in-data）：讀相期望值為純函數，避開 golden 對
「未寫位址」的依賴。

## Stimulus file 格式（file_master `$fscanf`，emitter 產出）

```
# write 檔（per txn，12 行 ax + len+1 個 W beat；parse_write）
id / addr / len / size / burst / lock / cache / prot / qos / region / atop / user
  （各一行十進位，addr 為 0x…；burst=INCR(1)、atop=0 固定）
接 len+1 個 W beat："0x<data> 0x<strb> <user>"（full strobe）
# read 檔（per txn，11 行 ax —— parse_read 無 atop 欄位，`axi_test.sv:2488-2498`）
id / addr / len / size / burst / lock / cache / prot / qos / region / user   （無 W beat）
```

per node 一組 (write 檔, read 檔)，輸出 `sim/test_patterns/<scenario>/node<i>/{write,read}.txt`；endpoint 經
plusarg 收檔名餵給 `load_files(read, write)`。emitter 走 synthetic 路徑（無 `--from`），依 pattern 算 dst、依 src
分割 offset、填 `data=f(addr)`，**不依賴任何 base scenario**。生成物 gitignored（更新 `.gitignore`）。

## Two-phase 執行（D4）

file_master 的 `run()` 為五路平行 fork（run_aw/run_w/run_ar/wait_b/wait_r），不保證寫先於讀。改在 tb 端呼叫其
**既有 public task** 分兩相，不改 pulp IP：

```
phase-1（write）:  fork run_aw; run_w; wait_b; join
barrier:           全 node write phase 完成（end_of_write[i] AND）
phase-2（read）:   fork run_ar; wait_r; join   ← scoreboard 於此相檢查 R
```

barrier 跨 node，確保讀相開始時全系統寫已 drain、golden 完整。

呼叫約束（Codex item 3）：先 `load_files()` 建 queue，再跑兩相 task；**不可再呼叫 `run()`**（它會 fork 五路
重新消耗同一 queue）。`b_outst`/`r_outst` 由 `load_files()` 解析時填、由 `wait_b`/`wait_r` 排空。

## Driver × pattern

| stimulus | driver | pattern | checker |
|---|---|---|---|
| deterministic | `axi_file_master` | neighbor / transpose / uniform / hotspot（**INCR-only, atop=0**）| **scoreboard**（data integrity）|
| constrained-random | `axi_rand_master` | AXI conformance corner（隨機 burst/size/addr，限自 region）| **reorder_compare**（transport）|

未涵蓋角落：**random × all-to-all**（rand_master 打所有 region）兩 checker 皆罩不到（reorder_compare 不能歸屬多對一、
scoreboard 不能驗隨機）→ 若需，只能 perf-only。all-to-all 資料完整性由 directed uniform 涵蓋、corner 隨機由
permutation-paired random 涵蓋，故非缺口。

## Harness 改寫

`matrix.yaml` entry 欄位：`{driver, pattern, topology, rob_mode, seed, num_txn}`。
`run_regress.py`：
- file_master entry：先呼叫 emitter 產 (write, read) 檔 → 跑 tb → gate = **無 scoreboard read-data mismatch `$warning`**（grep stdout，`axi_test.sv:2141` 附近訊息）
- rand_master entry：帶 seed / num_txn 跑 tb → gate = **無 reorder_compare `$error`** + compare queue drained（`end_of_sim_o`）
- 兩者皆加 non-vacuous `txn_cnt>0`；scoreboard 只 `$warning`，不能靠退出碼
- directed INCR-only 下不應出現 scoreboard unsupported-burst warning；若出現＝emitter 產了非法 stimulus，視為 fail（不豁免）
- 移除舊 scenario-YAML resolve / C++ scoreboard PASS-字串 gate / `is_self_checking` 過濾

## 元件處置

| 處置 | 元件 |
|------|------|
| 原封使用 | `axi_file_master`、`axi_rand_master`、`axi_rand_slave`(MAPPED=1)、`axi_scoreboard`、`axi_reorder_compare` |
| 改寫 | `user_node_endpoint.sv`（compile-time define 選 driver，仿現有 `TB_TRANSPORT_RUN`；file→scoreboard + two-phase，rand→reorder_compare）、`gen_tb_top.py`（checker 接線、region/partition stamp、exit 改 two-phase barrier）、`gen_test_patterns.py`（換 file emitter）、`matrix.yaml` / `run_regress.py` |
| 退役 / 刪除 | 舊 `run_benchmark.py` scenario flow、舊 C++ scoreboard exit guard、**`sim/test_patterns/` 全部舊 AX4 + Layer-2 scenario YAMLs**（D7；corner 覆蓋移交 rand_master）、`gen_test_patterns` 的 `--from` base-driven 路徑 |
| 不動 | fabric、NI wraps（DUT）、in-fabric PMU、`link_perf_monitor`、`axi_bw_monitor`、topology YAML |

## File tree（本輪產出落點）

```
sim/
├── dv/                              # 引入 IP，不動（本輪全用現成，零新增依賴）
│   ├── axi-0.39.7/src/axi_test.sv   #   axi_file_master / axi_rand_master / axi_scoreboard / axi_rand_slave
│   └── floonoc-test/                #   axi_reorder_compare / axi_bw_monitor
├── tb/
│   ├── user_node_endpoint.sv        # 改寫：compile-time define 選 file/rand，掛 checker + two-phase
│   └── tb_top_mesh_*.sv             # gen_tb_top 重生：checker 接線、two-phase barrier exit
├── tools/
│   └── gen_test_patterns.py         # 改寫：pattern 數學沿用，output emitter 換 file_master $fscanf 格式
├── regress/
│   ├── matrix.yaml                  # 改寫：entry = {driver, pattern, topology, rob_mode, seed, num_txn}
│   ├── run_regress.py               # 改寫：emitter 呼叫 + checker gate（grep）
│   └── output/                      # run log / summary（gitignored `sim/*/output/`）；stimulus 不放這
└── test_patterns/                   # 全面換新（D7）：刪舊 AX4/Layer-2 YAMLs，改放 emitter 生成的 file_master 檔
    └── <scenario>/node<i>/
        ├── write.txt                #   ← emitter 產（Stage 2），directed run stimulus，gitignored
        └── read.txt                 #   ← emitter 產，gitignored
```

- **新增檔案：0**（driver/checker/slave 全用 `sim/dv/` 現成；emitter 是改 `gen_test_patterns.py`）。
- **刪除（D7）**：`sim/test_patterns/` 全部舊 AX4 + Layer-2 YAMLs、`run_benchmark.py`、`gen_test_patterns` `--from` 路徑。
- **生成物**：`sim/test_patterns/<scenario>/node<i>/{write,read}.txt`，runtime 產、gitignored（更新 `.gitignore` 涵蓋 node0/1）。
- Stage 1 spike 用單 node tb 變體驗 scoreboard，屬 throwaway，不進 tree。
- 本 design doc：`docs/.../specs/2026-07-04-checked-traffic-benchmark-design.md`；plan 將落 `docs/superpowers/plans/`。

## Stages（gating）

| # | stage | success criteria |
|---|-------|------------------|
| 1 | ~~scoreboard 2-state spike~~ **DONE 2026-07-04** | clean readback 0 warning、fault(未寫位址讀) 8× Unexpected RData → scoreboard 直接可用於 Verilator directed 軸（plan `2026-07-04-scoreboard-2state-spike.md`）。無 fallback |
| 2 | ~~emitter~~ **DONE 2026-07-04**（commits `fcdfbe4..75f1549`）| `--format file_master` 出 write/read.txt；4 pattern × per-src 分割；address-in-data；寬度讀 constants.yaml SSoT；add-only（legacy YAML 未動）；55 test green；plan `2026-07-04-benchmark-stage2-emitter.md` |
| 3 | ~~file_master path~~ **DONE 2026-07-04** | endpoint `TB_DIRECTED` flavor（file_master + in-endpoint scoreboard on master_dv + **per-node** two-phase，barrier per-node 見 D4）；gen_tb_top `ifdef TB_DIRECTED` guard（reorder_compare compiled out，cmp_eos 保留 unconditional）；Makefile `RUN_CLASS=directed` + `run-directed` recipe（fail-loud guard against 非 directed 誤跑 vacuous PASS）。**驗證**：fault-injection 先證 scoreboard fires（寫回值改 1 bit → `Unexpected RData`）；clean 1x1 self-route + 4x4 全 4 pattern（neighbor/transpose/uniform_random/hotspot）scoreboard clean、16 node non-vacuous、0 %Error/timeout；4x4 directed 首建+跑 46s（~8300 scoreboard coroutine 無 OOM/hang）。plan `2026-07-04-benchmark-stage3-file-master-path.md` |
| 4 | ~~rand conformance~~ **DONE 2026-07-05**（`aead67e..<launcher commit>`）| 2-flavor 命名（`directed`/`constrained_random`，退役 `data_integrity`/`TB_TRANSPORT_RUN`）；`run-constrained-random`（rand_master WRAP/EXC + tb-level `reorder_compare`、`%Error` gate，fault-injection 先驗 checker 會 fire）；root `make sim TB=tb_<topo> PATTERN=<pat> [SEED=]` 統一入口（recursive make 分派 `run-directed`/`run-constrained-random`，SEED 未給隨機抽+記錄）；4x4 全 5 軸（4 directed pattern + constrained_random）+ no-SEED 隨機種子跑通 |
| 5 | harness | `matrix.yaml`/`run_regress` 驅動兩 driver，全 build checker gate（scoreboard / reorder_compare）|

## 環境

- constrained-random 與 co-sim regression 跑 WSL（Verilator 5.048 + z3），沿用現行 `VERILATOR`/`PYTHON3` override
- Windows 保留 ctest 純 C++ 與 tb compile/lint gate
- VCS：pulp VIP 原生支援；若 Stage 1 判定 scoreboard 僅 4-state 可用，spatial 資料完整性軸落 VCS

## 實作階段驗證項（非設計開口）

1. scoreboard monitor mode / check-enable API 按其 source 用法接（`enable_read_check` 等）
2. two-phase barrier 與 file_master task 生命週期的相位（單 node 波形驗寫相 drain → 讀相 R 檢查）
3. `data=f(addr)` 與 `wstrb` 對齊（narrow / unaligned burst 下 golden byte coverage）
4. VCS filelist / macro 相容（本環境無 VCS，列 backlog 待 workstation）

## 風險

| 風險 | 處置 |
|------|------|
| ~~scoreboard 2-state 誤報不可 patch~~ **RESOLVED 2026-07-04** | spike 證 directed full-readback 下 scoreboard 在 Verilator 不誤報（clean 0 warning / fault 8× warning）；無需 patch/VCS fallback |
| directed data-check 涵蓋 transport 但不驗 same-id ordering fidelity | ordering fidelity 由 random 的 reorder_compare 涵蓋（same-id order 檢查）；兩軸互補 |
| emitter 為我方唯一 SV 外自製碼 | 純 Python、沿用舊 pattern 數學、產上游規定格式，不觸 DUT/SV |
| 刪舊 AX4 後，BND/EXC 等 directed corner 從「確定命中」變 rand_master「機率命中」 | rand_master 多 seed 覆蓋；若需釘死特定 corner（如 4KB 邊界精確位址），Stage 4 後評估補少量 directed conformance 檔（backlog） |
