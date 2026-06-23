# noc-sim NI BFM — Implementer Review Log

This log substantiates the `D1.cross.implementer_review` claim for `E:/03_Learning/noc-sim/spec/ni/`. Per `stage_gates.md`, a `protocol-bfm + has-rtl-counterpart=yes` spec at D1 must have run an implementer review with at least 2 paradigm-paired reviewers. The converged ambiguity list must be either resolved in spec or recorded in `WAIVERS.md`.

**Run timestamp**: 2026-05-10
**Plugin version**: 0.3.1 (run executed manually as dogfood validation; `/spec-implementer-review` command itself landed in this run's plugin source)

## Paradigms

- **`c-bfm`**: senior verification engineer building a C++ / SystemC BFM. Wire-level equivalence with parallel SystemVerilog RTL is the contract.
- **`rtl`**: senior RTL designer building synthesizable SystemVerilog. Wire-level equivalence with parallel C++ / SystemC BFM is the contract.

## Round 1 — independent reads

Both agents read the spec in `E:/03_Learning/noc-sim/spec/ni/` (12 files: `MODE.md`, `README.md`, `doc/theory_of_operation.md`, `doc/signal_interface.md`, `doc/pin_level_reset.md`, `doc/protocol_rules.md`, `doc/channel_handshake.md`, `doc/transaction_api.md`, `doc/channel_api.md`, `doc/active_passive_mode.md`, `doc/registers.md`, `dv/plan.md`).

Full transcripts of Round 1 outputs are in this session's conversation log (2026-05-10). The summary below captures each agent's key findings.

### `c-bfm` — Round 1 summary

**Top-level architecture**: `class Ni` owning `Params`, `ConfigStore`, `ErrLogger`, `Sequencer`, NMU + NSU halves split per clock domain (`NmuAclk` / `NmuNoc`, `NsuNoc` / `NsuAclk`), two `CdcFifo` instances, `CsrPort`, `Probes`, `ApiSurface`. PASSIVE mode = all `*_o` drivers held at `pin_level_reset.md` during-reset values; monitors + `irq_o` continue.

**Key implementation decisions**: two-phase per-cycle scheduler (aclk → noc_clk, sub-phases sample → compute → drive → register-update); flit modeled as packed byte array with `FlitView` accessor; SECDED static lookup table (Hsiao variant); RoB as `std::array<RobEntry, MAX_TXNS>` with intrusive linked list + `prev_dest` map; Wormhole arbiter (LockIn=1); credit counters per-VC with bi-directional init handshake; AXI-Lite CSR access with sub-word/misalign/unmapped strict response; PASSIVE force every `*_o` to during-reset value within 1 cycle.

**Round 1 ambiguities raised** (6):

1. **Flit-header bit layout** — described prose-only; SECDED, route_par, every per-field accessor depends on exact bit positions; tentative resolution: jointly publish `packet_format.md` field map.
2. **Hamming SECDED vs Hsiao SECDED** — `theory_of_operation.md` §ECC says "Hamming"; `transaction_api.md set_inject_ecc_error` says "Hsiao"; tentative: implement Hsiao.
3. **`INPUT_BUFFER_DEPTH`** described as router-side parameter, no NI port exists; tentative: BFM construction parameter, default 4.
4. **ECC counter widths** — only `ECC_CORR_ERR_CNT` width is `ERR_COUNTER_WIDTH=16`; other 3 counters not pinned; tentative: all four are 16-bit in `[15:0]`.
5. **Wormhole RR pointer initial value + advancement rule** not pinned post-reset; tentative: resets to 0, advances by 1 per granted cycle.
6. **ACTIVE→PASSIVE "within 1 cycle" deadline** counted in aclk vs noc_clk; tentative: per-domain on next clock edge, not synchronised.

**Wire-level lockdowns proposed**: 10 specific cycle-counts / bit-positions to confirm with RTL team (AW→AW-flit injection latency; W-burst accumulation timing; RoB allocator priority direction; credit-init epoch cycle; route_par drop semantics; axi_rdata_par_o for 2-bit ECC; CDC FIFO partial-reset behavior; PENDING decrement timing; NSU back-pressure direction; ERR_STATUS RW1C atomic-clear cycle).

### `rtl` — Round 1 summary

**Top-level architecture**: `ni_top` owning `csr_file` (aclk), `err_irq_aggregator` (aclk), `nmu` (mixed clocks), `nsu` (mixed clocks), probes. NMU sub-modules: 18 leaf modules covering AXI ingress, address translation, QoS gen, upsize, flit pack, RoB, ECC gen / check, route_par gen / check, VC arb / demux, injection buffer, credit ctrl, CDC FIFOs, pending counters. NSU symmetric mirror plus MetaBuffer, Exclusive Monitor, R-response buffer, downsize.

**Key implementation decisions**: CSR + IRQ at `ni_top` (single instance); VC arbiter is `rr_arb_tree`-style with `LockIn=1`; drop on route_par mismatch with no AXI synthesis; on flit_ecc double-bit forward corrupted with `OKAY`; R-RoB flop array at `MAX_TXNS≤32`, SRAM macro at `MAX_TXNS≥64`; `NMU_BUFFER_DEPTH=2` per VC; `axi_rdata_par_o` regenerated post-flit_ecc-check; `quiesce_idle` combinational over latched aclk; RoB allocator lowest-FREE-index-first; SAM compile-time parameter, first-match-wins.

**Pipeline / timing**: `noc_clk_i` 1.2 GHz, `aclk_i` 800 MHz, ASIC 7nm. CUT_AX=0/1 → 1/2 cycles, CUT_RSP=0/1 → 1/2 cycles. CDC: gray ptr + 2FF, depth 16. flit_ecc decode registered. route_par combinational drop.

**Round 1 ambiguities raised** (9):

1. **Flit format / field offsets** not in `spec/ni/` (in `noc-sim/docs/design/packet_format.md`).
2. **NSU MetaBuffer indexing** — slot vs `axi_id`-keyed vs rob_idx-mirrored; tentative: slot-keyed, `(rob_idx, src_id)` lookup.
3. **`NMU_BUFFER_DEPTH=2` vs `CDC_FIFO_DEPTH=16`** relationship; tentative: different stages.
4. **Hamming vs Hsiao SECDED**; tentative: Hsiao (per dv/plan + test-knob references).
5. **`NOC_VC_MAPPING_HYBRID_RW_QOS` exact mapping function** for NUM_VC=4/8; tentative: bit-extract qos[3] for NUM_VC=4, qos[3:2] for NUM_VC=8.
6. **Address parity regeneration when NMU rewrites address** — definition of "modify" ambiguous; tentative: byte-by-byte differing regenerated.
7. **`rob_req` decision policy** — `NOC_FLIT_HDR_ROB_REQ_GEN` doesn't specify; tentative: tied to `R_ROB_TYPE`/`B_ROB_TYPE`.
8. **CDC `flush_on_full_reset` trigger condition** — "both resets eventually align" undefined; tentative: 2FF-sync sticky observation.
9. **`axi_rready_o = 1` "always-ready while in reset to drain"** — what happens to slave-driven B/R during reset; tentative: lost.

**Wire-level lockdowns proposed**: 10 specific behaviors for cross-team confirmation (flit_ecc encode bit pattern; route_par polarity; vc_id mapping function; wormhole-lock release cycle; credit-init epoch; PENDING counter timing; EXCLUSIVE_MONITOR_CTRL.clear_all race; LAST_ERR_INFO sticky update; reset-during-burst behavior; axi_rdata_par_o regeneration on 2-bit ECC error).

## Round 2 — cross-paradigm peer review

Each agent received the OTHER agent's Round 1 output and produced a peer review. Full transcripts in conversation log (2026-05-10).

### `c-bfm` reviewing `rtl` — summary

**Agreement** (7 points): CSR + IRQ at ni_top; wormhole arbiter (LockIn=1); route_par drop semantics; (B)-philosophy on flit_ecc double-bit; rob_req=0 still allocates tracker; lowest-FREE-index allocator; NMU_BUFFER_DEPTH=2.

**Disagreement** (3 points):

- "axi_rdata_par_o regenerated" — wording understates; per `theory_of_operation.md:398` it's *generated*, not regenerated. Cosmetic.
- "quiesce_idle combinational over aclk values, no CDC" — would report idle while flit is mid-CDC; recommend adding 2FF-sync noc_clk-side empty AND. **Material concern.**
- "axi_rready_o always-ready in reset" — re-read `pin_level_reset.md:432`; it should be 0 during reset for axi_*ready_o, not 1. RTL agent's interpretation may be wrong.

**New ambiguities surfaced** (5):

- **Credit-init epoch handshake exact cycle** — same-cycle vs next-cycle seed.
- **MetaBuffer indexing key** — slot vs `axi_id`-keyed vs `(rob_idx, src_id)` lookup tuple. C-bfm leans toward `(rob_idx, src_id)` because rob_idx alone is not unique across sources.
- **`CUT_AX`/`CUT_RSP` spill register placement** — pre-pack vs post-pack vs pre-CDC.
- **`prev_dest` adaptive-bypass arming/disarming exact cycle** — at AW issue, at flit-pack, or at allocation?
- **`R_ROB_TYPE=NoRoB` ordering trust assumption** — `NOC_FLIT_INORDER_PER_VC` only guarantees same-`(src_id, dst_id, vc_id)`; if NMU's VC mapping spreads same-`(src,dst)` traffic across VCs by qos, NoRoB silently breaks ordering. **NEW DISCOVERY.**

### `rtl` reviewing `c-bfm` — summary

**Agreement** (6 points): two-domain decomposition; wormhole arbiter LockIn until last & accept; RoB lowest-FREE / lower-rob_idx / prev_dest; CSR access semantics; test-only knobs have no RTL counterpart; PASSIVE drives during-reset values except `irq_o`.

**Disagreement** (3 points):

- **`axi_rready_o = 0` to back-pressure on R-buffer-full** — what spec says (`theory_of_operation.md:100`) and BFM should match. But `pin_level_reset.md:107,311,422` says `=1` "always-ready to drain". Genuine spec contradiction; needs erratum.
- **Hsiao vs Hamming** — RTL agent votes Hamming (3 of 4 spec sites use it; one Hsiao mention is typo). **Direct disagreement with c-bfm vote.**
- **CUT_AX=0 latency "T+1 not T combinational"** — agree on T+1, but spec phrasing "combinational pack + immediate inject" is misleading; worth tightening.

**New ambiguities surfaced** (5):

- **CDC FIFO depth and gray-code pointer width** unspecified beyond "synthesis-time parameter".
- **`CUT_AX` / `CUT_RSP` defaults** — both `bool, false` per signal_interface.md but never explicitly noted to flow through latency table.
- **`prev_dest[axi_id]` reset value + same-cycle update collision** — when AW + AR with same axi_id arrive same cycle, who writes prev_dest first?
- **AW + AR same-cycle RoB allocation order** — confirms designer ruling needed (RTL convention is AW-first).
- **`NMU_BUFFER_DEPTH=2`** documented but no equivalent NMU W-side buffer depth — which buffer absorbs W-burst back-pressure when noc_req credit exhausted?

## Synthesis

### Converged ambiguity list (ranked)

| Rank | Issue | Both flagged? | Affected wire/cycle behavior | Proposed resolution |
|------|-------|---------------|------------------------------|---------------------|
| 1 | Flit bit-layout (`packet_format.md` outside `spec/ni/`) | Both R1 #1 | Every flit on `noc_*_flit_o` | Publish `flit_layout.h` (SV pkg + C struct) checked into `spec/ni/`, version-locked |
| 2 | Hamming vs Hsiao SECDED | Both R1, **R2 disagree on which** | Every bit of `flit_ecc` | **Designer ruling** — spec self-contradicts; one side must change (3 sites Hamming vs 1 Hsiao) |
| 3 | `(R/W, qos) → vc_id` mapping function | RTL R1 #5; both R2 | Which VC each flit lands on (NUM_VC≥4) | Publish full `(R/W, qos[3:0], NUM_VC) → vc_id` table (not formula) |
| 4 | Credit-init epoch exact cycle | Both R1 wire-level lockdowns; C R2 expansion | Link bring-up timing + credit accounting | Seed counters cycle both `*_credit_init_ready_*` first concurrently HIGH; first injectable cycle = next |
| 5 | NSU MetaBuffer indexing | RTL R1 #2; C R2 expansion | NSU response packing correctness | Slot-keyed allocation, `(rob_idx, src_id)` lookup tuple |
| 6 | **NoRoB + Hybrid VC mapping ordering bug** | C R2 only — **NEW DISCOVERY** | Same-AXI-ID OoO across VCs violates AXI4 | NoRoB must force single-VC pinning OR `NOC_FLIT_INORDER_PER_VC` must be widened to `(src_id, dst_id)` only |
| 7 | CDC `flush_on_full_reset` trigger | RTL R1 #8 | Post-partial-reset link recovery | 2FF-sync sticky bit per side observing other's reset event; flush when both observe alignment |
| 8 | `CUT_AX` / `CUT_RSP` spill register placement | C R2 only | AXI-side observable latency | Pre-pack on AX, post-unpack on RSP; document in ToO §RTL pipeline |
| 9 | Same-cycle AW + AR allocation order | RTL R2 expansion | RoB index assignment + prev_dest evolution | Designer ruling: AW takes priority over AR same cycle |
| 10 | **`axi_rready_o` reset/operational contradiction** | RTL R2 only — **NEW SPEC CONTRADICTION** | R-channel back-pressure observable | Spec erratum: `pin_level_reset.md` row reads "0 during reset, 1 in steady-state, 0 when NSU R-buffer full" |
| 11 | ECC counter widths | C R1 #4; partial RTL R2 | ERR_STATUS register field placement | All four counters = `ERR_COUNTER_WIDTH=16` in `[15:0]`, `[31:16]` Reserved |
| 12 | **`quiesce_idle` missing NoC-domain empty AND** | C R2 only — **NEW DISCOVERY** | Reports idle while flit is mid-CDC | Add 2FF-sync noc_clk-side empty indicator AND |
| 13 | Wormhole RR pointer reset value + advancement rule | C R1 #5; RTL R2 expansion | First-cycle arbitration + multi-VC test reproducibility | Resets to 0; advances by 1 per granted **packet** (not per flit) |
| 14 | `prev_dest` arming/disarming + same-cycle update | C R2 + RTL R2 | NormalRoB fast-path correctness | Sample at allocation; same-cycle AW+AR resolved by AW-first rule |

### NEW DISCOVERIES (Round 2 only)

- **#6 NoRoB + Hybrid VC mapping ordering bug** (C-agent R2). Reader test could not have caught this — it required cross-paradigm reasoning about implementations.
- **#10 `axi_rready_o` reset/operational contradiction** (RTL R2). Spec self-contradicts across `pin_level_reset.md` and `theory_of_operation.md:100`.
- **#12 `quiesce_idle` missing NoC-domain empty AND** (C-agent R2). Operational correctness issue masked by spec staying purely aclk-domain in the formula.

### DISAGREEMENTS (need designer ruling)

| Issue | `c-bfm` position | `rtl` position |
|-------|------------------|----------------|
| Hsiao vs Hamming SECDED | Hsiao (dv/plan and test-knob authority; standard Hsiao SECDED) | Hamming (3 of 4 spec sites; transaction_api.md mention is typo) |
| `axi_rready_o` reset value (1) | spec says 1 (`pin_level_reset.md` for-real value) | should be erratum to 0; current spec text contradicts ToO §100 |

### SPEC CONTRADICTIONS (1-line erratum class)

- **Hamming vs Hsiao**: `theory_of_operation.md:351,493`, `protocol_rules.md NOC_FLIT_HDR_FLIT_ECC_GEN` say Hamming. `protocol_rules.md NI_CFG_INJECT_ECC_ERROR` (test-knob row) says Hsiao. Resolution requires designer ruling first.
- **`axi_rready_o` value**: `pin_level_reset.md` rows for AW_OUT / R_OUT say `=1`. `theory_of_operation.md:100` operational text says NSU drives `=0` when buffer full. Erratum: pin_level_reset.md row should clarify "post-reset steady-state default; operational value depends on R-buffer occupancy".

## Verdict

- `D1.cross.implementer_review`: ✓ ran successfully. 14-item converged ambiguity list produced; 3 NEW DISCOVERIES surfaced by Round 2 cross-review beyond Round 1 independent reads.
- Resolutions required before D2:
  - **2 items** flagged for designer ruling (Hsiao vs Hamming; AW+AR same-cycle allocation order).
  - **2 items** as 1-line spec erratum (`axi_rready_o` reset/operational text; Hamming wording in 3 spec sites if Hsiao is chosen).
  - **10 items** deferred to combined D1→D2 transition planning meeting (flit_layout.h publish, vc_id mapping table, credit-init epoch, MetaBuffer indexing, CDC flush algorithm, CUT_AX/CUT_RSP placement, RR pointer reset, ECC counter widths, prev_dest arming, NoRoB single-VC pinning).

The 14 items must be resolved before either C-model or RTL implementation work proceeds at D2, or each ambiguity will encode a different assumption in the two implementations and bit-equivalence testing will surface them as silent divergence.

## Resolution log

Updates to the 14-item list as items are closed by designer ruling, spec erratum, or deferral. The table above is the snapshot at run time. This section is the audit trail.

### 2026-05-10 — Bucket 1 mechanical erratum (closed)

- **#10 `axi_rready_o` reset/operational contradiction**: closed via `pin_level_reset.md` §Post-reset transitions edit. Previous "held at 1 (always-ready to drain)" grouped row was a defect inherited from the pre-LINT-BFM-001 grouped-table form. Now split: `axi_bready_o` truly always-1 (B has no NSU back-pressure path); `axi_rready_o` 1 by default but drops to 0 when NSU R-buffer is full per ToO §NSU Read response buffer.
- **#11 ECC counter widths**: closed via `registers.md` edit — added `ERR_COUNTER_WIDTH bits` to the three counter rows (`ECC_UNCORR_ERR_CNT`, `ROUTE_PAR_ERR_CNT`, `AXI_PARITY_ERR_CNT`) that previously stated only "saturating". `ECC_CORR_ERR_CNT` was already correct.

### 2026-05-10 — Bucket 2 #2 Hsiao vs Hamming SECDED (closed by designer ruling)

**Designer ruling**: **Hsiao SECDED**.

**Rationale**:

- The spec requires SECDED ECC across the entire flit — generic, does not specify matrix variant. Aligning with Hsiao is consistent with this.
- Hsiao has marginal technical advantages: equal column weight, one fewer XOR gate level in decode, slightly better double-bit detection probability.
- Spec author's original intent (per `dv/plan.md` and `protocol_rules.md NI_CFG_INJECT_ECC_ERROR`) was Hsiao. The "Hamming" wording in three other spec sites was inadvertent.

**Spec edits applied** (6 matrix-variant + 2 mathematical):

- `theory_of_operation.md:351` — "SECDED Hamming code" → "SECDED Hsiao code"
- `theory_of_operation.md:493` — "whole-flit SECDED Hamming over flit" → "whole-flit SECDED Hsiao over flit"
- `theory_of_operation.md:527` — RTL-vs-BFM equivalence row "whole-flit SECDED Hamming" → "whole-flit SECDED Hsiao"
- `protocol_rules.md:206` — prose "whole-flit SECDED Hamming" → "whole-flit SECDED Hsiao"
- `protocol_rules.md:212` — `NOC_FLIT_HDR_FLIT_ECC_GEN` rule body "Compute SECDED Hamming code" → "Compute SECDED Hsiao code"
- `dv/plan.md:123` — "flit_ecc SECDED Hamming gen + check" → "flit_ecc SECDED Hsiao gen + check"
- `signal_interface.md:303` — "SECDED Hamming bound" → "SECDED bit-count bound", with parenthetical noting bound is matrix-variant-agnostic
- `theory_of_operation.md:353` — "Derivation: Hamming SEC over `k` data bits" → "Derivation: a SEC (single-error-correcting) code over `k` data bits", plus added note that the bound applies to both Hsiao and classical Hamming SECDED

`protocol_rules.md:295` (`NI_CFG_INJECT_ECC_ERROR` test-knob row) already said Hsiao; no change required. The Round 2 disagreement between c-bfm (Hsiao) and rtl (Hamming) is closed in favour of c-bfm's reading.

**Implementation note for both teams**: pick a published Hsiao parity-check matrix for `FLIT_DATA_WIDTH=396, FLIT_ECC_WIDTH=10`. Use an open-source Hsiao SECDED generator (e.g., the `secded_gen.py` family) to produce the H-matrix for the `(396, 10)` tuple. Both implementations must consume the same H-matrix or wire-equivalence fails.

### 2026-05-10 — Bucket 2 #9 same-cycle AW+AR allocation order (closed by designer ruling)

**Designer ruling**: **fair round-robin, no W/R bias**.

**Rationale**:

- Fair round-robin arbitration (`rr_arb_tree` with `ExtPrio=0, LockIn=1, FairArb=1`) — pure fair round-robin between AxiAw, AxiW, AxiAr inputs.
- AW-first or AR-first would create long-term bias against the deferred channel, harming average latency for the deferred class.
- Standard `rr_arb_tree` (~50-line module) implements the policy; both BFM and RTL can consume the same reference.

**Spec edit applied**:

- `theory_of_operation.md` §RoB allocator: added new "Tie-breaking when AW and AR arrive in the same cycle" paragraph stating fair round-robin policy, marked "Designer-confirmed (D1→D2 ambiguity triage, 2026-05-10)".

**Implementation note**: both BFM and RTL teams use `rr_arb_tree` with `ExtPrio=0, LockIn=1, FairArb=1` parameters. Round-robin pointer state at reset must agree (typically pointer = 0 post-reset, advances on each granted packet).

### 2026-05-10 — Bucket 2 #12 quiesce_idle CDC drain (closed as false positive + clarification added)

**Re-examination ruling**: **spec is correct as-is. Round 2 reviewer's concern was based on misreading PENDING counter semantics.**

**Re-analysis**:

- C-bfm Round 2 reviewer claimed `quiesce_idle` could assert while a request flit is mid-CDC.
- The claim implicitly assumed `PENDING_W` decrements when the request flit enters the CDC FIFO. This is wrong.
- Spec (`theory_of_operation.md` §Software quiesce flow) defines: `PENDING_W` decrements only when AXI master receives `B` (the response handshake at `axi_*_i`).
- `B` reaching master requires the full round-trip: master → NMU AXI → CDC FIFO → NoC → NSU → slave → NSU → NoC → CDC → NMU → master.
- Therefore `PENDING_W = 0` implies every CDC FIFO (request-side AND response-side) has fully drained for previously-issued writes. Same argument for reads.
- `quiesce_idle = 1` therefore implicitly covers full CDC drain — no separate `noc_clk`-domain empty indicator needed.

**Spec edit applied** (clarification, not a behavioral fix):

- `theory_of_operation.md` §Software quiesce flow: added a paragraph after the PENDING counter definition explicitly walking through why `PENDING_W = 0` guarantees CDC drain. The reasoning was implicit before; the new paragraph makes it explicit so future reviewers do not raise the same false positive.

No external NI reference implements the quiesce / drain mechanism — our use case is spec-specific (CSR-driven quiesce is a design choice, not borrowed from a prior art).

### 2026-05-10 — Bucket 2 #6 NoRoB + Hybrid VC mapping ordering bug (closed by designer ruling)

**Designer ruling**: **NoRoB and `NUM_VC > 1` mutually exclusive** (option (d) — elaborate-time hard constraint).

**Bug summary**:

- NoRoB assumes NoC preserves response order.
- NoC actually only guarantees same-`(src_id, dst_id, vc_id)` order (`NOC_FLIT_INORDER_PER_VC`).
- Hybrid R/W × QoS VC-mapping routes same-`axi_id` traffic to different VCs when `qos` varies.
- Different VCs have no inter-VC ordering → responses can return OoO.
- NoRoB has no reorder buffer → AXI4 same-ID-ordering silently violated.

**Research context**:

- A pure physical-channel NI design (separate req/rsp links, no VC concept) has no VC ordering concern — AXI sub-channel arbitration via wormhole arbiter handles ordering.
- An alternative to option (d) is pinning NoRoB traffic to a fixed VC (option a), but this adds NMU mux logic.
- Physical-channel architecture ("routing resources are plentiful") is a v0.5.0+ candidate.

**Why (d) over (a)**: NoRoB users typically want simple deployment. Combining NoRoB (smallest area) with multi-VC (qos-tier separation) is a contradictory configuration. (d) hard-constrains the combination at elaborate time, simpler than (a)'s NMU mux logic. (a) would still be valid but adds complexity for a use case that doesn't exist in practice.

**Why not (e)**: (e) was the "switch entire NoC to physical channel architecture" option. That is a v0.5.0+ architectural redesign, out of scope for this round.

**Spec edits applied**:

- `signal_interface.md` §Parameters `NUM_VC` row: Constraint column extended to `1 ≤ x ≤ 8; when x > 1, both R_ROB_TYPE and B_ROB_TYPE MUST be != NoRoB (see ToO §RoB allocator §"NoRoB single-VC restriction")`.
- `theory_of_operation.md` §RoB allocator: new "NoRoB single-VC restriction" sub-section after the RoB variants bullets, explaining the constraint, its rationale, and the elaborate-time `ASSERT_INIT` enforcement. Marked "Designer-confirmed (D1→D2 ambiguity triage, 2026-05-10)".

**Implementation note**: both BFM and RTL elaborate-time check `ASSERT_INIT(NumVcNoRoBExclusive, NUM_VC == 1 || (R_ROB_TYPE != NoRoB && B_ROB_TYPE != NoRoB))`. Integrators selecting NoRoB and `NUM_VC > 1` simultaneously will fail elaboration; spec error message should direct them to either `SimpleRoB`/`NormalRoB` or `NUM_VC=1`.

**v0.5.0 candidate**: re-evaluate whole architecture toward physical-channel direction (option (e)). Tracked in `plan/DOGFOOD_OBSERVATIONS_A5.md`.

### 2026-05-10 — Bucket 2 extension: VC partition table architectural inconsistency (closed)

**Issue surfaced during #6 walkthrough** (not in original Round 1+2 list, but follows the same architectural concern):

`theory_of_operation.md` §VC Mapping had a partition policy table:

```
| NUM_VC | Request VCs | Response VCs |
| 1      | VC[0] shared | VC[0] shared |
| 2      | VC[0]        | VC[1]        |
| 4      | VC[0..1]     | VC[2..3]     |
| 8      | VC[0..3]     | VC[4..7]     |
```

implying VCs are partitioned across `noc_req` and `noc_rsp` from a single shared NUM_VC pool. This contradicted `signal_interface.md` which defines `noc_req_credit_i[NUM_VC-1:0]` and `noc_rsp_credit_i[NUM_VC-1:0]` as separate per-physical-channel credit arrays — i.e., each physical channel has its own independent NUM_VC VC pool.

**Designer ruling (during walkthrough)**: each physical channel has independent NUM_VC VCs. Within each physical channel, VCs are partitioned by QoS tier. There is no shared VC numbering across `noc_req` and `noc_rsp`.

**Spec edits applied**:

- `theory_of_operation.md` §VC Mapping: full rewrite. Removed the "R/W subset" framing (R/W is determined by which physical channel a flit goes on, not by VC subsetting). New partition table describes per-physical-channel QoS-tier partition. New preamble explicitly states "Each physical channel's VC pool is independent — there is no shared VC numbering across `noc_req` and `noc_rsp`."

- `protocol_rules.md` `NOC_VC_PARTITION` (RECOMMEND severity rule): rewrote rule body to describe per-physical-channel QoS-tier partition. The two physical channels carry the same partition independently.

- `protocol_rules.md` `NOC_VC_MAPPING_HYBRID_RW_QOS` (FAIL severity rule): rewrote rule body. `vc_id` is assigned by QoS-tier mapping within each physical channel. Mapping is a function of `qos` only (physical channel routing is determined separately). Added concrete bit-extract examples (`qos[3]` for NUM_VC=2, `qos[3:2]` for NUM_VC=4, `qos[3:1]` for NUM_VC=8). The "R/W" in the rule name preserved (rules are stable identifiers per CLAUDE.md), reinterpreted as "request and response see different physical-channel VC pools" not "R/W is part of qos→vc_id function".

- `SLIDES.zh-TW.md` Slide 5: rewrote NUM_VC partition table to match per-physical-channel framing (the original slide table inherited the "Request VCs / Response VCs" mistake from ToO).

**Why this matters**: this was a latent architectural inconsistency that both Round 1 implementer-review agents read but did not flag. They each accepted the partition table as-is without cross-checking signal_interface. This is a class of ambiguity neither reader-test nor single-paradigm implementer-review catches — it requires architectural cross-checking across multiple spec files. Surfaced here only because the user noticed during #6 walkthrough that "credit is per-VC but flit data line is shared".

**Plugin lesson**: future implementer-review prompts should explicitly ask each agent to spot-check architectural consistency across `signal_interface.md` and `theory_of_operation.md` (or equivalent files). Tracked in `plan/DOGFOOD_OBSERVATIONS_A5.md` for future plugin enhancement.

### 2026-05-10 — Bucket 3 #13 wormhole RR pointer reset + advancement (closed)

**Designer ruling**: pointer resets to 0 on `noc_rst_ni`. Pointer advances by 1 on the cycle the packet's `last=1` flit is granted (per-packet, not per-flit). Pointer held fixed during the lock interval.

Wormhole arbiter uses `rr_arb_tree` with `LockIn=1, FairArb=1, ExtPrio=0`. Reset and advancement semantics inherit from the standard `rr_arb_tree` behavior. The same arbiter pattern is used at two places in the NMU: (a) AW/W/AR request output arbiter (per #9 ruling), (b) per-VC wormhole arbiter on each physical channel (this item).

**Spec edit applied**:

- `theory_of_operation.md` §VC Mapping §"2. Wormhole arbiter": appended pointer-state semantics (reset value, per-packet advancement rule, lock-interval behavior). Cross-link to §RoB allocator (where the same arbiter pattern was specified for AW/W/AR tie-break in #9).

### 2026-05-10 — Bucket 3 #5 NSU MetaBuffer indexing (closed)

**Designer ruling**: index by master `axi_id` (`id_queue` pattern — per-ID FIFOs, each unique AXI ID has its own ordering chain). System-level integrator constraint: AXI IDs are guaranteed non-overlapping across multi-NMU traffic to the same NSU.

**Research context**: `id_queue` is essentially per-ID FIFOs keyed by AXI ID. Cross-ID transactions are independent. An offset for atomic-transaction ID space management (`MaxAtomicTxns`) is optional; for non-atomic, offset is 0. Multi-NMU AXI ID collision is not handled at the NI level — disambiguation is the integrator's responsibility.

**Why option (α) over (β) slot-direct-lookup or (γ) composite key**:

- (α) is the simplest.
- (β) and (γ) addressed multi-NMU AXI ID collisions, which the designer ruling now explicitly forbids at system level.
- Pushing the disambiguation to integrator keeps NI scope minimal.

**Spec edit applied**:

- `theory_of_operation.md` §MetaBuffer: added new §Indexing paragraph specifying axi_id-keyed `id_queue` indexing and the system-level integrator constraint that AXI IDs MUST be non-overlapping across multi-NMU traffic to any given NSU.

### 2026-05-10 — Bucket 3 #8 CUT_AX / CUT_RSP spill register placement (closed)

**Designer ruling**: `CUT_AX=1` spills at AXI ingress (pre-AddrTrans). `CUT_RSP=1` spills at AXI egress (post-FlitUnpack). Symmetric placement.

**Rationale**: standard pattern: spill at the boundary between AXI master and the internal pipeline (AddrTrans + QoSGen + FlitPack chain). RSP placement is symmetric — spill at the boundary between internal pipeline (FlitUnpack + RoB release) and AXI master B/R handshake.

**Spec edit applied**:

- `theory_of_operation.md` §RTL pipeline / timing: added new "`CUT_AX` / `CUT_RSP` spill register placement" sub-section explicitly stating the placement.

### 2026-05-10 — Bucket 3 #14 prev_dest arming / disarming (closed)

**Designer ruling**: write `prev_dest[axi_id]` unconditionally on every AW / AR push. Bypass decision uses previous-cycle value (`prev_dest_q`); next-cycle value (`prev_dest_d`) updated after the decision. `prev_dest` never explicitly cleared.

**Implementation detail** (prev_dest adaptive bypass):

- `prev_dest_d[ax_id_i] = ax_dest_i` on every push. Unconditional update.
- Bypass logic uses `prev_dest_q` (current cycle, pre-update) compared against new `ax_dest_i`. Three-way decision: empty queue → `rob_req=0`; non-empty + same-dst → `rob_req=0` (fast-path); else → `rob_req=1` (start reorder).
- Only `ax_rob_req_q[id]` clears when the per-id queue empties. `prev_dest_q[id]` retains its last value (no explicit clear path; stale data is harmless because bypass logic gates on queue occupancy first).

**Race analysis**: `prev_dest_q` (read-side) and `prev_dest_d` (write-side) are separate FF stages within the same cycle. Combinational logic reads `prev_dest_q` for bypass decision, then computes `prev_dest_d` for next cycle. Read-before-write within cycle is intrinsic to the FF model. No race.

**Spec edit applied**:

- `theory_of_operation.md` §RoB allocator §`prev_dest` adaptive bypass: appended new "`prev_dest` arming and disarming" sub-section detailing the unconditional update, race-free read-before-write semantics, and no-explicit-clear behavior.

### 2026-05-10 — Bucket 3 #3 vc_id mapping default function (closed)

**Designer ruling**: default mapping is **magnitude-tier division**. Split the 16-value `qos[3:0]` space into `NUM_VC` equal-sized tiers by magnitude; tier number = `vc_id`. High-qos flits land on high-numbered VCs (monotonic, priority-preserving). Compact form: `vc_id = qos >> (4 - log2(NUM_VC))`.

**Rationale**:

- Magnitude-tier preserves priority isolation: high-qos flits share a VC, low-qos flits share a separate VC, no cross-contention for credits within a tier.
- Monotonic mapping is intuitive: integrator can read "qos increases → vc_id increases" without consulting the bit-extract spec.
- Mathematically equivalent to high-bit extract for power-of-2 NUM_VC (qos[3] for NUM_VC=2, qos[3:2] for NUM_VC=4, qos[3:1] for NUM_VC=8) but the magnitude framing is more intuitive in spec prose.

**Anti-patterns explicitly rejected in spec**:

- `vc_id = qos % NUM_VC` (modulo): scatters same-priority flits across VCs, defeats isolation. e.g., qos=8 and qos=12 are both high-priority but land on different VCs.
- Low-bit extract `qos[log2(NUM_VC)-1:0]`: same scattering problem.

This QoS-tier mapping is an original design choice. The designer pushed back on "high-bit extract" framing — observed that it's mathematically equivalent to "compare overall magnitude" and the magnitude framing is more intuitive. Spec text reframed accordingly.

**Spec edit applied**:

- `protocol_rules.md` `NOC_VC_MAPPING_HYBRID_RW_QOS` rule body: replaced "exact qos bit-extract function (e.g., qos[3] for NUM_VC=2 ...) is integrator-configurable; the default mapping is the BFM-modeled function" with a concrete magnitude-tier specification + anti-patterns list. The BFM-modeled default is now spec-pinned (not just example).
- `theory_of_operation.md` §VC Mapping: updated cross-reference to use the magnitude-tier phrasing instead of bit-extract framing.

### 2026-05-10 — Bucket 3 #4 credit-init epoch precise cycle (closed)

**Designer ruling**: 3-cycle sequence pinned. T = first cycle both `*_credit_init_ready_o` and `*_credit_init_ready_i` sampled HIGH. T+1 = credit counter seed updates. T+2 = earliest `noc_*_valid_o = 1`.

**Rationale**: follows from the fully-registered output discipline (per ToO §Driver). Every output is a flop. Each condition observed on cycle X becomes a driven signal on cycle X+1. The 3-step pipeline (handshake-observed → credit-seeded → valid-driven) is a direct consequence, not a separate design choice.

The bi-directional `credit_init_ready` handshake is a spec-specific design choice. The precise cycle pinning is our designer ruling.

**Spec edit applied**:

- `signal_interface.md` §NoC credit signals §Behaviour summary: added a new bullet "Credit-init handshake precise cycle timing" walking through cycle T / T+1 / T+2 explicitly, with rationale referencing the fully-registered output discipline.

### 2026-05-10 — Bucket 3 #7 CDC flush_on_full_reset algorithm (closed by spec retraction)

**Designer ruling**: retract the `flush_on_full_reset` NI-level mechanism. No automatic partial-reset recovery is implemented. Cross-domain partial-reset recovery is integrator responsibility.

**Why the original spec text was wrong**: claimed `flush_on_full_reset` mechanism without specifying the algorithm (trigger condition, post-flush state, "align" definition). C-bfm Round 2 reviewer flagged this — the claim was an unverifiable promise.

**Spec edit applied**:

- `pin_level_reset.md` §CDC FIFO reset: full rewrite. Uses `cdc_fifo_gray`-style semantics (gray-counter pointer, no NI-level flush). Cross-domain partial-reset is explicitly stated as not-automatically-recoverable. Integrator responsibility is stated with two acceptable coordination patterns (simultaneous reset hold, or system-level coordination signal). The earlier `flush_on_full_reset` wording is explicitly retracted with rationale.

### 2026-05-10 — Bucket 3 #1 Flit bit-layout (closed by vendoring packet_format.md into spec/ni/doc/)

**Designer ruling**: vendor `packet_format.md` from `noc-sim/docs/design/packet_format.md` into `spec/ni/doc/packet_format.md`. The flit-format authoritative reference now lives inside the spec tree.

**Rationale**: both Round 1 implementer-review agents flagged this as the #1 highest-priority ambiguity. The flit bit-layout is the contract every implementation must agree on bit-for-bit, but it lived in a separate file outside `spec/ni/`. Either implementer reading just `spec/ni/` would lack the authoritative bit layout. Two options were considered:

- (a) Publish a `flit_layout.h` header file (SV pkg + C struct) checked into `spec/ni/`.
- (b) Vendor the existing `packet_format.md` from `docs/design/` into `spec/ni/doc/`.

Option (b) chosen because:
- `packet_format.md` (614 lines, 37 KB) already contains the authoritative bit-layout from the v0.4.0 flit format restructure. Re-publishing as a header would duplicate the source of truth.
- Per `memory/project_noc_sim_design_doc_priority.md`, only `packet_format.md` is the authoritative design doc going forward. Bringing it into the spec tree aligns with that priority decision.
- Cross-references from `theory_of_operation.md`, `signal_interface.md`, `pin_level_reset.md`, `channel_handshake.md`, `protocol_rules.md` now resolve to a local file inside `spec/ni/doc/`, removing the cross-repo dependency.

**Spec edits applied**:

- Copied `noc-sim/docs/design/packet_format.md` → `noc-sim/spec/ni/doc/packet_format.md` (614 lines, identical content).
- Mirror updated: `hw-spec-author-plugin/dogfood/noc-sim-ni-bfm/doc/packet_format.md` synced.
- `theory_of_operation.md`: updated 2 explicit cross-repo paths (`docs/design/packet_format.md §ECC` and `docs/design/packet_format.md §3.6`) → `packet_format.md §ECC` / `packet_format.md §3.6` (local).
- `signal_interface.md` line 5: updated "See `packet_format.md` in noc-sim source repo for flit format details" → "See `./packet_format.md` for flit format details (vendored from `noc-sim/docs/design/packet_format.md` per `IMPLEMENTER_REVIEW_LOG.md` #1 resolution)".

Other cross-refs in the spec that say just `packet_format.md §1.2` (no path) now resolve locally without text changes.

**Maintenance contract**: when `packet_format.md` updates upstream in `docs/design/`, the spec/ni/doc/ copy MUST be re-vendored. Tracked via simple `diff` of the two paths during D1+ gate reviews. Alternative for future: convert to a symlink (Windows-incompatible) or a build-time copy step in spec verification workflow.

**Bucket 3 status post-#1**: 8/8 ambiguities closed. D1→D2 ambiguity triage complete for the implementer-review surfaced items. Bucket 4 (architecture: BFM language, NMU/NSU shell evolution, DV scaffolding, coverage plan) remains, deferred to dedicated session.

### 2026-05-10 — Round 2 cross-paradigm peer review (post-Bucket-3 spec re-read)

After all Bucket 1/2/3 + partition-table + packet_format.md vendoring resolutions landed, a fresh Round 2 was run: each implementer agent (c-bfm, rtl) read the OTHER's Round 1 (the post-triage independent re-read) and produced a peer review. Plus `/spec-lint` was run for mechanical consistency cross-check.

**Lint result**: 0 blocking, 4 LINT-002 hygiene (TBD wording in vendored packet_format.md — fixed in-session).

**Round 2 cross-review NEW DISCOVERIES** (not in Round 1):

- **A. `packet_format.md` has 5 stale v0.3.0 SLVERR mentions** that contradict the (B)-philosophy expressed in ToO + protocol_rules + signal_interface. c-bfm Round 1 found 4; RTL Round 2 spot-check found a 5th (the R-flit specific paragraph at line 367). The vendoring did not scrub upstream's pre-v0.4.0 wording.
- **B. `INPUT_BUFFER_DEPTH` / `CREDIT_DELAY` undeclared**. After user clarification, these are router-side parameters; their absence from NI spec is correct. Router spec is not yet written. **Dropped from action list** as not-a-NI-spec-bug.
- **C. ATOP termination point ambiguous**. NMU local SLVERR synthesis vs NoC traversal then NSU termination. Wire-visible difference (`noc_req_o` carries ATOP flit or not).
- **D. `prev_dest_q` reset value undefined**. Reset to 0 collides with legitimate dst `(0,0)`. Mitigated by empty-queue first-push guard.
- **E. Hsiao H-matrix not pinned**. "Hsiao SECDED" is a family; specific matrix determines syndrome bits. BFM/RTL must consume identical matrix.

**Round 2 DISAGREEMENTS**:

- **F. NSU R buffer clock domain**. c-bfm Round 1 wrote "straddles aclk/noc_clk"; RTL Round 2 spot-checked ToO §96-108 and argued "purely aclk-domain". RTL's reading is supported by spec text (buffer "decouples local AXI slave's R-response timing from NoC injection back-pressure" — implies aclk-side accumulation before CDC). Designer ruling: aclk-only.
- **G. PENDING decrement point**. RTL Round 2 worried it might be ambiguous. Spot-check of `protocol_rules.md` `NI_CFG_PENDING_COUNT_ACCURACY` (line 297) shows the rule body already explicit: "decrements on B handshake completion at axi_*_i". Confirmed not actually ambiguous — RTL agent overlooked the rule body wording. No spec edit needed.

**Round 2 cross-review RESOLVED** (Round 1 ambiguities dropped after spot-check):

- RTL Round 1 #3 (B-payload over-pad): c-bfm Round 2 cited `packet_format.md:132/301` mandating zero-padding — resolved.
- RTL Round 1 #6 (PENDING width contradiction): c-bfm Round 2 cited `registers.md:140` "always within 32-bit register" — resolved.
- RTL Round 1 #4 (QOS_MODE reset disagreement): c-bfm spot-check found no actual disagreement — withdrawn.

### 2026-05-10 — Round 2 resolutions applied

**A. packet_format.md SLVERR scrub** (4 edit sites; the 5th at line 367 covered by the §3.4 edit):

- Line 88 (`ECC_FAIL_WIDTH` retired note): rewrote to state (B)-philosophy explicitly — uncorrectable ECC forwards flit as-is with `bresp=OKAY`, surfaces via CSR + IRQ.
- Lines 348-353 (W ECC uncorrectable description): replaced "直接以 `bresp=SLVERR` in-band 回報" with (B)-philosophy text matching ToO §ECC.
- Line 367 (R ECC uncorrectable description): same replacement for R-direction.
- Line 420 (ECC failures bullet): "2-bit→`bresp/rresp = SLVERR`" replaced with "forward as-is with `bresp/rresp = OKAY` ((B)-philosophy, no SLVERR synthesis)".

Upstream `noc-sim/docs/design/packet_format.md` synced to match.

**E. Hsiao H-matrix source-of-truth mandate**:

- `protocol_rules.md` `NOC_FLIT_HDR_FLIT_ECC_GEN` rule body extended: parity-check matrix MUST be a shared artifact between BFM and RTL. Use a Hsiao SECDED generator for the `(FLIT_DATA_WIDTH, FLIT_ECC_WIDTH)` tuple, with generated SV header checked into the project as single source of truth.

**C. ATOP termination point**:

- `theory_of_operation.md` §ATOPs scope: added explicit paragraph stating NMU locally synthesises B response with `bresp=SLVERR` when `axi_awatop_i != 0`; no AW or W flit injected on `noc_req_o`; downstream NSU never observes ATOP traffic.

**F. NSU R buffer clock domain**:

- `theory_of_operation.md` §NSU Read response buffer: added "Clock domain" paragraph explicitly stating the buffer is single-clock-domain (`aclk_i` only); R beats accumulate on AXI side, then pack into wide R flits in the same domain, then traverse the standard `aclk_i → noc_clk_i` CDC FIFO before injection.

**D. `prev_dest_q` reset value**:

- `theory_of_operation.md` §RoB allocator §`prev_dest` arming and disarming: added "Reset value" bullet stating `prev_dest_q[i] = 0` on `arst_ni`. Reset collision with legitimate dst `(0,0)` is harmless because the empty-queue first-push guard ignores `prev_dest_q` on the first push per axi_id.

**G. PENDING decrement point**: no edit needed. `protocol_rules.md` `NI_CFG_PENDING_COUNT_ACCURACY` (line 297) already explicitly states decrement happens at master B/R handshake at `axi_*_i`. Confirmed not actually ambiguous.

**Status after Round 2**: 14 original implementer-review ambiguities + 1 architectural partition issue + 6 Round 2 cross-review NEW DISCOVERIES = 21 items total triaged. Spec is internally consistent. Round 2 verifies that the Round 1 + triage pass produced a spec where two paradigm-paired implementers can produce wire-equivalent implementations on the load-bearing decisions (with the H-matrix source-of-truth artifact as the next concrete deliverable).
