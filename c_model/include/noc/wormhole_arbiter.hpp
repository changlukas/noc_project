#pragma once
// Generic N-to-1 wormhole arbiter for c_model NoC behavior model.
//
// Inherits FlooNoC's wormhole locking semantic (hw/floo_wormhole_arbiter.sv
// LockIn=1, release on `last_out & ready_i`) and collapses FlooNoC's separate
// SelW state machine into a single arbiter via the optional ChannelPairing
// config. Used at NI side for 5->2 AXI-to-NoC channel mapping; reusable at
// NoC fabric router output ports (future Stage 4 round).
//
// Pipeline placement:
//   NMU: Packetize{aw,w,ar} -> WormholeArbiter<NocReqOut>(3 in, {{0,1}})
//        -> VcArbiter -> NocReqOut
//   NSU: Packetize{b,r} -> WormholeArbiter<NocRspOut>(2 in, {}) -> VcArbiter
//        -> NocRspOut
//
// Lock semantic:
//   * When a flit with header.last=0 (packet start, e.g., AW) is drained
//     from a `pairing.from` port, lock to the corresponding `pairing.to`
//     port (e.g., w_in). Only the `to` port is serviceable until released.
//   * When a flit with header.last=1 (packet end, e.g., W with wlast) is
//     drained from the currently locked `to` port, unlock.
//   * Without pairing (NSU case), every flit is its own packet; no lock.
//
// Constraint A2 (from spec): REQUIRES Packetize stamps header.last per
// FlooNoC pattern (AW=0, W=wlast, AR/B/R=1). Malformed AW (from-port flit
// with last=1) triggers assert+abort at runtime.
//
// Lifetime: heap-allocate via std::unique_ptr OR construct as a stable
// named member of an owning class. Do NOT push_back into a
// std::vector<WormholeArbiter> (deleted move/copy makes that a compile
// error). InputAdapter holds a raw `parent` pointer; the pointer must
// remain valid for the arbiter's lifetime.
//
// References:
//   docs/superpowers/specs/2026-06-04-wormhole-arbiter-design.md
//   FlooNoC hw/floo_wormhole_arbiter.sv, hw/floo_axi_chimney.sv:744 / :758

#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include "noc/noc_req_out.hpp"
#include "noc/noc_rsp_out.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace ni::cmodel::noc {

struct ChannelPairing {
    std::size_t from;
    std::size_t to;
};

template <typename Downstream>
class WormholeArbiter {
public:
    static constexpr std::size_t MAX_INPUTS = 8;
    static constexpr std::size_t kDefaultPerInputDepth = 4;

    WormholeArbiter(Downstream& downstream,
                    std::size_t num_inputs,
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
        for (std::size_t i = 0; i < pairings_.size(); ++i) {
            const auto& p = pairings_[i];
            assert(p.from < num_inputs_ && p.to < num_inputs_ &&
                   "WormholeArbiter: pairing out of range");
            assert(p.from != p.to &&
                   "WormholeArbiter: pairing from == to");
            for (std::size_t j = i + 1; j < pairings_.size(); ++j) {
                assert(pairings_[j].from != p.from &&
                       "WormholeArbiter: duplicate pairing.from");
            }
            for (const auto& q : pairings_) {
                assert(!(q.from == p.to) &&
                       "WormholeArbiter: nested pairing chain (to is also a from)");
            }
        }

        pending_.resize(num_inputs_);
        for (std::size_t i = 0; i < num_inputs_; ++i) {
            input_adapters_.emplace_back(this, i);
        }
    }

    WormholeArbiter(const WormholeArbiter&) = delete;
    WormholeArbiter(WormholeArbiter&&)      = delete;
    WormholeArbiter& operator=(const WormholeArbiter&) = delete;
    WormholeArbiter& operator=(WormholeArbiter&&)      = delete;

    Downstream& input(std::size_t idx) {
        assert(idx < num_inputs_);
        return input_adapters_[idx];
    }

    void tick();

    // Test introspection
    std::size_t pending_size(std::size_t idx) const {
        assert(idx < num_inputs_);
        return pending_[idx].size();
    }
    bool is_locked() const noexcept { return locked_to_.has_value(); }
    std::optional<std::size_t> locked_to() const noexcept { return locked_to_; }

private:
    struct InputAdapter : Downstream {
        WormholeArbiter* parent;
        std::size_t      idx;

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
        for (const auto& p : pairings_) if (p.from == idx) return true;
        return false;
    }
    bool is_to_port(std::size_t idx) const {
        for (const auto& p : pairings_) if (p.to == idx) return true;
        return false;
    }

    Downstream&                  downstream_;
    std::size_t                  num_inputs_;
    std::vector<ChannelPairing>  pairings_;
    std::size_t                  per_input_depth_;
    std::vector<std::deque<Flit>> pending_;
    std::vector<InputAdapter>    input_adapters_;
    std::size_t                  round_robin_ptr_ = 0;
    std::optional<std::size_t>   locked_to_;
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
    uint8_t vc_id = static_cast<uint8_t>(flit.get_header_field("vc_id"));
    if (!downstream_.credit_avail(vc_id)) return;

    uint64_t last = flit.get_header_field("last");

    // Defensive guards (Constraint A2)
    if (is_from_port(target) && last == 1) {
        assert(false && "WormholeArbiter::tick: from-port flit with header.last=1 -- malformed AW (Packetize regression on header.last stamping; vc_arb round commit 1f82ba8 stamps AW=0)");
        std::abort();
    }
    if (is_to_port(target) && !locked_to_.has_value()) {
        assert(false && "WormholeArbiter::tick: to-port flit pushed without preceding from-port flit (W before AW; upstream serialization broken)");
        std::abort();
    }

    bool ok = downstream_.push_flit(flit);
    assert(ok && "WormholeArbiter::tick: lying downstream (credit_avail=true but push_flit refused)");
    if (!ok) std::abort();  // belt-and-braces for NDEBUG

    pending_[target].pop_front();
    round_robin_ptr_ = (target + 1) % num_inputs_;

    // Lock/unlock transition
    if (last == 0 && !locked_to_.has_value()) {
        for (const auto& p : pairings_) {
            if (p.from == target) {
                locked_to_ = p.to;
                break;
            }
        }
    } else if (last == 1 && locked_to_.has_value()) {
        assert(*locked_to_ == target && "WormholeArbiter::tick: unlock target mismatch");
        locked_to_ = std::nullopt;
    }
}

}  // namespace ni::cmodel::noc
