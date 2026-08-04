// RouterWrap — per-node Wrap wrapping ONE node's router subsystem.
//
// Owns this node's REQ router + RSP router at (x_coord, y_coord) in an NxM
// mesh plus their LOCAL adapters (NI edge) and LINK adapters (cross-DPI
// FlooNoC pulse-credit links toward each existing neighbor). One RouterWrap
// per mesh node; boundary directions with no neighbor are left unwired.
//
// Per node BOTH the LOCAL (NI edge) and LINK ports use identical FlooNoC
// pulse-credit wiring.
// LOCAL (NMU/NSU edge) and LINK (cross-DPI inter-router) per network:
//   set_downstream(port, LinkEjectAdapter)   — router output -> SV transport
//     buffer (no pop credit; credit returns over SV as a pulse).
//   set_upstream_credit(port, LinkCreditOut) — captures the router's input-drain
//     pulses; the wrap drains one/tick onto the *_credit/*_credit_return wire.
//   inbound flit pushes straight into router.input(port).
//   each inbound credit pulse calls router.receive_credit(port, vc), per VC.
// LOCAL pin mapping (NI edge):
//   inbound  : in_.req_in_valid/in_.req_in_flit -> req_router_->input(LOCAL).push_flit
//              in_.rsp_in_valid/in_.rsp_in_flit -> rsp_router_->input(LOCAL).push_flit
//   eject    : req_local_eject_->pop_flit() -> out_.req_out_valid/req_out_flit
//              rsp_local_eject_->pop_flit() -> out_.rsp_out_valid/rsp_out_flit
//   out cred : req_local_credit_/rsp_local_credit_.take(vc), one vc per num_vc_ ->
//              out_.req_out_credit_return[vc]/rsp_out_credit_return[vc]
//              (PULSE: router LOCAL input drained -> credit to NMU/NSU)
//   in  cred : in_.req_in_credit_return[vc]/rsp_in_credit_return[vc] (NMU/NSU consumed
//              an ejected flit) -> req_router_/rsp_router_->receive_credit(LOCAL, vc)
//              (replenishes the router's built-in credit_[LOCAL] sender counter, router->NI dir)
// The LINK port mapping (N/E/S/W, matching route_compute's dimension-order
// routing) is identical, over the link_<N>_* pins toward each neighbor.
//
// num_vc comes from cmodel_router_create; LOCAL/LINK depths = NOC_ROUTER_VC_DEPTH
// (spec-aligned; matches the NMU/NSU sender credit seed so link_perf_monitor assertions
// hold under high-fan-in hotspot traffic).
// Credit pulses marshal per-VC across the DPI boundary (VcCreditVec); the LINK
// face is per-direction (ROUTER_LINK_PORTS), only link_port_ live at 2-node.
//
// Reset invariant (construction-is-reset): the wrap holds no SV-driven reset and
// is created (cmodel_router_create) after rst_ni deasserts, so LinkCreditOut
// pending, LinkEjectAdapter queues, and the routers' FIFOs/counters all start
// empty. Mid-sim reset is NOT modeled (consistent with Router's construction-is-
// reset stance); the tb_top reset window precedes all *_create + traffic, so no
// stale pending credit can leak post-reset.
//
// Depth rationale: vc_depth = NOC_ROUTER_VC_DEPTH (spec default; also the value the
// NMU/NSU seed their own sender credit counter with, so both ends of the link agree
// on the credit window). The eject buffers are sized to num_vc * vc_depth (aggregate
// output-credit window).
// The NMU/NSU is credit-gated by *_out_credit_return / link_*_in_credit, so the
// router input never overflows.
#pragma once
#include "wrap/flit_byte_conv.hpp"  // flit_from_bytes, flit_to_bytes
#include "wrap/router_wrap_io.hpp"
#include "router/router.hpp"
#include "router/router_adapters.hpp"
#include "ni_params.h"  // NOC_ROUTER_VC_DEPTH, NOC_ROUTER_OUTPUT_FIFO_DEPTH
#include <array>
#include <memory>
#include <stdexcept>

namespace ni::cmodel::wrap {

class RouterWrap {
  public:
    void init(uint8_t x_coord, uint8_t y_coord = 0, uint8_t mesh_x_dim = 2, uint8_t mesh_y_dim = 1,
              uint8_t num_vc = 1) {
        num_vc_ = num_vc;

        // Determine which N/E/S/W directions have a neighbor in the mesh. The
        // router LOCAL port is always wired; each existing directional link gets
        // its own pulse-credit adapter pair. Boundary directions stay unwired
        // (downstream_[port] == nullptr): route_compute never routes a flit to an
        // absent neighbor (dst within mesh), and the SV generator emits a tie-off
        // assertion as defense-in-depth.
        using router::RouterPort;
        link_live_[static_cast<std::size_t>(RouterPort::EAST)] = (x_coord + 1 < mesh_x_dim);
        link_live_[static_cast<std::size_t>(RouterPort::WEST)] = (x_coord > 0);
        link_live_[static_cast<std::size_t>(RouterPort::NORTH)] = (y_coord + 1 < mesh_y_dim);
        link_live_[static_cast<std::size_t>(RouterPort::SOUTH)] = (y_coord > 0);

        router::RouterConfig c;
        c.x = x_coord;
        c.y = y_coord;
        c.mesh_x_dim = mesh_x_dim;
        c.mesh_y_dim = mesh_y_dim;
        c.num_vc = num_vc;
        // Use spec-aligned depths (NOC_ROUTER_VC_DEPTH and
        // NOC_ROUTER_OUTPUT_FIFO_DEPTH from ni_params.h / constants.yaml).
        c.vc_depth = static_cast<std::size_t>(::ni::NOC_ROUTER_VC_DEPTH);
        c.output_fifo_depth = static_cast<std::size_t>(::ni::NOC_ROUTER_OUTPUT_FIFO_DEPTH);
        req_router_ = std::make_unique<router::Router>(c);
        rsp_router_ = std::make_unique<router::Router>(c);

        wire_port(*req_router_, LOCAL, req_local_eject_, req_local_credit_);
        wire_port(*rsp_router_, LOCAL, rsp_local_eject_, rsp_local_credit_);
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            if (p == LOCAL || !link_live_[p]) continue;
            wire_port(*req_router_, p, req_link_eject_[p], req_link_credit_[p]);
            wire_port(*rsp_router_, p, rsp_link_eject_[p], rsp_link_credit_[p]);
        }

        in_ = RouterInputs{};
        out_ = RouterOutputs{};
    }

    void set_inputs(const RouterInputs& in) { in_ = in; }

    void tick() {
        // Step 1: push all inbound flits straight into the router inputs. LOCAL
        // and LINK are now symmetric FlooNoC pulse-credit ports (no InjectAdapter
        // mirror). The single NMU/NSU source sends <=1 LOCAL flit/tick, but the
        // router input register asserts on a 2nd push/port/cycle (router.hpp),
        // so guard it: exactly one LOCAL push per network per tick.
        if (in_.req_in_valid) {
            req_router_->input(LOCAL).push_flit(flit_from_bytes(in_.req_in_flit));
        }
        if (in_.rsp_in_valid) {
            rsp_router_->input(LOCAL).push_flit(flit_from_bytes(in_.rsp_in_flit));
        }
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            if (p == LOCAL || !link_live_[p]) continue;
            if (in_.link_req_in_valid[p]) {
                req_router_->input(p).push_flit(flit_from_bytes(in_.link_req_in_flit[p]));
            }
            if (in_.link_rsp_in_valid[p]) {
                rsp_router_->input(p).push_flit(flit_from_bytes(in_.link_rsp_in_flit[p]));
            }
        }
        // LOCAL credit IN: the NMU/NSU returned a pulse for a flit drained from
        // the router's LOCAL OUTPUT (router->NI direction). Replenish the
        // router's built-in credit_[LOCAL] sender counter, per VC.
        for (uint8_t vc = 0; vc < num_vc_; ++vc) {
            if (in_.req_in_credit_return[vc]) req_router_->receive_credit(LOCAL, vc);
            if (in_.rsp_in_credit_return[vc]) rsp_router_->receive_credit(LOCAL, vc);
            // Neighbor credit pulses for flits we previously sent over each LINK.
            for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
                if (p == LOCAL || !link_live_[p]) continue;
                if (in_.link_req_out_credit[p][vc]) req_router_->receive_credit(p, vc);
                if (in_.link_rsp_out_credit[p][vc]) rsp_router_->receive_credit(p, vc);
            }
        }

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
        // LOCAL credit OUT: PULSE/VC — the router's LOCAL INPUT drained a flit
        // from the NMU/NSU, so return one credit to the NMU/NSU (NI->router dir).
        for (uint8_t vc = 0; vc < num_vc_; ++vc) {
            out_.req_out_credit_return[vc] = req_local_credit_->take(vc);
            out_.rsp_out_credit_return[vc] = rsp_local_credit_->take(vc);
        }

        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            if (p == LOCAL || !link_live_[p]) continue;
            if (auto f = req_link_eject_[p]->pop_flit()) {
                out_.link_req_out_valid[p] = true;
                out_.link_req_out_flit[p] = flit_to_bytes(*f);
            }
            if (auto f = rsp_link_eject_[p]->pop_flit()) {
                out_.link_rsp_out_valid[p] = true;
                out_.link_rsp_out_flit[p] = flit_to_bytes(*f);
            }
            for (uint8_t vc = 0; vc < num_vc_; ++vc) {
                out_.link_req_in_credit[p][vc] = req_link_credit_[p]->take(vc);
                out_.link_rsp_in_credit[p][vc] = rsp_link_credit_[p]->take(vc);
            }
        }
    }

    void get_outputs(RouterOutputs& out) const { out = out_; }

    // Test introspection: the REQ router, so a test can read its built-in
    // credit_[LOCAL] sender counter (router->NI direction) across a
    // credit-return hop. Not used in production wiring.
    router::Router& req_router() { return *req_router_; }
    // RSP router accessor — used by cmodel_perf_sample_tick to sample occupancy.
    router::Router& rsp_router() { return *rsp_router_; }

    // VC count — read by the DPI handlers to size the per-VC credit
    // marshalling loops.
    uint8_t num_vc() const { return num_vc_; }

    // First live LINK direction (LOCAL excluded). For a 2-node line this is the
    // node's single neighbor (EAST for node0, WEST for node1); used by unit
    // tests that exercise the single-link case. The DPI now marshals every
    // direction port-major, so production wiring does not depend on this.
    std::size_t link_port() const {
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            if (p != LOCAL && link_live_[p]) return p;
        }
        return LOCAL;  // isolated node (no neighbor) — no live link
    }

    // Fabric-state-dump introspection (read-only): flits parked in the eject
    // adapters and undelivered credit pulses, per port (LOCAL + LINK slots).
    std::size_t req_eject_buffered(std::size_t port) const {
        const auto& ej = (port == LOCAL) ? req_local_eject_ : req_link_eject_[port];
        return ej ? ej->buffered() : 0;
    }
    std::size_t rsp_eject_buffered(std::size_t port) const {
        const auto& ej = (port == LOCAL) ? rsp_local_eject_ : rsp_link_eject_[port];
        return ej ? ej->buffered() : 0;
    }
    std::size_t req_credit_pending(std::size_t port, uint8_t vc) const {
        const auto& cr = (port == LOCAL) ? req_local_credit_ : req_link_credit_[port];
        return cr ? cr->pending(vc) : 0;
    }
    std::size_t rsp_credit_pending(std::size_t port, uint8_t vc) const {
        const auto& cr = (port == LOCAL) ? rsp_local_credit_ : rsp_link_credit_[port];
        return cr ? cr->pending(vc) : 0;
    }

  private:
    static constexpr std::size_t LOCAL = static_cast<std::size_t>(router::RouterPort::LOCAL);

    // FlooNoC pulse-credit wiring, identical for LOCAL and LINK: transport-only
    // eject (no pop credit) + a LinkCreditOut that captures the router's
    // input-drain pulses for the wrap to drain one/tick onto the credit wire.
    void wire_port(router::Router& r, std::size_t port,
                   std::unique_ptr<router::LinkEjectAdapter>& ej,
                   std::unique_ptr<router::LinkCreditOut>& credit) {
        ej = std::make_unique<router::LinkEjectAdapter>(
            static_cast<std::size_t>(num_vc_) *
            static_cast<std::size_t>(::ni::NOC_ROUTER_VC_DEPTH));
        credit = std::make_unique<router::LinkCreditOut>(num_vc_);
        r.set_downstream(port, *ej);
        r.set_upstream_credit(port, *credit);
    }

    uint8_t num_vc_ = 1;
    // Per-direction live mask (LOCAL slot unused): true where the node has a
    // neighbor in the mesh. Boundary directions stay false (unwired).
    std::array<bool, ROUTER_LINK_PORTS> link_live_{};

    std::unique_ptr<router::Router> req_router_, rsp_router_;
    std::unique_ptr<router::LinkEjectAdapter> req_local_eject_, rsp_local_eject_;
    std::unique_ptr<router::LinkCreditOut> req_local_credit_, rsp_local_credit_;
    // Per-direction LINK adapters; only live[p] slots are constructed.
    std::array<std::unique_ptr<router::LinkEjectAdapter>, ROUTER_LINK_PORTS> req_link_eject_,
        rsp_link_eject_;
    std::array<std::unique_ptr<router::LinkCreditOut>, ROUTER_LINK_PORTS> req_link_credit_,
        rsp_link_credit_;

    RouterInputs in_{};
    RouterOutputs out_{};
};

}  // namespace ni::cmodel::wrap
