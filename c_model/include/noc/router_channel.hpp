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

#include <cstdint>
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

}  // namespace ni::cmodel::noc
