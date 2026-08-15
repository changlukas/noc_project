// NsuWrap IO POD structs — combined three-physical-network + AXI master pin bundle.
//
// NsuInputs: signals consumed by the Nsu each cycle.
//   REQ face (ingress, ready/valid): rx_req_valid/flit — flit arriving.
//   RSP face (egress, ready/valid): tx_rsp_ready — downstream's readiness.
//   DAT face (both directions, credit): rx_dat_valid/flit (ingress) +
//     tx_dat_crdvalid (egress credit pulse/VC from downstream).
//   AXI master side: ready signals + B/R beats returning from the AXI slave.
//
// NsuOutputs: signals driven by the Nsu each cycle.
//   REQ face (ingress): rx_req_ready — this node's readiness, tied constant
//     true (the c_model's ingress queue is unbounded; see nsu_wrap.hpp).
//   RSP face (egress): tx_rsp_valid/flit.
//   DAT face: tx_dat_valid/flit (egress) + rx_dat_crdvalid (ingress credit
//     pulse/VC returned upstream).
//   AXI master side: Nsu drives AW/W/AR to slave; accepts bready/rready from it.
//
// Naming, node's own view (spec §4.3, S3a T5 mechanical rename): tx_* = this
// node's transmit side (`noc_rsp_*` before this stage), rx_* = receive side
// (`noc_req_*` before this stage).
//
// FLIT_BYTES = 80 (ni::FLIT_WIDTH = 633 bits, rounded to bytes; stays the max
// over networks, per-network widths bite only at the DPI/SV wire).
// AXI_DATA_BYTES = 64 (512-bit data bus).
// All multi-byte fields are byte-array little-endian, matching DPI wire packing.
//
// Direction inversion vs. NmuWrapIo:
//   Nmu transmits REQ / receives RSP  -> Nsu receives REQ / transmits RSP.
//   Nmu has axi_intf.slave (accepts AW/W/AR) -> Nsu has axi_intf.master (drives AW/W/AR).
#pragma once
#include "axi/types.hpp"        // axi::DATA_BYTES
#include "wrap/flit_bytes.hpp"  // FlitBytes, FLIT_BYTES
#include "ni_flit_constants.h"  // ni::header::VC_ID_WIDTH
#include <array>
#include <cstdint>

namespace ni::cmodel::wrap {

// Per-VC credit pulse vector (bit/entry vc = one credit pulse on VC vc). Sized to
// the max VC count; only [0 .. dat_num_vc) live. Mirrors router_wrap_io VcCreditVec.
inline constexpr std::size_t NSU_NUM_VC_MAX = 1u << ni::header::VC_ID_WIDTH;
using NsuVcCreditVec = std::array<bool, NSU_NUM_VC_MAX>;

// 512-bit data bus = 64 bytes. Aliased from axi::DATA_BYTES so the DPI/SV
// wire width agrees across master/slave/nmu/nsu wrap IO structs.
constexpr int NSU_AXI_DATA_BYTES = axi::DATA_BYTES;

// NsuInputs: signals consumed by Nsu each cycle.
struct NsuInputs {
    // REQ face (ingress, ready/valid): flit arriving from the router.
    bool rx_req_valid;
    FlitBytes rx_req_flit;

    // RSP face (egress, ready/valid): downstream's readiness for this node's
    // transmit side.
    bool tx_rsp_ready;

    // DAT face ingress (credit): flit arriving from the router.
    bool rx_dat_valid;
    FlitBytes rx_dat_flit;
    // DAT face egress (credit): credit pulse/VC from downstream for flits we sent.
    NsuVcCreditVec tx_dat_crdvalid;

    // AXI master side — AW channel (slave drives ready)
    bool awready;
    // AXI master side — W channel (slave drives ready)
    bool wready;
    // AXI master side — B channel (slave drives write response)
    bool bvalid;
    uint8_t bid;
    uint8_t bresp;  // 2-bit AXI4 response code in low 2 bits
    // AXI master side — AR channel (slave drives ready)
    bool arready;
    // AXI master side — R channel (slave drives read data)
    bool rvalid;
    uint8_t rid;
    std::array<uint8_t, NSU_AXI_DATA_BYTES> rdata;
    uint8_t rresp;  // 2-bit
    bool rlast;
};

// NsuOutputs: signals driven by Nsu each cycle.
struct NsuOutputs {
    // REQ face (ingress, ready/valid): this node's readiness, returned to
    // the router. Tied constant true — see nsu_wrap.hpp.
    bool rx_req_ready;

    // RSP face (egress, ready/valid): this node's transmit flit.
    bool tx_rsp_valid;
    FlitBytes tx_rsp_flit;

    // DAT face egress (credit): this node's transmit flit.
    bool tx_dat_valid;
    FlitBytes tx_dat_flit;
    // DAT face ingress (credit): credit pulse/VC this node returns upstream.
    NsuVcCreditVec rx_dat_crdvalid;

    // AXI master side — AW channel (Nsu drives write address to slave)
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
    // AXI master side — W channel (Nsu drives write data to slave)
    bool wvalid;
    std::array<uint8_t, NSU_AXI_DATA_BYTES> wdata;
    uint64_t wstrb;
    bool wlast;
    // AXI master side — B channel (Nsu accepts write response from slave)
    bool bready;
    // AXI master side — AR channel (Nsu drives read address to slave)
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
    // AXI master side — R channel (Nsu accepts read data from slave)
    bool rready;
};

}  // namespace ni::cmodel::wrap
