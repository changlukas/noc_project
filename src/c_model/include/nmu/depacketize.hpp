#pragma once
#include "axi/types.hpp"
#include "flit.hpp"
#include "router/null_adapters.hpp"
#include "router/rsp_in.hpp"
#include "response_io.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>

namespace ni::cmodel::nmu {

// NMU-side response depacketizer. Stateful demux: tick() pulls from two
// independent NocRspIn ingresses -- RSP (NarrowB/NarrowR/DataB) and DAT
// (DataR, S3a T4 ingress + T6 steering: NSU's Packetize steers Data-class R
// here) -- and routes B/R flits into the SAME
// per-channel deques (S3a stage design §5.2: "second ingress + per-network
// pending_"). Upstream port calls pop_b/pop_r to serve from those queues.
//
// Pending-flit stash semantics: if a pulled flit's target queue is full,
// the flit is held in that ingress's own `pending_` and re-attempted next
// tick. This blocks any other flits behind it on THAT ingress only
// (head-of-line blocking on a single-FIFO link) -- the two ingresses stall
// independently since each is a physically separate link.
class Depacketize : public ResponseDepacketizer {
  public:
    Depacketize(router::NocRspIn& rsp_in, std::size_t b_q_depth, std::size_t r_q_depth,
                router::NocRspIn& dat_rsp_in = router::null_rsp_in())
        : rsp_in_(rsp_in), dat_rsp_in_(dat_rsp_in), b_q_depth_(b_q_depth), r_q_depth_(r_q_depth) {}

    void tick();

    // ResponseDepacketizer interface — response methods are real
    std::optional<axi::BBeat> pop_b() override;
    std::optional<axi::RBeat> pop_r() override;
    std::optional<std::pair<axi::BBeat, ResponseMeta>> pop_b_with_meta() override;
    std::optional<std::pair<axi::RBeat, ResponseMeta>> pop_r_with_meta() override;

    // Introspection: deque occupancy for stage_occupancy(NmuRsp, 0, ch).
    std::size_t b_occupancy() const noexcept { return b_q_.size(); }
    std::size_t r_occupancy() const noexcept { return r_q_.size(); }

  private:
    struct BWithMeta {
        axi::BBeat beat;
        ResponseMeta meta;
    };
    struct RWithMeta {
        axi::RBeat beat;
        ResponseMeta meta;
    };

    router::NocRspIn& rsp_in_;
    router::NocRspIn& dat_rsp_in_;
    std::deque<BWithMeta> b_q_;
    std::deque<RWithMeta> r_q_;
    std::size_t b_q_depth_, r_q_depth_;
    std::optional<Flit> pending_rsp_;
    std::optional<Flit> pending_dat_;

    static axi::BBeat decode_b(const Flit& f);
    static axi::RBeat decode_r(const Flit& f);
    void drain_ingress_(router::NocRspIn& src, std::optional<Flit>& pending);
};

inline axi::BBeat Depacketize::decode_b(const Flit& f) {
    axi::BBeat b{};
    b.id = static_cast<uint8_t>(f.get_payload_field("B", "bid"));
    b.resp = static_cast<axi::Resp>(f.get_payload_field("B", "bresp"));
    b.user = static_cast<uint8_t>(f.get_payload_field("B", "buser"));
    return b;
}

inline axi::RBeat Depacketize::decode_r(const Flit& f) {
    // axi_ch picks which channel namespace the flit was packed into. Narrow's
    // rdata is the 8 B lane; this decode places it at byte offset 0 -- the
    // Rob layer (which holds the AR basis this flit has no address for) moves
    // it to the real lane. Data's rdata is the full width, no re-anchor needed.
    const bool is_data = f.get_header_field("axi_ch") == ni::AXI_CH_DataR;
    const char* ch = is_data ? "DATA_R" : "NARROW_R";
    axi::RBeat r{};
    r.id = static_cast<uint8_t>(f.get_payload_field(ch, "rid"));
    r.resp = static_cast<axi::Resp>(f.get_payload_field(ch, "rresp"));
    r.user = static_cast<uint8_t>(f.get_payload_field(ch, "ruser"));
    r.last = f.get_payload_field(ch, "rlast") != 0;
    const std::size_t bits = is_data ? ni::width::NOC_DATA_WIDTH : ni::width::NOC_NARROW_DATA_WIDTH;
    f.get_payload_bytes(ch, "rdata", r.data.data(), bits);
    return r;
}

inline void Depacketize::drain_ingress_(router::NocRspIn& src, std::optional<Flit>& pending) {
    while (true) {
        Flit f;
        if (pending) {
            f = *pending;
        } else {
            auto opt = src.pop_flit();
            if (!opt) return;
            f = *opt;
        }
        uint64_t ch = f.get_header_field("axi_ch");
        switch (ch) {
            case ni::AXI_CH_NarrowB:
            case ni::AXI_CH_DataB: {
                if (b_q_.size() >= b_q_depth_) {
                    pending = f;
                    return;
                }
                const auto cls =
                    (ch == ni::AXI_CH_DataB) ? axi::AxiClass::Data : axi::AxiClass::Narrow;
                ResponseMeta meta{static_cast<uint8_t>(f.get_header_field("ordering_tag")),
                                  static_cast<uint8_t>(f.get_header_field("ordering_req")), cls};
                b_q_.push_back({decode_b(f), meta});
                break;
            }
            case ni::AXI_CH_NarrowR:
            case ni::AXI_CH_DataR: {
                if (r_q_.size() >= r_q_depth_) {
                    pending = f;
                    return;
                }
                const auto cls =
                    (ch == ni::AXI_CH_DataR) ? axi::AxiClass::Data : axi::AxiClass::Narrow;
                ResponseMeta meta{static_cast<uint8_t>(f.get_header_field("ordering_tag")),
                                  static_cast<uint8_t>(f.get_header_field("ordering_req")), cls};
                r_q_.push_back({decode_r(f), meta});
                break;
            }
            default:
                assert(false &&
                       "nmu::Depacketize::drain_ingress_: NocRspIn delivered flit with axi_ch "
                       "outside {NarrowB, NarrowR, DataB, DataR} — NMU response path only accepts "
                       "response channels. Likely cause: NSU packetizer stamped wrong axi_ch "
                       "into a response flit, NoC fabric misrouted a request flit into the "
                       "response ingress, or codegen drift changed ni::AXI_CH_* encoding without "
                       "rebuilding both sides.");
                std::abort();
        }
        pending.reset();
    }
}

inline void Depacketize::tick() {
    drain_ingress_(rsp_in_, pending_rsp_);
    drain_ingress_(dat_rsp_in_, pending_dat_);
}

inline std::optional<axi::BBeat> Depacketize::pop_b() {
    if (b_q_.empty()) return std::nullopt;
    auto entry = b_q_.front();
    b_q_.pop_front();
    return entry.beat;
}

inline std::optional<axi::RBeat> Depacketize::pop_r() {
    if (r_q_.empty()) return std::nullopt;
    auto entry = r_q_.front();
    r_q_.pop_front();
    return entry.beat;
}

inline std::optional<std::pair<axi::BBeat, ResponseMeta>> Depacketize::pop_b_with_meta() {
    if (b_q_.empty()) return std::nullopt;
    auto entry = b_q_.front();
    b_q_.pop_front();
    return std::make_pair(entry.beat, entry.meta);
}

inline std::optional<std::pair<axi::RBeat, ResponseMeta>> Depacketize::pop_r_with_meta() {
    if (r_q_.empty()) return std::nullopt;
    auto entry = r_q_.front();
    r_q_.pop_front();
    return std::make_pair(entry.beat, entry.meta);
}

}  // namespace ni::cmodel::nmu
