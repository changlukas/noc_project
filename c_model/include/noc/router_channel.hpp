#pragma once
// RouterChannel: production 2-node, 1-hop, full-duplex fabric segment wiring the
// Router into the NMU<->NSU datapath. Spec:
// docs/superpowers/specs/2026-06-13-router-channel-design.md
//
// Three adapters bridge the NoC interface (retryable push + credit query + pull)
// to the Router link contract (void push + registered credit pulse):
//   InjectAdapter : NocReqOut/NocRspOut + RouterCreditSink  (NI -> router LOCAL input)
//   EjectAdapter  : NocReqIn/NocRspIn  + RouterLink         (router LOCAL output -> NI)
//   CreditRelay   : RouterCreditSink                        (downstream input credit
//                                                            -> upstream output credit)
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "ni_params.h"
#include "noc/noc_req_in.hpp"
#include "noc/noc_req_out.hpp"
#include "noc/noc_rsp_in.hpp"
#include "noc/noc_rsp_out.hpp"
#include "noc/router.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace ni::cmodel::noc {

// NI -> router LOCAL input. Implements all four producer-side NoC interfaces
// (NocReqOut and NocRspOut share the same shape) and is the router's
// RouterCreditSink for that input port. A per-VC credit mirror (seeded to the
// router input FIFO depth) plus a per-tick landing-register guard translate the
// router's void/assert push into a retryable false.
class InjectAdapter : public NocReqOut, public NocRspOut, public RouterCreditSink {
  public:
    InjectAdapter(Router& router, std::size_t port, uint8_t num_vc, std::size_t depth)
        : router_(router), port_(port), credit_(num_vc, depth) {}

    bool credit_avail(uint8_t vc) const override { return !pushed_this_tick_ && credit_[vc] > 0; }
    bool push_flit(const Flit& flit) override {
        const auto vc = static_cast<uint8_t>(flit.get_header_field("vc_id"));
        if (pushed_this_tick_ || credit_[vc] == 0) return false;
        router_.input(port_).push_flit(flit);
        --credit_[vc];
        pushed_this_tick_ = true;
        return true;
    }
    void receive_credit(uint8_t vc) override { ++credit_[vc]; }
    void on_tick() { pushed_this_tick_ = false; }
    std::size_t mirror_credit(uint8_t vc) const { return credit_[vc]; }  // test introspection

  private:
    Router& router_;
    std::size_t port_;
    std::vector<std::size_t> credit_;
    bool pushed_this_tick_ = false;
};

// router LOCAL output -> NI. Implements the consumer-side NoC interfaces and is
// the router's downstream RouterLink. Buffer depth MUST equal the router's
// LOCAL-output credit seed so the void push_flit never overflows (credit gating
// is the only backpressure -- see spec section 4).
// One instance per direction: the NocReqIn/NocRspIn pop_flit bases share one
// queue, so bind a given EjectAdapter to a single network's LOCAL output only
// (req OR rsp, never both) or the two streams would interleave on one queue.
class EjectAdapter : public NocReqIn, public NocRspIn, public RouterLink {
  public:
    EjectAdapter(Router& router, std::size_t port, std::size_t depth)
        : router_(router), port_(port), depth_(depth) {}

    void push_flit(const Flit& flit) override {
        assert(queue_.size() < depth_ &&
               "EjectAdapter overflow: queue depth must equal the router LOCAL-output credit "
               "seed (credit gating should have prevented this)");
        queue_.push_back(flit);
    }
    std::optional<Flit> pop_flit() override {
        if (queue_.empty()) return std::nullopt;
        Flit f = queue_.front();
        queue_.pop_front();
        const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
        router_.receive_credit(port_, vc);
        return f;
    }
    std::size_t buffered() const { return queue_.size(); }

  private:
    Router& router_;
    std::size_t port_;
    std::size_t depth_;
    std::deque<Flit> queue_;
};

// Forwards a downstream router's input-credit pulse to the upstream router's
// matching OUTPUT port. Registered on the downstream via set_upstream_credit.
class CreditRelay : public RouterCreditSink {
  public:
    CreditRelay(Router& upstream, std::size_t upstream_out_port)
        : upstream_(upstream), port_(upstream_out_port) {}
    void receive_credit(uint8_t vc) override { upstream_.receive_credit(port_, vc); }

  private:
    Router& upstream_;
    std::size_t port_;
};

// Production 2-node, 1-hop, full-duplex fabric segment. Two physical networks
// (REQ + RSP), each a 2-router 2x1 mesh. Node 0 = (0,0), node 1 = (1,0). The
// inter-router link uses WEST/EAST: router(1).WEST <-> router(0).EAST.
//
// Adapters live in unique_ptr (stable addresses); routers hold raw
// RouterLink*/RouterCreditSink* into them — do NOT move adapters into a
// reallocating vector.
class RouterChannel {
  public:
    static constexpr std::size_t kNodes = 2;
    static constexpr std::size_t LOCAL = static_cast<std::size_t>(RouterPort::LOCAL);
    static constexpr std::size_t EAST = static_cast<std::size_t>(RouterPort::EAST);
    static constexpr std::size_t WEST = static_cast<std::size_t>(RouterPort::WEST);

    explicit RouterChannel(uint8_t num_vc = NI_NOC_NUM_VC,
                           std::size_t vc_depth = NI_NOC_ROUTER_VC_DEPTH,
                           std::size_t out_fifo_depth = NI_NOC_ROUTER_OUTPUT_FIFO_DEPTH)
        : num_vc_(num_vc), vc_depth_(vc_depth) {
        for (std::size_t n = 0; n < kNodes; ++n) {
            RouterConfig c;
            c.x = static_cast<uint8_t>(n);
            c.y = 0;
            c.mesh_x_dim = 2;
            c.mesh_y_dim = 1;
            c.num_vc = num_vc;
            c.vc_depth = vc_depth;
            c.output_fifo_depth = out_fifo_depth;
            req_routers_.push_back(std::make_unique<Router>(c));
            rsp_routers_.push_back(std::make_unique<Router>(c));
        }
        for (std::size_t n = 0; n < kNodes; ++n) {
            wire_local(*req_routers_[n], req_inject_[n], req_eject_[n]);
            wire_local(*rsp_routers_[n], rsp_inject_[n], rsp_eject_[n]);
        }
        wire_link(*req_routers_[1], *req_routers_[0], req_relay_10_, req_relay_01_);
        wire_link(*rsp_routers_[1], *rsp_routers_[0], rsp_relay_10_, rsp_relay_01_);
    }

    NocReqOut& nmu_req_out(std::size_t node) { return *req_inject_[node]; }
    NocRspIn& nmu_rsp_in(std::size_t node) { return *rsp_eject_[node]; }
    NocReqIn& nsu_req_in(std::size_t node) { return *req_eject_[node]; }
    NocRspOut& nsu_rsp_out(std::size_t node) { return *rsp_inject_[node]; }

    void tick() {
        for (auto& a : req_inject_) a->on_tick();
        for (auto& a : rsp_inject_) a->on_tick();
        for (auto& r : req_routers_) r->tick();
        for (auto& r : rsp_routers_) r->tick();
    }

    // Test introspection (for the Task 4 conservation asserts).
    Router& req_router(std::size_t node) { return *req_routers_[node]; }
    Router& rsp_router(std::size_t node) { return *rsp_routers_[node]; }
    std::size_t req_eject_buffered(std::size_t node) const { return req_eject_[node]->buffered(); }
    std::size_t rsp_eject_buffered(std::size_t node) const { return rsp_eject_[node]->buffered(); }

  private:
    void wire_local(Router& r, std::unique_ptr<InjectAdapter>& inj,
                    std::unique_ptr<EjectAdapter>& ej) {
        inj = std::make_unique<InjectAdapter>(r, LOCAL, num_vc_, vc_depth_);
        ej = std::make_unique<EjectAdapter>(r, LOCAL, vc_depth_);
        r.set_upstream_credit(LOCAL, *inj);
        r.set_downstream(LOCAL, *ej);
    }
    void wire_link(Router& a, Router& b, std::unique_ptr<CreditRelay>& relay_a,
                   std::unique_ptr<CreditRelay>& relay_b) {
        a.set_downstream(WEST, b.input(EAST));  // a.WEST_out -> b.EAST_in
        b.set_downstream(EAST, a.input(WEST));  // b.EAST_out -> a.WEST_in
        relay_a = std::make_unique<CreditRelay>(a, WEST);
        relay_b = std::make_unique<CreditRelay>(b, EAST);
        b.set_upstream_credit(EAST, *relay_a);
        a.set_upstream_credit(WEST, *relay_b);
    }

    uint8_t num_vc_;
    std::size_t vc_depth_;
    std::vector<std::unique_ptr<Router>> req_routers_, rsp_routers_;
    std::array<std::unique_ptr<InjectAdapter>, kNodes> req_inject_, rsp_inject_;
    std::array<std::unique_ptr<EjectAdapter>, kNodes> req_eject_, rsp_eject_;
    std::unique_ptr<CreditRelay> req_relay_10_, req_relay_01_, rsp_relay_10_, rsp_relay_01_;
};

}  // namespace ni::cmodel::noc
