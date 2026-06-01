# Signal Interface

**Protocols:**
- AXI side: AXI4 (ARM IHI 0022)
- NoC side: custom flit-based packet protocol. Flit width `FLIT_WIDTH` bits (default 406 in v0.4.0). Header `HEADER_WIDTH` bits (default 54 in v0.4.0). See `./packet_format.md` for flit format details (vendored from `noc-sim/docs/design/packet_format.md`).
- CSR side: AXI4-Lite (subset of AXI4) for software-visible configuration and monitoring registers.

**Role:** Both master and slave. NMU (Network Master Unit) acts as AXI slave on the host side (receives AXI requests from local master) and initiates flits on the NoC. NSU (Network Slave Unit) receives flits from the NoC and acts as AXI master on the host side (drives AXI requests to local slave).

**BFM perspective:** Direction in the tables below is from the BFM out to the connected fabric / IP. The BFM has four external interfaces: AXI4 slave port (NMU side; receives AXI from local master), AXI4 master port (NSU side; drives AXI to local slave), NoC link pair (req + rsp, bidirectional), AXI4-Lite CSR access port (software configuration access).

## Naming convention

Lowercase signals with `_i` / `_o` direction suffixes and `_ni` for active-low resets. AXI4 channel signals use the standard `awvalid` / `awready` / `awaddr` lowercase form. Flit-level signals use `noc_*` prefix. Width parameters in brackets denote signal vector width.

## Per-block interface summary

Bundle-level interface contract per sub-block. Pin-level detail (width, reset value, optional flag) stays in the Wire table below, which is the single source of truth. The `Wire source` column points to the §Channel grouping tokens.

**NMU / NSU naming.** NMU (NoC Master Unit) and NSU (NoC Slave Unit) are the injection-side and ejection-side functions of one bidirectional Network Interface. NMU is injection (local AXI master to flit). NSU is ejection (flit to local AXI slave). This decomposition is a logical view. NoC backpressure is per-VC credit (see §NoC credit signals), not a ready handshake. Credit counters are statically preloaded at reset — no dynamic credit-init handshake is needed for an internal NoC.

**Host AXI port roles.** NMU is the AXI slave on its host port, with a local AXI master as peer. NSU is the AXI master on its host port, with a local AXI slave as peer.

### NMU (injection path)

| Interface | Direction | Peer | Protocol | Flow control | Clock domain | Wire source |
|---|---|---|---|---|---|---|
| AXI slave port (host) | in request, out response | local AXI master | AXI4 | ready/valid per channel | `aclk_i` / `arst_ni` | `AW_IN` `W_IN` `AR_IN` `B_IN` `R_IN` |
| NoC request out | out | Router local input | NoC request flit link | per-VC credit return | `noc_clk_i` / `noc_rst_ni` | `REQ_OUT` + req credit |
| NoC response in | in | Router local output | NoC response flit link | per-VC credit | `noc_clk_i` / `noc_rst_ni` | `RSP_IN` + credit |
| CSR | in request, out response | software CSR master | AXI4-Lite | ready/valid | `aclk_i` / `arst_ni` | `CSR_*` |

### NSU (ejection path)

| Interface | Direction | Peer | Protocol | Flow control | Clock domain | Wire source |
|---|---|---|---|---|---|---|
| NoC request in | in | Router local output | NoC request flit link | per-VC credit | `noc_clk_i` / `noc_rst_ni` | `REQ_IN` + credit |
| AXI master port (host) | out request, in response | local AXI slave | AXI4 | ready/valid per channel | `aclk_i` / `arst_ni` | `AW_OUT` `W_OUT` `AR_OUT` `B_OUT` `R_OUT` |
| NoC response out | out | Router local input | NoC response flit link | per-VC credit return | `noc_clk_i` / `noc_rst_ni` | `RSP_OUT` + credit |

### Router peer reference

The Router (also called the NoC Packet Switch, NPS) is a separate block. Its authoritative interface lives in the noc-sim router spec. It is listed here only as the NI's peer so the NoC-side connection is unambiguous.

| Interface | Direction | Peer | Flow control |
|---|---|---|---|
| local request in, response out | in / out | NMU NoC ports | per-VC credit |
| local request out, response in | out / in | NSU NoC ports | per-VC credit |
| fabric ports (per `route_direction_e`: N/E/S/W/Eject) | bidirectional per link | neighbor routers | per-VC credit + wormhole packet lock |

## Wire table

The Wire table excludes the protocol clocks and resets (listed in §Protocol clock and reset). LINT-BFM-001 (wire-set parity vs `pin_level_reset.md`) applies to this table only.

### AXI4 Slave port (NMU side; receives AXI from local AXI master)

#### AW channel (write address)

| Signal | Direction | Width | Active | Sample edge | Reset value (see pin_level_reset.md) | Optional in protocol | BFM supports | Notes |
|--------|-----------|-------|--------|-------------|--------------------------------------|----------------------|--------------|-------|
| `axi_awvalid_i` | input | 1 | H | pos aclk | §AXI in AW row 1 | no | yes | Master signals AW phase request |
| `axi_awready_o` | output | 1 | H | pos aclk | §AXI in AW row 2 | no | yes | NMU signals AW phase accept |
| `axi_awid_i` | input | IN_ID_WIDTH | — | pos aclk | §AXI in AW row 3 | no | yes | Transaction ID |
| `axi_awaddr_i` | input | ADDR_WIDTH | — | pos aclk | §AXI in AW row 4 | no | yes | Target address |
| `axi_awlen_i` | input | 8 | — | pos aclk | §AXI in AW row 5 | no | yes | Burst length minus 1 (0..255) |
| `axi_awsize_i` | input | 3 | — | pos aclk | §AXI in AW row 6 | no | yes | Beat size (log2 bytes per beat) |
| `axi_awburst_i` | input | 2 | — | pos aclk | §AXI in AW row 7 | no | yes | Burst type: 00=FIXED, 01=INCR, 10=WRAP, 11=reserved |
| `axi_awlock_i` | input | 1 | — | pos aclk | §AXI in AW row 8 | yes | yes | AXI4 Exclusive access indicator (`AxLOCK=Exclusive` in AXI4 spec). Routed to NSU Exclusive Monitor (see ToO §NSU Exclusive Monitor). |
| `axi_awcache_i` | input | 4 | — | pos aclk | §AXI in AW row 9 | yes | yes | Cache attributes; recorded but not enforced by NMU |
| `axi_awprot_i` | input | 3 | — | pos aclk | §AXI in AW row 10 | no | yes | Protection attributes; recorded but not enforced |
| `axi_awqos_i` | input | 4 | — | pos aclk | §AXI in AW row 11 | yes | yes | AXI4 QoS hint; packed into AW payload and forwarded to destination slave as-is |
| `axi_awregion_i` | input | 4 | — | pos aclk | §AXI in AW row 12 | yes | yes | AXI4 region identifier; sampled and forwarded to flit header. Most masters tie 0. |
| `axi_awuser_i` | input | USER_WIDTH | — | pos aclk | §AXI in AW row 13 | yes | yes | User signal; passed through to flit user payload |
| `axi_awatop_i` | input | 6 | — | pos aclk | §AXI in AW row 14 | yes | yes (sample-only) | AXI5 atomic operation code; sampled and recorded by monitor only — BFM terminates ATOPs with `bresp=SLVERR` per ToO §ATOPs scope (out-of-scope for stimulus generation in this revision) |

#### W channel (write data)

| Signal | Direction | Width | Active | Sample edge | Reset value | Optional | BFM supports | Notes |
|--------|-----------|-------|--------|-------------|-------------|----------|--------------|-------|
| `axi_wvalid_i` | input | 1 | H | pos aclk | §AXI in W row 1 | no | yes |  |
| `axi_wready_o` | output | 1 | H | pos aclk | §AXI in W row 2 | no | yes |  |
| `axi_wdata_i` | input | NOC_DATA_WIDTH | — | pos aclk | §AXI in W row 3 | no | yes |  |
| `axi_wstrb_i` | input | NOC_DATA_WIDTH/8 | — | pos aclk | §AXI in W row 4 | no | yes | Byte strobes |
| `axi_wlast_i` | input | 1 | H | pos aclk | §AXI in W row 5 | no | yes | Asserted on the last beat of the burst |
| `axi_wuser_i` | input | USER_WIDTH | — | pos aclk | §AXI in W row 6 | yes | yes |  |

#### B channel (write response)

| Signal | Direction | Width | Active | Sample edge | Reset value | Optional | BFM supports | Notes |
|--------|-----------|-------|--------|-------------|-------------|----------|--------------|-------|
| `axi_bvalid_o` | output | 1 | H | pos aclk | §AXI in B row 1 | no | yes |  |
| `axi_bready_i` | input | 1 | H | pos aclk | §AXI in B row 2 | no | yes |  |
| `axi_bid_o` | output | IN_ID_WIDTH | — | pos aclk | §AXI in B row 3 | no | yes | Matches awid of completed transaction |
| `axi_bresp_o` | output | 2 | — | pos aclk | §AXI in B row 4 | no | yes | 00=OKAY, 01=EXOKAY (unused), 10=SLVERR, 11=DECERR |
| `axi_buser_o` | output | USER_WIDTH | — | pos aclk | §AXI in B row 5 | yes | yes |  |

#### AR channel (read address)

| Signal | Direction | Width | Active | Sample edge | Reset value | Optional | BFM supports | Notes |
|--------|-----------|-------|--------|-------------|-------------|----------|--------------|-------|
| `axi_arvalid_i` | input | 1 | H | pos aclk | §AXI in AR row 1 | no | yes |  |
| `axi_arready_o` | output | 1 | H | pos aclk | §AXI in AR row 2 | no | yes |  |
| `axi_arid_i` | input | IN_ID_WIDTH | — | pos aclk | §AXI in AR row 3 | no | yes |  |
| `axi_araddr_i` | input | ADDR_WIDTH | — | pos aclk | §AXI in AR row 4 | no | yes |  |
| `axi_arlen_i` | input | 8 | — | pos aclk | §AXI in AR row 5 | no | yes |  |
| `axi_arsize_i` | input | 3 | — | pos aclk | §AXI in AR row 6 | no | yes |  |
| `axi_arburst_i` | input | 2 | — | pos aclk | §AXI in AR row 7 | no | yes |  |
| `axi_arlock_i` | input | 1 | — | pos aclk | §AXI in AR row 8 | yes | yes | AXI4 Exclusive access indicator. Routed to NSU Exclusive Monitor. |
| `axi_arcache_i` | input | 4 | — | pos aclk | §AXI in AR row 9 | yes | yes |  |
| `axi_arprot_i` | input | 3 | — | pos aclk | §AXI in AR row 10 | no | yes |  |
| `axi_arqos_i` | input | 4 | — | pos aclk | §AXI in AR row 11 | yes | yes |  |
| `axi_arregion_i` | input | 4 | — | pos aclk | §AXI in AR row 12 | yes | yes | AXI4 region identifier; sampled and forwarded. |
| `axi_aruser_i` | input | USER_WIDTH | — | pos aclk | §AXI in AR row 13 | yes | yes |  |

#### R channel (read data)

| Signal | Direction | Width | Active | Sample edge | Reset value | Optional | BFM supports | Notes |
|--------|-----------|-------|--------|-------------|-------------|----------|--------------|-------|
| `axi_rvalid_o` | output | 1 | H | pos aclk | §AXI in R row 1 | no | yes |  |
| `axi_rready_i` | input | 1 | H | pos aclk | §AXI in R row 2 | no | yes |  |
| `axi_rid_o` | output | IN_ID_WIDTH | — | pos aclk | §AXI in R row 3 | no | yes |  |
| `axi_rdata_o` | output | NOC_DATA_WIDTH | — | pos aclk | §AXI in R row 4 | no | yes |  |
| `axi_rresp_o` | output | 2 | — | pos aclk | §AXI in R row 5 | no | yes |  |
| `axi_rlast_o` | output | 1 | H | pos aclk | §AXI in R row 6 | no | yes |  |
| `axi_ruser_o` | output | USER_WIDTH | — | pos aclk | §AXI in R row 7 | yes | yes |  |

### AXI4 Master port (NSU side; drives AXI to local AXI slave)

The master port mirrors the slave port but with all directions reversed. Field semantics are identical to AXI4 spec; only the BFM-perspective direction differs.

#### AW channel

| Signal | Direction | Width | Reset | Notes |
|--------|-----------|-------|-------|-------|
| `axi_awvalid_o` | output | 1 | §AXI out AW row 1 | NSU drives AW phase to local slave |
| `axi_awready_i` | input | 1 | §AXI out AW row 2 | Local slave signals AW accept |
| `axi_awid_o` | output | OUT_ID_WIDTH | §AXI out AW row 3 |  |
| `axi_awaddr_o` | output | ADDR_WIDTH | §AXI out AW row 4 |  |
| `axi_awlen_o` | output | 8 | §AXI out AW row 5 |  |
| `axi_awsize_o` | output | 3 | §AXI out AW row 6 |  |
| `axi_awburst_o` | output | 2 | §AXI out AW row 7 |  |
| `axi_awlock_o` | output | 1 | §AXI out AW row 8 | Forwarded from request flit AW payload `awlock` field |
| `axi_awcache_o` | output | 4 | §AXI out AW row 9 |  |
| `axi_awprot_o` | output | 3 | §AXI out AW row 10 |  |
| `axi_awqos_o` | output | 4 | §AXI out AW row 11 | Forwarded from inbound flit AW payload `awqos` field |
| `axi_awregion_o` | output | 4 | §AXI out AW row 12 | Forwarded from request flit AW payload `awregion` field |
| `axi_awuser_o` | output | USER_WIDTH | §AXI out AW row 13 |  |

#### W / B / AR / R channels

Same shape as the slave port; all `_o` ↔ `_i` direction flipped (W: outputs from BFM driving wdata/wstrb/wlast; B: BFM samples bvalid/bid/bresp; AR: BFM drives ARVALID with full address; R: BFM samples rvalid + payload).

For brevity the wire-level expansion follows the slave port exactly (same fields, same widths). See pin_level_reset.md §AXI out for per-wire reset values.

### NoC Request link

Single shared data link per direction. One flit slot per cycle. The `vc_id` field of the flit header (see `packet_format.md` §1.2 Group 2) identifies the owning VC. Per-cycle backpressure is governed by credits, not by a `ready` handshake — see §"NoC credit signals". NMU at injection side maps each flit onto a VC per `theory_of_operation.md` §"VC Mapping" (VC selection policy: currently coarse req-vs-rsp split; round-robin arbitration across populated VCs at egress).

| Signal | Direction | Width | Active | Sample edge | Reset value | Optional | BFM supports | Notes |
|--------|-----------|-------|--------|-------------|-------------|----------|--------------|-------|
| `noc_req_valid_o` | output | 1 | H | pos noc_clk | §NoC req out row 1 | no | yes | NMU asserts when driving a valid flit this cycle. |
| `noc_req_flit_o[FLIT_WIDTH-1:0]` | output | FLIT_WIDTH | — | pos noc_clk | §NoC req out row 2 | no | yes | Outbound flit. Valid when `noc_req_valid_o=1`. |
| `noc_req_valid_i` | input | 1 | H | pos noc_clk | §NoC req in row 1 | no | yes | Router asserts when an inbound flit is on the link. |
| `noc_req_flit_i[FLIT_WIDTH-1:0]` | input | FLIT_WIDTH | — | pos noc_clk | §NoC req in row 2 | no | yes | Inbound flit. Valid when `noc_req_valid_i=1`. |

### NoC Response link

Same shape as Request link, with `noc_rsp_*` prefix.

- NSU injects via `noc_rsp_valid_o` / `noc_rsp_flit_o`; credit return on `noc_rsp_credit_i[NUM_VC-1:0]`
- NMU receives via `noc_rsp_valid_i` / `noc_rsp_flit_i`; returns credits via `noc_rsp_credit_o[NUM_VC-1:0]`

| Signal | Direction | Width | Reset | Notes |
|--------|-----------|-------|-------|-------|
| `noc_rsp_valid_o` | output | 1 | §NoC rsp out row 1 | NSU asserts when driving a valid flit this cycle. |
| `noc_rsp_flit_o[FLIT_WIDTH-1:0]` | output | FLIT_WIDTH | §NoC rsp out row 2 | Outbound response flit. |
| `noc_rsp_valid_i` | input | 1 | §NoC rsp in row 1 | Router asserts when an inbound response flit is on the link. |
| `noc_rsp_flit_i[FLIT_WIDTH-1:0]` | input | FLIT_WIDTH | §NoC rsp in row 2 | Inbound response flit. |

### NoC credit signals (always present)

The NoC link uses per-VC credit accounting (credit-based flow control). Credit return is per-VC: one bit per VC, asserted for one cycle to return one credit on that VC. The destination unit can return up to one credit per cycle per virtual channel. When `NUM_VC = 1` (default), the per-VC array `credit_*[NUM_VC-1:0]` degenerates to a single 1-bit signal. The startup handshake uses a single bi-directional ready signal per direction, indicating credit exchange is ready.

**Per-VC credit return signals:**

| Signal | Direction | Width | Active | Sample edge | Reset value | Optional | BFM supports | Notes |
|--------|-----------|-------|--------|-------------|-------------|----------|--------------|-------|
| `noc_req_credit_i[NUM_VC-1:0]` | input | NUM_VC | H | pos noc_clk | §NoC credit row 1 | no | yes | Per-VC credit return from downstream router. One bit asserts → upstream NMU credit counter for that VC increments by 1. |
| `noc_req_credit_o[NUM_VC-1:0]` | output | NUM_VC | H | pos noc_clk | §NoC credit row 2 | no | yes | Per-VC credit return generated by NSU input buffer pop. One bit assert → downstream router increments its credit. |
| `noc_rsp_credit_i[NUM_VC-1:0]` | input | NUM_VC | H | pos noc_clk | §NoC credit row 3 | no | yes | Per-VC credit return for response link. |
| `noc_rsp_credit_o[NUM_VC-1:0]` | output | NUM_VC | H | pos noc_clk | §NoC credit row 4 | no | yes | Per-VC credit return for NMU response input buffer. |

**Behaviour summary:**

- A flit is driven on `noc_*_flit_o` only when the source has at least one credit on the chosen VC. NMU/NSU decrement the per-VC credit counter on the cycle `noc_*_valid_o=1`.
- Source-credit counters are statically preloaded to `INPUT_BUFFER_DEPTH / NUM_VC` per VC at reset deassertion (where `INPUT_BUFFER_DEPTH` is the receiver's per-link buffer depth, a router-side parameter the integrator must communicate). No dynamic credit-init handshake is required for an internal NoC — static preload at reset is sufficient.
- Credit return latency is `CREDIT_DELAY` cycles (router-side parameter, default 1).
- Credit starvation: if the receiver returns no credits, the source is permanently stalled on that VC until credits resume. There is no automatic timeout / SLVERR escalation in v0.4.0 — software detects via PENDING counters / IRQ and handles recovery externally.

### CSR access port (AXI4-Lite slave)

A dedicated AXI4-Lite slave port for software access to NMU/NSU CSR file. Width assumptions: 32-bit CSR_ADDR_WIDTH=12 (4KB region accommodates ~32 registers per `registers.md`); 32-bit CSR_DATA_WIDTH=32.

| Signal | Direction | Width | Active | Reset value | Notes |
|--------|-----------|-------|--------|-------------|-------|
| `csr_awvalid_i` | input | 1 | H | §CSR row 1 |  |
| `csr_awready_o` | output | 1 | H | §CSR row 2 |  |
| `csr_awaddr_i` | input | 12 | — | §CSR row 3 | 4KB CSR window |
| `csr_awprot_i` | input | 3 | — | §CSR row 4 |  |
| `csr_wvalid_i` | input | 1 | H | §CSR row 5 |  |
| `csr_wready_o` | output | 1 | H | §CSR row 6 |  |
| `csr_wdata_i` | input | 32 | — | §CSR row 7 |  |
| `csr_wstrb_i` | input | 4 | — | §CSR row 8 |  |
| `csr_bvalid_o` | output | 1 | H | §CSR row 9 |  |
| `csr_bready_i` | input | 1 | H | §CSR row 10 |  |
| `csr_bresp_o` | output | 2 | — | §CSR row 11 |  |
| `csr_arvalid_i` | input | 1 | H | §CSR row 12 |  |
| `csr_arready_o` | output | 1 | H | §CSR row 13 |  |
| `csr_araddr_i` | input | 12 | — | §CSR row 14 |  |
| `csr_arprot_i` | input | 3 | — | §CSR row 15 |  |
| `csr_rvalid_o` | output | 1 | H | §CSR row 16 |  |
| `csr_rready_i` | input | 1 | H | §CSR row 17 |  |
| `csr_rdata_o` | output | 32 | — | §CSR row 18 |  |
| `csr_rresp_o` | output | 2 | — | §CSR row 19 |  |

### Sideband / configuration

| Signal | Direction | Width | Reset value | Notes |
|--------|-----------|-------|-------------|-------|
| `id_i` | input | X_WIDTH+Y_WIDTH (default 8) | §Sideband row 1 | This NI's Node ID (XY coordinate of the tile). Per-NI unique. Strap-style. Sampled in noc_clk domain. Modifying after `noc_rst_ni` deassertion is undefined. |

SAM rule table (`Sam`) is **not** a wire-level signal. It is a compile-time parameter (see §Parameters). All NIs in the system share the same `Sam` content. **Compile-time only**: runtime modification is out of scope for v0.4.0 (no `SAM_RULE_*` CSR exists). To change the SAM table, re-elaborate the design with the new `Sam` parameter value.

Single-NI-per-tile model: each tile (`(x, y)` coordinate) has exactly one NI. Tiles with multiple IPs (CPU + DMA + memory controller + accelerator) mux them through an upstream AXI crossbar before reaching the NI; per-IP identification is by AXI ID, not by any flit-header field.

### Interrupt output

Single level-sensitive interrupt output, asserted when any unmasked `ERR_STATUS` bit is set (per `registers.md` §`IRQ_ENABLE` and `protocol_rules.md` `NI_IRQ_LEVEL`). Software ISR reads `ERR_STATUS` to identify which event class fired and `LAST_ERR_INFO` for offending-transaction context.

| Signal | Direction | Width | Active | Sample edge | Reset value | Notes |
|--------|-----------|-------|--------|-------------|-------------|-------|
| `irq_o` | output | 1 | H | pos aclk | §IRQ row 1 | `aclk_i` domain. Combinational over latched `ERR_STATUS` AND `IRQ_ENABLE`. Deasserts when software RW1C clears all set+enabled `ERR_STATUS` bits. NoC-domain error sources (route_par drop, flit_ecc uncorrectable) reach `ERR_STATUS` via the existing CSR-file CDC sync path; no separate interrupt CDC. |

### Optional AXI parity sideband — present only when `ENABLE_AXI_PARITY = true`

AXI-side data and address parity is an integrator-tunable integrity layer at the host/slave AXI boundaries. Independent of the always-on whole-flit `flit_ecc` and `route_par` inside the NoC fabric. Default `ENABLE_AXI_PARITY = true` — these signals are present (AXI parity is always enabled by default). Integrators MAY set `false` at instantiation to omit the sideband if AXI-side parity is not required by the deployment; in that case all `axi_*_par_*` signals are absent from the wire list.

Coverage:
- 1-bit even parity per byte of data (data parity)
- 1-bit even parity per byte of address (address parity, ADDR_WIDTH/8 bits per AxAddress)

「1 bit per byte for Data」與「1 bit per byte for AxAddress」standard config。

**AXI Slave port (axi_*_i) parity inputs (when `EN_MST_PORT=1` and `ENABLE_AXI_PARITY=1`):**

| Signal | Direction | Width | Active | Sample edge | Reset value | Notes |
|--------|-----------|-------|--------|-------------|-------------|-------|
| `axi_awaddr_par_i[ADDR_WIDTH/8-1:0]` | input | ADDR_WIDTH/8 | H | pos aclk | §AXI parity row 1 | Per-byte even parity over `axi_awaddr_i`. Sampled when `axi_awvalid_i=1`. |
| `axi_araddr_par_i[ADDR_WIDTH/8-1:0]` | input | ADDR_WIDTH/8 | H | pos aclk | §AXI parity row 2 | Per-byte even parity over `axi_araddr_i`. Sampled when `axi_arvalid_i=1`. |
| `axi_wdata_par_i[NOC_DATA_WIDTH/8-1:0]` | input | NOC_DATA_WIDTH/8 | H | pos aclk | §AXI parity row 3 | Per-byte even parity over `axi_wdata_i`. Sampled when `axi_wvalid_i=1`. |

**AXI Slave port (axi_*_o) parity outputs (when `EN_MST_PORT=1` and `ENABLE_AXI_PARITY=1`):**

| Signal | Direction | Width | Active | Sample edge | Reset value | Notes |
|--------|-----------|-------|--------|-------------|-------------|-------|
| `axi_rdata_par_o[NOC_DATA_WIDTH/8-1:0]` | output | NOC_DATA_WIDTH/8 | H | pos aclk | §AXI parity row 8 | Per-byte even parity over `axi_rdata_o` (NMU-generated). NMU regenerates this **after** the `flit_ecc` check stage when converting the NoC packet back to AXI protocol — "Data parity for read responses is generated as 1 bit per byte after the ECC check stage, when the data is converted from NPP to AXI protocol." |

**AXI Master port (axi_*_o) parity outputs (when `EN_SLV_PORT=1` and `ENABLE_AXI_PARITY=1`):**

| Signal | Direction | Width | Active | Sample edge | Reset value | Notes |
|--------|-----------|-------|--------|-------------|-------------|-------|
| `axi_awaddr_par_o[ADDR_WIDTH/8-1:0]` | output | ADDR_WIDTH/8 | H | pos aclk | §AXI parity row 4 | Per-byte even parity over `axi_awaddr_o` (NSU-generated). |
| `axi_araddr_par_o[ADDR_WIDTH/8-1:0]` | output | ADDR_WIDTH/8 | H | pos aclk | §AXI parity row 5 | Per-byte even parity over `axi_araddr_o` (NSU-generated). |
| `axi_wdata_par_o[NOC_DATA_WIDTH/8-1:0]` | output | NOC_DATA_WIDTH/8 | H | pos aclk | §AXI parity row 6 | Per-byte even parity over `axi_wdata_o` (NSU-generated). |
| `axi_rdata_par_i[NOC_DATA_WIDTH/8-1:0]` | input | NOC_DATA_WIDTH/8 | H | pos aclk | §AXI parity row 7 | Per-byte even parity over `axi_rdata_i` (from local slave). NSU verifies on R reception. |

**Behaviour:**

- NMU verifies `axi_*_par_i` on each AW/AR/W handshake at the slave port. Mismatch logged to `ERR_STATUS[2] axi_parity_err` + `AXI_PARITY_ERR_CNT++` + `LAST_ERR_INFO` capture (per `protocol_rules.md` `AXI4_MST_PARITY_CHECK`). Transaction proceeds — no SLVERR injection at AXI boundary. Software observes via CSR / IRQ.
- NMU **generates** `axi_rdata_par_o` per byte for R responses returning to master. Generation point: after the `flit_ecc` check stage at NMU. Formalised in `protocol_rules.md` `AXI4_MST_PARITY_GEN_R`.
- NSU generates `axi_*_par_o` per byte for AW/AR/W signals driven to local slave. Address parity regenerated when NMU/NSU modify the address (e.g., AddrTrans address-map lookup may change upper address bits — parity over those bytes is recomputed, lower bytes carried through).
- NSU verifies `axi_rdata_par_i` from local slave. Mismatch logged to `ERR_STATUS[2] axi_parity_err` + `AXI_PARITY_ERR_CNT++` + `LAST_ERR_INFO` capture (per `protocol_rules.md` `AXI4_SLV_PARITY_CHECK`). R beat forwarded to AXI master with `rresp=OKAY`. Same observability path.
- Parity is verified at AXI boundary only. Inside the NoC fabric, `flit_ecc` (whole-flit SECDED) takes over end-to-end protection.
- Rationale for "log-only, no SLVERR": parity at AXI boundary detects local-wire / local-slave corruption, not fabric corruption. Per the (B)-philosophy ECC scheme (see ToO §ECC), the NI does not synthesise AXI rresp values from its own integrity checks — error visibility goes through CSR + IRQ, leaving the AXI rresp channel reserved for end-to-end (HBM/DDR-style) cases (no fabric-driven SLVERR synthesis in v0.4.0).

## Protocol clock and reset

| Signal | Direction | Description |
|--------|-----------|-------------|
| `aclk_i` | input | AXI side clock; samples all `axi_*_i`, `axi_*_o`, `csr_*` on rising edge |
| `arst_ni` | input | AXI side active-low reset; async assertion / sync deassertion to `aclk_i`; hold ≥ 16 `aclk_i` cycles |
| `noc_clk_i` | input | NoC fabric clock. Samples all `noc_*` and `id_i` on rising edge |
| `noc_rst_ni` | input | NoC side active-low reset; async assertion / sync deassertion to `noc_clk_i`; hold ≥ 16 `noc_clk_i` cycles |

NI internal async FIFOs (gray-counter pointer + 2FF synchronizer) bridge AXI ↔ NoC at the boundary inside both NMU and NSU. Cross-domain signals never propagate combinationally.

## Parameters

| Name | Type | Default | Constraint | Description |
|------|------|---------|------------|-------------|
| `ADDR_WIDTH` | int | 64 | 32 ≤ x ≤ 64 | AXI address width on host side |
| `AXI_DATA_WIDTH` | int | 256 | 32 / 64 / 128 / 256 / 512 | **External Width Bridge parameter — not used by the NI core.** The local AXI master/slave-side data width handled by the bolt-on Width Bridge (see ToO §Data Width Conversion). The NI core's own AXI port operates at `NOC_DATA_WIDTH`. Supported interface widths: 32 to 512; 1024 not supported. |
| `NOC_DATA_WIDTH` | int | 256 | 64 / 128 / 256 / 512 | NoC flit data-lane width (the `wdata` / `rdata` field inside a flit, see `packet_format.md` §3.2, §3.4) **and the NI core's AXI port width** — the NI port is fixed at this width. The external Width Bridge adapts the local master/slave `AXI_DATA_WIDTH` to it. |
| `USER_WIDTH` | int | 8 | 1 ≤ x ≤ 32 | AXI user signal width |
| `IN_ID_WIDTH` | int | 8 | 1 ≤ x ≤ 16 | AXI master (incoming) txnID width |
| `OUT_ID_WIDTH` | int | 8 | 1 ≤ x ≤ 16 | AXI slave (outgoing) txnID width |
| `EN_SLV_PORT` | bool | true | EN_SLV_PORT \|\| EN_MST_PORT | Enable NSU |
| `EN_MST_PORT` | bool | true | EN_SLV_PORT \|\| EN_MST_PORT | Enable NMU |
| `MAX_TXNS` | int | 32 | power-of-2 | HW ceiling on outstanding transactions |
| `MAX_UNIQUE_IDS` | int | 1 | 1 ≤ x ≤ MAX_TXNS | Number of unique downstream txnIDs |
| `MAX_TXNS_PER_ID` | int | 32 | 1 ≤ x ≤ MAX_TXNS | Outstanding count per unique ID |
| `B_ROB_TYPE` | enum {NormalRoB, SimpleRoB, NoRoB} | NoRoB | — | B response RoB mode. Default is smallest-area (NoRoB); typical multi-destination deployments use `SimpleRoB` for B — see `theory_of_operation.md` §RoB. |
| `B_ROB_SIZE` | int | 0 | 0 if NoRoB, else 1 ≤ x ≤ MAX_TXNS | B RoB depth |
| `R_ROB_TYPE` | enum | NoRoB | — | R response RoB mode. Default is smallest-area (NoRoB); typical multi-destination deployments use `NormalRoB` for R — see `theory_of_operation.md` §RoB. |
| `R_ROB_SIZE` | int | 0 | same as B_ROB_SIZE | R RoB depth |
| `CUT_AX` | bool | false | — | AW/AR spill register (pipeline cut at AW/AR channel) |
| `CUT_RSP` | bool | false | — | Response spill register (pipeline cut at response channel) |
| `ROUTE_ALGO` | enum {XYRouting, SourceRouting, IDRouting} | XYRouting | — | Routing algorithm |
| `USE_ID_TABLE` | bool | false | — | Use SAM table for dst_id derivation |
| `XY_ADDR_OFFSET_X` | int | 32 | 0 ≤ x ≤ ADDR_WIDTH-X_WIDTH | X coordinate bit offset in AXI address |
| `XY_ADDR_OFFSET_Y` | int | 36 | 0 ≤ x ≤ ADDR_WIDTH-Y_WIDTH | Y coordinate bit offset |
| `NUM_SAM_RULES` | int | 0 | 0 ≤ x ≤ 64 | SAM rules count when USE_ID_TABLE=1 |
| `FLIT_WIDTH` | derived | 402 | derived = HEADER_WIDTH + PAYLOAD_WIDTH | Flit total width (header + payload). post-QoS-removal default 402 (was 406 in v0.4.0 original; `NOC_QOS_WIDTH` 4→0 reduced header by 4). v0.3.0 was 400. |
| `HEADER_WIDTH` | derived | 50 | derived = Σ(all header field params) | Flit header width. post-QoS-removal default 50 (was 54 in v0.4.0 original; `NOC_QOS_WIDTH` changed to 0). v0.3.0 was 48. |
| `PAYLOAD_WIDTH` | derived | 352 | derived = max(per-channel payload widths) | Per-channel payload max (W/R = 352, AW/AR = 108, B = 64). v0.3.0 `wecc/recc` removed. Equivalent bits become `*_rsvd` future-extension. |
| `FLIT_ECC_WIDTH` | int | 10 | satisfy `2^(x-1) ≥ FLIT_DATA_WIDTH + x + 1` SECDED bit-count bound (conservative form, +1 margin over canonical `≥ k+p`; bound is matrix-variant-agnostic — applies to Hsiao SECDED used here per ToO §ECC) | Whole-flit SECDED syndrome width. Default 10 covers FLIT_DATA_WIDTH up to 501 (`2^9=512 ≥ 501+10+1=512`). Integrator must bump to 11 when FLIT_DATA_WIDTH ≥ 502. |
| `ROUTE_PAR_WIDTH` | int | 1 | fixed = 1 | Routing parity width. Always 1-bit even parity over `{dst_id, last}` (9 bits coverage at default; NPP packet DST ID + LAST coverage). |
| `X_WIDTH` | int | 4 | 2 ≤ x ≤ 8 | Mesh X coordinate width |
| `Y_WIDTH` | int | 4 | 2 ≤ x ≤ 8 | Mesh Y coordinate width |
| `URGENCY_WIDTH` | int | 3 | 2 ≤ x ≤ 4 | Regulator urgency level width (retained for potential future use) |
| `ERR_COUNTER_WIDTH` | int | 16 | 8 ≤ x ≤ 32 | Error counter width |
| `Sam` | `sam_rule_t [NUM_SAM_RULES-1:0]` | `'0` | array length = `NUM_SAM_RULES` | SAM rule table. **Compile-time parameter, not a port**. All NIs share the same content. Each `sam_rule_t` carries `{match, mask, dst_id}`. Used when `USE_ID_TABLE=1` (`SourceRouting`/`IDRouting`). Runtime modification is **out of scope for v0.4.0** — no `SAM_RULE_*` CSR exists; to change the table, re-elaborate the design |
| `MESH_COLS` | int | 4 | 1 ≤ x ≤ 2^X_WIDTH | Mesh column count; bounds `dst_id.x` for `NOC_FLIT_HDR_DST_ID_VALID` |
| `MESH_ROWS` | int | 4 | 1 ≤ x ≤ 2^Y_WIDTH | Mesh row count; bounds `dst_id.y` |
| `NUM_VC` | int | 1 | 1 ≤ x ≤ 8; when x > 1, both `R_ROB_TYPE` and `B_ROB_TYPE` MUST be != `NoRoB` (see `theory_of_operation.md` §RoB allocator §"NoRoB single-VC restriction") | Number of virtual channels per NoC link. Upper bound 8 matches `VC_ID_WIDTH = 3` in flit header (see `packet_format.md` §1.2 Group 2). Forward data link is shared (single 1-bit `valid` + 1× FLIT_WIDTH `flit`). Credit return is per-VC array (`noc_*_credit_*[NUM_VC-1:0]`). `NUM_VC=1` (default) collapses to single-VC operation. `NUM_VC > 1` adds VC mapping at NMU per `theory_of_operation.md` §"VC Mapping" (design-time fixed VC selection) and cycle-level round-robin VC arbitration at NMU/NSU egress (see `packet_format.md` §VC Arbitration). Deadlock-free routing across VCs is the integrator's responsibility. |
| `CDC_FIFO_DEPTH` | int | 16 | 4 ≤ x ≤ 64 (power-of-2 recommended) | Internal AXI ↔ NoC async-FIFO depth (gray-counter pointer + 2FF synchroniser). Sized to absorb `2 × max_round_trip_cycles × max(aclk_period, noc_clk_period) / min(aclk_period, noc_clk_period) + 2`; default 16 is conservative for ratio range [0.1, 10]. |
| `MAX_OUTSTANDING` | int | 8 | 1 ≤ x ≤ MAX_TXNS | Test-author-configurable (BFM knob via `transaction_api.md`) or compile-time parameter (RTL); **not CSR-accessible** (no `MAX_OUTSTANDING` CSR exists in `registers.md`). Caps concurrent outstanding transactions below the `MAX_TXNS` hardware ceiling for stress-testing scenarios. Default 8 when set as compile-time parameter. |
| `MAX_BURST_LEN` | int | 16 | 1 ≤ x ≤ 256 | BFM W-reassembly buffer-depth limit — a capacity bound, **not** an AXI4 legality check (`awlen` up to 255 is legal AXI4). A burst with `len + 1 > MAX_BURST_LEN` exceeds the configured buffer (raise the parameter). The BFM returns `BURST_LEN_EXCEEDS_MAX`. See `transaction_api.md`. |
| `ECC_GRANULE_WIDTH` | retired | — | — | (v0.3.0) Per-granule SECDED scheme retired in v0.4.0 in favour of whole-flit SECDED via `FLIT_ECC_WIDTH`. |
| `ECC_PER_GRANULE_WIDTH` | retired | — | — | (v0.3.0) Per-granule ECC width parameter retired with the per-granule scheme. |
| `ECC_FAIL_WIDTH` | retired | — | — | (v0.3.0) `ecc_fail` B-payload field dropped in v0.4.0. The NoC fabric no longer signals uncorrectable ECC via AXI rresp/bresp; visibility is via `ERR_STATUS[0] ecc_uncorr_err` + `ECC_UNCORR_ERR_CNT` + `irq_o` (per `protocol_rules.md` `NOC_FLIT_HDR_FLIT_ECC_CHECK`). The corrupted flit is forwarded as-is. |
| `ECC_WIDTH` | retired | — | — | (v0.3.0) Total per-granule ECC width replaced by parameter `FLIT_ECC_WIDTH` (whole-flit SECDED syndrome). |
| `MAX_RO_TXNS_PER_ID` | int | 32 | 1 ≤ x ≤ MAX_TXNS_PER_ID | NormalRoB status-table FIFO depth per AXI ID. Bounds simultaneous outstanding transactions per ID that **require reordering** (i.e., go to different destinations). |
| `ONLY_METADATA_B` | bool | true | — | B-RoB skips data-SRAM (B response is metadata-only: bid + bresp + buser). Saves significant area. R-RoB is always SRAM-backed (rdata is bulk data). |
| `NSU_R_BUFFER_DEPTH` | int | 16 | 1 ≤ x ≤ 64 | NSU read response buffer depth (entries × R flit). Smooths AXI-slave-to-NoC R injection back-pressure. "Read responses are buffered before forwarding to minimize bubbles." Independent of MetaBuffer (which stores request headers). |
| `EXCLUSIVE_MONITOR_DEPTH` | int | 8 | 1 ≤ x ≤ MAX_TXNS | NSU Exclusive Monitor capacity (per-axi_id reservation slots). Limits concurrent exclusive-access reservations. |
| `ENABLE_AXI_PARITY` | bool | true | — | Enable AXI-side byte parity (data: 1 bit/byte) and address parity (1 bit). Default-on: AXI parity is always enabled. When true: `axi_*_par_*` sideband signals are present. When false: omitted. Independent of NoC-fabric flit-level ECC (`flit_ecc` / `route_par`). This is end-of-pipe AXI integrity only. |

## Optional features in / out of scope

- **Supported**: AXI4 full (AW/W/AR/B/R with bursts up to AWLEN=255 / ARLEN=255; multiple outstanding via RoB), end-to-end SECDED ECC on W and R data, RoB modes (Normal / Simple / NoRoB) per-channel, Performance Probes (Packet / Transaction), runtime CSR-driven configuration, dual-clock-domain operation with internal CDC. VC egress arbitration is round-robin (no QoS-based arbitration in this version).
- **AXI4 ATOPs / AXI5**: monitor-only — `axi_awatop_i` is sampled and recorded, ATOP transactions are terminated with `bresp=SLVERR` (per ToO §ATOPs scope). **Stimulus generation is out of scope**.
- **Not supported**: Cache-coherent / CHI-derived features. Low-power AXI handshake (CACTIVE/CSYSREQ).

## Channel grouping

Multiple logical channels across two AXI ports + NoC + CSR. Protocol-rule IDs in `protocol_rules.md` use these channel tokens:

| Channel | Wires |
|---------|-------|
| AW_IN | axi_aw*_i (slave port AW) |
| W_IN | axi_w*_i |
| B_IN | axi_b*_o |
| AR_IN | axi_ar*_i |
| R_IN | axi_r*_o |
| AW_OUT | axi_aw*_o (master port AW) |
| W_OUT | axi_w*_o |
| B_OUT | axi_b*_i |
| AR_OUT | axi_ar*_o |
| R_OUT | axi_r*_i |
| REQ_OUT | `noc_req_valid_o`, `noc_req_flit_o`, `noc_req_credit_i[NUM_VC-1:0]` |
| REQ_IN | `noc_req_valid_i`, `noc_req_flit_i`, `noc_req_credit_o[NUM_VC-1:0]` |
| RSP_OUT | `noc_rsp_valid_o`, `noc_rsp_flit_o`, `noc_rsp_credit_i[NUM_VC-1:0]` |
| RSP_IN | `noc_rsp_valid_i`, `noc_rsp_flit_i`, `noc_rsp_credit_o[NUM_VC-1:0]` |
| CSR_AW / CSR_W / CSR_B / CSR_AR / CSR_R | csr_aw* / csr_w* / csr_b* / csr_ar* / csr_r* |

This separates master-side and slave-side channels (e.g., `AW_IN` vs `AW_OUT`) so rules can be specific to which port a given AXI4 STABLE rule applies to.

## Signal bundles (struct dot-notation)

Other spec documents (`channel_api.md`, `channel_handshake.md`, `protocol_rules.md`, `active_passive_mode.md`) refer to groups of related wires using SystemVerilog packed-struct dot-notation (e.g., `axi_req_i.awvalid`, `axi_rsp_o.b*`). These bundles are *logical groupings* over the bare wires declared in §Wire table — no separate physical wires. Convention follows ARM AXI4 `*_req_t` / `*_rsp_t` style.

**Bundle resolution rule**: `<bundle>_<dir>.<field>` resolves to wire `<bundle-prefix>_<field>_<dir>`, where the direction suffix on the bundle instance carries to every field. Wildcards (`.aw*`, `.*valid`, `.*ready`) expand to the field set listed below.

### `axi_req_t` — AXI4 request bundle (source → sink)

Carries everything the request initiator drives. Two instances in this spec:

- `axi_req_i` — slave port (BFM input; local AXI master drives)
- `axi_req_o` — master port (BFM output; BFM drives to local AXI slave)

Fields: `awvalid`, `awid`, `awaddr`, `awlen`, `awsize`, `awburst`, `awlock`, `awcache`, `awprot`, `awqos`, `awregion`, `awuser`, `awatop`, `wvalid`, `wdata`, `wstrb`, `wlast`, `wuser`, `bready`, `arvalid`, `arid`, `araddr`, `arlen`, `arsize`, `arburst`, `arlock`, `arcache`, `arprot`, `arqos`, `arregion`, `aruser`, `rready`.

Example: `axi_req_i.awvalid` ≡ wire `axi_awvalid_i`. `axi_req_o.aw*` expands to `axi_awvalid_o, axi_awid_o, axi_awaddr_o, ...`.

### `axi_rsp_t` — AXI4 response bundle (sink → source)

Carries everything the response responder drives.

- `axi_rsp_o` — slave port (BFM output; BFM drives back to local AXI master)
- `axi_rsp_i` — master port (BFM input; local AXI slave drives)

Fields: `awready`, `wready`, `bvalid`, `bid`, `bresp`, `buser`, `arready`, `rvalid`, `rid`, `rdata`, `rresp`, `rlast`, `ruser`.

Example: `axi_rsp_o.bvalid` ≡ wire `axi_bvalid_o`. `axi_rsp_*.*valid` expands to `bvalid` + `rvalid`. `axi_rsp_*.*ready` expands to `awready` + `wready` + `arready`.

### `csr_axi_req_t` / `csr_axi_rsp_t` — AXI4-Lite CSR bundles

Same shape as `axi_req_t` / `axi_rsp_t` but restricted to the AXI4-Lite wire subset (no `awid`/`awlen`/`awsize`/`awburst`/`awlock`/`awcache`/`awqos`/`awregion`/`awuser`/`awatop`; no `wlast`/`wuser`; no `bid`/`buser`; no `arid`/`arlen`/`arsize`/`arburst`/`arlock`/`arcache`/`arqos`/`arregion`/`aruser`; no `rid`/`rlast`/`ruser`).

- `csr_axi_req_i` — software-side AXI4-Lite request inputs (= `csr_awvalid_i, csr_awaddr_i, csr_awprot_i, csr_wvalid_i, csr_wdata_i, csr_wstrb_i, csr_bready_i, csr_arvalid_i, csr_araddr_i, csr_arprot_i, csr_rready_i`).
- `csr_axi_rsp_o` — BFM-side AXI4-Lite response outputs (= `csr_awready_o, csr_wready_o, csr_bvalid_o, csr_bresp_o, csr_arready_o, csr_rvalid_o, csr_rdata_o, csr_rresp_o`).

### NoC link bundles (informal grouping only)

NoC links (`noc_req_o`, `noc_req_i`, `noc_rsp_o`, `noc_rsp_i`) are referred to by link-name in spec prose. The wires of each link are listed in §Channel grouping (REQ_OUT/REQ_IN/RSP_OUT/RSP_IN rows) and §Wire table §NoC Request/Response link sub-sections. Dot-notation is occasionally used for clarity (e.g., `noc_req_o.flit` ≡ `noc_req_flit_o`); this is the same resolution rule as AXI bundles.
