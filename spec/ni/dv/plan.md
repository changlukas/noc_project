# DV Plan

> 測試環境、I/O pattern（text-job）、pattern 產生、可重用 OSS、與 verification closure 框架見 `test_environment.md`。本文件聚焦 testpoint / coverage model / ABV-FPV 清單。

## Verification scope

Verify the NI against (primary testpoints in the right column):

| # | Verification target | Primary testpoints |
|---|---------------------|--------------------|
| 1 | AXI4 protocol compliance (both slave + master ports + AXI4-Lite CSR access) | TP1–TP4, TP52, TP53; AXI4-Lite CSR-port protocol via FVIP (AXI4LITE mode) bound to the CSR port — CSR access itself exercised by TP11–TP19, TP42–TP50 |
| 2 | NoC flit protocol compliance (`NOC_*` rules) | TP8–TP10, TP41, TP51, TP52, TP55 |
| 3 | Cross-protocol transformation (AXI ↔ flit pack/unpack, ECC end-to-end) | TP1, TP2, TP8–TP10, TP52, TP53 |
| 4 | RoB ordering invariants (per-AXI-ID) | TP5–TP7, TP38, TP39, TP64 |
| 5 | QoS Generator modes (Bypass / Fixed / Limiter / Regulator) | TP11–TP15 |
| 6 | Performance Probe accuracy (packet bandwidth, txn latency histogram) | TP16, TP17, TP35, TP36 |
| 7 | Error monitoring CSRs (saturating counters, ERR_STATUS RW1C across 3 event classes, LAST_ERR_INFO sticky capture) | TP18, TP19 |
| 8 | Interrupt mechanism (`irq_o` level-sensitive, IRQ_ENABLE masking, RW1C deassertion, `NI_IRQ_LEVEL`) | TP42 |
| 9 | AXI host-side parity check (data + address), log-only (no AXI rresp synthesis) | TP43, TP50 |
| 10 | Dual-clock-domain CDC correctness (no metastability / data loss across aclk ↔ noc_clk) | TP20–TP22, TP66 |
| 11 | Reset behavior (per pin_level_reset.md, including partial-reset edge cases) | TP23–TP26 |
| 12 | Mode switch (ACTIVE / PASSIVE) | TP27, TP28, TP33 |
| 13 | NMU-side software quiesce flow (`QUIESCE_CTRL` / `QUIESCE_STATUS`, NMU-only scope, best-effort liveness) | TP44, TP46 |
| 14 | Outstanding-count CSRs (`PENDING_R_COUNT` / `PENDING_W_COUNT`) + NSU Exclusive Monitor (`EXCLUSIVE_MONITOR_CTRL` / `EXCLUSIVE_MONITOR_STATUS`, race semantics with concurrent NSU events) | TP47–TP49 |

DV strategy:

| Strategy | Approach |
|----------|----------|
| Constrained-random | UVM 1.2 (designer-confirmed, A5 wave 2026-05-08). Master DUT stimulates AXI, NoC router stub provides the flit endpoint, scoreboard cross-checks AXI handshakes against observed NoC flits. |
| Directed (config knobs) | Verify each CSR write produces the documented wire-level effect. |
| Mode-switch | ACTIVE / PASSIVE transitions with and without in-flight transactions. |
| Reset | Assert each reset mid-transaction at every channel state, both resets together, and partial reset. |
| CDC | Vary aclk : noc_clk ratio across [0.1, 1, 10] to stress async FIFO depth. |
| ABV | Every FAIL-severity row in protocol_rules.md maps to one SVA `assert property`. |
| FPV | RoB allocator state machine, ECC SECDED gen + check round-trip. |

## Testpoints

Mapping README Features → testpoints (per stage gate D1.dv.testpoints requirement). Source noc-sim 09_verification.md provides additional testpoints; merged here.

| ID | README Feature | Testpoint description | Protocol rules exercised |
|----|----------------|------------------------|--------------------------|
| TP1 | AXI4 full protocol conversion | Master issues 1000 randomised single AXI writes to randomised addresses; NoC stub captures flits; verify flit content matches AXI request. | All AXI4 AW/W rules; NOC_FLIT_HDR_*; AXI4_SLV_XCH_W_AFTER_AW; AXI4_SLV_XCH_B_AFTER_AW_AND_W |
| TP2 | AXI4 full protocol conversion | Same for AXI reads. | AXI4 AR/R rules; AXI4_SLV_XCH_R_AFTER_AR; AXI4_SLV_XCH_R_LAST_CONSISTENT |
| TP3 | AXI4 burst handling | Burst writes (awlen ∈ {1, 7, 15}); verify N+1 W flits per burst, all carrying same axi_id; verify single B response with correct id. | AXI4_MST_AW_AWLEN_STABLE; NOC_FLIT_AW_W_ORDER |
| TP4 | AXI4 burst handling | Burst reads; verify wormhole-locked R flit sequence; final beat carries RLAST=1. | AR/R rules; AXI4_SLV_XCH_R_LAST_CONSISTENT |
| TP5 | RoB Normal mode (NormalRoB) | Issue 32 outstanding reads with mixed axi_id; randomize NoC response order; verify per-id in-order release at AXI; verify cross-id reordering. | AXI4_MST_ROB_PER_ID_ORDER |
| TP6 | RoB Simple mode (SimpleRoB) | Same with SimpleRoB; verify FIFO ordering across all txnIDs (different IDs serialised). | AXI4_MST_ROB_PER_ID_ORDER |
| TP7 | RoB NoRoB mode | `R_ROB_TYPE`/`B_ROB_TYPE = NoRoB` (requires `NUM_VC = 1`); NMU allocates a tracker for RoB-full back-pressure but releases each response immediately on receive (no in-order buffering); verify responses pass through in the fabric-delivered order. | AXI4_MST_ROB_OUTSTANDING_LIMIT |
| TP8 | flit_ecc single-bit corrected on W | Inject 1-bit error in W flit at NMU egress (`set_inject_ecc_error(W, SINGLE_BIT)`); verify NSU sink corrects silently, increments `ECC_CORR_ERR_CNT` (saturating, no clear path), forwards corrected data to local AXI slave with `bresp=OKAY`; verify `ECC_UNCORR_ERR_CNT`, `ERR_STATUS[0]`, `LAST_ERR_INFO`, `irq_o` all unchanged. | NOC_FLIT_HDR_FLIT_ECC_GEN; NOC_FLIT_HDR_FLIT_ECC_CHECK |
| TP9 | flit_ecc double-bit forwarded with logging on W | Inject 2-bit error in W flit at NMU egress (`set_inject_ecc_error(W, DOUBLE_BIT)`); verify NSU sink detects, forwards the corrupted flit to local AXI slave **with `bresp=OKAY`** (NoC fabric does NOT synthesise SLVERR from this check), increments `ECC_UNCORR_ERR_CNT`, sets `ERR_STATUS[0] ecc_uncorr_err`, captures `LAST_ERR_INFO` if first sticky, asserts `irq_o` if `IRQ_ENABLE[0]=1`. RW1C-clear ERR_STATUS[0] then verify counter and bit clear together; verify `irq_o` deasserts when last set+enabled bit clears. | NOC_FLIT_HDR_FLIT_ECC_CHECK; NI_CFG_ERR_STATUS_RW1C; NI_IRQ_LEVEL |
| TP10 | flit_ecc single-bit corrected + double-bit forwarded on R | Mirror of TP8/TP9 on R direction (`set_inject_ecc_error(R, SINGLE_BIT)` and `set_inject_ecc_error(R, DOUBLE_BIT)`). NMU sink corrects single-bit silently + ECC_CORR_ERR_CNT++; double-bit forwarded to AXI master with `rresp=OKAY` + ECC_UNCORR_ERR_CNT++ + ERR_STATUS[0] + LAST_ERR_INFO + irq_o (if enabled). For multi-beat R bursts: only the affected beat carries the corrupted data; other beats are unaffected and rresp=OKAY across the whole burst. | NOC_FLIT_HDR_FLIT_ECC_CHECK; NI_CFG_ERR_STATUS_RW1C; NI_IRQ_LEVEL |
| TP11 | QoS Bypass mode | `QOS_MODE = 0`; verify flit header qos == AXI awqos / arqos directly. | NI_CFG_QOS_MODE_TRANSITION |
| TP12 | QoS Fixed mode | `QOS_MODE = 1`, set QOS_FIXED_VALUE = 7; verify all flits have qos = 7 regardless of AXI awqos. | NI_CFG_QOS_MODE_TRANSITION |
| TP13 | QoS Limiter mode | `QOS_MODE = 2`, configure BANDWIDTH_LIMIT and SATURATION_THRESHOLD; issue traffic at 2× the limit; verify qos drops to LOW_PRIORITY when threshold exceeded. | NI_CFG_BANDWIDTH_LIMIT_BOUND |
| TP14 | QoS Regulator mode | `QOS_MODE = 3`, configure BANDWIDTH_BUDGET; observe response bandwidth slow → urgency rises → qos rises; observe response bandwidth high → urgency drops → qos drops. | NI_CFG_BANDWIDTH_BUDGET_BOUND |
| TP15 | QoS Saturation | Regulator mode with BASE_QOS=12; force urgency to MAX; verify final qos clamped at 15 (not wrap). | (none specific) |
| TP16 | Packet Probe | Configure `PKT_PROBE_EN`, `PKT_PROBE_MODE`, `PKT_WINDOW_SIZE`; issue known-bandwidth traffic; verify `PKT_BYTE_COUNT` and `PKT_BANDWIDTH` match expected. | NI_CFG_PROBE_EN_TRANSITION |
| TP17 | Transaction Probe | Configure thresholds; issue traffic with various round-trip latencies; verify each TXN_BIN_*_COUNT receives expected number of transactions. | (none specific) |
| TP18 | ERR_STATUS RW1C across all 3 bits | For each i ∈ {0..2}: trigger the corresponding event class (ECC double-bit on W → bit 0; inject route_par mismatch on a request flit → bit 1; corrupt `axi_wdata_par_i` on an AW handshake → bit 2). Verify `ERR_STATUS[i]` is set, the paired counter increments (ECC_UNCORR_ERR_CNT, ROUTE_PAR_ERR_CNT, AXI_PARITY_ERR_CNT respectively). Software writes 1 to `ERR_STATUS[i]`; verify bit and counter cleared atomically; verify other ERR_STATUS bits + counters are unaffected. | NI_CFG_ERR_STATUS_RW1C |
| TP19 | LAST_ERR_INFO sticky capture across 3 event classes | Trigger error A from event class X (e.g., ECC uncorr); verify `LAST_ERR_INFO` captures A's err_axi_id/src/dst. Trigger error B from event class Y ≠ X (e.g., AXI parity) without clearing; verify `LAST_ERR_INFO` still shows A (sticky regardless of class). Software writes 1 to the corresponding `ERR_STATUS[X]`; trigger error C from any class; verify `LAST_ERR_INFO` now shows C. Repeat with all (X, Y) pairs from {0..2} × {0..2}. | NI_CFG_LAST_ERR_INFO_CAPTURE; NI_CFG_ERR_STATUS_RW1C |
| TP20 | CDC at fast aclk | Set aclk_freq = 2 × noc_clk_freq; issue burst traffic; verify no flit loss, no order corruption across CDC. | NI_CDC_AXI_TO_NOC_FIFO; NI_CDC_NOC_TO_AXI_FIFO |
| TP21 | CDC at slow aclk | Set aclk_freq = 0.1 × noc_clk_freq; same. | Same |
| TP22 | CDC at equal clocks | aclk_freq = noc_clk_freq; same; verify FIFOs degenerate to direct paths but still function. | Same |
| TP23 | Reset during AXI AW phase | Master raises awvalid; arst_ni asserts before awready; verify NMU returns to IDLE; verify any cross-domain in-flight is cleaned up by NoC-side draining. | NI_RST_OUTPUTS_LOW_AXI |
| TP24 | Reset during NoC injection | Mid-flit injection on `noc_req_o`; noc_rst_ni asserts; verify noc_req_valid_o drops to 0 same cycle. | NI_RST_OUTPUTS_LOW_NOC |
| TP25 | Reset during multi-beat R burst | Master in-flight reading; noc_rst_ni asserts mid-burst; verify R beats stop on AXI side; verify the in-flight RoB entry's pending beats are dropped (RoB entry returned to FREE on noc_rst_ni release). On subsequent reset deassertion, master can re-issue the read. AXI side does NOT see rresp=SLVERR for partial-reset case — instead sees no further R beats and the master's transaction times out per master DUT's logic. | NI_RST_PARTIAL |
| TP26 | Partial reset (only one of two resets) | Assert only `arst_ni`; verify NoC side continues operating but cross-domain transactions stall. | NI_RST_PARTIAL |
| TP27 | Mode switch ACTIVE→PASSIVE | Issue traffic; mid-burst, switch to PASSIVE; verify all BFM-driven outputs transition to during-reset values within 1 cycle; verify in-flight transactions return MODE_SWITCHED_TO_PASSIVE. | NI_CFG_MODE_SWITCH |
| TP28 | Mode switch PASSIVE→ACTIVE | Switch back; verify outputs return to active state; new traffic flows. | Same |
| TP29 | NMU-only configuration | `EN_MGR_PORT=1, EN_SBR_PORT=0`; verify only NMU-related signals operate; NSU-related signals tied to inactive defaults. | (none specific) |
| TP30 | NSU-only configuration | Mirror. | Same |

### Additional integration testpoints (system-level)

| ID | Feature | Testpoint description | Protocol rules exercised |
|----|---------|------------------------|--------------------------|
| TP31 | End-to-end AXI4 over NoC, 2-NI loopback | NMU at node A, NSU at node B; AXI master at A issues writes; verify NSU at B drives correct writes to local AXI slave; reverse path tested with reads. | All AXI4 + NoC rules in combination |
| TP32 | Wormhole-route deadlock prevention | Two simultaneous bursts contending on same router output; verify QoS arbitration prevents starvation; verify no deadlock under all permutation combinations. | NI_CFG_QOS_MODE_TRANSITION + Router-side rules (separate spec) |
| TP33 | Cross-traffic during mode switch | Mid-test ACTIVE→PASSIVE on NI A while NI B still actively transacting; verify NI B's traffic unaffected; verify NI A's wires float. | NI_CFG_MODE_SWITCH |
| TP34 | NMU-only / NSU-only configurations | Build BFM with `EN_MGR_PORT=1, EN_SBR_PORT=0`; verify NSU signals tied to inactive defaults; mirror with NSU-only. Coverage of `D1.bfm.signal_interface` parameter constraints. | (configuration-only, no protocol rule directly) |
| TP35 | Probe accuracy under sustained load | Configure PKT_PROBE_EN with various PKT_WINDOW_SIZE; issue traffic at known bandwidth; verify reported PKT_BANDWIDTH within ±5% of actual (target accuracy). | NI_CFG_PROBE_PKT_BYTE_COUNT |
| TP36 | Long-tail latency capture | Configure TXN_PROBE thresholds (e.g., 10/100/1000/10000 cycles); inject long-tail-latency traffic; verify all 5 bins populated correctly. | NI_CFG_PROBE_TXN_LATENCY |
| TP37 | RoB exhaustion / back-pressure | Issue MAX_TXNS+1 outstanding transactions in rapid succession; verify NMU asserts back-pressure on awready / arready until RoB slot frees; verify no transaction loss. | AXI4_MST_ROB_OUTSTANDING_LIMIT |
| TP38 | RoB FREE entry allocation policy | Issue 5 transactions to RoB entries 0-4; complete entry 2 first; issue new transaction; verify it allocates to entry 2 (lowest-index-FREE-first per ToO §RoB allocator). | (none specific; ToO §RoB) |
| TP39 | RoB tie-breaking on simultaneous READY | Issue 2 transactions same axi_id (back-to-back); arrange responses to arrive simultaneously; verify lower rob_idx releases first (per ToO §RoB tie-breaking). | AXI4_MST_ROB_PER_ID_ORDER |
| TP40 | AR blocked during W burst | Start a long W burst; mid-burst issue an AR; verify the AR flit is held by the wormhole lock and NOT injected on noc_req_o until the W burst final beat (`last=1`) releases the lock, then the AR flit is granted (per ToO §AR-during-W); verify NSU dispatches both in that order. | NOC_FLIT_AW_W_ORDER; NOC_MST_WORMHOLE_LOCK |
| TP41 | route_par drop (silent AXI hang in v0.4.0) | Inject a single-bit corruption into a request flit's `route_par`-protected fields (`dst_id` / `last`) on `noc_req_o` egress (e.g., via stub router); verify the receiving router or NSU sink drops the flit, increments `ROUTE_PAR_ERR_CNT`, sets `ERR_STATUS[1]`, captures `LAST_ERR_INFO` (if first sticky). Set `IRQ_ENABLE[1]=1` and verify `irq_o` asserts. The originating AXI master transaction hangs silently — no automatic SLVERR synthesis in v0.4.0. Test framework upper-bounded watchdog detects the hang and validates the test case. | NOC_FLIT_HDR_ROUTE_PAR_GEN; NOC_FLIT_HDR_ROUTE_PAR_CHECK |
| TP42 | IRQ assert/deassert + IRQ_ENABLE mask + RW1C interaction | (a) With `IRQ_ENABLE = 0x0`, trigger each of the 3 ERR_STATUS event classes; verify `irq_o` stays LOW even though ERR_STATUS bits set and counters increment (mask works). (b) With `IRQ_ENABLE = 0x7`, trigger one event class at a time; verify `irq_o` rises on the event cycle (after CSR-CDC sync delay where applicable) and falls on the cycle the matching `ERR_STATUS[i]` is RW1C-cleared. (c) With multiple ERR_STATUS bits set + multiple IRQ_ENABLE bits set, verify `irq_o` stays HIGH until ALL set+enabled bits are cleared; verify partial clear keeps `irq_o` HIGH. (d) Edge case: set ERR_STATUS bit, then set the matching IRQ_ENABLE bit; verify `irq_o` asserts on the IRQ_ENABLE write cycle (level-sensitive, no edge requirement). | NI_IRQ_LEVEL; NI_CFG_ERR_STATUS_RW1C |
| TP43 | AXI host-side parity error logging (data + addr, both directions) | (a) NMU side: with `ENABLE_AXI_PARITY=true` (default), drive `axi_awvalid_i=1` with corrupted `axi_awaddr_par_i` (parity flipped); verify NMU logs `ERR_STATUS[2]`, `AXI_PARITY_ERR_CNT++`, `LAST_ERR_INFO` capture, and the AW transaction proceeds (no SLVERR injected at AXI boundary). Repeat for `axi_araddr_par_i` and `axi_wdata_par_i[byte]`. (b) NSU side: drive `axi_rvalid_i=1` from local slave with corrupted `axi_rdata_par_i[byte]`; verify NSU logs the same way and forwards the R beat to the originating AXI master with `rresp=OKAY` (no SLVERR). (c) Cross-check: set `ENABLE_AXI_PARITY=false` at instantiation; verify the parity wires are absent and TP43 a/b cannot be exercised (parameter sanity test). | AXI4_MST_PARITY_CHECK; AXI4_SLV_PARITY_CHECK |
| TP44 | NMU quiesce flow nominal | Issue 16 outstanding mixed AW/AR transactions on the NMU slave port; while in-flight, software writes `QUIESCE_CTRL.quiesce_req=1`. Verify NMU back-pressures `axi_awready_o = axi_arready_o = 0` for any new AW/AR; verify in-flight transactions complete normally (responses arrive at AXI master); verify `PENDING_R_COUNT` / `PENDING_W_COUNT` decrement to 0 as responses are consumed; verify `QUIESCE_STATUS.quiesce_idle` asserts on the cycle both PENDING counters reach 0. Software writes `quiesce_req=0`; verify NMU resumes accepting AW/AR on the next cycle; verify `quiesce_idle` deasserts the same cycle. | NI_CFG_QUIESCE_FLOW; NI_CFG_PENDING_COUNT_ACCURACY |
| TP46 | Quiesce scope (NMU-only, NSU continues) | Set `quiesce_req=1` on NI A. From a remote NI B, issue AXI traffic that arrives at A's NSU via NoC (NMU at B → router fabric → NSU at A → A's local AXI slave). Verify NI A's NSU continues to drive `axi_*_o` to its local AXI slave normally — quiesce does NOT stop NSU. Verify `quiesce_idle` reflects ONLY NMU-side outstanding (PENDING_R/W_COUNT); NSU-side activity does NOT affect the bit. | NI_CFG_QUIESCE_FLOW |
| TP47 | Exclusive Monitor clear_all (no race) | Allocate 5 Exclusive AR reservations on NSU (different `axi_id`); verify `EXCLUSIVE_MONITOR_STATUS.occupancy = 5`. Software writes `EXCLUSIVE_MONITOR_CTRL.clear_all = 1`; verify on the next aclk edge: occupancy = 0; subsequent Exclusive AW from any of the 5 master IDs misses the monitor → bresp=OKAY (downgraded to normal write); read-back of `EXCLUSIVE_MONITOR_CTRL.clear_all` returns 0 (self-cleared on the next `aclk_i` edge after the CSR-write completed). | NI_CFG_EXCLUSIVE_CLEAR_RACE; NI_CFG_EXCLUSIVE_OCCUPANCY_ACCURACY |
| TP48 | Exclusive clear race semantics | Drive simultaneous events on the same `aclk_i` cycle as the `clear_all` CSR write handshake completes: (a) Exclusive AW match check on NSU → verify match check uses **pre-clear** state (the AW that arrived in the same cycle proceeds against the entry that was alive at start-of-cycle; if matched, EXOKAY); (b) new Exclusive AR allocation → verify the new entry survives clear (post-clear allocation); (c) overlap-invalidate triggered by a normal write → verify idempotent (entry invalidated either way). Cover all three race types in directed cycles. | NI_CFG_EXCLUSIVE_CLEAR_RACE |
| TP49 | PENDING_*_COUNT and Exclusive occupancy accuracy under load | Issue mixed single-beat + burst AW/AR traffic across the slave port concurrently with Exclusive AR/AW activity at NSU. At every aclk cycle, sample `PENDING_R_COUNT`, `PENDING_W_COUNT`, and `EXCLUSIVE_MONITOR_STATUS.occupancy` via CSR readback; cross-check against scoreboard tracking AW/AR/B/R handshake events at `axi_*_i` (for PENDING) and Exclusive AR/AW/clear events at NSU (for occupancy). Tolerance: counters MUST match scoreboard exactly on the cycle the read handshake completes (no CDC slack — all three counters are aclk-domain). | NI_CFG_PENDING_COUNT_ACCURACY; NI_CFG_EXCLUSIVE_OCCUPANCY_ACCURACY |
| TP50 | NMU R-direction parity regeneration (`axi_rdata_par_o`) | (a) Issue a normal AXI read with `ENABLE_AXI_PARITY=true`. Verify NMU drives `axi_rdata_par_o[AXI_DATA_WIDTH/8-1:0]` byte-wise correctly: each parity bit equals XOR-reduction of the corresponding `axi_rdata_o` byte (even-parity convention); regeneration occurs **after** the `flit_ecc` check stage. AXI master verifies parity, sees no mismatch. (b) Inject `flit_ecc` 1-bit error on the R flit (`set_inject_ecc_error(R, SINGLE_BIT)`); verify NMU corrects the bit silently and regenerates `axi_rdata_par_o` over the **corrected** data — master sees consistent data + parity. (c) Inject `flit_ecc` 2-bit error (`set_inject_ecc_error(R, DOUBLE_BIT)`); verify NMU forwards the corrupted `axi_rdata_o` with `rresp=OKAY` AND regenerates parity over the **corrupted** wire data — master can independently detect via parity check. Verify simultaneous `ECC_UNCORR_ERR_CNT++` + `ERR_STATUS[0]` + `irq_o` (if `IRQ_ENABLE[0]=1`) triggered by the ECC path. | AXI4_MST_PARITY_GEN_R; NOC_FLIT_HDR_FLIT_ECC_CHECK |
| TP51 | NMU credit-based flit injection contract | (a) Normal: with per-VC credit > 0 on the chosen VC, issue an AXI request; verify NMU drives `noc_req_valid_o=1` on the egress cycle and the per-VC credit counter decrements by 1. (b) Credit starvation: withhold all `noc_req_credit_i` returns to drain the per-VC credit pool to 0; issue a fresh AXI AW/AR; verify NMU does NOT assert `noc_req_valid_o`. The stall is permanent — no automatic escalation to SLVERR in v0.4.0. (c) Recovery: return one credit on `noc_req_credit_i[chosen_vc]`; verify NMU resumes flit injection on the next available cycle and decrements the counter again. (d) Per-VC isolation: starve VC 0 only, leave VC 1 with credits; verify NMU still injects flits whose `vc_id` maps to VC 1 (Hybrid R/W × QoS mapping per `NOC_VC_MAPPING_HYBRID_RW_QOS`). | NOC_MST_FLIT_ON_CREDIT_ONLY; NOC_VC_MAPPING_HYBRID_RW_QOS |

### Per-testbench bring-up testpoints

These verify each testbench's DUT-vs-golden self-check (per `test_environment.md` §1.5), independent of the protocol-detail testpoints above. TB-A/B/C run at P1 (C model DUT) and P2 (RTL DUT). TB-D loopback runs P1 to P3. "Closure" gives the V-milestone span.

| ID | Testbench | DUT scope | Golden / self-check | Stage | Closure | Rules / refs |
|----|-----------|-----------|---------------------|-------|---------|--------------|
| TP52 | TB-A NMU-only | NMU AXI→flit packing | Expected-flit-log byte compare. Pattern-gen golden derived from the AXI→flit mapping in `theory_of_operation.md` (no OSS golden). | P1, P2 | pre-V1 → V2 | NOC_FLIT_HDR_AXI_CH_VALUES; NOC_FLIT_HDR_DST_ID_VALID; AXI4_SLV_XCH_W_AFTER_AW |
| TP53 | TB-B NSU-only | NSU flit→AXI replay | memory_state byte-exact + response_log (rdata + resp-code + per-ID order). | P1, P2 | pre-V1 → V2 | AXI4_SLV_XCH_B_AFTER_AW_AND_W; AXI4_SLV_R_RLAST_LEN_CONSISTENT |
| TP54 | TB-C Router-only | Router routing / VC / credit / wormhole | XY routing oracle (all-pairs realized-hop compare) + analytic zero-load latency. | P1, P2 | pre-V1 → V2 | NOC_VC_PARTITION; NOC_MST_WORMHOLE_LOCK |
| TP55 | TB-D mixed loopback | full NMU → Router → NSU → AXI slave chain | AXI-in == AXI-out (cross-ID reorder tolerated, same-ID order enforced) + memory_state byte-exact + response_log. Needs no flit golden. | P1, P2, P3 | pre-V1 → V3 | all AXI4 + NOC rules in combination |

### Timing microbench testpoints (cycle-accuracy calibration)

Timing-axis testpoints per `test_environment.md` §6.3. RTL is the timing reference and the C model calibrates to it. These check cycle counts, not data correctness, so they sit beside the functional testpoints rather than replacing them. Expected values are oracle-derived where an analytic value exists (zero-load latency, XY hop count). Otherwise the expected value is the RTL-measured baseline frozen at first P2 calibration. Tolerance classes per §6.3: ±0 (single-clock deterministic path), phase-envelope (CDC crossing), ±5% (loaded aggregate, covered by TP35 and §6.2 Performance, not by these directed cases).

| ID | Scenario | Stimulus | Observation point | Expected | Tol | Stage | Closure | Related |
|----|----------|----------|-------------------|----------|-----|-------|---------|---------|
| TP56 | Zero-load single flit | 1 single-beat AW, idle fabric | cycles awvalid→noc_req_valid (NMU) and end-to-end (TB-D) | analytic zero-load latency oracle | ±0 | P1, P2/P3 | pre-V1, V2/V3 | TP1; §6.1 |
| TP57 | Multi-hop XY | 1 flit, src/dst at known hop distance | per-hop latency × hop count (TB-C / TB-D) | XY oracle + zero-load formula | ±0 | P1, P2/P3 | pre-V1, V2/V3 | TP31; §6.1 |
| TP58 | W-burst wormhole lock | AW burst awlen=15, idle fabric | W-flit sequence on one VC, contiguous, no interleave | RTL-calibrated burst-traversal baseline | ±0 | P2, P3 | V2/V3 | TP3; NOC_MST_WORMHOLE_LOCK |
| TP59 | AR blocked during W | long W burst, AR issued mid-burst | AR-flit injection cycle relative to W flits | per ToO AR-during-W policy, RTL-calibrated | ±0 | P2, P3 | V2/V3 | TP40; NOC_FLIT_AW_W_ORDER |
| TP60 | Same-cycle AW/AR RR tie | AW and AR valid same cycle, idle | first-grant order at arbiter | round-robin order per ToO arbiter | ±0 | P2, P3 | V2/V3 | (none specific; ToO arbiter) |
| TP61 | VC contention | two flows on distinct VC contend one router output | grant-alternation cycles | RR fairness per router spec, RTL-calibrated | ±0 | P2, P3 | V2/V3 | TP32; NOC_VC_PARTITION |
| TP62 | Credit starvation / recovery | drain chosen VC credit to 0, then return 1 | stall cycles + resume cycle | credit-return latency, RTL-calibrated | ±0 | P2, P3 | V2/V3 | TP51; NOC_MST_FLIT_ON_CREDIT_ONLY |
| TP63 | RoB full / backpressure | MAX_TXNS+1 outstanding in rapid succession | awready/arready deassert cycle + reassert on slot free | RoB depth-driven, RTL-calibrated | ±0 | P2, P3 | V2/V3 | TP37; AXI4_MST_ROB_OUTSTANDING_LIMIT |
| TP64 | Same-ID reorder release | 2 same-ID txns, responses arrive reversed | release cycle of each at AXI | RoB head-of-line release, RTL-calibrated | ±0 | P2, P3 | V2/V3 | TP5, TP39; AXI4_MST_ROB_PER_ID_ORDER |
| TP65 | QoS no-preempt | higher-qos flow arrives mid lower-qos wormhole burst | lock-hold cycles, no preemption | wormhole lock holds to last flit | ±0 | P2, P3 | V2/V3 | TP32; NOC_MST_WORMHOLE_LOCK |
| TP66 | CDC ratio 1:1 / 2:1 / 1:2 | single-flit traversal at aclk:noc_clk ∈ {1:1, 2:1, 1:2} | CDC crossing latency at each ratio | phase-dependent envelope, not cycle-exact | phase-envelope | P2, P3 | V2/V3 | TP20-22; NI_CDC_AXI_TO_NOC_FIFO, NI_CDC_NOC_TO_AXI_FIFO |

## Coverage model

Covergroups, each binned across the rules / scenarios it exercises:

| Covergroup | Bins / scenarios |
|------------|------------------|
| cg_axi_handshake_{aw,w,b,ar,r} | (VALID-before-READY, READY-before-VALID, simultaneous) × (AWLEN ∈ {0, 1-7, 8-15, 16+}) × (id ∈ {0..MAX_UNIQUE_IDS-1}) |
| cg_noc_handshake_{req,rsp}_{out,in} | (valid-before-ready, ready-before-valid, simultaneous) × (back-to-back, intermittent) |
| cg_rob_state_machine | per state (FREE / ALLOCATED / RESPONSE_RECEIVED / READY_TO_RELEASE) × per type (Normal / Simple / NoRoB) |
| cg_qos_modes | Bypass, Fixed, Limiter (<threshold), Limiter (≥threshold), Regulator (urgency = 0 / mid / MAX) |
| cg_qos_clamp | Regulator BASE_QOS + urgency at boundary (clamp to 15), SOCKET_QOS lift (≥ SOCKET_QOS) |
| cg_ecc | no error, 1-bit corrected (W / R), 2-bit forwarded-with-log (W / R), route_par drop (request / response flit). 2-bit cases verify forward+log, no SLVERR synthesis (`NOC_FLIT_HDR_FLIT_ECC_CHECK`) |
| cg_axi_parity | no error, NMU-side awaddr / araddr / wdata byte-parity error, NSU-side rdata byte-parity error (all log `ERR_STATUS[2]` + `AXI_PARITY_ERR_CNT`, rresp/bresp unchanged). Plus `axi_rdata_par_o` regen bins (no-error / 1-bit-corrected / 2-bit-uncorrectable, parity reflects on-the-wire data per `AXI4_MST_PARITY_GEN_R`) |
| cg_probe_packet | modes (Combined, Read, Write) × window-overlap scenarios |
| cg_probe_txn | each latency-bin coverage |
| cg_err_status | none, single class (×3), two-class combinations, all-3 set, partial-clear (RW1C), full-clear (RW1C) |
| cg_irq | mask-all-no-irq, single-bit-set+enable (×3), multi-bit-set+partial-mask, deassert-on-last-clear, rise-on-`IRQ_ENABLE`-write (late mask enable) |
| cg_cdc_clock_ratio | aclk:noc_clk ∈ {1:10, 1:2, 1:1, 2:1, 10:1} |
| cg_reset_phase | reset during (AW, W, B, AR, R, idle), partial-reset variants |
| cg_mode_switch | ACTIVE→PASSIVE (during AXI / during NoC / idle), PASSIVE→ACTIVE |
| cg_quiesce | idle, drain-in-progress (req=1, pending>0), drain-complete (req=1, pending=0, idle=1), resume (req=0 after idle) |
| cg_exclusive_clear | clear-in-isolation, clear+concurrent-AW-match, clear+concurrent-AR-alloc, clear+concurrent-overlap-invalidate, pre-clear-occupancy (1, EXCLUSIVE_MONITOR_DEPTH/2, EXCLUSIVE_MONITOR_DEPTH) |
| cg_pending_count | PENDING_R_COUNT (0, 1, MAX_TXNS/2, MAX_TXNS) × PENDING_W_COUNT (same set), cross-coverage for off-by-one between NMU tracker and CSR readback |

**`cg_protocol_rule_hits`** — one cover-property (one bin) per rule ID in protocol_rules.md. Count: **136 rule IDs** post-A5, from the canonical `grep -c '^| AXI4\\|^| NOC\\|^| NI\\|^| AXI4LITE' protocol_rules.md`. The per-channel decomposition is the section structure of protocol_rules.md itself (the authoritative source). (A5 wave removed `AXI4_MST_TIMEOUT_SLVERR` and `NI_CFG_QUIESCE_LIVENESS`; A4.7 baseline was 138.)

D3 coverage closure goal: 100% bin hits on every covergroup.

## ABV / FPV strategy

**ABV** — every FAIL-severity row in protocol_rules.md gets one SVA `assert property` in the testbench. Post-A5 canonical count: **126 FAIL-severity assertions** + **10 RECOMMEND cover-properties** (RECOMMEND family: `NI_RST_PARTIAL`, `AXI4_MST_AW_AWCACHE_STABLE`, `AXI4_MST_AR_ARCACHE_STABLE`, `NOC_FLIT_HDR_RSVD_IGNORE_RX`, `NOC_VC_PARTITION`, `AXI4LITE_SLV_RO_WRITE_IGNORED`, `NI_CFG_RESPONSE_DELAY_AXI`, `NI_CFG_RESPONSE_DELAY_NOC`, `NI_CFG_INJECT_ECC_ERROR`, `NI_CFG_RESPONSE_FAULT`). Total ABV library size: 136 properties.

**FPV** — formal verification scope:

| FPV target | Property proven |
|------------|-----------------|
| RoB allocator state machine (FREE → ALLOCATED → RESPONSE_RECEIVED → READY_TO_RELEASE → FREE) | no deadlock, no entry stuck, per-id ordering |
| flit_ecc SECDED Hsiao (gen + check round-trip) | single-bit correction for all single-bit patterns, double-bit detection for representative patterns (sampled, full enumeration infeasible at 406-bit flit), and the (B)-philosophy invariant: on double-bit, flit forwarded **without** modifying the AXI rresp/bresp value |
| route_par parity | XOR-reduction over `{dst_id, last}` (NPP packet DST ID + LAST coverage), drop-on-mismatch behaviour |
| IRQ assertion function | `irq_o = OR_i(ERR_STATUS[i] & IRQ_ENABLE[i])` purely combinational, no glitch under simultaneous bit transitions, no deadlock between RW1C clear and re-assert |
| CDC async FIFO | no data loss / corruption / pointer divergence across all clock-ratio extremes |
| Reset entry sequencing | wire-level reset values + post-reset transitions match pin_level_reset.md |

**Security role**: NI does NOT participate in security-critical paths (access control, attestation, key management). Sec_cm FPV is **not required**. AXI awprot/arprot are sampled but not enforced; protection-attribute checking is delegated to downstream slaves or upstream IP. **Designer-confirmed (A5 wave 2026-05-08): NI is not security-critical; no sec_cm FPV required. Revisit if a future revision adds security gating.**

## Out of scope

- AXI4 atomic operations (ATOPs) — explicitly out of scope per ToO §ATOPs scope. ATOP transactions terminate with `bresp=SLVERR`; not exercised by DV beyond a single negative testpoint (TP_NEG_ATOP: send ATOP write, verify SLVERR + counter increment).
- AXI5 features (CHI-derived, cache-coherent).
- Multi-NI integration (cross-node ordering across the mesh) — handled at system-level DV, not at this NI's unit DV.
- Router DV — separate spec.
- System-level deadlock testing — see system DV.
- Power / clock-gating verification — separate concern.
