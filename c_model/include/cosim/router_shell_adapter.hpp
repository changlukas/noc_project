// RouterShellAdapter — per-node ShellAdapter wrapping ONE node's router subsystem.
//
// Owns this node's REQ router + RSP router at coordinate (x,0) (2-node 2x1 mesh:
// mesh_x_dim=2, mesh_y_dim=1) plus their LOCAL adapters (NI edge) and LINK
// adapters (cross-DPI FlooNoC pulse-credit link toward the neighbor). Two of
// these wired in tb_top replace the bundled RouterChannel.
//
// Per node the LOCAL wiring mirrors RouterChannel::wire_local exactly:
//   req router LOCAL: InjectAdapter (NMU req in)  + EjectAdapter (NSU req out)
//   rsp router LOCAL: InjectAdapter (NSU rsp in)  + EjectAdapter (NMU rsp out)
//   *_out_credit_return = inject-side credit_avail(0) level (NI edge stub —
//   unchanged from RouterChannelShellAdapter; the SV-provided *_in_credit_return
//   inputs are accepted but unused).
// The LINK port is the only new wiring (x_coord==0 -> EAST, x_coord==1 -> WEST,
// matching route_compute: a (1,0)-dst flit leaves node0 EAST, a (0,0)-dst flit
// leaves node1 WEST). Per network:
//   set_downstream(LINK, LinkEjectAdapter)   — router LINK output -> SV transport
//     buffer (no pop credit; credit returns over SV from the neighbor).
//   set_upstream_credit(LINK, LinkCreditOut) — captures the router's LINK
//     input-drain pulses; the shell drains one/tick onto link_<N>_in_credit.
//   neighbor link-in flit pushes straight into router.input(LINK).
//   each neighbor link-out credit pulse calls router.receive_credit(LINK, 0).
//
// num_vc=1 pinned (SV wraps fatal otherwise); LOCAL/LINK depths = kPoCChannelModelDepth.
//
// Reset invariant (construction-is-reset): the shell holds no SV-driven reset and
// is created (cmodel_router_create) after rst_ni deasserts, so LinkCreditOut
// pending, LinkEjectAdapter queues, and the routers' FIFOs/counters all start
// empty. Mid-sim reset is NOT modeled (consistent with Router's construction-is-
// reset stance); the tb_top reset window precedes all *_create + traffic, so no
// stale pending credit can leak post-reset.
//
// Depth/throw rationale matches RouterChannelShellAdapter: the SV credit feedback
// is registered one cycle behind (beta-tick), so pin depths to kPoCChannelModelDepth
// for margin and throw if a NI push is ever rejected (the NMU/NSU is credit-gated
// via *_out_credit_return, so a reject means a real bug, not backpressure).
#pragma once
#include "cosim/flit_byte_conv.hpp"  // flit_from_bytes, flit_to_bytes
#include "cosim/poc_defaults.hpp"    // kPoCChannelModelDepth
#include "cosim/router_shell_io.hpp"
#include "noc/router.hpp"
#include "noc/router_adapters.hpp"
#include <memory>
#include <stdexcept>

namespace ni::cmodel::cosim {

class RouterShellAdapter {
  public:
    void init(uint8_t x_coord, uint8_t num_vc = 1) {
        num_vc_ = num_vc;
        link_port_ = (x_coord == 0) ? static_cast<std::size_t>(noc::RouterPort::EAST)
                                    : static_cast<std::size_t>(noc::RouterPort::WEST);

        noc::RouterConfig c;
        c.x = x_coord;
        c.y = 0;
        c.mesh_x_dim = 2;
        c.mesh_y_dim = 1;
        c.num_vc = num_vc;
        c.vc_depth = kPoCChannelModelDepth;
        c.output_fifo_depth = kPoCChannelModelDepth;
        req_router_ = std::make_unique<noc::Router>(c);
        rsp_router_ = std::make_unique<noc::Router>(c);

        wire_local(*req_router_, req_inject_, req_eject_);
        wire_local(*rsp_router_, rsp_inject_, rsp_eject_);
        wire_link(*req_router_, req_link_eject_, req_link_credit_);
        wire_link(*rsp_router_, rsp_link_eject_, rsp_link_credit_);

        in_ = RouterInputs{};
        out_ = RouterOutputs{};
    }

    void set_inputs(const RouterInputs& in) { in_ = in; }

    void tick() {
        // Step 1: reset inject landing guards; push all inbound flits.
        req_inject_->on_tick();
        rsp_inject_->on_tick();

        if (in_.req_in_valid && !req_inject_->push_flit(flit_from_bytes(in_.req_in_flit))) {
            throw std::runtime_error(
                "RouterShellAdapter: NMU req push rejected (credit discipline violated)");
        }
        if (in_.rsp_in_valid && !rsp_inject_->push_flit(flit_from_bytes(in_.rsp_in_flit))) {
            throw std::runtime_error(
                "RouterShellAdapter: NSU rsp push rejected (credit discipline violated)");
        }
        if (in_.link_req_in_valid) {
            req_router_->input(link_port_).push_flit(flit_from_bytes(in_.link_req_in_flit));
        }
        if (in_.link_rsp_in_valid) {
            rsp_router_->input(link_port_).push_flit(flit_from_bytes(in_.link_rsp_in_flit));
        }
        // Neighbor credit pulses for flits we previously sent over the LINK.
        if (in_.link_req_out_credit) req_router_->receive_credit(link_port_, 0);
        if (in_.link_rsp_out_credit) rsp_router_->receive_credit(link_port_, 0);

        // Step 2: advance both routers one stage.
        req_router_->tick();
        rsp_router_->tick();

        // Step 3: sample outputs.
        out_ = RouterOutputs{};
        if (auto f = req_eject_->pop_flit()) {
            out_.req_out_valid = true;
            out_.req_out_flit = flit_to_bytes(*f);
        }
        if (auto f = rsp_eject_->pop_flit()) {
            out_.rsp_out_valid = true;
            out_.rsp_out_flit = flit_to_bytes(*f);
        }
        out_.req_out_credit_return = req_inject_->credit_avail(0);
        out_.rsp_out_credit_return = rsp_inject_->credit_avail(0);

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

  private:
    static constexpr std::size_t LOCAL = static_cast<std::size_t>(noc::RouterPort::LOCAL);

    void wire_local(noc::Router& r, std::unique_ptr<noc::InjectAdapter>& inj,
                    std::unique_ptr<noc::EjectAdapter>& ej) {
        inj = std::make_unique<noc::InjectAdapter>(r, LOCAL, num_vc_, kPoCChannelModelDepth);
        ej = std::make_unique<noc::EjectAdapter>(
            r, LOCAL, static_cast<std::size_t>(num_vc_) * kPoCChannelModelDepth);
        r.set_upstream_credit(LOCAL, *inj);
        r.set_downstream(LOCAL, *ej);
    }

    void wire_link(noc::Router& r, std::unique_ptr<noc::LinkEjectAdapter>& ej,
                   std::unique_ptr<noc::LinkCreditOut>& credit) {
        ej = std::make_unique<noc::LinkEjectAdapter>(static_cast<std::size_t>(num_vc_) *
                                                     kPoCChannelModelDepth);
        credit = std::make_unique<noc::LinkCreditOut>(num_vc_);
        r.set_downstream(link_port_, *ej);
        r.set_upstream_credit(link_port_, *credit);
    }

    uint8_t num_vc_ = 1;
    std::size_t link_port_ = static_cast<std::size_t>(noc::RouterPort::EAST);

    std::unique_ptr<noc::Router> req_router_, rsp_router_;
    std::unique_ptr<noc::InjectAdapter> req_inject_, rsp_inject_;
    std::unique_ptr<noc::EjectAdapter> req_eject_, rsp_eject_;
    std::unique_ptr<noc::LinkEjectAdapter> req_link_eject_, rsp_link_eject_;
    std::unique_ptr<noc::LinkCreditOut> req_link_credit_, rsp_link_credit_;

    RouterInputs in_{};
    RouterOutputs out_{};
};

}  // namespace ni::cmodel::cosim
