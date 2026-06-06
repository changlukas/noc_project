# Karpathy Quality Sweep — Spec

**Date:** 2026-06-06
**Status:** Design — pending user review
**Branch:** any (independent of W1+W2 refactor work)
**Related memory:** [[feedback-codex-review-each-round]]

---

## 1. Goal

Release 前對 codebase 做品質檢測：Karpathy 4-lens + magic-number 掃描。

User intent (verbatim, 2026-06-06):

> 「使用 karpathy-guidelines 對所有 code 進行品質檢測，以及找尋是否有 magic number
> 的存在，這部份用 claude subagent+codex 一起執行」

## 2. Scope

In：`c_model/` + `cosim2/` + `specgen/`

Out：`cosim2/sv/wb2axip/` (vendored Apache 2.0，frozen)

Deferred 到後續 release-quality follow-up spec：Verilator strict warning-clean、
coverage thresholds、sanitizer (UBSan/ASan)、fault injection、parameter sweep、
release tag。

## 3. Findings axes

| Axis | Check |
|---|---|
| Karpathy 4-lens | overcomplication / non-surgical / unstated assumptions / unverifiable success (per `andrej-karpathy-skills:karpathy-guidelines`) |
| Magic numbers | width / timing / mask literal 該命名而沒命名 |

Reviewer 紀律：只報「同檔 3+ 次、語意明顯、或有 comment 試圖解釋」的 high-impact
literal。一次性 literal 不報。

## 4. Process

Claude subagent + Codex (via `codex:rescue` skill) 並行 dispatch → aggregate +
de-dupe → user triage (`fix` / `defer` / `ignore`) → apply fix-marked → regression
gate。Codex review 走 skill 而不是 Agent tool 帶 `subagent_type: codex:codex-rescue`，
後者 fire-and-forget 不會回應。

## 5. Success

- Regression gate 全 green：`c_model` ctest + `specgen` pytest + drift gate
- Findings 都已 triaged，沒 unresolved entry

## 6. Recommended order

W1+W2 ship 之後再跑。W1+W2 已 delete / rewrite 部份 `cosim2/sv/` 檔；先跑這個會
浪費 fix-effort 在被刪檔上。
