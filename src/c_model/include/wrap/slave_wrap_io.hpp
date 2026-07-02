// AxiSlave wrap IO POD structs — Stage 5b spec §6.1 (axi_intf wire bundle).
//
// SlaveInputs: signals consumed by the slave (driven by the master side onto the wire).
//   AW channel   — write address (awvalid + full attribute set)
//   W channel    — write data    (wvalid + wdata + wstrb + wlast)
//   AR channel   — read  address (arvalid + full attribute set)
//   bready       — master accepts write response
//   rready       — master accepts read data
//
// SlaveOutputs: signals driven by the slave back onto the axi_intf wire.
//   awready/wready/arready — handshake ready (slave accepts beats from master)
//   B channel              — write response (bvalid + bid + bresp)
//   R channel              — read data      (rvalid + rid + rdata + rresp + rlast)
//
// DATA_BYTES = 32 (256-bit data bus per ni::WSTRB_WIDTH). All multi-byte
// fields are stored byte-array little-endian, matching the axi_intf.sv wire.
#pragma once
#include "axi/types.hpp"  // axi::DATA_BYTES
#include <array>
#include <cstdint>

namespace ni::cmodel::wrap {

// 256-bit data bus = 32 bytes. Aliased from axi::DATA_BYTES so the DPI/SV
// wire width agrees across master/slave/nmu/nsu wrap IO structs.
constexpr int SLAVE_AXI_DATA_BYTES = axi::DATA_BYTES;

// SlaveInputs: from master side (consumed by the slave each cycle).
struct SlaveInputs {
    // AW channel — write address (master drives)
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
    // W channel — write data (master drives)
    bool wvalid;
    std::array<uint8_t, SLAVE_AXI_DATA_BYTES> wdata;
    uint32_t wstrb;
    bool wlast;
    // AR channel — read address (master drives)
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
    // Backpressure from master
    bool bready;  // master accepts B channel
    bool rready;  // master accepts R channel
};

// SlaveOutputs: from slave (driven back onto axi_intf wire each cycle).
struct SlaveOutputs {
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
    std::array<uint8_t, SLAVE_AXI_DATA_BYTES> rdata;
    uint8_t rresp;  // 2-bit
    bool rlast;
};

}  // namespace ni::cmodel::wrap
