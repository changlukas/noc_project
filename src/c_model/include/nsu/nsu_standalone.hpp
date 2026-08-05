#pragma once
// NsuStandalone — hermetic wrapper for the Nsu component.
//
// Includes nsu.hpp (Nsu, NsuConfig) plus the
// queue-backed terminal-endpoint scaffolding used by NsuWrap and tests.
// Separated from nsu.hpp so the production core does not carry co-sim
// harness weight.
#include "nsu/nsu.hpp"
// router bases needed by QueueNoc*
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
// QueueNocRspOut: push_flit enqueues into an internal deque (capped at
//   kMaxQueueDepth as a drain-forgotten sanity check). With FlooNoC credit
//   enabled (cosim opt-in) push_flit also gates+consumes per-VC credit and
//   credit_avail reflects the counter; with credit OFF (default) it accepts
//   unconditionally. Wrap drains via pop_rsp_flit() each tick.
//
// Invariant: NsuStandalone is non-copyable and non-movable (same as Nsu).
// -------------------------------------------------------------------------

namespace detail {

struct QueueNocReqIn : router::NocReqIn {
    // Wrap accessor: inject one flit per tick from DPI wire.
    void inject_req_flit(const Flit& f) { queue_.push_back(f); }

    // Consumer-pulse: size the per-VC pending counter before traffic. Always
    // present (no enable flag — pending only matters when the wrap drains it via
    // take_credit, which is cosim-only). Defaults to 1 VC.
    void size_pending(uint8_t num_vc) { pending_.assign(num_vc, 0); }

    // Nsu's Depacketize stage drains via pop_flit() each tick. Depacketize may
    // call pop_flit() MULTIPLE times per tick (it drains in a while-loop), so
    // pending_ MUST accumulate (counter), not latch a single bit.
    std::optional<Flit> pop_flit() override {
        if (queue_.empty()) return std::nullopt;
        Flit f = queue_.front();
        queue_.pop_front();
        const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
        ++pending_[vc];
        return f;
    }

    // Wrap accessor: drain one consumer credit pulse per tick (mirror of
    // router::LinkCreditOut::take). Returns true when a pulse was emitted.
    bool take_credit(uint8_t vc) {
        if (pending_[vc] == 0) return false;
        --pending_[vc];
        return true;
    }
    std::size_t pending(uint8_t vc) const { return pending_[vc]; }

  private:
    std::deque<Flit> queue_;
    // (count=1, value=0) vector ctor: one VC, pending pulse count 0. NOT a
    // 2-element {1, 0} initializer-list. size_pending() resizes for >1 VC.
    std::vector<std::size_t> pending_{1, 0};
};

struct QueueNocRspOut : router::NocRspOut {
    // Sanity cap: a real Wrap drains every tick, so a queue this deep
    // means the test forgot to drain. Asserts in debug; release builds skip
    // the check (and the queue is still allowed to grow unboundedly).
    static constexpr std::size_t kMaxQueueDepth = 1024;

    // FlooNoC-style NI-edge sender credit (default OFF = today's always-available).
    // When enabled, this models the InjectAdapter credit pattern: a per-VC
    // counter seeded to the downstream (router LOCAL input) depth; push_flit
    // decrements on accept, receive_credit increments on a credit pulse.
    // INVARIANT: credit_[vc] is decremented ONLY in push_flit and incremented
    // ONLY in receive_credit, so credit_[vc] + outstanding == seed holds.
    void enable_credit(uint8_t num_vc, std::size_t seed) {
        credit_enabled_ = true;
        credit_.assign(num_vc, seed);
    }
    void receive_credit(uint8_t vc) { ++credit_[vc]; }

    // S3a T5: ready/valid mode for the RSP face (SimpleRouter downstream, no
    // credit at all — spec §4.3). Orthogonal to credit_enabled_; exactly one
    // of the two is ever enabled by a given caller. ready_ is a live signal
    // (not consumed), set from the DPI-sampled tx_rsp_ready wire each tick
    // (S3a stage design §5.3 — credit_avail(vc) stays the predicate name, it
    // just reports downstream ready instead of a credit pool).
    void enable_ready_track() { ready_track_ = true; }
    void set_ready(bool r) { ready_ = r; }

    // Accept a flit into the queue. Ready-track mode gates on the live ready
    // signal. Credit mode gates on and consumes one per-VC credit. Neither
    // enabled models infinite downstream bandwidth (always accept).
    bool push_flit(const Flit& f) override {
        if (ready_track_) {
            if (!ready_) return false;
        } else if (credit_enabled_) {
            const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
            if (credit_[vc] == 0) return false;
            --credit_[vc];
        }
        assert(queue_.size() < kMaxQueueDepth &&
               "QueueNocRspOut overflow — did the test Wrap forget to drain?");
        queue_.push_back(f);
        return true;
    }
    bool credit_avail(uint8_t vc) const override {
        if (ready_track_) return ready_;
        return !credit_enabled_ || credit_[vc] > 0;
    }

    // Wrap accessor: pop one flit per tick for DPI forwarding.
    std::optional<Flit> pop_rsp_flit() {
        if (queue_.empty()) return std::nullopt;
        Flit f = queue_.front();
        queue_.pop_front();
        return f;
    }

  private:
    std::deque<Flit> queue_;
    bool credit_enabled_ = false;
    std::vector<std::size_t> credit_;
    bool ready_track_ = false;
    bool ready_ = false;
};

}  // namespace detail

class NsuStandalone {
  public:
    explicit NsuStandalone(NsuConfig cfg)
        : num_vc_(static_cast<uint8_t>(cfg.num_vc)),
          dat_num_vc_(static_cast<uint8_t>(cfg.dat_num_vc)),
          queue_req_in_(),
          queue_rsp_out_(),
          queue_dat_req_in_(),
          queue_dat_rsp_out_(),
          nsu_(std::move(cfg), queue_req_in_, queue_rsp_out_, queue_dat_req_in_,
               queue_dat_rsp_out_) {
        queue_req_in_.size_pending(num_vc_);
        queue_dat_req_in_.size_pending(dat_num_vc_);
    }

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
    std::optional<Flit> pop_rsp_flit() { return queue_rsp_out_.pop_rsp_flit(); }
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
    // DAT egress (push into nsu().dat_vc_arbiter(), drain here). Unwired to
    // real DPI until T5; ctest-mock-only until then.
    void inject_dat_req_flit(const Flit& f) { queue_dat_req_in_.inject_req_flit(f); }
    std::optional<Flit> pop_dat_rsp_flit() { return queue_dat_rsp_out_.pop_rsp_flit(); }
    bool dat_rsp_credit_avail(uint8_t vc = 0) const { return queue_dat_rsp_out_.credit_avail(vc); }
    void enable_dat_noc_credit(std::size_t seed) {
        queue_dat_rsp_out_.enable_credit(dat_num_vc_, seed);
    }
    void dat_rsp_receive_credit(uint8_t vc = 0) { queue_dat_rsp_out_.receive_credit(vc); }
    bool dat_req_take_credit(uint8_t vc = 0) { return queue_dat_req_in_.take_credit(vc); }

  private:
    uint8_t num_vc_;
    uint8_t dat_num_vc_;
    detail::QueueNocReqIn queue_req_in_;
    detail::QueueNocRspOut queue_rsp_out_;
    detail::QueueNocReqIn queue_dat_req_in_;
    detail::QueueNocRspOut queue_dat_rsp_out_;
    Nsu nsu_;
};

}  // namespace ni::cmodel::nsu
