#pragma once
// Router channel-bridge adapters. Two families:
//   LOCAL (NI edge): InjectAdapter / EjectAdapter / CreditRelay bridge the NoC
//     interface (retryable push + credit query + pull) to the Router link
//     contract (void push + registered credit pulse).
//   LINK (cross-DPI): LinkEjectAdapter / LinkCreditOut carry a FlooNoC pulse-credit
//     inter-router link over SV — the router holds real per-(port,vc) credit;
//     these adapters only marshal the wire side.
//
//   InjectAdapter   : NocReqOut/NocRspOut + RouterCreditSink (NI -> router LOCAL input)
//   EjectAdapter    : NocReqIn/NocRspIn  + RouterLink        (router LOCAL output -> NI)
//   CreditRelay     : RouterCreditSink                       (downstream input credit
//                                                             -> upstream output credit)
//   LinkEjectAdapter: RouterLink   (router LINK output -> SV transport buffer, no pop credit)
//   LinkCreditOut   : RouterCreditSink (router LINK input drain -> SV credit pulse)
//
// Reset invariant (construction-is-reset): these adapters model reset as
// construction. They hold no SV-driven reset and are created after rst_ni
// deasserts, so LinkCreditOut pending, LinkEjectAdapter queue, and the router's
// FIFOs/counters all start empty. Mid-sim reset is NOT modeled (consistent with
// Router's construction-is-reset stance); the tb_top reset window precedes all
// *_create + traffic, so no stale pending credit can leak post-reset.
#include "flit.hpp"
#include "noc/noc_req_in.hpp"
#include "noc/noc_req_out.hpp"
#include "noc/noc_rsp_in.hpp"
#include "noc/noc_rsp_out.hpp"
#include "noc/router.hpp"

#include <cassert>
#include <cstdint>
#include <deque>
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
// the router's downstream RouterLink. The eject queue is SHARED across all VCs,
// but the router's LOCAL output has num_vc INDEPENDENT credit counters each
// seeded to vc_depth, so it can grant up to num_vc*vc_depth flits total before
// any credit is returned (NSU stalled). Buffer depth MUST therefore cover the
// AGGREGATE LOCAL-output credit = num_vc*vc_depth so the void push_flit never
// overflows (credit gating is the only backpressure -- see spec section 4); the
// per-VC seed alone is insufficient when num_vc>1.
// One instance per direction: the NocReqIn/NocRspIn pop_flit bases share one
// queue, so bind a given EjectAdapter to a single network's LOCAL output only
// (req OR rsp, never both) or the two streams would interleave on one queue.
class EjectAdapter : public NocReqIn, public NocRspIn, public RouterLink {
  public:
    EjectAdapter(Router& router, std::size_t port, std::size_t depth)
        : router_(router), port_(port), depth_(depth) {}

    void push_flit(const Flit& flit) override {
        assert(queue_.size() < depth_ &&
               "EjectAdapter overflow: queue depth must cover the AGGREGATE LOCAL-output credit "
               "(num_vc*vc_depth) (credit gating should have prevented this)");
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

// Router output -> SV link transport buffer. Unlike EjectAdapter, pop does NOT
// return router credit: link credit arrives as the neighbor's input-drain pulse
// over SV (FlooNoC credit). Depth covers the in-flight grant window.
class LinkEjectAdapter : public RouterLink {
  public:
    explicit LinkEjectAdapter(std::size_t depth) : depth_(depth) {}
    void push_flit(const Flit& f) override {
        assert(queue_.size() < depth_ && "LinkEjectAdapter overflow");
        queue_.push_back(f);
    }
    std::optional<Flit> pop_flit() {
        if (queue_.empty()) return std::nullopt;
        Flit f = queue_.front();
        queue_.pop_front();
        return f;
    }
    std::size_t buffered() const { return queue_.size(); }

  private:
    std::size_t depth_;
    std::deque<Flit> queue_;
};

// Router input-port credit sink for a cross-DPI link. The router pulses
// receive_credit(vc) when a link input-FIFO slot frees; accumulate per-VC and
// let the shell drain one pulse/tick onto the SV credit wire to the neighbor.
class LinkCreditOut : public RouterCreditSink {
  public:
    explicit LinkCreditOut(uint8_t num_vc) : pending_(num_vc, 0) {}
    void receive_credit(uint8_t vc) override { ++pending_[vc]; }
    bool take(uint8_t vc) {  // returns true => assert credit pulse this tick
        if (pending_[vc] == 0) return false;
        --pending_[vc];
        return true;
    }
    std::size_t pending(uint8_t vc) const { return pending_[vc]; }

  private:
    std::vector<std::size_t> pending_;
};

}  // namespace ni::cmodel::noc
