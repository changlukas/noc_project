## Current status (2026-06-03)

Stage 3 ROB Enabled mode + multi-NSU testbench 完工：
- nmu/rob.hpp Enabled mode complete（per-beat slot pool, 32 slots from ROB_IDX_WIDTH=5, dynamic free-list allocator, per-id BeatRange commit sequence, in-order Path chain-flush, mixed-mode strict assert）
- ni/depacketizer.hpp 加 `pop_*_with_meta()` virtual + `ResponseMeta` struct（default impl 是 no-op forwarder, backward compat）
- nmu/depacketize.hpp override `pop_*_with_meta()`，抽 `rob_idx` / `rob_req` from flit header
- tests/common/loopback_noc.hpp 重寫成 multi-NSU testbench（4 NSU instances, per-NSU routing, per-NSU response latency static + random hybrid），backward-compat single-NSU ctor 保留 → 270 prior tests 零受影響
- integration testbench：`multi_dst_stress.yaml` 在 multi-NSU + per-NSU latency 下執行，positive `PerIdOrderTracker` 驗 Rob 不破壞 AXI4 §A5.3 同 id submission order（注意：Disabled mode 同樣保留 ordering，只是用 stall 而非 reorder — 本 fixture 是 positive ordering gate 而非 Disabled-vs-Enabled discriminator）
- 297 ctest sequential pass (276 prior + 21 new: 2 depacketize + 13 rob + 6 loopback), drift gates clean

**Next task per main plan §3.1**: `vc_arb` virtual channel arbitration（per-VC backpressure, round-robin or weighted scheduling, integrate with router fabric）。

後續 `vc_mapping` / `route_par` / `flit_ecc` / `nmu.hpp` top-level assembly 各自獨立 round。

---

## Stage 3 概念

Per 主 plan §3：NMU 與 NSU 內部單元全在 C++，獨立可跑，NoC 側先接 responder stub。資料正確性沿用 `c_model/include/axi/scoreboard.hpp`，latency 靠 per-cycle tick。

兩個對外 port (`nmu/axi_slave_port`、`nsu/axi_master_port`) 是**薄的透明 transport**：handshake + channel 原樣轉發 + wlast/rlast framing，**不**重算 beat addr、不重切 burst、不做 memory bounds 檢查。這些屬 memory endpoint，已在 Stage 2 `c_model/include/axi/{axi_master,axi_slave,memory}.hpp` 完成，凍結後 testbench 端 reuse。

對 NoC 是 credit-based pin struct (`NocReqOutPins` / `NocReqInPins` / `NocRspOutPins` / `NocRspInPins`)，與 AXI 的 valid/ready 不同。

---

## 下一步最小動作（per 主 plan §3）

主 plan §3 列出 NMU/NSU 兩個對外 port 是**最薄的 transparent transport**，與「真正花心思的」其餘單元 (packetize、addr_trans、rob...) 對比鮮明。port 對是 AXI 邊界的 contract anchor，應該**先做**。先把 port 對 anchor 起來，後面 packetize / rob / vc_arb 才有確定的上下游 channel 形狀可串。

### Task: 對等 port 對（NMU axi_slave_port + NSU axi_master_port）

**Files**:
- 建: `c_model/include/nmu/axi_slave_port.hpp`
- 建: `c_model/include/nsu/axi_master_port.hpp`
- 建: `c_model/tests/nmu/test_axi_slave_port.cpp`
- 建: `c_model/tests/nsu/test_axi_master_port.cpp`
- 建: `c_model/tests/nmu/CMakeLists.txt` + `c_model/tests/nsu/CMakeLists.txt`
- 改: `c_model/tests/CMakeLists.txt`（加 `add_subdirectory(nmu)` 與 `add_subdirectory(nsu)`）

**Goal**:
- `nmu/axi_slave_port`：對上游呈現 AXI **subordinate**（slave）介面，收 AW/AR 進 queue、收 W 追 wlast；channel 屬性原樣帶出去交給下游（packetize 介面 stub），從下游（depacketize stub）收 B/R 後依 AXI handshake 驅動回上游。
- `nsu/axi_master_port`：對下游呈現 AXI **manager**（master）介面，收上游（depacketize stub）來的已定形 AW/AR/W 原樣驅動出去（不重切不重算），收下游 B/R 交給上游（packetize stub）。
- 兩者都不需 `beat_addr`。
- 兩者都不重定 struct，reuse `c_model/include/axi/types.hpp` 的 `AwBeat / WBeat / ArBeat / BBeat / RBeat`。

**設計約束**（per 主 plan §1 + §3）：
- 透明 transport：handshake + channel 原樣搬運 + wlast/rlast framing。**不**做 per-beat 位址生成、bounds 檢查、DECERR 生成、burst 拆解。
- 與 packetize / depacketize 之間先用 stub interface（例如 queue<AwBeat> / queue<BBeat>），等 packetize 真做時對接。
- Tick-driven (per-cycle)，同 Stage 2 慣例。
- Backpressure：upstream / downstream 都用 ready 拒絕。

**測試**（GoogleTest）：
- **nmu/axi_slave_port**：
  - upstream 用 `axi/axi_master.hpp` 當合法 traffic source（既有 testbench，會送 burst / 4KB cross / narrow / WRAP / unaligned）。
  - downstream 用 stub queue 接出來的 channel。
  - 驗 5-channel handshake 不丟 beat、AW/AR 順序保留、W beats 跟 wlast 一致、B/R 從 stub queue 推進去後正確回到 master。
- **nsu/axi_master_port**：
  - upstream 用 stub queue 把已定形 AW/AR/W push 進去。
  - downstream 用 `axi/axi_slave.hpp` + `memory.hpp` 當端點。
  - 驗 channel handshake 不變形地驅動到 slave、收 B/R 回給 stub queue。
- **port 對 loopback**：兩個 port 直接 stub 對接（NoC = 零延遲 passthrough），用 `axi_master + axi_slave + memory + scoreboard` 跑既有 fixture（例如 `burst_incr_8beat.yaml`、`multi_outstanding_stress.yaml`），end-to-end scoreboard 零 mismatch。

**Drift gates**（每個 commit 必過）：
```
cd specgen
py -3 -m pytest -q                                                    # 159 tests
py -3 tools/codegen.py --check                                        # byte-identical .h / .sv
py -3 tools/gen_inventory.py --check                                  # FEATURE_INVENTORY drift
cd ../c_model && cmake --build build && ctest --test-dir build -j 1   # 182 + new port tests
```

**Karpathy 4-lens** per task：
- Overcomplication：port 應該是 thin handshake state machine，沒有 routing 邏輯、沒有 wlast 重算。
- Surgical：只動 `nmu/` 與 `nsu/`，AXI subsystem 完全不碰。
- Surface assumptions：「W 跟著 AW 的 issue order 走」、「per-id ordering 保留」這些假設要在 test 裡 anchor（即 testbench 故意打 multi-outstanding mixed-id 看 port 有沒有亂）。
- Verifiable success：loopback 端到端 scoreboard 零 mismatch，不是只測 compile。

---

## 進入 Stage 3 前的 open questions（implementer 起手前先答）

1. **port 與 packetize/depacketize 之間的 stub interface**：先用 `std::queue<AwBeat>` 之類最薄的 channel？還是定義 `IPacketizeSink / IDepacketizeSource` abstract base class 讓後續真品換上？影響 port API。

2. **Backpressure 模型**：upstream/downstream ready 訊號如何模擬？per-channel 獨立 backpressure 還是綁定？AXI4 spec 是 per-channel 獨立，c_model 該照辦。

3. **B/R 順序契約**：plan §3.1 說「per-AXI-ID 順序重排」是 ROB 的責任。port 本身**不**做重排，純 FIFO；但 port 收進去什麼就送出去什麼。loopback test 該對 same-id ordering 加 assertion 嗎？還是留給 ROB 階段才測？

4. **OSS-first survey**：cocotbext-axi 的 AxiSlave / AxiMaster 已經是 port-shaped 的 abstraction，handshake 邏輯已熟。值得 survey 一下 cocotbext-axi 的 port-side handshake 寫法做參考？plan §0 提的 FlooNoC 是 RTL，可能不如 cocotbext-axi 對等。

5. **Tick model 與 Stage 2 對齊**：Stage 2 的 `axi_master.tick()` / `axi_slave.tick()` 是 per-cycle 同步推進。port 對是否沿用同一個 tick 介面？loopback test harness 怎麼把幾個 tick-driven object 串起來。

---

## Stage 3 後續路線（port 對完成後）

照 plan §3 順序：

NMU side:
1. **`nmu/axi_slave_port.hpp` + `nsu/axi_master_port.hpp`** ← **下一個 task**（pair）
2. `nmu/packetize.hpp` — channel 原樣打進 flit payload，欄位吃 `ni::header`
3. `nmu/addr_trans.hpp` — awaddr / araddr → dst_id（先 XYRouting）
4. `nmu/rob.hpp` — read/write 各一獨立 mode (Disabled 先做 issue guard，Enabled 後做 reorder buffer)。主 plan §3.1 有完整設計。
5. `nmu/vc_mapping.hpp` / `nmu/vc_arb.hpp` / `nmu/depacketize.hpp`
6. `nmu/nmu.hpp` 組裝

NSU side（部分與 NMU 同步進行）：
1. `nsu/axi_master_port.hpp`（與 nmu/axi_slave_port 一起做）
2. `nsu/depacketize.hpp`
3. `nsu/meta_buffer.hpp`（回程 meta snapshot + atomic ID）
4. `nsu/packetize.hpp`
5. `nsu/vc_arb.hpp`
6. `nsu/nsu.hpp` 組裝

NoC 邊界先用 responder stub（直接 loopback / 零延遲），後續 Stage 4 才接 router。

---

## Process expectations（沿用 Stage 2）

- **Subagent-driven**：每 task 派 implementer subagent + spec reviewer + code quality reviewer
- **Karpathy 4-lens** review 每 task 完成後跑
- **OSS-first survey**：寫 source/test 前先看 OSS 對應實作
- **不重新設計 protocol**：AXI4 / NoC flit format / IHI 0022 標準行為 由 spec 決定，c_model 只實作
- **Don't bypass commit hooks**（no `--no-verify`）
- **Phase A+B+C 182 tests stay green** at every commit

---

## Inputs the next session should consult

- `docs/noc_cmodel_rtl_plan.md` — 主 plan，§3 / §3.1 / §8 為下一階段核心
- `specgen/generated/cpp/{ni_flit_constants.h, ni_signals.h}` — codegen header (packetize 對齊基準)
- `spec/ni/doc/packet_format.md` — packet format 文字描述
- `spec/ni/doc/theory_of_operation.md` — NMU/NSU 角色
- `c_model/include/axi/types.hpp` — `AwBeat` / `WBeat` 結構
- `c_model/include/axi/ATTRIBUTION.md` — OSS 引用記錄體例
- `c_model/NEXT_STEPS.md` — c_model 內部限制與已知 follow-up（Stage 2 audit 留下的）

GitHub Action 自動跑 specgen pytest + codegen + inventory drift gates。

---

## 開新 session 的最小 onboarding

開新 session 前先把舊 session 累積的 saved memory 搬過去（11 條 memory 含 OSS-first、不重新設計 protocol、bus semantics 等）：

```
# Windows path 編碼規則: 把絕對路徑的 : 與 / \ 都換成 -
# 例：clone 到 E:\06_NoC\noc_project\  → E--06-NoC-noc-project

cp -r "C:\Users\USER\.claude\projects\E--05-NoC-noc-sim\memory" \
      "C:\Users\USER\.claude\projects\<新 encoded path>\memory"
```

新 session 第一條訊息 (推薦)：

```
Read CLAUDE.md, NEXT_STEPS.md, docs/noc_cmodel_rtl_plan.md.

Then survey:
- ls c_model/include/{axi,nmu,nsu}/
- 看 specgen/generated/cpp/ni_flit_constants.h + ni_signals.h 的 ni::header 結構

Then propose answer to NEXT_STEPS open question #1 (port 與 packetize/depacketize
之間 stub interface 型態)，等我確認後再進 implementation.
```
