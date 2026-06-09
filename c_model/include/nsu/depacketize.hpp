#pragma once
#include "axi/types.hpp"
#include "ni/depacketizer.hpp"
#include "flit.hpp"
#include "noc/noc_req_in.hpp"
#include "nsu/meta_buffer.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>

namespace ni::cmodel::nsu {

// NSU-side request depacketizer. Stateful demux: tick() pulls flits from
// NocReqIn, decodes axi_ch, demuxes into AW/W/AR queues. AW/AR flits
// additionally snapshot {src_id, rob_req, rob_idx} into MetaBuffer keyed by
// awid/arid for the response path. W flits have no MetaBuffer side effect —
// W carries no AXI ID; W ordering is handled by a downstream W-meta FIFO.
//
// Pending-flit stash semantics: if a pulled flit's target queue is full,
// the flit is held in `pending_` and re-attempted next tick. This blocks
// any other flits behind it (head-of-line blocking on single-FIFO ingress).
// Critical: MetaBuffer snapshot for AW/AR happens AFTER the q_depth check
// passes, so a backpressure-induced retry of the same pending_ flit never
// double-snapshots.
class Depacketize : public Depacketizer {
  public:
    Depacketize(noc::NocReqIn& req_in, MetaBuffer& meta, std::size_t aw_q_depth,
                std::size_t w_q_depth, std::size_t ar_q_depth)
        : req_in_(req_in),
          meta_(meta),
          aw_q_depth_(aw_q_depth),
          w_q_depth_(w_q_depth),
          ar_q_depth_(ar_q_depth) {}

    void tick();

    // Request methods real
    std::optional<axi::AwBeat> pop_aw() override;
    std::optional<axi::WBeat> pop_w() override;
    std::optional<axi::ArBeat> pop_ar() override;
    // Response methods assert false
    std::optional<axi::BBeat> pop_b() override { wrong_side_("nsu::Depacketize", "pop_b"); }
    std::optional<axi::RBeat> pop_r() override { wrong_side_("nsu::Depacketize", "pop_r"); }

  private:
    noc::NocReqIn& req_in_;
    MetaBuffer& meta_;
    std::deque<axi::AwBeat> aw_q_;
    std::deque<axi::WBeat> w_q_;
    std::deque<axi::ArBeat> ar_q_;
    std::size_t aw_q_depth_, w_q_depth_, ar_q_depth_;
    std::optional<Flit> pending_;

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
                if (aw_q_.size() >= aw_q_depth_) {
                    pending_ = f;
                    return;
                }
                {
                    auto aw = decode_aw(f);
                    aw_q_.push_back(aw);
                    meta_.snapshot_write(aw.id,
                                         {
                                             static_cast<uint8_t>(f.get_header_field("src_id")),
                                             static_cast<uint8_t>(f.get_header_field("rob_req")),
                                             static_cast<uint8_t>(f.get_header_field("rob_idx")),
                                         });
                }
                break;
            case ni::AXI_CH_W:
                if (w_q_.size() >= w_q_depth_) {
                    pending_ = f;
                    return;
                }
                w_q_.push_back(decode_w(f));
                break;
            case ni::AXI_CH_AR:
                if (ar_q_.size() >= ar_q_depth_) {
                    pending_ = f;
                    return;
                }
                {
                    auto ar = decode_ar(f);
                    ar_q_.push_back(ar);
                    meta_.snapshot_read(ar.id,
                                        {
                                            static_cast<uint8_t>(f.get_header_field("src_id")),
                                            static_cast<uint8_t>(f.get_header_field("rob_req")),
                                            static_cast<uint8_t>(f.get_header_field("rob_idx")),
                                        });
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
    }
}

inline std::optional<axi::AwBeat> Depacketize::pop_aw() {
    if (aw_q_.empty()) return std::nullopt;
    auto b = aw_q_.front();
    aw_q_.pop_front();
    return b;
}
inline std::optional<axi::WBeat> Depacketize::pop_w() {
    if (w_q_.empty()) return std::nullopt;
    auto b = w_q_.front();
    w_q_.pop_front();
    return b;
}
inline std::optional<axi::ArBeat> Depacketize::pop_ar() {
    if (ar_q_.empty()) return std::nullopt;
    auto b = ar_q_.front();
    ar_q_.pop_front();
    return b;
}

}  // namespace ni::cmodel::nsu
