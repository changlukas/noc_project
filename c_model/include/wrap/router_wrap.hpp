// RouterWrap — per-node Wrap wrapping ONE node's router subsystem.
//
// Owns this node's REQ router + RSP router at coordinate (x,0) (2-node 2x1 mesh:
// mesh_x_dim=2, mesh_y_dim=1) plus their LOCAL adapters (NI edge) and LINK
// adapters (cross-DPI FlooNoC pulse-credit link toward the neighbor). Two of
// these wired in tb_top replace the bundled RouterChannel.
//
// Per node BOTH the LOCAL (NI edge) and LINK ports now use identical FlooNoC
// pulse-credit wiring (R2: the NI edge no longer uses the level-credit stub).
// LOCAL (NMU/NSU edge) and LINK (cross-DPI inter-router) per network:
//   set_downstream(port, LinkEjectAdapter)   — router output -> SV transport
//     buffer (no pop credit; credit returns over SV as a pulse).
//   set_upstream_credit(port, LinkCreditOut) — captures the router's input-drain
//     pulses; the wrap drains one/tick onto the *_credit/*_credit_return wire.
//   inbound flit pushes straight into router.input(port).
//   each inbound credit pulse calls router.receive_credit(port, 0).
// LOCAL pin mapping (NI edge):
//   inbound  : in_.req_in / in_.rsp_in            -> router.input(LOCAL).push_flit
//   eject    : req_local_eject_ / rsp_local_eject_ -> out_.req_out / out_.rsp_out
//   out cred : req_local_credit_/rsp_local_credit_.take(0) -> out_.*_out_credit_return
//              (PULSE: router LOCAL input drained -> credit to NMU/NSU)
//   in  cred : in_.req_in_credit_return / rsp_in_credit_return (NMU/NSU consumed
//              an ejected flit) -> router.receive_credit(LOCAL, 0) (replenishes
//              the router's built-in credit_[LOCAL] sender counter, router->NI dir)
// The LINK port mapping (x_coord==0 -> EAST, x_coord==1 -> WEST, matching
// route_compute: a (1,0)-dst flit leaves node0 EAST, a (0,0)-dst flit leaves
// node1 WEST) is identical, over the link_<N>_* pins toward the neighbor.
//
// num_vc=1 pinned (SV wraps fatal otherwise); LOCAL/LINK depths = kPoCChannelModelDepth.
//
// Reset invariant (construction-is-reset): the wrap holds no SV-driven reset and
// is created (cmodel_router_create) after rst_ni deasserts, so LinkCreditOut
// pending, LinkEjectAdapter queues, and the routers' FIFOs/counters all start
// empty. Mid-sim reset is NOT modeled (consistent with Router's construction-is-
// reset stance); the tb_top reset window precedes all *_create + traffic, so no
// stale pending credit can leak post-reset.
//
// Depth rationale: the SV credit feedback is registered one cycle behind
// (beta-tick), so pin the eject buffers to num_vc*kPoCChannelModelDepth (the
// aggregate router-output credit window) for margin. The NMU/NSU is credit-gated
// by *_out_credit_return / link_*_in_credit, so the router input never overflows.
#pragma once
#include "wrap/flit_byte_conv.hpp"  // flit_from_bytes, flit_to_bytes
#include "wrap/poc_defaults.hpp"    // kPoCChannelModelDepth
#include "wrap/router_wrap_io.hpp"
#include "router/router.hpp"
#include "router/router_adapters.hpp"
#include <memory>
#include <stdexcept>

namespace ni::cmodel::wrap {

class RouterWrap {
  public:
    void init(uint8_t x_coord, uint8_t num_vc = 1) {
        num_vc_ = num_vc;
        link_port_ = (x_coord == 0) ? static_cast<std::size_t>(router::RouterPort::EAST)
                                    : static_cast<std::size_t>(router::RouterPort::WEST);

        router::RouterConfig c;
        c.x = x_coord;
        c.y = 0;
        c.mesh_x_dim = 2;
        c.mesh_y_dim = 1;
        c.num_vc = num_vc;
        c.vc_depth = kPoCChannelModelDepth;
        c.output_fifo_depth = kPoCChannelModelDepth;
        req_router_ = std::make_unique<router::Router>(c);
        rsp_router_ = std::make_unique<router::Router>(c);

        wire_port(*req_router_, LOCAL, req_local_eject_, req_local_credit_);
        wire_port(*rsp_router_, LOCAL, rsp_local_eject_, rsp_local_credit_);
        wire_port(*req_router_, link_port_, req_link_eject_, req_link_credit_);
        wire_port(*rsp_router_, link_port_, rsp_link_eject_, rsp_link_credit_);

        in_ = RouterInputs{};
        out_ = RouterOutputs{};
    }

    void set_inputs(const RouterInputs& in) { in_ = in; }

    void tick() {
        // Step 1: push all inbound flits straight into the router inputs. LOCAL
        // and LINK are now symmetric FlooNoC pulse-credit ports (no InjectAdapter
        // mirror). The single NMU/NSU source sends <=1 LOCAL flit/tick, but the
        // router landing register asserts on a 2nd push/port/cycle (router.hpp),
        // so guard it: exactly one LOCAL push per network per tick.
        if (in_.req_in_valid) {
            req_router_->input(LOCAL).push_flit(flit_from_bytes(in_.req_in_flit));
        }
        if (in_.rsp_in_valid) {
            rsp_router_->input(LOCAL).push_flit(flit_from_bytes(in_.rsp_in_flit));
        }
        if (in_.link_req_in_valid) {
            req_router_->input(link_port_).push_flit(flit_from_bytes(in_.link_req_in_flit));
        }
        if (in_.link_rsp_in_valid) {
            rsp_router_->input(link_port_).push_flit(flit_from_bytes(in_.link_rsp_in_flit));
        }
        // LOCAL credit IN: the NMU/NSU returned a pulse for a flit drained from
        // the router's LOCAL OUTPUT (router->NI direction). Replenish the
        // router's built-in credit_[LOCAL] sender counter.
        if (in_.req_in_credit_return) req_router_->receive_credit(LOCAL, 0);
        if (in_.rsp_in_credit_return) rsp_router_->receive_credit(LOCAL, 0);
        // Neighbor credit pulses for flits we previously sent over the LINK.
        if (in_.link_req_out_credit) req_router_->receive_credit(link_port_, 0);
        if (in_.link_rsp_out_credit) rsp_router_->receive_credit(link_port_, 0);

        // Step 2: advance both routers one stage.
        req_router_->tick();
        rsp_router_->tick();

        // Step 3: sample outputs.
        out_ = RouterOutputs{};
        if (auto f = req_local_eject_->pop_flit()) {
            out_.req_out_valid = true;
            out_.req_out_flit = flit_to_bytes(*f);
        }
        if (auto f = rsp_local_eject_->pop_flit()) {
            out_.rsp_out_valid = true;
            out_.rsp_out_flit = flit_to_bytes(*f);
        }
        // LOCAL credit OUT: PULSE — the router's LOCAL INPUT drained a flit from
        // the NMU/NSU, so return one credit to the NMU/NSU (NI->router direction).
        out_.req_out_credit_return = req_local_credit_->take(0);
        out_.rsp_out_credit_return = rsp_local_credit_->take(0);

        if (auto f = req_link_eject_->pop_flit()) {
            out_.link_req_out_valid = true;
            out_.link_req_out_flit = flit_to_bytes(*f);
        }
        if (auto f = rsp_link_eject_->pop_flit()) {
            out_.link_rsp_out_valid = true;
            out_.link_rsp_out_flit = flit_to_bytes(*f);
        }
        out_.link_req_in_credit = req_link_credit_->take(0);
        out_.link_rsp_in_credit = rsp_link_credit_->take(0);
    }

    void get_outputs(RouterOutputs& out) const { out = out_; }

    // Test introspection: the REQ router, so a test can read its built-in
    // credit_[LOCAL] sender counter (router->NI direction) across a
    // credit-return hop. Not used in production wiring.
    router::Router& req_router() { return *req_router_; }
    // RSP router accessor — used by cmodel_perf_sample_tick to sample occupancy.
    router::Router& rsp_router() { return *rsp_router_; }

  private:
    static constexpr std::size_t LOCAL = static_cast<std::size_t>(router::RouterPort::LOCAL);

    // FlooNoC pulse-credit wiring, identical for LOCAL and LINK: transport-only
    // eject (no pop credit) + a LinkCreditOut that captures the router's
    // input-drain pulses for the wrap to drain one/tick onto the credit wire.
    void wire_port(router::Router& r, std::size_t port,
                   std::unique_ptr<router::LinkEjectAdapter>& ej,
                   std::unique_ptr<router::LinkCreditOut>& credit) {
        ej = std::make_unique<router::LinkEjectAdapter>(static_cast<std::size_t>(num_vc_) *
                                                        kPoCChannelModelDepth);
        credit = std::make_unique<router::LinkCreditOut>(num_vc_);
        r.set_downstream(port, *ej);
        r.set_upstream_credit(port, *credit);
    }

    uint8_t num_vc_ = 1;
    std::size_t link_port_ = static_cast<std::size_t>(router::RouterPort::EAST);

    std::unique_ptr<router::Router> req_router_, rsp_router_;
    std::unique_ptr<router::LinkEjectAdapter> req_local_eject_, rsp_local_eject_;
    std::unique_ptr<router::LinkCreditOut> req_local_credit_, rsp_local_credit_;
    std::unique_ptr<router::LinkEjectAdapter> req_link_eject_, rsp_link_eject_;
    std::unique_ptr<router::LinkCreditOut> req_link_credit_, rsp_link_credit_;

    RouterInputs in_{};
    RouterOutputs out_{};
};

}  // namespace ni::cmodel::wrap
