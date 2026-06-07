# Network interface (NI)

## Overview

Network Interface (NI) bridges an AXI4 manager / subordinate pair to the NoC packet substrate. The NI consists of two functional units -- NMU (Network Manager Unit) on the AXI-master ingress side and NSU (Network Subordinate Unit) on the AXI-subordinate egress side. The NI performs protocol conversion (AXI4 <-> flit) and reorder-buffered response handling on the NMU response path. This README describes the current C++ behaviour model (`c_model/`); an RTL counterpart is not yet in this repository.

## Features

The list below describes the NI features currently in `c_model/`. Per-feature design intent (modes not yet exposed, registers not yet wired) lives in the sibling specs under `spec/ni/doc/`.

**NMU (Network Manager Unit, `c_model/include/nmu/`):**

- **AXI4 slave port** (`AxiSlavePort`) -- accepts AW / W / AR beats from the AXI4 master with queue-vacancy-only `can_accept_*()` (no single-burst gating; see `axi_slave_port.hpp:101-103`).
- **Address translation** (`addr_trans::xy_route`, `addr_trans.hpp:31`) -- extracts the destination ID from the upper address bits using XY bit-slice routing; the local address passed downstream is the input address unmodified.
- **Packetize** (`packetize.hpp`) -- assembles AXI AW / W / AR beats into NoC request flits. The `noc_qos`, `route_par`, and `flit_ecc` header fields are zero-filled in this build (deferred; `packetize.hpp:24-26`).
- **Reorder Buffer (RoB)** (`rob.hpp`) -- per-AXI-ID response reordering on the read and write response paths. Per-channel mode is `RobMode::{Disabled, Enabled}` (`rob.hpp:19`), selected at construction via `NmuConfig::write_rob_mode` / `NmuConfig::read_rob_mode`.
- **Depacketize** (`depacketize.hpp`) -- parses inbound response flits into AXI B / R beats.
- **VC arbiter** (`VcArbiter`, `vc_arbiter.hpp`) -- two static factory methods, `read_write_split` and `multi_candidate`, selected at construction via `NmuConfig::vc_mode`.

**NSU (Network Subordinate Unit, `c_model/include/nsu/`):**

- **Depacketize** (`depacketize.hpp`) -- parses inbound request flits into AXI AW / W / AR beats for the downstream AXI4 slave.
- **Meta buffer** (`meta_buffer.hpp`) -- snapshots request-side flit metadata (`rob_idx`, `src_id`, `awid` / `arid`) at AW / AR reception and retrieves it at response injection time to reconstruct the return path.
- **Packetize** (`packetize.hpp`) -- assembles AXI B / R beats into NoC response flits. The `noc_qos`, `route_par`, and `flit_ecc` header fields are zero-filled (deferred).
- **AXI4 master port** (`AxiMasterPort`, `axi_master_port.hpp`) -- drives AW / W / AR to the downstream AXI4 slave and captures B / R.
- **VC arbiter** (`VcArbiter`, `vc_arbiter.hpp`) -- factory methods mirror the NMU side.

**Cross-cutting:**

- **Wide flit channel** -- each AXI4 beat (single AW, single W beat, single AR, single B, single R beat) fits in one NoC flit. Multi-beat bursts produce N+1 W flits per burst at the channel boundary (`spec/ni/dv/plan.md` TP3).
- **Error monitoring CSRs** -- `RegisterFile` (`c_model/include/register_file.hpp`, implemented in `c_model/src/register_file.cpp`) holds three offsets: `ERR_STATUS` (RW1C), `ECC_UNCORR_ERR_CNT`, `LAST_ERR_INFO` (`register_file.cpp:39-41,84-86`).

## Description

The NI is a bus-attached IP that lives at every NoC node where AXI4 traffic enters or exits the mesh. NMU receives AXI requests from a local AXI master, packs them into flits, and injects to the request network. NSU receives request flits from the network, unpacks them, drives AXI requests to a local AXI slave, captures the responses, packs them into response flits, and injects to the response network. NMU also handles the inverse path for incoming responses (RoB-mediated reordering and AXI B / R generation).

CSRs are accessed by direct method call on `RegisterFile` from the testbench; there is no AXI4-Lite access port in the c_model.

## Compatibility

The NI implements AXI4 (ARM IHI 0022) on the host side. The register set, QoS / Probe / Error CSR design, and the flit format are this IP's own conventions; the flit format is internal to this NoC and not interoperable with external networks.

## Further reading

- [Theory of operation](./doc/theory_of_operation.md)
- [Signal interface](./doc/signal_interface.md)
- [Pin-level reset](./doc/pin_level_reset.md)
- [Protocol rules](./doc/protocol_rules.md)
- [Channel handshake and dependencies](./doc/channel_handshake.md)
- [Transaction API](./doc/transaction_api.md)
- [Channel API](./doc/channel_api.md)
- [Active vs passive mode](./doc/active_passive_mode.md)
- [Registers](./doc/registers.md)
- [DV plan](./dv/plan.md)
