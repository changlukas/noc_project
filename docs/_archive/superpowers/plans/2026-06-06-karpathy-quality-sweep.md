# Karpathy Quality Sweep — Plan

> **For agentic workers:** REQUIRED SUB-SKILL — `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans`。動筆前先 invoke `andrej-karpathy-skills:karpathy-guidelines` skill 對齊 lens 定義。

**Goal:** 依 spec 跑 release-前 quality sweep — Karpathy 4-lens + magic-number 掃描。

**Spec:** `docs/superpowers/specs/2026-06-06-karpathy-quality-sweep-design.md`

**Tools:**
- Claude subagent dispatch：Agent tool (`subagent_type: general-purpose`)
- Codex review：`codex:rescue` skill (per memory [[feedback-codex-review-each-round]] — 不可 Agent tool 帶 `subagent_type: codex:codex-rescue`，那是 fire-and-forget)
- Karpathy 出處：`andrej-karpathy-skills:karpathy-guidelines` skill

**Regression gate commands:**

```bash
cd c_model/build && ctest --output-on-failure
cd specgen && py -3 -m pytest tests/
cd specgen && py -3 tools/codegen.py --check
```

---

## Task 1 — Dispatch parallel sweep

並行 dispatch (single message, multi-tool calls)：

- Claude subagent A：掃 `c_model/include/{axi,nmu,nsu,common,noc}/**` + 對應 `c_model/tests/`
- Claude subagent B：掃 `c_model/include/cosim/**` + `cosim/{c,sv,verilator,tests}/**` (排除 `cosim/sv/wb2axip/`)
- Claude subagent C：掃 `specgen/**`
- Codex via `codex:rescue` skill：cross-check 同樣 scope

每 subagent 回報 high-impact findings list，每筆格式：

- `file:line`
- axis：`karpathy:overcomplication` / `karpathy:surgical` / `karpathy:assumptions` / `karpathy:success` / `magic_number`
- 一行說明
- 一行 suggested fix

每 subagent cap 30 筆。不到 cap 就照實出。

## Task 2 — Aggregate + de-dupe

讀回所有 subagent + Codex output，同 `file:line + axis` merge 成單一 markdown
table 給 user。

## Task 3 — User triage

把 findings table 一次呈現給 user。每筆標 `fix` / `defer` / `ignore`。`defer` 與
`ignore` 各記一行原因。直接在 conversation reply 標，不用 JSON 檔。

## Task 4 — Apply fixes + regression gate

依 user 標記改 code：

- 同主題 commits batch 成一筆 (e.g. `refactor(c_model/axi): extract magic numbers`)
- 每 batch 跑 regression gate
- 全 green 才 push

Final gate：跑開頭定義的 3 條 regression gate command。全 green = sweep 結束。

---

## Notes

- 並行 dispatch 紀律參考 `superpowers:dispatching-parallel-agents`。
- 確認 W1+W2 已 merge / push 再啟動 (per spec §6 recommended order)。
