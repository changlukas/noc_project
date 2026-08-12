#pragma once
// NMU virtual channel arbiter. Decorator pattern over NocReqOut: receives
// packetized flits from nmu::Packetize, decides which VC each flit goes
// into, enqueues into a per-VC pending queue, and drains to the wrapped
// downstream via tick() using credit-gated round-robin.
//
// Candidate set is every VC in [0, num_vc): scanned round-robin from
// rr_start_; first VC with pending space AND downstream credit wins (else
// backpressure). Only the DAT face runs num_vc > 1; REQ/RSP are single-VC.
//
// Fixed VC id (same-destination bypass): an ordering_req=0 AW flit whose (dst_id, awid) matches
// the id's previous AW reuses that VC instead of round-robining --
// deterministic VC allocation that fixes a same-(dst,id) bypass streak to one
// VC so it cannot be reordered in-fabric. With no fixed VC yet (new id, or dst
// changed) it falls back to round-robin and records the new (dst, VC) for next
// time. ordering_req=1 flits are RoB-owned and order-free, so they always
// round-robin, never fixed. AR carries no streak state: the production wraps
// pin the REQ face it rides to num_vc == 1, and an AR reaching a multi-VC face
// (ctest fixtures do this) round-robins. Every flit of a fixed-VC stream also
// leaves with header fixed_vc=1 so downstream routers keep the NI's vc_id.
//
// W-follows-AW invariant: this arbiter MUST be downstream
// of a WormholeArbiter that serializes AW and all its W beats before
// admitting the next AW. Given that guarantee, a single
// std::optional<uint8_t> current_aw_vc_ is sufficient to track the in-flight
// burst's VC (push on AW, reset on W with payload.W.wlast=1).
//
// NUM_VC=1 degenerate behavior: routes everything to VC=0, observationally
// identical to a single-VC pipeline.
//
// References:
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

namespace ni::cmodel::nmu {

class VcAllocator : public router::NocReqOut {
  public:
    static constexpr std::size_t NUM_VC_MAX = 1u << ni::header::VC_ID_WIDTH;  // 8
    static constexpr std::size_t kDefaultPendingDepth = 4;

    VcAllocator(router::NocReqOut& downstream, std::size_t num_vc,
                std::size_t pending_depth = kDefaultPendingDepth)
        : downstream_(downstream), num_vc_(num_vc), pending_depth_(pending_depth) {
        assert(num_vc_ >= 1 && num_vc_ <= NUM_VC_MAX);
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
    std::optional<uint8_t> select_vc_for_axi_ch(uint8_t axi_ch, uint8_t dst_id,
                                                uint8_t ordering_req, uint8_t id);

    // Channel-kind classification is class-independent: narrow and data class
    // AW/AR/W route through the same VC-selection logic (steering both
    // classes onto the shared REQ/RSP link is S2's interim shape; S3a splits
    // this). Every axi_ch comparison below must accept both encodings.
    static bool is_aw(uint8_t axi_ch) {
        return axi_ch == ni::AXI_CH_NarrowAw || axi_ch == ni::AXI_CH_DataAw;
    }
    static bool is_ar(uint8_t axi_ch) {
        return axi_ch == ni::AXI_CH_NarrowAr || axi_ch == ni::AXI_CH_DataAr;
    }
    static bool is_w(uint8_t axi_ch) {
        return axi_ch == ni::AXI_CH_NarrowW || axi_ch == ni::AXI_CH_DataW;
    }

    router::NocReqOut& downstream_;
    std::size_t num_vc_;
    std::array<std::deque<Flit>, NUM_VC_MAX> pending_;
    std::size_t pending_depth_;
    uint8_t round_robin_ptr_ = 0;
    uint8_t rr_start_ = 0;  // round-robin scan start (selection)
    std::optional<uint8_t> current_aw_vc_;
    uint8_t current_aw_fixed_vc_ = 0;  // in-flight burst's fixed_vc, W beats copy it

    // Fixed VC id (same-destination bypass): last (dst_id, VC) a given AXI id took on an
    // ordering_req=0 AW. nullopt dst = id never seen.
    std::array<std::optional<uint8_t>, axi::AXI_ID_SPACE> last_aw_dst_{};
    std::array<uint8_t, axi::AXI_ID_SPACE> last_aw_vc_{};
};

inline std::optional<uint8_t> VcAllocator::select_vc_for_axi_ch(uint8_t axi_ch, uint8_t dst_id,
                                                                uint8_t ordering_req, uint8_t id) {
    // W invariant fires regardless of NUM_VC: this arbiter must be
    // downstream of a WormholeArbiter that serializes AW+W; W must always follow AW.
    if (is_w(axi_ch)) {
        if (!current_aw_vc_.has_value()) {
            assert(false &&
                   "nmu::VcAllocator::push_flit: W arrived with no current AW VC -- "
                   "must be downstream of a WormholeArbiter that serializes AW+W "
                   "(all W beats complete before the next AW). Standalone "
                   "VcAllocator use without upstream serialization is unsupported.");
            std::abort();
        }
        return *current_aw_vc_;
    }

    if (num_vc_ == 1) return uint8_t{0};

    if (!is_aw(axi_ch) && !is_ar(axi_ch)) return std::nullopt;

    // Fixed VC id (same-destination bypass): ordering_req=0 AW whose dst_id matches this id's
    // last AW dst_id reuses that VC. No fallback to round-robin on block --
    // rerouting a fixed-VC streak mid-flight is exactly the reorder the fixed
    // VC exists to prevent.
    if (ordering_req == 0 && is_aw(axi_ch) && last_aw_dst_[id].has_value() &&
        *last_aw_dst_[id] == dst_id) {
        uint8_t last_vc = last_aw_vc_[id];
        if (pending_[last_vc].size() < pending_depth_ && downstream_.credit_avail(last_vc)) {
            return last_vc;
        }
        return std::nullopt;
    }

    for (std::size_t k = 0; k < num_vc_; ++k) {  // round-robin from rr_start_, first available
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

    uint8_t dst_id = 0, ordering_req = 0, id = 0;
    if (is_aw(axi_ch) || is_ar(axi_ch)) {
        dst_id = static_cast<uint8_t>(flit.get_header_field("dst_id"));
        ordering_req = static_cast<uint8_t>(flit.get_header_field("ordering_req"));
        // "AW"/"AR" payload channel layout is shared by both classes (spec
        // §6: no Narrow*/Data* split for Aw/Ar).
        id = static_cast<uint8_t>(is_aw(axi_ch) ? flit.get_payload_field("AW", "awid")
                                                : flit.get_payload_field("AR", "arid"));
    }

    auto vc_opt = select_vc_for_axi_ch(axi_ch, dst_id, ordering_req, id);
    if (!vc_opt.has_value()) return false;
    uint8_t vc_id = *vc_opt;
    if (pending_[vc_id].size() >= pending_depth_) return false;

    // fixed_vc: the flit holds this vc_id end to end, routers must not
    // reallocate it (spec docs/noc-target-spec.md header table). An
    // ordering_req=0 AW streak is kept in order by same-VC delivery alone, so
    // EVERY packet of the streak carries the bit -- including the first, which
    // only records the (dst, VC) pair. ordering_req=1 is RoB-owned and
    // order-free, AR rides a single-VC face: both leave the bit clear. W copies
    // its owning AW's bit from current_aw_fixed_vc_ (read before the wlast
    // reset below), never re-derives it from its own header.
    uint8_t fixed_vc = 0;
    if (is_aw(axi_ch)) {
        fixed_vc = (ordering_req == 0) ? 1u : 0u;
    } else if (is_w(axi_ch)) {
        fixed_vc = current_aw_fixed_vc_;
    }

    // Update W-follows-AW optional only after pass conditions (atomicity)
    if (is_aw(axi_ch)) {
        if (current_aw_vc_.has_value()) {
            assert(false &&
                   "nmu::VcAllocator::push_flit: AW arrived while previous AW's W burst "
                   "still in progress -- must be downstream of a WormholeArbiter "
                   "that serializes AW+W (holds next AW until current W burst ends).");
            std::abort();  // belt-and-braces for NDEBUG
        }
        current_aw_vc_ = vc_id;
        current_aw_fixed_vc_ = fixed_vc;
    } else if (is_w(axi_ch)) {
        // wlast sits at bit 0 of both NARROW_W and DATA_W (same relative
        // position in both channel layouts), so reading it via either
        // channel name returns the same bit -- no class branch needed here.
        static_assert(ni::payload::narrow_w::WLAST_LSB == ni::payload::data_w::WLAST_LSB,
                      "narrow_w/data_w WLAST_LSB must match for the class-agnostic read below");
        if (flit.get_payload_field("NARROW_W", "wlast") != 0) {
            current_aw_vc_.reset();
        }
    }

    // Fixed VC id (same-destination bypass): record (dst_id, VC) for this id only after all
    // accept conditions pass (mirrors current_aw_vc_'s atomicity above).
    // ordering_req=1 flits are RoB-owned/order-free -- do not record a fixed VC.
    if (ordering_req == 0 && is_aw(axi_ch)) {
        last_aw_dst_[id] = dst_id;
        last_aw_vc_[id] = vc_id;
    }

    Flit stamped = flit;
    stamped.set_header_field("vc_id", vc_id);
    stamped.set_header_field("fixed_vc", fixed_vc);
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
                   "nmu::VcAllocator::tick: downstream returned credit_avail=true "
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
