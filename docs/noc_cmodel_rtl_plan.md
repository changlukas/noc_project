# NoC C-Model 與 RTL 整合計畫

本文件接續 `c_model` 既有進度,規劃 NMU / NSU / Router 從純 C model 到 RTL 混合 co-sim 的完整路徑。型別與 pin 契約引用 `specgen` 生成的 `ni_flit_constants.h`、`ni_signals.h`、`ni_regs.h`(在 `specgen/generated/cpp/`)與對應的 `specgen/generated/sv/*.sv`,這三組由 codegen 保證 C 側與 SV 側 byte-identical 且受 golden gate 約束。

AXI 行為的設計依據,是查證開源 NoC NI（FlooNoC）的 RTL 與 AMD PG313 / PG406 後的結論:NMU/NSU 對 AXI 是透明 transport,不是 memory-decode。

設計原則承襲既有規範,OSS-first survey、不重新設計 AXI standard、runtime validation 只做合規監控、文件精簡、每個 commit 必須通過 drift gate。

---

## 0. AXI 邊界的定性（查證結論）

查證的開源 NoC NI 把整個 AW channel 原樣放進 flit payload,W 也原樣轉發、只多帶一個 last 標記,全程沒有 per-beat 位址計算、memory bounds 檢查、DECERR 生成或 burst 拆解。AMD PG406 對 NMU 的 read reorder buffer 描述,是以 per-AXI-ID 結構依 AXI ordering 重排 response。PG313 的 NMU 做 packetize、QoS arbitration、address routing、address remapping、CDC。

因此 NMU/NSU 的 AXI 是一個具體的中間帶,比純 handshake 多,比完整 memory-decode 少。

需要做的：五 channel 的 valid/ready handshake、完整 AXI channel 屬性的透明搬運、wlast/rlast 的 burst framing、per-AXI-ID 的 response 重排、回程 ID 管理、address translation、QoS/VC 仲裁、CDC、可選 ATOP。

不需要做的：`beat_addr` 的 FIXED/INCR/WRAP per-beat 位址生成、memory bounds 檢查、OOB 的 DECERR 生成、slave 端 burst 拆解、WSTRB 對 memory 的 sparse 合法性。這些是 memory endpoint 的行為,發生在真正的 memory 那一端。

---

## 1. Reuse 契約

`c_model/include/axi/` 是 Stage 2 完成的純 AXI subsystem,進入 NoC 階段後凍結,只引用不修改。分成兩種角色。

| 角色 | 檔案 | NoC 階段用途 |
|---|---|---|
| 地基（共用 AXI 知識） | `types.hpp`、`protocol_rules.hpp` | NMU/NSU 的 port 與 testbench 端點都從這裡長 |
| Testbench 端點與 oracle | `axi_master.hpp`、`axi_slave.hpp`、`memory.hpp`、`memory_port.hpp`、`scoreboard.hpp`、`beat_addr` | NMU 外側 traffic source、NSU 外側 DDR 端點、跨 fabric golden |

NMU 的 `axi_slave_port` 與 NSU 的 `axi_master_port` 是對等的。兩者都是透明 transport,做 handshake 加 channel 原樣轉發加 wlast/rlast framing,差別只在一個是 subordinate 介面一個是 manager 介面,valid/ready 方向相反。兩者都只 reuse 地基的 `types` 與 `protocol_rules`,自己驅動 channel,都不掛 `AxiSlave` 或 `AxiMaster` 的完整類別,因為那兩個會做 NMU/NSU 不該做的 decode 與 beat 生成。

完整 AXI master/slave 的意義在於 testbench 而非 NMU/NSU 內部。`axi_master.hpp` 當 NMU 外側的合法 traffic source,負責生 burst、4KB 切、narrow、WRAP、unaligned 去打 NMU。`axi_slave.hpp` 加 `memory.hpp` 當 NSU 外側會正確回應、會 DECERR、會 backpressure 的端點。它的完整度定義了 NMU/NSU 透明性要被驗證的 coverage space,要證明 NoC 無損搬過 WRAP / narrow / unaligned,端點就必須會產也會收這些。投資沒有浪費,只是住在 testbench 那一側。

---

## 2. Stage 2 收尾，純 AXI subsystem

Phase A 與 Phase B 已完工,涵蓋 sparse WSTRB、unaligned start、narrow transfer、WRAP / FIXED burst、4KB cross、per-ID FIFO、runtime protocol validation,test 全綠。

剩 Phase C 的 exclusive access,`AxLOCK` 加 `EXOKAY`,`AxiSlave` 加 per-ID 加 addr range 的 exclusive monitor,YAML schema 補 optional `lock`。

本階段產出凍結成第 1 節的地基與 testbench 端點,NoC 階段只引用不修改。

出場條件,ctest 全綠,`codegen.py --check` byte-identical,`gen_inventory.py --check` 不漂。

---

## 3. Stage 3，NMU 與 NSU 內部單元

全在 C,獨立可跑,NoC 側先接 responder stub(消化 req flit、回注 rsp flit,非字面 loopback)。路徑照 `FEATURE_INVENTORY`。資料正確性沿用 `scoreboard`,latency 靠 per-cycle tick。

兩個對外 port 都是薄的透明 transport。`nmu/axi_slave_port` 呈現 AXI subordinate 介面,收 AW/AR 進 queue、收 W 追 wlast,把帶完整屬性的 channel 交給 packetize,從 depacketize 收 B/R 後依 handshake 驅動回去。`nsu/axi_master_port` 是它的 peer,呈現 AXI manager 介面,把 depacketize 出來、已定形的 AW/AR/W 原樣驅動出去,不重切不重算,收外部 B/R 交給 packetize。兩者皆不需 `beat_addr`。

真正花心思的是其餘單元。`nmu/packetize` 把 AW/W/AR 連屬性原樣打進 flit payload,欄位位置吃 `ni::header`,reuse `types` 不重定 struct,這個檔很薄。`nmu/addr_trans` 把 awaddr/araddr 轉 dst_id 加 local_addr,先做 XYRouting。`nmu/rob` 是 NMU 在 AXI 這塊唯一真正 stateful 的責任,read 與 write 各一個獨立 mode,細節見 3.1。`nmu/vc_mapping` 在 NUM_VC=1 固定 VC=0,`nmu/vc_arb` round-robin 加 per-VC credit,`nmu/depacketize` 把 response flit 拆回 B/R。`nmu` 組裝,對 NoC 用 `NocReqOutPins` 加 `NocRspInPins`。

NSU 側,`nsu/depacketize` 把 request flit 拆回 AW/W/AR,`nsu/meta_buffer` 在收 AW/AR flit 時 snapshot rob_idx、src_id、awid/arid,回應注入時取回重建回程,`nsu/packetize` 把 B/R 組成 response flit,`nsu/vc_arb` 同 NMU,`nsu` 組裝對 NoC 用 `NocReqInPins` 加 `NocRspOutPins`。

NoC 邊界是 credit-based,`NocReqOutPins` 是 valid 加 flit 加 credit,packetize 出去的 flit 受 per-VC credit 節流,這跟 AXI 側的 valid/ready 不一樣。

### 3.1 Re-order buffer（read / write 各一獨立 mode）

支援 out-of-order,同 ID 可能亂序回。亂序的根因是同 AXI ID 的交易打到不同目的地,latency 不一致。reorder 策略每條 path 各一個獨立 mode 參數:read path（AR 加 R）一個,write path（AW 加 B）一個,各自 Enabled 或 Disabled,互不綁定。讀與寫本來就各算各的,因為 AR 與 AW 是兩條 channel,各有自己的 in-flight 集合。獨立設定讓你能組出例如讀開 reorder、寫用 issue 端擋這種省硬體的配置。

Enabled（該 path 的 rob_req=1）。放一個 reorder buffer。發送端為該 path 的 AX 配一個 rob_idx,交易自由多 outstanding,回程 response 進 buffer 依 per-AXI-ID 順序重排後依序釋出。讀寫兩個 buffer 概念對稱但實體大小差很多。read buffer 存 R data,entry 是 NOC_DATA_WIDTH 等級且多 beat burst 要收齊;write buffer 只存 B（id 加 resp）且 single-beat,本質是一張小表加依序放行指標,sizing 時勿照 read 規格開。實務上 read 用支援 burst 的 per-ID reorder buffer,write 用較簡的 FIFO 式 metadata buffer 即可。

Disabled（該 path 的 rob_req=0）。不放 buffer。發送端做 hazard guard,只擋同 AXI ID 不同目的地:一筆新交易看其 ID 是否有 in-flight 同 ID,無則放行並記下該 ID 當前 dst_id,有則比對 dst_id,相同則放行且 outstanding 計數加一,不同才 stall,等該 ID in-flight 全排空、計數歸零後換新 dst_id 放行。同 ID 同目的地維持多 outstanding。每個 ID 只需記當前 dst_id 加一個計數,不需記每筆,因此這個 mode 的硬體開銷只有一組 per-ID counter。這是不放 reorder buffer 時靠發送端序列化保 AXI ordering 的標準做法。

**Ordering 前提（須寫進 spec）**：mode Disabled 下「同 ID 同目的地多 outstanding 仍保序」成立的前提,是 NoC 對同一組 source 到 destination 的路徑為 deterministic,不因 adaptive 或 multi-path routing 讓同 ID 同目的地的交易走不同路而亂序。固定 XY routing 滿足此前提。若日後引入 adaptive 或 multi-path routing,此假設破裂,同目的地不再天然保序,這條規則須重新檢視。此 ordering 規則依賴 deterministic routing,改 routing 演算法時必須一併評估。

`rob` 在 request path 排在 `addr_trans` 之後,因為 Disabled 的 hazard guard 需要 dst_id。rob_idx 為 5 bit,深度上限 32 entry。read 與 write 採各自獨立的 idx 池,靠 axi_ch 區分,理由是讀深要存 data、寫淺只存 metadata,sizing 本質不同,綁成一池會互相牽制。若目標 outstanding 數超過池容量需加寬 flit 欄位（會回動 spec-as-code）。

rob_req 為 per-transaction 訊號,AR 帶的反映 read mode、AW 帶的反映 write mode,回程 B/R 帶 rob_req 加 rob_idx 回來驅動對應 buffer 的重排。flit 只有一組 rob_req 加 rob_idx 欄位,靠 channel 區分屬於 read 還是 write buffer。read 與 write 各一個 mode 參數,在 NMU 建構時各自設定。

出場條件,NMU 與 NSU 各自 unit test 加 block integration 全綠,不碰 SV。

---

## 4. Stage 4，平台組裝與 performance model

多個 NI 加一個 router model 串成最小 NoC。NMU 外側用 `axi/axi_master.hpp` 當 traffic source 打,NSU 外側用 `axi/axi_slave.hpp` 加 `memory.hpp` 當端點收。端到端 scoreboard 跨整個 fabric 比對資料與順序。

performance 輸出有天然落點。`ni_regs.h` 的 PKT_PROBE 那組（window size、byte count、bandwidth）與 TXN_PROBE 那組（latency threshold、bin count、min/max latency、total count）就是量測結果寫入處,cycle model 量到的 bandwidth 與 latency histogram 直接寫進既有 `register_file`,functional、performance、CSR 在此收斂。

async data boundary crossing 在 C model 階段只模行為,aclk 與 noc_clk 的 rate 差異用 rate 模型表達,metastability 不模,那是 RTL 與 CDC 驗證的事。

出場條件,飽和打 e2e 零錯,且 CSR 讀出的 counter 與預期 latency 對得上。到此 functional 加 performance C model 完整成立。

---

## 5. Stage 5，混合 co-sim

目標是讓 NMU、NSU、Router 各自獨立可以是 C++ model 或 RTL,平台不因換掉其中一個而重接。熱抽換粒度落在 component 層級。

### 5.1 為何可行

每個 component 的 port 都用 `specgen` 生成的 pin struct 表達,AXI 側是 `AxiSlavePortPins` 與 `AxiMasterPortPins`,NoC 側是 `NocReqOut/In` 與 `NocRspOut/In`。同一份 spec 同時生成 C 的 pin struct 與 SV 的 pin package,兩邊 byte-identical 且 golden-gated。因此不論一個 component 是 C++ 還是 RTL,它對外的 wire 契約完全相同,這是混搭的根本前提,也是 spec-as-code 投資在此階段的回報。

### 5.2 ComponentHandle 抽象

每個 component 用一個 handle 包起來,對平台呈現統一的 tick 介面,輸入輸出都是生成的 pin struct。handle 背後可以是三種 backend 之一。

| Backend | 機制 | 用途 |
|---|---|---|
| C++ model | 直接呼叫 Stage 3 的 `nmu` / `nsu` / `router` | 全速、可 gdb、unit test |
| Verilated RTL | RTL 經 Verilator 編成 C++,同 process tick | C++ harness 當 master,無需 DPI |
| DPI-RTL | pin 狀態經 DPI-C 過邊界到 HDL sim | VCS 當 master,延續 dram 範例的 HDL-master 模式 |

平台用 pin struct 一拍一拍把 component 接起來,某個 component 從 C++ 換成 RTL,改的是 handle 指向哪個 backend,不是重接平台。這是 Abstract Interface 加 Adapter pattern,套在 NI 與 Router 的粒度上,介面是生成的 pin。

### 5.3 混搭矩陣

任一子集是 RTL,其餘是 C++,平台佈線不變。

| 場景 | NMU | NSU | Router |
|---|---|---|---|
| 全 C model 回歸 | C++ | C++ | C++ |
| NMU RTL bring-up | RTL | C++ | C++ |
| Router RTL 驗證 | C++ | C++ | RTL |
| NSU RTL 對 DDR | C++ | RTL | C++ |
| 全 RTL signoff | RTL | RTL | RTL |

RTL component 用 C model 當 reference 比對,C model component 提供已驗證環境讓被測 RTL 不被其他未驗證 RTL 干擾。

### 5.4 兩條 sim 驅動

低摩擦先走 Verilator。c_model 本來就是 tick-driven C++,Verilator 把 RTL 也編成 C++,同一個 harness loop 一起 tick,這步連 DPI 都不必,ComponentHandle 的 Verilated backend 直接 in-process。

再做 VCS 的 DPI-C,延續 dram 範例的 HDL-master 模式,DPI bridge 把 pin 狀態接到 c_model 的 ComponentHandle。pin-level 才驗得到的 `*_VALID_STABLE` 這類 handshake 規則在此解鎖,因為 beat-level 的 c_model 量不到。

出場條件,任一混搭矩陣組合都能跑且 reference 比對零錯,全 RTL signoff 與全 C model 回歸結果一致。

---

## 6. File Tree

`[done]` 為現存,`[new]` 為待長。header 全引 `specgen/generated/cpp/` 的三組。

```
noc-sim/
├── specgen/                             [done] NI single source of truth + codegen（原 spec_validate）
│   ├── source/                          [done] 唯一手改處（原 authored/）
│   │   └── ni_function_blocks.json (+ .schema.json)
│   ├── ni_spec/                         [done] generator library（含原 tools/elaborate/）
│   ├── tools/                           [done] CLI: codegen.py、gen_inventory.py
│   ├── generated/                       [done] 全部產物，單一根，勿手改
│   │   ├── json/                        (原 generated/) elaborated 中間 spec
│   │   ├── cpp/{ni_flit_constants.h, ni_signals.h, ni_regs.h}        (原 include/)
│   │   └── sv/{ni_flit_pkg.sv, ni_signals_pkg.sv, ni_regs_pkg.sv}    (原 rtl_pkg/) golden-gated
│   ├── tests/  examples/  docs/         [done]
│
├── c_model/include/
│   ├── ni_spec.hpp  flit.hpp  register_file.hpp        [done] Layer A
│   ├── axi/                                            [done] Stage 2, 凍結
│   │   ├── types.hpp                                   [done] 地基: AwBeat/... (+ beat_addr 給端點用)
│   │   ├── protocol_rules.hpp                          [done] 地基: 22 helper + ASSERT
│   │   ├── axi_master.hpp                              [done] testbench: NMU 外側 traffic source
│   │   ├── axi_slave.hpp  memory.hpp  memory_port.hpp  [done] testbench: NSU 外側 DDR 端點
│   │   ├── (Stage 2C) exclusive monitor 擴在 axi_slave.hpp   [new]
│   │   └── scoreboard.hpp  scenario_parser.hpp         [done]
│   ├── nmu/                                            [new] Stage 3
│   │   ├── axi_slave_port.hpp                          [new] 透明 transport, 對外 AxiSlavePortPins
│   │   ├── packetize.hpp                               [new] 薄: channel 原樣打進 flit
│   │   ├── addr_trans.hpp                              [new] awaddr 轉 dst_id (XYRouting 先)
│   │   ├── rob.hpp                                     [new] read/write 各一獨立 mode: Enabled(reorder buffer) / Disabled(issue guard)
│   │   ├── vc_mapping.hpp  vc_arb.hpp  depacketize.hpp [new]
│   │   └── nmu.hpp                                     [new] 對 NoC: NocReqOut/NocRspIn
│   ├── nsu/                                            [new] Stage 3
│   │   ├── axi_master_port.hpp                         [new] 透明 transport, peer of axi_slave_port
│   │   ├── depacketize.hpp  meta_buffer.hpp            [new] meta_buffer: snapshot 回程 meta + atomic ID
│   │   ├── packetize.hpp  vc_arb.hpp                   [new]
│   │   └── nsu.hpp                                     [new] 對 NoC: NocReqIn/NocRspOut
│   ├── noc/router.hpp                                  [new] Stage 4
│   ├── platform/noc_platform.hpp                       [new] Stage 4, pin-struct 佈線
│   └── engine/sim_engine.hpp                           [new] cycle loop
│
├── c_model/tests/                                      [done] 既有 tests
│   ├── axi/...                                         [done]
│   └── nmu/  nsu/  integration/                        [new] 照既有 style 擴
│
├── rtl/ni/{nmu.sv, nsu.sv}                             [done] stub, Stage 5 長肉
│   └── rtl/noc/router.sv                               [new] Stage 5
│
└── cosim/                                              [new] Stage 5
    ├── handle/component_handle.hpp                     [new] C++ | Verilated | DPI 三 backend
    ├── verilator/                                      [new] Verilated backend + harness
    ├── dpi/{ni_dpi.sv, ni_dpi.cpp}                     [new] DPI bridge, extern "C"
    └── tb/                                             [new] VCS testbench, axi/ master/slave 當端點
```

---

## 7. Drift Gate，每個 commit 必過

```
cd specgen
py -3 -m pytest -q                    # spec 三 domain symbolic
py -3 tools/codegen.py --check        # byte-identical .h / .sv
py -3 tools/gen_inventory.py --check  # FEATURE_INVENTORY 不漂
cd ../c_model && cmake --build build && ctest --test-dir build
```

新單元的 expected 路徑已在 `FEATURE_INVENTORY` 中,照路徑長不會觸發 inventory drift。

---

## 8. 下一步最小動作

不要一次鋪整個 Stage 3。先做 `nmu/packetize.hpp` 一個檔加它的 GoogleTest。輸入帶完整屬性的 `AwBeat` 加幾個 `WBeat`,輸出對照 `ni::header` 的欄位位置檢查 flit 打對沒,NoC 側先不接,reuse `types` 不重定 struct。因為 port 與 packetize 都是透明的,這個檔會很薄,做完 NMU 整條 path 的 pattern 就立起來。接著做 `nmu/rob.hpp`,那才是 NMU AXI 語意的重點,read 與 write 各一獨立 mode 依 3.1,先把 Disabled 的 issue 端 hazard guard 做對,再加 Enabled 的 reorder buffer。
