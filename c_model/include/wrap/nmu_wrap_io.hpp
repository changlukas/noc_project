// NmuWrap IO POD structs — Stage 5b spec §6 (combined AXI slave + NoC).
//
// NmuInputs: signals consumed by the Nmu each cycle.
//   AXI slave side: master drives AW/W/AR onto axi_intf; Nmu accepts them.
//   NoC rsp side:   flit arriving from channel model toward Nmu's Depacketize.
//   NoC req credit: credit_return from downstream (channel credits back to Nmu).
//
// NmuOutputs: signals driven by the Nmu each cycle.
//   AXI slave side: Nmu drives awready/wready/arready handshake + B/R channels.
//   NoC req side:   flit produced by Nmu's Packetize stage, leaving toward NoC.
//   NoC rsp credit: credit_return Nmu returns to the rsp-side upstream.
//
// FLIT_BYTES = 51 (ni::FLIT_WIDTH = 408 bits, rounded to bytes).
// AXI_DATA_BYTES = 32 (256-bit data bus).
// All multi-byte fields are byte-array little-endian, matching DPI wire packing.
#pragma once
#include "axi/types.hpp"                   // axi::DATA_BYTES
#include "wrap/channel_model_wrap_io.hpp"  // FlitBytes, FLIT_BYTES
#include <array>
#include <cstdint>

namespace ni::cmodel::wrap {

// 256-bit data bus = 32 bytes. Aliased from axi::DATA_BYTES so the DPI/SV
// wire width agrees across master/slave/nmu/nsu wrap IO structs.
constexpr int NMU_AXI_DATA_BYTES = axi::DATA_BYTES;

// NmuInputs: signals consumed by Nmu each cycle (master drives these).
struct NmuInputs {
    // AXI slave side — AW channel (master drives write address)
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
    // AXI slave side — W channel (master drives write data)
    bool wvalid;
    std::array<uint8_t, NMU_AXI_DATA_BYTES> wdata;
    uint32_t wstrb;
    bool wlast;
    // AXI slave side — B channel (master accepts write response)
    bool bready;
    // AXI slave side — AR channel (master drives read address)
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
    // AXI slave side — R channel (master accepts read data)
    bool rready;
    // NoC rsp side — flit arriving from channel toward Nmu Depacketize
    bool noc_rsp_valid;
    FlitBytes noc_rsp_flit;
    // NoC req credit — PULSE: router LOCAL input drained an Nmu req flit, return
    // one credit to the req-out sender counter (NUM_VC=1 PoC: 1 bit)
    bool noc_req_credit_return;
};

// NmuOutputs: signals driven by Nmu each cycle.
struct NmuOutputs {
    // AXI slave side — handshake ready (Nmu accepts beats from master)
    bool awready;
    bool wready;
    bool arready;
    // AXI slave side — B channel (Nmu drives write response)
    bool bvalid;
    uint8_t bid;
    uint8_t bresp;  // 2-bit AXI4 response code in low 2 bits
    // AXI slave side — R channel (Nmu drives read data)
    bool rvalid;
    uint8_t rid;
    std::array<uint8_t, NMU_AXI_DATA_BYTES> rdata;
    uint8_t rresp;  // 2-bit
    bool rlast;
    // NoC req side — flit produced by Nmu Packetize, leaving toward channel
    bool noc_req_valid;
    FlitBytes noc_req_flit;
    // NoC rsp credit — consumer PULSE: Nmu Depacketize consumed an injected rsp
    // flit, return one credit to the router LOCAL output sender counter
    bool noc_rsp_credit_return;
};

}  // namespace ni::cmodel::wrap
