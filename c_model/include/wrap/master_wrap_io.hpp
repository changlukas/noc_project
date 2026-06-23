// AxiMaster shell IO POD structs — Stage 5b spec §6.1 (axi_intf wire bundle).
//
// MasterInputs: signals consumed by the master (driven by the slave side).
//   awready/wready/arready — handshake ready (slave asserts to accept beats)
//   B channel              — write response from slave (bvalid + bid + bresp)
//   R channel              — read data from slave (rvalid + rid + rdata + rresp + rlast)
//
// MasterOutputs: signals driven by the master onto the axi_intf wire.
//   AW channel  — write address (awvalid + full attribute set)
//   W channel   — write data    (wvalid + wdata + wstrb + wlast)
//   bready      — master accepts write response
//   AR channel  — read  address (arvalid + full attribute set)
//   rready      — master accepts read data
//
// DATA_BYTES = 32 (256-bit data bus per ni::WSTRB_WIDTH). All multi-byte
// fields are stored byte-array little-endian, matching the axi_intf.sv wire.
#pragma once
#include "axi/types.hpp"  // axi::DATA_BYTES
#include <array>
#include <cstdint>

namespace ni::cmodel::wrap {

// 256-bit data bus = 32 bytes; 32-bit WSTRB = 4 bytes packed as uint32_t.
// Single source of truth: axi::DATA_BYTES (= ni::WSTRB_WIDTH). All wrap
// IO structs alias this so the DPI/SV wire and the c_model agree.
constexpr int AXI_DATA_BYTES = axi::DATA_BYTES;
static_assert(axi::DATA_BYTES == 32,
              "DPI/SV side hard-codes 256-bit data bus (32 bytes); wrap "
              "IO structs assume the same width");

// MasterInputs: from slave side (consumed by the master each cycle).
struct MasterInputs {
    // Handshake ready signals (slave drives; master samples)
    bool awready;
    bool wready;
    bool arready;
    // B channel — write response (slave drives)
    bool bvalid;
    uint8_t bid;
    uint8_t bresp;  // 2-bit AXI4 response code stored in low 2 bits
    // R channel — read data (slave drives)
    bool rvalid;
    uint8_t rid;
    std::array<uint8_t, AXI_DATA_BYTES> rdata;
    uint8_t rresp;  // 2-bit
    bool rlast;
};

// MasterOutputs: from master (driven onto axi_intf wire each cycle).
struct MasterOutputs {
    // AW channel — write address
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
    // W channel — write data
    bool wvalid;
    std::array<uint8_t, AXI_DATA_BYTES> wdata;
    uint32_t wstrb;
    bool wlast;
    // B channel — master accepts write response
    bool bready;
    // AR channel — read address
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
    // R channel — master accepts read data
    bool rready;
};

}  // namespace ni::cmodel::wrap
