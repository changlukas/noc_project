# Karpathy Quality Sweep — Design Spec

**Date:** 2026-06-06
**Status:** Design — pending user review
**Branch:** any (independent of W1+W2 refactor work)
**Sibling spec:** `2026-06-06-specgen-handshake-rtl-style-upstream-design.md` (W1+W2 atomic refactor)
**Related memory:** [[feedback-codex-review-each-round]]

---

## 1. Motivation & Scope

### 1.1 Motivation

User intent (verbatim, 2026-06-06 brainstorm):
> 「我還想規劃在這一輪使用 karpathy-guidelines 對所有code進行品質的檢測，以及找尋是否有magic number的存在，這個部份我需要claude subagent+codex一起執行」

The earlier attempt bundled this with a 12-gate release-level sweep (coverage / sanitizer / fault injection / parameter sweep). Codex round 5 found the technical gates were under-specified for the actual codebase (no `tb_top` parameter forwarding, no scenario-parser extension, IHI A9 matrix mis-classification, etc.). Round 6 deferred the entire bundle.

The Karpathy + magic-number sweep, however, has **no codebase-depth dependencies** — it is pure LLM dispatch + findings de-dup + user decisions + applied fixes. It can ship independently of W1+W2 and independently of the release-level gates.

### 1.2 In scope

- Karpathy 4-lens review (overcomplication / surgical / surface assumptions / verifiable success) over the repo, split into 3 zones.
- Magic-number hunt over the same zones.
- Two-reviewer protocol per zone: 1 Claude subagent (Agent tool, `subagent_type: general-purpose`) + 1 Codex review (via `codex:rescue` skill — NOT direct agent dispatch per [[feedback-codex-review-each-round]]).
- Findings de-dup + user decision matrix.
- Per-zone PR with architectural fixes; separate per-zone PR with magic-number fixes.

### 1.3 Out of scope

- Verilator strict warning-clean elaboration (deferred to release-sweep spec).
- Coverage thresholds + Verilator coverage parsing (deferred).
- Sanitizer (UBSan / ASan) runs (deferred).
- Fault injection (DPI-error + wb2axip protocol-violation) — deferred; existing `CheckerLiveness` ctest is NOT touched.
- Parameter sweep harness — deferred.
- `signal_interface.md` AXI matrix emission — deferred (see sibling spec §4.4).
- Release tag — this spec does not cut a release; it ships quality fixes only.

### 1.4 Success criteria

1. Each zone produces `zone_<X>_merged.json` (subagent + Codex de-duped, with overflow split into `zone_<X>_overflow.json` per §3.4).
2. Each zone produces `zone_<X>_decisions.json` annotated — every finding has a `decision` field (`fix` / `defer` / `ignore`).
3. Deferred entries split into the global `deferred_findings.json` with a non-empty `owner` field (not `"TBD"`); ignored entries split into `ignored_findings.json` with a rationale.
4. After Task 8 split, `zone_<X>_decisions.json` contains ONLY `fix`-marked entries (deferred + ignored removed).
5. All `fix`-marked findings applied via merged PRs; per-PR regression gate (c_model ctest + specgen pytest + drift gate) is GREEN.
6. Post-sweep `baseline.txt` shows the same three pass counts as the pre-sweep baseline.

---

## 2. Zones

| Zone | Files | Rationale |
|---|---|---|
| **A — Stage 3 c_model core** | `c_model/include/{axi,nmu,nsu,common,noc}/**` + `c_model/tests/{axi,nmu,nsu}/**` | Headers + tests for the c_model components that pre-date Stage 5b (NMU/NSU/AXI master/slave/scoreboard/Memory/etc.) |
| **B — Stage 5b cosim** | `c_model/include/cosim2/**` + `cosim2/{c,sv,verilator,tests}/**` | New code added during Stage 5b: shell adapters, DPI bridge, SV wraps, tests |
| **C — specgen** | `specgen/**` | Generator code + tests |

`cosim2/sv/wb2axip/**` is **excluded** from all zones (vendored Apache 2.0 code, frozen per Stage 5b rule).

### 2.1 Recommended ordering vs W1+W2

This sweep is **semantically coupled** to the W1+W2 atomic refactor (sibling spec), not independent of it:

- If this sweep ships **first**: it may flag findings inside `cosim2/sv/{axi,noc_req,noc_rsp}_intf.sv` and review pre-W2 specgen emitter shape. W2 then deletes the SV files and rewrites the emitters, wasting any fix-effort applied to them.
- If W1+W2 ships **first**: this sweep reviews the new `sv_params.py`/`cpp_params.py` peer emitters + the consolidated `axi4_intf`/`noc_req_intf`/`noc_rsp_intf` instead of the old shape. Findings are more durable.

**Recommended order: W1+W2 first, then this sweep.** This ordering is not enforced by the spec — user may override — but matches the realistic value calculus. Document the chosen order in `cosim2/quality/karpathy/baseline.txt`.

---

## 3. Two-axis findings per zone

### 3.1 Axis 1 — Karpathy 4-lens

Per `andrej-karpathy-skills:karpathy-guidelines`:

| Lens | Catches |
|---|---|
| Overcomplication | Unnecessary abstraction; wrapper over wrapper; framework when 1 function would do |
| Surgical | Changes that exceed stated scope; renames bundled with bug fixes |
| Surface assumptions | Implicit assumptions not stated in comments or asserts; "assumed reset is sync" without `// SYNC RESET` annotation |
| Verifiable success | Features / fixes without test coverage; "fixed in v2" with no regression test |

### 3.2 Axis 2 — Magic-number hunt

| Type | Action |
|---|---|
| Width literal in C++ (e.g., `std::array<int, 16> rob;`) | Replace with named `constexpr` |
| Width literal in SV (`logic [407:0] flit;`) | Replace with `parameter` / `localparam` |
| Timing constant (`wait_cycles(7)`) | Extract to named constant |
| Buffer size hardcoded multiple places | Single source-of-truth constant |
| Mask / shift literal | Named mask |

### 3.3 Scope rule (prevents balloon)

Reviewers report only literals that are HIGH IMPACT — referenced 3+ times in the same file, semantically meaningful, OR have a comment trying to explain them. Plain one-off literals are not reported.

### 3.4 Findings cap — per-reviewer + post-merge enforcement

| Layer | Cap | Action on overflow |
|---|---|---|
| Per reviewer (subagent / Codex) per axis | 30 | Reviewer self-truncates to highest-severity / highest-impact 30; instructed in the dispatch prompt |
| Post-merge per zone per axis | 60 (target) → trimmed by `merge_findings.py` to top 30/axis after dedup | Overflow auto-written to `cosim2/quality/karpathy/zone_<X>_overflow.json` with severity LOW; user can promote any back to a decisions file by hand |

The two layers together bound the per-zone user-decision phase to ~60 entries (30/axis × 2 axes) regardless of reviewer disagreement; the overflow file preserves anything that exceeds it for later promotion.

---

## 4. Dispatch protocol

### 4.1 Per-zone parallel dispatch

Each zone executes as a parallel pair:

```
Zone X:
  ├── Claude subagent  (Agent tool, subagent_type: general-purpose)
  └── Codex            (codex:rescue skill — NOT direct Agent dispatch)
```

Both reviewers get the same zone description + the same findings JSON schema. They run independently, in parallel (single tool message with both calls per [[feedback-superpowers-entry]] dispatch pattern).

### 4.2 Codex dispatch via `codex:rescue` skill (NOT direct Agent)

Per memory [[feedback-codex-review-each-round]] (Stage 5b lesson): Codex must be invoked via the `codex:rescue` skill, not via the `Agent` tool with `subagent_type: codex:codex-rescue`. The skill provides proper synchronous return; the direct Agent dispatch is fire-and-forget and the response never returns.

### 4.3 Findings JSON schema (every reviewer produces this)

```json
[
  {
    "severity": "CRITICAL | HIGH | MEDIUM | LOW",
    "category": "karpathy_overcomplication | karpathy_surgical | karpathy_surface_assumptions | karpathy_verifiable_success | magic_number_width | magic_number_timing | magic_number_buffer | magic_number_mask",
    "file": "<path relative to repo root>",
    "line": <int>,
    "description": "<one-line summary>",
    "suggested_fix": "<concrete code or patch hint; if magic_number_*, name the proposed constant>"
  },
  ...
]
```

### 4.4 Output locations

| Zone | Subagent output | Codex output | Merged |
|---|---|---|---|
| A | `cosim2/quality/karpathy/zone_A_subagent.json` | `cosim2/quality/karpathy/zone_A_codex.json` | `cosim2/quality/karpathy/zone_A_merged.json` |
| B | `cosim2/quality/karpathy/zone_B_subagent.json` | `cosim2/quality/karpathy/zone_B_codex.json` | `cosim2/quality/karpathy/zone_B_merged.json` |
| C | `cosim2/quality/karpathy/zone_C_subagent.json` | `cosim2/quality/karpathy/zone_C_codex.json` | `cosim2/quality/karpathy/zone_C_merged.json` |

Plus a global `cosim2/quality/karpathy/deferred_findings.json` for findings the scope rule excluded.

---

## 5. De-dup + merge

Findings de-dup by `(file, line, category)` tuple. On conflict (same key, both reviewers reported), the **higher-severity** entry wins. Both reviewers' descriptions are preserved in a `descriptions: [...]` array on the merged entry.

Implementation: `cosim2/scripts/karpathy/merge_findings.py` (~40 lines, see implementation plan).

---

## 6. User decision matrix

After merge, user reviews `zone_<X>_merged.json` and marks each finding's `decision` field:

| Decision | Meaning | Default |
|---|---|---|
| `fix` | Apply the `suggested_fix` in a PR | Default for CRITICAL + HIGH |
| `defer` | Move to `deferred_findings.json` with an `owner` field (Linear ticket ID or person name) | Default for MEDIUM + LOW |
| `ignore` | Drop with a rationale in an `ignored_findings.json` (kept for audit) | User-chosen only |

User editing happens in `zone_<X>_decisions.json` (copy of merged with `decision` + `owner` + `rationale` added per entry).

---

## 7. Applied-fix PR cadence

Per zone, **two PRs**:

1. **Architectural fixes PR** (`chore(quality-zone-<X>-arch)`): all `fix`-marked findings with `category` starting `karpathy_*`.
2. **Magic-number fixes PR** (`chore(quality-zone-<X>-magic)`): all `fix`-marked findings with `category` starting `magic_number_*`.

Why split: arch fixes touch logic; magic-number fixes touch constants. Splitting keeps review focused and rollback granular.

Per PR:
- Apply each fix according to its `suggested_fix`, adjusting for surrounding context.
- Run zone-relevant ctest suite. Failing tests block the PR.
- Commit incrementally — one finding per commit when feasible, multiple per commit when they touch the same file/symbol.

Total PR count: up to 6 (3 zones × 2 categories). PRs that have zero `fix`-marked findings of a category are skipped — no empty PRs.

---

## 8. Test plan

### 8.1 Pre-sweep baseline

- `cd /e/05_NoC/noc_project/c_model/build && ctest --output-on-failure` → record pass count (expected: 410/410).
- `cd specgen && py -3 -m pytest tests/ -v` → record pass count.
- `cd specgen && py -3 tools/codegen.py --check` → exit 0.

### 8.2 Per-PR gate

Each per-zone fix PR must reproduce:
- specgen pytest baseline pass count (no regression).
- c_model ctest baseline pass count (no regression).
- Drift gate clean.

No new gates are added — this sweep does not require coverage / sanitizer / strict warning-clean (those are deferred to release-sweep spec).

### 8.3 Final aggregate

After all per-zone PRs merged:
- All 3 zones have a `zone_<X>_decisions.json` with every finding marked.
- `deferred_findings.json` exists with `owner` populated for every entry.
- No `decision` field is empty in any merged file.
- Baseline pass counts hold across all merges.

---

## 9. Open items & risks

| # | Item | Mitigation |
|---|---|---|
| O1 | Findings volume may be larger than expected; user-decision phase could be the bottleneck | Two-layer cap (§3.4): per-reviewer 30/axis + post-merge trim to 30/axis. Overflow preserved in `zone_<X>_overflow.json` |
| O2 | Reviewers may disagree on severity for the same finding | Merge keeps higher severity; both descriptions preserved; merged entry's `description` reflects the higher-severity reviewer's wording |
| O3 | A fix may regress an existing test that wasn't covering the symbol robustly | Per-PR regression gate = full ctest + specgen pytest + drift gate (all 3 must pass). On regression: revert offending commit; move finding to `deferred_findings.json` with HIGH and failure log path |
| O4 | Sweep semantically coupled with W1+W2 (§2.1) — ordering affects which files get reviewed | Recommended ordering: W1+W2 first. Per-PR gate ensures whichever ships second sees green baseline. |
| O5 | wb2axip vendored code excluded; sweep may flag `cosim2/sv/wb2axip/**` references in non-vendored files (port wiring) | Scope rule: a finding *about how non-vendored code uses wb2axip* IS in scope; *inside wb2axip itself* is OUT. Reviewer prompt enforces |
| O6 | `c_model` has heavy templating; some "magic numbers" may be template parameters that look like literals | Reviewer prompt instructs: if a literal is template-parameter-bound, skip — it is not magic |
| O7 | Baseline / regression gate depends on a built `cosim2/verilator/obj_dir/Vtb_top` — two cosim ctests silently skip if missing (per `c_model/tests/CMakeLists.txt:40`) | Task 1 baseline + per-PR gates explicitly build Vtb_top before running ctest; baseline.txt records its existence as a pass condition |

---

## 10. References

- [[feedback-codex-review-each-round]] — Codex dispatch via `codex:rescue` skill (not direct Agent)
- [[feedback-superpowers-entry]] — parallel-dispatch pattern (single tool message with multiple calls)
- [[feedback-test-meaningfulness-over-count]] — magic-number sweep is per-impact, not per-count
- `andrej-karpathy-skills:karpathy-guidelines` skill — the 4-lens definitions
- Sibling refactor spec: `docs/superpowers/specs/2026-06-06-specgen-handshake-rtl-style-upstream-design.md`

---

## 11. Approval

- [ ] User review (pending)
- [ ] Spec self-review (this document)
- [ ] Codex review round 1 (planned after spec is committed)

After user approval, transition to `writing-plans` skill to produce `docs/superpowers/plans/2026-06-06-karpathy-quality-sweep.md`.
