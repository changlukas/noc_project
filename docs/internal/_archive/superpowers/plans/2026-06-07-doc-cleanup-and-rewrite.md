# Doc Cleanup + Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Trim 72 tracked .md files to 6 maintained docs + archive of 34 files. Add root README.md, docs/architecture.md, docs/development.md. Wire mandatory ASCII byte check into make check. Update CLAUDE.md and cosim/sv/wb2axip/ATTRIBUTION.md links.

**Architecture:** Four commits with explicit review gate between commits 3 and 4. Commit 1 lands `.gitignore` housekeeping (clean working tree first). Commit 2 is pure file moves (git mv + 1 new README). Commit 3 lands new docs + ASCII lint + link maintenance. Review gate runs disposition matrix verification. Commit 4 deletes obsolete files.

**Tech Stack:** Markdown, Python 3 (ASCII lint), git, grep. No code changes; documentation round.

**Spec:** `docs/superpowers/specs/2026-06-07-doc-cleanup-and-rewrite-design.md`

**Project-specific discipline (per CLAUDE.md + memory):**
- ASCII-only enforcement in maintained docs (any byte >0x7F fails)
- Commit messages `type(scope): description` (English) — types: feat, fix, docs, style, refactor, test, chore, perf
- Never `--no-verify`
- Use `py -3` not `python3` (Windows)
- Use forward slashes in source paths

---

## File Structure

### Files created (across all commits)

| Path | Commit | Responsibility |
|---|---|---|
| `README.md` (root) | 3 | Project-level entry; tagline / status / architecture / build / test / docs links |
| `docs/architecture.md` | 3 | Single architecture document; 6 sections + TOC |
| `docs/development.md` | 3 | Development guide; 8 sections + TOC |
| `docs/_archive/README.md` | 2 | Historical material warning + 4-class taxonomy ref |
| `tools/lint_docs.py` | 3 | Mandatory ASCII byte check for maintained docs |

### Files modified

| Path | Commit | Change |
|---|---|---|
| `.gitignore` | 1 | Add /Python/, /specgen/Python/, /.codegraph/, /dpi_ref/, /issue/, /docs/image/*.jpg, /.pytest_cache/, __pycache__/, *.py[cod] |
| `Makefile` | 3 | Add `lint_docs` target; chain into `check` |
| `CLAUDE.md` | 3 | Retarget link to docs/noc_cmodel_rtl_plan.md (now at docs/_archive/); drop 2 stale 2026-05-31 spec references |
| `cosim/sv/wb2axip/ATTRIBUTION.md` | 3 | Retarget link from cosim/KNOWN_LIMITATIONS.md to docs/architecture.md anchor |
| `spec/ni/README.md` | 3 | Heading case + ASCII substitutions + filename link corrections only (no content change) |

### Files moved (commit 2)

34 files moved via `git mv` to `docs/_archive/`:
- `docs/superpowers/specs/*.md` (14 files) -> `docs/_archive/superpowers/specs/`
- `docs/superpowers/plans/*.md` (12 files) -> `docs/_archive/superpowers/plans/`
- `docs/noc_cmodel_rtl_plan.md` -> `docs/_archive/noc_cmodel_rtl_plan.md`
- `spec/ni/{NEXT_SESSION_A6, PRESENTATION_OUTLINE, PRESENTATION_STYLE, SLIDES, IMPLEMENTER_REVIEW_LOG, READER_TEST_LOG}.md` (6 files) -> `docs/_archive/spec_ni/<lowercase-kebab>.md`
- `specgen/docs/plans/2026-05-26-spec-as-code-unified-design.md` -> `docs/_archive/specgen/2026-05-26-spec-as-code-unified-design.md`

After moves, delete empty `docs/superpowers/` directory.

### Files deleted (commit 4)

| Path | Disposition |
|---|---|
| `NEXT_STEPS.md` (root) | git log is source of truth |
| `c_model/NEXT_STEPS.md` | same; duplicate name confusion |
| `c_model/README.md` | module not independently distributed |
| `cosim/README.md` | same |
| `cosim/CODING_DISCIPLINE.md` | content absorbed into docs/development.md + docs/architecture.md |
| `cosim/KNOWN_LIMITATIONS.md` | active content absorbed into docs/architecture.md §4 |

---

## Commit 1 - .gitignore housekeeping

### Task 1: Add .gitignore entries

**Files:**
- Modify: `.gitignore`

- [ ] **Step 1: Verify current .gitignore state**

Run: `cat .gitignore | head -30`
Expected: existing entries visible; note current bottom-of-file content.

Run: `git status --short | head -10`
Expected output shows untracked directories that should be ignored:
```
?? .codegraph/
?? Python/
?? specgen/Python/
?? dpi_ref/
?? issue/
?? docs/image/nmu.jpg
?? docs/image/nsu.jpg
?? docs/image/reorder_buffer.jpg
```

- [ ] **Step 2: Append new entries**

Append to `.gitignore` (preserve existing content; add at end with section header):

```
# Local Python installs (project-local 3.14, ~278 MB total)
/Python/
/specgen/Python/

# Codegraph MCP database
/.codegraph/

# User-local workspace (kept local-only by default; see
# docs/superpowers/specs/2026-06-07-doc-cleanup-and-rewrite-design.md sec 4)
/dpi_ref/
/issue/

# Image artifacts only referenced by archived material
/docs/image/*.jpg

# Python tooling caches
/.pytest_cache/
__pycache__/
*.py[cod]
```

- [ ] **Step 3: Verify working tree clean**

Run: `git status --short`
Expected: all previously-listed untracked dirs disappear from output;
only `.gitignore` shows as modified.

Run: `git check-ignore -v Python/ specgen/Python/ .codegraph/ dpi_ref/ issue/`
Expected: each line shows `.gitignore:NN:/<path>/  <dir>` confirming ignore rule.

- [ ] **Step 4: Commit**

```bash
git add .gitignore
git commit -m "$(cat <<'EOF'
build(gitignore): exclude local Python installs + codegraph DB + workspace dirs

Adds .gitignore entries for ~493 MB of untracked clutter:
- /Python/ + /specgen/Python/ (local 3.14 installs, ~278 MB)
- /.codegraph/ (codegraph MCP database, ~215 MB)
- /dpi_ref/ (user-local SV reference; kept local-only by default)
- /issue/ (user-local scratch notes)
- /docs/image/*.jpg (only referenced by archived material)
- /.pytest_cache/, __pycache__/, *.py[cod] (Python tooling caches)

Preparatory commit for doc cleanup round; clean working tree first.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 5: Verify commit**

Run: `git log --oneline -1`
Expected: shows new commit at HEAD.

Run: `git status --short`
Expected: clean working tree (or only pre-existing items the round does not touch).

---

## Commit 2 - Archive moves

### Task 2: Create docs/_archive directory tree

**Files:**
- Create dirs only (mkdir): `docs/_archive/superpowers/specs/`, `docs/_archive/superpowers/plans/`, `docs/_archive/spec_ni/`, `docs/_archive/specgen/`

- [ ] **Step 1: Create directory skeleton**

```bash
mkdir -p docs/_archive/superpowers/specs
mkdir -p docs/_archive/superpowers/plans
mkdir -p docs/_archive/spec_ni
mkdir -p docs/_archive/specgen
```

- [ ] **Step 2: Verify dirs exist**

Run: `ls -d docs/_archive/superpowers/specs docs/_archive/superpowers/plans docs/_archive/spec_ni docs/_archive/specgen`
Expected: all 4 paths print without errors.

### Task 3: Move superpowers specs (14 files)

**Files:**
- Move: `docs/superpowers/specs/*.md` -> `docs/_archive/superpowers/specs/`

- [ ] **Step 1: Inventory before move**

Run: `ls docs/superpowers/specs/*.md | wc -l`
Expected: `14` (verify count matches spec section 4).

- [ ] **Step 2: git mv each file**

Run from repo root:
```bash
for f in docs/superpowers/specs/*.md; do
    name=$(basename "$f")
    git mv "$f" "docs/_archive/superpowers/specs/$name"
done
```

- [ ] **Step 3: Verify**

Run: `ls docs/_archive/superpowers/specs/*.md | wc -l`
Expected: `14`.

Run: `ls docs/superpowers/specs/ 2>/dev/null | wc -l`
Expected: `0` (directory now empty).

Run: `git status --short | head -5`
Expected: `R` (rename) entries showing each file moved.

### Task 4: Move superpowers plans (12 files)

**Files:**
- Move: `docs/superpowers/plans/*.md` -> `docs/_archive/superpowers/plans/`

- [ ] **Step 1: Inventory before move**

Run: `ls docs/superpowers/plans/*.md | wc -l`
Expected: `12`.

- [ ] **Step 2: git mv each file**

```bash
for f in docs/superpowers/plans/*.md; do
    name=$(basename "$f")
    git mv "$f" "docs/_archive/superpowers/plans/$name"
done
```

- [ ] **Step 3: Verify**

Run: `ls docs/_archive/superpowers/plans/*.md | wc -l`
Expected: `12`.

Run: `ls docs/superpowers/plans/ 2>/dev/null | wc -l`
Expected: `0`.

### Task 5: Move noc_cmodel_rtl_plan + specgen plan

**Files:**
- Move: `docs/noc_cmodel_rtl_plan.md` -> `docs/_archive/noc_cmodel_rtl_plan.md`
- Move: `specgen/docs/plans/2026-05-26-spec-as-code-unified-design.md` -> `docs/_archive/specgen/2026-05-26-spec-as-code-unified-design.md`

- [ ] **Step 1: Move both files**

```bash
git mv docs/noc_cmodel_rtl_plan.md docs/_archive/noc_cmodel_rtl_plan.md
git mv specgen/docs/plans/2026-05-26-spec-as-code-unified-design.md \
       docs/_archive/specgen/2026-05-26-spec-as-code-unified-design.md
```

- [ ] **Step 2: Verify**

Run: `ls docs/_archive/noc_cmodel_rtl_plan.md docs/_archive/specgen/`
Expected: both files appear at destination.

Run: `ls docs/noc_cmodel_rtl_plan.md 2>&1`
Expected: error - file not found.

### Task 6: Move spec/ni working notes (6 files; lowercase-kebab rename)

**Files:**
- Move: 6 files from `spec/ni/` to `docs/_archive/spec_ni/` with case-style rename

- [ ] **Step 1: Move with rename**

```bash
git mv spec/ni/NEXT_SESSION_A6.md         docs/_archive/spec_ni/next-session-a6.md
git mv spec/ni/PRESENTATION_OUTLINE.md    docs/_archive/spec_ni/presentation-outline.md
git mv spec/ni/PRESENTATION_STYLE.md      docs/_archive/spec_ni/presentation-style.md
git mv spec/ni/SLIDES.md                  docs/_archive/spec_ni/slides.md
git mv spec/ni/IMPLEMENTER_REVIEW_LOG.md  docs/_archive/spec_ni/implementer-review-log.md
git mv spec/ni/READER_TEST_LOG.md         docs/_archive/spec_ni/reader-test-log.md
```

- [ ] **Step 2: Verify**

Run: `ls docs/_archive/spec_ni/`
Expected: 6 lowercase-kebab `.md` files.

Run: `ls spec/ni/*.md | grep -vE '(README|MODE)\.md'`
Expected: empty (only README.md and MODE.md remain at spec/ni/ root).

### Task 7: Write docs/_archive/README.md

**Files:**
- Create: `docs/_archive/README.md`

- [ ] **Step 1: Write the file**

Content (ASCII only; ~50 lines):

```markdown
# Archive

Historical specs, plans, working notes, and presentation drafts. Contents
here are **not authoritative**; they may conflict with current implementation
and current architecture documentation.

## Status

These documents are frozen at the date in their filename or in their
internal `Date:` header. They are kept for design-decision archaeology
(why a choice was made, what alternatives were considered) and for
reproducibility of completed brainstorm + plan cycles.

## When to read this

- Investigating why a specific design decision was made
- Tracing the history of a refactor or rename
- Reading completed plans whose implementation is now in `git log`

## When not to read this

- Looking for current architecture: read `../architecture.md`
- Looking for how to build, test, or contribute: read `../development.md`
- Looking for the project entry point: read `../../README.md`
- Looking for the normative NI spec: read `../../spec/ni/README.md`

## Layout

```
_archive/
  README.md                       (this file)
  noc_cmodel_rtl_plan.md          (stage roadmap; superseded by current
                                   docs/architecture.md)
  superpowers/
    specs/                        (14 completed brainstorm specs)
    plans/                        (12 completed implementation plans)
  spec_ni/                        (6 spec working notes: presentation
                                   drafts, session handoffs, review logs)
  specgen/
    2026-05-26-spec-as-code-unified-design.md  (specgen sub-project plan)
```

## Doc classes (cross-reference)

The project uses a lightweight 4-class decision model when classifying
documentation:

- **Normative spec**: authoritative; changes require sign-off
  (e.g. `spec/ni/doc/*.md`).
- **Maintained guide**: tracks code drift
  (e.g. `README.md`, `docs/architecture.md`, `docs/development.md`).
- **Generated reference**: tool emits; humans do not edit
  (e.g. `c_model/FEATURE_INVENTORY.md`).
- **Historical archive**: contents here.

The classes are guidance, not per-file labels. Status is established by
directory location and per-file banners.
```

- [ ] **Step 2: Verify ASCII compliance**

Run: `py -3 -c "
data = open('docs/_archive/README.md', 'rb').read()
bad = [(i, b) for i, b in enumerate(data) if b > 0x7F]
print('OK' if not bad else f'FAIL: {len(bad)} non-ASCII bytes at offsets {bad[:5]}')
"`
Expected: `OK`.

### Task 8: Remove empty docs/superpowers directory

**Files:**
- Delete: `docs/superpowers/` (empty after tasks 3-4)

- [ ] **Step 1: Verify empty**

Run: `ls docs/superpowers/ 2>&1`
Expected: empty listing or error (depends on shell).

Run: `find docs/superpowers/ -type f 2>/dev/null`
Expected: no files.

- [ ] **Step 2: Remove directory**

```bash
rmdir docs/superpowers/specs docs/superpowers/plans docs/superpowers
```

- [ ] **Step 3: Verify gone**

Run: `ls docs/superpowers 2>&1`
Expected: error - no such file or directory.

### Task 9: Commit 2

- [ ] **Step 1: Verify staged content**

Run: `git status --short | head -40`
Expected: 34 R-renames + 1 new file (docs/_archive/README.md) + directory deletion is implicit (no git tracking of empty dirs).

Run: `git diff --cached --stat | tail -5`
Expected: summary shows ~35 files changed.

- [ ] **Step 2: Stage new README + commit**

```bash
git add docs/_archive/README.md

git commit -m "$(cat <<'EOF'
docs(archive): move completed brainstorm + plan + spec working notes

Moves 34 historical .md files to docs/_archive/ to clear surface
clutter for OSS reader audience:
- 14 docs/superpowers/specs/* (completed brainstorm specs)
- 12 docs/superpowers/plans/* (completed implementation plans)
- docs/noc_cmodel_rtl_plan.md (stage roadmap; superseded)
- 6 spec/ni/{NEXT_SESSION_A6,PRESENTATION_OUTLINE,PRESENTATION_STYLE,SLIDES,IMPLEMENTER_REVIEW_LOG,READER_TEST_LOG}.md
- specgen/docs/plans/2026-05-26-spec-as-code-unified-design.md

spec/ni working notes use lowercase-kebab-case at destination.

Adds docs/_archive/README.md noting archived material is non-authoritative
and may conflict with current implementation.

Empty docs/superpowers/ removed.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 3: Verify**

Run: `git log --oneline -1`
Expected: shows commit.

Run: `git log --diff-filter=R --stat HEAD~1..HEAD | wc -l`
Expected: confirms rename activity.

---

## Commit 3 - New docs + ASCII lint + link maintenance

### Task 10: Write ASCII byte check tool

**Files:**
- Create: `tools/lint_docs.py`

- [ ] **Step 1: Write the failing test**

Create file `tools/test_lint_docs.py` (will be deleted at end of task; this is a quick verification, not committed):

```python
# Quick verification script, not committed.
import subprocess
import tempfile
import os

# Test 1: clean ASCII file passes
with tempfile.NamedTemporaryFile(mode='w', suffix='.md', delete=False) as f:
    f.write("# Hello\nPure ASCII content.\n")
    clean = f.name

result = subprocess.run(
    ["py", "-3", "tools/lint_docs.py", clean],
    capture_output=True, text=True,
)
assert result.returncode == 0, f"clean file should pass: {result.stderr}"

# Test 2: non-ASCII file fails
with tempfile.NamedTemporaryFile(mode='w', suffix='.md',
                                  delete=False, encoding='utf-8') as f:
    f.write("# Hello\nContent with em dash em dash works.\n")
    f.write("But this arrow does not: →\n")
    dirty = f.name

result = subprocess.run(
    ["py", "-3", "tools/lint_docs.py", dirty],
    capture_output=True, text=True,
)
assert result.returncode != 0, "dirty file should fail"
assert "non-ASCII" in result.stderr or "non-ASCII" in result.stdout

os.unlink(clean)
os.unlink(dirty)
print("PASS")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `py -3 tools/test_lint_docs.py`
Expected: FAIL - `tools/lint_docs.py` does not exist yet.

- [ ] **Step 3: Implement lint_docs.py**

Create `tools/lint_docs.py`:

```python
#!/usr/bin/env python3
"""Mandatory ASCII byte check for maintained docs (spec sec 3.2).

Reads each file path argument as raw bytes; reports any byte > 0x7F.
Exits 0 if clean across all inputs, 1 if any non-ASCII byte found.

Usage:
    py -3 tools/lint_docs.py path1.md path2.md ...

Designed for use in make check on a curated set of maintained docs
(NOT archive, NOT normative spec, NOT sub-project guides, NOT
ATTRIBUTION.md or FEATURE_INVENTORY.md).
"""
import sys


def check_file(path: str) -> int:
    """Return count of non-ASCII bytes found in path."""
    with open(path, "rb") as f:
        data = f.read()
    bad = [(i, b) for i, b in enumerate(data) if b > 0x7F]
    if not bad:
        return 0
    # Compute line numbers for the first few bad bytes.
    sample = []
    for offset, byte in bad[:5]:
        # Count newlines before offset for line number.
        line = data[:offset].count(b"\n") + 1
        sample.append(f"  offset {offset} line {line} byte 0x{byte:02X}")
    print(
        f"LINT {path}: {len(bad)} non-ASCII byte(s) found\n"
        + "\n".join(sample),
        file=sys.stderr,
    )
    return len(bad)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: lint_docs.py FILE [FILE ...]", file=sys.stderr)
        return 2
    total = 0
    for path in sys.argv[1:]:
        total += check_file(path)
    if total == 0:
        print(f"lint_docs: {len(sys.argv) - 1} file(s) OK")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run test to verify it passes**

Run: `py -3 tools/test_lint_docs.py`
Expected: prints `PASS`.

- [ ] **Step 5: Cleanup verification script**

```bash
rm tools/test_lint_docs.py
```

Run: `ls tools/`
Expected: `lint_docs.py`, `lint_scenarios.py` (no `test_lint_docs.py`).

### Task 11: Wire lint_docs into make check

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Locate `check:` and `lint_scenarios:` targets**

Run: `grep -nE "^(check|lint_)" Makefile`
Expected output similar to:
```
check: lint_scenarios build-cmodel
lint_scenarios:
```

- [ ] **Step 2: Add lint_docs target + thread into check**

Edit `Makefile`. Find the `lint_scenarios:` recipe and add a parallel
`lint_docs:` target right after it. Then update `check:` to depend on
both linters before `build-cmodel`.

Add after the `lint_scenarios:` recipe block:

```makefile
# Mandatory ASCII byte check on maintained docs (spec sec 3.2).
# Excludes archive, normative spec, sub-project docs, legal, generated.
MAINTAINED_DOCS = \
    README.md \
    docs/architecture.md \
    docs/development.md \
    docs/_archive/README.md \
    sim/test_patterns/README.md \
    spec/ni/README.md

lint_docs:
	py -3 tools/lint_docs.py $(MAINTAINED_DOCS)
```

Update `check:` recipe line to include `lint_docs`:

Change:
```
check: lint_scenarios build-cmodel
```
to:
```
check: lint_scenarios lint_docs build-cmodel
```

Add `lint_docs` to the `.PHONY` declaration line (search for `.PHONY` near top of file; append `lint_docs` to that list).

- [ ] **Step 3: Verify make check still passes (before docs exist)**

Run: `make lint_docs 2>&1 | head -10`
Expected: lint will fail because the maintained docs don't exist yet
(README.md, docs/architecture.md, docs/development.md still missing).
This is expected at this point in the task ordering. Continue to next
task; the actual `make check` run is verified after tasks 12-15.

### Task 12: Write root README.md

**Files:**
- Create: `README.md`

- [ ] **Step 1: Write README.md (target ~110 lines)**

Per spec sec 5 outline. ASCII only. Sentence-case headings.

```markdown
# noc_project - AXI4 NoC c_model + cosim

A behavioural C++ model and Verilator co-sim of an AXI4 Network-on-Chip
Interface (NMU + NSU). The c_model passes IHI 0022H AXI4 conformity
scenarios; the cosim verifies a subset through wb2axip protocol checkers
under Verilator.

## Status

Research / alpha. Stage 5b in progress; behavioural c_model passing
<N>/<N> tests (verify with `make test`); Verilator cosim subset passes
plus skips for wb2axip structural limits (see `docs/architecture.md`).

## Architecture

```
AXI Master --> NMU --> [NoC fabric] --> NSU --> AXI Slave
              behavioural c_model in C++17
              Verilator cosim with wb2axip protocol check
```

### Where code lives

- `c_model/` - C++17 behavioural model + GoogleTest
- `cosim/` - Verilator wire-level cosim + wb2axip checker
- `sim/test_patterns/` - AXI4 scenario tree (AX4-CAT-NNN_slug)
- `spec/ni/` - normative NI specification
- `specgen/` - spec-to-header codegen sub-project
- `tools/` - repo-level tooling
- `docs/` - architecture + development guide

## Prerequisites

- CMake 3.12 or newer
- Verilator 5.036
- Python 3.13 or newer (with PyYAML)
- MSYS2 mingw64 toolchain (Windows host)

## Build

```bash
git clone <url> && cd noc_project
make build       # c_model + Verilator (correct dep order)
```

## Test

```bash
make test                                    # c_model gtest suite
make check                                   # lint + build + tests
make sim                                     # default scenario via cosim
make sim SCENARIO=AX4-BUR-002_incr_8beat     # specific scenario
```

Multi-beat and multi-outstanding scenarios are SKIPped by the cosim
integration test (reason codes WB2AXIP_MULTI_BEAT etc.) and will fire
faxi_slave.v assertions if run through `make sim` directly. See
`docs/architecture.md` for wb2axip structural limits.

## Documentation

- [Architecture overview](docs/architecture.md)
- [Development guide](docs/development.md)
- [NI specification](spec/ni/README.md)
- [specgen sub-project](specgen/docs/guide/index.md)
- [Historical archive](docs/_archive/README.md)

## Contributing

Branches target `main` via PR. Required before merging:

- `make check` clean (lint + build + tests)
- `clang-format -i` on every C++ file touched
- Commit message format: `type(scope): description` (English)
- Never `--no-verify`

Detailed conventions and workflow: `docs/development.md`.

## License and third-party

No project-wide license has been selected. Until one is added,
project-owned material is not offered under an open-source license.

Vendored / derived material has its own license:

- `c_model/include/axi/` ported from cocotbext-axi (MIT); see
  `c_model/include/axi/ATTRIBUTION.md`
- `cosim/sv/wb2axip/` is ZipCPU/wb2axip (Apache 2.0), used verbatim;
  see `cosim/sv/wb2axip/ATTRIBUTION.md`
```

- [ ] **Step 2: Verify ASCII compliance**

Run: `py -3 tools/lint_docs.py README.md`
Expected: `lint_docs: 1 file(s) OK`.

- [ ] **Step 3: Verify length**

Run: `wc -l README.md`
Expected: line count around 100-115.

### Task 13: Write docs/architecture.md

**Files:**
- Create: `docs/architecture.md`

- [ ] **Step 1: Write the file**

Per spec sec 6 outline. Target ~360 content lines + TOC. ASCII only.
Sentence-case headings. Six sections + references. The file is too long
to inline here verbatim; follow the outline below and the spec sec 6
content guidance, copy from the spec's quoted prose where verbatim
content is given, and fill the rest with concrete content from the
project (referring to actual files in c_model/, cosim/, sim/test_patterns/).

Structure (write each section in this order):

```markdown
# Architecture

## Contents

- [1. System context](#1-system-context)
- [2. NI components - NMU, NSU, router](#2-ni-components---nmu-nsu-router)
- [3. c_model component flow and tick discipline](#3-c_model-component-flow-and-tick-discipline)
- [4. Cosim and Verilator boundary](#4-cosim-and-verilator-boundary)
- [5. Verification layers](#5-verification-layers)
- [6. AXI4 conformity scope](#6-axi4-conformity-scope)
- [References](#references)

## 1. System context

(~30 lines per spec sec 6 outline)
- Project purpose, ASCII overview diagram, boundary definitions,
  c_model / RTL duality, pointer to spec/ni/README.md as normative.

## 2. NI components - NMU, NSU, router

(~60 lines)
- NMU role + RoB + QoS + address remapping (cite AMD PG313/PG406)
- NSU asymmetry (no RoB)
- Uniform router; XY routing
- AXI endpoints cited as cocotbext-axi derivatives, link to
  c_model/include/axi/ATTRIBUTION.md
- Per-component source path links (c_model/include/nmu/*, c_model/include/nsu/*, c_model/include/noc/*)

## 3. c_model component flow and tick discipline

(~90 lines, three H3 subsections per spec amend)

### 3.1 Component map
- Pipeline: AxiMaster -> AxiSlavePort -> Nmu -> LoopbackNoc -> Nsu -> AxiMasterPort -> AxiSlave + Memory
- Hermetic singleton; data flow only via IO structs (absorbs CODING_DISCIPLINE.md hermetic invariants verbatim)

### 3.2 Tick semantics
- beta tick discipline (1 tick per cycle; inputs first, then outputs)
- 1-cycle latency per hop; fully-registered handshake
- Pipeline boundaries explicit; no combinational delays
- Matches Verilator clk_i registration
- C++ vs SV timing-master nuance (absorbs KNOWN_LIMITATIONS.md content)

### 3.3 Extension boundaries
- Dual flow control (Valid/Ready and Credit-based; compile-time selected)
- Factory pattern; RTL parameter analogy
- Hot-swap Router_Interface / NI_Interface abstract base classes

## 4. Cosim and Verilator boundary

(~80 lines)
- Stage 5b wire-wrap: each c_model component lives in an SV shell module
- DPI 3-step pattern per shell (set_inputs -> tick -> get_outputs)
- 5 shells: AxiMaster, Nmu, LoopbackNoc, Nsu, AxiSlave
- Shells only convert wires / methods (absorbs CODING_DISCIPLINE.md
  shell-responsibility invariant verbatim)
- wb2axip protocol checkers bind on two AXI bundles
- Vtb_top binary; runtime plusarg scenario load
- **wb2axip slave structural limits** (active list, absorbs
  cosim/KNOWN_LIMITATIONS.md):
  - Single-beat write only (AWLEN = 0, faxi_slave.v:805-807)
  - Single outstanding write (wr_pending less than or equal to 1)
  - AW/W strict in-order (faxi_slave.v:561)
  - No exclusive monitor; no DECERR address-map check
  - Permissive faxi_wstrb.v stub (cosim/sv/wb2axip/faxi_wstrb.v)
- wb2axip is a formal-verification slave; structural limits are temporary
- cosim/tests/wb2axip_block.hpp - runtime predicate that maps scenario
  content to a SKIP reason

## 5. Verification layers

(~60 lines)
- Run-all tests: c_model/tests/axi/test_integration.cpp and
  cosim/tests/test_cosim_integration.cpp; both consume kAllAxi4Scenarios
  (CMake glob over sim/test_patterns/AX4-*/scenario.yaml)
- Cosim runtime SKIP path with WB2AXIP_* / INF_DEDICATED_TEST reasons
- Scoped tests: port_pair_loopback, request_response_loopback,
  checker_fires_on_violation; use RequireKnownScenario to abort on
  stale IDs
- Scenario tree convention AX4-CAT-NNN_slug; full taxonomy at
  sim/test_patterns/README.md

## 6. AXI4 conformity scope

(~30 lines)
- Covered IHI 0022H sections
- Deliberately excluded features
- Future phases (full sideband, multi-master, BFM replacement of wb2axip)

## References

(~10 lines)
- ../spec/ni/README.md
- ../spec/ni/doc/protocol_rules.md
- ../spec/ni/doc/packet_format.md
- ../sim/test_patterns/README.md
- ../c_model/FEATURE_INVENTORY.md
- ../c_model/include/axi/ATTRIBUTION.md
- ../cosim/sv/wb2axip/ATTRIBUTION.md
```

When fleshing out each section, draw concrete content from these absorbed
sources (per the disposition matrix in spec sec 8):

- `cosim/KNOWN_LIMITATIONS.md` headings -> section 4 (multi-beat, faxi_wstrb,
  dump accumulation if present) and section 3.2 (C++ vs SV timing nuance)
- `cosim/CODING_DISCIPLINE.md` headings -> section 3.1 (hermetic-singleton)
  and section 4 (shells only convert wires/methods)
- Intentionally dropped from CODING_DISCIPLINE: private AI workflow /
  Skill invocations (Codex feedback: do not republish private workflow as
  project requirement)

- [ ] **Step 2: Verify ASCII compliance**

Run: `py -3 tools/lint_docs.py docs/architecture.md`
Expected: `lint_docs: 1 file(s) OK`.

- [ ] **Step 3: Verify length budget**

Run: `wc -l docs/architecture.md`
Expected: 320-400 lines.

If over 400, do not split here. Note as concern and continue. The future
split hook (spec sec 9) provides guidance but is not triggered automatically.

### Task 14: Write docs/development.md

**Files:**
- Create: `docs/development.md`

- [ ] **Step 1: Write the file**

Per spec sec 7 outline. Target ~390-430 lines including TOC. ASCII only.
Eight sections + references.

Structure:

```markdown
# Development guide

## Contents

- [1. Workflow](#1-workflow)
- [2. Repository conventions](#2-repository-conventions)
- [3. Build system](#3-build-system)
- [4. Generated files and specgen](#4-generated-files-and-specgen)
- [5. Adding a scenario](#5-adding-a-scenario)
- [6. Targeted tests](#6-targeted-tests)
- [7. Debugging cosim](#7-debugging-cosim)
- [8. Pre-submit checklist](#8-pre-submit-checklist)
- [References](#references)

## 1. Workflow

(~30 lines)
- Branch / PR target
- Commit message format: type(scope): description (English); types
  feat / fix / docs / style / refactor / test / chore / perf
- Every commit: compiles + passes existing tests + includes tests for
  new functionality
- Pre-submit: `make check` clean
- Never --no-verify
- Prefer new commits over --amend
- Note: `CLAUDE.md` is internal AI assistant guidance, not contributor
  onboarding; this document is the canonical contributor workflow

## 2. Repository conventions

(~50 lines)
- Naming: snake_case for vars/methods, PascalCase for types, no
  abbreviation (write Arbiter, not Arb)
- C++17; clang-format -i after every .hpp/.cpp/.h edit;
  .clang-format at repo root (Google base + IndentWidth 4 +
  ContinuationIndentWidth 4 + ColumnLimit 100)
- YAML scenarios: schema_version + metadata.{name,category} + config +
  transactions (see ../sim/test_patterns/README.md)
- Markdown: lowercase-kebab-case filenames; ASCII only (no Unicode
  arrows, em-dash, beta, section sign, greater-than-or-equal); per-class
  structure guidance per docs/_archive/README.md
- Character encoding: UTF-8 without BOM
- Path separators: forward slash `/` in code
- Windows specifics: use `py -3` not `python3`
- Hermetic singleton invariants for c_model components
  (absorbs cosim/CODING_DISCIPLINE.md fully; do not republish private
  workflow / skill invocations)

## 3. Build system

(~40 lines)
- Top-level Makefile is user-facing entry point; `make help`
- Recursive $(MAKE) -C; subdir build responsibility split
- c_model/build/CMakeCache.txt cache; first build triggers
  `cmake -S c_model -B c_model/build`, subsequent `make build-cmodel`
  is `cmake --build` only
- When to `make clean-cmodel`: CMakeLists.txt changes / stale state
- CMake auto-reconfigure triggers (CONFIGURE_DEPENDS over AX4-*/scenario.yaml)

## 4. Generated files and specgen

(~50 lines)
- Generated artifacts: c_model/include/ni_spec.hpp,
  specgen/generated/cpp/*.h, specgen/generated/sv/*.sv
- c_model/FEATURE_INVENTORY.md auto-generated by gen_inventory.py
  (drift gate enforced)
- DO NOT manually edit generated files
- Regenerate: `py -3 specgen/tools/codegen.py --target <cpp|sv> --domain <packet|signals|registers>`
- Drift check: `py -3 specgen/tools/codegen.py --check`
- specgen sub-project guide: ../specgen/docs/guide/index.md

## 5. Adding a scenario

(~50 lines)
- Pick CAT + next NNN within category (BAS/BUR/BND/ORD/EXC/RSP/STR/HSH/INF)
- mkdir sim/test_patterns/AX4-CAT-NNN_slug/; write scenario.yaml + data.txt
- Cite IHI 0022H section or VIP test name in YAML header comment
- `make check` picks it up automatically via kAllAxi4Scenarios glob
- Cosim side may SKIP with WB2AXIP_* reason - expected for wb2axip-blocked
  patterns
- Commit per workflow conventions

## 6. Targeted tests

(~40 lines)
- Run single suite: `ctest --test-dir c_model/build -R <pattern> -V`
- Common patterns: `-R IntegrationP`, `-R CosimIntegration`, `-R PortPair`
- Adding new test files: register in subdir CMakeLists.txt mirroring
  existing entries

## 7. Debugging cosim

(~50 lines)
- Quick sanity: `make sim` (default scenario passes wb2axip)
- When `make sim SCENARIO=<id>` fires faxi_slave.v:807 assertion:
  scenario hits a wb2axip structural limit. Check the predicate first:
  scenarios that ctest CosimIntegration SKIPs (WB2AXIP_MULTI_BEAT etc.)
  will also fail `make sim`. Use ctest path for normal debugging.
- Vtb_top output: cosim/output/<scenario>/run.log
- Multi-beat coverage path: `NmuShellAdapter.multi_beat_w_burst_visible_per_cycle`
  c_model adapter unit test (until wb2axip is replaced)
- DPI fatal propagation test: cosim/tests/test_checker_fires_on_violation
  with AX4-INF-001_dpi_fatal_on_init_failure

## 8. Pre-submit checklist

(~25 lines)
- [ ] `make check` clean (lint_scenarios + lint_docs + build + tests)
- [ ] clang-format -i on every C++ file touched
- [ ] commit msg format: `type(scope): description` (English)
- [ ] new test for new functionality
- [ ] no --no-verify
- [ ] new scenarios cite IHI 0022H section or VIP test name in YAML
  header
- [ ] for RTL: rtl-style + rtl-reviewer (see CLAUDE.md; internal tooling)

## References

(~10 lines)
- ../README.md
- ./architecture.md
- ../spec/ni/README.md
- ../sim/test_patterns/README.md
- ../specgen/docs/guide/index.md
- ../CLAUDE.md (internal AI tooling; non-normative)
```

- [ ] **Step 2: Verify ASCII compliance**

Run: `py -3 tools/lint_docs.py docs/development.md`
Expected: `lint_docs: 1 file(s) OK`.

- [ ] **Step 3: Verify length**

Run: `wc -l docs/development.md`
Expected: 380-430 lines.

### Task 15: Rewrite spec/ni/README.md (style only)

**Files:**
- Modify: `spec/ni/README.md`

- [ ] **Step 1: Read current content**

Run: `wc -l spec/ni/README.md`
Note current line count.

Run: `head -40 spec/ni/README.md`
Read the current structure.

- [ ] **Step 2: Apply style adjustments only**

Allowed changes:
- Heading case: convert Title Case to sentence case
  (e.g. `## Network Manager Unit Features` -> `## Network Manager Unit features`)
- ASCII substitutions: replace any non-ASCII chars
  (em-dash `--` for ` -- ` or ` - `; `->` for arrows; `>=` for `greater-than-or-equal`)
- Filename link corrections: any link to `../noc_cmodel_rtl_plan.md`
  should be retargeted to `../docs/_archive/noc_cmodel_rtl_plan.md`
- Filename link corrections: any link to a moved spec/ni file
  (e.g. `./SLIDES.md`) should be retargeted to
  `../docs/_archive/spec_ni/slides.md` (lowercase-kebab)

Disallowed changes:
- New content
- Section reordering
- Removal of any normative content
- Rewording for clarity beyond style

After edit, run: `py -3 tools/lint_docs.py spec/ni/README.md`
Expected: `lint_docs: 1 file(s) OK`.

- [ ] **Step 3: Verify scope**

Run: `git diff spec/ni/README.md | head -40`
Inspect: changes are confined to heading case, ASCII substitutions,
and link path corrections. No semantic content changes.

### Task 16: Fix CLAUDE.md links

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Locate stale links**

Run: `grep -nE "docs/superpowers|docs/noc_cmodel|2026-05-31" CLAUDE.md`
Expected output:
```
25:- `docs/noc_cmodel_rtl_plan.md` - **main plan**: NoC c_model + RTL integration roadmap (Stage 2/3/4/5)
26:- `docs/superpowers/specs/2026-05-31-pure-axi-subsystem-phase-b-design.md` - Phase B reference
27:- `docs/superpowers/specs/2026-05-31-pure-axi-subsystem-phase-c-design.md` - Phase C reference
```

- [ ] **Step 2: Update links**

In `CLAUDE.md`, find the lines around 25-27 (Key Design Docs section):

Change:
```
- `docs/noc_cmodel_rtl_plan.md` - **main plan**: ...
- `docs/superpowers/specs/2026-05-31-pure-axi-subsystem-phase-b-design.md` - Phase B reference
- `docs/superpowers/specs/2026-05-31-pure-axi-subsystem-phase-c-design.md` - Phase C reference
```

To:
```
- `docs/_archive/noc_cmodel_rtl_plan.md` - stage roadmap (archived 2026-06-07; superseded by `docs/architecture.md`)
```

Drop the 2 phase-b / phase-c lines entirely (they reference files that
do not exist in the repo today).

- [ ] **Step 3: Verify**

Run: `grep -nE "docs/superpowers|docs/noc_cmodel|2026-05-31" CLAUDE.md`
Expected: empty (no matches).

Run: `grep -n "docs/_archive/noc_cmodel" CLAUDE.md`
Expected: 1 line matching the new path.

### Task 17: Fix cosim/sv/wb2axip/ATTRIBUTION.md link

**Files:**
- Modify: `cosim/sv/wb2axip/ATTRIBUTION.md`

- [ ] **Step 1: Locate stale link**

Run: `grep -n "KNOWN_LIMITATIONS" cosim/sv/wb2axip/ATTRIBUTION.md`
Expected: at least one line referencing the about-to-be-deleted file.

- [ ] **Step 2: Update link**

Replace the reference to `cosim/KNOWN_LIMITATIONS.md` or `../KNOWN_LIMITATIONS.md`
with `../../../docs/architecture.md` (anchor: `#4-cosim-and-verilator-boundary`).

If the original text was something like:
```
See cosim/KNOWN_LIMITATIONS.md for active limitations.
```
Change to:
```
See ../../../docs/architecture.md for active wb2axip limitations
(see "Cosim and Verilator boundary" section).
```

- [ ] **Step 3: Verify**

Run: `grep -n "KNOWN_LIMITATIONS" cosim/sv/wb2axip/ATTRIBUTION.md`
Expected: empty.

Run: `grep -n "docs/architecture.md" cosim/sv/wb2axip/ATTRIBUTION.md`
Expected: 1 line.

### Task 18: Run make check end-to-end

- [ ] **Step 1: Run lint_docs only**

Run: `py -3 tools/lint_docs.py README.md docs/architecture.md docs/development.md docs/_archive/README.md sim/test_patterns/README.md spec/ni/README.md`
Expected: `lint_docs: 6 file(s) OK`.

If any non-ASCII chars surface in sim/test_patterns/README.md (preexisting from
AXI standardization round), they must be fixed in this task before commit.
Apply same ASCII substitutions and re-verify.

- [ ] **Step 2: Run make check fully**

Run: `make check`
Expected: lint_scenarios passes (36 scenario dirs OK), lint_docs passes
(6 files OK), c_model build succeeds, ctest passes.

If make check fails on a pre-existing issue unrelated to this commit's
work (e.g. specgen environment, MSYS2 cmd.exe noise), document and
proceed; the failure mode should not be caused by our changes.

### Task 19: Build disposition matrix verification

This is a **review-only** task before commit 3. No file changes.

- [ ] **Step 1: Re-read spec sec 8 disposition matrix**

Open `docs/_archive/superpowers/specs/2026-06-07-doc-cleanup-and-rewrite-design.md`
(now at archive location after commit 2). Read sec 8 disposition matrix table.

- [ ] **Step 2: Verify each source-file heading has a target**

For `cosim/KNOWN_LIMITATIONS.md`:

Run: `grep -E "^##? " cosim/KNOWN_LIMITATIONS.md`

For each heading found, confirm by reading `docs/architecture.md` that
the content (or its equivalent durable summary) appears in section 4
or section 3.2 per disposition matrix. If a heading is missing from the
matrix or its target section, either update `docs/architecture.md` to
include it, or extend the disposition matrix in a separate commit
(spec amendment).

For `cosim/CODING_DISCIPLINE.md`:

Run: `grep -E "^##? " cosim/CODING_DISCIPLINE.md`

Verify each heading either appears in `docs/development.md` sec 2 or
`docs/architecture.md` sec 4, or is intentionally dropped per spec
sec 8 (private AI workflow).

- [ ] **Step 3: Document the result**

Add a note in the upcoming commit 3 commit message (Task 20) listing
the disposition verification outcome:

```
Disposition matrix verified:
- cosim/KNOWN_LIMITATIONS.md: N headings, all absorbed or intentionally
  dropped per spec sec 8
- cosim/CODING_DISCIPLINE.md: M headings, all absorbed or intentionally
  dropped per spec sec 8
```

### Task 20: Commit 3

- [ ] **Step 1: Verify staged content**

Run: `git status --short`
Expected files modified or new:
```
A  README.md
A  docs/architecture.md
A  docs/development.md
A  tools/lint_docs.py
M  Makefile
M  CLAUDE.md
M  cosim/sv/wb2axip/ATTRIBUTION.md
M  spec/ni/README.md
```

Run: `git diff --cached --stat`
Expected: ~8 files changed.

- [ ] **Step 2: Stage everything**

```bash
git add README.md docs/architecture.md docs/development.md \
        tools/lint_docs.py Makefile CLAUDE.md \
        cosim/sv/wb2axip/ATTRIBUTION.md spec/ni/README.md
```

- [ ] **Step 3: Commit**

```bash
git commit -m "$(cat <<'EOF'
docs: add root README + architecture guide + development guide + link fixes

New maintained docs per spec sec 5-7:
- README.md (root): tagline, status, architecture diagram, where code
  lives, prereqs, build/test, docs links, contributing, license.
- docs/architecture.md: 6 sections covering system context, NI components,
  c_model component flow + tick discipline, cosim + Verilator boundary
  (absorbing cosim/KNOWN_LIMITATIONS.md active content), verification
  layers, AXI4 conformity scope.
- docs/development.md: 8 sections covering workflow, conventions
  (absorbing cosim/CODING_DISCIPLINE.md hermetic-singleton invariants),
  build system, generated files + specgen, adding scenarios, targeted
  tests, debugging cosim, pre-submit checklist.

Tooling:
- tools/lint_docs.py: mandatory ASCII byte check (any byte >0x7F fails).
- Makefile: lint_docs target wired into check; runs over MAINTAINED_DOCS
  list.

Link maintenance:
- CLAUDE.md: retarget docs/noc_cmodel_rtl_plan.md to
  docs/_archive/noc_cmodel_rtl_plan.md; drop 2 stale 2026-05-31 spec
  references (those files do not exist in the repo today).
- cosim/sv/wb2axip/ATTRIBUTION.md: retarget cosim/KNOWN_LIMITATIONS.md
  reference to docs/architecture.md anchor.

spec/ni/README.md: style adjustments only (sentence-case headings,
ASCII substitutions, link path corrections to archive); no content
change.

Disposition matrix verified for commit 4 deletions:
- cosim/KNOWN_LIMITATIONS.md headings all absorbed into
  docs/architecture.md sec 4 (wb2axip structural limits) and sec 3.2
  (C++ vs SV timing), or intentionally dropped (resolved section 3).
- cosim/CODING_DISCIPLINE.md headings all absorbed into
  docs/development.md sec 2 (hermetic singleton) and docs/architecture.md
  sec 4 (shell responsibility), or intentionally dropped (private AI
  workflow per Codex feedback).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 4: Verify commit**

Run: `git log --oneline -1`
Expected: shows commit at HEAD.

Run: `make check`
Expected: lint + build + tests pass end-to-end.

---

## Review gate (between commit 3 and commit 4)

**This is a verification gate, not a commit.** Commit 4 is prohibited
until the gate passes.

### Task 21: Review gate verification

- [ ] **Step 1: Disposition matrix sanity**

Run: `grep -E "^##? " cosim/KNOWN_LIMITATIONS.md | wc -l`
Run: `grep -E "^##? " cosim/CODING_DISCIPLINE.md | wc -l`

For each heading count, verify that an equivalent number of dispositions
appears in the commit 3 commit message (Task 20 step 3).

- [ ] **Step 2: Local link validation**

Run a crude link check across maintained docs:
```bash
for f in README.md docs/architecture.md docs/development.md \
         docs/_archive/README.md spec/ni/README.md CLAUDE.md \
         c_model/include/axi/ATTRIBUTION.md \
         cosim/sv/wb2axip/ATTRIBUTION.md; do
    echo "=== $f ==="
    grep -oE '\[[^]]+\]\([^)]+\)' "$f" 2>/dev/null | head -20
done
```

For each `(path)` link found, verify the target file exists. Specifically
confirm:
- No link references `docs/superpowers/specs/...` (all moved)
- No link references `docs/noc_cmodel_rtl_plan.md` directly (now in archive)
- No link references `cosim/KNOWN_LIMITATIONS.md` (about to be deleted)
- No link references `cosim/CODING_DISCIPLINE.md` (about to be deleted)
- No link references `c_model/README.md` or `cosim/README.md` (about to
  be deleted)
- No link references `NEXT_STEPS.md` (about to be deleted)

If any dangling links are found that point to files about to be deleted
in commit 4, **add a fix commit between commit 3 and commit 4**
retargeting those links before proceeding.

- [ ] **Step 3: ASCII byte check (full re-run)**

Run: `py -3 tools/lint_docs.py README.md docs/architecture.md docs/development.md docs/_archive/README.md sim/test_patterns/README.md spec/ni/README.md`
Expected: `lint_docs: 6 file(s) OK`.

- [ ] **Step 4: Decision**

If steps 1-3 all pass:
- Proceed to Task 22 (commit 4).

If any step fails:
- Either fix in a separate commit and re-run steps 1-3, or roll back
  commit 3 and revisit Task 13/14/15.

---

## Commit 4 - Delete obsolete docs

### Task 22: Delete obsolete files

**Files:**
- Delete: `NEXT_STEPS.md`, `c_model/NEXT_STEPS.md`, `c_model/README.md`,
  `cosim/README.md`, `cosim/CODING_DISCIPLINE.md`, `cosim/KNOWN_LIMITATIONS.md`

- [ ] **Step 1: Final dangling-link check on files about to be deleted**

For each file to be deleted, search the repo for any remaining references
that would become dangling:

```bash
for f in NEXT_STEPS.md c_model/NEXT_STEPS.md c_model/README.md \
         cosim/README.md cosim/CODING_DISCIPLINE.md \
         cosim/KNOWN_LIMITATIONS.md; do
    name=$(basename "$f")
    echo "=== references to $f ==="
    grep -rn "$name" README.md docs/ spec/ni/README.md CLAUDE.md \
        c_model/include/axi/ATTRIBUTION.md \
        cosim/sv/wb2axip/ATTRIBUTION.md 2>/dev/null | head -5
done
```

Expected: no references in maintained docs (matches expected per Task 21).
If any remain, fix before continuing.

- [ ] **Step 2: Delete files**

```bash
git rm NEXT_STEPS.md
git rm c_model/NEXT_STEPS.md
git rm c_model/README.md
git rm cosim/README.md
git rm cosim/CODING_DISCIPLINE.md
git rm cosim/KNOWN_LIMITATIONS.md
```

- [ ] **Step 3: Verify**

Run: `git status --short`
Expected: 6 D (deletion) entries.

Run: `ls NEXT_STEPS.md c_model/NEXT_STEPS.md c_model/README.md cosim/README.md cosim/CODING_DISCIPLINE.md cosim/KNOWN_LIMITATIONS.md 2>&1 | tail -10`
Expected: 6 "No such file" errors.

- [ ] **Step 4: Run make check**

Run: `make check`
Expected: lint_scenarios passes, lint_docs passes (6 maintained docs
still OK), c_model build passes, ctest passes. No regressions.

- [ ] **Step 5: Commit**

```bash
git commit -m "$(cat <<'EOF'
docs: remove obsolete working docs + module READMEs

Deletes 6 files whose durable content has been absorbed into
docs/architecture.md and docs/development.md (commit 3) per spec
sec 8 disposition matrix:

- NEXT_STEPS.md (root) - working status note; git log is source of truth
- c_model/NEXT_STEPS.md - same; duplicate name confusion
- c_model/README.md - module not independently distributed; covered by
  root README + docs/architecture.md sec 2-3
- cosim/README.md - same; covered by docs/architecture.md sec 4
- cosim/CODING_DISCIPLINE.md - hermetic-singleton invariants in
  docs/development.md sec 2; shell responsibility in docs/architecture.md
  sec 4; private AI workflow intentionally dropped
- cosim/KNOWN_LIMITATIONS.md - active wb2axip limits in
  docs/architecture.md sec 4 (multi-beat, faxi_wstrb permissive stub);
  C++ vs SV timing nuance in docs/architecture.md sec 3.2; resolved
  history dropped

Local link validation pass: no maintained doc references any deleted
file. `make check` clean (lint_scenarios + lint_docs + build + tests).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 6: Verify**

Run: `git log --oneline -4`
Expected: 4 commits visible (gitignore, archive, new docs, deletes).

Run: `git log --oneline -1`
Expected: commit 4 at HEAD.

---

## Self-Review checklist (run after writing this plan)

- [x] Every spec section maps to tasks:
  - sec 1 scope: Task 1 (gitignore), Task 22 (deletes)
  - sec 2 taxonomy: Task 7 (docs/_archive/README.md)
  - sec 3 style guide: Task 10 (lint_docs.py), Task 11 (Makefile wiring),
    Task 12-15 (all written ASCII-only)
  - sec 4 migration table: Task 2-9 (archive moves), Task 22 (deletes),
    Task 15 (spec/ni/README rewrite)
  - sec 5 README outline: Task 12
  - sec 6 architecture outline: Task 13
  - sec 7 development outline: Task 14
  - sec 8 commit batching + disposition matrix: Task 9 (commit 2),
    Task 19 (build matrix), Task 20 (commit 3), Task 21 (review gate),
    Task 22 (commit 4)
  - sec 9 open considerations: addressed in README text (Task 12)
    and CLAUDE.md fix (Task 16)
- [x] No "TBD" / "TODO" / "fill in details" placeholders
- [x] File paths exact (every Task lists Files block)
- [x] Commands include expected output where applicable
- [x] ASCII byte check is mandatory and runs in `make check`

## What this plan does NOT do

- Inventory verification of the exact 72 .md count after every commit
  (the spec sec 4 lists expected ranges; implementer may discover small
  variations and is expected to update the spec amendment trail rather
  than the plan).
- Code redundancy audit (unused includes, orphan files) - out of scope
  per spec sec 1.
- LICENSE selection - out of scope per spec sec 1 / sec 9.
- Markdown lint (markdownlint-cli2) integration - deferred per spec sec 3.

## Out of scope (deferred to later rounds)

- mdbook / Sphinx documentation site
- CONTRIBUTING.md and THIRD_PARTY_NOTICES.md as separate root files
- spec/ni/doc/*.md and spec/ni/dv/*.md restyling
- specgen/docs/ restructuring
