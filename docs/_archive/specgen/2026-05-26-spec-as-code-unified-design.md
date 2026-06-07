# NI Spec-as-Code 整合設計

> **Date**: 2026-05-26
> **Status**: Active — 後續實作以本文為準
>
> **Supersedes**（已刪除，commit cf22464）：
> - `spec_validate/whats-next.md`（前 session handoff，Phase 3-6 細節被本文取代）
> - `spec_validate/docs/plans/2026-05-25-ni-spec-modular-design.md`（Path B 轉向後已 obsolete）
> - `spec_validate/docs/plans/spec_as_code_plan.md`（motivation 已吸收進本文 §1-2，部分方向調整見 §3-4）

---

## 1. 為什麼做（Motivation）

NI 規格目前以 Markdown 散文寫成，給人讀。它有三個無法靠「再 review 一次」解決的問題：

**(a) 規格自身的矛盾藏得住。** Flit header 的 bit field 寬度加起來對不對、register 位址有沒有重疊、兩份文件對同一個參數寫了不同值 —— 算術與一致性錯誤人眼難掃，機器一秒能抓。散文格式根本沒有「驗證」這個動作，只有「review」。

**(b) C model 與 RTL 會漂走。** 流程是先寫 C model（golden reference）、再寫 RTL（DUT）。這是**同一份規格的兩個獨立人工 implementation**。只要一個常數、一個 bit offset、一個位址兩邊不一致，co-sim 就是在拿錯的 golden 比 —— debug 成本極高，而且那根本不是 logic bug。

**(c) C model 的正確性無處可比。** C model 是 golden，但本身是人寫的，沒有更上層的 golden 能比對它。傳統流程無解，只能靠 review 撐著。

**核心目的不是「多一份文件」，是把上面三個問題從「靠人」變「靠機器」。** 建立機器可讀規格作為單一事實來源（single source of truth），配 validator + codegen，C model 與 RTL 都從同一份規格生出常數，且規格本身可被機器驗證。

---

## 2. 核心願景

1. **Single source of truth → validator → codegen → C model + RTL 各自的 import 入口**
2. **C model 與 RTL 在資料層逐位元一致** —— 由 codegen 同源產出 C header 與 SV package 保證，不靠人對齊
3. **Validator-as-C-model 的「精神」延續，不是 code 延續** —— 跨語言（Python validator + C++ cycle-accurate C model）做不到「原地長成」，但架構原則延續：「規格資料」與「對規格做運算的邏輯」乾淨分離、Python `ni_spec.constants` ↔ C++ codegen 產 `.h` 跨語言保證、Python 模組目錄佈局在 C++ side 鏡像（詳見 §7）
4. **不過度工程** —— 明確不涵蓋 datapath / FSM / 演算法 / pipeline 行為，這些沒有對應的規格標準，不該硬塞進這套，維持一般 RTL/C 開發即可

---

## 3. 整合架構總覽

### 3.0 總體 dataflow（概念圖）

```
  ┌──────────────────────────────────────────────────────────────┐
  │  Source Layer (human-edited)                                  │
  │                                                               │
  │   spec/ni/doc/*.md                  spec_validate/            │
  │   ├ packet_format.md                └ ni_function_blocks.json │
  │   ├ signal_interface.md                 (手寫 JSON, 無 parser)│
  │   ├ pin_level_reset.md                                        │
  │   ├ registers.md                                              │
  │   └ protocol_rules.md  ─×─ (不入系統, 留純散文給人 review)     │
  └─────────────────────────┬────────────────────────────────────┘
                            │
                            │  generator (Python: ni_spec.generator)
                            │  MD → JSON;  function_blocks 直接 passthrough
                            ▼
  ┌──────────────────────────────────────────────────────────────┐
  │  generated/*.json   ←   Single Source of Truth (不准手改)      │
  │                                                               │
  │  ni_packet.json   ni_signals.json   ni_registers.json         │
  │                  ni_function_blocks.json                       │
  └─────────────────────────┬────────────────────────────────────┘
                            │
              ┌─────────────┼─────────────┐
              ▼             ▼             ▼
        ┌──────────┐  ┌──────────┐  ┌──────────────┐
        │Validator │  │ Codegen  │  │ni_spec.      │
        │L1 schema │  │codegen.py│  │constants API │
        │L2 算術   │  │          │  │ (Python)     │
        │L2 xref   │  └─────┬────┘  └──────┬───────┘
        └──────────┘        │              │
                            │              │
                    ┌───────┴───────┐      │
                    ▼               ▼      │
                include/*.h    rtl_pkg/*.sv│
                (C++ headers)  (SV pkg)    │
                    │               │      │
                    ▼               ▼      ▼
                C++ C model      SV RTL    Python validator
                (c_model/,                 (現在) +
                 未來)                      Python prototype
                                           (未來, defer)
```

三條消費路徑（C++ / SV / Python）共用同一份 generated JSON，常數一致由 codegen 對偶輸出保證。

### 3.1 Per-domain 路線表

| Domain | Source 路線 | Source 檔 | Generated JSON | Codegen target |
|---|---|---|---|---|
| **Packet format** | MD-as-source | `spec/ni/doc/packet_format.md` | `generated/ni_packet.json` | `include/ni_flit_constants.h` + `rtl_pkg/ni_flit_pkg.sv` |
| **Signal interface** | MD-as-source | `spec/ni/doc/signal_interface.md` + `pin_level_reset.md` | `generated/ni_signals.json`（含 `signals[].reset_behavior`） | `include/ni_signals.h` + `rtl_pkg/ni_signals_pkg.sv` |
| **Registers** | MD-as-source | `spec/ni/doc/registers.md` | `generated/ni_registers.json` | `include/ni_regs.h` + `rtl_pkg/ni_regs_pkg.sv` |
| **Function blocks** | **JSON 直接寫**（不過 MD parser） | `spec_validate/ni_function_blocks.json` | 自身為 source | `include/ni_blocks.h` + `rtl_pkg/ni_blocks_pkg.sv` |
| **Protocol rules** | 留純散文，**不進 spec_validate** | `spec/ni/doc/protocol_rules.md` | — | — |

### 3.2 Per-domain source 選擇的判斷依據

每個 domain 的 source 路線是個案決定，不是通則：

- **結構化規格事實**（每根 wire / 每個 bit / 每個 register 都有具體屬性、人類要讀 prose 來理解）→ MD-as-source + generator parse。例：packet / signals / registers
- **清單型 metadata 且主要消費者是機器**（entry 少、欄位固定、value 大量是 cross-ref name list）→ 直接 JSON。例：function_blocks 的 `uses_packet_fields[]` 本身是 list of name，包成 MD 表格只是多一層 parser
- **行為規約 prose**（rule condition 是 prose、難 DSL 化）→ 留 MD，metadata 部分（id / severity / channels）lift-shift 進 JSON 做 cross-ref check，condition prose 不機器化。例：protocol_rules（§5.5）

function_blocks 走 JSON 不是「<20 entries 都該走 JSON」的通則，而是這個 domain 的 cross-ref 結構正好讓 JSON 比 MD 自然。其他 domain 若 entry 數一樣少但內容多是 prose，仍應走 MD。

---

## 4. 不做的邊界

### 4.1 判準

**「兩個工程師對著 spec 各自寫一份 implementation，輸出能不能逐位元 / 逐欄位一致？」**
- 能 → 進 spec_validate 系統
- 不能 → 留給 C model + RTL 各自實作

| 在系統內（規格事實） | 不在系統內（實作決定） |
|---|---|
| Header field 在哪個 bit、payload channel width | ROB entry 用 array-of-struct 還 ring buffer |
| Signal 名 / direction / width / reset value | AW handshake state machine 怎麼跑 |
| Register offset / field bit / reset / access policy | Register write triggering 哪些內部信號 |
| Function block 清單 / mode enum / compile_time_params / 跨域引用 | ROB release entry algorithm |
| Invariants（bit tiling / 算術 / offset alignment） | 內部狀態 reset 完幾 cycle 穩定 |

### 4.2 明確不做的清單

- ❌ Protocol rule **condition mini-DSL** + SVA stub 自動生成 —— defer。但結構化 metadata 部分（id/severity/channels）會 lift-shift 進 JSON 做 cross-ref check（見 §5.5）
- ❌ Python prototype reference model —— defer，`behavior/` 目錄保留可能性
- ❌ SystemRDL / PeakRDL —— 已選不走（register 走 MD parser 與 packet/signals 一致）
- ❌ IP-XACT —— 已盤點不適用（IP-XACT 主要 ROI 在跨 EDA vendor 工具流，本專案 stack 不在這生態）
- ❌ Function block 內部 behavior 描述 —— 永不做
- ❌ Cycle-accurate timing constraint codegen —— 永不做
- ❌ PeakRDL-style 自動產 reg block RTL 實作 —— 不做；codegen 只產 offset/mask const，RTL designer 自己寫 reg block 但 `import ni_regs_pkg::*` 取所有常數，避免手抄

### 4.3 Deliberately not doing（這次評估後決定不做的，附理由）

以下幾條曾被認真評估、決定不做，**不是被忽略**。觸發條件成立時可重新評估。

| 不做的事 | 為什麼不做 | 觸發重評的條件 |
|---|---|---|
| **Symbol registry**（`generated/ni_symbols.json` 集中管所有 cross-language symbol，per-language uniqueness validator） | 目前所有 domain 加起來 symbol 數 < 200，肉眼掃描就能抓 collision。Generate + validate symbol registry 是 future tax | 出現第一次 C++ 跟 SV symbol 命名撞到，或 symbol 數超過 ~500 |
| **`_AXI_CHANNEL_SIGNALS` externalize 到 YAML/JSON** | AXI4 (ARM IHI 0022) 是 2010 公佈、長期穩定的 spec。把這個 mapping 從 `generator.py` 拉出來進 source tree，多一份 source 要維護，沒換來 deliverable | 專案要支援 AXI5 / ACE-Lite 等變體，或要對外 deliver 此 mapping 給其他 IP |
| **CI / pre-commit hook 強制化** | 目前單人專案，full CI infra 是 over-engineering。改用較輕的方式：`tools/codegen.py --check` mode 加 `.gitignore` 把 `include/` 與 `rtl_pkg/` 排除（防止手改 commit 進去） | 第二位 contributor 加入，或 PR-based workflow 啟動時升為 mandatory CI |
| **Function block 內 behavior 描述機器化**（例：ROB release algorithm） | 永不做。Datapath / FSM 留 C model + RTL 各自實作 | 不重評估 |

§4.2 是「永遠不做」或「defer」；§4.3 是「當下評估不值得做」，觸發條件成立時可回頭。

---

## 5. Per-domain 設計細節

### 5.0 Per-domain pipeline 對照（流程圖）

```
  Domain         Source                Generator       Generated JSON
  ─────────      ──────                ─────────       ──────────────
  Packet         packet_format.md  ──→ MD parser   ──→ ni_packet.json
                                       (PASS)

  Signals        signal_interface.md ┐
                                     ├→ MD parser  ──→ ni_signals.json
                 pin_level_reset.md  ┘  (merge,        (含 signals[].
                                        待補)           reset_behavior)

  Registers      registers.md      ──→ MD parser   ──→ ni_registers.json
                                       (Phase 3)

  FuncBlocks     ni_function_      ──→ passthrough ──→ (自身就是 SSoT)
                 blocks.json           (手寫 JSON,
                                        無 parser)

  ProtoRules     protocol_rules.md ──×    (不入系統)
                                       (留純散文)
```

每個 domain 進 §6 codegen pipeline，產 `.h` + `.sv`。

### 5.1 Packet format（已完成，僅補 SV codegen）

| 欄位 | 內容 |
|---|---|
| Source | `spec/ni/doc/packet_format.md` |
| Generator | 既有 `ni_spec.generator.write_generated_json` |
| Validator L1 | JSON Schema Draft 2020-12 |
| Validator L2 | bit tiling 不重疊、derived width = sum of field widths、payload channel ↔ header 一致 |
| C++ output | `include/ni_flit_constants.h` ✅ 已有 |
| SV output | `rtl_pkg/ni_flit_pkg.sv` ❌ 待補 |

### 5.2 Signal interface（含 pin_level_reset）

**前置問題**：目前 `ni_signals.json` 用抽象 channel-level 名（`AW_ID`、`AW_ADDR`），但 `pin_level_reset.md` 用具體 RTL pin 名（`axi_awvalid_i`、`axi_awaddr_i`）。兩者要 merge 必須先把「具體 pin 識別」帶進 signal model — 否則 reset_behavior 接不上去。

| 欄位 | 內容 |
|---|---|
| Source | `spec/ni/doc/signal_interface.md` + `spec/ni/doc/pin_level_reset.md`（兩份 MD，generator 一次讀） |
| Generator | 既有 `write_generated_signals_json` + **新增** `parse_pin_level_reset()`，依 `pin_name` cross-merge 把 `reset_behavior` 與 `presence` 合進 `interfaces[].channels[].signals[]`。並抽取 `pin_level_reset.md` 開頭的 reset signal 清單到 `meta.reset_signals[]`（給後續 validator 查表用）|

**Schema 升級（schema 重點欄位）**：

```jsonc
{
  "interfaces": [{
    "channels": [{
      "signals": [{
        "name": "AW_ID",                   // 既有：channel-level 抽象名
        "pin_name": "axi_awid_i",          // 新增：對應 RTL 具體 pin name (用來 cross-merge reset)
        "direction": "input",              // 既有
        "width_param": "IN_ID_WIDTH",      // 既有，型別擴充見下
        "reset_behavior": {                // 新增（從 pin_level_reset.md 合進來）
          "kind": "async-active-low" | "sync-active-high" | "external_driven",
          "value": "0" | "1" | "X" | "preserved",   // kind="external_driven" 時省略
          "domain": "arst_ni"              // 必須屬於 meta.reset_signals[]
        },
        "presence": {                      // 新增（conditional pin）
          "condition_text": "ENABLE_AXI_PARITY && EN_MST_PORT"   // 自由 prose，validator 不解析
        }
      }]
    }]
  }],
  "meta": {
    "reset_signals": ["arst_ni", "noc_rst_ni"]   // 新增，從 pin_level_reset.md §「Reset signals」bullet 抽
  }
}
```

**Width 表達式**：既有 `width_param` 只接受名字。新增允許小型表達式 whitelist：`<NAME>`、`<NAME>-1`、`<NAME>/8`、`<NAME>/8-1`。覆蓋 `noc_req_credit_o[NUM_VC-1:0]` 與 `axi_awaddr_par_i[ADDR_WIDTH/8-1:0]` 這類 case。其他表達式 generator 不接受、強制改寫成這四種之一。

**Validator L2 新增**：

- 每個 signal 必須有 `pin_name` 與 `reset_behavior`（input wire 的 `kind` 必須是 `external_driven`，不能寫 reset value）
- `reset_behavior.domain` 必須屬於 `meta.reset_signals[]`
- `pin_name` 在整個 spec 內唯一（一根 RTL pin 只能對應一個 channel/signal entry）
- `width_param` 表達式必須在 whitelist 內

**C++ output** `include/ni_signals.h`（port struct + reset initializer） ❌ 待補
**SV output** `rtl_pkg/ni_signals_pkg.sv`（SV interface + parameter + `*_RESET` localparam） ❌ 待補

### 5.3 Registers（Phase 3 全新）

**前置問題**：`registers.md` 的格式有幾個 parser 必須處理的 case，不能假設每 row 都是乾淨欄位：

- `registers.md:57` 有 reserved placeholder row：`| 0x110 | (reserved for LAST_ERR_INFO_HI) | — | — | ... |`，Access 與 Reset 是 em-dash
- 部分 width 是 expression（如 `ERR_COUNTER_WIDTH` bit、`ceil(log2(MAX_TXNS+1))`），不是 literal
- 部分 register 是 conditional present（`LAST_ERR_INFO_HI` 只在某些 param 組合下存在）
- ABI 行為（RW1C / WO read-as-zero / unmapped DECERR / reserved-bit policy）寫在 §Access policy 那段 prose，不是欄位 — 但是 ABI-visible，C model 必須抓

| 欄位 | 內容 |
|---|---|
| Source | `spec/ni/doc/registers.md` |
| Generator (新) | `parse_csr_policy()` / `parse_register_map()` / `parse_register_fields()` / `parse_err_irq_map()` → `generate_ni_registers_json(md_dir)` composer。**parser 必須跳過 access/reset 是 em-dash 的 reserved row，但保留 offset reservation 進 JSON 標記為 `kind: "reserved"`** |
| Validator L1 | JSON Schema |
| Validator L2 (新) | `check_csr_offset_alignment`（offset % 4 == 0）/ `check_csr_offset_unique` / `check_field_bit_tiling`（每 reg 內 field 不重疊、含 reserved bit 不重複占用）/ `check_reset_in_data_width`（reset < 2^width） |
| C++ output | `include/ni_regs.h`（offset macro + field bit mask + reset value + access mode enum） ❌ 待補 |
| SV output | `rtl_pkg/ni_regs_pkg.sv`（parameter / typedef / field tag） ❌ 待補 |

**Schema 升級重點欄位**：

```jsonc
{
  "csr_policy": {                                    // §Access policy section
    "sub_word_write": "decerr" | "ignored",
    "unmapped_read": "decerr" | "zero",
    "misaligned": "decerr" | "lower-aligned",
    "wo_read": "zero" | "decerr"
  },
  "registers": [{
    "offset": "0x094",
    "name": "TXN_MIN_LATENCY",
    "kind": "register" | "reserved",                 // 新：reserved placeholder row 用這標
    "access": "RO" | "RW" | "RW1C" | "WO" | "WC",    // ABI semantics
    "reset_expr": "0xFFFF" | "0x0",                  // literal
    "width_expr": "16" | "ERR_COUNTER_WIDTH",        // 既有 width 為 literal；新允許 param name
    "conditional_presence": null | {                  // 新：only-exists-when
      "condition_text": "AXI_ID_WIDTH >= 16"
    },
    "reserved_bits_policy": "read-zero-write-ignored",
    "fields": [...]                                  // 既有
  }]
}
```

**不做的事**：不產 reg block 的 RTL 實作。Codegen 只產 offset/mask const + access enum，RTL designer 自己寫 reg block 但 `import ni_regs_pkg::*` 取所有 offset/mask。

**Phase 3 acceptance criterion**：regen `ni_registers.json` 後與 `deferred/ni_registers.json`（legacy 手寫版）做結構 diff，差異要能逐條解釋（新增欄位 / 改名 / fix bug），不能是 silent regression。完成後可刪 `deferred/`。

### 5.4 Function blocks（手寫 JSON）

| 欄位 | 內容 |
|---|---|
| Source | `spec_validate/ni_function_blocks.json`（**手寫**，位置在 `spec_validate/` 而非 `spec/`，因為這不是「規格散文」是「規格 metadata」） |
| Generator | **不需要** —— source 已是 JSON |
| Schema | `block.{name, fullname, role}` + `features[].{id, name, summary, modes[], compile_time_params{}, uses_packet_fields[], configured_by[], related_features[], source_doc}` |
| Validator L1 | JSON Schema (`ni_function_blocks.schema.json`) |
| Validator L2 (新) | id 唯一 / `compile_time_params` name 跨 feature 唯一 / **跨 domain cross-ref**：`uses_packet_fields[]` → `ni_packet.json` 真實存在 / `configured_by[]` → `ni_registers.json` 真實存在 / `related_features[]` 雙向引用 |
| C++ output | `include/ni_blocks.h` —— `enum class FunctionBlock { ROB, QOS, ECC, ADDR_MAP, ... }` + 每 block 的 mode enum `enum class ROBMode { Normal, Simple, NoRoB }` + `compile_time_params` 的 `constexpr int` ❌ 待補 |
| SV output | `rtl_pkg/ni_blocks_pkg.sv`（同上 mapping） ❌ 待補 |

**Cross-ref 的精神**：function_blocks **不重複定義** packet field 的 bit range / register field 的 offset —— 它只引用 name。實際 bit range 從 `ni_packet.json` 查、實際 offset 從 `ni_registers.json` 查。Validator 只 check name 存在，codegen 出來的 C++/SV ROB code 用 `ni::header::ROB_IDX_LSB` 取常數，跨層自動同步。

**手寫 JSON 不走 MD parser 的原因**：feature list 結構固定、entry 少（< 20 個），MD parser 比手寫 JSON 多一層 fragile parsing；且 `uses_packet_fields[]` / `configured_by[]` 這些 cross-ref 欄位本就是寫給機器看的，不需要先包成 MD 表格再 parse 回 JSON。

**手寫 JSON 必要的 discipline**（防止 JSON 變成無紀律的 dump）：

| 規則 | Schema 約束 / Validator check |
|---|---|
| `summary` 長度上限 | ≤ 200 字符（hard error）。長 prose 寫到 `source_doc` 引用的別處去，function_blocks 內只放精簡描述 |
| `modes[]` item 命名 | regex `^[A-Z][A-Za-z0-9_]*$`（codegen 直接當 C++ enum class member name 與 SV typedef enum member name 用，必須合法 identifier） |
| `id` 命名 | regex `^FEAT-(NMU\|NSU)-[A-Z][A-Z0-9_]*$` |
| `compile_time_params` 值來源 | **這份 JSON 是 design parameter 的權威 source**。值不能跟其他 domain JSON 衝突。如果 `ROB_DEPTH` 在 `packet_format.md` 也有定義，spec 要先消滅一邊（推薦把它從 packet_format 移到 function_blocks）|
| `uses_packet_fields[]` 引用粒度 | 只能引到 header field name 或 payload channel name（如 `"rob_idx"` 而非 `"header.rob_idx"`），由 validator 解析來源 |

### 5.5 Protocol rules（narrow lift-shift）

原本完全 defer；re-review 後改為「結構化 metadata 抓進 JSON、condition prose 留 MD」的窄版本。cross-ref check（id 唯一、channel token 屬於 ni_signals.bundles）1 天工，能擋住 spec 維護期常見的 typo bug。

| 欄位 | 內容 |
|---|---|
| Source | `spec/ni/doc/protocol_rules.md`（保持 MD 為 source、condition prose 仍在那裡給人讀）|
| Generator (新) | `parse_protocol_rule_index()` 從 MD 表頭抽結構化欄位，**不抓 condition 的 prose 內容** |
| Validated to | `generated/ni_protocol_rule_index.json` |
| Schema | `rules[].{id, proto: "AXI4" \| "AXI4LITE" \| "NOC" \| "NI", role: "master" \| "slave", channels[], severity, source_section, source_line}` 加 `condition_summary`（≤ 100 字摘要，不是完整 condition） |
| Validator L2 (新) | `id` 唯一 / `channels[]` 內每個 token 必須屬於 `ni_signals.json` 真實存在的 channel name / `source_section` 必須能在 `protocol_rules.md` 找到 |
| Codegen | **不產 .h / .sv**（暫不做 SVA stub）。產 documentation index：`include/ni_protocol_rules_index.md` 給人查表 |

**仍然 defer 的部分**：
- ❌ Condition mini-DSL（structured predicate）
- ❌ SVA stub 自動生成
- ❌ C model runtime assertion 自動生成

這些等真要做 formal verification 或 C model 細到要逐 rule check 時再評估。

---

## 6. Codegen target 結構

### 6.1 目錄佈局（新增 + 重整）

```
spec_validate/
├── include/                        ← C++ headers (codegen output)
│   ├── ni_flit_constants.h          ← packet (已有)
│   ├── ni_signals.h                 ← signal port struct + reset init (新)
│   ├── ni_regs.h                    ← register offset/mask/reset (新)
│   └── ni_blocks.h                  ← FunctionBlock + mode enum + compile_time_params (新)
├── rtl_pkg/                        ← SV packages (codegen output, 新目錄)
│   ├── ni_flit_pkg.sv
│   ├── ni_signals_pkg.sv
│   ├── ni_regs_pkg.sv
│   └── ni_blocks_pkg.sv
└── tools/
    └── codegen.py                   ← 統一入口
```

`tools/codegen.py --target {cpp|sv} --domain {packet|signals|registers|blocks} --out <path>`，一個 CLI 控四種 domain × 兩種語言共 8 種輸出組合。內部 dispatcher，per-domain emitter 各自 module，共享 spec loading / formatter。emitter 邏輯都吃 `ni_spec.constants`（資料/邏輯分離）。

`gen_cpp_header.py` 是原始 wrapper，已在 commit cf22464 刪除。

### 6.2 C++ namespace vs SV package 命名 + 型別對齊（強制）

| 概念 | C++ | SV |
|---|---|---|
| 整數常數（bit position / width / offset / mask） | `constexpr int` 例：`ni::header::ROB_IDX_LSB` | `localparam int unsigned` 例：`ni_flit_pkg::ROB_IDX_LSB` |
| compile-time design param | `constexpr int ni::blocks::ROB_DEPTH` | `parameter int unsigned ni_blocks_pkg::ROB_DEPTH` |
| signal reset value | `constexpr ni::signals::AW_VALID_RESET` | `localparam` 同名 |
| mode enum（每 block 一個強型別 enum） | `enum class ROBMode { Normal, Simple, NoRoB }` | `typedef enum logic [clog2-1:0] { ROB_MODE_NORMAL, ROB_MODE_SIMPLE, ROB_MODE_NOROB } rob_mode_e;` |
| function block 列表 enum | `enum class FunctionBlock { ROB, QOS, ECC, ... }` | `typedef enum logic [clog2-1:0] { FUNCTION_BLOCK_ROB, FUNCTION_BLOCK_QOS, ... } function_block_e;` |

**為什麼 SV 一定要 `typedef enum` 不能用 bare parameter**：bare `parameter ROB_MODE_NORMAL = 0` 在 SV 型別系統是 32-bit signed int，`logic [1:0] mode` 接時可能 silent truncate。`typedef enum logic [N-1:0] { ... } rob_mode_e;` 才與 C++ `enum class ROBMode` 型別對等。位寬 `N = $clog2(count)`，codegen 算出來填。

**為什麼整數常數要 `int unsigned`**：bit width、register offset 是非負量。SV `parameter` 默認 signed 32-bit，作為 vector 寬度用時若跟 signed 互動會有 implicit conversion bug。codegen 一律 emit `int unsigned`。

SV 沒 namespace 分層，用 `<pkg>::<NAME>`，name 內用 prefix 編 sub-namespace（如 `FUNCTION_BLOCK_ROB`）。Codegen 強制兩邊一致命名規則 + 型別匹配，差別是 C++ 多一層 `::` 分隔、SV flat + 顯式型別。

### 6.3 Codegen ↔ validator 耦合

```
ni_spec.constants  ← 所有「從 JSON 撈值」邏輯都在這層
       ▲                ▲
       │                │
   codegen.py        validator (invariants.py)
       │
   .h / .sv 文字
```

- Codegen **不直接讀 JSON** —— 一律 `from ni_spec import constants`，constants.* 是穩定 API
- 加新 codegen 常數 → **先擴 `constants.py`，再用 codegen 呼叫新 API**
- validator 共享同一個 constants API
- 這套是「Python validator → C++ C model」演進的 stable interface

### 6.4 Static_assert 內嵌進 .h（限算術子集，不是 drift protection 萬靈丹）

C++ codegen 把 **L2 算術 equality** invariants 內嵌進 `.h`，C++ 編譯時驗：

```cpp
// ni_flit_constants.h (codegen 自動產，目前還沒做)
constexpr int FLIT_WIDTH    = 406;
constexpr int HEADER_WIDTH  = 54;
constexpr int PAYLOAD_WIDTH = 352;
static_assert(HEADER_WIDTH + PAYLOAD_WIDTH == FLIT_WIDTH,
              "Flit width arithmetic inconsistent — re-run spec validator");
```

**範圍限定**：以下 L2 check 會 emit 成 `static_assert`：

- `FLIT_WIDTH = HEADER_WIDTH + PAYLOAD_WIDTH` 與 `derived.*` 系列 6 條算術 equality
- SECDED bound: `2^parity_bits >= data_bits + parity_bits + 1`
- 各 register field width 加總 ≤ data width

以下 L2 check **不能** 變 `static_assert`，仍只在 Python validator 跑：

- bit tiling（含 overlap 檢測，需迭代 + 比較，C++ `constexpr` 寫得出來但代價高）
- name-existence cross-ref（symbol 是否存在於另一個 spec，跨 .h 邊界，編譯期看不到）
- width_param 表達式 eval（grammar 解析）

static_assert 只擋算術錯誤，不擋「人手改了 .h 沒重 gen」這種 drift。後者靠 codegen `--check` mode（§6.7 / §4.3）。

### 6.6 Codegen output header（每個 .h / .sv 開頭必要欄位）

每個 codegen 產的檔案開頭強制有以下 metadata，debug 漂走時知道是哪份 spec 哪個 commit 生的：

```
// ----------------------------------------------------------------------------
// AUTO-GENERATED — do not edit
// Generator: tools/codegen.py @ v0.1.0
// Source:    spec_validate/generated/ni_packet.json
// JSON SHA:  3f8a2b1c5d... (truncate to 12 hex)
// Spec ver:  v0.4.0       (from spec/ni/VERSION)
// Generated: 2026-05-26T14:23:01Z
// ----------------------------------------------------------------------------
```

SV 同樣 4 行用 `//` 開頭。

`tools/codegen.py --check` 比對「重新算的 JSON SHA == .h 內 header 寫的 SHA」，不一致就 fail。

### 6.7 Schema version contract + spec_version SSoT

**`$schema_version`**（每個 generated JSON 帶）：

- 跟著 generator 同步演進，bump 規則：schema 加新 required 欄位 / 改現有欄位語意 → major bump（`2.0` → `3.0`）；新增 optional 欄位 → minor bump；純 doc / typo → patch
- Codegen 在 .h header 印 `$schema_version`，consumer 若 unsupported version 就 error
- 目前 `ni_packet.json` / `ni_signals.json` 都是 `"ni-spec/2.0"`，新 domain 進來 keep `2.0` 直到 schema break

**`spec_version`**（規格內容版本）：

- 單一來源：`spec/ni/VERSION`（一行 semver string，如 `v0.4.1`）
- 所有 generator 啟動時讀這檔，寫進每個 generated JSON 的 `meta.spec_version`
- 各個 MD source 不重複寫版本字串 — 避免 typo drift

**`tools/codegen.py --check` mode**：

- regen 到 `/tmp` 暫存
- 比對 `/tmp/include/*.h` vs `spec_validate/include/*.h`（diff or SHA 比對）
- 不一致就 exit non-zero
- 給人/CI 跑都行

### 6.5 三邊消費端

```
        spec/ni/doc/*.md  +  spec_validate/ni_function_blocks.json
                    │
                    │ generator (Python)
                    ▼
              generated/*.json    ← single source of truth
                    │
        ┌───────────┼────────────┐
        ▼           ▼            ▼
   include/*.h  rtl_pkg/*.sv  ni_spec.constants
        │           │            │
        ▼           ▼            ▼
    C++ C model  SV RTL      Python validator
```

三邊都 trace 回同一份 source JSON。

---

## 7. Validator-as-C-model 演進路徑

### 7.0 跨語言一致性（概念圖）

```
                  Single Source of Truth
              spec_validate/generated/*.json
                          │
        ┌─────────────────┼─────────────────┐
        │                 │                 │
        │ codegen.py      │ ni_spec.        │ codegen.py
        │ --target cpp    │ constants       │ --target sv
        ▼                 ▼                 ▼
   include/*.h       ni_spec/*.py       rtl_pkg/*.sv
   ┌─────────┐       ┌──────────┐       ┌─────────┐
   │constexpr│       │def header│       │parameter│
   │static_  │       │_field_pos│       │ /enum / │
   │assert   │       │(...)     │       │ struct  │
   └────┬────┘       └────┬─────┘       └────┬────┘
        │ #include        │ from ... import   │ import pkg::*
        ▼                 ▼                   ▼
   ┌─────────┐       ┌──────────┐       ┌─────────┐
   │ C++ C   │       │ Python   │       │ SV RTL  │
   │ model   │       │ validator│       │ (DUT)   │
   │(c_model/│       │ + future │       │         │
   │ 未來)   │       │ prototype│       │         │
   └─────────┘       └──────────┘       └─────────┘

  跨語言一致性保證:
   ─ 同名 (modulo C++/SV namespace syntax: ni::header::X  vs  ni_flit_pkg::X)
   ─ 同值 (codegen 對偶輸出)
   ─ Static_assert 內嵌 .h → C++ compile-time 自動驗 invariants
     (不必跑 Python, 編譯就能擋住 spec drift)
```

「Validator 是 C model 前身」這條精神由「常數 API」+「invariants 同步」+「目錄結構鏡像」三件事承載，不靠 code 原地長成。

### 7.1 現況：Python `ni_spec/` 的「資料/邏輯分離」已就位

```
spec_validate/ni_spec/
├── loader.py        ← I/O 邊界：讀 JSON → dict
├── generator.py     ← 邏輯：MD → JSON parsing
├── invariants.py    ← 邏輯：spec 不變量檢查 (L1 + L2)
├── constants.py     ← 邏輯：「從 spec 撈常數」穩定 API ← ★ firewall
├── report.py        ← 邏輯：報告格式
└── __main__.py      ← CLI
```

- **資料** = `generated/*.json`
- **邏輯** = 上述六個 .py
- **`constants.py` 是 firewall**：任何「想知道 ROB_IDX bit range」的消費端 —— Python invariants / codegen / 未來 Python prototype —— **全部呼叫 `constants.header_field_position(spec, "rob_idx")`，不直接讀 JSON**

JSON 結構未來改了，constants API 不變 → 消費端不受影響。

### 7.2 C++ C model 鏡像同原則

```
noc-sim/
├── spec_validate/                       ← Python: spec 自洽 + codegen
│   ├── ni_spec/*.py
│   ├── generated/*.json                  ← single source of truth
│   ├── include/*.h                       ← codegen 對 C++ 的 stable API
│   └── rtl_pkg/*.sv                      ← codegen 對 SV 的 stable API
│
└── c_model/                              ← C++: 真實 cycle-accurate behavior (未來)
    ├── include/
    │   ├── ni_spec.hpp                    ← #include 所有 codegen .h，C++ side stable umbrella
    │   ├── flit.hpp                       ← Flit class (pack/unpack)
    │   ├── rob.hpp / qos.hpp / ecc.hpp    ← per-feature class
    │   └── ...
    └── src/
        └── ...
```

C++ side 對應原則：
- 資料 = `spec_validate/include/*.h`（codegen 產，永遠不手抄）
- 邏輯 = `c_model/src/*.cpp`
- `ni_spec.hpp` = C++ side stable API umbrella，類似 Python 的 `ni_spec.constants`

### 7.3 跨語言一致性的三個保證點

| Layer | Python side | C++ side | 一致性怎麼保證 |
|---|---|---|---|
| **常數** | `ni_spec.constants` | codegen 產的 `*.h` 內 `constexpr` | codegen 對偶輸出 —— 同名同值 |
| **不變量** | `invariants.check_*()` runtime | codegen 內嵌 `static_assert(...)` 在 .h | compile time 直接 fail，不必跑 Python |
| **behavior** | (defer Python prototype) | `c_model/src/*.cpp` | 跑同一份 stimulus、co-sim 比對 |

### 7.4 「精神延續」的真正貢獻

跨語言（Python → C++）的現實：

- ✘ 不是「省一次 rewrite」—— behavior 從 Python 翻 C++ 仍要 rewrite
- ✓ 而是「rewrite 時不會迷失方向」—— 架構原則 + 常數 API + invariants 都已 stable，rewrite 只是把行為翻 C++，不必重新討論「資料怎麼存 / 常數從哪來 / 怎麼驗一致」

### 7.5 Python prototype reference model 的位置（defer）

`c_model/behavior/`（or `ni_spec/behavior/`）目錄留架構空位，等真開始寫 C model 時再決定。

---

## 8. Scope delta

### 8.1 已完成 ✅

1. `packet_format.md` → `ni_packet.json`（含 schema、L1/L2 invariants）
2. `signal_interface.md` → `ni_signals.json`（top-level 7 個 interface、`channels[].signals[]`）
3. C++ codegen for packet：`include/ni_flit_constants.h` + `examples/use_constants.cpp`（編譯 + 執行 PASS）
4. Python `ni_spec/` 模組（loader / generator / invariants / constants / report / __main__）

### 8.2 待做 ❌（按邏輯依賴排序，8 項）

> 原 7 項清單藏了隱含依賴（`constants.py` API extension 沒當 gate、signal pin-name redesign 沒擺在 reset merge 前），重排成 8 項。

1. **Foundation gates**（~0.5 day）
   - 寫 §6.7 schema-version contract 文件
   - `spec/ni/VERSION` 建立、generator 都讀這檔
   - `.gitignore` 加入 `include/`、`rtl_pkg/`（防手改 commit）
   - 設計 `tools/codegen.py --check` mode 的 spec（不實作，等 #6 一起做）
   - `ni_spec.constants` 加 signals / registers / blocks 三個 domain 的 API skeleton（空函式 + docstring，等 #3-#5 填）

2. **Signal model redesign**（~1 day）
   - schema 加 `pin_name`、`kind: "external_driven"`、`presence`、width expression grammar、`meta.reset_signals[]`
   - generator 改寫 — 從 `signal_interface.md` parse 出抽象 signal entry 後，cross-merge `pin_level_reset.md` 的 `pin_name`
   - validator L2：pin_name 唯一、external_driven 不能有 reset value、reset domain 必須在 whitelist
   - 既有 `ni_signals.json` 對拍：新 schema 跑出來內容應該是 superset，舊欄位不能 silent drop

3. **Pin-level reset merge**（~0.5 day，依賴 #2）
   - 既然 #2 已經把 pin_name 帶進來，這步是把 reset_behavior 實際填上
   - 60% rows 是 input wire → `kind: "external_driven"`，自動處理
   - acceptance: ni_signals.json 內每 signal 都有 reset_behavior

4. **Register domain end-to-end**（~2 day）
   - parser 處理 em-dash placeholder row（reserved registers）、width expression、conditional_presence
   - schema 加 `access` / `reserved_policy` / `width_expr` / `conditional_presence`
   - validator L2: offset align/unique、field tiling、reset bound
   - 與 `deferred/ni_registers.json` 結構 diff，差異要能逐條解釋

5. **Function blocks**（~1 day）
   - 手寫 `ni_function_blocks.json`（從 README §Features 轉錄）
   - schema 含 §5.4 列的所有 discipline（length cap、mode regex、compile_time_params 規則）
   - cross-ref validator：uses_packet_fields → packet、configured_by → registers
   - **acceptance**：把 `compile_time_params` 與其他 domain 重複定義的部分消除（推薦做法：所有 design parameter 都移到 function_blocks，packet_format 只剩 wire-level width）

6. **Protocol rule metadata lift-shift**（~1 day）
   - 新 parser `parse_protocol_rule_index()` 抓表頭結構化欄位
   - schema + L1/L2（id 唯一、channel token 屬於 signals.bundles）
   - 暫不做 SVA / mini-DSL

7. **Unified codegen.py + C++ emitters**（~2 day）
   - dispatcher：`--target {cpp|sv} --domain {packet|signals|registers|blocks} [--check]`
   - C++ emitters 4 個 domain + provenance header（§6.6）
   - regen 既有 `ni_flit_constants.h`，與 committed 版做 byte-level diff (regression check)
   - `gen_cpp_header.py` 已刪除（cf22464）

8. **SV emitters + 簡單 lint smoke test**（~2 day，依賴 #7）
   - 4 個 SV emitter（`ni_{flit,signals,regs,blocks}_pkg.sv`）
   - `typedef enum logic [N-1:0] { ... }` + `localparam int unsigned`
   - smoke test：`verilator --lint-only rtl_pkg/*.sv` 若 verilator 裝了就跑（沒裝就 skip 帶 warning）
   - 同時加 §6.4 的 static_assert 算術子集進 C++ emitter（同 phase，因為都要重訪 emit 路徑）

**Total ~10 工程日。** 具體順序、test plan、each step 的 acceptance criteria 留給 writing-plans 階段。

**不在 §8.2 內的事**：full CI infrastructure（見 §4.3 — 等第二位 contributor 加入再升級）、symbol registry（不做）、`_AXI_CHANNEL_SIGNALS` externalize（不做）。

---

## 9. 下一步

本 design doc 是 brainstorming 階段產物。下一步：

1. **writing-plans skill 接手** —— 將 §8.2 的 8 項待做轉成 implementation plan，含每項的：依賴、預估工程量、test criteria、檔案級 deliverable

2. **舊文件處理** —— ✅ 已完成（commit cf22464）。

### 9.1 Supersede checklist（已完成）

三份舊文件已在 commit cf22464 刪除。刪除前逐項確認 disposition：

| 舊文件 | Unique 內容 | Disposition |
|---|---|---|
| `whats-next.md` | Phase 1-2 已完成的具體 commit history、ENABLE_AXI_PARITY 處理過程、`_section_slice` bug 修法、escaped pipe bug 修法 | implementation 細節，已落在 code，不必搬進 design。**Disposition: 仍適用，本文未變動** |
| `whats-next.md` §critical_context | venv 路徑、PowerShell `$env:PATH` 設定、MSYS2 g++ 注意事項 | 已隨機器搬遷過時（路徑變更）。**Disposition: 過時，丟棄** |
| `whats-next.md` §memory files | 跨 session 的 `feedback_*` memory 條目 | 在新機器 `C:\Users\USER\.claude\projects\E--05-NoC-noc-sim\memory\` 重建。**Disposition: 不在 design doc 範圍** |
| `2026-05-25-ni-spec-modular-design.md` | 文件頂部已自宣告 Path B 轉向後 obsolete；三層校驗的 L3 cross-doc check 概念 | Path B 之後 L3 不必，已併入 L2。**Disposition: rejected (obsolete)** |
| `spec_as_code_plan.md` §1 三個藏不住的問題 | 規格矛盾 / C-RTL 漂走 / C model 雞生蛋的論述 | **Disposition: migrated 進 §1** |
| `spec_as_code_plan.md` §3 範圍表中「CSR 走 SystemRDL + PeakRDL」 | spec_as_code 原本主張 register 走業界標準 | 已評估後選擇不走（理由：consistency with packet/signals MD parser）。**Disposition: rejected with reason，記錄在 §4.2** |
| `spec_as_code_plan.md` §6 「validator 是 C model 前身（同一連續譜系）」框架 | 認為 validator 會原地長成 C model | 跨語言後不成立。**Disposition: 框架 retired，新版見 §7.4** |
| `spec_as_code_plan.md` §7 真正划算的三類（flit+SV/C / CSR PeakRDL / Validator 分層） | 三類中第 2 類 (PeakRDL) 已 rejected | **Disposition: 部分 migrated（第 1 與第 3 類），第 2 類 rejected** |

以下三份文件已刪除（commit cf22464）：
- `spec_validate/whats-next.md` ✅
- `spec_validate/docs/plans/2026-05-25-ni-spec-modular-design.md` ✅
- `spec_validate/docs/plans/spec_as_code_plan.md` ✅
