# pulp AXI VIP test node — design

Date: 2026-07-02
Branch: `feat/verilator-5048-axi-sv-bfm` (Stage 2 of `IMPLEMENTATION_PLAN.md`)
Status: approved by user, pending implementation plan

## Motivation

自製 C++ AxiMaster / AxiSlave 無法背書 AXI4 spec conformance。改用 pulp-platform/axi
`axi_test.sv` VIP(SHL-0.51)承擔 stimulus、response、checking;testbench 佈局照
FlooNoC `floo_axi_test_node` pattern。原則:pulp / FlooNoC 元件原封不動,自製 glue
只做格式橋接,不做 protocol 邏輯。

## Decisions

| # | 決策 | 依據 |
|---|------|------|
| D1 | Verilator 5.048;constrained-random run 跑 WSL Linux(自建 5.048 + z3)。Windows 的 Verilator solver pipe 依賴 `fork()`,不可用 | source 驗證 `_VL_SOLVER_PIPE` 只在 `__unix__`;WSL 實測 `std::randomize with{}` solve 成功 |
| D2 | directed 覆蓋 = seeded `axi_rand_master` config matrix(pulp 官方 testbench 原生用法)。既有 25 個 `sim/test_patterns` 留檔不刪;`axi_file_master` 轉換器待證明缺口後再做(backlog) | pulp repo 無現成 directed stimulus 檔(`axi_file_master` 零使用者);旋鈕覆蓋 burst/EXC/ATOP/ordering/outstanding/wait-cycle/對齊 |
| D3 | 雙 checker 依 run 型態配對(下表)。data-integrity run 各 master `add_memory_region` 給 disjoint 位址段 | `MAPPED=1` 拒 WRAP/ATOP 且一律回 `RESP_OKAY`(source `$error` 實錘);per-node scoreboard 在共享位址下失明(Codex 確認,現行 C++ 方案靠單顆 global scoreboard 迴避) |
| D4 | 對稱 per-node 佈局:只換 `user_node_endpoint.sv` 內臟,`gen_tb_top.py`、fabric、`ni_wrap`、topology YAML 不動 | 現行 tb 已是每 node 一 master+slave;`user_node_endpoint.sv` 本為 user-editable 接縫 |

## Run type × checker 配對

| Run class | Slave mode | Checker | 驗什麼 |
|-----------|-----------|---------|--------|
| data-integrity(INCR/FIXED、EXC、ORD、STR、QOS) | `MAPPED=1` | pulp `axi_scoreboard` per node(ReadCheck/BRespCheck/RRespCheck) | end-to-end 資料正確 |
| transport(WRAP、ATOP、error-response) | `MAPPED=0` | FlooNoC `axi_reorder_compare`(mst 端 vs slv 端逐欄位比對 + same-ID order) | fabric 搬運不失真 |

error-response 類歸 transport 組:MAPPED mode 無法產生非 OKAY response。

## Node 架構(user_node_endpoint 改後)

**INPUT** master/slave struct port(`ni_signals_pkg::axi_req_t/axi_rsp_t`,不變)
**COMPUTE** `AXI_BUS_DV` master_dv/slave_dv ← `axi_rand_master` / `axi_rand_slave`;
`AXI_ASSIGN_TO_REQ` / `AXI_ASSIGN_FROM_RESP` 巨集橋 struct ↔ DV interface;
checker 與 monitor 掛同一組 DV/struct 訊號
**OUTPUT** 驅動 fabric NMU 的 req、回 NSU 的 rsp,`end_of_sim_o` per node

rand_master 目標位址段由 `gen_tb_top.py` 從 topology YAML 既有 slot 計算 stamp 進
endpoint 參數(資料來源不變,多一個輸出口)。

## Monitors

| 元件 | 處置 |
|------|------|
| `axi_perf_monitor.sv`(endpoint) | 退役,換 FlooNoC `axi_bw_monitor`(latency mean±stddev、BW、util,end_of_sim `$display`,不進 perf.json) |
| `link_perf_monitor` + in-fabric PMU | 保留(業界無 in-fabric NoC counter 替代品)。`axi_bw_monitor` endpoint 數字兼作 PMU cross-check |

## End-of-sim

1. per node `end_of_sim[i]` = `rand_master.run(N_reads, N_writes)` 返回。source 實錘:`run()` 為 `fork...join` 含 `recv_rs`/`recv_bs`,返回即該 node R/B 全收(自帶 drain)
2. tb 頂層 `wait(&end_of_sim)` → checker 終檢 → `$finish`
3. watchdog 沿用 `TIMEOUT_CYCLES` pattern,額度隨 N txns 縮放,逾時 `$fatal`
4. non-vacuous:終檢斷言每 node `aw_cnt + ar_cnt > 0`(取自 `axi_bw_monitor` 計數)

## Seed management

| 項目 | 做法 |
|------|------|
| Verilator | `+verilator+seed+<N>` runtime plusarg |
| VCS | `+ntb_random_seed=<N>`,由 runner 依 simulator 映射 |
| matrix.yaml | 每 entry 顯式 seed(固定 → regression 可重現) |
| run log | 記 seed、旋鈕、topology、tool/solver 版本 |
| seed 掃蕩 / named-seed 釘精確 corner | backlog,config matrix 跑穩後 |

## 元件處置清單

| 處置 | 元件 |
|------|------|
| 新增(vendored 至 `sim/vendor/<name>-<version>/`,原封不動) | pulp axi 0.39.7(`axi_test.sv`、`typedef.svh`、`assign.svh`、`axi_pkg` 等)、common_cells 1.39.0、common_verification 0.2.5(`rand_id_queue_pkg`)、FlooNoC `axi_reorder_compare.sv`、`axi_bw_monitor.sv`;各附原 LICENSE,版本釘死 |
| 改寫 | `user_node_endpoint.sv`(換內臟)、`gen_tb_top.py`(region 參數 stamp、end_of_sim 收斂、seed plusarg)、`matrix.yaml` / `run_regress.py`(config-matrix entries、seed 欄位、toolchain 標籤) |
| 退役 | `axi_master_wrap.sv`、`axi_slave_wrap.sv`、`axi_perf_monitor.sv`、C++ `AxiMaster` / `AxiSlave` / `Memory` / scoreboard 及其 DPI entry points |
| 不動 | fabric(`noc_fabric_*.sv`、`router_wrap`)、`ni_wrap` / `nmu_wrap` / `nsu_wrap`(DUT)、in-fabric PMU、`link_perf_monitor`、topology YAML、既有 25 test_patterns(留檔) |

## 環境

- random co-sim regression 跑 WSL Ubuntu(自建 Verilator 5.048 + z3;c_model DPI、yaml-cpp、tb 均於 WSL 建置;Makefile 既有 `VERILATOR`/`PYTHON3` override 沿用)
- Windows 保留:ctest 純 C++ 單元測試、tb compile/lint gate(Verilator 5.048 編譯不需 solver)
- VCS:pulp VIP 原生支援(其官方 CI 即 Questa/VCS);filelist 相容性列入實作驗證項

## 實作階段驗證項(非設計開口)

1. minimal rand probe 在 WSL 收斂驗證(先前 probe hang 疑似 `AXI_ASSIGN(slv, mst)` 直短接無真 handshake,換經 fabric 或 sim_mem 的接法重驗)
2. `axi_bw_monitor` 吃 pulp nested struct(`req_i.ar.id`),與 `ni_signals_pkg` 平面 struct 不同型 — 用 `axi/typedef.svh` 產 per-config pulp struct + `AXI_ASSIGN` 轉出,零手寫欄位搬運
3. `axi_scoreboard` 的啟用細節(monitor mode、check enable API)按其 source 用法接
4. VCS compile gate(filelist + macro 相容)
