#pragma once
// Generic N-to-1 wormhole arbiter for c_model NoC behavior model.
//
// Inherits FlooNoC's wormhole locking semantic (hw/floo_wormhole_arbiter.sv
// LockIn=1, release on `last_out & ready_i`) and collapses FlooNoC's separate
// SelW state machine into a single arbiter via the optional ChannelPairing
// config. Used at NI side for 5->2 AXI-to-NoC channel mapping; reusable at
// NoC fabric router output ports.
//
// Pipeline placement:
//   NMU: Packetize{aw,w,ar} -> WormholeArbiter<NocReqOut>(3 in, {{0,1}})
//        -> VcArbiter -> NocReqOut
//   NSU: Packetize{b,r} -> WormholeArbiter<NocRspOut>(2 in, {}) -> VcArbiter
//        -> NocRspOut
//
// Lock semantic:
//   * When a flit with header.flit_tail=0 (packet start, e.g., AW) is
//     drained, lock to the SAME input by default (continue serving it across
//     the worm -- covers a multi-flit worm pre-merged onto one input, e.g.
//     wrap/dat_merge_wrap.hpp's NMU-side DataAw+DataW). A configured
//     `pairing.from == this port` overrides the default and locks to
//     `pairing.to` instead (the AW-port/W-port shape, e.g. nmu.hpp's req
//     wormhole_arbiter_: AW on port 0 locks to W's port 1).
//   * When a flit with header.flit_tail=1 (packet end, e.g., W with wlast) is
//     drained from the currently locked port, unlock.
//   * Without pairing and flit_tail always 1 (NSU case: B/R are always
//     single-flit worms), the lock branch never triggers; every flit is its
//     own packet, matching the pre-existing behavior unchanged.
//
// REQUIRES Packetize stamps header.flit_tail per FlooNoC pattern (AW=0, W=wlast,
// AR/B/R=1). Malformed AW (from-port flit with flit_tail=1) triggers assert+abort
// at runtime.
//
// Lifetime: heap-allocate via std::unique_ptr OR construct as a stable
// named member of an owning class. Do NOT push_back into a
// std::vector<WormholeArbiter> (deleted move/copy makes that a compile
// error). InputAdapter holds a raw `parent` pointer; the pointer must
// remain valid for the arbiter's lifetime.
//
// References:
//   FlooNoC hw/floo_wormhole_arbiter.sv, hw/floo_axi_chimney.sv:744 / :758

#include "flit.hpp"
#include "ni_flit_constants.h"
#include "router/req_out.hpp"
#include "router/rsp_out.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace ni::cmodel::router {

struct ChannelPairing {
    std::size_t from;
    std::size_t to;
};

template <typename Downstream>
class WormholeArbiter {
  public:
    static constexpr std::size_t MAX_INPUTS = 8;
    static constexpr std::size_t kDefaultPerInputDepth = 4;

    WormholeArbiter(Downstream& downstream, std::size_t num_inputs,
                    std::vector<ChannelPairing> pairings = {},
                    std::size_t per_input_depth = kDefaultPerInputDepth)
        : downstream_(downstream),
          num_inputs_(num_inputs),
          pairings_(std::move(pairings)),
          per_input_depth_(per_input_depth) {
        assert(num_inputs_ >= 1 && num_inputs_ <= MAX_INPUTS);
        assert(per_input_depth_ > 0);

        // Validate pairings: from/to in range, from != to, no duplicate from,
        // no nested chain (a `to` cannot also be a `from`).
        // Belt-and-braces assert + std::abort so NDEBUG release builds also
        // fail-fast (assert alone is stripped under -DNDEBUG).
        for (std::size_t i = 0; i < pairings_.size(); ++i) {
            const auto& p = pairings_[i];
            if (!(p.from < num_inputs_ && p.to < num_inputs_)) {
                assert(false && "WormholeArbiter: pairing out of range");
                std::abort();
            }
            if (p.from == p.to) {
                assert(false && "WormholeArbiter: pairing from == to");
                std::abort();
            }
            for (std::size_t j = i + 1; j < pairings_.size(); ++j) {
                if (pairings_[j].from == p.from) {
                    assert(false && "WormholeArbiter: duplicate pairing.from");
                    std::abort();
                }
            }
            for (const auto& q : pairings_) {
                if (q.from == p.to) {
                    assert(false && "WormholeArbiter: nested pairing chain (to is also a from)");
                    std::abort();
                }
            }
        }

        pending_.resize(num_inputs_);
        for (std::size_t i = 0; i < num_inputs_; ++i) {
            input_adapters_.emplace_back(this, i);
        }
    }

    WormholeArbiter(const WormholeArbiter&) = delete;
    WormholeArbiter(WormholeArbiter&&) = delete;
    WormholeArbiter& operator=(const WormholeArbiter&) = delete;
    WormholeArbiter& operator=(WormholeArbiter&&) = delete;

    Downstream& input(std::size_t idx) {
        assert(idx < num_inputs_);
        return input_adapters_[idx];
    }

    void tick();

    // Introspection (test + production use — see wrap/dat_merge_wrap.hpp,
    // which peeks the about-to-drain flit's header to attribute a per-VC
    // credit-return pulse correctly across a DPI boundary this arbiter has
    // no visibility into).
    std::size_t pending_size(std::size_t idx) const {
        assert(idx < num_inputs_);
        return pending_[idx].size();
    }
    bool is_locked() const noexcept { return locked_to_.has_value(); }
    std::optional<std::size_t> locked_to() const noexcept { return locked_to_; }
    // Front flit of input idx's pending queue, or nullopt if empty. Read-only
    // peek — does not affect tick()'s target selection or lock state.
    std::optional<Flit> peek(std::size_t idx) const {
        assert(idx < num_inputs_);
        if (pending_[idx].empty()) return std::nullopt;
        return pending_[idx].front();
    }

  private:
    struct InputAdapter : Downstream {
        WormholeArbiter* parent;
        std::size_t idx;

        InputAdapter(WormholeArbiter* p, std::size_t i) : parent(p), idx(i) {}

        bool push_flit(const Flit& f) override {
            if (parent->pending_[idx].size() >= parent->per_input_depth_) return false;
            parent->pending_[idx].push_back(f);
            return true;
        }
        bool credit_avail(uint8_t /*vc_id*/) const override {
            return parent->pending_[idx].size() < parent->per_input_depth_;
        }
    };

    bool is_from_port(std::size_t idx) const {
        for (const auto& p : pairings_)
            if (p.from == idx) return true;
        return false;
    }
    bool is_to_port(std::size_t idx) const {
        for (const auto& p : pairings_)
            if (p.to == idx) return true;
        return false;
    }

    Downstream& downstream_;
    std::size_t num_inputs_;
    std::vector<ChannelPairing> pairings_;
    std::size_t per_input_depth_;
    std::vector<std::deque<Flit>> pending_;
    std::vector<InputAdapter> input_adapters_;
    std::size_t round_robin_ptr_ = 0;
    std::optional<std::size_t> locked_to_;
};

template <typename Downstream>
inline void WormholeArbiter<Downstream>::tick() {
    std::size_t target;

    if (locked_to_.has_value()) {
        target = *locked_to_;
        if (pending_[target].empty()) return;
    } else {
        bool found = false;
        for (std::size_t k = 0; k < num_inputs_; ++k) {
            std::size_t i = (round_robin_ptr_ + k) % num_inputs_;
            if (!pending_[i].empty()) {
                target = i;
                found = true;
                break;
            }
        }
        if (!found) return;
    }

    const Flit& flit = pending_[target].front();
    uint64_t flit_tail = flit.get_header_field("flit_tail");

    // Defensive guards (header.flit_tail stamping invariant)
    if (is_from_port(target) && flit_tail == 1) {
        assert(false &&
               "WormholeArbiter::tick: from-port flit with header.flit_tail=1 -- malformed AW; "
               "Packetize must stamp header.flit_tail=0 on AW");
        std::abort();
    }
    if (is_to_port(target) && !locked_to_.has_value()) {
        assert(false &&
               "WormholeArbiter::tick: to-port flit pushed without preceding from-port flit (W "
               "before AW; upstream serialization broken)");
        std::abort();
    }

    // Pure valid/ready handshake: this arbiter only selects one request out of
    // the input queues toward the NoC -- it does not decide VC or track credit.
    // VC selection and per-VC credit gating live entirely in the downstream
    // nmu::VcArbiter. A false return is legitimate backpressure (req_out.hpp:
    // "a false return MUST be safely retried"), so retain the front flit and
    // retry next tick. Mirrors FlooNoC floo_wormhole_arbiter.sv, a pure
    // handshake with no credit logic.
    if (!downstream_.push_flit(flit)) return;

    pending_[target].pop_front();
    round_robin_ptr_ = (target + 1) % num_inputs_;

    // Lock/unlock transition. Default: lock to SELF (continue serving the
    // same input across the worm) -- covers a multi-flit worm arriving on
    // one already-merged input (S3a T5 DatMergeWrap: NMU's DataAw+DataW
    // arrive sequentially on ONE input, not two ports to pair). An explicit
    // ChannelPairing overrides this to lock a DIFFERENT port instead (the
    // AW-port/W-port shape, e.g. nmu.hpp's req wormhole_arbiter_). Router's/
    // SimpleRouter's own inline per-output lock is exactly this same
    // self-lock rule with no pairing concept at all; this generalizes
    // WormholeArbiter to the same default instead of adding a second
    // implementation of it.
    if (flit_tail == 0 && !locked_to_.has_value()) {
        locked_to_ = target;
        for (const auto& p : pairings_) {
            if (p.from == target) {
                locked_to_ = p.to;
                break;
            }
        }
    } else if (flit_tail == 1 && locked_to_.has_value()) {
        assert(*locked_to_ == target && "WormholeArbiter::tick: unlock target mismatch");
        locked_to_ = std::nullopt;
    }
}

}  // namespace ni::cmodel::router
