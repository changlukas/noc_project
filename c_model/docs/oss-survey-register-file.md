# OSS Survey — RegisterFile

## Need
- Storage keyed by offset (32-bit reg width)
- ABI policy dispatch: unmapped_read / misaligned / sub_word_write / wo_read
- RW1C semantics
- Reset from codegen-elaborated per-register reset value (sufficiency finding F-004)

## Candidates considered
| Library | License | Approach | Suitability |
|---|---|---|---|
| PeakRDL family (PeakRDL-cpp etc.) | MIT | SystemRDL → C++ register class generator | Excluded by spec_validate design plan §4.2 |
| `std::unordered_map<uint32_t, uint32_t>` + hand-rolled ABI | std | flat storage + per-policy dispatch | Aligns with codegen offset/mask/csr_policy directly |
| `register_dsl` (hypothetical) | n/a | declarative reg-block DSL | None mature for cycle-accurate semantics |

## Decision
**std::unordered_map<uint32_t, uint32_t> + hand-rolled ABI policy dispatch**

Rationale: PeakRDL family excluded by upstream design (§4.2). Other declarative
DSLs don't materially reduce the wiring code that translates `csr_policy::*`
constexpr (codegen output) into runtime behavior — the dispatch is ~20 LOC.

## Future revisit trigger
If RegisterFile grows beyond ~40 registers with multiple inter-register
constraints (interrupt aggregation, debounced counters), revisit for a
state-machine DSL.
