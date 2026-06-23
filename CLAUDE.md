# CLAUDE.md — NoC C++ Behavior Model

## Project Overview

**Namespace**: `ni::cmodel::` (production sub-namespaces under `c_model/include/`: `axi`, `ni`, `nmu`, `nsu`, `router`, `wrap`. Tests under `c_model/tests/common/` add a `ni::cmodel::testing` sub-namespace).

**Architecture**: AXI4 Master → NMU → ChannelModel (test stub) → NSU → AXI4 Slave. Verilator co-sim wraps each c_model component in a `*_wrap.hpp` and drives the scenarios through the wire-level testbench; correctness is checked by the c_model scoreboard (per-transaction write→readback compare) plus the model's own internal checks.

- **NMU / NSU**: per-direction units (`c_model/include/nmu/`, `c_model/include/nsu/`). Packetize / depacketize, AXI port adapters, per-ID RoB on the NMU response path.
- **NoC fabric**: no router class in c_model; `ChannelModel` (`c_model/tests/common/channel_model.hpp`) is the only NoC stub. Destination derivation (XY bit-slice) is done at NMU packetize time via `nmu::addr_trans::xy_route`.
- **Wrap layer**: `NmuWrap` / `NsuWrap` / `MasterWrap` / `SlaveWrap` / `ChannelModelWrap` in `c_model/include/wrap/`. Per-instance via a 64-bit integer handle ABI — `unsigned long long cmodel_<shell>_create(name)` returns an integer-encoded `HandleBlock*` registered in a process-wide `g_handle_registry`; cycle ops take `unsigned long long ctx` (SV `longint unsigned`) and validate via `REQUIRE_HANDLE`. (chandle is avoided because VCS rejects it as a module port.) No cross-component pointers.
- **Config**: YAML (`c_model/config/`); no JSON, no compile-time `<Mode>` templates.

**Build**: C++17, CMake 3.20+, GoogleTest.
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
