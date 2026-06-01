# Pin-Level Reset Behavior

The NI has **two reset signals** corresponding to its two clock domains. Per-wire reset behavior is enumerated separately for each reset assertion. The set of wires must match `signal_interface.md` §Wire table exactly (LINT-BFM-001).

**Reset signals:**
- `arst_ni` (AXI side, sync to `aclk_i`, active LOW); covers all `axi_*_i`, `axi_*_o`, and `csr_*` wires
- `noc_rst_ni` (NoC side, sync to `noc_clk_i`, active LOW). Covers all `noc_*` wires plus sideband (`id_i`)
- **Minimum assertion duration**: 16 cycles of the corresponding clock
- **Synchronicity**: async assertion / sync deassertion to the corresponding clock

A wire's "during reset" value is determined by its corresponding reset signal. Wires that straddle the AXI ↔ NoC CDC boundary internally are not exposed externally; external wires belong to exactly one reset domain.

## During reset (per relevant reset asserted)

### AXI Slave port — driven by AXI master DUT or by NMU; AXI domain (arst_ni)

| Channel | Signal | Value during reset | Notes |
|---------|--------|--------------------|-------|
| AW_IN | axi_awvalid_i | as driven by DUT | input from AXI master |
| AW_IN | axi_awready_o | 0 | NMU not ready while reset |
| AW_IN | axi_awid_i | as driven by DUT |  |
| AW_IN | axi_awaddr_i | as driven by DUT |  |
| AW_IN | axi_awlen_i | as driven by DUT |  |
| AW_IN | axi_awsize_i | as driven by DUT |  |
| AW_IN | axi_awburst_i | as driven by DUT |  |
| AW_IN | axi_awlock_i | as driven by DUT | Exclusive access indicator |
| AW_IN | axi_awcache_i | as driven by DUT |  |
| AW_IN | axi_awprot_i | as driven by DUT |  |
| AW_IN | axi_awqos_i | as driven by DUT |  |
| AW_IN | axi_awregion_i | as driven by DUT | Region identifier |
| AW_IN | axi_awuser_i | as driven by DUT |  |
| AW_IN | axi_awatop_i | as driven by DUT | sample-only; out-of-scope per ToO §ATOPs |
| W_IN | axi_wvalid_i | as driven by DUT |  |
| W_IN | axi_wready_o | 0 |  |
| W_IN | axi_wdata_i | as driven by DUT |  |
| W_IN | axi_wstrb_i | as driven by DUT |  |
| W_IN | axi_wlast_i | as driven by DUT |  |
| W_IN | axi_wuser_i | as driven by DUT |  |
| B_IN | axi_bvalid_o | 0 |  |
| B_IN | axi_bready_i | as driven by DUT |  |
| B_IN | axi_bid_o | 0 |  |
| B_IN | axi_bresp_o | 0 (OKAY) |  |
| B_IN | axi_buser_o | 0 |  |
| AR_IN | axi_arvalid_i | as driven by DUT |  |
| AR_IN | axi_arready_o | 0 |  |
| AR_IN | axi_arid_i | as driven by DUT |  |
| AR_IN | axi_araddr_i | as driven by DUT |  |
| AR_IN | axi_arlen_i | as driven by DUT |  |
| AR_IN | axi_arsize_i | as driven by DUT |  |
| AR_IN | axi_arburst_i | as driven by DUT |  |
| AR_IN | axi_arlock_i | as driven by DUT | Exclusive access indicator |
| AR_IN | axi_arcache_i | as driven by DUT |  |
| AR_IN | axi_arprot_i | as driven by DUT |  |
| AR_IN | axi_arqos_i | as driven by DUT |  |
| AR_IN | axi_arregion_i | as driven by DUT | Region identifier |
| AR_IN | axi_aruser_i | as driven by DUT |  |
| R_IN | axi_rvalid_o | 0 |  |
| R_IN | axi_rready_i | as driven by DUT |  |
| R_IN | axi_rid_o | 0 |  |
| R_IN | axi_rdata_o | 0 |  |
| R_IN | axi_rresp_o | 0 (OKAY) |  |
| R_IN | axi_rlast_o | 0 |  |
| R_IN | axi_ruser_o | 0 |  |

### AXI Master port — driven by NSU or by AXI slave DUT; AXI domain (arst_ni)

| Channel | Signal | Value during reset | Notes |
|---------|--------|--------------------|-------|
| AW_OUT | axi_awvalid_o | 0 | NSU not driving |
| AW_OUT | axi_awready_i | as driven by AXI slave | input |
| AW_OUT | axi_awid_o | 0 | registered default |
| AW_OUT | axi_awaddr_o | 0 | registered default |
| AW_OUT | axi_awlen_o | 0 | registered default |
| AW_OUT | axi_awsize_o | 0 | registered default |
| AW_OUT | axi_awburst_o | 0 | registered default |
| AW_OUT | axi_awlock_o | 0 | registered default |
| AW_OUT | axi_awcache_o | 0 | registered default |
| AW_OUT | axi_awprot_o | 0 | registered default |
| AW_OUT | axi_awqos_o | 0 | registered default |
| AW_OUT | axi_awregion_o | 0 | registered default |
| AW_OUT | axi_awuser_o | 0 | registered default |
| W_OUT | axi_wvalid_o | 0 |  |
| W_OUT | axi_wready_i | as driven by slave |  |
| W_OUT | axi_wdata_o | 0 |  |
| W_OUT | axi_wstrb_o | 0 |  |
| W_OUT | axi_wlast_o | 0 |  |
| W_OUT | axi_wuser_o | 0 |  |
| B_OUT | axi_bvalid_i | as driven by slave |  |
| B_OUT | axi_bready_o | 1 | always-ready while in reset to drain |
| B_OUT | axi_bid_i | as driven by slave |  |
| B_OUT | axi_bresp_i | as driven by slave |  |
| B_OUT | axi_buser_i | as driven by slave |  |
| AR_OUT | axi_arvalid_o | 0 |  |
| AR_OUT | axi_arready_i | as driven by slave |  |
| AR_OUT | axi_arid_o | 0 | registered default |
| AR_OUT | axi_araddr_o | 0 | registered default |
| AR_OUT | axi_arlen_o | 0 | registered default |
| AR_OUT | axi_arsize_o | 0 | registered default |
| AR_OUT | axi_arburst_o | 0 | registered default |
| AR_OUT | axi_arlock_o | 0 | registered default |
| AR_OUT | axi_arcache_o | 0 | registered default |
| AR_OUT | axi_arprot_o | 0 | registered default |
| AR_OUT | axi_arqos_o | 0 | registered default |
| AR_OUT | axi_arregion_o | 0 | registered default |
| AR_OUT | axi_aruser_o | 0 | registered default |
| R_OUT | axi_rvalid_i | as driven by slave |  |
| R_OUT | axi_rready_o | 1 | always-ready to drain |
| R_OUT | axi_rid_i | as driven by slave |  |
| R_OUT | axi_rdata_i | as driven by slave |  |
| R_OUT | axi_rresp_i | as driven by slave |  |
| R_OUT | axi_rlast_i | as driven by slave |  |
| R_OUT | axi_ruser_i | as driven by slave |  |

### NoC Request link — NoC domain (noc_rst_ni)

Single shared link per direction. `vc_id` carried in flit header (see `packet_format.md` §1.2). Backpressure governed by credits — see §"NoC credit signals".

| Channel | Signal | Value during reset | Notes |
|---------|--------|--------------------|-------|
| REQ_OUT | noc_req_valid_o | 0 |  |
| REQ_OUT | noc_req_flit_o[FLIT_WIDTH-1:0] | 0 (all bits) | Held to 0 for waveform readability. |
| REQ_IN | noc_req_valid_i | as driven by router |  |
| REQ_IN | noc_req_flit_i[FLIT_WIDTH-1:0] | as driven by router |  |

### NoC Response link — NoC domain (noc_rst_ni)

Single shared link per direction. Same shape as Request link with `noc_rsp_*` prefix.

| Channel | Signal | Value during reset | Notes |
|---------|--------|--------------------|-------|
| RSP_OUT | noc_rsp_valid_o | 0 |  |
| RSP_OUT | noc_rsp_flit_o[FLIT_WIDTH-1:0] | 0 (all bits) |  |
| RSP_IN | noc_rsp_valid_i | as driven by router |  |
| RSP_IN | noc_rsp_flit_i[FLIT_WIDTH-1:0] | as driven by router |  |

### CSR access port — AXI domain (arst_ni)

| Channel | Signal | Value during reset | Notes |
|---------|--------|--------------------|-------|
| CSR_AW | csr_awvalid_i | as driven by master | input from CSR master |
| CSR_AW | csr_awready_o | 0 |  |
| CSR_AW | csr_awaddr_i | as driven |  |
| CSR_AW | csr_awprot_i | as driven |  |
| CSR_W | csr_wvalid_i | as driven |  |
| CSR_W | csr_wready_o | 0 |  |
| CSR_W | csr_wdata_i | as driven |  |
| CSR_W | csr_wstrb_i | as driven |  |
| CSR_B | csr_bvalid_o | 0 |  |
| CSR_B | csr_bready_i | as driven |  |
| CSR_B | csr_bresp_o | 0 (OKAY) |  |
| CSR_AR | csr_arvalid_i | as driven |  |
| CSR_AR | csr_arready_o | 0 |  |
| CSR_AR | csr_araddr_i | as driven |  |
| CSR_AR | csr_arprot_i | as driven |  |
| CSR_R | csr_rvalid_o | 0 |  |
| CSR_R | csr_rready_i | as driven |  |
| CSR_R | csr_rdata_o | 0 |  |
| CSR_R | csr_rresp_o | 0 (OKAY) |  |

### Sideband — NoC domain (noc_rst_ni)

| Signal | Value during reset | Notes |
|--------|--------------------|-------|
| id_i | as driven by integrator | strap-style, expected stable. Per-NI unique node ID (XY coordinate) |

### Interrupt output — AXI domain (arst_ni)

| Signal | Value during reset | Notes |
|--------|--------------------|-------|
| irq_o | 0 | Held LOW while `arst_ni` is asserted. Internal `ERR_STATUS` and `IRQ_ENABLE` registers are reset to 0x0 at the same time, so the function `irq_o = OR(ERR_STATUS[i] & IRQ_ENABLE[i])` evaluates to 0 by construction. |

### NoC credit signals — NoC domain (noc_rst_ni)

Per-VC credit return signals:

| Channel | Signal | Value during reset | Notes |
|---------|--------|--------------------|-------|
| REQ_OUT | noc_req_credit_i[NUM_VC-1:0] | as driven by router | Input. Per-VC credit return from router. |
| REQ_IN | noc_req_credit_o[NUM_VC-1:0] | 0 (all VCs) | NSU not returning credits while reset. |
| RSP_OUT | noc_rsp_credit_i[NUM_VC-1:0] | as driven by router | Input. |
| RSP_IN | noc_rsp_credit_o[NUM_VC-1:0] | 0 (all VCs) | NMU not returning credits while reset. |

### Optional AXI parity signals — AXI domain (arst_ni)

Conditional presence:
- Master-port parity signals (`axi_awaddr_par_i`, `axi_araddr_par_i`, `axi_wdata_par_i`, `axi_rdata_par_o`) present only when `ENABLE_AXI_PARITY = true` AND `EN_MST_PORT = 1`
- Slave-port parity signals (`axi_awaddr_par_o`, `axi_araddr_par_o`, `axi_wdata_par_o`, `axi_rdata_par_i`) present only when `ENABLE_AXI_PARITY = true` AND `EN_SLV_PORT = 1`

All address and data parity signals are per-byte (1 bit per byte).

| Channel | Signal | Value during reset | Notes |
|---------|--------|--------------------|-------|
| AW_IN | axi_awaddr_par_i[ADDR_WIDTH/8-1:0] | as driven by DUT | Input. Per-byte parity. Requires `EN_MST_PORT=1`. |
| AR_IN | axi_araddr_par_i[ADDR_WIDTH/8-1:0] | as driven by DUT | Input. Per-byte parity. Requires `EN_MST_PORT=1`. |
| W_IN | axi_wdata_par_i[NOC_DATA_WIDTH/8-1:0] | as driven by DUT | Input. Per-byte parity. Requires `EN_MST_PORT=1`. |
| R_IN | axi_rdata_par_o[NOC_DATA_WIDTH/8-1:0] | 0 | NMU-driven per-byte parity for R responses to master. Held 0 while reset. Generated post-`flit_ecc`-check at NMU. Requires `EN_MST_PORT=1`. |
| AW_OUT | axi_awaddr_par_o[ADDR_WIDTH/8-1:0] | 0 | NSU-driven per-byte parity. Held 0 while reset. Requires `EN_SLV_PORT=1`. |
| AR_OUT | axi_araddr_par_o[ADDR_WIDTH/8-1:0] | 0 | NSU-driven per-byte parity. Requires `EN_SLV_PORT=1`. |
| W_OUT | axi_wdata_par_o[NOC_DATA_WIDTH/8-1:0] | 0 | NSU-driven per-byte parity. Requires `EN_SLV_PORT=1`. |
| R_OUT | axi_rdata_par_i[NOC_DATA_WIDTH/8-1:0] | as driven by slave | Input from local slave. Requires `EN_SLV_PORT=1`. |

Default `ENABLE_AXI_PARITY = true` — these parity signals are present on the wire list. Integrators MAY set `false` at instantiation to omit the entire parity sideband, in which case all `axi_*_par_*` signals are absent regardless of `EN_MST_PORT` / `EN_SLV_PORT`.

## After reset (first clock edge with respective reset deasserted)

For every BFM-driven wire, the first-cycle-after-reset value is the same as the during-reset value (registered outputs hold). For every DUT-driven input, the value continues as driven externally. After the first cycle, several wires transition over time as the BFM enables stimulus — see §"Post-reset transitions" below.

The per-wire enumeration below mirrors the §"During reset" section row-for-row to satisfy LINT-BFM-001 wire-set parity. Values are identical unless a different first-cycle-after-reset value is documented; transitions over time are documented separately so the per-wire value column carries cycle-1 only.

### AXI Slave port — driven by AXI master DUT or by NMU; AXI domain (arst_ni)

| Channel | Signal | Value first cycle after reset | Notes |
|---------|--------|--------------------|-------|
| AW_IN | axi_awvalid_i | as driven by DUT | input from AXI master |
| AW_IN | axi_awready_o | 0 | NMU not ready while reset |
| AW_IN | axi_awid_i | as driven by DUT |  |
| AW_IN | axi_awaddr_i | as driven by DUT |  |
| AW_IN | axi_awlen_i | as driven by DUT |  |
| AW_IN | axi_awsize_i | as driven by DUT |  |
| AW_IN | axi_awburst_i | as driven by DUT |  |
| AW_IN | axi_awlock_i | as driven by DUT | Exclusive access indicator |
| AW_IN | axi_awcache_i | as driven by DUT |  |
| AW_IN | axi_awprot_i | as driven by DUT |  |
| AW_IN | axi_awqos_i | as driven by DUT |  |
| AW_IN | axi_awregion_i | as driven by DUT | Region identifier |
| AW_IN | axi_awuser_i | as driven by DUT |  |
| AW_IN | axi_awatop_i | as driven by DUT | sample-only; out-of-scope per ToO §ATOPs |
| W_IN | axi_wvalid_i | as driven by DUT |  |
| W_IN | axi_wready_o | 0 |  |
| W_IN | axi_wdata_i | as driven by DUT |  |
| W_IN | axi_wstrb_i | as driven by DUT |  |
| W_IN | axi_wlast_i | as driven by DUT |  |
| W_IN | axi_wuser_i | as driven by DUT |  |
| B_IN | axi_bvalid_o | 0 |  |
| B_IN | axi_bready_i | as driven by DUT |  |
| B_IN | axi_bid_o | 0 |  |
| B_IN | axi_bresp_o | 0 (OKAY) |  |
| B_IN | axi_buser_o | 0 |  |
| AR_IN | axi_arvalid_i | as driven by DUT |  |
| AR_IN | axi_arready_o | 0 |  |
| AR_IN | axi_arid_i | as driven by DUT |  |
| AR_IN | axi_araddr_i | as driven by DUT |  |
| AR_IN | axi_arlen_i | as driven by DUT |  |
| AR_IN | axi_arsize_i | as driven by DUT |  |
| AR_IN | axi_arburst_i | as driven by DUT |  |
| AR_IN | axi_arlock_i | as driven by DUT | Exclusive access indicator |
| AR_IN | axi_arcache_i | as driven by DUT |  |
| AR_IN | axi_arprot_i | as driven by DUT |  |
| AR_IN | axi_arqos_i | as driven by DUT |  |
| AR_IN | axi_arregion_i | as driven by DUT | Region identifier |
| AR_IN | axi_aruser_i | as driven by DUT |  |
| R_IN | axi_rvalid_o | 0 |  |
| R_IN | axi_rready_i | as driven by DUT |  |
| R_IN | axi_rid_o | 0 |  |
| R_IN | axi_rdata_o | 0 |  |
| R_IN | axi_rresp_o | 0 (OKAY) |  |
| R_IN | axi_rlast_o | 0 |  |
| R_IN | axi_ruser_o | 0 |  |

### AXI Master port — driven by NSU or by AXI slave DUT; AXI domain (arst_ni)

| Channel | Signal | Value first cycle after reset | Notes |
|---------|--------|--------------------|-------|
| AW_OUT | axi_awvalid_o | 0 | NSU not driving |
| AW_OUT | axi_awready_i | as driven by AXI slave | input |
| AW_OUT | axi_awid_o | 0 | registered default |
| AW_OUT | axi_awaddr_o | 0 | registered default |
| AW_OUT | axi_awlen_o | 0 | registered default |
| AW_OUT | axi_awsize_o | 0 | registered default |
| AW_OUT | axi_awburst_o | 0 | registered default |
| AW_OUT | axi_awlock_o | 0 | registered default |
| AW_OUT | axi_awcache_o | 0 | registered default |
| AW_OUT | axi_awprot_o | 0 | registered default |
| AW_OUT | axi_awqos_o | 0 | registered default |
| AW_OUT | axi_awregion_o | 0 | registered default |
| AW_OUT | axi_awuser_o | 0 | registered default |
| W_OUT | axi_wvalid_o | 0 |  |
| W_OUT | axi_wready_i | as driven by slave |  |
| W_OUT | axi_wdata_o | 0 |  |
| W_OUT | axi_wstrb_o | 0 |  |
| W_OUT | axi_wlast_o | 0 |  |
| W_OUT | axi_wuser_o | 0 |  |
| B_OUT | axi_bvalid_i | as driven by slave |  |
| B_OUT | axi_bready_o | 1 | always-ready while in reset to drain |
| B_OUT | axi_bid_i | as driven by slave |  |
| B_OUT | axi_bresp_i | as driven by slave |  |
| B_OUT | axi_buser_i | as driven by slave |  |
| AR_OUT | axi_arvalid_o | 0 |  |
| AR_OUT | axi_arready_i | as driven by slave |  |
| AR_OUT | axi_arid_o | 0 | registered default |
| AR_OUT | axi_araddr_o | 0 | registered default |
| AR_OUT | axi_arlen_o | 0 | registered default |
| AR_OUT | axi_arsize_o | 0 | registered default |
| AR_OUT | axi_arburst_o | 0 | registered default |
| AR_OUT | axi_arlock_o | 0 | registered default |
| AR_OUT | axi_arcache_o | 0 | registered default |
| AR_OUT | axi_arprot_o | 0 | registered default |
| AR_OUT | axi_arqos_o | 0 | registered default |
| AR_OUT | axi_arregion_o | 0 | registered default |
| AR_OUT | axi_aruser_o | 0 | registered default |
| R_OUT | axi_rvalid_i | as driven by slave |  |
| R_OUT | axi_rready_o | 1 | always-ready to drain |
| R_OUT | axi_rid_i | as driven by slave |  |
| R_OUT | axi_rdata_i | as driven by slave |  |
| R_OUT | axi_rresp_i | as driven by slave |  |
| R_OUT | axi_rlast_i | as driven by slave |  |
| R_OUT | axi_ruser_i | as driven by slave |  |

### NoC Request link — NoC domain (noc_rst_ni)

Single shared link per direction. `vc_id` carried in flit header (see `packet_format.md` §1.2). Backpressure governed by credits — see §"NoC credit signals".

| Channel | Signal | Value first cycle after reset | Notes |
|---------|--------|--------------------|-------|
| REQ_OUT | noc_req_valid_o | 0 |  |
| REQ_OUT | noc_req_flit_o[FLIT_WIDTH-1:0] | 0 (all bits) | Held to 0 for waveform readability. |
| REQ_IN | noc_req_valid_i | as driven by router |  |
| REQ_IN | noc_req_flit_i[FLIT_WIDTH-1:0] | as driven by router |  |

### NoC Response link — NoC domain (noc_rst_ni)

Single shared link per direction. Same shape as Request link with `noc_rsp_*` prefix.

| Channel | Signal | Value first cycle after reset | Notes |
|---------|--------|--------------------|-------|
| RSP_OUT | noc_rsp_valid_o | 0 |  |
| RSP_OUT | noc_rsp_flit_o[FLIT_WIDTH-1:0] | 0 (all bits) |  |
| RSP_IN | noc_rsp_valid_i | as driven by router |  |
| RSP_IN | noc_rsp_flit_i[FLIT_WIDTH-1:0] | as driven by router |  |

### CSR access port — AXI domain (arst_ni)

| Channel | Signal | Value first cycle after reset | Notes |
|---------|--------|--------------------|-------|
| CSR_AW | csr_awvalid_i | as driven by master | input from CSR master |
| CSR_AW | csr_awready_o | 0 |  |
| CSR_AW | csr_awaddr_i | as driven |  |
| CSR_AW | csr_awprot_i | as driven |  |
| CSR_W | csr_wvalid_i | as driven |  |
| CSR_W | csr_wready_o | 0 |  |
| CSR_W | csr_wdata_i | as driven |  |
| CSR_W | csr_wstrb_i | as driven |  |
| CSR_B | csr_bvalid_o | 0 |  |
| CSR_B | csr_bready_i | as driven |  |
| CSR_B | csr_bresp_o | 0 (OKAY) |  |
| CSR_AR | csr_arvalid_i | as driven |  |
| CSR_AR | csr_arready_o | 0 |  |
| CSR_AR | csr_araddr_i | as driven |  |
| CSR_AR | csr_arprot_i | as driven |  |
| CSR_R | csr_rvalid_o | 0 |  |
| CSR_R | csr_rready_i | as driven |  |
| CSR_R | csr_rdata_o | 0 |  |
| CSR_R | csr_rresp_o | 0 (OKAY) |  |

### Sideband — NoC domain (noc_rst_ni)

| Signal | Value first cycle after reset | Notes |
|--------|--------------------|-------|
| id_i | as driven by integrator | strap-style, expected stable. Per-NI unique node ID (XY coordinate) |

### Interrupt output — AXI domain (arst_ni)

| Signal | Value first cycle after reset | Notes |
|--------|--------------------|-------|
| irq_o | 0 | Held LOW while `arst_ni` is asserted. Internal `ERR_STATUS` and `IRQ_ENABLE` registers are reset to 0x0 at the same time, so the function `irq_o = OR(ERR_STATUS[i] & IRQ_ENABLE[i])` evaluates to 0 by construction. |

### NoC credit signals — NoC domain (noc_rst_ni)

Per-VC credit return signals:

| Channel | Signal | Value first cycle after reset | Notes |
|---------|--------|--------------------|-------|
| REQ_OUT | noc_req_credit_i[NUM_VC-1:0] | as driven by router | Input. Per-VC credit return from router. |
| REQ_IN | noc_req_credit_o[NUM_VC-1:0] | 0 (all VCs) | NSU not returning credits while reset. |
| RSP_OUT | noc_rsp_credit_i[NUM_VC-1:0] | as driven by router | Input. |
| RSP_IN | noc_rsp_credit_o[NUM_VC-1:0] | 0 (all VCs) | NMU not returning credits while reset. |

### Optional AXI parity signals — AXI domain (arst_ni)

Conditional presence:
- Master-port parity signals (`axi_awaddr_par_i`, `axi_araddr_par_i`, `axi_wdata_par_i`, `axi_rdata_par_o`) present only when `ENABLE_AXI_PARITY = true` AND `EN_MST_PORT = 1`
- Slave-port parity signals (`axi_awaddr_par_o`, `axi_araddr_par_o`, `axi_wdata_par_o`, `axi_rdata_par_i`) present only when `ENABLE_AXI_PARITY = true` AND `EN_SLV_PORT = 1`

All address and data parity signals are per-byte (1 bit per byte).

| Channel | Signal | Value first cycle after reset | Notes |
|---------|--------|--------------------|-------|
| AW_IN | axi_awaddr_par_i[ADDR_WIDTH/8-1:0] | as driven by DUT | Input. Per-byte parity. Requires `EN_MST_PORT=1`. |
| AR_IN | axi_araddr_par_i[ADDR_WIDTH/8-1:0] | as driven by DUT | Input. Per-byte parity. Requires `EN_MST_PORT=1`. |
| W_IN | axi_wdata_par_i[NOC_DATA_WIDTH/8-1:0] | as driven by DUT | Input. Per-byte parity. Requires `EN_MST_PORT=1`. |
| R_IN | axi_rdata_par_o[NOC_DATA_WIDTH/8-1:0] | 0 | NMU-driven per-byte parity for R responses to master. Held 0 while reset. Generated post-`flit_ecc`-check at NMU. Requires `EN_MST_PORT=1`. |
| AW_OUT | axi_awaddr_par_o[ADDR_WIDTH/8-1:0] | 0 | NSU-driven per-byte parity. Held 0 while reset. Requires `EN_SLV_PORT=1`. |
| AR_OUT | axi_araddr_par_o[ADDR_WIDTH/8-1:0] | 0 | NSU-driven per-byte parity. Requires `EN_SLV_PORT=1`. |
| W_OUT | axi_wdata_par_o[NOC_DATA_WIDTH/8-1:0] | 0 | NSU-driven per-byte parity. Requires `EN_SLV_PORT=1`. |
| R_OUT | axi_rdata_par_i[NOC_DATA_WIDTH/8-1:0] | as driven by slave | Input from local slave. Requires `EN_SLV_PORT=1`. |

Default `ENABLE_AXI_PARITY = true` — these parity signals are present on the wire list. Integrators MAY set `false` at instantiation to omit the entire parity sideband, in which case all `axi_*_par_*` signals are absent regardless of `EN_MST_PORT` / `EN_SLV_PORT`.

## Post-reset transitions

After the first cycle, several wires transition over time as the BFM enables stimulus:

- `axi_awready_o`, `axi_wready_o`, `axi_arready_o`: 0 → 1 once NMU tracker reset is complete (1 cycle latency in active mode).
- `axi_bready_o`: held at 1 (always-ready to drain — B is metadata-only, no NSU back-pressure path).
- `axi_rready_o`: 1 by default; drops to 0 when NSU R-buffer is full (per `theory_of_operation.md` §NSU Read response buffer). NSU back-pressures the local AXI slave on this signal.
- BFM-driven `*valid_o` outputs (master-port responses `axi_bvalid_o`, `axi_rvalid_o`; slave-port requests `axi_awvalid_o`, `axi_wvalid_o`, `axi_arvalid_o`; NoC `noc_req_valid_o`, `noc_rsp_valid_o`; CSR `csr_bvalid_o`, `csr_rvalid_o`): 0 → asserted only when a transaction is ready to drive.
- `irq_o`: 0 → 1 on the `aclk_i` edge any unmasked `ERR_STATUS` bit asserts; deasserts on the cycle software RW1C clears all set+enabled bits.

For wires driven by external DUTs (rows showing `as driven by DUT`, `as driven by router`, or `as driven by slave`), values during the after-reset window remain externally controlled.

## Reset entry sequencing

1. **Either reset asserts asynchronously**. All BFM outputs in the corresponding domain assert their "during reset" values combinationally — no clock cycle of delay.
2. **While the reset is low**:
   - All in-flight transaction trackers in the affected domain are dropped.
   - RoB entries cleared (B and R RoBs both reset on `arst_ni`; cross-domain in-flight transactions on CDC FIFOs partially clear — write-side ptr resets if writer's domain is in reset, read-side ptr resets if reader's domain is in reset).
   - Pending response_delay countdowns cancelled.
   - One-shot fault flags cleared.
   - Observation lists are NOT cleared (cleared only by `reset_state()` API).
   - Configuration store survives wire-level reset; only `reset_state()` restores defaults.
3. **Reset deasserts on the corresponding clock's rising edge**. BFM outputs transition to their "after reset" values; ready signals become assertable when the BFM enables stimulus acceptance (default: enabled on cycle 1 post-reset deassertion in active mode).
4. **Cross-domain partial reset** (one reset asserted, the other deasserted):
   - The asserted side's wires hold during-reset values; the asserted side does not service traffic.
   - The deasserted side services its own intra-domain traffic but cross-domain transactions stall.
   - CDC FIFOs hold their reset values on the asserted side; the deasserted side sees its FIFO read port as empty (or write port as not-ready).
   - Integrator should ensure both resets reach a consistent state by power-on completion. Typical pattern: assert both resets together at power-on, hold for at least max(16 aclk cycles, 16 noc_clk cycles), then deassert both with proper sync.

## CDC FIFO reset

Internal async FIFOs at the AXI ↔ NoC boundary use the following reset semantics (gray-counter pointer, 2FF synchronizer, `cdc_fifo_gray` semantics):

- Write-side pointer resets on writer's domain reset (gray-counter cleared to 0).
- Read-side pointer resets on reader's domain reset (gray-counter cleared to 0).
- Empty signal asserts on read side when both synced gray pointers compare equal (post-reset baseline = both 0).

**Cross-domain partial reset**: when one side's reset asserts while the other side's clock is running, the side that resets clears its pointer. The other side may see a stale / non-empty status that does not match the data state. This condition is not automatically recoverable at the NI level.

**Integrator responsibility**: cross-domain partial reset MUST be avoided by system-level reset coordination. Either (a) assert both resets simultaneously and hold for at least `max(16 aclk_i cycles, 16 noc_clk_i cycles)` per §Reset entry sequencing item 4, OR (b) provide a system-level coordination signal that prevents one side from resuming traffic while the other is mid-reset.

Earlier revisions promised an automatic `flush_on_full_reset` mechanism in the NI. That claim is retracted — no automatic NI-level recovery is implemented. CDC async FIFO recovery relies on integrator coordination, not NI-level flush logic.

**Designer-confirmed (D1→D2 ambiguity triage, 2026-05-10): no automatic flush; partial-reset recovery is integrator responsibility.**
