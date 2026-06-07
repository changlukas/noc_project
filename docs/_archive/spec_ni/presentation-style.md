# NI Presentation Style Guide

Style guide for `SLIDES.md`. 來源：A5 wave reference 圖 (`spec/ni/1.jpg`，NoC RoB slide 風格) + NoC IP datasheet 章節節奏 + IP datasheet 文字密度標準。

---

## 0. 使用原則 (highest priority)

**規則是工具、不是律法。看情況適配。**

本檔列出的每條規則都有「典型應用」與「不適用」兩種狀態。寫 slide 時：

- 先理解每條規則背後想解決的問題（density、reviewer skim 時間、視覺重心）。
- 再判斷當前 slide 的內容是否觸發那個問題。
- 不觸發就放行。例如：S5 三個 routing mode 是天然 mutually-exclusive、用 3-group bullet 比 table 順；但如果某天有 8 個 mode 要列，table 還是對的。

**反例 (anti-pattern)**：把規則當 lint rule 機械套用、全 deck auto-sweep。會壓死該保留特色的頁。

---

## 1. Title — 動詞起頭、一行

**典型應用**：
- ✅ "Resolving the AXI address into a NoC destination"
- ✅ "Managing the ordering requirement of AXI"
- ✅ "Bridging two clock domains at the NI boundary"

**反例**：
- ❌ "Address Decoding and Map"（noun phrase、沒講做什麼）
- ❌ "Read Re-Order Buffer (RoB)"（component name 而非 action）

**不適用**：
- Title slide (S1) — "AXI4-over-NoC Network Interface" 是 product name，無需動詞化。
- Overview / context-setting slides (S2 NoC Communication Overview, S3 NoC Components 等) — noun-phrase 能直接命名 deck section，動詞化反而失焦。
- Closing slide — 可以 noun phrase。

判斷分界：slide 的主軸是「介紹一個 component / 一塊範圍」→ noun phrase 可；slide 的主軸是「解一個問題 / 做一個取捨」→ 動詞化。

---

## 2. WHY callout — 黃框、一句 constraint

每頁 body 上方放一個 callout box，內容是這頁存在的**原因 / constraint / 設計目標**，不是 spec 結論。

**典型應用**：
- ✅ "Transactions with the same ID must be ordered in AXI" (S9 RoB 的 WHY)
- ✅ "AXI address bits encode the destination — the NI extracts (X, Y) before injection" (S5 Address)
- ✅ "AXI master and NoC fabric run at independent frequencies" (S4 NMU CDC 的 WHY)

**反例**：
- ❌ "Three routing modes trade area, flexibility, and latency"（是 takeaway 不是 WHY constraint）
- ❌ "Design difference: reference NoC pairs ..."（是 comparison 不是 WHY）

**不適用**：
- Title slide / Closing slide — 不需要 callout。
- Pure-overview slides (S2 NoC Communication, S3 NoC Components) — context-setting，不一定有 single constraint 可以 frame。可省略或改成「scope 一句」。

**callout 不是 Takeaway 的同義詞**。Takeaway 是「看完這頁你要記得什麼」。Callout 是「為什麼需要這頁」。兩者寫法很像但角度差 180 度。

---

## 3. Layout — 左視覺 / 右 bullets（建議、非強制）

**典型應用**：
- ✅ 左半 = block diagram / bit-layout / timing diagram；右半 = bullets。
- ✅ 視覺資產讓 reviewer skim 時眼睛先停在圖、再讀字。

**反例**：
- ❌ 左半 bullet + 右半 bullet（純文字頁、視覺重心丟失）。

**不適用**：
- 表格本身是 slide 的核心訊息（S6 Arteris IP traffic class 表）→ 表格佔滿橫向、bullets 放下方。
- 結尾 Closing slide → 多區塊 bullet 結構，無視覺資產。
- Section divider 頁 → 通常滿屏一個大字，無 layout。

---

## 4. Bullet 結構 — 3 個 group × 1-2 sub-bullets

每條 bullet ≤ 10 字、無嵌套、無斜體 emphasis。子 bullet ≤ 2 條/組。

**典型應用**：
- ✅ S9 範本：Reorder Table (2 sub) / Reorder Buffer (2 sub) / Optimized for deterministic routing (2 sub) = 3 group × 2 sub。
- ✅ 3 groups 對應 reviewer 短期記憶上限 (Miller's law、3-4 chunks)。

**反例**：
- ❌ 4 個 group × 3 sub-bullet × 嵌套（S4 NMU 當前狀態，14 items）。
- ❌ 一條 bullet 寫 25+ 字（reviewer 必須 re-read 才懂）。

**不適用**：
- Closing slide / 統計頁 — 可破 3-group 限制，但每組仍要短。
- 列舉密集型 slide（如 testpoint count、register map 摘要）— 可改用 table 形式取代 bullet group。

**Bullet 寫法**：
- 主 bullet：**Bold short noun phrase**（模式名、機制名、能力名）
- 子 bullet：完整短句、動詞起頭、≤ 10 字。
- 不要 colon 開頭的「Mode: description」格式（會跟 markdown table 衝突視覺）。

---

## 5. Source attribution — speaker notes，不放 body

Verbatim quotes、design reference citations、design difference 比較 — 統一放 **speaker notes**，不放 slide body。

**典型應用**：
- ✅ Body 顯示「Two-layer ECC: routing parity + whole-flit SECDED」；speaker notes 補 spec §Data Integrity verbatim *Uncorrectable ECC errors result in a fatal interrupt*。
- ✅ Body 顯示「3 routing modes」；speaker notes 補「reference design pairs Master-Specified ID + Re-mapping; we collapse to uniform 3-mode selector」。

**例外（仍放 body）**：
- ✅ Verbatim 本身就是 slide hook（S6 Arteris IP traffic class 表 — 表格內容引號是賣點）。
- ✅ Closing slide 列 deliverables 引用統計（不算 attribution）。

**反例**：
- ❌ Body 同時放 3 條 spec verbatim + 自己重寫的 3 layer 說明（S7 當前）→ 兩套重複內容打架。

---

## 6. Body 表格 — 預設不用、有 hook 才用

預設用 bullet group 表達 mutually-exclusive options（3-4 modes）。**只有當「並列比較三維度以上」是 slide 的賣點時才放表格**。

**典型「應用 table」**：
- ✅ S5 三 mode × (mechanism / use case / cost) — 三維度比較是 reviewer 的核心問題 → table 對。
- ✅ S6 Arteris IP traffic class × (profile / reason) — 並列借用 Arteris 表是賣點。

**典型「不該用 table」**：
- ❌ 三個 mode 但只列 description（沒並列維度）→ 改 bullet group。
- ❌ "Why two layers" 比較表 — 是 spec rationale 不是 slide hook → 移 speaker notes 或附錄。

---

## 7. 密度上限 — 200 字 + 一個視覺資產

**典型應用**：
- ✅ 每頁 body 文字總字數 ≤ 200 中文字 / ≤ 350 英文字 (rough budget)。
- ✅ 視覺資產：1 圖 OR 1 表 OR 1 callout box（不疊加）。

**反例**：
- ❌ S4 NMU 當前：14 bullet items + 2 callout 區塊 + spec verbatim ≈ 400 字 → 投影時字小到不可讀。

**不適用**：
- 結尾 Closing slide — 統計 + roadmap 自然破表，但結構應該明確分區。
- Section divider — 字很少，反向 outlier。

---

## 8. 跨頁節奏

**典型應用**：
- ✅ Overview slide（NMU/NSU）跟 deep-dive slide（routing modes / ECC layers / RoB modes）交替。
- ✅ 結束一個 component 後接 cross-cutting concern（CDC / QoS / Credit）。

**反例**：
- ❌ 連 3 頁深 dive 中間沒 catch breath（reviewer cognitive load 飽和）。

---

## 8.5 Engineering vocabulary

Slide 用字遵循 `CLAUDE.md §Spec writing style §Banned AI tells` — 同一份 banned list（leverages / ensures / facilitates / robust / scalable / "push complexity to the edges" 之類隱喻），對 slide body + Takeaway + WHY callout 同樣適用。

**典型替換**：
- ❌ "Push complexity to the edges" → ✅ "Packetization at the NI boundary only"
- ❌ "Routers stay simple and fast" → ✅ "Routers stay protocol-agnostic" / "Routers carry no AXI state"
- ❌ "Edge-heavy NoC split" → ✅ "NMU / NSU split"
- ❌ "Single unified fabric" → ✅ "Single shared link pair" (具體到 topology unit)
- ❌ "Header and data ride together" → ✅ "Each flit carries one AXI header plus its data payload"

判斷：每個 bullet / Takeaway 句讀一次，**問「這句話描述的是物件還是情緒」**。物件 = OK；情緒 / 形容詞堆 / 隱喻 = 重寫。

Speaker notes 因為是給簡報人口語使用，可放寬一檔（但仍要避開 banned AI tells）。

---

## 9. 引用本檔的時機

寫新 slide / 改 slide 前，掃一遍 §1-7 對照自己寫的版本。**如果你發現要破某條規則，先問自己**：
- 破這條是因為內容真的需要、還是偷懶？
- 破這條後 reviewer skim 時間是否仍 < 30 秒？
- 破完之後跟其它頁是否一致？（一致破 = OK；單獨破 = 不 OK）

回答得出來就放行。回答不出來就回去重寫。

---

## 10. 相關文件

- `SLIDES.md` — single source of truth for slide content。
- `PRESENTATION_OUTLINE.md` — 舊版 plan（A5 wave 前），已 stale；A6 wave 預定刪除或重寫。
- `spec/ni/1.jpg` — reference 圖 (RoB slide style)。
- NoC IP datasheet — chapter rhythm 參考來源。

Plugin 端 `slide_style.md` 是更廣的 BFM-mode slide guide；本檔是本 spec local 適配版。

---

## 11. Industry vocabulary table

Sourced from 2026-05-15 web research (ARM IHI 0022 / Arteris docs / Dally canon / NoC academic surveys). Provenance summary in §11.3. 這張表是給 slide / spec doc 用字校準用的，**不是 lint rule** — 看情境判斷。

| Category | 我們現用 | 業界 alt(s) | 決策 |
|---|---|---|---|
| Component | NMU / NSU | NIU (Arteris) · Network Adapter (academic) | Keep — first-use gloss。 |
| Component | NPS | NoC router · switch | Keep — "NoC Packet Switch"。 |
| Component | Single-NI per tile | (project-specific) | 用「one NMU+NSU per tile」。 |
| Component | AXI master / slave | AXI manager / subordinate (ARM IHI 0022 H.b+, 2021-01) | Hold — signal 名永遠 master/slave；prose 是 deck-wide 決策、目前 deck 全部保留 master/slave 不切。 |
| Flit | Wide flit | "Single-flit message on a wide physical channel" | 對外可加中性 gloss。 |
| Flit | Flit / phit / beat | universal | Keep。"Beat" = AXI-only（一個 data phase）。 |
| Routing | XY-routed | DOR (dimension-order routing) | "XY routing (DOR)" — first-use gloss DOR。 |
| Routing | Source-routed / ID-table | industry-standard | Keep。 |
| Routing | SAM | "System Address Map" (ARM CHI canonical) | Keep — first-use 拼出全名。 |
| Flow ctrl | Credit-based / wormhole / VC | universal | Keep。 |
| Flow ctrl | Wormhole-locked | (varies) | 改 "VC held for write-burst duration (wormhole)" — 複合詞非業界標準。 |
| QoS | QoS Generator | Arteris-coined、半通用 | Keep。 |
| QoS | Bandwidth-Limiter | Arteris: "Bandwidth Limiter" | Keep（去 hyphen 也 OK）。 |
| QoS | **Urgency-Regulator** | Arteris: "Bandwidth Regulator" / "Rate Regulator" — "Urgency Regulator" 不在 Arteris 公開文件 | **Rename → "Bandwidth Regulator (urgency-mode)"** 或 "Rate Regulator"。 |
| RoB | RoB | RROB (AMD) · reorder queue | Keep — first-use gloss "Reorder Buffer"。 |
| RoB | MetaBuffer | (project naming) | Keep；或 "transaction context buffer"。 |
| Width | Upsize / Downsize | "Upsizer / Downsizer" (ARM/AMD 標準) | Keep。 |
| ECC | SECDED | "SEC-DED" hyphenated · "Hsiao SEC-DED" | 首次提及用 "Hsiao SEC-DED (single-bit correct, double-bit detect)"。 |
| ECC | Per-hop routing parity | "Address parity" (NoC datasheet) | 用 "per-hop routing-header parity" — 跟 data parity 區隔。 |
| ECC | SLVERR / DECERR | universal AXI signals | Keep verbatim。 |
| ECC | **"(B)-philosophy"** | (無業界對應) | **Drop label** — 改具體描述。見 §11.1。 |
| Misc | Node ID / (X,Y) / destination ID | "Node ID" = canonical entity | "Node ID" 是 entity、"(X,Y)" 是 encoding、"destination ID field" 是 packet 欄位。 |
| Misc | Outstanding transactions | universal AXI | Keep。 |
| Misc | Exclusive Monitor | universal (ARM/Synopsys/Cadence 都用) | Keep。 |

### 11.1 "(B)-philosophy" replacement phrasings

Drop the internal label。用以下其一：

- **AMD-PG313 style（spec doc 推薦）**: "On uncorrectable end-to-end ECC at the NSU sink, the NSU forwards the response with the original `BRESP` from the target unchanged. No `SLVERR` is synthesized from the NoC fabric. Errors are logged in the Error Status registers."
- **Slide-bullet form（推薦）**: "End-to-end ECC errors at the NSU sink: log and forward. Do not rewrite `BRESP`. Synthesized `SLVERR` from the fabric is disallowed by design."

### 11.2 Low-confidence flags

- **"Wormhole-locked"** 複合詞 — 無 canonical 業界用法。
- **"Urgency Regulator"** — 內部 coinage、Arteris 公開文件查無此名。
- **"MetaBuffer"** — project-specific naming; can be glossed as "transaction context buffer".
- **AXI Master→Manager** 業界轉換尚未完成；signal 名仍 Master/Slave（waveform / 3rd-party IP）。

### 11.3 Sources

ARM IHI 0022 H.b+ · ARM AMBA 5 CHI · Arteris End-to-End QoS · Arteris FlexNoC · Neurealm NoC overview · Wikipedia (flit / ECC memory / wormhole switching) · Synopsys AMBA Exclusive blog · arXiv 2402.15749 routing survey · Cadence support note on AXI exclusive monitors.
