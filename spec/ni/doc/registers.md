# Registers

NI exposes a software-visible CSR file via the dedicated AXI4-Lite slave port (`csr_*`). All CSRs are 32-bit-aligned. Access policies:

- **Sub-word access**: not supported. CSR writes with `csr_wstrb_i != 0xF` (any byte deasserted) trigger `csr_bresp_o = SLVERR` and the write is dropped. Reads ignore byte strobes (full 32-bit word returned).
- **Unmapped offset**: any `csr_awaddr_i` or `csr_araddr_i` that doesn't match a register in §Register map below triggers `csr_bresp_o = DECERR` (writes) or `csr_rresp_o = DECERR` (reads). Unmapped reads return `csr_rdata_o = 0x0`.
- **Misaligned access**: `csr_awaddr_i[1:0] != 0` or `csr_araddr_i[1:0] != 0` triggers `SLVERR`.
- **Write-only (WO) registers**: in this spec, `WO` means the register accepts writes; reads return `csr_rdata_o = 0x0` with `csr_rresp_o = OKAY` (NOT DECERR). Used for self-clearing trigger registers where there is no persistent read-back state. The unmapped-offset DECERR rule applies only to offsets not listed in §Register map; mapped WO offsets are reachable on reads with the read-as-zero contract.

CSR memory map below is sourced from noc-sim/docs/design (post-QoS-removal); QoS Generator registers removed per spec change §8.2.

## Reserved-bit policy

- **Writes**: bits in the Reserved column of any register layout are ignored — software writes to those bit positions have no effect on hardware state.
- **Reads**: Reserved bits return 0.
- **RW1C bits**: bits not in the §ERR_STATUS layout (i.e., bits [31:4] of ERR_STATUS) are Reserved and follow the standard reserved-bit policy above (write-ignored, read-zero).
- **Forward compatibility**: software should not assume Reserved bits will remain 0 in future revisions. Always mask reads of Reserved bits to 0 before logical comparison.

## Register map

| Offset | Register | Access | Reset | Description |
|--------|----------|--------|-------|-------------|
| **Packet Probe** |||||
| 0x040 | `PKT_PROBE_EN` | RW | 0x0 | 啟用 Packet Probe. |
| 0x044 | `PKT_PROBE_MODE` | RW | 0x0 (Combined) | 統計模式 (0=Combined, 1=Read, 2=Write). |
| 0x048 | `PKT_WINDOW_SIZE` | RW | 0x0 | 統計視窗 (cycles). |
| 0x04C | `PKT_BYTE_COUNT` | RO | 0x0 | 傳輸 bytes 累計 (saturating). |
| 0x050 | `PKT_BANDWIDTH` | RO | 0x0 | 計算的頻寬 (per current window). |
| **Transaction Probe** |||||
| 0x060 | `TXN_PROBE_EN` | RW | 0x0 | 啟用 Transaction Probe. |
| 0x064 | `TXN_THRESHOLD_0` | RW | 0x0 | 延遲閾值 0 (cycles). |
| 0x068 | `TXN_THRESHOLD_1` | RW | 0x0 | 延遲閾值 1. |
| 0x06C | `TXN_THRESHOLD_2` | RW | 0x0 | 延遲閾值 2. |
| 0x070 | `TXN_THRESHOLD_3` | RW | 0x0 | 延遲閾值 3. |
| 0x080 | `TXN_BIN_0_COUNT` | RO | 0x0 | Bin 0 計數 (saturating). |
| 0x084 | `TXN_BIN_1_COUNT` | RO | 0x0 | Bin 1 計數. |
| 0x088 | `TXN_BIN_2_COUNT` | RO | 0x0 | Bin 2 計數. |
| 0x08C | `TXN_BIN_3_COUNT` | RO | 0x0 | Bin 3 計數. |
| 0x090 | `TXN_BIN_4_COUNT` | RO | 0x0 | Bin 4 計數. |
| 0x094 | `TXN_MIN_LATENCY` | RO | 0xFFFF | 最小延遲 (16-bit; initialised to all-1s sentinel value; first observed transaction-latency overwrites; subsequent observations write only if smaller). Cleared back to 0xFFFF via `TXN_PROBE_EN` 1→0→1 transition. |
| 0x098 | `TXN_MAX_LATENCY` | RO | 0x0 | 最大延遲. |
| 0x09C | `TXN_TOTAL_COUNT` | RO | 0x0 | 總 transaction 數 (saturating). |
| **Error Status / IRQ** |||||
| 0x100 | `ERR_STATUS` | RW1C | 0x0 | 錯誤狀態 — write 1 to clear bit and the associated saturating counter. See §ERR_STATUS. <!-- source: 06_qos.md §4.1, post-fix RW1C --> |
| 0x108 | `ECC_UNCORR_ERR_CNT` | RO | 0x0 | ECC uncorrectable 錯誤計數 (saturating, `ERR_COUNTER_WIDTH` bits). Cleared via `ERR_STATUS[0]` write-1 (ecc_uncorr_err). |
| 0x10C | `LAST_ERR_INFO` | RO | 0x0 | 最近錯誤資訊 (sticky semantics; see §LAST_ERR_INFO for field layout). |
| 0x110 | (reserved for `LAST_ERR_INFO_HI`) | — | — | Allocated for compile-time `LAST_ERR_INFO` extension when total field width exceeds 32 bits (e.g., `AXI_ID_WIDTH=16` with extended `X_WIDTH+Y_WIDTH`). Unused at default parameters; reads as DECERR until activated. |
| 0x114 | `IRQ_ENABLE` | RW | 0x0 | Per-bit IRQ mask, layout 1:1 with `ERR_STATUS`. `irq_o = OR over i of (ERR_STATUS[i] & IRQ_ENABLE[i])`. Default 0 = all IRQs masked; software opts in. See §IRQ_ENABLE for field layout. |
| 0x118 | `ECC_CORR_ERR_CNT` | RO | 0x0 | flit_ecc 單位元（已修正）錯誤累計 (saturating, `ERR_COUNTER_WIDTH` bits). Pure informational; cumulative since hardware reset; no software clear path. Software polls and tracks deltas for health-monitoring purposes. |
| 0x11C | `ROUTE_PAR_ERR_CNT` | RO | 0x0 | route_par mismatch (flit dropped at router/sink) 累計 (saturating, `ERR_COUNTER_WIDTH` bits). Cleared via `ERR_STATUS[1]` write-1 (route_par_err). |
| 0x120 | `AXI_PARITY_ERR_CNT` | RO | 0x0 | AXI host-side parity mismatch 累計 (saturating, `ERR_COUNTER_WIDTH` bits). Covers both NMU-side `axi_*_i_par_i` checks and NSU-side `axi_rdata_par_i` checks. Cleared via `ERR_STATUS[2]` write-1 (axi_parity_err). |
| **Runtime control** |||||
| 0x130 | `PENDING_R_COUNT` | RO | 0x0 | NMU live outstanding read transactions. Width = `ceil(log2(MAX_TXNS+1))`. AXI-edge-defined per `protocol_rules.md` `NI_CFG_PENDING_COUNT_ACCURACY`. See §PENDING_R_COUNT. |
| 0x134 | `PENDING_W_COUNT` | RO | 0x0 | NMU live outstanding write transactions. Same width formula and contract as `PENDING_R_COUNT`. See §PENDING_W_COUNT. |
| 0x13C | `QUIESCE_CTRL` | RW | 0x0 | NMU-side quiesce request bit. See §QUIESCE_CTRL. |
| 0x140 | `QUIESCE_STATUS` | RO | 0x0 | NMU quiesce drain-complete indicator. See §QUIESCE_STATUS. |
| 0x144 | `EXCLUSIVE_MONITOR_CTRL` | WO | 0x0 | NSU Exclusive Monitor `clear_all` trigger (W1 self-clearing). See §EXCLUSIVE_MONITOR_CTRL. |
| 0x148 | `EXCLUSIVE_MONITOR_STATUS` | RO | 0x0 | NSU Exclusive Monitor live `occupancy` count. See §EXCLUSIVE_MONITOR_STATUS. |

## §ERR_STATUS Register (0x100) Field Layout

<!-- source: 06_qos.md §4.2, expanded in A3 for route_par + AXI parity error classes -->

| Field | Bit | Width | Description | Reset |
|-------|-----|-------|-------------|-------|
| `ecc_uncorr_err` | [0] | 1 | flit_ecc 雙位元錯偵測到 (corrupted flit forwarded; AXI rresp 不變). Write 1 clears bit + `ECC_UNCORR_ERR_CNT`. | 0x0 |
| `route_par_err` | [1] | 1 | route_par mismatch detected by router or NI sink (offending flit dropped). Write 1 clears bit + `ROUTE_PAR_ERR_CNT`. | 0x0 |
| `axi_parity_err` | [2] | 1 | AXI host-side parity mismatch (NMU-side AW/AR/W input parity OR NSU-side rdata parity). Transaction is logged but not aborted. Write 1 clears bit + `AXI_PARITY_ERR_CNT`. | 0x0 |
| Reserved | [31:3] | 29 | — | 0x0 |

**Bit-to-IRQ mapping**: each bit drives `irq_o` when its companion `IRQ_ENABLE[i]` bit is set. See §IRQ_ENABLE.

## §IRQ_ENABLE Register (0x114) Field Layout

| Field | Bit | Width | Description | Reset |
|-------|-----|-------|-------------|-------|
| `ecc_uncorr_irq_en` | [0] | 1 | Allow `ERR_STATUS[0]` to drive `irq_o`. | 0x0 |
| `route_par_irq_en` | [1] | 1 | Allow `ERR_STATUS[1]` to drive `irq_o`. | 0x0 |
| `axi_parity_irq_en` | [2] | 1 | Allow `ERR_STATUS[2]` to drive `irq_o`. | 0x0 |
| Reserved | [31:3] | 29 | — | 0x0 |

Reset default 0x0 (all IRQs masked) — software opts in per error class. The IRQ assertion function is purely combinational over the latched `ERR_STATUS` bits in the `aclk_i` domain (`irq_o = |(ERR_STATUS & IRQ_ENABLE)`). NoC-domain error sources reach `ERR_STATUS` via the existing CSR-file CDC sync path; no separate interrupt CDC is introduced. See `protocol_rules.md` `NI_IRQ_LEVEL`.

## §LAST_ERR_INFO Register (0x10C) Field Layout

<!-- source: 06_qos.md §4.3 -->

Field widths derived from defaults: `AXI_ID_WIDTH=8`, `X_WIDTH+Y_WIDTH=8`. For non-default configurations: register layout adjusts at compile time. If `AXI_ID_WIDTH=16`, `err_axi_id` occupies [15:0], `err_src_id` occupies [23:16], `err_dst_id` occupies [31:24], no Reserved bits. If total width exceeds 32 bits, register splits into LAST_ERR_INFO_LO (0x10C) + LAST_ERR_INFO_HI (0x110); see §Compile-time-conditional registers below.

| Field | Bit | Width | Description | Reset |
|-------|-----|-------|-------------|-------|
| `err_axi_id` | [7:0] | `AXI_ID_WIDTH` | 錯誤 transaction 的 AXI ID. | 0x0 |
| `err_src_id` | [15:8] | `X_WIDTH + Y_WIDTH` | 錯誤來源 node ID. | 0x0 |
| `err_dst_id` | [23:16] | `X_WIDTH + Y_WIDTH` | 錯誤目標 node ID. | 0x0 |
| Reserved | [31:24] | 8 | — | 0x0 |

**Update semantics** (resolved per ToO §ECC and protocol_rules.md `NI_CFG_LAST_ERR_INFO_CAPTURE`): **sticky** — first error since last clear is captured; subsequent errors do not overwrite until software clears via any `ERR_STATUS[0..2]` RW1C write. All three ERR_STATUS event classes (`ecc_uncorr_err`, `route_par_err`, `axi_parity_err`) qualify as "an error" for sticky-capture purposes — whichever fires first wins until cleared. Rationale: prevents losing the original triggering error while system processes subsequent cascaded errors. Test in dv/plan TP19.

## §PENDING_R_COUNT Register (0x130) Field Layout

| Field | Bit | Width | Description | Reset |
|-------|-----|-------|-------------|-------|
| `pending_r_count` | [n-1:0] | `n = ceil(log2(MAX_TXNS+1))` | NMU live outstanding read count. Increments on AR handshake completion at `axi_*_i`; decrements on R-with-`rlast` handshake completion at `axi_*_i`. `aclk_i`-native (no CDC). Range 0..`MAX_TXNS`. | 0x0 |
| Reserved | [31:n] | 32-n | — | 0x0 |

For default `MAX_TXNS=32` → `n=6`, field at `[5:0]`, Reserved at `[31:6]`. Field width adjusts at compile time when `MAX_TXNS` changes — always within 32-bit register, no `_HI` companion register needed.

Per `protocol_rules.md` `NI_CFG_PENDING_COUNT_ACCURACY`. Used by software during quiesce flow (poll `QUIESCE_STATUS.quiesce_idle` which depends on this counter; per ToO §"Software quiesce flow").

## §PENDING_W_COUNT Register (0x134) Field Layout

| Field | Bit | Width | Description | Reset |
|-------|-----|-------|-------------|-------|
| `pending_w_count` | [n-1:0] | `n = ceil(log2(MAX_TXNS+1))` | NMU live outstanding write count. Increments on AW handshake completion at `axi_*_i`; decrements on B handshake completion at `axi_*_i`. `aclk_i`-native (no CDC). Range 0..`MAX_TXNS`. | 0x0 |
| Reserved | [31:n] | 32-n | — | 0x0 |

Same width formula and design rationale as `PENDING_R_COUNT`. Per `protocol_rules.md` `NI_CFG_PENDING_COUNT_ACCURACY`.

## §QUIESCE_CTRL Register (0x13C) Field Layout

| Field | Bit | Width | Description | Reset |
|-------|-----|-------|-------------|-------|
| `quiesce_req` | [0] | 1 | Software requests NMU quiesce. `1`: NMU stops accepting new AW/AR (holds `axi_awready_o = axi_arready_o = 0`); existing in-flight transactions drain via normal response paths. `0`: resume normal NMU operation. NSU continues servicing inbound NoC requests in either state — quiesce is NMU-only by design. | 0x0 |
| Reserved | [31:1] | 31 | — | 0x0 |

Per `protocol_rules.md` `NI_CFG_QUIESCE_FLOW` and ToO §"Software quiesce flow".

## §QUIESCE_STATUS Register (0x140) Field Layout

| Field | Bit | Width | Description | Reset |
|-------|-----|-------|-------------|-------|
| `quiesce_idle` | [0] | 1 | Asserts when `(QUIESCE_CTRL.quiesce_req=1) AND (PENDING_R_COUNT=0) AND (PENDING_W_COUNT=0)`. Combinational over `aclk_i`-domain values (no CDC). Software polls this bit after writing `quiesce_req=1` to know when reconfig is safe. Polling is best-effort — no NI-side liveness guarantee in v0.4.0. If a slave hangs, `quiesce_idle` never asserts and software handles upper-bounded retry / reset externally. Deasserts on the cycle `quiesce_req` is cleared (because the AND-condition's first term goes false). | 0x0 |
| Reserved | [31:1] | 31 | — | 0x0 |

## §EXCLUSIVE_MONITOR_CTRL Register (0x144) Field Layout

| Field | Bit | Width | Description | Reset |
|-------|-----|-------|-------------|-------|
| `clear_all` | [0] | 1 | Write-1 self-clearing trigger: invalidates all pending NSU Exclusive Monitor entries on the `aclk_i` edge that completes the CSR write handshake (the "clear epoch" boundary). The bit self-clears on the next `aclk_i` edge after the clear epoch (latency = 1 cycle); subsequent reads return 0. Use case: OS bookkeeping when a process is killed mid-Exclusive. Race semantics with concurrent NSU events formalised in `protocol_rules.md` `NI_CFG_EXCLUSIVE_CLEAR_RACE`. | 0x0 |
| Reserved | [31:1] | 31 | — | 0x0 |

Access mode WO: writes to bit `[0]` accepted; writes to Reserved bits `[31:1]` silently ignored per §Reserved-bit policy. Reads return 0 always.

## §EXCLUSIVE_MONITOR_STATUS Register (0x148) Field Layout

| Field | Bit | Width | Description | Reset |
|-------|-----|-------|-------------|-------|
| `occupancy` | [m-1:0] | `m = ceil(log2(EXCLUSIVE_MONITOR_DEPTH+1))` | Live count of NSU Exclusive Monitor entries currently in `ALLOCATED` state. Range 0..`EXCLUSIVE_MONITOR_DEPTH`. `aclk_i`-native (same domain as the monitor itself). | 0x0 |
| Reserved | [31:m] | 32-m | — | 0x0 |

For default `EXCLUSIVE_MONITOR_DEPTH=8` → `m=4`, field at `[3:0]`, Reserved at `[31:4]`. Per `protocol_rules.md` `NI_CFG_EXCLUSIVE_OCCUPANCY_ACCURACY`.

## Counter saturation behavior

<!-- source: 06_qos.md §4.4 -->

All error counters and bin counters use **saturating arithmetic**: increment up to `2^W - 1`, then hold; no wrap-around. Clear mechanisms:

| Counter group | Clear mechanism |
|---|---|
| Error counters paired with `ERR_STATUS` bits (`ECC_UNCORR_ERR_CNT`, `ROUTE_PAR_ERR_CNT`, `AXI_PARITY_ERR_CNT`) | Software writes 1 to the corresponding `ERR_STATUS[N]` bit; counter clears atomically with the bit. |
| Correctable-ECC informational counter (`ECC_CORR_ERR_CNT`) | **No clear path** — pure saturating, cumulative since hardware reset (`arst_ni`). Software polls and tracks deltas. Rationale: single-bit corrections do not gate any IRQ source and are not part of the RW1C-clear contract; offering a clear-via-RW1C bit would inflate `ERR_STATUS` for an event class software typically samples rather than reacts to. |
| Packet Probe counters (`PKT_BYTE_COUNT`, `PKT_BANDWIDTH`) | Software writes `PKT_PROBE_EN = 0` then `PKT_PROBE_EN = 1`; on the 0→1 transition, counters reset to 0. |
| Transaction Probe counters (`TXN_BIN_*_COUNT`, `TXN_TOTAL_COUNT`) | Same — `TXN_PROBE_EN` 1→0→1 transition resets all bin counters and TXN_TOTAL_COUNT to 0 |
| Latency extremes (`TXN_MIN_LATENCY`, `TXN_MAX_LATENCY`) | Same — `TXN_PROBE_EN` 1→0→1 resets MIN to 0xFFFF and MAX to 0x0 |

## Cross-reference to behavior
For ECC error counter triggering (ECC_UNCORR_ERR_CNT / ECC_CORR_ERR_CNT / ROUTE_PAR_ERR_CNT), see [Theory of Operation §ECC](./theory_of_operation.md#ecc).
For AXI host-side parity check → `ERR_STATUS[2]` triggering, see protocol_rules.md `AXI4_MST_PARITY_CHECK` (NMU-side) and `AXI4_SLV_PARITY_CHECK` (NSU-side).
For IRQ assertion behaviour, see protocol_rules.md `NI_IRQ_LEVEL`.
For Probe counter update timing, see protocol_rules.md `NI_CFG_PROBE_PKT_BYTE_COUNT` and `NI_CFG_PROBE_TXN_LATENCY` for cycle-level update specification.
For NMU outstanding-transaction count exposure (`PENDING_R_COUNT` / `PENDING_W_COUNT`), see [Theory of Operation §Software quiesce flow](./theory_of_operation.md#software-quiesce-flow) and protocol_rules.md `NI_CFG_PENDING_COUNT_ACCURACY`.
For NMU quiesce flow (`QUIESCE_CTRL` / `QUIESCE_STATUS`), see [Theory of Operation §Software quiesce flow](./theory_of_operation.md#software-quiesce-flow) and protocol_rules.md `NI_CFG_QUIESCE_FLOW`.
For NSU Exclusive Monitor CSR clear / observability (`EXCLUSIVE_MONITOR_CTRL` / `EXCLUSIVE_MONITOR_STATUS`), see [Theory of Operation §NSU Exclusive Monitor](./theory_of_operation.md#nsu-exclusive-monitor-nsu-sub-block) and protocol_rules.md `NI_CFG_EXCLUSIVE_CLEAR_RACE` / `NI_CFG_EXCLUSIVE_OCCUPANCY_ACCURACY`.
