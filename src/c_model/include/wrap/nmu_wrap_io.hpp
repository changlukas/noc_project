// NmuWrap IO POD structs — combined AXI slave + three-physical-network pin bundle.
//
// NmuInputs: signals consumed by the Nmu each cycle.
//   AXI slave side: master drives AW/W/AR onto axi_intf; Nmu accepts them.
//   REQ face (egress, ready/valid): tx_req_ready — downstream's readiness.
//   RSP face (ingress, ready/valid): rx_rsp_valid/flit — flit arriving.
//   DAT face (both directions, credit): rx_dat_valid/flit (ingress) +
//     tx_dat_crdvalid (egress credit pulse/VC from downstream).
//
// NmuOutputs: signals driven by the Nmu each cycle.
//   AXI slave side: Nmu drives awready/wready/arready handshake + B/R channels.
//   REQ face (egress): tx_req_valid/flit.
//   RSP face (ingress): rx_rsp_ready — this node's readiness, tied constant
//     true (the c_model's ingress queue is unbounded; see nmu_wrap.hpp).
//   DAT face: tx_dat_valid/flit (egress) + rx_dat_crdvalid (ingress credit
//     pulse/VC returned upstream).
//
// Naming, node's own view (spec §4.3, S3a T5 mechanical rename): tx_* = this
// node's transmit side (`noc_req_*` before this stage), rx_* = receive side
// (`noc_rsp_*` before this stage).
//
// FLIT_BYTES = 79 (ni::FLIT_WIDTH = 629 bits, rounded to bytes; stays the max
// over networks, per-network widths bite only at the DPI/SV wire).
// AXI_DATA_BYTES = 64 (512-bit data bus).
// All multi-byte fields are byte-array little-endian, matching DPI wire packing.
#pragma once
#include "axi/types.hpp"        // axi::DATA_BYTES
#include "wrap/flit_bytes.hpp"  // FlitBytes, FLIT_BYTES
#include "ni_flit_constants.h"  // ni::header::VC_ID_WIDTH
#include <array>
#include <cstdint>

namespace ni::cmodel::wrap {

// Per-VC credit pulse vector (bit/entry vc = one credit pulse on VC vc). Sized to
// the max VC count; only [0 .. dat_num_vc) live. Mirrors router_wrap_io VcCreditVec.
inline constexpr std::size_t NMU_NUM_VC_MAX = 1u << ni::header::VC_ID_WIDTH;
using NmuVcCreditVec = std::array<bool, NMU_NUM_VC_MAX>;

// 512-bit data bus = 64 bytes. Aliased from axi::DATA_BYTES so the DPI/SV
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
    uint64_t wstrb;
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

    // REQ face (egress, ready/valid): downstream's readiness for this node's
    // transmit side.
    bool tx_req_ready;

    // RSP face (ingress, ready/valid): flit arriving from the router.
    bool rx_rsp_valid;
    FlitBytes rx_rsp_flit;

    // DAT face ingress (credit): flit arriving from the router.
    bool rx_dat_valid;
    FlitBytes rx_dat_flit;
    // DAT face egress (credit): credit pulse/VC from downstream for flits we sent.
    NmuVcCreditVec tx_dat_crdvalid;
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

    // REQ face (egress, ready/valid): this node's transmit flit.
    bool tx_req_valid;
    FlitBytes tx_req_flit;

    // RSP face (ingress, ready/valid): this node's readiness, returned to the
    // router. Tied constant true — see nmu_wrap.hpp.
    bool rx_rsp_ready;

    // DAT face egress (credit): this node's transmit flit.
    bool tx_dat_valid;
    FlitBytes tx_dat_flit;
    // DAT face ingress (credit): credit pulse/VC this node returns upstream.
    NmuVcCreditVec rx_dat_crdvalid;
};

}  // namespace ni::cmodel::wrap
