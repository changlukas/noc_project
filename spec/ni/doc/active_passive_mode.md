# Active vs Passive Mode

## Capability table

| Capability | Active | Passive |
|------------|--------|---------|
| Drives AXI slave port outputs (`axi_rsp_o.*`) | yes | no |
| Drives AXI master port outputs (`axi_req_o.*`) | yes | no |
| Drives NoC outputs (`noc_req_valid_o`, `noc_req_flit_o`, `noc_rsp_valid_o`, `noc_rsp_flit_o`) | yes | no |
| Drives CSR access port outputs (`csr_axi_rsp_o.*`) | yes | no |
| Drives NoC credit return + credit-init-ready outputs (`noc_req_credit_o[NUM_VC-1:0]`, `noc_rsp_credit_o[NUM_VC-1:0]`, `noc_*_credit_init_ready_o`) | yes | no — BFM stops participating in credit-based flow control, so it cannot promise buffer space or initiate credit exchange. NoC links un-drained in passive mode (must be paired with a real receiver) |
| Drives AXI slave-port parity sideband outputs (`axi_awaddr_par_o`, `axi_araddr_par_o`, `axi_wdata_par_o` — present only when `ENABLE_AXI_PARITY = true`) | yes | no — these are NSU-side derivatives of `axi_req_o.*` and follow the same rule |
| Drives interrupt output (`irq_o`) | yes | **yes** — IRQ is the wire-level mechanism by which logged violations are surfaced; PASSIVE mode continues to monitor + log + capture `LAST_ERR_INFO`, so `irq_o` continues to evaluate `OR(ERR_STATUS[i] & IRQ_ENABLE[i])`. This is consistent with the "Reports protocol-rule violations" + "Contributes to coverage hooks" capabilities below being available in PASSIVE. |
| Samples all inbound signals (AXI inputs, NoC inputs, CSR access inputs) | yes | yes |
| Reconstructs AXI transactions from observed activity | yes | yes |
| Reconstructs NoC flits / packets from observed activity | yes | yes |
| Generates response stimulus via Transaction API (`apply_*`, `set_response_*`, etc.) | yes | no — knobs accepted but no driving effect |
| Reports protocol-rule violations per `protocol_rules.md` | yes | yes |
| Updates error-status CSRs (`ERR_STATUS[2:0]`, paired counters, `LAST_ERR_INFO`) on detected violations | yes | yes — error logging is part of monitoring; PASSIVE accumulates the same evidence ACTIVE does |
| Contributes to coverage hooks per `dv/plan.md` | yes | yes |
| `expect_*` methods work for monitoring | yes | yes |
| ECC validation on inbound flits | yes | yes |

## Mode switch

- **Knob name**: `bfm_mode`
- **Type / values**: enum `{ACTIVE, PASSIVE}`
- **Default**: `ACTIVE`
- **Granularity**: per-NI (NMU and NSU switch together). A single `bfm_mode` knob is sufficient for all currently planned testbench setups, including the 4-file mix-and-match co-sim arrangements (NMU.rtl / NMU.cpp / NSU.rtl / NSU.cpp per `docs/design/08_simulation.md` §9 hot-swap). In those arrangements each NI BFM instance houses only one half — instantiated as NMU-only (`EN_MST_PORT=1, EN_SLV_PORT=0`) or NSU-only (`EN_MST_PORT=0, EN_SLV_PORT=1`) — so the single mode knob applies cleanly to whichever half is built. Future monitor-style use cases — e.g., a single instance hosting both halves where NMU passively observes the RTL master while NSU actively drives the slave — would require splitting the knob into per-half modes; that extension is intentionally out of scope until such a testbench is introduced.
- **API to switch**: `set_bfm_mode(mode)` per `transaction_api.md`.
- **Effect of ACTIVE → PASSIVE**: All ACTIVE-only BFM-driven outputs transition to their during-reset values per `pin_level_reset.md` within one cycle of the corresponding clock. The full set:
   - AXI side: `axi_rsp_o.*`, `axi_req_o.*`
   - NoC side: `noc_req_o.*`, `noc_rsp_o.*` (valid + flit forward bundle)
   - NoC credit side: `noc_req_credit_o[NUM_VC-1:0]`, `noc_rsp_credit_o[NUM_VC-1:0]`, `noc_*_credit_init_ready_o`
   - CSR side: `csr_axi_rsp_o.*`
   - AXI parity sideband (when `ENABLE_AXI_PARITY = true`, NSU-driven side): `axi_awaddr_par_o`, `axi_araddr_par_o`, `axi_wdata_par_o`

  **Exception — `irq_o` continues to be driven** by the function `OR(ERR_STATUS[i] AND IRQ_ENABLE[i])` (per `protocol_rules.md` `NI_IRQ_LEVEL`). This is intentional: PASSIVE mode preserves the monitoring + logging path, and IRQ is the wire-level surface for those logs. A passive monitor can still notify the testbench of detected violations without ever driving any other output.

  In-flight Transaction API calls unblock with `MODE_SWITCHED_TO_PASSIVE`. The BFM continues to monitor and log violations (incl. updating `ERR_STATUS[2:0]`, paired counters, and `LAST_ERR_INFO`). Corresponds to `protocol_rules.md` `NI_CFG_MODE_SWITCH`.
- **Effect of PASSIVE → ACTIVE**: BFM-driven outputs return to reset-deassertion values; configuration knobs (set during PASSIVE) become effective on the first transaction after the switch.
- **Switching mid-transaction**: Permitted; BFM logs warning. Test author should call `reset_state()` before switching back to ACTIVE if mid-transaction state was non-trivial.

## Common testbench setups

- **Single-NI testbench**: one active NI bridging an AXI master DUT to a stub NoC (router model). Most common for unit-test of an attached IP that uses the NI.
- **NI-pair testbench**: two active NIs facing each other through a router fabric, used for end-to-end transaction testing (e.g., IP at node A reads from memory at node B).
- **Mixed (mesh integration regression)**: real RTL NIs at all nodes; one passive NI BFM attached to a chosen node for protocol violation detection without altering DUT behavior.
- **Forbidden**: two active NIs driving the same AXI slave port or the same NoC link. Only one active driver per link.

## Reset interaction

`bfm_mode` is preserved across both `arst_ni` and `noc_rst_ni`. Mode survives wire-level reset; only `reset_state()` API or test-author intervention restores defaults. Default `bfm_mode = ACTIVE` on first instantiation.
