# CLAUDE.md — NoC c_model + RTL Integration Project

## Project Overview

**Top-level layout** (this repo's root):
- `c_model/` — C++ behavior model (Stage 2 AXI subsystem complete; Stage 3 NMU/NSU/Router 進行中)
- `specgen/` — NI spec single source of truth + codegen (golden-gated `.h` / `.sv`)
- `spec/` — NI spec markdown source (specgen 讀入)
- `rtl/` — SystemVerilog RTL (NMU/NSU stub + Stage 5 router 預留)
- `cosim/` — Verilator + DPI bridges (Stage 5)
- `docs/noc_cmodel_rtl_plan.md` — **主 plan**：Stage 2/3/4/5 roadmap

**Current stage**: Stage 2 (純 AXI subsystem) 完工 — 182/182 sequential ctest。下一步 Stage 3 NMU/NSU 內部單元，從 `c_model/include/nmu/packetize.hpp` 起頭。詳見 `NEXT_STEPS.md`。

**架構準則** (主 plan §0 結論)：
- NMU/NSU 對 AXI 是**透明 transport**：valid/ready handshake + 完整 AXI channel 屬性原樣搬運 + wlast/rlast burst framing + per-ID response 重排 + address translation + QoS/VC 仲裁 + CDC。
- **不**做：per-beat 位址生成、memory bounds 檢查、OOB DECERR 生成、slave 端 burst 拆解。這些屬 memory endpoint，住在 testbench 端 (`c_model/include/axi/`)。
- Hot-swap 邊界在 **NoC flit link**，不在 AXI port。

**Build**: C++17, CMake, GoogleTest.
- Use `py -3` instead of `python3` (Windows).
- Path separators: forward slash `/` or double backslash `\\`.

**Communication**:
- Reply in Traditional Chinese; keep technical terms in English.
- User is not fluent in C++; use RTL analogies when explaining implementation concepts.
- Ask before making any design decision.

## Key Design Docs

- `docs/noc_cmodel_rtl_plan.md` — **main plan**: NoC c_model + RTL integration roadmap (Stage 2/3/4/5)
- `NEXT_STEPS.md` — current Stage 3 next-step concrete tasks

Phase B / Phase C design specs (historical reference) live in the old
`noc-sim` repo; not carried into this project.

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
- Valid types: `feat`, `fix`, `docs`, `style`, `refactor`, `test`, `chore`, `perf`.

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
