// RouterWrap — per-node Wrap wrapping ONE node's three physical-network routers.
//
// Owns this node's REQ router + RSP router (both router::SimpleRouter,
// ready/valid, single VC) and DAT router (router::Router, credit) at
// (x_coord, y_coord) in an NxM mesh. Every network is wired uniformly across
// the same 5-port space (LOCAL + N/E/S/W); boundary directions with no
// neighbor simply never see valid asserted (the fabric ties them to 0 and
// asserts on a violation) -- RouterWrap does not special-case them.
//
// Two wiring shapes, no common base (S3a stage design §7 -- SimpleRouter and
// Router are unrelated classes):
//   REQ/RSP (SimpleRouter): SimpleRouterWireLink per output port marshals
//     ready/valid across the DPI/SV boundary. ready() is a live signal set
//     from the DPI-sampled tx_<net>_ready wire BEFORE tick(); push_flit()
//     (called only when ready() was true) stashes the grant, drained AFTER
//     tick() onto tx_<net>_valid/flit. rx_<net>_ready is the router's own
//     ready(port,0), read AFTER tick() so it reflects this cycle's push.
//   DAT (Router): unchanged FlooNoC pulse-credit LinkEjectAdapter/
//     LinkCreditOut pattern, now applied uniformly to all 5 ports (LOCAL
//     included) instead of LOCAL-special-cased + LINK-looped.
//
// Registered-DPI-tick discipline (shared by the NI wraps nmu_wrap/nsu_wrap):
// on every posedge clk_i the module samples the PREVIOUS cycle's registered
// wire inputs, pushes them to the C++ model via DPI set_inputs, advances the
// model via tick, pulls outputs via get_outputs, and registers those outputs
// nonblocking so they are visible to SV wires from the NEXT cycle onward.
//
// Reset invariant (construction-is-reset): the wrap holds no SV-driven reset
// and is created (cmodel_router_create) after rst_ni deasserts, so every
// adapter's queues/counters start empty. Mid-sim reset is NOT modeled
// (consistent with Router's/SimpleRouter's construction-is-reset stance); the
// tb_top reset window precedes all *_create + traffic, so no stale state can
// leak post-reset.
//
// Depth rationale (DAT): vc_depth = NOC_ROUTER_VC_DEPTH (spec default; also
// the value the NMU/NSU DAT face seeds its own sender credit counter with, so
// both ends of the link agree on the credit window). The eject buffers are
// sized to num_vc * vc_depth (aggregate output-credit window).
#pragma once
#include "wrap/flit_byte_conv.hpp"  // flit_from_bytes, flit_to_bytes
#include "wrap/router_wrap_io.hpp"
#include "router/router.hpp"
#include "router/simple_router.hpp"
#include "router/router_adapters.hpp"
#include "ni_params.h"  // NOC_ROUTER_VC_DEPTH, NOC_ROUTER_OUTPUT_FIFO_DEPTH
#include <array>
#include <memory>

namespace ni::cmodel::wrap {

class RouterWrap {
  public:
    void init(uint8_t x_coord, uint8_t y_coord = 0, uint8_t mesh_x_dim = 2, uint8_t mesh_y_dim = 1,
              uint8_t dat_num_vc = 1) {
        dat_num_vc_ = dat_num_vc;

        router::SimpleRouterConfig sc;
        sc.x = x_coord;
        sc.y = y_coord;
        sc.mesh_x_dim = mesh_x_dim;
        sc.mesh_y_dim = mesh_y_dim;
        sc.num_vc = 1;  // S1 Q2: REQ/RSP are ratified single-VC networks.
        req_router_ = std::make_unique<router::SimpleRouter>(sc);
        rsp_router_ = std::make_unique<router::SimpleRouter>(sc);
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            req_router_->set_downstream(p, req_link_[p]);
            rsp_router_->set_downstream(p, rsp_link_[p]);
        }

        router::RouterConfig dc;
        dc.x = x_coord;
        dc.y = y_coord;
        dc.mesh_x_dim = mesh_x_dim;
        dc.mesh_y_dim = mesh_y_dim;
        dc.num_vc = dat_num_vc;
        dc.vc_depth = static_cast<std::size_t>(::ni::NOC_ROUTER_VC_DEPTH);
        dc.output_fifo_depth = static_cast<std::size_t>(::ni::NOC_ROUTER_OUTPUT_FIFO_DEPTH);
        dat_router_ = std::make_unique<router::Router>(dc);
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            dat_eject_[p] = std::make_unique<router::LinkEjectAdapter>(
                static_cast<std::size_t>(dat_num_vc_) *
                static_cast<std::size_t>(::ni::NOC_ROUTER_VC_DEPTH));
            dat_credit_[p] = std::make_unique<router::LinkCreditOut>(dat_num_vc_);
            dat_router_->set_downstream(p, *dat_eject_[p]);
            dat_router_->set_upstream_credit(p, *dat_credit_[p]);
        }

        in_ = RouterInputs{};
        out_ = RouterOutputs{};
    }

    void set_inputs(const RouterInputs& in) { in_ = in; }

    // Per-network granular accessors (S3a T5 DPI split — see cmodel_dpi.cpp's
    // cmodel_router_{req,rsp,dat}_{set_inputs,get_outputs}). REQ/RSP/DAT have
    // three DIFFERENT flit widths; a single DPI call marshalling all three
    // networks' unpacked flit arrays together was the only place in this
    // codebase mixing more than one parameterized element width in one DPI
    // signature. Splitting per network removes that construct outright
    // instead of trying to prove which specific Verilator marshalling
    // assumption broke on it.
    void set_req_inputs(const PerPort<bool>& rx_valid, const PerPort<FlitBytes>& rx_flit,
                        const PerPort<bool>& tx_ready) {
        in_.rx_req_valid = rx_valid;
        in_.rx_req_flit = rx_flit;
        in_.tx_req_ready = tx_ready;
    }
    void set_rsp_inputs(const PerPort<bool>& rx_valid, const PerPort<FlitBytes>& rx_flit,
                        const PerPort<bool>& tx_ready) {
        in_.rx_rsp_valid = rx_valid;
        in_.rx_rsp_flit = rx_flit;
        in_.tx_rsp_ready = tx_ready;
    }
    void set_dat_inputs(const PerPort<bool>& rx_valid, const PerPort<FlitBytes>& rx_flit,
                        const PerPort<VcCreditVec>& tx_crdvalid) {
        in_.rx_dat_valid = rx_valid;
        in_.rx_dat_flit = rx_flit;
        in_.tx_dat_crdvalid = tx_crdvalid;
    }
    void get_req_outputs(PerPort<bool>& tx_valid, PerPort<FlitBytes>& tx_flit,
                         PerPort<bool>& rx_ready) const {
        tx_valid = out_.tx_req_valid;
        tx_flit = out_.tx_req_flit;
        rx_ready = out_.rx_req_ready;
    }
    void get_rsp_outputs(PerPort<bool>& tx_valid, PerPort<FlitBytes>& tx_flit,
                         PerPort<bool>& rx_ready) const {
        tx_valid = out_.tx_rsp_valid;
        tx_flit = out_.tx_rsp_flit;
        rx_ready = out_.rx_rsp_ready;
    }
    void get_dat_outputs(PerPort<bool>& tx_valid, PerPort<FlitBytes>& tx_flit,
                         PerPort<VcCreditVec>& rx_crdvalid) const {
        tx_valid = out_.tx_dat_valid;
        tx_flit = out_.tx_dat_flit;
        rx_crdvalid = out_.rx_dat_crdvalid;
    }

    void tick() {
        // Step 1: push inbound flits, set ready-mirror state, before tick().
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            if (in_.rx_req_valid[p])
                req_router_->input(p).push_flit(flit_from_bytes(in_.rx_req_flit[p]));
            if (in_.rx_rsp_valid[p])
                rsp_router_->input(p).push_flit(flit_from_bytes(in_.rx_rsp_flit[p]));
            req_link_[p].set_ready(in_.tx_req_ready[p]);
            rsp_link_[p].set_ready(in_.tx_rsp_ready[p]);

            if (in_.rx_dat_valid[p])
                dat_router_->input(p).push_flit(flit_from_bytes(in_.rx_dat_flit[p]));
            for (uint8_t vc = 0; vc < dat_num_vc_; ++vc) {
                if (in_.tx_dat_crdvalid[p][vc]) dat_router_->receive_credit(p, vc);
            }
        }

        // Step 2: advance all three routers one stage.
        req_router_->tick();
        rsp_router_->tick();
        dat_router_->tick();

        // Step 3: sample outputs.
        out_ = RouterOutputs{};
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            if (auto f = req_link_[p].take()) {
                out_.tx_req_valid[p] = true;
                out_.tx_req_flit[p] = flit_to_bytes(*f);
            }
            out_.rx_req_ready[p] = req_router_->ready(p, 0);

            if (auto f = rsp_link_[p].take()) {
                out_.tx_rsp_valid[p] = true;
                out_.tx_rsp_flit[p] = flit_to_bytes(*f);
            }
            out_.rx_rsp_ready[p] = rsp_router_->ready(p, 0);

            if (auto f = dat_eject_[p]->pop_flit()) {
                out_.tx_dat_valid[p] = true;
                out_.tx_dat_flit[p] = flit_to_bytes(*f);
            }
            for (uint8_t vc = 0; vc < dat_num_vc_; ++vc) {
                out_.rx_dat_crdvalid[p][vc] = dat_credit_[p]->take(vc);
            }
        }
    }

    void get_outputs(RouterOutputs& out) const { out = out_; }

    // DAT VC count — read by the DPI handlers to size the DAT per-VC credit
    // marshalling loops. REQ/RSP are fixed single-VC (no accessor needed).
    uint8_t num_vc() const { return dat_num_vc_; }

    // Test introspection: routers, so a test can read built-in state directly.
    router::SimpleRouter& req_router() { return *req_router_; }
    router::SimpleRouter& rsp_router() { return *rsp_router_; }
    router::Router& dat_router() { return *dat_router_; }

    // Fabric-state-dump introspection (read-only): DAT eject/credit-pending
    // per port. REQ/RSP have no eject buffer (SimpleRouterWireLink stashes at
    // most one in-flight grant) and no pending credit (ready is a live wire).
    std::size_t dat_eject_buffered(std::size_t port) const { return dat_eject_[port]->buffered(); }
    std::size_t dat_credit_pending(std::size_t port, uint8_t vc) const {
        return dat_credit_[port]->pending(vc);
    }

  private:
    uint8_t dat_num_vc_ = 1;

    std::unique_ptr<router::SimpleRouter> req_router_, rsp_router_;
    std::array<router::SimpleRouterWireLink, ROUTER_LINK_PORTS> req_link_, rsp_link_;

    std::unique_ptr<router::Router> dat_router_;
    std::array<std::unique_ptr<router::LinkEjectAdapter>, ROUTER_LINK_PORTS> dat_eject_;
    std::array<std::unique_ptr<router::LinkCreditOut>, ROUTER_LINK_PORTS> dat_credit_;

    RouterInputs in_{};
    RouterOutputs out_{};
};

}  // namespace ni::cmodel::wrap
