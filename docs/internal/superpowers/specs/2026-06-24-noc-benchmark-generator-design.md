# NoC Benchmark Generator — Design

- Date: 2026-06-24
- Driver: 在已 merge 的 configurable-topology 基礎上,新增**程式化 NoC traffic benchmark**(spatial pattern + per-pattern 效能彙整)。
- Builds on: `gen_tb_top.py`(生 `noc_fabric_<topo>` + `tb_top_<topo>`)、`user_node_endpoint.sv`(每 node 的 master/slave/monitor)、`perf.json`(latency/throughput/occupancy)、`scoreboard`(write→readback 正確性)。
- 術語對齊:BookSim2(traffic pattern)、AMBA AXI4。survey 見 memory `project_topology_configurable_survey` 的 benchmark 段。

## 0. Scope

**In scope:**
- 修正 scenario-ownership 契約(§1,destination address-driven)。
- `gen_test_patterns.py`:generated traffic pattern + user-custom 兩機制(§2)。
- benchmark runner + perf summary(§3,greedy 單點)。
- 檔案對齊 + 測試(§4/§5)。

**Out of scope(→ 下一輪):**
- injection-rate 控制 / saturation curve(latency-vs-offered-load)。需 AxiMaster 注入排程,動 c_model 介面。
- functional/code coverage、CRV。

## 1. Scenario-ownership 契約修正（前置,修現有設計 bug）

**問題**:現在 `gen_tb_top` 把 `master_i` 餵 `scn_node{(i+1)%N}`、`slave_{(i+1)}` 從同檔建(`gen_tb_top.py:473`)。destination 由這個**寫死的 ring 配對**決定,不是由 address 決定 → 只能產 ring traffic,任意 pattern 做不到。

**修正**:destination **完全由 transaction address 決定**(`addr[39:32]=dst_id`,`addr_trans::xy_route` + `route_compute` 已支援任意 mesh dst)。
- **identity 配對**:`master_i` ← `scn_node{i}`、`slave_i` ← `scn_node{i}`。
- `scn_node{i}` 檔完整描述 node i:`config.memory_base` = node i 的 slave 記憶體(coord i 區);transactions 的 `addr = dst_coord<<32 + local_offset`(dst 由 pattern 決定)。
- master_i 寫+讀**同一** dst address → scoreboard(以絕對位址比對 write/readback)照驗,與哪個 slave 實體儲存無關。

**做減法:不保留 ring 特例。** 現有 6-scenario 正確性 co-sim 改用 `gen_test_patterns` 的**確定性 pattern(`neighbor`)** 表達 destination(每 node 寄給固定鄰居),取代 `gen_coordinate_scenarios` 的 ring/coordinate 專用編碼。behavior-preserving 的定義 = **6 個 AXI conformity scenario 仍 scoreboard clean**(正確性覆蓋不變),不是「同樣的 ring 流量」。`gen_coordinate_scenarios.py` **刪除**(功能併入 `gen_test_patterns`)。

**影響面**:改 `gen_tb_top.py`(identity 配對)、**刪** `gen_coordinate_scenarios.py`、原子更新其 call sites(`run_regress.py`、Verilator/VCS Makefile)改呼叫 `gen_test_patterns`;**不動** c_model / DPI / scenario 格式 / scoreboard。

## 2. gen_test_patterns.py（兩機制）

**輸入**:`--topology <name>`(讀 N、x_dim、y_dim、X_WIDTH)、`--pattern`、`--seed`、`--transactions-per-node`、AXI shape(size/len,給最小預設)。
**輸出**:`<out>/node<i>/scenario.yaml`(現有格式),供 §1 identity 配對消費。

### 2a. generated traffic（`--pattern`）

對 node i 產 `transactions-per-node` 筆 write+read pair,每筆的 **dst 由 pattern 取樣**,addr = `dst_coord<<32 + uniq_local_offset`:

兩類:**確定性**(每 node 每筆固定 dst,可重現)vs **per-packet 隨機**(每筆各自取樣,seed-stable)。
**演算法一律 port booksim2 `src/traffic.cpp`(不自 derive,確保正確性)**:在 (x,y) 空間套用 booksim 語意,再映 `addr=((dst_y<<X_WIDTH)|dst_x)<<32 + uniq_off`。

| pattern | 類型 | 每筆 dst(port booksim) | 備註 |
|---|---|---|---|
| `neighbor` | **確定性** | `NeighborTrafficPattern`(`traffic.cpp:316`):每維 +1 mod dim → (x,y)→((x+1)%x_dim,(y+1)%y_dim) | bijection、非 self;**correctness 回歸用它** |
| `transpose` | 確定性 | `TransposeTrafficPattern`(`traffic.cpp:244`,id bit-half-swap)→ 方形下 (x,y)→(y,x) | guard:需偶次方方形 mesh(4x4 ✓);對角 (x,x)→self,benchmark、隱含 `--allow-self`;correctness smoke 不用它 |
| `uniform_random` | per-packet 隨機 | `UniformRandom`:`RandomInt(nodes-1)`(排除 self) | load balance、bisection |
| `hotspot` | per-packet 隨機 | `HotSpotTrafficPattern`(`traffic.cpp:490`):加權選 `--hotspot` node | sink 壅塞;**correctness smoke 用它** |

- **per-packet 隨機**:uniform/hotspot 每筆 transaction 各自取樣(seed 可重現);neighbor/transpose 為確定性。
- **self-traffic**:預設排除(dst≠src);transpose 對角隱含允許;`--allow-self` 全域開關。
- **位址唯一性(全域)**:任何 convergent pattern(多源→同 dst node)都可能撞同一絕對位址 → 每筆 write/read 對在 dst node `memory_base..+memory_size` 內分到**全域唯一 local offset**(以 (src,seq) 決定),非僅 hotspot。write/read 同址成對。dst 位址須落在該 node 的 memory 區內。
- **guard**:`x_dim×y_dim ≤ 2^DST_ID_WIDTH`、transpose 需方形 mesh;違反即 fail-fast。

### 2b. user-custom（`--from <base scenario.yaml>`）

`gen_test_patterns` 把「payload」與「destination」當兩個正交軸:
- **payload**:synthetic(§2a 的 size/len 預設)**或** `--from <base>`(user 手寫的 AXI 行為)。
- **destination**:`--pattern`(uniform/transpose/neighbor/hotspot)把 dst 編進 address。

組合:**benchmark run** = synthetic payload × 隨機/結構 pattern;**correctness 回歸** = conformity base(`--from`)× `neighbor`(確定性)。這完全取代 `gen_coordinate_scenarios`(刪除,無 wrapper);一支工具涵蓋兩種需求。

## 3. Benchmark runner + perf summary

`sim/tools/run_benchmark.py`(或 run_regress 的 `--benchmark` mode):
- **build**:該 topology 的 tb(若未 build)。
- **run**:生成的 pattern,傳全 N 個 `+scenario_node<i>`。
- **correctness gate(必要)**:`PASS: scoreboard clean` + master_count==N + reads_checked≥N(沿用既有 PASS guard;DECERR/SLVERR 不計入 reads → gate 守得住)。
- **perf**:讀 `perf.json` → per-pattern 彙整 latency(mean / p95 / max;p95 由 `latency.transactions` 算,CLI 目前只 min/mean/max)+ throughput(byte/txn)+ link/occupancy。輸出到 `output/<scenario>/bench_summary.json`。
- **誠實標示**:v1 注入是 **greedy 有限-trace stress run**(send-while-pending,max_outstanding 上限),**非 offered-load 飽和量測**;summary 標明此語意 + 量測 window(`perf.json` window 現為 `[0,total_cyc)` 含 reset/drain)。saturation curve → 下一輪。

## 4. 檔案對齊

- `gen_*` 全進 `sim/tools/`(`gen_filelist.py` 移入)。
- **刪 `gen_coordinate_scenarios.py` + 其單元測試 `test_gen_coordinate_scenarios.py`**(功能由 `gen_test_patterns` 取代,無 wrapper;改寫成 `test_gen_test_patterns.py`);**同一改動原子更新**所有 call sites(`run_regress.py:21,63`、Verilator/VCS Makefile `run-tb-top`)改呼叫 `gen_test_patterns`(correctness 用 `neighbor`)。
- `run_regress`(正確性回歸)保留,但改用 `gen_test_patterns`;benchmark runner(效能)新增。

## 5. 測試 / gate

- **pattern dst 公式單元測試**(對 booksim 語意):uniform 覆蓋範圍 + 排除 self;transpose (x,y)→(y,x);neighbor 每維 +1 mod dim(bijection、非 self);hotspot 集中於指定 node + 每源唯一位址。
- **guard 測試**:非方形 transpose / 超 flit 容量 → fail-fast。
- **非-ring smoke(必要)**:`hotspot` 在 `mesh_4x4_vc1` 實跑 → routing 正確 + **scoreboard clean** + perf summary 產出(non-vacuous)。選 hotspot(非 transpose,避開對角 self 模糊)。光單元測試抓不到配對風險,必須有真跑。
- **回歸**:契約修正(§1)後,6-scenario co-sim 改用 `neighbor` 確定性分佈,scoreboard clean + ctest 保持綠。

## 6. Success criteria

- §1 契約修正後:mesh_4x4_vc{1,2,4,8} co-sim 仍全綠(改用 `neighbor` 確定性分佈);destination 由 address 決定(可驗:改 scenario dst → 流量改向)。
- `gen_test_patterns` 4 pattern 各產 per-node scenario;guard 對非法 topology/pattern fail-fast。
- 非-ring smoke(transpose/hotspot)在 4x4 co-sim scoreboard clean + perf summary 產出。
- per-pattern perf summary(latency mean/p95/max + throughput)正確標示為 greedy stress run。
- ctest + 既有 co-sim 回歸綠;`gen_coordinate_scenarios` 刪除且所有 call sites 改到 `gen_test_patterns`。

## 7. 風險

- **契約修正觸及已 merge 的 gen_tb_top + 刪 gen_coordinate**:behavior-preserving 的 gate = 6 個 conformity scenario 在 `neighbor` 分佈下仍 scoreboard clean(覆蓋不變,非重現同樣流量)。call sites 須原子更新,否則回歸 runner 斷。
- **hotspot 位址規劃**:多源 → 同 dst node,需在該 node 記憶體區無重疊分配;memory_size 要夠。
- **per-packet random 的 scoreboard 量**:transactions-per-node × N 筆都要 write+read 成對且唯一址,reads_checked 才非 vacuous。
