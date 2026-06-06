# Karpathy Quality Sweep Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run a Karpathy 4-lens + magic-number sweep over the repo in 3 zones with parallel Claude subagent + Codex reviewers per zone, merge findings, take user decisions, apply fix-marked findings as per-zone PRs.

**Architecture:** Pure orchestration plan. No new c_model/SV code. Tasks: pre-sweep baseline → 3 parallel zone dispatches → merge + user decision matrix → per-zone fix application (arch PR + magic-number PR per zone, skipping empty PRs).

**Tech Stack:** Agent tool (general-purpose subagent), `codex:rescue` skill, Python 3 (merge script), pytest + GoogleTest (regression gate).

**Source spec:** `docs/superpowers/specs/2026-06-06-karpathy-quality-sweep-design.md`.

**Codebase facts (verified):**
- c_model test runner: `cd c_model/build && ctest --output-on-failure`
- specgen test runner: `cd specgen && py -3 -m pytest tests/`
- Drift gate: `cd specgen && py -3 tools/codegen.py --check`
- Codex dispatch: invoke `codex:rescue` skill (not direct Agent call) — per [[feedback-codex-review-each-round]] memory

---

## File Structure

### Created

- `cosim2/scripts/karpathy/dispatch_subagent.md` — dispatch prompt template (reused per zone)
- `cosim2/scripts/karpathy/dispatch_codex.md` — same content as subagent prompt; Codex variant
- `cosim2/scripts/karpathy/merge_findings.py` — de-dup + merge JSON
- `cosim2/quality/karpathy/zone_{A,B,C}_subagent.json` — raw subagent findings
- `cosim2/quality/karpathy/zone_{A,B,C}_codex.json` — raw Codex findings
- `cosim2/quality/karpathy/zone_{A,B,C}_merged.json` — de-duped merge
- `cosim2/quality/karpathy/zone_{A,B,C}_decisions.json` — user-annotated decisions
- `cosim2/quality/karpathy/deferred_findings.json` — global deferred list
- `cosim2/quality/karpathy/ignored_findings.json` — global ignore list with rationales
- `cosim2/quality/karpathy/baseline.txt` — captured pre-sweep test counts

### Modified

- Per-zone, per-PR: code files identified by `fix`-marked findings.

---

### Task 1: Capture pre-sweep baseline

**Files:**
- Create: `cosim2/quality/karpathy/baseline.txt`

- [ ] **Step 1: Run baselines**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd /e/05_NoC/noc_project
mkdir -p cosim2/quality/karpathy

{
    echo "=== Karpathy sweep pre-baseline ==="
    echo "Date: $(date -Iseconds)"
    echo "Commit: $(git rev-parse HEAD)"
    echo
    echo "--- c_model ctest ---"
    (cd c_model/build && ctest --output-on-failure 2>&1) | tail -5
    echo
    echo "--- specgen pytest ---"
    (cd specgen && py -3 -m pytest tests/ -q 2>&1) | tail -5
    echo
    echo "--- drift gate ---"
    (cd specgen && py -3 tools/codegen.py --check && echo "drift: OK") || echo "drift: FAIL"
} | tee cosim2/quality/karpathy/baseline.txt
```

Expected: c_model ctest reports 410 tests passed; specgen pytest reports all pass; drift: OK.

- [ ] **Step 2: Commit baseline**

```bash
git add cosim2/quality/karpathy/baseline.txt
git commit -m "chore(quality): capture karpathy sweep pre-baseline"
```

---

### Task 2: Author dispatch prompt template

**Files:**
- Create: `cosim2/scripts/karpathy/dispatch_subagent.md`

- [ ] **Step 1: Write the template**

```markdown
<!-- cosim2/scripts/karpathy/dispatch_subagent.md -->
You are running a Karpathy 4-lens + magic-number sweep on a specified file zone of the noc_project repo. Return findings ONLY as a JSON array — no prose, no markdown.

## Zone
- Zone tag: {{ZONE_TAG}}      # one of: A, B, C
- File paths in scope:
{{ZONE_PATHS_BULLETED}}

## Excluded
- `cosim2/sv/wb2axip/**` — vendored Apache 2.0, frozen. Findings inside these files MUST NOT appear.
- Generated files under `specgen/generated/**` — auto-generated.
- Findings about how non-vendored code uses wb2axip (port wiring, signal mapping) ARE in scope.

## Axis 1 — Karpathy 4-lens
- `karpathy_overcomplication` — unnecessary abstraction; wrapper over wrapper; framework where 1 function suffices.
- `karpathy_surgical` — changes that exceed the stated scope of a fix/feature (renames bundled with bug fixes, etc.).
- `karpathy_surface_assumptions` — implicit assumptions not documented (e.g. "assumed sync reset" without `// SYNC RESET` comment).
- `karpathy_verifiable_success` — features or fixes with no test demonstrating they work.

## Axis 2 — Magic-number hunt
- `magic_number_width` — bit-width literal (`[407:0]`, `std::array<int, 16>`).
- `magic_number_timing` — cycle counts, latencies (`wait_cycles(7)`).
- `magic_number_buffer` — buffer sizes referenced in multiple places.
- `magic_number_mask` — mask / shift literals.

## Scope rule for magic numbers
Only report literals that are HIGH IMPACT — referenced 3+ times in the same file, semantically meaningful, or have an explanatory comment that proves a constant name is missing. Do NOT report every literal you see. Template-parameter-bound values that look like literals are NOT magic.

## Cap
Maximum 30 findings per axis per zone (60 total per zone). Prioritize by severity then by impact.

## Severity classification
- `CRITICAL`: causes a bug or unsafe behavior right now (rare for a quality sweep).
- `HIGH`: significant quality / maintainability impact (likely magic number central to behavior; nameless invariants).
- `MEDIUM`: clear quality benefit, isolated.
- `LOW`: nice-to-have.

## Output schema
A JSON array of findings:

```json
[
  {
    "severity": "HIGH",
    "category": "magic_number_width",
    "file": "c_model/include/nmu/rob.hpp",
    "line": 42,
    "description": "ROB depth hardcoded as 16 in 4 places",
    "suggested_fix": "Define `constexpr size_t kRobDepth = 16;` near top of namespace and replace literals"
  }
]
```

Write the JSON to: {{OUTPUT_PATH}}
Do NOT write anything else.
```

- [ ] **Step 2: Commit**

```bash
git add cosim2/scripts/karpathy/dispatch_subagent.md
git commit -m "chore(quality): add Karpathy sweep dispatch prompt template"
```

(The same content serves as the Codex variant; pass via `codex:rescue` `args`.)

---

### Task 3: Write `merge_findings.py`

**Files:**
- Create: `cosim2/scripts/karpathy/merge_findings.py`

- [ ] **Step 1: Write the script**

```python
#!/usr/bin/env py -3
"""De-dup findings from subagent + Codex into a merged JSON.

Dedup key: (file, line, category).
On collision: keep higher-severity entry; preserve both descriptions in
`descriptions` array; mark `reviewers` array.

Usage:
    py -3 merge_findings.py <subagent.json> <codex.json> <merged.json>
"""
import json
import sys
from pathlib import Path

_RANK = {"CRITICAL": 0, "HIGH": 1, "MEDIUM": 2, "LOW": 3}


def _load(path: Path, reviewer: str) -> list:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, list):
        raise ValueError(f"{path}: expected JSON array, got {type(raw).__name__}")
    for f in raw:
        f["reviewers"] = [reviewer]
        f["descriptions"] = [f.get("description", "")]
    return raw


def merge(subagent: Path, codex: Path) -> list:
    items = _load(subagent, "claude-subagent") + _load(codex, "codex")
    seen: dict[tuple, dict] = {}
    for f in items:
        key = (f["file"], f["line"], f["category"])
        if key not in seen:
            seen[key] = f
            continue
        existing = seen[key]
        # Merge reviewers + descriptions
        existing["reviewers"] = sorted(set(existing["reviewers"] + f["reviewers"]))
        existing["descriptions"] = list(dict.fromkeys(existing["descriptions"] + f["descriptions"]))
        # Higher severity wins
        if _RANK[f["severity"]] < _RANK[existing["severity"]]:
            existing["severity"] = f["severity"]
            existing["suggested_fix"] = f["suggested_fix"]
    return sorted(
        seen.values(),
        key=lambda x: (_RANK[x["severity"]], x["file"], x["line"]),
    )


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(f"usage: {argv[0]} <subagent.json> <codex.json> <out.json>", file=sys.stderr)
        return 2
    out = merge(Path(argv[1]), Path(argv[2]))
    Path(argv[3]).write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(out)} merged findings -> {argv[3]}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
```

- [ ] **Step 2: Smoke-test with stub inputs**

```bash
cd /e/05_NoC/noc_project
mkdir -p /tmp/karpathy_test
cat > /tmp/karpathy_test/a.json << 'EOF'
[
  {"severity":"HIGH","category":"magic_number_width","file":"x.hpp","line":1,"description":"sub: 16 is magic","suggested_fix":"constexpr"}
]
EOF
cat > /tmp/karpathy_test/b.json << 'EOF'
[
  {"severity":"CRITICAL","category":"magic_number_width","file":"x.hpp","line":1,"description":"codex: 16 used in 5 places","suggested_fix":"constexpr int kX = 16;"}
]
EOF
py -3 cosim2/scripts/karpathy/merge_findings.py /tmp/karpathy_test/a.json /tmp/karpathy_test/b.json /tmp/karpathy_test/merged.json
cat /tmp/karpathy_test/merged.json
```

Expected: 1 finding with `severity: CRITICAL` (winner), both reviewers listed, both descriptions preserved.

- [ ] **Step 3: Commit**

```bash
git add cosim2/scripts/karpathy/merge_findings.py
git commit -m "chore(quality): add Karpathy sweep findings merge script"
```

---

### Task 4: Dispatch Zone A (parallel subagent + Codex)

**Files:**
- Create: `cosim2/quality/karpathy/zone_A_subagent.json`
- Create: `cosim2/quality/karpathy/zone_A_codex.json`

**Note:** Single message with TWO tool calls — Agent + Skill — so they run in parallel per [[feedback-superpowers-entry]] dispatch pattern.

- [ ] **Step 1: Compose the zone-specific prompt by template substitution**

Read `cosim2/scripts/karpathy/dispatch_subagent.md`. Substitute:
- `{{ZONE_TAG}}` → `A`
- `{{ZONE_PATHS_BULLETED}}` → bullet list:
    - `c_model/include/axi/**`
    - `c_model/include/nmu/**`
    - `c_model/include/nsu/**`
    - `c_model/include/common/**`
    - `c_model/include/noc/**`
    - `c_model/tests/axi/**`
    - `c_model/tests/nmu/**`
    - `c_model/tests/nsu/**`
- `{{OUTPUT_PATH}}` → `cosim2/quality/karpathy/zone_A_subagent.json`

For Codex, change only `{{OUTPUT_PATH}}` to `cosim2/quality/karpathy/zone_A_codex.json`.

- [ ] **Step 2: Dispatch both in parallel — single tool message with two calls**

In ONE Claude tool message:
1. Call Agent tool: `subagent_type: general-purpose`, prompt = subagent-substituted template.
2. Call Skill tool: `skill: codex:rescue`, args = codex-substituted template.

Wait for both to return.

- [ ] **Step 3: Verify both JSON files exist and parse**

```bash
py -3 -c "
import json
for p in ('cosim2/quality/karpathy/zone_A_subagent.json',
          'cosim2/quality/karpathy/zone_A_codex.json'):
    d = json.load(open(p))
    assert isinstance(d, list), p
    print(f'{p}: {len(d)} findings')
"
```

- [ ] **Step 4: Commit raw findings**

```bash
git add cosim2/quality/karpathy/zone_A_subagent.json \
        cosim2/quality/karpathy/zone_A_codex.json
git commit -m "chore(quality-zone-A): record raw sweep findings (subagent + Codex)"
```

---

### Task 5: Dispatch Zone B (parallel subagent + Codex)

Same shape as Task 4. Substitute:
- `{{ZONE_TAG}}` → `B`
- `{{ZONE_PATHS_BULLETED}}` →
    - `c_model/include/cosim2/**`
    - `cosim2/c/**`
    - `cosim2/sv/**` (excluding `wb2axip/`)
    - `cosim2/verilator/**`
    - `cosim2/tests/**`

- [ ] **Step 1-4: Repeat Task 4 with Zone B paths**

```bash
git add cosim2/quality/karpathy/zone_B_subagent.json \
        cosim2/quality/karpathy/zone_B_codex.json
git commit -m "chore(quality-zone-B): record raw sweep findings (subagent + Codex)"
```

---

### Task 6: Dispatch Zone C (parallel subagent + Codex)

Same shape. Substitute:
- `{{ZONE_TAG}}` → `C`
- `{{ZONE_PATHS_BULLETED}}` → `specgen/**` (excluding `specgen/generated/`)

- [ ] **Step 1-4: Repeat Task 4 with Zone C paths**

```bash
git add cosim2/quality/karpathy/zone_C_subagent.json \
        cosim2/quality/karpathy/zone_C_codex.json
git commit -m "chore(quality-zone-C): record raw sweep findings (subagent + Codex)"
```

---

### Task 7: Merge findings per zone

- [ ] **Step 1: Run merge_findings.py for each zone**

```bash
cd /e/05_NoC/noc_project
for z in A B C; do
    py -3 cosim2/scripts/karpathy/merge_findings.py \
        cosim2/quality/karpathy/zone_${z}_subagent.json \
        cosim2/quality/karpathy/zone_${z}_codex.json \
        cosim2/quality/karpathy/zone_${z}_merged.json
done
```

Expected: 3 merged JSON files. stderr reports per-zone finding counts.

- [ ] **Step 2: Sanity-check no wb2axip findings leaked through**

```bash
py -3 -c "
import json
for z in 'ABC':
    d = json.load(open(f'cosim2/quality/karpathy/zone_{z}_merged.json'))
    bad = [f for f in d if 'wb2axip' in f['file']]
    assert not bad, f'Zone {z}: leaked wb2axip findings: {bad}'
print('OK: no wb2axip findings in merged files')
"
```

- [ ] **Step 3: Commit**

```bash
git add cosim2/quality/karpathy/zone_{A,B,C}_merged.json
git commit -m "chore(quality): merge per-zone sweep findings (subagent + Codex de-dup)"
```

---

### Task 8: User decision matrix

**Files:**
- Create: `cosim2/quality/karpathy/zone_{A,B,C}_decisions.json`
- Create: `cosim2/quality/karpathy/deferred_findings.json`
- Create: `cosim2/quality/karpathy/ignored_findings.json` (if any ignored)

- [ ] **Step 1: For each zone, copy merged → decisions and present to user**

```bash
for z in A B C; do
    cp cosim2/quality/karpathy/zone_${z}_merged.json \
       cosim2/quality/karpathy/zone_${z}_decisions.json
done
```

Then surface each `zone_<X>_decisions.json` to the user. For each finding, user marks one of:
- `fix` → field added: `"decision": "fix"`
- `defer` → fields added: `"decision": "defer"`, `"owner": "<name or linear ID>"`, `"rationale": "<short>"`
- `ignore` → fields added: `"decision": "ignore"`, `"rationale": "<short>"`

Default suggestions (user may override):
- CRITICAL / HIGH → `fix`
- MEDIUM → user picks
- LOW → `defer`

- [ ] **Step 2: Split deferred + ignored into global files**

```bash
py -3 - << 'PY'
import json, pathlib
deferred = []
ignored = []
for z in 'ABC':
    p = pathlib.Path(f'cosim2/quality/karpathy/zone_{z}_decisions.json')
    d = json.loads(p.read_text())
    keep = []
    for f in d:
        if f.get('decision') == 'defer':
            f['zone'] = z
            deferred.append(f)
        elif f.get('decision') == 'ignore':
            f['zone'] = z
            ignored.append(f)
        else:
            keep.append(f)
    p.write_text(json.dumps(keep, indent=2) + '\n')

pathlib.Path('cosim2/quality/karpathy/deferred_findings.json').write_text(
    json.dumps(deferred, indent=2) + '\n')
if ignored:
    pathlib.Path('cosim2/quality/karpathy/ignored_findings.json').write_text(
        json.dumps(ignored, indent=2) + '\n')
print(f'deferred: {len(deferred)}, ignored: {len(ignored)}')
PY
```

- [ ] **Step 3: Verify no entry lacks `decision`**

```bash
py -3 -c "
import json
for z in 'ABC':
    d = json.load(open(f'cosim2/quality/karpathy/zone_{z}_decisions.json'))
    missing = [f for f in d if 'decision' not in f]
    assert not missing, f'Zone {z}: {len(missing)} entries missing decision'
print('OK: all entries have decision field')
"
```

- [ ] **Step 4: Commit decisions**

```bash
git add cosim2/quality/karpathy/zone_{A,B,C}_decisions.json \
        cosim2/quality/karpathy/deferred_findings.json
[ -f cosim2/quality/karpathy/ignored_findings.json ] && \
    git add cosim2/quality/karpathy/ignored_findings.json
git commit -m "chore(quality): record user decisions for Karpathy sweep findings"
```

---

### Task 9: Apply Zone A architectural fixes

**Files:** identified by `karpathy_*` entries in `zone_A_decisions.json` with `decision == "fix"`.

- [ ] **Step 1: Filter the work list**

```bash
py -3 -c "
import json
d = json.load(open('cosim2/quality/karpathy/zone_A_decisions.json'))
arch = [f for f in d if f.get('decision') == 'fix' and f['category'].startswith('karpathy_')]
print(f'Zone A architectural fixes to apply: {len(arch)}')
for f in arch:
    print(f'  [{f[\"severity\"]}] {f[\"file\"]}:{f[\"line\"]} -- {f[\"description\"]}')
" > /tmp/zone_A_arch_worklist.txt
cat /tmp/zone_A_arch_worklist.txt
```

If count == 0: skip to Task 10. Otherwise continue.

- [ ] **Step 2: Apply each fix**

For each entry in the work list:
- Read the named file
- Apply `suggested_fix`, adapting to the surrounding context (the suggestion is a hint, not a literal patch)
- Commit per logical group (one file or one symbol per commit)

- [ ] **Step 3: Run regression gate**

```bash
cd /e/05_NoC/noc_project/c_model/build
ctest --output-on-failure
```

Expected: same pass count as baseline (`cosim2/quality/karpathy/baseline.txt`). If any test fails, revert the offending commit and move the finding to `deferred_findings.json` with severity HIGH and `rationale: "fix regressed test <name>; deferred for investigation"`.

- [ ] **Step 4: Open PR**

```bash
git push -u origin <branch>
gh pr create --title "chore(quality-zone-A-arch): apply Karpathy architectural findings" \
    --body "Applies Karpathy 4-lens architectural findings from Zone A (Stage 3 c_model core). See cosim2/quality/karpathy/zone_A_decisions.json for the finding list."
```

---

### Task 10: Apply Zone A magic-number fixes

Same shape as Task 9, filtering `category` starting with `magic_number_`.

- [ ] **Step 1: Filter**, **Step 2: Apply**, **Step 3: Regression gate**, **Step 4: Open PR with title `chore(quality-zone-A-magic): ...`**

(Skip the entire task if 0 entries match.)

---

### Task 11: Apply Zone B architectural fixes

Same as Task 9 with Zone B data.

---

### Task 12: Apply Zone B magic-number fixes

Same as Task 10 with Zone B data.

---

### Task 13: Apply Zone C architectural fixes

Same as Task 9 with Zone C data.

---

### Task 14: Apply Zone C magic-number fixes

Same as Task 10 with Zone C data.

---

### Task 15: Final aggregate verification

**Files:** none (verification only).

- [ ] **Step 1: Compare baseline vs current**

```bash
cd /e/05_NoC/noc_project
{
    echo "=== Karpathy sweep post-baseline ==="
    echo "Date: $(date -Iseconds)"
    echo "Commit: $(git rev-parse HEAD)"
    echo
    echo "--- c_model ctest ---"
    (cd c_model/build && ctest --output-on-failure 2>&1) | tail -5
    echo
    echo "--- specgen pytest ---"
    (cd specgen && py -3 -m pytest tests/ -q 2>&1) | tail -5
    echo
    echo "--- drift gate ---"
    (cd specgen && py -3 tools/codegen.py --check && echo "drift: OK") || echo "drift: FAIL"
} | tee cosim2/quality/karpathy/post_baseline.txt

diff cosim2/quality/karpathy/baseline.txt cosim2/quality/karpathy/post_baseline.txt | head -20 || true
```

Expected: pass counts identical (no regression introduced by fixes).

- [ ] **Step 2: Confirm all `decision` fields populated and all deferred entries have `owner`**

```bash
py -3 -c "
import json, sys
fail = 0
for z in 'ABC':
    d = json.load(open(f'cosim2/quality/karpathy/zone_{z}_decisions.json'))
    bad = [f for f in d if 'decision' not in f]
    if bad:
        print(f'Zone {z}: {len(bad)} entries without decision')
        fail = 1
deferred = json.load(open('cosim2/quality/karpathy/deferred_findings.json'))
no_owner = [f for f in deferred if not f.get('owner') or f['owner'] in ('', 'TBD')]
if no_owner:
    print(f'Deferred: {len(no_owner)} entries without owner (placeholders not allowed)')
    fail = 1
sys.exit(fail)
"
```

- [ ] **Step 3: Commit final aggregate log**

```bash
git add cosim2/quality/karpathy/post_baseline.txt
git commit -m "chore(quality): record Karpathy sweep post-baseline (no regression)"
```

---

## Self-Review

**Spec coverage:**
- §1.2 in scope (3 zones + 4-lens + magic-number + 2-reviewer protocol) → Tasks 2-14 ✓
- §2 zones → Tasks 4-6 dispatch + Tasks 9-14 apply ✓
- §3 two-axis findings → Task 2 dispatch template explicitly lists both axes ✓
- §4.2 Codex via `codex:rescue` (not direct Agent) → Tasks 4-6 Step 2 explicit ✓
- §4.3 findings JSON schema → Task 2 prompt template + Task 3 merge script enforce ✓
- §5 de-dup + merge → Task 3 (merge_findings.py) + Task 7 (run merges) ✓
- §6 user decision matrix → Task 8 ✓
- §7 per-zone PR split (arch vs magic) → Tasks 9-14 ✓
- §8 test plan → Task 1 (pre-baseline), Tasks 9-14 Step 3 (per-PR regression), Task 15 (post-baseline) ✓
- §9 open items (cap at 30 per axis; reviewer disagreement; regression-revert; merge conflicts with W1+W2; wb2axip exclusion; template-bound literals) → Task 2 template covers all ✓

**Placeholder scan:** `merge_findings.py` defined inline with full code in Task 3. Dispatch template defined inline in Task 2. No `...` or "or whatever" hand-waves outside intentional ellipsis. No deferred fields with `owner: TBD` allowed past Task 15.

**Type consistency:**
- Findings JSON schema consistent across Tasks 2 (dispatch), 3 (merge), 7 (sanity check), 8 (decisions), 9-14 (filtering), 15 (final verify).
- `severity` set: `CRITICAL | HIGH | MEDIUM | LOW` used uniformly.
- `category` prefixes: `karpathy_*` and `magic_number_*` consistent across dispatch + filtering.

---

## Notes for Executor

- **Codex dispatch MUST use `codex:rescue` skill, NOT direct Agent call.** Direct Agent dispatch with `subagent_type: codex:codex-rescue` is fire-and-forget and the result never returns (Stage 5b lesson, [[feedback-codex-review-each-round]] memory).
- Dispatch the subagent + Codex per zone in a **single tool message with two calls** (parallel execution). Three separate zone dispatches across Tasks 4-6 — each task is one parallel pair.
- Per-PR ctest regression gate is mandatory. If a fix regresses any test, REVERT that commit and move the finding to `deferred_findings.json` with severity HIGH. Do NOT silence the test or hand-tweak the fix to make it pass — the test catching it is information.
- W1+W2 atomic refactor (sibling spec) is independent of this sweep. If both ship in the same window, user decides merge order. If sweep ships first, W1+W2 rebases. If W1+W2 ships first, sweep runs on the post-W2 tree (re-run Task 1 baseline then proceed).
- `cosim2/sv/wb2axip/**` exclusion is enforced at the dispatch prompt level (Task 2 template). Task 7 Step 2 sanity-checks no leakage in merged output.
- Task 8 (user decision matrix) is the human-in-the-loop step. Findings volume drives wall-clock. If volume is large, do per-zone decisions in separate sessions; this plan is resumable mid-Task-8.
