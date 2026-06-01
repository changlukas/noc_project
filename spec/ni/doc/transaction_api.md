# Transaction API

## API conventions

- Pseudocode style: C-like; maps to SystemVerilog tasks, SystemC functions, C++ methods via DPI-C.
- Naming: snake_case. `apply_*` for active stimulus drives, `expect_*` for monitor-mode assertions, `set_*` for CSR-mapped or knob-style configuration, `get_*` for observation getters, `csr_*` for direct CSR file access, `reset_state` for internal state reset.
- Error reporting: `status_t` enum return; `OK` on success, documented error enums otherwise.
- Blocking discipline: `apply_*` and `expect_*` block until the corresponding wire-level handshake completes (or timeout). All `set_*`, `get_*`, `csr_*`, and `reset_state` are non-blocking and return on the same testbench cycle they are called.

### Method documentation form

Every method is documented under §Method details with five sub-sections (Signature, Preconditions, Side effects, Return value, Error modes) **except** in three compact-form cases below, where omitted sub-sections are inherited or implied:

- **Mirror methods** (documented as "Mirror of X for reads/responses", e.g. `apply_axi_read`, `apply_burst_read`, `expect_axi_read`, `expect_axi_burst_read`, `expect_axi_burst_write`, `expect_noc_response`, `csr_read`, `set_response_delay_noc`): inherit all five sub-sections of the named parent method `X`, with the obvious channel/direction substitution. Where return value or error modes diverge, the mirror entry states the difference explicitly.
- **CSR wrappers** (`set_qos_mode`, `set_qos_fixed_value`, `set_bandwidth_limit`, `set_saturation_threshold`, `set_low_priority`, `set_bandwidth_budget`, `set_base_qos`, `set_urgency_step`, `set_socket_qos`, `set_pkt_probe`, `set_txn_probe`, `clear_err_status`, `set_quiesce`, `clear_exclusive_monitor`, `get_pending_r_count`, `get_pending_w_count`, `get_quiesce_idle`, `get_exclusive_monitor_occupancy`): inherit all five sub-sections of `csr_write` (or `csr_read` for `get_*` wrappers) with the offset and field encoding fixed per the wrapper signature. The "Equivalent channel API decomposition" of `csr_write` / `csr_read` applies transitively.
- **Observation getters and one-shot knobs** (`get_observed_axi_writes`, `get_observed_axi_reads`, `get_observed_noc_flits`, `set_inject_ecc_error`, `set_response_fault`, `set_bfm_mode`, `reset_state`): document Signature + Side effects explicitly. Implied: Preconditions = `BFM instantiated, post-reset`; Return value = `void` (or as stated in Signature); Error modes = `none` (illegal arguments trigger immediate assertion, not a return enum). Equivalent channel API decomposition is `(none — Transaction-API-only)`.

## When the Transaction API is insufficient

Transaction API covers ~95% of typical tests (single-master single-slave, RoB ordering checks, ECC error injection via knobs, CSR-driven QoS / Probe configuration). Use `channel_api.md` instead for:

- Driving AXI handshake without matching response (back-pressure recovery test on master DUT or slave DUT).
- Holding NoC `ready` LOW for extended cycles to test router timeout behavior on the upstream side.
- Injecting illegal flit headers (bad `dst_id`, malformed ECC, duplicate `rob_idx`) to test downstream validation.
- Driving per-cycle deterministic patterns on `awready` / `arready` / `noc_*_i.ready` (vs. randomized via `set_response_delay_*`).

## Method index

| Group | Method | One-line summary |
|-------|--------|------------------|
| **NMU stimulus (active mode; slave port)** ||
| Stimulus | `apply_axi_write(addr, data, [strb], [id], [qos])` | Drive a single-beat AXI write on the slave port; block until B response. |
| Stimulus | `apply_axi_read(addr, [id], [qos]) -> data` | Drive a single-beat AXI read; block until R received. |
| Stimulus | `apply_burst_write(addr, len, data[], [strb[]], [id], [burst_type], [qos])` | Drive an AXI burst write. |
| Stimulus | `apply_burst_read(addr, len, [id], [burst_type], [qos]) -> data[]` | Drive an AXI burst read; block until full burst received. |
| **NSU monitor (active or passive mode; master port)** ||
| Monitor | `expect_axi_write(addr, data, [timeout]) -> status_t` | Wait for an AXI write to addr on master port; assert WDATA matches. |
| Monitor | `expect_axi_read(addr, [timeout]) -> (status_t, data)` | Wait for an AXI read on master port; return BFM-supplied RDATA. |
| Monitor | `expect_axi_burst_write(addr, len, data[], [timeout]) -> status_t` | Wait for a burst write; verify all beats. |
| Monitor | `expect_axi_burst_read(addr, len, [timeout]) -> (status_t, data[])` | Wait for a burst read; return supplied RDATA[]. |
| **NoC monitor (any mode)** ||
| Monitor | `expect_noc_request(addr_match=<opt>, [timeout]) -> (status_t, flit_t)` | Wait for a NoC request flit on `noc_req_o`. |
| Monitor | `expect_noc_response(rob_idx_match=<opt>, [timeout]) -> (status_t, flit_t)` | Wait for a NoC response flit on `noc_rsp_o`. |
| Monitor | `get_observed_axi_writes(port) -> list` | Return all observed AXI writes since last reset_state, on specified port (master or sub). |
| Monitor | `get_observed_axi_reads(port) -> list` | Same for reads. |
| Monitor | `get_observed_noc_flits(link, direction) -> list` | Return observed NoC flits (link ∈ {req, rsp}; direction ∈ {out, in}). |
| **CSR-mapped configuration (via AXI4-Lite CSR port)** ||
| Configuration | `csr_write(offset, data) -> status_t` | Software AXI4-Lite write to CSR file. |
| Configuration | `csr_read(offset) -> (status_t, data)` | Software AXI4-Lite read from CSR file. |
| Configuration | `set_qos_mode(mode)` | Wrapper: csr_write(0x000, mode). |
| Configuration | `set_qos_fixed_value(value)` | Wrapper: csr_write(0x004, value). |
| Configuration | `set_bandwidth_limit(limit)` / `set_saturation_threshold(thr)` / `set_low_priority(qos)` | Wrappers for Limiter mode. |
| Configuration | `set_bandwidth_budget(budget)` / `set_base_qos(value)` / `set_urgency_step(step)` / `set_socket_qos(value, en)` | Wrappers for Regulator mode. (`set_base_qos` writes BASE_QOS[3:0]; `set_urgency_step` writes BASE_QOS[5:4]; both target the same 0x018 register.) |
| Configuration | `set_pkt_probe(en, mode, window_size)` | Wrappers for Packet Probe configuration. |
| Configuration | `set_txn_probe(en, threshold[])` | Wrappers for Transaction Probe configuration. |
| Configuration | `clear_err_status(bit_mask)` | Wrapper: csr_write(0x100, bit_mask) — RW1C clear of selected bits. |
| Configuration | `get_pending_r_count() -> uint` | Wrapper: csr_read(0x130) → `pending_r_count` field. NMU live outstanding-read count. |
| Configuration | `get_pending_w_count() -> uint` | Wrapper: csr_read(0x134) → `pending_w_count` field. NMU live outstanding-write count. |
| Configuration | `set_quiesce(req)` | Wrapper: csr_write(0x13C, req ? 1 : 0). NMU-only quiesce request. |
| Configuration | `get_quiesce_idle() -> bool` | Wrapper: csr_read(0x140) → bit[0]. Polled by software during quiesce flow. |
| Configuration | `clear_exclusive_monitor()` | Wrapper: csr_write(0x144, 1). Self-clearing W1 trigger; invalidates all NSU Exclusive Monitor entries. |
| Configuration | `get_exclusive_monitor_occupancy() -> uint` | Wrapper: csr_read(0x148) → `occupancy` field. NSU Exclusive Monitor live count. |
| **BFM-internal knobs (test-only; no CSR)** ||
| Configuration | `set_response_delay_axi(min, max)` | Synthetic delay on AXI response (BFM-only; RTL has fixed timing). |
| Configuration | `set_response_delay_noc(min, max)` | Synthetic delay on NoC injection. |
| Configuration | `set_inject_ecc_error(channel, kind)` | One-shot: corrupt ECC in next flit injection (kind ∈ {single-bit, double-bit}). |
| Configuration | `set_response_fault(channel, kind)` | Inject SLVERR/DECERR in next response (channel ∈ {B, R}). |
| Configuration | `set_bfm_mode(mode)` | Switch ACTIVE / PASSIVE; per active_passive_mode.md. |
| **State** ||
| State | `reset_state()` | Drop trackers + observed lists; restore knobs to defaults. |

## Method details

### apply_axi_write(addr, data, strb=all-ones, id=0, qos=0) -> status_t

**Signature:** `status_t apply_axi_write(uint64_t addr, uint64_t data, uint8_t strb, uint8_t id, uint4_t qos)`

**Preconditions:**
- BFM instantiated, `arst_ni` and `noc_rst_ni` have both deasserted at least once.
- `bfm_mode == ACTIVE` (passive mode does not drive stimulus).
- `id` ≤ MAX_UNIQUE_IDS; outstanding count for `id` < MAX_TXNS_PER_ID; total outstanding < MAX_TXNS.
- No concurrent `apply_axi_*` to overlapping addresses.
- No Channel API call holding AW or W channels of the slave port.

**Side effects:**
- Drives `axi_*_i` AW phase: awvalid HIGH, awid=id, awaddr=addr, awlen=0, awsize=log2(strb width), awburst=INCR, awqos=qos. Holds until awready observed.
- Drives W phase: wvalid HIGH, wdata=data, wstrb=strb, wlast=1. Holds until wready observed.
- NMU internally: AddrTrans → QoSGen → FlitPack → ECC Gen → InjectionBuffer → noc_req_o injection (AW + W flits).
- RoB allocates an entry for AWID=id; entry tracks B response.
- Blocks until B handshake observed at `axi_*_i`: bvalid HIGH, bid=id, bresp captured.
- The configured response_fault (if any) consumed (one-shot).
- Observed transaction appended to `get_observed_axi_writes(MANAGER)` list; corresponding NoC AW + W flits appended to `get_observed_noc_flits(REQ, OUT)`.

**Return value:**
- `OK`: write completed; bresp=OKAY.
- `SLVERR`: bresp=SLVERR (e.g., 4KB boundary violation, Exclusive monitor overflow, configured fault). Note: flit_ecc uncorrectable does NOT cause SLVERR — the corrupted flit is forwarded with `bresp=OKAY` and the error is observable only via CSR (`ECC_UNCORR_ERR_CNT`, `ERR_STATUS[0]`) plus `irq_o`. See ToO §ECC for the (B)-philosophy rationale.
- `DECERR`: bresp=DECERR.
- `RESET_DURING_TRANSACTION`: arst_ni or noc_rst_ni asserted before completion.
- `MODE_SWITCHED_TO_PASSIVE`: bfm_mode set to PASSIVE during call.

**Error modes:**

| Status | Trigger | BFM logs? |
|--------|---------|-----------|
| SLVERR | bresp=SLVERR observed | yes |
| DECERR | bresp=DECERR observed | yes |
| RESET_DURING_TRANSACTION | Either reset asserted | yes |
| MODE_SWITCHED_TO_PASSIVE | bfm_mode → PASSIVE during call | yes |

**Example:**
```c
status_t s = apply_axi_write(0x1000, 0xDEADBEEF, 0xFF, /*id=*/3, /*qos=*/8);
if (s != OK) { /* handle error */ }
```

**Equivalent channel API decomposition:**
```
begin_phase_AW_OUT(addr=addr, id=id, len=0, size=...)
assert_valid_AW_OUT()
wait_for_ready_AW_OUT()
end_phase_AW_OUT()
begin_phase_W_OUT(data=data, strb=strb, last=1)
assert_valid_W_OUT()
wait_for_ready_W_OUT()
end_phase_W_OUT()
begin_phase_B_OUT()
assert_ready_B_OUT()
wait_for_valid_B_OUT(id_match=id)
end_phase_B_OUT()
```
Plus internal NMU pipeline (FlitPack + ECC + InjectionBuffer) and NoC link injection on `noc_req_o`, which have no Channel API equivalent (these are NMU-internal).

### apply_axi_read(addr, id=0, qos=0) -> (status_t, data)

**Signature:** `(status_t, uint64_t) apply_axi_read(uint64_t addr, uint8_t id, uint4_t qos)`

**Preconditions:** Same as `apply_axi_write` for ACTIVE mode + outstanding limits.

**Side effects:**
- Drives `axi_*_i` AR phase: arvalid HIGH, arid=id, araddr=addr, arlen=0, arsize=full-width, arburst=INCR, arqos=qos.
- NMU: AddrTrans → QoSGen → FlitPack → noc_req_o (AR flit).
- RoB allocates for ARID=id.
- Blocks until R handshake at `axi_*_i` with rid=id, rlast=1.
- Returns (status, observed rdata).

**Return value:** `(OK, rdata)` on success; status enums same as `apply_axi_write`.

**Equivalent channel API decomposition:**
```
begin_phase_AR_OUT(addr=addr, id=id, ...)
assert_valid_AR_OUT()
wait_for_ready_AR_OUT()
end_phase_AR_OUT()
begin_phase_R_OUT()
assert_ready_R_OUT()
wait_for_valid_R_OUT(id_match=id)
end_phase_R_OUT()
```

### apply_burst_write(addr, len, data[], strb[]=all-ones, id=0, burst_type=INCR, qos=0) -> status_t

**Signature:** `status_t apply_burst_write(uint64_t addr, uint8_t len, uint64_t data[], uint8_t strb[], uint8_t id, axi_burst_t burst_type, uint4_t qos)`

`len` is awlen (0..255 per AXI4); `data` and `strb` arrays have len+1 entries.

**Preconditions:** Same as apply_axi_write. `len + 1 ≤ MAX_BURST_LEN` is a BFM buffer-capacity bound, not an AXI4 legality limit (see Maximum burst length below).

**Burst-type support:** `burst_type ∈ {FIXED, INCR, WRAP}` are all accepted. FIXED holds the address constant on every beat (per AXI4 §A3.4.1); the NMU passes the constant address to the NoC layer unchanged. INCR and WRAP semantics as below.

**Maximum burst length (`MAX_BURST_LEN`):** parameter that bounds the NSU's W-reassembly buffer depth. Default `MAX_BURST_LEN = 16` (sufficient for awlen ≤ 15 = 16 beats). To support full AXI4 max-length bursts (`awlen = 255` = 256 beats per `signal_interface.md` §"Optional features in / out of scope"), set `MAX_BURST_LEN ≥ 256` at BFM instantiation. The default 16-beat ceiling is a conservative choice for typical CPU-driven workloads. Integrators with DMA-style bulk-transfer workloads should override. `MAX_BURST_LEN` is a BFM W-reassembly buffer-depth limit, **not** an AXI4 legality check — `awlen` up to 255 is legal AXI4. A test issuing `len + 1 > MAX_BURST_LEN` exceeds the BFM's configured buffer capacity (raise `MAX_BURST_LEN`), so the BFM returns `BURST_LEN_EXCEEDS_MAX` to signal the capacity limit, not a protocol violation. Full per-burst-type AXI4 legality checking (NI-WIDTH-11) is Width Bridge spec scope, not basic-version NI core.

**Side effects:**
- Drives one AW phase + (len+1) W phases. wlast asserts on beat len+1 only.
- For burst_type INCR: each beat has aligned address `addr + i × (1 << size)`.
- For burst_type WRAP: address wraps within burst boundary per AXI4 §A3.4.1; the WRAP boundary is by AXI4 construction within a 4KB region, so 4KB-crossing rules in `protocol_rules.md` (`AXI4_SLV_AW_BURST_4KB_BOUNDARY`, `AXI4_SLV_AR_BURST_4KB_BOUNDARY`) do not apply to WRAP bursts.
- For burst_type FIXED: every beat uses the same address; FIXED has no notion of "boundary crossing", so the 4KB rules do not apply.
- NMU injects 1 AW flit + (len+1) W flits onto `noc_req_o`. NoC routers may interleave with other traffic but W burst itself is wormhole-locked at NSU reassembly.

**Return value / Error modes:** Same as apply_axi_write, plus one additional enum:
- `BURST_LEN_EXCEEDS_MAX`: `len + 1 > MAX_BURST_LEN` (exceeds configured BFM buffer capacity — raise `MAX_BURST_LEN`; not an AXI4 protocol violation).

**Example:**
```c
uint64_t buf[16] = { /* ... */ };
uint8_t strb[16] = { 0xFF, 0xFF, ... };
status_t s = apply_burst_write(0x2000, /*len=*/15, buf, strb, /*id=*/5);
```

**Equivalent channel API decomposition:** AW phase + (len+1) W phases; on the W channel, repeat begin/assert_valid/wait_for_ready/end for each beat with appropriate `last` flag on the final beat.

### apply_burst_read(addr, len, id=0, burst_type=INCR, qos=0) -> (status_t, data[])

Mirror of apply_burst_write but for reads. Returns array of len+1 rdata values.

### expect_axi_write(addr, data, timeout=10000) -> status_t

**Signature:** `status_t expect_axi_write(uint64_t addr, uint64_t data, uint32_t timeout_cycles)`

**Preconditions:**
- BFM instantiated, post-reset.
- ACTIVE or PASSIVE mode (works in both for monitoring; in PASSIVE the BFM does not drive B response; an external slave must do that or test will see BFM-side timeout).
- No concurrent expect_axi_write to same addr.

**Side effects:**
- Blocks until master DUT (or NMU forwarding remote write) issues an AXI write to addr on master port.
- In ACTIVE: BFM drives `axi_*_o` B response (after configured response_delay_axi); fault-injection knobs consumed.
- In PASSIVE: BFM observes only.
- WDATA captured and compared against expected `data`; over-strobe lanes compared per WSTRB.

**Return value:** OK / DATA_MISMATCH / STRB_PARTIAL / RESET_DURING_TRANSACTION / MODE_SWITCHED_TO_PASSIVE / TIMEOUT.

**Equivalent channel API decomposition:**
```
begin_phase_AW_IN()
assert_ready_AW_IN()
wait_for_valid_AW_IN(addr_match=addr)   // observe master DUT's AW
end_phase_AW_IN()
begin_phase_W_IN()
assert_ready_W_IN()
wait_for_valid_W_IN(last_match=1)
end_phase_W_IN()
begin_phase_B_IN(bresp=<per fault>, bid=<from observed AW>)
assert_valid_B_IN()
wait_for_ready_B_IN()
end_phase_B_IN()
```

### expect_axi_read(addr, timeout=10000) -> (status_t, data)

Mirror of expect_axi_write but for reads. BFM drives R response with rdata pre-loaded via `csr_write(register address, value)` if applicable, or 0x0 default.

### expect_axi_burst_write / expect_axi_burst_read

Burst variants of expect_axi_*.

### expect_noc_request(addr_match=<opt>, timeout=10000) -> (status_t, flit_t)

**Signature:** `(status_t, flit_t) expect_noc_request(uint64_t addr_match, uint32_t timeout)`

**Preconditions:** BFM instantiated, post-noc_rst_ni.

**Side effects:**
- Blocks until a flit is observed on `noc_req_o`. If `addr_match` provided, additionally requires the flit's payload AWADDR or ARADDR field equals addr_match.
- In ACTIVE: BFM injects on `noc_req_o` when per-VC `credit_counter[vc_id] > 0` (per `NOC_MST_FLIT_ON_CREDIT_ONLY`). `response_delay_noc` throttles the BFM's injection rate by inserting cycles between credit availability and `noc_req_valid_o = 1`.
- In PASSIVE: observes only.
- Flit appended to get_observed_noc_flits(REQ, OUT).

**Return:** (OK, flit) / (TIMEOUT, 0) / (RESET_DURING_TRANSACTION, 0).

**Equivalent channel API decomposition:**

`(none — this is Transaction-API-only.)` `expect_noc_request` taps the BFM's internal monitor on `noc_req_o.valid`. No direct channel-API equivalent for self-observation of BFM-driven outputs.

### expect_noc_response(rob_idx_match=<opt>, timeout=10000) -> (status_t, flit_t)

Mirror of expect_noc_request for response link.

### get_observed_axi_writes(port) -> list

Returns the list of all observed AXI writes since last reset_state, on `port ∈ {MANAGER, SUBORDINATE}`. List entries are structs with all observed fields (addr, data, strb, id, qos, beat count for bursts).

### get_observed_axi_reads(port) -> list

Same for reads.

### get_observed_noc_flits(link, direction) -> list

Returns observed flits on `link ∈ {REQ, RSP}` and `direction ∈ {OUT, IN}`. List entries are flit_t with all header + payload fields decoded.

### csr_write(offset, data) -> status_t

**Signature:** `status_t csr_write(uint12_t offset, uint32_t data)`

**Preconditions:**
- BFM post-reset.
- offset is 4-byte-aligned (lower 2 bits = 0).

**Side effects:**
- Drives an AXI4-Lite write transaction on `csr_*` port: AW (csr_awaddr_i=offset, csr_awprot_i=0) + W (csr_wdata_i=data, csr_wstrb_i=0xF) + receives B.
- BFM internally updates the addressed register's state per registers.md (RW writes: store; RW1C writes: clear bits where data has 1; RO writes: ignored, csr_bresp_o=OKAY).

**Return value:** `OK` (csr_bresp_o=OKAY) / `SLVERR` (misaligned, RO violation, etc.) / `DECERR` (unmapped offset) / `RESET_DURING_TRANSACTION` / `TIMEOUT`.

**Example:**
```c
csr_write(0x000, 1);  // QOS_MODE = Fixed
csr_write(0x004, 7);  // QOS_FIXED_VALUE = 7
```

**Equivalent channel API decomposition:** Trivial (one AW + one W + wait for B); typically used directly without channel decomposition.

### csr_read(offset) -> (status_t, data)

Mirror of csr_write for reads.

### set_qos_mode / set_qos_fixed_value / set_bandwidth_limit / etc. (CSR wrappers)

Convenience wrappers around csr_write to specific offsets. Each is a one-line shortcut. Example:

```c
void set_qos_mode(qos_mode_t mode) { csr_write(0x000, mode); }
void set_bandwidth_limit(uint16_t limit) { csr_write(0x008, limit); }
```

For full register layouts and offsets, see registers.md.

### set_response_delay_axi(min_cycles, max_cycles)

**Signature:** `void set_response_delay_axi(uint16_t min_cycles, uint16_t max_cycles)`

**Preconditions:** min ≤ max ≤ 65535.

**Side effects:** BFM internal config: next AXI response (B or R) on the slave port is held off by random K ∈ [min, max] aclk cycles. Persists across transactions until reconfigured or `reset_state`.

**Return value:** none. **Error modes:** none (illegal args trigger immediate assertion).

**Equivalent channel API decomposition:** (none — Transaction-API-only knob; the channel_api.md driver consumes the random count internally.)

### set_response_delay_noc(min_cycles, max_cycles)

Same as above but applies to NoC injection delay (cycles between flit ready-to-inject and noc_*_o.valid HIGH).

### set_inject_ecc_error(channel, kind)

**Signature:** `void set_inject_ecc_error(ecc_channel_t channel, ecc_error_kind_t kind)` where `channel ∈ {W, R}` and `kind ∈ {SINGLE_BIT, DOUBLE_BIT, NONE}`.

**Side effects:** BFM internal one-shot flag set. The next flit injection on the specified channel (W via NMU, R via NSU) has its ECC field corrupted: SINGLE_BIT flips one ECC bit (correctable); DOUBLE_BIT flips two ECC bits (uncorrectable). NONE clears any pending injection.

**Equivalent channel API decomposition:** (none — Transaction-API-only one-shot.)

### set_response_fault(channel, kind)

**Signature:** `void set_response_fault(rsp_channel_t channel, fault_kind_t kind)` where `channel ∈ {B, R}` and `kind ∈ {NONE, SLVERR, DECERR}`.

**Side effects:** BFM internal one-shot fault flag. Next response handshake on the specified channel drives the corresponding bresp/rresp value.

### set_bfm_mode(mode)

**Signature:** `void set_bfm_mode(bfm_mode_t mode)` where `mode ∈ {ACTIVE, PASSIVE}`.

See active_passive_mode.md for full semantics.

**Equivalent channel API decomposition:** (none — global state change.)

### reset_state()

**Signature:** `void reset_state(void)`

**Side effects:** Resets internal BFM state. Per-field behavior:

| Internal state field | reset_state() effect | Notes |
|----------------------|----------------------|-------|
| AXI in-flight tracker (NMU side) | Cleared | Drops any active AW/W/AR/B/R lifecycle |
| AXI in-flight tracker (NSU side) | Cleared | |
| RoB entries (B and R, both) | Cleared (all → FREE) | Per-AXI-ID linked-list state reset |
| Observed AXI write/read lists (both ports) | Cleared | |
| Observed NoC flit lists (both links, both directions) | Cleared | |
| ECC error injection one-shot flags | Cleared | |
| Response fault one-shot flags | Cleared | |
| `set_response_delay_axi` / `set_response_delay_noc` | Reset to (0, 0) | |
| CSR file state (QOS_MODE / QOS_FIXED_VALUE / BANDWIDTH_LIMIT / BANDWIDTH_BUDGET / BASE_QOS / SOCKET_QOS_EN / SOCKET_QOS / PKT_PROBE_* / TXN_PROBE_* / TXN_THRESHOLD_*) | **Preserved** | Software-managed state; reset via wire-level reset or explicit csr_write, not via reset_state |
| `ERR_STATUS` / `ECC_UNCORR_ERR_CNT` / `ECC_CORR_ERR_CNT` / `ROUTE_PAR_ERR_CNT` / `AXI_PARITY_ERR_CNT` / `LAST_ERR_INFO` | **Preserved** | Same; clear via RW1C write to the corresponding `ERR_STATUS` bit (counters auto-clear with their paired bit). `ECC_CORR_ERR_CNT` has no clear path — saturating cumulative. |
| Probe counters (`PKT_BYTE_COUNT`, `TXN_BIN_*_COUNT`, etc.) | **Preserved** | Same |
| `bfm_mode` (ACTIVE/PASSIVE) | **Preserved** | Testbench-level config |

Does **not** toggle `arst_ni` or `noc_rst_ni`.

**Equivalent channel API decomposition:** (none — internal state reset.)

## Behavior under reset

When either `arst_ni` or `noc_rst_ni` asserts during an in-flight Transaction-API call, the call unblocks with `RESET_DURING_TRANSACTION`. In-flight trackers in the affected domain are dropped per pin_level_reset.md §Reset entry sequencing.

If only one of the two resets is asserted (partial reset): cross-domain transactions (AXI→NoC or NoC→AXI) return `RESET_DURING_TRANSACTION`. Single-domain transactions (e.g., CSR access on aclk side while noc_rst_ni asserted) may still complete on the unaffected domain.

## Outstanding-limit overflow behavior

When a test issues a new transaction while the BFM's outstanding tracker is already full (`total outstanding == MAX_TXNS`, or `outstanding count for id == MAX_TXNS_PER_ID`), the BFM **mirrors RTL back-pressure**: the `apply_axi_*` call holds `axi_awready_o`/`axi_arready_o` LOW (or, on the slave-port direction, holds `axi_awvalid_o`/`axi_arvalid_o` until the local slave's ready accepts) and blocks until a tracker slot frees (i.e., a previously-issued transaction's response is received and consumed). The call then resumes, drives the AW/AR phase, and continues to the response phase as usual.

The `Preconditions` line `outstanding count for id < MAX_TXNS_PER_ID; total outstanding < MAX_TXNS` in `apply_axi_write` / `apply_axi_read` is a **performance / liveness guideline, not a hard contract violation**: a test that exceeds the limit will not return an error, but will see the API call stall until back-pressure relief arrives. If the test's slave path is also stalled (deadlock scenario), the call hangs indefinitely — software-side test framework is responsible for upper-bounded watchdog detection.

Behavior matches the RTL counterpart's `AXI4_MST_ROB_OUTSTANDING_LIMIT` rule in `protocol_rules.md` (NMU back-pressures `awready`/`arready` until a slot frees).

There is no separate `OUTSTANDING_LIMIT_EXCEEDED` return enum — overflow manifests as back-pressure-induced latency.

## Concurrency rules

| Combination | Safety | Who enforces |
|-------------|--------|--------------|
| `apply_axi_write` + `apply_axi_read` from different threads | **Safe** | BFM (channels are independent). |
| Two `apply_axi_write` to **different** addresses concurrent | Safe (BFM serialises at outstanding-tracker level) | BFM. |
| Two `apply_axi_write` to **the same / overlapping** address | **Unsafe** — second returns `CONCURRENT_OVERLAP` | BFM. |
| `apply_*` (slave port) + `expect_axi_*` (master port) concurrent | **Safe** — different ports | BFM. |
| Concurrent `csr_write` / `csr_read` from different threads | **Safe**; BFM serialises at AXI4-Lite handshake level | BFM. |
| `apply_*` + `set_*` knob | Safe; `set_*` is atomic | BFM. |
| `apply_*` + Channel API on the same channel | **Forbidden** | BFM enforces via `BUSY_TXN_API` return. |
| `reset_state` while `apply_*` blocked | **Unsafe**; result undefined | Test author should not call. |

The BFM does NOT add internal serialisation for cases marked "Unsafe" above — those are test-author contract violations and produce undefined behavior. The BFM logs warnings where detectable but does not silently arbitrate.
