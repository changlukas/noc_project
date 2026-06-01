# Next Steps — Stage 3 Bootstrap (NMU/NSU internal units)

**Current status (2026-06-01)**: Stage 2（純 AXI subsystem）完工，182/182 sequential ctest 全綠，specgen 三 domain 純 symbolic（pytest 159），drift gates clean。

**主 plan**: `docs/noc_cmodel_rtl_plan.md`（Stage 2/3/4/5 roadmap，本文件接續 §3 與 §8）。

---

## Stage 3 概念

Per 主 plan §3：NMU 與 NSU 內部單元全在 C++，獨立可跑，NoC 側先接 responder stub。資料正確性沿用 `c_model/include/axi/scoreboard.hpp`，latency 靠 per-cycle tick。

兩個對外 port (`nmu/axi_slave_port`、`nsu/axi_master_port`) 是**薄的透明 transport**：handshake + channel 原樣轉發 + wlast/rlast framing，**不**重算 beat addr、不重切 burst、不做 memory bounds 檢查。這些屬 memory endpoint，已在 Stage 2 `c_model/include/axi/{axi_master,axi_slave,memory}.hpp` 完成，凍結後 testbench 端 reuse。

對 NoC 是 credit-based pin struct (`NocReqOutPins` / `NocReqInPins` / `NocRspOutPins` / `NocRspInPins`)，與 AXI 的 valid/ready 不同。

---

## 下一步最小動作（per 主 plan §8）

**不要一次鋪整個 Stage 3。先做一個檔：`nmu/packetize.hpp` + GoogleTest。**

### Task: NMU packetize first cut

**Files**:
- 建: `c_model/include/nmu/packetize.hpp`
- 建: `c_model/tests/nmu/test_packetize.cpp`
- 建: `c_model/tests/nmu/CMakeLists.txt`
- 改: `c_model/tests/CMakeLists.txt`（加 `add_subdirectory(nmu)`）

**Goal**: 拿一個帶完整屬性的 `axi::AwBeat`（與選擇性的幾個 `axi::WBeat`），輸出一個 NoC request flit，欄位 byte 位置與 `ni::header`（來自 `specgen/generated/cpp/ni_flit_constants.h`、`ni_signals.h`）對得上。

**設計約束**（per 主 plan §3 + §1）：
- 薄。channel 原樣打進 flit payload，欄位位置吃 codegen 的 `ni::header`。
- **不**重定義 struct — 直接 `#include "axi/types.hpp"` 取用 `AwBeat` / `WBeat`。
- NoC 側先不接，packetize 只負責 encode 到 buffer / return flit by value。
- 沒有 cycle/timing 概念 — 純函數（或可重入物件）。

**測試**（GoogleTest）：
- 1 個 AW round-trip：建構 `AwBeat`（id / addr / len / size / burst / lock / cache / prot 全部填值）→ packetize → 從 flit raw bytes 按 `ni::header` 欄位 offset 把每個值讀回 → assert 與原值一一相等。
- 1 個 W round-trip：類似邏輯，包含 wlast。
- 1 個 unknown-field guard test：if `ni::header` 未來新增欄位，未被 packetize cover 的部分應為 0（或預期 sentinel）。

**Drift gates**（每個 commit 必過）：
```
cd specgen
py -3 -m pytest -q                         # 159 tests
py -3 tools/codegen.py --check             # byte-identical .h / .sv
py -3 tools/gen_inventory.py --check       # FEATURE_INVENTORY drift
cd ../c_model && cmake --build build && ctest --test-dir build -j 1   # 182 + new packetize tests
```

**Karpathy 4-lens** per task：
- Overcomplication：packetize 應該是薄函數，沒有 stateful class、沒有 builder pattern
- Surgical：只動 `nmu/`，AXI subsystem 完全不碰
- Surface assumptions：`ni::header` 欄位順序 / 位元位置必須在 test 裡 anchor，不能只靠"程式跑得過"
- Verifiable success：tests 必須 round-trip 比對欄位值，不能只測 compile

---

## 進入 Stage 3 前的 open questions（implementer 起手前先答）

1. **`ni::header` 結構**：codegen 產生的 `ni_flit_constants.h` 裡 `ni::header` 是 packed struct 還是欄位 offset 常數？packetize 應該 set struct 欄位、還是 memcpy 到 byte offsets？讀 `specgen/generated/cpp/ni_flit_constants.h` + `ni_signals.h` 確認後再決定 signature。

2. **AW 欄位完整 mapping**：`AwBeat` 的 `id / addr / len / size / burst / lock / cache / prot / region / user / qos` 都有對應 `ni::header` 欄位嗎？哪些欄位 NoC 不傳（不需 packetize）？對應到 spec 的 `spec/ni/doc/packet_format.md` 應該有答案。

3. **W beat 是否獨立打 flit**：plan §3 說「W 也原樣轉發、只多帶一個 last 標記」。是每個 W beat 一個 flit，還是 burst 級 framing？影響 packetize 的 API（單一 vs 累積）。

4. **Flit type 欄位**：AW / W / AR / B / R 五 channel 共用 `ni::header` 還是各自有 header type？需 codegen header 與 `packet_format.md` 對照。

5. **OSS-first survey**：FlooNoC 或其他 OSS NoC NI 的 packetize 怎麼做？Plan §0 提過 FlooNoC 的設計（整個 AW channel 原樣放進 flit payload）。survey 後再決定 c_model 是否需要 port 任何 OSS 邏輯，或自行實作。

---

## Stage 3 後續路線（packetize 完成後）

照 plan §3 順序，依序起 NMU 單元：
1. `nmu/packetize.hpp` ← **下一個 task**
2. `nmu/addr_trans.hpp` — awaddr / araddr → dst_id（先 XYRouting）
3. `nmu/rob.hpp` — read/write 各一獨立 mode (Disabled 先做 issue guard，Enabled 後做 reorder buffer)。主 plan §3.1 有完整設計。
4. `nmu/vc_mapping.hpp` / `nmu/vc_arb.hpp` / `nmu/depacketize.hpp`
5. `nmu/axi_slave_port.hpp`（薄 transport）
6. `nmu/nmu.hpp` 組裝

NSU 對等順序（per §3）：
1. `nsu/depacketize.hpp`
2. `nsu/meta_buffer.hpp`（回程 meta snapshot + atomic ID）
3. `nsu/packetize.hpp`
4. `nsu/vc_arb.hpp`
5. `nsu/axi_master_port.hpp`
6. `nsu/nsu.hpp` 組裝

NoC 邊界先用 responder stub，後續 Stage 4 才接 router。

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
