# AXI4 Pattern Standardization — Research Brief

**Date:** 2026-06-07
**Status:** Research — informs next-round plan (numbering + naming + expansion)
**Branch:** stage5b/dpi-wire-wrap (currently at `1e5da80`)
**References:** ARM IHI 0022H AMBA AXI4 spec; industry VIPs per §6

---

## 1. Context

After Stage A+B (rename `cosim2 → cosim`; unify scenarios under
`tests/scenarios/<layer>/<name>/`), the pattern tree has 32 scenarios across
3 layer sub-dirs. Next-round goal is standardization (numbering + naming)
plus expansion (current coverage is too thin against AXI4 spec). This brief
records two parallel surveys — one Claude subagent, one Codex — on
industry AXI4 verification pattern conventions, to seed that plan.

Current scenarios at survey time:

```
tests/scenarios/common/
  single_write_read_aligned, burst_incr_2beat, burst_incr_8beat,
  multi_txn_same_id, multi_txn_diff_id, backpressure_retry,
  single_write_no_read

tests/scenarios/sv-cosim-only/
  debug_multi1, injection_aw_unstable, conformity_write_read,
  conformity_backpressure, multi_id_single_beat_sequential

tests/scenarios/c-model-only/
  exclusive_pair_success, exclusive_intervening_write,
  exclusive_no_prior_read, exclusive_wrap_pair_success,
  decerr_oob_read, decerr_oob_write, burst_crosses_oob_boundary,
  sparse_multibeat, single_read_default_fill,
  narrow_aligned_multibeat, narrow_transfer_size0, narrow_transfer_size2,
  unaligned_start, wrap_burst_aligned, wrap_burst_actual_wrap,
  fixed_burst, cross_4kb_auto_split, multi_dst_stress,
  multi_outstanding_stress, latency_stress
```

## 2. Convergent findings

Both surveys agree:

- **Industry does not number patterns at file level.** tim_axi4_vip (154
  tests, UVM), cocotbext-axi, pulp-platform/axi, OSVVM/AXI4, verilog-axi —
  all use descriptive snake_case names with no sequential ID. Numbering
  appears only in (a) testplan spreadsheets, (b) commercial VIP product
  guides (Synopsys VC, Cadence, AMD PG267) where it identifies API methods
  not test cases, or (c) requirements-traceability schemes.
- **Snake_case `<category>_<scenario>` is the dominant naming pattern.**
- **Parameter matrices (cocotbext-axi style) scale better than per-combination
  directories.** One named test sweeps width × idle-ratio × backpressure.
- **Convergent category taxonomy** (intersection across tim, pulp, OSVVM, IHI):
  basic, burst, alignment, ordering, exclusive, error, stress, fault-inject,
  sideband (QoS/USER), atomics (AXI4 → degenerate to exclusive only).

## 3. Divergent findings — user decisions for next round

### 3.1 To number or not

| Survey | Recommendation | Rationale |
|---|---|---|
| Claude | **No numbering**, descriptive names only | Industry < 150 patterns dominant; snake_case is discoverable; refactor-friendly |
| Codex | **`AX4-CAT-NNN_slug` stable IDs** (categories: BAS / HSH / BUR / STR / RSP / ORD / EXC / ATTR / RST / PERF) | IDs never renumber, never reuse on delete (tombstones), externalize requirements traceability as metadata |

The user-stated intent for next round is to add numbering. Codex's scheme
fits that intent directly. Open question: whether the cost of stable IDs
(tombstone discipline, separate slug-vs-ID renaming policy, mandatory
external requirements doc to derive value from the IDs) is justified at
the current ~30-pattern scale.

### 3.2 Layer sub-dirs vs. metadata tags

Surveyed projects do NOT use a consumer-routing sub-dir axis
(`common/sv-cosim-only/c-model-only/`). They split by content (category
or DUT). Consumer routing is expressed as metadata or filtered at test
discovery:

- cocotbext-axi: pytest markers (`@pytest.mark.slow`)
- OSVVM: `.pro` file aggregation
- tim_axi4_vip: regression list files

For this project, the equivalent is a `consumers:` field inside
`scenario.yaml`:

```yaml
id: AX4-BUR-003
category: burst
consumers: [c_model, sv_cosim]
requirements: [IHI22-A3.4.1]
wb2axip_safe: true
```

Adopting this would let `tests/scenarios/` become flat-by-category and
drop the layer dirs. Layer filtering moves into the test runner (gtest
parameter list filtered by reading metadata, or CMake-time selection).

## 4. Industry pattern-category taxonomy

Synthesized intersection of tim_axi4_vip, pulp-platform/axi, OSVVM/AXI4,
cocotbext-axi, AMD PG267:

| Category | Scope | Example sub-patterns |
|---|---|---|
| Basic transfer | single beat W/R, default-fill, RAW | `single_write_read`, `read_default_fill`, `read_after_write_same_addr` |
| Burst | INCR (1, 2, 8, 256), WRAP (2/4/8/16), FIXED | `incr_8beat`, `wrap_len16`, `fixed_burst` |
| Alignment / boundary | narrow, unaligned, 4KB edge, OOB | `narrow_size2`, `unaligned_start`, `4kb_boundary_exact_end` |
| Ordering | multi-ID, R interleave, B order, AW/W timing | `r_interleave_diff_id`, `b_order_same_id`, `aw_after_w` |
| Exclusive | pair success, intervening, no prior, wrap, monitor reset | `exclusive_pair_success`, `exclusive_monitor_reset` |
| Error response | SLVERR, DECERR, OOB | `slverr_read`, `decerr_oob_write` |
| Stress / concurrency | multi-outstanding, multi-dst, backpressure, latency | `multi_outstanding_8way`, `backpressure_retry` |
| Sideband | QoS, USER, REGION | `qos_priority_pair`, `user_field_passthrough` |
| Fault inject | protocol violation (SV negative tests) | `injection_aw_unstable` |
| Reset / handshake | reset behaviour, payload stability | `reset_during_outstanding`, `payload_stability_all_channels` |

## 5. Spec-traced gap matrix

Codex walked IHI 0022H section by section; covered vs missing summary:

| Spec section | Topic | Status |
|---|---|---|
| §A3.1 | Reset behaviour | ✗ missing |
| §A3.2 | VALID/READY handshake stalls | ✓ (covered by backpressure_retry) |
| §A3.2 | Payload stability while VALID && !READY | ✗ partial; only AW negative injection exists |
| §A3.3 | Channel relationships (AW/W independent) | ✗ missing |
| §A3.3 | B-after-AW-and-final-W | ✓ |
| §A3.4.1 | Burst length (AxLEN = 0, 1, 255) | ✗ partial; have 2, 8, no 1, no 256 |
| §A3.4.1 | Transfer size (AxSIZE 0..7) | ✗ partial; have 0, 2, 5 |
| §A3.4.1 | Burst type (FIXED, INCR, WRAP) | ✓ |
| §A3.4.1 | WRAP lengths {2, 4, 8, 16} sweep | ✗ partial; only aligned + actual_wrap |
| §A3.4.1 | Unaligned INCR start | ✓ |
| §A3.4.1 | 4KB boundary (exact-end, would-cross edge pair) | ✗ partial; have auto-split, missing edges |
| §A3.4.3 | WSTRB legality (full, sparse, zero, lane masks) | ✗ partial; only sparse_multibeat |
| §A3.4.3 | WLAST / RLAST placement | ✗ missing |
| §A3.4.5 | Response codes (OKAY, EXOKAY, SLVERR, DECERR) | ✗ partial; SLVERR missing |
| §A4 | Memory attributes (AxCACHE) | ✗ missing |
| §A4 | Protection (AxPROT) | ✗ missing |
| §A5 | ID-based outstanding traffic | ✓ |
| §A5 | BID / RID return correctness | ✓ |
| §A6 | Same-ID order under latency skew | ✗ missing |
| §A6 | Different-ID legal out-of-order completion | ✗ missing |
| §A7.2.4 | Exclusive access success / fail / intervening | ✓ |
| §A7.2.4 | Exclusive monitor reset clears reservation | ✗ missing |
| §A8 | QoS / REGION preservation and routing | ✗ missing |
| §A8 | USER signal preservation | ✗ missing |

## 6. Top patterns to add — intersected recommendations

Both surveys flagged these as critical (priority order = severity of gap):

| # | Suggested name (Claude scheme) | Codex ID | Spec | Complexity |
|---|---|---|---|---|
| 1 | `error/slverr_response` | AX4-RSP-001_slverr_read_write | §A3.4.5 | medium (Memory model returns SLVERR for marked range) |
| 2 | `boundary/4kb_edge_exact_end` + `4kb_edge_would_cross` | AX4-BUR-002_4kb_boundary_edges | §A3.4.1 | small |
| 3 | `burst/wrap_len2`, `wrap_len4`, `wrap_len16` | AX4-BUR-003_wrap_len_8_16_positions | §A3.4.1 | small |
| 4 | `ordering/r_interleave_diff_id` | AX4-ORD-001_diff_id_out_of_order | §A5 / §A6 | large (needs slave to delay R for low ID) |
| 5 | `ordering/aw_w_timing_independent` (3 sub-cases) | AX4-HSH-002_aw_w_arrival_permutations | §A3.3 | large (master timing override) |
| 6 | `exclusive/monitor_reset_clears` | AX4-EXC-005_monitor_reset | §A7.2.4 | medium |
| 7 | `burst/incr_len256` | AX4-BUR-001_incr_len_256 | §A3.4.1 | small |
| 8 | `ordering/b_order_same_id_under_skew` | AX4-ORD-002_same_id_order_under_skew | §A6 | large (slave latency-randomizer needed) |
| 9 | `basic/read_after_write_same_addr` | (Claude only) | tim Basic Transfers | small |
| 10 | `reset/idle_and_outstanding` | AX4-RST-001 | §A3.1 | large (Vtb_top needs mid-sim reset toggle) |

Small = within current YAML schema (`scenario_parser` accepts as-is).
Medium = needs minor scenario_parser extension (e.g. `slverr_address_range`).
Large = needs DUT-side support (e.g. randomized R latency, mid-sim reset).

## 7. What NOT to add

- **Multi-master arbitration / bus matrix** — Stage 5b is nmu/nsu boundary
  conformity, not fabric. Defer to multi-NSU integration stage.
- **CDC / clock-domain crossing** — no async-FIFO infrastructure.
- **AXI4 atomics (AtomicStore/Load/Swap/Compare)** — stripped from AXI4
  baseline; ACE/AXI5 territory.
- **Power-domain isolation / quiescence** — no power modelling.
- **REGION-based decode** — single-NSU decode map doesn't use REGION.
- **AXI3 features** — write-data interleaving, locked transactions; AXI4
  removed both.
- **Per-`AxLEN` or per-offset directories** — use parameter matrices instead
  (cocotbext-axi precedent).
- **Broad malformed-traffic regression** — keep early-WLAST,
  unstable-payload, illegal-WRAP cases in a dedicated negative-checker
  suite, not in main functional regression.

## 8. Sources

- Claude survey URLs:
  - tim_axi4_vip — UVM AXI4 VIP, 154 tests: https://github.com/moonslide/tim_axi4_vip
  - pulp-platform/axi: https://github.com/pulp-platform/axi/tree/master/test
  - cocotbext-axi: https://github.com/alexforencich/cocotbext-axi
  - OSVVM/AXI4: https://github.com/OSVVM/AXI4
  - chiselverify AXI4: https://github.com/chiselverify/chiselverify
  - muneebullashariff/axi4_vip testplan.xlsx: https://github.com/muneebullashariff/axi4_vip
  - AXI4 IHI 0022 (mirror): http://www.gstitt.ece.ufl.edu/courses/fall15/eel4720_5721/labs/refs/AXI4_specification.pdf
- Codex survey URLs:
  - AMD AXI VIP PG267: https://docs.amd.com/r/en-US/pg267-axi-vip
  - verilog-axi: https://github.com/alexforencich/verilog-axi
  - IHI 0022H spec: https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/IHI0022H_amba_axi_protocol_spec.pdf
- Repository state at survey time: `tests/scenarios/` @ `1e5da80`

## 9. Out of scope for this brief

- Concrete next-round plan: that belongs in a separate plan file
  (`docs/superpowers/plans/...`) once user decisions in §3 are settled.
- `scenario_parser` extension design (slverr range, mid-sim reset, R
  latency injection) — those are implementation tasks per added pattern,
  to be specced separately.
- Renaming current 32 scenarios to fit a new scheme — depends on §3.1
  decision (numbering or not).
