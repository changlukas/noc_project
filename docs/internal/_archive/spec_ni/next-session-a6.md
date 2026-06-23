# NEXT_SESSION A6 — Handoff (cut 2026-05-15)

A5 wave (D1→D2 triage、implementer-review Round 1+2、plugin backflow、SLIDES.md user-curated) 已收尾。
本檔是 A6 session 重新進場用的 dispatch list：誰、做什麼、為什麼能開始、做完長什麼樣子。

A5 結束時 git head：`32d3f08`（spec(ni): D1->D2 triage — Round 2 cross-review closure...）。

---

## 0. Status snapshot

| 領域 | 狀態 | 備註 |
|---|---|---|
| MODE.md | locked | `protocol-bfm` / `has-rtl-counterpart: yes` |
| 6 BFM template files + registers + dv plan | content-locked | `/spec-lint` clean，134/134 wire parity，0 bare-TBD |
| Protocol rules | 136 條（126 FAIL + 10 RECOMMEND） | `protocol_rules.md` |
| Testpoints | TP1..TP51 但 TP45 缺號 → 實 50 條 | ⚠ 跟 SLIDES.md Slide 14 的「51」不一致；A6 開場決定 fix 編號還是 fix 數字 |
| ABV / SVA | 136 properties 1:1 映射 FAIL+RECOMMEND |  |
| `/spec-status` | D1 candidate 滿足；D2 ready-to-walk |  |
| `/spec-review` | Round 1 + Round 2 已執行；21 ambiguities 全部 triage 完 | 證據 in `IMPLEMENTER_REVIEW_LOG.md` |
| `IMPLEMENTER_REVIEW_LOG.md` | up-to-date through Round 2 |  |
| SLIDES.md | v0.4.0 A5 baseline 14-slide deck | user-curated；仍待優化（見 §2） |
| RTL skeleton | `nmu.sv` / `nsu.sv` empty shells 已 commit `517eb53` |  |
| packet_format.md | vendored 進 spec/ni/doc/ | 上游 `docs/design/packet_format.md` 同步 |

---

## 1. Deferred 任務（按 priority / blocker 排）

### T1. Bucket 4 — Architecture decisions（D2 phase 主軸）

A5 implementer-review 把 spec content ambiguity 清完；但有 4 個 architecture-level 決策在 A5 wave 故意 defer，因為超出 D1 scope。
這些不是 spec writing，而是 design decision，做完才能進 D2 sign-off。

#### T1.1 BFM 實作語言選定

- **Decision required**：BFM 用 SystemVerilog/UVM、純 C++/SystemC、還是 hybrid（C++ core + SV thin wrapper）？
- **Why deferred**：A5 wave 把焦點放在 spec 正確性；語言選定牽涉 build infra、CI、DV team workflow，是獨立 design decision。
- **Inputs needed**：
  - DV team 既有 UVM expertise 程度
  - 是否需要 portable 給 software team co-sim（C++ favorable）
  - performance budget（cycle-accurate vs transaction-accurate）
- **First step**：開新 doc `spec/ni/doc/bfm_language_decision.md`（D2 deliverable）；列三選項 + 三維度 tradeoff matrix（DV velocity / portability / cycle accuracy）。
- **Acceptance**：designer-confirmed pick；rule body 不變、但 §Theory of operation / §Active passive mode 補上 “implemented in `<language>`” 一行。
- **Effort**：~半個 wave；牽涉 cross-functional sync。

#### T1.2 NMU / NSU shell 演進路徑

- **Current state**：`rtl/ni/nmu.sv`、`rtl/ni/nsu.sv` 是 empty module shells（commit `517eb53`）。
- **Decision required**：A6 要開始實 sub-block 還是先把 sub-block partition / port list 文件化？
- **Why deferred**：A5 wave 內部 module 切割已在 `theory_of_operation.md §NMU sub-modules` / `§NSU sub-modules` 描述；但實作邊界（哪幾個 .sv 檔、各檔 port 怎麼接）未定。
- **First step**：對著 `doc/theory_of_operation.md` §NMU block diagram 把 10 個 sub-block 列出來，每個 sub-block 開一個 `rtl/ni/nmu/<name>.sv` shell，定義 input/output ports（不寫 logic）。
- **Acceptance**：`rtl/ni/nmu/` 跟 `rtl/ni/nsu/` 下面每個 sub-module 有 empty shell；top-level `.sv` 把 sub-module instantiate 起來；lint clean。
- **Effort**：~1 個 wave。
- **Risk**：sub-module port partition 一旦定下，後續實作 refactor 成本高。建議 spec-level review 一輪。

#### T1.3 DV testbench scaffolding

- **Decision required**：testbench 起點選哪個？
  - (a) 從既有 NI testbench 參考為 base 改
  - (b) cocotb + AXI VIP（Python-driven）
  - (c) self-roll UVM
- **Why deferred**：testbench 結構跟 T1.1 BFM language 是耦合的，要先定後者。
- **First step**：等 T1.1 結論；然後新增 `dv/testbench/README.md` 說明 build flow。
- **Acceptance**：一個 smoke test 跑得起來、回 0 errors、覆蓋 ≥1 個 testpoint（建議 TP1）。
- **Effort**：~2-3 weeks。
- **Blocked by**：T1.1。

#### T1.4 Coverage plan：per-rule testpoint mapping

- **Current state**：`dv/plan.md` 每個 TP 已標 `Protocol rules exercised` 欄，但未 reverse-index — 沒有「protocol rule X 被哪些 TP 覆蓋」這份表。
- **Decision required**：要不要做 forward-index 還是 reverse-index 還是兩個都做？
- **Why deferred**：126 FAIL × 50 TP 是 6300 cell 的 matrix，手工不可行；得寫 script。
- **First step**：寫 `dv/scripts/build_coverage_matrix.py`，吃 `protocol_rules.md` + `dv/plan.md`，產出 rule→testpoints 雙向 mapping。輸出 `dv/coverage_matrix.md`。
- **Acceptance**：matrix 顯示每條 FAIL rule 都有 ≥1 TP；orphan rule（無 TP 覆蓋）以高亮列出。
- **Effort**：腳本 ~半天；補 orphan TP（如果有）需另列 wave。
- **Risk**：可能發現 spec rule 寫了但 DV plan 沒覆蓋 → 屆時要決定 add TP 或降 severity。

---

### T2. D2 concrete deliverable — Hsiao H-matrix artifact

- **Why this matters**：A5 Round 2 補了 `NOC_FLIT_HDR_FLIT_ECC_GEN` rule body —「BFM 和 RTL MUST consume the same H-matrix」是 wire-level equivalence contract 的 load-bearing assertion。但 H-matrix 本身尚未產出。
- **Concrete action**：
  1. 使用 Hsiao SECDED generator（如 `secded_gen.py` 工具）；
  2. 算 `(FLIT_DATA_WIDTH=396, FLIT_ECC_WIDTH=10)` 的 Hsiao matrix（396 data bits、10 ECC bits、可糾正 1-bit、可偵測 2-bit）；
  3. 輸出三份 artifact：
     - `rtl/ni/common/secded_396_10.sv`（SystemVerilog 常數表）
     - `bfm/common/secded_396_10.{h,cpp}` 或 `.svh`（看 T1.1 結果）
     - `dv/scripts/secded_396_10_golden.py`（純 Python；DV scoreboard 用）
  4. 三份都 import 同一個 H-matrix；MD5 一致。
- **Acceptance**：三個檔的 H-matrix（10×396 binary matrix）每 bit 都一致；CI 加一個 hash check job。
- **Why this is small but blocking**：v0.4.0 wire-equivalence contract 寫了 “MUST consume the same H-matrix”，這 artifact 是該條 contract 的 ground truth。沒有就 D3 sign-off 不過。
- **Effort**：~半天～1 天（含 CI check）。
- **Blocker for**：D3 stage gate。
- **Note**：FLIT_DATA_WIDTH=396 跟 FLIT_ECC_WIDTH=10 兩個數字要先跟 `signal_interface.md` §Parameters / `packet_format.md` 對齊；目前 `packet_format.md` 寫 “FLIT_DATA_WIDTH default 256” 跟其它地方需 cross-check。**先做這個 verify 再下 secded_gen.py 的命令。**

---

### T3. Lint borderline cleanup

A5 wave `/spec-lint` 跑乾淨後仍有 2 條 borderline（非 blocking 但建議 D2 前處理）：

#### T3.1 LINT-006 — `0x110` placeholder reset `—`
- **Where**：`registers.md` register `0x110` 的 reset value 欄寫 `—`（em-dash），但這個 reg 不是 WO。
- **Why borderline**：LINT-006 規定 `—` 只用在 WO；目前該欄是 placeholder（A5 wave 故意留的）。
- **Fix options**：
  - (a) 補實際 reset value（hex）
  - (b) 改成 `TODO(designer): determine reset value (no issue yet — pending T1.x decision)`
  - (c) 真的 WO 就把 access 欄改成 WO 並保留 `—`
- **Effort**：~5 min；需 designer 確認語意。

#### T3.2 LINT-003 — Feature 2 implicit coverage
- **Where**：`README.md` Features bullet 2（具體哪一條已忘，A6 重跑 lint 確認）覆蓋是隱式的，沒 TP 顯式 keyword 匹配。
- **Fix options**：
  - (a) 補一個 TP 顯式 reference 該 keyword
  - (b) 改 Feature 措辭讓 keyword 對得上既有 TP
- **Effort**：~10 min。

#### T3.3 SLIDES Slide 14 「51 testpoints」校正
- **Issue**：實際 50 條 distinct testpoints（TP1..TP51 但 TP45 缺號）。
- **Fix options**：
  - (a) SLIDES 改成 50（最便宜）
  - (b) 補一個 TP45 把缺號填回去（如果有 testpoint 漏寫）
- **Effort**：~5 min。
- **Recommend**：先 grep history 看 TP45 是不是 wave 中曾被刪掉的；如果是、改 SLIDES = 50。

---

### T4. PRESENTATION_OUTLINE.md drift

- **Current state**：`spec/ni/PRESENTATION_OUTLINE.md` 仍寫 “12 slides”，但實際 SLIDES.md 是 14 slides；OUTLINE 是 SLIDES 重寫前的舊 plan。
- **Decision required**：A6 開場決定
  - (a) 把 OUTLINE 整份刪掉（SLIDES 已是 SoT）
  - (b) 把 OUTLINE 改成 SLIDES 的「reuse legend index」（保留 verbatim 引用對照表這層）
- **Recommend**：(a) — SLIDES 已有 §Appendix 列 Reference Sources，OUTLINE 沒附加價值。
- **Effort**：刪除 1 個 commit。

---

### T5. Hardware images cleanup

- **Current state**：`docs/images/NMU.png`、`docs/images/NSU.png` 是 untracked；SLIDES Slide 4 / 10 引用之。
- **Decision required**：要不要 commit 進 repo？
  - 若是 user 手繪 → commit
  - 若是外部文件截圖 → license issue，不要 commit；改成 mermaid 自繪
- **Effort**：depends on 來源。
- **Note**：`spec/ni/1.jpg / 2.jpg / 3.jpg`、`temp.pdf`、`table_dump.txt` 是 user local 工作檔，不入版。

---

## 2. SLIDES.md 待優化（A6 前段先處理）

詳細項目見 §2 規劃；摘要在這供 dispatch：

- T-SLIDES-1. 數字 audit（51 → 50；其餘 grep 重跑）
- T-SLIDES-2. verbatim quote attribution 補齊
- T-SLIDES-3. (B)-philosophy 措辭一致性掃描
- T-SLIDES-4. Slide 7/8 視覺資產（block diagram、protection-flow figure）
- T-SLIDES-5. Slide 1 roadmap 跟實際 deck 順序對齊

---

## 3. Plugin repo 端

A5 wave 已 push：
- `/spec-implementer-review` command + `implementer-reviewer` subagent
- `references/process/implementer_review.md`、`slide_style.md`
- `writing_principles.md §11-13`
- `stage_gates.md` 加入 `D1.cross.implementer_review`
- `plan/DOGFOOD_OBSERVATIONS_A5.md`

**未驗證項**（誠實標註）：`/spec-implementer-review` 從未被當作 slash command 實跑過，只有手動 prompt Round 1+2。A6 wave 第一個用 plugin 的 spec（如果有新 IP）建議實跑驗證一次。

---

## 4. 快速 re-entry checklist

A6 wave 進場前 5 分鐘做的事：

1. `cd E:/03_Learning/noc-sim && git log --oneline -5` — 確認上游沒人 push 衝突。
2. `cd spec/ni && cat MODE.md` — 確認 mode infrastructure 還在。
3. 跑 `/spec-status` — 確認 D1 candidate 還 hold。
4. 跑 `/spec-lint` — 確認沒回到 lint-dirty 狀態（CRLF / 編輯器自動格式化等 silent regression）。
5. 讀本檔 §1 挑下一個任務；不要 cross-cut multiple tasks。

---

## 5. 給未來 self 的提醒

- **不要重新觸發 implementer-review** — A5 wave 跑過 Round 1+2 並 21 個 ambiguities 全 triage；除非 A6 寫了大改動，否則跑這個是浪費 budget。
- **packet_format.md 在兩處**：`spec/ni/doc/packet_format.md`（spec-local vendored，SoT for this spec）、`docs/design/packet_format.md`（upstream，project-wide）。改 spec 內容只動 spec/ni/doc/；同步上游時兩邊都 grep 確認。
- **Writing-protocol 違反提醒**：A5 wave 的 working-protocol 違反主要是 numeric claim 沒 cite tool（51 vs 50 就是一例）；A6 強化 grep-verify-then-claim。
- **每個 commit 邊界**：T1.x 是 design decisions，每個獨立 commit；T2 (H-matrix) 是另一個獨立 commit（含 CI check）；T3 lint cleanups 可以一個 commit 包三項。
