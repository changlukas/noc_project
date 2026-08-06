#pragma once
#include "axi/types.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "router/null_adapters.hpp"
#include "router/req_in.hpp"
#include "ni/pipeline_stage.hpp"
#include "nsu/meta_buffer.hpp"
#include "request_io.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <stdexcept>

namespace ni::cmodel::nsu {

// NSU-side request depacketizer. Stateful demux: tick() pulls flits from two
// independent NocReqIn ingresses -- REQ and DAT (S3a T4 ingress + T6
// steering: NMU's Packetize steers Data-class AW/W here; DAT never carries
// Narrow* or DataAr -- see drain_ingress_'s defensive assert) -- reads
// axi_ch, and parks the flit in the SAME per-channel S1 stage register
// (S3a stage design §5.2: "second ingress + per-network pending_; s1_aw_/
// s1_w_/s1_ar_ stay shared" -- both classes already share s1_w_ today).
// tick() touches no MetaBuffer state and decodes nothing but W.
//
// pop_aw() / pop_ar() are the drain stage. Each decodes its flit, remaps the
// master's AXI id to the downstream id, allocates the MetaBuffer entry under
// that key, and hands the beat to AxiMasterPort. Each gates ONLY on its own
// pool: a full write pool stalls pop_aw and leaves pop_ar untouched. Allocating
// here rather than at ingress is what keeps a full pool from head-of-line
// blocking the other channels, mirroring the same independent-channel-draining
// pattern used by the NMU request path.
//
// Pending-flit stash semantics: if a pulled flit's S1 register is occupied, the
// flit is held in `pending_` and re-attempted next tick, blocking flits behind
// it. That is inherent to a serialized NoC link, not a modelling defect, and
// NocReqIn offers no peek to avoid it.
//
// W flits have no MetaBuffer side effect — W carries no AXI ID; W ordering is
// handled by a downstream W-meta FIFO.
class Depacketize : public RequestDepacketizer {
  public:
    Depacketize(router::NocReqIn& req_in, MetaBuffer& meta, std::size_t max_unique_ids,
                router::NocReqIn& dat_req_in = router::null_req_in())
        : req_in_(req_in), dat_req_in_(dat_req_in), meta_(meta), max_unique_ids_(max_unique_ids) {
        // Every path that configures an NSU funnels through here (YAML loader, co-sim
        // wrap defaults, direct NsuConfig test fixtures), so this is the config trust
        // boundary: validate with a throw, not an assert, so a misconfigured value fails
        // loud even in a release/NDEBUG build where asserts are compiled out.
        //
        // Only 1 and AXI_ID_SPACE are legal. This mirrors FlooNoC's floo_meta_buffer,
        // whose MaxUniqueIds has exactly two behaviors: collapse every id to one
        // downstream id (MaxUniqueIds==1, floo_meta_buffer.sv:87) or passthrough (else,
        // the unique-id count is set by OutIdWidth, floo_meta_buffer.sv:129). FlooNoC
        // provides no arbitrary-N remap (nothing in the chimney instantiates
        // axi_id_remap), so an intermediate value is intentionally unsupported, not a
        // modelling gap: with InIdWidth==OutIdWidth==8 it would silently degenerate to
        // the identity (full 256) remap.
        if (max_unique_ids != 1 && max_unique_ids != axi::AXI_ID_SPACE) {
            throw std::invalid_argument(
                "max_unique_ids must be 1 (collapse) or AXI_ID_SPACE (passthrough); "
                "FlooNoC provides no arbitrary-N unique-id remap");
        }
    }

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
            case ni::AXI_CH_NarrowAw:
            case ni::AXI_CH_DataAw:
                return s1_aw_.occupancy();
            case ni::AXI_CH_NarrowW:
            case ni::AXI_CH_DataW:
                return s1_w_.occupancy();
            case ni::AXI_CH_NarrowAr:
            case ni::AXI_CH_DataAr:
                return s1_ar_.occupancy();
            default:
                return 0;
        }
    }

  private:
    router::NocReqIn& req_in_;
    router::NocReqIn& dat_req_in_;
    MetaBuffer& meta_;
    std::size_t max_unique_ids_;
    std::optional<Flit> pending_req_;
    std::optional<Flit> pending_dat_;

    // AW / AR hold the raw flit until pop_aw / pop_ar admit it: the drain stage
    // needs the header's src_id / ordering_req / ordering_tag to allocate the MetaBuffer
    // entry, and decoding is pure. W carries no id and no metadata, so it stays
    // a decoded beat.
    router::PipelineStage<Flit> s1_aw_;
    router::PipelineStage<axi::WBeat> s1_w_;
    router::PipelineStage<Flit> s1_ar_;

    // Narrow-class W lane re-anchor (S2 design doc §2 site 2): the AW basis a
    // W burst's beats need is decoded eagerly here, at AW arrival, and staged
    // FIFO-order -- AXI4 W beats stream non-interleaved, one wormhole packet
    // (AW + its W beats) at a time, so the front entry always matches the W
    // beats currently arriving. Pushed only for narrow-class AW (mirrors
    // nmu::Rob's ar_lane_meta_): decode_w's data branch never pops, so an
    // unconditional push would leak one entry per data-class AW and, worse,
    // leave a stale front entry for the next narrow AW to misread.
    // Data class ignores this FIFO entirely (full-width payload, decoded
    // directly, no lane math).
    struct WAddrMeta {
        uint64_t local_addr;
        uint8_t len;
        uint8_t size;
        axi::Burst burst;
        uint16_t beat_counter = 0;
    };
    std::deque<WAddrMeta> w_addr_fifo_;

    static axi::AwBeat decode_aw(const Flit& f);
    axi::WBeat decode_w(const Flit& f);
    static axi::ArBeat decode_ar(const Flit& f);
    void drain_ingress_(router::NocReqIn& src, std::optional<Flit>& pending, bool is_dat_ingress);
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
    const bool is_data = f.get_header_field("axi_ch") == ni::AXI_CH_DataW;
    const char* ch = is_data ? "DATA_W" : "NARROW_W";
    axi::WBeat b{};
    b.last = f.get_payload_field(ch, "wlast") != 0;
    b.user = static_cast<uint8_t>(f.get_payload_field(ch, "wuser"));
    if (is_data) {
        b.strb = f.get_payload_field(ch, "wstrb");
        f.get_payload_bytes(ch, "wdata", b.data.data(), ni::width::NOC_DATA_WIDTH);
        return b;
    }
    // Narrow: the flit carries only the addressed 8 B lane. w_addr_fifo_'s
    // front entry is this beat's paired AW (see the struct comment).
    assert(!w_addr_fifo_.empty() &&
           "nsu::Depacketize::decode_w: narrow W flit with no staged AW address basis");
    WAddrMeta& am = w_addr_fifo_.front();
    const uint64_t addr = axi::beat_addr(am.local_addr, am.len, am.size, am.burst, am.beat_counter);
    const unsigned lane = axi::narrow_lane(addr);
    const uint64_t narrow_strb = f.get_payload_field(ch, "wstrb");
    b.strb = narrow_strb << (lane * axi::NARROW_DATA_BYTES);
    f.get_payload_bytes(ch, "wdata", b.data.data() + lane * axi::NARROW_DATA_BYTES,
                        ni::width::NOC_NARROW_DATA_WIDTH);
    ++am.beat_counter;
    if (b.last) w_addr_fifo_.pop_front();
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

// drain_ingress_ is the S1 stage for one physical ingress: park <=1 flit per
// channel per tick into the S1 stage registers (only W is decoded here;
// AW/AR are decoded at the drain). If a register is already occupied (not
// yet consumed by the S2 AxiMasterPort), backpressure the flit into this
// ingress's own `pending` stash (head-of-line blocking on single-FIFO
// ingress, same semantics as the original queue-based implementation).
// Touches no MetaBuffer state; allocation happens in pop_aw / pop_ar.
// Single-ingress HOL note: unlike the NMU request path, NSU depacketize has NO
// source-side pairing lock on ingress. It demuxes into independent S1 registers
// that drain into bounded AxiMasterPort queues, which drain to the slave.
// The per-ingress HOL is inherent to a single VC (AW/W/AR serialize on one
// channel) but cannot self-cycle: no ingress resource waits on a downstream
// that waits back on it. Given the slave eventually drains, `pending` always
// clears.
inline void Depacketize::drain_ingress_(router::NocReqIn& src, std::optional<Flit>& pending,
                                        bool is_dat_ingress) {
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
        // DAT carries DataAw/DataW only (spec :348; DataAr and every Narrow*
        // channel stay on REQ). A Narrow* AW/W arriving here would corrupt
        // w_addr_fifo_'s FIFO-order contiguity invariant (its front entry
        // must always be the REQ-ingress narrow AW currently being served);
        // fail loud instead of silently mis-pairing narrow lanes.
        assert((!is_dat_ingress || ch == ni::AXI_CH_DataAw || ch == ni::AXI_CH_DataW) &&
               "nsu::Depacketize::drain_ingress_: DAT ingress delivered a channel outside "
               "{DataAw, DataW} -- spec :348 keeps Narrow*/DataAr off DAT");
        switch (ch) {
            case ni::AXI_CH_NarrowAw:
            case ni::AXI_CH_DataAw:
                if (s1_aw_.full()) {
                    pending = f;
                    return;
                }
                s1_aw_.accept(f);
                // Eager decode (in addition to the raw stash above): the W
                // beats that follow need the AW's address basis before pop_aw
                // ever runs (W is decoded here, at arrival; pop_aw may drain
                // later, rate-limited to <=1/tick and gated on meta_.write_full()).
                // Narrow class only -- see w_addr_fifo_'s comment. Narrow class
                // is REQ-exclusive (S3a §1), so this never races the DAT ingress.
                if (ch == ni::AXI_CH_NarrowAw) {
                    const axi::AwBeat aw = decode_aw(f);
                    w_addr_fifo_.push_back(
                        {aw.addr, aw.len, aw.size, aw.burst, /*beat_counter=*/0});
                }
                break;
            case ni::AXI_CH_NarrowW:
            case ni::AXI_CH_DataW:
                if (s1_w_.full()) {
                    pending = f;
                    return;
                }
                s1_w_.accept(decode_w(f));
                break;
            case ni::AXI_CH_NarrowAr:
            case ni::AXI_CH_DataAr:
                if (s1_ar_.full()) {
                    pending = f;
                    return;
                }
                s1_ar_.accept(f);
                break;
            default:
                assert(false &&
                       "nsu::Depacketize::drain_ingress_: NocReqIn delivered flit with axi_ch "
                       "outside {NarrowAw, NarrowW, NarrowAr, DataAw, DataW, DataAr} — NSU request "
                       "path only accepts request channels. Likely cause: NMU packetizer stamped "
                       "wrong axi_ch into a request flit, NoC fabric misrouted a response flit "
                       "into the request ingress, or codegen drift changed ni::AXI_CH_* encoding "
                       "without rebuilding both sides.");
                std::abort();
        }
        pending.reset();
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
        // full, the next flit for that channel goes to `pending`. Because
        // `pending` is a single-slot stash, only one channel can be stalled
        // at a time per ingress (head-of-line blocking on that ingress's
        // single stream). The two ingresses drain independently -- REQ
        // blocked on a full register does not stall DAT, and vice versa.
    }
}

// tick(): drain both physical ingresses (REQ, DAT) into the shared S1
// registers. See the class comment for why sharing is correct.
inline void Depacketize::tick() {
    drain_ingress_(req_in_, pending_req_, /*is_dat_ingress=*/false);
    drain_ingress_(dat_req_in_, pending_dat_, /*is_dat_ingress=*/true);
}

// pop_aw/pop_w/pop_ar: S2 consumer interface — take from the S1 register.
// Called <=1 time per channel per tick by AxiMasterPort::drain_*_from_depkt.
// Returns nullopt when the S1 register is empty, or when this channel's
// MetaBuffer pool is full (backpressure: the flit stays in S1).
inline std::optional<axi::AwBeat> Depacketize::pop_aw() {
    if (!s1_aw_.full()) return std::nullopt;
    if (meta_.write_full()) return std::nullopt;
    const Flit f = s1_aw_.take();
    axi::AwBeat b = decode_aw(f);
    const uint8_t downstream_id = remap_downstream_id(b.id, max_unique_ids_);
    const AxiClass cls =
        (f.get_header_field("axi_ch") == ni::AXI_CH_DataAw) ? AxiClass::Data : AxiClass::Narrow;
    MetaEntry e{
        static_cast<uint8_t>(f.get_header_field("src_id")),
        b.id,
        static_cast<uint8_t>(f.get_header_field("ordering_req")),
        static_cast<uint8_t>(f.get_header_field("ordering_tag")),
        cls,
    };
    // Collective identity travels with the AW and comes back on its B (design
    // §3.1). Buffered here rather than re-derived: the B has no address to
    // translate from, and the RSP join matches on the exact echoed pair.
    e.collective_op = static_cast<uint8_t>(f.get_header_field("collective_op"));
    e.collective_mask = static_cast<uint8_t>(f.get_header_field("collective_mask"));
    meta_.allocate_write(downstream_id, e);
    b.id = downstream_id;
    return b;
}
inline std::optional<axi::WBeat> Depacketize::pop_w() {
    if (!s1_w_.full()) return std::nullopt;
    return s1_w_.take();
}
inline std::optional<axi::ArBeat> Depacketize::pop_ar() {
    if (!s1_ar_.full()) return std::nullopt;
    if (meta_.read_full()) return std::nullopt;
    const Flit f = s1_ar_.take();
    axi::ArBeat b = decode_ar(f);
    const uint8_t downstream_id = remap_downstream_id(b.id, max_unique_ids_);
    const AxiClass cls =
        (f.get_header_field("axi_ch") == ni::AXI_CH_DataAr) ? AxiClass::Data : AxiClass::Narrow;
    meta_.allocate_read(downstream_id, {
                                           static_cast<uint8_t>(f.get_header_field("src_id")),
                                           b.id,
                                           static_cast<uint8_t>(f.get_header_field("ordering_req")),
                                           static_cast<uint8_t>(f.get_header_field("ordering_tag")),
                                           cls,
                                           b.addr,
                                           b.len,
                                           b.size,
                                           b.burst,
                                       });
    b.id = downstream_id;
    return b;
}

}  // namespace ni::cmodel::nsu
