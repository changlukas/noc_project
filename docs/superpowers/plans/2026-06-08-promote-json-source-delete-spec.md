# Promote JSON Source-of-Truth + Delete spec/ Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Delete the `spec/*.md` markdown layer and promote `specgen/generated/json/` to canonical source-of-truth. After this, no code path reads `spec/`; the JSON files are the spec.

**Architecture:** Four atomic commits. C1 relocates VERSION + updates loader. C2 deletes the md→json generator package and its tests. C3 git rm -rf spec/. C4 cleans 11 surviving doc/code cross-references.

**Tech Stack:** Python 3.9+, specgen tooling, GoogleTest, CMake. No new dependencies.

**Spec doc:** `docs/superpowers/specs/2026-06-08-promote-json-source-delete-spec-design.md` (committed at `0c5d829`).

**Pre-existing environment note:** Windows MinGW GCC 15.2 ICE prevents `test_meta_buffer` and `test_loopback_noc_per_vc_credit` from building. NOT caused by this plan; verified pre-existing in prior session.

---

## Task 1 — C1: Relocate VERSION + update loader

**Files:**
- Move: `spec/ni/VERSION` → `specgen/source/VERSION`
- Modify: `specgen/ni_spec/loader.py` (lines 76-83)

- [ ] **Step 1.1: Move VERSION via git mv**

  ```bash
  git mv spec/ni/VERSION specgen/source/VERSION
  ```

  Verify: `git status -s` shows one renamed entry, no deleted/created mismatch.

- [ ] **Step 1.2: Update loader.py**

  Open `specgen/ni_spec/loader.py`. Locate the `load_spec_version` function at line 75. Replace:

  ```python
  def load_spec_version() -> str:
      """Read spec/ni/VERSION (single source of truth for spec_version).

      Looks for the file relative to the specgen parent directory:
          noc_project/spec/ni/VERSION  (one-line semver, no trailing newline content).
      """
      version_file = SPECGEN_ROOT.parent / "spec" / "ni" / "VERSION"
      if not version_file.exists():
          raise FileNotFoundError(f"spec/ni/VERSION not found at {version_file}")
      return version_file.read_text(encoding="utf-8").strip()
  ```

  with:

  ```python
  def load_spec_version() -> str:
      """Read specgen/source/VERSION (single source of truth for spec_version).

      Looks for the file relative to the specgen root:
          specgen/source/VERSION  (one-line semver, no trailing newline content).
      """
      version_file = SPECGEN_ROOT / "source" / "VERSION"
      if not version_file.exists():
          raise FileNotFoundError(f"specgen/source/VERSION not found at {version_file}")
      return version_file.read_text(encoding="utf-8").strip()
  ```

- [ ] **Step 1.3: Verify VERSION test passes**

  Run: `py -3 -m pytest specgen/tests/test_foundation.py::test_load_spec_version_returns_string -v`
  Expected: PASS.

- [ ] **Step 1.4: Verify drift gate**

  Run: `py -3 specgen/tools/codegen.py --check`
  Expected: exit 0 (no drift; the spec_version embedded in cpp/sv headers is unchanged since VERSION content is unchanged).

- [ ] **Step 1.5: Verify specgen goldens**

  Run: `py -3 -m pytest specgen/tests/test_byte_identical_golden.py -v`
  Expected: 6/6 PASS.

- [ ] **Step 1.6: Commit**

  ```bash
  git add spec/ni/VERSION specgen/source/VERSION specgen/ni_spec/loader.py
  git commit -m "refactor(specgen): relocate VERSION to specgen/source/ + update loader

  Move spec/ni/VERSION -> specgen/source/VERSION so VERSION lives
  alongside constants.yaml and other specgen source artifacts.
  Update specgen/ni_spec/loader.py load_spec_version() path,
  docstring, and error message to match. Prep step for deleting
  spec/ entirely (see docs/superpowers/specs/2026-06-08-promote-
  json-source-delete-spec-design.md).

  Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
  ```

---

## Task 2 — C2: Delete markdown → JSON generator layer

**Files:**
- Delete: `specgen/ni_spec/__main__.py`
- Delete: `specgen/ni_spec/generator/` (entire package: `packet.py`, `signals.py`, `registers.py`, `protocol_rules.py`, `_common.py`, `__init__.py`)
- Delete: `specgen/tests/test_protocol_rules.py`
- Delete: `specgen/tests/test_registers_parser.py`
- Modify: `specgen/tests/test_foundation.py` (remove md-parser test methods, lines 83-104)
- Modify: `specgen/ni_spec/__init__.py` (drop generator re-exports)

- [ ] **Step 2.1: Read `specgen/ni_spec/__init__.py` to confirm generator import lines**

  Use Read on `specgen/ni_spec/__init__.py`. Identify the `from .generator import ...` block (Codex reports lines 15-21) and the `__all__` list entries that reference generator symbols (Codex reports lines 37-40). Confirm before editing.

- [ ] **Step 2.2: Edit `specgen/ni_spec/__init__.py` to remove generator references**

  Remove:
  - The `from .generator import ...` import block (lines 15-21)
  - The `__all__` entries that reference deleted generator names (lines 37-40)

  Keep:
  - `from .invariants import ...`
  - `from .report import ...`
  - All other unrelated content

- [ ] **Step 2.3: Delete `specgen/ni_spec/__main__.py`**

  ```bash
  git rm specgen/ni_spec/__main__.py
  ```

- [ ] **Step 2.4: Delete the generator package**

  ```bash
  git rm -r specgen/ni_spec/generator
  ```

- [ ] **Step 2.5: Delete md-parser tests**

  ```bash
  git rm specgen/tests/test_protocol_rules.py
  git rm specgen/tests/test_registers_parser.py
  ```

- [ ] **Step 2.6: Strip md-parser methods from `test_foundation.py`**

  Open `specgen/tests/test_foundation.py`. Use Read to inspect the full file. Identify the test methods at lines 83-104 that import `from ni_spec.generator import parse_header_fields` and read `spec/ni/doc/packet_format.md`. Delete those test methods entirely. Keep:
  - `test_load_spec_version_returns_string` (top of file)
  - All tests that load JSON via `loader.load_doc(PACKET_JSON)` (these survive — JSON is the new source)
  - Any helper functions like `_load_packet()`

- [ ] **Step 2.7: Confirm `__pycache__` directories don't haunt us**

  Run: `find specgen -name "__pycache__" -type d 2>&1` to list.
  Delete stale `.pyc` files: `find specgen -name "*.pyc" -delete 2>&1` (these are not tracked but can cause confusion).

- [ ] **Step 2.8: Verify package still imports**

  Run: `py -3 -c "import specgen.ni_spec; print('ok')"`
  Expected: prints `ok` (no ImportError from dead generator re-exports).

- [ ] **Step 2.9: Verify surviving tests pass**

  Run: `py -3 -m pytest specgen/tests/ -v 2>&1 | tail -25`
  Expected: all collected tests PASS; the 3 deleted test files are absent from the collection report.

- [ ] **Step 2.10: Verify drift gate**

  Run: `py -3 specgen/tools/codegen.py --check`
  Expected: exit 0.

- [ ] **Step 2.11: Commit**

  ```bash
  git add specgen/ni_spec/__init__.py specgen/tests/test_foundation.py
  git commit -m "refactor(specgen): delete markdown -> JSON generator layer

  Promote specgen/generated/json/ to source-of-truth. Delete the
  md->json layer that drove drift between markdown prose and JSON:
  remove specgen/ni_spec/__main__.py (CLI entry), the entire
  specgen/ni_spec/generator/ package (packet/signals/registers/
  protocol_rules/_common + __init__.py), and the md-parser tests
  (test_protocol_rules.py, test_registers_parser.py, the parse_*
  methods in test_foundation.py).

  Keep specgen/ni_spec/invariants.py and report.py: imported by
  __init__.py and surviving test files (test_registers_validator,
  test_pin_level_reset, test_function_blocks). Clean up
  __init__.py re-exports of the deleted generator symbols.

  codegen.py JSON->cpp/sv pipeline is unchanged.

  Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
  ```

---

## Task 3 — C3: git rm -rf spec/

**Files:**
- Delete: `spec/` (entire subtree, 17 files after C1 moved VERSION)

- [ ] **Step 3.1: Verify VERSION is gone from spec/**

  Run: `ls spec/ni/VERSION 2>&1`
  Expected: "No such file or directory" (C1 moved it).

- [ ] **Step 3.2: List files about to be deleted**

  Run: `git ls-files spec/`
  Expected: 17 files under `spec/ni/{README.md, MODE.md, doc/, dv/, images/}`.

- [ ] **Step 3.3: Delete spec/ via git rm -rf**

  ```bash
  git rm -rf spec/
  ```

- [ ] **Step 3.4: Verify no broken imports**

  Run: `py -3 -c "from ni_spec.loader import load_spec_version; print(load_spec_version())"` from `specgen/`.

  Alternative: `cd specgen && py -3 -c "import ni_spec; v = ni_spec.loader.load_spec_version(); print(v)" && cd ..`

  Expected: prints the version string.

- [ ] **Step 3.5: Verify drift gate**

  Run: `py -3 specgen/tools/codegen.py --check`
  Expected: exit 0.

- [ ] **Step 3.6: Verify surviving tests pass**

  Run: `py -3 -m pytest specgen/tests/ -v 2>&1 | tail -25`
  Expected: all PASS (no test was reaching into spec/ after C2).

- [ ] **Step 3.7: Commit**

  ```bash
  git commit -m "chore: git rm -rf spec/

  The spec/ markdown layer has been the source of drift between
  prose claims and the codegen-anchored JSON for some time. With
  the md->JSON generator layer removed (C2) and VERSION relocated
  to specgen/source/ (C1), no code path reads spec/. Delete it.

  Future spec edits go directly into specgen/generated/json/*.json
  (drift-gate-protected against cpp/sv) and docs/image/*.jpg
  (visual ground truth).

  Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
  ```

---

## Task 4 — C4: Doc cross-reference cleanup

**Files (11 locations from §3.6 spec inventory):**
- Modify: `README.md` (lines 28, 65)
- Modify: `docs/architecture.md` (§7 References)
- Modify: `docs/development.md` (lines 128, 470)
- Modify: `docs/_archive/README.md` (§When not to read this)
- Modify: `Makefile` (line 93 `MAINTAINED_DOCS`)
- Modify: `specgen/source/ni_function_blocks.json` (lines 7-8 source array)
- Modify: `specgen/ni_spec/constants.py` (line 383 comment)
- Modify or move: `specgen/tools/README.md`
- Modify or move: `specgen/docs/guide/json-to-code-examples.md`

- [ ] **Step 4.1: Edit `README.md` — drop both spec/ni refs**

  At line 28, delete the bullet:
  ```
  - `spec/ni/` - normative NI specification
  ```

  At line 65, delete the bullet:
  ```
  - [NI specification](spec/ni/README.md)
  ```

  Renumber/reflow surrounding bullets if needed.

- [ ] **Step 4.2: Edit `docs/architecture.md` — drop spec/ni from §7 References**

  Open the file; find the `spec/ni/README.md` bullet in §7 References. Delete the bullet.

- [ ] **Step 4.3: Edit `docs/development.md` — drop both refs**

  At line 128 (the lint_docs MAINTAINED_DOCS narrative), remove the `spec/ni/README.md` entry from the parenthetical list.

  At line 470 (§9 References), delete the `spec/ni/README.md` bullet.

- [ ] **Step 4.4: Edit `docs/_archive/README.md` — drop "Looking for the normative NI spec" bullet**

  In the "When not to read this" section, delete:
  ```
  - Looking for the normative NI spec: read `../../spec/ni/README.md`
  ```

- [ ] **Step 4.5: Edit `Makefile` — drop spec/ni from MAINTAINED_DOCS**

  At line 93, remove `spec/ni/README.md` from the `MAINTAINED_DOCS` variable (and the trailing backslash on the preceding line if `spec/ni/README.md` was the last entry).

- [ ] **Step 4.6: Edit `specgen/source/ni_function_blocks.json`**

  Open the JSON. The `source` array around lines 7-8 contains string entries:
  ```json
  "spec/ni/README.md §Features",
  "spec/ni/doc/theory_of_operation.md"
  ```

  Replace with the new ground-truth references:
  ```json
  "docs/image/header.jpg",
  "specgen/generated/json/ni_packet.json"
  ```

  (or delete the entries entirely if no replacement is meaningful; pick whichever keeps the schema valid).

- [ ] **Step 4.7: Edit `specgen/ni_spec/constants.py` line 383**

  Find the comment:
  ```python
      Per spec/ni/doc/packet_format.md and ni_signals.json NOC_REQ_OUT /
  ```

  Replace with:
  ```python
      Per specgen/generated/json/ni_packet.json and ni_signals.json NOC_REQ_OUT /
  ```

- [ ] **Step 4.8: Decide and action on `specgen/tools/README.md`**

  Read the file. Identify the md→json workflow narrative at lines 10, 224, 264, 274, 314 (per Codex audit).

  **Decision:**
  - If the project still wants a specgen workflow guide, REWRITE the README to describe the JSON-source flow: "Edit `specgen/generated/json/*.json` directly; run `py -3 specgen/tools/codegen.py --target cpp --domain packet` (etc.) to regenerate cpp/sv constants. Drift gate via `--check` enforces JSON ↔ cpp/sv consistency."
  - If the new flow is trivial (edit JSON, run codegen, done), MOVE the file to `docs/_archive/specgen/2026-06-08-md-source-workflow-README.md` and leave a one-line replacement at `specgen/tools/README.md` pointing to the archived doc.

  Pick the lower-effort path that still leaves future contributors with a clear pointer. Default to MOVE if undecided.

- [ ] **Step 4.9: Decide and action on `specgen/docs/guide/json-to-code-examples.md`**

  Read the file. If it describes the md→json workflow at lines 1, 4, 9, 24, 35 (per Codex), update those sections to reflect the JSON-source flow.

  If the file is fundamentally about the deleted layer, move it to `docs/_archive/specgen/2026-06-08-json-to-code-examples.md`.

  Default to UPDATE if the file's structure can carry the new workflow; MOVE otherwise.

- [ ] **Step 4.10: Run lint_docs**

  Run: `make lint_docs PYTHON3="py -3"`
  Expected: clean. The Makefile's `MAINTAINED_DOCS` list was updated in Step 4.5 to drop `spec/ni/README.md`.

- [ ] **Step 4.11: Run drift gate**

  Run: `py -3 specgen/tools/codegen.py --check`
  Expected: exit 0.

- [ ] **Step 4.12: Run full make check**

  Run: `make check PYTHON3="py -3" 2>&1 | tail -20`
  Expected: lint_scenarios + lint_docs + builds + ctest. Pre-existing GCC ICE on `test_meta_buffer` and `test_loopback_noc_per_vc_credit` is acceptable (not introduced by this plan).

- [ ] **Step 4.13: Final survey — no surviving spec/ni references**

  Run:
  ```bash
  grep -rn "spec/ni\|packet_format\.md\|signal_interface\.md\|protocol_rules\.md\|registers\.md\|theory_of_operation" --include="*.py" --include="*.cpp" --include="*.hpp" --include="*.md" --include="Makefile" . 2>&1 | grep -v "^Binary file" | grep -v "docs/_archive/" | grep -v "docs/superpowers/specs/2026-06-08" | grep -v "docs/superpowers/specs/2026-06-07" | grep -v "docs/superpowers/plans/2026-06-08" | grep -v "docs/superpowers/plans/2026-06-07"
  ```

  Expected: empty. Any survivor must be either fixed inline or explicitly justified (e.g. unavoidable historical comment).

- [ ] **Step 4.14: Commit**

  ```bash
  git add README.md docs/architecture.md docs/development.md docs/_archive/README.md Makefile specgen/source/ni_function_blocks.json specgen/ni_spec/constants.py specgen/tools/README.md specgen/docs/guide/json-to-code-examples.md docs/_archive/specgen/
  git commit -m "docs: drop spec/ni cross-references after pipeline refactor

  11 locations cleaned per docs/superpowers/specs/2026-06-08-promote-
  json-source-delete-spec-design.md inventory: README.md, docs/
  architecture.md, docs/development.md, docs/_archive/README.md,
  Makefile MAINTAINED_DOCS, specgen/source/ni_function_blocks.json
  source array, specgen/ni_spec/constants.py:383 comment, and a
  rewrite-or-archive decision applied to specgen/tools/README.md and
  specgen/docs/guide/json-to-code-examples.md.

  After this commit, no functional repo reference points at spec/.

  Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
  ```

---

## Post-implementation gate (Codex acceptance review)

After Task 4 commit, **before pushing**:

- [ ] **Step P.1: Generate full diff vs `origin/main`**

  Run: `git diff origin/main..HEAD > .json_source_diff.patch && wc -l .json_source_diff.patch`

- [ ] **Step P.2: Codex final acceptance review**

  Dispatch `codex:codex-rescue` with: "Read `./.json_source_diff.patch`. Verify all 4 commits implement `docs/superpowers/specs/2026-06-08-promote-json-source-delete-spec-design.md`. Spot-check: (a) `specgen/source/VERSION` exists with original content, (b) `loader.py:load_spec_version` reads new path, (c) `specgen/ni_spec/generator/` is gone, (d) `specgen/ni_spec/__main__.py` is gone, (e) `spec/` directory is empty, (f) all 11 cross-ref locations in §3.6 are cleaned, (g) `codegen.py --check` exits 0, (h) `pytest specgen/tests/` passes. Report any MUST-FIX before push."

- [ ] **Step P.3: If Codex flags issues, fix inline and add fixup commits. Re-run Codex until clean.**

- [ ] **Step P.4: Cleanup + push**

  ```bash
  rm .json_source_diff.patch
  git push origin main
  ```

---

## Self-review checklist

**Spec coverage:** Every commit C1-C4 in the spec maps to a task here:
- C1 spec → Task 1 (6 steps)
- C2 spec → Task 2 (11 steps)
- C3 spec → Task 3 (7 steps)
- C4 spec → Task 4 (14 steps)

**Placeholder scan:** Every command has expected output. Every code block shows the actual replacement text. No "TBD" or "TODO". Steps 4.8 and 4.9 contain a small decision (rewrite vs move) but with a clear default (MOVE if undecided).

**Type consistency:** `load_spec_version` returns `str` (unchanged signature). `SPECGEN_ROOT / "source" / "VERSION"` is `pathlib.Path` (consistent with old expression).

**Ordering safety:** C1 moves VERSION before C2/C3 touch loader / spec; C2 deletes generator before C3 deletes spec (so deleted code can't reference still-present md); C3 must succeed before C4 because doc cleanup describes the post-spec/ state.
