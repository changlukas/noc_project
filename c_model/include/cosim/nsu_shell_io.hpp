// NsuShellAdapter IO POD structs — Stage 5b spec §6 (combined NoC consumer + AXI master).
//
// NsuInputs: signals consumed by the Nsu each cycle.
//   NoC req side:    flit arriving from channel model toward Nsu's Depacketize.
//   NoC rsp credit:  credit_return from downstream (channel credits back to Nsu).
//   AXI master side: ready signals + B/R beats returning from the AXI subordinate.
//
// NsuOutputs: signals driven by the Nsu each cycle.
//   NoC rsp side:    flit produced by Nsu's Packetize stage, leaving toward NoC.
//   NoC req credit:  credit_return Nsu returns to the req-side upstream.
//   AXI master side: Nsu drives AW/W/AR to subordinate; accepts bready/rready from it.
//
// FLIT_BYTES = 51 (ni::FLIT_WIDTH = 408 bits, rounded to bytes).
// AXI_DATA_BYTES = 32 (256-bit data bus).
// All multi-byte fields are byte-array little-endian, matching DPI wire packing.
//
// Direction inversion vs. NmuShellIO:
//   Nmu consumes noc_rsp / produces noc_req  → Nsu produces noc_rsp / consumes noc_req.
//   Nmu has axi_intf.slave (accepts AW/W/AR) → Nsu has axi_intf.master (drives AW/W/AR).
#pragma once
#include "axi/types.hpp"                     // axi::DATA_BYTES
#include "cosim/channel_model_shell_io.hpp"  // FlitBytes, FLIT_BYTES
#include <array>
#include <cstdint>

namespace ni::cmodel::cosim {

// 256-bit data bus = 32 bytes. Aliased from axi::DATA_BYTES so the DPI/SV
// wire width agrees across master/slave/nmu/nsu shell IO structs.
constexpr int NSU_AXI_DATA_BYTES = axi::DATA_BYTES;

// NsuInputs: signals consumed by Nsu each cycle.
struct NsuInputs {
    // NoC req side — flit arriving from channel toward Nsu Depacketize
    bool noc_req_valid;
    FlitBytes noc_req_flit;
    // NoC rsp credit — PULSE: router LOCAL input drained an Nsu rsp flit, return
    // one credit to the rsp-out sender counter (NUM_VC=1 PoC: 1 bit)
    bool noc_rsp_credit_return;
    // AXI master side — AW channel (subordinate drives ready)
    bool awready;
    // AXI master side — W channel (subordinate drives ready)
    bool wready;
    // AXI master side — B channel (subordinate drives write response)
    bool bvalid;
    uint8_t bid;
    uint8_t bresp;  // 2-bit AXI4 response code in low 2 bits
    // AXI master side — AR channel (subordinate drives ready)
    bool arready;
    // AXI master side — R channel (subordinate drives read data)
    bool rvalid;
    uint8_t rid;
    std::array<uint8_t, NSU_AXI_DATA_BYTES> rdata;
    uint8_t rresp;  // 2-bit
    bool rlast;
};

// NsuOutputs: signals driven by Nsu each cycle.
struct NsuOutputs {
    // NoC rsp side — flit produced by Nsu Packetize, leaving toward channel
    bool noc_rsp_valid;
    FlitBytes noc_rsp_flit;
    // NoC req credit — consumer PULSE: Nsu Depacketize consumed an injected req
    // flit, return one credit to the router LOCAL output sender counter
    bool noc_req_credit_return;
    // AXI master side — AW channel (Nsu drives write address to subordinate)
    bool awvalid;
    uint8_t awid;
    uint64_t awaddr;
    uint8_t awlen;
    uint8_t awsize;
    uint8_t awburst;
    uint8_t awlock;  // 1-bit, stored as uint8_t
    uint8_t awcache;
    uint8_t awprot;
    uint8_t awqos;
    // AXI master side — W channel (Nsu drives write data to subordinate)
    bool wvalid;
    std::array<uint8_t, NSU_AXI_DATA_BYTES> wdata;
    uint32_t wstrb;
    bool wlast;
    // AXI master side — B channel (Nsu accepts write response from subordinate)
    bool bready;
    // AXI master side — AR channel (Nsu drives read address to subordinate)
    bool arvalid;
    uint8_t arid;
    uint64_t araddr;
    uint8_t arlen;
    uint8_t arsize;
    uint8_t arburst;
    uint8_t arlock;  // 1-bit
    uint8_t arcache;
    uint8_t arprot;
    uint8_t arqos;
    // AXI master side — R channel (Nsu accepts read data from subordinate)
    bool rready;
};

}  // namespace ni::cmodel::cosim
