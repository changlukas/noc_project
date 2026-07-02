# pulp AXI VIP test node — design

Date: 2026-07-02
Branch: `feat/verilator-5048-axi-sv-bfm` (Stage 2 of `IMPLEMENTATION_PLAN.md`)
Status: rev2 (Codex review 修正後), pending user approval

## Motivation

自製 C++ AxiMaster / AxiSlave 無法背書 AXI4 spec conformance。改用 pulp-platform/axi
`axi_test.sv` VIP(SHL-0.51)承擔 stimulus、response、checking;testbench 佈局照
FlooNoC `floo_axi_test_node` pattern。原則:pulp / FlooNoC 元件原封不動,自製 glue
只做訊號接線與格式轉換,不做 protocol 邏輯。

## Decisions

| # | 決策 | 依據 |
|---|------|------|
| D1 | Verilator 5.048;constrained-random run 跑 WSL Linux(自建 5.048 + z3)。Windows 的 Verilator solver pipe 依賴 `fork()`,不可用 | source 驗證 `_VL_SOLVER_PIPE` 只在 `__unix__`;WSL 實測 `std::randomize with{}` solve 成功 |
| D2 | directed 覆蓋 = seeded `axi_rand_master` config matrix(pulp 官方 testbench 原生用法)。v1 matrix 只含 data-integrity/transport 兩個 run class;outstanding/wait-cycle/alignment 旋鈕軸 = backlog。既有 25 個 `sim/test_patterns` 留檔不刪;`axi_file_master` 轉換器待證明缺口後再做(backlog) | pulp repo 無現成 directed stimulus 檔(`axi_file_master` 零使用者);旋鈕覆蓋 burst/EXC/ordering/outstanding/wait-cycle/對齊 |
| D3 | 雙 checker 依 run 型態配對(下表)。data-integrity run 位址依 region contract 保證 disjoint | `MAPPED=1` 拒 WRAP 且一律回 `RESP_OKAY`(source `$error` 實錘);per-node scoreboard 在共享位址下失明(Codex 確認) |
| D4 | 對稱 per-node 佈局。改動範圍:`user_node_endpoint.sv` 換內臟並新增 `end_of_sim_o` port;`gen_tb_top.py` 改寫(endpoint 接線、exit 邏輯 cutover、region/seed stamp)。fabric、`ni_wrap`/`nmu_wrap`/`nsu_wrap`、topology YAML 不動 | 現行 tb 已是每 node 一 master+slave;`user_node_endpoint.sv` 本為 user-editable 接縫 |

## Scope 排除

| 項目 | 理由 |
|------|------|
| ATOP | `ni_signals_pkg::axi_req_t` 無 `awatop` 欄位;ATOP 為 AXI5/pulp 擴充,非 AXI4 base。全部 run `AXI_ATOPS=0` |
| user signal | struct 無 `awuser/wuser/...` 欄位;VIP 端 `UW=1` tie-off,不比對 |

## Run type × checker 配對

| Run class | Slave 參數 | Checker | 驗什麼 |
|-----------|-----------|---------|--------|
| data-integrity(INCR/FIXED、ORD、STR、QOS) | `MAPPED=1` | pulp `axi_scoreboard` per node(ReadCheck/BRespCheck/RRespCheck) | end-to-end 資料正確 |
| transport(WRAP、EXC、error-response) | `MAPPED=0, RAND_RESP=1` | FlooNoC `axi_reorder_compare`(mst 端 vs slv 端逐欄位比對 + same-ID order) | fabric 對 addr/lock/resp 等欄位搬運不失真 |

transport run 限 **permutation pairing**(master m 只打 node p(m),一對一):`axi_reorder_compare`
靠成對 stream 歸屬(FlooNoC 原生 2-node 用法),16 masters all-to-all 時 slave 端交錯流量
無法歸屬(ID 空間重疊)。all-to-all transport 驗證 = backlog。

歸類依據:
- WRAP:MAPPED mode `$error` 拒收。
- EXC:exclusive ownership 語意屬 endpoint slave,非 DUT(fabric+NI)責任;DUT 責任 = `awlock/arlock` 與 resp code 原樣搬運,reorder_compare 正是驗這個。MAPPED mode 一律 `RESP_OKAY`,無法回 `EXOKAY`。
- error-response:非 OKAY response 由 `RAND_RESP=1` 隨機產生,reorder_compare 驗 resp 不被 fabric 改寫。

## Region contract(data-integrity run)

沿用現行「address 高位 bits 選 destination node」機制(`gen_test_patterns.py` 同款)。
在 destination node s 的位址空間內,per-source-master 切 disjoint 視窗:

```
region(m → s) = node_base(s) + m × W        # W = 視窗大小,generator 常數
```

non-overlap by construction;每個 master 對每個 remote node 都有視窗(all-to-all)。
`gen_tb_top.py` 從 topology YAML 算出後 stamp 進 endpoint 參數,endpoint 內呼叫
`add_memory_region()`。transport run 不受此限(reorder_compare 不需 memory 語意),
可故意用重疊位址打 same-address 交錯。

## Node 架構(user_node_endpoint 改後)

**INPUT** master/slave struct port(`ni_signals_pkg::axi_req_t/axi_rsp_t`,不變)
**COMPUTE** `AXI_BUS_DV` master_dv/slave_dv ← `axi_rand_master` / `axi_rand_slave`;
flat struct ↔ DV interface 為一次性顯式欄位接線(同現行 wrap 對 DPI 的作法,
純訊號 wiring 無 protocol 邏輯);checker 與 monitor 掛同一組訊號
**OUTPUT** 驅動 fabric NMU 的 req、回 NSU 的 rsp,`end_of_sim_o` per node

VIP timing:clk 10ns(現行 tb `always #5`),`TA=2ns`、`TT=8ns`(FlooNoC 值)。

## Monitors

| 元件 | 處置 |
|------|------|
| `axi_perf_monitor.sv`(endpoint) | 退役,換 FlooNoC `axi_bw_monitor`(latency mean±stddev、BW、util,end_of_sim `$display`,不進 perf.json) |
| `link_perf_monitor` + in-fabric PMU | 保留(業界無 in-fabric NoC counter 替代品)。`axi_bw_monitor` endpoint 數字兼作 PMU cross-check |

## End-of-sim(exit 邏輯 cutover)

舊 exit 路徑(`cmodel_done` / `cmodel_scoreboard_clean` / `cmodel_reads_checked` DPI
guard)隨 C++ scoreboard 退役,`gen_tb_top.py` 生成下列新 scheme:

1. per node `end_of_sim[i]` = 該 node `rand_master.run(N_reads, N_writes)` 返回。
   source 實錘:`run()` 為 `fork...join` 含 `recv_rs`/`recv_bs`,返回即 R/B 全收
2. transport run 另 AND `axi_reorder_compare` 的 `end_of_sim_o`(compare queue 空)
3. 全 node AND 後等 settle window(generator 常數,default 100 cycles)→ checker
   終檢 → `$finish`
4. watchdog:`TIMEOUT_CYCLES = BASE + K × (N_reads + N_writes) × NUM_NODES`
   (BASE=100000、K=200,generator 常數),逾時 `$fatal`
5. non-vacuous:終檢斷言每 node `aw_cnt + ar_cnt > 0`(hierarchical ref 取
   `axi_bw_monitor` 內部計數)

## Seed management

| 項目 | 做法 |
|------|------|
| Verilator | `+verilator+seed+<N>` runtime plusarg |
| VCS | `+ntb_random_seed=<N>`,runner 依 simulator 映射 |
| matrix.yaml | 每 entry 新增 `seed` 欄位(顯式固定值 → regression 可重現) |
| run log | 記 seed、旋鈕、topology、tool/solver 版本 |
| seed 掃蕩 / named-seed 釘精確 corner | backlog,config matrix 跑穩後 |

## File tree(含既有目錄歸位)

判斷準則:DUT 本體與化身 → `src/`;驗它的 → `sim/`;引入的 DV IP → `sim/dv/`。

```
noc_project/
├── specgen/                       # 不動
├── src/                           # NEW:DUT 全體
│   ├── c_model/                   #   ← repo 根搬入(C++ model 本體,含其 tests)
│   ├── sv/                        #   ni_wrap nmu_wrap nsu_wrap router_wrap
│   │                              #   noc_fabric_mesh_*         ← sim/sv
│   └── dpi/                       #   cmodel_dpi.cpp/.h dpi_boundary_macros.h
│                                  #   handle_block.hpp          ← sim/c(先拔 master/slave/scoreboard 段)
└── sim/
    ├── tb/                        #   tb_top_* user_node_endpoint link_perf_monitor
    │                              #   ← sim/sv 剩餘 tb 件
    ├── dv/                        # NEW:引入的 DV IP,原封不動 + 原 LICENSE + 版本釘死
    │   ├── axi-0.39.7/            #   axi_pkg.sv axi_intf.sv axi_test.sv include/axi/*.svh
    │   ├── common_verification-0.2.5/  # rand_id_queue.sv
    │   └── floonoc-test-<rev>/    #   axi_reorder_compare.sv axi_bw_monitor.sv
    ├── test_patterns/  topologies/  regress/  tools/  verilator/  vcs/
    └── filelist_*.f               # gen_filelist.py 重生
```

## 元件處置清單

| 處置 | 元件 |
|------|------|
| 新增(引入 DV IP) | 上表 `sim/dv/` 三包(WSL 實編驗證:此最小集即建得起 rand VIP,`common_cells` 不需要) |
| 改寫 | `user_node_endpoint.sv`(換內臟 + `end_of_sim_o`)、`gen_tb_top.py`(接線、exit cutover、region/seed stamp、watchdog 公式)、`gen_filelist.py` + filelist 重生(新路徑 + 退役檔移除)、`sim/build_config.mk`(路徑 + 退役檔移除)、root `CMakeLists.txt` / `Makefile`(c_model 新路徑)、`matrix.yaml` / `run_regress.py`(config-matrix entries、seed、toolchain 標籤)、路徑引用文件同步(`CLAUDE.md`、`docs/architecture.md`、`docs/development.md`) |
| 退役(co-sim 側) | `axi_master_wrap.sv`、`axi_slave_wrap.sv`、`axi_perf_monitor.sv`、`cmodel_dpi.cpp` 的 master/slave/scoreboard DPI 段與 tb exit guards、`wrap/master_wrap.hpp` / `slave_wrap.hpp` + 其 tests |
| 降級為 c_model test harness | C++ `AxiMaster` / `AxiSlave` / `Memory` / scoreboard 類保留於 `src/c_model/`,僅供 ctest 使用(15 個 unit/integration test 以其為驅動;不再出現於 co-sim wire-level testbench) |
| 搬移(內容不改) | `src/c_model/`、`src/sv/`、`src/dpi/` 如上表 |
| 不動 | fabric、NI wraps(DUT)、in-fabric PMU、`link_perf_monitor`(搬移不改內容)、topology YAML、既有 25 test_patterns(留檔) |

## 環境

- random co-sim regression 跑 WSL Ubuntu(自建 Verilator 5.048 + z3;c_model DPI、yaml-cpp、tb 均於 WSL 建置;Makefile 既有 `VERILATOR`/`PYTHON3` override 沿用)
- Windows 保留:ctest 純 C++ 單元測試、tb compile/lint gate(編譯不需 solver)
- VCS:pulp VIP 原生支援(其官方 CI 即 Questa/VCS);filelist 相容性列入實作驗證項

## 實作階段驗證項(非設計開口)

1. minimal rand probe 在 WSL 收斂驗證(先前 probe hang 疑似 `AXI_ASSIGN(slv, mst)` 直短接無真 handshake,改經 fabric 或 `axi_sim_mem` 接法重驗)
2. flat struct ↔ DV interface 接線的欄位對齊 review(一次性,實作後跑 rtl lint + 單 node loopback smoke)
3. `axi_scoreboard` 啟用細節(monitor mode、check enable API)按其 source 用法接
4. VIP TA/TT 與 DPI cycle-stepped wrap 的相位驗證(單 node smoke 看 handshake 波形)
5. VCS compile gate(filelist + macro 相容)— 本環境無 VCS,列 backlog 待 workstation 驗
