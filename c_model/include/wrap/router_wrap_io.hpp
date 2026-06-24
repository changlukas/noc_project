// Per-node Router wrap IO — one node's NoC pin bundle.
//
// A single cosim RouterWrap owns ONE node's REQ+RSP routers at
// coordinate (x,0). Its pins split into three groups:
//   - NMU/NSU-facing (NI edge): FlooNoC pulse-credit, identical mechanism to the
//     LINK (R2) — req_in (NMU injects a request), rsp_in (NSU injects a response),
//     req_out (request ejected toward NSU), rsp_out (response ejected toward NMU).
//     *_out_credit_return is a single-cycle PULSE (router LOCAL input drained ->
//     credit to NMU/NSU); *_in_credit_return is the NMU/NSU's returned pulse
//     (consumed an ejected flit -> router.receive_credit(LOCAL)). FlitBytes
//     reused unchanged; only the credit-bit semantics changed (level -> pulse).
//   - LINK (per network req/rsp): the cross-DPI FlooNoC pulse-credit inter-router
//     link toward the neighbor node. Naming mirrors the plan's explicit link
//     ports on router_wrap:
//       link_<N>_out_valid / link_<N>_out_flit : this node's LINK output (-> neighbor)
//       link_<N>_out_credit                    : credit pulse IN from neighbor for
//                                                our sent flits (-> receive_credit(LINK))
//       link_<N>_in_valid  / link_<N>_in_flit  : neighbor's flit IN (-> input(LINK))
//       link_<N>_in_credit                     : credit pulse OUT we return when our
//                                                LINK input drains (LinkCreditOut.take)
//     credit pulses are single-cycle (per VC), NOT the level used on NI bundles.
#pragma once
#include "wrap/channel_model_wrap_io.hpp"  // FlitBytes
#include "router/router.hpp"               // ROUTER_PORT_COUNT
#include "ni_flit_constants.h"             // ni::header::VC_ID_WIDTH
#include <array>
#include <cstdint>

namespace ni::cmodel::wrap {

// Per-(direction,VC) credit marshalling sizes (DPI ABI is fixed at the max so
// Task 7 only fills more directions — it never re-shapes the struct).
//   ROUTER_LINK_PORTS  : router has 5 ports (LOCAL + N/E/S/W). LINK indices use
//                        the 4 non-LOCAL directions; the array is sized for all 5
//                        so RouterPort enum values index it directly (LOCAL slot
//                        unused on the LINK face).
//   ROUTER_NUM_VC_MAX  : credit vectors carry up to 2^VC_ID_WIDTH VCs; only the
//                        low num_vc entries are live.
inline constexpr std::size_t ROUTER_LINK_PORTS = router::ROUTER_PORT_COUNT;
inline constexpr std::size_t ROUTER_NUM_VC_MAX = 1u << ni::header::VC_ID_WIDTH;

// Per-VC credit pulse vector: bit/entry vc set => one credit pulse on VC vc this
// cycle. Sized to the max; only [0 .. num_vc) are meaningful.
using VcCreditVec = std::array<bool, ROUTER_NUM_VC_MAX>;

struct RouterInputs {
    // --- NMU/NSU-facing (NI edge, FlooNoC pulse credit) ---
    bool req_in_valid;  // NMU injects a request
    FlitBytes req_in_flit;
    VcCreditVec req_in_credit_return;  // PULSE/VC: NMU consumed an ejected rsp flit ->
                                       // router.receive_credit(LOCAL, vc) (router->NI dir)

    bool rsp_in_valid;  // NSU injects a response
    FlitBytes rsp_in_flit;
    VcCreditVec rsp_in_credit_return;  // PULSE/VC: NSU consumed an ejected req flit ->
                                       // router.receive_credit(LOCAL, vc)

    // --- LINK: REQ network (neighbor -> this node, pulse credit), per direction ---
    std::array<VcCreditVec, ROUTER_LINK_PORTS> link_req_out_credit;  // credit/VC from neighbor
                                                                     // for our sent REQ flits
    std::array<bool, ROUTER_LINK_PORTS> link_req_in_valid;           // neighbor's REQ flit in
    std::array<FlitBytes, ROUTER_LINK_PORTS> link_req_in_flit;

    // --- LINK: RSP network (neighbor -> this node, pulse credit), per direction ---
    std::array<VcCreditVec, ROUTER_LINK_PORTS> link_rsp_out_credit;
    std::array<bool, ROUTER_LINK_PORTS> link_rsp_in_valid;
    std::array<FlitBytes, ROUTER_LINK_PORTS> link_rsp_in_flit;
};

struct RouterOutputs {
    // --- NMU/NSU-facing (NI edge, FlooNoC pulse credit) ---
    bool req_out_valid;  // request ejected toward local NSU
    FlitBytes req_out_flit;
    VcCreditVec req_out_credit_return;  // PULSE/VC: router LOCAL input drained an NMU req
                                        // -> one credit back to the NMU (NI->router dir)

    bool rsp_out_valid;  // response ejected toward local NMU
    FlitBytes rsp_out_flit;
    VcCreditVec rsp_out_credit_return;  // PULSE/VC: router LOCAL input drained an NSU rsp
                                        // -> one credit back to the NSU

    // --- LINK: REQ network (this node -> neighbor, pulse credit), per direction ---
    std::array<bool, ROUTER_LINK_PORTS> link_req_out_valid;  // our LINK REQ output toward neighbor
    std::array<FlitBytes, ROUTER_LINK_PORTS> link_req_out_flit;
    std::array<VcCreditVec, ROUTER_LINK_PORTS> link_req_in_credit;  // credit/VC we return when our
                                                                    // REQ LINK input drains

    // --- LINK: RSP network (this node -> neighbor, pulse credit), per direction ---
    std::array<bool, ROUTER_LINK_PORTS> link_rsp_out_valid;
    std::array<FlitBytes, ROUTER_LINK_PORTS> link_rsp_out_flit;
    std::array<VcCreditVec, ROUTER_LINK_PORTS> link_rsp_in_credit;
};

}  // namespace ni::cmodel::wrap
