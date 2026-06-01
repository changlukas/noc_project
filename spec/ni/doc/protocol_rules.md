# Protocol Rules

**Protocols:**
- AXI4 (ARM IHI 0022) on the host side
- Custom flit-based packet protocol on the NoC side
- AXI4-Lite on the CSR access port

**Roles:** Both MST (NMU initiates AXI on remote-side egress; NSU initiates AXI to local memory) and SLV (NMU receives AXI from local master; NSU receives via NoC). MON (passive monitoring) supported.

**ID format:** Two legal variants — pick based on whether the rule's protocol has channels.

  - **Channel-based protocols** (AXI4): `<PROTO>_<ROLE>_<CHANNEL>_<SHORT_NAME>`
  - **Channel-less protocols** (NoC flit, CSR-via-AXI4-Lite): `<PROTO>_<ROLE>_<SHORT_NAME>`

Component definitions:
- **PROTO**: `AXI4` (host AXI4), `NOC` (NoC packet protocol), `AXI4LITE` (CSR access), `NI` (device-level NI behavior not bound to a single bus protocol)
- **ROLE**: `MST` (master / master), `SLV` (slave / slave), `MON` (monitor-only)
- **CHANNEL**: AXI4 channel (`AW`/`W`/`B`/`AR`/`R`); or `RST`, `XCH` (cross-channel), `CFG` (configuration knob)
- **SHORT_NAME**: snake_case

**NI device-level rules:** `NI_*` rule IDs describe NI behavior not tied to one bus protocol. They use subsystem-category tokens in place of the AXI ROLE / CHANNEL slots: `NI_RST_*` for reset behavior, `NI_CDC_*` for clock-domain crossing, `NI_CFG_*` for CSR / configuration-visible behavior, `NI_IRQ_*` for interrupt behavior. These are intentional non-AXI rule families, not malformed AXI IDs.

**Severity legend (RFC 2119, IETF):**
- **MUST / MUST NOT** — unconditional requirement; protocol violation if unmet.
- **SHOULD / SHOULD NOT** — recommended practice; deviation allowed with justification.
- **MAY** — permitted but not required.

**ARM SVA equivalent column convention:**
- Verified ARM Protocol Checker IDs listed verbatim
- IDs suffixed `(unverified)` follow ARM naming pattern but await cross-check against ARM DUI 0534B / DUI 0576A
- `(none)` for rules without ARM equivalent (NoC custom protocol, CFG, RST-duration, XCH cross-protocol, etc.)

## Channel naming convention

Rule IDs and sub-section headings in this document use **abstract channel tokens** (`AW`, `W`, `B`, `AR`, `R` for AXI4; wildcard `noc_*_o`/`noc_*_i` patterns for NoC) that alias the per-port / per-direction channels declared in `signal_interface.md` §Channel grouping:

| Token in this document | Aliases (signal_interface.md §Channel grouping) |
|------------------------|--------------------------------------------------|
| `AW` (in rule ID or §AW channel heading) | `AW_IN` (slave port) and `AW_OUT` (master port) — rule applies to whichever port the rule's `<ROLE>` (MST / SLV) selects |
| `W` | `W_IN` and `W_OUT` |
| `B` | `B_IN` and `B_OUT` |
| `AR` | `AR_IN` and `AR_OUT` |
| `R` | `R_IN` and `R_OUT` |
| `noc_*_o.valid` / `noc_*_o.ready` patterns | `REQ_OUT` and `RSP_OUT` (BFM-driven NoC outputs) |
| `noc_*_i.valid` / `noc_*_i.ready` patterns | `REQ_IN` and `RSP_IN` (BFM-observed NoC inputs) |
| `CSR_AW` / `CSR_W` / `CSR_B` / `CSR_AR` / `CSR_R` (in §CSR sub-section headings) | one-to-one with signal_interface tokens; no aliasing needed |

A rule with role `SLV` referencing `AW` applies at the BFM's slave-side AXI port (`axi_*_i` for NMU's slave port acting as slave to local AXI master, `axi_*_o` for NSU's master port). A rule with role `MST` referencing `AW` applies at the BFM's master-side AXI port. NoC `<PROTO>_MST_*` rules apply to BFM-driven NoC outputs (`REQ_OUT` and `RSP_OUT`); `<PROTO>_SLV_*` rules apply to BFM-observed NoC inputs (`REQ_IN` and `RSP_IN`).

## Reset rules

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| NI_RST_OUTPUTS_LOW_AXI | `arst_ni` is asserted | All NI-driven AXI outputs (axi_rsp_o.*valid, axi_req_o.*valid) must be at their during-reset values per pin_level_reset.md. | MUST | (none) |
| NI_RST_OUTPUTS_LOW_NOC | `noc_rst_ni` is asserted | All NI-driven NoC outputs (`noc_req_valid_o`, `noc_rsp_valid_o`, `noc_req_credit_o[NUM_VC-1:0]`, `noc_rsp_credit_o[NUM_VC-1:0]`, `noc_req_credit_init_ready_o`, `noc_rsp_credit_init_ready_o`) must be 0. | MUST | (none) |
| NI_RST_DURATION_AXI | `arst_ni` pulse begins | Held LOW for ≥ 16 `aclk_i` cycles. Shorter pulses leave the AXI side in undefined state. | MUST | (none) |
| NI_RST_DURATION_NOC | `noc_rst_ni` pulse begins | Held LOW for ≥ 16 `noc_clk_i` cycles. | MUST | (none) |
| NI_RST_PARTIAL | One reset asserted, the other not | NI may operate in partial state; cross-domain in-flight transactions will not complete. Integrator should ensure the two resets reach a consistent state by power-on completion. | SHOULD | (none) |

## CDC rules

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| NI_CDC_AXI_TO_NOC_FIFO | AXI ingress (NMU AW/W/AR or NSU B/R reception) crosses to NoC injection | NI uses an async FIFO (gray-counter pointer + 2FF sync) to bridge `aclk_i` → `noc_clk_i`. FIFO depth = `CDC_FIFO_DEPTH` parameter (default 16). The default is conservative for the supported clock-ratio range `aclk_period : noc_clk_period ∈ [0.1, 10]`; outside that range, integrator must size per the formula `2 × max_round_trip_cycles × max(aclk_period, noc_clk_period) / min(aclk_period, noc_clk_period) + 2`. The formula is documented in `signal_interface.md` §Parameters under `CDC_FIFO_DEPTH`. | MUST | (none) |
| NI_CDC_NOC_TO_AXI_FIFO | NoC ingress (NMU response reception or NSU request reception) crosses to AXI egress | Mirror of above; `noc_clk_i` → `aclk_i` async FIFO. | MUST | (none) |
| NI_CDC_NO_COMBINATIONAL_PATH | Any wire path | No combinational path crosses the AXI ↔ NoC clock boundary inside NI. All cross-domain signals are FIFO'd or 2FF-synchronized. | MUST NOT | (none) |

## AXI4 host-side rules

> Scope: AXI4 base, 5 channels (AW/W/B/AR/R), burst constraints, ordering, exclusive access; no AXI5/ACE/atomics.
> All rules use RFC 2119 conformance keywords.

### AW channel

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| AXI4_AW_VALID_STABLE | AWVALID is asserted and AWREADY has not yet been asserted in the same cycle. | All AW channel outputs (AWID, AWADDR, AWLEN, AWSIZE, AWBURST, AWLOCK, AWCACHE, AWPROT, AWQOS, AWREGION, AWUSER) MUST remain stable on every cycle until the handshake completes (AWVALID && AWREADY in the same cycle). | MUST | §A3.2.1 |
| AXI4_AW_VALID_NO_WAIT | A master wishes to initiate a write address transaction. | AWVALID MUST NOT be held deasserted solely because AWREADY is deasserted; a master must raise AWVALID independently of the slave's AWREADY. | MUST NOT | §A3.2.2 |
| AXI4_AW_LEN_ENCODING | Any AW channel transaction is issued. | The number of data transfers in a write burst equals AWLEN+1; AWLEN is in the range 0–255 for INCR bursts, 0–15 (burst lengths 1–16) for FIXED bursts, and restricted to 1/3/7/15 (burst lengths 2/4/8/16) for WRAP bursts. | MUST | §A3.4.1, §A3.4.2 |
| AXI4_AW_SIZE_BOUND | Any AW channel transaction is issued. | 2^AWSIZE bytes MUST NOT exceed the data bus width (AXI_DATA_WIDTH/8). AWSIZE MUST be in the range 0–7, and the encoded transfer size MUST NOT exceed the bus width. For this implementation with AXI_DATA_WIDTH=256, AWSIZE is in the range 0–5 (1–32 bytes per beat). | MUST | §A3.4.1 |
| AXI4_AW_BURST_ENCODING | Any AW channel transaction is issued. | AWBURST[1:0] MUST be one of 2'b00 (FIXED), 2'b01 (INCR), or 2'b10 (WRAP). The encoding 2'b11 is reserved and MUST NOT be used. | MUST NOT | §A3.4.2 |
| AXI4_AW_WRAP_ALIGN | A WRAP burst (AWBURST=2'b10) is issued. | The start address AWADDR MUST be aligned to AWSIZE-byte boundaries (i.e., AWADDR mod 2^AWSIZE == 0). | MUST | §A3.4.3 |
| AXI4_AW_RESET | System reset (arst_ni deasserted active-low). | AWVALID MUST be deasserted (driven 0) during reset and MUST remain deasserted until at least one clock cycle after reset is released. | MUST | §A3.1.2 |

### W channel

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| AXI4_W_VALID_STABLE | WVALID is asserted and WREADY has not yet been asserted in the same cycle. | All W channel outputs (WDATA, WSTRB, WLAST, WUSER) MUST remain stable on every cycle until the handshake completes (WVALID && WREADY in the same cycle). | MUST | §A3.2.1 |
| AXI4_W_VALID_NO_WAIT | A master issues write data. | WVALID MUST NOT be held deasserted solely because WREADY is deasserted. | MUST NOT | §A3.2.2 |
| AXI4_W_LAST_TIMING | A write burst (AWLEN+1 beats) is in progress. | WLAST MUST be asserted exactly on the final data beat of a burst — the beat numbered AWLEN+1 counting from the first accepted W beat — and MUST NOT be asserted on any earlier beat. | MUST | §A3.2.2, §A3.4.1 |
| AXI4_W_BEAT_COUNT | A write transaction is accepted on the AW channel. | The W channel MUST deliver exactly AWLEN+1 data beats, one per W handshake, before WLAST is accepted. Neither more nor fewer beats are permitted for a single write transaction. | MUST | §A3.4.1 |
| AXI4_W_STRB_VALID | Any W beat is transferred. | Each WSTRB bit corresponds to a byte lane; a deasserted WSTRB[n] indicates byte lane n contains no valid data. WSTRB width MUST equal AXI_DATA_WIDTH/8 (here: 32 bits for 256-bit data bus). All WSTRB bits MAY be 0 to indicate a null transfer for that beat. | MUST | §A3.4.3 |
| AXI4_W_NO_INTERLEAVE | Multiple write transactions are outstanding on the W channel. | Write data for different write transactions MUST NOT be interleaved on the W channel. All W beats for one write transaction MUST be sent consecutively before W beats of the next transaction are issued. AXI4 removes AXI3 write data interleaving; the WID field does not exist in AXI4. | MUST NOT | §A5.2.2, §A5.4 |
| AXI4_W_STRB_SPARSE_LEGAL | Any W beat is transferred with a narrow or partial transfer. | WSTRB MAY be sparse (not all asserted). However, any asserted WSTRB bit MUST correspond only to a valid byte lane for the transfer's address and size combination — active strobes MUST fall within the transfer's active byte range as defined by address and AWSIZE. | MUST | §A3.4.3 |

### B channel

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| AXI4_B_VALID_STABLE | BVALID is asserted and BREADY has not yet been asserted in the same cycle. | BID, BRESP, and BUSER MUST remain stable on every cycle until the handshake completes (BVALID && BREADY in the same cycle). | MUST | §A3.2.1 |
| AXI4_B_ID_MATCH | A write transaction completes (all AWLEN+1 W beats accepted). | BID MUST equal the AWID of the write transaction to which this response belongs. A slave MUST NOT issue a B response with a BID value that does not correspond to an outstanding write transaction. | MUST | §A5.3 |
| AXI4_B_RESP_ENCODING | Any B channel response is issued. | BRESP[1:0] MUST be one of 2'b00 (OKAY), 2'b01 (EXOKAY), 2'b10 (SLVERR), or 2'b11 (DECERR). EXOKAY is only valid for exclusive write transactions (AWLOCK=1). | MUST | §A3.4.4, §A7.2 |
| AXI4_B_ONE_RESPONSE_PER_WRITE | A write transaction is in progress. | Exactly one B response MUST be issued per write transaction. Multiple write transactions may share the same AWID; each such transaction independently receives its own B response in the order its AW was accepted. No partial or multiple B responses per transaction are permitted. | MUST | §A5.3 |
| AXI4_B_RESET | System reset (arst_ni deasserted). | BVALID MUST be deasserted during reset. | MUST | §A3.1.2 |

### AR channel

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| AXI4_AR_VALID_STABLE | ARVALID is asserted and ARREADY has not yet been asserted in the same cycle. | All AR channel outputs (ARID, ARADDR, ARLEN, ARSIZE, ARBURST, ARLOCK, ARCACHE, ARPROT, ARQOS, ARREGION, ARUSER) MUST remain stable on every cycle until the handshake completes (ARVALID && ARREADY in the same cycle). | MUST | §A3.2.1 |
| AXI4_AR_VALID_NO_WAIT | A master wishes to initiate a read address transaction. | ARVALID MUST NOT be held deasserted solely because ARREADY is deasserted. | MUST NOT | §A3.2.2 |
| AXI4_AR_LEN_ENCODING | Any AR channel transaction is issued. | The number of data transfers in a read burst equals ARLEN+1; ARLEN is in the range 0–255 for INCR bursts, 0–15 (burst lengths 1–16) for FIXED bursts, and restricted to 1/3/7/15 (burst lengths 2/4/8/16) for WRAP bursts. | MUST | §A3.4.1, §A3.4.2 |
| AXI4_AR_SIZE_BOUND | Any AR channel transaction is issued. | 2^ARSIZE bytes MUST NOT exceed the data bus width (AXI_DATA_WIDTH/8). ARSIZE MUST be in the range 0–7, and the encoded transfer size MUST NOT exceed the bus width. For this implementation with AXI_DATA_WIDTH=256, ARSIZE is in the range 0–5 (1–32 bytes per beat). | MUST | §A3.4.1 |
| AXI4_AR_BURST_ENCODING | Any AR channel transaction is issued. | ARBURST[1:0] MUST be 2'b00 (FIXED), 2'b01 (INCR), or 2'b10 (WRAP). The encoding 2'b11 is reserved and MUST NOT be used. | MUST NOT | §A3.4.2 |
| AXI4_AR_WRAP_ALIGN | A WRAP burst (ARBURST=2'b10) is issued. | ARADDR MUST be aligned to 2^ARSIZE-byte boundaries (ARADDR mod 2^ARSIZE == 0). | MUST | §A3.4.3 |
| AXI4_AR_RESET | System reset (arst_ni deasserted). | ARVALID MUST be deasserted during reset and MUST remain deasserted until at least one clock cycle after reset is released. | MUST | §A3.1.2 |

### R channel

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| AXI4_R_VALID_STABLE | RVALID is asserted and RREADY has not yet been asserted in the same cycle. | RID, RDATA, RRESP, RLAST, and RUSER MUST remain stable on every cycle until the handshake completes (RVALID && RREADY in the same cycle). | MUST | §A3.2.1 |
| AXI4_R_ID_MATCH | A read burst is in progress. | RID MUST equal the ARID of the read transaction whose data is being returned. All beats of a single read transaction MUST carry the same RID. | MUST | §A5.2 |
| AXI4_R_LAST_TIMING | A read burst (ARLEN+1 beats) is in progress. | RLAST MUST be asserted exactly on the final data beat of a burst — beat number ARLEN+1 counting from the first accepted R beat — and MUST NOT be asserted on any earlier beat of the same burst. | MUST | §A3.2.2, §A3.4.1 |
| AXI4_R_BEAT_COUNT | A read transaction is accepted on the AR channel. | The R channel MUST deliver exactly ARLEN+1 data beats before RLAST is accepted; neither more nor fewer beats are permitted per transaction. | MUST | §A3.4.1 |
| AXI4_R_RESP_ENCODING | Any R channel beat is transferred. | RRESP[1:0] MUST be one of 2'b00 (OKAY), 2'b01 (EXOKAY), 2'b10 (SLVERR), or 2'b11 (DECERR). For an exclusive read transaction (ARLOCK=1), a supporting subordinate MAY return EXOKAY on any beat; EXOKAY is not restricted to the last beat (RLAST=1) only. A subordinate MAY return SLVERR or DECERR on any beat regardless of ARLOCK. | MUST | §A3.4.4, §A7.2, §A7.2.5 |
| AXI4_R_RESET | System reset (arst_ni deasserted). | RVALID MUST be deasserted during reset. | MUST | §A3.1.2 |

### Burst constraints

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| AXI4_BURST_NO_4KB_CROSS | Any AW or AR channel transaction is issued. | A burst transaction MUST NOT cross a 4KB address boundary. The addresses of all beats within a burst MUST fall within the same aligned 4KB region as the start address. | MUST NOT | §A3.4.1 |

### Ordering

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| AXI4_ORDER_SAME_ID_W | Two write transactions with the same AWID are issued by the same master. | Write responses (B channel) for transactions with the same AWID MUST be returned in the order the AW channel accepted those transactions. | MUST | §A5.3 |
| AXI4_ORDER_SAME_ID_R | Two read transactions with the same ARID are issued by the same master. | Read data (R channel) for transactions with the same ARID MUST be returned in the order the AR channel accepted those transactions. | MUST | §A5.2 |
| AXI4_ORDER_DIFF_ID_INTERLEAVE | Two read transactions with different ARIDs are outstanding. | A slave MAY interleave R channel beats from different ARIDs. The NMU's RoB (NormalRoB / SimpleRoB mode) is responsible for re-ordering or holding interleaved responses before forwarding to the initiating master. | MAY | §A5.2 |
| AXI4_ORDER_W_BEFORE_B | A write transaction's AW and W channel activity is in progress. | A slave MUST NOT issue a B response before all AWLEN+1 W beats of the corresponding write transaction have been accepted (WVALID && WREADY on the last beat with WLAST=1). | MUST NOT | §A3.3.1 |
| AXI4_ORDER_AW_W_INDEPENDENCE | A master issues AW and W channel transactions. | A manager MUST NOT wait for AWREADY to be asserted before asserting AWVALID. A manager MUST NOT wait for WREADY to be asserted before asserting WVALID. AW and W are independent channels; holding either VALID pending the other's READY creates deadlock risk. | MUST NOT | §A3.3.1 |

### Exclusive access

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| AXI4_EX_LOCK_ENCODING | Any AW or AR transaction is issued. | AWLOCK/ARLOCK MUST be 1'b0 (Normal) or 1'b1 (Exclusive). AXI4 uses a 1-bit LOCK field; the AXI3 Locked encoding (2'b10) MUST NOT be used. | MUST NOT | §A7.1 |
| AXI4_EX_READ_BEFORE_WRITE | A master initiates an exclusive write sequence. | An exclusive write (AWLOCK=1) MUST be preceded by an exclusive read (ARLOCK=1) that forms a matched exclusive sequence. The exclusive write and the exclusive read MUST agree on all of the §A7.2.4 matching attributes: AxID, AxADDR, AxREGION, AxLEN, AxSIZE, AxBURST, AxLOCK, AxCACHE, AxPROT. An exclusive write without a prior matched exclusive read is a protocol violation. | MUST | §A7.2.2, §A7.2.4 |
| AXI4_EX_SIZE_MATCH | A paired exclusive read and write sequence is in progress. | AWSIZE and AWLEN MUST match the ARSIZE and ARLEN of the paired exclusive read, per the §A7.2.4 required attribute set (AxID, AxADDR, AxREGION, AxLEN, AxSIZE, AxBURST, AxLOCK, AxCACHE, AxPROT). WSTRB byte-lane equality with the exclusive read is NOT required by the AMBA specification and MUST NOT be enforced as a matching criterion. | MUST | §A7.2.3, §A7.2.4 |
| AXI4_EX_RESPONSE_EXOKAY | The exclusive monitor detects a valid exclusive reservation at the target address and no intervening write has invalidated it. | For a subordinate that supports exclusive access, a successful exclusive write MUST return EXOKAY on the B channel and MUST update memory. A failed exclusive write (reservation lost) MUST return OKAY and MUST NOT update the addressed memory location. A subordinate that does not support exclusive access MUST return OKAY and need not update memory. | MUST | §A7.2.3, §A7.2.5 |
| AXI4_EX_NO_EXOKAY_NON_EXCLUSIVE | A non-exclusive write (AWLOCK=0) or non-exclusive read (ARLOCK=0) is issued. | A slave MUST NOT return EXOKAY for a transaction in which AWLOCK=0 (or ARLOCK=0 on the R channel). EXOKAY is only a valid response for exclusive transactions. | MUST NOT | §A7.2 |
| AXI4_EX_MASTER_RETRY | A master receives OKAY (not EXOKAY) in response to an exclusive write. | A master MAY retry the exclusive sequence (exclusive read followed by exclusive write) any number of times. Retry is a manager implementation policy; no normative retry obligation is imposed by the AMBA AXI4 specification. | MAY | §A7.2 |
| AXI4_EX_ADDR_ALIGNED | An exclusive access transaction (AWLOCK=1 or ARLOCK=1) is issued. | The exclusive access address (AWADDR for write, ARADDR for read) MUST be naturally aligned to the total transfer size in bytes: address MUST be divisible by (AXLEN+1) × 2^AXSIZE. | MUST | §A7.2.4 |
| AXI4_EX_TOTAL_BYTES_POW2 | An exclusive access transaction (AWLOCK=1 or ARLOCK=1) is issued. | The total bytes transferred in an exclusive access — (AXLEN+1) × 2^AXSIZE — MUST be a power of 2 and MUST NOT exceed 128 bytes. Consequently, AWLEN/ARLEN MUST be ≤ 15 (burst length ≤ 16 beats) for exclusive transactions. | MUST | §A7.2.4 |
| AXI4_EX_SEQ_ATTR_MATCH | A paired exclusive read and exclusive write sequence is being evaluated for a match. | The exclusive read and the corresponding exclusive write MUST agree on all of the following attributes: AxID, AxADDR, AxREGION, AxLEN, AxSIZE, AxBURST, AxLOCK, AxCACHE, AxPROT. Any mismatch in this set means the pair is not a valid exclusive sequence; EXOKAY MUST NOT be returned for a mismatched pair. | MUST | §A7.2.4 |
| AXI4_EX_R_RESP_CONSISTENCY | A single exclusive read transaction (ARLOCK=1) is in progress across multiple R beats. | The R beats of a single exclusive read transaction MUST NOT mix OKAY and EXOKAY response codes. All beats MUST consistently return EXOKAY (indicating the exclusive read is accepted), or the subordinate MUST return a single non-EXOKAY response (OKAY, SLVERR, or DECERR) consistently for the transaction. | MUST NOT | §A7.2.5 |

## NoC flit-side rules

> Scope: credit-based flow control invariants only. Handshake / credit return / send-timing.
> Arbitration policy, VC ordering, async CDC, error recovery are out of scope for this section
> (separate concerns; covered elsewhere or by RTL design).

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| NOC_RESET_CREDIT_PRELOAD | Source comes out of reset (`noc_rst_ni` rising edge). | The source credit counter for each VC MUST be loaded to `VCDepth` (per-VC buffer depth at the destination) from a compile-time parameter. No runtime credit-init handshake is required. | MUST | (none — static preload, no protocol assertion) |
| NOC_SEND_REQUIRES_CREDIT | A source attempts to send a flit on virtual channel `vc_id`. | The source MUST NOT assert `*_valid_o` (or pulse the equivalent) unless `credit_cnt[vc_id] > 0`. | MUST NOT | `assert property (@posedge clk) noc_*_valid_o |-> credit_cnt[vc_id] > 0;` |
| NOC_CREDIT_DECREMENT_PER_FLIT | A flit handshake completes (`*_valid_o` accepted by destination). | The source `credit_cnt[vc_id]` MUST decrement by exactly 1 in the same cycle (or next cycle, registered) for the flit's VC. No multi-credit-per-flit consumption. | MUST | (per-VC counter assertion) |
| NOC_CREDIT_RETURN_PER_FREED_SLOT | A destination buffer slot for VC `vc_id` becomes available (slot exits FIFO). | The destination MUST return exactly one credit pulse on `*_credit_o[vc_id]` per freed slot. One pulse equals one slot. | MUST | `assert property (@posedge clk) (slot_freed[vc_id]) |-> ##[0:N] noc_*_credit_o[vc_id];` |
| NOC_CREDIT_RETURN_AT_SLOT_FREE | The destination is considering returning a credit. | The destination MUST NOT return a credit before the corresponding receive slot has actually been freed (no speculative or anticipatory credit return). | MUST NOT | (none — invariant) |
| NOC_PER_VC_ACCOUNTING | The source maintains credit counts across multiple VCs. | The source MUST maintain an independent `credit_cnt[vc_id]` for each VC. A VC at zero credit MUST block only that VC, not any other VC. | MUST | (per-VC structural) |
| NOC_FINITE_CREDITS | Credit accounting is in steady-state operation. | The credit counter for each VC MUST be in the range `[0, VCDepth]`. The value 0 means "no credit available" (NOT infinite). Infinite-credit modes (PCIe/CHI style) are NOT permitted. | MUST | `assert property (credit_cnt[vc_id] <= VCDepth);` |

## CSR access (AXI4-Lite) rules

The CSR access port is AXI4-Lite slave. AXI4-Lite is a subset of AXI4: single-beat transactions (no AWLEN/ARLEN/AWBURST/ARBURST), no AWID/ARID, fixed-size accesses. Standard AXI4-Lite STABLE / VALUES rules apply.

### CSR write channels (AW + W + B)

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| AXI4LITE_SLV_AW_AWVALID_STABLE | csr_awvalid_i rises HIGH | csr_awvalid_i must remain HIGH until csr_awready_o observed HIGH. | MUST | AXI4LITE_ERRM_AWVALID_STABLE (unverified) |
| AXI4LITE_SLV_AW_AWADDR_STABLE | csr_awvalid_i is HIGH | csr_awaddr_i must not change. | MUST | AXI4LITE_ERRM_AWADDR_STABLE (unverified) |
| AXI4LITE_SLV_AW_AWPROT_STABLE | csr_awvalid_i is HIGH | csr_awprot_i must not change. | MUST | AXI4LITE_ERRM_AWPROT_STABLE (unverified) |
| AXI4LITE_SLV_AW_AWADDR_ALIGNED | csr_awvalid_i + csr_awready_o handshake | csr_awaddr_i must be 4-byte-aligned (lower 2 bits = 0). Misaligned writes cause `csr_bresp_o=SLVERR`. | MUST | (none) |
| AXI4LITE_SLV_W_WVALID_STABLE | csr_wvalid_i rises HIGH | Until csr_wready_o observed HIGH. | MUST | AXI4LITE_ERRM_WVALID_STABLE (unverified) |
| AXI4LITE_SLV_W_WDATA_STABLE | csr_wvalid_i is HIGH | csr_wdata_i must not change. | MUST | (unverified) |
| AXI4LITE_SLV_W_WSTRB_STABLE | csr_wvalid_i is HIGH | csr_wstrb_i must not change. | MUST | (unverified) |
| AXI4LITE_SLV_B_BVALID_STABLE | csr_bvalid_o rises HIGH | Until csr_bready_i observed HIGH. | MUST | AXI4LITE_ERRS_BVALID_STABLE (unverified) |
| AXI4LITE_SLV_B_BRESP_VALUES | csr_bvalid_o + csr_bready_i handshake | csr_bresp_o ∈ {OKAY=2'b00, SLVERR=2'b10, DECERR=2'b11}. SLVERR for misaligned address or RW1C-write-to-RO-bit; DECERR for unmapped offset. | MUST | (none) |
| AXI4LITE_SLV_XCH_W_AFTER_AW | csr_wready_o asserted | Corresponding csr_awvalid_i + csr_awready_o handshake must have completed (or be on the same cycle). | MUST | (none) |
| AXI4LITE_SLV_XCH_B_AFTER_AW_AND_W | csr_bvalid_o asserted | Both AW and W phases of the same write must have completed. | MUST | (none) |

### CSR read channels (AR + R)

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| AXI4LITE_SLV_AR_ARVALID_STABLE | csr_arvalid_i rises HIGH | Until csr_arready_o observed HIGH. | MUST | (unverified) |
| AXI4LITE_SLV_AR_ARADDR_STABLE | csr_arvalid_i is HIGH | csr_araddr_i must not change. | MUST | (unverified) |
| AXI4LITE_SLV_AR_ARPROT_STABLE | csr_arvalid_i is HIGH | csr_arprot_i must not change. | MUST | (unverified) |
| AXI4LITE_SLV_AR_ARADDR_ALIGNED | csr_arvalid_i + csr_arready_o handshake | csr_araddr_i must be 4-byte-aligned. Misaligned reads cause `csr_rresp_o=SLVERR`. | MUST | (none) |
| AXI4LITE_SLV_R_RVALID_STABLE | csr_rvalid_o rises HIGH | Until csr_rready_i observed HIGH. | MUST | (unverified) |
| AXI4LITE_SLV_R_RDATA_STABLE | csr_rvalid_o is HIGH | csr_rdata_o must not change. | MUST | (unverified) |
| AXI4LITE_SLV_R_RRESP_STABLE | csr_rvalid_o is HIGH | csr_rresp_o must not change. | MUST | (unverified) |
| AXI4LITE_SLV_R_RRESP_VALUES | csr_rvalid_o + csr_rready_i handshake | csr_rresp_o ∈ {OKAY, SLVERR, DECERR}. DECERR for unmapped offset. | MUST | (none) |
| AXI4LITE_SLV_R_RLAST_NOT_REQUIRED | csr_rvalid_o + csr_rready_i handshake | AXI4-Lite reads are single-beat; no RLAST signal. (NI's CSR port omits csr_rlast.) | MUST | (none) |
| AXI4LITE_SLV_XCH_R_AFTER_AR | csr_rvalid_o asserted | Corresponding csr_arvalid_i + csr_arready_o handshake must have completed. | MUST | (none) |

### CSR address policy

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| AXI4LITE_SLV_UNMAPPED_DECERR | Read or write to a CSR offset not listed in registers.md §Register map | csr_bresp_o=DECERR (for write) or csr_rresp_o=DECERR (for read). | MUST | (none) |
| AXI4LITE_SLV_RO_WRITE_IGNORED | Write to a Read-Only register (per registers.md Access column) | Write data is silently ignored; csr_bresp_o=OKAY (write succeeds at the bus level but has no effect). Software contract: don't write to RO. | SHOULD | (none) |
| AXI4LITE_SLV_RW1C_WRITE_BIT_LEVEL | Write to a RW1C register | For each bit position: software writes 1 → bit clears + associated counter clears (per registers.md §ERR_STATUS); software writes 0 → no effect (bit retains current state). | MUST | (none) |

## Configuration-knob rules

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| NI_CFG_QOS_MODE_TRANSITION | `QOS_MODE` CSR written | New mode applies to the NEXT AW/AR flit injection; in-flight transactions retain the QoS computed at their injection time. | MUST | (none) |
| NI_CFG_QOS_FIXED_VALUE | `QOS_MODE = Fixed`; `QOS_FIXED_VALUE` CSR written | Next AW/AR flit's `qos` header field equals `QOS_FIXED_VALUE`, regardless of AXI awqos/arqos input. | MUST | (none) |
| NI_CFG_BANDWIDTH_LIMIT_BOUND | `BANDWIDTH_LIMIT` CSR written; `QOS_MODE = Limiter` | Limiter counter increments per request bytes, decrements per cycle by `BANDWIDTH_LIMIT`; QoS drops to `LOW_PRIORITY` when counter > `SATURATION_THRESHOLD`. Saturating arithmetic. | MUST | (none) |
| NI_CFG_BANDWIDTH_BUDGET_BOUND | `BANDWIDTH_BUDGET` CSR written; `QOS_MODE = Regulator` | Per cycle: counter += response_bytes − BANDWIDTH_BUDGET. Urgency adjusts per `BASE_QOS[5:4]` (URGENCY_STEP) per cycle: counter<0 → urgency increases; counter>0 → urgency decreases (saturating to 0..MAX_URGENCY). When software writes URGENCY_STEP=0 to `BASE_QOS[5:4]`, hardware treats the field as if it were 1 (effective minimum step is 1; legal SW-visible values are 1..3). | MUST | (none) |
| NI_CFG_REGULATOR_FINAL_QOS | `QOS_MODE = Regulator`; AW/AR flit being injected | flit.hdr.qos = max(min(BASE_QOS[3:0] + urgency_level, 15), `SOCKET_QOS_EN ? SOCKET_QOS : 0`). Saturation arithmetic; clamps to 4-bit range. | MUST | (none) |
| NI_CFG_PROBE_EN_TRANSITION | `PKT_PROBE_EN` or `TXN_PROBE_EN` CSR transitions 0→1 | Probe counters start counting from the next cycle; previous count state is preserved (not auto-cleared). To clear, software must explicitly write 0 to the count register or rely on saturating wrap-around. | MUST | (none) |
| NI_CFG_PROBE_PKT_BYTE_COUNT | `PKT_PROBE_EN=1`; AW or AR flit injected (depends on PKT_PROBE_MODE) | `PKT_BYTE_COUNT` increments by `(awlen+1) × (1 << awsize)` for writes (PKT_PROBE_MODE=0 or 2) or `(arlen+1) × (1 << arsize)` for reads (PKT_PROBE_MODE=0 or 1). Saturating. | MUST | (none) |
| NI_CFG_PROBE_TXN_LATENCY | `TXN_PROBE_EN=1`; B response or final R beat received | Latency = (response cycle) − (request injection cycle). Increment `TXN_BIN_<i>_COUNT` where bin `i` is the smallest index with `latency < TXN_THRESHOLD_<i>` (or final bin if larger than all thresholds). Update `TXN_MIN_LATENCY` / `TXN_MAX_LATENCY` / `TXN_TOTAL_COUNT`. | MUST | (none) |
| NI_CFG_ERR_STATUS_RW1C | Software writes 1 to `ERR_STATUS[i]` (i ∈ {0..2} = {ecc_uncorr_err, route_par_err, axi_parity_err}) | Bit `[i]` and the associated saturating counter are cleared atomically on the cycle the AXI4-Lite write handshake completes. Counter map: `ECC_UNCORR_ERR_CNT` (i=0), `ROUTE_PAR_ERR_CNT` (i=1), `AXI_PARITY_ERR_CNT` (i=2). The clear also deasserts the corresponding IRQ source if `IRQ_ENABLE[i]` was set. | MUST | (none) |
| NI_CFG_LAST_ERR_INFO_CAPTURE | Any of the three `ERR_STATUS` event classes fires while no prior un-cleared error is sticky: ECC double-bit at NI sink (per `NOC_FLIT_HDR_FLIT_ECC_CHECK`), route_par drop (per `NOC_FLIT_HDR_ROUTE_PAR_CHECK`), AXI parity mismatch (per `AXI4_MST_PARITY_CHECK` / `AXI4_SLV_PARITY_CHECK`) | `LAST_ERR_INFO` register captures the offending transaction's `err_axi_id`, `err_src_id`, `err_dst_id`. Sticky semantics: first qualifying error wins; subsequent errors do NOT overwrite until software clears the corresponding `ERR_STATUS[i]` via RW1C, at which point the next qualifying event re-arms capture. Rationale: prevents losing the original triggering error during cascaded-error storms. | MUST | (none) |
| NI_CFG_MODE_SWITCH | `set_bfm_mode(mode)` called (per `transaction_api.md`); `bfm_mode` transitions ACTIVE→PASSIVE or PASSIVE→ACTIVE | On ACTIVE→PASSIVE, all BFM-driven outputs (per `active_passive_mode.md` §Capability table) transition to their during-reset values within 1 cycle of the corresponding clock; in-flight Transaction API calls unblock with `MODE_SWITCHED_TO_PASSIVE`. On PASSIVE→ACTIVE, BFM-driven outputs return to reset-deassertion values; configuration knobs become effective on the next transaction. | MUST | (none) |
| NI_CFG_RESPONSE_DELAY_AXI | `set_response_delay_axi(min, max)` called; next AXI response handshake on slave port pending | BFM holds AXI B/R response output by random K ∈ [min, max] `aclk_i` cycles before asserting `bvalid`/`rvalid`. Persists across transactions until reconfigured or `reset_state()`. Test-only knob; RTL counterpart has fixed pipeline timing (`CUT_AX`/`CUT_RSP` synthesis params). | SHOULD | (none) |
| NI_CFG_RESPONSE_DELAY_NOC | `set_response_delay_noc(min, max)` called; next NoC injection pending | BFM holds NoC `noc_*_o.valid` HIGH assertion by random K ∈ [min, max] `noc_clk_i` cycles after the flit is ready-to-inject. Persists across transactions until reconfigured or `reset_state()`. Test-only knob; no RTL counterpart. | SHOULD | (none) |
| NI_CFG_INJECT_ECC_ERROR | `set_inject_ecc_error(channel, kind)` called; `kind ∈ {SINGLE_BIT, DOUBLE_BIT}`; next flit injection on the specified channel | Next flit's ECC field is corrupted: SINGLE_BIT flips one ECC bit (correctable by Hsiao SECDED at receiver); DOUBLE_BIT flips two ECC bits (uncorrectable). One-shot — flag clears after the next flit injection on the specified channel. `kind=NONE` clears any pending injection. Test-only knob. | SHOULD | (none) |
| NI_CFG_RESPONSE_FAULT | `set_response_fault(channel, kind)` called; `channel ∈ {B, R}`; `kind ∈ {SLVERR, DECERR}`; next response handshake on the specified channel | Next B/R response handshake drives the corresponding `bresp`/`rresp` value (`SLVERR=0b10` or `DECERR=0b11`) instead of the would-be `OKAY`. One-shot — flag clears after the response is consumed. `kind=NONE` clears any pending fault. Test-only knob. | SHOULD | (none) |
| NI_CFG_PENDING_COUNT_ACCURACY | Software reads `PENDING_R_COUNT` (0x130) or `PENDING_W_COUNT` (0x134) | Returned value MUST equal the AXI-edge-defined outstanding count for that direction. `PENDING_R_COUNT` increments on AR handshake completion at `axi_*_i`; decrements on R-with-`rlast` handshake completion at `axi_*_i`. `PENDING_W_COUNT` increments on AW handshake completion at `axi_*_i`; decrements on B handshake completion at `axi_*_i`. Both counters are `aclk_i`-native (no CDC sync delay). Counter width per direction = `ceil(log2(MAX_TXNS+1))`; saturation at `MAX_TXNS` is impossible by construction (NMU back-pressures `awready`/`arready` before exceed). On `arst_ni` assertion: counters reset to 0 (tracker dropped per ToO §Reset entry sequencing). The CSR readback path may pipeline by ≥1 aclk cycle; the value returned reflects the cycle the read handshake samples. | MUST | (none) |
| NI_CFG_QUIESCE_FLOW | Software writes `QUIESCE_CTRL.quiesce_req` to 1 | NMU MUST stop accepting new AW/AR handshakes by holding `axi_awready_o = axi_arready_o = 0` while `quiesce_req=1`. In-flight outstanding transactions continue to drain through normal response paths (no forced cancellation). `QUIESCE_STATUS.quiesce_idle` asserts on the cycle `(quiesce_req=1) AND (PENDING_R_COUNT=0) AND (PENDING_W_COUNT=0)` becomes true — combinational over latched aclk-domain values. NSU is **NOT** quiesced: NSU continues to service inbound `noc_req_i` requests and drive `axi_*_o` to the local AXI slave. Software clears `quiesce_req=0` to resume; on the same cycle, `quiesce_idle` deasserts (because the AND-term `quiesce_req=1` becomes false) and NMU resumes accepting AW/AR on the next cycle. Per ToO §"Software quiesce flow". | MUST | (none) |
| NI_CFG_EXCLUSIVE_CLEAR_RACE | Software writes `1` to `EXCLUSIVE_MONITOR_CTRL.clear_all` (0x144) in the same `aclk_i` cycle as one of: (a) NSU performing Exclusive AW match check; (b) NSU allocating new entry on Exclusive AR; (c) NSU invalidating an entry due to overlapping normal write per `AXI4_SLV_EXCLUSIVE_OVERLAP_INVALIDATE` | Race resolution: the `aclk_i` edge that completes the CSR-write handshake defines the "clear epoch boundary". On that edge: (a) AW match check uses **pre-clear** monitor state (the Exclusive AW that arrived in the same cycle proceeds against the entry that was alive at start-of-cycle; if matched, EXOKAY; entry then cleared); (b) Exclusive AR allocation occurs **post-clear** (the new entry is allocated from a cleared table and survives the clear); (c) overlap-invalidate is idempotent — both events independently invalidate the same entry. After the clear epoch, the `clear_all` bit self-clears on the next `aclk_i` edge (latency = 1 cycle); subsequent reads return 0. | MUST | (none) |
| NI_CFG_EXCLUSIVE_OCCUPANCY_ACCURACY | Software reads `EXCLUSIVE_MONITOR_STATUS.occupancy` (0x148) | Returned value MUST equal the live count of NSU Exclusive Monitor entries currently in `ALLOCATED` state. Field width = `ceil(log2(EXCLUSIVE_MONITOR_DEPTH+1))`; max value = `EXCLUSIVE_MONITOR_DEPTH`. `aclk_i`-native; same domain as the monitor itself (NSU). On `arst_ni` assertion: monitor cleared → occupancy=0. Pipelining same as `NI_CFG_PENDING_COUNT_ACCURACY` — value reflects the cycle the read handshake samples. | MUST | (none) |

## Interrupt assertion

This sub-section formalises `irq_o` behaviour. The interrupt is the sole sideband mechanism by which the NI surfaces error events to the system. AXI-rresp-based propagation is reserved for end-to-end SLVERR cases (per ToO §ECC rationale; no fabric-driven SLVERR in v0.4.0).

| ID | Condition | Required behavior | Severity | ARM SVA equivalent |
|----|-----------|-------------------|----------|--------------------|
| NI_IRQ_LEVEL | At any cycle of `aclk_i` while `arst_ni` is deasserted | `irq_o` MUST equal the bitwise-OR of `(ERR_STATUS[i] AND IRQ_ENABLE[i])` over `i ∈ {0..2}`. Level-sensitive: stays HIGH while any unmasked `ERR_STATUS` bit is set; deasserts on the cycle software RW1C clears the last set+enabled bit (per `NI_CFG_ERR_STATUS_RW1C`). NoC-domain error sources (route_par drop, flit_ecc uncorrectable detected at NoC-side sink) reach `ERR_STATUS` via the existing CSR-file CDC sync path; no separate interrupt CDC is introduced. During reset (`arst_ni` LOW), `ERR_STATUS` and `IRQ_ENABLE` reset to 0 by construction, so `irq_o = 0`. | MUST | (none) |
