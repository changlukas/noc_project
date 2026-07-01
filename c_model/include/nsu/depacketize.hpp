#pragma once
#include "axi/types.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "router/req_in.hpp"
#include "ni/pipeline_stage.hpp"
#include "nsu/meta_buffer.hpp"
#include "request_io.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>

namespace ni::cmodel::nsu {

// NSU-side request depacketizer. Stateful demux: tick() pulls flits from
// NocReqIn, decodes axi_ch, demuxes into AW/W/AR queues. AW/AR flits
// additionally allocate a MetaBuffer entry {src_id, rob_req, rob_idx} keyed by
// awid/arid for the response path. W flits have no MetaBuffer side effect —
// W carries no AXI ID; W ordering is handled by a downstream W-meta FIFO.
//
// Pending-flit stash semantics: if a pulled flit's target queue is full,
// the flit is held in `pending_` and re-attempted next tick. This blocks
// any other flits behind it (head-of-line blocking on single-FIFO ingress).
// Critical: MetaBuffer allocate for AW/AR happens AFTER the q_depth check
// passes, so a backpressure-induced retry of the same pending_ flit never
// double-allocates.
class Depacketize : public RequestDepacketizer {
  public:
    Depacketize(router::NocReqIn& req_in, MetaBuffer& meta, std::size_t aw_q_depth,
                std::size_t w_q_depth, std::size_t ar_q_depth)
        : req_in_(req_in),
          meta_(meta),
          aw_q_depth_(aw_q_depth),
          w_q_depth_(w_q_depth),
          ar_q_depth_(ar_q_depth) {}

    void tick();

    // RequestDepacketizer interface: takes from the S1 stage register.
    // Called by AxiMasterPort (S2) once per tick (<=1 beat/channel/tick).
    std::optional<axi::AwBeat> pop_aw() override;
    std::optional<axi::WBeat> pop_w() override;
    std::optional<axi::ArBeat> pop_ar() override;

    // stage_occupancy probe: returns 1 if the S1 register for axi_ch is
    // occupied, 0 otherwise. axi_ch uses ni::AXI_CH_* constants.
    std::size_t s1_occupancy(uint8_t axi_ch) const noexcept {
        switch (axi_ch) {
            case ni::AXI_CH_AW:
                return s1_aw_.occupancy();
            case ni::AXI_CH_W:
                return s1_w_.occupancy();
            case ni::AXI_CH_AR:
                return s1_ar_.occupancy();
            default:
                return 0;
        }
    }

  private:
    router::NocReqIn& req_in_;
    MetaBuffer& meta_;
    // Unused: old per-channel queues replaced by S1 PipelineStage registers.
    // Depths are kept as members to preserve backpressure checks if needed.
    std::size_t aw_q_depth_, w_q_depth_, ar_q_depth_;
    std::optional<Flit> pending_;

    // S1 stage registers: one per AXI channel (AW/W/AR). Depacketize::tick()
    // decodes <=1 flit/channel/tick into these registers. pop_aw/pop_w/pop_ar
    // (called by AxiMasterPort as the S2 stage) take from them.
    router::PipelineStage<axi::AwBeat> s1_aw_;
    router::PipelineStage<axi::WBeat> s1_w_;
    router::PipelineStage<axi::ArBeat> s1_ar_;

    static axi::AwBeat decode_aw(const Flit& f);
    static axi::WBeat decode_w(const Flit& f);
    static axi::ArBeat decode_ar(const Flit& f);
};

inline axi::AwBeat Depacketize::decode_aw(const Flit& f) {
    axi::AwBeat b{};
    b.id = static_cast<uint8_t>(f.get_payload_field("AW", "awid"));
    b.addr = f.get_payload_field("AW", "awaddr");
    b.len = static_cast<uint8_t>(f.get_payload_field("AW", "awlen"));
    b.size = static_cast<uint8_t>(f.get_payload_field("AW", "awsize"));
    b.burst = static_cast<axi::Burst>(f.get_payload_field("AW", "awburst"));
    b.cache = static_cast<uint8_t>(f.get_payload_field("AW", "awcache"));
    b.lock = static_cast<uint8_t>(f.get_payload_field("AW", "awlock"));
    b.prot = static_cast<uint8_t>(f.get_payload_field("AW", "awprot"));
    b.region = static_cast<uint8_t>(f.get_payload_field("AW", "awregion"));
    b.user = static_cast<uint8_t>(f.get_payload_field("AW", "awuser"));
    b.qos = static_cast<uint8_t>(f.get_payload_field("AW", "awqos"));
    return b;
}

inline axi::WBeat Depacketize::decode_w(const Flit& f) {
    axi::WBeat b{};
    b.last = f.get_payload_field("W", "wlast") != 0;
    b.user = static_cast<uint8_t>(f.get_payload_field("W", "wuser"));
    b.strb = static_cast<uint32_t>(f.get_payload_field("W", "wstrb"));
    f.get_payload_bytes("W", "wdata", b.data.data(), axi::NOC_DATA_WIDTH_BITS);
    return b;
}

inline axi::ArBeat Depacketize::decode_ar(const Flit& f) {
    axi::ArBeat b{};
    b.id = static_cast<uint8_t>(f.get_payload_field("AR", "arid"));
    b.addr = f.get_payload_field("AR", "araddr");
    b.len = static_cast<uint8_t>(f.get_payload_field("AR", "arlen"));
    b.size = static_cast<uint8_t>(f.get_payload_field("AR", "arsize"));
    b.burst = static_cast<axi::Burst>(f.get_payload_field("AR", "arburst"));
    b.cache = static_cast<uint8_t>(f.get_payload_field("AR", "arcache"));
    b.lock = static_cast<uint8_t>(f.get_payload_field("AR", "arlock"));
    b.prot = static_cast<uint8_t>(f.get_payload_field("AR", "arprot"));
    b.region = static_cast<uint8_t>(f.get_payload_field("AR", "arregion"));
    b.user = static_cast<uint8_t>(f.get_payload_field("AR", "aruser"));
    b.qos = static_cast<uint8_t>(f.get_payload_field("AR", "arqos"));
    return b;
}

// tick() is the S1 stage: decode <=1 flit per channel per tick into the S1
// stage registers. If a register is already occupied (not yet consumed by the
// S2 AxiMasterPort), backpressure the flit into pending_ (head-of-line
// blocking on single-FIFO ingress, same semantics as the original queue-based
// implementation). MetaBuffer allocate is atomic with decode into S1 (R4).
inline void Depacketize::tick() {
    while (true) {
        Flit f;
        if (pending_) {
            f = *pending_;
        } else {
            auto opt = req_in_.pop_flit();
            if (!opt) return;
            f = *opt;
        }
        uint64_t ch = f.get_header_field("axi_ch");
        switch (ch) {
            case ni::AXI_CH_AW:
                if (s1_aw_.full()) {
                    pending_ = f;
                    return;
                }
                {
                    auto aw = decode_aw(f);
                    meta_.allocate_write(aw.id,
                                         {
                                             static_cast<uint8_t>(f.get_header_field("src_id")),
                                             static_cast<uint8_t>(f.get_header_field("rob_req")),
                                             static_cast<uint8_t>(f.get_header_field("rob_idx")),
                                         });
                    s1_aw_.accept(aw);
                }
                break;
            case ni::AXI_CH_W:
                if (s1_w_.full()) {
                    pending_ = f;
                    return;
                }
                s1_w_.accept(decode_w(f));
                break;
            case ni::AXI_CH_AR:
                if (s1_ar_.full()) {
                    pending_ = f;
                    return;
                }
                {
                    auto ar = decode_ar(f);
                    meta_.allocate_read(ar.id,
                                        {
                                            static_cast<uint8_t>(f.get_header_field("src_id")),
                                            static_cast<uint8_t>(f.get_header_field("rob_req")),
                                            static_cast<uint8_t>(f.get_header_field("rob_idx")),
                                        });
                    s1_ar_.accept(ar);
                }
                break;
            default:
                assert(false &&
                       "nsu::Depacketize::tick: NocReqIn delivered flit with axi_ch outside "
                       "{AW, W, AR} — NSU request path only accepts request channels. Likely "
                       "cause: NMU packetizer stamped wrong axi_ch into a request flit, NoC "
                       "fabric misrouted a response flit into the request ingress, or codegen "
                       "drift changed ni::AXI_CH_* encoding without rebuilding both sides.");
                std::abort();
        }
        pending_.reset();
        // S1 registers accept only one flit per channel per tick.
        // After placing a flit in a register, stop advancing the ingress
        // stream for that channel (subsequent flits for any channel remain
        // for the next tick, preserving the <=1 beat/channel/tick bound).
        // Since all three registers are independent, we continue pulling
        // flits for other channels until all three are full or the ingress
        // is empty.
        //
        // The while(true) loop naturally handles this: after the switch we
        // loop back to pull the next flit. When a channel's register is
        // full, the next flit for that channel goes to pending_. Because
        // pending_ is a single-slot stash, only one channel can be stalled
        // at a time (head-of-line blocking on the single ingress stream).
    }
}

// pop_aw/pop_w/pop_ar: S2 consumer interface — take from the S1 register.
// Called <=1 time per channel per tick by AxiMasterPort::drain_*_from_depkt.
// Returns nullopt when the S1 register is empty (no beat ready this tick).
inline std::optional<axi::AwBeat> Depacketize::pop_aw() {
    if (!s1_aw_.full()) return std::nullopt;
    return s1_aw_.take();
}
inline std::optional<axi::WBeat> Depacketize::pop_w() {
    if (!s1_w_.full()) return std::nullopt;
    return s1_w_.take();
}
inline std::optional<axi::ArBeat> Depacketize::pop_ar() {
    if (!s1_ar_.full()) return std::nullopt;
    return s1_ar_.take();
}

}  // namespace ni::cmodel::nsu
