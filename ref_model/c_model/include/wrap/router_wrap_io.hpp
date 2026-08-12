// Per-node Router wrap IO — one node's NoC pin bundle, three physical networks.
//
// A single cosim RouterWrap owns ONE node's REQ + RSP + DAT routers at
// (x_coord, y_coord) in an NxM mesh. Every network's pins are ONE uniform
// per-PORT array sized ROUTER_LINK_PORTS (LOCAL=0, N/E/S/W=1..4), spec §4.3 /
// S3a stage design §7's `tx_*[LINK_PORTS]` shape: index LOCAL carries this
// node's own NMU/NSU traffic, indices N/E/S/W carry the inter-router links to
// each existing neighbor (boundary directions simply never see valid asserted
// -- the fabric ties them to 0 and asserts on a violation).
//
// Naming, node's own view (spec §4.3, S3a T5 mechanical rename):
//   tx_<net>_*  : this node's transmit side (`*_out_*` before this stage)
//   rx_<net>_*  : this node's receive side  (`*_in_*` before this stage)
//
// REQ / RSP (SimpleRouter, ready/valid, single VC per S1 Q2): scalar
// ready/valid per port, no credit.
//   tx_<net>_valid/flit  : this node's transmit flit toward port p
//   tx_<net>_ready       : ready FROM the receiver at port p (input)
//   rx_<net>_valid/flit  : flit arriving from port p (input)
//   rx_<net>_ready       : ready this node returns to port p (output)
//
// DAT (credit Router, unchanged flow control): per-VC credit pulse vector,
// mirrors the pre-S3a LINK-only shape now extended uniformly to LOCAL too.
//   tx_dat_valid/flit    : this node's transmit flit toward port p
//   tx_dat_crdvalid      : credit pulse/VC FROM port p for flits we sent there (input)
//   rx_dat_valid/flit    : flit arriving from port p (input)
//   rx_dat_crdvalid      : credit pulse/VC this node returns to port p (output)
#pragma once
#include "wrap/flit_bytes.hpp"  // FlitBytes, FLIT_BYTES
#include "router/router.hpp"    // ROUTER_PORT_COUNT
#include "ni_flit_constants.h"  // ni::header::VC_ID_WIDTH
#include <array>
#include <cstdint>

namespace ni::cmodel::wrap {

// ROUTER_LINK_PORTS: router has 5 ports (LOCAL + N/E/S/W); every network's
// pins are indexed by this same space.
// ROUTER_NUM_VC_MAX: DAT credit vectors carry up to 2^VC_ID_WIDTH VCs; only
// the low dat_num_vc entries are live.
inline constexpr std::size_t ROUTER_LINK_PORTS = router::ROUTER_PORT_COUNT;
inline constexpr std::size_t ROUTER_NUM_VC_MAX = 1u << ni::header::VC_ID_WIDTH;

using VcCreditVec = std::array<bool, ROUTER_NUM_VC_MAX>;

template <typename T>
using PerPort = std::array<T, ROUTER_LINK_PORTS>;

struct RouterInputs {
    // --- REQ network (ready/valid) ---
    PerPort<bool> rx_req_valid;
    PerPort<FlitBytes> rx_req_flit;
    PerPort<bool> tx_req_ready;  // from the receiver at port p

    // --- RSP network (ready/valid) ---
    PerPort<bool> rx_rsp_valid;
    PerPort<FlitBytes> rx_rsp_flit;
    PerPort<bool> tx_rsp_ready;

    // --- DAT network (credit) ---
    PerPort<bool> rx_dat_valid;
    PerPort<FlitBytes> rx_dat_flit;
    PerPort<VcCreditVec> tx_dat_crdvalid;  // credit/VC from port p for flits we sent
};

struct RouterOutputs {
    // --- REQ network (ready/valid) ---
    PerPort<bool> tx_req_valid;
    PerPort<FlitBytes> tx_req_flit;
    PerPort<bool> rx_req_ready;  // this node's readiness, returned to port p

    // --- RSP network (ready/valid) ---
    PerPort<bool> tx_rsp_valid;
    PerPort<FlitBytes> tx_rsp_flit;
    PerPort<bool> rx_rsp_ready;

    // --- DAT network (credit) ---
    PerPort<bool> tx_dat_valid;
    PerPort<FlitBytes> tx_dat_flit;
    PerPort<VcCreditVec> rx_dat_crdvalid;  // credit/VC this node returns to port p
};

}  // namespace ni::cmodel::wrap
