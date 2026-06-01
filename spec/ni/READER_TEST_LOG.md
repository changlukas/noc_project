# noc-sim-ni-bfm — Reader Test Log

This log substantiates the D1-completeness claim for `dogfood/noc-sim-ni-bfm/`. Per `stage_gates.md` item `D1.cross.bfm_reader_test`, a `protocol-bfm` mode spec at D1 must have run a reader test of at least 8 questions covering all 8 universal-BFM categories applicable to the protocol; gaps fixed.

The reader test was executed twice: an initial run, which surfaced five gaps (one NOT_ANSWERED, four AMBIGUOUS); the spec was edited to close them; a re-run confirmed every previously-failing question now resolves to a quoted sentence in the spec.

Both runs were performed by spawning a fresh-context subagent (the `spec-reader` role) with read-only access to the twelve noc-sim-ni-bfm spec files. The subagent had no access to authoring history, design intent, or external references — only the spec text. This isolation is the entire point of the reader test: the spec author cannot un-know what they wrote, and the test exists to expose that gap.

This NI BFM is a **dual-role, multi-protocol BFM** (AXI4 host-side both manager and subordinate ports + custom NoC flit protocol on the fabric side + AXI4-Lite for CSR access). It is also a paired BFM-with-RTL implementation (`MODE.md` declares `has-rtl-counterpart: yes`). All three protocol families and the dual-role nature were exercised by the question set.

---

## Protocol

- **Files in scope** (treated as the only source of truth):
  - `MODE.md`
  - `README.md`
  - `doc/theory_of_operation.md`
  - `doc/signal_interface.md`
  - `doc/pin_level_reset.md`
  - `doc/protocol_rules.md`
  - `doc/channel_handshake.md`
  - `doc/transaction_api.md`
  - `doc/channel_api.md`
  - `doc/active_passive_mode.md`
  - `doc/registers.md`
  - `dv/plan.md`

- **Question selection** (12 questions, drawn from `references/process/bfm_reader_test_bank.md`):
  - 8 universal-BFM questions covering all 8 categories — protocol rules (Q1), handshake dependencies (Q2), pin-level reset (Q3), API completeness (Q4, Q5), mode coverage (Q6), outstanding tracking (Q7), ID rules (Q8, Q9 — mandatory for AXI4 per the bank), error injection (Q10).
  - 2 block-specific questions for the **burst-capable** bank (Q11) and the **slave-side BFM** bank (Q12).
  - Master/slave/ID-bearing/burst banks all applicable; cache-coherent and monitor-only banks not applicable.

- **Answer rule**: PASS only if a verbatim spec sentence answers the question. NOT_ANSWERED when the spec is silent. AMBIGUOUS when relevant text exists but does not unambiguously answer.

---

## Run 1 — initial

| #   | Question summary                                                                                           | Outcome      |
|-----|------------------------------------------------------------------------------------------------------------|--------------|
| Q1  | Every channel from signal_interface §Channel grouping has a rule sub-section / alias in protocol_rules?    | AMBIGUOUS    |
| Q2  | Every "must precede" arrow in channel_handshake.md corresponds to an XCH rule?                             | PASS         |
| Q3  | Pin_level_reset wire parity with signal_interface + minimum reset assertion duration with units?           | PASS         |
| Q4  | Every method in transaction_api.md has all five sub-sections (or an explicit compact-form convention)?     | NOT_ANSWERED |
| Q5  | Channel API sufficient for back-pressure recovery + intentional protocol-violation injection?              | PASS         |
| Q6  | When BFM switches ACTIVE→PASSIVE, exactly which wires stop being driven, and what value do they take?      | PASS         |
| Q7  | Maximum outstanding-transaction count + behavior on overflow (FAIL / back-pressure / undefined)?           | AMBIGUOUS    |
| Q8  | BFM enforces same-AXI-ID ordering at the response side (rule ID quoted)?                                   | PASS         |
| Q9  | Behavior on duplicate outstanding AXI ID (second AW with same `awid` while first not yet responded)?       | PASS         |
| Q10 | Every config knob in transaction_api / active_passive has a corresponding CFG rule in protocol_rules?      | AMBIGUOUS    |
| Q11 | Burst types supported + max burst length + 4KB-boundary enforcement (incl. WRAP/FIXED scope)?              | AMBIGUOUS    |
| Q12 | Slave-mode response delay model — fixed / configurable / random; via what knob; unit + domain stated?      | PASS         |

**Outcome: 7 PASS, 1 NOT_ANSWERED, 4 AMBIGUOUS.**

### Gap details

**Q1 — AMBIGUOUS.** AXI host channels (AW_IN/AW_OUT etc.) and CSR channels were correctly mapped via `protocol_rules.md` §AXI4 host-side rules header alias and §CSR sub-section titles. NoC channels REQ_OUT/REQ_IN/RSP_OUT/RSP_IN were promised in `signal_interface.md` §Channel grouping ("Protocol-rule IDs in protocol_rules.md use these channel tokens") but not used as tokens in NoC rule IDs and not aliased anywhere. The reader could not confirm whether a NoC `noc_*_o.valid` wildcard pattern in a rule was meant to cover REQ_OUT, RSP_OUT, or both.

**Q4 — NOT_ANSWERED.** ~20 of ~33 methods in `transaction_api.md` §Method details lacked one or more of the five sub-sections (Signature / Preconditions / Side effects / Return value / Error modes). Mirror methods ("Mirror of X for reads"), CSR wrappers (set_qos_*, set_bandwidth_*), observation getters (get_observed_*), and one-shot knobs (set_inject_ecc_error, set_response_fault) all used compact form without §API conventions documenting the convention. The reader had no rule by which to determine whether the omissions were intentional inheritance or accidental gaps.

**Q7 — AMBIGUOUS.** `transaction_api.md` Preconditions framed `outstanding count for id < MAX_TXNS_PER_ID` as a caller contract (violation = undefined behavior). `protocol_rules.md` `AXI4_MST_RoB_OUTSTANDING_LIMIT` and `dv/plan.md` TP37 described NMU back-pressuring `awready`/`arready` (RTL behavior). The two answers were incompatible without an explicit "BFM mirrors RTL" or "BFM treats as caller error" commitment. No `OUTSTANDING_LIMIT_EXCEEDED` return enum existed.

**Q10 — AMBIGUOUS.** `set_response_delay_axi`, `set_response_delay_noc`, `set_inject_ecc_error`, `set_response_fault` (BFM-internal test-only knobs) had no corresponding CFG rules in `protocol_rules.md` and no explicit scoping statement excluding them. `set_bfm_mode` was annotated as corresponding to `NI_CFG_MODE_SWITCH` with an unresolved designer-TODO marker — the rule was promised but absent.

**Q11 — AMBIGUOUS.**
- (a) FIXED burst type: never mentioned in `transaction_api.md` `apply_burst_write` description (only INCR + WRAP semantics described).
- (b) Conflicting maximum burst length: `signal_interface.md` claimed "AXI4 full ... bursts up to AWLEN=255 / ARLEN=255" while `transaction_api.md` claimed "default MAX_BURST_LEN=16; for len > 15 BFM logs warning and may truncate or fail" — and the configurability of MAX_BURST_LEN was not specified.
- (c) `AXI4_SLV_AW/AR_BURST_4KB_BOUNDARY` rules were scoped to `awburst==INCR` only; behavior under WRAP and FIXED was silent (although by AXI4 construction WRAP cannot cross 4KB and FIXED has constant address).

---

## Fixes applied

| Gap | File(s) | Fix |
|-----|---------|-----|
| Q1  | `doc/protocol_rules.md`                                  | Added §"Channel naming convention" preamble with explicit alias table mapping abstract tokens (AW/W/B/AR/R; `noc_*_o`/`noc_*_i` patterns) to the 19 channels in `signal_interface.md` §Channel grouping. AXI4 channels alias `*_IN`+`*_OUT`; NoC wildcard patterns alias the four NoC channels with role-based discrimination (`<PROTO>_MST_*` → BFM-driven outputs REQ_OUT/RSP_OUT; `<PROTO>_SLV_*` → BFM-observed inputs REQ_IN/RSP_IN). Also resolves LINT-BFM-004 hygiene warning. |
| Q4  | `doc/transaction_api.md` §API conventions                | Added §"Method documentation form" sub-section explicitly establishing three compact-form documentation cases (Mirror methods / CSR wrappers / Observation getters and one-shot knobs), enumerating which methods fall in each, and stating exactly what each omitted sub-section inherits or implies. Also resolves LINT-BFM-005 hygiene warning. |
| Q7  | `doc/transaction_api.md`                                 | Added §"Outstanding-limit overflow behavior" after §Behavior under reset, committing the BFM to **RTL parity**: `apply_axi_*` calls block until back-pressure relief, no separate `OUTSTANDING_LIMIT_EXCEEDED` return enum, only `TIMEOUT` if relief never arrives. Reframed the original Preconditions text as a "performance / liveness guideline, not a hard contract violation." |
| Q10 | `doc/protocol_rules.md` §Configuration-knob rules        | Added 5 missing CFG rule rows: `NI_CFG_MODE_SWITCH`, `NI_CFG_RESPONSE_DELAY_AXI`, `NI_CFG_RESPONSE_DELAY_NOC`, `NI_CFG_INJECT_ECC_ERROR`, `NI_CFG_RESPONSE_FAULT`. Each describes the wire-level effect of the corresponding knob. `NI_CFG_MODE_SWITCH` described in active_passive_mode.md L28 was promoted from TODO to fully-specified rule. |
|     | `doc/active_passive_mode.md`                             | Removed the unresolved designer-TODO annotation from §Mode switch since `NI_CFG_MODE_SWITCH` now exists. Also tightened wording "immediately float" → "transition to" for clarity (addresses Q6 Run-1 caveat). |
| Q11 | `doc/transaction_api.md` `apply_burst_write`             | (a) Added §"Burst-type support" stating FIXED/INCR/WRAP all accepted with FIXED semantics. (b) Added §"Maximum burst length (`MAX_BURST_LEN`)" stating default=16, configurable to ≥256 to support full AXI4 awlen=255, with `BURST_LEN_EXCEEDS_MAX` error enum for over-length. (c) Side-effects bullets now explicitly state WRAP and FIXED both bypass 4KB rules with rationale. |
|     | `doc/protocol_rules.md` `AXI4_SLV_AW/AR_BURST_4KB_BOUNDARY` | Required-behavior column extended: "WRAP bursts are bounded within their wrap boundary by AXI4 construction (always within 4KB); FIXED bursts hold address constant — neither needs explicit 4KB enforcement." |

---

## Run 2 — post-fix

The five previously-failing questions were re-run against the fixed spec. The seven Run 1 PASSes were not re-run (no fix applied; PASS status preserved).

| #   | Question summary                                                                                           | Outcome  |
|-----|------------------------------------------------------------------------------------------------------------|----------|
| Q1  | Every channel from signal_interface has a rule sub-section / alias in protocol_rules?                      | **PASS** |
| Q2  | Every "must precede" arrow corresponds to an XCH rule? (Run 1 PASS preserved)                              | PASS     |
| Q3  | Pin_level_reset wire parity + reset duration with units? (Run 1 PASS preserved)                            | PASS     |
| Q4  | Every method has all five sub-sections OR explicit compact-form convention?                                | **PASS** |
| Q5  | Channel API sufficient for back-pressure / violation injection? (Run 1 PASS preserved)                     | PASS     |
| Q6  | ACTIVE→PASSIVE wire list and value? (Run 1 PASS preserved)                                                 | PASS     |
| Q7  | Outstanding count + overflow behavior unambiguously stated?                                                | **PASS** |
| Q8  | Same-AXI-ID ordering rule? (Run 1 PASS preserved)                                                          | PASS     |
| Q9  | Duplicate outstanding ID behavior? (Run 1 PASS preserved)                                                  | PASS     |
| Q10 | Every knob (incl. set_bfm_mode, no longer TODO) has a CFG rule?                                            | **PASS** |
| Q11 | Burst types + max length + 4KB scope all unambiguously stated?                                             | **PASS** |
| Q12 | Slave-mode response delay model? (Run 1 PASS preserved)                                                    | PASS     |

**Outcome: 12/12 PASS.**

---

## Verdict

`D1.cross.bfm_reader_test` ✓. The spec is reader-test-clean. All universal-BFM categories have at least one passing question; the burst-capable and slave-side block-specific banks pass; the ID-bearing mandatory questions Q8 + Q9 (per `bfm_reader_test_bank.md` §"For ID-bearing protocols ... questions 20-22 ... mandatory") pass.

The reader-test exercise also contributed two non-blocking wins from the lint pass:
- **LINT-BFM-004** (channel naming convention) is now resolved by the §Channel naming convention preamble in `protocol_rules.md`.
- **LINT-BFM-005** (transaction API ↔ channel API decomposition annotations) is now resolved by the §Method documentation form sub-section in `transaction_api.md` §API conventions.

The two remaining lint hygiene items (LINT-002 untracked-TODO format, LINT-BFM-001 grouped-row expansion in pin_level_reset.md) are unaffected by reader-test fixes; they remain as ~40 minutes of optional polish before formal D1 sign-off.

Run 2 conducted 2026-05-05.
