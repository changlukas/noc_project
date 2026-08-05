#pragma once
// NMU top-level assembly. Encapsulates the NI sub-modules into one
// class with a single tick() entrypoint, hiding the manual wiring that
// previously lived in test_request_response_loopback.cpp.
//
// Pipeline (req path, REQ network -- NarrowAw/NarrowW/NarrowAr/DataAr):
//   external AXI master ──> AxiSlavePort ──> Rob ──> Packetize{aw,w,ar}
//     ──> WormholeArbiter<NocReqOut>(3 in, pairing {{0,1}}) ──> VcAllocator
//     ──> external NocReqOut (ChannelModel or DPI bridge)
//
// DAT egress face (S3a T4 arbiter pair + T6 steering -- DataAw/DataW; per-
// network arbiter pair, {AW,W} lock independent of the REQ face's lock so a
// DAT AW never blocks a REQ W and vice versa. Packetize steers Data-class
// AW/W here (spec :348); ctest may still push directly into
// dat_wormhole_arbiter().input(0/1) to exercise the arbiter in isolation):
//   Packetize{aw,w} (Data class) ──> WormholeArbiter<NocReqOut>(2 in, pairing
//     {{0,1}}) ──> VcAllocator(dat_num_vc) ──> external NocReqOut (DPI bridge)
//
// Pipeline (rsp path, RSP network -- NarrowB/NarrowR/DataB):
//   external NocRspIn ──> Depacketize ──> Rob ──> AxiSlavePort
//     ──> back to external AXI master
//
// DAT ingress (S3a T4 -- DataR; Depacketize's second physical ingress,
// draining into the SAME b_q_/r_q_ queues as the RSP ingress -- see
// nmu::Depacketize's class comment. NSU's Packetize steers DataR here per T6):
//   external NocRspIn ──> Depacketize (second ingress) ──> Rob ──> AxiSlavePort
//
// Per-cycle tick order (exact sequence in Nmu::tick()):
//   req: wormhole_arbiter_.tick(); vc_allocator_.tick();
//        dat_wormhole_arbiter_.tick(); dat_vc_allocator_.tick();
//        req_s1_bridge_.tick(packetize_); axi_slave_port_.tick_req();
//   rsp: drain_rsp_b_output_(); drain_rsp_r_output_();
//        advance_rsp_b_shift_(); advance_rsp_r_shift_();
//        drain_rsp_s2_b_(); advance_rsp_s2_b_();  // B RoB is always on
//        read_rob_mode == Enabled: drain_rsp_s2_r_(); advance_rsp_s2_r_();
//        read_rob_mode == Disabled: drain_rsp_robless_r_();
//        depacketize_.tick();  // drains BOTH the RSP and DAT ingresses
//
// REQ/DAT are independent networks draining into disjoint sinks, so their
// relative tick order introduces no coupling (S3a stage design §5.4); DAT
// calls sit next to their REQ counterparts above for readability only.
//
// Lifetime: Nmu deletes move/copy (WormholeArbiter is non-movable).
// Member declaration order respects ctor ref dependencies — see private
// section comment for explanation.
//
// AXI binding: NOT via ctor (AxiMasterT<AxiSlavePort> template type
// collision in testbench). Use axi_slave_port() getter to obtain the
// AxiSlavePort& for the testbench's AxiMaster<AxiSlavePort> wiring.
#include "nmu/axi_slave_port.hpp"
#include "ni/ni_stage.hpp"
#include "nmu/depacketize.hpp"
#include "nmu/packetize.hpp"
#include "nmu/rob.hpp"
#include "nmu/vc_allocator.hpp"
#include "nmu/staged_beats.hpp"
#include "ni_params.h"
#include "router/req_out.hpp"
#include "router/rsp_in.hpp"
#include "ni/pipeline_stage.hpp"
#include "ni/wormhole_arbiter.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace ni::cmodel::nmu {

class NmuReqS1Bridge : public NmuPacketizeSink {
  public:
    bool push_aw_with_meta(const axi::AwBeat& b, AwHeaderMeta meta) override {
        if (s1_aw_.full()) return false;
        s1_aw_.accept(
            {b, meta.dst_id, meta.local_addr, meta.ordering_req, meta.ordering_tag, meta.cls});
        return true;
    }
    bool push_w(const axi::WBeat& b) override {
        if (s1_w_.full()) return false;
        s1_w_.accept({b});
        return true;
    }
    bool push_ar_with_meta(const axi::ArBeat& b, AwHeaderMeta meta) override {
        if (s1_ar_.full()) return false;
        s1_ar_.accept(
            {b, meta.dst_id, meta.local_addr, meta.ordering_req, meta.ordering_tag, meta.cls});
        return true;
    }

    // Drain each AXI sub-channel to Packetize INDEPENDENTLY. A full AW wormhole
    // input must not block W (the in-flight write's body, needed to release the
    // wormhole AW->W lock) or AR. Cross-channel HOL here self-deadlocks the
    // request path under load.
    // AW-before-W ordering is preserved downstream by Packetize's w_meta_fifo_,
    // not by gating W on AW admission.
    void tick(Packetize& packetize) {
        if (s1_aw_.full()) {
            const auto& e = s1_aw_.peek();
            if (packetize.push_aw_with_meta(
                    e.beat, {e.dst_id, e.local_addr, e.ordering_req, e.ordering_tag, e.cls})) {
                s1_aw_.take();
            }
        }
        if (s1_w_.full()) {
            const auto& e = s1_w_.peek();
            if (packetize.push_w(e.beat)) {
                s1_w_.take();
            }
        }
        if (s1_ar_.full()) {
            const auto& e = s1_ar_.peek();
            if (packetize.push_ar_with_meta(
                    e.beat, {e.dst_id, e.local_addr, e.ordering_req, e.ordering_tag, e.cls})) {
                s1_ar_.take();
            }
        }
    }

    // axi_ch is a selector token for "which channel kind" (AW/W/AR), not the
    // literal encoding of a produced flit -- accept both classes' encodings.
    std::size_t occupancy(uint8_t axi_ch) const noexcept {
        if (axi_ch == ni::AXI_CH_NarrowAw || axi_ch == ni::AXI_CH_DataAw) return s1_aw_.occupancy();
        if (axi_ch == ni::AXI_CH_NarrowW || axi_ch == ni::AXI_CH_DataW) return s1_w_.occupancy();
        if (axi_ch == ni::AXI_CH_NarrowAr || axi_ch == ni::AXI_CH_DataAr) return s1_ar_.occupancy();
        return 0;
    }

  private:
    router::PipelineStage<AdmittedAw> s1_aw_;
    router::PipelineStage<AdmittedW> s1_w_;
    router::PipelineStage<AdmittedAr> s1_ar_;
};

struct NmuRspBEntry {
    axi::BBeat beat;
    uint8_t ordering_tag = 0;
    uint8_t axi_id = 0;
    bool ordering_req = false;  // owns a RoB slot; false => bypassed
};

struct NmuRspREntry {
    axi::RBeat beat;
    uint8_t ordering_tag = 0;
    uint8_t axi_id = 0;
    bool ordering_req = false;
};

struct NmuConfig {
    uint8_t src_id = 0;
    addr_trans::SamTable sam{};
    RobMode read_rob_mode = RobMode::Disabled;
    // RoB pool depths, per direction. Enabled mode only.
    std::size_t b_rob_depth = ni::NMU_ROB_B_DEPTH;
    std::size_t r_rob_depth = ni::NMU_ROB_R_DEPTH;
    // Per-AXI-ID order-list depth (FlooNoC MaxRoTxnsPerId). Enabled mode only.
    std::size_t max_txns_per_id = ni::NMU_MAX_TXNS_PER_ID;
    // Shared outstanding-transaction pool depth, per direction (FlooNoC MaxTxns).
    // AW and AR pools are independent; each is shared across all AXI ids. Both modes.
    std::size_t outstanding_depth = ni::NMU_OUTSTANDING_DEPTH;
    nmu::PortParams port_params{};
    std::size_t num_vc = 1;
    // DAT face VC count (S3a T4; AW/W only -- no AR rides DAT). REQ/RSP got
    // their own NOC_{REQ,RSP}_NUM_VC=1 params. Round-robins across [0, dat_num_vc).
    std::size_t dat_num_vc = ni::NOC_DAT_NUM_VC;
    std::size_t wormhole_per_input_depth = ni::NMU_ARBITER_FIFO_DEPTH;
    std::size_t vc_allocator_pending_depth = ni::NMU_ARBITER_FIFO_DEPTH;
    std::size_t ni_rsp_extra_depth = 0;  // extra shift stages on the response path
};

class Nmu {
  public:
    // downstream_dat_req / downstream_dat_rsp: the DAT face (S3a T4). Every
    // assembly site wires these explicitly -- no default -- since Nmu is the
    // top-level product-facing class; see router/null_adapters.hpp for the
    // sentinel callers that don't yet exercise DAT traffic pass.
    Nmu(NmuConfig cfg, router::NocReqOut& downstream_req, router::NocRspIn& downstream_rsp,
        router::NocReqOut& downstream_dat_req, router::NocRspIn& downstream_dat_rsp);

    Nmu(const Nmu&) = delete;
    Nmu(Nmu&&) = delete;
    Nmu& operator=(const Nmu&) = delete;
    Nmu& operator=(Nmu&&) = delete;

    // AXI facade for testbench wiring (AxiMaster<AxiSlavePort> binds here).
    AxiSlavePort& axi_slave_port() noexcept { return axi_slave_port_; }

    // Per-cycle tick — orchestrates sub-modules in upstream-first order.
    void tick();

    // Test introspection (optional getters; add only as test code needs)
    const Rob& rob() const noexcept { return rob_; }
    const VcAllocator& vc_allocator() const noexcept { return vc_allocator_; }

    // DAT egress face (S3a T4 + T6 steering). Non-const: Packetize feeds this
    // (Data-class AW/W, wired via dat_wormhole_arbiter_.input(0/1) in the
    // ctor init list below); ctest may still push flits directly via
    // dat_wormhole_arbiter().input(0/1).push_flit(...) to exercise the
    // arbiter in isolation.
    router::WormholeArbiter<router::NocReqOut>& dat_wormhole_arbiter() noexcept {
        return dat_wormhole_arbiter_;
    }
    const VcAllocator& dat_vc_allocator() const noexcept { return dat_vc_allocator_; }
    std::size_t stage_occupancy(NiPath path, std::size_t stage, uint8_t axi_ch) const {
        if (path == NiPath::NmuReq) {
            // NmuReq: 3 stages
            //   S0 = NmuReqS1Bridge (AxiSlavePort→Rob→S1Bridge register)
            //   S1 = WormholeArbiter per-input pending (Packetize output)
            //   S2 = VcAllocator pending (toward NoC)
            if (stage == 0) return req_s1_bridge_.occupancy(axi_ch);
            if (stage == 1) {
                // WormholeArbiter inputs: 0=AW, 1=W, 2=AR (either class)
                if (axi_ch == ni::AXI_CH_NarrowAw || axi_ch == ni::AXI_CH_DataAw)
                    return wormhole_arbiter_.pending_size(0);
                if (axi_ch == ni::AXI_CH_NarrowW || axi_ch == ni::AXI_CH_DataW)
                    return wormhole_arbiter_.pending_size(1);
                if (axi_ch == ni::AXI_CH_NarrowAr || axi_ch == ni::AXI_CH_DataAr)
                    return wormhole_arbiter_.pending_size(2);
            }
            if (stage == 2) {
                // VcAllocator: single VC in default config; sum over all VCs per channel
                // (in VC=1 mode this is just vc_allocator_.pending_size(0))
                std::size_t total = 0;
                for (std::size_t v = 0; v < VcAllocator::NUM_VC_MAX; ++v)
                    total += vc_allocator_.pending_size(static_cast<uint8_t>(v));
                return total;
            }
        }
        if (path == NiPath::NmuRsp) {
            // NmuRsp ROB Enabled: 3 stages
            //   S0 = Depacketize deque (b_q_/r_q_)
            //   S1 = s2_rsp_b_/s2_rsp_r_ PipelineStage (Rob re-order stage)
            //   S2 = AxiSlavePort b_q/r_q (final output)
            // NmuRsp ROB Disabled: 2 stages
            //   S0 = Depacketize deque
            //   S1 = AxiSlavePort b_q/r_q
            const bool is_b = (axi_ch == ni::AXI_CH_NarrowB || axi_ch == ni::AXI_CH_DataB);
            const bool is_r = (axi_ch == ni::AXI_CH_NarrowR || axi_ch == ni::AXI_CH_DataR);
            bool rob_enabled = is_b ? true : (cfg_.read_rob_mode == RobMode::Enabled);
            if (stage == 0) {
                if (is_b) return depacketize_.b_occupancy();
                if (is_r) return depacketize_.r_occupancy();
            }
            if (rob_enabled) {
                if (stage == 1) {
                    if (is_b) return s2_rsp_b_.occupancy();
                    if (is_r) return s2_rsp_r_.occupancy();
                }
                if (stage == 2) {
                    if (is_b) return axi_slave_port_.b_q_size();
                    if (is_r) return axi_slave_port_.r_q_size();
                }
            } else {
                if (stage == 1) {
                    if (is_b) return axi_slave_port_.b_q_size();
                    if (is_r) return axi_slave_port_.r_q_size();
                }
            }
        }
        return 0;
    }

  private:
    bool push_rsp_b_to_axi_(const NmuRspBEntry& entry);
    bool push_rsp_r_to_axi_(const NmuRspREntry& entry);
    bool accept_rsp_b_entry_(NmuRspBEntry entry);
    bool accept_rsp_r_entry_(NmuRspREntry entry);
    void drain_rsp_b_output_();
    void drain_rsp_r_output_();
    void advance_rsp_b_shift_();
    void advance_rsp_r_shift_();
    void drain_rsp_s2_b_();
    void drain_rsp_s2_r_();
    void advance_rsp_s2_b_();
    void advance_rsp_s2_r_();
    void drain_rsp_robless_r_();

    // Declaration order respects ctor ref dependencies:
    //   1. cfg_ + external downstream refs (no deps).
    //   2. vc_allocator_ wraps downstream_req_.
    //   3. wormhole_arbiter_ wraps vc_allocator_ as its Downstream.
    //   4. dat_vc_allocator_ wraps downstream_dat_req_ (independent DAT egress
    //      face, S3a T4).
    //   5. dat_wormhole_arbiter_ wraps dat_vc_allocator_ (own {AW,W} lock, per §5.2).
    //   6. depacketize_ wraps downstream_rsp_ + downstream_dat_rsp_ (req path independent).
    //   7. packetize_ takes wormhole_arbiter_.input(0/1/2) (Narrow AW/W/AR + Data
    //      AR, REQ) and dat_wormhole_arbiter_.input(0/1) (Data AW/W, DAT; T6 steering).
    //   8. req_s1_bridge_ stages ROB-admitted requests before Packetize.
    //   9. rob_ takes req_s1_bridge_ + depacketize_.
    //   10. axi_slave_port_ takes rob_ (as Packetizer + Depacketizer via multi-inherit).
    NmuConfig cfg_;
    router::NocReqOut& downstream_req_;
    router::NocRspIn& downstream_rsp_;
    router::NocReqOut& downstream_dat_req_;
    router::NocRspIn& downstream_dat_rsp_;
    VcAllocator vc_allocator_;
    router::WormholeArbiter<router::NocReqOut> wormhole_arbiter_;
    VcAllocator dat_vc_allocator_;
    router::WormholeArbiter<router::NocReqOut> dat_wormhole_arbiter_;
    Depacketize depacketize_;
    Packetize packetize_;
    NmuReqS1Bridge req_s1_bridge_;
    Rob rob_;
    AxiSlavePort axi_slave_port_;
    router::PipelineStage<NmuRspBEntry> s2_rsp_b_;
    router::PipelineStage<NmuRspREntry> s2_rsp_r_;
    std::vector<router::PipelineStage<NmuRspBEntry>> rsp_extra_b_shift_;
    std::vector<router::PipelineStage<NmuRspREntry>> rsp_extra_r_shift_;
};

inline Nmu::Nmu(NmuConfig cfg, router::NocReqOut& downstream_req, router::NocRspIn& downstream_rsp,
                router::NocReqOut& downstream_dat_req, router::NocRspIn& downstream_dat_rsp)
    : cfg_(std::move(cfg)),
      downstream_req_(downstream_req),
      downstream_rsp_(downstream_rsp),
      downstream_dat_req_(downstream_dat_req),
      downstream_dat_rsp_(downstream_dat_rsp),
      vc_allocator_(downstream_req_, cfg_.num_vc, cfg_.vc_allocator_pending_depth),
      wormhole_arbiter_(vc_allocator_, /*num_inputs=*/3,
                        std::vector<router::ChannelPairing>{{0, 1}}, cfg_.wormhole_per_input_depth),
      dat_vc_allocator_(downstream_dat_req_, cfg_.dat_num_vc, cfg_.vc_allocator_pending_depth),
      dat_wormhole_arbiter_(dat_vc_allocator_, /*num_inputs=*/2,
                            std::vector<router::ChannelPairing>{{0, 1}},
                            cfg_.wormhole_per_input_depth),
      depacketize_(downstream_rsp_, cfg_.port_params.depkt_b_q_depth,
                   cfg_.port_params.depkt_r_q_depth, downstream_dat_rsp_),
      packetize_(wormhole_arbiter_.input(0), wormhole_arbiter_.input(1), wormhole_arbiter_.input(2),
                 dat_wormhole_arbiter_.input(0), dat_wormhole_arbiter_.input(1), cfg_.src_id,
                 cfg_.sam),
      req_s1_bridge_(),
      rob_(req_s1_bridge_, depacketize_, cfg_.read_rob_mode, cfg_.sam, cfg_.b_rob_depth,
           cfg_.r_rob_depth, cfg_.max_txns_per_id, cfg_.outstanding_depth),
      axi_slave_port_(rob_, rob_, cfg_.port_params),
      s2_rsp_b_(),
      s2_rsp_r_(),
      rsp_extra_b_shift_(cfg_.ni_rsp_extra_depth),
      rsp_extra_r_shift_(cfg_.ni_rsp_extra_depth) {}

inline void Nmu::tick() {
    wormhole_arbiter_.tick();
    vc_allocator_.tick();
    dat_wormhole_arbiter_.tick();
    dat_vc_allocator_.tick();
    req_s1_bridge_.tick(packetize_);
    axi_slave_port_.tick_req();

    drain_rsp_b_output_();
    drain_rsp_r_output_();
    advance_rsp_b_shift_();
    advance_rsp_r_shift_();
    // B RoB is always on; unlike R there is no RobLess drain path here.
    drain_rsp_s2_b_();
    advance_rsp_s2_b_();
    if (cfg_.read_rob_mode == RobMode::Enabled) {
        drain_rsp_s2_r_();
        advance_rsp_s2_r_();
    } else {
        drain_rsp_robless_r_();
    }
    depacketize_.tick();
}

// The AXI-side acceptance point, and so the single retire point for the RoB slot and
// the outstanding-pool entry alike (floo_meta_buffer.sv:205-206,210). Retiring any
// earlier -- e.g. at rob_.pop_*_staged, upstream of s2_rsp_*_, the extra shift stages
// and the slave-port output queue -- would admit new requests while completed
// responses are still inside the NI, by as many transactions as those stages hold.
inline bool Nmu::push_rsp_b_to_axi_(const NmuRspBEntry& entry) {
    if (!axi_slave_port_.push_b_staged(entry.beat)) return false;
    rob_.retire_b(entry.ordering_req, entry.ordering_tag, entry.axi_id);
    return true;
}

inline bool Nmu::push_rsp_r_to_axi_(const NmuRspREntry& entry) {
    if (!axi_slave_port_.push_r_staged(entry.beat)) return false;
    rob_.retire_r(entry.ordering_req, entry.ordering_tag, entry.axi_id, entry.beat.last);
    return true;
}

inline bool Nmu::accept_rsp_b_entry_(NmuRspBEntry entry) {
    if (rsp_extra_b_shift_.empty()) return push_rsp_b_to_axi_(entry);
    if (rsp_extra_b_shift_.front().full()) return false;
    rsp_extra_b_shift_.front().accept(std::move(entry));
    return true;
}

inline bool Nmu::accept_rsp_r_entry_(NmuRspREntry entry) {
    if (rsp_extra_r_shift_.empty()) return push_rsp_r_to_axi_(entry);
    if (rsp_extra_r_shift_.front().full()) return false;
    rsp_extra_r_shift_.front().accept(std::move(entry));
    return true;
}

inline void Nmu::drain_rsp_b_output_() {
    if (rsp_extra_b_shift_.empty() || !rsp_extra_b_shift_.back().full()) return;
    const auto& entry = rsp_extra_b_shift_.back().peek();
    if (push_rsp_b_to_axi_(entry)) rsp_extra_b_shift_.back().take();
}

inline void Nmu::drain_rsp_r_output_() {
    if (rsp_extra_r_shift_.empty() || !rsp_extra_r_shift_.back().full()) return;
    const auto& entry = rsp_extra_r_shift_.back().peek();
    if (push_rsp_r_to_axi_(entry)) rsp_extra_r_shift_.back().take();
}

inline void Nmu::advance_rsp_b_shift_() {
    if (rsp_extra_b_shift_.size() < 2) return;
    for (std::size_t i = rsp_extra_b_shift_.size() - 1; i > 0; --i) {
        if (!rsp_extra_b_shift_[i].full() && rsp_extra_b_shift_[i - 1].full()) {
            rsp_extra_b_shift_[i].accept(rsp_extra_b_shift_[i - 1].take());
        }
    }
}

inline void Nmu::advance_rsp_r_shift_() {
    if (rsp_extra_r_shift_.size() < 2) return;
    for (std::size_t i = rsp_extra_r_shift_.size() - 1; i > 0; --i) {
        if (!rsp_extra_r_shift_[i].full() && rsp_extra_r_shift_[i - 1].full()) {
            rsp_extra_r_shift_[i].accept(rsp_extra_r_shift_[i - 1].take());
        }
    }
}

inline void Nmu::drain_rsp_s2_b_() {
    if (!s2_rsp_b_.full()) return;
    const auto& entry = s2_rsp_b_.peek();
    if (accept_rsp_b_entry_(entry)) s2_rsp_b_.take();
}

inline void Nmu::drain_rsp_s2_r_() {
    if (!s2_rsp_r_.full()) return;
    const auto& entry = s2_rsp_r_.peek();
    if (accept_rsp_r_entry_(entry)) s2_rsp_r_.take();
}

inline void Nmu::advance_rsp_s2_b_() {
    if (s2_rsp_b_.full()) return;
    auto b = rob_.pop_b_staged();
    if (!b) return;
    s2_rsp_b_.accept({b->beat, b->ordering_tag, b->axi_id, b->ordering_req});
}

inline void Nmu::advance_rsp_s2_r_() {
    if (s2_rsp_r_.full()) return;
    auto r = rob_.pop_r_staged();
    if (!r) return;
    s2_rsp_r_.accept({r->beat, r->ordering_tag, r->axi_id, r->ordering_req});
}

inline void Nmu::drain_rsp_robless_r_() {
    if (rsp_extra_r_shift_.empty() &&
        axi_slave_port_.r_q_size() >= axi_slave_port_.params().r_queue_depth) {
        return;
    }
    if (!rsp_extra_r_shift_.empty() && rsp_extra_r_shift_.front().full()) return;
    // Non-retiring pop: retirement happens downstream at push_rsp_r_to_axi_, same as
    // the Enabled path. rob_.pop_r() would retire here and double-count.
    auto r = rob_.pop_r_robless();
    if (!r) return;
    // The beat has left the depacketizer, so a refusal here would lose it and leak the
    // pool entry. The guards above make that impossible; assert rather than discard.
    const bool accepted = accept_rsp_r_entry_({*r, 0, r->id, false});
    assert(accepted && "nmu::Nmu: RoBless R accept failed after its capacity guards passed");
    (void)accepted;
}

}  // namespace ni::cmodel::nmu
