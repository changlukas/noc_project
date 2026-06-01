# c_model First-Round Sufficiency Findings — Final Disposition

End-state: 0 PENDING, 5 RESOLVED, 2 DEFERRED with re-open triggers.

Each finding lists: gap, where surfaced, status, resolution evidence.

## F-001 — codegen does not elaborate `HeaderField` enum
- Surfaced: `Flit::set_header_field` in Task 10 (first round)
- Status: **DEFERRED**
- Re-open trigger: Layer B unit starts (any unit consuming Flit header field by name)
- Current workaround: hand-rolled string dispatch in `flit.hpp::detail::header_field_pos` (covers 6 named fields)

## F-002 — codegen does not elaborate padding-field list
- Surfaced: `Flit::check_padding_is_zero` was stub in Task 10
- Status: **RESOLVED** (Phase X.4)
- Resolution: codegen elaborates `ni::header::PADDING_FIELDS[]` array of `{name, lsb, msb}` for each header field with `enabled: false` AND `width > 0`. c_model `check_padding_is_zero` iterates this array (no hand-listed names).

## F-003 — codegen does not elaborate per-channel payload field positions
- Surfaced: `Flit::set_payload_channel` / `get_payload_channel` were stubs in Task 10
- Status: **DEFERRED**
- Re-open trigger: Layer B / Stage 2 payload pack/unpack work begins
- Action taken: removed `set_payload_channel` / `get_payload_channel` from `Flit` public API (Phase X.4 quarantine). Re-introduce when this finding closes.

## F-004 — codegen does not elaborate per-register reset value
- Surfaced: `RegisterFile::reset` was all-zero in Task 12
- Status: **RESOLVED** (Phase X.2)
- Resolution: codegen elaborates `constexpr uint32_t <REG>_RESET = N;` per non-reserved register. c_model `reset()` writes per-register codegen value. Reserved rows (e.g. `0x110` with `reset_expr: null`) are skipped.

## F-005 — codegen does not elaborate ALL_OFFSETS[] array
- Surfaced: `RegisterFile::known_offsets_` was a 31-entry hand-maintained set in Task 12
- Status: **RESOLVED** (Phase X.2)
- Resolution: codegen elaborates `ni::regs::ALL_OFFSETS[]` + `ALL_OFFSETS_COUNT`. c_model builds `known_offsets` from these.

## F-006 — codegen access-mode emission was awkward (per-register single-value enum class)
- Surfaced: `RegisterFile::is_wo_` / `is_rw1c_` were stubs returning false in Task 12. Codegen DID emit symbols but in an unusable shape.
- Status: **RESOLVED** (Phase X.3)
- Resolution: codegen redesigned to single `enum class AccessMode { RO, RW, RW1C, WO }` + per-register `constexpr <REG>_ACCESS`. c_model `access_mode_of(offset)` switch dispatches `read32` / `write32` per mode. RW1C clears bits, WO read-as-zero, RO silent-ignore writes.

## F-007 — RegisterFile ABI dispatch did not consume csr_policy sentinels
- Surfaced: `RegisterFile::read32` / `write32` had hardcoded DecErr for misaligned/sub-word
- Status: **RESOLVED** (first round, Task 12 fix `f6e0222`)
- Resolution: `if constexpr (ni::regs::csr_policy::*_IS_SLVERR)` dispatches on codegen-elaborated csr_policy sentinels.

---

## Process lesson recorded

The first-round Phase 1 sufficiency-findings policy did not require the implementer to `grep` codegen output before classifying a gap as "codegen does not elaborate X". F-002 and F-006 were misclassified — the symbols existed but the implementer wrote stubs anyway. Future rounds must include a **consume audit**: for each apparent missing symbol, `grep` the existing elaborated headers first.
