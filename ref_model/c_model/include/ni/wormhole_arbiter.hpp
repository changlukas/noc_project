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
//        -> VcAllocator -> NocReqOut
//   NSU: Packetize{b,r} -> WormholeArbiter<NocRspOut>(2 in, {}) -> VcAllocator
//        -> NocRspOut
//
// Lock semantic — PER VC (worm contiguity is a per-(link, VC) invariant;
// cross-VC interleave on the shared link is what VCs exist for):
//   * When a flit with header.flit_tail=0 (packet start, e.g., AW) is
//     drained, its header vc_id becomes OWNED by the same input by default
//     (continue serving that worm's VC from it -- covers a multi-flit worm
//     pre-merged onto one input, e.g. wrap/dat_merge_wrap.hpp's NMU-side
//     DataAw+DataW). A configured `pairing.from == this port` overrides the
//     default and hands the VC to `pairing.to` instead (the AW-port/W-port
//     shape, e.g. nmu.hpp's req wormhole_arbiter_: AW on port 0 locks to
//     W's port 1).
//   * When a flit with header.flit_tail=1 (packet end, e.g., W with wlast)
//     is drained on an owned VC, that VC is released. A tail on a DIFFERENT
//     VC releases nothing -- an input that legally interleaves worms across
//     VCs (nmu::VcAllocator's cross-VC round-robin drain) must not have one
//     worm's tail unlock another worm's VC and let the other input's flit
//     (NSU's single-flit DataR, hashed onto the same VC) split that worm
//     (2026-08-21 uniform_random burst root cause).
//   * An input whose front flit's VC is owned by ANOTHER input is skipped;
//     fronts on unowned or self-owned VCs stay eligible.
//   * With every flit on vc 0 (the single-VC faces: NMU REQ / DAT pre-merge,
//     NSU RSP) this degenerates to the previous stream-level lock exactly.
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

    // One grant per tick. Returns the (input index, flit) actually drained, or
    // nullopt when nothing drained — wrap/dat_merge_wrap.hpp attributes its
    // per-VC credit-return pulse from this across a DPI boundary the arbiter
    // has no visibility into.
    std::optional<std::pair<std::size_t, Flit>> tick();

    // Introspection: total pending flits for input idx, summed across VCs.
    std::size_t pending_size(std::size_t idx) const {
        assert(idx < num_inputs_);
        std::size_t total = 0;
        for (const auto& q : pending_[idx]) total += q.size();
        return total;
    }
    bool is_locked(uint8_t vc = 0) const noexcept { return vc_owner_[vc].has_value(); }
    std::optional<std::size_t> locked_to(uint8_t vc = 0) const noexcept { return vc_owner_[vc]; }

  private:
    // Per (input, VC) pending queues. One shared queue per input would let a
    // front flit blocked on ITS VC's downstream credit hold every other VC's
    // flits behind it — with the DAT router's whole-output wormhole lock on
    // the other side of that credit loop, exactly the cross-VC cycle that
    // deadlocked mode-1 co-sim (2026-08-21). VCs are independently
    // flow-controlled end to end everywhere else (per-VC credit, per-VC
    // router FIFOs, per-VC NI allocator queues); this stage matches.
    struct InputAdapter : Downstream {
        WormholeArbiter* parent;
        std::size_t idx;

        InputAdapter(WormholeArbiter* p, std::size_t i) : parent(p), idx(i) {}

        bool push_flit(const Flit& f) override {
            const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
            auto& q = parent->pending_[idx][vc];
            if (q.size() >= parent->per_input_depth_) return false;
            q.push_back(f);
            return true;
        }
        bool credit_avail(uint8_t vc_id) const override {
            return parent->pending_[idx][vc_id].size() < parent->per_input_depth_;
        }
    };

    bool try_grant_(std::size_t target, uint8_t vc);

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

    // Worm ownership per VC (vc_id is a 3 b header field): which input, if
    // any, holds this VC mid-worm. nullopt = free.
    static constexpr std::size_t kNumVcSlots = 1u << ni::header::VC_ID_WIDTH;

    Downstream& downstream_;
    std::size_t num_inputs_;
    std::vector<ChannelPairing> pairings_;
    std::size_t per_input_depth_;  // per (input, VC) queue depth
    std::vector<std::array<std::deque<Flit>, kNumVcSlots>> pending_;
    std::vector<InputAdapter> input_adapters_;
    std::size_t round_robin_ptr_ = 0;
    std::size_t vc_rr_ = 0;  // VC scan start, advanced with each grant
    std::array<std::optional<std::size_t>, kNumVcSlots> vc_owner_{};
    Flit drained_flit_;  // last granted flit, copied out by tick()
};

template <typename Downstream>
inline std::optional<std::pair<std::size_t, Flit>> WormholeArbiter<Downstream>::tick() {
    // Eligibility scan (per-VC lock, see file header) over (input, VC) queue
    // fronts, round-robin over both dimensions. A front is skipped when its
    // VC is mid-worm from ANOTHER input. A candidate whose downstream push
    // is refused (per-VC credit, queue full) is skipped too and the scan
    // CONTINUES — giving up on the first refused candidate would retry it
    // forever (round-robin only advances on a grant) while a grantable
    // candidate starves behind it; with the refused VC's credit waiting on
    // fabric progress that needs the starved candidate, that is the mode-1
    // livelock the 2026-08-21 watchdog dump caught. At most one grant per
    // tick either way.
    std::size_t target = 0;
    uint8_t vc = 0;
    bool granted = false;
    for (std::size_t k = 0; k < num_inputs_ && !granted; ++k) {
        const std::size_t i = (round_robin_ptr_ + k) % num_inputs_;
        for (std::size_t kv = 0; kv < kNumVcSlots; ++kv) {
            const auto v = static_cast<uint8_t>((vc_rr_ + kv) % kNumVcSlots);
            if (pending_[i][v].empty()) continue;
            if (vc_owner_[v].has_value() && *vc_owner_[v] != i) continue;
            // A to-port (paired W) front is a candidate only once its AW's
            // grant handed it the VC: an AW refused downstream leaves the VC
            // unowned, and the work-conserving scan must step PAST its W --
            // but only while that AW is actually queued. A W with no AW
            // anywhere is broken upstream serialization; parking it silently
            // would trade the old loud abort for a wedge.
            if (is_to_port(i) && !vc_owner_[v].has_value()) {
                bool aw_queued = false;
                for (const auto& p : pairings_) {
                    if (p.to == i && !pending_[p.from][v].empty()) {
                        aw_queued = true;
                        break;
                    }
                }
                if (!aw_queued) {
                    assert(false &&
                           "WormholeArbiter::tick: to-port flit pending with no from-port flit "
                           "queued and no VC ownership (W before AW; upstream serialization "
                           "broken)");
                    std::abort();
                }
                continue;
            }
            target = i;
            vc = v;
            granted = try_grant_(target, vc);
            if (granted) break;
        }
    }
    if (!granted) return std::nullopt;

    const std::pair<std::size_t, Flit> drained{target, drained_flit_};
    return drained;
}

// One candidate attempt: protocol guards, downstream push, and on success the
// pop + round-robin + per-VC ownership bookkeeping. Returns false on a
// downstream refusal (legitimate backpressure, req_out.hpp: "a false return
// MUST be safely retried") so tick()'s scan can move to the next candidate.
template <typename Downstream>
inline bool WormholeArbiter<Downstream>::try_grant_(std::size_t target, uint8_t vc) {
    const Flit& flit = pending_[target][vc].front();
    uint64_t flit_tail = flit.get_header_field("flit_tail");

    // Defensive guards (header.flit_tail stamping invariant)
    if (is_from_port(target) && flit_tail == 1) {
        assert(false &&
               "WormholeArbiter::tick: from-port flit with header.flit_tail=1 -- malformed AW; "
               "Packetize must stamp header.flit_tail=0 on AW");
        std::abort();
    }
    if (is_to_port(target) && !vc_owner_[vc].has_value()) {
        assert(false &&
               "WormholeArbiter::tick: to-port flit pushed without preceding from-port flit (W "
               "before AW; upstream serialization broken)");
        std::abort();
    }

    // Pure valid/ready handshake toward the downstream (VC selection and
    // credit gating live there). Refusal -> false, the flit stays queued.
    if (!downstream_.push_flit(flit)) return false;

    drained_flit_ = flit;
    pending_[target][vc].pop_front();
    round_robin_ptr_ = (target + 1) % num_inputs_;
    vc_rr_ = (static_cast<std::size_t>(vc) + 1) % kNumVcSlots;

    // Ownership transition, per VC. Default: the worm's VC stays with SELF
    // (continue serving the same input across the worm) -- covers a
    // multi-flit worm arriving on one already-merged input (S3a T5
    // DatMergeWrap: NMU's DataAw+DataW arrive sequentially on ONE input, not
    // two ports to pair). An explicit ChannelPairing hands the VC to a
    // DIFFERENT port instead (the AW-port/W-port shape, e.g. nmu.hpp's req
    // wormhole_arbiter_). A tail releases only ITS OWN VC -- another worm's
    // tail from the same input must not (file header, per-VC rule).
    if (flit_tail == 0 && !vc_owner_[vc].has_value()) {
        vc_owner_[vc] = target;
        for (const auto& p : pairings_) {
            if (p.from == target) {
                vc_owner_[vc] = p.to;
                break;
            }
        }
    } else if (flit_tail == 1 && vc_owner_[vc].has_value()) {
        assert(*vc_owner_[vc] == target && "WormholeArbiter::tick: unlock target mismatch");
        vc_owner_[vc] = std::nullopt;
    }
    return true;
}

}  // namespace ni::cmodel::router
