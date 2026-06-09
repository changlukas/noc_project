#pragma once
// NMU virtual channel arbiter. Decorator pattern over NocReqOut: receives
// packetized flits from nmu::Packetize, decides which VC each flit goes
// into (per-axi_ch mapping), enqueues into a per-VC pending queue, and
// drains to the wrapped downstream via tick() using credit-gated
// round-robin.
//
// Two modes (compile-time selected via factory):
//   Mode A (ReadWriteSplit, default): AW/W → write_vc; AR → read_vc.
//   Mode B (MultiCandidate): per-axi_ch candidate VC list; first VC with
//     pending space AND downstream credit wins.
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
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "noc/noc_req_out.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace ni::cmodel::nmu {

enum class VcMode {
    ReadWriteSplit,
    MultiCandidate,
};

class VcArbiter : public noc::NocReqOut {
  public:
    static constexpr std::size_t NUM_VC_MAX = 1u << ni::header::VC_ID_WIDTH;  // 8
    static constexpr std::size_t AXI_CH_COUNT = 5;  // AW, W, AR, B, R (B/R unused on NMU)
    static constexpr std::size_t kDefaultPendingDepth = 4;

    static VcArbiter read_write_split(noc::NocReqOut& downstream, std::size_t num_vc,
                                      uint8_t write_vc, uint8_t read_vc,
                                      std::size_t pending_depth = kDefaultPendingDepth) {
        std::array<std::vector<uint8_t>, AXI_CH_COUNT> empty_candidates{};
        return VcArbiter(downstream, num_vc, VcMode::ReadWriteSplit, write_vc, read_vc,
                         std::move(empty_candidates), pending_depth);
    }

    static VcArbiter multi_candidate(noc::NocReqOut& downstream, std::size_t num_vc,
                                     std::array<std::vector<uint8_t>, AXI_CH_COUNT> candidate_vcs,
                                     std::size_t pending_depth = kDefaultPendingDepth) {
        return VcArbiter(downstream, num_vc, VcMode::MultiCandidate,
                         /*write_vc*/ 0, /*read_vc*/ 0, std::move(candidate_vcs), pending_depth);
    }

    // NocReqOut decorator interface
    bool push_flit(const Flit& flit) override;
    bool credit_avail(uint8_t vc_id) const override;

    void tick();

    // Test introspection
    std::size_t pending_size(uint8_t vc_id) const noexcept { return pending_[vc_id].size(); }
    uint8_t round_robin_ptr() const noexcept { return round_robin_ptr_; }
    bool has_current_aw() const noexcept { return current_aw_vc_.has_value(); }

  private:
    VcArbiter(noc::NocReqOut& downstream, std::size_t num_vc, VcMode mode, uint8_t write_vc,
              uint8_t read_vc, std::array<std::vector<uint8_t>, AXI_CH_COUNT> candidate_vcs,
              std::size_t pending_depth)
        : downstream_(downstream),
          num_vc_(num_vc),
          mode_(mode),
          write_vc_(write_vc),
          read_vc_(read_vc),
          candidate_vcs_(std::move(candidate_vcs)),
          pending_depth_(pending_depth) {
        assert(num_vc_ >= 1 && num_vc_ <= NUM_VC_MAX);
        assert(write_vc_ < num_vc_);
        assert(read_vc_ < num_vc_);
    }

    std::optional<uint8_t> select_vc_for_axi_ch(uint8_t axi_ch);

    noc::NocReqOut& downstream_;
    std::size_t num_vc_;
    VcMode mode_;
    uint8_t write_vc_;
    uint8_t read_vc_;
    std::array<std::vector<uint8_t>, AXI_CH_COUNT> candidate_vcs_;
    std::array<std::deque<Flit>, NUM_VC_MAX> pending_;
    std::size_t pending_depth_;
    uint8_t round_robin_ptr_ = 0;
    std::optional<uint8_t> current_aw_vc_;
};

inline std::optional<uint8_t> VcArbiter::select_vc_for_axi_ch(uint8_t axi_ch) {
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

    if (mode_ == VcMode::ReadWriteSplit) {
        if (axi_ch == ni::AXI_CH_AW) return write_vc_;
        if (axi_ch == ni::AXI_CH_AR) return read_vc_;
        return std::nullopt;
    }

    // Mode B: MultiCandidate
    for (uint8_t vc : candidate_vcs_[axi_ch]) {
        if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) {
            return vc;
        }
    }
    return std::nullopt;
}

inline bool VcArbiter::push_flit(const Flit& flit) {
    uint8_t axi_ch = static_cast<uint8_t>(flit.get_header_field("axi_ch"));
    auto vc_opt = select_vc_for_axi_ch(axi_ch);
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
