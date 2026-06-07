# CLAUDE.md — NoC C++ Behavior Model

## Project Overview

**Namespace**: `noc::`

**Architecture**: User Code → NocSystem Public API → Internal Components → Co-Sim Bridge (DPI-C)

- **Uniform Router**: No Edge/Compute distinction; all routers identical.
- **Dual Flow Control**: Valid/Ready (Version A) AND Credit-Based (Version B), compile-time template.
- **Factory Pattern**: JSON config → factory function → template instantiation (analogous to RTL parameter).
- **Hot-Swap Interface**: `Router_Interface<Mode>` / `NI_Interface<Mode>` abstract base classes.

**Build**: C++17, CMake, GoogleTest.
- Use `py -3` instead of `python3` (Windows).
- Path separators: forward slash `/` or double backslash `\\`.

**Communication**:
- Reply in Traditional Chinese; keep technical terms in English.
- User is not fluent in C++; use RTL analogies when explaining implementation concepts.
- Ask before making any design decision.

## Key Design Docs

- `docs/_archive/noc_cmodel_rtl_plan.md` - stage roadmap (archived 2026-06-07; superseded by `docs/architecture.md`)

## Doc Writing Rules

### Parameter Discipline

- Before changing any parameter value, range, or default: propose the change and wait for user approval.
- Mark uncertain values `[TBD]`; never guess and write as fact.
- After any parameter edit, list all other files referencing that parameter and confirm consistency.

### Cross-File Consistency

- After editing a spec file, list every other `.md` that references the same definition.
- When editing multiple files, apply the change to all of them — do not stop after the first.
- If a cross-file mismatch is found, surface the diff and let the user decide which version wins.

### Technical Accuracy

- Claims about external architectures require a cited source. Public protocol specs (AMBA, etc.) are acceptable references; vendor-specific IP/product guides are not.
- Mark uncertain technical facts `[UNVERIFIED]`; distinguish confirmed from inferred.
- Do not reference specific external IP, vendor, or product-guide names in code or docs.

### Writing Quality

- Default tone: precise, professional engineering prose. No filler words ("notably", "additionally", "in conclusion").
- Match detail level to the ask: high-level summary → no field-level expansion; exact spec request → no vague overview.
- Confirm target granularity (overview / spec / implementation guide) before editing a doc.
- Apply the Karpathy 4-lens reviewer perspective: if removing a sentence leaves the reader able to complete their original task, delete the sentence.

## Process

Break complex work into 3-5 stages. Document each stage in `IMPLEMENTATION_PLAN.md`:

```
## Stage N: [Name]
Goal: [specific deliverable]
Success Criteria: [testable outcomes]
Status: [Not Started | In Progress | Complete]
```

Remove `IMPLEMENTATION_PLAN.md` when all stages are complete.

**When stuck after 3 attempts — STOP.** Document what failed (attempts, error messages, hypotheses). Research 2-3 alternative approaches. Question whether the abstraction level is right. Try a different angle before resuming.

Commit incrementally: every commit must compile, pass all existing tests, and include tests for new functionality.

## Quality Gates

Every commit:
- Compiles successfully.
- Passes all existing tests.
- Includes tests for new functionality.
- Has a commit message in format `type(scope): description` (English).
- Valid types: `feat`, `fix`, `docs`, `style`, `refactor`, `test`, `chore`, `perf`, `build`, `revert`.

Branch: feature work on feature branches; PRs target `main`.

## Reminders

Never:
- Use `--no-verify` to bypass commit hooks.
- Disable tests instead of fixing them.
- Commit non-compiling code.
- Make assumptions — verify against existing code.
- Reference external IP, vendor, or product-guide names in code or docs.

Always:
- Commit working code incrementally.
- Update `IMPLEMENTATION_PLAN.md` as you progress.
- Use RTL analogies when explaining C++ concepts to the user.
