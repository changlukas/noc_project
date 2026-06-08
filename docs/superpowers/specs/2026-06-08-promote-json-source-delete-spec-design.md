# Promote `specgen/generated/json/` to source-of-truth + delete `spec/`

**Date:** 2026-06-08
**Topic:** Remove the obsolete `spec/*.md` → `ni_packet.json` markdown-parse layer; promote the JSON to canonical source; delete `spec/` entirely.
**Scope:** specgen refactor + VERSION relocation + spec/ deletion + 5 doc cross-ref cleanups. Codegen.py JSON→cpp/sv pipeline is unchanged.

## 1. Problem

The current codegen pipeline has two stacked layers:

```
spec/ni/doc/*.md     (human-edited markdown prose)
    │
    │  specgen/ni_spec/__main__.py
    │  + specgen/ni_spec/generator/{packet,signals,protocol_rules,registers}.py
    │  parse markdown tables
    ▼
specgen/generated/json/*.json    (machine JSON, drift-gate-protected)
    │
    │  specgen/tools/codegen.py + specgen/tools/elaborate/*
    ▼
specgen/generated/{cpp,sv}/*     (downstream constants consumed by c_model + cosim)
```

The upper layer (md → json) is the source of the spec-vs-impl drift the audit surfaced (FLIT_WIDTH 402 vs 408, NOC_QOS_WIDTH 0 vs 4, B channel `rsvd_mc_status` missing, W channel `wstrb`/`wdata` order, etc.). The markdown prose drifted from the JSON; users (and future AI sessions) trust the markdown and write incorrect code.

The lower layer (json → cpp/sv) is the trusted layer. `codegen.py --check` regenerates cpp/sv from JSON and diffs against committed; drift is caught immediately. `c_model/include/ni/flit.hpp` and `cosim/c/cmodel_dpi.cpp` consume the JSON-derived constants. The JSON is what reality is built from.

User (2026-06-08) flagged the architecture: `specgen/generated/json/` should be the source. The `spec/` markdown layer is redundant and confusing — delete it.

## 2. Ground truth anchors

- `docs/image/{header,aw_ar_format,b_format,w_format,r_format}.jpg` — visual spec for header / per-channel payload (per `feedback-image-spec-ground-truth`)
- `specgen/generated/json/{ni_packet,ni_signals,ni_registers,ni_protocol_rule_index}.json` — canonical machine spec
- `specgen/generated/cpp/ni_flit_constants.h` — derived C++ constants (FLIT_WIDTH=408, HEADER_WIDTH=56, etc.)
- `cosim/c/cmodel_dpi.cpp:30` — `static_assert(FLIT_WIDTH==408)` enforces JSON's correctness at compile time

## 3. Design

### 3.1 VERSION relocation

`spec/ni/VERSION` is consumed by `specgen/ni_spec/loader.py:load_spec_version()` (called by `codegen.py:76`). After this refactor, `spec/` no longer exists, so VERSION must move.

Move `spec/ni/VERSION` → `specgen/source/VERSION`. Update `loader.py:load_spec_version()` to read `SPECGEN_ROOT / "source" / "VERSION"` instead of `SPECGEN_ROOT.parent / "spec" / "ni" / "VERSION"`.

Rationale: `specgen/source/` is conceptually the home for things specgen treats as source (it already contains `constants.yaml`, `interface_handshake.json`, `ni_function_blocks.json`). VERSION belongs alongside.

### 3.2 Files to delete

**Markdown → JSON generator layer (code):**

- `specgen/ni_spec/__main__.py` — CLI entry point `python -m ni_spec <md_dir>` that drives the md→json pipeline
- `specgen/ni_spec/generator/` — entire package (5 modules: `packet.py`, `signals.py`, `registers.py`, `protocol_rules.py`, `_common.py`, plus `__init__.py`)

**Markdown → JSON generator tests:**

- `specgen/tests/test_protocol_rules.py` — entirely md-parser tests (`generator.parse_protocol_rule_index(MD_DIR / "protocol_rules.md")`)
- `specgen/tests/test_registers_parser.py` — entirely md-parser tests (`generator.parse_csr_policy/parse_register_map/parse_register_fields(MD_DIR / "registers.md")`)
- Lines 83-104 of `specgen/tests/test_foundation.py` — md-parser method calls; KEEP the top-of-file `test_load_spec_version_returns_string` and the `_load_packet` JSON-loading helpers. Either delete the md-parser test methods or move them to a `test_md_parser.py.removed` file then delete.

**Tests that import `ni_spec.invariants` / `ni_spec.report` are KEPT** (invariants still validate hand-edited JSON; see §3.3).

**Spec directory:**

- `spec/` entire subtree — 17 files after C1 moves VERSION (10 `doc/*.md`, 3 `dv/*.md`, 2 `images/*.md`, `README.md`, `MODE.md`).

### 3.3 Files to keep (verify untouched)

- `specgen/tools/codegen.py` — orchestrator that walks DOMAIN_TO_EMITTER and runs JSON→cpp/sv emitters. **No spec/ access.** Keep, no change.
- `specgen/tools/elaborate/*.py` — the json→cpp/sv emitters. **No spec/ access.** Keep untouched.
- `specgen/ni_spec/loader.py` — keep, but C1 updates the VERSION path AND the docstring/error message strings at lines 76, 79, 83.
- `specgen/ni_spec/constants.py` — keep. C4 updates the line 383 comment "Per spec/ni/doc/packet_format.md and ni_signals.json" to reference `specgen/generated/json/ni_packet.json + ni_signals.json` instead.
- `specgen/ni_spec/handshake_schema.py` — keep, reads `specgen/source/constants.yaml`.
- `specgen/ni_spec/invariants.py` — **KEEP**. Imported by `specgen/ni_spec/__init__.py:9` AND by 4 test files (`test_registers_validator.py:3`, `test_protocol_rules.py:5` [tests deleted but invariants module survives], `test_pin_level_reset.py:4`, `test_function_blocks.py:5`). The invariants validate JSON structure and still apply when JSON is hand-edited; they are not dead code.
- `specgen/ni_spec/report.py` — **KEEP**. Imported by `specgen/ni_spec/__init__.py:23` (`n_err`, `n_warn`, `print_report`) and used by the kept test suite for reporting.

### 3.4 `specgen/ni_spec/__init__.py` cleanup (explicit, part of C2)

Read `specgen/ni_spec/__init__.py`. The file currently has generator re-exports at lines 15-21 and `__all__` entries at lines 37-40 (per Codex audit). C2 must:

- Delete the `from .generator import ...` lines (the generator package is gone)
- Delete corresponding `__all__` entries that reference generator symbols
- Keep `from .invariants import ...` and `from .report import ...` re-exports (those modules stay per §3.3)

Failure mode if skipped: ImportError at first `import ni_spec` after C2.

### 3.5 Test impact

- `specgen/tests/test_foundation.py:12-16` — `test_load_spec_version_returns_string` calls `loader.load_spec_version()`. After §3.1 relocation, the test still passes (the API surface is unchanged, only the file location moves).
- `specgen/tests/test_byte_identical_golden.py` — runs `codegen.py --check`. No md→json path touched; should still pass.
- Other specgen tests that import from `ni_spec.generator` — if any, they break. Survey before delete; either keep stubs or delete the tests.

### 3.6 Doc cross-references (full inventory, Codex-verified)

Cleanup is part of C4. Surviving `spec/ni` references in the repo:

| File:line | Reference | Action |
|---|---|---|
| `README.md:28` | `- \`spec/ni/\` - normative NI specification` (in "Where code lives") | Delete the bullet |
| `README.md:65` | `- [NI specification](spec/ni/README.md)` (in Documentation) | Delete the bullet |
| `docs/architecture.md` | `spec/ni/README.md` in §7 References | Delete the bullet |
| `docs/development.md:128` | `spec/ni/README.md` listed in `lint_docs` MAINTAINED_DOCS narrative | Drop from the list |
| `docs/development.md:470` | `spec/ni/README.md` in §9 References | Delete the bullet |
| `docs/_archive/README.md` | "Looking for the normative NI spec: read `../../spec/ni/README.md`" | Delete the bullet |
| `Makefile:93` | `spec/ni/README.md` in `MAINTAINED_DOCS` | Delete the line |
| `specgen/source/ni_function_blocks.json:7-8` | `"source"` array entries `"spec/ni/README.md §Features"` + `"spec/ni/doc/theory_of_operation.md"` | Delete the entries or replace with `"docs/image/*.jpg"` / `"specgen/generated/json/ni_packet.json"` |
| `specgen/tools/README.md:10,224,264,274,314` | Describes the deprecated `python -m ni_spec ../spec/ni/doc` md→json workflow | Rewrite the README to describe the new JSON-source workflow OR move the file to `docs/_archive/` |
| `specgen/docs/guide/json-to-code-examples.md` | Codex flagged stale workflow text (verify during execution; if md-source workflow described, update or archive) | Update or archive |
| `specgen/ni_spec/constants.py:383` | Comment "Per spec/ni/doc/packet_format.md and ni_signals.json" | Update comment to "Per specgen/generated/json/ni_packet.json and ni_signals.json" |

The 2026-06-07 `docs/superpowers/specs/*.md` and `docs/superpowers/plans/*.md` historical references to `spec/ni/doc/packet_format.md` are intentionally exempt: those docs describe a state before this refactor; they are project history, not actionable workflow.

### 3.7 Drift-gate verification

After all changes:

- `py -3 specgen/tools/codegen.py --check` — should pass (no logic change, drift gate is JSON→cpp/sv only)
- `py -3 -m pytest specgen/tests/test_byte_identical_golden.py -v` — should pass (goldens unchanged)
- `py -3 -m pytest specgen/tests/test_foundation.py -v` — should pass (VERSION at new location)
- `py -3 tools/lint_docs.py <MAINTAINED_DOCS without spec/ni/README.md>` — should pass
- `py -3 tools/lint_scenarios.py` — should pass (no spec/ touch)

## 4. Commit chain

Four atomic commits, each independently `make lint_docs` + drift-gate clean:

### C1 — `refactor(specgen): relocate VERSION + update loader`

- `git mv spec/ni/VERSION specgen/source/VERSION`
- Edit `specgen/ni_spec/loader.py` lines 76-83:
  - Docstring: `"""Read spec/ni/VERSION (...)` → `"""Read specgen/source/VERSION (...)`
  - Path: `SPECGEN_ROOT.parent / "spec" / "ni" / "VERSION"` → `SPECGEN_ROOT / "source" / "VERSION"`
  - Error message: `f"spec/ni/VERSION not found at {version_file}"` → `f"specgen/source/VERSION not found at {version_file}"`
- Verify: `py -3 specgen/tools/codegen.py --check` exits 0
- Verify: `py -3 -m pytest specgen/tests/test_foundation.py::test_load_spec_version_returns_string -v` passes

### C2 — `refactor(specgen): delete markdown → JSON generator layer`

- Delete `specgen/ni_spec/__main__.py`
- Delete entire `specgen/ni_spec/generator/` package (5 modules + `__init__.py`)
- Delete `specgen/tests/test_protocol_rules.py` (entire file — pure md-parser tests)
- Delete `specgen/tests/test_registers_parser.py` (entire file — pure md-parser tests)
- Edit `specgen/tests/test_foundation.py`: remove the md-parser test methods at lines 83-104 (keep `test_load_spec_version_returns_string` and JSON-loading helpers untouched)
- Edit `specgen/ni_spec/__init__.py`: remove `from .generator import ...` lines (15-21) and corresponding `__all__` entries (37-40); keep `from .invariants` and `from .report` re-exports
- KEEP `specgen/ni_spec/invariants.py` and `report.py` (used by surviving tests + `__init__.py`)
- Verify: `py -3 -m pytest specgen/tests/ -v` passes (modulo deleted parser tests)
- Verify: `py -3 specgen/tools/codegen.py --check` exits 0

### C3 — `chore: git rm -rf spec/`

- `git rm -rf spec/` (17 files after C1 moved VERSION)
- Verify drift gate: `py -3 specgen/tools/codegen.py --check` exits 0 (no code reads spec/ after C1+C2)

### C4 — `docs: drop spec/ni cross-references after pipeline refactor`

Edit per §3.6 inventory:
- `README.md` (lines 28, 65)
- `docs/architecture.md` (§7 References)
- `docs/development.md` (lines 128 narrative, 470 references)
- `docs/_archive/README.md` (§When not to read this)
- `Makefile` (line 93 `MAINTAINED_DOCS`)
- `specgen/source/ni_function_blocks.json` (lines 7-8 `source` strings)
- `specgen/ni_spec/constants.py` (line 383 comment)
- `specgen/tools/README.md` (rewrite to JSON-source workflow OR move to `docs/_archive/specgen/`)
- `specgen/docs/guide/json-to-code-examples.md` (review during execution; update or archive)

Verify:
- `py -3 tools/lint_docs.py` (with `MAINTAINED_DOCS` minus `spec/ni/README.md`) passes
- `make check PYTHON3="py -3"` clean (pre-existing GCC ICE on `test_meta_buffer` / `test_loopback_noc_per_vc_credit` excepted)

## 5. Out of scope

- Renaming `specgen/generated/json/` to `specgen/source/json/` (or similar). The directory name remains `generated/json/` in this commit chain; a follow-up `chore: rename generated/ → source/` session can do that purely cosmetically.
- Updating `specgen/docs/guide/*.md` workflow descriptions (no spec/ refs found in the survey; verify during execution).
- Removing now-orphaned banner content in generated JSONs (`"Source SHA"` comments may reference md_dir paths; cosmetic, can be left).
- Refactoring `specgen/ni_spec/invariants.py` checks if they reference md_dir state.
- Fixing pre-existing GCC ICE on `test_meta_buffer` / `test_loopback_noc_per_vc_credit` (environment issue, separate session).

## 6. Success criteria

- `git ls-files spec/` returns empty
- `py -3 specgen/tools/codegen.py --check` exits 0
- `py -3 -m pytest specgen/tests/` passes (excluding tests that explicitly required md→json layer; if such tests exist, delete with documented rationale)
- `make lint_docs` clean
- No surviving `spec/ni` references in `README.md`, `docs/architecture.md`, `docs/development.md`, `docs/_archive/README.md`, `Makefile`, `specgen/source/ni_function_blocks.json`
- `grep -rn "spec/ni\|packet_format\.md\|signal_interface\.md\|protocol_rules\.md\|registers\.md" --include="*.py" --include="*.cpp" --include="*.hpp"` returns no functional references (comments referencing the historical relationship are acceptable if labeled `[historical]`)

## 7. Risks

- **`invariants.py` and `report.py` are NOT orphan** — verified by Codex audit: imported by `specgen/ni_spec/__init__.py:9,23` and by 4 surviving test files (`test_registers_validator.py`, `test_pin_level_reset.py`, `test_function_blocks.py`, plus the unused `test_protocol_rules.py` import — which is removed in C2). Per §3.3 these modules stay.
- **`specgen/ni_spec/__init__.py` re-exports** — generator re-exports at lines 15-21 + `__all__` at 37-40 (Codex-verified) MUST be cleaned in C2. Failure mode: ImportError. Mitigation: explicit cleanup in C2 task list.
- **`test_foundation.py` line 83-104 imports `ni_spec.generator`** — these md-parser test methods must be deleted in C2 (NOT C1). C1's only test responsibility is the VERSION test.
- **Atomicity of C1 VERSION move + loader update** — `git mv` + loader edit in one commit; verify before commit.
- **specgen golden tests** — `test_byte_identical_golden.py` regenerates cpp/sv from JSON and diffs goldens. After C1, the regen runs `load_spec_version()` against the new path. If VERSION content is identical, goldens match. Verify in C1.
- **specgen/tests test_signals_resolver.py:136-137** comments reference `packet_format.md` — comment only, no functional impact. Leave or update opportunistically in C4.
- **Workflow doc rewrites in C4** — `specgen/tools/README.md` is described as a md→json workflow guide. If the project still wants a specgen workflow doc, it must be rewritten for the JSON-source flow; if the new flow is "edit JSON directly", a single short README suffices. C4 must decide: rewrite vs archive.
