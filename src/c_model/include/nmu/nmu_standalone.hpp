#pragma once
// NmuStandalone — hermetic wrapper for the Nmu component.
//
// Includes nmu.hpp (Nmu, NmuConfig, detail::make_vc_arbiter) plus the
// queue-backed terminal-endpoint scaffolding used by NmuWrap and tests.
// Separated from nmu.hpp so the production core does not carry co-sim
// harness weight.
#include "nmu/nmu.hpp"
// router bases needed by NullNoc*
#include "router/req_out.hpp"
#include "router/rsp_in.hpp"
#include <deque>
#include <optional>
#include <vector>

namespace ni::cmodel::nmu {

// -------------------------------------------------------------------------
// Stage 5b: NmuStandalone — hermetic wrapper, no external NoC refs.
//
// Wraps construct NmuStandalone(NmuConfig{...}) without supplying
// NocReqOut& / NocRspIn&. The wrapper owns null-stub implementations of
// both interfaces; the real DPI wiring replaces them at the Wrap
// tick boundary. The wrapper's NullNocReqOut/NullNocRspIn are real queues
// (not no-op stubs): push_flit enqueues, pop_flit dequeues.
//
// Invariant: NmuStandalone is non-copyable and non-movable (same as Nmu).
// -------------------------------------------------------------------------

// NullNocReqOut / NullNocRspIn are queue-backed terminal endpoints so that
// NmuWrap can drain produced req flits and inject incoming rsp flits
// at the DPI boundary without modifying the Nmu internals.
//
// NullNocReqOut: push_flit enqueues into an internal deque (capped at
//   kMaxQueueDepth as a drain-forgotten sanity check). With FlooNoC credit
//   enabled (cosim opt-in) push_flit also gates+consumes per-VC credit and
//   credit_avail reflects the counter; with credit OFF (default) it accepts
//   unconditionally. Wrap drains via pop_req_flit() each tick.
//
// NullNocRspIn: Wrap injects flits via inject_rsp_flit() before
//   calling nmu_.tick(); Nmu's Depacketize stage drains via pop_flit().
namespace detail {

struct NullNocReqOut : router::NocReqOut {
    // Sanity cap: a real Wrap drains every tick, so a queue this deep
    // means the test forgot to drain. Asserts in debug; release builds skip
    // the check (and the queue is still allowed to grow unboundedly).
    static constexpr std::size_t kMaxQueueDepth = 1024;

    // R2 opt-in FlooNoC sender credit (default OFF = today's always-available).
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

    // Accept a flit into the queue. When credit is disabled this models
    // infinite downstream bandwidth (always accept). When enabled it gates on
    // and consumes one per-VC credit, returning false (backpressure) if none.
    bool push_flit(const Flit& f) override {
        if (credit_enabled_) {
            const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
            if (credit_[vc] == 0) return false;
            --credit_[vc];
        }
        assert(queue_.size() < kMaxQueueDepth &&
               "NullNocReqOut overflow — did the test Wrap forget to drain?");
        queue_.push_back(f);
        return true;
    }
    bool credit_avail(uint8_t vc) const override { return !credit_enabled_ || credit_[vc] > 0; }

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
};

struct NullNocRspIn : router::NocRspIn {
    // Wrap accessor: inject one flit per tick from DPI wire.
    void inject_rsp_flit(const Flit& f) { queue_.push_back(f); }

    // R2 consumer-pulse: size the per-VC pending counter before traffic. Always
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
          null_req_out_(),
          null_rsp_in_(),
          nmu_(std::move(cfg), null_req_out_, null_rsp_in_) {
        null_rsp_in_.size_pending(num_vc_);
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

    // Stage 5b Wrap accessors — drain req side, inject rsp side.
    std::optional<Flit> pop_req_flit() { return null_req_out_.pop_req_flit(); }
    void inject_rsp_flit(const Flit& f) { null_rsp_in_.inject_rsp_flit(f); }
    bool req_credit_avail(uint8_t vc = 0) const { return null_req_out_.credit_avail(vc); }

    // R2 opt-in FlooNoC credit at the NoC terminal edge (cosim-only; default
    // OFF). Seeds the req-out sender counter; the rsp-in consumer pulse is
    // always active (sized in the ctor) but inert unless rsp_take_credit drains.
    void enable_noc_credit(std::size_t seed) { null_req_out_.enable_credit(num_vc_, seed); }
    void req_receive_credit(uint8_t vc = 0) { null_req_out_.receive_credit(vc); }
    bool rsp_take_credit(uint8_t vc = 0) { return null_rsp_in_.take_credit(vc); }

  private:
    uint8_t num_vc_;
    detail::NullNocReqOut null_req_out_;
    detail::NullNocRspIn null_rsp_in_;
    Nmu nmu_;
};

}  // namespace ni::cmodel::nmu
