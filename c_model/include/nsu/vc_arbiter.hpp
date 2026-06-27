#pragma once
// NSU virtual channel arbiter. Mirror of nmu::VcArbiter but for response
// side (B + R flits leaving NSU toward NMU). No W-follows-AW logic
// because NSU produces single-flit B (`floo_axi_chimney.sv:608-616`)
// and multi-flit R uses ROB not wormhole (`floo_axi_chimney.sv:624-633`).
//
// Two modes parallel nmu::VcArbiter:
//   Mode A (ReadWriteSplit, default): B -> write_rsp_vc; R -> read_rsp_vc.
//   Mode B (MultiCandidate): per-axi_ch candidate VC list.
//
// NUM_VC=1 degenerate behavior: both modes route everything to VC=0 and
// are observationally identical to the prior-round single-VC pipeline.
//
// References:
//   docs/superpowers/specs/2026-06-03-vc-arb-multi-mode-design.md §7.2
#include "axi/types.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "router/rsp_out.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace ni::cmodel::nsu {

enum class VcMode {
    ReadWriteSplit,
    MultiCandidate,
};

class VcArbiter : public router::NocRspOut {
  public:
    static constexpr std::size_t NUM_VC_MAX = 1u << ni::header::VC_ID_WIDTH;  // 8
    static constexpr std::size_t AXI_CH_COUNT = 5;  // AW, W, AR, B, R (AW/W/AR unused on NSU)
    static constexpr std::size_t kDefaultPendingDepth = 4;

    static VcArbiter read_write_split(router::NocRspOut& downstream, std::size_t num_vc,
                                      uint8_t write_rsp_vc, uint8_t read_rsp_vc,
                                      std::size_t pending_depth = kDefaultPendingDepth) {
        std::array<std::vector<uint8_t>, AXI_CH_COUNT> empty_candidates{};
        return VcArbiter(downstream, num_vc, VcMode::ReadWriteSplit, write_rsp_vc, read_rsp_vc,
                         std::move(empty_candidates), pending_depth);
    }

    static VcArbiter read_write_split_pools(router::NocRspOut& downstream, std::size_t num_vc,
                                            std::vector<uint8_t> write_rsp_vcs,
                                            std::vector<uint8_t> read_rsp_vcs,
                                            std::size_t pending_depth = kDefaultPendingDepth) {
        std::array<std::vector<uint8_t>, AXI_CH_COUNT> empty_candidates{};
        VcArbiter a(downstream, num_vc, VcMode::ReadWriteSplit, /*write_rsp_vc=*/0,
                    /*read_rsp_vc=*/0, std::move(empty_candidates), pending_depth);
        a.write_rsp_vcs_ = std::move(write_rsp_vcs);
        a.read_rsp_vcs_ = std::move(read_rsp_vcs);
        a.use_pools_ = true;
        return a;
    }

    static VcArbiter multi_candidate(router::NocRspOut& downstream, std::size_t num_vc,
                                     std::array<std::vector<uint8_t>, AXI_CH_COUNT> candidate_vcs,
                                     std::size_t pending_depth = kDefaultPendingDepth) {
        return VcArbiter(downstream, num_vc, VcMode::MultiCandidate,
                         /*write_rsp_vc*/ 0, /*read_rsp_vc*/ 0, std::move(candidate_vcs),
                         pending_depth);
    }

    // NocRspOut decorator interface
    bool push_flit(const Flit& flit) override;
    bool credit_avail(uint8_t vc_id) const override;

    void tick();

    // Test introspection
    std::size_t pending_size(uint8_t vc_id) const noexcept { return pending_[vc_id].size(); }
    uint8_t round_robin_ptr() const noexcept { return round_robin_ptr_; }

  private:
    VcArbiter(router::NocRspOut& downstream, std::size_t num_vc, VcMode mode, uint8_t write_rsp_vc,
              uint8_t read_rsp_vc, std::array<std::vector<uint8_t>, AXI_CH_COUNT> candidate_vcs,
              std::size_t pending_depth)
        : downstream_(downstream),
          num_vc_(num_vc),
          mode_(mode),
          write_rsp_vc_(write_rsp_vc),
          read_rsp_vc_(read_rsp_vc),
          candidate_vcs_(std::move(candidate_vcs)),
          pending_depth_(pending_depth) {
        assert(num_vc_ >= 1 && num_vc_ <= NUM_VC_MAX);
        assert(write_rsp_vc_ < num_vc_);
        assert(read_rsp_vc_ < num_vc_);
    }

    std::optional<uint8_t> select_vc_for_axi_ch(uint8_t axi_ch, uint8_t id);

    router::NocRspOut& downstream_;
    std::size_t num_vc_;
    VcMode mode_;
    uint8_t write_rsp_vc_;
    uint8_t read_rsp_vc_;
    std::array<std::vector<uint8_t>, AXI_CH_COUNT> candidate_vcs_;
    std::array<std::deque<Flit>, NUM_VC_MAX> pending_;
    std::size_t pending_depth_;
    uint8_t round_robin_ptr_ = 0;
    std::vector<uint8_t> write_rsp_vcs_;
    std::vector<uint8_t> read_rsp_vcs_;
    bool use_pools_ = false;
    uint8_t write_rr_start_ = 0;
    uint8_t read_rr_start_ = 0;
    static constexpr std::size_t AXI_ID_SPACE = axi::AXI_ID_SPACE;  // 256
    std::array<std::optional<uint8_t>, AXI_ID_SPACE> write_binding_{};
    std::array<std::optional<uint8_t>, AXI_ID_SPACE> read_binding_{};
};

inline std::optional<uint8_t> VcArbiter::select_vc_for_axi_ch(uint8_t axi_ch, uint8_t id) {
    if (num_vc_ == 1) return uint8_t{0};

    if (mode_ == VcMode::MultiCandidate) {
        for (uint8_t vc : candidate_vcs_[axi_ch]) {
            if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) return vc;
        }
        return std::nullopt;
    }

    // ReadWriteSplit, scalar (no pools configured).
    if (!use_pools_) {
        if (axi_ch == ni::AXI_CH_B) return write_rsp_vc_;
        if (axi_ch == ni::AXI_CH_R) return read_rsp_vc_;
        return std::nullopt;
    }

    // ReadWriteSplit pools: per-id sticky binding + round-robin within the class pool.
    std::array<std::optional<uint8_t>, AXI_ID_SPACE>* binding = nullptr;
    const std::vector<uint8_t>* cand = nullptr;
    uint8_t* rr = nullptr;
    if (axi_ch == ni::AXI_CH_B) {
        binding = &write_binding_;
        cand = &write_rsp_vcs_;
        rr = &write_rr_start_;
    } else if (axi_ch == ni::AXI_CH_R) {
        binding = &read_binding_;
        cand = &read_rsp_vcs_;
        rr = &read_rr_start_;
    } else {
        return std::nullopt;
    }
    if ((*binding)[id].has_value()) return (*binding)[id];  // bound: stick
    const std::size_t n = cand->size();
    for (std::size_t k = 0; k < n; ++k) {  // unbound: round-robin from rr
        uint8_t vc = (*cand)[(*rr + k) % n];
        if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) {
            *rr = static_cast<uint8_t>((static_cast<std::size_t>(*rr) + k + 1) % n);
            return vc;
        }
    }
    return std::nullopt;
}

inline bool VcArbiter::push_flit(const Flit& flit) {
    uint8_t axi_ch = static_cast<uint8_t>(flit.get_header_field("axi_ch"));
    uint8_t id = 0;
    if (use_pools_ && num_vc_ > 1) {
        if (axi_ch == ni::AXI_CH_B)
            id = static_cast<uint8_t>(flit.get_payload_field("B", "bid"));
        else if (axi_ch == ni::AXI_CH_R)
            id = static_cast<uint8_t>(flit.get_payload_field("R", "rid"));
    }
    auto vc_opt = select_vc_for_axi_ch(axi_ch, id);
    if (!vc_opt.has_value()) return false;
    uint8_t vc_id = *vc_opt;
    if (pending_[vc_id].size() >= pending_depth_) return false;

    // Commit the (class, id) -> vc binding after all accept conditions pass.
    if (use_pools_ && num_vc_ > 1) {
        if (axi_ch == ni::AXI_CH_B)
            write_binding_[id] = vc_id;
        else if (axi_ch == ni::AXI_CH_R)
            read_binding_[id] = vc_id;
    }

    Flit stamped = flit;
    stamped.set_header_field("vc_id", vc_id);
    pending_[vc_id].push_back(stamped);

    // Release the binding on the burst's terminal flit (payload R.rlast, or the
    // single-flit B) so the next same-id burst rebinds via round-robin. Header
    // `last` is always 1, so the payload field must be used for R.
    if (use_pools_ && num_vc_ > 1) {
        if (axi_ch == ni::AXI_CH_R) {
            if (flit.get_payload_field("R", "rlast") != 0) read_binding_[id].reset();
        } else if (axi_ch == ni::AXI_CH_B) {
            write_binding_[id].reset();
        }
    }
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
                   "nsu::VcArbiter::tick: downstream returned credit_avail=true "
                   "but push_flit refused -- protocol violation, downstream "
                   "must not lie about credit availability");
            if (!ok) std::abort();  // belt-and-braces for NDEBUG
            pending_[vc].pop_front();
            round_robin_ptr_ = static_cast<uint8_t>((vc + 1) % num_vc_);
            return;
        }
    }
}

}  // namespace ni::cmodel::nsu
