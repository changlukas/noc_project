#pragma once
// NSU virtual channel arbiter. Mirror of nmu::VcAllocator but for response
// side (B + R flits leaving NSU toward NMU). No W-follows-AW logic
// because NSU produces single-flit B (`floo_axi_chimney.sv:608-616`)
// and multi-flit R uses ROB not wormhole (`floo_axi_chimney.sv:624-633`).
//
// Candidate set is every VC in [0, num_vc). Only the DAT face runs
// num_vc > 1, and DAT carries R only (B rides RSP, single-VC).
//   Fixed VC id (same-destination bypass, return path): ANY R (regardless of
//   ordering_req) maps to (dst_id ^ rid) % num_vc -- deterministic VC
//   allocation, a pure function with zero state. This fixes a same-(dst,id)
//   bypassed response stream to one VC (so it cannot be reordered in-fabric)
//   and gives R burst coherence for free: every beat of a burst shares
//   (dst_id, rid) and hashes identically. A mapped VC that is full/no-credit
//   refuses (`return false`) rather than spilling to another VC -- spilling a
//   fixed-VC stream would reorder it. B is order-free at the NMU slot path
//   (or single-VC on RSP) and round-robins. R also leaves with header
//   fixed_vc=1 so downstream routers keep the NI's vc_id; B leaves it clear.
// NUM_VC=1 degenerate behavior: routes everything to VC=0.
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "router/rsp_out.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>

namespace ni::cmodel::nsu {

class VcAllocator : public router::NocRspOut {
  public:
    static constexpr std::size_t NUM_VC_MAX = 1u << ni::header::VC_ID_WIDTH;  // 8
    static constexpr std::size_t kDefaultPendingDepth = 4;

    VcAllocator(router::NocRspOut& downstream, std::size_t num_vc,
                std::size_t pending_depth = kDefaultPendingDepth)
        : downstream_(downstream), num_vc_(num_vc), pending_depth_(pending_depth) {
        assert(num_vc_ >= 1 && num_vc_ <= NUM_VC_MAX);
    }

    // NocRspOut decorator interface
    bool push_flit(const Flit& flit) override;
    bool credit_avail(uint8_t vc_id) const override;

    void tick();

    // Test introspection
    std::size_t pending_size(uint8_t vc_id) const noexcept { return pending_[vc_id].size(); }
    uint8_t round_robin_ptr() const noexcept { return round_robin_ptr_; }

  private:
    std::optional<uint8_t> select_vc_for_axi_ch(uint8_t axi_ch, uint8_t dst_id, uint8_t id);

    // Channel-kind classification is class-independent (see nmu::VcAllocator's
    // is_aw/is_ar/is_w comment): both narrow and data class B/R route through
    // this same VC-selection logic.
    static bool is_b(uint8_t axi_ch) {
        return axi_ch == ni::AXI_CH_NarrowB || axi_ch == ni::AXI_CH_DataB;
    }
    static bool is_r(uint8_t axi_ch) {
        return axi_ch == ni::AXI_CH_NarrowR || axi_ch == ni::AXI_CH_DataR;
    }

    router::NocRspOut& downstream_;
    std::size_t num_vc_;
    std::array<std::deque<Flit>, NUM_VC_MAX> pending_;
    std::size_t pending_depth_;
    uint8_t round_robin_ptr_ = 0;
    uint8_t rr_start_ = 0;  // round-robin scan start (selection)
};

inline std::optional<uint8_t> VcAllocator::select_vc_for_axi_ch(uint8_t axi_ch, uint8_t dst_id,
                                                                uint8_t id) {
    if (num_vc_ == 1) return uint8_t{0};

    if (is_r(axi_ch)) {
        // Fixed VC id (same-destination bypass, return path): deterministic pure function of
        // (dst_id, rid), zero state. Full/no-credit -> refuse, never spill
        // (spilling a fixed-VC stream to another VC would reorder it).
        uint8_t vc = static_cast<uint8_t>((dst_id ^ id) % num_vc_);
        if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) return vc;
        return std::nullopt;
    }
    if (!is_b(axi_ch)) return std::nullopt;

    for (std::size_t k = 0; k < num_vc_; ++k) {  // B: round-robin from rr_start_
        uint8_t vc = static_cast<uint8_t>((rr_start_ + k) % num_vc_);
        if (pending_[vc].size() < pending_depth_ && downstream_.credit_avail(vc)) {
            rr_start_ =
                static_cast<uint8_t>((static_cast<std::size_t>(rr_start_) + k + 1) % num_vc_);
            return vc;
        }
    }
    return std::nullopt;
}

inline bool VcAllocator::push_flit(const Flit& flit) {
    uint8_t axi_ch = static_cast<uint8_t>(flit.get_header_field("axi_ch"));
    uint8_t dst_id = 0, id = 0;
    if (num_vc_ > 1 && (is_b(axi_ch) || is_r(axi_ch))) {
        dst_id = static_cast<uint8_t>(flit.get_header_field("dst_id"));
        // "rid" sits at the same bit offset in NARROW_R and DATA_R (rlast then
        // rid; only rdata's trailing width differs), so reading it via either
        // channel name returns the same bits -- no class branch needed here.
        static_assert(ni::payload::narrow_r::RID_LSB == ni::payload::data_r::RID_LSB,
                      "narrow_r/data_r RID_LSB must match for the class-agnostic read below");
        id = static_cast<uint8_t>(is_b(axi_ch) ? flit.get_payload_field("B", "bid")
                                               : flit.get_payload_field("NARROW_R", "rid"));
    }
    auto vc_opt = select_vc_for_axi_ch(axi_ch, dst_id, id);
    if (!vc_opt.has_value()) return false;
    uint8_t vc_id = *vc_opt;
    if (pending_[vc_id].size() >= pending_depth_) return false;

    Flit stamped = flit;
    stamped.set_header_field("vc_id", vc_id);
    // fixed_vc: R is an ordered same-destination stream held on its mapped VC
    // end to end, so routers must not reallocate it. B is order-free (and rides
    // the single-VC RSP face), leaving the bit clear.
    stamped.set_header_field("fixed_vc", is_r(axi_ch) ? 1u : 0u);
    pending_[vc_id].push_back(stamped);
    return true;
}

inline bool VcAllocator::credit_avail(uint8_t vc_id) const {
    return pending_[vc_id].size() < pending_depth_;
}

inline void VcAllocator::tick() {
    for (std::size_t k = 0; k < num_vc_; ++k) {
        uint8_t vc = static_cast<uint8_t>((round_robin_ptr_ + k) % num_vc_);
        if (!pending_[vc].empty() && downstream_.credit_avail(vc)) {
            bool ok = downstream_.push_flit(pending_[vc].front());
            assert(ok &&
                   "nsu::VcAllocator::tick: downstream returned credit_avail=true "
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
