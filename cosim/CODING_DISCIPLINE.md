# Coding Discipline — cosim/

## SystemVerilog (`cosim/sv/*.sv`)

All `.sv` files MUST conform to the `rtl-style` skill (installed at
`~/.claude/skills/rtl-style/`, sourced from
[changlukas/rtl-forge](https://github.com/changlukas/rtl-forge)).

### Writing SV

- Before writing or modifying any `.sv` file, invoke `Skill('rtl-style')` first
- Naming: `_i` / `_o` / `_q` / `_d` / `_ni` suffixes; sync reset default
- Forbidden patterns (12-rule catalog in rtl-style references)
- Use templates under `~/.claude/skills/rtl-style/templates/` as starting points

### Reviewing SV

Dispatch `rtl-reviewer` agent per task:

    Agent(subagent_type='rtl-reviewer',
          description='Review <module>.sv',
          prompt='Review cosim/sv/<module>.sv per rtl-style skill. Return
                  categorized findings with CRITICAL/HIGH/MEDIUM/LOW severity,
                  file:line refs, exact rule violated, minimal-change fix.')

- Output: save reviewer report to `c_model/build/rtl-review-logs/<task-id>-rtl-review.md`
- Pass criteria: 0 CRITICAL + 0 HIGH findings (advancement gate)
- MEDIUM: flagged for decision; LOW: informational only
- Codex review runs in parallel for independent perspective

## All code (C++ + SV)

Invoke `Skill('karpathy-guidelines')` at the start of every coding task. Apply
the 4-lens discipline: overcomplication / surgical / surface assumptions /
verifiable success. Avoid LLM-typical mistakes: overengineering, defensive code
for impossible cases, mixed-concern files, untestable success criteria.

## Hermetic singleton invariant (cosim/c/)

Each `*_shell_adapter.hpp` owns ONE c_model component. Forbidden:

- `cosim/c/<comp_a>_dpi.cpp` referencing `g_<comp_b>_adapter`
- `*_shell_adapter.hpp` `#include`-ing another shell's adapter
- C++ component A holding a ref/ptr to component B

CI gate: `tools/check_cosim_hermetic.sh` greps for forbidden patterns. Must
pass before merge.

## Shells contain ONLY wire↔method conversion

`<comp>_shell_adapter.hpp::tick()` is allowed to:
- Read input latch + check `can_accept_*()` capacity + push beat into c_model
- Call `<comp>_->tick()` exactly once
- Read c_model output state into output latch

Forbidden: any business logic inside the adapter that should live in the c_model
component itself (e.g., packetization, routing, ROB reordering). If you find
yourself adding such logic, the c_model component is missing an API — extend
the c_model header instead.
