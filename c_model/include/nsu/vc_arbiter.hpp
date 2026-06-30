#pragma once
// NSU virtual channel arbiter. Mirror of nmu::VcArbiter but for response
// side (B + R flits leaving NSU toward NMU). No W-follows-AW logic
// because NSU produces single-flit B (`floo_axi_chimney.sv:608-616`)
// and multi-flit R uses ROB not wormhole (`floo_axi_chimney.sv:624-633`).
//
// ReadWriteSplit (only mode): B -> write_rsp_vc; R -> read_rsp_vc.
// Pools mode: B round-robins write_rsp_vcs_ (no per-id pin; B is single-flit).
//             R: first beat of an rid round-robins a VC; later beats of that
//             rid reuse it; released on payload rlast. The id is a
//             burst-grouping key, not a VC selector. See r_burst_vc_.
// NUM_VC=1 degenerate behavior: routes everything to VC=0.
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

class VcArbiter : public router::NocRspOut {
  public:
    static constexpr std::size_t NUM_VC_MAX = 1u << ni::header::VC_ID_WIDTH;  // 8
    static constexpr std::size_t kDefaultPendingDepth = 4;

    static VcArbiter read_write_split(router::NocRspOut& downstream, std::size_t num_vc,
                                      uint8_t write_rsp_vc, uint8_t read_rsp_vc,
                                      std::size_t pending_depth = kDefaultPendingDepth) {
        return VcArbiter(downstream, num_vc, write_rsp_vc, read_rsp_vc, pending_depth);
    }

    static VcArbiter read_write_split_pools(router::NocRspOut& downstream, std::size_t num_vc,
                                            std::vector<uint8_t> write_rsp_vcs,
                                            std::vector<uint8_t> read_rsp_vcs,
                                            std::size_t pending_depth = kDefaultPendingDepth) {
        VcArbiter a(downstream, num_vc, /*write_rsp_vc=*/0, /*read_rsp_vc=*/0, pending_depth);
        a.write_rsp_vcs_ = std::move(write_rsp_vcs);
        a.read_rsp_vcs_ = std::move(read_rsp_vcs);
        for (uint8_t v : a.write_rsp_vcs_) assert(v < num_vc && "write_rsp_vcs element >= num_vc");
        for (uint8_t v : a.read_rsp_vcs_) assert(v < num_vc && "read_rsp_vcs element >= num_vc");
        a.use_pools_ = true;
        return a;
    }

    // NocRspOut decorator interface
    bool push_flit(const Flit& flit) override;
    bool credit_avail(uint8_t vc_id) const override;

    void tick();

    // Test introspection
    std::size_t pending_size(uint8_t vc_id) const noexcept { return pending_[vc_id].size(); }
    uint8_t round_robin_ptr() const noexcept { return round_robin_ptr_; }

  private:
    VcArbiter(router::NocRspOut& downstream, std::size_t num_vc, uint8_t write_rsp_vc,
              uint8_t read_rsp_vc, std::size_t pending_depth)
        : downstream_(downstream),
          num_vc_(num_vc),
          write_rsp_vc_(write_rsp_vc),
          read_rsp_vc_(read_rsp_vc),
          pending_depth_(pending_depth) {
        assert(num_vc_ >= 1 && num_vc_ <= NUM_VC_MAX);
        assert(write_rsp_vc_ < num_vc_);
        assert(read_rsp_vc_ < num_vc_);
    }

    std::optional<uint8_t> select_vc_for_axi_ch(uint8_t axi_ch, uint8_t id);

    router::NocRspOut& downstream_;
    std::size_t num_vc_;
    uint8_t write_rsp_vc_;
    uint8_t read_rsp_vc_;
    std::array<std::deque<Flit>, NUM_VC_MAX> pending_;
    std::size_t pending_depth_;
    uint8_t round_robin_ptr_ = 0;
    std::vector<uint8_t> write_rsp_vcs_;
    std::vector<uint8_t> read_rsp_vcs_;
    bool use_pools_ = false;
    uint8_t write_rr_start_ = 0;
    uint8_t read_rr_start_ = 0;
    // R-follows-first-beat: the first R beat of an rid round-robins a VC;
    // later beats of that rid reuse it; released on payload rlast.
    // The id is a burst-grouping key, not a VC selector.
    std::array<std::optional<uint8_t>, axi::AXI_ID_SPACE> r_burst_vc_{};
};

inline std::optional<uint8_t> VcArbiter::select_vc_for_axi_ch(uint8_t axi_ch, uint8_t id) {
    if (num_vc_ == 1) return uint8_t{0};

    // ReadWriteSplit, scalar (no pools configured).
    if (!use_pools_) {
        if (axi_ch == ni::AXI_CH_B) return write_rsp_vc_;
        if (axi_ch == ni::AXI_CH_R) return read_rsp_vc_;
        return std::nullopt;
    }

    // ReadWriteSplit pools: B round-robins write pool (no pin);
    // R follows r_burst_vc_ for burst coherence, round-robins on first beat.
    const std::vector<uint8_t>* cand = nullptr;
    uint8_t* rr = nullptr;
    if (axi_ch == ni::AXI_CH_B) {
        cand = &write_rsp_vcs_;
        rr = &write_rr_start_;
    } else if (axi_ch == ni::AXI_CH_R) {
        if (r_burst_vc_[id].has_value()) return r_burst_vc_[id];  // burst follow
        cand = &read_rsp_vcs_;
        rr = &read_rr_start_;
    } else {
        return std::nullopt;
    }
    const std::size_t n = cand->size();
    for (std::size_t k = 0; k < n; ++k) {  // round-robin from rr
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
    if (use_pools_ && num_vc_ > 1 && axi_ch == ni::AXI_CH_R)
        id = static_cast<uint8_t>(flit.get_payload_field("R", "rid"));
    auto vc_opt = select_vc_for_axi_ch(axi_ch, id);
    if (!vc_opt.has_value()) return false;
    uint8_t vc_id = *vc_opt;
    if (pending_[vc_id].size() >= pending_depth_) return false;

    // Stamp r_burst_vc_ after all accept conditions pass (R only).
    if (use_pools_ && num_vc_ > 1 && axi_ch == ni::AXI_CH_R) r_burst_vc_[id] = vc_id;

    Flit stamped = flit;
    stamped.set_header_field("vc_id", vc_id);
    pending_[vc_id].push_back(stamped);

    // Release on payload rlast so the next same-rid burst rebinds via round-robin.
    // Header `last` is always 1, so the payload field must be used for R.
    if (use_pools_ && num_vc_ > 1 && axi_ch == ni::AXI_CH_R) {
        if (flit.get_payload_field("R", "rlast") != 0) r_burst_vc_[id].reset();
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
