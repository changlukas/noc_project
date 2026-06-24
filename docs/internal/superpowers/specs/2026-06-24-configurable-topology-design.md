# Configurable NoC Topology + Generated Testbench — Design

- Date: 2026-06-24
- Driver: NoC 拓樸從寫死 2-node 變成**可設定 N×M mesh**;testbench 由 generator **程式化產生**,不再手寫。
- Survey basis: generator-from-config 為開源/學術 NoC 主流(FlooNoC/FlooGen、CONNECT、OpenSMART、Constellation);純 SV `generate` 只適規則 mesh 且 Verilator interface-array 支援脆弱。詳見 memory `project_topology_configurable_survey`。
- 術語對齊:FlooNoC(wormhole、VC、credit-based flow control、NI)、AMBA AXI4(beat、burst、AxQOS)。

## 1. 範圍

**In scope:**
- 規則 **N×M mesh**,沿用既有 **XY routing**(X-first)。
- **multi-VC** 接出到 wire-level co-sim(c_model 已實作,wire 層被壓在 single-VC)。
- Python generator 產 `noc_fabric_<topo>.sv`(fabric)+ `tb_top.sv`(test harness)。
- 前置:specgen 參數改名去冗餘 `NI_` 前綴。

**Out of scope(留擴充欄位,本輪不做):**
- torus / 不規則 graph / routing table。
- 部分佈點(每節點皆 full endpoint)。
- QoS-driven VC mapping 調優(沿用既有 `VcArbiter` policy)。

## 2. 設計決策

| # | 決策 | 理由 |
|---|---|---|
| D1 | 拓樸家族 = 規則 N×M mesh,XY routing | router core 已 5-port mesh + XY-ready(`router.hpp`);torus 會拖出 deadlock/multi-VC 隔離,範圍暴增 |
| D2 | multi-VC 與 mesh 同輪 | wire 層 DPI marshalling 一定要重寫(1 link→4 方向);per-VC 與方向化改**同一段**,一次到位免碰兩次 |
| D3 | generator **展開 explicit SV instances**(非 SV generate) | 避開 Verilator interface-array 雷;任意連線可展;產物可讀可 diff |
| D4 | 產物為 generated artifact,納 **drift gate** | 沿用 `filelist.f` / specgen `codegen.py --check` 慣例 |
| D5 | 拓樸維度 source = specgen `MESH_X_DIM`/`MESH_Y_DIM` | 與 flit format 同住 specgen,binding 用 L2 invariant 保證(D6),單一真相 |
| D6 | flit format 為拓樸硬上限,以 specgen **L2 invariant** 強制 | x/y 綁 flit:`DST_ID_WIDTH`/`VC_ID_WIDTH` 決定可定址節點/VC 數 |
| D7 | generator 範圍 = NI(NMU+NSU)+ router + mesh wiring = **fabric**;每節點露 AXI port 空殼 | fabric 可重用;test master/slave 或 user IP 插同一個 AXI port |
| D8 | 前置改名 `NI_NOC_*`→`NOC_*`、`NI_AXI_*`→`AXI_*` | `NI_` 與 `ni::` namespace / `ni_params_pkg::` package 重複 |

## 3. Flit binding invariant（D6）

flit header 用 **flat node id**,非獨立 x/y 欄位(`ni_flit_constants.h`):

| 欄位 | 寬度 | 上限 |
|---|---|---|
| `DST_ID_WIDTH` | 8 | ≤ 256 nodes |
| `VC_ID_WIDTH` | 3 | ≤ 8 VC |

- node id 由 x/y bit-split 組成:低 `X_WIDTH` bits = x,高 `Y_WIDTH` bits = y。`X_WIDTH + Y_WIDTH = DST_ID_WIDTH`(`addr_trans.hpp:18`;X_WIDTH=4、Y_WIDTH=4、DST_ID_WIDTH=8)。
- specgen 新增 L2 invariant:
  - `MESH_X_DIM ≤ 2^X_WIDTH`、`MESH_Y_DIM ≤ 2^Y_WIDTH`。
  - `MESH_X_DIM × MESH_Y_DIM ≤ 2^DST_ID_WIDTH`。
  - `NUM_VC ≤ 2^VC_ID_WIDTH`。
- 違反 → `codegen.py --check` fail,不默默截斷。
- `DST_ID_WIDTH` 維持 8(`addr_trans.hpp:20` 有 `static_assert(DST_ID_BITS==8)`);本輪改的是 mesh 維度,非 dst 編碼寬度。

## 4. Generator（D3/D4/D7）

**輸入:** specgen 常數(`MESH_X_DIM`/`MESH_Y_DIM`/`NUM_VC`/flit 欄位)。
**工具:** `sim/tools/gen_tb_top.py`,`--check` drift gate。

**產物 1 — `noc_fabric_<topo>.sv`(可重用 fabric):**
- N = `MESH_X_DIM × MESH_Y_DIM` 個 node,每 node = NMU + req/rsp router + NSU。
- 相鄰 router 間方向 link 連線(N/E/S/W);邊界未用方向 tie-off。
- edge tie-off **配 assertion**:未接的方向 port 若收到 valid flit 即報錯(否則 `Router::tick` 不 drain 該 output FIFO → 變 timeout 而非即時錯,`router.hpp:201,203`)。XY routing 在 `dst < mesh_dim` 內保證不指向不存在鄰居(`router.hpp:64` 已 bounds-check),assertion 為 defense-in-depth。
- 每 node 對外露一個 AXI port(整合邊界)。
- DPI handle(`ctx`)由 **port 收入**,fabric 不自行 `cmodel_*_create`(create 留給 instantiator)。

**產物 2 — `tb_top.sv`(test harness):**
- instantiate fabric + 每 node 接 test `master_wrap`/`slave_wrap` + scoreboard。
- 每 node 一個 `+scenario_node<i>=` plusarg;`cmodel_*_create` 在此呼叫。
- PASS guard = 生成的 expected-master count(非寫死 2)。
- per-node PMU/link monitor:生成名字 + 方向化 credit 對向配對。

## 5. DPI ABI（D2，一次定型）

S2 一次把 router 的 **link DPI port 全部改成 per-direction(`[PORT]` indexed)** 形,credit 再 × `[NUM_VC]`;S3 只把多出來的方向接上(wiring),不再改 signature。**只改 credit、不改 valid/flit 的話,S3 仍會被迫改 signature** —— 故 valid/flit 也要在 S2 一併 `[PORT]` 化。

- `cmodel_router_create(name, x, y, mesh_x, mesh_y, num_vc)`(現只收 `x`,`RouterWrap` 寫死 y=0/mesh 2x1)。
- `cmodel_router_set_inputs` / `get_outputs`:link 的 **valid / flit / credit 全部 `[PORT]` 化**;credit 再 × `[NUM_VC]`。
  - NI↔router credit **介面**早已 per-VC(`noc_intf.req/rsp_credit_return[NUM_VC]`,`ni_signals_pkg.sv:141`),但 **DPI marshalling + C++ wrap 仍傳標量、寫死 `cfg.num_vc=1`**(`cmodel_dpi.h:135`、`nmu_wrap.hpp:47`、`nsu_wrap.hpp:52`)→ 要改 marshalling + wrap config,不只接線。PoC 的 `{NUM_VC{...}}` 壓扁改為原樣搬 c_model 的 `credit_[port][vc]`。
  - router↔router LINK credit(pulse)由 1-bit 加寬為 per-VC。
- 前向資料維持單 flit/cycle(vc_id 在 header),只有 credit 是 per-VC。

## 6. 分階段（每階段一個新未知數,前一個 gate 綠才下一步）

| 階段 | 內容 | gate |
|---|---|---|
| **S-1** 前置改名(D8) | `NI_NOC_*`→`NOC_*`、`NI_AXI_*`→`AXI_*`;改 source + 11 處 + SV | `codegen.py --check` 綠;ctest 全綠;repo 無 `NI_NOC_`/`NI_AXI_` 殘留 |
| **S0** generator 重現 2-node | `gen_tb_top.py` 產出**行為等價**今日 single-VC tb(非 byte-identical;手寫配對改由 config 推導)。先給 `run_regress.py` 加 **run-count 斷言**(run 0 視為 fail) | co-sim **跑滿 6 且全 PASS**(run-count 斷言生效) |
| **S1** scenario/config 接入 `num_vc` | scenario/config schema 加 `num_vc` 欄位 + 接到 co-sim wrap 參數(為 S2 鋪路)。c_model NMU→NSU multi-VC 路徑**已由 `test_request_response_loopback`/`test_router_loopback` 的 `MultiVc` instantiation 涵蓋(num_vc {2,4,8})**,不重證 | 既有 `MultiVc` 測試保持綠;`num_vc` 由 config 解析並傳入 wrap |
| **S2** wire-level multi-VC | DPI per-VC(§5);拔 `router_wrap`/`nmu_wrap`/`nsu_wrap` 三處 `$fatal`;NMU/NSU/router C++ wrap 的 `num_vc` 由單一來源穿入;`run_regress.py` 傳 `num_vc`(plusarg);指名一個 multi-VC co-sim scenario 當 driver | 指名 scenario 在 num_vc=2 co-sim 綠(run-count 斷言) |
| **S3** directional ports | wrap/DPI link valid/flit/credit 全 `[PORT]` 化(§5)1 link → N/E/S/W;generator 連 2×2、邊界 tie-off + assertion(§4) | 2×2 co-sim 綠(run-count 斷言) |
| **S4** N×M parameterized | per-node scenario 指派:node i base offset = `node_id_i << 32`(`node_id = (y<<X_WIDTH)\|x`),generator 指 scenario_i→node i;推廣 `gen_coordinate_scenarios` 的 2-node `NODE1_OFFSET` | N×M(N 或 M >2)co-sim 綠(run-count 斷言) |

## 7. Codex blind-spot review — 已 fold

| 發現 | 處置 | 落點 |
|---|---|---|
| multi-VC wire 層不只 router_wrap:`nmu_wrap`/`nsu_wrap` 亦 `$fatal`、取 credit`[0]`;C++ wrap `cfg.num_vc=1`(`nmu_wrap.hpp:47`/`nsu_wrap.hpp:52`) | S2 涵蓋三個 wrap + 單一 NUM_VC 來源 | §5、S2 |
| `cmodel_router_create` 只收 `x`,`RouterWrap` 寫死 y=0/mesh 2x1(`router_wrap.hpp:61,64`) | create ABI 擴 `(x,y,mesh_x,mesh_y,num_vc)` | §5 |
| S2/S3 DPI signature 改兩次 | ABI 一次定 per-direction×per-VC | §5 |
| S0 做不到 byte-identical(tb 有手寫配對:`cmodel_init(scn_node1)`、master 吃對側、crossed link、src 固定 0/1) | S0 目標改行為等價 | S0 |
| edge tie-off 未接 port 被 route → `Router::tick` 不 drain → timeout(`router.hpp:201,203`) | 加 routing invariant(mesh XY 不指向不存在鄰居)+ assertion | §4、§7-test |
| fabric/tb 拆檔 vs handle 所有權(ctx 在 tb_top 建) | fabric 收 ctx port,create 留 tb_top | §4 |
| PASS guard 寫死 2 master / PMU 2-node 名 | generator 產 master count + 方向化 monitor 名 | §4 |
| scenario gen 2-node 寫死 + run_regress 跳無變體 dir vs make 動態生 | S4 統一 per-node scenario 生成,納同一 drift gate | S4 |
| burst/STR wire bug 與 S2 c_model matrix 交集 | 不以 BUR/STR co-sim fail 當 multi-VC credit 改壞證據;先隔離已知 burst path | S2 risk |

## 8. Success criteria

- `make check` ctest 全綠;specgen drift gate 綠;新 L2 invariant 生效(超限 config 會 fail)。
- `gen_tb_top.py --check` drift gate 綠;手改 generated SV 會被擋。
- `run_regress.py` 有 **run-count 斷言**:實跑數低於預期(含 0)即 fail,不再「跳過即 pass」。
- co-sim(皆斷言 run count):single-VC 6 全 PASS(S0);指名 scenario num_vc=2 綠(S2);2×2 mesh 綠(S3);至少一個 N×M(N 或 M >2)綠(S4)。
- repo 無 `NI_NOC_`/`NI_AXI_` 殘留(S-1);無新增 single-VC 寫死。
- edge tie-off 有 assertion/invariant,錯誤拓樸即時報錯而非 timeout。

## 9. 風險

- **wire-level multi-VC 首次驗證**:c_model 已驗(unit + `test_request_response_loopback` num_vc {1,2,4,8}),wire 層從未跑。S1→S2 序確保 bug 落在 marshalling 而非 VC 邏輯。
- **2D mesh deadlock**:XY(dimension-ordered)routing 對 2D mesh deadlock-free 為標準結果;需確認既有 req/rsp 雙網路 + 單一 turn 限制無違 XY。
- **known burst co-sim bug** 與 S2 scenario 重疊,見 §7。
