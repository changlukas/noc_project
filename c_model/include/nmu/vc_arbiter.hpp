#pragma once
// NMU virtual channel arbiter. Decorator pattern over NocReqOut: receives
// packetized flits from nmu::Packetize, decides which VC each flit goes
// into (per-axi_ch mapping), enqueues into a per-VC pending queue, and
// drains to the wrapped downstream via tick() using credit-gated
// round-robin.
//
// Selection is gated first by a sticky (channel, axi_id) -> vc binding: a
// bound id always returns its assigned VC, even when that VC is full (it
// backpressures on its own VC rather than switching). Only UNBOUND ids fall
// through to the candidate scan below. The binding is released by
// on_id_drained (a later task / the Rob drain hook).
//
// ReadWriteSplit (only mode): candidate set is derived per direction —
//   AW → write_vcs_, AR → read_vcs_. First VC in the candidate set with
//   pending space AND downstream credit wins (else backpressure).
//
// W-follows-AW invariant (Constraint A1): this arbiter MUST be downstream
// of a WormholeArbiter that serializes AW and all its W beats before
// admitting the next AW. Given that guarantee, a single
// std::optional<uint8_t> current_aw_vc_ is sufficient to track the in-flight
// burst's VC (push on AW, reset on W with payload.W.wlast=1).
//
// NUM_VC=1 degenerate behavior: both modes route everything to VC=0 and
// are observationally identical to the prior-round single-VC pipeline.
//
// References:
//   docs/superpowers/specs/2026-06-03-vc-arb-multi-mode-design.md
//   FlooNoC floo_wormhole_arbiter.sv (output-port wormhole lock)
//   FlooNoC floo_vc_arbiter.sv (VC arbiter without wormhole lock)
//   gem5 Garnet OutputUnit::has_credit / OutVcState::m_credit_count
#include "axi/types.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "router/req_out.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace ni::cmodel::nmu {

class VcArbiter : public router::NocReqOut {
  public:
    static constexpr std::size_t NUM_VC_MAX = 1u << ni::header::VC_ID_WIDTH;  // 8
    static constexpr std::size_t AXI_CH_COUNT = 5;  // AW, W, AR, B, R (B/R unused on NMU)
    static constexpr std::size_t kDefaultPendingDepth = 4;

    static VcArbiter read_write_split(router::NocReqOut& downstream, std::size_t num_vc,
                                      uint8_t write_vc, uint8_t read_vc,
                                      std::size_t pending_depth = kDefaultPendingDepth) {
        return VcArbiter(downstream, num_vc, std::vector<uint8_t>{write_vc},
                         std::vector<uint8_t>{read_vc}, pending_depth);
    }

    static VcArbiter read_write_split_pools(router::NocReqOut& downstream, std::size_t num_vc,
                                            std::vector<uint8_t> write_vcs,
                                            std::vector<uint8_t> read_vcs,
                                            std::size_t pending_depth = kDefaultPendingDepth) {
        return VcArbiter(downstream, num_vc, std::move(write_vcs), std::move(read_vcs),
                         pending_depth);
    }

    // NocReqOut decorator interface
    bool push_flit(const Flit& flit) override;
    bool credit_avail(uint8_t vc_id) const override;

    void tick();

    // Release the (class,id) binding when its outstanding count reaches 0.
    // Called by the NMU Rob drain hook. is_write selects the write vs read table.
    void on_id_drained(bool is_write, uint8_t id) {
        if (is_write)
            write_binding_[id].reset();
        else
            read_binding_[id].reset();
    }

    // Test introspection
    std::size_t pending_size(uint8_t vc_id) const noexcept { return pending_[vc_id].size(); }
    uint8_t round_robin_ptr() const noexcept { return round_robin_ptr_; }
    bool has_current_aw() const noexcept { return current_aw_vc_.has_value(); }

  private:
    VcArbiter(router::NocReqOut& downstream, std::size_t num_vc, std::vector<uint8_t> write_vcs,
              std::vector<uint8_t> read_vcs, std::size_t pending_depth)
        : downstream_(downstream),
          num_vc_(num_vc),
          write_vcs_(std::move(write_vcs)),
          read_vcs_(std::move(read_vcs)),
          pending_depth_(pending_depth) {
        assert(num_vc_ >= 1 && num_vc_ <= NUM_VC_MAX);
        for (uint8_t v : write_vcs_) assert(v < num_vc_);
        for (uint8_t v : read_vcs_) assert(v < num_vc_);
    }

    std::optional<uint8_t> select_vc_for_axi_ch(uint8_t axi_ch, uint8_t id);

    const std::vector<uint8_t>* candidates_for(uint8_t axi_ch) const {
        return axi_ch == ni::AXI_CH_AW ? &write_vcs_ : &read_vcs_;
    }

    router::NocReqOut& downstream_;
    std::size_t num_vc_;
    std::vector<uint8_t> write_vcs_;
    std::vector<uint8_t> read_vcs_;
    std::array<std::deque<Flit>, NUM_VC_MAX> pending_;
    std::size_t pending_depth_;
    uint8_t round_robin_ptr_ = 0;
    uint8_t write_rr_start_ = 0;  // per-class round-robin scan start (selection)
    uint8_t read_rr_start_ = 0;
    std::optional<uint8_t> current_aw_vc_;
    static constexpr std::size_t AXI_ID_SPACE = axi::AXI_ID_SPACE;  // 256; single source of truth
    std::array<std::optional<uint8_t>, AXI_ID_SPACE> write_binding_{};
    std::array<std::optional<uint8_t>, AXI_ID_SPACE> read_binding_{};
};

inline std::optional<uint8_t> VcArbiter::select_vc_for_axi_ch(uint8_t axi_ch, uint8_t id) {
    // W invariant fires regardless of NUM_VC (Constraint A1: must be
    // downstream of WormholeArbiter; W must always follow AW)
    if (axi_ch == ni::AXI_CH_W) {
        if (!current_aw_vc_.has_value()) {
            assert(false &&
                   "nmu::VcArbiter::push_flit: W arrived with no current AW VC -- "
                   "Constraint A1 violated: must be downstream of WormholeArbiter "
                   "(which serializes AW + all W beats before next AW). Standalone "
                   "VcArbiter use without upstream serialization is unsupported.");
            std::abort();
        }
        return *current_aw_vc_;
    }

    if (num_vc_ == 1) return uint8_t{0};

    std::array<std::optional<uint8_t>, AXI_ID_SPACE>* binding = nullptr;
    if (axi_ch == ni::AXI_CH_AW)
        binding = &write_binding_;
    else if (axi_ch == ni::AXI_CH_AR)
        binding = &read_binding_;
    else
        return std::nullopt;
    if ((*binding)[id].has_value()) return (*binding)[id];  // bound: stick (even if full)
    const std::vector<uint8_t>* cand = candidates_for(axi_ch);
    uint8_t& rr = (axi_ch == ni::AXI_CH_AW) ? write_rr_start_ : read_rr_start_;
    const std::size_t n = cand->size();
    for (std::size_t k = 0; k < n; ++k) {  // unbound: round-robin from rr, first available
        uint8_t vc = (*cand)[(rr + k) % n];
        if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) {
            rr = static_cast<uint8_t>((static_cast<std::size_t>(rr) + k + 1) % n);
            return vc;
        }
    }
    return std::nullopt;
}

inline bool VcArbiter::push_flit(const Flit& flit) {
    uint8_t axi_ch = static_cast<uint8_t>(flit.get_header_field("axi_ch"));

    // Read the per-flow id (only when binding is active, i.e. num_vc_>1; spec §8).
    uint8_t id = 0;
    if (num_vc_ > 1) {
        if (axi_ch == ni::AXI_CH_AW)
            id = static_cast<uint8_t>(flit.get_payload_field("AW", "awid"));
        else if (axi_ch == ni::AXI_CH_AR)
            id = static_cast<uint8_t>(flit.get_payload_field("AR", "arid"));
    }

    auto vc_opt = select_vc_for_axi_ch(axi_ch, id);
    if (!vc_opt.has_value()) return false;
    uint8_t vc_id = *vc_opt;
    if (pending_[vc_id].size() >= pending_depth_) return false;

    // Update W-follows-AW optional only after pass conditions (atomicity)
    if (axi_ch == ni::AXI_CH_AW) {
        if (current_aw_vc_.has_value()) {
            assert(false &&
                   "nmu::VcArbiter::push_flit: AW arrived while previous AW's W burst "
                   "still in progress -- Constraint A1 violated: must be downstream of "
                   "WormholeArbiter (which holds next AW until current W burst ends).");
            std::abort();  // belt-and-braces for NDEBUG
        }
        current_aw_vc_ = vc_id;
    } else if (axi_ch == ni::AXI_CH_W) {
        if (flit.get_payload_field("W", "wlast") != 0) {
            current_aw_vc_.reset();
        }
    }

    // Commit the (channel, id) -> vc binding only after every accept condition
    // (including the AW-collision check above) has passed, so a malformed-AW
    // abort cannot leave a committed binding. Active for num_vc_>1 only.
    if (num_vc_ > 1) {
        if (axi_ch == ni::AXI_CH_AW)
            write_binding_[id] = vc_id;
        else if (axi_ch == ni::AXI_CH_AR)
            read_binding_[id] = vc_id;
    }

    Flit stamped = flit;
    stamped.set_header_field("vc_id", vc_id);
    pending_[vc_id].push_back(stamped);
    return true;
}

inline bool VcArbiter::credit_avail(uint8_t vc_id) const {
    return pending_[vc_id].size() < pending_depth_;
}

inline void VcArbiter::tick() {
    for (std::size_t k = 0; k < num_vc_; ++k) {
        uint8_t vc = static_cast<uint8_t>((round_robin_ptr_ + k) % num_vc_);
        if (!pending_[vc].empty() && downstream_.credit_avail(vc)) {
            bool ok = downstream_.push_flit(pending_[vc].front());
            assert(ok &&
                   "nmu::VcArbiter::tick: downstream returned credit_avail=true "
                   "but push_flit refused -- protocol violation, downstream "
                   "must not lie about credit availability");
            if (!ok) std::abort();  // belt-and-braces for NDEBUG
            pending_[vc].pop_front();
            round_robin_ptr_ = static_cast<uint8_t>((vc + 1) % num_vc_);
            return;
        }
    }
}

}  // namespace ni::cmodel::nmu
