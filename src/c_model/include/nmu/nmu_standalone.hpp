#pragma once
// NmuStandalone — hermetic wrapper for the Nmu component.
//
// Includes nmu.hpp (Nmu, NmuConfig, detail::make_vc_arbiter) plus the
// queue-backed terminal-endpoint scaffolding used by NmuWrap and tests.
// Separated from nmu.hpp so the production core does not carry co-sim
// harness weight.
#include "nmu/nmu.hpp"
// router bases needed by QueueNoc*
#include "router/req_out.hpp"
#include "router/rsp_in.hpp"
#include <deque>
#include <optional>
#include <vector>

namespace ni::cmodel::nmu {

// -------------------------------------------------------------------------
// NmuStandalone — hermetic wrapper, no external NoC refs.
//
// Wraps construct NmuStandalone(NmuConfig{...}) without supplying
// NocReqOut& / NocRspIn&. The wrapper owns queue-backed implementations of
// both interfaces; the real DPI wiring replaces them at the Wrap
// tick boundary. The wrapper's QueueNocReqOut/QueueNocRspIn: push_flit
// enqueues, pop_flit dequeues.
//
// Invariant: NmuStandalone is non-copyable and non-movable (same as Nmu).
// -------------------------------------------------------------------------

// QueueNocReqOut / QueueNocRspIn are queue-backed terminal endpoints so that
// NmuWrap can drain produced req flits and inject incoming rsp flits
// at the DPI boundary without modifying the Nmu internals.
//
// QueueNocReqOut: push_flit enqueues into an internal deque (capped at
//   kMaxQueueDepth as a drain-forgotten sanity check). With FlooNoC credit
//   enabled (cosim opt-in) push_flit also gates+consumes per-VC credit and
//   credit_avail reflects the counter; with credit OFF (default) it accepts
//   unconditionally. Wrap drains via pop_req_flit() each tick.
//
// QueueNocRspIn: Wrap injects flits via inject_rsp_flit() before
//   calling nmu_.tick(); Nmu's Depacketize stage drains via pop_flit().
namespace detail {

struct QueueNocReqOut : router::NocReqOut {
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

    // S3a T5: ready/valid mode for the REQ face (SimpleRouter downstream, no
    // credit at all — spec §4.3). Orthogonal to credit_enabled_; exactly one
    // of the two is ever enabled by a given caller. ready_ is a live signal
    // (not consumed), set from the DPI-sampled tx_req_ready wire each tick
    // (S3a stage design §5.3 — credit_avail(vc) stays the predicate name,
    // it just reports downstream ready instead of a credit pool).
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
               "QueueNocReqOut overflow — did the test Wrap forget to drain?");
        queue_.push_back(f);
        return true;
    }
    bool credit_avail(uint8_t vc) const override {
        if (ready_track_) return ready_;
        return !credit_enabled_ || credit_[vc] > 0;
    }

    // Wrap accessor: pop one flit per tick for DPI forwarding.
    std::optional<Flit> pop_req_flit() {
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

struct QueueNocRspIn : router::NocRspIn {
    // Wrap accessor: inject one flit per tick from DPI wire.
    void inject_rsp_flit(const Flit& f) { queue_.push_back(f); }

    // Consumer-pulse: size the per-VC pending counter before traffic. Always
    // present (no enable flag needed — pending only matters when the wrap drains
    // it via take_credit, which is cosim-only). Defaults to 1 VC.
    void size_pending(uint8_t num_vc) { pending_.assign(num_vc, 0); }

    // Nmu's Depacketize stage drains via pop_flit() each tick. Depacketize may
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

}  // namespace detail

class NmuStandalone {
  public:
    explicit NmuStandalone(NmuConfig cfg)
        : num_vc_(static_cast<uint8_t>(cfg.num_vc)),
          dat_num_vc_(static_cast<uint8_t>(cfg.dat_num_vc)),
          queue_req_out_(),
          queue_rsp_in_(),
          queue_dat_req_out_(),
          queue_dat_rsp_in_(),
          nmu_(std::move(cfg), queue_req_out_, queue_rsp_in_, queue_dat_req_out_,
               queue_dat_rsp_in_) {
        queue_rsp_in_.size_pending(num_vc_);
        queue_dat_rsp_in_.size_pending(dat_num_vc_);
    }

    NmuStandalone(const NmuStandalone&) = delete;
    NmuStandalone(NmuStandalone&&) = delete;
    NmuStandalone& operator=(const NmuStandalone&) = delete;
    NmuStandalone& operator=(NmuStandalone&&) = delete;

    AxiSlavePort& axi_slave_port() noexcept { return nmu_.axi_slave_port(); }
    void tick() { nmu_.tick(); }
    const Rob& rob() const noexcept { return nmu_.rob(); }
    const VcArbiter& vc_arbiter() const noexcept { return nmu_.vc_arbiter(); }
    std::size_t stage_occupancy(NiPath path, std::size_t stage, uint8_t axi_ch) const {
        return nmu_.stage_occupancy(path, stage, axi_ch);
    }
    Nmu& nmu() noexcept { return nmu_; }

    // Wrap accessors — drain req side, inject rsp side.
    std::optional<Flit> pop_req_flit() { return queue_req_out_.pop_req_flit(); }
    void inject_rsp_flit(const Flit& f) { queue_rsp_in_.inject_rsp_flit(f); }
    bool req_credit_avail(uint8_t vc = 0) const { return queue_req_out_.credit_avail(vc); }

    // S3a T5: REQ is a ready/valid network (spec §4.3) — no credit. The wrap
    // calls enable_req_ready_track() unconditionally in init(), then
    // req_set_ready(tx_req_ready) every tick from the DPI-sampled wire
    // (§5.3 — credit_avail(vc) stays the predicate name, it just reports
    // downstream ready instead of a credit pool). RSP ingress needs no
    // credit-return at all: the c_model's ingress queue is unbounded, so the
    // wrap ties rx_rsp_ready constant-high (see nmu_wrap.hpp).
    void enable_req_ready_track() { queue_req_out_.enable_ready_track(); }
    void req_set_ready(bool ready) { queue_req_out_.set_ready(ready); }

    // DAT face accessors (S3a T4): mirror of the REQ/RSP set above, for the
    // DAT egress (push into nmu().dat_wormhole_arbiter().input(0/1), drain
    // here) and DAT ingress (inject here, Depacketize's second ingress
    // drains it). Unwired to real DPI until T5; ctest-mock-only until then.
    std::optional<Flit> pop_dat_req_flit() { return queue_dat_req_out_.pop_req_flit(); }
    void inject_dat_rsp_flit(const Flit& f) { queue_dat_rsp_in_.inject_rsp_flit(f); }
    bool dat_req_credit_avail(uint8_t vc = 0) const { return queue_dat_req_out_.credit_avail(vc); }
    void enable_dat_noc_credit(std::size_t seed) {
        queue_dat_req_out_.enable_credit(dat_num_vc_, seed);
    }
    void dat_req_receive_credit(uint8_t vc = 0) { queue_dat_req_out_.receive_credit(vc); }
    bool dat_rsp_take_credit(uint8_t vc = 0) { return queue_dat_rsp_in_.take_credit(vc); }

  private:
    uint8_t num_vc_;
    uint8_t dat_num_vc_;
    detail::QueueNocReqOut queue_req_out_;
    detail::QueueNocRspIn queue_rsp_in_;
    detail::QueueNocReqOut queue_dat_req_out_;
    detail::QueueNocRspIn queue_dat_rsp_in_;
    Nmu nmu_;
};

}  // namespace ni::cmodel::nmu
