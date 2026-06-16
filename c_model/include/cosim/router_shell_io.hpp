// Per-node Router shell IO — one node's NoC pin bundle.
//
// A single cosim RouterShellAdapter owns ONE node's REQ+RSP routers at
// coordinate (x,0). Its pins split into three groups:
//   - NMU/NSU-facing (NI edge): identical level/stub-credit semantics to the
//     ChannelModel / RouterChannel shell — req_in (NMU injects a request),
//     rsp_in (NSU injects a response), req_out (request ejected toward NSU),
//     rsp_out (response ejected toward NMU), *_credit_return as in ChannelModel.
//     This task does NOT change NI-edge credit (FlitBytes reused unchanged).
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
#include "cosim/channel_model_shell_io.hpp"  // FlitBytes
#include <cstdint>

namespace ni::cmodel::cosim {

struct RouterInputs {
    // --- NMU/NSU-facing (NI edge, level/stub credit — unchanged from today) ---
    bool req_in_valid;  // NMU injects a request
    FlitBytes req_in_flit;
    bool req_in_credit_return;  // accepted but unused (shell drives credit via *_out)

    bool rsp_in_valid;  // NSU injects a response
    FlitBytes rsp_in_flit;
    bool rsp_in_credit_return;

    // --- LINK: REQ network (neighbor -> this node, pulse credit) ---
    bool link_req_out_credit;  // credit pulse from neighbor for our sent REQ flits
    bool link_req_in_valid;    // neighbor's REQ flit entering our LINK input
    FlitBytes link_req_in_flit;

    // --- LINK: RSP network (neighbor -> this node, pulse credit) ---
    bool link_rsp_out_credit;  // credit pulse from neighbor for our sent RSP flits
    bool link_rsp_in_valid;    // neighbor's RSP flit entering our LINK input
    FlitBytes link_rsp_in_flit;
};

struct RouterOutputs {
    // --- NMU/NSU-facing (NI edge, level/stub credit — unchanged from today) ---
    bool req_out_valid;  // request ejected toward local NSU
    FlitBytes req_out_flit;
    bool req_out_credit_return;  // inject-side credit_avail level (NMU gate)

    bool rsp_out_valid;  // response ejected toward local NMU
    FlitBytes rsp_out_flit;
    bool rsp_out_credit_return;  // inject-side credit_avail level (NSU gate)

    // --- LINK: REQ network (this node -> neighbor, pulse credit) ---
    bool link_req_out_valid;  // our LINK REQ output flit toward neighbor
    FlitBytes link_req_out_flit;
    bool link_req_in_credit;  // credit pulse we return when our REQ LINK input drains

    // --- LINK: RSP network (this node -> neighbor, pulse credit) ---
    bool link_rsp_out_valid;  // our LINK RSP output flit toward neighbor
    FlitBytes link_rsp_out_flit;
    bool link_rsp_in_credit;  // credit pulse we return when our RSP LINK input drains
};

}  // namespace ni::cmodel::cosim
