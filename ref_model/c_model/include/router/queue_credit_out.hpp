#pragma once
// QueueCreditOut — queue-backed NoC egress terminal, shared by the NMU
// (REQ/DAT) and NSU (RSP/DAT) standalone wrappers.
//
// `Base` is router::NocReqOut or router::NocRspOut. The two declare the same
// push_flit / credit_avail contract, so one definition serves both faces; the
// wrap layer drains the queue at the DPI tick boundary via pop_flit().
#include "flit.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace ni::cmodel::router {

template <class Base>
struct QueueCreditOut : Base {
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

    // S3a T5: ready/valid mode for the REQ/RSP faces (SimpleRouter downstream,
    // no credit at all — spec §4.3). Orthogonal to credit_enabled_; exactly one
    // of the two is ever enabled by a given caller. ready_ is a live signal
    // (not consumed), set from the DPI-sampled tx_{req,rsp}_ready wire each
    // tick (S3a stage design §5.3 — credit_avail(vc) stays the predicate name,
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
               "QueueCreditOut overflow — did the test Wrap forget to drain?");
        queue_.push_back(f);
        return true;
    }
    bool credit_avail(uint8_t vc) const override {
        if (ready_track_) return ready_;
        return !credit_enabled_ || credit_[vc] > 0;
    }

    // Wrap accessor: pop one flit per tick for DPI forwarding.
    std::optional<Flit> pop_flit() {
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

}  // namespace ni::cmodel::router
