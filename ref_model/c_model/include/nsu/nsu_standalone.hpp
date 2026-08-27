#pragma once
// NsuStandalone — hermetic wrapper for the Nsu component.
//
// Includes nsu.hpp (Nsu, NsuConfig) plus the
// queue-backed terminal-endpoint scaffolding used by NsuWrap and tests.
// Separated from nsu.hpp so the production core does not carry co-sim
// harness weight.
#include "nsu/nsu.hpp"
// router bases needed by QueueNoc*
#include "router/queue_credit_out.hpp"
#include "router/req_in.hpp"
#include "router/rsp_out.hpp"
#include <deque>
#include <optional>
#include <vector>

namespace ni::cmodel::nsu {

// -------------------------------------------------------------------------
// NsuStandalone — hermetic wrapper, no external NoC refs.
//
// Wraps construct NsuStandalone(NsuConfig{...}) without supplying
// NocReqIn& / NocRspOut&. The wrapper owns queue-backed terminal endpoints for
// both interfaces; real DPI wiring drives/drains them at the Wrap tick
// boundary.
//
// QueueNocReqIn: Wrap injects flits via inject_req_flit() before
//   calling nsu_.tick(); Nsu's Depacketize stage drains via pop_flit().
//
// QueueNocRspOut: router::QueueCreditOut over NocRspOut — see
//   router/queue_credit_out.hpp. Wrap drains via pop_flit() each tick.
//
// Invariant: NsuStandalone is non-copyable and non-movable (same as Nsu).
// -------------------------------------------------------------------------

namespace detail {

struct QueueNocReqIn : router::NocReqIn {
    // Wrap accessor: inject one flit per tick from DPI wire.
    void inject_req_flit(const Flit& f) { queue_.push_back(f); }

    // Nsu's Depacketize stage drains via pop_flit() each tick. No credit is
    // counted here: a flit leaving this queue only moves into Depacketize's
    // per-VC ingress queue, which is the buffer the sender's credit actually
    // tracks. That queue returns the slot when it hands the flit to AXI
    // (nsu::Depacketize::take_dat_credit).
    std::optional<Flit> pop_flit() override {
        if (queue_.empty()) return std::nullopt;
        Flit f = queue_.front();
        queue_.pop_front();
        return f;
    }

  private:
    std::deque<Flit> queue_;
};

using QueueNocRspOut = router::QueueCreditOut<router::NocRspOut>;

}  // namespace detail

class NsuStandalone {
  public:
    explicit NsuStandalone(NsuConfig cfg)
        : dat_num_vc_(static_cast<uint8_t>(cfg.dat_num_vc)),
          queue_req_in_(),
          queue_rsp_out_(),
          queue_dat_req_in_(),
          queue_dat_rsp_out_(),
          nsu_(std::move(cfg), queue_req_in_, queue_rsp_out_, queue_dat_req_in_,
               queue_dat_rsp_out_) {}

    NsuStandalone(const NsuStandalone&) = delete;
    NsuStandalone(NsuStandalone&&) = delete;
    NsuStandalone& operator=(const NsuStandalone&) = delete;
    NsuStandalone& operator=(NsuStandalone&&) = delete;

    AxiMasterPort& axi_master_port() noexcept { return nsu_.axi_master_port(); }
    void tick() { nsu_.tick(); }
    std::size_t stage_occupancy(NiPath path, std::size_t stage, uint8_t axi_ch) const {
        return nsu_.stage_occupancy(path, stage, axi_ch);
    }
    Nsu& nsu() noexcept { return nsu_; }

    // Wrap accessors — inject req side, drain rsp side.
    void inject_req_flit(const Flit& f) { queue_req_in_.inject_req_flit(f); }
    std::optional<Flit> pop_rsp_flit() { return queue_rsp_out_.pop_flit(); }
    bool rsp_credit_avail(uint8_t vc = 0) const { return queue_rsp_out_.credit_avail(vc); }

    // S3a T5: RSP is a ready/valid network (spec §4.3) — no credit. The wrap
    // calls enable_rsp_ready_track() unconditionally in init(), then
    // rsp_set_ready(tx_rsp_ready) every tick from the DPI-sampled wire
    // (§5.3 — credit_avail(vc) stays the predicate name, it just reports
    // downstream ready instead of a credit pool). REQ ingress needs no
    // credit-return at all: the c_model's ingress queue is unbounded, so the
    // wrap ties rx_req_ready constant-high (see nsu_wrap.hpp).
    void enable_rsp_ready_track() { queue_rsp_out_.enable_ready_track(); }
    void rsp_set_ready(bool ready) { queue_rsp_out_.set_ready(ready); }

    // DAT face accessors (S3a T4): mirror of the REQ/RSP set above, for the
    // DAT ingress (inject here, Depacketize's second ingress drains it) and
    // DAT egress (push into nsu().dat_vc_allocator(), drain here). Unwired to
    // real DPI until T5; ctest-mock-only until then.
    void inject_dat_req_flit(const Flit& f) { queue_dat_req_in_.inject_req_flit(f); }
    std::optional<Flit> pop_dat_rsp_flit() { return queue_dat_rsp_out_.pop_flit(); }
    bool dat_rsp_credit_avail(uint8_t vc = 0) const { return queue_dat_rsp_out_.credit_avail(vc); }
    void enable_dat_noc_credit(std::size_t seed) {
        queue_dat_rsp_out_.enable_credit(dat_num_vc_, seed);
    }
    void dat_rsp_receive_credit(uint8_t vc = 0) { queue_dat_rsp_out_.receive_credit(vc); }
    bool dat_req_take_credit(uint8_t vc = 0) { return nsu_.take_dat_credit(vc); }

  private:
    uint8_t dat_num_vc_;
    detail::QueueNocReqIn queue_req_in_;
    detail::QueueNocRspOut queue_rsp_out_;
    detail::QueueNocReqIn queue_dat_req_in_;
    detail::QueueNocRspOut queue_dat_rsp_out_;
    Nsu nsu_;
};

}  // namespace ni::cmodel::nsu
